// Procedural models of the rail junctions (rail_junction.h): turnout left / right, wye, 3-way, diamond, single slip,
// crossover, scissors crossover and the level crossing, all from parameters (gauge, angle, radius, lead and tail
// lengths, track spacing, road width). Header only, headless; docs/rail_junction_preview.html is a line-for-line port.
//
//   Frame      local x along the stem / main line (+x forward), z across (Right = +z, Left = -z, as RightOf), y up from
//              the track base (the host puts the base kRoadLift + 0.1 above the node, where the SPOM track's box sits).
//   Parts      ballast bed (top + two shoulder slopes per span), ties (one top quad each, all in one strip: one draw;
//              long timbers under the switch), rails (top + two sides per span, no bottoms), switch blades and closure
//              rails (the routes' own rails), frogs (a 10-tri block where two routes' rails cross), check rails (a short
//              rail inside the opposite rail at every frog), point machines (10-tri box), and for level crossings the
//              deck, stop lines, RR marks, two gate masts (crossed 2-tri cards: crossbuck + lights baked) with booms.
//   LOD        lod 1 (distance): bed and rails top only, coarser curves, no ties / frogs / check rails / machines,
//              one card per mast. The ballast texture carries the ties there.
//   Materials  Ballast (world UV / 4 m), Tie, Steel, Prop (a 4-cell atlas in u: machine, mast card, boom, deck), Paint.
//              Same texture set for every variant: one mesh per variant, at most five draws (three without props).
#pragma once
#include <array>
#include <cmath>
#include <vector>

#include "rail_junction.h"

