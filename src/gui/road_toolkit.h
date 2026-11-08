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
//
// Mini (State::mini, the default on touch: the host sets it): the whole kit is two slim 52 pt strips along the bottom so
// the city stays in view (the user's "as compact as possible" ask):
//   tools    close, tool mode (a popover row of the 7 modes) or the prefab's rotation, the elevation chip (down, the
//            readout - a tap cycles the step - , up, and 0 when off the ground), End, snapping (a popover row of all + 8),
//            parallel, undo, redo, bulldoze, the aim dot (touch), expand (back to the full panel)
//   palette  the tab (a popover row of the 9 tabs) and one row of icon tiles (drag to scroll)
// The caption floats above as a small pill for 3 s. Targets are 40 pt. Both strips keep inside State::safe.
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
#include "safe_area.h"

namespace tbd {
namespace gui {
namespace road_kit {

struct State {
    bool mini = false;     // the slim strips (touch default: the host sets it once)
    SafeInsets safe;       // points, the host sets it every frame
    bool dotAvail = false; // the aim dot exists here (touch): its toggle shows
    bool dotOn = true;     // the player's toggle: the screen centre is the cursor (gui/road_reticle.h)
    int pop = 0;           // the mini strip's open popover: 0 none, 1 tool modes, 2 snapping, 3 tabs
    float popX = 0;        // its anchor (points)
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
    bool close = false;     // the mini strip's close: the host closes the road tool
};

// The rectangles the toolkit takes this frame, in points (for tests and for the host's layout).
struct Layout {
    float panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    float palX = 0, palY = 0, palW = 0, palH = 0;
    bool palette = false;
    float top = 0; // the highest point the kit reaches (popovers and the caption aside): the reticle's buttons stay above
    float rightTop = 0; // ... at the screen's right side (the palette strip, or the panel when the palette sits above it)
};

constexpr float kPanelW = 452, kPad = 12, kB = 44, kGap = 4, kLabelH = 16, kRowGap = 8, kCaptionH = 18;
constexpr float kTileW = 88, kTileH = 72;
// mini: 40 pt targets in 52 pt strips
constexpr float kMB = 40, kMPad = 6, kMGap = 4, kMH = kMB + 2 * kMPad, kMTileW = 60, kMValueW = 64;

// The snapping toggles, in the row's order (the first is "all")
struct SnapDef {
    Icon icon;
    uint32_t bit;
    const char* name;
};
inline const SnapDef* Snaps() {
    static const SnapDef k[] = {{Icon::SnapAll, 0, "All snapping"},
                                {Icon::SnapGeometry, city::SnapGeometry, "Existing geometry"},
                                {Icon::SnapLength, city::SnapLength, "Cell length (8 m)"},
                                {Icon::SnapStraight, city::SnapStraight, "Straight directions (15 / 90 deg)"},
                                {Icon::SnapNetSide, city::SnapNetSide, "Building sides (zone-cell distances)"},
                                {Icon::SnapGuide, city::SnapGuide, "Guidelines"},
                                {Icon::SnapZone, city::SnapZone, "Zone grid"},
                                {Icon::SnapContour, city::SnapContour, "Contour lines"},
                                {Icon::None, city::SnapConnect, "Connections: ends, nodes, mid-points, aligned (rail: switches)"}};
    return k;
}
constexpr int kSnapCount = 9;

// The connection snap's glyph (gui.h has none): a road end meeting a node ring, with a branch.
inline void DrawConnectGlyph(DrawList& dl, float cx, float cy, float size, float stroke, Color c) {
    const float h = size * 0.5f;
    dl.Line(cx - h, cy + h * 0.55f, cx - h * 0.18f, cy + h * 0.02f, stroke, c);
    dl.Line(cx + h * 0.2f, cy - h * 0.2f, cx + h, cy - h * 0.75f, stroke, c);
    dl.Line(cx + h * 0.22f, cy + h * 0.12f, cx + h * 0.95f, cy + h * 0.55f, stroke, c);
    dl.Circle(cx, cy, h * 0.3f, 0, stroke, c);
}
// The aim dot's glyph: a ring with a centre dot and four ticks.
inline void DrawDotGlyph(DrawList& dl, float cx, float cy, float size, float stroke, Color c) {
    const float h = size * 0.5f;
    dl.Circle(cx, cy, h * 0.55f, 0, stroke, c);
    dl.Circle(cx, cy, std::max(1.5f, stroke * 1.1f), c);
    dl.Line(cx - h, cy, cx - h * 0.7f, cy, stroke, c), dl.Line(cx + h * 0.7f, cy, cx + h, cy, stroke, c);
    dl.Line(cx, cy - h, cx, cy - h * 0.7f, stroke, c), dl.Line(cx, cy + h * 0.7f, cx, cy + h, stroke, c);
}

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

// The mini tool strip's items, left to right (Place and Frame share it).
enum MiniItem { kClose, kMode, kRotL, kRotV, kRotR, kDown, kValue, kUp, kZero, kEnd, kSnap, kPar, kUndo, kRedo, kDoze, kDot, kExpand };
struct MiniSlot {
    MiniItem id;
    float w;
};
inline std::vector<MiniSlot> MiniItems(const city::RoadTool& t, const State& st, bool tablet) {
    const bool prefab = t.Current().kind != city::AssetKind::Road;
    std::vector<MiniSlot> v = {{kClose, kMB}};
    if (prefab) v.push_back({kRotL, kMB}), v.push_back({kRotV, 52}), v.push_back({kRotR, kMB});
    else v.push_back({kMode, kMB});
    v.push_back({kDown, kMB}), v.push_back({kValue, kMValueW}), v.push_back({kUp, kMB});
    if (t.elevation != 0) v.push_back({kZero, kMB});
    if (t.Pending()) v.push_back({kEnd, 48});
    v.push_back({kSnap, kMB});
    if (!prefab) v.push_back({kPar, kMB});
    v.push_back({kUndo, kMB}), v.push_back({kRedo, kMB});
    if (!prefab) v.push_back({kDoze, kMB});
    if (st.dotAvail) v.push_back({kDot, kMB});
    if (tablet) v.push_back({kExpand, kMB});
    return v;
}
inline float MiniWidth(const std::vector<MiniSlot>& v) {
    float w = 2 * kMPad - kMGap;
    for (const MiniSlot& m : v) w += m.w + kMGap;
    return w;
}

// bottom: points from the screen's bottom edge to the panels' bottoms (the host's dock, safe area included; the safe
// area's bottom is kept regardless). tablet: false = a phone (collapsed, no palette).
inline Layout Place(const Context& g, const city::RoadTool& t, const State& st, float bottom, bool tablet) {
    const Input& in = g.In();
    const float s = g.Scale(), W = in.viewW / s, H = in.viewH / s;
    const SafeInsets& sa = st.safe;
    const float left = 12 + sa.L(), right = W - 12 - sa.R(), top = 8 + sa.T();
    bottom = std::max(bottom, sa.B() + 8);
    Layout L;
    if (st.mini) {
        L.panelW = std::min(MiniWidth(MiniItems(t, st, tablet)), right - left);
        L.panelH = kMH;
        L.panelX = left, L.panelY = H - bottom - kMH;
        L.palette = true;
        L.palH = kMH;
        L.palX = L.panelX + L.panelW + 8, L.palW = right - L.palX, L.palY = L.panelY;
        if (L.palW < 260) { // narrow: the palette strip rides above the tools
            L.palX = left, L.palW = std::min(right - left, 700.0f), L.palY = L.panelY - 8 - kMH;
            if (L.palY < top) L.palette = false;
        }
        L.top = L.palette ? std::min(L.panelY, L.palY) : L.panelY;
        L.rightTop = L.palette ? L.palY : L.panelY;
        return L;
    }
    L.panelW = kPanelW;
    L.panelH = PanelHeight(t, st.collapsed || !tablet);
    L.panelX = left, L.panelY = H - bottom - L.panelH;
    if (!tablet) L.panelY = std::max(top, L.panelY);
    L.palette = tablet && !st.collapsed;
    L.palH = kPad + kB + kRowGap + kTileH + kPad;
    L.palX = L.panelX + L.panelW + 10, L.palW = right - L.palX, L.palY = H - bottom - L.palH;
    if (L.palW < 300) { // a narrow screen: the palette goes above the panel
        L.palX = L.panelX, L.palW = std::min(right - left, 700.0f), L.palY = L.panelY - 10 - L.palH;
        if (L.palY < top) L.palette = false;
    }
    L.top = L.palette ? std::min(L.panelY, L.palY) : L.panelY;
    L.rightTop = L.palette && L.palX > L.panelX ? L.palY : L.top;
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

    // the redo glyph: the undo glyph mirrored (gui.h has no redo icon)
    auto redoGlyph = [&](float x, float y, float b, bool dim) {
        const float cx = (x + b * 0.5f) * s, cy = (y + b * 0.5f) * s, h = std::round(b * 0.25f * s), w = std::max(1.0f, 1.9f * s);
        const Color c = dim ? Fade(kInk, 0.4f) : kInk;
        float lx = -0.8f, ly = 0.6f;
        for (int i = 1; i <= 8; ++i) { // the quadratic (-.8,.6) (-.8,-.5) (.4,-.5)
            const float q = i / 8.0f, u = 1 - q, nx = u * u * -0.8f + 2 * u * q * -0.8f + q * q * 0.4f, ny = u * u * 0.6f + 2 * u * q * -0.5f + q * q * -0.5f;
            dl.Line(cx + lx * h, cy + ly * h, cx + nx * h, cy + ny * h, w, c);
            lx = nx, ly = ny;
        }
        dl.Line(cx + 0.4f * h, cy - 0.5f * h, cx + 0.1f * h, cy - 0.85f * h, w, c);
        dl.Line(cx + 0.4f * h, cy - 0.5f * h, cx + 0.1f * h, cy - 0.15f * h, w, c);
    };
    const bool prefab = t.Current().kind != city::AssetKind::Road;
    const bool rail = t.IsRail();
    // ================================================================ mini: two slim strips
    if (st.mini) {
        const float W = in.viewW / s;
        const SafeInsets& sa = st.safe;
        auto elevSay = [&] {
            char v[48];
            std::snprintf(v, sizeof(v), "Elevation %g m%s", t.elevation, t.InTunnel() ? ": tunnel" : t.OnBridge() ? ": bridge" : t.elevation < 0 ? ": cut" : "");
            say(v);
        };
        // ---- the tools strip
        const std::vector<MiniSlot> items = MiniItems(t, st, tablet);
        const float sy = L.panelY + kMPad;
        P.Glass(L.panelX, L.panelY, L.panelW, L.panelH, 16, kPanel, 22, 1.6f, true);
        float bx = L.panelX + kMPad;
        float modeX = bx, snapX = bx;
        for (const MiniSlot& it : items) {
            if (bx + it.w > L.panelX + L.panelW - kMPad + 0.5f) break; // a very narrow screen: what does not fit is in the full panel
            switch (it.id) {
            case kClose:
                { // [user] a red down arrow (hides the kit and ends the tool), not an X
                    bool held = false;
                    const bool hit = tap(bx, sy, kMB, kMB, &held);
                    box(bx, sy, kMB, kMB, false, held, false);
                    DrawIcon(dl, Icon::ChevronDown, (bx + kMB * 0.5f) * s, (sy + kMB * 0.5f) * s, std::round(kMB * 0.5f * s), std::max(1.0f, 2.8f * s), Rgba(255, 69, 58));
                    if (hit) out.close = true, sfx(0.65f), out.changed = true;
                }
                break;
            case kMode: {
                static const Icon kModeIcons[] = {Icon::Straight, Icon::CurveSimple, Icon::CurveComplex, Icon::Continuous, Icon::Grid, Icon::Replace, Icon::Swap, Icon::Bulldoze};
                modeX = bx;
                if (button(bx, sy, kMB, kMB, kModeIcons[std::clamp((int)t.mode, 0, 7)], st.pop == 1)) st.pop = st.pop == 1 ? 0 : 1, st.popX = bx, sfx(1.0f), out.changed = true;
                break;
            }
            case kRotL:
                if (button(bx, sy, kMB, kMB, Icon::RotateLeft, false)) t.Rotate(15.0f), sfx(0.9f), out.changed = true;
                break;
            case kRotV: {
                char v[16];
                std::snprintf(v, sizeof(v), "%.0f deg", t.rotationDeg);
                P.Box(bx, sy, it.w, kMB, 10, Rgba(0, 0, 0, 70), 1, Rgba(255, 255, 255, 26));
                P.Text(v, bx + it.w * 0.5f, sy + kMB * 0.5f, 11, kInk, true, 0.5f);
                break;
            }
            case kRotR:
                if (button(bx, sy, kMB, kMB, Icon::RotateRight, false)) t.Rotate(-15.0f), sfx(1.1f), out.changed = true;
                break;
            case kDown: {
                const int n = repeatButton(1, bx, sy, kMB, kMB, Icon::ChevronDown, t.elevation <= city::RoadTool::kMinElevation);
                for (int i = 0; i < n; ++i) t.Raise(-1);
                if (n) sfx(0.9f), out.changed = true, elevSay();
                break;
            }
            case kValue: { // the readout; a tap cycles the step
                bool held = false;
                const bool hit = tap(bx, sy, it.w, kMB, &held);
                const float e = t.elevation;
                const Color col = t.InTunnel() ? Rgba(140, 190, 255) : t.OnBridge() ? Rgba(255, 214, 120) : e < 0 ? Rgba(170, 215, 255) : kInk;
                P.Box(bx, sy, it.w, kMB, 10, held ? Rgba(255, 255, 255, 50) : Rgba(0, 0, 0, 70), 1, Rgba(255, 255, 255, 26));
                char v[24];
                std::snprintf(v, sizeof(v), "%g m", e);
                P.Text(v, bx + it.w * 0.5f, sy + kMB * 0.5f - 6, 14, col, true, 0.5f, it.w - 4);
                std::snprintf(v, sizeof(v), "step %g", t.Step());
                P.Text(v, bx + it.w * 0.5f, sy + kMB * 0.5f + 10, 9, kMuted, false, 0.5f, it.w - 4);
                if (hit) {
                    t.CycleStep(), sfx(1.15f), out.changed = true;
                    std::snprintf(v, sizeof(v), "Elevation step %g m", t.Step());
                    say(v);
                }
                break;
            }
            case kUp: {
                const int n = repeatButton(2, bx, sy, kMB, kMB, Icon::ChevronUp, t.elevation >= city::RoadTool::kMaxElevation);
                for (int i = 0; i < n; ++i) t.Raise(1);
                if (n) sfx(1.1f), out.changed = true, elevSay();
                break;
            }
            case kZero:
                if (textButton(bx, sy, kMB, kMB, "0 m", false)) t.GroundLevel(), sfx(0.8f), out.changed = true, say("Ground level");
                break;
            case kEnd:
                if (textButton(bx, sy, it.w, kMB, "End", false)) t.Cancel(), sfx(0.65f), out.changed = true, say(rail ? "Track ended" : "Road ended");
                break;
            case kSnap:
                snapX = bx;
                if (button(bx, sy, kMB, kMB, Icon::SnapAll, t.snapAll || st.pop == 2)) st.pop = st.pop == 2 ? 0 : 2, st.popX = bx, sfx(1.0f), out.changed = true;
                break;
            case kPar:
                if (button(bx, sy, kMB, kMB, Icon::Parallel, t.parallel)) {
                    t.parallel = !t.parallel, sfx(1.2f), out.changed = true;
                    say(t.parallel ? "Parallel: a second road to the right" : "Parallel off");
                }
                break;
            case kUndo:
                if (button(bx, sy, kMB, kMB, Icon::Undo, false, !t.CanUndo()) && t.Undo()) sfx(0.65f), out.changed = true, say("Undo");
                break;
            case kRedo: {
                bool held = false;
                const bool hit = tap(bx, sy, kMB, kMB, &held);
                const bool dim = !t.CanRedo();
                box(bx, sy, kMB, kMB, false, held, dim);
                redoGlyph(bx, sy, kMB, dim);
                if (hit && !dim && t.Redo()) sfx(0.8f), out.changed = true, say("Redo");
                break;
            }
            case kDoze: {
                const bool dozing = t.mode == city::ToolMode::Bulldoze;
                if (button(bx, sy, kMB, kMB, Icon::Bulldoze, dozing)) {
                    t.SetMode(dozing ? city::ToolMode::Straight : city::ToolMode::Bulldoze), sfx(1.1f), out.changed = true;
                    say(dozing ? "Bulldoze off" : rail ? "Bulldoze: aim at a track" : "Bulldoze: aim at a road");
                }
                break;
            }
            case kDot: {
                bool held = false;
                const bool hit = tap(bx, sy, kMB, kMB, &held);
                box(bx, sy, kMB, kMB, st.dotOn, held, false);
                DrawDotGlyph(dl, (bx + kMB * 0.5f) * s, (sy + kMB * 0.5f) * s, std::round(kMB * 0.5f * s), std::max(1.0f, 1.9f * s), kInk);
                if (hit) {
                    st.dotOn = !st.dotOn, sfx(st.dotOn ? 1.25f : 0.85f), out.changed = true;
                    say(st.dotOn ? "Aim dot: drag the map under the dot, tick to place" : "Aim dot off: tap the map to place");
                }
                break;
            }
            case kExpand:
                if (button(bx, sy, kMB, kMB, Icon::ChevronUp, false)) st.mini = false, st.collapsed = false, st.pop = 0, sfx(0.9f), out.changed = true, say("Full toolkit");
                break;
            }
            bx += it.w + kMGap;
        }
        // ---- the palette strip: the tab, then one row of tiles
        st.tiles = {};
        float tabX = L.palX + kMPad;
        if (L.palette) {
            P.Glass(L.palX, L.palY, L.palW, L.palH, 16, kPanel, 22, 1.6f, true);
            const float py = L.palY + kMPad;
            {
                bool held = false;
                if (tap(tabX, py, kMB, kMB, &held)) st.pop = st.pop == 3 ? 0 : 3, st.popX = tabX, sfx(1.0f), out.changed = true;
                box(tabX, py, kMB, kMB, st.pop == 3, held, false);
                P.Icon(TabAtlasIcon(t.tab), tabX + kMB * 0.5f, py + kMB * 0.5f, 24, kInk, true);
            }
            const float tx0 = tabX + kMB + kMGap + 2, areaW = L.palX + L.palW - kMPad - tx0;
            if (st.lastTab != (int)t.tab) st.lastTab = (int)t.tab, st.scroll = 0;
            const auto& assets = city::Assets();
            std::vector<int> list;
            for (size_t i = 0; i < assets.size(); ++i)
                if (assets[i].tab == t.tab) list.push_back((int)i);
            const float contentW = list.size() * (kMTileW + kMGap) - kMGap;
            st.scrollMax = std::max(0.0f, contentW - areaW);
            st.scroll = std::clamp(st.scroll, 0.0f, st.scrollMax);
            st.tiles = R(tx0, py, std::max(0.0f, areaW), kMB);
            dl.SetClip(st.tiles.x, st.tiles.y, st.tiles.x + st.tiles.w, st.tiles.y + st.tiles.h);
            for (size_t k = 0; k < list.size(); ++k) {
                const int i = list[k];
                const city::Asset& a = assets[i];
                const float x = tx0 + k * (kMTileW + kMGap) - st.scroll;
                if (x + kMTileW < tx0 || x > tx0 + areaW) continue;
                bool held = false;
                const bool sel = i == t.asset;
                if (tap(x, py, kMTileW, kMB, &held) && st.tiles.Contains(in.mouseX, in.mouseY)) t.Select(i), sfx(1.45f), out.changed = true, say(a.name);
                P.Box(x, py, kMTileW, kMB, 10, sel ? Rgba(10, 132, 255, 80) : held ? Rgba(255, 255, 255, 40) : Rgba(255, 255, 255, 16), sel ? 2.0f : 1.0f, sel ? kAccent : Rgba(255, 255, 255, 31));
                const Rect pic = R(x + 3, py + 3, kMTileW - 6, kMB - 6);
                if (a.kind == city::AssetKind::Road) road_ui::DrawProfileTile(dl, pic, city::Profiles()[a.profile], s);
                else DrawIcon(dl, a.kind == city::AssetKind::Roundabout ? Icon::Roundabout : Icon::Intersection, pic.x + pic.w * 0.5f, pic.y + pic.h * 0.5f, std::round(22 * s), std::max(1.0f, 2.0f * s), kInk);
            }
            dl.ClearClip();
        }
        // ---- the popover: one row above the strips, under the finger that opened it
        float capTop = L.top;
        if (st.pop) {
            const int n = st.pop == 1 ? 7 : st.pop == 2 ? kSnapCount : (int)city::Tab::Count;
            const float pw = n * (kMB + kMGap) - kMGap + 2 * kMPad, ph = kMH;
            const float anchor = st.pop == 1 ? modeX : st.pop == 2 ? snapX : tabX;
            const float px = std::clamp(anchor - kMPad, 12 + sa.L(), std::max(12 + sa.L(), W - 12 - sa.R() - pw)), py = L.top - 8 - ph;
            P.Glass(px, py, pw, ph, 16, kPanel, 22, 1.6f, true);
            capTop = py;
            for (int i = 0; i < n; ++i) {
                const float x = px + kMPad + i * (kMB + kMGap), y = py + kMPad;
                if (st.pop == 1) {
                    static const Icon kModeIcons[] = {Icon::Straight, Icon::CurveSimple, Icon::CurveComplex, Icon::Continuous, Icon::Grid, Icon::Replace, Icon::Swap};
                    static const char* const kTips[] = {"Straight", "Simple curve", "Complex curve", "Continuous", "Grid", "Replace: upgrade / flip", "Swap: drag the way"};
                    if (button(x, y, kMB, kMB, kModeIcons[i], (int)t.mode == i)) t.SetMode((city::ToolMode)i), st.pop = 0, sfx(1.2f), out.changed = true, say(kTips[i]);
                } else if (st.pop == 2) {
                    const SnapDef& d = Snaps()[i];
                    const bool lit = i == 0 ? t.snapAll : t.snapAll && (t.snaps & d.bit) != 0, dim = i > 0 && !t.snapAll;
                    const bool hit = button(x, y, kMB, kMB, d.icon, lit, dim);
                    if (d.icon == Icon::None) DrawConnectGlyph(dl, (x + kMB * 0.5f) * s, (y + kMB * 0.5f) * s, std::round(kMB * 0.5f * s), std::max(1.0f, 1.9f * s), dim ? Fade(kInk, 0.4f) : kInk);
                    if (hit) {
                        if (i == 0) t.snapAll = !t.snapAll;
                        else t.ToggleSnap(d.bit);
                        const bool now = i == 0 ? t.snapAll : (t.snaps & d.bit) != 0;
                        sfx(now ? 1.25f : 0.85f), out.changed = true;
                        say(std::string(d.name) + (now ? ": on" : ": off"));
                    }
                } else {
                    bool held = false;
                    const bool on = (int)t.tab == i;
                    if (tap(x, y, kMB, kMB, &held)) t.SelectTab((city::Tab)i), st.pop = 0, sfx(1.2f), out.changed = true, say(city::TabName((city::Tab)i));
                    box(x, y, kMB, kMB, on, held, false);
                    P.Icon(TabAtlasIcon((city::Tab)i), x + kMB * 0.5f, y + kMB * 0.5f, 24, kInk, !on);
                }
            }
        }
        // ---- the caption pill: the last tap for 3 s, else the error
        {
            std::string c;
            Color col = kInk;
            if (st.captionT < 3.0f && !st.caption.empty()) c = st.caption, col = Fade(kInk, std::min(1.0f, (3.0f - st.captionT) * 2.0f));
            else if (!t.error.empty()) c = t.error, col = Rgba(255, 160, 140);
            if (!c.empty()) {
                const float maxW = W - 24 - sa.L() - sa.R() - 20;
                const float cw = std::min(P.TextW(c, 11) + 20, maxW + 20), ch = 24, cx = L.panelX, cy = capTop - 6 - ch;
                P.Box(cx, cy, cw, ch, 12, Rgba(20, 22, 28, 200));
                P.Text(c, cx + 10, cy + ch * 0.5f, 11, col, false, 0, maxW);
            }
        }
        if (!in.down) st.holdId = -1, st.armed = false;
        return L;
    }
    // ================================================================ the options panel
    const bool compact = st.collapsed || !tablet;
    const float px = L.panelX, py = L.panelY, inner = kPanelW - 2 * kPad;
    P.Glass(px, py, L.panelW, L.panelH, 18, kPanel, 22, 1.6f, true);
    float x0 = px + kPad, y = py + kPad;
    // ---- header
    {
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
        { // redo
            bool held = false;
            const bool hit = tap(rx, y, kB, kB, &held);
            const bool dim = !t.CanRedo();
            box(rx, y, kB, kB, false, held, dim);
            redoGlyph(rx, y, kB, dim);
            if (hit && !dim && t.Redo()) sfx(0.8f), out.changed = true, say("Redo");
        }
        rx -= kGap + kB;
        if (button(rx, y, kB, kB, Icon::Undo, false, !t.CanUndo()) && t.Undo()) sfx(0.65f), out.changed = true, say("Undo");
        if (t.Pending()) {
            rx -= kGap + 56;
            if (textButton(rx, y, 56, kB, "End", false)) t.Cancel(), sfx(0.65f), out.changed = true, say("Road ended: the next tap starts a new one");
        }
        if (tablet) { // [road ux] down to the slim strips
            rx -= kGap + 52;
            if (textButton(rx, y, 52, kB, "Mini", false)) st.mini = true, st.pop = 0, sfx(0.75f), out.changed = true, say("Mini toolkit: the expand button brings the panel back");
        }
        P.Text(rail ? "Rail Tools" : "Road Tools", x0, y + kB * 0.5f, 14, kInk, true, 0, rx - x0 - 6);
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
        // ---- Snapping: all + eight (the last: connection snaps)
        {
            const SnapDef* k = Snaps();
            int on = 0;
            for (int i = 1; i < kSnapCount; ++i) on += t.On(k[i].bit) ? 1 : 0;
            char right[24];
            std::snprintf(right, sizeof(right), t.snapAll ? "%d of %d on" : "off", on, kSnapCount - 1);
            label(x0, y, "Snapping", right);
            y += kLabelH;
            for (int i = 0; i < kSnapCount; ++i) {
                const float bx = x0 + i * (kB + kGap);
                const bool lit = i == 0 ? t.snapAll : t.snapAll && (t.snaps & k[i].bit) != 0;
                const bool hit = button(bx, y, kB, kB, k[i].icon, lit, i > 0 && !t.snapAll);
                if (k[i].icon == Icon::None) DrawConnectGlyph(dl, (bx + kB * 0.5f) * s, (y + kB * 0.5f) * s, std::round(kB * 0.5f * s), std::max(1.0f, 1.9f * s), i > 0 && !t.snapAll ? Fade(kInk, 0.4f) : kInk);
                if (hit) {
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
        else if (rail) c = "Rail: min radius 45 m, max grade 6 %";
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
