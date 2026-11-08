// City pedestrians for the city-builder view (Track W). Header only, platform free, headless (tests\sim_walkers_test.cpp).
//
//   Sidewalks  SidewalkGraph turns the road network into a walking graph: each road side with a sidewalk is an Along edge
//              at the sidewalk's centre (half the road width less half the sidewalk strip from the centre line), cut where
//              the junction fill starts (ComputeTrims); junction corners are quadratic curves through the sidewalk corner band
//              (the same control points BuildJunction uses for the curb); mitred bends are Links; dead ends walk round the
//              cap; Crossings run over the painted crosswalk of each junction arm (3+ arms) and are the only edges that touch
//              the carriageway. Pedestrian streets and paths are walkable on both halves and cross anywhere at their ends.
//   Doors      the host lists doors (lots' street faces, service and station fronts, bus / tram stops) with how many people
//              use them (residents, workers, from the sim's counters); each attaches to the nearest sidewalk point.
//   Trips      a pure schedule: time is cut into windows of `window` seconds; in window k door d sends out trips
//              id = Hash(door, k, j), j < n(d, k). A trip's purpose, destination, look, speed and start come from its id
//              alone, and its route from the graph, so a trip is the same person on the same walk whenever it is looked at:
//              position(id, T). The hour of the game clock only gates which trips exist (rank < keep * curve(purpose, hour)):
//              commuters at 7-9 and 17-18, lunch to the shops at 12-13, shoppers by day, visits in the evening, few at night.
//   Showing    only trips near the camera are materialized (a disk around the eye), at most maxWalkers. A trip that comes
//              into the set mid-walk is shown only if it is out of view then, so nobody pops into view: people appear
//              stepping out of a door and disappear stepping into one, or walk in from outside the view.
//   Signals    crossings wait for the walk phase of a fixed per-junction cycle (40 s, two groups of arms), so people bunch
//              at the corners and go together. Bus / tram stops: people wait there a while, then walk on to a door nearby.
//   Cost       no allocation per frame after the first frames (fixed pools, reused buffers); a scan of the doors in the disk
//              every `scanEvery` seconds (hash arithmetic only) and at most `plansPerFrame` A* route searches per frame.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "road_network.h"

namespace tbd {
namespace city {

namespace walk {
inline uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline uint64_t Hash(uint64_t a, uint64_t b, uint64_t c = 0) { return Mix(a ^ Mix(b ^ Mix(c + 0x632BE59BD9B4E019ull))); }
inline float U01(uint64_t h) { return (float)((double)(h >> 40) * (1.0 / 16777216.0)); } // [0, 1)
constexpr float kSideLift = 0.27f;  // sidewalk top over the node height: the road lift (0.12) and the curb (0.15)
constexpr float kPathLift = 0.17f;  // paved paths and pedestrian streets
constexpr float kRoadLift = 0.13f;  // the asphalt (crossings)
constexpr float kCrossAt = 2.3f;    // the crosswalk paint runs 0.8 .. 3.8 m out from the junction cut
constexpr float kSignalCycle = 40.0f, kSignalWalk = 15.0f; // seconds: each group of arms walks for kSignalWalk of each half cycle
inline float Bell(float h, float c, float w) { // a bump at hour c, wrapped over midnight
    float d = std::fabs(h - c);
    d = std::min(d, 24.0f - d);
    return std::exp(-(d / w) * (d / w));
}
inline float Day(float h, float a, float b) { // 1 between hours a and b, soft edges of about half an hour
    const float up = std::clamp((h - a) * 2.0f + 0.5f, 0.0f, 1.0f), down = std::clamp((b - h) * 2.0f + 0.5f, 0.0f, 1.0f);
    return std::min(up, down);
}
} // namespace walk

// ------------------------------------------------------------------------------------------------ the sidewalk graph
enum class WalkEdgeKind : uint8_t { Along, Corner, Link, Cap, Crossing };

struct WalkPt {
    V2 p{};
    float y = 0;
    float s = 0;   // metres from the edge's first point
    int edge = -1;
};
struct WalkEdge {
    int a = -1, b = -1;       // vertices
    uint32_t first = 0, count = 0; // its points in SidewalkGraph::pts, a to b
    float len = 0;
    WalkEdgeKind kind = WalkEdgeKind::Along;
    int seg = -1, node = -1;
    uint8_t group = 0;        // crossings: which half of the junction's signal cycle they walk in
    bool signal = false;      // crossings: wait for the walk phase (not on paths)
};
struct WalkVert {
    V2 p{};
    float y = 0;
    uint32_t adjFirst = 0, adjCount = 0;
    bool used = false;
};

class SidewalkGraph {
public:
    std::vector<WalkVert> verts; // 4 per segment: (end a / b) x (left / right side, looking from a to b)
    std::vector<WalkEdge> edges;
    std::vector<WalkPt> pts;
    std::vector<int> adj;        // edges around each vertex (CSR, WalkVert::adjFirst)
    uint64_t builtFrom = ~0ull;  // the network version

    static int Vid(int seg, int end, int side) { return seg * 4 + end * 2 + side; }
    // Pedestrians on this profile walk anywhere across it (paths, pedestrian streets): no cars.
    static bool FreeProfile(const Profile& p) {
        if (p.cat == Cat::Path) return true;
        for (const Strip& s : p.strips)
            if (s.kind == Kind::Path) return true;
        return false;
    }
    // The walking line of one side (0 left, 1 right looking from a to b): its distance from the centre line.
    static bool SideWalk(const Profile& p, int side, float& off) {
        if (p.cat == Cat::Rail || p.cat == Cat::Highway || p.strips.empty()) return false;
        for (const Strip& s : p.strips)
            if (s.kind == Kind::Path) { off = s.w * 0.25f; return true; }
        if (p.cat == Cat::Path) return false; // bicycle paths
        const Strip& e = side == 0 ? p.strips.front() : p.strips.back();
        if (e.kind != Kind::Sidewalk) return false;
        off = p.Half() - e.w * 0.5f;
        return true;
    }
    // Cars drive within this distance of the centre line on that side; < 0: no cars on the profile.
    static float CarriageHalf(const Profile& p, int side) {
        if (FreeProfile(p) || p.cat == Cat::Rail) return -1.0f;
        float band = 0;
        if (side == 0) for (size_t i = 0; i < p.strips.size() && (detail::Side(p.strips[i].kind)); ++i) band += p.strips[i].w;
        else for (size_t i = p.strips.size(); i-- > 0 && detail::Side(p.strips[i].kind);) band += p.strips[i].w;
        return p.Half() - band;
    }