namespace tbd {
namespace city {

// Spom: the plane path (rail_junction_plane.h), one SPOM box per junction drawn like the plain track (TrackPS).
// Road: the crossing's ramps, with the road network's own asphalt (uv = metres / 4, as Mat::Asphalt) so they carry the
// road on without a seam; in the game they join the road mesh's asphalt draw, as Paint joins its markings.
enum class JMat : int { Ballast, Tie, Steel, Prop, Paint, Spom, Road, Count };

struct JunctionParams {
    JType type = JType::Turnout;
    Hand hand = Hand::Right;
    float angleDeg = 12.0f;  // turnout / wye (between the legs) / 3-way / crossover diverging angle; diamond / slip / level crossing: crossing angle
    float radius = 150.0f;   // diverging curve radius (slip curve too)
    float gauge = kRailGauge;
    float stem = 4.0f;       // straight track before the points
    float tail = 6.0f;       // straight track after the curve
    float spacing = 5.0f;    // crossover / scissors: track centres
    float roadWidth = 9.0f;  // level crossing: carriageway width
    float sidewalk = 2.0f;   // level crossing: sidewalk each side (the deck covers it too)
    float gate = 0.0f;       // level crossing: 0 gates down (a train near), 1 up (open)
    bool spom = false;       // ballast, ties and the crossing deck baked into one SPOM pad (rail_junction_spom.h)
    int lod = 0;
};

struct JunctionMesh {
    std::array<std::vector<Vertex>, (int)JMat::Count> byMat;
    float angleDeg = 0, radius = 0; // what was built (after adjusting)
    bool adjusted = false;
    std::string note;
    size_t Tris() const {
        size_t n = 0;
        for (const auto& v : byMat) n += v.size() / 3;
        return n;
    }
    size_t TrisOf(JMat m) const { return byMat[(int)m].size() / 3; }
    int Draws() const {
        int d = 0;
        for (const auto& v : byMat) d += !v.empty();
        return d;
    }
};

namespace jmesh {
// heights above the track base
constexpr float kBedTop = 0.25f, kBedTopHalf = 1.6f, kBedFootHalf = 2.2f;
constexpr float kTieTop = 0.33f, kTieHalfLen = 1.3f, kTieWidth = 0.25f, kTieStep = 0.60f; // = the plain track's sleepers (tools/vehicle_gen/gen_track.py)
constexpr float kRailTop = 0.50f, kRailHead = 0.075f;
constexpr float kLongTie = 2.4f;   // a route's centre this close to an earlier route's: the earlier route's long timbers carry it
constexpr float kCheckGap = 0.11f; // check rail inside the running rail
// atlas cells (u ranges) of the Prop texture
constexpr float kCellMachine = 0.0f, kCellMast = 0.25f, kCellBoom = 0.5f, kCellDeck = 0.75f;

struct RPt {
    V2 c, f; // centre, forward (unit)
    float s; // metres along
};
using Route = std::vector<RPt>;

inline Vec3 P3(V2 p, float y) { return {p.x, y, p.z}; }
inline const Vec3 kBallastCol{0.47f, 0.45f, 0.42f}, kTieCol{0.36f, 0.30f, 0.25f}, kSteelCol{0.72f, 0.73f, 0.76f},
    kRailSideCol{0.42f, 0.33f, 0.27f}, kPropCol{1, 1, 1}, kPaintCol{0.93f, 0.93f, 0.90f};

// mesh::Quad, but also wound counter-clockwise seen from the normal's side (hint), so back-face culling keeps it.
inline void Q(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 col, const float uv[8], Vec3 hint = {0, 1, 0}) {
    if (Dot(Cross(b - a, d - a), hint) < 0) {
        const float r[8] = {uv[0], uv[1], uv[6], uv[7], uv[4], uv[5], uv[2], uv[3]};
        mesh::Quad(o, a, d, c, b, col, r, hint);
        return;
    }
    mesh::Quad(o, a, b, c, d, col, uv, hint);
}

// ---------------------------------------------------------------------------------------------------- routes
inline void Straight(Route& r, V2 a, V2 f, float len) {
    const float s0 = r.empty() ? 0.0f : r.back().s;
    if (r.empty()) r.push_back({a, f, 0.0f});
    r.push_back({a + f * len, f, s0 + len});
}
// A circular arc from the route's end (or a, f): radius R, turning angle (deg) to `side` (+1 right / -1 left).
inline void Arc(Route& r, V2 a, V2 f, float R, float angleDeg, float side, int lod) {
    if (r.empty()) r.push_back({a, f, 0.0f});
    a = r.back().c, f = r.back().f;
    const float s0 = r.back().s, ang = Rad(angleDeg), step = lod == 2 ? 0.5f : lod ? 6.0f : 3.0f, maxLen = lod == 2 ? 1.0f : lod ? 24.0f : 12.0f; // lod 2: fine, for baking; a 3 deg chord sags 5 cm at R 150
    const int n = std::max(1, std::max((int)std::ceil(angleDeg / step), (int)std::ceil(R * ang / maxLen)));
    const V2 nrm = RightOf(f) * side;
    for (int i = 1; i <= n; ++i) {
        const float t = ang * i / n;
        r.push_back({a + f * (R * std::sin(t)) + nrm * (R * (1 - std::cos(t))), Norm(f * std::cos(t) + nrm * std::sin(t)), s0 + R * t});
    }
}
// Pitch-snapped length: a whole number of sleeper pitches, so a junction's ends meet plain track in sleeper phase.
inline float Pitched(float len) { return std::ceil(len / kTieStep - 1e-3f) * kTieStep; }
// Straight on to at least len more, the route's total length snapped to whole sleeper pitches.
inline void Tail(Route& r, float len) {
    const float add = Pitched(r.back().s + std::max(0.0f, len)) - r.back().s;
    if (add > 0.01f) Straight(r, r.back().c, r.back().f, add);
}
inline V2 Offset(const RPt& p, float d) { return p.c + RightOf(p.f) * d; }

// Distance from q to a route's centre line, and the route's forward there.
inline float DistTo(const Route& r, V2 q, V2* fOut = nullptr) {
    float best = 1e30f;
    for (size_t i = 1; i < r.size(); ++i) {
        const V2 a = r[i - 1].c, b = r[i].c, ab = b - a;
        const float L2 = Dot2(ab, ab), tu = L2 > 1e-9f ? Dot2(q - a, ab) / L2 : 0.0f;
        if ((i == 1 && tu < 0) || (i + 1 == r.size() && tu > 1)) continue; // beyond the route's ends: not beside it
        const float t = std::clamp(tu, 0.0f, 1.0f), d = Len(a + ab * t - q);
        if (d < best) {
            best = d;
            if (fOut) *fOut = Norm(ab);
        }
    }
    return best;
}
// Where the line q + n*u crosses the route's centre line (|u| <= lim): u out, or false.
inline bool CrossAt(const Route& r, V2 q, V2 n, float lim, float& u) {
    float best = 1e30f;
    for (size_t i = 1; i < r.size(); ++i) {
        const V2 a = r[i - 1].c, e = r[i].c - a;
        const float den = Cross2(n, e);
        if (std::fabs(den) < 1e-6f) continue;
        const V2 w = a - q;
        const float uu = Cross2(w, e) / den, v = Cross2(w, n) / den;
        if (v < 0 || v > 1 || std::fabs(uu) > lim) continue;
        if (std::fabs(uu) < std::fabs(best)) best = uu;
    }
    if (best > 1e29f) return false;
    u = best;
    return true;
}

// ---------------------------------------------------------------------------------------------------- parts
// Ballast bed under a route: top, and at lod 0 a shoulder slope each side (world UV, 4 m tile).
inline void Bed(JunctionMesh& m, const Route& r, int lod) {
    auto& o = m.byMat[(int)JMat::Ballast];
    for (size_t i = 1; i < r.size(); ++i) {
        const RPt &a = r[i - 1], &b = r[i];
        auto uvq = [](Vec3 p, Vec3 q, Vec3 s, Vec3 t, float* uv) {
            const Vec3 v[4] = {p, q, s, t};
            for (int k = 0; k < 4; ++k) uv[k * 2] = v[k].x / 4.0f, uv[k * 2 + 1] = v[k].z / 4.0f;
        };
        float uv[8];
        const Vec3 la = P3(Offset(a, -kBedTopHalf), kBedTop), ra = P3(Offset(a, kBedTopHalf), kBedTop);
        const Vec3 lb = P3(Offset(b, -kBedTopHalf), kBedTop), rb = P3(Offset(b, kBedTopHalf), kBedTop);
        uvq(la, ra, rb, lb, uv), Q(o, la, ra, rb, lb, kBallastCol, uv);
        if (lod) continue;
        const Vec3 lfa = P3(Offset(a, -kBedFootHalf), 0), lfb = P3(Offset(b, -kBedFootHalf), 0);
        const Vec3 rfa = P3(Offset(a, kBedFootHalf), 0), rfb = P3(Offset(b, kBedFootHalf), 0);
        uvq(lfa, la, lb, lfb, uv), Q(o, lfa, la, lb, lfb, kBallastCol, uv);
        uvq(ra, rfa, rfb, rb, uv), Q(o, ra, rfa, rfb, rb, kBallastCol, uv);
    }
}
// One rail along a polyline (centre of the head): top, and at lod 0 both sides down to the tie top.
inline void RailStrip(JunctionMesh& m, const std::vector<V2>& pts, const std::vector<V2>& fs, int lod, float top = kRailTop) {
    auto& o = m.byMat[(int)JMat::Steel];
    float s = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        const float l = Len(pts[i] - pts[i - 1]), u0 = s / 2.0f, u1 = (s + l) / 2.0f;
        s += l;
        const V2 na = RightOf(fs[i - 1]) * kRailHead * 0.5f, nb = RightOf(fs[i]) * kRailHead * 0.5f;
        const Vec3 la = P3(pts[i - 1] - na, top), ra = P3(pts[i - 1] + na, top), lb = P3(pts[i] - nb, top), rb = P3(pts[i] + nb, top);
        const float uvT[8] = {u0, 0, u0, 1, u1, 1, u1, 0};
        Q(o, la, ra, rb, lb, kSteelCol, uvT);
        if (lod) continue;
        const float uvS[8] = {u0, 0, u1, 0, u1, 1, u0, 1};
        const Vec3 la0 = P3(pts[i - 1] - na, kTieTop), lb0 = P3(pts[i] - nb, kTieTop), ra0 = P3(pts[i - 1] + na, kTieTop), rb0 = P3(pts[i] + nb, kTieTop);
        const Vec3 outL = P3(LeftOf(fs[i - 1]), 0), outR = P3(RightOf(fs[i - 1]), 0);
        Q(o, la, lb, lb0, la0, kRailSideCol, uvS, outL);
        Q(o, rb, ra, ra0, rb0, kRailSideCol, uvS, outR);
    }
}
// Both rails of a route (head centres at +-(gauge/2 + head/2)).
inline void Rails(JunctionMesh& m, const Route& r, float gauge, int lod) {
    for (float side : {-1.0f, 1.0f}) {
        std::vector<V2> pts, fs;
        for (const RPt& p : r) pts.push_back(Offset(p, side * (gauge + kRailHead) * 0.5f)), fs.push_back(p.f);
        RailStrip(m, pts, fs, lod);
    }
}
// A box (no bottom): centre c, forward f, length l, width w, height h above y0. Prop atlas cell u0.
inline void Box(std::vector<Vertex>& o, V2 c, V2 f, float l, float w, float y0, float h, Vec3 col, float u0) {
    const V2 x = f * (l * 0.5f), z = RightOf(f) * (w * 0.5f);
    const V2 q[4] = {c - x - z, c + x - z, c + x + z, c - x + z};
    const float y1 = y0 + h, uvT[8] = {u0, 0, u0 + 0.25f, 0, u0 + 0.25f, 0.5f, u0, 0.5f};
    Q(o, P3(q[0], y1), P3(q[1], y1), P3(q[2], y1), P3(q[3], y1), col, uvT);
    for (int k = 0; k < 4; ++k) {
        const V2 a = q[k], b = q[(k + 1) % 4], mid = (a + b) * 0.5f - c;
        const float uvS[8] = {u0, 0.5f, u0 + 0.25f, 0.5f, u0 + 0.25f, 1, u0, 1};
        Q(o, P3(a, y1), P3(b, y1), P3(b, y0), P3(a, y0), col, uvS, P3(mid, 0));
    }
}
// A vertical 2-tri card from a to b (xz), y0..y1, showing atlas cell u0 (u0..u0+0.25 across, 0..1 up).
inline void Card(std::vector<Vertex>& o, V2 a, V2 b, float y0, float y1, float u0) {
    const float uv[8] = {u0, 0, u0 + 0.25f, 0, u0 + 0.25f, 1, u0, 1};
    Q(o, P3(a, y1), P3(b, y1), P3(b, y0), P3(a, y0), kPropCol, uv, P3(RightOf(Norm(b - a)), 0));
}

// Centre and forward of a route at s metres along.
inline void RouteAt(const Route& r, float s, V2& c, V2& f) {
    size_t i = 1;
    while (i + 1 < r.size() && r[i].s < s) ++i;
    const RPt &a = r[i - 1], &b = r[i];
    const float t = b.s > a.s ? std::clamp((s - a.s) / (b.s - a.s), 0.0f, 1.0f) : 0.0f;
    c = Lerp2(a.c, b.c, t), f = Norm(Lerp2(a.f, b.f, t));
}
// A run of plain ties as one strip along the route (s0..s1): the Tie texture repeats a tie every kTieStep in v
// (tie in the middle 40 %, ballast between), so plain track costs 2 tris per curve span, not 2 per tie.
inline void TieRun(std::vector<Vertex>& o, const Route& r, float s0, float s1, float half) {
    std::vector<float> cut = {s0};
    for (const RPt& p : r)
        if (p.s > s0 + 0.01f && p.s < s1 - 0.01f) cut.push_back(p.s);
    cut.push_back(s1);
    for (size_t i = 1; i < cut.size(); ++i) {
        V2 ca, fa, cb, fb;
        RouteAt(r, cut[i - 1], ca, fa), RouteAt(r, cut[i], cb, fb);
        const float va = cut[i - 1] / kTieStep, vb = cut[i] / kTieStep, uv[8] = {0, va, 1, va, 1, vb, 0, vb};
        Q(o, P3(ca - RightOf(fa) * half, kTieTop), P3(ca + RightOf(fa) * half, kTieTop), P3(cb + RightOf(fb) * half, kTieTop),
          P3(cb - RightOf(fb) * half, kTieTop), kTieCol, uv);
    }
}

// One tie position (every kTieStep along a route, the first half a step in): what lies there. Shared by the tie mesh
// (Ties) and the SPOM layout baker (tools/rail_junction_tex), so both put sleepers and timbers in the same places.
enum class TieState : int { None, Plain, Long, Covered, Skipped };
struct TieStation {
    float s = 0;           // metres along the route (the tie's centre)
    V2 c{}, f{1, 0};       // centre, forward
    TieState state = TieState::None;
    float lo = 0, hi = 0;  // Plain / Long: the tie from c + n*lo to c + n*hi (n = RightOf(f))
};
// Under a switch an earlier route's tie is lengthened (Long) to carry the rails of any later route whose centre crosses
// it within kLongTie, and the later route's own ties there are Covered. virt[k]: route k has no rails of its own (a
// wye's centre line), only timbers under the routes crossing it. skip(q): no tie (a level crossing's deck).
template <class Skip>
inline std::vector<std::vector<TieStation>> TieStations(const std::vector<Route>& routes, const std::vector<char>& virt, float gauge, Skip&& skip) {
    const float halfStd = gauge * 0.5f + (kTieHalfLen - kRailGauge * 0.5f);
    std::vector<std::vector<TieStation>> out(routes.size());
    for (size_t k = 0; k < routes.size(); ++k) {
        const Route& r = routes[k];
        const float L = r.back().s;
        for (float s = kTieStep * 0.5f; s < L; s += kTieStep) {
            TieStation t;
            t.s = s;
            RouteAt(r, s, t.c, t.f);
            const V2 n = RightOf(t.f);
            if (skip(t.c)) { t.state = TieState::Skipped, out[k].push_back(t); continue; }
            bool covered = false;
            for (size_t j = 0; j < k && !covered; ++j) {
                V2 fj;
                const float d = DistTo(routes[j], t.c, &fj);
                covered = d < (std::fabs(Dot2(fj, t.f)) < 0.5f ? halfStd : kLongTie);
            }
            if (covered) { t.state = TieState::Covered, out[k].push_back(t); continue; }
            float lo = virt[k] ? 1e9f : -halfStd, hi = virt[k] ? -1e9f : halfStd;
            bool longTie = false;
            for (size_t j = k + 1; j < routes.size(); ++j) {
                float u;
                if (!virt[j] && CrossAt(routes[j], t.c, n, kLongTie, u)) lo = std::min(lo, u - halfStd), hi = std::max(hi, u + halfStd), longTie = true;
            }
            t.lo = lo, t.hi = hi;
            t.state = hi < lo ? TieState::None : longTie ? TieState::Long : TieState::Plain;
            out[k].push_back(t);
        }
    }
    return out;
}

// Ties of every route, all in one vertex list (a single draw): each Long timber its own quad, Plain runs as TieRun
// strips (TieStations says which).
template <class Skip>
inline void Ties(JunctionMesh& m, const std::vector<Route>& routes, const std::vector<char>& virt, float gauge, Skip&& skip) {
    auto& o = m.byMat[(int)JMat::Tie];
    const float halfStd = gauge * 0.5f + (kTieHalfLen - kRailGauge * 0.5f);
    const auto st = TieStations(routes, virt, gauge, skip);
    for (size_t k = 0; k < routes.size(); ++k) {
        const Route& r = routes[k];
        const float L = r.back().s;
        float runA = -1, runB = -1; // the open run of plain ties
        auto flush = [&] {
            if (runA >= 0) TieRun(o, r, runA, runB, halfStd);
            runA = -1;
        };
        for (const TieStation& t : st[k]) {
            if (t.state == TieState::Plain) { // extend the run
                if (runA < 0) runA = std::max(0.0f, t.s - kTieStep * 0.5f);
                runB = std::min(L, t.s + kTieStep * 0.5f);
                continue;
            }
            flush();
            if (t.state != TieState::Long) continue;
            const V2 n = RightOf(t.f), w = t.f * (kTieWidth * 0.5f);
            const Vec3 p0 = P3(t.c + n * t.lo - w, kTieTop), p1 = P3(t.c + n * t.hi - w, kTieTop), p2 = P3(t.c + n * t.hi + w, kTieTop), p3 = P3(t.c + n * t.lo + w, kTieTop);
            const float ul = (t.hi - t.lo) / (2 * halfStd), v0 = 0.3f, v1 = 0.7f, uv[8] = {0, v0, ul, v0, ul, v1, 0, v1};
            Q(o, p0, p1, p2, p3, kTieCol, uv);
        }
        flush();
    }
}

// Rails of a route as polylines: side -1 (left) / +1 (right).
inline void RailLine(const Route& r, float gauge, float side, std::vector<V2>& pts, std::vector<V2>& fs) {
    pts.clear(), fs.clear();
    for (const RPt& p : r) pts.push_back(Offset(p, side * (gauge + kRailHead) * 0.5f)), fs.push_back(p.f);
}
// Where rails of two routes cross (not where they start together at the points): a frog each, and a check rail inside
// the opposite running rail of each route there (deduplicated). Shared by the mesh (Frogs) and the SPOM baker.
struct FrogPlan {
    struct Frog { V2 p, dir; };   // centre, along the bisector of the two rails
    struct Check { V2 c, f; };    // check rail centre (already inside the running rail) and direction, 3.2 m long
    std::vector<Frog> frogs;
    std::vector<Check> checks;
};
inline FrogPlan PlanFrogs(const std::vector<Route>& routes, const std::vector<char>& virt, float gauge) {
    FrogPlan F;
    std::vector<V2> pa, fa, pb, fb;
    std::vector<std::pair<int, V2>> seen; // (route * 2 + side, where)
    for (size_t i = 0; i < routes.size(); ++i)
        for (size_t j = i + 1; j < routes.size(); ++j) {
            if (virt[i] || virt[j]) continue;
            for (float si : {-1.0f, 1.0f})
                for (float sj : {-1.0f, 1.0f}) {
                    RailLine(routes[i], gauge, si, pa, fa), RailLine(routes[j], gauge, sj, pb, fb);
                    for (size_t x = 1; x < pa.size(); ++x)
                        for (size_t y = 1; y < pb.size(); ++y) {
                            const V2 r = pa[x] - pa[x - 1], q = pb[y] - pb[y - 1], w = pb[y - 1] - pa[x - 1];
                            const float den = Cross2(r, q);
                            if (std::fabs(den) < 1e-6f) continue;
                            const float u = Cross2(w, q) / den, v = Cross2(w, r) / den;
                            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                            const V2 P = pa[x - 1] + r * u;
                            if (Len(P - pa[0]) < 1.0f || Len(P - pb[0]) < 1.0f) continue; // the points, not a frog
                            F.frogs.push_back({P, Norm(Norm(r) + Norm(q))});
                            for (int k = 0; k < 2; ++k) {
                                const int ri = k ? (int)j : (int)i;
                                const float side = -(k ? sj : si);
                                V2 f;
                                DistTo(routes[ri], P, &f);
                                const V2 c = P + RightOf(f) * (side * (gauge + kRailHead)); // the opposite rail, level with the frog
                                bool dup = false;
                                for (const auto& ck : seen) dup = dup || (ck.first == ri * 2 + (side > 0) && Len(ck.second - c) < 1.5f);
                                if (dup) continue;
                                seen.push_back({ri * 2 + (side > 0), c});
                                F.checks.push_back({c + RightOf(f) * (-side * kCheckGap), f});
                            }
                        }
                }
        }
    return F;
}
inline int Frogs(JunctionMesh& m, const std::vector<Route>& routes, const std::vector<char>& virt, float gauge) {
    const FrogPlan F = PlanFrogs(routes, virt, gauge);
    for (const auto& f : F.frogs) Box(m.byMat[(int)JMat::Steel], f.p, f.dir, 1.4f, 0.22f, kTieTop, kRailTop - kTieTop, kSteelCol, 0.0f);
    for (const auto& c : F.checks) RailStrip(m, {c.c - c.f * 1.6f, c.c + c.f * 1.6f}, {c.f, c.f}, 0, kRailTop - 0.02f);
    return (int)F.frogs.size();
}

// A point machine beside the points at p (forward f), on `side` of the track.
inline void PointMachine(JunctionMesh& m, V2 p, V2 f, float side) {
    Box(m.byMat[(int)JMat::Prop], p + f * 0.9f + RightOf(f) * (side * 1.9f), f, 1.0f, 0.45f, 0.12f, 0.4f, {0.55f, 0.57f, 0.52f}, kCellMachine);
}

// ---------------------------------------------------------------------------------------------------- level crossing
// American style. The rail runs along x through the origin, the road along r = (cos a, sin a): a carriageway roadWidth
// wide and a sidewalk each side. The road surface lies kRoadY under the track base (road_network.h: a road at node +
// kRoadLift, the track base at node + kRoadLift + 0.1), so the road climbs a kRampLen ramp on each side to a deck flush
// with the rail head, across the whole road and sidewalks, with open flangeways inside the rails.
constexpr float kRoadY = -0.10f, kRampLen = 6.0f, kDeckHalf = 2.4f, kFlange = 0.075f;
struct CrossingFrame {
    V2 r, nr;                 // road forward, across the road
    float half, outer, sinA;  // half carriageway, half road incl. sidewalks, |sin| of the crossing angle
    // The point on the road line `e` across (along nr) where the track offset (z) is z.
    V2 At(float e, float z) const { return {(e - z * nr.z) / nr.x, z}; }
};
inline CrossingFrame CrossFrame(const JunctionParams& p) {
    CrossingFrame F;
    F.r = {std::cos(Rad(p.angleDeg)), std::sin(Rad(p.angleDeg))}, F.nr = RightOf(F.r);
    F.half = p.roadWidth * 0.5f, F.outer = F.half + p.sidewalk, F.sinA = std::max(0.2f, std::fabs(F.r.z));
    return F;
}
inline float RampZ(const CrossingFrame& F) { return kDeckHalf + kRampLen * F.sinA; }
// A triangle wound counter-clockwise seen from hint's side, all three corners at one atlas point (u, v).
inline void T(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 col, float u, float v, Vec3 hint) {
    Vec3 n = Cross(b - a, c - a);
    if (Dot(n, hint) < 0) std::swap(b, c), n = n * -1.0f;
    mesh::Tri(o, a, b, c, Normalize(n), col, u, v, u, v, u, v);
}
// The deck (Prop atlas deck cell, top half): lod 0 three panels leaving the flangeways open, lod 1 one slab.
inline void CrossingDeck(std::vector<Vertex>& o, const JunctionParams& p) {
    const CrossingFrame F = CrossFrame(p);
    const float rin = p.gauge * 0.5f - kFlange, rout = p.gauge * 0.5f + kRailHead;
    std::vector<std::pair<float, float>> bands;
    if (p.lod) bands = {{-kDeckHalf, kDeckHalf}};
    else bands = {{-kDeckHalf, -rout}, {-rin, rin}, {rout, kDeckHalf}};
    auto U = [&](float e) { return kCellDeck + 0.25f * (e + F.outer) / (2 * F.outer); };
    auto Vv = [&](float z) { return 0.5f * (z + kDeckHalf) / (2 * kDeckHalf); };
    for (const auto& b : bands) {
        const float uv[8] = {U(-F.outer), Vv(b.first), U(F.outer), Vv(b.first), U(F.outer), Vv(b.second), U(-F.outer), Vv(b.second)};
        Q(o, P3(F.At(-F.outer, b.first), kRailTop), P3(F.At(F.outer, b.first), kRailTop), P3(F.At(F.outer, b.second), kRailTop),
          P3(F.At(-F.outer, b.second), kRailTop), {0.34f, 0.34f, 0.35f}, uv);
    }
}
// Ramps (asphalt: the deck cell's lower half) from the deck edge down to the road, and at lod 0 the sides of the
// raised section (deck skirts and ramp wedges) along both outer edges.
inline void CrossingRamps(std::vector<Vertex>& o, const JunctionParams& p) { // o: the Road list
    const CrossingFrame F = CrossFrame(p);
    const float zr = RampZ(F), lo = kRoadY + 0.01f, ua = kCellDeck + 0.125f;
    const Vec3 asphalt{1, 1, 1}, up{0, 1, 0};
    for (float s : {-1.0f, 1.0f}) {
        const float z0 = s * kDeckHalf, z1 = s * zr;
        const V2 q[4] = {F.At(-F.outer, z0), F.At(F.outer, z0), F.At(F.outer, z1), F.At(-F.outer, z1)};
        const float uv[8] = {q[0].x / 4, q[0].z / 4, q[1].x / 4, q[1].z / 4, q[2].x / 4, q[2].z / 4, q[3].x / 4, q[3].z / 4};
        Q(o, P3(q[0], kRailTop), P3(q[1], kRailTop), P3(q[2], lo), P3(q[3], lo), asphalt, uv, up);
    }
    if (p.lod) return;
    for (float e : {-F.outer, F.outer}) {
        const Vec3 out = P3(F.nr * (e > 0 ? 1.0f : -1.0f), 0);
        const float uv[8] = {ua, 0.75f, ua, 0.75f, ua, 0.75f, ua, 0.75f};
        Q(o, P3(F.At(e, -kDeckHalf), kRailTop), P3(F.At(e, kDeckHalf), kRailTop), P3(F.At(e, kDeckHalf), lo), P3(F.At(e, -kDeckHalf), lo), asphalt, uv, out);
        for (float s : {-1.0f, 1.0f})
            T(o, P3(F.At(e, s * kDeckHalf), kRailTop), P3(F.At(e, s * zr), lo), P3(F.At(e, s * kDeckHalf), lo), asphalt, ua, 0.75f, out);
    }
}
// A vertical card in the plane of `across` and up, centred at c: from a to b along across (metres), y0..y1, atlas uv box.
inline void CardBox(std::vector<Vertex>& o, V2 c, V2 across, float a, float b, float y0, float y1, float u0, float u1, float v0, float v1, V2 facing) {
    const float uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    Q(o, P3(c + across * a, y1), P3(c + across * b, y1), P3(c + across * b, y0), P3(c + across * a, y0), kPropCol, uv, P3(facing, 0));
}
// Lanes per direction on a carriageway (3.6 m lanes, 1..3).
inline int CrossingLanes(float roadWidth) { return std::clamp((int)std::lround(roadWidth * 0.5f / 3.6f), 1, 3); }
// A gate boom: a striped card in the road's cross plane at s along d, pivot `l0` across (right), pointing `dir` (-1:
// towards the centre line, +1 outwards), length L. gate 0 = down across the lanes, 1 = up (88 deg).
inline void Boom(std::vector<Vertex>& o, V2 d, V2 right, float s, float l0, float dir, float L, float gate) {
    const float ph = Rad(88.0f) * std::clamp(gate, 0.0f, 1.0f), y0 = kRoadY + 1.0f, t = 0.09f;
    const float Dl = dir * std::cos(ph), Dy = std::sin(ph), Pl = -Dy * dir, Py = std::cos(ph); // along the arm, across it
    auto at = [&](float l, float y) { return Vec3{(right * l + d * s).x, y, (right * l + d * s).z}; };
    const float uv[8] = {kCellBoom, 0, kCellBoom + 0.25f, 0, kCellBoom + 0.25f, 1, kCellBoom, 1};
    Q(o, at(l0 + Pl * t, y0 + Py * t), at(l0 + Dl * L + Pl * t, y0 + Dy * L + Py * t), at(l0 + Dl * L - Pl * t, y0 + Dy * L - Py * t),
      at(l0 - Pl * t, y0 - Py * t), kPropCol, uv, P3(d * -1.0f, 0));
}
// A signal mast at P: the face card (crossbuck over two flashing lights, baked) square to the road, facing the traffic
// coming along d; at lod 0 a narrow card along d so the pole reads from the side too.
inline void Mast(std::vector<Vertex>& o, V2 P, V2 d, V2 right, int lod) {
    CardBox(o, P, right, -0.5f, 0.5f, kRoadY, kRoadY + 4.4f, kCellMast, kCellMast + 0.25f, 0, 1, d * -1.0f);
    if (!lod) CardBox(o, P, d, -0.08f, 0.08f, kRoadY, kRoadY + 4.4f, kCellMast + 0.25f * 0.46f, kCellMast + 0.25f * 0.54f, 0, 1, right);
}

// Everything of a level crossing but the track: deck (unless baked into the SPOM pad), ramps, and per approach (the
// right-hand half of the road, traffic along d): an RXR mark in each lane, a stop line square to the road, the gate
// mast on the verge beyond the sidewalk with its boom across the approach lanes to the centre line (a second gate on
// the centre line when a half is wider than 7.5 m), a cantilever with a light over each lane when there are two or
// more lanes; and a relay cabinet beside the track. Everything square to the road, set back from the deck edge
// wherever it is nearest (skewed crossings).
inline void LevelCrossingProps(JunctionMesh& m, const JunctionParams& p) {
    auto& prop = m.byMat[(int)JMat::Prop];
    auto& paint = m.byMat[(int)JMat::Paint];
    const CrossingFrame F = CrossFrame(p);
    if (!p.spom) CrossingDeck(prop, p);
    CrossingRamps(m.byMat[(int)JMat::Road], p);
    const int lanes = CrossingLanes(p.roadWidth);
    const float lw = F.half / lanes, lg = F.outer + 0.6f, zr = RampZ(F), yP = kRoadY + 0.02f;
    for (float sg : {1.0f, -1.0f}) {
        const V2 d = F.r * sg, right = RightOf(d);
        const float sz = d.z > 0 ? 1.0f : -1.0f;
        auto sAt = [&](float l, float z) { return (-sz * z - (right * l).z) / d.z; }; // s (along d) where lateral l meets track offset z
        auto Pt = [&](float l, float s) { return right * l + d * s; };
        const float sGate = std::min(sAt(0, kDeckHalf), sAt(lg, kDeckHalf)) - 1.0f;
        const float sStop = std::min(std::min(sAt(0, zr), sAt(F.outer, zr)) - 1.0f, sGate - 2.5f);
        const float uv0[8] = {};
        { // stop line across the approach lanes
            const V2 a = Pt(0.25f, sStop - 0.45f), b = Pt(F.half - 0.15f, sStop - 0.45f), c = Pt(F.half - 0.15f, sStop), e = Pt(0.25f, sStop);
            Q(paint, P3(a, yP), P3(b, yP), P3(c, yP), P3(e, yP), kPaintCol, uv0);
        }
        if (!p.lod)
            for (int j = 0; j < lanes; ++j) { // RXR: an X 6 m long across each lane, before the stop line
                const float lc = (j + 0.5f) * lw, hw = std::max(0.6f, lw * 0.5f - 0.5f), sc = sStop - 6.0f, t = 0.18f;
                for (float k : {-1.0f, 1.0f}) {
                    const V2 p0 = Pt(lc - k * hw, sc - 3.0f), p1 = Pt(lc + k * hw, sc + 3.0f), n = RightOf(Norm(p1 - p0)) * t;
                    Q(paint, P3(p0 - n, yP), P3(p1 - n, yP), P3(p1 + n, yP), P3(p0 + n, yP), kPaintCol, uv0);
                }
            }
        Mast(prop, Pt(lg, sGate), d, right, p.lod);
        const bool median = F.half > 7.5f;
        Boom(prop, d, right, sGate - 0.12f, lg - 0.35f, -1.0f, (lg - 0.35f) - (median ? F.half * 0.5f : 0.2f), p.gate);
        if (median) {
            Mast(prop, Pt(0.45f, sGate), d, right, p.lod);
            Boom(prop, d, right, sGate - 0.12f, 0.8f, 1.0f, F.half * 0.5f - 0.7f, p.gate);
        }
        if (lanes >= 2 && !p.lod) { // cantilever arm and a light head over each lane
            CardBox(prop, Pt(0, sGate), right, lw * 0.5f, lg, kRoadY + 5.6f, kRoadY + 5.85f, kCellMachine, kCellMachine + 0.25f, 0.5f, 1, d * -1.0f);
            for (int j = 0; j < lanes; ++j)
                CardBox(prop, Pt((j + 0.5f) * lw, sGate - 0.05f), right, -0.45f, 0.45f, kRoadY + 4.95f, kRoadY + 5.45f,
                        kCellMast + 0.25f * 0.12f, kCellMast + 0.25f * 0.88f, 0.31f, 0.42f, d * -1.0f);
        }
    }
    if (!p.lod) { // relay cabinet beside the track, clear of the road, ramps and sidewalk
        const float zc = -(kBedFootHalf + 1.6f);
        const V2 cab{std::max(F.At(-F.outer, zc).x, F.At(F.outer, zc).x) + 2.5f, zc};
        Box(prop, cab, {1, 0}, 1.4f, 1.0f, kRoadY, 1.6f, {0.62f, 0.63f, 0.60f}, kCellMachine);
    }
}
} // namespace jmesh

// Clamp parameters into what can be built (the same limits as the network: rail_junction.h). Returns the used set.
inline JunctionParams AdjustParams(JunctionParams p, bool* adjusted = nullptr, std::string* note = nullptr) {
    const JunctionParams in = p;
    p.radius = std::max(p.radius, kRailMinRadius);
    p.gauge = std::clamp(p.gauge, 0.6f, 1.7f);
    switch (p.type) {
    case JType::Diamond: p.angleDeg = std::clamp(p.angleDeg, kDiamondMinDeg, 90.0f); break;
    case JType::SingleSlip: p.angleDeg = std::clamp(p.angleDeg, kDiamondMinDeg, kSlipMaxDeg); break;
    case JType::LevelCrossing: p.angleDeg = std::clamp(p.angleDeg, kLevelCrossMinDeg, 90.0f), p.roadWidth = std::clamp(p.roadWidth, 4.0f, 30.0f);
        p.sidewalk = std::clamp(p.sidewalk, 0.0f, 4.0f), p.gate = std::clamp(p.gate, 0.0f, 1.0f);
        break;
    case JType::Crossover:
    case JType::Scissors:
        p.spacing = std::clamp(p.spacing, kCrossoverMinSpacing, kCrossoverMaxSpacing);
        p.angleDeg = std::clamp(p.angleDeg, kTurnoutMinDeg, std::min(kTurnoutMaxDeg, CrossoverMaxAngle(p.spacing, p.radius)));
        break;
    default: p.angleDeg = std::clamp(p.angleDeg, kTurnoutMinDeg, kTurnoutMaxDeg); break;
    }
    const bool changed = p.angleDeg != in.angleDeg || p.radius != in.radius || p.spacing != in.spacing || p.roadWidth != in.roadWidth || p.gauge != in.gauge;
    if (adjusted) *adjusted = changed;
    if (note && changed) *note = p.angleDeg != in.angleDeg ? "angle limited" : p.radius != in.radius ? "radius at least 45 m" : "size limited";
    return p;
}

// The route centre lines of a junction (local frame), and which are tie-only (virtual).
inline std::vector<jmesh::Route> JunctionRoutes(const JunctionParams& p, std::vector<char>* virt = nullptr) {
    using namespace jmesh;
    std::vector<jmesh::Route> R;
    std::vector<char> v;
    const float s = p.hand == Hand::Left ? -1.0f : 1.0f, a = p.angleDeg, Rr = p.radius;
    const V2 O{0, 0}, X{1, 0};
    auto add = [&](jmesh::Route r, bool isVirt = false) { R.push_back(std::move(r)), v.push_back(isVirt); };
    const float stemL = jmesh::Pitched(p.stem);
    auto stem = [&] { jmesh::Route r; Straight(r, {-stemL, 0}, X, stemL); add(r); };
    const float lead = TurnoutLead(a, Rr);
    switch (p.type) {
    case JType::Turnout: {
        stem();
        jmesh::Route st, dv;
        Straight(st, O, X, jmesh::Pitched(lead + p.tail)), add(st);
        Arc(dv, O, X, Rr, a, s, p.lod), Tail(dv, p.tail), add(dv);
    } break;
    case JType::Wye: {
        stem();
        jmesh::Route ax, l, r;
        Straight(ax, O, X, jmesh::Pitched(lead + p.tail)), add(ax, true);
        Arc(l, O, X, Rr, a, -1, p.lod), Tail(l, p.tail), add(l);
        Arc(r, O, X, Rr, a, 1, p.lod), Tail(r, p.tail), add(r);
    } break;
    case JType::ThreeWay: {
        stem();
        jmesh::Route st, l, r;
        Straight(st, O, X, jmesh::Pitched(lead + p.tail)), add(st);
        Arc(l, O, X, Rr, a, -1, p.lod), Tail(l, p.tail), add(l);
        Arc(r, O, X, Rr, a, 1, p.lod), Tail(r, p.tail), add(r);
    } break;
    case JType::Diamond:
    case JType::SingleSlip: {
        const float th = Rad(a), T = p.type == JType::SingleSlip ? Rr * std::tan(th * 0.5f) : 0.0f;
        const float H = std::max(8.0f + 1.6f / std::sin(th), T + 5.0f);
        const V2 b{std::cos(th), s * std::sin(th)};
        jmesh::Route A, B;
        const float Hp = jmesh::Pitched(2 * H) * 0.5f;
        Straight(A, {-Hp, 0}, X, 2 * Hp), add(A);
        Straight(B, b * -Hp, b, 2 * Hp), add(B);
        if (p.type == JType::SingleSlip) { jmesh::Route sl; Arc(sl, {-T, 0}, X, Rr, a, s, p.lod), add(sl); }
    } break;
    case JType::Crossover:
    case JType::Scissors: {
        const float run = CrossoverRun(p.spacing, a, Rr), o = TurnoutOffset(a, Rr);
        const float D = std::max(0.0f, p.spacing - 2 * o) / std::sin(Rad(a));
        jmesh::Route A, B, x;
        const float AL = jmesh::Pitched(run + 2 * p.stem);
        Straight(A, {-p.stem, 0}, X, AL), add(A);
        Straight(B, {-p.stem, s * p.spacing}, X, AL), add(B);
        auto mid = [&](jmesh::Route& r) { if (D > 0.01f) Straight(r, r.back().c, r.back().f, D); }; // exact: no pitch snap inside
        Arc(x, O, X, Rr, a, s, p.lod), mid(x), Arc(x, O, X, Rr, a, -s, p.lod), add(x);
        if (p.type == JType::Scissors) { jmesh::Route y; Arc(y, {0, s * p.spacing}, X, Rr, a, -s, p.lod), mid(y), Arc(y, O, X, Rr, a, s, p.lod), add(y); }
    } break;
    case JType::LevelCrossing: {
        const jmesh::CrossingFrame F = jmesh::CrossFrame(p);
        const float zr = jmesh::RampZ(F);
        float H = 0;
        for (float e : {-F.outer, F.outer})
            for (float z : {-zr, zr}) H = std::max(H, std::fabs(F.At(e, z).x));
        H += 8.0f; // track beyond the ramps and the relay cabinet
        jmesh::Route A;
        const float Hp = jmesh::Pitched(2 * H) * 0.5f;
        Straight(A, {-Hp, 0}, X, 2 * Hp), add(A);
    } break;
    default: break;
    }
    if (virt) *virt = v;
    return R;
}

// The model of one junction variant. Deterministic: the same parameters give the same vertices.
inline void JunctionProps(JunctionMesh& m, const JunctionParams& p);
inline JunctionMesh BuildJunctionMesh(const JunctionParams& want) {
    using namespace jmesh;
    JunctionMesh m;
    const JunctionParams p = AdjustParams(want, &m.adjusted, &m.note);
    m.angleDeg = p.angleDeg, m.radius = p.radius;
    std::vector<char> virt;
    const std::vector<jmesh::Route> routes = JunctionRoutes(p, &virt);
    if (routes.empty()) return m;
    for (size_t k = 0; k < routes.size(); ++k)
        if (!virt[k]) Bed(m, routes[k], p.lod);
    const bool level = p.type == JType::LevelCrossing;
    const V2 road{std::cos(Rad(p.angleDeg)), std::sin(Rad(p.angleDeg))};
    const float roadHalf = p.roadWidth * 0.5f + p.sidewalk + 0.2f; // under the deck: no ties
    auto underRoad = [&](V2 q) { return level && std::fabs(Dot2(q, RightOf(road))) < roadHalf; };
    if (!p.lod) Ties(m, routes, virt, p.gauge, underRoad);
    for (size_t k = 0; k < routes.size(); ++k)
        if (!virt[k]) Rails(m, routes[k], p.gauge, p.lod);
    if (!p.lod && !level) Frogs(m, routes, virt, p.gauge);
    JunctionProps(m, p);
    return m;
}

// What sticks up out of the track bed (shared by both paths): point machines (lod 0), and for a level crossing its
// ramps, markings, gates, signals and cabinet (and the deck unless p.spom bakes it).
inline void JunctionProps(JunctionMesh& m, const JunctionParams& p) {
    using namespace jmesh;
    const float s = p.hand == Hand::Left ? -1.0f : 1.0f;
    const V2 O{0, 0}, X{1, 0}, nX{-1, 0};
    if (!p.lod) switch (p.type) {
        case JType::Turnout: PointMachine(m, O, X, -s); break;
        case JType::Wye: PointMachine(m, O, X, -1); break;
        case JType::ThreeWay: PointMachine(m, O, X, -1), PointMachine(m, O, X, 1); break;
        case JType::SingleSlip: PointMachine(m, {-p.radius * std::tan(Rad(p.angleDeg) * 0.5f), 0}, X, -s); break;
        case JType::Crossover:
        case JType::Scissors: {
            const float run = CrossoverRun(p.spacing, p.angleDeg, p.radius);
            PointMachine(m, O, X, -s), PointMachine(m, {run, s * p.spacing}, nX, -s);
            if (p.type == JType::Scissors) PointMachine(m, {0, s * p.spacing}, X, s), PointMachine(m, {run, 0}, nX, s);
        } break;
        default: break;
        }
    if (p.type == JType::LevelCrossing) LevelCrossingProps(m, p);
}

// Every variant the game ships (both hands where it matters, LOD 0 and 1): for the tests and the preview's table.
inline std::vector<JunctionParams> JunctionCatalog() {
    std::vector<JunctionParams> v;
    auto add = [&](JType t, Hand h, float ang) {
        for (int lod = 0; lod < 2; ++lod) {
            JunctionParams p;
            p.type = t, p.hand = h, p.angleDeg = ang, p.lod = lod;
            v.push_back(p);
        }
    };
    add(JType::Turnout, Hand::Left, 12), add(JType::Turnout, Hand::Right, 12);
    add(JType::Wye, Hand::None, 8), add(JType::ThreeWay, Hand::None, 12);
    add(JType::Diamond, Hand::Right, 30), add(JType::Diamond, Hand::Right, 90);
    add(JType::SingleSlip, Hand::Right, 15);
    add(JType::Crossover, Hand::Right, 10), add(JType::Crossover, Hand::Left, 10), add(JType::Scissors, Hand::Right, 10);
    add(JType::LevelCrossing, Hand::None, 90), add(JType::LevelCrossing, Hand::None, 45);
    return v;
}

} // namespace city
} // namespace tbd
