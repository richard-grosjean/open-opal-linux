#pragma once

#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>

#include "camera.h"
#include "settings.h"

class SliderRow : public QWidget {
    Q_OBJECT
public:
    using Format = std::function<QString(int)>;
    SliderRow(int lo, int hi, int step = 1, Format fmt = {}, QWidget* parent = nullptr);
    int value() const { return slider_->value(); }
    void setValue(int v, bool quiet = false);
signals:
    void changed(int value);
private:
    QSlider* slider_;
    QLabel* label_;
    Format fmt_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(std::optional<QString> outputDevice, QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    QWidget* focusGroup();
    QWidget* wbGroup();
    QWidget* exposureGroup();
    QWidget* imageGroup();
    QWidget* outputGroup();
    void loadIntoWidgets(const Settings& s);
    void updateEnabled();
    // Apply a change to the settings (mutator sets the fields); notifies the camera and schedules a save.
    void set(const std::function<void(Settings&)>& change);
    void save();

    void onFocusOnce();
    void onLensLocked(int position);
    void onWbLock();
    void onReset();
    void showPreview(const QImage& img);
    void showStats(const Stats& st);
    void showStatus(const QString& text);

    Settings settings_;
    CameraWorker* worker_ = nullptr;
    QTimer saveTimer_;
    bool loading_ = true;  // widgets fire change signals while being built
    std::optional<Stats> lastStats_;

    QLabel* preview_ = nullptr;
    QLabel* info_ = nullptr;
    QCheckBox* autoFocus_ = nullptr;
    SliderRow* lens_ = nullptr;
    QPushButton* focusOnce_ = nullptr;
    QComboBox* wbMode_ = nullptr;
    SliderRow* wbWarmth_ = nullptr;
    SliderRow* wbKelvin_ = nullptr;
    QPushButton* wbLock_ = nullptr;
    QCheckBox* autoExposure_ = nullptr;
    QCheckBox* faceMetering_ = nullptr;
    SliderRow* exposureComp_ = nullptr;
    SliderRow* exposureUs_ = nullptr;
    SliderRow* iso_ = nullptr;
    QComboBox* antiBanding_ = nullptr;
    QComboBox* tuning_ = nullptr;
    SliderRow* brightness_ = nullptr;
    SliderRow* contrast_ = nullptr;
    SliderRow* saturation_ = nullptr;
    SliderRow* sharpness_ = nullptr;
    SliderRow* lumaDenoise_ = nullptr;
    SliderRow* chromaDenoise_ = nullptr;
    QCheckBox* outputEnabled_ = nullptr;
};
