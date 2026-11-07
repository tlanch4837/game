// The city HUD's info overlays (sim gameplay pass, phase C): a column of toggles at the right edge and the legend of the
// one that is on. Platform free; the host (src/city_overlays.inl) builds and draws the tint itself.
//   Power, Water, Sewage   per building: roofs green when served, red when not; the ground per sim cell by the share served
//   Land value             per sim cell, low (dark) .. high (bright)
//   Traffic                per road segment, from the flow solve: free, busy, congested, jammed
//   Coverage               per sim cell, the mean of fire, police, health and school coverage
// (No pollution layer: the sim has no pollution data yet.)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "road_toolbar.h"

#include "gui.h"

namespace tbd {
namespace gui {

enum class Overlay : int { None = -1, Power, Water, Sewage, LandValue, Traffic, Coverage, Count };

inline const char* OverlayName(Overlay o) {
    static const char* const k[] = {"Electricity", "Water", "Sewage", "Land value", "Traffic", "Service coverage"};
    return o == Overlay::None ? "" : k[(int)o];
}
inline Icon OverlayIcon(Overlay o) {
    static const Icon k[] = {Icon::Bolt, Icon::Drop, Icon::Drain, Icon::Economy, Icon::Roads, Icon::Health};
    return o == Overlay::None ? Icon::None : k[(int)o];
}

// The colour ramp of an overlay at t in [0, 1] (0 bad / low, 1 good / high). Traffic: 0 free .. 1 jammed.
struct Rgb { float r, g, b; };
inline Rgb OverlayRamp(Overlay o, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [](Rgb a, Rgb b, float k) { return Rgb{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
    const Rgb red{0.86f, 0.22f, 0.18f}, yellow{0.95f, 0.78f, 0.20f}, green{0.25f, 0.78f, 0.35f};
    switch (o) {
    case Overlay::Traffic: return t < 0.5f ? mix(green, yellow, t * 2) : mix(yellow, red, (t - 0.5f) * 2);
    case Overlay::LandValue: return t < 0.5f ? mix(Rgb{0.20f, 0.16f, 0.30f}, Rgb{0.30f, 0.55f, 0.80f}, t * 2) : mix(Rgb{0.30f, 0.55f, 0.80f}, Rgb{0.95f, 0.95f, 0.55f}, (t - 0.5f) * 2);
    case Overlay::Coverage: return t < 0.5f ? mix(Rgb{0.25f, 0.25f, 0.28f}, Rgb{0.20f, 0.55f, 0.65f}, t * 2) : mix(Rgb{0.20f, 0.55f, 0.65f}, Rgb{0.35f, 0.90f, 0.75f}, (t - 0.5f) * 2);
    default: return t < 0.5f ? mix(red, yellow, t * 2) : mix(yellow, green, (t - 0.5f) * 2);
    }
}

struct OverlayHudState {
    Overlay on = Overlay::None;
};

// The toggles (right edge, under the HUD's side icons) and, while one is on, its legend beside them. `top`: unscaled
// distance from the top edge.
inline void OverlayHudFrame(Context& g, OverlayHudState& st, float top) {
    using namespace road_ui;
    const Theme& th = g.Style();
    const float s = g.Scale();
    static Tip tip;
    tip.on = false;
    g.BeginPanel("##overlays", Anchor::TopRight, 8.0f, top, 56.0f, 0.0f, 6.0f);
    {
        const float size = std::round(40.0f * s);
        if (IconBox(g, tip, "##ovtitle", g.Reserve(size, size), Icon::Overlay, st.on != Overlay::None, "Info views (click again to hide)") && st.on != Overlay::None)
            st.on = Overlay::None;
        for (int i = 0; i < (int)Overlay::Count; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##ov%d", i);
            const Overlay o = (Overlay)i;
            if (IconBox(g, tip, id, g.Reserve(size, size), OverlayIcon(o), st.on == o, OverlayName(o))) st.on = st.on == o ? Overlay::None : o;
        }
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    if (st.on == Overlay::None) return;
    g.BeginPanel("##overlay legend", Anchor::TopRight, 72.0f, top, 250.0f, 0.0f, 10.0f);
    g.Text(TextStyle::Bright, "%s", OverlayName(st.on));
    const Rect bar = g.Reserve(std::round(14.0f * s), 0.0f);
    DrawList& dl = g.Draw();
    const int steps = 24;
    for (int i = 0; i < steps; ++i) {
        const Rgb c = OverlayRamp(st.on, (i + 0.5f) / steps);
        const Color col = Rgba((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255));
        dl.Rect(bar.x + bar.w * i / steps, bar.y, bar.x + bar.w * (i + 1) / steps + 0.5f, bar.y + bar.h, col, col, 0.0f);
    }
    dl.Rect(bar.x, bar.y, bar.x + bar.w, bar.y + bar.h, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0), 2.0f, th.border, th.panelBorder);
    switch (st.on) {
    case Overlay::Power: case Overlay::Water: case Overlay::Sewage:
        g.Text(TextStyle::Muted, "Red: no supply   Green: served");
        g.Text(TextStyle::Small, "Roofs: buildings. Ground: share served.");
        break;
    case Overlay::LandValue: g.Text(TextStyle::Muted, "Low  ..  High"); break;
    case Overlay::Traffic: g.Text(TextStyle::Muted, "Free   Busy   Congested   Jammed"); break;
    case Overlay::Coverage: g.Text(TextStyle::Muted, "None  ..  Full");
        g.Text(TextStyle::Small, "Fire, police, health, school"); break;
    default: break;
    }
    g.EndPanel();
}

} // namespace gui
} // namespace tbd
