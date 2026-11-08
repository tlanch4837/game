// Headless checks for the road / rail touch toolkit (src/gui/road_toolkit.h) on an iPad-sized viewport: taps on its
// chevrons lower as well as raise (through ground level into a tunnel and back), press-and-hold repeats, the ground
// level button, step size, tool modes, snapping, the palette's tabs (rail uses the same panel) and the collapsed layout.
// Build and run: tests\road_toolkit_test.bat
#include "../src/gui/road_toolkit.h"

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
namespace rk = tbd::gui::road_kit;

struct Harness {
    Context g;
    InputQueue q;
    dock::Atlas atlas; // empty: icons are skipped, layout and taps are the same
    city::RoadTool tool;
    rk::State st;
    rk::Layout L;
    rk::Out out;
    float w = 2360, h = 1640, dpi = 2.0f; // iPad Air 11" in pixels, 2 px per point
    float bottom = 146;                    // over the PC UI placement bar
    bool tablet = true;
    void Frame() {
        g.BeginFrame(q.Take(w, h, dpi));
        L = rk::Frame(g, atlas, tool, st, bottom, tablet, out);
        g.EndFrame();
    }
    // a tap at a point (points)
    void Tap(float x, float y) {
        q.Down(x * dpi, y * dpi);
        Frame();
        q.Up(x * dpi, y * dpi);
        Frame();
        q.Leave(); // a finger lifted: no position
        Frame();
    }
    void Hold(float x, float y, int frames) {
        q.Down(x * dpi, y * dpi);
        for (int i = 0; i < frames; ++i) Frame();
        q.Up(x * dpi, y * dpi);
        Frame();
        q.Leave();
        Frame();
    }
    // the rows' tops (points), as road_toolkit.h lays them out
    float Row(int index) const { // 0 = the first row under the header
        return L.panelY + rk::kPad + rk::kB + rk::kRowGap + index * (rk::kLabelH + rk::kB + rk::kRowGap) + rk::kLabelH;
    }
    float Slot(int i) const { return L.panelX + rk::kPad + i * (rk::kB + rk::kGap) + rk::kB * 0.5f; }
    float RightSlot(int fromRight) const { return L.panelX + rk::kPanelW - rk::kPad - fromRight * (rk::kB + rk::kGap) - rk::kB * 0.5f; }
};

