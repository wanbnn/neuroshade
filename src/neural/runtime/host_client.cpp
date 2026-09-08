#include "neural/runtime/host_client.hpp"
#include <sys/socket.h>
#include <chrono>
#include "logging/log.hpp"
#include <sys/un.h>
#include <unistd.h>
#include <endian.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cmath>
namespace neuroshade::neural {
namespace {
constexpr std::uint32_t magic=0x3152534e, limit=4096u*4096u*4u;
using Header=std::array<std::uint32_t,8>;
void send_all(int fd,const void* bytes,std::size_t size) {
    auto* p=static_cast<const char*>(bytes);
    while(size) {auto n=send(fd,p,size,MSG_NOSIGNAL);if(n<0&&errno==EINTR)continue;
        if(n<=0)throw std::runtime_error("host64 send failed");
        p+=n;size-=n;}
}
void receive_all(int fd,void* bytes,std::size_t size) {
    auto* p=static_cast<char*>(bytes);
    while(size) {auto n=recv(fd,p,size,0);if(n<0&&errno==EINTR)continue;
        if(n<=0)throw std::runtime_error("host64 disconnected or timed out");
        p+=n;size-=n;}
}
std::vector<std::uint8_t> exchange(int fd,std::uint32_t op,const void* bytes,std::uint32_t size,
                                  std::uint32_t w,std::uint32_t h,bool bgra,double& ms) {
    Header header={magic,1,op,w,h,bgra?1u:0u,size,0};
    for(auto& n:header)n=htole32(n);
    send_all(fd,header.data(),sizeof(header));send_all(fd,bytes,size);
    receive_all(fd,header.data(),sizeof(header));for(auto& n:header)n=le32toh(n);
    if(header[0]!=magic || header[1]!=1 || header[6]>limit || (header[2] && header[6]>4096))
        throw std::runtime_error("invalid host64 response");
    if(!header[2] && op==2 && (header[3]!=w || header[4]!=h || header[5]!=(bgra?1u:0u) || header[6]!=size))
        throw std::runtime_error("host64 output extent mismatch");
    if(op==1 && header[6]>4096)throw std::runtime_error("host64 metadata too large");
    std::vector<std::uint8_t> result(header[6]);receive_all(fd,result.data(),result.size());
    if(header[2])throw std::runtime_error(std::string(result.begin(),result.end()));
    ms=header[7]/1000.0;return result;
}
}
HostClient::HostClient(const std::string& model,std::uint32_t width,std::uint32_t height) {
    if(model.empty() || model.size()>4096)throw std::runtime_error("invalid host64 model path");
    const char* home=std::getenv("HOME");
    std::string path=home?std::string(home)+"/.local/state/neuroshade/runtime.sock":"";
    if(const char* override_path=std::getenv("NEUROSHADE_RUNTIME_SOCKET"))path=override_path;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(path.empty() || path.size()>=sizeof(address.sun_path))throw std::runtime_error("invalid runtime socket path");
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    socket_=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(socket_<0)throw std::runtime_error("cannot create runtime socket");
    try {
        timeval timeout{60,0};setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(socket_,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        if(connect(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0)
            throw std::runtime_error("host64 unavailable; run neuroshade-runtime start");
        double ms{};auto info=exchange(socket_,1,model.data(),static_cast<std::uint32_t>(model.size()),width,height,false,ms);
        description_=std::string(info.begin(),info.end());
        timeout={2,0};setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(socket_,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    } catch(...) {close(socket_);socket_=-1;throw;}
}
bool HostClient::configure_nr(const std::array<float,3>& controls,bool automatic_mask,unsigned style,unsigned preset,float intensity) noexcept {
    try{
        if(!supports_nr())throw std::runtime_error("host does not support NR controls");
        for(unsigned i=0;i<3;++i)if(!std::isfinite(controls[i])||controls[i]<(i==2?-1:0)||controls[i]>2)throw std::runtime_error("NR controls outside supported ranges");
        if(style>2||preset>3||!std::isfinite(intensity)||intensity<0||intensity>2)throw std::runtime_error("invalid NR selection or intensity");
        const bool v3=description_.find("\"nr_controls_v3\":true")!=std::string::npos;
        if(!v3&&(style||preset||intensity!=1.f))throw std::runtime_error("restart the native host to use NR Style, Preset and Intensity");
        const bool extended=description_.find("\"nr_controls_v2\":true")!=std::string::npos;
        if(!extended&&(!automatic_mask||controls[0]>1||controls[1]>1||controls[2]<0||controls[2]>1))throw std::runtime_error("restart the native host to use extended NR controls");
        const std::array<float,7> payload{controls[0],controls[1],controls[2],automatic_mask?1.f:0.f,float(style),float(preset),intensity};
        double ms{};auto result=exchange(socket_,v3?8:extended?7:6,payload.data(),v3?sizeof(payload):extended?16:sizeof(controls),0,0,false,ms);
        if(!result.empty())throw std::runtime_error("invalid NR control response");
        return true;
    }catch(const std::exception& e){error_=e.what();return false;}
}
int HostClient::import_frame(int fd,std::uint64_t allocation,std::uint64_t bytes,const unsigned char* uuid){
    if(socket_<0||fd<0||!uuid||!supports_shared())throw std::runtime_error("shared runtime unavailable");
    Header h={magic,1,4,0,0,0,32,0};for(auto& n:h)n=htole32(n);send_all(socket_,h.data(),sizeof(h));
    std::array<unsigned char,32> payload{};allocation=htole64(allocation);bytes=htole64(bytes);
    std::memcpy(payload.data(),&allocation,8);std::memcpy(payload.data()+8,&bytes,8);std::memcpy(payload.data()+16,uuid,16);
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};iovec io{payload.data(),payload.size()};msghdr message{};
    message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
    auto* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));std::memcpy(CMSG_DATA(c),&fd,sizeof(fd));
    ssize_t sent;do{sent=sendmsg(socket_,&message,MSG_NOSIGNAL);}while(sent<0&&errno==EINTR);
    if(sent<=0)throw std::runtime_error("cannot send external memory descriptor");
    send_all(socket_,payload.data()+sent,payload.size()-static_cast<std::size_t>(sent));
    receive_all(socket_,h.data(),sizeof(h));for(auto& n:h)n=le32toh(n);
    if(h[0]!=magic||h[1]!=1||h[6]>4096)throw std::runtime_error("invalid shared import response");
    std::vector<unsigned char> result(h[6]);receive_all(socket_,result.data(),result.size());
    if(h[2])throw std::runtime_error(std::string(result.begin(),result.end()));
    if(result.size()!=4)throw std::runtime_error("invalid shared slot");
    std::uint32_t slot;std::memcpy(&slot,result.data(),4);slot=le32toh(slot);if(slot>=16)throw std::runtime_error("invalid shared slot");return static_cast<int>(slot);
}
bool HostClient::process_shared(unsigned slot,std::uint32_t width,std::uint32_t height,bool bgra,float strength) noexcept {
    try{
        const std::array<std::uint32_t,2> payload={htole32(slot),htole32(static_cast<unsigned>(std::lround(std::clamp(strength,0.f,1.f)*255)))};
        double ms{};auto result=exchange(socket_,5,payload.data(),sizeof(payload),width,height,bgra,ms);
        if(!result.empty())throw std::runtime_error("invalid shared frame response");
        average_ms_=average_ms_?average_ms_*.9+ms*.1:ms;return true;
    }catch(const std::exception& e){error_=e.what();if(socket_>=0)close(socket_);socket_=-1;return false;}
}
HostClient::~HostClient(){if(socket_>=0)close(socket_);}
bool HostClient::process(std::span<std::uint8_t> pixels,std::uint32_t width,std::uint32_t height,bool bgra,float strength) noexcept {
    if(socket_<0)return false;
    try {
        if(!width || !height || width>4096 || height>4096 || pixels.size()!=static_cast<std::size_t>(width)*height*4)
            throw std::runtime_error("invalid neural frame extent");
        const auto started=std::chrono::steady_clock::now();
        // Read mapped staging once in bulk; scalar reads from uncached VRAM can stall every pixel.
        std::vector<std::uint8_t> source(pixels.size());
        std::memcpy(source.data(),pixels.data(),pixels.size());
        const auto copied=std::chrono::steady_clock::now();
        double ms{};auto output=exchange(socket_,2,source.data(),static_cast<std::uint32_t>(pixels.size()),width,height,bgra,ms);
        const auto inferred=std::chrono::steady_clock::now();
        average_ms_=average_ms_?average_ms_*.9+ms*.1:ms;
        const auto alpha=static_cast<unsigned>(std::lround(std::clamp(strength,0.f,1.f)*255));
        if(alpha!=255) {
            for(std::size_t i=0;i<source.size();++i)if(i%4!=3)
                output[i]=static_cast<std::uint8_t>((source[i]*(255-alpha)+output[i]*alpha+127)/255);
        }
        for(std::size_t i=3;i<source.size();i+=4)output[i]=source[i];
        std::memcpy(pixels.data(),output.data(),output.size());
        if(++frame_count_<=3 || frame_count_%120==0) {
            const auto ended=std::chrono::steady_clock::now();
            const auto millis=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
            logging::write(logging::Level::info,"host64_frame="+std::to_string(frame_count_)+
                " readback_cpu_ms="+std::to_string(millis(started,copied))+
                " exchange_ms="+std::to_string(millis(copied,inferred))+
                " compose_cpu_ms="+std::to_string(millis(inferred,ended))+
                " inference_ms="+std::to_string(ms));
        }
        return true;
    }catch(const std::exception& e){error_=e.what();close(socket_);socket_=-1;return false;}
}
}
