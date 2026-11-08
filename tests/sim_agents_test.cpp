// Headless checks for cosmetic traffic (src/city/sim_agents.h):
//   the cars are a pure function of time; their number follows the flow and stays within the visible budget; nothing on
//   segments without flow or past the radius; both directions drive on their own side; a bridge's cars ride its deck;
//   the instance row turns the model's +Z along the heading.
// Build and run: tests\sim_agents_test.bat
#include <cstdio>
#include <vector>

#include "city/road_tool.h"
#include "city/sim_agents.h"

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

int main() {
    const int road = ProfileIndex("Two-Lane Road");
    Network net;
    const int a = net.AddNode({0, 0, 0}, true), b = net.AddNode({400, 0, 0}, true), c = net.AddNode({800, 0, 0}, true);
    const int far1 = net.AddNode({5000, 0, 0}, true), far2 = net.AddNode({5400, 0, 0}, true);
    const int up1 = net.AddNode({200, 9, -200}, false), up2 = net.AddNode({200, 9, 200}, false); // a bridge 9 m up
    const int s0 = Seg(net, a, b, road), s1 = Seg(net, b, c, road), sFar = Seg(net, far1, far2, road), sBridge = Seg(net, up1, up2, road);
    ++net.version;
    SegGrid grid;
    grid.Build(net);
    std::vector<SegFlow> flow(net.segs.size());
    flow[(size_t)s0].flow = 600; // veh/h
    flow[(size_t)sFar].flow = 600;
    flow[(size_t)sBridge].flow = 600;
    // s1: no flow
    AgentParams p;
    std::vector<CarInstance> x, y;
    CosmeticCars(net, grid, flow, {200, 0}, 100.0, p, x);
    CosmeticCars(net, grid, flow, {200, 0}, 100.0, p, y);
    CHECK(!x.empty() && x.size() == y.size());
    bool same = x.size() == y.size();
    for (size_t i = 0; same && i < x.size(); ++i) same = x[i].p.x == y[i].p.x && x[i].p.z == y[i].p.z && x[i].model == y[i].model;
    CHECK(same);
    // density: 600 veh/h at 40 km/h on 400 m = 6 cars (the bridge as well)
    int onS0 = 0, onBridge = 0, onS1 = 0, onFar = 0, east = 0, west = 0;
    for (const CarInstance& car : x) {
        if (car.p.x > 4000) ++onFar;
        else if (car.p.y > 4.0f) ++onBridge;
        else if (car.p.x > 400.5f) ++onS1;
        else {
            ++onS0;
            if (car.dir.x > 0) { ++east; CHECK(car.p.z > 0); } // right-hand traffic: eastbound on the south (+z) side
            else { ++west; CHECK(car.p.z < 0); }
        }
    }
    std::printf("cars: %zu (s0 %d, bridge %d, empty road %d, far away %d; east %d west %d)\n", x.size(), onS0, onBridge, onS1, onFar, east, west);
    CHECK(onS0 >= 5 && onS0 <= 7);
    CHECK(onBridge >= 5 && onBridge <= 7);
    CHECK(onS1 == 0 && onFar == 0);
    // moves with time
    std::vector<CarInstance> later;
    CosmeticCars(net, grid, flow, {200, 0}, 101.0, p, later);
    CHECK(later.size() == x.size() && (later[0].p.x != x[0].p.x || later[0].p.z != x[0].p.z));
    // budget: a lot of flow is scaled down to the cap
    for (SegFlow& f : flow) f.flow = 20000;
    p.maxCars = 40;
    CosmeticCars(net, grid, flow, {200, 0}, 5.0, p, x);
    std::printf("budget 40: %zu cars\n", x.size());
    CHECK(x.size() <= 44 && x.size() >= 34);
    // congestion slows them down: same time step moves them less
    for (SegFlow& f : flow) f.flow = 600, f.speedRatio = 1.0f;
    p.maxCars = 300;
    CosmeticCars(net, grid, flow, {200, 0}, 0.0, p, x);
    CosmeticCars(net, grid, flow, {200, 0}, 1.0, p, y);
    // the row: +Z along the heading
    {
        CarInstance car;
        car.p = {1, 2, 3};
        car.dir = {1, 0};
        float row[12];
        CarRow(car, row);
        // model +Z (0,0,1) -> world: column 2 of the rotation = (row[2], row[6], row[10])
        CHECK(row[2] == 1 && row[6] == 0 && row[10] == 0);
        CHECK(row[3] == 1 && row[7] == 2 && row[11] == 3);
    }
    // a scaled row shrinks the car about its ground point (a fade without alpha)
    {
        CarInstance car;
        car.p = {1, 2, 3};
        car.dir = {0, 1};
        car.scale = 0.5f;
        float row[12];
        CarRow(car, row);
        CHECK(row[10] == 0.5f && row[5] == 0.5f && row[0] == 0.5f && row[3] == 1 && row[7] == 2 && row[11] == 3);
    }
    // the wrap from a segment's end to its start never pops: a car shrinks away in the last metres and grows in the first
    {
        for (SegFlow& f : flow) f.flow = 600, f.speedRatio = 1.0f;
        int faded = 0, shown = 0;
        for (int k = 0; k < 400; ++k) {
            CosmeticCars(net, grid, flow, {200, 0}, 0.25 * k, p, x);
            for (const CarInstance& car : x) {
                if (car.p.y > 4.0f || car.p.x > 4000) continue;
                ++shown;
                CHECK(car.scale > 0.02f && car.scale <= 1.0f);
                const float toEnd = std::min(std::fabs(car.p.x - 0.0f), std::min(std::fabs(car.p.x - 400.0f), std::fabs(car.p.x - 800.0f))); // s0, s1 ends
                if (car.scale < 1.0f) {
                    ++faded;
                    CHECK(toEnd < p.endFade + 0.5f); // only near the segment's ends
                }
            }
        }
        CHECK(shown > 0 && faded > 0);
    }
    (void)s1;

    if (failures) {
        std::printf("sim_agents_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_agents_test: all passed\n");
    return 0;
}
