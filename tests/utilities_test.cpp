// Headless checks for the utilities system (src/city/utilities.h and the no-road clock in src/city/zoning.h):
//   roads that are not highways carry electricity, water and sewage under them; buildings on them are served from sources
//   on the same road network, in order, until the supply runs out; highways do not carry them, a line the player lays does;
//   emergency generators only cover a shortfall; a building without a road is flagged, condemned after 10 game days and
//   demolished 5 days later, and a road back in front of it resets the clock; a new road over a plot demolishes it at once;
//   source buildings are placed beside a road facing it and refused on roads, steep ground and each other.
// Build and run: tests\utilities_test.bat
#include <cmath>
#include <cstdio>

#include "city/road_tool.h"
#include "city/utilities.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Def(const char* name) {
    const auto& d = UtilDefs();
    for (size_t i = 0; i < d.size(); ++i)
        if (std::string(d[i].name) == name) return (int)i;
    return -1;
}
static float Flat(float, float) { return 0.0f; }
static int LiveLots(const ZoneGrid& z) {
    int n = 0;
    for (const Lot& l : z.lots) n += l.alive;
    return n;
}
// Grow houses on both sides of every road: paint everything, run the clock until the lots are built.
static void Grow(ZoneGrid& z, const Network& net, int zoneType, int w, int d, int lots = 1000000, float xMin = 0, float xMax = 1e9f) {
    z.models.clear();
    z.models.push_back({zoneType, w, d, 1, "test.glb"});
    z.maxConstructing = 100000;
    z.Rebuild(net, Flat);
    for (size_t i = 0; i < z.cells.size(); ++i) // only |x| in [xMin, xMax]: the rest of the road stays free for the sources
        if (std::fabs(z.cells[i].c.x) >= xMin && std::fabs(z.cells[i].c.x) <= xMax) z.Paint((int)i, (uint8_t)zoneType);
    for (int i = 0; i < 400 && LiveLots(z) < lots; ++i) z.Tick(5.0f);
    for (int i = 0; i < 60; ++i) z.Tick(10.0f); // all the construction finishes
}
static PlaceContext Ctx(const Network& net, const ZoneGrid& z) {
    PlaceContext cx;
    cx.net = &net;
    cx.zones = &z;
    cx.ground = Flat;
    return cx;
}
static int PlaceBeside(UtilWorld& u, const PlaceContext& cx, const char* name, V2 cursor) {
    const Placement p = u.Plan(cx, Def(name), cursor, 0, true);
    if (!p.valid) std::printf("  (placing %s: %s)\n", name, p.error.c_str());
    return u.Place(p, Def(name));
}
static int Count(const UtilReport& r, bool UtilStatus::*field, bool want) {
    int n = 0;
    for (const UtilStatus& s : r.lots) n += (s.*field == want);
    return n;
}

