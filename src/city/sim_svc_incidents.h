// Phase 4 incidents: crime, fire, sickness, bodies, flood damage (CS2 sim roadmap Phase 4). Header only, headless
// (tests\sim_svc_incidents_test).
//
// Deferred: an incident is a small plan with a next-attention minute (`due`). Live incidents sit in one time-ordered
// queue (due, then id: a stable tie-break), so an incident costs nothing until its minute comes; a tick pops only what
// is due. A step bumps `step`, and a queue entry whose step no longer matches is stale and skipped (no erase).
//   Waiting    no vehicle free yet (or no service at all): tried again later; a fire that waits too long burns the
//              building down (and may spread), a crime nobody answers raises the cell's crime
//   EnRoute    a vehicle drives there (free-flow minutes x the traffic delay of the cell)
//   OnScene    the work (arrest, fire fight, first aid, collecting the body, pumping)
//   Returning  back to its base with what it carries (prisoner, patient, body)
//   Held       the carried one takes a unit (jail cell, bed, cold-room place) until released; graves are for good
// The queue is not saved: it is rebuilt from the incidents on load (same order: due, id).
#pragma once

#include <cstdint>
#include <queue>
#include <vector>

#include "city_cells.h"

namespace tbd {
namespace city {

enum SvcIncType : uint8_t { kIncCrime = 0, kIncFire, kIncSick, kIncBody, kIncFlood, kIncTypes };
enum SvcIncState : uint8_t { kIsFree = 0, kIsWaiting, kIsEnRoute, kIsOnScene, kIsReturning, kIsHeld };

inline const char* SvcIncName(int t) {
    static const char* const k[kIncTypes] = {"Crime", "Fire", "Sickness", "Death", "Flood damage"};
    return t >= 0 && t < kIncTypes ? k[t] : "";
}

struct SvcIncident {
    uint32_t id = 0;          // 0: a free slot
    uint8_t type = kIncCrime, state = kIsFree, carried = 0, flags = 0;
    uint16_t step = 0, severity = 0;
    uint32_t building = 0;    // BuildingId it happened at
    CellKey key{};            // its cell
    int32_t node = -1;        // its road node
    uint32_t responder = 0;   // site id of the vehicle's (or the patient's) base, 0 none yet
    float travel = 0;         // minutes the vehicle drives one way
    int64_t start = 0, due = 0;
};

class SvcIncidentStore {
public:
    std::vector<SvcIncident> inc; // slots
    uint32_t nextId = 1;
    uint32_t Live() const { return live_; }

    SvcIncident& Add(uint8_t type, uint32_t building, CellKey key, int node, int64_t start, int64_t due) {
        uint32_t slot;
        if (!free_.empty()) slot = free_.back(), free_.pop_back();
        else slot = (uint32_t)inc.size(), inc.emplace_back();
        SvcIncident& i = inc[slot];
        i = SvcIncident();
        i.id = nextId++, i.type = type, i.state = kIsWaiting, i.building = building, i.key = key, i.node = node, i.start = start;
        ++live_;
        Schedule(slot, due);
        return i;
    }
    // Next attention of slot s at `due` (the old queue entry goes stale).
    void Schedule(uint32_t s, int64_t due) {
        SvcIncident& i = inc[s];
        i.due = due, ++i.step;
        q_.push({due, i.id, s, i.step});
    }
    void Free(uint32_t s) {
        inc[s] = SvcIncident();
        free_.push_back(s), --live_;
    }
    // The next due slot at or before `minute` (true), stale entries dropped on the way.
    bool PopDue(int64_t minute, uint32_t& slot) {
        while (!q_.empty()) {
            const Entry e = q_.top();
            if (e.due > minute) return false;
            q_.pop();
            const SvcIncident& i = inc[e.slot];
            if (i.id == e.id && i.step == e.step && i.state != kIsFree) { slot = e.slot; return true; }
        }
        return false;
    }
    size_t Queued() const { return q_.size(); }
    // The free slots in reuse order (saved, so a loaded city hands out the same slots as the one that never stopped).
    const std::vector<uint32_t>& FreeList() const { return free_; }
    // After a load (inc and the saved free list filled): the queue from the live slots, in (due, id) order. False when
    // the free list does not match the free slots (then it is rebuilt from them).
    bool Rebuild(const std::vector<uint32_t>& savedFree) {
        live_ = 0;
        q_ = decltype(q_)();
        size_t freeSlots = 0;
        for (uint32_t s = 0; s < (uint32_t)inc.size(); ++s) {
            if (inc[s].state == kIsFree || !inc[s].id) { inc[s] = SvcIncident(), ++freeSlots; continue; }
            ++live_, q_.push({inc[s].due, inc[s].id, s, inc[s].step});
        }
        bool ok = savedFree.size() == freeSlots;
        for (uint32_t s : savedFree) ok = ok && s < inc.size() && inc[s].state == kIsFree;
        if (ok) { free_ = savedFree; return true; }
        free_.clear();
        for (uint32_t s = (uint32_t)inc.size(); s-- > 0;) if (inc[s].state == kIsFree) free_.push_back(s);
        return false;
    }
    void Clear() { inc.clear(), free_.clear(), q_ = decltype(q_)(), live_ = 0, nextId = 1; }

private:
    struct Entry {
        int64_t due;
        uint32_t id, slot;
        uint16_t step;
        bool operator>(const Entry& o) const { return due != o.due ? due > o.due : id > o.id; }
    };
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> q_;
    std::vector<uint32_t> free_;
    uint32_t live_ = 0;
};

} // namespace city
} // namespace tbd
