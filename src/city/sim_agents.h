// Cosmetic traffic (Track S, phase S7): the cars the player sees are not simulated. Each road segment near the camera
// shows as many cars as its solved flow (sim_flow.h) puts on it, scaled down to a visible budget; where a car is is a pure
// function of (segment, lane, car index, time), evaluated on the CPU (a few hundred instances). Cars never decide
// anything; they only slow down with the segment's congestion. Header only, headless (tests\sim_agents_test.cpp).
// This is the fallback (TBD_HERDS=0, or before the herds' link table is built): sim_children.h draws the real cars. A car
// wraps from its segment's end to its start, so it shrinks away over the last endFade metres and grows over the first.
//
// Density on a segment: vehicles per km = flow (veh/h) / speed (km/h), speed = free speed x the segment's speed ratio.
// Lanes: right-hand traffic, the first `back` lanes (left of a -> b) drive b -> a, the rest a -> b, 3.2 m apart.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "road_network.h"
#include "sim.h"
#include "sim_flow.h"

namespace tbd {
namespace city {

struct CarInstance {
    Vec3 p;            // ground point under the car's centre
    V2 dir;            // heading (unit, xz)
    int model = 0;     // 0 .. models-1
    float scale = 1;   // 0 .. 1: a car fading in or out (a driveway, the map edge, the edge of the view) is drawn smaller
    uint32_t id = 0;   // who it is across frames (0: no identity, e.g. parked or cosmetic cars)
};

struct AgentParams {
    int maxCars = 300;        // visible budget (the governor scales it, R7)
    float radius = 450.0f;    // metres around the focus: nothing past it
    float laneWidth = 3.2f;
    int models = 8;
    float endFade = 10.0f;    // metres at each segment end over which a car fades (the wrap point)
};

// Cars for this moment. `focus` is the camera's ground point; timeSec the time they drive on (game seconds, so the speed
// slider makes them faster). Deterministic: the same inputs give the same cars.
inline void CosmeticCars(const Network& net, const SegGrid& grid, const std::vector<SegFlow>& flow, V2 focus, double timeSec, const AgentParams& p,
                         std::vector<CarInstance>& out) {
    out.clear();
    struct Cand {
        int seg;
        float len, want; // metres, cars wanted at full density
    };
    std::vector<Cand> cand;
    double total = 0;
    grid.ForNear(focus, p.radius, [&](int s) {
        if (s < 0 || (size_t)s >= flow.size() || (size_t)s >= net.segs.size() || !net.segs[s].alive) return;
        const SegFlow& f = flow[(size_t)s];
        const Profile& pr = net.Prof(s);
        if (f.flow <= 0.5f || pr.cat == Cat::Rail || pr.cat == Cat::Path || pr.fwd + pr.back == 0) return;
        const float len = net.Curve(s).Length(8);
        const float kmh = FlowGraph::Speed(pr.cat) * std::max(0.1f, f.speedRatio);
        const float perKm = f.flow / kmh;
        cand.push_back({s, len, perKm * len * 0.001f});
        total += perKm * len * 0.001;
    });
    if (cand.empty()) return;
    const double scale = total > p.maxCars ? p.maxCars / total : 1.0;
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.seg < b.seg; });
    for (const Cand& c : cand) {
        const Profile& pr = net.Prof(c.seg);
        const SegFlow& f = flow[(size_t)c.seg];
        // a fractional count becomes a whole one by the segment's own hash, so the total stays near the budget
        const double want = c.want * scale;
        int n = (int)want;
        if (SplitMix64(SimSeed(0xCA25, (uint64_t)c.seg)).Below(1000) < (uint32_t)((want - n) * 1000.0)) ++n;
        if (n <= 0) continue;
        const Cubic curve = net.Curve(c.seg);
        const int lanes = pr.fwd + pr.back;
        const float W = lanes * p.laneWidth;
        const float mps = FlowGraph::Speed(pr.cat) / 3.6f * std::max(0.1f, f.speedRatio);
        for (int i = 0; i < n; ++i) {
            SplitMix64 r(SimSeed(0xCA25, (uint64_t)c.seg, (uint64_t)i + 1));
            const int lane = (int)r.Below((uint32_t)lanes);
            const bool forward = lane >= pr.back;
            const float phase = (float)(r.Below(100000) / 100000.0);
            const float jitter = 0.85f + 0.3f * (float)(r.Below(1000) / 1000.0); // drivers differ a little
            double s = std::fmod(phase + timeSec * mps * jitter / std::max(1.0f, c.len), 1.0);
            const float t = (float)(forward ? s : 1.0 - s);
            const V2 at = curve.At(t);
            const V2 tan = curve.Tan(t);
            const V2 right = RightOf(tan);
            const float off = (lane + 0.5f) * p.laneWidth - W * 0.5f; // + : right of a -> b
            const V2 q = at + right * off;
            CarInstance car;
            car.p = {q.x, net.YAt(net.segs[(size_t)c.seg], t), q.z};
            car.dir = forward ? tan : tan * -1.0f;
            car.model = (int)r.Below((uint32_t)std::max(1, p.models));
            // the car wraps from the segment's end to its start: it shrinks away in the last metres and grows in the first,
            // so the wrap never shows as a car popping at the junction
            const float fromEnd = (float)std::min(s, 1.0 - s) * c.len;
            car.scale = std::clamp(fromEnd / p.endFade, 0.0f, 1.0f);
            if (car.scale <= 0.02f) continue;
            out.push_back(car);
        }
    }
}

// A car's instance row (kInstanceFloats = 12: float3x4 row by row, translation last in each row): yaw so the model's +Z
// points along `dir`, scaled by c.scale (a fade without alpha: the car shrinks into its ground point).
inline void CarRow(const CarInstance& c, float* row) {
    const float g = std::clamp(c.scale, 0.0f, 1.0f);
    const float s = c.dir.x * g, k = c.dir.z * g; // sin, cos of the yaw
    const float m[12] = {k, 0, s, c.p.x, 0, g, 0, c.p.y, -s, 0, k, c.p.z};
    for (int i = 0; i < 12; ++i) row[i] = m[i];
}

} // namespace city
} // namespace tbd