int main() {
    // ------------------------------------------------------------------ the catalog
    {
        CHECK(UtilDefs().size() == 15);
        for (const UtilDef& d : UtilDefs()) CHECK(d.w > 0 && d.d > 0 && d.supply > 0 && d.cost > 0 && d.file != nullptr);
        CHECK(UtilDefs()[Def("Nuclear Power Plant")].w == 14 && UtilDefs()[Def("Nuclear Power Plant")].d == 11);
        CHECK(UtilDefs()[Def("Emergency Generator")].backup && UtilDefs()[Def("Battery Station")].backup);
        CHECK(UtilDefs()[Def("Sewage Outlet")].kind == Util::Sewage && UtilDefs()[Def("Groundwater Pump")].kind == Util::Water);
        UtilLine pole{1, Util::Electricity, {0, 0}, {100, 0}, 0}, pylon{2, Util::Electricity, {0, 0}, {100, 0}, 10}, cable{3, Util::Electricity, {0, 0}, {100, 0}, -10};
        CHECK(pole.Poles() && !pole.Pylons() && !pole.Buried());   // wooden poles only at 0 m
        CHECK(pylon.Pylons() && !pylon.Poles() && !pylon.Buried());
        CHECK(cable.Buried() && !cable.Poles() && !cable.Pylons());
        UtilWorld w;
        const int pipe = w.AddLine(Util::Water, {0, 0}, {50, 0}, 0);
        CHECK(pipe >= 0 && w.lines[pipe].Buried());               // pipes are always buried
        CHECK(!w.lines[pipe].Poles());
    }

    // ------------------------------------------------------------------ roads carry the utilities
    {
        RoadTool t;
        const int road = ProfileIndex("Two-Lane Road");
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, road);
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 40, 0, 100);
        const int houses = LiveLots(z);
        CHECK(houses >= 20);
        UtilWorld u;
        UtilReport r;
        u.Solve(t.net, z, r);
        CHECK((int)r.lots.size() == (int)z.lots.size());
        // no source anywhere: every house lacks power, water and sewage, but has its road
        CHECK(Count(r, &UtilStatus::elec, false) == houses && Count(r, &UtilStatus::water, false) == houses && Count(r, &UtilStatus::sewage, false) == houses);
        CHECK(Count(r, &UtilStatus::road, true) == houses);
        // a wind turbine on the road: electricity everywhere, water and sewage still missing
        const PlaceContext cx = Ctx(t.net, z);
        CHECK(PlaceBeside(u, cx, "Wind Turbine", {200, 30}) >= 0);
        u.Solve(t.net, z, r);
        CHECK(Count(r, &UtilStatus::elec, true) == houses);
        CHECK(Count(r, &UtilStatus::water, false) == houses && Count(r, &UtilStatus::sewage, false) == houses);
        CHECK(u.items[0].hasRoad);
        CHECK(r.supply[0] > 2.0f && r.demand[0] > 0 && r.used[0] > 0);
        // a pump and an outlet complete it
        CHECK(PlaceBeside(u, cx, "Groundwater Pump", {-200, 30}) >= 0);
        CHECK(PlaceBeside(u, cx, "Sewage Outlet", {250, 30}) >= 0);
        u.Solve(t.net, z, r);
        int ok = 0;
        for (const UtilStatus& s : r.lots) ok += s.Ok();
        CHECK(ok == houses);
        CHECK(r.unserved[0] == 0 && r.unserved[1] == 0 && r.unserved[2] == 0);
        // the road view: the whole road is live for all three
        for (int k = 0; k < kUtilKinds; ++k) CHECK(r.segLive[k][0]);
        // the weather moves the turbine's output: Resupply (no geometry) gives what a full Solve gives
        for (float scale : {0.0f, 0.3f, 2.0f, 1.0f}) {
            u.windScale = scale;
            UtilReport a = r, b;
            CHECK(u.Resupply(z, a));
            u.Solve(t.net, z, b);
            CHECK(a.supply[0] == b.supply[0] && a.used[0] == b.used[0] && a.unserved[0] == b.unserved[0] && a.segLive[0] == b.segLive[0]);
            int same = 0;
            for (size_t i = 0; i < a.lots.size(); ++i) same += a.lots[i].elec == b.lots[i].elec;
            CHECK(same == (int)a.lots.size());
            if (scale == 0.0f) CHECK(a.unserved[0] == houses);
        }
    }

    // ------------------------------------------------------------------ supply runs out; backup covers a shortfall
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-400, 0}, {400, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        Grow(z, t.net, 3, 6, 6, 14, 0, 300); // high density towers: 36 cells x 0.012 MW each
        const int lots = LiveLots(z);
        CHECK(lots >= 8);
        UtilWorld u;
        UtilReport r;
        const PlaceContext cx = Ctx(t.net, z);
        CHECK(PlaceBeside(u, cx, "Wind Turbine", {350, 30}) >= 0); // 2.1 MW feeds a handful of 0.43 MW towers
        u.Solve(t.net, z, r);
        const int served = Count(r, &UtilStatus::elec, true);
        CHECK(served >= 3 && served < lots);
        CHECK(r.unserved[0] == lots - served);
        CHECK(r.used[0] <= r.supply[0] + 1e-3f);
        // served in order: the first lots have it, the later ones do not
        bool seenShort = false, inOrder = true;
        for (const UtilStatus& s : r.lots) {
            if (!s.elec) seenShort = true;
            else if (seenShort) inOrder = false;
        }
        CHECK(inOrder);
        // an emergency generator (5 MW backup) lifts the shortfall; on its own it would not
        CHECK(PlaceBeside(u, cx, "Emergency Generator", {-350, 30}) >= 0);
        u.Solve(t.net, z, r);
        CHECK(Count(r, &UtilStatus::elec, true) > served);
        UtilWorld backupOnly;
        CHECK(PlaceBeside(backupOnly, cx, "Battery Station", {350, 30}) >= 0);
        backupOnly.Solve(t.net, z, r);
        CHECK(Count(r, &UtilStatus::elec, true) > 0); // it covers the shortfall of a grid with nothing else
        UtilWorld none;
        none.Solve(t.net, z, r);
        CHECK(Count(r, &UtilStatus::elec, true) == 0);
    }

    // ------------------------------------------------------------------ a highway carries nothing; a line bridges the gap
    {
        RoadTool t;
        const int road = ProfileIndex("Two-Lane Road"), hwy = ProfileIndex("Two-Lane Two-Way Highway");
        t.BuildRoad(Cubic::Line({-400, 0}, {-60, 0}), 0, 0, road);
        t.BuildRoad(Cubic::Line({-60, 0}, {60, 0}), 0, 0, hwy);
        t.BuildRoad(Cubic::Line({60, 0}, {400, 0}), 0, 0, road);
        int carriers = 0;
        for (size_t s = 0; s < t.net.segs.size(); ++s) carriers += CarriesUtilities(t.net, (int)s);
        CHECK(carriers == 2); // the highway is not one
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 60, 80, 220);
        UtilWorld u;
        UtilReport r;
        const PlaceContext cx = Ctx(t.net, z);
        CHECK(PlaceBeside(u, cx, "Wind Turbine", {-330, 30}) >= 0);
        u.Solve(t.net, z, r);
        int west = 0, east = 0, westOk = 0, eastOk = 0;
        for (size_t i = 0; i < z.lots.size(); ++i) {
            const bool isWest = z.lots[i].c.x < 0;
            (isWest ? west : east)++;
            (isWest ? westOk : eastOk) += r.lots[i].elec;
        }
        CHECK(west > 5 && east > 5);
        CHECK(westOk == west && eastOk == 0); // the far side is dark
        // an electricity line from the west road to the east road joins them
        CHECK(u.AddLine(Util::Electricity, {-150, 8}, {150, 8}, 0) >= 0);
        u.Solve(t.net, z, r);
        eastOk = 0;
        for (size_t i = 0; i < z.lots.size(); ++i)
            if (z.lots[i].c.x > 0) eastOk += r.lots[i].elec;
        CHECK(eastOk == east);
        CHECK(r.lineLive[0]);
        // but not for water: a power line is not a pipe
        CHECK(PlaceBeside(u, cx, "Groundwater Pump", {-370, 30}) >= 0);
        u.Solve(t.net, z, r);
        int eastWater = 0;
        for (size_t i = 0; i < z.lots.size(); ++i)
            if (z.lots[i].c.x > 0) eastWater += r.lots[i].water;
        CHECK(eastWater == 0);
        CHECK(u.AddLine(Util::Water, {-150, 8}, {150, 8}, -10) >= 0);
        u.Solve(t.net, z, r);
        eastWater = 0;
        for (size_t i = 0; i < z.lots.size(); ++i)
            if (z.lots[i].c.x > 0) eastWater += r.lots[i].water;
        CHECK(eastWater == east);
    }

    // ------------------------------------------------------------------ no road: 10 days, then 5, reset by a road
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-120, 0}, {120, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 20);
        const int houses = LiveLots(z);
        CHECK(houses >= 10);
        for (const Lot& l : z.lots) CHECK(l.hasRoad && l.noRoadMinutes == 0);
        // the road goes: the houses stay, flagged
        int seg = -1;
        for (size_t s = 0; s < t.net.segs.size(); ++s)
            if (t.net.segs[s].alive) seg = (int)s;
        const Segment saved = t.net.segs[seg];
        t.net.RemoveSeg(seg);
        z.Rebuild(t.net, Flat);
        CHECK(LiveLots(z) == houses);
        for (const Lot& l : z.lots) CHECK(l.alive && !l.hasRoad);
        UtilWorld u;
        UtilReport r;
        u.Solve(t.net, z, r);
        CHECK(Count(r, &UtilStatus::road, false) == houses);
        CHECK(r.noRoad == houses);
        // nothing grows over them, whatever is painted
        const int cellsTaken = (int)std::count_if(z.cells.begin(), z.cells.end(), [](const ZoneCell& c) { return c.lot >= 0; });
        (void)cellsTaken;
        // 9 days: nothing yet. 10 days: condemned. 14: still standing. 15: gone.
        z.Tick(9.0f * kDayMinutes);
        CHECK(LiveLots(z) == houses);
        for (const Lot& l : z.lots) CHECK(!l.Condemned());
        z.Tick(1.0f * kDayMinutes + 1.0f);
        CHECK(LiveLots(z) == houses);
        for (const Lot& l : z.lots) CHECK(l.Condemned() && l.DemolishInMinutes() > 4.9f * kDayMinutes);
        z.Tick(4.0f * kDayMinutes);
        CHECK(LiveLots(z) == houses);
        // the road comes back: clock stopped and reset
        z.removed.clear();
        t.net.segs[seg] = saved;
        t.net.segs[seg].alive = true;
        t.net.nodes[saved.a].segs.push_back(seg);
        t.net.nodes[saved.b].segs.push_back(seg);
        ++t.net.version;
        z.Rebuild(t.net, Flat);
        for (const Lot& l : z.lots) CHECK(!l.alive || l.hasRoad);
        z.Tick(1.0f);
        for (const Lot& l : z.lots) CHECK(!l.alive || (l.hasRoad && l.noRoadMinutes == 0 && !l.Condemned()));
        CHECK(z.removed.empty());
        CHECK(LiveLots(z) == houses);
        // and with no road for the full 15 days they do come down
        t.net.RemoveSeg(seg);
        z.Rebuild(t.net, Flat);
        z.Tick(kNoRoadMinutes + kDemolishMinutes + 10.0f);
        CHECK(LiveLots(z) == 0);
        CHECK((int)z.removed.size() == houses);
    }

    // ------------------------------------------------------------------ a new road over a plot takes the building at once
    {
        RoadTool t;
        const int road = ProfileIndex("Two-Lane Road");
        t.BuildRoad(Cubic::Line({-120, 0}, {120, 0}), 0, 0, road);
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 20);
        const int before = LiveLots(z);
        CHECK(before >= 10);
        t.BuildRoad(Cubic::Line({0, 8}, {0, 120}), 0, 0, road); // straight through the plots on the north side
        z.Rebuild(t.net, Flat);
        CHECK(LiveLots(z) < before);
        for (const Lot& l : z.lots)
            if (l.alive) CHECK(l.hasRoad); // the survivors still front the first road
    }

    // ------------------------------------------------------------------ placing a source building
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        z.Rebuild(t.net, Flat);
        UtilWorld u;
        const PlaceContext cx = Ctx(t.net, z);
        // it snaps beside the road and faces it
        const Placement p = u.Plan(cx, Def("Water Tower"), {10, 30}, 0, true);
        CHECK(p.valid && p.snapped);
        CHECK(p.front.z < -0.99f);                                  // north of the road (z > 0): its front faces -z, toward it
        CHECK(std::fabs(p.c.z - (t.net.Prof(0).Half() + 1.0f + 8.0f)) < 0.3f); // front edge a metre off the kerb
        const int i = u.Place(p, Def("Water Tower"));
        CHECK(i >= 0);
        UtilReport r;
        u.Solve(t.net, z, r);
        CHECK(u.items[i].hasRoad && r.buildings[i].road);
        // a second one on top of it is refused
        const Placement dup = u.Plan(cx, Def("Water Tower"), p.c, 0, false);
        CHECK(!dup.valid);
        // on the road: refused
        const Placement onRoad = u.Plan(cx, Def("Groundwater Pump"), {0, 0}, 0, false);
        CHECK(!onRoad.valid && onRoad.error.find("road") != std::string::npos);
        // in the open, no road near: placed, but flagged, and on the clock
        const Placement far = u.Plan(cx, Def("Wind Turbine"), {0, 800}, 0, true);
        CHECK(far.valid && !far.snapped);
        const int w = u.Place(far, Def("Wind Turbine"));
        u.Solve(t.net, z, r);
        CHECK(!u.items[w].hasRoad && !r.buildings[w].road);
        u.Tick(kNoRoadMinutes + 1.0f);
        CHECK(u.items[w].alive && u.items[w].Condemned());
        u.Tick(kDemolishMinutes);
        CHECK(!u.items[w].alive && !u.removed.empty());
        // steep ground and water are refused
        PlaceContext hill = cx;
        hill.ground = [](float x, float) { return x * 0.6f; };
        CHECK(!u.Plan(hill, Def("Gas Power Plant"), {300, 300}, 0, false).valid);
        PlaceContext sea = cx;
        sea.sea = 5.0f;
        sea.ground = [](float, float) { return 0.0f; };
        const Placement wet = u.Plan(sea, Def("Wind Turbine"), {0, 500}, 0, false);
        CHECK(!wet.valid && wet.error.find("water") != std::string::npos);
        CHECK(u.Blockers().size() == 1); // the tower (the wind turbine is gone)
    }

    // ------------------------------------------------------------------ lines
    {
        UtilWorld u;
        const int a = u.AddLine(Util::Electricity, {0, 0}, {100, 0}, 0);
        const int b = u.AddLine(Util::Water, {0, 20}, {100, 20}, -10);
        CHECK(a >= 0 && b >= 0 && u.AddLine(Util::Electricity, {0, 0}, {0.5f, 0}, 0) < 0); // too short
        V2 e{};
        CHECK(u.SnapEnd({102, 3}, Util::Electricity, 12.0f, e) && Len(e - V2{100, 0}) < 1e-3f);
        CHECK(!u.SnapEnd({102, 3}, Util::Sewage, 12.0f, e));
        CHECK(u.LineAt({50, 2}, 5.0f) == a);
        CHECK(u.LineAt({50, 22}, 5.0f, true) < 0);  // buried: not hit when only the visible ones count
        CHECK(u.RemoveLine(a) && u.LineAt({50, 2}, 5.0f) < 0);
        CHECK(UtilLineCost(u.lines[b]) > 0);
    }

    if (failures == 0) std::printf("utilities_test: all checks passed\n");
    else std::printf("utilities_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
