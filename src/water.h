// Water. Takes over any scene entity whose file is map_water or *_water_surface: that geometry stops being a lit mesh and becomes the
// surface drawn by shaders/water.hlsl after the sky and haze are composed.
//   Coarse mesh: the file itself, cut into a grid of chunks so that off-screen water costs nothing.
//   Dense mesh: the triangles near the camera split by distance on a worker, so waves can move the surface.
//   Compute: three repeating wave tiles, a sky table, and a 512 x 512 height field wave equation that slides with the camera.
//   Reactions: AddWaterImpulse, called directly or by WaterSystem::Colliders for anything that crosses the surface.
// Env: TBD_WATER=0 leaves the entity as it was; TBD_WATER=2 runs compute only, to time it.
// TBD_WATER_TEST="x z radius speed extent": a sphere moved through the water at a fixed step per frame, so runs reproduce.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.h"
#include "culling.h"
#include "render/renderer.h"
#include "scene.h"
#include "vecmath.h"

namespace tbd {

constexpr uint32_t kWaterRippleSize = 512, kWaterTile = 128, kWaterCascades = 3;
constexpr float kWaterCell = 0.25f;       // metres per ripple cell
constexpr float kWaterNearHalf = 192.0f;  // half the dense mesh's square

inline bool IsWaterFile(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return name.rfind("map_water", 0) == 0 || name.find("_water_surface") != std::string::npos;
}

struct WaterCollider { Vec3 center; float radius; };

class WaterSystem {
public:
    using LoadFn = bool (*)(const std::string& path, std::vector<Vertex>& out);

    static char Mode() { static const char m = [] { char b[8] = {}; return GetEnvironmentVariableA("TBD_WATER", b, 8) > 0 ? b[0] : '1'; }(); return m; }

    // After a scene loads: reads its water files on a worker.
    void Prepare(Renderer* r, JobSystem* jobs, const Scene& scene, LoadFn load) {
        surfaceFlags_.clear();
        if (r) {
            for (const Chunk& c : chunks_) r->RetireMesh(c.mesh);
            if (nearMesh_ != kInvalidMesh) r->RetireMesh(nearMesh_);
        }
        chunks_.clear();
        nearMesh_ = kInvalidMesh;
        map_.reset();
        dense_.reset();
        everBuilt_ = false;
        colliders_.clear();
        impulses_.clear();
        if (Mode() == '0') return;
        std::vector<std::pair<std::string, Mat4>> sources;
        for (const Entity& e : scene.entities)
            if (e.mesh == MeshKind::Imported && IsWaterFile(e.meshPath)) sources.push_back({e.meshPath, e.Model()});
        ScanEntities(scene);
        std::shared_ptr<Build> build = std::make_shared<Build>();
        building_ = build;
        if (sources.empty() || !jobs) { building_.reset(); return; }
        jobs_ = jobs;
        jobs->RunBackground([build, sources, load] {
            for (const auto& [path, model] : sources) {
                std::vector<Vertex> v;
                if (!load(path, v)) continue;
                for (Vertex& x : v) {
                    const Vec4 p = model * Vec4{x.px, x.py, x.pz, 1.0f};
                    x.px = p.x; x.py = p.y; x.pz = p.z;
                }
                build->triangles.insert(build->triangles.end(), v.begin(), v.end());
            }
            ComputeFlow(build->triangles);
            build->done.store(true, std::memory_order_release);
        });
    }

    void Forget() { // the renderer is gone, and its ids with it
        chunks_.clear();
        map_.reset();
        dense_.reset();
        building_.reset();
        everBuilt_ = false;
        nearMesh_ = kInvalidMesh;
        waves_ = sky_ = kNoGpuBuffer;
        ripple_[0] = ripple_[1] = kNoGpuBuffer;
        unused_ = kNoGpuBuffer;
        csRipple_ = csWaves_ = csSky_ = kNoComputeShader;
        gpuReady_ = false;
        visibleFrame_ = false;
    }

