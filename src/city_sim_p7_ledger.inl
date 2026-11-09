// ------------------------------------------------------------------- city sim Phase 7: the daily ledger's hooks (included by city_sim_p7.inl)
// city_sim_budget.inl's "economy" system calls these once a game day; the upkeep hooks run inside its SimDayUpkeep.

namespace sim_p7 {
// The Budget panel's per-service sliders and the CS2 HUD's six funding sliders (g_simFunding) are one setting: whichever
// moved last wins, the other follows.
float fundMirror[tbd::city::kServices] = {1, 1, 1, 1, 1, 1};
void SyncFunding() {
    bool hudMoved = false, panelMoved = false;
    for (int k = 0; k < tbd::city::kServices; ++k) {
        hudMoved |= g_simFunding[k] != fundMirror[k];
        panelMoved |= std::lround(fundMirror[k] * 100.0f) != gov.rates.budgetPct[k];
    }
    if (hudMoved) {
        for (int k = 0; k < tbd::city::kServices; ++k) gov.rates.budgetPct[k] = std::clamp((int)std::lround(g_simFunding[k] * 100.0f), 50, 150);
        gov.Touch();
    } else if (panelMoved) {
        for (int k = 0; k < tbd::city::kServices; ++k) g_simFunding[k] = (float)gov.rates.budgetPct[k] / 100.0f;
        g_simHudPush = true;
    }
    for (int k = 0; k < tbd::city::kServices; ++k) fundMirror[k] = g_simFunding[k];
}

int64_t BuildingCount() {
    using namespace tbd::city;
    int64_t n = 0;
    for (const Placed& b : g_transport.items) n += b.alive && b.state == BState::Done;
    for (const UtilBuilding& b : g_util.items) n += b.alive;
    return n;
}
// Daily statistics: traffic as the share of the flowing roads that are slow, pollution as the occupied cells' mean air.
void Sample(int64_t (&d)[tbd::city::kStatSeries]) {
    using namespace tbd::city;
    d[kStatPopulation] = (int64_t)g_simTotals.population, d[kStatJobs] = (int64_t)g_simTotals.jobs, d[kStatWorkers] = (int64_t)g_simTotals.working;
    d[kStatMoney] = g_simEcon.treasury / 100, d[kStatIncome] = g_simEcon.LastIncome() / 100;
    d[kStatExpenses] = (g_simBudget.LastExpenses() + g_simBudget.lastRepayment) / 100;
    d[kStatHappiness] = gov.happy.Permille();
    int64_t flowing = 0, slow = 0;
    for (const SegFlow& f : g_segFlow) flowing += f.flow > 1.0f, slow += f.cls >= 2;
    d[kStatTraffic] = flowing ? slow * 1000 / flowing : 0;
    int64_t air = 0, cells = 0;
    for (size_t c = 0; c < g_citySim.sim.size() && c < g_cellFactors.cells.size(); ++c)
        if (g_citySim.sim[c].Population() > 0) air += g_cellFactors.cells[c].air, ++cells;
    d[kStatPollution] = cells ? air * 10 / cells : 0;
}
} // namespace sim_p7

