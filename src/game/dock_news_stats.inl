// The Glass Dock's own extras on the city game: the stats overlay switch and the City News card. Included by
// glass_dock_host.inl inside namespace gdock, before the settings file code (it needs Legacy, g_state, g_atlas).
//
//   Show stats  the FPS / draws / polys / camera / sun / terrain text (game_core.inl DrawStatsText). Off by default on iOS
//               and in the Phone UI, on in the PC UI on Windows; Settings > Graphics options > Show stats switches it,
//               remembered in saves\settings.ini as stats=0|1 (no key: the device default). The toasts always show.
//   City News   a small pill at the top right (a bell, "News", a red count of unread headlines, a red dot while the radio
//               is on air) that is always there; a tap opens the card: the latest headlines, More / Less, the radio
//               mode (Radio on / Host only / Radio off) and a close button. While a build category's panel or the Phone
//               UI's menus are out the card folds back to the pill so it never covers the build palette. Collapsed by
//               default on iOS and the Phone UI; settings.ini news=0|1 remembers it.
// Both keys are written by SaveGuiPrefs after SaveSettingsFile wrote the rest of the file.

std::wstring SettingsFilePath(); // glass_dock_host.inl
int g_statsPref = -1;            // settings.ini stats=: -1 none (the device default), 0 off, 1 on
int g_newsPref = -1;             // settings.ini news=: -1 none (the device default), 0 collapsed, 1 open
tbd::gui::NewsPill g_newsPill;
bool g_newsPillInit = false;

inline bool TouchDefault() {
#if defined(__APPLE__)
    return true;
#else
    return !g_state.pcUi;
#endif
}
bool StatsOverlayOn() {
    if (Legacy()) return true; // the CS2 HUD: as it always was
    return g_statsPref >= 0 ? g_statsPref != 0 : !TouchDefault();
}

void LoadGuiPrefs() {
    FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[512];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    int v = 0;
    if (tbd::gfx::Find(text, "stats", v)) g_statsPref = v != 0 ? 1 : 0;
    if (tbd::gfx::Find(text, "news", v)) g_newsPref = v != 0 ? 1 : 0;
}
// Rewrites settings.ini with this file's keys (every other line kept as SaveSettingsFile wrote it).
void SaveGuiPrefs() {
    std::string text;
    if (FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"rb")) {
        char buf[512];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    std::string kept, line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '\n') { line += text[i]; continue; }
        if (!line.empty() && line.rfind("stats=", 0) != 0 && line.rfind("news=", 0) != 0) kept += line + "\n";
        line.clear();
    }
    if (g_statsPref >= 0) kept += std::string("stats=") + (g_statsPref ? "1" : "0") + "\n";
    if (g_newsPref >= 0) kept += std::string("news=") + (g_newsPref ? "1" : "0") + "\n";
    FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"wb");
    if (!f) return;
    fwrite(kept.data(), 1, kept.size(), f);
    fclose(f);
}

