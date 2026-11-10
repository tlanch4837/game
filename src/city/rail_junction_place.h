// Where the rail junction planes (rail_junction_plane.h) go in a network, and what they change in it. Header only,
// headless (tests\rail_junction_test.cpp); the game draws the result (game/rail_junction_host.inl).
//
//   Sites      every rail node with 3+ single-track rail arms that ClassifyArms can name (turnout, wye, 3-way, diamond),
//              crossovers (two turnouts joined by their diverging tracks, the layout's spacing and length apart) and
//              every level crossing (FindLevelCrossings). Each gets its canonical layout, mirrored for left hand.
//   Frame      the layout's x along the stem, z across (Right = +z): world = o + f * x + RightOf(f) * z.
//              A switch's graph node is the intersection point (IP) of its routes' end tangents: the box is set back
//              so the diverging route's end lies on the graph's diverging line, so the plain track carries on exactly
//              along the segments. A diamond's node is its centre; a crossover's first switch its points.
//   Cuts       every arm's plain track stops where its route of the layout ends (its sleeper phase is anchored there);
//              a level crossing's rail has none along the box (RailCuts::trackGaps). The segments' strips and junction
//              fills stay: the box's bed sits on them as the plain track sits on its gravel strip.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "rail_junction_plane.h"

namespace tbd {
namespace city {

struct JunctionSite {
    JType type = JType::Turnout;
    int layout = -1;
    bool mirror = false;      // drawn left-handed (z flipped)
    int node = -1, node2 = -1; // the graph node (crossover: both switches); level crossing: -1
    int rail = -1, road = -1;  // level crossing: the two segments
    float tRail = 0, tRoad = 0;
    V2 o{}, f{1, 0};          // the layout frame in the world
    float railTop = 0;        // world height of the rail head (the box top) at the frame's origin ...
    float slopeX = 0, slopeZ = 0; // ... and its grade along / across (canonical): the box tilts with the line (FitSitePlane)
    float roadY = 0;          // level crossing: the road surface's world height
    float roadWidth = 9, sidewalkL = 0, sidewalkR = 0; // level crossing: carriageway, side bands (left / right of +road)
    bool roadFlip = false;    // level crossing: the road runs against the layout's road direction
    V2 World(float x, float z) const { return o + f * x + RightOf(f) * (mirror ? -z : z); }
    float HeightAt(V2 w) const; // the rail head's world height over the world point w
    V2 Local(V2 w) const { // world -> canonical layout frame
        const V2 d = w - o;
        const float z = Dot2(d, RightOf(f));
        return {Dot2(d, f), mirror ? -z : z};
    }
};

// A layout's route ends in its canonical frame: point and outward direction.
struct RouteEnd {
    V2 p, out;
};
inline const std::vector<RouteEnd>& LayoutEnds(int layout) {
    static std::vector<std::vector<RouteEnd>> cache;
    const auto& L = JunctionLayouts();
    if (cache.empty()) {
        cache.resize(L.size());
        for (size_t i = 0; i < L.size(); ++i) {
            std::vector<char> virt;
            const auto routes = JunctionRoutes(AdjustParams(L[i].p), &virt);
            for (size_t k = 0; k < routes.size(); ++k) {
                if (virt[k] || routes[k].size() < 2) continue;
                cache[i].push_back({routes[k].front().c, routes[k].front().f * -1.0f});
                cache[i].push_back({routes[k].back().c, routes[k].back().f});
            }
        }
    }
    static const std::vector<RouteEnd> none;
    return layout >= 0 && layout < (int)cache.size() ? cache[(size_t)layout] : none;
}
// Where a switch layout's graph node sits along its stem line (x of the IP): the diverging end's tangent meets z = 0.
inline float LayoutIP(int layout) {
    const JunctionLayout& L = JunctionLayouts()[(size_t)layout];
    if (L.p.type != JType::Turnout && L.p.type != JType::Wye && L.p.type != JType::ThreeWay) return 0.0f;
    float best = 0, bz = 0;
    for (const RouteEnd& e : LayoutEnds(layout)) // the end farthest across (the diverging route; wye / 3-way: either leg)
        if (std::fabs(e.p.z) > bz + 1e-3f && std::fabs(e.out.z) > 1e-3f) bz = std::fabs(e.p.z), best = e.p.x - e.p.z * e.out.x / e.out.z;
    return best;
}

// A segment's rail-head height at parameter t, as BuildSegment lifts a ground track over the terrain.
inline float RailTopAt(const Network& net, int s, float t, const GroundFn& ground) {
    const Profile& p = net.Prof(s);
    const Cubic c = net.Curve(s);
    const V2 q = c.At(t), l = LeftOf(c.Tan(t));
    float y = net.YAt(net.segs[s], t);
    if (ground && std::fabs(y - ground(q.x, q.z)) < 1.5f) {
        float top = ground(q.x, q.z);
        for (float off : {p.Half(), p.Half() * 0.5f, -p.Half() * 0.5f, -p.Half()}) top = std::max(top, ground((q + l * off).x, (q + l * off).z));
        y = std::max(y, top + 0.02f);
    }
    float h = 0.1f;
    for (const Strip& st : p.strips)
        if (st.kind == Kind::Track) { h = st.h; break; }
    return y + mesh::kRoadLift + h + mesh::kTrackDepth - 0.05f;
}
inline int TrackStrips(const Network& net, int s) {
    int k = 0;
    for (const Strip& st : net.Prof(s).strips) k += st.kind == Kind::Track;
    return k;
}
inline float NodeRailTop(const Network& net, int n, int s, const GroundFn& ground) {
    return RailTopAt(net, s, net.segs[s].a == n ? 0.0f : 1.0f, ground);
}
inline int LayoutFor(JType t, float angleDeg, Hand h, bool& mirror, float roadWidth = 9.0f) {
    JunctionParams p;
    p.type = t, p.angleDeg = angleDeg, p.hand = h, p.roadWidth = roadWidth;
    return NearestLayout(p, &mirror);
}

inline float JunctionSite::HeightAt(V2 w) const {
    const V2 q = Local(w);
    return railTop + slopeX * q.x + slopeZ * q.z;
}
// The box's plane: through the plain track's rail head where each route of the layout meets it (the nearest single-track
// rail segment within 2.5 m of the route's end), least squares, grades within 6 %. So a junction on a slope meets its
// tracks without a step, as the plain sweep follows the line's height. Too few ends: flat at railTop.
inline void FitSitePlane(const Network& net, JunctionSite& S, const GroundFn& ground) {
    std::vector<Vec3> pts; // canonical x, height, canonical z
    for (const RouteEnd& e : LayoutEnds(S.layout)) {
        const V2 w = S.World(e.p.x, e.p.z);
        int best = -1;
        float bt = 0, bd = 2.5f;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || !net.IsRailSeg((int)s)) continue;
            float t, d;
            net.Nearest((int)s, w, t, d);
            if (d < bd) bd = d, bt = t, best = (int)s;
        }
        if (best >= 0) pts.push_back({e.p.x, RailTopAt(net, best, bt, ground), e.p.z});
    }
    if (pts.size() < 2) return;
    Vec3 m{0, 0, 0};
    for (const Vec3& p : pts) m = m + p * (1.0f / (float)pts.size());
    float sxx = 0, sxz = 0, szz = 0, sxh = 0, szh = 0;
    for (const Vec3& p : pts) {
        const float x = p.x - m.x, z = p.z - m.z, h = p.y - m.y;
        sxx += x * x, sxz += x * z, szz += z * z, sxh += x * h, szh += z * h;
    }
    float b = 0, c = 0;
    const float det = sxx * szz - sxz * sxz;
    if (szz > 1.0f && std::fabs(det) > 1e-3f * sxx * szz) b = (sxh * szz - szh * sxz) / det, c = (szh * sxx - sxh * sxz) / det;
    else if (sxx > 1e-3f) b = sxh / sxx;
    S.slopeX = std::clamp(b, -0.06f, 0.06f), S.slopeZ = std::clamp(c, -0.06f, 0.06f);
    S.railTop = m.y - S.slopeX * m.x - S.slopeZ * m.z;
}

