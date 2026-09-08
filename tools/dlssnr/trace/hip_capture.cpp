// CPU-only HIP capture shim for offline graph recovery. NOT a HIP runtime.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <string>
struct Dim { unsigned x,y,z; };
struct Allocation { unsigned id; size_t size; };
static std::map<void*,Allocation> allocations;
static std::map<const void*,std::string> kernels;
static unsigned next_id=1;
static Dim grid,block;
static size_t shared_bytes;
static void* stream;
static const std::map<std::string,size_t> arg_sizes={
#include "kernel_sizes.inc"
};
#define API extern "C" __declspec(dllexport)
static void* alloc(size_t n) {
 if(n>512u*1024u*1024u){std::fprintf(stderr,"capture allocation exceeds limit\n");ExitProcess(80);}
 void* p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,n?n:1);
 if(!p)ExitProcess(81);
 allocations[p]={next_id++,n};
 std::printf("{\"op\":\"alloc\",\"id\":%u,\"address\":%llu,\"size\":%zu}\n",allocations[p].id,(unsigned long long)p,n);std::fflush(stdout);return p;
}
API int hipMalloc(void** p,size_t n){*p=alloc(n);return 0;}
API int hipFree(void* p){if(p){std::printf("{\"op\":\"free\",\"address\":%llu}\n",(unsigned long long)p);allocations.erase(p);HeapFree(GetProcessHeap(),0,p);}return 0;}
API int hipMemcpy(void* d,const void* s,size_t n,int kind){
 if(!d||!s)return 1;
 std::memcpy(d,s,n);
 std::printf("{\"op\":\"copy\",\"dst\":%llu,\"src\":%llu,\"size\":%zu,\"kind\":%d}\n",(unsigned long long)d,(unsigned long long)s,n,kind);
 if(kind==1){char name[128];std::snprintf(name,sizeof(name),"upload_%llu.bin",(unsigned long long)d);FILE* f=std::fopen(name,"wb");if(!f)ExitProcess(82);std::fwrite(s,1,n,f);std::fclose(f);}return 0;
}
API int hipMemcpyAsync(void* d,const void* s,size_t n,int k,void*){return hipMemcpy(d,s,n,k);}
API int hipMemset(void* p,int v,size_t n){std::memset(p,v,n);std::printf("{\"op\":\"fill\",\"dst\":%llu,\"size\":%zu,\"value\":%d}\n",(unsigned long long)p,n,v);return 0;}
API int hipMemsetAsync(void* p,int v,size_t n,void*){return hipMemset(p,v,n);}
API void** __hipRegisterFatBinary(const void*){return (void**)0x10000;}
API void __hipUnregisterFatBinary(void**){ }
API void __hipRegisterFunction(void**,const void* host,const char*,const char* name,int,void*,void*,void*,void*,void*){kernels[host]=name;}
API void __hipRegisterVar(void**,void*,char*,const char*,int,size_t,int,int){ }
API int hipMemcpyToSymbol(const void*,const void* src,size_t n,size_t,int){FILE*f=std::fopen("lookup.bin","wb");if(!f)ExitProcess(83);std::fwrite(src,1,n,f);std::fclose(f);return 0;}
API int __hipPushCallConfiguration(Dim g,Dim b,size_t n,void* s){grid=g;block=b;shared_bytes=n;stream=s;return 0;}
API int __hipPopCallConfiguration(Dim* g,Dim* b,size_t* n,void** s){*g=grid;*b=block;*n=shared_bytes;*s=stream;return 0;}
API int hipLaunchKernel(const void* f,Dim g,Dim b,void** args,size_t shared,void*){
 auto it=kernels.find(f);if(it==kernels.end()){std::fprintf(stderr,"unknown kernel %p\n",f);ExitProcess(84);}
 auto sz=arg_sizes.find(it->second);if(sz==arg_sizes.end())ExitProcess(85);
 // Image kernels take one by-value struct. Flag helpers aren't called by the
 // graph and must not be recorded as though they had the same argument ABI.
 if(it->second.find("flag_")!=std::string::npos)ExitProcess(86);
 std::printf("{\"op\":\"launch\",\"kernel\":\"%s\",\"grid\":[%u,%u,%u],\"block\":[%u,%u,%u],\"shared\":%zu,\"args\":\"",it->second.c_str(),g.x,g.y,g.z,b.x,b.y,b.z,shared);
 auto*p=(const unsigned char*)args[0];for(size_t i=0;i<sz->second;++i)std::printf("%02x",p[i]);std::printf("\"}\n");std::fflush(stdout);return 0;
}
API int hipGetDeviceCount(int*p){*p=0;return 0;}
API int hipGetDevicePropertiesR0600(void*,int){return 101;}
API const char* hipGetErrorString(int){return "CPU capture shim (no GPU execution)";}
API int hipGetLastError(){return 0;}
API int hipSetDevice(int){return 0;}
API int hipDeviceSynchronize(){return 0;}
API int hipDriverGetVersion(int*p){*p=70000000;return 0;}
API int hipRuntimeGetVersion(int*p){*p=70000000;return 0;}
API int hipEventCreate(void**p){*p=(void*)0x20000;return 0;}
API int hipEventRecord(void*,void*){return 0;}
API int hipEventSynchronize(void*){return 0;}
API int hipEventElapsedTime(float*p,void*,void*){*p=0;return 0;}
API int hipImportExternalMemory(void**,const void*){return 801;}
API int hipExternalMemoryGetMappedBuffer(void**,void*,const void*){return 801;}
API int hipDestroyExternalMemory(void*){return 801;}
