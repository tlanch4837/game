// Snapping a building being placed beside the nearest road, entrance first. ONE helper for every placement tool
// (utilities, transport depots / terminals / rail yards, services, telecom, industry hubs, waste; harbours face the
// water and stations follow their track, so they keep their own rules). Header only, headless (tests\building_snap_test.cpp).
//
//   Entrance   each building has an entrance edge: the side with its door / driveway, in the footprint's own frame
//              (placement_rotation.h EdgeMids order: 0 the front (+Z in model space, the default), 1 the side
//              (front.z, -front.x), 2 the back, 3 the other side). Catalogs store it per entry (BuildingDef::entrance).
//   Snap       while placing, the nearest qualifying road (any road a driveway can join: not a railway, highway or path)
//              whose surface edge is within `range` of the cursor pulls the ghost beside it: turned so the ENTRANCE faces
//              the road (quarter turns 1..3 turn it on from there in 90 degree steps relative to the road), its road-side
//              edge `gap` metres off the kerb (the profile's full width: carriageway, parking and sidewalks), and slid
//              along the road with the cursor (the foot of the cursor on the road's centre line).
//   Free       far from every road (or with the Rotate dial out: the host passes snap = false) the ghost stands where the
//              cursor is, turned by the tool's own heading, as before.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "placement_rotation.h"

namespace tbd {
namespace city {

constexpr float kBuildingSnapRange = 28.0f; // metres from the road's edge to the cursor (a fingertip on a tablet)
constexpr float kBuildingSnapGap = 2.0f;    // the setback between the kerb's back and the building's road-side edge

// Can segment s come within `reach` of p at all? (its control points' box, grown by its half width: a cheap reject)
inline bool SegNear(const Network& net, int s, V2 p, float reach) {
    const Segment& g = net.segs[s];
    const V2 q[4] = {XZ(net.nodes[g.a].p), g.c1, g.c2, XZ(net.nodes[g.b].p)};
    float x0 = q[0].x, x1 = q[0].x, z0 = q[0].z, z1 = q[0].z;
    for (const V2& v : q) x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
    const float r = reach + net.Prof(s).Half();
    return p.x >= x0 - r && p.x <= x1 + r && p.z >= z0 - r && p.z <= z1 + r;
}
// The roads a driveway can join.
inline bool EntranceRoad(const Network& net, int s) {
    const Cat c = net.Prof(s).cat;
    return c != Cat::Rail && c != Cat::Highway && c != Cat::Path;
}
// The snap reach for a building of W x D metres: a fingertip, or the building's own half size for big lots (the cursor
// sits on the ghost's middle, so a deep lot's middle is far from the road it is snapped to).
inline float SnapRangeFor(float W, float D) { return std::max(kBuildingSnapRange, 0.5f * std::max(W, D) + kBuildingSnapRange * 0.5f); }

// The outward normal of edge e (EdgeMids order) of a footprint facing `front`.
inline V2 EdgeNormal(V2 front, int e) {
    const V2 side{front.z, -front.x};
    switch (e & 3) {
    case 1: return side;
    case 2: return front * -1.0f;
    case 3: return side * -1.0f;
    default: return front;
    }
}
// The front that puts edge e's outward normal along n.
inline V2 FrontForEdge(V2 n, int e) {
    switch (e & 3) {
    case 1: return {-n.z, n.x};
    case 2: return n * -1.0f;
    case 3: return {n.z, -n.x};
    default: return n;
    }
}
// Which edge of a footprint facing `front` looks most along the unit direction u.
inline int EdgeFacing(V2 front, V2 u) {
    int best = 0;
    float bd = -2.0f;
    for (int e = 0; e < 4; ++e) {
        const float d = Dot2(EdgeNormal(front, e), u);
        if (d > bd) bd = d, best = e;
    }
    return best;
}

struct BuildingSnap {
    bool snapped = false;
    V2 c{}, front{0, 1};
    int seg = -1;          // the road it stands beside
    float t = 0;           // where on it (the foot of the cursor)
    V2 roadPt{};           // that point on the centre line
    V2 away{0, 1};         // from the road toward the building (perpendicular to the road there)
    float roadHalf = 0;    // the road's half width (kerb to kerb plus sidewalks)
    int roadEdge = 0;      // the footprint edge that faces the road (the entrance when quarter is 0)
    V2 entrance{};         // the middle of that edge
};

// The ghost beside the nearest qualifying road to `cursor` within range (< 0: SnapRangeFor), or snapped = false.
inline BuildingSnap SnapBesideRoad(const Network& net, V2 cursor, float W, float D, int quarter = 0, int entranceEdge = 0,
                                   float gap = kBuildingSnapGap, float range = -1.0f, const std::function<bool(int)>& ok = {}) {
    BuildingSnap r;
    r.c = cursor;
    if (range < 0) range = SnapRangeFor(W, D);
    int best = -1;
    float bestGap = range, bt = 0;
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!net.segs[s].alive) continue;
        if (ok ? !ok((int)s) : !EntranceRoad(net, (int)s)) continue;
        if (!SegNear(net, (int)s, cursor, bestGap)) continue;
        float t, d;
        net.Nearest((int)s, cursor, t, d);
        const float g = d - net.Prof((int)s).Half();
        if (g < bestGap) bestGap = g, best = (int)s, bt = t;
    }
    if (best < 0) return r;
    const Cubic cu = net.Curve(best);
    const V2 P = cu.At(bt), left = LeftOf(Norm(cu.Tan(bt)));
    const V2 away = Dot2(cursor - P, left) >= 0 ? left : left * -1.0f; // the side of the road the cursor is on
    const float half = net.Prof(best).Half();
    const V2 faceRoad = away * -1.0f;
    const V2 n = quarter % 4 == 0 ? faceRoad : TurnFront(faceRoad, 90.0f * (float)(quarter % 4)); // the entrance's normal
    r.front = FrontForEdge(n, entranceEdge);
    r.c = P + away * (half + gap + HalfExtentAlong(r.front, W, D, away));
    r.snapped = true;
    r.seg = best, r.t = bt, r.roadPt = P, r.away = away, r.roadHalf = half;
    r.roadEdge = EdgeFacing(r.front, faceRoad);
    r.entrance = r.c + faceRoad * HalfExtentAlong(r.front, W, D, away);
    return r;
}

