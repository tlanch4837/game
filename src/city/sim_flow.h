// The city sim's flow traffic (Track S, phase S5): vehicles per hour on every road segment, solved on a graph every few
// game minutes. No individual vehicles. Header only, headless (tests\sim_flow_test.cpp).
//
//   SegGrid    a uniform grid over the road segments (64 m buckets): nearest segment, segments near a point, and which
//              CityCells a segment crosses (road edits dirty those cells). Replaces the linear scans in sim code.
//   FlowGraph  an immutable CSR snapshot of the road network: nodes are vertices, every segment gives an edge per driving
//              direction that has lanes; capacity from lane count and road class, free-flow time from length and speed.
//              Rail, paths and segments without car lanes are left out. Elevated / tunnel segments are ordinary edges.
//   Trips      OD pairs (from node, to node, vehicles per hour, cost profile); the host builds them from the commute links
//              (sim_cells.h, home supercell -> job supercell) and gravity shopping trips (homes -> commercial).
//   Solve      per destination, one reverse Dijkstra on the generalized cost
//                   wT * t + wD * km + wC * junctions,  t = t0 * (1 + 0.15 (v / c)^4)  (BPR)
//              then Dial-style logit loading: at every node the flow splits over the edges that get it closer to the
//              destination, share ~ exp(-theta * extra cost), so routes within a few minutes of the best share the flow
//              and a slower one keeps some. The edge flows move a fraction lambda toward the new loading per iteration
//              (method of successive averages), so the solution settles instead of flipping. A flow past gridlockVC times
//              capacity is held there and the edge is flagged (gridlock relief) instead of diverging.
//   Output     per segment: vehicles per hour (both directions), speed ratio (free = 1), congestion class 0..3.
// Floats are used (not fixed point): the same snapshot and trips give the same result on the same build; nothing the
// player owns (money, people) is derived from flow yet.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

#include "city_cells.h"
#include "road_network.h"

namespace tbd {
namespace city {

// ------------------------------------------------------------------------------------------------ segment grid
class SegGrid {
public:
    static constexpr float kBucket = 64.0f;
    uint64_t builtFrom = ~0ull;

    void Build(const Network& net) {
        buckets_.clear();
        builtFrom = net.version;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive) continue;
            const Cubic c = net.Curve((int)s);
            const float h = net.Prof((int)s).Half() + 1.0f;
            float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
            for (V2 p : {c.p0, c.p1, c.p2, c.p3}) x0 = std::min(x0, p.x), z0 = std::min(z0, p.z), x1 = std::max(x1, p.x), z1 = std::max(z1, p.z);
            for (int bz = Bucket(z0 - h); bz <= Bucket(z1 + h); ++bz)
                for (int bx = Bucket(x0 - h); bx <= Bucket(x1 + h); ++bx) buckets_[Key(bx, bz)].push_back((int)s);
        }
    }
    // Segments whose bounding box (grown by their half width) touches the square of radius r around p (may repeat none).
    template <class F>
    void ForNear(V2 p, float r, F&& f) const {
        std::vector<int> seen;
        for (int bz = Bucket(p.z - r); bz <= Bucket(p.z + r); ++bz)
            for (int bx = Bucket(p.x - r); bx <= Bucket(p.x + r); ++bx) {
                const auto it = buckets_.find(Key(bx, bz));
                if (it == buckets_.end()) continue;
                for (int s : it->second)
                    if (std::find(seen.begin(), seen.end(), s) == seen.end()) seen.push_back(s), f(s);
            }
    }
    // The nearest live segment within maxR of p (-1 none); t and dist as Network::Nearest gives them.
    int Nearest(const Network& net, V2 p, float maxR, float& t, float& dist, bool carsOnly = true) const {
        int best = -1;
        float bd = maxR;
        for (float r = kBucket; ; r = std::min(maxR, r * 2.0f)) {
            ForNear(p, r, [&](int s) {
                if (carsOnly) {
                    const Profile& pr = net.Prof(s);
                    if (pr.cat == Cat::Rail || pr.cat == Cat::Path || pr.fwd + pr.back == 0) return;
                }
                float ts, ds;
                net.Nearest(s, p, ts, ds);
                if (ds < bd) bd = ds, best = s, t = ts;
            });
            if (best >= 0 || r >= maxR) break;
        }
        dist = bd;
        return best;
    }
    // The CityCells a segment's curve passes through (road edits dirty them).
    static void CellsOf(const Network& net, int s, std::vector<CellKey>& out) {
        out.clear();
        const Cubic c = net.Curve(s);
        const int n = std::max(2, (int)std::ceil(c.Length(8) / (kCityCellSize * 0.25f)));
        for (int i = 0; i <= n; ++i) {
            const V2 p = c.At((float)i / n);
            const CellKey k = CellKeyAt(p.x, p.z);
            if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        }
    }

private:
    std::unordered_map<uint64_t, std::vector<int>> buckets_;
    static int Bucket(float v) { return (int)std::floor(v / kBucket); }
    static uint64_t Key(int x, int z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; }
};

