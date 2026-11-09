// ------------------------------------------------------------------- city sim Phase 3: environment: pollution, weather, seasons, water table, floods
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 3 owns this file and src/city/sim_env*.h (headless,
// tests\sim_env_*_test). The hub calls the five hooks at the bottom (frozen signatures).
//   env.weather  every 10 game minutes: the seeded weather chain (sim_env_weather.h), season and storm events
//   env          every game hour: snapshot the sources (industry lots and hubs, power plants, sewage outlets, landfill,
//                roads by flow), pumps and city cells, run EnvSim::Step on a worker, and apply the PREVIOUS hour's result
//                (factors air noise ground water, flags polluted noisy flooded, flood events). A pass the worker has not
//                finished by then is computed on the main thread from the same input, so the result never depends on timing.
// Tables P3 writes: CellFactors (its four columns), SimFlagTable (its bits), SimEventRing (flood, storm, season). The hub
// has no shared instances of those yet, so they are P3-local here behind three pointers (g_p3Factors, g_p3Flags,
// g_p3Events); a Phase 0 follow-up points them at the shared ones.

// game_core.inl (and so this file) sits inside `namespace {` in main.cpp, game_host.cpp and probe_game.cpp, and those
// translation units do not include the P3 headers yet. The headers must be seen at file scope (they open tbd::city), so
// the anonymous namespace is closed around the includes and reopened. Phase 0 follow-up: include "city/sim_env_save.h"
// in the three translation units beside sim_save.h, then these lines go (the includes become no-ops meanwhile).
} // namespace
#include <atomic>
#include <memory>

