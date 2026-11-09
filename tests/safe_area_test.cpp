// Headless checks for the safe area (src/gui/safe_area.h) across the touch GUI, and for the compact terrain kit
// (src/gui/terrain_kit.h): with large insets on every side (a notched / island device, landscape) no glass panel or
// button of the dock (PC UI and Phone UI: the bar, a category panel, the placement bar, the slim bar, the corner
// buttons, the menus, the pause menu and Settings), the road kit (full and mini), the aim dot's buttons or the terrain
// kit lands in an inset strip. Then the terrain kit's tools, chips, the painting pill and Save / Load.
// Build and run: tests\safe_area_test.bat
#include "../src/gui/road_reticle.h"
#include "../src/gui/road_toolkit.h"
#include "../src/gui/terrain_kit.h"

#ifdef _WINDOWS_
#error "the GUI must not pull in windows.h"
#endif

#include <cmath>
#include <cstdio>
#include <functional>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)

using namespace tbd;
using namespace tbd::gui;

struct Screen {
    Context g;
    InputQueue q;
    dock::Atlas atlas;
    float w, h, dpi;
    SafeInsets safe;
    // one frame of `draw`, then every glass panel / blocked button that is not the full-screen scrim must keep out of the insets
    // settled: only the last of several frames counts (panels slide in from past the edges while they ease)
    int Check(const char* what, const std::function<void()>& draw, int frames = 1) {
        for (int i = 1; i < frames; ++i) g.BeginFrame(q.Take(w, h, dpi)), draw(), g.EndFrame();
        g.BeginFrame(q.Take(w, h, dpi));
        draw();
        g.EndFrame();
        const float W = w / dpi, H = h / dpi;
        int n = 0, bad = 0;
        for (const Rect& r : g.ShownPanels()) {
            const Rect p{r.x / dpi, r.y / dpi, r.w / dpi, r.h / dpi};
            if (p.w >= W - 0.5f && p.h >= H - 0.5f) continue; // the pause menu's frosted scrim covers everything on purpose
            ++n;
            if (safe.Intrudes(p, W, H)) {
                ++bad;
                std::printf("  %s: panel %.0f,%.0f %.0fx%.0f reaches into the safe area\n", what, p.x, p.y, p.w, p.h);
            }
        }
        std::printf("%-34s %2d panels, %d in the insets\n", what, n, bad);
        CHECK(bad == 0);
        return n;
    }
};

static dock::Model MakeModel() {
    dock::Model m;
    for (int c = 0; c < 10; ++c) {
        dock::Category cat{"c" + std::to_string(c), "Cat " + std::to_string(c), "road", Rgba(120, 130, 150), {}};
        dock::Group g1{"Group A", "road", {}, {}}, g2{"Group B", "road", {}, {}};
        for (int i = 0; i < 14; ++i) g1.items.push_back({"Item " + std::to_string(i), "$10", "road", c * 100 + i});
        g2.items.push_back({"Other", "", "road", c * 100 + 50});
        cat.groups = {g1, g2};
        m.cats.push_back(cat);
    }
    return m;
}

