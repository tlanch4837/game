// Graphics API abstraction. One interface, two backends (Direct3D 12 and Vulkan) that draw the
// same thing the same way. Deliberately small: unindexed vertex meshes, one shader, four pipeline
// states (triangles or lines, with or without depth test). The backends wait for the GPU at the end
// of every frame (one frame in flight): simple first, pipelined later.
#pragma once
#include <atomic>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <chrono>
// Apple (Metal backend): the "window" handed to Init is a CAMetalLayer* (as void*); see renderer_metal.mm.
using HWND = void*;
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../vecmath.h"
#include "../floating_origin.h"
#include "vertex.h"    // struct Vertex (its own header so platform-free code can build vertex lists)
#include "ui_vertex.h" // struct UiVertex, the smooth UI path's vertex (DrawUi)

namespace tbd {

enum class Topology { Triangles, Lines };

constexpr uint32_t kShadowMapSize = 4096;      // the sun's shadow map is square
constexpr uint32_t kTightShadowMapSize = 2048; // so is the tight character map, which covers only a few meters

// Which shadow map a shadow pass renders into: the wide sun map, or the tight one around the camera's focus.
enum class ShadowMap { Sun, Tight };

// The scene's light rig, uploaded once per frame; mirrors LightingData in shaders/lighting.hlsli. Colors are
// already multiplied by their intensity. The defaults are the engine's original fixed light, unshadowed.
struct Lighting {
    float sunDir[4] = {0.4397f, 0.8305f, 0.3420f, 0.0f};      // xyz: unit vector toward the sun
    float sunColor[4] = {0.85f, 0.816f, 0.748f, 0.0f};        // rgb; zero when the sun is down
    float skyColor[4] = {0.378f, 0.468f, 0.612f, 0.0f};       // rgb: ambient light from straight up
    float groundColor[4] = {0.234f, 0.198f, 0.162f, 0.0f};    // rgb: ambient light from straight down
    float shadowVP[16] = {};                                  // world -> the sun's shadow map clip space
    float shadowParams[4] = {0.0f, 1.0f / kShadowMapSize, 0.0f, 0.0f}; // x: metres the depth spans, y: texel in uv, z: normal offset (world), w: on
    float eye[4] = {};                                        // xyz: camera position, for specular
    float tightVP[16] = {};                                   // world -> the tight character map's clip space
    float tightParams[4] = {0.0f, 1.0f / kTightShadowMapSize, 0.0f, 0.0f}; // x: metres the depth spans, y: texel in uv, z: normal offset (world), w: on
    float skySh[4][4] = {};    // sky and ground radiance, first order: mean rgb, then its rgb gradient along x, y, z
    float skyGrad[3][4] = {};  // rgb radiance for reflections: zenith, horizon, ground
    float probeOrigin[4] = {}; // irradiance probes: xyz grid cell of the lowest corner, w spacing across
    float probeGrid[4] = {};   // cells x, y, z; w spacing up
    float probeUpdate[4] = {}; // first probe this frame, probes per frame, rays per probe, on
    float probePrev[4] = {};   // last frame's probeOrigin
    float lightDebug[4] = {};  // x: which lighting component to view (TBD_LIGHTDEBUG); 0 = the final image
    float cloudMap[4] = {};    // cloud shadow map: centre x, z, half extent (m), on
    float captureVP[16] = {};  // world -> the lit capture from the sun (BeginCapture); element 15 is 0 until one exists
};
static_assert(sizeof(Lighting) == 512, "backends and lighting.hlsli hard-code this layout");

// The irradiance probe grid (DDGI-style, see post.hlsl): cells around the camera's focus, updated a slice at a time.
constexpr uint32_t kProbeX = 24, kProbeY = 8, kProbeZ = 24;
constexpr uint32_t kProbeRays = 16, kProbesPerFrame = 192;
constexpr float kProbeSpacing = 4.0f, kProbeSpacingY = 3.0f;
constexpr uint32_t kProbeCount = kProbeX * kProbeY * kProbeZ; // in each of the two cascades; the second is four times as wide (probe.hlsli derives its origin)
constexpr uint32_t kProbeCascades = 2, kProbeTotal = kProbeCount * kProbeCascades, kProbeAtlasH = kProbeZ * kProbeY * kProbeCascades;
constexpr uint32_t kCaptureSize = 512; // the lit capture from the sun that probe rays read: fits inside the scene's smallest depth buffer
constexpr uint32_t kCloudShadowSize = 256;
constexpr float kCloudShadowExtent = 15000.0f; // half width in metres

// The lighting fields the post passes read; mirrors the tail of PostParams in post.hlsl.
struct PostLightBlock {
    float shadowVP[16], sunColor[4], skySh[4][4], skyGrad[3][4], probeOrigin[4], probeGrid[4], probeUpdate[4], probePrev[4], lightDebug[4], cloudMap[4], captureVP[16];
};
static_assert(sizeof(PostLightBlock) == 352, "post.hlsl hard-codes this layout");
// `age`: frames since the probe atlas was made. lightDebug y: atlas just made, z: first pass over all probes, w: sun map valid.
inline void FillPostLight(const Lighting& l, uint32_t age, PostLightBlock& b) {
    std::copy(l.shadowVP, l.shadowVP + 16, b.shadowVP);
    std::copy(l.sunColor, l.sunColor + 4, b.sunColor);
    std::copy(&l.skySh[0][0], &l.skySh[0][0] + 16, &b.skySh[0][0]);
    std::copy(&l.skyGrad[0][0], &l.skyGrad[0][0] + 12, &b.skyGrad[0][0]);
    std::copy(l.probeOrigin, l.probeOrigin + 4, b.probeOrigin);
    std::copy(l.probeGrid, l.probeGrid + 4, b.probeGrid);
    std::copy(l.probeUpdate, l.probeUpdate + 4, b.probeUpdate);
    std::copy(l.probePrev, l.probePrev + 4, b.probePrev);
    std::copy(l.lightDebug, l.lightDebug + 4, b.lightDebug);
    std::copy(l.cloudMap, l.cloudMap + 4, b.cloudMap);
    std::copy(l.captureVP, l.captureVP + 16, b.captureVP);
    b.probeUpdate[0] = (float)(((uint64_t)age * kProbesPerFrame) % kProbeTotal);
    b.lightDebug[1] = age == 0 ? 1.0f : 0.0f;
    b.lightDebug[2] = (uint64_t)age * kProbesPerFrame < kProbeTotal ? 1.0f : 0.0f;
    b.lightDebug[3] = l.shadowParams[3];
}

// A window pane the probe rays can leave a shaded room through: world-space centre and the rectangle's two half-extent vectors.
struct WindowPortal {
    float center[3], u[3], v[3];
};
constexpr uint32_t kMaxPortals = 24;
// The inside of a building, as a box the probe rays inside it trace exactly: world-space centre, half height, and the two
// horizontal half-extent vectors (window_portals.h finds it from the building's own floor, ceiling and inner wall faces).
struct InteriorRoom {
    float center[3], halfHeight, x[3], z[3];
};
constexpr uint32_t kMaxRooms = 6;
// Tail of PostParams in post.hlsl: x of count is the number of panes, y the number of rooms; three float4 per pane (centre, u, v),
// then three per room (centre and half height, x, z).
struct PortalBlock {
    float count[4];
    float data[kMaxPortals * 3][4];
    float rooms[kMaxRooms * 3][4];
};
static_assert(sizeof(PortalBlock) == 16 + kMaxPortals * 48 + kMaxRooms * 48, "post.hlsl hard-codes this layout");
constexpr size_t kLightBufferSize = sizeof(Lighting) + sizeof(PortalBlock); // the mesh passes' constants: Lighting, then the panes (LightingData in lighting.hlsli)
inline void FillPortals(const WindowPortal* p, uint32_t n, PortalBlock& b) {
    n = std::min(n, kMaxPortals);
    std::fill(&b.data[0][0], &b.data[0][0] + sizeof(b.data) / sizeof(float), 0.0f);
    b.count[0] = (float)n;
    for (uint32_t k = 0; k < n; ++k) {
        for (int c = 0; c < 3; ++c) { b.data[k * 3][c] = p[k].center[c]; b.data[k * 3 + 1][c] = p[k].u[c]; b.data[k * 3 + 2][c] = p[k].v[c]; }
    }
}
inline void FillRooms(const InteriorRoom* r, uint32_t n, PortalBlock& b) {
    n = std::min(n, kMaxRooms);
    std::fill(&b.rooms[0][0], &b.rooms[0][0] + sizeof(b.rooms) / sizeof(float), 0.0f);
    b.count[1] = (float)n;
    for (uint32_t k = 0; k < n; ++k) {
        for (int c = 0; c < 3; ++c) { b.rooms[k * 3][c] = r[k].center[c]; b.rooms[k * 3 + 1][c] = r[k].x[c]; b.rooms[k * 3 + 2][c] = r[k].z[c]; }
        b.rooms[k * 3][3] = r[k].halfHeight;
    }
}

// What the post pipeline needs to know about the camera.
struct FrameView {
    Mat4 viewProj;     // unjittered
    Mat4 invJittered;  // inverse of the matrix the scene is actually drawn with
    Vec3 eye;
    float time = 0.0f; // seconds, moves the clouds
    bool sky = true;   // physical sky, clouds and haze
    float dt = 0.0f;   // seconds since last frame, for what animates in a vertex shader
    bool motionVectors = true; // TAA follows moving things by their own motion
};

using MeshId = uint32_t;
constexpr MeshId kInvalidMesh = 0xFFFFFFFFu;

using TextureId = uint32_t;
constexpr TextureId kNoTexture = 0xFFFFFFFFu; // "use the default": white albedo, flat normal

using InstanceBufferId = uint32_t;
constexpr InstanceBufferId kNoInstances = 0xFFFFFFFFu;

using GpuBufferId = uint32_t;
constexpr GpuBufferId kNoGpuBuffer = 0xFFFFFFFFu;
using ComputeShaderId = uint32_t;
constexpr ComputeShaderId kNoComputeShader = 0xFFFFFFFFu;
enum class GpuBufferUse { Read, Instances, Args, ShaderData }; // ShaderData: written by compute, read by vertex and pixel shaders

// What ResolveScene needs to draw water, right after the sky and haze are composed; see water.h and shaders/water.hlsl.
struct WaterDraw {
    const MeshId* meshes = nullptr;
    uint32_t meshCount = 0;
    uint32_t nearCount = 0; // the first nearCount meshes are the dense one(s) around the camera
    GpuBufferId waves = kNoGpuBuffer, ripple = kNoGpuBuffer, sky = kNoGpuBuffer;
    Mat4 viewProj;   // jittered, as the scene was drawn
    float ripplePlace[4] = {}, nearPlace[4] = {}, tiles[4] = {}, wind[4] = {}; // wind: the frame's jitter in clip space x, y; displacement scale; debug
};

// What the scene drew this frame (draws flagged `counted` only; editor overlays are not counted).
struct FrameCounts {
    uint32_t draws = 0;
    uint64_t triangles = 0;
    uint32_t shadowDraws = 0;     // every shadow map's casters, instanced ones counted once per draw
    uint64_t shadowTriangles = 0;
    uint32_t materialBinds = 0;   // SetMaterial calls that reached the GPU
};

// Render scale: the scene and post chain draw at window * scale and the tone map upscales to the window. One clamp and one
// rounding rule, shared so both backends and main.cpp's TAA jitter agree on the internal size.
inline float ClampRenderScale(float scale) { return scale != scale ? 1.0f : std::min(1.0f, std::max(0.5f, scale)); }
inline int ScaledSize(int window, float scale) { return std::max(1, (int)std::lround((double)window * scale)); }

// One static mesh placed in the hardware ray tracing scene: a float3x4 model matrix row by row, as instance buffers hold it.
struct RayInstance {
    MeshId mesh;
    float rows[12];
};
constexpr uint32_t kMaxRayInstances = 16384;

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual const char* Name() const = 0;
    // `window` becomes the render target; it must not be used by another graphics API.
    virtual bool Init(HWND window, int width, int height, bool validation) = 0;
    virtual void Resize(int width, int height) = 0;
    // Internal resolution as a fraction of the window (clamped 0.5..1.0, see ScaledSize). Call outside BeginFrame..EndFrame;
    // before Init it is only remembered. Targets are made again when it changes.
    virtual void SetRenderScale(float scale) = 0;

