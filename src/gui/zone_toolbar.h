// The zoning tool's GUI and the game clock:
//   launcher       a "Zoning" button beside "Roads" (bottom centre) opens it;
//   bottom left    the tool options: Tool (paint brush / paint bucket / box), brush size, Dezone;
//   bottom centre  the picker: one coloured tab per zone category (Residential, Commercial, Office, Industrial), the
//                  picked type's name and a tile per building type the category offers (what painting grows there);
//   clock          bottom centre under everything: the time of day (24 h), pause, and the speed button (> >> >>>).
#pragma once
#include <cstdio>

#include "../city/zoning.h"
#include "road_toolbar.h"

namespace tbd {
namespace gui {

struct ZoneToolState {
    enum Mode : int { Brush, Fill, Box };
    bool active = false;
    city::ZoneCat tab = city::ZoneCat::Residential;
    int type = 0;     // index into city::ZoneTypes()
    int lastType[(int)city::ZoneCat::Count] = {0, 4, 5, 6};
    Mode mode = Box;
    bool erase = false; // Dezone
    float radius = 12.0f;
    uint8_t Paint() const { return erase ? city::kNoZone : (uint8_t)type; }
};

struct GameClock {
    double minutes = 8 * 60; // since day 1, 00:00
    bool paused = false;
    int speed = 0;           // 0 >, 1 >>, 2 >>>
    float Rate() const {     // game minutes per real second
        static const float k[] = {2.0f, 6.0f, 18.0f};
        return paused ? 0.0f : k[speed];
    }
    int Day() const { return (int)(minutes / 1440.0) + 1; }
    int Hour() const { return (int)std::fmod(minutes / 60.0, 24.0); }
    int Minute() const { return (int)std::fmod(minutes, 60.0); }
};

inline Color ZoneColor(int type, float k = 1.0f) {
    const Vec3 c = city::ZoneTypes()[type].color;
    return Rgba((int)(c.x * 255 * k), (int)(c.y * 255 * k), (int)(c.z * 255 * k));
}
inline Color ZoneCatColor(city::ZoneCat c) {
    static const int first[] = {0, 4, 5, 6};
    return ZoneColor(first[(int)c]);
}

// A tab: a tilted coloured square (the zone colour), selected = accent frame.
inline bool ZoneTab(Context& g, road_ui::Tip& tip, const char* id, const Rect& r, Color c, bool on, const char* tipText) {
    const Theme& t = g.Style();
    bool hot = false, held = false;
    const bool clicked = g.ClickArea(id, r, &hot, &held);
    const float onT = g.Ease(id, 3, on ? 1.0f : 0.0f), hT = g.Ease(id, 1, hot ? 1.0f : 0.0f);
    DrawList& dl = g.Draw();
    dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Mix(Mix(t.widgetTop, t.widgetHoverTop, hT), t.accentTop, onT), Mix(Mix(t.widgetBottom, t.widgetHoverBottom, hT), t.accentBottom, onT),
            t.widgetRadius, t.border, Mix(t.widgetBorder, t.accentBorder, onT));
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, h = r.h * 0.26f;
    for (float k = -h; k <= h; k += 0.75f) { // a diamond from horizontal lines
        const float half = h - std::fabs(k);
        dl.Line(cx - half, cy + k, cx + half, cy + k, 1.4f, c);
    }
    if (hot && tipText) {
        tip.on = true;
        tip.x = r.x + r.w * 0.5f, tip.y = r.y - std::round(16.0f * g.Scale());
        std::snprintf(tip.text, sizeof(tip.text), "%s", tipText);
    }
    return clicked;
}

// The launcher (when closed) or the options + picker (when open). Returns true when it opened this frame.
inline bool ZoneToolbar(Context& g, ZoneToolState& z, float lift) {
    using namespace road_ui;
    const float s = g.Scale();
    if (!z.active) {
        return false; // the city HUD's toolbar (city_hud.h) opens it
    }
    const Theme& th = g.Style();
    Tip tip;
    g.BeginPanel("##zone options", Anchor::BottomLeft, 8.0f, lift, 448.0f, 0.0f, 10.0f);
    {
        Row row = BeginRow(g, "Tool", 430);
        static const Icon kIcons[] = {Icon::ZoneBrush, Icon::ZoneFill, Icon::ZoneBox};
        static const char* const kTips[] = {"Paint brush: hold and drag", "Paint bucket: fills the block", "Box: drag a rectangle"};
        for (int i = 0; i < 3; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##zmode%d", i);
            if (IconBox(g, tip, id, row.Slot(i, 3), kIcons[i], (int)z.mode == i, kTips[i])) z.mode = (ZoneToolState::Mode)i;
        }
    }
    {
        Row row = BeginRow(g, "Brush Size", 430);
        const float valueW = std::round(84.0f * s);
        const Rect upR = row.Slot(0, 1);
        const Rect valueR = {upR.x - row.gap - valueW, row.r.y, valueW, row.size};
        const Rect downR = {valueR.x - row.gap - row.size, row.r.y, row.size, row.size};
        if (IconBox(g, tip, "##zr down", downR, Icon::ChevronDown, false, "Smaller", z.mode != ZoneToolState::Brush)) z.radius = std::max(4.0f, z.radius - 4.0f);
        DrawList& dl = g.Draw();
        dl.Rect(valueR.x, valueR.y, valueR.x + valueR.w, valueR.y + valueR.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        char v[24];
        std::snprintf(v, sizeof(v), "%.0f m", z.radius);
        g.TextIn(valueR, v, th.textBright, 0.5f, th.fontSize);
        if (IconBox(g, tip, "##zr up", upR, Icon::ChevronUp, false, "Bigger", z.mode != ZoneToolState::Brush)) z.radius = std::min(64.0f, z.radius + 4.0f);
    }
    {
        Row row = BeginRow(g, "Dezone", 430);
        if (IconBox(g, tip, "##dezone", row.Slot(0, 1), Icon::Bulldoze, z.erase, z.erase ? "Dezone on: painting clears zones" : "Dezone off")) z.erase = !z.erase;
    }
    g.EndPanel();

    const float panelW = std::clamp(g.In().viewW / s - (8.0f + 448.0f + 8.0f + 8.0f), 300.0f, 640.0f);
    g.BeginPanel("##zone picker", Anchor::BottomLeft, 8.0f + 448.0f + 8.0f, lift, panelW, 0.0f, 10.0f);
    {
        const float size = std::round(40.0f * s), gap = std::round(4.0f * s);
        const Rect bar = g.Reserve(size, panelW * s);
        for (int i = 0; i < (int)city::ZoneCat::Count; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##ztab%d", i);
            const Rect r = {std::round(bar.x + i * (size + gap)), bar.y, size, size};
            if (ZoneTab(g, tip, id, r, ZoneCatColor((city::ZoneCat)i), (int)z.tab == i, city::ZoneCatName((city::ZoneCat)i))) {
                z.tab = (city::ZoneCat)i;
                z.type = z.lastType[i];
                z.erase = false;
            }
        }
        const Rect close = {bar.x + bar.w - size, bar.y, size, size};
        if (IconBox(g, tip, "##zclose", close, Icon::Close, false, "Close (Esc)")) z.active = false;
    }
    {
        const Rect r = g.Reserve(std::round(22.0f * s), 0);
        char line[96];
        std::snprintf(line, sizeof(line), "%s%s", z.erase ? "Dezone   " : "", city::ZoneTypes()[z.type].name);
        g.TextIn(r, line, th.textBright, 0.0f, th.fontSize, true);
    }
    {
        const auto& types = city::ZoneTypes();
        const float tw = std::round(104.0f * s), tH = std::round(72.0f * s), gap = std::round(6.0f * s);
        const Rect area = g.Reserve(tH, (panelW - 20.0f) * s);
        int k = 0;
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i].cat != z.tab) continue;
            const Rect r = {std::round(area.x + k * (tw + gap)), area.y, tw, tH};
            ++k;
            char id[16];
            std::snprintf(id, sizeof(id), "##ztype%zu", i);
            bool hot = false, held = false;
            if (g.ClickArea(id, r, &hot, &held)) {
                z.type = (int)i;
                z.lastType[(int)z.tab] = (int)i;
                z.erase = false;
            }
            const bool sel = (int)i == z.type && !z.erase;
            const float selT = g.Ease(id, 3, sel ? 1.0f : 0.0f), hT = g.Ease(id, 1, hot ? 1.0f : 0.0f);
            DrawList& dl = g.Draw();
            dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Mix(Mix(th.widgetTop, th.widgetHoverTop, hT), th.accentTop, selT * 0.7f),
                    Mix(Mix(th.widgetBottom, th.widgetHoverBottom, hT), th.accentBottom, selT * 0.7f), th.widgetRadius, std::max(th.border, selT * 2.0f * s),
                    Mix(th.widgetBorder, Rgba(120, 190, 255), selT));
            // a little block of zone cells in the type's colour
            const float cs = std::round(9.0f * s), cg = std::round(2.0f * s), bx = r.x + (r.w - (4 * cs + 3 * cg)) * 0.5f, by = r.y + std::round(8.0f * s);
            for (int cy = 0; cy < 3; ++cy)
                for (int cx = 0; cx < 4; ++cx)
                    dl.Rect(bx + cx * (cs + cg), by + cy * (cs + cg), bx + cx * (cs + cg) + cs, by + cy * (cs + cg) + cs, ZoneColor((int)i, cy == 0 ? 1.0f : 0.85f), 1.5f * s);
            static const char* const kShort[] = {"Low Density", "Row Housing", "Medium", "High Density", "Commercial", "Office", "Industrial"};
            g.TextIn({r.x, r.y + r.h - std::round(20.0f * s), r.w, std::round(16.0f * s)}, kShort[i], th.text, 0.5f, th.smallSize);
        }
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    return false;
}

