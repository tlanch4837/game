// The transport tool's GUI, laid out like Cities: Skylines II's asset placing panel:
//   launcher       a "Transport" button left of "Roads" (bottom centre) opens it;
//   bottom left    the tool options: Rotation, Show Marker, Snapping (all / track / coast), Tools (bulldoze);
//   bottom centre  the picker: an info card for the hovered (else the chosen) asset (name, price, description, lot size,
//                  workplaces, upkeep, vehicles, noise; the rule it needs, like "must be built on a qualifying coast"),
//                  the category tabs (Rail, Ships, Air) with a close button, and the tiles. The Rail tab also lists the
//                  railway tracks: picking one opens the road tool on its Railways tab.
// Edits TransportToolState directly. No platform headers (gui_test.cpp style hosts can drive it headless).
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../city/transit.h"
#include "road_toolbar.h"

namespace tbd {
namespace gui {

struct TransportToolState {
    bool active = false;
    city::TTab tab = city::TTab::Rail;
    int def = -1; // the chosen building (index into city::Defs()); -1: none, the cursor does nothing
    uint32_t lockedTabs = 0; // one bit per TTab locked by a milestone (the host sets it): shown with a lock, not selectable
    int lastDef[(int)city::TTab::Count] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    float rotation = 0;
    bool marker = true;     // show the lot outline, the water apron, the shoreline
    bool snapAll = true;
    bool snapTrack = true;  // stations jump onto the nearest railway
    bool snapCoast = true;  // harbours and shipyards jump onto the nearest shore and face the water
    bool bulldoze = false;  // click a placed building to remove it
    // transit lines + outside connections (city/transit.h)
    bool lines = false;                                 // the Lines page instead of the buildings picker
    city::TMode lmode = city::TMode::Bus;
    city::TKind lkind = city::TKind::Passenger;
    int outKind = -1;                                   // >= 0: placing an outside connection (a city::OutKind)
    bool lineBulldoze = false;                          // click a checkpoint / line / outside connection to remove it
    int lineStops = 0;                                  // set by the host: checkpoints in the line being built
    bool cancelLine = false;                            // set by Esc / the GUI: drop the line in progress (the host consumes it)
    bool finishLine = false;                            // set by the GUI: Finish pressed (the host consumes it)
    char status[160] = {};                              // set by the host: demand / flow line
    int openRoad = -1;      // set by the GUI: a track tile was picked (an index into city::Assets()); the host consumes it

