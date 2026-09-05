#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace neuroshade::resources {

enum class PreviewMode { rgba, rgb, red, green, blue, depth_normalized, depth_linearized, motion_xy, normal };

[[nodiscard]] std::vector<std::uint8_t> make_preview(std::span<const float> values,
                                                    std::uint32_t width,
                                                    std::uint32_t height,
                                                    std::uint32_t channels,
                                                    PreviewMode mode,
                                                    float near_plane = 0.1F,
                                                    float far_plane = 1000.F);

}  // namespace neuroshade::resources
