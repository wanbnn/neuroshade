#include "profile/profile.hpp"
#include "runtime/pipeline_plan.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected shader profile, temporal profile, and plugin root");

        const auto shader_profile = neuroshade::profile::load(argv[1]);
        require(shader_profile.valid(), "shader profile did not load");
        const auto shader = neuroshade::runtime::prepare_pipeline(shader_profile.profile, argv[3]);
        require(shader.valid(), "shader pipeline did not prepare");
        require(shader.mode == neuroshade::runtime::PipelineMode::shader_only,
                "shader mode was not selected");
        require(shader.plan.passes.size() == 2 && shader.effects.size() == 2,
                "disabled shader entry was not filtered");
        require(std::filesystem::is_regular_file(shader.effects.front().artifact),
                "shader artifact was not resolved");
        require(shader.plan.passes.back().outputs.front().semantic == "Output.Color",
                "final shader output was not canonical");

        const auto temporal_profile = neuroshade::profile::load(argv[2]);
        require(temporal_profile.valid(), "temporal profile did not load");
        const auto temporal = neuroshade::runtime::prepare_pipeline(temporal_profile.profile, argv[3]);
#ifdef NS_TEST_HAS_TEMPORAL_MODEL
        require(temporal.valid(), "temporal pipeline did not prepare");
        require(temporal.mode == neuroshade::runtime::PipelineMode::neural_temporal,
                "temporal mode was not selected");
        require(temporal.plan.passes.size() == 1 && temporal.effects.front().artifact.ends_with(".nsmodel"),
                "temporal model was not resolved");
#else
        require(!temporal.valid() && temporal.mode == neuroshade::runtime::PipelineMode::pass_through,
                "missing temporal model did not preserve pass-through");
#endif

        neuroshade::profile::Profile invalid;
        invalid.pipeline.push_back({"org.neuroshade.missing", true, 1.0F, {}, {}});
        const auto rejected = neuroshade::runtime::prepare_pipeline(invalid, argv[3]);
        require(!rejected.valid() && rejected.mode == neuroshade::runtime::PipelineMode::pass_through,
                "missing plugin did not preserve pass-through");

        std::cout << "pipeline_plan=pass shader_passes=" << shader.plan.passes.size()
                  << " temporal_passes=" << temporal.plan.passes.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipeline_plan=fail reason=" << error.what() << '\n';
        return 1;
    }
}
