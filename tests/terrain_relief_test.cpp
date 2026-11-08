// Headless checks for the default city map's relief (src/city_map.h CityMap::Height, TerrainParams::relief):
//   the new default (CityMap::kDefaultRelief) is much flatter than the original map (relief 1): the land's height
//   range and its steepest and mean slopes are well below the old ones, most land is buildable; the coast, the sea floor
//   and the corridor do not move; relief 1 is the old map (what saves without a T record keep); Terrain::BaseHeightAt
//   follows params.relief. Prints the before / after numbers.
// Build and run: tests\terrain_relief_test.bat
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../src/terrain.h"

static int g_failed = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_failed; } \
    } while (0)

using tbd::CityMap;

struct Stats {
    float lo = 1e9f, hi = -1e9f; // land heights, metres
    float maxSlope = 0.0f;       // rise over run, 8 m baseline
    float maxInland = 0.0f;      // the same, away from the beach (more than 300 m from the coast): the hills themselves
    double meanSlope = 0.0;
    double buildable = 0.0;      // share of land samples flatter than 5 %
    int land = 0;
};

// The land of the map (x in +-kHalf, z from -kHalf to the coast), every 16 m, slopes over 8 m.
static Stats Measure(int seed, float relief) {
    Stats s;
    for (float z = -CityMap::kHalf; z <= 400.0f; z += 16.0f)
        for (float x = -CityMap::kHalf; x <= CityMap::kHalf; x += 16.0f) {
            if (CityMap::Land(x, z, seed) <= 0.0f) continue;
            const float h = CityMap::Height(x, z, seed, relief);
            const float gx = (CityMap::Height(x + 4, z, seed, relief) - CityMap::Height(x - 4, z, seed, relief)) / 8.0f;
            const float gz = (CityMap::Height(x, z + 4, seed, relief) - CityMap::Height(x, z - 4, seed, relief)) / 8.0f;
            const float sl = std::sqrt(gx * gx + gz * gz);
            s.lo = std::min(s.lo, h), s.hi = std::max(s.hi, h);
            s.maxSlope = std::max(s.maxSlope, sl);
            if (CityMap::Land(x, z, seed) > 300.0f) s.maxInland = std::max(s.maxInland, sl);
            s.meanSlope += sl;
            s.buildable += sl < 0.05f ? 1.0 : 0.0;
            ++s.land;
        }
    if (s.land) s.meanSlope /= s.land, s.buildable /= s.land;
    return s;
}

int main() {
    const float flat = CityMap::kDefaultRelief;
    if (flat < 0.2f || flat > 0.4f) CHECK(!"kDefaultRelief is outside 0.2 .. 0.4");
    for (int seed : {1337, 42, 777, 123456}) {
        const Stats was = Measure(seed, 1.0f), now = Measure(seed, flat);
        std::printf("seed %6d  land %.0f..%.0f m (range %.0f) -> %.0f..%.0f m (range %.0f); max slope %.1f%% -> %.1f%% (inland %.1f%% -> %.1f%%); "
                    "mean slope %.2f%% -> %.2f%%; buildable (<5%%) %.0f%% -> %.0f%%\n",
                    seed, was.lo, was.hi, was.hi - was.lo, now.lo, now.hi, now.hi - now.lo, was.maxSlope * 100, now.maxSlope * 100, was.maxInland * 100, now.maxInland * 100,
                    was.meanSlope * 100, now.meanSlope * 100, was.buildable * 100, now.buildable * 100);
        CHECK(was.land == now.land && now.land > 1000); // the coastline does not move
        CHECK(now.hi - now.lo <= 0.45f * (was.hi - was.lo));
        CHECK(now.maxSlope <= 0.5f * was.maxSlope);   // the steepest spot left is a beach where the coast wiggles
        CHECK(now.maxInland <= 0.35f * was.maxInland); // the hills: about relief times the old slopes
        CHECK(now.meanSlope <= 0.4 * was.meanSlope);
        CHECK(now.buildable >= 0.85 && now.buildable > was.buildable);
        CHECK(now.lo > 0.0f); // the land stays above the sea
    }
    // the sea, the beach line and the corridor do not scale
    const int seed = 1337;
    for (float x = -3000.0f; x <= 3000.0f; x += 500.0f) {
        CHECK(CityMap::Height(x, 2500.0f, seed, flat) == CityMap::Height(x, 2500.0f, seed, 1.0f)); // open sea
        const float cz = CityMap::CorridorZ(x, seed);
        CHECK(std::fabs(CityMap::Height(x, cz, seed, flat) - CityMap::Height(x, cz, seed, 1.0f)) < 0.01f); // the corridor band
    }
    // relief 1 is the default argument (the original map), and the terrain follows params.relief
    CHECK(CityMap::Height(130.0f, -2100.0f, seed) == CityMap::Height(130.0f, -2100.0f, seed, 1.0f));
    tbd::Terrain t;
    t.params.mapKind = 2, t.params.seed = seed;
    CHECK(t.params.relief == 1.0f); // a fresh TerrainParams (an old save) is the original map
    const float old = t.BaseHeightAt(130.0f, -2100.0f);
    t.params.relief = flat;
    CHECK(t.BaseHeightAt(130.0f, -2100.0f) == CityMap::Height(130.0f, -2100.0f, seed, flat));
    CHECK(old == CityMap::Height(130.0f, -2100.0f, seed, 1.0f));

    if (g_failed) {
        std::printf("terrain_relief_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("terrain_relief_test: all passed (relief %.2f)\n", flat);
    return 0;
}
