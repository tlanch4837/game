// Editor-only drawing: ground grid, world axes, selection outline, orientation gizmo, stats text.
// None of this is counted in the draw/poly stats (those describe the scene itself).
#pragma once
#include <string>
#include <vector>

#include "render/renderer.h"
#include "scene.h"
#include "ui_draw.h"

namespace tbd {

namespace editor_draw {

inline void Line(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c) {
    out.push_back({a.x, a.y, a.z, 0, 0, 0, c.x, c.y, c.z, 1.0f});
    out.push_back({b.x, b.y, b.z, 0, 0, 0, c.x, c.y, c.z, 1.0f});
}

// Filled rectangle with notched corners (corner size n), as three quads: the overlay chips' pill shape.
inline void Pill(std::vector<Vertex>& out, float x, float y, float w, float h, float n, Vec3 c) {
    AppendQuad(out, x + n, y, x + w - n, y + h, c);
    AppendQuad(out, x, y + n, x + n, y + h - n, c);
    AppendQuad(out, x + w - n, y + n, x + w, y + h - n, c);
}

// A label chip: dark pill with padded text. Sizes follow the pixel font cell and the DPI scale `s`.
inline float ChipWidth(const std::string& text, float cell, float s) {
    return TextWidth(text, cell) - cell + 2.0f * std::round(6.0f * s); // the last glyph has no trailing gap
}
inline float ChipHeight(float cell, float s) { return 7.0f * cell + 2.0f * std::round(4.0f * s); }

// Draws the chip with its top-left corner at (x, y) and returns its width.
inline float Chip(std::vector<Vertex>& out, float x, float y, float cell, float s, const std::string& text, Vec3 fill,
                  Vec3 color) {
    const float w = ChipWidth(text, cell, s);
    Pill(out, x, y, w, ChipHeight(cell, s), std::round(3.0f * s), fill);
    AppendText(out, x + std::round(6.0f * s), y + std::round(4.0f * s), cell, text, color);
    return w;
}

} // namespace editor_draw

// Grid on the ground plane (slightly above it so the two never fight) and the three world axes.
inline void DrawGridAndAxes(Renderer& r, const Mat4& viewProj) {
    using editor_draw::Line;
    const float half = 10.0f, lift = 0.004f;
    std::vector<Vertex> v;
    for (int i = -10; i <= 10; ++i) {
        if (i == 0) continue; // the axes cover the center lines
        const Vec3 c = (i % 5 == 0) ? Vec3{0.42f, 0.42f, 0.42f} : Vec3{0.26f, 0.26f, 0.26f};
        Line(v, {(float)i, lift, -half}, {(float)i, lift, half}, c);
        Line(v, {-half, lift, (float)i}, {half, lift, (float)i}, c);
    }
    Line(v, {-half, lift, 0}, {half, lift, 0}, {0.78f, 0.28f, 0.24f});
    Line(v, {0, lift, -half}, {0, lift, half}, {0.27f, 0.45f, 0.84f});
    Line(v, {0, 0, 0}, {0, 3, 0}, {0.38f, 0.69f, 0.33f});
    r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Lines, viewProj, Mat4::Identity(), true);
}

