// Skin colour from photos (Character Creator, PHOTO tab; plan in docs/photo_fit_plan.md section 5). CPU only, no window code.
//
// Each photo that has a solved camera is projected onto the head's UV atlas: every atlas texel knows its point and normal on the current head,
// is checked for being seen (depth test from the photo's camera, facing the camera, skin coloured), and takes the photo's colour there. The light
// in the photo is then removed: a second-order spherical-harmonic lighting model is fitted to the brightness of the skin samples against their
// normals (camera space), over the samples left after throwing away highlights and shadows, and each sample is divided by it. Photos are blended
// by how squarely they see each texel. The blend becomes a smooth colour field; the result is a GAIN over the head's own albedo (the field
// divided by the same-sized blur of the albedo), so the pores and fine detail of the base skin stay and only the person's colour moves.
//   mode 0, "read colours": a wide blur (about 18 mm): the person's skin tone and its slow changes (forehead vs cheeks vs nose redness)
//   mode 1, "project photo": a narrow blur (about 3 mm): blotches, redness patches and moles come across too
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "photo_fit.h"

namespace tbd::character::photo {

namespace skin {

struct Surf {
    int N = 0;
    std::vector<float> p, n;   // N*N*3: the current head's point and normal under each atlas texel
    std::vector<uint8_t> ok;   // N*N
};

// Rasterises the head's skin triangles (flat list, 3 vertices each, with u, v) into the atlas at N x N.
inline void RasterSurface(const std::vector<Vertex>& tris, int N, Surf& s) {
    s.N = N;
    s.p.assign((size_t)N * N * 3, 0.0f);
    s.n.assign((size_t)N * N * 3, 0.0f);
    s.ok.assign((size_t)N * N, 0);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const Vertex* v[3] = {&tris[t], &tris[t + 1], &tris[t + 2]};
        float x[3], y[3];
        for (int k = 0; k < 3; ++k) { x[k] = v[k]->u * (float)N; y[k] = v[k]->v * (float)N; }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::fabs(area) < 1e-6f) continue;
        const int x0 = std::max(0, (int)std::floor(std::min({x[0], x[1], x[2]}))), x1 = std::min(N - 1, (int)std::ceil(std::max({x[0], x[1], x[2]})));
        const int y0 = std::max(0, (int)std::floor(std::min({y[0], y[1], y[2]}))), y1 = std::min(N - 1, (int)std::ceil(std::max({y[0], y[1], y[2]})));
        // a flat normal for vertices that carry none
        const float e1[3] = {v[1]->px - v[0]->px, v[1]->py - v[0]->py, v[1]->pz - v[0]->pz}, e2[3] = {v[2]->px - v[0]->px, v[2]->py - v[0]->py, v[2]->pz - v[0]->pz};
        float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]) + 1e-12f;
        for (int k = 0; k < 3; ++k) fn[k] /= fl;
        const float inv = 1.0f / area;
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px) {
                const float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
                const float w0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) * inv;
                const float w1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                const size_t i = (size_t)py * N + (size_t)px;
                const float w[3] = {w0, w1, w2};
                float pos[3] = {0, 0, 0}, nr[3] = {0, 0, 0};
                for (int k = 0; k < 3; ++k) {
                    pos[0] += w[k] * v[k]->px; pos[1] += w[k] * v[k]->py; pos[2] += w[k] * v[k]->pz;
                    nr[0] += w[k] * v[k]->nx; nr[1] += w[k] * v[k]->ny; nr[2] += w[k] * v[k]->nz;
                }
                float nl = std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
                if (nl < 1e-4f) { nr[0] = fn[0]; nr[1] = fn[1]; nr[2] = fn[2]; nl = 1.0f; }
                for (int k = 0; k < 3; ++k) { s.p[i * 3 + (size_t)k] = pos[k]; s.n[i * 3 + (size_t)k] = nr[k] / nl; }
                s.ok[i] = 1;
            }
    }
}

