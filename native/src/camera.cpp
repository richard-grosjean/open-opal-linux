#include "camera.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "depthai/depthai.hpp"
#include "usbstate.h"
#include "v4l2out.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::pair<uint32_t, uint32_t> kOutputSize{1920, 1080};
constexpr std::pair<uint32_t, uint32_t> kPreviewSize{640, 360};
constexpr std::pair<uint32_t, uint32_t> kSensorSize{3840, 2160};
constexpr float kFps = 30.f;
constexpr float kPreviewFps = 30.f;

constexpr double kOneShotFocusSeconds = 1.5;
constexpr double kWbLoopInterval = 0.25;
// the loop sleeps on the frame queue; this bounds how long a UI change waits without frames
constexpr auto kFrameWait = std::chrono::milliseconds(50);
// the ISP barely responds to manual white balance below ~2000 K
constexpr double kWbLoopMinK = 2000.0;
constexpr double kWbLoopMaxK = 10000.0;
constexpr double kWbRecoverK = 5000.0;  // where the loop drifts when it cannot measure the frame
constexpr double kSeedHoldSeconds = 0.5;      // keep last session's values this long before handing back to AE/AWB
constexpr double kLastAutoSaveInterval = 5.0;

// The camera must sit in Opal's firmware this long before we connect. Connecting while it
// is still coming back from a previous session fails, and rapid switching can leave Opal's
// firmware unstable.
constexpr double kCameraSettleSeconds = 2.0;
constexpr int kRetrySeconds[] = {2, 5, 10};

constexpr double kFaceInterval = 0.1;  // decode at most 10 times a second
constexpr double kFaceRegionInterval = 0.25;
constexpr double kFaceLostSeconds = 2.0;
constexpr float kFaceExposureMargin = 1.3f;  // meter a little beyond the face box
const char* kFaceModelFile = "yunet-s-240x320.rvc2.tar.xz";
// The Myriad X firmware, extracted from depthai-core's resources at install time. Loading it
// from a file instead of the copy embedded in the library saves ~75 MB of resident memory.
const char* kFirmwareFile = "depthai-device-firmware.cmd";

double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::vector<std::filesystem::path> dataDirs() {
    std::vector<std::filesystem::path> dirs;
    for (const auto& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        dirs.push_back(std::filesystem::path(d.toStdString()) / "open-opal-linux");
    return dirs;
}

std::optional<std::filesystem::path> findDataFile(const std::string& name) {
    for (const auto& d : dataDirs())
        if (std::filesystem::is_regular_file(d / name)) return d / name;
    return std::nullopt;
}

// The tuning blob for a setting, or nullopt for DepthAI's built-in tuning.
std::optional<std::filesystem::path> tuningPath(const QString& name) {
    if (name == "low_light") return findDataFile("tuning_color_low_light.bin");
    return std::nullopt;
}

// Where the chip's auto exposure and white balance settled last time, so the next session can
// start there instead of from the firmware defaults (which take a second or two to converge).
struct LastAuto {
    QString tuning;
    int exposureUs = 0, iso = 0, wbKelvin = 0;
    bool operator==(const LastAuto&) const = default;

    static QString path() { return QFileInfo(Settings::path()).path() + "/last-auto.json"; }

    static std::optional<LastAuto> load() {
        QFile f(path());
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        auto doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) return std::nullopt;
        auto o = doc.object();
        LastAuto l{o.value("tuning").toString(), o.value("exposure_us").toInt(), o.value("iso").toInt(), o.value("wb_kelvin").toInt()};
        if (l.exposureUs <= 0 || l.iso <= 0 || l.wbKelvin <= 0) return std::nullopt;
        return l;
    }

    void save() const {
        QDir().mkpath(QFileInfo(path()).path());
        QFile f(path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(QJsonObject{{"tuning", tuning}, {"exposure_us", exposureUs}, {"iso", iso}, {"wb_kelvin", wbKelvin}}).toJson());
    }
};

