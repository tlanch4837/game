// Tests for Settings > Controls (src/game/control_settings.h, src/gui/controls_page.h): the defaults are today's constants,
// values clamp, settings.ini round-trips, the tap / drag thresholds follow the settings, the camera's zoom / tilt / twist /
// floor scale with them, and the dock's tile lists scroll with the finger (the Phone UI column used to run against it).
// Headless: no renderer, no engine.
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/city_camera.h"
#include "../src/game/control_settings.h"
#include "../src/game/touch_gesture.h"
#include "../src/gui/glass_dock.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

namespace ctl = tbd::controls;
namespace touch = tbd::touch;
namespace cc = tbd::citycam;

static void Defaults() {
    const ctl::Settings d;
    CHECK(Near(d.tapSlopPt, touch::kTapSlopPt));
    CHECK(std::fabs((double)d.tapMaxSec - touch::kTapMaxSeconds) < 1e-6);
    CHECK(Near(d.eyeClearance, cc::kEyeClearance));
    CHECK(Near(d.maxDistance, cc::kMaxDistance));
    CHECK(Near(d.zoomEase, 14.0f));   // game_core.inl's eased zoom
    CHECK(Near(d.twoSlopPt, 12.0f));  // main_ios.mm's two-finger lock
    CHECK(Near(d.dotSnapPt, 28.0f));  // glass_dock_host.inl TouchSnapRadius
    for (float k : {d.touchPan1, d.touchPan2, d.pinchZoom, d.twistSpeed, d.tiltSpeed, d.touchEdgeSpeed, d.mousePan, d.mouseRotate,
                    d.mouseTilt, d.mouseZoom, d.mouseEdgeSpeed, d.keyPan, d.keyRotate})
        CHECK(Near(k, 1.0f));
    CHECK(!d.twistSnap && !d.invertTilt && !d.invertPinch && d.touchZoomToPoint);
    CHECK(!d.mouseInvertRotate && !d.mouseInvertTilt && !d.mouseInvertZoom && d.mouseZoomToCursor && !d.mouseEdge);
    CHECK(!d.invertMenuScroll);
    CHECK(ctl::IsDefault(d));
    CHECK(ctl::Serialize(d).empty()); // a file with no ctl_ keys is today's behaviour
    // every default sits inside its own range, and every key is unique
    int n = 0;
    const ctl::Desc* t = ctl::Table(n);
    CHECK(n > 25);
    for (int i = 0; i < n; ++i) {
        CHECK((t[i].f != nullptr) != (t[i].b != nullptr));
        if (t[i].f) CHECK(d.*(t[i].f) >= t[i].lo && d.*(t[i].f) <= t[i].hi);
        for (int j = i + 1; j < n; ++j) CHECK(std::string(t[i].key) != t[j].key);
    }
}

static void Clamping() {
    ctl::Settings s;
    s.touchPan1 = 100.0f, s.tapSlopPt = -5.0f, s.maxDistance = 1e7f, s.zoomEase = std::nanf("");
    ctl::Clamp(s);
    CHECK(Near(s.touchPan1, 3.0f));
    CHECK(Near(s.tapSlopPt, 4.0f));
    CHECK(Near(s.maxDistance, 9000.0f));
    CHECK(Near(s.zoomEase, 14.0f)); // NaN: the default
    // the camera's own clamp never goes past the map, whatever it is given
    CHECK(Near(cc::ClampDistance(1e6f, 1e6f), cc::kMaxDistance));
    CHECK(Near(cc::ClampDistance(5000.0f, 2000.0f), 2000.0f));
    CHECK(Near(cc::ClampDistance(5000.0f), 5000.0f));
    // a file's out-of-range values clamp too
    ctl::Settings p;
    ctl::Parse("ctl_pinch=99\nctl_tap_time=0\n", p);
    CHECK(Near(p.pinchZoom, 3.0f));
    CHECK(Near(p.tapMaxSec, 0.15f));
}

