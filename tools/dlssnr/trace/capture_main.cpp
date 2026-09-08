#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
struct MsvcString { char* pointer; char unused[8]; size_t length,capacity; };
int main(int argc,char**argv){
 if(argc!=3 && argc!=5){std::fprintf(stderr,"capture requires subject.dll weights.bin [width height]\n");return 64;}
 int width=argc==5?std::atoi(argv[3]):128,height=argc==5?std::atoi(argv[4]):128;
 if(width<1||height<1||width>3840||height>2160)return 68;
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 SetEnvironmentVariableA("DLSSNR_STAGES",nullptr);
 SetEnvironmentVariableA("DLSSNR_NO_REPACK",nullptr);
 auto dll=LoadLibraryA(argv[1]);
 if(!dll){std::fprintf(stderr,"LoadLibrary=%lu\n",GetLastError());return 65;}
 auto*base=(unsigned char*)dll;
 // The following RVAs are ONLY for the locally examined AMD companion.
 // The build/run driver must validate its SHA-256 before launching this probe.
 // Probe only: the companion hard-codes style 0 at the qualified pre-kernel
 // argument construction. Substitute the NGX style/128 constant before CPU capture.
 if(const char* text=std::getenv("NS_CAPTURE_STYLE")){
  char* end=nullptr;const long style=std::strtol(text,&end,10);
  if(!end||*end||style<0||style>2)return 69;
  const unsigned char expected[]={0xc7,0x85,0x5c,0x01,0,0,0,0,0,0};
  auto* instruction=base+0x1e44c;
  if(std::memcmp(instruction,expected,sizeof(expected)))return 70;
  DWORD previous; if(!VirtualProtect(instruction,sizeof(expected),PAGE_EXECUTE_READWRITE,&previous))return 70;
  const float conditioning=float(style)/128.f;std::memcpy(instruction+6,&conditioning,4);
  DWORD ignored;VirtualProtect(instruction,sizeof(expected),previous,&ignored);
  FlushInstructionCache(GetCurrentProcess(),instruction,sizeof(expected));
 }
 auto*engine=base+0x744d8;
 MsvcString path{};path.pointer=argv[2];path.length=std::strlen(argv[2]);path.capacity=path.length<16?16:path.length;
 std::fprintf(stderr,"capture init begin\n");
 auto init=(bool(*)(void*,MsvcString*))(base+0x10760);
 if(!init(engine,&path)){std::fprintf(stderr,"engine init failed\n");return 66;}
 // Reproduce the settings transfer performed by the companion worker (VA 18000cd20).
 // LocalTone=0, LocalStructure=1, SkinStructure=-1 inherits LocalStructure.
 float controls[]={0.0f,1.0f,1.0f,1.0f,0.03125f};
 if(const char* probe=std::getenv("NS_CAPTURE_CONTROLS")){
  if(std::sscanf(probe,"%f,%f,%f",&controls[0],&controls[1],&controls[2])!=3)return 69;
  controls[3]=controls[1];
  if(controls[2]<0)controls[2]=controls[1];
 }
 if(const char* mask=std::getenv("NS_CAPTURE_AUTO_MASK")){
  if(std::strcmp(mask,"0")==0)controls[2]=controls[3]=-1.f;
  else if(std::strcmp(mask,"1")!=0)return 69;
 }
 std::memcpy(engine+0x20,controls,sizeof(controls));
 std::fprintf(stderr,"capture init complete\n");
 auto hip=GetModuleHandleA("amdhip64_7.dll");
 auto allocate=(int(*)(void**,size_t))GetProcAddress(hip,"hipMalloc");
 void *input=nullptr,*output=nullptr;
 if(!allocate || allocate(&input,size_t(width)*height*4) || allocate(&output,size_t(width)*height*4))return 67;
 auto process=(void(*)(void*,void*,int,int,int,int,void*,int,int,int,void*))(base+0x1c750);
 for(int frame=0;frame<2;++frame){
 std::printf("{\"op\":\"begin_frame\",\"frame\":%d,\"width\":%d,\"height\":%d,\"input\":%llu,\"output\":%llu}\n",frame,width,height,(unsigned long long)input,(unsigned long long)output);
 // Source/destination pitch in bytes and format 1 (RGBA8), then image extent.
 process(engine,input,width*4,1,height,width,output,width*4,1,0,nullptr);
 std::puts("{\"op\":\"end_frame\",\"gpu_execution\":false}");std::fflush(stdout);
 }
 // No subject destructors: this process owns all captured allocations.
 ExitProcess(0);
}
