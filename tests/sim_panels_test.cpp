// Tests for src/gui/sim_panels.h (the sim phases' panel model, CS2 sim roadmap Phase 0). Headless: no window, no GPU.
#include "../src/gui/sim_panels.h"
#include "../src/gui/city_overlays.h"

#include <cmath>
#include <cstdio>
#include <functional>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

using namespace tbd::gui;

struct Harness {
    Context g;
    InputQueue q;
    float w = 1024, h = 768, dpi = 1.0f;
    void Frame(const std::function<void(Context&)>& ui) {
        g.BeginFrame(q.Take(w, h, dpi));
        ui(g);
        g.EndFrame();
    }
};

static float g_tax = 10.0f;
static int g_changedRow = -1, g_fills = 0;
static void FillTest(SimPanelSpec& s, void*) {
    ++g_fills;
    s.sub = "test panel";
    s.Stat("Residents", "1,204", 1);
    s.Bar("Coverage", 0.6f, "60 %");
    s.Slider("Tax", &g_tax, 0.0f, 30.0f, 1.0f, "%.0f %%");
}
static void ChangedTest(const SimPanelSpec&, int row, void*) { g_changedRow = row; }

int main() {
    // ---------------------------------------------------------------- the model and its builders
    {
        SimPanelSpec s;
        s.title = "Health";
        s.Stat("Sick", "12", -1);
        s.Bar("Capacity", 1.7f, "170 %"); // clamped to 1
        s.Stacked("Causes", {{"Pollution", 3, Rgba(200, 80, 80)}, {"Age", 1, Rgba(80, 80, 200)}});
        s.Spark("Patients", {1, 2, 3, 2});
        s.List("Hospitals", {{"General", "80/100"}, {"Clinic", "10/20"}}, 1);
        float v = 55.0f;
        SimPanelRow& sl = s.Slider("Funding", &v, 50.0f, 150.0f, 5.0f);
        s.Caption("Note");
        CHECK(s.rows.size() == 7);
        CHECK(s.rows[0].kind == SimRow::Stat && s.rows[0].tone == -1);
        CHECK(Near(s.rows[1].t, 1.0f));
        CHECK(s.rows[3].series.size() == 4);
        CHECK(sl.v == 55.0f && sl.bind == &v);
        SimPanelMetrics m;
        CHECK(SimRowHeight(s.rows[4], m) == m.listHeadH + m.listItemH); // maxItems 1 of 2
        CHECK(SimRowHeight(sl, m) >= 44.0f);                           // a touch target
        float sum = 0;
        for (const auto& r : s.rows) sum += SimRowHeight(r, m);
        CHECK(Near(SimPanelContentHeight(s, m), sum + m.gap * 6));
        m.textScale = 1.3f;
        CHECK(SimRowHeight(s.rows[0], m) > SimRowHeight(s.rows[0], SimPanelMetrics()));
        s.Clear();
        CHECK(s.rows.empty() && s.title.empty());
    }
    // ---------------------------------------------------------------- pure helpers
    {
        CHECK(SimSliderSnap(12.4f, 0, 30, 5) == 10.0f);
        CHECK(SimSliderSnap(13.0f, 0, 30, 5) == 15.0f);
        CHECK(SimSliderSnap(99.0f, 0, 30, 0) == 30.0f);
        CHECK(Near(SimSliderFromX(150, 100, 100, 0, 10, 0), 5.0f));
        CHECK(SimSliderFromX(0, 100, 100, 0, 10, 0) == 0.0f && SimSliderFromX(500, 100, 100, 0, 10, 0) == 10.0f);
        std::vector<float> sh;
        SimStackShares({{"a", 3, 0}, {"b", 1, 0}, {"c", -4, 0}}, sh);
        CHECK(sh.size() == 3 && Near(sh[0], 0.75f) && Near(sh[1], 0.25f) && sh[2] == 0.0f);
        SimStackShares({{"a", 0, 0}}, sh);
        CHECK(sh[0] == 0.0f);
        float lo, hi;
        CHECK(!SimSparkRange({1.0f}, lo, hi));
        CHECK(SimSparkRange({2, -1, 5}, lo, hi) && lo == -1.0f && hi == 5.0f);
        CHECK(SimSparkRange({3, 3}, lo, hi) && hi > lo);
    }
    // ---------------------------------------------------------------- placement: side card, bottom sheet
    {
        SimPanelMetrics m;
        const Rect side = SimPanelFrame(SimPanelPlace::Side, 1024, 768, 200, 120, 0, m);
        CHECK(side.w == m.sideW && side.x + side.w <= 1024 - 56 && side.y >= 60);
        CHECK(Near(side.h, m.headH + m.pad * 2 + 200));
        const Rect tall = SimPanelFrame(SimPanelPlace::Side, 1024, 768, 5000, 120, 0, m);
        CHECK(tall.y + tall.h <= 768 - 120); // clear of the toolbar; the rest scrolls
        const Rect sheet = SimPanelFrame(SimPanelPlace::Sheet, 390, 844, 5000, 90, 50, m); // a phone, portrait
        CHECK(sheet.x >= 0 && sheet.x + sheet.w <= 390 && Near(sheet.x, (390 - sheet.w) * 0.5f));
        CHECK(sheet.y + sheet.h <= 844 - 90 && sheet.h <= 844 * m.sheetMaxFrac + 0.01f && sheet.y >= 50);
        const Rect pad = SimPanelFrame(SimPanelPlace::Sheet, 1366, 1024, 100, 100, 50, m);
        CHECK(pad.w == m.sheetMaxW && Near(pad.y + pad.h, 1024 - 100 - 8));
    }
    // ---------------------------------------------------------------- registry and host
    {
        CHECK(RegisterSimPanel(SimPanelEntry{}) == -1); // no id, no fill
        SimPanelEntry e;
        e.id = "t.test", e.name = "Test", e.fill = FillTest, e.changed = ChangedTest;
        const int i = RegisterSimPanel(e);
        CHECK(i == 0 && RegisterSimPanel(e) == 0 && SimPanelRegistry().size() == 1); // same id replaces
        CHECK(FindSimPanel("t.test") == 0 && FindSimPanel("nope") == -1);
        SimPanelsHost h;
        CHECK(!h.IsOpen());
        h.Toggle(0);
        CHECK(h.open == 0);
        h.Toggle(0);
        CHECK(!h.IsOpen());
        h.Open(7); // out of range: closed
        CHECK(!h.IsOpen());

        // nothing open: nothing drawn, no fill
        Harness hs;
        bool up = true;
        hs.Frame([&](Context& g) { up = SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(!up && g_fills == 0);
        // open: the fill runs, the panel blocks the mouse
        h.Open(0);
        hs.Frame([&](Context& g) { up = SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(up && g_fills == 1 && h.spec.title == "Test" && h.spec.rows.size() == 3);
        // drag the slider (row 2) to the middle of its track: 15, the phase's value and callback follow
        SimPanelMetrics m;
        const Rect f = SimPanelFrame(SimPanelPlace::Side, hs.w, hs.h, SimPanelContentHeight(h.spec, m), 120, 0, m);
        CHECK(hs.g.WantsMouseAt(f.x + 5, f.y + 5));
        const float by0 = f.y + m.pad * 0.5f + m.headH, px = f.x + m.pad, pw = f.w - 2 * m.pad;
        const float ry = by0 + SimRowHeight(h.spec.rows[0], m) + m.gap + SimRowHeight(h.spec.rows[1], m) + m.gap;
        const float cy = ry + SimRowHeight(h.spec.rows[2], m) - 22.0f, tx = px + 11.0f, tw = pw - 22.0f;
        hs.q.Move(tx + tw * 0.5f, cy);
        hs.q.Down(tx + tw * 0.5f, cy);
        hs.Frame([&](Context& g) { SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(g_tax == 15.0f && g_changedRow == 2);
        hs.q.Move(tx + tw, cy); // keep dragging: to the end
        hs.Frame([&](Context& g) { SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(g_tax == 30.0f);
        hs.q.Up(tx + tw, cy);
        hs.Frame([&](Context& g) { SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(h.st.sliderRow == -1);
        // the close button (top right, 44 pt): press and release on it
        const float bx = f.x + f.w - 46.0f + 22.0f, bY = f.y + 2.0f + 22.0f;
        hs.q.Move(bx, bY);
        hs.q.Down(bx, bY);
        hs.Frame([&](Context& g) { SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(h.IsOpen());
        hs.q.Up(bx, bY);
        hs.Frame([&](Context& g) { SimPhasePanelsFrame(g, h, SimPanelPlace::Side, 120); });
        CHECK(!h.IsOpen());
    }
    // ---------------------------------------------------------------- overlays: every planned layer, providers
    {
        static_assert(kOverlayCount == 18, "six built in, twelve planned");
        for (int i = 0; i < kOverlayCount; ++i) {
            CHECK(OverlayName((Overlay)i)[0] != 0);
            CHECK(OverlayAtlasIcon((Overlay)i)[0] != 0);
        }
        CHECK(!OverlayAvailable(Overlay::None) && OverlayName(Overlay::None)[0] == 0);
        for (int i = 0; i < (int)Overlay::FirstProvided; ++i) CHECK(OverlayAvailable((Overlay)i)); // built in
        for (int i = (int)Overlay::FirstProvided; i < kOverlayCount; ++i) { // planned: invisible until provided
            CHECK(!OverlayAvailable((Overlay)i));
            CHECK(OverlayProvidedValue((Overlay)i, 0, 0, 0) < 0.0f);
        }
        CHECK(!RegisterOverlayProvider(Overlay::Power, [](void*, int, float, float) { return 1.0f; })); // built in: refused
        static float scale = 0.5f;
        const uint32_t v0 = OverlayProvidersVersion();
        CHECK(RegisterOverlayProvider(Overlay::Crime, [](void* u, int cell, float, float) { return cell * *(float*)u; }, &scale, "Safe .. Dangerous"));
        CHECK(OverlayProvidersVersion() != v0);
        CHECK(OverlayAvailable(Overlay::Crime) && !OverlayAvailable(Overlay::Health));
        CHECK(Near(OverlayProvidedValue(Overlay::Crime, 1, 0, 0), 0.5f));
        CHECK(OverlayProvidedValue(Overlay::Crime, 9, 0, 0) == 1.0f); // clamped
        CHECK(OverlayProvidedValue(Overlay::Crime, -1, 0, 0) < 0.0f); // left out
        CHECK(std::string(FindOverlayProvider(Overlay::Crime)->legend) == "Safe .. Dangerous");
        RegisterOverlayProvider(Overlay::Crime, [](void*, int, float, float) { return std::nanf(""); });
        CHECK(OverlayProvidedValue(Overlay::Crime, 1, 0, 0) < 0.0f); // NaN is left out
        RegisterOverlayProvider(Overlay::Crime, nullptr);
        CHECK(!OverlayAvailable(Overlay::Crime));
        // high-is-bad layers run green .. red; the others red .. green
        CHECK(OverlayHighIsBad(Overlay::AirPollution) && !OverlayHighIsBad(Overlay::Education));
        CHECK(OverlayRamp(Overlay::AirPollution, 1.0f).r > OverlayRamp(Overlay::AirPollution, 1.0f).g);
        CHECK(OverlayRamp(Overlay::Education, 1.0f).g > OverlayRamp(Overlay::Education, 1.0f).r);
        const Rgb d0 = OverlayRamp(Overlay::Districts, 0.1f), d1 = OverlayRamp(Overlay::Districts, 0.6f);
        CHECK(std::fabs(d0.r - d1.r) + std::fabs(d0.g - d1.g) + std::fabs(d0.b - d1.b) > 0.3f);
        // the toggle column draws no button for an unprovided layer: same block count either way
        Harness hs;
        OverlayHudState st;
        st.on = Overlay::Goods; // on but unprovided: no legend
        hs.Frame([&](Context& g) { OverlayHudFrame(g, st, 300); });
        CHECK(!hs.g.WantsMouseAt(1024 - 72 - 125, 310)); // where the legend would be
        RegisterOverlayProvider(Overlay::Goods, [](void*, int, float, float) { return 0.5f; });
        hs.Frame([&](Context& g) { OverlayHudFrame(g, st, 300); });
        CHECK(hs.g.WantsMouseAt(1024 - 72 - 125, 310)); // provided: the legend shows
        RegisterOverlayProvider(Overlay::Goods, nullptr);
    }

    std::printf("sim_panels_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
