// ------------------------------------------------------------------- city sim: the phase hub (CS2 sim roadmap 3.3, Phase 0)
// Included by game_core.inl right after city_sim.inl. It hosts the .tbdsim save (city_sim_save.inl) and the seven phase
// files city_sim_p1.inl .. city_sim_p7.inl, and calls each phase's five hooks (frozen signatures, N = 1..7):
//   void SimPNInit();                                   once, from SimInit, after the existing systems registered
//   void SimPNFrame(float realDt);                      every frame, from SimSync, after the scheduler ran
//   void SimPNSave(tbd::city::SimSaveRegistry& reg);    once, at init: the phase registers its .tbdsim chunks
//   void SimPNLoad(const tbd::city::SimLoadReport& rep); after every city reset and load, once the chunks were applied
//   void SimPNPanels();                                 once, at init: the phase's RegisterSimPanel calls
// The order is always P1 .. P7. A phase reads its tuning from g_simPhaseTune (its own prefix, sim_tuning_table.h) and
// is switched off by "<prefix>enabled": 0 (Init and Frame are not called; its chunks still load and save).
// Cost of the hub itself: the building registry sync (only on change, at most once a real second) and the autosave
// clock; "SIM TOTAL" in F3 is all of SimSync (scheduler, phases, hub).

#include "city_sim_save.inl" // .tbdsim and autosave (HEAD, BLDG, the slot files)
#include "city_sim_p1.inl"   // P1 citizens, households, jobs, migration
#include "city_sim_p2.inl"   // P2 demand, land value, rent, levelling
#include "city_sim_p3.inl"   // P3 pollution, weather, water
#include "city_sim_p4.inl"   // P4 services, incidents, garbage
#include "city_sim_p5.inl"   // P5 public transport, modes, outside connections
#include "city_sim_p6.inl"   // P6 companies, goods, trade
#include "city_sim_p7.inl"   // P7 budget, policies, districts, milestones

struct SimPhaseHooks {
    const char* name;
    const double* enabled; // g_simPhaseTune.<x>.enabled
    void (*init)();
    void (*frame)(float);
    void (*save)(tbd::city::SimSaveRegistry&);
    void (*load)(const tbd::city::SimLoadReport&);
    void (*panels)();
};
const SimPhaseHooks kSimPhases[] = {
    {"P1", &g_simPhaseTune.life.enabled, SimP1Init, SimP1Frame, SimP1Save, SimP1Load, SimP1Panels},
    {"P2", &g_simPhaseTune.land.enabled, SimP2Init, SimP2Frame, SimP2Save, SimP2Load, SimP2Panels},
    {"P3", &g_simPhaseTune.env.enabled, SimP3Init, SimP3Frame, SimP3Save, SimP3Load, SimP3Panels},
    {"P4", &g_simPhaseTune.svc.enabled, SimP4Init, SimP4Frame, SimP4Save, SimP4Load, SimP4Panels},
    {"P5", &g_simPhaseTune.move.enabled, SimP5Init, SimP5Frame, SimP5Save, SimP5Load, SimP5Panels},
    {"P6", &g_simPhaseTune.goods.enabled, SimP6Init, SimP6Frame, SimP6Save, SimP6Load, SimP6Panels},
    {"P7", &g_simPhaseTune.gov.enabled, SimP7Init, SimP7Frame, SimP7Save, SimP7Load, SimP7Panels},
};
bool SimPhaseOn(const SimPhaseHooks& p) { return *p.enabled != 0.0; }

// ---- the shared flag table (g_simFlagTable, by BuildingId) into the per-lot g_simFlags (zones.lots order)
namespace sim_hub {
tbd::city::SimFlags tableMask = 0;      // the bits of the phases that are on (set in SimPhasesInit)
std::vector<tbd::city::BuildingId> lotIds; // per lot: its BuildingId (cached for the zones and registry versions below)
uint64_t idsZones = ~0ull, idsReg = ~0ull;
uint64_t mergedTable = ~0ull, mergedFlags = ~0ull; // the table and g_simFlags versions of the last merge
double mergeMs = 0.0;
} // namespace sim_hub

