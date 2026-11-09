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

// SimInit: P0's chunks, then each phase's systems, chunks and panels.
void SimPhasesInit() {
    SimSaveRegisterP0();
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
    SimAutosaveSync(realDt);
}
