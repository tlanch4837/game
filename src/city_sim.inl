// ------------------------------------------------------------------- city simulation host (src/city/sim.h)
// Included by main.cpp after the other city .inl files. SimSync runs once per frame after GarbageSync and is the only
// place that advances the new sim systems (Track S). It does not own the game clock: ZonesSync still advances
// g_clock.minutes, and SimSync feeds the scheduler the change in g_clock.minutes since the last frame, so the sim minute
// and the HUD clock cannot drift apart. A jump that is not the normal per-frame advance (a save loaded, a new map) rebases
// the scheduler instead of queueing thousands of minutes.
//
// Environment (dev):
//   TBD_SIM_BUDGET_MS=<ms>        per-frame sim budget (default 2)
//   TBD_SIM_SPEED=<0|1|2>         start the game clock at that speed (2 = >>>, 18 game minutes per real second)
//   TBD_SIM_SCHED_STRESS=<ms>     registers a dummy system that busy-waits that long every game minute (budget check)
//   TBD_SIM_LOG=1                 logs the scheduler's numbers every 5 real seconds

tbd::city::SimScheduler g_sim;
tbd::city::CitySim g_citySim; // per-cell counters (sim_cells.h)
tbd::city::SimGrowth g_simGrowth; // demand, growth gate, levels (sim_growth.h)
tbd::city::SimTuning g_simTune;   // compiled defaults, then assets\sim\tuning.json

void SimLoadTuning() {
    g_simTune = tbd::city::SimTuning();
    const std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\sim\\tuning.json";
    std::vector<std::string> unknown;
    int applied = 0;
    if (g_simTune.LoadFile(path.c_str(), &unknown, &applied)) {
        tbd::Log("Sim: tuning.json applied %d values", applied);
        for (const std::string& u : unknown) tbd::Log("Sim: tuning.json: unknown name \"%s\" (ignored)", u.c_str());
    } else {
        tbd::Log("Sim: no assets\\sim\\tuning.json; compiled-in tuning");
    }
}
struct SimTotals {
    uint64_t population = 0, adults = 0, working = 0, jobs = 0, homes = 0;
} g_simTotals; // refreshed by the cells system when anything changed

namespace sim_host {
inline double EnvDouble(const char* name, double fallback) {
    char v[32] = {};
    return tbd::platform::GetEnvA(name, v, sizeof(v)) > 0 ? atof(v) : fallback;
}
} // namespace sim_host

std::vector<uint8_t> g_simLotUtil; // per lot (zones.lots order): LotUtil flags from the last utilities solve
std::vector<uint32_t> g_simFlags;  // per lot (zones.lots order): SimFlag bits (sim_flags.h, uint32), for the icons, overlays and info card
uint64_t g_simFlagsVersion = 0;    // bumped when g_simFlags changed
float g_simFunding[tbd::city::kServices] = {1, 1, 1, 1, 1, 1}; // S4's budget sliders scale the service radii
uint64_t g_simCoverageRuns = 0;

// The service kind of a transport-tab building, -1 for none (deathcare and communications cover nothing yet).
int SimServiceKind(const tbd::city::BuildingDef& d) {
    using tbd::city::TTab;
    switch (d.tab) {
    case TTab::Fire: return tbd::city::kFire;
    case TTab::Police: return tbd::city::kPolice;
    case TTab::Health: return tbd::city::kHealth;
    case TTab::Education: return tbd::city::kSchool;
    case TTab::Bus: case TTab::Rail: return tbd::city::kTransit;
    default: return -1;
    }
}

void SimServices() {
    using namespace tbd::city;
    // utilities: the solve's per-lot result (lots under construction count as fine there)
    static uint64_t utilSolves = ~0ull, utilZones = ~0ull;
    if (utilSolves != g_utilSolve.solves || utilZones != g_zones.version) {
        utilSolves = g_utilSolve.solves, utilZones = g_zones.version;
        g_simLotUtil.assign(g_zones.lots.size(), kLotAll);
        if (g_utilReport.lots.size() == g_zones.lots.size())
            for (size_t i = 0; i < g_zones.lots.size(); ++i) {
                const UtilStatus& u = g_utilReport.lots[i];
                g_simLotUtil[i] = (uint8_t)((u.elec ? kLotPower : 0) | (u.water ? kLotWater : 0) | (u.sewage ? kLotSewage : 0) | (u.road ? kLotRoad : 0));
            }
        ApplyUtilities(g_citySim, g_zones, g_simLotUtil);
    }
    { // the per-building flags: O(lots), every 10 game minutes (abandonment creeps up with time, so no cheaper key)
        static std::vector<tbd::city::SimFlags> fresh;
        ComputeSimFlags(g_zones, g_citySim, g_simGrowth, g_simTune, g_simLotUtil, fresh);
        if (fresh != g_simFlags) g_simFlags.swap(fresh), ++g_simFlagsVersion;
    }
    // coverage: only when the services, the roads, the lots or the funding changed
    static uint64_t kTrans = ~0ull, kNet = ~0ull, kZones = ~0ull;
    static float kFund[kServices] = {};
    static std::vector<int> segComp;
    static uint64_t compFor = ~0ull;
    int done = 0;
    for (const Placed& b : g_transport.items) done += b.alive && b.state == BState::Done;
    const uint64_t transKey = g_transport.version * 1000003ull + (uint64_t)done;
    bool fundChanged = false;
    for (int k = 0; k < kServices; ++k) fundChanged |= kFund[k] != g_simFunding[k];
    if (transKey == kTrans && kNet == g_roads.net.version && kZones == g_zones.version && !fundChanged) return;
    kTrans = transKey, kNet = g_roads.net.version, kZones = g_zones.version;
    for (int k = 0; k < kServices; ++k) kFund[k] = g_simFunding[k];
    if (compFor != g_roads.net.version) compFor = g_roads.net.version, RoadComponents(g_roads.net, segComp);
    std::vector<ServiceSource> src;
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        if (kind < 0) continue;
        ServiceSource s;
        s.at = b.c;
        s.kind = kind;
        s.radius = (float)std::min(g_simTune.serviceRadiusMax, g_simTune.serviceRadiusBase + g_simTune.serviceRadiusPerVehicle * d.vehicles);
        float t, dd;
        V2 mid[4], out[4]; // the gate: the front edge's road, else whichever edge of a turned lot has one (placement_rotation.h)
        EdgeMids(b.c, b.front, d.W(), d.D(), mid, out);
        int seg = -1;
        for (int e = 0; e < 4 && seg < 0; ++e) seg = g_roads.net.SegmentAt(mid[e] + out[e] * 2.0f, 14.0f, t, dd);
        s.comp = seg >= 0 && seg < (int)segComp.size() ? segComp[(size_t)seg] : -1;
        src.push_back(s);
    }
    ComputeCoverage(g_citySim, g_zones, segComp, src, g_simFunding);
    ++g_simCoverageRuns;
}

tbd::city::SimEconomy g_simEcon; // treasury and tax rates (sim_economy.h)
tbd::city::SimBudget g_simBudget; // itemised expenses, loans, balance history, bankruptcy (sim_budget.h)
// The sim minute a fresh city (start, new map) began at: its first ledger runs a whole game day later, not on the tick
// it was made (the scheduler's minute 0 runs every system, so the first ledger used to charge day 1 at once).
int64_t g_simLedgerFrom = 0;
tbd::city::SimMilestones g_simMilestones; // population milestones and what they unlock (sim_milestones.h)
city::NewsFeed g_simNews;          // news_lines.json; sim events post triggers, the radio reads Queue()
std::string g_simBanner, g_simBannerSub; // the last milestone, for the HUD banner
uint64_t g_simBannerAt = 0;
double g_simMilestoneQuietUntil = 0; // game minutes: a load re-reaches its milestones silently, without rewards
std::vector<std::string> g_simNotes; // the Notifications panel's log, newest last (capped)

