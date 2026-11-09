// Phase 1: occupancy from the BuildingRegistry, and the bootstrap of records for old saves (CS2 sim roadmap 3.4).
// Header only, headless (tests\sim_citizens_test).
//
//   LifeSyncBuildings  mirrors the registry into LifeWorld::bld: new buildings get empty homes and job slots (nobody moves
//                      in here; migration fills them), a building that shrank fires workers and evicts households to fit,
//                      a building that went fires everyone and evicts everyone. Evicted households take a free home
//                      elsewhere at once, or leave the city.
//   LifeBootstrap      fills every empty residential building with the old fixed split (sim_cells.h CitySim::Add: 5 % of
//                      homes free, ages 20/65/15, adult education 30/50/20, wealth 25/35/30/10), so an old city loads
//                      populated and its counters equal the old ones (age and education exactly; wealth exactly where every
//                      wealth share gets a household head, i.e. lots of 5 or more residents).
#pragma once

#include <functional>
#include <vector>

#include "sim_life_world.h"

namespace tbd {
namespace city {

// The same largest-remainder split as CitySim::Split.
template <int N>
inline void LifeSplit(uint32_t total, const int (&pct)[N], uint32_t (&out)[N]) {
    uint32_t used = 0;
    int rem[N];
    for (int i = 0; i < N; ++i) {
        out[i] = total * (uint32_t)pct[i] / 100u;
        rem[i] = (int)(total * (uint32_t)pct[i] % 100u);
        used += out[i];
    }
    while (used < total) {
        int best = 0;
        for (int i = 1; i < N; ++i) if (rem[i] > rem[best]) best = i;
        ++out[best], rem[best] = -1, ++used;
    }
}

// Statistical residents (past the record cap): counted as adults, never ticked.
inline void LifeAddStat(LifeWorld& w, LifeBuilding& L, int n) {
    L.stat = (uint16_t)((int)L.stat + n);
    w.statResidents += n;
    if (L.cell >= 0) w.CellAt(L.cell).residents[1] += (uint32_t)n;
}

// Every household and worker counted in the views comes out (s = -1) or goes back (s = +1). For a building whose cell,
// hood or job kind changes: uncount all, change the rows, count all.
inline void LifeCountAll(LifeWorld& w, int s) {
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) if (w.cit.At(ci).Valid()) w.Count(ci, s);
    for (uint32_t hi = 1; hi < w.hh.Capacity(); ++hi) {
        if (!w.hh.At(hi).Valid()) continue;
        const LifeBuilding* L = w.B(w.hh.home[hi]);
        if (L && L->cell >= 0) w.CellAt(L->cell).households += (uint32_t)s;
    }
    for (LifeBuilding& L : w.bld) if (L.id && L.stat && L.cell >= 0) w.CellAt(L.cell).residents[1] += (uint32_t)(s * (int)L.stat);
}

// A free home for a household of n (not `exclude`): the first building from a rotating cursor with room. 0 if none.
inline uint32_t LifeFindHome(LifeWorld& w, uint32_t n, uint32_t exclude, uint32_t& cursor) {
    const uint32_t count = (uint32_t)w.bld.size();
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t i = (cursor + k) % count;
        const LifeBuilding& L = w.bld[i];
        if (L.id && L.id != exclude && L.FreeHomes() >= n) { cursor = i; return L.id; }
    }
    return 0;
}

// Households without a home (already Unplaced): rehoused, else they leave the city.
inline void LifeRehouse(LifeWorld& w, const std::vector<uint32_t>& homeless) {
    uint32_t cursor = (uint32_t)(w.Rng(0, 7) % std::max<size_t>(1, w.bld.size()));
    for (uint32_t hi : homeless) {
        if (!w.hh.At(hi).Valid()) continue;
        const uint32_t b = LifeFindHome(w, w.hh.size[hi], 0, cursor);
        if (b) { w.Place(w.hh.At(hi), b); w.cur.moved += w.hh.size[hi]; w.Event(SimEventType::MoveIn, b, w.hh.firstMember[hi]); continue; }
        w.cur.emigrants += w.hh.size[hi];
        w.Event(SimEventType::Emigrated, 0, w.hh.firstMember[hi], w.hh.size[hi]);
        w.FreeHousehold(hi);
    }
}

// jobKindOf(row): sim_cells.h JobKind of a workplace (-1 none). Null: every workplace counts as commercial.
using LifeJobKindFn = std::function<int(const BuildingRow&)>;

