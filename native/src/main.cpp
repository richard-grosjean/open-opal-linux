#include <malloc.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QTimer>

#include "mainwindow.h"

int main(int argc, char** argv) {
    // Serve 3 MB frame buffers straight from mmap so they go back to the OS when freed;
    // otherwise glibc's adaptive threshold keeps them in the heap and RSS drifts up.
    mallopt(M_MMAP_THRESHOLD, 1 << 20);

    QApplication app(argc, argv);
    app.setApplicationName("Open Opal");
    app.setDesktopFileName("open-opal");

    QCommandLineParser parser;
    parser.setApplicationDescription("Opal C1 controls for Linux");
    parser.addHelpOption();
    QCommandLineOption device("device", "v4l2loopback device to write to (default: first one found)", "path");
    parser.addOption(device);
    QCommandLineOption screenshot("screenshot", "Save a PNG of the window after 40 s (for documentation)", "path");
    parser.addOption(screenshot);
    parser.process(app);

    std::optional<QString> outputDevice;
    if (parser.isSet(device)) outputDevice = parser.value(device);

    MainWindow window(outputDevice);
    window.show();
    if (parser.isSet(screenshot)) {
        QString path = parser.value(screenshot);
        QTimer::singleShot(40000, &window, [&window, path] { window.grab().save(path); });
    }
    return app.exec();
}
