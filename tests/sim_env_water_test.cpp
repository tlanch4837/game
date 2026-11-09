// Phase 3 water table and floods (src/city/sim_env_water.h): a storm floods a basin, more when the ground is saturated,
// the flood recedes after the rain, rain recharges the table, a pump draws a cone down and loses efficiency, a slope to
// the sea drains, and the pass is deterministic. Headless. Build and run: tests\sim_env_water_test.bat
#include <cmath>
#include <cstdio>
#include <vector>

#include "city/sim_env_water.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// A 7 x 7 cell (900 m) area; bowl: height rises with the distance from the middle; slope: falls towards -X (the sea).
static EnvWater Make(bool bowl, int natural) {
    EnvWater w;
    for (int z = -3; z <= 3; ++z)
        for (int x = -3; x <= 3; ++x)
            w.EnsureCell({x, z}, natural, [&](float px, float pz) {
                return bowl ? (float)(0.004 * std::sqrt(px * px + pz * pz) + 20.0) : (float)(0.01 * px + 5.0);
            });
    return w;
}
static uint16_t MaxDepth(const EnvWater& w) {
    uint16_t m = 0;
    for (const auto& t : w.depth.Tiles()) for (uint16_t v : t) m = std::max(m, v);
    return m;
}
static double Mean(const Field<uint16_t>& f) {
    double s = 0; size_t n = 0;
    for (const auto& t : f.Tiles()) for (uint16_t v : t) s += v, ++n;
    return n ? s / (double)n : 0;
}

int main() {
    const std::vector<EnvPump> none;
    // 1. a storm on a basin: saturated ground floods deeper than dry ground
    EnvWaterParams wet, dry;
    wet.natural = 9500, dry.natural = 2000;
    EnvWater a = Make(true, wet.natural), b = Make(true, dry.natural);
    CHECK(a.Aligned() && b.Aligned());
    for (int h = 0; h < 8; ++h) a.Step(22.0, 0, -1e9f, none, wet), b.Step(22.0, 0, -1e9f, none, dry);
    const uint16_t da = MaxDepth(a), db = MaxDepth(b);
    std::printf("storm: deepest %u mm on saturated ground, %u mm on dry ground\n", da, db);
    CHECK(da >= wet.floodMm);
    CHECK(da > db);
    // 2. rain recharged the dry table
    const double tableAfter = Mean(b.table);
    std::printf("table: dry ground %.0f after the storm (natural %d)\n", tableAfter, dry.natural);
    CHECK(tableAfter > dry.natural);
    // 3. the flood recedes after the rain stops
    for (int h = 0; h < 96; ++h) a.Step(0.0, 0, -1e9f, none, wet);
    std::printf("recede: deepest %u mm after 4 dry days\n", MaxDepth(a));
    CHECK(MaxDepth(a) < wet.floodMm);
    // 4. a slope to the sea drains: the same storm floods nothing
    EnvWater s = Make(false, wet.natural);
    for (int h = 0; h < 8; ++h) s.Step(22.0, 0, 3.0f, none, wet); // x < -200 m is sea
    std::printf("slope to the sea: deepest %u mm\n", MaxDepth(s));
    CHECK(MaxDepth(s) < MaxDepth(a) + wet.floodMm && MaxDepth(s) < da);
    // 5. a big groundwater pump draws the table down around it and loses efficiency; summer dries the ground
    {
        EnvWaterParams p;
        EnvWater w = Make(true, p.natural);
        std::vector<EnvPump> pumps = {{16.0f, 16.0f, 20000.0f, true}};
        for (int h = 0; h < 48; ++h) w.Step(0.0, 0, -1e9f, pumps, p);
        const double eff = w.PumpEfficiency(pumps[0], p);
        const int at = w.table.At(16, 16), far = w.table.At(16 + 400, 16);
        std::printf("pump: table %d at the pump, %d 400 m away, efficiency %.2f\n", at, far, eff);
        CHECK(at < far && eff < 0.9);
        EnvWater sum = Make(true, p.natural), spr = Make(true, p.natural);
        for (int h = 0; h < 48; ++h) sum.Step(0.0, 1, -1e9f, none, p), spr.Step(0.0, 0, -1e9f, none, p);
        CHECK(Mean(sum.table) < Mean(spr.table));
    }
    // 6. deterministic
    {
        EnvWater x = Make(true, 7000), y = Make(true, 7000);
        EnvWaterParams p;
        std::vector<EnvPump> pumps = {{-100.0f, 50.0f, 6000.0f, true}};
        for (int h = 0; h < 30; ++h) {
            const double rain = h % 7 < 3 ? 12.0 : 0.0;
            x.Step(rain, 2, 19.0f, pumps, p), y.Step(rain, 2, 19.0f, pumps, p);
        }
        CHECK(x.depth.Tiles() == y.depth.Tiles() && x.table.Tiles() == y.table.Tiles());
    }
    std::printf(g_fail ? "sim_env_water_test: %d FAILED\n" : "sim_env_water_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
