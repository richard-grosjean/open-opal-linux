#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

QString Settings::path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/open-opal-linux/settings.json";
}

namespace {

QJsonObject toJson(const Settings& s) {
    return QJsonObject{
        {"auto_focus", s.autoFocus},
        {"lens_position", s.lensPosition},
        {"wb_mode", s.wbMode},
        {"wb_warmth", s.wbWarmth},
        {"wb_kelvin", s.wbKelvin},
        {"auto_exposure", s.autoExposure},
        {"exposure_compensation", s.exposureCompensation},
        {"exposure_us", s.exposureUs},
        {"iso", s.iso},
        {"face_metering", s.faceMetering},
        {"anti_banding", s.antiBanding},
        {"tuning", s.tuning},
        {"contrast", s.contrast},
        {"saturation", s.saturation},
        {"sharpness", s.sharpness},
        {"luma_denoise", s.lumaDenoise},
        {"chroma_denoise", s.chromaDenoise},
        {"output_enabled", s.outputEnabled},
    };
}

Settings fromJson(const QJsonObject& o) {
    Settings d;  // defaults for anything missing
    Settings s;
    auto b = [&](const char* k, bool def) { return o.value(k).toBool(def); };
    auto i = [&](const char* k, int def) { return o.value(k).toInt(def); };
    auto str = [&](const char* k, const QString& def) { return o.value(k).toString(def); };
    s.autoFocus = b("auto_focus", d.autoFocus);
    s.lensPosition = i("lens_position", d.lensPosition);
    s.wbMode = str("wb_mode", d.wbMode);
    s.wbWarmth = i("wb_warmth", d.wbWarmth);
    s.wbKelvin = i("wb_kelvin", d.wbKelvin);
    s.autoExposure = b("auto_exposure", d.autoExposure);
    s.exposureCompensation = i("exposure_compensation", d.exposureCompensation);
    s.exposureUs = i("exposure_us", d.exposureUs);
    s.iso = i("iso", d.iso);
    s.faceMetering = b("face_metering", d.faceMetering);
    s.antiBanding = str("anti_banding", d.antiBanding);
    s.tuning = str("tuning", d.tuning);
    s.contrast = i("contrast", d.contrast);
    s.saturation = i("saturation", d.saturation);
    s.sharpness = i("sharpness", d.sharpness);
    s.lumaDenoise = i("luma_denoise", d.lumaDenoise);
    s.chromaDenoise = i("chroma_denoise", d.chromaDenoise);
    s.outputEnabled = b("output_enabled", d.outputEnabled);
    return s;
}

}  // namespace

Settings Settings::load() {
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return fromJson(doc.object());
}

void Settings::save() const {
    QDir().mkpath(QFileInfo(path()).path());
    QFile f(path());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(toJson(*this)).toJson(QJsonDocument::Indented));
}

QSet<QString> Settings::diff(const Settings& a, const Settings& b) {
    QSet<QString> out;
    auto ja = toJson(a), jb = toJson(b);
    for (auto it = ja.begin(); it != ja.end(); ++it)
        if (jb.value(it.key()) != it.value()) out.insert(it.key());
    return out;
}