// One line in the Notifications log, stamped with the HUD's calendar date.
void SimNote(const std::string& text) {
    static const char* const kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int day0 = std::max(0, g_clock.Day() - 1);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%s %d  ", kMonth[(day0 / 30) % 12], day0 % 30 + 1);
    g_simNotes.push_back(stamp + text);
    if (g_simNotes.size() > 40) g_simNotes.erase(g_simNotes.begin());
}

// Lane-metres of every live road and track (rail counts as two lanes, a path as one), recounted when the network changes.
double SimRoadLaneMetres() {
    static uint64_t lenFor = ~0ull;
    static double sum = 0;
    if (lenFor == g_roads.net.version) return sum;
    lenFor = g_roads.net.version, sum = 0;
    for (size_t i = 0; i < g_roads.net.segs.size(); ++i) {
        if (!g_roads.net.segs[i].alive) continue;
        const tbd::city::Profile& p = g_roads.net.Prof((int)i);
        const int lanes = p.cat == tbd::city::Cat::Rail ? 2 : p.fwd + p.back;
        sum += tbd::city::RoadLaneMetres(g_roads.net.Curve((int)i).Length(8), lanes);
    }
    return sum;
}

// A sim event for the radio anchor: event driven only, one headline per event, nothing on a timer.
void RadioSimEvent(const char* trigger); // city_radio.inl
void SimNews(const char* trigger) {
    RadioSimEvent(trigger);
    std::string text, tone;
    if (g_simNews.Post(trigger, text, tone)) tbd::Log("News [%s]: %s", trigger, text.c_str());
}

// The day's upkeep split for the ledger (cents per day).
tbd::city::DayUpkeep SimDayUpkeep() {
    using namespace tbd::city;
    DayUpkeep up;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive) up.utilities += UpkeepPerDay(UtilDefs()[b.def].upkeep, 1.0, 1.0);
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        up.services += UpkeepPerDay(d.upkeep, kind >= 0 ? g_simFunding[kind] : 1.0, kind >= 0 ? g_simTune.serviceUpkeepScale : 1.0);
    }
    up.roads = RoadUpkeepPerDay(PaidLaneMetres(SimRoadLaneMetres(), g_cityMapLaneMetres), kRoadUpkeepPerLaneKmDay); // not the map's corridor
    return up;
}

// ---- construction charges: what was placed since the last frame is paid for once, at its catalog cost (roads by the
// lane-metres added). A new map or a load is taken as it stands (free), like the radio's baseline, for 2 real seconds.
void SimBudgetEvents(); // below
namespace sim_build {
std::unordered_set<uint64_t> transport, util;
double laneMetres = -1.0;
uint64_t roadVersion = ~0ull, generation = ~0ull;
double settle = 0.0; // real seconds left of the baseline window
int64_t chargedTotal = 0; // cents, since the session started (dev log)
} // namespace sim_build
void SimConstructionSync(float realDt) {
    using namespace tbd::city;
    namespace sb = sim_build;
    if (sb::generation != g_cityGeneration) sb::generation = g_cityGeneration, sb::settle = 2.0, sb::laneMetres = -1.0;
    const bool settling = sb::settle > 0.0;
    if (settling) sb::settle -= realDt;
    if (settling) sb::transport.clear(), sb::util.clear();
    int64_t cost = 0;
    for (const Placed& b : g_transport.items)
        if (b.alive && sb::transport.insert(b.id).second) cost += (int64_t)Defs()[b.def].cost * 100;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive && sb::util.insert(b.id).second) cost += (int64_t)UtilDefs()[b.def].cost * 100;
    if (sb::roadVersion != g_roads.net.version || sb::laneMetres < 0.0) {
        const double was = sb::laneMetres, now = SimRoadLaneMetres();
        sb::roadVersion = g_roads.net.version, sb::laneMetres = now;
        if (was >= 0.0 && now > was + 0.5) cost += RoadCost(now - was); // shortened or bulldozed roads: no refund
        if (now < g_cityMapLaneMetres) g_cityMapLaneMetres = now; // the corridor was bulldozed: what is rebuilt is the city's
    }
    // demolished ones drop out of the sets once in a while (they stay the size of the city)
    if (sb::transport.size() > g_transport.items.size() + 256) {
        sb::transport.clear();
        for (const Placed& b : g_transport.items) if (b.alive) sb::transport.insert(b.id);
    }
    if (sb::util.size() > g_util.items.size() + 256) {
        sb::util.clear();
        for (const UtilBuilding& b : g_util.items) if (b.alive) sb::util.insert(b.id);
    }
    if (settling || cost <= 0) return;
    g_simBudget.Charge(g_simEcon, cost);
    sb::chargedTotal += cost;
    SimBudgetEvents(); // the bankruptcy warning, if this build emptied the treasury
}

// The budget's and the milestones' events into toasts and the news. The bankrupt story waits out the grace days
// (sim_budget.h BudgetNewsAllowed): a first day's spending spree is toasted, but the news only runs it if the city is
// still in the red once a ledger day has gone by.
bool g_simBankruptNewsPending = false;
void SimBudgetEvents() {
    const bool newsOk = tbd::city::BudgetNewsAllowed(g_simEcon, (int)g_simTune.budgetNewsGraceDays);
    if (g_simBankruptNewsPending && newsOk) {
        g_simBankruptNewsPending = false;
        if (g_simBudget.bankrupt) SimNews("bankrupt");
    }
    for (const std::string& e : g_simBudget.events) {
        if (e == "bankrupt") {
            Toast("THE CITY IS BANKRUPT: RAISE TAXES, CUT COSTS OR TAKE A LOAN"), SimNote("Bankrupt: the treasury is below zero");
            if (newsOk) SimNews("bankrupt");
            else g_simBankruptNewsPending = true;
        }
        else if (e == "solvent") Toast("THE TREASURY IS BACK IN THE BLACK"), SimNote("The treasury is back above zero");
        else if (e == "loan_taken") SimNews("loan_taken"), SimNote("Loan taken");
        else if (e == "loan_repaid") SimNote("Loan repaid early");
    }
    g_simBudget.events.clear();
}
void SimMilestonesSync() {
    using namespace tbd::city;
    const bool quiet = g_clock.minutes < g_simMilestoneQuietUntil;
    const int got = g_simMilestones.Update(g_simTotals.population, quiet ? nullptr : &g_simEcon.treasury);
    if (got <= 0 || quiet) return;
    int n = 0;
    const Milestone* m = Milestones(n);
    const Milestone& last = m[g_simMilestones.reached - 1];
    char b[128];
    std::snprintf(b, sizeof(b), "MILESTONE: %s (%u RESIDENTS)  +%lld", last.name, last.population, (long long)(last.reward / 100));
    std::string up = b;
    for (char& c : up) c = (char)std::toupper((unsigned char)c);
    Toast(up);
    g_simBanner = std::string("Milestone reached: ") + last.name;
    g_simBannerSub = std::to_string(last.population) + " residents.  +" + std::to_string((long long)(last.reward / 100)) + " grant.  New tools unlocked.";
    g_simBannerAt = tbd::platform::TickCountMs();
    tbd::Log("Sim: milestone %d %s reached at population %llu", g_simMilestones.reached, last.name, (unsigned long long)g_simTotals.population);
    SimNote(std::string("Milestone: ") + last.name + " (" + std::to_string(last.population) + " residents)");
    SimNews("milestone_reached");
}

// Upkeep of everything the city runs, cents per day: utility sources, and transport / service buildings (services scaled
// by their funding).
int64_t SimUpkeepPerDay() {
    using namespace tbd::city;
    int64_t sum = 0;
    for (const UtilBuilding& b : g_util.items)
        if (b.alive) sum += UpkeepPerDay(UtilDefs()[b.def].upkeep, 1.0, 1.0);
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        sum += UpkeepPerDay(d.upkeep, kind >= 0 ? g_simFunding[kind] : 1.0, kind >= 0 ? g_simTune.serviceUpkeepScale : 1.0);
    }
    return sum;
}