    void Build(const Network& net) {
        verts.assign(net.segs.size() * 4, WalkVert{});
        edges.clear(), pts.clear(), adj.clear();
        builtFrom = net.version;
        std::vector<std::vector<Arm>> junctionArms;
        const std::vector<SegTrim> trims = ComputeTrims(net, &junctionArms);
        offs_.assign(net.segs.size() * 2, -1.0f);
        // ---- along each side of each street
        std::vector<WalkPt> line;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive) continue;
            const Profile& p = net.Prof((int)s);
            const SegTrim& tr = trims[s];
            const Cubic c = net.Curve((int)s);
            const float L = c.Length();
            const bool free = FreeProfile(p);
            for (int side = 0; side < 2; ++side) {
                float off;
                if (!SideWalk(p, side, off)) continue;
                offs_[s * 2 + side] = off;
                const int n = std::clamp((int)(L * (tr.t1 - tr.t0) / 4.0f) + 1, 2, 96);
                line.clear();
                for (int i = 0; i <= n; ++i) {
                    const float t = tr.t0 + (tr.t1 - tr.t0) * (float)i / (float)n;
                    V2 nrm = LeftOf(c.Tan(t));
                    if (i == 0 && tr.miterA) nrm = tr.leftA;
                    if (i == n && tr.miterB) nrm = tr.leftB;
                    WalkPt q;
                    q.p = c.At(t) + nrm * (side == 0 ? off : -off);
                    q.y = net.YAt(net.segs[s], t) + (free ? walk::kPathLift : walk::kSideLift);
                    line.push_back(q);
                }
                const int va = Vid((int)s, 0, side), vb = Vid((int)s, 1, side);
                SetVert(va, line.front()), SetVert(vb, line.back());
                AddEdge(va, vb, WalkEdgeKind::Along, line, (int)s, -1);
            }
        }
        // ---- at the nodes: corners, links, caps, crossings
        for (size_t n = 0; n < net.nodes.size(); ++n) {
            if (!net.nodes[n].alive || net.IsRailNode((int)n)) continue;
            const bool junction = !junctionArms[n].empty();
            const std::vector<Arm> arms = junction ? junctionArms[n] : ArmsAt(net, (int)n);
            const size_t k = arms.size();
            if (k == 0) continue;
            const V2 C = XZ(net.nodes[n].p);
            const float yNode = net.NodeY((int)n);
            if (k == 1) { // a dead end: round the cap from the arm's left to its right
                const Arm& a = arms[0];
                const int l = ArmVid(a, true), r = ArmVid(a, false);
                if (!Walkable(l) || !Walkable(r)) continue;
                const float offL = Off(l), offR = Off(r);
                const V2 left = LeftOf(a.u), back = a.u * -1.0f;
                line.clear();
                const int m = 12;
                for (int j = 0; j <= m; ++j) {
                    const float ang = 3.14159265f * (float)j / (float)m, f = (float)j / (float)m;
                    const float rr = offL + (offR - offL) * f;
                    WalkPt q;
                    q.p = j == 0 ? verts[l].p : j == m ? verts[r].p : C + left * (std::cos(ang) * rr) + back * (std::sin(ang) * rr);
                    q.y = verts[l].y + (verts[r].y - verts[l].y) * f;
                    line.push_back(q);
                }
                AddEdge(l, r, WalkEdgeKind::Cap, line, a.seg, (int)n);
                continue;
            }
            if (!junction) { // a seamless bend (or a straight joint): the sides run on
                for (size_t i = 0; i < k; ++i) {
                    const Arm& a = arms[i];
                    const Arm& b = arms[(i + 1) % k];
                    const int ra = ArmVid(a, false), lb = ArmVid(b, true);
                    if (!Walkable(ra) || !Walkable(lb)) continue;
                    line.clear();
                    line.push_back(Pt(ra)), line.push_back(Pt(lb));
                    AddEdge(ra, lb, WalkEdgeKind::Link, line, -1, (int)n);
                }
                for (const Arm& a : arms) AddFreeCrossing(net, a, (int)n, line);
                continue;
            }
            // a junction: the corner band between neighbouring arms (arm a's right side faces arm b's left)
            for (size_t i = 0; i < k; ++i) {
                const Arm& a = arms[i];
                const Arm& b = arms[(i + 1) % k];
                const int ra = ArmVid(a, false), lb = ArmVid(b, true);
                if (!Walkable(ra) || !Walkable(lb)) continue;
                const V2 pa = verts[ra].p, pb = verts[lb].p;
                V2 q;
                const float maxR = 3.0f * std::max(a.hw, b.hw) + 10.0f;
                if (!LineHit(pa, a.u, pb, b.u, q) || Len(q - C) > maxR || Dot2(q - pa, a.u) > 0.5f) q = (pa + pb) * 0.5f; // as BuildJunction
                line.clear();
                const int m = 8;
                for (int j = 0; j <= m; ++j) {
                    const float t = (float)j / (float)m, u = 1 - t;
                    WalkPt w;
                    w.p = j == 0 ? pa : j == m ? pb : pa * (u * u) + q * (2 * u * t) + pb * (t * t);
                    w.y = verts[ra].y + (verts[lb].y - verts[ra].y) * t;
                    line.push_back(w);
                }
                AddEdge(ra, lb, WalkEdgeKind::Corner, line, -1, (int)n);
            }
            for (size_t i = 0; i < k; ++i) {
                const Arm& a = arms[i];
                const int l = ArmVid(a, true), r = ArmVid(a, false);
                if (!Walkable(l) || !Walkable(r)) continue;
                const bool free = FreeProfile(net.Prof(a.seg));
                if (free) { AddFreeCrossing(net, a, (int)n, line); continue; }
                if (k < 3) continue; // a sharp bend: no crosswalk painted
                const float yr = yNode + walk::kRoadLift;
                line.clear();
                WalkPt w;
                line.push_back(Pt(l));
                w.p = verts[l].p + a.u * walk::kCrossAt, w.y = yr, line.push_back(w);
                w.p = verts[r].p + a.u * walk::kCrossAt, w.y = yr, line.push_back(w);
                line.push_back(Pt(r));
                const int e = AddEdge(l, r, WalkEdgeKind::Crossing, line, a.seg, (int)n);
                edges[(size_t)e].signal = true, edges[(size_t)e].group = (uint8_t)(i % 2);
            }
        }
        // ---- adjacency
        std::vector<uint32_t> deg(verts.size(), 0);
        for (const WalkEdge& e : edges) ++deg[(size_t)e.a], ++deg[(size_t)e.b];
        uint32_t at = 0;
        for (size_t v = 0; v < verts.size(); ++v) verts[v].adjFirst = at, verts[v].adjCount = 0, at += deg[v];
        adj.assign(at, -1);
        for (size_t e = 0; e < edges.size(); ++e) {
            for (int v : {edges[e].a, edges[e].b}) {
                WalkVert& w = verts[(size_t)v];
                adj[w.adjFirst + w.adjCount++] = (int)e;
            }
        }
        BuildGrid();
    }

    // The point s metres along edge e (from its a end), its height and the edge's direction there.
    void At(int e, float s, V2& p, float& y, V2& dir) const {
        const WalkEdge& E = edges[(size_t)e];
        const WalkPt* q = &pts[E.first];
        s = std::clamp(s, 0.0f, E.len);
        uint32_t lo = 0, hi = E.count - 1; // q[lo].s <= s < q[hi].s
        while (hi - lo > 1) {
            const uint32_t mid = (lo + hi) / 2;
            if (q[mid].s <= s) lo = mid;
            else hi = mid;
        }
        const float span = q[hi].s - q[lo].s;
        const float f = span > 1e-5f ? (s - q[lo].s) / span : 0.0f;
        p = Lerp2(q[lo].p, q[hi].p, f);
        y = q[lo].y + (q[hi].y - q[lo].y) * f;
        const V2 d = q[hi].p - q[lo].p;
        if (Len(d) > 1e-5f) dir = Norm(d);
    }

