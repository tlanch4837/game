// Citizen and household record stores (CS2 sim roadmap 3.2, Phase 1). Header only, headless (tests\sim_citizens_test).
//
// Every resident is a record: SoA columns indexed by CitizenId::Index(), slots handed out by a HandlePool (sim_ids.h) so
// a stale id never matches a new occupant. Households hold their members as an intrusive list (firstMember ->
// nextInHousehold) and sit in their home building's intrusive list (LifeBuilding::firstHousehold -> nextInHome).
// Age is never stored: bornMonth is the life month of birth and stage is the life stage the counters last saw.
// Names in this header carry a Life / Cit / Hh prefix: the unity build shares one namespace with sim_cells.h.
#pragma once

#include <cstdint>
#include <vector>

#include "sim_ids.h"

namespace tbd {
namespace city {

// Life stages (roadmap: 0-5 / 6-12 / 13-17 / 18-64 / 65+). The counters' age bands: kids (baby, child, teen), adults,
// seniors, in the same order as sim_cells.h's AgeBand.
enum LifeStage : uint8_t { kLifeBaby = 0, kLifeChild = 1, kLifeTeen = 2, kLifeAdult = 3, kLifeSenior = 4, kLifeStages = 5 };
inline int LifeStageOfAge(int age, int retireAge = 65) { return age < 6 ? 0 : age < 13 ? 1 : age < 18 ? 2 : age < retireAge ? 3 : 4; }
inline int LifeBandOfStage(int s) { return s <= kLifeTeen ? 0 : s == kLifeAdult ? 1 : 2; }

enum CitFlag : uint8_t {
    kCitAlive = 1, kCitFemale = 2, kCitStudent = 4, kCitRetired = 8, kCitSick = 16, kCitCriminal = 32, kCitPinned = 64,
    kCitLeaving = 128, // wants an own home (an adult child) or is leaving the city
};
enum HhFlag : uint8_t { kHhLooking = 1, kHhLeaving = 2, kHhCrowded = 4 };

struct CitizenStore {
    HandlePool pool;
    // hot
    std::vector<uint32_t> household; // HouseholdId
    std::vector<uint32_t> work;      // BuildingId of the job, kNone
    std::vector<int32_t> bornMonth;  // life month of birth
    std::vector<uint32_t> school;    // BuildingId of a school seat (Phase 4 writes through SetSchool), kNone
    std::vector<uint8_t> edu;        // 0 none .. 4 university
    std::vector<uint8_t> jobLevel;   // edu level of the job slot held
    std::vector<uint8_t> health, happy, flags, stage;
    std::vector<uint16_t> commuteMin;
    // cold
    std::vector<uint32_t> seed;            // name, looks, personality
    std::vector<uint32_t> nextInHousehold; // CitizenId

    uint32_t Capacity() const { return pool.Capacity(); }
    uint32_t Live() const { return pool.Live(); }
    bool Alive(CitizenId c) const { return pool.Alive(c); }
    CitizenId At(uint32_t i) const { return CitizenId(pool.At(i)); }
    CitizenId Alloc() {
        const CitizenId c(pool.Alloc());
        if (!c.Valid()) return c;
        Fit();
        const uint32_t i = c.Index();
        household[i] = work[i] = school[i] = seed[i] = nextInHousehold[i] = 0;
        bornMonth[i] = 0, edu[i] = jobLevel[i] = 0, health[i] = 200, happy[i] = 128, flags[i] = kCitAlive, stage[i] = 0;
        commuteMin[i] = 0;
        return c;
    }
    void Free(CitizenId c) { if (pool.Free(c)) flags[c.Index()] = 0; }
    void Fit() {
        const size_t n = pool.Capacity();
        if (household.size() >= n) return;
        household.resize(n), work.resize(n), bornMonth.resize(n), school.resize(n), edu.resize(n), jobLevel.resize(n);
        health.resize(n), happy.resize(n), flags.resize(n), stage.resize(n), commuteMin.resize(n), seed.resize(n);
        nextInHousehold.resize(n);
    }
    void Clear() { *this = CitizenStore(); }
};

struct HouseholdStore {
    HandlePool pool;
    std::vector<uint32_t> home;        // BuildingId, kNone while being placed
    std::vector<uint32_t> firstMember; // CitizenId
    std::vector<uint32_t> nextInHome;  // HouseholdId
    std::vector<int32_t> savings, incomeMonth, rentMonth, spendMonth, movedInMonth; // whole currency, life months
    std::vector<uint8_t> size, adults, carCount, flags, wealth, unhappyMonths, poorMonths;

    uint32_t Capacity() const { return pool.Capacity(); }
    uint32_t Live() const { return pool.Live(); }
    bool Alive(HouseholdId h) const { return pool.Alive(h); }
    HouseholdId At(uint32_t i) const { return HouseholdId(pool.At(i)); }
    HouseholdId Alloc() {
        const HouseholdId h(pool.Alloc());
        if (!h.Valid()) return h;
        Fit();
        const uint32_t i = h.Index();
        home[i] = firstMember[i] = nextInHome[i] = 0;
        savings[i] = incomeMonth[i] = rentMonth[i] = spendMonth[i] = movedInMonth[i] = 0;
        size[i] = adults[i] = carCount[i] = flags[i] = wealth[i] = unhappyMonths[i] = poorMonths[i] = 0;
        return h;
    }
    void Free(HouseholdId h) { pool.Free(h); }
    void Fit() {
        const size_t n = pool.Capacity();
        if (home.size() >= n) return;
        home.resize(n), firstMember.resize(n), nextInHome.resize(n), savings.resize(n), incomeMonth.resize(n);
        rentMonth.resize(n), spendMonth.resize(n), movedInMonth.resize(n), size.resize(n), adults.resize(n);
        carCount.resize(n), flags.resize(n), wealth.resize(n), unhappyMonths.resize(n), poorMonths.resize(n);
    }
    void Clear() { *this = HouseholdStore(); }
};

} // namespace city
} // namespace tbd
