// Procedural open-world terrain: an infinite heightfield cut into square chunks that stream in
// around the camera and out again behind it. Distant chunks use fewer cells (LOD) and carry a
// short skirt so neighbours of different detail never show cracks.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "render/renderer.h"
#include "city_map.h"
#include "island_map.h"
#include "render/mesh_weld.h"
#include "terrain_field.h"
#include "vecmath.h"

namespace tbd {

struct TerrainParams {
    bool enabled = true;
    int seed = 1337;
    float amplitude = 40.0f;   // world units from valley floor to peak, roughly
    float frequency = 1.0f / 260.0f;
    int radius = 8;            // chunks loaded around the camera
    bool flatStart = true;     // keep the ground around the origin flat; a terrain sandbox turns this off so all of it is hills
    int mapKind = 0;           // 0: endless procedural hills; 1: the hand-shaped island with sea and river (island_map.h); 2: the random city map, north land / south sea (city_map.h)
    float relief = 1.0f;       // mapKind 2: the hills' height scale (CityMap::Height). 1 = the original hilly map (what saves without a T record were made on); new maps use CityMap::kDefaultRelief
};

class Terrain {
public:
    static constexpr float kChunkSize = 64.0f;

    TerrainParams params;

    // Player edits laid over the procedural ground (see terrain_field.h and docs/terrain_plan.md). Empty until the first brush.
    HeightField field;

    // Height of the ground at a world position, edits included.
    float HeightAt(float x, float z) const { return BaseHeightAt(x, z) + field.EditAt(x, z); }

    // A round brush on the edited ground; chunks and far rings that overlap it rebuild over the next few frames.
    void Brush(float cx, float cz, float radius, float strength, HeightField::BrushMode mode = HeightField::BrushMode::Add, float target = 0.0f, float hardRadius = 0.0f) {
        field.ApplyBrush(cx, cz, radius, strength, mode, target, [this](float x, float z) { return BaseHeightAt(x, z); }, hardRadius);
    }