    // The nearest point on any sidewalk (Along edge) within maxD of q: edge, metres along it, the point.
    bool Nearest(V2 q, float maxD, int& edge, float& s, V2& at, float& y) const {
        if (gridW_ <= 0) return false;
        float best = maxD * maxD;
        edge = -1;
        const int x0 = std::clamp((int)std::floor((q.x - maxD - gx_) / kGrid), 0, gridW_ - 1), x1 = std::clamp((int)std::floor((q.x + maxD - gx_) / kGrid), 0, gridW_ - 1);
        const int z0 = std::clamp((int)std::floor((q.z - maxD - gz_) / kGrid), 0, gridH_ - 1), z1 = std::clamp((int)std::floor((q.z + maxD - gz_) / kGrid), 0, gridH_ - 1);
        for (int gz = z0; gz <= z1; ++gz)
            for (int gx = x0; gx <= x1; ++gx) {
                const size_t c = (size_t)gz * (size_t)gridW_ + (size_t)gx;
                for (uint32_t k = gridStart_[c]; k < gridStart_[c + 1]; ++k) {
                    const uint32_t i = gridItems_[k]; // the piece pts[i] .. pts[i + 1] of an Along edge
                    const WalkPt& a = pts[i];
                    const WalkPt& b = pts[i + 1];
                    const V2 d = b.p - a.p;
                    const float l2 = Dot2(d, d);
                    const float t = l2 > 1e-8f ? std::clamp(Dot2(q - a.p, d) / l2, 0.0f, 1.0f) : 0.0f;
                    const V2 p = a.p + d * t;
                    const float d2 = Dot2(p - q, p - q);
                    if (d2 < best) best = d2, edge = a.edge, s = a.s + (b.s - a.s) * t, at = p, y = a.y + (b.y - a.y) * t;
                }
            }
        return edge >= 0;
    }
    float SideOffset(int seg, int side) const { return (size_t)(seg * 2 + side) < offs_.size() ? offs_[(size_t)(seg * 2 + side)] : -1.0f; }

private:
    static constexpr float kGrid = 16.0f;
    std::vector<float> offs_; // per segment side: the walking line's distance from the centre line (-1: no sidewalk)
    float gx_ = 0, gz_ = 0;
    int gridW_ = 0, gridH_ = 0;
    std::vector<uint32_t> gridStart_, gridItems_;

    static int ArmVid(const Arm& a, bool left) { // the arm's left / right looking outward, as a segment side
        const int end = a.atA ? 0 : 1;
        const int side = a.atA ? (left ? 0 : 1) : (left ? 1 : 0);
        return Vid(a.seg, end, side);
    }
    bool Walkable(int v) const { return v >= 0 && (size_t)v < verts.size() && verts[(size_t)v].used; }
    float Off(int v) const { return offs_[(size_t)(v / 4) * 2 + (size_t)(v % 2)]; }
    WalkPt Pt(int v) const { WalkPt q; q.p = verts[(size_t)v].p, q.y = verts[(size_t)v].y; return q; }
    void SetVert(int v, const WalkPt& q) { verts[(size_t)v].p = q.p, verts[(size_t)v].y = q.y, verts[(size_t)v].used = true; }
    int AddEdge(int a, int b, WalkEdgeKind kind, std::vector<WalkPt>& line, int seg, int node) {
        WalkEdge e;
        e.a = a, e.b = b, e.kind = kind, e.seg = seg, e.node = node;
        e.first = (uint32_t)pts.size(), e.count = (uint32_t)line.size();
        const int id = (int)edges.size();
        float s = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (i) s += Len(line[i].p - line[i - 1].p);
            line[i].s = s, line[i].edge = id;
            pts.push_back(line[i]);
        }
        if (line.size() < 2) { // a joint with nothing between: keep two points so At works
            WalkPt q = line.empty() ? WalkPt{} : line[0];
            q.edge = id;
            while (e.count < 2) pts.push_back(q), ++e.count;
        }
        e.len = s;
        edges.push_back(e);
        return id;
    }
    void AddFreeCrossing(const Network& net, const Arm& a, int n, std::vector<WalkPt>& line) {
        if (!FreeProfile(net.Prof(a.seg))) return;
        const int l = ArmVid(a, true), r = ArmVid(a, false);
        if (!Walkable(l) || !Walkable(r)) return;
        line.clear();
        line.push_back(Pt(l)), line.push_back(Pt(r));
        AddEdge(l, r, WalkEdgeKind::Crossing, line, a.seg, n);
    }
    void BuildGrid() {
        gridW_ = gridH_ = 0;
        float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        for (const WalkPt& q : pts) x0 = std::min(x0, q.p.x), z0 = std::min(z0, q.p.z), x1 = std::max(x1, q.p.x), z1 = std::max(z1, q.p.z);
        if (pts.empty()) return;
        gx_ = x0 - kGrid, gz_ = z0 - kGrid;
        gridW_ = std::min(4096, (int)((x1 - gx_) / kGrid) + 2), gridH_ = std::min(4096, (int)((z1 - gz_) / kGrid) + 2);
        const size_t cells = (size_t)gridW_ * (size_t)gridH_;
        gridStart_.assign(cells + 1, 0);
        auto cellOf = [&](V2 p) {
            const int cx = std::clamp((int)((p.x - gx_) / kGrid), 0, gridW_ - 1), cz = std::clamp((int)((p.z - gz_) / kGrid), 0, gridH_ - 1);
            return (size_t)cz * (size_t)gridW_ + (size_t)cx;
        };
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uint32_t> fill;
            if (pass) {
                for (size_t c = 0; c < cells; ++c) gridStart_[c + 1] += gridStart_[c];
                gridItems_.assign(gridStart_[cells], 0);
                fill.assign(gridStart_.begin(), gridStart_.end() - 1);
            }
            for (const WalkEdge& e : edges) {
                if (e.kind != WalkEdgeKind::Along) continue;
                for (uint32_t i = e.first; i + 1 < e.first + e.count; ++i) {
                    const size_t c = cellOf((pts[i].p + pts[i + 1].p) * 0.5f); // pieces are at most ~4 m: their middle's cell
                    if (!pass) ++gridStart_[c + 1];
                    else gridItems_[fill[c]++] = i;
                }
            }
        }
    }
};

// ------------------------------------------------------------------------------------------------ doors and trips
enum class DoorKind : uint8_t { Home, Shop, Work, School, Station, Service, Stop, Count };
inline const char* DoorKindName(DoorKind k) {
    static const char* const n[] = {"home", "shop", "workplace", "school", "station", "service building", "stop"};
    return n[(int)k];
}
struct WalkDoor {
    uint64_t id = 0;     // stable: the host's lot / building / stop id with a kind tag
    V2 p{}, out{0, 1};   // the door on the street face; out points from the building to the street
    float y = 0;
    DoorKind kind = DoorKind::Home;
    float people = 0;    // residents (homes), workers (workplaces), places (schools), passengers (stations)
    // set by WalkerSim::SetDoors
    int edge = -1;
    float s = 0;
    V2 at{};             // where it meets the sidewalk
    float atY = 0;
};

