// Direct3D 12 backend.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <vector>

#include "cloud_noise.h"
#include "mesh_weld.h"
#include "chunk_faces.h"
#include "renderer.h"
extern float g_brightness; // main.cpp: user brightness multiplier
extern float g_sceneExposure; // main.cpp: the scene's fixed exposure (Scene::exposure), 0 = auto

using Microsoft::WRL::ComPtr;

namespace tbd {
namespace {

ID3D12Device* g_logDevice = nullptr; // for CHECK: a failed call is explained by why the device is gone, if it is
void LogIfDeviceLost() {
    if (!g_logDevice) return;
    const HRESULT reason = g_logDevice->GetDeviceRemovedReason();
    if (reason != S_OK) Log("D3D12: the device is lost, removed reason 0x%08X", (unsigned)reason);
}

#define CHECK(expr)                                                              \
    do {                                                                         \
        HRESULT hr_ = (expr);                                                    \
        if (FAILED(hr_)) {                                                       \
            Log("D3D12: %s failed (0x%08X)", #expr, (unsigned)hr_);              \
            LogIfDeviceLost();                                                   \
            return false;                                                        \
        }                                                                        \
    } while (0)

constexpr UINT kBackBuffers = 2;
constexpr UINT kFramesInFlight = 2;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr size_t kTransientBytes = 16 * 1024 * 1024;
constexpr INT kShadowDepthBias = 2000;        // in units of the depth buffer's resolution (float: 2^-24 near z = 1)
constexpr float kShadowSlopeBias = 2.0f;
constexpr INT kTightDepthBias = 1000;         // the tight map's depth range is meters, not hundreds of them, so far less bias
constexpr float kTightSlopeBias = 1.5f;

struct PushConstants {
    float mvp[16];
    float model[16];
};
static_assert(sizeof(PushConstants) == 128, "32 root constants");


// fxc takes over a minute for all the shaders, so compiled blobs are kept on disk next to the executable, named by the hash of all the
// shader sources, the entry point, the target and the defines. PrecompileD3D12Shaders fills the cache on all cores before the renderer starts.
namespace {
uint64_t HashBytes(uint64_t h, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
bool ReadAllText(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    out.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}
std::atomic<bool> g_shaderHashStale{true}; // ReloadShaders sets it after the sources change
uint64_t ShaderSourcesHash() {
    static std::mutex lock;
    static uint64_t hash = 0;
    std::lock_guard<std::mutex> guard(lock);
    if (g_shaderHashStale.exchange(false)) hash = [] {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        const std::wstring dir = ExeDirectory() + L"\\shaders\\";
        HANDLE find = FindFirstFileW((dir + L"*.hlsl*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName); while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end());
        uint64_t h = 14695981039346656037ull;
        for (const std::wstring& name : names) {
            h = HashBytes(h, name.data(), name.size() * sizeof(wchar_t));
            std::string text;
            if (ReadAllText(dir + name, text)) h = HashBytes(h, text.data(), text.size());
        }
        return h;
    }();
    return hash;
}
HRESULT CompileCached(const std::wstring& path, const D3D_SHADER_MACRO* defines, const char* entry, const char* target, ComPtr<ID3DBlob>& out, ComPtr<ID3DBlob>& error) {
    uint64_t h = ShaderSourcesHash();
    h = HashBytes(h, path.data() + path.find_last_of(L"\\/"), (path.size() - path.find_last_of(L"\\/")) * sizeof(wchar_t));
    h = HashBytes(h, entry, strlen(entry));
    h = HashBytes(h, target, strlen(target));
    for (const D3D_SHADER_MACRO* m = defines; m && m->Name; ++m) h = HashBytes(h, m->Name, strlen(m->Name));
    const std::wstring dir = ExeDirectory() + L"\\shadercache";
    wchar_t name[32];
    swprintf(name, 32, L"\\%016llx.dxbc", (unsigned long long)h);
    const std::wstring file = dir + name;
    std::string bytes;
    if (ReadAllText(file, bytes) && !bytes.empty() && SUCCEEDED(D3DCreateBlob(bytes.size(), &out))) {
        std::memcpy(out->GetBufferPointer(), bytes.data(), bytes.size());
        return S_OK;
    }
    const HRESULT hr = D3DCompileFromFile(path.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &error);
    if (SUCCEEDED(hr)) {
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t tmp[32];
        swprintf(tmp, 32, L".%lu.tmp", GetCurrentThreadId());
        FILE* f = nullptr;
        if (_wfopen_s(&f, (file + tmp).c_str(), L"wb") == 0 && f) {
            fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), f);
            fclose(f);
            if (!MoveFileExW((file + tmp).c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW((file + tmp).c_str());
        }
    }
    return hr;
}
} // namespace

} // namespace

void PrecompileD3D12Shaders(std::atomic<int>& done, std::atomic<int>& total, const std::atomic<bool>& stop, std::atomic<int>* failed) {
    struct Item { const wchar_t* file; const char* entry; const char* target; bool water; };
    static const Item items[] = {
        {L"mesh.hlsl", "VSMain", "vs_5_1"}, {L"mesh.hlsl", "PSMain", "ps_5_1"}, {L"mesh.hlsl", "PSOverlay", "ps_5_1"}, {L"mesh.hlsl", "VSInstanced", "vs_5_1"},
        {L"mesh.hlsl", "VSVegetation", "vs_5_1"}, {L"mesh.hlsl", "PSMainMotion", "ps_5_1"}, {L"mesh.hlsl", "VSSkinnedMotion", "vs_5_1"}, {L"mesh.hlsl", "VSVegetationDepth", "vs_5_1"},
        {L"mesh.hlsl", "PSVegetationDepth", "ps_5_1"}, {L"mesh.hlsl", "PSVegetationShade", "ps_5_1"}, {L"mesh.hlsl", "ShadowVS", "vs_5_1"}, {L"mesh.hlsl", "ShadowInstancedVS", "vs_5_1"},
        {L"mesh.hlsl", "ShadowVegetationVS", "vs_5_1"}, {L"mesh.hlsl", "ShadowCutoutVS", "vs_5_1"}, {L"mesh.hlsl", "PSShadowCutout", "ps_5_1"}, {L"post.hlsl", "FullscreenVS", "vs_5_1"}, {L"post.hlsl", "AoPS", "ps_5_1"}, {L"post.hlsl", "CloudsPS", "ps_5_1"},
        {L"post.hlsl", "CompositePS", "ps_5_1"}, {L"post.hlsl", "TaaPS", "ps_5_1"}, {L"post.hlsl", "TonemapPS", "ps_5_1"}, {L"post.hlsl", "CloudShadowPS", "ps_5_1"},
        {L"post.hlsl", "CloudResolvePS", "ps_5_1"}, {L"post.hlsl", "ProbeTracePS", "ps_5_1"}, {L"post.hlsl", "ProbeBlendPS", "ps_5_1"}, {L"post.hlsl", "ExposurePS", "ps_5_1"},
        {L"post.hlsl", "BloomPS", "ps_5_1"}, {L"water.hlsl", "WaterVS", "vs_5_1"}, {L"water.hlsl", "WaterPS", "ps_5_1"}, {L"water.hlsl", "CSRipple", "cs_5_1", true},
        {L"water.hlsl", "CSWaves", "cs_5_1", true}, {L"water.hlsl", "CSSky", "cs_5_1", true},
        {L"mesh.hlsl", "ChunkBakeVS", "vs_5_1"}, {L"mesh.hlsl", "ChunkBakePS", "ps_5_1"}, {L"mesh.hlsl", "ChunkVolVS", "vs_5_1"}, {L"mesh.hlsl", "ChunkVolPS", "ps_5_1"},
    };
    total = (int)std::size(items);
    std::atomic<int> next{0};
    const auto work = [&] {
        for (int k; !stop && (k = next++) < (int)std::size(items);) {
            const D3D_SHADER_MACRO defines[] = {{"WATER_COMPUTE", "1"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> blob, error;
            if (FAILED(CompileCached(ExeDirectory() + L"\\shaders\\" + items[k].file, items[k].water ? defines : nullptr, items[k].entry, items[k].target, blob, error)) && failed) { // at startup a failure shows and logs when the renderer asks
                ++*failed;
                Log("D3D12: %s: %s", items[k].entry, error ? (const char*)error->GetBufferPointer() : "shader file not found");
            }
            ++done;
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0, n = std::max(1u, std::thread::hardware_concurrency() / 2); i < n; ++i) threads.emplace_back(work); // half the cores: only a changed shader compiles, the rest come from the cache
    for (std::thread& t : threads) t.join();
}

namespace {

class D3D12Renderer final : public Renderer {
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Resource> transient, lightBuffer, postBuffer, timerReadback;
        ComPtr<ID3D12QueryHeap> timerHeap;
        uint8_t *transientPtr = nullptr, *lightPtr = nullptr, *postPtr = nullptr;
        size_t transientUsed = 0;
        UINT nextPair = 1;
        UINT64 fenceValue = 0;
        bool timerPending = false;
    };

public:
    ~D3D12Renderer() override {
        if (spareBlock_.valid()) spareBlock_.wait(); // they use the device
        if (spareTexHeap_.valid()) spareTexHeap_.wait();
        if (queue_ && fence_) WaitIdle();
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    const char* Name() const override { return "D3D12"; }

    bool Init(HWND window, int width, int height, bool validation) override {
        w_ = (UINT)std::max(1, width);
        h_ = (UINT)std::max(1, height);
        UpdateRenderSize();

        UINT factoryFlags = 0;
        { // Device Removed Extended Data: if the GPU is lost, ReportDeviceRemoved can say whether it hung, faulted on memory
          // (a bug of ours) or vanished (power, heat, driver). Cheap; TBD_NODRED=1 leaves it off.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (!GetEnvironmentVariableA("TBD_NODRED", nullptr, 0) && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
        }
        if (validation) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            } else {
                Log("D3D12: debug layer unavailable (install the Graphics Tools optional feature)");
            }
        }

        ComPtr<IDXGIFactory6> factory;
        CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (adapter) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            Log("D3D12: adapter %s", name);
        }
        CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));
        g_logDevice = device_.Get();
        if (validation) device_.As(&infoQueue_);
        // No real-time ray tracing, ever: device5_ stays null, so no ray traced AO pipeline, BLAS or TLAS is made and the AO
        // pass is the rasterized one. The lighting is rasterized and must stand on its own; how light really bounces comes
        // from the offline path-traced reference (gameenginetools\lighting_reference, tbd_reference.exe), which we tune
        // against. Vulkan has no ray traced path either.
        Log("D3D12: real-time ray tracing off (reference renders: gameenginetools\\lighting_reference)");

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)));

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w_;
        sd.Height = h_;
        sd.Format = kColorFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        CHECK(factory->CreateSwapChainForHwnd(queue_.Get(), window, &sd, nullptr, nullptr, &sc1));
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        CHECK(sc1.As(&swap_));

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBackBuffers + kPostTargets;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 6; // the scene's depth buffer, the impostor bake's tile depth buffer, the shadow map, the overlays' depth buffer, the tight shadow map, the water pass's read-only view of the scene's
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_)));
        rtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!CreateTargets()) return false;

        for (Frame& frame : frames_) {
            CHECK(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)));
            CHECK(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&frame.list)));
            frame.list->Close();
        }
        CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!CreatePipelines()) return false;
        if (!CreateMaterialResources()) return false;
        if (!CreatePostPipelines() || !CreateCloudNoise() || !CreateCloudField() || !CreatePostTargets()) return false;
        CreateUiPipeline(); // optional: without it UiSupported() is false and the in-game GUI draws the classic way
        D3D12_RANGE noRead = {0, 0};
        for (Frame& frame : frames_) {
            if (!CreateUploadBuffer(kTransientBytes, frame.transient)) return false;
            CHECK(frame.transient->Map(0, &noRead, reinterpret_cast<void**>(&frame.transientPtr)));
            if (!CreateUploadBuffer(kLightBufferSize, frame.lightBuffer)) return false;
            CHECK(frame.lightBuffer->Map(0, &noRead, reinterpret_cast<void**>(&frame.lightPtr)));
            if (!CreateUploadBuffer(2048, frame.postBuffer)) return false;
            CHECK(frame.postBuffer->Map(0, &noRead, reinterpret_cast<void**>(&frame.postPtr)));
            if (!CreateTimers(frame)) return false;
        }
        if (!CreateUploadBuffer(kStreamRingBytes, streamRing_)) return false;
        CHECK(streamRing_->Map(0, &noRead, reinterpret_cast<void**>(&streamRingMapped_)));
        StartSpareBlock();
        StartSpareTexHeap();
        if (GetEnvironmentVariableA("TBD_TEXTURE_SELFTEST", nullptr, 0) > 0) Log("D3D12: texture self-test %s", SelfTestTextures() ? "passed" : "FAILED");
        return true;
    }

    void Resize(int width, int height) override {
        if (!swap_ || width <= 0 || height <= 0) return;
        if ((UINT)width == w_ && (UINT)height == h_) return;
        WaitIdle();
        for (auto& b : back_) b.Reset();
        depth_.Reset();
        overlayDepth_.Reset();
        w_ = (UINT)width;
        h_ = (UINT)height;
        UpdateRenderSize();
        if (FAILED(swap_->ResizeBuffers(0, w_, h_, DXGI_FORMAT_UNKNOWN, 0))) {
            Log("D3D12: ResizeBuffers failed");
            return;
        }
        CreateTargets();
        CreatePostTargets();
    }

    void SetRenderScale(float scale) override {
        scale = ClampRenderScale(scale);
        if (scale == scale_) return;
        scale_ = scale;
        if (!swap_) return; // Init makes the targets
        WaitIdle();
        for (auto& b : back_) b.Reset();
        depth_.Reset();
        overlayDepth_.Reset();
        UpdateRenderSize();
        CreateTargets();
        CreatePostTargets();
    }

    MeshId CreateMesh(const Vertex* vertices, uint32_t count, Topology topology, bool dynamic) override {
        Mesh mesh;
        mesh.count = count;
        mesh.topology = topology;
        if (dynamic) {
            const UINT64 bytes = (UINT64)count * sizeof(Vertex);
            if (!CreateUploadBuffer(bytes, mesh.buffer)) return kInvalidMesh;
            void* mapped = nullptr;
            D3D12_RANGE noRead = {0, 0};
            if (FAILED(mesh.buffer->Map(0, &noRead, &mapped))) return kInvalidMesh;
            std::memcpy(mapped, vertices, (size_t)bytes);
            mesh.buffer->Unmap(0, nullptr);
            mesh.view = {mesh.buffer->GetGPUVirtualAddress(), (UINT)bytes, sizeof(Vertex)};
        } else {
            std::vector<Vertex> welded;
            std::vector<uint32_t> indices;
            WeldVertices(vertices, count, welded, indices);
            const UINT64 vertexBytes = (UINT64)welded.size() * sizeof(Vertex), indexBytes = (UINT64)indices.size() * sizeof(uint32_t);
            ComPtr<ID3D12Resource> upload;
            if (!CreateUploadBuffer(vertexBytes + indexBytes, upload)) return kInvalidMesh;
            uint8_t* mapped = nullptr;
            D3D12_RANGE noRead = {0, 0};
            if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return kInvalidMesh;
            std::memcpy(mapped, welded.data(), (size_t)vertexBytes);
            std::memcpy(mapped + vertexBytes, indices.data(), (size_t)indexBytes);
            upload->Unmap(0, nullptr);

            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = vertexBytes + indexBytes;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mesh.buffer))))
                return kInvalidMesh;
            ComPtr<ID3D12CommandAllocator> alloc;
            ComPtr<ID3D12GraphicsCommandList> cl;
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
                FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
                return kInvalidMesh;
            cl->CopyBufferRegion(mesh.buffer.Get(), 0, upload.Get(), 0, vertexBytes + indexBytes);
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = mesh.buffer.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // the last: input to its ray tracing structure
            cl->ResourceBarrier(1, &b);
            cl->Close();
            ID3D12CommandList* lists[] = {cl.Get()};
            queue_->ExecuteCommandLists(1, lists);
            WaitIdle();
            const D3D12_GPU_VIRTUAL_ADDRESS base = mesh.buffer->GetGPUVirtualAddress();
            mesh.view = {base, (UINT)vertexBytes, sizeof(Vertex)};
            mesh.indexView = {base + vertexBytes, (UINT)indexBytes, DXGI_FORMAT_R32_UINT};
            mesh.indexed = true;
        }
        return PlaceMesh(std::move(mesh));
    }

    MeshId CreateDeformableMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        const UINT64 copyBytes = (UINT64)vertexCount * sizeof(Vertex), indexBytes = (UINT64)indexCount * sizeof(uint32_t);
        Mesh mesh;
        mesh.count = indexCount;
        mesh.topology = Topology::Triangles;
        // Staging: one copy of the vertices per frame in flight, staying mapped, and the indices once for the first copy.
        if (!CreateUploadBuffer(copyBytes * kFramesInFlight + indexBytes, mesh.upload)) return kInvalidMesh;
        D3D12_RANGE noRead = {0, 0};
        if (FAILED(mesh.upload->Map(0, &noRead, reinterpret_cast<void**>(&mesh.deformable)))) return kInvalidMesh;
        std::memcpy(mesh.deformable, vertices, (size_t)copyBytes);
        std::memcpy(mesh.deformable + copyBytes * kFramesInFlight, indices, (size_t)indexBytes);

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = copyBytes * 2 + indexBytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mesh.buffer))))
            return kInvalidMesh;
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
            return kInvalidMesh;
        cl->CopyBufferRegion(mesh.buffer.Get(), 0, mesh.upload.Get(), 0, copyBytes);
        cl->CopyBufferRegion(mesh.buffer.Get(), copyBytes, mesh.upload.Get(), 0, copyBytes);
        cl->CopyBufferRegion(mesh.buffer.Get(), copyBytes * 2, mesh.upload.Get(), copyBytes * kFramesInFlight, indexBytes);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = mesh.buffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = kDeformableReadState;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        const D3D12_GPU_VIRTUAL_ADDRESS base = mesh.buffer->GetGPUVirtualAddress();
        mesh.copyBytes = copyBytes;
        mesh.view = {base, (UINT)copyBytes, sizeof(Vertex)};
        mesh.indexView = {base + copyBytes * 2, (UINT)indexBytes, DXGI_FORMAT_R32_UINT};
        mesh.indexed = true;
        return PlaceMesh(std::move(mesh));
    }

    MeshId CreateMeshPart(MeshId parent, uint32_t firstIndex, uint32_t indexCount) override {
        if (parent >= meshes_.size() || !meshes_[parent].buffer || !meshes_[parent].copyBytes) return kInvalidMesh;
        Mesh mesh = meshes_[parent];
        mesh.block = UINT32_MAX; // the parent owns any block range
        mesh.blockBytes = 0;
        mesh.deformable = nullptr;
        mesh.copyParent = parent;
        mesh.count = indexCount;
        mesh.indexView.BufferLocation += (UINT64)firstIndex * sizeof(uint32_t);
        mesh.indexView.SizeInBytes = indexCount * sizeof(uint32_t);
        return PlaceMesh(std::move(mesh));
    }

    Vertex* DeformableVertices(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].deformable) return nullptr;
        pendingDeformables_.push_back(id);
        return reinterpret_cast<Vertex*>(meshes_[id].deformable + meshes_[id].copyBytes * frameSlot_);
    }

    void CommitDeformables() override {
        for (MeshId id : pendingDeformables_) {
            Mesh& mesh = meshes_[id];
            mesh.newestCopy ^= 1;
            Transition(mesh.buffer.Get(), kDeformableReadState, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->CopyBufferRegion(mesh.buffer.Get(), mesh.copyBytes * mesh.newestCopy, mesh.upload.Get(), mesh.copyBytes * frameSlot_, mesh.copyBytes);
            Transition(mesh.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kDeformableReadState);
        }
        pendingDeformables_.clear();
    }

    void DestroyMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].buffer) return;
        sunCacheKey_ = 0;
        WaitIdle();
        FreeMeshMemory(meshes_[id]);
        meshes_[id] = Mesh{};
        freeSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ instance buffers
    InstanceBufferId CreateInstanceBuffer(const float* rows, uint32_t count) override {
        if (!device_ || !rows || count == 0) return kNoInstances;
        const UINT64 bytes = (UINT64)count * kInstanceStride;
        ComPtr<ID3D12Resource> upload;
        if (!CreateUploadBuffer(bytes, upload)) return kNoInstances;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return kNoInstances;
        std::memcpy(mapped, rows, (size_t)bytes);
        upload->Unmap(0, nullptr);

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        InstanceBuffer ib;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ib.buffer))))
            return kNoInstances;
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
            return kNoInstances;
        cl->CopyBufferRegion(ib.buffer.Get(), 0, upload.Get(), 0, bytes);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = ib.buffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        ib.view = {ib.buffer->GetGPUVirtualAddress(), (UINT)bytes, kInstanceStride};
        if (!freeInstanceSlots_.empty()) {
            const InstanceBufferId id = freeInstanceSlots_.back();
            freeInstanceSlots_.pop_back();
            instanceBuffers_[id] = std::move(ib);
            return id;
        }
        instanceBuffers_.push_back(std::move(ib));
        return (InstanceBufferId)(instanceBuffers_.size() - 1);
    }

    void DestroyInstanceBuffer(InstanceBufferId id) override {
        if (id >= instanceBuffers_.size() || !instanceBuffers_[id].buffer) return;
        WaitIdle();
        instanceBuffers_[id] = InstanceBuffer{};
        freeInstanceSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ material textures
    TextureId CreateTexture(const uint8_t* rgba, int w, int h) override {
        if (!device_ || !rgba || w <= 0 || h <= 0 || !texHeap_) return kNoTexture;
        std::vector<std::vector<uint8_t>> mips;
        std::vector<std::pair<int, int>> dims;
        mips.emplace_back(rgba, rgba + (size_t)w * h * 4);
        dims.push_back({w, h});
        while (dims.back().first > 1 || dims.back().second > 1) { // 2x2 box filter down to 1x1
            const auto [pw, ph] = dims.back();
            const int nw = std::max(1, pw / 2), nh = std::max(1, ph / 2);
            std::vector<uint8_t> next((size_t)nw * nh * 4);
            const std::vector<uint8_t>& prev = mips.back();
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int sum = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                sum += prev[((size_t)std::min(2 * y + dy, ph - 1) * pw + std::min(2 * x + dx, pw - 1)) * 4 + c];
                        next[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                    }
            mips.push_back(std::move(next));
            dims.push_back({nw, nh});
        }
        const UINT mipCount = (UINT)mips.size();

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT64)w;
        rd.Height = (UINT)h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = (UINT16)mipCount;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        Tex tex;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&tex.resource))))
            return kNoTexture;

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mipCount);
        std::vector<UINT> rows(mipCount);
        std::vector<UINT64> rowBytes(mipCount);
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, 0, mipCount, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        ComPtr<ID3D12Resource> upload;
        if (!CreateUploadBuffer(total, upload)) return kNoTexture;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return kNoTexture;
        for (UINT m = 0; m < mipCount; ++m)
            for (int y = 0; y < dims[m].second; ++y)
                std::memcpy(mapped + fp[m].Offset + (size_t)y * fp[m].Footprint.RowPitch,
                            &mips[m][(size_t)y * dims[m].first * 4], (size_t)dims[m].first * 4);
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
            return kNoTexture;
        for (UINT m = 0; m < mipCount; ++m) {
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = tex.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[m];
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();

        UINT slot;
        if (!freeTexSlots_.empty()) { slot = freeTexSlots_.back(); freeTexSlots_.pop_back(); }
        else if (nextTexSlot_ < kMaxTextures) slot = nextTexSlot_++;
        else { Log("D3D12: out of texture slots"); return kNoTexture; }
        tex.slot = slot;
        WriteTextureSrv(tex.resource.Get(), mipCount, TexCpu(slot));
        if (slot >= textures_.size()) textures_.resize(slot + 1);
        textures_[slot] = std::move(tex);
        return (TextureId)slot;
    }

    void DestroyTexture(TextureId id) override {
        if (id >= textures_.size() || !textures_[id].resource) return;
        ForgetMaterialConstants(id);
        WaitIdle();
        FreeTextureMemory(textures_[id]);
        textures_[id] = Tex{};
        if (id < extraOf_.size()) extraOf_[id] = kNoTexture;
        freeTexSlots_.push_back(id);
    }

    void SetSurfaceExtra(TextureId surface, TextureId extra) override {
        if (surface >= textures_.size()) return;
        if (extraOf_.size() <= surface) extraOf_.resize(surface + 1, kNoTexture);
        extraOf_[surface] = extra;
    }

    void SetMaterial(TextureId albedo, TextureId normal, TextureId surface) override {
        if (!frame_ || !list_ || !shadowSrvHeap_) return;
        UINT slot = 0; // material slot 0 holds the defaults
        const TextureId extra = surface < extraOf_.size() ? extraOf_[surface] : kNoTexture;
        const TextureId ids[kMaterialTextures] = {albedo, normal, surface, extra};
        bool has[kMaterialTextures], any = false;
        for (UINT k = 0; k < kMaterialTextures; ++k) any |= has[k] = ids[k] < textures_.size() && textures_[ids[k]].resource;
        if (any && frame_->nextPair < kMaterialPairs) {
            slot = frame_->nextPair++;
            for (UINT k = 0; k < kMaterialTextures; ++k)
                device_->CopyDescriptorsSimple(1, ShaderCpu(MaterialBase(frameSlot_) + 1 + slot * kMaterialTextures + k), has[k] ? TexCpu(ids[k]) : TexCpu(kMaxTextures + k), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        list_->SetGraphicsRootDescriptorTable(3, ShaderGpu(MaterialBase(frameSlot_) + 1 + slot * kMaterialTextures));
        materialConstants_[0] = SkinDetailScale(albedo);
        materialConstants_[1] = SkinPoreDepthConstant(albedo);
        list_->SetGraphicsRoot32BitConstants(6, 4, materialConstants_, 0);
        ++counts_.materialBinds;
    }

    // ------------------------------------------------------------------ world streaming
    MeshId CreateMeshNoWait(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        if (!device_ || !vertices || !indices || !vertexCount || !indexCount) return kInvalidMesh;
        const UINT64 vertexBytes = (UINT64)vertexCount * sizeof(Vertex), indexBytes = (UINT64)indexCount * sizeof(uint32_t);
        Mesh mesh;
        mesh.count = indexCount;
        mesh.topology = Topology::Triangles;
        UploadSteps steps;
        if (!MeshMemory(vertexBytes + indexBytes, mesh.block, mesh.blockOffset, mesh.blockBytes)) return kInvalidMesh;
        steps.Mark("allocate");
        MeshBlock& block = meshBlocks_[mesh.block];
        if (!StreamCopy(vertices, vertexBytes, indices, indexBytes, block.buffer.Get(), mesh.blockOffset)) {
            FreeMeshMemory(mesh);
            return kInvalidMesh;
        }
        steps.Mark("stage+record");
        steps.Done("D3D12", "mesh upload", vertexBytes + indexBytes);
        if (!block.touched) {
            block.touched = true;
            streamTransitions_.push_back({block.buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER});
        }
        mesh.buffer = block.buffer;
        const D3D12_GPU_VIRTUAL_ADDRESS base = block.buffer->GetGPUVirtualAddress() + mesh.blockOffset;
        mesh.view = {base, (UINT)vertexBytes, sizeof(Vertex)};
        mesh.indexView = {base + vertexBytes, (UINT)indexBytes, DXGI_FORMAT_R32_UINT};
        mesh.indexed = true;
        return PlaceMesh(std::move(mesh));
    }

    InstanceBufferId CreateInstanceBufferNoWait(const float* rows, uint32_t count) override {
        if (!device_ || !rows || count == 0) return kNoInstances;
        const UINT64 bytes = (UINT64)count * kInstanceStride;
        InstanceBuffer ib;
        if (!CreateDefaultBuffer(bytes, ib.buffer) || !StreamCopy(rows, bytes, nullptr, 0, ib.buffer.Get(), 0)) return kNoInstances;
        streamTransitions_.push_back({ib.buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});
        ib.view = {ib.buffer->GetGPUVirtualAddress(), (UINT)bytes, kInstanceStride};
        if (!freeInstanceSlots_.empty()) {
            const InstanceBufferId id = freeInstanceSlots_.back();
            freeInstanceSlots_.pop_back();
            instanceBuffers_[id] = std::move(ib);
            return id;
        }
        instanceBuffers_.push_back(std::move(ib));
        return (InstanceBufferId)(instanceBuffers_.size() - 1);
    }

    TextureId CreateTextureNoWait(const std::vector<std::vector<uint8_t>>& levels, const std::vector<std::pair<int, int>>& dims) override {
        if (!device_ || levels.empty() || levels.size() != dims.size() || !texHeap_) return kNoTexture;
        const UINT mipCount = (UINT)levels.size();
        UINT slot;
        if (!freeTexSlots_.empty()) { slot = freeTexSlots_.back(); freeTexSlots_.pop_back(); }
        else if (nextTexSlot_ < kMaxTextures) slot = nextTexSlot_++;
        else { Log("D3D12: out of texture slots"); return kNoTexture; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT64)dims[0].first;
        rd.Height = (UINT)dims[0].second;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = (UINT16)mipCount;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        Tex tex;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mipCount);
        std::vector<UINT> rows(mipCount);
        std::vector<UINT64> rowBytes(mipCount);
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, 0, mipCount, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        UploadSteps steps;
        // Placed in a big heap made ahead of time: a committed texture costs 5-60 ms of the driver's time each. Every texel is
        // written by the copy, so the memory need not be zeroed first.
        const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &rd);
        if (TextureMemory(info.SizeInBytes, info.Alignment, tex.heap, tex.heapOffset, tex.heapBytes)) {
            if (FAILED(device_->CreatePlacedResource(texHeaps_[tex.heap].heap.Get(), tex.heapOffset, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex.resource)))) {
                FreeTextureMemory(tex);
                tex.resource.Reset();
            }
        }
        if (!tex.resource && FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex.resource)))) {
            freeTexSlots_.push_back(slot);
            return kNoTexture;
        }
        steps.Mark("texture-create");
        ID3D12Resource* source = nullptr;
        UINT64 base = 0;
        uint8_t* mapped = StreamStaging(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, source, base);
        if (!mapped || !StreamListOpen()) {
            FreeTextureMemory(tex);
            freeTexSlots_.push_back(slot);
            return kNoTexture;
        }
        steps.Mark("stage");
        if (tex.heap != UINT32_MAX) { // its range may have held a texture retired since: this one takes the memory over
            D3D12_RESOURCE_BARRIER alias = {};
            alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            alias.Aliasing.pResourceAfter = tex.resource.Get();
            streamList_->ResourceBarrier(1, &alias);
        }
        for (UINT m = 0; m < mipCount; ++m) {
            for (int y = 0; y < dims[m].second; ++y)
                std::memcpy(mapped + fp[m].Offset + (size_t)y * fp[m].Footprint.RowPitch, &levels[m][(size_t)y * dims[m].first * 4], (size_t)dims[m].first * 4);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = tex.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;
            src.pResource = source;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[m];
            src.PlacedFootprint.Offset += base;
            streamList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        steps.Mark("copy+record");
        streamTransitions_.push_back({tex.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE});
        tex.slot = slot;
        WriteTextureSrv(tex.resource.Get(), mipCount, TexCpu(slot));
        if (slot >= textures_.size()) textures_.resize(slot + 1);
        textures_[slot] = std::move(tex);
        steps.Done("D3D12", "texture upload", total);
        return (TextureId)slot;
    }

    void RetireMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].buffer) return;
        StreamRetired r;
        r.frame = streamFrame_;
        r.mesh = id;
        streamRetired_.push_back(std::move(r));
    }

    void RetireTexture(TextureId id) override {
        if (id >= textures_.size() || !textures_[id].resource) return;
        ForgetMaterialConstants(id);
        StreamRetired r;
        r.frame = streamFrame_;
        r.texture = id;
        streamRetired_.push_back(std::move(r));
    }

    uint64_t LocalMemoryBytes() const override {
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (!device_ || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) return 0;
        DXGI_ADAPTER_DESC1 desc;
        return SUCCEEDED(adapter->GetDesc1(&desc)) ? (uint64_t)desc.DedicatedVideoMemory : 0;
    }

    // ------------------------------------------------------------------ compute
    GpuBufferId CreateGpuBuffer(GpuBufferUse use, uint64_t bytes, const void* initial) override {
        if (!device_ || bytes == 0) return kNoGpuBuffer;
        GpuBuffer g;
        g.use = use;
        g.bytes = bytes;
        g.rest = use == GpuBufferUse::Read ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                 : use == GpuBufferUse::ShaderData ? (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                 : use == GpuBufferUse::Args ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        const D3D12_RESOURCE_STATES initialState = initial ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = use == GpuBufferUse::Read ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr, IID_PPV_ARGS(&g.buffer)))) return kNoGpuBuffer;
        ComPtr<ID3D12Resource> upload;
        if (initial) {
            if (!CreateUploadBuffer(bytes, upload)) return kNoGpuBuffer;
            uint8_t* mapped = nullptr;
            D3D12_RANGE noRead = {0, 0};
            if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return kNoGpuBuffer;
            std::memcpy(mapped, initial, (size_t)bytes);
            upload->Unmap(0, nullptr);
        }
        if (!ImmediateList([&](ID3D12GraphicsCommandList* cl) {
                if (initial) cl->CopyBufferRegion(g.buffer.Get(), 0, upload.Get(), 0, bytes);
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = g.buffer.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = initialState;
                b.Transition.StateAfter = g.rest;
                cl->ResourceBarrier(1, &b);
            }))
            return kNoGpuBuffer;
        for (size_t i = 0; i < gpuBuffers_.size(); ++i)
            if (!gpuBuffers_[i].buffer) { gpuBuffers_[i] = std::move(g); return (GpuBufferId)i; }
        gpuBuffers_.push_back(std::move(g));
        return (GpuBufferId)(gpuBuffers_.size() - 1);
    }

    void DestroyGpuBuffer(GpuBufferId id) override {
        if (id >= gpuBuffers_.size() || !gpuBuffers_[id].buffer) return;
        WaitIdle();
        gpuBuffers_[id] = GpuBuffer{};
    }

    ComputeShaderId CreateComputeShader(const char* entry) override {
        if (!computeRootSig_ && !CreateComputeRoot()) return kNoComputeShader;
        ComPtr<ID3DBlob> cs;
        if (!CompileShader(L"vegetation.hlsl", entry, "cs_5_1", cs)) return kNoComputeShader;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = computeRootSig_.Get();
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return kNoComputeShader;
        computePsos_.push_back(pso);
        return (ComputeShaderId)(computePsos_.size() - 1);
    }

    ComputeShaderId CreateComputeShaderIn(const char* file, const char* entry) override {
        if (!computeRootSig_ && !CreateComputeRoot()) return kNoComputeShader;
        std::wstring path = ExeDirectory() + L"\\shaders\\";
        for (const char* c = file; *c; ++c) path += (wchar_t)*c;
        path += L".hlsl";
        const D3D_SHADER_MACRO defines[] = {{"WATER_COMPUTE", "1"}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> cs, error;
        if (FAILED(CompileCached(path, defines, entry, "cs_5_1", cs, error))) {
            Log("D3D12: %s: %s", entry, error ? (const char*)error->GetBufferPointer() : "shader file not found");
            return kNoComputeShader;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = computeRootSig_.Get();
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return kNoComputeShader;
        computePsos_.push_back(pso);
        return (ComputeShaderId)(computePsos_.size() - 1);
    }

    bool SetWaterDraw(const WaterDraw& draw) override {
        if (!waterPso_ && !CreateWaterPipeline()) return false;
        water_ = draw;
        waterMeshes_.assign(draw.meshes, draw.meshes + draw.meshCount);
        waterPending_ = !waterMeshes_.empty();
        return true;
    }

    void BeginCompute() override {
        ComputeTransitions(false);
        list_->SetComputeRootSignature(computeRootSig_.Get());
    }

    void Dispatch(ComputeShaderId shader, const void* constants, uint32_t constantBytes, const GpuBufferId reads[4], const GpuBufferId writes[3], uint32_t x, uint32_t y, uint32_t z) override {
        if (shader >= computePsos_.size()) return;
        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // null resource: everything written so far is visible to this dispatch
        list_->ResourceBarrier(1, &uav);
        list_->SetPipelineState(computePsos_[shader].Get());
        list_->SetComputeRoot32BitConstants(0, constantBytes / 4, constants, 0);
        for (UINT i = 0; i < 4; ++i) list_->SetComputeRootShaderResourceView(1 + i, gpuBuffers_[reads[i]].buffer->GetGPUVirtualAddress());
        for (UINT i = 0; i < 3; ++i) list_->SetComputeRootUnorderedAccessView(5 + i, gpuBuffers_[writes[i]].buffer->GetGPUVirtualAddress());
        list_->Dispatch(x, y, z);
    }

    void EndCompute() override { ComputeTransitions(true); }

    void SetVegetationPass(int pass) override { vegetationPass_ = pass; }

    uint32_t MeshIndexCount(MeshId id) const override { return id < meshes_.size() && meshes_[id].buffer && meshes_[id].indexed ? meshes_[id].count : 0; }

    void DrawVegetationIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) override {
        if (resolved_ || !MeshIndexCount(id)) return;
        list_->SetPipelineState(vegetationPass_ == 1 ? vegetationDepthPso_.Get() : vegetationPass_ == 2 ? vegetationShadePso_.Get() : vegetationPso_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        VegetationIndirect(meshes_[id], instances, args, argsOffset, viewProj, params);
        ++counts_.draws;
    }

    void DrawVegetationShadowIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& params) override {
        if (!MeshIndexCount(id)) return;
        list_->SetPipelineState(vegetationShadowPso_.Get());
        forceShadowRebind_ = true; // the next caster sets its own pipeline
        VegetationIndirect(meshes_[id], instances, args, argsOffset, lightVP_, params);
    }

    // Hair cards into a shadow map: depth only where the card's texture has coverage (PSShadowCutout), so a head of hair casts
    // strand-shaped, partly see-through shadows instead of the cards' rectangles.
    void DrawShadowCasterCutout(MeshId id, const Mat4& model, TextureId albedo) override {
        if (id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles) return;
        SetMaterial(albedo, kNoTexture, kNoTexture);
        list_->SetPipelineState(shadowTarget_ == ShadowMap::Tight ? tightCutoutPso_.Get() : shadowCutoutPso_.Get());
        forceShadowRebind_ = true; // the next caster sets its own pipeline
        const Mesh& mesh = meshes_[id];
        const Mat4 mvp = lightVP_ * model;
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        const D3D12_VERTEX_BUFFER_VIEW view = DrawView(mesh);
        list_->IASetVertexBuffers(0, 1, &view);
        if (mesh.indexed) {
            list_->IASetIndexBuffer(&mesh.indexView);
            list_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
        } else {
            list_->DrawInstanced(mesh.count, 1, 0, 0);
        }
        ++counts_.shadowDraws;
        counts_.shadowTriangles += mesh.count / 3;
    }

    bool ReadGpuBuffer(GpuBufferId id, void* out, uint64_t bytes) override {
        if (id >= gpuBuffers_.size() || !gpuBuffers_[id].buffer) return false;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;
        GpuBuffer& g = gpuBuffers_[id];
        if (!ImmediateList([&](ID3D12GraphicsCommandList* cl) {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = g.buffer.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = g.rest;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(1, &b);
                cl->CopyBufferRegion(rb.Get(), 0, g.buffer.Get(), 0, bytes);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                cl->ResourceBarrier(1, &b);
            }))
            return false;
        void* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        std::memcpy(out, mapped, (size_t)bytes);
        rb->Unmap(0, nullptr);
        return true;
    }

    void BeginFrame(const float clearColor[4]) override {
        prepass_ = 0;
        lastCounts_ = counts_;
        counts_ = {};
        frameSlot_ = (frameSlot_ + 1) % kFramesInFlight;
        frame_ = &frames_[frameSlot_];
        list_ = frame_->list.Get();
        const double waitStart = TraceNowMs();
        WaitFrame(*frame_);
        if (device_->GetDeviceRemovedReason() != S_OK) ReportDeviceRemoved();
        FlushStreamRetired();
        rayScratch_[frameSlot_].clear(); // this slot's last builds are done
        fenceWaitMs_ = TraceNowMs() - waitStart;
        frame_->nextPair = 1;
        frame_->transientUsed = 0;
        frameIndex_ = swap_->GetCurrentBackBufferIndex();
        frame_->allocator->Reset();
        list_->Reset(frame_->allocator.Get(), nullptr);

        resolved_ = false;
        Mark(0);
        shadowMarked_ = false;
        Transition(back_[frameIndex_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(post_[kScene].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(post_[kVelocity].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = ColorRtv();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE sceneTargets[2] = {rtv, PostRtv(kVelocity)};
        list_->OMSetRenderTargets(2, sceneTargets, FALSE, &dsv);
        list_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        const float noMotion[4] = {0, 0, 0, 0};
        list_->ClearRenderTargetView(sceneTargets[1], noMotion, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); // reversed Z

        D3D12_VIEWPORT vp = {0, 0, (float)rw_, (float)rh_, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, (LONG)rw_, (LONG)rh_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        std::memcpy(frame_->lightPtr, &lighting_, sizeof(lighting_));
        std::memcpy(frame_->lightPtr + sizeof(lighting_), &portals_, sizeof(portals_));
        reinterpret_cast<Lighting*>(frame_->lightPtr)->eye[3] = (float)(postFrame_ & 1023); // the frame, for dithered transparency that TAA averages
        if (!cloudMapValid_) reinterpret_cast<Lighting*>(frame_->lightPtr)->cloudMap[3] = 0.0f; // nothing drawn into it yet
        cloudMapValid_ = lighting_.cloudMap[3] > 0.5f;
        if (probeAge_ == 0) reinterpret_cast<Lighting*>(frame_->lightPtr)->probeUpdate[3] = 0.0f; // the atlas holds nothing yet
        std::memcpy(motionPtr_[frameSlot_], view_.viewProj.m, 64);
        std::memcpy(motionPtr_[frameSlot_] + 64, prevViewProj_.m, 64);
        std::memcpy(motionPtr_[frameSlot_] + 128, &view_.dt, 4);
        ID3D12DescriptorHeap* heaps[] = {shadowSrvHeap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        BindMeshRoot();
    }

    void BindMeshRoot() {
        list_->SetGraphicsRootSignature(rootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(1, frame_->lightBuffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootConstantBufferView(5, motionBuffer_[frameSlot_]->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(2, shadowSrvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->SetGraphicsRootDescriptorTable(3, ShaderGpu(MaterialBase(frameSlot_) + 1)); // default material
        list_->SetGraphicsRootDescriptorTable(4, ShaderGpu(kPostDescriptorBase + 2 * kPostSrvs)); // the probe atlas
    }

    void SetFrameView(const FrameView& view) override { view_ = view; }

    // Takes the shader sources from the repo (bin sits next to src), compiles all of them, and only if every one compiles swaps in new
    // pipelines. Between frames; waits for the GPU.
    bool ReloadShaders() override {
        WaitIdle();
        const std::wstring from = ExeDirectory() + L"\\..\\src\\shaders\\", to = ExeDirectory() + L"\\shaders\\";
        WIN32_FIND_DATAW fd;
        if (HANDLE find = FindFirstFileW((from + L"*.hlsl*").c_str(), &fd); find != INVALID_HANDLE_VALUE) {
            do CopyFileW((from + fd.cFileName).c_str(), (to + fd.cFileName).c_str(), FALSE); while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        g_shaderHashStale = true;
        std::atomic<int> done{0}, total{0}, failed{0};
        const std::atomic<bool> stop{false};
        PrecompileD3D12Shaders(done, total, stop, &failed);
        if (failed > 0) {
            Log("D3D12: shader reload: %d of %d shaders failed to compile; the running shaders stay", failed.load(), total.load());
            return false;
        }
        waterPso_.Reset();
        const bool ok = CreatePipelines() && CreatePostPipelines();
        if (ok) CreateUiPipeline(); // a broken ui.hlsl only turns the smooth GUI off
        Log("D3D12: shader reload: %d shaders, pipelines %s", total.load(), ok ? "rebuilt" : "FAILED to rebuild");
        return ok;
    }

    // AO, clouds, sky and haze, TAA, tone mapping; afterwards the back buffer is current and keeps the scene's depth.
    void ResolveScene() override {
        if (resolved_) return;
        if (!shadowMarked_) Mark(1);
        Mark(2);
        struct PostParams {
            float invViewProj[16], prevViewProj[16], eye[4], sunDir[4], res[4], misc[4];
            PostLightBlock light;
            float motion[4];
            PortalBlock portals;
        } p = {};
        static_assert(sizeof(p) <= 2048, "the post constant buffer");
        p.portals = portals_;
        FillPostLight(lighting_, probeAge_, p.light);
        std::memcpy(p.invViewProj, view_.invJittered.m, sizeof(p.invViewProj));
        std::memcpy(p.prevViewProj, prevViewProj_.m, sizeof(p.prevViewProj));
        p.eye[0] = view_.eye.x; p.eye[1] = view_.eye.y; p.eye[2] = view_.eye.z;
        std::memcpy(p.sunDir, lighting_.sunDir, sizeof(p.sunDir));
        p.eye[3] = scale_ < 1.0f ? 1.0f : 0.0f;
        p.res[0] = (float)rw_; p.res[1] = (float)rh_; p.res[2] = 1.0f / (float)rw_; p.res[3] = 1.0f / (float)rh_;
        p.misc[0] = view_.time; p.misc[1] = (float)(postFrame_ & 1023); p.misc[2] = view_.sky ? 1.0f : 0.0f;
        p.misc[3] = historyValid_ ? 1.0f : 0.0f;
        p.motion[0] = view_.motionVectors ? 1.0f : 0.0f; p.motion[1] = g_brightness; p.motion[2] = g_sceneExposure;
        std::memcpy(frame_->postPtr, &p, sizeof(p));

        const bool rayTraced = BuildRayScene();
        Transition(post_[kScene].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(post_[kVelocity].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const UINT cur = postFrame_ & 1;
        list_->SetGraphicsRootSignature(postRootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(0, frame_->postBuffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(1, ShaderGpu(kPostDescriptorBase + cur * kPostSrvs));
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (lighting_.cloudMap[3] > 0.5f) PostPass(kCloudShadow, postPso_[9].Get(), kCloudShadowSize, kCloudShadowSize);
        Mark(3);
        if (lighting_.probeUpdate[3] > 0.5f) {
            PostPass(kProbeRaysTarget, postPso_[5].Get(), kProbeRays, kProbesPerFrame);
            PostPass(kProbes, postPso_[6].Get(), kProbeX * 3, kProbeAtlasH);
            ++probeAge_;
        }
        Mark(4);
        if (rayTraced) list_->SetGraphicsRootShaderResourceView(2, rayFrames_[frameSlot_].tlas->GetGPUVirtualAddress());
        PostPass(kAo, rayTraced ? aoRtPso_.Get() : postPso_[0].Get(), std::max(1u, rw_ / 2), std::max(1u, rh_ / 2));
        Mark(5);
        if (view_.sky) {
            PostPass(kCloudMarch, postPso_[1].Get(), std::max(1u, rw_ / 4), std::max(1u, rh_ / 4));
            PostPass(cur ? kClouds1 : kClouds, postPso_[10].Get(), std::max(1u, rw_ / 2), std::max(1u, rh_ / 2));
        }
        Mark(6);
        PostPass(kComposed, postPso_[2].Get(), rw_, rh_);
        if (waterPending_) WaterPass(cur);
        Mark(7);
        PostPass(kHistory0 + cur, postPso_[3].Get(), rw_, rh_);
        PostPass(kExposure, postPso_[7].Get(), 1, 1);
        PostPass(kBloom, postPso_[8].Get(), std::max(1u, rw_ / 4), std::max(1u, rh_ / 4));
        Mark(8);
        resolved_ = true;

        const D3D12_CPU_DESCRIPTOR_HANDLE back = ColorRtv();
        // Overlays test against the scene's depth, except when it is smaller than the window: then a window-sized one, cleared here.
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = overlayDepth_ ? OverlayDsv() : dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(1, &back, FALSE, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)w_, (float)h_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)w_, (LONG)h_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetPipelineState(postPso_[4].Get());
        list_->DrawInstanced(3, 1, 0, 0);
        Mark(9);

        Transition(depth_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (overlayDepth_) list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); // reversed Z
        list_->OMSetRenderTargets(1, &back, FALSE, &dsv);
        BindMeshRoot();
        prevViewProj_ = view_.viewProj;
        historyValid_ = true;
        ++postFrame_;
    }

    void StoreSunCache(uint64_t key) override {
        Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyResource(sunCache_.Get(), shadowMap_.Get());
        Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        sunCacheKey_ = key;
    }

    void BeginShadowPass(ShadowMap map, bool restoreSunCache) override {
        shadowTarget_ = map;
        const bool tight = map == ShadowMap::Tight;
        const UINT size = tight ? kTightShadowMapSize : kShadowMapSize;
        std::memcpy(lightVP_.m, tight ? lighting_.tightVP : lighting_.shadowVP, sizeof(lightVP_.m));
        const bool restore = restoreSunCache && !tight;
        if (restore) {
            Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(sunCache_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list_->CopyResource(shadowMap_.Get(), sunCache_.Get());
            Transition(sunCache_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        } else {
            Transition(tight ? tightMap_.Get() : shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = tight ? TightDsv() : ShadowDsv();
        list_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        if (!restore) list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)size, (float)size, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)size, (LONG)size};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetPipelineState(tight ? tightPso_.Get() : shadowPso_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void DrawShadowCaster(MeshId id, const Mat4& model) override {
        if (id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles) return;
        const Mesh& mesh = meshes_[id];
        if (shadowInstancedBound_ || forceShadowRebind_) { // an instanced caster switched the pipeline
            forceShadowRebind_ = false;
            list_->SetPipelineState(shadowTarget_ == ShadowMap::Tight ? tightPso_.Get() : shadowPso_.Get());
            shadowInstancedBound_ = false;
        }
        const Mat4 mvp = lightVP_ * model;
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        const D3D12_VERTEX_BUFFER_VIEW view = DrawView(mesh);
        list_->IASetVertexBuffers(0, 1, &view);
        if (mesh.indexed) {
            list_->IASetIndexBuffer(&mesh.indexView);
            list_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
        } else {
            list_->DrawInstanced(mesh.count, 1, 0, 0);
        }
        ++counts_.shadowDraws;
        counts_.shadowTriangles += mesh.count / 3;
    }

    void DrawShadowInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count) override {
        if (count == 0 || id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer].buffer)
            return;
        const Mesh& mesh = meshes_[id];
        if (!shadowInstancedBound_ || forceShadowRebind_) {
            forceShadowRebind_ = false;
            list_->SetPipelineState(shadowTarget_ == ShadowMap::Tight ? tightInstancedPso_.Get() : shadowInstancedPso_.Get());
            shadowInstancedBound_ = true;
        }
        PushConstants pc;
        std::memcpy(pc.mvp, lightVP_.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        DrawInstancedMesh(mesh, instanceBuffers_[buffer], first, count);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(mesh.count / 3) * count;
    }

    void SetScenePrepass(int mode) override { prepass_ = mode >= 0 && mode <= 2 ? mode : 0; }

    void DrawInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count, const Mat4& viewProj) override {
        if (count == 0 || resolved_ || id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer].buffer)
            return;
        const Mesh& mesh = meshes_[id];
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetPipelineState(prepass_ ? prepassPso_[1][prepass_ - 1].Get() : instancedPso_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        DrawInstancedMesh(mesh, instanceBuffers_[buffer], first, count);
        if (prepass_ == 1) return; // the pre-pass: its draws are counted once, when shaded
        ++counts_.draws;
        counts_.triangles += (uint64_t)(mesh.count / 3) * count;
    }

    void DrawInstancedTransient(MeshId id, const float* rows, uint32_t count, const Mat4& viewProj) override {
        if (count == 0 || !rows || resolved_ || id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles) return;
        const size_t bytes = (size_t)count * kInstanceStride;
        const size_t offset = (frame_->transientUsed + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("D3D12: transient buffer full, instanced draw dropped");
            return;
        }
        std::memcpy(frame_->transientPtr + offset, rows, bytes);
        frame_->transientUsed = offset + bytes;
        InstanceBuffer ib;
        ib.view = {frame_->transient->GetGPUVirtualAddress() + offset, (UINT)bytes, kInstanceStride};
        const Mesh& mesh = meshes_[id];
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetPipelineState(instancedPso_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        DrawInstancedMesh(mesh, ib, 0, count);
        ++counts_.draws;
        counts_.triangles += (uint64_t)(mesh.count / 3) * count;
    }

    void EndShadowPass() override {
        shadowInstancedBound_ = false;
        if (shadowTarget_ == ShadowMap::Sun) { // the tight pass, drawn first, falls inside this stretch
            Mark(1);
            shadowMarked_ = true;
        }
        Transition(shadowTarget_ == ShadowMap::Tight ? tightMap_.Get() : shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const D3D12_CPU_DESCRIPTOR_HANDLE sceneTargets[2] = {ColorRtv(), PostRtv(kVelocity)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(2, sceneTargets, FALSE, &dsv);
        const D3D12_VIEWPORT vp = {0, 0, (float)rw_, (float)rh_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)rw_, (LONG)rh_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
    }

    void BeginCapture() override {
        Transition(post_[kCapture].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {PostRtv(kCapture), PostRtv(kVelocity)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(2, targets, FALSE, &dsv);
        const float nothing[4] = {0, 0, 0, 0};
        list_->ClearRenderTargetView(targets[0], nothing, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)kCaptureSize, (float)kCaptureSize, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)kCaptureSize, (LONG)kCaptureSize};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
    }

    void EndCapture() override {
        Transition(post_[kCapture].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {ColorRtv(), PostRtv(kVelocity)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(2, targets, FALSE, &dsv);
        const float noMotion[4] = {0, 0, 0, 0};
        list_->ClearRenderTargetView(targets[1], noMotion, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)rw_, (float)rh_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)rw_, (LONG)rh_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
    }

    void Draw(MeshId id, const Mat4& mvp, const Mat4& model, bool depthTest, bool counted) override {
        if (id >= meshes_.size() || !meshes_[id].buffer) return;
        const Mesh& mesh = meshes_[id];
        if (prepass_ == 1 && (mesh.copyBytes || resolved_ || !depthTest || mesh.topology != Topology::Triangles)) return; // only static lit triangles
        if (mesh.copyBytes && !resolved_ && depthTest && mesh.topology == Topology::Triangles && mesh.indexed) { // skinned: it says how it moved
            PushConstants pc;
            std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
            std::memcpy(pc.model, model.m, sizeof(pc.model));
            list_->SetPipelineState(skinnedMotionPso_.Get());
            list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
            list_->SetGraphicsRoot32BitConstants(6, 4, materialConstants_, 0);
            D3D12_VERTEX_BUFFER_VIEW views[2] = {DrawView(mesh), DrawView(mesh)};
            views[1].BufferLocation = mesh.view.BufferLocation + mesh.copyBytes * (NewestCopy(mesh) ^ 1);
            list_->IASetVertexBuffers(0, 2, views);
            list_->IASetIndexBuffer(&mesh.indexView);
            list_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
            if (counted) { ++counts_.draws; counts_.triangles += mesh.count / 3; }
            return;
        }
        Submit(DrawView(mesh), mesh.count, mesh.topology, mvp, model, depthTest, mesh.indexed ? &mesh.indexView : nullptr);
        if (counted) {
            ++counts_.draws;
            if (mesh.topology == Topology::Triangles) counts_.triangles += mesh.count / 3;
        }
    }

    void DrawTransient(const Vertex* vertices, uint32_t count, Topology topology, const Mat4& mvp, const Mat4& model,
                       bool depthTest) override {
        if (count == 0) return;
        const size_t bytes = (size_t)count * sizeof(Vertex);
        const size_t offset = (frame_->transientUsed + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("D3D12: transient buffer full, draw dropped");
            return;
        }
        std::memcpy(frame_->transientPtr + offset, vertices, bytes);
        frame_->transientUsed = offset + bytes;
        D3D12_VERTEX_BUFFER_VIEW view = {frame_->transient->GetGPUVirtualAddress() + offset, (UINT)bytes, sizeof(Vertex)};
        Submit(view, count, topology, mvp, model, depthTest);
    }

    void EndFrame() override {
        ResolveScene(); // a caller that drew no overlays still gets its picture
        const bool capturing = capture_ && PrepareCapture();
        if (capturing) {
            Transition(back_[frameIndex_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = captureBuffer_.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = captureFootprint_;
            src.pResource = back_[frameIndex_].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(back_[frameIndex_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        } else {
            Transition(back_[frameIndex_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        Mark(10);
        list_->ResolveQueryData(frame_->timerHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kTimerMarks, frame_->timerReadback.Get(), 0);
        list_->Close();
        SubmitStreamList(fenceValue_ + 1);
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        static const bool noVsync = GetEnvironmentVariableA("TBD_NOVSYNC", nullptr, 0) > 0; // for measuring frame cost
        const double presentStart = TraceNowMs();
        const HRESULT presented = swap_->Present(noVsync ? 0 : 1, 0);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) ReportDeviceRemoved();
        presentMs_ = TraceNowMs() - presentStart;
        const UINT64 value = ++fenceValue_;
        queue_->Signal(fence_.Get(), value);
        frame_->fenceValue = value;
        frame_->timerPending = true;
        DrainDebugMessages();
        if (capturing) WaitFrame(*frame_);
        if (capturing) { // the GPU is idle, so the copy is complete
            const UINT w = captureFootprint_.Footprint.Width, h = captureFootprint_.Footprint.Height;
            uint8_t* px = nullptr;
            if (SUCCEEDED(captureBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&px)))) {
                captureBgra_.resize((size_t)w * h * 4);
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(&captureBgra_[(size_t)y * w * 4], px + (size_t)y * captureFootprint_.Footprint.RowPitch, (size_t)w * 4);
                captureBuffer_->Unmap(0, nullptr);
                captureW_ = (int)w;
                captureH_ = (int)h;
                captureNew_ = true;
            }
        }
    }

    void SetFrameCapture(bool on) override {
        capture_ = on;
        captureNew_ = false;
        if (!on) captureBuffer_.Reset();
    }

    bool CapturedFrame(std::vector<uint8_t>& bgra, int& width, int& height) override {
        if (!captureNew_) return false;
        captureNew_ = false;
        bgra.swap(captureBgra_);
        width = captureW_;
        height = captureH_;
        return true;
    }

private:
    // A readback buffer the size of the back buffer; made again after a resize.
    bool PrepareCapture() {
        const D3D12_RESOURCE_DESC rd = back_[frameIndex_]->GetDesc();
        if (captureBuffer_ && captureFootprint_.Footprint.Width == rd.Width && captureFootprint_.Footprint.Height == rd.Height) return true;
        captureBuffer_.Reset();
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&rd, 0, 1, 0, &captureFootprint_, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&captureBuffer_)));
    }

    bool capture_ = false, captureNew_ = false;
    ComPtr<ID3D12Resource> captureBuffer_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint_ = {};
    std::vector<uint8_t> captureBgra_;
    int captureW_ = 0, captureH_ = 0;

    // Descriptor layout. Shader-visible heap: 0 = shadow map, 1 = tight shadow map (the slot the material triples have always
    // left free), then (albedo, normal, surface) triples; triple 0 is the defaults and
    // the rest are handed out per SetMaterial, starting over every frame. CPU-only heap: one SRV per texture, with the
    // two default textures stored just past the last texture slot.
    static constexpr UINT kMaxTextures = 8192;
    static constexpr UINT kMaterialPairs = 16384;
    static constexpr UINT kMaterialTextures = 4; // albedo, normal, surface, extra: one table of this many descriptors per SetMaterial
    struct Tex {
        ComPtr<ID3D12Resource> resource;
        UINT slot = 0;
        uint32_t heap = UINT32_MAX; // streamed: placed in texHeaps_[heap]
        UINT64 heapOffset = 0, heapBytes = 0;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE TexCpu(UINT slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = texHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * srvInc_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ShaderCpu(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = shadowSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * srvInc_;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE ShaderGpu(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = shadowSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)index * srvInc_;
        return h;
    }
    void WriteTextureSrv(ID3D12Resource* resource, UINT mips, D3D12_CPU_DESCRIPTOR_HANDLE at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = mips;
        device_->CreateShaderResourceView(resource, &sv, at);
    }

    // The CPU descriptor heap, the three 1x1 default textures (white; flat normal; neutral surface) and the default triple in the shader heap.
    bool CreateMaterialResources() {
        srvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kMaxTextures + kMaterialTextures;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&texHeap_)));
        const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, surface[4] = {128, 255, 255, 255}, extra[4] = {255, 255, 255, 0};
        for (UINT k = 0; k < kMaterialTextures; ++k) {
            const TextureId id = CreateTexture(k == 0 ? white : k == 1 ? flat : k == 2 ? surface : extra, 1, 1);
            if (id == kNoTexture) return false;
            device_->CopyDescriptorsSimple(1, TexCpu(kMaxTextures + k), TexCpu(id), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (UINT f = 0; f < kFramesInFlight; ++f)
                device_->CopyDescriptorsSimple(1, ShaderCpu(MaterialBase(f) + 1 + k), TexCpu(id), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            defaults_[k] = id; // kept alive for the renderer's lifetime
        }
        return true;
    }

    // ------------------------------------------------------------------ HDR scene pipeline
    enum PostTarget : UINT { kScene, kAo, kClouds, kComposed, kHistory0, kHistory1, kProbes, kProbeRaysTarget, kExposure, kBloom, kCloudShadow, kVelocity, kClouds1, kCloudMarch, kCapture, kPostTargets };
    static constexpr UINT kPostSrvs = 19;
    static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // xy motion, z skin's surface reflection (post.hlsl ScatterSkin), w spare
    static constexpr UINT kPostDescriptorBase = 1 + kMaterialTextures * kMaterialPairs * kFramesInFlight; // two tables, one per history direction, then the probe atlas for the mesh root

    D3D12_CPU_DESCRIPTOR_HANDLE PostRtv(UINT target) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(kBackBuffers + target) * rtvSize_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ColorRtv() const {
        if (!resolved_) return PostRtv(kScene);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)frameIndex_ * rtvSize_;
        return h;
    }

    void PostPass(UINT target, ID3D12PipelineState* pso, UINT w, UINT h) {
        Transition(post_[target].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = PostRtv(target);
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)w, (LONG)h};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetPipelineState(pso);
        list_->DrawInstanced(3, 1, 0, 0);
        Transition(post_[target].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Render-sized targets, resting in the shader-resource state, and the two descriptor tables over them.
    bool CreatePostTargets() {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ++captureEpoch_;
        for (UINT t = 0; t < kPostTargets; ++t) {
            post_[t].Reset();
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, rw_ / 2) : t == kCloudMarch ? std::max(1u, rw_ / 4) : t == kProbes ? kProbeX * 3 : t == kProbeRaysTarget ? kProbeRays : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, rw_ / 4) : rw_;
            rd.Height = t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, rh_ / 2) : t == kCloudMarch ? std::max(1u, rh_ / 4) : t == kProbes ? kProbeAtlasH : t == kProbeRaysTarget ? kProbesPerFrame : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, rh_ / 4) : rh_;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = t == kAo ? DXGI_FORMAT_R8_UNORM : t == kVelocity ? kVelocityFormat : kHdrFormat;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {}; // the scene is the only target cleared; this is main.cpp's editor background
            cv.Format = rd.Format;
            cv.Color[0] = 24.0f / 255.0f; cv.Color[1] = 20.0f / 255.0f; cv.Color[2] = 16.0f / 255.0f; cv.Color[3] = 1.0f;
            CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   t == kScene ? &cv : nullptr, IID_PPV_ARGS(&post_[t])));
            device_->CreateRenderTargetView(post_[t].Get(), nullptr, PostRtv(t));
        }
        { // water: refraction reads a copy of the composed picture, made only when water is drawn
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = rw_;
            rd.Height = rh_;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = kHdrFormat;
            rd.SampleDesc.Count = 1;
            waterRefract_.Reset();
            CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&waterRefract_)));
            const UINT base = WaterTableBase();
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(depth_.Get(), &sv, ShaderCpu(base));
            device_->CreateShaderResourceView(shadowMap_.Get(), &sv, ShaderCpu(base + 2));
            sv.Format = kHdrFormat;
            device_->CreateShaderResourceView(waterRefract_.Get(), &sv, ShaderCpu(base + 1));
            device_->CreateShaderResourceView(post_[kProbes].Get(), &sv, ShaderCpu(base + 3));
            device_->CreateShaderResourceView(post_[kCloudShadow].Get(), &sv, ShaderCpu(base + 4));
        }
        for (UINT table = 0; table < 2; ++table) {
            const UINT base = kPostDescriptorBase + table * kPostSrvs;
            auto srv = [&](UINT slot, ID3D12Resource* res, DXGI_FORMAT format) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
                sv.Format = format;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                device_->CreateShaderResourceView(res, &sv, ShaderCpu(base + slot));
            };
            srv(0, post_[kScene].Get(), kHdrFormat);
            srv(1, depth_.Get(), DXGI_FORMAT_R32_FLOAT);
            srv(2, post_[kAo].Get(), DXGI_FORMAT_R8_UNORM);
            srv(3, post_[table ? kClouds1 : kClouds].Get(), kHdrFormat);
            D3D12_SHADER_RESOURCE_VIEW_DESC nv = {};
            nv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nv.Texture3D.MipLevels = 1;
            device_->CreateShaderResourceView(cloudNoise_.Get(), &nv, ShaderCpu(base + 4));
            srv(5, post_[kComposed].Get(), kHdrFormat);
            srv(6, post_[kHistory0 + (1 - table)].Get(), kHdrFormat);
            srv(7, post_[kHistory0 + table].Get(), kHdrFormat);
            srv(8, shadowMap_.Get(), DXGI_FORMAT_R32_FLOAT);
            srv(9, post_[kProbes].Get(), kHdrFormat);
            srv(10, post_[kProbeRaysTarget].Get(), kHdrFormat);
            srv(11, post_[kExposure].Get(), kHdrFormat);
            srv(12, post_[kBloom].Get(), kHdrFormat);
            srv(13, post_[kCloudShadow].Get(), kHdrFormat);
            srv(14, post_[kVelocity].Get(), kVelocityFormat);
            srv(15, cloudField_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            srv(16, post_[table ? kClouds : kClouds1].Get(), kHdrFormat);
            srv(17, post_[kCloudMarch].Get(), kHdrFormat);
            srv(18, post_[kCapture].Get(), kHdrFormat);
            if (table == 1) { // and the mesh root's own table over the atlas
                D3D12_SHADER_RESOURCE_VIEW_DESC pv = {};
                pv.Format = kHdrFormat;
                pv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                pv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                pv.Texture2D.MipLevels = 1;
                device_->CreateShaderResourceView(post_[kProbes].Get(), &pv, ShaderCpu(kPostDescriptorBase + 2 * kPostSrvs));
                device_->CreateShaderResourceView(post_[kCloudShadow].Get(), &pv, ShaderCpu(kPostDescriptorBase + 2 * kPostSrvs + 1));
            }
        }
        probeAge_ = 0;
        cloudMapValid_ = false;
        historyValid_ = false;
        return true;
    }

    bool CreatePostPipelines() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kPostSrvs;
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t20: the ray tracing scene, read by the ray traced AoPS only
        params[2].Descriptor = {20, 0};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = i ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) {
            Log("D3D12: post root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
            return false;
        }
        CHECK(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&postRootSig_)));

        ComPtr<ID3DBlob> vs;
        if (!CompileShader(L"post.hlsl", "FullscreenVS", "vs_5_1", vs)) return false;
        const struct { const char* entry; DXGI_FORMAT format; } passes[11] = {
            {"AoPS", DXGI_FORMAT_R8_UNORM}, {"CloudsPS", kHdrFormat}, {"CompositePS", kHdrFormat},
            {"TaaPS", kHdrFormat}, {"TonemapPS", kColorFormat}, {"ProbeTracePS", kHdrFormat}, {"ProbeBlendPS", kHdrFormat},
            {"ExposurePS", kHdrFormat}, {"BloomPS", kHdrFormat}, {"CloudShadowPS", kHdrFormat}, {"CloudResolvePS", kHdrFormat}};
        for (int i = 0; i < 11; ++i) {
            ComPtr<ID3DBlob> ps;
            if (!CompileShader(L"post.hlsl", passes[i].entry, "ps_5_1", ps)) return false;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = postRootSig_.Get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (i == 6 || i == 7) { // the temporal filters: alpha is the new data's weight
                D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
                b.BlendEnable = TRUE;
                b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
                b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
            }
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = passes[i].format;
            pd.SampleDesc.Count = 1;
            CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&postPso_[i])));
            if (i == 0) aoPsoDesc_ = pd;
        }
        CreateRayTracedAo();
        return true;
    }

    // The ray traced AoPS: DXIL from build.bat (fxc has no ray queries), so its vertex shader is DXIL too. Without it the
    // engine runs as before.
    void CreateRayTracedAo() {
        aoRtPso_.Reset();
        if (!device5_) return;
        std::string vs, ps;
        if (!ReadAllText(ExeDirectory() + L"\\shaders\\post_fullscreen_vs.dxil", vs) || !ReadAllText(ExeDirectory() + L"\\shaders\\post_ao_rt_ps.dxil", ps)) {
            Log("D3D12: no ray traced occlusion shaders (build.bat makes them with the Windows SDK's dxc)");
            return;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = aoPsoDesc_;
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&aoRtPso_)))) Log("D3D12: the ray traced occlusion pipeline failed");
        else Log("D3D12: ray traced ambient occlusion on");
    }

    bool CreateCloudField() {
        constexpr int kN = kCloudFieldSize;
        const std::vector<uint8_t> texels = GenerateCloudField();
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kN;
        rd.Height = kN;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cloudField_)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> upload;
        if (!CreateUploadBuffer(total, upload)) return false;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        CHECK(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
        for (int y = 0; y < kN; ++y)
            std::memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, &texels[(size_t)y * kN * 4], (size_t)kN * 4);
        upload->Unmap(0, nullptr);
        frame_ = &frames_[0];
        list_ = frame_->list.Get();
        CHECK(frame_->allocator->Reset());
        CHECK(list_->Reset(frame_->allocator.Get(), nullptr));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = cloudField_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(cloudField_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CHECK(list_->Close());
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        return true;
    }

    bool CreateCloudNoise() {
        constexpr int kN = kCloudNoiseSize;
        const std::vector<uint8_t> voxels = GenerateCloudNoise();
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = kN;
        rd.Height = kN;
        rd.DepthOrArraySize = kN;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cloudNoise_)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> upload;
        if (!CreateUploadBuffer(total, upload)) return false;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        CHECK(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
        for (int z = 0; z < kN; ++z)
            for (int y = 0; y < kN; ++y)
                std::memcpy(mapped + fp.Offset + (size_t)z * fp.Footprint.RowPitch * kN + (size_t)y * fp.Footprint.RowPitch,
                            &voxels[((size_t)z * kN + y) * kN * 4], (size_t)kN * 4);
        upload->Unmap(0, nullptr);
        frame_ = &frames_[0];
        list_ = frame_->list.Get();
        CHECK(frame_->allocator->Reset());
        CHECK(list_->Reset(frame_->allocator.Get(), nullptr));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = cloudNoise_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(cloudNoise_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CHECK(list_->Close());
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        return true;
    }

    bool CreateTimers(Frame& frame) {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kTimerMarks;
        CHECK(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&frame.timerHeap)));
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kTimerMarks * sizeof(uint64_t);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&frame.timerReadback)));
        CHECK(queue_->GetTimestampFrequency(&timerFreq_));
        return true;
    }

    // The debug layer only talks to a debugger by default; put what it says in the log, as Vulkan's callback does.
    void DrainDebugMessages() {
        if (!infoQueue_) return;
        const UINT64 count = infoQueue_->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T bytes = 0;
            infoQueue_->GetMessage(i, nullptr, &bytes);
            std::vector<uint8_t> storage(bytes);
            D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue_->GetMessage(i, m, &bytes)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && debugLogged_ < 50) {
                Log("D3D12 validation: %s", m->pDescription);
                ++debugLogged_;
            }
        }
        infoQueue_->ClearStoredMessages();
    }

    // Timestamps around each stretch of the frame; a 60-frame mean of each goes to the log.
    static constexpr int kTimerMarks = 11; // start, shadow, scene, cloud shadow, probes, ao, clouds, composite, taa, tonemap, overlays
    double timerPass_[kTimerMarks - 1] = {};
    bool shadowMarked_ = false;

    void AddTimerSample(const uint64_t* t, double msPerTick) {
        for (int k = 0; k + 1 < kTimerMarks; ++k) {
            gpuPasses_[k] = (double)(t[k + 1] - t[k]) * msPerTick;
            timerPass_[k] += gpuPasses_[k];
        }
        timerSum_ += (double)(t[kTimerMarks - 1] - t[0]) * msPerTick;
    }

    void LogTimers(const char* api, uint32_t w, uint32_t h) {
        const double n = 60.0;
        Log("%s: GPU frame %.2f ms at %ux%u (60-frame mean): shadow %.2f scene %.2f cloudshadow %.2f probes %.2f ao %.2f clouds %.2f composite %.2f taa %.2f tonemap %.2f overlays %.2f",
            api, timerSum_ / n, w, h, timerPass_[0] / n, timerPass_[1] / n, timerPass_[2] / n, timerPass_[3] / n,
            timerPass_[4] / n, timerPass_[5] / n, timerPass_[6] / n, timerPass_[7] / n, timerPass_[8] / n, timerPass_[9] / n);
        timerSum_ = 0.0;
        for (double& p : timerPass_) p = 0.0;
    }

    void Mark(UINT slot) { list_->EndQuery(frame_->timerHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot); }

    void ReadTimer(Frame& frame) {
        if (!frame.timerPending) return;
        frame.timerPending = false;
        uint64_t* t = nullptr;
        const D3D12_RANGE range = {0, kTimerMarks * sizeof(uint64_t)};
        if (FAILED(frame.timerReadback->Map(0, &range, reinterpret_cast<void**>(&t)))) return;
        AddTimerSample(t, 1000.0 / (double)timerFreq_);
        const D3D12_RANGE none = {0, 0};
        frame.timerReadback->Unmap(0, &none);
        if (++timerCount_ == 60) {
            LogTimers("D3D12", w_, h_);
            LogCounts("D3D12");
            timerCount_ = 0;
        }
    }

    ComPtr<ID3D12Resource> post_[kPostTargets], cloudNoise_, cloudField_;
    ComPtr<ID3D12RootSignature> postRootSig_;
    ComPtr<ID3D12PipelineState> postPso_[11], psoOverlay_[4]; // AO, clouds, composite, TAA, tone map, probe trace, probe blend, exposure, bloom, cloud shadow; display-space overlays
    uint32_t probeAge_ = 0;
    bool cloudMapValid_ = false;
    ComPtr<ID3D12InfoQueue> infoQueue_;
    int debugLogged_ = 0;
    FrameView view_;
    bool resolved_ = false, historyValid_ = false;
    UINT postFrame_ = 0, timerCount_ = 0;
    UINT64 timerFreq_ = 1;
    double timerSum_ = 0.0;

    static constexpr UINT kInstanceStride = 12 * sizeof(float);
    struct InstanceBuffer {
        ComPtr<ID3D12Resource> buffer;
        D3D12_VERTEX_BUFFER_VIEW view = {};
    };
    std::vector<InstanceBuffer> instanceBuffers_;
    std::vector<InstanceBufferId> freeInstanceSlots_;
    ComPtr<ID3D12PipelineState> instancedPso_, shadowInstancedPso_, tightInstancedPso_;
    ComPtr<ID3D12PipelineState> prepassPso_[2][2]; // [mesh, instanced][depth only, shade after]
    int prepass_ = 0;                              // SetScenePrepass
    bool shadowInstancedBound_ = false;

    // ------------------------------------------------------------------ hardware ray tracing (DXR 1.1 ray queries)
    bool RayTracingSupported() const override { return aoRtPso_ != nullptr; }

    ComPtr<ID3D12Resource> CreateRayBuffer(UINT64 bytes, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<UINT64>(bytes, 256);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> out;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out)))) return nullptr;
        return out;
    }

    void UavBarrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = resource;
        list_->ResourceBarrier(1, &b);
    }

    // A mesh's own structure, built once (fast to trace; its scratch lives until this frame slot comes round again).
    bool BuildBlas(ID3D12GraphicsCommandList4* l4, MeshId id) {
        auto& m = meshes_[id];
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer = {m.view.BufferLocation, sizeof(Vertex)};
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = m.view.SizeInBytes / sizeof(Vertex);
        g.Triangles.IndexBuffer = m.indexView.BufferLocation;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount = m.count;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        ComPtr<ID3D12Resource> scratch = CreateRayBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.blas = CreateRayBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!scratch || !m.blas) { m.blas.Reset(); return false; }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = m.blas->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        l4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        rayScratch_[frameSlot_].push_back(std::move(scratch));
        return true;
    }

    // Records this frame's structures: the newly listed meshes' own (kBlasPerFrame at most, so a big load never stalls a
    // frame), then the scene over everything built. False when there is nothing to trace.
    bool BuildRayScene() {
        if (!aoRtPso_ || rayScene_.empty()) return false;
        ComPtr<ID3D12GraphicsCommandList4> l4;
        if (FAILED(list_->QueryInterface(IID_PPV_ARGS(&l4)))) return false;
        RayFrame& rf = rayFrames_[frameSlot_];
        if (!rf.instances) {
            if (!CreateUploadBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * kMaxRayInstances, rf.instances)) return false;
            D3D12_RANGE noRead = {0, 0};
            if (FAILED(rf.instances->Map(0, &noRead, reinterpret_cast<void**>(&rf.mapped)))) { rf.instances.Reset(); return false; }
        }
        constexpr uint32_t kBlasPerFrame = 48;
        uint32_t n = 0, built = 0;
        for (const RayInstance& ri : rayScene_) {
            if (ri.mesh >= meshes_.size()) continue;
            auto& m = meshes_[ri.mesh];
            if (!m.buffer || !m.indexed || m.copyBytes || m.copyParent != kInvalidMesh || m.topology != Topology::Triangles || m.count < 3) continue;
            if (!m.blas) {
                if (built >= kBlasPerFrame || !BuildBlas(l4.Get(), ri.mesh)) continue;
                ++built;
            }
            D3D12_RAYTRACING_INSTANCE_DESC& d = rf.mapped[n++];
            std::memcpy(d.Transform, ri.rows, sizeof(d.Transform));
            d.InstanceID = 0;
            d.InstanceMask = 0xFF;
            d.InstanceContributionToHitGroupIndex = 0;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            d.AccelerationStructure = m.blas->GetGPUVirtualAddress();
        }
        if (built) UavBarrier(nullptr);
        if (!n) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs = n;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.InstanceDescs = rf.instances->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        if (info.ResultDataMaxSizeInBytes > rf.tlasBytes) {
            rf.tlas = CreateRayBuffer(info.ResultDataMaxSizeInBytes * 2, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            rf.tlasBytes = rf.tlas ? info.ResultDataMaxSizeInBytes * 2 : 0;
        }
        if (info.ScratchDataSizeInBytes > rf.scratchBytes) {
            rf.scratch = CreateRayBuffer(info.ScratchDataSizeInBytes * 2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rf.scratchBytes = rf.scratch ? info.ScratchDataSizeInBytes * 2 : 0;
        }
        if (!rf.tlas || !rf.scratch) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs = in;
        bd.DestAccelerationStructureData = rf.tlas->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = rf.scratch->GetGPUVirtualAddress();
        l4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        UavBarrier(rf.tlas.Get());
        return true;
    }

    struct RayFrame {
        ComPtr<ID3D12Resource> instances, tlas, scratch; // instances: upload, kMaxRayInstances descs, mapped
        D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
        UINT64 tlasBytes = 0, scratchBytes = 0;
    };
    ComPtr<ID3D12Device5> device5_; // set when the device has DXR 1.1
    RayFrame rayFrames_[kFramesInFlight];
    std::vector<ComPtr<ID3D12Resource>> rayScratch_[kFramesInFlight];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC aoPsoDesc_ = {};
    ComPtr<ID3D12PipelineState> aoRtPso_;

    struct Mesh {
        ComPtr<ID3D12Resource> buffer, upload; // upload: deformable only, the staging copies
        D3D12_VERTEX_BUFFER_VIEW view = {};
        D3D12_INDEX_BUFFER_VIEW indexView = {};
        bool indexed = false;
        uint32_t count = 0; // indices when indexed, vertices otherwise
        Topology topology = Topology::Triangles;
        UINT64 copyBytes = 0;      // deformable: bytes of one frame's vertices (0 = not deformable)
        uint8_t* deformable = nullptr; // deformable parent: the persistently mapped staging buffer (nullptr on parts)
        UINT newestCopy = 0;           // deformable parent: which of the two device-local copies is this frame's
        MeshId copyParent = kInvalidMesh; // a part: the mesh holding newestCopy
        ComPtr<ID3D12Resource> blas;      // its ray tracing structure, made the first time SetRayScene lists it
        uint32_t block = UINT32_MAX;      // streamed: the range of meshBlocks_[block] it lives in
        UINT64 blockOffset = 0, blockBytes = 0;
    };
    static constexpr D3D12_RESOURCE_STATES kDeformableReadState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
    std::vector<MeshId> pendingDeformables_;

    MeshId PlaceMesh(Mesh&& mesh) {
        if (!freeSlots_.empty()) {
            const MeshId id = freeSlots_.back();
            freeSlots_.pop_back();
            meshes_[id] = std::move(mesh);
            return id;
        }
        meshes_.push_back(std::move(mesh));
        return (MeshId)(meshes_.size() - 1);
    }
    // The vertices a draw of this frame reads.
    UINT NewestCopy(const Mesh& mesh) const { return mesh.copyParent != kInvalidMesh ? meshes_[mesh.copyParent].newestCopy : mesh.newestCopy; }
    D3D12_VERTEX_BUFFER_VIEW DrawView(const Mesh& mesh) const {
        D3D12_VERTEX_BUFFER_VIEW view = mesh.view;
        view.BufferLocation += mesh.copyBytes * NewestCopy(mesh);
        return view;
    }

    // What a no-wait upload or a retired resource must keep until no submitted work can use it.
    struct StreamRetired {
        uint64_t frame = 0;
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        MeshId mesh = kInvalidMesh;
        TextureId texture = kNoTexture;
    };

    // ------------------------------------------------------------------ streaming uploads
    // A streamed mesh used to cost two committed resources, a command allocator, a command list and an ExecuteCommandLists of
    // its own: 5-30 ms each on the main thread. Now it is a range of a big device-local block, a memcpy into one persistently
    // mapped staging ring, and a copy recorded into the frame's one stream list, which EndFrame submits ahead of the frame.
    // The ring and the stream allocators come back by polling the frame fence, never by waiting on it.
    static constexpr UINT64 kStreamRingBytes = 128ull << 20;
    static constexpr UINT64 kMeshBlockBytes = 64ull << 20;
    static constexpr UINT64 kMeshAlign = 256;
    struct MeshBlock {
        ComPtr<ID3D12Resource> buffer;
        UINT64 size = 0;
        std::map<UINT64, UINT64> free; // offset -> bytes
        bool touched = false;          // copied into by the open stream list
    };
    struct StreamTransition { ComPtr<ID3D12Resource> resource; D3D12_RESOURCE_STATES before, after; };
    struct StreamAllocator { ComPtr<ID3D12CommandAllocator> allocator; UINT64 fence = 0; };
    struct RingMark { UINT64 fence, head; };
    std::vector<MeshBlock> meshBlocks_;
    std::future<ComPtr<ID3D12Resource>> spareBlock_; // the next block, made on another thread (the device is free-threaded)
    ComPtr<ID3D12Resource> streamRing_;
    uint8_t* streamRingMapped_ = nullptr;
    UINT64 streamHead_ = 0, streamTail_ = 0; // bytes ever handed out / ever given back; [tail, head) may still be read
    std::deque<RingMark> streamMarks_;
    std::vector<StreamAllocator> streamAllocators_;
    ComPtr<ID3D12GraphicsCommandList> streamList_;
    size_t streamAllocatorInUse_ = 0;
    bool streamOpen_ = false;
    std::vector<StreamTransition> streamTransitions_;
    uint32_t streamOverflows_ = 0;

    ComPtr<ID3D12Resource> MakeBlockBuffer(UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)))) return nullptr;
        return out;
    }

    void StartSpareBlock() {
        if (spareBlock_.valid()) return;
        spareBlock_ = std::async(std::launch::async, [this] { return MakeBlockBuffer(kMeshBlockBytes); });
    }

    bool CreateDefaultBuffer(UINT64 bytes, ComPtr<ID3D12Resource>& out) {
        out = MakeBlockBuffer(bytes);
        return out != nullptr;
    }

    // First fit in the blocks; a new block when none has room (the spare, made ahead on another thread, when it is ready).
    bool MeshMemory(UINT64 bytes, uint32_t& block, UINT64& offset, UINT64& taken) {
        taken = (bytes + kMeshAlign - 1) / kMeshAlign * kMeshAlign;
        for (uint32_t b = 0; b < (uint32_t)meshBlocks_.size(); ++b) {
            auto& free = meshBlocks_[b].free;
            for (auto it = free.begin(); it != free.end(); ++it) {
                if (it->second < taken) continue;
                offset = it->first;
                const UINT64 rest = it->second - taken;
                free.erase(it);
                if (rest) free[offset + taken] = rest;
                block = b;
                return true;
            }
        }
        MeshBlock nb;
        nb.size = std::max(kMeshBlockBytes, taken);
        if (nb.size == kMeshBlockBytes && spareBlock_.valid() && spareBlock_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) nb.buffer = spareBlock_.get();
        if (!nb.buffer) nb.buffer = MakeBlockBuffer(nb.size);
        if (!nb.buffer) return false;
        StartSpareBlock();
        if (nb.size > taken) nb.free[taken] = nb.size - taken;
        offset = 0;
        block = (uint32_t)meshBlocks_.size();
        meshBlocks_.push_back(std::move(nb));
        return true;
    }

    void FreeMeshMemory(Mesh& mesh) {
        if (mesh.block >= meshBlocks_.size() || !mesh.blockBytes) return;
        auto& free = meshBlocks_[mesh.block].free;
        UINT64 at = mesh.blockOffset, bytes = mesh.blockBytes;
        auto next = free.lower_bound(at);
        if (next != free.end() && next->first == at + bytes) { bytes += next->second; next = free.erase(next); }
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) { at = prev->first; bytes += prev->second; free.erase(prev); }
        }
        free[at] = bytes;
        mesh.block = UINT32_MAX;
        mesh.blockBytes = 0;
    }

    // Streamed textures: placed resources in 256 MB heaps (a spare one made ahead on another thread), first fit with alignment.
    static constexpr UINT64 kTexHeapBytes = 256ull << 20;
    struct TexHeap {
        ComPtr<ID3D12Heap> heap;
        UINT64 size = 0;
        std::map<UINT64, UINT64> free; // offset -> bytes
    };
    std::vector<TexHeap> texHeaps_;
    std::future<ComPtr<ID3D12Heap>> spareTexHeap_;

    ComPtr<ID3D12Heap> MakeTexHeap(UINT64 bytes) {
        D3D12_HEAP_DESC hd = {};
        hd.SizeInBytes = bytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        ComPtr<ID3D12Heap> heap;
        if (FAILED(device_->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) return nullptr;
        return heap;
    }

    void StartSpareTexHeap() {
        if (spareTexHeap_.valid()) return;
        spareTexHeap_ = std::async(std::launch::async, [this] { return MakeTexHeap(kTexHeapBytes); });
    }

    static bool TakeRange(std::map<UINT64, UINT64>& free, UINT64 bytes, UINT64 align, UINT64& offset) {
        for (auto it = free.begin(); it != free.end(); ++it) {
            const UINT64 at = it->first, size = it->second;
            const UINT64 start = (at + align - 1) / align * align;
            if (start + bytes > at + size) continue;
            free.erase(it);
            if (start > at) free[at] = start - at;
            if (start + bytes < at + size) free[start + bytes] = at + size - (start + bytes);
            offset = start;
            return true;
        }
        return false;
    }

    static void GiveRange(std::map<UINT64, UINT64>& free, UINT64 at, UINT64 bytes) {
        auto next = free.lower_bound(at);
        if (next != free.end() && next->first == at + bytes) { bytes += next->second; next = free.erase(next); }
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) { at = prev->first; bytes += prev->second; free.erase(prev); }
        }
        free[at] = bytes;
    }

    // False when the texture is bigger than a heap (it is committed instead).
    bool TextureMemory(UINT64 bytes, UINT64 align, uint32_t& heap, UINT64& offset, UINT64& taken) {
        if (bytes == 0 || bytes > kTexHeapBytes) return false;
        taken = bytes;
        for (uint32_t h = 0; h < (uint32_t)texHeaps_.size(); ++h)
            if (TakeRange(texHeaps_[h].free, bytes, align, offset)) { heap = h; return true; }
        TexHeap th;
        th.size = kTexHeapBytes;
        if (spareTexHeap_.valid() && spareTexHeap_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) th.heap = spareTexHeap_.get();
        if (!th.heap) th.heap = MakeTexHeap(kTexHeapBytes);
        if (!th.heap) return false;
        StartSpareTexHeap();
        th.free[0] = th.size;
        texHeaps_.push_back(std::move(th));
        heap = (uint32_t)texHeaps_.size() - 1;
        return TakeRange(texHeaps_.back().free, bytes, align, offset);
    }

    // The resource goes first: a placed resource must not outlive its range being handed out again.
    void FreeTextureMemory(Tex& tex) {
        if (tex.heap >= texHeaps_.size() || !tex.heapBytes) return;
        tex.resource.Reset();
        GiveRange(texHeaps_[tex.heap].free, tex.heapOffset, tex.heapBytes);
        tex.heap = UINT32_MAX;
        tex.heapBytes = 0;
    }

    // Where this frame's copy reads from: a piece of the ring, or (the ring is full: a burst bigger than it) a buffer of its
    // own that lives until the frames that read it are done. Returns the CPU pointer; `source` and `base` locate it for the GPU.
    uint8_t* StreamStaging(UINT64 bytes, UINT64 align, ID3D12Resource*& source, UINT64& base) {
        const UINT64 done = fence_->GetCompletedValue();
        while (!streamMarks_.empty() && streamMarks_.front().fence <= done) {
            streamTail_ = streamMarks_.front().head;
            streamMarks_.pop_front();
        }
        UINT64 start = (streamHead_ + align - 1) / align * align;
        const UINT64 pos = start % kStreamRingBytes;
        if (pos + bytes > kStreamRingBytes) start += kStreamRingBytes - pos; // never across the end
        if (streamRingMapped_ && bytes <= kStreamRingBytes && start + bytes - streamTail_ <= kStreamRingBytes) {
            streamHead_ = start + bytes;
            source = streamRing_.Get();
            base = start % kStreamRingBytes;
            return streamRingMapped_ + base;
        }
        StreamRetired keep;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        if (!CreateUploadBuffer(bytes, keep.resource) || FAILED(keep.resource->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return nullptr;
        if (++streamOverflows_ % 64 == 1) Log("D3D12: the stream ring is full; %u uploads have had buffers of their own", streamOverflows_);
        source = keep.resource.Get();
        base = 0;
        keep.frame = streamFrame_;
        streamRetired_.push_back(std::move(keep)); // an upload heap buffer may stay mapped while the GPU reads it
        return mapped;
    }

    bool StreamListOpen() {
        if (streamOpen_) return true;
        const UINT64 done = fence_->GetCompletedValue();
        size_t pick = streamAllocators_.size();
        for (size_t i = 0; i < streamAllocators_.size(); ++i)
            if (streamAllocators_[i].fence <= done) { pick = i; break; }
        if (pick == streamAllocators_.size()) {
            StreamAllocator a;
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.allocator)))) return false;
            streamAllocators_.push_back(std::move(a));
        } else if (FAILED(streamAllocators_[pick].allocator->Reset())) return false;
        ID3D12CommandAllocator* allocator = streamAllocators_[pick].allocator.Get();
        if (!streamList_) {
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&streamList_)))) return false;
        } else if (FAILED(streamList_->Reset(allocator, nullptr))) return false;
        streamAllocators_[pick].fence = UINT64_MAX; // recording: not free until its submission has run
        streamAllocatorInUse_ = pick;
        streamOpen_ = true;
        return true;
    }

    bool StreamCopy(const void* a, UINT64 aBytes, const void* b, UINT64 bBytes, ID3D12Resource* dst, UINT64 dstOffset) {
        ID3D12Resource* source = nullptr;
        UINT64 base = 0;
        uint8_t* mapped = StreamStaging(aBytes + bBytes, 16, source, base);
        if (!mapped || !StreamListOpen()) return false;
        std::memcpy(mapped, a, (size_t)aBytes);
        if (bBytes) std::memcpy(mapped + aBytes, b, (size_t)bBytes);
        streamList_->CopyBufferRegion(dst, dstOffset, source, base, aBytes + bBytes);
        return true;
    }

    // Ahead of the frame's own list, in a submission of its own: buffers go back to the common state between submissions,
    // and this frame's draws of what was just streamed then find it copied. `fence` is the value signaled after the frame.
    void SubmitStreamList(UINT64 fence) {
        if (!streamOpen_) return;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(streamTransitions_.size());
        for (const StreamTransition& t : streamTransitions_) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = t.resource.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t.before;
            b.Transition.StateAfter = t.after;
            barriers.push_back(b);
        }
        if (!barriers.empty()) streamList_->ResourceBarrier((UINT)barriers.size(), barriers.data());
        streamTransitions_.clear();
        for (MeshBlock& block : meshBlocks_) block.touched = false;
        streamList_->Close();
        ID3D12CommandList* lists[] = {streamList_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        streamAllocators_[streamAllocatorInUse_].fence = fence;
        streamMarks_.push_back({fence, streamHead_});
        streamOpen_ = false;
    }

    // Frame U's fence has passed by BeginFrame U+2 (same slot); work queued after U runs before U+1's, so U+3 covers both.
    void FlushStreamRetired() {
        ++streamFrame_;
        size_t kept = 0;
        // Unloading a town frees thousands of objects, and each can cost a good part of a millisecond: 2 ms per frame.
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        for (size_t i = 0; i < streamRetired_.size(); ++i) {
            StreamRetired& r = streamRetired_[i];
            QueryPerformanceCounter(&now);
            if (r.frame + kFramesInFlight + 1 > streamFrame_ || (now.QuadPart - start.QuadPart) * 500 > freq.QuadPart) {
                if (kept != i) streamRetired_[kept] = std::move(r);
                ++kept;
                continue;
            }
            if (r.mesh != kInvalidMesh) { FreeMeshMemory(meshes_[r.mesh]); meshes_[r.mesh] = Mesh{}; freeSlots_.push_back(r.mesh); }
            if (r.texture != kNoTexture) { FreeTextureMemory(textures_[r.texture]); textures_[r.texture] = Tex{}; if (r.texture < extraOf_.size()) extraOf_[r.texture] = kNoTexture; freeTexSlots_.push_back(r.texture); }
            r = StreamRetired{};
        }
        streamRetired_.resize(kept);
    }

    bool CreateUploadBuffer(UINT64 bytes, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&out)));
        return true;
    }

    bool CreateTargets() {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            CHECK(swap_->GetBuffer(i, IID_PPV_ARGS(&back_[i])));
            device_->CreateRenderTargetView(back_[i].Get(), nullptr, h);
            h.ptr += rtvSize_;
        }

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rw_;
        rd.Height = rh_;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS; // written as D32_FLOAT, read by the post passes as R32_FLOAT
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = 0.0f;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                               IID_PPV_ARGS(&depth_)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(depth_.Get(), &dsvDesc, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
        dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device_->CreateDepthStencilView(depth_.Get(), &dsvDesc, WaterDsv());

        // Below 1.0 the scene's depth is smaller than the window, so the overlays get a window-sized one of their own.
        overlayDepth_.Reset();
        if (scale_ < 1.0f) {
            rd.Width = w_;
            rd.Height = h_;
            rd.Format = kDepthFormat;
            CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                   IID_PPV_ARGS(&overlayDepth_)));
            device_->CreateDepthStencilView(overlayDepth_.Get(), nullptr, OverlayDsv());
        }
        return true;
    }

    static constexpr UINT WaterTableBase() { return kPostDescriptorBase + 2 * kPostSrvs + 2; }

    D3D12_CPU_DESCRIPTOR_HANDLE WaterDsv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)5 * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE OverlayDsv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)3 * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }

    // The internal resolution for the current window size and render scale.
    void UpdateRenderSize() {
        rw_ = (UINT)ScaledSize((int)w_, scale_);
        rh_ = (UINT)ScaledSize((int)h_, scale_);
    }

    bool CreatePipelines() {
        // Root signature: 32 root constants (mvp + model) for the vertex shader, the per-frame lighting constant
        // buffer (b1) as a root descriptor, and the shadow map (t0) and tight shadow map (t4) with their comparison sampler (s0).
        D3D12_DESCRIPTOR_RANGE shadowRanges[2] = {};
        shadowRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRanges[0].NumDescriptors = 1;
        shadowRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        shadowRanges[1] = shadowRanges[0];
        shadowRanges[1].BaseShaderRegister = 4; // t1-t3 are the material's
        D3D12_DESCRIPTOR_RANGE materialRanges[2] = {};
        materialRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        materialRanges[0].NumDescriptors = 3; // t1 albedo, t2 normal map, t3 surface
        materialRanges[0].BaseShaderRegister = 1;
        materialRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        materialRanges[1] = materialRanges[0]; // the extra map: t0 in space1, right after the three
        materialRanges[1].NumDescriptors = 1;
        materialRanges[1].BaseShaderRegister = 0;
        materialRanges[1].RegisterSpace = 1;
        D3D12_DESCRIPTOR_RANGE probeRange = {}; // t5: the probe atlas
        probeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        probeRange.NumDescriptors = 2; // t6: the cloud shadow map
        probeRange.BaseShaderRegister = 5;
        D3D12_ROOT_PARAMETER params[7] = {};
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b3: the material's constants (materialConstants_)
        params[6].Constants.ShaderRegister = 3;
        params[6].Constants.Num32BitValues = 4;
        params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[5].Descriptor.ShaderRegister = 2; // b2: motion data
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable = {1, &probeRange};
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 32;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {2, shadowRanges};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable = {2, materialRanges};
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        D3D12_STATIC_SAMPLER_DESC& shadowSampler = samplers[0];
        shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
        shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC& texSampler = samplers[1]; // s1: material textures
        texSampler.Filter = D3D12_FILTER_ANISOTROPIC;
        texSampler.AddressU = texSampler.AddressV = texSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        texSampler.MaxAnisotropy = 8;
        texSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        texSampler.MaxLOD = D3D12_FLOAT32_MAX;
        texSampler.ShaderRegister = 1;
        texSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 7;
        rsd.pParameters = params;
        for (UINT i = 0; i < kFramesInFlight && !motionBuffer_[i]; ++i) { // not again on a shader reload
            if (!CreateUploadBuffer(256, motionBuffer_[i])) return false;
            D3D12_RANGE noRead = {0, 0};
            CHECK(motionBuffer_[i]->Map(0, &noRead, reinterpret_cast<void**>(&motionPtr_[i])));
            std::memset(motionPtr_[i], 0, 256);
        }
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = samplers;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsError))) {
            Log("D3D12: root signature: %s", rsError ? (const char*)rsError->GetBufferPointer() : "?");
            return false;
        }
        CHECK(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSig_)));

        // Shaders are compiled from source at startup.
        const std::wstring path = ExeDirectory() + L"\\shaders\\mesh.hlsl";
        ComPtr<ID3DBlob> vs, ps, overlayPs, err;
        if (FAILED(CompileCached(path, nullptr, "VSMain", "vs_5_1", vs, err))) {
            Log("D3D12: vertex shader: %s", err ? (const char*)err->GetBufferPointer() : "file not found");
            return false;
        }
        err.Reset();
        if (FAILED(CompileCached(path, nullptr, "PSMain", "ps_5_1", ps, err))) {
            Log("D3D12: pixel shader: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }

        if (!CompileShader(L"mesh.hlsl", "PSOverlay", "ps_5_1", overlayPs)) return false;

        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        const D3D12_INPUT_ELEMENT_DESC instancedLayout[] = {
            layout[0], layout[1], layout[2], layout[3],
            {"INSTROW", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTROW", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTROW", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC chunkScenePd = {};
        for (int i = 0; i < 8; ++i) { // 0-3 draw into the HDR scene, 4-7 are the same states for display-space overlays
            const bool lines = (i & 3) >= 2;
            const bool depth = (i % 2) == 0;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig_.Get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            ID3DBlob* const pixel = i < 4 ? ps.Get() : overlayPs.Get();
            pd.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.DepthStencilState.DepthEnable = depth ? TRUE : FALSE;
            pd.DepthStencilState.DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pd.InputLayout = {layout, 4};
            pd.PrimitiveTopologyType = lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = i < 4 ? 2 : 1;
            pd.BlendState.IndependentBlendEnable = i < 4 ? TRUE : FALSE; // the velocity target stays untouched unless a pipeline says otherwise
            pd.RTVFormats[0] = i < 4 ? kHdrFormat : kColorFormat;
            pd.RTVFormats[1] = i < 4 ? kVelocityFormat : DXGI_FORMAT_UNKNOWN;
            pd.DSVFormat = kDepthFormat;
            pd.SampleDesc.Count = 1;
            CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(i < 4 ? &pso_[i] : &psoOverlay_[i - 4])));
            if (i == 0) chunkScenePd = pd; // the chunk volumes draw like the scene's lit triangles
            if (i == 0) { // instanced draws: the HDR scene's lit triangles, with the second vertex stream
                ComPtr<ID3DBlob> instancedVs;
                if (!CompileShader(L"mesh.hlsl", "VSInstanced", "vs_5_1", instancedVs)) return false;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC ipd = pd;
                ipd.VS = {instancedVs->GetBufferPointer(), instancedVs->GetBufferSize()};
                ipd.InputLayout = {instancedLayout, 7};
                CHECK(device_->CreateGraphicsPipelineState(&ipd, IID_PPV_ARGS(&instancedPso_)));
                // The depth pre-pass (SetScenePrepass): the same vertex shaders, so depth matches to the bit. Depth only: no pixel
                // shader, nothing written to the targets. Then shading: depth equal (GREATER_EQUAL, reversed z), no depth write.
                for (int v = 0; v < 2; ++v) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC depthOnly = v ? ipd : pd, shade = v ? ipd : pd;
                    depthOnly.PS = {};
                    depthOnly.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
                    depthOnly.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    shade.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                    shade.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    CHECK(device_->CreateGraphicsPipelineState(&depthOnly, IID_PPV_ARGS(&prepassPso_[v][0])));
                    CHECK(device_->CreateGraphicsPipelineState(&shade, IID_PPV_ARGS(&prepassPso_[v][1])));
                }
                ComPtr<ID3DBlob> vegetationVs;
                if (!CompileShader(L"mesh.hlsl", "VSVegetation", "vs_5_1", vegetationVs)) return false;
                ipd.VS = {vegetationVs->GetBufferPointer(), vegetationVs->GetBufferSize()};
                ComPtr<ID3DBlob> motionPs, skinnedVs;
                if (!CompileShader(L"mesh.hlsl", "PSMainMotion", "ps_5_1", motionPs) || !CompileShader(L"mesh.hlsl", "VSSkinnedMotion", "vs_5_1", skinnedVs)) return false;
                ipd.PS = {motionPs->GetBufferPointer(), motionPs->GetBufferSize()};
                ipd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                CHECK(device_->CreateGraphicsPipelineState(&ipd, IID_PPV_ARGS(&vegetationPso_)));
                {
                    ComPtr<ID3DBlob> depthVs, depthPs, shadePs;
                    if (!CompileShader(L"mesh.hlsl", "VSVegetationDepth", "vs_5_1", depthVs) || !CompileShader(L"mesh.hlsl", "PSVegetationDepth", "ps_5_1", depthPs) ||
                        !CompileShader(L"mesh.hlsl", "PSVegetationShade", "ps_5_1", shadePs))
                        return false;
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC dpd = ipd;
                    dpd.VS = {depthVs->GetBufferPointer(), depthVs->GetBufferSize()};
                    dpd.PS = {depthPs->GetBufferPointer(), depthPs->GetBufferSize()};
                    dpd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
                    dpd.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    CHECK(device_->CreateGraphicsPipelineState(&dpd, IID_PPV_ARGS(&vegetationDepthPso_)));
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC spd = ipd;
                    spd.PS = {shadePs->GetBufferPointer(), shadePs->GetBufferSize()};
                    spd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    spd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                    CHECK(device_->CreateGraphicsPipelineState(&spd, IID_PPV_ARGS(&vegetationShadePso_)));
                }
                const D3D12_INPUT_ELEMENT_DESC skinnedLayout[] = {layout[0], layout[1], layout[2], layout[3],
                    {"PREVPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
                ipd.VS = {skinnedVs->GetBufferPointer(), skinnedVs->GetBufferSize()};
                ipd.InputLayout = {skinnedLayout, 5};
                CHECK(device_->CreateGraphicsPipelineState(&ipd, IID_PPV_ARGS(&skinnedMotionPso_)));
            }
        }
        if (!CreateChunkPipelines(layout, instancedLayout, chunkScenePd)) return false;
        return CreateShadowResources(layout, instancedLayout);
    }

    // The sun's shadow map, the tight character map and the depth-only pipelines that fill them.
    bool CreateShadowResources(const D3D12_INPUT_ELEMENT_DESC (&layout)[4], const D3D12_INPUT_ELEMENT_DESC (&instancedLayout)[7]) {
        if (!shadowMap_) { // the maps and their descriptors are made once; a shader reload rebuilds only the pipelines below
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kShadowMapSize;
        rd.Height = kShadowMapSize;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS; // written as D32_FLOAT, sampled as R32_FLOAT
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = 1.0f;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &cv, IID_PPV_ARGS(&shadowMap_)));
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, &cv, IID_PPV_ARGS(&sunCache_)));
        rd.Width = kTightShadowMapSize;
        rd.Height = kTightShadowMapSize;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               &cv, IID_PPV_ARGS(&tightMap_)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = kDepthFormat;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(shadowMap_.Get(), &dv, ShadowDsv());
        device_->CreateDepthStencilView(tightMap_.Get(), &dv, TightDsv());

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1 + kMaterialTextures * kMaterialPairs * kFramesInFlight + 2 * kPostSrvs + 2 + 5 + 1; // shadow map (the tight one rides in the slot after it), per-frame material triples, post tables, the UI font atlas (UiAtlasSlot)
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&shadowSrvHeap_)));
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(shadowMap_.Get(), &sv, shadowSrvHeap_->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE tightSrv = shadowSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        tightSrv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); // srvInc_ isn't set yet
        device_->CreateShaderResourceView(tightMap_.Get(), &sv, tightSrv);
        }

        ComPtr<ID3DBlob> vs;
        if (!CompileShader(L"mesh.hlsl", "ShadowVS", "vs_5_1", vs)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.RasterizerState.DepthBias = kShadowDepthBias;
        pd.RasterizerState.SlopeScaledDepthBias = kShadowSlopeBias;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.InputLayout = {layout, 4};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.DSVFormat = kDepthFormat;
        pd.SampleDesc.Count = 1;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadowPso_)));
        pd.RasterizerState.DepthBias = kTightDepthBias; // same pipeline, with the bias suited to the tight map's short depth range
        pd.RasterizerState.SlopeScaledDepthBias = kTightSlopeBias;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tightPso_)));
        // The same two, for instanced casters.
        ComPtr<ID3DBlob> instancedVs;
        if (!CompileShader(L"mesh.hlsl", "ShadowInstancedVS", "vs_5_1", instancedVs)) return false;
        pd.VS = {instancedVs->GetBufferPointer(), instancedVs->GetBufferSize()};
        pd.InputLayout = {instancedLayout, 7};
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tightInstancedPso_)));
        pd.RasterizerState.DepthBias = kShadowDepthBias;
        pd.RasterizerState.SlopeScaledDepthBias = kShadowSlopeBias;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadowInstancedPso_)));
        ComPtr<ID3DBlob> vegetationShadowVs;
        if (!CompileShader(L"mesh.hlsl", "ShadowVegetationVS", "vs_5_1", vegetationShadowVs)) return false;
        pd.VS = {vegetationShadowVs->GetBufferPointer(), vegetationShadowVs->GetBufferSize()};
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&vegetationShadowPso_)));
        // Hair cards: alpha-tested depth (DrawShadowCasterCutout), both maps.
        ComPtr<ID3DBlob> cutoutVs, cutoutPs;
        if (!CompileShader(L"mesh.hlsl", "ShadowCutoutVS", "vs_5_1", cutoutVs) || !CompileShader(L"mesh.hlsl", "PSShadowCutout", "ps_5_1", cutoutPs)) return false;
        pd.VS = {cutoutVs->GetBufferPointer(), cutoutVs->GetBufferSize()};
        pd.PS = {cutoutPs->GetBufferPointer(), cutoutPs->GetBufferSize()};
        pd.InputLayout = {layout, 4};
        pd.RasterizerState.DepthBias = kShadowDepthBias;
        pd.RasterizerState.SlopeScaledDepthBias = kShadowSlopeBias;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadowCutoutPso_)));
        pd.RasterizerState.DepthBias = kTightDepthBias;
        pd.RasterizerState.SlopeScaledDepthBias = kTightSlopeBias;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tightCutoutPso_)));
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDsv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)2 * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE TightDsv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)4 * device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }

    // Compute: one root signature with no descriptor tables; the buffers bind by address.
    struct GpuBuffer {
        ComPtr<ID3D12Resource> buffer;
        GpuBufferUse use = GpuBufferUse::Read;
        uint64_t bytes = 0;
        D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON;
    };
    std::vector<GpuBuffer> gpuBuffers_;
    std::vector<ComPtr<ID3D12PipelineState>> computePsos_;
    ComPtr<ID3D12RootSignature> computeRootSig_;
    ComPtr<ID3D12CommandSignature> drawIndexedSignature_;
    ComPtr<ID3D12PipelineState> vegetationPso_, vegetationShadowPso_, skinnedMotionPso_;
    ComPtr<ID3D12PipelineState> shadowCutoutPso_, tightCutoutPso_; // hair cards into the shadow maps, alpha tested
    ComPtr<ID3D12PipelineState> vegetationDepthPso_, vegetationShadePso_;
    int vegetationPass_ = 0;
    ComPtr<ID3D12Resource> motionBuffer_[kFramesInFlight];
    uint8_t* motionPtr_[kFramesInFlight] = {};
    bool forceShadowRebind_ = false;

    bool ImmediateList(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
            return false;
        record(cl.Get());
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        return true;
    }

    // ------------------------------------------------------------------ smooth UI (DrawUi, shaders/ui.hlsl)
    // Its own root signature: 16 root constants (the ortho matrix), one SRV table (the SDF font atlas, copied into a
    // spare slot of the shader-visible heap by SetUiAtlas) and a static trilinear clamp sampler. Straight-alpha blending,
    // no depth. Optional: if it fails to build, UiSupported() is false and the GUI falls back to DrawTransient.
    ComPtr<ID3D12RootSignature> uiRootSig_;
    ComPtr<ID3D12PipelineState> uiPso_;
    TextureId uiAtlas_ = kNoTexture; // SetUiAtlas; put back into the slot when the pipeline is rebuilt
    static constexpr UINT UiAtlasSlot() { return WaterTableBase() + 5; } // the heap's last slot, after the water table

    // The atlas, or the default white texture so a stray text quad still samples something valid.
    void WriteUiAtlasSlot() {
        const bool valid = uiAtlas_ < textures_.size() && textures_[uiAtlas_].resource;
        device_->CopyDescriptorsSimple(1, ShaderCpu(UiAtlasSlot()), TexCpu(valid ? uiAtlas_ : kMaxTextures), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    bool CreateUiPipeline() {
        uiPso_.Reset();
        uiRootSig_.Reset();
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 16;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &sampler;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error, vs, ps;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error)) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&uiRootSig_)))) {
            Log("D3D12: UI root signature failed; the GUI draws the classic way");
            return false;
        }
        if (!CompileShader(L"ui.hlsl", "UiVS", "vs_5_1", vs) || !CompileShader(L"ui.hlsl", "UiPS", "ps_5_1", ps)) {
            Log("D3D12: ui.hlsl did not compile; the GUI draws the classic way");
            return false;
        }
        const D3D12_INPUT_ELEMENT_DESC layout[] = { // render/ui_vertex.h
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = uiRootSig_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.InputLayout = {layout, (UINT)std::size(layout)};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kColorFormat;
        pd.DSVFormat = kDepthFormat; // the overlay phase keeps a depth buffer bound; this pipeline ignores it
        pd.SampleDesc.Count = 1;
        if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&uiPso_)))) {
            Log("D3D12: UI pipeline failed; the GUI draws the classic way");
            uiPso_.Reset();
            return false;
        }
        WriteUiAtlasSlot();
        return true;
    }

    bool UiSupported() const override { return uiPso_ != nullptr; }

    void SetUiAtlas(TextureId atlas) override {
        if (!shadowSrvHeap_ || !texHeap_) return;
        WaitIdle(); // a frame in flight may still read the slot
        uiAtlas_ = atlas;
        WriteUiAtlasSlot();
    }

    void DrawUi(const UiVertex* vertices, uint32_t count, const Mat4& ortho) override {
        if (!uiPso_ || !resolved_ || count == 0) return;
        const size_t bytes = (size_t)count * sizeof(UiVertex);
        const size_t offset = (frame_->transientUsed + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("D3D12: transient buffer full, UI draw dropped");
            return;
        }
        std::memcpy(frame_->transientPtr + offset, vertices, bytes);
        frame_->transientUsed = offset + bytes;
        const D3D12_VERTEX_BUFFER_VIEW view = {frame_->transient->GetGPUVirtualAddress() + offset, (UINT)bytes, sizeof(UiVertex)};
        list_->SetGraphicsRootSignature(uiRootSig_.Get());
        list_->SetPipelineState(uiPso_.Get());
        list_->SetGraphicsRoot32BitConstants(0, 16, ortho.m, 0);
        list_->SetGraphicsRootDescriptorTable(1, ShaderGpu(UiAtlasSlot()));
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->IASetVertexBuffers(0, 1, &view);
        list_->DrawInstanced(count, 1, 0, 0);
        BindMeshRoot(); // later overlay draws (DrawTransient) expect the mesh root signature
    }

    // ------------------------------------------------------------------ water
    WaterDraw water_;
    std::vector<MeshId> waterMeshes_;
    bool waterPending_ = false;
    ComPtr<ID3D12Resource> waterRefract_;
    ComPtr<ID3D12RootSignature> waterRootSig_;
    ComPtr<ID3D12PipelineState> waterPso_;

    bool CreateWaterPipeline() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 5; // scene depth, the refraction copy, the sun's shadow map, the probe atlas, the cloud shadow map
        D3D12_ROOT_PARAMETER params[7] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 2;
        params[0].Constants.Num32BitValues = 32;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister = 1;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable = {1, &range};
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (UINT i = 0; i < 3; ++i) {
            params[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[4 + i].Descriptor.ShaderRegister = 5 + i;
        }
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1] = samplers[0];
        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 7;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = samplers;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error, vs, ps;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) {
            Log("D3D12: water root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
            return false;
        }
        CHECK(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&waterRootSig_)));
        if (!CompileShader(L"water.hlsl", "WaterVS", "vs_5_1", vs) || !CompileShader(L"water.hlsl", "WaterPS", "ps_5_1", ps)) return false;
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = waterRootSig_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.IndependentBlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        pd.InputLayout = {layout, 4};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = kHdrFormat;
        pd.RTVFormats[1] = kVelocityFormat;
        pd.DSVFormat = kDepthFormat;
        pd.SampleDesc.Count = 1;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&waterPso_)));
        return true;
    }

    // Inside ResolveScene: the composed picture is copied for refraction, then the water draws over it, testing but not writing depth.
    void WaterPass(UINT cur) {
        waterPending_ = false;
        ID3D12Resource* composed = post_[kComposed].Get();
        Transition(composed, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(waterRefract_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(waterRefract_.Get(), composed);
        Transition(waterRefract_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(composed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(depth_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(post_[kVelocity].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {PostRtv(kComposed), PostRtv(kVelocity)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = WaterDsv();
        list_->OMSetRenderTargets(2, rtvs, FALSE, &dsv);
        const D3D12_VIEWPORT vp = {0, 0, (float)rw_, (float)rh_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)rw_, (LONG)rh_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetGraphicsRootSignature(waterRootSig_.Get());
        list_->SetPipelineState(waterPso_.Get());
        list_->SetGraphicsRootConstantBufferView(1, frame_->postBuffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootConstantBufferView(2, frame_->lightBuffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(3, ShaderGpu(WaterTableBase()));
        list_->SetGraphicsRootShaderResourceView(4, gpuBuffers_[water_.waves].buffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootShaderResourceView(5, gpuBuffers_[water_.ripple].buffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootShaderResourceView(6, gpuBuffers_[water_.sky].buffer->GetGPUVirtualAddress());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        struct { float viewProj[16], ripple[4], near_[4], tiles[4], wind[4]; } pc;
        std::memcpy(pc.viewProj, water_.viewProj.m, sizeof(pc.viewProj));
        std::memcpy(pc.ripple, water_.ripplePlace, sizeof(pc.ripple));
        std::memcpy(pc.near_, water_.nearPlace, sizeof(pc.near_));
        std::memcpy(pc.tiles, water_.tiles, sizeof(pc.tiles));
        std::memcpy(pc.wind, water_.wind, sizeof(pc.wind));
        for (size_t i = 0; i < waterMeshes_.size(); ++i) {
            const MeshId id = waterMeshes_[i];
            if (id >= meshes_.size() || !meshes_[id].buffer) continue;
            const Mesh& mesh = meshes_[id];
            pc.tiles[3] = i < water_.nearCount ? 1.0f : 0.0f;
            list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
            const D3D12_VERTEX_BUFFER_VIEW view = DrawView(mesh);
            list_->IASetVertexBuffers(0, 1, &view);
            list_->IASetIndexBuffer(&mesh.indexView);
            list_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
            ++counts_.draws;
            counts_.triangles += mesh.count / 3;
        }
        Transition(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(composed, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(post_[kVelocity].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list_->SetGraphicsRootSignature(postRootSig_.Get());
        list_->SetGraphicsRootConstantBufferView(0, frame_->postBuffer->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(1, ShaderGpu(kPostDescriptorBase + cur * kPostSrvs));
    }

    bool CreateComputeRoot() {
        D3D12_ROOT_PARAMETER params[8] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 32;
        for (UINT i = 0; i < 4; ++i) { params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[1 + i].Descriptor.ShaderRegister = i; }
        for (UINT i = 0; i < 3; ++i) { params[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[5 + i].Descriptor.ShaderRegister = i; }
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 8;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) {
            Log("D3D12: compute root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
            return false;
        }
        CHECK(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&computeRootSig_)));
        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC cs = {};
        cs.ByteStride = 20;
        cs.NumArgumentDescs = 1;
        cs.pArgumentDescs = &arg;
        CHECK(device_->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&drawIndexedSignature_)));
        return true;
    }

    void ComputeTransitions(bool toRest) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const GpuBuffer& g : gpuBuffers_) {
            if (!g.buffer || g.use == GpuBufferUse::Read) continue;
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g.buffer.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = toRest ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : g.rest;
            b.Transition.StateAfter = toRest ? g.rest : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers.push_back(b);
        }
        if (!barriers.empty()) list_->ResourceBarrier((UINT)barriers.size(), barriers.data());
    }

    void VegetationIndirect(const Mesh& mesh, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) {
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, params.m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {mesh.view, {gpuBuffers_[instances].buffer->GetGPUVirtualAddress(), (UINT)gpuBuffers_[instances].bytes, kInstanceStride}};
        list_->IASetVertexBuffers(0, 2, views);
        list_->IASetIndexBuffer(&mesh.indexView);
        list_->ExecuteIndirect(drawIndexedSignature_.Get(), 1, gpuBuffers_[args].buffer.Get(), argsOffset, nullptr, 0);
    }

    void DrawInstancedMesh(const Mesh& mesh, const InstanceBuffer& instances, uint32_t first, uint32_t count) {
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {mesh.view, instances.view};
        list_->IASetVertexBuffers(0, 2, views);
        if (mesh.indexed) {
            list_->IASetIndexBuffer(&mesh.indexView);
            list_->DrawIndexedInstanced(mesh.count, count, 0, 0, first);
        } else {
            list_->DrawInstanced(mesh.count, count, 0, first);
        }
    }

    void Submit(const D3D12_VERTEX_BUFFER_VIEW& view, uint32_t count, Topology topology, const Mat4& mvp,
                const Mat4& model, bool depthTest, const D3D12_INDEX_BUFFER_VIEW* indices = nullptr) {
        const int index = (topology == Topology::Lines ? 2 : 0) + (depthTest ? 0 : 1);
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        list_->SetPipelineState(resolved_ ? psoOverlay_[index].Get() : prepass_ && index == 0 ? prepassPso_[0][prepass_ - 1].Get() : pso_[index].Get());
        list_->IASetPrimitiveTopology(topology == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                                  : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        if (!resolved_) list_->SetGraphicsRoot32BitConstants(6, 4, materialConstants_, 0);
        list_->IASetVertexBuffers(0, 1, &view);
        if (indices) {
            list_->IASetIndexBuffer(indices);
            list_->DrawIndexedInstanced(count, 1, 0, 0, 0);
        } else {
            list_->DrawInstanced(count, 1, 0, 0);
        }
    }

    // ------------------------------------------------------------------ city chunk bakes (Track R; renderer.h, shaders/chunk_faces.hlsli)
    // The chunk store is an impostor store of its own (the same three channel arrays and tile depth buffer as the self-test's, one
    // layer per face, channel 2 RG16 for the relief's start and end): a face is drawn into a layer, then copied into the atlas
    // channel, a plain Texture2D registered as a material texture so DrawChunkVolumes binds it like any other.
    static constexpr DXGI_FORMAT kChunkFormats[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_UNORM};

    D3D12_CPU_DESCRIPTOR_HANDLE ChunkRtv(UINT channel, UINT layer) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = chunkRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(channel * kChunkFaceCount + layer) * rtvSize_;
        return h;
    }

    void ReleaseChunkAtlas() {
        if (!chunkTile_) return;
        WaitIdle();
        for (TextureId& id : chunkTex_) {
            if (id < textures_.size() && textures_[id].resource) {
                textures_[id] = Tex{};
                freeTexSlots_.push_back(id);
            }
            id = kNoTexture;
        }
        for (auto& r : chunkAtlas_) r.Reset();
        for (auto& r : chunkStore_) r.Reset();
        chunkDepth_.Reset();
        chunkTile_ = chunkW_ = chunkH_ = 0;
    }

    bool CreateChunkAtlas(uint32_t tile, uint32_t width, uint32_t height) override {
        if (!device_ || tile < 4 || width < tile * kChunkFaceCount || height < tile) return false;
        if (tile == chunkTile_ && width == chunkW_ && height == chunkH_) return true;
        ReleaseChunkAtlas();
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (int c = 0; c < 3; ++c) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = tile;
            rd.Height = tile;
            rd.DepthOrArraySize = (UINT16)kChunkFaceCount;
            rd.MipLevels = 1;
            rd.Format = kChunkFormats[c];
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = kChunkFormats[c];
            if (c == 2) cv.Color[0] = cv.Color[1] = 1.0f; // empty relief
            if (c == 1) cv.Color[0] = cv.Color[1] = 0.5f, cv.Color[2] = cv.Color[3] = 1.0f;
            if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&chunkStore_[c])))) return false;
            D3D12_RESOURCE_DESC ad = rd;
            ad.Width = width;
            ad.Height = height;
            ad.DepthOrArraySize = 1;
            ad.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&chunkAtlas_[c])))) return false;
        }
        if (!chunkRtvHeap_) {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 3 * kChunkFaceCount;
            CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&chunkRtvHeap_)));
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            hd.NumDescriptors = 1;
            CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&chunkDsvHeap_)));
        }
        for (UINT c = 0; c < 3; ++c)
            for (UINT l = 0; l < (UINT)kChunkFaceCount; ++l) {
                D3D12_RENDER_TARGET_VIEW_DESC rv = {};
                rv.Format = kChunkFormats[c];
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray.FirstArraySlice = l;
                rv.Texture2DArray.ArraySize = 1;
                device_->CreateRenderTargetView(chunkStore_[c].Get(), &rv, ChunkRtv(c, l));
            }
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = tile;
        dd.Height = tile;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kTileDepthFormat;
        dd.SampleDesc.Count = 1;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dc = {};
        dc.Format = kTileDepthFormat;
        dc.DepthStencil.Depth = 1.0f;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&chunkDepth_)))) return false;
        device_->CreateDepthStencilView(chunkDepth_.Get(), nullptr, chunkDsvHeap_->GetCPUDescriptorHandleForHeapStart());

        // The atlas starts empty (relief 1, 1), then waits in the pixel shader state like every texture.
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        CHECK(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
        CHECK(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)));
        for (int c = 0; c < 3; ++c) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = chunkAtlas_[c].Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            cl->ResourceBarrier(1, &b);
        }
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        chunkTile_ = tile, chunkW_ = width, chunkH_ = height;
        for (UINT c = 0; c < 3; ++c) { // empty texels: relief (1, 1) reads as nothing there
            std::vector<uint8_t> fill((size_t)width * 4, c == 2 ? 0xFF : 0);
            std::vector<uint8_t> all((size_t)width * height * 4);
            for (uint32_t y = 0; y < height; ++y) std::memcpy(all.data() + (size_t)y * width * 4, fill.data(), fill.size());
            if (!WriteChunkAtlas((int)c, 0, 0, width, height, all.data())) return false;
        }
        for (UINT c = 0; c < 3; ++c) {
            UINT slot;
            if (!freeTexSlots_.empty()) { slot = freeTexSlots_.back(); freeTexSlots_.pop_back(); }
            else if (nextTexSlot_ < kMaxTextures) slot = nextTexSlot_++;
            else { Log("D3D12: out of texture slots for the chunk atlas"); return false; }
            Tex tex;
            tex.resource = chunkAtlas_[c];
            tex.slot = slot;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = kChunkFormats[c];
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(chunkAtlas_[c].Get(), &sv, TexCpu(slot));
            if (slot >= textures_.size()) textures_.resize(slot + 1);
            textures_[slot] = std::move(tex);
            chunkTex_[c] = (TextureId)slot;
        }
        Log("D3D12: chunk atlas %ux%u, %u px tiles (%u cells)", width, height, tile, (width / (tile * kChunkFaceCount)) * (height / tile));
        return true;
    }

    TextureId ChunkAtlasTexture(int channel) const override { return channel >= 0 && channel < 3 ? chunkTex_[channel] : kNoTexture; }

    bool BeginChunkBake() override {
        if (!frame_ || resolved_ || !chunkTile_ || !chunkBakePso_[0] || !chunkBakePso_[1]) return false;
        for (auto& r : chunkStore_) Transition(r.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        chunkBaking_ = true;
        return true;
    }

    void ChunkBakeFace(const ChunkBakeItem* items, uint32_t count, const Mat4& faceViewProj, uint32_t atlasX, uint32_t atlasY) override {
        if (!chunkBaking_ || atlasX + chunkTile_ > chunkW_ || atlasY + chunkTile_ > chunkH_) return;
        const UINT layer = chunkLayer_++ % (UINT)kChunkFaceCount;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {ChunkRtv(0, layer), ChunkRtv(1, layer), ChunkRtv(2, layer)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = chunkDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
        const float clear0[4] = {0, 0, 0, 0}, clear1[4] = {0.5f, 0.5f, 1.0f, 1.0f}, clear2[4] = {1, 1, 0, 0};
        list_->ClearRenderTargetView(rtvs[0], clear0, 0, nullptr);
        list_->ClearRenderTargetView(rtvs[1], clear1, 0, nullptr);
        list_->ClearRenderTargetView(rtvs[2], clear2, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)chunkTile_, (float)chunkTile_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)chunkTile_, (LONG)chunkTile_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        PushConstants pc;
        std::memcpy(pc.mvp, faceViewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        // The rows go into the transient buffer once; both passes draw from there.
        static std::vector<InstanceBuffer> views;
        views.assign(count, InstanceBuffer{});
        for (uint32_t k = 0; k < count; ++k) {
            const ChunkBakeItem& it = items[k];
            if (!it.rows || !it.count || it.mesh >= meshes_.size() || !meshes_[it.mesh].buffer || meshes_[it.mesh].topology != Topology::Triangles) continue;
            const size_t bytes = (size_t)it.count * kInstanceStride, offset = (frame_->transientUsed + 15) & ~size_t(15);
            if (offset + bytes > kTransientBytes) { Log("D3D12: transient buffer full, chunk bake item dropped"); continue; }
            std::memcpy(frame_->transientPtr + offset, it.rows, bytes);
            frame_->transientUsed = offset + bytes;
            views[k].view = {frame_->transient->GetGPUVirtualAddress() + offset, (UINT)bytes, kInstanceStride};
        }
        for (int pass = 0; pass < 2; ++pass) {
            list_->SetPipelineState(chunkBakePso_[pass].Get());
            TextureId bound = kNoTexture;
            bool any = false;
            for (uint32_t k = 0; k < count; ++k) {
                if (!views[k].view.SizeInBytes) continue;
                if (!any || items[k].albedo != bound) { SetMaterial(items[k].albedo, kNoTexture, kNoTexture); bound = items[k].albedo; any = true; }
                DrawInstancedMesh(meshes_[items[k].mesh], views[k], 0, items[k].count);
            }
        }
        // Into the atlas.
        for (int c = 0; c < 3; ++c) {
            Transition(chunkStore_[c].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(chunkAtlas_[c].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = chunkAtlas_[c].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = chunkStore_[c].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = layer;
            const D3D12_BOX box = {0, 0, 0, chunkTile_, chunkTile_, 1};
            list_->CopyTextureRegion(&dst, atlasX, atlasY, 0, &src, &box);
            Transition(chunkAtlas_[c].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(chunkStore_[c].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    void EndChunkBake() override {
        if (!chunkBaking_) return;
        chunkBaking_ = false;
        for (auto& r : chunkStore_) Transition(r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {ColorRtv(), PostRtv(kVelocity)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(2, targets, FALSE, &dsv);
        const float noMotion[4] = {0, 0, 0, 0};
        list_->ClearRenderTargetView(targets[1], noMotion, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT vp = {0, 0, (float)rw_, (float)rh_, 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, (LONG)rw_, (LONG)rh_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        SetMaterial(kNoTexture, kNoTexture, kNoTexture);
    }

    // A one-off copy between an atlas channel and a CPU buffer, outside the frame; waits.
    bool ChunkAtlasCopy(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>* out, const uint8_t* in) {
        if (channel < 0 || channel > 2 || !chunkAtlas_[channel] || !w || !h) return false;
        D3D12_RESOURCE_DESC box = chunkAtlas_[channel]->GetDesc();
        if (x + w > box.Width || y + h > box.Height) return false;
        box.Width = w;
        box.Height = h;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&box, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = out ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, out ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)))) return false;
        if (in) {
            uint8_t* mapped = nullptr;
            D3D12_RANGE noRead = {0, 0};
            if (FAILED(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return false;
            for (uint32_t row = 0; row < h; ++row) std::memcpy(mapped + fp.Offset + (size_t)row * fp.Footprint.RowPitch, in + (size_t)row * w * 4, (size_t)w * 4);
            staging->Unmap(0, nullptr);
        }
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))))
            return false;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = chunkAtlas_[channel].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = out ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION tex = {}, buf = {};
        tex.pResource = chunkAtlas_[channel].Get();
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        buf.pResource = staging.Get();
        buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint = fp;
        if (out) {
            const D3D12_BOX src = {x, y, 0, x + w, y + h, 1};
            cl->CopyTextureRegion(&buf, 0, 0, 0, &tex, &src);
        } else {
            cl->CopyTextureRegion(&tex, x, y, 0, &buf, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();
        if (out) {
            out->resize((size_t)w * h * 4);
            uint8_t* mapped = nullptr;
            const D3D12_RANGE all = {0, (SIZE_T)bytes};
            if (FAILED(staging->Map(0, &all, reinterpret_cast<void**>(&mapped)))) return false;
            for (uint32_t row = 0; row < h; ++row) std::memcpy(out->data() + (size_t)row * w * 4, mapped + fp.Offset + (size_t)row * fp.Footprint.RowPitch, (size_t)w * 4);
            const D3D12_RANGE none = {0, 0};
            staging->Unmap(0, &none);
        }
        return true;
    }
    bool ReadChunkAtlas(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& out) override {
        return ChunkAtlasCopy(channel, x, y, w, h, &out, nullptr);
    }
    bool WriteChunkAtlas(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* texels) override {
        return texels && ChunkAtlasCopy(channel, x, y, w, h, nullptr, texels);
    }

    void DrawChunkVolumes(MeshId id, const Mat4& viewProj) override {
        if (!frame_ || resolved_ || !chunkTile_ || !chunkVolPso_ || id >= meshes_.size() || !meshes_[id].buffer || !meshes_[id].indexed) return;
        const Mesh& mesh = meshes_[id];
        SetMaterial(chunkTex_[0], chunkTex_[1], chunkTex_[2]);
        materialConstants_[2] = (float)chunkTile_;
        list_->SetGraphicsRoot32BitConstants(6, 4, materialConstants_, 0);
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetPipelineState(chunkVolPso_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        list_->IASetVertexBuffers(0, 1, &mesh.view);
        list_->IASetIndexBuffer(&mesh.indexView);
        list_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
        ++counts_.draws;
        counts_.triangles += mesh.count / 3;
        materialConstants_[2] = 0.0f;
        SetMaterial(kNoTexture, kNoTexture, kNoTexture);
    }

    void SetDrawFade(float keep) override {
        materialConstants_[3] = keep;
        if (frame_ && list_ && !resolved_) list_->SetGraphicsRoot32BitConstants(6, 4, materialConstants_, 0);
    }

    void DrawShadowInstancedTransient(MeshId id, const float* rows, uint32_t count) override {
        if (count == 0 || !rows || !frame_ || id >= meshes_.size() || !meshes_[id].buffer || meshes_[id].topology != Topology::Triangles) return;
        const size_t bytes = (size_t)count * kInstanceStride;
        const size_t offset = (frame_->transientUsed + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("D3D12: transient buffer full, instanced caster dropped");
            return;
        }
        std::memcpy(frame_->transientPtr + offset, rows, bytes);
        frame_->transientUsed = offset + bytes;
        InstanceBuffer ib;
        ib.view = {frame_->transient->GetGPUVirtualAddress() + offset, (UINT)bytes, kInstanceStride};
        if (!shadowInstancedBound_ || forceShadowRebind_) {
            forceShadowRebind_ = false;
            list_->SetPipelineState(shadowTarget_ == ShadowMap::Tight ? tightInstancedPso_.Get() : shadowInstancedPso_.Get());
            shadowInstancedBound_ = true;
        }
        PushConstants pc;
        std::memcpy(pc.mvp, lightVP_.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        list_->SetGraphicsRoot32BitConstants(0, 32, &pc, 0);
        DrawInstancedMesh(meshes_[id], ib, 0, count);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    // The bake's two pipelines and the volumes' one (CreatePipelines; `layout` / `instancedLayout` are its vertex layouts).
    bool CreateChunkPipelines(const D3D12_INPUT_ELEMENT_DESC* layout, const D3D12_INPUT_ELEMENT_DESC* instancedLayout, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& scene) {
        ComPtr<ID3DBlob> bakeVs, bakePs, volVs, volPs;
        if (!CompileShader(L"mesh.hlsl", "ChunkBakeVS", "vs_5_1", bakeVs) || !CompileShader(L"mesh.hlsl", "ChunkBakePS", "ps_5_1", bakePs) ||
            !CompileShader(L"mesh.hlsl", "ChunkVolVS", "vs_5_1", volVs) || !CompileShader(L"mesh.hlsl", "ChunkVolPS", "ps_5_1", volPs))
            return false;
        for (int pass = 0; pass < 2; ++pass) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig_.Get();
            pd.VS = {bakeVs->GetBufferPointer(), bakeVs->GetBufferSize()};
            pd.PS = {bakePs->GetBufferPointer(), bakePs->GetBufferSize()};
            pd.BlendState.IndependentBlendEnable = TRUE;
            for (int t = 0; t < 3; ++t) {
                pd.RTVFormats[t] = kChunkFormats[t];
                pd.BlendState.RenderTarget[t].RenderTargetWriteMask = (pass == 0) == (t < 2) ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
            }
            if (pass == 1) { // the relief: the nearest start and the farthest end over every surface, so MIN of (d, 1 - d)
                D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[2];
                b.BlendEnable = TRUE;
                b.SrcBlend = b.DestBlend = D3D12_BLEND_ONE;
                b.BlendOp = D3D12_BLEND_OP_MIN;
                b.SrcBlendAlpha = b.DestBlendAlpha = D3D12_BLEND_ONE;
                b.BlendOpAlpha = D3D12_BLEND_OP_MIN;
                b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
            }
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.DepthStencilState.DepthEnable = pass == 0 ? TRUE : FALSE;
            pd.DepthStencilState.DepthWriteMask = pass == 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            pd.InputLayout = {instancedLayout, 7};
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 3;
            pd.DSVFormat = kTileDepthFormat;
            pd.SampleDesc.Count = 1;
            CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&chunkBakePso_[pass])));
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC vd = scene;
        vd.VS = {volVs->GetBufferPointer(), volVs->GetBufferSize()};
        vd.PS = {volPs->GetBufferPointer(), volPs->GetBufferSize()};
        vd.InputLayout = {layout, 4};
        CHECK(device_->CreateGraphicsPipelineState(&vd, IID_PPV_ARGS(&chunkVolPso_)));
        return true;
    }

    // ------------------------------------------------------------------ impostor store
    // Three texture arrays (albedo, normal, depth; layer = slot * 6 + face). The bake renders into one layer of
    // each at a time; the impostor shader samples all of them through one descriptor table. At rest they sit in
    // the pixel-shader-resource state.
    static constexpr DXGI_FORMAT kStoreFormats[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                      DXGI_FORMAT_R16_UNORM};
    static constexpr DXGI_FORMAT kTileDepthFormat = DXGI_FORMAT_D32_FLOAT;

    D3D12_CPU_DESCRIPTOR_HANDLE StoreRtv(UINT channel, UINT layer) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = storeRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(channel * storeLayers_ + layer) * rtvSize_;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE TileDsv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return h;
    }

    bool CreateStore(UINT maxObjects, UINT tile) {
        storeLayers_ = maxObjects * 6;
        storeTile_ = tile;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (int c = 0; c < 3; ++c) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = tile;
            rd.Height = tile;
            rd.DepthOrArraySize = (UINT16)storeLayers_;
            rd.MipLevels = 1;
            rd.Format = kStoreFormats[c];
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = kStoreFormats[c];
            CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                   IID_PPV_ARGS(&store_[c])));
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3 * storeLayers_;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&storeRtvHeap_)));
        for (UINT c = 0; c < 3; ++c)
            for (UINT l = 0; l < storeLayers_; ++l) {
                D3D12_RENDER_TARGET_VIEW_DESC rv = {};
                rv.Format = kStoreFormats[c];
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray.FirstArraySlice = l;
                rv.Texture2DArray.ArraySize = 1;
                device_->CreateRenderTargetView(store_[c].Get(), &rv, StoreRtv(c, l));
            }

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&storeSrvHeap_)));
        const UINT srvSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = storeSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT c = 0; c < 3; ++c) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = kStoreFormats[c];
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2DArray.MipLevels = 1;
            sv.Texture2DArray.ArraySize = storeLayers_;
            device_->CreateShaderResourceView(store_[c].Get(), &sv, h);
            h.ptr += srvSize;
        }

        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = tile;
        dd.Height = tile;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kTileDepthFormat;
        dd.SampleDesc.Count = 1;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dc = {};
        dc.Format = kTileDepthFormat;
        dc.DepthStencil.Depth = 1.0f;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc,
                                               IID_PPV_ARGS(&tileDepth_)));
        device_->CreateDepthStencilView(tileDepth_.Get(), nullptr, TileDsv());
        return true;
    }

    // Root signature shared by the bake and impostor passes: slot 0 = the three store SRVs, slot 1 = 16 root
    // constants (visible to every stage), plus a static linear-clamp sampler at s0.
    bool CreateStoreRootSig() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 16};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &sampler;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) {
            Log("D3D12: store root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
            return false;
        }
        CHECK(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&storeRootSig_)));
        return true;
    }

    bool CompileShader(const wchar_t* file, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        const std::wstring path = ExeDirectory() + L"\\shaders\\" + file;
        ComPtr<ID3DBlob> error;
        if (FAILED(CompileCached(path, nullptr, entry, target, out, error))) {
            Log("D3D12: %s: %s", entry, error ? (const char*)error->GetBufferPointer() : "shader file not found");
            return false;
        }
        return true;
    }

    // A no-vertex-buffer, no-depth pipeline over the store root signature.
    bool CreateFullscreenPso(ID3DBlob* vs, ID3DBlob* ps, UINT targets, const DXGI_FORMAT* formats,
                             ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = storeRootSig_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        for (UINT i = 0; i < targets; ++i) {
            pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.RTVFormats[i] = formats[i];
        }
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = targets;
        pd.SampleDesc.Count = 1;
        CHECK(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
        return true;
    }

    void TransitionStore(D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        for (auto& texture : store_) Transition(texture.Get(), from, to);
    }

    // Renders known values into layer 3 of the store (after clearing every layer), then samples layers 3 and 2
    // through the SRV table into a 2x1 target and reads it back. Run once at startup when TBD_TEXTURE_SELFTEST is set.
    bool SelfTestTextures() {
        constexpr UINT kTile = 16, kLayer = 3;
        if (!CreateStore(1, kTile) || !CreateStoreRootSig()) return false;

        ComPtr<ID3DBlob> vs, fillPs, samplePs;
        if (!CompileShader(L"selftest.hlsl", "FullscreenVS", "vs_5_1", vs) ||
            !CompileShader(L"selftest.hlsl", "FillPS", "ps_5_1", fillPs) ||
            !CompileShader(L"selftest.hlsl", "SamplePS", "ps_5_1", samplePs))
            return false;
        ComPtr<ID3D12PipelineState> fillPso, samplePso;
        const DXGI_FORMAT sampleFormat[1] = {DXGI_FORMAT_R8G8B8A8_UNORM};
        if (!CreateFullscreenPso(vs.Get(), fillPs.Get(), 3, kStoreFormats, fillPso) ||
            !CreateFullscreenPso(vs.Get(), samplePs.Get(), 1, sampleFormat, samplePso))
            return false;

        // The 2x1 target the sample pass writes, and a buffer to read it back through.
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 2;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> target;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&target)));
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> targetHeap;
        CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&targetHeap)));
        const D3D12_CPU_DESCRIPTOR_HANDLE targetRtv = targetHeap->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(target.Get(), nullptr, targetRtv);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 readbackBytes = 0;
        device_->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = readbackBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)));

        frame_ = &frames_[0];
        list_ = frame_->list.Get();
        CHECK(frame_->allocator->Reset());
        CHECK(list_->Reset(frame_->allocator.Get(), nullptr));
        list_->SetGraphicsRootSignature(storeRootSig_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Pass 1: clear every layer, then draw into one.
        TransitionStore(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4] = {};
        for (UINT c = 0; c < 3; ++c)
            for (UINT l = 0; l < storeLayers_; ++l) list_->ClearRenderTargetView(StoreRtv(c, l), zero, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE layerRtvs[3] = {StoreRtv(0, kLayer), StoreRtv(1, kLayer), StoreRtv(2, kLayer)};
        list_->OMSetRenderTargets(3, layerRtvs, FALSE, nullptr);
        D3D12_VIEWPORT vp = {0, 0, (float)kTile, (float)kTile, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, (LONG)kTile, (LONG)kTile};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->SetPipelineState(fillPso.Get());
        list_->DrawInstanced(3, 1, 0, 0);
        TransitionStore(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 2: sample it back.
        list_->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
        vp = {0, 0, 2.0f, 1.0f, 0.0f, 1.0f};
        scissor = {0, 0, 2, 1};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = {storeSrvHeap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootDescriptorTable(0, storeSrvHeap_->GetGPUDescriptorHandleForHeapStart());
        list_->SetPipelineState(samplePso.Get());
        list_->DrawInstanced(3, 1, 0, 0);

        Transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        src.pResource = target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        CHECK(list_->Close());
        ID3D12CommandList* lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        WaitIdle();

        uint8_t* px = nullptr;
        D3D12_RANGE readRange = {0, 8};
        CHECK(readback->Map(0, &readRange, reinterpret_cast<void**>(&px)));
        uint8_t got[8];
        std::memcpy(got, px, 8);
        readback->Unmap(0, nullptr);

        // Expected: pixel 0 = (1.0, 0.5, 1.0, 0.75) -> (255, 128, 255, 191); pixel 1 = untouched layer = 0.
        const uint8_t want[8] = {255, 128, 255, 191, 0, 0, 0, 0};
        bool ok = true;
        for (int i = 0; i < 8; ++i)
            if (std::abs((int)got[i] - (int)want[i]) > 2) ok = false;
        Log("D3D12: self-test pixels (%u %u %u %u) (%u %u %u %u), expected (%u %u %u %u) (0 0 0 0)", got[0], got[1], got[2],
            got[3], got[4], got[5], got[6], got[7], want[0], want[1], want[2], want[3]);
        return ok;
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &b);
    }

    void WaitIdle() {
        const UINT64 value = ++fenceValue_;
        queue_->Signal(fence_.Get(), value);
        if (fence_->GetCompletedValue() < value) {
            fence_->SetEventOnCompletion(value, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    // Once, when the device is gone: why, the GPU work that was running (DRED's breadcrumbs: the last operation each command
    // list finished, and the one after it) and, for a memory fault, the address and what had lived there.
    void ReportDeviceRemoved() {
        static bool reported = false;
        if (reported || !device_) return;
        reported = true;
        const HRESULT reason = device_->GetDeviceRemovedReason();
        const char* name = reason == DXGI_ERROR_DEVICE_HUNG ? "hung (a GPU command ran too long: TDR)"
                         : reason == DXGI_ERROR_DEVICE_RESET ? "reset (bad commands)"
                         : reason == DXGI_ERROR_DEVICE_REMOVED ? "removed (the GPU stopped answering: power, heat, driver or hardware)"
                         : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver internal error"
                         : reason == DXGI_ERROR_INVALID_CALL ? "invalid call" : "other";
        Log("D3D12: DEVICE LOST, reason 0x%08X: %s", (unsigned)reason, name);
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (FAILED(device_.As(&dred))) { Log("D3D12: no DRED data (TBD_NODRED, or not supported)"); return; }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
            int lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node && lists < 16; node = node->pNext, ++lists) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                if (done >= node->BreadcrumbCount) continue; // this list finished
                Log("D3D12: DRED: an unfinished command list, %u of %u operations done; last done op %d, next op %d", done, node->BreadcrumbCount,
                    done ? (int)node->pCommandHistory[done - 1] : -1, (int)node->pCommandHistory[done]);
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
            int existing = 0, freed = 0;
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadExistingAllocationNode; a; a = a->pNext) ++existing;
            for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext) ++freed;
            Log("D3D12: DRED: MEMORY FAULT at GPU address 0x%llx (a bug of ours: %d live and %d recently freed allocations there)",
                (unsigned long long)fault.PageFaultVA, existing, freed);
        } else Log("D3D12: DRED: no memory fault recorded");
    }

    void WaitFrame(Frame& frame) {
        if (frame.fenceValue && fence_->GetCompletedValue() < frame.fenceValue) {
            fence_->SetEventOnCompletion(frame.fenceValue, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        ReadTimer(frame);
    }

    static constexpr UINT MaterialBase(UINT slot) { return 1 + slot * kMaterialTextures * kMaterialPairs; }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_;
    ComPtr<ID3D12Resource> back_[kBackBuffers], depth_, overlayDepth_; // overlayDepth_: window-sized, only while the scale is below 1
    Frame frames_[kFramesInFlight];
    Frame* frame_ = nullptr;
    UINT frameSlot_ = kFramesInFlight - 1;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_[4]; // triangles/lines x depth/no depth
    ComPtr<ID3D12Resource> store_[3], tileDepth_; // impostor albedo / normal / depth arrays, bake depth buffer
    ComPtr<ID3D12DescriptorHeap> storeRtvHeap_, storeSrvHeap_;
    ComPtr<ID3D12RootSignature> storeRootSig_;
    UINT storeLayers_ = 0, storeTile_ = 0;
    ComPtr<ID3D12Resource> chunkStore_[3], chunkAtlas_[3], chunkDepth_; // the chunk bake's store (a layer per face) and its atlas
    ComPtr<ID3D12DescriptorHeap> chunkRtvHeap_, chunkDsvHeap_;
    ComPtr<ID3D12PipelineState> chunkBakePso_[2], chunkVolPso_; // bake: surface, relief; ChunkVolPS
    TextureId chunkTex_[3] = {kNoTexture, kNoTexture, kNoTexture};
    UINT chunkTile_ = 0, chunkW_ = 0, chunkH_ = 0, chunkLayer_ = 0;
    bool chunkBaking_ = false;
    std::vector<Mesh> meshes_;
    std::vector<MeshId> freeSlots_;
    std::vector<StreamRetired> streamRetired_;
    uint64_t streamFrame_ = 0;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    ComPtr<ID3D12Resource> sunCache_; // the sun map's depth from the casters that don't move
    ComPtr<ID3D12Resource> shadowMap_, tightMap_; // the sun's map, and the tight character map around the camera's focus
    ShadowMap shadowTarget_ = ShadowMap::Sun;     // the shadow pass being recorded
    ComPtr<ID3D12DescriptorHeap> shadowSrvHeap_, texHeap_; // shader-visible (shadow + material pairs), CPU-only textures
    std::vector<Tex> textures_;
    std::vector<UINT> freeTexSlots_;
    UINT nextTexSlot_ = 0, srvInc_ = 0;
    TextureId defaults_[4] = {kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    std::vector<TextureId> extraOf_; // by surface texture id: its extra map
    ComPtr<ID3D12PipelineState> shadowPso_, tightPso_;
    Mat4 lightVP_;
    UINT rtvSize_ = 0, w_ = 1, h_ = 1, frameIndex_ = 0;
    UINT rw_ = 1, rh_ = 1; // the render size: the scene and post chain draw at this, the tone map upscales to w_ x h_
    float scale_ = 1.0f;
};

} // namespace

std::unique_ptr<Renderer> CreateD3D12Renderer() { return std::make_unique<D3D12Renderer>(); }

} // namespace tbd