// What the camera sees nearest, at a third of the photo's size.
struct Depth {
    int w = 0, h = 0;
    float scale = 1.0f / 3.0f;
    std::vector<float> z;
};
inline void BuildDepth(const Photo& ph, const std::vector<Vertex>& tris, const float* centre, Depth& d) {
    d.w = std::max(1, (int)(ph.w * d.scale)); d.h = std::max(1, (int)(ph.h * d.scale));
    d.z.assign((size_t)d.w * d.h, 1e30f);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        float s[3][3];
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            const float p[3] = {tris[t + (size_t)k].px, tris[t + (size_t)k].py, tris[t + (size_t)k].pz};
            Project(ph.cam, ph.w, ph.h, centre, p, s[k]);
            s[k][0] *= d.scale; s[k][1] *= d.scale;
            if (s[k][2] < 0.02f) bad = true;
        }
        if (bad) continue;
        const float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
        if (area >= 0) continue; // back-facing (same winding rule as RenderHead)
        const int x0 = std::max(0, (int)std::floor(std::min({s[0][0], s[1][0], s[2][0]}))), x1 = std::min(d.w - 1, (int)std::ceil(std::max({s[0][0], s[1][0], s[2][0]})));
        const int y0 = std::max(0, (int)std::floor(std::min({s[0][1], s[1][1], s[2][1]}))), y1 = std::min(d.h - 1, (int)std::ceil(std::max({s[0][1], s[1][1], s[2][1]})));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                const float w0 = ((s[1][0] - px) * (s[2][1] - py) - (s[2][0] - px) * (s[1][1] - py)) * inv;
                const float w1 = ((s[2][0] - px) * (s[0][1] - py) - (s[0][0] - px) * (s[2][1] - py)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2];
                float& zb = d.z[(size_t)y * d.w + x];
                if (z < zb) zb = z;
            }
    }
}

inline float ToLinear(float c8) { const float c = c8 / 255.0f; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float ToSrgb8(float l) { l = std::clamp(l, 0.0f, 1.0f); return 255.0f * (l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f); }
inline const float* LinLut() { static float t[256]; static bool done = false; if (!done) { for (int i = 0; i < 256; ++i) t[i] = ToLinear((float)i); done = true; } return t; }

// Is this 8-bit sRGB colour skin? (YCbCr box: keeps pale to dark skin, drops hair, eyes, clothes, sky, shadow black.)
inline bool SkinColour(float r, float g, float b) {
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float cb = 128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b, cr = 128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b;
    return y > 35.0f && y < 252.0f && cb > 74.0f && cb < 130.0f && cr > 132.0f && cr < 176.0f;
}

inline void SH2(const float* n, float* b) {
    b[0] = 1.0f; b[1] = n[1]; b[2] = n[2]; b[3] = n[0];
    b[4] = n[0] * n[1]; b[5] = n[1] * n[2]; b[6] = 3.0f * n[2] * n[2] - 1.0f; b[7] = n[0] * n[2]; b[8] = n[0] * n[0] - n[1] * n[1];
}

// Box blur of `ch` interleaved channels, radius r texels, two passes each way (close to a Gaussian).
inline void BoxBlur(std::vector<float>& a, int N, int ch, int r) {
    if (r < 1) return;
    std::vector<float> tmp((size_t)N);
    for (int pass = 0; pass < 2; ++pass)
        for (int dir = 0; dir < 2; ++dir)
            for (int c = 0; c < ch; ++c)
                for (int line = 0; line < N; ++line) {
                    const auto at = [&](int k) -> float& { return dir == 0 ? a[((size_t)line * N + (size_t)k) * ch + (size_t)c] : a[((size_t)k * N + (size_t)line) * ch + (size_t)c]; };
                    double sum = 0;
                    for (int k = -r; k <= r; ++k) sum += at(std::clamp(k, 0, N - 1));
                    for (int k = 0; k < N; ++k) {
                        tmp[(size_t)k] = (float)(sum / (2 * r + 1));
                        sum += at(std::min(N - 1, k + r + 1)) - at(std::max(0, k - r));
                    }
                    for (int k = 0; k < N; ++k) at(k) = tmp[(size_t)k];
                }
}

// Normalised blur: values only count where the weight is. Returns the blurred colour (3 ch) and blurred weight.
inline void BlurField(const std::vector<float>& rgb, const std::vector<float>& w, int N, int r, std::vector<float>& outRgb, std::vector<float>& outW) {
    std::vector<float> pw((size_t)N * N * 3);
    for (size_t i = 0; i < (size_t)N * N; ++i) for (int c = 0; c < 3; ++c) pw[i * 3 + (size_t)c] = rgb[i * 3 + (size_t)c] * w[i];
    outW = w;
    BoxBlur(pw, N, 3, r);
    BoxBlur(outW, N, 1, r);
    outRgb.assign((size_t)N * N * 3, 0.0f);
    for (size_t i = 0; i < (size_t)N * N; ++i) if (outW[i] > 1e-5f) for (int c = 0; c < 3; ++c) outRgb[i * 3 + (size_t)c] = pw[i * 3 + (size_t)c] / outW[i];
}

} // namespace skin

