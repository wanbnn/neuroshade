#include <tuple>
#include <cmath>
#include <stdexcept>
#include "profile/profile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>

namespace neuroshade::profile {
namespace {

[[nodiscard]] std::string escape(std::string_view value) {
    std::string result;
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view);
[[nodiscard]] std::optional<double> parse_double(std::string_view);
[[nodiscard]] std::string_view trim(std::string_view);
[[nodiscard]] std::string_view strip_quotes(std::string_view);
[[nodiscard]] std::size_t skip_value(std::string_view, std::size_t pos);
[[nodiscard]] std::optional<std::string_view> field_value(std::string_view, std::string_view);
void parse_resource_bindings(std::string_view, Effect&);
Effect parse_effect_v2(std::string_view);
void parse_pipeline_v2(std::string_view, std::vector<Effect>&);
void parse_output_extent(std::string_view, Profile&);

// Skip over the next JSON value starting at `pos`. Returns the position
// immediately after it (no input validation beyond quoting). Stricter than
// the v1 regex-only parser; enough to walk the schema v2 pipeline array and
// nested object literals.
std::size_t skip_value(std::string_view source, std::size_t pos) {
    if (pos >= source.size()) return pos;
    char quote = source[pos];
    if (quote == '"') {
        pos++;
        while (pos < source.size() && source[pos] != '"') {
            if (source[pos] == '\\' && pos + 1 < source.size()) pos += 2;
            else ++pos;
        }
        if (pos < source.size()) ++pos;  // skip closing quote
        return pos;
    }
    if (quote == '{' || quote == '[') {
        const char open = quote;
        const char close = open == '{' ? '}' : ']';
        int depth = 1;
        ++pos;
        while (pos < source.size() && depth > 0) {
            if (source[pos] == '"') {
                pos = skip_value(source, pos);
                continue;
            }
            if (source[pos] == open) ++depth;
            else if (source[pos] == close) --depth;
            ++pos;
        }
        return pos;
    }
    // Number / true / false / null
    while (pos < source.size() && source[pos] != ',' && source[pos] != '}' &&
           source[pos] != ']' && !std::isspace(static_cast<unsigned char>(source[pos]))) {
        ++pos;
    }
    return pos;
}

std::optional<std::string_view> field_value(std::string_view source, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    const std::size_t key_pos = source.find(quoted);
    if (key_pos == std::string_view::npos) return std::nullopt;
    std::size_t value_start = source.find(':', key_pos + quoted.size());
    if (value_start == std::string_view::npos) return std::nullopt;
    ++value_start;
    while (value_start < source.size() &&
           std::isspace(static_cast<unsigned char>(source[value_start]))) {
        ++value_start;
    }
    const std::size_t value_end = skip_value(source, value_start);
    if (value_end <= value_start) return std::nullopt;
    return source.substr(value_start, value_end - value_start);
}

[[nodiscard]] std::string_view trim(std::string_view view) {
    while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front()))) view.remove_prefix(1);
    while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back()))) view.remove_suffix(1);
    return view;
}

[[nodiscard]] std::string_view strip_quotes(std::string_view view) {
    view = trim(view);
    if (view.size() >= 2 && view.front() == '"' && view.back() == '"') {
        view.remove_prefix(1);
        view.remove_suffix(1);
    }
    return view;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view value) {
    value = trim(value);
    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view value) {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    std::string owned(value);
    char* end = nullptr;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (end == owned.c_str()) return std::nullopt;
    return parsed;
}

void parse_resource_bindings(std::string_view source, Effect& effect) {
    const auto array_value = field_value(source, "resource_bindings");
    if (!array_value || array_value->size() < 2 || array_value->front() != '[' ||
        array_value->back() != ']') return;
    const std::string_view body = array_value->substr(1, array_value->size() - 2);
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        const std::size_t object_start = body.find('{', cursor);
        if (object_start == std::string_view::npos) return;
        const std::size_t object_end = skip_value(body, object_start);
        const std::string_view obj = body.substr(object_start, object_end - object_start);
        ResourceBinding binding{};
        if (const auto value = field_value(obj, "semantic")) {
            binding.semantic = std::string(strip_quotes(*value));
        }
        if (const auto value = field_value(obj, "runtime_id"); value) {
            const auto d = parse_double(*value);
            if (d) binding.runtime_id = static_cast<std::uint64_t>(*d);
        }
        if (const auto value = field_value(obj, "format"); value) {
            const auto d = parse_double(*value);
            if (d) binding.format = static_cast<std::uint32_t>(*d);
        }
        if (const auto value = field_value(obj, "width"); value) {
            const auto d = parse_double(*value);
            if (d) binding.width_per_mille = static_cast<std::uint16_t>(*d);
        }
        if (const auto value = field_value(obj, "height"); value) {
            const auto d = parse_double(*value);
            if (d) binding.height_per_mille = static_cast<std::uint16_t>(*d);
        }
        effect.resource_bindings.push_back(std::move(binding));
        cursor = object_end;
    }
}

