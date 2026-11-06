// Skin painting for the Character Creator (HEAD tab, SKIN PAINT section): three layers over the head's UV atlas, painted on the 3D head
// or in the 2D view with the same brush as the hair (radius, strength, hardness, symmetry).
//   TINT   RGBA8: a colour and how much of it. Types are skin looks (inner-corner pink, flushed, olive, under-eye shadow ...) so the
//          inner corner of the eye, a blush or a shadow is painted with the colour that belongs there. Composited over the skin albedo
//          keeping the texture's light and dark (the tint takes the base's luminance), so pores and detail stay.
//   SPOTS  a reveal mask plus a kind (freckles, moles, age spots). The spots themselves are a fixed procedural field (jittered cells
//          hashed from the texel, irregular edges), so painting only decides WHERE they show and dragging never piles spots up.
//   SHINE  0..1: written into the skin surface map's red channel (oil shine, see LitSkin in lighting.hlsli) as r * (1 - shine).
// The bake order is tint, spots, then the hair strands, then the feature overlay (creator.cpp BakeHair), so the rig-region rings
// and mesh edges always draw over the paint.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "hair_paint.h"

namespace tbd::character {

class SkinPaint {
public:
    static constexpr int N = HairPainter::N;
    // Tool ids share the creator's hairTool int: 1..4 are the hair tools, these start at 10.
    enum Tool { None = 0, TintPaint = 10, TintErase, SpotPaint, SpotErase, ShinePaint, ShineErase };
    static bool IsLayerTool(int t) { return t >= TintPaint && t <= ShineErase; }

    struct TintType { const wchar_t* name; uint8_t rgb[3]; };
    static constexpr int kTintTypes = 8;
    static const TintType& Tint(int i) {
        static const TintType t[kTintTypes] = {
            {L"Inner corner pink", {214, 112, 108}}, {L"Flushed red", {196, 86, 78}},   {L"Deep red-brown", {142, 64, 54}},
            {L"Warm tan", {178, 128, 94}},           {L"Olive", {150, 130, 86}},        {L"Pale", {236, 208, 190}},
            {L"Under-eye shadow", {112, 86, 98}},    {L"Dark brown", {86, 58, 42}}};
        return t[std::clamp(i, 0, kTintTypes - 1)];
    }
    static constexpr int kSpotKinds = 3;
    static const wchar_t* SpotName(int k) { static const wchar_t* n[kSpotKinds] = {L"Freckles", L"Moles", L"Age spots"}; return n[std::clamp(k, 0, kSpotKinds - 1)]; }

    std::vector<uint8_t> tintR, tintG, tintB, tintA, spots, spotKind, shine;
    bool tintPainted = false, spotsPainted = false, shinePainted = false;
    // GAIN (skin colour from photos, photo_skin.h): N x N RGBA, r g b = gain * 80 (80 = 1.0), a = where it applies. The albedo is multiplied by it
    // in linear light before the tint, so the person's colour moves and the pores of the base skin stay.
    std::vector<uint8_t> gain;
    bool gainPainted = false;
    bool Any() const { return tintPainted || spotsPainted || shinePainted || gainPainted; }
    void SetGain(const std::vector<uint8_t>& g) {
        if (g.size() != (size_t)N * N * 4) return;
        Ensure();
        gain = g;
        gainPainted = false;
        for (size_t i = 3; i < gain.size(); i += 4) if (gain[i]) { gainPainted = true; break; }
    }
    void ClearGain() { gain.clear(); gainPainted = false; }

    void Ensure() {
        if (!tintA.empty()) return;
        const size_t n = (size_t)N * N;
        tintR.assign(n, 0); tintG.assign(n, 0); tintB.assign(n, 0); tintA.assign(n, 0);
        spots.assign(n, 0); spotKind.assign(n, 0); shine.assign(n, 0);
    }
    void Clear() {
        if (tintA.empty()) return;
        std::fill(tintA.begin(), tintA.end(), (uint8_t)0);
        std::fill(spots.begin(), spots.end(), (uint8_t)0);
        std::fill(shine.begin(), shine.end(), (uint8_t)0);
        tintPainted = spotsPainted = shinePainted = false;
        ClearGain();
    }