enum class WalkPurpose : uint8_t { ToWork, Home, Shopping, School, Visit, ToStop, Lunch, Errand, Count };
inline const char* WalkPurposeName(WalkPurpose p) {
    static const char* const n[] = {"walking to work", "going home", "shopping", "walking to school", "visiting a friend",
                                    "walking to the stop", "lunch break", "running an errand"};
    return n[(int)p];
}
// How likely a trip of this purpose is at an hour of the day (0..1).
inline float PurposeCurve(WalkPurpose p, float h) {
    using walk::Bell;
    using walk::Day;
    float v = 0;
    switch (p) {
    case WalkPurpose::ToWork: v = 0.04f + Bell(h, 8.0f, 1.0f) + 0.15f * Day(h, 9, 16); break;
    case WalkPurpose::Home: v = 0.04f + Bell(h, 17.5f, 1.3f) + 0.35f * Day(h, 10, 21); break;
    case WalkPurpose::Shopping: v = 0.02f + 0.75f * Day(h, 9, 20) + 0.25f * Bell(h, 18.0f, 1.5f); break;
    case WalkPurpose::School: v = 0.01f + Bell(h, 7.9f, 0.6f) + 0.9f * Bell(h, 15.3f, 0.7f); break;
    case WalkPurpose::Visit: v = 0.02f + 0.75f * Bell(h, 19.0f, 2.0f) + 0.2f * Day(h, 10, 18); break;
    case WalkPurpose::ToStop: v = 0.03f + 0.7f * Bell(h, 7.8f, 1.2f) + 0.6f * Bell(h, 17.3f, 1.5f) + 0.3f * Day(h, 9, 21); break;
    case WalkPurpose::Lunch: v = 0.01f + Bell(h, 12.5f, 0.8f); break;
    case WalkPurpose::Errand: v = 0.02f + 0.5f * Day(h, 9, 17); break;
    default: break;
    }
    return std::min(1.0f, v);
}
// What a trip out of a door of this kind is for: (purpose, weight) pairs summing to one.
struct PurposeMix {
    WalkPurpose p[4];
    float w[4];
    int n;
};
inline const PurposeMix& MixOf(DoorKind k) {
    using P = WalkPurpose;
    static const PurposeMix m[] = {
        {{P::ToWork, P::Shopping, P::Visit, P::ToStop}, {0.36f, 0.30f, 0.14f, 0.20f}, 4}, // Home (the school run: Home's School below)
        {{P::Home, P::Shopping, P::ToStop, P::Home}, {0.70f, 0.15f, 0.15f, 0.0f}, 3},   // Shop: customers leave
        {{P::Lunch, P::Home, P::Errand, P::ToStop}, {0.40f, 0.40f, 0.08f, 0.12f}, 4},   // Work
        {{P::Home, P::Shopping, P::Home, P::Home}, {0.85f, 0.15f, 0, 0}, 2},             // School
        {{P::Home, P::ToWork, P::Shopping, P::Home}, {0.55f, 0.35f, 0.10f, 0}, 3},       // Station
        {{P::Home, P::Errand, P::Lunch, P::Home}, {0.60f, 0.20f, 0.20f, 0}, 3},          // Service
        {{P::Home, P::Home, P::Home, P::Home}, {0, 0, 0, 0}, 0},                         // Stop: sends nobody out
    };
    return m[std::min((int)k, (int)DoorKind::Count - 1)];
}
// A home's trips also include the school run (morning) on a share of them.
constexpr float kSchoolShare = 0.10f;
// Where a purpose goes (kinds that qualify).
inline bool PurposeGoesTo(WalkPurpose p, DoorKind k) {
    switch (p) {
    case WalkPurpose::ToWork: return k == DoorKind::Work || k == DoorKind::Shop || k == DoorKind::Service || k == DoorKind::Station;
    case WalkPurpose::Home: case WalkPurpose::Visit: return k == DoorKind::Home;
    case WalkPurpose::Shopping: case WalkPurpose::Lunch: return k == DoorKind::Shop;
    case WalkPurpose::School: return k == DoorKind::School;
    case WalkPurpose::ToStop: return k == DoorKind::Stop;
    case WalkPurpose::Errand: return k == DoorKind::Service || k == DoorKind::Shop || k == DoorKind::Work;
    default: return false;
    }
}

enum class WalkLegKind : uint8_t { Door, Along, Corner, Link, Cap, Crossing, Stop };
struct WalkLeg {
    int edge = -1;        // >= 0: along this graph edge from s0 to s1 (s1 < s0: walked backwards); -1: straight a -> b
    float s0 = 0, s1 = 0;
    V2 a{}, b{};
    float ya = 0, yb = 0;
    float t0 = 0;         // seconds after the trip's start at which this leg's walking begins (they stand until then)
    float len = 0;        // metres
    float walked = 0;     // the trip's metres before this leg
    WalkLegKind kind = WalkLegKind::Door;
};
constexpr int kWalkMaxLegs = 56;

struct Walker {
    uint64_t id = 0;                 // the trip (and the person on it)
    int from = -1, to = -1, via = -1; // door indices at planning (the ids below survive a door list rebuild)
    uint64_t fromId = 0, toId = 0, viaId = 0;
    WalkPurpose purpose = WalkPurpose::Home;
    uint8_t look = 0;                // body / clothes variant
    float speed = 1.3f;              // metres per second
    float scale = 1.0f;              // body height factor
    float side = 0.4f;               // keeps this far right of the walking line (two-way sidewalks pass)
    double start = 0;                // walk-clock second the trip begins (stepping out of the door)
    float dur = 0;                   // seconds until they are inside the destination
    float length = 0;                // metres
    int nLegs = 0;
    int stopAt = -1;                 // they wait at the stop before this leg (-1: no stop on the way)
    WalkLeg legs[kWalkMaxLegs];
    // where they are (Evaluate)
    V2 pos{}, dir{0, 1};
    V2 line{};                       // pos before the keep-right shift: on the walking line
    float y = 0;
    float walked = 0;                // metres walked so far: the walk cycle's phase is walked / stride
    bool moving = false;
    int cur = 0;
    WalkLegKind legKind = WalkLegKind::Door;
};

struct WalkView { // the camera, for "is it on screen" (nobody pops into view)
    Mat4 viewProj = Mat4::Identity();
    Vec3 eye{};
    float range = 220.0f; // nothing is drawn further than this from the eye
    bool valid = false;
    bool InView(V2 p, float y, float margin = 0.08f) const {
        if (!valid) return false;
        const Vec3 d{p.x - eye.x, y + 0.9f - eye.y, p.z - eye.z};
        if (Dot(d, d) > range * range) return false;
        const Vec4 c = viewProj * Vec4{p.x, y + 0.9f, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        const float lim = 1.0f + margin;
        return std::fabs(c.x / c.w) <= lim && std::fabs(c.y / c.w) <= lim;
    }
};

struct WalkParams {
    int maxWalkers = 320;        // the hard cap on materialized walkers
    float window = 60.0f;        // seconds per schedule window
    float maxTrip = 420.0f;      // seconds: longer trips are not made
    float destRadius = 260.0f;   // straight-line metres to a destination at most
    float rate = 1.0f;           // scales every door's trip rate
    int plansPerFrame = 8;       // route searches per Update at most
    float scanEvery = 0.25f;     // seconds of walk clock between door scans
    float doorGrace = 1.6f;      // a trip materialized this soon after its start is still at its door
    float stepOut = 0.8f;        // seconds they stand at the door before walking off
    float stepIn = 0.4f;         // seconds at the destination door before they are inside
    uint64_t seed = 0x5EED;
};
// Trips per person per second (at the most likely hour), by door kind. Calibrated for about 5 % of residents and
// 6 % of workers on foot at their peaks with the ~3 minute walks the destination radius gives.
inline float DoorRate(DoorKind k) {
    static const float r[] = {5.5e-4f, 1.0e-3f, 7.5e-4f, 1.0e-3f, 2.5e-3f, 7.5e-4f, 0.0f};
    return r[std::min((int)k, (int)DoorKind::Count - 1)];
}

class WalkerSim {
public:
    SidewalkGraph graph;
    std::vector<WalkDoor> doors;
    std::vector<Walker> walkers;     // materialized now (pool: never more than params.maxWalkers)
    WalkParams params;
    float keep = 1.0f;               // the share of scheduled trips shown (eases off when the disk would hold more than the cap)
    float expected = 0;              // walkers the schedule puts in the disk now (before keep)
    double now = 0;
    float hour = 12;
    // counters (for tests and the HUD)
    uint64_t planned = 0, suppressed = 0, failedPlans = 0, finished = 0, dropped = 0, spawnedAtDoor = 0;

