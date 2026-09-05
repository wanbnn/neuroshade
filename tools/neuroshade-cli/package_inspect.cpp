#include "neural/model/package.hpp"
#include "plugins/shader/manifest.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("usage: ns-package-inspect plugin|model <directory>");
        }
        const std::string_view kind(argv[1]);
        if (kind == "plugin") {
            const auto result = neuroshade::plugins::load_shader_manifest(
                std::filesystem::path(argv[2]) / "manifest.toml");
            if (!result.valid()) throw std::runtime_error(result.errors.front());
            std::cout << "package=plugin status=valid id=" << result.manifest.id
                      << " version=" << result.manifest.version << '\n';
            return 0;
        }
        if (kind == "model") {
            const auto result = neuroshade::neural::load_model_package(argv[2]);
            if (!result.valid()) throw std::runtime_error(result.errors.front());
            std::cout << "package=model status=valid id=" << result.package.manifest.id
                      << " content_hash=" << result.package.content_hash << '\n';
            return 0;
        }
        throw std::invalid_argument("package kind must be plugin or model");
    } catch (const std::exception& error) {
        std::cerr << "ns-package-inspect: " << error.what() << '\n';
        return 1;
    }
}