struct SkinReport {
    bool ok = false;
    std::string message;
    int photosUsed = 0;
    float coverage = 0;          // share of the face's texels seen by at least one photo
    float lightingRange = 1;     // brightest / darkest shading over the samples of the most uneven photo
    bool unevenLight = false;
    size_t cntGeom = 0, cntSkin = 0, cntUsed = 0, cntFace = 0; // texels: seen, skin coloured, kept by the light fit, and all the face texels
    bool coolCast = false;       // the colours came out grey or blue: likely a cold photo (try Warmth)
    float zoneRgb[8][3] = {};    // sRGB 0..255 of the asked-for zone points
    bool zoneOk[8] = {};
};

// Builds the gain over the head's albedo. `skinTris`: the skin triangles of the current head (with u, v and normals). `occluders`: what blocks the
// view (the head without hair and eyes). `albedo`: the head's own albedo RGBA aw x ah. `outN`: size of the gain map (SkinPaint::N).
// gainRgba receives outN*outN*4: gain*80 for r, g, b (80 = 1.0), a = 255 where the gain applies.
inline SkinReport BuildSkinGain(const std::vector<Photo>& photos, const std::vector<Vertex>& skinTris, const std::vector<Vertex>& occluders, const float* centre,
                                const std::vector<uint8_t>& albedo, int aw, int ah, bool projectMode, float strength, float warmth, float exposure, float uvPerMetre, int outN,
                                const float zonePts[][3], int zones, std::vector<uint8_t>& gainRgba) {
    SkinReport rep;
    const int N = 1024;
    skin::Surf sf;
    skin::RasterSurface(skinTris, N, sf);
    size_t faceTexels = 0;
    for (uint8_t o : sf.ok) faceTexels += o;
    rep.cntFace = faceTexels;
    if (!faceTexels) { rep.message = "The head has no skin triangles to colour."; return rep; }
    if (albedo.size() < (size_t)aw * ah * 4 || aw <= 0) { rep.message = "The head albedo is not loaded yet (open the HEAD tab once)."; return rep; }
    const float* lin = skin::LinLut();
    // white balance by hand: warmth moves red against blue, exposure is in stops (a photo's own colour cast cannot be known from one picture)
    const float expo = std::exp2(std::clamp(exposure, -2.0f, 2.0f));
    const float mulRgb[3] = {expo * (1.0f + 0.35f * std::clamp(warmth, -1.0f, 1.0f)), expo, expo * (1.0f - 0.35f * std::clamp(warmth, -1.0f, 1.0f))};
    std::vector<float> sum((size_t)N * N * 3, 0.0f), wsum((size_t)N * N, 0.0f);
    std::vector<uint8_t> seenAny((size_t)N * N, 0);
    for (const Photo& ph : photos) {
        if (!ph.cam.ok || ph.rgba.empty()) continue;
        skin::Depth dp;
        skin::BuildDepth(ph, occluders, centre, dp);
        const float zero[3] = {0, 0, 0};
        struct S { uint32_t i; float c[3]; float y, facing; float nc[3]; };
        std::vector<S> samples;
        samples.reserve(200000);
        for (size_t i = 0; i < (size_t)N * N; ++i) {
            if (!sf.ok[i]) continue;
            float o[3], nc[3];
            Project(ph.cam, ph.w, ph.h, centre, &sf.p[i * 3], o);
            float ixf, iyf; // the frame position is on the placed photo: take the colour from the photo pixel under it
            ph.FrameToImg(o[0], o[1], &ixf, &iyf);
            if (ixf < 1 || iyf < 1 || ixf > (float)ph.w - 2 || iyf > (float)ph.h - 2 || o[0] < 1 || o[1] < 1 || o[2] < 0.02f) continue;
            const int dx = std::min(dp.w - 1, (int)(o[0] * dp.scale)), dy = std::min(dp.h - 1, (int)(o[1] * dp.scale));
            float zmin = 1e30f;
            for (int yy = std::max(0, dy - 1); yy <= std::min(dp.h - 1, dy + 1); ++yy)
                for (int xx = std::max(0, dx - 1); xx <= std::min(dp.w - 1, dx + 1); ++xx) zmin = std::min(zmin, dp.z[(size_t)yy * dp.w + xx]);
            if (o[2] > zmin + 0.012f) continue; // something nearer the camera (the nose over the cheek, a turned jaw)
            Rotate(ph.cam, zero, &sf.n[i * 3], nc);
            if (nc[2] < 0.3f) continue;         // turned away: grazing views smear
            const int x0 = (int)ixf, y0 = (int)iyf;
            const float fx = ixf - (float)x0, fy = iyf - (float)y0;
            float c8[3];
            for (int c = 0; c < 3; ++c) {
                const auto px = [&](int xx, int yy) { return (float)ph.rgba[((size_t)yy * ph.w + (size_t)xx) * 4 + (size_t)c]; };
                c8[c] = (px(x0, y0) * (1 - fx) + px(x0 + 1, y0) * fx) * (1 - fy) + (px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx) * fy;
            }
            ++rep.cntGeom;
            seenAny[i] = 1;
            if (!skin::SkinColour(c8[0], c8[1], c8[2])) continue;
            ++rep.cntSkin;
            S s;
            s.i = (uint32_t)i;
            for (int c = 0; c < 3; ++c) s.c[c] = lin[std::clamp((int)(c8[c] + 0.5f), 0, 255)];
            s.y = 0.2126f * s.c[0] + 0.7152f * s.c[1] + 0.0722f * s.c[2];
            s.facing = nc[2];
            for (int c = 0; c < 3; ++c) s.nc[c] = nc[c];
            samples.push_back(s);
        }
        if (samples.size() < 500) continue;
        // lighting: SH2 over the camera-space normals, fitted to the brightness; highlights and dark patches leave the fit and the result
        std::vector<char> use(samples.size(), 1);
        double coef[9] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<float> L(samples.size());
        for (int iter = 0; iter < 4; ++iter) {
            std::vector<double> A(81, 0.0), bv(9, 0.0);
            const size_t step = std::max<size_t>(1, samples.size() / 20000);
            double cnt = 0;
            for (size_t k = 0; k < samples.size(); k += step) {
                if (!use[k]) continue;
                float b[9];
                skin::SH2(samples[k].nc, b);
                for (int a = 0; a < 9; ++a) { bv[(size_t)a] += (double)b[a] * samples[k].y; for (int c = 0; c < 9; ++c) A[(size_t)a * 9 + (size_t)c] += (double)b[a] * b[c]; }
                cnt += 1;
            }
            for (int a = 1; a < 9; ++a) A[(size_t)a * 9 + (size_t)a] += 2e-3 * cnt; // the light is smooth: keep the higher terms small
            A[0] += 1e-6 * cnt;
            std::vector<double> A2 = A, x = bv;
            if (!detail::SolveSym(A2, x, 9)) break;
            for (int a = 0; a < 9; ++a) coef[a] = x[(size_t)a];
            std::vector<float> ratio;
            for (size_t k = 0; k < samples.size(); ++k) {
                float b[9];
                skin::SH2(samples[k].nc, b);
                double l = 0;
                for (int a = 0; a < 9; ++a) l += coef[a] * b[a];
                L[k] = (float)std::max(l, 0.25 * std::fabs(coef[0]) + 1e-4);
                ratio.push_back(samples[k].y / L[k]);
            }
            std::vector<float> sorted = ratio;
            std::sort(sorted.begin(), sorted.end());
            const float med = sorted[sorted.size() / 2];
            for (size_t k = 0; k < samples.size(); ++k) use[k] = ratio[k] > 0.55f * med && ratio[k] < 1.8f * med; // highlights, shadows, lips, moles
        }
        std::vector<float> Ls = L;
        std::sort(Ls.begin(), Ls.end());
        const float p5 = Ls[Ls.size() / 20], p95 = Ls[Ls.size() * 19 / 20], meanL = Ls[Ls.size() / 2];
        rep.lightingRange = std::max(rep.lightingRange, p95 / std::max(p5, 1e-4f));
        for (size_t k = 0; k < samples.size(); ++k) {
            if (!use[k]) continue;
            ++rep.cntUsed;
            const S& s = samples[k];
            const float shade = std::clamp(L[k] / meanL, 0.5f, 1.7f);
            const float wgt = std::pow(s.facing, 1.5f);
            for (int c = 0; c < 3; ++c) sum[(size_t)s.i * 3 + (size_t)c] += s.c[c] / shade * wgt * mulRgb[c];
            wsum[s.i] += wgt;
        }
        ++rep.photosUsed;
    }
    if (!rep.photosUsed) { rep.message = "No photo could be used: each needs a solved camera (4+ pins) and skin in view."; return rep; }
    rep.unevenLight = rep.lightingRange > 2.4f;
    { // mean skin colour of the result, to hint at a colour cast: skin is red over green over blue, a cool cast makes it grey or blue
        double m[3] = {0, 0, 0}, wt = 0;
        for (size_t i = 0; i < (size_t)N * N; ++i) if (wsum[i] > 0) { for (int c = 0; c < 3; ++c) m[c] += sum[i * 3 + (size_t)c] / wsum[i]; wt += 1; }
        if (wt > 0) { rep.coolCast = (m[2] / std::max(m[0], 1e-6)) > 0.62 || (m[1] / std::max(m[0], 1e-6)) > 0.85; }
    }
    std::vector<float> rgb((size_t)N * N * 3, 0.0f), w((size_t)N * N, 0.0f);
    size_t covered = 0;
    for (size_t i = 0; i < (size_t)N * N; ++i)
        if (wsum[i] > 0) { for (int c = 0; c < 3; ++c) rgb[i * 3 + (size_t)c] = sum[i * 3 + (size_t)c] / wsum[i]; w[i] = std::min(wsum[i], 2.0f); ++covered; }
    size_t seenCount = 0;
    for (uint8_t v : seenAny) seenCount += v;
    rep.coverage = seenCount ? (float)covered / (float)seenCount : 0.0f; // of what the cameras can see (not the back of the head)
    // the zone swatches: the colour the photos give around each asked-for point (before the blur), as sRGB
    for (int z = 0; z < zones && z < 8; ++z) {
        double acc[3] = {0, 0, 0}, wt = 0;
        for (size_t i = 0; i < (size_t)N * N; ++i) {
            if (!sf.ok[i] || w[i] <= 0) continue;
            const float dx = sf.p[i * 3] - zonePts[z][0], dy = sf.p[i * 3 + 1] - zonePts[z][1], dz = sf.p[i * 3 + 2] - zonePts[z][2];
            if (dx * dx + dy * dy + dz * dz > 0.012f * 0.012f) continue;
            for (int c = 0; c < 3; ++c) acc[c] += rgb[i * 3 + (size_t)c] * w[i];
            wt += w[i];
        }
        if (wt > 0) { rep.zoneOk[z] = true; for (int c = 0; c < 3; ++c) rep.zoneRgb[z][c] = skin::ToSrgb8((float)(acc[c] / wt)); }
    }
    double gm[3] = {0, 0, 0}, gw = 0; // the photos' overall skin colour: what skin no photo sees (ears, neck, back of the head) gets
    for (size_t i = 0; i < (size_t)N * N; ++i) if (w[i] > 0) { for (int c = 0; c < 3; ++c) gm[c] += rgb[i * 3 + (size_t)c] * w[i]; gw += w[i]; }
    const float meanF[3] = {gw > 0 ? (float)(gm[0] / gw) : 0.5f, gw > 0 ? (float)(gm[1] / gw) : 0.4f, gw > 0 ? (float)(gm[2] / gw) : 0.3f};
    // the smooth field: a wide blur reads the person's colours, a narrow one keeps their blotches; empty places take the wider blur's value
    const float mm = projectMode ? 3.0f : 18.0f;
    const int r = std::max(2, (int)std::lround(mm * uvPerMetre * (float)N / 1000.0f));
    std::vector<float> f1, w1, f2, w2, f3, w3, field((size_t)N * N * 3, 0.0f);
    std::vector<uint8_t> have((size_t)N * N, 0);
    skin::BlurField(rgb, w, N, r, f1, w1);
    skin::BlurField(rgb, w, N, r * 3, f2, w2);
    skin::BlurField(rgb, w, N, r * 9, f3, w3);
    for (size_t i = 0; i < (size_t)N * N; ++i) {
        const std::vector<float>* f = nullptr;
        if (w1[i] > 0.05f) f = &f1; else if (w2[i] > 0.04f) f = &f2; else if (w3[i] > 0.02f) f = &f3;
        if (!f) continue;
        have[i] = 1;
        for (int c = 0; c < 3; ++c) field[i * 3 + (size_t)c] = (*f)[i * 3 + (size_t)c];
    }
    // the head's own albedo, linear, blurred the same way
    std::vector<float> base((size_t)N * N * 3, 0.0f), okW((size_t)N * N, 0.0f), bb, bw;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const size_t i = (size_t)y * N + (size_t)x;
            if (!sf.ok[i]) continue;
            okW[i] = 1.0f;
            const int ax = std::min(aw - 1, (int)((x + 0.5f) / N * aw)), ay = std::min(ah - 1, (int)((y + 0.5f) / N * ah));
            for (int c = 0; c < 3; ++c) base[i * 3 + (size_t)c] = lin[albedo[((size_t)ay * aw + (size_t)ax) * 4 + (size_t)c]];
        }
    skin::BlurField(base, okW, N, r, bb, bw);
    // gain = the person's colour / the albedo's colour at the same scale, only on and just beside the skin texels
    std::vector<float> gain((size_t)N * N * 3, 1.0f);
    std::vector<uint8_t> near1((size_t)N * N, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            bool any = false;
            for (int dy = -2; dy <= 2 && !any; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < N && yy < N && sf.ok[(size_t)yy * N + (size_t)xx]) { any = true; break; }
                }
            const size_t i = (size_t)y * N + (size_t)x;
            if (!any || bw[i] <= 1e-4f) continue;
            near1[i] = 1;
            for (int c = 0; c < 3; ++c) {
                const float fv = have[i] ? field[i * 3 + (size_t)c] : meanF[c];
                const float g = std::clamp(fv / std::max(bb[i * 3 + (size_t)c], 0.01f), 0.4f, 2.5f);
                gain[i * 3 + (size_t)c] = 1.0f + (g - 1.0f) * std::clamp(strength, 0.0f, 1.0f);
            }
        }
    gainRgba.assign((size_t)outN * outN * 4, 0);
    for (int y = 0; y < outN; ++y)
        for (int x = 0; x < outN; ++x) {
            const float sx = std::clamp(((float)x + 0.5f) * (float)N / (float)outN - 0.5f, 0.0f, (float)N - 1.001f), sy = std::clamp(((float)y + 0.5f) * (float)N / (float)outN - 0.5f, 0.0f, (float)N - 1.001f);
            const int x0 = (int)sx, y0 = (int)sy;
            const float fx = sx - (float)x0, fy = sy - (float)y0;
            const size_t i00 = (size_t)y0 * N + (size_t)x0, i10 = i00 + 1, i01 = i00 + N, i11 = i01 + 1;
            uint8_t* o = &gainRgba[((size_t)y * outN + (size_t)x) * 4];
            if (!near1[(size_t)std::min(N - 1, (int)std::lround(sy)) * N + (size_t)std::min(N - 1, (int)std::lround(sx))]) continue;
            for (int c = 0; c < 3; ++c) {
                const float g = (gain[i00 * 3 + (size_t)c] * (1 - fx) + gain[i10 * 3 + (size_t)c] * fx) * (1 - fy) + (gain[i01 * 3 + (size_t)c] * (1 - fx) + gain[i11 * 3 + (size_t)c] * fx) * fy;
                o[c] = (uint8_t)std::clamp(g * 80.0f + 0.5f, 0.0f, 255.0f);
            }
            o[3] = 255;
        }
    rep.ok = true;
    char b[260];
    snprintf(b, sizeof b, "Skin colour from %d photo(s): %.0f%% of the visible skin used, %s%s", rep.photosUsed, rep.coverage * 100.0f, projectMode ? "photo projected" : "colours read",
             rep.unevenLight ? ". WARNING: the light in a photo is very uneven (strong side light or a hard shadow), so colours may be off." : (rep.coolCast ? ". The skin came out grey-blue: if that is not the person, raise Warmth." : "."));
    rep.message = b;
    char dbg[120];
    snprintf(dbg, sizeof dbg, " [seen %zu, skin-coloured %zu, kept %zu of %zu texels]", rep.cntGeom, rep.cntSkin, rep.cntUsed, rep.cntFace);
    rep.message = std::string(dbg + 1) + ". " + rep.message;
    return rep;
}

} // namespace tbd::character::photo
