// Hair painting for the Character Creator (HEAD tab, HAIR section): two maps over the head's UV atlas, painted on the 3D head.
//   density: 0..1 where hair grows.     flow: unit direction per texel in image space (x right, y down) + a length (0..1).
// Tools: Paint / Erase density, Comb (the stroke's direction in UV at the hit point goes into the flow, with falloff), Smooth,
// and Fill defaults (scalp with a crown swirl that runs forward at the front and down at the sides; brows up and out).
// Picking is a CPU ray / triangle test on the head's skin triangles in mesh space (the engine's own picking returns entities).
// Bake (no shader change): short strands from Assets\characters\hair_tiles\short_strands_01.png, rotated per 16 px cell to the
// local flow and blended between the four nearest cells (no breaks), masked by density, tinted root-dark / tip-light, are
// composited into a COPY of the head's skin albedo on the CPU; the caller uploads it as a new texture.
// Files: density and flow as PNG (written here, stored deflate; read back with DecodeImage).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../image_decode.h"
#include "face_morphs.h"

namespace tbd::character {

// ---------------------------------------------------------------- PNG out (RGBA8, stored deflate blocks)
inline bool WritePngRgba(const std::wstring& path, const uint8_t* rgba, int w, int h) {
    static uint32_t crcTable[256];
    static bool made = false;
    if (!made) {
        for (uint32_t n = 0; n < 256; ++n) { uint32_t c = n; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTable[n] = c; }
        made = true;
    }
    const auto crc = [](const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) { for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 255] ^ (c >> 8); return c; };
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw.size(); at += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 255)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 255)); z.push_back((uint8_t)((~n >> 8) & 255));
        z.insert(z.end(), raw.begin() + (std::ptrdiff_t)at, raw.begin() + (std::ptrdiff_t)(at + n));
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(adler >> s));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        const uint32_t n = (uint32_t)data.size();
        for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(n >> s));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        const uint32_t c = ~crc(td.data(), td.size());
        for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(c >> s));
    };
    std::vector<uint8_t> ihdr(13, 0);
    for (int s = 0; s < 4; ++s) { ihdr[(size_t)s] = (uint8_t)(w >> (24 - 8 * s)); ihdr[(size_t)(4 + s)] = (uint8_t)(h >> (24 - 8 * s)); }
    ihdr[8] = 8; ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

struct HairHit {
    bool hit = false;
    float u = 0, v = 0;   // atlas
    float p[3] = {};      // mesh space
    float uvPerMetre = 1; // the hit triangle's UV size per metre (brush radius in mm -> UV)
    float n[3] = {0, 0, 1}; // the hit triangle's normal (mesh space, as drawn), for the 3D brush rings
};

class HairPainter {
public:
    static constexpr int N = 2048; // map size (density / flow / length over the head's UV atlas; 1024 read as 4 px blocks)
    std::vector<float> density, fx, fy, len;
    float color[3] = {0.16f, 0.10f, 0.06f}; // linear-ish sRGB 0..1, dark brown
    bool painted = false;

    // The head's skin triangles at rest (flat list, mesh space) and its landmarks (morphs.json, same space for #native).
    bool Init(const std::vector<Vertex>& rest, const std::vector<std::pair<size_t, size_t>>& skinRanges, const Json* landmarks) {
        skinTri_.clear();
        for (const auto& [first, count] : skinRanges)
            for (size_t k = first; k + 2 < first + count; k += 3) skinTri_.push_back((uint32_t)k);
        rest_ = rest;
        if (skinTri_.empty()) return false;
        density.assign((size_t)N * N, 0.0f);
        fx.assign((size_t)N * N, 0.0f);
        fy.assign((size_t)N * N, 1.0f);
        len.assign((size_t)N * N, 1.0f);
        Raster();
        lm_ = landmarks ? *landmarks : Json();
        return true;
    }
    bool Ready() const { return !skinTri_.empty(); }

