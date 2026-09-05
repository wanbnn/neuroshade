#pragma once

#include "neural/model/package.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace neuroshade::neural {

struct TensorAllocation {
    std::string name;
    std::vector<std::size_t> dimensions;
    std::size_t byte_count{};
    bool output{};
};

enum class ExecutionResult { neural, copy_fallback };

[[nodiscard]] std::string active_hip_architecture();

class SpatialRuntime {
public:
    SpatialRuntime(ModelPackage package, std::string gfx_architecture);
    ~SpatialRuntime();

    SpatialRuntime(const SpatialRuntime&) = delete;
    SpatialRuntime& operator=(const SpatialRuntime&) = delete;

    [[nodiscard]] void* input_data(std::string_view tensor_name) const;
    [[nodiscard]] void* output_data() const noexcept;
    [[nodiscard]] std::size_t input_bytes(std::string_view tensor_name) const;
    [[nodiscard]] std::size_t output_bytes() const noexcept;
    [[nodiscard]] const std::vector<TensorAllocation>& tensor_plan() const noexcept;
    [[nodiscard]] const std::filesystem::path& cache_path() const noexcept;
    [[nodiscard]] bool cache_hit() const noexcept;
    [[nodiscard]] bool warmed_up() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

    void upload_input(std::string_view tensor_name, std::span<const std::byte> bytes);
    void download_output(std::span<std::byte> bytes) const;
    ExecutionResult execute();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuroshade::neural