// ---- S5: flow traffic. The graph snapshot and the trips go to a background worker; the result swaps in at a tick.
struct SimFlowJob {
    std::shared_ptr<const tbd::city::FlowGraph> graph;
    std::vector<tbd::city::Trip> trips;
    tbd::city::FlowSolver solver; // only the worker touches it while busy (warm start: the last flows)
    std::vector<tbd::city::SegFlow> result;
    size_t segCount = 0;
    std::atomic<bool> busy{false}, ready{false};
};
std::shared_ptr<SimFlowJob> g_simFlowJob = std::make_shared<SimFlowJob>();
std::vector<tbd::city::SegFlow> g_segFlow; // per road segment, the last finished solve (empty before the first)
uint64_t g_segFlowVersion = 0;               // network version g_segFlow belongs to
tbd::city::SegGrid g_simSegGrid;
double g_simFlowSolveMs = 0;
int64_t g_simFlowLastMinute = -1000000;

// Road edits: the cells the changed segments cross are dirty (render and flow).
void SimDirtyRoadCells() {
    static std::vector<uint8_t> alive;
    const auto& net = g_roads.net;
    std::vector<tbd::city::CellKey> keys;
    for (size_t s = 0; s < net.segs.size(); ++s) {
        const uint8_t now = net.segs[s].alive ? 1 : 0;
        const uint8_t was = s < alive.size() ? alive[s] : 0;
        if (now == was) continue;
        tbd::city::SegGrid::CellsOf(net, (int)s, keys);
        for (const tbd::city::CellKey& k : keys) {
            const int c = g_citySim.cells.Find(k);
            if (c >= 0) g_citySim.cells.Touch(c, tbd::city::kDirtyRender | tbd::city::kDirtyFlow);
        }
    }
    alive.resize(net.segs.size());
    for (size_t s = 0; s < net.segs.size(); ++s) alive[s] = net.segs[s].alive ? 1 : 0;
}

void SimAppendTownTrips(const tbd::city::FlowGraph& g, std::vector<tbd::city::Trip>& trips); // city_herds.inl (town traffic)

// The trips of the peak hour: commutes from the supercell links, shopping from homes to commercial (gravity).
void SimBuildTrips(const tbd::city::FlowGraph& g, std::vector<tbd::city::Trip>& trips) {
    using namespace tbd::city;
    trips.clear();
    const int nsc = g_citySim.Supercells();
    // a supercell's road node: the network node nearest its people and jobs
    std::vector<int> node((size_t)nsc, -1);
    std::vector<V2> centre((size_t)nsc);
    std::vector<double> pop((size_t)nsc, 0), comm((size_t)nsc, 0);
    for (int s = 0; s < nsc; ++s) {
        double w = 0, x = 0, z = 0;
        for (int c : g_citySim.SuperCellList(s)) {
            const CellSim& cs = g_citySim.sim[(size_t)c];
            const double cw = (double)cs.Population() + cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
            const CellKey k = g_citySim.cells.cells[(size_t)c].key;
            x += cw * (k.x + 0.5) * kCityCellSize, z += cw * (k.z + 0.5) * kCityCellSize, w += cw;
            pop[(size_t)s] += cs.Population();
            comm[(size_t)s] += cs.jobCap[kJobCommercial];
        }
        if (w <= 0) continue;
        centre[(size_t)s] = {(float)(x / w), (float)(z / w)};
        float t, d;
        const int seg = g_simSegGrid.Nearest(g_roads.net, centre[(size_t)s], 800.0f, t, d);
        if (seg < 0) continue;
        const Segment& sg = g_roads.net.segs[(size_t)seg];
        node[(size_t)s] = t < 0.5f ? sg.a : sg.b;
        if (node[(size_t)s] >= g.nodes) node[(size_t)s] = -1;
    }
    const float commute = (float)(g_simTune.flowCarShare * g_simTune.flowPeakShare);
    g_citySim.ForEachLink([&](int h, int j, int32_t n) {
        if (h == j || node[(size_t)h] < 0 || node[(size_t)j] < 0 || node[(size_t)h] == node[(size_t)j]) return;
        trips.push_back({node[(size_t)h], node[(size_t)j], (float)n * commute, (int)(SimSeed(7, (uint64_t)h * 131 + (uint64_t)j) % 3)});
    });
    for (int h = 0; h < nsc; ++h) {
        if (node[(size_t)h] < 0 || pop[(size_t)h] <= 0) continue;
        double wsum = 0;
        std::vector<std::pair<int, double>> w;
        for (int c = 0; c < nsc; ++c) {
            if (c == h || node[(size_t)c] < 0 || comm[(size_t)c] <= 0) continue;
            const double d = Len(centre[(size_t)c] - centre[(size_t)h]);
            if (d > 3.0 * g_simTune.flowShopDecayMetres) continue;
            const double v = comm[(size_t)c] * std::exp(-d / std::max(1.0, g_simTune.flowShopDecayMetres));
            w.push_back({c, v});
            wsum += v;
        }
        const double total = pop[(size_t)h] * g_simTune.flowShopPerResident;
        for (const auto& [c, v] : w)
            if (node[(size_t)c] != node[(size_t)h]) trips.push_back({node[(size_t)h], node[(size_t)c], (float)(total * v / wsum), 2});
    }
    SimAppendTownTrips(g, trips); // trips inside one hood and from the outside connections (city_herds.inl, sim_town_traffic.h)
}

void SimFlow(const tbd::city::SimTick& t) {
    using namespace tbd::city;
    SimFlowJob& job = *g_simFlowJob;
    if (job.ready.load(std::memory_order_acquire)) { // a finished solve: swap it in at this tick
        g_segFlow = job.result;
        g_segFlowVersion = job.graph ? job.graph->version : 0;
        g_simFlowSolveMs = job.solver.lastMs;
        job.ready.store(false, std::memory_order_release);
    }
    if (g_simSegGrid.builtFrom != g_roads.net.version) {
        g_simSegGrid.Build(g_roads.net);
        SimDirtyRoadCells();
    }
    if (job.busy.load(std::memory_order_acquire)) return;
    static uint64_t kNet = ~0ull, kZones = ~0ull;
    const bool changed = kNet != g_roads.net.version || kZones != g_zones.version;
    if (!changed && t.minute - g_simFlowLastMinute < (int64_t)g_simTune.flowSolveMinutes) return;
    kNet = g_roads.net.version, kZones = g_zones.version;
    g_simFlowLastMinute = t.minute;
    if (!job.graph || job.graph->version != g_roads.net.version) job.graph = std::make_shared<const FlowGraph>(FlowGraph::Build(g_roads.net));
    SimBuildTrips(*job.graph, job.trips);
    if (job.trips.empty()) {
        g_segFlow.assign(g_roads.net.segs.size(), SegFlow{});
        g_segFlowVersion = g_roads.net.version;
        return;
    }
    job.segCount = g_roads.net.segs.size();
    job.busy.store(true, std::memory_order_release);
    std::shared_ptr<SimFlowJob> keep = g_simFlowJob; // the worker holds it even if the city is reset meanwhile
    g_engine.jobs->RunBackground([keep] {
        keep->solver.Solve(*keep->graph, keep->trips);
        keep->solver.Segments(*keep->graph, keep->segCount, keep->result);
        keep->ready.store(true, std::memory_order_release);
        keep->busy.store(false, std::memory_order_release);
    });
}

// ---- S7: cosmetic traffic. Eight car models (assets/vehicles/cars), uploaded once when there is traffic to show; every
// frame the cars near the camera come from the segment flows (sim_agents.h) and each model is one instanced draw.
struct SimCarModel {
    tbd::MeshId mesh = tbd::kInvalidMesh;
};
std::vector<SimCarModel> g_simCars;
bool g_simCarsTried = false;
double g_simAgentTime = 0; // seconds the cosmetic cars have driven (real time while the game runs)
tbd::city::AgentParams g_simAgentParams;
uint32_t g_simCarsDrawn = 0;
bool g_herdsOwnCars = false; // city_herds.inl: the herd children draw the cars and the flow-driven cosmetic ones step aside

