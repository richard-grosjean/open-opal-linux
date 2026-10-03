#include "v4l2out.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

V4L2LoopbackOutput::V4L2LoopbackOutput(const std::string& device, int width, int height)
    : device_(device), frameSize_(static_cast<size_t>(width) * height * 3 / 2) {
    fd_ = ::open(device.c_str(), O_WRONLY);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), device);

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width;  // luma plane
    fmt.fmt.pix.sizeimage = frameSize_;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
    if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::generic_category(), device);
    }
}

V4L2LoopbackOutput::~V4L2LoopbackOutput() {
    if (fd_ >= 0) ::close(fd_);
}

void V4L2LoopbackOutput::write(const uint8_t* data, size_t size) {
    if (size != frameSize_) throw std::invalid_argument("frame is " + std::to_string(size) + " bytes, expected " + std::to_string(frameSize_));
    size_t done = 0;
    while (done < size) {
        ssize_t n = ::write(fd_, data + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), device_);
        }
        done += static_cast<size_t>(n);
    }
}

void V4L2LoopbackOutput::writeBlank() {
    std::vector<uint8_t> frame(frameSize_, 128);  // chroma planes: neutral
    std::fill(frame.begin(), frame.begin() + frameSize_ * 2 / 3, 16);  // luma plane: video black
    write(frame.data(), frame.size());
}

std::optional<std::string> findLoopbackDevice() {
    std::error_code ec;
    std::vector<std::pair<int, std::string>> found;
    for (const auto& e : std::filesystem::directory_iterator("/sys/devices/virtual/video4linux", ec)) {
        auto name = e.path().filename().string();
        if (name.rfind("video", 0) != 0) continue;
        int n = 0;
        try { n = std::stoi(name.substr(5)); } catch (...) {}
        found.emplace_back(n, name);
    }
    if (found.empty()) return std::nullopt;
    std::sort(found.begin(), found.end());
    return "/dev/" + found.front().second;
}