// Every junction site of the network, in node order (crossovers first), then the level crossings. Deterministic.
inline std::vector<JunctionSite> FindJunctionSites(const Network& net, const GroundFn& ground) {
    std::vector<JunctionSite> v;
    std::vector<char> used(net.nodes.size(), 0);
    auto armsOf = [&](int n, std::vector<int>& ss) { // single-track rail arms only (a double track has no layouts)
        std::vector<V2> arms = RailArms(net, n, &ss);
        for (int s : ss)
            if (TrackStrips(net, s) != 1) arms.clear();
        return arms;
    };
    for (size_t n = 0; n < net.nodes.size(); ++n) { // crossovers: two turnouts that share their diverging track
        std::vector<int> ss, s2;
        const std::vector<V2> arms = armsOf((int)n, ss);
        if (arms.size() != 3 || used[n]) continue;
        const JunctionInfo J = ClassifyArms(arms);
        if (J.type != JType::Turnout) continue;
        const int diag = ss[(size_t)J.routes[1]], m = net.segs[diag].a == (int)n ? net.segs[diag].b : net.segs[diag].a;
        if (m == (int)n || used[(size_t)m]) continue;
        const std::vector<V2> arms2 = armsOf(m, s2);
        if (arms2.size() != 3) continue;
        const JunctionInfo J2 = ClassifyArms(arms2);
        if (J2.type != JType::Turnout || s2[(size_t)J2.routes[1]] != diag) continue;
        JunctionSite S;
        S.type = JType::Crossover, S.layout = LayoutFor(JType::Crossover, 10, J.hand, S.mirror);
        if (S.layout < 0) continue;
        S.node = (int)n, S.node2 = m, S.o = XZ(net.nodes[n].p), S.f = arms[(size_t)J.stem] * -1.0f;
        const JunctionParams lp = AdjustParams(JunctionLayouts()[(size_t)S.layout].p);
        const V2 b = S.Local(XZ(net.nodes[(size_t)m].p));
        if (std::fabs(b.x - CrossoverRun(lp.spacing, lp.angleDeg, lp.radius)) > 2.0f || std::fabs(b.z - lp.spacing) > 0.8f ||
            Dot2(arms2[(size_t)J2.stem], S.f) < 0.99f)
            continue; // not the layout's spacing and length: two plain turnouts
        S.railTop = NodeRailTop(net, (int)n, ss[(size_t)J.stem], ground);
        used[n] = used[(size_t)m] = 1;
        FitSitePlane(net, S, ground);
        v.push_back(S);
    }
    for (size_t n = 0; n < net.nodes.size(); ++n) { // switches and diamonds
        std::vector<int> ss;
        const std::vector<V2> arms = armsOf((int)n, ss);
        if (arms.size() < 3 || used[n]) continue;
        const JunctionInfo J = ClassifyArms(arms);
        JunctionSite S;
        S.type = J.type, S.node = (int)n;
        if (J.type == JType::Turnout || J.type == JType::Wye || J.type == JType::ThreeWay) {
            S.layout = LayoutFor(J.type, J.angleDeg, J.hand == Hand::Left ? Hand::Left : Hand::Right, S.mirror);
            if (J.type != JType::Turnout) S.mirror = false;
            if (S.layout < 0) continue;
            S.f = arms[(size_t)J.stem] * -1.0f;
            S.o = XZ(net.nodes[n].p) - S.f * LayoutIP(S.layout); // the node is the IP of the routes' end tangents
            S.railTop = NodeRailTop(net, (int)n, ss[(size_t)J.stem], ground);
        } else if (J.type == JType::Diamond) {
            S.f = arms[(size_t)J.routes[1]];
            const V2 b = Dot2(arms[(size_t)J.routes[2]], S.f) >= 0 ? arms[(size_t)J.routes[2]] : arms[(size_t)J.routes[3]];
            const bool left = Dot2(b, RightOf(S.f)) < 0;
            S.layout = LayoutFor(JType::Diamond, J.angleDeg, left ? Hand::Left : Hand::Right, S.mirror);
            if (S.layout < 0) continue;
            S.o = XZ(net.nodes[n].p);
            S.railTop = NodeRailTop(net, (int)n, ss[(size_t)J.routes[0]], ground);
        } else continue;
        used[n] = 1;
        FitSitePlane(net, S, ground);
        v.push_back(S);
    }
    for (const RailCrossing& x : FindLevelCrossings(net)) { // rail across a road at grade
        if (TrackStrips(net, x.rail) != 1) continue;
        bool dup = false;
        for (const JunctionSite& o : v) dup = dup || (o.type == JType::LevelCrossing && o.rail == x.rail && Len(o.o - x.p) < 3.0f);
        if (dup) continue;
        JunctionSite S;
        S.type = JType::LevelCrossing, S.rail = x.rail, S.road = x.road, S.tRail = x.tNew, S.tRoad = x.tOld;
        S.o = x.p, S.f = Norm(net.Curve(x.rail).Tan(x.tNew));
        V2 T = Norm(net.Curve(x.road).Tan(x.tOld));
        if (Dot2(T, S.f) < 0) T = T * -1.0f, S.roadFlip = true;
        const Profile& rp = net.Prof(x.road);
        S.roadWidth = std::max(3.0f, rp.width - rp.sideL - rp.sideR);
        S.sidewalkL = S.roadFlip ? rp.sideR : rp.sideL, S.sidewalkR = S.roadFlip ? rp.sideL : rp.sideR;
        const bool left = Dot2(T, RightOf(S.f)) < 0;
        S.layout = LayoutFor(JType::LevelCrossing, AngleBetween(S.f, T), left ? Hand::Left : Hand::Right, S.mirror, S.roadWidth);
        if (S.layout < 0) continue;
        S.mirror = left;
        S.railTop = RailTopAt(net, x.rail, x.tNew, ground);
        S.roadY = net.YAt(net.segs[(size_t)x.road], x.tOld) + mesh::kRoadLift;
        if (ground && std::fabs(S.roadY - mesh::kRoadLift - ground(x.p.x, x.p.z)) < 1.5f) S.roadY = std::max(S.roadY, ground(x.p.x, x.p.z) + 0.02f + mesh::kRoadLift);
        FitSitePlane(net, S, ground);
        v.push_back(S);
    }
    return v;
}

