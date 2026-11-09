// The city sim's cost bench (CS2 sim roadmap Phase 0): a synthetic city of N lots on a road grid runs one game day (1440
// ticks) through the real scheduler with the systems SimInit registers (cells, demand, growth, economy, services) plus the
// Phase 0 hub's own work (the building registry sync and the .tbdsim image the autosave copies), Phase 1's citizen records
// (bootstrapped from the lots, CitySim::externalPeople; the "life" systems and their save image), and the flow solve the
// game runs on a worker. Prints ms per system: the total over the day, per tick, and per frame at 30 fps and speed 3
// (18 game minutes a real second). The roadmap's iPad gate is these numbers x 2 until calibrated on the device.
// Build and run: tests\sim_bench.bat [lots]   (default 5000)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "city/sim.h"
#include "city/sim_budget.h"
#include "city/sim_coverage.h"
#include "city/sim_flags.h"
#include "city/sim_flow.h"
#include "city/sim_growth.h"
#include "city/sim_save.h"
#include "city/sim_life_cells.h"
#include "city/sim_life_save.h"

using namespace tbd::city;

static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Bench {
    Network net;
    ZoneGrid zones;
    CitySim cells;
    SimGrowth growth;
    SimTuning tune;
    SimEconomy econ;
    SimBudget budget;
    BuildingRegistry reg;
    std::vector<uint8_t> lotUtil;
    std::vector<SimFlags> flags;
    std::vector<int> segComp;
    FlowSolver solver;
    LifeSim life;           // Phase 1: the citizen records (CitySim::externalPeople)
    CellFactors factors;
    uint64_t lifeReg = ~0ull; // the registry version the records last synced
    std::map<std::string, double> ms; // per system, over the day
    std::map<std::string, int> calls;
};

// Phase 0's hub: the building ids from the lots.
static void HubSync(Bench& b) {
    b.reg.BeginSync();
    for (const Lot& l : b.zones.lots) {
        BuildingRow r;
        const CellKey k = CellKeyAt(l.c.x, l.c.z);
        r.kind = l.type, r.cellX = k.x, r.cellZ = k.z, r.level = (uint8_t)l.level;
        b.reg.Upsert(BOrigin::Lot, l.id, r);
    }
    b.reg.EndSync();
}
// Phase 1 (city_sim_p1.inl): the rows' homes, jobs, cells and hoods (a hood per supercell here), then the records.
static void LifeSync(Bench& b) {
    LifeFillRows(b.reg, b.cells, b.zones.lots, [&b](int c) { return b.cells.SuperOf(c); });
    b.life.SyncBuildings(b.reg, [](const BuildingRow& r) { return LifeJobKind(r.kind); });
    b.lifeReg = b.reg.version;
}

// A B x B grid of 120 m blocks and lots along its blocks: types cycle residential (4 densities), commercial, office, industrial.
static void Build(Bench& b, int lots) {
    const int prof = ProfileIndex("Two-Lane Road");
    const int B = std::max(2, (int)std::ceil(std::sqrt(lots / 24.0)));
    std::vector<int> id((size_t)(B + 1) * (B + 1));
    for (int j = 0; j <= B; ++j)
        for (int i = 0; i <= B; ++i) id[(size_t)j * (B + 1) + i] = b.net.AddNode({i * 120.0f, 0.0f, j * 120.0f}, true);
    auto seg = [&](int a, int c) {
        const V2 pa{b.net.nodes[a].p.x, b.net.nodes[a].p.z}, pc{b.net.nodes[c].p.x, b.net.nodes[c].p.z};
        b.net.AddSeg(a, c, pa + (pc - pa) * (1.0f / 3.0f), pa + (pc - pa) * (2.0f / 3.0f), prof);
    };
    for (int j = 0; j <= B; ++j)
        for (int i = 0; i < B; ++i) seg(id[(size_t)j * (B + 1) + i], id[(size_t)j * (B + 1) + i + 1]), seg(id[(size_t)i * (B + 1) + j], id[(size_t)(i + 1) * (B + 1) + j]);
    ++b.net.version;
    static const int kType[10] = {0, 1, 2, 3, 0, 1, 4, 4, 5, 6};
    for (int n = 0; n < lots; ++n) {
        const int block = n / 24, k = n % 24, bx = block % B, bz = (block / B) % B;
        Lot l;
        l.id = (uint64_t)n + 1;
        l.c = {bx * 120.0f + 12.0f + (k % 6) * 18.0f, bz * 120.0f + 12.0f + (k / 6) * 26.0f};
        l.type = kType[n % 10], l.w = 1 + n % 2, l.d = 2, l.level = 1, l.path = "m.glb", l.progress = 2.0f;
        b.zones.lots.push_back(l);
    }
    ++b.zones.version;
    b.lotUtil.assign(b.zones.lots.size(), kLotAll);
    RoadComponents(b.net, b.segComp);
    b.econ.Reset(b.tune);
}

