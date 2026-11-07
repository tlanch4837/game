// Metal backend (iOS, CAMetalLayer) through metal-cpp. Feature parity with the Vulkan backend except hardware ray traced
// ambient occlusion: the AO pass is always the screen-space AoPS (RayTracingSupported() is false).
//
// UNVERIFIED: written on Windows, where Metal cannot be compiled. Every metal-cpp call was checked against the headers in
// third_party/metal-cpp, but nothing here has been built or run yet. docs/metal_backend.md lists what a Mac must verify.
//
// How it maps onto the Vulkan backend (renderer_vulkan.cpp is the reference for every behaviour):
//   * Shaders are the same HLSL, through SPIR-V and spirv-cross (msl.bat). Function names are the Vulkan .spv stems
//     (mesh_vs, post_taa_ps, ...). Descriptor set 0 binding N is Metal slot N; set 1 (the material) is an argument
//     buffer at buffer(1); push constants are buffer(29); vertex streams are buffers 30 and 28 (namespace slot below).
//   * Dynamic rendering's begin/end pairs become render command encoders. The scene pass opens lazily and only clears
//     the first time in a frame, so stopping it for a shadow or compute pass costs one store and one load.
//   * Transient attachments that are never read back use memoryless storage (tile memory only): the capture's depth and
//     velocity, and the window-sized overlay depth used below render scale 1. The scene's depth and G-buffer (HDR colour
//     and velocity) are sampled by the post chain, so they must live in memory.
//   * Instanced draws use per-instance vertex streams (MTLVertexStepFunctionPerInstance) and baseInstance, indirect
//     vegetation draws use drawIndexedPrimitives with an indirect buffer (the same five-uint record as Vulkan / D3D12).
//   * Unified memory: meshes, instance buffers and GPU buffers are shared-storage buffers written once by the CPU, so the
//     streaming (NoWait) mesh path needs no copy at all. Textures are private (lossless compression) and uploaded by blit.
//   * Metal command buffers retain what they reference, and everything freed while a frame may still use it goes through
//     a retire list keyed by frame serial, so Destroy* never waits for the GPU (Vulkan's vkDeviceWaitIdle calls).
#include "mesh_weld.h"
#include "renderer.h"

#if !defined(__APPLE__)
#error "renderer_metal.mm is the Apple (Metal) backend; Windows builds use renderer_d3d12.cpp and renderer_vulkan.cpp"
#endif

// The one translation unit that holds metal-cpp's implementation (selectors and class lookups).
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cloud_noise.h"
extern float g_brightness;    // the app shell: user brightness multiplier (main.cpp on Windows)
extern float g_sceneExposure; // the app shell: the scene's fixed exposure, 0 = auto

namespace tbd {
namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kTransientBytes = 16 * 1024 * 1024;
constexpr float kShadowDepthBias = 2000.0f; // as the Vulkan backend; Metal's depthBias uses the same units (verify on device)
constexpr float kShadowSlopeBias = 2.0f;
constexpr float kTightDepthBias = 1000.0f;
constexpr float kTightSlopeBias = 1.5f;
constexpr NS::UInteger kInstanceStride = 12 * sizeof(float);

// Metal argument-table slots. Must match tools/msl.ps1 (see the binding scheme in docs/metal_backend.md).
namespace slot {
constexpr NS::UInteger kPush = 29;     // push constants (mesh pc, ui pc, compute pc, water wc)
constexpr NS::UInteger kStream0 = 30;  // vertex stream 0: Vertex / UiVertex
constexpr NS::UInteger kStream1 = 28;  // vertex stream 1: instance rows, or a skinned mesh's previous vertices
constexpr NS::UInteger kMaterial = 1;  // Vulkan set 1 as an argument buffer
// Mesh set 0 (lighting.hlsli / mesh.hlsl)
constexpr NS::UInteger kLight = 0, kMotion = 6;
constexpr NS::UInteger kShadowTex = 1, kShadowSampler = 2, kTightTex = 3, kProbesTex = 4, kCloudShadowTex = 5;
// Post set 0 (post.hlsl): buffer 0, textures 1-8 and 11-21, samplers 9 and 10.
constexpr NS::UInteger kPostParams = 0, kPostClamp = 9, kPostWrap = 10;
// Water set 0 (water.hlsl)
constexpr NS::UInteger kWaterPost = 0, kWaterLight = 1, kWaterWaves = 7, kWaterRipple = 8, kWaterSky = 9;
constexpr NS::UInteger kWaterDepth = 2, kWaterRefract = 3, kWaterShadow = 4, kWaterProbes = 10, kWaterCloudShadow = 11;
constexpr NS::UInteger kWaterClamp = 5, kWaterCmp = 6;
// UI (ui.hlsl)
constexpr NS::UInteger kUiAtlas = 0, kUiSampler = 1, kUiImages = 2, kUiBackdrop = 3;
} // namespace slot

struct MeshPush { // mesh.hlsl PushConstants under VULKAN: both stages read the same 144 bytes
    float mvp[16];
    float model[16];
    float material[4];
};
static_assert(sizeof(MeshPush) == 144, "mesh.hlsl's push constant block");

NS::String* Str(const char* s) { return NS::String::string(s, NS::UTF8StringEncoding); }

// Scope for metal-cpp's autoreleased objects (descriptors, command buffers, encoders made outside a frame).
struct Pool {
    NS::AutoreleasePool* p = NS::AutoreleasePool::alloc()->init();
    ~Pool() { p->release(); }
};

template <class T> void Release(T*& o) {
    if (o) o->release();
    o = nullptr;
}

void LogError(const char* what, NS::Error* error) {
    Log("Metal: %s: %s", what, error && error->localizedDescription() ? error->localizedDescription()->utf8String() : "(no detail)");
}

enum class VL { None, Mesh, Instanced, Skinned, Ui }; // vertex input layouts
enum class Blend { None, Temporal, Ui };
enum class Pass { None, Scene, Shadow, Capture, Overlay, Post, Water, Compute, Blit };

class MetalRenderer final : public Renderer {
    struct Frame {
        MTL::Buffer* transient = nullptr;
        MTL::Buffer* light = nullptr;
        MTL::Buffer* post = nullptr;
        MTL::Buffer* motion = nullptr;
    };
    struct Mesh {
        MTL::Buffer* buffer = nullptr;   // owned unless sharesBuffer
        uint32_t count = 0;              // indices when indexed, vertices otherwise
        Topology topology = Topology::Triangles;
        NS::UInteger indexOffset = 0;
        bool indexed = false;
        NS::UInteger copyBytes = 0;      // deformable: bytes of one copy of the vertices (0 = not deformable)
        bool sharesBuffer = false;       // a part of a deformable mesh
        MTL::Buffer* upload = nullptr;   // deformable parent: one staging copy per frame in flight
        uint32_t newestCopy = 0;
        MeshId copyParent = kInvalidMesh;
        bool live = false;
    };
    struct Texture { MTL::Texture* tex = nullptr; };
    struct GpuBuffer { MTL::Buffer* buffer = nullptr; GpuBufferUse use = GpuBufferUse::Read; uint64_t bytes = 0; };
    struct Compute { MTL::ComputePipelineState* pipe = nullptr; MTL::Size group = MTL::Size(64, 1, 1); };
    // One material = one (albedo, normal, surface) triple; its argument buffers per layout (0: full, 1: cut-out shaders).
    struct Material { TextureId t[4] = {}; int32_t slot[2] = {-1, -1}; };
    struct Retired { uint64_t serial = 0; NS::Object* obj = nullptr; MeshId mesh = kInvalidMesh; TextureId texture = kNoTexture; int32_t matSlot = -1; };
    struct Att {
        MTL::Texture* tex = nullptr;
        MTL::LoadAction load = MTL::LoadActionDontCare;
        MTL::StoreAction store = MTL::StoreActionStore;
        float clear[4] = {0, 0, 0, 0};
        double clearDepth = 0.0;
        NS::UInteger slice = 0;
    };

public:
    ~MetalRenderer() override { Shutdown(); }

    const char* Name() const override { return "METAL"; }

    bool Init(void* nativeWindow, int width, int height, bool validation) override {
        Pool pool;
        layer_ = static_cast<CA::MetalLayer*>(nativeWindow);
        if (!layer_) { Log("Metal: Init needs a CAMetalLayer"); return false; }
        layer_->retain();
        device_ = MTL::CreateSystemDefaultDevice();
        if (!device_) { Log("Metal: no device"); return false; }
        queue_ = device_->newCommandQueue();
        Log("Metal: device %s%s", device_->name()->utf8String(), validation ? " (API validation: enable MTL_DEBUG_LAYER in the Xcode scheme)" : "");
        w_ = (uint32_t)std::max(1, width);
        h_ = (uint32_t)std::max(1, height);
        layer_->setDevice(device_);
        layer_->setPixelFormat(kDrawableFormat);
        layer_->setFramebufferOnly(false); // CaptureUiBackdrop and the frame capture copy from the drawable
        layer_->setMaximumDrawableCount(3);
        layer_->setDrawableSize(CGSizeMake(w_, h_));
        sceneW_ = (uint32_t)ScaledSize((int)w_, scale_);
        sceneH_ = (uint32_t)ScaledSize((int)h_, scale_);
        inflight_ = dispatch_semaphore_create(kFramesInFlight);
        if (!LoadLibrary() || !CreateFixedObjects() || !CreatePipelines() || !CreateMaterialResources() || !CreateCloudTextures() ||
            !CreatePostPipelines() || !CreateDepth() || !CreatePostTargets())
            return false;
        if (!CreateUiPipeline()) Log("Metal: UI pipeline unavailable (ui_vs / ui_ps missing or failed); the GUI draws the classic way");
        if (std::getenv("TBD_TEXTURE_SELFTEST")) Log("Metal: texture self-test %s", SelfTestTextures() ? "passed" : "FAILED");
        return true;
    }

    void Resize(int width, int height) override {
        if (!device_ || width <= 0 || height <= 0) return;
        if ((uint32_t)width == w_ && (uint32_t)height == h_) return;
        w_ = (uint32_t)width;
        h_ = (uint32_t)height;
        WaitIdle();
        layer_->setDrawableSize(CGSizeMake(w_, h_));
        sceneW_ = (uint32_t)ScaledSize((int)w_, scale_);
        sceneH_ = (uint32_t)ScaledSize((int)h_, scale_);
        if (CreateDepth()) CreatePostTargets();
    }

    void SetRenderScale(float scale) override {
        scale = ClampRenderScale(scale);
        if (scale == scale_) return;
        scale_ = scale;
        if (!device_) return;
        WaitIdle();
        sceneW_ = (uint32_t)ScaledSize((int)w_, scale_);
        sceneH_ = (uint32_t)ScaledSize((int)h_, scale_);
        if (CreateDepth()) CreatePostTargets();
    }

    // ------------------------------------------------------------------ meshes
    MeshId CreateMesh(const Vertex* vertices, uint32_t count, Topology topology, bool dynamic) override {
        if (!device_ || !vertices || count == 0) return kInvalidMesh;
        Mesh mesh;
        mesh.count = count;
        mesh.topology = topology;
        if (dynamic) {
            mesh.buffer = device_->newBuffer(vertices, (NS::UInteger)count * sizeof(Vertex), MTL::ResourceStorageModeShared);
        } else {
            std::vector<Vertex> welded;
            std::vector<uint32_t> indices;
            WeldVertices(vertices, count, welded, indices);
            mesh.buffer = NewMeshBuffer(welded.data(), welded.size() * sizeof(Vertex), indices.data(), indices.size() * sizeof(uint32_t));
            mesh.indexOffset = welded.size() * sizeof(Vertex);
            mesh.indexed = true;
        }
        if (!mesh.buffer) return kInvalidMesh;
        return PlaceMesh(mesh);
    }

    MeshId CreateDeformableMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        if (!device_ || !vertices || !indices || !vertexCount || !indexCount) return kInvalidMesh;
        const NS::UInteger copyBytes = (NS::UInteger)vertexCount * sizeof(Vertex), indexBytes = (NS::UInteger)indexCount * sizeof(uint32_t);
        // Device side: two copies of the vertices (newest and previous, for motion vectors), then the indices. The CPU writes
        // a staging copy per frame in flight and CommitDeformables blits it in, so a frame still reading never sees a write.
        Mesh mesh;
        mesh.upload = device_->newBuffer(copyBytes * kFramesInFlight, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        mesh.buffer = device_->newBuffer(copyBytes * 2 + indexBytes, MTL::ResourceStorageModePrivate);
        MTL::Buffer* staging = device_->newBuffer(copyBytes + indexBytes, MTL::ResourceStorageModeShared);
        if (!mesh.upload || !mesh.buffer || !staging) { Release(mesh.upload); Release(mesh.buffer); Release(staging); return kInvalidMesh; }
        std::memcpy(staging->contents(), vertices, copyBytes);
        std::memcpy(static_cast<uint8_t*>(staging->contents()) + copyBytes, indices, indexBytes);
        MTL::Buffer* dst = mesh.buffer;
        if (!Upload([&](MTL::BlitCommandEncoder* b) {
                b->copyFromBuffer(staging, 0, dst, 0, copyBytes);
                b->copyFromBuffer(staging, 0, dst, copyBytes, copyBytes);
                b->copyFromBuffer(staging, copyBytes, dst, copyBytes * 2, indexBytes);
            }, staging, true)) {
            Release(mesh.upload);
            Release(mesh.buffer);
            return kInvalidMesh;
        }
        mesh.count = indexCount;
        mesh.indexOffset = copyBytes * 2;
        mesh.indexed = true;
        mesh.copyBytes = copyBytes;
        return PlaceMesh(mesh);
    }

