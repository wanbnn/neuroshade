#pragma once

#include "layer/resource_tracker/resource_tracker.hpp"

#include <string>
#include <vector>

namespace neuroshade::resources {

enum class CandidateRole { depth, motion, scene_color, low_res_color };

struct Candidate {
    ImageInfo image;
    CandidateRole role{};
    float confidence{};
    std::vector<std::string> reasons;
};

struct Fingerprint {
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint16_t width_per_mille{};
    std::uint16_t height_per_mille{};
    VkImageUsageFlags usage{};
    std::uint8_t attachment_bucket{};
    std::uint8_t creation_bucket{};
    std::uint8_t write_bucket{};
    std::uint8_t read_bucket{};
};

[[nodiscard]] std::vector<Candidate> rank_candidates(const std::vector<ImageInfo>& images,
                                                     CandidateRole role,
                                                     VkExtent2D output_extent);
[[nodiscard]] Fingerprint fingerprint(const ImageInfo& image, VkExtent2D output_extent);
[[nodiscard]] float fingerprint_confidence(const Fingerprint& saved, const Fingerprint& candidate);
[[nodiscard]] bool is_depth_format(VkFormat format) noexcept;

}  // namespace neuroshade::resources
