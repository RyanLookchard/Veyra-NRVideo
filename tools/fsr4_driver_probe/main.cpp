// FSR4 driver-DLL probe (diagnostic only, never shipped in the player).
//
// Loads amdxcffx64.dll directly (no AMD driver install needed) and forces the
// INT8 Lite model through the RTTI vtable patch of ffxProvider_*::IsSupported
// (the same model gate bypass OptiScaler's master branch uses). Then dispatches
// a real upscale and validates the readback, so the verdict is evidence, not
// the provider's self-report.
//
// Usage: veyra_fsr4_driver_probe [path-to-amdxcffx64.dll] [--no-hook] [--loops N]
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "../../third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/ffx_api.h"
#include "../../third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/ffx_api_types.h"
#include "../../third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "../../third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kRenderWidth = 1920;
constexpr UINT kRenderHeight = 1080;
constexpr UINT kUpscaleWidth = 2560;
constexpr UINT kUpscaleHeight = 1440;

// ---- driver provider ABI (mirrors OptiScaler FSR4DriverProvider.h) ----
struct ProviderInterface {
    uint64_t versionId;
    const char* versionName;
    bool (*canProvide)(uint64_t descType);
    ffxReturnCode_t (*createContext)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
    ffxReturnCode_t (*destroyContext)(ffxContext*, const ffxAllocationCallbacks*);
    ffxReturnCode_t (*configure)(ffxContext*, ffxConfigureDescHeader*);
    ffxReturnCode_t (*query)(ffxContext*, ffxQueryDescHeader*);
    ffxReturnCode_t (*dispatch)(ffxContext*, const ffxDispatchDescHeader*);
};
struct ExternalProviderData {
    uint32_t structVersion;
    uint64_t descType;
    ProviderInterface provider;
    bool (*isSupported)(const void* backend);
    void* reserved;
};
static_assert(offsetof(ExternalProviderData, isSupported) == 0x50);
static_assert(sizeof(ExternalProviderData) == 0x60);
typedef HRESULT(STDMETHODCALLTYPE* PFN_UpdateFfxApiProvider)(void*, uint32_t);

// ---- RTTI vtable finder (mirrors OptiScaler scanner/rtti.cpp) ----
struct TypeDescriptor { void* vtable; void* spare; char name[1]; };
struct CompleteObjectLocator {
    uint32_t signature, offset, constructorDisplacementOffset;
    uint32_t typeDescriptor, classHierarchyDescriptor, self;
};
enum class SectionKind { Code, ReadOnlyData, WritableData };

std::vector<std::span<const uint8_t>> getSections(HMODULE module, SectionKind kind) {
    std::vector<std::span<const uint8_t>> sections;
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        const bool code = section->Characteristics & IMAGE_SCN_MEM_EXECUTE;
        const bool writable = section->Characteristics & IMAGE_SCN_MEM_WRITE;
        const auto sectionKind = code ? SectionKind::Code : (writable ? SectionKind::WritableData : SectionKind::ReadOnlyData);
        if (sectionKind == kind)
            sections.emplace_back(base + section->VirtualAddress, section->Misc.VirtualSize);
    }
    return sections;
}

void** findVtable(HMODULE module, const char* className) {
    const TypeDescriptor* typeDescriptor = nullptr;
    char mangled[256];
    for (auto prefix : {".?AV", ".?AU"}) {
        std::snprintf(mangled, sizeof(mangled), "%s%s@@", prefix, className);
        const size_t needleLen = std::strlen(mangled) + 1;
        for (auto section : getSections(module, SectionKind::WritableData)) {
            std::string_view data(reinterpret_cast<const char*>(section.data()), section.size());
            if (auto position = data.find(std::string_view(mangled, needleLen)); position != std::string_view::npos) {
                auto name = data.data() + position;
                typeDescriptor = reinterpret_cast<const TypeDescriptor*>(name - offsetof(TypeDescriptor, name));
                break;
            }
        }
        if (typeDescriptor) break;
    }
    if (!typeDescriptor) return nullptr;

    auto base = reinterpret_cast<const uint8_t*>(module);
    auto typeDescriptorRva = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(typeDescriptor) - base);
    const CompleteObjectLocator* locator = nullptr;
    auto roSections = getSections(module, SectionKind::ReadOnlyData);
    for (auto section : roSections) {
        for (size_t offset = 0; offset + sizeof(CompleteObjectLocator) <= section.size(); offset += alignof(uint32_t)) {
            auto candidate = reinterpret_cast<const CompleteObjectLocator*>(section.data() + offset);
            auto locatorRva = static_cast<uint32_t>(section.data() + offset - base);
            if (candidate->signature == 1 && candidate->offset == 0 &&
                candidate->typeDescriptor == typeDescriptorRva && candidate->self == locatorRva) {
                locator = candidate;
                break;
            }
        }
        if (locator) break;
    }
    if (!locator) return nullptr;

    for (auto section : roSections) {
        auto entries = reinterpret_cast<const uintptr_t*>(section.data());
        auto count = section.size() / sizeof(uintptr_t);
        for (size_t i = 0; i + 1 < count; i++) {
            if (entries[i] == reinterpret_cast<uintptr_t>(locator))
                return reinterpret_cast<void**>(const_cast<uintptr_t*>(&entries[i + 1]));
        }
    }
    return nullptr;
}