    bool IsSurface(size_t entity) const { return entity < surfaceFlags_.size() && surfaceFlags_[entity]; }
    void ScanEntities(const Scene& scene) {
        surfaceFlags_.resize(scene.entities.size());
        if (Mode() == '0') return;
        for (size_t i = 0; i < scene.entities.size(); ++i) {
            const Entity& e = scene.entities[i];
            surfaceFlags_[i] = e.mesh == MeshKind::Imported && IsWaterFile(e.meshPath);
        }
    }

    // Any game code: pushes the surface at a point. `strength` is metres per second of surface speed, down when positive.
    void AddImpulse(Vec3 pos, float radius, float strength) {
        if (impulses_.size() < 96) impulses_.push_back({pos.x, pos.z, std::max(radius, 0.12f), -strength});
    }

    // The surface's rest height under (x, z), from the map's triangles; false where there is no water.
    bool HeightAt(float x, float z, float& out) const {
        if (!map_) return false;
        const auto it = map_->cells.find(TileKey((int)std::floor(x / kCell), (int)std::floor(z / kCell)));
        if (it == map_->cells.end()) return false;
        for (uint32_t t : it->second) {
            const Vertex* v = &map_->triangles[t];
            const float d = (v[1].pz - v[2].pz) * (v[0].px - v[2].px) + (v[2].px - v[1].px) * (v[0].pz - v[2].pz);
            if (std::fabs(d) < 1e-9f) continue;
            const float a = ((v[1].pz - v[2].pz) * (x - v[2].px) + (v[2].px - v[1].px) * (z - v[2].pz)) / d;
            const float b = ((v[2].pz - v[0].pz) * (x - v[2].px) + (v[0].px - v[2].px) * (z - v[2].pz)) / d;
            const float c = 1.0f - a - b;
            if (a >= -1e-3f && b >= -1e-3f && c >= -1e-3f) { out = a * v[0].py + b * v[1].py + c * v[2].py; return true; }
        }
        return false;
    }

    // Once per frame, before BeginFrame: finishes loads, swaps in the dense mesh, turns anything crossing the surface into impulses.
    void Update(Renderer& r, const Scene& scene, const std::vector<Sphere>& worldBounds, Vec3 eye, float dt,
                const WaterCollider* extra, uint32_t extraCount) {
        if (Mode() == '0') return;
        if (surfaceFlags_.size() != scene.entities.size()) ScanEntities(scene);
        if (building_ && building_->done.load(std::memory_order_acquire)) FinishLoad(r);
        if (chunks_.empty()) return;
        if (!gpuReady_ && !CreateGpu(r)) return; // here, not in Compute: uploads wait on the queue outside any frame
        eye_ = eye;
        UpdateDenseMesh(r);
        const bool hasNear = nearMesh_ != kInvalidMesh;
        if (!hasNear) { impulses_.clear(); colliders_.clear(); return; }

        for (size_t i = 0; i < std::min(worldBounds.size(), scene.entities.size()); ++i) {
            if (surfaceFlags_[i] || worldBounds[i].radius <= 0.0f || worldBounds[i].radius > 40.0f) continue;
            const Vec3 away = worldBounds[i].center - eye;
            if (std::fabs(away.x) > kWaterRippleSize * kWaterCell * 0.5f || std::fabs(away.z) > kWaterRippleSize * kWaterCell * 0.5f) { colliders_.erase((uint64_t)i); continue; }
            Touch((uint64_t)i, worldBounds[i].center, worldBounds[i].radius, dt);
        }
        for (uint32_t i = 0; i < extraCount; ++i) Touch(0xFFFF0000ull + i, extra[i].center, extra[i].radius, dt);
    }

