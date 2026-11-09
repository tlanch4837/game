// ------------------------------------------------------------------- city sim Phase 7: budget, taxes, fees, policies, districts, milestones, statistics
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 7 owns this file; the hub calls the five hooks below,
// whose signatures are frozen by Phase 0. The model is headless: src/city/sim_gov*.h, sim_policy.h (tests\sim_gov_tax_test,
// sim_gov_policy_test, sim_gov_bench); this file is its host:
//   system     "gov", every game hour: one of the 24 hourly groups of households and cells into the day's tax sums
//              (deferred: one group per hour, never the whole city in one tick), and the citizens' happiness of that group
//   ledger     city_sim_budget.inl's daily "economy" calls SimGovBeforeLedger (the day's income into SimEconomy) and
//              SimGovAfterLedger (statistics sample, XP)
//   mods       PolicyMods per hood into g_cellFactors.hoods, recomposed only when an input changed (rates, policies,
//              districts, tree, hoods); taxBurden per cell rewritten only then; a daily check against a fresh compose
//   UI         Budget, Policies, Districts, Development and Statistics panels and the Districts overlay
//              (city_sim_p7_panels.inl)
//   save       BUDG PLCY DIST MILE (sim_gov_save.h)
// Tuning: the "gov." keys of tuning.json (sim_tuning_table.h GovTuning).
// The headers (sim_gov_save.h and what it includes) come from the host TUs.

namespace sim_p7 {
tbd::city::GovState gov;
bool on = false;
double tickMs = 0, tickAvgMs = 0, tickWorstMs = 0, frameMs = 0;
uint64_t burdenKey = ~0ull, milestonesSeen = ~0ull;
int checkFails = 0;

const tbd::city::GovTuning& T() { return g_simPhaseTune.gov; }
uint64_t KeyOf(int cell) {
    const auto& cs = g_citySim.cells.cells;
    return cell >= 0 && (size_t)cell < cs.size() ? tbd::city::PackKey(cs[(size_t)cell].key) : 0;
}
int HoodCount() { return (int)herds_host::hoods.hoods.size(); }

tbd::city::GovTaxInput Input() {
    tbd::city::GovTaxInput in;
    in.w = SimP1On() ? &g_life.w : nullptr;
    in.s = &g_citySim, in.cellHood = &herds_host::hoods.cellHood, in.hoodMods = &gov.mods;
    in.mainRate10 = g_simEcon.taxRate10[0], in.rates = &gov.rates;
    in.taxableShare = T().resTaxableShare, in.powerFee = T().powerFeePerResDay, in.waterFee = T().waterFeePerResDay;
    return in;
}

// The hour's group: taxes and happiness. Hour 0 starts a new day (the ledger at minute 0 already took yesterday's).
void Tick(const tbd::city::SimTick& t) {
    const double a = g_sim.nowMs();
    const int hour = (int)(((t.minute / 60) % 24 + 24) % 24);
    if (hour == 0) gov.acc.Clear(), gov.happy.Clear();
    tbd::city::GovTaxSlice(Input(), hour, gov.acc);
    if (SimP1On()) {
        const auto& c = g_life.w.cit;
        for (uint32_t ci = (uint32_t)hour; ci < c.Capacity(); ci += tbd::city::kGovSlices)
            if (ci && (c.flags[ci] & tbd::city::kCitAlive) && c.At(ci).Valid()) gov.happy.sum += c.happy[ci], ++gov.happy.count;
    }
    const double ms = g_sim.nowMs() - a;
    tickMs = ms, tickAvgMs += (ms - tickAvgMs) * 0.05, tickWorstMs = std::max(tickWorstMs, ms);
}

// The mods into the shared factors when they changed, and taxBurden with them.
void SyncMods() {
    const auto& hs = herds_host::hoods;
    if (gov.Compose(hs.cellHood, HoodCount(), hs.generation, KeyOf)) {
        g_cellFactors.hoods = gov.mods;
        ++g_cellFactors.version;
    }
    const uint64_t key = gov.modsVersion * 1000003ull ^ (uint64_t)g_simEcon.taxRate10[0] * 7919ull ^ (uint64_t)g_citySim.sim.size() << 40;
    if (key != burdenKey) burdenKey = key, gov.WriteTaxBurden(g_cellFactors, hs.cellHood, g_simEcon.taxRate10[0], g_citySim.sim.size());
}

// The deferred-state check: the incremental mods against a fresh compose (once a game day).
void CheckMods() {
    std::vector<tbd::city::PolicyMods> fresh;
    std::vector<uint8_t> hd;
    const auto& hs = herds_host::hoods;
    if (gov.mods.size() != (size_t)HoodCount()) return; // a rebuild since the last compose: the next frame recomposes
    gov.ComposeFresh(hs.cellHood, HoodCount(), KeyOf, fresh, hd);
    const bool same = fresh.size() == gov.mods.size() && (fresh.empty() || std::memcmp(fresh.data(), gov.mods.data(), fresh.size() * sizeof(fresh[0])) == 0);
    if (!same && ++checkFails <= 5) tbd::Log("Sim: P7 check: the policy mods were stale (recomposed)");
    if (!same) gov.composedKey = 0;
}
} // namespace sim_p7

#include "city_sim_p7_ledger.inl" // SimGovBeforeLedger / SimGovAfterLedger and the upkeep hooks
#include "city_sim_p7_panels.inl" // the panels and the Districts overlay
#include "city_sim_p7_save.inl"   // SimP7Save / SimP7Load: BUDG PLCY DIST MILE

void SimP7Init() {
    using namespace sim_p7;
    on = true;
    gov.progress.levelBase = std::max(1.0, T().xpLevelBase), gov.progress.levelGrowth = std::clamp(T().xpLevelGrowth, 1.0, 4.0);
    gov.feeHappyPerPoint = T().feeHappyPerPoint;
    g_sim.Register("gov", 60, 1, sim_p7::Tick);
    tbd::gui::RegisterOverlayProvider(tbd::gui::Overlay::Districts, sim_p7::OverlayCell, nullptr, "Districts (painted in the Districts panel)");
}

void SimP7Frame(float /*realDt*/) {
    using namespace sim_p7;
    const double a = g_sim.nowMs();
    SyncFunding();
    SyncMods();
    if ((uint64_t)g_simMilestones.reached != milestonesSeen) {
        milestonesSeen = (uint64_t)g_simMilestones.reached;
        const int got = gov.progress.OnMilestones(g_simMilestones.reached);
        if (got > 0 && g_clock.minutes >= g_simMilestoneQuietUntil) SimNote("Milestone: +" + std::to_string(got) + " development point" + (got > 1 ? "s" : ""));
    }
    PaintFrame();
    frameMs = g_sim.nowMs() - a;
}
