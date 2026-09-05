#include "neural/model/package.hpp"

#include <charconv>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace neuroshade::neural {
namespace {

struct Json;
using Object = std::map<std::string, Json>;
using Array = std::vector<Json>;

struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : remaining_(source) {}

    Json parse() {
        Json result = value();
        whitespace();
        if (!remaining_.empty()) fail("trailing JSON content");
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(offset_));
    }

    void whitespace() {
        while (!remaining_.empty() &&
               (remaining_.front() == ' ' || remaining_.front() == '\n' ||
                remaining_.front() == '\r' || remaining_.front() == '\t')) {
            remaining_.remove_prefix(1);
            ++offset_;
        }
    }

    char take() {
        if (remaining_.empty()) fail("unexpected end of JSON");
        const char result = remaining_.front();
        remaining_.remove_prefix(1);
        ++offset_;
        return result;
    }

    bool consume(std::string_view text) {
        if (!remaining_.starts_with(text)) return false;
        remaining_.remove_prefix(text.size());
        offset_ += text.size();
        return true;
    }

    Json value() {
        whitespace();
        if (remaining_.empty()) fail("expected JSON value");
        if (remaining_.front() == '{') return Json{object()};
        if (remaining_.front() == '[') return Json{array()};
        if (remaining_.front() == '"') return Json{string()};
        if (consume("true")) return Json{true};
        if (consume("false")) return Json{false};
        if (consume("null")) return Json{nullptr};
        return Json{number()};
    }

    std::string string() {
        if (take() != '"') fail("expected string");
        std::string result;
        while (true) {
            const char character = take();
            if (character == '"') break;
            if (character == '\\') {
                const char escaped = take();
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    default: fail("unsupported JSON escape");
                }
            } else {
                if (static_cast<unsigned char>(character) < 0x20) fail("control byte in JSON string");
                result.push_back(character);
            }
        }
        return result;
    }

    double number() {
        const char* begin = remaining_.data();
        char* end = nullptr;
        const double result = std::strtod(begin, &end);
        if (end == begin) fail("invalid JSON number");
        const auto consumed = static_cast<std::size_t>(end - begin);
        remaining_.remove_prefix(consumed);
        offset_ += consumed;
        return result;
    }

    Object object() {
        take();
        Object result;
        whitespace();
        if (!remaining_.empty() && remaining_.front() == '}') {
            take();
            return result;
        }
        while (true) {
            whitespace();
            if (remaining_.empty() || remaining_.front() != '"') fail("expected object key");
            auto key = string();
            whitespace();
            if (take() != ':') fail("expected ':'");
            if (!result.emplace(std::move(key), value()).second) fail("duplicate object key");
            whitespace();
            const char delimiter = take();
            if (delimiter == '}') break;
            if (delimiter != ',') fail("expected ',' or '}'");
        }
        return result;
    }

    Array array() {
        take();
        Array result;
        whitespace();
        if (!remaining_.empty() && remaining_.front() == ']') {
            take();
            return result;
        }
        while (true) {
            result.push_back(value());
            whitespace();
            const char delimiter = take();
            if (delimiter == ']') break;
            if (delimiter != ',') fail("expected ',' or ']'");
        }
        return result;
    }

    std::string_view remaining_;
    std::size_t offset_{};
};

const Object& object(const Json& value, std::string_view field) {
    if (const auto* result = std::get_if<Object>(&value.value)) return *result;
    throw std::runtime_error(std::string(field) + " must be an object");
}

const Array& array(const Json& value, std::string_view field) {
    if (const auto* result = std::get_if<Array>(&value.value)) return *result;
    throw std::runtime_error(std::string(field) + " must be an array");
}

const Json& required(const Object& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    if (found == value.end()) throw std::runtime_error("missing field: " + std::string(key));
    return found->second;
}

std::string text(const Object& value, std::string_view key) {
    const auto& field = required(value, key);
    if (const auto* result = std::get_if<std::string>(&field.value)) return *result;
    throw std::runtime_error(std::string(key) + " must be a string");
}

std::string text_or(const Object& value, std::string_view key, std::string fallback) {
    const auto found = value.find(std::string(key));
    if (found == value.end()) return fallback;
    if (const auto* result = std::get_if<std::string>(&found->second.value)) return *result;
    throw std::runtime_error(std::string(key) + " must be a string");
}

double number(const Object& value, std::string_view key) {
    const auto& field = required(value, key);
    if (const auto* result = std::get_if<double>(&field.value)) return *result;
    throw std::runtime_error(std::string(key) + " must be a number");
}

bool boolean_or(const Object& value, std::string_view key, bool fallback) {
    const auto found = value.find(std::string(key));
    if (found == value.end()) return fallback;
    if (const auto* result = std::get_if<bool>(&found->second.value)) return *result;
    throw std::runtime_error(std::string(key) + " must be a boolean");
}

std::uint32_t unsigned_number(const Object& value, std::string_view key) {
    const double parsed = number(value, key);
    if (parsed < 0 || parsed > UINT32_MAX || std::floor(parsed) != parsed) {
        throw std::runtime_error(std::string(key) + " must be an unsigned integer");
    }
    return static_cast<std::uint32_t>(parsed);
}

