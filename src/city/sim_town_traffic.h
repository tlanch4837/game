// Town traffic: the trips the hood herds alone never made, and the cars that stand still. Header only, headless
// (tests\sim_traffic_test.cpp). The host (city_herds.inl) rebuilds it with the hoods every 10 game minutes.
//
//   Local trips    commutes and shopping that start and end inside one hood. A player's first town is one hood (6 to 20
//                  cells of 128 m), and the hood herds only drive between two different hoods, so it had no traffic at all.
//                  Every occupied cell is tied to its nearest car-road node; the hood's own commute splits over (home
//                  node, job node) pairs by working residents x filled jobs (the three biggest job nodes per home node).
//   Outside conns  a car road that dead-ends at the map edge, or one the player flagged with an outside connection, leads to
//                  the world beyond. Through traffic crosses the map between two of them (never zero on a highway, more as
//                  the city grows); commuters come in to fill vacancies; jobless residents drive out to work; visitors come
//                  to the shops; trucks serve industry and shops. All as NodeLinks for the herds (sim_herds.h).
//   Flow trips     the same links as peak-hour OD pairs for sim_flow.h (the overlay and the TBD_HERDS=0 cosmetic cars).
//   Parked cars    a few cars at the curb of every occupied cell: at home overnight, at work in the day (the commute curves
//                  say how many are away). Positions are fixed per road / cell change; per frame only a copy near the camera.
// Deterministic: the same roads, cells and counts give the same links and the same parked cars.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "road_network.h"
#include "sim.h"
#include "sim_agents.h"
#include "sim_cells.h"
#include "sim_flow.h"
#include "sim_herds.h"
#include "sim_hoods.h"
#include "sim_routes.h"

namespace tbd {
namespace city {

struct OutsideConn {
    int node = -1;
    V2 p{0, 0};
    Cat cat = Cat::Small;
    float weight = 1;    // highway 3, large 2, medium 1.5, small 1: how much of the outside world this road carries
    bool flagged = false; // a player-placed outside connection (transit.h) rather than a road off the map edge
};

struct TownTrafficParams {
    float edgeMargin = 100.0f;     // metres from the map edge a dead end counts as leaving the map
    float flagRadius = 40.0f;      // a flagged outside connection takes the road node within this
    float minThroughMetres = 1500; // two connections closer than this exchange no through traffic
    float throughPerPair = 1200;   // vehicles a day between two highway connections, one direction, empty map
    float throughPopScale = 4000;  // through traffic x (1 + population / this) ...
    float throughGrowthMax = 3;    // ... up to this
    float inFill = 0.5f;           // of the vacancies, filled by commuters from outside
    float workOutside = 0.4f;      // of the jobless adults, working outside the map
    float visitorsPerShopJob = 0.6f; // visitor cars a day per commercial job
    float trucksPerIndJob = 0.3f, trucksPerShopJob = 0.08f; // trucks a day, each way
    float shopPerResident = 0.25f; // shopping car trips a day per resident (BuildShopLinks' number)
    float shopDecayMetres = 1500;  // gravity, as the hood shop links
    int topNodes = 6;              // destinations per outside connection
    int topLocal = 3;              // job nodes per home node inside a hood
    // parked cars
    float homeCarsPerAdult = 0.3f, workCarsPerJob = 0.2f;
    int maxPerCell = 14;           // of each kind; spots per cell = 2 x this
    float spotSpacing = 7.0f, laneWidth = 3.2f;
    int models = 8;
    uint64_t seed = 0x5EED;
};

struct TownTrafficStats {
    int conns = 0;
    size_t links = 0;
    float throughPerDay = 0, inPerDay = 0, outPerDay = 0, localPerDay = 0;
    int parkedShown = 0;
};

// The outside connections of a network: dead-end car roads at the map edge (|x| or |z| past mapHalf - margin) and the nodes
// of flagged connections. Only nodes the link table knows (they have a car link) count. Ascending node order.
inline void FindOutsideConnections(const Network& net, const LinkTable& t, const std::vector<V2>& flagged, float mapHalf, const TownTrafficParams& p,
                                   std::vector<OutsideConn>& out) {
    out.clear();
    for (size_t n = 0; n < net.nodes.size(); ++n) {
        const Node& nd = net.nodes[n];
        if (!nd.alive || (int)n >= t.nodes || t.outOff.size() <= n + 1 || t.inOff.size() <= n + 1) continue;
        int car = 0, seg = -1;
        bool rail = false;
        for (int s : nd.segs) {
            if (!net.segs[(size_t)s].alive) continue;
            const Profile& pr = net.Prof(s);
            if (pr.cat == Cat::Rail) { rail = true; continue; }
            if (pr.cat == Cat::Path || pr.fwd + pr.back == 0) continue;
            ++car, seg = s;
        }
        if (rail || car == 0) continue;
        const V2 q = XZ(nd.p);
        bool flag = false;
        for (V2 f : flagged) flag |= Len(f - q) <= p.flagRadius;
        const bool edge = car == 1 && std::max(std::fabs(q.x), std::fabs(q.z)) >= mapHalf - p.edgeMargin;
        if (!edge && !flag) continue;
        if (t.outOff[n + 1] == t.outOff[n] && t.inOff[n + 1] == t.inOff[n]) continue; // no car link here
        OutsideConn c;
        c.node = (int)n, c.p = q, c.cat = net.Prof(seg).cat, c.flagged = flag;
        c.weight = c.cat == Cat::Highway ? 3.0f : c.cat == Cat::Large ? 2.0f : c.cat == Cat::Medium ? 1.5f : 1.0f;
        out.push_back(c);
    }
}

class TownTraffic {
public:
    TownTrafficParams params;
    TownTrafficStats stats;
    std::vector<OutsideConn> conns;
    std::vector<NodeLink> links; // for HerdSystem::ReleaseHerds
    uint64_t builtNet = ~0ull;   // network version the links' nodes belong to

