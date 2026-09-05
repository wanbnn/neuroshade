#pragma once

#include "framegraph/compiler/compiler.hpp"
#include "profile/profile.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace neuroshade::runtime {

enum class EffectBackend { shader, neural };
enum class PipelineMode { pass_through, shader_only, neural_spatial, neural_temporal };

struct PlannedEffect {
    std::string plugin;
    std::string pass;
    std::string artifact;
    EffectBackend backend{EffectBackend::shader};
    float strength{1.0F};
};

struct PipelinePreparation {
    framegraph::ExecutionPlan plan;
    std::vector<PlannedEffect> effects;
    PipelineMode mode{PipelineMode::pass_through};
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

// Resolve enabled profile entries into an ordered FrameGraph before any GPU
// resources are allocated. Invalid or missing artifacts leave the caller in
// pass-through mode.
[[nodiscard]] PipelinePreparation prepare_pipeline(
    const profile::Profile& profile,
    const std::filesystem::path& plugin_root);

[[nodiscard]] const char* to_string(PipelineMode mode) noexcept;

}  // namespace neuroshade::runtime