// Point depthai-core at the installed firmware file unless the user already chose one.
void configureFirmwarePath() {
    static bool done = false;
    if (done) return;
    done = true;
    if (qEnvironmentVariableIsSet("DEPTHAI_DEVICE_BINARY")) return;
    if (auto p = findDataFile(kFirmwareFile)) qputenv("DEPTHAI_DEVICE_BINARY", QByteArray::fromStdString(p->string()));
}

// YuNet archive: installed data dir first, then whatever the DepthAI model zoo cached.
std::optional<std::filesystem::path> faceModelPath() {
    if (auto p = findDataFile(kFaceModelFile)) return p;
    std::filesystem::path cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation).toStdString();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(cache / "depthai" / "models", ec))
        if (std::filesystem::is_regular_file(e.path() / kFaceModelFile)) return e.path() / kFaceModelFile;
    return std::nullopt;
}

std::shared_ptr<dai::CameraControl> ctrl() { return std::make_shared<dai::CameraControl>(); }

bool want(const QSet<QString>& keys, std::initializer_list<const char*> names) {
    if (keys.isEmpty()) return true;
    for (auto n : names)
        if (keys.contains(n)) return true;
    return false;
}

dai::CameraControl::AntiBandingMode antiBanding(const QString& s) {
    using M = dai::CameraControl::AntiBandingMode;
    if (s == "OFF") return M::OFF;
    if (s == "AUTO") return M::AUTO;
    if (s == "MAINS_60_HZ") return M::MAINS_60_HZ;
    return M::MAINS_50_HZ;
}

// Translate settings into camera controls. With keys, only the affected groups.
std::vector<std::shared_ptr<dai::CameraControl>> buildControls(const Settings& s, const QSet<QString>& keys = {}) {
    std::vector<std::shared_ptr<dai::CameraControl>> out;
    if (want(keys, {"auto_focus", "lens_position"})) {
        auto c = ctrl();
        if (s.autoFocus) {
            c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::CONTINUOUS_VIDEO);
        } else {
            c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::OFF);
            c->setManualFocus(std::clamp(s.lensPosition, 0, 255));
        }
        out.push_back(c);
    }
    if (want(keys, {"wb_mode", "wb_kelvin"})) {
        if (s.wbMode == wb::kAuto) {
            auto c = ctrl();
            c->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
            out.push_back(c);
        } else if (s.wbMode == wb::kManual) {
            auto c = ctrl();
            c->setManualWhiteBalance(std::clamp(s.wbKelvin, 1000, 12000));
            out.push_back(c);
        }
        // auto_biased is driven by the feedback loop in the session
    }
    if (want(keys, {"auto_exposure", "exposure_compensation", "exposure_us", "iso"})) {
        auto c = ctrl();
        if (s.autoExposure) {
            c->setAutoExposureEnable();
            c->setAutoExposureCompensation(std::clamp(s.exposureCompensation, -9, 9));
        } else {
            c->setManualExposure(std::clamp(s.exposureUs, 1, 33000), std::clamp(s.iso, 100, 3200));
        }
        out.push_back(c);
    }
    if (want(keys, {"anti_banding"})) {
        auto c = ctrl();
        c->setAntiBandingMode(antiBanding(s.antiBanding));
        out.push_back(c);
    }
    // No brightness control: with any non-zero value the RVC2 firmware makes the picture pulse
    // (https://discuss.luxonis.com/d/5384); exposure compensation covers the same need.
    if (want(keys, {"contrast", "saturation", "sharpness", "luma_denoise", "chroma_denoise"})) {
        auto c = ctrl();
        c->setContrast(s.contrast);
        c->setSaturation(s.saturation);
        c->setSharpness(s.sharpness);
        c->setLumaDenoise(s.lumaDenoise);
        c->setChromaDenoise(s.chromaDenoise);
        out.push_back(c);
    }
    return out;
}

