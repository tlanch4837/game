// Headless checks for the sim gameplay pass, phase A (src/city/utilities.h, src/city/sim_flags.h):
//   flood fill: a network reaches only what its roads, lines and (with conductRange) touching buildings join;
//   capacity and brownout order: supply runs out and the lots laid out last go short first;
//   dirty flag: UtilSolveCache solves again only when the roads, the lots or the utilities changed;
//   shoreline rule for pumping stations and outlets; SimFlags per lot; efficiency of a cell short of power or water.
// Build and run: tests\sim_utilities_test.bat
#include <cmath>
#include <cstdio>

#include "city/road_tool.h"
#include "city/sim_flags.h"
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
    for (const Lot& l : z.lots) n += l.alive && l.Built();
    return n;
}
static void Grow(ZoneGrid& z, const Network& net, int zoneType, int w, int d, int lots, float xMin, float xMax) {
    z.models.clear();
    z.models.push_back({zoneType, w, d, 1, "test.glb"});
    z.maxConstructing = 100000;
    z.Rebuild(net, Flat);
    for (size_t i = 0; i < z.cells.size(); ++i)
        if (std::fabs(z.cells[i].c.x) >= xMin && std::fabs(z.cells[i].c.x) <= xMax) z.Paint((int)i, (uint8_t)zoneType);
    for (int i = 0; i < 400 && LiveLots(z) < lots; ++i) z.Tick(5.0f);
    for (int i = 0; i < 60; ++i) z.Tick(10.0f);
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
static int Powered(const UtilReport& r, const ZoneGrid& z, bool north, float zLine) {
    int n = 0;
    for (size_t i = 0; i < z.lots.size(); ++i)
        if (z.lots[i].alive && z.lots[i].Built() && ((z.lots[i].c.z > zLine) == north)) n += r.lots[i].elec;
    return n;
}
static int Lots(const ZoneGrid& z, bool north, float zLine) {
    int n = 0;
    for (const Lot& l : z.lots) n += l.alive && l.Built() && ((l.c.z > zLine) == north);
    return n;
}

int main() {
    // ------------------------------------------------------------------ flood fill and conduction
    {
        // two parallel roads that do not meet; the lots between them back onto each other
        RoadTool t;
        const int road = ProfileIndex("Two-Lane Road");
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, road);
        t.BuildRoad(Cubic::Line({-200, 52}, {200, 52}), 0, 0, road);
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 60, 0, 150);
        const float mid = 26.0f;
        const int southLots = Lots(z, false, mid), northLots = Lots(z, true, mid);
        CHECK(southLots > 0 && northLots > 0);
        UtilWorld u;
        UtilReport r;
        const PlaceContext cx = Ctx(t.net, z);
        CHECK(PlaceBeside(u, cx, "Small Coal Power Plant", {185, -30}) >= 0); // beside the south road only
        u.Solve(t.net, z, r);
        CHECK(Powered(r, z, false, mid) == southLots); // its own road network
        CHECK(Powered(r, z, true, mid) == 0);          // the other road is a separate network
        // CS1 conduction: the backs of the lots between the roads touch, so power jumps from block to block
        u.conductRange = 8.0f;
        u.Solve(t.net, z, r);
        CHECK(Powered(r, z, true, mid) == northLots);
        std::printf("flood fill: south %d/%d, north %d/%d with conduction\n", Powered(r, z, false, mid), southLots, Powered(r, z, true, mid), northLots);
    }

    // ------------------------------------------------------------------ capacity and brownout order, SimFlags
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-400, 0}, {400, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        Grow(z, t.net, 3, 6, 6, 14, 0, 300);
        UtilWorld u;
        UtilReport r;
        const PlaceContext cx = Ctx(t.net, z);
        CHECK(PlaceBeside(u, cx, "Wind Turbine", {350, 30}) >= 0);
        u.Solve(t.net, z, r);
        const int lots = LiveLots(z);
        int served = 0;
        bool seenShort = false, inOrder = true;
        for (size_t i = 0; i < z.lots.size(); ++i) {
            if (!z.lots[i].alive || !z.lots[i].Built()) continue;
            served += r.lots[i].elec;
            if (!r.lots[i].elec) seenShort = true;
            else if (seenShort) inOrder = false; // brownout order: first laid out, first served
        }
        CHECK(served > 0 && served < lots);
        CHECK(inOrder);
        CHECK(r.used[0] <= r.supply[0] + 1e-3f && r.demand[0] > r.supply[0]);
        CHECK(r.unserved[0] == lots - served);
        CHECK(PlaceBeside(u, cx, "Coal Power Plant", {-330, 40}) >= 0); // a big plant covers everyone
        u.Solve(t.net, z, r);
        CHECK(r.unserved[0] == 0);

        CitySim s;
        s.Sync(z.lots, 1);
        SimGrowth g;
        SimTuning tune;
        std::vector<uint8_t> util(z.lots.size(), kLotAll);
        std::vector<SimFlags> flags;
        UtilWorld none;
        none.Solve(t.net, z, r);
        for (size_t i = 0; i < z.lots.size(); ++i)
            util[i] = (uint8_t)((r.lots[i].elec ? kLotPower : 0) | (r.lots[i].water ? kLotWater : 0) | (r.lots[i].sewage ? kLotSewage : 0) | kLotRoad);
        ComputeSimFlags(z, s, g, tune, util, flags);
        const SimFlags all3 = kFlagNoPower | kFlagNoWater | kFlagNoSewage;
        int flagged = 0;
        for (size_t i = 0; i < z.lots.size(); ++i)
            if (z.lots[i].alive && z.lots[i].Built()) flagged += (flags[i] & all3) == all3;
        CHECK(flagged == lots);
        std::fill(util.begin(), util.end(), (uint8_t)kLotAll);
        ComputeSimFlags(z, s, g, tune, util, flags);
        for (SimFlags f : flags) CHECK(!(f & (all3 | kFlagAbandoned)));
    }

    // ------------------------------------------------------------------ dirty flag: solve only on change
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        Grow(z, t.net, 0, 2, 2, 20, 0, 120);
        UtilWorld u;
        UtilReport r;
        UtilSolveCache cache;
        CHECK(cache.Update(u, t.net, z, r));
        CHECK(!cache.Update(u, t.net, z, r));
        CHECK(!cache.Update(u, t.net, z, r));
        CHECK(cache.solves == 1);
        CHECK(PlaceBeside(u, Ctx(t.net, z), "Wind Turbine", {180, 30}) >= 0); // the utilities changed
        CHECK(cache.Update(u, t.net, z, r));
        CHECK(cache.solves == 2);
        t.BuildRoad(Cubic::Line({0, 0}, {0, 150}), 0, 0, ProfileIndex("Two-Lane Road")); // the roads changed
        CHECK(cache.Update(u, t.net, z, r));
        CHECK(!cache.Update(u, t.net, z, r));
        CHECK(cache.solves == 3);
    }

    // ------------------------------------------------------------------ shoreline rule (pumping stations, outlets)
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        ZoneGrid z;
        z.Rebuild(t.net, Flat);
        PlaceContext cx = Ctx(t.net, z);
        cx.sea = 0.0f;
        cx.ground = [](float, float zz) { return zz < -34.0f ? -10.0f : 1.0f; }; // the sea to the south
        UtilWorld u;
        CHECK(UtilDefShore(Def("Water Pumping Station")) && UtilDefShore(Def("Sewage Outlet")) && !UtilDefShore(Def("Groundwater Pump")));
        const Placement wet = u.Plan(cx, Def("Water Pumping Station"), {0, -30}, 0, true);
        if (!wet.valid) std::printf("  (shore: %s)\n", wet.error.c_str());
        CHECK(wet.valid);
        CHECK(!u.Plan(cx, Def("Water Pumping Station"), {0, 30}, 0, true).valid); // the inland side
        const Placement outlet = u.Plan(cx, Def("Sewage Outlet"), {60, -20}, 0, true);
        if (!outlet.valid) std::printf("  (outlet: %s)\n", outlet.error.c_str());
        CHECK(outlet.valid);
        CHECK(u.Plan(cx, Def("Groundwater Pump"), {0, 30}, 0, true).valid);
        // the Wastewater Treatment Plant is not a shore building: it places inland, away from any water
        CHECK(!UtilDefShore(Def("Wastewater Treatment Plant")) && UtilDefShore(Def("Large Water Pumping Station")));
        const Placement treat = u.Plan(cx, Def("Wastewater Treatment Plant"), {0, 40}, 0, true);
        if (!treat.valid) std::printf("  (treatment inland: %s)\n", treat.error.c_str());
        CHECK(treat.valid);
        CHECK(std::string(UtilDefs()[Def("Wastewater Treatment Plant")].rule).find("horeline:") == std::string::npos);
        cx.sea = -1e9f; // a map without a sea: the rule cannot apply
        CHECK(u.Plan(cx, Def("Water Pumping Station"), {0, 30}, 0, true).valid);
    }

    // ------------------------------------------------------------------ efficiency
    {
        CellSim c;
        CHECK(CellEfficiencyPermille(c) == 1000);
        c.powerOk = 0;
        CHECK(CellEfficiencyPermille(c) == 500);
        c.waterOk = 0;
        CHECK(CellEfficiencyPermille(c) == 350);
    }

    // ------------------------------------------------------------------ flags: 32 bits, owners, BuildingId table
    {
        static_assert(sizeof(SimFlags) == 4, "sim flags are 32 bits");
        static_assert(kFlagNoPower == 1u && kFlagAbandoned == 32u, "the current bits did not move");
        static_assert(kFlagCount <= 32, "32 bits");
        SimFlags all = 0;
        for (int b = 0; b < kFlagCount; ++b) {
            CHECK(SimFlagText(b)[0] != 0);
            CHECK(SimFlagOwner(b) >= 0 && SimFlagOwner(b) <= 7);
            all |= 1u << b;
        }
        CHECK(SimFlagText(kFlagCount)[0] == 0 && SimFlagOwner(31) == -1);
        SimFlags owned = 0; // every named bit has exactly one owning phase
        for (int p = 0; p <= 7; ++p) {
            CHECK((owned & SimFlagPhaseMask(p)) == 0);
            owned |= SimFlagPhaseMask(p);
        }
        CHECK(owned == all);
        CHECK((SimFlagPhaseMask(4) & (kFlagSick | kFlagCrime | kFlagFire | kFlagGarbage | kFlagNoSchool)) ==
              (kFlagSick | kFlagCrime | kFlagFire | kFlagGarbage | kFlagNoSchool));
        CHECK(SimFlagPhaseMask(2) == (kFlagHighRent | kFlagAbandoned));

        BuildingRegistry reg;
        std::vector<Lot> lots(3);
        for (int i = 0; i < 3; ++i) lots[(size_t)i].id = 100 + i, lots[(size_t)i].alive = true;
        reg.BeginSync();
        for (const Lot& l : lots) reg.Upsert(BOrigin::Lot, l.id, BuildingRow());
        reg.EndSync();
        SimFlagTable t;
        t.FromLots(lots, reg, {kFlagNoPower, 0u, kFlagNoWater | kFlagSick}, kFlagCurrentMask);
        const BuildingId b0 = reg.Find(BOrigin::Lot, 100), b2 = reg.Find(BOrigin::Lot, 102);
        CHECK(t.Get(b0) == kFlagNoPower);
        CHECK(t.Get(b2) == kFlagNoWater); // the sick bit is not the current pass's: masked out
        CHECK(t.Write(b2, SimFlagPhaseMask(4), kFlagSick | kFlagNoPower)); // a phase writes only its own bits
        CHECK(t.Get(b2) == (kFlagNoWater | kFlagSick));
        CHECK(!t.Write(b2, SimFlagPhaseMask(4), kFlagSick)); // no change
        CHECK(t.Any() == (kFlagNoPower | kFlagNoWater | kFlagSick));
        t.ClearAll(SimFlagPhaseMask(4));
        CHECK(t.Get(b2) == kFlagNoWater);
        // a compaction (lot 101 gone, 102 moves to position 1) keeps 102's flags on its id; 100's row is freed
        lots.erase(lots.begin());
        lots.erase(lots.begin());
        reg.BeginSync();
        for (const Lot& l : lots) reg.Upsert(BOrigin::Lot, l.id, BuildingRow());
        reg.EndSync();
        t.Prune(reg);
        CHECK(t.Get(reg.Find(BOrigin::Lot, 102)) == kFlagNoWater);
        CHECK(t.Get(b0) == 0); // stale id reads clean
        CHECK(!t.Write(BuildingId(), ~0u, ~0u));
    }

    if (failures) std::printf("%d FAILED\n", failures);
    else std::printf("sim_utilities_test: all passed\n");
    return failures ? 1 : 0;
}
