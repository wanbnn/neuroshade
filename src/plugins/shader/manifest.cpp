#include "plugins/shader/manifest.hpp"

#include "framegraph/compiler/compiler.hpp"

#include <charconv>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

namespace neuroshade::plugins {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

[[nodiscard]] std::optional<std::string> quoted(std::string_view value) {
    value = trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::nullopt;
    return std::string(value.substr(1, value.size() - 2));
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_value(std::string_view value) {
    value = trim(value);
    std::uint32_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<std::vector<std::string>> string_array(std::string_view value) {
    value = trim(value);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return std::nullopt;
    value.remove_prefix(1);
    value.remove_suffix(1);
    std::vector<std::string> output;
    while (!trim(value).empty()) {
        value = trim(value);
        if (value.front() != '"') return std::nullopt;
        const auto end = value.find('"', 1);
        if (end == std::string_view::npos) return std::nullopt;
        output.emplace_back(value.substr(1, end - 1));
        value.remove_prefix(end + 1);
        value = trim(value);
        if (value.empty()) break;
        if (value.front() != ',') return std::nullopt;
        value.remove_prefix(1);
    }
    return output;
}

}  // namespace

ManifestResult parse_shader_manifest(std::string_view source) {
    ManifestResult result;
    ShaderPassManifest* current_pass = nullptr;
    std::size_t line_number = 0;

    while (!source.empty()) {
        ++line_number;
        const auto newline = source.find('\n');
        std::string_view line = trim(source.substr(0, newline));
        if (newline == std::string_view::npos) source = {};
        else source.remove_prefix(newline + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line == "[[passes]]") {
            result.manifest.passes.emplace_back();
            current_pass = &result.manifest.passes.back();
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            result.errors.emplace_back("line " + std::to_string(line_number) + ": expected key = value");
            continue;
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));

        auto assign_string = [&](std::string& target) {
            const auto parsed = quoted(value);
            if (parsed) target = *parsed;
            else result.errors.emplace_back("line " + std::to_string(line_number) + ": expected quoted string");
        };
        auto assign_number = [&](std::uint32_t& target) {
            const auto parsed = unsigned_value(value);
            if (parsed) target = *parsed;
            else result.errors.emplace_back("line " + std::to_string(line_number) + ": expected unsigned integer");
        };

        if (current_pass != nullptr) {
            if (key == "id") assign_string(current_pass->id);
            else if (key == "backend") assign_string(current_pass->backend);
            else if (key == "shader") assign_string(current_pass->shader);
            else if (key == "output") assign_string(current_pass->output);
            else if (key == "inputs") {
                const auto parsed = string_array(value);
                if (parsed) current_pass->inputs = *parsed;
                else result.errors.emplace_back("line " + std::to_string(line_number) + ": invalid string array");
            } else {
                // Unknown future pass fields are intentionally ignored.
            }
        } else if (key == "schema") assign_number(result.manifest.schema);
        else if (key == "id") assign_string(result.manifest.id);
        else if (key == "name") assign_string(result.manifest.name);
        else if (key == "version") assign_string(result.manifest.version);
        else if (key == "api") assign_number(result.manifest.api);
    }

    if (result.manifest.schema != 1) result.errors.emplace_back("unsupported manifest schema");
    if (result.manifest.api != framegraph::kPluginApiVersion) result.errors.emplace_back("incompatible plugin API");
    if (result.manifest.id.empty()) result.errors.emplace_back("plugin id is required");
    if (result.manifest.name.empty()) result.errors.emplace_back("plugin name is required");
    if (result.manifest.version.empty()) result.errors.emplace_back("plugin version is required");
    if (result.manifest.passes.empty()) result.errors.emplace_back("at least one shader pass is required");
    for (const auto& pass : result.manifest.passes) {
        if (pass.id.empty() || pass.backend != "shader" || pass.shader.empty() || pass.inputs.empty() || pass.output.empty()) {
            result.errors.emplace_back("shader pass requires id, shader, inputs, and output");
        }
    }
    return result;
}

ManifestResult load_shader_manifest(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return {{}, {"unable to open plugin manifest: " + path.string()}};
    std::ostringstream contents;
    contents << stream.rdbuf();
    return parse_shader_manifest(contents.str());
}

}  // namespace neuroshade::plugins
