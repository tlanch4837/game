// ------------------------------------------------------------------- city sim Phase 2: demand, land value, rent, levelling, abandonment, density
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 2 owns this file and src/city/sim_land*.h,
// sim_demand.h (headless, tests\sim_land_*_test, sim_demand_test). With P2 on, city_sim_growth.inl registers nothing of
// the old S2 model (sim_growth.h): these systems replace it and write the same g_simGrowth.demand / raw the HUD, the
// spawn gate and the economy read.
//   land        every game hour: the previous hour's land pass lands (CellSim::landValue and CellFactor::rentBurden of
//               the cells that changed; the lots in them re-check), then the cells are snapshotted and the next pass
//               runs on a worker (incremental: only cells whose inputs changed). A pass the worker has not finished
//               by then is computed on the main thread from the same input: the result never depends on timing.
//   land.lots   every game minute: LotLevels (sim_land_lots.h): one sweep group, at most evalPerTick dirty lots, the due
//               queue. Lots are marked dirty only by triggers (land value of their cell, their utility / flag bits,
//               a demand threshold of their category, a company event).
//   demand      every 10 game minutes: the bars from causes (sim_demand.h) and the spawn gate.
//   land.rent   every 15 game minutes: one of 96 household groups (round robin, a day for all): affordability, the
//               high rent flag at the end of the day.
// Tables P2 writes: g_cellFactors.rentBurden; g_simFlagTable kFlagHighRent and kFlagAbandoned (the hub merges P2's bits
// when P2 is on); g_simEvents LevelUp, LevelDown, Abandoned, Recovered, HighRent. Reads P6's CompanyBankrupt / Opened.
// Tuning: the "land." keys (sim_tuning_table.h LandTuning).

namespace sim_p2 {
using namespace tbd::city;
LandParams lp;
RentParams rp;
DemandParams dp;
LotLevels lots;
std::shared_ptr<LandState> cur = std::make_shared<LandState>(); // the applied land state (the worker reads it through its own reference)
LandResult curResult;
DemandSaveState dm;          // bars (mirrored into g_simGrowth.demand), threshold zones, the rent day
DemandInput lastIn;
DemandResult lastDemand;
std::vector<float> history[kDemandCats]; // one per game hour, at most 48 (the panel's sparklines)
std::unordered_map<uint64_t, uint8_t> waterView; // PackKey -> water in view (sampled once per cell)
std::vector<uint8_t> cellDensity;                 // per CitySim cell: the homes-weighted density tier
uint64_t densityFor = ~0ull;
std::unordered_map<uint64_t, uint8_t> lotBits;    // lot id -> the LotInBits it was last judged with (triggers)
uint64_t bitsUtil = ~0ull, bitsTable = ~0ull;
std::unordered_set<uint32_t> closed;              // BuildingId raws of bankrupt companies (P6 events)
tbd::city::SimEventRing::Cursor cursor = 0;
double landMs = 0, lotsMs = 0, lotsAvgMs = 0, lotsWorstMs = 0, demandMs = 0, rentMs = 0, stepMs = 0;
int syncFallbacks = 0, checkMismatch = 0, checks = 0;
uint64_t highRentNow = 0;

struct Job {
    LandInput in;
    std::shared_ptr<const LandState> base; // the applied state the pass starts from (never written while shared)
    LandState state;
    bool check = false, checkOk = true;    // the debug check: the incremental result against a full recompute
    LandResult out;
    double ms = 0;
    std::atomic<bool> ready{false};
};
std::shared_ptr<Job> job;
bool jobDone = false;
LandState doneState;
LandResult doneResult;
LandState loadedCur, loadedPending;
bool loadedHasPending = false;
} // namespace sim_p2

bool SimP2On() { return g_simPhaseTune.land.enabled != 0.0; }

void SimP2ApplyTuning(const tbd::city::LandTuning& t) {
    using namespace sim_p2;
    auto i = [](double v) { return (int)std::lround(v); };
    lots.p = tbd::city::LotParams();
    lots.p.upMinutes = (int64_t)(t.upDays * 1440), lots.p.downMinutes = (int64_t)(t.downDays * 1440);
    lots.p.abandonMinutes = (int64_t)(t.abandonDays * 1440), lots.p.recoverMinutes = (int64_t)(t.recoverDays * 1440);
    lots.p.upLand = i(t.upLand), lots.p.upLandStep = i(t.upLandStep), lots.p.hysteresis = i(t.hysteresis), lots.p.abandonLand = i(t.abandonLand);
    lots.p.badDemand = t.badDemand, lots.p.maxActionsPerHour = std::max(1, i(t.maxActionsPerHour)), lots.p.actionsPerLots = std::max(1, i(t.actionsPerLots));
    lots.p.utilitiesRequired = g_simTune.utilitiesRequired > 0.0;
    rp = tbd::city::RentParams();
    rp.affordPct = std::clamp(i(t.affordPct), 1, 100), rp.referenceLand = std::max(1, i(t.referenceLand)), rp.floorPct = std::clamp(i(t.rentFloorPct), 0, 100);
    dp = tbd::city::DemandParams();
    dp.rent = t.demandRent, dp.migration = t.demandMigration, dp.workers = t.demandWorkers, dp.shortfall = t.demandShortfall;
    dp.startPull = g_simTune.demandStartPull, dp.startPopulation = g_simTune.demandStartPopulation, dp.baseResidential = g_simTune.demandBaseResidential;
    dp.commercialPerResident = g_simTune.commercialPerResident, dp.industryPerCommercial = g_simTune.industryPerCommercial;
    dp.tauMinutes = g_simTune.demandTauMinutes;
}

