// Headless checks for the front end (src/gui/title_screen.h): the title's buttons (Continue disabled without saves, New
// Game, Load Game opening the ten-slot list, Settings, Quit only where there is one), the picker's taps (load, save into an
// empty slot, overwrite / delete / leave-unsaved behind a confirm card, Back), the fade out, and on iPad and iPhone sizes
// with big safe-area insets: every tap target at least 44 pt and inside the safe area, every panel too.
// Build and run: tests\title_screen_test.bat
#include "../src/gui/title_screen.h"

#ifdef _WINDOWS_
#error "the GUI must not pull in windows.h"
#endif

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
namespace ti = tbd::gui::title;

struct Rig {
    Context g;
    InputQueue q;
    dock::Atlas atlas;
    float w, h, dpi;
    ti::State st;
    ti::Model m;
    ti::Out out;
    void Frame() {
        g.BeginFrame(q.Take(w, h, dpi, 1.0f / 60.0f));
        ti::Frame(g, st, m, atlas, out);
        g.EndFrame();
    }
    void Settle(int n = 40) { for (int i = 0; i < n; ++i) Frame(); }
    // a tap at the centre of a target (points): press, release; the release frame's Out
    ti::Out Tap(const Rect& r) {
        const float x = (r.x + r.w * 0.5f) * dpi, y = (r.y + r.h * 0.5f) * dpi;
        q.Down(x, y);
        Frame();
        q.Up(x, y);
        Frame();
        const ti::Out o = out;
        q.Leave();
        Frame(); // the hits of the screen as it is now
        return o;
    }
    Rect Hit(size_t i) const { return i < out.hits.size() ? out.hits[i] : Rect{-1000, -1000, 1, 1}; }
    // every target >= 44 pt, inside the safe area; every panel that is not the full-screen backdrop too
    int CheckLayout(const char* what) {
        const float W = w / dpi, H = h / dpi;
        int bad = 0;
        for (const Rect& r : out.hits) {
            if (r.w < 44 - 0.01f || r.h < 44 - 0.01f) ++bad, std::printf("  %s: target %.0f,%.0f %.0fx%.0f under 44 pt\n", what, r.x, r.y, r.w, r.h);
            if (m.safe.Intrudes(r, W, H)) ++bad, std::printf("  %s: target %.0f,%.0f %.0fx%.0f in the safe area\n", what, r.x, r.y, r.w, r.h);
        }
        for (const Rect& r : g.ShownPanels()) {
            const Rect p{r.x / dpi, r.y / dpi, r.w / dpi, r.h / dpi};
            if (p.w >= W - 0.5f && p.h >= H - 0.5f) continue;
            if (m.safe.Intrudes(p, W, H)) ++bad, std::printf("  %s: panel %.0f,%.0f %.0fx%.0f in the safe area\n", what, p.x, p.y, p.w, p.h);
        }
        std::printf("%-30s %2zu targets, %d bad\n", what, out.hits.size(), bad);
        CHECK(bad == 0);
        return bad;
    }
};

static void FillSlots(ti::Model& m, std::initializer_list<int> used) {
    for (auto& r : m.slots) r = ti::SlotRow{};
    for (int i : used) m.slots[(size_t)i] = {true, "City " + std::to_string(i + 1), "Nov 8 2026, 14:32  -  Mar 4, Y1", "1,234 citizens  -  $1.50M", false, i == 0};
}