#include "city/sim_env_save.h"
namespace {

namespace sim_p3 {
using namespace tbd::city;
EnvParams params;
EnvSim cur;               // the applied state
EnvResult curResult;      // what the factors, flags and overlays show
std::unordered_map<uint64_t, int> resultIndex; // PackKey(cell) -> index into curResult (overlays, inspector)
EnvWeatherState wx;       // chain, last season, last kind (WTHR)
EnvWeather weather;       // now (every 10 game minutes)
CellFactors factorsLocal;
SimFlagTable flagsLocal;
SimEventRing eventsLocal;
std::vector<float> airHistory; // the city's mean air, one per game hour (the panel's sparkline), at most 48
double buildMs = 0, stepMs = 0, applyMs = 0; // last hourly snapshot, worker pass, apply (main-thread ms)
int syncFallbacks = 0;    // passes the worker had not finished in time (computed on the main thread)
uint32_t terrainVersion = ~0u;
bool resampleAll = true;  // the next pass re-samples every height (a load, a terrain edit)

// One pass in flight. The worker reads `in` and writes state / out; the main thread only reads `in` until ready.
struct Job {
    EnvInput in;
    EnvSim state;
    EnvResult out;
    double ms = 0;
    std::atomic<bool> ready{false};
};
std::shared_ptr<Job> job;   // null: nothing in flight
bool jobDone = false;       // job's state / out are final (the worker finished, or the main thread computed them)
EnvSim doneState;           // when jobDone and the worker's copy was not used
EnvResult doneResult;
EnvLoadInto loaded;         // what the chunks read (SimP3Load moves it in)
bool loadedPoll = false, loadedWater = false;

uint64_t Seed() { return (uint64_t)(uint32_t)g_engine.terrain.params.seed; }
float Sea() { return g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1 ? tbd::IslandMap::kSeaLevel : -1e9f; }
} // namespace sim_p3

tbd::city::CellFactors* g_p3Factors = &sim_p3::factorsLocal; // Phase 0 follow-up: the hub's shared CellFactors
tbd::city::SimFlagTable* g_p3Flags = &sim_p3::flagsLocal;     // ... its SimFlagTable (by BuildingId)
tbd::city::SimEventRing* g_p3Events = &sim_p3::eventsLocal;   // ... its SimEventRing

// Getters for the other systems (utilities.h output, the clock chip, the inspector): read only.
const tbd::city::EnvWeather& SimEnvWeather() { return sim_p3::weather; }
float SimEnvSolarFactor() { return sim_p3::weather.solar; }    // 0..1, 0 at night
float SimEnvWindFactor() { return sim_p3::weather.windPower; } // 0..1, the turbine curve
// The P3 factors of a CitySim cell (neutral when none): the inspector's pollution row.
tbd::city::EnvCellOut SimEnvCell(int cell) {
    if (cell < 0 || (size_t)cell >= g_citySim.cells.cells.size()) return tbd::city::EnvCellOut();
    const auto it = sim_p3::resultIndex.find(tbd::city::PackKey(g_citySim.cells.cells[(size_t)cell].key));
    return it == sim_p3::resultIndex.end() ? tbd::city::EnvCellOut() : sim_p3::curResult.cells[(size_t)it->second];
}

// ---- env.weather: the chain to the step of this minute, the weather now, season and storm events
void SimP3Weather(int64_t minute) {
    using namespace tbd::city;
    namespace s = sim_p3;
    const int64_t step = SimCalendar::FloorDiv(minute, kWeatherStepMinutes);
    if (s::wx.chain.step > step) s::wx.chain = WeatherChain(); // the clock went back (a reset): replay from 0
    s::wx.chain = WeatherChain::Replay(step, s::Seed(), g_simCalendar, &s::wx.chain);
    s::weather = WeatherAt(s::wx.chain, minute, s::Seed(), g_simCalendar);
    if (s::wx.lastSeason >= 0 && s::weather.season != s::wx.lastSeason) g_p3Events->Push(SimEventType::SeasonChanged, minute, kNone, kNone, s::weather.season);
    if (s::wx.lastSeason >= 0 && s::weather.kind == kWxStorm && s::wx.lastKind != kWxStorm) g_p3Events->Push(SimEventType::Storm, minute, kNone, kNone, (int64_t)(s::weather.rainMmH * 10));
    s::wx.lastSeason = s::weather.season, s::wx.lastKind = s::weather.kind;
}

// ---- the hourly snapshot (main thread): sources, pumps, city cells, heights of new cells
void SimP3BuildInput(int64_t minute, tbd::city::EnvInput& in) {
    using namespace tbd::city;
    namespace s = sim_p3;
    in = EnvInput();
    in.minute = minute, in.sea = s::Sea();
    in.weather = WeatherAt(s::wx.chain, minute, s::Seed(), g_simCalendar);
    const auto& types = ZoneTypes();
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        if (types[l.type].cat == ZoneCat::Industrial) in.sources.push_back(EnvIndustryLotSource(l.c.x, l.c.z, l.level, l.w * l.d));
        else if (types[l.type].cat == ZoneCat::Commercial) in.sources.push_back(EnvCommercialLotSource(l.c.x, l.c.z, l.w * l.d));
    }
    for (const IndSite& site : g_ind.sites) in.sources.push_back(EnvIndustrySiteSource(site.c.x, site.c.z, (int)site.kind, site.eff));
    const auto& defs = UtilDefs();
    for (const UtilBuilding& b : g_util.items) {
        if (!b.alive || b.def < 0 || b.def >= (int)defs.size()) continue;
        const EnvSource src = EnvUtilitySource(b.c.x, b.c.z, b.def);
        if (src.air > 0 || src.noise > 0 || src.water > 0) in.sources.push_back(src);
        if (defs[(size_t)b.def].kind == Util::Water) in.pumps.push_back({b.c.x, b.c.z, defs[(size_t)b.def].supply, !UtilDefShore(b.def)});
    }
    // roads: one source per 32 m tile a loaded segment crosses (the loudest sample of the tile, the sum of its exhaust)
    const Network& net = g_roads.net;
    if (g_segFlowVersion == net.version && g_segFlow.size() == net.segs.size()) {
        struct RoadTile { float noise = 0, air = 0, x = 0, z = 0; uint8_t r = 1; };
        std::unordered_map<uint64_t, RoadTile> tiles;
        std::vector<uint64_t> order; // first-seen order: deterministic, whatever the hash map does
        for (size_t si = 0; si < net.segs.size(); ++si) {
            const Segment& sg = net.segs[si];
            const SegFlow& f = g_segFlow[si];
            if (!sg.alive || f.flow <= 1.0f) continue;
            const Cubic cv = net.Curve(sg);
            const float len = Len(cv.p1 - cv.p0) + Len(cv.p2 - cv.p1) + Len(cv.p3 - cv.p2);
            const int n = std::max(1, (int)(len / kFieldTile));
            const EnvSource rs = EnvRoadSource(0, 0, f.flow, f.speedRatio);
            for (int i = 0; i < n; ++i) {
                const V2 p = cv.At((i + 0.5f) / n);
                const int gx = (int)std::floor(p.x / kFieldTile), gz = (int)std::floor(p.z / kFieldTile);
                const uint64_t key = PackKey({gx, gz});
                auto [it, fresh] = tiles.try_emplace(key);
                if (fresh) order.push_back(key), it->second.x = (gx + 0.5f) * kFieldTile, it->second.z = (gz + 0.5f) * kFieldTile;
                it->second.noise = std::max(it->second.noise, rs.noise), it->second.air += rs.air, it->second.r = std::max(it->second.r, rs.noiseR);
            }
        }
        for (uint64_t key : order) {
            const RoadTile& t = tiles[key];
            EnvSource src;
            src.x = t.x, src.z = t.z, src.noise = t.noise, src.air = EnvQuant(t.air, 0.25f), src.noiseR = t.r;
            in.sources.push_back(src);
        }
    }
    // the city's cells, their water service, and the terrain under the new ones (all after a load or an edit)
    const uint32_t tv = g_engine.terrain.field.Version();
    if (tv != s::terrainVersion) s::terrainVersion = tv, s::resampleAll = true;
    const size_t n = std::min(g_citySim.sim.size(), g_citySim.cells.cells.size());
    in.cityKeys.resize(n), in.cityWaterOk.resize(n);
    for (size_t c = 0; c < n; ++c) {
        const CellKey k = g_citySim.cells.cells[c].key;
        in.cityKeys[c] = k, in.cityWaterOk[c] = g_citySim.sim[c].waterOk ? 1 : 0;
        if (!s::resampleAll && s::cur.water.table.FindCell(k)) continue;
        auto& t = in.heights.Cell(k);
        for (int i = 0; i < kFieldRes * kFieldRes; ++i)
            t[(size_t)i] = g_engine.terrain.HeightAt((k.x * kFieldRes + i % kFieldRes + 0.5f) * kFieldTile, (k.z * kFieldRes + i / kFieldRes + 0.5f) * kFieldTile);
    }
    s::resampleAll = false;
}

