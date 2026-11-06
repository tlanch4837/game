// In-game immediate-mode GUI: panels, labels, buttons, toggles, segmented choices and sliders, with small stroked icons and
// eased hover / press / slide transitions. API, input contract, the main.cpp integration and the font pipeline: docs/gui.md.
//
// Why its own layer: the editor GUI is native Windows controls, which will not exist in the iOS build. This core has no
// platform headers. The host feeds it one Input struct per frame; it emits backend-agnostic primitives (draw_list.h):
//   smooth (default): UiVertex quads for Renderer::DrawUi: anti-aliased rounded boxes, soft shadows, SDF text;
//   classic (SetClassic(true): no UI pipeline, or TBD_GUI_CLASSIC=1): flat quads + the 5x7 pixel font for DrawTransient.
// Layout and hit testing are the same in both; only text measurement follows the font that is drawn.
//
//     gui::Context g;  gui::InputQueue q;            // q is fed from window messages / touch events
//     g.BeginFrame(q.Take(w, h, dpiScale, dt));
//     g.BeginPanel("Terrain", gui::Anchor::CenterLeft, 8, 0, 296);
//     if (g.Button("Save", gui::Icon::Save)) Save();
//     g.SliderFloat("Size", &radius, 4, 300, "%.0f m");
//     g.EndPanel();
//     g.EndFrame();                                  // WantsMouse() is valid from here
//     r.DrawUi(g.UiVertices()...) or, classic, r.DrawTransient(g.Vertices()...)
//
// Hit testing: widgets test the pointer against the rectangles they get *this* frame, while the host decides whether a
// window message belongs to the GUI with WantsMouseAt(), which tests the panels of the last *finished* frame: exactly what
// is on screen when the user clicks. A panel's auto size and its anchoring use the previous frame's measured size, so a
// panel that changes size settles one frame later (the usual immediate-mode trade-off; fixed sizes are exact at once).
// Animations never move anything: they only blend colours and slide knobs inside fixed rectangles.
//
// Memory: the vertex vectors, panel lists, animation slots and per-panel memory are reused; after the first frames nothing
// is allocated. Text is passed as pointer + length, never as std::string.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "draw_list.h"

namespace tbd {
namespace gui {

using Id = uint32_t;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// One frame of pointer input, in viewport pixels (origin top-left, y down). One pointer: a touch maps onto it as
// begin = Down, move = Move, end = Up, and `hover` false (a finger that is not touching has no position to highlight).
struct Input {
    float mouseX = -1e9f, mouseY = -1e9f; // where the pointer is now; far off-screen when it is gone
    bool down = false;                    // left button / finger is down now
    bool pressed = false;                 // went down this frame *and* the host gave the press to the GUI
    bool released = false;                // went up this frame
    float pressX = -1e9f, pressY = -1e9f; // where it went down (widgets activate from this, not from the latest move)
    float wheel = 0.0f;                   // wheel notches this frame, positive away from the user
    float viewW = 0.0f, viewH = 0.0f;     // viewport size in pixels
    float dpiScale = 1.0f;                // 1.0 = 96 dpi; every theme size is multiplied by it
    bool hover = true;                    // false for touch: no hover highlight while nothing is pressed
    float dt = 1.0f / 60.0f;              // seconds since the last frame: only the transitions use it
};

// Collects the window (or touch) events that arrive between frames and hands them over as one Input. Press and release
// inside one frame (a fast click) both survive, so a click is never lost.
class InputQueue {
public:
    void Move(float x, float y) { x_ = x, y_ = y; }
    // claim = false: the host kept the press for itself (it was not over the GUI). The GUI still learns the button is down,
    // so it does not light up widgets the engine's own drag passes over, but it will not activate anything.
    void Down(float x, float y, bool claim = true) {
        Move(x, y);
        down_ = true;
        if (claim) pressed_ = true, px_ = x, py_ = y;
    }
    void Up(float x, float y) {
        Move(x, y);
        if (down_) released_ = true;
        down_ = false;
    }
    void Wheel(float notches) { wheel_ += notches; }
    void Leave() { x_ = y_ = -1e9f; } // pointer left the viewport, or a finger lifted on a touch screen
    void SetHover(bool hover) { hover_ = hover; }

    Input Take(float viewW, float viewH, float dpiScale, float dt = 1.0f / 60.0f) {
        Input in;
        in.mouseX = x_, in.mouseY = y_;
        in.down = down_, in.pressed = pressed_, in.released = released_;
        in.pressX = px_, in.pressY = py_;
        in.wheel = wheel_;
        in.viewW = viewW, in.viewH = viewH;
        in.dpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        in.hover = hover_;
        in.dt = std::clamp(dt, 0.0f, 0.25f);
        pressed_ = released_ = false;
        wheel_ = 0.0f;
        return in;
    }

private:
    float x_ = -1e9f, y_ = -1e9f, px_ = -1e9f, py_ = -1e9f, wheel_ = 0.0f;
    bool down_ = false, pressed_ = false, released_ = false, hover_ = true;
};

// Colours (RGBA8, straight alpha) and sizes at DPI scale 1.0. Context scales the sizes by Input::dpiScale every frame;
// an iOS build can also raise the base sizes here. The palette is the editor's (ui_theme.h): charcoal, one blue accent.
struct Theme {
    Color panelTop = Rgba(40, 42, 47, 228), panelBottom = Rgba(29, 31, 35, 228); // about 88% opaque
    Color panelBorder = Rgba(78, 82, 90, 235);
    Color shadow = Rgba(0, 0, 0, 120);
    Color rule = Rgba(62, 65, 72);
    Color text = Rgba(222, 225, 230), textBright = Rgba(250, 251, 252), muted = Rgba(150, 156, 165);
    Color widgetTop = Rgba(64, 68, 76), widgetBottom = Rgba(52, 55, 62), widgetBorder = Rgba(84, 89, 98);
    Color widgetHoverTop = Rgba(78, 83, 92), widgetHoverBottom = Rgba(64, 68, 76);
    Color well = Rgba(20, 21, 24, 220), wellBorder = Rgba(58, 61, 68); // segmented control, switch off
    Color track = Rgba(16, 17, 20), trackBorder = Rgba(74, 78, 86);     // slider track (lighter rim: it reads on the dark panel)
    Color rowHover = Rgba(255, 255, 255, 14);
    Color accent = Rgba(0, 112, 224); // #0070e0, the editor's accent
    Color accentTop = Rgba(38, 140, 245), accentBottom = Rgba(0, 104, 210), accentBorder = Rgba(76, 160, 250);
    Color accentPressedTop = Rgba(0, 92, 186), accentPressedBottom = Rgba(0, 80, 165);
    Color knob = Rgba(244, 245, 247), knobBorder = Rgba(0, 0, 0, 70);

    float fontSize = 15.0f;    // labels, buttons
    float smallSize = 13.0f;   // values, status line
    float titleSize = 15.0f;   // panel titles (bold face, brighter)
    float minFontSize = 10.0f; // text that does not fit shrinks down to this before it is cut
    float padding = 12.0f;     // inside panels
    float gap = 8.0f;          // between widgets
    float hitSize = 44.0f;     // height of everything you can press: touch friendly (Apple's 44 pt)
    float titleHeight = 36.0f;
    float labelPad = 4.0f;     // above and below a label's text
    float textPad = 12.0f;     // left / right inside buttons and segments
    float rowPad = 8.0f;       // left / right inside slider and toggle rows: their labels, tracks and switches line up
    float panelRadius = 8.0f, widgetRadius = 6.0f;
    float shadowBlur = 14.0f, shadowOffset = 5.0f;
    float border = 1.0f;
    float trackHeight = 4.0f, thumbRadius = 8.0f;
    float switchWidth = 40.0f, switchHeight = 22.0f;
    float iconSize = 14.0f, iconStroke = 1.75f, iconGap = 7.0f;
    float animRate = 18.0f;    // transitions: 1 / seconds (about 0.15 s to settle)

