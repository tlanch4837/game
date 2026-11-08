// The front end in the Glass Dock+ look: the black loading screen (the Optima Engine logo, a thin progress bar and the
// loading step under it), the title screen (the logo over a dark backdrop, glass buttons: Continue, New Game, Load Game,
// Settings, Quit on PC) and the ten-slot save / load picker (glass rows, Empty for an empty slot, tap to load or save, a
// trash button to delete, a confirm card before anything is overwritten, deleted or left unsaved).
// Platform free: the host (game/title_host.inl) fills Model, acts on Out and draws the Settings pages with the dock's
// own (dock::MenuFrame). The logo is a cell of the GUI image atlas ("logo", appended by the host from
// assets_src/branding/optima_engine_1024.png). Sizes are points (Context::Scale() pixels each); every tap target is at
// least 44 pt and every panel keeps out of the safe area (tests/title_screen_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "glass_dock.h"

namespace tbd {
namespace gui {
namespace title {

enum class Screen { None, Splash, Title, Slots, Settings, Loading };
enum class Pick { Load, Save };
enum class Ask { None, Overwrite, Delete, LoadDiscard, NewDiscard };

struct SlotRow {
    bool used = false;
    std::string name;   // "Bayside"
    std::string when;   // "Nov 8 2026, 14:32 - Mar 4, Y1"
    std::string stats;  // "1,234 citizens - $1.50M"
    bool current = false; // the city being played came from / was saved to this slot
    bool example = false; // the bundled example city
};

struct Model {
    std::vector<SlotRow> slots = std::vector<SlotRow>(10);
    int continueSlot = -1;     // the most recent save (-1: none, Continue is disabled)
    std::string continueLine;  // under Continue: "Bayside - Nov 8, 14:32"
    bool canQuit = true;       // PC only (iOS apps do not quit)
    bool inGame = false;       // a city is being played (the title came from the pause menu): leaving it asks first
    bool backdrop = false;     // the city is drawn behind the title (a lighter scrim)
    float progress = 0;        // loading: 0..1
    std::string step;          // loading: what is happening ("Building the terrain")
    std::string version;       // bottom corner of the title screen
    SafeInsets safe;
    bool sound = true;
    bool pad = true;           // iPad / PC sizes (false: iPhone)
    float text = 1.0f;         // Large text: 1.15
    int logoCell = -1;         // the atlas cell of the logo (-1: drawn as text)
};

struct State {
    Screen screen = Screen::None;
    Screen back = Screen::Title; // where the picker / Settings return (None: the pause menu, in game)
    Pick pick = Pick::Load;
    Ask ask = Ask::None;
    int askSlot = -1;
    float t = 0;          // seconds in this screen
    float leaveT = -1;    // >= 0: fading out to the game (seconds)
    float shown = 0;      // the overlay's fade in, 0..1
    void Go(Screen s) { screen = s, t = 0, leaveT = -1, ask = Ask::None, askSlot = -1; }
};

struct Out {
    bool cont = false, newGame = false, quit = false, settings = false, back = false;
    bool left = false;     // the fade out to the game finished: the host hides the front end
    int load = -1, save = -1, del = -1;
    std::vector<float> sfx; // the UI tick's pitches (as dock::Out::sfx)
    std::vector<Rect> hits; // every tap target drawn this frame, points (tests)
};

constexpr float kFadeOut = 0.5f;

inline void Frame(Context& g, State& st, const Model& m, const dock::Atlas& atlas, Out& out) {
    out = Out{};
    if (st.screen == Screen::None) return;
    const Input& in = g.In();
    const float s = g.Scale();
    dock::Painter P(g, atlas, s);
    P.text = m.text;
    const float W = in.viewW / s, H = in.viewH / s;
    const float SL = m.safe.L(), ST = m.safe.T(), SR = m.safe.R(), SB = m.safe.B();
    const float AW = W - SL - SR, AH = H - ST - SB, CX = SL + AW * 0.5f, CY = ST + AH * 0.5f;
    st.t += in.dt;
    if (st.leaveT >= 0) st.leaveT += in.dt;
    const float leave = st.leaveT >= 0 ? std::clamp(1.0f - st.leaveT / kFadeOut, 0.0f, 1.0f) : 1.0f;
    if (st.leaveT >= kFadeOut) out.left = true;
    auto sfx = [&](float p) { if (m.sound) out.sfx.push_back(p); };
    auto R = [&](float x, float y, float w, float h) { return Rect{x * s, y * s, w * s, h * s}; };
    bool modal = false; // the confirm card is being drawn: only its buttons take taps while it is up
    auto tap = [&](float x, float y, float w, float h, bool* held) {
        out.hits.push_back({x, y, w, h});
        if (st.ask != Ask::None && !modal) { if (held) *held = false; return false; }
        const Rect r = R(x, y, w, h);
        const bool inside = r.Contains(in.mouseX, in.mouseY), pressedIn = r.Contains(in.pressX, in.pressY);
        if (held) *held = in.down && pressedIn && inside;
        return in.released && pressedIn && inside;
    };
    // the logo: the atlas cell, `w` points wide, centred on (cx, cy)
    const float aspect = m.logoCell >= 0 ? atlas.cells[m.logoCell].w / std::max(1.0f, atlas.cells[m.logoCell].h) : 976.0f / 682.0f;
    auto logo = [&](float cx, float cy, float w, float a) {
        const float h = w / aspect;
        if (m.logoCell >= 0) {
            const dock::Sprite& sp = atlas.cells[m.logoCell];
            P.dl.Image((cx - w / 2) * s, (cy - h / 2) * s, (cx + w / 2) * s, (cy + h / 2) * s, sp.u0, sp.v0, sp.u1, sp.v1, Fade(dock::kInk, a * P.alpha));
        } else {
            const float keep = P.alpha;
            P.alpha = keep * a;
            P.Text("OPTIMA", cx, cy - h * 0.1f, h * 0.22f, dock::kInk, true, 0.5f);
            P.Text("E N G I N E", cx, cy + h * 0.22f, h * 0.08f, dock::kInk, false, 0.5f);
            P.alpha = keep;
        }
        return h;
    };
    const Color kDanger = Rgba(255, 69, 58);
    // a glass button (the pause menu's drawBtn look): style 0 normal, 1 primary, 2 muted, 3 danger; sub: a second line
    auto button = [&](float x, float y, float w, float h, const char* icon, const std::string& label, const std::string& sub, int style, bool enabled) {
        bool held = false;
        const bool t = tap(x, y, w, h, &held) && enabled;
        const Color fill = style == 1 ? dock::kAccent : style == 3 ? kDanger : Rgba(255, 255, 255, 31);
        const float keep = P.alpha;
        if (!enabled) P.alpha *= 0.45f;
        P.Box(x, y, w, h, std::min(18.0f, h * 0.5f), held ? Mix(fill, dock::kInk, 0.12f) : fill, style == 1 || style == 3 ? 0.0f : 1.0f, dock::kBorder);
        const float ts = 15;
        const float iconX = x + 18 + 11;
        P.Icon(icon, iconX, y + h * 0.5f, 22, style == 2 ? dock::kMuted : dock::kInk, style == 0 || style == 2);
        if (sub.empty()) P.Text(label, iconX + 21, y + h * 0.5f, ts, style == 2 ? dock::kMuted : dock::kInk, style == 1 || style == 3);
        else {
            P.Text(label, iconX + 21, y + h * 0.5f - 8 * P.text, ts, dock::kInk, style == 1);
            P.Text(sub, iconX + 21, y + h * 0.5f + 10 * P.text, 11, Fade(dock::kInk, 0.75f), false, 0, w - (iconX + 21 - x) - 12);
        }
        P.alpha = keep;
        return t;
    };
    const bool loadingLook = st.screen == Screen::Splash || st.screen == Screen::Loading;

    // ---------------------------------------------------------------- the backdrop
    {
        float a;
        if (loadingLook) a = st.screen == Screen::Splash ? 1.0f : std::min(1.0f, st.t / 0.25f); // the loading screen fades in over the city
        else a = m.backdrop ? 0.68f : 1.0f;
        P.dl.Rect(0, 0, in.viewW, in.viewH, Rgba(0, 0, 0, (uint8_t)std::lround(255 * a * leave)), Rgba(0, 0, 0, (uint8_t)std::lround(255 * a * leave)), 0, 0, 0);
        if (!loadingLook && m.backdrop) { // a soft vignette: darker at the top and bottom where the logo and the version sit
            P.dl.Rect(0, 0, in.viewW, in.viewH * 0.35f, Rgba(0, 0, 0, (uint8_t)(110 * leave)), Rgba(0, 0, 0, 0), 0, 0, 0);
            P.dl.Rect(0, in.viewH * 0.7f, in.viewW, in.viewH, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, (uint8_t)(130 * leave)), 0, 0, 0);
        }
        g.Block({0, 0, in.viewW, in.viewH}); // the world under it takes no input
    }
    P.alpha = leave;