    // Road edit: which link runs each segment's way (parked cars ride on links). The table must come from `g`.
    void SetTable(const Network& net, const FlowGraph& g, const LinkTable& t, const LinkBuild& b) {
        table_ = &t;
        tableVersion_ = t.version;
        segF_.assign(net.segs.size(), -1), segB_.assign(net.segs.size(), -1);
        for (size_t e = 0; e < g.edges.size() && e < b.edgeLink.size(); ++e) {
            const FlowEdge& E = g.edges[e];
            if (E.seg < 0 || (size_t)E.seg >= net.segs.size()) continue;
            (E.forward ? segF_ : segB_)[(size_t)E.seg] = b.edgeLink[e];
        }
        spotsKey_ = ~0ull; // positions again
    }

    // Everything from the current cells, hoods and roads. `t` is the herds' link table (nodes = network nodes).
    void Build(const CitySim& s, const SimHoods& hoods, const std::vector<HoodLink>& commute, const Network& net, const SegGrid& grid, const LinkTable& t,
               const std::vector<V2>& flagged, float mapHalf, float carShare, const DemandCurves& curves) {
        builtNet = net.version;
        FindOutsideConnections(net, t, flagged, mapHalf, params, conns);
        CellRoads(s, net, grid);
        links.clear();
        stats = TownTrafficStats();
        stats.conns = (int)conns.size();
        BuildLocal(s, hoods, commute, carShare);
        BuildOutside(s, hoods, carShare);
        stats.links = links.size();
        // what is away during the day: commutes left minus commutes back, per window
        float out = 0, back = 0;
        for (int w = 0; w < kWindows; ++w) {
            out += curves.c[kTripCommute][w], back += curves.c[kTripWorkHome][w];
            away_[w] = std::clamp(out - back, 0.0f, 1.0f);
        }
        if (table_ && table_->version == net.version) BuildSpots(s);
    }