// The site's box in its canonical frame (the plane's rect before mirroring).
inline PlaneRect SiteRect(const JunctionSite& S) { return JunctionPlaneRect(AdjustParams(JunctionLayouts()[(size_t)S.layout].p)); }
inline bool InRect(const PlaneRect& r, V2 q, float margin) { return q.x > r.x0 - margin && q.x < r.x1 + margin && q.z > r.z0 - margin && q.z < r.z1 + margin; }

// What the sites change in the network's mesh: every arm's plain track stops where its route of
// the layout ends (the nearest point of the segment to it; an arm no route matches stops at the box's edge), and a level
// crossing's rail has no track along the box.
inline RailCuts RailCutsFor(const Network& net, const std::vector<JunctionSite>& sites) {
    RailCuts R;
    R.trackGaps.assign(net.segs.size(), {});
    for (const JunctionSite& S : sites) {
        const PlaneRect r = SiteRect(S);
        if (S.type == JType::LevelCrossing) { // the rail's segments where they run inside the box, along it
            for (size_t s = 0; s < net.segs.size(); ++s) {
                if (!net.segs[s].alive || !net.IsRailSeg((int)s)) continue;
                const Cubic c = net.Curve((int)s);
                const int N = std::max(8, (int)std::ceil(c.Length() / 0.25f));
                auto in = [&](float t) {
                    const V2 q = S.Local(c.At(t));
                    return InRect(r, q, 0.0f) && std::fabs(q.z) < 1.0f && std::fabs(Dot2(Norm(c.Tan(t)), S.f)) > 0.97f;
                };
                auto edge = [&](float a, float b) { // in(a) != in(b): bisect to the box's edge
                    const bool ia = in(a);
                    for (int k = 0; k < 20; ++k) {
                        const float m = 0.5f * (a + b);
                        (in(m) == ia ? a : b) = m;
                    }
                    return 0.5f * (a + b);
                };
                float start = -1;
                bool prev = in(0.0f);
                if (prev) start = 0.0f;
                for (int i = 1; i <= N; ++i) {
                    const float t0 = (float)(i - 1) / N, t1 = (float)i / N;
                    const bool cur = in(t1);
                    if (cur && !prev) start = edge(t0, t1);
                    if (!cur && prev) R.trackGaps[s].push_back({start, edge(t0, t1)});
                    prev = cur;
                }
                if (prev) R.trackGaps[s].push_back({start, 1.0f});
            }
            continue;
        }
        std::vector<RouteEnd> ends; // in the world
        for (const RouteEnd& e : LayoutEnds(S.layout)) {
            const float z = S.mirror ? -e.p.z : e.p.z, oz = S.mirror ? -e.out.z : e.out.z;
            ends.push_back({S.o + S.f * e.p.x + RightOf(S.f) * z, S.f * e.out.x + RightOf(S.f) * oz});
        }
        for (int n : {S.node, S.node2}) {
            if (n < 0) continue;
            const V2 np = XZ(net.nodes[(size_t)n].p);
            for (int s : net.nodes[(size_t)n].segs) {
                if (!net.segs[s].alive || !net.IsRailSeg(s)) continue;
                const bool atA = net.segs[s].a == n;
                const V2 u = net.OutDir(s, n);
                float tCut = -1, farthest = -1;
                for (const RouteEnd& e : ends) {
                    if (Dot2(e.out, u) < 0.94f) continue;
                    float t, d;
                    net.Nearest(s, e.p, t, d);
                    if (d < 2.5f && Len(e.p - np) > farthest) farthest = Len(e.p - np), tCut = t;
                }
                if (tCut < 0) { // no route of the layout: stop at the box's edge (or nowhere: all inside)
                    const Cubic c = net.Curve(s);
                    tCut = atA ? 1.0f : 0.0f;
                    for (int i = 1; i <= 64; ++i) {
                        const float t = atA ? i / 64.0f : 1.0f - i / 64.0f;
                        if (!InRect(r, S.Local(c.At(t)), 0.0f)) { tCut = t; break; }
                    }
                }
                R.trackGaps[(size_t)s].push_back(atA ? std::pair<float, float>{0.0f, tCut} : std::pair<float, float>{tCut, 1.0f});
            }
        }
    }
    return R;
}

