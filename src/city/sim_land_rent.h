// Rent, density and affordability (CS2 sim roadmap Phase 2). Header only, headless (tests\sim_land_test).
//
//   Density   the residential zone types are four tiers: low, row, medium, high (zoning.h ZoneTypes order); the other
//             zones are mixed / non-residential (tier 2 for rent: shops and offices share their land like apartments).
//   Rent      CellFactor::rentBurden is Phase 1's rent multiplier (64 = the flat rent, sim_household_econ.h LifeRentFor):
//             64 x (floor + (1 - floor) x land / reference) x the cell's density factor x the policy rent factor.
//             Dense housing shares the land, so the rent per household is lower at the same land value.
//   Afford    the households are visited in 96 groups a game day (one per 15 minutes, round robin): a household whose
//             rent is over afford% of its income (or that has rent and no income) is burdened. The day's shares per
//             wealth band feed residential demand; a building with half or more of its households burdened gets the
//             high rent flag (sim_flags.h kFlagHighRent, P2's bit).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "zoning.h"

namespace tbd {
namespace city {

enum DensityTier : int { kDensityLow = 0, kDensityRow = 1, kDensityMedium = 2, kDensityHigh = 3, kDensityTiers = 4 };

// The density tier of a zone type (ZoneTypes index).
inline int DensityOf(int zoneType) {
    const auto& t = ZoneTypes();
    if (zoneType < 0 || zoneType >= (int)t.size()) return kDensityLow;
    return t[(size_t)zoneType].cat == ZoneCat::Residential ? std::min(zoneType, 3) : kDensityMedium;
}
inline const char* DensityName(int tier) {
    static const char* const k[kDensityTiers] = {"Low density", "Row housing", "Medium density", "High density"};
    return k[std::clamp(tier, 0, kDensityTiers - 1)];
}

struct RentParams {
    int floorPct = 45;       // the rent multiplier at land value 0, percent of the flat rent
    int referenceLand = 110; // land value at which the rent is the flat rent (times the density factor)
    int densityPct[kDensityTiers] = {115, 100, 85, 70};
    int affordPct = 40;      // rent over this share of income: burdened
    int highRentShare = 50;  // percent of a building's households burdened for the flag
};

// rentBurden (64 = flat) for a cell of land value lv and density tier; policy: PolicyMods::rent.
inline uint8_t RentBurden(const RentParams& p, int lv, int tier, float policy = 1.0f) {
    const int64_t f = p.floorPct * 100 + (int64_t)(100 - p.floorPct) * 100 * lv / std::max(1, p.referenceLand); // x 1e4
    const int64_t d = p.densityPct[std::clamp(tier, 0, kDensityTiers - 1)];
    const int64_t pol = (int64_t)(policy * 100.0f + 0.5f);
    const int64_t v = 64 * f * d / 100 * pol / 100 / 10000;
    return (uint8_t)std::clamp<int64_t>(v, 8, 255);
}

inline bool RentBurdened(const RentParams& p, int32_t rent, int32_t income) {
    if (rent <= 0) return false;
    return income <= 0 || (int64_t)rent * 100 > (int64_t)income * p.affordPct;
}

// The day's affordability, built group by group. Per wealth band: households seen, burdened; spending total.
struct Affordability {
    int32_t households[4] = {}, burdened[4] = {};
    int64_t spend = 0, income = 0, persons = 0;
    // A weighted burdened share 0..1 (the poorer bands weigh more: they are who cannot move in).
    double Pressure() const {
        static const double kW[4] = {1.0, 0.7, 0.4, 0.2};
        double n = 0, b = 0;
        for (int i = 0; i < 4; ++i) n += kW[i] * households[i], b += kW[i] * burdened[i];
        return n > 0 ? b / n : 0.0;
    }
    int32_t Households() const { return households[0] + households[1] + households[2] + households[3]; }
};

// Per building (BuildingId index) the day's counts; the host turns them into flags at the end of the day.
struct RentDay {
    Affordability cur, last;         // today so far, and the last full day
    std::vector<uint16_t> seen, hit; // per building index: households visited today, burdened
    int group = 0;                   // the next group (0..95)
    void Clear() { *this = RentDay(); }
    void Count(uint32_t building, bool burdened) {
        if (building >= seen.size()) seen.resize(building + 1, 0), hit.resize(building + 1, 0);
        seen[building] = (uint16_t)std::min(65535, seen[building] + 1);
        if (burdened) hit[building] = (uint16_t)std::min(65535, hit[building] + 1);
    }
    bool High(const RentParams& p, uint32_t building) const {
        return building < seen.size() && seen[building] > 0 && (int)hit[building] * 100 >= (int)seen[building] * p.highRentShare;
    }
};

} // namespace city
} // namespace tbd
