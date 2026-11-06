// Tests for src/gui/gui.h (the in-game immediate-mode GUI) and src/gui/terrain_toolbar.h. Headless: no window, no renderer.
// Each "frame" below is what the engine does once per rendered frame: Take() the queued input, BeginFrame, widgets, EndFrame.
#include "../src/gui/gui.h"
#include "../src/gui/terrain_toolbar.h"

#ifdef _WINDOWS_
#error "the GUI core must not pull in windows.h"
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
static bool Near(float a, float b, float eps = 0.51f) { return std::fabs(a - b) <= eps; }

using namespace tbd::gui;

struct Harness {
    Context g;
    InputQueue q;
    float w = 800, h = 600, dpi = 1.0f;
    void Frame(const std::function<void(Context&)>& ui) {
        g.BeginFrame(q.Take(w, h, dpi));
        ui(g);
        g.EndFrame();
    }
};

// A fixed 200 px panel at (10, 20) from the top-left with a title, two buttons and a two-column row.
static Rect b1, b2, c1, c2;
static bool click1 = false, clickC2 = false;
static void SimplePanel(Context& g) {
    g.BeginPanel("TEST", Anchor::TopLeft, 10, 20, 200);
    click1 = g.Button("ONE");
    b1 = g.LastItem();
    g.Button("TWO");
    b2 = g.LastItem();
    g.BeginRow(2);
    g.Button("A");
    c1 = g.LastItem();
    clickC2 = g.Button("B");
    c2 = g.LastItem();
    g.EndRow();
    g.EndPanel();
}

static void Layout() {
    Harness t;
    t.Frame(SimplePanel);
    const Theme& th = t.g.Style();
    CHECK(Near(th.hitSize, 44.0f));
    CHECK(Near(th.padding, 12.0f) && Near(th.gap, 8.0f)); // the 8 px grid
    const float pad = th.padding, gap = th.gap, inner = 200 - 2 * pad;
    // Content starts at panel + padding, half a padding below the title strip.
    CHECK(Near(b1.x, 10 + pad) && Near(b1.y, 20 + th.titleHeight + pad * 0.5f) && Near(b1.w, inner) && Near(b1.h, 44));
    CHECK(Near(b2.x, b1.x) && Near(b2.y, b1.y + 44 + gap));
    // Equal columns with one gap between them.
    const float colW = (inner - gap) / 2.0f;
    CHECK(Near(c1.x, 10 + pad) && Near(c1.y, b2.y + 44 + gap) && Near(c1.w, colW, 1.0f));
    CHECK(Near(c2.x, 10 + pad + colW + gap, 1.0f) && Near(c2.y, c1.y) && Near(c2.x + c2.w, 10 + pad + inner, 1.0f));
    // Auto height: last row bottom + padding.
    CHECK(t.g.ShownPanels().size() == 1);
    const Rect p = t.g.ShownPanels()[0];
    CHECK(Near(p.x, 10) && Near(p.y, 20) && Near(p.w, 200) && Near(p.h, (c1.y + 44 + pad) - 20));
    // Minimum hit size for everything pressable.
    CHECK(b1.h >= 44 && c1.h >= 44);
}

static void HitTesting() {
    Harness t;
    t.Frame(SimplePanel);
    const Rect p = t.g.ShownPanels()[0];
    CHECK(t.g.WantsMouseAt(p.x + 1, p.y + 1));
    CHECK(t.g.WantsMouseAt(p.x + p.w - 1, p.y + p.h - 1));
    CHECK(!t.g.WantsMouseAt(p.x - 1, p.y + 5));
    CHECK(!t.g.WantsMouseAt(p.x + p.w + 1, p.y + 5));
    CHECK(!t.g.WantsMouseAt(400, 400));
    // WantsMouse after the frame follows the pointer.
    t.q.Move(b1.x + 5, b1.y + 5);
    t.Frame(SimplePanel);
    CHECK(t.g.WantsMouse());
    CHECK(t.g.LastItemHovered() == false); // last item is B; the pointer is on ONE
    t.q.Move(600, 500);
    t.Frame(SimplePanel);
    CHECK(!t.g.WantsMouse());
    // Reset (GUI hidden): nothing is wanted any more.
    t.g.Reset();
    CHECK(!t.g.WantsMouseAt(p.x + 1, p.y + 1));
}