// Outline of the entity's pickable box, drawn over everything.
inline void DrawSelectionOutline(Renderer& r, const Mat4& viewProj, const Entity& e) {
    using editor_draw::Line;
    const Vec3 h = LocalHalfExtents(e);
    Vec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = {(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
    std::vector<Vertex> v;
    const Vec3 color = {0.0f, 0.44f, 0.88f}; // the editor's accent blue (#0070e0)
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
            if (!(i & bit)) Line(v, c[i], c[i | bit], color);
    const Mat4 model = e.Model();
    r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Lines, viewProj * model, model, false);
}

// Little X/Y/Z compass in the bottom-left corner showing the camera's orientation.
inline void DrawOrientationGizmo(Renderer& r, const Mat4& view, int w, int h, int dpi) {
    using editor_draw::Line;
    const float s = (float)dpi / 96.0f;
    const float len = 30.0f * s, ox = 46.0f * s, oy = (float)h - 46.0f * s;
    const Vec3 colors[3] = {{0.78f, 0.28f, 0.24f}, {0.38f, 0.69f, 0.33f}, {0.27f, 0.45f, 0.84f}};
    const char* labels[3] = {"X", "Y", "Z"};
    std::vector<Vertex> lines, text;
    for (int axis = 0; axis < 3; ++axis) {
        const float tx = ox + view.At(0, axis) * len, ty = oy - view.At(1, axis) * len;
        Line(lines, {ox, oy, 0}, {tx, ty, 0}, colors[axis]);
        AppendText(text, tx + 3.0f * s, ty - 4.0f * s, std::fmax(1.0f, std::floor(s)), labels[axis], colors[axis]);
    }
    const Mat4 ortho = OrthoPixels((float)w, (float)h);
    r.DrawTransient(lines.data(), (uint32_t)lines.size(), Topology::Lines, ortho, Mat4::Identity(), false);
    r.DrawTransient(text.data(), (uint32_t)text.size(), Topology::Triangles, ortho, Mat4::Identity(), false);
}

// Viewport overlay. The lines come from the frame as plain text and are sorted by their prefix:
//   top-left chips:  camera mode (Perspective, or Fly while the right button flies) and the graphics API;
//   top-right chip:  FPS;
//   under the chips: the remaining stats as tiny text with a drop shadow so it reads on any background;
//   bottom-centre:   any other line (the toast messages) as an accent-blue chip.
// Returns the y just below the top-left block (chips and stats), so the in-game GUI can keep its panels out of it.
// showStats false (the game's Show stats off): only the toast chip is drawn, and nothing is reserved at the top (returns 0).
inline float DrawStatsText(Renderer& r, int w, int h, int dpi, const std::vector<std::string>& lines, bool showStats = true) {
    using editor_draw::Chip;
    using editor_draw::ChipHeight;
    using editor_draw::ChipWidth;
    const float s = (float)dpi / 96.0f;
    const float cell = std::fmax(1.0f, std::round(1.5f * s * 2.0f) / 2.0f); // tiny: 1.5 px font cells
    const float margin = std::round(8.0f * s), gap = std::round(6.0f * s);
    const Vec3 chipFill = {0.10f, 0.10f, 0.10f}, chipText = {0.85f, 0.85f, 0.85f};
    const Vec3 accent = {0.0f, 0.44f, 0.88f}, white = {1.0f, 1.0f, 1.0f};

    auto starts = [](const std::string& line, const char* prefix) { return line.rfind(prefix, 0) == 0; };
    std::string api, fps, fly, toast;
    std::vector<std::string> stats;
    for (const std::string& line : lines) {
        if (starts(line, "API ")) api = line;
        else if (starts(line, "FPS ")) fps = line;
        else if (starts(line, "FLY SPEED")) fly = line;
        else if (starts(line, "DRAWS") || starts(line, "POLYS") || starts(line, "CAM ") || starts(line, "SUN ") ||
                 starts(line, "TERRAIN"))
            stats.push_back(line);
        else toast = line; // anything else is a message; the last one wins
    }

    std::vector<Vertex> v;
    const float chipH = ChipHeight(cell, s);
    if (!showStats) {
        if (toast.empty()) return 0.0f;
        Chip(v, ((float)w - ChipWidth(toast, cell, s)) * 0.5f, (float)h - margin - chipH, cell, s, toast, accent, white);
        r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Triangles, OrthoPixels((float)w, (float)h), Mat4::Identity(), false);
        return 0.0f;
    }
    float x = margin;
    if (fly.empty()) x += Chip(v, x, margin, cell, s, "PERSPECTIVE", chipFill, chipText) + gap;
    else x += Chip(v, x, margin, cell, s, "FLY CAMERA -" + fly.substr(3), accent, white) + gap;
    if (!api.empty()) Chip(v, x, margin, cell, s, api, chipFill, chipText);
    if (!fps.empty()) Chip(v, (float)w - margin - ChipWidth(fps, cell, s), margin, cell, s, fps, chipFill, chipText);
    if (!toast.empty()) Chip(v, ((float)w - ChipWidth(toast, cell, s)) * 0.5f, (float)h - margin - chipH, cell, s, toast, accent, white);

    float bottom = margin + chipH;
    for (int pass = 0; pass < 2; ++pass) {
        const float off = pass == 0 ? cell * 0.5f : 0.0f;
        const Vec3 color = pass == 0 ? Vec3{0, 0, 0} : Vec3{0.80f, 0.80f, 0.80f};
        float y = margin + chipH + gap;
        for (const std::string& line : stats) {
            AppendText(v, margin + off, y + off, cell, line, color);
            y += 9.0f * cell;
            bottom = std::fmax(bottom, y);
        }
    }
    r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Triangles, OrthoPixels((float)w, (float)h),
                    Mat4::Identity(), false);
    return bottom;
}

} // namespace tbd