// ---- the hourly pass
// The in-flight pass, finished: the worker's result if it is ready, else computed here from the same input and state.
void SimP3FinishJob() {
    namespace s = sim_p3;
    if (!s::job || s::jobDone) return;
    if (s::job->ready.load(std::memory_order_acquire)) {
        s::doneState = std::move(s::job->state), s::doneResult = std::move(s::job->out), s::stepMs = s::job->ms;
    } else {
        const double t0 = g_sim.nowMs();
        s::doneState = s::cur;
        s::doneState.Step(s::job->in, s::params, s::doneResult);
        s::stepMs = g_sim.nowMs() - t0, ++s::syncFallbacks;
        // the worker keeps its own reference and finishes on it; its result is never read
    }
    s::jobDone = true;
}

// curResult onto the tables: factors (by key: after a load the CitySim cells come back in another order), flags, the
// overlays' key index.
void SimP3ShowResult() {
    using namespace tbd::city;
    namespace s = sim_p3;
    const auto& cells = g_citySim.cells.cells;
    WriteEnvFactors(s::curResult, *g_p3Factors, [&](CellKey k, size_t i) {
        if (i < cells.size() && cells[i].key == k) return (int)i; // the usual case: the same order as at the pass
        return g_citySim.cells.Find(k);
    });
    WriteEnvFlags(s::curResult, g_simBuildings, *g_p3Flags);
    s::resultIndex.clear();
    for (size_t i = 0; i < s::curResult.keys.size(); ++i) s::resultIndex[PackKey(s::curResult.keys[i])] = (int)i;
    ++tbd::gui::OverlayProvidersVersion(); // the overlays re-read their cells
}

