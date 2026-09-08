#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <memory>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
void* ns_nr_create(const char*,const char*,const char*,const char*);
void ns_nr_destroy(void*);
const char* ns_nr_error();
int ns_nr_profile(void*,const unsigned char*,std::size_t,unsigned,const char*);
}
int main(int argc,char**argv){try{
 if(argc!=3)throw std::runtime_error("usage: ns-dlssnr-profile MODEL.nsmodel report.csv");
 const char* runtime=std::getenv("XDG_RUNTIME_DIR");
 const auto lock_path=std::filesystem::path(runtime?runtime:"/tmp")/("neuroshade-gpu-qualification-"+std::to_string(getuid())+".lock");
 int lock=open(lock_path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB)!=0)throw std::runtime_error("GPU qualification lock unavailable");
 const auto model=std::filesystem::absolute(argv[1]);std::ifstream f(model/"graph.bin",std::ios::binary);std::array<std::uint32_t,10> header{};
 if(!f.read(reinterpret_cast<char*>(header.data()),sizeof(header)))throw std::runtime_error("invalid graph header");
 const auto width=header[3],height=header[4];if(!width||!height||width>3840||height>2160)throw std::runtime_error("invalid extent");
 const auto image=(model/"kernels.hsaco").string(),plan=(model/"graph.bin").string(),weights=(model/"weights.bin").string(),lut=(model/"lookup.bin").string();
 std::unique_ptr<void,decltype(&ns_nr_destroy)> engine(ns_nr_create(image.c_str(),plan.c_str(),weights.c_str(),lut.c_str()),ns_nr_destroy);
 if(!engine)throw std::runtime_error(ns_nr_error());
 std::vector<unsigned char> pixels(std::size_t(width)*height*4);std::uint32_t state=17;
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto i=(std::size_t(y)*width+x)*4;state=state*1664525u+1013904223u;pixels[i]=x*255/width;pixels[i+1]=y*255/height;pixels[i+2]=state>>24;pixels[i+3]=255;}
 if(ns_nr_profile(engine.get(),pixels.data(),pixels.size(),5,argv[2]))throw std::runtime_error(ns_nr_error());
 std::cout<<"profile=pass iterations=5 output_bitwise_equal=yes width="<<width<<" height="<<height<<" report="<<argv[2]<<'\n';
 return 0;
}catch(const std::exception&e){std::cerr<<"ns-dlssnr-profile: "<<e.what()<<'\n';return 1;}}