    // Ray in mesh space against the given vertex positions (the head as drawn now: rest or morphed; same layout as rest).
    HairHit Raycast(const float o[3], const float d[3], const std::vector<Vertex>& verts) const {
        HairHit best;
        float bestT = 1e30f;
        for (uint32_t k : skinTri_) {
            const Vertex &A = verts[k], &B = verts[k + 1], &C = verts[k + 2];
            const float e1[3] = {B.px - A.px, B.py - A.py, B.pz - A.pz}, e2[3] = {C.px - A.px, C.py - A.py, C.pz - A.pz};
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-14f) continue;
            const float inv = 1.0f / det;
            const float tv[3] = {o[0] - A.px, o[1] - A.py, o[2] - A.pz};
            const float bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (bu < 0 || bu > 1) continue;
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
            const float bv = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
            if (bv < 0 || bu + bv > 1) continue;
            const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            if (t <= 0 || t >= bestT) continue;
            bestT = t;
            const Vertex &a = rest_[k], &b = rest_[k + 1], &c = rest_[k + 2];
            best.hit = true;
            best.u = a.u + bu * (b.u - a.u) + bv * (c.u - a.u);
            best.v = a.v + bu * (b.v - a.v) + bv * (c.v - a.v);
            for (int i = 0; i < 3; ++i) best.p[i] = o[i] + d[i] * t;
            const float area3 = Len(Cross(Sub(b, a), Sub(c, a)));
            const float areaUv = std::fabs((b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v));
            best.uvPerMetre = area3 > 1e-12f ? std::sqrt(areaUv / area3) : 1.0f;
            const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl > 1e-12f) { best.n[0] = nx / nl; best.n[1] = ny / nl; best.n[2] = nz / nl; if (best.n[0] * d[0] + best.n[1] * d[1] + best.n[2] * d[2] > 0) for (float& c : best.n) c = -c; }
        }
        return best;
    }

    enum Tool { None, Paint, Erase, Comb, Smooth };
    // One dab at `h`; `dirU, dirV`: the stroke's UV direction since the last dab (Comb).
    // Brush weight at q = distance / radius (0..1): 1 inside the hard core, then a soft (1 - t^2)^2 edge. hardness 0: all soft.
    static float Falloff(float q, float hardness) {
        if (q >= 1) return 0;
        if (q <= hardness) return 1;
        const float t = (q - hardness) / std::max(1e-4f, 1 - hardness);
        return (1 - t * t) * (1 - t * t);
    }
    void Dab(Tool tool, const HairHit& h, float radiusMm, float strength, float dirU, float dirV, float hardness = 0.0f) {
        if (!h.hit) return;
        const float r = std::max(1.5f, radiusMm * 0.001f * h.uvPerMetre * N);
        const float cx = h.u * N, cy = h.v * N;
        float dx = dirU, dy = dirV;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (tool == Comb && dl < 1e-7f) return;
        if (dl > 0) { dx /= dl; dy /= dl; }
        const int x0 = std::max(0, (int)(cx - r)), x1 = std::min(N - 1, (int)(cx + r)), y0 = std::max(0, (int)(cy - r)), y1 = std::min(N - 1, (int)(cy + r));
        // Smooth reads the maps as they were before this dab: a copy of the brush's box only (plus the 2-texel blur margin)
        const int bx0 = std::max(0, x0 - 2), by0 = std::max(0, y0 - 2), bw = std::min(N - 1, x1 + 2) - bx0 + 1, bh = std::min(N - 1, y1 + 2) - by0 + 1;
        std::vector<float> sd, sx, sy;
        if (tool == Smooth) {
            sd.resize((size_t)bw * bh); sx.resize(sd.size()); sy.resize(sd.size());
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    const size_t o = (size_t)(by0 + y) * N + (size_t)(bx0 + x), b = (size_t)y * bw + (size_t)x;
                    sd[b] = density[o]; sx[b] = fx[o]; sy[b] = fy[o];
                }
        }
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float q = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
                if (q >= 1) continue;
                const float w = strength * Falloff(q, std::clamp(hardness, 0.0f, 0.99f));
                const size_t i = (size_t)y * N + (size_t)x;
                if (tool == Paint) density[i] += (1 - density[i]) * w;
                else if (tool == Erase) density[i] -= density[i] * w;
                else if (tool == Comb) { fx[i] += (dx - fx[i]) * w; fy[i] += (dy - fy[i]) * w; Norm(i); }
                else if (tool == Smooth) {
                    float a = 0, bx = 0, by = 0;
                    int n = 0;
                    for (int j = -2; j <= 2; ++j)
                        for (int k = -2; k <= 2; ++k) {
                            const int X = std::clamp(x + k, bx0, bx0 + bw - 1), Y = std::clamp(y + j, by0, by0 + bh - 1);
                            const size_t o = (size_t)(Y - by0) * bw + (size_t)(X - bx0);
                            a += sd[o]; bx += sx[o]; by += sy[o]; ++n;
                        }
                    density[i] += (a / (float)n - density[i]) * w;
                    fx[i] += (bx / (float)n - fx[i]) * w;
                    fy[i] += (by / (float)n - fy[i]) * w;
                    Norm(i);
                }
            }
        painted = true;
    }

    // The rest surface point and normal under atlas (u, v); false where no skin is there.
    bool SurfaceAt(float u, float v, float p[3], float n[3]) const {
        const int x = std::clamp((int)(u * N), 0, N - 1), y = std::clamp((int)(v * N), 0, N - 1);
        const size_t i = (size_t)y * N + (size_t)x;
        if (tri_.empty() || tri_[i] < 0) return false;
        Surface(i, p, n);
        return true;
    }

    void Clear() {
        std::fill(density.begin(), density.end(), 0.0f);
        std::fill(fx.begin(), fx.end(), 0.0f);
        std::fill(fy.begin(), fy.end(), 1.0f);
        std::fill(len.begin(), len.end(), 1.0f);
        painted = false;
    }

    // Scalp and brows from the landmarks; directions from 3D rules turned into image directions per texel.
    void FillDefaults() {
        const auto num = [&](const char* k, float def) { const Json* j = lm_.Find(k); return j && j->type == Json::Number ? (float)j->n : def; };
        const auto vec = [&](const char* k, int i, float def) { const Json* j = lm_.Find(k); return j && j->a.size() > (size_t)i ? (float)j->a[(size_t)i].Num(def) : def; };
        const float hairline = num("hairline_y", 0.887f), top = num("head_top_y", 0.930f), back = num("head_back_z", -0.045f);
        const float front = num("face_front_z", 0.176f), browY = num("brow_y", 0.829f), mouthY = num("mouth_y", 0.754f);
        const float earTop = vec("ear_top", 1, 0.84f), earX = std::fabs(vec("ear_c", 0, 0.075f)), earY = vec("ear_c", 1, 0.81f), earZ = vec("ear_c", 2, 0.03f);
        const float cz = (front + back) * 0.5f;
        const float crown[3] = {0.0f, top - 0.012f, back + 0.065f};
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const size_t i = (size_t)y * N + (size_t)x;
                const int t = tri_[i];
                if (t < 0) continue;
                float p[3], n[3];
                Surface(i, p, n);
                // scalp: a hairline at the brow-top in front, down past the temples to above the ear, the nape at the back
                const float az = std::atan2(std::fabs(p[0]), p[2] - cz) * 57.29578f; // 0 front .. 180 back
                float hl;
                if (az < 30) hl = hairline;
                else if (az < 85) hl = hairline + (earTop + 0.008f - hairline) * (az - 30) / 55;
                else hl = earTop + 0.008f + (mouthY + 0.004f - earTop - 0.008f) * std::min(1.0f, (az - 85) / 80);
                float dScalp = Smooth01(hl - 0.003f, hl + 0.006f, p[1]);
                const float ed = std::sqrt((std::fabs(p[0]) - earX) * (std::fabs(p[0]) - earX) + (p[1] - earY) * (p[1] - earY) + (p[2] - earZ) * (p[2] - earZ));
                if (std::fabs(p[0]) > earX - 0.012f && ed < 0.034f) dScalp = 0; // the ears stay bare
                // brows: an arch above the eye, thick inside, thin at the tail
                float dBrow = 0;
                const float ax = std::fabs(p[0]);
                if (n[2] > 0.2f && p[2] > front - 0.07f && ax > 0.010f && ax < 0.062f) {
                    const float s = (ax - 0.010f) / 0.052f; // 0 inner .. 1 tail
                    const float centre = browY + 0.002f + 0.006f * std::sin(3.14159f * std::min(1.0f, s * 1.15f)) - 0.004f * s;
                    const float half = 0.0055f * (1 - 0.55f * s);
                    dBrow = 1 - Smooth01(half * 0.6f, half, std::fabs(p[1] - centre));
                    dBrow *= Smooth01(0.0f, 0.12f, s) * (1 - Smooth01(0.85f, 1.0f, s));
                }
                float d3[3];
                if (dBrow > dScalp) { // up and out along the brow
                    const float sx = p[0] >= 0 ? 1.0f : -1.0f, s = (ax - 0.010f) / 0.052f;
                    d3[0] = sx * (0.35f + 0.65f * s); d3[1] = 0.9f - 1.0f * s; d3[2] = 0;
                    len[i] = 0.30f;
                    density[i] = dBrow;
                } else { // away from the crown, a little swirl about it
                    const float v[3] = {p[0] - crown[0], p[1] - crown[1], p[2] - crown[2]};
                    const float sw[3] = {n[1] * v[2] - n[2] * v[1], n[2] * v[0] - n[0] * v[2], n[0] * v[1] - n[1] * v[0]};
                    const float closeC = 1 - Smooth01(0.02f, 0.07f, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
                    for (int k = 0; k < 3; ++k) d3[k] = v[k] + sw[k] * (0.25f + 0.8f * closeC);
                    if (p[2] > cz + 0.02f && p[1] > hl - 0.01f) d3[2] += 0.04f; // forward at the front
                    len[i] = 1.0f;
                    density[i] = dScalp;
                }
                ToImage(i, d3);
            }
        painted = true;
    }

    // The strand tile (RGBA, white strands on black, vertical) box-filtered to a square `size` grey mask: sampling it near 1:1
    // keeps thin strands from vanishing between nearest samples.
    static std::vector<float> TileMip(const std::vector<uint8_t>& rgba, int tw, int th, int size) {
        std::vector<float> m((size_t)size * size, 0.0f);
        if (rgba.empty()) return m;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const int x0 = x * tw / size, x1 = std::max(x0 + 1, (x + 1) * tw / size), y0 = y * th / size, y1 = std::max(y0 + 1, (y + 1) * th / size);
                float s = 0;
                for (int j = y0; j < y1; ++j) for (int k = x0; k < x1; ++k) s += rgba[((size_t)j * tw + (size_t)k) * 4];
                m[(size_t)y * size + (size_t)x] = s / (255.0f * (float)((x1 - x0) * (y1 - y0)));
            }
        return m;
    }

    // Composites strands into a copy of `albedo` (w x h RGBA). `tile`: the strand mask (tw x th, white strands, vertical).
    void Bake(const std::vector<uint8_t>& albedo, int w, int h, const std::vector<uint8_t>& tile, int tw, int th, std::vector<uint8_t>& out) const {
        out = albedo;
        if (tile.empty() || tw < 8 || th < 8) return;
        if (mipSource_ != tile.data()) { // two levels: scalp (long) and brows (short) strands sample each near 1:1
            mipSource_ = tile.data();
            mipBig_ = TileMip(tile, tw, th, kMipBig);
            mipSmall_ = TileMip(tile, tw, th, kMipSmall);
        }
        constexpr int C = 16; // cell size in albedo pixels
        const int cw = (w + C - 1) / C, ch = (h + C - 1) / C;
        std::vector<float> cdx((size_t)cw * ch), cdy((size_t)cw * ch), cl((size_t)cw * ch), cd((size_t)cw * ch);
        for (int cy = 0; cy < ch; ++cy)
            for (int cx = 0; cx < cw; ++cx) {
                float sx = 0, sy = 0, sl = 0, sdn = 0;
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k) {
                        const int X = std::min(N - 1, (cx * C + k * 4 + 2) * N / w), Y = std::min(N - 1, (cy * C + j * 4 + 2) * N / h);
                        const size_t i = (size_t)Y * N + (size_t)X;
                        sx += fx[i] * (density[i] + 0.01f); sy += fy[i] * (density[i] + 0.01f); sl += len[i]; sdn += density[i];
                    }
                const float l = std::sqrt(sx * sx + sy * sy);
                const size_t c = (size_t)cy * cw + (size_t)cx;
                cdx[c] = l > 1e-6f ? sx / l : 0.0f; cdy[c] = l > 1e-6f ? sy / l : 1.0f; cl[c] = sl / 16; cd[c] = sdn / 16;
            }
        const float pxPerTile = 110.0f * (float)w / 2048.0f; // finer strands than before (180) // one tile of strands spans this many albedo pixels at length 1
        for (int y = 0; y < h; ++y) {
            const float fyN = std::max(0.0f, (y + 0.5f) * N / h - 0.5f);
            const int Y0 = std::min(N - 1, (int)fyN), Y1 = std::min(N - 1, Y0 + 1);
            const float wy = fyN - (float)Y0;
            for (int x = 0; x < w; ++x) {
                // bilinear: the density's edges stay smooth at the albedo's resolution, not in map-texel blocks
                const float fxN = std::max(0.0f, (x + 0.5f) * N / w - 0.5f);
                const int X0 = std::min(N - 1, (int)fxN), X1 = std::min(N - 1, X0 + 1);
                const float wx = fxN - (float)X0;
                const float* r0 = &density[(size_t)Y0 * N], *r1 = &density[(size_t)Y1 * N];
                const float dens = (r0[X0] * (1 - wx) + r0[X1] * wx) * (1 - wy) + (r1[X0] * (1 - wx) + r1[X1] * wx) * wy;
                if (dens < 0.02f) continue;
                // the four cell centres around the pixel, bilinear weights
                const float gx = (x + 0.5f) / C - 0.5f, gy = (y + 0.5f) / C - 0.5f;
                const int ix = std::clamp((int)std::floor(gx), 0, cw - 1), iy = std::clamp((int)std::floor(gy), 0, ch - 1);
                const float ax = std::clamp(gx - ix, 0.0f, 1.0f), ay = std::clamp(gy - iy, 0.0f, 1.0f);
                float mask = 0, phase = 0;
                for (int q = 0; q < 4; ++q) {
                    const int cx = std::min(cw - 1, ix + (q & 1)), cy = std::min(ch - 1, iy + (q >> 1));
                    const float wq = ((q & 1) ? ax : 1 - ax) * ((q >> 1) ? ay : 1 - ay);
                    if (wq <= 0) continue;
                    const size_t c = (size_t)cy * cw + (size_t)cx;
                    const float span = pxPerTile * std::max(0.15f, cl[c]); // albedo pixels one tile covers here
                    const bool big = span > 0.6f * (float)kMipBig;
                    const int ts = big ? kMipBig : kMipSmall;
                    const std::vector<float>& mip = big ? mipBig_ : mipSmall_;
                    const float k = (float)ts / span;
                    const float along = (x * cdx[c] + y * cdy[c]) * k, across = (x * -cdy[c] + y * cdx[c]) * k;
                    int tu = (int)std::fmod(across, (float)ts), tv = (int)std::fmod(along, (float)ts);
                    if (tu < 0) tu += ts;
                    if (tv < 0) tv += ts;
                    mask += wq * mip[(size_t)tv * ts + (size_t)tu];
                    phase += wq * (float)tv / (float)ts;
                }
                const float a = std::clamp(std::min(1.0f, mask * 3.0f) * std::min(1.0f, dens * 1.3f) * 0.95f, 0.0f, 1.0f);
                const float shade = 0.65f + 0.6f * phase; // root darker, tip lighter
                uint8_t* p = &out[((size_t)y * w + (size_t)x) * 4];
                for (int c = 0; c < 3; ++c) {
                    const float base = p[c] * (1 - 0.30f * dens); // skin in the shadow of the hair
                    const float hair = std::min(255.0f, color[c] * shade * 255.0f);
                    p[c] = (uint8_t)std::lround(base * (1 - a) + hair * a);
                }
            }
        }
    }

    bool Save(const std::wstring& densityPath, const std::wstring& flowPath) const {
        std::vector<uint8_t> d((size_t)N * N * 4), f((size_t)N * N * 4);
        for (size_t i = 0; i < (size_t)N * N; ++i) {
            const uint8_t v = (uint8_t)std::lround(std::clamp(density[i], 0.0f, 1.0f) * 255);
            d[i * 4] = d[i * 4 + 1] = d[i * 4 + 2] = v; d[i * 4 + 3] = 255;
            f[i * 4] = (uint8_t)std::lround((fx[i] * 0.5f + 0.5f) * 255); f[i * 4 + 1] = (uint8_t)std::lround((fy[i] * 0.5f + 0.5f) * 255);
            f[i * 4 + 2] = (uint8_t)std::lround(std::clamp(len[i], 0.0f, 1.0f) * 255); f[i * 4 + 3] = 255;
        }
        return WritePngRgba(densityPath, d.data(), N, N) && WritePngRgba(flowPath, f.data(), N, N);
    }
    bool Load(const std::wstring& densityPath, const std::wstring& flowPath) {
        std::vector<uint8_t> db, fb, d, f;
        int dw = 0, dh = 0, fw = 0, fh = 0;
        if (!ReadBytes(densityPath, db) || !ReadBytes(flowPath, fb) || !DecodeImage(db.data(), db.size(), d, dw, dh) || !DecodeImage(fb.data(), fb.size(), f, fw, fh)) return false;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const size_t i = (size_t)y * N + (size_t)x;
                const size_t a = ((size_t)(y * dh / N) * dw + (size_t)(x * dw / N)) * 4, b = ((size_t)(y * fh / N) * fw + (size_t)(x * fw / N)) * 4;
                density[i] = d[a] / 255.0f;
                fx[i] = f[b] / 255.0f * 2 - 1; fy[i] = f[b + 1] / 255.0f * 2 - 1; len[i] = f[b + 2] / 255.0f;
                Norm(i);
            }
        painted = true;
        return true;
    }

