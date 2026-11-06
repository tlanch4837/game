// Brow maker (Character Creator, HEAD > BROWS): the user draws the boundary the brow ROOTS grow inside; hairs are grown there with
// the brow groom rules (gameenginetools/brow_cards/brow_cards.py: flat along the brow, upright fanning inner corner, top tips lifting,
// soft thinning edges, a straight stem that curves into the brow's path for its last 40 %), then baked into 5 cards per brow that hug
// the skin along and across, with a new texture for this character. The left brow is the right one mirrored (x -> -x).
// Everything is in the head's mesh space; the skin is found by casting rays at the rest head (HairPainter::Raycast).
// Default boundary = the default brow (brow_y arch +- the band's half height), default density/length = the current brow.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../mesh_import.h"

namespace tbd::character {

class BrowMaker {
public:
    // cast(o, d, p, n): the first skin hit on the ray, false when none
    using Cast = std::function<bool(const float o[3], const float d[3], float p[3], float n[3])>;
    // the importer's brow card: seg 8 in brow_cards.json (brow_bake.py) = 8 quads, both faces: 96 vertices; used as a 2 x 4 grid
    static constexpr int kCards = 5, kSeg = 8, kVertsPerCard = kSeg * 12, kAlong = 2, kAcross = 4; // (1 x 8 cut through the forehead lengthwise; 2 x 4 holds)
    static constexpr int kCellW = 384, kCellH = 1152;                      // texture cell per card (the 5 cards of one brow)

    std::vector<std::array<float, 2>> loop; // the boundary, front view (x, y), the subject's left brow (x > 0), closed
    float density = 1.0f, length = 1.0f;     // multipliers on the default brow
    // style: shape (rebuilds the default loop; a drawn loop is the shape itself) and hair (always). Defaults = the default brow.
    float thick = 0.85f;   // band height x (the default brow is a little slimmer than the old band)
    float arch = 1.0f;     // arch height x (0 flat .. 2.5 high)
    float peak = 0.0f;     // where the arch peaks, -1 inner .. +1 out toward the tail
    float tailDrop = 0.0f; // mm the tail falls (+) or lifts (-) beyond the default
    float taper = 1.0f;    // how thin the tail gets (0 as thick as the body .. 2 a fine point)
    float gap = 0.0f;      // mm the inner end moves out (+) from the nose, or in (-)
    float span = 1.0f;     // how far out the brow reaches x
    float upright = 0.0f;  // -1 flat along the brow .. +1 brushed up
    float messy = 0.3f;    // 0 neat .. 1 wild (direction scatter, strays)
    float soft = 1.0f;     // edge softness x
    float dark = 1.0f;     // hair darkness x (0.4 light .. 1.6 near black)
    float warm = 0.0f;     // -1 ash .. +1 auburn
    uint32_t seed = 7;
    float browY = 0.829f;
    bool custom = false; // the user drew the loop (else the default)

    // results of Bake
    std::vector<uint8_t> tex; // RGBA, texW x texH (kCellW*kCards x kCellH; a preview bake: half that)
    int texW = 0, texH = 0;
    std::vector<Vertex> cards; // 2 * kCards * kVertsPerCard: the left brow's 5 cards, then the right brow's
    int hairCount = 0;

    static float Arch(float s, float by) { return by + 0.002f + 0.006f * std::sin(3.14159265f * std::min(1.0f, s * 1.15f)) - 0.004f * s; }
    static float HalfH(float s) { return s > 0.15f ? 0.0045f * (1 - s) + 0.0014f * s : 0.0035f + 0.007f * s; }

