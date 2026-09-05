#include "neural/runtime/host_client.hpp"
#include <sys/socket.h>
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
HostClient::~HostClient(){if(socket_>=0)close(socket_);}
bool HostClient::process(std::span<std::uint8_t> pixels,std::uint32_t width,std::uint32_t height,bool bgra,float strength) noexcept {
    if(socket_<0)return false;
    try {
        if(!width || !height || width>4096 || height>4096 || pixels.size()!=static_cast<std::size_t>(width)*height*4)
            throw std::runtime_error("invalid neural frame extent");
        double ms{};auto output=exchange(socket_,2,pixels.data(),static_cast<std::uint32_t>(pixels.size()),width,height,bgra,ms);
        average_ms_=average_ms_?average_ms_*.9+ms*.1:ms;
        const auto alpha=static_cast<unsigned>(std::lround(std::clamp(strength,0.f,1.f)*255));
        for(std::size_t i=0;i<pixels.size();++i)if(i%4!=3)
            pixels[i]=static_cast<std::uint8_t>((pixels[i]*(255-alpha)+output[i]*alpha+127)/255);
        return true;
    }catch(const std::exception& e){error_=e.what();close(socket_);socket_=-1;return false;}
}
}
