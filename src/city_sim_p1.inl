// ------------------------------------------------------------------- city sim Phase 1: citizens, households, jobs, commuting, births, deaths, migration
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 1 owns this file; the hub calls the five hooks below,
// whose signatures are frozen by Phase 0.
//   Init    once, from SimInit after the existing systems: the "life" scheduler system, CitySim::externalPeople (the
//           records are the one source of who lives and works where), tuning ("life." keys in assets\sim\tuning.json)
//   Frame   once per frame from SimSync, after the scheduler ran: the building rows (homes, job slots, cells, hoods) into
//           the citizen sim when anything changed, a load's wait for its lots, the bootstrap, one hood travel tree a frame
//   Save    once, at init: CITZ HHLD OCCU MIGR (sim_life_save.h payloads in sim_save.h chunks)
//   Load    after every city reset and load: the four chunks (with BLDG) restore the records; else start fresh and
//           bootstrap the built homes with the old split once the lots are back (old saves), mapping their Q people
//   Panels  once, at init: Population and Building (households, members, workers)
// The "life" system each game minute: LifeSim::Tick (monthly slices, moves, job matching every 10 minutes); every 10
// minutes the records' views go into CitySim's cells and supercell links (sim_life_cells.h); every 30 minutes this
// phase's factors (unemployment, commuteMin) into g_cellFactors.

tbd::city::LifeSim g_life;
tbd::city::SimEventRing g_simEvents;  // the phases' shared event ring (sim_events.h): producers push, news / radio read
tbd::city::CellFactors g_cellFactors; // the phases' shared factor table (sim_factors.h): each field has one writer phase

namespace herds_host { // city_herds.inl (included after the phase hub) owns these; P1 reads the hoods and the link table
extern tbd::city::SimHoods hoods;
extern tbd::city::LinkTable table;
extern bool tableReady;
} // namespace herds_host

namespace sim_p1 {
bool on = false;               // Init ran (life.enabled)
bool cellsReady = false;       // the records' cell indices are CitySim's (false from a load until its lots are back)
bool bootstrap = false;        // fill the built homes with the old split at the next building sync (old saves, new maps)
bool waitLots = false;         // a load: its lots come back a few frames later
uint64_t waitSinceMs = 0;
std::vector<uint64_t> savedPeople; // an old save's Q records (their home lots): pinned onto records after the bootstrap
tbd::city::LifeChunks in;      // the .tbdsim chunks read on load
uint8_t inMask = 0;            // which of the four came in (1 CITZ, 2 HHLD, 4 OCCU, 8 MIGR)
struct Sig {
    uint64_t reg = ~0ull, zones = ~0ull, hoods = ~0ull;
    size_t cells = ~(size_t)0;
    bool operator==(const Sig& o) const { return reg == o.reg && zones == o.zones && hoods == o.hoods && cells == o.cells; }
} synced;
uint64_t lastSyncMs = 0;
double syncMs = 0.0, applyMs = 0.0; // last costs (the F3 line)
tbd::city::LifeApplyState applyState; // the records' links into CitySim, rebuilt only on a change
// hood -> hood free-flow minutes from the herds' link table: one destination tree per frame into `building`, swapped
// into `travel` when every hood is done (then the seekers' search order follows: LifeSim::TravelChanged)
std::vector<float> travel, building;
int travelHoods = 0, buildHoods = 0, nextDest = 0;
uint64_t travelKey = ~0ull, buildKey = ~0ull;
tbd::city::LifePopulationStats pop; // the Population panel's numbers, at most twice a second
uint64_t popMs = 0;

double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t CitySeed() { return (uint64_t)(uint32_t)g_engine.terrain.params.seed; }

// Minutes between two hoods by road (0: not known yet, the straight line rules).
float TravelMinutes(int a, int b) {
    if (a < 0 || b < 0 || a >= travelHoods || b >= travelHoods) return 0.0f;
    const float m = travel[(size_t)a * (size_t)travelHoods + (size_t)b];
    return std::isfinite(m) ? std::min(m, 600.0f) : 600.0f;
}

// The records' views into CitySim (cells and links) and the HUD totals.
void Apply() {
    if (!cellsReady) return;
    const double t0 = Now();
    tbd::city::LifeApplyCells(g_life.w, g_citySim, &applyState); // the link maps only when they changed
    g_simTotals.population = g_citySim.Population(), g_simTotals.adults = g_citySim.Adults();
    g_simTotals.working = g_citySim.Working(), g_simTotals.jobs = g_citySim.JobCap(), g_simTotals.homes = g_citySim.HomeCap();
    applyMs = Now() - t0;
}
} // namespace sim_p1