// ---- the land pass
namespace sim_p2 {
// Per CitySim cell: the homes-weighted density tier and whether a building there has a road entrance. Only when the
// building registry changed (not hourly); the rent of every cell follows.
std::vector<uint8_t> cellRoad;
void RefreshDensity() {
    if (densityFor == g_simBuildings.version && cellDensity.size() == g_citySim.sim.size()) return;
    densityFor = g_simBuildings.version;
    const size_t n = g_citySim.sim.size();
    std::vector<uint32_t> homes(n, 0), weighted(n, 0);
    cellRoad.assign(n, 0);
    for (const BuildingRow& r : g_simBuildings.rows) {
        if (r.alive != 1 || r.cell < 0 || (size_t)r.cell >= n) continue;
        if (r.entranceNode >= 0) cellRoad[(size_t)r.cell] = 1;
        if (r.origin != BOrigin::Lot || !r.homes) continue;
        homes[(size_t)r.cell] += r.homes, weighted[(size_t)r.cell] += r.homes * (uint32_t)DensityOf(r.kind);
    }
    cellDensity.assign(n, (uint8_t)kDensityRow);
    for (size_t c = 0; c < n; ++c)
        if (homes[c]) cellDensity[c] = (uint8_t)((weighted[c] + homes[c] / 2) / homes[c]);
}
uint8_t CellValue(CellKey k) { const auto it = cur->value.find(PackKey(k)); return it == cur->value.end() ? (uint8_t)lp.base : it->second; }
// One cell's land value and rent onto the shared tables.
void WriteCell(int ci) {
    if (ci < 0 || (size_t)ci >= g_citySim.sim.size()) return;
    const uint8_t v = CellValue(g_citySim.cells.cells[(size_t)ci].key);
    g_citySim.sim[(size_t)ci].landValue = v;
    g_cellFactors.Fit(g_citySim.sim.size());
    const int tier = (size_t)ci < cellDensity.size() ? cellDensity[(size_t)ci] : kDensityRow;
    g_cellFactors.cells[(size_t)ci].rentBurden = RentBurden(rp, v, tier);
}
void WriteAllCells() {
    for (size_t c = 0; c < g_citySim.sim.size(); ++c) WriteCell((int)c);
    ++g_cellFactors.version;
}
uint8_t WaterView(CellKey k) {
    const auto it = waterView.find(PackKey(k));
    if (it != waterView.end()) return it->second;
    const bool sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1;
    uint8_t w = 0;
    if (sea) // the cell's centre and 4 points 96 m out: water in any of them is a view
        for (int s = 0; s < 5 && !w; ++s) {
            const float x = (k.x + 0.5f) * kCityCellSize + (s == 1 ? 96.0f : s == 2 ? -96.0f : 0.0f);
            const float z = (k.z + 0.5f) * kCityCellSize + (s == 3 ? 96.0f : s == 4 ? -96.0f : 0.0f);
            w = g_engine.terrain.HeightAt(x, z) < tbd::IslandMap::kSeaLevel ? 1 : 0;
        }
    waterView[PackKey(k)] = w;
    return w;
}
} // namespace sim_p2

// The hourly snapshot (main thread): counters and factors of every cell.
void SimP2BuildInput(int64_t minute, tbd::city::LandInput& in) {
    using namespace tbd::city;
    namespace s = sim_p2;
    s::RefreshDensity();
    in = LandInput();
    in.minute = minute;
    const size_t n = std::min(g_citySim.sim.size(), g_citySim.cells.cells.size());
    in.cells.resize(n);
    int sampled = 0;
    for (size_t c = 0; c < n; ++c) {
        const CellSim& cs = g_citySim.sim[c];
        const CellFactor& f = g_cellFactors.At((int)c);
        LandCellIn& o = in.cells[c];
        o.key = g_citySim.cells.cells[c].key;
        o.jobs = (uint16_t)std::min(65535, cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2]), o.industry = cs.jobCap[kJobIndustrial];
        o.people = (uint16_t)std::min<uint32_t>(65535, cs.Population()), o.commuteMin = f.commuteMin;
        o.cov = (uint8_t)((f.coverage[kSvcFire] + f.coverage[kSvcPolice] + f.coverage[kSvcHealth] + f.coverage[kSvcSchool]) / 4);
        o.park = std::max(f.coverage[kSvcPark], f.parkAccess), o.transit = f.coverage[kSvcTransit];
        o.air = f.air, o.noise = f.noise, o.ground = f.ground, o.crime = f.crime;
        o.road = c < s::cellRoad.size() ? s::cellRoad[c] : 0;
        const bool known = s::waterView.count(PackKey(o.key)) != 0;
        o.water = known || sampled < 64 ? s::WaterView(o.key) : 0; // at most 64 new cells sampled an hour
        sampled += known ? 0 : 1;
    }
}

