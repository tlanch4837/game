// Headless checks for the road / rail aim dot (src/gui/road_reticle.h) and the connection snaps (src/city/road_tool.h
// SnapConnect) on an iPad-sized viewport:
//   routing    with the dot out a finger on the map is the camera's (never the tool's), the GUI keeps its own;
//   drag       one-finger drags pan the map under the dot and place nothing, a tap on the map places nothing;
//   tick       places the point at the dot (snapped), a second tick builds the piece; the cross takes it back / undoes;
//   snaps      an open end (from further away, aligned: the new road carries the old one's tangent), a node, a
//              segment (T-junction, its middle pulls); rails: an end carries on, a 20 deg switch builds, a 90 deg T is
//              refused, a track never joins a road node; the finger-sized radius follows snapRadius.
// Build and run: tests\road_reticle_test.bat
#include "../src/gui/road_reticle.h"

#ifdef _WINDOWS_
#error "the GUI must not pull in windows.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)

using namespace tbd;
using namespace tbd::gui;
namespace rt = tbd::gui::reticle;

// The game's camera reduced to what the dot needs: a top-down view whose screen centre looks at `cam` on flat ground;
// a one-finger drag moves the ground with the finger (1 px = mpp metres).
struct Harness {
    Context g;
    InputQueue q;
    dock::Atlas atlas;
    city::RoadTool tool;
    rt::State st;
    rt::Out out;
    rt::Layout L;
    SafeInsets safe;
    float w = 2360, h = 1640, dpi = 2.0f; // iPad Air 11"
    float mpp = 0.25f;                    // metres per pixel at the centre
    float camX = 0, camZ = 0;             // the ground point under the dot
    float rightTop = 1640 / 2.0f - 70;    // the kit's strips (points from the top)
    int toolClicks = 0;                   // clicks that reached the road tool through the map (must stay 0)

    Vec3 Dot() const { return {camX, 0, camZ}; }
    void Frame() {
        g.BeginFrame(q.Take(w, h, dpi));
        tool.Move(true, Dot()); // RoadsCursor with the dot out: the screen centre, every frame
        L = rt::Frame(g, atlas, tool, st, safe, rightTop, true, Dot(), out);
        g.EndFrame();
    }
    // What the iOS shell and the host do with one finger on the map (main_ios.mm touchesBegan / Moved / Ended,
    // game_host.cpp TouchBuilds, game_core.inl ViewportInput): route it, then pan or click.
    void Finger(float x0, float y0, float x1, float y1) {
        const bool onGui = g.WantsMouseAt(x0 * dpi, y0 * dpi);
        const rt::Route r = rt::RouteTouch(onGui, true, rt::Wanted(tool, true, true));
        if (r == rt::Route::Gui) { // the GUI's: a press it claims
            q.Down(x0 * dpi, y0 * dpi), Frame();
            q.Move(x1 * dpi, y1 * dpi), Frame();
            q.Up(x1 * dpi, y1 * dpi), Frame();
            q.Leave(), Frame();
            return;
        }
        const bool moved = std::hypot(x1 - x0, y1 - y0) > 10;
        q.Down(x0 * dpi, y0 * dpi, false); // the GUI sees a press it does not own
        Frame();
        for (int i = 1; i <= 8; ++i) {
            const float fx = x0 + (x1 - x0) * i / 8.0f, fy = y0 + (y1 - y0) * i / 8.0f;
            if (r == rt::Route::Camera && moved) { // Drag::Pan: the ground follows the finger
                const float px = x0 + (x1 - x0) * (i - 1) / 8.0f, py = y0 + (y1 - y0) * (i - 1) / 8.0f;
                camX -= (fx - px) * dpi * mpp, camZ -= (fy - py) * dpi * mpp;
            }
            q.Move(fx * dpi, fy * dpi), Frame();
        }
        if (r == rt::Route::Tool) ++toolClicks, tool.Click(); // a build tool's left button
        // Route::Camera and no move: a tap = a left click at the start; with the dot out ViewportInput drops it
        if (r == rt::Route::Camera && !moved && !rt::Wanted(tool, true, true)) ++toolClicks, tool.Click();
        q.Up(x1 * dpi, y1 * dpi), Frame();
        q.Leave(), Frame();
    }
    void TapGui(float x, float y) {
        q.Down(x * dpi, y * dpi), Frame();
        q.Up(x * dpi, y * dpi), Frame();
        q.Leave(), Frame();
    }
    void Tick() { TapGui(L.okX + L.okS * 0.5f, L.okY + L.okS * 0.5f); }
    void Cross() { TapGui(L.backX + L.backS * 0.5f, L.backY + L.backS * 0.5f); }
    void AimAt(float x, float z) { camX = x, camZ = z, Frame(); }
    int AliveSegs() const {
        int n = 0;
        for (const auto& s : tool.net.segs) n += s.alive ? 1 : 0;
        return n;
    }
    int LiveSegsAt(int node) const {
        int n = 0;
        for (int s : tool.net.nodes[node].segs) n += tool.net.segs[s].alive ? 1 : 0;
        return n;
    }
};