void SimAgentsSync(float realDt) {
    if (!g_clock.paused) g_simAgentTime += realDt * (1.0 + 0.5 * g_clock.speed);
    if (g_simCarsTried || g_segFlow.empty() || !g_engine.renderer) return;
    g_simCarsTried = true; // outside the frame: the uploads may wait
    static const char* const kModels[] = {"CAR_sedan_white", "CAR_hatch_red", "CAR_suv_black", "CAR_wagon_silver",
                                          "CAR_coupe_blue", "CAR_pickup_grey", "CAR_van_white", "CAR_compact_green"};
    for (const char* m : kModels) {
        tbd::Entity e;
        e.mesh = tbd::MeshKind::Imported;
        e.meshPath = UtilAbsolute((std::string("assets/vehicles/cars/glb/") + m + ".glb").c_str()) + "#native";
        e.color = {1, 1, 1};
        if (ImportedVertices(e.meshPath).empty()) continue;
        SimCarModel c;
        c.mesh = UploadEntityMesh(e);
        if (c.mesh != tbd::kInvalidMesh) g_simCars.push_back(c);
    }
    g_simAgentParams.models = (int)g_simCars.size();
    tbd::Log("Sim: %zu car models for cosmetic traffic", g_simCars.size());
}

// Dev: TBD_SIM_OVERLAY=1 draws every road segment's solved flow over it: green free, yellow busy, orange congested, red
// jammed (line width by volume), one transient draw.
void SimFlowOverlay(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    static const bool on = sim_host::EnvDouble("TBD_SIM_OVERLAY", 0.0) > 0.0;
    if (!on || g_segFlow.size() != g_roads.net.segs.size()) return;
    static std::vector<tbd::Vertex> v;
    v.clear();
    static const tbd::Vec3 kCol[4] = {{0.2f, 0.85f, 0.3f}, {0.95f, 0.85f, 0.2f}, {0.98f, 0.5f, 0.1f}, {0.9f, 0.15f, 0.1f}};
    for (size_t s = 0; s < g_segFlow.size(); ++s) {
        const tbd::city::SegFlow& f = g_segFlow[s];
        if (f.flow <= 1.0f || !g_roads.net.segs[s].alive) continue;
        const tbd::city::Cubic cu = g_roads.net.Curve((int)s);
        const float w = std::clamp(f.flow / 400.0f, 0.6f, 6.0f);
        const tbd::Vec3 col = kCol[f.cls];
        tbd::city::V2 prev = cu.At(0);
        for (int i = 1; i <= 8; ++i) {
            const tbd::city::V2 q = cu.At(i / 8.0f);
            const tbd::city::V2 n = tbd::city::RightOf(tbd::city::Norm(q - prev)) * w;
            const float y0 = g_roads.net.YAt(g_roads.net.segs[s], (i - 1) / 8.0f) + 1.5f, y1 = g_roads.net.YAt(g_roads.net.segs[s], i / 8.0f) + 1.5f;
            zfx::Quad(v, {prev.x - n.x, y0, prev.z - n.z}, {prev.x + n.x, y0, prev.z + n.z}, {q.x + n.x, y1, q.z + n.z}, {q.x - n.x, y1, q.z - n.z}, {0, 0, 0}, col);
            prev = q;
        }
    }
    if (!v.empty()) r.DrawTransient(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
}

// Near cars cast shadows (Track R9): the ones within kSimCarShadowRange of the eye, drawn into the sun's map by SimAgentsCasters. The
// shadow pass runs before the scene, so it draws the rows the previous frame gathered here: one frame behind, a quarter metre at
// city speeds, under the shadow map's own softness.
constexpr float kSimCarShadowRange = 150.0f;
std::vector<std::vector<float>> g_simCarShadowRows; // per car model
void SimAgentsCasters(tbd::Renderer& r) {
    for (size_t m = 0; m < g_simCarShadowRows.size() && m < g_simCars.size(); ++m) {
        const std::vector<float>& rows = g_simCarShadowRows[m];
        if (rows.empty()) continue;
        const uint32_t n = (uint32_t)(rows.size() / 12);
        const tbd::MeshId mesh = g_simCars[m].mesh;
        const auto it = g_engine.matParts.find(mesh);
        if (it == g_engine.matParts.end()) { r.DrawShadowInstancedTransient(mesh, rows.data(), n); continue; }
        for (const MatPart& p : it->second)
            if (p.lodError == 0.0f && p.caster) r.DrawShadowInstancedTransient(p.mesh, rows.data(), n);
    }
}

void SimAgentsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    SimFlowOverlay(r, viewProj);
    g_simCarsDrawn = 0;
    g_simCarShadowRows.resize(g_simCars.size());
    for (auto& v : g_simCarShadowRows) v.clear();
    if (g_herdsOwnCars) return;
    if (g_simCars.empty() || g_segFlow.size() != g_roads.net.segs.size() || g_segFlowVersion != g_roads.net.version) return;
    if (g_engine.camera.distance > 1200.0f) return; // past this the cars are specks: none drawn
    static std::vector<tbd::city::CarInstance> cars;
    const tbd::city::V2 focus{g_engine.camera.target.x, g_engine.camera.target.z};
    g_simAgentParams.radius = std::clamp(g_engine.camera.distance * 1.2f, 150.0f, 700.0f);
    tbd::city::CosmeticCars(g_roads.net, g_simSegGrid, g_segFlow, focus, g_simAgentTime, g_simAgentParams, cars);
    static std::vector<float> rows;
    for (int m = 0; m < (int)g_simCars.size(); ++m) {
        rows.clear();
        for (const tbd::city::CarInstance& c : cars) {
            if (c.model != m) continue;
            float row[12];
            tbd::city::CarRow(c, row);
            rows.insert(rows.end(), row, row + 12);
            const tbd::Vec3 eye = g_engine.camera.Eye(), at{row[3], row[7], row[11]};
            if (tbd::Dot(at - eye, at - eye) < kSimCarShadowRange * kSimCarShadowRange) g_simCarShadowRows[(size_t)m].insert(g_simCarShadowRows[(size_t)m].end(), row, row + 12);
        }
        if (rows.empty()) continue;
        const uint32_t n = (uint32_t)(rows.size() / 12);
        const tbd::MeshId mesh = g_simCars[(size_t)m].mesh;
        const auto it = g_engine.matParts.find(mesh);
        if (it == g_engine.matParts.end()) {
            r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            r.DrawInstancedTransient(mesh, rows.data(), n, viewProj);
        } else {
            for (const MatPart& p : it->second) {
                if (p.lodError > 0.0f) continue; // the full level only
                r.SetMaterial(p.albedo, p.normal, p.surface);
                r.DrawInstancedTransient(p.mesh, rows.data(), n, viewProj);
            }
        }
        g_simCarsDrawn += n;
    }
    r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
}

// ---- S6: tracked people. Selecting a zoned building materializes (up to) its first four residents; the HUD lists them.
tbd::city::SimPeople g_simPeople;
std::vector<std::string> g_simCitizenLines;
std::string g_simCitizenTitle;

// The zoned building the player clicked (city_buildings.inl CityPickLot: buildings are not scene entities since Track R); 0 = none.
// The serial changes with every pick, so clicking the same building again opens a panel the player closed.
uint64_t g_pickedLot = 0;
uint32_t g_pickedLotSerial = 0;
void SimShowLot(uint64_t lot);

void SimPeopleSelect() {
    static int lastSel = -2;
    static uint32_t lastPick = 0;
    if (g_pickedLotSerial != lastPick) {
        lastPick = g_pickedLotSerial;
        if (g_pickedLot) SimShowLot(g_pickedLot);
        return;
    }
    const int sel = g_engine.scene.selected;
    if (sel == lastSel) return;
    lastSel = sel;
    if (sel < 0 || sel >= (int)g_engine.scene.entities.size()) return;
    const std::string& name = g_engine.scene.entities[(size_t)sel].name;
    if (name.compare(0, 15, "Zoned building ") != 0) return; // TBD_ZONE_ENTITIES=1: the old entities
    SimShowLot(std::strtoull(name.c_str() + 15, nullptr, 10));
}

