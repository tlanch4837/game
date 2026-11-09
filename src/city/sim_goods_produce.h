// Phase 6: what a company does in an hour, and its day end (profit, bankruptcy, reopening). Header only, headless.
// Pure functions over one CompanyRec; the world (sim_goods_world.h) calls them in company index order.
#pragma once

#include <algorithm>
#include <cstdint>

#include "sim_goods_companies.h"

namespace tbd {
namespace city {

// What an hour of work moved, for the world's ledger.
struct HourResult {
    Good made = Good::None;
    int32_t madeUnits = 0;
    Good used[kMaxInputs] = {Good::None, Good::None, Good::None};
    int32_t usedUnits = 0;          // of each used input
    int32_t sold[2] = {0, 0};       // shop: food, goods (consumed by the residents)
    int32_t revenue = 0;
};

// The input stock a company aims for, per input (units).
inline int32_t InputTarget(const CompanyRec& c, const GoodsParams& p, int inputIndex) {
    const int64_t rate = CompanyRateMilli(c, p);
    if (c.kind == (int)CompanyKind::Shop) return (int32_t)(rate * kRetailShare[inputIndex] / 100 * p.stockHours / 1000);
    return (int32_t)(rate * p.stockHours / 1000); // a processor uses one unit of each input per `inputs` units made
}
inline int32_t OutputCap(const CompanyRec& c, const GoodsParams& p) {
    return std::max<int32_t>(p.truckUnits, (int32_t)((int64_t)CompanyRateMilli(c, p) * p.outCapHours / 1000));
}

inline HourResult CompanyHour(CompanyRec& c, const GoodsParams& p) {
    HourResult r;
    if (c.closedDays > 0) return r;
    const int32_t rate = CompanyRateMilli(c, p);
    switch ((CompanyKind)c.kind) {
    case CompanyKind::Raw: {
        if (c.rawGood < 0) break;
        const int32_t room = std::max(0, OutputCap(c, p) - c.stock[(size_t)c.rawGood]);
        c.frac = std::min(c.frac + rate, (room + 1) * 1000);
        const int32_t n = std::min(room, c.frac / 1000);
        c.frac -= n * 1000;
        c.stock[(size_t)c.rawGood] += n;
        r.made = (Good)c.rawGood, r.madeUnits = n;
        break;
    }
    case CompanyKind::Processor: {
        const Recipe& rc = RecipeOf(c.recipe);
        const size_t out = (size_t)rc.out;
        int32_t batches = (c.frac + rate) / (1000 * rc.inputs);
        batches = std::min(batches, std::max(0, OutputCap(c, p) - c.stock[out]) / rc.inputs);
        for (int i = 0; i < rc.inputs; ++i) batches = std::min(batches, c.stock[(size_t)rc.in[i]]);
        c.frac = std::min(c.frac + rate - batches * 1000 * rc.inputs, 1000 * rc.inputs); // idle capacity is not banked
        c.frac = std::max(c.frac, 0);
        for (int i = 0; i < rc.inputs; ++i) c.stock[(size_t)rc.in[i]] -= batches, r.used[i] = rc.in[i];
        c.stock[out] += batches * rc.inputs;
        r.usedUnits = batches, r.made = rc.out, r.madeUnits = batches * rc.inputs;
        break;
    }
    case CompanyKind::Shop: {
        const int32_t can = std::min(rate, c.demandMilli); // staff and customers both limit the sales
        int32_t* fr[2] = {&c.frac, &c.frac2};
        for (int k = 0; k < 2; ++k) {
            const size_t g = (size_t)kRetailGoods[k];
            *fr[k] = std::min(*fr[k] + can * kRetailShare[k] / 100, 4000);
            const int32_t n = std::min(c.stock[g], *fr[k] / 1000);
            *fr[k] -= n * 1000, c.stock[g] -= n, r.sold[k] = n;
            r.revenue += n * GoodPrice((Good)g) * p.retailMarkupPercent / 100;
        }
        c.soldToday += r.sold[0] + r.sold[1];
        break;
    }
    case CompanyKind::Office: r.revenue = c.workers * p.officeValuePerDay / 24; break;
    }
    c.madeToday += r.madeUnits;
    c.profitToday += r.revenue;
    return r;
}

// Day end. Returns 1 when the company went bankrupt today, 2 when it reopened, 0 otherwise. A bankrupt company's
// stock is written off (lost[] for the ledger) and its lot waits reopenDays for a new owner.
inline int CompanyDayEnd(CompanyRec& c, const GoodsParams& p, std::array<int64_t, kGoodCount>& lost) {
    c.madeLast = c.madeToday, c.soldLast = c.soldToday, c.madeToday = c.soldToday = 0;
    if (c.closedDays > 0) {
        c.profitLast = c.profitToday = 0;
        if (--c.closedDays > 0) return 0;
        c.cash = p.startCash, c.lossDays = 0, c.frac = c.frac2 = 0;
        return 2;
    }
    c.profitToday -= c.workers * p.wagePerDay + CompanyUpkeep(c);
    c.profitLast = c.profitToday;
    c.cash = (int32_t)std::clamp<int64_t>((int64_t)c.cash + c.profitToday, -1000000000, 1000000000);
    c.profitToday = 0;
    c.lossDays = c.profitLast < 0 && c.cash < 0 ? c.lossDays + 1 : 0;
    if (c.lossDays < p.bankruptDays) return 0;
    for (int g = 0; g < kGoodCount; ++g) lost[(size_t)g] += c.stock[(size_t)g], c.stock[(size_t)g] = 0;
    c.closedDays = std::max(1, p.reopenDays), c.lossDays = 0, c.flags = 0;
    return 1;
}

} // namespace city
} // namespace tbd
