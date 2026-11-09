// The city sim's cost bench (CS2 sim roadmap Phase 0): a synthetic city of N lots on a road grid runs one game day (1440
// ticks) through the real scheduler with the systems SimInit registers (cells, demand, growth, economy, services) plus the
// Phase 0 hub's own work (the building registry sync and the .tbdsim image the autosave copies), Phase 1's citizen records
// (bootstrapped from the lots, CitySim::externalPeople; the "life" systems and their save image), and the flow solve the
// game runs on a worker; Phase 3's environment (the weather every 10 minutes, the hourly snapshot and apply on the main
// thread, the pass itself as a worker line) and Phase 6's goods (the companies every game minute, the hourly site refresh,
// the day's truck list); and the hub's merge of the shared flag table into the per-lot flags. Prints ms per system: the
// total over the day, per tick, the worst single call, and per frame at 30 fps and speed 3 (18 game minutes a real second),
// then the worst whole tick (main thread). The roadmap's iPad gate is these numbers x 2 until calibrated on the device.
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
#include "city/sim_env_save.h"
#include "city/sim_goods_save.h"

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
    std::map<std::string, double> worst; // per system, the most one call took
    double tickMain = 0;                 // this tick's main-thread ms (systems without "worker" in their name)
    std::map<std::string, double> copyBy; // the autosave copy per chunk, over the day
    double tickSave = 0; // ... of which the autosave copy (the game copies every 10 real minutes, not every hour)
    double carve = 0;                    // ms of the current call that ran as a worker's part (not main thread)
    // Phase 3
    EnvSim env;
    EnvParams envP;
    EnvInput envBase;  // the sources, pumps and cells (the game rebuilds it every hour; here a copy of it is the snapshot)
    EnvResult envOut;  // the pass that lands at the next hour
    bool envPending = false;
    WeatherChain wx;
    EnvWeather weather;
    SimCalendar cal;
    SimFlagTable flagTable; // the hub's shared table (P3 and P6 bits)
    SimEventRing ring;
    // Phase 6
    GoodsWorld goods;
    std::vector<GoodsSite> sites;
    std::vector<BuildingId> lotIds; // the hub's merge cache
    std::vector<BuildingId> hubHint; // the registry sync's per-lot hints
    LifeApplyState applyState;       // P1's link rebuild memory (city_sim_p1.inl)
};

