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

    // ---- [example map] map kind 3: the Bayside archipelago (city_example.inl). A big enclosed bay with a strait to the north,
    // three islands and two islets in it, hills on the mainland ring. Analytic, so the street generator can query it.
    static float Ell(float x, float z, float cx, float cz, float rx, float rz, float rot) {
        const float c = std::cos(rot), s = std::sin(rot), dx = x - cx, dz = z - cz;
        const float u = (dx * c + dz * s) / rx, v = (-dx * s + dz * c) / rz;
        return std::sqrt(u * u + v * v);
    }
    // Signed distance-ish (metres): positive on land. The generator asks which land mass a point is on.
    static float BayMainland(float x, float z, int seed) {
        const int s = seed * 7 + 3;
        const float wx = x + 110.0f * (Fbm(x / 520.0f, z / 520.0f, s) - 0.5f) * 2.0f, wz = z + 110.0f * (Fbm(x / 520.0f + 7.0f, z / 520.0f + 3.0f, s + 9) - 0.5f) * 2.0f;
        float sd = (Ell(wx, wz, 60.0f, -150.0f, 1750.0f, 1350.0f, 0.0f) - 1.0f) * 1300.0f; // outside the bay: land
        const float strait = (std::fabs(wx - 260.0f) - 250.0f - 90.0f * Smooth((-wz - 1000.0f) / 1200.0f)); // the channel to the north sea (negative = water)
        if (wz < -900.0f) sd = std::min(sd, strait);
        return sd;
    }
    static float BayIslands(float x, float z, int seed) {
        const int s = seed * 7 + 3;
        const float wx = x + 70.0f * (Fbm(x / 300.0f + 4.0f, z / 300.0f, s + 3) - 0.5f) * 2.0f, wz = z + 70.0f * (Fbm(x / 300.0f, z / 300.0f + 9.0f, s + 4) - 0.5f) * 2.0f;
        float sd = (1.0f - Ell(wx, wz, -120.0f, -880.0f, 400.0f, 230.0f, -0.25f)) * 230.0f;      // north island
        sd = std::max(sd, (1.0f - Ell(wx, wz, 160.0f, -170.0f, 560.0f, 330.0f, 0.35f)) * 330.0f); // central island (downtown)
        sd = std::max(sd, (1.0f - Ell(wx, wz, 800.0f, 500.0f, 270.0f, 190.0f, 0.5f)) * 190.0f);   // south-east island
        sd = std::max(sd, (1.0f - Ell(wx, wz, -700.0f, -650.0f, 160.0f, 110.0f, 0.2f)) * 110.0f); // west islet
        sd = std::max(sd, (1.0f - Ell(wx, wz, 900.0f, -950.0f, 110.0f, 90.0f, 0.0f)) * 90.0f);    // north-east islet
        return sd;
    }
    static float BayLand(float x, float z, int seed) { return std::max(BayMainland(x, z, seed), BayIslands(x, z, seed)); }
    // The two highways that enter the mainland from the map edge (east and west); the terrain is eased flat under them.
    static float BayHwyEastZ(float x) { return 450.0f + 120.0f * std::sin(x / 700.0f); }
    static float BayHwyWestZ(float x) { return -250.0f + 100.0f * std::sin(x / 650.0f + 1.0f); }
    static float BayHeight(float x, float z, int seed) {
        const int s = seed * 7 + 3;
        const float sdM = BayMainland(x, z, seed), sdI = BayIslands(x, z, seed), sd = std::max(sdM, sdI);
        if (sd <= 0.0f) {
            const float out = -sd;
            return -(Smooth(out / 400.0f) * 40.0f + Smooth((out - 400.0f) / 3000.0f) * 40.0f);
        }
        const float beach = Smooth(sd / 45.0f);
        float h;
        if (sdI >= sdM) { // an island: low and gentle, a soft dome inland
            h = 3.5f + 5.0f * Fbm(x / 260.0f, z / 260.0f, s + 11) + 9.0f * Smooth(sd / 260.0f) * Fbm(x / 600.0f, z / 600.0f, s + 13);
        } else { // the mainland ring: a flat shore strip, then hills that climb away from the bay
            const float hills = Fbm(x / 1100.0f, z / 1100.0f, s + 5);
            h = 4.0f + 5.0f * Fbm(x / 300.0f, z / 300.0f, s + 9) + Smooth((sd - 300.0f) / 1500.0f) * (25.0f + 120.0f * hills * hills);
            h += 7.0f * (Fbm(x / 160.0f, z / 160.0f, s + 77) - 0.5f) * Smooth(sd / 250.0f);
            for (int k = 0; k < 2; ++k) { // ease the highway corridors flat
                const float cz = k == 0 ? BayHwyEastZ(x) : BayHwyWestZ(x);
                const bool inx = k == 0 ? x > 1300.0f : x < -1300.0f;
                if (!inx) continue;
                const float w = 1.0f - Smooth((std::fabs(z - cz) - 70.0f) / 260.0f);
                if (w > 0.0f) h += (9.0f + 6.0f * Fbm(x / 1800.0f, 0.5f, s + 21) - h) * w;
            }
        }
        return h * beach; // meets the seabed's 0 at the waterline (no ledge)
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