    // A copy with every size multiplied by `s`; lengths rounded to whole pixels (sizes of text and strokes kept fractional).
    Theme Scaled(float s) const {
        Theme t = *this;
        auto px = [s](float v) { return std::max(1.0f, std::round(v * s)); };
        t.fontSize = fontSize * s, t.smallSize = smallSize * s, t.titleSize = titleSize * s, t.minFontSize = minFontSize * s;
        t.padding = px(padding), t.gap = px(gap), t.hitSize = px(hitSize), t.titleHeight = px(titleHeight);
        t.labelPad = px(labelPad), t.textPad = px(textPad), t.rowPad = px(rowPad), t.panelRadius = px(panelRadius), t.widgetRadius = px(widgetRadius);
        t.shadowBlur = px(shadowBlur), t.shadowOffset = px(shadowOffset), t.border = px(border);
        t.trackHeight = px(trackHeight), t.thumbRadius = px(thumbRadius), t.switchWidth = px(switchWidth), t.switchHeight = px(switchHeight);
        t.iconSize = px(iconSize), t.iconStroke = iconStroke * s, t.iconGap = px(iconGap);
        return t;
    }
};

// Where a panel sits. The offsets given to BeginPanel are margins from the anchored edges (positive = towards the middle
// of the screen); on a centred axis they shift right / down.
enum class Anchor { TopLeft, TopCenter, TopRight, CenterLeft, Center, CenterRight, BottomLeft, BottomCenter, BottomRight };

enum class TextStyle { Normal, Muted, Bright, Small }; // Small: Theme::smallSize, for status lines
enum class SliderScale { Linear, Log };                 // Log: equal drag distance per doubling, for ranges like 4 .. 300

// Small stroked icons drawn from capsule lines (no textures), all with the same stroke weight.
enum class Icon {
    None, Raise, Dig, Flatten, Save, Load,
    // road tool (road_toolbar.h)
    Straight, CurveSimple, CurveComplex, Continuous, Grid, Replace, Swap, Bulldoze, Undo,
    ChevronUp, ChevronDown, Steps1, Steps2, Steps3, Steps4, Parallel, Close,
    SnapAll, SnapLength, SnapGeometry, SnapStraight, SnapNetSide, SnapGuide, SnapZone, SnapContour,
    RoadSmall, RoadMedium, RoadLarge, Highway, Quay, Path, Intersection, Roundabout, Roads,
    // zoning + clock (zone_toolbar.h)
    Zones, ZoneBrush, ZoneFill, ZoneBox, Pause, Play, Speed1, Speed2, Speed3,
    // transport (transport_toolbar.h; Rail is also the road tool's railway tab)
    Rail, Transport, Ship, Plane, RotateLeft, RotateRight, Marker, Waves,
    // utilities (utility_toolbar.h)
    Bolt, Drop, Drain, Underground, Pole, Utilities
};

inline void DrawRoadIcon(DrawList& dl, Icon icon, float cx, float cy, float size, float stroke, Color c);

// Draws `icon` centred on (cx, cy), `size` pixels across.
inline void DrawIcon(DrawList& dl, Icon icon, float cx, float cy, float size, float stroke, Color c) {
    const float h = size * 0.5f, q = size * 0.25f;
    switch (icon) {
    case Icon::None: break;
    case Icon::Raise: // a peak with an up chevron above it
        dl.Line(cx - h, cy + h * 0.75f, cx, cy - q * 0.25f, stroke, c);
        dl.Line(cx, cy - q * 0.25f, cx + h, cy + h * 0.75f, stroke, c);
        dl.Line(cx - q, cy - h * 0.55f, cx, cy - h * 1.05f, stroke, c);
        dl.Line(cx, cy - h * 1.05f, cx + q, cy - h * 0.55f, stroke, c);
        break;
    case Icon::Dig: // a valley with a down chevron under it
        dl.Line(cx - h, cy - h * 0.75f, cx, cy + q * 0.25f, stroke, c);
        dl.Line(cx, cy + q * 0.25f, cx + h, cy - h * 0.75f, stroke, c);
        dl.Line(cx - q, cy + h * 0.55f, cx, cy + h * 1.05f, stroke, c);
        dl.Line(cx, cy + h * 1.05f, cx + q, cy + h * 0.55f, stroke, c);
        break;
    case Icon::Flatten: // a level bar with arrows pressing on it from above and below
        dl.Line(cx - h, cy, cx + h, cy, stroke, c);
        dl.Line(cx - q, cy - h, cx, cy - q * 0.9f, stroke, c);
        dl.Line(cx, cy - q * 0.9f, cx + q, cy - h, stroke, c);
        dl.Line(cx - q, cy + h, cx, cy + q * 0.9f, stroke, c);
        dl.Line(cx, cy + q * 0.9f, cx + q, cy + h, stroke, c);
        break;
    case Icon::Save: // an arrow down into a tray
    case Icon::Load: { // an arrow up out of a tray
        const bool down = icon == Icon::Save;
        const float top = cy - h, tip = down ? cy + q * 0.5f : top, tail = down ? top : cy + q * 0.5f;
        dl.Line(cx, tail, cx, tip, stroke, c);
        const float dir = down ? -1.0f : 1.0f; // the chevron's arms go back along the shaft
        dl.Line(cx - q * 1.1f, tip + dir * q * 1.1f, cx, tip, stroke, c);
        dl.Line(cx, tip, cx + q * 1.1f, tip + dir * q * 1.1f, stroke, c);
        dl.Line(cx - h, cy + q, cx - h, cy + h, stroke, c); // tray
        dl.Line(cx - h, cy + h, cx + h, cy + h, stroke, c);
        dl.Line(cx + h, cy + h, cx + h, cy + q, stroke, c);
        break;
    }
    default: DrawRoadIcon(dl, icon, cx, cy, size, stroke, c); break;
    }
}

// The road tool's icons (CS2-style tool / snapping / category glyphs), stroked like the others.
inline void DrawRoadIcon(DrawList& dl, Icon icon, float cx, float cy, float size, float stroke, Color c) {
    const float h = size * 0.5f;
    auto L = [&](float x0, float y0, float x1, float y1) { dl.Line(cx + x0 * h, cy + y0 * h, cx + x1 * h, cy + y1 * h, stroke, c); };
    auto Q = [&](float x0, float y0, float x1, float y1, float x2, float y2) { // quadratic curve in unit coordinates
        float px = x0, py = y0;
        for (int i = 1; i <= 8; ++i) {
            const float t = i / 8.0f, u = 1 - t;
            const float x = u * u * x0 + 2 * u * t * x1 + t * t * x2, y = u * u * y0 + 2 * u * t * y1 + t * t * y2;
            L(px, py, x, y);
            px = x, py = y;
        }
    };
    auto C = [&](float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) { // cubic
        float px = x0, py = y0;
        for (int i = 1; i <= 10; ++i) {
            const float t = i / 10.0f, u = 1 - t;
            const float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            const float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            L(px, py, x, y);
            px = x, py = y;
        }
    };
    auto Ring = [&](float r, int n) {
        for (int i = 0; i < n; ++i) {
            const float a0 = i * 6.2831853f / n, a1 = (i + 1) * 6.2831853f / n;
            L(r * std::cos(a0), r * std::sin(a0), r * std::cos(a1), r * std::sin(a1));
        }
    };
    auto Dot = [&](float x, float y) { dl.Circle(cx + x * h, cy + y * h, stroke * 1.3f, c); };
    switch (icon) {
    case Icon::Straight: L(-0.8f, 0.8f, 0.8f, -0.8f); Dot(-0.8f, 0.8f); Dot(0.8f, -0.8f); break;
    case Icon::CurveSimple: Q(-0.85f, 0.85f, -0.6f, -0.7f, 0.85f, -0.85f); Dot(-0.85f, 0.85f); Dot(0.85f, -0.85f); break;
    case Icon::CurveComplex: C(-0.9f, 0.8f, -0.9f, -0.6f, 0.9f, 0.6f, 0.9f, -0.8f); Dot(-0.9f, 0.8f); Dot(0.9f, -0.8f); break;
    case Icon::Continuous:
        L(-0.8f, 0.9f, -0.8f, 0.1f); Q(-0.8f, 0.1f, -0.8f, -0.6f, -0.1f, -0.6f); Q(-0.1f, -0.6f, 0.6f, -0.6f, 0.6f, -0.1f); L(0.6f, -0.1f, 0.6f, 0.9f);
        break;
    case Icon::Grid:
        for (int i = 0; i < 3; ++i) { const float v = -0.8f + 0.8f * i; L(v, -0.8f, v, 0.8f); L(-0.8f, v, 0.8f, v); }
        break;
    case Icon::Replace: // two arrows chasing each other
        Q(-0.7f, 0.1f, -0.7f, -0.7f, 0.3f, -0.7f); L(0.3f, -0.7f, 0.05f, -0.95f); L(0.3f, -0.7f, 0.05f, -0.45f);
        Q(0.7f, -0.1f, 0.7f, 0.7f, -0.3f, 0.7f); L(-0.3f, 0.7f, -0.05f, 0.95f); L(-0.3f, 0.7f, -0.05f, 0.45f);
        break;
    case Icon::Swap: // two opposed arrows: one-way <-> two-way, pick the direction
        L(-0.8f, -0.35f, 0.8f, -0.35f); L(0.8f, -0.35f, 0.45f, -0.7f); L(0.8f, -0.35f, 0.45f, 0.0f);
        L(0.8f, 0.35f, -0.8f, 0.35f); L(-0.8f, 0.35f, -0.45f, 0.0f); L(-0.8f, 0.35f, -0.45f, 0.7f);
        break;
    case Icon::Bulldoze:
        L(-0.9f, 0.5f, 0.3f, 0.5f); L(0.3f, 0.5f, 0.3f, -0.2f); L(0.3f, -0.2f, -0.6f, -0.2f); L(-0.6f, -0.2f, -0.9f, 0.5f);
        L(0.3f, 0.1f, 0.7f, 0.1f); L(0.7f, -0.6f, 0.9f, 0.6f); L(0.7f, -0.6f, 0.7f, 0.5f); Dot(-0.5f, 0.75f); Dot(0.0f, 0.75f);
        break;
    case Icon::Undo: Q(0.8f, 0.6f, 0.8f, -0.5f, -0.4f, -0.5f); L(-0.4f, -0.5f, -0.1f, -0.85f); L(-0.4f, -0.5f, -0.1f, -0.15f); break;
    case Icon::ChevronUp: L(-0.6f, 0.3f, 0.0f, -0.3f); L(0.0f, -0.3f, 0.6f, 0.3f); break;
    case Icon::ChevronDown: L(-0.6f, -0.3f, 0.0f, 0.3f); L(0.0f, 0.3f, 0.6f, -0.3f); break;
    case Icon::Steps1: case Icon::Steps2: case Icon::Steps3: case Icon::Steps4: { // signal bars: how many are lit = step size
        const int lit = (int)icon - (int)Icon::Steps1 + 1;
        for (int i = 0; i < 4; ++i) {
            const float x = -0.75f + 0.5f * i, top = 0.8f - 0.42f * (i + 1);
            dl.Line(cx + x * h, cy + 0.8f * h, cx + x * h, cy + top * h, stroke * 1.5f, i < lit ? c : Fade(c, 0.3f));
        }
        break;
    }
    case Icon::Parallel: L(-0.3f, -0.9f, -0.3f, 0.9f); L(0.3f, -0.9f, 0.3f, 0.9f); break;
    case Icon::Close: L(-0.6f, -0.6f, 0.6f, 0.6f); L(-0.6f, 0.6f, 0.6f, -0.6f); break;
    case Icon::SnapAll: L(0, -0.9f, 0, 0.9f); L(-0.8f, -0.45f, 0.8f, 0.45f); L(-0.8f, 0.45f, 0.8f, -0.45f); break;
    case Icon::SnapLength: // a ruler
        L(-0.9f, -0.3f, 0.9f, -0.3f); L(-0.9f, 0.3f, 0.9f, 0.3f); L(-0.9f, -0.3f, -0.9f, 0.3f); L(0.9f, -0.3f, 0.9f, 0.3f);
        for (int i = 1; i < 6; ++i) { const float x = -0.9f + 0.3f * i; L(x, -0.3f, x, i % 2 ? -0.05f : 0.1f); }
        break;
    case Icon::SnapGeometry: L(-0.7f, -0.7f, 0.7f, -0.7f); L(0.7f, -0.7f, 0.7f, 0.7f); L(0.7f, 0.7f, -0.7f, 0.7f); L(-0.7f, 0.7f, -0.7f, -0.7f); Dot(0.7f, -0.7f); break;
    case Icon::SnapStraight: L(-0.8f, 0.8f, 0.8f, 0.8f); L(0, 0.8f, 0, -0.9f); L(0, 0.4f, 0.4f, 0.4f); L(0.4f, 0.4f, 0.4f, 0.8f); break;
    case Icon::SnapNetSide: L(0.8f, -0.9f, 0.8f, 0.9f); L(-0.9f, 0, 0.5f, 0); L(0.5f, 0, 0.15f, -0.35f); L(0.5f, 0, 0.15f, 0.35f); break;
    case Icon::SnapGuide: Ring(0.55f, 16); L(0, -0.95f, 0, 0.95f); L(-0.95f, 0, 0.95f, 0); break;
    case Icon::SnapZone:
        L(-0.8f, -0.8f, 0.8f, -0.8f); L(-0.8f, 0.8f, 0.8f, 0.8f); L(-0.8f, -0.8f, -0.8f, 0.8f); L(0.8f, -0.8f, 0.8f, 0.8f);
        L(0, -0.8f, 0, 0.8f); L(-0.8f, 0, 0.8f, 0);
        break;
    case Icon::SnapContour: Ring(0.85f, 20); Ring(0.5f, 14); Ring(0.15f, 8); break;
    case Icon::RoadSmall: case Icon::RoadMedium: case Icon::RoadLarge: { // a road in perspective with dashed lane lines
        const int lanes = icon == Icon::RoadSmall ? 2 : icon == Icon::RoadMedium ? 3 : 4;
        L(-0.35f, -0.9f, -0.95f, 0.9f); L(0.35f, -0.9f, 0.95f, 0.9f);
        for (int i = 1; i < lanes; ++i) {
            const float f = -1.0f + 2.0f * i / lanes;
            for (int k = 0; k < 3; ++k) {
                const float t0 = k / 3.0f, t1 = t0 + 0.18f;
                L(f * (0.35f + 0.6f * t0), -0.9f + 1.8f * t0, f * (0.35f + 0.6f * t1), -0.9f + 1.8f * t1);
            }
        }
        break;
    }
    case Icon::Highway: L(-0.4f, -0.9f, -0.95f, 0.9f); L(0.4f, -0.9f, 0.95f, 0.9f); L(-0.05f, -0.9f, -0.1f, 0.9f); L(0.05f, -0.9f, 0.1f, 0.9f); break;
    case Icon::Quay: L(-0.9f, -0.5f, 0.9f, -0.5f); L(-0.9f, -0.5f, -0.9f, 0.6f); Q(-0.9f, 0.6f, -0.45f, 0.35f, 0.0f, 0.6f); Q(0.0f, 0.6f, 0.45f, 0.85f, 0.9f, 0.6f); break;
    case Icon::Path: for (int i = 0; i < 4; ++i) { const float y = -0.9f + 0.5f * i; L(-0.25f + 0.15f * i, y, -0.1f + 0.15f * i, y + 0.3f); } Dot(0.45f, -0.6f); break;
    case Icon::Intersection:
        L(-0.25f, -0.95f, -0.25f, -0.25f); L(-0.25f, -0.25f, -0.95f, -0.25f); L(0.25f, -0.95f, 0.25f, -0.25f); L(0.25f, -0.25f, 0.95f, -0.25f);
        L(-0.25f, 0.95f, -0.25f, 0.25f); L(-0.25f, 0.25f, -0.95f, 0.25f); L(0.25f, 0.95f, 0.25f, 0.25f); L(0.25f, 0.25f, 0.95f, 0.25f);
        break;
    case Icon::Roundabout: Ring(0.45f, 16); L(0, -0.45f, 0, -0.95f); L(0, 0.45f, 0, 0.95f); L(-0.45f, 0, -0.95f, 0); L(0.45f, 0, 0.95f, 0); break;
    case Icon::Zones: L(0, -0.9f, 0.9f, 0); L(0.9f, 0, 0, 0.9f); L(0, 0.9f, -0.9f, 0); L(-0.9f, 0, 0, -0.9f); L(-0.45f, -0.45f, 0.45f, 0.45f); L(0.45f, -0.45f, -0.45f, 0.45f); break;
    case Icon::ZoneBrush: L(0.8f, -0.8f, -0.1f, 0.1f); Q(-0.1f, 0.1f, -0.7f, 0.1f, -0.8f, 0.85f); Q(-0.8f, 0.85f, -0.1f, 0.8f, 0.1f, 0.3f); break;
    case Icon::ZoneFill: L(-0.7f, -0.1f, 0.0f, -0.8f); L(0.0f, -0.8f, 0.7f, -0.1f); L(0.7f, -0.1f, 0.0f, 0.6f); L(0.0f, 0.6f, -0.7f, -0.1f); Q(0.7f, -0.1f, 0.95f, 0.4f, 0.8f, 0.75f); break;
    case Icon::ZoneBox: for (int i = 0; i < 4; ++i) { const float a = -0.8f + 0.45f * i; L(a, -0.8f, a + 0.25f, -0.8f); L(a, 0.8f, a + 0.25f, 0.8f); L(-0.8f, a, -0.8f, a + 0.25f); L(0.8f, a, 0.8f, a + 0.25f); } break;
    case Icon::Pause: L(-0.3f, -0.7f, -0.3f, 0.7f); L(0.3f, -0.7f, 0.3f, 0.7f); break;
    case Icon::Play: L(-0.4f, -0.7f, 0.6f, 0); L(0.6f, 0, -0.4f, 0.7f); L(-0.4f, 0.7f, -0.4f, -0.7f); break;
    case Icon::Speed1: case Icon::Speed2: case Icon::Speed3: {
        const int n = (int)icon - (int)Icon::Speed1 + 1;
        for (int i = 0; i < n; ++i) { const float x = -0.25f * (n - 1) + 0.5f * i - 0.2f; L(x, -0.55f, x + 0.4f, 0); L(x + 0.4f, 0, x, 0.55f); }
        break;
    }
    case Icon::Rail: // two rails in perspective with sleepers
        L(-0.3f, -0.9f, -0.75f, 0.9f); L(0.3f, -0.9f, 0.75f, 0.9f);
        L(-0.42f, -0.45f, 0.42f, -0.45f); L(-0.56f, 0.05f, 0.56f, 0.05f); L(-0.7f, 0.55f, 0.7f, 0.55f);
        break;
    case Icon::Transport: // a train seen from the front
        L(-0.6f, -0.75f, 0.6f, -0.75f); L(0.6f, -0.75f, 0.7f, 0.35f); L(0.7f, 0.35f, -0.7f, 0.35f); L(-0.7f, 0.35f, -0.6f, -0.75f);
        L(-0.45f, -0.5f, 0.45f, -0.5f); L(0.45f, -0.5f, 0.45f, -0.05f); L(0.45f, -0.05f, -0.45f, -0.05f); L(-0.45f, -0.05f, -0.45f, -0.5f);
        Dot(-0.4f, 0.18f); Dot(0.4f, 0.18f); L(-0.7f, 0.9f, -0.35f, 0.5f); L(0.7f, 0.9f, 0.35f, 0.5f);
        break;
    case Icon::Ship: // a hull with a cabin and a mast
        L(-0.9f, 0.1f, 0.9f, 0.1f); L(-0.9f, 0.1f, -0.6f, 0.6f); L(-0.6f, 0.6f, 0.6f, 0.6f); L(0.6f, 0.6f, 0.9f, 0.1f);
        L(-0.45f, 0.1f, -0.45f, -0.3f); L(-0.45f, -0.3f, 0.25f, -0.3f); L(0.25f, -0.3f, 0.25f, 0.1f); L(0.0f, -0.3f, 0.0f, -0.85f);
        Q(-0.9f, 0.9f, -0.45f, 0.75f, 0.0f, 0.9f); Q(0.0f, 0.9f, 0.45f, 1.05f, 0.9f, 0.9f);
        break;
    case Icon::Plane: // top view
        L(0.0f, -0.95f, 0.0f, 0.75f); L(-0.9f, 0.15f, 0.0f, -0.35f); L(0.0f, -0.35f, 0.9f, 0.15f); L(-0.35f, 0.85f, 0.0f, 0.55f); L(0.0f, 0.55f, 0.35f, 0.85f);
        L(-0.9f, 0.15f, -0.9f, 0.3f); L(0.9f, 0.15f, 0.9f, 0.3f);
        break;
    case Icon::RotateLeft: Q(0.6f, 0.5f, 0.6f, -0.7f, -0.3f, -0.6f); L(-0.3f, -0.6f, 0.0f, -0.9f); L(-0.3f, -0.6f, 0.0f, -0.3f); break;
    case Icon::RotateRight: Q(-0.6f, 0.5f, -0.6f, -0.7f, 0.3f, -0.6f); L(0.3f, -0.6f, 0.0f, -0.9f); L(0.3f, -0.6f, 0.0f, -0.3f); break;
    case Icon::Marker: // a footprint corner marker
        L(-0.8f, -0.4f, -0.8f, -0.8f); L(-0.8f, -0.8f, -0.4f, -0.8f); L(0.4f, -0.8f, 0.8f, -0.8f); L(0.8f, -0.8f, 0.8f, -0.4f);
        L(0.8f, 0.4f, 0.8f, 0.8f); L(0.8f, 0.8f, 0.4f, 0.8f); L(-0.4f, 0.8f, -0.8f, 0.8f); L(-0.8f, 0.8f, -0.8f, 0.4f); Dot(0, 0);
        break;
    case Icon::Waves: // the shoreline: waves over a beach
        Q(-0.9f, -0.35f, -0.45f, -0.85f, 0.0f, -0.35f); Q(0.0f, -0.35f, 0.45f, 0.15f, 0.9f, -0.35f);
        Q(-0.9f, 0.2f, -0.45f, -0.3f, 0.0f, 0.2f); Q(0.0f, 0.2f, 0.45f, 0.7f, 0.9f, 0.2f);
        L(-0.9f, 0.85f, 0.9f, 0.85f);
        break;
    case Icon::Bolt: // a lightning bolt
        L(0.3f, -0.95f, -0.5f, 0.15f); L(-0.5f, 0.15f, 0.0f, 0.15f); L(0.0f, 0.15f, -0.3f, 0.95f);
        L(-0.3f, 0.95f, 0.6f, -0.25f); L(0.6f, -0.25f, 0.1f, -0.25f); L(0.1f, -0.25f, 0.3f, -0.95f);
        break;
    case Icon::Drop: // a water drop
        C(0.0f, -0.95f, -0.2f, -0.5f, -0.65f, -0.05f, -0.65f, 0.3f); C(-0.65f, 0.3f, -0.65f, 0.75f, -0.35f, 0.95f, 0.0f, 0.95f);
        C(0.0f, -0.95f, 0.2f, -0.5f, 0.65f, -0.05f, 0.65f, 0.3f); C(0.65f, 0.3f, 0.65f, 0.75f, 0.35f, 0.95f, 0.0f, 0.95f);
        break;
    case Icon::Drain: // a pipe emptying downwards
        L(-0.95f, -0.55f, 0.15f, -0.55f); L(-0.95f, -0.05f, 0.15f, -0.05f); L(0.15f, -0.65f, 0.15f, 0.05f);
        L(0.15f, -0.65f, 0.35f, -0.65f); L(0.15f, 0.05f, 0.35f, 0.05f);
        Dot(0.7f, 0.35f); Dot(0.4f, 0.65f); Dot(0.75f, 0.85f);
        break;
    case Icon::Underground: // the ground line over a buried pipe
        Q(-0.95f, -0.45f, -0.5f, -0.75f, 0.0f, -0.45f); Q(0.0f, -0.45f, 0.5f, -0.15f, 0.95f, -0.45f);
        L(-0.8f, 0.3f, 0.8f, 0.3f); L(-0.8f, 0.7f, 0.8f, 0.7f); L(-0.8f, 0.3f, -0.8f, 0.7f);
        L(0.8f, 0.3f, 0.8f, 0.7f); L(-0.25f, 0.3f, -0.25f, 0.7f); L(0.25f, 0.3f, 0.25f, 0.7f);
        break;
    case Icon::Pole: // a utility pole with its cross arms and wires
        L(0.0f, -0.95f, 0.0f, 0.95f); L(-0.7f, -0.6f, 0.7f, -0.6f); L(-0.5f, -0.25f, 0.5f, -0.25f);
        Q(-0.7f, -0.6f, -0.9f, -0.2f, -0.95f, 0.35f); Q(0.7f, -0.6f, 0.9f, -0.2f, 0.95f, 0.35f);
        break;
    case Icon::Utilities: // a bolt over a drop
        L(0.25f, -0.95f, -0.3f, -0.05f); L(-0.3f, -0.05f, 0.05f, -0.05f); L(0.05f, -0.05f, -0.15f, 0.55f); L(-0.15f, 0.55f, 0.45f, -0.25f); L(0.45f, -0.25f, 0.1f, -0.25f); L(0.1f, -0.25f, 0.25f, -0.95f);
        C(-0.55f, 0.15f, -0.65f, 0.35f, -0.8f, 0.55f, -0.8f, 0.7f); C(-0.8f, 0.7f, -0.8f, 0.9f, -0.65f, 0.97f, -0.55f, 0.97f);
        C(-0.55f, 0.15f, -0.45f, 0.35f, -0.3f, 0.55f, -0.3f, 0.7f); C(-0.3f, 0.7f, -0.3f, 0.9f, -0.45f, 0.97f, -0.55f, 0.97f);
        break;
    case Icon::Roads: L(-0.3f, -0.9f, -0.9f, 0.9f); L(0.3f, -0.9f, 0.9f, 0.9f); L(0, -0.8f, 0, -0.5f); L(0, -0.2f, 0, 0.15f); L(0, 0.5f, 0, 0.9f); break;
    default: break;
    }
}

class Context {
public:
    Theme& BaseTheme() { return base_; }     // unscaled; change it between frames
    const Theme& Style() const { return t_; } // this frame's scaled copy