void SimShowLot(uint64_t lot) {
    int cell = -1;
    uint32_t first = 0, count = 0;
    g_simCitizenLines.clear();
    g_simCitizenTitle = "Lot " + std::to_string(lot);
    for (size_t i = 0; i < g_zones.lots.size() && i < g_simFlags.size(); ++i) // its problems first (sim_flags.h)
        if (g_zones.lots[i].id == lot && g_simFlags[i]) {
            std::string line = "Problems:";
            for (int b = 0; b < tbd::city::kFlagCount; ++b)
                if (g_simFlags[i] & (1u << b)) line += std::string(line.back() == ':' ? " " : ", ") + tbd::city::SimFlagText(b);
            g_simCitizenLines.push_back(line);
        }
    if (!tbd::city::SimPeople::LotSlots(g_citySim, lot, cell, first, count) || count == 0) {
        g_simCitizenLines.push_back("Nobody lives here (a workplace, or still being built).");
        return;
    }
    static const char* const kBand[] = {"child", "adult", "senior"};
    static const char* const kEdu[] = {"basic", "skilled", "higher"};
    static const char* const kWealth[] = {"low", "modest", "comfortable", "rich"};
    static const char* const kJob[] = {"industry", "a shop", "an office"};
    g_simCitizenTitle += ": " + std::to_string(count) + " residents";
    for (uint32_t k = 0; k < std::min<uint32_t>(count, 4); ++k) {
        const int i = g_simPeople.Materialize(g_citySim, cell, first + k, g_sim.Minute());
        if (i < 0) continue;
        const tbd::city::Person& p = g_simPeople.people[(size_t)i];
        char line[160];
        snprintf(line, sizeof(line), "%s, %d, %s, %s education, %s income, %s", tbd::city::SimPeople::Name(p).c_str(), p.age, kBand[p.band], kEdu[p.edu],
                 kWealth[p.wealth], p.Working() ? (std::string("works in ") + kJob[p.jobKind]).c_str() : p.band == tbd::city::kAdults ? "looking for work" : "not working");
        g_simCitizenLines.push_back(line);
    }
}

// ---- S8: the SIM records of a game save (city_game.inl writes the file; these are the sim's lines in it).
constexpr int kSimSaveVersion = 1;
struct SimPendingLoad {
    bool econ = false;
    int64_t treasury = 0;
    int tax[4] = {100, 100, 100, 100};
    float funding[6] = {1, 1, 1, 1, 1, 1};
    int64_t minute = 0;
    double demand[4] = {};
    uint64_t ups = 0, downs = 0, abandoned = 0;
    std::string budget; // SimBudget::SaveFields, "" in older saves
    std::vector<std::vector<std::string>> people;
    bool lotsBack = false; // the grown lots are on their cells again: the people can follow
} g_simPending;

void SimSaveLines(std::ostringstream& o) {
    o << "M|" << kSimSaveVersion << '|' << g_simEcon.treasury << '|';
    for (int c = 0; c < 4; ++c) o << (c ? " " : "") << g_simEcon.taxRate10[c];
    o << '|';
    for (int k = 0; k < tbd::city::kServices; ++k) o << (k ? " " : "") << g_simFunding[k];
    o << '|' << g_sim.Minute() << '|';
    for (int c = 0; c < 4; ++c) o << (c ? " " : "") << g_simGrowth.demand[c];
    o << '|' << g_simGrowth.levelUps << ' ' << g_simGrowth.levelDowns << ' ' << g_simGrowth.abandoned;
    o << '|' << g_simBudget.SaveFields(); // field 8: loans, the warning, the history (older builds ignore it)
    o << '|' << g_cityMapLaneMetres << '\n'; // field 9: the map corridor's lane-metres, free of upkeep (older builds ignore it)
    for (const tbd::city::Person& p : g_simPeople.people) {
        tbd::city::CellKey jk{0, 0};
        if (p.jobCell >= 0 && p.jobCell < (int)g_citySim.cells.cells.size()) jk = g_citySim.cells.cells[(size_t)p.jobCell].key;
        o << "Q|" << p.id << '|' << p.seed << '|' << p.homeLot << '|' << (int)p.band << '|' << (int)p.age << '|' << (int)p.edu << '|' << (int)p.wealth << '|'
          << (int)p.health << '|' << (int)p.happiness << '|' << (p.Working() ? 1 : 0) << '|' << jk.x << ' ' << jk.z << '|' << (int)p.jobKind << '|' << p.touched << '\n';
    }
}

void SimLoadLine(const std::vector<std::string>& f) {
    if (f.empty()) return;
    if (f[0] == "M" && f.size() >= 8) {
        if (atoi(f[1].c_str()) > kSimSaveVersion) {
            tbd::Log("Sim: save has sim records version %s (this build reads %d): sim state starts fresh", f[1].c_str(), kSimSaveVersion);
            return;
        }
        SimPendingLoad& s = g_simPending;
        s.econ = true;
        s.treasury = strtoll(f[2].c_str(), nullptr, 10);
        sscanf(f[3].c_str(), "%d %d %d %d", &s.tax[0], &s.tax[1], &s.tax[2], &s.tax[3]);
        sscanf(f[4].c_str(), "%f %f %f %f %f %f", &s.funding[0], &s.funding[1], &s.funding[2], &s.funding[3], &s.funding[4], &s.funding[5]);
        s.minute = strtoll(f[5].c_str(), nullptr, 10);
        sscanf(f[6].c_str(), "%lf %lf %lf %lf", &s.demand[0], &s.demand[1], &s.demand[2], &s.demand[3]);
        sscanf(f[7].c_str(), "%llu %llu %llu", (unsigned long long*)&s.ups, (unsigned long long*)&s.downs, (unsigned long long*)&s.abandoned);
        s.budget = f.size() >= 9 ? f[8] : std::string();
        if (f.size() >= 10 && !f[9].empty()) g_cityMapLaneMetres = std::max(0.0, atof(f[9].c_str())), g_cityMapLaneKnown = true; // else LoadCityGame counts it
    } else if (f[0] == "Q" && f.size() >= 14) {
        g_simPending.people.push_back(f);
    }
}

void SimLotsRestored() { g_simPending.lotsBack = true; }
extern bool g_simHudPush; // below, with the HUD glue

// After the reset for a load: the sim's own state back; the people once their homes are back.
void SimApplyPending() {
    SimPendingLoad& s = g_simPending;
    if (s.econ) {
        s.econ = false;
        g_simEcon.treasury = s.treasury;
        for (int c = 0; c < 4; ++c) g_simEcon.taxRate10[c] = std::clamp(s.tax[c], 0, 300), g_simGrowth.demand[c] = s.demand[c];
        for (int k = 0; k < tbd::city::kServices; ++k) g_simFunding[k] = std::clamp(s.funding[k], 0.5f, 1.5f);
        g_sim.Reset(s.minute);
        g_simGrowth.levelUps = s.ups, g_simGrowth.levelDowns = s.downs, g_simGrowth.abandoned = s.abandoned;
        if (!s.budget.empty() && !g_simBudget.LoadFields(s.budget)) tbd::Log("Sim: the save's budget field is unreadable: no loans restored");
        s.budget.clear();
        g_simBudget.events.clear();
        g_simLedgerFrom = -(1LL << 60); // a loaded city's days go on: its next ledger runs at the next day boundary
        g_simEcon.days = std::max<int64_t>(g_simEcon.days, (int64_t)g_simTune.budgetNewsGraceDays); // and it may be talked about
        g_simHudPush = true;
    }
    if (!s.lotsBack) return;
    s.lotsBack = false;
    // the counters at once (the lots just came back; a paused game would otherwise show nobody until it runs), then
    // each person on top of them without changing a total
    if (g_citySim.Sync(g_zones.lots, g_zones.version)) {
        g_simTotals.population = g_citySim.Population(), g_simTotals.adults = g_citySim.Adults();
        g_simTotals.working = g_citySim.Working(), g_simTotals.jobs = g_citySim.JobCap(), g_simTotals.homes = g_citySim.HomeCap();
    }
    if (s.people.empty()) return;
    int back = 0;
    for (const auto& f : s.people) {
        tbd::city::Person p;
        p.id = (uint32_t)strtoul(f[1].c_str(), nullptr, 10);
        p.seed = strtoull(f[2].c_str(), nullptr, 10);
        p.homeLot = strtoull(f[3].c_str(), nullptr, 10);
        p.band = (uint8_t)std::clamp(atoi(f[4].c_str()), 0, 2);
        p.age = (uint8_t)atoi(f[5].c_str()), p.edu = (uint8_t)atoi(f[6].c_str()), p.wealth = (uint8_t)atoi(f[7].c_str());
        p.health = (uint8_t)atoi(f[8].c_str()), p.happiness = (uint8_t)atoi(f[9].c_str());
        int jx = 0, jz = 0;
        sscanf(f[11].c_str(), "%d %d", &jx, &jz);
        const bool working = f[10] == "1";
        p.jobKind = (uint8_t)std::clamp(atoi(f[12].c_str()), 0, 2);
        p.touched = strtoll(f[13].c_str(), nullptr, 10);
        p.jobCell = working ? g_citySim.cells.Find({jx, jz}) : -1;
        back += g_simPeople.Restore(g_citySim, p) ? 1 : 0;
    }
    tbd::Log("Sim: %d of %zu tracked people restored", back, s.people.size());
    s.people.clear();
}