void SimP2FinishJob() {
    namespace s = sim_p2;
    if (!s::job || s::jobDone) return;
    if (s::job->ready.load(std::memory_order_acquire)) {
        s::doneState = std::move(s::job->state), s::doneResult = std::move(s::job->out), s::stepMs = s::job->ms;
    } else {
        const double t0 = g_sim.nowMs();
        s::doneState = *s::job->base;
        tbd::city::LandStep(s::doneState, s::job->in, s::lp, s::doneResult);
        s::stepMs = g_sim.nowMs() - t0, ++s::syncFallbacks;
    }
    s::jobDone = true;
}

// The finished pass becomes the applied state; the cells that changed write their value and rent, their lots re-check.
void SimP2ApplyResult() {
    namespace s = sim_p2;
    if (s::job && s::job->ready.load(std::memory_order_acquire) && s::job->check && !s::job->checkOk) {
        ++s::checkMismatch;
        tbd::Log("Sim P2: check: the incremental land pass differs from a full recompute (minute %lld)", (long long)s::job->in.minute);
    }
    s::cur = std::make_shared<tbd::city::LandState>(std::move(s::doneState)), s::curResult = std::move(s::doneResult);
    s::job.reset(), s::jobDone = false;
    for (const tbd::city::CellKey k : s::curResult.changed) {
        s::WriteCell(g_citySim.cells.Find(k));
        s::lots.MarkCell(k);
    }
    if (!s::curResult.changed.empty()) ++g_cellFactors.version;
}

// ---- levelling: a lot's inputs, the triggers that mark lots dirty, the minute system
namespace sim_p2 {
tbd::city::BuildingId LotBuilding(uint64_t lot) { return g_simBuildings.Find(tbd::city::BOrigin::Lot, lot); }
uint8_t LotBits(const tbd::city::Lot& l, size_t li) {
    using namespace tbd::city;
    uint8_t b = kLotInPower | kLotInWater | kLotInSewage;
    if (li < g_simLotUtil.size() && g_simLotUtil.size() == g_zones.lots.size()) {
        const uint8_t u = g_simLotUtil[li];
        b = (uint8_t)((u & kLotPower ? kLotInPower : 0) | (u & kLotWater ? kLotInWater : 0) | (u & kLotSewage ? kLotInSewage : 0));
    }
    const BuildingId id = LotBuilding(l.id);
    const SimFlags f = g_simFlagTable.Get(id);
    if (f & kFlagPolluted) b |= kLotInPolluted;
    if (f & kFlagHighRent) b |= kLotInHighRent;
    if (id.Valid() && closed.count(id.v)) b |= kLotInClosed;
    return b;
}
tbd::city::LotInputs LotIn(const tbd::city::Lot& l, size_t li) {
    tbd::city::LotInputs in;
    in.land = cur->field.CellCount() ? cur->field.At(l.c.x, l.c.z) : CellValue(tbd::city::CellKeyAt(l.c.x, l.c.z));
    in.bits = LotBits(l, li);
    lotBits[l.id] = in.bits;
    return in;
}
// The utilities solve or a phase's flags changed: only the lots whose bits differ from what they were judged with get
// re-judged. The comparison itself is spread: at most 600 lots a tick from where it stopped (a pass restarts on a change).
size_t diffCursor = ~(size_t)0;
void DiffLotBits() {
    if (bitsUtil != g_simFlagsVersion || bitsTable != g_simFlagTable.version) bitsUtil = g_simFlagsVersion, bitsTable = g_simFlagTable.version, diffCursor = 0;
    if (diffCursor >= g_zones.lots.size()) return;
    const size_t end = std::min(g_zones.lots.size(), diffCursor + 600);
    for (; diffCursor < end; ++diffCursor) {
        const tbd::city::Lot& l = g_zones.lots[diffCursor];
        if (!l.alive) continue;
        const auto it = lotBits.find(l.id);
        if (it != lotBits.end() && it->second != LotBits(l, diffCursor)) lots.MarkDirty(l.id);
    }
    if (diffCursor >= g_zones.lots.size()) diffCursor = ~(size_t)0;
    if (lotBits.size() > (size_t)lots.Tracked() * 2 + 64) // forget lots that are gone
        for (auto it = lotBits.begin(); it != lotBits.end();) it = lots.Find(it->first) ? std::next(it) : lotBits.erase(it);
}
// P6's company events: a bankrupt company's lot is bad until it reopens.
void ReadEvents() {
    using namespace tbd::city;
    SimEvent e;
    while (g_simEvents.Read(cursor, e)) {
        if (e.type != SimEventType::CompanyBankrupt && e.type != SimEventType::CompanyOpened) continue;
        const bool was = closed.count(e.building) != 0, now = e.type == SimEventType::CompanyBankrupt;
        if (was == now) continue;
        if (now) closed.insert(e.building); else closed.erase(e.building);
        const BuildingRow* r = g_simBuildings.Get(BuildingId(e.building));
        if (r && r->origin == BOrigin::Lot) lots.MarkDirty(r->originId);
    }
}
// The lots' events onto the shared ring and P2's abandoned bit; a few log lines.
void DrainLotEvents(int64_t minute) {
    using namespace tbd::city;
    static const SimEventType kType[] = {SimEventType::LevelUp, SimEventType::LevelDown, SimEventType::Abandoned, SimEventType::Recovered, SimEventType::Abandoned};
    static const char* const kText[] = {"level up", "level down", "abandoned", "recovered", "demolished (abandoned)"};
    int logged = 0;
    for (const LotEvent& e : lots.events) {
        const BuildingId b = LotBuilding(e.lot);
        g_simEvents.Push(kType[(int)e.type], minute, b.v, 0, e.type == LotEventType::Demolished ? -1 : e.level);
        if (e.type == LotEventType::Abandoned) g_simFlagTable.Write(b, kFlagAbandoned, kFlagAbandoned);
        if (e.type == LotEventType::Recovered || e.type == LotEventType::Demolished) g_simFlagTable.Write(b, kFlagAbandoned, 0);
        if (logged++ < 3) tbd::Log("Sim P2: lot %llu %s (L%d)", (unsigned long long)e.lot, kText[(int)e.type], e.level);
    }
    g_simGrowth.levelUps = lots.levelUps, g_simGrowth.levelDowns = lots.levelDowns, g_simGrowth.abandoned = lots.abandonedTotal;
    lots.events.clear();
}
} // namespace sim_p2

