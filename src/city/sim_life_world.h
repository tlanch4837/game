// LifeWorld: the Phase 1 citizen sim's state and the ONLY functions that change who lives or works where (CS2 sim
// roadmap 3.2 / Phase 1). Header only, headless (tests\sim_citizens_test).
//
//   Records    CitizenStore / HouseholdStore (sim_citizens.h).
//   Occupancy  LifeBuilding per BuildingId::Index(): homes (resident capacity, = LotCapacity homes), used, job slots by
//              education level and filled counts; no per-job record (a citizen's job is (work, jobLevel)).
//   Views      LifeCell per CitySim cell and the commute matrix (home cell, job cell) -> workers, hoodVac per hood and
//              level. Kept INCREMENTALLY: every change to a citizen goes Count(c, -1), change, Count(c, +1), so the views
//              are always a pure sum over the records (Verify() recounts from scratch and compares).
// Every live household has a home: one losing its home is rehoused at once or leaves the city.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim.h"
#include "sim_buildings.h"
#include "sim_calendar.h"
#include "sim_citizens.h"
#include "sim_events.h"
#include "sim_factors.h"
#include "sim_life_tuning.h"

namespace tbd {
namespace city {

struct LifeBuilding {
    uint32_t id = 0;             // BuildingId raw; 0: unused row
    int32_t cell = -1, hood = 0; // CitySim cell index, hood (0 when the host has none)
    int32_t cellX = 0, cellZ = 0;
    uint16_t homes = 0, used = 0, stat = 0; // resident capacity, residents with a record, statistical residents
    uint16_t slots[kEduLevels] = {}, filled[kEduLevels] = {};
    uint32_t firstHousehold = 0; // HouseholdId
    uint8_t level = 1;
    int8_t jobKind = -1;         // sim_cells.h JobKind of the workplace (0 ind, 1 com, 2 office), -1 none
    uint16_t pad = 0;
    uint32_t FreeHomes() const { const uint32_t u = (uint32_t)used + stat; return homes > u ? homes - u : 0; }
    uint32_t Slots() const { uint32_t s = 0; for (int e = 0; e < kEduLevels; ++e) s += slots[e]; return s; }
};

// Per CitySim cell; uint32 so a city total never saturates. The host copies these into CellSim (uint16, saturating).
struct LifeCell {
    uint32_t residents[3] = {}; // kids (0-17), adults (18-64), seniors (65+): records plus statistical (as adults)
    uint32_t working = 0;       // residents here with a job anywhere
    uint32_t employed[3] = {};  // jobs here that are filled, by JobKind
    uint32_t edu[kEduLevels] = {}; // adults by education level
    uint32_t wealth[4] = {};    // residents by household wealth band
    uint32_t students = 0;      // adults in college or university
    uint32_t households = 0;
    uint32_t commuteSum = 0;    // minutes, over `working`
    bool operator==(const LifeCell& o) const {
        for (int i = 0; i < 3; ++i) if (residents[i] != o.residents[i] || employed[i] != o.employed[i]) return false;
        for (int i = 0; i < kEduLevels; ++i) if (edu[i] != o.edu[i]) return false;
        for (int i = 0; i < 4; ++i) if (wealth[i] != o.wealth[i]) return false;
        return working == o.working && students == o.students && households == o.households && commuteSum == o.commuteSum;
    }
};

struct LifeMonthStats {
    int32_t births = 0, deaths = 0, immigrants = 0, emigrants = 0, moved = 0, graduates = 0, hired = 0, quit = 0;
    int32_t fired = 0, retired = 0, partnered = 0, population = 0, households = 0, workers = 0, unemployed = 0;
};

using LifeTravelFn = std::function<float(int, int)>; // minutes between two hoods (sim_hoods.h TravelFn)

struct LifeWorld {
    LifeTuning tune;
    SimCalendar cal;
    uint64_t citySeed = 1;
    CitizenStore cit;
    HouseholdStore hh;
    std::vector<LifeBuilding> bld; // by BuildingId::Index()
    std::vector<LifeCell> cells;   // by CitySim cell index
    std::unordered_map<uint64_t, int32_t> commute; // (homeCell << 32 | jobCell) -> workers
    int hoodCount = 1;
    std::vector<std::array<int32_t, kEduLevels>> hoodVac; // free job slots per hood and level
    std::vector<std::vector<uint32_t>> hoodJobs;          // workplace building indices per hood
    std::vector<std::vector<uint16_t>> hoodNear;          // per hood: hoods to look in, own first, by travel time
    std::vector<std::vector<uint32_t>> seekers;           // per hood: CitizenId raw of job seekers living there
    std::vector<uint32_t> wantsHome;                      // CitizenId raw: adult children who want an own home
    std::vector<uint32_t> jobCursor;                      // per hood: where the last hire was found in hoodJobs
    uint32_t matchStart = 0, homeCursor = 0;              // rotating starts (saved: the sim stays deterministic)
    LifeTravelFn travel;
    SimEventRing* events = nullptr;     // optional
    const CellFactors* factors = nullptr; // optional; read for wellbeing (neutral when null)
    int64_t minute = 0, month = 0;
    int32_t statResidents = 0;          // city total of statistical residents
    LifeMonthStats cur;                 // this month so far
    std::vector<LifeMonthStats> history; // closed months, newest last (at most 120)