// A wiped city (new map, load): the sim's state starts over with it.
void SimOnCityReset() {
    g_citySim.Clear();
    g_simGrowth.Clear();
    g_simEcon.Reset(g_simTune);
    g_simBudget.Reset();
    g_simLedgerFrom = g_sim.Minute(), g_simBankruptNewsPending = false;
    g_simMilestones.Reset();
    g_simNotes.clear();
    g_simMilestoneQuietUntil = g_clock.minutes + 30.0; // a loaded city's population arrives over the next ticks
    g_simFlags.clear();
    g_simTotals = {};
    g_simLotUtil.clear();
    g_simPeople = tbd::city::SimPeople();
    g_simCitizenLines.clear();
    if (!g_simFlowJob->busy.load()) g_simFlowJob->solver = tbd::city::FlowSolver(), g_simFlowJob->graph.reset();
    else g_simFlowJob = std::make_shared<SimFlowJob>(); // the running solve finishes on its own copy and is dropped
    g_segFlow.clear();
}

void SimInit() {
    static bool done = false;
    if (done) return;
    done = true;
    g_sim.budgetMs = sim_host::EnvDouble("TBD_SIM_BUDGET_MS", 2.0);
    {
        std::FILE* f = nullptr; // the repo's copy on the PC, assets\news on the iPad (tools\ipad_sync\pack_assets.ps1)
        for (const wchar_t* rel : {L"\\..\\src\\city\\news_lines.json", L"\\..\\assets\\news\\news_lines.json"})
            if (!f) f = tbd::platform::WOpen((tbd::ExeDirectory() + rel).c_str(), L"rb");
        if (f) {
            std::string text;
            char buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
            std::fclose(f);
            tbd::Log("Sim: %d news lines", city::LoadNewsLines(g_simNews, text));
        }
        g_simMilestones.unlockAll = sim_host::EnvDouble("TBD_UNLOCK_ALL", 0.0) > 0.0;
    }
    g_util.conductRange = (float)sim_host::EnvDouble("TBD_UTIL_CONDUCT_M", 8.0); // CS1 style: power passes between touching buildings
    const double speed = sim_host::EnvDouble("TBD_SIM_SPEED", -1.0);
    if (speed >= 0.0) g_clock.speed = std::clamp((int)speed, 0, 2);
    g_clockBoost = (float)std::clamp(sim_host::EnvDouble("TBD_CLOCK_BOOST", 1.0), 0.1, 200.0);
    // S1: the lots' homes and jobs into the cells, residents moved in, jobs matched. Every game minute, but it returns at
    // once unless the zone grid changed (lot spawned, finished, swapped or demolished).
    g_sim.Register("cells", 1, 1, [](const tbd::city::SimTick&) {
        if (!g_citySim.Sync(g_zones.lots, g_zones.version)) return;
        g_simTotals.population = g_citySim.Population();
        g_simTotals.adults = g_citySim.Adults();
        g_simTotals.working = g_citySim.Working();
        g_simTotals.jobs = g_citySim.JobCap();
        g_simTotals.homes = g_citySim.HomeCap();
    });
    // S2: demand every 10 game minutes (smoothed over a day), and the spawn gate it drives.
    SimLoadTuning();
    g_sim.Register("demand", 10, 1, [](const tbd::city::SimTick&) {
        g_simGrowth.UpdateDemand(g_citySim, g_simTune, 10.0, &g_zones);
        g_simGrowth.ApplyGate(g_zones, g_simTune);
    });
    // S2: land value (once an hour), level up / down and abandonment: every lot once a game hour, in 6 slices of 10 minutes.
    g_sim.Register("growth", 60, 6, [](const tbd::city::SimTick& t) {
        if (t.slice == 0) g_simGrowth.UpdateLand(g_citySim, g_simTune);
        g_simGrowth.TickLots(g_zones, g_citySim, g_simTune, 60.0, t.slice, t.sliceCount, g_simLotUtil.size() == g_zones.lots.size() ? &g_simLotUtil : nullptr);
        if (g_simGrowth.events.empty()) return;
        const size_t n = g_simGrowth.events.size();
        for (size_t i = 0; i < std::min<size_t>(n, 3); ++i) tbd::Log("Sim: %s", g_simGrowth.events[i].c_str());
        if (n > 3) tbd::Log("Sim: ... and %zu more (ups %llu, downs %llu, abandoned %llu so far)", n - 3, (unsigned long long)g_simGrowth.levelUps,
                            (unsigned long long)g_simGrowth.levelDowns, (unsigned long long)g_simGrowth.abandoned);
        g_simGrowth.events.clear();
    });
    // S6: named people follow what the counters did (building gone, job gone), idle ones collapse; every 10 minutes.
    g_sim.Register("people", 10, 1, [](const tbd::city::SimTick& t) { g_simPeople.Sync(g_citySim, t.minute); });
    // S5: flow traffic, checked every 5 game minutes (solved on a worker when anything changed or every flowSolveMinutes).
    g_sim.Register("flow", 5, 1, [](const tbd::city::SimTick& t) { SimFlow(t); });
    // S4: the ledger, once a game day.
    g_simEcon.Reset(g_simTune);
    g_sim.Register("economy", 1440, 1, [](const tbd::city::SimTick& t) {
        if (t.minute < g_simLedgerFrom + 1440) return; // a fresh city's first (partial) day: no ledger yet
        g_simBudget.Day(g_simEcon, g_citySim, g_simTune, SimDayUpkeep(), (int64_t)g_simTotals.population); // SimEconomy::Day inside
        SimBudgetEvents();
    });
    // S3: utilities into the cells (from the cached solve) and service coverage, every 10 game minutes, only on change.
    g_sim.Register("services", 10, 1, [](const tbd::city::SimTick&) { SimServices(); });
    const double stressMs = sim_host::EnvDouble("TBD_SIM_SCHED_STRESS", 0.0);
    if (stressMs > 0.0)
        g_sim.Register("stress", 1, 1, [stressMs](const tbd::city::SimTick&) {
            const double a = g_sim.nowMs();
            while (g_sim.nowMs() - a < stressMs) {}
        });
}

