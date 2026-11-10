// The engine path for rail junctions: like the plain track, a junction is a flat SPOM box (10 triangles: top and four
// sides, no bottom) drawn by the track's own parallax shader (TrackPS, mesh.hlsl) with a junction texture instead of the
// repeating track tile. The texture (albedo + height, tools/rail_junction_tex) draws the whole layout: rails, blades,
// frogs, check rails, long switch timbers, ballast filling the gaps between tracks, the level crossing deck. Only
// what sticks up stays geometry (JunctionProps: point machines, gates, signals, ramps, markings).
//
//   Layouts    a small set of canonical layouts (JunctionLayouts), each baked once, right-handed; a left-hand junction
//              is the same texture mirrored. A junction of another angle uses the nearest canonical layout of its type
//              (NearestLayout); the rail tool should snap switch angles to them so the graph matches the picture.
//   Seams      every route of a layout is a whole number of sleeper pitches long and starts a sleeper half a pitch in,
//              exactly like a plain track sweep starting at the junction's edge, and the plain parts of the texture
//              are stamped from the plain track tile itself: same scale, phase, colours and edge profile.
//   Box        rect in the layout's frame (x along the stem, z across), from the rail head (kRailTop) down kTrackDepth.
//              Vertex encoding as TrackSweep: uv = metres (across from rect.z0, along from rect.x0), color.r the
//              vertex's height under the rail head, color.g the rect's width across, color.b its length along,
//              alpha kJunctionSpomAlpha (TrackPS reads the texture once over the rect instead of tiling 4.4 x 0.6 m).
// Header only, headless (tests\rail_junction_test.cpp).
#pragma once
#include <string>
#include <vector>

#include "rail_junction_mesh.h"