// The finished pass becomes the applied state: factors, flags, events, overlays, the panel's history.
void SimP3ApplyResult(bool events) {
    using namespace tbd::city;
    namespace s = sim_p3;
    const double t0 = g_sim.nowMs();
    s::cur = std::move(s::doneState), s::curResult = std::move(s::doneResult);
    s::job.reset(), s::jobDone = false;
    SimP3ShowResult();
    if (events) PushEnvEvents(s::curResult, *g_p3Events);
    s::airHistory.push_back((float)s::curResult.meanAir);
    if (s::airHistory.size() > 48) s::airHistory.erase(s::airHistory.begin());
    s::applyMs = g_sim.nowMs() - t0;
}

void SimP3Hour(int64_t minute) {
    namespace s = sim_p3;
    if (s::job) SimP3FinishJob(), SimP3ApplyResult(true); // the previous hour's pass lands now, never earlier or later
    const double t0 = g_sim.nowMs();
    std::shared_ptr<s::Job> j = std::make_shared<s::Job>();
    SimP3BuildInput(minute, j->in);
    j->state = s::cur;
    s::buildMs = g_sim.nowMs() - t0;
    s::job = j, s::jobDone = false;
    const tbd::city::EnvParams p = s::params;
    g_engine.jobs->RunBackground([j, p] {
        const auto a = std::chrono::steady_clock::now();
        j->state.Step(j->in, p, j->out);
        j->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        j->ready.store(true, std::memory_order_release);
    });
}

// A city reset (new map, load): everything P3 holds starts over; a pass in flight is dropped (the worker finishes on its
// own reference).
void SimP3Fresh() {
    namespace s = sim_p3;
    s::cur.Clear(), s::curResult = tbd::city::EnvResult(), s::resultIndex.clear();
    s::job.reset(), s::jobDone = false, s::doneState.Clear(), s::doneResult = tbd::city::EnvResult();
    s::airHistory.clear(), s::resampleAll = true;
    for (tbd::city::CellFactor& f : g_p3Factors->cells) f.air = f.noise = f.ground = f.water = 0; // P3's columns only
    ++g_p3Factors->version;
    g_p3Flags->ClearAll(tbd::city::SimFlagPhaseMask(3));
}

// ---- the four pollution overlays (gui/city_overlays.h providers): the applied result per CitySim cell, 0..1
template <int Which>
float SimP3OverlayCell(void*, int, float x, float z) {
    const auto it = sim_p3::resultIndex.find(tbd::city::PackKey(tbd::city::CellKeyAt(x, z)));
    if (it == sim_p3::resultIndex.end()) return -1.0f;
    const tbd::city::EnvCellOut& o = sim_p3::curResult.cells[(size_t)it->second];
    const int v = Which == 0 ? o.air : Which == 1 ? o.noise : Which == 2 ? o.ground : o.water;
    return std::min(1.0f, v / 128.0f); // 128 factor steps and up is full red (the flag limits sit around half of it)
}
void SimP3RegisterOverlays() {
    using tbd::gui::Overlay;
    tbd::gui::RegisterOverlayProvider(Overlay::AirPollution, SimP3OverlayCell<0>, nullptr, "Clean  ..  Polluted (drifts with the wind)");
    tbd::gui::RegisterOverlayProvider(Overlay::NoisePollution, SimP3OverlayCell<1>, nullptr, "Quiet  ..  Loud");
    tbd::gui::RegisterOverlayProvider(Overlay::GroundPollution, SimP3OverlayCell<2>, nullptr, "Clean  ..  Polluted (lingers)");
    tbd::gui::RegisterOverlayProvider(Overlay::WaterPollution, SimP3OverlayCell<3>, nullptr, "Clean  ..  Polluted tap water");
}