    void Select(int d) {
        def = d;
        bulldoze = false;
        if (d >= 0) {
            tab = city::Defs()[d].tab;
            lastDef[(int)tab] = d;
        }
    }
    void SelectTab(city::TTab t) {
        tab = t;
        const int last = lastDef[(int)t];
        if (def >= 0 && city::Defs()[def].tab != t) def = last;
    }
    void Rotate(float deg) { rotation = std::fmod(rotation + deg + 360.0f, 360.0f); }
    city::PlanOptions Options() const {
        city::PlanOptions o;
        o.snapTrack = snapAll && snapTrack;
        o.snapCoast = snapAll && snapCoast;
        return o;
    }
    // Esc: drop the chosen asset first, then close
    void Escape() {
        if (lines && (lineStops > 0 || outKind >= 0 || lineBulldoze)) outKind = -1, lineBulldoze = false, cancelLine = true;
        else if (bulldoze) bulldoze = false;
        else if (def >= 0) def = -1;
        else active = false;
    }
};

namespace transport_ui {

inline void Money(char* out, size_t cap, int v) {
    char digits[24];
    std::snprintf(digits, sizeof(digits), "%d", v);
    std::string s;
    const int n = (int)std::strlen(digits);
    for (int i = 0; i < n; ++i) {
        s += digits[i];
        if ((n - 1 - i) % 3 == 0 && i + 1 < n) s += ',';
    }
    std::snprintf(out, cap, "$%s", s.c_str());
}

// Greedy word wrap by measured width.
inline void Wrap(const Context& g, const char* text, float maxW, float size, std::vector<std::string>& lines) {
    lines.clear();
    std::string cur;
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && g.TextWidth(trial.c_str(), size) > maxW) lines.push_back(cur), cur = word;
        else cur = trial;
        word.clear();
    };
    for (const char* c = text; *c; ++c) {
        if (*c == ' ') flush();
        else word += *c;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
}

// A top-down sketch of a lot: its footprint in the type's colour (to scale, front at the bottom), and what makes it what it is.
inline void DrawThumb(DrawList& dl, const Rect& r, const city::BuildingDef& d, float scale, bool inCard) {
    const float pad = std::round((inCard ? 6.0f : 5.0f) * scale);
    const float availW = r.w - 2 * pad, availH = r.h - 2 * pad;
    const float aspect = (float)d.w / (float)d.d; // w across, d deep
    float w = availW, h = w / aspect;
    if (h > availH) h = availH, w = h * aspect;
    const float x0 = r.x + (r.w - w) * 0.5f, y0 = r.y + (r.h - h) * 0.5f, x1 = x0 + w, y1 = y0 + h;
    const Vec3 c = d.tint;
    const Color body = Rgba((int)(c.x * 200), (int)(c.y * 200), (int)(c.z * 200)), dark = Rgba((int)(c.x * 120), (int)(c.y * 120), (int)(c.z * 120));
    if (d.site == city::Site::Coast) { // the sea in front
        const float wh = std::min(r.y + r.h - y1 - 1.0f, h * 0.45f);
        if (wh > 2.0f) dl.Rect(x0, y1, x1, y1 + wh, Rgba(40, 110, 170), Rgba(26, 78, 130), 2.0f * scale);
    }
    dl.Rect(x0, y0, x1, y1, body, dark, 2.0f * scale, 1.0f * scale, Rgba(250, 251, 252, 150));
    const Color ink = Rgba(240, 244, 250, 200);
    switch (d.model) {
    case city::Model::RailDepot: // a hall at the back and a fan of tracks
        dl.Rect(x0 + w * 0.12f, y0 + h * 0.1f, x1 - w * 0.12f, y0 + h * 0.4f, ink, 1.0f);
        for (int i = 0; i < 6; ++i) dl.Line(x0 + w * (0.17f + 0.13f * i), y0 + h * 0.4f, x0 + w * (0.17f + 0.13f * i), y1 - 1.0f, 1.0f * scale, ink);
        break;
    case city::Model::GlbBuilding: // bay doors along the front and a yard behind
        dl.Rect(x0 + w * 0.08f, y0 + h * 0.08f, x1 - w * 0.08f, y0 + h * 0.5f, ink, 1.0f);
        for (int i = 0; i < 4; ++i) dl.Rect(x0 + w * (0.14f + 0.2f * i), y0 + h * 0.4f, x0 + w * (0.26f + 0.2f * i), y0 + h * 0.5f, dark, 0.0f);
        break;
    case city::Model::TrainStation:
        dl.Line(x0, (y0 + y1) * 0.5f - 1.0f * scale, x1, (y0 + y1) * 0.5f - 1.0f * scale, 1.2f * scale, ink);
        dl.Line(x0, (y0 + y1) * 0.5f + 1.0f * scale, x1, (y0 + y1) * 0.5f + 1.0f * scale, 1.2f * scale, ink);
        dl.Rect(x0 + w * 0.08f, y0 + h * 0.12f, x1 - w * 0.08f, y0 + h * 0.34f, ink, 1.0f);
        dl.Rect(x0 + w * 0.08f, y1 - h * 0.34f, x1 - w * 0.08f, y1 - h * 0.12f, ink, 1.0f);
        break;
    case city::Model::CargoTerminal:
        for (int i = 0; i < 3; ++i) dl.Line(x0, y0 + h * (0.3f + 0.15f * i), x1, y0 + h * (0.3f + 0.15f * i), 1.0f * scale, ink);
        dl.Rect(x0 + w * 0.1f, y0 + h * 0.08f, x0 + w * 0.65f, y0 + h * 0.22f, ink, 1.0f);
        for (int i = 0; i < 6; ++i) dl.Rect(x0 + w * (0.08f + 0.14f * i), y1 - h * 0.28f, x0 + w * (0.17f + 0.14f * i), y1 - h * 0.14f, Rgba(220, 130, 80), 1.0f);
        break;
    case city::Model::Shipyard: case city::Model::CargoHarbor: case city::Model::PassengerHarbor:
        dl.Rect(x0 + w * 0.1f, y0 + h * 0.12f, x0 + w * 0.5f, y0 + h * 0.4f, ink, 1.0f);
        dl.Line(x0, y1 - h * 0.3f, x1, y1 - h * 0.3f, 1.2f * scale, Rgba(120, 200, 255)); // the shoreline
        dl.Rect(x0 + w * 0.6f, y0 + h * 0.5f, x0 + w * 0.8f, y1 - 1.0f, Rgba(240, 244, 250, 230), 1.5f * scale); // a hull
        break;
    case city::Model::SmallAirfield: case city::Model::RegionalAirport:
        dl.Rect(x0 + w * 0.03f, y0 + h * 0.12f, x1 - w * 0.03f, y0 + h * 0.3f, Rgba(30, 32, 38), 1.0f);
        for (int i = 0; i < 9; ++i) dl.Rect(x0 + w * (0.08f + 0.1f * i), y0 + h * 0.2f - 0.5f, x0 + w * (0.12f + 0.1f * i), y0 + h * 0.2f + 0.5f, ink, 0.0f);
        dl.Rect(x0 + w * 0.25f, y1 - h * 0.3f, x0 + w * 0.75f, y1 - h * 0.12f, ink, 1.0f);
        break;
    }
}

inline const char* ShortSite(city::Site s) { return s == city::Site::Coast ? "Coast" : s == city::Site::Track ? "On track" : ""; }

} // namespace transport_ui

// The launcher (closed) or the options + picker (open). Returns true on the frame it opened.
inline bool TransportToolbar(Context& g, TransportToolState& t, float lift) {
    using namespace road_ui;
    using namespace transport_ui;
    const float s = g.Scale();
    t.openRoad = -1;
    if (!t.active) {
        return false; // the city HUD's toolbar (city_hud.h) opens it
    }
    const Theme& th = g.Style();
    Tip tip;

    // ---------------------------------------------------------------- tool options (bottom left)
    g.BeginPanel("##transport options", Anchor::BottomLeft, 8.0f, lift, 448.0f, 0.0f, 10.0f);
    {
        Row row = BeginRow(g, "Rotation", 430);
        const float valueW = std::round(72.0f * s);
        const Rect plus = row.Slot(0, 1);
        const Rect val = {plus.x - row.gap - valueW, row.r.y, valueW, row.size};
        const Rect minus = {val.x - row.gap - row.size, row.r.y, row.size, row.size};
        const bool locked = t.def >= 0 && city::Defs()[t.def].site != city::Site::Flat && t.snapAll &&
                            (city::Defs()[t.def].site == city::Site::Track ? t.snapTrack : t.snapCoast); // the site decides the facing
        if (IconBox(g, tip, "##rot left", minus, Icon::RotateLeft, false, "Rotate left ( , )", locked)) t.Rotate(15.0f);
        DrawList& dl = g.Draw();
        dl.Rect(val.x, val.y, val.x + val.w, val.y + val.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        char v[24];
        if (locked) std::snprintf(v, sizeof(v), "Auto");
        else std::snprintf(v, sizeof(v), "%.0f deg", t.rotation);
        g.TextIn(val, v, locked ? th.muted : th.textBright, 0.5f, th.fontSize);
        if (IconBox(g, tip, "##rot right", plus, Icon::RotateRight, false, "Rotate right ( . )", locked)) t.Rotate(-15.0f);
    }
    {
        Row row = BeginRow(g, "Page", 430);
        if (IconBox(g, tip, "##page bld", row.Slot(0, 2), Icon::Transport, !t.lines, "Buildings: depots, stations, harbours, airports")) t.lines = false, t.outKind = -1, t.lineBulldoze = false, t.cancelLine = true;
        if (IconBox(g, tip, "##page lines", row.Slot(1, 2), Icon::Roads, t.lines, "Lines: bus, tram, train, boat, plane, subway routes and outside connections")) t.lines = true, t.def = -1, t.bulldoze = false;
    }
    {
        Row row = BeginRow(g, "Show Marker", 430);
        if (IconBox(g, tip, "##marker", row.Slot(0, 1), Icon::Marker, t.marker, t.marker ? "Lot outline, water apron and shoreline shown" : "Markers hidden")) t.marker = !t.marker;
    }
    {
        Row row = BeginRow(g, "Snapping", 430);
        if (IconBox(g, tip, "##tsnap all", row.Slot(0, 3), Icon::SnapAll, t.snapAll, "All snapping")) t.snapAll = !t.snapAll;
        if (IconBox(g, tip, "##tsnap track", row.Slot(1, 3), Icon::Rail, t.snapAll && t.snapTrack, "Stations snap onto the nearest railway", !t.snapAll)) t.snapTrack = !t.snapTrack;
        if (IconBox(g, tip, "##tsnap coast", row.Slot(2, 3), Icon::Waves, t.snapAll && t.snapCoast, "Harbours and shipyards snap to the nearest shore", !t.snapAll)) t.snapCoast = !t.snapCoast;
    }
    {
        Row row = BeginRow(g, "Tools", 430);
        if (IconBox(g, tip, "##tbulldoze", row.Slot(0, 1), Icon::Bulldoze, t.bulldoze, "Bulldoze a transport building")) {
            t.bulldoze = !t.bulldoze;
            if (t.bulldoze) t.def = -1;
        }
    }
    g.EndPanel();

    // ---------------------------------------------------------------- the Lines page: pick a mode, click checkpoints, click the first again to close
    if (t.lines) {
        const float lw = std::clamp(g.In().viewW / s - (8.0f + 448.0f + 8.0f + 8.0f), 340.0f, 700.0f);
        g.BeginPanel("##transit lines", Anchor::BottomLeft, 8.0f + 448.0f + 8.0f, lift, lw, 0.0f, 10.0f);
        const float size = std::round(40.0f * s), gap = std::round(4.0f * s);
        {
            const Rect bar = g.Reserve(size, lw * s);
            static const Icon kIc[] = {Icon::Roads, Icon::Transport, Icon::Rail, Icon::Ship, Icon::Plane, Icon::Underground};
            for (int i = 0; i < (int)city::TMode::Count; ++i) {
                char id[16];
                std::snprintf(id, sizeof(id), "##lmode%d", i);
                const Rect r = {std::round(bar.x + i * (size + gap)), bar.y, size, size};
                if (IconBox(g, tip, id, r, kIc[i], t.outKind < 0 && !t.lineBulldoze && (int)t.lmode == i, city::TModeName((city::TMode)i))) t.lmode = (city::TMode)i, t.outKind = -1, t.lineBulldoze = false, t.cancelLine = true;
            }
            const Rect close = {bar.x + bar.w - size, bar.y, size, size};
            if (IconBox(g, tip, "##lclose", close, Icon::Close, false, "Close (Esc)")) t.active = false, t.lines = false, t.cancelLine = true;
        }
        {
            const Rect bar = g.Reserve(size * 0.8f, lw * s);
            const float bw = std::round(120.0f * s);
            for (int k = 0; k < 2; ++k) {
                char id[16];
                std::snprintf(id, sizeof(id), "##lkind%d", k);
                const Rect r = {bar.x + k * (bw + gap), bar.y, bw, bar.h};
                const bool on = (int)t.lkind == k;
                if (IconBox(g, tip, id, r, Icon::None, on, k ? "Cargo lines only stop at cargo checkpoints" : "Passenger lines only stop at passenger checkpoints")) t.lkind = (city::TKind)k, t.cancelLine = true;
                g.TextIn(r, k ? "Cargo" : "Passengers", on ? th.textBright : th.text, 0.5f, th.smallSize);
            }
            const Rect fin = {bar.x + bar.w - bw, bar.y, bw, bar.h};
            if (IconBox(g, tip, "##lfinish", fin, Icon::None, false, "Finish an out-and-back line (Enter). Clicking the first checkpoint again closes a loop", t.lineStops < 2)) t.finishLine = true;
            g.TextIn(fin, "Finish (Enter)", t.lineStops < 2 ? th.muted : th.text, 0.5f, th.smallSize);
        }
        {
            const Rect bar = g.Reserve(size * 0.8f, lw * s);
            const float bw = std::round(104.0f * s);
            static const char* const kOut[] = {"Road", "Rail", "Sea", "Air"};
            for (int k = 0; k < 4; ++k) {
                char id[16];
                std::snprintf(id, sizeof(id), "##lout%d", k);
                const Rect r = {bar.x + k * (bw + gap), bar.y, bw, bar.h};
                if (IconBox(g, tip, id, r, Icon::None, t.outKind == k, "Outside connection: a two-way link to cities beyond the map, for people and cargo")) t.outKind = t.outKind == k ? -1 : k, t.lineBulldoze = false, t.cancelLine = true;
                char lab[24];
                std::snprintf(lab, sizeof(lab), "<> %s", kOut[k]);
                g.TextIn(r, lab, t.outKind == k ? th.textBright : th.text, 0.5f, th.smallSize);
            }
            const Rect bd = {bar.x + bar.w - size, bar.y, size, bar.h};
            if (IconBox(g, tip, "##lbulldoze", bd, Icon::Bulldoze, t.lineBulldoze, "Remove a checkpoint, line or outside connection")) t.lineBulldoze = !t.lineBulldoze, t.outKind = -1, t.cancelLine = true;
        }
        {
            const Rect r = g.Reserve(std::round(18.0f * s), 0);
            char hint[200];
            if (t.lineBulldoze) std::snprintf(hint, sizeof(hint), "Remove: click a line, checkpoint or outside connection   Esc: stop");
            else if (t.outKind >= 0) std::snprintf(hint, sizeof(hint), "Click to place a two-way outside connection (people and cargo both ways)   Esc: stop");
            else if (t.lineStops == 0) std::snprintf(hint, sizeof(hint), "%s %s line: click the first checkpoint", city::TKindName(t.lkind), city::TModeName(t.lmode));
            else std::snprintf(hint, sizeof(hint), "%d checkpoint%s: click the next, click the FIRST again to close the loop, Enter to finish, Esc to cancel", t.lineStops, t.lineStops == 1 ? "" : "s");
            g.TextIn(r, hint, th.muted, 0.0f, th.smallSize);
            const Rect r2 = g.Reserve(std::round(18.0f * s), 0);
            g.TextIn(r2, t.status, th.text, 0.0f, th.smallSize);
        }
        g.EndPanel();
        if (tip.on) g.Chip(tip.x, tip.y, tip.text);
        return false;
    }

    // ---------------------------------------------------------------- picker (bottom centre): info card, tabs, tiles
    const float panelW = std::clamp(g.In().viewW / s - (8.0f + 448.0f + 8.0f + 8.0f), 340.0f, 700.0f);
    g.BeginPanel("##transport picker", Anchor::BottomLeft, 8.0f + 448.0f + 8.0f, lift, panelW, 0.0f, 10.0f);
    static int hovered = -1; // a tile: an index into Defs(), or 1000 + a track asset
    const auto& defs = city::Defs();
    {
        // the card shows the hovered building, else the chosen one, else the tab's first
        int show = hovered >= 0 && hovered < 1000 ? hovered : t.def;
        if (show < 0) {
            for (size_t i = 0; i < defs.size(); ++i)
                if (defs[i].tab == t.tab) {
                    show = (int)i;
                    break;
                }
        }
        const bool trackHover = hovered >= 1000;
        const float cardH = std::round(184.0f * s), innerW = (panelW - 20.0f) * s;
        const Rect card = g.Reserve(cardH, innerW);
        DrawList& dl = g.Draw();
        dl.Rect(card.x, card.y, card.x + card.w, card.y + card.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        const float pad = std::round(10.0f * s);
        const float thumbW = std::round(96.0f * s), thumbH = std::round(70.0f * s);
        if (trackHover) {
            const city::Asset& a = city::Assets()[hovered - 1000];
            const city::Profile& pr = city::Profiles()[a.profile];
            DrawProfileTile(dl, {card.x + pad, card.y + pad, thumbH, thumbH}, pr, s);
            g.TextIn({card.x + pad * 2 + thumbH, card.y + pad, card.w - pad * 3 - thumbH, std::round(24.0f * s)}, a.name.c_str(), Rgba(120, 190, 255), 0.0f, th.fontSize + 2.0f, true);
            std::vector<std::string> lines;
            Wrap(g, "Lay track with the road tool: the same tool modes, elevation and snapping. Rails never join roads; where they cross they make a level crossing.", card.w - pad * 3 - thumbH, th.smallSize, lines);
            for (size_t i = 0; i < lines.size() && i < 4; ++i)
                g.TextIn({card.x + pad * 2 + thumbH, card.y + pad + std::round((26.0f + 17.0f * i) * s), card.w - pad * 3 - thumbH, std::round(16.0f * s)}, lines[i].c_str(), th.text, 0.0f, th.smallSize);
            char l[64];
            std::snprintf(l, sizeof(l), "Width: %.1f m", pr.width);
            g.TextIn({card.x + pad, card.y + card.h - std::round(26.0f * s), card.w - 2 * pad, std::round(18.0f * s)}, l, th.muted, 0.0f, th.smallSize);
        } else if (show >= 0) {
            const city::BuildingDef& d = defs[show];
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
            char l1[64], l2[48];
            std::snprintf(l1, sizeof(l1), "Lot Size: %dx%d (%.0fm x %.0fm)", d.w, d.d, d.W(), d.D());
            std::snprintf(l2, sizeof(l2), "Workplaces: %d", d.workplaces);
            const float ly = card.y + pad + std::round(78.0f * s);
            g.TextIn({card.x + pad, ly, card.w - 2 * pad, std::round(16.0f * s)}, l1, th.text, 0.0f, th.smallSize);
            g.TextIn({card.x + pad, ly + std::round(16.0f * s), card.w - 2 * pad, std::round(16.0f * s)}, l2, th.text, 0.0f, th.smallSize);
            g.TextIn({card.x + pad, ly + std::round(32.0f * s), card.w - 2 * pad, std::round(16.0f * s)}, city::SiteRule(d.site), d.site == city::Site::Flat ? th.muted : Rgba(255, 214, 120), 0.0f, th.smallSize);
            // the figures along the bottom
            const float sy = card.y + card.h - std::round(32.0f * s);
            dl.Line(card.x + pad, sy - 4.0f * s, card.x + card.w - pad, sy - 4.0f * s, 1.0f, th.rule);
            const float colW = (card.w - 2 * pad) / 3.0f;
            char up[40], veh[24];
            Money(up, sizeof(up), d.upkeep);
            std::snprintf(up + std::strlen(up), sizeof(up) - std::strlen(up), " /mo.");
            std::snprintf(veh, sizeof(veh), "%d", d.vehicles);
            const char* heads[3] = {"Upkeep", "Vehicles", "Noise Pollution"};
            const char* vals[3] = {up, veh, d.noise};
            for (int i = 0; i < 3; ++i) {
                const Rect c = {card.x + pad + colW * i, sy, colW, std::round(14.0f * s)};
                g.TextIn(c, heads[i], th.muted, 1.0f, th.smallSize);
                g.TextIn({c.x, c.y + std::round(13.0f * s), c.w, c.h}, vals[i], th.textBright, 1.0f, th.smallSize);
            }
        } else g.TextIn(card, "Pick a building", th.muted, 0.5f, th.fontSize);
    }
    {
        const float size = std::round(40.0f * s), gap = std::round(4.0f * s);
        const Rect bar = g.Reserve(size, panelW * s);
        static const Icon kIcons[] = {Icon::Transport, Icon::Ship, Icon::Plane, Icon::Roads, Icon::Fire, Icon::Health, Icon::Park, Icon::Police, Icon::Education, Icon::Comms};
        if ((t.lockedTabs >> (int)t.tab) & 1u) // the open tab is locked: the first open one of its group instead
            for (int i = 0; i < (int)city::TTab::Count; ++i)
                if (!((t.lockedTabs >> i) & 1u) && city::TabGroup((city::TTab)i) == city::TabGroup(t.tab)) { t.SelectTab((city::TTab)i); break; }
        int slot = 0; // a service tab (Fire, Healthcare, Police, ...) is its own panel from its HUD button; the others share Transportation
        for (int i = 0; i < (int)city::TTab::Count; ++i) {
            if (i != (int)t.tab && city::TabGroup((city::TTab)i) != city::TabGroup(t.tab)) continue;
            char id[16];
            std::snprintf(id, sizeof(id), "##ttab%d", i);
            const Rect r = {std::round(bar.x + slot++ * (size + gap)), bar.y, size, size};
            const bool locked = (t.lockedTabs >> i) & 1u;
            if (IconBox(g, tip, id, r, kIcons[i], (int)t.tab == i, locked ? "Locked: reach the next milestone" : city::TTabName((city::TTab)i)) && !locked)
                t.SelectTab((city::TTab)i);
            if (locked) {
                g.Draw().Rect(r.x, r.y, r.x + r.w, r.y + r.h, Rgba(10, 11, 13, 160), Rgba(10, 11, 13, 160), g.Style().widgetRadius);
                DrawIcon(g.Draw(), Icon::Lock, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.45f, g.Style().iconStroke, Rgba(250, 210, 120));
            }
        }
        const Rect close = {bar.x + bar.w - size, bar.y, size, size};
        if (IconBox(g, tip, "##tclose", close, Icon::Close, false, "Close (Esc)")) t.active = false, t.def = -1, t.bulldoze = false;
    }
    {
        // tiles: on the Rail tab the railway tracks first, then the buildings of the tab
        struct Tile {
            int id; // building index, or 1000 + asset index
        };
        std::vector<Tile> tiles;
        if (t.tab == city::TTab::Rail) {
            const auto& assets = city::Assets();
            for (size_t i = 0; i < assets.size(); ++i)
                if (assets[i].kind == city::AssetKind::Road && city::Profiles()[assets[i].profile].cat == city::Cat::Rail) tiles.push_back({1000 + (int)i});
        }
        for (size_t i = 0; i < defs.size(); ++i)
            if (defs[i].tab == t.tab) tiles.push_back({(int)i});
        const float tw = std::round(96.0f * s), tH = std::round(72.0f * s), gap = std::round(6.0f * s);
        const float innerW = (panelW - 20.0f) * s;
        const int perRow = std::max(1, (int)((innerW + gap) / (tw + gap)));
        const int rows = ((int)tiles.size() + perRow - 1) / perRow;
        const Rect area = g.Reserve(std::max(1, rows) * tH + (std::max(1, rows) - 1) * gap, innerW);
        int newHover = -1;
        for (size_t k = 0; k < tiles.size(); ++k) {
            const int id = tiles[k].id;
            const Rect r = {std::round(area.x + (k % perRow) * (tw + gap)), std::round(area.y + (k / perRow) * (tH + gap)), tw, tH};
            char name[24];
            std::snprintf(name, sizeof(name), "##tile%d", id);
            bool hot = false, held = false;
            if (g.ClickArea(name, r, &hot, &held)) {
                if (id >= 1000) t.openRoad = id - 1000;
                else t.Select(id);
            }
            if (hot) newHover = id;
            const bool sel = id < 1000 && id == t.def;
            const float selT = g.Ease(name, 3, sel ? 1.0f : 0.0f), hT = g.Ease(name, 1, hot ? 1.0f : 0.0f);
            DrawList& dl = g.Draw();
            dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Mix(Mix(th.widgetTop, th.widgetHoverTop, hT), th.accentTop, selT * 0.7f),
                    Mix(Mix(th.widgetBottom, th.widgetHoverBottom, hT), th.accentBottom, selT * 0.7f), th.widgetRadius, std::max(th.border, selT * 2.0f * s),
                    Mix(th.widgetBorder, Rgba(120, 190, 255), selT));
            const Rect art = {r.x, r.y, r.w, r.h - std::round(18.0f * s)};
            const char* cap;
            if (id >= 1000) {
                const city::Asset& a = city::Assets()[id - 1000];
                DrawProfileTile(dl, {art.x + (art.w - art.h) * 0.5f, art.y, art.h, art.h}, city::Profiles()[a.profile], s);
                cap = a.profile == city::ProfileIndex("Single Track Railway") ? "Single Track" : "Double Track";
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
        const char* hint = t.bulldoze ? "Bulldoze: click a transport building to remove it   Esc: stop"
                           : t.def >= 0 ? "Click: place   Right click / Esc: deselect   , . rotate"
                                        : "Pick a building, or a track to lay with the road tool";
        g.TextIn(r, hint, th.muted, 0.0f, th.smallSize);
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    return false;
}

} // namespace gui
} // namespace tbd