    // The links as peak-hour flow trips (the morning direction: commute, shopping, through, trucks), appended to `out`.
    void FlowTrips(const DemandCurves& curves, const FlowGraph& g, std::vector<Trip>& out) const {
        if (builtNet != g.version) return;
        for (const NodeLink& l : links) {
            if (l.type != kTripCommute && l.type != kTripShop && l.type != kTripThrough && l.type != kTripFreight) continue;
            if (l.fromNode < 0 || l.toNode < 0 || l.fromNode >= g.nodes || l.toNode >= g.nodes || l.fromNode == l.toNode) continue;
            float peak = 0;
            for (int w = 0; w < kWindows; ++w) peak = std::max(peak, curves.c[l.type][w]);
            out.push_back({l.fromNode, l.toNode, l.n * peak * 4.0f, 0});
        }
    }

    // Share of the commuters at work at this minute of the day (0 at night, ~0.8 at noon).
    float AwayShare(double minuteOfDay) const {
        const double m = std::fmod(std::fmod(minuteOfDay, 1440.0) + 1440.0, 1440.0);
        return away_[std::clamp((int)(m / kWindowMinutes), 0, kWindows - 1)];
    }

    // The parked cars within `radius` of the focus, appended to `out` (at most `max`). No allocation once `out` has grown.
    void Parked(V2 focus, float radius, double minuteOfDay, std::vector<CarInstance>& out, int max) {
        stats.parkedShown = 0;
        if (spotsKey_ == ~0ull) return;
        const float away = AwayShare(minuteOfDay);
        const float r2 = radius * radius, reach = radius + kCityCellSize;
        for (const CellSpots& c : cellSpots_) {
            if (stats.parkedShown >= max) break;
            if (std::fabs(c.centre.x - focus.x) > reach || std::fabs(c.centre.z - focus.z) > reach) continue;
            const int shown = std::min<int>((int)c.count, (int)std::lround(c.home * (1.0f - away)) + (int)std::lround(c.work * away));
            for (int k = 0; k < shown && stats.parkedShown < max; ++k) {
                const CarInstance& ci = spots_[c.off + (size_t)k];
                const float dx = ci.p.x - focus.x, dz = ci.p.z - focus.z;
                if (dx * dx + dz * dz > r2) continue;
                out.push_back(ci);
                ++stats.parkedShown;
            }
        }
    }
    size_t SpotCount() const { return spots_.size(); }

private:
    struct CellRoad {
        int node = -1, seg = -1;
        float t = 0;
    };
    struct NodeW {
        int node;
        float w;
        int hood;
    };
    struct CellSpots {
        V2 centre;
        size_t off = 0;
        uint16_t count = 0; // spots laid out
        float home = 0, work = 0; // cars that belong here
    };
    std::vector<CellRoad> cellRoad_; // per CitySim cell
    uint64_t cellKey_ = ~0ull;
    const LinkTable* table_ = nullptr;
    uint64_t tableVersion_ = ~0ull;
    std::vector<int> segF_, segB_;
    std::vector<CarInstance> spots_;
    std::vector<CellSpots> cellSpots_;
    uint64_t spotsKey_ = ~0ull;
    float away_[kWindows] = {};

    static uint64_t Occupancy(const CitySim& s) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < s.cells.cells.size(); ++i)
            if (!s.cells.cells[i].lots.empty()) h = (h ^ (uint64_t)i) * 1099511628211ull;
        return h ^ s.cells.cells.size();
    }

    // Each occupied cell's nearest car-road node (and the segment, for parking), when the roads or the cells changed.
    void CellRoads(const CitySim& s, const Network& net, const SegGrid& grid) {
        const uint64_t key = Occupancy(s) * 31 + net.version;
        if (key == cellKey_ && cellRoad_.size() == s.cells.cells.size()) return;
        cellKey_ = key;
        cellRoad_.assign(s.cells.cells.size(), CellRoad());
        for (size_t i = 0; i < s.cells.cells.size(); ++i) {
            if (s.cells.cells[i].lots.empty()) continue;
            const CellKey k = s.cells.cells[i].key;
            const V2 c{(k.x + 0.5f) * kCityCellSize, (k.z + 0.5f) * kCityCellSize};
            float t, d;
            const int seg = grid.Nearest(net, c, 400.0f, t, d);
            if (seg < 0) continue;
            cellRoad_[i].seg = seg, cellRoad_[i].t = t;
            cellRoad_[i].node = t < 0.5f ? net.segs[(size_t)seg].a : net.segs[(size_t)seg].b;
        }
        spotsKey_ = ~0ull;
    }
    int NodeOf(int cell) const { return (size_t)cell < cellRoad_.size() ? cellRoad_[(size_t)cell].node : -1; }

