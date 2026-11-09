// Phase 4 city services (CS2 sim roadmap Phase 4): the one header the host and the tests include. Header only, headless.
//   sim_svc_catalog.h    ServiceSpec per catalog building: capacity, units, vehicles, reach
//   sim_svc_coverage.h   graph snapshot, coverage input / output;  sim_svc_assign.h  the pass (worker)
//   sim_svc_incidents.h  incident store and time queue;  sim_svc_dispatch.h / sim_svc_step.h  responders, steps
//   sim_svc_rates.h      per-cell rates, crime and fire factors (round-robin groups)
//   sim_svc_flags.h      factors and flag bits (dirty buildings only)
//   sim_svc_education.h  school seats into CitizenStore::school;  sim_svc_garbage.h  piles, facilities, trucks
//   sim_svc_save.h       SERV INCD GARB
// SvcTick is all the per-minute work: the due incidents (usually none), one cell group every 15 minutes, the day's
// roll-over, and a bounded flags flush. Nothing in it loops over every cell, building or citizen.
#pragma once

#include <map> // the host's vehicle links (city_sim_p4_tick2.inl)

#include "sim_svc_save.h"

namespace tbd {
namespace city {

inline void SvcDayRollover(SvcWorld& w, int64_t minute) {
    w.yesterday = w.today, w.today = SvcDayStats();
    const int64_t keep = minute - 1440;
    w.trips.erase(std::remove_if(w.trips.begin(), w.trips.end(), [keep](const SvcTrip& t) { return t.minute < keep; }), w.trips.end());
    for (auto it = w.destroyedUntil.begin(); it != w.destroyedUntil.end();) {
        if (it->second <= minute) w.MarkDirty(it->first), it = w.destroyedUntil.erase(it);
        else ++it;
    }
    ++w.version;
}

// The per-minute work. `factors` may be null (tests without the shared table).
inline void SvcTick(SvcWorld& w, int64_t minute, CellFactors* factors) {
    SvcTickDue(w, minute);
    if (minute % 15 == 0) SvcVisitGroup(w, (size_t)((minute / 15) % kSvcGroups), minute, factors);
    if (minute % 1440 == 0 && minute > 0) SvcDayRollover(w, minute);
}

// The full recompute check for the incident side: vehicles out and units held per site, and the not-yet-attended
// counts per building, recounted from the incidents and compared with what the steps kept. 0: nothing stale.
inline int SvcVerifyCounts(const SvcWorld& w) {
    std::unordered_map<uint32_t, uint32_t> busy, held;
    std::unordered_map<uint64_t, uint16_t> act;
    for (const SvcIncident& i : w.inc.inc) {
        if (!i.id) continue;
        if (i.flags & 2 /* kIfVehicle */) busy[i.responder]++;
        if (i.state == kIsHeld) held[i.responder]++;
        if (i.state == kIsWaiting || i.state == kIsEnRoute) act[SvcWorld::ActiveKey(i.building, i.type)]++;
    }
    int bad = 0;
    for (const auto& [id, st] : w.site) {
        const auto b = busy.find(id), h = held.find(id);
        bad += st.busy != (b == busy.end() ? 0u : b->second);
        bad += st.used != st.graves + (h == held.end() ? 0u : h->second);
    }
    bad += act != w.active;
    return bad;
}

// City totals for the panels: per slot, sites, capacity, people served, vehicles and units in use.
struct SvcSlotTotals { int sites = 0; uint64_t serves = 0, served = 0, units = 0, used = 0; uint32_t vehicles = 0, busy = 0; double coverage = 0; };
inline SvcSlotTotals SvcTotals(const SvcWorld& w, int slot) {
    SvcSlotTotals t;
    for (size_t s = 0; s < w.cov.sites.size(); ++s) {
        const SvcSite& site = w.cov.sites[s];
        if (site.spec.slot != slot) continue;
        ++t.sites, t.serves += site.spec.serves, t.units += site.spec.units, t.vehicles += site.spec.vehicles;
        if (s < w.out.served.size()) t.served += w.out.served[s];
        const auto st = w.site.find(site.id);
        if (st != w.site.end()) t.busy += st->second.busy, t.used += st->second.used;
        if (site.spec.unit == kUnitSeats) { const auto e = w.enrolled.find(site.id); if (e != w.enrolled.end()) t.used += e->second; }
    }
    double people = 0, covered = 0;
    for (size_t c = 0; c < w.cov.cells.size(); ++c) people += w.cov.cells[c].people, covered += w.cov.cells[c].people * (w.out.cov[c][(size_t)slot] / 255.0);
    t.coverage = people > 0 ? covered / people : 0.0;
    return t;
}

} // namespace city
} // namespace tbd
