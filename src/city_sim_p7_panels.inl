// ------------------------------------------------------------------- city sim Phase 7: Budget and Statistics panels, Districts overlay (included by city_sim_p7.inl)
// Panels are SimPanelSpecs (gui/sim_panels.h), drawn by both HUDs: touch first, every control a 44 pt slider row. A
// two-position slider is a switch. A control writes the model only when the player moved it (Knob), so the CS2 HUD's own
// Budget panel and these stay one setting.

namespace sim_p7 {
// A slider bound to a model value: Sync applies a move (apply returns the value the model took) and otherwise follows it.
struct Knob {
    float v = 0.0f, seen = -1e9f;
    template <class Apply>
    void Sync(float model, Apply&& apply) {
        if (seen > -1e8f && v != seen) model = apply(v);
        v = seen = model;
    }
};
std::string Money(int64_t cents, bool sign = false) {
    char b[48];
    const long long v = (long long)(cents / 100);
    std::snprintf(b, sizeof(b), sign ? "%+lld" : "%lld", v);
    return b;
}
// A toast at most every 2 real seconds per message (a held switch that cannot move would otherwise repeat it).
void ToastOnce(const char* text) {
    static std::string last;
    static uint64_t at = 0;
    const uint64_t now = tbd::platform::TickCountMs();
    if (last == text && now - at < 2000) return;
    last = text, at = now;
    Toast(text);
}

struct BudgetUi {
    Knob tax[4], edu[tbd::city::kEduLevels], budget[tbd::city::kServicesV2], fee[3], loan[3];
} bui;
void FillLoans(tbd::gui::SimPanelSpec& p); // city_sim_p7_panels2.inl
const char* const kCatName[4] = {"Residential", "Commercial", "Office", "Industrial"};
const char* const kEduName[tbd::city::kEduLevels] = {"Uneducated", "Elementary", "High school", "College", "University"};
const char* const kSvcName[tbd::city::kServicesV2] = {"Fire", "Police", "Healthcare", "Schools", "Parks", "Public transport",
                                                      "Deathcare", "Garbage", "Post", "Telecom", "High schools", "Universities"};

void FillBudget(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    using tbd::gui::Rgba;
    const GovIncome& inc = gov.last;
    p.sub = "Treasury " + Money(g_simEcon.treasury) + ", " + Money(g_simEcon.LastNet(), true) + " a day";
    p.accent = Rgba(52, 199, 89);
    p.Stat("Income yesterday", Money(g_simEcon.LastIncome()), 1);
    p.Stat("Expenses yesterday", Money(g_simBudget.LastExpenses() + g_simBudget.lastRepayment), -1);
    p.Stacked("Income by source", {{"Residential", (float)inc.cat[0], Rgba(52, 199, 89)}, {"Commercial", (float)inc.cat[1], Rgba(10, 132, 255)},
                                   {"Office", (float)inc.cat[2], Rgba(191, 90, 242)}, {"Industrial", (float)inc.cat[3], Rgba(255, 159, 10)},
                                   {"Fees and trade", (float)inc.Other(), Rgba(160, 160, 170)}});
    std::vector<std::pair<std::string, std::string>> in, out;
    for (int c = 0; c < 4; ++c) in.push_back({std::string(kCatName[c]) + " tax", Money(inc.cat[c])});
    in.push_back({"Electricity and water fees", Money(inc.fees)}), in.push_back({"Trade (export tariff)", Money(inc.trade)});
    p.List("Income", in, 6);
    for (int x = 0; x < kExpenses; ++x) out.push_back({ExpenseName(x), Money(g_simBudget.lastExpense[x])});
    out.push_back({"  of which policies", Money(inc.policyCost)}), out.push_back({"Loan repayment", Money(g_simBudget.lastRepayment)});
    out.push_back({"Construction", Money(g_simBudget.lastConstruction)});
    p.List("Expenses", out, 7);
    p.Spark("Treasury", gov.stats.Floats(kStatMoney), Money(g_simEcon.treasury));
    p.Caption("TAXES");
    bool taxMoved = false;
    for (int c = 0; c < 4; ++c) {
        bui.tax[c].Sync((float)g_simEcon.taxRate10[c] / 10.0f, [&](float v) { g_simEcon.taxRate10[c] = (int)std::lround(std::clamp(v, 0.0f, 30.0f) * 10.0f); taxMoved = true; return (float)g_simEcon.taxRate10[c] / 10.0f; });
        p.Slider(std::string(kCatName[c]) + " tax", &bui.tax[c].v, 0.0f, 30.0f, 0.5f, "%.1f %%");
    }
    p.Caption("RESIDENTIAL TAX BY EDUCATION (yesterday)");
    for (int e = 0; e < kEduLevels; ++e) {
        bui.edu[e].Sync((float)gov.rates.eduOffset10[e] / 10.0f, [&](float v) { gov.rates.eduOffset10[e] = (int)std::lround(std::clamp(v, -5.0f, 5.0f) * 10.0f); gov.Touch(); return (float)gov.rates.eduOffset10[e] / 10.0f; });
        char b[64];
        std::snprintf(b, sizeof(b), "%.1f %% (%+.1f), %s", gov.rates.ResRate10(g_simEcon.taxRate10[0], e) / 10.0, bui.edu[e].v, Money(inc.resByEdu[e]).c_str());
        p.Slider(kEduName[e], &bui.edu[e].v, -5.0f, 5.0f, 0.5f).value = b;
    }
    p.Caption("SERVICE BUDGETS (capacity and upkeep)");
    for (int k = 0; k < kServicesV2; ++k) {
        bui.budget[k].Sync((float)gov.rates.budgetPct[k], [&](float v) { gov.rates.budgetPct[k] = std::clamp((int)std::lround(v), 50, 150); gov.Touch(); return (float)gov.rates.budgetPct[k]; });
        p.Slider(kSvcName[k], &bui.budget[k].v, 50.0f, 150.0f, 5.0f, "%.0f %%");
    }
    p.Caption("FEES");
    int* fees[3] = {&gov.rates.powerFeePct, &gov.rates.waterFeePct, &gov.rates.transitFarePct};
    static const char* const kFee[3] = {"Electricity fee", "Water fee", "Transit fare"};
    for (int f = 0; f < 3; ++f) {
        bui.fee[f].Sync((float)*fees[f], [&](float v) { *fees[f] = std::clamp((int)std::lround(v), 0, 200); gov.Touch(); return (float)*fees[f]; });
        p.Slider(kFee[f], &bui.fee[f].v, 0.0f, 200.0f, 5.0f, "%.0f %%");
    }
    if (taxMoved) g_simHudPush = true;
    FillLoans(p);
}
} // namespace sim_p7

#include "city_sim_p7_panels2.inl" // loans, Statistics, Policies, Districts (and painting), Development, the overlay