static void ButtonClick() {
    Harness t;
    t.Frame(SimplePanel);
    const float x = b1.x + 10, y = b1.y + 10;
    // Press inside, release inside: one click, on the release frame.
    t.q.Down(x, y);
    t.Frame(SimplePanel);
    CHECK(!click1);
    CHECK(t.g.Capturing());
    t.Frame(SimplePanel); // held
    CHECK(!click1);
    t.q.Up(x + 3, y + 3);
    t.Frame(SimplePanel);
    CHECK(click1);
    CHECK(!t.g.Capturing());
    t.Frame(SimplePanel);
    CHECK(!click1);
    // Press inside, release outside the button: no click.
    t.q.Down(x, y);
    t.Frame(SimplePanel);
    t.q.Up(x, b2.y + 10); // over TWO
    t.Frame(SimplePanel);
    CHECK(!click1);
    // A click faster than a frame (press and release queued together) still counts.
    t.q.Down(x, y);
    t.q.Up(x, y);
    t.Frame(SimplePanel);
    CHECK(click1);
    // Press elsewhere, slide onto the button, release: no click.
    t.q.Down(c2.x + 5, c2.y + 5);
    t.Frame(SimplePanel);
    t.q.Move(x, y);
    t.Frame(SimplePanel);
    t.q.Up(x, y);
    t.Frame(SimplePanel);
    CHECK(!click1);
    // A press the host kept for itself (claim = false: an engine drag) never clicks, even released over the button.
    t.q.Down(x, y, false);
    t.Frame(SimplePanel);
    CHECK(!t.g.Capturing());
    t.q.Up(x, y);
    t.Frame(SimplePanel);
    CHECK(!click1);
    // Column button in a row.
    t.q.Down(c2.x + 5, c2.y + 5);
    t.q.Up(c2.x + 5, c2.y + 5);
    t.Frame(SimplePanel);
    CHECK(clickC2);
    // A press on the panel's empty space holds the pointer until release, and clicks nothing.
    const Rect p = t.g.ShownPanels()[0];
    t.q.Down(p.x + 3, p.y + 3);
    t.Frame(SimplePanel);
    CHECK(t.g.Capturing());
    t.q.Move(700, 500);
    t.Frame(SimplePanel);
    CHECK(t.g.WantsMouse());
    t.q.Up(700, 500);
    t.Frame(SimplePanel);
    CHECK(!t.g.WantsMouse() && !t.g.Capturing());
}

static float sliderValue = 10.0f, logValue = 30.0f;
static Rect sliderRect, logRect;
static void SliderPanel(Context& g) {
    g.BeginPanel("S", Anchor::TopLeft, 0, 0, 220);
    g.SliderFloat("LIN", &sliderValue, 0.0f, 100.0f, "%.0f");
    sliderRect = g.LastItem();
    g.SliderFloat("LOG", &logValue, 4.0f, 300.0f, "%.0f M", SliderScale::Log);
    logRect = g.LastItem();
    g.EndPanel();
}

