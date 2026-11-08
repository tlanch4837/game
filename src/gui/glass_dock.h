// Glass Dock+ : the mobile / tablet GUI, ported 1:1 from the HTML mockup (gui_mockups/index.html tab 8, v2.css, v2.js).
// Two layouts, switched in Settings:
//   PC UI     a CS2-style glass bar along the bottom (status, 10 categories, alerts / layers / camera / settings); a
//             category opens a glass tool panel of preview tiles above it while the bar shrinks 78 -> 56 pt;
//   Phone UI  a status pill, a pause corner button and a chevron; the chevron (or a swipe in from the right edge) opens a
//             category grid on the left thumb and a column of preview tiles on the right thumb.
// Both share the placement bar, the frosted pause menu and Settings. Every size below is the mockup's CSS pixel (a
// point), times Context::Scale(). Platform free: the host fills DockModel (its real tools) and acts on DockOut.
//
// Drawing: glass is UiMode::Glass (one shared 1/16 backdrop, Renderer::CaptureUiBackdrop), icons and previews are cells
// of the sprite atlas (assets_src/gui/glass_atlas.png, tools/gui_atlas). Sounds: one tick, pitched per action (DockOut::sfx).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gui.h"
#include "news_pill.h"       // the City News card's collapsed / unread state (the host draws the card)
#include "safe_area.h"       // [safe area] the Dynamic Island / corners / home indicator strips (Model::safe)
#include "../gfx_settings.h" // the player's graphics options (the Graphics page edits Model::gfx)

namespace tbd {
namespace gui {
namespace dock {

// ---------------------------------------------------------------- the sprite atlas index (glass_atlas.txt)
struct Sprite { float u0 = 0, v0 = 0, u1 = 0, v1 = 0, w = 0, h = 0; };
struct Atlas {
    std::unordered_map<std::string, int> index;
    std::vector<Sprite> cells;
    bool loaded = false;
    // "key x y w h" lines after a "# glass_atlas.png WxH" header
    bool Parse(const char* text, size_t n) {
        index.clear(), cells.clear();
        float W = 0, H = 0;
        const char* p = text;
        const char* end = text + n;
        while (p < end) {
            const char* e = (const char*)std::memchr(p, '\n', (size_t)(end - p));
            if (!e) e = end;
            std::string line(p, e);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '#') {
                const size_t at = line.find(".png ");
                if (at != std::string::npos) std::sscanf(line.c_str() + at + 5, "%fx%f", &W, &H);
            } else if (!line.empty() && W > 0) {
                char key[128];
                float x, y, w, h;
                if (std::sscanf(line.c_str(), "%127s %f %f %f %f", key, &x, &y, &w, &h) == 5) {
                    index[key] = (int)cells.size();
                    cells.push_back({x / W, y / H, (x + w) / W, (y + h) / H, w, h});
                }
            }
            p = e + 1;
        }
        loaded = !cells.empty();
        return loaded;
    }
    int Find(const std::string& key) const {
        auto it = index.find(key);
        return it == index.end() ? -1 : it->second;
    }
};

inline std::string Slug(const std::string& n) {
    std::string o;
    bool us = false;
    for (char c : n) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c, us = false;
        else if (c >= 'A' && c <= 'Z') o += (char)(c - 'A' + 'a'), us = false;
        else if (!us && !o.empty()) o += '_', us = true;
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o;
}

// ---------------------------------------------------------------- what the host lists (its real tools)
struct Item {
    std::string name, cost; // cost as shown ("$40/m", "$24k", "" = none)
    std::string icon;       // mockup icon name (the tile's fallback and the placement bar's glyph)
    int id = -1;            // the host's action id
    int preview = -1;       // atlas cell of its render (Model::Resolve)
    bool on = false;        // a toggle that is on (Views: the overlay shown)
};
struct Group {
    std::string name, icon;
    std::vector<Item> items;
    std::vector<Group> subs; // tabs inside the group (Healthcare: Health / Deathcare)
};
struct Category {
    std::string id, name, icon;
    Color col;
    std::vector<Group> groups;
};
struct Model {
    std::vector<Category> cats;
    // the status shown
    std::string money = "$0", pop = "0", happy = "0%", date = "Day 1", popUp, moneyUp;
    float moneyBar = 0.5f, popBar = 0.5f, happyBar = 0.8f; // 0..1 fills of the PC bar's pills
    int alerts = 0;
    int speed = 1;       // 1..3
    bool paused = false; // the sim clock
    std::string city = "Shortville";
    // the active tool (empty: none)
    std::string tool, toolCost, toolIcon, modeName, modeIcon;
    std::vector<std::pair<std::string, std::string>> toolOpts; // (name, icon) shown on the placement bar
    std::vector<bool> toolOptOn; // [interact] parallel to toolOpts: a toggle that is on (Rotate, Snap) is drawn lit
    bool bulldozeOn = false;     // [interact] the dock's Bulldoze button is lit
    // the settings
    bool sound = true; // the UI ticks (follows the Effects volume)
    int volume[5] = {100, 70, 80, 100, 100}; // the Sound page, percent: Master, Music, Ambience, Effects, Radio
    tbd::gfx::Settings gfx; // the preset and the individual graphics options (src/gfx_settings.h)
    int textSize = 0; // 0 normal, 1 large
    bool edgePan = true;
    bool showStats = false; // Graphics > Show stats: the FPS / draws / polys / camera text (the host keeps it, settings.ini stats=)
    // [safe area] the strips every anchored element keeps clear, points (the host fills it each frame)
    SafeInsets safe;
    // [road ux] a road / rail / terrain tool with its own compact kit is out: the PC UI bar slims to icons (44 pt) and
    // the placement bar / Phone UI split controls step aside (the kit has their buttons)
    bool slim = false, hidePlaceBar = false;
    void Resolve(const Atlas& a) { // preview cells by slug, as the mockup's pvImg(): name, name_road, name_highway
        for (Category& c : cats)
            for (Group& g : c.groups) {
                auto res = [&](std::vector<Item>& items) {
                    for (Item& it : items) {
                        const std::string s = Slug(it.name);
                        for (const std::string& k : {s, s + "_road", s + "_highway"})
                            if ((it.preview = a.Find("p:" + k)) >= 0) break;
                    }
                };
                res(g.items);
                for (Group& sg : g.subs) res(sg.items);
            }
    }
};

// What happened this frame, for the host.
struct Out {
    int pickItem = -1;      // Item::id picked (closes the panel, shows the placement bar)
    bool toggledPause = false, setSpeed = false;
    int speed = 0;          // with setSpeed
    bool confirm = false, cancel = false, undo = false, mode = false;
    int opt = -1;           // a placement option button (index into Model::toolOpts)
    bool save = false, load = false, photo = false, mainMenu = false;
    bool alertsPressed = false, layersPressed = false, cameraPressed = false;
    bool bulldozePressed = false; // [interact] the dock's Bulldoze button
    bool settingsChanged = false;
    bool menuOpen = false;  // the pause menu is up (the host pauses the sim under it)
    std::vector<float> sfx; // pitches of the UI tick to play (volume .6)
    float bottomH = 0;      // [road ux] points from the screen's bottom edge the dock takes (the kits stack above it)
};

// ---------------------------------------------------------------- the GUI's own state
struct State {
    bool pcUi = true;      // PC UI / Phone UI (Settings)
    bool pad = true;       // the device is an iPad (sizes; Phone UI on iPad = phone layout at 1.3x text)
    int cat = -1, grp = 0, sub = 0;
    int lastCat = -1;      // the panel that is closing
    float catT = 0;        // seconds since the category opened / closed
    bool closing = false;
    bool open = false;     // Phone UI: the menus are out
    float openT = 1;
    bool menu = false, settings = false, gfx = false; // gfx: the Graphics page inside Settings
    bool snd = false;      // the Sound page inside Settings (the volumes)
    float menuT = 1;
    float scroll = 0, scrollMax = 0; // the tile area: PC UI pad vertical, phone horizontal; Phone UI vertical
    bool drag = false;
    float dragFrom = 0, dragScroll = 0, dragStartX = 0, dragStartY = 0;
    Rect scrollArea;
    float toolT = 1;
    std::string toolShown;
    float slimT = 0; // [road ux] 0 normal .. 1 the slim bar (Model::slim), eased
};

// ---------------------------------------------------------------- easing
inline float Bezier(float x, float x1, float y1, float x2, float y2) { // CSS cubic-bezier(x1, y1, x2, y2) at x
    x = std::clamp(x, 0.0f, 1.0f);
    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float u = 1 - t, bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t - x;
        const float d = 3 * u * u * x1 + 6 * u * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (std::fabs(d) < 1e-5f) break;
        t = std::clamp(t - bx / d, 0.0f, 1.0f);
    }
    const float u = 1 - t;
    return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}
inline float EaseDock(float x) { return Bezier(x, 0.3f, 0.9f, 0.3f, 1.0f); }   // the bar and panel: cubic-bezier(.3,.9,.3,1)
inline float EaseIn(float x) { return Bezier(x, 0.42f, 0.0f, 1.0f, 1.0f); }    // ease-in (the panel leaving)
inline float EaseOut(float x) { return Bezier(x, 0.0f, 0.0f, 0.58f, 1.0f); }   // ease-out (the place bar, the pause card)
inline float EaseBack(float x) { return Bezier(x, 0.2f, 0.9f, 0.3f, 1.15f); }  // the Phone UI grid

// ---------------------------------------------------------------- the painter: points in, pixels out
constexpr Color kInk = Rgba(255, 255, 255), kMuted = Rgba(255, 255, 255, 168), kPanel = Rgba(28, 29, 34, 153), kBorder = Rgba(255, 255, 255, 38);
constexpr Color kChip = Rgba(255, 255, 255, 34), kAccent = Rgba(10, 132, 255), kBarInk = Rgba(232, 238, 246);

struct Painter {
    Context& g;
    DrawList& dl;
    const Atlas& atlas;
    float s;            // pixels per point
    float text = 1.0f;  // text scale (Phone UI on iPad: 1.3)
    float alpha = 1.0f; // global fade for the group being drawn
    Painter(Context& c, const Atlas& a, float scale) : g(c), dl(c.Draw()), atlas(a), s(scale) {}