    // One dab at a hit (atlas u, v and the UV size per metre), same brush maths as HairPainter::Dab.
    void Dab(int tool, const HairHit& h, float radiusMm, float strength, float hardness, int tintType, int spotType) {
        if (!h.hit || !IsLayerTool(tool)) return;
        Ensure();
        const float r = std::max(1.5f, radiusMm * 0.001f * h.uvPerMetre * N);
        const float cx = h.u * N, cy = h.v * N;
        const int x0 = std::max(0, (int)(cx - r)), x1 = std::min(N - 1, (int)(cx + r)), y0 = std::max(0, (int)(cy - r)), y1 = std::min(N - 1, (int)(cy + r));
        const TintType& tt = Tint(tintType);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float q = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
                if (q >= 1) continue;
                const float w = std::clamp(strength * HairPainter::Falloff(q, std::clamp(hardness, 0.0f, 0.99f)), 0.0f, 1.0f);
                const size_t i = (size_t)y * N + (size_t)x;
                if (tool == TintPaint) {
                    const float a = tintA[i] / 255.0f, na = a + (1 - a) * w, k = na > 1e-4f ? w / na : 1.0f; // the new colour's share of the mix
                    tintR[i] = (uint8_t)(tintR[i] + (tt.rgb[0] - tintR[i]) * k);
                    tintG[i] = (uint8_t)(tintG[i] + (tt.rgb[1] - tintG[i]) * k);
                    tintB[i] = (uint8_t)(tintB[i] + (tt.rgb[2] - tintB[i]) * k);
                    tintA[i] = (uint8_t)std::clamp(na * 255.0f + 0.5f, 0.0f, 255.0f);
                    tintPainted = true;
                } else if (tool == TintErase) tintA[i] = (uint8_t)(tintA[i] * (1 - w));
                else if (tool == SpotPaint) {
                    spots[i] = (uint8_t)std::clamp(spots[i] + (255 - spots[i]) * w + 0.5f, 0.0f, 255.0f);
                    spotKind[i] = (uint8_t)std::clamp(spotType, 0, kSpotKinds - 1);
                    spotsPainted = true;
                } else if (tool == SpotErase) spots[i] = (uint8_t)(spots[i] * (1 - w));
                else if (tool == ShinePaint) { shine[i] = (uint8_t)std::clamp(shine[i] + (255 - shine[i]) * w + 0.5f, 0.0f, 255.0f); shinePainted = true; }
                else if (tool == ShineErase) shine[i] = (uint8_t)(shine[i] * (1 - w));
            }
    }

