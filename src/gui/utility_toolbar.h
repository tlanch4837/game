// The utilities tool's GUI, laid out like Cities: Skylines II's asset placing panel:
//   launcher       a "Utilities" button left of "Transport" (bottom centre) opens it;
//   bottom left    the tool options: Rotation, Snap to roads, Elevation (for lines), Show Marker, Underground view, Tools (bulldoze);
//   bottom centre  the picker: the networks' state (supply / demand), an info card for the hovered (else the chosen) item,
//                  the tabs (Electricity, Water & Sewage) with a close button, and the tiles: the drag-and-drop source
//                  buildings first, then the lines the player can lay (electricity line, water pipe, sewage pipe).
// Edits UtilToolState directly. No platform headers (gui_test.cpp style hosts can drive it headless).
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../city/utilities.h"
#include "transport_toolbar.h"

namespace tbd {
namespace gui {

struct UtilToolState {
    bool active = false;
    city::UTab tab = city::UTab::Electricity;
    int def = -1;          // the chosen source building (index into city::UtilDefs()); -1: none
    int line = -1;         // the chosen line: a city::Util kind (0 electricity line, 1 water pipe, 2 sewage pipe); -1: none
    float rotation = 0;
    bool snapRoad = true;  // buildings jump beside the nearest road and face it
    bool marker = true;    // lot outline
    bool underground = false; // the underground view: the pipes and cables under the roads
    bool bulldoze = false;
    float elevation = 0;      // electricity line: metres over the ground (0 wooden poles, above that pylons, below that buried)
    float pipeElevation = -10; // water / sewage pipes: always buried

