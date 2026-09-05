#include "layer/resource_tracker/analyzer.hpp"

#include <algorithm>
#include <cmath>

namespace neuroshade::resources {
namespace {

bool is_motion_format(VkFormat format) {
    return format == VK_FORMAT_R16G16_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT ||
           format == VK_FORMAT_R16G16_SNORM || format == VK_FORMAT_R8G8_SNORM;
}

std::uint8_t bucket(std::uint32_t value) { return static_cast<std::uint8_t>(std::min(value, 15U)); }

}  // namespace

bool is_depth_format(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
        default: return false;
    }
}

std::vector<Candidate> rank_candidates(const std::vector<ImageInfo>& images,
                                       CandidateRole role, VkExtent2D output) {
    std::vector<Candidate> result;
    for (const auto& image : images) {
        if (image.swapchain_image) continue;
        Candidate candidate{image, role, 0.F, {}};
        const float width_ratio = output.width == 0 ? 0.F : float(image.extent.width) / float(output.width);
        const float height_ratio = output.height == 0 ? 0.F : float(image.extent.height) / float(output.height);
        const bool screen_sized = width_ratio >= 0.75F && width_ratio <= 1.05F &&
                                  height_ratio >= 0.75F && height_ratio <= 1.05F;
        const bool lower_resolution = width_ratio >= 0.2F && width_ratio < 0.75F &&
                                      height_ratio >= 0.2F && height_ratio < 0.75F;
        if (role == CandidateRole::depth) {
            if (is_depth_format(image.format)) { candidate.confidence += 0.45F; candidate.reasons.emplace_back("depth format"); }
            if ((image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                candidate.confidence += 0.25F; candidate.reasons.emplace_back("depth attachment usage");
            }
            if (screen_sized) { candidate.confidence += 0.15F; candidate.reasons.emplace_back("screen-sized"); }
            if (image.attachment_writes > 0) { candidate.confidence += 0.10F; candidate.reasons.emplace_back("attachment writes"); }
            if (image.write_count > 1) candidate.confidence += 0.05F;
        } else if (role == CandidateRole::motion) {
            if (is_motion_format(image.format)) { candidate.confidence += 0.45F; candidate.reasons.emplace_back("two-component signed/float format"); }
            if (screen_sized) candidate.confidence += 0.25F;
            if ((image.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) != 0) candidate.confidence += 0.15F;
            if (image.write_count > 1) candidate.confidence += 0.15F;
        } else {
            if ((image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
                candidate.confidence += 0.30F; candidate.reasons.emplace_back("color attachment usage");
            }
            if ((image.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) != 0) {
                candidate.confidence += 0.20F; candidate.reasons.emplace_back("later-readable color");
            }
            const bool extent_match = role == CandidateRole::low_res_color ? lower_resolution : (screen_sized || lower_resolution);
            if (extent_match) { candidate.confidence += 0.30F; candidate.reasons.emplace_back("expected relative extent"); }
            if (image.attachment_writes > 0) candidate.confidence += 0.10F;
            if (image.read_count > 0) candidate.confidence += 0.10F;
        }
        candidate.confidence = std::clamp(candidate.confidence, 0.F, 1.F);
        if (candidate.confidence > 0.F) result.push_back(std::move(candidate));
    }
    std::ranges::sort(result, [](const Candidate& left, const Candidate& right) {
        if (left.confidence != right.confidence) return left.confidence > right.confidence;
        return left.image.id < right.image.id;
    });
    return result;
}

Fingerprint fingerprint(const ImageInfo& image, VkExtent2D output) {
    const auto relative = [](std::uint32_t value, std::uint32_t reference) {
        return static_cast<std::uint16_t>(reference == 0 ? 0 : std::min(2000U, value * 1000U / reference));
    };
    return {image.format, relative(image.extent.width, output.width), relative(image.extent.height, output.height),
            image.usage, bucket(image.attachment_writes), bucket(static_cast<std::uint32_t>(image.create_frame / 60)),
            bucket(image.write_count), bucket(image.read_count)};
}

float fingerprint_confidence(const Fingerprint& saved, const Fingerprint& candidate) {
    float score = 0.F;
    if (saved.format == candidate.format) score += 0.35F;
    if (saved.usage == candidate.usage) score += 0.20F;
    const auto distance = [](std::uint16_t a, std::uint16_t b) { return std::abs(int(a) - int(b)); };
    if (distance(saved.width_per_mille, candidate.width_per_mille) <= 25 &&
        distance(saved.height_per_mille, candidate.height_per_mille) <= 25) score += 0.25F;
    if (saved.attachment_bucket == candidate.attachment_bucket) score += 0.08F;
    if (saved.creation_bucket == candidate.creation_bucket) score += 0.04F;
    if (saved.write_bucket == candidate.write_bucket) score += 0.04F;
    if (saved.read_bucket == candidate.read_bucket) score += 0.04F;
    return score;
}

}  // namespace neuroshade::resources