int main() {
    Harness t;
    t.tool.Open();
    t.Frame();
    CHECK(t.L.panelW == rk::kPanelW && t.L.palette);
    CHECK(t.L.panelY > 0 && t.L.panelY + t.L.panelH <= t.h / t.dpi - t.bottom + 0.5f);
    CHECK(t.L.palX >= t.L.panelX + t.L.panelW && t.L.palX + t.L.palW <= t.w / t.dpi);
    CHECK(t.g.WantsMouseAt((t.L.panelX + 10) * t.dpi, (t.L.panelY + 10) * t.dpi)); // taps there are the GUI's, not the road's
    CHECK(t.g.WantsMouseAt((t.L.palX + 10) * t.dpi, (t.L.palY + 10) * t.dpi));

    // ---- elevation (row 1 when expanded): down chevron, readout, up chevron ... step, 0 m
    const float ey = t.Row(1) + rk::kB * 0.5f;
    const float downX = t.Slot(0), upX = t.L.panelX + rk::kPad + rk::kB + rk::kGap + 132 + rk::kGap + rk::kB * 0.5f;
    const float zeroX = t.RightSlot(0), stepX = t.RightSlot(1);
    CHECK(t.tool.stepIdx == 1 && t.tool.elevation == 0);
    t.Tap(downX, ey);
    CHECK(t.tool.elevation == -5.0f);
    t.Tap(downX, ey);
    CHECK(t.tool.elevation == -10.0f && t.tool.InTunnel());
    t.Tap(upX, ey), t.Tap(upX, ey), t.Tap(upX, ey);
    CHECK(t.tool.elevation == 5.0f && t.tool.OnBridge());
    t.Tap(zeroX, ey);
    CHECK(t.tool.elevation == 0.0f);
    t.Tap(stepX, ey);
    CHECK(t.tool.stepIdx == 2 && t.tool.Step() == 2.5f);
    t.Tap(downX, ey);
    CHECK(t.tool.elevation == -2.5f && !t.tool.InTunnel());
    // press and hold: about 1 + (1 s - .45 s) / .12 s steps, no extra step on the release
    t.tool.GroundLevel();
    t.Hold(downX, ey, 60);
    std::printf("held 1 s: %g m\n", t.tool.elevation);
    CHECK(t.tool.elevation <= -10.0f && t.tool.elevation >= -15.0f);
    t.Hold(downX, ey, 600); // all the way down: clamped
    CHECK(t.tool.elevation == city::RoadTool::kMinElevation);
    t.Tap(zeroX, ey);
    CHECK(t.tool.elevation == 0.0f);

    // ---- tool modes (row 0)
    t.Tap(t.Slot(4), t.Row(0) + rk::kB * 0.5f);
    CHECK(t.tool.mode == city::ToolMode::Grid);
    t.Frame();
    const float gridH = t.L.panelH;
    t.Tap(t.Slot(1), t.Row(0) + rk::kB * 0.5f);
    CHECK(t.tool.mode == city::ToolMode::SimpleCurve);
    t.Frame();
    CHECK(t.L.panelH < gridH); // the Grid Size row comes and goes with grid mode
    t.Tap(t.Slot(0), t.Row(0) + rk::kB * 0.5f);
    CHECK(t.tool.mode == city::ToolMode::Straight);

    // ---- parallel (row 2) and snapping (row 3)
    t.Tap(t.Slot(0), t.Row(2) + rk::kB * 0.5f);
    CHECK(t.tool.parallel);
    const float off = t.tool.ParallelOffset();
    t.Tap(t.RightSlot(0), t.Row(2) + rk::kB * 0.5f);
    CHECK(t.tool.ParallelOffset() == off + 4.0f);
    t.Tap(t.Slot(0), t.Row(2) + rk::kB * 0.5f);
    CHECK(!t.tool.parallel);
    const bool guide = t.tool.On(city::SnapGuide);
    t.Tap(t.Slot(5), t.Row(3) + rk::kB * 0.5f);
    CHECK(t.tool.On(city::SnapGuide) != guide);
    t.Tap(t.Slot(0), t.Row(3) + rk::kB * 0.5f);
    CHECK(!t.tool.snapAll);
    t.Tap(t.Slot(5), t.Row(3) + rk::kB * 0.5f); // dimmed while all snapping is off: no change
    CHECK(!t.tool.snapAll && (t.tool.snaps & city::SnapGuide) == (guide ? 0u : (uint32_t)city::SnapGuide));
    t.Tap(t.Slot(0), t.Row(3) + rk::kB * 0.5f);
    CHECK(t.tool.snapAll);

    // ---- the palette: the Railways tab, then the same elevation controls lay track underground
    const float tabY = t.L.palY + rk::kPad + rk::kB * 0.5f;
    t.Tap(t.L.palX + rk::kPad + (int)city::Tab::Rail * (rk::kB + rk::kGap) + rk::kB * 0.5f, tabY);
    CHECK(t.tool.tab == city::Tab::Rail && t.tool.IsRail());
    t.tool.stepIdx = 0;
    t.Tap(downX, ey);
    CHECK(t.tool.elevation == -10.0f && t.tool.InTunnel());
    // a second tile in the tab
    const int before = t.tool.asset;
    const float tileY = t.L.palY + rk::kPad + rk::kB + rk::kRowGap + rk::kTileH * 0.5f;
    t.Tap(t.L.palX + rk::kPad + (rk::kTileW + rk::kGap) + rk::kTileW * 0.5f, tileY);
    CHECK(t.tool.asset != before && t.tool.tab == city::Tab::Rail);
    // a drag over the tiles scrolls (when they overflow) and picks nothing
    t.Tap(t.L.palX + rk::kPad + (int)city::Tab::Small * (rk::kB + rk::kGap) + rk::kB * 0.5f, tabY);
    t.Frame();
    if (t.st.scrollMax > 0) {
        const int pick = t.tool.asset;
        const float sx = t.L.palX + 300;
        t.q.Down(sx * t.dpi, tileY * t.dpi);
        t.Frame();
        for (int i = 1; i <= 10; ++i) t.q.Move((sx - 12 * i) * t.dpi, tileY * t.dpi), t.Frame();
        t.q.Up((sx - 120) * t.dpi, tileY * t.dpi);
        t.Frame();
        CHECK(t.st.scroll > 50 && t.tool.asset == pick);
    }

    // ---- prefabs: modes dimmed, a Rotation row
    t.Tap(t.L.palX + rk::kPad + (int)city::Tab::Roundabout * (rk::kB + rk::kGap) + rk::kB * 0.5f, tabY);
    CHECK(t.tool.Current().kind == city::AssetKind::Roundabout);
    t.Frame();
    const float rot = t.tool.rotationDeg;
    t.Tap(t.RightSlot(0), t.Row(4) + rk::kB * 0.5f);
    CHECK(std::fabs(t.tool.rotationDeg - std::fmod(rot - 15.0f + 360.0f, 360.0f)) < 1e-3f);

    // ---- collapsed: the header and the elevation row only, which still work both ways
    t.Tap(t.RightSlot(0), t.L.panelY + rk::kPad + rk::kB * 0.5f);
    CHECK(t.st.collapsed);
    t.Frame();
    CHECK(!t.L.palette && t.L.panelH < 200);
    t.tool.GroundLevel();
    t.tool.stepIdx = 1;
    const float cy = t.Row(0) + rk::kB * 0.5f;
    t.Tap(downX, cy);
    CHECK(t.tool.elevation == -5.0f);
    t.Tap(upX, cy), t.Tap(upX, cy);
    CHECK(t.tool.elevation == 5.0f);

    // ---- a phone: compact, no palette, still both chevrons
    Harness p;
    p.w = 2556, p.h = 1179, p.dpi = 3.0f, p.tablet = false, p.bottom = 76;
    p.tool.Open();
    p.Frame();
    CHECK(!p.L.palette && p.L.panelY >= 8);
    p.Tap(downX, p.Row(0) + rk::kB * 0.5f);
    CHECK(p.tool.elevation == -5.0f);

    // ================================================================ [road ux] mini: two slim strips (the touch default)
    {
        Harness m;
        m.st.mini = true, m.st.dotAvail = true, m.bottom = 60; // right over the slim dock
        m.tool.Open();
        m.Frame();
        const float H = m.h / m.dpi, W = m.w / m.dpi;
        CHECK(m.L.panelH == rk::kMH && m.L.palH == rk::kMH && m.L.palette);
        CHECK(m.L.palY == m.L.panelY && m.L.palX > m.L.panelX + m.L.panelW); // one row: tools left, palette right
        CHECK(m.L.panelY + m.L.panelH <= H - m.bottom + 0.5f && m.L.palX + m.L.palW <= W - 12 + 0.5f);
        CHECK(H - m.L.top <= m.bottom + rk::kMH + 0.5f); // the kit is 52 pt tall, all of it
        std::printf("mini kit: %.0f pt tall (the full panel: %.0f pt)\n", m.L.panelH, rk::PanelHeight(m.tool, false));
        CHECK(rk::PanelHeight(m.tool, false) > 5 * rk::kMH);
        auto slotX = [&](rk::MiniItem id) { // the centre of an item, as Frame lays them out
            float x = m.L.panelX + rk::kMPad;
            for (const rk::MiniSlot& it : rk::MiniItems(m.tool, m.st, true)) {
                if (it.id == id) return x + it.w * 0.5f;
                x += it.w + rk::kMGap;
            }
            return -1000.0f;
        };
        const float sy = m.L.panelY + rk::kMPad + rk::kMB * 0.5f;
        // every target is at least 40 pt
        for (const rk::MiniSlot& it : rk::MiniItems(m.tool, m.st, true)) CHECK(it.w >= 40);
        // elevation both ways, the step on the readout, ground level when off the ground
        m.Tap(slotX(rk::kDown), sy);
        CHECK(m.tool.elevation == -5.0f);
        m.Tap(slotX(rk::kUp), sy), m.Tap(slotX(rk::kUp), sy);
        CHECK(m.tool.elevation == 5.0f && m.tool.OnBridge());
        m.Tap(slotX(rk::kValue), sy);
        CHECK(m.tool.stepIdx == 2);
        m.Frame();
        CHECK(slotX(rk::kZero) > 0);
        m.Tap(slotX(rk::kZero), sy);
        CHECK(m.tool.elevation == 0.0f);
        m.Frame();
        CHECK(slotX(rk::kZero) < 0); // on the ground the 0 m button goes
        // the tool mode popover: one row above the strips
        m.Tap(slotX(rk::kMode), sy);
        CHECK(m.st.pop == 1);
        auto popX = [&](int i, float anchor) {
            const int n = m.st.pop == 1 ? 7 : m.st.pop == 2 ? rk::kSnapCount : (int)city::Tab::Count;
            const float pw = n * (rk::kMB + rk::kMGap) - rk::kMGap + 2 * rk::kMPad;
            const float px = std::clamp(anchor - rk::kMB * 0.5f - rk::kMPad, 12.0f, std::max(12.0f, W - 12 - pw));
            return px + rk::kMPad + i * (rk::kMB + rk::kMGap) + rk::kMB * 0.5f;
        };
        const float popY = m.L.top - 8 - rk::kMH + rk::kMPad + rk::kMB * 0.5f;
        m.Tap(popX(4, slotX(rk::kMode)), popY);
        CHECK(m.tool.mode == city::ToolMode::Grid && m.st.pop == 0);
        m.tool.SetMode(city::ToolMode::Straight);
        // snapping popover: the connection snap is the last toggle
        m.Tap(slotX(rk::kSnap), sy);
        CHECK(m.st.pop == 2);
        CHECK(m.tool.On(city::SnapConnect));
        m.Tap(popX(8, slotX(rk::kSnap)), popY);
        CHECK(!m.tool.On(city::SnapConnect));
        m.Tap(popX(8, slotX(rk::kSnap)), popY);
        CHECK(m.tool.On(city::SnapConnect) && m.st.pop == 2);
        m.Tap(slotX(rk::kSnap), sy);
        CHECK(m.st.pop == 0);
        // the tab popover from the palette strip: Railways
        const float tabX = m.L.palX + rk::kMPad + rk::kMB * 0.5f, tabY = m.L.palY + rk::kMPad + rk::kMB * 0.5f;
        m.Tap(tabX, tabY);
        CHECK(m.st.pop == 3);
        m.Tap(popX((int)city::Tab::Rail, tabX), popY);
        CHECK(m.tool.tab == city::Tab::Rail && m.tool.IsRail() && m.st.pop == 0);
        // a second rail tile in the strip
        const int before = m.tool.asset;
        m.Tap(m.L.palX + rk::kMPad + rk::kMB + rk::kMGap + 2 + (rk::kMTileW + rk::kMGap) + rk::kMTileW * 0.5f, tabY);
        CHECK(m.tool.asset != before && m.tool.tab == city::Tab::Rail);
        // the aim dot toggle, close, expand
        CHECK(m.st.dotOn);
        m.Tap(slotX(rk::kDot), sy);
        CHECK(!m.st.dotOn);
        m.Tap(slotX(rk::kDot), sy);
        CHECK(m.st.dotOn);
        m.q.Down(slotX(rk::kClose) * m.dpi, sy * m.dpi), m.Frame();
        m.q.Up(slotX(rk::kClose) * m.dpi, sy * m.dpi), m.Frame();
        CHECK(m.out.close);
        m.q.Leave(), m.Frame();
        m.Tap(slotX(rk::kExpand), sy);
        CHECK(!m.st.mini);
        m.Frame();
        CHECK(m.L.panelH > 5 * rk::kMH); // the full panel is back
        // the full panel's Mini button goes back down
        const float miniX = m.RightSlot(3) - rk::kB * 0.5f - rk::kGap - 52 * 0.5f;
        m.Tap(miniX, m.L.panelY + rk::kPad + rk::kB * 0.5f);
        CHECK(m.st.mini);
    }
    // ---- [safe area] the strips keep out of a notched phone's insets
    {
        Harness m;
        m.w = 2556, m.h = 1179, m.dpi = 3.0f, m.tablet = false;
        m.st.mini = true, m.bottom = 0;
        m.st.safe = {59, 24, 59, 21, 47};
        m.tool.Open();
        m.Frame();
        const float W = m.w / m.dpi, H = m.h / m.dpi;
        const Rect a = m.st.safe.Area(W, H);
        CHECK(m.L.panelX >= a.x && m.L.panelY + m.L.panelH <= a.y + a.h + 0.01f);
        CHECK(!m.L.palette || (m.L.palX >= a.x && m.L.palX + m.L.palW <= a.x + a.w + 0.01f && m.L.palY >= a.y));
        for (const Rect& r : m.g.ShownPanels()) CHECK(!m.st.safe.Intrudes({r.x / m.dpi, r.y / m.dpi, r.w / m.dpi, r.h / m.dpi}, W, H));
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
