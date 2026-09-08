#include <hip/hip_runtime.h>
#include <cstddef>
// One thread per RGBA8 pixel. Keep integer blending identical to HostClient.
__global__ void prepare_frame(const uchar4* source,uchar4* destination,std::size_t pixels,bool bgra){
 const auto i=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(i>=pixels)return;
 auto p=source[i];if(bgra){auto r=p.x;p.x=p.z;p.z=r;}destination[i]=p;
}
__global__ void compose_frame(const uchar4* source,const uchar4* rendered,uchar4* destination,std::size_t pixels,bool bgra,unsigned strength){
 const auto i=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(i>=pixels)return;
 const auto s=source[i];auto r=rendered[i];if(bgra){auto v=r.x;r.x=r.z;r.z=v;}
 r.x=(unsigned(s.x)*(255-strength)+unsigned(r.x)*strength+127)/255;
 r.y=(unsigned(s.y)*(255-strength)+unsigned(r.y)*strength+127)/255;
 r.z=(unsigned(s.z)*(255-strength)+unsigned(r.z)*strength+127)/255;
 r.w=s.w;destination[i]=r;
}
extern "C" hipError_t ns_nr_prepare(const void* source,void* destination,std::size_t pixels,bool bgra,hipStream_t stream){
 hipLaunchKernelGGL(prepare_frame,dim3((pixels+255)/256),dim3(256),0,stream,static_cast<const uchar4*>(source),static_cast<uchar4*>(destination),pixels,bgra);return hipGetLastError();
}
extern "C" hipError_t ns_nr_compose(const void* source,const void* rendered,void* destination,std::size_t pixels,bool bgra,unsigned strength,hipStream_t stream){
 hipLaunchKernelGGL(compose_frame,dim3((pixels+255)/256),dim3(256),0,stream,static_cast<const uchar4*>(source),static_cast<const uchar4*>(rendered),static_cast<uchar4*>(destination),pixels,bgra,strength);return hipGetLastError();
}

// Scale the neural change before the recovered export quantizes/clips to RGBA8.
// Keep original recurrent tensors untouched. The caller owns a persistent scratch buffer.
__global__ void intensify_frame(const float* source,const float4* rendered,float4* destination,std::size_t pixels,float intensity){
 const auto i=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(i>=pixels)return;
 const auto s=make_float4(source[i*3],source[i*3+1],source[i*3+2],1.f);const auto r=rendered[i];
 destination[i]=intensity==0.f?s:make_float4(fmaf(intensity,r.x-s.x,s.x),fmaf(intensity,r.y-s.y,s.y),fmaf(intensity,r.z-s.z,s.z),1.f);
}
extern "C" hipError_t ns_nr_intensify(const void* source,const void* rendered,void* destination,std::size_t pixels,float intensity,hipStream_t stream){
 hipLaunchKernelGGL(intensify_frame,dim3((pixels+255)/256),dim3(256),0,stream,static_cast<const float*>(source),static_cast<const float4*>(rendered),static_cast<float4*>(destination),pixels,intensity);return hipGetLastError();
}
