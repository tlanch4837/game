// Surface maps for world materials, made at load from what a material already has, on both backends.
// Output RGBA, 512x512 at most: R roughness scale (0.5 = the material's factor), G cavity (1 = open, dark in crevices),
// B 0 (tells mesh.hlsl this is not the neutral default, whose B is 1), A height (1 = top).
// Height integrates the normal map: a Poisson solve over a pyramid (Frankot and Chellappa 1988 pose the problem; multigrid here, no FFT).
// A material with no normal map takes height from its albedo's high-pass and gets a normal map derived from that.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace tbd::surface_maps {

struct Result {
    int size = 0;
    std::vector<uint8_t> surface; // RGBA
    std::vector<uint8_t> normal;  // RGBA; empty when the material had its own
};

inline std::vector<float> Resample(const uint8_t* rgba, int w, int h, int n, int channel) { // box, wrapped; channel 3 is luminance
    std::vector<float> out((size_t)n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int x0 = x * w / n, x1 = std::max(x0 + 1, (x + 1) * w / n), y0 = y * h / n, y1 = std::max(y0 + 1, (y + 1) * h / n);
            int sum = 0, count = 0;
            for (int v = y0; v < y1; ++v)
                for (int u = x0; u < x1; ++u) {
                    const uint8_t* p = rgba + 4 * ((size_t)(v % h) * w + (u % w));
                    sum += channel < 3 ? p[channel] : (54 * p[0] + 183 * p[1] + 19 * p[2]) / 256;
                    ++count;
                }
            out[(size_t)y * n + x] = (float)sum / (count * 255.0f);
        }
    return out;
}

// A wrapped box blur, run twice: close to a Gaussian, and its cost does not grow with the radius.
inline std::vector<float> Blur(const std::vector<float>& src, int n, int radius) {
    radius = std::min(radius, n / 2 - 1);
    std::vector<float> a = src, b(src.size());
    const float norm = 1.0f / (2 * radius + 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < n; ++y) {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += a[(size_t)y * n + ((k + n) % n)];
            for (int x = 0; x < n; ++x) {
                b[(size_t)y * n + x] = sum * norm;
                sum += a[(size_t)y * n + ((x + radius + 1) % n)] - a[(size_t)y * n + ((x - radius + n) % n)];
            }
        }
        for (int x = 0; x < n; ++x) {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += b[(size_t)((k + n) % n) * n + x];
            for (int y = 0; y < n; ++y) {
                a[(size_t)y * n + x] = sum * norm;
                sum += b[(size_t)((y + radius + 1) % n) * n + x] - b[(size_t)((y - radius + n) % n) * n + x];
            }
        }
    }
    return a;
}

