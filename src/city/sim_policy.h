// Policies (CS2 sim roadmap Phase 7): the catalog and how a set of them turns into PolicyMods (sim_factors.h), which every
// other phase reads per hood. Header only, headless (tests\sim_gov_test).
//
//   A policy is city wide, per district, or either (its scope bits). Each one multiplies a few PolicyMods fields and may
//   cost money per resident it covers per day. Mods compose by multiplication, so two policies that both cut crime stack
//   and switching one off restores the value exactly (the mods are always recomposed from scratch, never patched).
//   Some policies need a node of the progression tree (sim_gov_progress.h) first (needs: GovNode bits).
#pragma once

#include <cstdint>

#include "sim_factors.h"

namespace tbd {
namespace city {

enum PolicyField : int8_t {
    kPmServiceUpkeep, kPmServiceCapacity, kPmCrime, kPmFire, kPmSickness, kPmPollution, kPmNoise, kPmGarbage,
    kPmTransitFare, kPmCarUse, kPmHeavyTraffic, kPmRent, kPmTaxRes, kPmTaxCom, kPmTaxInd, kPmTaxOff, kPmHappiness,
    kPmLeisure, kPmFields
};
inline float* PolicyModField(PolicyMods& m, int f) {
    switch (f) {
    case kPmServiceUpkeep: return &m.serviceUpkeep;
    case kPmServiceCapacity: return &m.serviceCapacity;
    case kPmCrime: return &m.crimeRate;
    case kPmFire: return &m.fireRate;
    case kPmSickness: return &m.sicknessRate;
    case kPmPollution: return &m.pollutionEmit;
    case kPmNoise: return &m.noiseEmit;
    case kPmGarbage: return &m.garbageMade;
    case kPmTransitFare: return &m.transitFare;
    case kPmCarUse: return &m.carUse;
    case kPmHeavyTraffic: return &m.heavyTraffic;
    case kPmRent: return &m.rent;
    case kPmTaxRes: return &m.taxResidential;
    case kPmTaxCom: return &m.taxCommercial;
    case kPmTaxInd: return &m.taxIndustrial;
    case kPmTaxOff: return &m.taxOffice;
    case kPmHappiness: return &m.happiness;
    case kPmLeisure: return &m.leisure;
    }
    return nullptr;
}

// Progression tree nodes (sim_gov_progress.h holds their names, costs and perks); policies name the one they need.
enum GovNode : uint32_t {
    kNodeEfficient = 1u << 0, kNodeHealthcare = 1u << 1, kNodeSafety = 1u << 2,     // services
    kNodeTaxOffice = 1u << 3, kNodeTrade = 1u << 4, kNodeSmallBiz = 1u << 5,        // economy
    kNodeRecycling = 1u << 6, kNodeFilters = 1u << 7, kNodeParks = 1u << 8,         // environment
    kNodeTransit = 1u << 9, kNodeFreight = 1u << 10, kNodeHousing = 1u << 11,       // transport and housing
};

enum PolicyScope : uint8_t { kScopeCity = 1, kScopeDistrict = 2 };

struct PolicyEffect {
    int8_t field = -1;
    float mul = 1.0f;
};
struct PolicyDef {
    const char* id;
    const char* name;
    const char* what;     // one line for the panel
    uint8_t scope;        // PolicyScope bits
    int costPerResDay;    // cents per covered resident per day
    uint32_t needs;       // GovNode bits (0: open from the start)
    PolicyEffect fx[3];
};

inline const PolicyDef* Policies(int& n) {
    static const PolicyDef k[] = {
        {"watch", "Neighbourhood Watch", "Crime -8 %", kScopeDistrict, 1, 0, {{kPmCrime, 0.92f}}},
        {"austerity", "Austerity", "Service upkeep and capacity -10 %, unhappy", kScopeCity, 0, 0,
         {{kPmServiceUpkeep, 0.9f}, {kPmServiceCapacity, 0.9f}, {kPmHappiness, 0.98f}}},
        {"clinics", "Free Clinics", "Sickness -15 %, happier", kScopeDistrict, 8, kNodeHealthcare, {{kPmSickness, 0.85f}, {kPmHappiness, 1.02f}}},
        {"smoke", "Smoke Detectors", "Fires -20 %", kScopeDistrict, 3, kNodeSafety, {{kPmFire, 0.8f}}},
        {"curfew", "Curfew", "Crime -15 %, less leisure", kScopeDistrict, 2, kNodeSafety,
         {{kPmCrime, 0.85f}, {kPmLeisure, 0.85f}, {kPmHappiness, 0.98f}}},
        {"freetransit", "Free Public Transport", "No fares, 15 % fewer car trips", kScopeCity | kScopeDistrict, 10, kNodeTransit,
         {{kPmTransitFare, 0.0f}, {kPmCarUse, 0.85f}, {kPmHappiness, 1.01f}}},
        {"recycling", "Recycling", "Garbage -20 %, pollution -3 %", kScopeDistrict, 5, kNodeRecycling, {{kPmGarbage, 0.8f}, {kPmPollution, 0.97f}}},
        {"heavyban", "Heavy Traffic Ban", "No trucks through, noise -15 %, industry tax -5 %", kScopeDistrict, 0, kNodeFreight,
         {{kPmHeavyTraffic, 0.0f}, {kPmNoise, 0.85f}, {kPmTaxInd, 0.95f}}},
        {"rent", "Rent Control", "Rent -15 %, happier, residential tax -3 %", kScopeDistrict, 0, kNodeHousing,
         {{kPmRent, 0.85f}, {kPmHappiness, 1.02f}, {kPmTaxRes, 0.97f}}},
        {"smallbiz", "Small Business Relief", "Commercial tax -20 %", kScopeDistrict, 0, kNodeSmallBiz, {{kPmTaxCom, 0.8f}, {kPmHappiness, 1.01f}}},
        {"filters", "Industrial Filters", "Pollution -20 %, industry tax -10 %", kScopeDistrict, 0, kNodeFilters, {{kPmPollution, 0.8f}, {kPmTaxInd, 0.9f}}},
        {"parks", "Park Maintenance", "Leisure +15 %, happier", kScopeDistrict, 4, kNodeParks, {{kPmLeisure, 1.15f}, {kPmHappiness, 1.02f}}},
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}
inline int PolicyCount() { int n = 0; Policies(n); return n; }
inline int PolicyIndex(const char* id) {
    int n = 0;
    const PolicyDef* p = Policies(n);
    for (int i = 0; i < n; ++i)
        for (int c = 0;; ++c) {
            if (p[i].id[c] != id[c]) break;
            if (!id[c]) return i;
        }
    return -1;
}

inline void ApplyEffects(PolicyMods& m, const PolicyEffect* fx, int n) {
    for (int i = 0; i < n; ++i)
        if (float* f = PolicyModField(m, fx[i].field)) *f *= fx[i].mul;
}
// Multiplies every policy in mask (bit i = Policies()[i]) into m.
inline void ApplyPolicies(PolicyMods& m, uint32_t mask) {
    int n = 0;
    const PolicyDef* p = Policies(n);
    for (int i = 0; i < n && mask; ++i)
        if (mask & (1u << i)) ApplyEffects(m, p[i].fx, 3);
}

} // namespace city
} // namespace tbd