    static void AddW(std::vector<NodeW>& v, int node, float w, int hood) {
        if (node < 0 || !(w > 0)) return;
        for (NodeW& x : v)
            if (x.node == node) { x.w += w; return; }
        v.push_back({node, w, hood});
    }
    static void Normalize(std::vector<NodeW>& v) {
        double sum = 0;
        for (const NodeW& x : v) sum += x.w;
        for (NodeW& x : v) x.w = sum > 0 ? (float)(x.w / sum) : 0.0f;
        std::sort(v.begin(), v.end(), [](const NodeW& a, const NodeW& b) { return a.node < b.node; });
    }
    // The k heaviest, renormalised (ties by node id), skipping `except`.
    static void Top(const std::vector<NodeW>& v, int k, int except, std::vector<NodeW>& out) {
        out.clear();
        for (const NodeW& x : v)
            if (x.node != except) out.push_back(x);
        std::sort(out.begin(), out.end(), [](const NodeW& a, const NodeW& b) { return a.w != b.w ? a.w > b.w : a.node < b.node; });
        if ((int)out.size() > k) out.resize((size_t)k);
        Normalize(out);
    }
    void Push(int a, int b, float n, int type, int fromHood, int toHood, int homeHood, int workHood) {
        if (a < 0 || b < 0 || a == b || !(n > 1e-3f)) return;
        NodeLink l;
        l.fromNode = a, l.toNode = b, l.n = n, l.type = (uint8_t)type;
        l.fromHood = fromHood, l.toHood = toHood, l.homeHood = homeHood, l.workHood = workHood;
        links.push_back(l);
    }

    // ---- inside one hood: home node -> job node, and back; residents -> the hood's own shops, and back
    void BuildLocal(const CitySim& s, const SimHoods& hoods, const std::vector<HoodLink>& commute, float carShare) {
        std::vector<NodeW> home, job, top;
        for (const HoodLink& l : commute) {
            if (l.from != l.to || (size_t)l.from >= hoods.hoods.size() || !(l.n > 0)) continue;
            const Hood& H = hoods.hoods[(size_t)l.from];
            home.clear(), job.clear();
            for (int c : H.cells) {
                const CellSim& cs = s.sim[(size_t)c];
                AddW(home, NodeOf(c), (float)cs.working, H.id);
                AddW(job, NodeOf(c), (float)cs.employed[0] + cs.employed[1] + cs.employed[2], H.id);
            }
            Normalize(home), Normalize(job);
            for (const NodeW& a : home) {
                Top(job, params.topLocal, a.node, top);
                for (const NodeW& b : top) {
                    const float n = l.n * carShare * a.w * b.w;
                    Push(a.node, b.node, n, kTripCommute, H.id, H.id, H.id, H.id);
                    Push(b.node, a.node, n, kTripWorkHome, H.id, H.id, H.id, H.id);
                    stats.localPerDay += 2.0f * n;
                }
            }
        }
        const float decay = std::max(1.0f, params.shopDecayMetres);
        for (const Hood& H : hoods.hoods) {
            if (H.Population() == 0 || H.jobs[kJobCommercial] == 0) continue;
            double others = 0; // the hood shop links take the rest (city_herds.inl BuildShopLinks)
            for (const Hood& c : hoods.hoods)
                if (c.id != H.id && c.jobs[kJobCommercial] > 0) {
                    const double d = Len(c.centre - H.centre);
                    if (d < 3.0 * decay) others += c.jobs[kJobCommercial] * std::exp(-d / decay);
                }
            const double own = H.jobs[kJobCommercial];
            const float trips = (float)(H.Population() * params.shopPerResident * carShare * own / (own + others));
            home.clear(), job.clear();
            for (int c : H.cells) {
                const CellSim& cs = s.sim[(size_t)c];
                AddW(home, NodeOf(c), (float)cs.Population(), H.id);
                AddW(job, NodeOf(c), (float)cs.jobCap[kJobCommercial], H.id);
            }
            Normalize(home), Normalize(job);
            for (const NodeW& a : home) {
                Top(job, params.topLocal, a.node, top);
                for (const NodeW& b : top) {
                    const float n = trips * a.w * b.w;
                    Push(a.node, b.node, n, kTripShop, H.id, H.id, H.id, H.id);
                    Push(b.node, a.node, n, kTripShopHome, H.id, H.id, H.id, H.id);
                    stats.localPerDay += 2.0f * n;
                }
            }
        }
    }

