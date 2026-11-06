// CPU-side texture data both graphics backends upload: the tiling cloud volume and material mip chains.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace tbd {

constexpr int kCloudNoiseSize = 64;

namespace cloud_detail {

inline float WorleyAt(float x, float y, float z, int cells) {
    const float fx = x * cells, fy = y * cells, fz = z * cells;
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float best = 9.0f;
    auto hash = [](int a, int b, int c, int k) {
        unsigned h = (unsigned)a * 73856093u ^ (unsigned)b * 19349663u ^ (unsigned)c * 83492791u ^ (unsigned)k * 2654435761u;
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        return (float)(h & 0xFFFFFF) / 16777215.0f;
    };
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                const int wx = (cx + cells) % cells, wy = (cy + cells) % cells, wz = (cz + cells) % cells;
                const float px = cx + hash(wx, wy, wz, 0) - fx, py = cy + hash(wx, wy, wz, 1) - fy, pz = cz + hash(wx, wy, wz, 2) - fz;
                best = std::min(best, px * px + py * py + pz * pz);
            }
    return 1.0f - std::min(1.0f, std::sqrt(best));
}

inline float ValueNoiseFbm(float x, float y, float z, int cells) {
    float sum = 0.0f, amp = 0.5f;
    auto hash = [](int a, int b, int c) {
        unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u + (unsigned)c * 2147483647u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
    };
    for (int o = 0; o < 4; ++o, cells *= 2, amp *= 0.5f) {
        const float fx = x * cells, fy = y * cells, fz = z * cells;
        const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
        auto sm = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
        const float tx = sm(fx - ix), ty = sm(fy - iy), tz = sm(fz - iz);
        float c[8];
        for (int k = 0; k < 8; ++k)
            c[k] = hash((ix + (k & 1)) % cells, (iy + ((k >> 1) & 1)) % cells, (iz + (k >> 2)) % cells);
        const float a = c[0] + (c[1] - c[0]) * tx, b = c[2] + (c[3] - c[2]) * tx;
        const float d = c[4] + (c[5] - c[4]) * tx, e = c[6] + (c[7] - c[6]) * tx;
        const float f = a + (b - a) * ty, g = d + (e - d) * ty;
        sum += amp * (f + (g - f) * tz);
    }
    return sum / 0.9375f;
}

} // namespace cloud_detail

// The 2D cloud data fields, after Schneider's Nubis data fields: tiling RGBA8 maps the shaders read in world XZ.
// R = coverage of the low layer, G = type (0 wispy stratus, 1 billowy cumulus), B = cloud top height, A = coverage of the high wisps.
// Procedural here; the format is what a painted map would use. The same bytes on every backend.
constexpr int kCloudFieldSize = 256;
inline std::vector<uint8_t> GenerateCloudField() {
    using namespace cloud_detail;
    constexpr int kN = kCloudFieldSize;
    std::vector<uint8_t> texels((size_t)kN * kN * 4);
    auto ramp = [](float a, float b, float v) { const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
    auto put = [](float v) { return (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f); };
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) {
            const float u = (x + 0.5f) / kN, v = (y + 0.5f) / kN;
            uint8_t* out = &texels[((size_t)y * kN + x) * 4];
            out[0] = put(ramp(0.40f, 0.62f, ValueNoiseFbm(u, v, 0.13f, 4)));
            out[1] = put(ramp(0.38f, 0.62f, ValueNoiseFbm(u, v, 0.37f, 2)));
            out[2] = put(ramp(0.36f, 0.66f, ValueNoiseFbm(u, v, 0.61f, 3)));
            out[3] = put(ramp(0.44f, 0.60f, ValueNoiseFbm(u, v, 0.83f, 2)));
        }
    return texels;
}

// RGBA8, z then y then x: R = Perlin-Worley, GBA = Worley at rising frequency. The same bytes on every backend.
inline std::vector<uint8_t> GenerateCloudNoise() {
    using namespace cloud_detail;
    constexpr int kN = kCloudNoiseSize;
    std::vector<uint8_t> voxels((size_t)kN * kN * kN * 4);
    auto put = [](float v) { return (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f); };
    for (int z = 0; z < kN; ++z)
        for (int y = 0; y < kN; ++y)
            for (int x = 0; x < kN; ++x) {
                const float u = (x + 0.5f) / kN, v = (y + 0.5f) / kN, w = (z + 0.5f) / kN;
                const float w4 = WorleyAt(u, v, w, 4), w8 = WorleyAt(u, v, w, 8), w16 = WorleyAt(u, v, w, 16);
                const float fbmW = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;
                const float pw = std::clamp(fbmW + (ValueNoiseFbm(u, v, w, 4) - (1.0f - fbmW)) * 0.6f, 0.0f, 1.0f);
                uint8_t* out = &voxels[((size_t)z * kN * kN + (size_t)y * kN + x) * 4];
                out[0] = put(pw);
                out[1] = put(fbmW);
                out[2] = put(w8 * 0.625f + w16 * 0.375f);
                out[3] = put(w16);
            }
    return voxels;
}

// 2x2 box filter down to 1x1; level 0 is a copy of the source.
struct MipChain {
    std::vector<std::vector<uint8_t>> levels;
    std::vector<std::pair<int, int>> dims;
    bool opaque = false; // set by whoever decoded it: every alpha at least 103 (see world_streaming's depth pre-pass test)
};
inline MipChain BuildMipChain(const uint8_t* rgba, int w, int h) {
    MipChain c;
    c.levels.emplace_back(rgba, rgba + (size_t)w * h * 4);
    c.dims.push_back({w, h});
    while (c.dims.back().first > 1 || c.dims.back().second > 1) {
        const auto [pw, ph] = c.dims.back();
        const int nw = std::max(1, pw / 2), nh = std::max(1, ph / 2);
        std::vector<uint8_t> next((size_t)nw * nh * 4);
        const std::vector<uint8_t>& prev = c.levels.back();
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int k = 0; k < 4; ++k) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            sum += prev[((size_t)std::min(2 * y + dy, ph - 1) * pw + std::min(2 * x + dx, pw - 1)) * 4 + k];
                    next[((size_t)y * nw + x) * 4 + k] = (uint8_t)((sum + 2) / 4);
                }
        c.levels.push_back(std::move(next));
        c.dims.push_back({nw, nh});
    }
    return c;
}

} // namespace tbd
