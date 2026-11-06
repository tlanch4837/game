// City road network: the road profiles (ported 1:1 from assets/roads/roadlib.js, the CS2 road list), a node / segment
// graph with cubic Bezier segments, splitting, auto-intersections where roads cross, and the mesh builder (strips,
// curbs, lane markings, crosswalks, junction fills with curved corners, bridge decks, pillars, parapets, tunnel portals
// and quay walls). Platform free: the host gives it the ground height and uploads the per-material vertex lists.
//
// Conventions: world x east, z south, y up. A segment runs a -> b; a profile's strips are listed left to right looking
// from a to b, the first `back` car lanes drive b -> a, the rest a -> b (right-hand traffic).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../render/vertex.h"
#include "../vecmath.h"

namespace tbd {
namespace city {

// ------------------------------------------------------------------------------------------------ materials
enum class Mat : uint8_t { Asphalt, AsphaltWorn, Gravel, Concrete, Curb, Pavers, Grass, BikeAsphalt, Paint, Track, Count }; // Track: SPOM railway (TrackSweep), texture loaded by the host
constexpr int kMatCount = (int)Mat::Count;
// assets/roads/tex/<name>_albedo.png / _normal.png (tools/road_gen/gen_road_textures.py). Paint has no texture.
constexpr const char* kMatFile[kMatCount] = {"asphalt", "asphalt_worn", "gravel", "concrete", "curb", "pavers", "grass", "bike_asphalt", nullptr, nullptr};
constexpr float kMatTile[kMatCount] = {4, 4, 3, 3, 2, 2, 4, 4, 1, 1}; // metres per texture repeat

// ------------------------------------------------------------------------------------------------ profiles
enum class Kind : uint8_t { Lane, Shoulder, Sidewalk, Curb, Median, Barrier, Verge, Path, Bike, Parking, QuayWall, Track };
struct Strip {
    float w;
    Kind kind;
    Mat mat;
    float h; // top above the road surface
};
enum class Cat : uint8_t { Small, Medium, Large, Highway, Quay, Path, Rail };
struct Profile {
    std::string name;
    Cat cat;
    int fwd, back; // car lanes per direction (one-way: back = 0)
    std::vector<Strip> strips;
    float width = 0;
    float sideL = 0, sideR = 0; // sidewalk / verge / barrier band at each edge (what junction corners round off)
    bool OneWay() const { return back == 0 && fwd > 0; }
    float Half() const { return width * 0.5f; }
};

namespace detail {
using S = std::vector<Strip>;
inline Strip st(float w, Kind k, Mat m, float h = 0) { return {w, k, m, h}; }
inline S cat(std::initializer_list<S> parts) {
    S out;
    for (const S& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
inline S lanes(int n, float w = 3.5f, Mat m = Mat::Asphalt) { return S((size_t)n, st(w, Kind::Lane, m)); }
inline S walkL(float w) { return {st(w - 0.25f, Kind::Sidewalk, Mat::Concrete, 0.15f), st(0.25f, Kind::Curb, Mat::Curb, 0.15f)}; }
inline S walkR(float w) { return {st(0.25f, Kind::Curb, Mat::Curb, 0.15f), st(w - 0.25f, Kind::Sidewalk, Mat::Concrete, 0.15f)}; }
inline S median(float w, Mat m = Mat::Grass) {
    return {st(0.25f, Kind::Curb, Mat::Curb, 0.15f), st(w - 0.5f, Kind::Median, m, 0.15f), st(0.25f, Kind::Curb, Mat::Curb, 0.15f)};
}
inline Strip barrier() { return st(0.5f, Kind::Barrier, Mat::Curb, 0.85f); }
inline Strip verge(float w) { return st(w, Kind::Verge, Mat::Grass); }
inline Strip park(int kind) { return st(kind == 2 ? 5.0f : kind == 1 ? 4.5f : 2.5f, Kind::Parking, Mat::AsphaltWorn); } // 2 perp, 1 angled
constexpr float SWs = 2.5f, SWm = 3.0f, SWl = 4.0f;
inline S street(float sw, const S& mid) { return cat({walkL(sw), mid, walkR(sw)}); }
inline Strip alleyEdge() { return st(0.3f, Kind::Curb, Mat::Curb, 0.12f); }
inline S hwyEdgeL() { return {verge(2), barrier(), st(1, Kind::Shoulder, Mat::Asphalt)}; }
inline S hwyEdgeR() { return {st(2.5f, Kind::Shoulder, Mat::Asphalt), barrier(), verge(2)}; }
inline S hwyMid() { return {st(1, Kind::Shoulder, Mat::Asphalt), barrier(), st(1, Kind::Shoulder, Mat::Asphalt)}; }
inline S hwy1(int n) { return cat({hwyEdgeL(), lanes(n, 3.75f), hwyEdgeR()}); }
inline S hwy2(int back, int fwd) {
    return cat({S{verge(2), barrier(), st(2.5f, Kind::Shoulder, Mat::Asphalt)}, lanes(back, 3.75f), hwyMid(), lanes(fwd, 3.75f), hwyEdgeR()});
}
inline bool Side(Kind k) { return k == Kind::Sidewalk || k == Kind::Curb || k == Kind::Verge || k == Kind::Barrier || k == Kind::QuayWall; }
inline Profile P(const char* name, Cat c, int fwd, int back, S strips) {
    Profile p{name, c, fwd, back, std::move(strips)};
    for (const Strip& s : p.strips) p.width += s.w;
    for (size_t i = 0; i < p.strips.size() && Side(p.strips[i].kind); ++i) p.sideL += p.strips[i].w;
    for (size_t i = p.strips.size(); i-- > 0 && Side(p.strips[i].kind);) p.sideR += p.strips[i].w;
    if (p.sideL >= p.width * 0.5f) p.sideL = 0; // paths: all "side", round nothing off
    if (p.sideR >= p.width * 0.5f) p.sideR = 0;
    return p;
}
} // namespace detail

// Names, categories and lane counts follow the CS2 road list (cs2.paradoxwikis.com/Roads); widths are our estimates.
inline const std::vector<Profile>& Profiles() {
    using namespace detail;
    static const std::vector<Profile> list = [] {
        std::vector<Profile> v;
        const Cat s = Cat::Small, m = Cat::Medium, l = Cat::Large, hw = Cat::Highway;
        // ---- small roads
        v.push_back(P("Two-Lane Road", s, 1, 1, street(SWs, lanes(2, 3))));
        v.push_back(P("Three-Lane Asymmetric Road", s, 2, 1, street(SWs, lanes(3, 3))));
        v.push_back(P("One-Lane One-Way Road", s, 1, 0, street(SWs, lanes(1, 3.5f))));
        v.push_back(P("Two-Lane One-Way Road", s, 2, 0, street(SWs, lanes(2, 3))));
        v.push_back(P("Three-Lane One-Way Road", s, 3, 0, street(SWs, lanes(3, 3))));
        v.push_back(P("Gravel Road", s, 1, 1, {verge(1), st(6, Kind::Lane, Mat::Gravel), verge(1)}));
        v.push_back(P("Gravel One-Way Road", s, 1, 0, {verge(1), st(3.5f, Kind::Lane, Mat::Gravel), verge(1)}));
        v.push_back(P("Alley", s, 1, 1, cat({S{alleyEdge()}, lanes(2, 2.75f, Mat::AsphaltWorn), S{alleyEdge()}})));
        v.push_back(P("One-Way Alley", s, 1, 0, cat({S{alleyEdge()}, lanes(1, 3.25f, Mat::AsphaltWorn), S{alleyEdge()}})));
        v.push_back(P("Two-Lane Perpendicular Parking Road", s, 1, 1, street(SWs, cat({S{park(2)}, lanes(2, 3), S{park(2)}}))));
        v.push_back(P("Two-Lane Angled Parking Road", s, 1, 1, street(SWs, cat({S{park(1)}, lanes(2, 3), S{park(1)}}))));
        v.push_back(P("Two-Lane Divided Perpendicular Parking Road", s, 1, 1, street(SWs, cat({S{park(2)}, lanes(1, 3), median(2), lanes(1, 3), S{park(2)}}))));
        v.push_back(P("Two-Lane Divided Angled Parking Road", s, 1, 1, street(SWs, cat({S{park(1)}, lanes(1, 3), median(2), lanes(1, 3), S{park(1)}}))));
        v.push_back(P("One-Lane One-Way Perpendicular Parking Road", s, 1, 0, street(SWs, cat({S{park(2)}, lanes(1, 3.5f), S{park(2)}}))));
        v.push_back(P("One-Lane One-Way Angled Parking Road", s, 1, 0, street(SWs, cat({S{park(1)}, lanes(1, 3.5f), S{park(1)}}))));
        v.push_back(P("Perpendicular Parking Alley", s, 1, 1, cat({S{alleyEdge(), park(2)}, lanes(2, 2.75f, Mat::AsphaltWorn), S{park(2), alleyEdge()}})));
        v.push_back(P("Angled Parking Alley", s, 1, 1, cat({S{alleyEdge(), park(1)}, lanes(2, 2.75f, Mat::AsphaltWorn), S{park(1), alleyEdge()}})));
        v.push_back(P("One-Way Perpendicular Parking Alley", s, 1, 0, cat({S{alleyEdge(), park(2)}, lanes(1, 3.25f, Mat::AsphaltWorn), S{park(2), alleyEdge()}})));
        v.push_back(P("One-Way Angled Parking Alley", s, 1, 0, cat({S{alleyEdge(), park(1)}, lanes(1, 3.25f, Mat::AsphaltWorn), S{park(1), alleyEdge()}})));
        v.push_back(P("Pedestrian Street", s, 1, 1, {st(0.3f, Kind::Curb, Mat::Curb, 0.05f), st(11, Kind::Path, Mat::Pavers, 0.05f), st(0.3f, Kind::Curb, Mat::Curb, 0.05f)}));
        v.push_back(P("Small Pedestrian Street", s, 1, 0, {st(0.3f, Kind::Curb, Mat::Curb, 0.05f), st(7, Kind::Path, Mat::Pavers, 0.05f), st(0.3f, Kind::Curb, Mat::Curb, 0.05f)}));
        // ---- medium roads
        v.push_back(P("Four-Lane Road", m, 2, 2, street(SWm, lanes(4))));
        v.push_back(P("Four-Lane Divided Road", m, 2, 2, street(SWm, cat({lanes(2), median(2), lanes(2)}))));
        v.push_back(P("Four-Lane Asymmetric Road", m, 3, 1, street(SWm, cat({lanes(1), median(1, Mat::Curb), lanes(3)}))));
        v.push_back(P("Five-Lane Asymmetric Road", m, 3, 2, street(SWm, cat({lanes(2), median(1, Mat::Curb), lanes(3)}))));
        v.push_back(P("Four-Lane One-Way Road", m, 4, 0, street(SWm, lanes(4))));
        v.push_back(P("Five-Lane One-Way Road", m, 5, 0, street(SWm, lanes(5))));
        v.push_back(P("Four-Lane Perpendicular Parking Road", m, 2, 2, street(SWm, cat({S{park(2)}, lanes(4), S{park(2)}}))));
        v.push_back(P("Four-Lane Angled Parking Road", m, 2, 2, street(SWm, cat({S{park(1)}, lanes(4), S{park(1)}}))));
        // ---- large roads
        v.push_back(P("Six-Lane Road", l, 3, 3, street(SWl, lanes(6))));
        v.push_back(P("Six-Lane Divided Road", l, 3, 3, street(SWl, cat({lanes(3), median(3), lanes(3)}))));
        v.push_back(P("Eight-Lane Divided Road", l, 4, 4, street(SWl, cat({lanes(4), median(3), lanes(4)}))));
        v.push_back(P("Six-Lane Asymmetric Road", l, 4, 2, street(SWl, cat({lanes(2), median(2), lanes(4)}))));
        v.push_back(P("Seven-Lane Asymmetric Road (5+2)", l, 5, 2, street(SWl, cat({lanes(2), median(2), lanes(5)}))));
        v.push_back(P("Seven-Lane Asymmetric Road (4+3)", l, 4, 3, street(SWl, cat({lanes(3), median(2), lanes(4)}))));
        v.push_back(P("Six-Lane One-Way Road", l, 6, 0, street(SWl, lanes(6))));
        v.push_back(P("Seven-Lane One-Way Road", l, 7, 0, street(SWl, lanes(7))));
        // ---- highways
        v.push_back(P("Two-Lane Two-Way Highway", hw, 2, 2, hwy2(2, 2)));
        v.push_back(P("Three-Lane Two-Way Highway", hw, 3, 3, hwy2(3, 3)));
        v.push_back(P("Four-Lane Two-Way Highway", hw, 4, 4, hwy2(4, 4)));
        v.push_back(P("Four-Lane Asymmetric Highway", hw, 3, 1, hwy2(1, 3)));
        v.push_back(P("Five-Lane Asymmetric Highway", hw, 3, 2, hwy2(2, 3)));
        static const char* const kN[] = {"One", "Two", "Three", "Four", "Five"};
        for (int n = 1; n <= 5; ++n) v.push_back(P((std::string(kN[n - 1]) + "-Lane One-Way Highway").c_str(), hw, n, 0, hwy1(n)));
        // ---- quays
        v.push_back(P("Medium Quay With Two-Way Road", Cat::Quay, 1, 1, cat({S{st(1, Kind::QuayWall, Mat::Curb, 1.0f)}, lanes(2, 3), walkR(SWs)})));
        v.push_back(P("Wide Quay With Four-Lane Road", Cat::Quay, 2, 2, cat({S{st(1, Kind::QuayWall, Mat::Curb, 1.0f)}, lanes(4), walkR(SWm)})));
        // ---- paths
        v.push_back(P("Pedestrian Path", Cat::Path, 0, 0, {verge(1), st(3, Kind::Path, Mat::Concrete, 0.04f), verge(1)}));
        v.push_back(P("Bicycle Path", Cat::Path, 0, 0, {verge(0.75f), st(3, Kind::Bike, Mat::BikeAsphalt, 0.03f), verge(0.75f)}));
        // ---- rail: same node / segment graph as roads (nodes are shared, split and snapped the same way); no lanes, so no
        // traffic, zoning or markings. Use Network::CanConnect so a rail segment never joins a road node.
        v.push_back(P("Single Track Railway", Cat::Rail, 0, 0, {verge(1.5f), st(4, Kind::Track, Mat::Gravel, 0.1f), verge(1.5f)}));
        v.push_back(P("Double Track Railway", Cat::Rail, 0, 0, {verge(1.5f), st(4, Kind::Track, Mat::Gravel, 0.1f), verge(1.5f), st(4, Kind::Track, Mat::Gravel, 0.1f), verge(1.5f)}));
        return v;
    }();
    return list;
}
inline int ProfileIndex(const char* name) {
    const auto& p = Profiles();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].name == name) return (int)i;
    return 0;
}

// ------------------------------------------------------------------------------------------------ 2D helpers (x, z)
struct V2 {
    float x = 0, z = 0;
};
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.z + b.z}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.z - b.z}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.z * s}; }
inline float Dot2(V2 a, V2 b) { return a.x * b.x + a.z * b.z; }
inline float Cross2(V2 a, V2 b) { return a.x * b.z - a.z * b.x; }
inline float Len(V2 a) { return std::sqrt(a.x * a.x + a.z * a.z); }
inline V2 Norm(V2 a) {
    const float l = Len(a);
    return l > 1e-6f ? a * (1.0f / l) : V2{1, 0};
}
inline V2 LeftOf(V2 d) { return {d.z, -d.x}; }  // looking along d (y up): left
inline V2 RightOf(V2 d) { return {-d.z, d.x}; }
inline V2 Lerp2(V2 a, V2 b, float t) { return a + (b - a) * t; }
inline V2 XZ(Vec3 p) { return {p.x, p.z}; }
inline float Angle(V2 d) { return std::atan2(d.z, d.x); }
// Intersection of the lines p + d*s and q + e*u; false when (nearly) parallel.
inline bool LineHit(V2 p, V2 d, V2 q, V2 e, V2& out) {
    const float den = Cross2(d, e);
    if (std::fabs(den) < 1e-4f) return false;
    const float s = Cross2(q - p, e) / den;
    out = p + d * s;
    return true;
}

