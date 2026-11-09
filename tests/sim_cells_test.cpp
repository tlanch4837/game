// Headless checks for the city sim's per-cell counters (src/city/sim_cells.h, src/city/city_cells.h):
//   LotCapacity matches the per-area factors; conservation (adults >= working, employed <= jobs, vacancies = jobs -
//   employed, working == employed == links city-wide and per supercell) under a random sequence of spawns, completions,
//   level swaps and demolitions; demolishing everything returns every counter to zero; same inputs give the same result;
//   a full matching pass for ~50k residents over 10k lots is timed (target under 1 ms). Phase 1: with externalPeople a lot
//   brings empty homes and job slots only; the citizen records' bootstrap equals the old counters per cell exactly, and the
//   cells and links copied from the records (sim_life_cells.h) keep the same invariants through matching and demolition.
// Build and run: tests\sim_cells_test.bat
#include <chrono>
#include <cstdio>
#include <vector>

#include "city/sim.h"
#include "city/sim_cells.h"
#include "city/sim_life_cells.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool Invariants(const CitySim& s, bool verbose = false) {
    bool ok = true;
    for (size_t i = 0; i < s.sim.size(); ++i) {
        const CellSim& c = s.sim[i];
        if (c.working > c.residents[kAdults]) ok = false;
        for (int k = 0; k < kJobKinds; ++k) {
            if (c.employed[k] > c.jobCap[k]) ok = false;
            if (c.vacancies[k] != c.jobCap[k] - c.employed[k]) ok = false;
        }
    }
    if (s.Working() != s.Employed() || s.Employed() != s.Links()) ok = false;
    std::vector<uint64_t> work((size_t)s.Supercells(), 0), emp((size_t)s.Supercells(), 0);
    for (size_t i = 0; i < s.sim.size(); ++i) {
        work[(size_t)s.SuperOf((int)i)] += s.sim[i].working;
        emp[(size_t)s.SuperOf((int)i)] += (uint64_t)s.sim[i].employed[0] + s.sim[i].employed[1] + s.sim[i].employed[2];
    }
    for (int sc = 0; sc < s.Supercells(); ++sc) {
        uint64_t h, j;
        s.SuperLinks(sc, h, j);
        if (h != work[(size_t)sc] || j != emp[(size_t)sc]) ok = false;
    }
    if (!ok && verbose) std::printf("  working %llu employed %llu links %llu\n", (unsigned long long)s.Working(), (unsigned long long)s.Employed(), (unsigned long long)s.Links());
    return ok;
}

static Lot MakeLot(uint64_t id, float x, float z, int type, int w, int d, bool built) {
    Lot l;
    l.id = id;
    l.c = {x, z};
    l.type = type, l.w = w, l.d = d;
    l.path = "T" + std::to_string(type) + "_" + std::to_string(w) + "x" + std::to_string(d) + "_L1.glb";
    l.progress = built ? 2.0f : 0.0f;
    return l;
}

// A random city history: returns the final hash; checks the invariants after every sync.
static uint64_t Fuzz(uint64_t seed, int steps, bool& invOk) {
    SplitMix64 r(seed);
    CitySim s;
    std::vector<Lot> lots;
    uint64_t version = 1, nextId = 1;
    invOk = true;
    for (int step = 0; step < steps; ++step) {
        const uint32_t op = r.Below(100);
        if (op < 45 || lots.empty()) { // spawn
            const int type = (int)r.Below(7);
            lots.push_back(MakeLot(nextId++, (float)r.Below(3000) - 1500.0f, (float)r.Below(3000) - 1500.0f, type, 1 + (int)r.Below(6), 1 + (int)r.Below(6), false));
        } else if (op < 75) { // finish one
            Lot& l = lots[r.Below((uint32_t)lots.size())];
            l.progress = 2.0f;
        } else if (op < 85) { // level swap (same footprint, new model)
            Lot& l = lots[r.Below((uint32_t)lots.size())];
            l.path += "+";
            l.progress = r.Below(2) ? 2.0f : 0.0f;
        } else { // demolish (alive false, then compacted away some of the time)
            const size_t i = r.Below((uint32_t)lots.size());
            if (r.Below(2)) lots[i].alive = false;
            else lots.erase(lots.begin() + (ptrdiff_t)i);
        }
        s.Sync(lots, ++version);
        if (!Invariants(s)) invOk = false;
    }
    // demolish all: everything back to zero
    lots.clear();
    s.Sync(lots, ++version);
    if (s.Population() != 0 || s.Working() != 0 || s.Employed() != 0 || s.JobCap() != 0 || s.HomeCap() != 0 || s.Links() != 0 || s.Lots() != 0) invOk = false;
    return s.Hash() ^ (uint64_t)s.Population();
}

