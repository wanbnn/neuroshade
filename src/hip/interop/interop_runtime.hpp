#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace neuroshade::hip {

class ImportedBuffer {
public:
    ImportedBuffer(int opaque_fd, std::size_t allocation_size, std::size_t data_size);
    ~ImportedBuffer();

    ImportedBuffer(const ImportedBuffer&) = delete;
    ImportedBuffer& operator=(const ImportedBuffer&) = delete;

    [[nodiscard]] void* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_size_; }

private:
    void* memory_{nullptr};
    void* data_{nullptr};
    std::size_t data_size_{};
};

struct HipDeviceInfo {
    int ordinal{};
    std::string name;
    std::string architecture;
    int runtime_version{};
};

[[nodiscard]] HipDeviceInfo select_device();
[[nodiscard]] std::uint64_t hash_bytes(const void* device_data, std::size_t byte_count);
void run_color_effect(const void* input, void* output, std::size_t pixel_count);

class HostStagingWorkspace {
public:
    explicit HostStagingWorkspace(std::size_t byte_count);
    ~HostStagingWorkspace();

    HostStagingWorkspace(const HostStagingWorkspace&) = delete;
    HostStagingWorkspace& operator=(const HostStagingWorkspace&) = delete;

    void process(const void* host_input, void* host_output);

private:
    std::size_t byte_count_{};
    void* pinned_input_{nullptr};
    void* pinned_output_{nullptr};
    void* device_input_{nullptr};
    void* device_output_{nullptr};
};

} // namespace neuroshade::hip
