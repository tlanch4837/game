// Phase 4 state: SvcWorld, everything the services hold between ticks (CS2 sim roadmap Phase 4). Header only, headless.
// The host fills the buildings and the coverage snapshot, applies the coverage pass's result, and calls Tick each game
// minute (sim_svc_tick.h). Names carry an Svc prefix: the unity build shares one namespace with the other phases.
#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include "sim.h"
#include "sim_events.h"
#include "sim_flags.h"
#include "sim_svc_assign.h"
#include "sim_svc_incidents.h"

namespace tbd {
namespace city {

struct SvcParams {
    float yearDays = 12;           // game days in a life year (sim_calendar.h: 1 game day = 1 life month)
    float crimePerK = 24, firePerKHomes = 2.5f, sickPerK = 60; // a year, per 1,000 residents / homes (design doc 11)
    int burnMinutes = 90;          // a fire nobody fights for this long burns the building down
    int floodMinutes = 720;        // flood damage nobody pumps out for this long ruins the building
    int retryMinutes = 10;         // a waiting incident looks for a free vehicle again
    int crimeGiveUp = 90, sickGiveUp = 1440; // unanswered after this long: the criminal got away / the patient recovered alone
    float spreadChance = 0.35f, arrestShare = 0.5f;
    int bedMinutes = 360, jailMinutes = 1440, cremateMinutes = 720, destroyedMinutes = 4320;
    int flagCrime = 150;           // cell crime factor at which buildings show "High crime"
};

// Recurring per-cell work (incident rates, crime decay, the crime and fire factors) runs one group per 15-minute
// window: every cell once a game day, in a fixed group picked by its key (so the order survives a load).
constexpr int kSvcGroups = 96;
inline size_t SvcGroupOf(CellKey k) { return (size_t)(SimSeed(0x5C6Bull, (uint64_t)(uint32_t)k.x, (uint64_t)(uint32_t)k.z) % kSvcGroups); }

enum SvcBKind : uint8_t { kBkHome = 0, kBkShop, kBkIndustry, kBkOffice, kBkService, kBkOther };
struct SvcBldg {
    uint32_t id = 0;               // BuildingId
    uint64_t lot = 0;              // Lot::id for zoned lots (garbage piles), 0 else
    CellKey key{};
    int32_t cell = -1;             // cov cell hint (the host's CitySim index; checked against key)
    int32_t node = -1;
    uint8_t kind = kBkOther, level = 1;
    uint16_t homes = 0, jobs = 0;
};
struct SvcCellStat { uint32_t residents = 0, homes = 0, jobs = 0, industryJobs = 0; uint8_t maxLevel = 1; };
struct SvcSiteState { uint16_t busy = 0; uint32_t used = 0, graves = 0, handled = 0; }; // used = held now + graves
struct SvcDayStats {
    uint32_t n[kIncTypes] = {}, unanswered[kIncTypes] = {};
    uint32_t destroyed = 0;
    double responseMin = 0; uint32_t responses = 0; // sum of minutes from start to arrival
};
struct SvcTrip { int fromNode = -1, toNode = -1; int64_t minute = 0; uint8_t slot = 0; };

class SvcWorld {
public:
    SvcParams P;
    uint64_t seed = 1;
    SimEventRing* events = nullptr;
    std::function<float(CellKey)> delay; // traffic: x the free-flow minutes (1: empty roads); null = 1
    std::function<float(int cell, int type)> rateMul; // policies (P7 PolicyMods): x a cell's crime / fire / sick rate; null = 1
    // the applied coverage (the pass's input and output) and its lookups
    SvcCovIn cov;
    SvcCovOut out;
    std::vector<SvcCellStat> cellStat;              // parallel to cov.cells
    std::unordered_map<uint64_t, int> cellIndex;    // PackKey -> cov cell
    std::unordered_map<uint32_t, int> siteIndex;    // site id -> cov site
    // buildings (host, from the registry) and per cell
    std::vector<SvcBldg> bld;
    std::vector<int> bldAt;                         // BuildingId index -> bld position (-1), checked against the id
    std::vector<int> cellBldOff, cellBldIdx;        // CSR over cov cells (rebuilt on either change)
    // state (saved: SERV INCD)
    std::unordered_map<uint32_t, SvcSiteState> site;
    SvcIncidentStore inc;
    std::unordered_map<uint64_t, uint16_t> crimeAcc;      // PackKey -> unanswered-crime pressure
    std::unordered_map<uint32_t, int64_t> destroyedUntil; // BuildingId -> rubble until
    // derived (rebuilt from the state on load: Recount)
    std::unordered_map<uint64_t, uint16_t> active;  // (building << 3 | type) -> live incidents not yet attended
    std::vector<uint8_t> crimeF, fireF;             // per cov cell, the factors written
    SvcDayStats today, yesterday;
    std::vector<SvcTrip> trips;                     // vehicle trips of the last game day (the herds draw them)
    std::vector<uint32_t> dirty;                    // buildings whose flags need a look (FIFO, deduplicated)
    std::unordered_map<uint32_t, uint8_t> dirtyMark;
    std::vector<uint16_t> unseated;                 // per cov cell: students without a seat (sim_svc_education.h)
    std::unordered_map<uint32_t, uint32_t> enrolled; // school site id -> seats taken (sim_svc_education.h keeps it)
    bool eduSwept = false;                          // the seat pass has visited every citizen once (NoStudents waits for it)
    std::vector<std::vector<int>> cellGroups;      // kSvcGroups round-robin groups of cov cells (by key hash)
    uint64_t version = 0;                           // bumps when anything the overlays or panels show changed