    // After BeginFrame, before any shadow pass.
    void Compute(Renderer& r, const ViewVolume& volume, float dt) {
        visibleFrame_ = false;
        if (chunks_.empty() || Mode() == '0') return;
        visibleChunks_.clear();
        const bool hasNear = nearMesh_ != kInvalidMesh;
        if (hasNear) visibleChunks_.push_back(nearMesh_);
        nearCount_ = (uint32_t)visibleChunks_.size();
        for (const Chunk& c : chunks_) {
            const Vec3 d = c.bounds.center - eye_;
            if (std::sqrt(Dot(d, d)) - c.bounds.radius < 6000.0f && volume.Touches(c.bounds)) visibleChunks_.push_back(c.mesh);
        }
        if (visibleChunks_.empty()) { time_ += dt; return; } // nothing near and nothing on screen: nothing runs
        if (!gpuReady_) return;
        visibleFrame_ = true;
        time_ += dt;

        r.BeginCompute();
        const GpuBufferId reads[4] = {unused_, unused_, unused_, unused_};
        const GpuBufferId outWaves[3] = {ripple_[0], ripple_[1], waves_};
        // A tile a frame, in turn would save little: at 128 x 128 x 20 waves all three stay under a tenth of a millisecond.
        for (uint32_t c = 0; c < kWaterCascades; ++c) {
            ComputeConstants pc = {};
            pc.a[0] = time_; pc.a[1] = kTiles[c]; pc.a[2] = (float)c; pc.a[3] = kAmplitude[c];
            pc.b[0] = kWind[0]; pc.b[1] = kWind[1];
            r.Dispatch(csWaves_, &pc, 32, reads, outWaves, kWaterTile / 8, kWaterTile / 8, 1);
        }
        const GpuBufferId outSky[3] = {ripple_[0], ripple_[1], sky_};
        {
            ComputeConstants pc = {};
            pc.a[0] = eye_.x; pc.a[1] = eye_.y; pc.a[2] = eye_.z;
            pc.b[0] = sunDir_[0]; pc.b[1] = sunDir_[1]; pc.b[2] = sunDir_[2];
            r.Dispatch(csSky_, &pc, 32, reads, outSky, 4, 2, 1);
        }

        if (hasNear) StepRipples(r, reads, dt);
        r.EndCompute();
    }

    void SetSun(const float dir[4]) { for (int i = 0; i < 3; ++i) sunDir_[i] = dir[i]; }

    // After the lit scene, before ResolveScene.
    void SubmitDraw(Renderer& r, const Mat4& sceneViewProj, const float jitterNdc[2]) {
        if (!visibleFrame_ || Mode() == '2') return;
        WaterDraw d;
        d.meshes = visibleChunks_.data();
        d.meshCount = (uint32_t)visibleChunks_.size();
        d.nearCount = nearCount_;
        d.waves = waves_;
        d.ripple = ripple_[current_];
        d.sky = sky_;
        d.viewProj = sceneViewProj;
        d.ripplePlace[0] = (float)originCell_[0] * kWaterCell;
        d.ripplePlace[1] = (float)originCell_[1] * kWaterCell;
        d.ripplePlace[2] = kWaterCell;
        d.ripplePlace[3] = nearMesh_ != kInvalidMesh ? 1.0f : 0.0f;
        d.nearPlace[0] = nearCentre_[0]; d.nearPlace[1] = nearCentre_[1];
        d.nearPlace[2] = nearMesh_ != kInvalidMesh ? kWaterNearHalf : 0.0f;
        d.nearPlace[3] = time_;
        d.tiles[0] = kTiles[0]; d.tiles[1] = kTiles[1]; d.tiles[2] = kTiles[2];
        d.wind[0] = jitterNdc[0]; d.wind[1] = jitterNdc[1]; d.wind[2] = 1.0f; d.wind[3] = Mode() == '3' ? 1.0f : 0.0f;
        r.SetWaterDraw(d);
    }