    // ---- the outside connections
    void BuildOutside(const CitySim& s, const SimHoods& hoods, float carShare) {
        if (conns.empty()) return;
        const double pop = (double)s.Population();
        const float growth = (float)std::min<double>(params.throughGrowthMax, 1.0 + pop / std::max(1.0f, params.throughPopScale));
        for (size_t i = 0; i < conns.size(); ++i)
            for (size_t j = 0; j < conns.size(); ++j) {
                if (i == j || Len(conns[i].p - conns[j].p) < params.minThroughMetres) continue;
                const float n = params.throughPerPair * conns[i].weight * conns[j].weight / 9.0f * growth;
                Push(conns[i].node, conns[j].node, n, kTripThrough, -1, -1, -1, -1);
                stats.throughPerDay += n;
            }
        // the city's side, node by node
        std::vector<NodeW> vac, unemp, shop, ind, top;
        for (size_t c = 0; c < s.sim.size(); ++c) {
            const int node = NodeOf((int)c);
            if (node < 0) continue;
            const CellSim& cs = s.sim[c];
            const int hood = c < hoods.cellHood.size() ? hoods.cellHood[c] : -1;
            AddW(vac, node, (float)cs.vacancies[0] + cs.vacancies[1] + cs.vacancies[2], hood);
            AddW(unemp, node, (float)cs.Unemployed(), hood);
            AddW(shop, node, (float)cs.jobCap[kJobCommercial], hood);
            AddW(ind, node, (float)cs.jobCap[kJobIndustrial] * params.trucksPerIndJob + (float)cs.jobCap[kJobCommercial] * params.trucksPerShopJob, hood);
        }
        auto total = [](const std::vector<NodeW>& v) { double t = 0; for (const NodeW& x : v) t += x.w; return (float)t; };
        const float V = total(vac) * params.inFill * carShare, U = total(unemp) * params.workOutside * carShare;
        const float S = total(shop) * params.visitorsPerShopJob, F = total(ind);
        float wsum = 0;
        for (const OutsideConn& c : conns) wsum += c.weight;
        for (const OutsideConn& c : conns) {
            const float share = c.weight / std::max(1e-3f, wsum);
            Top(vac, params.topNodes, c.node, top);
            for (const NodeW& b : top) { // commuters in: to work in the morning, home in the evening
                const float n = V * share * b.w;
                Push(c.node, b.node, n, kTripCommute, -1, b.hood, -1, b.hood);
                Push(b.node, c.node, n, kTripWorkHome, b.hood, -1, -1, b.hood);
                stats.inPerDay += n;
            }
            Top(unemp, params.topNodes, c.node, top);
            for (const NodeW& a : top) { // residents who work beyond the map
                const float n = U * share * a.w;
                Push(a.node, c.node, n, kTripCommute, a.hood, -1, a.hood, -1);
                Push(c.node, a.node, n, kTripWorkHome, -1, a.hood, a.hood, -1);
                stats.outPerDay += n;
            }
            Top(shop, params.topNodes, c.node, top);
            for (const NodeW& b : top) { // visitors to the shops
                const float n = S * share * b.w;
                Push(c.node, b.node, n, kTripShop, -1, b.hood, -1, b.hood);
                Push(b.node, c.node, n, kTripShopHome, b.hood, -1, -1, b.hood);
                stats.inPerDay += n;
            }
            Top(ind, params.topNodes, c.node, top);
            for (const NodeW& b : top) { // trucks: goods in, exports out
                const float n = F * share * b.w;
                Push(c.node, b.node, n, kTripFreight, -1, b.hood, -1, b.hood);
                Push(b.node, c.node, n, kTripFreight, b.hood, -1, -1, b.hood);
                stats.inPerDay += n;
            }
        }
    }

