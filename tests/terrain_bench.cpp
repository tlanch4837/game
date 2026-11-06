// Benchmark: sparse base+edit terrain tiles vs a dense 20 km x 20 km heightmap, and vs the engine's current per-call noise.
// Standalone (no renderer). The noise below is copied from src/terrain.h so the "current" numbers are for the same function.
// Single thread. Build: see tests/terrain_bench.bat
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// ---- current engine height function (copied from src/terrain.h) ----
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
    const int seed = 1337; const float amplitude = 40.0f, frequency = 1.0f / 260.0f;
    const float d = std::sqrt(x * x + z * z);
    const float mask = Smooth((d - 30.0f) / 70.0f);
    float sum = 0, amp = 1, total = 0, freq = frequency;
    for (int o = 0; o < 5; ++o) { sum += amp * ValueNoise(x * freq, z * freq, seed + o * 101); total += amp; amp *= 0.5f; freq *= 2.0f; }
    const float n = (sum / total - 0.5f) * 2.0f;
    const float rangeMask = Smooth((d - 500.0f) / 2500.0f);
    float ridge = 0.0f;
    if (rangeMask > 0.0f) {
        float a = 1.0f, f = 1.0f / 2600.0f;
        for (int o = 0; o < 4; ++o, a *= 0.48f, f *= 2.1f) {
            const float r = 1.0f - std::fabs(ValueNoise(x * f, z * f, seed + 500 + o) * 2.0f - 1.0f);
            ridge += a * r * r;
        }
    }
    return n * amplitude * mask + ridge * 900.0f * rangeMask - 0.04f;
}

// ---- sparse design ----
constexpr float kWorld = 20000.0f, kCell = 2.0f;
constexpr int kTileN = 128;                       // samples per tile edge
constexpr float kTileM = kTileN * kCell;          // 256 m
constexpr float kQuant = 0.02f;                   // metres per delta unit
constexpr int kTilesPerAxis = (int)(kWorld / kTileM) + 1;

struct EditTile { std::vector<int16_t> d = std::vector<int16_t>(kTileN * kTileN, 0); };
struct BaseTile { std::vector<float> h; };
static uint64_t Key(int tx, int tz) { return ((uint64_t)(uint32_t)tx << 32) | (uint32_t)tz; }

struct Field {
    std::unordered_map<uint64_t, EditTile> edits;
    std::unordered_map<uint64_t, BaseTile> base;
    double baseGenMs = 0; int baseGens = 0;

    const BaseTile& Base(int tx, int tz) {
        auto it = base.find(Key(tx, tz));
        if (it != base.end()) return it->second;
        const auto t0 = Clock::now();
        BaseTile b; b.h.resize((kTileN + 1) * (kTileN + 1)); // +1 overlap so bilinear never leaves the tile
        const float ox = (float)tx * kTileM - kWorld * 0.5f, oz = (float)tz * kTileM - kWorld * 0.5f;
        for (int j = 0; j <= kTileN; ++j) for (int i = 0; i <= kTileN; ++i) b.h[j * (kTileN + 1) + i] = NoiseHeight(ox + i * kCell, oz + j * kCell);
        baseGenMs += Ms(t0, Clock::now()); ++baseGens;
        return base.emplace(Key(tx, tz), std::move(b)).first->second;
    }
    // Bilinear height at world position. Edit lookup is a hash probe; untouched tiles cost nothing.
    float HeightAt(float x, float z) {
        const float gx = (x + kWorld * 0.5f) / kCell, gz = (z + kWorld * 0.5f) / kCell;
        const int ix = (int)std::floor(gx), iz = (int)std::floor(gz);
        const float fx = gx - ix, fz = gz - iz;
        const int tx = ix / kTileN, tz = iz / kTileN, lx = ix % kTileN, lz = iz % kTileN;
        const BaseTile& b = Base(tx, tz);
        const int s = kTileN + 1;
        const float h00 = b.h[lz * s + lx], h10 = b.h[lz * s + lx + 1], h01 = b.h[(lz + 1) * s + lx], h11 = b.h[(lz + 1) * s + lx + 1];
        float h = (h00 + (h10 - h00) * fx) + ((h01 + (h11 - h01) * fx) - (h00 + (h10 - h00) * fx)) * fz;
        auto e = edits.find(Key(tx, tz));
        if (e != edits.end() && lx + 1 < kTileN && lz + 1 < kTileN) {
            const int16_t* d = e->second.d.data();
            const float a = d[lz * kTileN + lx], bb = d[lz * kTileN + lx + 1], c = d[(lz + 1) * kTileN + lx], dd = d[(lz + 1) * kTileN + lx + 1];
            h += ((a + (bb - a) * fx) + ((c + (dd - c) * fx) - (a + (bb - a) * fx)) * fz) * kQuant;
        }
        return h;
    }
    EditTile& Edit(int tx, int tz) { return edits[Key(tx, tz)]; }
};