    // Debug: the height field's peak and total motion, read from the GPU after a frame, to prove the reactions in numbers.
    void LogRipples(Renderer& r) {
        if (ripple_[current_] == kNoGpuBuffer) { Log("Water: no simulation ran (near triangles %u)", nearTriangles_); return; }
        std::vector<float> cells(kWaterRippleSize * kWaterRippleSize * 6);
        if (!r.ReadGpuBuffer(ripple_[current_], cells.data(), cells.size() * sizeof(float))) return;
        float peak = 0.0f, foam = 0.0f;
        double energy = 0.0;
        for (size_t i = 0; i < cells.size(); i += 6) {
            peak = std::max(peak, std::fabs(cells[i]));
            foam = std::max(foam, cells[i + 2]);
            energy += (double)cells[i] * cells[i] + (double)cells[i + 1] * cells[i + 1] * 0.05;
        }
        std::vector<float> tiles((size_t)kWaterTile * kWaterTile * kWaterCascades * 8);
        if (r.ReadGpuBuffer(waves_, tiles.data(), tiles.size() * sizeof(float)))
            for (uint32_t c = 0; c < kWaterCascades; ++c) {
                double h = 0.0, sx = 0.0, sz = 0.0;
                const float* t = tiles.data() + (size_t)c * kWaterTile * kWaterTile * 8;
                for (size_t i = 0; i < (size_t)kWaterTile * kWaterTile; ++i) { h += t[i * 8 + 1] * t[i * 8 + 1]; sx += t[i * 8 + 4] * t[i * 8 + 4]; sz += t[i * 8 + 5] * t[i * 8 + 5]; }
                const double n = (double)kWaterTile * kWaterTile;
                Log("Water: tile %u rms height %.4f slope x %.4f z %.4f; texel (9,21) %.4f %.4f %.4f", c, std::sqrt(h / n), std::sqrt(sx / n), std::sqrt(sz / n), t[(21 * kWaterTile + 9) * 8 + 1], t[(21 * kWaterTile + 9) * 8 + 4], t[(21 * kWaterTile + 9) * 8 + 5]);
            }
        Log("Water: ripple peak %.3f m, energy %.3f, foam %.2f, steps %u, impulses %u, dense triangles %u, chunks %zu, drawn %zu, time %.2f",
            peak, energy, foam, steps_, totalImpulses_, nearTriangles_, chunks_.size(), visibleChunks_.size(), time_);
    }

    bool Active() const { return !chunks_.empty(); }

private:
    struct Build {
        std::atomic<bool> done{false};
        std::vector<Vertex> triangles;
    };
    struct Dense {
        std::atomic<bool> done{false};
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        float centre[2] = {};
    };
    struct Chunk { MeshId mesh; Sphere bounds; };
    struct MapData {
        std::vector<Vertex> triangles;
        std::unordered_map<uint64_t, std::vector<uint32_t>> cells; // triangles per lookup cell
    };
    struct ComputeConstants { float a[4], b[4], impulses[6][4]; };
    static_assert(sizeof(ComputeConstants) == 128);
    struct Impulse { float x, z, radius, strength; };
    struct Tracked { Vec3 last; bool under; };

    static constexpr float kCell = 32.0f; // metres per lookup cell
    static constexpr float kTiles[3] = {7.3f, 29.0f, 131.0f};
    static constexpr float kAmplitude[3] = {1.0f, 0.85f, 0.35f};
    static constexpr float kWind[2] = {0.82f, 0.57f};

    static uint64_t TileKey(int x, int z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; }

    // The surface runs downhill, in metres per second: the map's rivers fall, its lakes are flat, so slope alone sorts them out.
    static void ComputeFlow(std::vector<Vertex>& v) {
        for (size_t t = 0; t + 2 < v.size(); t += 3) {
            const Vec3 a = {v[t].px, v[t].py, v[t].pz}, b = {v[t + 1].px, v[t + 1].py, v[t + 1].pz}, c = {v[t + 2].px, v[t + 2].py, v[t + 2].pz};
            const Vec3 n0 = Cross(b - a, c - a);
            const float len = std::sqrt(Dot(n0, n0));
            float fx = 0.0f, fz = 0.0f;
            if (len > 1e-6f && std::fabs(n0.y) > 1e-6f) {
                const float sx = -n0.x / n0.y, sz = -n0.z / n0.y; // height per metre along x and z
                const float slope = std::sqrt(sx * sx + sz * sz);
                if (slope > 2e-4f) {
                    const float speed = std::min(6.0f * std::sqrt(slope), 3.5f);
                    fx = -sx / slope * speed;
                    fz = -sz / slope * speed;
                }
            }
            for (int k = 0; k < 3; ++k) { v[t + k].u = fx; v[t + k].v = fz; }
        }
    }

