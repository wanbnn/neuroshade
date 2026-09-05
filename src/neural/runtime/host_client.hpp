#pragma once
#include <cstdint>
#include <string>
#include <span>
namespace neuroshade::neural {
class HostClient {
public:
    explicit HostClient(const std::string& model,std::uint32_t width,std::uint32_t height);
    ~HostClient();
    HostClient(const HostClient&)=delete;
    HostClient& operator=(const HostClient&)=delete;
    bool process(std::span<std::uint8_t> pixels,std::uint32_t width,std::uint32_t height,bool bgra,float strength) noexcept;
    double average_ms() const noexcept {return average_ms_;}
    const std::string& description() const noexcept {return description_;}
    const std::string& error() const noexcept {return error_;}
private:
    int socket_{-1};
    double average_ms_{};
    std::string description_,error_;
};
}