    // Classic output (flat quads + pixel font for DrawTransient) instead of smooth UiVertex primitives. Takes effect at
    // the next BeginFrame. The host sets it when the UI pipeline is unavailable or TBD_GUI_CLASSIC=1.
    void SetClassic(bool classic) { classic_ = classic; }
    bool Classic() const { return classic_; }

    // Areas the host keeps for itself (iOS safe area, the engine's stats text), in pixels. Panels are pushed inside.
    void SetInsets(float left, float top, float right, float bottom) { inset_[0] = left, inset_[1] = top, inset_[2] = right, inset_[3] = bottom; }

    // ---------------------------------------------------------------- frame
    void BeginFrame(const Input& in) {
        in_ = in;
        t_ = base_.Scaled(in.dpiScale);
        dl_.Begin(classic_);
        panels_.clear();
        idDepth_ = 0;
        activeSeen_ = false;
        inPanel_ = false;
        ++frame_;
    }

    void EndFrame() {
        if (inPanel_) EndPanel();
        if (active_ != 0 && active_ != kBackground && !activeSeen_) active_ = 0; // its widget is gone (panel hidden)
        if (in_.pressed && active_ == 0 && OverAny(panels_, in_.pressX, in_.pressY)) active_ = kBackground; // a press on a
                                                                     // panel's empty space still belongs to the GUI until release
        if (!in_.down) active_ = 0;
        wantsMouse_ = active_ != 0 || OverAny(panels_, in_.mouseX, in_.mouseY);
        shown_.swap(panels_); // what is on screen now: WantsMouseAt() tests these until the next EndFrame
    }

