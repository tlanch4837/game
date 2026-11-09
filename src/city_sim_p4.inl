// ------------------------------------------------------------------- city sim Phase 4: city services, coverage, crime, fire, health, death, garbage, post, telecom
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 4 owns this file; the hub calls the five hooks below,
// whose signatures are frozen by Phase 0. Until Phase 4 lands they do nothing.
//   Init    once, from SimInit after the existing systems: register scheduler systems (g_sim.Register); tuning is
//           g_simPhaseTune.svc ("svc." keys in assets\sim\tuning.json)
//   Frame   once per frame from SimSync, after the scheduler ran (realDt: real seconds)
//   Save    once, at init: register this phase's .tbdsim chunks (its fourccs in sim_save.h simchunk) with reg
//   Load    after every city reset (new map: no file) and every load, once the chunks were applied; rep tells which
//           chunks loaded and which started fresh (Missing: a save from before this phase)
//   Panels  once, at init: RegisterSimPanel for this phase's panels (gui/sim_panels.h)

void SimP4Init() {}
void SimP4Frame(float /*realDt*/) {}
void SimP4Save(tbd::city::SimSaveRegistry& /*reg*/) {}
void SimP4Load(const tbd::city::SimLoadReport& /*rep*/) {}
void SimP4Panels() {}