    int CellOf(CellKey k) const { const auto it = cellIndex.find(PackKey(k)); return it == cellIndex.end() ? -1 : it->second; }
    const SvcSite* Site(uint32_t id) const { const auto it = siteIndex.find(id); return it == siteIndex.end() ? nullptr : &cov.sites[(size_t)it->second]; }
    const SvcBldg* Bldg(uint32_t id) const {
        const uint32_t i = id & Handle::kIndexMask;
        if (i >= bldAt.size() || bldAt[i] < 0) return nullptr;
        const SvcBldg& b = bld[(size_t)bldAt[i]];
        return b.id == id ? &b : nullptr;
    }
    // The cov cell of a building: its host hint when it still names the same cell, else by key.
    int CellOfB(const SvcBldg& b) const {
        if (b.cell >= 0 && (size_t)b.cell < cov.cells.size() && cov.cells[(size_t)b.cell].key == b.key) return b.cell;
        return CellOf(b.key);
    }
    int flagSweep = -1;                             // a full flags pass in progress: the next bld position (-1: none)
    void MarkDirty(uint32_t b) { if (b && !dirtyMark[b]) dirtyMark[b] = 1, dirty.push_back(b); }
    static uint64_t ActiveKey(uint32_t b, int type) { return (uint64_t)b << 3 | (uint64_t)type; }
    uint16_t Active(uint32_t b, int type) const { if (active.empty()) return 0; const auto it = active.find(ActiveKey(b, type)); return it == active.end() ? 0 : it->second; }
    void AddActive(uint32_t b, int type, int d) {
        uint16_t& n = active[ActiveKey(b, type)];
        n = (uint16_t)std::max(0, (int)n + d);
        if (!n) active.erase(ActiveKey(b, type));
        MarkDirty(b);
    }
    void Event(SimEventType t, int64_t minute, uint32_t b, int64_t v = 0) { if (events) events->Push(t, minute, b, 0, v); }
    float Delay(CellKey k) const { return delay ? std::clamp(delay(k), 1.0f, 4.0f) : 1.0f; }
};

} // namespace city
} // namespace tbd