    // ---------------------------------------------------------------- the loading screen
    if (loadingLook) {
        const float fadeIn = std::clamp((st.t - 0.1f) / 0.8f, 0.0f, 1.0f);
        const float lw = std::min({AW * 0.5f, AH * 0.62f * aspect, 560.0f});
        const float lh = lw / aspect;
        const float barW = lw * 0.72f, barH = 3, gapBar = std::max(26.0f, lh * 0.12f);
        const float total = lh + gapBar + barH + 34;
        const float top = CY - total * 0.5f;
        logo(CX, top + lh * 0.5f, lw, fadeIn * fadeIn * (3 - 2 * fadeIn));
        const float by = top + lh + gapBar;
        const float a = std::clamp((st.t - 0.4f) / 0.5f, 0.0f, 1.0f);
        const float keep = P.alpha;
        P.alpha = keep * a;
        P.Box(CX - barW / 2, by, barW, barH, barH / 2, Rgba(255, 255, 255, 40));
        P.Box(CX - barW / 2, by, barW * std::clamp(m.progress, 0.0f, 1.0f), barH, barH / 2, Rgba(255, 255, 255, 230));
        P.Text(m.step, CX, by + barH + 18, 12, dock::kMuted, false, 0.5f, AW - 40);
        P.alpha = keep;
        return;
    }