private:
    static constexpr int kMipBig = 192, kMipSmall = 64;
    mutable const uint8_t* mipSource_ = nullptr;
    mutable std::vector<float> mipBig_, mipSmall_;
    std::vector<uint32_t> skinTri_; // first flat vertex of each skin triangle
    std::vector<Vertex> rest_;
    std::vector<int> tri_;          // per texel: index into skinTri_, -1 = no skin
    std::vector<float> bary_;       // per texel: 2 barycentrics
    Json lm_;

    struct F3 { float x, y, z; };
    static F3 Sub(const Vertex& a, const Vertex& b) { return {a.px - b.px, a.py - b.py, a.pz - b.pz}; }
    static F3 Cross(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    static float Len(F3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    static float Smooth01(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
    void Norm(size_t i) {
        const float l = std::sqrt(fx[i] * fx[i] + fy[i] * fy[i]);
        if (l > 1e-6f) { fx[i] /= l; fy[i] /= l; } else { fx[i] = 0; fy[i] = 1; }
    }

    // Which skin triangle covers each texel centre, and where in it.
    void Raster() {
        tri_.assign((size_t)N * N, -1);
        bary_.assign((size_t)N * N * 2, 0.0f);
        for (size_t t = 0; t < skinTri_.size(); ++t) {
            const Vertex* v = &rest_[skinTri_[t]];
            const float x[3] = {v[0].u * N, v[1].u * N, v[2].u * N}, y[3] = {v[0].v * N, v[1].v * N, v[2].v * N};
            const float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
            if (std::fabs(den) < 1e-9f) continue;
            const int X0 = std::max(0, (int)std::ceil(std::min({x[0], x[1], x[2]}) - 0.5f)), X1 = std::min(N - 1, (int)std::floor(std::max({x[0], x[1], x[2]}) - 0.5f));
            const int Y0 = std::max(0, (int)std::ceil(std::min({y[0], y[1], y[2]}) - 0.5f)), Y1 = std::min(N - 1, (int)std::floor(std::max({y[0], y[1], y[2]}) - 0.5f));
            for (int py = Y0; py <= Y1; ++py)
                for (int px = X0; px <= X1; ++px) {
                    const float cx = px + 0.5f, cy = py + 0.5f;
                    const float a = ((y[1] - y[2]) * (cx - x[2]) + (x[2] - x[1]) * (cy - y[2])) / den;
                    const float b = ((y[2] - y[0]) * (cx - x[2]) + (x[0] - x[2]) * (cy - y[2])) / den;
                    if (a < -1e-4f || b < -1e-4f || a + b > 1 + 1e-4f) continue;
                    const size_t i = (size_t)py * N + (size_t)px;
                    tri_[i] = (int)t;
                    bary_[i * 2] = a; bary_[i * 2 + 1] = b;
                }
        }
    }
    void Surface(size_t i, float p[3], float n[3]) const {
        const Vertex* v = &rest_[skinTri_[(size_t)tri_[i]]];
        const float a = bary_[i * 2], b = bary_[i * 2 + 1], c = 1 - a - b;
        p[0] = v[0].px * a + v[1].px * b + v[2].px * c; p[1] = v[0].py * a + v[1].py * b + v[2].py * c; p[2] = v[0].pz * a + v[1].pz * b + v[2].pz * c;
        n[0] = v[0].nx * a + v[1].nx * b + v[2].nx * c; n[1] = v[0].ny * a + v[1].ny * b + v[2].ny * c; n[2] = v[0].nz * a + v[1].nz * b + v[2].nz * c;
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-9f) { n[0] /= l; n[1] /= l; n[2] /= l; }
    }
    // A 3D direction on the surface at texel i -> the image direction (u right, v down), through the triangle's UV gradient.
    void ToImage(size_t i, const float d3[3]) {
        const Vertex* v = &rest_[skinTri_[(size_t)tri_[i]]];
        const float e1[3] = {v[1].px - v[0].px, v[1].py - v[0].py, v[1].pz - v[0].pz}, e2[3] = {v[2].px - v[0].px, v[2].py - v[0].py, v[2].pz - v[0].pz};
        const float du1 = v[1].u - v[0].u, dv1 = v[1].v - v[0].v, du2 = v[2].u - v[0].u, dv2 = v[2].v - v[0].v;
        // d3 ~ s*e1 + t*e2 (least squares in the triangle's plane), then duv = s*(du1,dv1) + t*(du2,dv2)
        const float a11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], a12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2], a22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        const float b1 = d3[0] * e1[0] + d3[1] * e1[1] + d3[2] * e1[2], b2 = d3[0] * e2[0] + d3[1] * e2[1] + d3[2] * e2[2];
        const float det = a11 * a22 - a12 * a12;
        if (std::fabs(det) < 1e-20f) return;
        const float s = (b1 * a22 - b2 * a12) / det, t = (a11 * b2 - a12 * b1) / det;
        fx[i] = s * du1 + t * du2;
        fy[i] = s * dv1 + t * dv2;
        Norm(i);
    }
};

} // namespace tbd::character
