// Persistent user settings, stored as JSON at ~/.config/open-opal-linux/settings.json.
// Same file and keys as the Python version, so existing settings carry over.
#pragma once

#include <QString>
#include <QStringList>
#include <QSet>

namespace wb {
inline const QString kAuto = "auto";              // the chip's own auto white balance
inline const QString kAutoBiased = "auto_biased";  // host-side feedback loop with a warmth bias
inline const QString kManual = "manual";
}  // namespace wb

struct Settings {
    // focus
    bool autoFocus = false;
    int lensPosition = 120;  // 0..255

    // white balance
    QString wbMode = wb::kAuto;
    int wbWarmth = 4;      // -10 (cooler) .. +10 (warmer), used by auto_biased; 0 = grey-world
    int wbKelvin = 4000;   // 1000..12000, used by manual

    // exposure
    bool autoExposure = true;
    int exposureCompensation = 0;  // -9..9
    int exposureUs = 20000;        // 1..33000 keeps 30 fps
    int iso = 800;                 // 100..3200
    bool faceMetering = true;      // aim auto exposure (and continuous AF) at the largest face
    QString antiBanding = "MAINS_50_HZ";  // OFF, AUTO, MAINS_50_HZ, MAINS_60_HZ

    // image processing on the chip
    QString tuning = "low_light";  // "low_light" or "default"; applies on connect
    int brightness = 0;   // -10..10
    int contrast = 0;     // -10..10
    int saturation = 0;   // -10..10
    int sharpness = 1;    // 0..4
    int lumaDenoise = 2;  // 0..4
    int chromaDenoise = 2;  // 0..4

    // output
    bool outputEnabled = true;

    static QString path();
    static Settings load();
    void save() const;

    // Names of fields that differ between two settings (Python-style keys).
    static QSet<QString> diff(const Settings& a, const Settings& b);
    bool operator==(const Settings&) const = default;
};
