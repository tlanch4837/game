// Phase 4 garbage into the service model (CS2 sim roadmap Phase 4: city_garbage.inl's collection, with capacity,
// trucks and saved piles). Header only, headless (tests\sim_svc_garbage_test).
//
// Every built lot makes garbage at its rate (tonnes per 30-day game month, city_garbage.inl's table). It piles up on the
// lot until a facility on the same road network within range takes it, nearest first (industry prefers the hazardous
// waste site), as far as the facility's capacity goes: each facility's capacity refills a bucket with time (at most an
// hour's worth). A truck leaves for every truckTonnes a facility collects towards one road node (the trucks become
// herds: they drive the roads and traffic sees them).
// Deferred: the lots live in kGarbGroups dense groups (by id hash, each sorted by id); the host visits one group a game
// minute, so a lot is visited once a game hour and its pile grows by rate x the minutes since its last visit. A lot's
// pile goes out to the host's map (city_garbage.inl's g_garbagePile, which draws the heaps) only when its level moved by
// a tenth, so the visit stays free of map work for most lots.
#pragma once

#include <climits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "sim_svc_world.h"

namespace tbd {
namespace city {

constexpr int kGarbGroups = 60;       // one group a game minute: every lot once a game hour
constexpr float kGarbMonthMinutes = 30.0f * 1440.0f;
constexpr float kGarbPileMonths = 3.0f; // a pile of more than this many months of output is "piling up"
inline int GarbGroupOf(uint64_t lot) { return (int)(SimSeed(0x6A4Bull, lot) % kGarbGroups); }

struct SvcGarbFacility {
    uint32_t id = 0;
    uint8_t hazardous = 0;
    float x = 0, z = 0, range = 2200;
    int comp = -1, node = -1;
    float perMonth = 0;          // tonnes it can take a month (0: full or closed)
    float took = 0;              // this visit (out)
};
struct SvcGarbLot {
    uint64_t lot = 0;
    float x = 0, z = 0;
    int comp = -1, node = -1;
    int32_t cell = -1;           // host hint: its CitySim cell (the garbage coverage factor)
    float perMonth = 0;          // tonnes it makes a month
    uint8_t industrial = 0;
    uint8_t shown = 0;           // the level step (tenths, 0..40) last mirrored to the host
    double pile = 0;             // tonnes waiting (saved)
    int64_t last = INT64_MIN;    // minute of its last visit (saved)
    bool Piling() const { return pile > perMonth * kGarbPileMonths; }
    float Level() const { return perMonth > 0 ? (float)std::min(4.0, pile / (perMonth * kGarbPileMonths)) : 0.0f; }
};
struct SvcGarbStats {
    double produced = 0, collected = 0, uncollected = 0; // a month (produced), this visit (collected), tonnes waiting
    int piling = 0, lots = 0, unserved = 0, trucks = 0;
};

class SvcGarbage {
public:
    std::vector<SvcGarbLot> groups[kGarbGroups];   // each sorted by lot id
    std::unordered_map<uint64_t, float> truckAcc; // (facility, node) -> tonnes not yet a truckload (saved)
    std::unordered_map<uint32_t, double> bucket;  // facility id -> tonnes it can still take (saved)
    int64_t bucketAt = INT64_MIN;                 // minute of the last refill (saved)
    float truckTonnes = 10.0f;

    // The city's lots (any order). A lot that was here keeps its pile and last visit; `loaded` (a load's piles by
    // lot id: pile, last) fills the rest once and is emptied.
    void SetLots(std::vector<SvcGarbLot> lots, std::unordered_map<uint64_t, std::pair<double, int64_t>>* loaded = nullptr) {
        std::sort(lots.begin(), lots.end(), [](const SvcGarbLot& a, const SvcGarbLot& b) { return a.lot < b.lot; });
        std::vector<SvcGarbLot> next[kGarbGroups];
        for (SvcGarbLot& l : lots) next[GarbGroupOf(l.lot)].push_back(l);
        for (int g = 0; g < kGarbGroups; ++g) { // both sides sorted by id: a merge, no map
            const std::vector<SvcGarbLot>& o = groups[g];
            size_t j = 0;
            for (SvcGarbLot& l : next[g]) {
                while (j < o.size() && o[j].lot < l.lot) ++j;
                if (j < o.size() && o[j].lot == l.lot) l.pile = o[j].pile, l.last = o[j].last, l.shown = o[j].shown;
                else if (loaded) {
                    const auto q = loaded->find(l.lot);
                    if (q != loaded->end()) l.pile = q->second.first, l.last = q->second.second;
                    l.shown = (uint8_t)(l.Level() * 10.0f);
                }
            }
            groups[g].swap(next[g]);
        }
        if (loaded) loaded->clear();
    }
    // The deferred re-list for a big city: Prepare (pure, a worker) sorts and groups the new list; Adopt (main thread,
    // O(groups)) parks it; each group merges its part on its own next visit (or all at once in MergePending).
    struct Prepared { std::vector<SvcGarbLot> groups[kGarbGroups]; };
    static std::shared_ptr<Prepared> Prepare(std::vector<SvcGarbLot> lots) {
        auto p = std::make_shared<Prepared>();
        std::sort(lots.begin(), lots.end(), [](const SvcGarbLot& a, const SvcGarbLot& b) { return a.lot < b.lot; });
        for (SvcGarbLot& l : lots) p->groups[GarbGroupOf(l.lot)].push_back(l);
        return p;
    }
    void Adopt(const std::shared_ptr<Prepared>& p) {
        for (int g = 0; g < kGarbGroups; ++g) pending[g].swap(p->groups[g]), hasPending[g] = 1;
    }
    void MergeGroup(int g) {
        if (!hasPending[g]) return;
        const std::vector<SvcGarbLot>& o = groups[g];
        size_t j = 0;
        for (SvcGarbLot& l : pending[g]) {
            while (j < o.size() && o[j].lot < l.lot) ++j;
            if (j < o.size() && o[j].lot == l.lot) l.pile = o[j].pile, l.last = o[j].last, l.shown = o[j].shown;
        }
        groups[g].swap(pending[g]), pending[g].clear(), hasPending[g] = 0;
    }
    void MergePending() { for (int g = 0; g < kGarbGroups; ++g) MergeGroup(g); }
    std::vector<SvcGarbLot> pending[kGarbGroups];
    uint8_t hasPending[kGarbGroups] = {};

    const SvcGarbLot* Find(uint64_t lot) const {
        const auto& g = groups[GarbGroupOf(lot)];
        const auto it = std::lower_bound(g.begin(), g.end(), lot, [](const SvcGarbLot& l, uint64_t v) { return l.lot < v; });
        return it != g.end() && it->lot == lot ? &*it : nullptr;
    }
    size_t Lots() const { size_t n = 0; for (const auto& g : groups) n += g.size(); return n; }

    // One group. Facilities' `took` is set. Lots whose piling state flipped go to `changed`, lots whose level moved a
    // tenth to `mirror` (lot, pile), trucks into `trips`.
    void Visit(int group, int64_t now, std::vector<SvcGarbFacility>& fac, std::vector<SvcTrip>& trips, std::vector<uint64_t>& changed,
               std::vector<std::pair<uint64_t, double>>& mirror, SvcGarbStats& st);
    void Clear() {
        for (int g = 0; g < kGarbGroups; ++g) groups[g].clear(), pending[g].clear(), hasPending[g] = 0;
        truckAcc.clear(), bucket.clear(), bucketAt = INT64_MIN;
    }
};

} // namespace city
} // namespace tbd

#include "sim_svc_garbage_visit.h"