namespace sim_p1 {
// The building sync in three steps, one per frame (each about a third of the ~1 ms it took at 15k lots in one frame):
//   1 the registry rows' homes, jobs, cells and hoods (LifeFillRows)
//   2 the citizen sim's buildings: evictions, firing, rehousing (LifeSyncBuildings); the rows again first if the
//     registry synced in between (P0 rewrites them)
//   3 each hood's search order (LifeComputeNear) and the views into CitySim
// The first sync after a reset or a load (and the bootstrap) runs all three at once. stage: the next step, 0 none.
int stage = 0;
uint64_t filledReg = ~0ull; // the registry version step 1 filled
void FillRows() {
    using namespace tbd::city;
    const auto& hs = herds_host::hoods;
    LifeFillRows(g_simBuildings, g_citySim, g_zones.lots, [&hs](int c) { return (size_t)c < hs.cellHood.size() ? hs.cellHood[(size_t)c] : -1; },
                 &sim_save::hintLot); // the registry sync's per-lot ids (checked before use)
    filledReg = g_simBuildings.version;
}
void SyncStep(bool all) {
    using namespace tbd::city;
    const double t0 = Now();
    const auto& hs = herds_host::hoods;
    if (stage == 0) stage = 1;
    if (stage == 1) {
        FillRows();
        synced = {g_simBuildings.version, g_zones.version, hs.generation, g_citySim.cells.cells.size()};
        lastSyncMs = tbd::platform::TickCountMs();
        stage = 2;
        if (!all) { syncMs = Now() - t0; return; }
    }
    if (stage == 2) {
        if (filledReg != g_simBuildings.version) FillRows();
        LifeSyncBuildings(g_life.w, g_simBuildings, [](const BuildingRow& r) { return r.origin == BOrigin::Lot ? LifeJobKind(r.kind) : -1; });
        stage = 3;
        if (!all) { syncMs = Now() - t0; return; }
    }
    LifeComputeNear(g_life.w); // what LifeSim::SyncBuildings did after the sync
    stage = 0;
    syncMs = Now() - t0;
    Apply();
}

// One destination hood's route tree a frame (the herds' link table, free-flow minutes) until the matrix is complete.
void TravelStep() {
    using namespace tbd::city;
    const auto& hs = herds_host::hoods;
    if (!herds_host::tableReady || hs.hoods.empty()) return;
    const uint64_t key = herds_host::table.version * 1000003ull ^ hs.generation;
    if (key == travelKey) return;
    const int n = (int)hs.hoods.size();
    if (key != buildKey) { // new roads or hoods: the old matrix's ids mean nothing now (the straight line rules meanwhile)
        buildKey = key, buildHoods = n, nextDest = 0, travelHoods = 0;
        building.assign((size_t)n * (size_t)n, INFINITY);
    }
    if (nextDest < n) {
        const int dest = hs.hoods[(size_t)nextDest].centroidNode;
        if (dest >= 0) {
            RouteTree tr;
            RouteTree::Build(herds_host::table, dest, tr);
            for (int a = 0; a < n; ++a) {
                const int src = hs.hoods[(size_t)a].centroidNode;
                building[(size_t)a * (size_t)n + (size_t)nextDest] = src >= 0 && (size_t)src < tr.minutes.size() ? tr.minutes[(size_t)src] : INFINITY;
            }
        }
        building[(size_t)nextDest * (size_t)n + (size_t)nextDest] = 0.0f;
        ++nextDest;
        return;
    }
    travel.swap(building), travelHoods = buildHoods, travelKey = key;
    g_life.TravelChanged();
    static int logged = -1; // the hoods rebuild often while a city grows: log a new count only
    if (travelHoods != logged) logged = travelHoods, tbd::Log("Sim: P1 hood travel times for %d hoods", travelHoods);
}

// The "life" system: every game minute.
void Tick(const tbd::city::SimTick& t) {
    if (!cellsReady) return; // a load waits for its lots
    g_life.Tick(t.minute);
    if (t.minute % 10 == 5) Apply();
    if (t.minute % 30 == 17) g_life.WriteFactors(g_cellFactors);
}

// "life.<name>" values of tuning.json into the citizen sim (the phase table holds only enabled and instantFill).
void LoadLifeTuning(tbd::city::LifeTuning& t) {
    t = tbd::city::LifeTuning();
    std::ifstream f(tbd::platform::FsPath(tbd::ExeDirectory() + L"\\..\\assets\\sim\\tuning.json"), std::ios::binary);
    if (f) {
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        tbd::city::ForEachJsonNumber(text, [&t](const std::string& k, double v) { t.Set(k.c_str(), v); });
    }
    if (g_simPhaseTune.life.instantFill != 0.0) t.instantFill = (int)g_simPhaseTune.life.instantFill;
}
} // namespace sim_p1

