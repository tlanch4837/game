// The road / rail tool's touch toolkit for the Glass Dock+ GUI (glass_dock.h), laid out like Cities: Skylines II's road
// tool panel (docs/ref_toolbar/cs2_road_toolbar_panel.png) with the road-type palette beside it (cs2_road_toolbar_in_game.png):
//   options panel (bottom left), top to bottom
//     header        Road Tools / Rail Tools, End (stop the chained road), Undo, Redo, Bulldoze, collapse
//     Tool Mode     straight, simple curve, complex curve, continuous, grid, replace (upgrade / flip), swap (drag the way)
//     Elevation     down chevron, the readout (metres, tunnel / cut / ground / bridge, the step), up chevron,
//                   step size (10 / 5 / 2.5 / 1.25 m), ground level (one tap back to 0 m); chevrons repeat while held
//     Parallel Mode switch, closer, the offset, further
//     Snapping      all, existing geometry, cell length, straight directions (15 / 90 deg), net (building) sides,
//                   guidelines, zone grid, contour lines
//     Grid Size     (grid mode) - / the spacing / +;  Rotation (intersections, roundabouts) left / angle / right
//     caption       what the last tap did (a tablet has no hover tooltips), or the rail limits
//   palette (bottom, right of the panel): the category tabs and a row of road-type tiles (drag to scroll).
// Collapsed it keeps the header and the Elevation row. Everything is a 44 pt target; nothing needs a keyboard.
// It edits the RoadTool directly; it draws into the GUI's one UI vertex list (no extra draw calls but the glass).
// Platform free (tests/road_toolkit_test.cpp drives it headless).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../city/road_tool.h"
#include "glass_dock.h"
#include "road_toolbar.h" // road_ui::DrawProfileTile: the tiles' top-down cross-sections