// The clock at the bottom centre: "Day 3  14:05", pause, speed. Space pauses (the host binds it).
inline void ClockBar(Context& g, GameClock& c, float lift) {
    using namespace road_ui;
    const Theme& th = g.Style();
    const float s = g.Scale();
    Tip tip;
    g.BeginPanel("##clock", Anchor::BottomCenter, 0.0f, lift, 260.0f, 0.0f, 6.0f);
    const float size = std::round(30.0f * s), gap = std::round(4.0f * s);
    const Rect row = g.Reserve(size, 248.0f * s);
    char t[32];
    std::snprintf(t, sizeof(t), "Day %d   %02d:%02d", c.Day(), c.Hour(), c.Minute());
    g.TextIn({row.x + std::round(6.0f * s), row.y, row.w - 2 * size - gap, size}, t, c.paused ? th.muted : th.textBright, 0.0f, th.fontSize, true);
    const Rect speedR = {row.x + row.w - size, row.y, size, size}, pauseR = {speedR.x - gap - size, row.y, size, size};
    if (IconBox(g, tip, "##pause", pauseR, c.paused ? Icon::Play : Icon::Pause, c.paused, c.paused ? "Resume (Space)" : "Pause (Space)")) c.paused = !c.paused;
    static const Icon kSpeed[] = {Icon::Speed1, Icon::Speed2, Icon::Speed3};
    static const char* const kTip[] = {"Speed: normal", "Speed: fast", "Speed: fastest"};
    if (IconBox(g, tip, "##speed", speedR, kSpeed[c.speed], c.speed > 0, kTip[c.speed])) {
        c.speed = (c.speed + 1) % 3;
        c.paused = false;
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
}

} // namespace gui
} // namespace tbd