    // Uploads vertices and returns a handle. The mesh lives until the renderer is destroyed.
    // Meshes live welded and indexed in GPU memory. `dynamic` is for geometry replaced often (a live face rig): it
    // skips welding and the wait for the copy, and stays in memory the CPU can write.
    virtual MeshId CreateMesh(const Vertex* vertices, uint32_t count, Topology topology, bool dynamic = false) = 0;
    // A mesh the CPU rewrites often (skinned characters): indexed, drawn from device-local memory, starting as `vertices`.
    // It holds its newest vertices and the ones before them, which motion vectors read. A frame that doesn't ask for its
    // vertices keeps both and costs nothing; ask once more after the last change, so the two agree and motion ends.
    virtual MeshId CreateDeformableMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) = 0;
    // A draw of part of a deformable mesh's index range, sharing its vertices. Destroy it before its parent.
    virtual MeshId CreateMeshPart(MeshId parent, uint32_t firstIndex, uint32_t indexCount) = 0;
    // Staging memory for this frame's new vertices of a deformable mesh, write-only (uncached), to be filled whole before
    // EndFrame; nullptr when no frame is open or the mesh is not deformable. Call on the render thread, between BeginFrame
    // and CommitDeformables; any thread may write through the pointer.
    virtual Vertex* DeformableVertices(MeshId mesh) = 0;
    // Records the copies of what DeformableVertices handed out this frame. Before the frame's first draw.
    virtual void CommitDeformables() = 0;
    // Frees a mesh (streamed terrain chunks come and go). Safe between frames; the id may be reused.
    virtual void DestroyMesh(MeshId mesh) = 0;

    // Per-instance model matrices in device-local memory: `count` rows of 12 floats, a float3x4 row by row (x axis, y axis,
    // z axis and translation are the columns). Waits for the copy; call outside BeginFrame..EndFrame.
    virtual InstanceBufferId CreateInstanceBuffer(const float* rows, uint32_t count) = 0;
    virtual void DestroyInstanceBuffer(InstanceBufferId buffer) = 0;

    // Material textures. `rgba` is
    // width*height*4 bytes, top row first; mip levels are generated. Call outside BeginFrame..EndFrame.
    virtual TextureId CreateTexture(const uint8_t* rgba, int width, int height) = 0;
    virtual void DestroyTexture(TextureId texture) = 0;
    // The albedo (multiplies the vertex color; alpha below 0.4 is cut out) and tangent-space normal map used by the
    // lit draws that follow, until changed. kNoTexture = default. Reset to (none, none) after drawing a textured mesh.
    // `surface` packs per-pixel skin data: R roughness (0.5 = neutral), G cavity (1 = open skin), B scatter amount.
    virtual void SetMaterial(TextureId albedo, TextureId normal, TextureId surface = kNoTexture) = 0;
    // Links a surface texture to its extra material map, bound with it by SetMaterial: R baked occlusion (1 = open), G reserved (1),
    // B metalness scale (1 = the material's own factor), A emissive strength (0 = none). kNoTexture unlinks; the default texel is
    // (255, 255, 255, 0). The link ends when either texture is destroyed. Call outside BeginFrame..EndFrame.
    virtual void SetSurfaceExtra(TextureId surface, TextureId extra) { (void)surface; (void)extra; }
    // Skin's pore normal (the normal map of a skin material, mesh.hlsl's skin branch) repeats this many times across the UV
    // atlas. Per material, keyed by its albedo texture: SetMaterial with that albedo uses it. Default kDefaultSkinDetailScale
    // (48, the old fixed value); 1 samples the normal map untiled. A scale <= 0 resets to the default. Forgotten when the
    // texture is destroyed. Any time, both backends.
    static constexpr float kDefaultSkinDetailScale = 48.0f;
    void SetSkinDetailScale(TextureId albedo, float scale) {
        if (albedo == kNoTexture) return;
        if (scale > 0.0f && scale != kDefaultSkinDetailScale) skinDetailScale_[albedo] = scale;
        else skinDetailScale_.erase(albedo);
    }
    float SkinDetailScale(TextureId albedo) const {
        const auto it = skinDetailScale_.find(albedo);
        return it == skinDetailScale_.end() ? kDefaultSkinDetailScale : it->second;
    }
    // How deep skin's pores read (the pit darkening and the normal tilt of mesh.hlsl's skin branch), per material keyed by its
    // albedo like SetSkinDetailScale: 1 = the default, 0.5 young and fine, 2 old and coarse; clamped 0..3. A depth < 0 resets.
    // Sent as materialConstants_.y (0 there means the default, so materials never set read as 1). Any time, both backends.
    void SetSkinPoreDepth(TextureId albedo, float depth) {
        if (albedo == kNoTexture) return;
        if (depth >= 0.0f && depth != 1.0f) skinPoreDepth_[albedo] = depth > 3.0f ? 3.0f : depth;
        else skinPoreDepth_.erase(albedo);
    }
    float SkinPoreDepthConstant(TextureId albedo) const { // the value for the shader: 0 = default
        const auto it = skinPoreDepth_.find(albedo);
        return it == skinPoreDepth_.end() ? 0.0f : (it->second > 0.0f ? it->second : 1e-4f);
    }

    // World streaming: uploads that never wait for the GPU, and frees that wait until no frame in flight can use the resource.
    // The copy is queued ahead of the next frame, so the result can be drawn at once. Call outside BeginFrame..EndFrame.
    virtual MeshId CreateMeshNoWait(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) = 0;
    virtual InstanceBufferId CreateInstanceBufferNoWait(const float* rows, uint32_t count) = 0;
    // A full mip chain made ahead of time; `dims[i]` is level i's size, each level width*height*4 bytes.
    virtual TextureId CreateTextureNoWait(const std::vector<std::vector<uint8_t>>& levels, const std::vector<std::pair<int, int>>& dims) = 0;
    virtual void RetireMesh(MeshId mesh) = 0;
    virtual void RetireTexture(TextureId texture) = 0;
    // The GPU's own memory in bytes; 0 when unknown.
    virtual uint64_t LocalMemoryBytes() const = 0;

    // ------------------------------------------------------------------ compute (GPU-driven placement)
    // A device-local buffer. Instances may also be a vertex stream; Args holds indirect draws (five uints per draw: index count,
    // instance count, first index, base vertex, first instance); both are writable by compute. Read is only ever read by it.
    virtual GpuBufferId CreateGpuBuffer(GpuBufferUse use, uint64_t bytes, const void* initial) = 0;
    virtual void DestroyGpuBuffer(GpuBufferId buffer) = 0;
    // An entry point of shaders/vegetation.hlsl; on Vulkan its SPIR-V is shaders/vegetation_<entry>.spv.
    virtual ComputeShaderId CreateComputeShader(const char* entry) = 0;
    // An entry point of shaders/<file>.hlsl (D3D12: compiled with WATER_COMPUTE defined for "water"); on Vulkan shaders/<file>_<entry>.spv.
    virtual ComputeShaderId CreateComputeShaderIn(const char* file, const char* entry) = 0;
    // Before ResolveScene: draws this water in it, this frame only. The arrays must live until then. False if its pipeline failed.
    virtual bool SetWaterDraw(const WaterDraw& draw) = 0;
    // Right after BeginFrame and before any shadow pass: every writable buffer stays writable until EndCompute.
    virtual void BeginCompute() = 0;
    // Four read buffers (t0-t3) and three written ones (u0-u2); constants are at most 128 bytes. Dispatches run in order.
    virtual void Dispatch(ComputeShaderId shader, const void* constants, uint32_t constantBytes, const GpuBufferId reads[4],
                          const GpuBufferId writes[3], uint32_t x, uint32_t y, uint32_t z) = 0;
    virtual void EndCompute() = 0;
    // A welded mesh's index count, for indirect arguments; 0 when it has no indices.
    virtual uint32_t MeshIndexCount(MeshId mesh) const = 0;
    // One indirect draw of a mesh over instances, with the vegetation vertex shader; `params` reach it as pc.model.
    virtual void DrawVegetationIndirect(MeshId mesh, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) = 0;
    virtual void DrawVegetationShadowIndirect(MeshId mesh, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& params) = 0;
    // Which pipeline DrawVegetationIndirect uses: 0 opaque with alpha test, 1 alpha test into depth only, 2 shade against that depth.
    virtual void SetVegetationPass(int pass) = 0;
    // Debug: copies a buffer's start to the CPU, waiting for the GPU. Outside BeginFrame..EndFrame.
    virtual bool ReadGpuBuffer(GpuBufferId buffer, void* out, uint64_t bytes) = 0;

    virtual void BeginFrame(const float clearColor[4]) = 0;
    // Renders depth from the sun into a shadow map, using the shadowVP (Sun) or tightVP (Tight) of the lighting set before
    // BeginFrame. Call right after BeginFrame, before any other draw; casters are drawn in between. Casters are not counted.
    // Passes don't nest. When both maps are drawn, draw Tight first: the Sun pass's end closes the "shadow" GPU timer.
    // restoreSunCache: the Sun map starts as the depth last stored by StoreSunCache instead of cleared.
    virtual void BeginShadowPass(ShadowMap map, bool restoreSunCache = false) = 0;
    // Inside the Sun pass: keeps the depth drawn so far (the casters that don't move) under `key`, for later frames.
    virtual void StoreSunCache(uint64_t key) = 0;
    // Whether the stored depth is for `key`. Destroying a mesh forgets it, since a reused id may be other geometry.
    bool SunCacheHolds(uint64_t key) const { return key != 0 && sunCacheKey_ == key; }
    virtual void DrawShadowCaster(MeshId mesh, const Mat4& model) = 0;
    // A caster cut out by its albedo's alpha (hair cards): depth only where the texture has coverage, hashed so partial coverage
    // casts a partial shadow once the map's filter averages it. Binds `albedo` as the material.
    virtual void DrawShadowCasterCutout(MeshId mesh, const Mat4& model, TextureId albedo) = 0;
    // `count` instances of the mesh, from instance `first` of the buffer, into the current shadow pass.
    virtual void DrawShadowInstanced(MeshId mesh, InstanceBufferId buffer, uint32_t first, uint32_t count) = 0;
    virtual void EndShadowPass() = 0;
    // A small lit picture of the scene, drawn with Draw and matrices that reverse Z like the scene's, into a texture the probe rays read
    // (Dachsbacher and Stamminger's reflective shadow map, holding radiance). After the shadow passes, before the scene's draws.
    virtual void BeginCapture() = 0;
    virtual void EndCapture() = 0;
    uint32_t CaptureEpoch() const { return captureEpoch_; } // changes when the capture's texture was made anew and holds nothing
    // mvp = projection * view * model; model is used for lighting normals.
    virtual void Draw(MeshId mesh, const Mat4& mvp, const Mat4& model, bool depthTest, bool counted) = 0;
    // `count` instances of the mesh, from instance `first`, into the HDR scene with the current material. Always counted.
    virtual void DrawInstanced(MeshId mesh, InstanceBufferId buffer, uint32_t first, uint32_t count, const Mat4& viewProj) = 0;
    // Vertices valid for this call only (copied into a per-frame buffer).
    virtual void DrawTransient(const Vertex* vertices, uint32_t count, Topology topology, const Mat4& mvp,
                               const Mat4& model, bool depthTest) = 0;
    // The sky comes from ResolveScene; draw the scene with a sub-pixel jittered matrix for temporal anti-aliasing.
    // ResolveScene ends the HDR scene: draws after it are display-space overlays.
    // ---- smooth UI (src/gui, shaders/ui.hlsl, render/ui_vertex.h): its own pipeline with real alpha blending and no depth
    // test, for the in-game GUI. Rounded boxes, shadows and capsules by signed distance, text from an SDF atlas.
    // UiSupported() is false when the pipeline did not build (shader missing or failed): callers then use DrawTransient.
    virtual bool UiSupported() const { return false; }
    // The SDF font atlas (a texture from CreateTexture) the text mode samples. Set once after Init; kNoTexture = none.
    virtual void SetUiAtlas(TextureId atlas) { (void)atlas; }
    // Overlay phase only (after ResolveScene), like DrawTransient. ortho = OrthoPixels(width, height).
    virtual void DrawUi(const UiVertex* vertices, uint32_t count, const Mat4& ortho) { (void)vertices; (void)count; (void)ortho; }

    virtual void SetFrameView(const FrameView& view) = 0;
    virtual void ResolveScene() = 0;
    virtual void EndFrame() = 0; // submits, presents, waits
    // Debug: while on, EndFrame keeps a copy of the frame it presented (BGRA bytes).
    virtual void SetFrameCapture(bool on) = 0;
    // Rebuilds the shaders from source without a restart; false when unsupported or a shader failed to compile (the old ones stay).
    virtual bool ReloadShaders() { return false; }
    virtual bool CapturedFrame(std::vector<uint8_t>& rgba, int& width, int& height) = 0;

    // Call between frames when the world origin moves. Re-express the previous
    // unjittered projection in the new coordinates for TAA and motion vectors.
    void ShiftPreviousView(Vec3 offset) { prevViewProj_ = ShiftPreviousViewProjection(prevViewProj_, offset); }

    // Takes effect at the next BeginFrame and stays until changed.
    void SetLighting(const Lighting& lighting) { lighting_ = lighting; }
    // The panes the irradiance probes treat as openings this frame (at most kMaxPortals; none = every probe behaves as before).
    void SetWindowPortals(const WindowPortal* panes, uint32_t count) { FillPortals(panes, count, portals_); }
    // The building interiors whose probes trace their walls, floor and ceiling (at most kMaxRooms).
    void SetInteriorRooms(const InteriorRoom* rooms, uint32_t count) { FillRooms(rooms, count, portals_); }

    // Hardware ray tracing (DXR 1.1 ray queries on Direct3D 12, VK_KHR_ray_query on Vulkan): the ambient occlusion pass traces
    // real rays against the static meshes given here, so objects shade the light around them (a barrel the floor it stands on).
    virtual bool RayTracingSupported() const { return false; }
    // What the rays can hit next frame (at most kMaxRayInstances; triangle meshes made by CreateMesh / CreateMeshNoWait). Each
    // mesh gets its own acceleration structure the first time it is listed, a few a frame; until then it is left out.
    void SetRayScene(const RayInstance* instances, uint32_t count) { rayScene_.assign(instances, instances + std::min(count, kMaxRayInstances)); }

    // The scene's depth pre-pass, for Draw and DrawInstanced of lit triangles: 1 = depth only (no pixel shader, nothing counted),
    // 2 = shade what the pre-pass drew (depth equal, no depth write), 0 = as normal. Only parts that never cut out or discard
    // may be drawn in mode 1, with the same matrices as in mode 2. Skinned meshes ignore it (mode 1 skips them).
    virtual void SetScenePrepass(int mode) { (void)mode; }

    FrameCounts Counts() const { return counts_; }
    // Diagnostics (TBD_FRAMELOG): the newest finished frame's GPU passes in ms (shadow, scene, ao, clouds, composite, taa,
    // tonemap, overlays; that frame is kFramesInFlight old), and CPU ms of this frame's fence wait and Present.
    const double* GpuPasses() const { return gpuPasses_; }
    double FenceWaitMs() const { return fenceWaitMs_; }
    double PresentMs() const { return presentMs_; }

