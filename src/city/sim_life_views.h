// Phase 1 read-only views for the UI and the active tier (CS2 sim roadmap Phase 1). Header only, headless
// (tests\sim_citizens_test). Nothing here changes a record.
//
//   LifePopulation    the Population panel: age pyramid (5-year bins by sex), education mix of adults, employment,
//                     and the monthly series (births, deaths, immigrants, emigrants) for sparklines.
//   LifeBuildingInfo  the building inspector: households and each member's age, education, job, commute, happiness
//                     and its top two reasons (sim_wellbeing.h terms).
//   LifeActivityAt    the schedule of one citizen at a game minute, a pure function of the record and its seed
//                     (wake, commute, work or school, shop, leisure, home, sleep) with a seeded jitter.
//   LifeActiveSet     at most 512 citizens of the buildings near the camera, ticked each minute: it reports who
//                     changed activity so the host can spawn or retire their car or walker.
#pragma once

#include <cstdint>
#include <vector>

#include "sim_wellbeing.h"

namespace tbd {
namespace city {

struct LifePopulationStats {
    static constexpr int kBins = 21; // 0-4, 5-9 .. 100+
    uint32_t male[kBins] = {}, female[kBins] = {};
    uint32_t eduAdults[kEduLevels] = {};
    uint32_t adults = 0, working = 0, seekers = 0, students = 0, retired = 0, statistical = 0;
    std::vector<int32_t> births, deaths, immigrants, emigrants, population; // oldest first, one per closed month
};

inline void LifePopulation(const LifeWorld& w, LifePopulationStats& s) {
    s = LifePopulationStats();
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) {
        if (!w.cit.At(ci).Valid()) continue;
        const int bin = std::min(LifePopulationStats::kBins - 1, std::max(0, w.AgeOf(ci)) / 5);
        ((w.cit.flags[ci] & kCitFemale) ? s.female : s.male)[bin]++;
        if (w.cit.stage[ci] == kLifeAdult) {
            s.adults++, s.eduAdults[w.cit.edu[ci]]++;
            s.working += w.cit.work[ci] != 0, s.students += (w.cit.flags[ci] & kCitStudent) != 0;
            s.seekers += LifeIsWorkingAge(w, ci) && !w.cit.work[ci];
        }
        s.retired += (w.cit.flags[ci] & kCitRetired) != 0;
    }
    s.statistical = (uint32_t)w.statResidents;
    for (const LifeMonthStats& m : w.history) {
        s.births.push_back(m.births), s.deaths.push_back(m.deaths), s.immigrants.push_back(m.immigrants);
        s.emigrants.push_back(m.emigrants), s.population.push_back(m.population);
    }
}

struct LifeMemberInfo {
    CitizenId id;
    uint32_t seed = 0, work = 0;
    int age = 0, edu = 0, jobLevel = 0, commuteMin = 0, happy = 0, health = 0;
    uint8_t flags = 0;
    int reason[2] = {-1, -1}, reasonPts[2] = {}; // sim_wellbeing.h LifeReason and its points
};
struct LifeHouseholdInfo {
    HouseholdId id;
    int32_t savings = 0, income = 0, rent = 0;
    int wealth = 0;
    std::vector<LifeMemberInfo> members;
};
inline void LifeBuildingInfo(const LifeWorld& w, uint32_t building, std::vector<LifeHouseholdInfo>& out, uint32_t& statistical) {
    out.clear();
    statistical = 0;
    const uint32_t i = LifeWorld::Ix(building);
    if (!building || i >= w.bld.size() || w.bld[i].id != building) return;
    statistical = w.bld[i].stat;
    for (uint32_t h = w.bld[i].firstHousehold; h; h = w.hh.nextInHome[LifeWorld::Ix(h)]) {
        const uint32_t hi = LifeWorld::Ix(h);
        LifeHouseholdInfo H;
        H.id = HouseholdId(h), H.savings = w.hh.savings[hi], H.income = w.hh.incomeMonth[hi], H.rent = w.hh.rentMonth[hi], H.wealth = w.hh.wealth[hi];
        for (uint32_t c = w.hh.firstMember[hi]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)]) {
            const uint32_t ci = LifeWorld::Ix(c);
            LifeMemberInfo m;
            m.id = CitizenId(c), m.seed = w.cit.seed[ci], m.work = w.cit.work[ci], m.age = w.AgeOf(ci), m.edu = w.cit.edu[ci];
            m.jobLevel = w.cit.jobLevel[ci], m.commuteMin = w.cit.commuteMin[ci], m.happy = w.cit.happy[ci], m.health = w.cit.health[ci];
            m.flags = w.cit.flags[ci];
            LifeTerms t;
            LifeHappyTerms(w, ci, t);
            t.Top2(m.reason);
            for (int k = 0; k < 2; ++k) m.reasonPts[k] = m.reason[k] >= 0 ? t.v[m.reason[k]] : 0;
            H.members.push_back(m);
        }
        out.push_back(H);
    }
}

