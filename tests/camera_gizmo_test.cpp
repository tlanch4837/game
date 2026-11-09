// Tests for src/gui/camera_gizmo.h, the 2D compass at the top left: the heading math (north on the dial, compass points,
// the short way round, the finger's bearing), the hit tests and the safe area, the presets' targets, the animation, the
// top-view toggle and the pointer (tap = north up, double tap = top view, drag = turn, long press = the popover). Headless.
#include <cmath>
#include <cstdio>

#include "../src/gui/camera_gizmo.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

namespace gz = tbd::gui::gizmo;
using tbd::OrbitCamera;
constexpr float kPi = 3.14159265f;

static OrbitCamera Cam(float yaw, float pitch) {
    OrbitCamera c;
    c.target = {0, 0, 0}, c.distance = 500.0f, c.yaw = yaw, c.pitch = pitch;
    return c;
}

static void Heading() {
    // north on the dial is where world north (-Z) shows on the screen: x = right . north, up = forward . north
    for (float yaw : {0.0f, 0.4f, 1.5708f, 2.5f, -1.0f, -3.0f}) {
        const OrbitCamera c = Cam(yaw, 0.6f);
        tbd::Vec3 f = c.Forward(), r = c.Right();
        f.y = 0.0f;
        const float fl = std::sqrt(f.x * f.x + f.z * f.z);
        const float sx = r.z * -1.0f, sy = -f.z / fl; // north = (0, 0, -1)
        const float a = gz::NorthOnDial(yaw);
        CHECK(Near(std::sin(a), sx, 1e-3f) && Near(std::cos(a), sy, 1e-3f));
    }
    // the compass points face their way
    const float want[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}}; // N -Z, E +X, S +Z, W -X
    for (int i = 0; i < 4; ++i) {
        const OrbitCamera c = Cam(gz::CompassYaw(i), 0.3f);
        tbd::Vec3 f = c.Forward();
        const float fl = std::sqrt(f.x * f.x + f.z * f.z);
        CHECK(Near(f.x / fl, want[i][0], 1e-3f) && Near(f.z / fl, want[i][1], 1e-3f));
    }
    CHECK(Near(gz::NearestYaw(3.0f, -3.0f), 3.0f + (2.0f * kPi - 6.0f), 1e-3f)); // the short way, across +-pi
    CHECK(Near(gz::NearestYaw(7.0f, 0.0f), 2.0f * kPi, 1e-3f));                  // north from a wound-up heading
    // the finger's bearing: clockwise from the top
    CHECK(Near(gz::Bearing(0, 0, 0, -10), 0.0f) && Near(gz::Bearing(0, 0, 10, 0), kPi * 0.5f) && Near(gz::Bearing(0, 0, -10, 0), -kPi * 0.5f));
    CHECK(Near(std::fabs(gz::Bearing(0, 0, 0, 10)), kPi));
}

static void HitsAndSafeArea() {
    const gz::Layout L = gz::Place(59.0f, 24.0f); // a notched phone in landscape (TBD_GUI_SAFE=24,59,21,59)
    const tbd::gui::Rect b = gz::Bounds(L, false);
    CHECK(b.x >= 59.0f && b.y >= 24.0f);
    CHECK(b.x + b.w <= 59.0f + 60.0f); // left of the Phone UI's status pill (glass_dock.h: x = 60 + SL)
    CHECK(L.r * 2.0f >= 44.0f && L.seg.w >= 44.0f && L.seg.h >= 44.0f && gz::kPopB >= 44.0f); // touch targets
    CHECK(gz::HitTest(L, L.cx, L.cy, false) == gz::Hit::Dial);
    CHECK(gz::HitTest(L, L.cx + L.r * 0.9f, L.cy, false) == gz::Hit::Dial);
    CHECK(gz::HitTest(L, L.seg.x + L.seg.w * 0.5f, L.seg.y + L.seg.h * 0.5f, false) == gz::Hit::Segment);
    CHECK(gz::HitTest(L, L.cx + 200.0f, L.cy, false) == gz::Hit::None);
    for (int i = 0; i < gz::kPops; ++i) {
        const float x = L.popX + i * (gz::kPopB + gz::kPopGap) + gz::kPopB * 0.5f, y = L.popY + gz::kPopB * 0.5f;
        int pop = -1;
        CHECK(gz::HitTest(L, x, y, true, &pop) == gz::Hit::Pop && pop == i);
        CHECK(gz::HitTest(L, x, y, false) != gz::Hit::Pop); // closed: no buttons
    }
    const tbd::gui::Rect bp = gz::Bounds(L, true);
    CHECK(bp.y + bp.h >= L.popY + gz::kPopB && bp.x + bp.w >= L.popX + gz::kPops * gz::kPopB);
    // no safe area (Windows): the top left corner, a margin in
    const gz::Layout W = gz::Place(0.0f, 0.0f);
    CHECK(Near(W.cx - W.r, gz::kMargin) && Near(W.cy - W.r, gz::kMargin));
}

static void Presets() {
    for (int i = 0; i < 4; ++i) {
        const gz::Target t = gz::PopTarget(i);
        CHECK(!t.keepYaw && t.keepPitch && Near(t.yaw, gz::CompassYaw(i)));
    }
    const gz::Target top = gz::PopTarget(4), iso = gz::PopTarget(5);
    CHECK(top.keepYaw && !top.keepPitch && Near(top.pitch, tbd::citycam::kMaxPitch));
    CHECK(iso.keepYaw && Near(iso.pitch, 0.7853982f));
    CHECK(gz::IsTopDown(tbd::citycam::kMaxPitch) && !gz::IsTopDown(1.2f));
}