namespace tbd {
namespace gui {
namespace road_kit {

struct State {
    bool collapsed = false;
    // the palette's horizontal scroll (points) and its drag
    float scroll = 0, scrollMax = 0;
    bool armed = false, drag = false;
    float dragX = 0, dragScroll = 0;
    Rect tiles;       // pixels, last frame
    int lastTab = -1; // the palette scrolls home when the tab changes
    // press-and-hold repeat (the elevation chevrons)
    int holdId = -1, repeats = 0;
    float holdT = 0;
    // the caption: what the last tap did
    std::string caption;
    float captionT = 99;
};

struct Out {
    std::vector<float> sfx; // UI tick pitches (the host plays them like the dock's)
    bool changed = false;   // a control was used this frame
};

// The rectangles the toolkit takes this frame, in points (for tests and for the host's layout).
struct Layout {
    float panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    float palX = 0, palY = 0, palW = 0, palH = 0;
    bool palette = false;
};

constexpr float kPanelW = 404, kPad = 12, kB = 44, kGap = 4, kLabelH = 16, kRowGap = 8, kCaptionH = 18;
constexpr float kTileW = 88, kTileH = 72;

inline const char* TabAtlasIcon(city::Tab t) {
    static const char* const k[] = {"road_s", "road_m", "road_l", "highway", "quay", "path", "rail", "xing", "roundabout"};
    return k[(int)t];
}

// Heights of the panel's pieces: header, the four CS2 rows, the optional contextual row, the caption.
inline float PanelHeight(const city::RoadTool& t, bool collapsed) {
    const float row = kLabelH + kB + kRowGap;
    float h = kPad + kB + kRowGap + row; // header + elevation
    if (!collapsed) {
        h += 3 * row; // tool mode, parallel, snapping
        const bool prefab = t.Current().kind != city::AssetKind::Road;
        if (prefab || t.mode == city::ToolMode::Grid) h += row;
    }
    return h + kCaptionH + kPad - kRowGap;
}

// bottom: points from the screen's bottom edge to the panels' bottoms. tablet: false = a phone (collapsed, no palette).
inline Layout Place(const Context& g, const city::RoadTool& t, const State& st, float bottom, bool tablet) {
    const Input& in = g.In();
    const float s = g.Scale(), W = in.viewW / s, H = in.viewH / s;
    Layout L;
    L.panelW = kPanelW;
    L.panelH = PanelHeight(t, st.collapsed || !tablet);
    L.panelX = 12, L.panelY = H - bottom - L.panelH;
    if (!tablet) L.panelY = std::max(8.0f, L.panelY);
    L.palette = tablet && !st.collapsed;
    L.palH = kPad + kB + kRowGap + kTileH + kPad;
    L.palX = L.panelX + L.panelW + 10, L.palW = W - 12 - L.palX, L.palY = H - bottom - L.palH;
    if (L.palW < 300) { // a narrow screen: the palette goes above the panel
        L.palX = L.panelX, L.palW = std::min(W - 24, 700.0f), L.palY = L.panelY - 10 - L.palH;
        if (L.palY < 8) L.palette = false;
    }
    return L;
}

// One frame. atlas: the dock's (its tab icons). textScale: the dock's text size (Large text), capped so rows still fit.
inline Layout Frame(Context& g, const dock::Atlas& atlas, city::RoadTool& t, State& st, float bottom, bool tablet, Out& out, float textScale = 1.0f) {
    using namespace dock;
    out = Out{};
    Layout L = Place(g, t, st, bottom, tablet);
    if (!t.active) return L;
    const Input& in = g.In();
    const float s = g.Scale();
    Painter P(g, atlas, s);
    P.text = std::clamp(textScale, 1.0f, 1.15f);
    DrawList& dl = P.dl;
    auto R = [&](float x, float y, float w, float h) { return Rect{x * s, y * s, w * s, h * s}; };
    auto sfx = [&](float p) { out.sfx.push_back(p); };
    auto say = [&](const std::string& c) { st.caption = c, st.captionT = 0; };
    st.captionT += in.dt;

    // the palette's drag (decided before any tap so a scroll is never a pick)
    if (in.pressed) {
        st.drag = false;
        st.armed = st.tiles.Contains(in.pressX, in.pressY);
        st.dragX = in.pressX, st.dragScroll = st.scroll;
    }
    if (in.down && st.armed && st.scrollMax > 0) {
        const float d = in.mouseX - st.dragX;
        if (std::fabs(d) > 8 * s) st.drag = true;
        if (st.drag) st.scroll = std::clamp(st.dragScroll - d / s, 0.0f, st.scrollMax);
    }
    if (in.wheel != 0 && st.tiles.Contains(in.mouseX, in.mouseY)) st.scroll = std::clamp(st.scroll - in.wheel * 40, 0.0f, st.scrollMax);

    auto tap = [&](float x, float y, float w, float h, bool* held) {
        const Rect r = R(x, y, w, h);
        const bool inside = r.Contains(in.mouseX, in.mouseY), pressedIn = r.Contains(in.pressX, in.pressY);
        if (held) *held = in.down && pressedIn && inside && !st.drag;
        return in.released && pressedIn && inside && !st.drag;
    };
    auto box = [&](float x, float y, float w, float h, bool on, bool held, bool dim) {
        Color fill = on ? kAccent : held ? Rgba(255, 255, 255, 64) : kChip;
        if (dim) fill = Fade(fill, 0.45f);
        P.Box(x, y, w, h, 12, fill, on ? 0.0f : 1.0f, on ? 0u : Rgba(255, 255, 255, 26));
    };
    auto icon = [&](Icon ic, float x, float y, float w, float h, bool dim) {
        DrawIcon(dl, ic, (x + w * 0.5f) * s, (y + h * 0.5f) * s, std::round(std::min(w, h) * 0.5f * s), std::max(1.0f, 1.9f * s), dim ? Fade(kInk, 0.4f) : kInk);
    };
    // an icon button: true when tapped (never while dimmed)
    auto button = [&](float x, float y, float w, float h, Icon ic, bool on, bool dim = false) {
        bool held = false;
        const bool hit = tap(x, y, w, h, &held);
        box(x, y, w, h, on, held, dim);
        icon(ic, x, y, w, h, dim);
        return hit && !dim;
    };
    auto textButton = [&](float x, float y, float w, float h, const char* label, bool on, bool dim = false) {
        bool held = false;
        const bool hit = tap(x, y, w, h, &held);
        box(x, y, w, h, on, held, dim);
        P.Text(label, x + w * 0.5f, y + h * 0.5f, 12, dim ? Fade(kInk, 0.4f) : kInk, true, 0.5f);
        return hit && !dim;
    };
    // press and hold: one step on the tap, then a step every .12 s after .45 s
    auto repeatButton = [&](int id, float x, float y, float w, float h, Icon ic, bool dim) {
        bool held = false;
        const bool hit = tap(x, y, w, h, &held);
        box(x, y, w, h, false, held, dim);
        icon(ic, x, y, w, h, dim);
        if (dim) return 0;
        int steps = 0;
        if (held) {
            if (st.holdId != id) st.holdId = id, st.holdT = 0, st.repeats = 0;
            st.holdT += in.dt;
            const int due = st.holdT > 0.45f ? 1 + (int)((st.holdT - 0.45f) / 0.12f) : 0;
            while (st.repeats < due) ++st.repeats, ++steps;
        }
        if (hit) {
            if (!(st.holdId == id && st.repeats > 0)) ++steps;
            st.holdId = -1;
        }
        return steps;
    };
    auto label = [&](float x, float y, const char* text, const char* right = nullptr, Color rightCol = kMuted) {
        P.Text(text, x, y + kLabelH * 0.5f, 12, kInk, true);
        if (right) P.Text(right, x + kPanelW - 2 * kPad, y + kLabelH * 0.5f, 11, rightCol, true, 1.0f);
    };

    // ================================================================ the options panel
    const bool prefab = t.Current().kind != city::AssetKind::Road;
    const bool rail = t.IsRail();
    const bool compact = st.collapsed || !tablet;
    const float px = L.panelX, py = L.panelY, inner = kPanelW - 2 * kPad;
    P.Glass(px, py, L.panelW, L.panelH, 18, kPanel, 22, 1.6f, true);
    float x0 = px + kPad, y = py + kPad;
    // ---- header
    {
        P.Text(rail ? "Rail Tools" : "Road Tools", x0, y + kB * 0.5f, 14, kInk, true);
        float rx = x0 + inner;
        rx -= kB;
        if (tablet && button(rx, y, kB, kB, st.collapsed ? Icon::ChevronUp : Icon::ChevronDown, false)) {
            st.collapsed = !st.collapsed, sfx(st.collapsed ? 0.75f : 0.9f), out.changed = true;
            say(st.collapsed ? "Toolkit collapsed: elevation only" : "Toolkit expanded");
        }
        rx -= kGap + kB;
        const bool dozing = t.mode == city::ToolMode::Bulldoze;
        if (button(rx, y, kB, kB, Icon::Bulldoze, dozing)) {
            t.SetMode(dozing ? city::ToolMode::Straight : city::ToolMode::Bulldoze), sfx(1.1f), out.changed = true;
            say(dozing ? "Bulldoze off" : "Bulldoze: tap a road or track to remove it");
        }
        rx -= kGap + kB;
        { // redo: the undo glyph mirrored (gui.h has no redo icon)
            bool held = false;
            const bool hit = tap(rx, y, kB, kB, &held);
            const bool dim = !t.CanRedo();
            box(rx, y, kB, kB, false, held, dim);
            const float cx = (rx + kB * 0.5f) * s, cy = (y + kB * 0.5f) * s, h = std::round(kB * 0.25f * s), w = std::max(1.0f, 1.9f * s);
            const Color c = dim ? Fade(kInk, 0.4f) : kInk;
            float lx = -0.8f, ly = 0.6f;
            for (int i = 1; i <= 8; ++i) { // the quadratic (-.8,.6) (-.8,-.5) (.4,-.5)
                const float q = i / 8.0f, u = 1 - q, nx = u * u * -0.8f + 2 * u * q * -0.8f + q * q * 0.4f, ny = u * u * 0.6f + 2 * u * q * -0.5f + q * q * -0.5f;
                dl.Line(cx + lx * h, cy + ly * h, cx + nx * h, cy + ny * h, w, c);
                lx = nx, ly = ny;
            }
            dl.Line(cx + 0.4f * h, cy - 0.5f * h, cx + 0.1f * h, cy - 0.85f * h, w, c);
            dl.Line(cx + 0.4f * h, cy - 0.5f * h, cx + 0.1f * h, cy - 0.15f * h, w, c);
            if (hit && !dim && t.Redo()) sfx(0.8f), out.changed = true, say("Redo");
        }
        rx -= kGap + kB;
        if (button(rx, y, kB, kB, Icon::Undo, false, !t.CanUndo()) && t.Undo()) sfx(0.65f), out.changed = true, say("Undo");
        if (t.Pending()) {
            rx -= kGap + 56;
            if (textButton(rx, y, 56, kB, "End", false)) t.Cancel(), sfx(0.65f), out.changed = true, say("Road ended: the next tap starts a new one");
        }
        y += kB + kRowGap;
    }
    // ---- Tool Mode
    if (!compact) {
        static const Icon kModeIcons[] = {Icon::Straight, Icon::CurveSimple, Icon::CurveComplex, Icon::Continuous, Icon::Grid, Icon::Replace, Icon::Swap};
        static const char* const kModeTips[] = {"Straight: tap the start, tap the end (it chains on)", "Simple curve: start, bend point, end",
                                                "Complex curve: start, two bend points, end", "Continuous: curves that carry on tangent",
                                                "Grid: two taps for one side, a third for the depth", "Replace: tap a road to upgrade it to the picked type (again: flip it)",
                                                "Swap: press on a road, drag the way traffic should go, release"};
        label(x0, y, "Tool Mode", prefab ? "prefab: one tap places it" : city::ModeName(t.mode));
        y += kLabelH;
        for (int i = 0; i < 7; ++i) {
            const float bx = x0 + i * (kB + kGap);
            if (button(bx, y, kB, kB, kModeIcons[i], !prefab && (int)t.mode == i, prefab)) {
                t.SetMode((city::ToolMode)i), sfx(1.2f), out.changed = true, say(kModeTips[i]);
            }
        }
        y += kB + kRowGap;
    }
    // ---- Elevation: [v] [ readout ] [^]   [step] [0 m]
    {
        const float e = t.elevation;
        const char* state = t.InTunnel() ? "tunnel" : t.OnBridge() ? "bridge" : e < 0 ? "cut" : e > 0 ? "raised" : "ground";
        const Color stateCol = t.InTunnel() ? Rgba(140, 190, 255) : t.OnBridge() ? Rgba(255, 214, 120) : e < 0 ? Rgba(170, 215, 255) : kMuted;
        label(x0, y, "Elevation", state, stateCol);
        y += kLabelH;
        const float valueW = 132;
        float bx = x0;
        const int down = repeatButton(1, bx, y, kB, kB, Icon::ChevronDown, e <= city::RoadTool::kMinElevation);
        for (int i = 0; i < down; ++i) t.Raise(-1);
        if (down) sfx(0.9f), out.changed = true;
        bx += kB + kGap;
        P.Box(bx, y, valueW, kB, 12, Rgba(0, 0, 0, 70), 1, Rgba(255, 255, 255, 26));
        char v[32];
        std::snprintf(v, sizeof(v), "%g m", e);
        P.Text(v, bx + valueW * 0.5f, y + kB * 0.5f - 6, 16, t.InTunnel() ? Rgba(140, 190, 255) : t.OnBridge() ? Rgba(255, 214, 120) : kInk, true, 0.5f);
        std::snprintf(v, sizeof(v), "step %g m", t.Step());
        P.Text(v, bx + valueW * 0.5f, y + kB * 0.5f + 11, 10, kMuted, false, 0.5f);
        bx += valueW + kGap;
        const int up = repeatButton(2, bx, y, kB, kB, Icon::ChevronUp, e >= city::RoadTool::kMaxElevation);
        for (int i = 0; i < up; ++i) t.Raise(1);
        if (up) sfx(1.1f), out.changed = true;
        if (down || up) {
            std::snprintf(v, sizeof(v), "Elevation %g m%s", t.elevation, t.InTunnel() ? ": tunnel" : t.OnBridge() ? ": bridge" : t.elevation < 0 ? ": cut" : "");
            say(v);
        }
        bx = x0 + inner - kB;
        if (textButton(bx, y, kB, kB, "0 m", t.elevation == 0, false) && t.elevation != 0) t.GroundLevel(), sfx(0.8f), out.changed = true, say("Ground level");
        bx -= kGap + kB;
        if (button(bx, y, kB, kB, (Icon)((int)Icon::Steps1 + (3 - (t.stepIdx & 3))), false)) {
            t.CycleStep(), sfx(1.15f), out.changed = true;
            std::snprintf(v, sizeof(v), "Elevation step %g m", t.Step());
            say(v);
        }
        y += kB + kRowGap;
    }
    if (!compact) {
        // ---- Parallel Mode: [switch]  [closer] [ offset ] [further]
        {
            label(x0, y, "Parallel Mode", t.parallel ? "on" : "off", t.parallel ? Rgba(120, 200, 255) : kMuted);
            y += kLabelH;
            if (button(x0, y, kB, kB, Icon::Parallel, t.parallel)) {
                t.parallel = !t.parallel, sfx(1.2f), out.changed = true;
                say(t.parallel ? "Parallel mode: a second road to the right of each one" : "Parallel mode off");
            }
            const float valueW = 132;
            float bx = x0 + inner - (kB * 2 + valueW + kGap * 2);
            if (button(bx, y, kB, kB, Icon::ChevronDown, false, !t.parallel)) t.NudgeParallel(-1), sfx(0.9f), out.changed = true;
            bx += kB + kGap;
            P.Box(bx, y, valueW, kB, 12, Rgba(0, 0, 0, t.parallel ? 70 : 35), 1, Rgba(255, 255, 255, 26));
            char v[32];
            std::snprintf(v, sizeof(v), "%.0f m apart", t.ParallelOffset());
            P.Text(v, bx + valueW * 0.5f, y + kB * 0.5f, 13, t.parallel ? kInk : Fade(kInk, 0.4f), true, 0.5f);
            bx += valueW + kGap;
            if (button(bx, y, kB, kB, Icon::ChevronUp, false, !t.parallel)) t.NudgeParallel(1), sfx(1.1f), out.changed = true;
            y += kB + kRowGap;
        }
        // ---- Snapping: all + seven
        {
            struct S {
                Icon icon;
                uint32_t bit;
                const char* name;
            };
            static const S k[] = {{Icon::SnapAll, 0, "All snapping"},
                                  {Icon::SnapGeometry, city::SnapGeometry, "Existing geometry"},
                                  {Icon::SnapLength, city::SnapLength, "Cell length (8 m)"},
                                  {Icon::SnapStraight, city::SnapStraight, "Straight directions (15 / 90 deg)"},
                                  {Icon::SnapNetSide, city::SnapNetSide, "Building sides (zone-cell distances)"},
                                  {Icon::SnapGuide, city::SnapGuide, "Guidelines"},
                                  {Icon::SnapZone, city::SnapZone, "Zone grid"},
                                  {Icon::SnapContour, city::SnapContour, "Contour lines"}};
            int on = 0;
            for (int i = 1; i < 8; ++i) on += t.On(k[i].bit) ? 1 : 0;
            char right[24];
            std::snprintf(right, sizeof(right), t.snapAll ? "%d of 7 on" : "off", on);
            label(x0, y, "Snapping", right);
            y += kLabelH;
            for (int i = 0; i < 8; ++i) {
                const float bx = x0 + i * (kB + kGap);
                const bool lit = i == 0 ? t.snapAll : t.snapAll && (t.snaps & k[i].bit) != 0;
                if (button(bx, y, kB, kB, k[i].icon, lit, i > 0 && !t.snapAll)) {
                    if (i == 0) t.snapAll = !t.snapAll;
                    else t.ToggleSnap(k[i].bit);
                    const bool now = i == 0 ? t.snapAll : (t.snaps & k[i].bit) != 0;
                    sfx(now ? 1.25f : 0.85f), out.changed = true;
                    say(std::string(k[i].name) + (now ? ": on" : ": off"));
                }
            }
            y += kB + kRowGap;
        }
        // ---- the contextual row
        if (prefab || t.mode == city::ToolMode::Grid) {
            const float valueW = 132;
            char v[32];
            if (prefab) {
                label(x0, y, "Rotation");
                std::snprintf(v, sizeof(v), "%.0f deg", t.rotationDeg);
            } else {
                label(x0, y, "Grid Size");
                std::snprintf(v, sizeof(v), "%.0f m (%d cells)", t.gridSpacing, (int)std::lround(t.gridSpacing / 8.0f));
            }
            y += kLabelH;
            float bx = x0 + inner - (kB * 2 + valueW + kGap * 2);
            if (button(bx, y, kB, kB, prefab ? Icon::RotateLeft : Icon::ChevronDown, false)) {
                if (prefab) t.Rotate(15.0f);
                else t.NudgeGrid(-1);
                sfx(0.9f), out.changed = true;
            }
            bx += kB + kGap;
            P.Box(bx, y, valueW, kB, 12, Rgba(0, 0, 0, 70), 1, Rgba(255, 255, 255, 26));
            P.Text(v, bx + valueW * 0.5f, y + kB * 0.5f, 13, kInk, true, 0.5f);
            bx += valueW + kGap;
            if (button(bx, y, kB, kB, prefab ? Icon::RotateRight : Icon::ChevronUp, false)) {
                if (prefab) t.Rotate(-15.0f);
                else t.NudgeGrid(1);
                sfx(1.1f), out.changed = true;
            }
            y += kB + kRowGap;
        }
    }
    // ---- the caption: the last tap, else the error, else the rail limits / a hint
    {
        std::string c;
        Color col = kMuted;
        if (st.captionT < 3.0f && !st.caption.empty()) c = st.caption, col = Fade(kInk, std::min(1.0f, (3.0f - st.captionT) * 2.0f) * 0.9f);
        else if (!t.error.empty()) c = t.error, col = Rgba(255, 140, 120);
        else if (rail) c = "Rail: curves 45 m radius or wider, grades up to 6 %";
        else if (t.Pending()) c = "Tap to continue; End stops the road";
        P.Text(c, x0, y - kRowGap + kCaptionH * 0.5f + 2, 11, col, false, 0, inner);
    }

    // ================================================================ the palette
    st.tiles = {};
    if (L.palette) {
        const float ax = L.palX, ay = L.palY, aw = L.palW;
        P.Glass(ax, ay, aw, L.palH, 18, kPanel, 22, 1.6f, true);
        const float tx0 = ax + kPad;
        float ty = ay + kPad;
        // tabs
        const int nt = (int)city::Tab::Count;
        for (int i = 0; i < nt; ++i) {
            const float bx = tx0 + i * (kB + kGap);
            bool held = false;
            const bool on = (int)t.tab == i;
            if (tap(bx, ty, kB, kB, &held)) {
                t.SelectTab((city::Tab)i), sfx(1.2f), out.changed = true;
                say(city::TabName((city::Tab)i));
            }
            box(bx, ty, kB, kB, on, held, false);
            P.Icon(TabAtlasIcon((city::Tab)i), bx + kB * 0.5f, ty + kB * 0.5f, 26, kInk, !on);
        }
        // the picked type's name and numbers
        {
            const float nx = tx0 + nt * (kB + kGap) + 6, nw = ax + aw - kPad - nx;
            const city::Asset& a = t.Current();
            char line[96] = {};
            if (a.kind == city::AssetKind::Road) {
                const city::Profile& p = city::Profiles()[a.profile];
                if (p.cat == city::Cat::Rail) std::snprintf(line, sizeof(line), "%.1f m wide, min radius 45 m", p.width);
                else if (p.fwd + p.back > 0 && p.cat != city::Cat::Path) std::snprintf(line, sizeof(line), "%d+%d lanes, %.1f m wide%s", p.back, p.fwd, p.width, p.OneWay() ? ", one-way" : "");
                else std::snprintf(line, sizeof(line), "%.1f m wide", p.width);
            } else if (a.kind == city::AssetKind::Roundabout) std::snprintf(line, sizeof(line), "radius %.0f m", a.radius);
            else std::snprintf(line, sizeof(line), "%zu arms", a.armDeg.size());
            if (nw > 60) {
                P.Text(a.name, nx, ty + 13, 13, kInk, true, 0, nw);
                P.Text(line, nx, ty + 31, 11, kMuted, false, 0, nw);
            }
        }
        ty += kB + kRowGap;
        // tiles: one row, dragged sideways
        if (st.lastTab != (int)t.tab) st.lastTab = (int)t.tab, st.scroll = 0;
        const auto& assets = city::Assets();
        std::vector<int> list;
        for (size_t i = 0; i < assets.size(); ++i)
            if (assets[i].tab == t.tab) list.push_back((int)i);
        const float areaW = aw - 2 * kPad;
        const float contentW = list.size() * (kTileW + kGap) - kGap;
        st.scrollMax = std::max(0.0f, contentW - areaW);
        st.scroll = std::clamp(st.scroll, 0.0f, st.scrollMax);
        st.tiles = R(tx0, ty, areaW, kTileH);
        dl.SetClip(st.tiles.x, st.tiles.y, st.tiles.x + st.tiles.w, st.tiles.y + st.tiles.h);
        for (size_t k = 0; k < list.size(); ++k) {
            const int i = list[k];
            const city::Asset& a = assets[i];
            const float bx = tx0 + k * (kTileW + kGap) - st.scroll;
            if (bx + kTileW < tx0 || bx > tx0 + areaW) continue;
            bool held = false;
            const bool sel = i == t.asset;
            if (tap(bx, ty, kTileW, kTileH, &held) && st.tiles.Contains(in.mouseX, in.mouseY)) {
                t.Select(i), sfx(1.45f), out.changed = true;
                say(a.name);
            }
            P.Box(bx, ty, kTileW, kTileH, 12, sel ? Rgba(10, 132, 255, 80) : held ? Rgba(255, 255, 255, 40) : Rgba(255, 255, 255, 16), sel ? 2.0f : 1.0f,
                  sel ? kAccent : Rgba(255, 255, 255, 31));
            const Rect pic = R(bx + 4, ty + 4, kTileW - 8, kTileH - 26);
            if (a.kind == city::AssetKind::Road) road_ui::DrawProfileTile(dl, pic, city::Profiles()[a.profile], s);
            else DrawIcon(dl, a.kind == city::AssetKind::Roundabout ? Icon::Roundabout : Icon::Intersection, pic.x + pic.w * 0.5f, pic.y + pic.h * 0.5f, std::round(28 * s), std::max(1.0f, 2.0f * s), kInk);
            P.Text(a.name, bx + kTileW * 0.5f, ty + kTileH - 11, 9.5f, sel ? kInk : kBarInk, sel, 0.5f, kTileW - 6);
        }
        dl.ClearClip();
    }
    if (!in.down) st.holdId = -1, st.armed = false;
    return L;
}

} // namespace road_kit
} // namespace gui
} // namespace tbd
