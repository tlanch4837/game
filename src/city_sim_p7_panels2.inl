// ------------------------------------------------------------------- city sim Phase 7: loans, Statistics and Policies panels (included by city_sim_p7_panels.inl)

namespace sim_p7 {
void FillLoans(tbd::gui::SimPanelSpec& p) {
    using namespace tbd::city;
    p.Caption("LOANS (switch on to borrow, off to repay)");
    int nt = 0;
    const LoanTier* t = LoanTiers(nt);
    for (int i = 0; i < std::min(nt, 3); ++i) {
        const Loan* l = nullptr;
        for (const Loan& x : g_simBudget.loans) if (x.tier == i) l = &x;
        bui.loan[i].Sync(l ? 1.0f : 0.0f, [&](float v) {
            if (v >= 0.5f && !l) {
                if (g_simBudget.TakeLoan(i, g_simEcon)) ToastOnce("LOAN TAKEN"), SimBudgetEvents();
            } else if (v < 0.5f && l) {
                for (size_t k = 0; k < g_simBudget.loans.size(); ++k)
                    if (g_simBudget.loans[k].tier == i) {
                        ToastOnce(g_simBudget.RepayLoan((int)k, g_simEcon) ? "LOAN REPAID" : "NOT ENOUGH MONEY TO REPAY THE LOAN");
                        break;
                    }
                SimBudgetEvents();
            }
            return g_simBudget.HasTier(i) ? 1.0f : 0.0f;
        });
        char b[96];
        l = nullptr;
        for (const Loan& x : g_simBudget.loans) if (x.tier == i) l = &x;
        if (l) std::snprintf(b, sizeof(b), "owes %s, %d days, %s a day interest", Money(l->owed).c_str(), l->daysLeft, Money(SimBudget::Interest(*l)).c_str());
        else std::snprintf(b, sizeof(b), "%s at %.1f %% over %d days", Money(t[i].amount).c_str(), t[i].ratePermille / 10.0, t[i].termDays);
        p.Slider(t[i].name, &bui.loan[i].v, 0.0f, 1.0f, 1.0f).value = b;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "gov tick %.3f ms avg, %.3f worst; frame %.3f ms", tickAvgMs, tickWorstMs, frameMs);
    p.Caption(b);
}

void FillStats(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.accent = tbd::gui::Rgba(100, 210, 255);
    const GovStats& s = gov.stats;
    p.sub = std::to_string(s.Days()) + " days of history (one sample a game day)";
    if (s.Days() < 2) p.Caption("The charts fill in as the days go by.");
    auto num = [&](int k, const char* unit = "") { return std::to_string((long long)s.Last(k)) + unit; };
    p.Spark("Population", s.Floats(kStatPopulation), num(kStatPopulation));
    p.Spark("Jobs", s.Floats(kStatJobs), num(kStatJobs));
    p.Spark("Workers", s.Floats(kStatWorkers), num(kStatWorkers));
    p.Spark("Money", s.Floats(kStatMoney), num(kStatMoney));
    p.Spark("Income a day", s.Floats(kStatIncome), num(kStatIncome));
    p.Spark("Expenses a day", s.Floats(kStatExpenses), num(kStatExpenses));
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f %%", s.Last(kStatHappiness) / 10.0);
    p.Spark("Happiness", s.Floats(kStatHappiness, 0.1f), b).tone = s.Last(kStatHappiness) >= 600 ? 1 : s.Last(kStatHappiness) < 450 ? -1 : 0;
    std::snprintf(b, sizeof(b), "%.1f %% slow", s.Last(kStatTraffic) / 10.0);
    p.Spark("Traffic", s.Floats(kStatTraffic, 0.1f), b).tone = s.Last(kStatTraffic) > 200 ? -1 : 0;
    std::snprintf(b, sizeof(b), "%.1f", s.Last(kStatPollution) / 10.0);
    p.Spark("Air pollution", s.Floats(kStatPollution, 0.1f), b).tone = s.Last(kStatPollution) > 600 ? -1 : 0;
}

// Policies: the scope slider picks the whole city or one district; the switches below are that scope's policies.
struct PolicyUi {
    Knob scope;
    Knob on[32];
    int scopeFor = -1;
} pui;
void FillPolicies(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.accent = tbd::gui::Rgba(255, 159, 10);
    const int nd = gov.districts.Count();
    static int scope = 0;
    pui.scope.Sync((float)scope, [&](float v) { scope = std::clamp((int)std::lround(v), 0, nd); return (float)scope; });
    scope = std::clamp(scope, 0, nd);
    if (pui.scopeFor != scope) { pui.scopeFor = scope; for (Knob& k : pui.on) k = Knob(); } // the switches follow the new scope
    const GovDistrict* d = gov.districts.Get(scope);
    p.sub = d ? "District: " + d->name : "Whole city";
    if (nd > 0) p.Slider("Scope (0 = whole city)", &pui.scope.v, 0.0f, (float)nd, 1.0f).value = d ? d->name : "Whole city";
    else p.Caption("Paint districts in the Districts panel for local policies.");
    int n = 0;
    const PolicyDef* pd = Policies(n);
    int nodes = 0;
    const GovNodeDef* nd2 = GovNodes(nodes);
    for (int i = 0; i < n && i < 32; ++i) {
        const bool fits = (pd[i].scope & (d ? kScopeDistrict : kScopeCity)) != 0;
        if (!fits) continue;
        const bool active = d ? (d->policies & (1u << i)) != 0 : (gov.cityPolicies & (1u << i)) != 0;
        if (!gov.PolicyOpen(i) && !active) {
            std::string need = "development tree";
            for (int k = 0; k < nodes; ++k) if (pd[i].needs & (1u << k)) need = nd2[k].name;
            p.Stat(pd[i].name, "needs " + need, 0);
            continue;
        }
        pui.on[i].Sync(active ? 1.0f : 0.0f, [&](float v) {
            const bool want = v >= 0.5f;
            if (d) gov.SetDistrictPolicy(scope, i, want);
            else gov.SetCityPolicy(i, want);
            const GovDistrict* dd = gov.districts.Get(scope);
            return (dd ? (dd->policies >> i) & 1u : (gov.cityPolicies >> i) & 1u) ? 1.0f : 0.0f;
        });
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s", pd[i].what, pd[i].costPerResDay ? (", " + std::to_string(pd[i].costPerResDay) + " c / resident").c_str() : "");
        p.Slider(pd[i].name, &pui.on[i].v, 0.0f, 1.0f, 1.0f).value = b;
    }
    p.Stat("Policies cost yesterday", Money(gov.last.policyCost), gov.last.policyCost > 0 ? -1 : 0);
}
} // namespace sim_p7

#include "city_sim_p7_panels3.inl" // Districts, Development, the overlay and the registration