void SimMergeTableFlags(std::vector<uint32_t>& perLot) {
    using namespace tbd::city;
    namespace h = sim_hub;
    if (!h::tableMask) return;
    if (h::idsZones != g_zones.version || h::idsReg != g_simBuildings.version || h::lotIds.size() != g_zones.lots.size()) {
        h::idsZones = g_zones.version, h::idsReg = g_simBuildings.version;
        h::lotIds.assign(g_zones.lots.size(), BuildingId());
        for (size_t i = 0; i < g_zones.lots.size(); ++i)
            if (g_zones.lots[i].alive) h::lotIds[i] = g_simBuildings.Find(BOrigin::Lot, g_zones.lots[i].id);
    }
    const size_t n = std::min(perLot.size(), h::lotIds.size());
    for (size_t i = 0; i < n; ++i) perLot[i] = (perLot[i] & ~h::tableMask) | (g_simFlagTable.Get(h::lotIds[i]) & h::tableMask);
}

// Per frame: a phase changed its bits in the table (P3 hourly, P6 hourly): the per-lot flags follow.
void SimMergeTableFlagsFrame() {
    namespace h = sim_hub;
    if (!h::tableMask || (h::mergedTable == g_simFlagTable.version && h::mergedFlags == g_simFlagsVersion && h::idsReg == g_simBuildings.version)) return;
    const double t0 = g_sim.nowMs();
    if (h::mergedTable != g_simFlagTable.version) g_simFlagTable.Prune(g_simBuildings);
    static std::vector<uint32_t> next;
    next = g_simFlags;
    SimMergeTableFlags(next);
    if (next != g_simFlags) g_simFlags.swap(next), ++g_simFlagsVersion;
    h::mergedTable = g_simFlagTable.version, h::mergedFlags = g_simFlagsVersion;
    h::mergeMs = g_sim.nowMs() - t0;
}

// SimInit: P0's chunks, then each phase's systems, chunks and panels.
void SimPhasesInit() {
    SimSaveRegisterP0();
    for (int i = 0; i < (int)(sizeof(kSimPhases) / sizeof(kSimPhases[0])); ++i) // the table's bits of the phases that are on
        if (SimPhaseOn(kSimPhases[i])) sim_hub::tableMask |= tbd::city::SimFlagPhaseMask(i + 1);
    sim_hub::tableMask &= ~(tbd::city::kFlagCurrentMask & ~tbd::city::SimFlagPhaseMask(6) & ~(SimPhaseOn(kSimPhases[1]) ? tbd::city::SimFlagPhaseMask(2) : 0u)); // ComputeSimFlags keeps its own (P6 takes no customers, P2 abandoned)
    for (const SimPhaseHooks& p : kSimPhases) {
        if (SimPhaseOn(p)) p.init();
        else tbd::Log("Sim: phase %s switched off (tuning enabled = 0)", p.name);
        p.save(g_simChunks);
        p.panels();
    }
    tbd::Log("Sim: phase hub up, %zu .tbdsim chunks registered", g_simChunks.Defs().size());
}

// SimOnCityReset (a new map or a load wiped the city): every chunk's owner starts fresh.
void SimPhasesReset() {
    for (const tbd::city::SimChunkDef& d : g_simChunks.Defs())
        if (d.fresh) d.fresh();
    sim_save::zones = ~0ull, sim_save::waiting = sim_save::lotsBack = false, sim_save::releaseDeadlineMs = 0;
    g_cellFactors.Clear(); // every phase's columns go back to neutral with the city
    sim_hub::lotIds.clear(), sim_hub::idsZones = sim_hub::idsReg = sim_hub::mergedTable = sim_hub::mergedFlags = ~0ull;
}

// SimSync, after the reset and the M / Q records: the .tbdsim of a load (or nothing: a new map), then the Load hooks.
void SimPhasesAfterReset() {
    const tbd::city::SimLoadReport rep = SimTbdApplyPending();
    for (const SimPhaseHooks& p : kSimPhases) p.load(rep);
}

// SimSync, after the scheduler: the building ids, the phases' frames, the autosave.
void SimPhasesFrame(float realDt) {
    SimBuildingsSync(false);
    for (const SimPhaseHooks& p : kSimPhases)
        if (SimPhaseOn(p)) p.frame(realDt);
    SimMergeTableFlagsFrame();
    SimAutosaveSync(realDt);
}
