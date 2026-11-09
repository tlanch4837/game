// ------------------------------------------------------------------- city sim: demand and growth registrations (S2)
// Included by city_sim.inl (moved out of SimInit, CS2 sim roadmap Phase 0; owned by Phase 2 from here on). The headless
// logic is city/sim_growth.h; g_simGrowth itself stays in city_sim.inl.

// SimInit's registration (same place in the order as before the move: after cells and SimLoadTuning, before people).
// With Phase 2 on ("land.enabled", the default) its systems (city_sim_p2.inl: demand from causes, the land field,
// rent, LotLevels) replace these; the old S2 model stays for "land.enabled": 0 (a dev switch for bisecting).
void SimRegisterGrowth() {
    if (g_simPhaseTune.land.enabled != 0.0) return;
    // S2: demand every 10 game minutes (smoothed over a day), and the spawn gate it drives.
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
}