bool patchVtableEntry(void** entry, void* value) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &oldProtection)) return false;
    *entry = value;
    VirtualProtect(entry, sizeof(*entry), oldProtection, &oldProtection);
    return true;
}

// ---- IsSupported hook: force INT8 Lite, block the rest ----
// vtable index 2 is IsSupported(void* device) in FFX SDK 2.0+ providers.
constexpr size_t kIsSupportedIndex = 2;
typedef bool (*PFN_IsSupported)(void* provider, void* device);
struct PatchedProvider { void** vtable; const char* name; bool isLite; PFN_IsSupported original; };
std::vector<PatchedProvider> g_patched;
int g_selectedModel = 3; // FSR4Model order: 0 FP8, 1 INT8, 2 INT8_2, 3 INT8Lite
int g_modelIndex(const char* name) {
    if (std::strcmp(name, "FP8") == 0) return 0;
    if (std::strcmp(name, "INT8") == 0) return 1;
    if (std::strcmp(name, "INT8_2") == 0) return 2;
    return 3;
}
bool __cdecl hkIsSupported(void* provider, void* device) {
    auto vtable = *static_cast<void***>(provider);
    for (auto& patched : g_patched) {
        if (patched.vtable != vtable) continue;
        const bool original = patched.original ? patched.original(provider, device) : false;
        const bool selected = g_modelIndex(patched.name) == g_selectedModel;
        std::printf("[fsr4] IsSupported(%s) original=%d -> forced=%d\n", patched.name, int(original), int(selected));
        return selected;
    }
    return false;
}

void hookModels(HMODULE module) {
    struct Entry { const char* className; const char* label; };
    const Entry entries[] = {
        {"ffxProvider_FSR4", "FP8"},
        {"ffxProvider_FSR4_Int8", "INT8"},
        {"ffxProvider_FSR4_Int8_Navi2", "INT8_2"},
        {"ffxProvider_FSR4Lite_Int8", "INT8Lite"},
    };
    for (auto& entry : entries) {
        auto vtable = findVtable(module, entry.className);
        if (!vtable) {
            std::printf("[fsr4] model %s: vtable not found\n", entry.label);
            continue;
        }
        PatchedProvider patched{vtable, entry.label, g_modelIndex(entry.label) == 3,
                                reinterpret_cast<PFN_IsSupported>(vtable[kIsSupportedIndex])};
        if (patchVtableEntry(&vtable[kIsSupportedIndex], reinterpret_cast<void*>(&hkIsSupported))) {
            g_patched.push_back(patched);
            std::printf("[fsr4] model %s: IsSupported hooked\n", entry.label);
        }
    }
}

// ---- FFX plumbing ----
const char* resultName(ffxReturnCode_t code) {
    switch (code) {
    case FFX_API_RETURN_OK: return "OK";
    case FFX_API_RETURN_ERROR: return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "ERROR_UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "ERROR_RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_PARAMETER: return "ERROR_PARAMETER";
    default: return "OTHER";
    }
}

void ffxMessage(uint32_t type, const wchar_t* message) {
    std::printf("[fsr4-dll] type=%u %ls\n", type, message ? message : L"(null)");
}

FfxApiResource makeResource(ID3D12Resource* resource, uint32_t state) {
    FfxApiResource api{};
    if (!resource) return api;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    api.resource = resource;
    api.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    api.description.format = desc.Format == DXGI_FORMAT_R16G16_FLOAT ? FFX_API_SURFACE_FORMAT_R16G16_FLOAT
        : desc.Format == DXGI_FORMAT_R32_FLOAT ? FFX_API_SURFACE_FORMAT_R32_FLOAT
        : FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    api.description.width = uint32_t(desc.Width);
    api.description.height = desc.Height;
    api.description.depth = desc.DepthOrArraySize;
    api.description.mipCount = desc.MipLevels;
    api.description.flags = 0;
    api.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0)
        api.description.usage = FfxApiResourceUsage(api.description.usage | FFX_API_RESOURCE_USAGE_UAV);
    api.state = state;
    return api;
}