void SimP1Init() {
    using namespace sim_p1;
    on = true;
    g_citySim.externalPeople = true; // CitySim::Add gives empty homes and job slots; the records move people in
    LoadLifeTuning(g_life.w.tune);
    g_life.w.cal = g_simCalendar;
    g_life.w.events = &g_simEvents;
    g_life.w.factors = &g_cellFactors;
    g_life.w.travel = [](int a, int b) { return sim_p1::TravelMinutes(a, b); };
    g_life.Reset(CitySeed());
    g_sim.Register("life", 1, 1, sim_p1::Tick);
    tbd::Log("Sim: P1 citizens on (record cap %d, move-in %.2f a month%s)", g_life.w.tune.recordCap, g_life.w.tune.moveInRate,
             g_life.w.tune.instantFill ? ", instant fill" : "");
}

// city_sim.inl SimApplyPending: a load's lots are back on their cells (the Q records come with them).
void SimP1LotsBack() { if (sim_p1::waitLots) sim_p1::waitLots = false; }
void SimP1SavedPerson(uint64_t homeLot) { sim_p1::savedPeople.push_back(homeLot); }
bool SimP1On() { return sim_p1::on; }

void SimP1Frame(float /*realDt*/) {
    using namespace sim_p1;
    const uint64_t now = tbd::platform::TickCountMs();
    if (waitLots && g_pendingZones.empty() && g_pendingLots.empty()) waitLots = false; // restored (or there were none)
    if (waitLots && now - waitSinceMs > 30000) { // the zone grid never came back for them
        tbd::Log("Sim: P1 gave up waiting for the load's lots");
        waitLots = false;
    }
    // The load's lots are back: the registry claims their saved ids by key at the next sync (forced below). Rows that
    // never come back stay reserved (LifeSyncBuildings keeps them) until P0 releases them; then their people move out.
    if (waitLots) return;
    const Sig sig{g_simBuildings.version, g_zones.version, herds_host::hoods.generation, g_citySim.cells.cells.size()};
    const bool first = !cellsReady || bootstrap;
    if (stage && !first) { SyncStep(false); TravelStep(); return; } // a staged sync goes on (one step a frame)
    if (first || (!(sig == synced) && now - lastSyncMs >= 1000)) { // the registry's own cadence
        if (first) SimBuildingsSync(true), stage = 0; // the registry has every lot now
        SyncStep(first);
        if (!first) { TravelStep(); return; }
        if (bootstrap) {
            bootstrap = false;
            g_life.Bootstrap();
            const int pinned = savedPeople.empty() ? 0 : tbd::city::LifePinSaved(g_life, g_simBuildings, savedPeople);
            tbd::Log("Sim: P1 bootstrap: %u citizens in %u households, %d statistical; %d of %zu saved people pinned", g_life.w.cit.Live(),
                     g_life.w.hh.Live(), g_life.w.statResidents, pinned, savedPeople.size());
        }
        savedPeople.clear(); // loaded records keep their own pins
        cellsReady = true;
        Apply();
    }
    TravelStep();
}

