// ------------------------------------------------------------------- city sim: the .tbdsim file and autosave (CS2 sim roadmap 3.4)
// Included by city_sim_phases.inl. The format, CRC and chunk registry are src/city/sim_save.h (headless, tests\sim_save_test);
// this file is the host: one registry for every phase (P0 registers HEAD and BLDG first, then the hub asks each phase's
// Save hook for its chunks), the BuildingRegistry that BLDG carries, and the slot files beside .tbdgame and .hfd.
//   Save   SaveCityGame (city_game.inl) asks SimTbdSnapshot() for the chunks and writes slot_NN.tbdsim with the other two.
//   Load   LoadCityGame reads the file into g_simTbdPending (SimTbdLoadPending); SimSync applies it after the reset that
//          the load caused (SimTbdApplyPending), so nothing the reset clears is lost. No file (an old save) = every chunk
//          Missing: each system starts fresh, and the M and Q records still load as before.
//   Autosave  every 10 real minutes in the city (TBD_SIM_AUTOSAVE_S=<seconds>, 0 off), and when the iOS app goes to the
//          background (tbd::host::EnterBackground). Rotating slots autosave_1..3 (save_slots.h kAutosaveSlot0). The sim
//          image is built on the main thread (the copy, logged in ms); the files are written on a worker.

tbd::city::SimSaveRegistry g_simChunks;    // every phase's chunks, in registration order
tbd::city::BuildingRegistry g_simBuildings; // BuildingId <-> (origin, origin id) for every sim-relevant building (BLDG)
tbd::city::SimCalendar g_simCalendar;       // minute -> day, life month, season (sim_calendar.h)
tbd::city::SimSaveHead g_simHead;           // the HEAD of the last .tbdsim loaded (fresh: zeros)
uint32_t g_simAutosaveCount = 0;            // autosaves written (picks the rotating slot; saved in HEAD)
double g_simSaveCopyMs = 0.0;               // main-thread ms of the last SimTbdBytes (the autosave copy)

struct SimTbdPending {
    bool has = false;            // a load read (or looked for) a .tbdsim: apply it after the reset
    std::vector<uint8_t> bytes;  // empty: no file (an old save)
    int slot = -1;
} g_simTbdPending;

namespace sim_save {
uint64_t zones = ~0ull, transport = ~0ull, util = ~0ull, ind = ~0ull;
uint64_t lastSyncMs = 0;
bool waiting = false;          // a load's BLDG rows are reserved: released after the lots came back
bool lotsBack = false;         // GameSync put the load's grown lots back (SimLotsRestored)
uint64_t releaseDeadlineMs = 0; // or this long after the load at the latest (a save without grown lots)
uint64_t lotsBackGen = ~0ull;  // g_cityGeneration when GameSync last put a load's lots back (it may run before the apply)
std::vector<tbd::city::BuildingId> hintLot, hintTr, hintUt, hintInd; // per source entry: its BuildingId at the last sync
uint64_t syncCount = 0;        // every 64th sync checks the registry's index against its rows (CheckIndex)
} // namespace sim_save