// Solves laplacian(h) = div(g), coarse to fine; g is in height per texel.
inline std::vector<float> Integrate(std::vector<float> gx, std::vector<float> gy, int n) {
    std::vector<float> h;
    if (n > 8) {
        const int m = n / 2;
        std::vector<float> cx((size_t)m * m), cy((size_t)m * m);
        for (int y = 0; y < m; ++y)
            for (int x = 0; x < m; ++x) {
                const size_t a = (size_t)2 * y * n + 2 * x, b = a + 1, c = a + n, d = c + 1;
                cx[(size_t)y * m + x] = (gx[a] + gx[b] + gx[c] + gx[d]) * 0.5f; // a coarse texel is two wide: twice the rise
                cy[(size_t)y * m + x] = (gy[a] + gy[b] + gy[c] + gy[d]) * 0.5f;
            }
        const std::vector<float> coarse = Integrate(std::move(cx), std::move(cy), m);
        h.resize((size_t)n * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) h[(size_t)y * n + x] = coarse[(size_t)(y / 2) * m + x / 2] * 2.0f;
    } else {
        h.assign((size_t)n * n, 0.0f);
    }
    std::vector<float> div((size_t)n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            div[(size_t)y * n + x] = gx[(size_t)y * n + x] - gx[(size_t)y * n + (x + n - 1) % n] + gy[(size_t)y * n + x] - gy[(size_t)((y + n - 1) % n) * n + x];
    for (int it = 0; it < 12; ++it) // red-black Gauss-Seidel
        for (int color = 0; color < 2; ++color)
            for (int y = 0; y < n; ++y)
                for (int x = (y + color) & 1; x < n; x += 2) {
                    const float s = h[(size_t)y * n + (x + 1) % n] + h[(size_t)y * n + (x + n - 1) % n] + h[(size_t)((y + 1) % n) * n + x] + h[(size_t)((y + n - 1) % n) * n + x];
                    h[(size_t)y * n + x] = (s - div[(size_t)y * n + x]) * 0.25f;
                }
    return h;
}

inline void Normalize(std::vector<float>& h) { // the 2nd to 98th percentile becomes 0..1
    std::vector<float> s = h;
    const size_t lo = s.size() / 50, hi = s.size() - 1 - lo;
    std::nth_element(s.begin(), s.begin() + lo, s.end());
    const float a = s[lo];
    std::nth_element(s.begin(), s.begin() + hi, s.end());
    const float scale = 1.0f / std::max(s[hi] - a, 1e-5f);
    for (float& v : h) v = std::clamp((v - a) * scale, 0.0f, 1.0f);
}

// The material's own metalness, baked occlusion and emissive, as they come from a glTF (see SetSurfaceExtra in renderer.h). Encoded image
// bytes, copied so the model may go away; any may be empty.
struct ExtraInputs {
    std::shared_ptr<std::vector<uint8_t>> rough, occlusion, emissive; // metallicRoughnessTexture (B = metal), occlusionTexture (R), emissiveTexture
    float emissiveFactor[3] = {0, 0, 0}, metallic = 0;
    bool Any() const { return (rough && metallic > 0.0f) || occlusion || emissive || emissiveFactor[0] + emissiveFactor[1] + emissiveFactor[2] > 0.0f; }
};

// RGBA n x n: R occlusion (1 = open), G 1 (full weathering), B metalness, A emissive strength (its square times a scale lights the albedo).
// `images` are decoded RGBA; a missing one leaves its channel neutral. Returns empty when the maps say nothing.
struct DecodedImage { std::vector<uint8_t> rgba; int w = 0, h = 0; };
inline std::vector<uint8_t> MakeExtra(const DecodedImage* rough, const DecodedImage* occlusion, const DecodedImage* emissive, const float emissiveFactor[3], float metallic, int& sizeOut) {
    int source = 1;
    for (const DecodedImage* d : {rough, occlusion, emissive}) if (d) source = std::max(source, std::max(d->w, d->h));
    int n = 512;
    while (n > source && n > 16) n /= 2;
    sizeOut = n;
    std::vector<uint8_t> out((size_t)n * n * 4, 255);
    std::vector<float> metal, ao, e0, e1, e2;
    if (rough && metallic > 0.0f) metal = Resample(rough->rgba.data(), rough->w, rough->h, n, 2);
    if (occlusion) ao = Resample(occlusion->rgba.data(), occlusion->w, occlusion->h, n, 0);
    if (emissive) { e0 = Resample(emissive->rgba.data(), emissive->w, emissive->h, n, 0); e1 = Resample(emissive->rgba.data(), emissive->w, emissive->h, n, 1); e2 = Resample(emissive->rgba.data(), emissive->w, emissive->h, n, 2); }
    const float flat = std::max(emissiveFactor[0], std::max(emissiveFactor[1], emissiveFactor[2]));
    bool neutral = true;
    for (size_t k = 0; k < (size_t)n * n; ++k) {
        float emis = 0.0f;
        if (emissive) emis = std::max(e0[k] * emissiveFactor[0], std::max(e1[k] * emissiveFactor[1], e2[k] * emissiveFactor[2]));
        else if (flat > 0.0f) emis = flat;
        uint8_t* p = &out[k * 4];
        p[0] = occlusion ? (uint8_t)std::lround(std::clamp(ao[k], 0.0f, 1.0f) * 255.0f) : 255;
        p[1] = 255;
        p[2] = metal.empty() ? 255 : (uint8_t)std::lround(std::clamp(metal[k], 0.0f, 1.0f) * 255.0f);
        p[3] = (uint8_t)std::lround(std::sqrt(std::clamp(emis, 0.0f, 1.0f)) * 255.0f);
        neutral = neutral && p[0] >= 250 && p[2] >= 250 && p[3] == 0;
    }
    if (neutral) out.clear();
    return out;
}

// normalRgba and roughRgba may be null. Roughness is read from G, the glTF metallic-roughness layout.
inline Result Make(const uint8_t* albedoRgba, int aw, int ah, const uint8_t* normalRgba, int nw, int nh, const uint8_t* roughRgba, int rw, int rh) {
    Result r;
    const int source = normalRgba ? nw : aw;
    int n = 512;
    while (n > source && n > 16) n /= 2;
    r.size = n;
    const std::vector<float> lum = Resample(albedoRgba, aw, ah, n, 3);
    std::vector<float> height;
    if (normalRgba) {
        const std::vector<float> nx = Resample(normalRgba, nw, nh, n, 0), ny = Resample(normalRgba, nw, nh, n, 1), nz = Resample(normalRgba, nw, nh, n, 2);
        std::vector<float> gx((size_t)n * n), gy((size_t)n * n);
        for (size_t k = 0; k < gx.size(); ++k) {
            const float z = std::max(nz[k] * 2.0f - 1.0f, 0.2f);
            gx[k] = -(nx[k] * 2.0f - 1.0f) / z;
            gy[k] = (ny[k] * 2.0f - 1.0f) / z; // +Y up in the map, image rows run down
        }
        height = Integrate(std::move(gx), std::move(gy), n);
        const std::vector<float> wide = Blur(height, n, n / 8); // planks and stones, not the tile's slow drift
        for (size_t k = 0; k < height.size(); ++k) height[k] -= wide[k];
    } else {
        const std::vector<float> wide = Blur(lum, n, n / 16);
        height.resize(lum.size());
        for (size_t k = 0; k < lum.size(); ++k) height[k] = lum[k] - wide[k];
    }
    Normalize(height);
    const std::vector<float> around = Blur(height, n, std::max(n / 64, 2));
    const std::vector<float> rough = roughRgba ? Resample(roughRgba, rw, rh, n, 1) : std::vector<float>();
    const std::vector<float> lumWide = Blur(lum, n, std::max(n / 32, 2));
    r.surface.resize((size_t)n * n * 4);
    for (size_t k = 0; k < height.size(); ++k) {
        const float roughScale = roughRgba ? rough[k] * 0.5f : 0.5f + std::clamp((lumWide[k] - lum[k]) * 0.5f, -0.1f, 0.1f); // duller where darker
        const float cavity = std::clamp(1.0f - (around[k] - height[k]) * 3.0f, 0.0f, 1.0f);
        uint8_t* p = r.surface.data() + k * 4;
        p[0] = (uint8_t)std::lround(std::clamp(roughScale, 0.0f, 1.0f) * 255.0f);
        p[1] = (uint8_t)std::lround(cavity * 255.0f);
        p[2] = 0;
        p[3] = (uint8_t)std::lround(height[k] * 255.0f);
    }
    if (!normalRgba) {
        r.normal.resize((size_t)n * n * 4);
        const float strength = 1.25f * n / 256.0f;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float dx = height[(size_t)y * n + (x + 1) % n] - height[(size_t)y * n + (x + n - 1) % n];
                const float dy = height[(size_t)((y + 1) % n) * n + x] - height[(size_t)((y + n - 1) % n) * n + x];
                const float vx = -dx * strength, vy = dy * strength, len = 1.0f / std::sqrt(vx * vx + vy * vy + 1.0f);
                uint8_t* p = r.normal.data() + ((size_t)y * n + x) * 4;
                p[0] = (uint8_t)std::lround((vx * len * 0.5f + 0.5f) * 255.0f);
                p[1] = (uint8_t)std::lround((vy * len * 0.5f + 0.5f) * 255.0f);
                p[2] = (uint8_t)std::lround((len * 0.5f + 0.5f) * 255.0f);
                p[3] = 255;
            }
    }
    return r;
}

} // namespace tbd::surface_maps
