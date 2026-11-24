// SPDX-License-Identifier: GPL-3.0-only
// Experimental Intel XMX DLSS-NR worker. The network runs in the upstream
// Python daemon (dlss-nr-on-intel, Apache-2.0, vendored subset under
// runtime_local/intel/nr); this backend is its protocol-3 D3D12 client:
// shared input/output buffers + one shared fence, named-pipe control.
// P0 conservative contract: zero motion, per-frame history reset (RESET_HERE
// every frame), Veyra owns residual/protection/history. The wire carries the
// graph's parity proxy bytes (sRGB RGBA8); COMPOSE decodes the answer back to
// linear and restores the HDR excess, mirroring ParityDecode's luminance rule.
#include "veyra/pipeline/IntelNrBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/Log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>
#include <vector>

// Vendored upstream protocol headers (Apache-2.0 notice in
// third_party_local/intel/nr-protocol/LICENSE). nr_gpu_d3d12.h is C with
// COBJMACROS; in C++ the COM interfaces are classes, so include it as C++
// without COBJMACROS and provide the C-style macro shims it calls.
#include "nr_gpu_wire.h"
#include "nr_gpu_rpc.h"
#define ID3D12Device_CreateCommittedResource(d,h,f,s,st,opt,i,p) (d)->CreateCommittedResource(h,f,s,st,opt,*(i),(p))
#define ID3D12Device_CreateSharedHandle(d,o,a,m,n,h) (d)->CreateSharedHandle(o,a,m,n,h)
#define ID3D12Device_CreateFence(d,v,f,i,p) (d)->CreateFence(v,f,*(i),(p))
#define ID3D12Resource_Release(r) (r)->Release()
#define ID3D12Fence_Release(f) (f)->Release()
#include "nr_gpu_d3d12.h"

namespace veyra::pipeline {
namespace {
bool hrFailed(int line,HRESULT hr){if(SUCCEEDED(hr))return false;log::error("intel-nr",std::format("line={} hr=0x{:08X}",line,unsigned(hr)));return true;}
void drop(HANDLE& h){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=nullptr;}
void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&v);
}
constexpr auto kCommon=D3D12_RESOURCE_STATE_COMMON;
constexpr auto kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kUav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
bool fileExists(const std::filesystem::path& p){std::error_code ec;return std::filesystem::is_regular_file(p,ec);}
}

struct IntelNrBackend::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    nr_gpu_d3d12 session{};
    ComPtr<ID3D12Resource> decoded;
    ComputePass encode,compose;DescriptorStager views;
    ID3D12Resource* boundProxy=nullptr;ID3D12Resource* boundOriginal=nullptr;
    HANDLE process=nullptr,job=nullptr;
    std::string pipeName;
    std::jthread monitor;HANDLE stop=nullptr;
    std::atomic<bool> failed=false,closing=false;
    std::atomic<uint64_t> queued=0,queuedAt=0;
    // First frame after open pays model load + shader JIT (7-10 s at 1080p).
    // Keep the RPC and GPU-job deadlines raised until the first frame lands.
    std::atomic<DWORD> rpcTimeoutMs=30000;
    uint32_t w=0,h=0;
    bool opened=false,prepared=false,enqueued=false,outputs=false,bound=false;
    float intensity=1;uint64_t frameValue=0;
    std::filesystem::path settingsPath;
    int knobsStyle=-1;float knobsTone=-1,knobsStructure=-1,knobsIntensity=-1;
    std::string error;
    bool fail(const std::string& m){error=m;failed=true;log::error("intel-nr",m);return false;}
    bool healthy()const{return opened&&!failed.load()&&process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
    // The daemon imports our shared handles by NT handle value in OUR process
    // (OpenProcess(pid, DUP_HANDLE) + DuplicateHandle); the pipe authenticates
    // the client by its real PID. No handle inheritance needed.
    bool rpc(uint32_t action,uint64_t value,DWORD& status){
        nr_gpu_packet packet{};nr_gpu_reply reply{};
        nr_gpu_d3d12_packet(&session,action,value,&packet);
        if(action==NR_GPU_FRAME)nr_gpu_d3d12_tell_reset(&packet,1); // P0: reset every frame
        DWORD peer=0;
        nr_gpu_rpc_timeout_ms=rpcTimeoutMs.load();
        if(!nr_gpu_rpc_peer(pipeName.c_str(),&packet,&reply,&peer))
            return fail(std::format("daemon pipe exchange failed win32={}",GetLastError()));
        status=reply.status;
        return true;
    }
    void watch(){
        while(WaitForSingleObject(stop,50)==WAIT_TIMEOUT){
            const auto q=queued.load();const auto at=queuedAt.load();
            const bool exited=WaitForSingleObject(process,0)!=WAIT_TIMEOUT;
            if(exited&&closing.load())break;
            const bool expired=q&&at&&session.fence->GetCompletedValue()<q+1&&GetTickCount64()-at>rpcTimeoutMs.load()+5000;
            if(!exited&&!expired)continue;
            failed=true;
            log::error("intel-nr",std::format("daemon GPU job exceeded {}ms; stopping only this worker",rpcTimeoutMs.load()+5000));
            if(!exited){TerminateProcess(process,125);WaitForSingleObject(process,1000);}
            // Never leave the shared D3D12 queue waiting for a dead daemon:
            // signal the fence past the pending V+1 ourselves (we own it).
            const auto hr=session.fence->Signal(UINT64_MAX-1);
            if(hrFailed(__LINE__,hr))log::error("intel-nr",std::format("failed to release daemon wait hr=0x{:X}",unsigned(hr)));
            break;
        }
    }
};

