// Decode YuNet (OpenCV 2022, prior-box variant) outputs into the most confident face.
// The network runs on the camera; only its three small tensors come back to the host.
// Decoding follows OpenCV's face_detect.cpp for a 320x240 input.
#pragma once

#include <cstddef>
#include <optional>

namespace faces {

constexpr int kInputW = 320;
constexpr int kInputH = 240;

struct Face {
    float x, y, w, h;  // normalised 0..1
    float score;
};

// Number of prior boxes the tensors must have (rows).
size_t priorCount();

// loc: [N,14] (x, y, w, h, landmarks...), conf: [N,2], iou: [N,1]; row strides in floats.
std::optional<Face> bestFace(const float* loc, size_t locStride, const float* conf, size_t confStride, const float* iou, size_t iouStride, size_t rows);

}  // namespace faces
