// ------------------------------------------------------------------- city sim: .tbdsim slot files, load apply, autosave
// Included by city_sim_save.inl (second half; see its header).

// Every chunk's payload as the sim stands (main thread). This is the autosave's copy, its cost g_simSaveCopyMs; the
// header, framing and CRCs are added where the file is written (SimSaveRegistry::Assemble, a worker for an autosave).
std::vector<tbd::city::SimSaveRegistry::ChunkImage> SimTbdSnapshot() {
    const double t0 = g_sim.nowMs();
    SimBuildingsSync(true);
    std::vector<tbd::city::SimSaveRegistry::ChunkImage> chunks = g_simChunks.Snapshot();
    g_simSaveCopyMs = g_sim.nowMs() - t0;
    return chunks;
}

// LoadCityGame: the slot's .tbdsim (if any) waits for the reset the load causes.
void SimTbdLoadPending(int slot) {
    SimTbdPending& p = g_simTbdPending;
    p.has = true, p.slot = slot;
    if (!tbd::city::ReadSimFileBytes(GameSavePath(slot, L".tbdsim"), p.bytes)) p.bytes.clear();
}

bool SimTbdLoadIsPending() { return g_simTbdPending.has; }

// After the reset (SimSync): every chunk loaded or fresh; the per-chunk report goes to the log. Returns the report the
// phases' Load hooks get (no load pending: a new map, every chunk Missing).
tbd::city::SimLoadReport SimTbdApplyPending() {
    using namespace tbd::city;
    SimTbdPending& p = g_simTbdPending;
    const std::vector<uint8_t> none;
    const SimLoadReport rep = g_simChunks.Load(p.has ? p.bytes : none); // empty: every chunk Missing (fresh)
    if (p.has) {
        if (p.bytes.empty()) tbd::Log("Sim: no .tbdsim beside save %d (an older save): the sim systems start fresh", p.slot + 1);
        else {
            tbd::Log("Sim: .tbdsim of save %d: %zu bytes, %d of %zu chunks loaded%s", p.slot + 1, p.bytes.size(), rep.Loaded(), rep.chunks.size(),
                     rep.fileOk ? (rep.truncated ? " (file truncated)" : "") : " (unreadable header)");
            for (const SimChunkResult& c : rep.chunks)
                if (c.status != SimChunkStatus::Loaded) tbd::Log("Sim:   %s %s", FourCCName(c.fourcc).c_str(), SimChunkStatusName(c.status));
            for (uint32_t f : rep.skipped) tbd::Log("Sim:   %s skipped (unknown to this build)", FourCCName(f).c_str());
        }
        const SimChunkResult* b = rep.Find(simchunk::BLDG);
        if (b && b->status == SimChunkStatus::Loaded) sim_save::waiting = true, sim_save::lotsBack = false, sim_save::releaseDeadlineMs = tbd::platform::TickCountMs() + 30000;
    }
    sim_save::zones = ~0ull; // the next sync claims the reserved rows
    p = SimTbdPending();
    return rep;
}

// ---- autosave
bool SaveCityGameFiles(int slot, int mode); // city_game.inl: mode 0 manual, 1 autosave now, 2 autosave on a worker
namespace title_host { bool Active(); }      // game/title_host.inl: the front end is up (no city being played)
double g_simAutosaveClock = 0.0;              // real seconds since the last autosave
int64_t g_simAutosaveMinute = -1;             // the sim minute it saved (nothing new: no autosave)
std::atomic<int> g_simAutosaveDone{0};        // set by the worker: 1 written, 2 failed (the toast reads it)
uint64_t g_simAutosaveGeneration = ~0ull;
// The save's files are written (any thread: a worker for mode 2): the toast on the next frame.
void SimAutosaveFinished(bool ok) { g_simAutosaveDone.store(ok ? 1 : 2); }

void SimAutosave(bool now) {
    const int slot = tbd::saves::kAutosaveSlot0 + (int)(g_simAutosaveCount % (uint32_t)tbd::saves::kAutosaveSlots);
    ++g_simAutosaveCount;
    g_simAutosaveClock = 0.0, g_simAutosaveMinute = g_sim.Minute();
    const double t0 = g_sim.nowMs();
    const bool ok = SaveCityGameFiles(slot, now ? 1 : 2);
    tbd::Log("Sim: autosave %s to %s: main thread %.2f ms (sim copy %.2f ms)%s", ok ? "started" : "FAILED", tbd::saves::SlotStem(slot).c_str(),
             g_sim.nowMs() - t0, g_simSaveCopyMs, now ? ", written at once" : "");
}

// Per frame (SimSync). The toast once the worker finished.
void SimAutosaveSync(float realDt) {
    static const double interval = sim_host::EnvDouble("TBD_SIM_AUTOSAVE_S", 600.0);
    const int done = g_simAutosaveDone.exchange(0);
    if (done) Toast(done == 1 ? "AUTOSAVED" : "AUTOSAVE FAILED");
    if (g_simAutosaveGeneration != g_cityGeneration) g_simAutosaveGeneration = g_cityGeneration, g_simAutosaveClock = 0.0; // a new or loaded city
    if (interval <= 0.0 || title_host::Active()) return;
    g_simAutosaveClock += realDt;
    if (g_simAutosaveClock < interval) return;
    if (g_sim.Minute() == g_simAutosaveMinute) { g_simAutosaveClock = 0.0; return; } // nothing ran since the last one
    SimAutosave(false);
}

// The app goes to the background (iOS): written at once, the OS may suspend the workers.
void SimEnterBackground() {
    if (title_host::Active() || g_cityGeneration == 0) return;
    SimAutosave(true);
}