IntelNrBackend::IntelNrBackend():impl_(std::make_unique<Impl>()){}
IntelNrBackend::~IntelNrBackend(){close();}

bool IntelNrBackend::runtimeAvailable(const std::filesystem::path& p){
    for(const auto* n:{L"python/python.exe",L"src/layer/nr_daemon.py",L"work/libxmx.dll",L"work/gemm_coopmat.spv",L"work/attention.spv"})
        if(!fileExists(p/n))return false;
    return true;
}
bool IntelNrBackend::weightsAvailable(const std::filesystem::path& p,bool smallModel){
    return fileExists(p/L"work"/L"mlxw"/(smallModel?L"dlssnr-b-logical.safetensors":L"dlssnr-logical.safetensors"));
}

bool IntelNrBackend::open(const std::filesystem::path& directory,ID3D12Device* device,ID3D12CommandQueue* queue,uint32_t w,uint32_t h,bool smallModel){
    close();impl_=std::make_unique<Impl>();auto& s=*impl_;
    log::info("intel-nr",std::format("open requested dir={} {}x{} model={}",directory.string(),w,h,smallModel?"B":"A"));
    if(!device||!queue||!directory.is_absolute()||!w||!h||uint64_t(w)*h>(1u<<28))return s.fail("invalid Intel NR device, queue, path or extent");
    if(!runtimeAvailable(directory))return s.fail("Intel NR daemon subset is incomplete; install python/, src/ and work/ under runtime_local/intel/nr");
    if(!weightsAvailable(directory,smallModel))return s.fail(std::format("Intel NR weights missing (work/mlxw/{}); extract them from your own nvngx_dlssnr.dll first",smallModel?"dlssnr-b-logical.safetensors":"dlssnr-logical.safetensors"));
    s.device=device;s.queue=queue;s.w=w;s.h=h;
    // Protocol-3 session: shared input (proxy bytes, no motion room in P0),
    // shared output (answer bytes + stamp), one shared fence. VkFormat 37 =
    // R8G8B8A8_UNORM, the wire format the daemon's SDR path reads.
    const auto hr=nr_gpu_d3d12_open(&s.session,device,w,h,37,(uint64_t(GetTickCount64())<<16)|GetCurrentProcessId(),0);
    if(hrFailed(__LINE__,hr))return s.fail(std::format("Intel NR shared session allocation hr=0x{:X}",unsigned(hr)));
    s.decoded=makeTexture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    if(!s.decoded)return s.fail("Intel NR decoded texture allocation failed");
    std::vector<uint8_t> code;
    if(!s.encode.loadShader("IntelNrEncode.dxil",code))return s.fail("Intel NR encode shader missing");
    if(!s.encode.create(device,code,2,1,1))return s.fail("Intel NR encode pipeline create failed");
    if(!s.compose.loadShader("IntelNrCompose.dxil",code))return s.fail("Intel NR compose shader missing");
    if(!s.compose.create(device,code,5,3,1))return s.fail("Intel NR compose pipeline create failed");
    if(!s.views.initialize(device,8))return s.fail("Intel NR descriptor heap init failed");
    s.pipeName=std::format("\\\\.\\pipe\\veyra-intel-nr-{}-{}",GetCurrentProcessId(),uintptr_t(device));
    const auto python=directory/L"python"/L"python.exe";
    const auto daemon=directory/L"src"/L"layer"/L"nr_daemon.py";
    const auto pipeW=std::wstring(s.pipeName.begin(),s.pipeName.end());
    s.settingsPath=directory/L"work"/L"veyra-nr-settings.json";
    auto command=std::format(L"\"{}\" -X utf8 -B \"{}\" --socket \"{}\" --root \"{}\" --preset {} --render-scale 1.0 --timeout 3600 --settings \"{}\"",
        python.wstring(),daemon.wstring(),pipeW,directory.wstring(),smallModel?L"B":L"A",s.settingsPath.wstring());
    // The daemon reads NR_ROOT before argparse; NR_LAYER_SPAWNED ends it with
    // Veyra; the GPU transport is opt-in via NR_GPU_IMAGE/NR_GPU_TRANSPORT.
    // Build the CREATE_UNICODE_ENVIRONMENT block by hand: a wide string literal
    // stops at the first embedded NUL, so std::format would truncate the block
    // to NR_GPU_IMAGE=1 and the daemon would refuse the GPU transport.
    std::wstring env;
    auto addEnv=[&env](const std::wstring& kv){env+=kv;env+=L'\0';};
    addEnv(L"NR_GPU_IMAGE=1");
    addEnv(L"NR_GPU_TRANSPORT=1");
    addEnv(L"NR_GPU_ONLY=1");
    addEnv(std::format(L"NR_LAYER_SPAWNED={}",GetCurrentProcessId()));
    addEnv(std::format(L"NR_PRESET={}",smallModel?L"B":L"A"));
    addEnv(std::format(L"NR_ROOT={}",directory.wstring()));
    env+=L'\0'; // double-NUL terminator
    std::vector<wchar_t> envBlock(env.begin(),env.end());
    // Capture the daemon's stdout/stderr so XMX/driver failures are diagnosable
    // (the daemon prints its real error there; CREATE_NO_WINDOW discards it).
    const auto logPath=directory/L"work"/L"daemon.log";
    {std::error_code ec;std::filesystem::create_directories(directory/L"work",ec);}
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE logFile=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    if(logFile!=INVALID_HANDLE_VALUE){
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdOutput=logFile;startup.hStdError=logFile;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,envBlock.data(),directory.c_str(),&startup,&process)!=FALSE;
    if(logFile!=INVALID_HANDLE_VALUE)CloseHandle(logFile);
    if(!created)return s.fail(std::format("Intel NR daemon launch failed win32={}",GetLastError()));
    s.process=process.hProcess;drop(process.hThread);
    s.job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!s.job||!SetInformationJobObject(s.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||!AssignProcessToJobObject(s.job,s.process)){
        TerminateProcess(s.process,126);return s.fail("Intel NR daemon could not be confined to its owned job");
    }
    // HELLO imports the shared buffers/fence in the daemon; the model load can
    // take a while. Retry until the daemon answers or exits (bounded ~3 min).
    bool hello=false;
    for(unsigned attempt=0;attempt<180&&!s.closing.load();++attempt){
        if(WaitForSingleObject(s.process,0)!=WAIT_TIMEOUT)return s.fail("Intel NR daemon exited during startup; see its log");
        DWORD st=NR_GPU_FAILURE;
        if(s.rpc(NR_GPU_HELLO,0,st)&&st==NR_GPU_SUCCESS){hello=true;break;}
        Sleep(1000);
    }
    if(!hello)return s.fail("Intel NR daemon did not accept the GPU session (weights/driver/XMX: check the daemon log)");
    s.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!s.stop)return s.fail("Intel NR monitor event failed");
    s.monitor=std::jthread([&s]{s.watch();});s.opened=true;
    // rpcTimeoutMs starts at 30000 (Impl field) so the first FRAME's model
    // load + shader JIT fits; enqueue() drops it to 10000 after the first
    // successful frame.
    log::info("intel-nr",std::format("daemon ready pid={} model={} internal={}x{} protocol-3 same-frame GPU interop; Veyra owns history",process.dwProcessId,smallModel?"B":"A",w,h));
    return true;
}
bool IntelNrBackend::ready()const{return impl_->opened&&!impl_->failed.load();}
bool IntelNrBackend::admits(uint32_t w,uint32_t h)const{return ready()&&w==impl_->w&&h==impl_->h;}
const std::string& IntelNrBackend::error()const{static const std::string dead="Intel NR daemon failed; see intel-nr log";return impl_->failed&&impl_->error.empty()?dead:impl_->error;}

