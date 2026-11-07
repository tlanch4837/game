// Headless checks for the city sim's demand, growth, level-up and abandonment (src/city/sim_growth.h) on a synthetic
// road grid with a fixed seed:
//   residential demand rises when jobs exceed residents, job demand when residents exceed jobs; a painted district grows
//   from nothing under the demand gate; lots level up under good conditions and are abandoned under sustained bad ones;
//   two runs give identical results; the tuning JSON reader applies known names and reports unknown ones.
// Build and run: tests\sim_growth_test.bat
#include <cstdio>
#include <string>
#include <vector>

#include "city/sim.h"
#include "city/sim_growth.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static float Flat(float, float) { return 0.0f; }

static Lot MakeLot(uint64_t id, float x, float z, int type, int w, int d) {
    Lot l;
    l.id = id;
    l.c = {x, z};
    l.type = type, l.w = w, l.d = d;
    l.path = "m.glb";
    l.progress = 2.0f;
    return l;
}

// A 4 x 4 grid of 120 m blocks, two-lane roads.
static void Grid(Network& net) {
    const int prof = ProfileIndex("Two-Lane Road");
    constexpr int B = 4;
    int id[B + 1][B + 1];
    for (int j = 0; j <= B; ++j)
        for (int i = 0; i <= B; ++i) id[j][i] = net.AddNode({i * 120.0f, 0.0f, j * 120.0f}, true);
    auto seg = [&](int a, int b) {
        const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
        net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
    };
    for (int j = 0; j <= B; ++j)
        for (int i = 0; i < B; ++i) seg(id[j][i], id[j][i + 1]), seg(id[i][j], id[i + 1][j]);
    ++net.version;
}

struct World {
    Network net;
    ZoneGrid zones;
    CitySim cells;
    SimGrowth growth;
    SimTuning tune;
    int64_t minute = 0;
    bool forceBad = false; // every cell without power (abandonment test)

    void Setup(bool levels) {
        Grid(net);
        for (int type = 0; type < 7; ++type)
            for (int w = 1; w <= 4; ++w)
                for (int d = 1; d <= 4; ++d)
                    for (int level = 1; level <= (levels ? 2 : 1); ++level)
                        zones.models.push_back({type, w, d, level, "T" + std::to_string(type) + "_" + std::to_string(w) + "x" + std::to_string(d) + "_L" + std::to_string(level)});
        zones.maxConstructing = 16;
        zones.Rebuild(net, Flat);
        // a district: west half residential (low + medium), then commercial, office, industrial strips
        for (size_t i = 0; i < zones.cells.size(); ++i) {
            const float x = zones.cells[i].c.x;
            const uint8_t zone = x < 200 ? (i % 3 ? 0 : 2) : x < 300 ? 4 : x < 380 ? 5 : 6;
            zones.Paint((int)i, zone);
        }
    }
    // One game minute: the order SimSync's systems run in.
    void Step() {
        zones.Tick(1.0f);
        cells.Sync(zones.lots, zones.version);
        if (forceBad)
            for (CellSim& c : cells.sim) c.powerOk = 0;
        if (minute % 10 == 0) {
            growth.UpdateDemand(cells, tune, 10.0, &zones);
            growth.ApplyGate(zones, tune);
        }
        if (minute % 60 == 0) {
            growth.UpdateLand(cells, tune);
            growth.TickLots(zones, cells, tune, 60.0);
        }
        ++minute;
    }
    void Days(double d) {
        for (int64_t m = 0; m < (int64_t)(d * 1440); ++m) Step();
    }
    int Live() const {
        int n = 0;
        for (const Lot& l : zones.lots) n += l.alive;
        return n;
    }
    uint64_t Hash() const {
        uint64_t h = cells.Hash();
        for (const Lot& l : zones.lots) {
            h = (h ^ l.id) * 1099511628211ull;
            h = (h ^ (uint64_t)l.level) * 1099511628211ull;
            h = (h ^ (uint64_t)l.alive) * 1099511628211ull;
            h = (h ^ std::hash<std::string>()(l.path)) * 1099511628211ull;
        }
        return h;
    }
};