void SimSync(float realDt) {
    SimInit();
    static uint64_t generation = 0;
    if (generation != g_cityGeneration) { // new map or a load wiped the city
        generation = g_cityGeneration;
        SimOnCityReset();
    }
    SimApplyPending(); // a loaded game's sim records, once the reset above is done
    SimMilestonesSync();
    SimConstructionSync(realDt); // what was placed since the last frame is paid for
    for (int c = 0; c < tbd::city::SimEconomy::kCats; ++c) g_simGrowth.offset[c] = g_simEcon.DemandOffset(c, g_simTune);
    SimAgentsSync(realDt);
    SimPeopleSelect();
    static bool seen = false;
    static double lastClock = 0.0;
    const double now = g_clock.minutes;
    double delta = seen ? now - lastClock : 0.0;
    const double expected = (double)realDt * g_clock.Rate() * g_clockBoost; // what ZonesSync added this frame
    if (seen && (delta < 0.0 || std::fabs(delta - expected) > 1.0)) { // load, new map, clock set by hand
        tbd::Log("Sim: clock jumped %.1f game minutes; scheduler rebased", delta);
        g_sim.Reset(g_sim.Minute());
        delta = 0.0;
    }
    seen = true;
    lastClock = now;
    g_sim.paused = g_clock.paused;
    g_sim.Frame(delta);

    static const bool log = sim_host::EnvDouble("TBD_SIM_LOG", 0.0) > 0.0;
    if (log) {
        static uint64_t lastLog = tbd::platform::TickCountMs();
        static double worstFrame = 0.0;
        static int frames = 0;
        static int64_t ticksAtLog = 0;
        worstFrame = std::max(worstFrame, g_sim.FrameMs());
        ++frames;
        if (tbd::platform::TickCountMs() - lastLog >= 5000) {
            tbd::Log("Sim: speed %d (%.0f min/s), %lld ticks in %d frames, avg %.3f ms, worst %.3f ms (budget %.2f), backlog %.1f, dropped %lld",
                     g_clock.speed, g_clock.Rate(), (long long)(g_sim.Minute() - ticksAtLog), frames, g_sim.AvgFrameMs(), worstFrame, g_sim.budgetMs,
                     g_sim.Backlog(), (long long)g_sim.DroppedMinutes());
            for (const tbd::city::SimSystem& s : g_sim.Systems())
                tbd::Log("Sim:   %-12s avg %.3f ms/frame, worst call %.3f ms, %llu calls", s.name.c_str(), s.avgMs, s.worstMs, (unsigned long long)s.calls);
            lastLog = tbd::platform::TickCountMs();
            worstFrame = 0.0;
            frames = 0;
            ticksAtLog = g_sim.Minute();
        }
    }
}

// The info panels' numbers (gui/city_info_panels.h): counted only while one of them is open, at most twice a second.
void SimInfoFill(tbd::gui::CityHud& hud) {
    using namespace tbd::city;
    tbd::gui::HudCityStats& st = hud.stats;
    if (!tbd::gui::IsCityInfoPanel(hud.notDoneName)) return;
    if (st.notes.size() != g_simNotes.size() || (!g_simNotes.empty() && st.notes.back() != g_simNotes.back())) st.notes = g_simNotes;
    static uint64_t last = 0;
    const uint64_t now = tbd::platform::TickCountMs();
    if (last && now - last < 500) return;
    last = now;
    st.cityName = hud.cityName;
    int nm = 0;
    const Milestone* ms = Milestones(nm);
    const int reached = std::clamp(g_simMilestones.reached, 0, nm);
    st.milestone = reached > 0 ? ms[reached - 1].name : "None yet";
    st.nextMilestone = reached < nm ? ms[reached].name : "";
    st.nextPop = reached < nm ? (long long)ms[reached].population : 0;
    st.treasury = (long long)(g_simEcon.treasury / 100), st.net = (long long)(g_simEcon.LastNet() / 100);
    st.population = st.kids = st.adults = st.seniors = st.working = st.unemployed = st.homes = st.jobs = st.jobsFilled = 0;
    for (int k = 0; k < 3; ++k) st.jobsByKind[k] = st.filledByKind[k] = st.edu[k] = 0;
    for (long long& w : st.wealth) w = 0;
    double land = 0, cov[6] = {}, power = 0, water = 0, sewage = 0;
    for (const CellSim& c : g_citySim.sim) {
        const long long p = c.Population();
        st.population += p;
        st.kids += c.residents[kKids], st.adults += c.residents[kAdults], st.seniors += c.residents[kSeniors];
        st.working += c.working;
        st.unemployed += c.residents[kAdults] > c.working ? c.residents[kAdults] - c.working : 0;
        st.homes += c.homeCap;
        for (int k = 0; k < 3; ++k) st.jobsByKind[k] += c.jobCap[k], st.filledByKind[k] += c.employed[k], st.edu[k] += c.edu[k];
        for (int w = 0; w < 4; ++w) st.wealth[w] += c.wealth[w];
        land += (double)p * c.landValue;
        for (int k = 0; k < 6; ++k) cov[k] += (double)p * c.coverage[k];
        power += c.powerOk ? (double)p : 0.0, water += c.waterOk ? (double)p : 0.0, sewage += c.sewageOk ? (double)p : 0.0;
    }
    st.jobs = st.jobsByKind[0] + st.jobsByKind[1] + st.jobsByKind[2];
    st.jobsFilled = st.filledByKind[0] + st.filledByKind[1] + st.filledByKind[2];
    const double P = std::max<double>(1.0, (double)st.population);
    st.landValue = (float)(land / P);
    for (int k = 0; k < 6; ++k) st.coverage[k] = (float)(cov[k] / P);
    st.powerShare = st.population ? (float)(power / P) : 1.0f, st.waterShare = st.population ? (float)(water / P) : 1.0f;
    st.sewageShare = st.population ? (float)(sewage / P) : 1.0f;
    for (int c = 0; c < 4; ++c) st.demand[c] = (float)g_simGrowth.demand[c];
    st.levelUps = (long long)g_simGrowth.levelUps, st.levelDowns = (long long)g_simGrowth.levelDowns, st.abandoned = (long long)g_simGrowth.abandoned;
    st.trips = st.slowRoads = 0;
    for (const SegFlow& f : g_segFlow) st.trips += f.flow > 1.0f, st.slowRoads += f.cls >= 2;
    int64_t h[SimBudget::kHistory], p[SimBudget::kHistory];
    const int n = g_simBudget.History(h, SimBudget::kHistory);
    g_simBudget.PopHistory(p, SimBudget::kHistory);
    st.balance.resize((size_t)n), st.pop.resize((size_t)n);
    for (int i = 0; i < n; ++i) st.balance[(size_t)i] = (long long)(h[i] / 100), st.pop[(size_t)i] = (long long)p[i];
}