void SimP2LotsTick(int64_t minute) {
    namespace s = sim_p2;
    const double t0 = g_sim.nowMs();
    s::DiffLotBits();
    s::lots.Tick(g_zones, minute, g_simGrowth.demand, [](const tbd::city::Lot& l, size_t li) { return s::LotIn(l, li); });
    if (!s::lots.events.empty()) s::DrainLotEvents(minute);
    // the debug check: one group of lots a tick re-judged from scratch, over checkHours (0 off)
    const double hours = g_simPhaseTune.land.checkHours;
    if (hours > 0 && s::diffCursor == ~(size_t)0 && (minute / 60) % std::max<int64_t>(1, (int64_t)hours) == 0) {
        const size_t groups = 60;
        const int bad = s::lots.FullCheck(g_zones, g_simGrowth.demand, [](const tbd::city::Lot& l, size_t li) {
            tbd::city::LotInputs in;
            in.land = s::cur->field.CellCount() ? s::cur->field.At(l.c.x, l.c.z) : s::CellValue(tbd::city::CellKeyAt(l.c.x, l.c.z));
            in.bits = s::LotBits(l, li);
            return in;
        }, (size_t)(minute % 60), groups);
        if (bad) s::checkMismatch += bad, tbd::Log("Sim P2: check: %d lots judged on stale inputs (minute %lld)", bad, (long long)minute);
        if (minute % 60 == 59) ++s::checks;
    }
    const double ms = g_sim.nowMs() - t0;
    s::lotsMs = ms, s::lotsAvgMs += (ms - s::lotsAvgMs) * 0.01, s::lotsWorstMs = std::max(s::lotsWorstMs, ms);
}

// ---- demand: the causes from the counters and the records, every 10 game minutes
namespace sim_p2 {
uint64_t pendFor = ~0ull;
int64_t pendHomes = 0, pendJobs[3] = {};
void Pipeline() { // what is under construction; only when the zone grid changed
    if (pendFor == g_zones.version) return;
    pendFor = g_zones.version, pendHomes = 0, pendJobs[0] = pendJobs[1] = pendJobs[2] = 0;
    for (const tbd::city::Lot& l : g_zones.lots)
        if (l.alive && !l.Built()) {
            const tbd::city::LotCap c = tbd::city::LotCapacity(l);
            pendHomes += c.homes;
            for (int k = 0; k < 3; ++k) pendJobs[k] += c.jobs[k];
        }
}
} // namespace sim_p2

void SimP2BuildDemand(tbd::city::DemandInput& in) {
    using namespace tbd::city;
    namespace s = sim_p2;
    in = DemandInput();
    int64_t goods = 0, goodsN = 0;
    for (size_t c = 0; c < g_citySim.sim.size(); ++c) {
        const CellSim& cs = g_citySim.sim[c];
        in.population += cs.Population(), in.adults += cs.residents[kAdults], in.homes += cs.homeCap;
        for (int k = 0; k < 3; ++k) in.jobs[k] += cs.jobCap[k], in.vacancies[k] += cs.vacancies[k];
        if (cs.Population() || cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2]) goods += g_cellFactors.At((int)c).goodsAvailable, ++goodsN;
    }
    in.freeHomes = std::max<int64_t>(0, in.homes - in.population);
    in.goodsAvailable = goodsN ? (int)(goods / goodsN) : 255;
    s::Pipeline();
    in.pendingHomes = s::pendHomes;
    for (int k = 0; k < 3; ++k) in.pendingJobs[k] = s::pendJobs[k];
    if (SimP1On()) { // the records: job seekers by education level, the migration attractiveness
        const LifeWorld& w = g_life.w;
        for (const auto& hood : w.seekers)
            for (uint32_t c : hood) {
                const uint32_t i = LifeWorld::Ix(c);
                if (i < w.cit.edu.size()) ++in.seekers[std::min<int>(4, w.cit.edu[i])];
            }
        in.attract = g_life.mig.attract;
    } else { // the old counters: jobless adults split by the cells' education bands
        for (const CellSim& cs : g_citySim.sim) {
            const int64_t u = cs.Unemployed(), e = (int64_t)cs.edu[0] + cs.edu[1] + cs.edu[2];
            if (!u) continue;
            const int64_t hi = e ? u * cs.edu[2] / e : 0, mid = e ? u * cs.edu[1] / e : 0;
            in.seekers[1] += u - hi - mid, in.seekers[2] += mid, in.seekers[4] += hi;
        }
    }
    const Affordability& a = s::dm.rent.last;
    in.rentPressure = a.Pressure();
    if (a.persons > 0 && SimP1On()) in.spendIndex = std::clamp((double)a.spend / (double)a.persons / std::max(1.0, 1.3 * g_life.w.tune.spendPerPerson), 0.0, 2.0);
    for (int c = 0; c < kDemandCats; ++c) in.tax[c] = g_simGrowth.offset[c];
}

