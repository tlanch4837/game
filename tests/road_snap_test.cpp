// Headless checks for the road tool's auto snap (src/city/road_snap.h, RoadTool::SnapAt): a road along a building's
// entrance edge (offset = the road's half width with sidewalks, parallel, sidewalk flush with the footprint, any
// rotation and road type, carried on as it is dragged), angle steps, parallel / perpendicular to the nearest road, the
// extension of a dead end, length rounding, equal spacing, the priority between kinds and free placement (snap off).
// Build and run: tests\road_snap_test.bat
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "city/road_tool.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool Is(const RoadTool& t, const char* k) { return t.cursor.kind && std::strcmp(t.cursor.kind, k) == 0; }
static int RoadAsset(const char* profile) {
    const int p = ProfileIndex(profile);
    for (size_t i = 0; i < Assets().size(); ++i)
        if (Assets()[i].kind == AssetKind::Road && Assets()[i].profile == p) return (int)i;
    return -1;
}
static void Start(RoadTool& t, V2 o) {
    Snap s;
    s.p = {o.x, 0, o.z};
    t.pts.assign(1, s);
}
static V2 Dir(float deg) { return {std::cos(deg * kDegToRad), std::sin(deg * kDegToRad)}; }
static float DegOf(V2 d) { return std::atan2(d.z, d.x) / kDegToRad; }

// ---- (1) along a building's entrance edge
static void AccessTests() {
    const char* roads[] = {"Two-Lane Road", "Four-Lane Divided Road", "Gravel Road", "Six-Lane Divided Road"};
    const float rots[] = {0.0f, 30.0f, 77.0f, 135.0f, 210.0f, 333.0f};
    int cases = 0;
    for (const char* rn : roads) {
        const int asset = RoadAsset(rn);
        if (asset < 0) continue;
        for (float rot : rots)
            for (int e = 0; e < 4; ++e) {
                RoadTool t;
                t.Open();
                t.Select(asset);
                const float half = t.Prof().Half();
                BuildingLot b{{100, -50}, FrontFromDeg(rot), 24, 16, e};
                t.lots.push_back(b);
                V2 mid[4], out[4];
                EdgeMids(b.c, b.front, b.W, b.D, mid, out);
                const V2 n = out[e], along{-n.z, n.x};
                // the cursor a little off the line, beside the edge
                const V2 c0 = mid[e] + n * (half + 2.5f) + along * 3.0f;
                t.Move(true, {c0.x, 0, c0.z});
                CHECK(Is(t, "access"));
                const V2 p0 = XZ(t.cursor.p);
                const float off = Dot2(p0 - mid[e], n);
                CHECK(std::fabs(off - half) < 1e-3f); // centre line = half width (carriageway + sidewalks) from the edge
                CHECK(std::fabs(Dot2((p0 - n * half) - mid[e], n)) < 1e-3f); // the sidewalk's outer edge lies on the footprint's edge
                CHECK(std::fabs(half - t.Prof().width * 0.5f) < 1e-5f); // the profile's whole width: sidewalks included
                CHECK(std::fabs(Dot2(p0 - mid[e], along) - 3.0f) < 1e-3f); // it slides along with the cursor
                // place the start, drag on along the edge (off the line a bit): still on it, parallel, same offset
                t.Click();
                CHECK(t.Pending() && t.pts.back().lot == 0);
                const V2 c1 = p0 + along * 47.0f + n * 1.8f;
                t.Move(true, {c1.x, 0, c1.z});
                CHECK(Is(t, "access"));
                const V2 p1 = XZ(t.cursor.p), d = p1 - p0;
                CHECK(std::fabs(Cross2(Norm(d), along)) < 1e-4f);        // parallel to the edge
                CHECK(std::fabs(Dot2(p1 - mid[e], n) - half) < 1e-3f);   // the same offset
                CHECK(std::fabs(std::fmod(Len(d) + 1e-3f, t.lengthStep)) < 2e-3f); // whole length steps
                // free placement: no snap at all
                t.snapAll = false;
                t.Move(true, {c1.x, 0, c1.z});
                CHECK(!Is(t, "access") && Len(XZ(t.cursor.p) - c1) < 1e-4f);
                ++cases;
            }
    }
    CHECK(cases >= 48);
    // the snap toggle off before the first point: the start is not pulled onto the line either
    RoadTool t;
    t.Open();
    t.lots.push_back({{0, 0}, {0, 1}, 20, 20, 0});
    t.snapAll = false;
    t.Move(true, {2, 0, 10 + t.Prof().Half() + 1});
    CHECK(!Is(t, "access"));
    std::printf("access: %d cases\n", cases);
}