// ---------------------------------------------------------------------------------------------------- meshes
// Layout-frame vertices -> world. mirror: flip z first (and the winding, so culling keeps the face). y0: the frame's y 0.
// y0 < -1e8: on the site's plane (the frame's y kRailTop is the rail head there); else y0 is the frame's y 0.
inline void SiteToWorld(const JunctionSite& S, const std::vector<Vertex>& in, std::vector<Vertex>& out, float y0, bool mirror, bool twoSided) {
    const V2 R = RightOf(S.f);
    auto tf = [&](Vertex v) {
        const float z = mirror ? -v.pz : v.pz, nz = mirror ? -v.nz : v.nz;
        const V2 p = S.o + S.f * v.px + R * z, n = S.f * v.nx + R * nz;
        v.px = p.x, v.pz = p.z, v.py += y0 < -1e8f ? S.HeightAt(p) - jmesh::kRailTop : y0, v.nx = n.x, v.nz = n.z;
        return v;
    };
    for (size_t k = 0; k + 2 < in.size(); k += 3) {
        Vertex a = tf(in[k]), b = tf(in[k + 1]), c = tf(in[k + 2]);
        if (mirror) std::swap(b, c);
        out.push_back(a), out.push_back(b), out.push_back(c);
        if (twoSided) { // the back face of a card: reversed, its normal flipped
            for (Vertex* q : {&a, &b, &c}) q->nx = -q->nx, q->ny = -q->ny, q->nz = -q->nz;
            out.push_back(a), out.push_back(c), out.push_back(b);
        }
    }
}
inline bool IsBoomTri(const Vertex* t) { // the gate booms' atlas cell (jmesh::Boom): every corner in it, one at its far side
    float lo = 9, hi = -9;
    for (int k = 0; k < 3; ++k) lo = std::min(lo, t[k].u), hi = std::max(hi, t[k].u);
    return lo > jmesh::kCellBoom - 0.01f && hi > jmesh::kCellBoom + 0.24f && hi < jmesh::kCellDeck + 0.01f;
}
// A level crossing's props in its frame (masts, booms at `gate`, cantilevers, cabinet, markings), for the actual road.
inline JunctionMesh CrossingProps(const JunctionSite& S, float gate) {
    JunctionParams p = AdjustParams(JunctionLayouts()[(size_t)S.layout].p);
    p.roadWidth = S.roadWidth, p.sidewalk = std::max(0.5f, 0.5f * (S.sidewalkL + S.sidewalkR)), p.gate = gate, p.spom = true, p.lod = 0;
    JunctionMesh m;
    jmesh::LevelCrossingProps(m, p);
    return m;
}
// The booms of a level crossing at `gate` (0 down, 1 up), in the world.
inline void CrossingBooms(const JunctionSite& S, float gate, std::vector<Vertex>& out) {
    const JunctionMesh m = CrossingProps(S, gate);
    const auto& prop = m.byMat[(int)JMat::Prop];
    std::vector<Vertex> booms;
    for (size_t k = 0; k + 2 < prop.size(); k += 3)
        if (IsBoomTri(&prop[k])) booms.insert(booms.end(), prop.begin() + (ptrdiff_t)k, prop.begin() + (ptrdiff_t)k + 3);
    SiteToWorld(S, booms, out, S.roadY + 0.1f, S.mirror, true);
}