// The game's flow job, run inline: commute trips between the supercells' nearest nodes.
static void Flow(Bench& b) {
    const FlowGraph g = FlowGraph::Build(b.net);
    const int nsc = b.cells.Supercells();
    std::vector<int> node((size_t)nsc, -1);
    for (int s = 0; s < nsc; ++s) {
        const std::vector<int>& list = b.cells.SuperCellList(s);
        if (list.empty()) continue;
        const CellKey k = b.cells.cells.cells[(size_t)list[0]].key;
        const V2 c{(k.x + 0.5f) * kCityCellSize, (k.z + 0.5f) * kCityCellSize};
        float best = 1e30f;
        for (size_t n = 0; n < b.net.nodes.size(); ++n) {
            const float d = Len(V2{b.net.nodes[n].p.x, b.net.nodes[n].p.z} - c);
            if (d < best) best = d, node[(size_t)s] = (int)n;
        }
    }
    std::vector<Trip> trips;
    b.cells.ForEachLink([&](int h, int j, int32_t n) {
        if (h != j && node[(size_t)h] >= 0 && node[(size_t)j] >= 0 && node[(size_t)h] != node[(size_t)j])
            trips.push_back({node[(size_t)h], node[(size_t)j], (float)n * 0.25f, 0});
    });
    if (!trips.empty()) b.solver.Solve(g, trips);
}

