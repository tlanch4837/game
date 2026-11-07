// The city HUD's Budget panel (sim gameplay pass, phase B), opened by the Economy button or the money cell. Platform free.
//   Taxes      one slider per zone category, 0 .. 30 % (higher rates lower that category's demand)
//   Funding    one slider per service, 50 .. 150 % (coverage radius and upkeep)
//   Ledger     income per tax, expenses per item (services, utility plants, roads, loan interest), loan repayments, net
//   Trend      the treasury over the last 60 game days as a sparkline (red below zero)
//   Loans      three tiers: take one (one of each at most), or repay what is left early
// The host (city_sim.inl SimHudFill / SimHudRead) fills BudgetView and acts on its requests after the frame.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "gui.h"

namespace tbd {
namespace gui {

struct BudgetView {
    // in
    long long expense[4] = {};   // whole currency per day: services, utility plants, roads, loan interest
    long long repayment = 0;     // loan principal per day
    long long history[60] = {};  // treasury, oldest first
    int historyCount = 0;
    bool bankrupt = false;
    struct LoanRow {
        const char* name = "";
        long long amount = 0;    // whole currency
        float ratePct = 0;
        int termDays = 0;
        bool active = false;
        long long owed = 0;
        int daysLeft = 0;
    } loans[3];
    int loanCount = 0;
    // out (one frame): a loan tier to take, a tier to repay; -1 none
    int takeLoan = -1, repayLoan = -1;
};

// Draws the treasury trend into r.
inline void BudgetSparkline(DrawList& dl, const Rect& r, const BudgetView& v, const Theme& th) {
    dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
    if (v.historyCount < 2) return;
    long long lo = v.history[0], hi = v.history[0];
    for (int i = 0; i < v.historyCount; ++i) lo = std::min(lo, v.history[i]), hi = std::max(hi, v.history[i]);
    const double span = std::max<double>(1.0, (double)(hi - lo));
    const float pad = 4.0f, w = r.w - 2 * pad, h = r.h - 2 * pad;
    auto Y = [&](long long val) { return r.y + pad + h * (float)(1.0 - (double)(val - lo) / span); };
    if (lo < 0 && hi > 0) dl.Line(r.x + pad, Y(0), r.x + r.w - pad, Y(0), 1.0f, Rgba(150, 156, 165, 120)); // the zero line
    for (int i = 1; i < v.historyCount; ++i) {
        const float x0 = r.x + pad + w * (float)(i - 1) / (float)(v.historyCount - 1), x1 = r.x + pad + w * (float)i / (float)(v.historyCount - 1);
        const Color c = v.history[i] < 0 ? Rgba(235, 90, 70) : Rgba(90, 200, 120);
        dl.Line(x0, Y(v.history[i - 1]), x1, Y(v.history[i]), std::max(1.5f, th.iconStroke), c);
    }
}

// Hud: CityHud (city_hud.h): tax[4], funding[6], incomeDay[4], upkeepDay, budget, bv.
template <class Hud>
inline void DrawBudgetPanel(Context& g, Hud& h) {
    const Theme& th = g.Style();
    BudgetView& v = h.bv;
    v.takeLoan = v.repayLoan = -1;
    g.BeginPanel("Budget", Anchor::TopCenter, 0.0f, 40.0f, 680.0f, 0.0f, 12.0f); // from the top: it is tall
    long long income = 0, expense = 0;
    for (long long x : h.incomeDay) income += x;
    for (long long x : v.expense) expense += x;
    const long long net = income - expense - v.repayment;
    g.Text(TextStyle::Bright, "Income %lld / day   Expenses %lld / day   Net %+lld", income, expense + v.repayment, net);
    if (v.bankrupt) g.Text(TextStyle::Bright, "WARNING: the treasury is empty. Raise taxes, cut funding or take a loan.");
    g.Text(TextStyle::Muted, "Taxes: residential %lld, commercial %lld, office %lld, industrial %lld", h.incomeDay[0], h.incomeDay[1], h.incomeDay[2], h.incomeDay[3]);
    g.Text(TextStyle::Muted, "Services %lld, utility plants %lld, roads %lld, interest %lld, repayment %lld", v.expense[0], v.expense[1], v.expense[2],
           v.expense[3], v.repayment);
    g.Text(TextStyle::Muted, "Balance, last %d days", v.historyCount);
    BudgetSparkline(g.Draw(), g.Reserve(std::round(40.0f * g.Scale()), 0.0f), v, th);
    g.Spacing(4.0f);
    g.Text(TextStyle::Muted, "Tax rates (higher rates lower demand)");
    static const char* const kTax[] = {"Residential", "Commercial", "Office", "Industrial"};
    for (int c = 0; c < 4; ++c) {
        if (c == 0) g.BeginRow(4);
        g.SliderFloat(kTax[c], &h.tax[c], 0.0f, 30.0f, "%.0f %%");
    }
    g.EndRow();
    g.Spacing(4.0f);
    g.Text(TextStyle::Muted, "Service funding (scales coverage radius and upkeep)");
    static const char* const kSvc[] = {"Fire & Rescue", "Police", "Healthcare", "Education", "Parks", "Transit"};
    for (int k = 0; k < 6; ++k) {
        if (k % 3 == 0) g.BeginRow(3);
        g.SliderFloat(kSvc[k], &h.funding[k], 50.0f, 150.0f, "%.0f %%");
    }
    g.EndRow();
    g.Spacing(4.0f);
    g.Text(TextStyle::Muted, "Loans (one of each; repaid daily with interest)");
    for (int i = 0; i < v.loanCount; ++i) { // the terms, or what is still owed
        const BudgetView::LoanRow& l = v.loans[i];
        if (l.active) g.Text(TextStyle::Small, "%s: %lld owed, %d days left", l.name, l.owed, l.daysLeft);
        else g.Text(TextStyle::Small, "%s: %lld at %.1f %% a year over %d days", l.name, l.amount, l.ratePct, l.termDays);
    }
    g.BeginRow(v.loanCount + 1);
    for (int i = 0; i < v.loanCount; ++i) {
        const BudgetView::LoanRow& l = v.loans[i];
        char label[48];
        std::snprintf(label, sizeof(label), "%s %lldk", l.active ? "Repay" : "Take", (l.active ? l.owed : l.amount) / 1000);
        if (g.Button(label)) (l.active ? v.repayLoan : v.takeLoan) = i;
    }
    if (g.Button("Close", Icon::Close)) h.budget = false;
    g.EndRow();
    g.EndPanel();
}

} // namespace gui
} // namespace tbd