// log(mean R / mean B) over mid-tone pixels of an interleaved BGR image; 0 means neutral.
std::optional<double> redBlueBalance(const uint8_t* bgr, int w, int h, int stride) {
    double sumR = 0, sumB = 0;
    long n = 0;
    for (int y = 0; y < h; y += 6) {
        const uint8_t* row = bgr + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x += 6) {
            int b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
            float luma = (b + g + r) / 3.f;
            if (luma > 25 && luma < 225 && std::max({b, g, r}) < 250) {
                sumR += r;
                sumB += b;
                ++n;
            }
        }
    }
    if (n < 200) return std::nullopt;
    double r = sumR / n, b = sumB / n;
    if (b < 1 || r < 1) return std::nullopt;
    return std::log(r / b);
}

struct Region { uint16_t x, y, w, h; };

// Normalised box, scaled around its centre, as sensor-pixel (x, y, w, h).
Region region(const faces::Face& f, float scale) {
    float cx = f.x + f.w / 2, cy = f.y + f.h / 2;
    float w = std::min(1.f, f.w * scale), h = std::min(1.f, f.h * scale);
    float x0 = std::min(std::max(0.f, cx - w / 2), 1.f - w);
    float y0 = std::min(std::max(0.f, cy - h / 2), 1.f - h);
    auto [sw, sh] = kSensorSize;
    return {uint16_t(x0 * sw), uint16_t(y0 * sh), uint16_t(std::max(1.f, w * sw)), uint16_t(std::max(1.f, h * sh))};
}

// Smooths face detections and turns them into AE/AF region controls.
class FaceMeter {
public:
    std::optional<faces::Face> box;

    void observe(const std::optional<faces::Face>& face, double t) {
        if (!face) {
            if (box && t - lastSeen_ > kFaceLostSeconds) box.reset();
            return;
        }
        lastSeen_ = t;
        if (!box) {
            box = *face;
        } else {
            const float a = 0.35f;
            box->x += a * (face->x - box->x);
            box->y += a * (face->y - box->y);
            box->w += a * (face->w - box->w);
            box->h += a * (face->h - box->h);
        }
    }

    std::vector<std::shared_ptr<dai::CameraControl>> controls(const Settings& s, double t) {
        bool wanted = s.faceMetering && (s.autoExposure || s.autoFocus);
        if (!wanted || !box) {
            if (regionActive_) {  // hand metering back to the whole frame
                regionActive_ = false;
                sentBox_.reset();
                auto [sw, sh] = kSensorSize;
                auto c = ctrl();
                c->setAutoExposureRegion(0, 0, sw, sh);
                auto c2 = ctrl();
                c2->setAutoFocusRegion(0, 0, sw, sh);
                return {c, c2};
            }
            return {};
        }
        if (t - lastSent_ < kFaceRegionInterval) return {};
        if (sentBox_) {
            float d = std::max({std::abs(box->x - sentBox_->x), std::abs(box->y - sentBox_->y), std::abs(box->w - sentBox_->w),
                                std::abs(box->h - sentBox_->h)});
            if (d < 0.02f) return {};
        }
        lastSent_ = t;
        sentBox_ = box;
        regionActive_ = true;
        std::vector<std::shared_ptr<dai::CameraControl>> out;
        if (s.autoExposure) {
            auto r = region(*box, kFaceExposureMargin);
            auto c = ctrl();
            c->setAutoExposureRegion(r.x, r.y, r.w, r.h);
            out.push_back(c);
        }
        if (s.autoFocus) {
            auto r = region(*box, 1.f);
            auto c = ctrl();
            c->setAutoFocusRegion(r.x, r.y, r.w, r.h);
            out.push_back(c);
        }
        return out;
    }

private:
    double lastSeen_ = 0, lastSent_ = 0;
    std::optional<faces::Face> sentBox_;
    bool regionActive_ = false;
};

// Rows and row stride of a tensor whose last dimension is `cols`.
std::pair<size_t, size_t> rowsAndStride(const xt::xarray<float>& t, size_t cols) {
    if (t.dimension() == 0) return {0, cols};
    size_t last = t.shape().back();
    if (last == 0) return {0, cols};
    return {t.size() / last, last};
}