// ------------------------------------------------------------------------------------------------ graph snapshot
struct FlowEdge {
    int from = 0, to = 0, seg = -1;
    bool forward = true; // along the segment a -> b
    float len = 0;       // metres
    float t0 = 0;        // free-flow minutes
    float cap = 0;       // vehicles per hour
};

struct FlowGraph {
    uint64_t version = ~0ull; // Network::version it was built from
    int nodes = 0;
    std::vector<FlowEdge> edges;   // grouped by `from`
    std::vector<int> outOff;       // nodes + 1: edges[outOff[n] .. outOff[n+1]) leave n
    std::vector<int> inOff, inIdx; // edges entering n: inIdx[inOff[n] .. inOff[n+1])
    std::vector<uint8_t> junction; // per node: more than two road segments meet (a turn costs wC)

    // km/h and vehicles per hour per lane by road class
    static float Speed(Cat c) { return c == Cat::Highway ? 90.0f : c == Cat::Large ? 60.0f : c == Cat::Medium ? 50.0f : 40.0f; }
    static float LaneCap(Cat c) { return c == Cat::Highway ? 1800.0f : c == Cat::Large ? 1200.0f : c == Cat::Medium ? 1000.0f : 800.0f; }

    static FlowGraph Build(const Network& net) {
        FlowGraph g;
        g.version = net.version;
        g.nodes = (int)net.nodes.size();
        std::vector<int> deg((size_t)g.nodes, 0);
        for (size_t s = 0; s < net.segs.size(); ++s) {
            const Segment& sg = net.segs[s];
            if (!sg.alive) continue;
            const Profile& p = net.Prof((int)s);
            if (p.cat == Cat::Rail || p.cat == Cat::Path || p.fwd + p.back == 0) continue;
            ++deg[(size_t)sg.a], ++deg[(size_t)sg.b];
            const float len = std::max(1.0f, net.Curve((int)s).Length(8));
            const float t0 = len / (Speed(p.cat) * 1000.0f / 60.0f);
            if (p.fwd > 0) g.edges.push_back({sg.a, sg.b, (int)s, true, len, t0, p.fwd * LaneCap(p.cat)});
            if (p.back > 0) g.edges.push_back({sg.b, sg.a, (int)s, false, len, t0, p.back * LaneCap(p.cat)});
        }
        std::stable_sort(g.edges.begin(), g.edges.end(), [](const FlowEdge& a, const FlowEdge& b) { return a.from < b.from; });
        g.outOff.assign((size_t)g.nodes + 1, 0);
        for (const FlowEdge& e : g.edges) ++g.outOff[(size_t)e.from + 1];
        for (int n = 0; n < g.nodes; ++n) g.outOff[(size_t)n + 1] += g.outOff[(size_t)n];
        g.inOff.assign((size_t)g.nodes + 1, 0);
        for (const FlowEdge& e : g.edges) ++g.inOff[(size_t)e.to + 1];
        for (int n = 0; n < g.nodes; ++n) g.inOff[(size_t)n + 1] += g.inOff[(size_t)n];
        g.inIdx.assign(g.edges.size(), 0);
        std::vector<int> fill(g.inOff.begin(), g.inOff.end() - 1);
        for (size_t e = 0; e < g.edges.size(); ++e) g.inIdx[(size_t)fill[(size_t)g.edges[e].to]++] = (int)e;
        g.junction.assign((size_t)g.nodes, 0);
        for (int n = 0; n < g.nodes; ++n) g.junction[(size_t)n] = deg[(size_t)n] > 2;
        return g;
    }
};