Effect parse_effect_v2(std::string_view obj) {
    obj = trim(obj);
    Effect effect;
    for (const std::string_view key : {std::string_view{"\"plugin\""},
                                      std::string_view{"\"enabled\""},
                                      std::string_view{"\"strength\""},
                                      std::string_view{"\"model\""}}) {
        const auto value_found = field_value(obj, key.substr(1, key.size() - 2));
        if (!value_found) continue;
        const std::string_view value = trim(*value_found);
        if (key == "\"plugin\"") {
            effect.plugin = std::string(strip_quotes(value));
        } else if (key == "\"enabled\"") {
            if (auto boolean = parse_bool(value)) effect.enabled = *boolean;
        } else if (key == "\"strength\"") {
            if (auto strength = parse_double(value)) effect.strength = static_cast<float>(*strength);
        } else if (key == "\"model\"") {
            effect.model = std::string(strip_quotes(value));
        }
    }
    const char* controls[]={"nr_local_tone","nr_local_structure","nr_skin_structure"};
    for(unsigned i=0;i<3;++i)if(const auto value=field_value(obj,controls[i])){
        const auto number=parse_double(*value);
        if(!number||!std::isfinite(*number)||*number<(i==2?-1:0)||*number>2)throw std::runtime_error("NR tone/structure must be in [0,2], skin in [-1,2]");
        effect.nr_controls[i]=static_cast<float>(*number);
    }
    if(const auto value=field_value(obj,"nr_auto_mask")){
        const auto flag=parse_bool(*value);if(!flag)throw std::runtime_error("nr_auto_mask must be boolean");
        effect.nr_auto_mask=*flag;
    }
    for(auto [name,target,limit]:{std::tuple{"nr_style",&effect.nr_style,2u},std::tuple{"nr_preset",&effect.nr_preset,3u}}){
        if(const auto value=field_value(obj,name)){
            const auto number=parse_double(*value);
            if(!number||!std::isfinite(*number)||*number<0||*number>limit||std::floor(*number)!=*number)throw std::runtime_error("invalid NR selection");
            *target=static_cast<unsigned>(*number);
        }
    }
    if(const auto value=field_value(obj,"nr_intensity")){
        const auto number=parse_double(*value);
        if(!number||!std::isfinite(*number)||*number<0||*number>2)throw std::runtime_error("NR intensity must be in [0,2]");
        effect.nr_intensity=static_cast<float>(*number);
    }
    parse_resource_bindings(obj, effect);
    return effect;
}

void parse_pipeline_v2(std::string_view source, std::vector<Effect>& effects) {
    const std::string key = "\"pipeline\"";
    const std::size_t key_pos = source.find(key);
    if (key_pos == std::string_view::npos) return;
    const std::size_t array_start = source.find('[', key_pos);
    if (array_start == std::string_view::npos) return;
    const std::size_t array_end_after = skip_value(source, array_start);
    const std::size_t array_end = array_end_after == 0 ? 0 : array_end_after - 1;
    if (array_end == std::string_view::npos) return;
    std::string_view body = source.substr(array_start + 1, array_end - array_start - 1);
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        const std::size_t object_start = body.find('{', cursor);
        if (object_start == std::string_view::npos) return;
        const std::size_t object_end = skip_value(body, object_start);
        effects.push_back(parse_effect_v2(body.substr(object_start, object_end - object_start)));
        cursor = object_end;
    }
}

void parse_output_extent(std::string_view source, Profile& profile) {
    const auto object = field_value(source, "output_extent");
    if (!object) return;
    if (const auto value = field_value(*object, "width")) {
        const auto width = parse_double(*value);
        if (width) profile.output_extent_width = static_cast<std::uint32_t>(*width);
    }
    if (const auto value = field_value(*object, "height")) {
        const auto height = parse_double(*value);
        if (height) profile.output_extent_height = static_cast<std::uint32_t>(*height);
    }
}

}  // namespace

