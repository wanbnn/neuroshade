#include "layer/resource_tracker/analyzer.hpp"
#include "layer/resource_tracker/bindings.hpp"
#include "layer/resource_tracker/preview.hpp"
#include "layer/resource_tracker/resource_tracker.hpp"

#include <filesystem>
#include <iostream>

using namespace neuroshade::resources;

int main() {
    ResourceTracker tracker;
    VkImageCreateInfo depth_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.extent = {1920, 1080, 1};
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    const auto depth_handle = reinterpret_cast<VkImage>(std::uintptr_t{0x1000});
    const auto wrong_handle = reinterpret_cast<VkImage>(std::uintptr_t{0x2000});
    const auto view = reinterpret_cast<VkImageView>(std::uintptr_t{0x3000});
    const auto motion_handle = reinterpret_cast<VkImage>(std::uintptr_t{0x5000});
    const auto motion_view = reinterpret_cast<VkImageView>(std::uintptr_t{0x6000});
    const auto depth_id = tracker.track_image(depth_handle, depth_info, 2);
    VkImageCreateInfo color_info = depth_info;
    color_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_info.extent = {960, 540, 1};
    color_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto wrong_id = tracker.track_image(wrong_handle, color_info, 3);
    VkImageCreateInfo motion_info = color_info;
    motion_info.format = VK_FORMAT_R16G16_SFLOAT;
    motion_info.extent = {1920, 1080, 1};
    motion_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto motion_id = tracker.track_image(motion_handle, motion_info, 3);
    tracker.track_view(view, depth_handle);
    tracker.track_view(motion_view, motion_handle);
    tracker.observe_view_write(view, 4);
    tracker.observe_view_write(view, 5);
    tracker.observe_view_write(motion_view, 4);
    tracker.observe_view_write(motion_view, 5);
    const auto depth = rank_candidates(tracker.images(), CandidateRole::depth, {1920, 1080});
    if (depth.empty() || depth.front().image.id != depth_id || depth.front().confidence < 0.9F) {
        std::cerr << "known testbed depth was not ranked first\n"; return 1;
    }
    const auto low_res = rank_candidates(tracker.images(), CandidateRole::low_res_color, {1920, 1080});
    if (low_res.empty() || low_res.front().image.id != wrong_id) {
        std::cerr << "low-resolution scene color was not identified\n"; return 1;
    }
    const auto motion = rank_candidates(tracker.images(), CandidateRole::motion, {1920, 1080});
    if (motion.empty() || motion.front().image.id != motion_id || motion.front().confidence < 0.9F) {
        std::cerr << "motion vector target was not ranked first\n"; return 1;
    }

    BindingStore bindings;
    bindings.bind("Depth.Device", *tracker.image(wrong_handle), {1920, 1080});
    if (!bindings.get("Depth.Device") || bindings.get("Depth.Device")->runtime_id != wrong_id) {
        std::cerr << "intentional wrong binding failed\n"; return 1;
    }
    bindings.bind("Depth.Device", *tracker.image(depth_handle), {1920, 1080});
    bindings.bind("Motion.Screen", *tracker.image(motion_handle), {1920, 1080});
    bindings.bind("Color.LowRes", *tracker.image(wrong_handle), {1920, 1080});
    const auto path = std::filesystem::temp_directory_path() / "neuroshade-bindings-test.json";
    std::string error;
    if (!bindings.save(path, error)) { std::cerr << error << '\n'; return 1; }
    BindingStore restarted;
    if (!restarted.load(path, error)) { std::cerr << error << '\n'; return 1; }
    std::filesystem::remove(path);
    ImageInfo recreated = *tracker.image(depth_handle);
    recreated.id = 99;
    recreated.image = reinterpret_cast<VkImage>(std::uintptr_t{0x4000});
    ImageInfo recreated_motion = *tracker.image(motion_handle);
    recreated_motion.id = 100;
    recreated_motion.image = reinterpret_cast<VkImage>(std::uintptr_t{0x7000});
    ImageInfo recreated_low_res = *tracker.image(wrong_handle);
    recreated_low_res.id = 101;
    recreated_low_res.image = reinterpret_cast<VkImage>(std::uintptr_t{0x8000});
    const std::vector<ImageInfo> recreated_candidates{
        recreated, recreated_motion, recreated_low_res};
    const auto match = restarted.rebind("Depth.Device", recreated_candidates, {1920, 1080});
    if (!match || match->first != 99 || match->second < 0.95F) {
        std::cerr << "fingerprint did not survive restart\n"; return 1;
    }
    const auto motion_match = restarted.rebind("Motion.Screen", recreated_candidates, {1920, 1080});
    const auto low_res_match = restarted.rebind("Color.LowRes", recreated_candidates, {1920, 1080});
    if (!motion_match || motion_match->first != 100 || motion_match->second < 0.95F ||
        !low_res_match || low_res_match->first != 101 || low_res_match->second < 0.95F) {
        std::cerr << "motion/low-resolution fingerprints did not survive restart\n"; return 1;
    }

    const std::vector<float> depth_values{0.F, 0.25F, 0.5F, 1.F};
    const auto preview = make_preview(depth_values, 2, 2, 1, PreviewMode::depth_normalized);
    if (preview.size() != 16 || preview.front() != 0 || preview[12] != 255) {
        std::cerr << "depth preview failed\n"; return 1;
    }
    const std::vector<float> vector_values{
        -1.F, 0.F, 1.F, 1.F, 0.F, 0.5F, 0.F, 1.F,
        0.25F, -0.5F, 0.75F, 1.F, 1.F, 1.F, -1.F, 0.5F};
    for (const auto mode : {PreviewMode::rgba, PreviewMode::rgb, PreviewMode::red, PreviewMode::green,
                            PreviewMode::blue, PreviewMode::depth_linearized, PreviewMode::motion_xy,
                            PreviewMode::normal}) {
        if (make_preview(vector_values, 2, 2, 4, mode).size() != 16) {
            std::cerr << "preview mode failed\n"; return 1;
        }
    }
    const auto motion_preview = make_preview(vector_values, 2, 2, 4, PreviewMode::motion_xy);
    if (motion_preview[0] != 0 || motion_preview[1] != 128 || motion_preview[2] != 128 ||
        motion_preview[12] != 255 || motion_preview[13] != 255 || motion_preview[14] != 128) {
        std::cerr << "motion XY preview encoding failed\n"; return 1;
    }
    tracker.destroy_view(view);
    tracker.destroy_image(depth_handle);
    if (tracker.image(depth_handle) || tracker.image_count() != 2) {
        std::cerr << "destroyed resource was retained\n"; return 1;
    }
    const auto next_id = tracker.track_image(depth_handle, depth_info, 10);
    if (next_id <= wrong_id) { std::cerr << "stable IDs were reused\n"; return 1; }
    return 0;
}
