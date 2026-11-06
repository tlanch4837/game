// Headless checks for the transport system (src/city/transport.h, transport_models.h and the rail rules of the road tool):
//   rails never join road nodes, level crossings make no junction, rail grade / curve limits;
//   stations snap onto a straight railway and line up with it, and are refused on bends, short pieces and inland;
//   shipyards / harbours MUST sit on a qualifying coast (snap to the shore facing the water; refused inland, on cliffs,
//   in shallow water, or when the map has no sea), however the cursor / rotation is set;
//   flat sites refuse water, slopes, roads and each other; placed buildings go Planned -> Building -> Done;
//   every model emits finite geometry.
// Build and run: tests\transport_test.bat
#include <cmath>
#include <cstdio>

#include "city/road_tool.h"
#include "city/transport_models.h"

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
static int LiveSegs(const Network& n) {
    int c = 0;
    for (const auto& s : n.segs) c += s.alive;
    return c;
}

int main() {
    // ------------------------------------------------------------------ the catalog
    {
        CHECK(Defs().size() >= 8);
        for (const BuildingDef& d : Defs()) {
            CHECK(d.w > 0 && d.d > 0 && d.cost > 0 && d.buildMinutes > 0);
            CHECK(d.site != Site::Coast || (d.apron > 0 && d.draft > 0));
        }
        const BuildingDef& depot = Defs()[Index("Railway Depot")];
        CHECK(depot.w == 18 && depot.d == 9 && depot.cost == 120000 && depot.workplaces == 24); // the CS2 depot of the reference GUI
        CHECK(Defs()[Index("Shipyard")].site == Site::Coast);
    }

    // ------------------------------------------------------------------ rails and roads
    {
        RoadTool t;
        const int road = ProfileIndex("Two-Lane Road"), rail = ProfileIndex("Single Track Railway");
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, road);
        const int before = LiveSegs(t.net);
        t.BuildRoad(Cubic::Line({0, -150}, {0, 150}), 0, 0, rail); // crosses the road at grade
        CHECK(LiveSegs(t.net) == before + 1);                       // a level crossing: no split, no junction
        for (size_t n = 0; n < t.net.nodes.size(); ++n)
            if (t.net.nodes[n].alive) CHECK(!(t.net.IsRailNode((int)n) && t.net.HasRoadSeg((int)n)));
        // the snap never picks a road node for a rail, nor a rail node for a road
        t.active = true;
        t.Select(RoadTool::AssetIndex("Single Track Railway"));
        CHECK(t.tab == Tab::Rail);
        t.Move(true, {-199, 0, 1});
        CHECK(t.cursor.node < 0 || t.net.IsRailNode(t.cursor.node));
        t.Move(true, {1, 0, 140});
        CHECK(t.cursor.node < 0 || t.net.IsRailNode(t.cursor.node));
        // rail limits: grade and curve radius
        t.Cancel();
        t.Select(RoadTool::AssetIndex("Single Track Railway"));
        t.SetMode(ToolMode::Straight);
        t.Move(true, {300, 0, 300});
        t.Click();
        t.elevation = 30; // 30% grade over the 100 m
        t.Move(true, {300, 0, 400});
        std::vector<PreviewCurve> curves;
        CHECK(!t.PendingCurves(curves));
        CHECK(t.error.find("rail") != std::string::npos);
        t.Cancel();
        t.elevation = 0;
        t.SetMode(ToolMode::SimpleCurve);
        t.Move(true, {500, 0, 300});
        t.Click();
        t.Move(true, {520, 0, 300}); // a sharp handle
        t.Click();
        t.Move(true, {520, 0, 320});
        CHECK(!t.PendingCurves(curves));
        std::printf("rail refused: %s\n", t.error.c_str());
        // swapping a road for a rail (or back) is refused
        t.Cancel();
        t.SetMode(ToolMode::Replace);
        t.Select(RoadTool::AssetIndex("Single Track Railway"));
        t.Move(true, {-100, 0, 0});
        CHECK(t.hoverSeg < 0); // the hover only finds rails for a rail asset
        // the mesh builder: sleepers and rails, all finite
        MeshOut out;
        BuildNetwork(t.net, nullptr, out);
        CHECK(out.byMat[(int)Mat::Paint].size() > 600);
        bool finite = true;
        for (const auto& v : out.byMat)
            for (const auto& x : v) finite = finite && std::isfinite(x.px) && std::isfinite(x.py) && std::isfinite(x.pz) && std::isfinite(x.nx);
        CHECK(finite);
    }

    // ------------------------------------------------------------------ stations
    {
        RoadTool t;
        auto flat5 = [](float, float) { return 5.0f; };
        t.ground = flat5, t.net.ground = flat5;
        const int rail = ProfileIndex("Double Track Railway"), road = ProfileIndex("Two-Lane Road");
        t.BuildRoad(Cubic::Line({-400, 0}, {400, 0}), 5, 5, rail);       // one long straight piece
        t.BuildRoad(Cubic::Line({0, 100}, {0, 300}), 5, 5, road);
        t.BuildRoad(Cubic::Quad({-400, -200}, {0, -400}, {400, -200}), 5, 5, rail); // a curved piece (not straight enough)
        t.BuildRoad(Cubic::Line({500, 0}, {600, 0}), 5, 5, rail);      // a 100 m piece (too short)
        TransportWorld w;
        PlaceContext cx;
        cx.net = &t.net;
        cx.ground = [](float, float) { return 5.0f; };
        const int st = Index("Train Station");
        Placement p = w.Plan(cx, st, {40, 12}, 0);
        CHECK(p.valid);
        CHECK(p.snapped && p.track >= 0 && t.net.IsRailSeg(p.track));
        CHECK(std::fabs(p.c.z) < 0.5f);                   // centred on the line
        CHECK(std::fabs(p.front.x) < 0.01f && std::fabs(std::fabs(p.front.z) - 1.0f) < 0.01f); // faces across the track
        CHECK(p.front.z > 0);                              // toward the cursor's side
        CHECK(p.platformInner > 5.0f);                     // outside a double track
        p = w.Plan(cx, st, {40, -12}, 0);
        CHECK(p.valid && p.front.z < 0);
        p = w.Plan(cx, st, {0, -300}, 0);
        std::printf("curved: %s\n", p.error.c_str());
        CHECK(!p.valid);
        p = w.Plan(cx, st, {550, 8}, 0);
        std::printf("short: %s\n", p.error.c_str());
        CHECK(!p.valid);
        p = w.Plan(cx, st, {0, 900}, 0); // nowhere near a railway
        CHECK(!p.valid && p.error.find("railway") != std::string::npos);
        // a road through the station's lot blocks it (the road at x = 0 reaches z = 100..300: the lot spans z -24..24 only,
        // so push it near)
        t.BuildRoad(Cubic::Line({90, 4}, {90, 60}), 5, 5, road);
        p = w.Plan(cx, st, {90, 12}, 0);
        CHECK(!p.valid);
    }

    // ------------------------------------------------------------------ coasts: a beach running north-south, sea to the east
    {
        // ground height: land west of x = 0 (rising inland), sea floor east of it falling away to 12 m under the sea
        auto beach = [](float x, float) { return x < 0 ? std::min(60.0f, 2.0f - x * 0.04f) : -0.25f - std::min(12.0f, x * 0.35f) - 0.2f; };
        TransportWorld w;
        PlaceContext cx;
        cx.ground = beach;
        cx.sea = -0.25f;
        const int yard = Index("Shipyard"), harbor = Index("Cargo Harbor"), ferry = Index("Passenger Harbor");
        // on the shore: snaps, faces the sea (+x), valid
        Placement p = w.Plan(cx, yard, {-30, 0}, 0);
        std::printf("shipyard at the shore: valid=%d err=%s front=(%.2f,%.2f) c=(%.0f,%.0f) depth=%.1f\n", p.valid, p.error.c_str(), p.front.x, p.front.z, p.c.x, p.c.z, p.waterDepth);
        CHECK(p.valid);
        CHECK(p.front.x > 0.95f);
        CHECK(p.haveShore && std::fabs(p.shore.x) < 3.0f);
        CHECK(p.y > cx.sea + 2.0f);
        // the cursor on the water snaps just the same
        p = w.Plan(cx, yard, {60, 10}, 90);
        CHECK(p.valid && p.front.x > 0.95f);
        // whatever the rotation, snapping faces the water
        for (float rot : {0.0f, 77.0f, 180.0f, 270.0f}) CHECK(w.Plan(cx, harbor, {-20, 40}, rot).front.x > 0.95f);
        // inland, with no sea in reach: refused (the shoreline is 400 m away)
        p = w.Plan(cx, yard, {-800, 0}, 0);
        std::printf("inland: %s\n", p.error.c_str());
        CHECK(!p.valid);
        // snapping off: the lot must still straddle the shore, rotation matters
        PlanOptions free;
        free.snapCoast = false;
        p = w.Plan(cx, yard, {-60, 0}, 90, free); // facing the sea but 60 m inland
        CHECK(!p.valid);
        p = w.Plan(cx, yard, {-4, 0}, 0, free);   // front toward +z (along the shore) at the shoreline
        CHECK(!p.valid);
        // placing, then the next one cannot overlap it
        p = w.Plan(cx, ferry, {-30, 0}, 0);
        CHECK(p.valid);
        CHECK(w.Place(p, ferry) == 0);
        CHECK(!w.Plan(cx, ferry, {-30, 4}, 0).valid);
        CHECK(w.Plan(cx, ferry, {-30, 200}, 0).valid); // further along the coast is fine
        // shallow water in front: not a qualifying coast
        PlaceContext shallow = cx;
        shallow.ground = [](float x, float) { return x < 0 ? 2.0f - x * 0.04f : -0.25f - 0.8f; };
        p = w.Plan(shallow, yard, {-30, 600}, 0);
        std::printf("shallow: %s\n", p.error.c_str());
        CHECK(!p.valid && p.error.find("shallow") != std::string::npos);
        // a cliff: too steep
        PlaceContext cliff = cx;
        cliff.ground = [](float x, float) { return x < 0 ? std::min(80.0f, 2.0f - x * 4.0f) : -0.25f - std::min(12.0f, x * 0.35f) - 0.2f; };
        p = w.Plan(cliff, yard, {-5, 900}, 0);
        std::printf("cliff: %s\n", p.error.c_str());
        CHECK(!p.valid);
        // no sea at all
        PlaceContext dry;
        dry.ground = [](float, float) { return 3.0f; };
        p = w.Plan(dry, yard, {0, 0}, 0);
        CHECK(!p.valid);
    }

    // ------------------------------------------------------------------ flat sites
    {
        TransportWorld w;
        PlaceContext cx;
        cx.ground = [](float, float) { return 4.0f; };
        cx.sea = -0.25f;
        const int depot = Index("Railway Depot"), air = Index("Regional Airport");
        Placement p = w.Plan(cx, depot, {0, 0}, 30);
        CHECK(p.valid && std::fabs(p.y - 4.0f) < 0.01f);
        CHECK(w.Place(p, depot) == 0);
        CHECK(!w.Plan(cx, depot, {40, 20}, 0).valid); // on top of the first
        CHECK(w.Plan(cx, depot, {500, 0}, 0).valid);
        // slopes
        PlaceContext slope = cx;
        slope.ground = [](float x, float) { return 0.12f * x + 10.0f; }; // 12% grade: ~33 m of relief over the depot
        CHECK(!w.Plan(slope, depot, {0, 900}, 0).valid);
        // water under it
        PlaceContext wet = cx;
        wet.ground = [](float x, float) { return x > 50 ? -3.0f : 4.0f; };
        CHECK(!w.Plan(wet, depot, {0, 800}, 0).valid);
        CHECK(w.Plan(wet, air, {-600, 800}, 0).valid);
        // a road through the lot
        RoadTool t;
        t.BuildRoad(Cubic::Line({-300, 1600}, {300, 1600}), 4, 4, ProfileIndex("Two-Lane Road"));
        cx.net = &t.net;
        CHECK(!w.Plan(cx, depot, {0, 1600}, 0).valid);
        CHECK(w.Plan(cx, depot, {0, 1800}, 0).valid);
        // zoned lots stay clear, painted-only cells do not matter
        ZoneGrid zg;
        t.BuildRoad(Cubic::Line({-300, 2400}, {300, 2400}), 4, 4, ProfileIndex("Two-Lane Road"));
        zg.Rebuild(t.net, cx.ground);
        CHECK(!zg.cells.empty());
        cx.zones = &zg;
        // pretend one cell holds a building
        for (ZoneCell& z : zg.cells)
            if (z.c.z > 2300 && z.c.z < 2500) { z.lot = 0; break; }
        bool refused = false;
        for (const ZoneCell& z : zg.cells)
            if (z.lot >= 0) refused = !w.Plan(cx, Index("Passenger Harbor") >= 0 ? Index("Cargo Train Terminal") : 0, z.c, 0).valid;
        CHECK(refused);
        // the lifecycle
        const int a = 0;
        CHECK(w.items[a].state == BState::Planned);
        w.Tick(kPlanMinutes + 1.0f);
        CHECK(w.items[a].state == BState::Building);
        w.Tick(Defs()[depot].buildMinutes * 0.5f);
        CHECK(w.items[a].state == BState::Building && w.items[a].progress > 0.4f && w.items[a].progress < 0.6f);
        w.Tick(Defs()[depot].buildMinutes);
        CHECK(w.items[a].state == BState::Done && w.items[a].progress >= 1.0f);
        CHECK(w.HitTest({0, 0}) == a);
        CHECK(w.HitTest({3000, 3000}) < 0);
        CHECK(w.Remove(a) && w.HitTest({0, 0}) < 0);
        CHECK(w.Plan(cx, depot, {0, 0}, 0).valid); // the lot is free again
        // zoning keeps out of placed buildings
        CHECK(w.Count() == 0);
    }

    // ------------------------------------------------------------------ models
    {
        for (size_t i = 0; i < Defs().size(); ++i) {
            Placed p;
            p.def = (int)i;
            p.c = {10, 20};
            p.front = {0.6f, 0.8f};
            p.y = 3;
            p.platformInner = 3.0f;
            for (BState st : {BState::Done, BState::Building, BState::Planned}) {
                p.state = st;
                p.progress = st == BState::Building ? 0.55f : 1.0f;
                std::vector<tbd::Vertex> tris, edges, holo, lines;
                tmodel::BuildPlaced(p, 1.5f, tris, &edges);
                tmodel::Holographic(tris, holo, 1.0f, true);
                tmodel::HolographicEdges(edges, lines, 1.0f, false);
                bool finite = !tris.empty();
                for (const auto& v : tris) finite = finite && std::isfinite(v.px) && std::isfinite(v.py) && std::isfinite(v.pz) && std::isfinite(v.nx);
                CHECK(finite);
                CHECK(tris.size() % 3 == 0 && holo.size() == tris.size() && lines.size() == edges.size());
                if (st == BState::Done) std::printf("%-22s %6zu triangles %6zu edges\n", Defs()[i].name, tris.size() / 3, edges.size() / 2);
            }
        }
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
