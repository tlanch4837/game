// Headless checks for the city road network and tool (src/city): crossings make junctions, grade-separated roads do
// not, splitting keeps curves continuous, roundabouts clear their middle, the mesh builder emits finite geometry.
// Build and run: tests\road_test.bat
#include <cmath>
#include <cstdio>

#include "city/road_tool.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Alive(const Network& n, int& nodes) {
    int s = 0;
    nodes = 0;
    for (const auto& g : n.segs) s += g.alive;
    for (const auto& x : n.nodes) nodes += x.alive;
    return s;
}
static int Degree(const Network& n, int deg) {
    int c = 0;
    for (const auto& x : n.nodes) c += x.alive && (int)x.segs.size() == deg;
    return c;
}

int main() {
    auto hills = [](float x, float z) { return 6.0f * std::sin(x * 0.02f) * std::cos(z * 0.017f); };
    for (int pass = 0; pass < 2; ++pass) {
        RoadTool t;
        if (pass) t.ground = hills, t.net.ground = hills;
        auto G = [&](float x, float z) { return pass ? hills(x, z) : 0.0f; };
        const int two = ProfileIndex("Two-Lane Road"), four = ProfileIndex("Four-Lane Divided Road"), hwy = ProfileIndex("Two-Lane Two-Way Highway");
        t.BuildRoad(Cubic::Line({-200, 0}, {200, 0}), G(-200, 0), G(200, 0), four);
        t.BuildRoad(Cubic::Line({0, -200}, {0, 200}), G(0, -200), G(0, 200), two);
        int nodes = 0;
        CHECK(Alive(t.net, nodes) == 4);
        CHECK(Degree(t.net, 4) == 1); // the crossing became a four-way junction
        t.BuildRoad(Cubic::Line({-200, -100}, {200, -100}), G(-200, -100) + 15, G(200, -100) + 15, hwy); // a bridge over the two-lane road
        CHECK(Degree(t.net, 4) == 1);
        CHECK(Alive(t.net, nodes) == 5); // no junction under the bridge
        t.BuildRoad(Cubic::Quad({50, 0}, {150, 0}, {150, 100}), G(50, 0), G(150, 100), two); // T onto the avenue
        CHECK(Degree(t.net, 3) == 1);
        const int before = Alive(t.net, nodes);
        t.PushUndo();
        t.BuildAsset(RoadTool::AssetIndex("Medium Roundabout"), {0, G(0, 0), 0}); // on the four-way
        int after = Alive(t.net, nodes);
        CHECK(after > before);
        CHECK(Degree(t.net, 4) == 0); // the junction in the middle is gone
        CHECK(Degree(t.net, 3) >= 5); // four roads meet the ring (+ the T)
        // continuity: every segment's ends sit on its nodes
        for (size_t i = 0; i < t.net.segs.size(); ++i) {
            if (!t.net.segs[i].alive) continue;
            const Cubic c = t.net.Curve((int)i);
            CHECK(Len(c.At(0) - XZ(t.net.nodes[t.net.segs[i].a].p)) < 1e-3f);
            CHECK(Len(c.At(1) - XZ(t.net.nodes[t.net.segs[i].b].p)) < 1e-3f);
        }
        MeshOut out;
        BuildNetwork(t.net, t.ground, out);
        bool finite = true;
        for (const auto& v : out.byMat)
            for (const auto& x : v) finite = finite && std::isfinite(x.px) && std::isfinite(x.py) && std::isfinite(x.pz) && std::isfinite(x.nx);
        CHECK(finite);
        CHECK(out.Triangles() > 1000);
        // undo restores the previous network
        CHECK(t.Undo());
        CHECK(Alive(t.net, nodes) == before);
        std::printf("pass %d: %d segments, %d nodes, %zu triangles\n", pass, after, nodes, out.Triangles());
    }
    // Swap: a two-way road becomes a one-way pointing the way the mouse was dragged
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({0, 0}, {100, 0}), 0, 0, RoadTool::AssetIndex("Two-Lane Road") >= 0 ? Assets()[RoadTool::AssetIndex("Two-Lane Road")].profile : 0);
        t.Select(RoadTool::AssetIndex("Two-Lane One-Way Road"));
        t.SetMode(ToolMode::Swap);
        for (float dir : {-1.0f, 1.0f}) {
            t.Move(true, {50, 0, 0});
            int seg = t.hoverSeg;
            CHECK(seg >= 0);
            t.Click();
            CHECK(t.Dragging());
            t.Move(true, {50 + 20 * dir, 0, 0});
            t.Release();
            CHECK(!t.Dragging());
            for (size_t i = 0; i < t.net.segs.size(); ++i)
                if (t.net.segs[i].alive) {
                    seg = (int)i;
                    CHECK(Profiles()[t.net.segs[i].profile].OneWay());
                }
            const V2 tan = t.net.Curve(seg).Tan(0.5f);
            CHECK(tan.x * dir > 0);
        }
    }
    // the tool's click flow: straight from a click to a click, chained, then cancelled
    {
        RoadTool t;
        t.active = true;
        t.Move(true, {0, 0, 0});
        t.Click();
        t.Move(true, {0, 0, 100});
        CHECK(t.Click());
        CHECK(t.Pending()); // chains from the end
        t.Move(true, {100, 0, 100});
        CHECK(t.Click());
        int nodes;
        CHECK(Alive(t.net, nodes) == 2 && nodes == 3);
        t.Cancel();
        CHECK(!t.Pending());
        // a sharp turn back onto the road is refused
        t.Move(true, {100, 0, 100});
        t.Click();
        t.Move(true, {2, 0, 100.5f});
        std::vector<PreviewCurve> curves;
        CHECK(!t.PendingCurves(curves));
        std::printf("refused: %s\n", t.error.c_str());
        // grid: 3 clicks
        t.Cancel();
        t.SetMode(ToolMode::Grid);
        t.Move(true, {300, 0, 0});
        t.Click();
        t.Move(true, {500, 0, 0});
        t.Click();
        t.Move(true, {500, 0, 200});
        CHECK(t.Click());
        CHECK(Degree(t.net, 4) >= 1);
        std::printf("grid: %d segments\n", Alive(t.net, nodes));
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