LoadResult parse(std::string_view json) {
    LoadResult result;
    const std::string source(json);
    std::smatch match;
    const std::regex schema_re(R"("schema_version"\s*:\s*([0-9]+))");
    if (!std::regex_search(source, match, schema_re)) {
        result.errors.emplace_back("profile is missing schema_version");
    } else {
        result.profile.schema_version = static_cast<unsigned>(std::stoul(match[1].str()));
        if (result.profile.schema_version != 1 && result.profile.schema_version != 2) {
            result.errors.emplace_back("unsupported profile schema_version");
            return result;
        }
    }

    const std::regex executable_re(R"re("executable"\s*:\s*"([^"]*)")re");
    if (std::regex_search(source, match, executable_re))
        result.profile.executable = match[1].str();
    else
        result.errors.emplace_back("profile is missing executable");

    parse_output_extent(source, result.profile);

    if (result.profile.schema_version == 2) {
        try{parse_pipeline_v2(source, result.profile.pipeline);}
        catch(const std::exception& e){result.errors.push_back(e.what());}
    } else {
        const std::regex effect_re(
            R"re(\{\s*"plugin"\s*:\s*"([^"]+)"\s*,\s*"enabled"\s*:\s*(true|false)\s*,\s*"strength"\s*:\s*([-+]?[0-9]*\.?[0-9]+)\s*\})re");
        for (auto iterator = std::sregex_iterator(source.begin(), source.end(), effect_re);
             iterator != std::sregex_iterator(); ++iterator) {
            Effect effect;
            effect.plugin = (*iterator)[1].str();
            effect.enabled = (*iterator)[2].str() == "true";
            effect.strength = std::stof((*iterator)[3].str());
            result.profile.pipeline.push_back(std::move(effect));
        }
    }
    return result;
}

LoadResult load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return {{}, {"unable to open profile: " + path.string()}};
    std::ostringstream contents;
    contents << stream.rdbuf();
    return parse(contents.str());
}

bool save(const Profile& profile, const std::filesystem::path& path, std::string& error) {
    for(const auto& effect:profile.pipeline)for(unsigned i=0;i<3;++i){
        const float value=effect.nr_controls[i];
        if(!std::isfinite(value)||value<(i==2?-1:0)||value>2){error="NR tone/structure must be in [0,2], skin in [-1,2]";return false;}
    }
    for(const auto& effect:profile.pipeline)
        if(effect.nr_style>2||effect.nr_preset>3||!std::isfinite(effect.nr_intensity)||effect.nr_intensity<0||effect.nr_intensity>2){error="invalid NR selection or intensity";return false;}
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "unable to create profile directory: " + filesystem_error.message();
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) {
        error = "unable to create temporary profile";
        return false;
    }
    stream << "{\n  \"schema_version\": " << profile.schema_version
           << ",\n  \"game\": {\"executable\": \"" << escape(profile.executable) << "\"},\n";
    stream << "  \"output_extent\": {\"width\": " << profile.output_extent_width
           << ", \"height\": " << profile.output_extent_height << "},\n";
    stream << "  \"pipeline\": [\n";
    for (std::size_t index = 0; index < profile.pipeline.size(); ++index) {
        const auto& effect = profile.pipeline[index];
        stream << "    {\"plugin\": \"" << escape(effect.plugin) << "\", \"enabled\": "
               << (effect.enabled ? "true" : "false") << ", \"strength\": " << effect.strength;
        if (!effect.model.empty()) stream << ", \"model\": \"" << escape(effect.model) << "\"";
        if(!effect.model.empty())stream<<", \"nr_local_tone\": "<<effect.nr_controls[0]<<", \"nr_local_structure\": "<<effect.nr_controls[1]<<", \"nr_skin_structure\": "<<effect.nr_controls[2]<<", \"nr_auto_mask\": "<<(effect.nr_auto_mask?"true":"false")<<", \"nr_style\": "<<effect.nr_style<<", \"nr_preset\": "<<effect.nr_preset<<", \"nr_intensity\": "<<effect.nr_intensity;
        if (!effect.resource_bindings.empty()) {
            stream << ", \"resource_bindings\": [";
            for (std::size_t binding_index = 0; binding_index < effect.resource_bindings.size();
                 ++binding_index) {
                const auto& binding = effect.resource_bindings[binding_index];
                stream << "{\"semantic\": \"" << escape(binding.semantic)
                       << "\", \"runtime_id\": " << binding.runtime_id
                       << ", \"format\": " << binding.format
                       << ", \"width\": " << binding.width_per_mille
                       << ", \"height\": " << binding.height_per_mille << "}";
                if (binding_index + 1 != effect.resource_bindings.size()) stream << ", ";
            }
            stream << "]";
        }
        stream << "}" << (index + 1 == profile.pipeline.size() ? "\n" : ",\n");
    }
    stream << "  ]\n}\n";
    stream.close();
    if (!stream) {
        error = "unable to write profile";
        return false;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary);
        error = "unable to commit profile: " + filesystem_error.message();
        return false;
    }
    return true;
}

}  // namespace neuroshade::profile