    // Forget everything shown, e.g. while the GUI is hidden, so it stops taking clicks at once.
    void Reset() {
        dl_.Begin(classic_);
        panels_.clear();
        shown_.clear();
        active_ = 0;
        wantsMouse_ = false;
        inPanel_ = false;
    }

    // After EndFrame: the pointer is over a panel or a widget holds the pointer (a slider drag, even outside the panel).
    bool WantsMouse() const { return wantsMouse_; }
    // For window messages arriving between frames: over a panel that is on screen now, or something is held.
    bool WantsMouseAt(float x, float y) const { return active_ != 0 || OverAny(shown_, x, y); }
    bool Capturing() const { return active_ != 0; } // a press is held by the GUI (until release)
    Id ActiveId() const { return active_; }

    const std::vector<UiVertex>& UiVertices() const { return dl_.Ui(); } // smooth: for Renderer::DrawUi
    const std::vector<Vertex>& Vertices() const { return dl_.Flat(); }   // classic: triangles for DrawTransient
    const DrawList& Draw() const { return dl_; }
    const std::vector<Rect>& ShownPanels() const { return shown_; } // last finished frame
    Rect LastItem() const { return last_; }                          // the rectangle of the latest widget
    bool LastItemHovered() const { return lastHot_; }

    // ---------------------------------------------------------------- building blocks for custom widgets (road_toolbar.h)
    float Scale() const { return in_.dpiScale; }
    const Input& In() const { return in_; }
    DrawList& Draw() { return dl_; }
    // Room for a custom widget in the open panel: `height` and `naturalW` in scaled pixels (Style() sizes).
    Rect Reserve(float height, float naturalW) { return Place(height, naturalW); }
    // Button behaviour (press inside, click on release inside) for any rectangle; hot = hovered / held.
    bool ClickArea(const char* id, const Rect& r, bool* hot = nullptr, bool* held = nullptr) {
        const Id i = GetId(id);
        bool h = false;
        const bool clicked = Press(i, r, h);
        if (hot) *hot = h;
        if (held) *held = active_ == i && h;
        return clicked;
    }
    float Ease(const char* id, int channel, float target) { return Anim(GetId(id), channel, target); }
    void TextIn(const Rect& box, const char* text, Color color, float alignX, float size, bool bold = false) {
        DrawText(box, text, TextLen(text), color, alignX, size, bold ? FontFace::Bold : FontFace::Regular, 0.0f);
    }
    float TextWidth(const char* text, float size, bool bold = false) const {
        return dl_.TextWidth(text, TextLen(text), size, bold ? FontFace::Bold : FontFace::Regular);
    }
    // A small floating label chip at a screen point (tooltips over the world). Takes no input.
    void Chip(float x, float y, const char* text, Color fill = Rgba(20, 22, 26, 215), Color ink = Rgba(250, 251, 252)) {
        const float size = t_.smallSize, pad = std::round(6.0f * in_.dpiScale);
        const float w = dl_.TextWidth(text, TextLen(text), size) + 2 * pad, hgt = std::round(size + 1.6f * pad);
        const float x0 = std::round(x - w * 0.5f), y0 = std::round(y - hgt * 0.5f);
        dl_.Rect(x0, y0, x0 + w, y0 + hgt, fill, fill, std::round(4.0f * in_.dpiScale));
        DrawText({x0, y0, w, hgt}, text, TextLen(text), ink, 0.5f, size, FontFace::Regular, 0.0f);
    }