// ---- (2) angle steps from the previous point (world axes here) and (5) length rounding
static void AngleTests() {
    const struct { float step, aim, want; } k[] = {{15, 31, 30}, {15, 62, 60}, {30, 58, 60}, {45, 43, 45}, {45, 133, 135}, {90, 88, 90}, {90, -2, 0}};
    for (const auto& c : k) {
        RoadTool t;
        t.Open();
        t.angleStep = c.step;
        Start(t, {0, 0});
        const V2 p = Dir(c.aim) * 50.0f;
        t.Move(true, {p.x, 0, p.z});
        CHECK(Is(t, "length"));
        const V2 q = XZ(t.cursor.p);
        CHECK(std::fabs(DegOf(q) - c.want) < 0.01f);
        CHECK(std::fabs(t.cursor.angleDeg - std::fabs(c.want)) < 0.01f);
        CHECK(std::fabs(Len(q) - 48.0f) < 1e-3f); // 50 m rounds to 6 cells
        CHECK(!t.guides.empty());                 // the ray and the protractor
    }
    // a step that does not fit stays free of the angle snap (45: 22 deg is 23 deg off at 50 m)
    {
        RoadTool t;
        t.Open();
        t.angleStep = 45;
        t.snaps &= ~(uint32_t)SnapGuide;
        Start(t, {0, 0});
        const V2 p = Dir(22) * 50.0f;
        t.Move(true, {p.x, 0, p.z});
        CHECK(!Is(t, "length") && !Is(t, "angle"));
    }
    // the angle step cycles 15 -> 30 -> 45 -> 90 -> 15
    RoadTool t;
    const float cycle[] = {30, 45, 90, 15};
    for (float want : cycle) {
        t.CycleAngleStep();
        CHECK(t.angleStep == want);
    }
    CHECK(RoundLength(43, 8) == 40 && RoundLength(45, 8) == 48 && RoundLength(2, 8) == 8 && RoundLength(13, 5) == 15);
    // length off: the snapped direction keeps the cursor's own length
    RoadTool u;
    u.Open();
    u.snaps &= ~(uint32_t)SnapLength;
    Start(u, {0, 0});
    u.Move(true, {50, 0, 0.5f});
    CHECK(Is(u, "angle") && std::fabs(u.cursor.p.z) < 1e-4f && std::fabs(Len(XZ(u.cursor.p)) - 50.0025f) < 0.01f);
}

// ---- (3) parallel / perpendicular to the nearest road
static void AlignTests() {
    for (float roadDeg : {20.0f, 52.0f, -37.0f}) {
        for (int perp = 0; perp < 2; ++perp) {
            RoadTool t;
            t.Open();
            t.angleStep = 90; // the world axes only snap at 0 / 90 here, so the road's direction must win
            const V2 u = Dir(roadDeg), c = LeftOf(u) * -80.0f;
            t.BuildRoad(Cubic::Line(c - u * 600.0f, c + u * 600.0f), 0, 0, ProfileIndex("Two-Lane Road"));
            Start(t, {0, 0});
            const float aim = roadDeg + (perp ? 90.0f : 0.0f) + 1.0f;
            const V2 p = Dir(aim) * 40.0f;
            t.Move(true, {p.x, 0, p.z});
            CHECK(Is(t, perp ? "perpendicular" : "parallel"));
            const V2 d = Norm(XZ(t.cursor.p));
            CHECK(std::fabs(perp ? Dot2(d, u) : Cross2(d, u)) < 1e-4f);
        }
    }
}