static bool Kind(const city::RoadTool& t, const char* k) { return t.haveCursor && t.cursor.kind && std::strcmp(t.cursor.kind, k) == 0; }

int main() {
    // ---- routing
    CHECK(rt::RouteTouch(true, true, true) == rt::Route::Gui);
    CHECK(rt::RouteTouch(false, true, true) == rt::Route::Camera); // the dot: a finger on the map is the camera
    CHECK(rt::RouteTouch(false, true, false) == rt::Route::Tool);  // other tools (buildings, zones, terrain) keep their input
    CHECK(rt::RouteTouch(false, false, false) == rt::Route::Camera);
    {
        city::RoadTool t;
        CHECK(!rt::Wanted(t, true, true)); // no tool out
        t.Open();
        CHECK(rt::Wanted(t, true, true) && !rt::Wanted(t, false, true) && !rt::Wanted(t, true, false)); // the PC / the toggle
        t.SetMode(city::ToolMode::Swap);
        CHECK(!rt::Wanted(t, true, true)); // Swap is a drag on a road
    }

    // ---- drags pan, never place; the tick places at the dot; the cross takes back
    {
        Harness t;
        t.tool.Open();
        t.Frame();
        const float W = t.w / t.dpi, H = t.h / t.dpi;
        CHECK(std::fabs(t.L.cx - W * 0.5f) < 0.01f && std::fabs(t.L.cy - H * 0.5f) < 0.01f);
        CHECK(t.L.okY + t.L.okS <= t.rightTop && t.L.okX + t.L.okS <= W && t.L.backX + t.L.backS <= t.L.okX);
        CHECK(t.L.okS >= 44 && t.L.backS >= 44);
        CHECK(t.g.WantsMouseAt((t.L.okX + 30) * t.dpi, (t.L.okY + 30) * t.dpi)); // a finger on the tick is the GUI's
        CHECK(!t.g.WantsMouseAt(W * 0.5f * t.dpi, H * 0.5f * t.dpi));            // the dot itself is the map's
        for (int i = 0; i < 5; ++i) t.Finger(300 + i * 40, 300, 700, 500 - i * 30); // drags all over
        t.Finger(500, 300, 500, 300);                                               // a tap on the map
        t.Finger(W * 0.5f, H * 0.5f, W * 0.5f + 2, H * 0.5f);                        // a tap right on the dot
        CHECK(t.toolClicks == 0 && t.tool.pts.empty() && t.AliveSegs() == 0);
        CHECK(std::fabs(t.camX) > 100); // ... but the map moved under the dot
        const float ax = t.camX, az = t.camZ;
        CHECK(std::fabs(t.tool.cursor.p.x - ax) < 1e-3f && std::fabs(t.tool.cursor.p.z - az) < 1e-3f); // the cursor is the dot
        t.Tick();
        CHECK(t.out.placed || t.tool.pts.size() == 1);
        CHECK(t.tool.pts.size() == 1 && std::fabs(t.tool.pts[0].p.x - ax) < 8.5f && std::fabs(t.tool.pts[0].p.z - az) < 8.5f);
        t.Finger(680, 400, 560, 400); // pan 120 pt = 60 m east
        CHECK(t.AliveSegs() == 0 && t.tool.pts.size() == 1);
        t.Tick();
        CHECK(t.AliveSegs() == 1);
        const float len = t.tool.net.Curve(0).Length();
        std::printf("dot road: %.1f m\n", len);
        CHECK(len > 50 && len < 70);
        CHECK(t.tool.Pending()); // straight chains on from its end
        t.Cross();
        CHECK(!t.tool.Pending() && t.AliveSegs() == 1); // the cross drops the pending point first
        t.Cross();
        CHECK(t.AliveSegs() == 0); // then undoes the road
        // the tick on water / sky: nothing, and a hint
        t.Tick();
        t.Cross();
        CHECK(!t.tool.Pending());
    }

    // ---- road connection snaps at the dot
    {
        Harness t;
        t.tool.Open();
        t.tool.BuildRoad(city::Cubic::Line({0, 0}, {60, 0}), 0, 0, city::ProfileIndex("Two-Lane Road"));
        CHECK(t.AliveSegs() == 1);
        const int end = t.tool.net.NearestNode({60, 0}, 1.0f);
        CHECK(end >= 0);
        // an open end pulls from further away than a plain node (and further still with a finger's radius)
        t.AimAt(71, 3);
        CHECK(Kind(t.tool, "end") && t.tool.cursor.node == end);
        t.tool.ToggleSnap(city::SnapConnect);
        t.AimAt(71, 3);
        CHECK(!Kind(t.tool, "end") && t.tool.cursor.node != end); // the mouse's own radius
        t.tool.ToggleSnap(city::SnapConnect);
        t.tool.snapRadius = 30; // a finger at this zoom
        t.AimAt(95, 10);
        std::printf("finger snap: %s node %d\n", t.tool.cursor.kind, t.tool.cursor.node);
        CHECK(Kind(t.tool, "end"));
        t.tool.snapRadius = 0;
        CHECK(city::IsConnection("end") && city::IsConnection("road") && !city::IsConnection("grid") && !city::IsConnection(""));
        CHECK(std::strcmp(city::SnapLabel("road"), "T-junction") == 0 && std::strcmp(city::SnapLabel("end"), "Connect end") == 0);
        // carry the end on: tick at the end, aim 25 deg off its line, tick: the new road leaves along the old one's tangent
        t.AimAt(61, 1);
        t.Tick();
        CHECK(t.tool.pts.size() == 1 && t.tool.pts[0].node == end);
        t.AimAt(140, 37);
        t.Tick();
        CHECK(t.AliveSegs() == 2 && t.LiveSegsAt(end) == 2);
        {
            int a = -1, b = -1;
            for (int s : t.tool.net.nodes[end].segs) (a < 0 ? a : b) = s;
            const city::V2 ua = t.tool.net.OutDir(a, end), ub = t.tool.net.OutDir(b, end);
            std::printf("joint at the end: cos %.4f (-1 = straight on)\n", city::Dot2(ua, ub));
            CHECK(city::Dot2(ua, ub) < -0.999f); // tangent-continuous
        }
        t.Cross(), t.Cross(); // back to the one road
        CHECK(t.AliveSegs() == 1);
        // 90 deg off an open end is a corner, not bent
        t.AimAt(61, 1), t.Tick();
        t.AimAt(60, 60), t.Tick();
        CHECK(t.AliveSegs() == 2);
        {
            const int s = t.tool.net.segs[1].alive ? 1 : 0;
            CHECK(t.tool.net.Curve(s).MaxTurnPerMetre() < 1e-3f); // straight
        }
        t.Cross(), t.Cross();
        CHECK(t.AliveSegs() == 1);
        // a T-junction on the segment, and its middle
        t.AimAt(30, 3);
        CHECK(Kind(t.tool, "middle") && std::fabs(t.tool.cursor.t - 0.5f) < 1e-4f);
        t.AimAt(20, 3);
        CHECK(Kind(t.tool, "road") && t.tool.cursor.seg >= 0);
        t.Tick();
        t.AimAt(20, 60);
        t.Tick();
        CHECK(t.AliveSegs() == 3); // the road split at the T
        const int tee = t.tool.net.NearestNode({20, 0}, 1.0f);
        CHECK(tee >= 0 && t.LiveSegsAt(tee) == 3);
    }

    // ---- rails: an end carries on, a switch within 35 deg builds, a 90 deg T does not, never onto a road
    {
        Harness t;
        t.tool.Open();
        int railAsset = -1;
        for (size_t i = 0; i < city::Assets().size(); ++i)
            if (city::Assets()[i].kind == city::AssetKind::Road && city::Profiles()[city::Assets()[i].profile].cat == city::Cat::Rail) { railAsset = (int)i; break; }
        CHECK(railAsset >= 0);
        t.tool.Select(railAsset);
        CHECK(t.tool.IsRail());
        const int prof = t.tool.ProfileId();
        t.tool.BuildRoad(city::Cubic::Line({0, 200}, {200, 200}), 0, 0, prof);
        t.tool.BuildRoad(city::Cubic::Line({0, 400}, {100, 400}), 0, 0, city::ProfileIndex("Two-Lane Road"));
        CHECK(t.AliveSegs() == 2);
        const int end = t.tool.net.NearestNode({200, 200}, 1.0f);
        // carry on from the end, 17 deg off: a curve tangent to the old track
        t.AimAt(203, 201);
        CHECK(Kind(t.tool, "end") && t.tool.cursor.node == end);
        t.Tick();
        t.AimAt(300, 230);
        std::vector<city::PreviewCurve> pc;
        CHECK(t.tool.PendingCurves(pc));
        t.Tick();
        CHECK(t.AliveSegs() == 3 && t.LiveSegsAt(end) == 2);
        {
            int a = -1, b = -1;
            for (int s : t.tool.net.nodes[end].segs) (a < 0 ? a : b) = s;
            CHECK(city::Dot2(t.tool.net.OutDir(a, end), t.tool.net.OutDir(b, end)) < -0.999f);
        }
        t.Cross(), t.Cross();
        CHECK(t.AliveSegs() == 2);
        // a 90 deg T off the track: refused (no switch turns a train that sharply)
        t.AimAt(100, 202);
        CHECK(t.tool.cursor.seg >= 0 && (Kind(t.tool, "switch") || Kind(t.tool, "middle")));
        t.Tick();
        CHECK(t.tool.pts.size() == 1);
        t.AimAt(100, 320);
        CHECK(!t.tool.PendingCurves(pc));
        std::printf("rail T: \"%s\"\n", t.tool.error.c_str());
        CHECK(t.tool.error.find("switch") != std::string::npos);
        t.Tick();
        CHECK(t.AliveSegs() == 2); // nothing built
        t.Cross();
        // a switch: leave the track 20 deg off at x = 60
        t.AimAt(60, 201);
        CHECK(Kind(t.tool, "switch") && t.tool.cursor.seg >= 0);
        t.Tick();
        t.AimAt(60 + 140 * 0.9397f, 200 + 140 * 0.3420f);
        CHECK(t.tool.PendingCurves(pc));
        t.Tick();
        CHECK(t.AliveSegs() == 4); // the track split at the switch, the new one off it
        const int sw = t.tool.net.NearestNode({60, 200}, 1.0f);
        CHECK(sw >= 0 && t.LiveSegsAt(sw) == 3);
        t.Cross();
        // track never snaps to a road's end
        t.AimAt(101, 401);
        CHECK(!(t.tool.cursor.node >= 0 && t.tool.net.HasRoadSeg(t.tool.cursor.node)));
    }

    // ---- [auto snap] the dot snaps on its own: along a building's entrance edge, the whole piece parallel to it
    {
        Harness t;
        t.tool.Open();
        const float half = t.tool.Prof().Half();
        t.tool.lots.push_back({{300, 300}, city::FrontFromDeg(30.0f), 24, 16, 0}); // a rotated building, door to the front
        const city::V2 f = city::FrontFromDeg(30.0f), along{-f.z, f.x}, mid = city::V2{300, 300} + f * 8.0f;
        const city::V2 a = mid + f * (half + 2.0f) - along * 6.0f; // near the line, the snap toggle untouched
        t.AimAt(a.x, a.z);
        CHECK(t.tool.cursor.kind && std::strcmp(t.tool.cursor.kind, "access") == 0);
        t.Tick();
        const city::V2 b = mid + f * (half - 1.5f) + along * 30.0f;
        t.AimAt(b.x, b.z);
        t.Tick();
        int seg = -1;
        for (size_t i = 0; i < t.tool.net.segs.size(); ++i)
            if (t.tool.net.segs[i].alive) seg = (int)i;
        CHECK(seg >= 0);
        if (seg >= 0) {
            const city::Cubic c = t.tool.net.Curve(seg);
            for (float u : {0.0f, 0.5f, 1.0f}) CHECK(std::fabs(city::Dot2(c.At(u) - mid, f) - half) < 0.01f); // the whole road on the line
        }
        // free placement: the toggle off, the same aim is not pulled
        t.tool.snapAll = false;
        t.AimAt(a.x, a.z);
        CHECK(!(t.tool.cursor.kind && t.tool.cursor.kind[0]));
    }

    // ---- the buttons keep out of a large safe area
    {
        Harness t;
        t.safe = {80, 60, 80, 60, 0};
        t.rightTop = t.h / t.dpi; // nothing under them
        t.tool.Open();
        t.Frame();
        const float W = t.w / t.dpi, H = t.h / t.dpi;
        for (const Rect& r : t.g.ShownPanels()) CHECK(!t.safe.Intrudes({r.x / t.dpi, r.y / t.dpi, r.w / t.dpi, r.h / t.dpi}, W, H));
        CHECK(t.L.okX + t.L.okS <= W - 80 && t.L.okY + t.L.okS <= H - 60);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
