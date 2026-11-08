// The aim dot: on touch, while the road / rail tool is out, the screen centre is the placement cursor. One finger drags
// the map under the dot (the camera pans; a drag or a tap on the map never places anything), two fingers stay the camera
// (zoom, twist, tilt), and the tick button places the point at the dot; the cross takes the last point back (none
// pending: undoes the last piece). The dot snaps like the mouse cursor (RoadTool::Move at the centre: nodes, open ends,
// segments, guidelines) and shows it: the ring turns amber on a snap and green on a connection, with the snap's name and
// the piece's length / slope under it. Only roads and rails use it (not buildings, zones, terrain or the bulldozer
// tools; not the road tool's Swap mode, which is a drag); the PC keeps the mouse, and the toolkit's dot button turns it
// off (road_toolkit.h State::dotOn).
// Platform free: tests/road_reticle_test.cpp drives the routing, the buttons and the snaps headless.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../city/road_tool.h"
#include "glass_dock.h"
#include "safe_area.h"

namespace tbd {
namespace gui {
namespace reticle {

// Where a finger landing on the screen goes (the platform shell asks the host: game_host.h TouchBuilds).
enum class Route { Gui, Tool, Camera };
inline Route RouteTouch(bool onGui, bool toolActive, bool dotMode) {
    if (onGui) return Route::Gui;
    if (dotMode) return Route::Camera; // the dot places, the finger only moves the map
    return toolActive ? Route::Tool : Route::Camera;
}

// The dot is the cursor: a touch screen, the toggle on, the road tool out in a mode that acts at a point.
inline bool Wanted(const city::RoadTool& t, bool touch, bool dotOn) { return touch && dotOn && t.active && t.mode != city::ToolMode::Swap; }

// A finger's reach in metres at the dot: the snap radius (RoadTool::snapRadius) from `px` pixels on the screen and the
// ground distance one pixel covers there.
inline float SnapRadiusMetres(float metresPerPixel, float px = 56.0f) { return std::clamp(metresPerPixel * px, 4.0f, 60.0f); }

// The tick: the cursor moves to the dot's ground point (snapped) and clicks. True when a point was taken or a piece built.
inline bool PlaceAtDot(city::RoadTool& t, bool onGround, tbd::Vec3 groundAtDot) {
    t.Move(onGround, groundAtDot);
    if (!t.haveCursor) return false;
    const size_t before = t.pts.size();
    const uint64_t v = t.net.version;
    const bool built = t.Click();
    return built || t.net.version != v || t.pts.size() != before;
}
// The cross: drop the last point of the piece being drawn; nothing pending: undo the last piece. True when it did.
inline bool Back(city::RoadTool& t) {
    if (t.Pending()) {
        if (t.pts.size() > 1) t.pts.pop_back();
        else t.Cancel();
        return true;
    }
    return t.Undo();
}

struct State {
    float pulse = 0;   // the ring's breathing (seconds)
    float placedT = 9; // since the last placement (the ring pops)
    std::string hint;  // why the tick did nothing
    float hintT = 9;
};
struct Out {
    bool placed = false, back = false;
    std::vector<float> sfx;
};
struct Layout {
    float cx = 0, cy = 0;                         // the dot, points
    float okX = 0, okY = 0, okS = 0;              // the tick
    float backX = 0, backY = 0, backS = 0;        // the cross
};

constexpr float kOk = 64, kBack = 48, kRing = 15;

// The buttons sit on the right thumb, above the kit's strips (rightTop: points from the top of the screen to them).
inline Layout Place(const Context& g, const SafeInsets& sa, float rightTop) {
    const Input& in = g.In();
    const float s = g.Scale(), W = in.viewW / s, H = in.viewH / s;
    Layout L;
    L.cx = W * 0.5f, L.cy = H * 0.5f;
    L.okS = kOk, L.backS = kBack;
    const float bottom = std::min(rightTop, H - sa.B()) - 12;
    L.okX = W - sa.R() - 16 - kOk, L.okY = std::max(sa.T() + 8, bottom - kOk);
    L.backX = L.okX - 12 - kBack, L.backY = L.okY + (kOk - kBack) * 0.5f;
    return L;
}

// One frame: the dot, its labels and the two buttons. groundAtDot: the ground under the screen centre (the host's ray).
inline Layout Frame(Context& g, const dock::Atlas& atlas, city::RoadTool& t, State& st, const SafeInsets& sa, float rightTop, bool onGround,
                    tbd::Vec3 groundAtDot, Out& out) {
    using namespace dock;
    out = Out{};
    Layout L = Place(g, sa, rightTop);
    if (!t.active) return L;
    const Input& in = g.In();
    const float s = g.Scale();
    Painter P(g, atlas, s);
    DrawList& dl = P.dl;
    st.pulse += in.dt, st.placedT += in.dt, st.hintT += in.dt;
    auto tap = [&](float x, float y, float w, float h, bool* held) {
        const Rect r{x * s, y * s, w * s, h * s};
        const bool inside = r.Contains(in.mouseX, in.mouseY), pressedIn = r.Contains(in.pressX, in.pressY);
        if (held) *held = in.down && pressedIn && inside;
        return in.released && pressedIn && inside;
    };

    // ---- the buttons (marked as GUI: a finger on them is never the map's)
    bool okHeld = false, backHeld = false;
    const bool ok = tap(L.okX, L.okY, kOk, kOk, &okHeld), back = tap(L.backX, L.backY, kBack, kBack, &backHeld);
    if (ok) {
        if (PlaceAtDot(t, onGround, groundAtDot)) out.placed = true, st.placedT = 0, out.sfx.push_back(t.Pending() ? 1.3f : 1.5f);
        else st.hint = !t.error.empty() ? t.error : !onGround ? "Aim the dot at the ground" : "Nothing to place here", st.hintT = 0, out.sfx.push_back(0.6f);
    }
    if (back) {
        if (Back(t)) out.back = true, out.sfx.push_back(0.7f);
        else st.hint = "Nothing to take back", st.hintT = 0;
    }
    {
        const float x = L.okX, y = L.okY;
        dl.Shadow(x * s, (y + 6) * s, (x + kOk) * s, (y + kOk + 6) * s, kOk * 0.5f * s, 10 * s, Rgba(0, 0, 0, 85));
        P.Box(x, y, kOk, kOk, kOk * 0.5f, okHeld ? Mix(kAccent, kInk, 0.15f) : kAccent, 2, Rgba(255, 255, 255, 120));
        const float cx = (x + kOk * 0.5f) * s, cy = (y + kOk * 0.5f) * s, u = kOk * 0.16f * s, w = std::max(1.5f, 3.2f * s);
        dl.Line(cx - 1.3f * u, cy + 0.05f * u, cx - 0.35f * u, cy + 1.0f * u, w, kInk); // the tick
        dl.Line(cx - 0.35f * u, cy + 1.0f * u, cx + 1.45f * u, cy - 1.0f * u, w, kInk);
        g.Block({x * s, y * s, kOk * s, kOk * s});
    }
    {
        const float x = L.backX, y = L.backY;
        P.Glass(x, y, kBack, kBack, kBack * 0.5f, backHeld ? Rgba(60, 62, 70, 200) : kPanel, 18, 1.2f, true);
        DrawIcon(dl, t.Pending() ? Icon::Close : Icon::Undo, (x + kBack * 0.5f) * s, (y + kBack * 0.5f) * s, std::round(kBack * 0.42f * s), std::max(1.0f, 2.0f * s), kInk);
    }

    // ---- the dot: a ring and a centre point; amber on a snap, green on a connection, red when the piece is refused
    const bool snapped = t.haveCursor && t.cursor.kind && t.cursor.kind[0];
    const bool joins = t.haveCursor && city::IsConnection(t.cursor.kind);
    const bool bad = t.haveCursor && !t.error.empty();
    const Color ring = !onGround ? Rgba(255, 255, 255, 120) : bad ? Rgba(255, 110, 90) : joins ? Rgba(90, 240, 140) : snapped ? Rgba(255, 210, 70) : kInk;
    const float pop = st.placedT < 0.25f ? 1.0f + 0.5f * (1.0f - st.placedT / 0.25f) : 1.0f;
    const float r = (snapped ? kRing * 1.15f : kRing) * pop * (1.0f + 0.04f * std::sin(st.pulse * 4.0f));
    const float cx = L.cx * s, cy = L.cy * s, w = std::max(1.5f, 2.2f * s);
    dl.Circle(cx, cy, (r + 1.5f) * s, 0, std::max(1.0f, 1.2f * s), Rgba(0, 0, 0, 120)); // a dark rim reads on snow and on water
    dl.Circle(cx, cy, r * s, 0, w, ring);
    dl.Circle(cx, cy, std::max(2.0f, 2.6f * s), ring);
    for (int i = 0; i < 4; ++i) { // ticks out of the ring
        const float a = i * 1.5707963f, ux = std::cos(a), uy = std::sin(a);
        dl.Line(cx + ux * (r + 3) * s, cy + uy * (r + 3) * s, cx + ux * (r + 9) * s, cy + uy * (r + 9) * s, w, ring);
    }
    // ---- under it: the snap's name, then the piece's numbers (the same labels the cursor has)
    float ly = L.cy + r + 22;
    auto chip = [&](const std::string& text, Color fill, Color ink) {
        const float tw = P.TextW(text, 11, true) + 16, th = 22;
        P.Box(L.cx - tw * 0.5f, ly - th * 0.5f, tw, th, 11, fill);
        P.Text(text, L.cx, ly, 11, ink, true, 0.5f);
        ly += th + 4;
    };
    if (snapped) chip(city::SnapLabel(t.cursor.kind), joins ? Rgba(30, 120, 70, 220) : Rgba(120, 90, 20, 220), kInk);
    if (bad) chip(t.error, Rgba(170, 40, 34, 230), kInk);
    else if (t.Pending()) {
        std::vector<city::WorldLabel> labels;
        t.Labels(labels);
        if (!labels.empty()) chip(labels.front().text, Rgba(20, 22, 28, 210), kInk);
    }
    if (st.hintT < 2.5f && !st.hint.empty()) chip(st.hint, Rgba(20, 22, 28, 210), Fade(kInk, std::min(1.0f, (2.5f - st.hintT) * 2)));
    return L;
}

} // namespace reticle
} // namespace gui
} // namespace tbd