// ------------------------------------------------------------------------------------------------ trips and solver
// How a kind of trip weighs time, distance and turns (minutes-equivalent). The brief's per-age profiles: adults weigh
// time, teens money (tolls; none exist yet, so distance stands in), seniors comfort (turns).
struct CostProfile {
    float wT = 1.0f;   // per minute
    float wD = 0.0f;   // per km
    float wC = 0.15f;  // per junction passed
    float wM = 0.0f;   // per unit of toll (no tolls yet)
};
inline const CostProfile* CostProfiles() {
    static const CostProfile k[3] = {{1.0f, 0.2f, 0.15f, 0.0f},  // adults: time
                                     {0.6f, 1.2f, 0.10f, 2.0f},  // teens: money (distance, tolls)
                                     {0.9f, 0.2f, 0.60f, 0.0f}}; // seniors: comfort (fewer turns)
    return k;
}

struct Trip {
    int from = 0, to = 0; // graph nodes
    float flow = 0;       // vehicles per hour
    int profile = 0;      // CostProfiles() index
};

struct SegFlow {
    float flow = 0;       // vehicles per hour, both directions
    float speedRatio = 1; // congested speed / free speed, the slower direction
    uint8_t cls = 0;      // 0 free, 1 busy (v/c > 0.6), 2 congested (> 0.9), 3 jammed (> 1.2)
    bool gridlock = false;
};

struct FlowParams {
    int iterations = 6;
    float lambda = 0.15f;     // MSA step: the share of the new loading taken per iteration
    float theta = 1.2f;       // logit sensitivity (1 / generalized minutes)
    float gridlockVC = 2.5f;  // flow held at this times capacity, edge flagged
};

class FlowSolver {
public:
    std::vector<float> flow; // per edge of the graph it was last solved on, vehicles per hour
    std::vector<uint8_t> held; // per edge: hit the gridlock cap
    uint64_t graphVersion = ~0ull;
    double lastMs = 0;
    int lastDestinations = 0;

    // Full solve: `iterations` MSA rounds from the current flows (zero if the graph changed).
    void Solve(const FlowGraph& g, const std::vector<Trip>& trips, const FlowParams& p = {}) {
        const auto t0 = std::chrono::steady_clock::now();
        if (graphVersion != g.version || flow.size() != g.edges.size()) {
            flow.assign(g.edges.size(), 0.0f);
            held.assign(g.edges.size(), 0);
            graphVersion = g.version;
        }
        // group by destination and profile, in a fixed order
        std::vector<Trip> sorted = trips;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Trip& a, const Trip& b) { return a.to != b.to ? a.to < b.to : a.profile < b.profile; });
        std::vector<float> aux(g.edges.size());
        lastDestinations = 0;
        for (int it = 0; it < std::max(1, p.iterations); ++it) {
            std::fill(aux.begin(), aux.end(), 0.0f);
            for (size_t i = 0; i < sorted.size();) {
                size_t j = i;
                while (j < sorted.size() && sorted[j].to == sorted[i].to && sorted[j].profile == sorted[i].profile) ++j;
                LoadDestination(g, sorted, i, j, p, aux);
                if (it == 0) ++lastDestinations;
                i = j;
            }
            const float lam = it == 0 && Empty() ? 1.0f : p.lambda;
            for (size_t e = 0; e < flow.size(); ++e) {
                float v = flow[e] + lam * (aux[e] - flow[e]);
                const float capV = p.gridlockVC * g.edges[e].cap;
                held[e] = v > capV;
                flow[e] = std::min(v, capV);
            }
        }
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // Per segment results for segments of the network the graph came from.
    void Segments(const FlowGraph& g, size_t segCount, std::vector<SegFlow>& out) const {
        out.assign(segCount, SegFlow{});
        if (flow.size() != g.edges.size()) return;
        for (size_t e = 0; e < g.edges.size(); ++e) {
            const FlowEdge& E = g.edges[e];
            if (E.seg < 0 || (size_t)E.seg >= segCount) continue;
            SegFlow& s = out[(size_t)E.seg];
            s.flow += flow[e];
            const float vc = E.cap > 0 ? flow[e] / E.cap : 0.0f;
            s.speedRatio = std::min(s.speedRatio, 1.0f / (1.0f + 0.15f * vc * vc * vc * vc));
            s.cls = std::max<uint8_t>(s.cls, vc > 1.2f ? 3 : vc > 0.9f ? 2 : vc > 0.6f ? 1 : 0);
            s.gridlock |= held[e] != 0;
        }
    }

    // Generalized cost of edge e at the current flows for profile p.
    static float EdgeCost(const FlowGraph& g, size_t e, float v, const CostProfile& pr) {
        const FlowEdge& E = g.edges[e];
        const float vc = E.cap > 0 ? v / E.cap : 0.0f;
        const float t = E.t0 * (1.0f + 0.15f * vc * vc * vc * vc);
        return pr.wT * t + pr.wD * E.len * 0.001f + (g.junction[(size_t)E.to] ? pr.wC : 0.0f);
    }