    void FinishLoad(Renderer& r) {
        std::shared_ptr<MapData> map = std::make_shared<MapData>();
        map->triangles = std::move(building_->triangles);
        building_.reset();
        const std::vector<Vertex>& triangles_ = map->triangles;
        auto& tileCells_ = map->cells;
        std::unordered_map<uint64_t, std::vector<uint32_t>> byChunk;
        for (uint32_t t = 0; t + 2 < triangles_.size(); t += 3) {
            const Vertex* v = &triangles_[t];
            const float x0 = std::min({v[0].px, v[1].px, v[2].px}), x1 = std::max({v[0].px, v[1].px, v[2].px});
            const float z0 = std::min({v[0].pz, v[1].pz, v[2].pz}), z1 = std::max({v[0].pz, v[1].pz, v[2].pz});
            for (int z = (int)std::floor(z0 / kCell); z <= (int)std::floor(z1 / kCell); ++z)
                for (int x = (int)std::floor(x0 / kCell); x <= (int)std::floor(x1 / kCell); ++x) tileCells_[TileKey(x, z)].push_back(t);
            byChunk[TileKey((int)std::floor((v[0].px + v[1].px + v[2].px) / 3.0f / 512.0f), (int)std::floor((v[0].pz + v[1].pz + v[2].pz) / 3.0f / 512.0f))].push_back(t);
        }
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        for (const auto& [key, list] : byChunk) {
            verts.clear();
            indices.clear();
            Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
            for (uint32_t t : list)
                for (int k = 0; k < 3; ++k) {
                    const Vertex& x = triangles_[t + k];
                    indices.push_back((uint32_t)verts.size());
                    verts.push_back(x);
                    lo = {std::min(lo.x, x.px), std::min(lo.y, x.py), std::min(lo.z, x.pz)};
                    hi = {std::max(hi.x, x.px), std::max(hi.y, x.py), std::max(hi.z, x.pz)};
                }
            Chunk c;
            c.mesh = r.CreateMeshNoWait(verts.data(), (uint32_t)verts.size(), indices.data(), (uint32_t)indices.size());
            c.bounds.center = (lo + hi) * 0.5f;
            const Vec3 half = (hi - lo) * 0.5f;
            c.bounds.radius = std::sqrt(Dot(half, half)) + 3.0f;
            if (c.mesh != kInvalidMesh) chunks_.push_back(c);
        }
        Log("Water: %zu triangles in %zu chunks", triangles_.size() / 3, chunks_.size());
        map_ = map;
    }

    // ---- the dense mesh
    void UpdateDenseMesh(Renderer& r) {
        if (dense_ && dense_->done.load(std::memory_order_acquire)) {
            if (nearMesh_ != kInvalidMesh) r.RetireMesh(nearMesh_);
            nearMesh_ = kInvalidMesh;
            nearTriangles_ = (uint32_t)dense_->indices.size() / 3;
            if (!dense_->indices.empty())
                nearMesh_ = r.CreateMeshNoWait(dense_->vertices.data(), (uint32_t)dense_->vertices.size(), dense_->indices.data(), (uint32_t)dense_->indices.size());
            nearCentre_[0] = dense_->centre[0];
            nearCentre_[1] = dense_->centre[1];
            dense_.reset();
        }
        if (dense_ || !jobs_) return;
        const float dx = eye_.x - builtAt_.x, dy = eye_.y - builtAt_.y, dz = eye_.z - builtAt_.z;
        if (everBuilt_ && dx * dx + dy * dy + dz * dz < 64.0f) return;
        everBuilt_ = true;
        builtAt_ = eye_;
        std::shared_ptr<Dense> job = std::make_shared<Dense>();
        job->centre[0] = std::floor(eye_.x / 16.0f) * 16.0f;
        job->centre[1] = std::floor(eye_.z / 16.0f) * 16.0f;
        dense_ = job;
        const Vec3 eye = eye_;
        const std::shared_ptr<MapData> map = map_;
        jobs_->RunBackground([job, eye, map] { BuildDense(*job, eye, map->triangles, map->cells); });
    }

