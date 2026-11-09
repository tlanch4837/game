// The player's control settings (Settings > Controls, both HUDs): touch gestures, the city camera's limits, the mouse and
// the keyboard, and the menus' scroll direction. One struct, one global (tbd::controls::g) that the input code reads at
// the single place each value was a constant before. Every default is today's behaviour, so nothing changes until the
// player moves a slider. Persisted in saves\settings.ini as ctl_* keys (game/control_prefs.inl). Platform free, headless
// (tests\controls_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tbd {
namespace controls {

struct Settings {
    // ---- touch: the camera (multipliers: 1 = today)
    float touchPan1 = 1.0f;      // one-finger pan: how far the ground moves per finger point
    float touchPan2 = 1.0f;      // two-finger pan (the MOVE gesture's centre)
    float pinchZoom = 1.0f;      // pinch zoom: notches per spread
    float twistSpeed = 1.0f;     // twist turns the camera by this times the fingers' turn (0: twist off)
    bool twistSnap = false;      // the camera's heading snaps to kTwistSnapDeg steps while twisting
    float tiltSpeed = 1.0f;      // two fingers vertical: tilt per point
    bool invertTilt = false;     // fingers up = toward top-down (default: toward the horizon)
    bool invertPinch = false;    // spread = farther (default: closer)
    bool touchZoomToPoint = true; // pinch zooms toward the fingers' centre (else the screen centre)
    float touchEdgeSpeed = 1.0f; // the edge pan's speed (900 px/s at the very edge times this)
    // ---- touch: gestures (src/game/touch_gesture.h, src/platform/ios/main_ios.mm)
    float tapSlopPt = 10.0f;     // a finger that travels more than this is a drag (points)
    float tapMaxSec = 0.45f;     // a tap lifts within this; held longer it is a hold / long-press (seconds)
    float twoSlopPt = 12.0f;     // two fingers lock into TILT or MOVE after this much movement (points)
    float dotSnapPt = 28.0f;     // the road aim dot's reach: the snap radius at the dot, a fingertip (points)
    // ---- the city camera (src/city_camera.h)
    float eyeClearance = 2.0f;   // the eye never comes lower than this over the ground / sea (metres)
    float maxDistance = 9000.0f; // the farthest zoom (metres eye to pivot)
    float zoomEase = 14.0f;      // the eased zoom's rate (per second; higher = snappier, 60 is about instant)
    // ---- mouse
    float mousePan = 1.0f;       // middle drag
    float mouseRotate = 1.0f;    // right drag / Alt + left drag, sideways
    float mouseTilt = 1.0f;      // right drag / Alt + left drag, up and down
    float mouseZoom = 1.0f;      // the wheel
    bool mouseInvertRotate = false;
    bool mouseInvertTilt = false;
    bool mouseInvertZoom = false;
    bool mouseZoomToCursor = true; // the wheel zooms toward the ground under the cursor (else the screen centre)
    bool mouseEdge = false;      // the cursor at the window's edge pans the city (off: as before)
    float mouseEdgeSpeed = 1.0f; // edge scroll speed (the keyboard pan's speed times this)
    // ---- keyboard
    float keyPan = 1.0f;         // WASD / arrows
    float keyRotate = 1.0f;      // Q / E
    // ---- menus
    bool invertMenuScroll = false; // the dock's tile lists: content moves against the finger / wheel (default: follows it)
    // ---- the aim dot's look (src/gui/road_reticle.h owns it; kept here for when that file reads them; not on the page yet)
    float dotOffsetY = 0.0f;     // points below the screen centre
    float dotRing = 15.0f;       // the ring's radius (points)
};

constexpr float kTwistSnapDeg = 15.0f;
constexpr float kEdgeBandPx = 8.0f; // the mouse edge scroll's band at the window's edge (pixels)

inline Settings g; // the live values (the input code reads these)

// One row of the table: the ini key, the page's label and group, the field (a float or a bool), its range and step.
enum Group : int { kTouchCam, kTouchGesture, kCamera, kMouse, kKeyboard, kMenus, kGroups };
inline const char* GroupName(int gr) {
    static const char* const k[kGroups] = {"TOUCH - CAMERA", "TOUCH - GESTURES", "CAMERA LIMITS", "MOUSE", "KEYBOARD", "MENUS"};
    return gr >= 0 && gr < kGroups ? k[gr] : "";
}
struct Desc {
    const char* key;
    const char* label;
    int group;
    float Settings::*f; // a slider (null for a toggle)
    bool Settings::*b;  // a toggle (null for a slider)
    float lo, hi, step;
    const char* fmt;    // how the value prints
    bool page = true;   // shown on the Controls page
};

inline const Desc* Table(int& n) {
    using S = Settings;
    static const Desc k[] = {
        {"ctl_pan1", "One-finger pan speed", kTouchCam, &S::touchPan1, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_pan2", "Two-finger pan speed", kTouchCam, &S::touchPan2, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_pinch", "Pinch zoom speed", kTouchCam, &S::pinchZoom, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_pinch_inv", "Invert pinch zoom", kTouchCam, nullptr, &S::invertPinch, 0, 1, 1, ""},
        {"ctl_pinch_point", "Pinch zooms to fingers", kTouchCam, nullptr, &S::touchZoomToPoint, 0, 1, 1, ""},
        {"ctl_twist", "Twist rotate speed", kTouchCam, &S::twistSpeed, nullptr, 0.0f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_twist_snap", "Twist snaps to 15 deg", kTouchCam, nullptr, &S::twistSnap, 0, 1, 1, ""},
        {"ctl_tilt", "Tilt speed (two fingers up / down)", kTouchCam, &S::tiltSpeed, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_tilt_inv", "Invert tilt", kTouchCam, nullptr, &S::invertTilt, 0, 1, 1, ""},
        {"ctl_edge_touch", "Edge pan speed", kTouchCam, &S::touchEdgeSpeed, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_tap_slop", "Tap vs drag distance", kTouchGesture, &S::tapSlopPt, nullptr, 4.0f, 30.0f, 1.0f, "%.0f pt"},
        {"ctl_tap_time", "Tap time (longer = long-press)", kTouchGesture, &S::tapMaxSec, nullptr, 0.15f, 1.5f, 0.05f, "%.2f s"},
        {"ctl_two_slop", "Two-finger lock distance", kTouchGesture, &S::twoSlopPt, nullptr, 4.0f, 40.0f, 1.0f, "%.0f pt"},
        {"ctl_dot_snap", "Aim dot snap reach", kTouchGesture, &S::dotSnapPt, nullptr, 8.0f, 80.0f, 1.0f, "%.0f pt"},
        {"ctl_clearance", "Camera ground clearance", kCamera, &S::eyeClearance, nullptr, 0.5f, 30.0f, 0.5f, "%.1f m"},
        {"ctl_max_dist", "Max zoom distance", kCamera, &S::maxDistance, nullptr, 500.0f, 9000.0f, 250.0f, "%.0f m"},
        {"ctl_zoom_ease", "Zoom smoothing (higher = snappier)", kCamera, &S::zoomEase, nullptr, 4.0f, 60.0f, 1.0f, "%.0f"},
        {"ctl_m_pan", "Pan speed (middle drag)", kMouse, &S::mousePan, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_m_rot", "Rotate speed (right drag)", kMouse, &S::mouseRotate, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_m_tilt", "Tilt speed (right drag)", kMouse, &S::mouseTilt, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_m_zoom", "Wheel zoom speed", kMouse, &S::mouseZoom, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_m_rot_inv", "Invert drag rotate", kMouse, nullptr, &S::mouseInvertRotate, 0, 1, 1, ""},
        {"ctl_m_tilt_inv", "Invert drag tilt", kMouse, nullptr, &S::mouseInvertTilt, 0, 1, 1, ""},
        {"ctl_m_zoom_inv", "Invert wheel zoom", kMouse, nullptr, &S::mouseInvertZoom, 0, 1, 1, ""},
        {"ctl_m_zoom_cur", "Zoom to cursor", kMouse, nullptr, &S::mouseZoomToCursor, 0, 1, 1, ""},
        {"ctl_m_edge", "Edge scroll", kMouse, nullptr, &S::mouseEdge, 0, 1, 1, ""},
        {"ctl_m_edge_speed", "Edge scroll speed", kMouse, &S::mouseEdgeSpeed, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_k_pan", "Keyboard pan speed (WASD)", kKeyboard, &S::keyPan, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_k_rot", "Keyboard rotate speed (Q / E)", kKeyboard, &S::keyRotate, nullptr, 0.25f, 3.0f, 0.05f, "%.2fx"},
        {"ctl_menu_scroll_inv", "Invert menu scroll", kMenus, nullptr, &S::invertMenuScroll, 0, 1, 1, ""},
        {"ctl_dot_offset", "Aim dot offset", kTouchGesture, &S::dotOffsetY, nullptr, -200.0f, 200.0f, 1.0f, "%.0f pt", false},
        {"ctl_dot_ring", "Aim dot size", kTouchGesture, &S::dotRing, nullptr, 8.0f, 40.0f, 1.0f, "%.0f pt", false},
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

// Every slider into its range (a NaN goes to the default).
inline void Clamp(Settings& s) {
    const Settings d;
    int n = 0;
    const Desc* t = Table(n);
    for (int i = 0; i < n; ++i)
        if (t[i].f) {
            float& v = s.*(t[i].f);
            v = std::isfinite(v) ? std::clamp(v, t[i].lo, t[i].hi) : d.*(t[i].f);
        }
}

inline bool IsDefault(const Settings& s) {
    const Settings d;
    int n = 0;
    const Desc* t = Table(n);
    for (int i = 0; i < n; ++i)
        if (t[i].f ? std::fabs(s.*(t[i].f) - d.*(t[i].f)) > 1e-6f : s.*(t[i].b) != d.*(t[i].b)) return false;
    return true;
}

// "key=value" lines; only values that differ from the defaults are written (a file with none is today's behaviour).
inline std::string Serialize(const Settings& s) {
    const Settings d;
    int n = 0;
    const Desc* t = Table(n);
    std::string out;
    char b[64];
    for (int i = 0; i < n; ++i) {
        if (t[i].f) {
            if (std::fabs(s.*(t[i].f) - d.*(t[i].f)) <= 1e-6f) continue;
            std::snprintf(b, sizeof(b), "%s=%g\n", t[i].key, (double)(s.*(t[i].f)));
        } else {
            if (s.*(t[i].b) == d.*(t[i].b)) continue;
            std::snprintf(b, sizeof(b), "%s=%d\n", t[i].key, s.*(t[i].b) ? 1 : 0);
        }
        out += b;
    }
    return out;
}

// Reads the ctl_* keys of a settings.ini text into s (missing keys keep s's value), then clamps. Lines may end in \r.
inline void Parse(const std::string& text, Settings& s) {
    int n = 0;
    const Desc* t = Table(n);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.rfind("ctl_", 0) != 0) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        char* stop = nullptr;
        const float v = std::strtof(val.c_str(), &stop);
        if (stop == val.c_str()) continue; // not a number
        for (int i = 0; i < n; ++i)
            if (key == t[i].key) {
                if (t[i].f) s.*(t[i].f) = v;
                else s.*(t[i].b) = v != 0.0f;
                break;
            }
    }
    Clamp(s);
}

// settings.ini with its ctl_* lines replaced by s's (every other line kept, in order).
inline std::string Merge(const std::string& file, const Settings& s) {
    std::string kept, line;
    for (size_t i = 0; i <= file.size(); ++i) {
        if (i < file.size() && file[i] != '\n') { line += file[i]; continue; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.rfind("ctl_", 0) != 0) kept += line + "\n";
        line.clear();
    }
    return kept + Serialize(s);
}

// ---- helpers the input code uses (one place each)
// The wheel / pinch zoom's notches after the player's speed and invert (fromTouch: a pinch, else the mouse wheel).
inline float ZoomNotches(const Settings& s, float notches, bool fromTouch) {
    return fromTouch ? notches * s.pinchZoom * (s.invertPinch ? -1.0f : 1.0f) : notches * s.mouseZoom * (s.mouseInvertZoom ? -1.0f : 1.0f);
}
// Two fingers' vertical tilt (radians) after speed and invert.
inline float TouchTilt(const Settings& s, float radians) { return radians * s.tiltSpeed * (s.invertTilt ? -1.0f : 1.0f); }
// The heading snapped to kTwistSnapDeg steps (radians).
inline float SnapYaw(float yaw) {
    const float step = kTwistSnapDeg * 3.14159265f / 180.0f;
    return std::round(yaw / step) * step;
}
// The eased zoom's blend this frame (0..1) for dt seconds.
inline float ZoomBlend(const Settings& s, float dt) { return 1.0f - std::exp(-s.zoomEase * dt); }
// The mouse edge scroll's push on one axis: -1 at the near edge, 1 at the far edge, 0 inside the band (pixels).
inline float EdgePush(float v, float extent, float band = kEdgeBandPx) {
    if (extent <= 2.0f * band) return 0.0f;
    if (v < band) return -1.0f;
    if (v > extent - 1.0f - band) return 1.0f;
    return 0.0f;
}
// The page's value text for row i ("1.25x", "On").
inline std::string ValueText(const Settings& s, const Desc& d) {
    if (d.b) return s.*(d.b) ? "On" : "Off";
    char b[32];
    std::snprintf(b, sizeof(b), d.fmt, (double)(s.*(d.f)));
    return b;
}

} // namespace controls
} // namespace tbd
