// Vulkan backend (1.3: dynamic rendering + synchronization2, so there are no render passes or framebuffers).
#include "mesh_weld.h"
#include "renderer.h"
#include "chunk_faces.h"

#ifndef TBD_HAVE_VULKAN

namespace tbd {
std::unique_ptr<Renderer> CreateVulkanRenderer() { return nullptr; } // built without the Vulkan SDK
} // namespace tbd

#else

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <vector>

#include "cloud_noise.h"
extern float g_brightness; // main.cpp: user brightness multiplier
extern float g_sceneExposure; // main.cpp: the scene's fixed exposure (Scene::exposure), 0 = auto

namespace tbd {
namespace {

constexpr uint32_t kFramesInFlight = 2;

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult res_ = (expr);                                                \
        if (res_ != VK_SUCCESS) {                                              \
            Log("Vulkan: %s failed (%d)", #expr, (int)res_);                   \
            return false;                                                      \
        }                                                                      \
    } while (0)

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr size_t kTransientBytes = 16 * 1024 * 1024;
constexpr float kShadowDepthBias = 2000.0f; // in units of the depth buffer's resolution (float: 2^-24 near z = 1)
constexpr float kShadowSlopeBias = 2.0f;
constexpr float kTightDepthBias = 1000.0f; // the tight map's depth range is meters, not hundreds of them, so far less bias
constexpr float kTightSlopeBias = 1.5f;

struct PushConstants {
    float mvp[16];
    float model[16];
};
static_assert(sizeof(PushConstants) == 128, "guaranteed push constant budget");

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) Log("Vulkan validation: %s", data->pMessage);
    return VK_FALSE;
}

bool ReadFile(const std::wstring& path, std::vector<uint32_t>& words) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    words.assign((size_t)(size + 3) / 4, 0);
    const size_t got = fread(words.data(), 1, (size_t)size, f);
    fclose(f);
    return got == (size_t)size && size > 0;
}

class VulkanRenderer final : public Renderer {
    struct Mesh; // below; the streaming helpers take one
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer transient, lightBuffer, postBuffer;
        VkDescriptorSet lightSet = VK_NULL_HANDLE;
        VkQueryPool timerPool = VK_NULL_HANDLE;
        bool timerPending = false;
        uint64_t serial = 0; // which submission its fence covers (frameSerial_ at its EndFrame)
    };