void SimP1Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    struct Part { uint32_t fourcc; uint8_t bit; std::vector<uint8_t> LifeChunks::*bytes; };
    static const Part kParts[] = {{simchunk::CITZ, 1, &LifeChunks::citz}, {simchunk::HHLD, 2, &LifeChunks::hhld},
                                  {simchunk::OCCU, 4, &LifeChunks::occu}, {simchunk::MIGR, 8, &LifeChunks::migr}};
    for (const Part& p : kParts) {
        SimChunkDef d;
        d.fourcc = p.fourcc;
        d.version = kLifeChunkVersion;
        // Each chunk makes its own part straight into the payload (no second copy); the four only read g_life, so the
        // autosave copies them on four threads at once (city_sim_save_io.inl SimTbdSnapshot). Empty (a load then
        // bootstraps) while the bootstrap of an old save still waits for its lots.
        d.save = [p](ByteWriter& w) {
            if (!sim_p1::on || sim_p1::bootstrap) return;
            const int part = p.bit == 1 ? 0 : p.bit == 2 ? 1 : p.bit == 4 ? 2 : 3;
            if (w.bytes.empty()) { LifeSavePart(g_life, part, w.bytes); return; }
            std::vector<uint8_t> b;
            LifeSavePart(g_life, part, b);
            w.Raw(b.data(), b.size());
        };
        d.load = [p](ByteReader& r, uint16_t) {
            std::vector<uint8_t>& b = sim_p1::in.*(p.bytes);
            b.resize(r.Remaining());
            if (!b.empty() && !r.Raw(b.data(), b.size())) return false;
            sim_p1::inMask |= p.bit;
            return true;
        };
        d.fresh = [p] { (sim_p1::in.*(p.bytes)).clear(), sim_p1::inMask &= (uint8_t)~p.bit; };
        reg.Register(d);
    }
}

void SimP1Load(const tbd::city::SimLoadReport& rep) {
    using namespace sim_p1;
    using namespace tbd::city;
    if (!on) return;
    const bool isLoad = g_simLoadInProgress; // city_sim.inl: this reset came with a load
    const SimChunkResult* bldg = rep.Find(simchunk::BLDG);
    const bool bldgOk = bldg && bldg->status == SimChunkStatus::Loaded;
    g_life.w.cal = g_simCalendar;
    g_life.Reset(CitySeed());
    cellsReady = false, bootstrap = false, synced = Sig(), travelKey = buildKey = ~0ull, travelHoods = 0, stage = 0;
    if (inMask == 15 && bldgOk) {
        if (LifeLoad(g_life, in)) tbd::Log("Sim: P1 loaded %u citizens in %u households", g_life.w.cit.Live(), g_life.w.hh.Live());
        else tbd::Log("Sim: P1 chunks did not check out: citizens start fresh"), bootstrap = true;
    } else {
        bootstrap = true; // a new map, or a save from before this phase: the built homes get the old split
    }
    in = LifeChunks(), inMask = 0;
    waitLots = isLoad, waitSinceMs = tbd::platform::TickCountMs();
    if (!isLoad) savedPeople.clear();
}

namespace sim_p1 {
using Item = std::pair<std::string, std::string>;
const char* const kEduName[tbd::city::kEduLevels] = {"no school", "elementary", "high school", "college", "university"};

std::string NameOf(uint32_t seed) {
    tbd::city::Person p;
    p.seed = seed;
    return tbd::city::SimPeople::Name(p);
}
std::string JobPlace(uint32_t work) {
    static const char* const kJob[] = {"in industry", "in a shop", "in an office"};
    const tbd::city::LifeBuilding* J = g_life.w.B(work);
    return J && J->jobKind >= 0 && J->jobKind < 3 ? kJob[J->jobKind] : "nearby";
}
// One resident: ("Name, 34", "high school, works in an office (22 min), happy 62%: Commute -8, Wealth +5").
Item MemberItem(const tbd::city::LifeMemberInfo& m) {
    using namespace tbd::city;
    std::string what = kEduName[std::clamp(m.edu, 0, kEduLevels - 1)];
    if (m.work) what += ", works " + JobPlace(m.work) + " (" + std::to_string(m.commuteMin) + " min)";
    else if (m.flags & kCitStudent) what += m.age < 18 ? ", at school" : ", student";
    else if (m.flags & kCitRetired) what += ", retired";
    else if (m.age < 18) what += ", child";
    else what += ", looking for work";
    what += ", happy " + std::to_string((int)std::lround(m.happy / 2.55)) + "%";
    for (int k = 0; k < 2; ++k)
        if (m.reason[k] >= 0) what += std::string(k ? ", " : ": ") + LifeReasonName(m.reason[k]) + (m.reasonPts[k] >= 0 ? " +" : " ") + std::to_string(m.reasonPts[k]);
    return {NameOf(m.seed) + ", " + std::to_string(m.age), what};
}

// The residents and workers of zoned lot `lot` from the records. False: no such building in the citizen sim.
struct LotPeople {
    uint32_t homes = 0, residents = 0, households = 0, statistical = 0, slots = 0, filled = 0;
    std::vector<Item> members, workers;
};
bool CollectLot(uint64_t lot, LotPeople& out, size_t maxMembers, size_t maxWorkers) {
    using namespace tbd::city;
    out = LotPeople();
    const BuildingId b = g_simBuildings.Find(BOrigin::Lot, lot);
    const LifeBuilding* L = b.Valid() ? g_life.w.B(b.v) : nullptr;
    if (!L) return false;
    std::vector<LifeHouseholdInfo> hh;
    LifeBuildingInfo(g_life.w, b.v, hh, out.statistical);
    out.homes = L->homes, out.households = (uint32_t)hh.size(), out.residents = out.statistical, out.slots = L->Slots();
    for (int e = 0; e < kEduLevels; ++e) out.filled += L->filled[e];
    for (const LifeHouseholdInfo& h : hh)
        for (const LifeMemberInfo& m : h.members) {
            ++out.residents;
            if (out.members.size() < maxMembers) out.members.push_back(MemberItem(m));
        }
    if (out.filled) // one pass over the records (a click, not per frame)
        for (uint32_t ci = 1; ci < g_life.w.cit.Capacity() && out.workers.size() < maxWorkers; ++ci)
            if (g_life.w.cit.work[ci] == b.v && g_life.w.cit.At(ci).Valid())
                out.workers.push_back({NameOf(g_life.w.cit.seed[ci]) + ", " + std::to_string(g_life.w.AgeOf(ci)),
                                       std::string(kEduName[g_life.w.cit.edu[ci]]) + ", " + std::to_string(g_life.w.cit.commuteMin[ci]) + " min commute"});
    return true;
}
} // namespace sim_p1

