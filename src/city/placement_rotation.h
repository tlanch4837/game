// Rotating a placed building's footprint (the utility, transport / service and industry / waste placement tools).
// Header only, headless (tests\placement_rotation_test.cpp).
//
// A footprint is W metres across and D metres from its back to its front, centred on c, its front facing `front` (a unit
// xz vector). Every tool keeps a rotation in degrees with the same sense as its Plan(): front = (sin a, cos a).
//   Free placement     the rotation is the building's absolute heading.
//   Snapped to a road  the building faces the road (rotation 0) or is turned by whole quarter turns from that (90: its side
//                      to the road, 180: its back), and the turned footprint is pulled in so its road-side edge sits `gap`
//                      metres off the kerb, however deep or wide the lot is (RoadSnap).
// Road access works from whichever edge of the turned footprint lies along a road (EdgeMids; utilities.h UtilRoadAccess),
// so a turned building is not flagged "no road" just because its entrance side is away from the street.
#pragma once
#include <cmath>

#include "road_network.h"

namespace tbd {
namespace city {

constexpr float kDegToRad = 0.01745329252f;

// The heading of a rotation in degrees.
inline V2 FrontFromDeg(float deg) { return {std::sin(deg * kDegToRad), std::cos(deg * kDegToRad)}; }
// A heading's rotation in degrees, [0, 360).
inline float DegOfFront(V2 f) {
    float d = std::atan2(f.x, f.z) / kDegToRad;
    while (d < 0.0f) d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d;
}
// A heading turned by deg, in the tools' sense.
inline V2 TurnFront(V2 f, float deg) { return FrontFromDeg(DegOfFront(f) + deg); }
// The nearest whole quarter turn of a rotation: 0..3.
inline int QuarterTurns(float deg) {
    int q = (int)std::lround(deg / 90.0f) % 4;
    return q < 0 ? q + 4 : q;
}
// One rotate step of a tool: rotation + step, kept in [0, 360).
inline float StepRotation(float rotation, float step) {
    float r = std::fmod(rotation + step, 360.0f);
    if (r < 0.0f) r += 360.0f;
    if (r > 359.95f) r = 0.0f;
    return r;
}

// Half the extent of a W x D footprint facing `front`, measured along the unit direction u.
inline float HalfExtentAlong(V2 front, float W, float D, V2 u) {
    const V2 side{front.z, -front.x};
    return 0.5f * (W * std::fabs(Dot2(side, u)) + D * std::fabs(Dot2(front, u)));
}

// A footprint beside a road: P the road's centre-line point, `away` the unit direction from the road toward the lot,
// roadHalf the road's half width. quarter 0 faces the road; 1..3 turn it by 90, 180, 270 degrees in the tools' sense.
struct SnapResult { V2 c, front; };
inline SnapResult RoadSnap(V2 P, V2 away, float roadHalf, float W, float D, int quarter, float gap = 1.0f) {
    SnapResult r;
    const V2 faceRoad = away * -1.0f;
    r.front = quarter % 4 == 0 ? faceRoad : TurnFront(faceRoad, 90.0f * (float)(quarter % 4));
    r.c = P + away * (roadHalf + gap + HalfExtentAlong(r.front, W, D, away));
    return r;
}

// The four edge midpoints of a footprint and their outward normals: 0 front, 1 the side (front.z, -front.x), 2 back,
// 3 the other side.
inline void EdgeMids(V2 c, V2 front, float W, float D, V2 (&mid)[4], V2 (&out)[4]) {
    const V2 side{front.z, -front.x};
    out[0] = front, out[1] = side, out[2] = front * -1.0f, out[3] = side * -1.0f;
    mid[0] = c + front * (D * 0.5f), mid[1] = c + side * (W * 0.5f), mid[2] = c - front * (D * 0.5f), mid[3] = c - side * (W * 0.5f);
}

// The four corners, counter-clockwise from back-left in the footprint's frame.
inline void Corners(V2 c, V2 front, float W, float D, V2 (&q)[4]) {
    const V2 side{front.z, -front.x};
    const V2 sx = side * (W * 0.5f), fz = front * (D * 0.5f);
    q[0] = c - sx - fz, q[1] = c + sx - fz, q[2] = c + sx + fz, q[3] = c - sx + fz;
}

} // namespace city
} // namespace tbd