private:
    bool Empty() const {
        for (float v : flow)
            if (v != 0) return false;
        return true;
    }

    // One destination: reverse Dijkstra, then Dial loading of every trip to it.
    void LoadDestination(const FlowGraph& g, const std::vector<Trip>& trips, size_t i0, size_t i1, const FlowParams& p, std::vector<float>& aux) {
        const int dest = trips[i0].to;
        if (dest < 0 || dest >= g.nodes) return;
        const CostProfile& pr = CostProfiles()[std::clamp(trips[i0].profile, 0, 2)];
        cost_.resize(g.edges.size());
        for (size_t e = 0; e < g.edges.size(); ++e) cost_[e] = EdgeCost(g, e, flow[e], pr);
        dist_.assign((size_t)g.nodes, INFINITY);
        dist_[(size_t)dest] = 0;
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        q.push({0.0f, dest});
        order_.clear();
        while (!q.empty()) {
            const auto [d, n] = q.top();
            q.pop();
            if (d > dist_[(size_t)n]) continue;
            order_.push_back(n);
            for (int k = g.inOff[(size_t)n]; k < g.inOff[(size_t)n + 1]; ++k) {
                const int e = g.inIdx[(size_t)k];
                const int from = g.edges[(size_t)e].from;
                const float nd = d + cost_[(size_t)e];
                if (nd < dist_[(size_t)from]) dist_[(size_t)from] = nd, q.push({nd, from});
            }
        }
        // the trips' flow onto their origins, then down the DAG of efficient edges, farthest node first
        load_.assign((size_t)g.nodes, 0.0f);
        bool any = false;
        for (size_t i = i0; i < i1; ++i)
            if (trips[i].from >= 0 && trips[i].from < g.nodes && trips[i].from != dest && std::isfinite(dist_[(size_t)trips[i].from]))
                load_[(size_t)trips[i].from] += trips[i].flow, any = true;
        if (!any) return;
        for (size_t k = order_.size(); k-- > 0;) {
            const int n = order_[k];
            const float f = load_[(size_t)n];
            if (f <= 0 || n == dest) continue;
            float wsum = 0;
            const float dn = dist_[(size_t)n];
            for (int e = g.outOff[(size_t)n]; e < g.outOff[(size_t)n + 1]; ++e) {
                const int to = g.edges[(size_t)e].to;
                if (!(dist_[(size_t)to] < dn)) continue; // only edges that get closer: the loading graph is acyclic
                wsum += std::exp(-p.theta * (cost_[(size_t)e] + dist_[(size_t)to] - dn));
            }
            if (wsum <= 0) continue;
            for (int e = g.outOff[(size_t)n]; e < g.outOff[(size_t)n + 1]; ++e) {
                const int to = g.edges[(size_t)e].to;
                if (!(dist_[(size_t)to] < dn)) continue;
                const float share = f * std::exp(-p.theta * (cost_[(size_t)e] + dist_[(size_t)to] - dn)) / wsum;
                aux[(size_t)e] += share;
                load_[(size_t)to] += share;
            }
        }
    }

    std::vector<float> cost_, dist_, load_;
    std::vector<int> order_;
};

} // namespace city
} // namespace tbd
