// SPDX-License-Identifier: GPL-3.0-only
// Stub Intel XMX NR backend for builds without the vendored protocol-3
// headers (third_party_local/intel/nr-protocol, gitignored like the other
// SDKs). Reports the runtime unavailable; the capability gate hides nr5.
#include "veyra/pipeline/IntelNrBackend.h"
#include "veyra/Log.h"

namespace veyra::pipeline {
struct IntelNrBackend::Impl { std::string error; };
IntelNrBackend::IntelNrBackend():impl_(std::make_unique<Impl>()){}
IntelNrBackend::~IntelNrBackend()=default;
bool IntelNrBackend::runtimeAvailable(const std::filesystem::path&){return false;}
bool IntelNrBackend::weightsAvailable(const std::filesystem::path&,bool){return false;}
bool IntelNrBackend::open(const std::filesystem::path&,ID3D12Device*,ID3D12CommandQueue*,uint32_t,uint32_t,bool){
    impl_->error="Intel NR protocol headers not present in this build (third_party_local/intel/nr-protocol)";
    log::error("intel-nr",impl_->error);return false;
}
void IntelNrBackend::close()noexcept{}
bool IntelNrBackend::ready()const{return false;}
bool IntelNrBackend::admits(uint32_t,uint32_t)const{return false;}
bool IntelNrBackend::recordInputs(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,uint64_t,bool,float){return false;}
bool IntelNrBackend::enqueue(){return false;}
bool IntelNrBackend::recordOutputs(ID3D12GraphicsCommandList*,ID3D12Resource*){return false;}
bool IntelNrBackend::retireSubmitted(){return true;}
const std::string& IntelNrBackend::error()const{return impl_->error;}
}