// ------------------------------------------------------------------- city simulation host (src/city/sim.h)
// Included by main.cpp after the other city .inl files. SimSync runs once per frame after GarbageSync and is the only
// place that advances the new sim systems (Track S). It does not own the game clock: ZonesSync still advances
// g_clock.minutes, and SimSync feeds the scheduler the change in g_clock.minutes since the last frame, so the sim minute
// and the HUD clock cannot drift apart. A jump that is not the normal per-frame advance (a save loaded, a new map) rebases
// the scheduler instead of queueing thousands of minutes.
// Since the CS2 sim roadmap's Phase 0 this file keeps the scheduler, the cells, the people, the save hub (M, Q) and
// SimSync; services, budget, flow and the growth registrations are in city_sim_services / _budget / _flow / _growth.inl
// (included below), and the phase hub with the .tbdsim save is city_sim_phases.inl (included after this file).
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
tbd::city::PhaseTuning g_simPhaseTune; // the CS2 phases' prefixed values from the same file (sim_tuning_table.h)

// The CS2 phase hub (city_sim_phases.inl, included right after this file).
void SimPhasesInit();
void SimPhasesReset();
void SimPhasesAfterReset();
void SimPhasesFrame(float realDt);
bool SimTbdLoadIsPending(); // city_sim_save.inl: a load's .tbdsim waits to be applied
void SimBuildingsLotsBack();   // city_sim_save.inl: the load's lots are back (BLDG releases what never came back)
// Phase 1 (city_sim_p1.inl): the citizen records own who lives and works where once it is on.
bool SimP1On();
bool SimP1LotLines(uint64_t lot, std::string& title, std::vector<std::string>& lines);
void SimP1LotsBack();
void SimP1SavedPerson(uint64_t homeLot);
bool g_simLoadInProgress = false; // SimSync: the reset being settled came with a load (P1 bootstraps after its lots)

void SimLoadTuning() {
    g_simTune = tbd::city::SimTuning();
    g_simPhaseTune = tbd::city::PhaseTuning();
    const std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\sim\\tuning.json";
    std::vector<std::string> unknown;
    int applied = 0;
    if (g_simTune.LoadFile(path.c_str(), &unknown, &applied)) {
        tbd::Log("Sim: tuning.json applied %d values", applied);
        for (const std::string& u : unknown)
            if (!tbd::city::IsPhaseTuningKey(u)) tbd::Log("Sim: tuning.json: unknown name \"%s\" (ignored)", u.c_str()); // prefixed: a phase's
        std::vector<std::string> phaseUnknown;
        int phaseApplied = 0;
        g_simPhaseTune.LoadFile(path, &phaseUnknown, &phaseApplied);
        if (phaseApplied) tbd::Log("Sim: tuning.json applied %d phase values", phaseApplied);
        for (const std::string& u : phaseUnknown) {
            tbd::city::LifeTuning life; // P1's full set (sim_life_tuning.h; city_sim_p1.inl reads it): not unknown
            if (!life.Set(u.c_str(), 0.0)) tbd::Log("Sim: tuning.json: unknown phase name \"%s\" (ignored)", u.c_str());
        }
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

#include "city_sim_services.inl" // S3 utilities into the cells, flags, service coverage (Phase 4 owns it)
#include "city_sim_budget.inl"   // S4 economy, budget, construction charges, milestones, the Budget panel (Phase 7)
#include "city_sim_flow.inl"     // S5 flow traffic, S7 cosmetic cars (Phase 5)
#include "city_sim_growth.inl"   // S2 demand and growth registrations (Phase 2)


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
    if (SimP1LotLines(lot, g_simCitizenTitle, g_simCitizenLines)) return; // the records (Phase 1)
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

void SimLotsRestored() { g_simPending.lotsBack = true, SimBuildingsLotsBack(); }
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
    if (SimP1On()) { // the records move in (P1 bootstrap); the saved people are pinned onto them
        for (const auto& f : s.people) SimP1SavedPerson(strtoull(f[3].c_str(), nullptr, 10));
        s.people.clear();
        g_citySim.Sync(g_zones.lots, g_zones.version);
        SimP1LotsBack();
        return;
    }
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
    SimPhasesReset(); // every .tbdsim chunk's owner starts fresh (city_sim_phases.inl)
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
    SimLoadTuning();
    SimRegisterGrowth(); // S2: demand and growth (city_sim_growth.inl)
    // S6: named people follow what the counters did (building gone, job gone), idle ones collapse; every 10 minutes.
    g_sim.Register("people", 10, 1, [](const tbd::city::SimTick& t) { g_simPeople.Sync(g_citySim, t.minute); });
    SimRegisterFlow();     // S5 (city_sim_flow.inl)
    SimRegisterEconomy();  // S4 (city_sim_budget.inl)
    SimRegisterServices(); // S3 (city_sim_services.inl)
    const double stressMs = sim_host::EnvDouble("TBD_SIM_SCHED_STRESS", 0.0);
    if (stressMs > 0.0)
        g_sim.Register("stress", 1, 1, [stressMs](const tbd::city::SimTick&) {
            const double a = g_sim.nowMs();
            while (g_sim.nowMs() - a < stressMs) {}
        });
    SimPhasesInit(); // the CS2 phases (city_sim_phases.inl): P0's chunks, then P1 .. P7
}

double g_simTotalMs = 0.0, g_simTotalAvgMs = 0.0; // all of SimSync per frame (F3 "SIM TOTAL")
void SimSyncFrame(float realDt);
void SimSync(float realDt) {
    const double t0 = g_sim.nowMs();
    SimSyncFrame(realDt);
    g_simTotalMs = g_sim.nowMs() - t0;
    g_simTotalAvgMs += (g_simTotalMs - g_simTotalAvgMs) * 0.05;
}

void SimSyncFrame(float realDt) {
    SimInit();
    static uint64_t generation = 0;
    const bool reset = generation != g_cityGeneration;
    if (reset) { // new map or a load wiped the city
        generation = g_cityGeneration;
        SimOnCityReset();
    }
    SimApplyPending(); // a loaded game's sim records, once the reset above is done
    if (reset || SimTbdLoadIsPending()) { // the load's .tbdsim (or fresh), then the phases' Load hooks
        g_simLoadInProgress = SimTbdLoadIsPending();
        SimPhasesAfterReset();
        g_simLoadInProgress = false;
    }
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
    SimPhasesFrame(realDt); // the building ids, P1 .. P7, the autosave (city_sim_phases.inl)

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
    SimBudgetHudFill(hud); // money, the milestone banner, the Budget panel and the locks (city_sim_budget.inl)
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

// The F3 overlay's SIM section.
void SimProfile(tbd::profiler::Sample& smp) {
    smp.simActive = true;
    smp.simMs = g_sim.FrameMs();
    smp.simAvgMs = g_sim.AvgFrameMs();
    smp.simBudgetMs = g_sim.budgetMs;
    smp.simBacklog = g_sim.Backlog();
    smp.simTicks = (uint32_t)g_sim.TicksLastFrame();
    smp.simTotalMs = g_simTotalMs, smp.simTotalAvgMs = g_simTotalAvgMs; // the F3 line "sim total ms/frame"
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
