// CPU visibility: bounding spheres, view volumes and the spatial cells instanced assets are split into. Renderer-agnostic.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "vecmath.h"

namespace tbd {

// radius < 0 means "unknown": never culled.
struct Sphere {
    Vec3 center = {0, 0, 0};
    float radius = -1.0f;
};

// The sphere around a range of points: the middle of their box, reaching the farthest one.
template <typename Point>
inline Sphere SphereAround(const Point* points, size_t count) {
    if (count == 0) return {};
    Vec3 lo = {points[0].px, points[0].py, points[0].pz}, hi = lo;
    for (size_t i = 1; i < count; ++i) {
        lo = {std::min(lo.x, points[i].px), std::min(lo.y, points[i].py), std::min(lo.z, points[i].pz)};
        hi = {std::max(hi.x, points[i].px), std::max(hi.y, points[i].py), std::max(hi.z, points[i].pz)};
    }
    Sphere s;
    s.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    float far2 = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const Vec3 d = {points[i].px - s.center.x, points[i].py - s.center.y, points[i].pz - s.center.z};
        far2 = std::max(far2, Dot(d, d));
    }
    s.radius = std::sqrt(far2);
    return s;
}

// A local sphere under a world matrix; the radius grows with the largest axis.
inline Sphere TransformSphere(const Sphere& s, const Mat4& model) {
    if (s.radius < 0.0f) return s;
    const auto axis = [&](int c) { return Vec3{model.At(0, c), model.At(1, c), model.At(2, c)}; };
    const Vec3 ax = axis(0), ay = axis(1), az = axis(2);
    Sphere out;
    out.center = {model.At(0, 0) * s.center.x + model.At(0, 1) * s.center.y + model.At(0, 2) * s.center.z + model.At(0, 3),
                  model.At(1, 0) * s.center.x + model.At(1, 1) * s.center.y + model.At(1, 2) * s.center.z + model.At(1, 3),
                  model.At(2, 0) * s.center.x + model.At(2, 1) * s.center.y + model.At(2, 2) * s.center.z + model.At(2, 3)};
    out.radius = s.radius * std::sqrt(std::max({Dot(ax, ax), Dot(ay, ay), Dot(az, az)}));
    return out;
}

// Half spaces a sphere must touch. Built from a view-projection matrix, left/right/bottom/top are enough for a perspective
// view: they meet at the eye, so everything behind it is outside too, and the reversed, far-away depth range needs no plane.
struct ViewVolume {
    float planes[6][4] = {};
    int count = 0;

    void Add(float a, float b, float c, float d) {
        const float len = std::sqrt(a * a + b * b + c * c);
        if (len < 1e-9f) return;
        planes[count][0] = a / len;
        planes[count][1] = b / len;
        planes[count][2] = c / len;
        planes[count][3] = d / len;
        ++count;
    }

    // `depth`: also test 0 <= z <= w, for the shadow maps' orthographic boxes.
    static ViewVolume FromMatrix(const Mat4& m, bool depth) {
        ViewVolume v;
        const auto row = [&](int r, int c) { return m.At(r, c); };
        for (int axis = 0; axis < 2; ++axis)
            for (float sign : {1.0f, -1.0f})
                v.Add(row(3, 0) + sign * row(axis, 0), row(3, 1) + sign * row(axis, 1), row(3, 2) + sign * row(axis, 2), row(3, 3) + sign * row(axis, 3));
        if (depth) {
            v.Add(row(2, 0), row(2, 1), row(2, 2), row(2, 3));
            v.Add(row(3, 0) - row(2, 0), row(3, 1) - row(2, 1), row(3, 2) - row(2, 2), row(3, 3) - row(2, 3));
        }
        return v;
    }

    bool Touches(const Sphere& s) const {
        if (s.radius < 0.0f) return true;
        for (int i = 0; i < count; ++i)
            if (planes[i][0] * s.center.x + planes[i][1] * s.center.y + planes[i][2] * s.center.z + planes[i][3] < -s.radius) return false;
        return true;
    }
};