enum LifeActivity : uint8_t {
    kActSleep = 0, kActHome, kActToWork, kActWork, kActToSchool, kActSchool, kActShop, kActLeisure, kActGoHome, kActivities
};
inline const char* LifeActivityName(int a) {
    static const char* const k[kActivities] = {"Sleeping", "At home", "Going to work", "At work", "Going to school", "At school",
                                               "Shopping", "Out", "Going home"};
    return a >= 0 && a < kActivities ? k[a] : "";
}

// What citizen ci is doing at `minute`: a pure function of the record, its seed and the calendar.
inline uint8_t LifeActivityAt(const LifeWorld& w, uint32_t ci, int64_t minute) {
    const int t = (int)SimCalendar::Mod(minute, kMinutesPerDay);
    const uint32_t sd = w.cit.seed[ci];
    const int j = (int)(sd % 61), jl = (int)((sd >> 8) % 91);
    const bool weekend = w.cal.Weekend(minute), shops = (sd >> 16 & 3) == 0, out = (sd >> 18 & 3) < 2;
    const int st = w.cit.stage[ci];
    const int wake = (st == kLifeSenior ? 450 : 360) + j, sleep = (st <= kLifeChild ? 1230 : 1350) + j / 2;
    if (t < wake || t >= sleep) return kActSleep;
    if (st == kLifeBaby) return kActHome;
    if (!weekend && (w.cit.flags[ci] & kCitStudent)) { // school 08:00 - 15:00, a 15-minute walk (college: 09:00 - 16:00)
        const int start = st == kLifeAdult ? 540 : 480, end = start + 420;
        if (t >= start - 15 && t < start) return kActToSchool;
        if (t >= start && t < end) return kActSchool;
        if (t >= end && t < end + 15) return kActGoHome;
    } else if (!weekend && w.cit.work[ci]) { // work 8 hours from 08:00 + jitter, the commute both ways
        const int c = std::max(5, (int)w.cit.commuteMin[ci]), start = 480 + j, end = start + 510;
        if (t >= start - c && t < start) return kActToWork;
        if (t >= start && t < end) return kActWork;
        if (t >= end && t < end + c) return kActGoHome;
        if (shops && t >= end + c && t < end + c + 45) return kActShop;
        if (out && t >= 1170 + jl && t < 1290 + jl / 2) return kActLeisure;
        return kActHome;
    }
    // weekends, the jobless, the retired, kids after school
    if (shops && t >= 600 + jl && t < 660 + jl) return kActShop;
    if (out && t >= (weekend ? 840 : 930) + jl && t < (weekend ? 1080 : 1050) + jl) return kActLeisure;
    return kActHome;
}

struct LifeActivityChange { CitizenId c; uint8_t from = 0, to = 0; };

class LifeActiveSet {
public:
    static constexpr uint32_t kCap = 512;
    std::vector<uint32_t> members; // CitizenId raw
    std::vector<uint8_t> act;

    // Residents of `buildings` (nearest to the camera first) up to the cap. Members kept from before keep their state.
    void Refresh(const LifeWorld& w, const std::vector<uint32_t>& buildings, int64_t minute, uint32_t cap = kCap) {
        std::vector<uint32_t> next;
        std::vector<uint8_t> nextAct;
        for (uint32_t b : buildings) {
            const uint32_t i = LifeWorld::Ix(b);
            if (!b || i >= w.bld.size() || w.bld[i].id != b) continue;
            for (uint32_t h = w.bld[i].firstHousehold; h && next.size() < cap; h = w.hh.nextInHome[LifeWorld::Ix(h)])
                for (uint32_t c = w.hh.firstMember[LifeWorld::Ix(h)]; c && next.size() < cap; c = w.cit.nextInHousehold[LifeWorld::Ix(c)]) {
                    next.push_back(c);
                    nextAct.push_back(LifeActivityAt(w, LifeWorld::Ix(c), minute));
                }
            if (next.size() >= cap) break;
        }
        members.swap(next), act.swap(nextAct);
    }
    // Each minute: who changed what they do (the host moves their car or walker). Dead or gone members drop out.
    void Tick(const LifeWorld& w, int64_t minute, std::vector<LifeActivityChange>& out) {
        out.clear();
        size_t k = 0;
        for (size_t i = 0; i < members.size(); ++i) {
            const CitizenId c(members[i]);
            if (!w.cit.Alive(c)) continue;
            const uint8_t a = LifeActivityAt(w, c.Index(), minute);
            if (a != act[i]) out.push_back({c, act[i], a});
            members[k] = members[i], act[k] = a, ++k;
        }
        members.resize(k), act.resize(k);
    }
};

} // namespace city
} // namespace tbd
