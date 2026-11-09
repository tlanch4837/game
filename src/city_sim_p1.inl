// ------------------------------------------------------------------- city sim Phase 1: citizens, households, jobs, commuting, births, deaths, migration
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 1 owns this file; the hub calls the five hooks below,
// whose signatures are frozen by Phase 0. Until Phase 1 lands they do nothing.
//   Init    once, from SimInit after the existing systems: register scheduler systems (g_sim.Register); tuning is
//           g_simPhaseTune.life ("life." keys in assets\sim\tuning.json)
//   Frame   once per frame from SimSync, after the scheduler ran (realDt: real seconds)
//   Save    once, at init: register this phase's .tbdsim chunks (its fourccs in sim_save.h simchunk) with reg
//   Load    after every city reset (new map: no file) and every load, once the chunks were applied; rep tells which
//           chunks loaded and which started fresh (Missing: a save from before this phase)
//   Panels  once, at init: RegisterSimPanel for this phase's panels (gui/sim_panels.h)

void SimP1Init() {}
void SimP1Frame(float /*realDt*/) {}
void SimP1Save(tbd::city::SimSaveRegistry& /*reg*/) {}
void SimP1Load(const tbd::city::SimLoadReport& /*rep*/) {}
void SimP1Panels() {}