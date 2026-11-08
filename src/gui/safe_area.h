// The screen's safe area: the strips along its edges the GUI keeps clear (iOS: the Dynamic Island / notch, the rounded
// display corners, the home indicator; landscape iPad has them left, right and top). One place for every anchored
// element (glass_dock.h, road_toolkit.h, road_reticle.h, terrain_kit.h, the news pill): they add these to their edge
// margins. Values are in the GUI's points (pixels / Context::Scale()). Windows has none, unless TBD_GUI_SAFE simulates
// them for screenshots ("top,left,bottom,right" in points, e.g. TBD_GUI_SAFE=24,59,21,59 for a notched phone).
// Platform free (tests/safe_area_test.cpp feeds large insets and checks nothing lands in them).
#pragma once
#include <algorithm>
#include <cstdio>

#include "gui.h"

namespace tbd {
namespace gui {

struct SafeInsets {
    float left = 0, top = 0, right = 0, bottom = 0; // points
    float corner = 0;                               // the display's corner radius, points (0: square corners)

    bool Any() const { return left > 0 || top > 0 || right > 0 || bottom > 0 || corner > 0; }
    // An inset with the rounded corner folded in: a rect this far from both edges clears a corner of radius R
    // (R (1 - 1/sqrt 2) ~ .3 R), so every side keeps at least that much.
    float L() const { return std::max(left, corner * 0.3f); }
    float T() const { return std::max(top, corner * 0.3f); }
    float R() const { return std::max(right, corner * 0.3f); }
    float B() const { return std::max(bottom, corner * 0.3f); }
    // From device pixels (the platform's safe area) at the GUI's pixels-per-point.
    static SafeInsets FromPixels(const float px[5], float scale) {
        const float k = scale > 0 ? 1.0f / scale : 1.0f;
        return {px[0] * k, px[1] * k, px[2] * k, px[3] * k, px[4] * k};
    }
    // "top,left,bottom,right[,corner]" (points): the TBD_GUI_SAFE switch.
    static bool Parse(const char* text, SafeInsets& out) {
        float t = 0, l = 0, b = 0, r = 0, c = 0;
        const int n = text ? std::sscanf(text, "%f,%f,%f,%f,%f", &t, &l, &b, &r, &c) : 0;
        if (n < 4) return false;
        out = {l, t, r, b, n >= 5 ? c : 0.0f};
        return true;
    }
    // The safe rectangle of a W x H (points) screen.
    Rect Area(float W, float H) const { return {L(), T(), W - L() - R(), H - T() - B()}; }
    // Does a rectangle (points) reach into an inset strip?
    bool Intrudes(const Rect& r, float W, float H) const {
        const Rect a = Area(W, H);
        const float eps = 0.01f;
        return r.x < a.x - eps || r.y < a.y - eps || r.x + r.w > a.x + a.w + eps || r.y + r.h > a.y + a.h + eps;
    }
};

} // namespace gui
} // namespace tbd