// city_sim.inl SimShowLot (the HUD list and the info card's notes): the picked lot's people. False: P1 is off.
bool SimP1LotLines(uint64_t lot, std::string& title, std::vector<std::string>& lines) {
    using namespace sim_p1;
    if (!on) return false;
    LotPeople p;
    if (!CollectLot(lot, p, 8, 4)) { lines.push_back("Nobody lives or works here yet (still being built)."); return true; }
    if (p.homes) {
        title += ": " + std::to_string(p.residents) + " residents in " + std::to_string(p.households) + " households";
        if (!p.residents) lines.push_back(std::to_string(p.homes) + " homes, nobody has moved in yet.");
        for (const Item& m : p.members) lines.push_back(m.first + ", " + m.second);
        const uint32_t named = p.residents - p.statistical;
        if (named > p.members.size()) lines.push_back("... and " + std::to_string(named - p.members.size()) + " more");
        if (p.statistical) lines.push_back(std::to_string(p.statistical) + " more residents (statistical: past the record cap)");
    }
    if (p.slots) {
        lines.push_back("Workers: " + std::to_string(p.filled) + " of " + std::to_string(p.slots) + " jobs filled");
        for (const Item& w : p.workers) lines.push_back("  " + w.first + ", " + w.second);
    }
    return true;
}
// city_interact.inl's info card: residents of a lot (-1: P1 is off, the old counters say).
int SimP1LotResidents(uint64_t lot) {
    if (!sim_p1::on) return -1;
    const tbd::city::BuildingId b = g_simBuildings.Find(tbd::city::BOrigin::Lot, lot);
    const tbd::city::LifeBuilding* L = b.Valid() ? g_life.w.B(b.v) : nullptr;
    return L ? (int)L->used + (int)L->stat : 0;
}

