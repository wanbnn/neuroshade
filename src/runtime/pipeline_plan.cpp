#include "runtime/pipeline_plan.hpp"

#include "framegraph/resources/frame.hpp"
#include "neural/model/package.hpp"
#include "plugins/shader/manifest.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace neuroshade::runtime {
namespace {

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& root, std::string_view plugin) {
    auto direct = root / std::string(plugin) / "manifest.toml";
    if (std::filesystem::is_regular_file(direct)) return direct;
    const auto separator = plugin.rfind('.');
    const std::string leaf(plugin.substr(separator == std::string_view::npos ? 0 : separator + 1));
    return root / leaf / "manifest.toml";
}

[[nodiscard]] bool temporal_effect(const profile::Effect& effect) {
    if (effect.plugin.find("temporal") != std::string::npos) return true;
    return std::ranges::any_of(effect.resource_bindings, [](const auto& binding) {
        return binding.semantic.starts_with("History.") || binding.semantic == "Motion.Screen";
    });
}

}  // namespace

PipelinePreparation prepare_pipeline(const profile::Profile& profile,
                                     const std::filesystem::path& plugin_root) {
    PipelinePreparation result;
    std::vector<framegraph::PassDescriptor> descriptors;
    std::unordered_map<std::string, framegraph::ResourceFormat> external{
        {std::string(framegraph::semantic::color_final), framegraph::ResourceFormat::rgba8_uint}};
    std::string input(framegraph::semantic::color_final);
    bool has_neural = false;
    bool has_temporal = false;

    std::size_t emitted = 0;
    for (const auto& effect : profile.pipeline) {
        if (!effect.enabled) continue;
        const bool neural = !effect.model.empty();
        has_neural = has_neural || neural;
        has_temporal = has_temporal || (neural && temporal_effect(effect));

        if (neural) {
            const auto model_path = plugin_root.parent_path() / effect.model;
            const auto package = neural::load_model_package(model_path);
            if (!package.valid()) {
                result.errors.push_back("model " + effect.model + ": " +
                                        (package.errors.empty() ? "invalid package"
                                                                : package.errors.front()));
                continue;
            }
            const std::string output = "Work." + std::to_string(++emitted);
            std::vector<framegraph::ResourceRequirement> inputs{
                {input, framegraph::ResourceFormat::unknown, true}};
            for (const auto& binding : effect.resource_bindings) {
                if (binding.semantic.empty()) continue;
                inputs.push_back({binding.semantic, framegraph::ResourceFormat::unknown, true});
                external.emplace(binding.semantic, framegraph::ResourceFormat::unknown);
            }
            descriptors.push_back({effect.plugin, framegraph::PassType::neural, std::move(inputs),
                                   {{output, framegraph::ResourceFormat::unknown}},
                                   framegraph::QueuePreference::compute,
                                   framegraph::kPluginApiVersion});
            result.effects.push_back(
                {effect.plugin, effect.plugin, model_path.string(), EffectBackend::neural,
                 effect.strength,effect.nr_controls,effect.nr_auto_mask,effect.nr_style,effect.nr_preset,effect.nr_intensity});
            input = output;
            continue;
        }

        const auto path = manifest_path(plugin_root, effect.plugin);
        const auto loaded = plugins::load_shader_manifest(path);
        if (!loaded.valid()) {
            result.errors.push_back("plugin " + effect.plugin + ": " +
                                    (loaded.errors.empty() ? "invalid manifest" : loaded.errors.front()));
            continue;
        }
        if (loaded.manifest.id != effect.plugin) {
            result.errors.push_back("plugin id mismatch for " + effect.plugin);
            continue;
        }
        for (const auto& pass : loaded.manifest.passes) {
            auto shader_path = path.parent_path() / pass.shader;
            if (!std::filesystem::is_regular_file(shader_path)) {
                shader_path = plugin_root.parent_path() / "shaders" / pass.shader;
            }
            if (!std::filesystem::is_regular_file(shader_path)) {
                result.errors.push_back("shader artifact is missing for " + effect.plugin +
                                        ": " + pass.shader);
                continue;
            }
            ++emitted;
            const std::string output = "Work." + std::to_string(emitted);
            std::vector<framegraph::ResourceRequirement> inputs;
            inputs.reserve(pass.inputs.size());
            for (const auto& semantic : pass.inputs) {
                inputs.push_back({semantic == framegraph::semantic::color_final ? input : semantic,
                                  framegraph::ResourceFormat::rgba8_uint, true});
            }
            descriptors.push_back({effect.plugin + "." + pass.id,
                                   framegraph::PassType::shader,
                                   std::move(inputs),
                                   {{output, framegraph::ResourceFormat::rgba8_uint}},
                                   framegraph::QueuePreference::compute,
                                   loaded.manifest.api});
            result.effects.push_back(
                {effect.plugin, pass.id, shader_path.string(),
                 EffectBackend::shader, effect.strength});
            input = output;
        }
    }

    if (!result.errors.empty()) return result;
    if (!descriptors.empty()) {
        descriptors.back().outputs.front().semantic =
            std::string(framegraph::semantic::output_color);
    }
    const auto compiled = framegraph::Compiler{}.compile(descriptors, external);
    if (!compiled.valid()) {
        result.errors = compiled.errors;
        return result;
    }
    result.plan = compiled.plan;
    if (descriptors.empty()) result.mode = PipelineMode::pass_through;
    else if (!has_neural) result.mode = PipelineMode::shader_only;
    else if (has_temporal) result.mode = PipelineMode::neural_temporal;
    else result.mode = PipelineMode::neural_spatial;
    return result;
}

const char* to_string(PipelineMode mode) noexcept {
    switch (mode) {
        case PipelineMode::pass_through: return "PASS_THROUGH";
        case PipelineMode::shader_only: return "SHADER_ONLY";
        case PipelineMode::neural_spatial: return "NEURAL_SPATIAL";
        case PipelineMode::neural_temporal: return "NEURAL_TEMPORAL";
    }
    return "PASS_THROUGH";
}

}  // namespace neuroshade::runtime