    // Width in pixels of text drawn in `style` this frame (the smooth font's metrics, or the pixel font's when classic).
    float MeasureText(const char* text, TextStyle style = TextStyle::Normal) const {
        const size_t n = TextLen(text);
        return dl_.TextWidth(text, n, style == TextStyle::Small ? t_.smallSize : t_.fontSize);
    }

    // ---------------------------------------------------------------- ids
    // Every widget's id is a hash of its label and the id scope. Text after "##" is hashed but not drawn, so two
    // buttons can both read "Save" ("Save##map", "Save##terrain"), and a label can change ("On##edit" / "Off##edit").
    void PushId(const char* s) { Push(Hash(s, std::strlen(s), Top())); }
    void PushId(int i) { Push(Hash((const char*)&i, sizeof(i), Top())); }
    void PopId() { if (idDepth_ > 0) --idDepth_; }
    Id GetId(const char* label) const { return Hash(label, std::strlen(label), Top()); }

    // ---------------------------------------------------------------- panels and layout
    // width / height in unscaled pixels (times the DPI scale); 0 = fit the contents (measured on the previous frame).
    // title: nullptr or "" for none; "##name" gives an untitled panel its own identity. padding: unscaled, < 0 = theme.
    void BeginPanel(const char* title, Anchor anchor, float offX, float offY, float width = 0.0f, float height = 0.0f, float padding = -1.0f) {
        if (inPanel_) EndPanel();
        inPanel_ = true;
        const char* name = title ? title : "";
        const Id id = GetId(name[0] ? name : "##panel");
        PushId(name[0] ? name : "##panel");
        const PanelMemory& mem = Memory(id);
        const float s = in_.dpiScale;
        pad_ = padding >= 0.0f ? std::round(padding * s) : t_.padding;
        fixedW_ = width > 0.0f ? std::round(width * s) : 0.0f;
        fixedH_ = height > 0.0f ? std::round(height * s) : 0.0f;
        const float w = fixedW_ > 0.0f ? fixedW_ : mem.w, h = fixedH_ > 0.0f ? fixedH_ : mem.h;
        const float ox = std::round(offX * s), oy = std::round(offY * s);
        const float L = inset_[0], T = inset_[1], R = in_.viewW - inset_[2], B = in_.viewH - inset_[3];
        // Anchor is laid out row by row (top, centre, bottom), each row left, centre, right.
        const int col = (int)anchor % 3, row = (int)anchor / 3;
        float x = col == 0 ? L + ox : col == 2 ? R - ox - w : (L + R - w) * 0.5f + ox;
        float y = row == 0 ? T + oy : row == 2 ? B - oy - h : (T + B - h) * 0.5f + oy;
        x = std::max(L, std::min(x, R - w)); // keep it on screen; the top-left edge wins when it cannot fit
        y = std::max(T, std::min(y, B - h));
        panel_ = {std::round(x), std::round(y), w, h};
        panelId_ = id;
        bg_ = dl_.Reserve(kBgPrims); // shadow, body, title rule, accent: filled in by EndPanel once the size is known
        const size_t shown = TextLen(name);
        hasTitle_ = shown > 0;
        float top = panel_.y + pad_;
        naturalW_ = 0.0f;
        if (hasTitle_) {
            const float baseline = panel_.y + std::round((t_.titleHeight + dl_.CapHeight(t_.titleSize, FontFace::Bold)) * 0.5f);
            dl_.Text(panel_.x + pad_, baseline, t_.titleSize, FontFace::Bold, name, shown, t_.textBright);
            naturalW_ = dl_.TextWidth(name, shown, t_.titleSize, FontFace::Bold);
            top = panel_.y + t_.titleHeight + pad_ * 0.5f;
        }
        contentX_ = panel_.x + pad_;
        contentW_ = std::max(0.0f, (w > 0.0f ? w : 0.0f) - 2.0f * pad_);
        cursorY_ = top;
        contentTop_ = top;
        bottom_ = top;
        row_ = Row{};
    }