namespace tbd {
namespace city {

constexpr float kJunctionSpomAlpha = 0.84f; // TrackPS's tag is 0.83 (tiling track); 0.84: one junction texture over the box
constexpr float kJunctionTexel = 0.02f;     // metres per texel of the baked layouts (long side capped at 2048)

struct JunctionLayout {
    std::string id; // file stem of its textures: assets/rail/junction/tex/<id>_{albedo.jpg,height.png}
    JunctionParams p;
};

// The canonical layouts the game ships (right-handed; Left mirrors).
inline const std::vector<JunctionLayout>& JunctionLayouts() {
    static const std::vector<JunctionLayout> v = [] {
        std::vector<JunctionLayout> l;
        auto add = [&](const char* id, JType t, float ang, float R = 150.0f, float road = 9.0f, float walk = 2.0f) {
            JunctionLayout L;
            L.id = id, L.p.type = t, L.p.hand = t == JType::Wye || t == JType::ThreeWay || t == JType::LevelCrossing ? Hand::None : Hand::Right;
            L.p.angleDeg = ang, L.p.radius = R, L.p.lod = 2; // lod 2: fine curves for baking
            L.p.roadWidth = road, L.p.sidewalk = walk;
            l.push_back(L);
        };
        add("turnout_08", JType::Turnout, 8), add("turnout_12", JType::Turnout, 12), add("turnout_20", JType::Turnout, 20, 90);
        add("wye_08", JType::Wye, 8), add("threeway_12", JType::ThreeWay, 12);
        add("diamond_30", JType::Diamond, 30), add("diamond_45", JType::Diamond, 45), add("diamond_60", JType::Diamond, 60);
        add("diamond_90", JType::Diamond, 90), add("slip_15", JType::SingleSlip, 15);
        add("crossover_10", JType::Crossover, 10), add("scissors_10", JType::Scissors, 10);
        // level crossings: five angles x the common carriageway classes (two-lane 6 m, 9 m, four-lane 14 m, six-lane 21 m;
        // their sidewalks 2.5, 2, 3, 4 m). ids: level_<angle> (9 m), level_<angle>_w6, _w14, _w21
        for (int w : {9, 6, 14, 21})
            for (int a : {90, 75, 60, 45, 30}) {
                const std::string id = "level_" + std::to_string(a) + (w == 9 ? std::string() : "_w" + std::to_string(w));
                add(id.c_str(), JType::LevelCrossing, (float)a, 150, (float)w, w == 6 ? 2.5f : w == 9 ? 2.0f : w == 14 ? 3.0f : 4.0f);
            }
        return l;
    }();
    return v;
}

// The canonical layout for a junction: same type, nearest angle (level crossings: then the nearest carriageway width,
// 1 m of width counting as 1 deg). mirror: drawn left-handed.
inline int NearestLayout(const JunctionParams& p, bool* mirror = nullptr) {
    int best = -1;
    float bd = 1e9f;
    const auto& L = JunctionLayouts();
    for (size_t i = 0; i < L.size(); ++i) {
        if (L[i].p.type != p.type) continue;
        float d = std::fabs(L[i].p.angleDeg - p.angleDeg);
        if (p.type == JType::LevelCrossing) d += std::fabs(L[i].p.roadWidth - p.roadWidth);
        if (d < bd) bd = d, best = (int)i;
    }
    if (mirror) *mirror = p.hand == Hand::Left;
    return best;
}

struct PlaneRect {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    float W() const { return z1 - z0; } // across
    float L() const { return x1 - x0; } // along
};
// The box over a layout: every rail-carrying route's bed (kBedFootHalf each side), the crossing deck for level crossings.
inline PlaneRect JunctionPlaneRect(const JunctionParams& p) {
    using namespace jmesh;
    PlaneRect r{1e9f, 1e9f, -1e9f, -1e9f};
    std::vector<char> virt;
    const auto routes = JunctionRoutes(AdjustParams(p), &virt);
    const float half = p.type == JType::LevelCrossing ? kDeckHalf : kBedFootHalf;
    for (size_t k = 0; k < routes.size(); ++k) {
        if (virt[k]) continue;
        for (const RPt& q : routes[k])
            for (float sd : {-half, half}) {
                const V2 e = Offset(q, sd);
                r.x0 = std::min(r.x0, e.x), r.x1 = std::max(r.x1, e.x), r.z0 = std::min(r.z0, e.z), r.z1 = std::max(r.z1, e.z);
            }
    }
    return r;
}
// Texture size of a layout: kJunctionTexel, long side at most 2048, both multiples of 4 (ASTC / BC blocks).
inline void LayoutTexSize(const PlaneRect& r, int& nAlong, int& nAcross) {
    float t = kJunctionTexel;
    if (std::max(r.L(), r.W()) / t > 2048.0f) t = std::max(r.L(), r.W()) / 2048.0f;
    nAlong = ((int)std::ceil(r.L() / t) + 3) / 4 * 4, nAcross = ((int)std::ceil(r.W() / t) + 3) / 4 * 4;
    nAlong = std::min(nAlong, 2048), nAcross = std::min(nAcross, 2048);
}

// The SPOM box of a junction drawn with its canonical layout (mirrored for Left), plus the props. JMat::Spom holds the
// box (10 tris, TrackSweep-style vertices, see the header); layout / mirror say which texture and which way.
struct JunctionPlane {
    JunctionMesh mesh;
    int layout = -1;
    bool mirror = false;
    PlaneRect rect; // in the junction's own frame (mirrored already)
};
inline JunctionPlane BuildJunctionPlane(const JunctionParams& want) {
    using namespace jmesh;
    JunctionPlane J;
    JunctionParams p = AdjustParams(want, &J.mesh.adjusted, &J.mesh.note);
    J.layout = NearestLayout(p, &J.mirror);
    if (J.layout < 0) return J;
    const JunctionParams& lp = JunctionLayouts()[J.layout].p;
    J.mesh.angleDeg = lp.angleDeg, J.mesh.radius = lp.radius;
    const PlaneRect r = JunctionPlaneRect(lp);
    const float m = J.mirror ? -1.0f : 1.0f, yT = kRailTop, yB = kRailTop - mesh::kTrackDepth, W = r.W(), L = r.L();
    J.rect = J.mirror ? PlaneRect{r.x0, -r.z1, r.x1, -r.z0} : r;
    auto& o = J.mesh.byMat[(int)JMat::Spom];
    // canonical (x, z, y) -> vertex: position mirrored, uv kept (the texture is the canonical picture)
    // As TrackSweep: the vertex normal carries the forward direction (+x of the layout), not the face's; color.g is the
    // width across, negative when mirrored (the shader flips its across axis), color.b the length along.
    auto vtx = [&](float x, float z, float y) {
        o.push_back({x, y, z * m, 1, 0, 0, y - yT, W * m, L, kJunctionSpomAlpha, z - r.z0, x - r.x0});
    };
    auto quad = [&](const float c[4][3], Vec3 n) { // c: (x, z, y) corners in order; wound CCW about n after mirroring
        static const int ccw[6] = {0, 1, 2, 0, 2, 3}, cw[6] = {0, 3, 2, 0, 2, 1};
        const Vec3 a{c[0][0], c[0][2], c[0][1] * m}, b{c[1][0], c[1][2], c[1][1] * m}, d{c[3][0], c[3][2], c[3][1] * m};
        const int* idx = Dot(Cross(b - a, d - a), Vec3{n.x, n.y, n.z * m}) >= 0 ? ccw : cw;
        for (int k = 0; k < 6; ++k) vtx(c[idx[k]][0], c[idx[k]][1], c[idx[k]][2]);
    };
    const float top[4][3] = {{r.x0, r.z0, yT}, {r.x1, r.z0, yT}, {r.x1, r.z1, yT}, {r.x0, r.z1, yT}};
    quad(top, {0, 1, 0});
    const float zn[4][3] = {{r.x0, r.z0, yT}, {r.x1, r.z0, yT}, {r.x1, r.z0, yB}, {r.x0, r.z0, yB}};
    const float zp[4][3] = {{r.x0, r.z1, yT}, {r.x1, r.z1, yT}, {r.x1, r.z1, yB}, {r.x0, r.z1, yB}};
    const float xn[4][3] = {{r.x0, r.z0, yT}, {r.x0, r.z1, yT}, {r.x0, r.z1, yB}, {r.x0, r.z0, yB}};
    const float xp[4][3] = {{r.x1, r.z0, yT}, {r.x1, r.z1, yT}, {r.x1, r.z1, yB}, {r.x1, r.z0, yB}};
    quad(zn, {0, 0, -1}), quad(zp, {0, 0, 1}), quad(xn, {-1, 0, 0}), quad(xp, {1, 0, 0});
    JunctionParams pp = lp;
    pp.hand = p.hand, pp.lod = p.lod, pp.spom = true, pp.roadWidth = p.roadWidth, pp.sidewalk = p.sidewalk, pp.gate = p.gate;
    JunctionProps(J.mesh, pp);
    return J;
}

} // namespace city
} // namespace tbd