void SimGovBeforeLedger() {
    using namespace sim_p7;
    using namespace tbd::city;
    if (!on) return;
    GovTaxInput in = Input();
    if (gov.acc.Slices() == 0) GovTaxFull(in, gov.acc); // a load right at midnight, or the first day: the full recompute
    const double perJob[3] = {g_simTune.taxCommercialPerJobDay, g_simTune.taxOfficePerJobDay, g_simTune.taxIndustrialPerJobDay};
    GovIncome inc;
    GovTaxFinish(gov.acc, in, g_simEcon.taxRate10, perJob, g_simTune.taxResidentialPerResidentDay, std::max(1, g_simCalendar.lifeDaysPerMonth), inc);
    if (!in.w) { // no records (P1 off): residential from the cells, as before
        int64_t old[SimEconomy::kCats];
        g_simEcon.DayIncome(g_citySim, g_simTune, old);
        inc.cat[0] = old[0], inc.resStat = old[0];
    }
    if (g_simPhaseTune.goods.enabled != 0.0) {
        const double tariff = T().tradeTariffPermille * (gov.progress.Has(1u << 4) ? 1.5 : 1.0); // Trade Agreements
        inc.trade = (int64_t)std::llround((double)std::max<int64_t>(0, sim_p6::world.last.exportIncome) * tariff / 1000.0 * 100.0);
        // the companies' profit tax: yesterday's positive profit (whole currency) at its category's rate x profitTaxShare
        static const int kCatOf[4] = {3, 3, 1, 2}; // CompanyKind Raw, Processor, Shop, Office -> SimEconomy cat
        double profit[4] = {0, 0, 0, 0};
        sim_p6::world.cos.ForEach([&](uint32_t, const CompanyRec& c) {
            if (c.closedDays <= 0 && c.profitLast > 0 && c.kind >= 0 && c.kind < 4) profit[kCatOf[c.kind]] += c.profitLast;
        });
        for (int k = 1; k < 4; ++k)
            inc.cat[k] += (int64_t)std::llround(profit[k] * g_simEcon.taxRate10[k] / 1000.0 * std::clamp(T().profitTaxShare, 0.0, 1.0) * 100.0);
    }
    inc.policyCost = gov.last.policyCost; // SimDayUpkeep's (it ran just before, inside this ledger)
    gov.last = inc;
    g_simEcon.PresetIncome(inc.cat, inc.Other());
}

void SimGovAfterLedger() {
    using namespace sim_p7;
    using namespace tbd::city;
    if (!on) return;
    int64_t d[kStatSeries];
    Sample(d);
    gov.stats.Push(d);
    const int64_t pop = (int64_t)g_simTotals.population, bld = BuildingCount();
    int levels = gov.progress.OnPopulation(pop, T().xpPerResident);
    if (bld > gov.progress.bestBuildings) levels += gov.progress.AddXp((bld - gov.progress.bestBuildings) * (int64_t)T().xpPerBuilding), gov.progress.bestBuildings = bld;
    if (d[kStatHappiness] > 600) levels += gov.progress.AddXp((int64_t)std::llround((double)pop * T().xpHappyPerDay));
    if (levels > 0 && g_clock.minutes >= g_simMilestoneQuietUntil) {
        Toast("CITY LEVEL " + std::to_string(gov.progress.level) + ": +" + std::to_string(levels) + " DEVELOPMENT POINT" + (levels > 1 ? "S" : ""));
        SimNote("City level " + std::to_string(gov.progress.level) + " (development points to spend)");
    }
    CheckMods();
}

double SimGovUpkeepMul() { return sim_p7::on ? sim_p7::gov.ModsFor(0).serviceUpkeep : 1.0; }
uint64_t SimGovModsVersion() { return sim_p7::on ? sim_p7::gov.modsVersion : 0; }
// P4: a service's capacity and efficiency multiplier (city policies x the Budget panel's slider); -1 while P7 is off.
float SimGovServiceCapacity(int slot) {
    using namespace sim_p7;
    if (!on) return -1.0f;
    static uint64_t key = ~0ull;
    static tbd::city::PolicyMods m;
    const uint64_t k = gov.ComposeKey(0, 0);
    if (k != key) key = k, m = gov.ModsFor(0);
    return std::clamp(m.ServiceCapacity(slot), 0.25f, 2.0f);
}
int64_t SimGovPolicyCostDay() {
    using namespace sim_p7;
    if (!on) return 0;
    std::vector<uint32_t> hoodPop;
    for (const tbd::city::Hood& h : herds_host::hoods.hoods) hoodPop.push_back(h.Population());
    if (hoodPop.size() != gov.hoodDistrict.size()) hoodPop.resize(gov.hoodDistrict.size(), 0);
    gov.last.policyCost = gov.PolicyCostPerDay(hoodPop, (int64_t)g_simTotals.population);
    return gov.last.policyCost;
}