    WalkerSim() {
        cache_.assign(kCache, CacheEntry{});
        pending_.reserve(2048);
    }

    // The roads changed. The walkers out now keep walking: each one walks on from where it is to its destination over the new
    // sidewalks (nobody vanishes because a street was built elsewhere); the ones with no way left go.
    void RebuildGraph(const Network& net) {
        graph.Build(net);
        AttachDoors();
        size_t kept = 0;
        for (size_t i = 0; i < walkers.size(); ++i) {
            if (Reroute(walkers[i])) {
                if (kept != i) walkers[kept] = walkers[i];
                ++kept;
            } else Forget(walkers[i].id);
        }
        walkers.resize(kept);
    }
    // The host's door list (rebuilt when lots or buildings change). Walkers keep their routes; their door ids still name them.
    void SetDoors(const std::vector<WalkDoor>& list) {
        doors = list;
        AttachDoors();
    }

    // One step: time T (walk-clock seconds, only ever increasing while the game runs), the game clock's hour, the
    // disk around the camera that may hold walkers, and the view (for the pop rule).
    void Update(double T, float hourOfDay, V2 centre, float radius, const WalkView& view) {
        now = T, hour = hourOfDay;
        centre_ = centre, radius_ = radius;
        if (walkers.capacity() < (size_t)params.maxWalkers) walkers.reserve((size_t)params.maxWalkers);
        // ---- the walkers we have: move, finish, drop those far outside the disk
        for (size_t i = 0; i < walkers.size();) {
            Walker& w = walkers[i];
            Evaluate(w, T);
            const bool done = T - w.start >= (double)w.dur;
            const bool tooFar = Len(w.pos - centre) > radius + 40.0f && !view.InView(w.pos, w.y);
            if (done || tooFar) {
                if (done) ++finished, Remember(w.id, w.start + params.maxTrip, false);
                else ++dropped, Forget(w.id); // it may come back when the camera does
                walkers[i] = walkers.back();
                walkers.pop_back();
                continue;
            }
            ++i;
        }
        if (graph.edges.empty() || doors.empty() || radius <= 1.0f) { pending_.clear(); return; }
        // ---- the schedule: which trips run in the disk now
        if (T - lastScan_ >= params.scanEvery || T < lastScan_) {
            lastScan_ = T;
            Scan(T);
        }
        // ---- routes for the newest pending trips (the ones just stepping out first)
        int budget = params.plansPerFrame;
        while (budget > 0 && !pending_.empty() && (int)walkers.size() < params.maxWalkers) {
            const Pending p = pending_.back();
            pending_.pop_back();
            if (Known(p.id)) continue;
            --budget;
            Walker w;
            if (!Plan(p.id, p.door, p.start, w)) { ++failedPlans; Remember(p.id, p.start + params.maxTrip, false); continue; }
            ++planned;
            if (T - w.start >= (double)w.dur) { Remember(p.id, p.start + params.maxTrip, false); continue; } // already home
            Evaluate(w, T);
            const bool atDoor = T - w.start <= (double)params.doorGrace;
            if (!atDoor && view.InView(w.pos, w.y)) { ++suppressed; Remember(p.id, p.start + params.maxTrip, false); continue; }
            if (atDoor) ++spawnedAtDoor;
            Remember(p.id, p.start + params.maxTrip, true);
            meanDur_ += (w.dur - meanDur_) * 0.02f;
            walkers.push_back(w);
        }
    }

    // Where a walker is at time T. Pure: the same (trip, T) gives the same answer however the walker got there.
    void Evaluate(Walker& w, double T) const {
        const float rel = (float)(T - w.start);
        if (w.nLegs <= 0) return;
        if (w.cur >= w.nLegs || w.cur < 0 || rel < w.legs[w.cur].t0) w.cur = 0;
        while (w.cur + 1 < w.nLegs && rel >= w.legs[w.cur + 1].t0) ++w.cur;
        const WalkLeg& L = w.legs[w.cur];
        w.legKind = L.kind;
        const float d = rel < L.t0 ? 0.0f : std::min((rel - L.t0) * w.speed, L.len);
        w.moving = rel >= L.t0 && (rel - L.t0) * w.speed < L.len;
        w.walked = L.walked + d;
        V2 travel = w.dir;
        LegAt(L, d, w.pos, w.y, travel);
        w.line = w.pos;
        w.pos = w.pos + Offset(w, w.cur, d, travel); // keep right of the walking line, eased across every change of leg
        w.dir = travel;
        if (!w.moving && rel >= L.t0 && w.cur + 1 < w.nLegs) { // waiting at the leg's end (a red crossing, the stop): face the way on
            V2 p2, d2 = travel;
            float y2;
            const WalkLeg& N = w.legs[w.cur + 1];
            LegAt(N, std::min(N.len, 0.5f), p2, y2, d2);
            w.dir = d2;
            if (N.kind == WalkLegKind::Crossing) w.legKind = WalkLegKind::Crossing;
            if (w.cur + 1 == w.stopAt) w.legKind = WalkLegKind::Stop;
        }
    }
    // The sideways shift at d metres into leg i: right of the travel direction by w.side (none on door legs), blended over the
    // first two metres of a leg from where the leg before left off, so turning onto a crossing never jumps.
    V2 Offset(const Walker& w, int i, float d, V2 travel) const {
        auto own = [&](int k, V2 dir) { return w.legs[k].kind == WalkLegKind::Door ? V2{0, 0} : RightOf(dir) * w.side; };
        const V2 cur = own(i, travel);
        if (d >= 2.0f || i == 0) return cur;
        int k = i - 1;
        while (k > 0 && w.legs[k].len < 0.5f) --k; // past zero-length joints
        V2 p, dir = travel;
        float y;
        LegAt(w.legs[k], w.legs[k].len, p, y, dir);
        const V2 prev = own(k, dir);
        const float f = d * 0.5f, s = f * f * (3.0f - 2.0f * f);
        return prev + (cur - prev) * s;
    }