bool IntelNrBackend::recordInputs(ID3D12GraphicsCommandList* list,ID3D12Resource* proxy,ID3D12Resource* original,uint64_t frame,bool reset,float intensity){
    auto& s=*impl_;if(!ready()||!s.healthy()||s.prepared||!list||!proxy||!original||!frame||!std::isfinite(intensity))return s.fail("invalid/failed Intel NR source transaction");
    const auto d=proxy->GetDesc();if(d.Width!=s.w||d.Height!=s.h||d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return s.fail("Intel NR needs the parity proxy at the internal extent (RGBA8)");
    const auto o=original->GetDesc();if(o.Width!=s.w||o.Height!=s.h||o.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return s.fail("Intel NR needs the linear original at the internal extent");
    if(s.bound&&(s.boundProxy!=proxy||s.boundOriginal!=original))return s.fail("Intel NR layer input resource changed without rebuilding the provider");
    if(!s.bound){
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        srv.Format=proxy->GetDesc().Format;s.views.stageSrv(proxy,&srv,s.encode.heap.Get(),0);
        // ENCODE UAV: the shared input buffer, raw bytes.
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;u.Buffer.NumElements=UINT(s.session.input_alloc/4);
        auto c=s.encode.heap->GetCPUDescriptorHandleForHeapStart();c.ptr+=s.encode.increment;
        s.device->CreateUnorderedAccessView(s.session.input,nullptr,&u,c);
        // COMPOSE: t1 raw output buffer SRV, t2 original linear SRV, u1 decoded UAV.
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;raw.Buffer.NumElements=UINT(s.session.output_alloc/4);
        s.views.stageSrv(s.session.output,&raw,s.compose.heap.Get(),1);
        srv.Format=original->GetDesc().Format;s.views.stageSrv(original,&srv,s.compose.heap.Get(),2);
        D3D12_UNORDERED_ACCESS_VIEW_DESC du{};du.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;du.Format=s.decoded->GetDesc().Format;
        auto dc=s.compose.heap->GetCPUDescriptorHandleForHeapStart();dc.ptr+=3*s.compose.increment;
        s.device->CreateUnorderedAccessView(s.decoded.Get(),nullptr,&du,dc);
        s.bound=true;s.boundProxy=proxy;s.boundOriginal=original;
    }
    // ENCODE: proxy sRGB bytes -> shared input buffer. The graph leaves the
    // proxy in NON_PIXEL_SHADER_RESOURCE (its own encode just read/wrote it);
    // the shared input buffer is COMMON between frames.
    barrier(list,s.session.input,kCommon,kUav);
    const uint32_t pitch=s.w*4;
    float constants[8];memcpy(constants+0,&s.w,4);memcpy(constants+1,&s.h,4);memcpy(constants+2,&pitch,4);
    constants[3]=0;constants[4]=0;constants[5]=0;constants[6]=0;constants[7]=0;
    const auto gpu=s.encode.heap->GetGPUDescriptorHandleForHeapStart();
    s.encode.bind(list,constants,gpu.ptr,gpu.ptr+s.encode.increment);
    list->Dispatch((s.w+15)/16,(s.h+15)/16,1);
    barrier(list,s.session.input,kUav,kCommon);
    (void)reset; // P0: every frame is a reset, told in the FRAME packet
    s.intensity=std::clamp(intensity,0.f,1.f);
    s.prepared=true;s.enqueued=s.outputs=false;
    return true;
}
bool IntelNrBackend::enqueue(){
    auto& s=*impl_;if(!ready()||!s.prepared||s.enqueued||!s.healthy())return s.fail("Intel NR enqueue without a prepared source");
    s.frameValue=nr_gpu_d3d12_next(&s.session);
    const auto hr=s.queue->Signal(s.session.fence,s.frameValue);if(hrFailed(__LINE__,hr))return s.fail(std::format("Intel NR producer signal hr=0x{:X}",unsigned(hr)));
    DWORD status=NR_GPU_FAILURE;
    if(!s.rpc(NR_GPU_FRAME,s.frameValue,status))return false;
    if(status!=NR_GPU_SUCCESS)return s.fail(std::format("Intel NR daemon refused frame status={}",status));
    s.queued=s.frameValue;s.queuedAt=GetTickCount64();
    const auto wait=s.queue->Wait(s.session.fence,s.frameValue+1);if(hrFailed(__LINE__,wait))return s.fail(std::format("Intel NR consumer wait hr=0x{:X}",unsigned(wait)));
    s.enqueued=true;
    // First frame succeeded; subsequent frames use the normal 10 s deadline.
    s.rpcTimeoutMs=NR_GPU_RPC_TIMEOUT_MS;
    return true;
}
bool IntelNrBackend::recordOutputs(ID3D12GraphicsCommandList* list,ID3D12Resource* destination){
    auto& s=*impl_;if(!ready()||!s.enqueued||s.outputs||!list||!destination)return s.fail("Intel NR output without current-frame evaluation");
    barrier(list,s.decoded.Get(),kCommon,kUav);
    // The graph leaves layer->input() in NON_PIXEL_SHADER_RESOURCE after its
    // own encode read it; do not re-transition from COMMON (invalid barrier).
    const uint32_t lo=uint32_t(s.frameValue),hi=uint32_t(s.frameValue>>32),pitch=s.w*4;
    float constants[8];memcpy(constants+0,&s.w,4);memcpy(constants+1,&s.h,4);memcpy(constants+2,&pitch,4);memcpy(constants+3,&lo,4);memcpy(constants+4,&hi,4);
    constants[5]=s.intensity;constants[6]=0;constants[7]=0;
    const auto gpu=s.compose.heap->GetGPUDescriptorHandleForHeapStart();
    s.compose.bind(list,constants,gpu.ptr,gpu.ptr+3*s.compose.increment);
    list->Dispatch((s.w+15)/16,(s.h+15)/16,1);
    // original stays in NON_PIXEL_SHADER_RESOURCE for the graph's next use.
    barrier(list,s.decoded.Get(),kUav,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(destination,s.decoded.Get());
    barrier(list,s.decoded.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,kCommon);
    s.outputs=true;return true;
}
bool IntelNrBackend::retireSubmitted(){auto& s=*impl_;if(!s.outputs)return true;if(!ready())return false;s.prepared=s.enqueued=s.outputs=false;return true;}
void IntelNrBackend::setKnobs(int style,float tone,float structure,float intensity){
    auto& s=*impl_;if(!s.opened||s.settingsPath.empty())return;
    if(style==s.knobsStyle&&tone==s.knobsTone&&structure==s.knobsStructure&&intensity==s.knobsIntensity)return;
    s.knobsStyle=style;s.knobsTone=tone;s.knobsStructure=structure;s.knobsIntensity=intensity;
    // The daemon's profile table: style 0/1/2 = standard/natural/cinematic.
    // intensity stays 1.0 here: the user's strength is applied once, in the
    // COMPOSE shader's lerp. Writing it here too would square it (0.5 -> 0.25).
    const char* profile=style==1?"natural":style==2?"cinematic":"standard";
    const auto json=std::format("{{\"profile\":\"{}\",\"intensity\":1.0,\"detail_strength\":{:.4f},\"colour_strength\":1.0}}",
        profile,std::clamp(structure,0.f,2.f));
    std::ofstream out(s.settingsPath,std::ios::binary|std::ios::trunc);
    if(out){out<<json;out.close();log::info("intel-nr",std::format("knobs written profile={} intensity={:.3f} detail={:.3f}",profile,intensity,structure));}
    (void)tone; // tone maps to the daemon's local_tone_strength via profile; kept for future knob
}
void IntelNrBackend::close()noexcept{
    if(!impl_)return;auto& s=*impl_;
    s.closing=true;
    if(s.process&&WaitForSingleObject(s.process,0)==WAIT_TIMEOUT){
        DWORD status=0;s.rpc(NR_GPU_RELEASE,0,status);
        if(WaitForSingleObject(s.process,3000)==WAIT_TIMEOUT){log::warn("intel-nr","daemon close timed out; terminating owned daemon");TerminateProcess(s.process,125);WaitForSingleObject(s.process,1000);}
    }
    if(s.stop)SetEvent(s.stop);if(s.monitor.joinable())s.monitor.join();
    if(s.session.fence&&s.queued.load())s.session.fence->Signal(UINT64_MAX-1);
    nr_gpu_d3d12_close(&s.session);
    drop(s.process);drop(s.job);drop(s.stop);s.opened=false;
}
}