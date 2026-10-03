// Minimal writer for a v4l2loopback device: set the NV12 output format once, then
// each frame is a plain write().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

class V4L2LoopbackOutput {
public:
    // Throws std::system_error if the device cannot be opened or configured.
    V4L2LoopbackOutput(const std::string& device, int width, int height);
    ~V4L2LoopbackOutput();
    V4L2LoopbackOutput(const V4L2LoopbackOutput&) = delete;
    V4L2LoopbackOutput& operator=(const V4L2LoopbackOutput&) = delete;

    // Throws std::system_error on I/O error, std::invalid_argument on a wrong size.
    void write(const uint8_t* data, size_t size);
    // A black frame, so readers that open the device before the camera streams get a picture.
    void writeBlank();
    size_t frameSize() const { return frameSize_; }
    const std::string& device() const { return device_; }

private:
    std::string device_;
    size_t frameSize_;
    int fd_ = -1;
};

// The first /dev/videoN that belongs to v4l2loopback, if any.
std::optional<std::string> findLoopbackDevice();