    // ---------------------------------------------------------------- the title screen (also under Settings)
    if (st.screen == Screen::Title || st.screen == Screen::Settings) {
        const float a = std::clamp(st.t / 0.35f, 0.0f, 1.0f), ease = dock::EaseOut(a), rise = 12 * (1 - ease);
        const float bh = m.pad ? 54.0f : 46.0f, gap = m.pad ? 10.0f : 8.0f, pad = 14;
        const int n = m.canQuit ? 5 : 4;
        const float cardW = std::min(m.pad ? 340.0f : 300.0f, AW - 32), cardH = pad * 2 + n * bh + (n - 1) * gap;
        float lw = std::min({AW * 0.5f, 420.0f, (AH - cardH - 40) * 0.9f * aspect});
        float logoCx = CX, logoCy, cardX = CX - cardW / 2, cardY;
        const bool side = lw < 200; // too short for logo over buttons: logo left, buttons right (iPhone landscape)
        if (side) {
            lw = std::min({AW * 0.42f, (AH - 40) * aspect * 0.8f, 360.0f});
            logoCx = SL + AW * 0.27f, logoCy = CY;
            cardX = SL + AW * 0.56f, cardY = CY - cardH / 2;
            if (cardX + cardW > W - SR - 12) cardX = W - SR - 12 - cardW;
        } else {
            const float lh = lw / aspect, total = lh + 34 + cardH;
            logoCy = CY - total / 2 + lh / 2, cardY = CY - total / 2 + lh + 34;
        }
        const float dim = st.screen == Screen::Settings ? 0.35f : 1.0f; // Settings draws over it (the host: dock::MenuFrame)
        const float keep = P.alpha;
        P.alpha = keep * dim;
        logo(logoCx, logoCy - rise * 0.5f, lw, ease);
        if (!m.version.empty()) P.Text(m.version, W - SR - 12, H - SB - 12, 10, Fade(dock::kInk, 0.4f * ease), false, 1);
        if (st.screen == Screen::Settings) { P.alpha = keep; return; }
        P.alpha = keep * ease;
        cardY += rise;
        P.Glass(cardX, cardY, cardW, cardH, 24, dock::kPanel, 24, 1.7f, true);
        float y = cardY + pad;
        const float x = cardX + pad, w = cardW - 2 * pad;
        if (button(x, y, w, bh, "play", "Continue", m.continueSlot >= 0 ? m.continueLine : "No saved games yet", 1, m.continueSlot >= 0)) out.cont = true, sfx(0.8f);
        y += bh + gap;
        if (button(x, y, w, bh, "newmap", "New Game", "", 0, true)) {
            sfx(1.1f);
            if (m.inGame) st.ask = Ask::NewDiscard; else out.newGame = true;
        }
        y += bh + gap;
        if (button(x, y, w, bh, "folder", "Load Game", "", 0, true)) st.pick = Pick::Load, st.back = Screen::Title, st.Go(Screen::Slots), sfx(1.1f);
        y += bh + gap;
        if (button(x, y, w, bh, "gear", "Settings", "", 0, true)) out.settings = true, sfx(1.1f);
        y += bh + gap;
        if (m.canQuit && button(x, y, w, bh, "exit", "Quit", "", 2, true)) out.quit = true, sfx(0.65f);
        P.alpha = keep;
    }