int main() {
    struct Dev { const char* name; float w, h, dpi; bool pad; SafeInsets safe; };
    const Dev devs[] = {{"iPad", 2360, 1640, 2.0f, true, {60, 40, 60, 36, 18}}, {"iPhone", 2556, 1179, 3.0f, false, {59, 24, 59, 21, 55}},
                        {"PC 1080p", 1920, 1080, 1.0f, true, {}}};
    for (const Dev& d : devs) {
        std::printf("---- %s\n", d.name);
        Rig R{{}, {}, {}, d.w, d.h, d.dpi};
        R.m.safe = d.safe, R.m.pad = d.pad, R.m.canQuit = std::string(d.name).rfind("PC", 0) == 0;
        // ---- the loading screen: nothing to tap, the backdrop blocks the world
        R.st.Go(ti::Screen::Splash);
        R.m.progress = 0.5f, R.m.step = "Shaping the terrain";
        R.Settle();
        CHECK(R.out.hits.empty() && !R.g.ShownPanels().empty());
        R.CheckLayout("splash");
        // ---- the title, no saves: Continue disabled
        FillSlots(R.m, {});
        R.m.continueSlot = -1;
        R.st.Go(ti::Screen::Title);
        R.Settle();
        R.CheckLayout("title");
        const size_t nButtons = R.out.hits.size();
        CHECK(nButtons == (R.m.canQuit ? 5u : 4u));
        CHECK(!R.Tap(R.Hit(0)).cont); // Continue without a save does nothing
        R.m.continueSlot = 0, R.m.continueLine = "Save 1 - City 1";
        CHECK(R.Tap(R.Hit(0)).cont);
        CHECK(R.Tap(R.Hit(1)).newGame && R.st.ask == ti::Ask::None);
        CHECK(R.Tap(R.Hit(3)).settings);
        if (R.m.canQuit) CHECK(R.Tap(R.Hit(4)).quit);
        // from the game, New Game asks first
        R.m.inGame = true;
        CHECK(!R.Tap(R.Hit(1)).newGame && R.st.ask == ti::Ask::NewDiscard);
        R.CheckLayout("title, confirm new game");
        CHECK(R.Tap(R.Hit(R.out.hits.size() - 1)).newGame && R.st.ask == ti::Ask::None);
        R.m.inGame = false;
        // Load Game: the list
        R.Settle(2);
        R.Tap(R.Hit(2));
        CHECK(R.st.screen == ti::Screen::Slots && R.st.pick == ti::Pick::Load && R.st.back == ti::Screen::Title);
        // ---- the picker: Save 1 and Save 4 used
        FillSlots(R.m, {0, 3});
        R.Settle();
        R.CheckLayout("load picker");
        CHECK(R.out.hits.size() == 1 + 10 + 2); // Back, ten rows, two trash buttons
        // the targets in order: Back, [trash] row per slot
        auto rowHit = [&](int slot, bool trash) {
            size_t k = 1;
            for (int i = 0; i < slot; ++i) k += R.m.slots[(size_t)i].used ? 2 : 1;
            return R.Hit(R.m.slots[(size_t)slot].used ? (trash ? k : k + 1) : k);
        };
        CHECK(R.Tap(rowHit(1, false)).load == -1); // empty: nothing to load
        CHECK(R.Tap(rowHit(3, false)).load == 3);
        R.m.inGame = true; // in game: loading asks first
        CHECK(R.Tap(rowHit(0, false)).load == -1 && R.st.ask == ti::Ask::LoadDiscard && R.st.askSlot == 0);
        R.CheckLayout("load picker, confirm");
        CHECK(R.Tap(R.Hit(R.out.hits.size() - 1)).load == 0);
        R.m.inGame = false;
        // delete, cancelled then confirmed
        CHECK(R.Tap(rowHit(3, true)).del == -1 && R.st.ask == ti::Ask::Delete && R.st.askSlot == 3);
        CHECK(R.Tap(R.Hit(R.out.hits.size() - 2)).del == -1 && R.st.ask == ti::Ask::None); // Cancel
        R.Tap(rowHit(3, true));
        CHECK(R.Tap(R.Hit(R.out.hits.size() - 1)).del == 3);
        // the row taps are dead while the card is up
        R.Tap(rowHit(3, true));
        CHECK(R.Tap(rowHit(0, false)).load == -1 && R.st.ask == ti::Ask::Delete);
        R.Tap(R.Hit(R.out.hits.size() - 2));
        // save mode: an empty slot saves at once, a used one asks
        R.st.pick = ti::Pick::Save;
        R.Settle(2);
        R.CheckLayout("save picker");
        CHECK(R.Tap(rowHit(5, false)).save == 5);
        CHECK(R.Tap(rowHit(0, false)).save == -1 && R.st.ask == ti::Ask::Overwrite);
        CHECK(R.Tap(R.Hit(R.out.hits.size() - 1)).save == 0);
        // Back
        CHECK(R.Tap(R.Hit(0)).back);
        // ---- the fade out to the game
        R.st.Go(ti::Screen::Title);
        R.st.leaveT = 0;
        bool left = false;
        for (int i = 0; i < 40 && !left; ++i) R.Frame(), left = R.out.left;
        CHECK(left);
        // ---- Settings over the title: the dock's pages on their own (dock::MenuFrame), inside the safe area
        {
            dock::State ds;
            ds.pad = d.pad, ds.pcUi = d.pad, ds.menu = ds.settings = true;
            dock::Model dm;
            dm.safe = d.safe;
            R.st.Go(ti::Screen::Settings);
            for (int i = 0; i < 30; ++i) {
                R.g.BeginFrame(R.q.Take(R.w, R.h, R.dpi));
                ti::Frame(R.g, R.st, R.m, R.atlas, R.out);
                dock::Out mo;
                dock::MenuFrame(R.g, ds, dm, R.atlas, mo, true);
                R.g.EndFrame();
            }
            CHECK(ds.menuT > 0.4f); // the standalone menu keeps its own clock
            R.out.hits.clear();     // (the dock's targets are the safe-area test's)
            R.CheckLayout("settings over the title");
        }
    }
    std::printf("title_screen_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