TensorBinding tensor(const Object& value) {
    TensorBinding result{text(value, "semantic"), text(value, "tensor"),
                         text(value, "dtype"), text(value, "layout"),
                         boolean_or(value, "optional", false)};
    if (result.dtype != "fp16" && result.dtype != "fp32") {
        throw std::runtime_error("tensor dtype must be fp16 or fp32");
    }
    if (result.layout != "NCHW" && result.layout != "NHWC") {
        throw std::runtime_error("tensor layout must be NCHW or NHWC");
    }
    return result;
}

std::vector<std::size_t> dimensions(const Json& value, std::string_view name) {
    std::vector<std::size_t> result;
    for (const auto& dimension : array(value, name)) {
        const auto* parsed = std::get_if<double>(&dimension.value);
        if (parsed == nullptr || *parsed <= 0 || std::floor(*parsed) != *parsed) {
            throw std::runtime_error(std::string(name) + " dimensions must be positive integers");
        }
        result.push_back(static_cast<std::size_t>(*parsed));
    }
    if (result.empty()) throw std::runtime_error(std::string(name) + " dimensions are empty");
    return result;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open " + path.string());
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string stable_hash(const std::vector<std::string>& contents) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& content : contents) {
        for (const unsigned char byte : content) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream output;
    output << std::hex << hash;
    return output.str();
}

} // namespace

PackageResult load_model_package(const std::filesystem::path& path) {
    PackageResult result;
    result.package.root = path;
    result.package.onnx_path = path / "model.onnx";
    try {
        if (!std::filesystem::is_directory(path) || path.extension() != ".nsmodel") {
            throw std::runtime_error(".nsmodel package must be a directory with .nsmodel extension");
        }
        constexpr std::array required_files{"manifest.json", "model.onnx", "signature.json",
                                             "metadata.json", "preview.webp"};
        std::vector<std::string> contents;
        contents.reserve(required_files.size());
        for (const auto* filename : required_files) contents.push_back(read_file(path / filename));
        if (contents[1].empty()) throw std::runtime_error("model.onnx cannot be empty");
        const auto signature = object(JsonParser(contents[2]).parse(), "signature");
        const auto metadata = object(JsonParser(contents[3]).parse(), "metadata");
        if (unsigned_number(signature, "schema") != 1 || unsigned_number(metadata, "schema") != 1) {
            throw std::runtime_error("unsupported signature or metadata schema");
        }

        const auto root = object(JsonParser(contents[0]).parse(), "manifest");
        auto& manifest = result.package.manifest;
        manifest.schema = unsigned_number(root, "schema");
        manifest.id = text(root, "id");
        manifest.name = text(root, "name");
        manifest.version = text(root, "version");
        manifest.runtime = text(root, "runtime");
        for (const auto& input : array(required(root, "inputs"), "inputs")) {
            manifest.inputs.push_back(tensor(object(input, "input")));
        }
        manifest.output = tensor(object(required(root, "output"), "output"));
        manifest.history = unsigned_number(root, "history");
        const auto& scale = object(required(root, "scale"), "scale");
        manifest.scale_x = static_cast<float>(number(scale, "x"));
        manifest.scale_y = static_cast<float>(number(scale, "y"));
        manifest.first_frame = text(root, "first_frame");
        manifest.missing_motion = text_or(root, "missing_motion", "reject");
        const auto& shapes = object(required(root, "shapes"), "shapes");
        const auto kind = text(shapes, "kind");
        if (kind == "fixed") manifest.shape_kind = ShapeKind::fixed;
        else if (kind == "dynamic") manifest.shape_kind = ShapeKind::dynamic;
        else if (kind == "bucketed") manifest.shape_kind = ShapeKind::bucketed;
        else throw std::runtime_error("shapes.kind must be fixed, dynamic, or bucketed");
        for (const auto& bucket : array(required(shapes, "buckets"), "buckets")) {
            const auto& entry = object(bucket, "bucket");
            manifest.buckets.push_back({dimensions(required(entry, "input"), "input"),
                                        dimensions(required(entry, "output"), "output")});
        }

        if (manifest.schema != 1) throw std::runtime_error("unsupported model schema");
        if (manifest.runtime != "migraphx" && manifest.runtime != "pytorch" && manifest.runtime != "onnxruntime")
            throw std::runtime_error("unsupported neural runtime");
        if (manifest.runtime == "pytorch") {
            const auto weights = std::filesystem::exists(path / "model.safetensors") ? path / "model.safetensors" : path / "model.pth";
            contents.push_back(read_file(weights));
            if (contents.back().empty()) throw std::runtime_error("PyTorch weights cannot be empty");
        }
        if (manifest.id.empty() || manifest.name.empty() || manifest.version.empty()) {
            throw std::runtime_error("model identity fields cannot be empty");
        }
        if (manifest.inputs.empty() || manifest.output.tensor.empty() || manifest.buckets.empty()) {
            throw std::runtime_error("model tensors and shape buckets are required");
        }
        if (manifest.missing_motion != "reject" && manifest.missing_motion != "copy" &&
            manifest.missing_motion != "spatial") {
            throw std::runtime_error("missing_motion must be reject, copy, or spatial");
        }
        result.package.content_hash = stable_hash(contents);
    } catch (const std::exception& error) {
        result.errors.push_back(error.what());
    }
    return result;
}

} // namespace neuroshade::neural