// Phase 0's hub: the building ids from the lots.
static void HubSync(Bench& b) {
    b.reg.BeginSync();
    if (b.hubHint.size() != b.zones.lots.size()) b.hubHint.resize(b.zones.lots.size());
    for (size_t i = 0; i < b.zones.lots.size(); ++i) { // the game's per-lot hints (city_sim_save.inl SimBuildingsSync)
        const Lot& l = b.zones.lots[i];
        BuildingRow r;
        const CellKey k = CellKeyAt(l.c.x, l.c.z);
        r.kind = l.type, r.cellX = k.x, r.cellZ = k.z, r.level = (uint8_t)l.level;
        b.reg.Upsert(BOrigin::Lot, l.id, r, b.hubHint[i]);
    }
    b.reg.EndSync();
}
// Phase 1 (city_sim_p1.inl): the rows' homes, jobs, cells and hoods (a hood per supercell here), then the records.
static void LifeSync(Bench& b) {
    LifeFillRows(b.reg, b.cells, b.zones.lots, [&b](int c) { return b.cells.SuperOf(c); }, &b.hubHint);
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

// Phase 3 (city_sim_p3.inl SimP3BuildInput): industry and commerce lots, one road source per 32 m tile of every segment
// (a fixed load here; the game takes the flow), a power plant, pumps, the CitySim cells and their heights (flat, gently
// sloped). The input is built once; each hour copies it (the snapshot the game hands the worker).
static void BuildEnv(Bench& b) {
    EnvInput& in = b.envBase;
    const auto& types = ZoneTypes();
    for (const Lot& l : b.zones.lots) {
        const ZoneCat cat = types[(size_t)l.type].cat;
        if (cat == ZoneCat::Industrial) in.sources.push_back(EnvIndustryLotSource(l.c.x, l.c.z, l.level, l.w * l.d));
        else if (cat == ZoneCat::Commercial) in.sources.push_back(EnvCommercialLotSource(l.c.x, l.c.z, l.w * l.d));
    }
    int si = 0;
    for (const Segment& sg : b.net.segs) {
        const V2 a = XZ(b.net.nodes[(size_t)sg.a].p), c = XZ(b.net.nodes[(size_t)sg.b].p);
        const int n = std::max(1, (int)(Len(c - a) / kFieldTile));
        for (int i = 0; i < n; ++i) {
            const V2 p = a + (c - a) * ((i + 0.5f) / n);
            in.sources.push_back(EnvRoadSource(p.x, p.z, 300.0f + 40.0f * (float)(si++ % 30), 0.8f));
        }
    }
    in.sources.push_back(EnvUtilitySource(60.0f, 60.0f, 4));
    for (int i = 0; i < 4; ++i) in.pumps.push_back({200.0f + 300.0f * i, 400.0f, 4000.0f, true});
    for (const CityCell& cell : b.cells.cells.cells) {
        in.cityKeys.push_back(cell.key), in.cityWaterOk.push_back(1);
        auto& t = in.heights.Cell(cell.key);
        for (int i = 0; i < kFieldRes * kFieldRes; ++i) t[(size_t)i] = 10.0f + 0.002f * (float)((cell.key.x * 4 + i % 4) * (cell.key.z * 4 + i / 4) % 997);
    }
}

// Phase 6 (city_sim_p6.inl BuildSites): a company per industrial, commercial and office lot (BuildingId order), workers
// from the cells, five raw hubs, one outside connection at node 0; the entrance is the block corner nearest the lot.
static void BuildGoods(Bench& b) {
    const auto& types = ZoneTypes();
    const int B1 = (int)std::lround(std::sqrt((double)b.net.nodes.size())); // nodes per side
    b.sites.clear();
    int64_t residents = (int64_t)b.cells.Population();
    for (size_t li = 0; li < b.zones.lots.size(); ++li) {
        const Lot& l = b.zones.lots[li];
        const ZoneCat cat = types[(size_t)l.type].cat;
        GoodsSite s;
        int kind;
        if (cat == ZoneCat::Industrial) s.kind = CompanyKind::Processor, s.recipe = (uint8_t)RecipeForLot(l.id), kind = kJobIndustrial;
        else if (cat == ZoneCat::Commercial) s.kind = CompanyKind::Shop, kind = kJobCommercial;
        else if (cat == ZoneCat::Office) s.kind = CompanyKind::Office, kind = kJobOffice;
        else continue;
        s.building = (li < b.hubHint.size() && b.reg.Get(b.hubHint[li]) ? b.hubHint[li] : b.reg.Find(BOrigin::Lot, l.id)).v; // the game: sim_save::hintLot
        s.jobs = LotCapacity(l).jobs[kind];
        s.x = (int32_t)l.c.x, s.z = (int32_t)l.c.z;
        s.node = std::clamp((int)std::lround(l.c.z / 120.0f), 0, B1 - 1) * B1 + std::clamp((int)std::lround(l.c.x / 120.0f), 0, B1 - 1);
        s.cell = b.cells.cells.Find(CellKeyAt(l.c.x, l.c.z));
        if (s.cell >= 0) {
            const CellSim& cs = b.cells.sim[(size_t)s.cell];
            s.workers = cs.jobCap[kind] ? (uint16_t)((uint32_t)s.jobs * cs.employed[kind] / cs.jobCap[kind]) : 0;
            s.localResidents = (int32_t)cs.Population();
        }
        b.sites.push_back(s);
    }
    const Good raws[5] = {Good::Grain, Good::Livestock, Good::Timber, Good::Oil, Good::Ore};
    for (int i = 0; i < 5; ++i) {
        GoodsSite s;
        s.building = Handle::Make(0xF00000u + (uint32_t)i, 1).v, s.kind = CompanyKind::Raw, s.raw = raws[i];
        s.rawMilliPerHour = 60 * 10 * 1000 / 24, s.x = 120 * i, s.z = 0, s.node = i;
        b.sites.push_back(s);
    }
    b.goods.residents = residents;
    b.goods.outsides.assign(1, {-2000, 0, 0});
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
    const double setup0 = NowMs();
    BuildEnv(b);
    BuildGoods(b);
    b.goods.SetSites(b.sites, 0);
    std::printf("sim_bench: P3 %zu sources, %zu cells; P6 %zu sites; set up in %.1f ms\n", b.envBase.sources.size(), b.envBase.cityKeys.size(), b.sites.size(),
                NowMs() - setup0);
    SimScheduler s;
    s.budgetMs = 1e9; // the bench measures, it never defers
    auto timed = [&](const char* name, std::function<void(const SimTick&)> fn) {
        const bool worker = std::string(name).find("worker") != std::string::npos;
        s.Register(name, 1, 1, [name, fn, worker](const SimTick& t) {
            const double t0 = NowMs();
            fn(t);
            const double d = NowMs() - t0 - b.carve; // carve: a worker's part measured inside the call
            b.carve = 0;
            b.ms[name] += d, ++b.calls[name], b.worst[name] = std::max(b.worst[name], d);
            if (!worker) b.tickMain += d;
            if (std::string(name) == "hub: .tbdsim copy") b.tickSave += d;
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
    // The game runs the building sync in three steps on three frames (city_sim_p1.inl SyncStep): the rows, the records'
    // buildings, the hoods' search order; here on three ticks.
    timed("life: buildings", [](const SimTick& t) {
        const int64_t m = t.minute % 60;
        if (m == 1) LifeFillRows(b.reg, b.cells, b.zones.lots, [](int c) { return b.cells.SuperOf(c); }, &b.hubHint);
        if (m == 2) LifeSyncBuildings(b.life.w, b.reg, [](const BuildingRow& r) { return LifeJobKind(r.kind); }), b.lifeReg = b.reg.version;
        if (m == 3) LifeComputeNear(b.life.w);
    });
    timed("life", [](const SimTick& t) {
        b.life.Tick(t.minute);
        if (t.minute % 10 == 5) LifeApplyCells(b.life.w, b.cells, &b.applyState);
        if (t.minute % 30 == 17) b.life.WriteFactors(b.factors);
    });
    // Phase 3: the weather every 10 minutes; every hour the previous pass lands (factors, flags, events, the overlay index)
    // and the snapshot for the next one is taken (main thread); the pass itself is the worker's, and so are the copy of the
    // applied state it steps (city_sim_p3.inl Job::base), the road tiles and freeing the replaced state.
    timed("env: weather", [](const SimTick& t) {
        if (t.minute % 10) return;
        b.wx = WeatherChain::Replay(SimCalendar::FloorDiv(t.minute, kWeatherStepMinutes), 7, b.cal, &b.wx);
        b.weather = WeatherAt(b.wx, t.minute, 7, b.cal);
    });
    static EnvInput envJob;
    static EnvSim envState;
    timed("env: hourly apply", [](const SimTick& t) {
        if (t.minute % 60) return;
        if (b.envPending) {
            std::swap(b.env, envState);
            const double f0 = NowMs();
            envState = EnvSim(); // the game frees the old state on a worker
            b.carve += NowMs() - f0;
            WriteEnvFactors(b.envOut, b.factors);
            WriteEnvFlags(b.envOut, b.reg, b.flagTable);
            PushEnvEvents(b.envOut, b.ring);
            std::unordered_map<uint64_t, int> index;
            for (size_t i = 0; i < b.envOut.keys.size(); ++i) index[PackKey(b.envOut.keys[i])] = (int)i;
        }
        envJob = b.envBase, envJob.minute = t.minute, envJob.weather = b.weather;
        if (t.minute > 0) envJob.heights.Clear(); // only the first pass brings the heights (the game: new cells only)
    });
    timed("env: pass (worker)", [](const SimTick& t) {
        if (t.minute % 60) return;
        envState = b.env;
        b.envOut = EnvResult();
        envState.Step(envJob, b.envP, b.envOut);
        b.envPending = true;
    });
    // Phase 6: the companies every game minute (5-minute boundaries inside), the sites every hour, the trucks once a day
    timed("goods", [](const SimTick& t) { b.goods.Tick(t.minute); });
    timed("goods: sites", [](const SimTick& t) {
        if (t.minute % 60 != 30) return;
        BuildGoods(b);
        b.goods.SetSites(b.sites, t.minute);
    });
    timed("goods: trucks", [](const SimTick& t) { if (t.minute % 1440 == 2) (void)b.goods.TrucksLast(); });
    // the hub: the shared flag table's P3 and P6 bits onto the per-lot flags, with every services pass (city_sim_phases.inl)
    timed("hub: flag merge", [](const SimTick& t) {
        if (t.minute % 10) return;
        if (b.lotIds.size() != b.zones.lots.size()) {
            b.lotIds.assign(b.zones.lots.size(), BuildingId());
            for (size_t i = 0; i < b.zones.lots.size(); ++i) b.lotIds[i] = b.reg.Find(BOrigin::Lot, b.zones.lots[i].id);
        }
        const SimFlags mask = SimFlagPhaseMask(3) | SimFlagPhaseMask(6);
        for (size_t i = 0; i < b.flags.size() && i < b.lotIds.size(); ++i) b.flags[i] = (b.flags[i] & ~mask) | (b.flagTable.Get(b.lotIds[i]) & mask);
    });
    // ... and the .tbdsim image (HEAD + BLDG + the P1, P3, P6 chunks) the autosave copies, once a game hour here. The game
    // makes these chunks at once on the workers while the main thread waits (city_sim_save_io.inl SimTbdSnapshot, one job
    // per group: POLL with WATR, every other chunk alone), so the main thread pays the longest group: that is what the
    // copy line counts; the rest of the sequential time is booked as the worker's.
    size_t imageBytes = 0;
    timed("hub: .tbdsim copy", [&imageBytes](const SimTick& t) {
        if (t.minute % 60) return;
        SimSaveRegistry chunks;
        chunks.Register({simchunk::HEAD, 1, 0, [](ByteWriter& w) { WriteHead(w, SimSaveHead{}); }, nullptr, nullptr});
        chunks.Register({simchunk::BLDG, 1, 0, [](ByteWriter& w) { WriteBuildings(w, b.reg); }, nullptr, nullptr});
        // Phase 1 as the game writes it (city_sim_p1.inl): each chunk makes its own part
        chunks.Register({simchunk::CITZ, 1, 0, [](ByteWriter& w) { LifeSavePart(b.life, 0, w.bytes); }, nullptr, nullptr});
        chunks.Register({simchunk::HHLD, 1, 0, [](ByteWriter& w) { LifeSavePart(b.life, 1, w.bytes); }, nullptr, nullptr});
        chunks.Register({simchunk::OCCU, 1, 0, [](ByteWriter& w) { LifeSavePart(b.life, 2, w.bytes); }, nullptr, nullptr});
        chunks.Register({simchunk::MIGR, 1, 0, [](ByteWriter& w) { LifeSavePart(b.life, 3, w.bytes); }, nullptr, nullptr});
        // Phase 3 and Phase 6 (city_sim_p3.inl, city_sim_p6.inl)
        chunks.Register({simchunk::POLL, 1, 0, [](ByteWriter& w) { EnvSavePoll(w, EnvSaveView{&b.env, &b.envOut, nullptr, nullptr}); }, nullptr, nullptr});
        chunks.Register({simchunk::WTHR, 1, 0, [](ByteWriter& w) { EnvWeatherState st; st.chain = b.wx; EnvSaveWeather(w, st); }, nullptr, nullptr});
        chunks.Register({simchunk::WATR, 1, 0, [](ByteWriter& w) { EnvSaveWater(w, EnvSaveView{&b.env, &b.envOut, nullptr, nullptr}); }, nullptr, nullptr});
        chunks.Register({simchunk::COMP, 1, 0, [](ByteWriter& w) { WriteGoodsComp(w, b.goods); }, nullptr, nullptr});
        chunks.Register({simchunk::GOOD, 1, 0, [](ByteWriter& w) { WriteGoodsGood(w, b.goods); }, nullptr, nullptr});
        chunks.Register({simchunk::TRAD, 1, 0, [](ByteWriter& w) { WriteGoodsTrad(w, b.goods); }, nullptr, nullptr});
        std::vector<SimSaveRegistry::ChunkImage> snap; // timed per chunk
        double seq = 0, pollWatr = 0, longest = 0;
        for (const SimChunkDef& d : chunks.Defs()) {
            const double c0 = NowMs();
            ByteWriter pw;
            d.save(pw);
            snap.push_back({d.fourcc, d.version, d.flags, std::move(pw.bytes)});
            const double ms = NowMs() - c0;
            b.copyBy[FourCCName(d.fourcc)] += ms, seq += ms;
            if (d.fourcc == simchunk::POLL || d.fourcc == simchunk::WATR) pollWatr += ms;
            else longest = std::max(longest, ms);
        }
        longest = std::max(longest, pollWatr);
        const double a0 = NowMs();
        imageBytes = SimSaveRegistry::Assemble(snap).size(); // the worker's part (framing, CRCs): not in the total
        const double assemble = NowMs() - a0;
        b.ms["hub: .tbdsim (worker)"] += assemble + seq - longest, ++b.calls["hub: .tbdsim (worker)"], b.carve = assemble + seq - longest;
        b.worst["hub: .tbdsim (worker)"] = std::max(b.worst["hub: .tbdsim (worker)"], assemble);
    });
    const double t0 = NowMs();
    double worstTick = 0;
    int64_t worstMinute = -1, worstNoSaveMinute = -1;
    double worstNoSave = 0;
    for (int m = 0; m < 1440; ++m) {
        b.tickMain = b.tickSave = 0;
        s.Frame(1.0);
        if (b.tickMain > worstTick) worstTick = b.tickMain, worstMinute = m;
        if (b.tickMain - b.tickSave > worstNoSave) worstNoSave = b.tickMain - b.tickSave, worstNoSaveMinute = m;
    }
    const double total = NowMs() - t0;
    std::printf("sim_bench: %d lots, %llu residents, %llu jobs, %d supercells, %zu segments; one game day = %lld ticks in %.1f ms\n", lots,
                (unsigned long long)b.cells.Population(), (unsigned long long)b.cells.JobCap(), b.cells.Supercells(), b.net.segs.size(),
                (long long)s.Minute(), total);
    std::printf("  %-22s %10s %8s %12s %10s %14s\n", "system", "day ms", "calls", "ms/tick", "worst ms", "ms/frame@x3");
    double sum = 0;
    for (const auto& [name, v] : b.ms) {
        const bool worker = name.find("worker") != std::string::npos;
        if (!worker) sum += v;
        // speed 3: 18 game minutes a real second = 0.6 ticks a frame at 30 fps
        std::printf("  %-22s %10.2f %8d %12.4f %10.3f %14.4f\n", name.c_str(), v, b.calls[name], v / 1440.0, b.worst[name], v / 1440.0 * 0.6);
    }
    std::printf("  %-22s %10.2f %8s %12.4f %10s %14.4f   (main thread; x2 = the iPad gate: %.4f ms/frame)\n", "TOTAL", sum, "", sum / 1440.0, "", sum / 1440.0 * 0.6,
                sum / 1440.0 * 0.6 * 2.0);
    std::printf("  .tbdsim image (HEAD BLDG, P1 CITZ HHLD OCCU MIGR, P3 POLL WTHR WATR, P6 COMP GOOD TRAD): %zu bytes\n", imageBytes);
    std::printf("  autosave copy per save (main thread, ms):");
    for (const auto& [k, v] : b.copyBy) std::printf(" %s %.3f", k.c_str(), v / 24.0);
    std::printf("\n");
    std::printf("  worst tick (main thread): %.3f ms at minute %lld (x2 iPad: %.3f ms); P6 %u companies, %zu trucks on the road, %zu truck pairs a day\n",
                worstTick, (long long)worstMinute, worstTick * 2.0, b.goods.cos.pool.Live(), b.goods.ships.size(), b.goods.TrucksLast().size());
    std::printf("  worst tick without the autosave copy: %.3f ms at minute %lld (x2 iPad: %.3f ms)\n", worstNoSave, (long long)worstNoSaveMinute, worstNoSave * 2.0);
    return 0;
}