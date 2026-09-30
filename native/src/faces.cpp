#include "faces.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace faces {
namespace {

constexpr std::array<int, 4> kSteps = {8, 16, 32, 64};
const std::vector<std::vector<int>> kMinSizes = {{10, 16, 24}, {32, 48}, {64, 96}, {128, 192, 256}};
constexpr float kVariance0 = 0.1f, kVariance1 = 0.2f;
constexpr float kScoreThreshold = 0.6f;

struct Prior { float cx, cy, w, h; };

std::vector<Prior> makePriors(int w, int h) {
    int fh = ((h + 1) / 2) / 2, fw = ((w + 1) / 2) / 2;
    std::vector<std::pair<int, int>> sizes = {{fh / 2, fw / 2}};
    for (int k = 0; k < 3; ++k) sizes.push_back({sizes.back().first / 2, sizes.back().second / 2});
    std::vector<Prior> out;
    for (size_t k = 0; k < 4; ++k) {
        auto [ph, pw] = sizes[k];
        for (int i = 0; i < ph; ++i)
            for (int j = 0; j < pw; ++j)
                for (int m : kMinSizes[k])
                    out.push_back({(j + 0.5f) * kSteps[k] / w, (i + 0.5f) * kSteps[k] / h, float(m) / w, float(m) / h});
    }
    return out;
}

const std::vector<Prior>& priors() {
    static const std::vector<Prior> p = makePriors(kInputW, kInputH);
    return p;
}

}  // namespace

size_t priorCount() { return priors().size(); }

std::optional<Face> bestFace(const float* loc, size_t locStride, const float* conf, size_t confStride, const float* iou, size_t iouStride, size_t rows) {
    const auto& pr = priors();
    if (rows != pr.size()) return std::nullopt;
    size_t best = 0;
    float bestScore = -1.f;
    for (size_t i = 0; i < rows; ++i) {
        float io = std::clamp(iou[i * iouStride], 0.f, 1.f);
        float s = std::sqrt(conf[i * confStride + 1] * io);
        if (s > bestScore) { bestScore = s; best = i; }
    }
    if (bestScore < kScoreThreshold) return std::nullopt;
    const Prior& p = pr[best];
    const float* l = loc + best * locStride;
    float cx = p.cx + l[0] * kVariance0 * p.w;
    float cy = p.cy + l[1] * kVariance0 * p.h;
    float w = p.w * std::exp(l[2] * kVariance1);
    float h = p.h * std::exp(l[3] * kVariance1);
    float x0 = std::max(0.f, cx - w / 2), y0 = std::max(0.f, cy - h / 2);
    float x1 = std::min(1.f, cx + w / 2), y1 = std::min(1.f, cy + h / 2);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return Face{x0, y0, x1 - x0, y1 - y0, bestScore};
}

}  // namespace faces