public:
    ~VulkanRenderer() override { Shutdown(); }

    const char* Name() const override { return "VULKAN"; }

    bool Init(void* nativeWindow, int width, int height, bool validation) override {
        const HWND window = (HWND)nativeWindow;
        w_ = (uint32_t)std::max(1, width);
        h_ = (uint32_t)std::max(1, height);
        if (!CreateInstance(validation)) return false;

        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = window;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_));

        if (!PickDevice() || !CreateDevice()) return false;
        if (!CreateSwapchain()) return false;
        if (!CreateFrameObjects()) return false;
        if (!CreateBuffer(kStreamRingBytes, streamRing_, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
        StartSpareBlock();
        if (!CreatePipelines()) return false;
        for (Frame& frame : frames_)
            if (!CreateBuffer(kTransientBytes, frame.transient, true)) return false;
        if (!CreateMaterialResources() || !CreateCloudNoise() || !CreateCloudField() || !CreatePostPipelines() || !CreatePostTargets() || !CreateTimers()) return false;
        if (!CreateUiPipeline()) { // optional: the in-game GUI then draws the classic way
            Log("Vulkan: UI pipeline unavailable (ui_vs.spv / ui_ps.spv missing or failed); the GUI draws the classic way");
            DestroyUiPipeline();
        }
        if (GetEnvironmentVariableA("TBD_TEXTURE_SELFTEST", nullptr, 0) > 0)
            Log("Vulkan: texture self-test %s", SelfTestTextures() ? "passed" : "FAILED");
        return true;
    }

    void Resize(int width, int height) override {
        if (!device_ || width <= 0 || height <= 0) return;
        if ((uint32_t)width == w_ && (uint32_t)height == h_) return;
        w_ = (uint32_t)width;
        h_ = (uint32_t)height;
        vkDeviceWaitIdle(device_);
        if (CreateSwapchain()) CreatePostTargets();
    }

    void SetRenderScale(float scale) override {
        scale = ClampRenderScale(scale);
        if (scale == scale_) return;
        scale_ = scale;
        if (!swapchain_) return; // Init makes the targets
        vkDeviceWaitIdle(device_);
        sceneExtent_ = {(uint32_t)ScaledSize((int)extent_.width, scale_), (uint32_t)ScaledSize((int)extent_.height, scale_)};
        if (CreateDepth()) CreatePostTargets();
    }

    MeshId CreateMesh(const Vertex* vertices, uint32_t count, Topology topology, bool dynamic) override {
        Buffer b;
        VkDeviceSize indexOffset = 0;
        bool indexed = false;
        if (dynamic) {
            const size_t bytes = (size_t)count * sizeof(Vertex);
            if (!CreateBuffer(bytes, b, true)) return kInvalidMesh;
            std::memcpy(b.mapped, vertices, bytes);
        } else {
            std::vector<Vertex> welded;
            std::vector<uint32_t> indices;
            WeldVertices(vertices, count, welded, indices);
            const size_t vertexBytes = welded.size() * sizeof(Vertex), indexBytes = indices.size() * sizeof(uint32_t);
            Buffer staging;
            if (!CreateBuffer(vertexBytes + indexBytes, staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return kInvalidMesh;
            std::memcpy(staging.mapped, welded.data(), vertexBytes);
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + vertexBytes, indices.data(), indexBytes);
            if (!CreateDeviceBuffer(vertexBytes + indexBytes, b, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
                DestroyBuffer(staging);
                return kInvalidMesh;
            }
            const bool ok = Immediate([&] {
                const VkBufferCopy region = {0, 0, vertexBytes + indexBytes};
                vkCmdCopyBuffer(cmd_, staging.buffer, b.buffer, 1, &region);
                VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
                vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            });
            DestroyBuffer(staging);
            if (!ok) {
                DestroyBuffer(b);
                return kInvalidMesh;
            }
            indexOffset = vertexBytes;
            indexed = true;
        }
        return PlaceMesh(Mesh{b, count, topology, indexOffset, indexed});
    }

    MeshId CreateDeformableMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        const size_t copyBytes = (size_t)vertexCount * sizeof(Vertex), indexBytes = (size_t)indexCount * sizeof(uint32_t);
        // Staging: one copy of the vertices per frame in flight, staying mapped, and the indices once for the first copy.
        Buffer upload, b;
        if (!CreateBuffer(copyBytes * kFramesInFlight + indexBytes, upload, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return kInvalidMesh;
        uint8_t* mapped = static_cast<uint8_t*>(upload.mapped);
        std::memcpy(mapped, vertices, copyBytes);
        std::memcpy(mapped + copyBytes * kFramesInFlight, indices, indexBytes);
        if (!CreateDeviceBuffer(copyBytes * 2 + indexBytes, b, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            DestroyBuffer(upload);
            return kInvalidMesh;
        }
        const bool ok = Immediate([&] {
            const VkBufferCopy regions[3] = {{0, 0, copyBytes}, {0, copyBytes, copyBytes}, {copyBytes * kFramesInFlight, copyBytes * 2, indexBytes}};
            vkCmdCopyBuffer(cmd_, upload.buffer, b.buffer, 3, regions);
            VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
        if (!ok) {
            DestroyBuffer(upload);
            DestroyBuffer(b);
            return kInvalidMesh;
        }
        Mesh mesh = {b, indexCount, Topology::Triangles, (VkDeviceSize)copyBytes * 2, true};
        mesh.copyBytes = copyBytes;
        mesh.upload = upload;
        return PlaceMesh(mesh);
    }

    MeshId CreateMeshPart(MeshId parent, uint32_t firstIndex, uint32_t indexCount) override {
        if (parent >= meshes_.size() || !meshes_[parent].buffer.buffer || !meshes_[parent].copyBytes) return kInvalidMesh;
        Mesh mesh = meshes_[parent];
        mesh.count = indexCount;
        mesh.indexOffset += (VkDeviceSize)firstIndex * sizeof(uint32_t);
        mesh.sharesBuffer = true;
        mesh.copyParent = parent;
        return PlaceMesh(mesh);
    }

    Vertex* DeformableVertices(MeshId id) override {
        if (!frameActive_ || id >= meshes_.size() || !meshes_[id].copyBytes || meshes_[id].sharesBuffer) return nullptr;
        pendingDeformables_.push_back(id);
        return reinterpret_cast<Vertex*>(static_cast<uint8_t*>(meshes_[id].upload.mapped) + meshes_[id].copyBytes * frameSlot_);
    }

    void CommitDeformables() override {
        if (pendingDeformables_.empty()) return;
        if (frameActive_) {
            vkCmdEndRendering(cmd_); // the main pass has drawn nothing yet
            // The frame before may still be reading these vertices.
            VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            for (MeshId id : pendingDeformables_) {
                Mesh& mesh = meshes_[id];
                mesh.newestCopy ^= 1;
                const VkBufferCopy region = {mesh.copyBytes * frameSlot_, mesh.copyBytes * mesh.newestCopy, mesh.copyBytes};
                vkCmdCopyBuffer(cmd_, mesh.upload.buffer, mesh.buffer.buffer, 1, &region);
            }
            VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            BeginMainRendering();
        }
        pendingDeformables_.clear();
    }

    // The frame in flight may still read the buffer, so it is released after the next fence wait.
    void DestroyMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].buffer.buffer) return;
        sunCacheKey_ = 0;
        vkDeviceWaitIdle(device_);
        if (!meshes_[id].sharesBuffer) {
            DestroyBuffer(meshes_[id].buffer);
            if (meshes_[id].upload.buffer) DestroyBuffer(meshes_[id].upload);
        }
        FreeMeshMemory(meshes_[id]);
        meshes_[id] = {};
        freeSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ instance buffers
    InstanceBufferId CreateInstanceBuffer(const float* rows, uint32_t count) override {
        if (!device_ || !rows || count == 0) return kNoInstances;
        const size_t bytes = (size_t)count * kInstanceStride;
        Buffer staging, b;
        if (!CreateBuffer(bytes, staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return kNoInstances;
        std::memcpy(staging.mapped, rows, bytes);
        if (!CreateDeviceBuffer(bytes, b, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            DestroyBuffer(staging);
            return kNoInstances;
        }
        const bool ok = Immediate([&] {
            const VkBufferCopy region = {0, 0, bytes};
            vkCmdCopyBuffer(cmd_, staging.buffer, b.buffer, 1, &region);
            VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
        DestroyBuffer(staging);
        if (!ok) {
            DestroyBuffer(b);
            return kNoInstances;
        }
        if (!freeInstanceSlots_.empty()) {
            const InstanceBufferId id = freeInstanceSlots_.back();
            freeInstanceSlots_.pop_back();
            instanceBuffers_[id] = b;
            return id;
        }
        instanceBuffers_.push_back(b);
        return (InstanceBufferId)(instanceBuffers_.size() - 1);
    }

    void DestroyInstanceBuffer(InstanceBufferId id) override {
        if (id >= instanceBuffers_.size() || !instanceBuffers_[id].buffer) return;
        vkDeviceWaitIdle(device_);
        DestroyBuffer(instanceBuffers_[id]);
        instanceBuffers_[id] = Buffer{};
        freeInstanceSlots_.push_back(id);
    }

    // ------------------------------------------------------------------ material textures
    TextureId CreateTexture(const uint8_t* rgba, int w, int h) override {
        if (!device_ || !rgba || w <= 0 || h <= 0) return kNoTexture;
        const MipChain mips = BuildMipChain(rgba, w, h);
        const uint32_t levels = (uint32_t)mips.levels.size();
        Texture tex;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = levels;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!AllocateImage(ii, tex.image, tex.memory)) return kNoTexture;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tex.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &tex.view) != VK_SUCCESS) return kNoTexture;

        size_t total = 0;
        for (const auto& level : mips.levels) total += level.size();
        Buffer staging;
        if (!CreateBuffer(total, staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return kNoTexture;
        std::vector<VkBufferImageCopy> copies(levels);
        size_t offset = 0;
        for (uint32_t m = 0; m < levels; ++m) {
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips.levels[m].data(), mips.levels[m].size());
            copies[m].bufferOffset = offset;
            copies[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            copies[m].imageExtent = {(uint32_t)mips.dims[m].first, (uint32_t)mips.dims[m].second, 1};
            offset += mips.levels[m].size();
        }
        const bool ok = Immediate([&] {
            ImageBarrier(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, levels);
            vkCmdCopyBufferToImage(cmd_, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies.data());
            ImageBarrier(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, levels);
        });
        DestroyBuffer(staging);
        if (!ok) return kNoTexture;
        TextureId id;
        if (!freeTextures_.empty()) { id = freeTextures_.back(); freeTextures_.pop_back(); textures_[id] = tex; }
        else { id = (TextureId)textures_.size(); textures_.push_back(tex); }
        return id;
    }

    void DestroyTexture(TextureId id) override {
        ForgetMaterialConstants(id);
        if (id >= textures_.size() || !textures_[id].image) return;
        vkDeviceWaitIdle(device_); // a frame in flight may still sample it
        for (auto it = materialSets_.begin(); it != materialSets_.end();) {
            if ((TextureId)(it->first >> 42) == id || (TextureId)((it->first >> 21) & 0x1FFFFF) == id || (TextureId)(it->first & 0x1FFFFF) == id || ExtraOf((TextureId)(it->first & 0x1FFFFF)) == id) {
                vkFreeDescriptorSets(device_, materialPool_, 1, &it->second);
                it = materialSets_.erase(it);
            } else ++it;
        }
        DestroyTextureObjects(textures_[id]);
        textures_[id] = {};
        if (id < extraOf_.size()) extraOf_[id] = kNoTexture;
        freeTextures_.push_back(id);
    }

    TextureId ExtraOf(TextureId surface) const { return surface < extraOf_.size() ? extraOf_[surface] : kNoTexture; }

    void SetSurfaceExtra(TextureId surface, TextureId extra) override {
        if (surface >= textures_.size()) return;
        if (extraOf_.size() <= surface) extraOf_.resize(surface + 1, kNoTexture);
        if (extraOf_[surface] == extra) return;
        vkDeviceWaitIdle(device_); // a frame in flight may hold a set that names the old link
        for (auto it = materialSets_.begin(); it != materialSets_.end();) {
            if ((TextureId)(it->first & 0x1FFFFF) == surface) {
                vkFreeDescriptorSets(device_, materialPool_, 1, &it->second);
                it = materialSets_.erase(it);
            } else ++it;
        }
        extraOf_[surface] = extra;
    }

    void SetMaterial(TextureId albedo, TextureId normal, TextureId surface) override {
        if (!frameActive_) return;
        const VkDescriptorSet set = MaterialSet(albedo, normal, surface);
        if (set) { vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &set, 0, nullptr); ++counts_.materialBinds; }
        materialConstants_[0] = SkinDetailScale(albedo);
        materialConstants_[1] = SkinPoreDepthConstant(albedo);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_), materialConstants_);
    }

    void SetFrameView(const FrameView& view) override { view_ = view; }

    // AO, clouds, sky and haze, TAA, tone mapping; afterwards the swapchain image is current and keeps the scene's depth.
    void ResolveScene() override {
        if (!frameActive_ || resolved_) return;
        vkCmdEndRendering(cmd_);
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
        const uint32_t w = sceneExtent_.width, h = sceneExtent_.height;
        p.eye[3] = scale_ < 1.0f ? 1.0f : 0.0f;
        p.res[0] = (float)w; p.res[1] = (float)h; p.res[2] = 1.0f / (float)w; p.res[3] = 1.0f / (float)h;
        p.misc[0] = view_.time; p.misc[1] = (float)(postFrame_ & 1023); p.misc[2] = view_.sky ? 1.0f : view_.cityAir ? 0.5f : 0.0f;
        p.misc[3] = historyValid_ ? 1.0f : 0.0f;
        p.motion[0] = view_.motionVectors ? 1.0f : 0.0f; p.motion[1] = g_brightness; p.motion[2] = g_sceneExposure;
        p.motion[3] = (float)(view_.postTier + 1); // post.hlsl PostTier; 0 would read as unset
        std::memcpy(frame_->postBuffer.mapped, &p, sizeof(p));

        ColorToRead(post_[kScene].image);
        ColorToRead(post_[kVelocity].image);
        ImageBarrier(depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const uint32_t cur = postFrame_ & 1;
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSets_[cur * kFramesInFlight + frameSlot_], 0, nullptr);
        if (lighting_.cloudMap[3] > 0.5f) PostPass(kCloudShadow, postPipelines_[9], kCloudShadowSize, kCloudShadowSize);
        Mark(3);
        if (lighting_.probeUpdate[3] > 0.5f) {
            PostPass(kProbeRaysTarget, postPipelines_[5], kProbeRays, kProbesPerFrame);
            PostPass(kProbes, postPipelines_[6], kProbeX * 3, kProbeAtlasH);
            ++probeAge_;
        }
        Mark(4);
        if (!(view_.cityAir && view_.postTier == 0)) // Low: the city composite reads no occlusion
            PostPass(kAo, postPipelines_[0], std::max(1u, w / 2), std::max(1u, h / 2));
        Mark(5);
        if (view_.sky) {
            PostPass(kCloudMarch, postPipelines_[1], std::max(1u, w / 4), std::max(1u, h / 4));
            PostPass(cur ? kClouds1 : kClouds, postPipelines_[10], std::max(1u, w / 2), std::max(1u, h / 2));
        }
        Mark(6);
        PostPass(kComposed, postPipelines_[2], w, h);
        if (waterPending_) WaterPass(w, h, cur);
        Mark(7);
        PostPass(kHistory0 + cur, postPipelines_[3], w, h);
        PostPass(kExposure, postPipelines_[7], 1, 1);
        if (view_.postTier > 1) // Low and Low/Med: the tone map adds no bloom
            PostPass(kBloom, postPipelines_[8], std::max(1u, w / 4), std::max(1u, h / 4));
        Mark(8);

        BeginColorPass(views_[imageIndex_], VK_NULL_HANDLE, extent_.width, extent_.height);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelines_[4]);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRendering(cmd_);
        Mark(9);

        ImageBarrier(depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        resolved_ = true;
        if (overlayDepthView_) { // the scene's depth is smaller than the window: the overlays get a window-sized one, cleared here
            ImageBarrier(overlayDepthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            BeginColorPass(views_[imageIndex_], overlayDepthView_, extent_.width, extent_.height, true);
        } else {
            BeginColorPass(views_[imageIndex_], depthView_, extent_.width, extent_.height);
        }
        BindMeshSets();
        prevViewProj_ = view_.viewProj;
        historyValid_ = true;
        ++postFrame_;
    }

    void SetFrameCapture(bool on) override {
        capture_ = on;
        captureNew_ = false;
        if (!on && captureBuffer_.buffer) {
            vkDeviceWaitIdle(device_);
            for (Buffer& b : retired_) DestroyBuffer(b);
            retired_.clear();
            DestroyBuffer(captureBuffer_);
        }
    }

    bool CapturedFrame(std::vector<uint8_t>& rgba, int& width, int& height) override {
        if (!captureNew_) return false;
        captureNew_ = false;
        rgba.swap(captureRgba_);
        width = captureW_;
        height = captureH_;
        return true;
    }

    // ------------------------------------------------------------------ world streaming
    MeshId CreateMeshNoWait(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) override {
        if (!device_ || !vertices || !indices || !vertexCount || !indexCount) return kInvalidMesh;
        const size_t vertexBytes = (size_t)vertexCount * sizeof(Vertex), indexBytes = (size_t)indexCount * sizeof(uint32_t);
        UploadSteps steps;
        Mesh mesh{{}, indexCount, Topology::Triangles, 0, true};
        if (!MeshMemory(vertexBytes + indexBytes, mesh.block, mesh.base, mesh.blockBytes)) return kInvalidMesh;
        steps.Mark("allocate");
        mesh.buffer = meshBlocks_[mesh.block].buffer;
        mesh.sharesBuffer = true; // the block owns the buffer
        mesh.indexOffset = mesh.base + vertexBytes;
        if (!StreamCopy(vertices, vertexBytes, indices, indexBytes, mesh.buffer.buffer, mesh.base)) {
            FreeMeshMemory(mesh);
            return kInvalidMesh;
        }
        steps.Mark("stage+record");
        steps.Done("Vulkan", "mesh upload", vertexBytes + indexBytes);
        return PlaceMesh(mesh);
    }

    InstanceBufferId CreateInstanceBufferNoWait(const float* rows, uint32_t count) override {
        if (!device_ || !rows || count == 0) return kNoInstances;
        Buffer b;
        const size_t bytes = (size_t)count * kInstanceStride;
        if (!CreateDeviceBuffer(bytes, b, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return kNoInstances;
        if (!StreamCopy(rows, bytes, nullptr, 0, b.buffer, 0)) { DestroyBuffer(b); return kNoInstances; }
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
        const uint32_t count = (uint32_t)levels.size();
        Texture tex;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {(uint32_t)dims[0].first, (uint32_t)dims[0].second, 1};
        ii.mipLevels = count;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        UploadSteps steps;
        if (vkCreateImage(device_, &ii, nullptr, &tex.image) != VK_SUCCESS) return kNoTexture;
        if (!BindStreamedImage(tex)) { // too big for a block: memory of its own
            vkDestroyImage(device_, tex.image, nullptr);
            tex.image = VK_NULL_HANDLE;
            if (!AllocateImage(ii, tex.image, tex.memory)) { DestroyTextureObjects(tex); return kNoTexture; }
        }
        steps.Mark("image");
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tex.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, count, 0, 1};
        size_t total = 0;
        for (const auto& level : levels) total += (level.size() + 15) / 16 * 16;
        VkBuffer source = VK_NULL_HANDLE;
        VkDeviceSize base = 0;
        uint8_t* mapped = nullptr;
        if (vkCreateImageView(device_, &vi, nullptr, &tex.view) != VK_SUCCESS || !(mapped = StreamStaging(total, 16, source, base))) {
            DestroyTextureObjects(tex);
            return kNoTexture;
        }
        steps.Mark("view+stage");
        std::vector<VkBufferImageCopy> copies(count);
        size_t offset = 0;
        for (uint32_t m = 0; m < count; ++m) {
            std::memcpy(mapped + offset, levels[m].data(), levels[m].size());
            copies[m].bufferOffset = base + offset;
            copies[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            copies[m].imageExtent = {(uint32_t)dims[m].first, (uint32_t)dims[m].second, 1};
            offset += (levels[m].size() + 15) / 16 * 16;
        }
        steps.Mark("copy");
        const bool ok = StreamRecord([&] {
            ImageBarrier(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, count);
            vkCmdCopyBufferToImage(cmd_, source, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies.data());
            ImageBarrier(tex.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, count);
        });
        if (!ok) { DestroyTextureObjects(tex); return kNoTexture; }
        steps.Done("Vulkan", "texture upload", total);
        TextureId id;
        if (!freeTextures_.empty()) { id = freeTextures_.back(); freeTextures_.pop_back(); textures_[id] = tex; }
        else { id = (TextureId)textures_.size(); textures_.push_back(tex); }
        return id;
    }

    void RetireMesh(MeshId id) override {
        if (id >= meshes_.size() || !meshes_[id].buffer.buffer) return;
        StreamRetired r;
        r.frame = streamFrame_;
        r.mesh = id;
        streamRetired_.push_back(r);
    }

    void RetireTexture(TextureId id) override {
        ForgetMaterialConstants(id);
        if (id >= textures_.size() || !textures_[id].image) return;
        StreamRetired r;
        r.frame = streamFrame_;
        r.texture = id;
        streamRetired_.push_back(r);
    }

    uint64_t LocalMemoryBytes() const override {
        if (!phys_) return 0;
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(phys_, &props);
        uint64_t largest = 0;
        for (uint32_t i = 0; i < props.memoryHeapCount; ++i)
            if (props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) largest = std::max<uint64_t>(largest, props.memoryHeaps[i].size);
        return largest;
    }

    // ------------------------------------------------------------------ compute
    GpuBufferId CreateGpuBuffer(GpuBufferUse use, uint64_t bytes, const void* initial) override {
        if (!device_ || bytes == 0) return kNoGpuBuffer;
        GpuBuffer g;
        g.use = use;
        g.bytes = bytes;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (!CreateDeviceBuffer((size_t)bytes, g.buffer, usage)) return kNoGpuBuffer;
        if (initial) {
            Buffer staging;
            if (!CreateBuffer((size_t)bytes, staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return kNoGpuBuffer;
            std::memcpy(staging.mapped, initial, (size_t)bytes);
            const bool ok = Immediate([&] {
                const VkBufferCopy region = {0, 0, bytes};
                vkCmdCopyBuffer(cmd_, staging.buffer, g.buffer.buffer, 1, &region);
                VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            });
            DestroyBuffer(staging);
            if (!ok) { DestroyBuffer(g.buffer); return kNoGpuBuffer; }
        }
        for (size_t i = 0; i < gpuBuffers_.size(); ++i)
            if (!gpuBuffers_[i].buffer.buffer) { gpuBuffers_[i] = g; return (GpuBufferId)i; }
        gpuBuffers_.push_back(g);
        return (GpuBufferId)(gpuBuffers_.size() - 1);
    }

    void DestroyGpuBuffer(GpuBufferId id) override {
        if (id >= gpuBuffers_.size() || !gpuBuffers_[id].buffer.buffer) return;
        vkDeviceWaitIdle(device_);
        DestroyBuffer(gpuBuffers_[id].buffer);
        gpuBuffers_[id] = GpuBuffer{};
    }

    ComputeShaderId CreateComputeShader(const char* entry) override {
        if (!computeLayout_ && !CreateComputeLayout()) return kNoComputeShader;
        std::wstring file = L"vegetation_";
        for (const char* c = entry; *c; ++c) file += (wchar_t)*c;
        file += L".spv";
        VkShaderModule module = LoadShader(file.c_str());
        if (!module) return kNoComputeShader;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = entry;
        ci.layout = computeLayout_;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        vkDestroyShaderModule(device_, module, nullptr);
        if (made != VK_SUCCESS) return kNoComputeShader;
        computePipelines_.push_back(pipeline);
        return (ComputeShaderId)(computePipelines_.size() - 1);
    }

    ComputeShaderId CreateComputeShaderIn(const char* file, const char* entry) override {
        if (!computeLayout_ && !CreateComputeLayout()) return kNoComputeShader;
        std::wstring name;
        for (const char* c = file; *c; ++c) name += (wchar_t)*c;
        name += L"_";
        for (const char* c = entry; *c; ++c) name += (wchar_t)*c;
        name += L".spv";
        VkShaderModule module = LoadShader(name.c_str());
        if (!module) return kNoComputeShader;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = entry;
        ci.layout = computeLayout_;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult made = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        vkDestroyShaderModule(device_, module, nullptr);
        if (made != VK_SUCCESS) return kNoComputeShader;
        computePipelines_.push_back(pipeline);
        return (ComputeShaderId)(computePipelines_.size() - 1);
    }

    bool SetWaterDraw(const WaterDraw& draw) override {
        if (!waterPipeline_ && !CreateWaterPipeline()) return false;
        water_ = draw;
        waterMeshes_.assign(draw.meshes, draw.meshes + draw.meshCount);
        waterPending_ = !waterMeshes_.empty();
        return true;
    }

    void BeginCompute() override {
        if (!frameActive_) return;
        vkCmdEndRendering(cmd_); // the main pass has drawn nothing yet; EndCompute starts it again
        ComputeBarrier(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
    }

    void Dispatch(ComputeShaderId shader, const void* constants, uint32_t constantBytes, const GpuBufferId reads[4], const GpuBufferId writes[3], uint32_t x, uint32_t y, uint32_t z) override {
        if (!frameActive_ || shader >= computePipelines_.size()) return;
        ComputeBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = computePools_[frameSlot_];
        da.descriptorSetCount = 1;
        da.pSetLayouts = &computeSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &da, &set) != VK_SUCCESS) return;
        VkDescriptorBufferInfo infos[7];
        VkWriteDescriptorSet descriptorWrites[7];
        for (uint32_t i = 0; i < 7; ++i) {
            infos[i] = {gpuBuffers_[i < 4 ? reads[i] : writes[i - 4]].buffer.buffer, 0, VK_WHOLE_SIZE};
            descriptorWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            descriptorWrites[i].dstSet = set;
            descriptorWrites[i].dstBinding = i;
            descriptorWrites[i].descriptorCount = 1;
            descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 7, descriptorWrites, 0, nullptr);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelines_[shader]);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd_, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, constantBytes, constants);
        vkCmdDispatch(cmd_, x, y, z);
    }

    void EndCompute() override {
        if (!frameActive_) return;
        ComputeBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        BeginMainRendering();
    }

    void SetVegetationPass(int pass) override { vegetationPass_ = pass; }

    uint32_t MeshIndexCount(MeshId id) const override { return id < meshes_.size() && meshes_[id].buffer.buffer && meshes_[id].indexed ? meshes_[id].count : 0; }

    void DrawVegetationIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) override {
        if (!frameActive_ || resolved_ || !MeshIndexCount(id)) return;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vegetationPass_ == 1 ? vegetationDepthPipeline_ : vegetationPass_ == 2 ? vegetationShadePipeline_ : vegetationPipeline_);
        VegetationIndirect(meshes_[id], instances, args, argsOffset, viewProj, params);
        ++counts_.draws;
    }

    void DrawVegetationShadowIndirect(MeshId id, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& params) override {
        if (!shadowActive_ || !MeshIndexCount(id)) return;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vegetationShadowPipeline_);
        forceShadowRebind_ = true;
        VegetationIndirect(meshes_[id], instances, args, argsOffset, lightVP_, params);
    }

    bool ReadGpuBuffer(GpuBufferId id, void* out, uint64_t bytes) override {
        if (id >= gpuBuffers_.size() || !gpuBuffers_[id].buffer.buffer) return false;
        Buffer staging;
        if (!CreateBuffer((size_t)bytes, staging, true, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
        vkDeviceWaitIdle(device_);
        const bool ok = Immediate([&] {
            const VkBufferCopy region = {0, 0, bytes};
            vkCmdCopyBuffer(cmd_, gpuBuffers_[id].buffer.buffer, staging.buffer, 1, &region);
        });
        if (ok) std::memcpy(out, staging.mapped, (size_t)bytes);
        DestroyBuffer(staging);
        return ok;
    }

    void BeginFrame(const float clearColor[4]) override {
        prepass_ = 0;
        lastCounts_ = counts_;
        counts_ = {};
        frameSlot_ = (frameSlot_ + 1) % kFramesInFlight;
        frame_ = &frames_[frameSlot_];
        cmd_ = frame_->cmd;
        used_ = 0;
        frameActive_ = false;
        if (needRecreate_) {
            vkDeviceWaitIdle(device_);
            if (!CreateSwapchain() || !CreatePostTargets()) return;
            needRecreate_ = false;
        }
        if (!swapchain_) return;

        const double waitStart = TraceNowMs();
        vkWaitForFences(device_, 1, &frame_->fence, VK_TRUE, UINT64_MAX);
        completedSerial_ = std::max(completedSerial_, frame_->serial);
        if (computePools_[frameSlot_]) vkResetDescriptorPool(device_, computePools_[frameSlot_], 0); // once a frame: several BeginCompute may share it
        FlushStreamRetired(false);
        ReadTimer(*frame_);
        fenceWaitMs_ = TraceNowMs() - waitStart;
        std::memcpy(frame_->lightBuffer.mapped, &lighting_, sizeof(lighting_));
        std::memcpy(static_cast<uint8_t*>(frame_->lightBuffer.mapped) + sizeof(lighting_), &portals_, sizeof(portals_));
        reinterpret_cast<Lighting*>(frame_->lightBuffer.mapped)->eye[3] = (float)(postFrame_ & 1023); // the frame, for dithered transparency that TAA averages
        if (!cloudMapValid_) reinterpret_cast<Lighting*>(frame_->lightBuffer.mapped)->cloudMap[3] = 0.0f; // nothing drawn into it yet
        cloudMapValid_ = lighting_.cloudMap[3] > 0.5f;
        if (probeAge_ == 0) reinterpret_cast<Lighting*>(frame_->lightBuffer.mapped)->probeUpdate[3] = 0.0f; // the atlas holds nothing yet
        std::memcpy(motionBuffers_[frameSlot_].mapped, view_.viewProj.m, 64);
        std::memcpy(static_cast<uint8_t*>(motionBuffers_[frameSlot_].mapped) + 64, prevViewProj_.m, 64);
        std::memcpy(static_cast<uint8_t*>(motionBuffers_[frameSlot_].mapped) + 128, &view_.dt, 4);
        const VkResult acquired =
            vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame_->imageAvailable, VK_NULL_HANDLE, &imageIndex_);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            needRecreate_ = true;
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;
        vkResetFences(device_, 1, &frame_->fence);

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &bi);
        resolved_ = false;
        vkCmdResetQueryPool(cmd_, frame_->timerPool, 0, kTimerMarks);
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame_->timerPool, 0);
        shadowMarked_ = false;

        ImageBarrier(images_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        ImageBarrier(post_[kScene].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        ImageBarrier(post_[kVelocity].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        ImageBarrier(depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

        std::memcpy(clear_, clearColor, sizeof(clear_));
        BeginMainRendering();
        frameActive_ = true;
    }

    void StoreSunCache(uint64_t key) override {
        if (!shadowActive_) return;
        vkCmdEndRendering(cmd_);
        ImageBarrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        CopySunDepth(shadow_.image, sunCache_.image);
        ImageBarrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        BeginSunRendering(VK_ATTACHMENT_LOAD_OP_LOAD);
        sunCacheKey_ = key;
    }

    void BeginShadowPass(ShadowMap map, bool restoreSunCache) override {
        if (!frameActive_) return;
        shadowTarget_ = map;
        const bool tight = map == ShadowMap::Tight;
        const StoreImage& target = tight ? tightShadow_ : shadow_;
        const uint32_t size = tight ? kTightShadowMapSize : kShadowMapSize;
        vkCmdEndRendering(cmd_); // the main pass has drawn nothing yet; EndShadowPass starts it again
        std::memcpy(lightVP_.m, tight ? lighting_.tightVP : lighting_.shadowVP, sizeof(lightVP_.m));
        const bool restore = restoreSunCache && !tight;
        if (restore) {
            ImageBarrier(sunCache_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            ImageBarrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkImageCopy region = {{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {kShadowMapSize, kShadowMapSize, 1}};
            vkCmdCopyImage(cmd_, sunCache_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shadow_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ImageBarrier(sunCache_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            ImageBarrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            BeginSunRendering(VK_ATTACHMENT_LOAD_OP_LOAD);
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
            shadowActive_ = true;
            return;
        }
        ImageBarrier(target.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = target.layerViews[0];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {size, size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        // Same flipped Y as the main pass, so one shader formula maps clip space to shadow-map uv on both APIs.
        const VkViewport vp = {0.0f, (float)size, (float)size, -(float)size, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {size, size}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, tight ? tightPipeline_ : shadowPipeline_);
        shadowActive_ = true;
    }

    // Rendering into the sun map, with the same flipped viewport as BeginShadowPass.
    void BeginSunRendering(VkAttachmentLoadOp load) {
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = shadow_.layerViews[0];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = load;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport vp = {0.0f, (float)kShadowMapSize, (float)kShadowMapSize, -(float)kShadowMapSize, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    // From an image in TRANSFER_SRC to the cache, resting in TRANSFER_DST.
    void CopySunDepth(VkImage from, VkImage to) {
        const VkImageCopy region = {{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {kShadowMapSize, kShadowMapSize, 1}};
        vkCmdCopyImage(cmd_, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    void DrawShadowCaster(MeshId id, const Mat4& model) override {
        if (!shadowActive_ || id >= meshes_.size() || !meshes_[id].buffer.buffer ||
            meshes_[id].topology != Topology::Triangles)
            return;
        const Mesh& mesh = meshes_[id];
        if (shadowInstancedBound_ || forceShadowRebind_) { // an instanced caster switched the pipeline
            forceShadowRebind_ = false;
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTarget_ == ShadowMap::Tight ? tightPipeline_ : shadowPipeline_);
            shadowInstancedBound_ = false;
        }
        const Mat4 mvp = lightVP_ * model;
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        const VkDeviceSize offset = mesh.base + mesh.copyBytes * NewestCopy(mesh); // the base, plus the copy if deformable
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &mesh.buffer.buffer, &offset);
        if (mesh.indexed) {
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd_, mesh.count, 1, 0, 0);
        }
        ++counts_.shadowDraws;
        counts_.shadowTriangles += mesh.count / 3;
    }

    // Hair cards into a shadow map: depth only where the card's texture has coverage (PSShadowCutout), so a head of hair casts
    // strand-shaped, partly see-through shadows instead of the cards' rectangles.
    void DrawShadowCasterCutout(MeshId id, const Mat4& model, TextureId albedo) override {
        if (!shadowActive_ || id >= meshes_.size() || !meshes_[id].buffer.buffer || meshes_[id].topology != Topology::Triangles) return;
        SetMaterial(albedo, kNoTexture, kNoTexture);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTarget_ == ShadowMap::Tight ? tightCutoutPipeline_ : shadowCutoutPipeline_);
        forceShadowRebind_ = true; // the next caster sets its own pipeline
        const Mesh& mesh = meshes_[id];
        const Mat4 mvp = lightVP_ * model;
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        const VkDeviceSize offset = mesh.base + mesh.copyBytes * NewestCopy(mesh); // the base, plus the copy if deformable
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &mesh.buffer.buffer, &offset);
        if (mesh.indexed) {
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd_, mesh.count, 1, 0, 0);
        }
        ++counts_.shadowDraws;
        counts_.shadowTriangles += mesh.count / 3;
    }

    void DrawShadowInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count) override {
        if (!shadowActive_ || count == 0 || id >= meshes_.size() || !meshes_[id].buffer.buffer || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer].buffer)
            return;
        if (!shadowInstancedBound_ || forceShadowRebind_) {
            forceShadowRebind_ = false;
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTarget_ == ShadowMap::Tight ? tightInstancedPipeline_ : shadowInstancedPipeline_);
            shadowInstancedBound_ = true;
        }
        PushConstants pc;
        std::memcpy(pc.mvp, lightVP_.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        DrawInstancedMesh(meshes_[id], instanceBuffers_[buffer], first, count);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    void SetScenePrepass(int mode) override { prepass_ = mode >= 0 && mode <= 2 ? mode : 0; }

    void DrawInstanced(MeshId id, InstanceBufferId buffer, uint32_t first, uint32_t count, const Mat4& viewProj) override {
        if (!frameActive_ || count == 0 || resolved_ || id >= meshes_.size() || !meshes_[id].buffer.buffer || meshes_[id].topology != Topology::Triangles ||
            buffer >= instanceBuffers_.size() || !instanceBuffers_[buffer].buffer)
            return;
        const Mesh& mesh = meshes_[id];
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, prepass_ ? prepassPipelines_[1][prepass_ - 1] : instancedPipeline_);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        DrawInstancedMesh(mesh, instanceBuffers_[buffer], first, count);
        if (prepass_ == 1) return; // the pre-pass: its draws are counted once, when shaded
        ++counts_.draws;
        counts_.triangles += (uint64_t)(mesh.count / 3) * count;
    }

    void DrawInstancedTransient(MeshId id, const float* rows, uint32_t count, const Mat4& viewProj) override {
        if (!frameActive_ || count == 0 || !rows || resolved_ || id >= meshes_.size() || !meshes_[id].buffer.buffer || meshes_[id].topology != Topology::Triangles)
            return;
        const size_t bytes = (size_t)count * kInstanceStride;
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("Vulkan: transient buffer full, instanced draw dropped");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(frame_->transient.mapped) + offset, rows, bytes);
        used_ = offset + bytes;
        const Mesh& mesh = meshes_[id];
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, instancedPipeline_);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        const VkBuffer buffers[2] = {mesh.buffer.buffer, frame_->transient.buffer};
        const VkDeviceSize offsets[2] = {mesh.base, (VkDeviceSize)offset};
        vkCmdBindVertexBuffers(cmd_, 0, 2, buffers, offsets);
        if (mesh.indexed) {
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, count, 0, 0, 0);
        } else {
            vkCmdDraw(cmd_, mesh.count, count, 0, 0);
        }
        ++counts_.draws;
        counts_.triangles += (uint64_t)(mesh.count / 3) * count;
    }

    void SetDrawFade(float keep) override {
        materialConstants_[3] = keep;
        if (frameActive_ && !resolved_) vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_), materialConstants_);
    }

    void DrawShadowInstancedTransient(MeshId id, const float* rows, uint32_t count) override {
        if (!shadowActive_ || count == 0 || !rows || id >= meshes_.size() || !meshes_[id].buffer.buffer || meshes_[id].topology != Topology::Triangles) return;
        const size_t bytes = (size_t)count * kInstanceStride;
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("Vulkan: transient buffer full, instanced caster dropped");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(frame_->transient.mapped) + offset, rows, bytes);
        used_ = offset + bytes;
        if (!shadowInstancedBound_ || forceShadowRebind_) {
            forceShadowRebind_ = false;
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTarget_ == ShadowMap::Tight ? tightInstancedPipeline_ : shadowInstancedPipeline_);
            shadowInstancedBound_ = true;
        }
        PushConstants pc;
        std::memcpy(pc.mvp, lightVP_.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        DrawTransientInstances(meshes_[id], offset, count);
        ++counts_.shadowDraws;
        counts_.shadowTriangles += (uint64_t)(meshes_[id].count / 3) * count;
    }

    // ------------------------------------------------------------------ city chunk bakes (Track R; renderer.h, shaders/chunk_faces.hlsli)
    // The chunk store is an impostor store of its own (three channel arrays, one layer per face, channel 2 RG16 for the relief's
    // start and end, and a tile depth buffer): a face is drawn into a layer, then copied into the atlas channel, a plain 2D image
    // registered in textures_ so DrawChunkVolumes (and SetMaterial) binds it like any other texture.
    static constexpr VkFormat kChunkFormats[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_UNORM};

    void ReleaseChunkAtlas() {
        if (!chunkTile_) return;
        vkDeviceWaitIdle(device_);
        for (TextureId& id : chunkTex_) {
            DestroyTexture(id); // also drops the material sets that name it
            id = kNoTexture;
        }
        for (StoreImage& img : chunkStore_) DestroyStoreImage(img);
        DestroyStoreImage(chunkDepth_);
        chunkTile_ = chunkW_ = chunkH_ = 0;
    }

    bool CreateChunkAtlas(uint32_t tile, uint32_t width, uint32_t height) override {
        if (!device_ || frameActive_ || tile < 4 || width < tile * kChunkFaceCount || height < tile) return false;
        if (tile == chunkTile_ && width == chunkW_ && height == chunkH_) return true;
        ReleaseChunkAtlas();
        chunkTile_ = tile, chunkW_ = width, chunkH_ = height; // from here a failure releases what was made
        const auto fail = [this] { ReleaseChunkAtlas(); return false; };
        for (int c = 0; c < 3; ++c) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys_, kChunkFormats[c], &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((fp.optimalTilingFeatures & need) != need) {
                Log("Vulkan: format %d cannot be rendered to, blended and sampled; no chunk atlas", (int)kChunkFormats[c]);
                return fail();
            }
            if (!CreateStoreImage(kChunkFormats[c], tile, tile, (uint32_t)kChunkFaceCount,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, chunkStore_[c]))
                return fail();
        }
        if (!CreateStoreImage(kDepthFormat, tile, tile, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, chunkDepth_)) return fail();
        for (int c = 0; c < 3; ++c) {
            Texture tex;
            VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = kChunkFormats[c];
            ii.extent = {width, height, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!AllocateImage(ii, tex.image, tex.memory)) { DestroyTextureObjects(tex); return fail(); }
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = tex.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ii.format; // RG16 reads (r, g, 0, 1)
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &vi, nullptr, &tex.view) != VK_SUCCESS) { DestroyTextureObjects(tex); return fail(); }
            TextureId id;
            if (!freeTextures_.empty()) { id = freeTextures_.back(); freeTextures_.pop_back(); textures_[id] = tex; }
            else { id = (TextureId)textures_.size(); textures_.push_back(tex); }
            chunkTex_[c] = id;
        }
        // The atlas starts empty (channel 2 all ones: relief (1, 1) reads as nothing there), then rests in the shader-read layout.
        const bool ok = Immediate([&] {
            for (int c = 0; c < 3; ++c) {
                const VkImage image = textures_[chunkTex_[c]].image;
                ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkClearColorValue value = {};
                if (c == 2) value.float32[0] = value.float32[1] = value.float32[2] = value.float32[3] = 1.0f;
                const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        });
        if (!ok) return fail();
        Log("Vulkan: chunk atlas %ux%u, %u px tiles (%u cells)", width, height, tile, (width / (tile * kChunkFaceCount)) * (height / tile));
        return true;
    }

    TextureId ChunkAtlasTexture(int channel) const override { return channel >= 0 && channel < 3 ? chunkTex_[channel] : kNoTexture; }

    bool BeginChunkBake() override {
        if (!frameActive_ || resolved_ || shadowActive_ || chunkBaking_ || !chunkTile_ || !chunkBakePipelines_[0] || !chunkBakePipelines_[1]) return false;
        vkCmdEndRendering(cmd_); // the main pass has drawn nothing yet; EndChunkBake starts it again
        chunkBaking_ = true;
        return true;
    }

    void ChunkBakeFace(const ChunkBakeItem* items, uint32_t count, const Mat4& faceViewProj, uint32_t atlasX, uint32_t atlasY) override {
        if (!chunkBaking_ || atlasX + chunkTile_ > chunkW_ || atlasY + chunkTile_ > chunkH_) return;
        const uint32_t layer = chunkLayer_++ % (uint32_t)kChunkFaceCount;
        // Every layer is cleared before it is drawn, so the store's old contents never matter: from UNDEFINED, after the last copy.
        for (StoreImage& img : chunkStore_)
            ImageBarrier(img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, (uint32_t)kChunkFaceCount);
        ImageBarrier(chunkDepth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        VkRenderingAttachmentInfo colors[3] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                               {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        const float clears[3][4] = {{0, 0, 0, 0}, {0.5f, 0.5f, 1.0f, 1.0f}, {1, 1, 0, 0}};
        for (int c = 0; c < 3; ++c) {
            colors[c].imageView = chunkStore_[c].layerViews[layer];
            colors[c].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[c].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[c].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            std::memcpy(colors[c].clearValue.color.float32, clears[c], sizeof(clears[c]));
        }
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = chunkDepth_.layerViews[0];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0}; // the bake tests LESS, not reversed
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {chunkTile_, chunkTile_}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 3;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        // Flipped like the main pass, so clip y = +1 is the image's top row, as on Direct3D: the atlas reads the same on both.
        const VkViewport vp = {0.0f, (float)chunkTile_, (float)chunkTile_, -(float)chunkTile_, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {chunkTile_, chunkTile_}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        PushConstants pc;
        std::memcpy(pc.mvp, faceViewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        // The rows go into the transient buffer once; both passes draw from there. SIZE_MAX marks an item that is skipped.
        chunkOffsets_.assign(count, SIZE_MAX);
        for (uint32_t k = 0; k < count; ++k) {
            const ChunkBakeItem& it = items[k];
            if (!it.rows || !it.count || it.mesh >= meshes_.size() || !meshes_[it.mesh].buffer.buffer || meshes_[it.mesh].topology != Topology::Triangles) continue;
            const size_t bytes = (size_t)it.count * kInstanceStride, offset = (used_ + 15) & ~size_t(15);
            if (offset + bytes > kTransientBytes) { Log("Vulkan: transient buffer full, chunk bake item dropped"); continue; }
            std::memcpy(static_cast<uint8_t*>(frame_->transient.mapped) + offset, it.rows, bytes);
            used_ = offset + bytes;
            chunkOffsets_[k] = offset;
        }
        for (int pass = 0; pass < 2; ++pass) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, chunkBakePipelines_[pass]);
            TextureId bound = kNoTexture;
            bool any = false;
            for (uint32_t k = 0; k < count; ++k) {
                if (chunkOffsets_[k] == SIZE_MAX) continue;
                if (!any || items[k].albedo != bound) { SetMaterial(items[k].albedo, kNoTexture, kNoTexture); bound = items[k].albedo; any = true; }
                DrawTransientInstances(meshes_[items[k].mesh], chunkOffsets_[k], items[k].count);
            }
        }
        vkCmdEndRendering(cmd_);
        // Into the atlas.
        for (int c = 0; c < 3; ++c) {
            const VkImage atlas = textures_[chunkTex_[c]].image;
            ImageBarrier(chunkStore_[c].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, (uint32_t)kChunkFaceCount);
            // The previous face's copy into the same image counts as well as this frame's samplers.
            ImageBarrier(atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy region = {};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffset = {(int32_t)atlasX, (int32_t)atlasY, 0};
            region.extent = {chunkTile_, chunkTile_, 1};
            vkCmdCopyImage(cmd_, chunkStore_[c].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ImageBarrier(atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }

    void EndChunkBake() override {
        if (!chunkBaking_) return;
        chunkBaking_ = false;
        BeginMainRendering(); // clears the scene's targets: nothing was drawn into them yet
        SetMaterial(kNoTexture, kNoTexture, kNoTexture);
    }

    // A one-off copy between an atlas channel and a CPU buffer, outside the frame; waits. Rows tightly packed, top row first.
    bool ChunkAtlasCopy(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>* out, const uint8_t* in) {
        if (channel < 0 || channel > 2 || !chunkTile_ || !w || !h || x + w > chunkW_ || y + h > chunkH_) return false;
        const VkImage atlas = textures_[chunkTex_[channel]].image;
        const size_t bytes = (size_t)w * h * 4;
        Buffer staging;
        if (!CreateBuffer(bytes, staging, true, out ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            DestroyBuffer(staging);
            return false;
        }
        if (in) std::memcpy(staging.mapped, in, bytes);
        const VkImageLayout copyLayout = out ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const VkAccessFlags2 copyAccess = out ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        const bool ok = Immediate([&] {
            ImageBarrier(atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, copyLayout,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, copyAccess);
            VkBufferImageCopy region = {}; // bufferRowLength 0: tightly packed
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {(int32_t)x, (int32_t)y, 0};
            region.imageExtent = {w, h, 1};
            if (out) vkCmdCopyImageToBuffer(cmd_, atlas, copyLayout, staging.buffer, 1, &region);
            else vkCmdCopyBufferToImage(cmd_, staging.buffer, atlas, copyLayout, 1, &region);
            ImageBarrier(atlas, VK_IMAGE_ASPECT_COLOR_BIT, copyLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, out ? 0 : VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (out) { // the copy's writes, for the host to map
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd_, &dep);
            }
        });
        if (ok && out) out->assign(static_cast<const uint8_t*>(staging.mapped), static_cast<const uint8_t*>(staging.mapped) + bytes);
        DestroyBuffer(staging);
        return ok;
    }
    bool ReadChunkAtlas(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& out) override {
        return ChunkAtlasCopy(channel, x, y, w, h, &out, nullptr);
    }
    bool WriteChunkAtlas(int channel, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* texels) override {
        return texels && ChunkAtlasCopy(channel, x, y, w, h, nullptr, texels);
    }

    void DrawChunkVolumes(MeshId id, const Mat4& viewProj) override {
        if (!frameActive_ || resolved_ || chunkBaking_ || !chunkTile_ || !chunkVolPipeline_ || id >= meshes_.size() || !meshes_[id].buffer.buffer || !meshes_[id].indexed) return;
        const Mesh& mesh = meshes_[id];
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, chunkVolPipeline_);
        SetMaterial(chunkTex_[0], chunkTex_[1], chunkTex_[2]);
        materialConstants_[2] = (float)chunkTile_;
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_), materialConstants_);
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, Mat4::Identity().m, sizeof(pc.model));
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &mesh.buffer.buffer, &mesh.base);
        vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_, mesh.count, 1, 0, 0, 0);
        ++counts_.draws;
        counts_.triangles += mesh.count / 3;
        materialConstants_[2] = 0.0f;
        SetMaterial(kNoTexture, kNoTexture, kNoTexture);
    }

    void EndShadowPass() override {
        if (!shadowActive_) return;
        shadowActive_ = false;
        shadowInstancedBound_ = false;
        vkCmdEndRendering(cmd_);
        if (shadowTarget_ == ShadowMap::Sun) { // the tight pass, drawn first, falls inside this stretch
            Mark(1);
            shadowMarked_ = true;
        }
        ImageBarrier((shadowTarget_ == ShadowMap::Tight ? tightShadow_ : shadow_).image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        BeginMainRendering();
    }

    void BeginCapture() override {
        if (!frameActive_) return;
        vkCmdEndRendering(cmd_);
        ImageBarrier(post_[kCapture].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo colors[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        colors[0].imageView = post_[kCapture].layerViews[0];
        colors[1].imageView = post_[kVelocity].layerViews[0];
        for (VkRenderingAttachmentInfo& c : colors) {
            c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // zero alpha: nothing here
            c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reversed Z
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kCaptureSize, kCaptureSize}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport vp = {0.0f, (float)kCaptureSize, (float)kCaptureSize, -(float)kCaptureSize, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {kCaptureSize, kCaptureSize}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void EndCapture() override {
        if (!frameActive_) return;
        vkCmdEndRendering(cmd_);
        ColorToRead(post_[kCapture].image);
        BeginMainRendering(); // clears the velocity and depth the capture used
    }

    // Starts (or, after the shadow pass, restarts) rendering into the swapchain image: clears it, sets the flipped
    // viewport and binds the per-frame descriptors.
    void BeginMainRendering() {
        VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = post_[kScene].layerViews[0];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(color.clearValue.color.float32, clear_, sizeof(clear_));
        VkRenderingAttachmentInfo sceneColors[2] = {color, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        sceneColors[1].imageView = post_[kVelocity].layerViews[0];
        sceneColors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        sceneColors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // zero: "ask the depth buffer"
        sceneColors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // read by the post passes, then by the overlays
        depth.clearValue.depthStencil = {0.0f, 0}; // reversed Z
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, sceneExtent_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = sceneColors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);

        // Negative height flips Vulkan's Y so images match Direct3D (clip space Y up).
        VkViewport vp = {0.0f, (float)sceneExtent_.height, (float)sceneExtent_.width, -(float)sceneExtent_.height, 0.0f, 1.0f};
        VkRect2D scissor = {{0, 0}, sceneExtent_};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        BindMeshSets();
    }

    void BindMeshSets() {
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame_->lightSet, 0, nullptr);
        const VkDescriptorSet material = MaterialSet(kNoTexture, kNoTexture, kNoTexture);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &material, 0, nullptr);
    }

    void Draw(MeshId id, const Mat4& mvp, const Mat4& model, bool depthTest, bool counted) override {
        if (!frameActive_ || id >= meshes_.size() || !meshes_[id].buffer.buffer) return;
        const Mesh& mesh = meshes_[id];
        if (prepass_ == 1 && (mesh.copyBytes || resolved_ || !depthTest || mesh.topology != Topology::Triangles)) return; // only static lit triangles
        if (mesh.copyBytes && !resolved_ && depthTest && mesh.topology == Topology::Triangles && mesh.indexed) { // skinned: it says how it moved
            PushConstants pc;
            std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
            std::memcpy(pc.model, model.m, sizeof(pc.model));
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedMotionPipeline_);
            vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_), materialConstants_);
            const VkBuffer buffers[2] = {mesh.buffer.buffer, mesh.buffer.buffer};
            const VkDeviceSize offsets[2] = {mesh.copyBytes * NewestCopy(mesh), mesh.copyBytes * (NewestCopy(mesh) ^ 1)};
            vkCmdBindVertexBuffers(cmd_, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, 1, 0, 0, 0);
            if (counted) { ++counts_.draws; counts_.triangles += mesh.count / 3; }
            return;
        }
        Submit(mesh.buffer.buffer, mesh.base + mesh.copyBytes * NewestCopy(mesh), mesh.count, mesh.topology, mvp, model, depthTest, mesh.indexed, mesh.indexOffset);
        if (counted) {
            ++counts_.draws;
            if (mesh.topology == Topology::Triangles) counts_.triangles += mesh.count / 3;
        }
    }

    void DrawTransient(const Vertex* vertices, uint32_t count, Topology topology, const Mat4& mvp, const Mat4& model,
                       bool depthTest) override {
        if (!frameActive_ || count == 0) return;
        const size_t bytes = (size_t)count * sizeof(Vertex);
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("Vulkan: transient buffer full, draw dropped");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(frame_->transient.mapped) + offset, vertices, bytes);
        used_ = offset + bytes;
        Submit(frame_->transient.buffer, offset, count, topology, mvp, model, depthTest);
    }

    void EndFrame() override {
        if (!frameActive_) return;
        ResolveScene(); // a caller that drew no overlays still gets its picture
        vkCmdEndRendering(cmd_);
        const bool capturing = capture_ && PrepareCapture();
        if (capturing) {
            ImageBarrier(images_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyImageToBuffer(cmd_, images_[imageIndex_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer_.buffer, 1, &copy);
            ImageBarrier(images_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        } else {
            ImageBarrier(images_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        }
        Mark(10);
        vkEndCommandBuffer(cmd_);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &frame_->imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        frame_->serial = ++frameSerial_;
        const VkCommandBuffer stream = CloseStream(frame_->serial);
        const VkCommandBuffer both[2] = {stream, cmd_}; // this frame's streamed copies first; their barrier ends the stream buffer
        si.commandBufferCount = stream ? 2 : 1;
        si.pCommandBuffers = stream ? both : &cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone_[imageIndex_];
        vkQueueSubmit(queue_, 1, &si, frame_->fence);

        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone_[imageIndex_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &imageIndex_;
        const double presentStart = TraceNowMs();
        const VkResult presented = vkQueuePresentKHR(queue_, &pi);
        presentMs_ = TraceNowMs() - presentStart;
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
        frameActive_ = false;
        frame_->timerPending = true;
        if (capturing) {
            vkWaitForFences(device_, 1, &frame_->fence, VK_TRUE, UINT64_MAX);
            const size_t bytes = (size_t)extent_.width * extent_.height * 4;
            captureRgba_.resize(bytes);
            std::memcpy(captureRgba_.data(), captureBuffer_.mapped, bytes);
            captureW_ = (int)extent_.width;
            captureH_ = (int)extent_.height;
            captureNew_ = true;
        }
    }

private:
    // ------------------------------------------------------------------ helpers shared by the texture and post code
    bool AllocateImage(const VkImageCreateInfo& ii, VkImage& image, VkDeviceMemory& memory) {
        VK_CHECK(vkCreateImage(device_, &ii, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device_, &ai, nullptr, &memory) != VK_SUCCESS) return false;
        VK_CHECK(vkBindImageMemory(device_, image, memory, 0));
        return true;
    }

    // Records into a throwaway command buffer (the frame's own may be in flight), submits it and waits.
    // What a no-wait upload or a retired resource must keep until no submitted work can use it.
    struct StreamRetired {
        uint64_t frame = 0;
        Buffer staging;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        MeshId mesh = kInvalidMesh;
        TextureId texture = kNoTexture;
    };

    // ------------------------------------------------------------------ streaming uploads
    // A streamed mesh used to cost two buffers with memory of their own, a command buffer and a queue submit: milliseconds each
    // on the main thread, and two of the driver's few thousand allocations. Now it is a range of a big device-local block, a
    // memcpy into one persistently mapped staging ring, and a copy recorded into the frame's one stream command buffer, which
    // EndFrame submits ahead of the frame's own in the same vkQueueSubmit. The ring and the command buffers come back by
    // polling the frame fences (vkGetFenceStatus), never by waiting on them.
    static constexpr VkDeviceSize kStreamRingBytes = 128ull << 20;
    static constexpr VkDeviceSize kMeshBlockBytes = 64ull << 20;
    static constexpr VkDeviceSize kMeshAlign = 256;
    static constexpr VkBufferUsageFlags kMeshBlockUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    struct MeshBlock {
        Buffer buffer;
        VkDeviceSize size = 0;
        std::map<VkDeviceSize, VkDeviceSize> free; // offset -> bytes
    };
    struct StreamCommands { VkCommandBuffer cmd = VK_NULL_HANDLE; uint64_t serial = 0; };
    struct RingMark { uint64_t serial; VkDeviceSize head; };
    std::vector<MeshBlock> meshBlocks_;
    std::future<Buffer> spareBlock_; // the next block, made on another thread
    Buffer streamRing_;
    VkDeviceSize streamHead_ = 0, streamTail_ = 0; // bytes ever handed out / ever given back; [tail, head) may still be read
    std::deque<RingMark> streamMarks_;
    std::vector<StreamCommands> streamCommands_;
    size_t streamInUse_ = 0;
    bool streamOpen_ = false;
    uint64_t frameSerial_ = 0, completedSerial_ = 0; // EndFrame numbers its submissions; the newest known to be done
    uint32_t streamOverflows_ = 0;

    void PollCompletedFrames() {
        for (const Frame& f : frames_)
            if (f.serial > completedSerial_ && f.fence && vkGetFenceStatus(device_, f.fence) == VK_SUCCESS) completedSerial_ = std::max(completedSerial_, f.serial);
    }

    void StartSpareBlock() {
        if (spareBlock_.valid()) return;
        spareBlock_ = std::async(std::launch::async, [this] {
            Buffer b;
            if (!CreateDeviceBuffer(kMeshBlockBytes, b, kMeshBlockUsage)) b = {};
            return b;
        });
    }

    bool MeshMemory(VkDeviceSize bytes, uint32_t& block, VkDeviceSize& offset, VkDeviceSize& taken) {
        taken = (bytes + kMeshAlign - 1) / kMeshAlign * kMeshAlign;
        for (uint32_t b = 0; b < (uint32_t)meshBlocks_.size(); ++b) {
            auto& free = meshBlocks_[b].free;
            for (auto it = free.begin(); it != free.end(); ++it) {
                if (it->second < taken) continue;
                offset = it->first;
                const VkDeviceSize rest = it->second - taken;
                free.erase(it);
                if (rest) free[offset + taken] = rest;
                block = b;
                return true;
            }
        }
        MeshBlock nb;
        nb.size = std::max(kMeshBlockBytes, taken);
        if (nb.size == kMeshBlockBytes && spareBlock_.valid() && spareBlock_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) nb.buffer = spareBlock_.get();
        if (!nb.buffer.buffer && !CreateDeviceBuffer(nb.size, nb.buffer, kMeshBlockUsage)) return false;
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
        VkDeviceSize at = mesh.base, bytes = mesh.blockBytes;
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

    // Where this frame's copy reads from: a piece of the ring, or (a burst bigger than the ring) a buffer of its own that
    // lives until the frames that read it are done.
    uint8_t* StreamStaging(VkDeviceSize bytes, VkDeviceSize align, VkBuffer& source, VkDeviceSize& base) {
        PollCompletedFrames();
        while (!streamMarks_.empty() && streamMarks_.front().serial <= completedSerial_) {
            streamTail_ = streamMarks_.front().head;
            streamMarks_.pop_front();
        }
        VkDeviceSize start = (streamHead_ + align - 1) / align * align;
        const VkDeviceSize pos = start % kStreamRingBytes;
        if (pos + bytes > kStreamRingBytes) start += kStreamRingBytes - pos; // never across the end
        if (streamRing_.mapped && bytes <= kStreamRingBytes && start + bytes - streamTail_ <= kStreamRingBytes) {
            streamHead_ = start + bytes;
            source = streamRing_.buffer;
            base = start % kStreamRingBytes;
            return static_cast<uint8_t*>(streamRing_.mapped) + base;
        }
        StreamRetired keep;
        if (!CreateBuffer(bytes, keep.staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return nullptr;
        if (++streamOverflows_ % 64 == 1) Log("Vulkan: the stream ring is full; %u uploads have had buffers of their own", streamOverflows_);
        source = keep.staging.buffer;
        base = 0;
        keep.frame = streamFrame_;
        streamRetired_.push_back(keep);
        return static_cast<uint8_t*>(keep.staging.mapped);
    }

    bool StreamOpen() {
        if (streamOpen_) return true;
        PollCompletedFrames();
        size_t pick = streamCommands_.size();
        for (size_t i = 0; i < streamCommands_.size(); ++i)
            if (streamCommands_[i].serial <= completedSerial_) { pick = i; break; }
        if (pick == streamCommands_.size()) {
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            StreamCommands c;
            if (vkAllocateCommandBuffers(device_, &ai, &c.cmd) != VK_SUCCESS) return false;
            streamCommands_.push_back(c);
        } else vkResetCommandBuffer(streamCommands_[pick].cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(streamCommands_[pick].cmd, &bi) != VK_SUCCESS) return false;
        streamCommands_[pick].serial = UINT64_MAX; // recording: not free until its submission has run
        streamInUse_ = pick;
        streamOpen_ = true;
        return true;
    }

    // Records into the open stream command buffer (helpers that write to cmd_ work unchanged).
    bool StreamRecord(const std::function<void()>& record) {
        if (!StreamOpen()) return false;
        const VkCommandBuffer frame = cmd_;
        cmd_ = streamCommands_[streamInUse_].cmd;
        record();
        cmd_ = frame;
        return true;
    }

    bool StreamCopy(const void* a, size_t aBytes, const void* b, size_t bBytes, VkBuffer dst, VkDeviceSize dstOffset) {
        VkBuffer source = VK_NULL_HANDLE;
        VkDeviceSize base = 0;
        uint8_t* mapped = StreamStaging(aBytes + bBytes, 16, source, base);
        if (!mapped || !StreamOpen()) return false;
        std::memcpy(mapped, a, aBytes);
        if (bBytes) std::memcpy(mapped + aBytes, b, bBytes);
        const VkBufferCopy region = {base, dstOffset, aBytes + bBytes};
        vkCmdCopyBuffer(streamCommands_[streamInUse_].cmd, source, dst, 1, &region);
        return true;
    }

    // The stream command buffer to submit ahead of the frame's (null when nothing was streamed), closed with one barrier that
    // makes every copy visible to the vertex input and the shaders. `serial` numbers that submission.
    VkCommandBuffer CloseStream(uint64_t serial) {
        if (!streamOpen_) return VK_NULL_HANDLE;
        const VkCommandBuffer c = streamCommands_[streamInUse_].cmd;
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(c);
        streamCommands_[streamInUse_].serial = serial;
        streamMarks_.push_back({serial, streamHead_});
        streamOpen_ = false;
        return c;
    }

    void DestroyStreaming() {
        if (spareBlock_.valid()) { Buffer b = spareBlock_.get(); DestroyBuffer(b); }
        for (MeshBlock& block : meshBlocks_) DestroyBuffer(block.buffer);
        meshBlocks_.clear();
        DestroyBuffer(streamRing_);
        for (StreamCommands& c : streamCommands_) vkFreeCommandBuffers(device_, pool_, 1, &c.cmd);
        streamCommands_.clear();
    }

    // Frame U's fence has passed by BeginFrame U+2 (same slot); work queued after U runs before U+1's, so U+3 covers both.
    void FlushStreamRetired(bool all) {
        if (!all) ++streamFrame_;
        size_t kept = 0;
        // Unloading a town frees thousands of objects, and each can cost a good part of a millisecond: 2 ms per frame.
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        for (size_t i = 0; i < streamRetired_.size(); ++i) {
            StreamRetired& r = streamRetired_[i];
            QueryPerformanceCounter(&now);
            if (!all && (r.frame + kFramesInFlight + 1 > streamFrame_ || (now.QuadPart - start.QuadPart) * 500 > freq.QuadPart)) {
                streamRetired_[kept++] = r;
                continue;
            }
            if (r.staging.buffer) DestroyBuffer(r.staging);
            if (r.commands) vkFreeCommandBuffers(device_, pool_, 1, &r.commands);
            if (r.mesh != kInvalidMesh) {
                if (!meshes_[r.mesh].sharesBuffer) DestroyBuffer(meshes_[r.mesh].buffer);
                FreeMeshMemory(meshes_[r.mesh]);
                meshes_[r.mesh] = {};
                freeSlots_.push_back(r.mesh);
            }
            if (r.texture != kNoTexture) {
                for (auto it = materialSets_.begin(); it != materialSets_.end();) {
                    if ((TextureId)(it->first >> 42) == r.texture || (TextureId)((it->first >> 21) & 0x1FFFFF) == r.texture || (TextureId)(it->first & 0x1FFFFF) == r.texture || ExtraOf((TextureId)(it->first & 0x1FFFFF)) == r.texture) {
                        vkFreeDescriptorSets(device_, materialPool_, 1, &it->second);
                        it = materialSets_.erase(it);
                    } else ++it;
                }
                DestroyTextureObjects(textures_[r.texture]);
                textures_[r.texture] = {};
                if (r.texture < extraOf_.size()) extraOf_[r.texture] = kNoTexture;
                freeTextures_.push_back(r.texture);
            }
        }
        streamRetired_.resize(kept);
    }

    bool Immediate(const std::function<void()>& record) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer temp = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &temp));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(temp, &bi));
        const VkCommandBuffer frame = cmd_;
        cmd_ = temp;
        record();
        cmd_ = frame;
        VK_CHECK(vkEndCommandBuffer(temp));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &temp;
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue_));
        vkFreeCommandBuffers(device_, pool_, 1, &temp);
        return true;
    }

    // ------------------------------------------------------------------ materials
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE; // its own; null when it lives in an image block
        VkImageView view = VK_NULL_HANDLE;
        uint32_t block = UINT32_MAX;            // streamed: a range of imageBlocks_[block]
        VkDeviceSize blockOffset = 0, blockBytes = 0;
    };

    void DestroyTextureObjects(Texture& t) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        if (t.block < imageBlocks_.size() && t.blockBytes) GiveRange(imageBlocks_[t.block].free, t.blockOffset, t.blockBytes);
        t = {};
    }

    // Streamed textures: images bound into 256 MB device-local blocks (one per memory type, a spare made ahead on another
    // thread), so a texture costs no allocation of its own: one was 20-45 ms now and then, and the driver allows only thousands.
    static constexpr VkDeviceSize kImageBlockBytes = 256ull << 20;
    struct ImageBlock { VkDeviceMemory memory = VK_NULL_HANDLE; uint32_t type = 0; std::map<VkDeviceSize, VkDeviceSize> free; };
    std::vector<ImageBlock> imageBlocks_;
    std::future<VkDeviceMemory> spareImageBlock_;
    uint32_t spareImageType_ = UINT32_MAX;

    VkDeviceMemory MakeImageMemory(uint32_t type) {
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = kImageBlockBytes;
        ai.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        return vkAllocateMemory(device_, &ai, nullptr, &memory) == VK_SUCCESS ? memory : VK_NULL_HANDLE;
    }
    void StartSpareImageBlock(uint32_t type) {
        if (spareImageBlock_.valid() || type == UINT32_MAX) return;
        spareImageType_ = type;
        spareImageBlock_ = std::async(std::launch::async, [this, type] { return MakeImageMemory(type); });
    }
    static bool TakeRange(std::map<VkDeviceSize, VkDeviceSize>& free, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize& offset) {
        for (auto it = free.begin(); it != free.end(); ++it) {
            const VkDeviceSize at = it->first, size = it->second, start = (at + align - 1) / align * align;
            if (start + bytes > at + size) continue;
            free.erase(it);
            if (start > at) free[at] = start - at;
            if (start + bytes < at + size) free[start + bytes] = at + size - (start + bytes);
            offset = start;
            return true;
        }
        return false;
    }
    static void GiveRange(std::map<VkDeviceSize, VkDeviceSize>& free, VkDeviceSize at, VkDeviceSize bytes) {
        auto next = free.lower_bound(at);
        if (next != free.end() && next->first == at + bytes) { bytes += next->second; next = free.erase(next); }
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) { at = prev->first; bytes += prev->second; free.erase(prev); }
        }
        free[at] = bytes;
    }
    // Binds `tex.image` into a block; false (nothing bound) when it does not fit one, and the caller allocates it alone.
    bool BindStreamedImage(Texture& tex) {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, tex.image, &req);
        const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX || req.size > kImageBlockBytes) return false;
        for (uint32_t b = 0; b < (uint32_t)imageBlocks_.size(); ++b) {
            if (imageBlocks_[b].type != type || !TakeRange(imageBlocks_[b].free, req.size, req.alignment, tex.blockOffset)) continue;
            if (vkBindImageMemory(device_, tex.image, imageBlocks_[b].memory, tex.blockOffset) != VK_SUCCESS) { GiveRange(imageBlocks_[b].free, tex.blockOffset, req.size); return false; }
            tex.block = b;
            tex.blockBytes = req.size;
            return true;
        }
        ImageBlock nb;
        nb.type = type;
        if (spareImageBlock_.valid() && spareImageType_ == type && spareImageBlock_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) nb.memory = spareImageBlock_.get();
        if (!nb.memory) nb.memory = MakeImageMemory(type);
        if (!nb.memory) return false;
        StartSpareImageBlock(type);
        nb.free[0] = kImageBlockBytes;
        imageBlocks_.push_back(nb);
        return BindStreamedImage(tex);
    }

    bool CreateMaterialResources() {
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * kMaterialSets}, {VK_DESCRIPTOR_TYPE_SAMPLER, kMaterialSets}};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = kMaterialSets;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &materialPool_));
        const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, surface[4] = {128, 255, 255, 255}, extra[4] = {255, 255, 255, 0};
        defaults_[0] = CreateTexture(white, 1, 1);
        defaults_[1] = CreateTexture(flat, 1, 1);
        defaults_[2] = CreateTexture(surface, 1, 1);
        defaults_[3] = CreateTexture(extra, 1, 1);
        return defaults_[0] != kNoTexture && defaults_[1] != kNoTexture && defaults_[2] != kNoTexture && defaults_[3] != kNoTexture;
    }

    // One descriptor set per (albedo, normal, surface) triple, made on first use and kept until a texture in it is destroyed.
    VkDescriptorSet MaterialSet(TextureId albedo, TextureId normal, TextureId surface) {
        if (albedo >= textures_.size() || !textures_[albedo].image) albedo = defaults_[0];
        if (normal >= textures_.size() || !textures_[normal].image) normal = defaults_[1];
        if (surface >= textures_.size() || !textures_[surface].image) surface = defaults_[2];
        TextureId extra = ExtraOf(surface);
        if (extra >= textures_.size() || !textures_[extra].image) extra = defaults_[3];
        const uint64_t key = ((uint64_t)albedo << 42) | ((uint64_t)normal << 21) | surface;
        const auto found = materialSets_.find(key);
        if (found != materialSets_.end()) return found->second;
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = materialPool_;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &materialSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &da, &set) != VK_SUCCESS) {
            Log("Vulkan: out of material descriptor sets");
            return VK_NULL_HANDLE;
        }
        const VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, textures_[albedo].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                {VK_NULL_HANDLE, textures_[normal].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 2;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos;
        const VkDescriptorImageInfo surfaceInfo = {VK_NULL_HANDLE, textures_[surface].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet ws = w;
        ws.dstBinding = 3;
        ws.descriptorCount = 1;
        ws.pImageInfo = &surfaceInfo;
        const VkDescriptorImageInfo extraInfo = {VK_NULL_HANDLE, textures_[extra].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet we = ws;
        we.dstBinding = 4;
        we.pImageInfo = &extraInfo;
        const VkWriteDescriptorSet writes[3] = {w, ws, we};
        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
        materialSets_[key] = set;
        return set;
    }

    // ------------------------------------------------------------------ HDR scene pipeline
    enum PostTarget : uint32_t { kScene, kAo, kClouds, kComposed, kHistory0, kHistory1, kProbes, kProbeRaysTarget, kExposure, kBloom, kCloudShadow, kVelocity, kClouds1, kCloudMarch, kCapture, kPostTargets };
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // xy motion, z skin's surface reflection (post.hlsl ScatterSkin), w spare
    static constexpr uint32_t kMaterialSets = 16384; // one per distinct texture triple, cached for the renderer's life

    void ColorToRead(VkImage image) {
        ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // Keeps what the attachment holds; the flipped viewport makes uv and clip space match Direct3D.
    void BeginColorPass(VkImageView view, VkImageView depthView, uint32_t w, uint32_t h, bool clearDepth = false) {
        VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reversed Z
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        if (depthView) ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport vp = {0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {w, h}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void PostPass(uint32_t target, VkPipeline pipeline, uint32_t w, uint32_t h) {
        ImageBarrier(post_[target].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        BeginColorPass(post_[target].layerViews[0], VK_NULL_HANDLE, w, h);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRendering(cmd_);
        ColorToRead(post_[target].image);
    }



    void DestroyPostTargets() {
        for (StoreImage& img : post_) DestroyStoreImage(img);
        DestroyStoreImage(waterRefract_);
    }

    // Render-sized targets, resting in the shader-read layout, and the two descriptor sets over them.
    bool CreatePostTargets() {
        DestroyPostTargets();
        ++captureEpoch_;
        const uint32_t w = sceneExtent_.width, h = sceneExtent_.height;
        for (uint32_t t = 0; t < kPostTargets; ++t)
            if (!CreateStoreImage(t == kAo ? VK_FORMAT_R8_UNORM : t == kVelocity ? kVelocityFormat : kHdrFormat,
                                  t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, w / 2) : t == kCloudMarch ? std::max(1u, w / 4) : t == kProbes ? kProbeX * 3 : t == kProbeRaysTarget ? kProbeRays : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, w / 4) : w,
                                  t == kAo || t == kClouds || t == kClouds1 ? std::max(1u, h / 2) : t == kCloudMarch ? std::max(1u, h / 4) : t == kProbes ? kProbeAtlasH : t == kProbeRaysTarget ? kProbesPerFrame : t == kExposure ? 1 : t == kCloudShadow ? kCloudShadowSize : t == kCapture ? kCaptureSize : t == kBloom ? std::max(1u, h / 4) : h, 1,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, post_[t]))
                return false;
        if (!CreateStoreImage(kHdrFormat, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, waterRefract_)) return false;
        const bool ok = Immediate([&] {
            for (StoreImage& img : post_)
                ImageBarrier(img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            ImageBarrier(waterRefract_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) return false;
        for (uint32_t set = 0; set < 2; ++set) {
            const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const VkDescriptorImageInfo images[8] = {
                {VK_NULL_HANDLE, post_[kScene].layerViews[0], read},
                {VK_NULL_HANDLE, depthView_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, post_[kAo].layerViews[0], read},
                {VK_NULL_HANDLE, post_[set ? kClouds1 : kClouds].layerViews[0], read},
                {VK_NULL_HANDLE, cloudNoise_.view, read},
                {VK_NULL_HANDLE, post_[kComposed].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kHistory0 + (1 - set)].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kHistory0 + set].layerViews[0], read}};
            const VkDescriptorImageInfo moreImages[6] = { // bindings 11-16
                {VK_NULL_HANDLE, shadow_.layerViews[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, post_[kProbes].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kProbeRaysTarget].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kExposure].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kBloom].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kCloudShadow].layerViews[0], read}};
            VkWriteDescriptorSet writes[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            const VkDescriptorImageInfo velocityImage[5] = { // bindings 17-21
                {VK_NULL_HANDLE, post_[kVelocity].layerViews[0], read},
                {VK_NULL_HANDLE, cloudField_.view, read},
                {VK_NULL_HANDLE, post_[set ? kClouds : kClouds1].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kCloudMarch].layerViews[0], read},
                {VK_NULL_HANDLE, post_[kCapture].layerViews[0], read}};
            writes[3].dstBinding = 17;
            writes[3].descriptorCount = 5;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[3].pImageInfo = velocityImage;
            writes[2].dstBinding = 11;
            writes[2].descriptorCount = 6;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[2].pImageInfo = moreImages;
            writes[1].dstSet = postSets_[set];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 8;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[1].pImageInfo = images;
            for (uint32_t f = 0; f < kFramesInFlight; ++f) {
                const VkDescriptorBufferInfo ubo = {frames_[f].postBuffer.buffer, 0, VK_WHOLE_SIZE};
                const uint32_t index = set * kFramesInFlight + f;
                writes[0].dstSet = postSets_[index];
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &ubo;
                writes[1].dstSet = postSets_[index];
                writes[2].dstSet = postSets_[index];
                writes[3].dstSet = postSets_[index];
                vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
            }
        }
        const VkDescriptorImageInfo probeInfo = {VK_NULL_HANDLE, post_[kProbes].layerViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo cloudInfo = {VK_NULL_HANDLE, post_[kCloudShadow].layerViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (Frame& frame : frames_) {
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = frame.lightSet;
            w.dstBinding = 4;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &probeInfo;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            w.dstBinding = 5;
            w.pImageInfo = &cloudInfo;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        probeAge_ = 0;
        cloudMapValid_ = false;
        historyValid_ = false;
        return true;
    }

    bool CreateCloudField() {
        constexpr uint32_t kN = kCloudFieldSize;
        const std::vector<uint8_t> texels = GenerateCloudField();
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {kN, kN, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!AllocateImage(ii, cloudField_.image, cloudField_.memory)) return false;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = cloudField_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &cloudField_.view));
        Buffer staging;
        if (!CreateBuffer(texels.size(), staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
        std::memcpy(staging.mapped, texels.data(), texels.size());
        const bool ok = Immediate([&] {
            ImageBarrier(cloudField_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kN, kN, 1};
            vkCmdCopyBufferToImage(cmd_, staging.buffer, cloudField_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            ImageBarrier(cloudField_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        DestroyBuffer(staging);
        return ok;
    }

    bool CreateCloudNoise() {
        constexpr uint32_t kN = kCloudNoiseSize;
        const std::vector<uint8_t> voxels = GenerateCloudNoise();
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_3D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {kN, kN, kN};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!AllocateImage(ii, cloudNoise_.image, cloudNoise_.memory)) return false;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = cloudNoise_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &cloudNoise_.view));
        Buffer staging;
        if (!CreateBuffer(voxels.size(), staging, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
        std::memcpy(staging.mapped, voxels.data(), voxels.size());
        const bool ok = Immediate([&] {
            ImageBarrier(cloudNoise_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kN, kN, kN};
            vkCmdCopyBufferToImage(cmd_, staging.buffer, cloudNoise_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            ImageBarrier(cloudNoise_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        DestroyBuffer(staging);
        return ok;
    }

    bool CreatePostPipelines() {
        for (Frame& frame : frames_)
            if (!CreateBuffer(2048, frame.postBuffer, true, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
        for (int i = 0; i < 2; ++i) {
            VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.addressModeU = si.addressModeV = si.addressModeW = i ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = VK_LOD_CLAMP_NONE;
            VK_CHECK(vkCreateSampler(device_, &si, nullptr, &postSamplers_[i]));
        }
        // Binding 0: parameters; 1-8: textures t0-t7; 9, 10: the immutable samplers s0, s1; 11-16: textures t8-t13; 17: t14, velocity; 18-20: t15-t17, clouds; 21: t18, the lit capture.
        VkDescriptorSetLayoutBinding bindings[22] = {};
        for (uint32_t i = 11; i <= 21; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (uint32_t i = 1; i <= 8; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[9] = {9, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &postSamplers_[0]};
        bindings[10] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &postSamplers_[1]};
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 22;
        sl.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &postSetLayout_));
        const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kFramesInFlight}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 38 * kFramesInFlight}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 2 * kFramesInFlight;
        dp.poolSizeCount = 3;
        dp.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &postPool_));
        VkDescriptorSetLayout setLayouts[2 * kFramesInFlight];
        for (uint32_t i = 0; i < 2 * kFramesInFlight; ++i) setLayouts[i] = postSetLayout_;
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = postPool_;
        da.descriptorSetCount = 2 * kFramesInFlight;
        da.pSetLayouts = setLayouts;
        VK_CHECK(vkAllocateDescriptorSets(device_, &da, postSets_));
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &postSetLayout_;
        VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &postLayout_));

        const VkShaderModule vs = LoadShader(L"post_vs.spv");
        if (!vs) return false;
        const struct { const wchar_t* file; const char* entry; VkFormat format; } passes[11] = {
            {L"post_ao_ps.spv", "AoPS", VK_FORMAT_R8_UNORM},
            {L"post_clouds_ps.spv", "CloudsPS", kHdrFormat},
            {L"post_composite_ps.spv", "CompositePS", kHdrFormat},
            {L"post_taa_ps.spv", "TaaPS", kHdrFormat},
            {L"post_tonemap_ps.spv", "TonemapPS", format_},
            {L"post_probetrace_ps.spv", "ProbeTracePS", kHdrFormat},
            {L"post_probeblend_ps.spv", "ProbeBlendPS", kHdrFormat},
            {L"post_exposure_ps.spv", "ExposurePS", kHdrFormat},
            {L"post_bloom_ps.spv", "BloomPS", kHdrFormat},
            {L"post_cloudshadow_ps.spv", "CloudShadowPS", kHdrFormat},
            {L"post_cloudresolve_ps.spv", "CloudResolvePS", kHdrFormat}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState attachment = {};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
        bool ok = true;
        for (int i = 0; i < 11 && ok; ++i) {
            // The temporal filters blend, with alpha as the new data's weight; RGB only.
            attachment.blendEnable = (i == 6 || i == 7) ? VK_TRUE : VK_FALSE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | ((i == 6 || i == 7) ? 0 : VK_COLOR_COMPONENT_A_BIT);
            const VkShaderModule ps = LoadShader(passes[i].file);
            if (!ps) { ok = false; break; }
            VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vs;
            stages[0].pName = "FullscreenVS";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = ps;
            stages[1].pName = passes[i].entry;
            VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &passes[i].format;
            VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gi.pNext = &rendering;
            gi.stageCount = 2;
            gi.pStages = stages;
            gi.pVertexInputState = &vi;
            gi.pInputAssemblyState = &ia;
            gi.pViewportState = &vps;
            gi.pRasterizationState = &rs;
            gi.pMultisampleState = &ms;
            gi.pDepthStencilState = &ds;
            gi.pColorBlendState = &blend;
            gi.pDynamicState = &dyn;
            gi.layout = postLayout_;
            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &postPipelines_[i]) == VK_SUCCESS;
            vkDestroyShaderModule(device_, ps, nullptr);
        }
        vkDestroyShaderModule(device_, vs, nullptr);
        return ok;
    }

    bool PrepareCapture() {
        const size_t bytes = (size_t)extent_.width * extent_.height * 4;
        if (captureBuffer_.buffer && captureBytes_ == bytes) return true;
        if (captureBuffer_.buffer) retired_.push_back(captureBuffer_);
        captureBuffer_ = {};
        captureBytes_ = bytes;
        return CreateBuffer(bytes, captureBuffer_, true, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    bool CreateTimers() {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kTimerMarks;
        for (Frame& frame : frames_)
            VK_CHECK(vkCreateQueryPool(device_, &qi, nullptr, &frame.timerPool));
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys_, &props);
        timerPeriodNs_ = props.limits.timestampPeriod;
        return true;
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

    void Mark(uint32_t slot) { vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frame_->timerPool, slot); }

    void ReadTimer(Frame& frame) {
        if (!frame.timerPending) return;
        frame.timerPending = false;
        uint64_t t[kTimerMarks];
        if (vkGetQueryPoolResults(device_, frame.timerPool, 0, kTimerMarks, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        AddTimerSample(t, timerPeriodNs_ / 1e6);
        if (++timerCount_ == 60) {
            LogTimers("Vulkan", extent_.width, extent_.height);
            LogCounts("Vulkan");
            timerCount_ = 0;
        }
    }

    struct Mesh {
        Buffer buffer;
        uint32_t count; // indices when indexed, vertices otherwise
        Topology topology;
        VkDeviceSize indexOffset;
        bool indexed;
        VkDeviceSize copyBytes = 0; // deformable: bytes of one frame's vertices (0 = not deformable)
        bool sharesBuffer = false;  // a part of a deformable mesh: its parent owns the buffer
        Buffer upload = {};         // deformable parent: the staging copies, staying mapped
        uint32_t newestCopy = 0;    // deformable parent: which of the two device-local copies is this frame's
        MeshId copyParent = kInvalidMesh; // a part: the mesh holding newestCopy
        VkDeviceSize base = 0;            // where its vertices start in `buffer` (streamed: a range of meshBlocks_[block])
        uint32_t block = UINT32_MAX;
        VkDeviceSize blockBytes = 0;
    };
    uint32_t NewestCopy(const Mesh& mesh) const { return mesh.copyParent != kInvalidMesh ? meshes_[mesh.copyParent].newestCopy : mesh.newestCopy; }
    std::vector<MeshId> pendingDeformables_;

    MeshId PlaceMesh(const Mesh& mesh) {
        if (!freeSlots_.empty()) {
            const MeshId id = freeSlots_.back();
            freeSlots_.pop_back();
            meshes_[id] = mesh;
            return id;
        }
        meshes_.push_back(mesh);
        return (MeshId)(meshes_.size() - 1);
    }

    bool CreateInstance(bool validation) {
        std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        std::vector<const char*> layers;
        if (validation) {
            uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> props(n);
            vkEnumerateInstanceLayerProperties(&n, props.data());
            static const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation"; // must outlive `props`
            for (const auto& p : props)
                if (std::strcmp(p.layerName, kValidationLayer) == 0) layers.push_back(kValidationLayer);
            if (layers.empty()) Log("Vulkan: validation layer not found");
            else extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "TBD Engine";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (uint32_t)extensions.size();
        ci.ppEnabledExtensionNames = extensions.data();
        ci.enabledLayerCount = (uint32_t)layers.size();
        ci.ppEnabledLayerNames = layers.data();
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

        if (!layers.empty()) {
            auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
            VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = DebugCallback;
            if (create) create(instance_, &mi, nullptr, &messenger_);
            Log("Vulkan: validation enabled");
        }
        return true;
    }

    bool PickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(instance_, &n, devices.data());

        int bestScore = -1;
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> families(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, families.data());
            for (uint32_t i = 0; i < qn; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore) {
                    bestScore = score;
                    phys_ = d;
                    family_ = i;
                }
                break;
            }
        }
        if (bestScore < 0) {
            Log("Vulkan: no GPU with Vulkan 1.3 and presentation support");
            return false;
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys_, &props);
        Log("Vulkan: device %s (API %u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion));
        return true;
    }

    bool CreateDevice() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f13.shaderDemoteToHelperInvocation = VK_TRUE; // the alpha cutout's clip() compiles to demote
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures supported;
        vkGetPhysicalDeviceFeatures(phys_, &supported);
        VkPhysicalDeviceFeatures enabled = {};
        enabled.samplerAnisotropy = supported.samplerAnisotropy;
        anisotropy_ = supported.samplerAnisotropy == VK_TRUE;
        VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &f13;
        di.pEnabledFeatures = &enabled;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = extensions;
        VK_CHECK(vkCreateDevice(phys_, &di, nullptr, &device_));
        vkGetDeviceQueue(device_, family_, 0, &queue_);
        return true;
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags wanted) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return UINT32_MAX;
    }

    // Host-visible, host-coherent, persistently mapped (fine for this renderer's small, CPU-written data).
    bool CreateBuffer(size_t bytes, Buffer& out, bool mapped, VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &out.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, out.buffer, &req);
        const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &out.memory));
        VK_CHECK(vkBindBufferMemory(device_, out.buffer, out.memory, 0));
        if (mapped) VK_CHECK(vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
        return true;
    }

    bool CreateDeviceBuffer(size_t bytes, Buffer& out, VkBufferUsageFlags usage) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &out.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, out.buffer, &req);
        const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &out.memory));
        VK_CHECK(vkBindBufferMemory(device_, out.buffer, out.memory, 0));
        return true;
    }

    void DestroyBuffer(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = {};
    }

    void DestroySwapchainViews() {
        for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
        views_.clear();
        for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
        renderDone_.clear();
        DestroyDepth();
    }

    void DestroyDepth() {
        if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
        if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
        depthView_ = VK_NULL_HANDLE;
        depthImage_ = VK_NULL_HANDLE;
        depthMemory_ = VK_NULL_HANDLE;
        if (overlayDepthView_) vkDestroyImageView(device_, overlayDepthView_, nullptr);
        if (overlayDepthImage_) vkDestroyImage(device_, overlayDepthImage_, nullptr);
        if (overlayDepthMemory_) vkFreeMemory(device_, overlayDepthMemory_, nullptr);
        overlayDepthView_ = VK_NULL_HANDLE;
        overlayDepthImage_ = VK_NULL_HANDLE;
        overlayDepthMemory_ = VK_NULL_HANDLE;
    }

    // (Re)creates the swapchain, its views, the depth buffer and the per-image semaphores.
    bool CreateSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(w_, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(h_, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return false; // minimized

        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
        if (format_ != VK_FORMAT_UNDEFINED && chosen.format != format_) Log("Vulkan: surface format changed");
        format_ = chosen.format;

        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // frame capture copies from it
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync
        if (GetEnvironmentVariableA("TBD_NOVSYNC", nullptr, 0) > 0) { // for measuring frame cost
            uint32_t modeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &modeCount, nullptr);
            std::vector<VkPresentModeKHR> modes(modeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &modeCount, modes.data());
            for (VkPresentModeKHR m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) ci.presentMode = m;
        }
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain_;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &created));

        DestroySwapchainViews();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = created;
        extent_ = extent;
        sceneExtent_ = {(uint32_t)ScaledSize((int)extent.width, scale_), (uint32_t)ScaledSize((int)extent.height, scale_)};

        uint32_t in = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &in, nullptr);
        images_.resize(in);
        vkGetSwapchainImagesKHR(device_, swapchain_, &in, images_.data());
        for (VkImage image : images_) {
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &view));
            views_.push_back(view);
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore sem;
            VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &sem));
            renderDone_.push_back(sem);
        }

        return CreateDepth();
    }

    // The scene's depth buffer at the render size and, while the scale is below 1, a window-sized one for the overlays.
    bool CreateDepth() {
        DestroyDepth();
        return CreateDepthImage(sceneExtent_, depthImage_, depthMemory_, depthView_) &&
               (scale_ >= 1.0f || CreateDepthImage(extent_, overlayDepthImage_, overlayDepthMemory_, overlayDepthView_));
    }

    bool CreateDepthImage(VkExtent2D extent, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo di = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        di.imageType = VK_IMAGE_TYPE_2D;
        di.format = kDepthFormat;
        di.extent = {extent.width, extent.height, 1};
        di.mipLevels = 1;
        di.arrayLayers = 1;
        di.samples = VK_SAMPLE_COUNT_1_BIT;
        di.tiling = VK_IMAGE_TILING_OPTIMAL;
        di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &di, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(device_, image, memory, 0));
        VkImageViewCreateInfo dv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        dv.image = image;
        dv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dv.format = kDepthFormat;
        dv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &dv, nullptr, &view));
        return true;
    }

    bool CreateFrameObjects() {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = family_;
        VK_CHECK(vkCreateCommandPool(device_, &pi, nullptr, &pool_));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kFramesInFlight;
        VkCommandBuffer commands[kFramesInFlight] = {};
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, commands));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            frames_[i].cmd = commands[i];
            VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &frames_[i].imageAvailable));
            VK_CHECK(vkCreateFence(device_, &fi, nullptr, &frames_[i].fence));
        }
        cmd_ = frames_[0].cmd;
        return true;
    }

    VkShaderModule LoadShader(const wchar_t* name) {
        std::vector<uint32_t> words;
        const std::wstring path = ExeDirectory() + L"\\shaders\\" + name;
        if (!ReadFile(path, words)) {
            Log("Vulkan: cannot read shader %ls (run build.bat with the Vulkan SDK installed)", name);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = words.size() * 4;
        mi.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &mi, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
        return module;
    }

    bool CreatePipelines() {
        vs_ = LoadShader(L"mesh_vs.spv");
        ps_ = LoadShader(L"mesh_ps.spv");
        overlayPs_ = LoadShader(L"mesh_overlay_ps.spv");
        if (!vs_ || !ps_ || !overlayPs_) return false;

        // The sun's shadow map, a depth image sampled with a comparison sampler (see lighting.hlsli).
        if (!CreateStoreImage(kDepthFormat, kShadowMapSize, kShadowMapSize, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, shadow_))
            return false;
        if (!CreateStoreImage(kDepthFormat, kShadowMapSize, kShadowMapSize, 1,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, sunCache_))
            return false;
        // The tight character map: the same kind of image, covering a few meters around the camera's focus.
        if (!CreateStoreImage(kDepthFormat, kTightShadowMapSize, kTightShadowMapSize, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, tightShadow_))
            return false;
        VkSamplerCreateInfo shadowSi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        shadowSi.magFilter = shadowSi.minFilter = VK_FILTER_LINEAR;
        shadowSi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        shadowSi.addressModeU = shadowSi.addressModeV = shadowSi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSi.compareEnable = VK_TRUE;
        shadowSi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        shadowSi.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &shadowSi, nullptr, &shadowSampler_));

        // Set 0: binding 0 the per-frame lighting uniform buffer, 1 the shadow map, 2 its (immutable) sampler, 3 the tight map.
        const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding lightBindings[7] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &shadowSampler_},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 7;
        sl.pBindings = lightBindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &lightSetLayout_));
        const VkDescriptorPoolSize lightPoolSizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kFramesInFlight},
                                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * kFramesInFlight},
                                                        {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight}};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = kFramesInFlight;
        dp.poolSizeCount = 3;
        dp.pPoolSizes = lightPoolSizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &lightPool_));
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = lightPool_;
        VkDescriptorSetLayout lightLayouts[kFramesInFlight];
        VkDescriptorSet lightSets[kFramesInFlight];
        for (uint32_t i = 0; i < kFramesInFlight; ++i) lightLayouts[i] = lightSetLayout_;
        da.descriptorSetCount = kFramesInFlight;
        da.pSetLayouts = lightLayouts;
        VK_CHECK(vkAllocateDescriptorSets(device_, &da, lightSets));
        const VkDescriptorImageInfo shadowInfo = {VK_NULL_HANDLE, shadow_.layerViews[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo tightInfo = {VK_NULL_HANDLE, tightShadow_.layerViews[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            frames_[i].lightSet = lightSets[i];
            if (!CreateBuffer(kLightBufferSize, frames_[i].lightBuffer, true, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
            const VkDescriptorBufferInfo lightInfo = {frames_[i].lightBuffer.buffer, 0, kLightBufferSize};
            VkWriteDescriptorSet lightWrites[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                                   {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            lightWrites[0].dstSet = lightSets[i];
            lightWrites[0].dstBinding = 0;
            lightWrites[0].descriptorCount = 1;
            lightWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            lightWrites[0].pBufferInfo = &lightInfo;
            lightWrites[1].dstSet = lightSets[i];
            lightWrites[1].dstBinding = 1;
            lightWrites[1].descriptorCount = 1;
            lightWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            lightWrites[1].pImageInfo = &shadowInfo;
            lightWrites[2].dstSet = lightSets[i];
            lightWrites[2].dstBinding = 3;
            lightWrites[2].descriptorCount = 1;
            lightWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            lightWrites[2].pImageInfo = &tightInfo;
            vkUpdateDescriptorSets(device_, 3, lightWrites, 0, nullptr);
            if (!CreateBuffer(256, motionBuffers_[i], true, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
            std::memset(motionBuffers_[i].mapped, 0, 256);
            const VkDescriptorBufferInfo motionInfo = {motionBuffers_[i].buffer, 0, 256};
            VkWriteDescriptorSet motionWrite = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            motionWrite.dstSet = lightSets[i];
            motionWrite.dstBinding = 6;
            motionWrite.descriptorCount = 1;
            motionWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            motionWrite.pBufferInfo = &motionInfo;
            vkUpdateDescriptorSets(device_, 1, &motionWrite, 0, nullptr);
        }

        // The matrices for the vertex stage, then the material's constants (materialConstants_) for the fragment stage at 128:
        // 144 bytes in all, over the 128 every device guarantees, so the device's limit is checked.
        const VkPushConstantRange ranges[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)}, {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_)}};
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(phys_, &props);
            if (props.limits.maxPushConstantsSize < sizeof(PushConstants) + sizeof(materialConstants_)) {
                Log("Vulkan: %u bytes of push constants; the mesh pipelines need %u", props.limits.maxPushConstantsSize, (unsigned)(sizeof(PushConstants) + sizeof(materialConstants_)));
                return false;
            }
        }
        // Set 1: the material's albedo and normal map, and their (immutable) anisotropic sampler.
        VkSamplerCreateInfo texSi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        texSi.magFilter = texSi.minFilter = VK_FILTER_LINEAR;
        texSi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        texSi.addressModeU = texSi.addressModeV = texSi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        texSi.anisotropyEnable = anisotropy_ ? VK_TRUE : VK_FALSE;
        texSi.maxAnisotropy = 8.0f;
        texSi.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &texSi, nullptr, &materialSampler_));
        const VkDescriptorSetLayoutBinding materialBindings[5] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &materialSampler_},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo ml = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ml.bindingCount = 5;
        ml.pBindings = materialBindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ml, nullptr, &materialSetLayout_));

        const VkDescriptorSetLayout meshSets[2] = {lightSetLayout_, materialSetLayout_};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 2;
        li.pSetLayouts = meshSets;
        li.pushConstantRangeCount = 2;
        li.pPushConstantRanges = ranges;
        VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &layout_));

        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_;
        stages[0].pName = "VSMain";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps_;
        stages[1].pName = "PSMain";

        VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24},
                                                      {3, 0, VK_FORMAT_R32G32_SFLOAT, 40}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attrs;

        // The instanced pipelines add a per-instance stream: the three rows of a float3x4 model matrix, after the vertex attributes.
        const VkVertexInputBindingDescription instancedBindings[2] = {binding, {1, kInstanceStride, VK_VERTEX_INPUT_RATE_INSTANCE}};
        const VkVertexInputAttributeDescription instancedAttrs[7] = {attrs[0], attrs[1], attrs[2], attrs[3],
                                                                     {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                                     {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                                                                     {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32}};
        VkPipelineVertexInputStateCreateInfo instancedVi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        instancedVi.vertexBindingDescriptionCount = 2;
        instancedVi.pVertexBindingDescriptions = instancedBindings;
        instancedVi.vertexAttributeDescriptionCount = 7;
        instancedVi.pVertexAttributeDescriptions = instancedAttrs;

                VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachments[2] = {}; // the velocity target stays untouched unless a pipeline says otherwise
        blendAttachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = blendAttachments;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.depthAttachmentFormat = kDepthFormat;

        for (int i = 0; i < 8; ++i) { // 0-3 draw into the HDR scene, 4-7 are the same states for display-space overlays
            const bool lines = (i & 3) >= 2;
            const VkFormat colorFormats[2] = {i < 4 ? kHdrFormat : format_, kVelocityFormat};
            rendering.colorAttachmentCount = i < 4 ? 2 : 1;
            blend.attachmentCount = i < 4 ? 2 : 1;
            rendering.pColorAttachmentFormats = colorFormats;
            stages[1].module = i < 4 ? ps_ : overlayPs_;
            stages[1].pName = i < 4 ? "PSMain" : "PSOverlay";
            const bool depth = (i % 2) == 0;
            VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
            ds.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp = VK_COMPARE_OP_GREATER;

            VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gi.pNext = &rendering;
            gi.stageCount = 2;
            gi.pStages = stages;
            gi.pVertexInputState = &vi;
            gi.pInputAssemblyState = &ia;
            gi.pViewportState = &vps;
            gi.pRasterizationState = &rs;
            gi.pMultisampleState = &ms;
            gi.pDepthStencilState = &ds;
            gi.pColorBlendState = &blend;
            gi.pDynamicState = &dyn;
            gi.layout = layout_;
            VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &pipelines_[i]));
            if (i == 0) { // instanced draws: the HDR scene's lit triangles, with the second vertex stream
                instancedVs_ = LoadShader(L"mesh_inst_vs.spv");
                if (!instancedVs_) return false;
                VkPipelineShaderStageCreateInfo instancedStages[2] = {stages[0], stages[1]};
                instancedStages[0].module = instancedVs_;
                instancedStages[0].pName = "VSInstanced";
                VkGraphicsPipelineCreateInfo igi = gi;
                igi.pStages = instancedStages;
                igi.pVertexInputState = &instancedVi;
                VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &igi, nullptr, &instancedPipeline_));
                { // The depth pre-pass (SetScenePrepass): the same vertex shaders, so depth matches to the bit. Depth only: the vertex
                  // stage alone, nothing written to the targets. Then shading: depth equal (GREATER_OR_EQUAL, reversed z), no depth write.
                    VkPipelineColorBlendAttachmentState none[2] = {};
                    VkPipelineColorBlendStateCreateInfo noColor = blend;
                    noColor.pAttachments = none;
                    VkPipelineDepthStencilStateCreateInfo equal = ds;
                    equal.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
                    equal.depthWriteEnable = VK_FALSE;
                    for (int v = 0; v < 2; ++v) {
                        VkGraphicsPipelineCreateInfo depthOnly = v ? igi : gi, shade = v ? igi : gi;
                        depthOnly.stageCount = 1;
                        depthOnly.pColorBlendState = &noColor;
                        shade.pDepthStencilState = &equal;
                        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &depthOnly, nullptr, &prepassPipelines_[v][0]));
                        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &shade, nullptr, &prepassPipelines_[v][1]));
                    }
                }
                // Optional: without them the city simply has no chunk volumes (BeginChunkBake says false).
                if (!CreateChunkPipelines(gi, instancedVi)) Log("Vulkan: chunk bake / volume pipelines unavailable (mesh_chunk_*.spv missing or failed)");
                VkShaderModule vegetationVs = LoadShader(L"mesh_veg_vs.spv");
                if (!vegetationVs) return false;
                instancedStages[0].module = vegetationVs;
                instancedStages[0].pName = "VSVegetation";
                VkShaderModule motionPs = LoadShader(L"mesh_motion_ps.spv"), skinnedVs = LoadShader(L"mesh_skinned_motion_vs.spv");
                if (!motionPs || !skinnedVs) return false;
                instancedStages[1].module = motionPs;
                instancedStages[1].pName = "PSMainMotion";
                blendAttachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &igi, nullptr, &vegetationPipeline_));
                {
                    VkShaderModule depthVs = LoadShader(L"mesh_veg_depth_vs.spv"), depthPs = LoadShader(L"mesh_veg_depth_ps.spv"), shadePs = LoadShader(L"mesh_veg_shade_ps.spv");
                    if (!depthVs || !depthPs || !shadePs) return false;
                    VkPipelineShaderStageCreateInfo passStages[2] = {instancedStages[0], instancedStages[1]};
                    VkGraphicsPipelineCreateInfo pgi = igi;
                    pgi.pStages = passStages;
                    passStages[0].module = depthVs; passStages[0].pName = "VSVegetationDepth";
                    passStages[1].module = depthPs; passStages[1].pName = "PSVegetationDepth";
                    VkPipelineColorBlendAttachmentState noWrites[2] = {};
                    VkPipelineColorBlendStateCreateInfo noBlend = blend;
                    noBlend.pAttachments = noWrites;
                    pgi.pColorBlendState = &noBlend;
                    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pgi, nullptr, &vegetationDepthPipeline_));
                    passStages[0] = instancedStages[0];
                    passStages[1].module = shadePs; passStages[1].pName = "PSVegetationShade";
                    pgi.pColorBlendState = &blend;
                    VkPipelineDepthStencilStateCreateInfo shadeDs = ds;
                    shadeDs.depthWriteEnable = VK_FALSE;
                    shadeDs.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
                    pgi.pDepthStencilState = &shadeDs;
                    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pgi, nullptr, &vegetationShadePipeline_));
                    vkDestroyShaderModule(device_, depthVs, nullptr);
                    vkDestroyShaderModule(device_, depthPs, nullptr);
                    vkDestroyShaderModule(device_, shadePs, nullptr);
                }
                const VkVertexInputBindingDescription skinnedBindings[2] = {binding, {1, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
                const VkVertexInputAttributeDescription skinnedAttrs[5] = {attrs[0], attrs[1], attrs[2], attrs[3], {4, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}};
                VkPipelineVertexInputStateCreateInfo skinnedVi = instancedVi;
                skinnedVi.pVertexBindingDescriptions = skinnedBindings;
                skinnedVi.vertexAttributeDescriptionCount = 5;
                skinnedVi.pVertexAttributeDescriptions = skinnedAttrs;
                igi.pVertexInputState = &skinnedVi;
                instancedStages[0].module = skinnedVs;
                instancedStages[0].pName = "VSSkinnedMotion";
                VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &igi, nullptr, &skinnedMotionPipeline_));
                blendAttachments[1].colorWriteMask = 0;
                vkDestroyShaderModule(device_, skinnedVs, nullptr);
                vkDestroyShaderModule(device_, motionPs, nullptr);
                vkDestroyShaderModule(device_, vegetationVs, nullptr);
            }
        }

        // The shadow pass: depth only, with a depth bias against shadow acne.
        shadowVs_ = LoadShader(L"mesh_shadow_vs.spv");
        if (!shadowVs_) return false;
        VkPipelineShaderStageCreateInfo shadowStage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shadowStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        shadowStage.module = shadowVs_;
        shadowStage.pName = "ShadowVS";
        VkPipelineRasterizationStateCreateInfo shadowRs = rs;
        shadowRs.depthBiasEnable = VK_TRUE;
        shadowRs.depthBiasConstantFactor = kShadowDepthBias;
        shadowRs.depthBiasSlopeFactor = kShadowSlopeBias;
        VkPipelineInputAssemblyStateCreateInfo shadowIa = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        shadowIa.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineDepthStencilStateCreateInfo shadowDs = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        shadowDs.depthTestEnable = VK_TRUE;
        shadowDs.depthWriteEnable = VK_TRUE;
        shadowDs.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo shadowBlend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineRenderingCreateInfo shadowRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        shadowRendering.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo sgi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        sgi.pNext = &shadowRendering;
        sgi.stageCount = 1;
        sgi.pStages = &shadowStage;
        VkPipelineVertexInputStateCreateInfo shadowVi = vi; // positions only; the depth pass reads nothing else
        shadowVi.vertexAttributeDescriptionCount = 1;
        sgi.pVertexInputState = &shadowVi;
        sgi.pInputAssemblyState = &shadowIa;
        sgi.pViewportState = &vps;
        sgi.pRasterizationState = &shadowRs;
        sgi.pMultisampleState = &ms;
        sgi.pDepthStencilState = &shadowDs;
        sgi.pColorBlendState = &shadowBlend;
        sgi.pDynamicState = &dyn;
        sgi.layout = layout_;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &shadowPipeline_));
        // The tight character map's pass: the same pipeline with the bias suited to its short depth range.
        shadowRs.depthBiasConstantFactor = kTightDepthBias;
        shadowRs.depthBiasSlopeFactor = kTightSlopeBias;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &tightPipeline_));
        // The same two, for instanced casters.
        shadowInstancedVs_ = LoadShader(L"mesh_shadow_inst_vs.spv");
        if (!shadowInstancedVs_) return false;
        shadowStage.module = shadowInstancedVs_;
        shadowStage.pName = "ShadowInstancedVS";
        sgi.pVertexInputState = &instancedVi;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &tightInstancedPipeline_));
        shadowRs.depthBiasConstantFactor = kShadowDepthBias;
        shadowRs.depthBiasSlopeFactor = kShadowSlopeBias;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &shadowInstancedPipeline_));
        VkShaderModule vegetationShadowVs = LoadShader(L"mesh_shadow_veg_vs.spv");
        if (!vegetationShadowVs) return false;
        shadowStage.module = vegetationShadowVs;
        shadowStage.pName = "ShadowVegetationVS";
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &vegetationShadowPipeline_));
        vkDestroyShaderModule(device_, vegetationShadowVs, nullptr);
        // Hair cards: alpha-tested depth (DrawShadowCasterCutout), both maps. They read uv and colour: the full vertex input.
        VkShaderModule cutoutVs = LoadShader(L"mesh_shadow_cut_vs.spv"), cutoutPs = LoadShader(L"mesh_shadow_cut_ps.spv");
        if (!cutoutVs || !cutoutPs) return false;
        VkPipelineShaderStageCreateInfo cutoutStages[2] = {shadowStage, shadowStage};
        cutoutStages[0].module = cutoutVs;
        cutoutStages[0].pName = "ShadowCutoutVS";
        cutoutStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        cutoutStages[1].module = cutoutPs;
        cutoutStages[1].pName = "PSShadowCutout";
        sgi.stageCount = 2;
        sgi.pStages = cutoutStages;
        sgi.pVertexInputState = &vi;
        shadowRs.depthBiasConstantFactor = kShadowDepthBias;
        shadowRs.depthBiasSlopeFactor = kShadowSlopeBias;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &shadowCutoutPipeline_));
        shadowRs.depthBiasConstantFactor = kTightDepthBias;
        shadowRs.depthBiasSlopeFactor = kTightSlopeBias;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sgi, nullptr, &tightCutoutPipeline_));
        vkDestroyShaderModule(device_, cutoutVs, nullptr);
        vkDestroyShaderModule(device_, cutoutPs, nullptr);

        // The shadow maps rest in the read-only layout that the descriptors expect.
        VK_CHECK(vkResetCommandBuffer(cmd_, 0));
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &cbi));
        ImageBarrier(sunCache_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        ImageBarrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        ImageBarrier(tightShadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VK_CHECK(vkEndCommandBuffer(cmd_));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue_));
        return true;
    }

    // The bake's two pipelines and the volumes' one. `scene` is the HDR scene's lit-triangle pipeline (depth tested, the velocity
    // target untouched), `instancedVi` the instanced vertex input. All or none: a failure leaves none of them.
    bool CreateChunkPipelines(const VkGraphicsPipelineCreateInfo& scene, const VkPipelineVertexInputStateCreateInfo& instancedVi) {
        VkShaderModule bakeVs = LoadShader(L"mesh_chunk_bake_vs.spv"), bakePs = LoadShader(L"mesh_chunk_bake_ps.spv");
        VkShaderModule volVs = LoadShader(L"mesh_chunk_vol_vs.spv"), volPs = LoadShader(L"mesh_chunk_vol_ps.spv");
        bool ok = bakeVs && bakePs && volVs && volPs;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int pass = 0; pass < 2 && ok; ++pass) {
            stages[0].module = bakeVs;
            stages[0].pName = "ChunkBakeVS";
            stages[1].module = bakePs;
            stages[1].pName = "ChunkBakePS";
            VkPipelineColorBlendAttachmentState targets[3] = {};
            const VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            for (int t = 0; t < 3; ++t) targets[t].colorWriteMask = (pass == 0) == (t < 2) ? all : 0;
            if (pass == 1) { // the relief: the nearest start and the farthest end over every surface, so MIN of (d, 1 - d)
                VkPipelineColorBlendAttachmentState& b = targets[2];
                b.blendEnable = VK_TRUE;
                b.srcColorBlendFactor = b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                b.colorBlendOp = VK_BLEND_OP_MIN;
                b.srcAlphaBlendFactor = b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.alphaBlendOp = VK_BLEND_OP_MIN;
                b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
            }
            VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 3;
            blend.pAttachments = targets;
            VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = pass == 0 ? VK_TRUE : VK_FALSE;
            ds.depthWriteEnable = pass == 0 ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineRasterizationStateCreateInfo rs = *scene.pRasterizationState;
            rs.cullMode = VK_CULL_MODE_NONE;
            VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 3;
            rendering.pColorAttachmentFormats = kChunkFormats;
            rendering.depthAttachmentFormat = kDepthFormat;
            VkGraphicsPipelineCreateInfo gi = scene;
            gi.pNext = &rendering;
            gi.pStages = stages;
            gi.pVertexInputState = &instancedVi;
            gi.pRasterizationState = &rs;
            gi.pDepthStencilState = &ds;
            gi.pColorBlendState = &blend;
            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &chunkBakePipelines_[pass]) == VK_SUCCESS;
        }
        if (ok) { // the volumes: the scene's own state, ChunkVolPS writing depth as well
            stages[0].module = volVs;
            stages[0].pName = "ChunkVolVS";
            stages[1].module = volPs;
            stages[1].pName = "ChunkVolPS";
            VkGraphicsPipelineCreateInfo gi = scene;
            gi.pStages = stages;
            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &chunkVolPipeline_) == VK_SUCCESS;
        }
        for (VkShaderModule m : {bakeVs, bakePs, volVs, volPs})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        if (!ok) DestroyChunkPipelines();
        return ok;
    }

    void DestroyChunkPipelines() {
        for (VkPipeline* p : {&chunkBakePipelines_[0], &chunkBakePipelines_[1], &chunkVolPipeline_}) {
            if (*p) vkDestroyPipeline(device_, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    }

    // Compute: one set layout (four read buffers, three written), a descriptor pool per frame in flight.
    struct GpuBuffer {
        Buffer buffer;
        GpuBufferUse use = GpuBufferUse::Read;
        uint64_t bytes = 0;
    };
    std::vector<GpuBuffer> gpuBuffers_;
    std::vector<VkPipeline> computePipelines_;
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool computePools_[kFramesInFlight] = {};
    VkPipeline vegetationPipeline_ = VK_NULL_HANDLE, vegetationShadowPipeline_ = VK_NULL_HANDLE, skinnedMotionPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowCutoutPipeline_ = VK_NULL_HANDLE, tightCutoutPipeline_ = VK_NULL_HANDLE; // hair cards into the shadow maps, alpha tested
    VkPipeline vegetationDepthPipeline_ = VK_NULL_HANDLE, vegetationShadePipeline_ = VK_NULL_HANDLE;
    int vegetationPass_ = 0;
    Buffer motionBuffers_[kFramesInFlight];
    bool forceShadowRebind_ = false;

    // ------------------------------------------------------------------ smooth UI (DrawUi, shaders/ui.hlsl)
    // The same pipeline as the Direct3D 12 side: a 64-byte push constant (the ortho matrix), one set with the SDF font
    // atlas (binding 0) and the trilinear clamp sampler postSamplers_[0] (binding 1), straight-alpha blending, no depth
    // test, into the swapchain image of the overlay phase. Optional: if it fails, UiSupported() is false and the GUI
    // draws the classic way.
    VkDescriptorSetLayout uiSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout uiLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool uiPool_ = VK_NULL_HANDLE;
    VkDescriptorSet uiSet_ = VK_NULL_HANDLE;
    VkPipeline uiPipeline_ = VK_NULL_HANDLE;

    bool CreateUiPipeline() {
        const VkDescriptorSetLayoutBinding bindings[4] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &postSamplers_[0]},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // the sprite atlas (SetUiImages)
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}; // the GUI backdrop: 1x1 here, glass is a plain tint
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 4;
        sl.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device_, &sl, nullptr, &uiSetLayout_) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device_, &dp, nullptr, &uiPool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = uiPool_;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &uiSetLayout_;
        if (vkAllocateDescriptorSets(device_, &da, &uiSet_) != VK_SUCCESS) return false;
        WriteUiAtlas(defaults_[0]); // until SetUiAtlas: white, so the set is never incomplete
        WriteUiImage(2, defaults_[0]);
        WriteUiImage(3, defaults_[0]);
        const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &uiSetLayout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(device_, &li, nullptr, &uiLayout_) != VK_SUCCESS) return false;

        const VkShaderModule vs = LoadShader(L"ui_vs.spv"), ps = LoadShader(L"ui_ps.spv");
        if (!vs || !ps) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            if (ps) vkDestroyShaderModule(device_, ps, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "UiVS";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "UiPS";
        const VkVertexInputBindingDescription binding = {0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attrs[7] = { // render/ui_vertex.h
            {0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, 24}, {4, 0, VK_FORMAT_R32G32_SFLOAT, 32}, {5, 0, VK_FORMAT_R8G8B8A8_UNORM, 40},
            {6, 0, VK_FORMAT_R8G8B8A8_UNORM, 44}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 7;
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; // off
        VkPipelineColorBlendAttachmentState attachment = {};
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format_;
        rendering.depthAttachmentFormat = kDepthFormat; // the overlay pass keeps depth attached; this pipeline ignores it
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &ds;
        gi.pColorBlendState = &blend;
        gi.pDynamicState = &dyn;
        gi.layout = uiLayout_;
        const VkResult made = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &uiPipeline_);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, ps, nullptr);
        if (made != VK_SUCCESS) uiPipeline_ = VK_NULL_HANDLE;
        return made == VK_SUCCESS;
    }

    void WriteUiAtlas(TextureId atlas) { WriteUiImage(0, atlas); }
    void SetUiImages(TextureId atlas) override {
        if (!uiSet_) return;
        vkDeviceWaitIdle(device_);
        WriteUiImage(2, atlas);
    }
    void WriteUiImage(uint32_t binding, TextureId atlas) {
        if (atlas >= textures_.size() || !textures_[atlas].image) atlas = defaults_[0];
        if (atlas >= textures_.size() || !uiSet_) return;
        const VkDescriptorImageInfo image = {VK_NULL_HANDLE, textures_[atlas].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = uiSet_;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    void DestroyUiPipeline() {
        if (uiPipeline_) vkDestroyPipeline(device_, uiPipeline_, nullptr);
        if (uiLayout_) vkDestroyPipelineLayout(device_, uiLayout_, nullptr);
        if (uiPool_) vkDestroyDescriptorPool(device_, uiPool_, nullptr);
        if (uiSetLayout_) vkDestroyDescriptorSetLayout(device_, uiSetLayout_, nullptr);
        uiPipeline_ = VK_NULL_HANDLE, uiLayout_ = VK_NULL_HANDLE, uiPool_ = VK_NULL_HANDLE, uiSetLayout_ = VK_NULL_HANDLE, uiSet_ = VK_NULL_HANDLE;
    }

    bool UiSupported() const override { return uiPipeline_ != VK_NULL_HANDLE; }

    void SetUiAtlas(TextureId atlas) override {
        if (!uiSet_) return;
        vkDeviceWaitIdle(device_); // the set may be in use by a frame in flight
        WriteUiAtlas(atlas);
    }

    void DrawUi(const UiVertex* vertices, uint32_t count, const Mat4& ortho) override {
        if (!frameActive_ || !resolved_ || !uiPipeline_ || count == 0) return;
        const size_t bytes = (size_t)count * sizeof(UiVertex);
        const size_t offset = (used_ + 15) & ~size_t(15);
        if (offset + bytes > kTransientBytes) {
            Log("Vulkan: transient buffer full, UI draw dropped");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(frame_->transient.mapped) + offset, vertices, bytes);
        used_ = offset + bytes;
        const VkDeviceSize vbOffset = offset;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, uiLayout_, 0, 1, &uiSet_, 0, nullptr);
        vkCmdPushConstants(cmd_, uiLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, ortho.m);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &frame_->transient.buffer, &vbOffset);
        vkCmdDraw(cmd_, count, 1, 0, 0);
        BindMeshSets(); // later overlay draws (DrawTransient) use the mesh layout's sets again
    }

    // ------------------------------------------------------------------ water
    WaterDraw water_;
    std::vector<MeshId> waterMeshes_;
    bool waterPending_ = false;
    VkDescriptorSetLayout waterSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout waterLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool waterPool_ = VK_NULL_HANDLE;
    VkDescriptorSet waterSets_[kFramesInFlight] = {};
    VkPipeline waterPipeline_ = VK_NULL_HANDLE;

    bool CreateWaterPipeline() {
        const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding bindings[12] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &postSamplers_[0]},
            {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &shadowSampler_},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},
            {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},
            {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},
            {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 12;
        sl.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &waterSetLayout_));
        const VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kFramesInFlight}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5 * kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * kFramesInFlight}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = kFramesInFlight;
        dp.poolSizeCount = 4;
        dp.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &waterPool_));
        VkDescriptorSetLayout setLayouts[kFramesInFlight];
        for (uint32_t i = 0; i < kFramesInFlight; ++i) setLayouts[i] = waterSetLayout_;
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = waterPool_;
        da.descriptorSetCount = kFramesInFlight;
        da.pSetLayouts = setLayouts;
        VK_CHECK(vkAllocateDescriptorSets(device_, &da, waterSets_));
        const VkPushConstantRange range = {both, 0, 128};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &waterSetLayout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &waterLayout_));

        const VkShaderModule vs = LoadShader(L"water_vs.spv"), ps = LoadShader(L"water_ps.spv");
        if (!vs || !ps) return false;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "WaterVS";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "WaterPS";
        const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24}, {3, 0, VK_FORMAT_R32G32_SFLOAT, 40}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER;
        VkPipelineColorBlendAttachmentState attachments[2] = {};
        attachments[0].colorWriteMask = attachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 2;
        blend.pAttachments = attachments;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
        const VkFormat colorFormats[2] = {kHdrFormat, kVelocityFormat};
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachmentFormats = colorFormats;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &ds;
        gi.pColorBlendState = &blend;
        gi.pDynamicState = &dyn;
        gi.layout = waterLayout_;
        const VkResult made = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &waterPipeline_);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, ps, nullptr);
        return made == VK_SUCCESS;
    }

    // Inside ResolveScene: the composed picture is copied for refraction, then the water draws over it, testing but not writing depth.
    void WaterPass(uint32_t w, uint32_t h, uint32_t cur) {
        waterPending_ = false;
        const VkImage composed = post_[kComposed].image;
        ImageBarrier(composed, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        ImageBarrier(waterRefract_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {w, h, 1};
        vkCmdCopyImage(cmd_, composed, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, waterRefract_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(waterRefract_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        ImageBarrier(composed, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const VkDescriptorSet set = waterSets_[frameSlot_];
        const VkDescriptorBufferInfo buffers[5] = {{frame_->postBuffer.buffer, 0, VK_WHOLE_SIZE}, {frame_->lightBuffer.buffer, 0, kLightBufferSize},
                                                   {gpuBuffers_[water_.waves].buffer.buffer, 0, VK_WHOLE_SIZE}, {gpuBuffers_[water_.ripple].buffer.buffer, 0, VK_WHOLE_SIZE},
                                                   {gpuBuffers_[water_.sky].buffer.buffer, 0, VK_WHOLE_SIZE}};
        const VkDescriptorImageInfo images[5] = {{VK_NULL_HANDLE, depthView_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, waterRefract_.layerViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, shadow_.layerViews[0], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, post_[kProbes].layerViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, post_[kCloudShadow].layerViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[10];
        for (uint32_t i = 0; i < 10; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].descriptorCount = 1;
        }
        const uint32_t bufferBinding[5] = {0, 1, 7, 8, 9};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].dstBinding = bufferBinding[i];
            writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        }
        for (uint32_t i = 0; i < 5; ++i) {
            writes[5 + i].dstBinding = i < 3 ? 2 + i : 7 + i;
            writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[5 + i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, 10, writes, 0, nullptr);

        ImageBarrier(post_[kVelocity].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = post_[kComposed].layerViews[0];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // read only: what the TAA and tone map read next must stay
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        VkRenderingAttachmentInfo colors[2] = {color, color};
        colors[1].imageView = post_[kVelocity].layerViews[0];
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport vp = {0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {w, h}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 0, 1, &set, 0, nullptr);
        struct { float viewProj[16], ripple[4], near_[4], tiles[4], wind[4]; } pc;
        std::memcpy(pc.viewProj, water_.viewProj.m, sizeof(pc.viewProj));
        std::memcpy(pc.ripple, water_.ripplePlace, sizeof(pc.ripple));
        std::memcpy(pc.near_, water_.nearPlace, sizeof(pc.near_));
        std::memcpy(pc.tiles, water_.tiles, sizeof(pc.tiles));
        std::memcpy(pc.wind, water_.wind, sizeof(pc.wind));
        for (size_t i = 0; i < waterMeshes_.size(); ++i) {
            const MeshId id = waterMeshes_[i];
            if (id >= meshes_.size() || !meshes_[id].buffer.buffer) continue;
            const Mesh& mesh = meshes_[id];
            pc.tiles[3] = i < water_.nearCount ? 1.0f : 0.0f;
            vkCmdPushConstants(cmd_, waterLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            const VkDeviceSize offset = mesh.base + mesh.copyBytes * NewestCopy(mesh); // the base, plus the copy if deformable
            vkCmdBindVertexBuffers(cmd_, 0, 1, &mesh.buffer.buffer, &offset);
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, 1, 0, 0, 0);
            ++counts_.draws;
            counts_.triangles += mesh.count / 3;
        }
        vkCmdEndRendering(cmd_);
        ColorToRead(composed);
        ColorToRead(post_[kVelocity].image);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSets_[cur * kFramesInFlight + frameSlot_], 0, nullptr);
    }

    bool CreateComputeLayout() {
        VkDescriptorSetLayoutBinding bindings[7];
        for (uint32_t i = 0; i < 7; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 7;
        sl.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &computeSetLayout_));
        const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &computeSetLayout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &computeLayout_));
        const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * 128};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 128;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &size;
        for (VkDescriptorPool& pool : computePools_) VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &pool));
        return true;
    }

    void ComputeBarrier(VkPipelineStageFlags fromStage, VkAccessFlags fromAccess, VkPipelineStageFlags toStage, VkAccessFlags toAccess) {
        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = fromAccess;
        barrier.dstAccessMask = toAccess;
        vkCmdPipelineBarrier(cmd_, fromStage, toStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void VegetationIndirect(const Mesh& mesh, GpuBufferId instances, GpuBufferId args, uint32_t argsOffset, const Mat4& viewProj, const Mat4& params) {
        PushConstants pc;
        std::memcpy(pc.mvp, viewProj.m, sizeof(pc.mvp));
        std::memcpy(pc.model, params.m, sizeof(pc.model));
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        const VkBuffer buffers[2] = {mesh.buffer.buffer, gpuBuffers_[instances].buffer.buffer};
        const VkDeviceSize offsets[2] = {mesh.base, 0};
        vkCmdBindVertexBuffers(cmd_, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd_, gpuBuffers_[args].buffer.buffer, argsOffset, 1, 20);
    }

    void DrawInstancedMesh(const Mesh& mesh, const Buffer& instances, uint32_t first, uint32_t count) {
        const VkBuffer buffers[2] = {mesh.buffer.buffer, instances.buffer};
        const VkDeviceSize offsets[2] = {mesh.base, 0};
        vkCmdBindVertexBuffers(cmd_, 0, 2, buffers, offsets);
        if (mesh.indexed) {
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, count, 0, 0, first);
        } else {
            vkCmdDraw(cmd_, mesh.count, count, 0, first);
        }
    }

    // DrawInstancedMesh with the instance rows at `offset` of this frame's transient buffer.
    void DrawTransientInstances(const Mesh& mesh, size_t offset, uint32_t count) {
        const VkBuffer buffers[2] = {mesh.buffer.buffer, frame_->transient.buffer};
        const VkDeviceSize offsets[2] = {mesh.base, (VkDeviceSize)offset};
        vkCmdBindVertexBuffers(cmd_, 0, 2, buffers, offsets);
        if (mesh.indexed) {
            vkCmdBindIndexBuffer(cmd_, mesh.buffer.buffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, mesh.count, count, 0, 0, 0);
        } else {
            vkCmdDraw(cmd_, mesh.count, count, 0, 0);
        }
    }

    void Submit(VkBuffer buffer, VkDeviceSize offset, uint32_t count, Topology topology, const Mat4& mvp,
                const Mat4& model, bool depthTest, bool indexed = false, VkDeviceSize indexOffset = 0) {
        const int index = (topology == Topology::Lines ? 2 : 0) + (depthTest ? 0 : 1);
        PushConstants pc;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        std::memcpy(pc.model, model.m, sizeof(pc.model));
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, !resolved_ && prepass_ && index == 0 ? prepassPipelines_[0][prepass_ - 1] : pipelines_[index + (resolved_ ? 4 : 0)]);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        if (!resolved_) vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(PushConstants), sizeof(materialConstants_), materialConstants_);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &buffer, &offset);
        if (indexed) {
            vkCmdBindIndexBuffer(cmd_, buffer, indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_, count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd_, count, 1, 0, 0);
        }
    }

    void ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess, uint32_t layers = 1, uint32_t levels = 1) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, levels, 0, layers};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &dep);
    }

    // ------------------------------------------------------------------ impostor store
    // Three texture arrays (albedo, normal, depth; layer = slot * 6 + face). The bake renders into one layer of
    // each at a time; the impostor shader samples all of them through one descriptor set. At rest they sit in
    // SHADER_READ_ONLY_OPTIMAL.
    static constexpr VkFormat kStoreFormats[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                   VK_FORMAT_R16_UNORM};

    struct StoreImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView arrayView = VK_NULL_HANDLE;    // the whole array, for sampling
        std::vector<VkImageView> layerViews;       // one per layer, for rendering
    };
    StoreImage waterRefract_; // refraction reads this copy of the composed picture

    bool CreateStoreImage(VkFormat format, uint32_t width, uint32_t height, uint32_t layers, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, StoreImage& out) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {width, height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ii, nullptr, &out.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, out.image, &req);
        const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &out.memory));
        VK_CHECK(vkBindImageMemory(device_, out.image, out.memory, 0));

        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.format = format;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.subresourceRange = {aspect, 0, 1, 0, layers};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &out.arrayView));
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        for (uint32_t l = 0; l < layers; ++l) {
            vi.subresourceRange = {aspect, 0, 1, l, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &view));
            out.layerViews.push_back(view);
        }
        return true;
    }

    void DestroyStoreImage(StoreImage& img) {
        for (VkImageView v : img.layerViews) vkDestroyImageView(device_, v, nullptr);
        if (img.arrayView) vkDestroyImageView(device_, img.arrayView, nullptr);
        if (img.image) vkDestroyImage(device_, img.image, nullptr);
        if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
        img = {};
    }

    bool CreateStore(uint32_t maxObjects, uint32_t tile) {
        storeLayers_ = maxObjects * 6;
        storeTile_ = tile;
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (int c = 0; c < 3; ++c) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys_, kStoreFormats[c], &fp);
            if ((fp.optimalTilingFeatures & need) != need) {
                Log("Vulkan: format %d cannot be rendered to and sampled", (int)kStoreFormats[c]);
                return false;
            }
            if (!CreateStoreImage(kStoreFormats[c], tile, tile, storeLayers_,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, store_[c]))
                return false;
        }
        if (!CreateStoreImage(kDepthFormat, tile, tile, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, tileDepth_))
            return false;

        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &si, nullptr, &storeSampler_));

        // Bindings 0-2: the three arrays; binding 3: the sampler (immutable, so it needs no write).
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (uint32_t i = 0; i < 3; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &storeSampler_};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 4;
        li.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &storeSetLayout_));

        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &storePool_));
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = storePool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &storeSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &storeSet_));
        VkDescriptorImageInfo infos[3];
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            infos[i] = {VK_NULL_HANDLE, store_[i].arrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = storeSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

        // 16 push constants (visible to every stage), the counterpart of Direct3D's 16 root constants.
        VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &storeSetLayout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &storeLayout_));
        return true;
    }

    void DestroyStore() {
        for (StoreImage& img : store_) DestroyStoreImage(img);
        DestroyStoreImage(tileDepth_);
        if (storeLayout_) vkDestroyPipelineLayout(device_, storeLayout_, nullptr);
        if (storePool_) vkDestroyDescriptorPool(device_, storePool_, nullptr);
        if (storeSetLayout_) vkDestroyDescriptorSetLayout(device_, storeSetLayout_, nullptr);
        if (storeSampler_) vkDestroySampler(device_, storeSampler_, nullptr);
        storeLayout_ = VK_NULL_HANDLE;
        storePool_ = VK_NULL_HANDLE;
        storeSetLayout_ = VK_NULL_HANDLE;
        storeSampler_ = VK_NULL_HANDLE;
    }

    // A no-vertex-buffer, no-depth pipeline over the store pipeline layout.
    bool CreateFullscreenPipeline(VkShaderModule vs, VkShaderModule ps, const char* psEntry, const VkFormat* formats,
                                  uint32_t targets, VkPipeline& out) {
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "FullscreenVS";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = psEntry;
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachments[3] = {};
        for (uint32_t i = 0; i < targets; ++i)
            blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = targets;
        blend.pAttachments = blendAttachments;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = targets;
        rendering.pColorAttachmentFormats = formats;

        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &blend;
        gi.pDynamicState = &dyn;
        gi.layout = storeLayout_;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &out));
        return true;
    }

    // Renders known values into layer 3 of the store (clearing every other layer), then samples layers 3 and 2
    // through the descriptor set into a 2x1 target and reads it back. Run once at startup when TBD_TEXTURE_SELFTEST is set.
    bool SelfTestTextures() {
        constexpr uint32_t kTile = 16, kLayer = 3;
        if (!CreateStore(1, kTile)) return false;

        struct Temps {
            VulkanRenderer* r;
            VkShaderModule vs = VK_NULL_HANDLE, fillPs = VK_NULL_HANDLE, samplePs = VK_NULL_HANDLE;
            VkPipeline fill = VK_NULL_HANDLE, sample = VK_NULL_HANDLE;
            StoreImage target;
            Buffer readback;
            ~Temps() {
                VkDevice d = r->device_;
                if (fill) vkDestroyPipeline(d, fill, nullptr);
                if (sample) vkDestroyPipeline(d, sample, nullptr);
                if (vs) vkDestroyShaderModule(d, vs, nullptr);
                if (fillPs) vkDestroyShaderModule(d, fillPs, nullptr);
                if (samplePs) vkDestroyShaderModule(d, samplePs, nullptr);
                r->DestroyStoreImage(target);
                r->DestroyBuffer(readback);
            }
        } t{this};

        t.vs = LoadShader(L"selftest_vs.spv");
        t.fillPs = LoadShader(L"selftest_fill_ps.spv");
        t.samplePs = LoadShader(L"selftest_sample_ps.spv");
        if (!t.vs || !t.fillPs || !t.samplePs) return false;
        const VkFormat sampleFormat[1] = {VK_FORMAT_R8G8B8A8_UNORM};
        if (!CreateFullscreenPipeline(t.vs, t.fillPs, "FillPS", kStoreFormats, 3, t.fill) ||
            !CreateFullscreenPipeline(t.vs, t.samplePs, "SamplePS", sampleFormat, 1, t.sample))
            return false;
        if (!CreateStoreImage(VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 1,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, t.target) ||
            !CreateBuffer(8, t.readback, true, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            return false;

        VK_CHECK(vkResetCommandBuffer(cmd_, 0));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));

        constexpr VkPipelineStageFlags2 kAttachStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        // Pass 1: one rendering per layer (each clears its layer), the chosen one also draws.
        for (StoreImage& img : store_)
            ImageBarrier(img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAttachStage, 0, kAttachStage,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, storeLayers_);
        for (uint32_t l = 0; l < storeLayers_; ++l) {
            VkRenderingAttachmentInfo att[3] = {};
            for (uint32_t c = 0; c < 3; ++c) {
                att[c] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                att[c].imageView = store_[c].layerViews[l];
                att[c].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att[c].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att[c].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {kTile, kTile}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 3;
            ri.pColorAttachments = att;
            vkCmdBeginRendering(cmd_, &ri);
            if (l == kLayer) {
                const VkViewport vp = {0.0f, 0.0f, (float)kTile, (float)kTile, 0.0f, 1.0f};
                const VkRect2D scissor = {{0, 0}, {kTile, kTile}};
                vkCmdSetViewport(cmd_, 0, 1, &vp);
                vkCmdSetScissor(cmd_, 0, 1, &scissor);
                vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, t.fill);
                vkCmdDraw(cmd_, 3, 1, 0, 0);
            }
            vkCmdEndRendering(cmd_);
        }
        for (StoreImage& img : store_)
            ImageBarrier(img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAttachStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, storeLayers_);

        // Pass 2: sample it back.
        ImageBarrier(t.target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAttachStage, 0, kAttachStage,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo out = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        out.imageView = t.target.layerViews[0];
        out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        out.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {2, 1}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &out;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport vp = {0.0f, 0.0f, 2.0f, 1.0f, 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {2, 1}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, t.sample);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, storeLayout_, 0, 1, &storeSet_, 0, nullptr);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRendering(cmd_);

        ImageBarrier(t.target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAttachStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {2, 1, 1};
        vkCmdCopyImageToBuffer(cmd_, t.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback.buffer, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cmd_));

        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue_));

        uint8_t got[8];
        std::memcpy(got, t.readback.mapped, 8);
        // Expected: pixel 0 = (1.0, 0.5, 1.0, 0.75) -> (255, 128, 255, 191); pixel 1 = untouched layer = 0.
        const uint8_t want[8] = {255, 128, 255, 191, 0, 0, 0, 0};
        bool ok = true;
        for (int i = 0; i < 8; ++i)
            if (std::abs((int)got[i] - (int)want[i]) > 2) ok = false;
        Log("Vulkan: self-test pixels (%u %u %u %u) (%u %u %u %u), expected (%u %u %u %u) (0 0 0 0)", got[0], got[1], got[2],
            got[3], got[4], got[5], got[6], got[7], want[0], want[1], want[2], want[3]);
        return ok;
    }

    void Shutdown() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            FlushStreamRetired(true);
            DestroyStreaming();
            DestroyStore();
            for (StoreImage& img : chunkStore_) DestroyStoreImage(img); // the atlas itself is in textures_
            DestroyStoreImage(chunkDepth_);
            DestroyChunkPipelines();
            DestroyPostTargets();
            DestroyBuffer(captureBuffer_);
            for (VkPipeline p : postPipelines_)
                if (p) vkDestroyPipeline(device_, p, nullptr);
            if (postLayout_) vkDestroyPipelineLayout(device_, postLayout_, nullptr);
            if (postPool_) vkDestroyDescriptorPool(device_, postPool_, nullptr);
            if (postSetLayout_) vkDestroyDescriptorSetLayout(device_, postSetLayout_, nullptr);
            for (VkSampler sampler : postSamplers_)
                if (sampler) vkDestroySampler(device_, sampler, nullptr);
            DestroyTextureObjects(cloudNoise_);
            DestroyTextureObjects(cloudField_);
            for (Texture& t : textures_) DestroyTextureObjects(t);
            if (spareImageBlock_.valid()) { const VkDeviceMemory m = spareImageBlock_.get(); if (m) vkFreeMemory(device_, m, nullptr); }
            for (ImageBlock& b : imageBlocks_) vkFreeMemory(device_, b.memory, nullptr);
            imageBlocks_.clear();
            if (materialPool_) vkDestroyDescriptorPool(device_, materialPool_, nullptr);
            if (materialSetLayout_) vkDestroyDescriptorSetLayout(device_, materialSetLayout_, nullptr);
            if (materialSampler_) vkDestroySampler(device_, materialSampler_, nullptr);
            if (overlayPs_) vkDestroyShaderModule(device_, overlayPs_, nullptr);
            for (Frame& frame : frames_) {
                if (frame.timerPool) vkDestroyQueryPool(device_, frame.timerPool, nullptr);
                DestroyBuffer(frame.transient);
                DestroyBuffer(frame.lightBuffer);
                DestroyBuffer(frame.postBuffer);
                if (frame.fence) vkDestroyFence(device_, frame.fence, nullptr);
                if (frame.imageAvailable) vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
            }
            if (shadowPipeline_) vkDestroyPipeline(device_, shadowPipeline_, nullptr);
            if (tightPipeline_) vkDestroyPipeline(device_, tightPipeline_, nullptr);
            if (shadowVs_) vkDestroyShaderModule(device_, shadowVs_, nullptr);
            DestroyStoreImage(shadow_);
            DestroyStoreImage(sunCache_);
            DestroyStoreImage(tightShadow_);
            if (lightPool_) vkDestroyDescriptorPool(device_, lightPool_, nullptr);
            if (lightSetLayout_) vkDestroyDescriptorSetLayout(device_, lightSetLayout_, nullptr);
            if (shadowSampler_) vkDestroySampler(device_, shadowSampler_, nullptr);
            for (Mesh& m : meshes_)
                if (!m.sharesBuffer) {
                    DestroyBuffer(m.buffer);
                    if (m.upload.buffer) DestroyBuffer(m.upload);
                }
            for (Buffer& b : instanceBuffers_) DestroyBuffer(b);
            for (GpuBuffer& g : gpuBuffers_) DestroyBuffer(g.buffer);
            for (VkPipeline p : computePipelines_) vkDestroyPipeline(device_, p, nullptr);
            DestroyUiPipeline();
            if (waterPipeline_) vkDestroyPipeline(device_, waterPipeline_, nullptr);
            if (waterLayout_) vkDestroyPipelineLayout(device_, waterLayout_, nullptr);
            if (waterPool_) vkDestroyDescriptorPool(device_, waterPool_, nullptr);
            if (waterSetLayout_) vkDestroyDescriptorSetLayout(device_, waterSetLayout_, nullptr);
            for (VkDescriptorPool pool : computePools_) if (pool) vkDestroyDescriptorPool(device_, pool, nullptr);
            if (computeLayout_) vkDestroyPipelineLayout(device_, computeLayout_, nullptr);
            if (computeSetLayout_) vkDestroyDescriptorSetLayout(device_, computeSetLayout_, nullptr);
            if (vegetationDepthPipeline_) vkDestroyPipeline(device_, vegetationDepthPipeline_, nullptr);
            if (vegetationShadePipeline_) vkDestroyPipeline(device_, vegetationShadePipeline_, nullptr);
            if (skinnedMotionPipeline_) vkDestroyPipeline(device_, skinnedMotionPipeline_, nullptr);
            for (Buffer& b : motionBuffers_) DestroyBuffer(b);
            if (vegetationPipeline_) vkDestroyPipeline(device_, vegetationPipeline_, nullptr);
            if (vegetationShadowPipeline_) vkDestroyPipeline(device_, vegetationShadowPipeline_, nullptr);
            if (shadowCutoutPipeline_) vkDestroyPipeline(device_, shadowCutoutPipeline_, nullptr);
            if (tightCutoutPipeline_) vkDestroyPipeline(device_, tightCutoutPipeline_, nullptr);
            for (VkPipeline p : {instancedPipeline_, shadowInstancedPipeline_, tightInstancedPipeline_, prepassPipelines_[0][0], prepassPipelines_[0][1], prepassPipelines_[1][0], prepassPipelines_[1][1]})
                if (p) vkDestroyPipeline(device_, p, nullptr);
            for (VkShaderModule m : {instancedVs_, shadowInstancedVs_})
                if (m) vkDestroyShaderModule(device_, m, nullptr);
            for (VkPipeline p : pipelines_)
                if (p) vkDestroyPipeline(device_, p, nullptr);
            if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
            if (vs_) vkDestroyShaderModule(device_, vs_, nullptr);
            if (ps_) vkDestroyShaderModule(device_, ps_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            DestroySwapchainViews();
            if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) {
            if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
            if (messenger_) {
                auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
                if (destroy) destroy(instance_, messenger_, nullptr);
            }
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_ = {};      // the window's: swapchain, tone map, overlays, capture
    VkExtent2D sceneExtent_ = {}; // the render size: the scene and post chain draw at this, the tone map upscales
    float scale_ = 1.0f;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> renderDone_; // one per swapchain image
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkImage overlayDepthImage_ = VK_NULL_HANDLE; // window-sized, only while the scale is below 1
    VkDeviceMemory overlayDepthMemory_ = VK_NULL_HANDLE;
    VkImageView overlayDepthView_ = VK_NULL_HANDLE;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    Frame frames_[kFramesInFlight];
    Frame* frame_ = nullptr;
    uint32_t frameSlot_ = kFramesInFlight - 1;

    VkShaderModule vs_ = VK_NULL_HANDLE, ps_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[8] = {}; // triangles/lines x depth/no depth, for the HDR scene then for overlays
    VkShaderModule overlayPs_ = VK_NULL_HANDLE;

    std::vector<Texture> textures_;
    std::vector<TextureId> freeTextures_;
    TextureId defaults_[4] = {kNoTexture, kNoTexture, kNoTexture, kNoTexture};
    std::vector<TextureId> extraOf_; // by surface texture id: its extra map
    std::map<uint64_t, VkDescriptorSet> materialSets_;
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
    VkSampler materialSampler_ = VK_NULL_HANDLE;
    bool anisotropy_ = false;

    StoreImage post_[kPostTargets];
    Texture cloudNoise_, cloudField_;
    Buffer captureBuffer_;
    VkSampler postSamplers_[2] = {};
    VkDescriptorSetLayout postSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool postPool_ = VK_NULL_HANDLE;
    VkDescriptorSet postSets_[2 * kFramesInFlight] = {};
    VkPipelineLayout postLayout_ = VK_NULL_HANDLE;
    VkPipeline postPipelines_[11] = {}; // AO, clouds, composite, TAA, tone map, probe trace, probe blend, exposure, bloom, cloud shadow
    uint32_t probeAge_ = 0;
    bool cloudMapValid_ = false;
    FrameView view_;
    bool resolved_ = false, historyValid_ = false;
    uint32_t postFrame_ = 0;

    bool capture_ = false, captureNew_ = false;
    std::vector<uint8_t> captureRgba_;
    size_t captureBytes_ = 0;
    int captureW_ = 0, captureH_ = 0;

    float timerPeriodNs_ = 1.0f;
    uint32_t timerCount_ = 0;
    double timerSum_ = 0.0;

    StoreImage store_[3], tileDepth_; // impostor albedo / normal / depth arrays, bake depth buffer
    VkSampler storeSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout storeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool storePool_ = VK_NULL_HANDLE;
    VkDescriptorSet storeSet_ = VK_NULL_HANDLE;
    VkPipelineLayout storeLayout_ = VK_NULL_HANDLE;
    uint32_t storeLayers_ = 0, storeTile_ = 0;

    StoreImage chunkStore_[3], chunkDepth_;  // city chunk bakes: one layer per face, and the tile's depth
    TextureId chunkTex_[3] = {kNoTexture, kNoTexture, kNoTexture}; // the atlas channels, in textures_
    VkPipeline chunkBakePipelines_[2] = {}; // surfaces, then the relief's MIN pass
    VkPipeline chunkVolPipeline_ = VK_NULL_HANDLE;
    uint32_t chunkTile_ = 0, chunkW_ = 0, chunkH_ = 0, chunkLayer_ = 0;
    bool chunkBaking_ = false;
    std::vector<size_t> chunkOffsets_; // ChunkBakeFace: each item's rows in the transient buffer

    float clear_[4] = {};
    StoreImage sunCache_; // the sun map's depth from the casters that don't move
    StoreImage shadow_, tightShadow_; // the sun's map, and the tight character map around the camera's focus
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkShaderModule shadowVs_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE, tightPipeline_ = VK_NULL_HANDLE;
    Mat4 lightVP_;
    bool shadowActive_ = false;
    ShadowMap shadowTarget_ = ShadowMap::Sun; // the shadow pass being recorded
    VkDescriptorSetLayout lightSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool lightPool_ = VK_NULL_HANDLE;

    static constexpr uint32_t kInstanceStride = 12 * sizeof(float);
    std::vector<Buffer> instanceBuffers_;
    std::vector<InstanceBufferId> freeInstanceSlots_;
    VkShaderModule instancedVs_ = VK_NULL_HANDLE, shadowInstancedVs_ = VK_NULL_HANDLE;
    VkPipeline instancedPipeline_ = VK_NULL_HANDLE, shadowInstancedPipeline_ = VK_NULL_HANDLE, tightInstancedPipeline_ = VK_NULL_HANDLE;
    VkPipeline prepassPipelines_[2][2] = {}; // [mesh, instanced][depth only, shade after]
    int prepass_ = 0;                        // SetScenePrepass
    bool shadowInstancedBound_ = false;

    std::vector<Mesh> meshes_;
    std::vector<MeshId> freeSlots_;
    std::vector<Buffer> retired_; // legacy deferred resources
    std::vector<StreamRetired> streamRetired_;
    uint64_t streamFrame_ = 0;
    size_t used_ = 0;
    uint32_t w_ = 1, h_ = 1, imageIndex_ = 0;
    bool frameActive_ = false, needRecreate_ = false;
};

} // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer() { return std::make_unique<VulkanRenderer>(); }

} // namespace tbd

#endif // TBD_HAVE_VULKAN