// The HUD's population (and, from S4, money) cells.
bool g_simHudPush = true; // the sim's tax rates and funding go into the Budget panel (start, load); afterwards the panel leads
void SimHudFill(tbd::gui::CityHud& hud) {
    hud.population = (long long)g_simTotals.population;
    hud.adults = (long long)g_simTotals.adults;
    hud.working = (long long)g_simTotals.working;
    hud.jobs = (long long)g_simTotals.jobs;
    hud.money = (long long)(g_simEcon.treasury / 100);
    for (int c = 0; c < 4; ++c) hud.incomeDay[c] = (long long)(g_simEcon.lastIncome[c] / 100);
    hud.upkeepDay = (long long)(g_simEcon.lastUpkeep / 100);
    { // the milestone banner: 5 s, fading over the last one
        const double age = g_simBannerAt ? (double)(tbd::platform::TickCountMs() - g_simBannerAt) / 1000.0 : 1e9;
        hud.bannerAlpha = (float)std::clamp(5.0 - age, 0.0, 1.0);
        hud.banner = g_simBanner, hud.bannerSub = g_simBannerSub;
    }
    { // the Budget panel's ledger and loans
        using namespace tbd::city;
        tbd::gui::BudgetView& v = hud.bv;
        for (int e = 0; e < kExpenses; ++e) v.expense[e] = (long long)(g_simBudget.lastExpense[e] / 100);
        v.repayment = (long long)(g_simBudget.lastRepayment / 100);
        v.construction = (long long)(g_simBudget.lastConstruction / 100);
        v.constructionToday = (long long)(g_simBudget.constructionToday / 100);
        int64_t h[SimBudget::kHistory];
        v.historyCount = g_simBudget.History(h, SimBudget::kHistory);
        for (int i = 0; i < v.historyCount; ++i) v.history[i] = (long long)(h[i] / 100);
        v.bankrupt = g_simBudget.bankrupt;
        int nt = 0;
        const LoanTier* t = LoanTiers(nt);
        v.loanCount = std::min(nt, 3);
        for (int i = 0; i < v.loanCount; ++i) {
            auto& r = v.loans[i];
            r = {};
            r.name = t[i].name, r.amount = (long long)(t[i].amount / 100), r.ratePct = t[i].ratePermille / 10.0f, r.termDays = t[i].termDays;
            for (const Loan& l : g_simBudget.loans)
                if (l.tier == i) r.active = true, r.owed = (long long)(l.owed / 100), r.daysLeft = l.daysLeft;
        }
        // milestones: the population each locked toolbar button and transport tab needs
        using tbd::gui::HudTool;
        static const struct { HudTool tool; uint32_t bit; } kTools[] = {
            {HudTool::Terrain, kUnlockTerrain}, {HudTool::Zoning, kUnlockZoning}, {HudTool::Roads, kUnlockRoads},
            {HudTool::Electricity, kUnlockElectricity}, {HudTool::Water, kUnlockWater}, {HudTool::Garbage, kUnlockGarbage},
            {HudTool::Transport, kUnlockTransport}, {HudTool::Industry, kUnlockIndustry}, {HudTool::Fire, kUnlockFire},
            {HudTool::Health, kUnlockHealth}, {HudTool::Police, kUnlockPolice}, {HudTool::Education, kUnlockEducation}, {HudTool::Comms, kUnlockComms}};
        for (const auto& k : kTools) hud.lockPop[(int)k.tool] = g_simMilestones.Unlocked(k.bit) ? 0 : (long long)UnlockPopulation(k.bit);
        hud.milestone = g_simMilestones.Level();
        uint32_t tabs = 0;
        if (!g_simMilestones.Unlocked(kUnlockRail)) tabs |= 1u << (int)TTab::Rail;
        if (!g_simMilestones.Unlocked(kUnlockShip)) tabs |= 1u << (int)TTab::Ship;
        if (!g_simMilestones.Unlocked(kUnlockAirport)) tabs |= 1u << (int)TTab::Air;
        g_transportTool.lockedTabs = tabs;
    }
    SimInfoFill(hud);
    hud.citizenTitle = g_simCitizenTitle;
    hud.citizens = g_simCitizenLines;
    static bool openBudget = sim_host::EnvDouble("TBD_SIM_SHOW_BUDGET", 0.0) > 0.0; // dev: the Budget panel open at start
    if (openBudget && g_simTotals.population > 0) openBudget = false, hud.budget = true;
    if (g_simHudPush) {
        g_simHudPush = false;
        for (int c = 0; c < 4; ++c) hud.tax[c] = (float)g_simEcon.taxRate10[c] / 10.0f;
        for (int k = 0; k < tbd::city::kServices; ++k) hud.funding[k] = g_simFunding[k] * 100.0f;
    }
}
// After the HUD drew: the panel's sliders into the sim.
void SimHudRead(const tbd::gui::CityHud& hud) {
    for (int c = 0; c < 4; ++c) g_simEcon.taxRate10[c] = (int)std::lround(std::clamp(hud.tax[c], 0.0f, 30.0f) * 10.0f);
    for (int k = 0; k < tbd::city::kServices; ++k) g_simFunding[k] = std::round(std::clamp(hud.funding[k], 50.0f, 150.0f)) / 100.0f;
    if (hud.bv.takeLoan >= 0 && g_simBudget.TakeLoan(hud.bv.takeLoan, g_simEcon)) Toast("LOAN TAKEN"), SimBudgetEvents();
    if (hud.bv.repayLoan >= 0) {
        for (size_t i = 0; i < g_simBudget.loans.size(); ++i)
            if (g_simBudget.loans[i].tier == hud.bv.repayLoan) {
                Toast(g_simBudget.RepayLoan((int)i, g_simEcon) ? "LOAN REPAID" : "NOT ENOUGH MONEY TO REPAY THE LOAN");
                break;
            }
        SimBudgetEvents(); // the log line (loan_repaid)
    }
}

// The F3 overlay's SIM section.
void SimProfile(tbd::profiler::Sample& smp) {
    smp.simActive = true;
    smp.simMs = g_sim.FrameMs();
    smp.simAvgMs = g_sim.AvgFrameMs();
    smp.simBudgetMs = g_sim.budgetMs;
    smp.simBacklog = g_sim.Backlog();
    smp.simTicks = (uint32_t)g_sim.TicksLastFrame();
    for (const tbd::city::SimSystem& s : g_sim.Systems()) smp.sim.push_back({s.name, s.frameMs, s.avgMs});
    char b[96];
    snprintf(b, sizeof(b), "R %+.2f C %+.2f O %+.2f I %+.2f", g_simGrowth.demand[0], g_simGrowth.demand[1], g_simGrowth.demand[2], g_simGrowth.demand[3]);
    smp.simInfo.push_back({"DEMAND", b});
    snprintf(b, sizeof(b), "%llu / %llu / %llu", (unsigned long long)g_simTotals.population, (unsigned long long)g_simTotals.working, (unsigned long long)g_simTotals.jobs);
    smp.simInfo.push_back({"POP / WORK / JOBS", b});
    snprintf(b, sizeof(b), "UP %llu DOWN %llu GONE %llu", (unsigned long long)g_simGrowth.levelUps, (unsigned long long)g_simGrowth.levelDowns, (unsigned long long)g_simGrowth.abandoned);
    smp.simInfo.push_back({"LEVELS", b});
    int jammed = 0;
    float busiest = 0;
    for (const tbd::city::SegFlow& f : g_segFlow) jammed += f.cls >= 2, busiest = std::max(busiest, f.flow);
    snprintf(b, sizeof(b), "%zu TRIPS  %.1f MS  %d SLOW  TOP %.0f", g_simFlowJob->trips.size(), g_simFlowSolveMs, jammed, busiest);
    smp.simInfo.push_back({"FLOW (WORKER)", b});
    snprintf(b, sizeof(b), "%lld  NET %lld / DAY", (long long)(g_simEcon.treasury / 100), (long long)(g_simEcon.LastNet() / 100));
    smp.simInfo.push_back({"MONEY", b});
    // the cell inspector: the CityCell under the camera's focus
    const tbd::city::CellKey k = tbd::city::CellKeyAt(g_engine.camera.target.x, g_engine.camera.target.z);
    const int c = g_citySim.cells.Find(k);
    if (c >= 0) {
        const tbd::city::CellSim& cs = g_citySim.sim[(size_t)c];
        snprintf(b, sizeof(b), "%d %d  %zu LOTS  V%u/%u", k.x, k.z, g_citySim.cells.cells[(size_t)c].lots.size(), g_citySim.cells.cells[(size_t)c].simVersion,
                 g_citySim.cells.cells[(size_t)c].renderVersion);
        smp.simInfo.push_back({"CELL", b});
        snprintf(b, sizeof(b), "POP %u WORK %u JOBS %u/%u/%u FILLED %u/%u/%u", cs.Population(), cs.working, cs.jobCap[0], cs.jobCap[1], cs.jobCap[2], cs.employed[0],
                 cs.employed[1], cs.employed[2]);
        smp.simInfo.push_back({"CELL PEOPLE", b});
        snprintf(b, sizeof(b), "LAND %u  P%u W%u S%u  COV %u %u %u %u %u %u", cs.landValue, cs.powerOk, cs.waterOk, cs.sewageOk, cs.coverage[0], cs.coverage[1],
                 cs.coverage[2], cs.coverage[3], cs.coverage[4], cs.coverage[5]);
        smp.simInfo.push_back({"CELL STATE", b});
    }
}