namespace sim_p1 {
std::string Num(long long v) { return std::to_string(v); }
std::vector<float> Series(const std::vector<int32_t>& v) {
    std::vector<float> s;
    s.reserve(v.size());
    for (int32_t x : v) s.push_back((float)x);
    return s;
}
std::string Last(const std::vector<int32_t>& v) { return v.empty() ? "-" : Num(v.back()) + " last month"; }

void FillPopulation(tbd::gui::SimPanelSpec& s, void*) {
    using namespace tbd::city;
    using tbd::gui::Rgba;
    const uint64_t now = tbd::platform::TickCountMs();
    if (!popMs || now - popMs >= 500) popMs = now, LifePopulation(g_life.w, pop); // O(citizens): twice a second at most
    const LifePopulationStats& p = pop;
    s.title = "Population";
    s.sub = Num(g_life.w.Population()) + " residents, " + Num(g_life.w.hh.Live()) + " households";
    s.accent = Rgba(20, 184, 166);
    if (p.statistical) s.Stat("Statistical residents", Num(p.statistical));
    s.Caption("Age (men | women)");
    uint32_t bins[9][2] = {}, top = 1;
    for (int b = 0; b < LifePopulationStats::kBins; ++b) {
        const int k = std::min(8, b / 2);
        bins[k][0] += p.male[b], bins[k][1] += p.female[b];
    }
    for (int k = 0; k < 9; ++k) top = std::max(top, bins[k][0] + bins[k][1]);
    for (int k = 8; k >= 0; --k) {
        const std::string label = k == 8 ? "80+" : Num(k * 10) + "-" + Num(k * 10 + 9);
        s.Bar(label, (float)(bins[k][0] + bins[k][1]) / (float)top, Num(bins[k][0]) + " | " + Num(bins[k][1]));
    }
    static const tbd::gui::Color kEduCol[kEduLevels] = {Rgba(120, 120, 130), Rgba(90, 160, 220), Rgba(80, 200, 140), Rgba(240, 180, 60), Rgba(200, 110, 230)};
    std::vector<tbd::gui::SimStackPart> edu;
    for (int e = 0; e < kEduLevels; ++e) edu.push_back({kEduName[e], (float)p.eduAdults[e], kEduCol[e]});
    s.Stacked("Education (adults)", edu);
    const uint32_t able = p.adults > p.students ? p.adults - p.students : 0;
    s.Bar("Employed", able ? (float)p.working / (float)able : 0.0f, Num(p.working) + " of " + Num(able), 1);
    const float jobless = able ? (float)p.seekers / (float)able : 0.0f;
    s.Stat("Looking for work", Num(p.seekers) + " (" + Num(std::lround(jobless * 100.0f)) + "%)", jobless > 0.1f ? -1 : jobless < 0.05f ? 1 : 0);
    s.Stat("Students (college)", Num(p.students));
    s.Stat("Retired", Num(p.retired));
    s.Spark("Population", Series(p.population), p.population.empty() ? "-" : Num(p.population.back()));
    s.Spark("Births", Series(p.births), Last(p.births));
    s.Spark("Deaths", Series(p.deaths), Last(p.deaths));
    s.Spark("Moved in", Series(p.immigrants), Last(p.immigrants));
    s.Spark("Moved away", Series(p.emigrants), Last(p.emigrants));
}

void FillBuilding(tbd::gui::SimPanelSpec& s, void*) {
    using tbd::gui::Rgba;
    s.title = "Building", s.accent = Rgba(79, 174, 91);
    static uint64_t lot = ~0ull, at = 0;
    static LotPeople p;
    static bool known = false;
    const uint64_t now = tbd::platform::TickCountMs();
    if (g_pickedLot != lot || now - at >= 1000) lot = g_pickedLot, at = now, known = g_pickedLot && CollectLot(g_pickedLot, p, 40, 12);
    if (!g_pickedLot) { s.Caption("Tap a building to see who lives and works there."); return; }
    s.sub = "Lot " + Num((long long)g_pickedLot);
    if (!known) { s.Caption("Nobody lives or works here yet."); return; }
    if (p.homes) {
        s.Stat("Residents", Num(p.residents) + " of " + Num(p.homes) + " homes");
        s.Stat("Households", Num(p.households));
        if (p.statistical) s.Stat("Statistical residents", Num(p.statistical));
        if (!p.members.empty()) s.List("Residents", p.members, 40);
    }
    if (p.slots) {
        s.Bar("Jobs filled", (float)p.filled / (float)p.slots, Num(p.filled) + " of " + Num(p.slots), p.filled * 2 >= p.slots ? 1 : -1);
        if (!p.workers.empty()) s.List("Workers", p.workers, 12);
    }
}
} // namespace sim_p1

void SimP1Panels() {
    if (!sim_p1::on) return;
    tbd::gui::SimPanelEntry pop;
    pop.id = "p1.population", pop.name = "Citizens", pop.icon = "ppl", pop.fill = sim_p1::FillPopulation;
    tbd::gui::RegisterSimPanel(pop);
    tbd::gui::SimPanelEntry bld;
    bld.id = "p1.building", bld.name = "Building", bld.icon = "house", bld.fill = sim_p1::FillBuilding;
    tbd::gui::RegisterSimPanel(bld);
}