static void SliderDrag() {
    Harness t;
    t.Frame(SliderPanel);
    const float pad = t.g.Style().rowPad;
    const float trackX = sliderRect.x + pad, trackW = sliderRect.w - 2 * pad;
    const float y = sliderRect.y + sliderRect.h * 0.5f;
    // Press at a quarter of the track: the value jumps there.
    t.q.Down(trackX + trackW * 0.25f, y);
    t.Frame(SliderPanel);
    CHECK(Near(sliderValue, 25.0f, 0.01f));
    // Drag right, past the panel's edge: still captured, clamped to max.
    const Rect p = t.g.ShownPanels()[0];
    t.q.Move(p.x + p.w + 300, y + 200);
    t.Frame(SliderPanel);
    CHECK(Near(sliderValue, 100.0f, 0.01f));
    CHECK(t.g.WantsMouse());             // outside every panel, but the drag owns the pointer
    CHECK(t.g.WantsMouseAt(780, 590));   // and window messages anywhere go to the GUI
    // Back inside, to the middle.
    t.q.Move(trackX + trackW * 0.5f, y + 200); // vertical position does not matter while dragging
    t.Frame(SliderPanel);
    CHECK(Near(sliderValue, 50.0f, 0.01f));
    // Drag far left: min.
    t.q.Move(-500, y);
    t.Frame(SliderPanel);
    CHECK(Near(sliderValue, 0.0f, 0.01f));
    // Release outside: the drag ends, the GUI lets go.
    t.q.Up(700, 500);
    t.Frame(SliderPanel);
    CHECK(!t.g.WantsMouse() && !t.g.Capturing());
    const float after = sliderValue;
    t.q.Move(trackX + trackW, y);
    t.Frame(SliderPanel);
    CHECK(sliderValue == after); // hovering does not change it
    // Wheel over the slider steps 2% of the range per notch.
    sliderValue = 50.0f;
    t.q.Move(trackX + 5, y);
    t.q.Wheel(1.0f);
    t.Frame(SliderPanel);
    CHECK(Near(sliderValue, 52.0f, 0.01f));
    // Log scale: the middle of the track is the geometric mean of the range.
    const float ly = logRect.y + logRect.h * 0.5f;
    t.q.Down(trackX + trackW * 0.5f, ly);
    t.Frame(SliderPanel);
    CHECK(Near(logValue, std::sqrt(4.0f * 300.0f), 0.05f));
    t.q.Up(trackX + trackW * 0.5f, ly);
    t.Frame(SliderPanel);
    CHECK(Near(logValue, std::sqrt(4.0f * 300.0f), 0.05f));
}

static bool toggleValue = false;
static int choiceValue = 0;
static Rect toggleRect, choiceRect;
static bool toggleChanged = false, choiceChanged = false;
static void TogglePanel(Context& g) {
    g.BeginPanel("T", Anchor::TopLeft, 0, 0, 300);
    toggleChanged = g.Toggle(toggleValue ? "ON##t" : "OFF##t", &toggleValue);
    toggleRect = g.LastItem();
    static const char* const opts[] = {"RAISE", "DIG", "FLATTEN"};
    choiceChanged = g.Choice("MODE", opts, 3, &choiceValue);
    choiceRect = g.LastItem();
    g.EndPanel();
}

static void ToggleAndChoice() {
    Harness t;
    t.Frame(TogglePanel);
    const float x = toggleRect.x + 20, y = toggleRect.y + 20;
    t.q.Down(x, y);
    t.q.Up(x, y);
    t.Frame(TogglePanel);
    CHECK(toggleChanged && toggleValue);
    // Its label changed ("ON##t"), its id did not: a second click flips it back.
    t.q.Down(x, y);
    t.Frame(TogglePanel);
    t.q.Up(x, y);
    t.Frame(TogglePanel);
    CHECK(toggleChanged && !toggleValue);
    t.Frame(TogglePanel);
    CHECK(!toggleChanged && !toggleValue);
    // Third segment.
    const float sx = choiceRect.x + choiceRect.w * 5.0f / 6.0f, sy = choiceRect.y + 10;
    t.q.Down(sx, sy);
    t.q.Up(sx, sy);
    t.Frame(TogglePanel);
    CHECK(choiceChanged && choiceValue == 2);
    // Clicking the chosen segment again changes nothing.
    t.q.Down(sx, sy);
    t.q.Up(sx, sy);
    t.Frame(TogglePanel);
    CHECK(!choiceChanged && choiceValue == 2);
    // First segment.
    t.q.Down(choiceRect.x + 5, sy);
    t.q.Up(choiceRect.x + 5, sy);
    t.Frame(TogglePanel);
    CHECK(choiceChanged && choiceValue == 0);
    CHECK(choiceRect.h >= 44);
}

static void Ids() {
    Context g;
    CHECK(g.GetId("SAVE##a") != g.GetId("SAVE##b"));
    CHECK(g.GetId("ON##x") != g.GetId("OFF##y"));
    const tbd::gui::Id outer = g.GetId("X");
    g.PushId("scope");
    CHECK(g.GetId("X") != outer);
    g.PopId();
    CHECK(g.GetId("X") == outer);
    g.PushId(3);
    const tbd::gui::Id three = g.GetId("X");
    g.PopId();
    g.PushId(4);
    CHECK(g.GetId("X") != three);
    g.PopId();
}