    // The procedural ground without edits. The area around the origin is kept flat so the
    // default scene (a ground plane and a box) sits on it.
    float BaseHeightAt(float x, float z) const {
        if (params.mapKind == 1) return IslandMap::Height(x, z);
        if (params.mapKind == 2) return CityMap::Height(x, z, params.seed, params.relief);
        const float d = std::sqrt(x * x + z * z);
        const float mask = params.flatStart ? Smooth((d - 30.0f) / 70.0f) : 1.0f;
        float sum = 0, amp = 1, total = 0, freq = params.frequency;
        for (int octave = 0; octave < 5; ++octave) {
            sum += amp * ValueNoise(x * freq, z * freq, params.seed + octave * 101);
            total += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        const float n = (sum / total - 0.5f) * 2.0f; // about -1 .. 1
        // Mountain ranges on the horizon: ridged noise that rises only well away from the origin.
        const float rangeMask = Smooth((d - 500.0f) / 2500.0f);
        float ridge = 0.0f;
        if (rangeMask > 0.0f) {
            float a = 1.0f, f = 1.0f / 2600.0f;
            for (int octave = 0; octave < 4; ++octave, a *= 0.48f, f *= 2.1f) {
                const float r = 1.0f - std::fabs(ValueNoise(x * f, z * f, params.seed + 500 + octave) * 2.0f - 1.0f);
                ridge += a * r * r;
            }
        }
        return n * params.amplitude * mask + ridge * 900.0f * rangeMask - 0.04f;
    }

    // First point where the ray meets the ground, searched out to `maxDist`.
    bool Raycast(const Ray& ray, float maxDist, Vec3& hit) const {
        auto clearance = [&](float t) {
            const Vec3 p = ray.origin + ray.dir * t;
            return p.y - HeightAt(p.x, p.z);
        };
        if (clearance(0.0f) < 0.0f) return false; // the camera is underground
        float t = 0.0f;
        while (t < maxDist) {
            const float next = t + std::fmax(0.5f, t * 0.01f);
            if (clearance(next) < 0.0f) {
                float lo = t, hi = next;
                for (int i = 0; i < 14; ++i) {
                    const float mid = 0.5f * (lo + hi);
                    (clearance(mid) < 0.0f ? hi : lo) = mid;
                }
                hit = ray.origin + ray.dir * (0.5f * (lo + hi));
                return true;
            }
            t = next;
        }
        return false;
    }

    int LoadedChunks() const { return (int)chunks_.size(); }
    // Rebuilt at most one level per Update. Each level's grid is centered on the camera, snapped to its own coarse step.
    void UpdateFar(Renderer& r, Vec3 center) {
        const float nearHole = (float)params.radius * kChunkSize * 0.70f;
        for (int level = 0; level < kFarLevels; ++level) {
            FarLevel& f = far_[level];
            const float cell = kFarCell0 * (float)(1 << (level * 2));
            const float snap = cell * 8.0f;
            const int sx = (int)std::floor(center.x / snap), sz = (int)std::floor(center.z / snap);
            const float ext = FarExtent(level), ccx = ((float)sx + 0.5f) * snap, ccz = ((float)sz + 0.5f) * snap;
            const bool placed = f.mesh != kInvalidMesh && f.sx == sx && f.sz == sz && f.seed == params.seed && f.amp == params.amplitude && f.relief == params.relief &&
                                f.radius == params.radius && f.kind == params.mapKind;
            // Edits under a ring rebuild it, but at most twice a second: a brush drag changes the field every frame.
            const bool stale = placed && f.editVersion != field.TileVersionInRect(ccx - ext, ccz - ext, ccx + ext, ccz + ext) &&
                               NowSeconds() - f.builtAt >= kFarEditInterval;
            if (placed && !stale) continue;
            BuildFar(r, f, level, sx, sz, cell, snap, level == 0 ? nearHole : FarExtent(level - 1) - cell * 2.0f);
            return;
        }
    }
    int LoadedTriangles() const { return triangles_; }

    // Streams chunks around `center`. At most `budget` chunks are (re)built per call, nearest first.
    void Update(Renderer& r, Vec3 center, int budget = 3) {
        if (!params.enabled) {
            Clear(r);
            return;
        }
        UpdateWater(r);
        const int ccx = (int)std::floor(center.x / kChunkSize), ccz = (int)std::floor(center.z / kChunkSize);

        // Unload chunks that fell out of range (one ring of hysteresis so edges don't thrash).
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            const int dx = it->second.cx - ccx, dz = it->second.cz - ccz;
            if (dx * dx + dz * dz > (params.radius + 1) * (params.radius + 1)) {
                triangles_ -= it->second.triangles;
                r.RetireMesh(it->second.mesh);
                it = chunks_.erase(it);
            } else {
                ++it;
            }
        }

        // Chunks that are missing or at the wrong detail, nearest first.
        struct Want {
            int cx, cz, lod, dist2;
        };
        std::vector<Want> wanted;
        for (int dz = -params.radius; dz <= params.radius; ++dz)
            for (int dx = -params.radius; dx <= params.radius; ++dx) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > params.radius * params.radius) continue;
                const int lod = d2 < 3 * 3 ? 0 : (d2 < 5 * 5 ? 1 : 2);
                auto it = chunks_.find(Key(ccx + dx, ccz + dz));
                if (it == chunks_.end() || it->second.lod != lod || it->second.seed != params.seed ||
                    it->second.amp != params.amplitude || it->second.relief != params.relief || it->second.kind != params.mapKind ||
                    it->second.editVersion != ChunkEditVersion(ccx + dx, ccz + dz))
                    wanted.push_back({ccx + dx, ccz + dz, lod, d2});
            }
        std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.dist2 < b.dist2; });
        for (int i = 0; i < (int)wanted.size() && i < budget; ++i) Build(r, wanted[i].cx, wanted[i].cz, wanted[i].lod);
    }

    // Draws every loaded chunk that is not clearly behind the camera.
    void Draw(Renderer& r, const Mat4& viewProj, Vec3 eye, Vec3 forward) const {
        const Mat4 identity = Mat4::Identity();
        const float reach = kChunkSize * 0.75f; // half the diagonal plus the skirt and height range
        for (const auto& kv : chunks_) {
            const Chunk& c = kv.second;
            const Vec3 mid = {(c.cx + 0.5f) * kChunkSize, 0.0f, (c.cz + 0.5f) * kChunkSize};
            const Vec3 to = {mid.x - eye.x, 0.0f, mid.z - eye.z};
            const float len = std::sqrt(Dot(to, to));
            if (len > reach * 1.5f && Dot(to, forward) < -reach * std::fmax(0.3f, std::sqrt(1.0f - forward.y * forward.y)))
                continue; // behind the camera
            r.Draw(c.mesh, viewProj, identity, true, true);
        }
        for (const FarLevel& f : far_)
            if (f.mesh != kInvalidMesh) r.Draw(f.mesh, viewProj, identity, true, true);
        if (seaMesh_ != kInvalidMesh) r.Draw(seaMesh_, viewProj, identity, true, true);
        if (riverMesh_ != kInvalidMesh) r.Draw(riverMesh_, viewProj, identity, true, true);
    }

    // Draws the chunks near `center` into the shadow pass.
    void DrawShadowCasters(Renderer& r, Vec3 center, float radius) const {
        const Mat4 identity = Mat4::Identity();
        const float reach = radius + kChunkSize * 0.75f;
        for (const auto& kv : chunks_) {
            const Chunk& c = kv.second;
            const float dx = (c.cx + 0.5f) * kChunkSize - center.x, dz = (c.cz + 0.5f) * kChunkSize - center.z;
            if (dx * dx + dz * dz <= reach * reach) r.DrawShadowCaster(c.mesh, identity);
        }
    }

    // Changes when DrawShadowCasters would draw other chunks. Independent of the map's order.
    uint64_t ShadowCastersKey(Vec3 center, float radius) const {
        const float reach = radius + kChunkSize * 0.75f;
        uint64_t key = 0;
        for (const auto& kv : chunks_) {
            const Chunk& c = kv.second;
            const float dx = (c.cx + 0.5f) * kChunkSize - center.x, dz = (c.cz + 0.5f) * kChunkSize - center.z;
            if (dx * dx + dz * dz <= reach * reach) key += (kv.first ^ ((uint64_t)c.mesh << 20)) * 0x9E3779B97F4A7C15ull;
        }
        return key;
    }

    void Clear(Renderer& r) {
        for (auto& kv : chunks_) r.DestroyMesh(kv.second.mesh);
        chunks_.clear();
        triangles_ = 0;
        if (seaMesh_ != kInvalidMesh) r.DestroyMesh(seaMesh_);
        if (riverMesh_ != kInvalidMesh) r.DestroyMesh(riverMesh_);
        seaMesh_ = riverMesh_ = kInvalidMesh;
        for (FarLevel& f : far_) {
            if (f.mesh != kInvalidMesh) r.DestroyMesh(f.mesh);
            f = {};
        }
    }

    // The renderer was torn down (API switch, window pop-out): its meshes are gone already.
    void ForgetMeshes() {
        chunks_.clear();
        triangles_ = 0;
        seaMesh_ = riverMesh_ = kInvalidMesh;
        for (FarLevel& f : far_) f = {};
    }

