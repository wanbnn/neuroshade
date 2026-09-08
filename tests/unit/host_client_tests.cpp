#include "neural/runtime/host_client.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <vector>
#include <cstring>
#include <iostream>
#include <stdexcept>
void require(bool b){if(!b)throw std::runtime_error("host client regression");}
void receive(int fd,void* data,std::size_t size){auto*p=static_cast<char*>(data);while(size){auto n=recv(fd,p,size,0);require(n>0);p+=n;size-=n;}}
void send_bytes(int fd,const void* data,std::size_t size){auto*p=static_cast<const char*>(data);while(size){auto n=send(fd,p,size,MSG_NOSIGNAL);require(n>0);p+=n;size-=n;}}
int main(){
 const std::string path="/tmp/ns-host-test-"+std::to_string(getpid());
 int listener=socket(AF_UNIX,SOCK_STREAM,0);require(listener>=0);sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,path.c_str());
 require(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);require(listen(listener,1)==0);
 auto child=fork();require(child>=0);
 if(child==0){try{int fd=accept(listener,nullptr,nullptr);require(fd>=0);
  for(int request=0;request<9;++request){std::array<std::uint32_t,8> h{};receive(fd,h.data(),sizeof(h));std::vector<unsigned char> body(h[6]);
   if(request==4){
    require(h[2]==4&&h[6]==32);alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    iovec io{body.data(),body.size()};msghdr msg{};msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    auto n=recvmsg(fd,&msg,MSG_CMSG_CLOEXEC);require(n>0);receive(fd,body.data()+n,body.size()-n);
    auto* c=CMSG_FIRSTHDR(&msg);require(c&&c->cmsg_type==SCM_RIGHTS);int imported;std::memcpy(&imported,CMSG_DATA(c),sizeof(imported));require(fcntl(imported,F_GETFD)&FD_CLOEXEC);close(imported);
    std::uint64_t allocation,bytes;std::memcpy(&allocation,body.data(),8);std::memcpy(&bytes,body.data()+8,8);require(allocation==4096&&bytes==16);require(body[16]==42);body={3,0,0,0};
   }else receive(fd,body.data(),body.size());
   if(request==0){const std::string info="{\"shared_buffers\":true,\"nr_controls\":true,\"nr_controls_v2\":true,\"nr_controls_v3\":true}";body.assign(info.begin(),info.end());}
   else if(request>=6){require(h[2]==8&&body.size()==28);float controls[7];std::memcpy(controls,body.data(),28);if(request==6)require(controls[0]==.25f&&controls[1]==.5f&&controls[2]==.75f&&controls[3]==1.f);else if(request==7)require(controls[0]==1.54f&&controls[1]==2.f&&controls[2]==-1.f&&controls[3]==0.f);else require(controls[4]==2.f&&controls[5]==3.f&&controls[6]==1.38f);body.clear();}
   else if(request==5){require(h[2]==5&&h[3]==2&&h[4]==2&&h[5]==1&&body.size()==8&&body[0]==3&&body[4]==128);body.clear();}
   else if(request<4){require(body.size()==16);for(int i=0;i<4;++i){body[i*4]=200;body[i*4+1]=100;body[i*4+2]=50;body[i*4+3]=0;}}
   h[2]=0;h[6]=body.size();h[7]=1250;send_bytes(fd,h.data(),sizeof(h));send_bytes(fd,body.data(),body.size());
  }close(fd);_exit(0);
 }catch(...){_exit(1);}}
 close(listener);setenv("NEUROSHADE_RUNTIME_SOCKET",path.c_str(),1);int result=0;
 try{neuroshade::neural::HostClient client("fixture.nsmodel",2,2);
  for(float strength:{1.f,0.f,.5f}){std::array<std::uint8_t,16> pixels{};for(int i=0;i<4;++i){pixels[i*4]=20;pixels[i*4+1]=40;pixels[i*4+2]=60;pixels[i*4+3]=77;}
   require(client.process(pixels,2,2,false,strength));const unsigned alpha=strength==1?255:strength==0?0:128;
   for(int i=0;i<4;++i){require(pixels[i*4]==(20*(255-alpha)+200*alpha+127)/255);require(pixels[i*4+1]==(40*(255-alpha)+100*alpha+127)/255);require(pixels[i*4+2]==(60*(255-alpha)+50*alpha+127)/255);require(pixels[i*4+3]==77);}
  }
  require(client.supports_shared());int exported=open("/dev/null",O_RDONLY|O_CLOEXEC);require(exported>=0);unsigned char identity[16]={42};
  require(client.import_frame(exported,4096,16,identity)==3);require(fcntl(exported,F_GETFD)>=0);close(exported);
  require(client.process_shared(3,2,2,true,.5f));
  require(client.configure_nr({.25f,.5f,.75f}));
  require(!client.configure_nr({2.01f,1.f,1.f}));
  require(client.configure_nr({1.54f,2.f,-1.f},false));
  require(!client.configure_nr({0.f,1.f,1.f},true,3));
  require(client.configure_nr({0.f,1.f,1.f},true,2,3,1.38f));
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';result=1;kill(child,SIGTERM);}
 int status{};waitpid(child,&status,0);unlink(path.c_str());if(!WIFEXITED(status)||WEXITSTATUS(status))result=1;
 if(!result)std::cout<<"host_client=pass strengths=0,0.5,1 alpha=preserved\n";return result;
}
