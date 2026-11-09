// Phase 4: one incident's step at its due minute (the states of sim_svc_incidents.h). Header only, headless.
#pragma once

#include "sim_svc_dispatch.h"

namespace tbd {
namespace city {

enum SvcIncFlag : uint8_t { kIfFullSaid = 1, kIfVehicle = 2 };

inline int SvcHoldMinutes(const SvcWorld& w, const SvcIncident& i, const SvcSite* s) {
    if (i.type == kIncCrime) return w.P.jailMinutes;
    if (i.type == kIncSick) return w.P.bedMinutes;
    return s && s->spec.burns ? w.P.cremateMinutes : -1; // a grave is for good
}

inline void SvcStepWaiting(SvcWorld& w, uint32_t slot, int64_t now) {
    SvcIncident& i = w.inc.inc[slot];
    const SvcParams& P = w.P;
    const int64_t waited = now - i.start;
    const bool burnt = i.type == kIncFire && waited >= P.burnMinutes, ruined = i.type == kIncFlood && waited >= P.floodMinutes;
    if (burnt || ruined) {
        w.today.unanswered[i.type]++;
        SvcDestroy(w, i.building, now);
        w.Event(SimEventType::Disaster, now, i.building, burnt ? 1 : 2);
        if (burnt) SvcSpread(w, i, now);
        SvcFinish(w, slot);
        return;
    }
    if ((i.type == kIncCrime && waited >= P.crimeGiveUp) || (i.type == kIncSick && waited >= P.sickGiveUp)) {
        w.today.unanswered[i.type]++;
        if (i.type == kIncCrime) { uint16_t& a = w.crimeAcc[PackKey(i.key)]; a = (uint16_t)std::min(400, a + 16); }
        SvcFinish(w, slot);
        return;
    }
    bool any = false, full = false;
    if (SvcFindResponder(w, i, now, any, full)) {
        const SvcSite* s = w.Site(i.responder);
        if (s && !(i.type == kIncSick && s->spec.vehicles == 0)) i.flags |= kIfVehicle;
        i.state = kIsEnRoute;
        w.inc.Schedule(slot, now + std::max<int64_t>(1, (int64_t)std::lround(i.travel)));
        return;
    }
    if (full && !(i.flags & kIfFullSaid)) i.flags |= kIfFullSaid, w.Event(SimEventType::ServiceFull, now, i.building, SvcSlotOf(i.type));
    w.inc.Schedule(slot, now + P.retryMinutes);
}

inline void SvcStepIncident(SvcWorld& w, uint32_t slot, int64_t now) {
    SvcIncident& i = w.inc.inc[slot];
    const SvcSite* s = w.Site(i.responder);
    switch (i.state) {
    case kIsWaiting: SvcStepWaiting(w, slot, now); return;
    case kIsEnRoute:
        w.AddActive(i.building, i.type, -1);
        w.today.responseMin += (double)(now - i.start), w.today.responses++;
        if (i.type == kIncFire && now - i.start >= w.P.burnMinutes) SvcDestroy(w, i.building, now); // too late: they save the rest
        i.state = (i.flags & kIfVehicle) ? kIsOnScene : kIsReturning; // a walk-in patient is at the clinic now
        if (!(i.flags & kIfVehicle)) i.carried = 1;
        w.inc.Schedule(slot, now + ((i.flags & kIfVehicle) ? SvcSceneMinutes(i) : 0));
        return;
    case kIsOnScene: {
        if (i.type == kIncCrime) {
            SplitMix64 r(SimSeed(w.seed, i.id, 0xA77Eull));
            const auto st = w.site.find(i.responder);
            i.carried = r.Below(1000) < (uint64_t)(w.P.arrestShare * 1000) && s && st != w.site.end() && st->second.used < s->spec.units;
            if (i.carried) w.Event(SimEventType::Arrest, now, i.building);
        } else if (i.type == kIncSick || i.type == kIncBody) {
            i.carried = 1;
            if (i.type == kIncBody) w.Event(SimEventType::BodyCollected, now, i.building);
        } else if (i.type == kIncFire) {
            w.Event(SimEventType::FireOut, now, i.building);
        }
        i.state = kIsReturning;
        w.inc.Schedule(slot, now + std::max<int64_t>(1, (int64_t)std::lround(i.travel)));
        if (i.flags & kIfVehicle) w.trips.push_back({i.node, s ? s->node : -1, now, (uint8_t)SvcSlotOf(i.type)});
        return;
    }
    case kIsReturning: {
        auto st = w.site.find(i.responder);
        if (st != w.site.end()) {
            if ((i.flags & kIfVehicle) && st->second.busy) --st->second.busy;
            st->second.handled++;
        }
        i.flags &= (uint8_t)~kIfVehicle;
        const int hold = SvcHoldMinutes(w, i, s);
        if (i.carried && st != w.site.end() && s && st->second.used < s->spec.units) {
            ++st->second.used;
            if (hold > 0) { i.state = kIsHeld, w.inc.Schedule(slot, now + hold); return; }
            ++st->second.graves;
        }
        SvcFinish(w, slot); // nobody to hold, or a grave (its place stays used)
        return;
    }
    case kIsHeld: {
        auto st = w.site.find(i.responder);
        if (st != w.site.end() && st->second.used) --st->second.used;
        if (i.type == kIncSick) w.Event(SimEventType::Healed, now, i.building);
        SvcFinish(w, slot);
        return;
    }
    default: SvcFinish(w, slot); return;
    }
}

// Every incident due at `now`, in (due, id) order; a bounded number per tick (the rest stay due: next tick).
inline int SvcTickDue(SvcWorld& w, int64_t now, int maxSteps = 2048) {
    uint32_t s;
    int n = 0;
    while (n < maxSteps && w.inc.PopDue(now, s)) SvcStepIncident(w, s, now), ++n;
    return n;
}

} // namespace city
} // namespace tbd