    // Is this walker on a crossing (in the carriageway, legally) right now?
    static bool OnCrossing(const Walker& w) { return w.legKind == WalkLegKind::Crossing && w.moving; }
    const Walker* Find(uint64_t id) const {
        for (const Walker& w : walkers)
            if (w.id == id) return &w;
        return nullptr;
    }
    int DoorIndex(uint64_t id) const {
        for (size_t i = 0; i < doors.size(); ++i)
            if (doors[i].id == id) return (int)i;
        return -1;
    }
    // Plans a trip's walk without materializing it (tests: the same trip planned twice must give the same walk).
    bool PlanTrip(uint64_t id, int door, double start, Walker& out) const { return Plan(id, door, start, out); }
    // The trips the schedule has door `d` start in window k (ids and starts), at density 1 (before keep and the hour).
    int TripsInWindow(int d, int64_t k, uint64_t* ids, double* starts, int cap) const {
        const WalkDoor& D = doors[(size_t)d];
        const float lambda = D.people * DoorRate(D.kind) * params.rate * params.window;
        const int n = std::min(cap, std::min(24, (int)std::floor(lambda + walk::U01(walk::Hash(D.id, (uint64_t)k, params.seed ^ 0x51)))));
        for (int j = 0; j < n; ++j) {
            ids[j] = walk::Hash(D.id ^ params.seed, (uint64_t)k, (uint64_t)j + 1);
            starts[j] = ((double)k + (double)walk::U01(walk::Hash(ids[j], 2))) * params.window;
        }
        return n;
    }
    // The purpose a trip out of door kind `k` has: from its id alone.
    static WalkPurpose PurposeOf(uint64_t id, DoorKind k) {
        const PurposeMix& m = MixOf(k);
        float u = walk::U01(walk::Hash(id, 5));
        if (k == DoorKind::Home && walk::U01(walk::Hash(id, 6)) < kSchoolShare) return WalkPurpose::School;
        for (int i = 0; i < m.n; ++i) {
            if (u < m.w[i]) return m.p[i];
            u -= m.w[i];
        }
        return m.n > 0 ? m.p[m.n - 1] : WalkPurpose::Home;
    }
    // Is the trip scheduled to happen at this hour (before the cap's thinning)?
    bool Exists(uint64_t id, DoorKind k, float hourOfDay, float keepShare) const {
        return walk::U01(walk::Hash(id, 1)) < keepShare * PurposeCurve(PurposeOf(id, k), hourOfDay);
    }

private:
    struct CacheEntry {
        uint64_t id = 0;
        double expiry = -1;
        bool alive = false;
    };
    static constexpr size_t kCache = 8192;
    std::vector<CacheEntry> cache_; // trips already decided: walking now (alive), done, refused or hidden (until expiry)
    struct Pending {
        uint64_t id;
        int door;
        double start;
    };
    std::vector<Pending> pending_;
    double lastScan_ = -1e30;
    V2 centre_{};
    float radius_ = 0;
    float meanDur_ = 170.0f;
    // A* buffers (sized to the graph, reused)
    mutable std::vector<float> g_;
    mutable std::vector<int> prevEdge_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t stampNow_ = 0;
    mutable std::vector<std::pair<float, int>> heap_;
    mutable std::vector<int> chain_;
    mutable std::vector<int> cand_;
    // door grid
    static constexpr float kDoorGrid = 32.0f;
    float dgx_ = 0, dgz_ = 0;
    int dgW_ = 0, dgH_ = 0;
    std::vector<uint32_t> dStart_, dItems_;

    size_t Slot(uint64_t id) const { return (size_t)(walk::Mix(id) & (kCache - 1)); }
    bool Known(uint64_t id) const {
        size_t s = Slot(id);
        for (int k = 0; k < 8; ++k, s = (s + 1) & (kCache - 1))
            if (cache_[s].id == id && (cache_[s].alive || cache_[s].expiry >= now)) return true;
        return false;
    }
    void Remember(uint64_t id, double expiry, bool alive) {
        size_t s = Slot(id), pick = s;
        double oldest = 1e300;
        for (int k = 0; k < 8; ++k, s = (s + 1) & (kCache - 1)) {
            CacheEntry& e = cache_[s];
            if (e.id == id) { pick = s; break; }
            if (!e.alive && e.expiry < oldest) oldest = e.expiry, pick = s; // a free or expired slot, or the soonest to expire
        }
        cache_[pick] = {id, expiry, alive};
    }
    void Forget(uint64_t id) {
        size_t s = Slot(id);
        for (int k = 0; k < 8; ++k, s = (s + 1) & (kCache - 1))
            if (cache_[s].id == id) { cache_[s] = CacheEntry{}; return; }
    }

    void AttachDoors() {
        for (WalkDoor& d : doors) {
            d.edge = -1;
            const V2 probe = d.p + d.out * 1.0f;
            float y;
            if (graph.Nearest(probe, d.kind == DoorKind::Stop ? 14.0f : 18.0f, d.edge, d.s, d.at, y)) d.atY = y;
        }
        // the door grid (cells of kDoorGrid metres) over the attached doors
        dgW_ = dgH_ = 0;
        float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        for (const WalkDoor& d : doors) if (d.edge >= 0) x0 = std::min(x0, d.p.x), z0 = std::min(z0, d.p.z), x1 = std::max(x1, d.p.x), z1 = std::max(z1, d.p.z);
        if (x0 > x1) { dStart_.assign(1, 0), dItems_.clear(); return; }
        dgx_ = x0 - 1.0f, dgz_ = z0 - 1.0f;
        dgW_ = std::min(2048, (int)((x1 - dgx_) / kDoorGrid) + 1), dgH_ = std::min(2048, (int)((z1 - dgz_) / kDoorGrid) + 1);
        const size_t cells = (size_t)dgW_ * (size_t)dgH_;
        dStart_.assign(cells + 1, 0);
        for (const WalkDoor& d : doors) if (d.edge >= 0) ++dStart_[DoorCell(d.p) + 1];
        for (size_t c = 0; c < cells; ++c) dStart_[c + 1] += dStart_[c];
        dItems_.assign(dStart_[cells], 0);
        std::vector<uint32_t> fill(dStart_.begin(), dStart_.end() - 1);
        for (size_t i = 0; i < doors.size(); ++i) if (doors[i].edge >= 0) dItems_[fill[DoorCell(doors[i].p)]++] = (uint32_t)i;
        g_.assign(graph.verts.size(), 0.0f), prevEdge_.assign(graph.verts.size(), -1), stamp_.assign(graph.verts.size(), 0), stampNow_ = 0;
    }
    size_t DoorCell(V2 p) const {
        const int cx = std::clamp((int)((p.x - dgx_) / kDoorGrid), 0, dgW_ - 1), cz = std::clamp((int)((p.z - dgz_) / kDoorGrid), 0, dgH_ - 1);
        return (size_t)cz * (size_t)dgW_ + (size_t)cx;
    }
    template <class F> void ForDoorsNear(V2 c, float r, F&& f) const {
        if (dgW_ <= 0) return;
        const int x0 = std::clamp((int)std::floor((c.x - r - dgx_) / kDoorGrid), 0, dgW_ - 1), x1 = std::clamp((int)std::floor((c.x + r - dgx_) / kDoorGrid), 0, dgW_ - 1);
        const int z0 = std::clamp((int)std::floor((c.z - r - dgz_) / kDoorGrid), 0, dgH_ - 1), z1 = std::clamp((int)std::floor((c.z + r - dgz_) / kDoorGrid), 0, dgH_ - 1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const size_t cell = (size_t)z * (size_t)dgW_ + (size_t)x;
                for (uint32_t k = dStart_[cell]; k < dStart_[cell + 1]; ++k) {
                    const int i = (int)dItems_[k];
                    if (Len(doors[(size_t)i].p - c) <= r) f(i);
                }
            }
    }

