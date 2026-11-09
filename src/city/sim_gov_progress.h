// Progression (CS2 sim roadmap Phase 7): XP, levels, development points and the development tree. Header only, headless
// (tests\sim_gov_test). The population milestones (sim_milestones.h) stay the unlock gate of the toolbar; on top of them:
//
//   XP       from population (each resident above the best population so far), buildings (each new service or transport
//            building) and happiness (a daily share of the residents while the mean is above 60 %), like CS2
//   Levels   level L needs xpLevelBase * growth^L more XP than level L-1; every level and every milestone gives one
//            development point (milestones: their reward in money stays in sim_milestones.h)
//   Tree     12 nodes in four branches; a node costs points and needs the branch's first node. Bought nodes unlock
//            policies (sim_policy.h PolicyDef::needs) and add a small city-wide perk (PolicyMods multipliers).
#pragma once

#include <cmath>
#include <cstdint>

#include "sim_policy.h"

namespace tbd {
namespace city {

struct GovNodeDef {
    const char* name;
    const char* what;
    int cost;          // development points
    uint32_t needs;    // GovNode bits that must be bought first
    PolicyEffect fx[2]; // the city-wide perk
};
inline const GovNodeDef* GovNodes(int& n) {
    static const GovNodeDef k[] = {
        {"Efficient Services", "Service upkeep -5 %", 1, 0, {{kPmServiceUpkeep, 0.95f}}},
        {"Healthcare", "Sickness -5 %; Free Clinics", 2, kNodeEfficient, {{kPmSickness, 0.95f}}},
        {"Public Safety", "Crime -5 %; Smoke Detectors, Curfew", 2, kNodeEfficient, {{kPmCrime, 0.95f}, {kPmFire, 0.95f}}},
        {"Tax Office", "All taxes +2 %", 1, 0, {{kPmTaxRes, 1.02f}, {kPmTaxCom, 1.02f}}},
        {"Trade Agreements", "Trade income x1.5", 2, kNodeTaxOffice, {}},
        {"Small Business", "Small Business Relief", 1, kNodeTaxOffice, {{kPmTaxOff, 1.02f}}},
        {"Recycling", "Recycling policy", 1, 0, {{kPmGarbage, 0.97f}}},
        {"Clean Industry", "Pollution -5 %; Industrial Filters", 2, kNodeRecycling, {{kPmPollution, 0.95f}}},
        {"Parks Department", "Leisure +5 %; Park Maintenance", 2, kNodeRecycling, {{kPmLeisure, 1.05f}}},
        {"Transit Authority", "Car trips -3 %; Free Public Transport", 1, 0, {{kPmCarUse, 0.97f}}},
        {"Freight Planning", "Noise -5 %; Heavy Traffic Ban", 2, kNodeTransit, {{kPmNoise, 0.95f}}},
        {"Housing Board", "Rent -3 %; Rent Control", 2, kNodeTransit, {{kPmRent, 0.97f}}},
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}
inline const char* const kGovBranch[4] = {"Services", "Economy", "Environment", "Transport and housing"};

class GovProgress {
public:
    int64_t xp = 0;          // total
    int level = 0;
    int points = 0;          // unspent development points
    uint32_t bought = 0;     // GovNode bits
    int64_t bestPop = 0;     // the highest population XP was paid for
    int milestonesPaid = 0;  // milestones that already gave their point
    int64_t bestBuildings = 0; // the most service / transport buildings XP was paid for
    double levelBase = 400.0, levelGrowth = 1.35; // gov.xpLevelBase / xpLevelGrowth

    void Reset() { xp = 0, level = 0, points = 0, bought = 0, bestPop = 0, milestonesPaid = 0, bestBuildings = 0; }

    // XP from the start to level L (integer, the same on every machine: the growth is applied in fixed steps).
    int64_t XpForLevel(int L) const {
        int64_t sum = 0;
        double step = levelBase;
        for (int i = 0; i < L; ++i) sum += (int64_t)std::llround(step), step *= levelGrowth;
        return sum;
    }
    int64_t NextLevelXp() const { return XpForLevel(level + 1); }

    // Adds XP; returns the levels gained (each one a development point).
    int AddXp(int64_t v) {
        if (v <= 0) return 0;
        xp += v;
        int got = 0;
        while (level < 200 && xp >= NextLevelXp()) ++level, ++points, ++got;
        return got;
    }
    // Population XP: only growth past the best population so far counts (losing people and regaining them pays once).
    int OnPopulation(int64_t pop, double xpPerResident) {
        if (pop <= bestPop) return 0;
        const int64_t gain = pop - bestPop;
        bestPop = pop;
        return AddXp((int64_t)std::llround((double)gain * xpPerResident));
    }
    // The milestones reached so far (sim_milestones.h reached): each new one past the founding gives a point.
    int OnMilestones(int reached) {
        const int paid = reached > 1 ? reached - 1 : 0;
        const int got = paid > milestonesPaid ? paid - milestonesPaid : 0;
        milestonesPaid = paid > milestonesPaid ? paid : milestonesPaid;
        points += got;
        return got;
    }
    bool CanBuy(int node) const {
        int n = 0;
        const GovNodeDef* d = GovNodes(n);
        if (node < 0 || node >= n || (bought & (1u << node))) return false;
        return points >= d[node].cost && (bought & d[node].needs) == d[node].needs;
    }
    bool Buy(int node) {
        if (!CanBuy(node)) return false;
        int n = 0;
        points -= GovNodes(n)[node].cost;
        bought |= 1u << node;
        return true;
    }
    bool Has(uint32_t nodeBits) const { return (bought & nodeBits) == nodeBits; }
    // The bought nodes' perks, city wide.
    void ApplyPerks(PolicyMods& m) const {
        int n = 0;
        const GovNodeDef* d = GovNodes(n);
        for (int i = 0; i < n; ++i)
            if (bought & (1u << i)) ApplyEffects(m, d[i].fx, 2);
    }
};

} // namespace city
} // namespace tbd