// Anchoring at two viewport sizes and two DPI scales. Fixed size: exact on the first frame; auto size: from the second.
static void Anchoring() {
    for (float dpi : {1.0f, 2.0f}) {
        for (int size = 0; size < 2; ++size) {
            Harness t;
            t.w = size == 0 ? 975.0f : 1920.0f;
            t.h = size == 0 ? 483.0f : 1080.0f;
            t.dpi = dpi;
            auto ui = [](Context& g) {
                g.BeginPanel("##tl", Anchor::TopLeft, 8, 8, 100, 50);
                g.EndPanel();
                g.BeginPanel("##br", Anchor::BottomRight, 8, 8, 100, 50);
                g.EndPanel();
                g.BeginPanel("##bc", Anchor::BottomCenter, 0, 10, 100, 50);
                g.EndPanel();
                g.BeginPanel("##cl", Anchor::CenterLeft, 8, 0, 100, 50);
                g.EndPanel();
                g.BeginPanel("##tr auto", Anchor::TopRight, 8, 8); // auto size: one label
                g.Label("AUTO SIZED");
                g.EndPanel();
            };
            t.Frame(ui);
            t.Frame(ui);
            const auto& p = t.g.ShownPanels();
            CHECK(p.size() == 5);
            if (p.size() != 5) continue;
            const float s = dpi, W = t.w, H = t.h;
            CHECK(Near(p[0].x, 8 * s) && Near(p[0].y, 8 * s) && Near(p[0].w, 100 * s) && Near(p[0].h, 50 * s));
            CHECK(Near(p[1].x, W - 8 * s - 100 * s) && Near(p[1].y, H - 8 * s - 50 * s));
            CHECK(Near(p[2].x, (W - 100 * s) / 2) && Near(p[2].y, H - 10 * s - 50 * s));
            CHECK(Near(p[3].x, 8 * s) && Near(p[3].y, (H - 50 * s) / 2));
            CHECK(Near(p[4].x + p[4].w, W - 8 * s) && Near(p[4].y, 8 * s));
            const Theme th = t.g.Style();
            CHECK(Near(p[4].w, t.g.MeasureText("AUTO SIZED") + 2 * th.padding, 1.0f)); // the text + padding
            CHECK(Near(t.g.MeasureText("AUTO SIZED"), tbd::gui::MeasureSdfText("AUTO SIZED", 10, th.fontSize), 0.01f));
            CHECK(th.hitSize >= 44 * s - 0.5f);
            // Vertex output: non-empty, finite, on screen (shadows may spill their blur past the edge).
            const auto& v = t.g.UiVertices();
            CHECK(!v.empty() && v.size() % 6 == 0);
            CHECK(t.g.Vertices().empty()); // smooth by default: nothing on the classic list
            bool finite = true, inside = true;
            const float spill = th.shadowBlur + th.shadowOffset + 2;
            for (const tbd::UiVertex& x : v) {
                finite = finite && std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.localX) && std::isfinite(x.localY) &&
                         std::isfinite(x.a) && std::isfinite(x.b) && std::isfinite(x.radius) && std::isfinite(x.softness);
                inside = inside && x.x >= -spill && x.y >= -spill && x.x <= W + spill && x.y <= H + spill;
            }
            CHECK(finite);
            CHECK(inside);
        }
    }
    // A panel too close to an edge is pushed back on screen; insets keep it out of the host's areas.
    Harness t;
    t.w = 300, t.h = 200;
    auto ui = [](Context& g) {
        g.BeginPanel("##big", Anchor::BottomRight, -50, -50, 120, 80);
        g.EndPanel();
        g.BeginPanel("##inset", Anchor::TopLeft, 0, 0, 50, 50);
        g.EndPanel();
    };
    t.g.SetInsets(10, 40, 0, 0);
    t.Frame(ui);
    const auto& p = t.g.ShownPanels();
    CHECK(Near(p[0].x + p[0].w, 300) && Near(p[0].y + p[0].h, 200));
    CHECK(Near(p[1].x, 10) && Near(p[1].y, 40));
}