// ---- (7) the extension line of a dead end, (6) equal spacing, priority, free placement
static void LineTests() {
    {
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-100, 0}, {0, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        t.Move(true, {40, 0, 1.5f});
        CHECK(Is(t, "extension"));
        CHECK(std::fabs(t.cursor.p.z) < 1e-4f && std::fabs(t.cursor.p.x - 40) < 1e-3f);
        t.snapAll = false;
        t.Move(true, {40, 0, 1.5f});
        CHECK(std::fabs(t.cursor.p.z - 1.5f) < 1e-4f && !(t.cursor.kind && t.cursor.kind[0]));
    }
    {
        RoadTool t;
        t.Open();
        const int two = ProfileIndex("Two-Lane Road");
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        t.BuildRoad(Cubic::Line({-200, 44}, {200, 44}), 0, 0, two);
        t.Move(true, {0, 0, -42.8f});
        CHECK(Is(t, "spacing"));
        CHECK(std::fabs(t.cursor.p.z + 44.0f) < 1e-3f);
    }
    { // priority: a road's open end beats the building's access line beside it
        RoadTool t;
        t.Open();
        const float half = t.Prof().Half();
        t.BuildRoad(Cubic::Line({-100, 10 + half}, {0, 10 + half}), 0, 0, ProfileIndex("Two-Lane Road"));
        t.lots.push_back({{14, 0}, {0, 1}, 20, 20, 0}); // its entrance edge runs x 4..24, the end is at x 0
        t.Move(true, {1.0f, 0, 10 + half + 1.0f});
        CHECK(Is(t, "end"));
        t.Move(true, {22.0f, 0, 10 + half + 1.0f}); // further from the end, on the edge: the access line
        CHECK(Is(t, "access") && std::fabs(t.cursor.p.z - (10 + half)) < 1e-3f);
    }
    { // priority: the access line beats the angle snap once it has the point
        RoadTool t;
        t.Open();
        const float half = t.Prof().Half();
        t.lots.push_back({{0, 0}, {0, 1}, 30, 20, 0});
        Start(t, {-60, 10 + half + 6});
        t.Move(true, {2, 0, 10 + half + 1.5f});
        CHECK(Is(t, "access"));
    }
}

