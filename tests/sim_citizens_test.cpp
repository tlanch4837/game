// Headless checks for the Phase 1 citizen records (src/city/sim_citizens.h, sim_life_world.h, sim_life_boot.h,
// sim_life_save.h, sim_life.h):
//   handles and generations (a freed citizen's id never matches the next occupant); the bootstrap from lots equals the old
//   CitySim counters (ages and education exactly, wealth exactly on lots of 5+); the incremental views equal a full
//   recount after 10k random operations (births, deaths, hires, fires, moves, evictions, building removal); determinism
//   (same seed same hash, other seed other hash); the UI views and the active tier (sim_life_views.h); save / load through plain byte vectors continues identically; a bench
//   of ms per tick at 50k residents (target 0.25 ms average).
// Build and run: tests\sim_citizens_test.bat
#include <chrono>
#include <cstdio>

#include "city/sim_cells.h"
#include "sim_life_town.h"

using namespace lifetest;

static void TestHandles() {
    CitizenStore s;
    const CitizenId a = s.Alloc(), b = s.Alloc();
    CHECK(a.Valid() && b.Valid() && a != b && s.Live() == 2);
    s.Free(a);
    CHECK(!s.Alive(a) && s.Alive(b) && s.Live() == 1);
    const CitizenId c = s.Alloc(); // reuses a's slot with a new generation
    CHECK(c.Index() == a.Index() && c.Gen() != a.Gen() && !s.Alive(a) && s.Alive(c));
    CHECK(s.flags[c.Index()] == kCitAlive && s.household[c.Index()] == 0);
    for (int i = 0; i < 300; ++i) { const CitizenId t = s.Alloc(); s.Free(t); } // generation wraps, never 0
    CHECK(!s.Alive(CitizenId(0)) && s.Alive(c));
}

// The old counters: CitySim from built lots. The new: LifeBootstrap over registry rows with the same homes and cells.
static void BootstrapCity(bool smallLots) {
    std::vector<Lot> lots;
    uint64_t id = 1;
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x) {
            const int type = (x + z) % 4; // the four residential densities
            const int w = smallLots ? 1 + (x % 2) : 2 + (x % 3), d = smallLots ? 1 + (z % 2) : 2 + (z % 2);
            Lot l;
            l.id = id++, l.c = {x * 128.0f + 30.0f, z * 128.0f + 40.0f}, l.type = type, l.w = w, l.d = d, l.progress = 2.0f;
            l.path = "T" + std::to_string(type) + "_" + std::to_string(w) + "x" + std::to_string(d) + "_L1.glb";
            lots.push_back(l);
            Lot l2 = l;
            l2.id = id++, l2.c.x += 50.0f, l2.path += "b";
            lots.push_back(l2);
        }
    CitySim old;
    old.Sync(lots, 1);
    BuildingRegistry reg;
    reg.BeginSync();
    bool allBig = true;
    for (const Lot& l : lots) {
        BuildingRow r;
        r.cellX = (int)(l.c.x / 128.0f), r.cellZ = (int)(l.c.z / 128.0f);
        r.cell = old.cells.Find(CellKeyAt(l.c.x, l.c.z)), r.hood = 0, r.homes = LotCapacity(l).homes;
        const uint32_t occ = r.homes >= 2 ? r.homes - std::max<uint32_t>(1u, r.homes / 20u) : r.homes;
        allBig = allBig && occ >= 5;
        reg.Upsert(BOrigin::Lot, l.id, r);
    }
    reg.EndSync();
    LifeSim s;
    s.SyncBuildings(reg);
    s.Bootstrap();
    std::string why;
    CHECK(s.w.Verify(&why));
    if (!why.empty()) std::printf("  verify: %s\n", why.c_str());
    int ageBad = 0, eduBad = 0, wealthBad = 0;
    uint32_t total = 0;
    for (size_t c = 0; c < old.sim.size(); ++c) {
        const CellSim& o = old.sim[c];
        const LifeCell n = c < s.w.cells.size() ? s.w.cells[c] : LifeCell();
        for (int a = 0; a < 3; ++a) ageBad += o.residents[a] != n.residents[a], total += n.residents[a];
        eduBad += o.edu[0] != n.edu[0] + n.edu[1] || o.edu[1] != n.edu[2] || o.edu[2] != n.edu[3] + n.edu[4];
        for (int b = 0; b < 4; ++b) wealthBad += o.wealth[b] != n.wealth[b];
    }
    CHECK(ageBad == 0 && eduBad == 0 && total == s.w.cit.Live() && total > 0);
    CHECK(wealthBad == 0 || (smallLots && !allBig));
    std::printf("  bootstrap (%s lots): %u residents, %u households, cells off: age %d edu %d wealth %d\n", smallLots ? "small" : "large",
                total, s.w.hh.Live(), ageBad, eduBad, wealthBad);
}