// The vertex vector is reused: same storage once warm, so no per-frame heap churn.
static void NoChurn() {
    Harness t;
    t.Frame(SimplePanel);
    t.Frame(SimplePanel);
    const tbd::UiVertex* before = t.g.UiVertices().data();
    const size_t n = t.g.UiVertices().size();
    for (int i = 0; i < 10; ++i) t.Frame(SimplePanel);
    CHECK(t.g.UiVertices().data() == before && t.g.UiVertices().size() == n);
    t.g.SetClassic(true); // the classic list too
    t.Frame(SimplePanel);
    t.Frame(SimplePanel);
    const tbd::Vertex* flat = t.g.Vertices().data();
    const size_t fn = t.g.Vertices().size();
    for (int i = 0; i < 10; ++i) t.Frame(SimplePanel);
    CHECK(fn > 0 && t.g.Vertices().data() == flat && t.g.Vertices().size() == fn);
}

// The terrain toolbar: bound values, actions, and everything inside its panels at the capture size (975 x 483) and the
// same window at 150% scale.
static void TerrainToolbarTest() {
    for (int run = 0; run < 4; ++run) {
        const float dpi = run % 2 ? 1.5f : 1.0f;
        const bool classic = run >= 2; // the same logical window at both scales: 975 x 483 at 96 dpi, 1463 x 725 at 144 dpi
        Harness t;
        t.w = std::round(975 * dpi), t.h = std::round(483 * dpi), t.dpi = dpi;
        t.g.SetClassic(classic);
        bool on = true;
        int mode = 0;
        float radius = 30.0f, strength = 6.0f;
        TerrainToolbarState s{&on, &mode, &radius, &strength};
        TerrainStatus st{true, -1234.0f, 2345.0f, 123.4f, 17};
        TerrainAction action = TerrainAction::None;
        auto ui = [&](Context& g) { action = TerrainToolbar(g, s, st); };
        t.Frame(ui);
        t.Frame(ui);
        const auto panels = t.g.ShownPanels();
        CHECK(panels.size() == 2);
        if (panels.size() != 2) continue;
        const Rect tool = panels[0], status = panels[1];
        // On screen, not overlapping each other.
        for (const Rect& p : panels) CHECK(p.x >= 0 && p.y >= 0 && p.x + p.w <= t.w && p.y + p.h <= t.h);
        const bool overlap = tool.x < status.x + status.w && status.x < tool.x + tool.w && tool.y < status.y + status.h && status.y < tool.y + tool.h;
        CHECK(!overlap);
        // Every vertex inside one of the panels (one pixel of anti-aliasing margin): no text spills out. Shadows excepted.
        bool contained = true;
        auto inPanel = [&](float x, float y) {
            for (const Rect& p : panels)
                if (x >= p.x - 2 && x <= p.x + p.w + 2 && y >= p.y - 2 && y <= p.y + p.h + 2) return true;
            return false;
        };
        for (const tbd::Vertex& v : t.g.Vertices()) contained = contained && inPanel(v.px, v.py);
        for (const tbd::UiVertex& v : t.g.UiVertices()) contained = contained && (v.mode == (float)tbd::UiMode::Shadow || inPanel(v.x, v.y));
        CHECK(contained);
        CHECK(classic ? !t.g.Vertices().empty() && t.g.UiVertices().empty() : !t.g.UiVertices().empty() && t.g.Vertices().empty());
        // No label is cut: "Flatten" fits its segment in both modes (it may shrink, never lose letters).
        {
            const Theme& sty = t.g.Style();
            const float segW = (tool.w - 2 * sty.padding - 2 * std::round(3.0f * dpi)) / 3.0f;
            const float textRoom = segW - 2 * std::round(4.0f * dpi) - sty.iconSize - sty.iconGap;
            const float minSize = classic ? 7.0f : sty.minFontSize;
            CHECK(t.g.Draw().TextWidth("Flatten", 7, minSize) <= textRoom);
        }
        // Locate widgets by walking the layout the toolbar uses: title, then rows of hitSize + gap.
        const Theme th = t.g.Style();
        const float x0 = tool.x + th.padding, w = tool.w - 2 * th.padding;
        const float row0 = tool.y + th.titleHeight + th.padding * 0.5f, step = th.hitSize + th.gap;
        auto click = [&](float x, float y) {
            t.q.Down(x, y);
            t.q.Up(x, y);
            t.Frame(ui);
        };
        click(x0 + w * 0.5f, row0 + 10); // DIG
        CHECK(mode == 1);
        click(x0 + w - 12, row0 + 10); // FLATTEN (clear of the well's inset)
        CHECK(mode == 2);
        // Brush size: press at the right end of its track -> 300.
        t.q.Down(x0 + w - th.rowPad, row0 + step + th.hitSize * 0.7f);
        t.Frame(ui);
        CHECK(Near(radius, 300.0f, 0.01f));
        t.q.Move(x0 + th.rowPad, row0 + step + 20); // left end -> 4
        t.Frame(ui);
        CHECK(Near(radius, 4.0f, 0.01f));
        t.q.Up(x0 + th.rowPad, row0 + step + 20);
        t.Frame(ui);
        // [ and ] change g_terrainEdit.radius directly; the slider just shows the new value next frame.
        radius = 75.0f;
        t.Frame(ui);
        CHECK(radius == 75.0f);
        // Strength at the middle: 10.5.
        t.q.Down(x0 + w * 0.5f, row0 + 2 * step + 30);
        t.q.Up(x0 + w * 0.5f, row0 + 2 * step + 30);
        t.Frame(ui);
        CHECK(Near(strength, 10.5f, 0.2f));
        click(x0 + 20, row0 + 3 * step + 20); // EDIT toggle
        CHECK(!on);
        click(x0 + 20, row0 + 3 * step + 20);
        CHECK(on);
        click(x0 + 10, row0 + 4 * step + 20); // SAVE
        CHECK(action == TerrainAction::Save);
        t.Frame(ui);
        CHECK(action == TerrainAction::None);
        click(x0 + w - 10, row0 + 4 * step + 20); // LOAD
        CHECK(action == TerrainAction::Load);
        // The toolbar's bottom is the LOAD row.
        CHECK(Near(tool.y + tool.h, row0 + 4 * step + th.hitSize + th.padding, 1.0f));
    }
}

