#include "framegraph/compiler/compiler.hpp"
#include "plugins/shader/manifest.hpp"
#include "profile/profile.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fg = neuroshade::framegraph;

int main() {
    const fg::ResourceFormat format = fg::ResourceFormat::rgba8_uint;
    const std::vector<fg::PassDescriptor> passes{
        {"plugin.sharpen", fg::PassType::shader, {{"Color.Final", format, true}},
         {{"Work.Sharpen", format}}, fg::QueuePreference::compute, 1},
        {"plugin.color", fg::PassType::shader, {{"Work.Sharpen", format, true}},
         {{"Output.Color", format}}, fg::QueuePreference::compute, 1},
    };
    const fg::Compiler compiler;
    const auto plan = compiler.compile(passes, {{"Color.Final", format}});
    if (!plan.valid() || plan.plan.passes.size() != 2 || plan.plan.passes[0].id != "plugin.sharpen" ||
        plan.plan.cache_key == 0) {
        std::cerr << "valid graph did not compile\n";
        return 1;
    }
    auto invalid = passes;
    invalid[1].inputs[0].semantic = "Missing.Resource";
    if (compiler.compile(invalid, {{"Color.Final", format}}).valid()) {
        std::cerr << "missing resource was accepted\n";
        return 1;
    }
    invalid = passes;
    invalid[1].outputs[0].semantic = "Work.Sharpen";
    if (compiler.compile(invalid, {{"Color.Final", format}}).valid()) {
        std::cerr << "multiple writers were accepted\n";
        return 1;
    }
    const std::vector<fg::PassDescriptor> cycle{
        {"a", fg::PassType::shader, {{"B", format, true}}, {{"A", format}}, fg::QueuePreference::compute, 1},
        {"b", fg::PassType::shader, {{"A", format, true}}, {{"B", format}}, fg::QueuePreference::compute, 1},
    };
    if (compiler.compile(cycle, {}).valid()) {
        std::cerr << "cycle was accepted\n";
        return 1;
    }

    const auto manifest = neuroshade::plugins::parse_shader_manifest(R"(
schema = 1
id = "org.neuroshade.test"
name = "Test"
version = "1.0.0"
api = 1
[[passes]]
id = "main"
backend = "shader"
shader = "main.spv"
inputs = ["Color.Final"]
output = "Output.Color"
)");
    if (!manifest.valid() || manifest.manifest.passes.size() != 1) {
        std::cerr << "valid manifest was rejected\n";
        return 1;
    }
    if (neuroshade::plugins::parse_shader_manifest("schema = 1\napi = 99\n").valid()) {
        std::cerr << "invalid manifest was accepted\n";
        return 1;
    }

    const auto profile_path = std::filesystem::temp_directory_path() / "neuroshade-profile-test.json";
    const neuroshade::profile::Profile source{1, "test-game", {{"sharpen", true, 0.75F}, {"color", false, 0.5F}}};
    std::string error;
    if (!neuroshade::profile::save(source, profile_path, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto loaded = neuroshade::profile::load(profile_path);
    std::filesystem::remove(profile_path);
    if (!loaded.valid() || loaded.profile.executable != source.executable || loaded.profile.pipeline.size() != 2 ||
        !loaded.profile.pipeline[0].enabled || loaded.profile.pipeline[1].enabled) {
        std::cerr << "profile round trip failed\n";
        return 1;
    }
    return 0;
}