    void EndPanel() {
        if (!inPanel_) return;
        if (row_.columns > 0) EndRow();
        inPanel_ = false;
        PopId();
        const float w = fixedW_ > 0.0f ? fixedW_ : std::max(naturalW_ + 2.0f * pad_, 2.0f * pad_);
        const float contentBottom = bottom_ > contentTop_ ? bottom_ : contentTop_ - pad_ * 0.5f;
        const float h = fixedH_ > 0.0f ? fixedH_ : contentBottom + pad_ - panel_.y;
        PanelMemory& mem = Memory(panelId_);
        mem.w = w, mem.h = h;
        panel_.w = w, panel_.h = h;
        const float x0 = panel_.x, y0 = panel_.y, x1 = x0 + w, y1 = y0 + h;
        const size_t slot = dl_.SlotSize();
        dl_.ShadowAt(bg_, x0, y0 + t_.shadowOffset, x1, y1 + t_.shadowOffset, t_.panelRadius, t_.shadowBlur, t_.shadow);
        dl_.RectAt(bg_ + slot, x0, y0, x1, y1, t_.panelTop, t_.panelBottom, t_.panelRadius, t_.border, t_.panelBorder);
        if (hasTitle_) { // a thin rule under the title, with a short accent stroke at its left end
            const float ry = y0 + t_.titleHeight - t_.border;
            dl_.RectAt(bg_ + 2 * slot, x0 + pad_, ry, x1 - pad_, ry + t_.border, t_.rule, t_.rule, 0.0f);
            const float aw = std::round(28.0f * in_.dpiScale), ah = std::max(2.0f, std::round(2.0f * in_.dpiScale));
            dl_.RectAt(bg_ + 3 * slot, x0 + pad_, ry - ah * 0.5f, x0 + pad_ + aw, ry + ah * 0.5f, t_.accent, t_.accent, ah * 0.5f);
        } else { // unused: zero-area quads on the panel, so nothing strays to the screen origin
            dl_.RectAt(bg_ + 2 * slot, x0 + 1, y0 + 1, x0 + 1, y0 + 1, 0, 0, 0.0f);
            dl_.RectAt(bg_ + 3 * slot, x0 + 1, y0 + 1, x0 + 1, y0 + 1, 0, 0, 0.0f);
        }
        panels_.push_back(panel_);
    }

    // The next `columns` widgets share one row, equal widths with the gap between them.
    void BeginRow(int columns) {
        if (row_.columns > 0) EndRow();
        row_.columns = std::max(1, columns);
        row_.index = 0;
        row_.y = cursorY_;
        row_.height = 0.0f;
        row_.natural = 0.0f;
    }
    void EndRow() {
        if (row_.columns == 0) return;
        if (row_.index > 0) {
            cursorY_ = row_.y + row_.height + t_.gap;
            bottom_ = std::max(bottom_, row_.y + row_.height);
        }
        naturalW_ = std::max(naturalW_, row_.natural);
        row_ = Row{};
    }

    // Fixed width for the next widget, in unscaled pixels (otherwise widgets fill the panel or their row column).
    void NextWidth(float w) { nextW_ = std::round(w * in_.dpiScale); }

    void Spacing(float px = 0.0f) { // unscaled pixels; 0 = one gap
        if (!inPanel_) return;
        cursorY_ += px > 0.0f ? std::round(px * in_.dpiScale) : t_.gap;
    }

    void Separator() {
        const Rect r = Place(std::max(1.0f, t_.border), 0.0f);
        dl_.Rect(r.x, r.y, r.x + r.w, r.y + r.h, t_.rule, 0.0f);
    }

    // ---------------------------------------------------------------- widgets
    void Label(const char* text, TextStyle style = TextStyle::Normal) {
        const size_t n = TextLen(text);
        const float size = style == TextStyle::Small ? t_.smallSize : t_.fontSize;
        const Rect r = Place(LineHeight(size) + 2.0f * t_.labelPad, dl_.TextWidth(text, n, size));
        const Color c = style == TextStyle::Muted ? t_.muted : style == TextStyle::Bright ? t_.textBright : t_.text;
        DrawText(r, text, n, c, 0.0f, size, FontFace::Regular, 0.0f);
    }

    // printf-style label through a fixed stack buffer (no allocation); longer text is cut at 127 characters.
    void Text(TextStyle style, const char* format, ...) {
        char buf[128];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        Label(buf, style);
    }

    // True on the frame the press is released over the button (press and release both inside).
    bool Button(const char* label, Icon icon = Icon::None) {
        const size_t n = TextLen(label);
        const float content = ContentWidth(label, n, icon);
        const Rect r = Place(t_.hitSize, content + 2.0f * t_.textPad);
        const Id id = GetId(label);
        bool hot = false;
        const bool clicked = Press(id, r, hot);
        const bool held = active_ == id && hot;
        const float hoverT = Anim(id, 1, hot ? 1.0f : 0.0f), pressT = Anim(id, 2, held ? 1.0f : 0.0f);
        const Color top = Mix(Mix(t_.widgetTop, t_.widgetHoverTop, hoverT), t_.accentPressedTop, pressT);
        const Color bottom = Mix(Mix(t_.widgetBottom, t_.widgetHoverBottom, hoverT), t_.accentPressedBottom, pressT);
        dl_.Rect(r.x, r.y, r.x + r.w, r.y + r.h, top, bottom, t_.widgetRadius, t_.border, Mix(t_.widgetBorder, t_.accentBorder, pressT));
        DrawContent(r, label, n, icon, Mix(t_.text, t_.textBright, std::max(hoverT, pressT)));
        return clicked;
    }

    // A row with the label on the left and a pill switch on the right. Returns true when it flipped.
    bool Toggle(const char* label, bool* value) {
        const size_t n = TextLen(label);
        const Rect r = Place(t_.hitSize, dl_.TextWidth(label, n, t_.fontSize) + 2.0f * t_.rowPad + t_.textPad + t_.switchWidth);
        const Id id = GetId(label);
        bool hot = false;
        const bool clicked = Press(id, r, hot);
        if (clicked && value) *value = !*value;
        const bool on = value && *value, held = active_ == id && hot;
        const float hoverT = Anim(id, 1, hot ? 1.0f : 0.0f), onT = Anim(id, 3, on ? 1.0f : 0.0f);
        if (hoverT > 0.01f) dl_.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Fade(t_.rowHover, hoverT * (held ? 2.0f : 1.0f)), t_.widgetRadius);
        const float sw = t_.switchWidth, sh = t_.switchHeight;
        const float sx = std::round(r.x + r.w - t_.rowPad - sw), sy = std::round(r.y + (r.h - sh) * 0.5f);
        dl_.Rect(sx, sy, sx + sw, sy + sh, Mix(t_.well, t_.accentTop, onT), Mix(t_.well, t_.accentBottom, onT), sh * 0.5f, t_.border,
                 Mix(t_.wellBorder, t_.accentBorder, onT));
        const float kr = sh * 0.5f - std::round(3.0f * in_.dpiScale), kx = sx + sh * 0.5f + (sw - sh) * onT;
        dl_.Shadow(kx - kr, sy + sh * 0.5f - kr + 1.0f, kx + kr, sy + sh * 0.5f + kr + 1.0f, kr, std::round(2.0f * in_.dpiScale), Fade(t_.shadow, 0.8f));
        dl_.Circle(kx, sy + sh * 0.5f, kr, t_.knob);
        Rect text = r;
        text.w = std::max(0.0f, sx - r.x);
        DrawText(text, label, n, Mix(t_.text, t_.textBright, std::max(onT, hoverT)), 0.0f, t_.fontSize, FontFace::Regular, t_.rowPad);
        return clicked;
    }

