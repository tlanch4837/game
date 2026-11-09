// Lot levelling, abandonment and recovery (CS2 sim roadmap Phase 2). Header only, headless (tests\sim_land_lots_test).
// The member functions that tick are in sim_land_lots_tick.h (included at the end).
//
// A built lot is in one of three conditions, from its inputs (land value at the lot, utilities, P3 pollution, P2 high
// rent, a bankrupt company in it) and its category's demand:
//   good     utilities, demand above 0, land value at or over the level's up threshold, no high rent, not polluted
//   bad      no power or water, demand under badDemand, land value under the level's down threshold, or bankrupt
//   neutral  between (the gap between the up and the down thresholds is the hysteresis: a lot that just levelled up is
//            neutral at its new level, not bad, so nothing flips back and forth)
// Good for upMinutes (x 1..1.5, a fixed per-lot patience): one level up. Bad for downMinutes (x 1..2): one level down, or
// at level 1 ABANDONED: the lot keeps standing, empty and flagged (kFlagAbandoned). Abandoned and no longer bad for
// recoverMinutes: it recovers. Abandoned and still bad abandonMinutes after it was abandoned: demolished (the zone stays
// painted and grows again when there is demand).
// At most max(maxActionsPerHour, lots / actionsPerLots) level changes a game hour: no levelling waves.
//
// Deferred: a lot costs nothing until it is due or dirty. Its next action time sits in a time-ordered queue (ties by lot
// id). The host marks lots dirty from triggers only (a cell's land value changed, a lot's utility or flag bits changed, a
// demand threshold was crossed for a category, a company event) and LotLevels re-evaluates at most evalPerTick of them a
// tick, in mark order. A round-robin sweep of one group of lots a tick finds lots that were built, rebuilt or removed.
// FullCheck re-evaluates every lot (or one group of them) from scratch and counts disagreements with the stored state
// (tests and the debug check: must be 0; lots still waiting in the dirty list are skipped).
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "city_cells.h"
#include "sim.h"
#include "zoning.h"

namespace tbd {
namespace city {

struct LotParams {
    int upLand = 70, upLandStep = 30;  // land value to level up from level L: upLand + upLandStep x (L - 1)
    int hysteresis = 20;               // the down threshold of level L is the up threshold of L - 1 minus this
    int abandonLand = 12;              // the down threshold of level 1
    double badDemand = -0.35;
    int64_t upMinutes = 3 * 1440, downMinutes = 4 * 1440, abandonMinutes = 3 * 1440, recoverMinutes = 1440;
    int maxActionsPerHour = 3, actionsPerLots = 150;
    int evalPerTick = 48;              // dirty lots re-evaluated per tick at most
    int popsPerTick = 16, actionsPerTick = 2; // due queue entries looked at / acted on per tick at most
    int sweepGroups = 60;              // the sweep visits lots with index % sweepGroups == group, one group a tick
    bool utilitiesRequired = true;
    int UpLand(int level) const { return upLand + upLandStep * (std::max(1, level) - 1); }
    int DownLand(int level) const { return level <= 1 ? abandonLand : UpLand(level - 1) - hysteresis; }
};

enum LotCond : uint8_t { kLotNeutral = 0, kLotGood = 1, kLotBad = 2 };
enum LotInBits : uint8_t { kLotInPower = 1, kLotInWater = 2, kLotInSewage = 4, kLotInPolluted = 8, kLotInHighRent = 16, kLotInClosed = 32 };

// What a lot's condition depends on, besides its category's demand.
struct LotInputs {
    uint8_t land = 0;
    uint8_t bits = kLotInPower | kLotInWater | kLotInSewage; // LotInBits
};

inline uint8_t LotCondition(const LotParams& p, int level, const LotInputs& in, double demand) {
    const bool power = !p.utilitiesRequired || (in.bits & kLotInPower), water = !p.utilitiesRequired || (in.bits & kLotInWater);
    const bool sewage = !p.utilitiesRequired || (in.bits & kLotInSewage);
    if (!power || !water || demand < p.badDemand || (int)in.land < p.DownLand(level) || (in.bits & kLotInClosed)) return kLotBad;
    if (sewage && demand > 0.0 && (int)in.land >= p.UpLand(level) && !(in.bits & (kLotInHighRent | kLotInPolluted))) return kLotGood;
    return kLotNeutral;
}

struct LotState {
    uint64_t id = 0;
    int32_t cellX = 0, cellZ = 0;
    uint8_t cond = kLotNeutral, level = 1, abandoned = 0, built = 0;
    uint8_t dirty = 0, maxed = 0, cat = 0, pad = 0; // maxed: no model one level up (good does not schedule); cat: ZoneCat
    int64_t since = 0, due = -1, abandonedAt = -1;
    uint32_t seen = 0; // the sweep cycle that last saw the lot alive
};

enum class LotEventType : uint8_t { LevelUp, LevelDown, Abandoned, Recovered, Demolished };
struct LotEvent {
    LotEventType type = LotEventType::LevelUp;
    uint64_t lot = 0;
    int level = 1;
};

class LotLevels {
public:
    LotParams p;
    std::unordered_map<uint64_t, LotState> st;   // by Lot::id
    std::set<std::pair<int64_t, uint64_t>> queue; // (due minute, lot id)
    std::vector<uint64_t> dirty;                  // lot ids in mark order (LotState::dirty says it is in here)
    std::vector<LotEvent> events;                 // since the host last emptied it
    uint64_t levelUps = 0, levelDowns = 0, abandonedTotal = 0, recovered = 0, demolished = 0;
    int64_t hour = INT64_MIN;
    int actionsThisHour = 0, sweepGroup = 0;
    uint32_t cycle = 1;
    int seenNow = 0; // states seen in this sweep cycle: the end-of-cycle prune runs only when some were not (a lot went)
    int evaluated = 0; // last tick (bench)