    // the styled centre line and half height at s (0 inner .. 1 tail); every style value at its default gives Arch / HalfH exactly
    float StyledArch(float s) const {
        const float p0 = std::clamp(0.435f * (1 + 0.6f * peak), 0.15f, 0.75f); // where the arch peaks (default 0.435)
        const float u = s < p0 ? 0.5f * s / p0 : std::min(1.0f, 0.5f + 0.5f * (s - p0) / std::max(0.05f, 0.87f - p0));
        return browY + 0.002f + 0.006f * arch * std::sin(3.14159265f * u) - 0.004f * s - 0.001f * tailDrop * s * s;
    }
    float StyledHalf(float s) const {
        const float tailH = std::max(0.0004f, 0.0045f - 0.0031f * std::clamp(taper, 0.0f, 2.0f)); // taper 1: 1.4 mm at the tail
        const float h = s > 0.15f ? 0.0045f * (1 - s) + tailH * s : 0.0035f + 0.007f * s;
        return std::max(0.0004f, h * thick);
    }
    void DefaultLoop() {
        loop.clear();
        const float x0 = 0.011f + gap * 0.001f, x1 = x0 + 0.050f * span;
        for (int k = 0; k <= 32; ++k) { const float s = k / 32.0f; loop.push_back({x0 + (x1 - x0) * s, StyledArch(s) + StyledHalf(s)}); }
        for (int k = 32; k >= 0; --k) { const float s = k / 32.0f; loop.push_back({x0 + (x1 - x0) * s, StyledArch(s) - StyledHalf(s)}); }
        custom = false;
    }