struct Cubic {
    V2 p0, p1, p2, p3;
    V2 At(float t) const {
        const float u = 1 - t;
        return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
    }
    V2 Tan(float t) const {
        const float u = 1 - t;
        V2 d = (p1 - p0) * (3 * u * u) + (p2 - p1) * (6 * u * t) + (p3 - p2) * (3 * t * t);
        if (Len(d) < 1e-5f) d = p3 - p0;
        return Norm(d);
    }
    void Split(float t, Cubic& l, Cubic& r) const {
        const V2 a = Lerp2(p0, p1, t), b = Lerp2(p1, p2, t), c = Lerp2(p2, p3, t);
        const V2 d = Lerp2(a, b, t), e = Lerp2(b, c, t), m = Lerp2(d, e, t);
        l = {p0, a, d, m};
        r = {m, e, c, p3};
    }
    float Length(int n = 24) const {
        float s = 0;
        V2 prev = p0;
        for (int i = 1; i <= n; ++i) {
            const V2 q = At((float)i / n);
            s += Len(q - prev);
            prev = q;
        }
        return s;
    }
    // Curvature proxy: the largest turn between consecutive samples, radians per metre.
    float MaxTurnPerMetre(int n = 16) const {
        float worst = 0;
        V2 prev = Tan(0), pp = p0;
        for (int i = 1; i <= n; ++i) {
            const float t = (float)i / n;
            const V2 d = Tan(t), q = At(t);
            const float step = std::max(0.01f, Len(q - pp));
            worst = std::max(worst, std::acos(std::clamp(Dot2(prev, d), -1.0f, 1.0f)) / step);
            prev = d, pp = q;
        }
        return worst;
    }
    static Cubic Line(V2 a, V2 b) { return {a, Lerp2(a, b, 1.0f / 3), Lerp2(a, b, 2.0f / 3), b}; }
    static Cubic Quad(V2 a, V2 q, V2 b) { return {a, a + (q - a) * (2.0f / 3), b + (q - b) * (2.0f / 3), b}; }
    // Offset sideways by d (right positive): good enough for road-sized curves.
    Cubic Offset(float d) const {
        const V2 n0 = RightOf(Tan(0)), n3 = RightOf(Tan(1));
        const V2 n1 = RightOf(Norm(p1 - p0 + (p2 - p1) * 0.01f)), n2 = RightOf(Norm(p3 - p2 + (p2 - p1) * 0.01f));
        const float k = 1.0f; // control polygon offset; scaled for curvature below
        Cubic c{p0 + n0 * d, p1 + n1 * (d * k), p2 + n2 * (d * k), p3 + n3 * d};
        // keep the tangent lengths in proportion to the new chord
        const float s0 = Len(p3 - p0), s1 = Len(c.p3 - c.p0);
        if (s0 > 1e-3f) {
            const float f = s1 / s0;
            c.p1 = c.p0 + (p1 - p0) * f;
            c.p2 = c.p3 + (p2 - p3) * f;
        }
        return c;
    }
};

