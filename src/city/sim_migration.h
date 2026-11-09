// Migration and moving (CS2 sim roadmap Phase 1). Header only, headless (tests\sim_migration_test).
//
//   Immigration  once a month the city works out its attractiveness (mean happiness, jobs free against seekers) and a
//                quota: free home capacity x life.moveInRate x attractiveness (all of it with life.instantFill). The
//                quota arrives spread over the month's 96 slices as households (1-5 people, about 2.5) that pick the best
//                of a few sampled free homes. They arrive at an outside connection: SimEventType::Immigrated carries the
//                home, the host picks the connection and animates the arrival. Past life.recordCap homes fill with
//                statistical residents instead.
//   Emigration   a household leaves after life.leaveUnhappyMonths unhappy months, or life.leavePoorMonths with savings
//                below zero (a long stretch of unemployment), each with a monthly chance (a lag, never all at once).
//   Moving       adult children leave home, singles pair up, crowded or long-commute households move to a better free
//                home: a monthly pass of at most life.movesPerMonth moves.
#pragma once

#include <algorithm>
#include <vector>

#include "sim_household_econ.h"
#include "sim_life_boot.h"
#include "sim_lifecycle.h"

namespace tbd {
namespace city {

struct LifeMigrationState {
    float attract = 1.0f;     // this month's attractiveness 0..2.5
    int32_t quota = 0;        // people to admit this month
    int32_t admitted = 0;     // so far
    int32_t avgHappy = 128;   // of last month
    int32_t vacantJobs = 0, seekers = 0, freeHomes = 0;
    std::vector<uint32_t> freeList; // building raws with free homes (rebuilt each month)
};

// How much a free home appeals regardless of who moves in: the place terms of sim_wellbeing.h (no personal terms).
inline int LifePlaceScore(const LifeWorld& w, const LifeBuilding& L) {
    const CellFactor& f = w.Factor(L.cell);
    int cov = 0;
    for (int s = 0; s < 6; ++s) cov += f.coverage[s];
    return cov / 40 + f.parkAccess / 24 + (f.telecom - 128) / 24 - (f.air / 8 + f.ground / 12 + f.water / 8) - f.noise / 10 - f.crime / 8 -
           (f.rentBurden - 64) / 5 - (f.taxBurden - 64) / 5 - (255 - f.goodsAvailable) / 12;
}

// The month's numbers and quota. O(citizens + buildings), once a month.
inline void LifeMigrationMonth(LifeWorld& w, LifeMigrationState& m) {
    int64_t happy = 0;
    uint32_t n = 0, seekers = 0;
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) {
        if (!(w.cit.flags[ci] & kCitAlive) || !w.cit.At(ci).Valid()) continue;
        happy += w.cit.happy[ci], ++n;
        seekers += LifeIsWorkingAge(w, ci) && !w.cit.work[ci];
    }
    m.avgHappy = n ? (int32_t)(happy / n) : 128;
    m.seekers = (int32_t)seekers;
    m.vacantJobs = 0;
    for (const auto& v : w.hoodVac) for (int32_t x : v) m.vacantJobs += std::max(0, x);
    m.freeList.clear();
    m.freeHomes = 0;
    for (const LifeBuilding& L : w.bld)
        if (L.id && L.FreeHomes()) m.freeList.push_back(L.id), m.freeHomes += (int32_t)L.FreeHomes();
    uint32_t adults = 1;
    for (const LifeCell& c : w.cells) adults += c.residents[1];
    const float jobs = std::clamp((float)(m.vacantJobs - m.seekers) / (float)adults * w.tune.attractJobs, -1.0f, 1.0f);
    m.attract = std::clamp(1.0f + (float)(m.avgHappy - 128) / w.tune.attractHappy + jobs, 0.0f, 2.5f);
    m.quota = w.tune.instantFill ? m.freeHomes : (int32_t)std::lround((float)m.freeHomes * w.tune.moveInRate * m.attract);
    m.admitted = 0;
}

// The best of a few sampled free homes with room for n (not `exclude`); 0 if the sampled ones are all full.
inline uint32_t LifePickHome(LifeWorld& w, LifeMigrationState& m, uint32_t n, uint64_t r, uint32_t exclude = 0, int minScore = -1000) {
    uint32_t best = 0;
    int bestScore = minScore;
    for (int k = 0; k < 6 && !m.freeList.empty(); ++k) {
        SplitMix64 s(r + (uint64_t)k);
        const size_t i = (size_t)(s.Next() % m.freeList.size());
        const LifeBuilding* L = w.B(m.freeList[i]);
        if (!L || !L->FreeHomes()) { m.freeList[i] = m.freeList.back(), m.freeList.pop_back(); continue; } // full or gone
        if (L->FreeHomes() < n || L->id == exclude) continue;
        const int sc = LifePlaceScore(w, *L) * 4 + (int)(s.Next() % 4);
        if (sc > bestScore) bestScore = sc, best = L->id;
    }
    return best;
}

// One immigrant household into building b; returns the people who came.
inline int LifeMakeImmigrants(LifeWorld& w, uint32_t b, uint64_t r) {
    SplitMix64 s(r);
    static const int kSize[5] = {28, 34, 16, 14, 8};
    int size = 1;
    for (int u = (int)(s.Next() % 100), k = 0; k < 5; u -= kSize[k++]) if (u < kSize[k]) { size = k + 1; break; }
    LifeBuilding* L = w.B(b);
    if (!L) return 0;
    size = std::min<int>(size, (int)L->FreeHomes());
    if (size <= 0) return 0;
    if (w.cit.Live() + (uint32_t)size > (uint32_t)w.tune.recordCap) { LifeAddStat(w, *L, size); return size; }
    static const int kEdu[5] = {5, 25, 40, 20, 10};
    const auto drawEdu = [&] { int u = (int)(s.Next() % 100), e = 0; while (e < 4 && u >= kEdu[e]) u -= kEdu[e++]; return e; };
    const int wealth = (int)(s.Next() % 100) < 25 ? 0 : (int)(s.Next() % 100) < 47 ? 1 : (int)(s.Next() % 100) < 75 ? 2 : 3;
    const HouseholdId h = w.NewHousehold(LifeSavingsFor(wealth, size), wealth);
    if (!h.Valid()) return 0;
    const bool seniors = size <= 2 && s.Next() % 100 < 15;
    const int headAge = seniors ? 65 + (int)(s.Next() % 20) : 20 + (int)(LifeWorld::U01(s.Next()) * LifeWorld::U01(s.Next()) * 40.0f);
    const auto born = [&](int age) { return (int32_t)(w.month - (int64_t)age * w.cal.lifeMonthsPerYear - (int64_t)(s.Next() % 12)); };
    const bool headFemale = (s.Next() & 1) != 0;
    w.AddMember(h, born(headAge), seniors ? 1 + (int)(s.Next() % 2) : drawEdu(), headFemale, (uint32_t)s.Next());
    int made = 1;
    if (size >= 2 && (seniors || s.Next() % 100 < 80)) { // a partner
        const int age = std::max(18, headAge + (int)(s.Next() % 9) - 4);
        w.AddMember(h, born(age), seniors ? 1 : drawEdu(), !headFemale, (uint32_t)s.Next()), ++made;
    }
    for (; made < size; ++made) {
        const int age = (int)(s.Next() % (uint64_t)std::clamp(headAge - 18, 1, 18));
        w.AddMember(h, born(age), LifeKidEdu(age), (s.Next() & 1) != 0, (uint32_t)s.Next());
    }
    w.Place(h, b);
    w.Event(SimEventType::Immigrated, b, w.hh.firstMember[h.Index()], size);
    return size;
}

// Admits up to `people` of the month's quota now (a slice's share).
inline void LifeImmigrate(LifeWorld& w, LifeMigrationState& m, int people) {
    for (int guard = 0; people > 0 && guard < 4 * people + 8 && !m.freeList.empty(); ++guard) {
        const uint64_t r = w.Rng((uint32_t)(m.admitted + 1) * 2654435761u, 11 + (uint64_t)guard);
        const uint32_t b = LifePickHome(w, m, 1, r);
        if (!b) continue;
        const int n = LifeMakeImmigrants(w, b, r ^ 0x5bd1e995u);
        people -= n, m.admitted += n, w.cur.immigrants += n;
    }
}

inline void LifeEmigrate(LifeWorld& w, uint32_t hi) {
    w.cur.emigrants += w.hh.size[hi];
    w.Event(SimEventType::Emigrated, w.hh.home[hi], w.hh.firstMember[hi], w.hh.size[hi]);
    w.FreeHousehold(hi);
}

// The monthly visit of household hi (the daily slice): money, unhappy and poor months, leaving, looking for a home.
inline void LifeHouseholdVisit(LifeWorld& w, uint32_t hi) {
    if (!w.hh.size[hi]) { w.Unplace(hi), w.hh.Free(w.hh.At(hi)); return; }
    if (!w.hh.adults[hi]) { LifeOrphans(w, hi); return; }
    LifeHouseholdMonth(w, hi);
    int happy = 0, commuteLong = 0;
    for (uint32_t c = w.hh.firstMember[hi]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)]) {
        const uint32_t ci = LifeWorld::Ix(c);
        happy += w.cit.happy[ci];
        commuteLong += w.cit.work[ci] && w.cit.commuteMin[ci] > w.tune.quitCommute;
    }
    happy /= std::max(1, (int)w.hh.size[hi]);
    uint8_t& um = w.hh.unhappyMonths[hi];
    um = (uint8_t)(happy < w.tune.unhappyBelow ? std::min(255, um + 1) : um > 0 ? um - 1 : 0);
    const uint32_t raw = w.hh.At(hi).v;
    if ((um >= w.tune.leaveUnhappyMonths && w.Chance(raw, 20, w.tune.leaveUnhappyRate)) ||
        (w.hh.poorMonths[hi] >= w.tune.leavePoorMonths && w.Chance(raw, 21, w.tune.leavePoorRate))) {
        LifeEmigrate(w, hi);
        return;
    }
    const LifeBuilding* L = w.B(w.hh.home[hi]);
    const bool crowded = L && (uint32_t)L->used + L->stat > L->homes;
    w.hh.flags[hi] = (uint8_t)((w.hh.flags[hi] & ~(kHhCrowded | kHhLooking)) | (crowded ? kHhCrowded | kHhLooking : 0) |
                               (commuteLong ? kHhLooking : 0));
}

