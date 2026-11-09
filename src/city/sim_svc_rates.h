// Phase 4 incident rates per cell (design doc 5 and 11), one round-robin group of cells per 15-minute window. Header
// only, headless (tests\sim_svc_incidents_test). A rate decides THAT incidents happen in a cell today (a Poisson draw
// seeded by the cell's key and the day); a building is picked only then, weighted by its homes and jobs.
//   crime  crimePerK a year per 1,000 residents x (0.7 + 6 x unemployment + rent burden above 25 %) x police
//   fire   firePerKHomes per 1,000 homes (jobs count a tenth) x (1 + industry share / 2) x density x fire coverage
//   sick   sickPerK per 1,000 residents x (1 + pollution) x health coverage
// "x coverage" = 1.4 - 0.8 x coverage / 255 (1 at a middling 128). The factors crime and fireRisk are written here:
// crime = 32 x crime multiplier + the cell's unanswered-crime pressure (which loses a quarter each day).
#pragma once

#include <cmath>

#include "sim_svc_step.h"

namespace tbd {
namespace city {

inline float SvcCoverMult(uint8_t cov) { return 1.4f - 0.8f * (float)cov / 255.0f; }

struct SvcCellRates { float crime = 0, fire = 0, sick = 0, crimeMult = 1, fireMult = 1; };

inline SvcCellRates SvcRatesOf(const SvcWorld& w, int c, const CellFactor& f) {
    SvcCellRates r;
    const SvcCellStat& s = w.cellStat[(size_t)c];
    const auto& cov = w.out.cov[(size_t)c];
    const float day = 1.0f / std::max(1.0f, w.P.yearDays);
    r.crimeMult = std::max(0.2f, 0.7f + 6.0f * f.unemployment / 255.0f + ((float)f.rentBurden - 64.0f) / 255.0f) * SvcCoverMult(cov[kSvcPolice]);
    const float ind = s.jobs ? (float)s.industryJobs / (float)s.jobs : 0.0f;
    r.fireMult = (1.0f + 0.5f * ind) * (s.maxLevel >= 3 ? 1.2f : 1.0f) * SvcCoverMult(cov[kSvcFire]);
    const float poll = ((float)f.air + (float)f.water + (float)f.ground * 0.5f) / 128.0f;
    r.crime = w.P.crimePerK / 1000.0f * (float)s.residents * r.crimeMult * day;
    r.fire = w.P.firePerKHomes / 1000.0f * ((float)s.homes + (float)s.jobs * 0.1f) * r.fireMult * day;
    r.sick = w.P.sickPerK / 1000.0f * (float)s.residents * (1.0f + poll) * SvcCoverMult(cov[kSvcHealth]) * day;
    return r;
}

// Poisson(lambda) from one generator (inversion; lambda is small per cell and day).
inline int SvcPoisson(SplitMix64& r, float lambda) {
    if (!(lambda > 0)) return 0;
    const double u = (double)(r.Next() >> 11) * (1.0 / 9007199254740992.0);
    double p = std::exp(-(double)lambda), cdf = p;
    int k = 0;
    while (u > cdf && k < 200) ++k, p *= lambda / k, cdf += p;
    return k;
}

// A building of cov cell c picked by weight (homes / jobs per type), 0 if none.
inline uint32_t SvcPickBuilding(const SvcWorld& w, int c, int type, SplitMix64& r) {
    if ((size_t)c + 1 >= w.cellBldOff.size()) return 0;
    auto weight = [type](const SvcBldg& b) -> uint32_t {
        if (b.kind == kBkService) return 0;
        if (type == kIncSick) return b.homes;
        if (type == kIncCrime) return b.homes + (b.kind == kBkShop ? 2u : 1u) * b.jobs;
        return b.homes + b.jobs / 4u + 1u;
    };
    uint64_t total = 0;
    for (int k = w.cellBldOff[(size_t)c]; k < w.cellBldOff[(size_t)c + 1]; ++k) total += weight(w.bld[(size_t)w.cellBldIdx[(size_t)k]]);
    if (!total) return 0;
    uint64_t pick = ((r.Next() >> 11) % total);
    for (int k = w.cellBldOff[(size_t)c]; k < w.cellBldOff[(size_t)c + 1]; ++k) {
        const SvcBldg& b = w.bld[(size_t)w.cellBldIdx[(size_t)k]];
        const uint32_t wt = weight(b);
        if (pick < wt) return w.destroyedUntil.count(b.id) ? 0u : b.id;
        pick -= wt;
    }
    return 0;
}

// The window's group: today's incidents of its cells (each at a seeded minute inside the next window), crime decay and
// the crime / fireRisk factors. `factors` is indexed like cov.cells (the host keeps them in CitySim order).
inline void SvcVisitGroup(SvcWorld& w, size_t group, int64_t minute, CellFactors* factors) {
    if (group >= w.cellGroups.size()) return;
    const int64_t day = minute / 1440;
    static const CellFactor kNeutral;
    for (int c : w.cellGroups[group]) {
        const CellKey k = w.cov.cells[(size_t)c].key;
        const CellFactor& f = factors ? factors->At(c) : kNeutral;
        const SvcCellRates rt = SvcRatesOf(w, c, f);
        SplitMix64 r(SimSeed(w.seed ^ PackKey(k), (uint64_t)day, 0x1C1Dull));
        const float lam[3] = {rt.crime, rt.fire, rt.sick};
        static const uint8_t kType[3] = {kIncCrime, kIncFire, kIncSick};
        for (int t = 0; t < 3; ++t) {
            const int n = SvcPoisson(r, lam[t]);
            for (int j = 0; j < n; ++j) {
                const uint32_t b = SvcPickBuilding(w, c, kType[t], r);
                const int64_t at = minute + (int64_t)r.Below(15);
                if (b) SvcReport(w, kType[t], b, at, kType[t] == kIncFire ? (uint16_t)(20 + r.Below(70)) : 0);
            }
        }
        const auto it = w.crimeAcc.find(PackKey(k));
        int acc = 0;
        if (it != w.crimeAcc.end()) {
            it->second = (uint16_t)(it->second * 3 / 4), acc = it->second;
            if (!acc) w.crimeAcc.erase(it);
        }
        const uint8_t crime = (uint8_t)std::clamp((int)std::lround(32.0f * rt.crimeMult) + acc, 0, 255);
        const uint8_t fire = (uint8_t)std::clamp((int)std::lround(64.0f * rt.fireMult), 0, 255);
        if (crime != w.crimeF[(size_t)c] || fire != w.fireF[(size_t)c]) {
            if ((crime >= w.P.flagCrime) != (w.crimeF[(size_t)c] >= w.P.flagCrime) && (size_t)c + 1 < w.cellBldOff.size())
                for (int q = w.cellBldOff[(size_t)c]; q < w.cellBldOff[(size_t)c + 1]; ++q) w.MarkDirty(w.bld[(size_t)w.cellBldIdx[(size_t)q]].id);
            w.crimeF[(size_t)c] = crime, w.fireF[(size_t)c] = fire, ++w.version;
        }
        if (CellFactor* m = factors ? factors->Mut(c) : nullptr) m->crime = crime, m->fireRisk = fire;
    }
}

} // namespace city
} // namespace tbd
