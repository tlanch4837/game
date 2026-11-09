// The road / rail tool's auto snap helpers (road_tool.h SnapAt uses them; the aim dot shows them). Header only, headless
// (tests/road_snap_test.cpp). Every kind is on by default; the kit's snap button off means free placement.
//   access     beside a placed building: the road runs along the building's ENTRANCE edge, its centre line the new
//              road's half width (carriageway + sidewalks: Profile::Half) out from that edge, so the sidewalk's outer edge
//              lies exactly on the footprint's edge. The line carries on past the edge as the road is dragged on.
//   angle      the piece's direction in angleStep steps (15 / 30 / 45 / 90) from the previous road, the previous piece or
//              the world axes; "parallel" / "perpendicular" to the nearest road.
//   length     the piece's length in whole lengthStep metres.
//   spacing    the same distance from the nearest road as that road's parallel neighbour (blocks line up).
//   extension  on the line that carries a dead end straight on.
// Guides are drawn dashed (DashedGuide) with the snap's label at the dot.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "placement_rotation.h"

namespace tbd {
namespace city {

// A placed building the road tool can line up with (the host fills RoadTool::lots: transport, utilities, ...).
struct BuildingLot {
    V2 c{}, front{0, 1};
    float W = 0, D = 0;  // across, back to front (metres)
    int entrance = 0;    // the door edge (placement_rotation.h EdgeMids order)
};

// The line a road along a lot's entrance edge follows: origin (opposite the edge's middle), dir (along the edge),
// out (from the building toward the road) and the edge's half length.
struct AccessLine {
    V2 o{}, dir{1, 0}, out{0, 1};
    float halfLen = 0;
};
inline AccessLine AccessOf(const BuildingLot& b, float roadHalf) {
    V2 mid[4], out[4];
    EdgeMids(b.c, b.front, b.W, b.D, mid, out);
    const int e = b.entrance & 3;
    AccessLine a;
    a.out = out[e];
    a.dir = {-a.out.z, a.out.x};
    a.o = mid[e] + a.out * roadHalf;
    a.halfLen = (e & 1) ? b.D * 0.5f : b.W * 0.5f;
    return a;
}
// The point p moved onto the line (and how far it moved, and where along it).
inline V2 OntoLine(V2 o, V2 dir, V2 p, float& off, float& along) {
    along = Dot2(p - o, dir);
    const V2 q = o + dir * along;
    off = Len(p - q);
    return q;
}

// The nearest multiple of step (> 0) to x; step <= 0 keeps x.
inline float RoundTo(float x, float step) { return step > 0 ? std::round(x / step) * step : x; }
// A length rounded to whole steps, never under one step.
inline float RoundLength(float L, float step) { return step > 0 ? std::max(step, RoundTo(L, step)) : L; }

// Angle snap: the ray from o toward p turned to the nearest stepDeg multiple from ref. True when p is within
// tolerance (metres at p) of that ray; q the snapped point (same length), relDeg the snapped angle from ref (0..180).
inline bool AngleSnap(V2 o, V2 p, V2 ref, float stepDeg, float tolM, V2& q, float& relDeg, V2& dir) {
    const V2 d = p - o;
    const float L = Len(d);
    if (L < 2.0f || stepDeg <= 0) return false;
    const float stepA = stepDeg * kDegToRad;
    float rel = Angle(d) - Angle(ref);
    while (rel > 3.14159265f) rel -= 6.2831853f;
    while (rel < -3.14159265f) rel += 6.2831853f;
    const float snapped = std::round(rel / stepA) * stepA;
    if (std::fabs(rel - snapped) * L > tolM) return false;
    const float a = Angle(ref) + snapped;
    dir = {std::cos(a), std::sin(a)};
    q = o + dir * L;
    relDeg = std::fabs(snapped) / kDegToRad;
    if (relDeg > 180.0f) relDeg = 360.0f - relDeg;
    return true;
}
// The angle tolerance (metres off the ray at the cursor): wider with the finger's reach and the length.
inline float AngleTolerance(float L, float snapRadius) { return std::max({2.5f, L * 0.035f, snapRadius * 0.35f}); }

// A guide's dashes: [a, b] cut into dash / gap pieces (pairs of points), so a guide reads as a dashed line on the map.
inline void DashedGuide(V2 a, V2 b, float dash, float gap, std::vector<std::pair<V2, V2>>& out) {
    const V2 d = b - a;
    const float L = Len(d);
    if (L < 1e-3f) return;
    const V2 u = d * (1.0f / L);
    for (float s = 0; s < L; s += dash + gap) out.push_back({a + u * s, a + u * std::min(L, s + dash)});
}

// Equal spacing: the distance between a road (point q on it, tangent T) and its nearest parallel neighbour on the other
// side from `side` (unit, the cursor's side), or 0. Segment tangents and points come from the caller (cheap: near only).
inline float ParallelSpacing(V2 q, V2 T, V2 side, const std::vector<std::pair<V2, V2>>& others, float maxD = 200.0f) {
    float best = 0;
    for (const auto& o : others) { // (a point on the other road nearest q, its tangent)
        if (std::fabs(Cross2(Norm(o.second), T)) > 0.05f) continue; // not parallel (3 deg)
        const V2 r = o.first - q;
        const float across = Dot2(r, side);
        if (across > -10.0f) continue; // must be on the far side
        const float d = std::fabs(across);
        if (d <= maxD && (best == 0 || d < best)) best = d;
    }
    return best;
}

} // namespace city
} // namespace tbd
