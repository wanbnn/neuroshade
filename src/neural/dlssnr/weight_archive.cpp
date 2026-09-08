#include "neural/dlssnr/weight_archive.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace neuroshade::neural::dlssnr {
namespace {
constexpr std::size_t max_file_bytes = 512u * 1024u * 1024u;
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::span<const std::byte> slice(std::size_t offset, std::size_t count) const {
        if (offset > bytes_.size() || count > bytes_.size() - offset)
            throw std::runtime_error("DLSSNR: truncated archive or invalid offset");
        return bytes_.subspan(offset, count);
    }
    std::uint64_t integer(std::size_t offset, std::size_t count) const {
        const auto data = slice(offset, count);
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < count; ++i)
            result |= std::uint64_t(std::to_integer<unsigned>(data[i])) << (8 * i);
        return result;
    }
    std::size_t size(std::size_t offset) const {
        const auto value = integer(offset, 8);
        if (value > max_file_bytes) throw std::runtime_error("DLSSNR: oversized field");
        return static_cast<std::size_t>(value);
    }
private:
    std::span<const std::byte> bytes_;
};
std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("DLSSNR: cannot open " + path.string());
    const auto end = file.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_file_bytes)
        throw std::runtime_error("DLSSNR: invalid file size");
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size())))
        throw std::runtime_error("DLSSNR: failed reading file");
    return result;
}
} // namespace

