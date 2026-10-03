// Runs the DepthAI pipeline on the Opal C1 and applies settings to the on-chip ISP.
//
// The sensor captures 4K; the Myriad X ISP denoises, sharpens, runs AE/AWB/AF and scales
// to 1080p NV12 on the device. The host only forwards those frames to v4l2loopback, plus a
// small BGR preview for the UI.
#pragma once

#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "faces.h"
#include "settings.h"

struct Stats {
    double fps = 0.0;
    int lensPosition = 0;
    int iso = 0;
    int exposureUs = 0;
    int wbKelvin = 0;
    QString output;
    std::optional<faces::Face> face;  // normalised x, y, w, h
};
Q_DECLARE_METATYPE(Stats)

// Owns the device in a background thread; reconnects if the camera goes away.
// Signals are emitted from the camera thread; connect them to GUI-thread objects
// (the connection is queued automatically).
class CameraWorker : public QObject {
    Q_OBJECT
public:
    explicit CameraWorker(const Settings& settings, std::optional<QString> outputDevice, QObject* parent = nullptr);
    ~CameraWorker() override;

    void start();
    void stop();
    // Apply new settings; keys = names of changed fields (empty set = everything).
    void update(const Settings& s, const QSet<QString>& keys);
    void focusOnce();
    void setPreviewEnabled(bool enabled);
    void restart();  // reconnect, e.g. after a setting that only applies at startup

signals:
    void preview(const QImage& image);
    void stats(const Stats& stats);
    void status(const QString& text);
    void lensLocked(int position);

private:
    struct Impl;
    void run();
    bool waitForCamera();
    void session();
    void sleepFor(double seconds);
    void blankOutput();

    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
    Settings settings_;
    std::vector<std::pair<Settings, QSet<QString>>> pending_;
    std::optional<QString> outputDevice_;
    std::atomic<bool> running_{false};
    std::atomic<bool> oneShotFocus_{false};
    std::atomic<bool> restart_{false};
    std::atomic<bool> previewEnabled_{true};
    std::thread thread_;
};