static void RoundTrip() {
    ctl::Settings s;
    s.touchPan1 = 1.5f, s.pinchZoom = 0.5f, s.twistSnap = true, s.invertTilt = true, s.tapSlopPt = 16.0f, s.tapMaxSec = 0.6f;
    s.eyeClearance = 5.0f, s.maxDistance = 4000.0f, s.mouseZoomToCursor = false, s.mouseEdge = true, s.keyPan = 2.0f, s.invertMenuScroll = true;
    const std::string text = ctl::Serialize(s);
    ctl::Settings r;
    ctl::Parse(text, r);
    int n = 0;
    const ctl::Desc* t = ctl::Table(n);
    for (int i = 0; i < n; ++i) {
        if (t[i].f) CHECK(Near(r.*(t[i].f), s.*(t[i].f), 1e-4f));
        else CHECK(r.*(t[i].b) == s.*(t[i].b));
    }
    // Merge keeps the other keys (in order), replaces the old ctl_ lines, and reads back with \r\n line ends
    const std::string file = "preset=2\r\nctl_pan1=2.5\r\nvol_master=80\r\nctl_m_edge=0\r\nstats=1\r\n";
    const std::string merged = ctl::Merge(file, s);
    CHECK(merged.find("preset=2\nvol_master=80\nstats=1\n") == 0);
    CHECK(merged.find("ctl_pan1=2.5") == std::string::npos);
    CHECK(merged.find("ctl_m_edge=1") != std::string::npos);
    ctl::Settings m;
    ctl::Parse(merged, m);
    CHECK(Near(m.touchPan1, 1.5f) && m.mouseEdge && m.invertMenuScroll);
    ctl::Settings crlf;
    ctl::Parse("ctl_pan2=2\r\nctl_tilt_inv=1\r\njunk\r\nctl_pan1=abc\r\n", crlf);
    CHECK(Near(crlf.touchPan2, 2.0f) && crlf.invertTilt && Near(crlf.touchPan1, 1.0f));
    // back to the defaults: nothing written
    CHECK(ctl::Merge(merged, ctl::Settings{}) == "preset=2\nvol_master=80\nstats=1\n");
}

static void Gestures() {
    const ctl::Settings d;
    // the defaults classify exactly as the constants did
    for (float travel : {0.0f, 5.0f, 9.9f, 10.0f, 10.1f, 30.0f})
        for (double held : {0.0, 0.2, 0.45, 0.46, 1.0})
            for (bool lifted : {false, true}) CHECK(touch::Classify(travel, held, lifted, d) == touch::Classify(travel, held, lifted));
    // a wider slop: 15 pt is still a tap; a narrower one: 6 pt is already a drag
    ctl::Settings wide;
    wide.tapSlopPt = 20.0f;
    CHECK(touch::Classify(15.0f, 0.1, true) == touch::Gesture::Drag);
    CHECK(touch::Classify(15.0f, 0.1, true, wide) == touch::Gesture::Tap);
    ctl::Settings narrow;
    narrow.tapSlopPt = 5.0f;
    CHECK(touch::Classify(6.0f, 0.1, false, narrow) == touch::Gesture::Drag);
    CHECK(touch::Classify(6.0f, 0.1, false) == touch::Gesture::Undecided);
    // a longer tap time: 0.8 s is a tap, not a hold; a shorter one: 0.3 s is already a long-press
    ctl::Settings slow;
    slow.tapMaxSec = 1.0f;
    CHECK(touch::Classify(2.0f, 0.8, true) == touch::Gesture::Hold);
    CHECK(touch::Classify(2.0f, 0.8, true, slow) == touch::Gesture::Tap);
    ctl::Settings quick;
    quick.tapMaxSec = 0.25f;
    CHECK(touch::Classify(2.0f, 0.3, false, quick) == touch::Gesture::Hold);
    CHECK(touch::Classify(2.0f, 0.3, false) == touch::Gesture::Undecided);
}

static tbd::OrbitCamera CityStart() {
    tbd::OrbitCamera c;
    c.target = {0.0f, 0.0f, -1000.0f};
    c.distance = 3200.0f;
    c.pitch = 1.1f;
    c.yaw = 0.0f;
    c.fovY = 0.9f;
    return c;
}
static float Flat(float, float) { return 5.0f; }

