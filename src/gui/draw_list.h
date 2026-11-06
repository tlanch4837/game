// The in-game GUI's output: backend-agnostic 2D primitives (rounded rectangle with gradient, border and radius; soft shadow;
// circle; capsule line; text run). In the smooth mode each primitive becomes one UiVertex quad for Renderer::DrawUi
// (shaders/ui.hlsl draws the shape by signed distance). In the classic mode, used when the UI pipeline is unavailable or
// TBD_GUI_CLASSIC=1, the same calls become flat Vertex quads and the 5x7 pixel font for DrawTransient (no alpha, no
// shadows, square corners). Platform free; both vectors are reused frame to frame.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../render/ui_vertex.h"
#include "../ui_draw.h"
#include "ui_font.h"

namespace tbd {
namespace gui {

using Color = uint32_t; // RGBA8, R in the low byte (UiRgba)

constexpr Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return UiRgba(r, g, b, a); }
inline float Channel(Color c, int i) { return (float)((c >> (8 * i)) & 0xFF); }
// Linear blend of two colours (alpha too), t clamped to 0..1.
inline Color Mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    Color out = 0;
    for (int i = 0; i < 4; ++i) out |= (Color)std::lround(Channel(a, i) + (Channel(b, i) - Channel(a, i)) * t) << (8 * i);
    return out;
}
// The colour with its alpha multiplied by f.
inline Color Fade(Color c, float f) {
    return (c & 0x00FFFFFFu) | ((Color)std::lround(Channel(c, 3) * std::clamp(f, 0.0f, 1.0f)) << 24);
}

class DrawList {
public:
    void Begin(bool classic) {
        classic_ = classic;
        ui_.clear();
        flat_.clear();
    }
    bool Classic() const { return classic_; }
    const std::vector<UiVertex>& Ui() const { return ui_; }
    const std::vector<Vertex>& Flat() const { return flat_; }
    size_t Size() const { return classic_ ? flat_.size() : ui_.size(); }

    // ---------------------------------------------------------------- text metrics (match what Text() draws)
    static float ClassicCell(float size) { return std::max(1.0f, std::round(size / 7.0f * 2.0f) / 2.0f); } // 7 rows tall
    float TextWidth(const char* s, size_t n, float size, FontFace face = FontFace::Regular) const {
        if (n == 0) return 0.0f;
        if (classic_) {
            const float cell = ClassicCell(size);
            return (float)n * 6.0f * cell - cell;
        }
        return MeasureSdfText(s, n, size, face);
    }
    float CapHeight(float size, FontFace face = FontFace::Regular) const {
        return classic_ ? 7.0f * ClassicCell(size) : std::round(SdfCapHeight(size, face));
    }

    // ---------------------------------------------------------------- primitives
    // Rounded rectangle, vertical gradient top -> bottom, optional inside border.
    void Rect(float x0, float y0, float x1, float y1, Color top, Color bottom, float radius, float border = 0.0f, Color borderColor = 0) {
        if (classic_) {
            flat_.resize(flat_.size() + 12);
            ClassicRect(flat_.data() + flat_.size() - 12, x0, y0, x1, y1, top, bottom, border, borderColor);
        } else {
            ui_.resize(ui_.size() + 6);
            SmoothRect(ui_.data() + ui_.size() - 6, x0, y0, x1, y1, top, bottom, radius, border, borderColor);
        }
    }
    void Rect(float x0, float y0, float x1, float y1, Color fill, float radius) { Rect(x0, y0, x1, y1, fill, fill, radius); }

    // Soft shadow of a rounded rectangle: `blur` pixels of falloff on each side of its edge. Nothing in classic mode.
    void Shadow(float x0, float y0, float x1, float y1, float radius, float blur, Color color) {
        if (classic_) return;
        ui_.resize(ui_.size() + 6);
        SmoothShadow(ui_.data() + ui_.size() - 6, x0, y0, x1, y1, radius, blur, color);
    }

