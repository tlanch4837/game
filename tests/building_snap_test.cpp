// Headless checks for the shared building road snap (src/city/building_snap.h) and its use by the placement tools:
//   a ghost near a road snaps beside it with its entrance toward the road, for roads running all four ways and the cursor
//   on either side; the footprint never overlaps the carriageway or sidewalks; it slides along the road with the cursor;
//   quarter turns turn it from facing the road; far from every road it stays free; with the snap off (the Rotate dial out)
//   it is free too; validation passes on the snapped result (the Telecom Center on a road, the case that used to say
//   "A road or railway is in the way"); industry hubs use the same snap.
// Build and run: tests\building_snap_test.bat
#include <cmath>
#include <cstdio>

#include "city/industry.h"
#include "city/road_tool.h"
#include "city/transport.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Index(const char* name) {
    const auto& d = Defs();
    for (size_t i = 0; i < d.size(); ++i)
        if (std::string(d[i].name) == name) return (int)i;
    return -1;
}
// the smallest distance from the road's centre line (the segment's chord) to the footprint's outline
static float Clearance(V2 a, V2 b, V2 c, V2 front, float W, float D) {
    V2 q[4];
    Corners(c, front, W, D, q);
    const V2 u = Norm(b - a);
    float best = 1e30f;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k <= 40; ++k) {
            const V2 p = Lerp2(q[i], q[(i + 1) % 4], k / 40.0f);
            const float along = Dot2(p - a, u);
            if (along < 0 || along > Len(b - a)) continue;
            best = std::min(best, std::fabs(Cross2(p - a, u)));
        }
    return best;
}

int main() {
    const int road = ProfileIndex("Two-Lane Road");
    const float half = Profiles()[road].Half();
    // ------------------------------------------------------------------ four road directions, both sides
    {
        const V2 dirs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        const float W = 48.0f, D = 32.0f; // the Telecom Center's 6 x 4 cells
        for (const V2& d : dirs) {
            RoadTool t;
            const V2 a = d * -300.0f, b = d * 300.0f;
            t.BuildRoad(Cubic::Line(a, b), 0, 0, road);
            for (float sideSign : {1.0f, -1.0f}) {
                const V2 n = LeftOf(d) * sideSign;            // the side of the road the cursor is on
                const V2 cursor = d * 37.0f + n * 12.0f;      // 12 m off the centre line
                const BuildingSnap s = SnapBesideRoad(t.net, cursor, W, D, 0, 0);
                CHECK(s.snapped);
                CHECK(Dot2(EdgeNormal(s.front, 0), n * -1.0f) > 0.999f); // the entrance (the front) faces the road
                CHECK(s.roadEdge == 0);
                CHECK(Dot2(s.c, n) > 0);                                  // on the cursor's side
                CHECK(std::fabs(Dot2(s.c - cursor, d)) < 0.05f);          // slid along the road with the cursor
                const float clear = Clearance(a, b, s.c, s.front, W, D);
                CHECK(clear >= half + kBuildingSnapGap - 0.05f && clear <= half + kBuildingSnapGap + 0.05f); // off the kerb, never on it
                // the cursor moves along the road: the ghost follows
                const BuildingSnap s2 = SnapBesideRoad(t.net, cursor + d * 20.0f, W, D, 0, 0);
                CHECK(s2.snapped && std::fabs(Dot2(s2.c - s.c, d) - 20.0f) < 0.05f);
                // quarter turns from facing the road: 90 puts a side to it, 180 the back; still clear of the road
                for (int q = 1; q < 4; ++q) {
                    const BuildingSnap r = SnapBesideRoad(t.net, cursor, W, D, q, 0);
                    CHECK(r.snapped && r.roadEdge == (q == 2 ? 2 : q == 1 ? 3 : 1));
                    CHECK(Clearance(a, b, r.c, r.front, W, D) >= half + kBuildingSnapGap - 0.05f);
                }
                // a catalog entrance on a side edge: that side faces the road
                const BuildingSnap e1 = SnapBesideRoad(t.net, cursor, W, D, 0, 1);
                CHECK(e1.snapped && Dot2(EdgeNormal(e1.front, 1), n * -1.0f) > 0.999f && e1.roadEdge == 1);
            }
            // far away: free
            CHECK(!SnapBesideRoad(t.net, LeftOf(d) * 400.0f, W, D).snapped);
        }
    }

    // ------------------------------------------------------------------ the transport / service tool on the snapped result
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 4, 4, road);
        TransportWorld w;
        PlaceContext cx;
        cx.net = &t.net;
        cx.ground = [](float, float) { return 4.0f; };
        const int tc = Index("Telecom Center");
        CHECK(tc >= 0);
        // the cursor right on the road: before, "A road or railway is in the way"; now it stands beside it
        PlanOptions dial;
        dial.snapRoad = false; // the Rotate dial is out
        const Placement freeP = w.Plan(cx, tc, {20, 1}, 0, dial);
        std::printf("free on the road: %s\n", freeP.error.c_str());
        CHECK(!freeP.valid && !freeP.roadSnapped && freeP.error.find("in the way") != std::string::npos);
        CHECK(std::fabs(freeP.c.x - 20) < 1e-3f && std::fabs(freeP.c.z - 1) < 1e-3f); // free: where the cursor is
        const Placement p = w.Plan(cx, tc, {20, 1}, 0);
        CHECK(p.valid && p.roadSnapped && p.snapped);
        CHECK(p.entranceEdge == 0 && p.roadSeg >= 0 && Dot2(p.entranceDir, V2{0, -std::copysign(1.0f, p.c.z)}) > 0.999f);
        CHECK(std::fabs(std::fabs(p.c.z) - (half + kBuildingSnapGap + Defs()[tc].D() * 0.5f)) < 0.05f);
        // dialled to 30 degrees with the dial out: free heading, no snap
        const Placement turned = w.Plan(cx, tc, {20, 80}, 30, dial);
        CHECK(!turned.roadSnapped && std::fabs(turned.front.x - std::sin(30 * kDegToRad)) < 1e-3f);
        // far from roads: free as before, valid (a service building needs no road to be placed; a rail yard does)
        const Placement farP = w.Plan(cx, tc, {20, 600}, 0);
        CHECK(farP.valid && !farP.roadSnapped && farP.entranceEdge < 0);
        // each flat-site service follows the same rule
        for (const char* name : {"Fire Station", "Family Clinic", "Police Station", "Elementary School", "Cell Tower", "Bus Depot", "Small Cemetery"}) {
            const int d = Index(name);
            const Placement q = w.Plan(cx, d, {-120, -3}, 0);
            CHECK(d >= 0 && q.valid && q.roadSnapped && q.entranceEdge == 0);
        }
        // harbours keep the coast rule, stations their track (no road snap for them)
        CHECK(!w.Plan(cx, Index("Train Station"), {20, 1}, 0).roadSnapped);
    }

    // ------------------------------------------------------------------ industry hubs share the snap
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({0, -300}, {0, 300}), 0, 0, road);
        IndustryWorld iw;
        IndSite s;
        CHECK(iw.Snap(t.net, {-10, 40}, s, 0));
        CHECK(s.front.x > 0.999f);                                                 // faces the road (east of it)
        CHECK(std::fabs(s.c.x + (half + 0.5f + kHubD * 0.5f)) < 0.05f && std::fabs(s.c.z - 40) < 0.05f);
        CHECK(!iw.Snap(t.net, {-400, 40}, s, 0));
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