    static uint32_t Ix(uint32_t raw) { return raw & Handle::kIndexMask; }
    int AgeOf(uint32_t ci) const { return cal.AgeYears(cit.bornMonth[ci], month); }
    uint64_t Rng(uint32_t raw, uint64_t purpose) const { return SimSeed(citySeed, raw, (uint64_t)month * 64u + purpose); }
    static float U01(uint64_t r) { return (float)(r >> 40) * (1.0f / 16777216.0f); }
    bool Chance(uint32_t raw, uint64_t purpose, float p) const { return p > 0 && U01(Rng(raw, purpose)) < p; }
    const CellFactor& Factor(int cell) const { static const CellFactor kNeutral; return factors ? factors->At(cell) : kNeutral; }
    LifeCell& CellAt(int c) { if ((size_t)c >= cells.size()) cells.resize((size_t)c + 1); return cells[(size_t)c]; }
    LifeBuilding* B(uint32_t raw) { const uint32_t i = Ix(raw); return raw && i < bld.size() && bld[i].id == raw ? &bld[i] : nullptr; }
    void Event(SimEventType t, uint32_t b, uint32_t c, int64_t v = 0) { if (events) events->Push(t, minute, b, c, v); }

    // Adds (s = +1) or takes away (s = -1) citizen ci's share of every view. The only writer of the views.
    void Count(uint32_t ci, int s) {
        const uint32_t h = cit.household[ci];
        if (!h) return;
        const uint32_t hi = Ix(h);
        LifeBuilding* home = B(hh.home[hi]);
        if (!home) return;
        home->used = (uint16_t)(home->used + s);
        LifeBuilding* job = B(cit.work[ci]);
        if (home->cell >= 0) {
            LifeCell& C = CellAt(home->cell);
            const int st = cit.stage[ci];
            C.residents[LifeBandOfStage(st)] += (uint32_t)s;
            if (st == kLifeAdult) {
                C.edu[cit.edu[ci]] += (uint32_t)s;
                if (cit.flags[ci] & kCitStudent) C.students += (uint32_t)s;
            }
            C.wealth[hh.wealth[hi]] += (uint32_t)s;
            if (job) C.working += (uint32_t)s, C.commuteSum += (uint32_t)(s * (int)cit.commuteMin[ci]);
        }
        if (job) {
            const int l = cit.jobLevel[ci];
            job->filled[l] = (uint16_t)(job->filled[l] + s);
            hoodVac[(size_t)job->hood][(size_t)l] -= s;
            if (job->cell >= 0 && job->jobKind >= 0) CellAt(job->cell).employed[job->jobKind] += (uint32_t)s;
            if (home->cell >= 0 && job->cell >= 0) {
                const uint64_t k = (uint64_t)(uint32_t)home->cell << 32 | (uint32_t)job->cell;
                auto it = commute.emplace(k, 0).first;
                if ((it->second += s) == 0) commute.erase(it);
            }
        }
    }
    template <class F>
    void Mutate(uint32_t ci, F&& f) { Count(ci, -1); f(); Count(ci, +1); }

    // Minutes from home to job: straight line at an average 30 km/h door to door plus 4 minutes, or the hood-to-hood
    // route cost (RouteCache through `travel`) when the hoods differ and it is longer. Whole minutes.
    uint16_t CommuteBetween(const LifeBuilding& a, const LifeBuilding& b) const {
        const float dx = (float)(a.cellX - b.cellX), dz = (float)(a.cellZ - b.cellZ);
        float m = 4.0f + std::sqrt(dx * dx + dz * dz) * 128.0f / 500.0f;
        if (travel && a.hood != b.hood) m = std::max(m, travel(a.hood, b.hood) + 4.0f);
        return (uint16_t)std::clamp(std::lround(m), 1L, 600L);
    }

