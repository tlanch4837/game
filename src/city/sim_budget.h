// The city sim's budget (sim gameplay pass, phase B): on top of SimEconomy (sim_economy.h: treasury, tax rates, income),
// itemised expenses, loans, a balance history and the bankruptcy warning. Header only, headless
// (tests\sim_economy_test.cpp). Whole cents, integer arithmetic: the same city gives the same ledger.
//
//   Expenses  per day: service upkeep (fire, police, health, school, transit buildings, scaled by funding), utility plant
//             upkeep, road upkeep (per km of road a day), loan interest. Loan principal repayments are charged too but
//             are not an expense (they show as "repayment").
//   Loans     three tiers (amount, yearly interest, term in days). A loan pays its amount into the treasury at once and is
//             repaid in equal daily principal instalments plus interest on what is still owed. Repay early: pay the rest.
//   Build     construction is paid at once when something is placed (Charge): buildings at their catalog cost, roads per
//             lane-metre. It may push the treasury below zero (the warning fires, the build goes on). It shows on the next
//             day's ledger as "construction" but is not part of the running expenses (it is already paid).
//   Bankrupt  the treasury below zero raises one warning event; it clears once the treasury is back above zero. The game
//             goes on (no game over).
//   Save      SaveFields / LoadFields: loans, the warning flag and the history as one text field (city_sim.inl's M record).
//   Taxes     the rates stay in SimEconomy (0 .. 30 %, default 10 %); their demand effect is SimEconomy::DemandOffset,
//             which the host feeds into SimGrowth::offset.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
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
    int64_t popHistory[kHistory] = {}; // population at the end of each day (the Statistics panel), same order
    int historyCount = 0;
    int64_t constructionToday = 0;  // charged since the last day ran, cents
    int64_t lastConstruction = 0;   // what the last day's ledger shows as construction
    bool bankrupt = false;
    std::vector<std::string> events; // "bankrupt", "solvent", "loan_taken", "loan_repaid": the host empties it

    void Reset() {
        loans.clear();
        for (int64_t& v : lastExpense) v = 0;
        lastRepayment = 0;
        historyCount = 0;
        constructionToday = lastConstruction = 0;
        bankrupt = false;
        events.clear();
    }

    int64_t LastExpenses() const { int64_t n = 0; for (int64_t v : lastExpense) n += v; return n; }

    // Construction, paid at once (cents). It can push the treasury below zero: the warning fires, the build goes on.
    void Charge(SimEconomy& e, int64_t cents) {
        if (cents <= 0) return;
        e.treasury -= cents;
        constructionToday += cents;
        if (!bankrupt && e.treasury < 0) bankrupt = true, events.push_back("bankrupt");
    }
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
    // population: the city's residents for the Statistics panel's history (-1: not tracked, kept at 0).
    void Day(SimEconomy& e, const CitySim& s, const SimTuning& t, const DayUpkeep& up, int64_t population = -1) {
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
        lastConstruction = constructionToday, constructionToday = 0;
        Push(e.treasury, std::max<int64_t>(0, population));
        if (!bankrupt && e.treasury < 0) bankrupt = true, events.push_back("bankrupt");
        else if (bankrupt && e.treasury > 0) bankrupt = false, events.push_back("solvent");
    }

    // The balance trend, oldest first.
    int History(int64_t* out, int max) const {
        const int n = std::min(historyCount, std::min(max, kHistory));
        for (int i = 0; i < n; ++i) out[i] = history[(size_t)(kHistory - n + i)];
        return n;
    }
    int PopHistory(int64_t* out, int max) const {
        const int n = std::min(historyCount, std::min(max, kHistory));
        for (int i = 0; i < n; ++i) out[i] = popHistory[(size_t)(kHistory - n + i)];
        return n;
    }

    // One text field (no '|'): "B1;<bankrupt>;<loans: tier owed instalment daysLeft, comma separated>;<history: treasury
    // population, comma separated>". Older saves have none: the budget then starts fresh (no loans), as before.
    std::string SaveFields() const {
        std::ostringstream o;
        o << "B1;" << (bankrupt ? 1 : 0) << ';';
        for (size_t i = 0; i < loans.size(); ++i)
            o << (i ? "," : "") << loans[i].tier << ' ' << (long long)loans[i].owed << ' ' << (long long)loans[i].instalment << ' ' << loans[i].daysLeft;
        o << ';';
        for (int i = 0; i < historyCount; ++i) {
            const size_t k = (size_t)(kHistory - historyCount + i);
            o << (i ? "," : "") << (long long)history[k] << ' ' << (long long)popHistory[k];
        }
        return o.str();
    }
    bool LoadFields(const std::string& f) {
        if (f.rfind("B1;", 0) != 0) return false;
        std::vector<std::string> part;
        size_t a = 0;
        for (size_t b; (b = f.find(';', a)) != std::string::npos; a = b + 1) part.push_back(f.substr(a, b - a));
        part.push_back(f.substr(a));
        if (part.size() < 4) return false;
        Reset();
        bankrupt = std::atoi(part[1].c_str()) != 0;
        auto items = [](const std::string& s) {
            std::vector<std::string> v;
            size_t p = 0;
            for (size_t q; (q = s.find(',', p)) != std::string::npos; p = q + 1) v.push_back(s.substr(p, q - p));
            if (p < s.size()) v.push_back(s.substr(p));
            return v;
        };
        int nt = 0;
        LoanTiers(nt);
        for (const std::string& it : items(part[2])) {
            Loan l;
            long long owed = 0, inst = 0;
            if (std::sscanf(it.c_str(), "%d %lld %lld %d", &l.tier, &owed, &inst, &l.daysLeft) != 4) continue;
            l.owed = owed, l.instalment = inst;
            if (l.tier < 0 || l.tier >= nt || HasTier(l.tier) || l.owed <= 0 || l.daysLeft <= 0 || (int)loans.size() >= kMaxLoans) continue;
            loans.push_back(l);
        }
        for (const std::string& it : items(part[3])) {
            long long t = 0, p = 0;
            if (std::sscanf(it.c_str(), "%lld %lld", &t, &p) == 2) Push(t, p);
        }
        return true;
    }

private:
    void Push(int64_t v, int64_t pop) {
        for (int i = 0; i + 1 < kHistory; ++i) history[i] = history[i + 1], popHistory[i] = popHistory[i + 1];
        history[kHistory - 1] = v, popHistory[kHistory - 1] = pop;
        historyCount = std::min(historyCount + 1, kHistory);
    }
};

// Road upkeep per day in cents from a length in metres (the host passes lane-metres: see RoadLaneMetres).
inline int64_t RoadUpkeepPerDay(double metres, double perKmDay) { return (int64_t)(metres / 1000.0 * perKmDay * 100.0 + 0.5); }

// Roads cost by lane-metre: a two-lane road of length L is 2 L lane-metres (rail tracks count as two lanes).
inline double RoadLaneMetres(double lengthMetres, int lanes) { return std::max(0.0, lengthMetres) * (double)std::max(1, lanes); }
constexpr double kRoadUpkeepPerLaneKmDay = 20.0; // whole currency a day per lane-km (a 2-lane km: 40, as before)
constexpr int64_t kRoadCostPerLaneMetre = 1000;  // cents (a 2-lane km: 20,000 to build)
inline int64_t RoadCost(double laneMetres) { return (int64_t)(std::max(0.0, laneMetres) * (double)kRoadCostPerLaneMetre + 0.5); }

} // namespace city
} // namespace tbd