    // Auto inner-corner redness: tint a soft ellipse at each eye's medial canthus (the pink of the tear duct), found through the head's
    // own surface (`surf(u, v, p, n)` gives the rest point under an atlas texel). Landmarks are the face maker's, mesh space.
    template <class SurfFn>
    void FillInnerCorners(const float eyeC[2][3], float eyeHalf, float strength, SurfFn surf) {
        Ensure();
        const TintType& tt = Tint(0);
        for (int e = 0; e < 2; ++e) {
            const float side = eyeC[e][0] >= 0 ? 1.0f : -1.0f;
            // the palpebral fissure is about 2 * 14 mm wide: the inner corner sits ~13 mm toward the nose from the ball's centre, a touch low
            const float corner[3] = {eyeC[e][0] - side * 0.0127f, eyeC[e][1] - 0.0012f, eyeC[e][2] + 0.004f};
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    float p[3], n[3];
                    if (!surf((x + 0.5f) / N, (y + 0.5f) / N, p, n)) continue;
                    if (n[2] < 0.15f) continue; // faces away from the front: not the eye corner
                    const float dx = (p[0] - corner[0]) / 0.0058f, dy = (p[1] - corner[1]) / 0.0040f, dz = (p[2] - corner[2]) / 0.010f;
                    const float q = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (q >= 1.0f) continue;
                    const float w = std::clamp(strength * HairPainter::Falloff(q, 0.15f), 0.0f, 1.0f);
                    const size_t i = (size_t)y * N + (size_t)x;
                    const float a = tintA[i] / 255.0f, na = a + (1 - a) * w, k = na > 1e-4f ? w / na : 1.0f;
                    tintR[i] = (uint8_t)(tintR[i] + (tt.rgb[0] - tintR[i]) * k);
                    tintG[i] = (uint8_t)(tintG[i] + (tt.rgb[1] - tintG[i]) * k);
                    tintB[i] = (uint8_t)(tintB[i] + (tt.rgb[2] - tintB[i]) * k);
                    tintA[i] = (uint8_t)std::clamp(na * 255.0f + 0.5f, 0.0f, 255.0f);
                    tintPainted = true;
                }
        }
        (void)eyeHalf;
    }

    // Tint then spots into `rgba` (the albedo copy, W x H, nearest from the N x N maps).
    void Composite(std::vector<uint8_t>& rgba, int W, int H) const {
        if (tintA.empty() || (!tintPainted && !spotsPainted && !gainPainted) || rgba.size() < (size_t)W * H * 4) return;
        static float dec[256];
        static uint8_t enc[4096];
        static bool lut = false;
        if (!lut) { // sRGB <-> linear tables for the gain
            for (int i = 0; i < 256; ++i) { const float c = i / 255.0f; dec[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
            for (int i = 0; i < 4096; ++i) { const float l = i / 4095.0f, c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f; enc[i] = (uint8_t)std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f); }
            lut = true;
        }
        for (int y = 0; y < H; ++y) {
            const int my = std::min(N - 1, (int)((long long)y * N / H));
            for (int x = 0; x < W; ++x) {
                const int mx = std::min(N - 1, (int)((long long)x * N / W));
                const size_t m = (size_t)my * N + (size_t)mx;
                uint8_t* p = &rgba[((size_t)y * W + (size_t)x) * 4];
                if (gainPainted && gain[m * 4 + 3]) {
                    for (int j = 0; j < 3; ++j) p[j] = enc[std::min(4095, (int)(dec[p[j]] * (gain[m * 4 + (size_t)j] / 80.0f) * 4095.0f + 0.5f))];
                }
                if (tintA[m]) {
                    const float a = tintA[m] / 255.0f * 0.92f;
                    const float lb = 0.30f * p[0] + 0.59f * p[1] + 0.11f * p[2];
                    const float lc = std::max(1.0f, 0.30f * tintR[m] + 0.59f * tintG[m] + 0.11f * tintB[m]);
                    const float k = std::pow(std::clamp(lb / lc, 0.55f, 1.5f), 0.8f); // the base's light and dark ride on the tint
                    const float c[3] = {tintR[m] * k, tintG[m] * k, tintB[m] * k};
                    for (int j = 0; j < 3; ++j) p[j] = (uint8_t)std::clamp(p[j] + (c[j] - p[j]) * a, 0.0f, 255.0f);
                }
                if (spots[m]) {
                    const int kind = spotKind[m];
                    const float v = SpotPattern(kind, mx, my) * (spots[m] / 255.0f);
                    if (v > 0.003f) {
                        static const float mult[kSpotKinds][3] = {{0.60f, 0.42f, 0.31f}, {0.30f, 0.20f, 0.16f}, {0.66f, 0.50f, 0.38f}};
                        for (int j = 0; j < 3; ++j) p[j] = (uint8_t)(p[j] * (1.0f + (mult[kind][j] - 1.0f) * std::min(1.0f, v)));
                    }
                }
            }
        }
    }

    // The skin surface map with shine painted in: red is oil shine in LitSkin, low red = glossier. Same size as `base`.
    void BuildSurface(const std::vector<uint8_t>& base, int w, int h, std::vector<uint8_t>& out) const {
        out = base;
        if (shine.empty() || !shinePainted || base.size() < (size_t)w * h * 4) return;
        for (int y = 0; y < h; ++y) {
            const int my = std::min(N - 1, (int)((long long)y * N / h));
            for (int x = 0; x < w; ++x) {
                const uint8_t s = shine[(size_t)my * N + (size_t)std::min(N - 1, (int)((long long)x * N / w))];
                if (!s) continue;
                uint8_t& r = out[((size_t)y * w + (size_t)x) * 4];
                r = (uint8_t)(r * (1.0f - s / 255.0f));
            }
        }
    }

    // A map as an RGBA picture W x H for the 2D maps viewer. which: 0 tint, 1 spots, 2 shine. `under` (the skin albedo, optional)
    // shows through at a low level so the paint can be placed against the face.
    void MapImage(int which, int W, int H, const std::vector<uint8_t>* under, std::vector<uint8_t>& rgba) const {
        rgba.assign((size_t)W * H * 4, 0);
        for (int y = 0; y < H; ++y) {
            const int my = std::min(N - 1, (int)((long long)y * N / H));
            for (int x = 0; x < W; ++x) {
                const size_t m = (size_t)my * N + (size_t)std::min(N - 1, (int)((long long)x * N / W));
                const size_t o = ((size_t)y * W + (size_t)x) * 4;
                float c[3] = {28, 26, 24};
                if (under && under->size() >= rgba.size()) for (int j = 0; j < 3; ++j) c[j] = (*under)[o + (size_t)j] * 0.28f + 14.0f;
                if (!tintA.empty()) {
                    if (which == 0 && tintA[m]) { const float a = tintA[m] / 255.0f; const float t[3] = {(float)tintR[m], (float)tintG[m], (float)tintB[m]}; for (int j = 0; j < 3; ++j) c[j] += (t[j] - c[j]) * a; }
                    else if (which == 1 && spots[m]) {
                        const float v = std::min(1.0f, SpotPattern(spotKind[m], (int)(m % N), (int)(m / N)) * 1.6f) , a = spots[m] / 255.0f;
                        c[0] += (240 - c[0]) * a * 0.18f; c[1] += (200 - c[1]) * a * 0.18f; c[2] += (120 - c[2]) * a * 0.18f; // where spots may appear
                        for (int j = 0; j < 3; ++j) c[j] += ((j == 0 ? 255.0f : j == 1 ? 230.0f : 90.0f) - c[j]) * v * a;
                    } else if (which == 2 && shine[m]) { const float a = shine[m] / 255.0f; for (int j = 0; j < 3; ++j) c[j] += (255.0f - c[j]) * a; }
                }
                for (int j = 0; j < 3; ++j) rgba[o + (size_t)j] = (uint8_t)std::clamp(c[j], 0.0f, 255.0f);
                rgba[o + 3] = 255;
            }
        }
    }

    // The spot field: 0..1 at texel (x, y) for a kind. Jittered cells, a few spots per cell by occupancy, irregular edges.
    static float SpotPattern(int kind, int x, int y) {
        static const int cell[kSpotKinds] = {5, 56, 38};
        static const float occ[kSpotKinds] = {0.55f, 0.13f, 0.22f}, r0[kSpotKinds] = {0.9f, 2.6f, 6.0f}, r1[kSpotKinds] = {2.0f, 4.4f, 12.0f};
        static const float s0[kSpotKinds] = {0.35f, 0.85f, 0.25f}, s1[kSpotKinds] = {0.75f, 1.0f, 0.50f}, edge[kSpotKinds] = {0.45f, 0.55f, 0.05f};
        const int C = cell[kind];
        const int cx = x / C, cy = y / C;
        float best = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = cx + dx, gy = cy + dy;
                const uint32_t h = Hash((uint32_t)(gx + 4096), (uint32_t)(gy + 4096), (uint32_t)kind);
                if ((h & 1023) / 1023.0f > occ[kind]) continue;
                const float jx = ((h >> 10) & 255) / 255.0f, jy = ((h >> 18) & 255) / 255.0f, rr = ((h >> 26) & 63) / 63.0f;
                const float px = (gx + 0.15f + 0.7f * jx) * C, py = (gy + 0.15f + 0.7f * jy) * C;
                const float ddx = x + 0.5f - px, ddy = y + 0.5f - py;
                const float ang = std::atan2(ddy, ddx), phase = jx * 6.28f;
                const float rad = (r0[kind] + (r1[kind] - r0[kind]) * rr) * (1.0f + 0.22f * std::sin(ang * 3.0f + phase) + 0.12f * std::sin(ang * 5.0f + phase * 2.0f));
                const float d = std::sqrt(ddx * ddx + ddy * ddy) / std::max(0.5f, rad);
                if (d >= 1.0f) continue;
                const float t = std::clamp((1.0f - d) / std::max(0.05f, 1.0f - edge[kind]), 0.0f, 1.0f);
                const float v = (t * t * (3.0f - 2.0f * t)) * (s0[kind] + (s1[kind] - s0[kind]) * (((h >> 3) & 255) / 255.0f));
                best = std::max(best, v);
            }
        return best;
    }

private:
    static uint32_t Hash(uint32_t a, uint32_t b, uint32_t c) {
        uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 1u) * 0xC2B2AE35u;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
        return h;
    }
};

} // namespace tbd::character