    void SetWork(uint32_t ci, uint32_t b, int level) {
        const LifeBuilding* home = B(hh.home[Ix(cit.household[ci])]);
        const LifeBuilding* job = B(b);
        Mutate(ci, [&] {
            cit.work[ci] = job ? b : 0;
            cit.jobLevel[ci] = (uint8_t)(job ? level : 0);
            cit.commuteMin[ci] = (uint16_t)(job && home ? CommuteBetween(*home, *job) : 0);
        });
    }
    void SetStage(uint32_t ci, int st) {
        const uint32_t hi = Ix(cit.household[ci]);
        const bool wasAdult = cit.stage[ci] >= kLifeAdult;
        Mutate(ci, [&] { cit.stage[ci] = (uint8_t)st; });
        const bool isAdult = st >= kLifeAdult;
        if (cit.household[ci] && wasAdult != isAdult) hh.adults[hi] = (uint8_t)(hh.adults[hi] + (isAdult ? 1 : -1));
    }
    void SetEdu(uint32_t ci, int e) { Mutate(ci, [&] { cit.edu[ci] = (uint8_t)std::clamp(e, 0, kEduLevels - 1); }); }
    void SetFlag(uint32_t ci, uint8_t f, bool on) { Mutate(ci, [&] { cit.flags[ci] = (uint8_t)(on ? cit.flags[ci] | f : cit.flags[ci] & ~f); }); }
    void SetWealth(uint32_t hi, int w) {
        if (hh.wealth[hi] == w) return;
        for (uint32_t c = hh.firstMember[hi]; c; c = cit.nextInHousehold[Ix(c)]) Count(Ix(c), -1);
        hh.wealth[hi] = (uint8_t)w;
        for (uint32_t c = hh.firstMember[hi]; c; c = cit.nextInHousehold[Ix(c)]) Count(Ix(c), +1);
    }

    HouseholdId NewHousehold(int32_t savings, int wealth) {
        const HouseholdId h = hh.Alloc();
        if (h.Valid()) hh.savings[h.Index()] = savings, hh.wealth[h.Index()] = (uint8_t)wealth;
        return h;
    }
    // A new member (born, or arriving with the household). Counted at once when the household has a home.
    CitizenId AddMember(HouseholdId h, int32_t bornMonth, int edu, bool female, uint32_t seed) {
        const CitizenId c = cit.Alloc();
        if (!c.Valid()) return c;
        const uint32_t ci = c.Index(), hi = h.Index();
        cit.household[ci] = h.v, cit.bornMonth[ci] = bornMonth, cit.edu[ci] = (uint8_t)edu, cit.seed[ci] = seed;
        cit.stage[ci] = (uint8_t)LifeStageOfAge(AgeOf(ci), tune.retireAge);
        cit.flags[ci] = (uint8_t)(kCitAlive | (female ? kCitFemale : 0) | (cit.stage[ci] == kLifeSenior ? kCitRetired : 0) |
                                  (cit.stage[ci] >= kLifeChild && cit.stage[ci] <= kLifeTeen ? kCitStudent : 0));
        cit.nextInHousehold[ci] = hh.firstMember[hi], hh.firstMember[hi] = c.v;
        hh.size[hi] = (uint8_t)(hh.size[hi] + 1);
        if (cit.stage[ci] >= kLifeAdult) hh.adults[hi] = (uint8_t)(hh.adults[hi] + 1);
        Count(ci, +1);
        return c;
    }
    // Takes the member out of its household (keeps the record: the caller frees it or adds it elsewhere).
    void Detach(uint32_t ci) {
        Count(ci, -1);
        const uint32_t hi = Ix(cit.household[ci]);
        uint32_t* p = &hh.firstMember[hi];
        while (*p && Ix(*p) != ci) p = &cit.nextInHousehold[Ix(*p)];
        if (*p) *p = cit.nextInHousehold[ci];
        hh.size[hi] = (uint8_t)(hh.size[hi] - 1);
        if (cit.stage[ci] >= kLifeAdult) hh.adults[hi] = (uint8_t)(hh.adults[hi] - 1);
        cit.household[ci] = 0, cit.nextInHousehold[ci] = 0;
    }
    void Attach(uint32_t ci, HouseholdId h) {
        const uint32_t hi = h.Index();
        cit.household[ci] = h.v;
        cit.nextInHousehold[ci] = hh.firstMember[hi], hh.firstMember[hi] = cit.At(ci).v;
        hh.size[hi] = (uint8_t)(hh.size[hi] + 1);
        if (cit.stage[ci] >= kLifeAdult) hh.adults[hi] = (uint8_t)(hh.adults[hi] + 1);
        Count(ci, +1);
    }
    void RemoveCitizen(uint32_t ci) { Detach(ci); cit.Free(cit.At(ci)); }