ComPtr<ID3D12Resource> makeTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> resource;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&resource));
    return resource;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&resource));
    return resource;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::filesystem::path dllPath;
    bool hook = true;
    int loops = 3;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--no-hook") == 0) hook = false;
        else if (_wcsicmp(argv[i], L"--loops") == 0 && i + 1 < argc) loops = _wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--model") == 0 && i + 1 < argc) g_selectedModel = _wtoi(argv[++i]);
        else dllPath = argv[i];
    }
    if (dllPath.empty())
        dllPath = std::filesystem::current_path() / L"amdxcffx64.dll";
    dllPath = std::filesystem::absolute(dllPath);
    SetCurrentDirectoryW(dllPath.parent_path().wstring().c_str());

    // The driver DLL validates the FFX SDK it finds in the process. Preload the
    // SDK loader + upscaler beside it (the MOD package ships both); without a
    // signed SDK module the provider query returns E_INVALIDARG.
    for (auto sdkName : {L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll"}) {
        const auto sdkPath = dllPath.parent_path() / sdkName;
        if (std::filesystem::exists(sdkPath)) {
            auto sdk = LoadLibraryExW(sdkPath.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            std::printf("[fsr4] preload %ls -> %p win32=%lu\n", sdkName, static_cast<void*>(sdk),
                        sdk ? 0UL : GetLastError());
        }
    }

    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        std::printf("[fsr4] load failed win32=%lu path=%ls\n", GetLastError(), dllPath.c_str());
        return 2;
    }
    auto updateProvider = reinterpret_cast<PFN_UpdateFfxApiProvider>(GetProcAddress(module, "UpdateFfxApiProvider"));
    if (!updateProvider) {
        std::printf("[fsr4] UpdateFfxApiProvider export missing\n");
        return 3;
    }
    if (hook) {
        hookModels(module);
        // Model selection relies on the IsSupported vtable hook only; a
        // getModelBlob detour (OptiScaler nightly) can be added via Detours later.
    }

    // Query the upscaler provider: version 2 then version 1 (driver ABI).
    ProviderInterface provider{};
    uint32_t usedStructVersion = 0;
    for (uint32_t structVersion : {2u, 1u}) {
        ExternalProviderData data{};
        data.structVersion = structVersion;
        data.descType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        const uint32_t size = structVersion >= 3 ? sizeof(data) : uint32_t(offsetof(ExternalProviderData, isSupported));
        const HRESULT hr = updateProvider(&data, size);
        std::printf("[fsr4] UpdateFfxApiProvider(v%u) hr=0x%lX createContext=%p\n", structVersion,
                    static_cast<unsigned long>(hr), static_cast<void*>(data.provider.createContext));
        if (SUCCEEDED(hr) && data.provider.createContext) {
            provider = data.provider;
            usedStructVersion = structVersion;
            break;
        }
    }
    if (!provider.createContext) {
        std::printf("[fsr4] VERDICT no upscaler provider from the driver DLL\n");
        return 4;
    }
    std::printf("[fsr4] provider version=%s (struct v%u)\n",
                provider.versionName ? provider.versionName : "?", usedStructVersion);

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::printf("[fsr4] factory creation failed\n");
        return 5;
    }
    // Prefer an Intel adapter (the FSR4-on-Arc question); the first non-software
    // adapter is only a fallback for the NVIDIA baseline run.
    ComPtr<IDXGIAdapter1> fallback;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        std::wprintf(L"[fsr4] adapter[%u]=%s vendor=0x%04X\n", i, desc.Description, desc.VendorId);
        if (desc.VendorId == 0x8086) { adapter = candidate; break; }
        if (!fallback) fallback = candidate;
    }
    if (!adapter) adapter = fallback;
    if (!adapter) {
        std::printf("[fsr4] adapter selection failed\n");
        return 5;
    }
    DXGI_ADAPTER_DESC1 adapterDesc{};
    adapter->GetDesc1(&adapterDesc);
    std::wprintf(L"[fsr4] adapter=%s vendor=0x%04X\n", adapterDesc.Description, adapterDesc.VendorId);

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::printf("[fsr4] D3D12CreateDevice failed\n");
        return 6;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return 7;

    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = device.Get();
    ffxCreateContextDescUpscaleVersion versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale upscaleDesc{};
    upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext = &backendDesc.header;
    backendDesc.header.pNext = &versionDesc.header;
    upscaleDesc.flags = 0;
    upscaleDesc.maxRenderSize = {kRenderWidth, kRenderHeight};
    upscaleDesc.maxUpscaleSize = {kUpscaleWidth, kUpscaleHeight};
    upscaleDesc.fpMessage = ffxMessage;
    ffxContext context = nullptr;
    auto createResult = provider.createContext(&context, &upscaleDesc.header, nullptr);
    std::printf("[fsr4] CreateContext(version=%u) result=%s context=%p\n", versionDesc.version,
                resultName(createResult), static_cast<void*>(context));
    if (createResult != FFX_API_RETURN_OK || !context) {
        backendDesc.header.pNext = nullptr;
        createResult = provider.createContext(&context, &upscaleDesc.header, nullptr);
        std::printf("[fsr4] CreateContext(no version desc) result=%s context=%p\n",
                    resultName(createResult), static_cast<void*>(context));
    }
    if (createResult != FFX_API_RETURN_OK || !context) {
        std::printf("[fsr4] VERDICT context creation failed\n");
        return 8;
    }
    if (provider.configure) {
        ffxConfigureDescGlobalDebug1 global{};
        global.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        global.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        global.fpMessage = ffxMessage;
        provider.configure(&context, &global.header);
    }

    ComPtr<ID3D12Resource> color = makeTexture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D12Resource> output = makeTexture(device.Get(), kUpscaleWidth, kUpscaleHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D12Resource> motion = makeTexture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R16G16_FLOAT);
    ComPtr<ID3D12Resource> depth = makeTexture(device.Get(), kRenderWidth, kRenderHeight, DXGI_FORMAT_R32_FLOAT);
    if (!color || !output || !motion || !depth) return 9;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT colorFootprint{}, readFootprint{};
    UINT rows = 0; UINT64 rowSize = 0, colorTotal = 0, readTotal = 0;
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    device->GetCopyableFootprints(&colorDesc, 0, 1, 0, &colorFootprint, &rows, &rowSize, &colorTotal);
    device->GetCopyableFootprints(&outputDesc, 0, 1, 0, &readFootprint, &rows, &rowSize, &readTotal);
    ComPtr<ID3D12Resource> upload = makeBuffer(device.Get(), colorTotal, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> readback = makeBuffer(device.Get(), readTotal, D3D12_HEAP_TYPE_READBACK);
    if (!upload || !readback) return 10;
    {
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) return 11;
        auto* base = static_cast<uint8_t*>(mapped) + colorFootprint.Offset;
        for (UINT y = 0; y < kRenderHeight; ++y) {
            uint8_t* row = base + size_t(y) * colorFootprint.Footprint.RowPitch;
            for (UINT x = 0; x < kRenderWidth; ++x) {
                const bool light = ((x / 64) + (y / 64)) % 2 == 0;
                const uint8_t ramp = uint8_t(x * 255 / (kRenderWidth - 1));
                row[size_t(x) * 4 + 0] = light ? ramp : uint8_t(255 - ramp);
                row[size_t(x) * 4 + 1] = light ? 255 : 0;
                row[size_t(x) * 4 + 2] = light ? 0 : 255;
                row[size_t(x) * 4 + 3] = 255;
            }
        }
        upload->Unmap(0, nullptr);
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 12;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = color.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = colorFootprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Committed resources are not guaranteed zero: clear motion/depth explicitly
    // so the static-scene guidance is deterministic.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> clearHeap;
        if (SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&clearHeap)))) {
            const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = clearHeap->GetCPUDescriptorHandleForHeapStart();
            auto gpu = clearHeap->GetGPUDescriptorHandleForHeapStart();
            int slot = 0;
            for (auto* guidance : {motion.Get(), depth.Get()}) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Format = guidance->GetDesc().Format;
                D3D12_CPU_DESCRIPTOR_HANDLE c{cpu.ptr + SIZE_T(slot) * inc};
                device->CreateUnorderedAccessView(guidance, nullptr, &uav, c);
                transition(list.Get(), guidance, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                const UINT zeros[4] = {0, 0, 0, 0};
                D3D12_GPU_DESCRIPTOR_HANDLE g{gpu.ptr + UINT64(slot) * inc};
                list->ClearUnorderedAccessViewUint(g, c, guidance, zeros, 0, nullptr);
                transition(list.Get(), guidance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ++slot;
            }
        }
    }
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxReturnCode_t dispatchResult = FFX_API_RETURN_ERROR;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double totalMs = 0.0;
    for (int frame = 0; frame < loops; ++frame) {
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = list.Get();
        dispatch.color = makeResource(color.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.depth = makeResource(depth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.motionVectors = makeResource(motion.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.output = makeResource(output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {0.0f, 0.0f};
        dispatch.motionVectorScale = {1.0f, 1.0f};
        dispatch.renderSize = {kRenderWidth, kRenderHeight};
        dispatch.upscaleSize = {kUpscaleWidth, kUpscaleHeight};
        dispatch.enableSharpening = false;
        dispatch.sharpness = 0.0f;
        dispatch.frameTimeDelta = 16.6f;
        dispatch.preExposure = 1.0f;
        dispatch.reset = frame == 0;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.0f;
        dispatch.viewSpaceToMetersFactor = 0.0f;
        dispatch.flags = 0;
        QueryPerformanceCounter(&t0);
        dispatchResult = provider.dispatch(&context, &dispatch.header);
        QueryPerformanceCounter(&t1);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        totalMs += ms;
        std::printf("[fsr4] dispatch frame=%d result=%s recordTime=%.3fms\n", frame, resultName(dispatchResult), ms);
        if (dispatchResult != FFX_API_RETURN_OK) break;
    }

    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION outDst{};
    outDst.pResource = readback.Get();
    outDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    outDst.PlacedFootprint = readFootprint;
    D3D12_TEXTURE_COPY_LOCATION outSrc{};
    outSrc.pResource = output.Get();
    outSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&outDst, 0, 0, 0, &outSrc, nullptr);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    LARGE_INTEGER g0, g1;
    QueryPerformanceCounter(&g0);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fenceEvent);
    const DWORD waitResult = WaitForSingleObject(fenceEvent, 30000);
    QueryPerformanceCounter(&g1);
    std::printf("[fsr4] gpuWall=%.3fms (execute+wait+readback, all %d frames)\n",
                double(g1.QuadPart - g0.QuadPart) * 1000.0 / double(freq.QuadPart), loops);
    if (waitResult != WAIT_OBJECT_0) {
        std::printf("[fsr4] VERDICT GPU fence timed out; readback skipped\n");
        provider.destroyContext(&context, nullptr);
        CloseHandle(fenceEvent);
        return 1;
    }

    double mean = 0.0;
    int distinct = 0;
    bool structureOk = false;
    void* mapped = nullptr;
    if (SUCCEEDED(readback->Map(0, nullptr, &mapped))) {
        const uint8_t* pixels = static_cast<const uint8_t*>(mapped) + readFootprint.Offset;
        std::vector<uint8_t> histogram(256, 0);
        for (UINT y = 0; y < kUpscaleHeight; ++y) {
            const uint8_t* row = pixels + size_t(y) * readFootprint.Footprint.RowPitch;
            for (UINT x = 0; x < kUpscaleWidth; ++x) {
                const uint8_t luma = row[size_t(x) * 4 + 1];
                mean += luma;
                histogram[luma] = 1;
            }
        }
        mean /= double(kUpscaleWidth) * double(kUpscaleHeight);
        distinct = int(std::count(histogram.begin(), histogram.end(), uint8_t(1)));
        const auto sample = [&](UINT x, UINT y) {
            const uint8_t* row = pixels + size_t(y) * readFootprint.Footprint.RowPitch;
            return int(row[size_t(x) * 4 + 1]);
        };
        double lightSum = 0.0, darkSum = 0.0;
        int lightCount = 0, darkCount = 0;
        for (UINT ry = 32; ry < kRenderHeight; ry += 64) {
            for (UINT rx = 32; rx < kRenderWidth; rx += 64) {
                const bool light = ((rx / 64) + (ry / 64)) % 2 == 0;
                const int green = sample(UINT(rx * double(kUpscaleWidth) / kRenderWidth),
                                         UINT(ry * double(kUpscaleHeight) / kRenderHeight));
                if (light) { lightSum += green; ++lightCount; } else { darkSum += green; ++darkCount; }
            }
        }
        structureOk = lightCount > 0 && darkCount > 0 && (lightSum / lightCount - darkSum / darkCount) > 100.0;
        readback->Unmap(0, nullptr);
    }
    std::printf("[fsr4] readback mean=%.2f distinctLevels=%d checkerStructure=%s avgRecord=%.3fms\n", mean, distinct,
                structureOk ? "ok" : "unexpected", loops > 0 ? totalMs / loops : 0.0);
    provider.destroyContext(&context, nullptr);
    if (fenceEvent) CloseHandle(fenceEvent);

    const bool ok = dispatchResult == FFX_API_RETURN_OK && mean > 1.0 && distinct > 4 && structureOk;
    std::printf("[fsr4] VERDICT %s\n", ok ? "FSR4 upscale produced a real image on this adapter"
                                          : "no proof of a correct upscaled image");
    return ok ? 0 : 1;
}