// BuildingRegistry from the zones, transport, utility and industry lists: only when one of them changed, at most once a
// real second (force: a save). Cells, hoods, homes and jobs are left to the phases that own them (rows keep kind, level
// and the CityCell key).
void SimBuildingsSync(bool force) {
    using namespace tbd::city;
    namespace ss = sim_save;
    const uint64_t now = tbd::platform::TickCountMs();
    const bool changed = ss::zones != g_zones.version || ss::transport != g_transport.version || ss::util != g_util.version || ss::ind != g_ind.version;
    if (changed && (force || now - ss::lastSyncMs >= 1000)) {
        ss::zones = g_zones.version, ss::transport = g_transport.version, ss::util = g_util.version, ss::ind = g_ind.version;
        ss::lastSyncMs = now;
        auto row = [](int kind, V2 c, int level) {
            BuildingRow r;
            const CellKey k = CellKeyAt(c.x, c.z);
            r.kind = kind, r.cellX = k.x, r.cellZ = k.z, r.level = (uint8_t)std::clamp(level, 0, 255);
            return r;
        };
        // one hint per source entry (ss::hint*): an unchanged building costs no hash lookup (BuildingRegistry::Upsert)
        auto fit = [](std::vector<BuildingId>& h, size_t n) { if (h.size() != n) h.resize(n); };
        fit(ss::hintLot, g_zones.lots.size()), fit(ss::hintTr, g_transport.items.size());
        fit(ss::hintUt, g_util.items.size()), fit(ss::hintInd, g_ind.sites.size());
        g_simBuildings.BeginSync();
        for (size_t i = 0; i < g_zones.lots.size(); ++i) {
            const Lot& l = g_zones.lots[i];
            if (l.alive) g_simBuildings.Upsert(BOrigin::Lot, l.id, row(l.type, l.c, l.level), ss::hintLot[i]);
        }
        for (size_t i = 0; i < g_transport.items.size(); ++i) {
            const Placed& b = g_transport.items[i];
            if (b.alive) g_simBuildings.Upsert(BOrigin::Transport, b.id, row(b.def, b.c, 1), ss::hintTr[i]);
        }
        for (size_t i = 0; i < g_util.items.size(); ++i) {
            const UtilBuilding& b = g_util.items[i];
            if (b.alive) g_simBuildings.Upsert(BOrigin::Utility, b.id, row(b.def, b.c, 1), ss::hintUt[i]);
        }
        for (size_t i = 0; i < g_ind.sites.size(); ++i)
            g_simBuildings.Upsert(BOrigin::Industry, g_ind.sites[i].id, row((int)g_ind.sites[i].kind, g_ind.sites[i].c, 1), ss::hintInd[i]);
        g_simBuildings.EndSync();
        if (++ss::syncCount % 64 == 0 && !g_simBuildings.CheckIndex()) tbd::Log("Sim: BUG building registry index out of step with its rows");
        if (ss::waiting && ss::lotsBack) ss::waiting = ss::lotsBack = false, ss::releaseDeadlineMs = 0, g_simBuildings.ReleaseUnclaimed();
    }
    if (ss::waiting && ss::releaseDeadlineMs && now >= ss::releaseDeadlineMs) // the lots never came back (no G records)
        ss::waiting = ss::lotsBack = false, ss::releaseDeadlineMs = 0, g_simBuildings.ReleaseUnclaimed();
}
void SimBuildingsLotsBack() { sim_save::lotsBack = sim_save::waiting, sim_save::lotsBackGen = g_cityGeneration; }

// P0's chunks: HEAD (the clock and seeds) and BLDG (the building ids).
void SimSaveRegisterP0() {
    using namespace tbd::city;
    SimChunkDef head;
    head.fourcc = simchunk::HEAD;
    head.save = [](ByteWriter& w) {
        SimSaveHead h;
        h.minute = g_sim.Minute(), h.lifeMonth = g_simCalendar.LifeMonth(h.minute);
        h.citySeed = (uint64_t)(uint32_t)g_engine.terrain.params.seed, h.autosaveCounter = g_simAutosaveCount;
        WriteHead(w, h);
    };
    head.load = [](ByteReader& r, uint16_t) {
        if (!ReadHead(r, g_simHead)) return false;
        g_simAutosaveCount = std::max(g_simAutosaveCount, g_simHead.autosaveCounter);
        return true;
    };
    head.fresh = [] { g_simHead = SimSaveHead(); };
    g_simChunks.Register(head);
    SimChunkDef bldg;
    bldg.fourcc = simchunk::BLDG;
    bldg.save = [](ByteWriter& w) { WriteBuildings(w, g_simBuildings); };
    bldg.load = [](ByteReader& r, uint16_t) { return ReadBuildings(r, g_simBuildings); };
    bldg.fresh = [] { g_simBuildings.Clear(); };
    g_simChunks.Register(bldg);
}

#include "city_sim_save_io.inl" // the slot files, the load apply and the autosave