// 10k random operations through the LifeWorld primitives and the phase functions; the views must equal a recount.
static void TestDeltaView() {
    Town t;
    t.sim.w.citySeed = 77;
    t.Build(8, 40, 30);
    t.sim.Bootstrap();
    LifeWorld& w = t.sim.w;
    SplitMix64 r(12345);
    int removed = 0;
    for (int op = 0; op < 10000; ++op) {
        const uint32_t ci = 1 + (uint32_t)(r.Next() % std::max<uint32_t>(1, w.cit.Capacity() - 1));
        const bool live = ci < w.cit.Capacity() && w.cit.At(ci).Valid();
        switch (r.Next() % 10) {
        case 0: if (live && w.cit.stage[ci] >= kLifeAdult) LifeBirth(w, ci); break;
        case 1: if (live) LifeDie(w, ci); break;
        case 2: if (live && LifeIsWorkingAge(w, ci)) { w.jobCursor.resize(w.hoodJobs.size()); LifeTryHire(w, ci, w.jobCursor); } break;
        case 3: if (live && w.cit.work[ci]) w.SetWork(ci, 0, 0); break;
        case 4: if (live) { const uint32_t hi = LifeWorld::Ix(w.cit.household[ci]); uint32_t cur = (uint32_t)(r.Next() % w.bld.size());
                            const uint32_t b = LifeFindHome(w, w.hh.size[hi], w.hh.home[hi], cur); if (b) w.MoveHousehold(hi, b); } break;
        case 5: if (live) w.SetEdu(ci, (int)(r.Next() % 5)); break;
        case 6: if (live) LifeCitizenMonth(w, ci); break;
        case 7: if (live) LifeHouseholdVisit(w, LifeWorld::Ix(w.cit.household[ci])); break;
        case 8: { LifeMigrationState m; LifeMigrationMonth(w, m); LifeImmigrate(w, m, 3); } break;
        default: break;
        }
        if (op % 400 == 200) { // a building goes, or one shrinks
                const uint64_t victim = t.ids[(size_t)(r.Next() % t.ids.size())];
                const bool shrink = (r.Next() & 1) != 0;
                t.Resync([&](uint64_t id, BuildingRow& row) {
                    if (id != victim) return true;
                    if (!shrink) return false;
                    row.homes = (uint16_t)(row.homes / 3);
                    for (int e = 0; e < kEduLevels; ++e) row.jobSlots[e] = (uint16_t)(row.jobSlots[e] / 3);
                    return true;
                });
                ++removed;
        }
        if (op % 2000 == 1999) { std::string why; const bool ok = w.Verify(&why); CHECK(ok); if (!ok) { std::printf("  op %d: %s\n", op, why.c_str()); return; } }
    }
    std::string why;
    CHECK(w.Verify(&why));
    std::printf("  delta view: 10k ops, %d building changes, %u citizens, %u working, %s\n", removed, w.cit.Live(), w.Working(), why.empty() ? "views = recount" : why.c_str());
}