    // An edge splits by its own length and distance only, so the two triangles that share it always agree and no crack opens.
    static float EdgeLimit(const Vertex& a, const Vertex& b, Vec3 eye) {
        const Vec3 m = {(a.px + b.px) * 0.5f - eye.x, (a.py + b.py) * 0.5f - eye.y, (a.pz + b.pz) * 0.5f - eye.z};
        return std::max(0.5f, std::sqrt(Dot(m, m)) * 0.045f);
    }
    static bool Splits(const Vertex& a, const Vertex& b, Vec3 eye) {
        const float dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
        const float limit = EdgeLimit(a, b, eye);
        return dx * dx + dy * dy + dz * dz > limit * limit;
    }
    static Vertex Mid(const Vertex& a, const Vertex& b) {
        Vertex m = a;
        m.px = (a.px + b.px) * 0.5f; m.py = (a.py + b.py) * 0.5f; m.pz = (a.pz + b.pz) * 0.5f;
        m.u = (a.u + b.u) * 0.5f; m.v = (a.v + b.v) * 0.5f;
        return m;
    }
    static void Split(const Vertex& a, const Vertex& b, const Vertex& c, Vec3 eye, int depth, Dense& out) {
        const bool ab = depth > 0 && Splits(a, b, eye), bc = depth > 0 && Splits(b, c, eye), ca = depth > 0 && Splits(c, a, eye);
        if (!ab && !bc && !ca) {
            for (const Vertex* v : {&a, &b, &c}) { out.indices.push_back((uint32_t)out.vertices.size()); out.vertices.push_back(*v); }
            return;
        }
        const Vertex mab = Mid(a, b), mbc = Mid(b, c), mca = Mid(c, a);
        if (ab && bc && ca) {
            Split(a, mab, mca, eye, depth - 1, out); Split(mab, b, mbc, eye, depth - 1, out);
            Split(mca, mbc, c, eye, depth - 1, out); Split(mab, mbc, mca, eye, depth - 1, out);
        } else if (ab && bc) {
            Split(a, mab, c, eye, depth - 1, out); Split(mab, mbc, c, eye, depth - 1, out); Split(mab, b, mbc, eye, depth - 1, out);
        } else if (bc && ca) {
            Split(b, mbc, a, eye, depth - 1, out); Split(mbc, mca, a, eye, depth - 1, out); Split(mbc, c, mca, eye, depth - 1, out);
        } else if (ca && ab) {
            Split(c, mca, b, eye, depth - 1, out); Split(mca, mab, b, eye, depth - 1, out); Split(mca, a, mab, eye, depth - 1, out);
        } else if (ab) {
            Split(a, mab, c, eye, depth - 1, out); Split(mab, b, c, eye, depth - 1, out);
        } else if (bc) {
            Split(b, mbc, a, eye, depth - 1, out); Split(mbc, c, a, eye, depth - 1, out);
        } else {
            Split(c, mca, b, eye, depth - 1, out); Split(mca, a, b, eye, depth - 1, out);
        }
    }
    static void BuildDense(Dense& out, Vec3 eye, const std::vector<Vertex>& tris, const std::unordered_map<uint64_t, std::vector<uint32_t>>& cells) {
        std::vector<uint32_t> seen;
        const int reach = (int)std::ceil((kWaterNearHalf + 16.0f) / kCell) + 1;
        const int cx = (int)std::floor(out.centre[0] / kCell), cz = (int)std::floor(out.centre[1] / kCell);
        for (int z = cz - reach; z <= cz + reach; ++z)
            for (int x = cx - reach; x <= cx + reach; ++x) {
                const auto it = cells.find(TileKey(x, z));
                if (it != cells.end()) seen.insert(seen.end(), it->second.begin(), it->second.end());
            }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (uint32_t t : seen) {
            const Vertex& a = tris[t], & b = tris[t + 1], & c = tris[t + 2];
            const float x0 = std::min({a.px, b.px, c.px}), x1 = std::max({a.px, b.px, c.px}), z0 = std::min({a.pz, b.pz, c.pz}), z1 = std::max({a.pz, b.pz, c.pz});
            if (x1 < out.centre[0] - kWaterNearHalf || x0 > out.centre[0] + kWaterNearHalf || z1 < out.centre[1] - kWaterNearHalf || z0 > out.centre[1] + kWaterNearHalf) continue;
            Split(a, b, c, eye, 10, out);
            if (out.vertices.size() > 900000) break; // a ceiling, against a bad map
        }
        out.done.store(true, std::memory_order_release);
    }