// Dirty-tile rebuild: what the renderer needs after an edit (heights, normals, 4 mip levels of min/max).
static double RebuildTile(Field& f, int tx, int tz, std::vector<float>& heights, std::vector<int8_t>& normals, std::vector<float>& mips) {
    const auto t0 = Clock::now();
    const BaseTile& b = f.Base(tx, tz);
    const EditTile& e = f.Edit(tx, tz);
    heights.resize(kTileN * kTileN);
    for (int j = 0; j < kTileN; ++j) for (int i = 0; i < kTileN; ++i) heights[j * kTileN + i] = b.h[j * (kTileN + 1) + i] + e.d[j * kTileN + i] * kQuant;
    normals.resize(kTileN * kTileN * 2);
    for (int j = 1; j < kTileN - 1; ++j) for (int i = 1; i < kTileN - 1; ++i) {
        const float dx = heights[j * kTileN + i - 1] - heights[j * kTileN + i + 1], dz = heights[(j - 1) * kTileN + i] - heights[(j + 1) * kTileN + i];
        const float inv = 1.0f / std::sqrt(dx * dx + dz * dz + 4.0f * kCell * kCell);
        normals[(j * kTileN + i) * 2] = (int8_t)(dx * inv * 127.0f); normals[(j * kTileN + i) * 2 + 1] = (int8_t)(dz * inv * 127.0f);
    }
    mips.clear();
    std::vector<float> lo = heights, hi = heights; int n = kTileN;
    for (int lvl = 0; lvl < 4; ++lvl) {
        const int m = n / 2; std::vector<float> nlo(m * m), nhi(m * m);
        for (int j = 0; j < m; ++j) for (int i = 0; i < m; ++i) {
            float l = 1e30f, h = -1e30f;
            for (int dj = 0; dj < 2; ++dj) for (int di = 0; di < 2; ++di) { l = std::min(l, lo[(2 * j + dj) * n + 2 * i + di]); h = std::max(h, hi[(2 * j + dj) * n + 2 * i + di]); }
            nlo[j * m + i] = l; nhi[j * m + i] = h;
        }
        mips.insert(mips.end(), nlo.begin(), nlo.end()); mips.insert(mips.end(), nhi.begin(), nhi.end());
        lo.swap(nlo); hi.swap(nhi); n = m;
    }
    return Ms(t0, Clock::now());
}

static void Brush(Field& f, float cx, float cz, float radius, float strengthM, std::vector<uint64_t>& touched) {
    const int i0 = (int)std::floor((cx - radius + kWorld * 0.5f) / kCell), i1 = (int)std::floor((cx + radius + kWorld * 0.5f) / kCell);
    const int j0 = (int)std::floor((cz - radius + kWorld * 0.5f) / kCell), j1 = (int)std::floor((cz + radius + kWorld * 0.5f) / kCell);
    touched.clear();
    for (int j = j0; j <= j1; ++j) for (int i = i0; i <= i1; ++i) {
        if (i < 0 || j < 0) continue;
        const float x = i * kCell - kWorld * 0.5f - cx, z = j * kCell - kWorld * 0.5f - cz;
        const float t = (x * x + z * z) / (radius * radius);
        if (t >= 1.0f) continue;
        const float w = (1 - t) * (1 - t) * strengthM / kQuant;
        const int tx = i / kTileN, tz = j / kTileN;
        EditTile& e = f.Edit(tx, tz);
        int16_t& v = e.d[(j % kTileN) * kTileN + (i % kTileN)];
        v = (int16_t)std::clamp((int)std::lround(v + w), -32000, 32000);
        const uint64_t k = Key(tx, tz);
        if (std::find(touched.begin(), touched.end(), k) == touched.end()) touched.push_back(k);
    }
}

// Save size: zero-run RLE of int16 deltas (a simple stand-in; a real codec would do better).
static size_t RleSize(const EditTile& e) {
    size_t bytes = 4; size_t i = 0; const size_t n = e.d.size();
    while (i < n) {
        if (e.d[i] == 0) { size_t j = i; while (j < n && e.d[j] == 0) ++j; bytes += 3; i = j; }
        else { size_t j = i; while (j < n && e.d[j] != 0) ++j; bytes += 3 + (j - i) * 2; i = j; }
    }
    return bytes;
}