inline void LifeSyncBuildings(LifeWorld& w, const BuildingRegistry& reg, const LifeJobKindFn& jobKindOf = nullptr) {
    const uint32_t cap = reg.Capacity();
    if (w.bld.size() < cap) w.bld.resize(cap);
    int hoods = 1, maxCell = -1;
    std::vector<uint32_t> gone, homeless, shrunk; // shrunk: homes went down (births may crowd a home; that is no eviction)
    bool relocate = false;
    auto kindOf = [&](const BuildingRow& r) {
        uint32_t jobs = 0;
        for (int e = 0; e < kEduLevels; ++e) jobs += r.jobSlots[e];
        return jobs == 0 ? -1 : jobKindOf ? jobKindOf(r) : 1;
    };
    for (uint32_t i = 1; i < cap; ++i) { // pass 1: what went, what moved
        const BuildingId id = reg.At(i);
        const BuildingRow* r = id.Valid() ? reg.Get(id) : nullptr;
        const uint32_t raw = r && r->alive ? id.v : 0;
        LifeBuilding& L = w.bld[i];
        if (r && raw) hoods = std::max(hoods, r->hood + 1), maxCell = std::max(maxCell, r->cell);
        if (L.id && L.id != raw) gone.push_back(i);
        else if (L.id && r->alive == 1 && (L.cell != r->cell || L.hood != std::max(0, r->hood) || L.cellX != r->cellX ||
                                           L.cellZ != r->cellZ || L.jobKind != kindOf(*r)))
            relocate = true;
    }
    for (const LifeBuilding& L : w.bld) if (L.id) hoods = std::max(hoods, L.hood + 1);
    w.hoodCount = hoods;
    if (w.hoodVac.size() < (size_t)hoods) w.hoodVac.resize((size_t)hoods, {});
    if (maxCell >= 0) w.CellAt(maxCell);
    if (relocate) LifeCountAll(w, -1);
    for (uint32_t i = 1; i < cap; ++i) { // pass 2: refresh the rows (homes and slots shrink below)
        const BuildingId id = reg.At(i);
        const BuildingRow* r = id.Valid() ? reg.Get(id) : nullptr;
        if (!r || !r->alive) continue;
        LifeBuilding& L = w.bld[i];
        if (L.id != id.v) L = LifeBuilding(), L.id = id.v;
        else if (r->alive == 2) continue; // reserved from a load: keep the saved row until the building is back
        L.cell = r->cell, L.hood = std::max(0, r->hood), L.cellX = r->cellX, L.cellZ = r->cellZ;
        L.level = r->level, L.jobKind = (int8_t)kindOf(*r);
        if (r->homes < L.homes) shrunk.push_back(i);
        L.homes = r->homes;
        for (int e = 0; e < kEduLevels; ++e) L.slots[e] = r->jobSlots[e];
    }
    if (relocate) LifeCountAll(w, +1);
    for (uint32_t i : gone) { // homes of a building that went: everyone out
        LifeBuilding& L = w.bld[i];
        while (L.firstHousehold) { const uint32_t hi = LifeWorld::Ix(L.firstHousehold); w.Unplace(hi), homeless.push_back(hi); }
        if (L.stat) LifeAddStat(w, L, -(int)L.stat);
        L.homes = 0;
        for (int e = 0; e < kEduLevels; ++e) L.slots[e] = 0;
    }
    bool overFull = false;
    for (const LifeBuilding& L : w.bld)
        for (int e = 0; e < kEduLevels; ++e) overFull = overFull || (L.id && L.filled[e] > L.slots[e]);
    for (uint32_t i : shrunk) { // fewer homes: statistical residents first, then the newest households
        LifeBuilding& L = w.bld[i];
        if ((uint32_t)L.used + L.stat > L.homes && L.stat) LifeAddStat(w, L, -(int)std::min<uint32_t>(L.stat, (uint32_t)L.used + L.stat - L.homes));
        while (L.used > L.homes && L.firstHousehold) { const uint32_t hi = LifeWorld::Ix(L.firstHousehold); w.Unplace(hi), homeless.push_back(hi); }
    }
    if (overFull) // fire workers over the slots, newest record first
        for (uint32_t ci = w.cit.Capacity(); ci-- > 1;) {
            if (!w.cit.At(ci).Valid() || !w.cit.work[ci]) continue;
            const LifeBuilding* J = w.B(w.cit.work[ci]);
            if (J && J->filled[w.cit.jobLevel[ci]] > J->slots[w.cit.jobLevel[ci]]) {
                const bool counted = w.cit.household[ci] && w.hh.home[LifeWorld::Ix(w.cit.household[ci])];
                if (!counted) continue; // homeless: its job is not counted; LifeRehouse places or removes it
                w.SetWork(ci, 0, 0), w.cur.fired++;
                w.Event(SimEventType::Fired, J->id, w.cit.At(ci).v);
            }
        }
    for (uint32_t i : gone) w.bld[i] = LifeBuilding();
    LifeRehouse(w, homeless);
    w.hoodJobs.assign((size_t)hoods, {});
    w.hoodVac.assign((size_t)hoods, {});
    for (uint32_t i = 1; i < w.bld.size(); ++i) {
        const LifeBuilding& L = w.bld[i];
        if (!L.id) continue;
        if (L.Slots()) w.hoodJobs[(size_t)L.hood].push_back(i);
        for (int e = 0; e < kEduLevels; ++e) w.hoodVac[(size_t)L.hood][(size_t)e] += (int32_t)L.slots[e] - (int32_t)L.filled[e];
    }
    if (w.seekers.size() < (size_t)hoods) w.seekers.resize((size_t)hoods);
}

