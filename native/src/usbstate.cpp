#include "usbstate.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
std::string readTrimmed(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

OpalState opalState() {
    std::error_code ec;
    for (const auto& dev : std::filesystem::directory_iterator("/sys/bus/usb/devices", ec)) {
        if (readTrimmed(dev.path() / "idVendor") != "03e7") continue;
        auto pid = readTrimmed(dev.path() / "idProduct");
        if (pid.empty()) continue;
        if (pid == "f63d") return OpalState::Camera;
        if (pid == "f63b") return OpalState::DepthAI;
        return OpalState::Bootloader;
    }
    return OpalState::Absent;
}