using GroundFn = std::function<float(float x, float z)>;

// A road's height at t. Ground roads (both ends on the ground) follow the terrain, smoothed over about 30 m, like CS2's;
// bridges, tunnels and ramps (an end off the ground) run straight between their end heights.
// follow < 0: decide from the end heights (on the ground within 0.3 m); 0 / 1: forced.
inline float FollowY(const Cubic& c, float ya, float yb, float t, const GroundFn& ground, int follow = -1) {
    const float lin = ya + (yb - ya) * t;
    if (!ground || follow == 0) return lin;
    const float ga = ground(c.p0.x, c.p0.z), gb = ground(c.p3.x, c.p3.z);
    if (follow < 0 && (std::fabs(ya - ga) > 0.3f || std::fabs(yb - gb) > 0.3f)) return lin;
    if (follow == 1) ya = ga, yb = gb;
    const float len = std::max(1.0f, Len(c.p3 - c.p0));
    float sum = 0;
    int n = 0;
    for (int k = -3; k <= 3; ++k) {
        const float tk = std::clamp(t + k * 5.0f / len, 0.0f, 1.0f);
        const V2 q = c.At(tk);
        sum += ground(q.x, q.z) - (ga + (gb - ga) * tk);
        ++n;
    }
    const float edge = std::min(1.0f, std::min(t, 1.0f - t) * len / 6.0f); // pinned to the node heights at the ends
    return lin + sum / n * edge;
}

// ------------------------------------------------------------------------------------------------ the graph
struct Node {
    Vec3 p;               // p.y: the height when off the ground (bridges, tunnels); ground nodes read the terrain live
    std::vector<int> segs;
    bool alive = true;
    bool grounded = true; // CS2-style elevation 0: sits on (and follows) the terrain however it is edited later
};
struct Segment {
    int a = -1, b = -1;
    V2 c1, c2;
    int profile = 0;
    bool alive = true;
};

class Network {
public:
    std::vector<Node> nodes;
    std::vector<Segment> segs;
    uint64_t version = 1; // bumped on every change: the host rebuilds meshes when it moves
    GroundFn ground;      // for terrain-following ground roads (FollowY)

    Cubic Curve(const Segment& s) const { return {XZ(nodes[s.a].p), s.c1, s.c2, XZ(nodes[s.b].p)}; }
    Cubic Curve(int s) const { return Curve(segs[s]); }
    float NodeY(int n) const { return nodes[n].grounded && ground ? ground(nodes[n].p.x, nodes[n].p.z) : nodes[n].p.y; }
    Vec3 NodePos(int n) const { return {nodes[n].p.x, NodeY(n), nodes[n].p.z}; }
    bool Follows(const Segment& s) const { return nodes[s.a].grounded && nodes[s.b].grounded; }
    float YAt(const Segment& s, float t) const {
        float y = FollowY(Curve(s), NodeY(s.a), NodeY(s.b), t, ground, Follows(s) ? 1 : 0);
        if (ground && !Follows(s)) { // a ramp (not a ground road) with neither end underground rides over a hill on its way
            const V2 pa = XZ(nodes[s.a].p), pb = XZ(nodes[s.b].p);
            if (NodeY(s.a) > ground(pa.x, pa.z) - 4.0f && NodeY(s.b) > ground(pb.x, pb.z) - 4.0f) {
                const V2 q = Curve(s).At(t);
                y = std::max(y, ground(q.x, q.z));
            }
        }
        return y;
    }
    const Profile& Prof(int s) const { return Profiles()[segs[s].profile]; }
    // Rail and road share the graph but not nodes: a node is rail if any live segment on it is rail.
    bool IsRailSeg(int s) const { return Prof(s).cat == Cat::Rail; }
    bool IsRailNode(int n) const {
        for (int s : nodes[n].segs)
            if (segs[s].alive && IsRailSeg(s)) return true;
        return false;
    }
    bool HasRoadSeg(int n) const {
        for (int s : nodes[n].segs)
            if (segs[s].alive && !IsRailSeg(s)) return true;
        return false;
    }
    // May a segment of this profile attach to node n? (empty nodes accept anything)
    bool CanConnect(int n, int profile) const {
        const bool rail = Profiles()[profile].cat == Cat::Rail;
        return rail ? !HasRoadSeg(n) : !IsRailNode(n);
    }

    int AddNode(Vec3 p, bool grounded) {
        nodes.push_back({p, {}, true, grounded});
        ++version;
        return (int)nodes.size() - 1;
    }
    int AddSeg(int a, int b, V2 c1, V2 c2, int profile) {
        segs.push_back({a, b, c1, c2, profile, true});
        const int id = (int)segs.size() - 1;
        nodes[a].segs.push_back(id);
        nodes[b].segs.push_back(id);
        ++version;
        return id;
    }
    int AddSeg(int a, int b, const Cubic& c, int profile) { return AddSeg(a, b, c.p1, c.p2, profile); }
    void RemoveSeg(int s) {
        Segment& g = segs[s];
        if (!g.alive) return;
        g.alive = false;
        for (int n : {g.a, g.b}) {
            auto& v = nodes[n].segs;
            v.erase(std::remove(v.begin(), v.end(), s), v.end());
            if (v.empty()) nodes[n].alive = false;
        }
        ++version;
    }
    // Splits segment s at t; returns the new middle node. s keeps the a half, a new segment takes the b half.
    int Split(int s, float t) {
        Cubic l, r;
        Curve(s).Split(t, l, r);
        const float y = YAt(segs[s], t);
        const int m = AddNode({l.p3.x, y, l.p3.z}, Follows(segs[s]));
        const int b = segs[s].b, prof = segs[s].profile;
        auto& bs = nodes[b].segs;
        bs.erase(std::remove(bs.begin(), bs.end(), s), bs.end());
        segs[s].b = m;
        segs[s].c1 = l.p1, segs[s].c2 = l.p2;
        nodes[m].segs.push_back(s);
        AddSeg(m, b, r.p1, r.p2, prof);
        return m;
    }
    // Nearest point of a segment's centre line to p (xz): parameter and distance.
    void Nearest(int s, V2 p, float& bestT, float& bestD) const {
        const Cubic c = Curve(s);
        bestD = 1e30f, bestT = 0;
        const int n = 48;
        for (int i = 0; i <= n; ++i) {
            const float t = (float)i / n, d = Len(c.At(t) - p);
            if (d < bestD) bestD = d, bestT = t;
        }
        float lo = std::max(0.0f, bestT - 1.0f / n), hi = std::min(1.0f, bestT + 1.0f / n);
        for (int k = 0; k < 20; ++k) { // golden-ish refinement
            const float m1 = lo + (hi - lo) * 0.382f, m2 = lo + (hi - lo) * 0.618f;
            if (Len(c.At(m1) - p) < Len(c.At(m2) - p)) hi = m2;
            else lo = m1;
        }
        bestT = 0.5f * (lo + hi);
        bestD = Len(c.At(bestT) - p);
    }
    // profile >= 0: only nodes a segment of that profile may attach to (CanConnect): rails never join road nodes
    int NearestNode(V2 p, float radius, int skip = -1, int profile = -1) const {
        int best = -1;
        float bd = radius;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].alive || (int)i == skip) continue;
            if (profile >= 0 && !CanConnect((int)i, profile)) continue;
            const float d = Len(XZ(nodes[i].p) - p);
            if (d < bd) bd = d, best = (int)i;
        }
        return best;
    }
    // The segment whose surface covers p (within its half width + pad); t and centre-line distance out.
    // profile >= 0: only segments of the same kind (rail / road) as that profile
    int SegmentAt(V2 p, float pad, float& t, float& dist, int profile = -1) const {
        int best = -1;
        float score = 1e30f;
        const bool wantRail = profile >= 0 && Profiles()[profile].cat == Cat::Rail;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!segs[i].alive) continue;
            if (profile >= 0 && IsRailSeg((int)i) != wantRail) continue;
            const float hw = Prof((int)i).Half();
            float ti, di;
            Nearest((int)i, p, ti, di);
            if (di <= hw + pad && di - hw < score) score = di - hw, best = (int)i, t = ti, dist = di;
        }
        return best;
    }
    // Outward unit direction of segment s at node n.
    V2 OutDir(int s, int n) const {
        const Cubic c = Curve(s);
        return segs[s].a == n ? c.Tan(0) : c.Tan(1) * -1.0f;
    }
    void Clear() {
        nodes.clear();
        segs.clear();
        ++version;
    }
};

