// The road tool's GUI, laid out like Cities: Skylines II's:
//   bottom left    the tool options: Tool Mode (6 modes), Elevation (down / value / up / step size), Parallel Mode
//                  (switch and offset), Snapping (all + 7 options), and Bulldoze / Undo;
//   bottom centre  the asset picker: category tabs (small / medium / large roads, highways, quays, paths,
//                  intersections, roundabouts), a close button, the chosen asset's name and a grid of tiles (each road
//                  tile is a drawn top-down cross-section of its strips and lane markings);
//   closed         a single "Roads" button at the bottom centre opens it.
// Edits the RoadTool directly. No platform headers (gui_test.cpp style hosts can drive it headless).
#pragma once
#include <cstdio>

#include "../city/road_tool.h"
#include "gui.h"

namespace tbd {
namespace gui {

namespace road_ui {

struct Tip {
    bool on = false;
    float x = 0, y = 0;
    char text[96] = {};
};

inline Color MatSwatch(city::Mat m) {
    switch (m) {
    case city::Mat::Asphalt: return Rgba(62, 64, 70);
    case city::Mat::AsphaltWorn: return Rgba(88, 86, 84);
    case city::Mat::Gravel: return Rgba(146, 130, 104);
    case city::Mat::Concrete: return Rgba(170, 170, 165);
    case city::Mat::Curb: return Rgba(205, 205, 200);
    case city::Mat::Pavers: return Rgba(176, 140, 112);
    case city::Mat::Grass: return Rgba(92, 138, 66);
    case city::Mat::BikeAsphalt: return Rgba(150, 66, 58);
    default: return Rgba(255, 255, 255);
    }
}

// A square icon button; `on` paints it accent (selected / enabled). Sets the tooltip while hovered.
inline bool IconBox(Context& g, Tip& tip, const char* id, const Rect& r, Icon icon, bool on, const char* tipText, bool dim = false) {
    const Theme& t = g.Style();
    bool hot = false, held = false;
    const bool clicked = g.ClickArea(id, r, &hot, &held);
    const float onT = g.Ease(id, 3, on ? 1.0f : 0.0f), hT = g.Ease(id, 1, hot ? 1.0f : 0.0f), pT = g.Ease(id, 2, held ? 1.0f : 0.0f);
    Color top = Mix(Mix(t.widgetTop, t.widgetHoverTop, hT), t.accentTop, onT), bottom = Mix(Mix(t.widgetBottom, t.widgetHoverBottom, hT), t.accentBottom, onT);
    top = Mix(top, t.accentPressedTop, pT), bottom = Mix(bottom, t.accentPressedBottom, pT);
    DrawList& dl = g.Draw();
    dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, top, bottom, t.widgetRadius, t.border, Mix(t.widgetBorder, t.accentBorder, onT));
    Color ink = Mix(t.text, t.textBright, std::max(onT, hT));
    if (dim) ink = Fade(ink, 0.45f);
    DrawIcon(dl, icon, r.x + r.w * 0.5f, r.y + r.h * 0.5f, std::round(std::min(r.w, r.h) * 0.52f), t.iconStroke, ink);
    if (hot && tipText) {
        tip.on = true;
        tip.x = r.x + r.w * 0.5f, tip.y = r.y - std::round(16.0f * g.Scale());
        std::snprintf(tip.text, sizeof(tip.text), "%s", tipText);
    }
    return clicked;
}

// One options row: the label on the left, `count` icon slots right-aligned. Returns the slot rectangles' origin.
struct Row {
    Rect r;
    float size, gap;
    Rect Slot(int indexFromLeft, int count, float extraLeft = 0) const {
        const float x0 = r.x + r.w - (size * count + gap * (count - 1)) - extraLeft;
        return {std::round(x0 + indexFromLeft * (size + gap)), r.y, size, size};
    }
};
inline Row BeginRow(Context& g, const char* label, float naturalW) {
    const Theme& t = g.Style();
    const float s = g.Scale();
    Row row;
    row.size = std::round(32.0f * s);
    row.gap = std::round(4.0f * s);
    row.r = g.Reserve(row.size, naturalW * s);
    g.TextIn({row.r.x, row.r.y, std::round(118.0f * s), row.r.h}, label, t.text, 0.0f, t.fontSize, true);
    return row;
}

// A road tile: the cross-section drawn top-down (strips as bands, lane lines, one-way chevron).
inline void DrawProfileTile(DrawList& dl, const Rect& r, const city::Profile& p, float scale) {
    const float pad = std::round(6.0f * scale), x0 = r.x + pad, x1 = r.x + r.w - pad, y0 = r.y + pad, y1 = r.y + r.h - pad;
    const float k = (x1 - x0) / p.width;
    float x = x0;
    int lane = 0;
    for (size_t i = 0; i < p.strips.size(); ++i) {
        const city::Strip& st = p.strips[i];
        const float w = st.w * k;
        dl.Rect(x, y0, x + w + 0.5f, y1, MatSwatch(st.mat), Mix(MatSwatch(st.mat), Rgba(0, 0, 0), 0.18f), 0.0f);
        const bool nextLane = i + 1 < p.strips.size() && p.strips[i + 1].kind == city::Kind::Lane;
        if (st.kind == city::Kind::Lane && nextLane && st.mat != city::Mat::Gravel) {
            const float lx = x + w;
            if (lane + 1 == p.back && p.fwd > 0) dl.Rect(lx - 1.0f * scale, y0, lx + 1.0f * scale, y1, Rgba(240, 190, 40), 0.0f);
            else
                for (float yy = y0 + 2 * scale; yy < y1; yy += 9 * scale) dl.Rect(lx - 0.6f * scale, yy, lx + 0.6f * scale, std::min(y1, yy + 4 * scale), Rgba(235, 235, 230), 0.0f);
        }
        if (st.kind == city::Kind::Lane) ++lane;
        if (st.kind == city::Kind::Track) { // sleepers and two rails
            const float cx = x + w * 0.5f, g = 0.7175f * k;
            for (float yy = y0 + 2 * scale; yy < y1; yy += 5 * scale) dl.Rect(cx - 1.3f * k, yy, cx + 1.3f * k, yy + 1.6f * scale, Rgba(74, 54, 40), 0.0f);
            for (float o : {-g, g}) dl.Rect(cx + o - 0.7f * scale, y0, cx + o + 0.7f * scale, y1, Rgba(190, 192, 200), 0.0f);
        }
        x += w;
    }
    if (p.OneWay()) {
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, a = 5.0f * scale;
        dl.Line(cx - a, cy + a * 0.5f, cx, cy - a * 0.5f, 1.6f * scale, Rgba(255, 255, 255));
        dl.Line(cx, cy - a * 0.5f, cx + a, cy + a * 0.5f, 1.6f * scale, Rgba(255, 255, 255));
    }
}

inline Icon TabIcon(city::Tab t) {
    static const Icon k[] = {Icon::RoadSmall, Icon::RoadMedium, Icon::RoadLarge, Icon::Highway, Icon::Quay, Icon::Path, Icon::Rail, Icon::Intersection, Icon::Roundabout};
    return k[(int)t];
}

} // namespace road_ui

// Runs the road GUI for this frame. `lift`: unscaled margin above the bottom edge (clear of the toast row).
inline void RoadToolbar(Context& g, city::RoadTool& t, float lift = 34.0f) {
    using namespace road_ui;
    static Tip tip;
    tip.on = false;
    const float s = g.Scale();
    if (!t.active) {
        return; // the city HUD's toolbar (city_hud.h) opens it
    }
    const Theme& th = g.Style();

    // ---------------------------------------------------------------- tool options (bottom left)
    g.BeginPanel("##road options", Anchor::BottomLeft, 8.0f, lift, 448.0f, 0.0f, 10.0f);
    {
        static const Icon kModeIcons[] = {Icon::Straight, Icon::CurveSimple, Icon::CurveComplex, Icon::Continuous, Icon::Grid, Icon::Replace, Icon::Swap};
        static const char* const kModeTips[] = {"Straight", "Simple Curve", "Complex Curve", "Continuous", "Grid", "Replace",
                                                "Swap: press on a road, drag the way traffic goes, release"};
        const bool prefab = t.Current().kind != city::AssetKind::Road;
        Row row = BeginRow(g, "Tool Mode", 430);
        for (int i = 0; i < 7; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##mode%d", i);
            if (IconBox(g, tip, id, row.Slot(i, 7), kModeIcons[i], !prefab && (int)t.mode == i, kModeTips[i], prefab) && !prefab) t.SetMode((city::ToolMode)i);
        }
    }
    {
        Row row = BeginRow(g, "Elevation", 430);
        const float valueW = std::round(84.0f * s);
        // [v] [ value ] [^]  [steps]
        const Rect steps = row.Slot(0, 1);
        Rect downR = {steps.x - row.gap * 3 - row.size * 2 - valueW, row.r.y, row.size, row.size};
        Rect valueR = {downR.x + row.size + row.gap, row.r.y, valueW, row.size};
        Rect upR = {valueR.x + valueW + row.gap, row.r.y, row.size, row.size};
        if (IconBox(g, tip, "##elev down", downR, Icon::ChevronDown, false, "Lower (Page Down)")) t.Raise(-1);
        DrawList& dl = g.Draw();
        dl.Rect(valueR.x, valueR.y, valueR.x + valueR.w, valueR.y + valueR.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        char v[24];
        std::snprintf(v, sizeof(v), "%g m", t.elevation);
        g.TextIn(valueR, v, t.elevation < -city::mesh::kTunnelDepth ? Rgba(140, 190, 255) : t.elevation >= city::mesh::kElevated ? Rgba(255, 214, 120) : th.textBright, 0.5f, th.fontSize);
        if (IconBox(g, tip, "##elev up", upR, Icon::ChevronUp, false, "Raise (Page Up)")) t.Raise(1);
        static char stepTip[32];
        std::snprintf(stepTip, sizeof(stepTip), "Step %g m", t.Step());
        if (IconBox(g, tip, "##elev step", steps, (Icon)((int)Icon::Steps1 + (3 - (t.stepIdx & 3))), true, stepTip)) t.CycleStep();
    }
    {
        Row row = BeginRow(g, "Parallel Mode", 430);
        const Rect sw = row.Slot(0, 1);
        if (t.parallel) {
            const float valueW = std::round(72.0f * s);
            const Rect plus = {sw.x - row.gap * 2 - row.size, row.r.y, row.size, row.size};
            const Rect val = {plus.x - row.gap - valueW, row.r.y, valueW, row.size};
            const Rect minus = {val.x - row.gap - row.size, row.r.y, row.size, row.size};
            if (IconBox(g, tip, "##par minus", minus, Icon::ChevronDown, false, "Closer")) t.parallelOffset = std::max(t.Prof().width + 0.5f, t.ParallelOffset() - 4.0f);
            DrawList& dl = g.Draw();
            dl.Rect(val.x, val.y, val.x + val.w, val.y + val.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
            char v[24];
            std::snprintf(v, sizeof(v), "%.0f m", t.ParallelOffset());
            g.TextIn(val, v, th.textBright, 0.5f, th.fontSize);
            if (IconBox(g, tip, "##par plus", plus, Icon::ChevronUp, false, "Further")) t.parallelOffset = std::min(400.0f, t.ParallelOffset() + 4.0f);
        }
        if (IconBox(g, tip, "##parallel", sw, Icon::Parallel, t.parallel, t.parallel ? "Parallel mode on" : "Parallel mode off")) t.parallel = !t.parallel;
    }
    {
        Row row = BeginRow(g, "Snapping", 430);
        struct S {
            Icon icon;
            uint32_t bit;
            const char* tip;
        };
        static const S k[] = {{Icon::SnapAll, 0, "All snapping"},
                              {Icon::SnapLength, city::SnapLength, "Cell length"},
                              {Icon::SnapGeometry, city::SnapGeometry, "Existing geometry"},
                              {Icon::SnapStraight, city::SnapStraight, "Straight directions"},
                              {Icon::SnapNetSide, city::SnapNetSide, "Net side"},
                              {Icon::SnapGuide, city::SnapGuide, "Guidelines"},
                              {Icon::SnapZone, city::SnapZone, "Zone grid"},
                              {Icon::SnapContour, city::SnapContour, "Contour lines"}};
        // a slightly smaller box so eight fit beside the label
        row.size = std::round(30.0f * s);
        for (int i = 0; i < 8; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##snap%d", i);
            Rect r = row.Slot(i, 8);
            r.y += std::round(1.0f * s);
            const bool on = i == 0 ? t.snapAll : (t.snaps & k[i].bit) != 0;
            if (IconBox(g, tip, id, r, k[i].icon, on && (i == 0 || t.snapAll), k[i].tip, i > 0 && !t.snapAll)) {
                if (i == 0) t.snapAll = !t.snapAll;
                else t.snaps ^= k[i].bit;
            }
        }
    }
    {
        Row row = BeginRow(g, "Tools", 430);
        if (IconBox(g, tip, "##bulldoze", row.Slot(0, 2), Icon::Bulldoze, t.mode == city::ToolMode::Bulldoze, "Bulldoze roads")) {
            t.SetMode(t.mode == city::ToolMode::Bulldoze ? city::ToolMode::Straight : city::ToolMode::Bulldoze);
        }
        if (IconBox(g, tip, "##undo", row.Slot(1, 2), Icon::Undo, false, "Undo (Ctrl+Z)", !t.CanUndo())) t.Undo();
    }
    g.EndPanel();

    // ---------------------------------------------------------------- asset picker (bottom centre)
    // beside the options panel; narrower windows get a narrower picker (more tile rows) rather than an overlap
    const float panelW = std::clamp(g.In().viewW / s - (8.0f + 448.0f + 8.0f + 8.0f), 300.0f, 700.0f);
    g.BeginPanel("##road picker", Anchor::BottomLeft, 8.0f + 448.0f + 8.0f, lift, panelW, 0.0f, 10.0f);
    static int hovered = -1;
    {
        const float size = std::round(std::min(40.0f, (panelW - 20.0f - 44.0f - 36.0f) / (float)city::Tab::Count) * s), gap = std::round(4.0f * s);
        const Rect bar = g.Reserve(size, panelW * s);
        for (int i = 0; i < (int)city::Tab::Count; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "##tab%d", i);
            const Rect r = {std::round(bar.x + i * (size + gap)), bar.y, size, size};
            if (IconBox(g, tip, id, r, TabIcon((city::Tab)i), (int)t.tab == i, city::TabName((city::Tab)i))) t.SelectTab((city::Tab)i);
        }
        const Rect close = {bar.x + bar.w - size, bar.y, size, size};
        if (IconBox(g, tip, "##close", close, Icon::Close, false, "Close (Esc)")) t.Close();
    }
    {
        const city::Asset& show = city::Assets()[hovered >= 0 ? hovered : t.asset];
        char line[160];
        if (show.kind == city::AssetKind::Road) {
            const city::Profile& p = city::Profiles()[show.profile];
            if (p.fwd + p.back > 0 && p.cat != city::Cat::Path) std::snprintf(line, sizeof(line), "%s   %d+%d lanes   %.1f m", show.name.c_str(), p.back, p.fwd, p.width);
            else std::snprintf(line, sizeof(line), "%s   %.1f m", show.name.c_str(), p.width);
        } else if (show.kind == city::AssetKind::Roundabout) std::snprintf(line, sizeof(line), "%s   radius %.0f m   , . rotate", show.name.c_str(), show.radius);
        else std::snprintf(line, sizeof(line), "%s   %zu arms   , . rotate", show.name.c_str(), show.armDeg.size());
        const Rect r = g.Reserve(std::round(22.0f * s), 0);
        g.TextIn(r, line, th.textBright, 0.0f, th.fontSize, true);
    }
    {
        int count = 0;
        const auto& assets = city::Assets();
        for (const city::Asset& a : assets) count += a.tab == t.tab ? 1 : 0;
        const float tw = std::round(72.0f * s), tH = std::round(58.0f * s), gap = std::round(6.0f * s);
        const float innerW = (panelW - 20.0f) * s;
        const int perRow = std::max(1, (int)((innerW + gap) / (tw + gap)));
        const int rows = (count + perRow - 1) / perRow;
        const Rect area = g.Reserve(rows * tH + (rows - 1) * gap, innerW);
        int k = 0, newHover = -1;
        for (size_t i = 0; i < assets.size(); ++i) {
            const city::Asset& a = assets[i];
            if (a.tab != t.tab) continue;
            const Rect r = {std::round(area.x + (k % perRow) * (tw + gap)), std::round(area.y + (k / perRow) * (tH + gap)), tw, tH};
            ++k;
            char id[16];
            std::snprintf(id, sizeof(id), "##asset%zu", i);
            bool hot = false, held = false;
            if (g.ClickArea(id, r, &hot, &held)) t.Select((int)i);
            if (hot) newHover = (int)i;
            const bool sel = (int)i == t.asset;
            const float selT = g.Ease(id, 3, sel ? 1.0f : 0.0f), hT = g.Ease(id, 1, hot ? 1.0f : 0.0f);
            DrawList& dl = g.Draw();
            dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, Mix(Mix(th.widgetTop, th.widgetHoverTop, hT), th.accentTop, selT * 0.7f),
                    Mix(Mix(th.widgetBottom, th.widgetHoverBottom, hT), th.accentBottom, selT * 0.7f), th.widgetRadius, std::max(th.border, selT * 2.0f * s),
                    Mix(th.widgetBorder, Rgba(120, 190, 255), selT));
            if (a.kind == city::AssetKind::Road) DrawProfileTile(dl, r, city::Profiles()[a.profile], s);
            else {
                DrawIcon(dl, a.kind == city::AssetKind::Roundabout ? Icon::Roundabout : Icon::Intersection, r.x + r.w * 0.5f, r.y + r.h * 0.45f, std::round(26.0f * s), th.iconStroke * 1.2f, th.textBright);
                char cap[16];
                if (a.kind == city::AssetKind::Roundabout) std::snprintf(cap, sizeof(cap), "R %.0f m", a.radius);
                else std::snprintf(cap, sizeof(cap), "%zu-way", a.armDeg.size());
                g.TextIn({r.x, r.y + r.h - std::round(18.0f * s), r.w, std::round(16.0f * s)}, cap, th.muted, 0.5f, th.smallSize);
            }
        }
        hovered = newHover;
    }
    g.EndPanel();
    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
}

} // namespace gui
} // namespace tbd