    // The loop's vertical extent at x: the band's centre and half height there (false: x outside the loop)
    bool Band(float x, float& c, float& h) const {
        float lo = 1e9f, hi = -1e9f;
        const size_t n = loop.size();
        for (size_t i = 0; i < n; ++i) {
            const auto& a = loop[i];
            const auto& b = loop[(i + 1) % n];
            if ((a[0] - x) * (b[0] - x) > 0 || a[0] == b[0]) continue;
            const float t = (x - a[0]) / (b[0] - a[0]), y = a[1] + (b[1] - a[1]) * t;
            lo = std::min(lo, y); hi = std::max(hi, y);
        }
        if (hi < lo) return false;
        c = (lo + hi) * 0.5f; h = std::max(0.0004f, (hi - lo) * 0.5f);
        return true;
    }
    bool Inside(float x, float y) const {
        bool in = false;
        const size_t n = loop.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++)
            if ((loop[i][1] > y) != (loop[j][1] > y) && x < (loop[j][0] - loop[i][0]) * (y - loop[i][1]) / (loop[j][1] - loop[i][1]) + loop[i][0]) in = !in;
        return in;
    }
    void Extent(float& x0, float& x1) const {
        x0 = 1e9f; x1 = -1e9f;
        for (const auto& p : loop) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); }
    }

    // Grows the hairs and bakes them. False (and `why`) when the loop or the skin cannot be used.
    // preview: about a third of the hairs (drawn thicker to keep the look), half the texture, no supersampling: ~10x faster, for live
    // feedback while a slider moves; the full bake follows when it is let go.
    bool Bake(const Cast& cast, std::string& why, bool preview = false) {
        if (loop.size() < 6) DefaultLoop();
        float X0, X1;
        Extent(X0, X1);
        if (X1 - X0 < 0.005f) { why = "the boundary is too small"; return false; }
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> U(0, 1);
        std::normal_distribution<float> N(0, 1);
        const auto sm = [](float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        const auto surface = [&](float x, float y, float p[3], float n[3]) { const float o[3] = {x, y, 0.4f}, d[3] = {0, 0, -1}; return cast(o, d, p, n); };
        const auto snap = [&](float p[3], float n[3]) { // back onto the skin along the normal
            const float o[3] = {p[0] + n[0] * 0.02f, p[1] + n[1] * 0.02f, p[2] + n[2] * 0.02f}, d[3] = {-n[0], -n[1], -n[2]};
            float q[3], m[3];
            if (cast(o, d, q, m)) { for (int c = 0; c < 3; ++c) { p[c] = q[c]; n[c] = m[c]; } }
        };
        const auto pathDeg = [&](float x) {
            float c0, h0, c1, h1;
            const float dx = 0.001f;
            if (!Band(std::max(X0 + 1e-4f, x - dx), c0, h0) || !Band(std::min(X1 - 1e-4f, x + dx), c1, h1)) return 0.0f;
            return std::atan2(c1 - c0, std::min(X1 - 1e-4f, x + dx) - std::max(X0 + 1e-4f, x - dx)) * 57.29578f;
        };
        // 1. hairs: roots on a staggered grid inside the boundary
        struct Hair { std::vector<std::array<float, 3>> pts; float w, fade, tone, lift; bool wisp; float s, depth; };
        std::vector<Hair> hairs;
        const float spacing = 0.00031f / std::sqrt(std::max(0.05f, density)) * (preview ? 1.75f : 1.0f);
        float yMin = 1e9f, yMax = -1e9f;
        for (const auto& p : loop) { yMin = std::min(yMin, p[1]); yMax = std::max(yMax, p[1]); }
        int col = 0;
        for (float gx = X0; gx <= X1; gx += spacing, ++col)
            for (float gy = yMin + (col % 2 ? spacing * 0.5f : 0.0f); gy <= yMax; gy += spacing * 0.87f) {
                const float x = gx + (U(rng) - 0.5f) * 0.7f * spacing, y = gy + (U(rng) - 0.5f) * 0.7f * spacing;
                if (!Inside(x, y)) continue;
                float c, h;
                if (!Band(x, c, h)) continue;
                const float s = (x - X0) / (X1 - X0), a = std::clamp((y - c) / h, -1.0f, 1.0f);
                const float sf = std::clamp(soft, 0.2f, 2.5f);
                const float edge = (1 - sm(1.0f - 0.4f * sf, 1.0f + 0.3f * sf, std::fabs(a))) * (0.75f + 0.25f * sm(-0.04f * sf, 0.06f * sf, s)) * (1 - sm(1.0f - 0.2f * sf, 1.05f, s)); // the inner start stays firm
                if (U(rng) > 0.65f + 0.35f * edge) continue; // even: only the very edges thin out
                const float path = pathDeg(x);
                const float corner = std::clamp(1 - s / 0.14f, 0.0f, 1.0f);
                const float fan = 62 - 25 * (s / 0.14f) + N(rng) * 4 * (0.5f + messy); // a groomed fan: up and out, little scatter
                const float body = path + 22 - 12 * s - 14 * a + upright * 30 * (1 - 0.4f * s);
                float ang = fan * corner + body * (1 - corner);
                if (a > 0.55f && corner == 0 && U(rng) < 0.45f) ang = path + 12 + 20 * U(rng);
                ang += N(rng) * (2 + 14 * messy);
                if (U(rng) < 0.02f * messy) ang += (U(rng) - 0.5f) * 90; // strays
                float len = (s < 0.14f ? 0.0048f : 0.0105f * (1 - 0.35f * s)) * (a < -0.5f ? 0.8f : 1.0f) * (0.7f + 0.5f * U(rng)) * length;
                float p[3], n[3];
                if (!surface(x, y, p, n)) continue;
                Hair hair;
                hair.s = s; hair.wisp = a > 0.6f; hair.fade = 0.55f + 0.45f * edge; // wisps: only the top edge's tips (the inner corner read as a faded patch)
                hair.tone = 0.9f + 0.14f * U(rng); hair.lift = U(rng) * 0.06f * 255; hair.depth = U(rng);
                hair.w = preview ? 0.0017f : 0.0011f;
                // stem straight (60 %), then the direction turns into the path (20 %), the top 20 % along the path
                constexpr int K = 10;
                hair.pts.push_back({p[0], p[1], p[2]});
                for (int k = 0; k < K; ++k) {
                    const float u = (k + 0.5f) / K;
                    const float th = (u < 0.6f ? ang : u < 0.8f ? ang + (path - ang) * sm(0.6f, 0.8f, u) : path) * 0.0174533f;
                    float d[3] = {std::cos(th), std::sin(th), 0};
                    const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
                    for (int c = 0; c < 3; ++c) d[c] -= n[c] * dn;
                    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (dl < 1e-6f) break;
                    for (int c = 0; c < 3; ++c) p[c] += d[c] / dl * len / K;
                    snap(p, n);
                    hair.pts.push_back({p[0], p[1], p[2]});
                }
                // the hair ends where it would leave the boundary: the brow is exactly the drawn / styled shape
                for (size_t k = 1; k < hair.pts.size(); ++k)
                    if (!Inside(std::fabs(hair.pts[k][0]), hair.pts[k][1])) { hair.pts.resize(std::max<size_t>(k, 2)); break; }
                if (hair.pts.size() > 2) hairs.push_back(std::move(hair));
            }
        if (hairs.empty()) { why = "no skin inside the boundary"; return false; }
        hairCount = (int)hairs.size();
        std::sort(hairs.begin(), hairs.end(), [](const Hair& l, const Hair& r) { return l.depth < r.depth; });

        // 2. cards: 5 along the boundary, each baked from the hairs rooted in its fifth
        const int S = preview ? 1 : 2, cellW = preview ? kCellW / 2 : kCellW, cellH = preview ? kCellH / 2 : kCellH;
        const int CW = cellW * S, CH = cellH * S;
        std::vector<uint8_t> big((size_t)CW * kCards * CH * 4, 0);
        const int BW = CW * kCards;
        cards.clear();
        std::vector<Vertex> right;
        for (int k = 0; k < kCards; ++k) {
            const float over = 0.12f / kCards;
            const float s0 = std::max(0.0f, (float)k / kCards - over), s1 = std::min(1.0f, (float)(k + 1) / kCards + over);
            const float xa = X0 + (X1 - X0) * s0, xb = X0 + (X1 - X0) * s1;
            float ca, ha, cb, hb;
            if (!Band(std::clamp(xa, X0 + 1e-4f, X1 - 1e-4f), ca, ha) || !Band(std::clamp(xb, X0 + 1e-4f, X1 - 1e-4f), cb, hb)) { why = "the boundary has a gap"; return false; }
            float A[3], nA[3], B[3], nB[3];
            if (!surface(xa, ca, A, nA) || !surface(xb, cb, B, nB)) { why = "no skin under the boundary"; return false; }
            float f[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
            const float fn = f[0] * nA[0] + f[1] * nA[1] + f[2] * nA[2];
            for (int c = 0; c < 3; ++c) f[c] -= nA[c] * fn;
            Norm(f);
            float side[3] = {nA[1] * f[2] - nA[2] * f[1], nA[2] * f[0] - nA[0] * f[2], nA[0] * f[1] - nA[1] * f[0]};
            Norm(side);
            // front-view coordinates on the card: d.xy = along * f.xy + across * side.xy
            const float det = f[0] * side[1] - side[0] * f[1];
            if (std::fabs(det) < 1e-6f) { why = "a card stands edge-on"; return false; }
            const auto uv = [&](const std::array<float, 3>& p, float& al, float& ac) {
                const float dx = p[0] - A[0], dy = p[1] - A[1];
                al = (dx * side[1] - side[0] * dy) / det;
                ac = (f[0] * dy - dx * f[1]) / det;
            };
            std::vector<const Hair*> mine;
            for (const Hair& h : hairs) {
                const float sr = h.s;
                const bool first = k == 0 && sr < s0 + over, last = k == kCards - 1 && sr >= s1 - over;
                if ((sr >= s0 + (k > 0 ? over : 0) && sr < s1 - (k < kCards - 1 ? over : 0)) || first || last) mine.push_back(&h);
            }
            float aLo = 1e9f, aHi = -1e9f, cMax = 0.004f;
            for (const Hair* h : mine)
                for (const auto& p : h->pts) { float al, ac; uv(p, al, ac); aLo = std::min(aLo, al); aHi = std::max(aHi, al); cMax = std::max(cMax, std::fabs(ac)); }
            if (aHi < aLo) { aLo = 0; aHi = 0.01f; }
            const float pad = (aHi - aLo) * 0.06f;
            aLo -= pad; aHi += pad;
            const float half = cMax * 1.08f / 0.84f, width = 2 * half, len = aHi - aLo;
            const int ox = k * CW;
            const auto toPx = [&](const std::array<float, 3>& p, float& px, float& py) {
                float al, ac;
                uv(p, al, ac);
                px = (ac + half) / width * CW + ox;
                py = (al - aLo) / len * CH;
            };
            // pass 1: contact shadows below every hair; pass 2: the hairs back to front, root dark -> tip lighter
            const float pxPerM = CW / width;
            for (int pass = 0; pass < 2; ++pass)
                for (const Hair* h : mine) {
                    const size_t n = h->pts.size();
                    const float w0 = std::max(1.0f, h->w * (h->wisp ? 0.045f : 0.065f) * pxPerM);
                    for (size_t i = 0; i + 1 < n; ++i) {
                        const float t = (float)i / (n - 1);
                        float ax, ay, bx, by;
                        toPx(h->pts[i], ax, ay);
                        toPx(h->pts[i + 1], bx, by);
                        uint8_t col[4];
                        float w;
                        if (pass == 0) {
                            ax -= 1.6f * S; bx -= 1.6f * S;
                            col[0] = 70; col[1] = 55; col[2] = 48; col[3] = (uint8_t)(22 * h->fade * (1 - t));
                            w = w0 * 2.4f * (1 - 0.6f * t);
                        } else {
                            const float ramp = 0.65f + 0.35f * std::pow(t, 0.8f), tone = h->tone * (0.7f + 0.3f * h->depth);
                            const float base[3] = {std::clamp(118 / dark * (1 + 0.12f * warm), 20.0f, 255.0f), std::clamp(78 / dark, 15.0f, 255.0f), std::clamp(50 / dark * (1 - 0.2f * warm), 10.0f, 255.0f)};
                            for (int c = 0; c < 3; ++c) col[c] = (uint8_t)std::min(255.0f, base[c] * tone * ramp + h->lift);
                            col[3] = (uint8_t)(255 * h->fade * (h->wisp ? 0.7f : 1.0f) * (1 - 0.6f * t * t));
                            w = w0 * (1 - 0.75f * std::pow(t, 1.4f));
                        }
                        Line(big, BW, CH, ax, ay, bx, by, std::max(1.0f, w), col);
                    }
                }
            // the card: a kAlong x kAcross grid (the importer's 8 quads), every vertex placed onto the skin, so a card follows the brow
            // ridge along it and, for a thick brow, across it too
            const float u0 = (float)k / kCards, u1 = (float)(k + 1) / kCards;
            Vertex grid[kAlong + 1][kAcross + 1];
            for (int r = 0; r <= kAlong; ++r)
                for (int c = 0; c <= kAcross; ++c) {
                    const float t = (float)r / kAlong, w = (float)c / kAcross * 2 - 1;
                    float p[3] = {A[0] + f[0] * (aLo + len * t) + side[0] * half * w, A[1] + f[1] * (aLo + len * t) + side[1] * half * w,
                                  A[2] + f[2] * (aLo + len * t) + side[2] * half * w};
                    float n[3] = {nA[0], nA[1], nA[2]};
                    snap(p, n);
                    Vertex& v = grid[r][c];
                    v = Vertex{};
                    v.px = p[0] + n[0] * kHug; v.py = p[1] + n[1] * kHug; v.pz = p[2] + n[2] * kHug;
                    v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
                    v.r = HairPackR(t, (float)k / kCards); v.g = HairPackG(kHairFine, 0.0f); v.b = 0.0f; v.a = kHairAlpha; // HairCardPS fine card, colour from the strand sheet (character/hair_vertex.h)
                    v.u = u0 + (u1 - u0) * (float)c / kAcross; v.v = t;
                }
            // between its vertices a quad can still cut into the brow ridge (a thick brow's wide card): where the skin under a quad's
            // centre or edge midpoints comes within 0.8 mm of it, lift that quad's corners clear (twice, for neighbours that moved)
            for (int pass = 0; pass < 2; ++pass) {
                float liftV[kAlong + 1][kAcross + 1] = {};
                for (int r = 0; r < kAlong; ++r)
                    for (int c = 0; c < kAcross; ++c) {
                        const Vertex* qv[4] = {&grid[r][c], &grid[r][c + 1], &grid[r + 1][c], &grid[r + 1][c + 1]};
                        float need = 0;
                        static const float wts[5][4] = {{.25f, .25f, .25f, .25f}, {.5f, .5f, 0, 0}, {0, 0, .5f, .5f}, {.5f, 0, .5f, 0}, {0, .5f, 0, .5f}};
                        for (const auto& wt : wts) {
                            float P[3] = {0, 0, 0}, N[3] = {0, 0, 0};
                            for (int i = 0; i < 4; ++i) { P[0] += qv[i]->px * wt[i]; P[1] += qv[i]->py * wt[i]; P[2] += qv[i]->pz * wt[i]; N[0] += qv[i]->nx * wt[i]; N[1] += qv[i]->ny * wt[i]; N[2] += qv[i]->nz * wt[i]; }
                            Norm(N);
                            const float o[3] = {P[0] + N[0] * 0.02f, P[1] + N[1] * 0.02f, P[2] + N[2] * 0.02f}, dd[3] = {-N[0], -N[1], -N[2]};
                            float hp[3], hn[3];
                            if (!cast(o, dd, hp, hn)) continue;
                            const float above = (hp[0] - P[0]) * N[0] + (hp[1] - P[1]) * N[1] + (hp[2] - P[2]) * N[2]; // skin over the card: > 0
                            need = std::max(need, above + 0.0008f);
                        }
                        if (need <= 0) continue;
                        need = std::min(need, 0.002f); // more and the card floats off the face over the eye
                        for (int i = 0; i < 2; ++i) for (int j2 = 0; j2 < 2; ++j2) liftV[r + i][c + j2] = std::max(liftV[r + i][c + j2], need);
                    }
                for (int r = 0; r <= kAlong; ++r)
                    for (int c = 0; c <= kAcross; ++c) {
                        Vertex& v = grid[r][c];
                        v.px += v.nx * liftV[r][c]; v.py += v.ny * liftV[r][c]; v.pz += v.nz * liftV[r][c];
                    }
            }
            for (int r = 0; r < kAlong; ++r)
                for (int c = 0; c < kAcross; ++c) {
                    const Vertex q[4] = {grid[r][c], grid[r][c + 1], grid[r + 1][c], grid[r + 1][c + 1]};
                    static const int front[6] = {0, 2, 1, 1, 2, 3}, back[6] = {0, 1, 2, 1, 3, 2};
                    for (int i : front) right.push_back(q[i]);
                    for (int i : back) { Vertex v = q[i]; v.nx = -v.nx; v.ny = -v.ny; v.nz = -v.nz; right.push_back(v); }
                }
        }
        // the importer's order: the brow on +x first (brow_cards.json's first 5 cards), then its mirror
        cards = right;
        for (size_t i = 0; i < right.size(); ++i) {
            Vertex v = right[i];
            v.px = -v.px; v.nx = -v.nx;
            // the mirror flips the card's across axis: swap its cell's two u edges so the top rows stay on top
            const int k = (int)(i / kVertsPerCard);
            const float u0 = (float)k / kCards, u1 = (float)(k + 1) / kCards;
            v.u = u0 + u1 - v.u;
            cards.push_back(v);
        }
        // 2x box downsample to the final texture (the full bake)
        const int TW = cellW * kCards, TH = cellH;
        texW = TW; texH = TH;
        if (S == 1) { tex = std::move(big); return true; } // preview: drawn at its final size
        tex.assign((size_t)TW * TH * 4, 0);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x) {
                int acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        const uint8_t* p = &big[(((size_t)(y * 2 + j)) * BW + (size_t)(x * 2 + i)) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    }
                uint8_t* o = &tex[((size_t)y * TW + x) * 4];
                for (int c = 0; c < 4; ++c) o[c] = (uint8_t)(acc[c] / 4);
            }
        return true;
    }

private:
    static constexpr float kHug = 0.0016f; // off the skin: the concave spots (by the nose, over the eye socket) must not poke through
    static void Norm(float v[3]) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9f) for (int c = 0; c < 3; ++c) v[c] /= l; }
    // A solid line of width w (no blending: it overwrites, like the offline bake), into an RGBA buffer.
    static void Line(std::vector<uint8_t>& img, int W, int H, float ax, float ay, float bx, float by, float w, const uint8_t col[4]) {
        const float r = w * 0.5f;
        const int x0 = std::max(0, (int)std::floor(std::min(ax, bx) - r)), x1 = std::min(W - 1, (int)std::ceil(std::max(ax, bx) + r));
        const int y0 = std::max(0, (int)std::floor(std::min(ay, by) - r)), y1 = std::min(H - 1, (int)std::ceil(std::max(ay, by) + r));
        const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f - ax, py = y + 0.5f - ay;
                const float t = l2 > 1e-9f ? std::clamp((px * dx + py * dy) / l2, 0.0f, 1.0f) : 0.0f;
                const float ex = px - dx * t, ey = py - dy * t;
                if (ex * ex + ey * ey > r * r) continue;
                uint8_t* p = &img[((size_t)y * W + x) * 4];
                p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = col[3];
            }
    }
};

} // namespace tbd::character