// ---- the Environment panel (gui/sim_panels.h)
void SimP3FillPanel(tbd::gui::SimPanelSpec& sp, void*) {
    using namespace tbd::city;
    using tbd::gui::SimRow;
    namespace s = sim_p3;
    const EnvWeather& w = s::weather;
    const EnvResult& r = s::curResult;
    static const char* const kDir[8] = {"E", "SE", "S", "SW", "W", "NW", "N", "NE"}; // towards; +Z is south on the map
    char b[96];
    sp.title = "Environment", sp.icon = "pollution";
    const SimDate d = g_simCalendar.At(g_sim.Minute());
    std::snprintf(b, sizeof(b), "%s, %s  -  %s", SeasonName(w.season), MonthName(d.monthOfYear), WeatherName(w.kind, w.snow));
    sp.sub = b;
    std::snprintf(b, sizeof(b), "%.1f C", w.tempC), sp.Add(SimRow::Stat, "Temperature").value = b;
    if (w.rainMmH > 0) std::snprintf(b, sizeof(b), "%.1f mm/h%s", w.rainMmH, w.snow ? " (snow)" : "");
    else std::snprintf(b, sizeof(b), "dry");
    sp.Add(SimRow::Stat, "Precipitation").value = b;
    std::snprintf(b, sizeof(b), "%.1f m/s towards %s", w.windMs, kDir[w.windDir & 7]), sp.Add(SimRow::Stat, "Wind").value = b;
    auto bar = [&](const char* label, float t, const char* fmt, double v, int tone) {
        auto& row = sp.Add(SimRow::Bar, label);
        row.t = std::clamp(t, 0.0f, 1.0f), row.tone = tone;
        std::snprintf(b, sizeof(b), fmt, v), row.value = b;
    };
    bar("Solar output", w.solar, "%.0f %%", w.solar * 100.0, 0);
    bar("Wind turbine output", w.windPower, "%.0f %%", w.windPower * 100.0, 0);
    sp.Add(SimRow::Caption, "Pollution, city average (0 .. 255)");
    bar("Air", (float)(r.meanAir / 128.0), "%.0f", r.meanAir, r.meanAir >= 45 ? -1 : 0);
    bar("Noise", (float)(r.meanNoise / 128.0), "%.0f", r.meanNoise, r.meanNoise >= 40 ? -1 : 0);
    bar("Ground", (float)(r.meanGround / 128.0), "%.0f", r.meanGround, r.meanGround >= 40 ? -1 : 0);
    bar("Tap water", r.supply / 128.0f, "%.0f", (double)r.supply, r.supply >= s::params.waterLimit ? -1 : 0);
    auto& sl = sp.Add(SimRow::Sparkline, "Air, last 48 hours");
    sl.series = s::airHistory;
    std::snprintf(b, sizeof(b), "%.1f", s::airHistory.empty() ? 0.0 : (double)s::airHistory.back()), sl.value = b;
    sp.Add(SimRow::Caption, "Water");
    bar("Groundwater", (float)(r.meanTable / 10000.0), "%.0f %%", r.meanTable / 100.0, r.meanTable < 3000 ? -1 : 0);
    bar("Groundwater pumps", (float)r.pumpEff, "%.0f %%", r.pumpEff * 100.0, r.pumpEff < 0.7 ? -1 : 0);
    auto stat = [&](const char* label, int v, int badAt) {
        auto& row = sp.Add(SimRow::Stat, label);
        row.value = std::to_string(v), row.tone = v >= badAt ? -1 : 0;
    };
    stat("Flooded areas", r.flooded, 1);
    stat("Polluted areas", r.polluted, 1);
    stat("Noisy areas", r.noisy, 1);
    std::snprintf(b, sizeof(b), "%.2f / %.2f / %.2f ms", s::buildMs, s::stepMs, s::applyMs);
    sp.Add(SimRow::Stat, "Hourly cost (snapshot / pass / apply)").value = b;
}

// ---- .tbdsim chunks. A save in the middle of an hour finishes the pass in flight first (SimP3FinishJob: the worker's
// result or the same pass computed here) and stores it as pending, so the loaded city applies it at the same hour.
tbd::city::EnvSaveView SimP3SaveView() {
    namespace s = sim_p3;
    SimP3FinishJob();
    tbd::city::EnvSaveView v{&s::cur, &s::curResult, nullptr, nullptr};
    if (s::job && s::jobDone) v.pending = &s::doneState, v.pendingResult = &s::doneResult;
    return v;
}