// What the sites draw: one box list per layout (so one draw per layout in use), the props (Prop atlas, two-sided cards;
// the booms apart, CrossingBooms), and into the road network's own mesh the level crossings' markings and ramps.
struct JunctionDraws {
    std::vector<std::vector<Vertex>> byLayout;
    std::vector<Vertex> props;
    int sites = 0;
};
inline void CrossingRamps(const Network& net, const JunctionSite& S, MeshOut& road);
inline void BuildJunctionDraws(const Network& net, const std::vector<JunctionSite>& sites, JunctionDraws& d, MeshOut* road) {
    d.byLayout.assign(JunctionLayouts().size(), {});
    d.props.clear();
    d.sites = 0;
    for (const JunctionSite& S : sites) {
        if (S.layout < 0) continue;
        const JunctionParams& lp = JunctionLayouts()[(size_t)S.layout].p;
        JunctionParams want = lp;
        want.hand = S.mirror ? Hand::Left : Hand::Right, want.lod = 0;
        JunctionPlane J = BuildJunctionPlane(want);
        if (J.layout != S.layout) continue;
        const bool level = S.type == JType::LevelCrossing;
        SiteToWorld(S, J.mesh.byMat[(int)JMat::Spom], d.byLayout[(size_t)S.layout], -1e9f, false, false);
        if (!level) SiteToWorld(S, J.mesh.byMat[(int)JMat::Prop], d.props, -1e9f, false, false);
        else {
            const JunctionMesh m = CrossingProps(S, 1.0f);
            std::vector<Vertex> fixed;
            const auto& prop = m.byMat[(int)JMat::Prop];
            for (size_t k = 0; k + 2 < prop.size(); k += 3)
                if (!IsBoomTri(&prop[k])) fixed.insert(fixed.end(), prop.begin() + (ptrdiff_t)k, prop.begin() + (ptrdiff_t)k + 3);
            SiteToWorld(S, fixed, d.props, S.roadY + 0.1f, S.mirror, true);
            if (road) {
                SiteToWorld(S, m.byMat[(int)JMat::Paint], road->byMat[(int)Mat::Paint], S.roadY + 0.11f, S.mirror, false);
                CrossingRamps(net, S, *road);
            }
        }
        ++d.sites;
    }
}

