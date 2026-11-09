// Phase 3 pollution (src/city/sim_env_field.h, sim_env_pollution.h): diffusion conservation, the fast diffuse equals
// sim_field.h Diffuse bit for bit, point source falloff, wind shift, decay to zero after removal, noise without memory,
// deterministic passes. Headless. Build and run: tests\sim_env_pollution_test.bat
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "city/sim_env_pollution.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static EnvField Block(int r) { // cells -r..r on both axes, all zero
    EnvField f;
    for (int z = -r; z <= r; ++z) for (int x = -r; x <= r; ++x) f.Cell({x, z});
    return f;
}
static uint64_t Hash(const EnvField& f) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < f.Keys().size(); ++i) {
        h = (h ^ (uint32_t)f.Keys()[i].x) * 1099511628211ull, h = (h ^ (uint32_t)f.Keys()[i].z) * 1099511628211ull;
        for (uint16_t v : f.Tiles()[i]) h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

int main() {
    // 1. conservation: a mass in the middle of a big block, kernel weights sum to 1, keep 1
    {
        EnvField f = Block(4);
        f.Set(16.0f, 16.0f, 60000);
        const double before = EnvFieldSum(f);
        DiffuseKernel k;
        k.steps = 6;
        const EnvField g = EnvDiffuse(f, k);
        const double after = EnvFieldSum(g);
        std::printf("conservation: before %.0f after %.0f (%.3f %%)\n", before, after, 100.0 * (after - before) / before);
        CHECK(std::abs(after - before) / before < 0.01);
        // 2. the fast path equals sim_field.h Diffuse, also with wind
        k.windX = 1, k.windZ = -1, k.steps = 3;
        CHECK(Hash(EnvDiffuse(f, k)) == Hash(Diffuse(f, k)));
        k.windX = 0, k.windZ = 0, k.keep = 0.9f;
        CHECK(Hash(EnvDiffuse(f, k)) == Hash(Diffuse(f, k)));
    }
    EnvPollutionParams p;
    EnvWeather calm;
    calm.windMs = 0, calm.windX = calm.windZ = 0;
    EnvSource coal = EnvUtilitySource(0.0f, 0.0f, 6); // the big coal plant
    // 3. falloff: after a day of a point source, the air is highest at it and falls with distance
    {
        EnvPollution e;
        for (int h = 0; h < 24; ++h) e.Step({coal}, calm, p);
        const int a0 = e.air.At(5, 5), a1 = e.air.At(5 + 96, 5), a2 = e.air.At(5 + 192, 5), a3 = e.air.At(5 + 320, 5);
        std::printf("falloff: %d %d %d %d (0, 96, 192, 320 m)\n", a0, a1, a2, a3);
        CHECK(a0 > a1 && a1 > a2 && a2 >= a3);
        CHECK(a0 > 0);
        CHECK(e.noise.At(5, 5) > e.noise.At(5 + 64, 5));
        // 4. removal: air decays to exactly zero, noise is gone at once, ground stays a while then goes too
        EnvSource dump = EnvIndustrySiteSource(0, 0, 5, 1.0f); // landfill: ground pollution
        for (int h = 0; h < 48; ++h) e.Step({coal, dump}, calm, p);
        const int groundOn = e.ground.At(5, 5);
        e.Step({}, calm, p);
        CHECK(e.noise.At(5, 5) == 0);
        for (int h = 0; h < 72; ++h) e.Step({}, calm, p);
        CHECK(EnvFieldSum(e.air) == 0);
        CHECK(e.ground.At(5, 5) > 0 && e.ground.At(5, 5) < groundOn);
        for (int h = 0; h < 2000; ++h) e.Step({}, calm, p);
        CHECK(EnvFieldSum(e.ground) == 0);
        std::printf("removal: ground %d at the landfill, gone after 2000 h\n", groundOn);
    }
    // 5. wind: an east wind (towards +X) carries the plume east
    {
        EnvWeather wind = calm;
        wind.windMs = 12.0f, wind.windDir = 0, wind.windX = 1, wind.windZ = 0;
        EnvPollution e;
        for (int h = 0; h < 24; ++h) e.Step({coal}, wind, p);
        const int east = e.air.At(5 + 192, 5), west = e.air.At(5 - 192, 5);
        std::printf("wind: east %d west %d\n", east, west);
        CHECK(east > west * 2);
    }
    // 6. determinism: two runs with the same sources give the same fields
    {
        std::vector<EnvSource> src = {coal, EnvRoadSource(300, 40, 1200, 0.7f), EnvIndustryLotSource(-200, 100, 2, 24)};
        src.push_back(EnvUtilitySource(400, -300, 13)); // sewage outlet
        EnvWeather w = calm;
        w.windMs = 7, w.windX = 1, w.windZ = 1;
        EnvPollution a, b;
        for (int h = 0; h < 50; ++h) a.Step(src, w, p), b.Step(src, w, p);
        CHECK(Hash(a.air) == Hash(b.air) && Hash(a.water) == Hash(b.water) && Hash(a.noise) == Hash(b.noise) && Hash(a.ground) == Hash(b.ground));
        CHECK(EnvFieldSum(a.water) > 0);
        std::printf("determinism: air %016llx water %016llx\n", (unsigned long long)Hash(a.air), (unsigned long long)Hash(a.water));
    }
    std::printf(g_fail ? "sim_env_pollution_test: %d FAILED\n" : "sim_env_pollution_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