// The SDF font's measurement: what layout uses must be what DrawList::Text draws.
static void FontMeasurement() {
    using namespace tbd::gui;
    const float a = SdfAdvance(FontFace::Regular, 'A'), b = SdfAdvance(FontFace::Regular, 'b');
    CHECK(a > 0.3f && a < 1.0f && b > 0.3f && b < 1.0f); // em
    CHECK(Near(MeasureSdfText("Ab", 2, 15.0f), (a + b + SdfKerning(FontFace::Regular, 'A', 'b')) * 15.0f, 0.001f));
    CHECK(Near(MeasureSdfText("Terrain", 7, 30.0f), 2.0f * MeasureSdfText("Terrain", 7, 15.0f), 0.01f)); // linear in size
    CHECK(MeasureSdfText("Terrain", 7, 15.0f, FontFace::Bold) > MeasureSdfText("Terrain", 7, 15.0f)); // bold is wider
    CHECK(MeasureSdfText("", 0, 15.0f) == 0.0f);
    CHECK(MeasureSdfText("iiii", 4, 15.0f) < MeasureSdfText("WWWW", 4, 15.0f)); // proportional, unlike the pixel font
    CHECK(Near(MeasureSdfText("\x01", 1, 10.0f), SdfAdvance(FontFace::Regular, ' ') * 10.0f, 0.001f)); // unknown = space
    const float cap = SdfCapHeight(15.0f);
    CHECK(cap > 9.0f && cap < 12.0f);
    // Glyph data: inside the atlas, quads above the baseline for capitals, below it for descenders.
    bool inAtlas = true;
    for (int f = 0; f < font_data::kFaces; ++f)
        for (int c = font_data::kFirst; c <= font_data::kLast; ++c) {
            const font_data::Glyph* g = FindSdfGlyph((FontFace)f, (char)c);
            inAtlas = inAtlas && g && g->u1 <= font_data::kAtlasWidth && g->v1 <= font_data::kAtlasHeight && g->u0 <= g->u1 && g->v0 <= g->v1;
        }
    CHECK(inAtlas);
    CHECK(FindSdfGlyph(FontFace::Regular, 'H')->y0 < -0.6f);
    CHECK(FindSdfGlyph(FontFace::Regular, 'g')->y1 > 0.15f);
    CHECK(FindSdfGlyph(FontFace::Regular, ' ')->x1 == FindSdfGlyph(FontFace::Regular, ' ')->x0); // nothing to draw
    // Classic measurement: the pixel font, 6 cells per character minus the trailing gap.
    DrawList classic;
    classic.Begin(true);
    CHECK(Near(classic.TextWidth("SAVE", 4, 14.0f), 4 * 6 * 2.0f - 2.0f, 0.001f));
    CHECK(Near(classic.CapHeight(14.0f), 14.0f, 0.001f));
}

