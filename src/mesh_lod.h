// Automatic LODs: quadric error edge collapse (Garland and Heckbert, "Surface Simplification Using Quadric Error Metrics",
// 1997). Each collapse moves a vertex onto a neighbour that already exists, so every kept vertex keeps its authored normal, color
// and UV. Vertices on an open border or a UV/normal seam (one position, several vertices) never move, which keeps silhouettes,
// tile edges and texture layout intact at the price of less reduction on very seamy meshes. One pass makes the whole chain:
// a snapshot each time the live triangle count crosses the next target. Renderer-agnostic, touches no shared state.
#pragma once
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>


#include "render/renderer.h"

namespace tbd {

struct LodLevel {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float error = 0.0f; // meters in the mesh's own space: how far this level may sit from the full mesh
};

namespace lod_detail {

struct Quadric { // the symmetric 4x4 of Garland and Heckbert, upper triangle
    double q[10] = {};
    void AddPlane(double a, double b, double c, double d) {
        q[0] += a * a; q[1] += a * b; q[2] += a * c; q[3] += a * d;
        q[4] += b * b; q[5] += b * c; q[6] += b * d;
        q[7] += c * c; q[8] += c * d; q[9] += d * d;
    }
    void Add(const Quadric& o) { for (int i = 0; i < 10; ++i) q[i] += o.q[i]; }
    double At(double x, double y, double z) const { // summed squared distance to the planes
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y +
               q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

struct PositionKey {
    float p[3];
    bool operator==(const PositionKey& o) const { return std::memcmp(p, o.p, sizeof(p)) == 0; }
};
struct PositionHash {
    size_t operator()(const PositionKey& k) const {
        uint32_t h[3];
        std::memcpy(h, k.p, sizeof(h));
        return (size_t)(h[0] * 73856093u ^ h[1] * 19349663u ^ h[2] * 83492791u);
    }
};

struct Collapse {
    double cost;
    uint32_t from, to, stampFrom, stampTo;
    bool operator>(const Collapse& o) const { return cost > o.cost; }
};

inline LodLevel Snapshot(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& corner, const std::vector<uint8_t>& dead, float error) {
    LodLevel level;
    level.error = error;
    std::vector<uint32_t> remap(vertices.size(), UINT32_MAX);
    for (size_t t = 0; t < dead.size(); ++t) {
        if (dead[t]) continue;
        for (int k = 0; k < 3; ++k) {
            uint32_t& to = remap[corner[t * 3 + k]];
            if (to == UINT32_MAX) { to = (uint32_t)level.vertices.size(); level.vertices.push_back(vertices[corner[t * 3 + k]]); }
            level.indices.push_back(to);
        }
    }
    return level;
}

} // namespace lod_detail

// The coarser levels of a welded mesh, each about `step` times the triangles of the one before, down to ~64 triangles and at most
// four levels. Empty when the mesh is too small to bother or cannot be reduced. `radius` caps the error: a level further than
// half the mesh's size from the original is no longer the same object.
inline std::vector<LodLevel> BuildLodChain(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, float step, float radius) {
    using namespace lod_detail;
    std::vector<LodLevel> levels;
    const size_t triangleCount = indices.size() / 3;
    if (triangleCount < 256 || step <= 0.05f || step >= 0.95f) return levels;
    std::vector<size_t> targets;
    for (double t = (double)triangleCount * step; t >= 64.0 && targets.size() < 4; t *= step) targets.push_back((size_t)t);

    // One canonical id per position; a position with several vertices is a seam.
    const uint32_t n = (uint32_t)vertices.size();
    std::unordered_map<PositionKey, uint32_t, PositionHash> byPosition;
    byPosition.reserve(n);
    std::vector<uint32_t> canon(n), rep, groupSize;
    for (uint32_t i = 0; i < n; ++i) {
        const auto [it, added] = byPosition.try_emplace(PositionKey{{vertices[i].px, vertices[i].py, vertices[i].pz}}, (uint32_t)rep.size());
        if (added) { rep.push_back(i); groupSize.push_back(0); }
        canon[i] = it->second;
        ++groupSize[it->second];
    }
    const uint32_t c = (uint32_t)rep.size();
    std::vector<uint32_t> tri(indices.size()), corner(indices);
    std::vector<uint8_t> dead(triangleCount, 0);
    std::vector<std::vector<uint32_t>> around(c);
    std::vector<Quadric> quadric(c);
    std::unordered_map<uint64_t, uint32_t> edgeUses;
    edgeUses.reserve(triangleCount * 2);
    const auto edgeKey = [](uint32_t a, uint32_t b) { return a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a; };
    const auto pos = [&](uint32_t id) { const Vertex& v = vertices[rep[id]]; return Vec3{v.px, v.py, v.pz}; };
    size_t live = 0;
    for (size_t t = 0; t < triangleCount; ++t) {
        for (int k = 0; k < 3; ++k) tri[t * 3 + k] = canon[indices[t * 3 + k]];
        const uint32_t a = tri[t * 3], b = tri[t * 3 + 1], d = tri[t * 3 + 2];
        if (a == b || b == d || a == d) { dead[t] = 1; continue; }
        const Vec3 pa = pos(a), nrm = Cross(pos(b) - pa, pos(d) - pa);
        const float len = std::sqrt(Dot(nrm, nrm));
        if (len > 1e-12f) {
            const Vec3 u = {nrm.x / len, nrm.y / len, nrm.z / len};
            for (uint32_t id : {a, b, d}) quadric[id].AddPlane(u.x, u.y, u.z, -Dot(u, pa));
        }
        for (uint32_t id : {a, b, d}) around[id].push_back((uint32_t)t);
        ++edgeUses[edgeKey(a, b)]; ++edgeUses[edgeKey(b, d)]; ++edgeUses[edgeKey(d, a)];
        ++live;
    }
    std::vector<uint8_t> locked(c, 0), alive(c, 1);
    for (uint32_t id = 0; id < c; ++id) locked[id] = groupSize[id] > 1;
    for (const auto& [key, uses] : edgeUses)
        if (uses == 1) { locked[(uint32_t)(key >> 32)] = 1; locked[(uint32_t)key] = 1; } // an open border
    std::vector<uint32_t> stamp(c, 0);
    std::priority_queue<Collapse, std::vector<Collapse>, std::greater<Collapse>> heap;
    const auto push = [&](uint32_t from, uint32_t to) {
        if (locked[from] || groupSize[to] > 1) return; // seams neither move nor take a vertex: theirs would be ambiguous
        Quadric q = quadric[from];
        q.Add(quadric[to]);
        const Vec3 p = pos(to);
        heap.push({std::max(0.0, q.At(p.x, p.y, p.z)), from, to, stamp[from], stamp[to]});
    };
    for (const auto& [key, uses] : edgeUses) {
        push((uint32_t)(key >> 32), (uint32_t)key);
        push((uint32_t)key, (uint32_t)(key >> 32));
    }
    std::unordered_map<uint64_t, uint32_t>().swap(edgeUses);

    const double maxError = (double)radius * 0.5;
    float error = 0.0f;
    size_t next = 0;
    while (next < targets.size() && !heap.empty()) {
        const Collapse top = heap.top();
        heap.pop();
        const uint32_t a = top.from, b = top.to;
        if (!alive[a] || !alive[b] || top.stampFrom != stamp[a] || top.stampTo != stamp[b]) continue;
        if (std::sqrt(top.cost) > maxError) break;
        // No triangle around `a` may fold over or collapse to a sliver once `a` sits on `b`.
        bool ok = true, sharesEdge = false;
        const Vec3 pb = pos(b);
        for (uint32_t t : around[a]) {
            if (dead[t]) continue;
            const uint32_t* v = &tri[(size_t)t * 3];
            if (v[0] == b || v[1] == b || v[2] == b) { sharesEdge = true; continue; }
            Vec3 p[3], q[3];
            for (int k = 0; k < 3; ++k) { p[k] = pos(v[k]); q[k] = v[k] == a ? pb : p[k]; }
            const Vec3 before = Cross(p[1] - p[0], p[2] - p[0]), after = Cross(q[1] - q[0], q[2] - q[0]);
            const float lb = Dot(before, before), la = Dot(after, after);
            if (la <= lb * 1e-6f || Dot(before, after) < 0.25f * std::sqrt(lb * la)) { ok = false; break; }
        }
        if (!ok || !sharesEdge) continue;
        for (uint32_t t : around[a]) {
            if (dead[t]) continue;
            uint32_t* v = &tri[(size_t)t * 3];
            if (v[0] == b || v[1] == b || v[2] == b) { dead[t] = 1; --live; continue; }
            for (int k = 0; k < 3; ++k)
                if (v[k] == a) { v[k] = b; corner[(size_t)t * 3 + k] = rep[b]; }
            around[b].push_back(t);
        }
        std::vector<uint32_t>().swap(around[a]);
        alive[a] = 0;
        quadric[b].Add(quadric[a]);
        ++stamp[b];
        error = std::max(error, (float)std::sqrt(top.cost));
        std::vector<uint32_t> neighbours;
        std::vector<uint32_t>& list = around[b];
        list.erase(std::remove_if(list.begin(), list.end(), [&](uint32_t t) { return dead[t] != 0; }), list.end());
        for (uint32_t t : list)
            for (int k = 0; k < 3; ++k)
                if (tri[(size_t)t * 3 + k] != b) neighbours.push_back(tri[(size_t)t * 3 + k]);
        std::sort(neighbours.begin(), neighbours.end());
        neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
        for (uint32_t m : neighbours) { push(b, m); push(m, b); }
        if (live <= targets[next]) {
            levels.push_back(Snapshot(vertices, corner, dead, std::max(error, 1e-5f)));
            while (next < targets.size() && live <= targets[next]) ++next;
        }
    }
    // A level that barely shrank costs memory and a switch for nothing.
    std::vector<LodLevel> kept;
    size_t previous = triangleCount;
    for (LodLevel& level : levels)
        if (level.indices.size() / 3 <= previous * 85 / 100) { previous = level.indices.size() / 3; kept.push_back(std::move(level)); }
    return kept;
}

// Levels for meshes edge collapse cannot touch: kits of small separate pieces (cobbles, boards, rails, shutters, chimney pots)
// whose every vertex is a border or a hard-edged seam. Each coarser level leaves out the whole pieces under a size, so its
// error is the size of the biggest piece it dropped: with LodShows's one pixel, a piece goes only once it covers under a pixel
// on screen. A piece is what its triangles join by shared positions. The biggest piece always stays, and nothing over half the
// mesh's radius ever goes. A level is cut each time the triangles fall to 60% of the level before, at most three, so a chain
// costs at most about 1.2 times the full mesh's memory. Empty when nothing would go.
inline std::vector<LodLevel> BuildPieceLevels(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, float radius) {
    using namespace lod_detail;
    std::vector<LodLevel> levels;
    const size_t triangleCount = indices.size() / 3;
    if (triangleCount < 256) return levels;
    const uint32_t n = (uint32_t)vertices.size();
    std::unordered_map<PositionKey, uint32_t, PositionHash> byPosition;
    byPosition.reserve(n);
    std::vector<uint32_t> canon(n);
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const auto [it, added] = byPosition.try_emplace(PositionKey{{vertices[i].px, vertices[i].py, vertices[i].pz}}, c);
        if (added) ++c;
        canon[i] = it->second;
    }
    std::unordered_map<PositionKey, uint32_t, PositionHash>().swap(byPosition);
    std::vector<uint32_t> parent(c);
    for (uint32_t i = 0; i < c; ++i) parent[i] = i;
    const auto find = [&](uint32_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    for (size_t t = 0; t < triangleCount; ++t) {
        const uint32_t a = find(canon[indices[t * 3]]), b = find(canon[indices[t * 3 + 1]]), d = find(canon[indices[t * 3 + 2]]);
        parent[b] = a;
        parent[find(d)] = a;
    }
    // Each piece's box; its size is the box's diagonal.
    std::vector<uint32_t> piece(c, UINT32_MAX);
    struct Box { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; size_t triangles = 0; };
    std::vector<Box> boxes;
    std::vector<uint32_t> triPiece(triangleCount);
    for (size_t t = 0; t < triangleCount; ++t) {
        const uint32_t root = find(canon[indices[t * 3]]);
        if (piece[root] == UINT32_MAX) { piece[root] = (uint32_t)boxes.size(); boxes.emplace_back(); }
        Box& box = boxes[piece[root]];
        ++box.triangles;
        triPiece[t] = piece[root];
        for (int k = 0; k < 3; ++k) {
            const Vertex& v = vertices[indices[t * 3 + k]];
            const float p[3] = {v.px, v.py, v.pz};
            for (int a = 0; a < 3; ++a) { box.lo[a] = std::min(box.lo[a], p[a]); box.hi[a] = std::max(box.hi[a], p[a]); }
        }
    }
    if (boxes.size() < 2) return levels;
    std::vector<float> size(boxes.size());
    size_t biggest = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const float dx = boxes[i].hi[0] - boxes[i].lo[0], dy = boxes[i].hi[1] - boxes[i].lo[1], dz = boxes[i].hi[2] - boxes[i].lo[2];
        size[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (size[i] > size[biggest]) biggest = i;
    }
    std::vector<uint32_t> bySize(boxes.size());
    for (uint32_t i = 0; i < (uint32_t)boxes.size(); ++i) bySize[i] = i;
    std::sort(bySize.begin(), bySize.end(), [&](uint32_t a, uint32_t b) { return size[a] < size[b]; });
    // Walk the pieces smallest first; a level is cut each time the dropped ones reach the next share of the triangles.
    const float maxError = radius * 0.5f;
    std::vector<uint8_t> dropped(boxes.size(), 0);
    size_t kept = triangleCount, previous = triangleCount;
    float error = 0.0f;
    for (size_t k = 0; k < bySize.size() && levels.size() < 3; ++k) {
        const uint32_t p = bySize[k];
        if (p == biggest || size[p] > maxError) break;
        dropped[p] = 1;
        kept -= boxes[p].triangles;
        error = std::max(error, size[p]);
        const bool sameSizeNext = k + 1 < bySize.size() && size[bySize[k + 1]] <= error; // pieces of one size go together
        if (sameSizeNext || kept > previous * 60 / 100 || kept < 32) continue;
        LodLevel level;
        level.error = std::max(error, 1e-5f);
        std::vector<uint32_t> remap(n, UINT32_MAX);
        for (size_t t = 0; t < triangleCount; ++t) {
            if (dropped[triPiece[t]]) continue;
            for (int j = 0; j < 3; ++j) {
                uint32_t& to = remap[indices[t * 3 + j]];
                if (to == UINT32_MAX) { to = (uint32_t)level.vertices.size(); level.vertices.push_back(vertices[indices[t * 3 + j]]); }
                level.indices.push_back(to);
            }
        }
        previous = kept;
        levels.push_back(std::move(level));
    }
    return levels;
}

// The LOD cache: one file per welded part, named by a hash of its contents, so a changed mesh never reads stale levels and
// identical parts in many files share one. A file with no levels means "tried, nothing worth keeping".
inline uint64_t LodContentHash(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over 8-byte words
    const auto mix = [&](const void* data, size_t bytes) {
        const uint8_t* p = (const uint8_t*)data;
        size_t i = 0;
        for (; i + 8 <= bytes; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
        for (; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    };
    mix(vertices.data(), vertices.size() * sizeof(Vertex));
    mix(indices.data(), indices.size() * sizeof(uint32_t));
    return h;
}

constexpr uint32_t kLodFileMagic = 0x33444F4C;    // "LOD3": edge collapse, else dropped pieces
constexpr uint32_t kLodFileMagicOld = 0x31444F4C; // "LOD1": edge collapse only; its "nothing worth keeping" is tried again
constexpr uint32_t kLodFileMagicDraft = 0x32444F4C; // "LOD2": piece levels of a first draft (four levels, 85% steps): made again

inline bool LoadLodLevels(const std::wstring& path, std::vector<LodLevel>& levels) {
    std::ifstream f(path, std::ios::binary);
    uint32_t magic = 0, count = 0;
    if (!f.read((char*)&magic, 4) || (magic != kLodFileMagic && magic != kLodFileMagicOld) || !f.read((char*)&count, 4) || count > 8) return false; // LOD2 too
    if (magic == kLodFileMagicOld && count == 0) return false; // made before piece levels existed
    levels.assign(count, {});
    for (LodLevel& level : levels) {
        uint32_t nv = 0, ni = 0;
        if (!f.read((char*)&level.error, 4) || !f.read((char*)&nv, 4) || !f.read((char*)&ni, 4) || nv > (1u << 26) || ni > (1u << 28)) return false;
        level.vertices.resize(nv);
        level.indices.resize(ni);
        if (!f.read((char*)level.vertices.data(), (std::streamsize)nv * sizeof(Vertex)) || !f.read((char*)level.indices.data(), (std::streamsize)ni * 4)) return false;
    }
    return true;
}

inline bool SaveLodLevels(const std::wstring& path, const std::vector<LodLevel>& levels) {
    const std::wstring temp = path + L".tmp"; // a half-written file must never look complete
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        const uint32_t count = (uint32_t)levels.size();
        f.write((const char*)&kLodFileMagic, 4).write((const char*)&count, 4);
        for (const LodLevel& level : levels) {
            const uint32_t nv = (uint32_t)level.vertices.size(), ni = (uint32_t)level.indices.size();
            f.write((const char*)&level.error, 4).write((const char*)&nv, 4).write((const char*)&ni, 4);
            f.write((const char*)level.vertices.data(), (std::streamsize)nv * sizeof(Vertex)).write((const char*)level.indices.data(), (std::streamsize)ni * 4);
        }
        if (!f) return false;
    }
    std::error_code ec; // like MoveFileExW(MOVEFILE_REPLACE_EXISTING): replaces an existing file
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

} // namespace tbd
