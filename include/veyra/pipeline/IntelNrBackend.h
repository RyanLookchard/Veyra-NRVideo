// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/pipeline/AmdNrBackend.h"
#include <filesystem>
#include <memory>
namespace veyra::pipeline {
// Experimental Intel XMX DLSS-NR: the network runs in the upstream Python
// daemon (Vulkan coopmat on XMX), reached over its protocol-3 D3D12 shared
// buffer/fence transport. Same same-frame linear contract as the AMD
// providers: Veyra owns history/residual/protection; the daemon is told to
// reset every frame (P0 conservative contract, zero motion).
class IntelNrBackend final:public AmdNrBackend {
public:
    IntelNrBackend();
    ~IntelNrBackend() override;
    IntelNrBackend(const IntelNrBackend&)=delete;
    IntelNrBackend& operator=(const IntelNrBackend&)=delete;
    // directory = runtime_local/intel/nr (daemon subset: python/, src/, work/).
    // smallModel selects network B (the faster 310.9.16 preset).
    bool open(const std::filesystem::path&,ID3D12Device*,ID3D12CommandQueue*,uint32_t,uint32_t,bool);
    void close() noexcept;
    bool ready()const override;
    bool admits(uint32_t,uint32_t)const override;
    // proxy = parity sRGB RGBA8 at the internal extent; original = linear
    // RGBA16F at the same extent (COMPOSE restores the HDR excess from it).
    bool recordInputs(ID3D12GraphicsCommandList* list,ID3D12Resource* proxy,ID3D12Resource* original,uint64_t frame,bool reset,float intensity);
    bool recordInputs(ID3D12GraphicsCommandList*,ID3D12Resource*,uint64_t,bool,float) override{return false;}
    bool enqueue() override;
    bool recordOutputs(ID3D12GraphicsCommandList*,ID3D12Resource*) override;
    bool retireSubmitted() override;
    const std::string& error()const override;
    // Daemon subset present (python interpreter, daemon script, libxmx, shaders).
    static bool runtimeAvailable(const std::filesystem::path&);
    // Extracted weights for the chosen preset present (work/mlxw/*.safetensors).
    static bool weightsAvailable(const std::filesystem::path&,bool smallModel);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}