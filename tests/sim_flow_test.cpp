// Headless checks for the city sim's flow traffic (src/city/sim_flow.h) on synthetic networks:
//   opening a faster highway shifts flow onto it; a slightly longer route keeps a share; congestion pushes flow back to
//   the old road; a road edit (new graph version) re-solves from scratch; same inputs give the same flows; the segment
//   grid finds the nearest segment and the cells a segment crosses; a ~10k segment grid is timed per destination.
// Build and run: tests\sim_flow_test.bat
#include <chrono>
#include <cstdio>
#include <vector>

#include "city/road_tool.h"
#include "city/sim.h"
#include "city/sim_flow.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static int Seg(Network& net, int a, int b, int prof) {
    const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
    return net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
}
static int AddN(Network& net, float x, float z) { return net.AddNode({x, 0, z}, true); }

// Share of the A -> B flow on the segments of `segs`.
static float FlowOn(const FlowGraph& g, const FlowSolver& s, const std::vector<int>& segs) {
    std::vector<SegFlow> out;
    s.Segments(g, 1000, out);
    float f = 0;
    for (int x : segs) f = std::max(f, out[(size_t)x].flow);
    return f;
}

int main() {
    const int road = ProfileIndex("Two-Lane Road"), hwy = ProfileIndex("Two-Lane Two-Way Highway");
    CHECK(road >= 0 && hwy >= 0);
    // A (0,0) to B (3000,0) along a small road; later a highway on a 300 m detour north.
    Network net;
    const int A = AddN(net, 0, 0), M = AddN(net, 1500, 0), B = AddN(net, 3000, 0);
    const int r1 = Seg(net, A, M, road), r2 = Seg(net, M, B, road);
    ++net.version;
    std::vector<Trip> trips = {{A, B, 300.0f, 0}};
    FlowSolver s;
    FlowGraph g = FlowGraph::Build(net);
    s.Solve(g, trips);
    const float roadOnly = FlowOn(g, s, {r1, r2});
    std::printf("road only: %.0f veh/h on the road\n", roadOnly);
    CHECK(roadOnly > 290 && roadOnly < 310);

    // Open the highway: faster, a bit longer.
    const int H1 = AddN(net, 300, -300), H2 = AddN(net, 2700, -300);
    const int h0 = Seg(net, A, H1, hwy), h1 = Seg(net, H1, H2, hwy), h2 = Seg(net, H2, B, hwy);
    ++net.version;
    FlowGraph g2 = FlowGraph::Build(net);
    s.Solve(g2, trips); // new graph version: starts from zero
    CHECK(s.graphVersion == net.version);
    const float onHwy = FlowOn(g2, s, {h1}), onRoad = FlowOn(g2, s, {r1});
    std::printf("highway open: %.0f on the highway, %.0f on the road\n", onHwy, onRoad);
    CHECK(onHwy > onRoad * 2.0f); // most of it moves to the faster road
    CHECK(onRoad > 1.0f);         // the old road keeps a share (logit)
    (void)h0, (void)h2;

    // Congestion: ten times the traffic overloads the highway's 2 x 1800 / h; flow comes back to the road.
    {
        FlowSolver heavy;
        std::vector<Trip> many = {{A, B, 9000.0f, 0}};
        FlowParams p;
        p.iterations = 30;
        heavy.Solve(g2, many, p);
        const float hw = FlowOn(g2, heavy, {h1}), rd = FlowOn(g2, heavy, {r1});
        std::printf("heavy: %.0f on the highway, %.0f on the road (road share %.2f vs %.2f light)\n", hw, rd, rd / (hw + rd), onRoad / (onHwy + onRoad));
        CHECK(rd / (hw + rd) > onRoad / (onHwy + onRoad) + 0.05f);
    }

    // Two near-equal routes: a 4% longer road keeps a real share.
    {
        Network n2;
        const int a = AddN(n2, 0, 0), b = AddN(n2, 2000, 0), c = AddN(n2, 1000, 400);
        const int direct = Seg(n2, a, b, road), up = Seg(n2, a, c, road), down = Seg(n2, c, b, road);
        ++n2.version;
        FlowGraph g3 = FlowGraph::Build(n2);
        FlowSolver s3;
        s3.Solve(g3, {{a, b, 200.0f, 0}});
        const float d = FlowOn(g3, s3, {direct}), u = FlowOn(g3, s3, {up});
        std::printf("near-equal: direct %.0f, longer %.0f (%.0f%% longer)\n", d, u, (2.0f * std::sqrt(1000.0f * 1000.0f + 400.0f * 400.0f) / 2000.0f - 1.0f) * 100.0f);
        CHECK(u > 0.05f * (d + u));
        CHECK(d > u);
        (void)down;
    }

    // Determinism.
    {
        FlowSolver x, y;
        x.Solve(g2, trips);
        y.Solve(g2, trips);
        CHECK(x.flow == y.flow);
    }

    // Segment grid: nearest and the cells a segment crosses.
    {
        SegGrid grid;
        grid.Build(net);
        float t, d;
        CHECK(grid.Nearest(net, {800, 5}, 200.0f, t, d) == r1);
        CHECK(grid.Nearest(net, {1500, -290}, 200.0f, t, d) == h1);
        CHECK(grid.Nearest(net, {1500, 5000}, 200.0f, t, d) == -1);
        std::vector<CellKey> cells;
        SegGrid::CellsOf(net, r1, cells);
        CHECK(cells.size() >= 12 && cells.size() <= 14); // 1500 m along x: cells 0..11 (and the end on 11/12)
    }

    // Scale: a 70 x 70 grid of 120 m blocks (~9.9k segments), arterials every 10th line; 60 destinations x 200 origins.
    {
        Network big;
        const int N = 70;
        const int med = ProfileIndex("Four-Lane Road") >= 0 ? ProfileIndex("Four-Lane Road") : road;
        std::vector<int> id((size_t)(N + 1) * (N + 1));
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) id[(size_t)j * (N + 1) + i] = AddN(big, i * 120.0f, j * 120.0f);
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i < N; ++i) {
                Seg(big, id[(size_t)j * (N + 1) + i], id[(size_t)j * (N + 1) + i + 1], j % 10 == 0 ? med : road);
                Seg(big, id[(size_t)i * (N + 1) + j], id[(size_t)(i + 1) * (N + 1) + j], j % 10 == 0 ? med : road);
            }
        ++big.version;
        const auto t0 = std::chrono::steady_clock::now();
        FlowGraph gb = FlowGraph::Build(big);
        const auto t1 = std::chrono::steady_clock::now();
        SplitMix64 r(3);
        std::vector<Trip> tr;
        for (int d = 0; d < 60; ++d) {
            const int to = (int)r.Below((uint32_t)gb.nodes);
            for (int o = 0; o < 200; ++o) tr.push_back({(int)r.Below((uint32_t)gb.nodes), to, 5.0f, d % 3});
        }
        FlowSolver sb;
        FlowParams p;
        p.iterations = 1;
        sb.Solve(gb, tr, p);
        const double one = sb.lastMs;
        p.iterations = 6;
        FlowSolver sb6;
        sb6.Solve(gb, tr, p);
        const double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("scale: %zu segments, %d nodes, %zu edges; graph build %.1f ms; one iteration %.1f ms (%.2f ms per destination); 6 iterations %.1f ms\n",
                    big.segs.size(), gb.nodes, gb.edges.size(), buildMs, one, one / 60.0, sb6.lastMs);
        std::printf("scale: sliced at one destination per frame that is %.2f ms per frame; the host solves on a worker thread instead\n", one / 60.0);
        CHECK(one / 60.0 < 4.0);
    }

    if (failures) {
        std::printf("sim_flow_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_flow_test: all passed\n");
    return 0;
}
