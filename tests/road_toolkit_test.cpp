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

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