int main() {
    // ---- the helper
    {
        SafeInsets s;
        CHECK(!s.Any());
        CHECK(SafeInsets::Parse("24,59,21,59", s) && s.top == 24 && s.left == 59 && s.bottom == 21 && s.right == 59 && s.corner == 0);
        CHECK(SafeInsets::Parse("24,59,21,59,55", s) && s.corner == 55 && s.B() >= 21 && s.T() >= 0.3f * 55);
        CHECK(!SafeInsets::Parse("1,2", s) && !SafeInsets::Parse(nullptr, s));
        const float px[5] = {118, 48, 118, 42, 0};
        const SafeInsets f = SafeInsets::FromPixels(px, 2.0f);
        CHECK(f.left == 59 && f.top == 24 && f.right == 59 && f.bottom == 21);
        const Rect a = f.Area(852, 393);
        CHECK(a.x == 59 && a.y == 24 && a.w == 852 - 118 && a.h == 393 - 45);
        CHECK(f.Intrudes({10, 100, 50, 50}, 852, 393) && !f.Intrudes({70, 100, 50, 50}, 852, 393));
    }

    // ---- every screen of the dock, the kits and the dot, with big insets on all four sides (iPad and iPhone sizes)
    struct Dev { const char* name; float w, h, dpi; bool pad; SafeInsets safe; };
    const Dev devs[] = {{"iPad", 2360, 1640, 2.0f, true, {60, 40, 60, 36, 18}},
                        {"iPhone", 2556, 1179, 3.0f, false, {59, 24, 59, 21, 55}}};
    for (const Dev& d : devs) {
        std::printf("---- %s %.0fx%.0f pt, insets T%.0f L%.0f B%.0f R%.0f\n", d.name, d.w / d.dpi, d.h / d.dpi, d.safe.top, d.safe.left, d.safe.bottom, d.safe.right);
        for (int pc = 0; pc < 2; ++pc) {
            Screen S{{}, {}, {}, d.w, d.h, d.dpi, d.safe};
            dock::State st;
            st.pad = d.pad, st.pcUi = pc == 1;
            dock::Model m = MakeModel();
            m.safe = d.safe;
            dock::Out o;
            const char* ui = pc ? "PC UI" : "Phone UI";
            char what[64];
            auto frame = [&] { dock::Frame(S.g, st, m, S.atlas, o); };
            S.Check((std::snprintf(what, sizeof(what), "%s bar", ui), what), frame, 20); // eased in
            st.cat = 3, st.catT = 0, st.open = true;
            S.Check((std::snprintf(what, sizeof(what), "%s category open", ui), what), frame, 20);
            st.cat = -1, st.open = false;
            m.tool = "Two-Lane Road", m.toolIcon = "road", m.modeIcon = "straight", m.modeName = "Straight";
            m.toolOpts = {{"Lower", "down"}, {"Raise", "up"}, {"Snapping", "snap"}};
            S.Check((std::snprintf(what, sizeof(what), "%s placement controls", ui), what), frame, 20);
            CHECK(o.bottomH > d.safe.bottom);
            m.slim = m.hidePlaceBar = true;
            S.Check((std::snprintf(what, sizeof(what), "%s slim (a kit is out)", ui), what), frame, 30);
            if (pc) {
                CHECK(st.slimT == 1.0f);
                std::printf("  slim bar: %.0f pt from the bottom (%.0f pt of it the safe area)\n", o.bottomH, d.safe.bottom);
                CHECK(o.bottomH <= d.safe.bottom + 8 + 44 + 0.5f); // 44 pt tall over its 8 pt margin
            }
            // the road kit (mini, then full) and the dot over the slim dock
            road_kit::State ks;
            ks.safe = d.safe, ks.mini = true, ks.dotAvail = true;
            city::RoadTool tool;
            tool.Open();
            road_kit::Out ko;
            road_kit::Layout kl;
            reticle::State rs;
            reticle::Out ro;
            reticle::Layout rl;
            auto kits = [&] {
                dock::Frame(S.g, st, m, S.atlas, o);
                kl = road_kit::Frame(S.g, S.atlas, tool, ks, o.bottomH + (m.tuck ? 4 : 8), d.pad, ko);
                rl = reticle::Frame(S.g, S.atlas, tool, rs, d.safe, kl.rightTop, true, {0, 0, 0}, ro);
            };
            S.Check((std::snprintf(what, sizeof(what), "%s mini road kit + dot", ui), what), kits);
            ks.pop = 2; // the snapping popover
            S.Check((std::snprintf(what, sizeof(what), "%s mini kit, popover", ui), what), kits);
            ks.pop = 0, ks.caption = "A long caption about what the last tap did on the toolkit", ks.captionT = 0;
            S.Check((std::snprintf(what, sizeof(what), "%s mini kit, caption", ui), what), kits);
            // [road ui] compact: the dock tucked away, the ~60 % strips on the bottom edge, the tick / undo clear above them
            ks.caption.clear(), ks.compact = ks.dockAvail = true, m.tuck = true;
            S.Check((std::snprintf(what, sizeof(what), "%s compact kit, dock tucked", ui), what), kits, 2);
            CHECK(std::fabs(o.bottomH - d.safe.B()) < 0.01f); // the dock takes nothing but the safe area
            CHECK(rl.okY + rl.okS <= kl.top + 0.5f && rl.backY + rl.backS + 16 <= kl.top + 0.5f); // the buttons (and the Undo label) clear the strips
            std::printf("  compact kit: %.0f pt from the bottom (was the dock + the mini strips)\n", d.h / d.dpi - kl.top);
            ks.compact = ks.dockAvail = false, m.tuck = false;
            ks.mini = false;
            m.slim = m.hidePlaceBar = false;
            S.Check((std::snprintf(what, sizeof(what), "%s full road kit", ui), what), kits);
            // the terrain kit
            m.slim = m.hidePlaceBar = true, m.tool = "Raise";
            int mode = 0;
            float radius = 30, strength = 6;
            terrain_kit::State ts;
            ts.safe = d.safe, ts.more = true;
            terrain_kit::Out to;
            S.Check((std::snprintf(what, sizeof(what), "%s terrain kit", ui), what), [&] {
                dock::Frame(S.g, st, m, S.atlas, o);
                terrain_kit::Frame(S.g, S.atlas, {&mode, &radius, &strength}, ts, o.bottomH + 8, false, to);
            });
            // the pause menu and Settings (the cards stay inside; the scrim is skipped)
            m.tool.clear();
            st.menu = true, st.menuT = 1;
            S.Check((std::snprintf(what, sizeof(what), "%s pause menu", ui), what), frame);
            st.settings = true;
            S.Check((std::snprintf(what, sizeof(what), "%s settings", ui), what), frame);
        }
    }

    // ---- the terrain kit's controls (iPad, no insets)
    {
        Screen S{{}, {}, {}, 2360, 1640, 2.0f, {}};
        int mode = 0;
        float radius = 30, strength = 6;
        terrain_kit::State ts;
        terrain_kit::Out to;
        terrain_kit::Layout L;
        bool painting = false;
        auto frame = [&] {
            S.g.BeginFrame(S.q.Take(S.w, S.h, S.dpi));
            L = terrain_kit::Frame(S.g, S.atlas, {&mode, &radius, &strength}, ts, 60, painting, to);
            S.g.EndFrame();
        };
        bool sawSave = false, sawClose = false;
        auto tap = [&](float x, float y) {
            S.q.Down(x * S.dpi, y * S.dpi), frame();
            S.q.Up(x * S.dpi, y * S.dpi), frame();
            sawSave |= to.save, sawClose |= to.close;
            S.q.Leave(), frame();
        };
        frame();
        CHECK(L.h == terrain_kit::kH && L.w == terrain_kit::StripWidth() && L.y + L.h <= S.h / S.dpi - 60 + 0.5f);
        std::printf("terrain kit: %.0f x %.0f pt\n", L.w, L.h);
        using namespace terrain_kit;
        const float y = L.y + kPad + kB * 0.5f;
        auto toolX = [&](int m) { return L.x + kPad + kB + kGap + kSep + m * (kB + kGap) + kB * 0.5f; };
        const float chip0 = L.x + kPad + kB + kGap + kSep + 4 * (kB + kGap) + kSep; // the size chip's left
        for (int m = 3; m >= 0; --m) {
            tap(toolX(m), y);
            CHECK(mode == m);
        }
        tap(chip0 + kB * 0.5f, y); // size -
        CHECK(std::fabs(radius - 24) < 0.01f);
        tap(chip0 + kB + kGap + kValueW + kGap + kB * 0.5f, y); // size +
        CHECK(std::fabs(radius - 30) < 0.01f);
        const float chip1 = chip0 + 2 * (kB + kGap) + kValueW + kGap;
        tap(chip1 + kB + kGap + kValueW + kGap + kB * 0.5f, y); // strength +
        CHECK(std::fabs(strength - 7.5f) < 0.01f);
        // held: repeats, clamped
        S.q.Down((chip0 + kB * 0.5f) * S.dpi, y * S.dpi);
        for (int i = 0; i < 400; ++i) frame();
        S.q.Up((chip0 + kB * 0.5f) * S.dpi, y * S.dpi), frame(), S.q.Leave(), frame();
        CHECK(radius == kMinRadius);
        // more: Save
        const float moreX = chip1 + 2 * (kB + kGap) + kValueW + kGap + kB * 0.5f;
        tap(moreX, y);
        CHECK(ts.more && L.top < L.y);
        tap(std::min(moreX - kB * 0.5f - kPad, S.w / S.dpi - 12 - (2 * kPad + 2 * 76 + kGap)) + kPad + 38, L.y - 8 - kH + kPad + kB * 0.5f);
        CHECK(sawSave && !ts.more);
        // painting: the strip folds to a pill, then comes back
        painting = true;
        for (int i = 0; i < 30; ++i) frame();
        CHECK(L.w < 160);
        painting = false;
        for (int i = 0; i < 30; ++i) frame();
        CHECK(L.w == StripWidth());
        tap(L.x + kPad + kB * 0.5f, y);
        CHECK(sawClose);
        CHECK(std::string(ModeName(3)) == "Smooth");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