// One spatial cell of an instanced asset: a run of the asset's instance buffer.
struct InstanceCell {
    Sphere bounds;
    uint32_t first = 0, count = 0;
};

// Meters. A cell's sphere must be small against the asset's draw distance or the distance limit means nothing: short-range
// assets (grass, drawn to 40 m) get finer cells than the rest.
inline float InstanceCellSize(float maxDistance) { return maxDistance <= 80.0f ? 16.0f : 64.0f; }
constexpr int kInstanceFloats = 12;        // a float3x4, row by row; the translation is each row's last value

// Reorders `rows` so each cell's instances are contiguous, shuffled within the cell (so that drawing the first part of a
// cell is a thinner, evenly spread version of it), and returns the cells. `meshBounds` is the mesh's local sphere.
inline std::vector<InstanceCell> BuildInstanceCells(std::vector<float>& rows, const Sphere& meshBounds, float cellSize) {
    const size_t count = rows.size() / kInstanceFloats;
    struct Key {
        int64_t cell;
        uint32_t shuffle, index;
    };
    std::vector<Key> keys(count);
    uint32_t random = 2166136261u; // deterministic: both backends and every run draw the same instances
    for (size_t i = 0; i < count; ++i) {
        const float* r = rows.data() + i * kInstanceFloats;
        const int64_t cx = (int64_t)std::floor(r[3] / cellSize), cz = (int64_t)std::floor(r[11] / cellSize);
        random = random * 1664525u + 1013904223u;
        keys[i] = {(cx + (1 << 20)) << 32 | (cz + (1 << 20)), random, (uint32_t)i};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return a.cell != b.cell ? a.cell < b.cell : (a.shuffle != b.shuffle ? a.shuffle < b.shuffle : a.index < b.index);
    });
    std::vector<float> sorted(rows.size());
    for (size_t i = 0; i < count; ++i)
        std::copy_n(rows.data() + (size_t)keys[i].index * kInstanceFloats, kInstanceFloats, sorted.data() + i * kInstanceFloats);
    rows.swap(sorted);

    std::vector<InstanceCell> cells;
    const float meshRadius = std::max(meshBounds.radius, 0.0f);
    for (size_t begin = 0; begin < count;) {
        size_t end = begin + 1;
        while (end < count && keys[end].cell == keys[begin].cell) ++end;
        // Each instance's own sphere first, then the cell's sphere around all of them.
        Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        float reach = 0.0f;
        std::vector<Vec3> centers;
        centers.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            const float* r = rows.data() + i * kInstanceFloats;
            const Vec3 ax = {r[0], r[4], r[8]}, ay = {r[1], r[5], r[9]}, az = {r[2], r[6], r[10]};
            const Vec3 c = {r[0] * meshBounds.center.x + r[1] * meshBounds.center.y + r[2] * meshBounds.center.z + r[3],
                            r[4] * meshBounds.center.x + r[5] * meshBounds.center.y + r[6] * meshBounds.center.z + r[7],
                            r[8] * meshBounds.center.x + r[9] * meshBounds.center.y + r[10] * meshBounds.center.z + r[11]};
            reach = std::max(reach, meshRadius * std::sqrt(std::max({Dot(ax, ax), Dot(ay, ay), Dot(az, az)})));
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
            centers.push_back(c);
        }
        InstanceCell cell;
        cell.first = (uint32_t)begin;
        cell.count = (uint32_t)(end - begin);
        cell.bounds.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        float far2 = 0.0f;
        for (const Vec3& c : centers) {
            const Vec3 d = c - cell.bounds.center;
            far2 = std::max(far2, Dot(d, d));
        }
        cell.bounds.radius = std::sqrt(far2) + reach;
        cells.push_back(cell);
        begin = end;
    }
    return cells;
}

} // namespace tbd