static void Session(const char* name, int strokes, float areaKm) {
    Field f;
    std::mt19937 rng(7); std::uniform_real_distribution<float> U(-0.5f, 0.5f), S(0.3f, 1.5f);
    std::vector<uint64_t> touched; std::vector<float> h, mips; std::vector<int8_t> nrm;
    double brushMs = 0, rebuildMs = 0, worstStroke = 0;
    for (int s = 0; s < strokes; ++s) {
        const float cx = U(rng) * areaKm * 1000.0f, cz = U(rng) * areaKm * 1000.0f;
        const auto t0 = Clock::now();
        Brush(f, cx, cz, 40.0f, S(rng), touched);
        const auto t1 = Clock::now();
        double rb = 0;
        for (uint64_t k : touched) rb += RebuildTile(f, (int)(k >> 32), (int)(k & 0xFFFFFFFFu), h, nrm, mips);
        brushMs += Ms(t0, t1); rebuildMs += rb;
        worstStroke = std::max(worstStroke, Ms(t0, t1) + rb);
    }
    size_t editBytes = f.edits.size() * sizeof(int16_t) * kTileN * kTileN, saveBytes = 0;
    for (auto& kv : f.edits) saveBytes += RleSize(kv.second);
    printf("\n[%s] %d strokes (radius 40 m) over %.0f x %.0f km\n", name, strokes, areaKm, areaKm);
    printf("  edited tiles          : %zu of %d (%.1f%% of world)\n", f.edits.size(), kTilesPerAxis * kTilesPerAxis, 100.0 * f.edits.size() / (kTilesPerAxis * kTilesPerAxis));
    printf("  resident edit memory  : %.1f MB   (dense 2 m grid: %.1f MB)\n", editBytes / 1048576.0, (kWorld / kCell) * (kWorld / kCell) * 2 / 1048576.0);
    printf("  save size (zero-RLE)  : %.1f MB\n", saveBytes / 1048576.0);
    printf("  stroke: apply %.3f ms avg + dirty rebuild %.3f ms avg ; worst stroke %.2f ms\n", brushMs / strokes, rebuildMs / strokes, worstStroke);
}

int main() {
    printf("Terrain benchmark: %.0f km world, %.0f m cells, %d-sample tiles (%.0f m)\n", kWorld / 1000, kCell, kTileN, kTileM);

    // 1. Cost of one height query: current engine (noise on every call) vs sparse tiles with a warm cache.
    constexpr int Q = 4000000;
    std::mt19937 rng(1); std::uniform_real_distribution<float> U(-3000.0f, 3000.0f); // 6 km around the origin: a city-sized working set
    std::vector<float> xs(Q), zs(Q);
    for (int i = 0; i < Q; ++i) { xs[i] = U(rng); zs[i] = U(rng); }
    volatile float sink = 0; double acc = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < Q; ++i) acc += NoiseHeight(xs[i], zs[i]);
    const double noiseMs = Ms(t0, Clock::now()); sink = (float)acc;

    Field f;
    for (int i = 0; i < 20000; ++i) f.HeightAt(xs[i], zs[i]); // warm: create base tiles
    // warm every tile of the region so the timed loop measures steady state, not first-touch
    for (float z = -3000; z < 3000; z += kTileM) for (float x = -3000; x < 3000; x += kTileM) f.HeightAt(x, z);
    acc = 0; t0 = Clock::now();
    for (int i = 0; i < Q; ++i) acc += f.HeightAt(xs[i], zs[i]);
    const double sparseMs = Ms(t0, Clock::now()); sink = (float)acc;
    printf("\n[query cost, %d random lookups in a 6 km area]\n", Q);
    printf("  current noise HeightAt : %.1f ns/query\n", noiseMs * 1e6 / Q);
    printf("  sparse tiles (warm)    : %.1f ns/query  (%.1fx faster)\n", sparseMs * 1e6 / Q, noiseMs / sparseMs);
    printf("  base tiles cached: %zu, generation avg %.2f ms/tile (%.2f MB resident)\n", f.base.size(), f.baseGenMs / std::max(1, f.baseGens),
           f.base.size() * (kTileN + 1.0) * (kTileN + 1.0) * 4 / 1048576.0);

    // 2. Dense bake cost: what a 20 km dense heightmap costs to fill once at startup (measured on a sample, scaled).
    const int sampleN = 2000000;
    t0 = Clock::now(); acc = 0;
    for (int i = 0; i < sampleN; ++i) acc += NoiseHeight(xs[i % Q], zs[(i * 7) % Q]);
    const double perSample = Ms(t0, Clock::now()) / sampleN; sink = (float)acc;
    const double denseSamples = (kWorld / kCell) * (kWorld / kCell);
    printf("\n[startup]\n  dense 2 m grid = %.0f M samples; filling it from noise ~ %.0f s single-thread (extrapolated from %d samples)\n", denseSamples / 1e6, perSample * denseSamples / 1000.0, sampleN);
    printf("  sparse: generates only the tiles that are looked at (%.2f ms each)\n", f.baseGenMs / std::max(1, f.baseGens));

    // 3. Edit sessions.
    Session("light session", 300, 3.0f);
    Session("city-building session", 3000, 5.0f);
    Session("whole-map stress", 30000, 20.0f);
    printf("\n(sink %f)\n", (double)sink);
    return 0;
}
