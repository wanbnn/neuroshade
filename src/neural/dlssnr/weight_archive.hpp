#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neuroshade::neural::dlssnr {
// Serialized containers, not decoded logical tensors. Their internal packing
// must be interpreted by the layer implementation, never by an fp16 cast.
struct WeightRecord {
    std::string name;
    std::size_t offset{};
    std::size_t bytes{};
    std::uint32_t device{};
    std::uint32_t field0{};
    std::uint32_t field1{};
    std::vector<std::uint32_t> shape;
};
class WeightArchive {
public:
    explicit WeightArchive(std::vector<std::byte> bytes);
    static WeightArchive load(const std::filesystem::path& path);
    [[nodiscard]] const std::vector<WeightRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::span<const std::byte> payload(std::string_view name) const;
    [[nodiscard]] std::span<const std::byte> serialized() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t payload_bytes() const noexcept;
private:
    std::vector<std::byte> bytes_;
    std::vector<WeightRecord> records_;
};
// Read the PE resource directory. Never load or execute the Windows DLL.
[[nodiscard]] WeightArchive load_dll_weights(const std::filesystem::path& dll);
} // namespace neuroshade::neural::dlssnr
