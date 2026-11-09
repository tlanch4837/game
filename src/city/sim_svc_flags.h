// Phase 4 outputs: the factors coverage[] telecom parkAccess (on each applied coverage pass) and this phase's flag bits
// per building (only the buildings marked dirty, a bounded number per tick). Header only, headless.
//   Fire, Sick, DeadUncollected   an incident at the building nobody has reached yet
//   Destroyed                     burnt down or ruined by a flood, until the rubble is cleared
//   Crime                         its cell's crime factor at svc flagCrime or more
//   NoHealthcare, NoMail          no coverage of the kind while the city has such a service (homes; mail: any workplace too)
//   NoTelecom                     an office with poor network coverage while the city has a mast
//   NoSchool                      a home in a cell with students that found no seat
//   NoStudents                    a school with under a tenth of its seats taken, after the seat pass's first sweep
//   Garbage                       the host's pile level (1: piling up) for zoned lots
#pragma once

#include "sim_svc_rates.h"

namespace tbd {
namespace city {

constexpr SimFlags kSvcFlagMask = kFlagSick | kFlagCrime | kFlagFire | kFlagGarbage | kFlagNoSchool | kFlagNoStudents | kFlagNoTelecom |
                                  kFlagNoHealthcare | kFlagDeadUncollected | kFlagNoMail | kFlagDestroyed;

inline void SvcWriteCoverageFactors(const SvcWorld& w, CellFactors& f) {
    f.Fit(w.cov.cells.size());
    for (size_t c = 0; c < w.cov.cells.size(); ++c) {
        CellFactor& m = f.cells[c];
        const auto& cv = w.out.cov[c];
        for (int s = 0; s < kServicesV2; ++s)
            if (s != kSvcGarbage) m.coverage[s] = cv[(size_t)s]; // garbage: written from the piles (the host's garbage visit)
        m.telecom = w.out.slotHasSite[kSvcTelecom] ? std::max<uint8_t>(48, cv[kSvcTelecom]) : (uint8_t)128;
        m.parkAccess = cv[kSvcPark];
        if (c < w.crimeF.size()) m.crime = w.crimeF[c], m.fireRisk = w.fireF[c];
    }
    ++f.version;
}

using SvcLotLevelFn = std::function<float(uint64_t lot)>;

inline SimFlags SvcBuildingFlags(const SvcWorld& w, const SvcBldg& b, int64_t now, const SvcLotLevelFn& garbage) {
    SimFlags f = 0;
    if (w.Active(b.id, kIncFire) || w.Active(b.id, kIncFlood)) f |= kFlagFire;
    if (w.Active(b.id, kIncSick)) f |= kFlagSick;
    if (w.Active(b.id, kIncBody)) f |= kFlagDeadUncollected;
    const auto d = w.destroyedUntil.find(b.id);
    if (d != w.destroyedUntil.end() && d->second > now) f |= kFlagDestroyed;
    const int c = w.CellOfB(b);
    if (c >= 0) {
        const auto& cv = w.out.cov[(size_t)c];
        const auto has = [&](int s) { return w.out.slotHasSite[(size_t)s] != 0; };
        if ((size_t)c < w.crimeF.size() && w.crimeF[(size_t)c] >= w.P.flagCrime && b.kind != kBkService) f |= kFlagCrime;
        if (b.kind == kBkHome && has(kSvcHealth) && cv[kSvcHealth] == 0) f |= kFlagNoHealthcare;
        if (b.kind != kBkService && b.kind != kBkOther && has(kSvcPost) && cv[kSvcPost] < 40) f |= kFlagNoMail;
        if (b.kind == kBkOffice && has(kSvcTelecom) && cv[kSvcTelecom] < 40) f |= kFlagNoTelecom;
        if (b.kind == kBkHome && (size_t)c < w.unseated.size() && w.unseated[(size_t)c] > 0) f |= kFlagNoSchool;
    }
    if (b.kind == kBkService && w.eduSwept) {
        const SvcSite* s = w.Site(b.id);
        if (s && s->spec.unit == kUnitSeats && s->spec.units >= 10) {
            const auto e = w.enrolled.find(b.id);
            if ((e == w.enrolled.end() ? 0u : e->second) * 10 < s->spec.units) f |= kFlagNoStudents;
        }
    }
    if (b.lot && garbage && garbage(b.lot) >= 1.0f) f |= kFlagGarbage;
    return f;
}

// Up to `maxN` buildings into the table (P4's bits only): the dirty ones first, then the next stretch of a full sweep
// (after a coverage pass or a load). Returns how many were looked at.
inline int SvcFlushFlags(SvcWorld& w, SimFlagTable& table, int64_t now, const SvcLotLevelFn& garbage, int maxN = 160) {
    int n = 0;
    size_t i = 0;
    for (; i < w.dirty.size() && n < maxN; ++i, ++n) {
        const uint32_t id = w.dirty[i];
        w.dirtyMark.erase(id);
        const SvcBldg* b = w.Bldg(id);
        table.Write(BuildingId(id), kSvcFlagMask, b ? SvcBuildingFlags(w, *b, now, garbage) : 0u);
    }
    w.dirty.erase(w.dirty.begin(), w.dirty.begin() + (std::ptrdiff_t)i);
    for (; w.flagSweep >= 0 && n < maxN; ++n) {
        if ((size_t)w.flagSweep >= w.bld.size()) { w.flagSweep = -1; break; }
        const SvcBldg& b = w.bld[(size_t)w.flagSweep++];
        table.Write(BuildingId(b.id), kSvcFlagMask, SvcBuildingFlags(w, b, now, garbage));
    }
    return n;
}
inline bool SvcFlagsSettled(const SvcWorld& w) { return w.dirty.empty() && w.flagSweep < 0; }

// The full recompute check: every building's bits from scratch against the table. Returns the mismatches (0: the
// incremental flags are not stale). Only meaningful with no dirty buildings left.
inline int SvcVerifyFlags(const SvcWorld& w, const SimFlagTable& table, int64_t now, const SvcLotLevelFn& garbage) {
    int bad = 0;
    for (const SvcBldg& b : w.bld) bad += (table.Get(BuildingId(b.id)) & kSvcFlagMask) != SvcBuildingFlags(w, b, now, garbage);
    return bad;
}

} // namespace city
} // namespace tbd