// Phase 1: the counters as a view of the citizen records (CitySim::externalPeople, sim_life_cells.h).
static void SyncLife(LifeSim& life, BuildingRegistry& reg, const CitySim& s, const std::vector<Lot>& lots) {
    reg.BeginSync();
    for (const Lot& l : lots) {
        BuildingRow r;
        const CellKey k = CellKeyAt(l.c.x, l.c.z);
        r.kind = l.type, r.cellX = k.x, r.cellZ = k.z, r.level = (uint8_t)l.level;
        reg.Upsert(BOrigin::Lot, l.id, r);
    }
    reg.EndSync();
    LifeFillRows(reg, s, lots, [&s](int c) { return s.SuperOf(c); }); // a hood per supercell
    life.SyncBuildings(reg, [](const BuildingRow& r) { return LifeJobKind(r.kind); });
}
static void TestExternalPeople() {
    SplitMix64 r(77);
    std::vector<Lot> lots;
    for (uint64_t i = 0; i < 1500; ++i) {
        const uint32_t pick = r.Below(100);
        const int type = pick < 40 ? (int)r.Below(4) : pick < 60 ? 4 : pick < 80 ? 5 : 6;
        lots.push_back(MakeLot(i + 1, (float)r.Below(2400) - 1200.0f, (float)r.Below(1600) - 800.0f, type, 1 + (int)r.Below(3), 1 + (int)r.Below(3), true));
    }
    CitySim old; // the old fixed split, for the bootstrap comparison
    old.Sync(lots, 1);
    CitySim s;
    s.externalPeople = true;
    s.Sync(lots, 1);
    CHECK(s.Population() == 0 && s.Working() == 0 && s.Links() == 0); // empty homes and job slots only
    CHECK(s.HomeCap() == old.HomeCap() && s.JobCap() == old.JobCap());
    CHECK(Invariants(s, true));
    s.Clear();
    CHECK(s.externalPeople); // Clear keeps the mode
    s.Sync(lots, 2);

    // bootstrap (no ticks): every cell's residents and education equal the old counters exactly
    BuildingRegistry reg;
    LifeSim life;
    life.Reset(5);
    SyncLife(life, reg, s, lots);
    life.Bootstrap();
    LifeApplyCells(life.w, s);
    bool same = s.sim.size() == old.sim.size();
    for (size_t c = 0; same && c < s.sim.size(); ++c)
        for (int a = 0; a < 3; ++a) same = same && s.sim[c].residents[a] == old.sim[c].residents[a] && s.sim[c].edu[a] == old.sim[c].edu[a];
    CHECK(same);
    CHECK(s.Population() == old.Population() && s.Population() == life.w.Population());

    // a few days of job matching: the links come from real assignments and keep every invariant
    for (int64_t m = 0; m < 3 * 1440; ++m) life.Tick(m);
    CitySim deferred = s; // the host's deferred apply (LifeApplyState) gives the same cells and links, and skips a no-change
    LifeApplyState st;
    LifeApplyCells(life.w, deferred, &st);
    LifeApplyCells(life.w, s);
    CHECK(deferred.Hash() == s.Hash() && deferred.Links() == s.Links() && st.rebuilt == 1);
    LifeApplyCells(life.w, deferred, &st);
    CHECK(deferred.Hash() == s.Hash() && st.skipped == 1 && st.rebuilt == 1);
    LifeSim later = life; // (a copy: the checks below run on the original)
    for (int64_t m = 3 * 1440; m < 3 * 1440 + 30; ++m) later.Tick(m); // job passes change links: rebuilt again, same as a full apply
    CitySim full = deferred;
    LifeApplyCells(later.w, deferred, &st);
    LifeApplyCells(later.w, full);
    CHECK(deferred.Hash() == full.Hash());
    std::printf("external: %llu residents, %llu working, %llu links, %d supercells\n", (unsigned long long)s.Population(), (unsigned long long)s.Working(),
                (unsigned long long)s.Links(), s.Supercells());
    CHECK(s.Working() > 0 && s.Working() == s.Employed() && s.Employed() == s.Links());
    CHECK(Invariants(s, true));
    std::string why;
    CHECK(life.w.Verify(&why));
    auto crowded = [&s] { uint64_t n = 0; for (const CellSim& c : s.sim) n += c.Population() > c.homeCap ? c.Population() - c.homeCap : 0; return n; };
    const uint64_t crowdedBefore = crowded(); // births may crowd a home (that is no eviction)
    std::printf("external: before demolition %llu people over their cells' homes\n", (unsigned long long)crowdedBefore);

    // demolish a third: capacity goes at once, the people when the records follow (evicted, rehoused or gone)
    std::vector<Lot> keep;
    for (const Lot& l : lots) if (l.id % 3) keep.push_back(l);
    s.Sync(keep, 3);
    SyncLife(life, reg, s, keep);
    LifeApplyCells(life.w, s);
    CHECK(Invariants(s, true));
    std::printf("external: after demolition %llu residents in %llu homes, %llu over their cells' homes\n", (unsigned long long)s.Population(),
                (unsigned long long)s.HomeCap(), (unsigned long long)crowded());
    CHECK(s.Population() <= s.HomeCap() + crowdedBefore && crowded() <= crowdedBefore); // the evicted found free homes or left
    CHECK(life.w.Verify(&why));
}

