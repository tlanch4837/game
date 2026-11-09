// CellFactors: the one table that decouples the sim phases (CS2 sim roadmap 3.2, Phase 0; frozen interface). Header only.
//
// Per CitySim cell (index-parallel to CitySim::sim, grown with it), everything that moves health, happiness, demand or
// land value. Every field has exactly ONE writer phase (named below) and any number of readers. A field nobody writes
// yet keeps its neutral default, so a reader phase may merge before its writer. Formulas that combine factors live only
// in Phase 1's sim_wellbeing.h; other phases add factors, never formulas.
//
// Scale: uint8 0..255 unless noted. "Neutral" is the value that changes nothing (no pollution, average coverage, ...).
#pragma once

#include <cstdint>
#include <vector>

namespace tbd {
namespace city {

constexpr int kServicesV2 = 12; // coverage[] slots (sim_coverage.h's 6 first, in its order, then the Phase 4 services)
enum ServiceV2 : int {
    kSvcFire = 0, kSvcPolice = 1, kSvcHealth = 2, kSvcSchool = 3, kSvcPark = 4, kSvcTransit = 5, // = sim_coverage.h Service
    kSvcDeathcare = 6, kSvcGarbage = 7, kSvcPost = 8, kSvcTelecom = 9, kSvcHighSchool = 10, kSvcUniversity = 11,
};

struct CellFactor {
    uint8_t air = 0;        // P3 sim_pollution.h: air pollution (0 clean)
    uint8_t noise = 0;      // P3: noise
    uint8_t ground = 0;     // P3: ground pollution
    uint8_t water = 0;      // P3: water pollution of the cell's supply
    uint8_t crime = 0;      // P4 sim_incidents.h: crime rate
    uint8_t fireRisk = 0;   // P4: fire hazard
    uint8_t coverage[kServicesV2] = {}; // P4 sim_services.h (until then: mirrored from CellSim::coverage by P0's host)
    uint8_t telecom = 128;  // P4: network quality (128 neutral)
    uint8_t parkAccess = 0; // P4: parks and leisure in reach
    uint8_t rentBurden = 64;// P2 sim_land.h: rent as a share of income (64 = 25 %, 255 = 100 %)
    uint16_t commuteMin = 0;// P1 sim_jobs.h: mean commute of the cell's workers, whole minutes
    uint8_t unemployment = 0; // P1: share of the cell's adults without work (255 = all)
    uint8_t goodsAvailable = 255; // P6 sim_goods.h: shops supplied (255 fully)
    uint8_t taxBurden = 64; // P7 sim_economy.h: tax load relative to 10 % (64 neutral)
};

// Multipliers that policies set per hood or district (P7 sim_policies.h writes; everyone reads). Default 1 = no policy.
struct PolicyMods {
    float serviceUpkeep = 1.0f, serviceCapacity = 1.0f;
    float crimeRate = 1.0f, fireRate = 1.0f, sicknessRate = 1.0f;
    float pollutionEmit = 1.0f, noiseEmit = 1.0f, garbageMade = 1.0f;
    float transitFare = 1.0f, carUse = 1.0f, heavyTraffic = 1.0f;
    float rent = 1.0f, taxResidential = 1.0f, taxCommercial = 1.0f, taxIndustrial = 1.0f, taxOffice = 1.0f;
    float happiness = 1.0f, leisure = 1.0f;
    // P7: the Budget panel's per-service slider (50 .. 150 % -> 0.5 .. 1.5), by ServiceV2 slot. A service's capacity and
    // efficiency multiplier is ServiceCapacity(svc) (the policy multiplier times the budget); its upkeep scales the same.
    float serviceBudget[kServicesV2] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float ServiceCapacity(int svc) const { return serviceCapacity * (svc >= 0 && svc < kServicesV2 ? serviceBudget[svc] : 1.0f); }
};

class CellFactors {
public:
    std::vector<CellFactor> cells; // per CitySim cell index
    std::vector<PolicyMods> hoods; // per hood id (sim_hoods.h); P7 maps districts onto them
    uint64_t version = 0;          // bumped by any writer after a pass that changed something

    // Grows to n cells (new cells neutral). Never shrinks while the city lives; Clear on a city reset.
    void Fit(size_t cellCount) { if (cells.size() < cellCount) cells.resize(cellCount); }
    void FitHoods(size_t hoodCount) { if (hoods.size() < hoodCount) hoods.resize(hoodCount); }
    const CellFactor& At(int cell) const { static const CellFactor kNeutral; return cell >= 0 && (size_t)cell < cells.size() ? cells[(size_t)cell] : kNeutral; }
    CellFactor* Mut(int cell) { return cell >= 0 && (size_t)cell < cells.size() ? &cells[(size_t)cell] : nullptr; }
    const PolicyMods& Mods(int hood) const { static const PolicyMods kNone; return hood >= 0 && (size_t)hood < hoods.size() ? hoods[(size_t)hood] : kNone; }
    void Clear() { cells.clear(), hoods.clear(), ++version; }
};

} // namespace city
} // namespace tbd