    // Segmented control: one rounded well with a sliding accent pill under the chosen option; optional icons beside the
    // labels. Returns true when *index changed.
    bool Choice(const char* label, const char* const* options, int count, int* index, const Icon* icons = nullptr) {
        if (count <= 0) return false;
        float natural = 0.0f;
        for (int i = 0; i < count; ++i)
            natural = std::max(natural, ContentWidth(options[i], TextLen(options[i]), icons ? icons[i] : Icon::None) + 2.0f * t_.textPad * 0.5f);
        const float inset = std::round(3.0f * in_.dpiScale); // the pill sits this far inside the well
        const Rect r = Place(t_.hitSize, natural * count + 2.0f * inset);
        const Id groupId = GetId(label);
        PushId(label);
        bool changed = false;
        const float segW = (r.w - 2.0f * inset) / count;
        dl_.Rect(r.x, r.y, r.x + r.w, r.y + r.h, t_.well, t_.well, t_.widgetRadius + inset, t_.border, t_.wellBorder);
        // First pass: input (so the pill can slide this frame), hover backgrounds.
        bool anyHot = false;
        int heldSeg = -1;
        for (int i = 0; i < count; ++i) {
            const Rect seg = Segment(r, inset, segW, i, count);
            PushId(i);
            const Id id = GetId("##seg");
            PopId();
            bool hot = false;
            if (Press(id, seg, hot) && index && *index != i) *index = i, changed = true;
            anyHot = anyHot || hot;
            if (active_ == id && hot) heldSeg = i;
            const float hoverT = Anim(id, 1, hot && !(index && *index == i) ? 1.0f : 0.0f);
            if (hoverT > 0.01f) dl_.Rect(seg.x, seg.y, seg.x + seg.w, seg.y + seg.h, Fade(t_.rowHover, hoverT * 1.6f), t_.widgetRadius);
        }
        const int chosen = index ? std::clamp(*index, 0, count - 1) : -1;
        if (chosen >= 0) { // the pill slides between segments
            const float pos = Anim(groupId, 4, (float)chosen);
            const Rect a = Segment(r, inset, segW, 0, count);
            const float px = a.x + pos * segW;
            const bool pressedOnChosen = heldSeg == chosen;
            dl_.Shadow(px, a.y + 1.0f, px + a.w, a.y + a.h + 1.0f, t_.widgetRadius, std::round(3.0f * in_.dpiScale), Fade(t_.shadow, 0.7f));
            dl_.Rect(px, a.y, px + a.w, a.y + a.h, pressedOnChosen ? t_.accentPressedTop : t_.accentTop, pressedOnChosen ? t_.accentPressedBottom : t_.accentBottom,
                     t_.widgetRadius, t_.border, t_.accentBorder);
        }
        if (heldSeg >= 0 && heldSeg != chosen) { // pressed, not yet released: a dark accent preview where it will go
            const Rect seg = Segment(r, inset, segW, heldSeg, count);
            dl_.Rect(seg.x, seg.y, seg.x + seg.w, seg.y + seg.h, Fade(t_.accentPressedTop, 0.85f), Fade(t_.accentPressedBottom, 0.85f), t_.widgetRadius);
        }
        for (int i = 0; i < count; ++i) {
            const Rect seg = Segment(r, inset, segW, i, count);
            const float on = chosen >= 0 ? std::max(0.0f, 1.0f - std::fabs(Anim(groupId, 4, (float)chosen) - (float)i)) : 0.0f;
            DrawContent(seg, options[i], TextLen(options[i]), icons ? icons[i] : Icon::None, Mix(t_.text, t_.textBright, on), std::round(4.0f * in_.dpiScale));
        }
        PopId();
        last_ = r;
        lastHot_ = anyHot;
        return changed;
    }

    // Horizontal slider: label top left, the value (printf `format`) top right in a dimmer colour, a thin track below with
    // a round thumb. Press anywhere on the row and drag; the drag keeps going outside the panel until release. The wheel
    // over it steps 2% of the range.
    bool SliderFloat(const char* label, float* value, float minV, float maxV, const char* format = "%.1f",
                     SliderScale scale = SliderScale::Linear) {
        const size_t n = TextLen(label);
        char valueText[48];
        snprintf(valueText, sizeof(valueText), format ? format : "%.1f", value ? (double)*value : 0.0);
        const size_t vn = std::strlen(valueText);
        const Rect r = Place(t_.hitSize, dl_.TextWidth(label, n, t_.fontSize) + dl_.TextWidth(valueText, vn, t_.smallSize) + 3.0f * t_.textPad);
        const Id id = GetId(label);
        const bool logScale = scale == SliderScale::Log && minV > 0.0f && maxV > minV;
        const float trackX = r.x + t_.rowPad, trackW = std::max(1.0f, r.w - 2.0f * t_.rowPad);
        auto toT = [&](float v) {
            if (maxV <= minV) return 0.0f;
            const float t = logScale ? std::log(v / minV) / std::log(maxV / minV) : (v - minV) / (maxV - minV);
            return std::clamp(t, 0.0f, 1.0f);
        };
        auto fromT = [&](float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            return logScale ? minV * std::pow(maxV / minV, t) : minV + (maxV - minV) * t;
        };
        if (active_ == id) activeSeen_ = true;
        if (in_.pressed && active_ == 0 && r.Contains(in_.pressX, in_.pressY)) active_ = id, activeSeen_ = true;
        const bool hot = Hot(id, r);
        bool changed = false;
        if (value) {
            const float before = *value;
            if (active_ == id) *value = fromT((in_.mouseX - trackX) / trackW);
            else if (hot && in_.wheel != 0.0f && active_ == 0) *value = fromT(toT(*value) + 0.02f * in_.wheel);
            changed = *value != before;
        }
        if (active_ == id && in_.released) active_ = 0;
        if (changed) snprintf(valueText, sizeof(valueText), format ? format : "%.1f", (double)*value);
        const size_t vlen = std::strlen(valueText);
        const bool held = active_ == id;
        const float hoverT = Anim(id, 1, hot || held ? 1.0f : 0.0f), heldT = Anim(id, 2, held ? 1.0f : 0.0f);
        if (hoverT > 0.01f) dl_.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Fade(t_.rowHover, hoverT), t_.widgetRadius);
        // Text on the top half, track on the bottom half of the row.
        const Rect top = {r.x, r.y + std::round(2.0f * in_.dpiScale), r.w, std::round(r.h * 0.5f)};
        const float vw = dl_.TextWidth(valueText, vlen, t_.smallSize);
        Rect labelBox = top;
        labelBox.w = std::max(0.0f, r.w - vw - t_.rowPad);
        DrawText(labelBox, label, n, Mix(t_.text, t_.textBright, hoverT), 0.0f, t_.fontSize, FontFace::Regular, t_.rowPad);
        const float baseline = std::round(top.y + (top.h + dl_.CapHeight(t_.fontSize)) * 0.5f); // values share the label's baseline
        dl_.Text(r.x + r.w - t_.rowPad - vw, baseline, t_.smallSize, FontFace::Regular, valueText, vlen, Mix(t_.muted, t_.textBright, heldT));
        const float trackY = std::round(r.y + r.h * 0.72f - t_.trackHeight * 0.5f), th = t_.trackHeight;
        const float t = value ? toT(*value) : 0.0f, fillX = trackX + trackW * t;
        dl_.Rect(trackX, trackY, trackX + trackW, trackY + th, t_.track, t_.track, th * 0.5f, t_.border, t_.trackBorder);
        if (fillX > trackX + 0.5f) dl_.Rect(trackX, trackY, fillX, trackY + th, t_.accentTop, t_.accentBottom, th * 0.5f);
        const float kr = t_.thumbRadius * (1.0f + 0.15f * hoverT), ky = trackY + th * 0.5f;
        dl_.Shadow(fillX - kr, ky - kr + 1.5f, fillX + kr, ky + kr + 1.5f, kr, std::round(3.0f * in_.dpiScale), t_.shadow);
        dl_.Circle(fillX, ky, kr, Mix(t_.knob, t_.textBright, heldT), t_.border, t_.knobBorder);
        last_ = r;
        lastHot_ = hot;
        return changed;
    }

private:
    static constexpr Id kBackground = 1; // "a press on a panel's background": holds the pointer, does nothing
    static constexpr int kBgPrims = 4;   // shadow, body, title rule, accent stroke
    static constexpr int kMaxIdDepth = 16;
    static constexpr int kAnimSlots = 96;

    struct PanelMemory {
        Id id = 0;
        float w = 0.0f, h = 0.0f; // measured on the last frame it was shown
    };
    struct Row {
        int columns = 0, index = 0;
        float y = 0.0f, height = 0.0f, natural = 0.0f;
    };
    struct AnimSlot {
        Id key = 0;
        float value = 0.0f;
        uint32_t frame = 0; // last used: the least recently used slot is recycled
    };

    static Id Hash(const char* s, size_t n, Id seed) { // FNV-1a
        Id h = seed ^ 2166136261u;
        for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619u;
        return h < 2 ? h + 2 : h; // 0 = nothing, 1 = kBackground
    }
    Id Top() const { return idDepth_ > 0 ? ids_[idDepth_ - 1] : 0; }
    void Push(Id id) { if (idDepth_ < kMaxIdDepth) ids_[idDepth_++] = id; }

