// Phase 4 school seats (CS2 sim roadmap Phase 4, sim_education): every student holds a seat in a school of the level
// they study at, local first (the coverage pass's nearest schools of the home cell, in travel-time order) and only
// while the school has seats left. The seat is written to CitizenStore::school (the Phase 1 field; its lifecycle
// graduates a seated student for certain, an unseated one by the coverage-scaled stub chance). Header only, headless
// (tests\sim_svc_education_test).
//
// Deferred: one slice of citizens per call, the whole store once a game day (the host's sweep). A citizen whose seat
// still fits costs a lookup. At the end of each sweep the seats are recounted from the records (the full recompute
// check): the count the sweep kept and the recount must agree unless Phase 1 freed or reused a record in between,
// which the recount corrects (reported as drift).
#pragma once

#include "sim_citizens.h"
#include "sim_svc_flags.h"

namespace tbd {
namespace city {

// The level a citizen needs a seat for: 0 none, 1 elementary, 2 high school, 3 college / university.
inline int SvcSeatLevel(uint8_t flags, uint8_t edu, int age) {
    if (!(flags & kCitAlive) || !(flags & kCitStudent)) return 0;
    if (age < 18) return edu == 0 ? 1 : 2;
    return edu >= 2 ? 3 : 0;
}
inline int SvcSeatSlot(int level) { return level == 1 ? kSvcSchool : level == 2 ? kSvcHighSchool : level == 3 ? kSvcUniversity : -1; }

struct SvcSeatPass {
    uint32_t next = 1;                        // the next citizen index of this sweep
    std::vector<uint16_t> unseatedNext;       // per cov cell, this sweep
    uint32_t drift = 0, sweeps = 0;           // last sweep's recount difference; sweeps done
    uint32_t seated = 0, unseated = 0, lastSeated = 0, lastUnseated = 0;
    std::unordered_map<uint32_t, uint32_t> counted; // seats seen this sweep (the recount, spread over the sweep)
};

// The kept counts against `fresh` (seats per site); returns the difference and takes `fresh`.
inline uint32_t SvcSeatTake(SvcWorld& w, std::unordered_map<uint32_t, uint32_t>& fresh) {
    uint32_t off = 0;
    for (const auto& [id, k] : fresh) { const auto it = w.enrolled.find(id); off += (uint32_t)std::abs((int)k - (int)(it == w.enrolled.end() ? 0 : it->second)); }
    for (const auto& [id, k] : w.enrolled) if (!fresh.count(id)) off += k;
    w.enrolled.swap(fresh);
    fresh.clear();
    return off;
}

inline void SvcSeatRelease(SvcWorld& w, CitizenStore& cit, uint32_t ci) {
    const uint32_t s = cit.school[ci];
    if (!s) return;
    auto it = w.enrolled.find(s);
    if (it != w.enrolled.end() && it->second) --it->second;
    cit.school[ci] = 0;
}

// The full recompute check: every seat recounted from the records at once (tests, debug). Returns how far the kept
// counts were off, and takes the recount.
inline uint32_t SvcSeatRecount(SvcWorld& w, const CitizenStore& cit) {
    std::unordered_map<uint32_t, uint32_t> n;
    for (uint32_t ci = 1; ci < cit.school.size(); ++ci)
        if (cit.school[ci] && (cit.flags[ci] & kCitAlive) && w.Site(cit.school[ci])) ++n[cit.school[ci]];
    return SvcSeatTake(w, n);
}

// Visits up to `count` citizens. ageOf(ci) and homeCell(ci) (cov cell index, -1 unknown) come from Phase 1.
template <class AgeFn, class CellFn>
inline void SvcSeatSlice(SvcWorld& w, CitizenStore& cit, SvcSeatPass& p, uint32_t count, AgeFn&& ageOf, CellFn&& homeCell) {
    const uint32_t cap = (uint32_t)cit.school.size();
    if (p.unseatedNext.size() != w.cov.cells.size()) p.unseatedNext.assign(w.cov.cells.size(), 0);
    for (uint32_t k = 0; k < count; ++k) {
        if (p.next >= cap) { // end of a sweep: recount, publish the unseated cells, start over
            p.drift = SvcSeatTake(w, p.counted), ++p.sweeps;
            for (size_t c = 0; c < p.unseatedNext.size() && c < w.unseated.size() && c + 1 < w.cellBldOff.size(); ++c)
                if ((w.unseated[c] > 0) != (p.unseatedNext[c] > 0))
                    for (int q = w.cellBldOff[c]; q < w.cellBldOff[c + 1]; ++q) w.MarkDirty(w.bld[(size_t)w.cellBldIdx[(size_t)q]].id);
            w.unseated = p.unseatedNext, std::fill(p.unseatedNext.begin(), p.unseatedNext.end(), (uint16_t)0);
            if (!w.eduSwept) { w.eduSwept = true; for (const SvcSite& s : w.cov.sites) if (s.spec.unit == kUnitSeats) w.MarkDirty(s.id); }
            p.lastSeated = p.seated, p.lastUnseated = p.unseated, p.seated = p.unseated = 0;
            p.next = 1, ++w.version;
            return;
        }
        const uint32_t ci = p.next++;
        const bool alive = cit.At(ci).Valid() && (cit.flags[ci] & kCitAlive);
        const int level = alive ? SvcSeatLevel(cit.flags[ci], cit.edu[ci], ageOf(ci)) : 0;
        const int slot = SvcSeatSlot(level);
        const SvcSite* cur = cit.school[ci] ? w.Site(cit.school[ci]) : nullptr;
        if (cur && cur->spec.slot == slot) { ++p.seated, ++p.counted[cit.school[ci]]; continue; } // still the right school
        SvcSeatRelease(w, cit, ci);
        if (slot < 0) continue;
        const int cell = homeCell(ci);
        if (cell < 0 || (size_t)cell >= w.cov.cells.size()) continue;
        bool got = false;
        for (int n = 0; n < kSvcNear && !got; ++n) {
            const SvcNear& nr = w.out.Near((size_t)cell, slot, n);
            if (nr.site < 0) break;
            const SvcSite& s = w.cov.sites[(size_t)nr.site];
            uint32_t& e = w.enrolled[s.id];
            if (e < s.spec.units) ++e, cit.school[ci] = s.id, got = true;
        }
        if (got) ++p.seated, ++p.counted[cit.school[ci]];
        else if (w.out.slotHasSite[(size_t)slot]) ++p.unseated, p.unseatedNext[(size_t)cell] = (uint16_t)std::min(65535, p.unseatedNext[(size_t)cell] + 1);
    }
}

} // namespace city
} // namespace tbd
