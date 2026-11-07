// The city's milestones (sim gameplay pass, phase B): population thresholds that unlock toolbar categories. Header only,
// headless (tests\sim_milestones_test.cpp). Pure data plus a tiny state machine: the host feeds the population, gets back
// the milestones newly reached (once each, in order, never lost again when the population falls), shows a toast and posts
// a "milestone_reached" news trigger for each.
#pragma once

#include <cstdint>

namespace tbd {
namespace city {

// What a milestone can unlock. The HUD maps its toolbar buttons and the transport toolbar's tabs onto these.
enum Unlock : uint32_t {
    kUnlockTerrain = 1u << 0,
    kUnlockZoning = 1u << 1,
    kUnlockRoads = 1u << 2,
    kUnlockElectricity = 1u << 3,
    kUnlockWater = 1u << 4,
    kUnlockGarbage = 1u << 5,
    kUnlockHealth = 1u << 6,
    kUnlockFire = 1u << 7,
    kUnlockPolice = 1u << 8,
    kUnlockEducation = 1u << 9,
    kUnlockComms = 1u << 10,
    kUnlockTransport = 1u << 11, // the Transportation button: bus depots and stops
    kUnlockIndustry = 1u << 12,
    kUnlockRail = 1u << 13,      // transport tabs
    kUnlockShip = 1u << 14,
    kUnlockAirport = 1u << 15,
};

struct Milestone {
    const char* name;
    uint32_t population; // reached at this many residents
    uint32_t unlocks;    // Unlock bits
    int64_t reward;      // cents paid into the treasury when reached
};

inline const Milestone* Milestones(int& n) {
    static const Milestone k[] = {
        {"Founding", 0, kUnlockTerrain | kUnlockZoning | kUnlockRoads | kUnlockElectricity | kUnlockWater, 0},
        {"Little Hamlet", 250, kUnlockGarbage | kUnlockFire, 20000 * 100LL},
        {"Worthy Village", 1000, kUnlockHealth | kUnlockPolice | kUnlockTransport, 40000 * 100LL},
        {"Busy Town", 2500, kUnlockEducation | kUnlockComms, 60000 * 100LL},
        {"Big Town", 5000, kUnlockRail | kUnlockIndustry, 100000 * 100LL},
        {"Small City", 10000, kUnlockShip, 150000 * 100LL},
        {"Big City", 20000, kUnlockAirport, 250000 * 100LL},
        {"Metropolis", 50000, 0, 500000 * 100LL},
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

// The population a set of unlock bits needs (0: from the start; UINT32_MAX: no milestone unlocks it).
inline uint32_t UnlockPopulation(uint32_t bits) {
    int n = 0;
    const Milestone* m = Milestones(n);
    for (int i = 0; i < n; ++i)
        if (m[i].unlocks & bits) return m[i].population;
    return UINT32_MAX;
}

class SimMilestones {
public:
    int reached = 0;      // milestones reached so far (index of the next one); 1 = only the founding one
    uint32_t unlocked = 0; // the union of their unlocks
    bool unlockAll = false; // sandbox / dev: everything open

    void Reset() {
        reached = 0, unlocked = 0;
        Update(0, nullptr);
    }
    bool Unlocked(uint32_t bits) const { return unlockAll || (unlocked & bits) == bits; }
    int Level() const { return reached; } // 1-based number of the current milestone

    // Feeds the population. Returns how many milestones were newly reached (their indices are reached-k .. reached-1).
    // Adds each one's reward to *treasury when given.
    int Update(uint64_t population, int64_t* treasury) {
        int n = 0;
        const Milestone* m = Milestones(n);
        int got = 0;
        while (reached < n && population >= m[reached].population) {
            unlocked |= m[reached].unlocks;
            if (treasury) *treasury += m[reached].reward;
            ++reached, ++got;
        }
        return got;
    }
    // The next milestone, or nullptr past the last.
    const Milestone* Next() const {
        int n = 0;
        const Milestone* m = Milestones(n);
        return reached < n ? &m[reached] : nullptr;
    }
    const Milestone& Current() const {
        int n = 0;
        const Milestone* m = Milestones(n);
        return m[reached > 0 ? reached - 1 : 0];
    }
};

} // namespace city
} // namespace tbd