    // ---- reactions
    void Touch(uint64_t id, Vec3 center, float radius, float dt) {
        float surface = 0.0f;
        if (!HeightAt(center.x, center.z, surface)) { colliders_.erase(id); return; }
        const float gap = center.y - surface;
        const bool crossing = std::fabs(gap) < radius;
        auto it = colliders_.find(id);
        if (it == colliders_.end()) { colliders_[id] = {center, gap < 0.0f}; return; }
        Tracked& track = it->second;
        const Vec3 step = center - track.last;
        const float moved = std::sqrt(Dot(step, step));
        const float velY = dt > 0.0f ? step.y / dt : 0.0f;
        const float velFlat = dt > 0.0f ? std::sqrt(step.x * step.x + step.z * step.z) / dt : 0.0f;
        track.last = center;
        const bool under = gap < 0.0f;
        const bool entered = under != track.under; // through the surface: a splash once
        track.under = under;
        if (!crossing || moved < 1e-5f || moved > 20.0f) return;
        const float section = std::sqrt(std::max(radius * radius - gap * gap, 0.01f)); // what the body's circle at the surface is
        const float share = std::min(1.0f, dt * 60.0f);
        if (entered) AddImpulse(center, section * 1.1f, std::clamp(std::fabs(velY) * 0.35f, 0.1f, 1.5f));
        // A body in motion heaves water up ahead and leaves a hollow behind: a pair of pushes along its path. That is its wake.
        if (velFlat > 0.05f && dt > 0.0f) {
            const float inv = 1.0f / std::max(velFlat * dt, 1e-5f);
            const Vec3 forward = {step.x * inv, 0.0f, step.z * inv};
            const float push = std::min(velFlat, 8.0f) * 0.045f * share;
            AddImpulse(center + forward * (section * 0.9f), section * 0.75f, -push);
            AddImpulse(center - forward * (section * 0.9f), section * 0.85f, push);
        }
        if (std::fabs(velY) > 0.05f) AddImpulse(center, section, velY > 0.0f ? -std::min(velY, 6.0f) * 0.04f * share : std::min(-velY, 6.0f) * 0.04f * share);
    }

    bool CreateGpu(Renderer& r) {
        csWaves_ = r.CreateComputeShaderIn("water", "CSWaves");
        csRipple_ = r.CreateComputeShaderIn("water", "CSRipple");
        csSky_ = r.CreateComputeShaderIn("water", "CSSky");
        if (csWaves_ == kNoComputeShader || csRipple_ == kNoComputeShader || csSky_ == kNoComputeShader) { Log("Water: compute shaders failed"); chunks_.clear(); return false; }
        unused_ = r.CreateGpuBuffer(GpuBufferUse::Read, 16, nullptr);
        waves_ = r.CreateGpuBuffer(GpuBufferUse::ShaderData, (uint64_t)kWaterTile * kWaterTile * kWaterCascades * 2 * 16, nullptr);
        sky_ = r.CreateGpuBuffer(GpuBufferUse::ShaderData, 32 * 16 * 16, nullptr);
        const std::vector<float> empty(kWaterRippleSize * kWaterRippleSize * 6, 0.0f);
        ripple_[0] = r.CreateGpuBuffer(GpuBufferUse::ShaderData, empty.size() * sizeof(float), empty.data());
        ripple_[1] = r.CreateGpuBuffer(GpuBufferUse::ShaderData, empty.size() * sizeof(float), empty.data());
        gpuReady_ = waves_ != kNoGpuBuffer && sky_ != kNoGpuBuffer && ripple_[0] != kNoGpuBuffer && ripple_[1] != kNoGpuBuffer;
        return gpuReady_;
    }