    void Place(HouseholdId h, uint32_t b) {
        LifeBuilding* L = B(b);
        const uint32_t hi = h.Index();
        if (!L) return;
        hh.home[hi] = b, hh.nextInHome[hi] = L->firstHousehold, L->firstHousehold = h.v, hh.movedInMonth[hi] = (int32_t)month;
        if (L->cell >= 0) CellAt(L->cell).households++;
        for (uint32_t c = hh.firstMember[hi]; c; c = cit.nextInHousehold[Ix(c)]) {
            const uint32_t m = Ix(c);
            if (cit.work[m]) { // a job kept while homeless: still there and still free (it was not counted meanwhile)?
                const LifeBuilding* J = B(cit.work[m]);
                if (J && J->filled[cit.jobLevel[m]] < J->slots[cit.jobLevel[m]]) cit.commuteMin[m] = CommuteBetween(*L, *J);
                else cit.work[m] = 0, cit.jobLevel[m] = 0, cit.commuteMin[m] = 0, cur.fired++;
            }
            Count(m, +1);
        }
    }
    void Unplace(uint32_t hi) {
        LifeBuilding* L = B(hh.home[hi]);
        if (!L) { hh.home[hi] = 0; return; }
        for (uint32_t c = hh.firstMember[hi]; c; c = cit.nextInHousehold[Ix(c)]) Count(Ix(c), -1);
        uint32_t* p = &L->firstHousehold;
        while (*p && Ix(*p) != hi) p = &hh.nextInHome[Ix(*p)];
        if (*p) *p = hh.nextInHome[hi];
        if (L->cell >= 0) CellAt(L->cell).households--;
        hh.home[hi] = 0, hh.nextInHome[hi] = 0;
    }
    // Removes the household and all its members (deaths and emigration call this).
    void FreeHousehold(uint32_t hi) {
        while (hh.firstMember[hi]) RemoveCitizen(Ix(hh.firstMember[hi]));
        Unplace(hi);
        hh.Free(hh.At(hi));
    }
    void MoveHousehold(uint32_t hi, uint32_t b) { const HouseholdId h = hh.At(hi); Unplace(hi); Place(h, b); }

    // Recounts every view from the records and compares (tests; also a debug check in the host). why gets the first
    // difference.
    bool Verify(std::string* why = nullptr) const {
        LifeWorld w;
        w.cit = cit, w.hh = hh, w.bld = bld, w.hoodCount = hoodCount;
        w.hoodVac.assign((size_t)hoodCount, {});
        for (LifeBuilding& L : w.bld) {
            if (!L.id) continue;
            L.used = 0;
            for (int e = 0; e < kEduLevels; ++e) L.filled[e] = 0, w.hoodVac[(size_t)L.hood][(size_t)e] += L.slots[e];
        }
        w.cells.assign(cells.size(), LifeCell());
        for (const LifeBuilding& L : w.bld)
            if (L.id && L.cell >= 0) w.CellAt(L.cell).residents[1] += L.stat;
        for (uint32_t hi = 1; hi < hh.Capacity(); ++hi) {
            if (!hh.At(hi).Valid()) continue;
            const LifeBuilding* L = w.B(hh.home[hi]);
            if (!L) { if (why) *why = "household without a home " + std::to_string(hi); return false; }
            if (L->cell >= 0) w.CellAt(L->cell).households++;
            uint32_t n = 0, a = 0;
            for (uint32_t c = hh.firstMember[hi]; c; c = cit.nextInHousehold[Ix(c)], ++n) a += cit.stage[Ix(c)] >= kLifeAdult;
            if (n != hh.size[hi] || a != hh.adults[hi]) { if (why) *why = "household size " + std::to_string(hi); return false; }
        }
        for (uint32_t ci = 1; ci < cit.Capacity(); ++ci)
            if (cit.At(ci).Valid()) {
                if (cit.work[ci] && !w.B(cit.work[ci])) { if (why) *why = "job in a dead building"; return false; }
                w.Count(ci, +1);
            }
        for (size_t i = 0; i < bld.size(); ++i) {
            const LifeBuilding& A = bld[i];
            const LifeBuilding& E = w.bld[i];
            bool same = A.used == E.used;
            for (int e = 0; e < kEduLevels; ++e) same = same && A.filled[e] == E.filled[e] && A.filled[e] <= A.slots[e];
            if (!same) { if (why) *why = "building " + std::to_string(i); return false; }
        }
        for (size_t c = 0; c < cells.size(); ++c)
            if (!(cells[c] == w.cells[c])) { if (why) *why = "cell " + std::to_string(c); return false; }
        if (commute != w.commute) { if (why) *why = "commute matrix"; return false; }
        if (hoodVac != w.hoodVac) { if (why) *why = "hood vacancies"; return false; }
        return true;
    }

    // City totals (from the views, O(cells)).
    uint32_t Population() const { return cit.Live() + (uint32_t)statResidents; }
    uint32_t Working() const { uint32_t n = 0; for (const LifeCell& c : cells) n += c.working; return n; }
};

} // namespace city
} // namespace tbd
