#pragma once

#include <cstdint>
#include <string_view>

namespace neuroshade::framegraph {

using ResourceHandle = std::uint64_t;
inline constexpr ResourceHandle kInvalidResource = 0;

namespace semantic {
inline constexpr std::string_view color_final = "Color.Final";
inline constexpr std::string_view color_scene = "Color.Scene";
inline constexpr std::string_view color_low_resolution = "Color.LowRes";
inline constexpr std::string_view depth_device = "Depth.Device";
inline constexpr std::string_view depth_linear = "Depth.Linear";
inline constexpr std::string_view motion_screen = "Motion.Screen";
inline constexpr std::string_view normal_world = "Normal.World";
inline constexpr std::string_view normal_view = "Normal.View";
inline constexpr std::string_view history_color_1 = "History.Color.1";
inline constexpr std::string_view history_color_2 = "History.Color.2";
inline constexpr std::string_view history_color_3 = "History.Color.3";
inline constexpr std::string_view history_depth_1 = "History.Depth.1";
inline constexpr std::string_view history_motion_1 = "History.Motion.1";
inline constexpr std::string_view output_color = "Output.Color";
}  // namespace semantic

struct Frame {
    std::uint64_t frame_index{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    ResourceHandle final_color{kInvalidResource};
    ResourceHandle scene_color{kInvalidResource};
    ResourceHandle low_res_color{kInvalidResource};
    ResourceHandle depth_device{kInvalidResource};
    ResourceHandle depth_linear{kInvalidResource};
    ResourceHandle motion{kInvalidResource};
    ResourceHandle normals{kInvalidResource};
    float delta_time{};
    bool has_scene_color{};
    bool has_low_res_color{};
    bool has_depth{};
    bool has_motion{};
    bool has_normals{};
    bool history_valid{};
};

}  // namespace neuroshade::framegraph