int main(int argc, char** argv) {
    const int lots = argc > 1 ? std::max(1, std::atoi(argv[1])) : 5000;
    static Bench b; // big: not on the stack
    Build(b, lots);
    // Phase 1: the records own the people; the city starts populated by the old-save bootstrap.
    b.cells.externalPeople = true;
    b.cells.Sync(b.zones.lots, b.zones.version);
    b.life.w.factors = &b.factors;
    b.life.Reset(1);
    const double boot0 = NowMs();
    HubSync(b);
    LifeSync(b);
    b.life.Bootstrap();
    LifeApplyCells(b.life.w, b.cells);
    std::printf("sim_bench: P1 bootstrap %.1f ms: %u citizens, %u households\n", NowMs() - boot0, b.life.w.cit.Live(), b.life.w.hh.Live());
    SimScheduler s;
    s.budgetMs = 1e9; // the bench measures, it never defers
    auto timed = [&](const char* name, std::function<void(const SimTick&)> fn) {
        s.Register(name, 1, 1, [name, fn](const SimTick& t) {
            const double t0 = NowMs();
            fn(t);
            b.ms[name] += NowMs() - t0, ++b.calls[name];
        });
    };
    // the systems of SimInit, in its order and at its periods (the periods are checked inside: one registration each)
    timed("cells", [](const SimTick&) { b.cells.Sync(b.zones.lots, b.zones.version); });
    timed("demand", [](const SimTick& t) {
        if (t.minute % 10) return;
        b.growth.UpdateDemand(b.cells, b.tune, 10.0, &b.zones);
        b.growth.ApplyGate(b.zones, b.tune);
    });
    timed("growth", [](const SimTick& t) {
        if (t.minute % 10) return;
        const int slice = (int)((t.minute / 10) % 6);
        if (slice == 0) b.growth.UpdateLand(b.cells, b.tune);
        b.growth.TickLots(b.zones, b.cells, b.tune, 60.0, slice, 6, &b.lotUtil);
        b.growth.events.clear();
    });
    timed("flow (worker)", [](const SimTick& t) { if (t.minute % 240 == 0) Flow(b); });
    timed("economy", [](const SimTick& t) {
        if (t.minute % 1440 == 1439) b.budget.Day(b.econ, b.cells, b.tune, DayUpkeep{}, (int64_t)b.cells.Population());
    });
    timed("services", [](const SimTick& t) {
        if (t.minute % 10) return;
        ApplyUtilities(b.cells, b.zones, b.lotUtil);
        ComputeSimFlags(b.zones, b.cells, b.growth, b.tune, b.lotUtil, b.flags);
        if (t.minute % 60 == 0) {
            std::vector<ServiceSource> src;
            for (int k = 0; k < kServices; ++k) {
                ServiceSource ss;
                ss.at = {240.0f * (k + 1), 240.0f}, ss.kind = k, ss.radius = 600.0f, ss.comp = b.segComp.empty() ? -1 : b.segComp[0];
                src.push_back(ss);
            }
            const float funding[kServices] = {1, 1, 1, 1, 1, 1};
            ComputeCoverage(b.cells, b.zones, b.segComp, src, funding);
        }
    });
    // Phase 0's hub: the building ids (the game syncs on change, at most once a second: here every 10 minutes) ...
    timed("hub: buildings", [](const SimTick& t) {
        if (t.minute % 10) return;
        if (t.minute % 60 == 0) ++b.zones.lots[(size_t)(t.minute / 60) % b.zones.lots.size()].level; // some change
        HubSync(b);
    });
    // Phase 1: the records' building sync after a lot changed (the hourly level change here; the game: on a zones change, at
    // most once a real second), the "life" system (monthly slices, moves, job matching every 10 minutes), the views into
    // the cells every 10 minutes and the factors every 30.
    timed("life: buildings", [](const SimTick& t) { if (t.minute % 60 == 1) LifeSync(b); });
    timed("life", [](const SimTick& t) {
        b.life.Tick(t.minute);
        if (t.minute % 10 == 5) LifeApplyCells(b.life.w, b.cells);
        if (t.minute % 30 == 17) b.life.WriteFactors(b.factors);
    });
    // ... and the .tbdsim image (HEAD + BLDG + the P1 records) the autosave copies on the main thread, once a game hour here.
    size_t imageBytes = 0;
    timed("hub: .tbdsim copy", [&imageBytes](const SimTick& t) {
        if (t.minute % 60) return;
        SimSaveRegistry chunks;
        chunks.Register({simchunk::HEAD, 1, 0, [](ByteWriter& w) { WriteHead(w, SimSaveHead{}); }, nullptr, nullptr});
        chunks.Register({simchunk::BLDG, 1, 0, [](ByteWriter& w) { WriteBuildings(w, b.reg); }, nullptr, nullptr});
        chunks.Register({simchunk::CITZ, 1, 0, [](ByteWriter& w) { // Phase 1: the four chunks as one here (the game writes four)
            LifeChunks c;
            LifeSave(b.life, c);
            for (const std::vector<uint8_t>* v : {&c.citz, &c.hhld, &c.occu, &c.migr}) w.Raw(v->data(), v->size());
        }, nullptr, nullptr});
        const std::vector<SimSaveRegistry::ChunkImage> snap = chunks.Snapshot(); // the main thread's part
        const double a0 = NowMs();
        imageBytes = SimSaveRegistry::Assemble(snap).size(); // the worker's part (framing, CRCs): not in the total
        const double assemble = NowMs() - a0;
        b.ms["hub: .tbdsim (worker)"] += assemble, ++b.calls["hub: .tbdsim (worker)"], b.ms["hub: .tbdsim copy"] -= assemble;
    });
    const double t0 = NowMs();
    for (int m = 0; m < 1440; ++m) s.Frame(1.0);
    const double total = NowMs() - t0;
    std::printf("sim_bench: %d lots, %llu residents, %llu jobs, %d supercells, %zu segments; one game day = %lld ticks in %.1f ms\n", lots,
                (unsigned long long)b.cells.Population(), (unsigned long long)b.cells.JobCap(), b.cells.Supercells(), b.net.segs.size(),
                (long long)s.Minute(), total);
    std::printf("  %-20s %10s %8s %12s %14s\n", "system", "day ms", "calls", "ms/tick", "ms/frame@x3");
    double sum = 0;
    for (const auto& [name, v] : b.ms) {
        const bool worker = name.find("worker") != std::string::npos;
        if (!worker) sum += v;
        // speed 3: 18 game minutes a real second = 0.6 ticks a frame at 30 fps
        std::printf("  %-20s %10.2f %8d %12.4f %14.4f\n", name.c_str(), v, b.calls[name], v / 1440.0, v / 1440.0 * 0.6);
    }
    std::printf("  %-20s %10.2f %8s %12.4f %14.4f   (main thread; x2 = the iPad gate: %.4f ms/frame)\n", "TOTAL", sum, "", sum / 1440.0, sum / 1440.0 * 0.6,
                sum / 1440.0 * 0.6 * 2.0);
    std::printf("  .tbdsim image (HEAD + BLDG + CITZ HHLD OCCU MIGR): %zu bytes\n", imageBytes);
    return 0;
}