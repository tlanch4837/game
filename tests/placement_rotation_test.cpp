// Headless checks for turning a building's footprint while it is placed (src/city/placement_rotation.h and its users):
//   headings and quarter turns round-trip; a turned footprint's extents swap; a road-snapped lot keeps its road-side edge a
//   metre off the kerb whichever way it is turned; utility sources snapped and turned still reach their road from the side
//   that touches it and never sit on it; the shoreline rule of a pumping station follows the turned lot (back to the water
//   or refused); industry hubs snap turned the same way.
// Build and run: tests\placement_rotation_test.bat
#include <cmath>
#include <cstdio>

#include "city/industry.h"
#include "city/road_tool.h"
#include "city/utilities.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool NearV(V2 a, V2 b, float eps = 1e-3f) { return Near(a.x, b.x, eps) && Near(a.z, b.z, eps); }
static int Def(const char* name) {
    const auto& d = UtilDefs();
    for (size_t i = 0; i < d.size(); ++i)
        if (std::string(d[i].name) == name) return (int)i;
    return -1;
}
static float Flat(float, float) { return 1.0f; }

int main() {
    // ---- headings and quarter turns
    {
        CHECK(NearV(FrontFromDeg(0), {0, 1}) && NearV(FrontFromDeg(90), {1, 0}) && NearV(FrontFromDeg(180), {0, -1}));
        for (float d : {0.0f, 15.0f, 90.0f, 135.0f, 270.0f, 345.0f}) CHECK(Near(DegOfFront(FrontFromDeg(d)), d, 1e-2f));
        CHECK(QuarterTurns(0) == 0 && QuarterTurns(90) == 1 && QuarterTurns(180) == 2 && QuarterTurns(270) == 3 && QuarterTurns(360) == 0);
        CHECK(QuarterTurns(-90) == 3 && QuarterTurns(44) == 0 && QuarterTurns(46) == 1 && QuarterTurns(-180) == 2);
        CHECK(Near(StepRotation(0, 90), 90) && Near(StepRotation(270, 90), 0) && Near(StepRotation(0, -90), 270) && Near(StepRotation(350, 15), 5));
        V2 f{0.6f, 0.8f};
        V2 g = f;
        for (int i = 0; i < 4; ++i) g = TurnFront(g, 90);
        CHECK(NearV(f, g)); // four quarter turns come back
        CHECK(Near(Dot2(f, TurnFront(f, 90)), 0.0f)); // a quarter turn is perpendicular
    }

    // ---- extents of a turned footprint (16 m across, 32 m deep)
    {
        const float W = 16, D = 32;
        CHECK(Near(HalfExtentAlong({0, 1}, W, D, {0, 1}), 16) && Near(HalfExtentAlong({0, 1}, W, D, {1, 0}), 8));
        CHECK(Near(HalfExtentAlong({1, 0}, W, D, {0, 1}), 8) && Near(HalfExtentAlong({1, 0}, W, D, {1, 0}), 16)); // turned 90: they swap
        const float r2 = std::sqrt(0.5f);
        CHECK(Near(HalfExtentAlong({r2, r2}, W, D, {0, 1}), 0.5f * (W + D) * r2));
        V2 q[4];
        Corners({10, 20}, FrontFromDeg(90), W, D, q);
        float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
        for (V2 p : q) xmin = std::min(xmin, p.x), xmax = std::max(xmax, p.x), zmin = std::min(zmin, p.z), zmax = std::max(zmax, p.z);
        CHECK(Near(xmax - xmin, D) && Near(zmax - zmin, W)); // the turned lot's bounding box
        V2 mid[4], out[4];
        EdgeMids({0, 0}, {0, 1}, W, D, mid, out);
        CHECK(NearV(mid[0], {0, 16}) && NearV(out[0], {0, 1}) && NearV(mid[2], {0, -16}) && Near(std::fabs(mid[1].x), 8));
    }

    // ---- snapped beside a road along x (centre line z = 0, half width 6), the lot on the +z side
    {
        const float W = 16, D = 32, half = 6;
        for (int q = 0; q < 4; ++q) {
            const SnapResult r = RoadSnap({0, 0}, {0, 1}, half, W, D, q, 1.0f);
            V2 c[4];
            Corners(r.c, r.front, W, D, c);
            float zmin = 1e9f;
            for (V2 p : c) zmin = std::min(zmin, p.z);
            CHECK(Near(zmin, half + 1.0f, 1e-2f)); // its road-side edge a metre off the kerb, whichever way it is turned
            CHECK(Near(Len(r.front), 1.0f));
        }
        CHECK(NearV(RoadSnap({0, 0}, {0, 1}, half, W, D, 0).front, {0, -1})); // quarter 0 faces the road
        CHECK(NearV(RoadSnap({0, 0}, {0, 1}, half, W, D, 2).front, {0, 1}));  // 180: its back to the road
        CHECK(Near(std::fabs(RoadSnap({0, 0}, {0, 1}, half, W, D, 1).front.x), 1.0f)); // 90: its side
        CHECK(Near(RoadSnap({0, 0}, {0, 1}, half, W, D, 1).c.z, half + 1 + W * 0.5f)); // pulled in by the narrow side
    }

    // ---- utility sources on a real road: snapped and turned, still on their road network, never on the road
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        PlaceContext cx;
        cx.net = &t.net, cx.zones = &z, cx.ground = Flat;
        UtilWorld u;
        const int coal = Def("Small Coal Power Plant"); // 4 x 4 cells
        const int solar = Def("Solar Power Station");
        CHECK(coal >= 0 && solar >= 0);
        const Placement p0 = u.Plan(cx, coal, {-150, 40}, 0, true), p1 = u.Plan(cx, coal, {150, 40}, 90, true);
        CHECK(p0.valid && p1.valid && p0.snapped && p1.snapped);
        CHECK(Near(Dot2(p0.front, p1.front), 0.0f, 1e-3f)); // the second one turned a quarter
        CHECK(UtilRoadAccess(t.net, p1.c, p1.front, UtilDefs()[coal].W(), UtilDefs()[coal].D(), 6.0f) >= 0);
        // a rectangular lot (the generator: 1 x 2 cells) turned 90: pulled in by its narrow side, still clear of the road
        const int gen = Def("Emergency Generator");
        const Placement g0 = u.Plan(cx, gen, {0, 30}, 0, true), g1 = u.Plan(cx, gen, {0, 30}, 90, true);
        CHECK(g0.valid && g1.valid);
        CHECK(std::fabs(g1.c.z) < std::fabs(g0.c.z)); // depth 16 m facing the road, width 8 m when turned
        CHECK(u.Place(p0, coal) >= 0 && u.Place(p1, coal) >= 0);
        UtilReport rep;
        u.Solve(t.net, z, rep);
        CHECK(u.items[0].hasRoad && u.items[1].hasRoad); // the turned one reaches its road from the side
        // free placement keeps its exact heading
        const Placement f = u.Plan(cx, solar, {0, 200}, 30, false);
        CHECK(!f.snapped && Near(DegOfFront(f.front), 30, 1e-2f));
    }

    // ---- the shoreline rule follows the turned lot: a pumping station's back must be on the water
    {
        ZoneGrid z;
        PlaceContext cx;
        cx.zones = &z;
        cx.sea = 0.0f;
        cx.ground = [](float, float zz) { return zz < 0.0f ? -5.0f : 2.0f; }; // the sea south of z = 0
        UtilWorld u;
        const int pump = Def("Water Pumping Station"); // 4 x 3 cells: 24 m deep
        CHECK(pump >= 0 && UtilDefShore(pump));
        const float d = UtilDefs()[pump].D();
        // facing north (rotation 0: front +z), its back edge on the shore: fine
        const Placement ok = u.Plan(cx, pump, {0, d * 0.5f + 2.0f}, 0, false);
        CHECK(ok.valid);
        // turned 180 at the same spot, its back is inland: refused with the shoreline reason
        const Placement back = u.Plan(cx, pump, {0, d * 0.5f + 2.0f}, 180, false);
        CHECK(!back.valid && back.error.find("shoreline") != std::string::npos);
        // a shore running north-south (sea west of x = 0): turned 90 so its back faces west, it fits
        PlaceContext west = cx;
        west.ground = [](float x, float) { return x < 0.0f ? -5.0f : 2.0f; };
        const Placement w90 = u.Plan(west, pump, {d * 0.5f + 2.0f, 0}, 90, false);
        const Placement w0 = u.Plan(west, pump, {d * 0.5f + 2.0f, 0}, 0, false);
        CHECK(w90.valid);
        CHECK(!w0.valid);
    }

    // ---- industry hubs snap turned the same way
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        IndustryWorld w;
        IndSite a, b;
        CHECK(w.Snap(t.net, {0, 40}, a, 0) && w.Snap(t.net, {0, 40}, b, 1));
        CHECK(Near(Dot2(a.front, b.front), 0.0f, 1e-3f));
        CHECK(std::fabs(b.c.z) < std::fabs(a.c.z)); // 24 m across, 32 m deep: turned it sits closer
        CHECK(w.Free(t.net, a) && w.Free(t.net, b));
    }

    if (failures) {
        std::printf("placement_rotation_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("placement_rotation_test: all passed\n");
    return 0;
}