// Road access: the first edge (the entrance edge first, then the others) whose middle has a qualifying road within reach
// of its kerb. Returns the edge (-1: none); seg the road.
inline int RoadAtEdge(const Network& net, V2 c, V2 front, float W, float D, int entranceEdge, float reach, int& seg) {
    V2 mid[4], out[4];
    EdgeMids(c, front, W, D, mid, out);
    seg = -1;
    for (int k = 0; k < 4; ++k) {
        const int e = (entranceEdge + k) & 3;
        int best = -1;
        float bg = reach;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || !EntranceRoad(net, (int)s) || !SegNear(net, (int)s, mid[e], reach + 1.0f)) continue;
            float t, d;
            net.Nearest((int)s, mid[e] + out[e] * 0.5f, t, d);
            const float g = d - net.Prof((int)s).Half();
            if (g < bg) bg = g, best = (int)s;
        }
        if (best >= 0) return seg = best, e;
    }
    return -1;
}

// The entrance mark on a ghost: a door bar along the entrance edge and an arrow from the road into the building
// (lines for DrawTransient). mid / out: the edge's middle and outward normal.
inline void EntranceMark(V2 mid, V2 out, float edgeLen, const std::function<float(float, float)>& ground, std::vector<Vertex>& lines, Vec3 col) {
    auto Y = [&](V2 q) { return (ground ? ground(q.x, q.z) : 0.0f) + 0.7f; };
    auto L = [&](V2 a, V2 b) {
        lines.push_back({a.x, Y(a), a.z, 0, 0, 0, col.x, col.y, col.z, 1});
        lines.push_back({b.x, Y(b), b.z, 0, 0, 0, col.x, col.y, col.z, 1});
    };
    const V2 along{out.z, -out.x};
    const float door = std::min(10.0f, edgeLen * 0.4f);
    for (float o : {0.0f, 0.4f}) L(mid - along * (door * 0.5f) - out * o, mid + along * (door * 0.5f) - out * o); // the door
    const V2 tail = mid + out * 9.0f, tip = mid - out * 1.0f;                                                   // the way in
    L(tail, tip);
    L(tip, tip + out * 3.0f + along * 2.2f);
    L(tip, tip + out * 3.0f - along * 2.2f);
}

} // namespace city
} // namespace tbd