    // Which trips of the doors in the disk exist now and are not decided yet: the pending list (newest last).
    void Scan(double T) {
        pending_.clear();
        float sumExpected = 0;
        const int64_t kHi = (int64_t)std::floor(T / params.window), kLo = (int64_t)std::floor((T - params.maxTrip) / params.window);
        ForDoorsNear(centre_, radius_, [&](int d) {
            const WalkDoor& D = doors[(size_t)d];
            if (D.people <= 0 || D.kind == DoorKind::Stop) return;
            const PurposeMix& m = MixOf(D.kind);
            float eff = 0;
            for (int i = 0; i < m.n; ++i) eff += m.w[i] * PurposeCurve(m.p[i], hour);
            sumExpected += D.people * DoorRate(D.kind) * params.rate * eff * meanDur_;
            const float lambda = D.people * DoorRate(D.kind) * params.rate * params.window;
            for (int64_t k = kLo; k <= kHi; ++k) {
                const int n = std::min(24, (int)std::floor(lambda + walk::U01(walk::Hash(D.id, (uint64_t)k, params.seed ^ 0x51))));
                for (int j = 0; j < n; ++j) { // as TripsInWindow, cheapest test first: the rank against keep (curves are <= 1)
                    const uint64_t id = walk::Hash(D.id ^ params.seed, (uint64_t)k, (uint64_t)j + 1);
                    const float rank = walk::U01(walk::Hash(id, 1));
                    if (rank >= keep) continue;
                    const double start = ((double)k + (double)walk::U01(walk::Hash(id, 2))) * params.window;
                    if (start > T || T - start > params.maxTrip) continue;
                    if (rank >= keep * PurposeCurve(PurposeOf(id, D.kind), hour)) continue;
                    if (Known(id)) continue;
                    if (pending_.size() < 2048) pending_.push_back({id, d, start});
                }
            }
        });
        expected = sumExpected;
        // keep: thin the schedule when the disk would hold more than ~90 % of the cap; steps with hysteresis so it rarely moves
        const float want = expected > 1.0f ? std::min(1.0f, 0.9f * (float)params.maxWalkers / expected) : 1.0f;
        if (want < keep * 0.85f || want > keep * 1.3f) {
            float q = 1.0f;
            while (q > want && q > 0.02f) q *= 0.8f;
            keep = q;
        }
        // the most recent starts last (Update pops from the back): people stepping out now get their routes first
        std::sort(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) { return a.start < b.start || (a.start == b.start && a.id < b.id); });
    }

    // ---- routes
    void LegAt(const WalkLeg& L, float d, V2& p, float& y, V2& dir) const {
        if (L.edge >= 0) {
            const float s = L.s1 >= L.s0 ? L.s0 + d : L.s0 - d;
            graph.At(L.edge, s, p, y, dir);
            if (L.s1 < L.s0) dir = dir * -1.0f;
            return;
        }
        const float f = L.len > 1e-4f ? std::clamp(d / L.len, 0.0f, 1.0f) : 1.0f;
        p = Lerp2(L.a, L.b, f), y = L.ya + (L.yb - L.ya) * f;
        if (L.len > 1e-4f) dir = Norm(L.b - L.a);
    }
    static WalkLegKind KindOf(WalkEdgeKind k) {
        switch (k) {
        case WalkEdgeKind::Along: return WalkLegKind::Along;
        case WalkEdgeKind::Corner: return WalkLegKind::Corner;
        case WalkEdgeKind::Link: return WalkLegKind::Link;
        case WalkEdgeKind::Cap: return WalkLegKind::Cap;
        default: return WalkLegKind::Crossing;
        }
    }
    float EdgeCost(const WalkEdge& e) const { return e.len + (e.kind == WalkEdgeKind::Crossing ? (e.signal ? 9.0f : 2.0f) : 0.0f); }

    // Shortest walk from (edge e0, s0) to (edge e1, s1); appends the edge pieces as legs. False: no way (or too long).
    bool Route(int e0, float s0, int e1, float s1, V2 goal, Walker& w, float maxLen) const {
        const WalkEdge& A = graph.edges[(size_t)e0];
        const WalkEdge& B = graph.edges[(size_t)e1];
        float direct = 1e30f;
        if (e0 == e1) direct = std::fabs(s1 - s0);
        if (++stampNow_ == 0) std::fill(stamp_.begin(), stamp_.end(), 0u), stampNow_ = 1;
        heap_.clear();
        auto h = [&](int v) { return Len(graph.verts[(size_t)v].p - goal); };
        auto relax = [&](int v, float g, int viaEdge) {
            if (stamp_[(size_t)v] == stampNow_ && g >= g_[(size_t)v]) return;
            stamp_[(size_t)v] = stampNow_, g_[(size_t)v] = g, prevEdge_[(size_t)v] = viaEdge;
            heap_.push_back({-(g + h(v)), v});
            std::push_heap(heap_.begin(), heap_.end());
        };
        relax(A.a, s0, -2);           // -2: the start edge, walked backwards to a
        relax(A.b, A.len - s0, -3);   // -3: forwards to b
        float best = direct;
        int bestEnd = -1;             // 0: reach B by its a end, 1: by its b end, -1: the direct piece
        while (!heap_.empty()) {
            std::pop_heap(heap_.begin(), heap_.end());
            const auto [negF, v] = heap_.back();
            heap_.pop_back();
            const float g = g_[(size_t)v];
            if (-negF > g + h(v) + 1e-3f) continue; // stale
            if (g + h(v) >= best || g > maxLen) break;
            if (v == B.a && g + s1 < best) best = g + s1, bestEnd = 0;
            if (v == B.b && g + (B.len - s1) < best) best = g + (B.len - s1), bestEnd = 1;
            const WalkVert& V = graph.verts[(size_t)v];
            for (uint32_t k = 0; k < V.adjCount; ++k) {
                const int e = graph.adj[V.adjFirst + k];
                const WalkEdge& E = graph.edges[(size_t)e];
                const int u = E.a == v ? E.b : E.a;
                relax(u, g + EdgeCost(E), e);
            }
        }
        if (best >= 1e29f || best > maxLen) return false;
        auto push = [&](int edge, float a, float b) {
            if (w.nLegs >= kWalkMaxLegs) return false;
            WalkLeg& L = w.legs[w.nLegs++];
            L = WalkLeg{};
            L.edge = edge, L.s0 = a, L.s1 = b, L.len = std::fabs(b - a), L.kind = KindOf(graph.edges[(size_t)edge].kind);
            return true;
        };
        if (bestEnd < 0) return push(e0, s0, s1);
        // unwind from B's end vertex to the start
        chain_.clear();
        int v = bestEnd == 0 ? B.a : B.b;
        while (prevEdge_[(size_t)v] >= 0) {
            const int e = prevEdge_[(size_t)v];
            chain_.push_back(e);
            const WalkEdge& E = graph.edges[(size_t)e];
            v = E.a == v ? E.b : E.a;
            if (chain_.size() > (size_t)kWalkMaxLegs) return false;
        }
        const bool startBack = prevEdge_[(size_t)v] == -2;
        if (!push(e0, s0, startBack ? 0.0f : A.len)) return false;
        int at = v;
        for (size_t i = chain_.size(); i-- > 0;) {
            const WalkEdge& E = graph.edges[(size_t)chain_[i]];
            const bool fwd = E.a == at;
            if (!push(chain_[i], fwd ? 0.0f : E.len, fwd ? E.len : 0.0f)) return false;
            at = fwd ? E.b : E.a;
        }
        return push(e1, bestEnd == 0 ? 0.0f : B.len, s1);
    }