    MeshId CreateMeshPart(MeshId parent, uint32_t firstIndex, uint32_t indexCount) override {
        if (parent >= meshes_.size() || !meshes_[parent].live || !meshes_[parent].copyBytes) return kInvalidMesh;
        Mesh mesh = meshes_[parent];
        mesh.count = indexCount;
        mesh.indexOffset += (NS::UInteger)firstIndex * sizeof(uint32_t);
        mesh.sharesBuffer = true;
        mesh.upload = nullptr;
        mesh.copyParent = parent;
        return PlaceMesh(mesh);
    }

    Vertex* DeformableVertices(MeshId id) override {
        if (!frameActive_ || id >= meshes_.size() || !meshes_[id].live || !meshes_[id].copyBytes || meshes_[id].sharesBuffer) return nullptr;
        pendingDeformables_.push_back(id);
        return reinterpret_cast<Vertex*>(static_cast<uint8_t*>(meshes_[id].upload->contents()) + meshes_[id].copyBytes * frameSlot_);
    }

    void CommitDeformables() override {
        if (pendingDeformables_.empty()) return;
        if (frameActive_) {
            MTL::BlitCommandEncoder* b = BlitEncoder();
            for (MeshId id : pendingDeformables_) {
                Mesh& mesh = meshes_[id];
                mesh.newestCopy ^= 1;
                b->copyFromBuffer(mesh.upload, mesh.copyBytes * frameSlot_, mesh.buffer, mesh.copyBytes * mesh.newestCopy, mesh.copyBytes);
            }
            EndEncoder(); // the next draw reopens the scene pass
        }
        pendingDeformables_.clear();
    }

