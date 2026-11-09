// Phase 4 garbage: one group's visit (SvcGarbage::Visit, see sim_svc_garbage.h). Header only, headless.
#pragma once

#include <algorithm>
#include <cmath>

namespace tbd {
namespace city {

inline void SvcGarbage::Visit(int group, int64_t now, std::vector<SvcGarbFacility>& fac, std::vector<SvcTrip>& trips, std::vector<uint64_t>& changed,
                              std::vector<std::pair<uint64_t, double>>& mirror, SvcGarbStats& st) {
    if (group < 0 || group >= kGarbGroups) return;
    MergeGroup(group); // a re-list parked by Adopt lands now, for this group only
    std::vector<SvcGarbLot>& lots = groups[group];
    // each facility's capacity refills its bucket with time (at most an hour's worth), whatever the groups' sizes
    const int64_t dt = bucketAt == INT64_MIN ? 1 : std::clamp<int64_t>(now - bucketAt, 0, 60);
    bucketAt = now;
    std::vector<double*> budget(fac.size());
    for (size_t f = 0; f < fac.size(); ++f) {
        double& b = bucket[fac[f].id];
        b = std::min(b + fac[f].perMonth * (double)dt / kGarbMonthMinutes, fac[f].perMonth * 60.0 / kGarbMonthMinutes);
        budget[f] = &b, fac[f].took = 0;
    }
    struct Want { float d; uint32_t li; int f; };
    std::vector<Want> wants;
    std::vector<uint8_t> was(lots.size());
    for (uint32_t li = 0; li < (uint32_t)lots.size(); ++li) {
        SvcGarbLot& l = lots[li];
        const int64_t elapsed = l.last == INT64_MIN ? 60 : std::max<int64_t>(0, now - l.last);
        l.last = now;
        was[li] = l.Piling();
        l.pile += l.perMonth * (double)elapsed / kGarbMonthMinutes;
        st.produced += l.perMonth, ++st.lots;
        int best = -1;
        float bestD = 1e30f;
        for (int f = 0; f < (int)fac.size(); ++f) {
            const SvcGarbFacility& F = fac[(size_t)f];
            if (F.perMonth <= 0 || F.comp < 0 || F.comp != l.comp) continue;
            float d = std::sqrt((F.x - l.x) * (F.x - l.x) + (F.z - l.z) * (F.z - l.z));
            if (d > F.range) continue;
            if (F.hazardous) d = l.industrial ? d * 0.25f : d * 4.0f;
            if (d < bestD) bestD = d, best = f;
        }
        if (best < 0) ++st.unserved;
        else wants.push_back({bestD, li, best});
    }
    std::sort(wants.begin(), wants.end(), [&lots](const Want& a, const Want& b) { return a.d != b.d ? a.d < b.d : lots[a.li].lot < lots[b.li].lot; });
    for (const Want& w : wants) {
        SvcGarbLot& l = lots[w.li];
        const double take = std::min(l.pile, *budget[(size_t)w.f]);
        if (take <= 0) continue;
        l.pile -= take, *budget[(size_t)w.f] -= take, fac[(size_t)w.f].took += (float)take, st.collected += take;
        const SvcGarbFacility& F = fac[(size_t)w.f];
        float& acc = truckAcc[(uint64_t)F.id << 32 | (uint32_t)l.node];
        acc += (float)take;
        for (; acc >= truckTonnes; acc -= truckTonnes) trips.push_back({F.node, l.node, now, (uint8_t)kSvcGarbage}), ++st.trucks;
    }
    for (uint32_t li = 0; li < (uint32_t)lots.size(); ++li) {
        SvcGarbLot& l = lots[li];
        st.uncollected += l.pile, st.piling += l.Piling();
        if (l.Piling() != (was[li] != 0)) changed.push_back(l.lot);
        const uint8_t step = (uint8_t)(l.Level() * 10.0f);
        if (step != l.shown) l.shown = step, mirror.push_back({l.lot, l.pile});
    }
}

} // namespace city
} // namespace tbd