static void Camera() {
    const ctl::Settings d;
    // zoom: the defaults leave the notches alone (wheel and pinch); speed scales, invert flips, each on its own input
    CHECK(Near(ctl::ZoomNotches(d, 2.0f, true), 2.0f) && Near(ctl::ZoomNotches(d, -1.5f, false), -1.5f));
    ctl::Settings z;
    z.pinchZoom = 2.0f, z.invertPinch = true, z.mouseZoom = 0.5f;
    CHECK(Near(ctl::ZoomNotches(z, 1.0f, true), -2.0f));
    CHECK(Near(ctl::ZoomNotches(z, 1.0f, false), 0.5f));
    z.mouseInvertZoom = true;
    CHECK(Near(ctl::ZoomNotches(z, 1.0f, false), -0.5f));
    // the max zoom distance caps the zoom out
    CHECK(Near(cc::ZoomedDistance(3000.0f, -40.0f, 2000.0f), 2000.0f));
    CHECK(Near(cc::ZoomedDistance(3000.0f, -40.0f), cc::kMaxDistance));
    CHECK(Near(cc::ZoomedDistance(3000.0f, 1.0f, d.maxDistance), cc::ZoomedDistance(3000.0f, 1.0f)));
    // tilt: speed and invert
    CHECK(Near(ctl::TouchTilt(d, 0.1f), 0.1f));
    ctl::Settings t;
    t.tiltSpeed = 2.0f, t.invertTilt = true;
    CHECK(Near(ctl::TouchTilt(t, 0.1f), -0.2f));
    // twist snap: 15 degree steps
    const float deg = 3.14159265f / 180.0f;
    CHECK(Near(ctl::SnapYaw(7.0f * deg), 0.0f, 1e-4f) && Near(ctl::SnapYaw(8.0f * deg), 15.0f * deg, 1e-4f));
    CHECK(Near(ctl::SnapYaw(-50.0f * deg), -45.0f * deg, 1e-4f));
    // the eased zoom: 14 / s by default (game_core.inl's old constant); higher is snappier
    CHECK(Near(ctl::ZoomBlend(d, 1.0f / 60.0f), 1.0f - std::exp(-14.0f / 60.0f)));
    ctl::Settings e;
    e.zoomEase = 40.0f;
    CHECK(ctl::ZoomBlend(e, 1.0f / 60.0f) > ctl::ZoomBlend(d, 1.0f / 60.0f));
    // the floor: the default clearance is the old one; a higher one keeps the eye higher (the tilt never changes)
    tbd::OrbitCamera a = CityStart(), b = CityStart();
    a.pitch = b.pitch = cc::kMinPitch, a.distance = b.distance = 3.0f;
    cc::GroundFloor(a, Flat);
    cc::GroundFloor(b, Flat, d.eyeClearance, d.maxDistance);
    CHECK(Near(a.Eye().y, b.Eye().y) && Near(a.target.y, b.target.y));
    tbd::OrbitCamera c = CityStart();
    c.pitch = cc::kMinPitch, c.distance = 3.0f;
    cc::GroundFloor(c, Flat, 12.0f);
    CHECK(c.Eye().y >= 5.0f + 12.0f - 1e-3f && Near(c.pitch, cc::kMinPitch));
    tbd::OrbitCamera far = CityStart();
    far.distance = 8000.0f;
    cc::GroundFloor(far, Flat, 2.0f, 3000.0f);
    CHECK(Near(far.distance, 3000.0f));
    // the mouse edge scroll's band
    CHECK(Near(ctl::EdgePush(2.0f, 1000.0f), -1.0f) && Near(ctl::EdgePush(995.0f, 1000.0f), 1.0f) && Near(ctl::EdgePush(500.0f, 1000.0f), 0.0f));
}

