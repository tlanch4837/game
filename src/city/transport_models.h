// Procedural models of the transport buildings (no art assets needed): boxes, prisms and beams in the building's local
// frame (x across, z toward the front, y up from the building's level), emitted as lit triangles plus edge lines. The same
// geometry drives all three looks: the solid building, the rising construction site, and the blue hologram shown for the
// preview and for a placed building that is still "Planned". Platform free: the host draws what BuildPlaced returns.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "transport.h"

namespace tbd {
namespace city {
namespace tmodel {

struct Mesher {
    std::vector<Vertex>& tris;
    std::vector<Vertex>* edges; // may be null
    Frame fr;
    float y0 = 0;
    float sy = 1; // vertical scale (the construction rise)

    Mesher(std::vector<Vertex>& t, std::vector<Vertex>* e, V2 c, V2 front, float y) : tris(t), edges(e), fr(c, front), y0(y) {}

    Vec3 P(float x, float y, float z) const {
        const V2 w = fr.World(x, z);
        return {w.x, y0 + y * sy, w.z};
    }
    void Tri(Vec3 a, Vec3 b, Vec3 c, Vec3 n, Vec3 col) {
        tris.push_back({a.x, a.y, a.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
        tris.push_back({b.x, b.y, b.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
        tris.push_back({c.x, c.y, c.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
    }
    // a quad a b c d; the normal from the winding, flipped to face `hint`
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 hint, Vec3 col) {
        Vec3 n = Cross(b - a, d - a);
        const float l = std::sqrt(Dot(n, n));
        if (l < 1e-6f) return;
        n = n * (1.0f / l);
        if (Dot(n, hint) < 0) n = n * -1.0f;
        Tri(a, b, c, n, col);
        Tri(a, c, d, n, col);
    }
    void Line(Vec3 a, Vec3 b, Vec3 col) {
        if (!edges) return;
        edges->push_back({a.x, a.y, a.z, 0, 0, 0, col.x, col.y, col.z, 1.0f});
        edges->push_back({b.x, b.y, b.z, 0, 0, 0, col.x, col.y, col.z, 1.0f});
    }

    // A straight SPOM railway track (mesh::TrackSweep, TrackPS in mesh.hlsl) from local (x0, z0) to (x1, z1), ballast bottom at y.
    // Its vertices carry alpha 0.83: the host draws them apart, with the track texture bound (TransportDraw).
    void Track(float x0, float z0, float x1, float z1, float y = 0.15f) {
        const V2 a = fr.World(x0, z0), b = fr.World(x1, z1);
        const float len = Len(b - a);
        if (len < 0.1f) return;
        const V2 f = (b - a) * (1.0f / len);
        const float top = y0 + (y + mesh::kTrackDepth - 0.05f) * sy;
        std::vector<mesh::TrackPt> pts;
        const int n = std::max(1, (int)std::ceil(len / 6.0f));
        for (int i = 0; i <= n; ++i) pts.push_back({a + f * (len * i / n), top, f, len * i / n});
        mesh::TrackSweep(tris, pts);
    }
    // An upright box in local coordinates.
    void Box(float x0, float x1, float z0, float z1, float ya, float yb, Vec3 col, bool edge = true) {
        const Vec3 c[8] = {P(x0, ya, z0), P(x1, ya, z0), P(x1, ya, z1), P(x0, ya, z1), P(x0, yb, z0), P(x1, yb, z0), P(x1, yb, z1), P(x0, yb, z1)};
        const V2 s = fr.side, f = fr.front;
        const Vec3 nx{s.x, 0, s.z}, nz{f.x, 0, f.z};
        Quad(c[1], c[2], c[6], c[5], nx, col);
        Quad(c[0], c[3], c[7], c[4], nx * -1.0f, col);
        Quad(c[3], c[2], c[6], c[7], nz, col);
        Quad(c[0], c[1], c[5], c[4], nz * -1.0f, col);
        Quad(c[4], c[5], c[6], c[7], {0, 1, 0}, col);
        if (edge && edges) {
            const Vec3 e = {0.55f, 0.9f, 1.0f};
            for (int i = 0; i < 4; ++i) {
                Line(c[i], c[(i + 1) % 4], e);
                Line(c[4 + i], c[4 + (i + 1) % 4], e);
                Line(c[i], c[4 + i], e);
            }
        }
    }
    // A roof prism: eaves at yEave over the rectangle, the ridge at yRidge, along x or z.
    void Gable(float x0, float x1, float z0, float z1, float yEave, float yRidge, Vec3 col, bool ridgeAlongX) {
        if (ridgeAlongX) {
            const float zm = 0.5f * (z0 + z1);
            Quad(P(x0, yEave, z0), P(x1, yEave, z0), P(x1, yRidge, zm), P(x0, yRidge, zm), {0, 1, 0}, col);
            Quad(P(x0, yEave, z1), P(x1, yEave, z1), P(x1, yRidge, zm), P(x0, yRidge, zm), {0, 1, 0}, col);
            const V2 s = fr.side;
            Tri(P(x0, yEave, z0), P(x0, yEave, z1), P(x0, yRidge, zm), {-s.x, 0, -s.z}, col);
            Tri(P(x1, yEave, z0), P(x1, yEave, z1), P(x1, yRidge, zm), {s.x, 0, s.z}, col);
            Line(P(x0, yRidge, zm), P(x1, yRidge, zm), {0.55f, 0.9f, 1.0f});
        } else {
            const float xm = 0.5f * (x0 + x1);
            Quad(P(x0, yEave, z0), P(x0, yEave, z1), P(xm, yRidge, z1), P(xm, yRidge, z0), {0, 1, 0}, col);
            Quad(P(x1, yEave, z0), P(x1, yEave, z1), P(xm, yRidge, z1), P(xm, yRidge, z0), {0, 1, 0}, col);
            const V2 f = fr.front;
            Tri(P(x0, yEave, z0), P(x1, yEave, z0), P(xm, yRidge, z0), {-f.x, 0, -f.z}, col);
            Tri(P(x0, yEave, z1), P(x1, yEave, z1), P(xm, yRidge, z1), {f.x, 0, f.z}, col);
            Line(P(xm, yRidge, z0), P(xm, yRidge, z1), {0.55f, 0.9f, 1.0f});
        }
    }
    // An extruded convex polygon (local x, z), from ya to yb.
    void Prism(const std::vector<V2>& poly, float ya, float yb, Vec3 col, bool edge = true) {
        const size_t n = poly.size();
        V2 cen{};
        for (const V2& p : poly) cen = cen + p;
        cen = cen * (1.0f / (float)n);
        for (size_t i = 0; i < n; ++i) {
            const V2 a = poly[i], b = poly[(i + 1) % n];
            const V2 mid = (a + b) * 0.5f - cen, w = fr.side * mid.x + fr.front * mid.z;
            Quad(P(a.x, ya, a.z), P(b.x, ya, b.z), P(b.x, yb, b.z), P(a.x, yb, a.z), {w.x, 0, w.z}, col);
            Tri(P(a.x, yb, a.z), P(b.x, yb, b.z), P(cen.x, yb, cen.z), {0, 1, 0}, col);
            if (edge) {
                Line(P(a.x, yb, a.z), P(b.x, yb, b.z), {0.55f, 0.9f, 1.0f});
                Line(P(a.x, ya, a.z), P(a.x, yb, a.z), {0.55f, 0.9f, 1.0f});
            }
        }
    }
    void Cylinder(float x, float z, float r, float ya, float yb, Vec3 col, int n = 12) {
        std::vector<V2> poly;
        for (int i = 0; i < n; ++i) poly.push_back({x + r * std::cos(6.2831853f * i / n), z + r * std::sin(6.2831853f * i / n)});
        Prism(poly, ya, yb, col, false);
    }
    // A square beam between two local points.
    void Beam(Vec3 a, Vec3 b, float t, Vec3 col) { // a, b in local (x, y, z)
        const Vec3 wa = P(a.x, a.y, a.z), wb = P(b.x, b.y, b.z);
        Vec3 d = wb - wa;
        const float len = std::sqrt(Dot(d, d));
        if (len < 1e-4f) return;
        d = d * (1.0f / len);
        const Vec3 ref = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 u = Normalize(Cross(d, ref)) * (t * 0.5f), v = Normalize(Cross(d, u)) * (t * 0.5f);
        const Vec3 c[8] = {wa - u - v, wa + u - v, wa + u + v, wa - u + v, wb - u - v, wb + u - v, wb + u + v, wb - u + v};
        const int f[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {3, 2, 1, 0}, {4, 5, 6, 7}};
        for (const auto& q : f) {
            const Vec3 mid = (c[q[0]] + c[q[2]]) * 0.5f, ctr = (wa + wb) * 0.5f;
            Quad(c[q[0]], c[q[1]], c[q[2]], c[q[3]], mid - ctr, col);
        }
        Line(wa, wb, {0.55f, 0.9f, 1.0f});
    }
};

// ---- shared palette
const Vec3 kConcrete{0.62f, 0.62f, 0.60f}, kAsphalt{0.17f, 0.17f, 0.19f}, kGravel{0.38f, 0.36f, 0.33f}, kSteel{0.55f, 0.57f, 0.60f},
    kYellow{0.95f, 0.72f, 0.10f}, kWhite{0.92f, 0.92f, 0.90f}, kDark{0.08f, 0.09f, 0.11f}, kRoofBlue{0.30f, 0.36f, 0.44f};

inline uint32_t Hash(int a, int b) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
    h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
    return h;
}
inline Vec3 ContainerColor(uint32_t h) {
    static const Vec3 k[] = {{0.72f, 0.2f, 0.15f}, {0.15f, 0.35f, 0.65f}, {0.85f, 0.55f, 0.15f}, {0.2f, 0.5f, 0.3f}, {0.6f, 0.6f, 0.62f}, {0.75f, 0.7f, 0.2f}};
    return k[h % 6];
}

// ---- parts used by several models
inline void TrackZ(Mesher& m, float x, float z0, float z1) { m.Track(x, z0, x, z1); } // a track running along z
inline void TrackX(Mesher& m, float z, float x0, float x1) { m.Track(x0, z, x1, z); } // along x
// a train on a track along z (loco first) or along x
inline void TrainZ(Mesher& m, float x, float zStart, int cars, uint32_t seed, bool freight) {
    float z = zStart;
    m.Box(x - 1.6f, x + 1.6f, z, z + 18.0f, 0.5f, 4.4f, {0.2f, 0.32f, 0.6f});
    m.Box(x - 1.65f, x + 1.65f, z + 14.0f, z + 18.0f, 2.2f, 3.6f, kDark, false);
    z += 19.0f;
    for (int i = 0; i < cars; ++i, z += 15.0f)
        m.Box(x - 1.5f, x + 1.5f, z, z + 14.0f, 0.6f, freight ? 3.2f : 4.0f, freight ? ContainerColor(Hash((int)seed, i)) : Vec3{0.8f, 0.82f, 0.86f});
}
inline void TrainX(Mesher& m, float z, float xStart, int cars, uint32_t seed, bool freight) {
    float x = xStart;
    m.Box(x, x + 18.0f, z - 1.6f, z + 1.6f, 0.5f, 4.4f, {0.55f, 0.2f, 0.15f});
    x += 19.0f;
    for (int i = 0; i < cars; ++i, x += 15.0f) {
        m.Box(x, x + 14.0f, z - 1.5f, z + 1.5f, 0.6f, 1.2f, kSteel, false); // the flat wagon
        if (freight) {
            m.Box(x + 0.5f, x + 6.5f, z - 1.3f, z + 1.3f, 1.2f, 3.8f, ContainerColor(Hash((int)seed, i * 2)));
            m.Box(x + 7.3f, x + 13.3f, z - 1.3f, z + 1.3f, 1.2f, 3.8f, ContainerColor(Hash((int)seed, i * 2 + 1)));
        }
    }
}
inline void ContainerStack(Mesher& m, float x, float z, int h, uint32_t seed, bool alongX = true) {
    for (int k = 0; k < h; ++k) {
        const Vec3 col = ContainerColor(Hash((int)seed, k + 7));
        if (alongX) m.Box(x, x + 12.0f, z, z + 2.5f, 0.2f + k * 2.7f, 2.9f + k * 2.7f, col, false);
        else m.Box(x, x + 2.5f, z, z + 12.0f, 0.2f + k * 2.7f, 2.9f + k * 2.7f, col, false);
    }
}
// a jet along local x (nose at +x), wing span `span`
inline void Plane(Mesher& m, float x, float z, float len, bool jet) {
    const float w = len * 0.1f;
    m.Box(x - len * 0.5f, x + len * 0.5f, z - w * 0.5f, z + w * 0.5f, 0.6f, 0.6f + w, kWhite);
    m.Box(x - len * 0.1f, x + len * 0.1f, z - len * (jet ? 0.45f : 0.55f), z + len * (jet ? 0.45f : 0.55f), 0.6f + w * 0.3f, 0.6f + w * 0.45f, {0.78f, 0.8f, 0.85f}, false);
    m.Box(x - len * 0.5f, x - len * 0.42f, z - 0.15f, z + 0.15f, 0.6f + w, 0.6f + w + len * 0.18f, {0.2f, 0.35f, 0.7f}, false); // fin
    m.Box(x - len * 0.5f, x - len * 0.4f, z - len * 0.18f, z + len * 0.18f, 0.6f + w * 0.6f, 0.6f + w * 0.75f, {0.78f, 0.8f, 0.85f}, false);
}
inline void Hull(Mesher& m, float x0, float x1, float z0, float z1, float ya, float yb, Vec3 col, bool bowToPlusX) {
    const float zm = 0.5f * (z0 + z1), nose = (x1 - x0) * 0.16f;
    std::vector<V2> poly;
    if (bowToPlusX) poly = {{x0, z0}, {x1 - nose, z0}, {x1, zm}, {x1 - nose, z1}, {x0, z1}};
    else poly = {{x0, zm}, {x0 + nose, z0}, {x1, z0}, {x1, z1}, {x0 + nose, z1}};
    m.Prism(poly, ya, yb, col);
}
// a hull along local z (bow toward +z)
inline void HullZ(Mesher& m, float x0, float x1, float z0, float z1, float ya, float yb, Vec3 col) {
    const float xm = 0.5f * (x0 + x1), nose = (z1 - z0) * 0.2f;
    m.Prism({{x0, z0}, {x1, z0}, {x1, z1 - nose}, {xm, z1}, {x0, z1 - nose}}, ya, yb, col);
}
// a dock over water: deck on piles (the water is `drop` m below the deck's level)
inline void Pier(Mesher& m, float x0, float x1, float z0, float z1, float drop) {
    m.Box(x0, x1, z0, z1, -0.1f, 0.5f, kConcrete, false);
    for (float x = x0 + 1.0f; x <= x1; x += 12.0f)
        for (float z = z0 + 2.0f; z <= z1; z += 12.0f) m.Box(x - 0.5f, x + 0.5f, z - 0.5f, z + 0.5f, -drop - 3.0f, -0.1f, {0.3f, 0.3f, 0.3f}, false);
    m.Box(x0, x1, z1 - 0.5f, z1, -drop - 1.0f, 0.5f, {0.45f, 0.45f, 0.43f}, false); // the quay wall
}
inline void StsCrane(Mesher& m, float x, float zLand, float zWater) {
    for (float dx : {-8.0f, 8.0f})
        for (float z : {zLand, zWater}) m.Box(x + dx - 0.7f, x + dx + 0.7f, z - 0.7f, z + 0.7f, 0.5f, 24.0f, {0.8f, 0.2f, 0.18f}, false);
    m.Box(x - 9.5f, x + 9.5f, zLand - 1.0f, zWater + 1.0f, 24.0f, 26.0f, {0.8f, 0.2f, 0.18f});
    m.Box(x - 1.6f, x + 1.6f, zLand - 14.0f, zWater + 36.0f, 26.0f, 27.8f, {0.85f, 0.85f, 0.88f}); // boom and backstay
    m.Beam({x, 34.0f, zLand + 4.0f}, {x, 27.8f, zLand - 14.0f}, 0.4f, kSteel);
    m.Beam({x, 34.0f, zLand + 4.0f}, {x, 27.8f, zWater + 36.0f}, 0.4f, kSteel);
    m.Box(x - 0.6f, x + 0.6f, zLand + 3.0f, zLand + 5.0f, 26.0f, 34.0f, kSteel, false);
}

// ---------------------------------------------------------------------------------------------------- the eight models
// The depot (assets/rail/rail_depot.html): a 120 x 32 m maintenance shed straddling five straight through roads, its long grey roof
// set with two rows of six skylights, portals at both ends; two outer sidings under a carriage wash at the back, the office and fuel
// tanks at the front. Tracks run along local x, right through the lot (SPOM track); no trains.
inline void RailDepot(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f;
    const Vec3 wall{0.79f, 0.77f, 0.72f}, pilaster{0.66f, 0.64f, 0.59f}, roof{0.55f, 0.56f, 0.58f}, frame{0.72f, 0.73f, 0.75f}, glass{0.17f, 0.23f, 0.27f},
        trim{0.85f, 0.66f, 0.16f}, apron{0.55f, 0.53f, 0.49f}, sidingBed{0.49f, 0.48f, 0.44f};
    const float roads[5] = {-10, -5, 0, 5, 10}, sidings[2] = {-27, -33};
    const float L = 60.0f, Wd = 16.0f, H = 10.0f; // shed half length, half width, wall height
    m.Box(-hw, hw, -hd, hd, 0.0f, 0.1f, kGravel, false);
    m.Box(-hw, hw, -20, 20, 0.1f, 0.12f, apron, false);           // shed apron
    m.Box(-hw, hw, -37, -23, 0.1f, 0.12f, sidingBed, false);       // siding bed
    for (float z : roads) m.Track(-hw, z, hw, z);
    for (float z : sidings) m.Track(-hw, z, hw, z);
    // shed walls, pilasters and window bands
    for (float s : {-1.0f, 1.0f}) {
        const float z = s * Wd;
        m.Box(-L, L, z - 0.3f, z + 0.3f, 0.12f, H, wall);
        for (float x = -L + 6.0f; x < L; x += 12.0f) {
            m.Box(x - 0.4f, x + 0.4f, z - 0.5f, z + 0.5f, 0.12f, H, pilaster, false);
            m.Box(x + 2.0f, x + 10.0f, z + s * 0.3f - 0.08f, z + s * 0.3f + 0.08f, 4.3f, 6.7f, glass, false);
        }
    }
    // roof slab, parapet lips, skylights (2 x 6), roof plant
    m.Box(-L - 0.75f, L + 0.75f, -Wd - 0.75f, Wd + 0.75f, H, H + 0.8f, roof);
    for (float s : {-1.0f, 1.0f}) m.Box(-L - 0.75f, L + 0.75f, s * (Wd + 0.55f) - 0.2f, s * (Wd + 0.55f) + 0.2f, H + 0.8f, H + 1.4f, {0.42f, 0.44f, 0.46f}, false);
    for (int i = 0; i < 6; ++i)
        for (float z : {-7.0f, 7.0f}) {
            const float x = -L + 12.0f + i * 19.0f;
            m.Box(x - 6.5f, x + 6.5f, z - 4.0f, z + 4.0f, H + 0.8f, H + 1.3f, frame, false);
            m.Box(x - 6.0f, x + 6.0f, z - 3.5f, z + 3.5f, H + 1.3f, H + 1.42f, glass, false);
        }
    for (int i = 0; i < 4; ++i) m.Box(-46.2f + i * 30.0f, -43.8f + i * 30.0f, -1.2f, 1.2f, H + 0.8f, H + 2.2f, {0.6f, 0.63f, 0.65f}, false);
    // end portals: a lintel over five openings, pillars between them, a yellow strip over the doors
    for (float ex : {-L, L}) {
        m.Box(ex - 0.4f, ex + 0.4f, -Wd, Wd, 6.6f, H, wall);
        m.Box(ex - 0.5f, ex + 0.5f, -Wd - 0.2f, Wd + 0.2f, 6.4f, 6.8f, trim, false);
        float z0 = -Wd;
        for (float z : roads) {
            if (z - 2.2f > z0 + 0.05f) m.Box(ex - 0.4f, ex + 0.4f, z0, z - 2.2f, 0.12f, 6.6f, wall);
            z0 = z + 2.2f;
        }
        m.Box(ex - 0.4f, ex + 0.4f, z0, Wd, 0.12f, 6.6f, wall);
    }
    m.Box(-L + 2, L - 2, -Wd + 1, Wd - 1, 0.12f, 0.14f, kDark, false); // the shed floor
    // carriage wash over the sidings
    for (float x : {-10.0f, 10.0f})
        for (float z : {-37.0f, -23.0f}) m.Box(x - 0.3f, x + 0.3f, z - 0.3f, z + 0.3f, 0.1f, 7.0f, {0.31f, 0.44f, 0.56f}, false);
    m.Box(-11, 11, -38, -22, 7.0f, 8.0f, {0.44f, 0.56f, 0.69f});
    // office block, fuel tanks and the depot sign at the front
    m.Box(-59, -37, 20, 32, 0.1f, 7.0f, wall);
    m.Box(-59.2f, -36.8f, 19.8f, 32.2f, 7.0f, 7.6f, roof);
    for (float x = -56.0f; x <= -40.0f; x += 4.0f) m.Box(x - 1.2f, x + 1.2f, 32.0f, 32.1f, 2.7f, 4.3f, glass, false);
    for (float x : {-32.0f, -26.0f}) m.Cylinder(x, 26.0f, 2.4f, 0.1f, 6.0f, {0.89f, 0.89f, 0.86f}, 10);
    m.Box(-37, -23, Wd + 0.6f, Wd + 0.9f, H - 2.4f, H - 0.4f, {0.12f, 0.31f, 0.56f}, false);
    // signals at the four throats, light masts along the sidings
    for (float x : {-hw + 2.0f, hw - 2.0f})
        for (float zz : {14.0f, -35.2f}) {
            m.Box(x - 0.12f, x + 0.12f, zz - 0.12f, zz + 0.12f, 0.1f, 5.0f, kSteel, false);
            m.Box(x - 0.25f, x + 0.25f, zz - 0.2f, zz + 0.2f, 4.0f, 5.4f, kDark, false);
        }
    for (float x : {-50.0f, 50.0f}) {
        m.Cylinder(x, -20.0f, 0.35f, 0.1f, 22.0f, {0.55f, 0.57f, 0.59f}, 6);
        m.Box(x - 1.5f, x + 1.5f, -21.5f, -18.5f, 22.0f, 22.6f, {0.17f, 0.18f, 0.19f}, false);
    }
}
inline void TrainStation(Mesher& m, const BuildingDef& d, float inner) {
    const float hd = d.D() * 0.5f;
    const float pw = 7.5f; // platform width
    for (float side : {-1.0f, 1.0f}) {
        const float za = side > 0 ? inner : -(inner + pw), zb = side > 0 ? inner + pw : -inner;
        m.Box(-90, 90, za, zb, 0.0f, 0.95f, {0.72f, 0.72f, 0.70f}, false);
        const float ze = side > 0 ? inner : -inner - 0.45f; // the yellow safety edge along the track side
        m.Box(-90, 90, ze, ze + 0.45f, 0.95f, 0.97f, kYellow, false);
        const float pz = side * (inner + pw - 1.2f);
        for (float x = -84.0f; x <= 84.1f; x += 12.0f) m.Box(x - 0.2f, x + 0.2f, pz - 0.2f, pz + 0.2f, 0.95f, 4.7f, kSteel, false);
        const float r0 = side > 0 ? inner + 0.8f : -(inner + pw + 0.3f), r1 = side > 0 ? inner + pw + 0.3f : -(inner + 0.8f);
        m.Box(-88, 88, r0, r1, 4.7f, 5.0f, {0.28f, 0.40f, 0.58f});
    }
    const float z0 = inner + pw + 2.2f, z1 = hd - 1.5f;
    if (z1 > z0 + 3.0f) {
        m.Box(-30, 30, z0, z1, 0.0f, 8.5f, {0.76f, 0.69f, 0.58f});
        m.Gable(-32, 32, z0 - 0.8f, z1 + 0.8f, 8.5f, 11.5f, {0.36f, 0.30f, 0.28f}, true);
        m.Box(-10, 10, z1, z1 + 2.5f, 5.2f, 5.5f, {0.3f, 0.3f, 0.34f}, false);
        m.Box(-28, 28, z1 - 0.1f, z1 + 0.05f, 1.0f, 4.0f, {0.18f, 0.28f, 0.38f}, false);
    }
    for (float x : {-62.0f, 62.0f}) { // footbridges
        m.Box(x - 1.6f, x + 1.6f, -(inner + pw + 0.5f), inner + pw + 0.5f, 6.2f, 7.0f, kSteel);
        for (float s : {-1.0f, 1.0f}) m.Box(x - 1.6f, x + 1.6f, s * (inner + pw - 0.5f) - 0.8f, s * (inner + pw - 0.5f) + 0.8f, 0.95f, 6.2f, kSteel, false);
    }
}
inline void CargoTerminal(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f;
    m.Box(-hw, hw, -hd, hd, 0.0f, 0.15f, kGravel, false);
    m.Box(-92, 62, -44, -20, 0.15f, 10.0f, {0.62f, 0.55f, 0.45f});
    m.Box(-93, 63, -45, -19, 10.0f, 10.5f, {0.35f, 0.35f, 0.38f}, false);
    for (int i = 0; i < 6; ++i) m.Box(-84.0f + 24.0f * i, -76.0f + 24.0f * i, -19.8f, -19.4f, 0.15f, 5.0f, kDark, false);
    m.Box(82, 106, -40, -20, 0.15f, 6.0f, {0.8f, 0.78f, 0.72f});
    for (float z : {-6.0f, 8.0f, 22.0f}) TrackX(m, z, -108.0f, 108.0f);
    TrainX(m, 8.0f, -90.0f, 5, 11, true);
    TrainX(m, 22.0f, -10.0f, 4, 12, true);
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 15; ++i) {
            const uint32_t h = Hash(i, r + 31);
            if (h % 5 == 0) continue;
            ContainerStack(m, -104.0f + 14.0f * i, 32.0f + r * 6.0f, 1 + (int)(h % 3), h);
        }
    for (float x : {-34.0f, 34.0f}) // the gantry: portal frames straddling the tracks
        for (float z : {-14.0f, 30.0f}) m.Box(x - 0.8f, x + 0.8f, z - 0.8f, z + 0.8f, 0.15f, 15.0f, kYellow, false);
    for (float x : {-34.0f, 34.0f}) m.Box(x - 1.0f, x + 1.0f, -14.0f, 30.0f, 15.0f, 17.0f, kYellow);
    for (float z : {-14.0f, 30.0f}) m.Box(-34.0f, 34.0f, z - 1.0f, z + 1.0f, 15.0f, 17.0f, kYellow);
    m.Box(-2.0f, 2.0f, 4.0f, 10.0f, 12.0f, 15.0f, {0.2f, 0.2f, 0.22f}); // trolley
}
inline void Shipyard(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f, shoreZ = d.D() * (0.5f - d.dock);
    m.Box(-hw, hw, -hd, shoreZ, 0.0f, 0.5f, kConcrete, false);
    Pier(m, -hw, hw, shoreZ, shoreZ + 22.0f, 2.4f);
    m.Box(-76, -4, -44, -8, 0.5f, 18.0f, {0.50f, 0.55f, 0.60f});
    m.Gable(-78, -2, -46, -6, 18.0f, 23.0f, {0.28f, 0.33f, 0.40f}, true);
    m.Box(-60, -20, -8.0f, -7.4f, 0.5f, 14.0f, kDark, false);
    m.Box(-76, -56, -4, 14, 0.5f, 8.0f, {0.80f, 0.78f, 0.72f});
    m.Box(24, 60, -26, shoreZ + 14.0f, 0.5f, 0.8f, {0.5f, 0.5f, 0.48f}, false); // the slipway
    HullZ(m, 35, 49, -28, shoreZ + 8.0f, 1.2f, 7.5f, {0.55f, 0.14f, 0.10f});
    m.Box(37, 47, -24, -8, 7.5f, 12.5f, kWhite);
    m.Box(40, 44, -19, -14, 12.5f, 15.0f, {0.8f, 0.3f, 0.1f}, false);
    for (float x : {24.0f, 60.0f})
        for (float z : {-18.0f, 6.0f}) m.Box(x - 0.9f, x + 0.9f, z - 0.9f, z + 0.9f, 0.5f, 28.0f, kYellow, false);
    for (float z : {-18.0f, 6.0f}) m.Box(22.0f, 62.0f, z - 1.3f, z + 1.3f, 26.0f, 29.0f, kYellow);
    for (float x : {24.0f, 60.0f}) m.Box(x - 1.3f, x + 1.3f, -18.0f, 6.0f, 26.0f, 29.0f, kYellow);
    m.Box(38.0f, 46.0f, -8.0f, -2.0f, 22.5f, 26.0f, {0.2f, 0.2f, 0.22f});
    m.Beam({42.0f, 22.5f, -5.0f}, {42.0f, 14.0f, -5.0f}, 0.2f, kSteel);
    m.Cylinder(-30.0f, 6.0f, 5.0f, 0.5f, 9.0f, {0.75f, 0.76f, 0.78f});
    m.Cylinder(-16.0f, 6.0f, 5.0f, 0.5f, 9.0f, {0.75f, 0.76f, 0.78f});
    m.Box(-hw, -hw + 14.0f, 18.0f, 26.0f, 0.5f, 5.0f, {0.6f, 0.62f, 0.66f}); // gatehouse / stores
}
inline void CargoHarbor(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f, shoreZ = d.D() * (0.5f - d.dock);
    m.Box(-hw, hw, -hd, shoreZ, 0.0f, 0.5f, kConcrete, false);
    Pier(m, -hw, hw, shoreZ, shoreZ + 26.0f, 2.4f);
    m.Box(-90, -30, -52, -34, 0.5f, 12.0f, {0.6f, 0.5f, 0.4f});
    m.Box(-91, -29, -53, -33, 12.0f, 12.6f, {0.35f, 0.35f, 0.38f}, false);
    m.Box(10, 90, -52, -34, 0.5f, 10.0f, {0.52f, 0.56f, 0.6f});
    m.Box(9, 91, -53, -33, 10.0f, 10.6f, {0.33f, 0.35f, 0.4f}, false);
    for (int r = 0; r < 4; ++r)
        for (int i = 0; i < 10; ++i) {
            const uint32_t h = Hash(i + 3, r + 5);
            if (h % 6 == 0) continue;
            ContainerStack(m, -88.0f + 17.5f * i, -24.0f + r * 9.0f, 1 + (int)(h % 4), h);
        }
    StsCrane(m, -40, shoreZ + 4.0f, shoreZ + 20.0f);
    StsCrane(m, 40, shoreZ + 4.0f, shoreZ + 20.0f);
    const float z0 = shoreZ + 30.0f, z1 = shoreZ + 54.0f; // a container ship alongside
    Hull(m, -75, 80, z0, z1, -5.5f, 2.5f, {0.2f, 0.22f, 0.3f}, true);
    for (int i = 0; i < 9; ++i) {
        const int h = 1 + (int)(Hash(i, 9) % 3);
        for (int k = 0; k < h; ++k) m.Box(-60.0f + 14.0f * i, -48.0f + 14.0f * i, z0 + 2.0f, z1 - 2.0f, 2.5f + k * 2.7f, 5.2f + k * 2.7f, ContainerColor(Hash(i, k + 40)), false);
    }
    m.Box(-74, -64, z0 + 3, z1 - 3, 2.5f, 12.0f, kWhite);
}
inline void PassengerHarbor(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f, shoreZ = d.D() * (0.5f - d.dock);
    m.Box(-hw, hw, -hd, shoreZ, 0.0f, 0.4f, {0.66f, 0.64f, 0.6f}, false);
    m.Box(-34, 34, -26, -6, 0.4f, 8.0f, {0.6f, 0.72f, 0.8f});
    m.Box(-36, 36, -28, -4, 8.0f, 9.0f, {0.9f, 0.9f, 0.92f});
    m.Box(-33, 33, -6.2f, -5.9f, 1.0f, 6.5f, {0.16f, 0.26f, 0.36f}, false);
    m.Box(-34, 34, -6, 8, 4.8f, 5.1f, {0.9f, 0.9f, 0.92f}); // the covered walkway to the pier
    Pier(m, -5, 5, shoreZ - 2.0f, shoreZ + 30.0f, 2.4f);
    Pier(m, -5, 22, shoreZ + 22.0f, shoreZ + 30.0f, 2.4f);
    HullZ(m, 8, 22, shoreZ + 2.0f, shoreZ + 40.0f, -4.0f, 1.2f, kWhite);
    m.Box(9.5f, 20.5f, shoreZ + 8.0f, shoreZ + 30.0f, 1.2f, 4.6f, {0.95f, 0.95f, 0.97f});
    m.Box(12.0f, 18.0f, shoreZ + 12.0f, shoreZ + 20.0f, 4.6f, 6.2f, {0.85f, 0.3f, 0.2f}, false);
}
inline void RunwayMarks(Mesher& m, float x0, float x1, float zc, float width) {
    for (float x = x0 + 12.0f; x < x1 - 40.0f; x += 30.0f) m.Box(x, x + 14.0f, zc - 0.4f, zc + 0.4f, 0.3f, 0.33f, kWhite, false);
    for (int k = 0; k < 6; ++k)
        for (float xe : {x0 + 3.0f, x1 - 11.0f}) m.Box(xe, xe + 8.0f, zc - width * 0.5f + 2.0f + k * (width - 4.0f) / 5.0f - 0.5f, zc - width * 0.5f + 2.0f + k * (width - 4.0f) / 5.0f + 0.5f, 0.3f, 0.33f, kWhite, false);
}
inline void SmallAirfield(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f;
    m.Box(-hw, hw, -hd, hd, 0.0f, 0.1f, {0.36f, 0.5f, 0.3f}, false);
    m.Box(-186, 186, -50, -20, 0.1f, 0.3f, kAsphalt, false);
    RunwayMarks(m, -186, 186, -35.0f, 30.0f);
    m.Box(-170, 170, -12, 2, 0.1f, 0.3f, kAsphalt, false);
    for (float x : {-110.0f, 0.0f, 110.0f}) m.Box(x - 7, x + 7, -20, -12, 0.1f, 0.3f, kAsphalt, false);
    m.Box(-60, 60, 2, 30, 0.1f, 0.3f, {0.5f, 0.5f, 0.52f}, false);
    m.Box(-24, 24, 34, 48, 0.1f, 8.0f, {0.85f, 0.85f, 0.88f});
    m.Box(-26, 26, 32, 50, 8.0f, 8.6f, {0.3f, 0.35f, 0.5f}, false);
    m.Box(-22, 22, 33.8f, 34.0f, 1.0f, 5.0f, {0.18f, 0.28f, 0.38f}, false);
    m.Box(40, 46, 38, 44, 0.1f, 16.0f, {0.8f, 0.8f, 0.82f});
    m.Box(38, 48, 36, 46, 16.0f, 20.0f, {0.28f, 0.48f, 0.58f});
    m.Box(37, 49, 35, 47, 20.0f, 20.6f, {0.3f, 0.3f, 0.34f}, false);
    for (float x : {-150.0f, -118.0f}) {
        m.Box(x, x + 28, 8, 36, 0.1f, 9.0f, {0.6f, 0.62f, 0.66f});
        m.Gable(x - 1, x + 29, 7, 37, 9.0f, 12.5f, kRoofBlue, false);
        m.Box(x + 4, x + 24, 36.0f, 36.4f, 0.1f, 7.0f, kDark, false);
    }
    Plane(m, -25, 14, 12.0f, false);
    Plane(m, 10, 16, 12.0f, false);
    Plane(m, 45, 12, 16.0f, true);
}
inline void RegionalAirport(Mesher& m, const BuildingDef& d) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f;
    m.Box(-hw, hw, -hd, hd, 0.0f, 0.1f, {0.36f, 0.5f, 0.3f}, false);
    m.Box(-250, 250, -100, -55, 0.1f, 0.3f, kAsphalt, false);
    RunwayMarks(m, -250, 250, -77.5f, 45.0f);
    m.Box(-230, 230, -48, -30, 0.1f, 0.3f, kAsphalt, false);
    for (float x : {-190.0f, -80.0f, 40.0f, 170.0f}) m.Box(x, x + 18, -55, -48, 0.1f, 0.3f, kAsphalt, false);
    m.Box(-170, 170, -30, 44, 0.1f, 0.3f, {0.5f, 0.5f, 0.52f}, false);
    m.Box(-92, 92, 66, 98, 0.1f, 14.0f, {0.8f, 0.82f, 0.86f});
    m.Box(-96, 96, 62, 102, 14.0f, 15.2f, {0.25f, 0.3f, 0.4f});
    m.Box(-90, 90, 65.8f, 66.1f, 1.5f, 10.0f, {0.16f, 0.26f, 0.36f}, false);
    for (int i = 0; i < 6; ++i) {
        const float x = -75.0f + 30.0f * i;
        m.Box(x - 1.5f, x + 1.5f, 40, 66, 5.0f, 8.0f, {0.6f, 0.6f, 0.65f});
        Plane(m, x, 26.0f, 38.0f, true);
    }
    m.Box(112, 120, 74, 82, 0.1f, 34.0f, {0.8f, 0.8f, 0.82f});
    m.Box(109, 123, 71, 85, 34.0f, 39.0f, {0.28f, 0.48f, 0.58f});
    m.Box(108, 124, 70, 86, 39.0f, 39.8f, {0.3f, 0.3f, 0.34f}, false);
    for (float x : {150.0f, 190.0f}) {
        m.Box(x, x + 36, 20, 62, 0.1f, 16.0f, {0.6f, 0.62f, 0.66f});
        m.Gable(x - 1, x + 37, 19, 63, 16.0f, 21.0f, kRoofBlue, false);
    }
    for (float z : {70.0f, 88.0f, 106.0f}) m.Cylinder(-190.0f, z, 6.0f, 0.1f, 8.0f, {0.75f, 0.76f, 0.78f});
    m.Box(-100, 100, 104, 110, 0.1f, 0.2f, kAsphalt, false);
    for (int i = 0; i < 20; ++i) {
        const float x = -90.0f + 9.5f * i;
        m.Box(x, x + 2.0f, 104.5f, 109.0f, 0.2f, 1.6f, {0.55f + 0.1f * (Hash(i, 2) % 4), 0.4f, 0.35f}, false);
    }
    Plane(m, -230.0f, -77.5f, 38.0f, true);
}

inline void BuildModel(Mesher& m, const BuildingDef& d, float platformInner) {
    switch (d.model) {
    case Model::RailDepot: RailDepot(m, d); break;
    case Model::TrainStation: TrainStation(m, d, platformInner); break;
    case Model::CargoTerminal: CargoTerminal(m, d); break;
    case Model::Shipyard: Shipyard(m, d); break;
    case Model::CargoHarbor: CargoHarbor(m, d); break;
    case Model::PassengerHarbor: PassengerHarbor(m, d); break;
    case Model::SmallAirfield: SmallAirfield(m, d); break;
    case Model::RegionalAirport: RegionalAirport(m, d); break;
    }
}

// The construction site: dirt and a slab, the building rising inside corner posts, a tower crane slewing beside it.
inline void ConstructionSite(Mesher& m, const BuildingDef& d, float progress, float time, uint64_t id) {
    const float hw = d.W() * 0.5f, hd = d.D() * 0.5f;
    m.Box(-hw, hw, -hd, hd, -0.2f, 0.06f, {0.42f, 0.34f, 0.25f}, false);
    const float rise = std::clamp((progress - 0.12f) / 0.78f, 0.0f, 1.0f);
    const float H = d.height;
    const float mx = -hw + 6.0f, mz = -hd + 6.0f, mastH = H + 8.0f, jib = std::min(60.0f, std::max(18.0f, hw * 0.35f)), w = 0.8f;
    for (int i = 0; i < 4; ++i) m.Box(mx + ((i & 1) ? w : -w) - 0.12f, mx + ((i & 1) ? w : -w) + 0.12f, mz + ((i & 2) ? w : -w) - 0.12f, mz + ((i & 2) ? w : -w) + 0.12f, 0.0f, mastH, kYellow, false);
    const float slew = std::sin(time * 0.35f + (float)(id % 7)) * 1.2f + 0.8f;
    const Vec3 top{mx, mastH, mz}, tip{mx + std::cos(slew) * jib, mastH, mz + std::sin(slew) * jib}, tail{mx - std::cos(slew) * jib * 0.3f, mastH, mz - std::sin(slew) * jib * 0.3f};
    m.Beam(top, tip, 0.6f, kYellow);
    m.Beam(top, tail, 0.6f, kYellow);
    m.Beam({mx, mastH, mz}, {mx, mastH + 4.0f, mz}, 0.5f, kYellow);
    const float tr = 0.5f + 0.3f * std::sin(time * 0.6f);
    const Vec3 hook{mx + (tip.x - mx) * tr, mastH, mz + (tip.z - mz) * tr};
    m.Beam(hook, {hook.x, mastH * (0.3f + 0.2f * std::sin(time * 0.9f)), hook.z}, 0.08f, kSteel);
    // scaffold posts round the footprint's main mass, as high as the building has risen
    const float sh = rise * H + 1.5f;
    for (float x = -hw * 0.6f; x <= hw * 0.6f + 0.1f; x += hw * 0.3f)
        for (float z : {-hd * 0.55f, hd * 0.2f}) m.Box(x - 0.1f, x + 0.1f, z - 0.1f, z + 0.1f, 0.0f, sh, kSteel, false);
}

// The building at `p` for the current moment: solid (Done), site + rising shell (Building) or just the shell (hologram).
// The mesh is in world space. `time` animates the crane.
inline void BuildPlaced(const Placed& p, float time, std::vector<Vertex>& tris, std::vector<Vertex>* edges) {
    const BuildingDef& d = Defs()[p.def];
    Mesher m(tris, edges, p.c, p.front, p.y);
    if (p.state == BState::Building) {
        ConstructionSite(m, d, p.progress, time, p.id);
        const float rise = std::clamp((p.progress - 0.12f) / 0.78f, 0.0f, 1.0f);
        if (rise > 0.0f) {
            m.sy = std::max(0.03f, rise);
            BuildModel(m, d, p.platformInner);
        }
    } else BuildModel(m, d, p.platformInner);
}

// The solid geometry becomes the hologram: unlit blue (or red), scan lines drifting upward, brighter on top faces and edges.
inline void Holographic(const std::vector<Vertex>& src, std::vector<Vertex>& out, float time, bool valid) {
    const Vec3 base = valid ? Vec3{0.10f, 0.45f, 1.0f} : Vec3{1.0f, 0.18f, 0.14f};
    for (const Vertex& v : src) {
        const float scan = 0.55f + 0.45f * std::sin(v.py * 1.3f - time * 3.0f);
        const float k = (0.55f + 0.25f * v.ny) * (0.7f + 0.3f * scan) * (0.9f + 0.1f * std::sin(time * 2.0f));
        out.push_back({v.px, v.py, v.pz, 0, 0, 0, base.x * k + 0.04f, base.y * k + 0.06f, base.z * k + 0.12f, 1.0f});
    }
}
inline void HolographicEdges(const std::vector<Vertex>& src, std::vector<Vertex>& out, float time, bool valid) {
    const Vec3 c = valid ? Vec3{0.45f, 0.95f, 1.6f} : Vec3{1.7f, 0.55f, 0.45f};
    const float k = 0.85f + 0.15f * std::sin(time * 2.0f);
    for (const Vertex& v : src) out.push_back({v.px, v.py, v.pz, 0, 0, 0, c.x * k, c.y * k, c.z * k, 1.0f});
}

} // namespace tmodel
} // namespace city
} // namespace tbd
