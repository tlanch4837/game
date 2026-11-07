// Routes for herds and promoted cars (docs/city_sim_design.md section 6). Header only, headless (tests\sim_routes_test.cpp).
//
//   BuildLinkTable   FlowGraph edges -> links. A run of highway edges through nodes where nothing else meets is merged into
//                    one link, a "pipe": its travel time is the sum over the run, nobody is simulated inside it. Also the
//                    drawable polyline of every link (children place cars on it).
//   PathFinder       Dijkstra on links with reusable buffers (no allocation after the first call).
//   RouteTree        one backwards Dijkstra from a destination node on free-flow time: nextLink[node] is the link to take
//                    from that node. A car's route is a lookup. RouteCache keeps the trees of active destinations (LRU) and
//                    drops them all when the network version changes. Pure functions: a worker can build them.
//   Alternatives     up to three routes for a pair: the fastest, then k-shortest by penalising the links already used.
//   RoutePicker      shares between a pair's alternatives by logit on their current travel times, smoothed over windows
//                    (share = lerp(share, target, 0.3) once per window), so a loaded highway sheds traffic gradually and
//                    does not flip-flop.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "road_network.h"
#include "sim_flow.h"
#include "sim_herds.h"

namespace tbd {
namespace city {

// ------------------------------------------------------------------------------------------------ link table from the network
struct LinkBuild {
    std::vector<int> edgeLink; // FlowGraph edge -> link
};

inline void BuildLinkTable(const Network& net, const FlowGraph& g, LinkTable& t, LinkBuild* build = nullptr) {
    t = LinkTable();
    t.version = net.version;
    t.nodes = g.nodes;
    const size_t E = g.edges.size();
    auto isHwy = [&](size_t e) { return net.Prof(g.edges[e].seg).cat == Cat::Highway; };
    // successor of a highway edge: the edge that carries on from its end node when nothing else meets there
    std::vector<int> next(E, -1);
    std::vector<uint8_t> hasPrev(E, 0);
    for (size_t e = 0; e < E; ++e) {
        if (!isHwy(e)) continue;
        const FlowEdge& a = g.edges[e];
        if (g.junction[(size_t)a.to]) continue;
        for (int k = g.outOff[(size_t)a.to]; k < g.outOff[(size_t)a.to + 1]; ++k) {
            const FlowEdge& b = g.edges[(size_t)k];
            if (b.to == a.from || b.seg == a.seg || !isHwy((size_t)k)) continue;
            next[e] = k;
            hasPrev[(size_t)k] = 1;
            break;
        }
    }
    std::vector<int> edgeLink(E, -1);
    t.ptOff.push_back(0);
    auto addEdgePoints = [&](size_t e, bool first) {
        const FlowEdge& E_ = g.edges[e];
        const Segment& sg = net.segs[(size_t)E_.seg];
        const Cubic c = net.Curve(E_.seg);
        const int n = std::max(2, (int)std::ceil(E_.len / 12.0f));
        for (int i = first ? 0 : 1; i <= n; ++i) {
            const float u = (float)i / n, tt = E_.forward ? u : 1.0f - u;
            const V2 p = c.At(tt);
            t.pts.push_back(p.x), t.pts.push_back(net.YAt(sg, tt)), t.pts.push_back(p.z);
        }
    };
    auto finishLink = [&](const std::vector<size_t>& chain) {
        LinkInfo l;
        l.from = g.edges[chain.front()].from, l.to = g.edges[chain.back()].to;
        float minutes = 0, cap = 1e30f;
        for (size_t e : chain) {
            l.len += g.edges[e].len;
            minutes += g.edges[e].t0;
            cap = std::min(cap, g.edges[e].cap);
        }
        l.len = std::max(1.0f, l.len);
        l.freeMpm = l.len / std::max(1e-3f, minutes);
        l.cap = cap;
        l.pipe = isHwy(chain.front());
        const Profile& pr = net.Prof(g.edges[chain.front()].seg);
        l.lanes = (uint8_t)std::max(1, g.edges[chain.front()].forward ? pr.fwd : pr.back);
        const int id = (int)t.links.size();
        t.links.push_back(l);
        for (size_t i = 0; i < chain.size(); ++i) {
            addEdgePoints(chain[i], i == 0);
            edgeLink[chain[i]] = id;
        }
        t.ptOff.push_back((uint32_t)(t.pts.size() / 3));
    };
    std::vector<size_t> chain;
    for (size_t e = 0; e < E; ++e) {
        if (edgeLink[e] >= 0 || hasPrev[e]) continue; // chain heads only
        chain.assign(1, e);
        for (int k = next[e]; k >= 0 && edgeLink[(size_t)k] < 0 && chain.size() < 4096; k = next[(size_t)k]) chain.push_back((size_t)k);
        finishLink(chain);
    }
    for (size_t e = 0; e < E; ++e) // a ring of highway edges has no head: break it anywhere
        if (edgeLink[e] < 0) {
            chain.assign(1, e);
            for (int k = next[e]; k >= 0 && edgeLink[(size_t)k] < 0 && k != (int)e && chain.size() < 4096; k = next[(size_t)k]) chain.push_back((size_t)k);
            finishLink(chain);
        }
    t.BuildAdjacency();
    if (build) build->edgeLink = std::move(edgeLink);
}

// ------------------------------------------------------------------------------------------------ path finding
class PathFinder {
public:
    // Forward Dijkstra from `a` to `b`; cost(link) in minutes (+ penalty). Fills `route` with link ids; false if unreachable.
    template <class CostFn>
    bool Find(const LinkTable& t, int a, int b, CostFn&& cost, std::vector<int>& route) {
        route.clear();
        if (a < 0 || b < 0 || a >= t.nodes || b >= t.nodes) return false;
        if (a == b) return true;
        dist_.assign((size_t)t.nodes, INFINITY);
        via_.assign((size_t)t.nodes, -1);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        dist_[(size_t)a] = 0;
        q.push({0.0f, a});
        while (!q.empty()) {
            const auto [d, n] = q.top();
            q.pop();
            if (d > dist_[(size_t)n]) continue;
            if (n == b) break;
            for (int k = t.outOff[(size_t)n]; k < t.outOff[(size_t)n + 1]; ++k) {
                const int l = t.outIdx[(size_t)k];
                const int to = t.links[(size_t)l].to;
                const float nd = d + cost(l);
                if (nd < dist_[(size_t)to]) dist_[(size_t)to] = nd, via_[(size_t)to] = l, q.push({nd, to});
            }
        }
        if (!std::isfinite(dist_[(size_t)b])) return false;
        for (int n = b; n != a;) {
            const int l = via_[(size_t)n];
            route.push_back(l);
            n = t.links[(size_t)l].from;
        }
        std::reverse(route.begin(), route.end());
        return true;
    }

private:
    std::vector<float> dist_;
    std::vector<int> via_;
};

inline float FreeMinutes(const LinkTable& t, int l) { return t.links[(size_t)l].len / std::max(1e-3f, t.links[(size_t)l].freeMpm); }

// ------------------------------------------------------------------------------------------------ route trees
struct RouteTree {
    int dest = -1;
    uint64_t version = 0;
    std::vector<int> nextLink; // per node: the link to take towards dest (-1: dest itself or no way)
    std::vector<float> minutes; // per node: free-flow minutes to dest (INFINITY: none)

