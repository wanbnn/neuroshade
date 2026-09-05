#include "layer/resource_tracker/preview.hpp"

#include <algorithm>
#include <cmath>

namespace neuroshade::resources {

std::vector<std::uint8_t> make_preview(std::span<const float> values, std::uint32_t width,
                                       std::uint32_t height, std::uint32_t channels, PreviewMode mode,
                                       float near_plane, float far_plane) {
    const std::size_t pixels = std::size_t(width) * height;
    if (channels == 0 || values.size() < pixels * channels) return {};
    std::vector<std::uint8_t> output(pixels * 4, 255);
    float depth_min = 1.F;
    float depth_max = 0.F;
    const auto depth_value = [=](float value) {
        if (mode != PreviewMode::depth_linearized) return value;
        const float denominator = std::max(far_plane - value * (far_plane - near_plane), 1e-6F);
        return near_plane * far_plane / denominator;
    };
    if (mode == PreviewMode::depth_normalized || mode == PreviewMode::depth_linearized) {
        for (std::size_t i = 0; i < pixels; ++i) {
            depth_min = std::min(depth_min, depth_value(values[i * channels]));
            depth_max = std::max(depth_max, depth_value(values[i * channels]));
        }
    }
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.F, 1.F) * 255.F)); };
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto sample = [&](std::uint32_t channel, float fallback = 0.F) {
            return channel < channels ? values[i * channels + channel] : fallback;
        };
        float r = sample(0), g = sample(1, r), b = sample(2, r);
        if (mode == PreviewMode::red) g = b = r;
        else if (mode == PreviewMode::green) r = b = g;
        else if (mode == PreviewMode::blue) r = g = b;
        else if (mode == PreviewMode::depth_normalized || mode == PreviewMode::depth_linearized) {
            const float range = std::max(depth_max - depth_min, 1e-6F);
            r = g = b = (depth_value(sample(0)) - depth_min) / range;
        } else if (mode == PreviewMode::motion_xy) {
            r = sample(0) * 0.5F + 0.5F; g = sample(1) * 0.5F + 0.5F; b = 0.5F;
        } else if (mode == PreviewMode::normal) {
            r = sample(0) * 0.5F + 0.5F; g = sample(1) * 0.5F + 0.5F; b = sample(2) * 0.5F + 0.5F;
        }
        output[i * 4] = byte(r); output[i * 4 + 1] = byte(g); output[i * 4 + 2] = byte(b);
        output[i * 4 + 3] = mode == PreviewMode::rgba ? byte(sample(3, 1.F)) : 255;
    }
    return output;
}

}  // namespace neuroshade::resources
