// Taxes and fees (CS2 sim roadmap Phase 7). Header only, headless (tests\sim_gov_test). Whole cents, integer arithmetic.
//
//   Residential  per household member with a job: the wage of the slot they hold (sim_household_econ.h), taxed at the
//                residential rate of THEIR education level (the main rate plus that level's offset, like CS2), times
//                the hood's PolicyMods::taxResidential, times gov.resTaxableShare (the share of the wage the city's rate
//                applies to; calibrated so a city earns what the old per-resident formula earned). Pensions are not
//                taxed. Statistical residents (past the record cap) pay the old per-resident amount.
//   C / O / I    per filled job as before (sim_economy.h), times the hood's taxCommercial / taxOffice / taxIndustrial
//   Fees         electricity and water per resident of a served cell, at the fee slider (100 % = the tuned base)
//   Slicing      the work is split into 24 hourly groups (households and cells with index % 24 == hour): one group runs
//                each game hour, and the day's ledger takes the sum. A day that saw fewer groups (a load mid-day) is
//                scaled up to 24; GovTaxFull runs all groups at once (the full recompute the tests compare against).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_cells.h"
#include "sim_factors.h"
#include "sim_flags.h"
#include "sim_life_world.h"

namespace tbd {
namespace city {

constexpr int kGovSlices = 24;

struct GovRates {
    int eduOffset10[kEduLevels] = {};   // tenths of a percent added to the residential rate per education level (-50 .. 50)
    int budgetPct[kServicesV2] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100}; // 50 .. 150
    int powerFeePct = 100, waterFeePct = 100, transitFarePct = 100; // 0 .. 200
    uint64_t version = 1; // bumped by any change (mods recompose, taxBurden rewrites)

    int ResRate10(int mainRate10, int edu) const { return std::clamp(mainRate10 + eduOffset10[std::clamp(edu, 0, kEduLevels - 1)], 0, 300); }
};

// One day's sums (cents x the scales noted).
struct GovTaxAccum {
    int64_t resNum[kEduLevels] = {}; // sum of wage x rate10 x share-micro / 1000 (see GovTaxFinish)
    int64_t jobNum[kJobKinds] = {};  // sum of employed x efficiency permille x mod permille
    int64_t feeNum = 0;              // cents x fee percent
    uint32_t sliceMask = 0;
    int Slices() const { int n = 0; for (int i = 0; i < kGovSlices; ++i) n += (sliceMask >> i) & 1u; return n; }
    void Clear() { *this = GovTaxAccum(); }
};

struct GovTaxInput {
    const LifeWorld* w = nullptr;          // null: no records (P1 off): residential falls back to the cells
    const CitySim* s = nullptr;
    const std::vector<int>* cellHood = nullptr;          // per CitySim cell
    const std::vector<PolicyMods>* hoodMods = nullptr;   // per hood (null: neutral)
    int mainRate10 = 100;
    const GovRates* rates = nullptr;
    double taxableShare = 0.005, powerFee = 6.0, waterFee = 5.0; // gov.* tuning
};

inline const PolicyMods& GovModsOf(const GovTaxInput& in, int hood) {
    static const PolicyMods kNeutralMods;
    return in.hoodMods && hood >= 0 && (size_t)hood < in.hoodMods->size() ? (*in.hoodMods)[(size_t)hood] : kNeutralMods;
}
inline int64_t Permille(float m) { return (int64_t)std::llround((double)m * 1000.0); }

// One hourly group.
inline void GovTaxSlice(const GovTaxInput& in, int slice, GovTaxAccum& a) {
    if (slice < 0 || slice >= kGovSlices || (a.sliceMask & (1u << slice))) return;
    a.sliceMask |= 1u << slice;
    const int64_t shareMicro = (int64_t)std::llround(in.taxableShare * 1e6);
    if (in.w) {
        const LifeWorld& w = *in.w;
        for (uint32_t hi = (uint32_t)slice; hi < w.hh.Capacity(); hi += kGovSlices) {
            if (!hi || !w.hh.At(hi).Valid()) continue;
            const uint32_t b = w.hh.home[hi], bi = LifeWorld::Ix(b);
            const int hood = b && bi < w.bld.size() && w.bld[bi].id == b ? w.bld[bi].hood : -1;
            const int64_t mod = Permille(GovModsOf(in, hood).taxResidential);
            for (uint32_t c = w.hh.firstMember[hi]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)]) {
                const uint32_t ci = LifeWorld::Ix(c);
                if (!w.cit.work[ci]) continue;
                const int e = w.cit.edu[ci];
                a.resNum[e] += (int64_t)w.tune.Wage(w.cit.jobLevel[ci]) * in.rates->ResRate10(in.mainRate10, e) * mod * shareMicro / 1000;
            }
        }
    }
    if (!in.s) return;
    const auto& cells = in.s->sim;
    for (size_t c = (size_t)slice; c < cells.size(); c += kGovSlices) {
        const CellSim& cs = cells[c];
        const int hood = in.cellHood && c < in.cellHood->size() ? (*in.cellHood)[c] : -1;
        const PolicyMods& m = GovModsOf(in, hood);
        const int64_t eff = CellEfficiencyPermille(cs);
        const int64_t mods[kJobKinds] = {Permille(m.taxIndustrial), Permille(m.taxCommercial), Permille(m.taxOffice)};
        for (int k = 0; k < kJobKinds; ++k) a.jobNum[k] += (int64_t)cs.employed[k] * eff * mods[k];
        const int64_t pop = cs.Population();
        if (cs.powerOk) a.feeNum += pop * (int64_t)std::llround(in.powerFee * in.rates->powerFeePct);
        if (cs.waterOk) a.feeNum += pop * (int64_t)std::llround(in.waterFee * in.rates->waterFeePct);
    }
}
inline void GovTaxFull(const GovTaxInput& in, GovTaxAccum& a) {
    a.Clear();
    for (int i = 0; i < kGovSlices; ++i) GovTaxSlice(in, i, a);
}