// DrawList primitives: one quad per primitive, one per visible glyph, correct modes, colours and placeholder patching.
static void DrawListPrimitives() {
    using namespace tbd::gui;
    DrawList dl;
    dl.Begin(false);
    dl.Rect(10, 20, 110, 60, Rgba(255, 0, 0), Rgba(0, 0, 255), 8.0f, 1.0f, Rgba(0, 255, 0));
    CHECK(dl.Ui().size() == 6);
    const tbd::UiVertex& v0 = dl.Ui()[0];
    CHECK(v0.mode == (float)tbd::UiMode::Rect && Near(v0.a, 50.0f, 0.001f) && Near(v0.b, 20.0f, 0.001f) && v0.radius == 8.0f && v0.border == 1.0f);
    CHECK(Near(v0.x, 9.0f, 0.001f) && Near(v0.localX, -51.0f, 0.001f)); // one pixel of anti-aliasing margin, local from the centre
    CHECK(dl.Ui()[0].fill == Rgba(255, 0, 0) && dl.Ui()[2].fill == Rgba(0, 0, 255)); // top / bottom of the gradient
    dl.Text(0, 20, 15.0f, FontFace::Regular, "A b", 3, Rgba(255, 255, 255));
    CHECK(dl.Ui().size() == 6 + 2 * 6); // the space draws nothing
    CHECK(dl.Ui()[6].mode == (float)tbd::UiMode::Text && dl.Ui()[6].localX >= 0.0f && dl.Ui()[6].localX <= 1.0f);
    dl.Line(0, 0, 10, 10, 2.0f, Rgba(1, 2, 3));
    CHECK(dl.Ui().back().mode == (float)tbd::UiMode::Line && Near(dl.Ui().back().radius, 1.0f, 0.001f));
    dl.Shadow(0, 0, 10, 10, 4.0f, 6.0f, Rgba(0, 0, 0, 100));
    CHECK(dl.Ui().back().mode == (float)tbd::UiMode::Shadow && dl.Ui().back().softness == 6.0f && Near(dl.Ui().back().x, -7.0f, 0.001f));
    const size_t at = dl.Reserve(2);
    CHECK(dl.Ui()[at].x == 0.0f && dl.Ui()[at + 5].x == 0.0f); // zero-area until filled
    dl.RectAt(at + dl.SlotSize(), 0, 0, 20, 20, Rgba(9, 9, 9), Rgba(9, 9, 9), 4.0f);
    CHECK(dl.Ui()[at + 6].mode == (float)tbd::UiMode::Rect && Near(dl.Ui()[at + 7].x, 21.0f, 0.001f));
    // Classic: flat quads, alpha 1 (the old path reads vertex alpha as a material marker), shadows dropped.
    dl.Begin(true);
    dl.Shadow(0, 0, 10, 10, 4, 6, Rgba(0, 0, 0, 100));
    CHECK(dl.Flat().empty());
    dl.Rect(0, 0, 10, 10, Rgba(255, 0, 0, 100), 3.0f);
    CHECK(dl.Flat().size() == 12 && dl.Flat()[6].a == 1.0f && Near(dl.Flat()[6].r, 1.0f, 0.001f));
    dl.Text(0, 20, 14.0f, FontFace::Regular, "I", 1, Rgba(255, 255, 255));
    CHECK(dl.Flat().size() > 12);
    // Colour helpers.
    CHECK(Mix(Rgba(0, 0, 0, 0), Rgba(255, 255, 255, 255), 0.5f) == Rgba(128, 128, 128, 128));
    CHECK(Mix(Rgba(10, 20, 30), Rgba(200, 200, 200), 2.0f) == Rgba(200, 200, 200)); // clamped
    CHECK(Fade(Rgba(1, 2, 3, 200), 0.5f) == Rgba(1, 2, 3, 100));
}

