// Eye look for the Character Creator (HEAD tab, EYES section): the eyeball texture (Assets\eyes\eye_colour.png, a front view: r = 1 at
// the silhouette, iris edge at r = 0.49, see gameenginetools\eye_atlas) re-made on the CPU from a few sliders, then swapped in for
// the eye parts' albedo like the baked skin. The defaults make brown eyes; irisAmount 0 and the rest 0 give the source picture back.
//   iris colour + amount   the iris takes the chosen colour but keeps the source's light and dark (fibres, pupil, limbal darkness)
//   iris brightness        gain on the iris
//   limbal ring            dark ring where the iris meets the white
//   redness                the whole sclera blushes, more toward the corners
//   corner redness         the pink toward both corners of the eye (one texture serves both eyes, so both corners get it)
//   veins                  procedural conjunctival vessels: wavy lines running in from the periphery, three scales, thinner and
//                          shorter as they go in, count and strength from the slider
//   yellowing              the white turns ivory
// Everything is a function of (r, angle) only, so it needs no UV unwrap or mesh.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tbd::character {

struct EyeLook {
    float irisColor[3] = {0.32f, 0.19f, 0.09f}; // 0..1 display colour the iris moves toward: brown by default
    float irisAmount = 0.9f, irisBright = 0.5f; // brightness 0.5 = unchanged, 0..1 = 0.5x .. 1.5x
    float limbal = 0.0f, redness = 0.0f, corner = 0.0f, veins = 0.0f, yellow = 0.0f;
    bool IsDefault() const { return irisAmount == 0 && irisBright == 0.5f && limbal == 0 && redness == 0 && corner == 0 && veins == 0 && yellow == 0; }
    bool operator==(const EyeLook& o) const {
        return irisColor[0] == o.irisColor[0] && irisColor[1] == o.irisColor[1] && irisColor[2] == o.irisColor[2] && irisAmount == o.irisAmount &&
               irisBright == o.irisBright && limbal == o.limbal && redness == o.redness && corner == o.corner && veins == o.veins && yellow == o.yellow;
    }
};