std::optional<faces::Face> decodeFace(dai::NNData& nn) {
    auto loc = nn.getTensor<float>("loc");
    auto conf = nn.getTensor<float>("conf");
    auto iou = nn.getTensor<float>("iou");
    auto [rows, locStride] = rowsAndStride(loc, 14);
    auto [rowsC, confStride] = rowsAndStride(conf, 2);
    auto [rowsI, iouStride] = rowsAndStride(iou, 1);
    if (rows != rowsC || rows != rowsI || locStride < 4 || confStride < 2 || iouStride < 1) return std::nullopt;
    return faces::bestFace(loc.data(), locStride, conf.data(), confStride, iou.data(), iouStride, rows);
}

}  // namespace

namespace {

struct Output {
    std::unique_ptr<V4L2LoopbackOutput> dev;
    QString label;
};

Output openOutput(const Settings& s, const std::optional<QString>& preferred) {
    if (!s.outputEnabled) return {nullptr, "output off"};
    std::optional<std::string> device = preferred ? std::optional(preferred->toStdString()) : findLoopbackDevice();
    if (!device) return {nullptr, "no v4l2loopback device"};
    try {
        return {std::make_unique<V4L2LoopbackOutput>(*device, kOutputSize.first, kOutputSize.second), QString::fromStdString(*device)};
    } catch (const std::system_error& e) {
        return {nullptr, QString("%1: %2").arg(QString::fromStdString(*device), QString::fromUtf8(e.code().message().c_str()))};
    }
}

}  // namespace

struct CameraWorker::Impl {
    // Held for the life of the worker, not per session: Chrome only re-reads the camera list on
    // udev events, and the one event it gets is the Opal's own UVC node vanishing while we connect.
    // Being attached before that happens is what makes us show up in the list.
    Output output;
    std::optional<dai::NNArchive> faceArchive;
    std::optional<QString> faceModelError;

    // YuNet from the installed data dir or the zoo cache; loaded once.
    dai::NNArchive* loadFaceModel() {
        if (!faceArchive && !faceModelError) {
            auto path = faceModelPath();
            if (!path) {
                faceModelError = "face model not installed (run install.sh)";
            } else {
                try {
                    faceArchive.emplace(*path);
                } catch (const std::exception& e) {
                    faceModelError = QString::fromUtf8(e.what()).section('\n', 0, 0);
                }
            }
        }
        return faceArchive ? &*faceArchive : nullptr;
    }
};

CameraWorker::CameraWorker(const Settings& settings, std::optional<QString> outputDevice, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()), settings_(settings), outputDevice_(std::move(outputDevice)) {
    qRegisterMetaType<Stats>();
}

CameraWorker::~CameraWorker() { stop(); }

void CameraWorker::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void CameraWorker::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void CameraWorker::update(const Settings& s, const QSet<QString>& keys) {
    std::lock_guard lock(mutex_);
    pending_.emplace_back(s, keys);
}

void CameraWorker::focusOnce() { oneShotFocus_ = true; }
void CameraWorker::setPreviewEnabled(bool enabled) { previewEnabled_ = enabled; }
void CameraWorker::restart() { restart_ = true; }

// Show black rather than a frozen last frame while the camera is away.
void CameraWorker::blankOutput() {
    if (!impl_->output.dev) return;
    try {
        impl_->output.dev->writeBlank();
    } catch (const std::exception&) {
        impl_->output.dev.reset();
    }
}

