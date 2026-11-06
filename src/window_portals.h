// Window panes as light portals for the irradiance probes. An imported glass part (vertex alpha 0.645, panes tagged by
// TagPanes in mesh_import.h) becomes a list of rectangles; each frame the nearest ones go to the renderer, and a probe
// ray that leaves a shaded room through one sees the sky, where a ray that meets the roof is stopped (post.hlsl). The rooms
// themselves, found from the building's faces, let the probes inside trace the walls, floor and ceiling (ExtractInteriorRooms).
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <vector>

#include "render/renderer.h"
#include "vecmath.h"

namespace tbd {

constexpr float kGlassAlpha = 0.645f;

// `v` is a flat triangle list. Panes come out in the list's own space: centre and the two half-extent vectors of the rectangle.
inline std::vector<WindowPortal> ExtractWindowPortals(const Vertex* v, size_t count) {
    std::vector<WindowPortal> out;
    if (count < 3 || std::fabs(v[0].a - kGlassAlpha) > 0.003f) return out;
    std::map<int, std::vector<size_t>> panes; // the per-pane seed TagPanes wrote into b
    for (size_t t = 0; t + 3 <= count; t += 3) panes[(int)std::lround(v[t].b * 65535.0f)].push_back(t);
    for (const auto& [seed, tris] : panes) {
        // A pane is often a thin box: its two faces would cancel, so each face counts turned to agree with the largest one.
        Vec3 normal = {0, 0, 0}, largest = {0, 0, 0};
        for (size_t t : tris) {
            const Vec3 a = {v[t].px, v[t].py, v[t].pz}, b = {v[t + 1].px, v[t + 1].py, v[t + 1].pz}, c = {v[t + 2].px, v[t + 2].py, v[t + 2].pz};
            const Vec3 n = Cross(b - a, c - a);
            if (Dot(n, n) > Dot(largest, largest)) largest = n;
        }
        for (size_t t : tris) {
            const Vec3 a = {v[t].px, v[t].py, v[t].pz}, b = {v[t + 1].px, v[t + 1].py, v[t + 1].pz}, c = {v[t + 2].px, v[t + 2].py, v[t + 2].pz};
            const Vec3 n = Cross(b - a, c - a);
            normal = normal + (Dot(n, largest) < 0.0f ? n * -1.0f : n);
        }
        if (Dot(normal, normal) < 1e-8f) continue;
        normal = Normalize(normal);
        const Vec3 e1 = std::fabs(normal.y) > 0.9f ? Vec3{1, 0, 0} : Normalize(Cross(normal, Vec3{0, 1, 0}));
        const Vec3 e2 = Cross(normal, e1);
        float lo1 = 1e30f, hi1 = -1e30f, lo2 = 1e30f, hi2 = -1e30f, plane = 0.0f;
        for (size_t t : tris)
            for (size_t k = 0; k < 3; ++k) {
                const Vec3 p = {v[t + k].px, v[t + k].py, v[t + k].pz};
                lo1 = std::min(lo1, Dot(p, e1)); hi1 = std::max(hi1, Dot(p, e1));
                lo2 = std::min(lo2, Dot(p, e2)); hi2 = std::max(hi2, Dot(p, e2));
                plane += Dot(p, normal) / (float)(tris.size() * 3);
            }
        if ((hi1 - lo1) * (hi2 - lo2) < 0.05f) continue; // a sliver or a lamp's chimney, not a window
        const Vec3 centre = e1 * (0.5f * (lo1 + hi1)) + e2 * (0.5f * (lo2 + hi2)) + normal * plane;
        const Vec3 u = e1 * (0.5f * (hi1 - lo1)), w = e2 * (0.5f * (hi2 - lo2));
        out.push_back({{centre.x, centre.y, centre.z}, {u.x, u.y, u.z}, {w.x, w.y, w.z}});
    }
    return out;
}

// The rooms of a hollow building, one box per storey, in the list's own space. The faces are binned by position along their
// normal: a floor is a large upward plane near the bottom, a ceiling a large downward one at least 2 m above it, and each wall
// the outermost large plane looking into the room (the outward face, moved in by a wall's thickness, where there is no inner one).
// Inner partitions are ignored: a storey is one room. A storey under a pitched roof, with no flat ceiling, gets none.
inline std::vector<InteriorRoom> ExtractInteriorRooms(const Vertex* v, size_t count) {
    struct Face { Vec3 c, n; float area; };
    std::vector<Face> faces;
    Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (size_t t = 0; t + 3 <= count; t += 3) {
        if (std::fabs(v[t].a - kGlassAlpha) < 0.003f) continue;
        const Vec3 a = {v[t].px, v[t].py, v[t].pz}, b = {v[t + 1].px, v[t + 1].py, v[t + 1].pz}, c = {v[t + 2].px, v[t + 2].py, v[t + 2].pz};
        for (const Vec3& p : {a, b, c}) { lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
        const Vec3 n = Cross(b - a, c - a);
        const float len = std::sqrt(Dot(n, n));
        if (len < 2e-4f) continue;
        faces.push_back({(a + b + c) * (1.0f / 3.0f), n * (1.0f / len), 0.5f * len});
    }
    std::vector<InteriorRoom> out;
    if (faces.empty()) return out;
    const float footprint = (hi.x - lo.x) * (hi.z - lo.z);
    if (footprint < 9.0f || hi.y - lo.y < 2.5f) return out;

    // Faces along `axis` with normal sign `sign`, in 10 cm bins: area-weighted position (still summed) and area of each.
    struct Slab { float pos, area; };
    const auto bins = [&](int axis, float sign, float y0, float y1) {
        std::map<int, Slab> out;
        for (const Face& f : faces) {
            const float n = axis == 0 ? f.n.x : axis == 1 ? f.n.y : f.n.z, p = axis == 0 ? f.c.x : axis == 1 ? f.c.y : f.c.z;
            if (n * sign < 0.9f || (axis != 1 && (f.c.y < y0 || f.c.y > y1))) continue;
            Slab& s = out[(int)std::lround(p * 10.0f)];
            s.pos += p * f.area; s.area += f.area;
        }
        return out;
    };
    // The bins of at least `minArea` (shelves and table tops fall out), neighbours joined into one plane.
    const auto planes = [](const std::map<int, Slab>& bins, float minArea) {
        std::vector<Slab> out;
        int last = INT32_MIN;
        for (const auto& [bin, s] : bins) {
            if (s.area < minArea) continue;
            if (out.empty() || bin - last > 1) out.push_back({0.0f, 0.0f});
            out.back().pos += s.pos; out.back().area += s.area;
            last = bin;
        }
        for (Slab& s : out) s.pos /= s.area;
        return out;
    };
    // The inner face of the wall on the low or high side of `axis`.
    const auto wall = [&](int axis, bool lowSide, float y0, float y1) {
        const float mid = axis == 0 ? 0.5f * (lo.x + hi.x) : 0.5f * (lo.z + hi.z);
        for (const float look : {1.0f, -1.0f}) { // facing into the room, then the outward face
            std::map<int, Slab> side;
            float most = 0.0f;
            for (const auto& [bin, s] : bins(axis, lowSide ? look : -look, y0, y1))
                if ((s.pos / s.area < mid) == lowSide) { side[bin] = s; most = std::max(most, s.area); }
            if (most < 0.25f * (y1 - y0)) continue;
            const std::vector<Slab> found = planes(side, 0.25f * most);
            const float pos = lowSide ? found.front().pos : found.back().pos;
            const float inset = look > 0.0f ? 0.0f : 0.15f;
            return lowSide ? pos + inset : pos - inset;
        }
        const float edge = axis == 0 ? (lowSide ? lo.x : hi.x) : (lowSide ? lo.z : hi.z);
        return lowSide ? edge + 0.2f : edge - 0.2f;
    };

    const std::vector<Slab> ups = planes(bins(1, 1.0f, 0, 0), 0.3f * footprint), downs = planes(bins(1, -1.0f, 0, 0), 0.3f * footprint);
    float floorY = 1e30f;
    for (const Slab& s : ups) if (s.pos < lo.y + 1.5f) { floorY = s.pos; break; }
    for (int storey = 0; storey < 2 && floorY < 1e29f; ++storey) {
        float ceilY = 1e30f;
        for (const Slab& s : downs) if (s.pos > floorY + 2.0f) { ceilY = s.pos; break; }
        if (ceilY > 1e29f) break; // the roof itself: no flat ceiling to bound a box
        const float y0 = floorY + 0.3f, y1 = ceilY - 0.3f;
        const float x0 = wall(0, true, y0, y1), x1 = wall(0, false, y0, y1), z0 = wall(2, true, y0, y1), z1 = wall(2, false, y0, y1);
        if (x1 - x0 > 2.0f && z1 - z0 > 2.0f) {
            InteriorRoom r = {};
            r.center[0] = 0.5f * (x0 + x1); r.center[1] = 0.5f * (floorY + ceilY); r.center[2] = 0.5f * (z0 + z1);
            r.halfHeight = 0.5f * (ceilY - floorY);
            r.x[0] = 0.5f * (x1 - x0); r.z[2] = 0.5f * (z1 - z0);
            out.push_back(r);
        }
        float next = 1e30f; // the floor above: a large upward face within a board's depth over this ceiling
        for (const Slab& s : ups) if (s.pos > ceilY - 0.1f && s.pos < ceilY + 0.6f) { next = s.pos; break; }
        floorY = next;
    }
    return out;
}

} // namespace tbd
