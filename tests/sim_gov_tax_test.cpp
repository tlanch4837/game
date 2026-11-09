// Headless checks for Phase 7's taxes and ledger (src/city/sim_gov_tax.h, sim_gov.h, sim_budget.h):
//   residential tax follows the households' wages (a hand count over the records agrees), the rate and the per-education
//   offsets scale only what they should; the 24 hourly groups sum to the full recompute; a part day scales up; the budget
//   slider moves PolicyMods' service capacity; loan interest; the economy takes the preset income.
// Build and run: tests\sim_gov_tax_test.bat
#include <cmath>
#include <cstdlib>

#include "sim_life_town.h"

#include "city/sim_budget.h"
#include "city/sim_gov.h"

using namespace lifetest;

static GovTaxInput Input(const Town& t, const GovRates& r, int rate10 = 100) {
    GovTaxInput in;
    in.w = &t.sim.w, in.rates = &r, in.mainRate10 = rate10;
    return in;
}
static int64_t ResTotal(const GovTaxAccum& a, const GovTaxInput& in) {
    GovIncome inc;
    const int rates[4] = {in.mainRate10, 100, 100, 100};
    const double perJob[3] = {60, 80, 50};
    GovTaxFinish(a, in, rates, perJob, 40, 1, inc);
    return inc.cat[0];
}

static void TestResidential() {
    Town t;
    t.sim.w.citySeed = 3;
    t.sim.w.tune.instantFill = 1;
    t.Build(8, 40, 40);
    int64_t minute = 0;
    t.RunDays(minute, 3);
    const LifeWorld& w = t.sim.w;
    CHECK(w.Working() > 100);
    GovRates r;
    GovTaxAccum full;
    GovTaxFull(Input(t, r), full);
    // the hand count: every worker's wage x 10 % x the taxable share, in cents
    double hand = 0;
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci)
        if (w.cit.At(ci).Valid() && w.cit.work[ci]) hand += w.tune.Wage(w.cit.jobLevel[ci]) * 0.10 * 0.005 * 100.0;
    const int64_t res = ResTotal(full, Input(t, r));
    std::printf("residential: %u workers, %lld cents a day (hand count %.0f)\n", w.Working(), (long long)res, hand);
    CHECK(res > 0 && std::fabs((double)res - hand) <= hand * 0.01 + 5);
    // double the rate: double the tax
    GovTaxAccum twice;
    GovTaxFull(Input(t, r, 200), twice);
    CHECK(std::llabs(ResTotal(twice, Input(t, r, 200)) - 2 * res) <= 5);
    // an offset on one education level moves only that level
    GovRates r2 = r;
    r2.eduOffset10[2] = 50;
    GovTaxAccum off;
    GovTaxFull(Input(t, r2), off);
    for (int e = 0; e < kEduLevels; ++e) CHECK(e == 2 ? (full.resNum[e] == 0 || off.resNum[e] > full.resNum[e]) : off.resNum[e] == full.resNum[e]);
    // the hourly groups sum to the full recompute (the deferred check), in any order
    GovTaxAccum sliced;
    for (int i = kGovSlices - 1; i >= 0; --i) GovTaxSlice(Input(t, r), i, sliced);
    CHECK(sliced.Slices() == kGovSlices);
    for (int e = 0; e < kEduLevels; ++e) CHECK(sliced.resNum[e] == full.resNum[e]);
    GovTaxSlice(Input(t, r), 3, sliced); // a group twice in one day counts once
    CHECK(sliced.resNum[1] == full.resNum[1]);
    // half a day (a load at noon) is scaled to the whole day
    GovTaxAccum half;
    for (int i = 0; i < 12; ++i) GovTaxSlice(Input(t, r), i, half);
    CHECK(std::llabs(ResTotal(half, Input(t, r)) - res) < res / 4 + 10);
    // the hoods' residential tax multiplier (district policies) scales what their households pay
    std::vector<PolicyMods> mods(32);
    for (PolicyMods& m : mods) m.taxResidential = 0.5f;
    GovTaxInput in = Input(t, r);
    in.hoodMods = &mods;
    GovTaxAccum halfRate;
    GovTaxFull(in, halfRate);
    CHECK(std::llabs(ResTotal(halfRate, in) * 2 - res) <= 10);
}

static void TestBudgetAndLoans() {
    GovState g;
    g.rates.budgetPct[kSvcPolice] = 150, g.Touch();
    std::vector<int> cellHood = {0, 0, 1};
    CHECK(g.Compose(cellHood, 2, 1, [](int c) { return (uint64_t)c; }));
    CHECK(std::fabs(g.mods[1].ServiceCapacity(kSvcPolice) - 1.5f) < 1e-6f && g.mods[1].ServiceCapacity(kSvcFire) == 1.0f);
    CHECK(!g.Compose(cellHood, 2, 1, [](int c) { return (uint64_t)c; })); // nothing changed: no work
    g.rates.budgetPct[kSvcPolice] = 50, g.Touch();
    CHECK(g.Compose(cellHood, 2, 1, [](int c) { return (uint64_t)c; }) && std::fabs(g.mods[0].ServiceCapacity(kSvcPolice) - 0.5f) < 1e-6f);
    // loans: the money at once, interest on what is owed, the principal in instalments
    SimTuning t;
    SimEconomy e;
    e.Reset(t);
    SimBudget b;
    const int64_t before = e.treasury;
    CHECK(b.TakeLoan(0, e) && e.treasury == before + 50000 * 100LL && !b.TakeLoan(0, e));
    const int64_t owed = b.loans[0].owed;
    CitySim s;
    const int64_t income[4] = {1000, 0, 0, 0};
    e.PresetIncome(income, 250);
    const int64_t t0 = e.treasury;
    b.Day(e, s, t, DayUpkeep());
    CHECK(b.lastExpense[kExpInterest] == owed * 50 / (1000LL * 360));
    CHECK(e.lastIncome[0] == 1000 && e.lastOther == 250 && e.LastIncome() == 1250);
    CHECK(e.treasury == t0 + 1250 - b.lastExpense[kExpInterest] - b.lastRepayment);
    CHECK(b.loans[0].owed == owed - b.lastRepayment && b.lastRepayment == (owed + 359) / 360);
    b.Day(e, s, t, DayUpkeep()); // no preset: the old formula (an empty city earns nothing)
    CHECK(e.lastOther == 0 && e.lastIncome[0] == 0);
}

int main() {
    TestResidential();
    TestBudgetAndLoans();
    std::printf(failures ? "sim_gov_tax_test: %d FAILED\n" : "sim_gov_tax_test: all passed\n", failures);
    return failures ? 1 : 0;
}
