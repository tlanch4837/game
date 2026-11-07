// The city sim's budget (sim gameplay pass, phase B): on top of SimEconomy (sim_economy.h: treasury, tax rates, income),
// itemised expenses, loans, a balance history and the bankruptcy warning. Header only, headless
// (tests\sim_economy_test.cpp). Whole cents, integer arithmetic: the same city gives the same ledger.
//
//   Expenses  per day: service upkeep (fire, police, health, school, transit buildings, scaled by funding), utility plant
//             upkeep, road upkeep (per km of road a day), loan interest. Loan principal repayments are charged too but
//             are not an expense (they show as "repayment").
//   Loans     three tiers (amount, yearly interest, term in days). A loan pays its amount into the treasury at once and is
//             repaid in equal daily principal instalments plus interest on what is still owed. Repay early: pay the rest.
//   Bankrupt  the treasury below zero raises one warning event; it clears once the treasury is back above zero. The game
//             goes on (no game over).
//   Taxes     the rates stay in SimEconomy (0 .. 30 %, default 10 %); their demand effect is SimEconomy::DemandOffset,
//             which the host feeds into SimGrowth::offset.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sim_economy.h"

namespace tbd {
namespace city {

struct LoanTier {
    const char* name;
    int64_t amount;      // cents
    int ratePermille;    // yearly interest, permille (50 = 5 %)
    int termDays;
};
inline const LoanTier* LoanTiers(int& n) {
    static const LoanTier k[] = {
        {"Small loan", 50000 * 100LL, 50, 360},    // 50k at 5 % over a year
        {"Medium loan", 200000 * 100LL, 75, 720},  // 200k at 7.5 % over two years
        {"Large loan", 500000 * 100LL, 100, 1440}, // 500k at 10 % over four years
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}
constexpr int kLoanDaysPerYear = 360;

struct Loan {
    int tier = 0;
    int64_t owed = 0;          // principal still owed, cents
    int64_t instalment = 0;    // principal per day, cents
    int daysLeft = 0;
};

enum Expense : int { kExpService = 0, kExpUtility = 1, kExpRoad = 2, kExpInterest = 3, kExpenses = 4 };
inline const char* ExpenseName(int e) {
    static const char* const k[kExpenses] = {"Services", "Utility plants", "Roads", "Loan interest"};
    return e >= 0 && e < kExpenses ? k[e] : "";
}

// What the host hands in each day, cents per day.
struct DayUpkeep {
    int64_t services = 0, utilities = 0, roads = 0;
};

class SimBudget {
public:
    static constexpr int kHistory = 60; // days of balance kept for the trend sparkline
    static constexpr int kMaxLoans = 3; // one per tier
    std::vector<Loan> loans;
    int64_t lastExpense[kExpenses] = {};
    int64_t lastRepayment = 0;
    int64_t history[kHistory] = {}; // treasury at the end of each day, oldest first once full
    int historyCount = 0;
    bool bankrupt = false;
    std::vector<std::string> events; // "bankrupt", "solvent", "loan_taken", "loan_repaid": the host empties it

    void Reset() {
        loans.clear();
        for (int64_t& v : lastExpense) v = 0;
        lastRepayment = 0;
        historyCount = 0;
        bankrupt = false;
        events.clear();
    }

    int64_t LastExpenses() const { return lastExpense[0] + lastExpense[1] + lastExpense[2] + lastExpense[3]; }
    int64_t Owed() const { int64_t n = 0; for (const Loan& l : loans) n += l.owed; return n; }
    bool HasTier(int tier) const { for (const Loan& l : loans) if (l.tier == tier) return true; return false; }

    // Takes a loan of that tier (one of each at most). The money is in the treasury at once.
    bool TakeLoan(int tier, SimEconomy& e) {
        int n = 0;
        const LoanTier* t = LoanTiers(n);
        if (tier < 0 || tier >= n || HasTier(tier) || (int)loans.size() >= kMaxLoans) return false;
        Loan l;
        l.tier = tier;
        l.owed = t[tier].amount;
        l.daysLeft = t[tier].termDays;
        l.instalment = (t[tier].amount + t[tier].termDays - 1) / t[tier].termDays;
        loans.push_back(l);
        e.treasury += t[tier].amount;
        events.push_back("loan_taken");
        return true;
    }
    // Pays off loan i at once, if the treasury has the money.
    bool RepayLoan(int i, SimEconomy& e) {
        if (i < 0 || i >= (int)loans.size() || e.treasury < loans[(size_t)i].owed) return false;
        e.treasury -= loans[(size_t)i].owed;
        loans.erase(loans.begin() + i);
        events.push_back("loan_repaid");
        return true;
    }

    // Daily interest of a loan, cents.
    static int64_t Interest(const Loan& l) {
        int n = 0;
        const LoanTier* t = LoanTiers(n);
        return l.owed * t[l.tier].ratePermille / (1000LL * kLoanDaysPerYear);
    }

    // One game day: SimEconomy's income in, every expense and the loan instalments out.
    void Day(SimEconomy& e, const CitySim& s, const SimTuning& t, const DayUpkeep& up) {
        lastExpense[kExpService] = std::max<int64_t>(0, up.services);
        lastExpense[kExpUtility] = std::max<int64_t>(0, up.utilities);
        lastExpense[kExpRoad] = std::max<int64_t>(0, up.roads);
        int64_t interest = 0, repay = 0;
        for (Loan& l : loans) {
            interest += Interest(l);
            const int64_t p = std::min(l.owed, l.instalment);
            repay += p;
            l.owed -= p;
            --l.daysLeft;
        }
        loans.erase(std::remove_if(loans.begin(), loans.end(), [](const Loan& l) { return l.owed <= 0 || l.daysLeft <= 0; }), loans.end());
        lastExpense[kExpInterest] = interest;
        lastRepayment = repay;
        e.Day(s, t, LastExpenses() + repay); // SimEconomy keeps income, the treasury and the day count
        Push(e.treasury);
        if (!bankrupt && e.treasury < 0) bankrupt = true, events.push_back("bankrupt");
        else if (bankrupt && e.treasury > 0) bankrupt = false, events.push_back("solvent");
    }

    // The balance trend, oldest first.
    int History(int64_t* out, int max) const {
        const int n = std::min(historyCount, std::min(max, kHistory));
        for (int i = 0; i < n; ++i) out[i] = history[(size_t)(kHistory - n + i)];
        return n;
    }

private:
    void Push(int64_t v) {
        for (int i = 0; i + 1 < kHistory; ++i) history[i] = history[i + 1];
        history[kHistory - 1] = v;
        historyCount = std::min(historyCount + 1, kHistory);
    }
};

// Road upkeep per day in cents from the network's length in metres.
inline int64_t RoadUpkeepPerDay(double metres, double perKmDay) { return (int64_t)(metres / 1000.0 * perKmDay * 100.0 + 0.5); }

} // namespace city
} // namespace tbd
