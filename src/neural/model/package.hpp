#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neuroshade::neural {

enum class ShapeKind { fixed, dynamic, bucketed };

struct TensorBinding {
    std::string semantic;
    std::string tensor;
    std::string dtype;
    std::string layout;
    bool optional{};
};

struct ShapeBucket {
    std::vector<std::size_t> input;
    std::vector<std::size_t> output;
};

struct ModelManifest {
    std::uint32_t schema{};
    std::string id;
    std::string name;
    std::string version;
    std::string runtime;
    std::vector<TensorBinding> inputs;
    TensorBinding output;
    std::uint32_t history{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    std::string first_frame;
    std::string missing_motion{"reject"};
    ShapeKind shape_kind{ShapeKind::fixed};
    std::vector<ShapeBucket> buckets;
};

struct ModelPackage {
    std::filesystem::path root;
    std::filesystem::path onnx_path;
    ModelManifest manifest;
    std::string content_hash;
};

struct PackageResult {
    ModelPackage package;
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

[[nodiscard]] PackageResult load_model_package(const std::filesystem::path& path);

} // namespace neuroshade::neural
