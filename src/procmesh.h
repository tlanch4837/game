// Dense procedural meshes (a tree and a rock) for exercising LOD: tens of thousands of triangles where
// the primitives have a few hundred at most. Same conventions as the primitives in scene.h: unit size
// (the entity's scale sizes it), centered on the origin, vertex-colored, flat unindexed triangles.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/renderer.h"
#include "vecmath.h"

namespace tbd {

namespace procmesh_detail {

struct Rng {
    uint32_t state;
    float Next() {
        state = state * 1664525u + 1013904223u;
        return (float)(state >> 8) * (1.0f / 16777216.0f);
    }
    float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

inline float Hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u + seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// Smooth value noise in 0..1.
inline float Noise(Vec3 p, uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int xi = (int)fx, yi = (int)fy, zi = (int)fz;
    auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float tx = fade(p.x - fx), ty = fade(p.y - fy), tz = fade(p.z - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float c[2][2];
    for (int dy = 0; dy < 2; ++dy)
        for (int dz = 0; dz < 2; ++dz)
            c[dy][dz] = lerp(Hash(xi, yi + dy, zi + dz, seed), Hash(xi + 1, yi + dy, zi + dz, seed), tx);
    return lerp(lerp(c[0][0], c[0][1], tz), lerp(c[1][0], c[1][1], tz), ty);
}

// A grid of points: `nu` columns around (wrapping) by `nv + 1` rows. `hint` points outward and picks the
// side the normal faces (and stands in for it where the grid pinches to a point).
struct Surface {
    int nu = 0, nv = 0;
    std::vector<Vec3> p, hint;

    Surface(int columns, int rows) : nu(columns), nv(rows), p((size_t)columns * (rows + 1)), hint(p.size()) {}
    size_t Index(int i, int j) const { return (size_t)j * nu + (size_t)(((i % nu) + nu) % nu); }
    Vec3& P(int i, int j) { return p[Index(i, j)]; }
};

inline void Emit(std::vector<Vertex>& out, const Surface& s, Vec3 color, float variation, uint32_t seed) {
    auto vertex = [&](int i, int j) {
        const size_t at = s.Index(i, j);
        const Vec3 pos = s.p[at];
        const Vec3 hint = Normalize(s.hint[at]);
        const Vec3 du = s.p[s.Index(i + 1, j)] - s.p[s.Index(i - 1, j)];
        const Vec3 dv = s.p[s.Index(i, std::min(j + 1, s.nv))] - s.p[s.Index(i, std::max(j - 1, 0))];
        Vec3 n = Cross(du, dv);
        n = Dot(n, n) > 1e-12f ? Normalize(n) : hint;
        if (Dot(n, hint) < 0) n = n * -1.0f;
        const float shade = 1.0f - variation + 2.0f * variation * Noise(pos * 9.0f, seed);
        return Vertex{pos.x, pos.y, pos.z, n.x, n.y, n.z, color.x * shade, color.y * shade, color.z * shade, 1.0f};
    };
    for (int j = 0; j < s.nv; ++j)
        for (int i = 0; i < s.nu; ++i) {
            const Vertex a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1), d = vertex(i, j + 1);
            auto area2 = [](const Vertex& x, const Vertex& y, const Vertex& z) {
                const Vec3 e = Cross(Vec3{y.px - x.px, y.py - x.py, y.pz - x.pz}, Vec3{z.px - x.px, z.py - x.py, z.pz - x.pz});
                return Dot(e, e);
            };
            if (area2(a, b, c) > 1e-14f) { out.push_back(a); out.push_back(b); out.push_back(c); }
            if (area2(a, c, d) > 1e-14f) { out.push_back(a); out.push_back(c); out.push_back(d); }
        }
}

// A lumpy ellipsoid. `bump` is the noise displacement as a fraction of the radius.
inline void AddBlob(std::vector<Vertex>& out, Vec3 center, Vec3 radii, float bump, float frequency, int columns,
                    int rows, Vec3 color, float variation, uint32_t seed) {
    Surface s(columns, rows);
    for (int j = 0; j <= rows; ++j)
        for (int i = 0; i < columns; ++i) {
            const float phi = kPi * (float)j / (float)rows, theta = 2.0f * kPi * (float)i / (float)columns;
            const Vec3 dir = {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            const float k = 1.0f + bump * 2.0f * (Noise(dir * frequency + center * 7.0f, seed) - 0.5f);
            s.P(i, j) = center + Vec3{dir.x * radii.x, dir.y * radii.y, dir.z * radii.z} * k;
            s.hint[s.Index(i, j)] = dir;
        }
    Emit(out, s, color, variation, seed);
}

// A tapered tube from a to b, wobbling sideways by up to `wobble`.
inline void AddTube(std::vector<Vertex>& out, Vec3 a, Vec3 b, float r0, float r1, float wobble, int columns, int rows,
                    Vec3 color, uint32_t seed) {
    const Vec3 axis = b - a;
    const Vec3 w = Normalize(axis);
    const Vec3 u = Normalize(Cross(w, std::fabs(w.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 v = Cross(w, u);
    Surface s(columns, rows);
    for (int j = 0; j <= rows; ++j) {
        const float t = (float)j / (float)rows;
        const float r = r0 + (r1 - r0) * t;
        const Vec3 along = a + axis * t;
        for (int i = 0; i < columns; ++i) {
            const float theta = 2.0f * kPi * (float)i / (float)columns;
            const Vec3 radial = u * std::cos(theta) + v * std::sin(theta);
            const float k = 1.0f + wobble * (Noise(along * 40.0f + radial * 3.0f, seed) - 0.5f);
            s.P(i, j) = along + radial * (r * k);
            s.hint[s.Index(i, j)] = radial;
        }
    }
    Emit(out, s, color, 0.12f, seed);
}

} // namespace procmesh_detail

// About 28k triangles: a trunk, a few branches and a crown of lumpy leaf clumps. Spans y = -0.5 .. 0.46 and
// about 0.7 wide. `leaves` colors the crown.
inline std::vector<Vertex> MakeTreeMesh(Vec3 leaves) {
    using namespace procmesh_detail;
    std::vector<Vertex> out;
    out.reserve(40000);
    const Vec3 bark = {0.34f, 0.23f, 0.14f};
    Rng rng{0x7EE5EEDu};

    AddTube(out, {0, -0.5f, 0}, {0, 0.1f, 0}, 0.05f, 0.022f, 0.35f, 16, 24, bark, 11);
    for (int i = 0; i < 6; ++i) {
        const float angle = rng.Range(0.0f, 2.0f * kPi), reach = rng.Range(0.14f, 0.24f);
        const Vec3 from = {0, rng.Range(-0.15f, 0.0f), 0};
        const Vec3 to = {std::cos(angle) * reach, from.y + rng.Range(0.12f, 0.22f), std::sin(angle) * reach};
        AddTube(out, from, to, 0.02f, 0.008f, 0.3f, 8, 8, bark, 20 + i);
    }

    const int clumps = 16;
    for (int i = 0; i < clumps; ++i) {
        const float angle = rng.Range(0.0f, 2.0f * kPi), reach = rng.Range(0.0f, 0.24f);
        const float r = rng.Range(0.13f, 0.2f);
        const float y = std::min(rng.Range(-0.05f, 0.3f), 0.5f - r * 0.9f);
        const Vec3 center = {std::cos(angle) * reach, y, std::sin(angle) * reach};
        AddBlob(out, center, {r, r * 0.85f, r}, 0.28f, 3.2f, 36, 24, leaves, 0.18f, 100 + i);
    }
    return out;
}

// About 9k triangles: a lumpy boulder, about 1 wide and 0.8 tall.
inline std::vector<Vertex> MakeRockMesh(Vec3 color) {
    using namespace procmesh_detail;
    std::vector<Vertex> out;
    out.reserve(30000);
    AddBlob(out, {0, 0, 0}, {0.46f, 0.36f, 0.42f}, 0.2f, 2.4f, 80, 56, color, 0.14f, 7);
    return out;
}

// Axis-aligned bounds of a vertex list; both corners are zero for an empty one.
inline void MeshBounds(const std::vector<Vertex>& v, Vec3& mn, Vec3& mx) {
    mn = mx = {};
    if (v.empty()) return;
    mn = mx = {v[0].px, v[0].py, v[0].pz};
    for (const Vertex& x : v) {
        mn = {std::fmin(mn.x, x.px), std::fmin(mn.y, x.py), std::fmin(mn.z, x.pz)};
        mx = {std::fmax(mx.x, x.px), std::fmax(mx.y, x.py), std::fmax(mx.z, x.pz)};
    }
}

} // namespace tbd
