// Phase 4 dispatch: an incident's next step when its minute comes (see sim_svc_incidents.h for the states). Header
// only, headless. A vehicle comes from the nearest site (the coverage pass's nearest list for the incident's cell) that
// has a vehicle free and, if it brings someone back, a unit free; it drives free-flow minutes x the cell's traffic delay.
#pragma once

#include "sim_svc_apply.h"

namespace tbd {
namespace city {

inline int SvcSlotOf(int type) {
    return type == kIncCrime ? kSvcPolice : type == kIncSick ? kSvcHealth : type == kIncBody ? kSvcDeathcare : kSvcFire;
}
inline bool SvcCarries(int type) { return type == kIncCrime || type == kIncSick || type == kIncBody; }
inline int SvcSceneMinutes(const SvcIncident& i) {
    return i.type == kIncFire ? 30 + i.severity : i.type == kIncFlood ? 60 : i.type == kIncCrime ? 20 : 10;
}

// A new incident at building b (its cell and node from the building list). `delayMin`: when it starts.
inline uint32_t SvcReport(SvcWorld& w, uint8_t type, uint32_t b, int64_t minute, uint16_t severity = 0) {
    const SvcBldg* B = w.Bldg(b);
    if (!B) return 0;
    for (const SvcIncident& o : w.inc.inc) // one fire (or flood) per building at a time
        if (o.id && o.building == b && o.type == type && (type == kIncFire || type == kIncFlood)) return 0;
    SvcIncident& i = w.inc.Add(type, b, B->key, B->node, minute, minute);
    i.severity = severity;
    w.today.n[type]++;
    w.AddActive(b, type, +1);
    static const SimEventType kEv[kIncTypes] = {SimEventType::Crime, SimEventType::Fire, SimEventType::Sick, SimEventType::None, SimEventType::Disaster};
    if (kEv[type] != SimEventType::None) w.Event(kEv[type], minute, b, type == kIncFlood ? 2 : severity);
    return i.id;
}

// Picks the responder for slot s's incident: true with i.responder and i.travel set (a vehicle taken).
inline bool SvcFindResponder(SvcWorld& w, SvcIncident& i, int64_t minute, bool& anySite, bool& full) {
    const int cell = w.CellOf(i.key), slot = SvcSlotOf(i.type);
    anySite = cell >= 0 && w.out.Near((size_t)cell, slot, 0).site >= 0;
    full = false;
    if (cell < 0) return false;
    for (int k = 0; k < kSvcNear; ++k) {
        const SvcNear& n = w.out.Near((size_t)cell, slot, k);
        if (n.site < 0) break;
        const SvcSite& s = w.cov.sites[(size_t)n.site];
        SvcSiteState& st = w.site[s.id];
        const bool walkIn = i.type == kIncSick && s.spec.vehicles == 0;
        if (!walkIn && st.busy >= s.spec.vehicles) continue;
        if (SvcCarries(i.type) && i.type != kIncCrime && st.used >= s.spec.units) { full = true; continue; }
        if (!walkIn) ++st.busy;
        i.responder = s.id, i.travel = n.minutes * w.Delay(i.key);
        if (!walkIn) w.trips.push_back({s.node, i.node, minute, (uint8_t)slot});
        return true;
    }
    return false;
}

inline void SvcFinish(SvcWorld& w, uint32_t slot) {
    SvcIncident& i = w.inc.inc[slot];
    if (i.state == kIsWaiting || i.state == kIsEnRoute) w.AddActive(i.building, i.type, -1);
    w.inc.Free(slot);
}

inline void SvcDestroy(SvcWorld& w, uint32_t b, int64_t minute) {
    w.destroyedUntil[b] = minute + w.P.destroyedMinutes;
    w.today.destroyed++;
    w.MarkDirty(b);
}

// Fire spread (the simple disaster): another building of the cell catches fire, picked by the hash of the old one.
inline void SvcSpread(SvcWorld& w, const SvcIncident& i, int64_t minute) {
    const int cell = w.CellOf(i.key);
    if (cell < 0 || (size_t)cell + 1 >= w.cellBldOff.size()) return;
    const int a = w.cellBldOff[(size_t)cell], n = w.cellBldOff[(size_t)cell + 1] - a;
    SplitMix64 r(SimSeed(w.seed, i.id, 0x5B3Eull));
    if (n < 2 || r.Below(1000) >= (uint64_t)(w.P.spreadChance * 1000)) return;
    const SvcBldg& to = w.bld[(size_t)w.cellBldIdx[(size_t)(a + (int)r.Below((uint32_t)n))]];
    if (to.id != i.building && !w.destroyedUntil.count(to.id)) SvcReport(w, kIncFire, to.id, minute + 5, (uint16_t)(20 + r.Below(60)));
}

} // namespace city
} // namespace tbd