    Color F(Color c) const { return Fade(c, alpha); }
    void Glass(float x, float y, float w, float h, float r, Color tint = kPanel, float blur = 22, float sat = 1.6f, bool shadow = true) {
        if (shadow) dl.Shadow(x * s, (y + 10) * s, (x + w) * s, (y + h + 10) * s, r * s, 30 * s * 0.5f, F(Rgba(0, 0, 0, 68)));
        dl.Glass(x * s, y * s, (x + w) * s, (y + h) * s, r * s, Fade(tint, alpha), blur * s, sat, std::max(1.0f, std::round(s)), F(kBorder));
        g.Block({x * s, y * s, w * s, h * s});
    }
    void Box(float x, float y, float w, float h, float r, Color fill, float border = 0, Color bc = 0) {
        dl.Rect(x * s, y * s, (x + w) * s, (y + h) * s, F(fill), F(fill), r * s, border * s, F(bc));
    }
    void BoxV(float x, float y, float w, float h, float r, Color top, Color bottom, float border = 0, Color bc = 0) {
        dl.Rect(x * s, y * s, (x + w) * s, (y + h) * s, F(top), F(bottom), r * s, border * s, F(bc));
    }
    // an icon: `size` points across; native = its own colours (the "n:" cell) when it has them. A trailing '~' mirrors
    // it left to right ("rotate~": the rotate-back button).
    void Icon(const std::string& name0, float cx, float cy, float size, Color tint = kInk, bool native = true) {
        const bool flip = !name0.empty() && name0.back() == '~';
        const std::string name = flip ? name0.substr(0, name0.size() - 1) : name0;
        int c = native ? atlas.Find("n:" + name) : -1;
        if (c < 0) c = atlas.Find("i:" + name);
        if (c < 0) c = atlas.Find("i:info");
        if (c < 0) return;
        const Sprite& sp = atlas.cells[c];
        const float h = size * 0.5f;
        dl.Image((cx - h) * s, (cy - h) * s, (cx + h) * s, (cy + h) * s, flip ? sp.u1 : sp.u0, sp.v0, flip ? sp.u0 : sp.u1, sp.v1, native && atlas.Find("n:" + name) >= 0 ? F(kInk) : F(tint));
    }
    void Glow(const std::string& name, float cx, float cy, float size, Color col) { // drop-shadow(0 0 6px cc 50%)
        const int c = atlas.Find("g:" + name);
        if (c < 0) return;
        const Sprite& sp = atlas.cells[c];
        const float h = size * 0.75f; // the cell is 1.5x the icon
        dl.Image((cx - h) * s, (cy - h) * s, (cx + h) * s, (cy + h) * s, sp.u0, sp.v0, sp.u1, sp.v1, F(Fade(col, 0.5f)));
    }
    // a preview render in a w x h box (object-fit: contain), or the item's icon when it has none
    void Preview(const Item& it, float x, float y, float w, float h) {
        if (it.preview >= 0) {
            const Sprite& sp = atlas.cells[it.preview];
            const float k = std::min(w / sp.w, h / sp.h), pw = sp.w * k, ph = sp.h * k;
            const float px = x + (w - pw) * 0.5f, py = y + (h - ph) * 0.5f;
            dl.Image(px * s, py * s, (px + pw) * s, (py + ph) * s, sp.u0, sp.v0, sp.u1, sp.v1, F(kInk));
        } else Icon(it.icon, x + w * 0.5f, y + h * 0.5f, std::min(w, h) * 0.55f);
    }
    float TextW(const std::string& t, float size, bool bold = false) const {
        return dl.TextWidth(t.c_str(), t.size(), size * text * s, bold ? FontFace::Bold : FontFace::Regular) / s;
    }
    // text with its box's vertical centre at cy (by cap height); align 0 left, .5 centre, 1 right; ellipsis past maxW
    void Text(const std::string& t0, float x, float cy, float size, Color c, bool bold = false, float align = 0, float maxW = 0) {
        const size_t dot = t0.find("Â·"); // the middle dot (outside the font's ASCII): drawn as a disc
        if (dot != std::string::npos) {
            const std::string a = t0.substr(0, dot), b = t0.substr(dot + 2);
            const float gap = TextW(" ", size, bold), d = size * text * 0.16f;
            const float total = TextW(a, size, bold) + TextW(b, size, bold) + gap * 2;
            const float x0 = x - total * align;
            Text(a, x0, cy, size, c, bold);
            const float dx = x0 + TextW(a, size, bold) + gap;
            dl.Circle(dx * s, (cy - size * text * 0.02f) * s, d * s, F(c));
            Text(b, dx + gap, cy, size, c, bold);
            return;
        }
        std::string t = t0;
        const float sz = size * text;
        if (maxW > 0 && TextW(t, size, bold) > maxW) {
            while (!t.empty() && TextW(t + "...", size, bold) > maxW) t.pop_back();
            t += "...";
        }
        const float w = TextW(t, size, bold);
        const FontFace face = bold ? FontFace::Bold : FontFace::Regular;
        const float cap = dl.CapHeight(sz * s, face) / s;
        dl.Text((x - w * align) * s, (cy + cap * 0.5f) * s, sz * s, face, t.c_str(), t.size(), F(c));
    }
    // a solid triangle pointing right (the speed toggle's arrows), w x h points
    void Arrow(float x, float y, float w, float h, Color c) {
        const int n = std::max(3, (int)std::ceil(h * s));
        for (int i = 0; i < n; ++i) {
            const float t = (i + 0.5f) / n, yy = y + h * t, len = w * (1 - std::fabs(t - 0.5f) * 2);
            dl.Line(x * s, yy * s, (x + len) * s, yy * s, h * s / n * 1.15f, F(c));
        }
    }
};

// A tap: pressed inside, released inside, no scroll drag in between. Hover / held for the visuals.
struct Tapper {
    Context& g;
    State& st;
    bool Tap(const Rect& rp, bool* held = nullptr) const { // rp in pixels
        const Input& in = g.In();
        const bool inside = rp.Contains(in.mouseX, in.mouseY), pressedIn = rp.Contains(in.pressX, in.pressY);
        if (held) *held = in.down && pressedIn && inside && !st.drag;
        return in.released && pressedIn && inside && !st.drag;
    }
};

inline void MenuFrame(Context& g, State& st, Model& m, const Atlas& atlas, Out& out, bool standalone); // [title]

// ---------------------------------------------------------------- the frame
// s: pixels per point. viewW / viewH: the viewport in pixels.
inline void Frame(Context& g, State& st, Model& m, const Atlas& atlas, Out& out) {
    out = Out{};
    const Input& in = g.In();
    const float dt = in.dt;
    const bool phoneLayout = !st.pad; // PC UI sizes: the iPhone set or the iPad set
    // Phone UI on iPad: the phone layout on the 1180 x 820 canvas with 1.3x text (the mockup's font-size: 1.3em)
    const float s = g.Scale();
    Painter P(g, atlas, s);
    P.text = (!st.pcUi && st.pad ? 1.3f : 1.0f) * (m.textSize ? 1.15f : 1.0f);
    // [safe area] W / H are the safe area's right / bottom edges (so every right / bottom anchor clears it); left / top
    // anchors add SL / ST, centred ones centre on (W + SL) / 2
    const float SL = m.safe.L(), ST = m.safe.T();
    const float W = in.viewW / s - m.safe.R(), H = in.viewH / s - m.safe.B();
    Tapper T{g, st};
    auto R = [&](float x, float y, float w, float h) { return Rect{x * s, y * s, w * s, h * s}; };
    auto sfx = [&](float pitch) { if (m.sound) out.sfx.push_back(pitch); };

    // scroll drag: a press that travels 8 pt inside the tile area scrolls it and is no tap
    if (in.pressed) {
        st.drag = false;
        st.dragStartX = in.pressX, st.dragStartY = in.pressY;
        st.dragScroll = st.scroll;
    }
    const bool horizontal = st.pcUi && phoneLayout;
    if (in.down && st.scrollArea.Contains(st.dragStartX, st.dragStartY) && st.scrollMax > 0) {
        const float d = horizontal ? in.mouseX - st.dragStartX : in.mouseY - st.dragStartY;
        if (std::fabs(d) > 8 * s) st.drag = true;
        if (st.drag) st.scroll = std::clamp(st.dragScroll - d / s, 0.0f, st.scrollMax);
    }
    if (in.wheel != 0 && st.scrollArea.Contains(in.mouseX, in.mouseY)) st.scroll = std::clamp(st.scroll - in.wheel * 40, 0.0f, st.scrollMax);
    if (!in.down && in.released == false) st.drag = st.drag && in.down;

    st.catT += dt, st.openT += dt, st.menuT += dt, st.toolT += dt;
    if (m.tool != st.toolShown) st.toolShown = m.tool, st.toolT = 0;
    const bool toolOn = !m.tool.empty();

    auto openCat = [&](int c) {
        if (st.cat == c) { // the same again: close
            st.lastCat = st.cat, st.cat = -1, st.catT = 0, st.closing = true;
            sfx(st.pcUi ? 0.75f : 0.8f);
            return;
        }
        sfx(1.0f);
        if (st.cat < 0) st.catT = 0;
        st.cat = c, st.grp = 0, st.sub = 0, st.scroll = 0, st.closing = false;
    };

    // ================================================================ PC UI
    if (st.pcUi) {
        struct Mx {
            float side, bottom, barH, barSm, pad, r, rSm, gap;
            float emb, embIcon, barsW, pillH, pillFont, pillGap, dtFont, dtGap, spdPadX, spdPadY, spdW, spdFont;
            float catW, catIcon, catIconSm, catFont, catGap, rBtn, rIcon, rGap, rr;
            float panelBottom, panelPadX, panelPadY, panelR, panelMargin, tabPadX, tabPadY, tabFont, tabIcon;
            float tileW, pvW, pvH, tileFont, costFont, tileGap, tileR, maxH, placeBottom;
            bool labels;
        };
        const Mx kPhone{8, 8, 52, 44, 8, 16, 16, 6, 30, 16, 54, 10, 7, 2, 8, 1, 5, 2, 34, 10,
                        36, 20, 20, 7, 0, 28, 14, 3, 8,
                        56, 8, 6, 14, 8, 6, 2, 8, 9,
                        84, 76, 54, 8, 8, 4, 14, 0, 60, false};
        Mx k = phoneLayout
            ? kPhone
            : Mx{12, 12, 78, 56, 12, 22, 20, 10, 46, 24, 92, 14, 10, 4, 11, 4, 7, 3, 44, 10,
                 54, 28, 24, 10, 3, 40, 20, 6, 12,
                 80, 14, 12, 22, 20, 11, 5, 12, 13,
                 118, 104, 78, 11, 10, 8, 14, 300, 86, true};
        // [road ux] the slim bar: a tool with its own kit is out and no category is open. The iPad's sizes ease into the
        // iPhone's (every float of Mx, in order) and the bar to its shrunk height without labels: about 44 pt.
        st.slimT = std::clamp(st.slimT + (m.slim && st.cat < 0 ? dt : -dt) / 0.25f, 0.0f, 1.0f);
        const float slim = EaseDock(st.slimT);
        if (!phoneLayout && slim > 0) {
            float* a = &k.side;
            const float* b = &kPhone.side;
            for (size_t i = 0; i < offsetof(Mx, labels) / sizeof(float); ++i) a[i] += (b[i] - a[i]) * slim;
        }
        // the bar's height: 78 -> 56 over .28 s when a category opens, back when it closes
        const float tOpen = std::min(st.catT / 0.28f, 1.0f);
        const float shrink = std::max(st.cat >= 0 ? EaseDock(tOpen) : (st.closing ? 1 - EaseDock(tOpen) : 0), slim);
        const float barH = k.barH + (k.barSm - k.barH) * shrink;
        const float bx = k.side + SL, bw = W - 2 * k.side - SL, by = H - k.bottom - barH; // [safe area] SL
        const float rad = k.r + (k.rSm - k.r) * shrink;
        out.bottomH = in.viewH / s - by; // [road ux]

        // ---- the tool panel (drawn first: it slides out from under the bar)
        const int pc = st.cat >= 0 ? st.cat : (st.closing && st.catT < 0.22f ? st.lastCat : -1);
        st.scrollArea = {};
        if (pc >= 0 && pc < (int)m.cats.size()) {
            Category& c = m.cats[pc];
            Group& gr = c.groups[std::min(st.grp, (int)c.groups.size() - 1)];
            Group* sg = gr.subs.empty() ? nullptr : &gr.subs[std::min(st.sub, (int)gr.subs.size() - 1)];
            std::vector<Item>& items = sg ? sg->items : gr.items;
            const bool leaving = st.cat < 0;
            const float a = leaving ? 1 - EaseIn(std::min(st.catT / 0.22f, 1.0f)) : EaseDock(tOpen); // opacity
            const float lift = 14 * (1 - a), scale = 0.96f + 0.04f * a;
            // size: tabs row + tiles (wrapping on iPad, one scrolling row on iPhone)
            const bool tabs = c.groups.size() > 1 || sg;
            const float maxW = W - SL - 2 * k.panelMargin; // [safe area] SL
            const float tabH = k.tabFont * 1.4f + 2 * k.tabPadY;
            float tabsW = 0;
            std::vector<float> tabW;
            if (tabs) {
                for (const Group& t : c.groups) {
                    const float w = k.tabIcon + 5 + P.TextW(t.name, k.tabFont) + 2 * k.tabPadX;
                    tabW.push_back(w), tabsW += w + 4;
                }
                if (sg) for (const Group& t : gr.subs) { const float w = k.tabIcon + 5 + P.TextW(t.name, k.tabFont) + 2 * k.tabPadX; tabW.push_back(w), tabsW += w + 4; }
                tabsW -= 4;
            }
            const float tileH = 6 + k.pvH + 2 + k.tileFont * 1.25f + 2 + k.costFont * 1.25f + 6;
            const int n = (int)items.size();
            float innerW, gridH, contentH;
            int cols;
            if (phoneLayout) {
                cols = n;
                innerW = std::min(maxW - 2 * k.panelPadX, n * k.tileW + (n - 1) * k.tileGap);
                gridH = tileH, contentH = gridH;
                st.scrollMax = std::max(0.0f, n * k.tileW + (n - 1) * k.tileGap - innerW);
            } else {
                // width: max-content (the tabs row or every tile in one row) up to the max-width; tiles wrap left-aligned
                const float avail = maxW - 2 * k.panelPadX;
                innerW = std::min(avail, std::max(tabsW, n * k.tileW + (n - 1) * k.tileGap));
                cols = std::max(1, (int)((innerW + k.tileGap) / (k.tileW + k.tileGap)));
                const int rows = (n + cols - 1) / std::max(cols, 1);
                contentH = rows * tileH + (rows - 1) * k.tileGap;
                gridH = std::min(contentH, k.maxH);
                st.scrollMax = std::max(0.0f, contentH - gridH);
            }
            innerW = std::max(innerW, std::min(tabsW, maxW - 2 * k.panelPadX));
            const float pw = innerW + 2 * k.panelPadX, ph = (tabs ? tabH + 6 : 0) + gridH + 2 * k.panelPadY;
            const float cx = (W + SL) * 0.5f, cy = H - k.panelBottom - ph * 0.5f + lift; // [safe area] SL
            const float px = cx - pw * scale * 0.5f, py = cy - ph * scale * 0.5f;
            P.alpha = a;
            P.Glass(px, py, pw * scale, ph * scale, k.panelR * scale, kPanel, 22, 1.6f, false);
            // contents at the scaled origin
            auto X = [&](float lx) { return px + lx * scale; };
            auto Y = [&](float ly) { return py + ly * scale; };
            float ty = k.panelPadY;
            if (tabs) {
                float tx = k.panelPadX + std::max(0.0f, (innerW - tabsW) * 0.5f) * 0;
                int ti = 0;
                auto tab = [&](const Group& t, bool on, int which, bool isSub) {
                    const float w = tabW[ti++];
                    const Rect hr = R(X(tx), Y(ty), w * scale, tabH * scale);
                    if (!leaving && T.Tap(hr)) {
                        sfx(1.2f);
                        if (isSub) st.sub = which; else st.grp = which, st.sub = 0;
                        st.scroll = 0;
                    }
                    P.Box(X(tx), Y(ty), w * scale, tabH * scale, tabH * 0.5f * scale, on ? kAccent : kChip);
                    P.Icon(t.icon, X(tx + k.tabPadX + k.tabIcon * 0.5f), Y(ty + tabH * 0.5f), k.tabIcon * scale, kInk, !on);
                    P.Text(t.name, X(tx + k.tabPadX + k.tabIcon + 5), Y(ty + tabH * 0.5f), k.tabFont * scale, kInk);
                    tx += w + 4;
                };
                for (int i = 0; i < (int)c.groups.size(); ++i) tab(c.groups[i], i == st.grp && !sg, i, false);
                if (sg) for (int i = 0; i < (int)gr.subs.size(); ++i) tab(gr.subs[i], i == st.sub, i, true);
                ty += tabH + 6;
            }
            const Rect clip = R(X(k.panelPadX), Y(ty), innerW * scale, gridH * scale);
            st.scrollArea = clip;
            P.dl.SetClip(clip.x, clip.y, clip.x + clip.w, clip.y + clip.h);
            for (int i = 0; i < n; ++i) {
                const Item& it = items[i];
                const int col = phoneLayout ? i : i % cols, row = phoneLayout ? 0 : i / cols;
                float lx = k.panelPadX + col * (k.tileW + k.tileGap), ly = ty + row * (tileH + k.tileGap);
                if (phoneLayout) lx -= st.scroll; else ly -= st.scroll;
                const Rect hr = R(X(lx), Y(ly), k.tileW * scale, tileH * scale);
                bool held = false;
                if (!leaving && hr.y + hr.h > clip.y && hr.y < clip.y + clip.h && T.Tap(hr, &held) && clip.Contains(in.mouseX, in.mouseY)) {
                    sfx(it.on ? 1.15f : 1.45f);
                    out.pickItem = it.id;
                }
                const float k2 = held ? 0.96f : 1.0f;
                const float tw = k.tileW * k2 * scale, th = tileH * k2 * scale, tx0 = X(lx) + (k.tileW * scale - tw) * 0.5f, ty0 = Y(ly) + (tileH * scale - th) * 0.5f;
                P.Box(tx0, ty0, tw, th, k.tileR * scale, it.on ? Rgba(10, 132, 255, 70) : Rgba(255, 255, 255, 13), 1, it.on ? kAccent : Rgba(255, 255, 255, 31));
                P.Preview(it, tx0 + (tw - k.pvW * scale * k2) * 0.5f, ty0 + 6 * scale, k.pvW * scale * k2, k.pvH * scale * k2);
                const float ny = ty0 + (6 + k.pvH + 2 + k.tileFont * 0.62f) * scale;
                P.Text(it.name, tx0 + tw * 0.5f, ny, k.tileFont * scale, kBarInk, true, 0.5f, (k.tileW - 8) * scale);
                P.Text(it.cost.empty() ? "-" : it.cost, tx0 + tw * 0.5f, ny + (k.tileFont * 0.62f + 2 + k.costFont * 0.62f) * scale, k.costFont * scale, Fade(kBarInk, 0.7f), false, 0.5f);
            }
            P.dl.ClearClip();
            P.alpha = 1;
        }

        // ---- the placement bar (above the dock)
        if (toolOn && st.cat < 0 && !m.hidePlaceBar) { // [road ux] hidePlaceBar
            const float a = EaseOut(std::min(st.toolT / 0.25f, 1.0f));
            P.alpha = a;
            const float bh = 50, nameW = P.TextW(m.tool, 14, true), costW = P.TextW(m.toolCost, 12);
            const int nopt = std::min((int)m.toolOpts.size(), phoneLayout ? 2 : 5);
            const int nbtn = (m.modeIcon.empty() ? 0 : 1) + nopt + 3;
            const float w = 14 + 22 + 6 + nameW + 2 + 6 + costW + 8 + 6 + nbtn * (38 + 6) - 6 + 6;
            const float x = (W + SL - w) * 0.5f, y = H - k.placeBottom - bh + 24 * (1 - a); // [safe area] SL
            out.bottomH = std::max(out.bottomH, in.viewH / s - y);                       // [road ux]
            P.Glass(x, y, w, bh, 18, kPanel, 22, 1.0f, false);
            float cx = x + 14;
            P.Icon(m.toolIcon, cx + 11, y + bh * 0.5f, 22);
            cx += 22 + 6;
            P.Text(m.tool, cx, y + bh * 0.5f, 14, kInk, true);
            cx += nameW + 2 + 6;
            P.Text(m.toolCost, cx, y + bh * 0.5f, 12, kMuted);
            cx += costW + 8 + 6;
            auto btn = [&](const std::string& icon, Color fill, bool ok) {
                const Rect hr = R(cx, y + 6, 38, 38);
                bool held = false;
                const bool tap = T.Tap(hr, &held);
                P.Box(cx, y + 6, 38, 38, 14, held ? Mix(fill, kInk, 0.15f) : fill);
                P.Icon(icon, cx + 19, y + 25, 22, kInk, !ok);
                cx += 38 + 6;
                return tap;
            };
            if (!m.modeIcon.empty() && btn(m.modeIcon, kChip, false)) out.mode = true, sfx(1.25f);
            for (int i = 0; i < nopt; ++i) // [interact] tinted white (the native rotate cell is slate grey: it read as disabled), lit when on
                if (btn(m.toolOpts[i].second, i < (int)m.toolOptOn.size() && m.toolOptOn[i] ? kAccent : kChip, true)) out.opt = i, sfx(1.25f);
            if (btn("undo", kChip, false)) out.undo = true, sfx(0.65f);
            if (btn("x", kChip, false)) out.cancel = true, sfx(0.65f);
            if (btn("ok", kAccent, true)) out.confirm = true, sfx(0.7f);
            P.alpha = 1;
        }

        // ---- the bar
        P.dl.Shadow(bx * s, (by + 10) * s, (bx + bw) * s, (by + barH + 10) * s, rad * s, 15 * s, Rgba(0, 0, 0, 68));
        P.Glass(bx, by, bw, barH, rad, kPanel, 22, 1.6f, false);
        const float midY = by + barH * 0.5f;
        // left cluster: emblem, the three pills, date + pause + speed (fixed size: only re-centred vertically)
        float lx = bx + k.pad;
        P.Box(lx, midY - k.emb * 0.5f, k.emb, k.emb, k.emb * 0.5f, Rgba(255, 255, 255, 24));
        P.Icon("ppl", lx + k.emb * 0.5f, midY, k.embIcon);
        lx += k.emb + k.gap;
        const float pillsH = 3 * k.pillH + 2 * k.pillGap;
        const char* const vals[3] = {m.money.c_str(), m.pop.c_str(), m.happy.c_str()};
        const float fills[3] = {m.moneyBar, m.popBar, m.happyBar};
        const Color pc3[3] = {Rgba(74, 222, 128, 191), Rgba(96, 165, 250, 191), Rgba(250, 204, 21, 191)};
        for (int i = 0; i < 3; ++i) {
            const float py = midY - pillsH * 0.5f + i * (k.pillH + k.pillGap);
            P.Box(lx, py, k.barsW, k.pillH, k.pillH * 0.5f, Rgba(0, 0, 0, 85));
            P.dl.SetClip(lx * s, py * s, (lx + k.barsW * std::clamp(fills[i], 0.0f, 1.0f)) * s, (py + k.pillH) * s);
            P.Box(lx, py, k.barsW, k.pillH, k.pillH * 0.5f, pc3[i]);
            P.dl.ClearClip();
            P.Text(vals[i], lx + (phoneLayout ? 3 : 6), py + k.pillH * 0.5f, k.pillFont, kBarInk);
        }
        lx += k.barsW + k.gap;
        {
            const float spdH = k.spdFont + 2 * k.spdPadY + 2, dateH = k.dtFont * 1.2f, dtGap = k.dtGap + (shrink > 0 ? -2 * shrink * (phoneLayout ? 0 : 1) : 0);
            const float colH = dateH + dtGap + spdH, cy0 = midY - colH * 0.5f;
            P.Text(m.date, lx, cy0 + dateH * 0.5f, k.dtFont, kBarInk);
            const float sy = cy0 + dateH + dtGap;
            const float pauseW = 12 + 2 * k.spdPadX;
            bool held = false;
            if (T.Tap(R(lx, sy, pauseW, spdH), &held)) out.toggledPause = true, sfx(m.paused ? 0.8f : 0.55f);
            P.Box(lx, sy, pauseW, spdH, 6, m.paused ? Rgba(74, 222, 128) : Rgba(255, 255, 255, 20));
            P.Icon(m.paused ? "play" : "pause", lx + pauseW * 0.5f, sy + spdH * 0.5f, 12, m.paused ? Rgba(4, 33, 15) : kBarInk, false);
            const float sx = lx + pauseW + 3;
            if (T.Tap(R(sx, sy, k.spdW, spdH))) out.setSpeed = true, out.speed = m.speed % 3 + 1, sfx(1.35f);
            P.Box(sx, sy, k.spdW, spdH, 6, Rgba(255, 255, 255, 20));
            const float aw = k.spdFont * 0.62f, ah = k.spdFont * 0.68f, gap = -0.5f;
            const float total = m.speed * aw + (m.speed - 1) * gap;
            for (int i = 0; i < m.speed; ++i) P.Arrow(sx + (k.spdW - total) * 0.5f + i * (aw + gap), sy + (spdH - ah) * 0.5f, aw, ah, kInk);
            const float dw = std::max(P.TextW(m.date, k.dtFont), pauseW + 3 + k.spdW);
            lx += dw + k.rr + 2;
        }
        P.Box(lx - 0.5f, midY - (barH - 16) * 0.5f, 1, barH - 16, 0, Rgba(255, 255, 255, 34)); // divider
        const float leftEnd = lx + k.gap;
        // right cluster: alerts (badge), layers, camera, settings
        const char* const rIcons[5] = {"bulldoze", "alert", "layers", "cam", "gear"}; // [interact] Bulldoze first
        float rx = bx + bw - k.pad - 5 * k.rBtn - 4 * k.rGap;
        P.Box(rx - k.rr - 0.5f, midY - (barH - 16) * 0.5f, 1, barH - 16, 0, Rgba(255, 255, 255, 34));
        const float rightStart = rx - k.rr - k.gap;
        for (int i = 0; i < 5; ++i) {
            const float x = rx + i * (k.rBtn + k.rGap), y = midY - k.rBtn * 0.5f;
            bool held = false;
            if (T.Tap(R(x, y, k.rBtn, k.rBtn), &held)) {
                if (i == 0) out.bulldozePressed = true, sfx(m.bulldozeOn ? 0.75f : 1.2f);
                if (i == 1) out.alertsPressed = true, sfx(1.1f);
                if (i == 2) out.layersPressed = true, sfx(1.15f);
                if (i == 3) out.cameraPressed = true, sfx(1.1f);
                if (i == 4) st.menu = true, st.settings = false, st.menuT = 0, sfx(0.55f);
            }
            const bool lit = i == 0 && m.bulldozeOn;
            P.Box(x, y, k.rBtn, k.rBtn, phoneLayout ? 8 : 12, lit ? Rgba(239, 68, 68, 150) : held ? Rgba(255, 255, 255, 40) : Rgba(255, 255, 255, 20));
            P.Icon(rIcons[i], x + k.rBtn * 0.5f, midY, k.rIcon);
            if (i == 1 && m.alerts > 0) {
                char b[8];
                std::snprintf(b, sizeof(b), "%d", m.alerts);
                const float bw2 = std::max(14.0f, P.TextW(b, 9) + 10);
                P.Box(x + k.rBtn + 3 - bw2, y - 3, bw2, 14, 7, Rgba(239, 68, 68));
                P.Text(b, x + k.rBtn + 3 - bw2 * 0.5f, y + 4, 9, kInk, false, 0.5f);
            }
        }
        // centre: the categories, centred in what the clusters leave; icons only move down as the bar shrinks
        const int nc = (int)m.cats.size();
        const float rowW = nc * k.catW + (nc - 1) * 2;
        float cx0 = leftEnd + std::max(0.0f, (rightStart - leftEnd - rowW) * 0.5f);
        const float labelA = k.labels ? 1 - std::min(1.0f, shrink * 1.4f) : 0;
        const float icon = k.catIcon + (k.catIconSm - k.catIcon) * shrink;
        const float btnPadY = phoneLayout ? 0 : 5 - 2 * shrink;
        const float contentH = icon + (k.labels ? (k.catGap + k.catFont * 1.2f) * labelA : 0);
        for (int i = 0; i < nc; ++i) {
            const Category& c = m.cats[i];
            const float x = cx0 + i * (k.catW + 2);
            const float h = contentH + 2 * btnPadY, y = midY - h * 0.5f;
            const bool on = st.cat == i;
            bool held = false;
            if (T.Tap(R(x, by, k.catW, barH), &held)) openCat(i);
            if (on || held) {
                P.Box(x, y, k.catW, h, 12, on ? Rgba(255, 255, 255, 31) : Rgba(255, 255, 255, 18));
                if (on) { // inset 0 -2px 0 cc: the underline inside the rounded bottom
                    P.dl.SetClip(x * s, (y + h - 2) * s, (x + k.catW) * s, (y + h) * s);
                    P.Box(x, y, k.catW, h, 12, c.col);
                    P.dl.ClearClip();
                }
            }
            const float iy = y + btnPadY + icon * 0.5f;
            P.Glow(c.icon, x + k.catW * 0.5f, iy, icon, c.col);
            P.Icon(c.icon, x + k.catW * 0.5f, iy, icon, c.col);
            if (labelA > 0.01f) {
                P.alpha = labelA;
                P.Text(c.name, x + k.catW * 0.5f, iy + icon * 0.5f + k.catGap + k.catFont * 0.6f, k.catFont, on ? kInk : Rgba(207, 216, 228), false, 0.5f, k.catW);
                P.alpha = 1;
            }
        }
    }
    // ================================================================ Phone UI
    else {
        const float st0 = 14 + ST, sl = 60 + SL; // [safe area] ST, SL
        out.bottomH = in.viewH / s - (H - 16 - 64); // [road ux] the corner buttons / the split controls
        // the status pill
        {
            const float f = 12.5f, ih = 15, padY = 5, padX = 8, gap = 10, itemPad = 7;
            struct Part { std::string icon, text, up; Color upCol; bool alert; };
            const char spdGlyph[4] = {0, 1, 2, 3};
            (void)spdGlyph;
            Part parts[4] = {{"ppl", m.pop, m.popUp, Rgba(94, 234, 212), false},
                             {"coin", m.money, m.moneyUp, Rgba(74, 222, 128), false},
                             {"clock", m.date, "", Rgba(125, 211, 252), false},
                             {"", std::to_string(m.alerts), "", 0, true}};
            float w = padX * 2 - gap;
            float pw[4];
            for (int i = 0; i < 4; ++i) {
                const Part& p = parts[i];
                pw[i] = p.alert ? (7 + 5 + P.TextW(p.text, 11, true) + 2 * itemPad)
                                : (ih + 5 + P.TextW(p.text, f, true) + (p.up.empty() ? 0 : 3 + P.TextW(p.up, 10)) + (i == 2 ? 4 + 14 : 0) + 2 * itemPad);
                w += pw[i] + gap;
            }
            const float h = f * 1.2f + 6 + 2 * padY;
            P.Glass(sl, st0, w, h, h * 0.5f, kPanel, 24, 1.7f, true);
            float x = sl + padX;
            const float cy = st0 + h * 0.5f;
            for (int i = 0; i < 4; ++i) {
                const Part& p = parts[i];
                float px = x + itemPad;
                if (p.alert) {
                    P.Box(x, cy - 10, pw[i], 20, 10, Rgba(255, 69, 58, 34));
                    P.dl.Shadow((px - 1) * s, (cy - 4.5f) * s, (px + 8) * s, (cy + 4.5f) * s, 4.5f * s, 4 * s, Rgba(255, 69, 58, 160));
                    P.Box(px, cy - 3.5f, 7, 7, 3.5f, Rgba(255, 69, 58));
                    P.Text(p.text, px + 12, cy, 11, Rgba(255, 180, 174), true);
                } else {
                    P.Icon(p.icon, px + ih * 0.5f, cy, ih);
                    px += ih + 5;
                    P.Text(p.text, px, cy, f, kInk, true);
                    px += P.TextW(p.text, f, true);
                    if (!p.up.empty()) P.Text(p.up, px + 3, cy, 10, p.upCol), px += 3 + P.TextW(p.up, 10);
                    if (i == 2) { // the sim speed glyph: pause bars or 1-3 arrows
                        px += 4;
                        if (m.paused) P.Box(px + 2, cy - 4, 2.5f, 8, 1, Rgba(125, 211, 252)), P.Box(px + 7, cy - 4, 2.5f, 8, 1, Rgba(125, 211, 252));
                        else for (int a = 0; a < m.speed; ++a) P.Arrow(px + a * 4.5f, cy - 3.5f, 5, 7, Rgba(125, 211, 252));
                    }
                }
                x += pw[i] + gap;
            }
        }
        // corners: pause (left), chevron (right) — hidden while placing
        if (!toolOn) {
            const float y = H - 16 - 48;
            bool held = false;
            if (T.Tap(R(sl, y, 48, 48), &held)) st.menu = true, st.settings = false, st.menuT = 0, sfx(0.55f);
            P.Glass(sl, y, 48, 48, 24, kPanel, 24, 1.7f, true);
            P.Icon("pause", sl + 24, y + 24, 20, kInk, false);
            // [interact] Bulldoze beside pause (the placement controls take this corner while it is on: its cross ends it)
            if (T.Tap(R(sl + 56, y, 48, 48), &held)) out.bulldozePressed = true, sfx(1.2f);
            P.Glass(sl + 56, y, 48, 48, 24, held ? Rgba(60, 62, 70, 200) : kPanel, 24, 1.7f, true);
            P.Icon("bulldoze", sl + 56 + 24, y + 24, 22, kInk, true);
            const float rx = W - 60 - 48; // [safe area] W is the safe right edge
            if (T.Tap(R(rx, y, 48, 48))) {
                st.open = !st.open, st.openT = 0;
                if (!st.open) st.cat = -1;
                sfx(st.open ? 0.9f : 0.75f);
            }
            P.Glass(rx, y, 48, 48, 24, kPanel, 24, 1.7f, true);
            // the chevron turns over .3 s: drawn as two strokes rotated about the centre
            const float turn = st.open ? std::min(st.openT / 0.3f, 1.0f) : 1 - std::min(st.openT / 0.3f, 1.0f);
            const float ang = 3.14159265f * EaseOut(turn), cs = std::cos(ang), sn = std::sin(ang);
            auto rot = [&](float x, float y2) { return std::pair<float, float>{rx + 24 + (x * cs - y2 * sn), y + 24 + (x * sn + y2 * cs)}; };
            const float u = 20.0f / 24.0f;
            auto [ax, ay] = rot(-6 * u, 3 * u);
            auto [bx2, by2] = rot(0, -3 * u);
            auto [cx2, cy2] = rot(6 * u, 3 * u);
            P.dl.Line(ax * s, ay * s, bx2 * s, by2 * s, 1.9f * u * s, kInk);
            P.dl.Line(bx2 * s, by2 * s, cx2 * s, cy2 * s, 1.9f * u * s, kInk);
        }
        // swipe in from the right edge opens; a swipe right closes
        if (in.released && !st.drag) {
            const float dx = (in.mouseX - in.pressX) / s, dy = (in.mouseY - in.pressY) / s;
            if (!st.open && in.pressX / s > W - 130 && dx < -40 && std::fabs(dy) < 60) st.open = true, st.openT = 0, sfx(0.9f);
            else if (st.open && dx > 40 && in.pressX / s > W * 0.5f && std::fabs(dy) < 60) st.open = false, st.cat = -1, st.openT = 0, sfx(0.75f);
        }
        st.scrollArea = {};
        if (st.open && !toolOn) {
            const float a = EaseBack(std::min(st.openT / 0.28f, 1.0f));
            Category* c = st.cat >= 0 ? &m.cats[st.cat] : nullptr;
            // left: the category grid, or (drilled in) the groups + back; wrap-reverse rows from the bottom-left
            {
                const bool groups = c && c->groups.size() > 1;
                const int n = groups ? (int)c->groups.size() + 1 : (int)m.cats.size();
                const float bwid = groups ? 84 : 52, bh = groups ? 52 : 46, gap = 6, pad = 8;
                const int perRow = groups ? 3 : 3;
                const int rows = (n + perRow - 1) / perRow;
                const float gw = groups ? 282 : 196, gh = rows * bh + (rows - 1) * gap + 2 * pad;
                const float gx = 52 + SL, gy = H - 74 - gh + 24 * (1 - a); // [safe area] SL
                P.alpha = std::clamp(a, 0.0f, 1.0f);
                P.Glass(gx, gy, gw, gh, 24, kPanel, 24, 1.7f, true);
                for (int i = 0; i < n; ++i) {
                    const int row = i / perRow, col = i % perRow;
                    const float x = gx + pad + col * (bwid + gap), y = gy + gh - pad - (row + 1) * bh - row * gap;
                    bool held = false;
                    const bool tap = T.Tap(R(x, y, bwid, bh), &held);
                    if (groups) {
                        const bool back = i == n - 1;
                        const Group* gp = back ? nullptr : &c->groups[i];
                        const bool on = !back && st.grp == i;
                        if (tap) {
                            if (back) st.cat = -1, sfx(0.65f);
                            else st.grp = i, st.sub = 0, st.scroll = 0, sfx(1.2f);
                        }
                        if (on) {
                            P.dl.Shadow(x * s, y * s, (x + bwid) * s, (y + bh) * s, 15 * s, 18 * s * 0.5f, Fade(c->col, 0.45f * P.alpha));
                            P.Box(x, y, bwid, bh, 15, Fade(c->col, 0.35f), 1.5f, c->col);
                        } else if (back) P.Box(x, y, bwid, bh, 15, Rgba(255, 255, 255, 20));
                        else if (held) P.Box(x, y, bwid, bh, 15, Rgba(255, 255, 255, 20));
                        const std::string icon = back ? "left" : gp->icon, label = back ? c->name : gp->name;
                        const float textH = 10 * 1.05f, cy = y + bh * 0.5f - (22 + 2 + textH) * 0.5f;
                        P.Icon(icon, x + bwid * 0.5f, cy + 11, 22, kInk, !on);
                        P.Text(label, x + bwid * 0.5f, cy + 22 + 2 + textH * 0.5f, 10, Rgba(255, 255, 255, 208), false, 0.5f, bwid - 4);
                    } else {
                        const Category& k = m.cats[i];
                        const bool on = st.cat == i;
                        if (tap) openCat(i);
                        if (on) {
                            P.dl.Shadow(x * s, y * s, (x + bwid) * s, (y + bh) * s, 15 * s, 18 * s * 0.5f, Fade(k.col, 0.45f * P.alpha));
                            P.Box(x, y, bwid, bh, 15, Fade(k.col, 0.35f), 1.5f, k.col);
                        } else if (held) P.Box(x, y, bwid, bh, 15, Rgba(255, 255, 255, 20));
                        P.Icon(k.icon, x + bwid * 0.5f, y + bh * 0.5f, 22, kInk, !on);
                    }
                }
                P.alpha = 1;
            }
            // right: sub tabs, then the item column (rows of two from the bottom, the first item at the right thumb)
            if (c) {
                Group& gr = c->groups[std::min(st.grp, (int)c->groups.size() - 1)];
                Group* sg = gr.subs.empty() ? nullptr : &gr.subs[std::min(st.sub, (int)gr.subs.size() - 1)];
                std::vector<Item>& items = sg ? sg->items : gr.items;
                const float ia = std::clamp(EaseDock(std::min(st.catT / 0.28f, 1.0f)), 0.0f, 1.0f);
                P.alpha = ia;
                const float colW = 196, colX = W - 52 - colW + 30 * (1 - ia);
                if (sg) {
                    const float tx = colX, ty = H - 76 - 52, tw = colW, th = 52;
                    P.Glass(tx, ty, tw, th, 18, kPanel, 24, 1.7f, true);
                    const int nt = (int)gr.subs.size();
                    const float bw2 = (tw - 8 - (nt - 1) * 4) / nt;
                    for (int i = 0; i < nt; ++i) {
                        const float x = tx + 4 + i * (bw2 + 4);
                        if (T.Tap(R(x, ty + 4, bw2, 44))) st.sub = i, st.scroll = 0, sfx(1.2f);
                        if (i == st.sub) P.Box(x, ty + 4, bw2, 44, 14, Rgba(255, 255, 255, 38), 1.5f, Rgba(255, 255, 255, 85));
                        const float lw = 22 + 6 + P.TextW(gr.subs[i].name, 12, true);
                        P.Icon(gr.subs[i].icon, x + (bw2 - lw) * 0.5f + 11, ty + 26, 22);
                        P.Text(gr.subs[i].name, x + (bw2 - lw) * 0.5f + 28, ty + 26, 12, kInk, true);
                    }
                }
                const float bottom = sg ? 132 : 76, maxH = std::min(sg ? 230.0f : 290.0f, H - bottom - ST - 8), pad = 6, gap = 6, tileH = 100; // [safe area] ST
                const int rows = ((int)items.size() + 1) / 2;
                const float contentH = rows * tileH + (rows - 1) * gap;
                const float boxH = std::min(contentH, maxH - 2 * pad) + 2 * pad;
                const float by = H - bottom - boxH;
                P.Glass(colX, by, colW, boxH, 22, kPanel, 24, 1.7f, true);
                const Rect clip = R(colX + pad, by + pad, colW - 2 * pad, boxH - 2 * pad);
                st.scrollArea = clip;
                st.scrollMax = std::max(0.0f, contentH - (boxH - 2 * pad));
                P.dl.SetClip(clip.x, clip.y, clip.x + clip.w, clip.y + clip.h);
                const float tw = (colW - 2 * pad - gap) * 0.5f;
                for (int i = 0; i < (int)items.size(); ++i) {
                    const Item& it = items[i];
                    const int row = i / 2, col = i % 2; // row-reverse: the first of a pair on the right
                    const float x = colX + pad + (col == 0 ? tw + gap : 0), y = by + boxH - pad - (row + 1) * tileH - row * gap + st.scroll;
                    bool held = false;
                    if (T.Tap(R(x, y, tw, tileH), &held) && clip.Contains(in.mouseX, in.mouseY)) {
                        sfx(it.on ? 1.15f : 1.45f);
                        out.pickItem = it.id;
                    }
                    const float k2 = held ? 0.96f : 1, w2 = tw * k2, h2 = tileH * k2, x2 = x + (tw - w2) * 0.5f, y2 = y + (tileH - h2) * 0.5f;
                    P.BoxV(x2, y2, w2, h2, 15, it.on ? Rgba(10, 132, 255, 90) : Rgba(255, 255, 255, 24), it.on ? Rgba(10, 132, 255, 50) : Rgba(255, 255, 255, 8));
                    P.Preview(it, x2 + (w2 - 62 * k2) * 0.5f, y2 + 6, 62 * k2, 62 * k2);
                    P.Text(it.name, x2 + w2 * 0.5f, y2 + 6 + 62 + 1 + 6, 10.5f, kInk, false, 0.5f, w2 - 6);
                    if (!it.cost.empty()) P.Text(it.cost, x2 + w2 * 0.5f, y2 + 6 + 62 + 1 + 12 + 6, 9.5f, kMuted, false, 0.5f);
                }
                P.dl.ClearClip();
                P.alpha = 1;
            }
        }
        // the split placement controls: the name pill at the bottom centre, ✕ / undo / option left, ✓ / mode / rotate right
        if (toolOn && m.hidePlaceBar) out.bottomH = in.viewH / s - H; // [road ux] the kit's own controls
        if (toolOn && !m.hidePlaceBar) {                               // [road ux] hidePlaceBar
            const float a = EaseOut(std::min(st.toolT / 0.25f, 1.0f));
            P.alpha = a;
            const float lift = 24 * (1 - a);
            {
                std::string sub = m.toolCost + (m.modeName.empty() ? "" : (m.toolCost.empty() ? "" : " - ") + m.modeName);
                const float w = 14 + 18 + 8 + P.TextW(m.tool, 12.5f, true) + (sub.empty() ? 0 : 8 + P.TextW(sub, 12.5f)) + 14, h = 30;
                const float x = (W + SL - w) * 0.5f, y = H - 24 - h + lift; // [safe area] SL
                P.Glass(x, y, w, h, 15, kPanel, 18, 1.0f, false);
                P.Icon(m.toolIcon, x + 14 + 9, y + h * 0.5f, 18);
                P.Text(m.tool, x + 14 + 18 + 8, y + h * 0.5f, 12.5f, kInk, true);
                if (!sub.empty()) P.Text(sub, x + 14 + 18 + 8 + P.TextW(m.tool, 12.5f, true) + 8, y + h * 0.5f, 12.5f, kMuted);
            }
            auto sb = [&](float x, float y, float size, float r, const std::string& icon, bool big, bool on = false) { // on: [interact] a lit toggle
                bool held = false;
                const bool tap = T.Tap(R(x, y, size, size), &held);
                if (big) {
                    P.dl.Shadow(x * s, (y + 6) * s, (x + size) * s, (y + size + 6) * s, r * s, 10 * s, Fade(Rgba(0, 0, 0, 85), P.alpha));
                    P.Box(x, y, size, size, r, held ? Mix(kAccent, kInk, 0.15f) : kAccent);
                } else P.Glass(x, y, size, size, r, held ? Rgba(60, 62, 70, 200) : on ? Rgba(10, 132, 255, 170) : kPanel, 18, 1.0f, false);
                P.Icon(icon, x + size * 0.5f, y + size * 0.5f, big ? 28 : 20, kInk, false);
                return tap;
            };
            float y = H - 16 - 48 + lift;
            float x = 58 + SL; // [safe area] SL
            if (sb(x, y, 48, 16, "x", false)) out.cancel = true, sfx(0.65f);
            x += 56;
            if (sb(x, y, 48, 16, "undo", false)) out.undo = true, sfx(0.65f);
            x += 56;
            auto optOn = [&](int i) { return i < (int)m.toolOptOn.size() && m.toolOptOn[(size_t)i]; }; // [interact]
            if (!m.toolOpts.empty() && sb(x, y, 48, 16, m.toolOpts[0].second, false, optOn(0))) out.opt = 0, sfx(1.25f);
            float rx = W - 58 - 64;
            if (sb(rx, H - 16 - 64 + lift, 64, 22, "ok", true)) out.confirm = true, sfx(0.7f);
            rx -= 8 + 48;
            if (!m.modeIcon.empty()) {
                if (sb(rx, y, 48, 16, m.modeIcon, false)) out.mode = true, sfx(1.25f);
                rx -= 8 + 48;
            }
            if (m.toolOpts.size() > 1 && sb(rx, y, 48, 16, m.toolOpts[1].second, false, optOn(1))) out.opt = 1, sfx(1.25f);
            P.alpha = 1;
        }
    }

    MenuFrame(g, st, m, atlas, out, false); // [title]
}

// [title] The pause menu and Settings on their own: the title screen (gui/title_screen.h) shows Settings without the dock.
// standalone: called by itself (not from Frame): it keeps the menu's clock.
inline void MenuFrame(Context& g, State& st, Model& m, const Atlas& atlas, Out& out, bool standalone) {
    const Input& in = g.In();
    const float s = g.Scale();
    Painter P(g, atlas, s);
    P.text = (!st.pcUi && st.pad ? 1.3f : 1.0f) * (m.textSize ? 1.15f : 1.0f);
    const float SL = m.safe.L(), ST = m.safe.T();
    const float W = in.viewW / s - m.safe.R(), H = in.viewH / s - m.safe.B();
    Tapper T{g, st};
    auto R = [&](float x, float y, float w, float h) { return Rect{x * s, y * s, w * s, h * s}; };
    auto sfx = [&](float pitch) { if (m.sound) out.sfx.push_back(pitch); };
    if (standalone) st.menuT += in.dt;
    // ================================================================ pause menu / settings (frosted scrim, centred card)
    out.menuOpen = st.menu;
    if (st.menu) {
        const float a = EaseOut(std::min(st.menuT / 0.25f, 1.0f));
        const bool padSizes = st.pcUi && st.pad;
        P.alpha = a;
        P.dl.Glass(0, 0, in.viewW, in.viewH, 0, Rgba(0, 0, 0, 102), 14 * s, 0.8f, 0, 0);
        g.Block({0, 0, in.viewW, in.viewH});
        static const char* const kOnOff[] = {"Off", "On"};
        static const char* const kPreset[] = {"Low", "Med", "High", "Ultra"};
        static const char* const kPresetC[] = {"Low", "Med", "High", "Ultra", "Custom"};
        static const char* const kRes[] = {tbd::gfx::ScaleLabel(0), tbd::gfx::ScaleLabel(1), tbd::gfx::ScaleLabel(2), tbd::gfx::ScaleLabel(3), tbd::gfx::ScaleLabel(4)};
        static const char* const kTier[] = {"Low", "L/M", "Med", "High", "Ultra"};
        static const char* const kDist[] = {"Near", "Med", "Far"};
        static const char* const kLight[] = {"Full", "Fast", "Unlit"};
        static const char* const kText[] = {"Normal", "Large"};
        static const char* const kVol[] = {"Off", "25%", "50%", "75%", "100%"};
        static const char* const kVolName[] = {"Master", "Music", "Ambience", "Effects", "Radio"};
        static const char* const kVolIcon[] = {"star", "play", "layers", "gear", "info"};
        // A settings row is the icon, its label and a segmented choice on the right: the card is as wide as the widest row
        // needs (the touch layout's 1.3x and Large text's 1.15x text made the Graphics rows' choices run over their labels).
        auto rowNeed = [&](const char* label, const char* const* opts, int n) {
            float w = 44 + P.TextW(label, 13) + 12 + 6;
            for (int i = 0; i < n; ++i) w += P.TextW(opts[i], 11, true) + 16;
            return w;
        };
        float cardW = padSizes ? 340 : (st.gfx || st.snd ? 330 : 260);
        if (st.settings) {
            float need = 0;
            if (st.snd) {
                need = rowNeed("Ambience", kVol, 5);
            } else if (st.gfx) {
                need = std::max({rowNeed("Preset", kPresetC, 5), rowNeed("Resolution", kRes, 5), rowNeed("Effects", kTier, 5), rowNeed("Buildings", kTier, 5),
                                 rowNeed("Draw distance", kDist, 3), rowNeed("Sun shadows", kOnOff, 2), rowNeed("Lighting mode", kLight, 3),
                                 rowNeed("Show stats", kOnOff, 2)});
            } else {
                need = std::max({rowNeed("Graphics", kPreset, 4), rowNeed("Text size", kText, 2), rowNeed("Edge pan", kOnOff, 2)});
            }
            cardW = std::min(std::max(cardW, need), W - SL - 24); // [safe area] SL
        }
        const float CX = (W + SL) * 0.5f; // [safe area] the card's centre: the safe area's
        const float gap = padSizes ? 8 : 6, bh = (padSizes ? 14 : 11) * 2 + 14 * P.text * 1.25f;
        const float popScale = 0.6f + 0.4f * a; // pop: scale(.6) -> 1
        auto btnRect = [&](float x, float y, float w, float h) { return R(CX + (x - CX) * popScale, H * 0.5f + (y - H * 0.5f) * popScale, w * popScale, h * popScale); };
        auto drawBtn = [&](float x, float y, float w, float h, const std::string& icon, const std::string& label, int style, bool centre) {
            // style 0 normal, 1 primary, 2 muted (main menu), 3 selected card
            const Rect r = btnRect(x, y, w, h);
            bool held = false;
            const bool tap = T.Tap(r, &held);
            const float X0 = r.x / s, Y0 = r.y / s, Wd = r.w / s, Hd = r.h / s;
            const Color fill = style == 1 ? kAccent : style == 3 ? Rgba(74, 222, 128, 46) : Rgba(255, 255, 255, 31);
            P.Box(X0, Y0, Wd, Hd, 18 * popScale, held ? Mix(fill, kInk, 0.12f) : fill, style == 1 ? 0 : (style == 3 ? 2 : 1), style == 3 ? Rgba(74, 222, 128) : kBorder);
            const float ts = 14 * popScale;
            const float lw = 22 * popScale + 10 * popScale + P.TextW(label, ts, style == 1);
            float ix = centre ? X0 + (Wd - lw) * 0.5f : X0 + (padSizes ? 18 : 16) * popScale;
            P.Icon(icon, ix + 11 * popScale, Y0 + Hd * 0.5f, 22 * popScale, style == 2 ? kMuted : kInk, style != 1);
            P.Text(label, ix + 32 * popScale, Y0 + Hd * 0.5f, ts, style == 2 ? kMuted : kInk, style == 1);
            return tap;
        };
        auto title = [&](const std::string& t, float y, float size, Color c, bool caps, bool bold) {
            std::string u = t;
            if (caps) for (char& ch : u) ch = (char)std::toupper((unsigned char)ch);
            P.Text(u, CX, H * 0.5f + (y - H * 0.5f) * popScale, size * popScale, c, bold, 0.5f);
        };
        const float x0 = CX - cardW * 0.5f;
        if (!st.settings) {
            const float hTitle = 13 * 1.4f + 6, hCity = (padSizes ? 30 : 22) * 1.3f + 2, hSub = 13 * 1.4f + 2;
            const float total = hTitle + hCity + hSub + bh + gap + 2 * (bh + gap) + bh + gap * 2;
            float y = ST + (H - ST - total) * 0.5f; // [safe area] ST
            title("Paused", y + 9, 13, kMuted, true, false);
            y += hTitle;
            title(m.city, y + hCity * 0.5f, padSizes ? 30 : 22, kInk, false, true);
            y += hCity;
            title(m.pop + " citizens - " + m.money + " - " + m.date, y + hSub * 0.5f - 4, 13, kMuted, false, false);
            y += hSub;
            if (drawBtn(x0, y, cardW, bh, "play", "Resume", 1, true)) st.menu = false, sfx(0.8f);
            y += bh + gap;
            const float hw = (cardW - gap) * 0.5f;
            if (drawBtn(x0, y, hw, bh, "save", "Save", 0, false)) out.save = true, sfx(1.1f);
            if (drawBtn(x0 + hw + gap, y, hw, bh, "folder", "Load", 0, false)) out.load = true, sfx(1.1f);
            y += bh + gap;
            if (drawBtn(x0, y, hw, bh, "cam", "Photo mode", 0, false)) out.photo = true, st.menu = false, sfx(1.1f);
            if (drawBtn(x0 + hw + gap, y, hw, bh, "gear", "Settings", 0, false)) st.settings = true, sfx(1.1f);
            y += bh + gap;
            if (drawBtn(x0, y, cardW, bh, "exit", "Main menu", 2, true)) out.mainMenu = true, sfx(1.1f);
        } else {
            // Settings: the interface switch (two cards), then the options rows, then Back
            const float cardH = 66;
            const float rowH = st.gfx ? std::min(bh, (H - ST - 80.0f) / 10.0f - gap) : bh; // the Graphics page has nine rows
            const float total = st.gfx ? 13 * 1.4f + 6 + 9 * (rowH + gap)
                              : st.snd ? 13 * 1.4f + 6 + 6 * (rowH + gap)
                                       : 13 * 1.4f + 6 + 13 * 1.4f + cardH + gap + 5 * (rowH + gap) + rowH;
            float y = ST + (H - ST - total) * 0.5f; // [safe area] ST
            title(st.gfx ? "Graphics" : st.snd ? "Sound" : "Settings", y + 9, 13, kMuted, true, false);
            y += 13 * 1.4f + 6;
            if (!st.gfx && !st.snd) { title("Interface", y + 6, 13, kMuted, false, false); y += 13 * 1.4f; }
            const float hw = (cardW - 8) * 0.5f;
            auto card = [&](float x, bool on, const std::string& icon, const std::string& name, const std::string& sub) {
                const Rect r = btnRect(x, y, hw, cardH);
                const bool tap = T.Tap(r);
                const float X0 = r.x / s, Y0 = r.y / s, Wd = r.w / s, Hd = r.h / s;
                P.Box(X0, Y0, Wd, Hd, 18 * popScale, on ? Rgba(74, 222, 128, 46) : Rgba(255, 255, 255, 31), on ? 2 : 1, on ? Rgba(74, 222, 128) : kBorder);
                P.Icon(icon, X0 + (10 + 11) * popScale, Y0 + (10 + 11) * popScale, 22 * popScale, kInk, false);
                P.Text(name, X0 + 10 * popScale, Y0 + (10 + 22 + 2 + 8) * popScale, 14 * popScale, kInk, true);
                P.Text(sub, X0 + 10 * popScale, Y0 + (10 + 22 + 2 + 16 + 7) * popScale, 10 * popScale, Fade(kInk, 0.65f));
                return tap;
            };
            if (!st.gfx && !st.snd) {
                if (card(x0, !st.pcUi, "up", "Phone UI", "Hidden dock, thumb reach") && st.pcUi) st.pcUi = false, st.cat = -1, st.open = false, out.settingsChanged = true, sfx(1.3f);
                if (card(x0 + hw + 8, st.pcUi, "gear", "PC UI", "Full CS2-style bar") && !st.pcUi) st.pcUi = true, st.cat = -1, st.open = false, out.settingsChanged = true, sfx(1.3f);
                y += cardH + gap;
            }
            // options: a label and a segmented choice per row
            auto row = [&](const std::string& icon, const std::string& label, const char* const* opts, int n, int cur) {
                const Rect r = btnRect(x0, y, cardW, rowH);
                const float X0 = r.x / s, Y0 = r.y / s, Wd = r.w / s, Hd = r.h / s;
                P.Box(X0, Y0, Wd, Hd, 18 * popScale, Rgba(255, 255, 255, 31), 1, kBorder);
                P.Icon(icon, X0 + 27 * popScale, Y0 + Hd * 0.5f, 20 * popScale, kInk, false);
                P.Text(label, X0 + 44 * popScale, Y0 + Hd * 0.5f, 13 * popScale, kInk);
                int picked = -1;
                float sw = 0;
                for (int i = 0; i < n; ++i) sw += P.TextW(opts[i], 11 * popScale, true) + 16 * popScale;
                float sx = X0 + Wd - 6 * popScale - sw, sh = Hd - 12 * popScale;
                P.Box(sx - 2 * popScale, Y0 + 4 * popScale, sw + 4 * popScale, Hd - 8 * popScale, (Hd - 8 * popScale) * 0.5f, Rgba(0, 0, 0, 70));
                for (int i = 0; i < n; ++i) {
                    const float ow = P.TextW(opts[i], 11 * popScale, true) + 16 * popScale;
                    if (T.Tap(R(sx, Y0 + 6 * popScale, ow, sh))) picked = i;
                    if (i == cur) P.Box(sx, Y0 + 6 * popScale, ow, sh, sh * 0.5f, kAccent);
                    P.Text(opts[i], sx + ow * 0.5f, Y0 + Hd * 0.5f, 11 * popScale, kInk, true, 0.5f);
                    sx += ow;
                }
                y += rowH + gap;
                return picked;
            };
            int v;
            if (st.snd) {
                // Sound: a volume per bus in quarter steps (the host saves percent and applies them, glass_dock_host.inl)
                for (int i = 0; i < 5; ++i) {
                    const int cur = std::clamp((m.volume[i] + 12) / 25, 0, 4);
                    if ((v = row(kVolIcon[i], kVolName[i], kVol, 5, cur)) >= 0 && v != cur) {
                        m.volume[i] = v * 25, out.settingsChanged = true;
                        if (i == 3) m.sound = v > 0; // the UI ticks follow Effects
                        sfx(0.9f + 0.1f * v);
                    }
                }
                if (drawBtn(x0, y, cardW, rowH, "left", "Back", 1, true)) st.snd = false, sfx(0.65f);
            } else if (st.gfx) {
                // Graphics: a preset, or every option on its own. Touching any option makes the preset Custom.
                tbd::gfx::Settings& G = m.gfx;
                auto custom = [&](int& field, int val) { field = val, G.preset = tbd::gfx::kCustom, out.settingsChanged = true, sfx(1.3f); };
                if ((v = row("layers", "Preset", kPresetC, 5, G.preset)) >= 0) {
                    if (v < 4) tbd::gfx::ApplyPreset(G, v);
                    else G.preset = tbd::gfx::kCustom;
                    out.settingsChanged = true, sfx(1.3f);
                }
                if ((v = row("cam", "Resolution", kRes, 5, G.scale)) >= 0) custom(G.scale, v);
                if ((v = row("star", "Effects", kTier, 5, G.look)) >= 0) custom(G.look, v);
                if ((v = row("info", "Buildings", kTier, 5, G.detail)) >= 0) custom(G.detail, v);
                if ((v = row("srch", "Draw distance", kDist, 3, G.distance)) >= 0) custom(G.distance, v);
                if ((v = row("play", "Sun shadows", kOnOff, 2, G.shadows)) >= 0) custom(G.shadows, v);
                if ((v = row("gear", "Lighting mode", kLight, 3, G.lighting)) >= 0) custom(G.lighting, v);
                // developer: the stats text (off by default on iOS and the Phone UI); not a graphics option, so the preset stays
                if ((v = row("stats", "Show stats", kOnOff, 2, m.showStats ? 1 : 0)) >= 0) m.showStats = v == 1, out.settingsChanged = true, sfx(1.3f);
                if (drawBtn(x0, y, cardW, rowH, "left", "Back", 1, true)) st.gfx = false, sfx(0.65f);
            } else {
                if (drawBtn(x0, y, cardW, rowH, "star", m.volume[0] == 0 ? "Sound (muted)" : "Sound and music", 0, false)) st.snd = true, sfx(1.1f);
                y += rowH + gap;
                if ((v = row("layers", "Graphics", kPreset, 4, m.gfx.preset)) >= 0) tbd::gfx::ApplyPreset(m.gfx, v), out.settingsChanged = true, sfx(1.3f);
                if (drawBtn(x0, y, cardW, rowH, "gear", m.gfx.preset == tbd::gfx::kCustom ? "Graphics options (Custom)" : "Graphics options", 0, false)) st.gfx = true, sfx(1.1f);
                y += rowH + gap;
                if ((v = row("info", "Text size", kText, 2, m.textSize)) >= 0) m.textSize = v, out.settingsChanged = true, sfx(1.3f);
                if ((v = row("srch", "Edge pan", kOnOff, 2, m.edgePan ? 1 : 0)) >= 0) m.edgePan = v == 1, out.settingsChanged = true, sfx(1.3f);
                if (drawBtn(x0, y, cardW, rowH, "left", "Back", 1, true)) st.settings = false, sfx(0.65f);
            }
        }
        P.alpha = 1;
    }
}

} // namespace dock
} // namespace gui
} // namespace tbd
