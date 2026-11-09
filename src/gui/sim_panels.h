// The phases' info panels (CS2 sim roadmap 3.5, Phase 0; frozen interface). Platform free, headless (tests\sim_panels_test).
//
// A phase never draws: it fills a SimPanelSpec (title, stat rows, bars, a stacked bar, a sparkline, a list, slider rows)
// in its own .inl and registers it with RegisterSimPanel. Both HUDs draw every registered panel through the one entry
// point SimPhasePanelsFrame: the CS2 HUD (city_hud.h) as a card at the right edge, Glass Dock+ (glass_dock_host.inl) as a
// bottom sheet. Touch first: every slider row is a 44 pt target, the sheet scrolls by dragging, and nothing needs hover.
//
//   Stat       label left, value right (tone: -1 bad red, 0 neutral, 1 good green)
//   Bar        label and value over a 0..1 meter
//   Stacked    one bar split by parts (shares of the sum), a legend line under it
//   Sparkline  a series, oldest first, with the latest value at the right
//   List       label then up to maxItems lines (left text, right text)
//   Slider     label and value over a track; writes *bind (if set) and reports the row in SimPanelOut
//   Caption    one muted line (a section header or a note)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gui.h"

namespace tbd {
namespace gui {

enum class SimRow : uint8_t { Stat, Bar, Stacked, Sparkline, List, Slider, Caption };

struct SimStackPart {
    std::string label;
    float value = 0.0f;
    Color color = 0;
};

struct SimPanelRow {
    SimRow kind = SimRow::Stat;
    std::string label, value; // value: the right-hand text (stat, bar, sparkline, slider)
    int tone = 0;             // -1 bad, 0 neutral, 1 good
    float t = 0.0f;           // bar: fill 0..1
    std::vector<SimStackPart> parts;                        // stacked
    std::vector<float> series;                              // sparkline, oldest first
    std::vector<std::pair<std::string, std::string>> items; // list
    int maxItems = 6;
    float v = 0.0f, lo = 0.0f, hi = 1.0f, step = 0.0f; // slider: value, range, step (0 = continuous)
    const char* fmt = "%.0f";                          // slider: how the value prints (when `value` is empty)
    float* bind = nullptr;                             // slider: written on a drag (the phase's own setting)
};

struct SimPanelSpec {
    std::string title, sub, icon; // icon: a Glass Dock+ atlas name (the Info group's item)
    Color accent = Rgba(10, 132, 255);
    std::vector<SimPanelRow> rows;