void SimP2Demand(double minutes) {
    using namespace tbd::city;
    namespace s = sim_p2;
    const double t0 = g_sim.nowMs();
    SimP2BuildDemand(s::lastIn);
    s::lastDemand = ComputeDemand(s::lastIn, s::dp);
    SmoothDemand(s::dm.bars, s::lastDemand, s::dp, minutes);
    for (int c = 0; c < kDemandCats; ++c) {
        g_simGrowth.demand[c] = s::dm.bars[c], g_simGrowth.raw[c] = std::clamp(s::lastDemand.bar[c].raw, -1.0, 1.0);
        const int8_t zone = (int8_t)(s::dm.bars[c] < s::lots.p.badDemand ? -1 : s::dm.bars[c] > 0.0 ? 1 : 0);
        if (zone != s::dm.zone[c]) { // a threshold of this category was crossed: its lots re-judge (spread over ticks)
            if (s::dm.zone[c] != -2) s::lots.MarkCat(c);
            s::dm.zone[c] = zone;
        }
    }
    g_simGrowth.ApplyGate(g_zones, g_simTune);
    s::demandMs = g_sim.nowMs() - t0;
}

// ---- rent: one household group of 96 (a game day for all); the high rent flags at the end of the day
void SimP2RentGroup(int64_t minute) {
    using namespace tbd::city;
    namespace s = sim_p2;
    const double t0 = g_sim.nowMs();
    RentDay& d = s::dm.rent;
    if (SimP1On()) {
        const HouseholdStore& hh = g_life.w.hh;
        for (uint32_t hi = d.group ? (uint32_t)d.group : 96u; hi < hh.Capacity(); hi += 96) { // slot 0 is never a household
            if (!hh.At(hi).Valid() || hi >= hh.rentMonth.size() || !hh.home[hi]) continue;
            const bool b = RentBurdened(s::rp, hh.rentMonth[hi], hh.incomeMonth[hi]);
            const int band = std::clamp<int>(hh.wealth[hi], 0, 3);
            ++d.cur.households[band], d.cur.burdened[band] += b ? 1 : 0;
            d.cur.spend += hh.spendMonth[hi], d.cur.income += hh.incomeMonth[hi], d.cur.persons += hh.size[hi];
            d.Count(BuildingId(hh.home[hi]).Index(), b);
        }
    }
    if (++d.group >= 96) { // the day is complete: its numbers, and the flags of the buildings that changed
        d.group = 0, d.last = d.cur, d.cur = Affordability();
        uint64_t high = 0;
        for (uint32_t i = 1; i < g_simBuildings.Capacity(); ++i) {
            const BuildingId b = g_simBuildings.At(i);
            if (!g_simBuildings.Alive(b)) continue;
            const bool on = d.High(s::rp, i), was = g_simFlagTable.Has(b, kFlagHighRent);
            high += on;
            if (on == was) continue;
            g_simFlagTable.Write(b, kFlagHighRent, on ? kFlagHighRent : 0);
            if (on) g_simEvents.Push(SimEventType::HighRent, minute, b.v, 0, i < d.hit.size() ? d.hit[i] : 0);
        }
        s::highRentNow = high;
        std::fill(d.seen.begin(), d.seen.end(), (uint16_t)0), std::fill(d.hit.begin(), d.hit.end(), (uint16_t)0);
    }
    s::rentMs = g_sim.nowMs() - t0;
}

// ---- the hourly land system
void SimP2Hour(int64_t minute) {
    namespace s = sim_p2;
    const double t0 = g_sim.nowMs();
    if (s::job) SimP2FinishJob(), SimP2ApplyResult(); // the previous hour's pass lands now, never earlier or later
    static uint64_t rentFor = ~0ull;
    std::shared_ptr<s::Job> j = std::make_shared<s::Job>();
    SimP2BuildInput(minute, j->in);
    if (rentFor != s::densityFor) rentFor = s::densityFor, s::WriteAllCells(); // the density changed: every cell's rent
    j->base = s::cur;
    const double hours = g_simPhaseTune.land.checkHours;
    j->check = hours > 0 && (minute / 60) % std::max<int64_t>(1, (int64_t)hours) == 0;
    s::job = j, s::jobDone = false;
    const tbd::city::LandParams p = s::lp;
    g_engine.jobs->RunBackground([j, p] {
        const auto a = std::chrono::steady_clock::now();
        j->state = *j->base;
        j->state.minute = j->base->minute;
        tbd::city::LandStep(j->state, j->in, p, j->out);
        if (j->check) j->checkOk = tbd::city::LandSame(j->state, tbd::city::LandFull(j->in, p), j->in);
        j->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        j->ready.store(true, std::memory_order_release);
    });
    // the Demand panel's sparklines: one point a game hour
    for (int c = 0; c < tbd::city::kDemandCats; ++c) {
        s::history[c].push_back((float)s::dm.bars[c]);
        if (s::history[c].size() > 48) s::history[c].erase(s::history[c].begin());
    }
    s::landMs = g_sim.nowMs() - t0;
}