    static void Build(const LinkTable& t, int dest, RouteTree& out) {
        out.dest = dest;
        out.version = t.version;
        out.nextLink.assign((size_t)t.nodes, -1);
        out.minutes.assign((size_t)t.nodes, INFINITY);
        if (dest < 0 || dest >= t.nodes) return;
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        out.minutes[(size_t)dest] = 0;
        q.push({0.0f, dest});
        while (!q.empty()) {
            const auto [d, n] = q.top();
            q.pop();
            if (d > out.minutes[(size_t)n]) continue;
            for (int k = t.inOff[(size_t)n]; k < t.inOff[(size_t)n + 1]; ++k) {
                const int l = t.inIdx[(size_t)k];
                const int from = t.links[(size_t)l].from;
                const float nd = d + FreeMinutes(t, l);
                if (nd < out.minutes[(size_t)from]) out.minutes[(size_t)from] = nd, out.nextLink[(size_t)from] = l, q.push({nd, from});
            }
        }
    }
    // The route from `node` to the destination by following next-hops (empty: none, or already there).
    bool Follow(const LinkTable& t, int node, std::vector<int>& route) const {
        route.clear();
        if (node < 0 || (size_t)node >= nextLink.size() || !std::isfinite(minutes[(size_t)node])) return false;
        for (int guard = 0; node != dest && guard < 100000; ++guard) {
            const int l = nextLink[(size_t)node];
            if (l < 0) return false;
            route.push_back(l);
            node = t.links[(size_t)l].to;
        }
        return true;
    }
};

class RouteCache {
public:
    size_t capacity = 24;
    uint64_t builds = 0, drops = 0;