// ---- the five hooks (frozen signatures, called by city_sim_phases.inl)
void SimP3Init() {
    namespace s = sim_p3;
    s::params = tbd::city::EnvParams(); // the compiled table (sim_env.h); only env.enabled comes from tuning.json so far
    g_sim.Register("env.weather", 10, 1, [](const tbd::city::SimTick& t) { SimP3Weather(t.minute); });
    g_sim.Register("env", 60, 1, [](const tbd::city::SimTick& t) { SimP3Hour(t.minute); });
    SimP3RegisterOverlays();
    tbd::Log("Sim P3: environment up (pollution fields on %.0f m tiles, weather chain, water table, floods)", (double)tbd::city::kFieldTile);
}

void SimP3Frame(float /*realDt*/) {} // all of P3 runs in its two scheduler systems

void SimP3Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    namespace s = sim_p3;
    SimChunkDef poll;
    poll.fourcc = simchunk::POLL, poll.version = kEnvPollVersion;
    poll.save = [](ByteWriter& w) { EnvSavePoll(w, SimP3SaveView()); };
    poll.load = [](ByteReader& r, uint16_t) { return EnvLoadPoll(r, s::loaded); };
    poll.fresh = [] { SimP3Fresh(); };
    reg.Register(poll);
    SimChunkDef wthr;
    wthr.fourcc = simchunk::WTHR, wthr.version = kEnvWeatherVersion;
    wthr.save = [](ByteWriter& w) { EnvSaveWeather(w, s::wx); };
    wthr.load = [](ByteReader& r, uint16_t) { return EnvLoadWeather(r, s::wx); };
    wthr.fresh = [] { s::wx = EnvWeatherState(); }; // the next env.weather tick replays the chain from step 0
    reg.Register(wthr);
    SimChunkDef watr;
    watr.fourcc = simchunk::WATR, watr.version = kEnvWaterVersion;
    watr.save = [](ByteWriter& w) { EnvSaveWater(w, SimP3SaveView()); };
    watr.load = [](ByteReader& r, uint16_t) { return EnvLoadWater(r, s::loaded); };
    watr.fresh = [] { SimP3Fresh(); };
    reg.Register(watr);
}

// After a reset or a load: what the chunks read becomes the applied state (and the pass in flight, if both halves came
// back); the factors and flags show it at once, without events (they happened before the save).
void SimP3Load(const tbd::city::SimLoadReport& rep) {
    using namespace tbd::city;
    namespace s = sim_p3;
    auto ok = [&](uint32_t f) { const SimChunkResult* c = rep.Find(f); return c && c->status == SimChunkStatus::Loaded; };
    const bool poll = ok(simchunk::POLL), watr = ok(simchunk::WATR);
    if (poll || watr) {
        if (poll) s::cur.poll = std::move(s::loaded.cur.poll), s::curResult = std::move(s::loaded.curResult);
        if (watr) s::cur.water = std::move(s::loaded.cur.water), s::cur.wasFlooded = std::move(s::loaded.cur.wasFlooded), s::cur.lastMinute = s::loaded.cur.lastMinute;
        SimP3ShowResult();
        if (poll && watr && s::loaded.hasPollPending && s::loaded.hasWaterPending) { // the pass that was in flight lands next hour
            auto j = std::make_shared<s::Job>();
            s::doneState = std::move(s::loaded.pending), s::doneResult = std::move(s::loaded.pendingResult);
            s::job = j, s::jobDone = true;
        }
        s::resampleAll = true;
        tbd::Log("Sim P3: environment loaded (%d air cells, %d water cells%s)", s::cur.poll.air.CellCount(), s::cur.water.table.CellCount(), s::job ? ", a pass pending" : "");
    }
    s::loaded = EnvLoadInto();
    s::weather = WeatherAt(s::wx.chain, g_sim.Minute(), s::Seed(), g_simCalendar);
}

void SimP3Panels() {
    tbd::gui::SimPanelEntry e;
    e.id = "p3.environment", e.name = "Environment", e.icon = "pollution", e.fill = SimP3FillPanel;
    tbd::gui::RegisterSimPanel(e);
}
