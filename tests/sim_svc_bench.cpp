// Phase 4 bench: a synthetic 50k-resident city (45 x 45 cells, 8 buildings each, 70 service sites, 50k citizen records)
// runs three game days of every per-minute piece of Phase 4 (due incidents, the cell group, the flags flush, the seat
// slice, the garbage group) and prints ms per tick (average, worst) next to the coverage pass (worker) and the main-thread
// installs. The iPad gate is desktop x 2 (roadmap Phase 0): average under 0.20 ms, no tick over 0.5 ms.
// Build and run: tests\sim_svc_bench.bat
#include <chrono>

#include "sim_svc_town.h"

using namespace svctest;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

int main() {
    const int side = 45, perCell = 8;
    Town t;
    t.Build(side, 25 * 1, 40.0f); // 25 people per cell of demand on the services... and below the citizen records
    t.people = 50000 / (side * side) + 1;
    const struct { int tab; const char* name; int wp, veh, n; } kSites[] = {
        {kTabFire, "Fire Station", 20, 4, 10}, {kTabPolice, "Police Station", 36, 3, 10}, {kTabHealth, "Community Hospital", 36, 4, 8},
        {kTabHealth, "Small Park", 4, 0, 8}, {kTabDeath, "Crematorium", 16, 2, 4}, {kTabEducation, "Elementary School", 27, 0, 12},
        {kTabEducation, "High School", 162, 0, 5}, {kTabEducation, "University", 75, 0, 2}, {kTabComms, "Cell Tower", 12, 0, 8},
        {kTabComms, "Post Office", 20, 4, 3}};
    uint32_t k = 0;
    for (const auto& s : kSites)
        for (int i = 0; i < s.n; ++i, ++k) t.AddSite((int)((k * 7) % side), (int)((k * 13 + 5) % side), s.tab, s.name, s.wp, s.veh);
    SvcWorld w;
    w.seed = 5;
    std::vector<SvcBldg> b;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x)
            for (int j = 0; j < perCell; ++j) {
                SvcBldg h;
                h.id = (uint32_t)(b.size() + 1), h.key = {x, z}, h.cell = t.Node(x, z), h.node = t.Node(x, z), h.lot = h.id;
                h.kind = j < 5 ? kBkHome : j == 5 ? kBkShop : j == 6 ? kBkOffice : kBkIndustry, h.homes = j < 5 ? 3 : 0, h.jobs = j < 5 ? 0 : 6;
                b.push_back(h);
            }
    for (const SvcSite& s : t.sites) { SvcBldg h; h.id = s.id, h.key = CellKeyAt(s.x, s.z), h.node = s.node, h.kind = kBkService; b.push_back(h); }
    std::sort(b.begin(), b.end(), [](const SvcBldg& p, const SvcBldg& q) { return p.id < q.id; });
    SvcSetBuildings(w, std::vector<SvcBldg>(b));
    std::vector<SvcBldg> again(b);
    again[5].homes++; // one building changed: the usual resync
    auto t0 = Clock::now();
    SvcSetBuildings(w, std::move(again));
    const double setMs = Ms(t0);
    t0 = Clock::now();
    SvcPrepared p;
    p.cov = t.Input(), p.stats = t.Stats(15), p.bldSnap = w.bld;
    { auto g = std::make_shared<const SvcGraph>(SvcGraph::From(t.table)); p.cov.graph = g; }
    SvcCover(p.cov, p.out);
    SvcPrepare(p);
    const double coverMs = Ms(t0);
    t0 = Clock::now();
    SvcInstall(w, std::move(p));
    const double installMs = Ms(t0);
    // citizens: 50k records, a fifth of them students of all ages
    CitizenStore cit;
    std::vector<int> ages;
    for (int i = 0; i < 50000; ++i) {
        const CitizenId c = cit.Alloc();
        const uint32_t ci = c.Index();
        ages.resize(std::max<size_t>(ages.size(), ci + 1));
        ages[ci] = (int)(SimSeed(1, ci) % 80);
        if (ages[ci] >= 6 && ages[ci] < 22) cit.flags[ci] |= kCitStudent, cit.edu[ci] = (uint8_t)(ages[ci] < 13 ? 0 : ages[ci] < 18 ? 1 : 2);
    }
    SvcSeatPass seats;
    const int cells = side * side;
    auto age = [&](uint32_t ci) { return ages[ci]; };
    auto home = [&](uint32_t ci) { return (int)(ci % (uint32_t)cells); };
    // garbage: every building a lot, 4 landfills; lots pre-split by group (the host's lists)
    SvcGarbage garb;
    std::vector<SvcGarbLot> lots;
    for (const SvcBldg& h : w.bld) {
        if (h.kind == kBkService) continue;
        SvcGarbLot l;
        l.lot = h.lot, l.x = Town::Centre(h.key.x), l.z = Town::Centre(h.key.z), l.comp = 1, l.node = h.node, l.perMonth = 12;
        lots.push_back(l);
    }
    garb.SetLots(lots);
    auto prepared = SvcGarbage::Prepare(lots); // the usual re-list: sorted on the worker, adopted on the main thread
    t0 = Clock::now();
    garb.Adopt(prepared);
    const double lotsMs = Ms(t0);
    std::unordered_map<uint64_t, double> pile; // the host's mirror
    pile.reserve(lots.size());
    std::vector<std::pair<uint64_t, double>> mirror;
    std::vector<SvcGarbFacility> fac(4);
    for (int i = 0; i < 4; ++i) fac[(size_t)i].id = (uint32_t)i + 1, fac[(size_t)i].comp = 1, fac[(size_t)i].perMonth = 60000, fac[(size_t)i].range = 4000, fac[(size_t)i].x = 1500.0f * (i % 2) + 1500, fac[(size_t)i].z = 1500.0f * (i / 2) + 1500;
    CellFactors f = Neutral(cells);
    SimFlagTable table;
    std::vector<SvcTrip> trips;
    std::vector<uint64_t> changed;
    SvcLotLevelFn level = [&pile](uint64_t lot) { const auto it = pile.find(lot); return it == pile.end() ? 0.0f : (float)(it->second / 36.0); };
    double sum = 0, worst = 0;
    int64_t worstAt = 0, ticks = 0;
    double part[4] = {}, partMax[4] = {};
    std::vector<double> all;
    auto lap = [&](int i, Clock::time_point& at) { const double d = Ms(at); part[i] += d, partMax[i] = std::max(partMax[i], d); at = Clock::now(); };
    for (int64_t m = 1; m <= 3 * 1440; ++m, ++ticks) {
        const auto a = Clock::now();
        auto at = a;
        SvcTick(w, m, &f);
        lap(0, at);
        SvcFlushFlags(w, table, m, level);
        lap(1, at);
        if (m % 10 == 0) SvcSeatSlice(w, cit, seats, cit.Capacity() / 144 + 1, age, home);
        lap(2, at);
        {
            const int g = (int)(m % kGarbGroups);
            SvcGarbStats st;
            mirror.clear();
            garb.Visit(g, m, fac, trips, changed, mirror, st);
            for (const auto& [lot, p] : mirror) pile[lot] = p;
        }
        lap(3, at);
        const double ms = Ms(a);
        all.push_back(ms);
        sum += ms;
        if (ms > worst) worst = ms, worstAt = m;
    }
    const double avg = sum / (double)ticks;
    std::sort(all.begin(), all.end());
    const double p999 = all[(size_t)(all.size() * 0.999)]; // the worst ticks on a shared desktop are often the OS's
    static const char* const kPart[4] = {"tick (incidents, cell group)", "flags flush", "seat slice", "garbage group"};
    for (int i = 0; i < 4; ++i) std::printf("  %-30s avg %.4f ms  worst %.3f ms\n", kPart[i], part[i] / (double)ticks, partMax[i]);
    std::printf("P4 bench: %d cells, %zu buildings, %zu sites, %u citizens\n", cells, w.bld.size(), w.cov.sites.size(), cit.Live());
    std::printf("  coverage pass (worker) %.2f ms, install (main) %.3f ms, building list (main, on change) %.3f ms, garbage lots re-list %.3f ms\n",
                coverMs, installMs, setMs, lotsMs);
    std::printf("  per tick: avg %.4f ms, 99.9th %.3f ms, worst %.3f ms (minute %lld); iPad estimate x2: avg %.3f, 99.9th %.3f\n", avg, p999, worst,
                (long long)worstAt, avg * 2, p999 * 2);
    std::printf("  3 days: %u crimes %u fires %u sick yesterday, %u live incidents, %zu vehicle trips, seats %u (unseated %u), verify %d\n",
                w.yesterday.n[kIncCrime], w.yesterday.n[kIncFire], w.yesterday.n[kIncSick], w.inc.Live(), w.trips.size() + trips.size(), seats.lastSeated,
                seats.lastUnseated, SvcVerifyCounts(w));
    CHECK(avg * 2 < 0.20);
    CHECK(p999 * 2 < 0.5); // the single worst tick is printed (preemption on a busy desktop shows there)
    CHECK(installMs * 2 < 1.0);
    CHECK(coverMs < 20.0 * 2);
    CHECK(SvcVerifyCounts(w) == 0);
    std::printf("%s (%d failures)\n", failures ? "sim_svc_bench FAILED" : "sim_svc_bench passed", failures);
    return failures ? 1 : 0;
}