    void Circle(float cx, float cy, float r, Color fill, float border = 0.0f, Color borderColor = 0) {
        Rect(cx - r, cy - r, cx + r, cy + r, fill, fill, r, border, borderColor);
    }

    // Stroke from (x0, y0) to (x1, y1) with round caps: the icons are built from these.
    void Line(float x0, float y0, float x1, float y1, float thickness, Color color) {
        if (classic_) { // stamp squares along it: chunky, like the rest of the classic look
            const float len = std::hypot(x1 - x0, y1 - y0), half = std::max(0.5f, std::round(thickness * 0.5f));
            const int steps = std::max(1, (int)std::ceil(len));
            for (int i = 0; i <= steps; ++i) {
                const float t = (float)i / steps, x = std::round(x0 + (x1 - x0) * t), y = std::round(y0 + (y1 - y0) * t);
                AppendQuad(flat_, x - half, y - half, x + half, y + half, ToVec3(color));
            }
            return;
        }
        const float r = thickness * 0.5f, m = r + 1.0f;
        ui_.resize(ui_.size() + 6);
        Quad(ui_.data() + ui_.size() - 6, std::min(x0, x1) - m, std::min(y0, y1) - m, std::max(x0, x1) + m, std::max(y0, y1) + m,
             x0, y0, x1 - x0, y1 - y0, r, 0.0f, UiMode::Line, 0.0f, color, color, 0);
    }

    // A run of text with its pen starting at x on the baseline. Snapped to whole pixels so it stays crisp.
    void Text(float x, float baseline, float size, FontFace face, const char* s, size_t n, Color color) {
        x = std::round(x);
        baseline = std::round(baseline);
        if (classic_) {
            const float cell = ClassicCell(size);
            AppendTextN(flat_, x, baseline - 7.0f * cell, cell, s, n, ToVec3(color), true);
            return;
        }
        const float iw = 1.0f / font_data::kAtlasWidth, ih = 1.0f / font_data::kAtlasHeight;
        float pen = x;
        for (size_t i = 0; i < n; ++i) {
            const font_data::Glyph* g = FindSdfGlyph(face, s[i]);
            if (g && g->x1 > g->x0) {
                ui_.resize(ui_.size() + 6);
                UiVertex* q = ui_.data() + ui_.size() - 6;
                const float qx0 = pen + g->x0 * size, qy0 = baseline + g->y0 * size, qx1 = pen + g->x1 * size, qy1 = baseline + g->y1 * size;
                const float p[4][4] = {{qx0, qy0, g->u0 * iw, g->v0 * ih}, {qx1, qy0, g->u1 * iw, g->v0 * ih},
                                       {qx1, qy1, g->u1 * iw, g->v1 * ih}, {qx0, qy1, g->u0 * iw, g->v1 * ih}};
                const int order[6] = {0, 1, 2, 0, 2, 3};
                for (int k = 0; k < 6; ++k) {
                    const float* v = p[order[k]];
                    q[k] = {v[0], v[1], v[2], v[3], 0.0f, 0.0f, 0.0f, 0.0f, (float)UiMode::Text, 1.0f, color, 0};
                }
            }
            pen += (g ? g->advance : SdfAdvance(face, ' ')) * size;
            if (i + 1 < n) pen += SdfKerning(face, s[i], s[i + 1]) * size;
        }
    }