    int PickDoor(uint64_t id, uint64_t salt, int from, WalkPurpose p, V2 around, float r) const {
        cand_.clear();
        ForDoorsNear(around, r, [&](int i) {
            if (i != from && PurposeGoesTo(p, doors[(size_t)i].kind) && Len(doors[(size_t)i].p - doors[(size_t)from].p) > 12.0f) cand_.push_back(i);
        });
        if (cand_.empty()) return -1;
        std::sort(cand_.begin(), cand_.end()); // grid order is the same as door order within a cell; sorted: independent of the grid
        return cand_[(size_t)(walk::Hash(id, salt) % (uint64_t)cand_.size())];
    }

    bool Plan(uint64_t id, int from, double start, Walker& w) const {
        if (from < 0 || from >= (int)doors.size() || doors[(size_t)from].edge < 0) return false;
        const WalkDoor& F = doors[(size_t)from];
        w = Walker{};
        w.id = id, w.from = from, w.fromId = F.id, w.start = start;
        w.purpose = PurposeOf(id, F.kind);
        w.look = (uint8_t)(walk::Hash(id, 3) % 6);
        w.speed = 1.15f + 0.35f * walk::U01(walk::Hash(id, 4));
        w.scale = 0.93f + 0.12f * walk::U01(walk::Hash(id, 8));
        w.side = 0.25f + 0.35f * walk::U01(walk::Hash(id, 9));
        int to = PickDoor(id, 10, from, w.purpose, F.p, params.destRadius);
        if (to < 0) return false;
        int via = -1;
        if (w.purpose == WalkPurpose::ToStop) { // to the stop, a wait, then on to a home or a workplace near it
            via = to;
            const WalkPurpose then = walk::U01(walk::Hash(id, 11)) < 0.5f ? WalkPurpose::Home : WalkPurpose::ToWork;
            to = PickDoor(id, 12, via, then, doors[(size_t)via].p, 160.0f);
            if (to < 0 || to == from) return false;
        }
        const float maxLen = params.maxTrip * w.speed;
        // out of the door
        auto doorLeg = [&](V2 a, float ya, V2 b, float yb) {
            if (w.nLegs >= kWalkMaxLegs) return false;
            WalkLeg& L = w.legs[w.nLegs++];
            L = WalkLeg{};
            L.a = a, L.b = b, L.ya = ya, L.yb = yb, L.len = Len(b - a), L.kind = WalkLegKind::Door;
            return true;
        };
        if (!doorLeg(F.p, F.y, F.at, F.atY)) return false;
        int curEdge = F.edge;
        float curS = F.s;
        const int stops[2] = {via, to};
        int stopLeg = -1;
        for (int k = 0; k < 2; ++k) {
            const int dIdx = stops[k];
            if (dIdx < 0) continue;
            const WalkDoor& D = doors[(size_t)dIdx];
            if (D.edge < 0) return false;
            if (!Route(curEdge, curS, D.edge, D.s, D.at, w, maxLen)) return false;
            curEdge = D.edge, curS = D.s;
            if (dIdx == via) stopLeg = w.nLegs; // the next leg starts after the wait at the stop
        }
        const WalkDoor& T = doors[(size_t)to];
        if (!doorLeg(T.at, T.atY, T.p, T.y)) return false;
        w.to = to, w.toId = T.id, w.via = via, w.viaId = via >= 0 ? doors[(size_t)via].id : 0;
        const float stopWait = 15.0f + 30.0f * walk::U01(walk::Hash(id, 13));
        Time(w, params.stepOut, 0.0f, stopLeg, stopWait);
        if (w.dur > params.maxTrip + stopWait + 60.0f) return false;
        return true;
    }
    // The legs' start times: stand t first, walk; wait at red crossings and at the stop (before leg stopLeg).
    void Time(Walker& w, float t, float walked, int stopLeg, float stopWait) const {
        for (int i = 0; i < w.nLegs; ++i) {
            WalkLeg& L = w.legs[i];
            if (i == stopLeg) t += stopWait;
            if (L.edge >= 0) {
                const WalkEdge& E = graph.edges[(size_t)L.edge];
                if (E.kind == WalkEdgeKind::Crossing && E.signal && L.len > 1.0f) {
                    const float off = walk::U01(walk::Hash((uint64_t)E.node, 0xC5)) * walk::kSignalCycle;
                    const double abs = w.start + (double)t + (double)off;
                    const float local = (float)std::fmod(abs, (double)walk::kSignalCycle);
                    const float open = (float)E.group * walk::kSignalCycle * 0.5f;
                    float into = local - open;
                    if (into < 0) into += walk::kSignalCycle;
                    if (into > walk::kSignalWalk) t += walk::kSignalCycle - into; // red: wait for the next walk phase
                }
            }
            L.t0 = t, L.walked = walked;
            t += L.len / w.speed, walked += L.len;
        }
        w.stopAt = stopLeg;
        w.dur = t + params.stepIn;
        w.length = walked;
    }
    // After a road change: from where the walker is now to its destination door, on the current graph.
    bool Reroute(Walker& w) {
        const int to = DoorIndex(w.toId);
        if (to < 0 || doors[(size_t)to].edge < 0) return false;
        int e;
        float s, y;
        V2 at;
        if (!graph.Nearest(w.line, 30.0f, e, s, at, y)) return false;
        const float walkedBefore = w.walked;
        Walker n = w;
        n.nLegs = 0, n.cur = 0, n.via = -1, n.viaId = 0, n.start = now;
        WalkLeg& first = n.legs[n.nLegs++];
        first = WalkLeg{};
        first.a = w.line, first.b = at, first.ya = w.y, first.yb = y, first.len = Len(at - w.line);
        first.kind = OnCrossing(w) ? WalkLegKind::Crossing : WalkLegKind::Link; // mid-crossing: they finish it to the nearest kerb
        const WalkDoor& D = doors[(size_t)to];
        if (!Route(e, s, D.edge, D.s, D.at, n, params.maxTrip * n.speed)) return false;
        if (n.nLegs >= kWalkMaxLegs) return false;
        WalkLeg& last = n.legs[n.nLegs++];
        last = WalkLeg{};
        last.a = D.at, last.b = D.p, last.ya = D.atY, last.yb = D.y, last.len = Len(D.p - D.at), last.kind = WalkLegKind::Door;
        n.to = to;
        Time(n, 0.0f, walkedBefore, -1, 0.0f);
        w = n;
        Evaluate(w, now);
        return true;
    }
};

} // namespace city
} // namespace tbd
