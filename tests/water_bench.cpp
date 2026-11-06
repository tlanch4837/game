// Benchmark: sparse "virtual pipes" shallow-water simulation on a coarse grid with sleeping tiles.
// Question: can CS2-style flowing water (sources, flooding, terrain coupling) be simulated on a 20 km map for a small CPU budget?
// Single thread, desktop CPU, CPU only. Terrain is the engine's noise (copied from src/terrain.h) sampled at the water cell size.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

static float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
static float Hash(int x, int z, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000;
}
static float ValueNoise(float x, float z, int seed) {
    const int ix = (int)std::floor(x), iz = (int)std::floor(z);
    const float fx = Smooth(x - (float)ix), fz = Smooth(z - (float)iz);
    const float a = Hash(ix, iz, seed), b = Hash(ix + 1, iz, seed), c = Hash(ix, iz + 1, seed), d = Hash(ix + 1, iz + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}
static float NoiseHeight(float x, float z) {
    const float d = std::sqrt(x * x + z * z);
    const float mask = Smooth((d - 30.0f) / 70.0f);
    float sum = 0, amp = 1, total = 0, freq = 1.0f / 260.0f;
    for (int o = 0; o < 5; ++o) { sum += amp * ValueNoise(x * freq, z * freq, 1337 + o * 101); total += amp; amp *= 0.5f; freq *= 2.0f; }
    return ((sum / total - 0.5f) * 2.0f) * 40.0f * mask;
}

constexpr float kWorld = 20000.0f, kCell = 8.0f;
constexpr int kN = (int)(kWorld / kCell);   // 2500 cells per axis
constexpr int kT = 32;                      // cells per tile edge (256 m)
constexpr int kTN = (kN + kT - 1) / kT;     // tiles per axis

struct Water {
    std::vector<float> ground, depth, qL, qR, qU, qD;   // dense for the benchmark; real design allocates per tile
    std::vector<uint8_t> active;                        // per tile
    std::vector<uint8_t> next;
    Water() : ground(kN * kN), depth(kN * kN, 0.0f), qL(kN * kN, 0.0f), qR(kN * kN, 0.0f), qU(kN * kN, 0.0f), qD(kN * kN, 0.0f), active(kTN * kTN, 0), next(kTN * kTN, 0) {}

    void Wake(int tx, int tz) {
        for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
            const int x = tx + dx, z = tz + dz;
            if (x >= 0 && z >= 0 && x < kTN && z < kTN) next[z * kTN + x] = 1;
        }
    }

    // One step over active tiles only. Returns active tile count.
    int Step(float dt, float seaLevel) {
        constexpr float g = 9.81f, l = kCell;
        std::fill(next.begin(), next.end(), 0);
        int count = 0;
        for (int tz = 0; tz < kTN; ++tz) for (int tx = 0; tx < kTN; ++tx) {
            if (!active[tz * kTN + tx]) continue;
            ++count;
            const int x0 = tx * kT, z0 = tz * kT, x1 = std::min(x0 + kT, kN), z1 = std::min(z0 + kT, kN);
            // 1) update flows from surface height differences
            for (int z = z0; z < z1; ++z) for (int x = x0; x < x1; ++x) {
                const int i = z * kN + x;
                const float s = ground[i] + depth[i];
                auto upd = [&](float& q, int xn, int zn) {
                    float sn;
                    if (xn < 0 || zn < 0 || xn >= kN || zn >= kN) sn = seaLevel; // map edge is open sea
                    else sn = ground[zn * kN + xn] + depth[zn * kN + xn];
                    q = std::max(0.0f, q * 0.995f + dt * g * (s - sn));
                };
                upd(qL[i], x - 1, z); upd(qR[i], x + 1, z); upd(qU[i], x, z - 1); upd(qD[i], x, z + 1);
                const float out = (qL[i] + qR[i] + qU[i] + qD[i]) * dt;
                const float avail = depth[i] * l; // volume per unit width
                if (out > avail && out > 0) { const float k = avail / out; qL[i] *= k; qR[i] *= k; qU[i] *= k; qD[i] *= k; }
            }
            // 2) update depth from net flow
            bool anyWet = false;
            for (int z = z0; z < z1; ++z) for (int x = x0; x < x1; ++x) {
                const int i = z * kN + x;
                float in = 0;
                if (x > 0) in += qR[i - 1];
                if (x + 1 < kN) in += qL[i + 1];
                if (z > 0) in += qD[i - kN];
                if (z + 1 < kN) in += qU[i + kN];
                const float out = qL[i] + qR[i] + qU[i] + qD[i];
                depth[i] = std::max(0.0f, depth[i] + (in - out) * dt / l);
                if (depth[i] > 1e-3f) anyWet = true;
            }
            if (anyWet) Wake(tx, tz); // stays awake and wakes its neighbours; dry tiles fall asleep
        }
        active.swap(next);
        return count;
    }
};

