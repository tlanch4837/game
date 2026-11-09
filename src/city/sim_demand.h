// RCI(+Office) demand from causes (CS2 sim roadmap Phase 2). Header only, headless (tests\sim_demand_test).
//
// Each bar is a sum of named factors (the Demand panel lists them; they add up to the raw bar before the clamp):
//   Residential  start-up pull of a tiny town, a base, free homes (fewer free homes = more demand), open jobs against
//                job seekers, migration (Phase 1 attractiveness), rent pressure (burdened households, poor bands
//                weigh more), taxes
//   Commercial   shoppers (residents x commercialPerResident against shop jobs), household spending against normal,
//                shop supply (Phase 6 goods availability: shops without goods do not open more shops), taxes
//   Office       educated job seekers (education 2..4) against office vacancies, taxes
//   Industrial   less educated job seekers (education 0..2) against factory vacancies, the goods the shops need from
//                industry, the shops' goods shortfall (Phase 6), taxes
// What is under construction counts as built: its homes as future seekers, its jobs as future vacancies.
// Smoothing: the bars move toward the clamped raw value over tauMinutes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace tbd {
namespace city {

constexpr int kDemandCats = 4; // ZoneCat order: Residential, Commercial, Office, Industrial
constexpr int kDemandMaxFactors = 7;

struct DemandParams {
    double startPull = 0.6, startPopulation = 300; // a tiny town wants homes
    double baseResidential = 0.15;
    double freeHomes = 2.5, freeHomesOk = 0.05;    // per share of homes free beyond freeHomesOk
    double jobs = 1.2;                             // (vacancies - seekers) / adults
    double migration = 0.35;                       // per attractiveness above 1
    double rent = 0.8;                             // per weighted share burdened
    double shoppers = 0.9, commercialPerResident = 0.12, spending = 0.3, supply = 0.4;
    double workers = 1.6;                          // seekers / adults (office, industry)
    double goods = 0.6, industryPerCommercial = 0.8, shortfall = 0.5;
    double tauMinutes = 1440;
};

struct DemandInput {
    int64_t population = 0, adults = 0, homes = 0, freeHomes = 0;
    int64_t pendingHomes = 0, pendingJobs[3] = {};  // under construction (JobKind order: industrial, commercial, office)
    int64_t seekers[5] = {};                        // job seekers by education level (0..4)
    int64_t vacancies[3] = {}, jobs[3] = {};        // by JobKind
    double attract = 1.0;                           // Phase 1 migration attractiveness (1 neutral)
    double rentPressure = 0.0;                      // Affordability::Pressure
    double spendIndex = 1.0;                        // household spending per resident against the normal (1)
    int goodsAvailable = 255;                       // city mean, Phase 6
    double tax[kDemandCats] = {};                   // the economy's demand offsets (taxes away from 10 %)
};

struct DemandFactor {
    const char* name = "";
    double value = 0;
};
struct DemandBar {
    DemandFactor f[kDemandMaxFactors];
    int n = 0;
    double raw = 0;     // sum of the factors (unclamped)
    void Add(const char* name, double v) { if (n < kDemandMaxFactors) f[n].name = name, f[n].value = v, ++n, raw += v; }
};
struct DemandResult {
    DemandBar bar[kDemandCats];
};

inline DemandResult ComputeDemand(const DemandInput& in, const DemandParams& p) {
    DemandResult r;
    const double pendAdults = (double)in.pendingHomes * 0.65;
    const double A = std::max(50.0, (double)in.adults + pendAdults);
    const double P = (double)(in.population + in.pendingHomes);
    double seek = pendAdults, vac = 0;
    for (int e = 0; e < 5; ++e) seek += (double)in.seekers[e];
    for (int k = 0; k < 3; ++k) vac += (double)(in.vacancies[k] + in.pendingJobs[k]);
    const double homes = std::max(50.0, (double)(in.homes + in.pendingHomes));
    const double freeShare = (double)(in.freeHomes + in.pendingHomes) / homes;
    DemandBar& R = r.bar[0];
    R.Add("Starting town", p.startPull * std::max(0.0, 1.0 - P / std::max(1.0, p.startPopulation)));
    R.Add("Base demand", p.baseResidential);
    R.Add("Free homes", -p.freeHomes * std::max(0.0, freeShare - p.freeHomesOk));
    R.Add("Jobs open", p.jobs * (vac - seek) / A);
    R.Add("Migration", p.migration * (in.attract - 1.0));
    R.Add("Rent", -p.rent * in.rentPressure);
    R.Add("Taxes", in.tax[0]);
    const double comm = (double)(in.jobs[1] + in.pendingJobs[1]), want = P * p.commercialPerResident;
    const double supplyGap = (255.0 - std::clamp(in.goodsAvailable, 0, 255)) / 255.0;
    DemandBar& C = r.bar[1];
    C.Add("Shoppers", p.shoppers * (want - comm) / std::max(want, 20.0));
    C.Add("Spending", p.spending * std::clamp(in.spendIndex - 1.0, -1.0, 1.0));
    C.Add("Shop supply", -p.supply * supplyGap);
    C.Add("Taxes", in.tax[1]);
    const double low = (double)(in.seekers[0] + in.seekers[1]) + 0.5 * (double)in.seekers[2] + pendAdults * 0.6;
    const double high = 0.5 * (double)in.seekers[2] + (double)(in.seekers[3] + in.seekers[4]) + pendAdults * 0.4;
    DemandBar& O = r.bar[2];
    O.Add("Educated workers", p.workers * high / A);
    O.Add("Office vacancies", -p.workers * (double)(in.vacancies[2] + in.pendingJobs[2]) / A);
    O.Add("Taxes", in.tax[2]);
    const double ind = (double)(in.jobs[0] + in.pendingJobs[0]), wantGoods = comm * p.industryPerCommercial;
    DemandBar& I = r.bar[3];
    I.Add("Workers", p.workers * low / A);
    I.Add("Factory vacancies", -p.workers * (double)(in.vacancies[0] + in.pendingJobs[0]) / A);
    I.Add("Goods for shops", p.goods * (wantGoods - ind) / std::max(wantGoods, 20.0));
    I.Add("Goods shortfall", p.shortfall * supplyGap);
    I.Add("Taxes", in.tax[3]);
    return r;
}

// The bars move toward the clamped raw values over `minutes` of game time.
inline void SmoothDemand(double bars[kDemandCats], const DemandResult& r, const DemandParams& p, double minutes) {
    const double k = std::clamp(minutes / std::max(1.0, p.tauMinutes), 0.0, 1.0);
    for (int c = 0; c < kDemandCats; ++c) bars[c] += (std::clamp(r.bar[c].raw, -1.0, 1.0) - bars[c]) * k;
}

} // namespace city
} // namespace tbd
