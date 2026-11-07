// Headless checks for the city sim's service coverage and utilities per cell (src/city/sim_coverage.h), and the utilities
// solve cache (src/city/utilities.h UtilSolveCache):
//   a service covers lots on its own road network within its radius, falling off with distance; a separate road network
//   is not covered however close; funding scales the radius; a cell's utility flag needs all its built lots served; the
//   solve cache re-solves only when roads, zones or utilities change.
// Build and run: tests\sim_coverage_test.bat
#include <cstdio>
#include <vector>

#include "city/road_tool.h"
#include "city/sim_coverage.h"
#include "city/utilities.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static float Flat(float, float) { return 0.0f; }

static int Road(Network& net, V2 a, V2 b) {
    const int prof = ProfileIndex("Two-Lane Road");
    const int na = net.AddNode({a.x, 0, a.z}, true), nb = net.AddNode({b.x, 0, b.z}, true);
    return net.AddSeg(na, nb, a + (b - a) * (1.0f / 3.0f), a + (b - a) * (2.0f / 3.0f), prof);
}

static Lot MakeLot(uint64_t id, V2 c, int seg) {
    Lot l;
    l.id = id;
    l.c = c;
    l.type = 0, l.w = 2, l.d = 2;
    l.path = "m";
    l.progress = 2.0f;
    l.frontSeg = seg;
    return l;
}

int main() {
    Network net;
    const int a = Road(net, {0, 0}, {3000, 0});     // road network A, east-west
    const int b = Road(net, {0, 300}, {3000, 300}); // network B, parallel, not connected
    ++net.version;
    std::vector<int> comp;
    RoadComponents(net, comp);
    CHECK(comp[(size_t)a] >= 0 && comp[(size_t)b] >= 0 && comp[(size_t)a] != comp[(size_t)b]);

    ZoneGrid z;
    z.lots = {MakeLot(1, {100, 20}, a), MakeLot(2, {500, 20}, a), MakeLot(3, {1500, 20}, a), MakeLot(4, {110, 280}, b)};
    CitySim s;
    s.Sync(z.lots, 1);
    auto cov = [&](const Lot& l, int kind) { return s.sim[(size_t)s.cells.Find(CellKeyAt(l.c.x, l.c.z))].coverage[kind]; };

    std::vector<ServiceSource> src = {{{100, 0}, kFire, 800, comp[(size_t)a]}};
    float funding[kServices] = {1, 1, 1, 1, 1, 1};
    ComputeCoverage(s, z, comp, src, funding);
    std::printf("fire coverage: near %d, 400 m %d, 1400 m %d, other network %d\n", cov(z.lots[0], kFire), cov(z.lots[1], kFire), cov(z.lots[2], kFire), cov(z.lots[3], kFire));
    CHECK(cov(z.lots[0], kFire) > 240);
    CHECK(cov(z.lots[1], kFire) > 0 && cov(z.lots[1], kFire) < cov(z.lots[0], kFire));
    CHECK(cov(z.lots[2], kFire) == 0);   // past the radius
    CHECK(cov(z.lots[3], kFire) == 0);   // 280 m away but on another road network
    CHECK(cov(z.lots[0], kPolice) == 0); // other services untouched
    funding[kFire] = 2.0f;               // the budget doubles the radius
    ComputeCoverage(s, z, comp, src, funding);
    CHECK(cov(z.lots[2], kFire) > 0);
    funding[kFire] = 0.0f;               // unfunded: nothing
    ComputeCoverage(s, z, comp, src, funding);
    CHECK(cov(z.lots[0], kFire) == 0);
    // join the networks: lot 4 is now covered
    {
        Network n2 = net;
        const int prof = ProfileIndex("Two-Lane Road");
        n2.AddSeg(n2.segs[(size_t)a].a, n2.segs[(size_t)b].a, V2{0, 100}, V2{0, 200}, prof);
        std::vector<int> c2;
        RoadComponents(n2, c2);
        std::vector<ServiceSource> s2 = {{{100, 0}, kFire, 800, c2[(size_t)a]}};
        float f1[kServices] = {1, 1, 1, 1, 1, 1};
        ComputeCoverage(s, z, c2, s2, f1);
        CHECK(cov(z.lots[3], kFire) > 0);
    }

    // utilities per cell: both lots of a cell must be served
    {
        ZoneGrid z2;
        z2.lots = {MakeLot(1, {10, 10}, a), MakeLot(2, {20, 10}, a), MakeLot(3, {300, 10}, a)};
        CitySim s2;
        s2.Sync(z2.lots, 1);
        ApplyUtilities(s2, z2, {kLotAll, (uint8_t)(kLotAll & ~kLotPower), kLotAll});
        const CellSim& c0 = s2.sim[(size_t)s2.cells.Find({0, 0})];
        const CellSim& c2 = s2.sim[(size_t)s2.cells.Find({2, 0})];
        CHECK(!c0.powerOk && c0.waterOk && c0.sewageOk);
        CHECK(c2.powerOk && c2.waterOk);
    }

    // the solve cache: no re-solve while nothing changes; a change of any of the three re-solves
    {
        UtilWorld w;
        ZoneGrid zg;
        zg.Rebuild(net, Flat);
        UtilReport rep;
        UtilSolveCache cache;
        CHECK(cache.Update(w, net, zg, rep));
        CHECK(!cache.Update(w, net, zg, rep));
        CHECK(!cache.Update(w, net, zg, rep));
        ++zg.version;
        CHECK(cache.Update(w, net, zg, rep));
        ++net.version;
        CHECK(cache.Update(w, net, zg, rep));
        ++w.version;
        CHECK(cache.Update(w, net, zg, rep));
        CHECK(!cache.Update(w, net, zg, rep));
        CHECK(cache.solves == 4);
    }

    if (failures) {
        std::printf("sim_coverage_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_coverage_test: all passed\n");
    return 0;
}