// ------------------------------------------------------------------------------------------------ mesh building
struct MeshOut {
    std::array<std::vector<Vertex>, kMatCount> byMat;
    std::vector<Vertex> arrows; // direction arrows (paint), shown only while the road tool is open
    size_t Triangles() const {
        size_t n = 0;
        for (const auto& v : byMat) n += v.size() / 3;
        return n;
    }
};

namespace mesh {
constexpr float kRoadLift = 0.12f;   // the driving surface above the node height (clears terrain sampling noise)
constexpr float kPaintLift = 0.145f; // markings above the asphalt
constexpr float kDeck = 1.2f;        // deck / skirt depth under the surface
constexpr float kTunnelDepth = 4.0f; // centre line this far under the ground: inside a tunnel (not drawn)
constexpr float kElevated = 3.0f;    // this far over the ground: a bridge (pillars, parapets, underside)

inline Vec3 V(V2 p, float y) { return {p.x, y, p.z}; }
inline void Tri(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 n, Vec3 col, float ua, float va, float ub, float vb, float uc, float vc) {
    out.push_back({a.x, a.y, a.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f, ua, va});
    out.push_back({b.x, b.y, b.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f, ub, vb});
    out.push_back({c.x, c.y, c.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f, uc, vc});
}
// Quad a b c d (in order around), normal from the winding, flipped to face `hint` when given.
inline void Quad(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 col, const float uv[8], Vec3 hint = {0, 1, 0}) {
    Vec3 n = Normalize(Cross(b - a, d - a));
    if (Dot(n, n) < 0.5f) n = Normalize(Cross(c - b, a - b));
    if (Dot(n, hint) < 0) n = n * -1.0f;
    Tri(out, a, b, c, n, col, uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]);
    Tri(out, a, c, d, n, col, uv[0], uv[1], uv[4], uv[5], uv[6], uv[7]);
}
inline void FlatQuad(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 col, float tile) {
    const float uv[8] = {a.x / tile, a.z / tile, b.x / tile, b.z / tile, c.x / tile, c.z / tile, d.x / tile, d.z / tile};
    Quad(out, a, b, c, d, col, uv);
}
inline void Wall(std::vector<Vertex>& out, Vec3 a, Vec3 b, float yTopA, float yTopB, float yBotA, float yBotB, Vec3 outward, Vec3 col, float tile) {
    const float L = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z)) / tile;
    const float uv[8] = {0, yTopA / tile, L, yTopB / tile, L, yBotB / tile, 0, yBotA / tile};
    Quad(out, {a.x, yTopA, a.z}, {b.x, yTopB, b.z}, {b.x, yBotB, b.z}, {a.x, yBotA, a.z}, col, uv, outward);
}
// SPOM railway track (TrackPS in mesh.hlsl, vertex alpha 0.83): a box of width kTrackW and depth kTrackDepth under the rail top,
// swept through the given centre-line points (rail-top height in .y). Top and both sides; the relief texture does the rest.
constexpr float kTrackW = 4.4f, kTrackDepth = 0.55f;
struct TrackPt {
    V2 c;      // centre line
    float y;   // rail top
    V2 f;      // forward (unit)
    float s;   // metres along
};
inline void TrackSweep(std::vector<Vertex>& out, const std::vector<TrackPt>& pts) {
    const float hw = kTrackW * 0.5f;
    auto vtx = [&](const TrackPt& p, float x, float dy) {
        const V2 across{p.f.z, -p.f.x}; // = cross(up, forward), as the shader builds it
        const V2 w = p.c + across * x;
        out.push_back({w.x, p.y + dy, w.z, p.f.x, 0.0f, p.f.z, dy, kTrackW, 0.0f, 0.83f, x, p.s});
    };
    for (size_t i = 1; i < pts.size(); ++i) {
        const TrackPt &a = pts[i - 1], &b = pts[i];
        const float face[3][4] = {{-hw, 0, hw, 0}, {-hw, -kTrackDepth, -hw, 0}, {hw, 0, hw, -kTrackDepth}}; // top, left side, right side: (x0,dy0) -> (x1,dy1)
        for (const auto& fc : face) {
            vtx(a, fc[0], fc[1]), vtx(a, fc[2], fc[3]), vtx(b, fc[2], fc[3]);
            vtx(a, fc[0], fc[1]), vtx(b, fc[2], fc[3]), vtx(b, fc[0], fc[1]);
        }
    }
}
// An axis-free box: centre bottom c, along d (unit) length len, across width w, from y0 to y1.
inline void Box(std::vector<Vertex>& out, V2 c, V2 d, float len, float w, float y0, float y1, Vec3 col, float tile) {
    const V2 s = LeftOf(d) * (w * 0.5f), f = d * (len * 0.5f);
    const V2 p[4] = {c - f + s, c + f + s, c + f - s, c - f - s};
    for (int i = 0; i < 4; ++i) {
        const V2 a = p[i], b = p[(i + 1) % 4], mid = (a + b) * 0.5f - c;
        Wall(out, V(a, 0), V(b, 0), y1, y1, y0, y0, {mid.x, 0, mid.z}, col, tile);
    }
    FlatQuad(out, V(p[0], y1), V(p[1], y1), V(p[2], y1), V(p[3], y1), col, tile);
}
} // namespace mesh

// Per segment, the parameter range the strip sweep covers (junctions take the ends).
struct SegTrim {
    float t0 = 0, t1 = 1;
    bool junctionA = false, junctionB = false; // the end meets a junction (crosswalks there)
    bool miterA = false, miterB = false;       // the end continues into one same-width road: cut along the bisector
    V2 leftA, leftB;                           // the cut's left vector, already scaled so the width stays true
    // lane attachments (highway exits / on-ramps that start or end on a lane, src/city/road_tool.h lane snapping):
    int host = -1;                             // this road's free end sits on that road: no edges while over it
    struct Cut { float t0, t1, side; };        // on the host: where a ramp peels off; side +1 left, -1 right
    std::vector<Cut> cuts;                     // no verge / barrier / edge walls on that side over [t0, t1]
};

// Distance along the curve -> parameter, from a sampled arc length table.
inline float ParamAtDistance(const Cubic& c, float dist, bool fromEnd) {
    const int n = 48;
    float acc = 0;
    V2 prev = fromEnd ? c.p3 : c.p0;
    for (int i = 1; i <= n; ++i) {
        const float t = fromEnd ? 1.0f - (float)i / n : (float)i / n;
        const V2 q = c.At(t);
        const float step = Len(q - prev);
        if (acc + step >= dist) {
            const float f = step > 1e-6f ? (dist - acc) / step : 0.0f;
            const float tPrev = fromEnd ? 1.0f - (float)(i - 1) / n : (float)(i - 1) / n;
            return tPrev + (t - tPrev) * f;
        }
        acc += step;
        prev = q;
    }
    return fromEnd ? 0.0f : 1.0f;
}

struct Arm {
    int seg;
    bool atA;
    V2 u;      // outward
    float ang; // atan2 of u
    float hw, sideL, sideR; // half width; side bands on the arm's left / right looking outward
    float trim = 0, t = 0;  // distance and parameter of the cut
    V2 end, left;           // cut centre and its left (outward-looking)
};

// Gathers the arms at node n, sorted counter-clockwise by angle (atan2(z, x)).
inline std::vector<Arm> ArmsAt(const Network& net, int n) {
    std::vector<Arm> arms;
    for (int s : net.nodes[n].segs) {
        if (!net.segs[s].alive) continue;
        const Profile& p = net.Prof(s);
        Arm a;
        a.seg = s;
        a.atA = net.segs[s].a == n;
        a.u = net.OutDir(s, n);
        a.ang = Angle(a.u);
        a.hw = p.Half();
        a.sideL = a.atA ? p.sideL : p.sideR; // looking outward from the b end, the segment's right is the arm's left
        a.sideR = a.atA ? p.sideR : p.sideL;
        arms.push_back(a);
    }
    std::sort(arms.begin(), arms.end(), [](const Arm& x, const Arm& y) { return x.ang < y.ang; });
    return arms;
}

