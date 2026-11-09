// Settings > Controls: every value of game/control_settings.h as a slider or an On / Off row under its heading, the value
// printed at the right, a Reset chip in the header (everything back to the game's own feel) and the close button (back).
// Drawn by both HUDs through the sim panels' scrolling card (sim_panels.h DrawSimPanel: 44 pt rows, drag to scroll):
// Glass Dock+ as a sheet inside the pause menu's Settings (glass_dock.h MenuFrame), the CS2 HUD as a card from Options
// (city_hud.h). Values apply live (the input code reads tbd::controls::g); the host saves when a drag ends.
// Platform free, headless (tests\controls_test.cpp).
#pragma once
#include <string>
#include <vector>

#include "gui.h"
#include "sim_panels.h"
#include "../game/control_settings.h"

namespace tbd {
namespace gui {

struct ControlsPage {
    SimPanelState st;
    SimPanelSpec spec;
    std::vector<int> rowDesc; // per spec row: the table index, -1 a heading, -2 the Edge pan switch
    std::vector<float> vals;  // the rows' bound values (sliders write them)
    bool dirty = false;       // changed since the last save
};
struct ControlsPageOut {
    bool changed = false;     // a value moved this frame (already in the settings)
    bool edgePanChanged = false; // the Edge pan switch moved (the host keeps it as settings.ini edgepan=)
    bool save = false;        // write settings.ini now (a drag ended, a switch flipped, Reset)
    bool closed = false;      // the close button: back to Settings
    bool reset = false;
    Rect frame;               // pixels
};

// The rows: the touch groups first on a touch screen, the mouse and keyboard first on the PC. edgePan: the existing
// Settings > Edge pan switch (shown under the touch camera group), or null to leave it out.
inline void BuildControlsSpec(ControlsPage& p, const controls::Settings& c, bool touchFirst, const bool* edgePan) {
    int n = 0;
    const controls::Desc* t = controls::Table(n);
    p.spec.Clear();
    p.spec.title = "Controls";
    p.spec.sub = "Changes apply at once";
    p.spec.icon = "gear";
    p.rowDesc.clear();
    p.vals.assign((size_t)n + 1, 0.0f); // never reallocated below: the rows bind into it
    static const int kTouchOrder[] = {controls::kTouchCam, controls::kTouchGesture, controls::kCamera, controls::kMouse, controls::kKeyboard, controls::kMenus};
    static const int kPcOrder[] = {controls::kMouse, controls::kKeyboard, controls::kCamera, controls::kTouchCam, controls::kTouchGesture, controls::kMenus};
    const int* order = touchFirst ? kTouchOrder : kPcOrder;
    for (int gi = 0; gi < controls::kGroups; ++gi) {
        const int gr = order[gi];
        p.spec.Caption(controls::GroupName(gr));
        p.rowDesc.push_back(-1);
        for (int i = 0; i < n; ++i) {
            const controls::Desc& d = t[i];
            if (d.group != gr || !d.page) continue;
            float& v = p.vals[(size_t)i];
            v = d.f ? c.*(d.f) : (c.*(d.b) ? 1.0f : 0.0f);
            SimPanelRow& r = p.spec.Slider(d.label, &v, d.lo, d.hi, d.step, d.f ? d.fmt : "%.0f");
            r.value = controls::ValueText(c, d);
            p.rowDesc.push_back(i);
        }
        if (gr == controls::kTouchCam && edgePan) {
            float& v = p.vals[(size_t)n];
            v = *edgePan ? 1.0f : 0.0f;
            SimPanelRow& r = p.spec.Slider("Edge pan (finger at the edge)", &v, 0.0f, 1.0f, 1.0f);
            r.value = *edgePan ? "On" : "Off";
            p.rowDesc.push_back(-2);
        }
    }
}

// The bound values back into the settings. True when anything changed.
inline bool ReadControlsSpec(ControlsPage& p, controls::Settings& c, bool* edgePan, bool* edgeChanged = nullptr) {
    int n = 0;
    const controls::Desc* t = controls::Table(n);
    bool any = false;
    for (int idx : p.rowDesc) {
        if (idx == -2 && edgePan) {
            const bool on = p.vals[(size_t)n] > 0.5f;
            if (on != *edgePan) {
                *edgePan = on, any = true;
                if (edgeChanged) *edgeChanged = true;
            }
        } else if (idx >= 0) {
            const controls::Desc& d = t[idx];
            const float v = p.vals[(size_t)idx];
            if (d.f && v != c.*(d.f)) c.*(d.f) = v, any = true;
            if (d.b && (v > 0.5f) != c.*(d.b)) c.*(d.b) = v > 0.5f, any = true;
        }
    }
    if (any) controls::Clamp(c);
    return any;
}

// One frame of the page. place: Sheet (Glass Dock+) or Side (the CS2 HUD); insets in points (sideInset: the safe area's
// wider side strip, the sheet keeps clear of both sides).
inline ControlsPageOut ControlsPageFrame(Context& g, ControlsPage& p, controls::Settings& c, bool* edgePan, SimPanelPlace place, float bottomInset,
                                         float topInset, float textScale, float sideInset, bool touchFirst) {
    ControlsPageOut out;
    const Input& in = g.In();
    const float s = std::max(g.Scale(), 0.01f);
    BuildControlsSpec(p, c, touchFirst, edgePan);
    SimPanelMetrics m;
    m.textScale = textScale;
    m.sheetMaxFrac = 0.9f;
    m.sheetMaxW = std::max(240.0f, std::min(560.0f, in.viewW / s - 2.0f * sideInset - 24.0f));
    const SimPanelOut o = DrawSimPanel(g, p.spec, p.st, place, bottomInset, topInset, m);
    out.frame = o.frame;
    out.closed = o.closed;
    if (ReadControlsSpec(p, c, edgePan, &out.edgePanChanged)) out.changed = true, p.dirty = true;
    // the Reset chip, left of the close button (a 44 pt target)
    {
        DrawList& dl = g.Draw();
        const float fx = o.frame.x / s, fy = o.frame.y / s, fw = o.frame.w / s;
        const float w = 72.0f, h = 44.0f, x = fx + fw - 46.0f - w, y = fy + 2.0f;
        auto inR = [&](float px, float py) { return px >= x * s && py >= y * s && px < (x + w) * s && py < (y + h) * s; };
        const bool held = in.down && inR(in.pressX, in.pressY) && inR(in.mouseX, in.mouseY);
        if (in.released && inR(in.pressX, in.pressY) && inR(in.mouseX, in.mouseY)) {
            c = controls::Settings{};
            if (edgePan) *edgePan = true, out.edgePanChanged = true;
            out.reset = out.changed = true, p.dirty = true;
        }
        const bool atDefault = controls::IsDefault(c) && (!edgePan || *edgePan);
        const Color fill = held ? Rgba(255, 255, 255, 70) : atDefault ? Rgba(255, 255, 255, 20) : Rgba(10, 132, 255, 150);
        dl.Rect((x + 4.0f) * s, (y + 9.0f) * s, (x + w - 4.0f) * s, (y + h - 9.0f) * s, fill, fill, 13.0f * s);
        g.TextIn({x * s, y * s, w * s, h * s}, "Reset", atDefault ? Rgba(255, 255, 255, 140) : Rgba(255, 255, 255), 0.5f, 12.0f * textScale * s, true);
    }
    if (p.dirty && (!in.down || out.reset)) out.save = true, p.dirty = false; // a drag saves once, when it ends
    return out;
}

} // namespace gui
} // namespace tbd