    // ---------------------------------------------------------------- placeholders
    // Panel backgrounds are drawn first but sized last: Reserve() keeps room for `count` rectangles / shadows, filled in
    // later with RectAt / ShadowAt (unfilled ones stay zero-area and draw nothing).
    size_t Reserve(int count) {
        const size_t at = Size();
        if (classic_) flat_.resize(flat_.size() + 12 * (size_t)count, Vertex{});
        else ui_.resize(ui_.size() + 6 * (size_t)count, UiVertex{});
        return at;
    }
    size_t SlotSize() const { return classic_ ? 12 : 6; } // vertices per reserved primitive
    void RectAt(size_t at, float x0, float y0, float x1, float y1, Color top, Color bottom, float radius, float border = 0.0f, Color borderColor = 0) {
        if (classic_) ClassicRect(flat_.data() + at, x0, y0, x1, y1, top, bottom, border, borderColor);
        else SmoothRect(ui_.data() + at, x0, y0, x1, y1, top, bottom, radius, border, borderColor);
    }
    void ShadowAt(size_t at, float x0, float y0, float x1, float y1, float radius, float blur, Color color) {
        if (!classic_) SmoothShadow(ui_.data() + at, x0, y0, x1, y1, radius, blur, color);
        else FlatQuad(flat_.data() + at, x0, y0, x0, y0, 0), FlatQuad(flat_.data() + at + 6, x0, y0, x0, y0, 0); // no shadows: zero area, in place
    }

private:
    static Vec3 ToVec3(Color c) { return {Channel(c, 0) / 255.0f, Channel(c, 1) / 255.0f, Channel(c, 2) / 255.0f}; }

    // One quad (x0..x1, y0..y1); local coordinates are the position minus (ox, oy). Top two vertices get `top`, bottom two `bottom`.
    static void Quad(UiVertex* q, float x0, float y0, float x1, float y1, float ox, float oy, float a, float b, float radius, float border,
                     UiMode mode, float softness, Color top, Color bottom, Color line) {
        const float p[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k) {
            const int i = order[k];
            q[k] = {p[i][0], p[i][1], p[i][0] - ox, p[i][1] - oy, a, b, radius, border, (float)mode, softness, i < 2 ? top : bottom, line};
        }
    }

    static void SmoothRect(UiVertex* q, float x0, float y0, float x1, float y1, Color top, Color bottom, float radius, float border, Color borderColor) {
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = std::max(0.0f, (x1 - x0) * 0.5f), hy = std::max(0.0f, (y1 - y0) * 0.5f);
        const float m = 1.0f; // one pixel of margin for the anti-aliased edge
        Quad(q, x0 - m, y0 - m, x1 + m, y1 + m, cx, cy, hx, hy, radius, border, UiMode::Rect, 0.0f, top, bottom, borderColor);
    }

    static void SmoothShadow(UiVertex* q, float x0, float y0, float x1, float y1, float radius, float blur, Color color) {
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, m = blur + 1.0f;
        Quad(q, x0 - m, y0 - m, x1 + m, y1 + m, cx, cy, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, radius, 0.0f, UiMode::Shadow, blur, color, color, 0);
    }

    static void FlatQuad(Vertex* q, float x0, float y0, float x1, float y1, Color c) {
        const Vec3 v = ToVec3(c);
        const float p[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k) q[k] = {p[order[k]][0], p[order[k]][1], 0, 0, 0, 0, v.x, v.y, v.z, 1.0f};
    }

    // Classic: an optional border quad, then the body (the gradient's middle colour); square corners, alpha ignored.
    static void ClassicRect(Vertex* q, float x0, float y0, float x1, float y1, Color top, Color bottom, float border, Color borderColor) {
        x0 = std::round(x0), y0 = std::round(y0), x1 = std::round(x1), y1 = std::round(y1);
        const float b = border > 0.0f ? std::max(1.0f, std::round(border)) : 0.0f;
        if (b > 0.0f) FlatQuad(q, x0, y0, x1, y1, borderColor);
        else FlatQuad(q, x0, y0, x0, y0, top); // zero area
        FlatQuad(q + 6, x0 + b, y0 + b, x1 - b, y1 - b, Mix(top, bottom, 0.5f));
    }

    bool classic_ = false;
    std::vector<UiVertex> ui_;
    std::vector<Vertex> flat_;
};

} // namespace gui
} // namespace tbd