// The dock's tile lists: a drag of +dy points moves the tiles +dy (with the finger) by default, in both layouts.
static void MenuScroll() {
    namespace dk = tbd::gui::dock;
    auto offset = [](float dir, float scroll) { return dir * scroll; }; // where the tiles sit (State::scrollDir)
    for (float dir : {1.0f, -1.0f}) { // +1: the Phone UI's right column (rows from the bottom); -1: the PC UI panel
        const float from = 100.0f, dy = 30.0f;
        const float to = dk::DragScrollTo(from, dy, dir, false, 500.0f);
        CHECK(Near(offset(dir, to) - offset(dir, from), dy));
        const float back = dk::DragScrollTo(from, -dy, dir, false, 500.0f);
        CHECK(Near(offset(dir, back) - offset(dir, from), -dy));
        const float inv = dk::DragScrollTo(from, dy, dir, true, 500.0f); // Invert menu scroll
        CHECK(Near(offset(dir, inv) - offset(dir, from), -dy));
        const float wheel = dk::WheelScrollTo(from, 1.0f, dir, false, 500.0f); // a notch up: the tiles come 40 pt down
        CHECK(Near(offset(dir, wheel) - offset(dir, from), 40.0f));
    }
    CHECK(Near(dk::DragScrollTo(0.0f, 30.0f, -1.0f, false, 500.0f), 0.0f));   // clamped at the ends
    CHECK(Near(dk::DragScrollTo(490.0f, 30.0f, 1.0f, false, 500.0f), 500.0f));
    CHECK(Near(dk::DragScrollTo(100.0f, 30.0f, -1.0f, false, 500.0f), 70.0f)); // the PC UI: exactly the old dragScroll - d
    dk::State st;
    CHECK(Near(st.scrollDir, -1.0f)); // until a list says otherwise: the old direction
}

// The page: every table row on it once, under its heading; the bound values read back into the settings.
static void Page() {
    ctl::Settings s;
    bool edgePan = true;
    tbd::gui::ControlsPage p;
    tbd::gui::BuildControlsSpec(p, s, true, &edgePan);
    int n = 0, onPage = 0;
    const ctl::Desc* t = ctl::Table(n);
    for (int i = 0; i < n; ++i) onPage += t[i].page ? 1 : 0;
    int sliders = 0, captions = 0, edge = 0;
    for (size_t r = 0; r < p.spec.rows.size(); ++r) {
        if (p.spec.rows[r].kind == tbd::gui::SimRow::Caption) { ++captions; CHECK(p.rowDesc[r] == -1); }
        if (p.spec.rows[r].kind == tbd::gui::SimRow::Slider) ++sliders;
        if (p.rowDesc[r] == -2) ++edge;
    }
    CHECK(captions == ctl::kGroups && sliders == onPage + 1 && edge == 1);
    CHECK(p.spec.rows[0].label == "TOUCH - CAMERA");
    tbd::gui::BuildControlsSpec(p, s, false, nullptr);
    CHECK(p.spec.rows[0].label == "MOUSE");
    // nothing moved: nothing changes
    tbd::gui::BuildControlsSpec(p, s, true, &edgePan);
    CHECK(!tbd::gui::ReadControlsSpec(p, s, &edgePan) && ctl::IsDefault(s));
    // a slider and a switch moved (as DrawSimPanel writes *bind)
    for (size_t r = 0; r < p.rowDesc.size(); ++r) {
        const int idx = p.rowDesc[r];
        if (idx >= 0 && std::string(t[idx].key) == "ctl_pan1") *p.spec.rows[r].bind = 2.0f;
        if (idx >= 0 && std::string(t[idx].key) == "ctl_menu_scroll_inv") *p.spec.rows[r].bind = 1.0f;
        if (idx == -2) *p.spec.rows[r].bind = 0.0f;
    }
    bool edgeChanged = false;
    CHECK(tbd::gui::ReadControlsSpec(p, s, &edgePan, &edgeChanged));
    CHECK(Near(s.touchPan1, 2.0f) && s.invertMenuScroll && !edgePan && edgeChanged);
    CHECK(ctl::ValueText(s, t[0]) == "2.00x");
}

int main() {
    Defaults();
    Clamping();
    RoundTrip();
    Gestures();
    Camera();
    MenuScroll();
    Page();
    std::printf("controls_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