// Ages for new records (whole years), census-like: kids flat 0..17, adults thinning towards 64, seniors decaying.
inline int LifeDrawAge(uint64_t r, int band) {
    const float u = LifeWorld::U01(r);
    if (band == 0) return (int)(u * 18.0f) % 18;
    if (band == 1) return 18 + (int)(47.0f * (1.0f - std::sqrt(1.0f - u * 0.55f)) / (1.0f - std::sqrt(0.45f)));
    return 65 + std::min(34, (int)(-std::log(1.0f - u * 0.999f) * 9.0f));
}
inline int LifeKidEdu(int age) { return age >= 12 ? 1 : 0; }
inline int32_t LifeSavingsFor(int wealth, int size) { static const int32_t k[4] = {200, 1500, 6000, 20000}; return k[wealth & 3] * size; }

// Fills building b with `people` residents in the old fixed split, as households (about 2.5 people, at least one adult
// or senior each, couples where there are heads to spare). Deterministic from (city seed, building id).
// agePct overrides the old 20/65/15 age split (tests use the census structure 22/61/17).
inline void LifeFillBuilding(LifeWorld& w, uint32_t b, uint32_t people, const int (*agePct)[3] = nullptr) {
    static const int kAge[3] = {20, 65, 15}, kEdu[3] = {30, 50, 20}, kWealth[4] = {25, 35, 30, 10};
    uint32_t age[3], edu[3], wealth[4];
    LifeSplit(people, agePct ? *agePct : kAge, age);
    LifeSplit(age[1], kEdu, edu);
    LifeSplit(people, kWealth, wealth);
    struct P { uint8_t band, edu; };
    std::vector<P> heads, kids;
    for (int e = 0; e < 3; ++e) for (uint32_t k = 0; k < edu[e]; ++k) heads.push_back({1, (uint8_t)(e + 1)});
    for (uint32_t k = 0; k < age[2]; ++k) heads.push_back({2, (uint8_t)1});
    for (uint32_t k = 0; k < age[0]; ++k) kids.push_back({0, (uint8_t)0});
    // interleave so every wealth share draws adults, seniors and kids alike
    SplitMix64 rng(SimSeed(w.citySeed, b, 0xB007));
    for (size_t i = heads.size(); i > 1; --i) std::swap(heads[i - 1], heads[(size_t)(rng.Next() % i)]);
    size_t nh = 0, nk = 0;
    uint32_t serial = 0;
    for (int wb = 0; wb < 4; ++wb) {
        if (!wealth[wb]) continue;
        std::vector<P> chunk;
        if (nh < heads.size()) chunk.push_back(heads[nh++]);
        while (chunk.size() < wealth[wb] && (nh < heads.size() || nk < kids.size())) {
            const uint64_t takenH = nh, takenK = nk; // Bresenham over what is left
            const bool kid = nk < kids.size() && (nh >= heads.size() || takenK * heads.size() < takenH * kids.size());
            chunk.push_back(kid ? kids[nk++] : heads[nh++]);
        }
        std::vector<HouseholdId> made;
        uint32_t nHeads = 0;
        for (const P& p : chunk) nHeads += p.band > 0;
        const uint32_t nHouse = std::max<uint32_t>(1, std::min<uint32_t>(nHeads ? nHeads : 1, (uint32_t)std::lround(chunk.size() / 2.5)));
        for (uint32_t k = 0; k < nHouse; ++k) made.push_back(w.NewHousehold(0, wb));
        uint32_t hk = 0, kk = 0;
        for (const P& p : chunk) {
            const HouseholdId h = made[(size_t)(p.band > 0 ? hk++ % nHouse : kk++ % nHouse)];
            const uint64_t r = SimSeed(w.citySeed, b, 0xA9E0u + serial++);
            const int a = LifeDrawAge(r, p.band);
            const int e = p.band == 0 ? LifeKidEdu(a) : p.edu;
            w.AddMember(h, (int32_t)(w.month - (int64_t)a * w.cal.lifeMonthsPerYear - (int64_t)((r >> 8) % 12)), e, ((r >> 20) & 1) != 0, (uint32_t)(r >> 32));
        }
        for (HouseholdId h : made) {
            const uint32_t hi = h.Index();
            w.hh.savings[hi] = LifeSavingsFor(wb, w.hh.size[hi]);
            w.Place(h, b);
        }
    }
}

// Old saves (no CITZ chunk): every residential building with nobody in it gets the old split. The record cap still holds:
// past it the residents are statistical.
inline void LifeBootstrap(LifeWorld& w) {
    for (uint32_t i = 1; i < w.bld.size(); ++i) {
        LifeBuilding& L = w.bld[i];
        if (!L.id || !L.homes || L.used || L.stat) continue;
        const uint32_t occ = L.homes >= 2 ? L.homes - std::max<uint32_t>(1u, L.homes / 20u) : L.homes;
        if (w.cit.Live() + occ > (uint32_t)w.tune.recordCap) LifeAddStat(w, L, (int)occ);
        else LifeFillBuilding(w, L.id, occ);
    }
}

} // namespace city
} // namespace tbd