int main() {
    // Capacity factors (TransitDemand's): LD 2x3 = 6 cells * 0.8 = 4.8 -> 5 homes; HD 4x4 * 5 = 80; office 3x3 * 2.5 = 22.5 -> 23.
    CHECK(LotCapacity(0, 2, 3).homes == 5);
    CHECK(LotCapacity(3, 4, 4).homes == 80);
    CHECK(LotCapacity(5, 3, 3).jobs[kJobOffice] == 23);
    CHECK(LotCapacity(6, 2, 2).jobs[kJobIndustrial] == 8);
    CHECK(LotCapacity(4, 2, 2).jobs[kJobCommercial] == 6);
    CHECK(LotCapacity(4, 2, 2).homes == 0);

    // Cell assignment by lot centre; registry dirty bits.
    {
        CitySim s;
        std::vector<Lot> lots = {MakeLot(1, 10, 10, 0, 2, 2, false), MakeLot(2, 130, 10, 0, 2, 2, false), MakeLot(3, -1, -1, 0, 2, 2, false)};
        s.Sync(lots, 1);
        CHECK(s.cells.cells.size() == 3);
        CHECK(s.cells.Find(CellKeyAt(10, 10)) >= 0 && s.cells.Find({1, 0}) >= 0 && s.cells.Find({-1, -1}) >= 0);
        CHECK(s.Population() == 0); // not built yet
        const int c0 = s.cells.Find({0, 0});
        CHECK(s.cells.cells[(size_t)c0].dirty & kDirtyRender);
        CHECK(!s.Sync(lots, 1)); // same version: nothing to do
        lots[0].progress = 2;
        s.Sync(lots, 2);
        const uint32_t homes = LotCapacity(0, 2, 2).homes; // 2x2 low density: 3 homes, one kept free (5%, at least 1)
        CHECK(s.Population() == homes - 1);
        CHECK(s.HomeCap() == homes);
    }

    // Jobs next door are taken; jobs 3 supercells away are not (rings = 2).
    {
        CitySim s;
        std::vector<Lot> lots = {MakeLot(1, 10, 10, 3, 6, 6, true), MakeLot(2, 140, 10, 6, 6, 6, true), MakeLot(3, 10 + 128 * 4 * 3, 10, 5, 6, 6, true)};
        s.Sync(lots, 1);
        const uint32_t adults = (uint32_t)s.Adults();
        CHECK(adults > 0);
        // industrial lot: 36 * 2 = 72 jobs, all within reach; office lot 3 supercells east: out of reach
        CHECK(s.Working() == std::min<uint64_t>(adults, 72));
        CHECK(Invariants(s, true));
        // demolish the factory: everyone loses their job, the office stays out of reach
        lots.erase(lots.begin() + 1);
        s.Sync(lots, 2);
        CHECK(s.Working() == 0);
        CHECK(Invariants(s, true));
    }

    // Fuzz: invariants after every step, zero after demolishing all, deterministic.
    {
        bool ok1 = false, ok2 = false;
        const uint64_t h1 = Fuzz(7, 4000, ok1), h2 = Fuzz(7, 4000, ok2);
        CHECK(ok1 && ok2);
        CHECK(h1 == h2);
        bool ok3 = false;
        Fuzz(12345, 4000, ok3);
        CHECK(ok3);
    }

    // Scale: ~50k residents on 10k lots over a 4.5 x 2.3 km grid; time a full matching pass from scratch.
    {
        SplitMix64 r(99);
        std::vector<Lot> lots;
        for (uint64_t i = 0; i < 10000; ++i) {
            const uint32_t pick = r.Below(100);
            const int type = pick < 30 ? 0 : pick < 45 ? 1 : pick < 58 ? 2 : pick < 64 ? 3 : pick < 78 ? 4 : pick < 88 ? 5 : 6;
            lots.push_back(MakeLot(i + 1, (float)r.Below(4560) - 2280.0f, (float)r.Below(2280) - 1140.0f, type, 1 + (int)r.Below(3), 1 + (int)r.Below(4), true));
        }
        CitySim s;
        const auto t0 = std::chrono::steady_clock::now();
        s.Sync(lots, 1); // capacities + first match
        const auto t1 = std::chrono::steady_clock::now();
        // a fresh full pass: everybody quits (demolish all jobs), then all jobs come back and are matched again
        std::vector<Lot> homesOnly;
        for (const Lot& l : lots)
            if (ZoneTypes()[l.type].cat == ZoneCat::Residential) homesOnly.push_back(l);
        s.Sync(homesOnly, 2);
        CHECK(s.Working() == 0);
        const auto t2 = std::chrono::steady_clock::now();
        s.Sync(lots, 3);
        const auto t3 = std::chrono::steady_clock::now();
        // a match pass with nothing to do (the steady state cost)
        const auto t4 = std::chrono::steady_clock::now();
        s.Match();
        const auto t5 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::printf("scale: %llu residents (%llu adults), %llu jobs, %llu working, %zu cells, %d supercells\n", (unsigned long long)s.Population(),
                    (unsigned long long)s.Adults(), (unsigned long long)s.JobCap(), (unsigned long long)s.Working(), s.cells.cells.size(), s.Supercells());
        std::printf("scale: first sync %.3f ms, drop all jobs %.3f ms, re-add + full match %.3f ms, idle match %.3f ms\n", ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t4, t5));
        CHECK(s.Population() >= 40000 && s.Population() <= 70000);
        CHECK(Invariants(s, true));
        CHECK(ms(t4, t5) < 1.0);
        // The full sync includes the lot diff (10k hash lookups); the matching share must be under 1 ms.
        s.Sync(homesOnly, 4);
        const auto t6 = std::chrono::steady_clock::now();
        s.Sync(lots, 5);
        const auto t7 = std::chrono::steady_clock::now();
        std::printf("scale: second re-add + full match %.3f ms\n", ms(t6, t7));
    }

    TestExternalPeople();

    if (failures) {
        std::printf("sim_cells_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_cells_test: all passed\n");
    return 0;
}
