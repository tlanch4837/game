// Life events of one citizen, once per life month (CS2 sim roadmap Phase 1): ageing through the life stages, school and
// graduation, health and happiness, births, deaths, retirement, job search. Header only, headless
// (tests\sim_lifecycle_test).
//
//   Stages      0-5 baby, 6-12 child, 13-17 teen (both at school), 18-64 adult, 65+ senior (retired).
//   Education   elementary at 12, high school at 18, college 18-21, university 22-24. Whether a citizen passes is a
//               fixed draw per citizen against a chance scaled by the cell's school coverage (x 0.5 .. 1.5), or certain
//               with a school seat from Phase 4 (CitizenStore::school). Graduations push SimEventType::Graduated.
//   Births      a woman of 18..44 has life.fertility children over those 27 years on average, more often with a
//               partner in the household.
//   Deaths      Gompertz hazard from sim_wellbeing.h. A household left with no adult joins a neighbour in the building or
//               leaves the city.
#pragma once

#include <cmath>

#include "sim_jobs.h"

namespace tbd {
namespace city {

// A fixed draw per citizen and purpose (not per month): the same citizen passes or fails the same way every visit.
inline bool LifeFixedChance(const LifeWorld& w, uint32_t raw, uint64_t purpose, float p) {
    return LifeWorld::U01(SimSeed(w.citySeed, raw, 0xED00u + purpose)) < p;
}
inline float LifeSchoolScale(const LifeWorld& w, uint32_t ci, int svc) {
    const LifeBuilding* L = LifeHomeOf(w, ci);
    const CellFactor& f = w.Factor(L ? L->cell : -1);
    const int cov = std::max((int)f.coverage[svc], svc == kSvcSchool ? 0 : (int)f.coverage[kSvcSchool] / 2);
    return std::min(1.0f / 0.97f, 0.75f + 0.5f * (float)cov / 255.0f);
}

inline void LifeGraduate(LifeWorld& w, uint32_t ci, int edu) {
    w.SetEdu(ci, edu);
    w.cur.graduates++;
    w.Event(SimEventType::Graduated, w.hh.home[LifeWorld::Ix(w.cit.household[ci])], w.cit.At(ci).v, edu);
}

inline void LifeSchoolMonth(LifeWorld& w, uint32_t ci, int age) {
    const uint32_t raw = w.cit.At(ci).v;
    const bool seat = w.cit.school[ci] != 0;
    const uint8_t e = w.cit.edu[ci];
    const bool student = (w.cit.flags[ci] & kCitStudent) != 0;
    if (age >= 12 && e == 0 && student && (seat || LifeFixedChance(w, raw, 1, w.tune.eduElementary * LifeSchoolScale(w, ci, kSvcSchool))))
        LifeGraduate(w, ci, 1);
    if (age < 18) return;
    if (e == 1 && age < 19 && (seat || LifeFixedChance(w, raw, 2, w.tune.eduHigh * LifeSchoolScale(w, ci, kSvcHighSchool))))
        LifeGraduate(w, ci, 2);
    if (w.cit.stage[ci] != kLifeAdult) return;
    const uint8_t e2 = w.cit.edu[ci];
    if (!student && !w.cit.work[ci] && e2 == 2 && age < 19 && LifeFixedChance(w, raw, 3, w.tune.eduCollege * LifeSchoolScale(w, ci, kSvcUniversity)))
        w.SetFlag(ci, kCitStudent, true); // college
    else if (student && e2 == 2 && age >= 22) {
        LifeGraduate(w, ci, 3);
        if (!LifeFixedChance(w, raw, 4, w.tune.eduUniversity * LifeSchoolScale(w, ci, kSvcUniversity))) w.SetFlag(ci, kCitStudent, false);
    } else if (student && e2 >= 3 && age >= 25) {
        LifeGraduate(w, ci, 4);
        w.SetFlag(ci, kCitStudent, false);
    } else if (student && e2 < 2 && age >= 18) w.SetFlag(ci, kCitStudent, false); // left school
}

inline void LifeStageChange(LifeWorld& w, uint32_t ci, int to) {
    const int from = w.cit.stage[ci];
    w.SetStage(ci, to);
    if (to >= kLifeChild && to <= kLifeTeen && from < kLifeChild) w.SetFlag(ci, kCitStudent, true);
    if (to == kLifeAdult && from < kLifeAdult && w.cit.edu[ci] < 2) w.SetFlag(ci, kCitStudent, false);
    if (to == kLifeSenior) {
        if (w.cit.work[ci]) w.SetWork(ci, 0, 0);
        w.SetFlag(ci, kCitStudent, false);
        w.SetFlag(ci, kCitRetired, true);
        w.cur.retired++;
        w.Event(SimEventType::Retired, w.hh.home[LifeWorld::Ix(w.cit.household[ci])], w.cit.At(ci).v);
    }
}

// Household hi lost its last adult: the kids join another household of the building, or leave the city.
inline void LifeOrphans(LifeWorld& w, uint32_t hi) {
    const LifeBuilding* L = w.B(w.hh.home[hi]);
    uint32_t to = 0;
    for (uint32_t o = L ? L->firstHousehold : 0; o && !to; o = w.hh.nextInHome[LifeWorld::Ix(o)])
        if (LifeWorld::Ix(o) != hi && w.hh.adults[LifeWorld::Ix(o)] > 0) to = o;
    if (!to) {
        w.cur.emigrants += w.hh.size[hi];
        w.Event(SimEventType::Emigrated, w.hh.home[hi], w.hh.firstMember[hi], w.hh.size[hi]);
        w.FreeHousehold(hi);
        return;
    }
    while (w.hh.firstMember[hi]) {
        const uint32_t c = LifeWorld::Ix(w.hh.firstMember[hi]);
        w.Detach(c), w.Attach(c, HouseholdId(to));
    }
    w.hh.savings[LifeWorld::Ix(to)] += w.hh.savings[hi];
    w.Unplace(hi), w.hh.Free(w.hh.At(hi));
}

inline void LifeDie(LifeWorld& w, uint32_t ci) {
    const uint32_t hi = LifeWorld::Ix(w.cit.household[ci]);
    w.cur.deaths++;
    w.Event(SimEventType::Death, w.hh.home[hi], w.cit.At(ci).v, w.AgeOf(ci));
    w.RemoveCitizen(ci);
    if (!w.hh.size[hi]) { w.Unplace(hi), w.hh.Free(w.hh.At(hi)); return; }
    if (!w.hh.adults[hi]) LifeOrphans(w, hi);
}

inline void LifeBirth(LifeWorld& w, uint32_t mother) {
    const HouseholdId h(w.cit.household[mother]);
    const uint64_t r = w.Rng(w.cit.At(mother).v, 9);
    const CitizenId c = w.AddMember(h, (int32_t)w.month, 0, (r & 1) != 0, (uint32_t)(r >> 32));
    if (!c.Valid()) return;
    w.cur.births++;
    w.Event(SimEventType::Birth, w.hh.home[h.Index()], c.v);
    const LifeBuilding* L = w.B(w.hh.home[h.Index()]);
    if (L && (uint32_t)L->used + L->stat > L->homes) w.hh.flags[h.Index()] |= kHhCrowded | kHhLooking;
}

// The monthly visit of citizen ci (the daily slice calls it once per game day = once per life month).
inline void LifeCitizenMonth(LifeWorld& w, uint32_t ci) {
    const uint32_t raw = w.cit.At(ci).v;
    const int age = w.AgeOf(ci);
    const float hz = LifeDeathHazard(w, ci, age);
    if (w.Chance(raw, 1, 1.0f - std::exp(-hz / (float)w.cal.lifeMonthsPerYear))) { LifeDie(w, ci); return; }
    const int st = LifeStageOfAge(age, w.tune.retireAge);
    if (st != w.cit.stage[ci]) LifeStageChange(w, ci, st);
    LifeSchoolMonth(w, ci, age);
    LifeWellbeingMonth(w, ci, age);
    const uint32_t hi = LifeWorld::Ix(w.cit.household[ci]);
    if ((w.cit.flags[ci] & kCitFemale) && age >= 18 && age < 45) {
        const float p = w.tune.fertility / (27.0f * (float)w.cal.lifeMonthsPerYear) * (w.hh.adults[hi] >= 2 ? 1.3f : 0.55f);
        if (w.hh.size[hi] < 8 && w.Chance(raw, 2, p)) LifeBirth(w, ci);
    }
    if (LifeIsWorkingAge(w, ci)) {
        if (!w.cit.work[ci]) LifeQueueSeeker(w, ci);
        else if ((w.cit.commuteMin[ci] > w.tune.quitCommute || w.cit.edu[ci] > w.cit.jobLevel[ci]) && w.Chance(raw, 5, w.tune.quitRate))
            LifeQueueSeeker(w, ci); // looks for a better job, keeps this one meanwhile
        // an adult child (a parent 15+ years older in the household) may want an own home
        if (age < 35 && w.hh.adults[hi] >= 2 && w.Chance(raw, 6, w.tune.leaveHomeRate)) {
            for (uint32_t o = w.hh.firstMember[hi]; o; o = w.cit.nextInHousehold[LifeWorld::Ix(o)])
                if (w.cit.bornMonth[LifeWorld::Ix(o)] + 15 * w.cal.lifeMonthsPerYear <= w.cit.bornMonth[ci]) { w.wantsHome.push_back(raw); break; }
        }
    }
}

} // namespace city
} // namespace tbd