    // The tree for dest if it is cached and current; touching it makes it recent.
    const RouteTree* Get(uint64_t version, int dest) {
        if (version != version_) Drop(version);
        for (Entry& e : entries_)
            if (e.tree.dest == dest) return e.stamp = ++clock_, &e.tree;
        return nullptr;
    }
    // Builds (or rebuilds) the tree for dest now. A worker may call RouteTree::Build itself and hand the result to Put.
    const RouteTree& Ensure(const LinkTable& t, int dest) {
        if (const RouteTree* r = Get(t.version, dest)) return *r;
        RouteTree tr;
        RouteTree::Build(t, dest, tr);
        return Put(std::move(tr));
    }
    const RouteTree& Put(RouteTree&& tr) {
        if (tr.version != version_) Drop(tr.version);
        ++builds;
        if (entries_.size() >= capacity) { // evict the least recent
            size_t old = 0;
            for (size_t i = 1; i < entries_.size(); ++i)
                if (entries_[i].stamp < entries_[old].stamp) old = i;
            entries_[old] = Entry{std::move(tr), ++clock_};
            return entries_[old].tree;
        }
        entries_.push_back(Entry{std::move(tr), ++clock_});
        return entries_.back().tree;
    }
    size_t Size() const { return entries_.size(); }
    void Drop(uint64_t version) {
        drops += entries_.size();
        entries_.clear();
        version_ = version;
    }

private:
    struct Entry {
        RouteTree tree;
        uint64_t stamp = 0;
    };
    std::vector<Entry> entries_;
    uint64_t version_ = 0, clock_ = 0;
};

// ------------------------------------------------------------------------------------------------ alternatives and the pick
// Up to kMaxAlternatives distinct routes: the fastest on current times, then the fastest again with the links already used
// made 1.7x as costly each round (so highway 1, highway 2, local roads come out as separate routes where they exist).
// Alternatives much slower than the best (past slowFactor) are left out.
inline int Alternatives(const LinkTable& t, PathFinder& pf, int a, int b, const std::function<float(int)>& minutes, std::vector<std::vector<int>>& out,
                        float slowFactor = 1.8f) {
    out.clear();
    std::vector<float> mult(t.links.size(), 1.0f);
    std::vector<int> route;
    float best = 0;
    for (int round = 0; round < kMaxAlternatives + 2 && (int)out.size() < kMaxAlternatives; ++round) {
        if (!pf.Find(t, a, b, [&](int l) { return minutes(l) * mult[(size_t)l]; }, route)) break;
        if (route.empty()) break;
        for (int l : route) mult[(size_t)l] *= 1.7f;
        if (std::find(out.begin(), out.end(), route) != out.end()) continue;
        float tm = 0;
        for (int l : route) tm += minutes(l);
        if (out.empty()) best = tm;
        else if (tm > best * slowFactor) continue;
        out.push_back(route);
    }
    return (int)out.size();
}

struct PickParams {
    float theta = 0.35f;     // logit sensitivity, 1 / minutes: five minutes slower is about a sixth of the share
    float smooth = 0.3f;     // share = lerp(share, target, smooth) once per window
    int maxNewPerWindow = 12; // pairs computed from scratch per window (the rest wait a window)
};

class RoutePicker {
public:
    PickParams params;
    const LinkTable* table = nullptr;
    HerdSystem* herds = nullptr; // for LinkSpeed (current loads) and the route store
    PathFinder finder;
    RouteCache cache;
    uint64_t pairsBuilt = 0, pairsDeferred = 0;

