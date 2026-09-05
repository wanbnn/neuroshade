#include "profile/profile.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verify(const neuroshade::profile::Profile& profile) {
    require(profile.schema_version == 2, "schema_version was not preserved");
    require(profile.executable == "v1-game-with-temporal-sr", "executable mismatch");
    require(profile.output_extent_width == 1920 && profile.output_extent_height == 1080,
            "output extent mismatch");
    require(profile.pipeline.size() == 1, "pipeline size mismatch");
    const auto& effect = profile.pipeline.front();
    require(effect.plugin == "org.neuroshade.bundled.temporal_sr_2x", "plugin mismatch");
    require(effect.model == "models/temporal_sr_2x.nsmodel", "model mismatch");
    require(std::abs(effect.strength - 1.0F) < 1.0e-6F, "strength mismatch");
    require(effect.resource_bindings.size() == 3, "resource binding count mismatch");
    require(effect.resource_bindings[0].semantic == "Color.LowRes", "low-res binding missing");
    require(effect.resource_bindings[0].width_per_mille == 500 &&
                effect.resource_bindings[0].height_per_mille == 500,
            "low-res fingerprint mismatch");
    require(effect.resource_bindings[1].semantic == "Motion.Screen", "motion binding missing");
    require(effect.resource_bindings[2].semantic == "History.Output.Color",
            "history binding missing");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: ns-profile-tests <input> <roundtrip>");
        const auto source = neuroshade::profile::load(argv[1]);
        require(source.valid(), "schema-v2 example did not parse");
        verify(source.profile);

        auto roundtrip_source = source.profile;
        roundtrip_source.pipeline.front().enabled = false;
        roundtrip_source.pipeline.front().resource_bindings[0].runtime_id = 0x12345678ULL;
        std::string error;
        require(neuroshade::profile::save(roundtrip_source, argv[2], error), error.c_str());
        const auto loaded = neuroshade::profile::load(argv[2]);
        std::filesystem::remove(argv[2]);
        require(loaded.valid(), "saved schema-v2 profile did not parse");
        verify(loaded.profile);
        require(!loaded.profile.pipeline.front().enabled, "enabled=false was not preserved");
        require(loaded.profile.pipeline.front().resource_bindings[0].runtime_id == 0x12345678ULL,
                "64-bit runtime ID was not preserved");
        std::cout << "schema_v2_roundtrip=pass bindings=3 output_extent=1920x1080\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ns-profile-tests: " << error.what() << '\n';
        return 1;
    }
}