// The monthly moving pass (month start, after LifeMigrationMonth built the free list): adult children leave home,
// singles pair up, looking households move. At most life.movesPerMonth moves.
inline void LifeMovePass(LifeWorld& w, LifeMigrationState& m) {
    int moves = w.tune.movesPerMonth;
    for (uint32_t raw : w.wantsHome) { // adult children
        if (moves <= 0) break;
        const CitizenId c(raw);
        if (!w.cit.Alive(c) || w.cit.stage[c.Index()] != kLifeAdult) continue;
        const uint32_t ci = c.Index(), from = LifeWorld::Ix(w.cit.household[ci]);
        if (w.hh.adults[from] < 2) continue;
        const uint32_t b = LifePickHome(w, m, 1, w.Rng(raw, 30));
        if (!b) continue;
        const int32_t share = std::max(0, w.hh.savings[from] / std::max(1, (int)w.hh.size[from]));
        const HouseholdId h = w.NewHousehold(share, LifeWealthBand(share, 1));
        if (!h.Valid()) break;
        w.hh.savings[from] -= share;
        w.Place(h, b);
        w.Detach(ci), w.Attach(ci, h);
        w.cur.moved++, --moves;
        w.Event(SimEventType::MoveIn, b, raw);
    }
    w.wantsHome.clear();
    // singles 20..45 pair up, in record order within each hood (deterministic)
    std::vector<std::vector<uint32_t>> singles((size_t)std::max(1, w.hoodCount));
    for (uint32_t hi = 1; hi < w.hh.Capacity() && moves > 0; ++hi) {
        if (!w.hh.At(hi).Valid() || w.hh.size[hi] != 1 || w.hh.adults[hi] != 1) continue;
        const uint32_t ci = LifeWorld::Ix(w.hh.firstMember[hi]);
        const int age = w.AgeOf(ci);
        const LifeBuilding* L = w.B(w.hh.home[hi]);
        if (L && age >= 20 && age < 45 && w.Chance(w.hh.At(hi).v, 31, w.tune.partnerRate * 2.0f)) singles[(size_t)L->hood].push_back(hi);
    }
    for (auto& list : singles)
        for (size_t k = 0; k + 1 < list.size() && moves > 0; k += 2) {
            const uint32_t a = list[k], b = list[k + 1];
            const uint32_t ca = LifeWorld::Ix(w.hh.firstMember[b]);
            w.Detach(ca), w.Attach(ca, w.hh.At(a));
            w.hh.savings[a] += w.hh.savings[b];
            w.Unplace(b), w.hh.Free(w.hh.At(b));
            w.cur.partnered++, --moves;
            w.Event(SimEventType::Partnered, w.hh.home[a], w.cit.At(ca).v);
        }
    for (uint32_t hi = 1; hi < w.hh.Capacity() && moves > 0; ++hi) { // looking households
        if (!w.hh.At(hi).Valid() || !(w.hh.flags[hi] & kHhLooking) || !w.Chance(w.hh.At(hi).v, 32, w.tune.moveRate)) continue;
        const LifeBuilding* L = w.B(w.hh.home[hi]);
        if (!L) continue;
        const bool crowded = (w.hh.flags[hi] & kHhCrowded) != 0;
        const uint32_t b = LifePickHome(w, m, w.hh.size[hi], w.Rng(w.hh.At(hi).v, 33), L->id, crowded ? -1000 : LifePlaceScore(w, *L) * 4 + 3);
        if (!b) continue;
        w.MoveHousehold(hi, b);
        w.hh.flags[hi] &= (uint8_t)~(kHhLooking | kHhCrowded);
        w.cur.moved += w.hh.size[hi], --moves;
        w.Event(SimEventType::MoveIn, b, w.hh.firstMember[hi]);
    }
}

} // namespace city
} // namespace tbd
