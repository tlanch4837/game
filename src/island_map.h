// A hand-shaped island for the terrain sandbox: roughly 7 km east-west and 10 km north-south, a rough echo of the GTA V map's
// outline (a wide north with a mountain, a river running down to the south coast, a few smaller hills), at its own scale.
// Height(x, z) is the whole base terrain in metres, sea level at 0; build the sea and river water with BuildWater.
// x is east, z is south (north is -z). The outline, peaks and river are tables below: edit them to reshape the island.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/renderer.h"

namespace tbd {

class IslandMap {
public:
    static constexpr float kSeaLevel = -0.25f; // the sea plane; the beach sits just above it

    // Ground height in metres (negative under the sea). Includes the river valley.
    static float Height(float x, float z) {
        const Data& d = D();
        float sd;
        const float h0 = BaseHeight(d, x, z, sd);
        if (sd <= 0.0f) return h0;
        float t, w, half, dist;
        if (!NearestRiver(d, x, z, t, w, half, dist)) return h0;
        const float reach = half * 3.5f;
        if (dist >= reach) return h0;
        const float m = (dist <= half ? 1.0f : 1.0f - Smooth((dist - half) / (half * 2.5f))) * Smooth(sd / 80.0f);
        return h0 + (w - 2.5f - h0) * m; // bed 2.5 m under the water surface, blending into the banks
    }

    // The sea (one big flat quad) and the river (a ribbon on the water surface), as lit triangles with vertex colour.
    static void BuildWater(std::vector<Vertex>& sea, std::vector<Vertex>& river) {
        const Data& d = D();
        const float e = 40000.0f, y = kSeaLevel;
        const Vec3 seaColor = {0.05f, 0.22f, 0.34f};
        Tri(sea, {-e, y, -e}, {-e, y, e}, {e, y, e}, seaColor);
        Tri(sea, {-e, y, -e}, {e, y, e}, {e, y, -e}, seaColor);
        const Vec3 riverColor = {0.10f, 0.30f, 0.42f};
        const size_t n = d.pts.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            float sd0, sd1;
            BaseHeight(d, d.pts[i].x, d.pts[i].z, sd0);
            BaseHeight(d, d.pts[i + 1].x, d.pts[i + 1].z, sd1);
            if (sd0 < 10.0f || sd1 < 10.0f) continue; // the ribbon stops at the shore
            Vec3 a0, a1, b0, b1;
            Edge(d, i, a0, a1);
            Edge(d, i + 1, b0, b1);
            Tri(river, a0, a1, b1, riverColor);
            Tri(river, a0, b1, b0, riverColor);
        }
    }

    // Where the river starts, for placing the camera or markers.
    static Vec3 RiverSource() { const Data& d = D(); return {d.pts.front().x, d.W.front(), d.pts.front().z}; }

private:
    struct P { float x, z; };
    struct Data {
        std::vector<P> outline;           // metres, x east / z south, in order
        std::vector<P> pts;               // dense river centreline, source to mouth
        std::vector<float> W, half, cum;  // water surface height, half width, arc length at each point
        float total = 0.0f;
        static constexpr int kGrid = 48;  // spatial hash for the river segments: 48 x 48 cells of 250 m over +-6 km
        std::vector<std::vector<int>> cells;
    };