// A node is a junction (gets a fill and trims its arms) with 3+ arms, or 2 arms meeting at an angle or with different widths.
inline bool IsJunction(const std::vector<Arm>& arms) {
    if (arms.size() >= 3) return true;
    if (arms.size() == 2) {
        const float c = Dot2(arms[0].u, arms[1].u);
        // gentle bends (under ~25 degrees) of the same width: a seamless mitred joint (ComputeTrims); sharper ones get
        // a rounded corner (trimmed arms and a curved fill, like any junction)
        return c > -0.906f || std::fabs(arms[0].hw - arms[1].hw) > 0.6f;
    }
    return false;
}

// Trim distances at a junction: far enough that neighbouring arms' edges do not overlap, plus a corner radius.
inline void TrimArms(const Network& net, int n, std::vector<Arm>& arms) {
    const size_t k = arms.size();
    for (size_t i = 0; i < k; ++i) {
        Arm& a = arms[i];
        float need = 0;
        for (int side : {-1, 1}) {
            const Arm& b = arms[(i + k + side) % k];
            if (&b == &a) continue;
            const float c = std::clamp(Dot2(a.u, b.u), -1.0f, 1.0f), th = std::acos(c), sn = std::sin(th);
            if (th > 3.0f) continue; // straight through: no conflict on this side
            need = std::max(need, (b.hw + a.hw * c) / std::max(sn, 0.17f));
        }
        const bool streetCorner = a.sideL > 0.5f || a.sideR > 0.5f;
        a.trim = std::max(need, a.hw * 0.6f) + (streetCorner ? 3.0f : 1.5f);
        const Cubic c = net.Curve(a.seg);
        const float len = c.Length();
        a.trim = std::min(a.trim, len * 0.45f);
        a.t = ParamAtDistance(c, a.trim, !a.atA);
        a.end = c.At(a.t);
        a.left = LeftOf(a.atA ? c.Tan(a.t) : c.Tan(a.t) * -1.0f);
    }
    (void)n;
}

// The whole network's trims (junction cuts).
inline std::vector<SegTrim> ComputeTrims(const Network& net, std::vector<std::vector<Arm>>* junctionArms = nullptr) {
    std::vector<SegTrim> trims(net.segs.size());
    if (junctionArms) junctionArms->assign(net.nodes.size(), {});
    for (size_t n = 0; n < net.nodes.size(); ++n) {
        if (!net.nodes[n].alive) continue;
        std::vector<Arm> arms = ArmsAt(net, (int)n);
        if (arms.size() == 2 && !IsJunction(arms)) { // mitre: both ends cut along the bisector of the two directions
            for (int k = 0; k < 2; ++k) {
                const Arm& me = arms[k];
                const Arm& other = arms[1 - k];
                const Cubic c = net.Curve(me.seg);
                const V2 T = me.atA ? c.Tan(0) : c.Tan(1);
                const V2 cont = me.atA ? other.u * -1.0f : other.u; // the other road's direction, travelling the same way
                const V2 m = Norm(T + cont);
                const V2 l = LeftOf(m) * (1.0f / std::max(0.5f, Dot2(m, T)));
                if (me.atA) trims[me.seg].miterA = true, trims[me.seg].leftA = l;
                else trims[me.seg].miterB = true, trims[me.seg].leftB = l;
            }
            continue;
        }
        if (!IsJunction(arms)) continue;
        TrimArms(net, (int)n, arms);
        for (const Arm& a : arms) {
            if (a.atA) trims[a.seg].t0 = a.t, trims[a.seg].junctionA = arms.size() >= 3;
            else trims[a.seg].t1 = a.t, trims[a.seg].junctionB = arms.size() >= 3;
        }
        if (junctionArms) (*junctionArms)[n] = std::move(arms);
    }
    return trims;
}

inline Vec3 MatColor(Mat m) {
    switch (m) {
    case Mat::Paint: return {0.92f, 0.92f, 0.9f};
    default: return {1, 1, 1};
    }
}

