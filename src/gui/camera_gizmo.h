// The camera gizmo: a small 2D compass at the top left (both HUDs), clear of the Dynamic Island / notch (safe_area.h).
//   the dial      its needle points at north as the camera sees it (it turns with the heading); the ellipse inside is the
//                 ground seen at the camera's tilt (a flat line at the horizon, a full circle straight down)
//   tap           north up (animated)                double tap   straight down and back to the tilt it had
//   drag the ring turns the heading with the finger (Settings > Controls' rotate speed and 15 degree snap)
//   long press    a popover: N / E / S / W, Top (straight down) and 45 (isometric)
//   the 2D / 3D segment under the dial    straight down and back, like the double tap
// Hidden by the host while a modal page is up. Platform free: tests\camera_gizmo_test.cpp covers the hit tests, the heading
// math and the presets headless. Heading: the camera's yaw (city_camera.h, camera.h): yaw 0 faces north (-Z), and north
// shows on the dial `yaw` radians clockwise from the top.
#pragma once
#include <algorithm>
#include <cmath>

#include "gui.h"
#include "safe_area.h"
#include "../camera.h"
#include "../city_camera.h"

namespace tbd {
namespace gui {
namespace gizmo {

constexpr float kPi = 3.14159265f;
constexpr float kR = 24.0f;          // the dial's radius (points): 48 pt across
constexpr float kMargin = 10.0f;     // from the safe area's top left
constexpr float kSegW = 48.0f, kSegH = 44.0f; // the 2D / 3D segment's touch target (drawn 40 x 22)
constexpr int kPops = 6;             // the popover: N, E, S, W, Top, 45
constexpr float kPopB = 44.0f, kPopGap = 4.0f;
constexpr float kIsoPitch = 0.7853982f; // 45 degrees down
constexpr float kAnimSec = 0.35f;

inline float Wrap(float a) { // to -pi .. pi
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
// Where north shows on the dial: radians clockwise from the top.
inline float NorthOnDial(float yaw) { return Wrap(yaw); }
// The yaw that faces a compass point: 0 N, 1 E, 2 S, 3 W (forward = (-sin yaw, -cos yaw); east is +X).
inline float CompassYaw(int point) {
    static const float k[4] = {0.0f, -kPi * 0.5f, kPi, kPi * 0.5f};
    return k[((point % 4) + 4) % 4];
}
// The nearest yaw to `from` that faces the same way as `target` (an animation never turns the long way round).
inline float NearestYaw(float from, float target) { return from + Wrap(target - from); }
// A finger's bearing around the dial's centre, clockwise from the top (screen y grows downward).
inline float Bearing(float cx, float cy, float x, float y) { return std::atan2(x - cx, -(y - cy)); }

struct Layout {
    float cx = 0, cy = 0, r = kR;    // the dial (points)
    Rect seg;                         // the 2D / 3D segment (points)
    float popX = 0, popY = 0;         // the popover's first button
};
// leftPt / topPt: the safe area's left and top insets (points).
inline Layout Place(float leftPt, float topPt) {
    Layout L;
    L.cx = leftPt + kMargin + kR, L.cy = topPt + kMargin + kR;
    L.seg = {L.cx - kSegW * 0.5f, L.cy + kR + 2.0f, kSegW, kSegH};
    L.popX = leftPt + kMargin, L.popY = L.seg.y + L.seg.h + 6.0f;
    return L;
}
// The whole gizmo's rectangle (points), for the GUI's Block (popover: with it open).
inline Rect Bounds(const Layout& L, bool popover) {
    const float x0 = L.cx - L.r - 2.0f, y0 = L.cy - L.r - 2.0f;
    float x1 = L.cx + L.r + 2.0f, y1 = L.seg.y + L.seg.h;
    if (popover) x1 = std::max(x1, L.popX + kPops * (kPopB + kPopGap)), y1 = L.popY + kPopB;
    return {x0, y0, x1 - x0, y1 - y0};
}

enum class Hit { None, Dial, Segment, Pop };
// What a point (points) is over; `pop` gets the popover button (0..5) on Hit::Pop. The dial's target is at least 44 pt.
inline Hit HitTest(const Layout& L, float x, float y, bool popover, int* pop = nullptr) {
    const float dx = x - L.cx, dy = y - L.cy, reach = std::max(L.r + 2.0f, 22.0f);
    if (dx * dx + dy * dy <= reach * reach) return Hit::Dial;
    if (L.seg.Contains(x, y)) return Hit::Segment;
    if (popover)
        for (int i = 0; i < kPops; ++i) {
            const Rect b{L.popX + i * (kPopB + kPopGap), L.popY, kPopB, kPopB};
            if (b.Contains(x, y)) {
                if (pop) *pop = i;
                return Hit::Pop;
            }
        }
    return Hit::None;
}

// A preset's target: heading (yaw) and tilt (pitch); keepYaw / keepPitch leave that one alone.
struct Target {
    float yaw = 0, pitch = 0;
    bool keepYaw = false, keepPitch = false;
};
// The popover's buttons: 0..3 the compass points (the tilt stays), 4 straight down, 5 isometric 45 (the heading stays).
inline Target PopTarget(int i) {
    Target t;
    if (i >= 0 && i < 4) t.yaw = CompassYaw(i), t.keepPitch = true;
    else t.pitch = i == 4 ? citycam::kMaxPitch : kIsoPitch, t.keepYaw = true;
    return t;
}
inline const char* PopLabel(int i) {
    static const char* const k[kPops] = {"N", "E", "S", "W", "Top", "45"};
    return i >= 0 && i < kPops ? k[i] : "";
}
// Straight down counts from a degree or so off it.
inline bool IsTopDown(float pitch) { return pitch >= citycam::kMaxPitch - 0.02f; }

struct Params {
    float rotateSpeed = 1.0f; // Settings > Controls: twist (touch) or right-drag rotate (mouse) speed
    bool snap = false;        // ... and its 15 degree snap
    float tapMaxSec = 0.45f;  // a press longer than this, unmoved, is the long press
    float slopPt = 10.0f;     // a finger that travels more is a drag
    float doubleTapSec = 0.35f;
};

struct State {
    // the press
    bool pressed = false, dragging = false, longDone = false;
    Hit pressHit = Hit::None;
    float pressT = 0, lastAng = 0, rawYaw = 0;
    float clock = 0, lastTap = -10.0f;
    bool popover = false;
    // the animation
    bool anim = false;
    float animT = 0, fromYaw = 0, toYaw = 0, fromPitch = 0, toPitch = 0;
    bool animYaw = false, animPitch = false;
    float savedPitch = -1.0f; // the tilt before straight down (-1: none)
};

// Starts the eased move to t from the camera's heading / tilt now.
inline void Animate(State& st, const OrbitCamera& cam, const Target& t) {
    st.anim = true, st.animT = 0;
    st.fromYaw = cam.yaw, st.toYaw = t.keepYaw ? cam.yaw : NearestYaw(cam.yaw, t.yaw), st.animYaw = !t.keepYaw;
    st.fromPitch = cam.pitch, st.toPitch = t.keepPitch ? cam.pitch : citycam::ClampPitch(t.pitch), st.animPitch = !t.keepPitch;
}
// Straight down, or back to the tilt before it (the 2D / 3D segment and the double tap).
inline void ToggleTop(State& st, const OrbitCamera& cam) {
    Target t;
    t.keepYaw = true;
    if (IsTopDown(cam.pitch)) t.pitch = st.savedPitch >= 0.0f && !IsTopDown(st.savedPitch) ? st.savedPitch : kIsoPitch, st.savedPitch = -1.0f;
    else st.savedPitch = cam.pitch, t.pitch = citycam::kMaxPitch;
    Animate(st, cam, t);
}
// One step of the animation (dt seconds): ease-out over kAnimSec. True while it runs.
inline bool Step(State& st, OrbitCamera& cam, float dt) {
    if (!st.anim) return false;
    st.animT = std::min(st.animT + dt, kAnimSec);
    const float u = st.animT / kAnimSec, e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
    if (st.animYaw) cam.yaw = st.fromYaw + (st.toYaw - st.fromYaw) * e;
    if (st.animPitch) cam.pitch = st.fromPitch + (st.toPitch - st.fromPitch) * e;
    if (st.animT >= kAnimSec) st.anim = false;
    return st.anim;
}
// The ring drag: the heading turns by the finger's change of bearing (times the speed; snapped to 15 degrees when asked).
inline void DragTurn(State& st, OrbitCamera& cam, float bearingDelta, const Params& p) {
    st.rawYaw += Wrap(bearingDelta) * p.rotateSpeed;
    const float step = 15.0f * kPi / 180.0f;
    cam.yaw = p.snap ? std::round(st.rawYaw / step) * step : st.rawYaw;
}

struct Out {
    bool used = false; // the gizmo took the pointer this frame
};

// The press, the drag, the taps and the long press (points; the host's Input in pixels, s pixels per point).
inline void HandleInput(State& st, OrbitCamera& cam, const Layout& L, const Input& in, float s, const Params& p, Out& out) {
    const float mx = in.mouseX / s, my = in.mouseY / s, px = in.pressX / s, py = in.pressY / s;
    if (in.pressed) {
        int pop = -1;
        const Hit h = HitTest(L, px, py, st.popover, &pop);
        if (h == Hit::None) st.popover = false; // a press anywhere else closes the popover
        else {
            st.pressed = true, st.pressHit = h, st.pressT = 0, st.dragging = false, st.longDone = false;
            st.lastAng = Bearing(L.cx, L.cy, px, py), st.rawYaw = cam.yaw;
        }
    }
    if (!st.pressed) return;
    out.used = true;
    if (in.down) {
        st.pressT += in.dt;
        if (st.pressHit == Hit::Dial) {
            if (!st.dragging && !st.longDone && std::hypot(mx - px, my - py) > p.slopPt) st.dragging = true, st.anim = false;
            if (st.dragging) {
                const float ang = Bearing(L.cx, L.cy, mx, my);
                DragTurn(st, cam, ang - st.lastAng, p);
                st.lastAng = ang;
            } else if (!st.longDone && st.pressT > p.tapMaxSec) st.longDone = true, st.popover = !st.popover;
        }
        return;
    }
    // released (or the press was lost)
    int pop = -1;
    const Hit now = HitTest(L, mx, my, st.popover, &pop);
    if (in.released && st.pressHit == Hit::Dial && now == Hit::Dial && !st.dragging && !st.longDone) {
        if (st.clock - st.lastTap < p.doubleTapSec) { // a double tap: straight down / back, the first tap's turn to north goes on
            const bool yawWas = st.anim && st.animYaw;
            const float to = st.toYaw;
            ToggleTop(st, cam);
            if (yawWas) st.animYaw = true, st.toYaw = to;
            st.lastTap = -10.0f;
        } else {
            Target t;
            t.yaw = 0.0f, t.keepPitch = true;
            Animate(st, cam, t);
            st.lastTap = st.clock;
        }
    }
    if (in.released && st.pressHit == Hit::Segment && now == Hit::Segment) ToggleTop(st, cam);
    if (in.released && st.pressHit == Hit::Pop && now == Hit::Pop) {
        int was = -1;
        HitTest(L, in.pressX / s, in.pressY / s, true, &was);
        if (was == pop) {
            if (pop == 4 && !IsTopDown(cam.pitch)) st.savedPitch = cam.pitch;
            Animate(st, cam, PopTarget(pop));
            st.popover = false;
        }
    }
    st.pressed = false;
}

// One frame: steps the animation, handles the pointer, draws. sa: the safe area (points). Call it only while the city
// camera is on and no modal page is up (the host's job).
inline Out Frame(Context& g, State& st, OrbitCamera& cam, const SafeInsets& sa, const Params& p) {
    Out out;
    const Input& in = g.In();
    const float s = std::max(g.Scale(), 0.01f);
    st.clock += in.dt;
    const Layout L = Place(sa.L(), sa.T());
    Step(st, cam, in.dt);
    HandleInput(st, cam, L, in, s, p, out);
    const Rect b = Bounds(L, st.popover);
    g.Block({b.x * s, b.y * s, b.w * s, b.h * s});

    DrawList& dl = g.Draw();
    const Color ink = Rgba(255, 255, 255), faint = Rgba(255, 255, 255, 70);
    const bool held = st.pressed && st.pressHit == Hit::Dial;
    // the dial
    dl.Circle(L.cx * s, L.cy * s, L.r * s, held ? Rgba(60, 62, 70, 190) : Rgba(28, 29, 34, 150), std::max(1.0f, s), Rgba(255, 255, 255, 60));
    { // the ground at the camera's tilt
        const float rx = L.r * 0.62f, ry = std::max(0.04f * L.r, rx * std::sin(std::clamp(cam.pitch, 0.0f, kPi * 0.5f)));
        float ox = L.cx + rx, oy = L.cy;
        for (int i = 1; i <= 20; ++i) {
            const float a = 2.0f * kPi * (float)i / 20.0f, x = L.cx + rx * std::cos(a), y = L.cy + ry * std::sin(a);
            dl.Line(ox * s, oy * s, x * s, y * s, std::max(1.0f, 1.2f * s), faint);
            ox = x, oy = y;
        }
    }
    const float a = NorthOnDial(cam.yaw), dx = std::sin(a), dy = -std::cos(a);
    for (int k = 1; k < 4; ++k) { // E, S, W ticks at the rim
        const float ak = a + k * kPi * 0.5f, tx = std::sin(ak), ty = -std::cos(ak);
        dl.Line((L.cx + tx * L.r * 0.8f) * s, (L.cy + ty * L.r * 0.8f) * s, (L.cx + tx * L.r * 0.95f) * s, (L.cy + ty * L.r * 0.95f) * s, std::max(1.0f, 1.5f * s), faint);
    }
    dl.Line(L.cx * s, L.cy * s, (L.cx - dx * L.r * 0.42f) * s, (L.cy - dy * L.r * 0.42f) * s, std::max(1.5f, 3.0f * s), Rgba(255, 255, 255, 170));
    dl.Line(L.cx * s, L.cy * s, (L.cx + dx * L.r * 0.5f) * s, (L.cy + dy * L.r * 0.5f) * s, std::max(1.5f, 3.0f * s), Rgba(255, 69, 58));
    dl.Circle(L.cx * s, L.cy * s, 2.5f * s, ink);
    {
        const float nx = L.cx + dx * L.r * 0.74f, ny = L.cy + dy * L.r * 0.74f, bw = 14.0f;
        g.TextIn({(nx - bw * 0.5f) * s, (ny - bw * 0.5f) * s, bw * s, bw * s}, "N", Rgba(255, 105, 97), 0.5f, 11.0f * s, true);
    }
    // the 2D / 3D segment
    {
        const bool top = IsTopDown(cam.pitch), segHeld = st.pressed && st.pressHit == Hit::Segment;
        const float w = 40.0f, h = 22.0f, x = L.seg.x + (L.seg.w - w) * 0.5f, y = L.seg.y + (L.seg.h - h) * 0.5f - 8.0f;
        const Color fill = segHeld ? Rgba(60, 62, 70, 200) : top ? Rgba(10, 132, 255, 170) : Rgba(28, 29, 34, 150);
        dl.Rect(x * s, y * s, (x + w) * s, (y + h) * s, fill, fill, h * 0.5f * s, std::max(1.0f, s), Rgba(255, 255, 255, 60));
        g.TextIn({x * s, y * s, w * s, h * s}, top ? "2D" : "3D", ink, 0.5f, 11.0f * s, true);
    }
    // the popover
    if (st.popover) {
        const float w = kPops * (kPopB + kPopGap) - kPopGap + 8.0f;
        const Color panel = Rgba(28, 29, 34, 210);
        dl.Rect((L.popX - 4.0f) * s, (L.popY - 4.0f) * s, (L.popX - 4.0f + w) * s, (L.popY + kPopB + 4.0f) * s, panel, panel, 14.0f * s);
        for (int i = 0; i < kPops; ++i) {
            const float x = L.popX + i * (kPopB + kPopGap);
            const bool on = i < 4 ? std::fabs(Wrap(cam.yaw - CompassYaw(i))) < 0.01f : i == 4 ? IsTopDown(cam.pitch) : std::fabs(cam.pitch - kIsoPitch) < 0.01f;
            const Color c = on ? Rgba(10, 132, 255, 190) : Rgba(255, 255, 255, 30);
            dl.Rect((x + 2.0f) * s, (L.popY + 2.0f) * s, (x + kPopB - 2.0f) * s, (L.popY + kPopB - 2.0f) * s, c, c, 11.0f * s);
            g.TextIn({x * s, L.popY * s, kPopB * s, kPopB * s}, PopLabel(i), i == 0 ? Rgba(255, 140, 130) : ink, 0.5f, 12.0f * s, true);
        }
    }
    return out;
}

} // namespace gizmo
} // namespace gui
} // namespace tbd