static uint64_t RunTown(uint64_t seed, int days) {
    Town t;
    t.sim.w.citySeed = seed;
    t.Build(8, 40, 30);
    t.sim.Bootstrap();
    int64_t minute = 0;
    t.RunDays(minute, days);
    return LifeHash(t.sim.w);
}
static void TestDeterminism() {
    const uint64_t a = RunTown(5, 30), b = RunTown(5, 30), c = RunTown(6, 30);
    CHECK(a == b);
    CHECK(a != c);
    std::printf("  determinism: hash %016llx twice, other seed %016llx\n", (unsigned long long)a, (unsigned long long)c);
}

// UI views and the active tier: totals add up, the inspector lists exactly the building's records, the active set stays
// within 512, workers are at work at 10:00 on a weekday and everyone sleeps at 03:00, TenantOf walks the same records.
static void TestViews() {
    Town t;
    t.sim.w.citySeed = 3;
    t.Build(8, 40, 30);
    t.sim.Bootstrap();
    int64_t minute = 0;
    t.RunDays(minute, 3);
    LifeWorld& w = t.sim.w;
    LifePopulationStats ps;
    LifePopulation(w, ps);
    uint32_t sum = 0;
    for (int b = 0; b < LifePopulationStats::kBins; ++b) sum += ps.male[b] + ps.female[b];
    CHECK(sum == w.cit.Live() && ps.births.size() == w.history.size() && ps.working == w.Working());
    std::vector<uint32_t> near;
    for (const LifeBuilding& L : w.bld) if (L.id && L.homes) near.push_back(L.id);
    std::vector<LifeHouseholdInfo> info;
    uint32_t stat = 0, listed = 0;
    LifeBuildingInfo(w, near[0], info, stat);
    for (const LifeHouseholdInfo& h : info) listed += (uint32_t)h.members.size();
    CHECK(listed == w.B(near[0])->used && listed > 0);
    CHECK(t.sim.TenantOf(near[0], 0) == info[0].members[0].id && !t.sim.TenantOf(near[0], listed).Valid());
    LifeActiveSet as;
    const int64_t day = minute - minute % 1440 + 1440 * (w.cal.Weekend(minute) ? 2 : 0); // a weekday start
    as.Refresh(w, near, day + 600);
    int working = 0, atWork = 0, asleep = 0;
    for (size_t i = 0; i < as.members.size(); ++i) {
        const uint32_t ci = CitizenId(as.members[i]).Index();
        if (w.cit.work[ci] && !w.cal.Weekend(day + 600)) ++working, atWork += as.act[i] == kActWork;
        asleep += LifeActivityAt(w, ci, day + 180) == kActSleep;
    }
    std::vector<LifeActivityChange> ch;
    size_t changes = 0;
    const auto a = std::chrono::steady_clock::now();
    for (int64_t m = day; m < day + 1440; ++m) as.Tick(w, m, ch), changes += ch.size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 1440.0;
    std::printf("  views: pyramid sums to %u, inspector lists %u, active set %zu (cap 512): %d/%d workers at work at 10:00, %d asleep at 03:00, "
                "%zu activity changes a day, %.4f ms per minute\n", sum, listed, as.members.size(), atWork, working, asleep, changes, ms);
    CHECK(as.members.size() <= 512 && as.members.size() > 100);
    CHECK(atWork == working && asleep == (int)as.members.size() && changes > as.members.size());
    CHECK(ms < 0.1);
}