// Sweeps one segment's strips between its trims; markings, bridge, tunnel and quay pieces included.
inline void BuildSegment(const Network& net, int si, const SegTrim& trim, const GroundFn& ground, MeshOut& out) {
    using namespace mesh;
    const Segment& s = net.segs[si];
    const Profile& p = Profiles()[s.profile];
    const Cubic c = net.Curve(s);
    const float len = c.Length() * (trim.t1 - trim.t0);
    if (len < 0.05f) return;
    const int n = std::max(2, (int)std::ceil(len / 2.0f));
    // strip edges, left to right (left positive)
    std::vector<float> edge(p.strips.size() + 1);
    edge[0] = p.Half();
    for (size_t k = 0; k < p.strips.size(); ++k) edge[k + 1] = edge[k] - p.strips[k].w;
    struct Sample {
        V2 c, l;
        float y, g, dist;
        bool tunnel, elevated;
        float t = 0;
    };
    std::vector<Sample> sm(n + 1);
    float dist = 0;
    for (int i = 0; i <= n; ++i) {
        const float f = (float)i / n, t = trim.t0 + (trim.t1 - trim.t0) * f;
        Sample& q = sm[i];
        q.c = c.At(t);
        q.t = t;
        q.l = LeftOf(c.Tan(t));
        if (i == 0 && trim.miterA) q.l = trim.leftA;
        if (i == n && trim.miterB) q.l = trim.leftB;
        q.y = net.YAt(s, t);
        q.g = ground ? ground(q.c.x, q.c.z) : 0.0f;
        if (i) dist += Len(q.c - sm[i - 1].c);
        q.dist = dist;
        if (ground && std::fabs(q.y - q.g) < 1.5f) { // a ground road never dips under the terrain anywhere across its width
            float top = q.g;
            for (float off : {p.Half(), p.Half() * 0.5f, -p.Half() * 0.5f, -p.Half()}) {
                const V2 e = q.c + q.l * off;
                top = std::max(top, ground(e.x, e.z));
            }
            q.y = std::max(q.y, top + 0.02f);
        }
        q.tunnel = q.y < q.g - kTunnelDepth;
        q.elevated = q.y > q.g + kElevated;
    }
    auto at = [&](int i, float off) { return sm[i].c + sm[i].l * off; };
    const Vec3 white{1, 1, 1};
    // ---- lane attachments: which side's edge strips are cut away here (host), or whether we are over the host (ramp)
    int firstLane = -1, lastLane = -1;
    for (size_t k = 0; k < p.strips.size(); ++k)
        if (p.strips[k].kind == Kind::Lane) lastLane = (int)k, firstLane = firstLane < 0 ? (int)k : firstLane;
    auto cutSide = [&](const Sample& A, const Sample& B) { // +1 / -1: that side is open for a ramp; 0: none
        for (const SegTrim::Cut& ct : trim.cuts)
            if (std::max(A.t, B.t) >= ct.t0 && std::min(A.t, B.t) <= ct.t1) return ct.side;
        return 0.0f;
    };
    auto overHost = [&](const Sample& A) {
        if (trim.host < 0 || !net.segs[trim.host].alive) return false;
        float t, d;
        net.Nearest(trim.host, A.c, t, d);
        return d < net.Prof(trim.host).Half() - 0.2f;
    };
    // ---- strips, curb faces, skirts
    for (int i = 1; i <= n; ++i) {
        const Sample &A = sm[i - 1], &B = sm[i];
        if (A.tunnel && B.tunnel) continue;
        const float cut = cutSide(A, B);
        const bool onHost = overHost(A) || overHost(B);
        auto skip = [&](int k) {
            if (onHost) return p.strips[k].kind != Kind::Lane; // the ramp's lanes only, laid over the host's surface
            if (cut > 0) return firstLane >= 0 && k < firstLane;
            if (cut < 0) return lastLane >= 0 && k > lastLane;
            return false;
        };
        const float lift = onHost ? 0.03f : 0.0f;
        for (size_t k = 0; k < p.strips.size(); ++k) {
            if (skip((int)k)) continue;
            const Strip& st = p.strips[k];
            // on a bridge there is no greenery: verges become bare deck (headroom beside the parapet walls)
            const Mat mat = (A.elevated || B.elevated) && st.mat == Mat::Grass ? Mat::Concrete : st.mat;
            auto& o = out.byMat[(int)mat];
            const float tile = kMatTile[(int)mat], h = kRoadLift + st.h + lift;
            const float uv[8] = {edge[k] / tile, A.dist / tile, edge[k + 1] / tile, A.dist / tile, edge[k + 1] / tile, B.dist / tile, edge[k] / tile, B.dist / tile};
            Quad(o, V(at(i - 1, edge[k]), A.y + h), V(at(i - 1, edge[k + 1]), A.y + h), V(at(i, edge[k + 1]), B.y + h), V(at(i, edge[k]), B.y + h), white, uv);
            if (k + 1 < p.strips.size() && !skip((int)k + 1) && std::fabs(p.strips[k + 1].h - st.h) > 0.01f) { // curb face
                const float hi = std::max(st.h, p.strips[k + 1].h) + kRoadLift, lo = std::min(st.h, p.strips[k + 1].h) + kRoadLift;
                const bool leftHigher = st.h > p.strips[k + 1].h;
                const V2 ln = A.l * (leftHigher ? -1.0f : 1.0f);
                Wall(out.byMat[(int)Mat::Curb], V(at(i - 1, edge[k + 1]), 0), V(at(i, edge[k + 1]), 0), A.y + hi, B.y + hi, A.y + lo, B.y + lo, {ln.x, 0, ln.z}, white, 2);
            }
        }
        // outer edges: skirt / deck sides down to the deck bottom; quay wall deeper
        const float hL = kRoadLift + p.strips.front().h, hR = kRoadLift + p.strips.back().h;
        const bool quayL = p.strips.front().kind == Kind::QuayWall, quayR = p.strips.back().kind == Kind::QuayWall;
        const float downL = quayL ? 14.0f : kDeck, downR = quayR ? 14.0f : kDeck;
        auto& conc = out.byMat[(int)Mat::Concrete];
        if (onHost) continue; // the host's deck carries it here
        if (cut <= 0) Wall(conc, V(at(i - 1, edge.front()), 0), V(at(i, edge.front()), 0), A.y + hL, B.y + hL, A.y - downL, B.y - downL, {A.l.x, 0, A.l.z}, white, 3);
        if (cut >= 0) Wall(conc, V(at(i - 1, edge.back()), 0), V(at(i, edge.back()), 0), A.y + hR, B.y + hR, A.y - downR, B.y - downR, {-A.l.x, 0, -A.l.z}, white, 3);
        if (A.elevated || B.elevated) { // bridge: underside and parapets
            const float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
            Quad(conc, V(at(i - 1, edge.front()), A.y - kDeck), V(at(i - 1, edge.back()), A.y - kDeck), V(at(i, edge.back()), B.y - kDeck),
                 V(at(i, edge.front()), B.y - kDeck), {0.8f, 0.8f, 0.8f}, uv, {0, -1, 0});
            for (int side = 0; side < 2; ++side) {
                if ((side == 0 && cut > 0) || (side == 1 && cut < 0)) continue; // open where the ramp leaves
                const float off = side ? edge.back() + 0.15f : edge.front() - 0.15f, hh = side ? hR : hL;
                Wall(conc, V(at(i - 1, off), 0), V(at(i, off), 0), A.y + hh + 1.0f, B.y + hh + 1.0f, A.y + hh, B.y + hh, {A.l.x * (side ? -1.f : 1.f), 0, A.l.z * (side ? -1.f : 1.f)}, white, 3);
                Wall(conc, V(at(i - 1, off), 0), V(at(i, off), 0), A.y + hh + 1.0f, B.y + hh + 1.0f, A.y + hh, B.y + hh, {A.l.x * (side ? 1.f : -1.f), 0, A.l.z * (side ? 1.f : -1.f)}, white, 3);
            }
        }
    }
    // ---- supports, as modelled in assets/roads/roadlib.js (buildPillars / buildRetaining): 5 m or more above the ground, a cap beam
    // (0.85 W x 1.2 x 1.6) under the deck on 1.4 m square columns, ceil(W / 14) across spread over 0.6 W, one bent per
    // ~28 m; lower (0.6 .. 5 m) the deck sits on retaining walls down to the ground (an embankment)
    {
        constexpr float kPillarMin = 5.0f, kPillarStep = 28.0f, kParapetMin = 0.6f;
        auto& curb = out.byMat[(int)Mat::Curb];
        const int count = std::max(1, (int)std::round(len / kPillarStep)), cols = std::max(1, (int)std::ceil(p.width / 14.0f));
        for (int k = 0; k < count; ++k) {
            const float f = (k + 0.5f) / count;
            int i = std::clamp((int)std::round(f * n), 0, n);
            const Sample& q = sm[i];
            if (q.y - q.g < kPillarMin) continue;
            const V2 d = {-q.l.z, q.l.x};
            const float H = q.y - kDeck;
            mesh::Box(curb, q.c, LeftOf(d) * -1.0f, p.width * 0.85f, 1.6f, H - 1.2f, H, white, 2); // cap beam across
            for (int c = 0; c < cols; ++c) {
                const float o = cols == 1 ? 0.0f : ((float)c / (cols - 1) - 0.5f) * p.width * 0.6f;
                const V2 at2 = q.c + q.l * o;
                const float g = ground ? ground(at2.x, at2.z) : q.g;
                mesh::Box(curb, at2, d, 1.4f, 1.4f, g - 1.0f, H - 1.2f, white, 2); // column down into the ground
            }
        }
        auto& conc = out.byMat[(int)Mat::Concrete];
        for (int i = 1; i <= n; ++i) { // retaining walls on low decks
            const Sample &A = sm[i - 1], &B = sm[i];
            const float hA = A.y - A.g, hB = B.y - B.g;
            if (std::max(hA, hB) <= kParapetMin || std::min(hA, hB) > kPillarMin) continue;
            for (int side = 0; side < 2; ++side) {
                const float off = side ? edge.back() : edge.front();
                const V2 a2 = at(i - 1, off), b2 = at(i, off);
                const float gA = ground ? ground(a2.x, a2.z) : A.g, gB = ground ? ground(b2.x, b2.z) : B.g;
                const V2 nrm = A.l * (side ? -1.0f : 1.0f);
                Wall(conc, V(a2, 0), V(b2, 0), A.y - kDeck, B.y - kDeck, gA - 1.0f, gB - 1.0f, {nrm.x, 0, nrm.z}, white, 3);
            }
        }
    }
    // ---- tunnel portals where the road dives in / comes out
    for (int i = 1; i <= n; ++i) {
        if (sm[i - 1].tunnel == sm[i].tunnel) continue;
        const Sample& q = sm[i - 1].tunnel ? sm[i] : sm[i - 1];
        auto& conc = out.byMat[(int)Mat::Concrete];
        const V2 d = {-q.l.z, q.l.x};
        const float y0 = q.y - 0.5f, y1 = q.y + 7.5f;
        for (float side : {-1.0f, 1.0f}) mesh::Box(conc, q.c + q.l * (side * (p.Half() + 0.6f)), d, 2.0f, 1.2f, y0, y1, white, 3);
        mesh::Box(conc, q.c, LeftOf(d) * -1.0f, p.width + 2.4f, 2.0f, y1 - 1.2f, y1 + 0.3f, white, 3);
    }
    // ---- rail: one SPOM track box per track strip (TrackSweep; the relief carries rails, sleepers and ballast)
    for (size_t k = 0; k < p.strips.size(); ++k) {
        if (p.strips[k].kind != Kind::Track) continue;
        const float mid = 0.5f * (edge[k] + edge[k + 1]), top = kRoadLift + p.strips[k].h + kTrackDepth - 0.05f;
        std::vector<TrackPt> run;
        auto flush = [&] { if (run.size() >= 2) TrackSweep(out.byMat[(int)Mat::Track], run); run.clear(); };
        for (int i = 0; i <= n; ++i) {
            if (sm[i].tunnel) { flush(); continue; }
            const V2 f = Norm(sm[std::min(i + 1, n)].c - sm[std::max(i - 1, 0)].c);
            run.push_back({at(i, mid), sm[i].y + top, f, sm[i].dist});
        }
        flush();
    }
    // ---- markings (paint)
    auto& paint = out.byMat[(int)Mat::Paint];
    const Vec3 yellow{0.95f, 0.75f, 0.15f};
    auto paintRun = [&](float off, float wdt, Vec3 col, float dash, float gap) { // a line along the road, dashed when gap > 0
        for (int i = 1; i <= n; ++i) {
            const Sample &A = sm[i - 1], &B = sm[i];
            if (A.tunnel && B.tunnel) continue;
            if (gap > 0) {
                const float mid = 0.5f * (A.dist + B.dist);
                if (std::fmod(mid, dash + gap) > dash) continue;
            }
            const float uv[8] = {};
            Quad(paint, V(at(i - 1, off + wdt * 0.5f), A.y + kPaintLift), V(at(i - 1, off - wdt * 0.5f), A.y + kPaintLift),
                 V(at(i, off - wdt * 0.5f), B.y + kPaintLift), V(at(i, off + wdt * 0.5f), B.y + kPaintLift), col, uv);
        }
    };
    int laneIdx = 0;
    const bool paved = p.cat != Cat::Path && p.cat != Cat::Rail;
    for (size_t k = 0; k < p.strips.size() && paved; ++k) {
        const Strip& st = p.strips[k];
        if (st.kind != Kind::Lane || st.mat == Mat::Gravel) {
            if (st.kind == Kind::Parking) { // stall separators
                const float every = st.w >= 4.5f ? 2.6f : 6.0f;
                for (int i = 1; i <= n; ++i) {
                    const Sample &A = sm[i - 1], &B = sm[i];
                    if (std::floor(A.dist / every) == std::floor(B.dist / every)) continue;
                    const float uv[8] = {};
                    Quad(paint, V(at(i - 1, edge[k] - 0.1f), A.y + kPaintLift), V(at(i - 1, edge[k + 1] + 0.1f), A.y + kPaintLift),
                         V(at(i - 1, edge[k + 1] + 0.1f) + A.l * 0.0f + V2{-A.l.z, A.l.x} * 0.12f, A.y + kPaintLift),
                         V(at(i - 1, edge[k] - 0.1f) + V2{-A.l.z, A.l.x} * 0.12f, A.y + kPaintLift), white, uv);
                }
            }
            continue;
        }
        const bool nextLane = k + 1 < p.strips.size() && p.strips[k + 1].kind == Kind::Lane;
        const Kind prevKind = k > 0 ? p.strips[k - 1].kind : Kind::Verge, nextKind = k + 1 < p.strips.size() ? p.strips[k + 1].kind : Kind::Verge;
        if (nextLane) {
            if (laneIdx + 1 == p.back && p.fwd > 0) { // between the directions: double yellow
                paintRun(edge[k + 1] + 0.14f, 0.12f, yellow, 0, 0);
                paintRun(edge[k + 1] - 0.14f, 0.12f, yellow, 0, 0);
            } else paintRun(edge[k + 1], 0.13f, white, 3.0f, 6.0f);
        }
        if (prevKind == Kind::Shoulder || prevKind == Kind::Parking) paintRun(edge[k] - 0.15f, 0.15f, white, 0, 0);
        if (nextKind == Kind::Shoulder || nextKind == Kind::Parking) paintRun(edge[k + 1] + 0.15f, 0.15f, white, 0, 0);
        ++laneIdx;
    }
    // ---- direction arrows: one per side (direction of travel), centred on that side's lanes and sized to its width,
    // every 45 m. A 2+2 road shows one arrow each way, a 4-lane one-way a single wide arrow.
    if (paved) {
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f}; // [0] lanes driving b -> a, [1] a -> b
        int li = 0;
        for (size_t k = 0; k < p.strips.size(); ++k) {
            if (p.strips[k].kind != Kind::Lane) continue;
            const int d = li++ >= p.back ? 1 : 0;
            lo[d] = std::min(lo[d], std::min(edge[k], edge[k + 1]));
            hi[d] = std::max(hi[d], std::max(edge[k], edge[k + 1]));
        }
        for (int d = 0; d < 2; ++d) {
            if (hi[d] <= lo[d]) continue;
            const float centre = 0.5f * (lo[d] + hi[d]), w = hi[d] - lo[d];
            const float head = std::min(0.32f * w, 3.2f), shaft = std::max(0.12f, 0.07f * w), L = std::clamp(1.4f * w, 3.6f, 9.0f);
            for (float a = 20.0f + d * 22.5f; a < len - 6.0f; a += 45.0f) {
                int i = 0;
                while (i < n && sm[i].dist < a) ++i;
                const Sample& q = sm[i];
                if (q.tunnel) continue;
                const V2 fwd = V2{-q.l.z, q.l.x} * (d ? 1.0f : -1.0f), side = LeftOf(fwd), o = q.c + q.l * centre;
                const float y = q.y + kPaintLift, uv[8] = {}, back = -0.5f * L, neck = 0.5f * L - head * 1.1f, tip = 0.5f * L;
                Quad(out.arrows, V(o + fwd * back + side * shaft, y), V(o + fwd * back - side * shaft, y), V(o + fwd * neck - side * shaft, y), V(o + fwd * neck + side * shaft, y), white, uv);
                mesh::Tri(out.arrows, V(o + fwd * tip, y), V(o + fwd * neck + side * head, y), V(o + fwd * neck - side * head, y), {0, 1, 0}, white, 0, 0, 0, 0, 0, 0);
            }
        }
    }
    // ---- crosswalks at junction ends (streets with sidewalks)
    if (paved && (p.sideL > 1.0f || p.sideR > 1.0f)) {
        const float inner0 = edge.front() - p.sideL, inner1 = edge.back() + p.sideR;
        for (int end = 0; end < 2; ++end) {
            if (!(end ? trim.junctionB : trim.junctionA)) continue;
            const int i0 = end ? n : 0;
            const Sample& q = sm[i0];
            const V2 fwd = V2{-q.l.z, q.l.x} * (end ? -1.0f : 1.0f);
            for (float o = inner0 - 0.6f; o > inner1 + 0.3f; o -= 1.0f) {
                const V2 a = q.c + q.l * o + fwd * 0.8f;
                const float y = q.y + kPaintLift, uv[8] = {};
                Quad(paint, V(a + q.l * 0.25f, y), V(a - q.l * 0.25f, y), V(a - q.l * 0.25f + fwd * 3.0f, y), V(a + q.l * 0.25f + fwd * 3.0f, y), white, uv);
            }
        }
    }
}