    static float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    static float Hash(int x, int z, int s) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)s * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000;
    }
    static float Noise(float x, float z, int s) {
        const int ix = (int)std::floor(x), iz = (int)std::floor(z);
        const float fx = Smooth(x - (float)ix), fz = Smooth(z - (float)iz);
        const float a = Hash(ix, iz, s), b = Hash(ix + 1, iz, s), c = Hash(ix, iz + 1, s), dd = Hash(ix + 1, iz + 1, s);
        const float top = a + (b - a) * fx, bot = c + (dd - c) * fx;
        return top + (bot - top) * fz; // 0..1
    }
    static float Fbm(float x, float z, int s) {
        float sum = 0, amp = 1, tot = 0;
        for (int o = 0; o < 4; ++o) { sum += amp * Noise(x, z, s + o * 17); tot += amp; amp *= 0.5f; x *= 2.0f; z *= 2.0f; }
        return sum / tot;
    }

    // Distance from a point to a segment, with the clamped parameter along it.
    static float SegDist(float px, float pz, P a, P b, float& u) {
        const float dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
        u = len2 > 0.0f ? std::clamp(((px - a.x) * dx + (pz - a.z) * dz) / len2, 0.0f, 1.0f) : 0.0f;
        const float cx = a.x + dx * u - px, cz = a.z + dz * u - pz;
        return std::sqrt(cx * cx + cz * cz);
    }

    // Positive inside the coast (metres to the shore), negative outside.
    static float SignedCoast(const Data& d, float px, float pz) {
        float best = 1e30f;
        bool inside = false;
        const size_t n = d.outline.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const P a = d.outline[j], b = d.outline[i];
            float u;
            best = std::min(best, SegDist(px, pz, a, b, u));
            if (((a.z > pz) != (b.z > pz)) && (px < (b.x - a.x) * (pz - a.z) / (b.z - a.z) + a.x)) inside = !inside;
        }
        return inside ? best : -best;
    }

    static float BaseHeight(const Data& d, float x, float z, float& sdOut) {
        // A wobble on the query point makes the coast irregular instead of a polygon.
        const float wx = (Noise(x / 650.0f, z / 650.0f, 11) - 0.5f) * 2.0f * 260.0f;
        const float wz = (Noise(x / 650.0f + 37.0f, z / 650.0f + 91.0f, 23) - 0.5f) * 2.0f * 260.0f;
        const float sd = SignedCoast(d, x + wx, z + wz);
        sdOut = sd;
        if (sd <= 0.0f) {
            const float out = -sd;
            return -(Smooth(out / 600.0f) * 45.0f + Smooth((out - 600.0f) / 3400.0f) * 35.0f); // a shelf, then deeper water
        }
        const float land = Smooth(sd / 220.0f); // beach ramp
        float h = 14.0f + 38.0f * Fbm(x / 1100.0f, z / 1100.0f, 5) + 25.0f * Smooth(sd / 2500.0f);
        struct Peak { float x, z, amp, sigma; };
        static const Peak peaks[] = {
            {-400.0f, -2600.0f, 480.0f, 750.0f},  // the big mountain, north-centre
            {2300.0f, -4000.0f, 220.0f, 450.0f},  // a headland peak in the north-east
            {-300.0f, 300.0f, 110.0f, 650.0f},    // hills behind the city
            {2200.0f, 1500.0f, 60.0f, 600.0f},    // eastern mesa
            {-2300.0f, -1800.0f, 140.0f, 600.0f}, // western hills
        };
        for (const Peak& p : peaks) {
            const float dx = x - p.x, dz = z - p.z;
            const float g = std::exp(-0.5f * (dx * dx + dz * dz) / (p.sigma * p.sigma));
            h += p.amp * g * (0.8f + 0.4f * Noise(x / 260.0f, z / 260.0f, 41));
        }
        h += 14.0f * (Fbm(x / 280.0f, z / 280.0f, 77) - 0.5f) * Smooth(sd / 400.0f);
        return h * land;
    }

    static void Tri(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 color) {
        const float cy = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z); // y of (b-a)x(c-a)
        if (cy < 0.0f) std::swap(b, c); // face up
        for (const Vec3& p : {a, b, c}) out.push_back({p.x, p.y, p.z, 0.0f, 1.0f, 0.0f, color.x, color.y, color.z, 1.0f});
    }

    // Left and right edge of the river ribbon at dense point i, slightly wider than the channel so the banks tuck over it.
    static void Edge(const Data& d, size_t i, Vec3& l, Vec3& r) {
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < d.pts.size() ? i + 1 : i;
        float dx = d.pts[b].x - d.pts[a].x, dz = d.pts[b].z - d.pts[a].z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.0f) { dx /= len; dz /= len; }
        const float w = d.half[i] * 1.2f;
        l = {d.pts[i].x - dz * w, d.W[i], d.pts[i].z + dx * w};
        r = {d.pts[i].x + dz * w, d.W[i], d.pts[i].z - dx * w};
    }

    static bool NearestRiver(const Data& d, float x, float z, float& t, float& w, float& half, float& dist) {
        const int cx = (int)std::floor((x + 6000.0f) / 250.0f), cz = (int)std::floor((z + 6000.0f) / 250.0f);
        if (cx < 0 || cz < 0 || cx >= Data::kGrid || cz >= Data::kGrid) return false;
        const std::vector<int>& list = d.cells[(size_t)cz * Data::kGrid + cx];
        if (list.empty()) return false;
        float best = 1e30f, bu = 0.0f;
        int bi = -1;
        for (int i : list) {
            float u;
            const float dd = SegDist(x, z, d.pts[(size_t)i], d.pts[(size_t)i + 1], u);
            if (dd < best) { best = dd; bu = u; bi = i; }
        }
        if (bi < 0) return false;
        const size_t i = (size_t)bi;
        w = d.W[i] + (d.W[i + 1] - d.W[i]) * bu;
        half = d.half[i] + (d.half[i + 1] - d.half[i]) * bu;
        t = (d.cum[i] + (d.cum[i + 1] - d.cum[i]) * bu) / d.total;
        dist = best;
        return true;
    }

    static P CatmullRom(P p0, P p1, P p2, P p3, float t) {
        const float t2 = t * t, t3 = t2 * t;
        auto f = [&](float a, float b, float c, float dd) { return 0.5f * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - dd) * t2 + (-a + 3 * b - 3 * c + dd) * t3); };
        return {f(p0.x, p1.x, p2.x, p3.x), f(p0.z, p1.z, p2.z, p3.z)};
    }

    static const Data& D() {
        static const Data data = [] {
            Data d;
            // Coast, clockwise from the north-west, in km (east, north); stored as metres with z = -north.
            static const float coast[][2] = {
                {-3.2f, 3.8f}, {-2.4f, 4.6f}, {-1.2f, 4.9f}, {0.2f, 4.7f}, {1.4f, 4.9f}, {2.4f, 4.6f}, {3.2f, 4.0f},
                {3.4f, 2.8f}, {3.0f, 1.6f}, {3.3f, 0.4f}, {3.2f, -0.8f}, {2.8f, -1.9f}, {3.0f, -2.8f}, {2.6f, -3.6f},
                {2.0f, -4.2f}, {1.0f, -4.4f}, {0.2f, -4.9f}, {-0.8f, -4.6f}, {-1.6f, -4.0f}, {-2.4f, -3.4f},
                {-2.9f, -2.4f}, {-3.2f, -1.2f}, {-2.8f, 0.0f}, {-3.4f, 1.2f}, {-3.0f, 2.4f},
            };
            for (const auto& c : coast) d.outline.push_back({c[0] * 1000.0f, -c[1] * 1000.0f});
            // River: from the big mountain's south slope down to the south coast, in km (east, north).
            static const float river[][2] = {
                {-0.3f, 1.9f}, {-0.9f, 1.4f}, {-0.6f, 0.5f}, {0.1f, -0.1f}, {0.7f, -0.9f}, {0.4f, -1.7f},
                {0.9f, -2.5f}, {1.0f, -3.3f}, {0.7f, -4.0f}, {0.6f, -4.8f},
            };
            std::vector<P> ctl;
            for (const auto& c : river) ctl.push_back({c[0] * 1000.0f, -c[1] * 1000.0f});
            for (size_t i = 0; i + 1 < ctl.size(); ++i) { // ~30 m spacing
                const P p0 = ctl[i > 0 ? i - 1 : i], p1 = ctl[i], p2 = ctl[i + 1], p3 = ctl[i + 2 < ctl.size() ? i + 2 : i + 1];
                const float len = std::hypot(p2.x - p1.x, p2.z - p1.z);
                const int n = std::max(2, (int)std::ceil(len / 30.0f));
                for (int k = 0; k < n; ++k) d.pts.push_back(CatmullRom(p0, p1, p2, p3, (float)k / n));
            }
            d.pts.push_back(ctl.back());
            const size_t n = d.pts.size();
            d.cum.assign(n, 0.0f);
            for (size_t i = 1; i < n; ++i) d.cum[i] = d.cum[i - 1] + std::hypot(d.pts[i].x - d.pts[i - 1].x, d.pts[i].z - d.pts[i - 1].z);
            d.total = d.cum.back();
            // Water surface: follows the ground down, never rises, flattened a little. Dry-land height ignores the river itself.
            d.W.assign(n, 0.0f);
            float run = 1e30f;
            for (size_t i = 0; i < n; ++i) {
                float sd;
                run = std::min(run, BaseHeight(d, d.pts[i].x, d.pts[i].z, sd) - 0.8f);
                d.W[i] = std::max(run, 0.3f);
            }
            for (int pass = 0; pass < 6; ++pass) { // smooth, then keep it from rising
                std::vector<float> s = d.W;
                for (size_t i = 0; i < n; ++i) {
                    float sum = 0; int c = 0;
                    for (int k = -6; k <= 6; ++k) { const long j = (long)i + k; if (j >= 0 && j < (long)n) { sum += d.W[(size_t)j]; ++c; } }
                    s[i] = sum / (float)c;
                }
                for (size_t i = 1; i < n; ++i) s[i] = std::min(s[i], s[i - 1]);
                d.W = s;
            }
            d.half.assign(n, 0.0f);
            for (size_t i = 0; i < n; ++i) d.half[i] = 7.0f + 25.0f * std::pow(d.cum[i] / d.total, 0.8f);
            // Spatial hash: every segment goes into each 250 m cell its box (grown by the valley's reach) touches.
            d.cells.assign((size_t)Data::kGrid * Data::kGrid, {});
            for (size_t i = 0; i + 1 < n; ++i) {
                const float reach = std::max(d.half[i], d.half[i + 1]) * 3.5f + 5.0f;
                const float x0 = std::min(d.pts[i].x, d.pts[i + 1].x) - reach, x1 = std::max(d.pts[i].x, d.pts[i + 1].x) + reach;
                const float z0 = std::min(d.pts[i].z, d.pts[i + 1].z) - reach, z1 = std::max(d.pts[i].z, d.pts[i + 1].z) + reach;
                const int cx0 = std::max(0, (int)std::floor((x0 + 6000.0f) / 250.0f)), cx1 = std::min(Data::kGrid - 1, (int)std::floor((x1 + 6000.0f) / 250.0f));
                const int cz0 = std::max(0, (int)std::floor((z0 + 6000.0f) / 250.0f)), cz1 = std::min(Data::kGrid - 1, (int)std::floor((z1 + 6000.0f) / 250.0f));
                for (int cz = cz0; cz <= cz1; ++cz)
                    for (int cx = cx0; cx <= cx1; ++cx) d.cells[(size_t)cz * Data::kGrid + cx].push_back((int)i);
            }
            return d;
        }();
        return data;
    }
};

} // namespace tbd