    void Clear() { title.clear(), sub.clear(), rows.clear(); }
    SimPanelRow& Add(SimRow k, std::string label) {
        rows.emplace_back();
        rows.back().kind = k, rows.back().label = std::move(label);
        return rows.back();
    }
    SimPanelRow& Stat(std::string label, std::string value, int tone = 0) {
        SimPanelRow& r = Add(SimRow::Stat, std::move(label));
        r.value = std::move(value), r.tone = tone;
        return r;
    }
    SimPanelRow& Bar(std::string label, float t, std::string value = "", int tone = 0) {
        SimPanelRow& r = Add(SimRow::Bar, std::move(label));
        r.t = std::clamp(t, 0.0f, 1.0f), r.value = std::move(value), r.tone = tone;
        return r;
    }
    SimPanelRow& Stacked(std::string label, std::vector<SimStackPart> parts) {
        SimPanelRow& r = Add(SimRow::Stacked, std::move(label));
        r.parts = std::move(parts);
        return r;
    }
    SimPanelRow& Spark(std::string label, std::vector<float> series, std::string value = "") {
        SimPanelRow& r = Add(SimRow::Sparkline, std::move(label));
        r.series = std::move(series), r.value = std::move(value);
        return r;
    }
    SimPanelRow& List(std::string label, std::vector<std::pair<std::string, std::string>> items, int maxItems = 6) {
        SimPanelRow& r = Add(SimRow::List, std::move(label));
        r.items = std::move(items), r.maxItems = std::max(0, maxItems);
        return r;
    }
    SimPanelRow& Slider(std::string label, float* bind, float lo, float hi, float step = 0.0f, const char* fmt = "%.0f") {
        SimPanelRow& r = Add(SimRow::Slider, std::move(label));
        r.bind = bind, r.lo = lo, r.hi = std::max(hi, lo), r.step = step, r.fmt = fmt;
        r.v = std::clamp(bind ? *bind : lo, r.lo, r.hi);
        return r;
    }
    SimPanelRow& Caption(std::string text) { return Add(SimRow::Caption, std::move(text)); }
};

// ---------------------------------------------------------------- pure helpers (the test covers these)
inline float SimSliderSnap(float v, float lo, float hi, float step) {
    v = std::clamp(v, lo, hi);
    if (step > 0.0f) v = std::clamp(lo + std::round((v - lo) / step) * step, lo, hi);
    return v;
}
// The value under x on a track from x0, w wide.
inline float SimSliderFromX(float x, float x0, float w, float lo, float hi, float step) {
    const float k = w > 0.0f ? std::clamp((x - x0) / w, 0.0f, 1.0f) : 0.0f;
    return SimSliderSnap(lo + (hi - lo) * k, lo, hi, step);
}
// Each part's share of the sum (negatives count as 0); all 0 when the sum is 0.
inline void SimStackShares(const std::vector<SimStackPart>& parts, std::vector<float>& out) {
    out.assign(parts.size(), 0.0f);
    double sum = 0;
    for (const SimStackPart& p : parts) sum += std::max(0.0f, p.value);
    if (sum <= 0) return;
    for (size_t i = 0; i < parts.size(); ++i) out[i] = (float)(std::max(0.0f, parts[i].value) / sum);
}
// The series' range, widened when flat; false with fewer than 2 points.
inline bool SimSparkRange(const std::vector<float>& s, float& lo, float& hi) {
    if (s.size() < 2) return false;
    lo = hi = s[0];
    for (float v : s) lo = std::min(lo, v), hi = std::max(hi, v);
    if (hi - lo < 1e-6f) lo -= 0.5f, hi += 0.5f;
    return true;
}

// ---------------------------------------------------------------- layout, in points (pixels = points * Context::Scale)
struct SimPanelMetrics {
    float pad = 14.0f, headH = 44.0f, gap = 6.0f;
    float statH = 24.0f, barH = 34.0f, stackH = 48.0f, sparkH = 60.0f, listHeadH = 20.0f, listItemH = 20.0f;
    float sliderH = 50.0f; // label line + a 44 pt touch target is the track's hit row
    float captionH = 20.0f;
    float textScale = 1.0f; // the Phone UI on an iPad: 1.15 .. 1.3
    float sideW = 340.0f;   // the CS2 HUD's card
    float sheetMaxW = 560.0f, sheetMaxFrac = 0.6f; // Glass Dock+: the sheet's widest, and its share of the screen height
};

inline float SimRowHeight(const SimPanelRow& r, const SimPanelMetrics& m) {
    const float k = m.textScale;
    switch (r.kind) {
    case SimRow::Stat: return m.statH * k;
    case SimRow::Bar: return m.barH * k;
    case SimRow::Stacked: return m.stackH * k;
    case SimRow::Sparkline: return m.sparkH * k;
    case SimRow::List: return (m.listHeadH + m.listItemH * (float)std::min<size_t>(r.items.size(), (size_t)r.maxItems)) * k;
    case SimRow::Slider: return std::max(m.sliderH * k, 44.0f + 16.0f * k);
    case SimRow::Caption: return m.captionH * k;
    }
    return 0.0f;
}
// The rows' total height (gaps between rows), header and padding excluded.
inline float SimPanelContentHeight(const SimPanelSpec& s, const SimPanelMetrics& m) {
    float h = 0.0f;
    for (size_t i = 0; i < s.rows.size(); ++i) h += SimRowHeight(s.rows[i], m) + (i ? m.gap : 0.0f);
    return h;
}

enum class SimPanelPlace : uint8_t {
    Side,  // the CS2 HUD: a card at the right edge, under the top bar
    Sheet, // Glass Dock+: a bottom sheet centred above the dock, full width on a phone
};

// Where the panel goes (points). view: the screen in points; bottomInset: what the dock / toolbar takes at the bottom;
// topInset: the status bar / safe area at the top. The height fits the content up to the place's maximum (it scrolls).
inline Rect SimPanelFrame(SimPanelPlace place, float viewW, float viewH, float contentH, float bottomInset, float topInset,
                          const SimPanelMetrics& m) {
    const float full = m.headH * m.textScale + m.pad * 2.0f + contentH;
    if (place == SimPanelPlace::Side) {
        const float w = std::min(m.sideW, std::max(200.0f, viewW - 96.0f)); // left of the HUD's right column (56 pt + gaps)
        const float y = std::max(topInset, 0.0f) + 60.0f;
        const float maxH = std::max(120.0f, viewH - y - bottomInset - 12.0f);
        return {std::max(0.0f, viewW - w - 80.0f), y, w, std::min(full, maxH)};
    }
    const float w = std::min(m.sheetMaxW, std::max(200.0f, viewW - 24.0f));
    const float maxH = std::max(140.0f, std::min(viewH * m.sheetMaxFrac, viewH - bottomInset - topInset - 12.0f));
    const float h = std::min(full, maxH);
    return {(viewW - w) * 0.5f, std::max(topInset, viewH - bottomInset - 8.0f - h), w, h};
}

struct SimPanelState {
    float scroll = 0.0f;    // points the rows are scrolled up
    int sliderRow = -1;     // the slider being dragged
    bool dragging = false;  // a scroll drag is under way
    float dragY = 0.0f, dragScroll = 0.0f;
};
struct SimPanelOut {
    bool closed = false;  // the close button was tapped
    int changedRow = -1;  // a slider row moved this frame (its v and *bind hold the new value)
    Rect frame;           // where the panel was drawn (pixels)
};

// ---------------------------------------------------------------- drawing (Glass Dock+ look; the classic HUD falls back to flat)
inline Color SimToneColor(int tone, Color neutral) { return tone < 0 ? Rgba(255, 105, 97) : tone > 0 ? Rgba(74, 222, 128) : neutral; }

// Draws spec and handles its input. topInset / bottomInset in points (see SimPanelFrame).
inline SimPanelOut DrawSimPanel(Context& g, SimPanelSpec& spec, SimPanelState& st, SimPanelPlace place, float bottomInset, float topInset,
                                const SimPanelMetrics& m = SimPanelMetrics()) {
    SimPanelOut out;
    const Input& in = g.In();
    const float s = std::max(g.Scale(), 0.01f), k = m.textScale;
    const float contentH = SimPanelContentHeight(spec, m);
    const Rect f = SimPanelFrame(place, in.viewW / s, in.viewH / s, contentH, bottomInset, topInset, m);
    out.frame = {f.x * s, f.y * s, f.w * s, f.h * s};
    DrawList& dl = g.Draw();
    const Color ink = Rgba(255, 255, 255), muted = Rgba(255, 255, 255, 168), chip = Rgba(255, 255, 255, 34), edge = Rgba(255, 255, 255, 38);
    auto R = [&](float x, float y, float w, float h, float r, Color c) { dl.Rect(x * s, y * s, (x + w) * s, (y + h) * s, c, c, r * s); };
    auto T = [&](const std::string& t, float x, float y, float w, float h, Color c, float align, float pt, bool bold = false) {
        g.TextIn({x * s, y * s, w * s, h * s}, t.c_str(), c, align, pt * k * s, bold);
    };
    auto In = [&](float x, float y, float w, float h, float px, float py) { return px >= x * s && py >= y * s && px < (x + w) * s && py < (y + h) * s; };

    dl.Shadow(f.x * s, (f.y + 8.0f) * s, (f.x + f.w) * s, (f.y + f.h + 8.0f) * s, 18.0f * s, 15.0f * s, Rgba(0, 0, 0, 68));
    dl.Glass(f.x * s, f.y * s, (f.x + f.w) * s, (f.y + f.h) * s, 18.0f * s, Rgba(28, 29, 34, 200), 22.0f * s, 1.6f, std::max(1.0f, std::round(s)), edge);
    g.Block(out.frame);
    if (place == SimPanelPlace::Sheet) R(f.x + f.w * 0.5f - 18.0f, f.y + 5.0f, 36.0f, 4.0f, 2.0f, Rgba(255, 255, 255, 90)); // the grabber

    // header: title, subtitle, close (a 44 pt target)
    const float px = f.x + m.pad, pw = f.w - 2.0f * m.pad, headH = m.headH * k, hy = f.y + m.pad * 0.5f;
    const float cs = 44.0f, cx = f.x + f.w - cs - 2.0f, cy = f.y + 2.0f;
    T(spec.title, px, hy, pw - cs, spec.sub.empty() ? headH : headH * 0.55f, ink, 0.0f, 15.0f, true);
    if (!spec.sub.empty()) T(spec.sub, px, hy + headH * 0.55f, pw - cs, headH * 0.4f, muted, 0.0f, 11.5f);
    {
        const bool inside = In(cx, cy, cs, cs, in.mouseX, in.mouseY), pressedIn = In(cx, cy, cs, cs, in.pressX, in.pressY);
        if (in.released && pressedIn && inside) out.closed = true;
        R(cx + 9.0f, cy + 9.0f, 26.0f, 26.0f, 13.0f, in.down && pressedIn && inside ? Rgba(255, 255, 255, 60) : chip);
        DrawIcon(dl, Icon::Close, (cx + cs * 0.5f) * s, (cy + cs * 0.5f) * s, 11.0f * s, 1.6f * s, ink);
    }

    // body: the rows, clipped and scrolled
    const float by0 = hy + headH, by1 = f.y + f.h - m.pad * 0.5f, bodyH = std::max(0.0f, by1 - by0);
    const float maxScroll = std::max(0.0f, contentH - bodyH);
    const bool overBody = In(px, by0, pw, bodyH, in.mouseX, in.mouseY);
    if (overBody && in.wheel != 0.0f) st.scroll -= in.wheel * 40.0f;
    if (st.dragging && in.down) st.scroll = st.dragScroll - (in.mouseY - st.dragY) / s;
    if (!in.down) st.dragging = false, st.sliderRow = -1;
    st.scroll = std::clamp(st.scroll, 0.0f, maxScroll);
    dl.SetClip(px * s, by0 * s, (px + pw) * s, by1 * s);
    std::vector<float> shares;
    float y = by0 - st.scroll;
    for (size_t i = 0; i < spec.rows.size(); ++i) {
        SimPanelRow& r = spec.rows[i];
        const float h = SimRowHeight(r, m), line = 16.0f * k;
        const float ry = y;
        y += h + m.gap;
        if (ry + h < by0 || ry > by1) continue; // out of view: no draw, no input
        const Color tone = SimToneColor(r.tone, ink);
        switch (r.kind) {
        case SimRow::Stat:
            T(r.label, px, ry, pw * 0.55f, h, muted, 0.0f, 12.0f);
            T(r.value, px + pw * 0.4f, ry, pw * 0.6f, h, tone, 1.0f, 12.5f, true);
            break;
        case SimRow::Caption: T(r.label, px, ry, pw, h, muted, 0.0f, 11.0f, true); break;
        case SimRow::Bar: {
            T(r.label, px, ry, pw * 0.6f, line, muted, 0.0f, 12.0f);
            T(r.value, px + pw * 0.4f, ry, pw * 0.6f, line, tone, 1.0f, 12.0f, true);
            const float ty = ry + line + (h - line - 8.0f) * 0.5f;
            R(px, ty, pw, 8.0f, 4.0f, chip);
            if (r.t > 0.0f) R(px, ty, std::max(8.0f, pw * r.t), 8.0f, 4.0f, SimToneColor(r.tone, spec.accent));
            break;
        }
        case SimRow::Stacked: {
            T(r.label, px, ry, pw, line, muted, 0.0f, 12.0f);
            const float ty = ry + line + 2.0f, bh = 12.0f;
            R(px, ty, pw, bh, 4.0f, chip);
            SimStackShares(r.parts, shares);
            float x = px;
            for (size_t p = 0; p < r.parts.size(); ++p) {
                const float w = pw * shares[p];
                if (w >= 0.5f) dl.Rect(x * s, ty * s, (x + w) * s, (ty + bh) * s, r.parts[p].color, r.parts[p].color, p == 0 || p + 1 == r.parts.size() ? 4.0f * s : 0.0f);
                x += w;
            }
            const size_t n = std::min<size_t>(r.parts.size(), 4); // the legend: up to four parts
            for (size_t p = 0; p < n; ++p) {
                const float lx = px + pw * (float)p / (float)n, ly = ty + bh + 4.0f, lw = pw / (float)n;
                R(lx, ly + 4.0f, 8.0f, 8.0f, 2.0f, r.parts[p].color);
                char pct[16];
                std::snprintf(pct, sizeof(pct), " %d%%", (int)std::lround(shares[p] * 100.0f));
                T(r.parts[p].label + pct, lx + 11.0f, ly, lw - 12.0f, line, muted, 0.0f, 10.5f);
            }
            break;
        }
        case SimRow::Sparkline: {
            T(r.label, px, ry, pw * 0.6f, line, muted, 0.0f, 12.0f);
            T(r.value, px + pw * 0.4f, ry, pw * 0.6f, line, tone, 1.0f, 12.0f, true);
            const float ty = ry + line + 2.0f, th = h - line - 4.0f;
            R(px, ty, pw, th, 6.0f, Rgba(0, 0, 0, 60));
            float lo, hi;
            if (!SimSparkRange(r.series, lo, hi)) break;
            const size_t n = r.series.size();
            auto Y = [&](float v) { return ty + 3.0f + (th - 6.0f) * (1.0f - (v - lo) / (hi - lo)); };
            if (lo < 0.0f && hi > 0.0f) R(px + 3.0f, Y(0.0f), pw - 6.0f, 1.0f, 0.0f, Rgba(255, 255, 255, 50)); // the zero line
            for (size_t p = 1; p < n; ++p) {
                const float x0 = px + 3.0f + (pw - 6.0f) * (float)(p - 1) / (float)(n - 1), x1 = px + 3.0f + (pw - 6.0f) * (float)p / (float)(n - 1);
                const Color c = r.series[p] < 0.0f ? Rgba(255, 105, 97) : SimToneColor(r.tone, spec.accent);
                dl.Line(x0 * s, Y(r.series[p - 1]) * s, x1 * s, Y(r.series[p]) * s, std::max(1.5f, 1.75f * s), c);
            }
            break;
        }
        case SimRow::List: {
            T(r.label, px, ry, pw, m.listHeadH * k, muted, 0.0f, 11.0f, true);
            const size_t n = std::min<size_t>(r.items.size(), (size_t)r.maxItems);
            for (size_t p = 0; p < n; ++p) {
                const float iy = ry + (m.listHeadH + m.listItemH * (float)p) * k;
                T(r.items[p].first, px + 6.0f, iy, pw * 0.65f, m.listItemH * k, ink, 0.0f, 12.0f);
                T(r.items[p].second, px + pw * 0.35f, iy, pw * 0.65f, m.listItemH * k, muted, 1.0f, 12.0f);
            }
            break;
        }
        case SimRow::Slider: {
            const float hitY = ry + h - 44.0f, tx = px + 11.0f, tw = pw - 22.0f;
            if (in.pressed && st.sliderRow < 0 && !st.dragging && In(px, hitY, pw, 44.0f, in.pressX, in.pressY) && In(px, by0, pw, bodyH, in.pressX, in.pressY))
                st.sliderRow = (int)i;
            if (st.sliderRow == (int)i && in.down) {
                const float nv = SimSliderFromX(in.mouseX / s, tx, tw, r.lo, r.hi, r.step);
                if (nv != r.v) {
                    r.v = nv, out.changedRow = (int)i;
                    if (r.bind) *r.bind = nv;
                }
            }
            char vb[32];
            if (r.value.empty()) std::snprintf(vb, sizeof(vb), r.fmt ? r.fmt : "%.0f", r.v);
            T(r.label, px, ry, pw * 0.6f, line, muted, 0.0f, 12.0f);
            T(r.value.empty() ? std::string(vb) : r.value, px + pw * 0.4f, ry, pw * 0.6f, line, ink, 1.0f, 12.0f, true);
            const float k01 = r.hi > r.lo ? (r.v - r.lo) / (r.hi - r.lo) : 0.0f, cyT = hitY + 22.0f;
            R(tx, cyT - 2.0f, tw, 4.0f, 2.0f, chip);
            R(tx, cyT - 2.0f, tw * k01, 4.0f, 2.0f, spec.accent);
            dl.Circle((tx + tw * k01) * s, cyT * s, (st.sliderRow == (int)i ? 13.0f : 11.0f) * s, ink, 1.0f * s, Rgba(0, 0, 0, 70));
            break;
        }
        }
    }
    dl.ClearClip();
    // a press in the body that no slider took scrolls the rows (touch: drag them)
    if (in.pressed && st.sliderRow < 0 && !st.dragging && maxScroll > 0.0f && In(px, by0, pw, bodyH, in.pressX, in.pressY))
        st.dragging = true, st.dragY = in.pressY, st.dragScroll = st.scroll;
    if (maxScroll > 0.0f) { // the scroll bar
        const float barH = std::max(24.0f, bodyH * bodyH / contentH), barY = by0 + (bodyH - barH) * (st.scroll / maxScroll);
        R(f.x + f.w - 5.0f, barY, 3.0f, barH, 1.5f, Rgba(255, 255, 255, 70));
    }
    return out;
}

// ---------------------------------------------------------------- the phases' registry and the one HUD entry point
// A phase registers once (its Init, before the HUDs build their catalogues): fill builds the spec every frame the panel
// is open (from the phase's own state); changed, if set, hears about a slider row the player moved (after *bind was
// written). id is stable ("p4.services"); name is the menu label; icon a Glass Dock+ atlas name.
struct SimPanelEntry {
    const char* id = "";
    const char* name = "";
    const char* icon = "stats";
    void (*fill)(SimPanelSpec& spec, void* user) = nullptr;
    void (*changed)(const SimPanelSpec& spec, int row, void* user) = nullptr;
    void* user = nullptr;
};
inline std::vector<SimPanelEntry>& SimPanelRegistry() {
    static std::vector<SimPanelEntry> k;
    return k;
}
// Adds e, or replaces the entry with the same id. Returns its index (-1 without an id or a fill).
inline int RegisterSimPanel(const SimPanelEntry& e) {
    if (!e.id || !*e.id || !e.fill) return -1;
    auto& r = SimPanelRegistry();
    for (size_t i = 0; i < r.size(); ++i)
        if (std::strcmp(r[i].id, e.id) == 0) { r[i] = e; return (int)i; }
    r.push_back(e);
    return (int)r.size() - 1;
}
inline int FindSimPanel(const char* id) {
    const auto& r = SimPanelRegistry();
    for (size_t i = 0; id && i < r.size(); ++i)
        if (std::strcmp(r[i].id, id) == 0) return (int)i;
    return -1;
}

// What a HUD keeps: the open panel (one at a time) and its scroll / drag state.
struct SimPanelsHost {
    int open = -1; // index into SimPanelRegistry, -1 none
    SimPanelState st;
    SimPanelSpec spec;
    void Open(int i) {
        const int n = (int)SimPanelRegistry().size();
        open = i >= 0 && i < n ? i : -1, st = SimPanelState();
    }
    void Toggle(int i) { Open(open == i ? -1 : i); }
    void Close() { Open(-1); }
    bool IsOpen() const { return open >= 0; }
};

// Both HUDs call this once per frame. Draws the open panel (if any), routes its sliders to the phase and closes it on
// the close button. True while a panel is up. Sheet in Glass Dock+, Side in the CS2 HUD.
inline bool SimPhasePanelsFrame(Context& g, SimPanelsHost& h, SimPanelPlace place, float bottomInset, float topInset = 0.0f,
                                float textScale = 1.0f) {
    const auto& reg = SimPanelRegistry();
    if (h.open < 0 || h.open >= (int)reg.size() || !reg[(size_t)h.open].fill) { h.open = -1; return false; }
    const SimPanelEntry& e = reg[(size_t)h.open];
    h.spec.Clear();
    h.spec.title = e.name, h.spec.icon = e.icon ? e.icon : "";
    e.fill(h.spec, e.user);
    SimPanelMetrics m;
    m.textScale = textScale;
    const SimPanelOut o = DrawSimPanel(g, h.spec, h.st, place, bottomInset, topInset, m);
    if (o.changedRow >= 0 && e.changed) e.changed(h.spec, o.changedRow, e.user);
    if (o.closed) h.Close();
    return true;
}

} // namespace gui
} // namespace tbd
