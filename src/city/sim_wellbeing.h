// Health and happiness of a citizen (CS2 sim roadmap Phase 1): the ONLY place these formulas live. Other phases add
// factors (sim_factors.h CellFactor, PolicyMods), never formulas. Header only, headless (tests\sim_lifecycle_test).
//
// Happiness target = 128 + the sum of named terms (points), clamped 0..255; the citizen drifts towards it by
// life.happyDrift of the gap each month. The same terms feed the inspector's "top two reasons". At neutral factors an
// employed, healthy, modest citizen sits near 140; services, parks and telecom add, pollution, crime, rent and taxes take.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sim_life_world.h"

namespace tbd {
namespace city {

enum LifeReason : int {
    kWhyHealth = 0, kWhyWealth, kWhyJob, kWhyOverqualified, kWhyCommute, kWhyPollution, kWhyNoise, kWhyCrime,
    kWhyServices, kWhyParks, kWhyTelecom, kWhyRent, kWhyTax, kWhyGoods, kWhyCrowded, kWhyPolicy, kWhyCount
};
inline const char* LifeReasonName(int r) {
    static const char* const k[kWhyCount] = {"Health", "Wealth", "Work", "Overqualified", "Commute", "Pollution", "Noise", "Crime",
                                             "Services", "Parks", "Telecom", "Rent", "Taxes", "Goods", "Crowded", "Policies"};
    return r >= 0 && r < kWhyCount ? k[r] : "";
}

struct LifeTerms {
    int v[kWhyCount] = {};
    int Sum() const { int s = 0; for (int x : v) s += x; return s; }
    // The two terms that move this person most (by size), worst first when equal. -1 when nothing moves them.
    void Top2(int out[2]) const {
        out[0] = out[1] = -1;
        for (int i = 0; i < kWhyCount; ++i) {
            if (!v[i]) continue;
            const auto better = [&](int a, int b) { return b < 0 || std::abs(v[a]) > std::abs(v[b]) || (std::abs(v[a]) == std::abs(v[b]) && v[a] < v[b]); };
            if (better(i, out[0])) out[1] = out[0], out[0] = i;
            else if (better(i, out[1])) out[1] = i;
        }
    }
};

inline const LifeBuilding* LifeHomeOf(const LifeWorld& w, uint32_t ci) {
    const uint32_t h = w.cit.household[ci];
    if (!h) return nullptr;
    const uint32_t b = w.hh.home[LifeWorld::Ix(h)], i = LifeWorld::Ix(b);
    return b && i < w.bld.size() && w.bld[i].id == b ? &w.bld[i] : nullptr;
}
inline bool LifeIsWorkingAge(const LifeWorld& w, uint32_t ci) {
    return w.cit.stage[ci] == kLifeAdult && !(w.cit.flags[ci] & (kCitStudent | kCitRetired));
}

inline void LifeHappyTerms(const LifeWorld& w, uint32_t ci, LifeTerms& t) {
    t = LifeTerms();
    const LifeBuilding* home = LifeHomeOf(w, ci);
    const CellFactor& f = w.Factor(home ? home->cell : -1);
    const uint32_t hi = LifeWorld::Ix(w.cit.household[ci]);
    static const int kWealth[4] = {-14, -4, 6, 12};
    t.v[kWhyHealth] = ((int)w.cit.health[ci] - 160) / 5;
    t.v[kWhyWealth] = kWealth[w.hh.wealth[hi] & 3];
    if (LifeIsWorkingAge(w, ci)) t.v[kWhyJob] = w.cit.work[ci] ? 6 : -28;
    if (w.cit.work[ci]) {
        const int over = (int)w.cit.edu[ci] - (int)w.cit.jobLevel[ci];
        if (over > 0) t.v[kWhyOverqualified] = -std::min(18, 6 * over);
        if (w.cit.commuteMin[ci] > 25) t.v[kWhyCommute] = -std::min(30, ((int)w.cit.commuteMin[ci] - 25) / 2);
    }
    t.v[kWhyPollution] = -((int)f.air / 8 + (int)f.ground / 12 + (int)f.water / 8);
    t.v[kWhyNoise] = -(int)f.noise / 10;
    t.v[kWhyCrime] = -(int)f.crime / 8;
    int cov = 0;
    for (int s = 0; s < 6; ++s) cov += f.coverage[s];
    t.v[kWhyServices] = cov / 40;
    t.v[kWhyParks] = (int)f.parkAccess / 24;
    t.v[kWhyTelecom] = ((int)f.telecom - 128) / 24;
    t.v[kWhyRent] = -((int)f.rentBurden - 64) / 5;
    t.v[kWhyTax] = -((int)f.taxBurden - 64) / 5;
    t.v[kWhyGoods] = -(255 - (int)f.goodsAvailable) / 12;
    if (w.hh.flags[hi] & kHhCrowded) t.v[kWhyCrowded] = -10;
    if (w.factors && home) t.v[kWhyPolicy] = (int)std::lround((w.factors->Mods(home->hood).happiness - 1.0f) * 64.0f);
}
inline int LifeHappyTarget(const LifeWorld& w, uint32_t ci) {
    LifeTerms t;
    LifeHappyTerms(w, ci, t);
    return std::clamp(128 + t.Sum(), 0, 255);
}

// Health target: falls with age past 45 and with air, water and ground pollution; health coverage and wealth lift it.
inline int LifeHealthTarget(const LifeWorld& w, uint32_t ci, int age) {
    const LifeBuilding* home = LifeHomeOf(w, ci);
    const CellFactor& f = w.Factor(home ? home->cell : -1);
    const int wealth = w.hh.wealth[LifeWorld::Ix(w.cit.household[ci])] & 3;
    const int t = 210 - (int)(std::max(0, age - 45) * 1.6f) - f.air / 5 - f.water / 5 - f.ground / 8 + f.coverage[kSvcHealth] / 12 + (wealth - 1) * 6;
    return std::clamp(t, 20, 255);
}

// Yearly death hazard: Gompertz in age plus a floor, raised by poor health, scaled by policy (life.mort*).
inline float LifeDeathHazard(const LifeWorld& w, uint32_t ci, int age) {
    if (age >= w.tune.maxAge) return 1e9f;
    float h = w.tune.mortFloor + w.tune.mortA * std::exp(w.tune.mortB * (float)age);
    if (age == 0) h += 0.004f; // infant
    h *= 1.0f + (float)std::max(0, 150 - (int)w.cit.health[ci]) / 100.0f;
    const LifeBuilding* home = LifeHomeOf(w, ci);
    if (w.factors && home) h *= w.factors->Mods(home->hood).sicknessRate;
    return h;
}

// Monthly drift of health and happiness, and sickness (Phase 4 hospitals heal; until then sickness passes by itself).
inline void LifeWellbeingMonth(LifeWorld& w, uint32_t ci, int age) {
    const float dh = w.tune.healthDrift, dp = w.tune.happyDrift;
    const int ht = LifeHealthTarget(w, ci, age) - ((w.cit.flags[ci] & kCitSick) ? 40 : 0);
    w.cit.health[ci] = (uint8_t)std::clamp((int)w.cit.health[ci] + (int)std::lround((ht - (int)w.cit.health[ci]) * dh), 0, 255);
    const int pt = LifeHappyTarget(w, ci);
    w.cit.happy[ci] = (uint8_t)std::clamp((int)w.cit.happy[ci] + (int)std::lround((pt - (int)w.cit.happy[ci]) * dp), 0, 255);
    const float hl = (float)w.cit.health[ci];
    const uint32_t raw = w.cit.At(ci).v;
    if (!(w.cit.flags[ci] & kCitSick) && hl < w.tune.sickBelow && w.Chance(raw, 3, (w.tune.sickBelow - hl) / w.tune.sickBelow * 0.5f)) {
        w.cit.flags[ci] |= kCitSick;
        w.Event(SimEventType::Sick, w.hh.home[LifeWorld::Ix(w.cit.household[ci])], raw);
    } else if ((w.cit.flags[ci] & kCitSick) && w.Chance(raw, 4, 0.35f)) {
        w.cit.flags[ci] &= (uint8_t)~kCitSick;
        w.Event(SimEventType::Healed, w.hh.home[LifeWorld::Ix(w.cit.household[ci])], raw);
    }
}

} // namespace city
} // namespace tbd