// Save after 20 days, load into a fresh sim, run both 20 more days: identical. A truncated chunk means start fresh.
static void TestSaveLoad() {
    Town a;
    a.sim.w.citySeed = 31;
    a.Build(8, 40, 30);
    a.sim.Bootstrap();
    int64_t minute = 0;
    a.RunDays(minute, 20);
    a.RunMinutes(minute, 437), minute += 437; // mid-month, mid-slice
    LifeChunks c;
    LifeSave(a.sim, c);
    Town b;
    b.sim.w.citySeed = 31;
    b.reg = a.reg;
    const bool ok = LifeLoad(b.sim, c);
    CHECK(ok);
    CHECK(LifeHash(a.sim.w) == LifeHash(b.sim.w));
    b.sim.SyncBuildings(b.reg); // the host syncs after a load: nothing changes
    std::string why;
    CHECK(b.sim.w.Verify(&why));
    a.RunMinutes(minute, 20 * 1440), b.RunMinutes(minute, 20 * 1440);
    CHECK(LifeHash(a.sim.w) == LifeHash(b.sim.w));
    CHECK(a.sim.w.Population() == b.sim.w.Population());
    LifeChunks bad = c;
    bad.hhld.resize(bad.hhld.size() / 2);
    Town d;
    CHECK(!LifeLoad(d.sim, bad) && d.sim.w.cit.Live() == 0);
    bad = c;
    for (size_t i = 16; i < bad.citz.size(); i += 97) bad.citz[i] ^= 0x5A; // garbage in the columns
    CHECK(!LifeLoad(d.sim, bad) || d.sim.w.Verify());
    std::printf("  save/load: %zu + %zu + %zu + %zu bytes, continues identically (%u residents after 40 days)\n", c.citz.size(), c.hhld.size(),
                c.occu.size(), c.migr.size(), a.sim.w.Population());
}

// ms per tick at about 50k residents: bootstrap, settle a month, then time 10 game days tick by tick.
static void Bench(uint32_t homesPerCell, int side, int days) {
    Town t;
    t.sim.w.citySeed = 9;
    t.Build(side, homesPerCell, homesPerCell * 7 / 10);
    t.sim.Bootstrap();
    int64_t minute = 0;
    t.RunDays(minute, 1);
    const uint32_t pop = t.sim.w.Population();
    double total = 0, worst = 0;
    int64_t worstAt = 0;
    double byKind[4] = {}; // month start, move pass, slices, job passes
    const int64_t ticks = (int64_t)days * 1440;
    for (int64_t m = minute; m < minute + ticks; ++m) {
        const auto a = std::chrono::steady_clock::now();
        t.sim.Tick(m);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        total += ms;
        if (ms > worst) worst = ms, worstAt = m;
        const int64_t in = m % 1440;
        byKind[in == 0 ? 0 : in == 3 ? 1 : in % 15 == 7 ? 2 : 3] = std::max(byKind[in == 0 ? 0 : in == 3 ? 1 : in % 15 == 7 ? 2 : 3], ms);
    }
    const double avg = total / (double)ticks;
    std::printf("  bench worst by step: month start %.3f, move pass %.3f, slice %.3f, job pass %.3f ms (worst at minute %lld of its day)\n", byKind[0],
                byKind[1], byKind[2], byKind[3], (long long)(worstAt % 1440));
    std::printf("  bench: %u residents (%u at end), %u households, %d game days: %.4f ms/tick avg, worst %.3f ms (target 0.25 avg)\n", pop,
                t.sim.w.Population(), t.sim.w.hh.Live(), days, avg, worst);
    const size_t bytes = t.sim.w.cit.Capacity() * (size_t)(6 * 4 + 6 + 2 + 2) + t.sim.w.hh.Capacity() * (size_t)(3 * 4 + 5 * 4 + 7 + 2);
    std::printf("  memory: records %.2f MB\n", (double)bytes / (1024.0 * 1024.0));
    CHECK(avg < 0.25);
}

int main(int argc, char** argv) {
    const bool benchOnly = argc > 1 && std::string(argv[1]) == "bench";
    if (!benchOnly) {
        TestHandles();
        BootstrapCity(false);
        BootstrapCity(true);
        TestDeltaView();
        TestDeterminism();
        TestViews();
        TestSaveLoad();
    }
    Bench(33, 40, 10);
    if (failures) { std::printf("sim_citizens_test: %d FAILED\n", failures); return 1; }
    std::printf("sim_citizens_test: all passed\n");
    return 0;
}
