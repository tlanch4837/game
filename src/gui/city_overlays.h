// The city HUD's info overlays (sim gameplay pass, phase C): a column of toggles at the right edge and the legend of the
// one that is on. Platform free; the host (src/city_overlays.inl) builds and draws the tint itself.
//   Power, Water, Sewage   per building: roofs green when served, red when not; the ground per sim cell by the share served
//   Land value             per sim cell, low (dark) .. high (bright)
//   Traffic                per road segment, from the flow solve: free, busy, congested, jammed
//   Coverage               per sim cell, the mean of fire, police, health and school coverage
//   the planned layers (CS2 sim roadmap 3.5, Phase 0; frozen interface): air, noise, ground and water pollution, crime,
//                          health, education, garbage, telecom, transit ridership, goods, districts. Each is a per-cell
//                          scalar that its phase supplies by RegisterOverlayProvider. A layer with no provider does
//                          nothing visible: no toggle, no legend, no tint (OverlayAvailable).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "road_toolbar.h"

#include "gui.h"

namespace tbd {
namespace gui {

enum class Overlay : int {
    None = -1,
    Power, Water, Sewage, LandValue, Traffic, Coverage, // built in (src/city_overlays.inl)
    AirPollution, NoisePollution, GroundPollution, WaterPollution, // P3
    Crime, Health, Education, Garbage, Telecom, // P4
    TransitRidership, // P5
    Goods,            // P6
    Districts,        // P7
    Count,
    FirstProvided = AirPollution,
};
constexpr int kOverlayCount = (int)Overlay::Count;

inline bool OverlayValid(Overlay o) { return (int)o >= 0 && (int)o < kOverlayCount; }
inline const char* OverlayName(Overlay o) {
    static const char* const k[kOverlayCount] = {"Electricity", "Water", "Sewage", "Land value", "Traffic", "Service coverage",
                                                 "Air pollution", "Noise pollution", "Ground pollution", "Water pollution",
                                                 "Crime", "Healthcare", "Education", "Garbage", "Telecom", "Transit ridership",
                                                 "Goods", "Districts"};
    return OverlayValid(o) ? k[(int)o] : "";
}
inline Icon OverlayIcon(Overlay o) {
    static const Icon k[kOverlayCount] = {Icon::Bolt, Icon::Drop, Icon::Drain, Icon::Economy, Icon::Roads, Icon::Health,
                                          Icon::Overlay, Icon::Overlay, Icon::Overlay, Icon::Drop, Icon::Police, Icon::Health,
                                          Icon::Education, Icon::Drain, Icon::Comms, Icon::Transport, Icon::Cart, Icon::MapTiles};
    return OverlayValid(o) ? k[(int)o] : Icon::None;
}
// The Glass Dock+ atlas name of each layer's icon (src/game/glass_dock_host.inl's Views group).
inline const char* OverlayAtlasIcon(Overlay o) {
    static const char* const k[kOverlayCount] = {"power", "water", "sewage", "landvalue", "traffic", "health",
                                                 "pollution", "noise", "groundwater", "water", "police", "health",
                                                 "school", "trash", "comms", "bus", "cargo", "district"};
    return OverlayValid(o) ? k[(int)o] : "";
}
// High values are bad (pollution, crime, garbage): the ramp runs green .. red.
inline bool OverlayHighIsBad(Overlay o) {
    return o == Overlay::AirPollution || o == Overlay::NoisePollution || o == Overlay::GroundPollution || o == Overlay::WaterPollution ||
           o == Overlay::Crime || o == Overlay::Garbage;
}

// ---------------------------------------------------------------- per-cell scalar providers (one per planned layer)
// fn(user, cell, x, z): the value of sim cell `cell` (the host's CitySim cell index) centred on world (x, z) metres, in
// [0, 1]; < 0 leaves the cell out. Called at most twice a second per cell while the layer is on, on the main thread.
// legend: "Low  ..  High" style text under the ramp (may be null). Registering null unregisters.
using OverlayCellFn = float (*)(void* user, int cell, float x, float z);
struct OverlayProvider {
    OverlayCellFn fn = nullptr;
    void* user = nullptr;
    const char* legend = nullptr;
};
inline OverlayProvider* OverlayProviders() {
    static OverlayProvider k[kOverlayCount];
    return k;
}
inline uint32_t& OverlayProvidersVersion() { static uint32_t v = 0; return v; }
// Built-in layers cannot be replaced (false).
inline bool RegisterOverlayProvider(Overlay o, OverlayCellFn fn, void* user = nullptr, const char* legend = nullptr) {
    if (!OverlayValid(o) || (int)o < (int)Overlay::FirstProvided) return false;
    OverlayProviders()[(int)o] = {fn, user, legend};
    ++OverlayProvidersVersion();
    return true;
}
inline const OverlayProvider* FindOverlayProvider(Overlay o) {
    return OverlayValid(o) && OverlayProviders()[(int)o].fn ? &OverlayProviders()[(int)o] : nullptr;
}
// A layer is shown (toggle, legend, tint) only when built in or provided.
inline bool OverlayAvailable(Overlay o) {
    return OverlayValid(o) && ((int)o < (int)Overlay::FirstProvided || FindOverlayProvider(o) != nullptr);
}
// The value of a provided layer at a cell, or -1 (no provider, or the provider left it out).
inline float OverlayProvidedValue(Overlay o, int cell, float x, float z) {
    const OverlayProvider* p = FindOverlayProvider(o);
    if (!p) return -1.0f;
    const float v = p->fn(p->user, cell, x, z);
    return v < 0.0f || v != v ? -1.0f : std::min(v, 1.0f);
}

// The colour ramp of an overlay at t in [0, 1] (0 bad / low, 1 good / high). Traffic: 0 free .. 1 jammed. A high-is-bad
// layer (OverlayHighIsBad) runs green .. red; Districts is categorical: t picks a hue.
struct Rgb { float r, g, b; };
inline Rgb OverlayRamp(Overlay o, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [](Rgb a, Rgb b, float k) { return Rgb{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
    const Rgb red{0.86f, 0.22f, 0.18f}, yellow{0.95f, 0.78f, 0.20f}, green{0.25f, 0.78f, 0.35f};
    switch (o) {
    case Overlay::Traffic: return t < 0.5f ? mix(green, yellow, t * 2) : mix(yellow, red, (t - 0.5f) * 2);
    case Overlay::LandValue: return t < 0.5f ? mix(Rgb{0.20f, 0.16f, 0.30f}, Rgb{0.30f, 0.55f, 0.80f}, t * 2) : mix(Rgb{0.30f, 0.55f, 0.80f}, Rgb{0.95f, 0.95f, 0.55f}, (t - 0.5f) * 2);
    case Overlay::Coverage: return t < 0.5f ? mix(Rgb{0.25f, 0.25f, 0.28f}, Rgb{0.20f, 0.55f, 0.65f}, t * 2) : mix(Rgb{0.20f, 0.55f, 0.65f}, Rgb{0.35f, 0.90f, 0.75f}, (t - 0.5f) * 2);
    case Overlay::Districts: { // a hue wheel at 70 % saturation
        const float h = t * 6.0f, f = h - std::floor(h), lo = 0.25f, hi = 0.85f, up = lo + (hi - lo) * f, dn = hi - (hi - lo) * f;
        switch ((int)h % 6) { case 0: return {hi, up, lo}; case 1: return {dn, hi, lo}; case 2: return {lo, hi, up};
                              case 3: return {lo, dn, hi}; case 4: return {up, lo, hi}; default: return {hi, lo, dn}; }
    }
    default:
        if (OverlayHighIsBad(o)) t = 1.0f - t;
        return t < 0.5f ? mix(red, yellow, t * 2) : mix(yellow, green, (t - 0.5f) * 2);
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
            if (!OverlayAvailable((Overlay)i)) continue; // a planned layer whose phase has not registered: no toggle
            char id[16];
            std::snprintf(id, sizeof(id), "##ov%d", i);
            const Overlay o = (Overlay)i;
            if (IconBox(g, tip, id, g.Reserve(size, size), OverlayIcon(o), st.on == o, OverlayName(o))) st.on = st.on == o ? Overlay::None : o;
        }
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    if (!OverlayAvailable(st.on)) return; // None, or a planned layer with no provider: nothing visible
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
    default: {
        const OverlayProvider* p = FindOverlayProvider(st.on);
        g.Text(TextStyle::Muted, "%s", p && p->legend ? p->legend : OverlayHighIsBad(st.on) ? "Low  ..  High (bad)" : "Low  ..  High");
        break;
    }
    }
    g.EndPanel();
}

} // namespace gui
} // namespace tbd
