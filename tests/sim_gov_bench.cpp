// Phase 7's cost at about 50k residents (headless): the hourly "gov" group (taxes of 1/24 of the households and cells,
// happiness), the daily ledger finish, the mods compose with 120 hoods and 8 districts, taxBurden. The roadmap budget:
// 0.05 ms per tick (game minute) on average, no call above 0.5 ms (numbers x 2 are the iPad gate until calibrated).
// Build and run: tests\sim_gov_bench.bat [side]   (side x side cells of 40 homes; default 36 = about 52k residents)
#include <chrono>
#include <cstdlib>

#include "sim_life_town.h"

#include "city/sim_gov.h"

using namespace lifetest;

static double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    const int side = argc > 1 ? std::max(4, std::atoi(argv[1])) : 36;
    Town t;
    t.sim.w.citySeed = 11;
    t.sim.w.tune.instantFill = 1;
    t.Build(side, 40, 30);
    int64_t minute = 0;
    t.RunDays(minute, 2);
    const LifeWorld& w = t.sim.w;
    // a CitySim with a matching number of built lots for the per-job taxes and fees
    std::vector<Lot> lots;
    SplitMix64 r(5);
    for (uint64_t i = 0; i < (uint64_t)side * side * 4; ++i) {
        Lot l;
        l.id = i + 1, l.c = {(float)r.Below(4000), (float)r.Below(4000)}, l.type = (int)r.Below(7), l.w = 2, l.d = 2, l.path = "m", l.progress = 2.0f;
        lots.push_back(l);
    }
    CitySim s;
    s.Sync(lots, 1);
    std::vector<int> cellHood(s.sim.size());
    for (size_t c = 0; c < cellHood.size(); ++c) cellHood[c] = (int)(c % 120);
    GovState g;
    for (int d = 1; d <= 8; ++d) {
        g.districts.Add("D" + std::to_string(d));
        std::vector<uint64_t> keys;
        for (size_t c = (size_t)d; c < cellHood.size(); c += 9) keys.push_back(PackKey(s.cells.cells[c].key));
        g.districts.Paint(keys, d);
    }
    auto keyOf = [&](int c) { return PackKey(s.cells.cells[(size_t)c].key); };
    const double c0 = Now();
    g.Compose(cellHood, 120, 1, keyOf);
    const double composeMs = Now() - c0;
    GovTaxInput in;
    in.w = &w, in.s = &s, in.cellHood = &cellHood, in.hoodMods = &g.mods, in.rates = &g.rates;
    double worst = 0, sum = 0;
    const int days = 3;
    for (int d = 0; d < days; ++d) {
        g.acc.Clear(), g.happy.Clear();
        for (int h = 0; h < kGovSlices; ++h) {
            const double a = Now();
            GovTaxSlice(in, h, g.acc);
            for (uint32_t ci = (uint32_t)h; ci < w.cit.Capacity(); ci += kGovSlices)
                if (ci && w.cit.At(ci).Valid()) g.happy.sum += w.cit.happy[ci], ++g.happy.count;
            const double ms = Now() - a;
            sum += ms, worst = std::max(worst, ms);
        }
    }
    const double f0 = Now();
    GovIncome inc;
    const int rates[4] = {100, 100, 100, 100};
    const double perJob[3] = {60, 80, 50};
    GovTaxFinish(g.acc, in, rates, perJob, 40, 1, inc);
    CellFactors f;
    g.WriteTaxBurden(f, cellHood, 100, s.sim.size());
    const double finishMs = Now() - f0;
    const double groupAvg = sum / (days * kGovSlices);
    const double perTick = groupAvg / 60.0 + (finishMs + composeMs) / 1440.0;
    std::printf("sim_gov_bench: %u residents, %u households, %zu cells, 120 hoods, 8 districts\n", w.Population(), w.hh.Live(), s.sim.size());
    std::printf("  hourly group  avg %.4f ms, worst %.4f ms\n", groupAvg, worst);
    std::printf("  ledger finish + taxBurden %.4f ms, compose %.4f ms (only on change)\n", finishMs, composeMs);
    std::printf("  per tick (game minute) average %.5f ms (budget 0.05), worst call %.4f ms (budget 0.5)\n", perTick, std::max(worst, finishMs + composeMs));
    std::printf("  residential tax %lld a day, happiness %lld permille\n", (long long)(inc.cat[0] / 100), (long long)g.happy.Permille());
    const bool ok = perTick < 0.05 && std::max(worst, finishMs + composeMs) < 0.5;
    std::printf(ok ? "sim_gov_bench: inside the budget\n" : "sim_gov_bench: OVER BUDGET\n");
    return ok ? 0 : 1;
}
