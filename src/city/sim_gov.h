// Phase 7's state in one object (CS2 sim roadmap Phase 7): rates and sliders, city and district policies, districts,
// progression, statistics, the day's tax sums and the PolicyMods per hood. Header only, headless (tests\sim_gov_test).
//
//   Deferred   the mods are recomposed only when their inputs changed (ComposeKey: rates, policies, districts, tree,
//              hood generation); otherwise Compose costs nothing. The taxes run one hourly group per game hour.
//   Check      ComposeFresh builds the same mods from scratch; the tests (and gov.check in the host) compare the two.
//   taxBurden  CellFactor::taxBurden (this phase's one factor): 64 x the cell's effective residential rate / 10 %,
//              rewritten only when the rates, the mods or the cell count changed.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_gov_districts.h"
#include "sim_gov_progress.h"
#include "sim_gov_stats.h"
#include "sim_gov_tax.h"
#include "sim_policy.h"

namespace tbd {
namespace city {

class GovState {
public:
    GovRates rates;
    uint32_t cityPolicies = 0; // sim_policy.h bits, city wide
    GovDistricts districts;
    GovProgress progress;
    GovStats stats;
    GovTaxAccum acc;           // today so far
    GovHappyAccum happy;
    GovIncome last;            // the last ledger's breakdown
    double feeHappyPerPoint = 0.0008;

    // derived (not saved)
    std::vector<PolicyMods> mods;    // per hood
    std::vector<uint8_t> hoodDistrict;
    uint64_t composedKey = 0, modsVersion = 0;

    void Reset() {
        rates = GovRates(), cityPolicies = 0, districts.Clear(), progress.Reset(), stats.Clear();
        acc.Clear(), happy.Clear(), last = GovIncome(), mods.clear(), hoodDistrict.clear(), composedKey = 0, ++modsVersion;
    }
    void Touch() { ++rates.version; }

    // A policy may be switched on when its tree node is bought (switching one off is always allowed).
    bool PolicyOpen(int i) const {
        int n = 0;
        const PolicyDef* p = Policies(n);
        return i >= 0 && i < n && progress.Has(p[i].needs);
    }
    bool SetCityPolicy(int i, bool on) {
        int n = 0;
        const PolicyDef* p = Policies(n);
        if (i < 0 || i >= n || !(p[i].scope & kScopeCity) || (on && !PolicyOpen(i))) return false;
        const uint32_t was = cityPolicies;
        cityPolicies = on ? was | (1u << i) : was & ~(1u << i);
        if (was != cityPolicies) Touch();
        return was != cityPolicies;
    }
    bool SetDistrictPolicy(int district, int i, bool on) {
        int n = 0;
        const PolicyDef* p = Policies(n);
        if (i < 0 || i >= n || !(p[i].scope & kScopeDistrict) || (on && !PolicyOpen(i))) return false;
        return districts.SetPolicy(district, i, on);
    }

    // The mods of one hood from scratch: city policies, the hood's district's, the tree's perks, the budget sliders and fees.
    PolicyMods ModsFor(int district) const {
        PolicyMods m;
        ApplyPolicies(m, cityPolicies);
        if (const GovDistrict* d = districts.Get(district)) ApplyPolicies(m, d->policies);
        progress.ApplyPerks(m);
        for (int k = 0; k < kServicesV2; ++k) m.serviceBudget[k] = (float)rates.budgetPct[k] / 100.0f;
        m.transitFare *= (float)rates.transitFarePct / 100.0f;
        const int over = rates.powerFeePct + rates.waterFeePct - 200; // fee points above 100 % (both together)
        m.happiness *= (float)std::clamp(1.0 - feeHappyPerPoint * over * 0.5, 0.8, 1.1);
        return m;
    }
    uint64_t ComposeKey(uint64_t hoodGeneration, size_t hoods) const {
        uint64_t k = 0x9E3779B97F4A7C15ull;
        for (uint64_t v : {rates.version, (uint64_t)cityPolicies, districts.version, (uint64_t)progress.bought, hoodGeneration, (uint64_t)hoods})
            k = (k ^ v) * 0x100000001B3ull + 0x7F4A7C15ull;
        return k;
    }
    // Recomposes the mods only when an input changed. True: they changed (the host copies them into CellFactors).
    template <class KeyOf>
    bool Compose(const std::vector<int>& cellHood, int hoodCount, uint64_t hoodGeneration, KeyOf&& keyOf) {
        const uint64_t key = ComposeKey(hoodGeneration, (size_t)hoodCount);
        if (key == composedKey && mods.size() == (size_t)std::max(0, hoodCount)) return false;
        composedKey = key;
        ComposeFresh(cellHood, hoodCount, keyOf, mods, hoodDistrict);
        ++modsVersion;
        return true;
    }
    template <class KeyOf>
    void ComposeFresh(const std::vector<int>& cellHood, int hoodCount, KeyOf&& keyOf, std::vector<PolicyMods>& out, std::vector<uint8_t>& hd) const {
        districts.HoodDistricts(cellHood, hoodCount, keyOf, hd);
        out.assign((size_t)std::max(0, hoodCount), PolicyMods());
        PolicyMods byDistrict[GovDistricts::kMax + 1];
        for (int d = 0; d <= GovDistricts::kMax; ++d) byDistrict[d] = ModsFor(d);
        for (int h = 0; h < hoodCount; ++h) out[(size_t)h] = byDistrict[hd[(size_t)h]];
    }

    // Policy costs per day (cents): each policy's cost per resident it covers. hoodPop: residents per hood.
    int64_t PolicyCostPerDay(const std::vector<uint32_t>& hoodPop, int64_t cityPop) const {
        int n = 0;
        const PolicyDef* p = Policies(n);
        int64_t sum = 0;
        for (int i = 0; i < n; ++i)
            if (cityPolicies & (1u << i)) sum += cityPop * p[i].costPerResDay;
        for (size_t h = 0; h < hoodPop.size() && h < hoodDistrict.size(); ++h) {
            const GovDistrict* d = districts.Get(hoodDistrict[h]);
            if (!d) continue;
            for (int i = 0; i < n; ++i)
                if ((d->policies & (1u << i)) && !(cityPolicies & (1u << i))) sum += (int64_t)hoodPop[h] * p[i].costPerResDay;
        }
        return sum;
    }

    // taxBurden for every cell (64 = a 10 % residential rate). False: nothing changed.
    bool WriteTaxBurden(CellFactors& f, const std::vector<int>& cellHood, int mainRate10, size_t cellCount) const {
        f.Fit(cellCount);
        bool changed = false;
        for (size_t c = 0; c < f.cells.size(); ++c) {
            const int h = c < cellHood.size() ? cellHood[c] : -1;
            const float mod = h >= 0 && (size_t)h < mods.size() ? mods[(size_t)h].taxResidential : 1.0f;
            const int v = std::clamp((int)std::lround(64.0 * rates.ResRate10(mainRate10, 2) * mod / 100.0), 0, 255);
            if (f.cells[c].taxBurden != (uint8_t)v) f.cells[c].taxBurden = (uint8_t)v, changed = true;
        }
        if (changed) ++f.version;
        return changed;
    }
};

} // namespace city
} // namespace tbd