    void StepRipples(Renderer& r, const GpuBufferId reads[4], float dt) {
        // A fixed step, so a capture reproduces and the wave speed stays stable; two at most in a frame, so a hitch cannot snowball.
        constexpr float kStep = 1.0f / 120.0f;
        constexpr float kSpeed = 3.4f; // metres per second: the speed of a metre-long wave on deep water, and stable at this cell size
        accumulator_ = std::min(accumulator_ + dt, kStep * 2.0f);
        int steps = 0;
        while (accumulator_ >= kStep && steps < 2) { accumulator_ -= kStep; ++steps; }
        if (steps == 0) { if (!impulses_.empty()) steps = 1; else return; }

        // The window slides by whole cells so the simulation's grid never swims under the camera.
        const int wantX = (int)std::floor(eye_.x / kWaterCell) - (int)kWaterRippleSize / 2;
        const int wantZ = (int)std::floor(eye_.z / kWaterCell) - (int)kWaterRippleSize / 2;
        int shiftX = wantX - originCell_[0], shiftZ = wantZ - originCell_[1];
        if (!originValid_) { shiftX = shiftZ = 100000; originValid_ = true; }
        originCell_[0] = wantX;
        originCell_[1] = wantZ;

        for (int s = 0; s < steps; ++s) {
            ComputeConstants pc = {};
            pc.a[0] = kStep;
            pc.a[1] = kSpeed * kSpeed / (kWaterCell * kWaterCell);
            pc.a[2] = kWaterCell;
            pc.b[0] = (float)shiftX; pc.b[1] = (float)shiftZ;
            pc.b[2] = (float)originCell_[0] * kWaterCell; pc.b[3] = (float)originCell_[1] * kWaterCell;
            const size_t take = s + 1 == steps ? std::min<size_t>(impulses_.size(), 6) : std::min<size_t>(impulses_.size(), 3);
            for (size_t i = 0; i < take; ++i) {
                const Impulse& m = impulses_[i];
                pc.impulses[i][0] = m.x; pc.impulses[i][1] = m.z; pc.impulses[i][2] = m.radius; pc.impulses[i][3] = m.strength;
            }
            pc.a[3] = (float)take;
            impulses_.erase(impulses_.begin(), impulses_.begin() + take);
            totalImpulses_ += (uint32_t)take;
            const GpuBufferId writes[3] = {ripple_[current_], ripple_[1 - current_], sky_};
            r.Dispatch(csRipple_, &pc, sizeof(pc), reads, writes, kWaterRippleSize / 8, kWaterRippleSize / 8, 1);
            current_ = 1 - current_;
            shiftX = shiftZ = 0;
            ++steps_;
        }
        if (impulses_.size() > 48) impulses_.erase(impulses_.begin(), impulses_.begin() + (impulses_.size() - 48)); // old news
    }

    JobSystem* jobs_ = nullptr;
    std::shared_ptr<Build> building_;
    std::shared_ptr<Dense> dense_;
    std::shared_ptr<MapData> map_;
    std::vector<Chunk> chunks_;
    std::vector<uint8_t> surfaceFlags_;
    std::vector<MeshId> visibleChunks_;
    std::unordered_map<uint64_t, Tracked> colliders_;
    std::vector<Impulse> impulses_;
    MeshId nearMesh_ = kInvalidMesh;
    uint32_t nearCount_ = 0, nearTriangles_ = 0, steps_ = 0, totalImpulses_ = 0;
    GpuBufferId waves_ = kNoGpuBuffer, sky_ = kNoGpuBuffer, ripple_[2] = {kNoGpuBuffer, kNoGpuBuffer}, unused_ = kNoGpuBuffer;
    ComputeShaderId csRipple_ = kNoComputeShader, csWaves_ = kNoComputeShader, csSky_ = kNoComputeShader;
    bool gpuReady_ = false, visibleFrame_ = false, everBuilt_ = false, originValid_ = false;
    int current_ = 0, originCell_[2] = {0, 0};
    float time_ = 0.0f, accumulator_ = 0.0f, nearCentre_[2] = {0, 0}, sunDir_[3] = {0, 1, 0};
    Vec3 eye_ = {0, 0, 0}, builtAt_ = {0, 0, 0};
};

} // namespace tbd