    // ---------------------------------------------------------------- the slot picker
    if (st.screen == Screen::Slots) {
        const float a = std::clamp(st.t / 0.25f, 0.0f, 1.0f), ease = dock::EaseOut(a);
        const float keep = P.alpha;
        P.alpha = keep * ease;
        const float pad = 14, gap = 8, headH = 48;
        const float cardW = std::min(AW - 24, 820.0f);
        const int cols = cardW >= 560 ? 2 : 1, rows = (10 + cols - 1) / cols;
        const float rowH = std::clamp((AH - 24 - 2 * pad - headH - gap - (rows - 1) * gap) / rows, 44.0f, 68.0f);
        const float cardH = 2 * pad + headH + gap + rows * rowH + (rows - 1) * gap;
        const float cardX = CX - cardW / 2, cardY = std::max(ST + 12, CY - cardH / 2) + 10 * (1 - ease);
        P.Glass(cardX, cardY, cardW, cardH, 24, dock::kPanel, 24, 1.7f, true);
        // header: Back, the title
        const bool asking = st.ask != Ask::None;
        if (button(cardX + pad, cardY + pad + 2, 104, 44, "left", "Back", "", 0, !asking)) out.back = true, sfx(0.65f);
        P.Text(st.pick == Pick::Save ? "Save game" : "Load game", CX, cardY + pad + headH * 0.5f, 18, dock::kInk, true, 0.5f);
        P.Text(st.pick == Pick::Save ? "Pick a slot" : "Pick a city", cardX + cardW - pad - 4, cardY + pad + headH * 0.5f, 12, dock::kMuted, false, 1);
        const float colW = (cardW - 2 * pad - (cols - 1) * gap) / cols;
        for (int i = 0; i < 10 && i < (int)m.slots.size(); ++i) {
            const SlotRow& r = m.slots[(size_t)i];
            const int c = i / rows, k = i % rows;
            const float x = cardX + pad + c * (colW + gap), y = cardY + pad + headH + gap + k * (rowH + gap);
            const float trashW = r.used ? 44.0f : 0.0f;
            bool held = false, trashHeld = false;
            // the trash button first: its rect is not the row's
            const bool trash = r.used && tap(x + colW - trashW - 4, y + (rowH - 44) * 0.5f, 44, 44, &trashHeld) && !asking;
            const bool rowTap = tap(x, y, colW - trashW - 4, rowH, &held) && !asking;
            const bool loadable = r.used || st.pick == Pick::Save;
            const Color fill = r.current ? Rgba(10, 132, 255, 60) : Rgba(255, 255, 255, (uint8_t)(r.used ? 31 : 16));
            P.Box(x, y, colW, rowH, 16, held && loadable ? Mix(fill, dock::kInk, 0.12f) : fill, r.current ? 2.0f : 1.0f, r.current ? dock::kAccent : dock::kBorder);
            // the slot number
            P.Box(x + 10, y + rowH * 0.5f - 15, 30, 30, 15, r.used ? Rgba(255, 255, 255, 46) : Rgba(255, 255, 255, 18));
            P.Text(std::to_string(i + 1), x + 25, y + rowH * 0.5f, 13, r.used ? dock::kInk : dock::kMuted, true, 0.5f);
            const float tx = x + 52, tw = colW - 52 - trashW - 12;
            if (r.used) {
                const bool three = rowH >= 60;
                const std::string head = r.name + (r.example ? "  (example)" : "");
                if (three) {
                    P.Text(head, tx, y + rowH * 0.5f - 15 * P.text, 14, dock::kInk, true, 0, tw);
                    P.Text(r.when, tx, y + rowH * 0.5f + 1, 11, Fade(dock::kInk, 0.75f), false, 0, tw);
                    P.Text(r.stats, tx, y + rowH * 0.5f + 15 * P.text, 11, Fade(dock::kInk, 0.6f), false, 0, tw);
                } else {
                    P.Text(head, tx, y + rowH * 0.5f - 8 * P.text, 14, dock::kInk, true, 0, tw);
                    P.Text(r.when + "  " + r.stats, tx, y + rowH * 0.5f + 9 * P.text, 11, Fade(dock::kInk, 0.7f), false, 0, tw);
                }
                P.Box(x + colW - trashW - 4, y + (rowH - 44) * 0.5f, 44, 44, 14, trashHeld ? Rgba(255, 69, 58, 120) : Rgba(255, 255, 255, 0));
                P.Icon("trash", x + colW - trashW / 2 - 4, y + rowH * 0.5f, 20, dock::kMuted, false);
            } else
                P.Text(st.pick == Pick::Save ? "Empty - tap to save here" : "Empty", tx, y + rowH * 0.5f, 13, dock::kMuted, false, 0, tw);
            if (trash) st.ask = Ask::Delete, st.askSlot = i, sfx(0.9f);
            else if (rowTap && loadable) {
                sfx(1.1f);
                if (st.pick == Pick::Save) {
                    if (r.used) st.ask = Ask::Overwrite, st.askSlot = i;
                    else out.save = i;
                } else if (m.inGame) st.ask = Ask::LoadDiscard, st.askSlot = i;
                else out.load = i;
            }
        }
        P.alpha = keep;
    }

