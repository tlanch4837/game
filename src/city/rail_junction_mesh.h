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

enum class JMat : int { Ballast, Tie, Steel, Prop, Paint, Count };

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
constexpr float kTieTop = 0.33f, kTieHalfLen = 1.3f, kTieWidth = 0.26f, kTieStep = 0.65f;
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
    const float s0 = r.back().s, ang = Rad(angleDeg), step = lod ? 6.0f : 3.0f, maxLen = lod ? 24.0f : 12.0f; // a 3 deg chord sags 5 cm at R 150
    const int n = std::max(1, std::max((int)std::ceil(angleDeg / step), (int)std::ceil(R * ang / maxLen)));
    const V2 nrm = RightOf(f) * side;
    for (int i = 1; i <= n; ++i) {
        const float t = ang * i / n;
        r.push_back({a + f * (R * std::sin(t)) + nrm * (R * (1 - std::cos(t))), Norm(f * std::cos(t) + nrm * std::sin(t)), s0 + R * t});
    }
}
inline void Tail(Route& r, float len) {
    if (len > 0.01f) Straight(r, r.back().c, r.back().f, len);
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

// Ties of every route, all in one vertex list (a single draw). Under a switch, each tie is its own quad (long timbers:
// an earlier route's tie is lengthened to carry the rails of any later route whose centre crosses it within kLongTie,
// and the later route's own ties are skipped there). Plain track between is a TieRun strip. virt[k]: route k has no
// rails of its own (a wye's centre line), only timbers under the routes crossing it. `skip(q)`: no tie (a level
// crossing's deck).
template <class Skip>
inline void Ties(JunctionMesh& m, const std::vector<Route>& routes, const std::vector<char>& virt, float gauge, Skip&& skip) {
    auto& o = m.byMat[(int)JMat::Tie];
    const float halfStd = gauge * 0.5f + (kTieHalfLen - kRailGauge * 0.5f);
    for (size_t k = 0; k < routes.size(); ++k) {
        const Route& r = routes[k];
        const float L = r.back().s;
        float runA = -1, runB = -1; // the open run of plain ties
        auto flush = [&] {
            if (runA >= 0) TieRun(o, r, runA, runB, halfStd);
            runA = -1;
        };
        for (float s = kTieStep * 0.5f; s < L; s += kTieStep) {
            V2 c, f;
            RouteAt(r, s, c, f);
            const V2 n = RightOf(f);
            bool covered = skip(c);
            for (size_t j = 0; j < k && !covered; ++j) {
                V2 fj;
                const float d = DistTo(routes[j], c, &fj);
                covered = d < (std::fabs(Dot2(fj, f)) < 0.5f ? halfStd : kLongTie);
            }
            if (covered) { flush(); continue; }
            float lo = virt[k] ? 1e9f : -halfStd, hi = virt[k] ? -1e9f : halfStd;
            bool longTie = false;
            for (size_t j = k + 1; j < routes.size(); ++j) {
                float u;
                if (!virt[j] && CrossAt(routes[j], c, n, kLongTie, u)) lo = std::min(lo, u - halfStd), hi = std::max(hi, u + halfStd), longTie = true;
            }
            if (hi < lo) { flush(); continue; }
            if (!longTie) { // plain: extend the run
                if (runA < 0) runA = std::max(0.0f, s - kTieStep * 0.5f);
                runB = std::min(L, s + kTieStep * 0.5f);
                continue;
            }
            flush();
            const V2 w = f * (kTieWidth * 0.5f);
            const Vec3 p0 = P3(c + n * lo - w, kTieTop), p1 = P3(c + n * hi - w, kTieTop), p2 = P3(c + n * hi + w, kTieTop), p3 = P3(c + n * lo + w, kTieTop);
            const float ul = (hi - lo) / (2 * halfStd), v0 = 0.3f, v1 = 0.7f, uv[8] = {0, v0, ul, v0, ul, v1, 0, v1};
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
// Frogs where rails of two routes cross (not where they start together at the points), and a check rail inside the
// opposite running rail of each route there. Returns the frog count.
inline int Frogs(JunctionMesh& m, const std::vector<Route>& routes, const std::vector<char>& virt, float gauge) {
    struct Hit {
        V2 p, fa, fb;
        int ra, rb;
        float sa, sb;
    };
    std::vector<Hit> hits;
    std::vector<V2> pa, fa, pb, fb;
    for (size_t i = 0; i < routes.size(); ++i)
        for (size_t j = i + 1; j < routes.size(); ++j)
            if (!virt[i] && !virt[j])
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
                            hits.push_back({P, Norm(r), Norm(q), (int)i, (int)j, si, sj});
                        }
                }
    auto& steel = m.byMat[(int)JMat::Steel];
    std::vector<std::pair<int, V2>> checks; // (route * 2 + side, where): deduplicated check rails
    for (const Hit& h : hits) {
        const V2 bis = Norm(h.fa + h.fb);
        Box(steel, h.p, bis, 1.4f, 0.22f, kTieTop, kRailTop - kTieTop, kSteelCol, 0.0f);
        for (int w = 0; w < 2; ++w) {
            const int ri = w ? h.rb : h.ra;
            const float side = -(w ? h.sb : h.sa);
            V2 f;
            DistTo(routes[ri], h.p, &f);
            // the opposite rail, level with the frog
            const V2 c = h.p + RightOf(f) * (side * (gauge + kRailHead));
            bool dup = false;
            for (const auto& ck : checks) dup = dup || (ck.first == ri * 2 + (side > 0) && Len(ck.second - c) < 1.5f);
            if (dup) continue;
            checks.push_back({ri * 2 + (side > 0), c});
            const V2 in = RightOf(f) * (-side * kCheckGap);
            RailStrip(m, {c + in - f * 1.6f, c + in + f * 1.6f}, {f, f}, 0, kRailTop - 0.02f);
        }
    }
    return (int)hits.size();
}

// A point machine beside the points at p (forward f), on `side` of the track.
inline void PointMachine(JunctionMesh& m, V2 p, V2 f, float side) {
    Box(m.byMat[(int)JMat::Prop], p + f * 0.9f + RightOf(f) * (side * 1.9f), f, 1.0f, 0.45f, 0.12f, 0.4f, {0.55f, 0.57f, 0.52f}, kCellMachine);
}

// Level crossing furniture: the rail runs along x through the origin, the road along r (angle from x), w wide.
// Deck between the road's edges over the bed, a stop line and an RR mark on each approach lane, a gate mast (crossbuck
// and lights baked on crossed cards) on the right kerb of each approach with its boom over the lane, a relay cabinet.
inline void LevelCrossingProps(JunctionMesh& m, V2 r, float w, int lod) {
    auto& prop = m.byMat[(int)JMat::Prop];
    auto& paint = m.byMat[(int)JMat::Paint];
    const V2 nr = RightOf(r);
    const float half = w * 0.5f, deckHalf = kBedTopHalf + 0.2f, sinA = std::max(0.2f, std::fabs(Cross2(V2{1, 0}, r)));
    // deck: the parallelogram where the road (|dot(q, nr)| <= half) covers the bed (|z| <= deckHalf)
    auto edgeX = [&](float e, float z) { return (e - z * nr.z) / nr.x; };
    if (std::fabs(nr.x) > 1e-3f) {
        const V2 q0{edgeX(-half, -deckHalf), -deckHalf}, q1{edgeX(half, -deckHalf), -deckHalf}, q2{edgeX(half, deckHalf), deckHalf}, q3{edgeX(-half, deckHalf), deckHalf};
        const float uv[8] = {kCellDeck, 0, kCellDeck + 0.25f, 0, kCellDeck + 0.25f, 1, kCellDeck, 1};
        Q(prop, P3(q0, kRailTop + 0.005f), P3(q1, kRailTop + 0.005f), P3(q2, kRailTop + 0.005f), P3(q3, kRailTop + 0.005f), {0.34f, 0.34f, 0.35f}, uv);
    }
    const float clear = deckHalf / sinA; // along the road from the track centre to the deck's edge
    for (float dirSign : {1.0f, -1.0f}) {
        const V2 d = r * dirSign, right = RightOf(d); // traffic approaching along d, on its right half
        const V2 back = d * -1.0f;
        const float yP = 0.02f, uv0[8] = {};
        // stop line across the approach lane
        const V2 sc = back * (clear + 3.5f);
        const V2 a = sc + right * 0.2f, b = sc + right * (half - 0.2f), bw = d * 0.4f;
        Q(paint, P3(a, yP), P3(b, yP), P3(b + bw, yP), P3(a + bw, yP), kPaintCol, uv0);
        if (!lod) { // an X across the lane before the stop line
            const V2 xc = back * (clear + 12.0f) + right * (half * 0.5f);
            for (float k : {-1.0f, 1.0f}) {
                const V2 u = Norm(d + right * k) * 2.6f, t = RightOf(Norm(u)) * 0.2f;
                Q(paint, P3(xc - u - t, yP), P3(xc + u - t, yP), P3(xc + u + t, yP), P3(xc - u + t, yP), kPaintCol, uv0);
            }
        }
        // gate: mast on the right kerb 1 m before the stop line, boom over the lane to the centre line
        const V2 mast = back * (clear + 2.5f) + right * (half + 0.7f);
        const V2 ax = d * 0.45f, az = right * 0.45f;
        Card(prop, mast - az, mast + az, 0, 4.2f, kCellMast);
        if (!lod) Card(prop, mast - ax, mast + ax, 0, 4.2f, kCellMast);
        Card(prop, mast + right * -0.3f, mast + right * (-0.3f - half), 1.0f, 1.18f, kCellBoom);
    }
    if (!lod) { // relay cabinet beside the track, off the road
        const V2 cab{half / sinA + 4.0f, -(kBedFootHalf + 1.2f)};
        Box(prop, cab, {1, 0}, 1.4f, 1.0f, 0, 1.6f, {0.62f, 0.63f, 0.60f}, kCellMachine);
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
    case JType::LevelCrossing: p.angleDeg = std::clamp(p.angleDeg, kLevelCrossMinDeg, 90.0f), p.roadWidth = std::clamp(p.roadWidth, 4.0f, 30.0f); break;
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
    std::vector<Route> R;
    std::vector<char> v;
    const float s = p.hand == Hand::Left ? -1.0f : 1.0f, a = p.angleDeg, Rr = p.radius;
    const V2 O{0, 0}, X{1, 0};
    auto add = [&](Route r, bool isVirt = false) { R.push_back(std::move(r)), v.push_back(isVirt); };
    auto stem = [&] { Route r; Straight(r, {-p.stem, 0}, X, p.stem); add(r); };
    const float lead = TurnoutLead(a, Rr);
    switch (p.type) {
    case JType::Turnout: {
        stem();
        Route st, dv;
        Straight(st, O, X, lead + p.tail), add(st);
        Arc(dv, O, X, Rr, a, s, p.lod), Tail(dv, p.tail), add(dv);
    } break;
    case JType::Wye: {
        stem();
        Route ax, l, r;
        Straight(ax, O, X, lead + p.tail), add(ax, true);
        Arc(l, O, X, Rr, a, -1, p.lod), Tail(l, p.tail), add(l);
        Arc(r, O, X, Rr, a, 1, p.lod), Tail(r, p.tail), add(r);
    } break;
    case JType::ThreeWay: {
        stem();
        Route st, l, r;
        Straight(st, O, X, lead + p.tail), add(st);
        Arc(l, O, X, Rr, a, -1, p.lod), Tail(l, p.tail), add(l);
        Arc(r, O, X, Rr, a, 1, p.lod), Tail(r, p.tail), add(r);
    } break;
    case JType::Diamond:
    case JType::SingleSlip: {
        const float th = Rad(a), T = p.type == JType::SingleSlip ? Rr * std::tan(th * 0.5f) : 0.0f;
        const float H = std::max(8.0f + 1.6f / std::sin(th), T + 5.0f);
        const V2 b{std::cos(th), s * std::sin(th)};
        Route A, B;
        Straight(A, {-H, 0}, X, 2 * H), add(A);
        Straight(B, b * -H, b, 2 * H), add(B);
        if (p.type == JType::SingleSlip) { Route sl; Arc(sl, {-T, 0}, X, Rr, a, s, p.lod), add(sl); }
    } break;
    case JType::Crossover:
    case JType::Scissors: {
        const float run = CrossoverRun(p.spacing, a, Rr), o = TurnoutOffset(a, Rr);
        const float D = std::max(0.0f, p.spacing - 2 * o) / std::sin(Rad(a));
        Route A, B, x;
        Straight(A, {-p.stem, 0}, X, run + 2 * p.stem), add(A);
        Straight(B, {-p.stem, s * p.spacing}, X, run + 2 * p.stem), add(B);
        Arc(x, O, X, Rr, a, s, p.lod), Tail(x, D), Arc(x, O, X, Rr, a, -s, p.lod), add(x);
        if (p.type == JType::Scissors) { Route y; Arc(y, {0, s * p.spacing}, X, Rr, a, -s, p.lod), Tail(y, D), Arc(y, O, X, Rr, a, s, p.lod), add(y); }
    } break;
    case JType::LevelCrossing: {
        const float th = Rad(a), H = (p.roadWidth * 0.5f) / std::sin(th) + 1.8f / std::tan(th) + 10.0f;
        Route A;
        Straight(A, {-H, 0}, X, 2 * H), add(A);
    } break;
    default: break;
    }
    if (virt) *virt = v;
    return R;
}

// The model of one junction variant. Deterministic: the same parameters give the same vertices.
inline JunctionMesh BuildJunctionMesh(const JunctionParams& want) {
    using namespace jmesh;
    JunctionMesh m;
    const JunctionParams p = AdjustParams(want, &m.adjusted, &m.note);
    m.angleDeg = p.angleDeg, m.radius = p.radius;
    std::vector<char> virt;
    const std::vector<Route> routes = JunctionRoutes(p, &virt);
    if (routes.empty()) return m;
    for (size_t k = 0; k < routes.size(); ++k)
        if (!virt[k]) Bed(m, routes[k], p.lod);
    const bool level = p.type == JType::LevelCrossing;
    const V2 road{std::cos(Rad(p.angleDeg)), std::sin(Rad(p.angleDeg))};
    const float roadHalf = p.roadWidth * 0.5f + 0.3f;
    auto underRoad = [&](V2 q) { return level && std::fabs(Dot2(q, RightOf(road))) < roadHalf; };
    if (!p.lod) Ties(m, routes, virt, p.gauge, underRoad);
    for (size_t k = 0; k < routes.size(); ++k)
        if (!virt[k]) Rails(m, routes[k], p.gauge, p.lod);
    if (!p.lod && !level) Frogs(m, routes, virt, p.gauge);
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
    if (level) LevelCrossingProps(m, road, p.roadWidth, p.lod);
    return m;
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