    // An eased value per (widget, channel): moves towards `target` at Theme::animRate. A key seen for the first time starts
    // at its target, so nothing animates in when a panel first appears (and captures are stable).
    float Anim(Id id, int channel, float target) {
        const Id key = Hash((const char*)&channel, sizeof(channel), id);
        AnimSlot* slot = nullptr;
        AnimSlot* oldest = &anims_[0];
        for (AnimSlot& a : anims_) {
            if (a.key == key) { slot = &a; break; }
            if (a.frame < oldest->frame) oldest = &a;
        }
        if (!slot) {
            *oldest = {key, target, frame_};
            return target;
        }
        if (slot->frame != frame_) { // once per frame
            const float k = 1.0f - std::exp(-base_.animRate * in_.dt);
            slot->value += (target - slot->value) * k;
            if (std::fabs(target - slot->value) < 0.002f) slot->value = target;
            slot->frame = frame_;
        }
        return slot->value;
    }

    // Characters drawn: up to "##".
    static size_t TextLen(const char* s) {
        if (!s) return 0;
        const char* hash = std::strstr(s, "##");
        return hash ? (size_t)(hash - s) : std::strlen(s);
    }
    float LineHeight(float size) const { return dl_.Classic() ? 7.0f * DrawList::ClassicCell(size) : std::round(size * 1.2f); }

    static bool OverAny(const std::vector<Rect>& rects, float x, float y) {
        for (const Rect& r : rects)
            if (r.Contains(x, y)) return true;
        return false;
    }

    PanelMemory& Memory(Id id) {
        for (PanelMemory& m : memory_)
            if (m.id == id) return m;
        memory_.push_back(PanelMemory{id, 0.0f, 0.0f});
        return memory_.back();
    }

    Rect Segment(const Rect& r, float inset, float segW, int i, int count) const {
        const float x0 = std::round(r.x + inset + i * segW), x1 = i == count - 1 ? r.x + r.w - inset : std::round(r.x + inset + (i + 1) * segW);
        return {x0, r.y + inset, x1 - x0, r.h - 2.0f * inset};
    }

    float ContentWidth(const char* label, size_t n, Icon icon) const {
        const float text = dl_.TextWidth(label, n, t_.fontSize);
        return icon == Icon::None ? text : t_.iconSize + (n > 0 ? t_.iconGap + text : 0.0f);
    }

    // Icon and label centred together in r. Too wide: the label shrinks (DrawText's rule) and the icon stays.
    void DrawContent(const Rect& r, const char* label, size_t n, Icon icon, Color c, float padX = -1.0f) {
        if (icon == Icon::None) {
            DrawText(r, label, n, c, 0.5f, t_.fontSize, FontFace::Regular, padX);
            return;
        }
        const float pad = padX >= 0.0f ? padX : t_.textPad * 0.5f;
        const float textW = std::min(dl_.TextWidth(label, n, t_.fontSize), std::max(0.0f, r.w - 2.0f * pad - t_.iconSize - t_.iconGap));
        const float total = t_.iconSize + (n > 0 ? t_.iconGap + textW : 0.0f);
        const float x = std::round(r.x + (r.w - total) * 0.5f);
        DrawIcon(dl_, icon, x + t_.iconSize * 0.5f, std::round(r.y + r.h * 0.5f), t_.iconSize, t_.iconStroke, c);
        if (n > 0) DrawText({x + t_.iconSize + t_.iconGap, r.y, textW, r.h}, label, n, c, 0.0f, t_.fontSize, FontFace::Regular, 0.0f);
    }

    // Where the next widget goes: the next column of the open row, or a full-width line (or NextWidth) below the last.
    Rect Place(float height, float naturalW) {
        Rect r;
        if (!inPanel_) { // outside a panel there is nowhere to put it; keep the call harmless
            last_ = r;
            return r;
        }
        const float fixed = nextW_;
        nextW_ = 0.0f;
        if (row_.columns > 0) {
            if (row_.index >= row_.columns) { // a full row wraps to a new one
                cursorY_ = row_.y + row_.height + t_.gap;
                bottom_ = std::max(bottom_, row_.y + row_.height);
                naturalW_ = std::max(naturalW_, row_.natural);
                row_.index = 0, row_.y = cursorY_, row_.height = 0.0f, row_.natural = 0.0f;
            }
            const float colW = (contentW_ - t_.gap * (row_.columns - 1)) / row_.columns;
            const float x = std::round(contentX_ + row_.index * (colW + t_.gap));
            const float x1 = std::round(contentX_ + row_.index * (colW + t_.gap) + colW);
            r = {x, row_.y, fixed > 0.0f ? fixed : std::max(0.0f, x1 - x), height};
            row_.height = std::max(row_.height, height);
            row_.natural += (row_.index > 0 ? t_.gap : 0.0f) + std::max(naturalW, fixed);
            // A row's natural width is what the widest column needs times the columns, so equal columns all fit.
            row_.natural = std::max(row_.natural, (std::max(naturalW, fixed) + t_.gap) * row_.columns - t_.gap);
            ++row_.index;
        } else {
            r = {contentX_, cursorY_, fixed > 0.0f ? fixed : contentW_, height};
            cursorY_ += height + t_.gap;
            bottom_ = std::max(bottom_, r.y + r.h);
            naturalW_ = std::max(naturalW_, std::max(naturalW, fixed));
        }
        last_ = r;
        lastHot_ = false;
        return r;
    }

    // Hover: the pointer is over it and either nothing is held (and no button is down for someone else's drag) or it is
    // the held widget. Touch (Input::hover false) only highlights the widget being pressed.
    bool Hot(Id id, const Rect& r) const {
        if (!r.Contains(in_.mouseX, in_.mouseY)) return false;
        if (active_ != 0) return active_ == id;
        return in_.hover && !in_.down;
    }

    // Button behaviour shared by buttons, toggles and segments: activate on a press inside, click on a release inside.
    bool Press(Id id, const Rect& r, bool& hot) {
        if (active_ == id) activeSeen_ = true;
        if (in_.pressed && active_ == 0 && r.Contains(in_.pressX, in_.pressY)) active_ = id, activeSeen_ = true;
        hot = Hot(id, r);
        bool clicked = false;
        if (active_ == id && in_.released) {
            clicked = r.Contains(in_.mouseX, in_.mouseY);
            active_ = 0;
            hot = clicked && in_.hover;
        }
        last_ = r;
        lastHot_ = hot;
        return clicked;
    }

    // Text vertically centred in a box (by cap height), aligned left (0) or centred (0.5). Too wide: the size shrinks in
    // half-pixel steps down to Theme::minFontSize, then characters are cut from the end, so text never spills out.
    void DrawText(const Rect& box, const char* text, size_t n, Color color, float alignX, float size, FontFace face, float padX = -1.0f) {
        if (n == 0) return;
        const float pad = padX >= 0.0f ? padX : (alignX == 0.5f ? t_.textPad * 0.5f : t_.textPad);
        const float avail = box.w - 2.0f * pad;
        const float minSize = dl_.Classic() ? 7.0f : t_.minFontSize; // classic: down to one-pixel font cells, as before
        while (dl_.TextWidth(text, n, size, face) > avail && size - 0.5f >= minSize) size -= 0.5f;
        while (n > 0 && dl_.TextWidth(text, n, size, face) > avail) --n;
        if (n == 0) return;
        const float w = dl_.TextWidth(text, n, size, face);
        const float x = alignX == 0.0f ? box.x + pad : box.x + (box.w - w) * alignX;
        const float baseline = box.y + (box.h + dl_.CapHeight(size, face)) * 0.5f;
        dl_.Text(x, baseline, size, face, text, n, color);
    }

    Theme base_, t_;
    Input in_;
    DrawList dl_;
    bool classic_ = false;
    std::vector<Rect> panels_, shown_;
    std::vector<PanelMemory> memory_;
    AnimSlot anims_[kAnimSlots] = {};
    uint32_t frame_ = 0;
    float inset_[4] = {0, 0, 0, 0};
    Id ids_[kMaxIdDepth] = {};
    int idDepth_ = 0;
    Id active_ = 0;
    bool activeSeen_ = false, wantsMouse_ = false;

    bool inPanel_ = false, hasTitle_ = false;
    Rect panel_, last_;
    bool lastHot_ = false;
    Id panelId_ = 0;
    size_t bg_ = 0;
    float pad_ = 0.0f;
    float fixedW_ = 0.0f, fixedH_ = 0.0f;
    float contentX_ = 0.0f, contentW_ = 0.0f, cursorY_ = 0.0f, contentTop_ = 0.0f, bottom_ = 0.0f, naturalW_ = 0.0f;
    float nextW_ = 0.0f;
    Row row_;
};

} // namespace gui
} // namespace tbd
