// Dedicated host64 for the native model. Protocol v1 remains compatible with x86 layers.
#include <json-c/json.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
void* ns_nr_create(const char*,const char*,const char*,const char*);
void ns_nr_destroy(void*);
const char* ns_nr_error();
int ns_nr_run(void*,const unsigned char*,unsigned char*,std::size_t,int);
int ns_nr_reset(void*);
int ns_nr_controls(void*,float,float,float);
int ns_nr_controls_v2(void*,float,float,float,unsigned);
int ns_nr_controls_v3(void*,float,float,float,unsigned,unsigned,unsigned,float);
int ns_nr_import_frame(void*,int,std::size_t,std::size_t);
int ns_nr_run_shared(void*,unsigned,int,unsigned);
int ns_nr_device_pci(unsigned char*);
}
namespace {
using Header=std::array<std::uint32_t,8>;
using Json=std::unique_ptr<json_object,decltype(&json_object_put)>;
using Engine=std::unique_ptr<void,decltype(&ns_nr_destroy)>;
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
struct Fd {int value=-1;~Fd(){if(value>=0)close(value);} Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;explicit Fd(int n):value(n){};};
void transfer(int fd,void* data,std::size_t size,bool writing){
 auto* p=static_cast<char*>(data);
 while(size){auto n=writing?send(fd,p,size,MSG_NOSIGNAL):recv(fd,p,size,0);
  if(n<0&&errno==EINTR&&!stopping)continue;
  if(n<=0)throw std::runtime_error("peer disconnected or timed out");
  p+=n;size-=static_cast<std::size_t>(n);
 }
}
void reply(int fd,Header h,std::vector<unsigned char>& body){h[6]=static_cast<std::uint32_t>(body.size());for(auto& x:h)x=htole32(x);transfer(fd,h.data(),sizeof(h),true);transfer(fd,body.data(),body.size(),true);}
Json document(const std::filesystem::path& path){
 if(std::filesystem::file_size(path)>65536)throw std::runtime_error("metadata too large");
 Json result(json_object_from_file(path.c_str()),json_object_put);
 if(!result||!json_object_is_type(result.get(),json_type_object))throw std::runtime_error("invalid model metadata");
 return result;
}
json_object* member(json_object* root,const char* key){json_object* value=nullptr;if(!json_object_object_get_ex(root,key,&value))throw std::runtime_error(std::string("missing metadata: ")+key);return value;}
std::string string(json_object* value){if(!json_object_is_type(value,json_type_string))throw std::runtime_error("expected string metadata");return json_object_get_string(value);}
std::string digest(const std::filesystem::path& path){
 std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("missing model artifact");
 std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
 if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("SHA256 initialization failed");
 std::array<char,65536> chunk{};std::size_t total=0;
 while(file){file.read(chunk.data(),chunk.size());auto n=file.gcount();total+=static_cast<std::size_t>(n);
  if(total>512u*1024*1024||EVP_DigestUpdate(context.get(),chunk.data(),static_cast<std::size_t>(n))!=1)throw std::runtime_error("invalid artifact size or digest");}
 if(!file.eof())throw std::runtime_error("artifact read failed");
 unsigned char bytes[32];unsigned size=0;if(EVP_DigestFinal_ex(context.get(),bytes,&size)!=1||size!=32)throw std::runtime_error("SHA256 failed");
 std::string out;for(auto b:bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;
}
void handle(int peer,const std::filesystem::path& allowed){
 Engine engine(nullptr,ns_nr_destroy);unsigned width=0,height=0;
 while(!stopping){Header h{};transfer(peer,h.data(),sizeof(h),false);for(auto& x:h)x=le32toh(x);
  const auto op=h[2],w=h[3],height_in=h[4],fmt=h[5],size=h[6];
  if(h[0]!=0x3152534e||h[1]!=1||op<1||op>8||size>4096u*4096u*4u||
     (op==1&&(!size||size>4096))||(op==3&&size)||(op==4&&size!=32)||(op==5&&size!=8)||(op==6&&size!=12)||(op==7&&size!=16)||(op==8&&size!=28)||
     (op==2&&(!w||!height_in||w>4096||height_in>4096||fmt>1||size!=std::uint64_t(w)*height_in*4)))throw std::runtime_error("invalid protocol request");
  std::vector<unsigned char> input(size),output;Fd imported(-1);
  if(op==4){
   alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};iovec io{input.data(),input.size()};msghdr message{};
   message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
   const auto received=recvmsg(peer,&message,MSG_CMSG_CLOEXEC);
   for(auto* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS){
    const auto count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);for(std::size_t i=0;i<count;++i){int fd;std::memcpy(&fd,CMSG_DATA(c)+i*sizeof(int),sizeof(fd));if(imported.value<0)imported.value=fd;else close(fd);}
   }
   if(received<=0||(message.msg_flags&MSG_CTRUNC)||imported.value<0)throw std::runtime_error("invalid external memory descriptor");
   transfer(peer,input.data()+received,input.size()-static_cast<std::size_t>(received),false);
  }else transfer(peer,input.data(),input.size(),false);
  Header response={0x3152534e,1,0,w,height_in,fmt,0,0};
  try{
   if(op==3){const std::string status="{\"version\":1,\"engines\":[\"dlssnr_hip\"],\"host\":\"cpp64\"}";output.assign(status.begin(),status.end());}
   else if(op==1){
    const std::string requested(input.begin(),input.end());if(requested.find('\0')!=std::string::npos||std::filesystem::canonical(requested)!=allowed)throw std::runtime_error("host restricted to configured model");
    auto manifest=document(allowed/"manifest.json"),metadata=document(allowed/"metadata.json");
    if(string(member(manifest.get(),"runtime"))!="dlssnr_hip")throw std::runtime_error("native host requires dlssnr_hip");
    auto* hashes=member(metadata.get(),"sha256");
    if(!json_object_is_type(hashes,json_type_object)||json_object_object_length(hashes)!=4)throw std::runtime_error("native artifact hashes required");
    for(const auto* name:{"graph.bin","kernels.hsaco","weights.bin","lookup.bin"})if(digest(allowed/name)!=string(member(hashes,name)))throw std::runtime_error(std::string("artifact SHA256 mismatch: ")+name);
    std::ifstream plan(allowed/"graph.bin",std::ios::binary);std::array<std::uint32_t,10> header{};
    if(!plan.read(reinterpret_cast<char*>(header.data()),sizeof(header))||std::memcmp(header.data(),"NSNRPLAN",8)||header[2]!=1)throw std::runtime_error("invalid graph header");
    width=header[3];height=header[4];if(!width||!height||width>3840||height>2160||((w||height_in)&&(w!=width||height_in!=height)))throw std::runtime_error("model extent mismatch");
    engine.reset();engine.reset(ns_nr_create((allowed/"kernels.hsaco").c_str(),(allowed/"graph.bin").c_str(),(allowed/"weights.bin").c_str(),(allowed/"lookup.bin").c_str()));
    if(!engine)throw std::runtime_error(ns_nr_error());
    if(w){std::vector<unsigned char> warm(std::size_t(width)*height*4),result(warm.size());if(ns_nr_run(engine.get(),warm.data(),result.data(),warm.size(),0)||ns_nr_reset(engine.get()))throw std::runtime_error(ns_nr_error());}
    Json info(json_object_new_object(),json_object_put);
    json_object_object_add(info.get(),"backend",json_object_new_string("NeuroShade/HIP native"));
    json_object_object_add(info.get(),"host",json_object_new_string("cpp64"));
    json_object_object_add(info.get(),"shared_buffers",json_object_new_boolean(true));
    json_object_object_add(info.get(),"nr_controls",json_object_new_boolean(true));
    json_object_object_add(info.get(),"nr_controls_v2",json_object_new_boolean(true));
    json_object_object_add(info.get(),"nr_controls_v3",json_object_new_boolean(true));
    json_object_object_add(info.get(),"model_version",json_object_get(member(manifest.get(),"version")));
    json_object* controls=nullptr;
    if(json_object_object_get_ex(metadata.get(),"dlssnr_controls",&controls))json_object_object_add(info.get(),"controls",json_object_get(controls));
    json_object_object_add(info.get(),"external_motion_depth",json_object_new_boolean(false));
    const std::string text=json_object_to_json_string_ext(info.get(),JSON_C_TO_STRING_PLAIN);output.assign(text.begin(),text.end());
    std::cout<<"model=loaded "<<text<<std::endl;
   }else if(op==8){
    float c[7];std::memcpy(c,input.data(),sizeof(c));
    if((c[3]!=0.f&&c[3]!=1.f)||(c[4]!=0.f&&c[4]!=1.f&&c[4]!=2.f)||(c[5]!=0.f&&c[5]!=1.f&&c[5]!=2.f&&c[5]!=3.f))throw std::runtime_error("invalid NR selection");
    if(!engine||ns_nr_controls_v3(engine.get(),c[0],c[1],c[2],c[3]==1.f,static_cast<unsigned>(c[4]),static_cast<unsigned>(c[5]),c[6]))throw std::runtime_error(ns_nr_error());
    std::cout<<"nr_controls=applied style="<<c[4]<<" preset_requested="<<c[5]<<" preset_effective=1 intensity="<<c[6]<<" automatic_mask="<<c[3]<<std::endl;
   }else if(op==7){
    float c[4];std::memcpy(c,input.data(),sizeof(c));
    if(c[3]!=0.f&&c[3]!=1.f)throw std::runtime_error("invalid automatic mask flag");
    if(!engine||ns_nr_controls_v2(engine.get(),c[0],c[1],c[2],c[3]==1.f))throw std::runtime_error(ns_nr_error());
    std::cout<<"nr_controls=applied tone="<<c[0]<<" structure="<<c[1]<<" skin="<<c[2]<<" automatic_mask="<<c[3]<<std::endl;
   }else if(op==6){
    float controls[3];std::memcpy(controls,input.data(),sizeof(controls));
    if(!engine||ns_nr_controls(engine.get(),controls[0],controls[1],controls[2]))throw std::runtime_error(ns_nr_error());
    std::cout<<"nr_controls=applied tone="<<controls[0]<<" structure="<<controls[1]<<" skin="<<controls[2]<<std::endl;
   }else if(op==4){
    if(!engine)throw std::runtime_error("load model first");
    std::uint64_t allocation,bytes;std::memcpy(&allocation,input.data(),8);std::memcpy(&bytes,input.data()+8,8);
    unsigned char uuid[16];if(ns_nr_device_pci(uuid)||std::memcmp(uuid,input.data()+16,16))throw std::runtime_error("Vulkan/HIP PCI device mismatch");
    const int slot=ns_nr_import_frame(engine.get(),imported.value,le64toh(allocation),le64toh(bytes));if(slot<0)throw std::runtime_error(ns_nr_error());
    const auto value=htole32(static_cast<std::uint32_t>(slot));output.resize(4);std::memcpy(output.data(),&value,4);
   }else if(op==5){
    if(!engine||w!=width||height_in!=height||fmt>1)throw std::runtime_error("invalid shared frame extent");
    std::uint32_t slot,strength;std::memcpy(&slot,input.data(),4);std::memcpy(&strength,input.data()+4,4);
    const auto start=std::chrono::steady_clock::now();
    if(ns_nr_run_shared(engine.get(),le32toh(slot),static_cast<int>(fmt),le32toh(strength)))throw std::runtime_error(ns_nr_error());
    response[7]=static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
   }else{
    if(!engine||w!=width||height_in!=height)throw std::runtime_error("load matching model first");
    output.resize(size);const auto start=std::chrono::steady_clock::now();
    if(ns_nr_run(engine.get(),input.data(),output.data(),size,static_cast<int>(fmt)))throw std::runtime_error(ns_nr_error());
    response[7]=static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
   }
  }catch(const std::exception& e){if(op!=4&&op!=6&&op!=7&&op!=8)engine.reset();response[2]=1;const std::string error=e.what();output.assign(error.begin(),error.begin()+std::min<std::size_t>(4096,error.size()));}
  reply(peer,response,output);
 }
}
}
int main(int argc,char**argv){try{
 if(argc!=5||std::string(argv[1])!="--socket"||std::string(argv[3])!="--model")throw std::runtime_error("usage: ns-dlssnr-host --socket PATH --model MODEL.nsmodel");
 const std::filesystem::path path=std::filesystem::absolute(argv[2]),model=std::filesystem::canonical(argv[4]);
 sockaddr_un address{};address.sun_family=AF_UNIX;if(path.string().size()>=sizeof(address.sun_path))throw std::runtime_error("socket path too long");std::strcpy(address.sun_path,path.c_str());
 std::filesystem::create_directories(path.parent_path());
 Fd lock(open((path.string()+".lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600));
 if(lock.value<0||flock(lock.value,LOCK_EX|LOCK_NB))throw std::runtime_error("native host already running");
 struct stat st{};if(lstat(path.c_str(),&st)==0){
  if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid())throw std::runtime_error("refusing to replace socket path");
  Fd probe(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
  if(connect(probe.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0||errno!=ECONNREFUSED)throw std::runtime_error("runtime endpoint already in use");
  if(unlink(path.c_str()))throw std::runtime_error("cannot remove stale socket");
 }
 Fd listener(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
 const auto previous=umask(0077);const auto bound=bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address));umask(previous);
 if(bound)throw std::runtime_error("cannot bind runtime socket");
 struct Cleanup {std::filesystem::path path;~Cleanup(){unlink(path.c_str());}} cleanup{path};
 if(listen(listener.value,4))throw std::runtime_error("cannot listen");
 struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,nullptr);sigaction(SIGINT,&action,nullptr);
 std::cout<<"runtime=ready host=cpp64 socket="<<path<<std::endl;
 // One owner at a time: recurrent state cannot be shared across swapchains or games.
 while(!stopping){Fd peer(accept4(listener.value,nullptr,nullptr,SOCK_CLOEXEC));if(peer.value<0){if(errno==EINTR)continue;throw std::runtime_error("accept failed");}
  ucred credentials{};socklen_t length=sizeof(credentials);
  if(getsockopt(peer.value,SOL_SOCKET,SO_PEERCRED,&credentials,&length)||credentials.uid!=getuid())continue;
  timeval timeout{120,0};setsockopt(peer.value,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(peer.value,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  try{handle(peer.value,model);}catch(const std::exception& e){if(!stopping)std::cerr<<"client_closed="<<e.what()<<std::endl;}
 }
 return 0;
}catch(const std::exception& e){std::cerr<<"ns-dlssnr-host: "<<e.what()<<'\n';return 1;}}
