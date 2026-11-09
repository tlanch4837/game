// The sim's preset scaling (src/city/sim_preset.h): budgets fall with the preset and the thermal state, never rise.
#include <cstdio>

#include "city/sim_preset.h"

using namespace tbd::city;

static int g_fails = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fails; } \
    } while (0)

int main() {
    for (int p = 0; p < 4; ++p) {
        const SimPresetScale n = SimPresetFor(p, 0);
        CHECK(n == SimPresetFor(p, 1)); // fair changes nothing
        const SimPresetScale s = SimPresetFor(p, 2), c = SimPresetFor(p, 3);
        CHECK(s.budgetMs == n.budgetMs * 0.5); // the roadmap: half the budget at serious
        CHECK(c.budgetMs < s.budgetMs && c.carDensity < s.carDensity && c.maxParked < s.maxParked && c.truckShare < s.truckShare);
        CHECK(n.budgetMs <= 2.0 && n.budgetMs >= 1.0);
        if (p > 0) {
            const SimPresetScale lo = SimPresetFor(p - 1, 0);
            CHECK(lo.budgetMs <= n.budgetMs && lo.carDensity <= n.carDensity && lo.maxParked <= n.maxParked);
        }
    }
    CHECK(SimPresetFor(-3, 9) == SimPresetFor(0, 3)); // clamped
    CHECK(SimPresetFor(0, 0).budgetMs == 1.0 && SimPresetFor(2, 0).budgetMs == 2.0);
    std::printf(g_fails ? "sim_preset_test: %d FAILED\n" : "sim_preset_test: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