protected:
    Mat4 prevViewProj_ = Mat4::Identity();
    FrameCounts counts_, lastCounts_; // lastCounts_: the previous frame's, for the timer log
    std::unordered_map<TextureId, float> skinDetailScale_; // SetSkinDetailScale; only the ones off the default
    std::unordered_map<TextureId, float> skinPoreDepth_;   // SetSkinPoreDepth; only the ones off the default
    // The bound material's constants for the pixel shader (D3D12 root constants b3, Vulkan push constants at offset 128):
    // x skin detail scale, y skin pore depth (0 = default 1), zw spare. Set by SetMaterial, sent with every lit draw.
    float materialConstants_[4] = {kDefaultSkinDetailScale, 0.0f, 0.0f, 0.0f};
    void ForgetMaterialConstants(TextureId id) { skinDetailScale_.erase(id); skinPoreDepth_.erase(id); }
    uint64_t sunCacheKey_ = 0;
    // The line after each 60-frame GPU timer line: what the last frame drew.
    void LogCounts(const char* api) const; // log.cpp
    double gpuPasses_[8] = {};
    double fenceWaitMs_ = 0.0, presentMs_ = 0.0;
    static double TraceNowMs() {
#ifdef _WIN32
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }
    // Scoped step timer for the streaming upload calls: Mark() after each step; Done() logs the steps when the call was slow.
    struct UploadSteps {
        const char* name[10] = {};
        double at[10] = {};
        int n = 0;
        double start = TraceNowMs();
        void Mark(const char* step) { if (n < 10) { name[n] = step; at[n++] = TraceNowMs(); } }
        void Done(const char* api, const char* what, uint64_t bytes, double slowMs = 8.0);
    };
    Lighting lighting_;
    PortalBlock portals_ = {};
    uint32_t captureEpoch_ = 0;
    std::vector<RayInstance> rayScene_;
};

std::unique_ptr<Renderer> CreateD3D12Renderer();
// Fills the D3D12 shader cache on all cores; blocks. Run it off the main thread so the window stays alive.
void PrecompileD3D12Shaders(std::atomic<int>& done, std::atomic<int>& total, const std::atomic<bool>& stop, std::atomic<int>* failed = nullptr);
std::unique_ptr<Renderer> CreateVulkanRenderer();
// Apple only (renderer_metal.mm): Metal via metal-cpp. Init's window is a CAMetalLayer*.
std::unique_ptr<Renderer> CreateMetalRenderer();

// Appends to engine.log next to the executable (and the debugger output).
void Log(const char* format, ...);
// Crash reporting: crash_<time>.log (stack, registers, modules, last log lines) and crash_<time>.dmp beside the executable,
// for unhandled exceptions, std::terminate, abort, stack overflow and CRT errors. MarkCleanExit() tells the log the exit was normal.
void InstallCrashHandlers();
void MarkCleanExit();

// Folder holding the running executable (shaders live in <it>\shaders).
std::wstring ExeDirectory();

} // namespace tbd