// The junction fill at node n: asphalt fan inside the curb line, sidewalk corners with curved curbs.
inline void BuildJunction(const Network& net, int n, const std::vector<Arm>& arms, MeshOut& out) {
    using namespace mesh;
    if (arms.size() < 2) return;
    const Vec3 white{1, 1, 1};
    const float y = net.NodeY(n);
    const V2 C = XZ(net.nodes[n].p);
    const size_t k = arms.size();
    // pick the junction surface: the dominant arm's lane material (gravel stays gravel, paths stay paved)
    Mat road = Mat::Asphalt;
    {
        float widest = -1;
        for (const Arm& a : arms) {
            if (a.hw <= widest) continue;
            widest = a.hw;
            road = Mat::Asphalt;
            for (const Strip& st : net.Prof(a.seg).strips)
                if (st.kind == Kind::Lane || st.kind == Kind::Path || st.kind == Kind::Bike || st.kind == Kind::Track) { road = st.mat; break; }
        }
    }
    const bool raisedPath = road == Mat::Pavers || road == Mat::Concrete; // pedestrian streets / paths: surface at their own height
    bool railJunction = false; // a rail junction fill sits at the ballast's height
    for (const Arm& a : arms) railJunction = railJunction || net.Prof(a.seg).cat == Cat::Rail;
    const float roadH = kRoadLift + (raisedPath ? 0.04f : 0.0f) + (railJunction ? 0.1f : 0.0f);
    struct LP {
        V2 p;
        float y;
    };
    std::vector<LP> loop; // inner boundary, counter-clockwise, with each point's height (arms may slope)
    auto armY = [&](const Arm& a) { return net.YAt(net.segs[a.seg], a.t); };
    for (size_t i = 0; i < k; ++i) {
        const Arm& a = arms[i];
        const Arm& b = arms[(i + 1) % k];
        const float ya = armY(a), yb = armY(b);
        // arm a's right side faces arm b (counter-clockwise order)
        const V2 aOuter = a.end - a.left * a.hw, aInner = a.end - a.left * (a.hw - a.sideR);
        const V2 bOuter = b.end + b.left * b.hw, bInner = b.end + b.left * (b.hw - b.sideL);
        auto ctrl = [&](V2 p0, V2 d0, V2 p1, V2 d1) {
            V2 q;
            const float maxR = 3.0f * std::max(a.hw, b.hw) + 10.0f;
            if (!LineHit(p0, d0, p1, d1, q) || Len(q - C) > maxR || Dot2(q - p0, d0) > 0.5f) q = (p0 + p1) * 0.5f; // nearly parallel or behind
            return q;
        };
        const V2 qo = ctrl(aOuter, a.u, bOuter, b.u), qi = ctrl(aInner, a.u, bInner, b.u);
        const int m = 10;
        std::vector<V2> outer(m + 1), inner(m + 1);
        std::vector<float> ys(m + 1);
        for (int j = 0; j <= m; ++j) {
            const float t = (float)j / m, u = 1 - t;
            outer[j] = aOuter * (u * u) + qo * (2 * u * t) + bOuter * (t * t);
            inner[j] = aInner * (u * u) + qi * (2 * u * t) + bInner * (t * t);
            ys[j] = ya + (yb - ya) * t;
        }
        // arm a's end edge (left inner -> right inner), then the corner
        loop.push_back({a.end + a.left * (a.hw - a.sideL), ya});
        for (int j = 0; j <= m; ++j) loop.push_back({inner[j], ys[j]});
        // sidewalk corner band, at the side height of arm a's facing edge
        if (a.sideR > 0.05f || b.sideL > 0.05f) {
            const Profile& pa = net.Prof(a.seg);
            const Strip& edgeStrip = a.atA ? pa.strips.back() : pa.strips.front();
            const Mat sideMat = edgeStrip.kind == Kind::Verge ? Mat::Grass : Mat::Concrete;
            const float sh = kRoadLift + std::max(0.0f, edgeStrip.kind == Kind::Verge ? 0.0f : edgeStrip.h > 0.5f ? 0.15f : edgeStrip.h);
            for (int j = 1; j <= m; ++j)
                FlatQuad(out.byMat[(int)sideMat], V(outer[j - 1], ys[j - 1] + sh), V(inner[j - 1], ys[j - 1] + sh), V(inner[j], ys[j] + sh), V(outer[j], ys[j] + sh), white,
                         kMatTile[(int)sideMat]);
            if (sh > roadH + 0.01f)
                for (int j = 1; j <= m; ++j) {
                    const V2 mid = (inner[j - 1] + inner[j]) * 0.5f, dir = C - mid;
                    Wall(out.byMat[(int)Mat::Curb], V(inner[j - 1], 0), V(inner[j], 0), ys[j - 1] + sh, ys[j] + sh, ys[j - 1] + roadH, ys[j] + roadH, {dir.x, 0, dir.z}, white, 2);
                }
            for (int j = 1; j <= m; ++j) { // outer skirt
                const V2 mid = (outer[j - 1] + outer[j]) * 0.5f - C;
                Wall(out.byMat[(int)Mat::Concrete], V(outer[j - 1], 0), V(outer[j], 0), ys[j - 1] + sh, ys[j] + sh, ys[j - 1] - kDeck, ys[j] - kDeck, {mid.x, 0, mid.z}, white, 3);
            }
        }
    }
    auto& o = out.byMat[(int)road];
    const float tile = kMatTile[(int)road];
    float yc = 0;
    for (const Arm& a : arms) yc += armY(a);
    yc = 0.5f * (y + yc / (float)k);
    for (size_t i = 0; i < loop.size(); ++i) {
        const LP p0 = loop[i], p1 = loop[(i + 1) % loop.size()];
        auto lifted = [&](V2 q, float yy) { // ground junctions stay over the terrain too
            if (!net.ground) return yy;
            const float g = net.ground(q.x, q.z);
            return std::fabs(yy - g) < 1.5f ? std::max(yy, g + 0.02f) : yy;
        };
        const Vec3 a = V(C, lifted(C, yc) + roadH), b = V(p0.p, lifted(p0.p, p0.y) + roadH), c = V(p1.p, lifted(p1.p, p1.y) + roadH);
        mesh::Tri(o, a, b, c, {0, 1, 0}, white, a.x / tile, a.z / tile, b.x / tile, b.z / tile, c.x / tile, c.z / tile);
    }
}