    static constexpr float kStep = 10.0f;
    bool Laying() const { return line >= 0; }
    float LineElevation() const { return line == (int)city::Util::Electricity ? elevation : pipeElevation; }
    void Select(int d) {
        def = d, line = -1, bulldoze = false;
        if (d >= 0) tab = city::UtilDefs()[d].tab;
    }
    void SelectLine(int kind) {
        line = kind, def = -1, bulldoze = false;
        tab = kind == (int)city::Util::Electricity ? city::UTab::Electricity : city::UTab::Water;
    }
    void SelectTab(city::UTab t) {
        tab = t;
        if (def >= 0 && city::UtilDefs()[def].tab != t) def = -1;
        if (line >= 0 && (line == (int)city::Util::Electricity) != (t == city::UTab::Electricity)) line = -1;
    }
    void Raise(int dir) { // Page Up / Down: one step
        if (line == (int)city::Util::Electricity) elevation = std::clamp(elevation + dir * kStep, -30.0f, 60.0f);
        else if (line >= 0) pipeElevation = std::clamp(pipeElevation + dir * kStep, -30.0f, -kStep);
    }
    void Rotate(float deg) { rotation = std::fmod(rotation + deg + 360.0f, 360.0f); }
    // Esc: drop the chosen item first, then close
    void Escape() {
        if (bulldoze) bulldoze = false;
        else if (def >= 0 || line >= 0) def = -1, line = -1;
        else active = false, underground = false;
    }
    void Close() { active = false, def = -1, line = -1, bulldoze = false, underground = false; }
};

namespace util_ui {

inline Color Tint(const Vec3& c, float k = 1.0f) { return Rgba((int)(c.x * 255 * k), (int)(c.y * 255 * k), (int)(c.z * 255 * k)); }
inline Icon KindIcon(city::Util u) { return u == city::Util::Electricity ? Icon::Bolt : u == city::Util::Water ? Icon::Drop : Icon::Drain; }
inline Color KindColor(city::Util u) {
    return u == city::Util::Electricity ? Rgba(255, 214, 90) : u == city::Util::Water ? Rgba(96, 170, 255) : Rgba(176, 140, 96);
}
inline const char* LineName(int kind) { return kind == 0 ? "Electricity Line" : kind == 1 ? "Water Pipe" : "Sewage Pipe"; }
inline const char* LineShort(int kind) { return kind == 0 ? "Power Line" : kind == 1 ? "Water Pipe" : "Sewage Pipe"; }
inline const char* LineDesc(int kind) {
    return kind == 0 ? "Carries electricity where there is no road to run it under. At 0 m it runs on small wooden poles, above ground on steel pylons, "
                       "below ground as a buried cable that only shows in the underground view. Click to start, click again to continue, Esc to stop."
           : kind == 1 ? "Carries water where there is no road to run it under. Always buried: it only shows in the underground view. Click to start, "
                         "click again to continue, Esc to stop."
                       : "Carries sewage away where there is no road to run it under. Always buried: it only shows in the underground view. Click to start, "
                         "click again to continue, Esc to stop.";
}
// "12.3 / 20.1 MW" (supply first: what the sources give, then what the buildings ask for)
inline void Load(char* out, size_t cap, city::Util u, float supply, float demand) {
    if (u == city::Util::Electricity) std::snprintf(out, cap, "%.1f / %.1f MW", supply, demand);
    else std::snprintf(out, cap, "%.0f / %.0f m3/d", supply, demand);
}
inline void Elevation(char* out, size_t cap, float e, bool electricity) {
    const char* how = !electricity ? "buried" : e < -0.01f ? "buried" : e > 0.01f ? "pylons" : "wooden poles";
    std::snprintf(out, cap, "%g m  %s", e, how);
}

// A tile's drawing: the lot's footprint in the source's colour (to scale, front at the bottom) with its glyph.
inline void DrawThumb(DrawList& dl, const Rect& r, const city::UtilDef& d, float scale, bool inCard) {
    const float pad = std::round((inCard ? 6.0f : 5.0f) * scale);
    const float availW = r.w - 2 * pad, availH = r.h - 2 * pad;
    const float aspect = (float)d.w / (float)d.d;
    float w = availW, h = w / aspect;
    if (h > availH) h = availH, w = h * aspect;
    const float x0 = r.x + (r.w - w) * 0.5f, y0 = r.y + (r.h - h) * 0.5f, x1 = x0 + w, y1 = y0 + h;
    const Color body = Tint(d.tint, 0.78f), dark = Tint(d.tint, 0.46f);
    dl.Rect(x0, y0, x1, y1, body, dark, 2.0f * scale, 1.0f * scale, Rgba(250, 251, 252, 150));
    DrawIcon(dl, KindIcon(d.kind), (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, std::min(std::min(w, h) * 0.7f, 26.0f * scale), 1.6f * scale, Rgba(250, 251, 252, 235));
    if (d.backup) dl.Line(x0 + 3 * scale, y1 - 3 * scale, x1 - 3 * scale, y1 - 3 * scale, 1.5f * scale, Rgba(250, 251, 252, 170));
}

} // namespace util_ui

// The launcher (closed) or the options + picker (open). Returns true on the frame it opened.
inline bool UtilityToolbar(Context& g, UtilToolState& t, const city::UtilReport& rep, float lift) {
    using namespace road_ui;
    using namespace transport_ui;
    using namespace util_ui;
    const float s = g.Scale();
    if (!t.active) {
        g.BeginPanel("##utilities launcher", Anchor::BottomCenter, -300.0f, lift, 0.0f, 0.0f, 6.0f);
        g.NextWidth(132);
        const bool open = g.Button("Utilities", Icon::Utilities);
        g.EndPanel();
        if (open) t.active = true;
        return open;
    }
    const Theme& th = g.Style();
    Tip tip;
    const auto& defs = city::UtilDefs();

    // ---------------------------------------------------------------- tool options (bottom left)
    g.BeginPanel("##utility options", Anchor::BottomLeft, 8.0f, lift, 448.0f, 0.0f, 10.0f);
    {
        Row row = BeginRow(g, "Rotation", 430);
        const float valueW = std::round(72.0f * s);
        const Rect plus = row.Slot(0, 1);
        const Rect val = {plus.x - row.gap - valueW, row.r.y, valueW, row.size};
        const Rect minus = {val.x - row.gap - row.size, row.r.y, row.size, row.size};
        const bool locked = t.snapRoad && t.def >= 0; // a building snapped to a road faces it
        if (IconBox(g, tip, "##urot left", minus, Icon::RotateLeft, false, "Rotate left ( , )", locked)) t.Rotate(15.0f);
        DrawList& dl = g.Draw();
        dl.Rect(val.x, val.y, val.x + val.w, val.y + val.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        char v[24];
        if (locked) std::snprintf(v, sizeof(v), "Auto");
        else std::snprintf(v, sizeof(v), "%.0f deg", t.rotation);
        g.TextIn(val, v, locked ? th.muted : th.textBright, 0.5f, th.fontSize);
        if (IconBox(g, tip, "##urot right", plus, Icon::RotateRight, false, "Rotate right ( . )", locked)) t.Rotate(-15.0f);
    }
    {
        Row row = BeginRow(g, "Elevation", 430);
        const bool lines = t.Laying();
        const float valueW = std::round(150.0f * s);
        const Rect up = row.Slot(0, 1);
        const Rect val = {up.x - row.gap - valueW, row.r.y, valueW, row.size};
        const Rect down = {val.x - row.gap - row.size, row.r.y, row.size, row.size};
        if (IconBox(g, tip, "##uelev down", down, Icon::ChevronDown, false, "Lower (Page Down)", !lines)) t.Raise(-1);
        DrawList& dl = g.Draw();
        dl.Rect(val.x, val.y, val.x + val.w, val.y + val.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        char v[40];
        if (lines) Elevation(v, sizeof(v), t.LineElevation(), t.line == (int)city::Util::Electricity);
        else std::snprintf(v, sizeof(v), "lines only");
        const float e = t.LineElevation();
        g.TextIn(val, v, !lines ? th.muted : e < -0.01f ? Rgba(140, 190, 255) : e > 0.01f ? Rgba(255, 214, 120) : th.textBright, 0.5f, th.fontSize);
        if (IconBox(g, tip, "##uelev up", up, Icon::ChevronUp, false, "Raise (Page Up)", !lines)) t.Raise(1);
    }
    {
        Row row = BeginRow(g, "Snapping", 430);
        if (IconBox(g, tip, "##usnap", row.Slot(0, 1), Icon::Roads, t.snapRoad, t.snapRoad ? "Buildings snap beside the nearest road and face it" : "Free placement")) t.snapRoad = !t.snapRoad;
    }
    {
        Row row = BeginRow(g, "Show Marker", 430);
        if (IconBox(g, tip, "##umarker", row.Slot(0, 1), Icon::Marker, t.marker, t.marker ? "Lot outline shown" : "Markers hidden")) t.marker = !t.marker;
    }
    {
        Row row = BeginRow(g, "View", 430);
        if (IconBox(g, tip, "##uunder", row.Slot(0, 1), Icon::Underground, t.underground, t.underground ? "Underground view: the pipes and cables under the roads" : "Underground view off")) t.underground = !t.underground;
    }
    {
        Row row = BeginRow(g, "Tools", 430);
        if (IconBox(g, tip, "##ubulldoze", row.Slot(0, 1), Icon::Bulldoze, t.bulldoze, "Bulldoze a source building or a line")) {
            t.bulldoze = !t.bulldoze;
            if (t.bulldoze) t.def = -1, t.line = -1;
        }
    }
    g.EndPanel();

    // ---------------------------------------------------------------- picker (bottom centre): the networks, the card, tabs, tiles
    const float panelW = std::clamp(g.In().viewW / s - (8.0f + 448.0f + 8.0f + 8.0f), 340.0f, 700.0f);
    g.BeginPanel("##utility picker", Anchor::BottomLeft, 8.0f + 448.0f + 8.0f, lift, panelW, 0.0f, 10.0f);
    static int hovered = -1; // a tile: an index into UtilDefs(), or 1000 + a line kind
    {
        // the three networks: supply / demand, red when short
        const float rowH = std::round(18.0f * s), innerW = (panelW - 20.0f) * s;
        const Rect bar = g.Reserve(rowH, innerW);
        const float colW = bar.w / 3.0f;
        for (int k = 0; k < city::kUtilKinds; ++k) {
            const city::Util u = (city::Util)k;
            const Rect c = {bar.x + colW * k, bar.y, colW, bar.h};
            DrawIcon(g.Draw(), KindIcon(u), c.x + 9.0f * s, c.y + c.h * 0.5f, std::round(13.0f * s), th.iconStroke, KindColor(u));
            char v[48];
            Load(v, sizeof(v), u, rep.supply[k], rep.demand[k]);
            const bool short_ = rep.unserved[k] > 0;
            g.TextIn({c.x + 20.0f * s, c.y, c.w - 20.0f * s, c.h}, v, short_ ? Rgba(255, 120, 110) : th.text, 0.0f, th.smallSize);
        }
    }
    {
        // the card shows the hovered tile, else the chosen one, else the tab's first building
        int show = hovered >= 0 ? hovered : t.line >= 0 ? 1000 + t.line : t.def;
        if (show < 0)
            for (size_t i = 0; i < defs.size(); ++i)
                if (defs[i].tab == t.tab) {
                    show = (int)i;
                    break;
                }
        const float cardH = std::round(168.0f * s), innerW = (panelW - 20.0f) * s;
        const Rect card = g.Reserve(cardH, innerW);
        DrawList& dl = g.Draw();
        dl.Rect(card.x, card.y, card.x + card.w, card.y + card.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        const float pad = std::round(10.0f * s);
        const float thumbW = std::round(96.0f * s), thumbH = std::round(70.0f * s);
        if (show >= 1000) {
            const int kind = show - 1000;
            const city::Util u = (city::Util)kind;
            dl.Rect(card.x + pad, card.y + pad, card.x + pad + thumbW, card.y + pad + thumbH, Rgba(46, 52, 62), Rgba(30, 34, 42), 2.0f * s, 1.0f * s, Rgba(250, 251, 252, 120));
            DrawIcon(dl, kind == 0 ? Icon::Pole : KindIcon(u), card.x + pad + thumbW * 0.5f, card.y + pad + thumbH * 0.5f, 34.0f * s, 2.0f * s, KindColor(u));
            const float tx = card.x + pad * 2 + thumbW, tw = card.x + card.w - pad - tx;
            g.TextIn({tx, card.y + pad - 2.0f * s, tw, std::round(24.0f * s)}, LineName(kind), Rgba(120, 190, 255), 0.0f, th.fontSize + 2.0f, true);
            std::vector<std::string> lines;
            Wrap(g, LineDesc(kind), tw, th.smallSize, lines);
            for (size_t i = 0; i < lines.size() && i < 6; ++i)
                g.TextIn({tx, card.y + pad + std::round((26.0f + 16.0f * i) * s), tw, std::round(16.0f * s)}, lines[i].c_str(), th.text, 0.0f, th.smallSize);
            char l[96];
            std::snprintf(l, sizeof(l), "Per metre: $%d    Elevation: %s", city::UtilLineCostPerMetre(u, t.line == kind ? t.LineElevation() : kind == 0 ? 0.0f : -10.0f),
                          kind == 0 ? "-30 .. 60 m (0 m: wooden poles)" : "buried");
            g.TextIn({card.x + pad, card.y + card.h - std::round(26.0f * s), card.w - 2 * pad, std::round(18.0f * s)}, l, th.muted, 0.0f, th.smallSize);
        } else if (show >= 0) {
            const city::UtilDef& d = defs[show];
            DrawThumb(dl, {card.x + pad, card.y + pad, thumbW, thumbH}, d, s, true);
            const float tx = card.x + pad * 2 + thumbW, tw = card.x + card.w - pad - tx;
            char cost[32];
            Money(cost, sizeof(cost), d.cost);
            g.TextIn({tx, card.y + pad - 2.0f * s, tw, std::round(24.0f * s)}, d.name, Rgba(120, 190, 255), 0.0f, th.fontSize + 2.0f, true);
            g.TextIn({tx, card.y + pad - 2.0f * s, tw, std::round(24.0f * s)}, cost, Rgba(110, 220, 110), 1.0f, th.fontSize + 1.0f, true);
            std::vector<std::string> lines;
            Wrap(g, d.desc, tw, th.smallSize, lines);
            for (size_t i = 0; i < lines.size() && i < 3; ++i)
                g.TextIn({tx, card.y + pad + std::round((26.0f + 16.0f * i) * s), tw, std::round(16.0f * s)}, lines[i].c_str(), th.text, 0.0f, th.smallSize);
            char l1[96], l2[96];
            std::snprintf(l1, sizeof(l1), "Lot Size: %dx%d (%.0fm x %.0fm)", d.w, d.d, d.W(), d.D());
            if (d.kind == city::Util::Electricity) std::snprintf(l2, sizeof(l2), "%s: %.1f MW", d.backup ? "Backup output" : "Output", d.supply);
            else std::snprintf(l2, sizeof(l2), "%s: %.0f m3/day", d.kind == city::Util::Water ? "Water" : "Sewage", d.supply);
            const float ly = card.y + pad + std::round(78.0f * s);
            g.TextIn({card.x + pad, ly, card.w - 2 * pad, std::round(16.0f * s)}, l1, th.text, 0.0f, th.smallSize);
            g.TextIn({card.x + pad, ly + std::round(16.0f * s), card.w - 2 * pad, std::round(16.0f * s)}, l2, KindColor(d.kind), 0.0f, th.smallSize);
            g.TextIn({card.x + pad, ly + std::round(32.0f * s), card.w - 2 * pad, std::round(16.0f * s)}, d.rule, th.muted, 0.0f, th.smallSize);
            char up[40];
            Money(up, sizeof(up), d.upkeep);
            std::snprintf(up + std::strlen(up), sizeof(up) - std::strlen(up), " /mo.");
            g.TextIn({card.x + pad, card.y + card.h - std::round(26.0f * s), card.w - 2 * pad, std::round(18.0f * s)}, up, th.textBright, 1.0f, th.smallSize);
            g.TextIn({card.x + pad, card.y + card.h - std::round(26.0f * s), card.w - 2 * pad, std::round(18.0f * s)}, "Upkeep", th.muted, 0.0f, th.smallSize);
        } else g.TextIn(card, "Pick a building or a line", th.muted, 0.5f, th.fontSize);
    }
    {
        const float size = std::round(40.0f * s), gap = std::round(4.0f * s);
        const Rect bar = g.Reserve(size, panelW * s);
        static const Icon kIcons[] = {Icon::Bolt, Icon::Drop};
        for (int i = 0; i < (int)city::UTab::Count; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##utab%d", i);
            const Rect r = {std::round(bar.x + i * (size + gap)), bar.y, size, size};
            if (IconBox(g, tip, id, r, kIcons[i], (int)t.tab == i, city::UTabName((city::UTab)i))) t.SelectTab((city::UTab)i);
        }
        const Rect close = {bar.x + bar.w - size, bar.y, size, size};
        if (IconBox(g, tip, "##uclose", close, Icon::Close, false, "Close (Esc)")) t.Close();
    }
    {
        // tiles: the tab's buildings, then its lines
        std::vector<int> tiles;
        for (size_t i = 0; i < defs.size(); ++i)
            if (defs[i].tab == t.tab) tiles.push_back((int)i);
        if (t.tab == city::UTab::Electricity) tiles.push_back(1000);
        else tiles.push_back(1001), tiles.push_back(1002);
        const float tw = std::round(96.0f * s), tH = std::round(72.0f * s), gap = std::round(6.0f * s);
        const float innerW = (panelW - 20.0f) * s;
        const int perRow = std::max(1, (int)((innerW + gap) / (tw + gap)));
        const int rows = ((int)tiles.size() + perRow - 1) / perRow;
        const Rect area = g.Reserve(std::max(1, rows) * tH + (std::max(1, rows) - 1) * gap, innerW);
        int newHover = -1;
        for (size_t k = 0; k < tiles.size(); ++k) {
            const int id = tiles[k];
            const Rect r = {std::round(area.x + (k % perRow) * (tw + gap)), std::round(area.y + (k / perRow) * (tH + gap)), tw, tH};
            char name[24];
            std::snprintf(name, sizeof(name), "##utile%d", id);
            bool hot = false, held = false;
            if (g.ClickArea(name, r, &hot, &held)) {
                if (id >= 1000) t.SelectLine(id - 1000);
                else t.Select(id);
            }
            if (hot) newHover = id;
            const bool sel = id >= 1000 ? t.line == id - 1000 : id == t.def;
            const float selT = g.Ease(name, 3, sel ? 1.0f : 0.0f), hT = g.Ease(name, 1, hot ? 1.0f : 0.0f);
            DrawList& dl = g.Draw();
            dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Mix(Mix(th.widgetTop, th.widgetHoverTop, hT), th.accentTop, selT * 0.7f),
                    Mix(Mix(th.widgetBottom, th.widgetHoverBottom, hT), th.accentBottom, selT * 0.7f), th.widgetRadius, std::max(th.border, selT * 2.0f * s),
                    Mix(th.widgetBorder, Rgba(120, 190, 255), selT));
            const Rect art = {r.x, r.y, r.w, r.h - std::round(18.0f * s)};
            const char* cap;
            if (id >= 1000) {
                const city::Util u = (city::Util)(id - 1000);
                DrawIcon(dl, id == 1000 ? Icon::Pole : KindIcon(u), art.x + art.w * 0.5f, art.y + art.h * 0.5f, std::round(30.0f * s), th.iconStroke, KindColor(u));
                cap = LineShort(id - 1000);
            } else {
                DrawThumb(dl, art, defs[id], s, false);
                cap = defs[id].shortName;
            }
            g.TextIn({r.x, r.y + r.h - std::round(18.0f * s), r.w, std::round(16.0f * s)}, cap, th.text, 0.5f, th.smallSize);
        }
        hovered = newHover;
    }
    {
        const Rect r = g.Reserve(std::round(18.0f * s), 0);
        const char* hint = t.bulldoze ? "Bulldoze: click a source building or a line to remove it   Esc: stop"
                           : t.Laying() ? "Click: start / continue the line   Right click / Esc: end it   Page Up / Down: elevation"
                           : t.def >= 0 ? "Click: place   Right click / Esc: deselect   , . rotate"
                                        : "Roads carry power, water and sewage under them (not highways). Pick a source to place, or a line to lay";
        g.TextIn(r, hint, th.muted, 0.0f, th.smallSize);
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    return false;
}

} // namespace gui
} // namespace tbd