// The building inspector's rows (city_interact.inl): rent and levelling of a lot. False with P2 off.
bool SimLandInspect(uint64_t lot, std::vector<std::pair<std::string, std::string>>& rows) {
    using namespace tbd::city;
    namespace s = sim_p2;
    if (!SimP2On()) return false;
    const LotState* st = s::lots.Find(lot);
    int li = -1;
    for (size_t i = 0; i < g_zones.lots.size(); ++i) if (g_zones.lots[i].id == lot && g_zones.lots[i].alive) { li = (int)i; break; } // a click: O(lots) is fine
    if (li < 0) return false;
    const Lot& l = g_zones.lots[(size_t)li];
    const int ci = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
    const int land = s::cur->field.CellCount() ? s::cur->field.At(l.c.x, l.c.z) : s::CellValue(CellKeyAt(l.c.x, l.c.z));
    char b[160];
    const uint8_t burden = g_cellFactors.At(ci).rentBurden;
    std::snprintf(b, sizeof(b), "%d at the lot, %s; rent x%.2f of the flat rent", land, DensityName(DensityOf(l.type)), burden / 64.0);
    rows.push_back({"Land value", b});
    if (LotCapacity(l).homes && SimP1On()) { // the households' rent against their income (the records)
        const LifeWorld& w = g_life.w;
        int64_t rent = 0, income = 0;
        int n = 0, hit = 0;
        const BuildingId bid = s::LotBuilding(lot);
        const LifeBuilding* L = bid.Valid() && bid.Index() < w.bld.size() && w.bld[bid.Index()].id == bid.v ? &w.bld[bid.Index()] : nullptr;
        for (uint32_t h = L ? L->firstHousehold : 0; h; h = w.hh.nextInHome[LifeWorld::Ix(h)]) {
            const uint32_t hi = LifeWorld::Ix(h);
            rent += w.hh.rentMonth[hi], income += w.hh.incomeMonth[hi], ++n, hit += RentBurdened(s::rp, w.hh.rentMonth[hi], w.hh.incomeMonth[hi]);
        }
        if (n) std::snprintf(b, sizeof(b), "%lld a month per household (%.0f%% of income), %d of %d struggle", (long long)(rent / n), income ? 100.0 * rent / income : 0.0, hit, n);
        else std::snprintf(b, sizeof(b), "nobody pays rent here yet");
        rows.push_back({"Rent", b});
    }
    if (st) {
        static const char* const kCond[] = {"steady", "thriving", "declining"};
        const int64_t left = st->due >= 0 ? std::max<int64_t>(0, st->due - g_sim.Minute()) : -1;
        if (st->abandoned) std::snprintf(b, sizeof(b), "abandoned%s", left >= 0 ? (st->cond == kLotBad ? ", demolished soon" : ", recovering") : "");
        else if (left >= 0) std::snprintf(b, sizeof(b), "%s: %s in about %.1f days", kCond[st->cond], st->cond == kLotGood ? "levels up" : l.level > 1 ? "levels down" : "abandoned", left / 1440.0);
        else std::snprintf(b, sizeof(b), "%s%s (next level needs land value %d)", kCond[st->cond], st->maxed ? ", top level" : "", s::lots.p.UpLand(l.level));
        rows.push_back({"Development", b});
    }
    return true;
}

// ---- the Demand panel (gui/sim_panels.h): the four bars, what makes each, land and rent, levelling
void SimP2FillPanel(tbd::gui::SimPanelSpec& sp, void*) {
    using namespace tbd::city;
    namespace s = sim_p2;
    static const char* const kCat[kDemandCats] = {"Residential", "Commercial", "Office", "Industrial"};
    char b[128];
    sp.title = "Demand", sp.icon = "stats", sp.sub = "What the city wants built, and why";
    for (int c = 0; c < kDemandCats; ++c) {
        const double v = s::dm.bars[c];
        std::snprintf(b, sizeof(b), "%+.0f%%", v * 100.0);
        sp.Bar(kCat[c], (float)((v + 1.0) * 0.5), b, v > 0.05 ? 1 : v < -0.05 ? -1 : 0);
        std::vector<std::pair<std::string, std::string>> why;
        const DemandBar& bar = s::lastDemand.bar[c];
        for (int f = 0; f < bar.n; ++f) {
            if (std::fabs(bar.f[f].value) < 0.005) continue;
            std::snprintf(b, sizeof(b), "%+.0f", bar.f[f].value * 100.0);
            why.push_back({bar.f[f].name, b});
        }
        if (!why.empty()) sp.List(std::string("Why: ") + kCat[c], why, kDemandMaxFactors);
    }
    sp.Spark("Residential, last 48 hours", s::history[0], "");
    sp.Caption("Land and rent");
    std::snprintf(b, sizeof(b), "%.0f of 255", s::curResult.mean);
    sp.Bar("Land value, city mean", (float)(s::curResult.mean / 255.0), b, 0);
    const Affordability& a = s::dm.rent.last;
    const int32_t n = a.Households();
    int32_t hit = 0;
    for (int i = 0; i < 4; ++i) hit += a.burdened[i];
    std::snprintf(b, sizeof(b), "%d of %d households", hit, n);
    sp.Stat("Rent over 40% of income", b, n && hit * 5 > n ? -1 : 0);
    sp.Stat("Buildings with rent too high", std::to_string(s::highRentNow), s::highRentNow ? -1 : 0);
    sp.Caption("Buildings");
    std::snprintf(b, sizeof(b), "%llu up, %llu down", (unsigned long long)s::lots.levelUps, (unsigned long long)s::lots.levelDowns);
    sp.Stat("Level changes", b);
    std::snprintf(b, sizeof(b), "%d now, %llu recovered, %llu demolished", s::lots.AbandonedNow(), (unsigned long long)s::lots.recovered, (unsigned long long)s::lots.demolished);
    sp.Stat("Abandoned", b, s::lots.AbandonedNow() ? -1 : 0);
    std::snprintf(b, sizeof(b), "lots %.3f avg / %.3f worst, land %.2f + %.2f worker, demand %.3f, rent %.3f ms", s::lotsAvgMs, s::lotsWorstMs, s::landMs, s::stepMs,
                  s::demandMs, s::rentMs);
    sp.Caption(b);
    if (s::checkMismatch) sp.Stat("Debug check mismatches", std::to_string(s::checkMismatch), -1);
}

