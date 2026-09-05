#include "layer/resource_tracker/resource_tracker.hpp"

#include <algorithm>

namespace neuroshade::resources {

std::uint64_t ResourceTracker::track_image(VkImage image, const VkImageCreateInfo& info,
                                           std::uint64_t frame, bool swapchain) {
    std::scoped_lock lock(mutex_);
    ImageInfo tracked{};
    tracked.id = next_image_id_++;
    tracked.image = image;
    tracked.format = info.format;
    tracked.extent = info.extent;
    tracked.usage = info.usage;
    tracked.samples = info.samples;
    tracked.create_frame = frame;
    tracked.swapchain_image = swapchain;
    images_[image] = tracked;
    return tracked.id;
}

void ResourceTracker::destroy_image(VkImage image) {
    std::scoped_lock lock(mutex_);
    images_.erase(image);
    std::erase_if(views_, [image](const auto& entry) { return entry.second == image; });
}

std::uint64_t ResourceTracker::track_memory(VkDeviceMemory memory) {
    std::scoped_lock lock(mutex_);
    const auto [iterator, inserted] = memories_.try_emplace(memory, next_memory_id_);
    if (inserted) ++next_memory_id_;
    return iterator->second;
}

void ResourceTracker::destroy_memory(VkDeviceMemory memory) {
    std::scoped_lock lock(mutex_);
    const auto found = memories_.find(memory);
    if (found == memories_.end()) return;
    const auto destroyed_id = found->second;
    memories_.erase(found);
    for (auto& [image, info] : images_) {
        (void)image;
        if (info.memory_id == destroyed_id) info.memory_id = 0;
    }
}

void ResourceTracker::bind_image(VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    std::scoped_lock lock(mutex_);
    const auto found = images_.find(image);
    if (found == images_.end()) return;
    const auto memory_found = memories_.find(memory);
    if (memory_found != memories_.end()) found->second.memory_id = memory_found->second;
    found->second.memory_offset = offset;
}

void ResourceTracker::track_view(VkImageView view, VkImage image) {
    std::scoped_lock lock(mutex_);
    if (images_.contains(image)) views_[view] = image;
}

void ResourceTracker::destroy_view(VkImageView view) {
    std::scoped_lock lock(mutex_);
    views_.erase(view);
}

void ResourceTracker::observe_view_write(VkImageView view, std::uint64_t frame, bool attachment) {
    std::scoped_lock lock(mutex_);
    const auto view_found = views_.find(view);
    if (view_found == views_.end()) return;
    const auto image_found = images_.find(view_found->second);
    if (image_found == images_.end()) return;
    image_found->second.last_write_frame = frame;
    ++image_found->second.write_count;
    if (attachment) ++image_found->second.attachment_writes;
}

void ResourceTracker::observe_image_read(VkImage image, std::uint64_t frame) {
    std::scoped_lock lock(mutex_);
    if (const auto found = images_.find(image); found != images_.end()) {
        found->second.last_read_frame = frame;
        ++found->second.read_count;
    }
}

std::optional<ImageInfo> ResourceTracker::image(VkImage image) const {
    std::scoped_lock lock(mutex_);
    const auto found = images_.find(image);
    if (found == images_.end()) return std::nullopt;
    return found->second;
}

std::vector<ImageInfo> ResourceTracker::images() const {
    std::scoped_lock lock(mutex_);
    std::vector<ImageInfo> result;
    result.reserve(images_.size());
    for (const auto& [handle, info] : images_) { (void)handle; result.push_back(info); }
    std::ranges::sort(result, {}, &ImageInfo::id);
    return result;
}

std::size_t ResourceTracker::image_count() const {
    std::scoped_lock lock(mutex_);
    return images_.size();
}

}  // namespace neuroshade::resources
