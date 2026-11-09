// Household money (CS2 sim roadmap Phase 1): wages by job level, pensions, rent, spending, savings and the wealth band.
// Header only, headless (tests\sim_migration_test). Whole currency, int32, once per life month per household.
//
//   income  sum of members' wages (life.wage0..4 by the level of the slot they hold) + a pension per retired member
//   rent    life.rentPerPerson x (2 + residents) / 3 x the building level (1, 1.25, 1.5 ...) x CellFactor::rentBurden / 64
//           (a flat table until Phase 2's sim_land.h writes rentBurden from land value)
//   spend   life.spendPerPerson x residents + life.spendShare of what is left after rent and that
//   wealth  band from savings per resident: < 500, < 3000, < 12000, more (CellSim's low / modest / comfortable / rich)
// Phase 7 takes residential tax from incomeMonth; Phase 2 reads rentMonth for affordability.
#pragma once

#include <algorithm>
#include <cstdint>

#include "sim_life_world.h"

namespace tbd {
namespace city {

inline int LifeWealthBand(int64_t savings, int size) {
    const int64_t per = savings / std::max(1, size);
    return per < 500 ? 0 : per < 3000 ? 1 : per < 12000 ? 2 : 3;
}

inline int32_t LifeRentFor(const LifeWorld& w, const LifeBuilding& home, int size) {
    const int64_t base = (int64_t)w.tune.rentPerPerson * (2 + size) / 3 * (4 + std::max(0, (int)home.level - 1)) / 4;
    return (int32_t)(base * w.Factor(home.cell).rentBurden / 64);
}

// The month's ledger for household hi; returns the new savings. Moves the wealth band through LifeWorld::SetWealth so the
// cell views follow.
inline int32_t LifeHouseholdMonth(LifeWorld& w, uint32_t hi) {
    const LifeBuilding* home = w.B(w.hh.home[hi]);
    const int size = std::max(1, (int)w.hh.size[hi]);
    int64_t income = 0;
    for (uint32_t c = w.hh.firstMember[hi]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)]) {
        const uint32_t ci = LifeWorld::Ix(c);
        if (w.cit.work[ci]) income += w.tune.Wage(w.cit.jobLevel[ci]);
        else if (w.cit.flags[ci] & kCitRetired) income += w.tune.pension;
    }
    const int64_t rent = home ? LifeRentFor(w, *home, size) : 0;
    const int64_t need = (int64_t)w.tune.spendPerPerson * size;
    const int64_t spend = need + (int64_t)((double)std::max<int64_t>(0, income - rent - need) * w.tune.spendShare);
    const int64_t savings = std::clamp<int64_t>((int64_t)w.hh.savings[hi] + income - rent - spend, -1000000, 100000000);
    w.hh.incomeMonth[hi] = (int32_t)std::min<int64_t>(income, INT32_MAX);
    w.hh.rentMonth[hi] = (int32_t)rent, w.hh.spendMonth[hi] = (int32_t)spend, w.hh.savings[hi] = (int32_t)savings;
    w.hh.poorMonths[hi] = (uint8_t)(savings < 0 ? std::min(255, w.hh.poorMonths[hi] + 1) : 0);
    w.hh.carCount[hi] = (uint8_t)std::min<int>(w.hh.adults[hi], LifeWealthBand(savings, size) >= 1 ? w.hh.adults[hi] : 0);
    w.SetWealth(hi, LifeWealthBand(savings, size));
    return (int32_t)savings;
}

} // namespace city
} // namespace tbd