// ---- .tbdsim chunks: GROW LAND DMND (sim_land_save.h). A save mid-hour finishes the land pass in flight first and
// stores it as pending, so the loaded city applies it at the same hour.
void SimP2SaveLand(tbd::city::ByteWriter& w) {
    namespace s = sim_p2;
    SimP2FinishJob();
    tbd::city::WriteLandState(w, *s::cur);
    const bool pending = s::job && s::jobDone;
    w.U8(pending ? 1 : 0);
    if (!pending) return;
    tbd::city::WriteLandState(w, s::doneState);
    w.U32((uint32_t)s::doneResult.changed.size());
    for (const tbd::city::CellKey k : s::doneResult.changed) w.I32(k.x), w.I32(k.z);
    w.F64(s::doneResult.mean);
}
std::vector<tbd::city::CellKey> g_p2LoadedChanged;
double g_p2LoadedMean = 0;
bool SimP2LoadLand(tbd::city::ByteReader& r) {
    namespace s = sim_p2;
    if (!tbd::city::ReadLandState(r, s::loadedCur)) return false;
    s::loadedHasPending = r.U8() != 0;
    g_p2LoadedChanged.clear();
    if (!s::loadedHasPending) return r.ok;
    if (!tbd::city::ReadLandState(r, s::loadedPending)) return false;
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 8 > r.Remaining()) return false;
    for (uint32_t i = 0; i < n; ++i) { tbd::city::CellKey k; k.x = r.I32(), k.z = r.I32(); g_p2LoadedChanged.push_back(k); }
    g_p2LoadedMean = r.F64();
    return r.ok;
}
void SimP2SaveDemand(tbd::city::ByteWriter& w) {
    tbd::city::WriteDemand(w, sim_p2::dm);
    std::vector<uint32_t> closed(sim_p2::closed.begin(), sim_p2::closed.end());
    std::sort(closed.begin(), closed.end());
    w.PutVec(closed);
    std::vector<uint32_t> high; // the flags are not saved by the table: P2's high rent buildings come back from here
    for (uint32_t i = 1; i < g_simBuildings.Capacity(); ++i) {
        const tbd::city::BuildingId b = g_simBuildings.At(i);
        if (g_simBuildings.Alive(b) && g_simFlagTable.Has(b, tbd::city::kFlagHighRent)) high.push_back(b.v);
    }
    w.PutVec(high);
}
std::vector<uint32_t> g_p2LoadedHigh;
bool SimP2LoadDemand(tbd::city::ByteReader& r) {
    if (!tbd::city::ReadDemand(r, sim_p2::dm)) return false;
    std::vector<uint32_t> closed;
    if (!r.GetVec(closed)) return false;
    sim_p2::closed = std::unordered_set<uint32_t>(closed.begin(), closed.end());
    return r.GetVec(g_p2LoadedHigh);
}

// ---- the five hooks (frozen signatures, called by city_sim_phases.inl)
namespace sim_p2 {
size_t writtenCells = 0; // CitySim cells that got their land value and rent (new cells get theirs in SimP2Frame)
void FreshLots() { lots.Clear(), lotBits.clear(), bitsUtil = bitsTable = ~0ull, diffCursor = ~(size_t)0; }
void FreshLand() {
    cur = std::make_shared<LandState>(), curResult = LandResult(), job.reset(), jobDone = false, doneState.Clear(), doneResult = LandResult();
    waterView.clear(), cellDensity.clear(), cellRoad.clear(), densityFor = ~0ull, pendFor = ~0ull, writtenCells = 0;
    for (CellFactor& f : g_cellFactors.cells) f.rentBurden = 64; // P2's column only
    ++g_cellFactors.version;
    g_simFlagTable.ClearAll(SimFlagPhaseMask(2));
}
void FreshDemand() {
    dm = DemandSaveState(), lastDemand = DemandResult(), lastIn = DemandInput(), closed.clear();
    for (auto& h : history) h.clear();
}
} // namespace sim_p2