    // ---------------------------------------------------------------- the confirm card (over the title or the picker)
    if (st.ask != Ask::None) {
        P.dl.Rect(0, 0, in.viewW, in.viewH, Rgba(0, 0, 0, 110), Rgba(0, 0, 0, 110), 0, 0, 0);
        const float cw = std::min(380.0f, AW - 32), bh = 48.0f, pad = 18.0f, ch = pad * 2 + 26 + 40 + bh;
        const float cx = CX - cw / 2, cy = CY - ch / 2;
        P.Glass(cx, cy, cw, ch, 24, Rgba(28, 29, 34, 210), 24, 1.7f, true);
        const std::string slot = st.askSlot >= 0 ? "Save " + std::to_string(st.askSlot + 1) : "";
        const std::string name = st.askSlot >= 0 && st.askSlot < (int)m.slots.size() ? m.slots[(size_t)st.askSlot].name : "";
        std::string head, body, yes;
        int style = 1;
        switch (st.ask) {
        case Ask::Overwrite: head = "Overwrite " + slot + "?", body = name + " will be replaced by this city.", yes = "Overwrite"; break;
        case Ask::Delete: head = "Delete " + slot + "?", body = name + " will be gone for good.", yes = "Delete", style = 3; break;
        case Ask::LoadDiscard: head = "Load " + slot + "?", body = "Progress since your last save is lost.", yes = "Load"; break;
        case Ask::NewDiscard: head = "Start a new city?", body = "Progress since your last save is lost.", yes = "New Game"; break;
        default: break;
        }
        modal = true;
        P.Text(head, CX, cy + pad + 12, 17, dock::kInk, true, 0.5f, cw - 2 * pad);
        P.Text(body, CX, cy + pad + 26 + 16, 12, dock::kMuted, false, 0.5f, cw - 2 * pad);
        const float bw = (cw - 2 * pad - 10) / 2, by = cy + ch - pad - bh;
        if (button(cx + pad, by, bw, bh, "x", "Cancel", "", 0, true)) st.ask = Ask::None, st.askSlot = -1, sfx(0.65f);
        else if (button(cx + pad + bw + 10, by, bw, bh, st.ask == Ask::Delete ? "trash" : "ok", yes, "", style, true)) {
            sfx(1.2f);
            switch (st.ask) {
            case Ask::Overwrite: out.save = st.askSlot; break;
            case Ask::Delete: out.del = st.askSlot; break;
            case Ask::LoadDiscard: out.load = st.askSlot; break;
            case Ask::NewDiscard: out.newGame = true; break;
            default: break;
            }
            st.ask = Ask::None, st.askSlot = -1;
        }
    }
    P.alpha = 1;
}

} // namespace title
} // namespace gui
} // namespace tbd
