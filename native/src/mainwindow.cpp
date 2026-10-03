#include "mainwindow.h"

#include <QCloseEvent>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

const std::vector<std::pair<QString, QString>> kWbModes = {
    {wb::kAuto, "Auto (camera)"},
    {wb::kAutoBiased, "Auto + warmth bias"},
    {wb::kManual, "Manual"},
};
const std::vector<std::pair<QString, QString>> kTunings = {
    {"low_light", "Low light (Luxonis)"},
    {"default", "DepthAI default"},
};
const std::vector<std::pair<QString, QString>> kAntiBanding = {
    {"MAINS_50_HZ", "50 Hz"},
    {"MAINS_60_HZ", "60 Hz"},
    {"AUTO", "Auto"},
    {"OFF", "Off"},
};

QString signed_(int v) { return v ? QString::asprintf("%+d", v) : QString("0"); }

void fill(QComboBox* box, const std::vector<std::pair<QString, QString>>& items) {
    for (const auto& [key, label] : items) box->addItem(label, key);
}

void selectData(QComboBox* box, const QString& key, bool quiet) {
    int i = std::max(0, box->findData(key));
    if (quiet) box->blockSignals(true);
    box->setCurrentIndex(i);
    if (quiet) box->blockSignals(false);
}

}  // namespace

// --- SliderRow --------------------------------------------------------------------------

SliderRow::SliderRow(int lo, int hi, int step, Format fmt, QWidget* parent)
    : QWidget(parent), fmt_(fmt ? std::move(fmt) : [](int v) { return QString::number(v); }) {
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(lo, hi);
    slider_->setSingleStep(step);
    slider_->setPageStep(step * 10);
    label_ = new QLabel;
    label_->setMinimumWidth(64);
    label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(slider_, 1);
    layout->addWidget(label_);
    connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        label_->setText(fmt_(v));
        emit changed(v);
    });
}

void SliderRow::setValue(int v, bool quiet) {
    if (quiet) slider_->blockSignals(true);
    slider_->setValue(v);
    label_->setText(fmt_(slider_->value()));
    if (quiet) slider_->blockSignals(false);
}

// --- MainWindow -------------------------------------------------------------------------

MainWindow::MainWindow(std::optional<QString> outputDevice, QWidget* parent) : QMainWindow(parent), settings_(Settings::load()) {
    setWindowTitle("Open Opal");

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(800);
    connect(&saveTimer_, &QTimer::timeout, this, &MainWindow::save);

    // Start connecting first: the camera takes seconds to boot, the window milliseconds.
    // Signals are queued until the event loop runs, by which time the widgets exist.
    worker_ = new CameraWorker(settings_, std::move(outputDevice), this);
    connect(worker_, &CameraWorker::preview, this, &MainWindow::showPreview);
    connect(worker_, &CameraWorker::stats, this, &MainWindow::showStats);
    connect(worker_, &CameraWorker::status, this, &MainWindow::showStatus);
    connect(worker_, &CameraWorker::lensLocked, this, &MainWindow::onLensLocked);
    worker_->start();

    buildUi();
    loadIntoWidgets(settings_);
}

// --- layout -----------------------------------------------------------------------------

void MainWindow::buildUi() {
    preview_ = new QLabel("Starting camera…");
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(480, 270);
    preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    preview_->setStyleSheet("background: #111; color: #aaa;");

    info_ = new QLabel;
    info_->setStyleSheet("font-family: monospace;");

    auto* left = new QVBoxLayout;
    left->addWidget(preview_, 1);
    left->addWidget(info_);

    auto* controls = new QVBoxLayout;
    controls->addWidget(focusGroup());
    controls->addWidget(wbGroup());
    controls->addWidget(exposureGroup());
    controls->addWidget(imageGroup());
    controls->addWidget(outputGroup());
    controls->addStretch(1);

    auto* panel = new QWidget;
    panel->setLayout(controls);
    auto* scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(400);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* root = new QHBoxLayout;
    root->addLayout(left, 1);
    root->addWidget(scroll);
    auto* central = new QWidget;
    central->setLayout(root);
    setCentralWidget(central);
    statusBar();
    resize(1200, 720);
}

QWidget* MainWindow::focusGroup() {
    auto* box = new QGroupBox("Focus");
    auto* form = new QFormLayout(box);
    autoFocus_ = new QCheckBox("Continuous autofocus");
    lens_ = new SliderRow(0, 255);
    focusOnce_ = new QPushButton("Focus once, then lock");
    form->addRow(autoFocus_);
    form->addRow("Lens position", lens_);
    form->addRow(focusOnce_);

    connect(autoFocus_, &QCheckBox::toggled, this, [this](bool v) { set([v](Settings& s) { s.autoFocus = v; }); });
    connect(lens_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.lensPosition = v; }); });
    connect(focusOnce_, &QPushButton::clicked, this, &MainWindow::onFocusOnce);
    return box;
}