// ---- [T] clean T junctions and X crossings; [parallel] close parallel roads with a crossable median
static int Deg(const Network& n, int node) {
    int k = 0;
    for (int s : n.nodes[node].segs) k += n.segs[s].alive ? 1 : 0;
    return k;
}
static int LiveCount(const Network& n) {
    int k = 0;
    for (const auto& s : n.segs) k += s.alive ? 1 : 0;
    return k;
}
static bool StraightSeg(const Network& n, int s) { // its control points lie on the chord: no blend near either end
    const Cubic c = n.Curve(s);
    const V2 u = Norm(c.p3 - c.p0);
    return std::fabs(Cross2(c.p1 - c.p0, u)) < 0.05f && std::fabs(Cross2(c.p2 - c.p0, u)) < 0.05f;
}
static bool Connected(const Network& n, int from, int to) {
    std::vector<char> seen(n.nodes.size(), 0);
    std::vector<int> stack{from};
    seen[from] = 1;
    while (!stack.empty()) {
        const int x = stack.back();
        stack.pop_back();
        if (x == to) return true;
        for (int s : n.nodes[x].segs) {
            if (!n.segs[s].alive) continue;
            const int y = n.segs[s].a == x ? n.segs[s].b : n.segs[s].a;
            if (!seen[y]) seen[y] = 1, stack.push_back(y);
        }
    }
    return false;
}
static void JunctionTests() {
    const int two = ProfileIndex("Two-Lane Road");
    { // onto a road's side 14 deg off square: slides to the foot, meets it square, the old road splits there
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        Start(t, {10, 60});
        t.Move(true, {25, 0, 0.5f});
        CHECK(Is(t, "road") && std::fabs(t.cursor.p.x - 10) < 0.05f && std::fabs(t.cursor.p.z) < 0.05f && t.cursor.angleDeg == 90.0f);
        t.Click();
        CHECK(LiveCount(t.net) == 3);
        const int j = t.net.NearestNode({10, 0}, 0.2f);
        CHECK(j >= 0 && Deg(t.net, j) == 3);
        for (size_t s = 0; s < t.net.segs.size(); ++s) {
            if (!t.net.segs[s].alive) continue;
            CHECK(StraightSeg(t.net, (int)s));
            const Cubic c = t.net.Curve((int)s);
            if (std::fabs(c.p0.z - c.p3.z) > 1) CHECK(std::fabs(Norm(c.p3 - c.p0).x) < 1e-3f); // the new road: square
        }
    }
    { // 40 deg off square: the real angle stays (a Y), still a junction on the road
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        Start(t, {10, 60});
        t.Move(true, {60.3f, 0, 0.5f});
        CHECK(Is(t, "road") && std::fabs(t.cursor.p.x - 60.3f) < 0.6f && t.cursor.angleDeg < 0);
    }
    { // leaving a road's side 14 deg off square: the piece goes out square
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        t.Move(true, {10, 0, 0.3f});
        CHECK(Is(t, "road"));
        t.Click();
        t.Move(true, {25, 0, 60});
        CHECK(Is(t, "perpendicular") && std::fabs(t.cursor.p.x - 10) < 0.05f);
        t.Click();
        const int j = t.net.NearestNode({10, 0}, 0.5f);
        CHECK(j >= 0 && Deg(t.net, j) == 3 && LiveCount(t.net) == 3);
    }
    { // a crossing: an X with four straight arms
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        Start(t, {5, -60});
        t.Move(true, {5, 0, 60});
        t.Click();
        const int x = t.net.NearestNode({5, 0}, 0.5f);
        CHECK(x >= 0 && Deg(t.net, x) == 4 && LiveCount(t.net) == 4);
        for (size_t s = 0; s < t.net.segs.size(); ++s)
            if (t.net.segs[s].alive) CHECK(StraightSeg(t.net, (int)s));
    }
    { // parallel, 2 m apart edge to edge: both straight, a crossing at each end, a median in the gap, one network
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        const float half = Profiles()[two].Half(), D = 2 * half + 2.0f;
        t.snaps &= ~(uint32_t)(SnapGuide | SnapNetSide | SnapLength); // place exactly where aimed
        Start(t, {-50, D});
        t.Move(true, {50, 0, D});
        CHECK(std::fabs(t.cursor.p.z - D) < 1e-3f);
        t.Click();
        CHECK(t.net.medians.size() == 1);
        if (!t.net.medians.empty()) {
            const Network::Median& m = t.net.medians[0];
            CHECK(std::fabs(m.w - 1.8f) < 0.01f && std::fabs(m.a.z - (half + 1.0f)) < 0.01f && MedianLive(t.net, m));
        }
        const int a = t.net.NearestNode({-50, D}, 0.5f), b = t.net.NearestNode({50, D}, 0.5f);
        const int fa = t.net.NearestNode({-50, 0}, 0.5f), fb = t.net.NearestNode({50, 0}, 0.5f);
        CHECK(a >= 0 && b >= 0 && fa >= 0 && fb >= 0);
        if (a >= 0 && fa >= 0 && b >= 0 && fb >= 0) {
            CHECK(Deg(t.net, a) == 2 && Deg(t.net, b) == 2);   // the new road and its crossing
            CHECK(Deg(t.net, fa) == 3 && Deg(t.net, fb) == 3); // T junctions on the old road
            CHECK(Connected(t.net, t.net.NearestNode({-200, 0}, 0.5f), a)); // a path across the median
        }
        for (size_t s = 0; s < t.net.segs.size(); ++s)
            if (t.net.segs[s].alive) CHECK(StraightSeg(t.net, (int)s));
        MeshOut mo;
        BuildNetwork(t.net, {}, mo);
        CHECK(mo.byMat[(int)Mat::Grass].size() >= 30); // the median's box
        // bulldozing the old road drops the median
        for (size_t s = 0; s < t.net.segs.size(); ++s) {
            const Cubic c = t.net.Curve((int)s);
            if (t.net.segs[s].alive && std::fabs(c.p0.z) < 0.1f && std::fabs(c.p3.z) < 0.1f) t.net.RemoveSeg((int)s);
        }
        CHECK(!MedianLive(t.net, t.net.medians[0]));
    }
    { // nearly touching: the second road snaps flush, sidewalk to sidewalk
        RoadTool t;
        t.Open();
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, two);
        const float half = Profiles()[two].Half();
        t.Move(true, {0, 0, 2 * half + 0.6f});
        CHECK(Is(t, "flush") && std::fabs(t.cursor.p.z - 2 * half) < 1e-3f);
        t.snapAll = false;
        t.Move(true, {0, 0, 2 * half + 0.6f});
        CHECK(std::fabs(t.cursor.p.z - (2 * half + 0.6f)) < 1e-4f);
    }
}

int main() {
    AccessTests();
    AngleTests();
    AlignTests();
    LineTests();
    JunctionTests();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