private:
    static constexpr int kFarLevels = 3, kFarHalfCells = 48;
    static constexpr float kFarCell0 = 32.0f; // each level's cells are four times the last: 32, 128, 512 m
    static float FarExtent(int level) { return kFarCell0 * (float)(1 << (level * 2)) * (float)kFarHalfCells; }

    struct FarLevel {
        MeshId mesh = kInvalidMesh;
        int sx = 0, sz = 0, seed = 0, radius = 0;
        float amp = 0.0f;
        uint32_t editVersion = 0; // HeightField::TileVersionInRect over the ring when built
        double builtAt = 0.0;     // seconds; edits rebuild a ring at most every kFarEditInterval
        int kind = -1;            // TerrainParams::mapKind when built
        float relief = -1.0f;     // TerrainParams::relief when built
    };
    static constexpr double kFarEditInterval = 0.5;

    // Newest edit version among the 64 m blocks under a chunk, 0 if untouched. The field marks a neighbouring block too when an edit
    // lies within 2 samples of the seam, so the chunk's rectangle needs no margin of its own.
    uint32_t ChunkEditVersion(int cx, int cz) const {
        return field.VersionInRect((float)cx * kChunkSize, (float)cz * kChunkSize, (float)(cx + 1) * kChunkSize - 0.01f, (float)(cz + 1) * kChunkSize - 0.01f);
    }

    // A square ring: the grid minus a hole that the next finer level fills. It sits slightly low, so where two levels
    // overlap the finer one wins without seams.
    void BuildFar(Renderer& r, FarLevel& f, int level, int sx, int sz, float cell, float snap, float hole) {
        const float cx = ((float)sx + 0.5f) * snap, cz = ((float)sz + 0.5f) * snap;
        const int cells = kFarHalfCells * 2, n = cells + 1;
        const float x0 = cx - cell * kFarHalfCells, z0 = cz - cell * kFarHalfCells;
        const float drop = 1.5f + cell * 0.04f;
        std::vector<Vertex> grid((size_t)n * n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float x = x0 + (float)i * cell, z = z0 + (float)j * cell;
                const Vec3 nrm = NormalAt(x, z, cell);
                grid[(size_t)j * n + i] = {x, HeightAt(x, z) - drop, z, nrm.x, nrm.y, nrm.z, 1.0f, 1.0f, 1.0f, kTerrainAlpha};
            }
        std::vector<Vertex> v;
        v.reserve((size_t)cells * cells * 6);
        for (int j = 0; j < cells; ++j)
            for (int i = 0; i < cells; ++i) {
                const float mx = x0 + ((float)i + 0.5f) * cell - cx - (0.0f), mz = z0 + ((float)j + 0.5f) * cell - cz;
                // The hole is around the camera, which is within one snap of the grid's center.
                if (std::fmax(std::fabs(mx), std::fabs(mz)) < hole - snap) continue;
                const Vertex& a = grid[(size_t)j * n + i];
                const Vertex& b = grid[(size_t)j * n + i + 1];
                const Vertex& c = grid[(size_t)(j + 1) * n + i + 1];
                const Vertex& d = grid[(size_t)(j + 1) * n + i];
                for (const Vertex* p : {&a, &d, &c, &a, &c, &b}) v.push_back(*p);
            }
        if (f.mesh != kInvalidMesh) r.RetireMesh(f.mesh);
        f.mesh = v.empty() ? kInvalidMesh : MakeMesh(r, v);
        f.sx = sx;
        f.sz = sz;
        f.seed = params.seed;
        f.amp = params.amplitude;
        f.radius = params.radius;
        f.editVersion = field.TileVersionInRect(cx - FarExtent(level), cz - FarExtent(level), cx + FarExtent(level), cz + FarExtent(level));
        f.builtAt = NowSeconds();
        f.kind = params.mapKind;
        f.relief = params.relief;
    }

    static double NowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Welds a triangle list and uploads it without waiting for the GPU (CreateMesh stalls the frame until the copy is done, which
    // made every rebuilt chunk cost a frame hitch). The old mesh is retired, not destroyed, for the same reason.
    static MeshId MakeMesh(Renderer& r, const std::vector<Vertex>& triangles) {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        WeldVertices(triangles.data(), (uint32_t)triangles.size(), verts, indices);
        return r.CreateMeshNoWait(verts.data(), (uint32_t)verts.size(), indices.data(), (uint32_t)indices.size());
    }

    struct Chunk {
        int cx, cz, lod, triangles;
        int seed;
        float amp;
        MeshId mesh;
        uint32_t editVersion;
        int kind;
        float relief;
    };

    static uint64_t Key(int cx, int cz) { return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz; }

    static float Smooth(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    static float Hash(int x, int z, int seed) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000; // 0 .. 1
    }

    static float ValueNoise(float x, float z, int seed) {
        const int ix = (int)std::floor(x), iz = (int)std::floor(z);
        const float fx = Smooth(x - (float)ix), fz = Smooth(z - (float)iz);
        const float a = Hash(ix, iz, seed), b = Hash(ix + 1, iz, seed);
        const float c = Hash(ix, iz + 1, seed), d = Hash(ix + 1, iz + 1, seed);
        return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
    }

    Vec3 NormalAt(float x, float z, float step) const {
        const float hl = HeightAt(x - step, z), hr = HeightAt(x + step, z);
        const float hd = HeightAt(x, z - step), hu = HeightAt(x, z + step);
        return Normalize(Vec3{hl - hr, 2.0f * step, hd - hu});
    }

    static constexpr float kTerrainAlpha = 0.0f; // vertex alpha that selects the terrain material in mesh.hlsl

    void Build(Renderer& r, int cx, int cz, int lod) {
        const int cells = lod == 0 ? 32 : (lod == 1 ? 16 : 8);
        const float step = kChunkSize / (float)cells;
        const float x0 = (float)cx * kChunkSize, z0 = (float)cz * kChunkSize;
        const int n = cells + 1;

        struct P {
            Vec3 pos, normal, color;
        };
        std::vector<P> grid((size_t)n * n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float x = x0 + (float)i * step, z = z0 + (float)j * step;
                const float h = HeightAt(x, z);
                const Vec3 nrm = NormalAt(x, z, std::fmax(step, 1.0f));
                grid[(size_t)j * n + i] = {{x, h, z}, nrm, {1.0f, 1.0f, 1.0f}};
            }

        std::vector<Vertex> v;
        v.reserve((size_t)cells * cells * 6 + (size_t)cells * 4 * 6);
        auto push = [&](const P& p, float drop) {
            v.push_back({p.pos.x, p.pos.y - drop, p.pos.z, p.normal.x, p.normal.y, p.normal.z, p.color.x, p.color.y,
                         p.color.z, kTerrainAlpha});
        };
        for (int j = 0; j < cells; ++j)
            for (int i = 0; i < cells; ++i) {
                const P& a = grid[(size_t)j * n + i];
                const P& b = grid[(size_t)j * n + i + 1];
                const P& c = grid[(size_t)(j + 1) * n + i + 1];
                const P& d = grid[(size_t)(j + 1) * n + i];
                for (const P* p : {&a, &d, &c, &a, &c, &b}) push(*p, 0.0f); // counter-clockwise from above
            }

        // Skirts: a strip hanging down from each outer edge hides cracks against coarser neighbours.
        const float skirt = 3.0f + (float)lod * 2.0f;
        auto edge = [&](int i0, int j0, int i1, int j1, bool flip) {
            const P& a = grid[(size_t)j0 * n + i0];
            const P& b = grid[(size_t)j1 * n + i1];
            const P* order[6] = {&a, &b, &b, &a, &b, &a};
            const float drop[6] = {0, 0, skirt, 0, skirt, skirt};
            if (flip) {
                for (int k : {0, 2, 1, 3, 5, 4}) push(*order[k], drop[k]);
            } else {
                for (int k = 0; k < 6; ++k) push(*order[k], drop[k]);
            }
        };
        for (int i = 0; i < cells; ++i) {
            edge(i, 0, i + 1, 0, false);
            edge(i, cells, i + 1, cells, true);
            edge(0, i, 0, i + 1, true);
            edge(cells, i, cells, i + 1, false);
        }
        // Skirts are visible from both sides for simplicity; the pipeline does not cull, so no extra faces.

        const MeshId mesh = MakeMesh(r, v);
        if (mesh == kInvalidMesh) return;
        const uint64_t key = Key(cx, cz);
        auto it = chunks_.find(key);
        if (it != chunks_.end()) {
            triangles_ -= it->second.triangles;
            r.RetireMesh(it->second.mesh);
        }
        const int tris = (int)(v.size() / 3);
        chunks_[key] = {cx, cz, lod, tris, params.seed, params.amplitude, mesh, ChunkEditVersion(cx, cz), params.mapKind, params.relief};
        triangles_ += tris;
    }

    // The island's sea and river: flat lit meshes made once. They do not follow terrain edits (a dug lake stays dry for now).
    void UpdateWater(Renderer& r) {
        const bool want = params.mapKind >= 1;
        if (want && seaMesh_ != kInvalidMesh && seaKind_ != params.mapKind) {
            r.DestroyMesh(seaMesh_);
            if (riverMesh_ != kInvalidMesh) r.DestroyMesh(riverMesh_);
            seaMesh_ = riverMesh_ = kInvalidMesh;
        }
        if (!want) {
            if (seaMesh_ != kInvalidMesh) r.DestroyMesh(seaMesh_);
            if (riverMesh_ != kInvalidMesh) r.DestroyMesh(riverMesh_);
            seaMesh_ = riverMesh_ = kInvalidMesh;
            return;
        }
        if (seaMesh_ != kInvalidMesh) return;
        std::vector<Vertex> sea, river;
        if (params.mapKind == 2) CityMap::BuildWater(sea);
        else IslandMap::BuildWater(sea, river);
        seaKind_ = params.mapKind;
        seaMesh_ = r.CreateMesh(sea.data(), (uint32_t)sea.size(), Topology::Triangles);
        if (!river.empty()) riverMesh_ = r.CreateMesh(river.data(), (uint32_t)river.size(), Topology::Triangles);
    }

    MeshId seaMesh_ = kInvalidMesh, riverMesh_ = kInvalidMesh;
    int seaKind_ = 0;
    std::unordered_map<uint64_t, Chunk> chunks_;
    FarLevel far_[kFarLevels];
    int triangles_ = 0;
};

} // namespace tbd