QWidget* MainWindow::wbGroup() {
    auto* box = new QGroupBox("White balance");
    auto* form = new QFormLayout(box);
    wbMode_ = new QComboBox;
    fill(wbMode_, kWbModes);
    wbWarmth_ = new SliderRow(-10, 10, 1, signed_);
    wbKelvin_ = new SliderRow(1000, 12000, 50, [](int v) { return QString("%1 K").arg(v); });
    wbLock_ = new QPushButton("Lock current");
    form->addRow("Mode", wbMode_);
    form->addRow("Warmth", wbWarmth_);
    form->addRow("Temperature", wbKelvin_);
    form->addRow(wbLock_);

    connect(wbMode_, &QComboBox::currentIndexChanged, this, [this](int) {
        auto mode = wbMode_->currentData().toString();
        set([mode](Settings& s) { s.wbMode = mode; });
    });
    connect(wbWarmth_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.wbWarmth = v; }); });
    connect(wbKelvin_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.wbKelvin = v; }); });
    connect(wbLock_, &QPushButton::clicked, this, &MainWindow::onWbLock);
    return box;
}

QWidget* MainWindow::exposureGroup() {
    auto* box = new QGroupBox("Exposure");
    auto* form = new QFormLayout(box);
    autoExposure_ = new QCheckBox("Auto exposure");
    faceMetering_ = new QCheckBox("Meter on face");
    faceMetering_->setToolTip("Aim auto exposure, and continuous autofocus, at the largest face in view");
    exposureComp_ = new SliderRow(-9, 9, 1, signed_);
    exposureUs_ = new SliderRow(100, 33000, 100, [](int v) { return QString::asprintf("%.1f ms", v / 1000.0); });
    iso_ = new SliderRow(100, 3200, 50);
    antiBanding_ = new QComboBox;
    fill(antiBanding_, kAntiBanding);
    form->addRow(autoExposure_);
    form->addRow(faceMetering_);
    form->addRow("Compensation", exposureComp_);
    form->addRow("Shutter", exposureUs_);
    form->addRow("ISO", iso_);
    form->addRow("Anti-flicker", antiBanding_);

    connect(autoExposure_, &QCheckBox::toggled, this, [this](bool v) { set([v](Settings& s) { s.autoExposure = v; }); });
    connect(faceMetering_, &QCheckBox::toggled, this, [this](bool v) { set([v](Settings& s) { s.faceMetering = v; }); });
    connect(exposureComp_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.exposureCompensation = v; }); });
    connect(exposureUs_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.exposureUs = v; }); });
    connect(iso_, &SliderRow::changed, this, [this](int v) { set([v](Settings& s) { s.iso = v; }); });
    connect(antiBanding_, &QComboBox::currentIndexChanged, this, [this](int) {
        auto mode = antiBanding_->currentData().toString();
        set([mode](Settings& s) { s.antiBanding = mode; });
    });
    return box;
}

QWidget* MainWindow::imageGroup() {
    auto* box = new QGroupBox("Image");
    auto* form = new QFormLayout(box);
    tuning_ = new QComboBox;
    fill(tuning_, kTunings);
    tuning_->setToolTip("Colour and exposure tuning loaded onto the camera. Changing it restarts the camera (about 20 s).");
    form->addRow("Tuning", tuning_);
    connect(tuning_, &QComboBox::currentIndexChanged, this, [this](int) {
        auto t = tuning_->currentData().toString();
        set([t](Settings& s) { s.tuning = t; });
    });

    auto add = [&](const char* label, SliderRow*& row, int lo, int hi, SliderRow::Format fmt, int Settings::*field) {
        row = new SliderRow(lo, hi, 1, std::move(fmt));
        form->addRow(label, row);
        connect(row, &SliderRow::changed, this, [this, field](int v) { set([v, field](Settings& s) { s.*field = v; }); });
    };
    add("Contrast", contrast_, -10, 10, signed_, &Settings::contrast);
    add("Saturation", saturation_, -10, 10, signed_, &Settings::saturation);
    add("Sharpness", sharpness_, 0, 4, {}, &Settings::sharpness);
    add("Noise reduction", lumaDenoise_, 0, 4, {}, &Settings::lumaDenoise);
    add("Colour noise reduction", chromaDenoise_, 0, 4, {}, &Settings::chromaDenoise);
    return box;
}

QWidget* MainWindow::outputGroup() {
    auto* box = new QGroupBox("Output");
    auto* layout = new QVBoxLayout(box);
    outputEnabled_ = new QCheckBox("Send to virtual camera (v4l2loopback)");
    auto* reset = new QPushButton("Reset all to defaults");
    layout->addWidget(outputEnabled_);
    layout->addWidget(reset);
    connect(outputEnabled_, &QCheckBox::toggled, this, [this](bool v) { set([v](Settings& s) { s.outputEnabled = v; }); });
    connect(reset, &QPushButton::clicked, this, &MainWindow::onReset);
    return box;
}