// A half-round cap past a dead end: road surface inside, the side band (sidewalk / verge) around it, a curb, a skirt.
inline void BuildCap(const Network& net, int n, MeshOut& out) {
    using namespace mesh;
    const int s = net.nodes[n].segs[0];
    const Profile& p = net.Prof(s);
    const bool atA = net.segs[s].a == n;
    const V2 C = XZ(net.nodes[n].p), u = net.OutDir(s, n), back = u * -1.0f, left = LeftOf(u);
    float y = net.YAt(net.segs[s], atA ? 0.0f : 1.0f);
    const float hw = p.Half();
    if (net.ground) { // like the segment: a ground cap stays over the terrain
        const float g = net.ground(C.x, C.z);
        if (std::fabs(y - g) < 1.5f) {
            float top = g;
            for (float off : {hw, hw * 0.5f, -hw * 0.5f, -hw}) { // exactly the segment's end sample, so they meet flush
                const V2 q = C + LeftOf(u) * off;
                top = std::max(top, net.ground(q.x, q.z));
            }
            y = std::max(y, top + 0.02f);
        }
    }
    const float side = std::max(atA ? p.sideL : p.sideR, atA ? p.sideR : p.sideL);
    const float inner = std::max(0.5f, hw - side);
    Mat road = Mat::Asphalt;
    for (const Strip& st : p.strips)
        if (st.kind == Kind::Lane || st.kind == Kind::Path || st.kind == Kind::Bike || st.kind == Kind::Track) { road = st.mat; break; }
    const Strip& edge = p.strips.front();
    const Mat sideMat = edge.kind == Kind::Verge ? Mat::Grass : Mat::Concrete;
    const float sh = kRoadLift + (edge.kind == Kind::Verge ? 0.0f : std::min(edge.h, 0.15f)), rh = kRoadLift + (p.cat == Cat::Rail ? 0.1f : 0.0f);
    const Vec3 white{1, 1, 1};
    const int m = 16;
    auto pt = [&](float r, int j) { // j = 0 .. m sweeps from the arm's left edge round the back to its right edge
        const float a = 3.14159265f * j / m;
        return C + left * (std::cos(a) * r) + back * (std::sin(a) * r);
    };
    for (int j = 1; j <= m; ++j) {
        const V2 a = pt(inner, j - 1), b = pt(inner, j);
        Tri(out.byMat[(int)road], V(C, y + rh), V(a, y + rh), V(b, y + rh), {0, 1, 0}, white, C.x / 4, C.z / 4, a.x / 4, a.z / 4, b.x / 4, b.z / 4);
        if (side > 0.05f) {
            const V2 oa = pt(hw, j - 1), ob = pt(hw, j);
            FlatQuad(out.byMat[(int)sideMat], V(a, y + sh), V(oa, y + sh), V(ob, y + sh), V(b, y + sh), white, kMatTile[(int)sideMat]);
            const V2 mid = (a + b) * 0.5f, toC = C - mid;
            if (sh > rh + 0.01f) Wall(out.byMat[(int)Mat::Curb], V(a, 0), V(b, 0), y + sh, y + sh, y + rh, y + rh, {toC.x, 0, toC.z}, white, 2);
        }
        const V2 oa = pt(hw, j - 1), ob = pt(hw, j), mo = (oa + ob) * 0.5f - C;
        Wall(out.byMat[(int)Mat::Concrete], V(oa, 0), V(ob, 0), y + sh, y + sh, y - kDeck, y - kDeck, {mo.x, 0, mo.z}, white, 3);
    }
}

// Everything: segments between their trims, then junction fills.
inline void BuildNetwork(const Network& net, const GroundFn& ground, MeshOut& out) {
    for (auto& v : out.byMat) v.clear();
    out.arrows.clear();
    std::vector<std::vector<Arm>> junctions;
    std::vector<SegTrim> trims = ComputeTrims(net, &junctions);
    // lane attachments: a dead end sitting on another road's surface is a ramp leaving / joining that road's lane
    std::vector<char> attached(net.nodes.size(), 0);
    for (size_t nd = 0; nd < net.nodes.size(); ++nd) {
        if (!net.nodes[nd].alive || net.nodes[nd].segs.size() != 1) continue;
        const int own = net.nodes[nd].segs[0];
        const V2 np = XZ(net.nodes[nd].p);
        int host = -1;
        float ht = 0, hd = 1e9f;
        for (size_t h = 0; h < net.segs.size(); ++h) {
            if ((int)h == own || !net.segs[h].alive) continue;
            float t, d;
            net.Nearest((int)h, np, t, d);
            if (d < net.Prof((int)h).Half() && d < hd && std::fabs(net.YAt(net.segs[h], t) - net.NodeY((int)nd)) < 1.5f) host = (int)h, ht = t, hd = d;
        }
        if (host < 0) continue;
        attached[nd] = 1;
        trims[own].host = host;
        const Cubic hc = net.Curve(host), oc = net.Curve(own);
        const float side = Dot2(np - hc.At(ht), LeftOf(hc.Tan(ht))) >= 0 ? 1.0f : -1.0f;
        float t0 = ht, t1 = ht;
        const float reach = net.Prof(host).Half() + net.Prof(own).Half();
        for (int k = 0; k <= 32; ++k) {
            const V2 q = oc.At(k / 32.0f);
            float t, d;
            net.Nearest(host, q, t, d);
            if (d < reach) t0 = std::min(t0, t), t1 = std::max(t1, t);
        }
        trims[host].cuts.push_back({t0, t1, side});
    }
    for (size_t s = 0; s < net.segs.size(); ++s)
        if (net.segs[s].alive) BuildSegment(net, (int)s, trims[s], ground, out);
    for (size_t n = 0; n < net.nodes.size(); ++n)
        if (net.nodes[n].alive && !junctions[n].empty()) {
            const float g = ground ? ground(net.nodes[n].p.x, net.nodes[n].p.z) : 0.0f;
            if (net.NodeY((int)n) < g - mesh::kTunnelDepth) continue; // an underground junction
            BuildJunction(net, (int)n, junctions[n], out);
        }
    for (size_t n = 0; n < net.nodes.size(); ++n) // rounded dead ends (CS2's cul-de-sac caps)
        if (net.nodes[n].alive && net.nodes[n].segs.size() == 1 && !attached[n]) {
            const float g = ground ? ground(net.nodes[n].p.x, net.nodes[n].p.z) : 0.0f;
            if (net.NodeY((int)n) < g - mesh::kTunnelDepth) continue;
            BuildCap(net, (int)n, out);
        }
}

} // namespace city
} // namespace tbd