int main() {
    printf("Water benchmark: %.0f km map, %.0f m cells (%d x %d = %.2f M cells), %d-cell tiles (%d m)\n", kWorld / 1000, kCell, kN, kN, (double)kN * kN / 1e6, kT, (int)(kT * kCell));
    Water w;
    auto t0 = Clock::now();
    for (int z = 0; z < kN; ++z) for (int x = 0; x < kN; ++x) w.ground[z * kN + x] = NoiseHeight(x * kCell - kWorld * 0.5f, z * kCell - kWorld * 0.5f) - 5.0f;
    printf("terrain sample for water grid: %.0f ms (one time)\n", Ms(t0, Clock::now()));

    const float seaLevel = -30.0f; // map edge drains here
    // A river source in the middle plus two more: constant inflow, run a long time and watch the active set grow.
    struct Src { int x, z; float rate; };
    const Src srcs[] = {{kN / 2, kN / 2, 80.0f}, {kN / 2 + 200, kN / 2 - 150, 60.0f}, {kN / 2 - 300, kN / 2 + 100, 60.0f}};
    const float dt = 0.5f;
    printf("\nScenario: 3 sources (m^3/s: 80, 60, 60) flowing over the noise terrain, dt = %.2f s\n", dt);
    printf("%8s %12s %14s %14s %14s\n", "step", "active tiles", "active cells", "ms/step", "ns/cell");
    double total = 0; int stepsDone = 0, peak = 0; double peakMs = 0;
    for (int s = 1; s <= 6000; ++s) {
        for (const Src& r : srcs) { w.depth[r.z * kN + r.x] += r.rate * dt / (kCell * kCell); w.Wake(r.x / kT, r.z / kT); }
        // sources inject into `next`; merge them for this step
        for (size_t i = 0; i < w.active.size(); ++i) w.active[i] |= w.next[i];
        const auto a = Clock::now();
        const int n = w.Step(dt, seaLevel);
        const double ms = Ms(a, Clock::now());
        total += ms; ++stepsDone; peak = std::max(peak, n); peakMs = std::max(peakMs, ms);
        if (s == 1 || s == 100 || s == 500 || s == 1000 || s == 2000 || s == 4000 || s == 6000)
            printf("%8d %12d %14d %14.3f %14.1f\n", s, n, n * kT * kT, ms, n ? ms * 1e6 / (n * (double)kT * kT) : 0.0);
    }
    double wet = 0; for (float d : w.depth) if (d > 0.01f) ++wet;
    printf("\nafter %d steps (%.0f simulated min): %.0f wet cells (%.2f km^2), peak %d active tiles, avg %.3f ms/step, worst %.3f ms\n",
           stepsDone, stepsDone * dt / 60.0, wet, wet * kCell * kCell / 1e6, peak, total / stepsDone, peakMs);
    const double tileBytes = (double)kT * kT * 5 * sizeof(float); // depth + 4 flows (ground is shared with terrain)
    printf("sparse water memory at peak: %d tiles x %.0f KB = %.1f MB (dense grid would be %.0f MB)\n", peak, tileBytes / 1024, peak * tileBytes / 1048576.0, (double)kN * kN * 5 * 4 / 1048576.0);
    printf("a 30 Hz sim needs <= 33 ms/step total; at 5 Hz (water is slow) <= 200 ms\n");
    return 0;
}
