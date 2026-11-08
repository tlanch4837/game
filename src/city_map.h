// The default city map (terrain mapKind 2), random from the terrain seed:
//   the northern half (z < 0, north is -z) is land with organic topology: a wandering, bay-and-cape coast, rolling hills
//   that rise toward the north; the southern half is open ocean. A gentle, flattened band runs east to west across the
//   whole map (CorridorZ) for the default highway / railway / power line (see city_game.inl).
// Height(x, z, seed) is the whole base terrain in metres, sea level IslandMap::kSeaLevel. x east, z south.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "island_map.h"
#include "render/renderer.h"

namespace tbd {

class CityMap {
public:
    static constexpr float kSeaLevel = IslandMap::kSeaLevel;
    static constexpr float kHalf = 4000.0f; // the map spans x in +-kHalf; the corridor crosses all of it

    // Centre line of the infrastructure corridor: z at x. Gently curving, always well inland.
    static float CorridorZ(float x, int seed) { return -1150.0f + 140.0f * std::sin(x / 1500.0f + (float)seed * 0.37f) + 60.0f * std::sin(x / 530.0f + 1.3f); }
    static float CorridorSlope(float x, int seed) { return 140.0f / 1500.0f * std::cos(x / 1500.0f + (float)seed * 0.37f) + 60.0f / 530.0f * std::cos(x / 530.0f + 1.3f); }

    // Positive on land (metres to the shore, roughly), negative at sea.
    static float Land(float x, float z, int seed) {
        const int s = seed * 7 + 3;
        const float bigX = (Fbm(x / 1700.0f, z / 1700.0f, s) - 0.5f) * 2.0f * 520.0f;  // capes and bays
        const float smallX = (Fbm(x / 420.0f + 11.0f, z / 420.0f + 5.0f, s + 31) - 0.5f) * 2.0f * 150.0f;
        return -z + bigX + smallX; // the coast sits near z = 0
    }
    // The hills' height scale of a new map: 1 is the original, hilly map (old saves keep it: city_game.inl's T record);
    // 0.3 leaves mostly buildable plains with a few soft rolling hills. The coast, the sea floor, the beach and the
    // corridor do not scale. TBD_TERRAIN_RELIEF=<0.05..2> overrides it for a new map (city_game.inl NewCityGame).
    static constexpr float kDefaultRelief = 0.3f;

    static float Height(float x, float z, int seed, float relief = 1.0f) {
        const int s = seed * 7 + 3;
        const float sd = Land(x, z, seed);
        if (sd <= 0.0f) {
            const float out = -sd;
            return -(Smooth(out / 500.0f) * 45.0f + Smooth((out - 500.0f) / 3200.0f) * 35.0f); // a shelf, then deeper water
        }
        const float ramp = Smooth(sd / 160.0f); // the beach
        const float inland = Smooth(sd / 2600.0f);
        const float hills = Fbm(x / 1300.0f, z / 1300.0f, s + 5);
        // relief 1 multiplies by exactly 1: the same floats as before the relief scale existed
        float h = 5.0f + relief * (24.0f * Fbm(x / 800.0f, z / 800.0f, s + 9)) + inland * (relief * (30.0f + 150.0f * hills * hills));
        h += relief * (9.0f * (Fbm(x / 230.0f, z / 230.0f, s + 77) - 0.5f)) * Smooth(sd / 300.0f);
        h *= ramp;
        // the corridor band is eased toward a smooth, low profile so track and highway lie easy on the land
        const float cz = CorridorZ(x, seed);
        const float w = 1.0f - Smooth((std::fabs(z - cz) - 120.0f) / 320.0f);
        if (w > 0.0f) {
            const float target = 12.0f + 10.0f * Fbm(x / 2600.0f, 0.5f, s + 21);
            h += (target - h) * w;
        }
        return h;
    }

    // The sea: one big flat quad.
    static void BuildWater(std::vector<Vertex>& sea) {
        const float e = 40000.0f, y = kSeaLevel;
        const Vec3 c = {0.05f, 0.22f, 0.34f};
        const Vec3 q[6] = {{-e, y, -e}, {-e, y, e}, {e, y, e}, {-e, y, -e}, {e, y, e}, {e, y, -e}};
        for (const Vec3& p : q) sea.push_back({p.x, p.y, p.z, 0.0f, 1.0f, 0.0f, c.x, c.y, c.z, 1.0f});
    }

private:
    static float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    static float Hash(int x, int z, int s) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)s * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000;
    }
    static float Noise(float x, float z, int s) {
        const int ix = (int)std::floor(x), iz = (int)std::floor(z);
        const float fx = Smooth(x - (float)ix), fz = Smooth(z - (float)iz);
        const float a = Hash(ix, iz, s), b = Hash(ix + 1, iz, s), c = Hash(ix, iz + 1, s), d = Hash(ix + 1, iz + 1, s);
        const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
        return top + (bot - top) * fz;
    }
    static float Fbm(float x, float z, int s) {
        float sum = 0, amp = 1, tot = 0;
        for (int o = 0; o < 4; ++o) {
            sum += amp * Noise(x, z, s + o * 17);
            tot += amp;
            amp *= 0.5f;
            x *= 2.0f, z *= 2.0f;
        }
        return sum / tot;
    }
};

} // namespace tbd