// --- state ------------------------------------------------------------------------------

void MainWindow::loadIntoWidgets(const Settings& s) {
    loading_ = true;
    autoFocus_->setChecked(s.autoFocus);
    lens_->setValue(s.lensPosition);
    selectData(wbMode_, s.wbMode, false);
    wbWarmth_->setValue(s.wbWarmth);
    wbKelvin_->setValue(s.wbKelvin);
    autoExposure_->setChecked(s.autoExposure);
    faceMetering_->setChecked(s.faceMetering);
    exposureComp_->setValue(s.exposureCompensation);
    exposureUs_->setValue(s.exposureUs);
    iso_->setValue(s.iso);
    selectData(antiBanding_, s.antiBanding, false);
    selectData(tuning_, s.tuning, false);
    contrast_->setValue(s.contrast);
    saturation_->setValue(s.saturation);
    sharpness_->setValue(s.sharpness);
    lumaDenoise_->setValue(s.lumaDenoise);
    chromaDenoise_->setValue(s.chromaDenoise);
    outputEnabled_->setChecked(s.outputEnabled);
    loading_ = false;
    updateEnabled();
}

void MainWindow::updateEnabled() {
    const Settings& s = settings_;
    lens_->setEnabled(!s.autoFocus);
    wbWarmth_->setEnabled(s.wbMode == wb::kAutoBiased);
    wbKelvin_->setEnabled(s.wbMode == wb::kManual);
    wbLock_->setEnabled(s.wbMode != wb::kManual);
    exposureComp_->setEnabled(s.autoExposure);
    faceMetering_->setEnabled(s.autoExposure || s.autoFocus);
    exposureUs_->setEnabled(!s.autoExposure);
    iso_->setEnabled(!s.autoExposure);
}

void MainWindow::set(const std::function<void(Settings&)>& change) {
    if (loading_) return;
    Settings next = settings_;
    change(next);
    auto keys = Settings::diff(settings_, next);
    if (keys.isEmpty()) return;
    settings_ = next;
    worker_->update(settings_, keys);
    updateEnabled();
    saveTimer_.start();
}

void MainWindow::save() { settings_.save(); }

// --- actions ----------------------------------------------------------------------------

void MainWindow::onFocusOnce() {
    autoFocus_->setChecked(false);
    worker_->focusOnce();
}

void MainWindow::onLensLocked(int position) {
    settings_.autoFocus = false;
    settings_.lensPosition = position;
    lens_->setValue(position, true);
    updateEnabled();
    saveTimer_.start();
}

void MainWindow::onWbLock() {
    int kelvin = lastStats_ ? lastStats_->wbKelvin : settings_.wbKelvin;
    wbKelvin_->setValue(kelvin, true);
    set([kelvin](Settings& s) {
        s.wbMode = wb::kManual;
        s.wbKelvin = kelvin;
    });
    selectData(wbMode_, wb::kManual, true);
}

void MainWindow::onReset() {
    Settings defaults;
    auto changed = Settings::diff(settings_, defaults);
    changed |= QSet<QString>{"auto_focus", "wb_mode", "auto_exposure"};
    settings_ = defaults;
    loadIntoWidgets(defaults);
    worker_->update(defaults, changed);
    saveTimer_.start();
}

// --- camera feedback --------------------------------------------------------------------

void MainWindow::showPreview(const QImage& img) {
    QPixmap pix = QPixmap::fromImage(img).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (lastStats_ && lastStats_->face && settings_.faceMetering) {
        const auto& f = *lastStats_->face;
        QPainter painter(&pix);
        painter.setPen(QPen(QColor(80, 220, 120), 2));
        painter.drawRect(int(f.x * pix.width()), int(f.y * pix.height()), int(f.w * pix.width()), int(f.h * pix.height()));
    }
    preview_->setPixmap(pix);
}

void MainWindow::showStats(const Stats& st) {
    lastStats_ = st;
    info_->setText(QString::asprintf("%4.1f fps   lens %3d   ISO %4d   shutter %4.1f ms   WB %d K   → ", st.fps, st.lensPosition, st.iso,
                                     st.exposureUs / 1000.0, st.wbKelvin) +
                   st.output);
    // sliders follow what the camera is doing while it's in auto
    const Settings& s = settings_;
    if (s.autoFocus) lens_->setValue(st.lensPosition, true);
    if (s.wbMode != wb::kManual) wbKelvin_->setValue(st.wbKelvin, true);
    if (s.autoExposure) {
        exposureUs_->setValue(st.exposureUs, true);
        iso_->setValue(st.iso, true);
    }
}

void MainWindow::showStatus(const QString& text) {
    statusBar()->showMessage(text);
    if (!text.startsWith("Streaming")) preview_->setText(text);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) worker_->setPreviewEnabled(!isMinimized());
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    save();
    worker_->stop();
    QMainWindow::closeEvent(event);
}