    void DestroyMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].live) return;
        sunCacheKey_ = 0;
        Mesh& m = meshes_[id];
        if (!m.sharesBuffer) { RetireObject(m.buffer); RetireObject(m.upload); }
        m = {};
        freeSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ instance buffers
    InstanceBufferId CreateInstanceBuffer(const float* rows, uint32_t count) override { return CreateInstanceBufferNoWait(rows, count); }

    void DestroyInstanceBuffer(InstanceBufferId id) override {
        if (id >= instanceBuffers_.size() || !instanceBuffers_[id]) return;
        RetireObject(instanceBuffers_[id]);
        instanceBuffers_[id] = nullptr;
        freeInstanceSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ textures
    TextureId CreateTexture(const uint8_t* rgba, int w, int h) override {
        if (!device_ || !rgba || w <= 0 || h <= 0) return kNoTexture;
        const MipChain mips = BuildMipChain(rgba, w, h);
        return UploadTexture(mips.levels, mips.dims, true);
    }

    void DestroyTexture(TextureId id) override {
        ForgetMaterialConstants(id);
        if (id >= textures_.size() || !textures_[id].tex) return;
        DropMaterialsUsing(id, false);
        RetireObject(textures_[id].tex);
        textures_[id] = {};
        if (id < extraOf_.size()) extraOf_[id] = kNoTexture;
        freeTextures_.push_back(id);
    }

    void SetSurfaceExtra(TextureId surface, TextureId extra) override {
        if (surface >= textures_.size()) return;
        if (extraOf_.size() <= surface) extraOf_.resize(surface + 1, kNoTexture);
        if (extraOf_[surface] == extra) return;
        DropMaterialsUsing(surface, true); // the argument buffers naming the old link
        extraOf_[surface] = extra;
    }

    void SetMaterial(TextureId albedo, TextureId normal, TextureId surface) override {
        if (!frameActive_) return;
        curMat_[0] = albedo;
        curMat_[1] = normal;
        curMat_[2] = surface;
        ++counts_.materialBinds;
        materialConstants_[0] = SkinDetailScale(albedo);
        materialConstants_[1] = SkinPoreDepthConstant(albedo);
        boundMatKey_ = ~0ull; // rebound before the next draw that reads it
    }

    // ------------------------------------------------------------------ streaming
    MeshId CreateMeshNoWait(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        if (!device_ || !vertices || !indices || !vertexCount || !indexCount) return kInvalidMesh;
        UploadSteps steps;
        Mesh mesh;
        const size_t vertexBytes = (size_t)vertexCount * sizeof(Vertex), indexBytes = (size_t)indexCount * sizeof(uint32_t);
        mesh.buffer = NewMeshBuffer(vertices, vertexBytes, indices, indexBytes);
        if (!mesh.buffer) return kInvalidMesh;
        steps.Mark("allocate+copy");
        mesh.count = indexCount;
        mesh.indexOffset = vertexBytes;
        mesh.indexed = true;
        steps.Done("Metal", "mesh upload", vertexBytes + indexBytes);
        return PlaceMesh(mesh);
    }

    InstanceBufferId CreateInstanceBufferNoWait(const float* rows, uint32_t count) override {
        if (!device_ || !rows || count == 0) return kNoInstances;
        MTL::Buffer* b = device_->newBuffer(rows, (NS::UInteger)count * kInstanceStride, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!b) return kNoInstances;
        if (!freeInstanceSlots_.empty()) {
            const InstanceBufferId id = freeInstanceSlots_.back();
            freeInstanceSlots_.pop_back();
            instanceBuffers_[id] = b;
            return id;
        }
        instanceBuffers_.push_back(b);
        return (InstanceBufferId)(instanceBuffers_.size() - 1);
    }

    TextureId CreateTextureNoWait(const std::vector<std::vector<uint8_t>>& levels, const std::vector<std::pair<int, int>>& dims) override {
        if (!device_ || levels.empty() || levels.size() != dims.size()) return kNoTexture;
        UploadSteps steps;
        const TextureId id = UploadTexture(levels, dims, false);
        size_t total = 0;
        for (const auto& l : levels) total += l.size();
        steps.Mark("texture+blit");
        steps.Done("Metal", "texture upload", total);
        return id;
    }

    void RetireMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].live) return;
        Retired r;
        r.serial = frameSerial_;
        r.mesh = id;
        retired_.push_back(r);
    }

    void RetireTexture(TextureId id) override {
        ForgetMaterialConstants(id);
        if (id >= textures_.size() || !textures_[id].tex) return;
        Retired r;
        r.serial = frameSerial_;
        r.texture = id;
        retired_.push_back(r);
    }

    uint64_t LocalMemoryBytes() const override { return device_ ? (uint64_t)device_->recommendedMaxWorkingSetSize() : 0; }

    // ------------------------------------------------------------------ compute
    GpuBufferId CreateGpuBuffer(GpuBufferUse use, uint64_t bytes, const void* initial) override {
        if (!device_ || bytes == 0) return kNoGpuBuffer;
        GpuBuffer g;
        g.use = use;
        g.bytes = bytes;
        g.buffer = initial ? device_->newBuffer(initial, (NS::UInteger)bytes, MTL::ResourceStorageModeShared)
                           : device_->newBuffer((NS::UInteger)bytes, MTL::ResourceStorageModeShared);
        if (!g.buffer) return kNoGpuBuffer;
        if (!initial) std::memset(g.buffer->contents(), 0, (size_t)bytes);
        for (size_t i = 0; i < gpuBuffers_.size(); ++i)
            if (!gpuBuffers_[i].buffer) { gpuBuffers_[i] = g; return (GpuBufferId)i; }
        gpuBuffers_.push_back(g);
        return (GpuBufferId)(gpuBuffers_.size() - 1);
    }

    void DestroyGpuBuffer(GpuBufferId id) override {
        if (id >= gpuBuffers_.size() || !gpuBuffers_[id].buffer) return;
        RetireObject(gpuBuffers_[id].buffer);
        gpuBuffers_[id] = GpuBuffer{};
    }

    ComputeShaderId CreateComputeShader(const char* entry) override { return CreateComputeShaderIn("vegetation", entry); }

    ComputeShaderId CreateComputeShaderIn(const char* file, const char* entry) override {
        if (!device_) return kNoComputeShader;
        Pool pool;
        const std::string name = std::string(file) + "_" + entry;
        MTL::Function* fn = Fn(name.c_str());
        if (!fn) return kNoComputeShader;
        NS::Error* error = nullptr;
        Compute c;
        c.pipe = device_->newComputePipelineState(fn, &error);
        if (!c.pipe) { LogError(name.c_str(), error); return kNoComputeShader; }
        // [numthreads] of the HLSL entry points (MSL keeps no record of it; vegetation.hlsl and water.hlsl).
        const std::string e = entry;
        c.group = e == "CSPlace" ? MTL::Size(16, 16, 1) : (e == "CSRipple" || e == "CSWaves" || e == "CSSky") ? MTL::Size(8, 8, 1) : MTL::Size(64, 1, 1);
        computes_.push_back(c);
        return (ComputeShaderId)(computes_.size() - 1);
    }

    bool SetWaterDraw(const WaterDraw& draw) override {
        if (!waterPipe_ && !CreateWaterPipeline()) return false;
        water_ = draw;
        waterMeshes_.assign(draw.meshes, draw.meshes + draw.meshCount);
        waterPending_ = !waterMeshes_.empty();
        return true;
    }

    void BeginCompute() override {
        if (!frameActive_) return;
        EndEncoder();
        cce_ = cmd_->computeCommandEncoder(); // serial dispatch: each dispatch sees the writes of those before it
        pass_ = Pass::Compute;
    }

    void Dispatch(ComputeShaderId shader, const void* constants, uint32_t constantBytes, const GpuBufferId reads[4], const GpuBufferId writes[3], uint32_t x, uint32_t y, uint32_t z) override {
        if (!frameActive_ || shader >= computes_.size() || pass_ != Pass::Compute) return;
        cce_->setComputePipelineState(computes_[shader].pipe);
        for (uint32_t i = 0; i < 7; ++i) {
            const GpuBufferId id = i < 4 ? reads[i] : writes[i - 4];
            if (id < gpuBuffers_.size() && gpuBuffers_[id].buffer) cce_->setBuffer(gpuBuffers_[id].buffer, 0, i);
        }
        if (constants && constantBytes) cce_->setBytes(constants, constantBytes, slot::kPush);
        cce_->dispatchThreadgroups(MTL::Size(x, y, z), computes_[shader].group);
    }

    void EndCompute() override {
        if (pass_ == Pass::Compute) EndEncoder();
    }

    void SetVegetationPass(int pass) override { vegetationPass_ = pass; }

    uint32_t MeshIndexCount(MeshId id) const override { return id < meshes_.size() && meshes_[id].live && meshes_[id].indexed ? meshes_[id].count : 0; }

    void DrawVegetationIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) override {
        if (!frameActive_ || resolved_ || !MeshIndexCount(id) || !ValidGpu(instances) || !ValidGpu(args)) return;
        MTL::RenderCommandEncoder* e = DrawEncoder();
        if (!e) return;
        if (vegetationPass_ == 1) UsePipe(vegDepthPipe_, 1, dsScene_);
        else if (vegetationPass_ == 2) UsePipe(vegShadePipe_, -1, dsEqual_);
        else UsePipe(vegPipe_, 0, dsScene_);
        VegetationIndirect(meshes_[id], instances, args, argsOffset, viewProj, params);
        ++counts_.draws;
    }

    void DrawVegetationShadowIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& params) override {
        if (pass_ != Pass::Shadow || !MeshIndexCount(id) || !ValidGpu(instances) || !ValidGpu(args)) return;
        UsePipe(vegShadowPipe_, -1, dsShadow_);
        VegetationIndirect(meshes_[id], instances, args, argsOffset, lightVP_, params);
    }

    bool ReadGpuBuffer(GpuBufferId id, void* out, uint64_t bytes) override {
        if (!ValidGpu(id) || bytes > gpuBuffers_[id].bytes) return false;
        WaitIdle();
        std::memcpy(out, gpuBuffers_[id].buffer->contents(), (size_t)bytes);
        return true;
    }

    // ------------------------------------------------------------------ frame
    void BeginFrame(const float clearColor[4]) override {
        prepass_ = 0;
        lastCounts_ = counts_;
        counts_ = {};
        if (!device_) return;
        const double waitStart = TraceNowMs();
        dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER); // the frame two back, which used this slot, is done
        fenceWaitMs_ = TraceNowMs() - waitStart;
        frameSlot_ = (frameSlot_ + 1) % kFramesInFlight;
        frame_ = &frames_[frameSlot_];
        used_ = 0;
        FlushRetired(false);
        ReportTimers();
        framePool_ = NS::AutoreleasePool::alloc()->init();
        cmd_ = queue_->commandBuffer();
        ++frameSerial_;

        std::memcpy(frame_->light->contents(), &lighting_, sizeof(lighting_));
        std::memcpy(static_cast<uint8_t*>(frame_->light->contents()) + sizeof(lighting_), &portals_, sizeof(portals_));
        Lighting* gpuLight = reinterpret_cast<Lighting*>(frame_->light->contents());
        gpuLight->eye[3] = (float)(postFrame_ & 1023); // the frame, for dithered transparency that TAA averages
        if (!cloudMapValid_) gpuLight->cloudMap[3] = 0.0f; // nothing drawn into it yet
        cloudMapValid_ = lighting_.cloudMap[3] > 0.5f;
        if (probeAge_ == 0) gpuLight->probeUpdate[3] = 0.0f; // the atlas holds nothing yet
        uint8_t* motion = static_cast<uint8_t*>(frame_->motion->contents());
        std::memcpy(motion, view_.viewProj.m, 64);
        std::memcpy(motion + 64, prevViewProj_.m, 64);
        std::memcpy(motion + 128, &view_.dt, 4);

        std::memcpy(clear_, clearColor, sizeof(clear_));
        resolved_ = false;
        sceneStarted_ = false;
        drawable_ = nullptr;
        frameActive_ = true;
    }

    void StoreSunCache(uint64_t key) override {
        if (pass_ != Pass::Shadow || shadowTarget_ != ShadowMap::Sun) return;
        EndEncoder();
        MTL::BlitCommandEncoder* b = BlitEncoder();
        b->copyFromTexture(shadow_, sunCache_);
        EndEncoder();
        BeginShadowEncoder(ShadowMap::Sun, MTL::LoadActionLoad);
        sunCacheKey_ = key;
    }

    void BeginShadowPass(ShadowMap map, bool restoreSunCache) override {
        if (!frameActive_) return;
        EndEncoder();
        shadowTarget_ = map;
        const bool tight = map == ShadowMap::Tight;
        std::memcpy(lightVP_.m, tight ? lighting_.tightVP : lighting_.shadowVP, sizeof(lightVP_.m));
        const bool restore = restoreSunCache && !tight;
        if (restore) {
            MTL::BlitCommandEncoder* b = BlitEncoder();
            b->copyFromTexture(sunCache_, shadow_);
            EndEncoder();
        }
        BeginShadowEncoder(map, restore ? MTL::LoadActionLoad : MTL::LoadActionClear);
    }

    void DrawShadowCaster(MeshId id, const Mat4& model) override {
        if (pass_ != Pass::Shadow || id >= meshes_.size() || !meshes_[id].live || meshes_[id].topology != Topology::Triangles) return;
        UsePipe(shadowPipe_, -1, dsShadow_);
        DrawMeshInPass(meshes_[id], lightVP_ * model, model);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += meshes_[id].count / 3;
    }

    void DrawShadowCasterCutout(MeshId id, const Mat4& model, TextureId albedo) override {
        if (pass_ != Pass::Shadow || id >= meshes_.size() || !meshes_[id].live || meshes_[id].topology != Topology::Triangles) return;
        SetMaterial(albedo, kNoTexture, kNoTexture);
        UsePipe(shadowCutPipe_, 1, dsShadow_);
        DrawMeshInPass(meshes_[id], lightVP_ * model, model);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += meshes_[id].count / 3;
    }

    // One-frame instance rows (cosmetic traffic, zoned buildings): a shared buffer per call, retired with the frame.
    void DrawShadowInstancedTransient(MeshId id, const float* rows, uint32_t count) override {
        if (pass_ != Pass::Shadow || count == 0 || !rows || !device_ || id >= meshes_.size() || !meshes_[id].live ||
            meshes_[id].topology != Topology::Triangles)
            return;
        MTL::Buffer* b = device_->newBuffer(rows, (NS::UInteger)count * kInstanceStride, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!b) return;
        UsePipe(shadowInstPipe_, -1, dsShadow_);
        PushMesh(lightVP_, Mat4::Identity());
        DrawInstancedMesh(meshes_[id], b, 0, count);
        RetireObject(b);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    void DrawShadowInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count) override {
        if (pass_ != Pass::Shadow || count == 0 || id >= meshes_.size() || !meshes_[id].live || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer])
            return;
        UsePipe(shadowInstPipe_, -1, dsShadow_);
        PushMesh(lightVP_, Mat4::Identity());
        DrawInstancedMesh(meshes_[id], instanceBuffers_[buffer], first, count);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    void EndShadowPass() override {
        if (pass_ != Pass::Shadow) return;
        EndEncoder();
    }

    void SetScenePrepass(int mode) override { prepass_ = mode >= 0 && mode <= 2 ? mode : 0; }

    void DrawInstancedTransient(MeshId id, const float* rows, uint32_t count, const Mat4& viewProj) override {
        if (!frameActive_ || count == 0 || !rows || !device_ || resolved_ || id >= meshes_.size() || !meshes_[id].live ||
            meshes_[id].topology != Topology::Triangles)
            return;
        if (!DrawEncoder()) return;
        MTL::Buffer* b = device_->newBuffer(rows, (NS::UInteger)count * kInstanceStride, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!b) return;
        if (prepass_ == 1) UsePipe(prepassDepthPipe_[1], -1, dsScene_);
        else UsePipe(instancedPipe_, 0, prepass_ == 2 ? dsEqual_ : dsScene_);
        PushMesh(viewProj, Mat4::Identity());
        DrawInstancedMesh(meshes_[id], b, 0, count);
        RetireObject(b);
        if (prepass_ == 1) return;
        ++counts_.draws;
        counts_.triangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    void DrawInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count, const Mat4& viewProj) override {
        if (!frameActive_ || count == 0 || resolved_ || id >= meshes_.size() || !meshes_[id].live || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer])
            return;
        if (!DrawEncoder()) return;
        if (prepass_ == 1) UsePipe(prepassDepthPipe_[1], -1, dsScene_);
        else UsePipe(instancedPipe_, 0, prepass_ == 2 ? dsEqual_ : dsScene_);
        PushMesh(viewProj, Mat4::Identity());
        DrawInstancedMesh(meshes_[id], instanceBuffers_[buffer], first, count);
        if (prepass_ == 1) return;
        ++counts_.draws;
        counts_.triangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    // The lit capture from the sun (reflective shadow map): drawn with Draw into kCapture. Its depth and second colour
    // target (the scene pipelines write velocity) are memoryless: they exist only in tile memory during this pass.
    void BeginCapture() override {
        if (!frameActive_) return;
        EndEncoder();
        Att color, velocity, depth;
        color.tex = post_[kCapture];
        color.load = MTL::LoadActionClear; // zero alpha: nothing here
        velocity.tex = captureVelocity_;
        velocity.load = MTL::LoadActionClear;
        velocity.store = MTL::StoreActionDontCare;
        depth.tex = captureDepth_;
        depth.load = MTL::LoadActionClear;
        depth.store = MTL::StoreActionDontCare;
        depth.clearDepth = 0.0; // reversed Z
        BeginRender({color, velocity}, &depth, kCaptureSize, kCaptureSize, Pass::Capture);
        BindMeshResources(true);
    }

    void EndCapture() override {
        if (pass_ == Pass::Capture) EndEncoder();
    }

    void Draw(MeshId id, const Mat4& mvp, const Mat4& model, bool depthTest, bool counted) override {
        if (!frameActive_ || id >= meshes_.size() || !meshes_[id].live) return;
        const Mesh& mesh = meshes_[id];
        if (prepass_ == 1 && (mesh.copyBytes || resolved_ || !depthTest || mesh.topology != Topology::Triangles)) return;
        if (!DrawEncoder()) return;
        if (mesh.copyBytes && !resolved_ && depthTest && mesh.topology == Topology::Triangles && mesh.indexed) { // skinned: says how it moved
            UsePipe(skinnedPipe_, 0, dsScene_);
            PushMesh(mvp, model);
            const uint32_t newest = NewestCopy(mesh);
            rce_->setVertexBuffer(mesh.buffer, mesh.copyBytes * newest, slot::kStream0);
            rce_->setVertexBuffer(mesh.buffer, mesh.copyBytes * (newest ^ 1), slot::kStream1);
            rce_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, mesh.count, MTL::IndexTypeUInt32, mesh.buffer, mesh.indexOffset);
            if (counted) { ++counts_.draws; counts_.triangles += mesh.count / 3; }
            return;
        }
        Submit(mesh.buffer, mesh.copyBytes * NewestCopy(mesh), mesh.count, mesh.topology, mvp, model, depthTest, mesh.indexed, mesh.indexOffset);
        if (counted) {
            ++counts_.draws;
            if (mesh.topology == Topology::Triangles) counts_.triangles += mesh.count / 3;
        }
    }

    void DrawTransient(const Vertex* vertices, uint32_t count, Topology topology, const Mat4& mvp, const Mat4& model, bool depthTest) override {
        if (!frameActive_ || count == 0) return;
        const size_t bytes = (size_t)count * sizeof(Vertex);
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) { Log("Metal: transient buffer full, draw dropped"); return; }
        if (!DrawEncoder()) return;
        std::memcpy(static_cast<uint8_t*>(frame_->transient->contents()) + offset, vertices, bytes);
        used_ = offset + bytes;
        Submit(frame_->transient, offset, count, topology, mvp, model, depthTest);
    }

    bool UiSupported() const override { return uiPipe_ != nullptr; }
    void SetUiAtlas(TextureId atlas) override { uiAtlas_ = atlas; }

    void DrawUi(const UiVertex* vertices, uint32_t count, const Mat4& ortho) override {
        if (!frameActive_ || !resolved_ || !uiPipe_ || count == 0 || pass_ != Pass::Overlay) return;
        const size_t bytes = (size_t)count * sizeof(UiVertex);
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) { Log("Metal: transient buffer full, UI draw dropped"); return; }
        std::memcpy(static_cast<uint8_t*>(frame_->transient->contents()) + offset, vertices, bytes);
        used_ = offset + bytes;
        UsePipe(uiPipe_, -1, dsOff_);
        const TextureId atlas = uiAtlas_ < textures_.size() && textures_[uiAtlas_].tex ? uiAtlas_ : defaults_[0];
        rce_->setFragmentTexture(textures_[atlas].tex, slot::kUiAtlas);
        const TextureId images = uiImages_ < textures_.size() && textures_[uiImages_].tex ? uiImages_ : defaults_[0];
        rce_->setFragmentTexture(textures_[images].tex, slot::kUiImages);
        rce_->setFragmentTexture(backdropSmall_ ? backdropSmall_ : textures_[defaults_[0]].tex, slot::kUiBackdrop);
        rce_->setFragmentSamplerState(clampSampler_, slot::kUiSampler);
        rce_->setVertexBytes(ortho.m, 64, slot::kPush);
        rce_->setFragmentBytes(ortho.m, 64, slot::kPush); // the glass mode reads the viewport size
        rce_->setVertexBuffer(frame_->transient, offset, slot::kStream0);
        rce_->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)count);
    }

    void SetUiImages(TextureId atlas) override { uiImages_ = atlas; }

    // The Glass Dock GUI's backdrop: the overlay pass ends, the drawable is copied into a mip-mapped texture, the mips are
    // generated (a box filter chain) and level 4 (1/16) is kept; then the overlay pass resumes on the same drawable.
    void CaptureUiBackdrop() override {
        if (!frameActive_ || !resolved_ || !drawable_ || pass_ != Pass::Overlay) return;
        if (!backdropMip_ || backdropMip_->width() != w_ || backdropMip_->height() != h_) {
            Release(backdropMip_);
            Release(backdropSmall_);
            backdropMip_ = NewTexture(MTL::TextureType2D, kDrawableFormat, w_, h_, 1, 5, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
            backdropSmall_ = NewTexture(MTL::TextureType2D, kDrawableFormat, std::max(1u, w_ >> 4), std::max(1u, h_ >> 4), 1, 1, MTL::TextureUsageShaderRead,
                                        MTL::StorageModePrivate);
            if (!backdropMip_ || !backdropSmall_) return;
        }
        MTL::BlitCommandEncoder* b = BlitEncoder();
        b->copyFromTexture(drawable_->texture(), 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size(w_, h_, 1), backdropMip_, 0, 0, MTL::Origin::Make(0, 0, 0));
        b->generateMipmaps(backdropMip_);
        b->copyFromTexture(backdropMip_, 0, 4, MTL::Origin::Make(0, 0, 0), MTL::Size(backdropSmall_->width(), backdropSmall_->height(), 1), backdropSmall_, 0, 0,
                           MTL::Origin::Make(0, 0, 0));
        Att color, depth;
        color.tex = drawable_->texture();
        color.load = MTL::LoadActionLoad;
        depth.tex = overlayDepth_ ? overlayDepth_ : depth_;
        depth.load = MTL::LoadActionClear; // the GUI draws without depth; what follows it gets a cleared one
        depth.store = MTL::StoreActionDontCare;
        depth.clearDepth = 0.0;
        BeginRender({color}, &depth, w_, h_, Pass::Overlay);
        boundPipe_ = nullptr;
        BindMeshResources(true);
    }

    void SetFrameView(const FrameView& view) override { view_ = view; }

    // AO, clouds, sky and haze, water, TAA, exposure, bloom, then the tone map into the drawable, in the same render pass as
    // the overlays that follow (one pass over the drawable instead of two).
    void ResolveScene() override {
        if (!frameActive_ || resolved_) return;
        DrawEncoder(); // a frame that drew nothing still gets its cleared scene, velocity and depth
        EndEncoder();
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
        const uint32_t w = sceneW_, h = sceneH_;
        p.eye[3] = scale_ < 1.0f ? 1.0f : 0.0f;
        p.res[0] = (float)w; p.res[1] = (float)h; p.res[2] = 1.0f / (float)w; p.res[3] = 1.0f / (float)h;
        p.misc[0] = view_.time; p.misc[1] = (float)(postFrame_ & 1023); p.misc[2] = view_.sky ? 1.0f : 0.0f;
        p.misc[3] = historyValid_ ? 1.0f : 0.0f;
        p.motion[0] = view_.motionVectors ? 1.0f : 0.0f; p.motion[1] = g_brightness; p.motion[2] = g_sceneExposure;
        std::memcpy(frame_->post->contents(), &p, sizeof(p));

        const uint32_t cur = postFrame_ & 1;
        if (lighting_.cloudMap[3] > 0.5f) PostPass(kCloudShadow, 9, kCloudShadowSize, kCloudShadowSize, cur);
        if (lighting_.probeUpdate[3] > 0.5f) {
            PostPass(kProbeRaysTarget, 5, kProbeRays, kProbesPerFrame, cur);
            PostPass(kProbes, 6, kProbeX * 3, kProbeAtlasH, cur);
            ++probeAge_;
        }
        PostPass(kAo, 0, std::max(1u, w / 2), std::max(1u, h / 2), cur);
        if (view_.sky) {
            PostPass(kCloudMarch, 1, std::max(1u, w / 4), std::max(1u, h / 4), cur);
            PostPass(cur ? kClouds1 : kClouds, 10, std::max(1u, w / 2), std::max(1u, h / 2), cur);
        }
        PostPass(kComposed, 2, w, h, cur);
        if (waterPending_) WaterPass(w, h);
        PostPass(kHistory0 + cur, 3, w, h, cur);
        PostPass(kExposure, 7, 1, 1, cur);
        PostPass(kBloom, 8, std::max(1u, w / 4), std::max(1u, h / 4), cur);

        drawable_ = layer_->nextDrawable(); // as late as possible: the drawable is the scarcest thing in the frame
        if (drawable_) {
            Att color, depth;
            color.tex = drawable_->texture();
            color.load = MTL::LoadActionDontCare; // the tone map covers it
            if (overlayDepth_) { // the scene's depth is smaller than the window: a window-sized, tile-only one for the overlays
                depth.tex = overlayDepth_;
                depth.load = MTL::LoadActionClear;
            } else {
                depth.tex = depth_;
                depth.load = MTL::LoadActionLoad;
            }
            depth.store = MTL::StoreActionDontCare;
            depth.clearDepth = 0.0;
            BeginRender({color}, &depth, w_, h_, Pass::Overlay);
            rce_->setRenderPipelineState(postPipes_[4]);
            rce_->setDepthStencilState(dsOff_);
            BindPostResources(cur, nullptr, overlayDepth_ ? nullptr : depth_);
            rce_->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
            boundPipe_ = nullptr;
            BindMeshResources(true);
        } else {
            Log("Metal: no drawable this frame");
        }
        resolved_ = true;
        prevViewProj_ = view_.viewProj;
        historyValid_ = true;
        ++postFrame_;
    }

    void EndFrame() override {
        if (!frameActive_) return;
        ResolveScene();
        EndEncoder();
        const bool capturing = capture_ && drawable_ && PrepareCapture();
        if (capturing) {
            MTL::BlitCommandEncoder* b = BlitEncoder();
            b->copyFromTexture(drawable_->texture(), 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size(w_, h_, 1), captureBuffer_, 0, (NS::UInteger)w_ * 4,
                               (NS::UInteger)w_ * h_ * 4);
            EndEncoder();
        }
        if (drawable_) cmd_->presentDrawable(drawable_);
        const uint64_t serial = frameSerial_;
        // An explicit std::function: a bare lambda is ambiguous between metal-cpp's block and std::function overloads.
        const MTL::HandlerFunction onDone = [this, serial](MTL::CommandBuffer* cb) {
            const double ms = (cb->GPUEndTime() - cb->GPUStartTime()) * 1000.0;
            if (ms > 0.0 && ms < 1000.0) { gpuMicros_ += (uint64_t)(ms * 1000.0); ++gpuSamples_; }
            uint64_t done = completedSerial_.load();
            while (done < serial && !completedSerial_.compare_exchange_weak(done, serial)) {}
            dispatch_semaphore_signal(inflight_);
        };
        cmd_->addCompletedHandler(onDone);
        const double presentStart = TraceNowMs();
        cmd_->commit();
        presentMs_ = TraceNowMs() - presentStart; // presenting is scheduled by the command buffer: this is the commit
        Release(lastCommitted_);
        lastCommitted_ = cmd_->retain();
        frameActive_ = false;
        if (capturing) {
            cmd_->waitUntilCompleted();
            const size_t bytes = (size_t)w_ * h_ * 4;
            captureRgba_.resize(bytes);
            std::memcpy(captureRgba_.data(), captureBuffer_->contents(), bytes);
            captureW_ = (int)w_;
            captureH_ = (int)h_;
            captureNew_ = true;
        }
        cmd_ = nullptr;
        drawable_ = nullptr;
        framePool_->release();
        framePool_ = nullptr;
    }

    void SetFrameCapture(bool on) override {
        capture_ = on;
        captureNew_ = false;
        // the drawable stays readable: the capture blit and the GUI backdrop (CaptureUiBackdrop) copy from it
        if (!on) RetireObject(captureBuffer_);
    }

    bool CapturedFrame(std::vector<uint8_t>& rgba, int& width, int& height) override {
        if (!captureNew_) return false;
        captureNew_ = false;
        rgba.swap(captureRgba_);
        width = captureW_;
        height = captureH_;
        return true;
    }

private:
    // ------------------------------------------------------------------ shader library
    // default.metallib (compiled from src/shaders/msl by Xcode / CMake) when present; otherwise each function is compiled at
    // run time from msl/<name>.metal in the app bundle (slower start, handy before the offline step works).
    bool LoadLibrary() {
        defaultLibrary_ = device_->newDefaultLibrary();
        if (!defaultLibrary_) Log("Metal: no default.metallib; compiling msl/*.metal from the bundle at run time");
        return true;
    }

    MTL::Function* Fn(const char* name) {
        const auto found = functions_.find(name);
        if (found != functions_.end()) return found->second;
        MTL::Function* fn = defaultLibrary_ ? defaultLibrary_->newFunction(Str(name)) : nullptr;
        if (!fn) {
            std::string path = NS::Bundle::mainBundle()->resourcePath()->utf8String();
            path += "/msl/";
            path += name;
            path += ".metal";
            std::string source;
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
                std::fclose(f);
            }
            if (source.empty()) { Log("Metal: shader %s not in default.metallib nor at %s", name, path.c_str()); return nullptr; }
            MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
            NS::Error* error = nullptr;
            MTL::Library* lib = device_->newLibrary(Str(source.c_str()), options, &error);
            options->release();
            if (!lib) { LogError(name, error); return nullptr; }
            fn = lib->newFunction(Str(name));
            runtimeLibraries_.push_back(lib);
            if (!fn) { Log("Metal: %s.metal has no function %s", name, name); return nullptr; }
        }
        functions_[name] = fn;
        return fn;
    }

    // ------------------------------------------------------------------ pipelines
    static constexpr MTL::PixelFormat kDrawableFormat = MTL::PixelFormatBGRA8Unorm;
    static constexpr MTL::PixelFormat kHdrFormat = MTL::PixelFormatRGBA16Float;
    static constexpr MTL::PixelFormat kVelocityFormat = MTL::PixelFormatRGBA16Float; // xy motion, z skin reflection, w spare
    static constexpr MTL::PixelFormat kDepthFormat = MTL::PixelFormatDepth32Float;

    // The vertex descriptor for `layout`, keeping only the attributes the vertex function declares (Metal rejects a
    // descriptor that leaves a declared attribute out; listing exactly the declared ones keeps every pipeline valid).
    MTL::VertexDescriptor* VertexLayout(MTL::Function* vs, VL layout) {
        struct A { uint32_t loc; MTL::VertexFormat fmt; uint32_t offset; NS::UInteger buffer; };
        std::vector<A> attrs;
        const A mesh[4] = {{0, MTL::VertexFormatFloat3, 0, slot::kStream0}, {1, MTL::VertexFormatFloat3, 12, slot::kStream0},
                           {2, MTL::VertexFormatFloat4, 24, slot::kStream0}, {3, MTL::VertexFormatFloat2, 40, slot::kStream0}};
        if (layout == VL::Mesh || layout == VL::Instanced || layout == VL::Skinned) attrs.assign(mesh, mesh + 4);
        if (layout == VL::Instanced)
            for (uint32_t r = 0; r < 3; ++r) attrs.push_back({4 + r, MTL::VertexFormatFloat4, 16 * r, slot::kStream1});
        if (layout == VL::Skinned) attrs.push_back({4, MTL::VertexFormatFloat3, 0, slot::kStream1});
        if (layout == VL::Ui) { // render/ui_vertex.h
            for (uint32_t i = 0; i < 5; ++i) attrs.push_back({i, MTL::VertexFormatFloat2, 8 * i, slot::kStream0});
            attrs.push_back({5, MTL::VertexFormatUChar4Normalized, 40, slot::kStream0});
            attrs.push_back({6, MTL::VertexFormatUChar4Normalized, 44, slot::kStream0});
        }
        if (attrs.empty()) return nullptr;
        bool declared[16] = {};
        if (NS::Array* va = vs->vertexAttributes())
            for (NS::UInteger i = 0; i < va->count(); ++i) {
                MTL::VertexAttribute* a = va->object<MTL::VertexAttribute>(i);
                if (a && a->isActive() && a->attributeIndex() < 16) declared[a->attributeIndex()] = true;
            }
        MTL::VertexDescriptor* d = MTL::VertexDescriptor::alloc()->init();
        bool stream1 = false;
        for (const A& a : attrs) {
            if (!declared[a.loc]) continue;
            d->attributes()->object(a.loc)->setFormat(a.fmt);
            d->attributes()->object(a.loc)->setOffset(a.offset);
            d->attributes()->object(a.loc)->setBufferIndex(a.buffer);
            stream1 |= a.buffer == slot::kStream1;
        }
        d->layouts()->object(slot::kStream0)->setStride(layout == VL::Ui ? sizeof(UiVertex) : sizeof(Vertex));
        d->layouts()->object(slot::kStream0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
        if (stream1) {
            d->layouts()->object(slot::kStream1)->setStride(layout == VL::Instanced ? kInstanceStride : sizeof(Vertex));
            d->layouts()->object(slot::kStream1)->setStepFunction(layout == VL::Instanced ? MTL::VertexStepFunctionPerInstance : MTL::VertexStepFunctionPerVertex);
            d->layouts()->object(slot::kStream1)->setStepRate(1);
        }
        return d;
    }

    // `masks`: per colour target, MTL::ColorWriteMask (default all).
    MTL::RenderPipelineState* Pipe(const char* vsName, const char* fsName, VL layout, std::initializer_list<MTL::PixelFormat> colors,
                                   MTL::PixelFormat depth, Blend blend = Blend::None, std::initializer_list<MTL::ColorWriteMask> masks = {}) {
        Pool pool;
        MTL::Function* vs = Fn(vsName);
        MTL::Function* fs = fsName ? Fn(fsName) : nullptr;
        if (!vs || (fsName && !fs)) return nullptr;
        MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
        d->setLabel(Str(fsName ? fsName : vsName));
        d->setVertexFunction(vs);
        d->setFragmentFunction(fs);
        MTL::VertexDescriptor* vd = VertexLayout(vs, layout);
        if (vd) { d->setVertexDescriptor(vd); vd->release(); }
        NS::UInteger i = 0;
        for (MTL::PixelFormat f : colors) {
            MTL::RenderPipelineColorAttachmentDescriptor* c = d->colorAttachments()->object(i);
            c->setPixelFormat(f);
            c->setWriteMask(i < masks.size() ? masks.begin()[i] : MTL::ColorWriteMaskAll);
            if (blend != Blend::None && i == 0) {
                c->setBlendingEnabled(true);
                c->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                c->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                c->setRgbBlendOperation(MTL::BlendOperationAdd);
                c->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                c->setDestinationAlphaBlendFactor(blend == Blend::Ui ? MTL::BlendFactorOneMinusSourceAlpha : MTL::BlendFactorZero);
                c->setAlphaBlendOperation(MTL::BlendOperationAdd);
                if (blend == Blend::Temporal) c->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
            }
            ++i;
        }
        d->setDepthAttachmentPixelFormat(depth);
        NS::Error* error = nullptr;
        MTL::RenderPipelineState* p = device_->newRenderPipelineState(d, &error);
        d->release();
        if (!p) LogError(fsName ? fsName : vsName, error);
        else pipes_.push_back(p);
        return p;
    }

    MTL::DepthStencilState* DepthState(MTL::CompareFunction compare, bool write) {
        MTL::DepthStencilDescriptor* d = MTL::DepthStencilDescriptor::alloc()->init();
        d->setDepthCompareFunction(compare);
        d->setDepthWriteEnabled(write);
        MTL::DepthStencilState* s = device_->newDepthStencilState(d);
        d->release();
        return s;
    }

    MTL::SamplerState* Sampler(MTL::SamplerAddressMode address, MTL::SamplerMipFilter mip, NS::UInteger anisotropy, bool compare, bool argumentBuffers) {
        MTL::SamplerDescriptor* d = MTL::SamplerDescriptor::alloc()->init();
        d->setMinFilter(MTL::SamplerMinMagFilterLinear);
        d->setMagFilter(MTL::SamplerMinMagFilterLinear);
        d->setMipFilter(mip);
        d->setSAddressMode(address);
        d->setTAddressMode(address);
        d->setRAddressMode(address);
        d->setMaxAnisotropy(anisotropy);
        if (compare) d->setCompareFunction(MTL::CompareFunctionLessEqual);
        d->setSupportArgumentBuffers(argumentBuffers);
        MTL::SamplerState* s = device_->newSamplerState(d);
        d->release();
        return s;
    }

    MTL::Texture* NewTexture(MTL::TextureType type, MTL::PixelFormat format, uint32_t w, uint32_t h, uint32_t depthOrLayers, uint32_t levels,
                             MTL::TextureUsage usage, MTL::StorageMode storage) {
        MTL::TextureDescriptor* d = MTL::TextureDescriptor::alloc()->init();
        d->setTextureType(type);
        d->setPixelFormat(format);
        d->setWidth(w);
        d->setHeight(h);
        if (type == MTL::TextureType3D) d->setDepth(depthOrLayers);
        if (type == MTL::TextureType2DArray) d->setArrayLength(depthOrLayers);
        d->setMipmapLevelCount(levels);
        d->setUsage(usage);
        d->setStorageMode(storage);
        MTL::Texture* t = device_->newTexture(d);
        d->release();
        return t;
    }

    // Tile-only attachment; falls back to private memory where memoryless is unsupported (the iOS Simulator on Intel Macs).
    MTL::Texture* NewTransientTarget(MTL::PixelFormat format, uint32_t w, uint32_t h) {
        MTL::Texture* t = NewTexture(MTL::TextureType2D, format, w, h, 1, 1, MTL::TextureUsageRenderTarget, MTL::StorageModeMemoryless);
        return t ? t : NewTexture(MTL::TextureType2D, format, w, h, 1, 1, MTL::TextureUsageRenderTarget, MTL::StorageModePrivate);
    }

    bool CreateFixedObjects() {
        for (Frame& f : frames_) {
            f.transient = device_->newBuffer(kTransientBytes, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
            f.light = device_->newBuffer(kLightBufferSize, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
            f.post = device_->newBuffer(2048, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
            f.motion = device_->newBuffer(256, MTL::ResourceStorageModeShared);
            if (!f.transient || !f.light || !f.post || !f.motion) return false;
            std::memset(f.motion->contents(), 0, 256);
        }
        dsScene_ = DepthState(MTL::CompareFunctionGreater, true); // reversed Z
        dsEqual_ = DepthState(MTL::CompareFunctionGreaterEqual, false);
        dsWater_ = DepthState(MTL::CompareFunctionGreater, false);
        dsOff_ = DepthState(MTL::CompareFunctionAlways, false);
        dsShadow_ = DepthState(MTL::CompareFunctionLess, true);
        clampSampler_ = Sampler(MTL::SamplerAddressModeClampToEdge, MTL::SamplerMipFilterLinear, 1, false, false);
        wrapSampler_ = Sampler(MTL::SamplerAddressModeRepeat, MTL::SamplerMipFilterLinear, 1, false, false);
        shadowSampler_ = Sampler(MTL::SamplerAddressModeClampToEdge, MTL::SamplerMipFilterNearest, 1, true, false);
        materialSampler_ = Sampler(MTL::SamplerAddressModeRepeat, MTL::SamplerMipFilterLinear, 8, false, true); // lives in argument buffers
        storeSampler_ = Sampler(MTL::SamplerAddressModeClampToEdge, MTL::SamplerMipFilterNearest, 1, false, false);
        const MTL::TextureUsage depthUse = MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
        shadow_ = NewTexture(MTL::TextureType2D, kDepthFormat, kShadowMapSize, kShadowMapSize, 1, 1, depthUse, MTL::StorageModePrivate);
        sunCache_ = NewTexture(MTL::TextureType2D, kDepthFormat, kShadowMapSize, kShadowMapSize, 1, 1, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        tightShadow_ = NewTexture(MTL::TextureType2D, kDepthFormat, kTightShadowMapSize, kTightShadowMapSize, 1, 1, depthUse, MTL::StorageModePrivate);
        captureDepth_ = NewTransientTarget(kDepthFormat, kCaptureSize, kCaptureSize);
        captureVelocity_ = NewTransientTarget(kVelocityFormat, kCaptureSize, kCaptureSize);
        dummy_ = NewTexture(MTL::TextureType2D, MTL::PixelFormatRGBA8Unorm, 1, 1, 1, 1, MTL::TextureUsageShaderRead, MTL::StorageModeShared);
        if (dummy_) { const uint8_t zero[4] = {}; dummy_->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, zero, 4); }
        return dsScene_ && dsEqual_ && dsWater_ && dsOff_ && dsShadow_ && clampSampler_ && wrapSampler_ && shadowSampler_ && materialSampler_ &&
               storeSampler_ && shadow_ && sunCache_ && tightShadow_ && captureDepth_ && captureVelocity_ && dummy_;
    }

    bool CreatePipelines() {
        const MTL::ColorWriteMask all = MTL::ColorWriteMaskAll, none = MTL::ColorWriteMaskNone;
        // The HDR scene: colour plus the velocity target, which only the motion pipelines write.
        scenePipe_ = Pipe("mesh_vs", "mesh_ps", VL::Mesh, {kHdrFormat, kVelocityFormat}, kDepthFormat, Blend::None, {all, none});
        overlayPipe_ = Pipe("mesh_vs", "mesh_overlay_ps", VL::Mesh, {kDrawableFormat}, kDepthFormat);
        instancedPipe_ = Pipe("mesh_inst_vs", "mesh_ps", VL::Instanced, {kHdrFormat, kVelocityFormat}, kDepthFormat, Blend::None, {all, none});
        // The depth pre-pass: the same vertex functions, so depth matches to the bit; no fragment function. Its shading half is
        // the normal pipeline with the depth-equal state (dsEqual_).
        prepassDepthPipe_[0] = Pipe("mesh_vs", nullptr, VL::Mesh, {kHdrFormat, kVelocityFormat}, kDepthFormat, Blend::None, {none, none});
        prepassDepthPipe_[1] = Pipe("mesh_inst_vs", nullptr, VL::Instanced, {kHdrFormat, kVelocityFormat}, kDepthFormat, Blend::None, {none, none});
        vegPipe_ = Pipe("mesh_veg_vs", "mesh_motion_ps", VL::Instanced, {kHdrFormat, kVelocityFormat}, kDepthFormat);
        vegDepthPipe_ = Pipe("mesh_veg_depth_vs", "mesh_veg_depth_ps", VL::Instanced, {kHdrFormat, kVelocityFormat}, kDepthFormat, Blend::None, {none, none});
        vegShadePipe_ = Pipe("mesh_veg_vs", "mesh_veg_shade_ps", VL::Instanced, {kHdrFormat, kVelocityFormat}, kDepthFormat);
        skinnedPipe_ = Pipe("mesh_skinned_motion_vs", "mesh_motion_ps", VL::Skinned, {kHdrFormat, kVelocityFormat}, kDepthFormat);
        // Shadows: depth only. Sun and tight maps share pipelines; their biases are set per pass (setDepthBias).
        shadowPipe_ = Pipe("mesh_shadow_vs", nullptr, VL::Mesh, {}, kDepthFormat);
        shadowInstPipe_ = Pipe("mesh_shadow_inst_vs", nullptr, VL::Instanced, {}, kDepthFormat);
        vegShadowPipe_ = Pipe("mesh_shadow_veg_vs", nullptr, VL::Instanced, {}, kDepthFormat);
        shadowCutPipe_ = Pipe("mesh_shadow_cut_vs", "mesh_shadow_cut_ps", VL::Mesh, {}, kDepthFormat);
        return scenePipe_ && overlayPipe_ && instancedPipe_ && prepassDepthPipe_[0] && prepassDepthPipe_[1] && vegPipe_ && vegDepthPipe_ &&
               vegShadePipe_ && skinnedPipe_ && shadowPipe_ && shadowInstPipe_ && vegShadowPipe_ && shadowCutPipe_;
    }

    bool CreatePostPipelines() {
        const struct { const char* fs; MTL::PixelFormat format; } passes[11] = {
            {"post_ao_ps", MTL::PixelFormatR8Unorm}, {"post_clouds_ps", kHdrFormat}, {"post_composite_ps", kHdrFormat},
            {"post_taa_ps", kHdrFormat}, {"post_tonemap_ps", kDrawableFormat}, {"post_probetrace_ps", kHdrFormat},
            {"post_probeblend_ps", kHdrFormat}, {"post_exposure_ps", kHdrFormat}, {"post_bloom_ps", kHdrFormat},
            {"post_cloudshadow_ps", kHdrFormat}, {"post_cloudresolve_ps", kHdrFormat}};
        for (int i = 0; i < 11; ++i) {
            // The temporal filters (probe blend, exposure) blend, with alpha as the new data's weight; RGB only. The tone map
            // shares its render pass with the overlays, which keep a depth attachment.
            postPipes_[i] = Pipe("post_vs", passes[i].fs, VL::None, {passes[i].format}, i == 4 ? kDepthFormat : MTL::PixelFormatInvalid,
                                 (i == 6 || i == 7) ? Blend::Temporal : Blend::None);
            if (!postPipes_[i]) return false;
        }
        return true;
    }

    bool CreateUiPipeline() {
        uiPipe_ = Pipe("ui_vs", "ui_ps", VL::Ui, {kDrawableFormat}, kDepthFormat, Blend::Ui);
        return uiPipe_ != nullptr;
    }

    bool CreateWaterPipeline() {
        waterPipe_ = Pipe("water_vs", "water_ps", VL::Mesh, {kHdrFormat, kVelocityFormat}, kDepthFormat);
        return waterPipe_ != nullptr;
    }

    // ------------------------------------------------------------------ materials (argument buffers)
    // Vulkan's set 1 is one argument buffer per (texture triple, layout): layout 0 is the full set (albedo, normal, sampler,
    // surface, extra) that mesh_ps / mesh_overlay_ps / mesh_motion_ps declare, layout 1 the albedo-and-sampler set of the
    // cut-out shaders (mesh_shadow_cut_ps, mesh_veg_depth_ps): spirv-cross only declares what a shader uses, so the two
    // layouts differ and each gets its own encoder. Argument buffers live in fixed-size slots of 256 KB pages.
    static constexpr NS::UInteger kMatPageBytes = 256 * 1024;

    bool CreateMaterialResources() {
        MTL::Function* full = Fn("mesh_ps");
        MTL::Function* cut = Fn("mesh_shadow_cut_ps");
        if (!full || !cut) return false;
        matEncoder_[0] = full->newArgumentEncoder(slot::kMaterial);
        matEncoder_[1] = cut->newArgumentEncoder(slot::kMaterial);
        if (!matEncoder_[0] || !matEncoder_[1]) return false;
        matStride_ = (std::max(matEncoder_[0]->encodedLength(), matEncoder_[1]->encodedLength()) + 255) / 256 * 256;
        const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, surface[4] = {128, 255, 255, 255}, extra[4] = {255, 255, 255, 0};
        defaults_[0] = CreateTexture(white, 1, 1);
        defaults_[1] = CreateTexture(flat, 1, 1);
        defaults_[2] = CreateTexture(surface, 1, 1);
        defaults_[3] = CreateTexture(extra, 1, 1);
        return defaults_[0] != kNoTexture && defaults_[1] != kNoTexture && defaults_[2] != kNoTexture && defaults_[3] != kNoTexture;
    }

    TextureId ExtraOf(TextureId surface) const { return surface < extraOf_.size() ? extraOf_[surface] : kNoTexture; }
    bool LiveTexture(TextureId id) const { return id < textures_.size() && textures_[id].tex; }

    int32_t AllocMatSlot() {
        if (!matFree_.empty()) { const int32_t s = matFree_.back(); matFree_.pop_back(); return s; }
        const NS::UInteger perPage = kMatPageBytes / matStride_;
        if (matPages_.empty() || matNext_ == perPage) {
            MTL::Buffer* page = device_->newBuffer(kMatPageBytes, MTL::ResourceStorageModeShared);
            if (!page) return -1;
            matPages_.push_back(page);
            matNext_ = 0;
        }
        return (int32_t)((matPages_.size() - 1) * perPage + matNext_++);
    }
    MTL::Buffer* MatPage(int32_t s, NS::UInteger& offset) const {
        const NS::UInteger perPage = kMatPageBytes / matStride_;
        offset = ((NS::UInteger)s % perPage) * matStride_;
        return matPages_[(size_t)s / perPage];
    }

    // Binds the current material's argument buffer for `layout` (and makes its textures resident) unless already bound.
    void BindMaterial(int layout) {
        TextureId albedo = curMat_[0], normal = curMat_[1], surface = curMat_[2];
        if (!LiveTexture(albedo)) albedo = defaults_[0];
        if (!LiveTexture(normal)) normal = defaults_[1];
        if (!LiveTexture(surface)) surface = defaults_[2];
        const uint64_t key = ((uint64_t)albedo << 42) | ((uint64_t)normal << 21) | surface;
        if (boundMatKey_ == key && boundMatLayout_ == layout) return;
        Material& m = materials_[key];
        if (m.slot[0] < 0 && m.slot[1] < 0) {
            TextureId extra = ExtraOf(surface);
            if (!LiveTexture(extra)) extra = defaults_[3];
            m.t[0] = albedo; m.t[1] = normal; m.t[2] = surface; m.t[3] = extra;
        }
        if (m.slot[layout] < 0) {
            m.slot[layout] = AllocMatSlot();
            if (m.slot[layout] < 0) return;
            NS::UInteger offset = 0;
            MTL::Buffer* page = MatPage(m.slot[layout], offset);
            MTL::ArgumentEncoder* enc = matEncoder_[layout];
            enc->setArgumentBuffer(page, offset);
            enc->setTexture(textures_[m.t[0]].tex, 0);
            enc->setSamplerState(materialSampler_, 2);
            if (layout == 0) {
                enc->setTexture(textures_[m.t[1]].tex, 1);
                enc->setTexture(textures_[m.t[2]].tex, 3);
                enc->setTexture(textures_[m.t[3]].tex, 4);
            }
        }
        NS::UInteger offset = 0;
        MTL::Buffer* page = MatPage(m.slot[layout], offset);
        rce_->setFragmentBuffer(page, offset, slot::kMaterial);
        const MTL::Resource* used[4] = {textures_[m.t[0]].tex, textures_[m.t[1]].tex, textures_[m.t[2]].tex, textures_[m.t[3]].tex};
        rce_->useResources(used, layout == 0 ? 4 : 1, MTL::ResourceUsageRead, MTL::RenderStageFragment);
        boundMatKey_ = key;
        boundMatLayout_ = layout;
    }

    // Forgets the argument buffers that name `id` (as surface only, when `asSurface`). Their slots come back once no frame
    // in flight can read them.
    void DropMaterialsUsing(TextureId id, bool asSurface) {
        for (auto it = materials_.begin(); it != materials_.end();) {
            const Material& m = it->second;
            const bool hit = asSurface ? m.t[2] == id : (m.t[0] == id || m.t[1] == id || m.t[2] == id || m.t[3] == id);
            if (!hit) { ++it; continue; }
            for (int l = 0; l < 2; ++l)
                if (m.slot[l] >= 0) { Retired r; r.serial = frameSerial_; r.matSlot = m.slot[l]; retired_.push_back(r); }
            it = materials_.erase(it);
        }
        boundMatKey_ = ~0ull;
    }

    // ------------------------------------------------------------------ encoders
    void EndEncoder() {
        if (rce_) rce_->endEncoding();
        if (cce_) cce_->endEncoding();
        if (bce_) bce_->endEncoding();
        rce_ = nullptr;
        cce_ = nullptr;
        bce_ = nullptr;
        pass_ = Pass::None;
        boundPipe_ = nullptr;
    }

    MTL::BlitCommandEncoder* BlitEncoder() {
        if (pass_ == Pass::Blit) return bce_;
        EndEncoder();
        bce_ = cmd_->blitCommandEncoder();
        pass_ = Pass::Blit;
        return bce_;
    }

    void BeginRender(std::initializer_list<Att> colors, const Att* depth, uint32_t w, uint32_t h, Pass pass) {
        EndEncoder();
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        NS::UInteger i = 0;
        for (const Att& a : colors) {
            MTL::RenderPassColorAttachmentDescriptor* c = rp->colorAttachments()->object(i++);
            c->setTexture(a.tex);
            c->setSlice(a.slice);
            c->setLoadAction(a.load);
            c->setStoreAction(a.store);
            c->setClearColor(MTL::ClearColor::Make(a.clear[0], a.clear[1], a.clear[2], a.clear[3]));
        }
        if (depth && depth->tex) {
            rp->depthAttachment()->setTexture(depth->tex);
            rp->depthAttachment()->setLoadAction(depth->load);
            rp->depthAttachment()->setStoreAction(depth->store);
            rp->depthAttachment()->setClearDepth(depth->clearDepth);
        }
        rce_ = cmd_->renderCommandEncoder(rp);
        pass_ = pass;
        boundPipe_ = nullptr;
        boundMatKey_ = ~0ull;
        // Metal's clip space and texture origin match Direct3D, so no flipped viewport (Vulkan needs one).
        rce_->setViewport(MTL::Viewport{0.0, 0.0, (double)w, (double)h, 0.0, 1.0});
        rce_->setScissorRect(MTL::ScissorRect{0, 0, w, h});
    }

    // The HDR scene pass: scene colour, velocity and depth, cleared the first time in a frame and loaded after a break.
    void BeginScene() {
        const MTL::LoadAction load = sceneStarted_ ? MTL::LoadActionLoad : MTL::LoadActionClear;
        Att color, velocity, depth;
        color.tex = post_[kScene];
        color.load = load;
        std::memcpy(color.clear, clear_, sizeof(clear_));
        velocity.tex = post_[kVelocity];
        velocity.load = load; // zero: "ask the depth buffer"
        depth.tex = depth_;
        depth.load = load;
        depth.clearDepth = 0.0; // reversed Z
        BeginRender({color, velocity}, &depth, sceneW_, sceneH_, Pass::Scene);
        sceneStarted_ = true;
        BindMeshResources(true);
    }

    // The encoder lit draws go into: the open scene, capture or overlay pass; otherwise (before ResolveScene) the scene.
    MTL::RenderCommandEncoder* DrawEncoder() {
        if (pass_ == Pass::Scene || pass_ == Pass::Capture || pass_ == Pass::Overlay) return rce_;
        if (resolved_) return nullptr; // overlays with no drawable this frame
        BeginScene();
        return rce_;
    }

    void BeginShadowEncoder(ShadowMap map, MTL::LoadAction load) {
        const bool tight = map == ShadowMap::Tight;
        const uint32_t size = tight ? kTightShadowMapSize : kShadowMapSize;
        Att depth;
        depth.tex = tight ? tightShadow_ : shadow_;
        depth.load = load;
        depth.clearDepth = 1.0;
        BeginRender({}, &depth, size, size, Pass::Shadow);
        rce_->setDepthBias(tight ? kTightDepthBias : kShadowDepthBias, tight ? kTightSlopeBias : kShadowSlopeBias, 0.0f);
        BindMeshResources(false); // the shadow maps are attachments here: no fragment textures
    }

    // Set 0 of the mesh pipelines: lighting and motion buffers, the shadow maps, probes and cloud shadow.
    void BindMeshResources(bool fragmentTextures) {
        rce_->setVertexBuffer(frame_->light, 0, slot::kLight);
        rce_->setFragmentBuffer(frame_->light, 0, slot::kLight);
        rce_->setVertexBuffer(frame_->motion, 0, slot::kMotion);
        rce_->setFragmentBuffer(frame_->motion, 0, slot::kMotion);
        if (!fragmentTextures) return;
        rce_->setFragmentTexture(shadow_, slot::kShadowTex);
        rce_->setFragmentTexture(tightShadow_, slot::kTightTex);
        rce_->setFragmentTexture(post_[kProbes], slot::kProbesTex);
        rce_->setFragmentTexture(post_[kCloudShadow], slot::kCloudShadowTex);
        rce_->setFragmentSamplerState(shadowSampler_, slot::kShadowSampler);
    }

    // `materialLayout`: which argument buffer the pipeline's fragment function reads (-1 none).
    void UsePipe(MTL::RenderPipelineState* pipe, int materialLayout, MTL::DepthStencilState* depth) {
        if (pipe != boundPipe_) {
            rce_->setRenderPipelineState(pipe);
            boundPipe_ = pipe;
        }
        rce_->setDepthStencilState(depth);
        if (materialLayout >= 0) BindMaterial(materialLayout);
    }

    void PushMesh(const Mat4& mvp, const Mat4& model) {
        MeshPush pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        std::memcpy(pc.material, materialConstants_, sizeof(pc.material));
        rce_->setVertexBytes(&pc, sizeof(pc), slot::kPush);
        rce_->setFragmentBytes(&pc, sizeof(pc), slot::kPush);
    }

    uint32_t NewestCopy(const Mesh& mesh) const { return mesh.copyParent != kInvalidMesh ? meshes_[mesh.copyParent].newestCopy : mesh.newestCopy; }

    // A mesh into the open pass with the bound pipeline: vertices from the newest copy when deformable.
    void DrawMeshInPass(const Mesh& mesh, const Mat4& mvp, const Mat4& model) {
        PushMesh(mvp, model);
        rce_->setVertexBuffer(mesh.buffer, mesh.copyBytes * NewestCopy(mesh), slot::kStream0);
        if (mesh.indexed) rce_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, mesh.count, MTL::IndexTypeUInt32, mesh.buffer, mesh.indexOffset);
        else rce_->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)mesh.count);
    }

    void DrawInstancedMesh(const Mesh& mesh, MTL::Buffer* instances, uint32_t first, uint32_t count) {
        rce_->setVertexBuffer(mesh.buffer, 0, slot::kStream0);
        rce_->setVertexBuffer(instances, 0, slot::kStream1);
        if (mesh.indexed) rce_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, mesh.count, MTL::IndexTypeUInt32, mesh.buffer, mesh.indexOffset, count, 0, first);
        else rce_->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)mesh.count, (NS::UInteger)count, (NS::UInteger)first);
    }

    void VegetationIndirect(const Mesh& mesh, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) {
        PushMesh(viewProj, params);
        rce_->setVertexBuffer(mesh.buffer, 0, slot::kStream0);
        rce_->setVertexBuffer(gpuBuffers_[instances].buffer, 0, slot::kStream1);
        rce_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, MTL::IndexTypeUInt32, mesh.buffer, mesh.indexOffset, gpuBuffers_[args].buffer, argsOffset);
    }

    void Submit(MTL::Buffer* buffer, NS::UInteger offset, uint32_t count, Topology topology, const Mat4& mvp, const Mat4& model, bool depthTest,
                bool indexed = false, NS::UInteger indexOffset = 0) {
        const bool lit = topology == Topology::Triangles && depthTest;
        if (!resolved_ && prepass_ == 1 && lit) UsePipe(prepassDepthPipe_[0], -1, dsScene_);
        else UsePipe(resolved_ ? overlayPipe_ : scenePipe_, 0, !depthTest ? dsOff_ : (!resolved_ && prepass_ == 2 && lit) ? dsEqual_ : dsScene_);
        PushMesh(mvp, model);
        rce_->setVertexBuffer(buffer, offset, slot::kStream0);
        const MTL::PrimitiveType prim = topology == Topology::Lines ? MTL::PrimitiveTypeLine : MTL::PrimitiveTypeTriangle;
        if (indexed) rce_->drawIndexedPrimitives(prim, count, MTL::IndexTypeUInt32, buffer, indexOffset);
        else rce_->drawPrimitives(prim, (NS::UInteger)0, (NS::UInteger)count);
    }

    bool ValidGpu(GpuBufferId id) const { return id < gpuBuffers_.size() && gpuBuffers_[id].buffer; }

    // ------------------------------------------------------------------ post chain
    enum PostTarget : uint32_t { kScene, kAo, kClouds, kComposed, kHistory0, kHistory1, kProbes, kProbeRaysTarget, kExposure, kBloom, kCloudShadow, kVelocity, kClouds1, kCloudMarch, kCapture, kPostTargets };

    // post.hlsl's set 0 for parity `cur`, with any texture that is an attachment of the pass swapped for a dummy (Metal must
    // not sample what it renders to, even where the shader never reads that binding).
    void BindPostResources(uint32_t cur, const MTL::Texture* target, const MTL::Texture* attachedDepth) {
        const MTL::Texture* t[22] = {};
        t[1] = post_[kScene];
        t[2] = depth_;
        t[3] = post_[kAo];
        t[4] = post_[cur ? kClouds1 : kClouds];
        t[5] = cloudNoise_;
        t[6] = post_[kComposed];
        t[7] = post_[kHistory0 + (1 - cur)];
        t[8] = post_[kHistory0 + cur];
        t[11] = shadow_;
        t[12] = post_[kProbes];
        t[13] = post_[kProbeRaysTarget];
        t[14] = post_[kExposure];
        t[15] = post_[kBloom];
        t[16] = post_[kCloudShadow];
        t[17] = post_[kVelocity];
        t[18] = cloudField_;
        t[19] = post_[cur ? kClouds : kClouds1];
        t[20] = post_[kCloudMarch];
        t[21] = post_[kCapture];
        for (const MTL::Texture*& x : t)
            if (x && (x == target || x == attachedDepth)) x = dummy_;
        rce_->setFragmentTextures(t + 1, NS::Range::Make(1, 21));
        rce_->setFragmentSamplerState(clampSampler_, slot::kPostClamp);
        rce_->setFragmentSamplerState(wrapSampler_, slot::kPostWrap);
        rce_->setFragmentBuffer(frame_->post, 0, slot::kPostParams);
    }

    void PostPass(uint32_t target, int pipe, uint32_t w, uint32_t h, uint32_t cur) {
        Att color;
        color.tex = post_[target];
        color.load = (pipe == 6 || pipe == 7) ? MTL::LoadActionLoad : MTL::LoadActionDontCare; // the temporal ones blend over the old
        BeginRender({color}, nullptr, w, h, Pass::Post);
        rce_->setRenderPipelineState(postPipes_[pipe]);
        BindPostResources(cur, post_[target], nullptr);
        rce_->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
        EndEncoder();
    }

    // The composed picture is copied for refraction and the depth for the shader's own reads (it is also the attachment the
    // water tests against), then the water draws over the composed picture.
    void WaterPass(uint32_t w, uint32_t h) {
        waterPending_ = false;
        if (!ValidGpu(water_.waves) || !ValidGpu(water_.ripple) || !ValidGpu(water_.sky)) return;
        MTL::BlitCommandEncoder* b = BlitEncoder();
        b->copyFromTexture(post_[kComposed], waterRefract_);
        b->copyFromTexture(depth_, depthCopy_);
        EndEncoder();
        Att color, velocity, depth;
        color.tex = post_[kComposed];
        color.load = MTL::LoadActionLoad;
        velocity.tex = post_[kVelocity];
        velocity.load = MTL::LoadActionLoad;
        depth.tex = depth_;
        depth.load = MTL::LoadActionLoad;
        BeginRender({color, velocity}, &depth, w, h, Pass::Water);
        UsePipe(waterPipe_, -1, dsWater_);
        for (int stage = 0; stage < 2; ++stage) {
            auto buffer = [&](MTL::Buffer* buf, NS::UInteger index) { stage ? rce_->setFragmentBuffer(buf, 0, index) : rce_->setVertexBuffer(buf, 0, index); };
            buffer(frame_->post, slot::kWaterPost);
            buffer(frame_->light, slot::kWaterLight);
            buffer(gpuBuffers_[water_.waves].buffer, slot::kWaterWaves);
            buffer(gpuBuffers_[water_.ripple].buffer, slot::kWaterRipple);
            buffer(gpuBuffers_[water_.sky].buffer, slot::kWaterSky);
        }
        rce_->setFragmentTexture(depthCopy_, slot::kWaterDepth);
        rce_->setFragmentTexture(waterRefract_, slot::kWaterRefract);
        rce_->setFragmentTexture(shadow_, slot::kWaterShadow);
        rce_->setFragmentTexture(post_[kProbes], slot::kWaterProbes);
        rce_->setFragmentTexture(post_[kCloudShadow], slot::kWaterCloudShadow);
        rce_->setFragmentSamplerState(clampSampler_, slot::kWaterClamp);
        rce_->setFragmentSamplerState(shadowSampler_, slot::kWaterCmp);
        struct { float viewProj[16], ripple[4], near_[4], tiles[4], wind[4]; } pc;
        std::memcpy(pc.viewProj, water_.viewProj.m, sizeof(pc.viewProj));
        std::memcpy(pc.ripple, water_.ripplePlace, sizeof(pc.ripple));
        std::memcpy(pc.near_, water_.nearPlace, sizeof(pc.near_));
        std::memcpy(pc.tiles, water_.tiles, sizeof(pc.tiles));
        std::memcpy(pc.wind, water_.wind, sizeof(pc.wind));
        for (size_t i = 0; i < waterMeshes_.size(); ++i) {
            const MeshId id = waterMeshes_[i];
            if (id >= meshes_.size() || !meshes_[id].live || !meshes_[id].indexed) continue;
            const Mesh& mesh = meshes_[id];
            pc.tiles[3] = i < water_.nearCount ? 1.0f : 0.0f;
            rce_->setVertexBytes(&pc, sizeof(pc), slot::kPush);
            rce_->setFragmentBytes(&pc, sizeof(pc), slot::kPush);
            rce_->setVertexBuffer(mesh.buffer, mesh.copyBytes * NewestCopy(mesh), slot::kStream0);
            rce_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, mesh.count, MTL::IndexTypeUInt32, mesh.buffer, mesh.indexOffset);
            ++counts_.draws;
            counts_.triangles += mesh.count / 3;
        }
        EndEncoder();
    }

    // The scene's depth at the render size, its copy for the water, and (scale below 1) the overlays' window-sized depth.
    bool CreateDepth() {
        RetireObject(depth_);
        RetireObject(depthCopy_);
        RetireObject(overlayDepth_);
        depth_ = NewTexture(MTL::TextureType2D, kDepthFormat, sceneW_, sceneH_, 1, 1, MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        depthCopy_ = NewTexture(MTL::TextureType2D, kDepthFormat, sceneW_, sceneH_, 1, 1, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        if (scale_ < 1.0f) overlayDepth_ = NewTransientTarget(kDepthFormat, w_, h_);
        return depth_ && depthCopy_ && (scale_ >= 1.0f || overlayDepth_);
    }

    bool CreatePostTargets() {
        for (MTL::Texture*& t : post_) RetireObject(t);
        RetireObject(waterRefract_);
        ++captureEpoch_;
        const uint32_t w = sceneW_, h = sceneH_;
        for (uint32_t t = 0; t < kPostTargets; ++t) {
            const uint32_t tw = t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, w / 2) : t == kCloudMarch ? std::max(1u, w / 4) : t == kProbes ? kProbeX * 3 : t == kProbeRaysTarget ? kProbeRays : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, w / 4) : w;
            const uint32_t th = t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, h / 2) : t == kCloudMarch ? std::max(1u, h / 4) : t == kProbes ? kProbeAtlasH : t == kProbeRaysTarget ? kProbesPerFrame : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, h / 4) : h;
            const MTL::PixelFormat format = t == kAo ? MTL::PixelFormatR8Unorm : t == kVelocity ? kVelocityFormat : kHdrFormat;
            post_[t] = NewTexture(MTL::TextureType2D, format, tw, th, 1, 1, MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
            if (!post_[t]) return false;
        }
        waterRefract_ = NewTexture(MTL::TextureType2D, kHdrFormat, w, h, 1, 1, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        probeAge_ = 0;
        cloudMapValid_ = false;
        historyValid_ = false;
        return waterRefract_ != nullptr;
    }

    bool CreateCloudTextures() {
        const std::vector<uint8_t> voxels = GenerateCloudNoise();
        const std::vector<uint8_t> field = GenerateCloudField();
        constexpr uint32_t kN = kCloudNoiseSize, kF = kCloudFieldSize;
        cloudNoise_ = NewTexture(MTL::TextureType3D, MTL::PixelFormatRGBA8Unorm, kN, kN, kN, 1, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        cloudField_ = NewTexture(MTL::TextureType2D, MTL::PixelFormatRGBA8Unorm, kF, kF, 1, 1, MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        if (!cloudNoise_ || !cloudField_) return false;
        MTL::Buffer* staging = device_->newBuffer(voxels.size() + field.size(), MTL::ResourceStorageModeShared);
        if (!staging) return false;
        std::memcpy(staging->contents(), voxels.data(), voxels.size());
        std::memcpy(static_cast<uint8_t*>(staging->contents()) + voxels.size(), field.data(), field.size());
        const NS::UInteger fieldAt = voxels.size();
        return Upload([&](MTL::BlitCommandEncoder* b) {
            b->copyFromBuffer(staging, 0, kN * 4, kN * kN * 4, MTL::Size(kN, kN, kN), cloudNoise_, 0, 0, MTL::Origin::Make(0, 0, 0));
            b->copyFromBuffer(staging, fieldAt, kF * 4, kF * kF * 4, MTL::Size(kF, kF, 1), cloudField_, 0, 0, MTL::Origin::Make(0, 0, 0));
        }, staging, true);
    }

    // ------------------------------------------------------------------ uploads and lifetime
    MTL::Buffer* NewMeshBuffer(const void* vertices, size_t vertexBytes, const void* indices, size_t indexBytes) {
        MTL::Buffer* b = device_->newBuffer(vertexBytes + indexBytes, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!b) return nullptr;
        std::memcpy(b->contents(), vertices, vertexBytes);
        if (indexBytes) std::memcpy(static_cast<uint8_t*>(b->contents()) + vertexBytes, indices, indexBytes);
        return b;
    }

    MeshId PlaceMesh(Mesh mesh) {
        mesh.live = true;
        if (!freeSlots_.empty()) {
            const MeshId id = freeSlots_.back();
            freeSlots_.pop_back();
            meshes_[id] = mesh;
            return id;
        }
        meshes_.push_back(mesh);
        return (MeshId)(meshes_.size() - 1);
    }

    // Blits on a command buffer of its own. It runs before any frame committed later (same queue, commit order), so a
    // NoWait upload can be drawn by the very next frame; `wait` blocks until it is done. `staging` is released afterwards.
    bool Upload(const std::function<void(MTL::BlitCommandEncoder*)>& record, MTL::Buffer* staging, bool wait) {
        Pool pool;
        MTL::CommandBuffer* cb = queue_->commandBuffer();
        MTL::BlitCommandEncoder* b = cb->blitCommandEncoder();
        record(b);
        b->endEncoding();
        if (staging) {
            const MTL::HandlerFunction release = [staging](MTL::CommandBuffer*) { staging->release(); };
            cb->addCompletedHandler(release);
        }
        cb->commit();
        if (!wait) return true;
        cb->waitUntilCompleted();
        return cb->status() == MTL::CommandBufferStatusCompleted;
    }

    TextureId UploadTexture(const std::vector<std::vector<uint8_t>>& levels, const std::vector<std::pair<int, int>>& dims, bool wait) {
        const uint32_t count = (uint32_t)levels.size();
        MTL::Texture* tex = NewTexture(MTL::TextureType2D, MTL::PixelFormatRGBA8Unorm, (uint32_t)dims[0].first, (uint32_t)dims[0].second, 1, count,
                                       MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        if (!tex) return kNoTexture;
        size_t total = 0;
        for (const auto& l : levels) total += (l.size() + 15) / 16 * 16;
        MTL::Buffer* staging = device_->newBuffer(total, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!staging) { tex->release(); return kNoTexture; }
        std::vector<NS::UInteger> offsets(count);
        size_t offset = 0;
        for (uint32_t m = 0; m < count; ++m) {
            std::memcpy(static_cast<uint8_t*>(staging->contents()) + offset, levels[m].data(), levels[m].size());
            offsets[m] = offset;
            offset += (levels[m].size() + 15) / 16 * 16;
        }
        const bool ok = Upload([&](MTL::BlitCommandEncoder* b) {
            for (uint32_t m = 0; m < count; ++m) {
                const NS::UInteger w = (NS::UInteger)dims[m].first, h = (NS::UInteger)dims[m].second;
                b->copyFromBuffer(staging, offsets[m], w * 4, w * h * 4, MTL::Size(w, h, 1), tex, 0, m, MTL::Origin::Make(0, 0, 0));
            }
        }, staging, wait);
        if (!ok) { tex->release(); return kNoTexture; }
        TextureId id;
        if (!freeTextures_.empty()) { id = freeTextures_.back(); freeTextures_.pop_back(); textures_[id].tex = tex; }
        else { id = (TextureId)textures_.size(); textures_.push_back({tex}); }
        return id;
    }

    // Released once every frame begun so far is done (a frame being recorded counts).
    template <class T> void RetireObject(T*& o) {
        if (!o) return;
        Retired r;
        r.serial = frameSerial_;
        r.obj = o;
        retired_.push_back(r);
        o = nullptr;
    }

    void FlushRetired(bool all) {
        const uint64_t done = completedSerial_.load();
        const double start = TraceNowMs();
        std::vector<Retired> list;
        list.swap(retired_); // what this flush retires in turn (a texture's material slots) lands in the fresh retired_
        for (Retired& r : list) {
            // Unloading a town frees thousands of objects: at most 2 ms of it per frame.
            if (!all && (r.serial > done || TraceNowMs() - start > 2.0)) { retired_.push_back(r); continue; }
            if (r.obj) r.obj->release();
            if (r.matSlot >= 0) matFree_.push_back(r.matSlot);
            if (r.mesh != kInvalidMesh && r.mesh < meshes_.size() && meshes_[r.mesh].live) {
                Mesh& m = meshes_[r.mesh];
                if (!m.sharesBuffer) { Release(m.buffer); Release(m.upload); }
                m = {};
                freeSlots_.push_back(r.mesh);
            }
            if (r.texture != kNoTexture && r.texture < textures_.size() && textures_[r.texture].tex) {
                DropMaterialsUsing(r.texture, false);
                RetireObject(textures_[r.texture].tex); // the material slots it named just went on the list too
                textures_[r.texture] = {};
                if (r.texture < extraOf_.size()) extraOf_[r.texture] = kNoTexture;
                freeTextures_.push_back(r.texture);
            }
        }
        if (all && !retired_.empty()) FlushRetired(true); // what the texture frees added
    }

    void WaitIdle() {
        if (lastCommitted_) lastCommitted_->waitUntilCompleted();
    }

    bool PrepareCapture() {
        const NS::UInteger bytes = (NS::UInteger)w_ * h_ * 4;
        if (captureBuffer_ && captureBuffer_->length() == bytes) return true;
        RetireObject(captureBuffer_);
        captureBuffer_ = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        return captureBuffer_ != nullptr;
    }

    // Whole-frame GPU time from the command buffers (GPUStartTime / GPUEndTime). The per-pass split of the other backends
    // (gpuPasses_) needs counter sample buffers at encoder boundaries: not done yet, those stay zero.
    void ReportTimers() {
        if (++timerFrames_ < 60) return;
        timerFrames_ = 0;
        const uint64_t samples = gpuSamples_.exchange(0), micros = gpuMicros_.exchange(0);
        if (samples) Log("Metal: GPU frame %.2f ms at %ux%u (render %ux%u, %llu-frame mean)", (double)micros / 1000.0 / (double)samples, w_, h_, sceneW_, sceneH_, (unsigned long long)samples);
        LogCounts("Metal");
    }

    // ------------------------------------------------------------------ texture self-test (TBD_TEXTURE_SELFTEST)
    // Renders known values into layer 3 of three texture arrays (clearing every other layer), then samples layers 3 and 2
    // into a 2x1 target and reads it back; the same check as the other backends.
    bool SelfTestTextures() {
        Pool pool;
        constexpr uint32_t kTile = 16, kLayers = 6, kLayer = 3;
        const MTL::PixelFormat formats[3] = {MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatR16Unorm};
        MTL::Texture* store[3] = {};
        for (int c = 0; c < 3; ++c)
            store[c] = NewTexture(MTL::TextureType2DArray, formats[c], kTile, kTile, kLayers, 1, MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead, MTL::StorageModePrivate);
        MTL::Texture* target = NewTexture(MTL::TextureType2D, MTL::PixelFormatRGBA8Unorm, 2, 1, 1, 1, MTL::TextureUsageRenderTarget, MTL::StorageModePrivate);
        MTL::Buffer* readback = device_->newBuffer(8, MTL::ResourceStorageModeShared);
        MTL::RenderPipelineState* fill = Pipe("selftest_vs", "selftest_fill_ps", VL::None, {formats[0], formats[1], formats[2]}, MTL::PixelFormatInvalid);
        MTL::RenderPipelineState* sample = Pipe("selftest_vs", "selftest_sample_ps", VL::None, {MTL::PixelFormatRGBA8Unorm}, MTL::PixelFormatInvalid);
        bool ok = store[0] && store[1] && store[2] && target && readback && fill && sample;
        if (ok) {
            MTL::CommandBuffer* cb = queue_->commandBuffer();
            for (uint32_t l = 0; l < kLayers; ++l) {
                MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
                for (NS::UInteger c = 0; c < 3; ++c) {
                    rp->colorAttachments()->object(c)->setTexture(store[c]);
                    rp->colorAttachments()->object(c)->setSlice(l);
                    rp->colorAttachments()->object(c)->setLoadAction(MTL::LoadActionClear);
                    rp->colorAttachments()->object(c)->setStoreAction(MTL::StoreActionStore);
                    rp->colorAttachments()->object(c)->setClearColor(MTL::ClearColor::Make(0, 0, 0, 0));
                }
                MTL::RenderCommandEncoder* e = cb->renderCommandEncoder(rp);
                if (l == kLayer) {
                    e->setRenderPipelineState(fill);
                    e->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
                }
                e->endEncoding();
            }
            MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
            rp->colorAttachments()->object(0)->setTexture(target);
            rp->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionClear);
            rp->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
            MTL::RenderCommandEncoder* e = cb->renderCommandEncoder(rp);
            e->setRenderPipelineState(sample);
            for (NS::UInteger c = 0; c < 3; ++c) e->setFragmentTexture(store[c], c);
            e->setFragmentSamplerState(storeSampler_, 3);
            e->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
            e->endEncoding();
            MTL::BlitCommandEncoder* b = cb->blitCommandEncoder();
            b->copyFromTexture(target, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size(2, 1, 1), readback, 0, 8, 8);
            b->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();
            uint8_t got[8];
            std::memcpy(got, readback->contents(), 8);
            const uint8_t want[8] = {255, 128, 255, 191, 0, 0, 0, 0}; // pixel 0 = (1.0, 0.5, 1.0, 0.75); pixel 1 = an untouched layer
            for (int i = 0; i < 8; ++i)
                if (std::abs((int)got[i] - (int)want[i]) > 2) ok = false;
            Log("Metal: self-test pixels (%u %u %u %u) (%u %u %u %u), expected (255 128 255 191) (0 0 0 0)", got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
        }
        for (MTL::Texture*& t : store) Release(t);
        Release(target);
        Release(readback);
        return ok; // the two pipelines stay in pipes_ and go at shutdown
    }

    void Shutdown() {
        if (!device_) { Release(layer_); return; }
        if (frameActive_) { EndEncoder(); cmd_->commit(); frameActive_ = false; Release(lastCommitted_); lastCommitted_ = cmd_->retain(); framePool_->release(); framePool_ = nullptr; }
        WaitIdle();
        if (inflight_) { // every completion handler has run (and signalled) once all slots can be taken
            for (uint32_t i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
            for (uint32_t i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_signal(inflight_);
        }
        completedSerial_ = ~0ull;
        FlushRetired(true);
        Release(lastCommitted_);
        for (Mesh& m : meshes_)
            if (m.live && !m.sharesBuffer) { Release(m.buffer); Release(m.upload); }
        for (MTL::Buffer*& b : instanceBuffers_) Release(b);
        for (GpuBuffer& g : gpuBuffers_) Release(g.buffer);
        for (Texture& t : textures_) Release(t.tex);
        for (Compute& c : computes_) Release(c.pipe);
        for (MTL::Buffer*& p : matPages_) Release(p);
        for (MTL::ArgumentEncoder*& e : matEncoder_) Release(e);
        for (MTL::RenderPipelineState*& p : pipes_) Release(p);
        for (auto& f : functions_) f.second->release();
        for (MTL::Library*& l : runtimeLibraries_) Release(l);
        Release(defaultLibrary_);
        for (Frame& f : frames_) { Release(f.transient); Release(f.light); Release(f.post); Release(f.motion); }
        for (MTL::Texture*& t : post_) Release(t);
        for (MTL::Texture** t : {&waterRefract_, &depth_, &depthCopy_, &overlayDepth_, &shadow_, &sunCache_, &tightShadow_, &captureDepth_, &captureVelocity_, &cloudNoise_, &cloudField_, &dummy_})
            Release(*t);
        Release(captureBuffer_);
        for (MTL::SamplerState** s : {&clampSampler_, &wrapSampler_, &shadowSampler_, &materialSampler_, &storeSampler_}) Release(*s);
        Release(backdropMip_);
        Release(backdropSmall_);
        for (MTL::DepthStencilState** s : {&dsScene_, &dsEqual_, &dsWater_, &dsOff_, &dsShadow_}) Release(*s);
        Release(queue_);
        Release(device_);
        Release(layer_);
        if (inflight_) { dispatch_release(inflight_); inflight_ = nullptr; }
    }

    // ------------------------------------------------------------------ state
    CA::MetalLayer* layer_ = nullptr;
    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::Library* defaultLibrary_ = nullptr;
    std::vector<MTL::Library*> runtimeLibraries_;
    std::unordered_map<std::string, MTL::Function*> functions_;
    std::vector<MTL::RenderPipelineState*> pipes_; // every render pipeline, for shutdown
    dispatch_semaphore_t inflight_ = nullptr;

    Frame frames_[kFramesInFlight];
    Frame* frame_ = nullptr;
    uint32_t frameSlot_ = kFramesInFlight - 1;
    NS::AutoreleasePool* framePool_ = nullptr;
    MTL::CommandBuffer* cmd_ = nullptr;          // this frame's, autoreleased in framePool_
    MTL::CommandBuffer* lastCommitted_ = nullptr; // retained, for WaitIdle
    CA::MetalDrawable* drawable_ = nullptr;
    MTL::RenderCommandEncoder* rce_ = nullptr;
    MTL::ComputeCommandEncoder* cce_ = nullptr;
    MTL::BlitCommandEncoder* bce_ = nullptr;
    Pass pass_ = Pass::None;
    MTL::RenderPipelineState* boundPipe_ = nullptr;
    uint64_t boundMatKey_ = ~0ull;
    int boundMatLayout_ = -1;
    uint64_t frameSerial_ = 0;
    std::atomic<uint64_t> completedSerial_{0};
    std::atomic<uint64_t> gpuMicros_{0}, gpuSamples_{0};
    uint32_t timerFrames_ = 0;

    uint32_t w_ = 1, h_ = 1, sceneW_ = 1, sceneH_ = 1;
    float scale_ = 1.0f;
    float clear_[4] = {};
    bool frameActive_ = false, resolved_ = false, sceneStarted_ = false, historyValid_ = false, cloudMapValid_ = false;
    uint32_t postFrame_ = 0, probeAge_ = 0;
    FrameView view_;
    size_t used_ = 0;
    int prepass_ = 0, vegetationPass_ = 0;

    MTL::DepthStencilState *dsScene_ = nullptr, *dsEqual_ = nullptr, *dsWater_ = nullptr, *dsOff_ = nullptr, *dsShadow_ = nullptr;
    MTL::SamplerState *clampSampler_ = nullptr, *wrapSampler_ = nullptr, *shadowSampler_ = nullptr, *materialSampler_ = nullptr, *storeSampler_ = nullptr;
    MTL::RenderPipelineState *scenePipe_ = nullptr, *overlayPipe_ = nullptr, *instancedPipe_ = nullptr, *prepassDepthPipe_[2] = {};
    MTL::RenderPipelineState *vegPipe_ = nullptr, *vegDepthPipe_ = nullptr, *vegShadePipe_ = nullptr, *skinnedPipe_ = nullptr;
    MTL::RenderPipelineState *shadowPipe_ = nullptr, *shadowInstPipe_ = nullptr, *vegShadowPipe_ = nullptr, *shadowCutPipe_ = nullptr;
    MTL::RenderPipelineState *postPipes_[11] = {}, *uiPipe_ = nullptr, *waterPipe_ = nullptr;

    MTL::Texture *depth_ = nullptr, *depthCopy_ = nullptr, *overlayDepth_ = nullptr;
    MTL::Texture *shadow_ = nullptr, *sunCache_ = nullptr, *tightShadow_ = nullptr;
    MTL::Texture *captureDepth_ = nullptr, *captureVelocity_ = nullptr, *dummy_ = nullptr;
    MTL::Texture *post_[kPostTargets] = {}, *waterRefract_ = nullptr, *cloudNoise_ = nullptr, *cloudField_ = nullptr;
    ShadowMap shadowTarget_ = ShadowMap::Sun;
    Mat4 lightVP_;

    std::vector<Mesh> meshes_;
    std::vector<MeshId> freeSlots_, pendingDeformables_;
    std::vector<MTL::Buffer*> instanceBuffers_;
    std::vector<InstanceBufferId> freeInstanceSlots_;
    std::vector<Texture> textures_;
    std::vector<TextureId> freeTextures_, extraOf_;
    TextureId defaults_[4] = {kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    TextureId curMat_[3] = {kNoTexture, kNoTexture, kNoTexture};
    TextureId uiAtlas_ = kNoTexture, uiImages_ = kNoTexture;
    MTL::Texture *backdropMip_ = nullptr, *backdropSmall_ = nullptr; // CaptureUiBackdrop
    std::unordered_map<uint64_t, Material> materials_;
    MTL::ArgumentEncoder* matEncoder_[2] = {};
    NS::UInteger matStride_ = 256, matNext_ = 0;
    std::vector<MTL::Buffer*> matPages_;
    std::vector<int32_t> matFree_;
    std::vector<GpuBuffer> gpuBuffers_;
    std::vector<Compute> computes_;
    std::vector<Retired> retired_;

    WaterDraw water_;
    std::vector<MeshId> waterMeshes_;
    bool waterPending_ = false;

    bool capture_ = false, captureNew_ = false;
    MTL::Buffer* captureBuffer_ = nullptr;
    std::vector<uint8_t> captureRgba_;
    int captureW_ = 0, captureH_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateMetalRenderer() { return std::make_unique<MetalRenderer>(); }

} // namespace tbd