    // The host sets the window before ReleaseHerds: shares of a pair move once per window however often it is asked.
    void BeginWindow(int64_t window) {
        window_ = window;
        newThisWindow_ = 0;
    }
    void SetTable(const LinkTable* t) {
        if (t != table || (t && t->version != version_)) {
            pairs_.clear();
            if (t) version_ = t->version;
        }
        table = t;
    }

    // The RouteFn for HerdSystem::ReleaseHerds.
    int Pick(int a, int b, RouteChoice* out) {
        if (!table || !herds) return 0;
        const uint64_t key = ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
        auto it = pairs_.find(key);
        if (it == pairs_.end()) {
            if (newThisWindow_ >= params.maxNewPerWindow) {
                ++pairsDeferred;
                return 0; // wait a window; the herd's trips stay pending
            }
            ++newThisWindow_, ++pairsBuilt;
            Pair p;
            std::vector<std::vector<int>> alts;
            Alternatives(*table, finder, a, b, [&](int l) { return Minutes(l); }, alts);
            if (alts.empty()) {
                it = pairs_.emplace(key, p).first; // unreachable: remembered so it is not searched again every window
            } else {
                p.n = (int)alts.size();
                for (int i = 0; i < p.n; ++i) p.route[i] = herds->routes.Intern(*table, alts[(size_t)i]), p.share[i] = i == 0 ? 1.0f : 0.0f;
                it = pairs_.emplace(key, p).first;
            }
            it->second.window = window_ - 1;
        }
        Pair& p = it->second;
        if (p.n == 0) return 0;
        if (p.window != window_ && p.n > 1) {
            p.window = window_;
            float tm[kMaxAlternatives], best = INFINITY;
            for (int i = 0; i < p.n; ++i) {
                tm[i] = 0;
                const RouteStore::Rec& r = herds->routes.recs[p.route[i]];
                for (uint32_t k = 0; k < r.n; ++k) tm[i] += Minutes(herds->routes.links[r.off + k]);
                best = std::min(best, tm[i]);
            }
            float w[kMaxAlternatives], sum = 0;
            for (int i = 0; i < p.n; ++i) sum += w[i] = std::exp(-params.theta * (tm[i] - best));
            const float k = p.fresh ? 1.0f : params.smooth; // a new pair starts at its target
            p.fresh = false;
            for (int i = 0; i < p.n; ++i) p.share[i] += (w[i] / sum - p.share[i]) * k;
        }
        p.window = window_;
        for (int i = 0; i < p.n; ++i) out[i] = {p.route[i], p.share[i]};
        return p.n;
    }
    float Share(int a, int b, int alt) const {
        const auto it = pairs_.find(((uint64_t)(uint32_t)a << 32) | (uint32_t)b);
        return it == pairs_.end() || alt >= it->second.n ? 0.0f : it->second.share[alt];
    }
    size_t Pairs() const { return pairs_.size(); }

private:
    struct Pair {
        int n = 0;
        uint32_t route[kMaxAlternatives] = {~0u, ~0u, ~0u};
        float share[kMaxAlternatives] = {1, 0, 0};
        int64_t window = -1;
        bool fresh = true;
    };
    // minutes to drive link l with the current loads
    float Minutes(int l) const { return table->links[(size_t)l].len / std::max(1e-3f, herds->LinkSpeed(l)); }

    std::unordered_map<uint64_t, Pair> pairs_;
    int64_t window_ = 0;
    int newThisWindow_ = 0;
    uint64_t version_ = 0;
};

} // namespace city
} // namespace tbd