namespace eye_detail {
inline float Sm(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
inline uint32_t Hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
inline float Hash01(uint32_t a, uint32_t b) { return (Hash(a, b) & 0xFFFFFF) / 16777215.0f; }
inline float Noise1(float x, uint32_t seed) { // smooth value noise 0..1
    const float f = std::floor(x), t = x - f;
    const float a = Hash01((uint32_t)(int)f, seed), b = Hash01((uint32_t)(int)f + 1, seed), s = t * t * (3 - 2 * t);
    return a + (b - a) * s;
}
// 0..1: the vessel field at polar (angle in turns 0..1, radius r 0..1). `amount` sets how many strands exist and how strong they are.
inline float Veins(float turns, float r, float amount) {
    static const int count[3] = {13, 27, 53};
    static const float width[3] = {0.0042f, 0.0030f, 0.0020f};
    float best = 0;
    for (int L = 0; L < 3; ++L) {
        const float a = turns * (float)count[L];
        const int k0 = (int)std::floor(a + 0.5f);
        for (int dk = -1; dk <= 1; ++dk) { // the wobble moves a strand sideways, so look at the neighbours too
            const int k = k0 + dk;
            const uint32_t kk = (uint32_t)((k % count[L] + count[L]) % count[L]);
            const float h = Hash01(kk, (uint32_t)(L * 7 + 1));
            if (h > amount * (0.45f + 0.22f * L)) continue; // present
            const float h2 = Hash01(kk, (uint32_t)(L * 7 + 2));
            const float start = 0.53f + 0.17f * h2 + 0.05f * L; // how far in toward the iris it reaches
            const float wob = (Noise1(r * (6.0f + 5.0f * L) + h2 * 40.0f, kk + (uint32_t)(L * 131)) - 0.5f) * 0.8f
                            + (Noise1(r * 23.0f + h * 90.0f, kk + 977u) - 0.5f) * 0.18f;
            const float d = std::fabs(a - (float)k - wob);                       // strand units
            const float arc = d * 6.2831853f * std::max(r, 0.3f) / (float)count[L]; // radius units
            const float w = width[L] * (0.55f + 0.9f * Sm(start, start + 0.3f, r)); // thicker toward the edge
            const float line = 1.0f - Sm(w * 0.35f, w, arc);
            const float along = Sm(start, start + 0.07f, r) * (1.0f - Sm(0.93f, 0.99f, r));
            best = std::max(best, line * along * (0.55f + 0.45f * h2));
        }
    }
    return best;
}
} // namespace eye_detail

// base: the source picture (RGBA, bw x bh). out: n x n RGBA.
inline void RenderEye(const std::vector<uint8_t>& base, int bw, int bh, const EyeLook& k, int n, std::vector<uint8_t>& out) {
    using namespace eye_detail;
    out.assign((size_t)n * n * 4, 255);
    if (base.size() < (size_t)bw * bh * 4 || bw < 8 || bh < 8) return;
    const auto sample = [&](float u, float v, float c[3]) { // bilinear, display bytes
        const float x = std::clamp(u * bw - 0.5f, 0.0f, (float)bw - 1.001f), y = std::clamp(v * bh - 0.5f, 0.0f, (float)bh - 1.001f);
        const int x0 = (int)x, y0 = (int)y;
        const float fx = x - x0, fy = y - y0;
        for (int j = 0; j < 3; ++j) {
            const float a = base[((size_t)y0 * bw + x0) * 4 + j], b = base[((size_t)y0 * bw + x0 + 1) * 4 + j];
            const float c0 = base[((size_t)(y0 + 1) * bw + x0) * 4 + j], d = base[((size_t)(y0 + 1) * bw + x0 + 1) * 4 + j];
            c[j] = (a + (b - a) * fx) * (1 - fy) + (c0 + (d - c0) * fx) * fy;
        }
    };
    // the iris's average luminance in the source: the tint keeps each texel's light relative to it
    double sum = 0; int cnt = 0;
    for (int y = 0; y < bh; y += 4)
        for (int x = 0; x < bw; x += 4) {
            const float rr = std::hypot((x + 0.5f) / bw - 0.5f, (y + 0.5f) / bh - 0.5f) * 2.0f;
            if (rr > 0.22f && rr < 0.46f) { const uint8_t* p = &base[((size_t)y * bw + x) * 4]; sum += 0.30 * p[0] + 0.59 * p[1] + 0.11 * p[2]; ++cnt; }
        }
    const float irisAvg = cnt ? (float)(sum / cnt) : 100.0f;
    const bool neutral = k.IsDefault();
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / n, v = (y + 0.5f) / n, cx = u - 0.5f, cy = v - 0.5f;
            const float r = std::hypot(cx, cy) * 2.0f;
            float c[3];
            sample(u, v, c);
            if (!neutral) {
                const float iris = Sm(0.53f, 0.47f, r), sclera = 1.0f - iris;
                if (iris > 0) {
                    const float lum = 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];
                    const float gain = std::clamp(lum / irisAvg, 0.15f, 2.4f) * (0.5f + k.irisBright);
                    for (int j = 0; j < 3; ++j) {
                        const float tinted = k.irisColor[j] * 255.0f * gain;
                        c[j] = (c[j] + (tinted - c[j]) * k.irisAmount * iris) * (1.0f + (0.5f + k.irisBright - 1.0f) * iris * (1.0f - k.irisAmount));
                    }
                    const float ring = Sm(0.34f, 0.46f, r) * Sm(0.535f, 0.485f, r);
                    for (int j = 0; j < 3; ++j) c[j] *= 1.0f - k.limbal * 0.75f * ring;
                }
                if (sclera > 0.001f) {
                    const float side = Sm(0.45f, 0.95f, std::fabs(cx) * 2.0f);          // toward the corners
                    const float edge = Sm(0.55f, 1.0f, r);                              // toward the lids
                    for (int j = 0; j < 3; ++j) {
                        static const float ivory[3] = {238, 218, 176}, pink[3] = {214, 112, 104}, red[3] = {176, 46, 44};
                        c[j] += (ivory[j] * (c[j] / 235.0f) - c[j]) * k.yellow * 0.7f * sclera;
                        c[j] += (pink[j] * (c[j] / 235.0f) - c[j]) * k.redness * (0.35f + 0.65f * std::max(side, edge * 0.6f)) * 0.75f * sclera;
                        c[j] += (pink[j] * (c[j] / 235.0f) - c[j]) * k.corner * side * 0.95f * sclera;
                        (void)red;
                    }
                    if (k.veins > 0.001f) {
                        float turns = std::atan2(cy, cx) / 6.2831853f;
                        turns -= std::floor(turns);
                        const float vv = Veins(turns, r, k.veins) * std::clamp(0.35f + k.veins * 0.9f, 0.0f, 1.0f) * sclera;
                        static const float vein[3] = {168, 46, 48};
                        for (int j = 0; j < 3; ++j) c[j] += (vein[j] - c[j]) * vv * 0.62f;
                    }
                }
            }
            uint8_t* o = &out[((size_t)y * n + (size_t)x) * 4];
            for (int j = 0; j < 3; ++j) o[j] = (uint8_t)std::clamp(c[j], 0.0f, 255.0f);
            o[3] = 255;
        }
}

} // namespace tbd::character