// The ramps of a level crossing, into the road's own mesh (its materials, so they draw with the road): every strip of
// the road climbs kRampLen from its own surface (lanes from the asphalt, sidewalks and medians from their curb height,
// verges from the grass) to the deck at the rail head, on both sides of the track; a curb face where two strips differ
// in height and a side face at both road edges close the raised section.
inline void CrossingRamps(const Network& net, const JunctionSite& S, MeshOut& road) {
    const Profile& p = net.Prof(S.road);
    const V2 R = RightOf(S.f), T0 = Norm(net.Curve(S.road).Tan(S.tRoad)), T = S.roadFlip ? T0 * -1.0f : T0, side = LeftOf(T0);
    const float sinA = Dot2(T, R);
    if (std::fabs(sinA) < 0.3f) return;
    auto P = [&](float off, float z) { // the road's point at lateral `off` (its own left positive) and track offset z
        const V2 l = side * off;
        return S.o + l + T * ((z - Dot2(l, R)) / sinA);
    };
    std::vector<float> edge(p.strips.size() + 1);
    edge[0] = p.Half();
    for (size_t k = 0; k < p.strips.size(); ++k) edge[k + 1] = edge[k] - p.strips[k].w;
    const float lift = 0.035f; // the ramp's foot just over the road's markings (kPaintLift)
    auto foot = [&](size_t k) { return S.roadY + std::min(p.strips[k].h, 0.3f) + lift; };
    const Vec3 white{1, 1, 1};
    auto V = [](V2 q, float y) { return Vec3{q.x, y, q.z}; };
    auto tri2 = [&](std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c) { // both faces
        const Vec3 n = Normalize(Cross(b - a, c - a));
        mesh::Tri(o, a, b, c, n, white, a.x / 2, a.y / 2, b.x / 2, b.y / 2, c.x / 2, c.y / 2);
        mesh::Tri(o, a, c, b, n * -1.0f, white, a.x / 2, a.y / 2, c.x / 2, c.y / 2, b.x / 2, b.y / 2);
    };
    for (float sg : {-1.0f, 1.0f}) {
        const float zt = sg * jmesh::kDeckHalf, zf = sg * (jmesh::kDeckHalf + jmesh::kRampLen * std::fabs(sinA));
        for (size_t k = 0; k < p.strips.size(); ++k) {
            const Mat mat = p.strips[k].mat == Mat::Curb ? Mat::Concrete : p.strips[k].mat;
            const float tile = kMatTile[(int)mat], yf = foot(k);
            const V2 q[4] = {P(edge[k], zt), P(edge[k + 1], zt), P(edge[k + 1], zf), P(edge[k], zf)};
            const float uv[8] = {q[0].x / tile, q[0].z / tile, q[1].x / tile, q[1].z / tile, q[2].x / tile, q[2].z / tile, q[3].x / tile, q[3].z / tile};
            jmesh::Q(road.byMat[(int)mat], V(q[0], S.HeightAt(q[0])), V(q[1], S.HeightAt(q[1])), V(q[2], yf), V(q[3], yf), white, uv, {0, 1, 0});
            if (k + 1 < p.strips.size() && std::fabs(foot(k + 1) - yf) > 0.01f) // curb face between two strip heights
                tri2(road.byMat[(int)Mat::Curb], V(q[1], S.HeightAt(q[1])), V(q[2], yf), V(q[2], foot(k + 1)));
        }
        for (size_t k : {(size_t)0, p.strips.size()}) { // side faces at the road's edges, down to its own surface
            const size_t s = k ? k - 1 : 0;
            const float yb = S.roadY - 0.1f;
            tri2(road.byMat[(int)Mat::Concrete], V(P(edge[k], zt), S.HeightAt(P(edge[k], zt))), V(P(edge[k], zf), foot(s)), V(P(edge[k], zt), yb));
            tri2(road.byMat[(int)Mat::Concrete], V(P(edge[k], zf), foot(s)), V(P(edge[k], zf), yb), V(P(edge[k], zt), yb));
        }
    }
}

} // namespace city
} // namespace tbd
