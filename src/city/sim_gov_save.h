// Phase 7's .tbdsim chunks (CS2 sim roadmap 3.4): BUDG (treasury, rates, sliders, loans, ledger, today's tax sums,
// statistics), PLCY (city policies), DIST (districts and their cells), MILE (milestones and the progression). Header only,
// headless (tests\sim_gov_test). Field by field, little-endian; the same state always writes the same bytes.
// Old saves (no chunks) keep what the M record restored (its B1 budget field is still read by city_sim.inl).
#pragma once

#include <cstdint>
#include <string>

#include "sim_budget.h"
#include "sim_gov.h"
#include "sim_milestones.h"
#include "sim_save.h"

namespace tbd {
namespace city {

constexpr uint16_t kGovBudgVersion = 1, kGovPlcyVersion = 1, kGovDistVersion = 1, kGovMileVersion = 1;

inline void WriteGovBudg(ByteWriter& w, const GovState& g, const SimEconomy& e, const SimBudget& b) {
    w.I64(e.treasury), w.I64(e.days), w.I64(e.lastUpkeep), w.I64(e.lastOther);
    for (int c = 0; c < SimEconomy::kCats; ++c) w.I32(e.taxRate10[c]), w.I64(e.lastIncome[c]);
    w.Str(b.SaveFields());
    for (int x = 0; x < kExpenses; ++x) w.I64(b.lastExpense[x]);
    w.I64(b.lastRepayment), w.I64(b.constructionToday), w.I64(b.lastConstruction);
    for (int v : g.rates.eduOffset10) w.I32(v);
    for (int v : g.rates.budgetPct) w.I32(v);
    w.I32(g.rates.powerFeePct), w.I32(g.rates.waterFeePct), w.I32(g.rates.transitFarePct);
    for (int64_t v : g.acc.resNum) w.I64(v);
    for (int64_t v : g.acc.jobNum) w.I64(v);
    w.I64(g.acc.feeNum), w.U32(g.acc.sliceMask), w.I64(g.happy.sum), w.I64(g.happy.count);
    for (int64_t v : g.last.resByEdu) w.I64(v);
    for (int64_t v : g.last.cat) w.I64(v);
    w.I64(g.last.resStat), w.I64(g.last.fees), w.I64(g.last.trade), w.I64(g.last.policyCost);
    for (int s = 0; s < kStatSeries; ++s) w.PutVec(g.stats.s[s]);
}
inline bool ReadGovBudg(ByteReader& r, GovState& g, SimEconomy& e, SimBudget& b) {
    e.treasury = r.I64(), e.days = r.I64(), e.lastUpkeep = r.I64(), e.lastOther = r.I64();
    for (int c = 0; c < SimEconomy::kCats; ++c) e.taxRate10[c] = std::clamp(r.I32(), 0, 300), e.lastIncome[c] = r.I64();
    const std::string fields = r.Str();
    if (!r.ok || !b.LoadFields(fields)) return false;
    for (int x = 0; x < kExpenses; ++x) b.lastExpense[x] = r.I64();
    b.lastRepayment = r.I64(), b.constructionToday = r.I64(), b.lastConstruction = r.I64();
    for (int& v : g.rates.eduOffset10) v = std::clamp(r.I32(), -50, 50);
    for (int& v : g.rates.budgetPct) v = std::clamp(r.I32(), 50, 150);
    g.rates.powerFeePct = std::clamp(r.I32(), 0, 200), g.rates.waterFeePct = std::clamp(r.I32(), 0, 200);
    g.rates.transitFarePct = std::clamp(r.I32(), 0, 200);
    for (int64_t& v : g.acc.resNum) v = r.I64();
    for (int64_t& v : g.acc.jobNum) v = r.I64();
    g.acc.feeNum = r.I64(), g.acc.sliceMask = r.U32(), g.happy.sum = r.I64(), g.happy.count = r.I64();
    for (int64_t& v : g.last.resByEdu) v = r.I64();
    for (int64_t& v : g.last.cat) v = r.I64();
    g.last.resStat = r.I64(), g.last.fees = r.I64(), g.last.trade = r.I64(), g.last.policyCost = r.I64();
    for (int s = 0; s < kStatSeries; ++s) r.GetVec(g.stats.s[s]);
    for (int s = 1; s < kStatSeries; ++s)
        if (g.stats.s[s].size() != g.stats.s[0].size()) return false;
    ++g.rates.version;
    return r.ok;
}

inline void WriteGovPlcy(ByteWriter& w, const GovState& g) { w.U32(g.cityPolicies); }
inline bool ReadGovPlcy(ByteReader& r, GovState& g) {
    g.cityPolicies = r.U32() & ((1u << PolicyCount()) - 1u);
    ++g.rates.version;
    return r.ok;
}

inline void WriteGovDist(ByteWriter& w, const GovState& g) {
    w.U32((uint32_t)g.districts.list.size());
    for (const GovDistrict& d : g.districts.list) w.Str(d.name), w.U32(d.policies);
    const auto cells = g.districts.Sorted();
    w.U32((uint32_t)cells.size());
    for (const auto& c : cells) w.U64(c.first), w.U8(c.second);
}
inline bool ReadGovDist(ByteReader& r, GovState& g) {
    g.districts.Clear();
    const uint32_t n = r.U32();
    if (!r.ok || n > (uint32_t)GovDistricts::kMax) return false;
    for (uint32_t i = 0; i < n; ++i) {
        GovDistrict d;
        d.name = r.Str(), d.policies = r.U32() & ((1u << PolicyCount()) - 1u);
        g.districts.list.push_back(d);
    }
    const uint32_t m = r.U32();
    if (!r.ok || (uint64_t)m * 9 > r.Remaining()) return false;
    for (uint32_t i = 0; i < m; ++i) {
        const uint64_t k = r.U64();
        const uint8_t d = r.U8();
        if (d >= 1 && d <= n) g.districts.cells[k] = d;
    }
    ++g.districts.version;
    return r.ok;
}

inline void WriteGovMile(ByteWriter& w, const GovState& g, const SimMilestones& m) {
    w.I32(m.reached), w.U32(m.unlocked);
    const GovProgress& p = g.progress;
    w.I64(p.xp), w.I32(p.level), w.I32(p.points), w.U32(p.bought), w.I64(p.bestPop), w.I32(p.milestonesPaid), w.I64(p.bestBuildings);
}
inline bool ReadGovMile(ByteReader& r, GovState& g, SimMilestones& m) {
    int n = 0;
    Milestones(n);
    m.reached = std::clamp(r.I32(), 0, n), m.unlocked = r.U32();
    GovProgress& p = g.progress;
    p.xp = r.I64(), p.level = r.I32(), p.points = r.I32(), p.bought = r.U32(), p.bestPop = r.I64(), p.milestonesPaid = r.I32(), p.bestBuildings = r.I64();
    ++g.rates.version;
    return r.ok && p.level >= 0 && p.points >= 0;
}

} // namespace city
} // namespace tbd