// Smooth and classic share the layout and hit areas: only the drawing differs.
static void ClassicMatchesSmooth() {
    Harness s, c;
    c.g.SetClassic(true);
    for (int i = 0; i < 2; ++i) {
        s.Frame(SimplePanel);
        const Rect sb1 = b1, sc2 = c2;
        c.Frame(SimplePanel);
        CHECK(Near(sb1.x, b1.x) && Near(sb1.y, b1.y) && Near(sb1.w, b1.w) && Near(sb1.h, b1.h));
        CHECK(Near(sc2.x, c2.x) && Near(sc2.y, c2.y) && Near(sc2.w, c2.w));
    }
    CHECK(s.g.ShownPanels().size() == 1 && c.g.ShownPanels().size() == 1);
    CHECK(Near(s.g.ShownPanels()[0].h, c.g.ShownPanels()[0].h));
    // Click behaviour is the same in classic.
    c.q.Down(b1.x + 5, b1.y + 5);
    c.q.Up(b1.x + 5, b1.y + 5);
    c.Frame(SimplePanel);
    CHECK(click1);
}

// Transitions ease towards their target over a few frames and never change hit areas; the first frame starts settled.
static void Animations() {
    Harness t;
    bool on = false;
    Rect r;
    size_t knobIndex = 0;
    float knobX = 0.0f;
    auto ui = [&](Context& g) {
        g.BeginPanel("A", Anchor::TopLeft, 0, 0, 300);
        g.Toggle("Switch", &on);
        r = g.LastItem();
        g.EndPanel();
        // The knob is the last full circle drawn: find it (mode Rect with radius = half size).
        const auto& v = g.UiVertices();
        for (size_t i = 0; i + 5 < v.size(); i += 6)
            if (v[i].mode == (float)tbd::UiMode::Rect && v[i].radius > 0 && Near(v[i].radius, v[i].a, 0.01f) && Near(v[i].a, v[i].b, 0.01f)) knobIndex = i;
        knobX = (v[knobIndex].x + v[knobIndex + 1].x) * 0.5f;
    };
    t.Frame(ui);
    const float offX = knobX;
    const Rect before = r;
    t.q.Down(r.x + 10, r.y + 10);
    t.q.Up(r.x + 10, r.y + 10);
    t.Frame(ui);
    CHECK(on);
    const float firstStep = knobX;
    CHECK(firstStep > offX); // started moving...
    for (int i = 0; i < 60; ++i) t.Frame(ui);
    const float settled = knobX;
    CHECK(settled > firstStep + 1.0f); // ...and kept going
    t.Frame(ui);
    CHECK(Near(knobX, settled, 0.01f)); // and stopped
    CHECK(Near(r.x, before.x) && Near(r.y, before.y) && Near(r.w, before.w) && Near(r.h, before.h));
    // A fresh context starts at the target: no animation on the first frame.
    Harness fresh;
    on = true;
    fresh.Frame(ui);
    CHECK(Near(knobX, settled, 0.5f));
}

int main() {
    Layout();
    HitTesting();
    ButtonClick();
    SliderDrag();
    ToggleAndChoice();
    Ids();
    Anchoring();
    NoChurn();
    TerrainToolbarTest();
    FontMeasurement();
    DrawListPrimitives();
    ClassicMatchesSmooth();
    Animations();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
