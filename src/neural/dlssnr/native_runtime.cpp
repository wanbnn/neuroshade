// Native HIP execution of a recovered spatial DLSSNR graph. No Windows/PyTorch runtime.
#include <hip/hip_runtime_api.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <iomanip>
#include <unistd.h>
#include <cstdio>
#include <cmath>
extern "C" hipError_t ns_nr_prepare(const void*,void*,std::size_t,bool,hipStream_t);
extern "C" hipError_t ns_nr_compose(const void*,const void*,void*,std::size_t,bool,unsigned,hipStream_t);
extern "C" hipError_t ns_nr_intensify(const void*,const void*,void*,std::size_t,float,hipStream_t);
namespace {
thread_local std::string last_error;
void check(hipError_t e) { if(e!=hipSuccess) throw std::runtime_error(hipGetErrorString(e)); }
std::vector<unsigned char> read(const char* path, std::size_t limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()<0 || static_cast<std::uint64_t>(f.tellg())>limit) throw std::runtime_error("invalid native artifact size");
    std::vector<unsigned char> data(static_cast<std::size_t>(f.tellg())); f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()))) throw std::runtime_error("cannot read native artifact");
    return data;
}
struct Reader {
    const std::vector<unsigned char>& data; std::size_t offset{};
    std::vector<unsigned char> bytes(std::size_t n) {
        if(n>data.size()-offset) throw std::runtime_error("truncated native plan");
        auto first=data.begin()+static_cast<std::ptrdiff_t>(offset);offset+=n;return {first,first+static_cast<std::ptrdiff_t>(n)};
    }
    template<class T> T integer() { auto b=bytes(sizeof(T)); T value{};std::memcpy(&value,b.data(),sizeof(T));return value; }
    std::uint32_t u32() {return integer<std::uint32_t>();} std::uint64_t u64() {return integer<std::uint64_t>();}
};
struct Buffer { unsigned char* pointer{};std::size_t size{}; };
struct Operation { std::string name; unsigned tag{}; hipFunction_t function{};std::array<unsigned,7> dims{};std::vector<unsigned char> arguments;unsigned char *dst{},*src{};std::size_t bytes{}; };
struct SharedFrame {
    hipExternalMemory_t memory{};void* pointer{};
    ~SharedFrame(){if(pointer)(void)hipFree(pointer);if(memory)(void)hipDestroyExternalMemory(memory);}
    void initialize(int fd,std::size_t allocation,std::size_t bytes){
        // Duplicate: the caller retains its descriptor on both success and failure.
        const int imported_fd=dup(fd);if(imported_fd<0)throw std::runtime_error("cannot duplicate external memory fd");
        hipExternalMemoryHandleDesc desc{};desc.type=hipExternalMemoryHandleTypeOpaqueFd;desc.handle.fd=imported_fd;desc.size=allocation;
        auto error=hipImportExternalMemory(&memory,&desc);if(error!=hipSuccess){close(imported_fd);check(error);}
        hipExternalMemoryBufferDesc mapping{};mapping.size=bytes;check(hipExternalMemoryGetMappedBuffer(&pointer,memory,&mapping));
    }
};
struct Runtime {
    hipModule_t module{};hipStream_t stream{};hipGraph_t graph{};hipGraphExec_t executable{};
    std::vector<Buffer> buffers;std::vector<Operation> operations;
    unsigned width{},height{},input{},output{},weights{};
    std::vector<std::unique_ptr<SharedFrame>> shared_frames;
    unsigned char *host_frame{},*device_frame{},*intensity_frame{};
    float nr_intensity{1.f};
    unsigned nr_style{},nr_preset{};
    void enqueue(Operation& op){
        if(op.tag==2){check(hipMemcpyAsync(op.dst,op.src,op.bytes,hipMemcpyDeviceToDevice,stream));return;}
        auto arguments=op.arguments;
        if(nr_intensity!=1.f&&op.name=="_Z8k_export12ExportParams"){
            void *rendered{},*original{};
            std::memcpy(&rendered,arguments.data(),8);std::memcpy(&original,arguments.data()+48,8);
            check(ns_nr_intensify(original,rendered,intensity_frame,std::size_t(width)*height,nr_intensity,stream));
            std::memcpy(arguments.data(),&intensity_frame,8);
        }
        void* args[]={arguments.data()};auto& d=op.dims;
        check(hipModuleLaunchKernel(op.function,d[0],d[1],d[2],d[3],d[4],d[5],d[6],stream,args,nullptr));
    }
    void run_device(void* frame,std::size_t size,bool bgra,unsigned strength){
        if(!frame||size!=std::size_t(width)*height*4||strength>255)throw std::runtime_error("invalid device frame");
        check(ns_nr_prepare(frame,buffers[input].pointer,size/4,bgra,stream));
        check(hipGraphLaunch(executable,stream));
        check(ns_nr_compose(frame,buffers[output].pointer,frame,size/4,bgra,strength,stream));
    }
    unsigned import_frame(int fd,std::size_t allocation,std::size_t bytes){
        if(fd<0||bytes!=std::size_t(width)*height*4||allocation<bytes||allocation>512u*1024*1024||shared_frames.size()>=16)throw std::runtime_error("invalid external frame allocation");
        auto frame=std::make_unique<SharedFrame>();frame->initialize(fd,allocation,bytes);shared_frames.push_back(std::move(frame));return static_cast<unsigned>(shared_frames.size()-1);
    }
    ~Runtime() {
        if(stream) (void)hipStreamSynchronize(stream);
        if(executable)(void)hipGraphExecDestroy(executable);if(graph)(void)hipGraphDestroy(graph);
        shared_frames.clear();
        if(intensity_frame)(void)hipFree(intensity_frame);
        if(host_frame)(void)hipHostFree(host_frame);if(device_frame)(void)hipFree(device_frame);
        for(auto b:buffers)if(b.pointer)(void)hipFree(b.pointer);
        if(module)(void)hipModuleUnload(module);if(stream)(void)hipStreamDestroy(stream);
    }
    unsigned char* reference(unsigned index,std::uint64_t offset,std::uint64_t length=1) {
        if(index>=buffers.size() || offset>buffers[index].size || length>buffers[index].size-offset)throw std::runtime_error("native buffer range invalid");
        return buffers[index].pointer+offset;
    }
    void initialize(const char* image,const char* plan,const char* packed,const char* lookup) {
        auto data=read(plan,1024*1024);Reader r{data};auto magic=r.bytes(8);
        if(std::memcmp(magic.data(),"NSNRPLAN",8) || r.u32()!=1)throw std::runtime_error("unsupported native plan");
        width=r.u32();height=r.u32();auto count=r.u32(),op_count=r.u32();input=r.u32();output=r.u32();weights=r.u32();
        if(!width||!height||width>3840||height>2160||count>128||count<3||op_count>512||!op_count||input>=count||output>=count||weights>=count||input==output||input==weights||output==weights)throw std::runtime_error("invalid native plan header");
        std::vector<std::uint64_t> sizes;std::uint64_t total{};
        for(unsigned i=0;i<count;++i){auto n=r.u64();if(!n||n>512ULL*1024*1024)throw std::runtime_error("invalid native allocation");sizes.push_back(n);total+=n;}
        if(total>3ULL*1024*1024*1024 || sizes[input]<width*height*4ULL || sizes[output]<width*height*4ULL)throw std::runtime_error("native memory budget exceeded");
        check(hipSetDevice(0));hipDeviceProp_t properties{};check(hipGetDeviceProperties(&properties,0));
        if(std::string(properties.gcnArchName)!="gfx1200")throw std::runtime_error("this native artifact requires gfx1200");
        check(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking));check(hipModuleLoad(&module,image));
        buffers.resize(count);
        for(unsigned i=0;i<count;++i){buffers[i].size=sizes[i];check(hipMalloc(reinterpret_cast<void**>(&buffers[i].pointer),sizes[i]));}
        auto weight_data=read(packed,512ULL*1024*1024);
        if(weight_data.size()!=sizes[weights])throw std::runtime_error("native weights size mismatch");
        check(hipMemcpy(buffers[weights].pointer,weight_data.data(),weight_data.size(),hipMemcpyHostToDevice));
        auto table=read(lookup,512);void* symbol{};std::size_t symbol_size{};
        check(hipModuleGetGlobal(&symbol,&symbol_size,module,"g_e4m3_lut"));
        if(table.size()!=512 || symbol_size!=512)throw std::runtime_error("invalid native FP8 table");
        check(hipMemcpy(symbol,table.data(),512,hipMemcpyHostToDevice));
        for(unsigned i=0;i<op_count;++i){
            Operation op;op.tag=r.u32();
            if(op.tag==2){
                auto di=r.u32();auto d=r.u64();auto si=r.u32();auto s=r.u64();op.bytes=r.u64();
                op.dst=reference(di,d,op.bytes);op.src=reference(si,s,op.bytes);
                if(di==weights)throw std::runtime_error("frame modifies native weights");
            }else if(op.tag==3){
                auto length=r.u32();if(!length||length>128)throw std::runtime_error("invalid native kernel name");
                auto name=r.bytes(length);if(!std::all_of(name.begin(),name.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}))throw std::runtime_error("invalid kernel identifier");
                std::string kernel(name.begin(),name.end());op.name=kernel;check(hipModuleGetFunction(&op.function,module,kernel.c_str()));
                for(auto&d:op.dims)d=r.u32();
                if(!op.dims[0]||!op.dims[1]||!op.dims[2]||!op.dims[3]||!op.dims[4]||!op.dims[5]||std::uint64_t(op.dims[3])*op.dims[4]*op.dims[5]>1024||op.dims[6]>65536)throw std::runtime_error("invalid native dispatch");
                length=r.u32();if(!length||length>256)throw std::runtime_error("invalid native arguments");op.arguments=r.bytes(length);
                auto fixups=r.u32();if(fixups>32)throw std::runtime_error("too many native relocations");
                std::vector<unsigned> seen;
                for(unsigned j=0;j<fixups;++j){auto at=r.u32(),index=r.u32();auto offset=r.u64();
                    if(at%8||at>length||length-at<8||std::find(seen.begin(),seen.end(),at)!=seen.end())throw std::runtime_error("invalid native relocation");
                    seen.push_back(at);std::uint64_t previous{};std::memcpy(&previous,op.arguments.data()+at,8);if(previous)throw std::runtime_error("native plan contains an absolute pointer");
                    auto pointer=reference(index,offset);std::memcpy(op.arguments.data()+at,&pointer,8);
                }
            }else throw std::runtime_error("unsupported native operation");
            operations.push_back(std::move(op));
        }
        if(r.offset!=data.size())throw std::runtime_error("trailing native plan data");
        reset();
        check(hipStreamBeginCapture(stream,hipStreamCaptureModeThreadLocal));
        for(auto&op:operations)enqueue(op);
        check(hipStreamEndCapture(stream,&graph));check(hipGraphInstantiate(&executable,graph,nullptr,nullptr,0));
    }
    void controls(float tone,float structure,float skin,bool automatic_mask=true,unsigned style=0,unsigned preset=0,float intensity=1.f){
        if(style>2||preset>3||!std::isfinite(intensity)||intensity<0||intensity>2)throw std::runtime_error("invalid NR style, preset or intensity");
        if(!std::isfinite(tone)||tone<0||tone>2||!std::isfinite(structure)||structure<0||structure>2||!std::isfinite(skin)||skin< -1||skin>2)throw std::runtime_error("invalid NR controls: tone/structure [0,2], skin [-1,2]");
        check(hipStreamSynchronize(stream));
        Operation* pre=nullptr;
        for(auto& op:operations)if(op.name=="_Z10k_swin_varILi32ELb1EEv9VarParams"&&op.arguments.size()==168){
            unsigned flags;std::memcpy(&flags,op.arguments.data()+40,4);
            if(flags==20){if(pre)throw std::runtime_error("ambiguous NR control node");pre=&op;}
        }
        if(!pre)throw std::runtime_error("model has no qualified NR control node");
        // The local weight descriptor declares 3 styles and one effective preset (1).
        // Match NGX: Default/#2/#3 all resolve to the shipping preset 1.
        const auto previous=pre->arguments;const float previous_intensity=nr_intensity;
        if(intensity!=1.f){
            unsigned exports=0;
            for(auto& op:operations)if(op.name=="_Z8k_export12ExportParams"){
                if(op.arguments.size()!=64)throw std::runtime_error("unsupported NR export ABI");
                unsigned fmt{},transfer{},residual{};float exposure{};
                std::memcpy(&fmt,op.arguments.data()+24,4);std::memcpy(&transfer,op.arguments.data()+40,4);
                std::memcpy(&residual,op.arguments.data()+56,4);std::memcpy(&exposure,op.arguments.data()+60,4);
                if(fmt!=1||transfer||residual||exposure!=1.f)throw std::runtime_error("NR intensity requires qualified RGBA8 export");
                unsigned ew{},eh{},stride{};std::uint64_t original{},rendered{};
                std::memcpy(&ew,op.arguments.data()+8,4);std::memcpy(&eh,op.arguments.data()+12,4);std::memcpy(&stride,op.arguments.data()+16,4);
                std::memcpy(&rendered,op.arguments.data(),8);std::memcpy(&original,op.arguments.data()+48,8);
                const auto bounded=[&](std::uint64_t pointer,unsigned components){return std::any_of(buffers.begin(),buffers.end(),[&](const auto& b){const auto base=reinterpret_cast<std::uintptr_t>(b.pointer);return pointer>=base&&pointer-base<=b.size&&std::uint64_t(width)*height*components*4<=b.size-(pointer-base);});};
                if(ew!=width||eh!=height||stride!=width||!bounded(original,3)||!bounded(rendered,4))throw std::runtime_error("invalid NR intensity tensor extent");
                ++exports;
            }
            if(exports!=1)throw std::runtime_error("ambiguous NR export node");
            if(!intensity_frame)check(hipMalloc(reinterpret_cast<void**>(&intensity_frame),std::size_t(width)*height*16));
        }
        nr_intensity=intensity;
        for(auto [offset,value]:{std::pair{84,tone},std::pair{88,structure},std::pair{92,float(style)/128.f},std::pair{96,automatic_mask?(skin<0?structure:skin):-1.f},std::pair{100,automatic_mask?structure:-1.f}})std::memcpy(pre->arguments.data()+offset,&value,4);
        hipGraph_t replacement{};hipGraphExec_t compiled{};bool capturing=false;
        try{
            check(hipStreamBeginCapture(stream,hipStreamCaptureModeThreadLocal));capturing=true;
            for(auto& op:operations)enqueue(op);
            const auto ended=hipStreamEndCapture(stream,&replacement);capturing=false;check(ended);
            check(hipGraphInstantiate(&compiled,replacement,nullptr,nullptr,0));
        }catch(...){
            if(capturing)(void)hipStreamEndCapture(stream,&replacement);
            if(compiled)(void)hipGraphExecDestroy(compiled);if(replacement)(void)hipGraphDestroy(replacement);
            pre->arguments=previous;nr_intensity=previous_intensity;throw;
        }
        (void)hipGraphExecDestroy(executable);(void)hipGraphDestroy(graph);executable=compiled;graph=replacement;
        nr_style=style;nr_preset=preset;
        reset(); // A new conditioning invalidates the recurrent state.
    }
    void profile(const unsigned char* source,std::size_t size,unsigned iterations,const char* report) {
        if(!source || size!=std::size_t(width)*height*4 || iterations<1 || iterations>16 || !report)
            throw std::runtime_error("invalid profiling request");
        struct Instrumentation {
            hipGraph_t graph{};hipGraphExec_t executable{};std::vector<hipEvent_t> events;
            ~Instrumentation(){if(executable)(void)hipGraphExecDestroy(executable);if(graph)(void)hipGraphDestroy(graph);for(auto e:events)if(e)(void)hipEventDestroy(e);}
        } instrument;
        instrument.events.resize(operations.size()*2+2);
        for(auto&e:instrument.events)check(hipEventCreate(&e));
        check(hipStreamBeginCapture(stream,hipStreamCaptureModeThreadLocal));
        for(std::size_t i=0;i<operations.size();++i){
            check(hipEventRecordWithFlags(instrument.events[i*2],stream,hipEventRecordExternal));
            auto&op=operations[i];
            enqueue(op);
            check(hipEventRecordWithFlags(instrument.events[i*2+1],stream,hipEventRecordExternal));
        }
        check(hipStreamEndCapture(stream,&instrument.graph));
        check(hipGraphInstantiate(&instrument.executable,instrument.graph,nullptr,nullptr,0));
        auto begin=instrument.events[instrument.events.size()-2],end=instrument.events.back();
        std::ofstream csv(report);if(!csv)throw std::runtime_error("cannot write profiling CSV");
        csv<<"iteration,index,kind,kernel,gpu_ms,grid_x,grid_y,grid_z,block_x,block_y,block_z,static_shared_bytes,dynamic_shared_bytes\n"<<std::setprecision(9);
        std::vector<std::vector<unsigned char>> expected(iterations,std::vector<unsigned char>(size));
        std::vector<unsigned char> actual(size);
        // Identical initial history and warmup sequence for normal and instrumented graphs.
        for(unsigned pass=0;pass<2;++pass){
            reset();check(hipMemcpyAsync(buffers[input].pointer,source,size,hipMemcpyHostToDevice,stream));
            for(unsigned i=0;i<5;++i)check(hipGraphLaunch(executable,stream));
            check(hipStreamSynchronize(stream));
            for(unsigned frame=0;frame<iterations;++frame){
                check(hipEventRecord(begin,stream));
                check(hipGraphLaunch(pass?instrument.executable:executable,stream));
                check(hipEventRecord(end,stream));check(hipEventSynchronize(end));
                float total{};check(hipEventElapsedTime(&total,begin,end));
                csv<<frame<<",-1,"<<(pass?"instrumented":"baseline")<<",graph,"<<total<<",0,0,0,0,0,0,0,0\n";
                check(hipMemcpy(actual.data(),buffers[output].pointer,size,hipMemcpyDeviceToHost));
                if(!pass)expected[frame]=actual;
                else {
                    if(expected[frame]!=actual)throw std::runtime_error("profiling changed the rendered output");
                    for(std::size_t i=0;i<operations.size();++i){
                        auto&op=operations[i];float ms{};check(hipEventElapsedTime(&ms,instrument.events[i*2],instrument.events[i*2+1]));
                        int static_shared{};if(op.tag==3)check(hipFuncGetAttribute(&static_shared,HIP_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,op.function));
                        csv<<frame<<','<<i<<','<<(op.tag==3?"kernel":"copy")<<','<<(op.tag==3?op.name:"device_to_device")<<','<<ms;
                        for(unsigned j=0;j<6;++j)csv<<','<<op.dims[j];
                        csv<<','<<static_shared<<','<<op.dims[6]<<'\n';
                    }
                }
            }
        }
        if(!csv)throw std::runtime_error("profiling CSV write failed");
    }
    void reset(){for(unsigned i=0;i<buffers.size();++i)if(i!=weights)check(hipMemsetAsync(buffers[i].pointer,0,buffers[i].size,stream));check(hipStreamSynchronize(stream));}
    void run(const unsigned char* source,unsigned char* result,std::size_t size,bool bgra){
        if(!source||!result||size!=std::size_t(width)*height*4)throw std::runtime_error("native frame extent mismatch");
        // Persistent pinned staging avoids pageable-transfer allocations; channel conversion
        // and alpha restoration now run on the GPU, identically to the shared-buffer path.
        if(!host_frame)check(hipHostMalloc(reinterpret_cast<void**>(&host_frame),size));
        if(!device_frame)check(hipMalloc(reinterpret_cast<void**>(&device_frame),size));
        std::memcpy(host_frame,source,size);
        check(hipMemcpyAsync(device_frame,host_frame,size,hipMemcpyHostToDevice,stream));
        run_device(device_frame,size,bgra,255);
        check(hipMemcpyAsync(host_frame,device_frame,size,hipMemcpyDeviceToHost,stream));
        check(hipStreamSynchronize(stream));std::memcpy(result,host_frame,size);
    }
};
}
extern "C" {
const char* ns_nr_error(){return last_error.c_str();}
void* ns_nr_create(const char* image,const char* plan,const char* weights,const char* lookup){try{auto r=std::make_unique<Runtime>();r->initialize(image,plan,weights,lookup);return r.release();}catch(const std::exception&e){last_error=e.what();return nullptr;}}
int ns_nr_run(void* handle,const unsigned char* input,unsigned char* output,std::size_t bytes,int bgra){try{if(!handle)throw std::runtime_error("native runtime is closed");static_cast<Runtime*>(handle)->run(input,output,bytes,bgra!=0);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_reset(void* handle){try{if(!handle)throw std::runtime_error("native runtime is closed");static_cast<Runtime*>(handle)->reset();return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_profile(void* handle,const unsigned char* input,std::size_t bytes,unsigned iterations,const char* report){try{if(!handle)throw std::runtime_error("native runtime is closed");static_cast<Runtime*>(handle)->profile(input,bytes,iterations,report);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_controls_v3(void* handle,float tone,float structure,float skin,unsigned automatic_mask,unsigned style,unsigned preset,float intensity){try{if(!handle||automatic_mask>1)throw std::runtime_error("invalid NR controls request");static_cast<Runtime*>(handle)->controls(tone,structure,skin,automatic_mask!=0,style,preset,intensity);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_controls_v2(void* handle,float tone,float structure,float skin,unsigned automatic_mask){try{if(!handle||automatic_mask>1)throw std::runtime_error("invalid NR controls request");static_cast<Runtime*>(handle)->controls(tone,structure,skin,automatic_mask!=0);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_controls(void* handle,float tone,float structure,float skin){try{if(!handle)throw std::runtime_error("native runtime is closed");static_cast<Runtime*>(handle)->controls(tone,structure,skin);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_device_pci(unsigned char* identity){try{if(!identity)throw std::runtime_error("missing PCI identity");char bus_id[64]{};check(hipDeviceGetPCIBusId(bus_id,sizeof(bus_id),0));std::uint32_t pci[4]{};if(std::sscanf(bus_id,"%x:%x:%x.%x",&pci[0],&pci[1],&pci[2],&pci[3])!=4)throw std::runtime_error("invalid HIP PCI identity");std::memcpy(identity,pci,16);return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_import_frame(void* handle,int fd,std::size_t allocation,std::size_t bytes){try{if(!handle)throw std::runtime_error("native runtime is closed");return static_cast<int>(static_cast<Runtime*>(handle)->import_frame(fd,allocation,bytes));}catch(const std::exception&e){last_error=e.what();return -1;}}
int ns_nr_run_shared(void* handle,unsigned slot,int bgra,unsigned strength){try{if(!handle)throw std::runtime_error("native runtime is closed");auto& r=*static_cast<Runtime*>(handle);if(slot>=r.shared_frames.size())throw std::runtime_error("invalid shared frame slot");r.run_device(r.shared_frames[slot]->pointer,std::size_t(r.width)*r.height*4,bgra!=0,strength);check(hipStreamSynchronize(r.stream));return 0;}catch(const std::exception&e){last_error=e.what();return -1;}}
void ns_nr_destroy(void* handle){delete static_cast<Runtime*>(handle);}
}