int main() {
    // Demand direction from counters alone.
    {
        SimTuning t;
        t.demandStartPull = 0; // no start-up pull: the counters only
        CitySim s;
        SimGrowth g;
        std::vector<Lot> lots = {MakeLot(1, 10, 10, 6, 6, 6), MakeLot(2, 20, 10, 0, 2, 2)}; // a big factory, one small house
        s.Sync(lots, 1);
        for (int i = 0; i < 300; ++i) g.UpdateDemand(s, t, 10.0);
        std::printf("jobs > residents: R %.2f C %.2f O %.2f I %.2f\n", g.demand[0], g.demand[1], g.demand[2], g.demand[3]);
        CHECK(g.demand[(int)ZoneCat::Residential] > 0.3);
        CHECK(g.demand[(int)ZoneCat::Industrial] < 0.0);
        CitySim s2;
        SimGrowth g2;
        std::vector<Lot> homes;
        for (uint64_t i = 0; i < 40; ++i) homes.push_back(MakeLot(i + 1, 10.0f + i, 10, 3, 4, 4)); // tower blocks, no jobs
        s2.Sync(homes, 1);
        for (int i = 0; i < 300; ++i) g2.UpdateDemand(s2, t, 10.0);
        std::printf("residents > jobs: R %.2f C %.2f O %.2f I %.2f\n", g2.demand[0], g2.demand[1], g2.demand[2], g2.demand[3]);
        CHECK(g2.demand[(int)ZoneCat::Residential] < 0.0);
        CHECK(g2.demand[(int)ZoneCat::Industrial] > 0.2);
        CHECK(g2.demand[(int)ZoneCat::Office] > 0.05);
        CHECK(g2.demand[(int)ZoneCat::Commercial] > 0.3);
        // smoothing: one step moves only a little
        SimGrowth g3;
        g3.UpdateDemand(s, t, 10.0);
        CHECK(g3.demand[0] > 0 && g3.demand[0] < 0.05);
    }

    // A painted district grows from nothing; homes first, then jobs; levels up under good conditions.
    uint64_t hashA = 0;
    {
        World w;
        w.Setup(true);
        w.tune.levelUpLandValue = 0; // conditions always good enough for land value
        w.tune.levelUpDays = 2;
        const size_t cellCount = w.zones.cells.size();
        w.Days(1);
        const int day1 = w.Live();
        uint64_t homes1 = 0, jobs1 = 0;
        for (const CellSim& c : w.cells.sim) homes1 += c.homeCap, jobs1 += (uint64_t)c.jobCap[0] + c.jobCap[1] + c.jobCap[2];
        w.Days(29);
        uint64_t jobs30 = 0;
        for (const CellSim& c : w.cells.sim) jobs30 += (uint64_t)c.jobCap[0] + c.jobCap[1] + c.jobCap[2];
        std::printf("grow: %zu cells, day 1 %d lots (homes %llu jobs %llu), day 30 %d lots, pop %llu, working %llu, jobs %llu, level ups %llu\n", cellCount, day1,
                    (unsigned long long)homes1, (unsigned long long)jobs1, w.Live(), (unsigned long long)w.cells.Population(), (unsigned long long)w.cells.Working(),
                    (unsigned long long)jobs30, (unsigned long long)w.growth.levelUps);
        std::printf("grow: demand R %.2f C %.2f O %.2f I %.2f\n", w.growth.demand[0], w.growth.demand[1], w.growth.demand[2], w.growth.demand[3]);
        CHECK(day1 > 0);
        CHECK(homes1 > jobs1); // the start-up pull is residential
        CHECK(w.Live() > day1);
        CHECK(w.cells.Population() > 100);
        CHECK(jobs30 > 0);
        CHECK(w.cells.Working() > 0);
        CHECK(w.growth.levelUps > 0);
        hashA = w.Hash();
    }
    // Determinism: the same run again.
    {
        World w;
        w.Setup(true);
        w.tune.levelUpLandValue = 0;
        w.tune.levelUpDays = 2;
        w.Days(30);
        CHECK(w.Hash() == hashA);
    }
    // Abandonment: grown for 20 days, then no power anywhere for longer than levelDownDays.
    {
        World w;
        w.Setup(false); // level 1 only: bad conditions abandon
        w.Days(20);
        const int before = w.Live();
        w.forceBad = true;
        w.zones.gateSpawns = true;
        w.Days(w.tune.levelDownDays + 1.5);
        std::printf("decay: %d lots before, %d after %.1f bad days, abandoned %llu, pop %llu\n", before, w.Live(), w.tune.levelDownDays + 1.5,
                    (unsigned long long)w.growth.abandoned, (unsigned long long)w.cells.Population());
        CHECK(before > 0);
        // Patience is 6-12 days per lot, so after 7.5 bad days part of the district has gone; abandoned plots regrow (and
        // fail again) the way they do in CS2, so the lot count itself need not fall.
        CHECK(w.growth.abandoned >= (uint64_t)before / 5);
    }
    // No next-level model: logged once, nothing changes.
    {
        World w;
        w.Setup(false);
        w.tune.levelUpLandValue = 0;
        w.tune.levelUpDays = 1;
        w.Days(12);
        int noModel = 0;
        for (const std::string& e : w.growth.events) noModel += e.rfind("no ", 0) == 0;
        CHECK(w.growth.levelUps == 0);
        CHECK(noModel > 0 && noModel <= 7 * 16);
    }
    // Months: default construction cap, weekly samples; demand must settle (few sign flips of R), the city keeps growing.
    {
        World w;
        w.Setup(true);
        w.zones.maxConstructing = 8;
        int flips = 0, prevSign = 0;
        uint64_t prevPop = 0;
        bool popFell = false;
        for (int week = 1; week <= 17; ++week) {
            w.Days(7);
            const int sign = w.growth.demand[0] > 0.05 ? 1 : w.growth.demand[0] < -0.05 ? -1 : 0;
            if (sign && prevSign && sign != prevSign) ++flips;
            if (sign) prevSign = sign;
            const uint64_t pop = w.cells.Population();
            if (week > 4 && pop + pop / 5 < prevPop) popFell = true; // a 20% drop in a week is an oscillation
            prevPop = pop;
            if (week % 2 == 1)
                std::printf("week %2d: lots %4d pop %5llu working %5llu jobs %5llu  R %+.2f C %+.2f O %+.2f I %+.2f  ups %llu downs %llu abandoned %llu\n", week, w.Live(),
                            (unsigned long long)pop, (unsigned long long)w.cells.Working(), (unsigned long long)w.cells.JobCap(), w.growth.demand[0], w.growth.demand[1],
                            w.growth.demand[2], w.growth.demand[3], (unsigned long long)w.growth.levelUps, (unsigned long long)w.growth.levelDowns,
                            (unsigned long long)w.growth.abandoned);
        }
        std::printf("months: residential demand sign flips %d\n", flips);
        CHECK(flips <= 3);
        CHECK(!popFell);
    }
    // Tuning JSON.
    {
        SimTuning t;
        std::vector<std::string> unknown;
        const int n = t.ParseJson("{ \"growThreshold\": 0.2, \"comment\": \"text\", \"levelUpDays\" : 5, \"nope\": 1 }", &unknown);
        CHECK(n == 2);
        CHECK(t.growThreshold == 0.2 && t.levelUpDays == 5.0);
        CHECK(unknown.size() == 1 && unknown[0] == "nope");
    }

    if (failures) {
        std::printf("sim_growth_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_growth_test: all passed\n");
    return 0;
}
