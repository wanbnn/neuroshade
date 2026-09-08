#pragma once
#include <cstdint>
#include <array>
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
    bool configure_nr(const std::array<float,3>& controls,bool automatic_mask=true,unsigned style=0,unsigned preset=0,float intensity=1.f) noexcept;
    bool supports_nr() const noexcept {return description_.find("\"nr_controls\":true")!=std::string::npos;}
    bool supports_shared() const noexcept {return description_.find("\"shared_buffers\":true")!=std::string::npos;}
    int import_frame(int fd,std::uint64_t allocation,std::uint64_t bytes,const unsigned char* uuid);
    bool process_shared(unsigned slot,std::uint32_t width,std::uint32_t height,bool bgra,float strength) noexcept;
    double average_ms() const noexcept {return average_ms_;}
    const std::string& description() const noexcept {return description_;}
    const std::string& error() const noexcept {return error_;}
private:
    int socket_{-1};
    double average_ms_{};
    std::uint64_t frame_count_{};
    std::string description_,error_;
};
}