// The City News pill and card (city_radio.inl fills hud.news, newest first). Also keeps the Graphics page's Show stats row
// in step with StatsOverlayOn (the dock drew it this frame: a change is the player's).
void NewsCard(tbd::gui::CityHud& h, bool menuOpen) {
    using namespace tbd::gui;
    using namespace tbd::gui::dock;
    { // the Show stats row: a value other than the one put there last frame is the player's tap
        static int lastSet = -1;
        if (lastSet >= 0 && (g_model.showStats ? 1 : 0) != lastSet) g_statsPref = g_model.showStats ? 1 : 0, SaveGuiPrefs();
        g_model.showStats = StatsOverlayOn();
        lastSet = g_model.showStats ? 1 : 0;
    }
    if (!g_newsPillInit) { // the card opens on 3 headlines (More: 6)
        g_newsPillInit = true, h.newsOpen = false;
        g_newsPill.expanded = g_newsPref >= 0 ? g_newsPref != 0 : !TouchDefault();
    }
    // folded back to the pill while the build palette or the Phone UI's menus are out (the player's choice is kept)
    const bool palette = g_state.cat >= 0 || (!g_state.pcUi && g_state.open);
    const bool hidden = menuOpen || h.options;
    {
        std::vector<std::string> keys;
        keys.reserve(h.news.size());
        for (const CityHud::NewsItem& it : h.news) keys.push_back(NewsPill::Key(it.when, it.text));
        g_newsPill.Update(keys, g_newsPill.expanded && !palette && !hidden);
    }
    if (hidden) return;
    Context& g = g_gui;
    const float s = g.Scale();
    Painter P(g, g_atlas, s);
    Tapper T{g, g_state};
    const Input& in = g.In();
    auto R = [&](float rx, float ry, float rw, float rh) { return Rect{rx * s, ry * s, rw * s, rh * s}; };
    const float W = in.viewW / s - g_model.safe.R(), H = in.viewH / s, top = 16.0f + g_model.safe.T(); // [safe area]
    const bool onAir = h.onAir && h.radioMode > 0;
    auto toggle = [&](bool open) {
        g_newsPill.expanded = open, g_newsPref = open ? 1 : 0;
        SaveGuiPrefs();
        tbd::audio::PlayUi("sfx/ui_category_select", 0.5f, open ? 0.9f : 0.75f);
    };
    if (!g_newsPill.expanded || palette) {
        const float ph = 34.0f, icon = 18.0f, label = 13.0f;
        char count[8] = {};
        if (g_newsPill.unread > 9) std::snprintf(count, sizeof(count), "9+");
        else if (g_newsPill.unread > 0) std::snprintf(count, sizeof(count), "%d", g_newsPill.unread);
        const float cw = count[0] ? std::max(18.0f, P.TextW(count, 10.5f, true) + 10.0f) : 0.0f;
        const float pw = 12.0f + icon + 7.0f + P.TextW("News", label, true) + (cw > 0 ? 7.0f + cw : 0.0f) + (onAir ? 13.0f : 0.0f) + 12.0f;
        const float x = W - 16.0f - pw, y = top, cy = y + ph * 0.5f;
        bool held = false;
        if (T.Tap(R(x, y, pw, ph), &held)) {
            if (palette) // the news over the palette: the palette goes, the card opens
                g_state.lastCat = g_state.cat, g_state.cat = -1, g_state.catT = 0, g_state.closing = true, g_state.open = false;
            toggle(true);
        }
        P.Glass(x, y, pw, ph, ph * 0.5f, held ? Rgba(60, 62, 70, 200) : kPanel, 22, 1.6f, true);
        float cx = x + 12.0f;
        P.Icon("bell", cx + icon * 0.5f, cy, icon, kInk, false);
        cx += icon + 7.0f;
        P.Text("News", cx, cy, label, kInk, true);
        cx += P.TextW("News", label, true);
        if (cw > 0) {
            cx += 7.0f;
            P.Box(cx, cy - 9.0f, cw, 18.0f, 9.0f, Rgba(255, 69, 58));
            P.Text(count, cx + cw * 0.5f, cy, 10.5f, kInk, true, 0.5f);
            cx += cw;
        }
        if (onAir) P.dl.Circle((cx + 9.0f) * s, cy * s, 3.5f * s, P.F(Rgba(255, 69, 58)));
        return;
    }
    // the open card: up to 3 headlines, 6 with More, never taller than the top half of the screen
    const float w = std::min(300.0f, W - 32.0f), pad = 14.0f, x = W - 16.0f - w, y = top;
    const float textSize = 13.0f, metaSize = 10.5f, lineH = 17.0f, innerW = w - 2 * pad, chipH = 28.0f;
    auto wrap = [&](const std::string& text) {
        std::vector<std::string> out;
        std::string line, word;
        auto flush = [&] {
            if (word.empty()) return;
            const std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && P.TextW(t, textSize) > innerW) out.push_back(line), line = word;
            else line = t;
            word.clear();
        };
        for (char c : text) { if (c == ' ') flush(); else word += c; }
        flush();
        if (!line.empty()) out.push_back(line);
        return out;
    };
    const float maxH = std::max(160.0f, H * 0.5f - y);
    const float fixedH = pad + 18.0f + 8.0f + 8.0f + chipH + pad; // title row, the chips
    std::vector<std::vector<std::string>> lines;
    float hgt = fixedH;
    const size_t want = h.newsOpen ? 6 : 3;
    if (h.news.empty()) lines.push_back(wrap("Quiet day. Build something and the city will talk about it.")), hgt += lineH * lines[0].size() + 4;
    size_t shown = 0;
    for (size_t i = 0; i < h.news.size() && shown < want; ++i) {
        std::vector<std::string> l = wrap(h.news[i].text);
        const float add = 15.0f + lineH * l.size() + 10.0f;
        if (shown > 0 && hgt + add > maxH) break;
        hgt += add, lines.push_back(std::move(l)), ++shown;
    }
    P.Glass(x, y, w, hgt, 18, kPanel, 22, 1.6f, true);
    float cy = y + pad + 9.0f;
    P.Text("CITY NEWS", x + pad, cy, 12.0f, kInk, true);
    { // close: back to the pill
        const float bs = 26.0f, bx = x + w - pad - bs + 4.0f, by = cy - bs * 0.5f;
        bool held = false;
        if (T.Tap(R(bx, by, bs, bs), &held)) toggle(false);
        P.Box(bx, by, bs, bs, bs * 0.5f, held ? Rgba(255, 255, 255, 60) : kChip);
        P.Icon("x", bx + bs * 0.5f, by + bs * 0.5f, 14.0f, kInk, false);
        if (onAir) {
            const float tw = P.TextW("ON AIR", 10.5f, true);
            P.dl.Circle((bx - 8 - tw - 9) * s, cy * s, 3.5f * s, P.F(Rgba(255, 69, 58)));
            P.Text("ON AIR", bx - 8, cy, 10.5f, Rgba(255, 105, 97), true, 1.0f);
        }
    }
    cy += 9.0f + 8.0f;
    P.Box(x + pad, cy - 4.0f, innerW, 1.0f, 0, kBorder);
    if (h.news.empty()) {
        for (const std::string& l : lines[0]) P.Text(l, x + pad, cy + lineH * 0.5f, textSize, kMuted), cy += lineH;
        cy += 4;
    }
    for (size_t i = 0; i < shown; ++i) {
        const CityHud::NewsItem& it = h.news[i];
        const Color dot = it.tone > 0 ? Rgba(48, 209, 88) : it.tone < 0 ? Rgba(255, 159, 10) : Rgba(255, 255, 255, 120);
        P.dl.Circle((x + pad + 3) * s, (cy + 7.5f) * s, 3.0f * s, P.F(dot));
        P.Text(it.when + (it.aired ? "   ON THE RADIO" : ""), x + pad + 12, cy + 7.5f, metaSize, kMuted, it.aired);
        cy += 15.0f;
        for (const std::string& l : lines[i]) P.Text(l, x + pad, cy + lineH * 0.5f, textSize, i == 0 ? kInk : kBarInk), cy += lineH;
        cy += 10.0f;
    }
    cy = y + hgt - pad - chipH;
    auto chip = [&](float cx, const char* label, bool on) {
        const float cw = P.TextW(label, 11.5f, true) + 24.0f;
        bool held = false;
        const bool tap = T.Tap(R(cx, cy, cw, chipH), &held);
        P.Box(cx, cy, cw, chipH, chipH * 0.5f, on ? Fade(kAccent, 0.85f) : held ? Rgba(255, 255, 255, 60) : kChip);
        P.Text(label, cx + cw * 0.5f, cy + chipH * 0.5f, 11.5f, kInk, true, 0.5f);
        return std::pair<bool, float>{tap, cw};
    };
    float chipX = x + pad;
    if (h.news.size() > 3) {
        auto [moreTap, mw] = chip(chipX, h.newsOpen ? "Less" : "More", false);
        if (moreTap) h.newsOpen = !h.newsOpen, tbd::audio::PlayUi("sfx/ui_category_select", 0.5f, 0.8f);
        chipX += mw + 8.0f;
    }
    static const char* const kMode[] = {"Radio off", "Host only", "Radio on"};
    auto [modeTap, rw] = chip(chipX, kMode[std::clamp(h.radioMode, 0, 2)], h.radioMode == 2);
    if (modeTap) h.radioMode = (h.radioMode + 2) % 3, tbd::audio::PlayUi("sfx/ui_category_select", 0.5f, 1.0f);
    (void)rw;
}
