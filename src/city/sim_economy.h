// The city sim's economy (Track S, phase S4): a treasury in whole cents (int64), income and expenses once per game day.
// Header only, headless (tests\sim_economy_test.cpp). All arithmetic is integer: the same city gives the same ledger.
//
//   Income   residential tax per resident per day, scaled by wealth band (60 / 90 / 120 / 180 %) and the residential tax
//            rate; commercial, industrial and office tax per filled job per day, scaled by their rates. Rates are in
//            tenths of a percent, 100 = 10 % = the tuned base (SimTuning tax*PerDay is the amount at 10 %).
//   Expenses the upkeep the host hands in (utility buildings, service and transport buildings: catalog upkeep is per game
//            month, charged a 30th a day), services scaled by their funding (50 .. 150 %).
//   Demand   a rate above 10 % lowers that category's demand (taxDemandPerPoint per point), below raises it.
// Construction costs are not charged yet (placing a building is free); see the S4 report.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_cells.h"
#include "sim_tuning.h"

namespace tbd {
namespace city {

class SimEconomy {
public:
    static constexpr int kCats = 4; // Residential, Commercial, Office, Industrial (ZoneCat order)
    int64_t treasury = 0;           // cents
    int taxRate10[kCats] = {100, 100, 100, 100}; // tenths of a percent
    // the last full day
    int64_t lastIncome[kCats] = {}, lastUpkeep = 0, lastDay = -1;
    int64_t days = 0;

    void Reset(const SimTuning& t) {
        treasury = (int64_t)t.startingMoney;
        for (int c = 0; c < kCats; ++c) taxRate10[c] = 100, lastIncome[c] = 0;
        lastUpkeep = 0, lastDay = -1, days = 0;
    }

    int64_t LastIncome() const { return lastIncome[0] + lastIncome[1] + lastIncome[2] + lastIncome[3]; }
    int64_t LastNet() const { return LastIncome() - lastUpkeep; }

    // Income of one day from the cells' counters, cents per category.
    void DayIncome(const CitySim& s, const SimTuning& t, int64_t (&out)[kCats]) const {
        static const int64_t kWealth[4] = {600, 900, 1200, 1800}; // permille
        int64_t resWeighted = 0; // residents x wealth permille
        int64_t jobs[3] = {};
        for (const CellSim& c : s.sim) {
            for (int w = 0; w < 4; ++w) resWeighted += (int64_t)c.wealth[w] * kWealth[w];
            for (int k = 0; k < 3; ++k) jobs[k] += c.employed[k];
        }
        const int64_t perRes = (int64_t)t.taxResidentialPerResidentDay, perC = (int64_t)t.taxCommercialPerJobDay;
        const int64_t perI = (int64_t)t.taxIndustrialPerJobDay, perO = (int64_t)t.taxOfficePerJobDay;
        out[0] = resWeighted * perRes * taxRate10[0] / (1000 * 100);
        out[1] = jobs[kJobCommercial] * perC * taxRate10[1] / 100;
        out[2] = jobs[kJobOffice] * perO * taxRate10[2] / 100;
        out[3] = jobs[kJobIndustrial] * perI * taxRate10[3] / 100;
    }

    // One game day: income in, upkeep (cents per day, already scaled by funding) out.
    void Day(const CitySim& s, const SimTuning& t, int64_t upkeepPerDay) {
        DayIncome(s, t, lastIncome);
        lastUpkeep = std::max<int64_t>(0, upkeepPerDay);
        treasury += LastIncome() - lastUpkeep;
        ++days;
    }

    // Demand offset of a category from its tax rate (positive: cheaper than base).
    double DemandOffset(int cat, const SimTuning& t) const { return -t.taxDemandPerPoint * ((double)taxRate10[cat] - 100.0) / 10.0; }
};

// Upkeep of one building per day in cents: catalog upkeep is per game month; funding 1 = as built.
inline int64_t UpkeepPerDay(int upkeepPerMonth, double funding, double scale) {
    return (int64_t)((double)upkeepPerMonth * 100.0 * std::max(0.0, funding) * std::max(0.0, scale) / 30.0 + 0.5);
}

} // namespace city
} // namespace tbd