void SimP2Init() {
    namespace s = sim_p2;
    SimP2ApplyTuning(g_simPhaseTune.land);
    s::cursor = g_simEvents.Now();
    // one minute system each, offset inside the hour so no two heavy steps share a tick: lots every minute, demand at
    // minute 4 of every 10, rent groups at minute 11 of every 15, the land pass at minute 30 of the hour
    g_sim.Register("land.lots", 1, 1, [](const tbd::city::SimTick& t) { SimP2LotsTick(t.minute); });
    g_sim.Register("demand", 1, 1, [](const tbd::city::SimTick& t) { if (t.minute % 10 == 4) SimP2Demand(10.0); });
    g_sim.Register("land.rent", 1, 1, [](const tbd::city::SimTick& t) { if (t.minute % 15 == 11) SimP2RentGroup(t.minute); });
    g_sim.Register("land", 1, 1, [](const tbd::city::SimTick& t) { if (t.minute % 60 == 30) SimP2Hour(t.minute); });
    tbd::Log("Sim P2: demand from causes, land value field (%.0f m), rent, levelling with abandonment up", (double)tbd::city::kFieldTile);
}

void SimP2Frame(float /*realDt*/) {
    namespace s = sim_p2;
    s::ReadEvents();
    if (g_citySim.sim.size() < s::writtenCells) s::writtenCells = 0; // the cells were cleared
    if (g_citySim.sim.size() > s::writtenCells) {
        for (size_t c = s::writtenCells; c < g_citySim.sim.size(); ++c) s::WriteCell((int)c);
        s::writtenCells = g_citySim.sim.size(), ++g_cellFactors.version;
    }
}

void SimP2Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    namespace s = sim_p2;
    SimChunkDef grow, land, dmnd;
    grow.fourcc = simchunk::GROW, grow.version = kLandGrowVersion;
    grow.save = [](ByteWriter& w) { WriteLots(w, s::lots); };
    grow.load = [](ByteReader& r, uint16_t) { return ReadLots(r, s::lots); };
    grow.fresh = [] { s::FreshLots(); };
    land.fourcc = simchunk::LAND, land.version = kLandLandVersion;
    land.save = [](ByteWriter& w) { SimP2SaveLand(w); };
    land.load = [](ByteReader& r, uint16_t) { return SimP2LoadLand(r); };
    land.fresh = [] { s::FreshLand(); };
    dmnd.fourcc = simchunk::DMND, dmnd.version = kLandDemandVersion;
    dmnd.save = [](ByteWriter& w) { SimP2SaveDemand(w); };
    dmnd.load = [](ByteReader& r, uint16_t) { return SimP2LoadDemand(r); };
    dmnd.fresh = [] { s::FreshDemand(); };
    reg.Register(grow), reg.Register(land), reg.Register(dmnd);
}

// After a reset or a load. A save from before P2 (no chunks): the M record's demand and counters carry on.
void SimP2Load(const tbd::city::SimLoadReport& rep) {
    using namespace tbd::city;
    namespace s = sim_p2;
    auto ok = [&](uint32_t f) { const SimChunkResult* c = rep.Find(f); return c && c->status == SimChunkStatus::Loaded; };
    s::cursor = g_simEvents.Now();
    if (ok(simchunk::LAND)) {
        s::cur = std::make_shared<LandState>(std::move(s::loadedCur));
        s::curResult = LandResult();
        if (s::loadedHasPending) {
            s::job = std::make_shared<s::Job>(), s::jobDone = true;
            s::doneState = std::move(s::loadedPending), s::doneResult.changed = g_p2LoadedChanged, s::doneResult.mean = g_p2LoadedMean;
        }
        s::writtenCells = 0; // every cell writes its value as the cells come back
    }
    s::loadedCur.Clear(), s::loadedPending.Clear(), s::loadedHasPending = false;
    if (ok(simchunk::DMND)) for (int c = 0; c < kDemandCats; ++c) g_simGrowth.demand[c] = s::dm.bars[c];
    else for (int c = 0; c < kDemandCats; ++c) s::dm.bars[c] = g_simGrowth.demand[c];
    for (uint32_t b : g_p2LoadedHigh) g_simFlagTable.Write(BuildingId(b), kFlagHighRent, kFlagHighRent);
    g_p2LoadedHigh.clear();
    for (const auto& kv : s::lots.st) // P2's abandoned bit from the lot states
        if (kv.second.abandoned) g_simFlagTable.Write(s::LotBuilding(kv.first), kFlagAbandoned, kFlagAbandoned);
    if (ok(simchunk::GROW)) g_simGrowth.levelUps = s::lots.levelUps, g_simGrowth.levelDowns = s::lots.levelDowns, g_simGrowth.abandoned = s::lots.abandonedTotal;
    else s::lots.levelUps = g_simGrowth.levelUps, s::lots.levelDowns = g_simGrowth.levelDowns, s::lots.abandonedTotal = g_simGrowth.abandoned;
    if (ok(simchunk::GROW) || ok(simchunk::LAND)) tbd::Log("Sim P2: %d lots, %zu land cells loaded%s", s::lots.Tracked(), s::cur->value.size(), s::job ? ", a land pass pending" : "");
}

void SimP2Panels() {
    tbd::gui::SimPanelEntry e;
    e.id = "p2.demand", e.name = "Demand", e.icon = "stats", e.fill = SimP2FillPanel;
    tbd::gui::RegisterSimPanel(e);
}