    void Clear() { LotParams keep = p; *this = LotLevels(); p = keep; }
    int Tracked() const { return (int)st.size(); }
    int AbandonedNow() const { int n = 0; for (const auto& kv : st) n += kv.second.abandoned; return n; }
    const LotState* Find(uint64_t id) const { const auto it = st.find(id); return it == st.end() ? nullptr : &it->second; }
    void MarkDirty(uint64_t id) {
        const auto it = st.find(id);
        if (it == st.end() || it->second.dirty) return;
        it->second.dirty = 1, dirty.push_back(id);
    }
    // Every lot in a cell (its land value changed).
    void MarkCell(CellKey k) {
        const auto it = byCell_.find(PackKey(k));
        if (it != byCell_.end()) for (uint64_t id : it->second) MarkDirty(id);
    }
    // Every lot of a zone category (its demand crossed a threshold); in id order.
    void MarkCat(int cat) {
        std::vector<uint64_t> ids;
        for (const auto& kv : st) if (kv.second.cat == cat) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        for (uint64_t id : ids) MarkDirty(id);
    }

    // Defined in sim_land_lots_tick.h. inputs(const Lot&, size_t li) -> LotInputs; demand: per ZoneCat.
    template <class Inputs>
    void Tick(ZoneGrid& z, int64_t minute, const double demand[4], Inputs&& inputs);
    template <class Inputs>
    int FullCheck(const ZoneGrid& z, const double demand[4], Inputs&& inputs, size_t group = 0, size_t groups = 1) const;
    void Track(const Lot& l, int64_t minute, size_t li = ~(size_t)0);
    void Untrack(uint64_t id);
    void Rebuild(); // the queue and the cell index from st (after a load)

private:
    std::unordered_map<uint64_t, std::vector<uint64_t>> byCell_; // PackKey -> lot ids
    std::unordered_map<uint64_t, size_t> index_;                 // lot id -> zones.lots index (checked on use)
    std::unordered_set<uint64_t> models_;
    size_t modelsFor_ = ~(size_t)0;
    template <class Inputs>
    void Evaluate(ZoneGrid& z, LotState& s, size_t li, int64_t minute, const double demand[4], Inputs& inputs);
    void Act(ZoneGrid& z, LotState& s, size_t li, int64_t minute);
    void Schedule(LotState& s, int64_t due);
    int LotIndex(const ZoneGrid& z, uint64_t id);
    void Sweep(ZoneGrid& z, int64_t minute);
    bool HasModel(const ZoneGrid& z, int type, int w, int d, int level);
    static double Jitter(uint64_t id) { return (double)(SimSeed(0x6C0, id) % 1000u) / 1000.0; }
};

} // namespace city
} // namespace tbd

#include "sim_land_lots_tick.h"