    // ---- parked cars: positions when the roads or the cells changed, counts every build
    void BuildSpots(const CitySim& s) {
        const LinkTable& t = *table_;
        const uint64_t key = cellKey_ * 131 + tableVersion_;
        if (key != spotsKey_) {
            spotsKey_ = key;
            spots_.clear(), cellSpots_.clear();
            const int perCell = 2 * std::max(1, params.maxPerCell);
            for (size_t c = 0; c < cellRoad_.size(); ++c) {
                const CellRoad& r = cellRoad_[c];
                if (r.seg < 0 || (size_t)r.seg >= segF_.size()) continue;
                const int lf = segF_[(size_t)r.seg], lb = segB_[(size_t)r.seg];
                const bool okF = lf >= 0 && !t.links[(size_t)lf].pipe, okB = lb >= 0 && !t.links[(size_t)lb].pipe;
                if (!okF && !okB) continue; // highways: no curbside parking
                CellSpots cs;
                const CellKey k = s.cells.cells[c].key;
                cs.centre = {(k.x + 0.5f) * kCityCellSize, (k.z + 0.5f) * kCityCellSize};
                cs.off = spots_.size();
                for (int i = 0; i < perCell; ++i) {
                    const bool back = okB && (!okF || (i & 1));
                    const int l = back ? lb : lf;
                    const float len = t.links[(size_t)l].len;
                    const float s0 = (back ? 1.0f - r.t : r.t) * len;
                    const int ring = i / 2; // 0, +1, -1, +2, -2 ... spots outward from the cell's point on the road
                    const float step = (float)((ring + 1) / 2) * ((ring & 1) ? 1.0f : -1.0f);
                    SplitMix64 rnd(SimSeed(params.seed, PackKey(k), (uint64_t)i + 0x9A4C));
                    float sAt = s0 + step * params.spotSpacing + ((float)rnd.Below(100) / 100.0f - 0.5f);
                    sAt = len > 12.0f ? std::clamp(sAt, 6.0f, len - 6.0f) : len * 0.5f;
                    V2 p, d;
                    float y;
                    if (!t.PointAt(l, sAt, p, y, d)) continue;
                    const V2 q = p + RightOf(d) * ((float)t.links[(size_t)l].lanes * params.laneWidth + 1.4f);
                    CarInstance ci;
                    ci.p = {q.x, y, q.z};
                    ci.dir = d;
                    ci.model = (int)rnd.Below((uint32_t)std::max(1, params.models));
                    spots_.push_back(ci);
                }
                cs.count = (uint16_t)(spots_.size() - cs.off);
                cellSpots_.push_back(cs);
            }
        }
        for (CellSpots& cs : cellSpots_) {
            const CellKey k = CellKeyAt(cs.centre.x, cs.centre.z);
            const int c = s.cells.Find(k);
            if (c < 0 || (size_t)c >= s.sim.size()) { cs.home = cs.work = 0; continue; }
            const CellSim& sim = s.sim[(size_t)c];
            cs.home = std::min((float)params.maxPerCell, std::round((float)sim.residents[kAdults] * params.homeCarsPerAdult));
            cs.work = std::min((float)params.maxPerCell, std::round((float)(sim.employed[0] + sim.employed[1] + sim.employed[2]) * params.workCarsPerJob));
        }
    }
};

} // namespace city
} // namespace tbd
