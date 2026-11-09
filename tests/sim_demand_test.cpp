// Phase 2 demand (src/city/sim_demand.h): each driver moves the right bar the right way, the breakdown sums to the bar,
// smoothing is bounded and converges. Headless. Build and run: tests\sim_demand_test.bat
#include <cmath>
#include <cstdio>

#include "city/sim_demand.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static DemandInput Base() { // a balanced town of 5,000
    DemandInput in;
    in.population = 5000, in.adults = 3200, in.homes = 5200, in.freeHomes = 200;
    in.seekers[1] = 60, in.seekers[2] = 60, in.seekers[3] = 30;
    in.jobs[0] = 900, in.jobs[1] = 600, in.jobs[2] = 900;
    in.vacancies[0] = 50, in.vacancies[1] = 40, in.vacancies[2] = 50;
    return in;
}
static double Bar(const DemandInput& in, int c) { return ComputeDemand(in, DemandParams()).bar[c].raw; }
enum { R = 0, C = 1, O = 2, I = 3 };

int main() {
    const DemandInput b = Base();
    const DemandResult base = ComputeDemand(b, DemandParams());
    for (int c = 0; c < 4; ++c) {
        std::printf("%c %+.3f:", "RCOI"[c], base.bar[c].raw);
        for (int f = 0; f < base.bar[c].n; ++f) std::printf("  %s %+.3f", base.bar[c].f[f].name, base.bar[c].f[f].value);
        std::printf("\n");
    }
    // 1. unemployment: job seekers up -> industry and office up, residential down
    {
        DemandInput u = b;
        u.seekers[1] += 400, u.seekers[4] += 300;
        CHECK(Bar(u, I) > Bar(b, I) && Bar(u, O) > Bar(b, O) && Bar(u, R) < Bar(b, R));
        DemandInput lo = b, hi = b; // education decides which: basic seekers feed industry, graduates offices
        lo.seekers[0] += 500, hi.seekers[4] += 500;
        CHECK(Bar(lo, I) - Bar(b, I) > Bar(lo, O) - Bar(b, O));
        CHECK(Bar(hi, O) - Bar(b, O) > Bar(hi, I) - Bar(b, I));
    }
    // 2. vacant jobs: residential up, the workplace bar of that kind down
    {
        DemandInput v = b;
        v.vacancies[0] += 500;
        CHECK(Bar(v, R) > Bar(b, R) && Bar(v, I) < Bar(b, I));
        DemandInput w = b;
        w.vacancies[2] += 500;
        CHECK(Bar(w, O) < Bar(b, O));
    }
    // 3. goods: shops short of goods -> industry up, commercial down; more shops -> more goods wanted from industry
    {
        DemandInput g = b;
        g.goodsAvailable = 60;
        CHECK(Bar(g, I) > Bar(b, I) && Bar(g, C) < Bar(b, C));
        DemandInput s = b;
        s.jobs[1] = 1500;
        CHECK(Bar(s, I) > Bar(b, I) && Bar(s, C) < Bar(b, C));
    }
    // 4. rent, free homes, migration, spending, taxes
    {
        DemandInput r = b; r.rentPressure = 0.6;
        CHECK(Bar(r, R) < Bar(b, R));
        DemandInput f = b; f.freeHomes = 1500;
        CHECK(Bar(f, R) < Bar(b, R));
        DemandInput m = b; m.attract = 1.8;
        CHECK(Bar(m, R) > Bar(b, R));
        DemandInput sp = b; sp.spendIndex = 1.5;
        CHECK(Bar(sp, C) > Bar(b, C));
        DemandInput t = b; t.tax[I] = -0.2;
        CHECK(std::fabs(Bar(t, I) - Bar(b, I) + 0.2) < 1e-9);
    }
    // 5. the breakdown sums to the bar, and the start-up pull is there only in a tiny town
    {
        DemandInput tiny; // nothing built yet
        const DemandResult t = ComputeDemand(tiny, DemandParams());
        bool sums = true;
        for (const DemandResult* d : {&base, &t})
            for (int c = 0; c < 4; ++c) {
                double s = 0;
                for (int f = 0; f < d->bar[c].n; ++f) s += d->bar[c].f[f].value;
                sums &= std::fabs(s - d->bar[c].raw) < 1e-12;
            }
        CHECK(sums);
        CHECK(t.bar[R].raw > 0.5 && base.bar[R].f[0].value == 0.0);
    }
    // 6. smoothing: bounded, converges to the clamped raw values, never overshoots
    {
        DemandInput u = b;
        u.seekers[0] = 5000; // far past the clamp
        const DemandResult r = ComputeDemand(u, DemandParams());
        double bars[4] = {};
        bool bounded = true;
        for (int i = 0; i < 2000; ++i) {
            SmoothDemand(bars, r, DemandParams(), 10.0);
            for (double v : bars) bounded &= v >= -1.0 && v <= 1.0;
        }
        CHECK(bounded);
        CHECK(std::fabs(bars[I] - 1.0) < 1e-3);
    }
    std::printf(g_fail ? "sim_demand_test: %d FAILED\n" : "sim_demand_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
