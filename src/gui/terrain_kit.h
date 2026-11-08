// The terrain tools for the Glass Dock+ GUI on touch: one slim 52 pt strip along the bottom (like the road kit's mini
// mode, gui/road_toolkit.h) so the ground being shaped stays in view:
//   close | Raise, Lower, Level (flatten to where the stroke starts), Smooth | brush size  - 30 m +  | strength  - 6 m/s + | more
// "more" opens a popover row with Save and Load (the terrain edits file). While a stroke is being painted the strip
// shrinks to a small pill (the tool and the brush size), and comes back when the finger lifts. The size and strength
// chips step x1.25 a tap and repeat while held. Every target is 40 pt; the strip keeps inside the safe area.
// Platform free: the host owns the brush (game_core.inl g_terrainEdit) and the file I/O; tests/terrain_kit_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "glass_dock.h"
#include "safe_area.h"
#include "terrain_toolbar.h" // TerrainBrushMode

namespace tbd {
namespace gui {
namespace terrain_kit {

struct Brush {
    int* mode = nullptr;       // TerrainBrushMode as int
    float* radius = nullptr;   // metres, 4 .. 300
    float* strength = nullptr; // metres per second, 1 .. 20
};
struct State {
    SafeInsets safe;     // points, the host sets it every frame
    bool more = false;   // the Save / Load popover
    float shrink = 0;    // 0 the strip .. 1 the painting pill (eased)
    int holdId = -1, repeats = 0;
    float holdT = 0;
    std::string caption;
    float captionT = 99;
};
struct Out {
    bool close = false, save = false, load = false, changed = false;
    std::vector<float> sfx;
};
struct Layout {
    float x = 0, y = 0, w = 0, h = 0; // the strip (or the pill), points
    float top = 0;                    // the highest point it reaches (the popover included)
};

constexpr float kB = 40, kPad = 6, kGap = 4, kH = kB + 2 * kPad, kValueW = 62, kSep = 8;
constexpr float kMinRadius = 4, kMaxRadius = 300, kMinStrength = 1, kMaxStrength = 20;

inline const char* ModeName(int m) {
    static const char* const k[] = {"Raise", "Lower", "Level", "Smooth"};
    return k[std::clamp(m, 0, 3)];
}
inline Icon ModeIcon(int m) {
    static const Icon k[] = {Icon::Raise, Icon::Dig, Icon::Flatten, Icon::Waves};
    return k[std::clamp(m, 0, 3)];
}
// close + 4 tools + 2 chips (- value +) + more, with two separators
inline float StripWidth() { return 2 * kPad + (kB + kGap + kSep) + 4 * (kB + kGap) + kSep + 2 * (2 * (kB + kGap) + kValueW + kGap) + kB; }

// bottom: points from the screen's bottom edge to the strip's bottom (the dock, the safe area). painting: a stroke is on.
inline Layout Frame(Context& g, const dock::Atlas& atlas, const Brush& b, State& st, float bottom, bool painting, Out& out) {
    using namespace dock;
    out = Out{};
    const Input& in = g.In();
    const float s = g.Scale(), W = in.viewW / s, H = in.viewH / s;
    const SafeInsets& sa = st.safe;
    bottom = std::max(bottom, sa.B() + 8);
    st.shrink = std::clamp(st.shrink + (painting ? in.dt : -in.dt) / 0.2f, 0.0f, 1.0f);
    st.captionT += in.dt;
    Layout L;
    L.x = 12 + sa.L(), L.h = kH, L.y = H - bottom - kH;
    L.w = std::min(StripWidth(), W - 24 - sa.L() - sa.R());
    L.top = L.y;
    if (!b.mode || !b.radius || !b.strength) return L;
    Painter P(g, atlas, s);
    DrawList& dl = P.dl;
    auto say = [&](const std::string& c) { st.caption = c, st.captionT = 0; };
    auto sfx = [&](float p) { out.sfx.push_back(p); };
    auto tap = [&](float x, float y, float w, float h, bool* held) {
        const Rect r{x * s, y * s, w * s, h * s};
        const bool inside = r.Contains(in.mouseX, in.mouseY), pressedIn = r.Contains(in.pressX, in.pressY);
        if (held) *held = in.down && pressedIn && inside;
        return in.released && pressedIn && inside;
    };
    auto box = [&](float x, float y, float w, float h, bool on, bool held) {
        P.Box(x, y, w, h, 10, on ? kAccent : held ? Rgba(255, 255, 255, 64) : kChip, on ? 0.0f : 1.0f, on ? 0u : Rgba(255, 255, 255, 26));
    };
    auto icon = [&](Icon ic, float x, float y, float w, float h, Color c = kInk) {
        DrawIcon(dl, ic, (x + w * 0.5f) * s, (y + h * 0.5f) * s, std::round(std::min(w, h) * 0.5f * s), std::max(1.0f, 1.9f * s), c);
    };
    auto button = [&](float x, float y, Icon ic, bool on) {
        bool held = false;
        const bool hit = tap(x, y, kB, kB, &held);
        box(x, y, kB, kB, on, held);
        icon(ic, x, y, kB, kB);
        return hit;
    };
    // one step on the tap, then a step every .12 s after .45 s held
    auto repeat = [&](int id, float x, float y, Icon ic, bool dim) {
        bool held = false;
        const bool hit = tap(x, y, kB, kB, &held);
        box(x, y, kB, kB, false, held);
        icon(ic, x, y, kB, kB, dim ? Fade(kInk, 0.4f) : kInk);
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
    char v[48];

    // ---- painting: the pill (the tool and its size), nothing to tap
    if (st.shrink > 0.5f) {
        std::snprintf(v, sizeof(v), "%.0f m", *b.radius);
        const float pw = kPad + kB + 4 + P.TextW(v, 13, true) + 12;
        L.w = pw;
        P.alpha = 0.85f;
        P.Glass(L.x, L.y, pw, kH, kH * 0.5f, kPanel, 22, 1.4f, true);
        icon(ModeIcon(*b.mode), L.x + kPad, L.y + kPad, kB, kB);
        P.Text(v, L.x + kPad + kB + 4, L.y + kH * 0.5f, 13, kInk, true);
        P.alpha = 1;
        return L;
    }

    P.Glass(L.x, L.y, L.w, L.h, 16, kPanel, 22, 1.6f, true);
    const float y = L.y + kPad;
    float x = L.x + kPad;
    if (button(x, y, Icon::Close, false)) out.close = true, sfx(0.65f);
    x += kB + kGap + kSep;
    static const char* const kTips[] = {"Raise: drag to lift the ground", "Lower: drag to dig", "Level: drag to flatten to where the stroke starts",
                                        "Smooth: drag to soften bumps and ridges"};
    for (int m = 0; m < 4; ++m) {
        if (button(x, y, ModeIcon(m), *b.mode == m) && *b.mode != m) *b.mode = m, sfx(1.2f), out.changed = true, say(kTips[m]);
        x += kB + kGap;
    }
    x += kSep;
    // a chip: - value +, x1.25 a step
    auto chip = [&](int id, float* value, float lo, float hi, const char* fmt, const char* what) {
        const int dn = repeat(id, x, y, Icon::ChevronDown, *value <= lo + 1e-3f);
        x += kB + kGap;
        P.Box(x, y, kValueW, kB, 10, Rgba(0, 0, 0, 70), 1, Rgba(255, 255, 255, 26));
        std::snprintf(v, sizeof(v), fmt, *value);
        P.Text(v, x + kValueW * 0.5f, y + kB * 0.5f - 6, 13, kInk, true, 0.5f, kValueW - 4);
        P.Text(what, x + kValueW * 0.5f, y + kB * 0.5f + 10, 9, kMuted, false, 0.5f, kValueW - 4);
        x += kValueW + kGap;
        const int up = repeat(id + 1, x, y, Icon::ChevronUp, *value >= hi - 1e-3f);
        x += kB + kGap;
        for (int i = 0; i < dn; ++i) *value = std::max(lo, *value / 1.25f);
        for (int i = 0; i < up; ++i) *value = std::min(hi, *value * 1.25f);
        if (dn || up) {
            sfx(dn ? 0.9f : 1.1f), out.changed = true;
            char c[64];
            std::snprintf(c, sizeof(c), "%s ", what);
            std::snprintf(c + std::strlen(c), sizeof(c) - std::strlen(c), fmt, *value);
            say(c);
        }
    };
    chip(10, b.radius, kMinRadius, kMaxRadius, "%.0f m", "size");
    chip(20, b.strength, kMinStrength, kMaxStrength, "%.1f m/s", "strength");
    if (button(x, y, Icon::Gear, st.more)) st.more = !st.more, sfx(1.0f);
    const float moreX = x;
    // ---- more: Save / Load
    if (st.more) {
        const float pw = 2 * kPad + 2 * 76 + kGap, px = std::min(moreX - kPad, W - 12 - sa.R() - pw), py = L.y - 8 - kH;
        P.Glass(px, py, pw, kH, 16, kPanel, 22, 1.6f, true);
        L.top = py;
        const char* const names[2] = {"Save", "Load"};
        for (int i = 0; i < 2; ++i) {
            const float bx = px + kPad + i * (76 + kGap);
            bool held = false;
            const bool hit = tap(bx, py + kPad, 76, kB, &held);
            box(bx, py + kPad, 76, kB, false, held);
            icon(i == 0 ? Icon::Save : Icon::Load, bx, py + kPad, kB, kB);
            P.Text(names[i], bx + kB, py + kPad + kB * 0.5f, 12, kInk, true);
            if (hit) {
                (i == 0 ? out.save : out.load) = true, st.more = false, sfx(1.1f);
                say(i == 0 ? "Terrain edits saved" : "Terrain edits loaded");
            }
        }
    }
    // ---- the caption pill
    if (st.captionT < 3.0f && !st.caption.empty()) {
        const float cw = std::min(P.TextW(st.caption, 11) + 20, W - 24 - sa.L() - sa.R()), ch = 24, cy = L.top - 6 - ch;
        P.Box(L.x, cy, cw, ch, 12, Rgba(20, 22, 28, 200));
        P.Text(st.caption, L.x + 10, cy + ch * 0.5f, 11, Fade(kInk, std::min(1.0f, (3.0f - st.captionT) * 2.0f)), false, 0, cw - 20);
    }
    if (!in.down) st.holdId = -1;
    return L;
}

} // namespace terrain_kit
} // namespace gui
} // namespace tbd
