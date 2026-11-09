// ------------------------------------------------------------------- city sim Phase 4: the svc system
// Included by city_sim_p4.inl.

namespace sim_p4 {
// A finished pass into the world, the factors, the legacy CellSim coverage (growth and land value read those six) and
// the overlays.
void Install(SvcPrepared&& p) {
    const double t0 = Now();
    SvcInstall(w, std::move(p));
    SvcWriteCoverageFactors(w, g_cellFactors);
    const auto& cells = g_citySim.cells.cells;
    for (size_t c = 0; c < w.cov.cells.size() && c < g_citySim.sim.size() && c < cells.size(); ++c) {
        if (!(cells[c].key == w.cov.cells[c].key)) continue;
        for (int k = 0; k < kServices; ++k) g_citySim.sim[c].coverage[k] = w.out.cov[c][(size_t)k];
    }
    ++g_simCoverageRuns, ++covRuns;
    ++tbd::gui::OverlayProvidersVersion();
    installMs = Now() - t0;
}
void Compute(SvcPrepared& p) { SvcCover(p.cov, p.out), SvcPrepare(p); }

// Every 10 game minutes: the pass requested last time lands (worker's, or the same pass computed here), then a new
// snapshot if anything changed.
void CoverageTick() {
    if (job) {
        if (job->ready.load(std::memory_order_acquire)) Install(std::move(job->out)), covMs = job->ms;
        else { // the worker keeps its own reference and finishes on it; its result is never read
            SvcPrepared p = job->in;
            const double t0 = Now();
            Compute(p);
            covMs = Now() - t0, ++covFallbacks;
            Install(std::move(p));
        }
        job.reset();
    }
    SvcPrepared p;
    if (!CoverageInput(p, syncNext)) return;
    if (syncNext) { // a load: the dispatch needs its sites now, as the city that was saved had them
        syncNext = false;
        const double t0 = Now();
        Compute(p);
        covMs = Now() - t0;
        Install(std::move(p));
        return;
    }
    auto j = std::make_shared<CovJob>();
    j->in = std::move(p);
    job = j;
    g_engine.jobs->RunBackground([j] {
        const auto a = std::chrono::steady_clock::now();
        j->out = j->in;
        Compute(j->out);
        j->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        j->ready.store(true, std::memory_order_release);
    });
}

// The traffic delay per cell from the flow solve (x free-flow time), when the flow changed.
void DelayTick() {
    const Network& net = g_roads.net;
    if (g_segFlowVersion != net.version || g_segFlow.size() != net.segs.size()) return;
    const uint64_t key = g_segFlowVersion * 31 + (uint64_t)(g_simFlowSolveMs * 1000.0);
    if (key == delayFrom) return;
    delayFrom = key, delay.clear();
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!net.segs[s].alive || g_segFlow[s].flow <= 1.0f) continue;
        const tbd::Vec3 p = net.nodes[(size_t)net.segs[s].a].p;
        float& d = delay[PackKey(CellKeyAt(p.x, p.z))];
        d = std::max(d, std::round(4.0f / std::max(0.25f, g_segFlow[s].speedRatio)) / 4.0f); // quarter steps: gameplay input
    }
}

// Deaths (a hearse request at the home) and floods (damage at every flooded building) from the shared ring.
void ReadEvents(int64_t minute) {
    SimEvent e;
    bool flood = false;
    while (g_simEvents.Read(events, e)) {
        if (e.type == SimEventType::Death && e.building) SvcReport(w, kIncBody, e.building, minute);
        else if (e.type == SimEventType::Flood) flood = true;
    }
    if (!flood) return;
    int n = 0;
    for (const SvcBldg& b : w.bld)
        if (b.kind != kBkService && g_simFlagTable.Has(BuildingId(b.id), kFlagFlooded) && !w.Active(b.id, kIncFlood) && n < 64)
            n += SvcReport(w, kIncFlood, b.id, minute) != 0;
}
} // namespace sim_p4

#include "city_sim_p4_tick2.inl" // seats, garbage, vehicles, the system itself