void CameraWorker::sleepFor(double seconds) {
    double end = now() + seconds;
    while (running_ && now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void CameraWorker::run() {
    {
        std::lock_guard lock(mutex_);
        impl_->output = openOutput(settings_, outputDevice_);
    }
    if (impl_->output.dev) impl_->output.dev->writeBlank();
    int failures = 0;
    while (running_) {
        if (!waitForCamera()) return;
        try {
            session();
            failures = 0;
        } catch (const std::exception& e) {  // device unplugged, busy, firmware hiccup, ...
            int delay = kRetrySeconds[std::min<size_t>(failures, std::size(kRetrySeconds) - 1)];
            ++failures;
            emit status(QString("Camera error: %1. Retrying in %2 s…").arg(QString::fromUtf8(e.what()).section('\n', 0, 0)).arg(delay));
            sleepFor(delay);
        }
    }
}

// Block until the camera has settled in Opal's firmware. False if stopped.
bool CameraWorker::waitForCamera() {
    std::optional<double> stableSince;
    std::optional<OpalState> shown;
    while (running_) {
        auto state = opalState();
        if (state == OpalState::Camera) {
            if (!stableSince) stableSince = now();
            if (now() - *stableSince >= kCameraSettleSeconds) return true;
        } else {
            stableSince.reset();
            if (shown != state) {
                shown = state;
                switch (state) {
                    case OpalState::Absent: emit status("Camera is restarting or unplugged… waiting"); break;
                    case OpalState::DepthAI: emit status("Camera is still being released… waiting"); break;
                    default: emit status("Camera is rebooting… waiting"); break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

namespace {

// Nudge the manual WB so the frame's red/blue balance approaches the target.
// Telling the ISP the light is warmer (lower K) makes it add more blue, so a frame that is
// too red needs a lower K.
double wbStep(const dai::ImgFrame& img, double kelvin, int warmth, dai::InputQueue& controlQ) {
    auto data = img.getData();
    int w = img.getWidth(), h = img.getHeight();
    if (data.size() < static_cast<size_t>(w) * h * 3) return kelvin;
    auto balance = redBlueBalance(data.data(), w, h, w * 3);
    // No usable mid-tones: the frame is dark, or so far off that one channel clips everywhere
    // (a stale manual K does this). Drift back towards daylight instead of staying stuck.
    double error = balance ? *balance - warmth * 0.05 : std::log(kelvin / kWbRecoverK);
    if (std::abs(error) < 0.015) return kelvin;
    bool atFloor = kelvin <= kWbLoopMinK && error > 0;
    bool atCeiling = kelvin >= kWbLoopMaxK && error < 0;
    if (atFloor || atCeiling) return kelvin;
    // log(R/B) moves ~2.6x as fast as log(K), so a gain of 0.2 settles without ringing
    double step = std::clamp(0.2 * error, -0.05, 0.05);
    kelvin = std::clamp(kelvin * std::exp(-step), kWbLoopMinK, kWbLoopMaxK);
    auto c = ctrl();
    c->setManualWhiteBalance(static_cast<int>(kelvin));
    controlQ.send(c);
    return kelvin;
}

// Wrap the BGR888i preview frame in a QImage without copying; the frame stays alive
// until the QImage is destroyed.
QImage wrapPreview(std::shared_ptr<dai::ImgFrame> frame) {
    int w = frame->getWidth(), h = frame->getHeight();
    auto data = frame->getData();
    if (data.size() < static_cast<size_t>(w) * h * 3) return {};
    auto* keep = new std::shared_ptr<dai::ImgFrame>(std::move(frame));
    return QImage(data.data(), w, h, w * 3, QImage::Format_BGR888,
                  [](void* p) { delete static_cast<std::shared_ptr<dai::ImgFrame>*>(p); }, keep);
}

}  // namespace

void CameraWorker::session() {
    // Connecting reboots the camera out of Opal's firmware and uploads DepthAI's over USB
    emit status("Restarting camera… (takes about 7 s)");
    restart_ = false;
    Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }

    configureFirmwarePath();
    dai::Pipeline pipeline;
    emit status("Starting stream…");
    auto blob = tuningPath(s.tuning);
    if (blob) pipeline.setCameraTuningBlobPath(*blob);

    auto cam = pipeline.create<dai::node::Camera>()->build(dai::CameraBoardSocket::CAM_A, kSensorSize, kFps);

    // Start from where AE/AWB settled last time (same tuning only: the numbers differ between
    // tunings), then hand control back to the chip once frames are flowing.
    auto last = LastAuto::load();
    if (last && last->tuning != s.tuning) last.reset();
    bool seedAe = last && s.autoExposure;
    bool seedWb = last && s.wbMode != wb::kManual;
    if (seedAe) cam->initialControl.setManualExposure(std::clamp(last->exposureUs, 1, 33000), std::clamp(last->iso, 100, 3200));
    if (seedWb) cam->initialControl.setManualWhiteBalance(std::clamp(last->wbKelvin, 1000, 12000));
    auto mainQ = cam->requestOutput(kOutputSize, dai::ImgFrame::Type::NV12, dai::ImgResizeMode::CROP, kFps)->createOutputQueue(2, false);
    auto previewQ =
        cam->requestOutput(kPreviewSize, dai::ImgFrame::Type::BGR888i, dai::ImgResizeMode::CROP, kPreviewFps)->createOutputQueue(2, false);
    auto controlQ = cam->inputControl.createInputQueue();

    std::shared_ptr<dai::MessageQueue> faceQ;
    if (auto* archive = impl_->loadFaceModel()) {
        auto nn = pipeline.create<dai::node::NeuralNetwork>();
        nn->setNNArchive(*archive);
        nn->input.setBlocking(false);
        nn->input.setMaxSize(1);
        // same fps as the other outputs: mixed rates can stall the camera pipeline
        cam->requestOutput({faces::kInputW, faces::kInputH}, dai::ImgFrame::Type::BGR888p, dai::ImgResizeMode::STRETCH, kFps)->link(nn->input);
        faceQ = nn->out.createOutputQueue(1, false);
    }
    FaceMeter meter;
    double lastFace = 0;
    pipeline.start();

    {
        QSet<QString> keys{"auto_focus", "anti_banding", "contrast"};
        if (!seedAe) keys.insert("auto_exposure");
        if (!seedWb) keys.insert("wb_mode");
        for (auto& c : buildControls(s, keys)) controlQ->send(c);
    }

    Output& output = impl_->output;
    if (s.outputEnabled && !output.dev) {  // not opened yet, or lost on an earlier write error
        output = openOutput(s, outputDevice_);
        if (output.dev) output.dev->writeBlank();
    }
    QStringList notes;
    if (s.tuning == "low_light" && !blob) notes << "tuning file missing, using default; run install.sh";
    if (!faceQ) notes << QString("face metering unavailable: %1").arg(impl_->faceModelError.value_or("unknown"));
    emit status(QString("Streaming") + (notes.isEmpty() ? QString() : QString(" (%1)").arg(notes.join("; "))));

    double wbKelvin = s.wbKelvin;
    bool wbSeeded = s.wbMode != wb::kAutoBiased;  // biased: start from the chip's AWB once it reports one
    if (seedWb && s.wbMode == wb::kAutoBiased) {  // ... unless we have last time's value: the loop just carries on
        wbKelvin = std::clamp<double>(last->wbKelvin, kWbLoopMinK, kWbLoopMaxK);
        wbSeeded = true;
    }
    double seedHandoffAt = 0;  // when to switch the seeded values back to auto; 0 = nothing pending
    LastAuto cache = last.value_or(LastAuto{});
    cache.tuning = s.tuning;
    double lastCacheSave = now();
    auto rememberAuto = [&](const Stats& st) {
        LastAuto next = cache;
        if (s.autoExposure && st.exposureUs > 0 && st.iso > 0) next.exposureUs = st.exposureUs, next.iso = st.iso;
        if (s.wbMode != wb::kManual && st.wbKelvin > 0) next.wbKelvin = st.wbKelvin;
        if (next != cache) (cache = next).save();
    };
    double lastWbStep = 0;
    double focusDeadline = 0;
    int frames = 0;
    double fpsT0 = now();
    Stats st;
    st.output = output.label;

    try {
        while (running_ && pipeline.isRunning() && !restart_) {
            // apply settings changes from the UI
            std::vector<std::pair<Settings, QSet<QString>>> changes;
            {
                std::lock_guard lock(mutex_);
                changes.swap(pending_);
            }
            for (auto& [next, keys] : changes) {
                bool enteringBiased = next.wbMode == wb::kAutoBiased && s.wbMode != wb::kAutoBiased;
                bool outputToggled = next.outputEnabled != s.outputEnabled;
                if (keys.contains("tuning") && next.tuning != s.tuning) restart_ = true;
                s = next;
                {
                    std::lock_guard lock(mutex_);
                    settings_ = s;
                }
                for (auto& c : buildControls(s, keys)) controlQ->send(c);
                if (enteringBiased) wbSeeded = false;
                if (outputToggled) {
                    output = openOutput(s, outputDevice_);
                    if (output.dev) output.dev->writeBlank();
                    st.output = output.label;
                }
            }

            if (oneShotFocus_.exchange(false)) {
                auto c = ctrl();
                c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);
                c->setAutoFocusTrigger();
                controlQ->send(c);
                focusDeadline = now() + kOneShotFocusSeconds;
            }

            bool timedOut = false;
            auto frame = mainQ->get<dai::ImgFrame>(kFrameWait, timedOut);  // wakes once per frame instead of polling
            if (frame) {
                if (output.dev) {
                    try {
                        auto data = frame->getData();
                        output.dev->write(data.data(), data.size());
                    } catch (const std::exception& e) {
                        output.dev.reset();
                        st.output = QString("output error: %1").arg(e.what());
                    }
                }
                ++frames;
                st.lensPosition = frame->getLensPosition();
                st.iso = frame->getSensitivity();
                st.exposureUs = static_cast<int>(frame->getExposureTime().count());
                st.wbKelvin = frame->getColorTemperature();
                if ((seedAe || seedWb) && !seedHandoffAt) seedHandoffAt = now() + kSeedHoldSeconds;
            }

            if (seedHandoffAt && now() >= seedHandoffAt) {
                seedHandoffAt = 0;
                if (seedAe) for (auto& c : buildControls(s, {"auto_exposure"})) controlQ->send(c);
                if (seedWb && s.wbMode == wb::kAuto) for (auto& c : buildControls(s, {"wb_mode"})) controlQ->send(c);
                seedAe = seedWb = false;
            }

            if (auto pv = previewQ->tryGet<dai::ImgFrame>()) {
                double t = now();
                if (s.wbMode == wb::kAutoBiased && t - lastWbStep >= kWbLoopInterval) {
                    // start near the chip's own estimate rather than whatever K was saved last time
                    if (!wbSeeded && st.wbKelvin) {
                        wbKelvin = std::clamp<double>(st.wbKelvin, kWbLoopMinK, kWbLoopMaxK);
                        wbSeeded = true;
                    }
                    if (wbSeeded) {
                        lastWbStep = t;
                        wbKelvin = wbStep(*pv, wbKelvin, s.wbWarmth, *controlQ);
                    }
                }
                if (previewEnabled_) {
                    auto img = wrapPreview(std::move(pv));
                    if (!img.isNull()) emit preview(img);
                }
            }

            double t = now();
            if (faceQ && t - lastFace >= kFaceInterval) {
                if (auto result = faceQ->tryGet<dai::NNData>()) {
                    lastFace = t;
                    if (s.faceMetering)
                        meter.observe(decodeFace(*result), t);
                    else
                        meter.box.reset();
                }
                st.face = meter.box;
            }
            for (auto& c : meter.controls(s, t)) controlQ->send(c);

            if (focusDeadline && t >= focusDeadline) {
                focusDeadline = 0;
                s.autoFocus = false;
                s.lensPosition = st.lensPosition;
                {
                    std::lock_guard lock(mutex_);
                    settings_ = s;
                }
                for (auto& c : buildControls(s, {"lens_position"})) controlQ->send(c);
                emit lensLocked(st.lensPosition);
            }

            if (t - fpsT0 >= 1.0) {
                st.fps = frames / (t - fpsT0);
                frames = 0;
                fpsT0 = t;
                emit stats(st);
            }
            if (t - lastCacheSave >= kLastAutoSaveInterval) {
                lastCacheSave = t;
                rememberAuto(st);
            }
        }
        rememberAuto(st);
    } catch (...) {
        blankOutput();
        try { pipeline.stop(); pipeline.wait(); } catch (...) {}
        throw;
    }
    blankOutput();
    pipeline.stop();
    pipeline.wait();
}