WeightArchive::WeightArchive(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {
    Reader reader(bytes_);
    if (reader.size(0) != bytes_.size()) throw std::runtime_error("DLSSNR: map size mismatch");
    std::size_t offset = 8;
    std::set<std::string> names;
    while (offset < bytes_.size()) {
        if (records_.size() >= 65536) throw std::runtime_error("DLSSNR: too many weights");
        const auto length = reader.size(offset);
        offset += 8;
        if (!length || length > 1024) throw std::runtime_error("DLSSNR: invalid weight name length");
        const auto name = reader.slice(offset, length);
        WeightRecord weight;
        for (auto character : name) {
            const auto c = std::to_integer<unsigned char>(character);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                throw std::runtime_error("DLSSNR: invalid weight name");
            weight.name.push_back(static_cast<char>(c));
        }
        if (!names.insert(weight.name).second) throw std::runtime_error("DLSSNR: duplicate weight");
        offset += length;
        const auto length_record = reader.size(offset);
        offset += 8;
        Reader record(reader.slice(offset, length_record));
        if (length_record < 36 || record.size(0) != length_record)
            throw std::runtime_error("DLSSNR: record size mismatch");
        weight.bytes = record.size(8);
        weight.device = static_cast<std::uint32_t>(record.integer(16, 4));
        if (!weight.bytes || weight.device > 1) throw std::runtime_error("DLSSNR: invalid payload/device");
        (void)record.slice(20, weight.bytes);
        weight.offset = offset + 20;
        const auto tail = 20 + weight.bytes;
        weight.field0 = static_cast<std::uint32_t>(record.integer(tail, 4));
        weight.field1 = static_cast<std::uint32_t>(record.integer(tail + 4, 4));
        const auto rank = record.size(tail + 8);
        if (rank > 32 || tail + 16 + rank * 4 != length_record)
            throw std::runtime_error("DLSSNR: invalid shape framing");
        for (std::size_t i = 0; i < rank; ++i) {
            const auto dim = record.integer(tail + 16 + i * 4, 4);
            if (dim > std::numeric_limits<std::int32_t>::max())
                throw std::runtime_error("DLSSNR: negative dimension");
            weight.shape.push_back(static_cast<std::uint32_t>(dim));
        }
        records_.push_back(std::move(weight));
        offset += length_record;
    }
    if (records_.empty()) throw std::runtime_error("DLSSNR: empty weight map");
}
WeightArchive WeightArchive::load(const std::filesystem::path& path) {
    return WeightArchive(read_file(path));
}
std::span<const std::byte> WeightArchive::payload(std::string_view name) const {
    const auto found = std::find_if(records_.begin(), records_.end(),
        [name](const auto& record) { return record.name == name; });
    if (found == records_.end()) throw std::runtime_error("DLSSNR: missing weight " + std::string(name));
    return std::span<const std::byte>(bytes_).subspan(found->offset, found->bytes);
}
std::size_t WeightArchive::payload_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& record : records_) total += record.bytes;
    return total;
}
WeightArchive load_dll_weights(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    Reader reader(bytes);
    if (reader.integer(0, 2) != 0x5a4d) throw std::runtime_error("DLSSNR: expected PE DLL");
    const auto pe = static_cast<std::size_t>(reader.integer(0x3c, 4));
    (void)reader.slice(pe, 24);
    if (reader.integer(pe, 4) != 0x4550 || reader.integer(pe + 4, 2) != 0x8664)
        throw std::runtime_error("DLSSNR: expected x64 PE DLL");
    const auto opt = pe + 24;
    const auto opt_size = static_cast<std::size_t>(reader.integer(pe + 20, 2));
    if (opt_size < 136 || reader.integer(opt, 2) != 0x20b || reader.integer(opt + 108, 4) < 3)
        throw std::runtime_error("DLSSNR: missing PE32+ resource directory");
    (void)reader.slice(opt, opt_size);
    const auto count = reader.integer(pe + 6, 2);
    if (!count || count > 96) throw std::runtime_error("DLSSNR: invalid section count");
    auto raw_offset = [&](std::uint64_t rva, std::size_t size) -> std::size_t {
        for (std::size_t i = 0; i < count; ++i) {
            const auto section = opt + opt_size + i * 40;
            const auto va = reader.integer(section + 12, 4);
            const auto raw_size = reader.integer(section + 16, 4);
            if (rva >= va && rva - va < raw_size && size <= raw_size - (rva - va)) {
                const auto raw = reader.integer(section + 20, 4) + rva - va;
                if (raw > bytes.size()) break;
                (void)reader.slice(static_cast<std::size_t>(raw), size);
                return static_cast<std::size_t>(raw);
            }
        }
        throw std::runtime_error("DLSSNR: unmapped resource RVA");
    };
    const auto resource_size = static_cast<std::size_t>(reader.integer(opt + 132, 4));
    const auto resource_base = raw_offset(reader.integer(opt + 128, 4), resource_size);
    Reader resource(reader.slice(resource_base, resource_size));
    std::size_t dir = 0;
    // Exactly three resource levels: RCDATA -> WEIGHTS_HT -> language.
    for (unsigned depth = 0; depth < 3; ++depth) {
        const auto n = resource.integer(dir + 12, 2) + resource.integer(dir + 14, 2);
        bool found = false;
        for (std::size_t i = 0; i < n; ++i) {
            const auto entry = dir + 16 + i * 8;
            const auto id = resource.integer(entry, 4);
            const auto child = resource.integer(entry + 4, 4);
            bool match = depth == 0 ? id == 10 : depth == 2;
            if (depth == 1 && (id & 0x80000000)) {
                const auto name = static_cast<std::size_t>(id & 0x7fffffff);
                constexpr std::string_view wanted = "WEIGHTS_HT";
                match = resource.integer(name, 2) == wanted.size();
                for (std::size_t k = 0; match && k < wanted.size(); ++k)
                    match = resource.integer(name + 2 + k * 2, 2) == static_cast<unsigned char>(wanted[k]);
            }
            if (!match) continue;
            if (depth < 2) {
                if (!(child & 0x80000000)) throw std::runtime_error("DLSSNR: expected resource directory");
                dir = static_cast<std::size_t>(child & 0x7fffffff);
                found = true;
                break;
            }
            if (child & 0x80000000) throw std::runtime_error("DLSSNR: expected resource data");
            const auto data = static_cast<std::size_t>(child);
            const auto size = static_cast<std::size_t>(resource.integer(data + 4, 4));
            const auto offset = raw_offset(resource.integer(data, 4), size);
            const auto payload = reader.slice(offset, size);
            return WeightArchive(std::vector<std::byte>(payload.begin(), payload.end()));
        }
        if (!found) throw std::runtime_error("DLSSNR: WEIGHTS_HT resource not found");
    }
    throw std::runtime_error("DLSSNR: WEIGHTS_HT resource not found");
}
} // namespace neuroshade::neural::dlssnr