static void RunTo(gz::State& st, OrbitCamera& c) { for (int i = 0; i < 60; ++i) gz::Step(st, c, 1.0f / 60.0f); }

static void AnimationAndToggle() {
    OrbitCamera c = Cam(2.0f, 0.9f);
    gz::State st;
    gz::Target north;
    north.yaw = 0.0f, north.keepPitch = true;
    gz::Animate(st, c, north);
    gz::Step(st, c, gz::kAnimSec * 0.5f);
    CHECK(c.yaw < 2.0f && c.yaw > 0.0f && Near(c.pitch, 0.9f)); // on its way, the tilt kept
    RunTo(st, c);
    CHECK(Near(c.yaw, 0.0f) && !st.anim);
    // straight down and back to the tilt it had
    gz::ToggleTop(st, c);
    RunTo(st, c);
    CHECK(Near(c.pitch, tbd::citycam::kMaxPitch) && Near(c.yaw, 0.0f));
    gz::ToggleTop(st, c);
    RunTo(st, c);
    CHECK(Near(c.pitch, 0.9f));
    // the ring drag: speed and snap
    gz::Params p;
    p.rotateSpeed = 2.0f;
    st.rawYaw = c.yaw = 0.0f;
    gz::DragTurn(st, c, 0.1f, p);
    CHECK(Near(c.yaw, 0.2f));
    p.rotateSpeed = 1.0f, p.snap = true;
    st.rawYaw = c.yaw = 0.0f;
    gz::DragTurn(st, c, 0.1f, p); // 5.7 degrees: stays at 0
    CHECK(Near(c.yaw, 0.0f));
    gz::DragTurn(st, c, 0.1f, p); // 11.5: 15
    CHECK(Near(c.yaw, 15.0f * kPi / 180.0f));
}

// The pointer, as the GUI's Input (points = pixels here).
struct Finger {
    tbd::gui::Input in;
    void Press(float x, float y) { in.mouseX = in.pressX = x, in.mouseY = in.pressY = y, in.down = in.pressed = true, in.released = false; }
    void Move(float x, float y) { in.mouseX = x, in.mouseY = y, in.pressed = false; }
    void Release() { in.down = in.pressed = false, in.released = true; }
    void Idle() { in.pressed = in.released = false; }
};

static void Pointer() {
    const gz::Layout L = gz::Place(0.0f, 0.0f);
    gz::Params p;
    gz::Out out;
    // a tap: north up
    {
        OrbitCamera c = Cam(1.0f, 0.8f);
        gz::State st;
        Finger f;
        f.Press(L.cx, L.cy), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(out.used);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(st.anim && st.animYaw && !st.animPitch);
        RunTo(st, c);
        CHECK(Near(c.yaw, 0.0f) && Near(c.pitch, 0.8f));
        // a double tap: straight down
        st.clock = 10.0f;
        f.Press(L.cx, L.cy), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        st.clock = 10.2f;
        f.Press(L.cx, L.cy), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        RunTo(st, c);
        CHECK(gz::IsTopDown(c.pitch));
        // the segment: back to the tilt
        f.Press(L.seg.x + L.seg.w * 0.5f, L.seg.y + L.seg.h * 0.5f), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        RunTo(st, c);
        CHECK(Near(c.pitch, 0.8f));
    }
    // a drag around the ring: a quarter turn clockwise turns the heading a quarter (north follows the finger)
    {
        OrbitCamera c = Cam(0.0f, 0.8f);
        gz::State st;
        Finger f;
        f.Press(L.cx, L.cy - 20.0f), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        for (int i = 1; i <= 10; ++i) {
            const float a = kPi * 0.5f * (float)i / 10.0f;
            f.Move(L.cx + 20.0f * std::sin(a), L.cy - 20.0f * std::cos(a)), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        }
        CHECK(Near(c.yaw, kPi * 0.5f, 1e-3f) && Near(gz::NorthOnDial(c.yaw), kPi * 0.5f, 1e-3f));
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(!st.anim); // a drag is no tap
    }
    // a long press: the popover; its E button faces east
    {
        OrbitCamera c = Cam(0.0f, 0.8f);
        gz::State st;
        Finger f;
        f.Press(L.cx, L.cy), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        f.Idle();
        for (int i = 0; i < 40; ++i) gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(st.popover);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(!st.anim && st.popover); // the long press's release is no tap
        const float ex = L.popX + 1 * (gz::kPopB + gz::kPopGap) + 20.0f, ey = L.popY + 20.0f;
        f.Press(ex, ey), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        f.Release(), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        RunTo(st, c);
        CHECK(!st.popover && Near(c.yaw, gz::CompassYaw(1), 1e-3f));
        // a press elsewhere closes it
        st.popover = true;
        f.Press(400.0f, 400.0f), gz::HandleInput(st, c, L, f.in, 1.0f, p, out);
        CHECK(!st.popover);
    }
}

int main() {
    Heading();
    HitsAndSafeArea();
    Presets();
    AnimationAndToggle();
    Pointer();
    std::printf("camera_gizmo_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