// What the day's ledger shows and pays (cents).
struct GovIncome {
    int64_t resByEdu[kEduLevels] = {}, resStat = 0;
    int64_t cat[4] = {}; // SimEconomy order: residential, commercial, office, industrial
    int64_t fees = 0, trade = 0, policyCost = 0;
    int64_t Other() const { return fees + trade; }
};

// The sums into a day's income. perJob: SimTuning tax*PerJobDay in C, O, I order; perRes: taxResidentialPerResidentDay.
inline void GovTaxFinish(const GovTaxAccum& a, const GovTaxInput& in, const int (&rate10)[4], const double (&perJob)[3], double perRes,
                         int daysPerMonth, GovIncome& out) {
    const int n = a.Slices();
    const int64_t scaleNum = n > 0 ? kGovSlices : 0, scaleDen = n > 0 ? n : 1;
    int64_t res = 0;
    for (int e = 0; e < kEduLevels; ++e) {
        // resNum = wage (whole) x rate10 x mod permille x share micro / 1000; the tax in whole currency is
        // wage x rate10/1000 x mod/1000 x share/1e6 = resNum / 1e9, in cents resNum / 1e7
        out.resByEdu[e] = a.resNum[e] / 10000000 * scaleNum / scaleDen / std::max(1, daysPerMonth);
        res += out.resByEdu[e];
    }
    const int64_t stat = in.w ? in.w->statResidents : 0;
    out.resStat = stat * (int64_t)perRes * 900 / 1000 * rate10[0] / 100; // the old formula at a modest wealth
    out.cat[0] = res + out.resStat;
    static const int kJobOf[3] = {kJobCommercial, kJobOffice, kJobIndustrial};
    for (int c = 0; c < 3; ++c)
        out.cat[c + 1] = a.jobNum[kJobOf[c]] / 1000000 * (int64_t)perJob[c] * rate10[c + 1] / 100 * scaleNum / scaleDen;
    out.fees = a.feeNum / 100 * scaleNum / scaleDen; // feeNum: cents x fee percent
}

} // namespace city
} // namespace tbd
