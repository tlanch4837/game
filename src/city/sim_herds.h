// Herds over the day (docs/city_sim_design.md section 6). Header only, headless (tests\sim_herds_test.cpp).
//
// A herd is everyone with the same origin neighbourhood, destination neighbourhood, trip type and departure window (15
// game minutes). It has a size (vehicles), a route (a list of links) and a progress. Nobody inside it is simulated.
//
//   LinkTable     the road network as links: a junction-to-junction stretch, or a whole highway run ("pipe": one link, travel
//                 time from load, nobody simulated inside). Built from the Network by sim_routes.h; tests build it by hand.
//   DemandCurves  per trip type, the share of the day's trips that leave in each of the 96 windows (each sums to 1).
//   ReleaseHerds  every window: pending trips = link count x car share x curve[window] (a fraction carries over to the next
//                 window, so the day's herds add up to the day's trips); a herd goes out once it is a vehicle or more. A
//                 pair with route alternatives splits the herd by the route shares.
//   AdvanceHerds  per tick: progress += LinkSpeed(link) x speed factor; the load of the window counts the herd when it
//                 enters a link; the BPR curve slows loaded links. A herd is a train: its head and tail spread out with
//                 speedSd as it goes, and it dissolves at the destination from head to tail (arrivals spread out).
//   Chains        a school drop-off herd that has arrived sends a share of its vehicles on to work.
//   Node links    trips between two graph nodes rather than two hoods (sim_town_traffic.h): the commutes and shopping inside
//                 one hood (home cell -> job cell), and the outside connections (through traffic, commuters, visitors and
//                 trucks from the map edge). Released by ReleaseHerds like the hood links, as a stream of small herds
//                 spread over the window so a road shows a steady trickle rather than one bunch every 15 minutes.
// Trips are vehicles: car share is already applied (people on foot, bus or train are not herds yet).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "road_network.h"
#include "sim.h"
#include "sim_hoods.h"

namespace tbd {
namespace city {

// ------------------------------------------------------------------------------------------------ links and routes
struct LinkInfo {
    int from = -1, to = -1; // graph nodes at the ends
    float len = 1;          // metres
    float freeMpm = 1;      // free-flow metres per minute
    float cap = 1;          // vehicles per hour
    uint8_t pipe = 0;       // a highway run merged into one link
    uint8_t lanes = 1;      // car lanes in the link's direction
};

struct LinkTable {
    std::vector<LinkInfo> links;
    std::vector<float> pts;       // polyline, x y z per point, in driving direction (children place cars on it)
    std::vector<uint32_t> ptOff;  // links + 1 offsets into pts, in points
    std::vector<int> outOff, outIdx; // links leaving node n: outIdx[outOff[n] .. outOff[n+1])
    std::vector<int> inOff, inIdx;   // links entering node n
    int nodes = 0;
    uint64_t version = 0;         // Network::version it came from

    void BuildAdjacency() {
        int maxNode = nodes - 1;
        for (const LinkInfo& l : links) maxNode = std::max(maxNode, std::max(l.from, l.to));
        nodes = maxNode + 1;
        outOff.assign((size_t)nodes + 1, 0);
        for (const LinkInfo& l : links) ++outOff[(size_t)l.from + 1];
        for (int n = 0; n < nodes; ++n) outOff[(size_t)n + 1] += outOff[(size_t)n];
        outIdx.assign(links.size(), 0);
        std::vector<int> fill(outOff.begin(), outOff.end() - 1);
        for (size_t i = 0; i < links.size(); ++i) outIdx[(size_t)fill[(size_t)links[i].from]++] = (int)i;
        inOff.assign((size_t)nodes + 1, 0);
        for (const LinkInfo& l : links) ++inOff[(size_t)l.to + 1];
        for (int n = 0; n < nodes; ++n) inOff[(size_t)n + 1] += inOff[(size_t)n];
        inIdx.assign(links.size(), 0);
        fill.assign(inOff.begin(), inOff.end() - 1);
        for (size_t i = 0; i < links.size(); ++i) inIdx[(size_t)fill[(size_t)links[i].to]++] = (int)i;
    }
    // Point and heading `s` metres along link `l` (clamped), from the polyline; y is the road height. Straight links
    // without a polyline interpolate between nothing: callers that skip the polyline get {0,0,0}.
    bool PointAt(int l, float s, V2& p, float& y, V2& dir) const {
        if ((size_t)l + 1 >= ptOff.size()) return false;
        const uint32_t a = ptOff[(size_t)l], b = ptOff[(size_t)l + 1];
        if (b < a + 2) return false;
        s = std::clamp(s, 0.0f, links[(size_t)l].len);
        float acc = 0;
        for (uint32_t i = a; i + 1 < b; ++i) {
            const V2 p0{pts[i * 3], pts[i * 3 + 2]}, p1{pts[(i + 1) * 3], pts[(i + 1) * 3 + 2]};
            const float seg = Len(p1 - p0);
            if (acc + seg >= s || i + 2 == b) {
                const float t = seg > 1e-4f ? std::clamp((s - acc) / seg, 0.0f, 1.0f) : 0.0f;
                p = p0 + (p1 - p0) * t;
                y = pts[i * 3 + 1] + (pts[(i + 1) * 3 + 1] - pts[i * 3 + 1]) * t;
                dir = Norm(p1 - p0);
                return true;
            }
            acc += seg;
        }
        return false;
    }
    // The point of link l nearest p: metres along the link and the distance to it (false: no polyline).
    bool Project(int l, V2 p, float& s, float& dist) const {
        if ((size_t)l + 1 >= ptOff.size()) return false;
        const uint32_t a = ptOff[(size_t)l], b = ptOff[(size_t)l + 1];
        if (b < a + 2) return false;
        float acc = 0, best = 1e30f;
        s = 0;
        for (uint32_t i = a; i + 1 < b; ++i) {
            const V2 p0{pts[i * 3], pts[i * 3 + 2]}, p1{pts[(i + 1) * 3], pts[(i + 1) * 3 + 2]};
            const V2 d = p1 - p0;
            const float seg = Len(d);
            const float t = seg > 1e-4f ? std::clamp(Dot2(p - p0, d) / (seg * seg), 0.0f, 1.0f) : 0.0f;
            const float dd = Len(p0 + d * t - p);
            if (dd < best) best = dd, s = acc + seg * t;
            acc += seg;
        }
        dist = best;
        return true;
    }
    // A straight link from a to b with the given class numbers (tests, and the fallback for links without geometry).
    int AddStraight(int from, int to, V2 a, V2 b, float kmh, float cap, int lanes = 1, bool pipe = false) {
        LinkInfo l;
        l.from = from, l.to = to;
        l.len = std::max(1.0f, Len(b - a));
        l.freeMpm = kmh * 1000.0f / 60.0f;
        l.cap = cap, l.lanes = (uint8_t)lanes, l.pipe = pipe;
        if (ptOff.empty()) ptOff.push_back(0);
        links.push_back(l);
        for (V2 p : {a, b}) pts.push_back(p.x), pts.push_back(0), pts.push_back(p.z);
        ptOff.push_back((uint32_t)(pts.size() / 3));
        return (int)links.size() - 1;
    }
};

// Routes are interned lists of link ids; a herd holds only an id. Cleared (with the herds) when the link table changes.
class RouteStore {
public:
    struct Rec {
        uint32_t off = 0, n = 0;
        float len = 0;
    };
    std::vector<int> links;
    std::vector<float> cum; // parallel to links: metres from the route start to the END of that link
    std::vector<Rec> recs;

    void Clear() {
        links.clear(), cum.clear(), recs.clear(), index_.clear();
    }
    uint32_t Intern(const LinkTable& t, const std::vector<int>& route) {
        if (route.empty()) return ~0u;
        uint64_t h = 1469598103934665603ull;
        for (int l : route) h = (h ^ (uint64_t)(uint32_t)l) * 1099511628211ull;
        auto range = index_.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            const Rec& r = recs[it->second];
            if (r.n == route.size() && std::equal(route.begin(), route.end(), links.begin() + r.off)) return it->second;
        }
        Rec r;
        r.off = (uint32_t)links.size(), r.n = (uint32_t)route.size();
        float acc = 0;
        for (int l : route) {
            acc += t.links[(size_t)l].len;
            links.push_back(l);
            cum.push_back(acc);
        }
        r.len = acc;
        recs.push_back(r);
        index_.emplace(h, (uint32_t)recs.size() - 1);
        return (uint32_t)recs.size() - 1;
    }
    int LinkAt(uint32_t route, uint32_t i) const { return links[recs[route].off + i]; }
    float EndAt(uint32_t route, uint32_t i) const { return cum[recs[route].off + i]; }
    // Where `dist` metres along the route is: link index within the route and metres into that link.
    void Locate(const LinkTable& t, uint32_t route, float dist, uint32_t& idx, float& into) const {
        const Rec& r = recs[route];
        dist = std::clamp(dist, 0.0f, r.len);
        uint32_t lo = 0, hi = r.n - 1;
        while (lo < hi) { // first link whose end is >= dist
            const uint32_t mid = (lo + hi) / 2;
            if (cum[r.off + mid] >= dist) hi = mid;
            else lo = mid + 1;
        }
        idx = lo;
        const float start = lo ? cum[r.off + lo - 1] : 0.0f;
        into = std::min(dist - start, t.links[(size_t)links[r.off + lo]].len);
    }

private:
    std::unordered_multimap<uint64_t, uint32_t> index_;
};

// ------------------------------------------------------------------------------------------------ demand over the day
// kTripThrough: vehicles crossing the map between two outside connections; kTripFreight: trucks to and from the map edge.
enum TripType : int { kTripCommute = 0, kTripWorkHome, kTripSchool, kTripSchoolHome, kTripShop, kTripShopHome, kTripChain, kTripThrough, kTripFreight, kTripTypes };
constexpr int kWindows = 96;       // 15-minute windows in a day
constexpr int kWindowMinutes = 15;

struct DemandCurves {
    float c[kTripTypes][kWindows] = {};
    DemandCurves() { Build(); }
    void Build() {
        auto bump = [](float hour, float mean, float sd) { // wraps midnight
            float d = std::fabs(hour - mean);
            d = std::min(d, 24.0f - d);
            return std::exp(-0.5f * d * d / (sd * sd));
        };
        for (int w = 0; w < kWindows; ++w) {
            const float h = (w + 0.5f) / 4.0f;
            c[kTripCommute][w] = 0.86f * bump(h, 7.6f, 0.95f) + 0.08f * bump(h, 13.6f, 1.0f) + 0.06f * bump(h, 21.6f, 1.0f);
            c[kTripWorkHome][w] = 0.86f * bump(h, 17.0f, 1.2f) + 0.08f * bump(h, 14.0f, 1.0f) + 0.06f * bump(h, 6.0f, 1.0f);
            c[kTripSchool][w] = bump(h, 7.9f, 0.4f);
            c[kTripSchoolHome][w] = bump(h, 15.4f, 0.5f);
            c[kTripShop][w] = 0.4f * bump(h, 12.4f, 1.6f) + 0.6f * bump(h, 18.4f, 1.6f);
            c[kTripShopHome][w] = 0.4f * bump(h, 13.6f, 1.6f) + 0.6f * bump(h, 19.6f, 1.6f);
            c[kTripChain][w] = 0;
            // through traffic never stops: a floor all night, both rush hours on top
            c[kTripThrough][w] = 0.25f + 0.5f * bump(h, 8.0f, 1.8f) + 0.5f * bump(h, 17.0f, 2.0f) + 0.3f * bump(h, 12.5f, 3.0f);
            c[kTripFreight][w] = 0.08f + bump(h, 11.0f, 3.2f); // trucks run in the working day
        }
        for (int t = 0; t < kTripTypes; ++t) {
            float sum = 0;
            for (int w = 0; w < kWindows; ++w) sum += c[t][w];
            if (sum > 0)
                for (int w = 0; w < kWindows; ++w) c[t][w] /= sum;
        }
    }
};

// ------------------------------------------------------------------------------------------------ herds
struct RouteChoice {
    uint32_t route = ~0u;
    float share = 0; // of the herd; the choices of one pair sum to 1
};
constexpr int kMaxAlternatives = 3;
// Routes for a pair of graph nodes: fills up to kMaxAlternatives choices, returns how many (0: unreachable).
using RouteFn = std::function<int(int fromNode, int toNode, RouteChoice* out)>;

// A trip between two graph nodes (sim_town_traffic.h builds them): vehicles per day (car share already applied), the trip
// type that picks the demand curve, and the hoods behind each end (-1: the outside world, no tenants to pick).
struct NodeLink {
    int fromNode = -1, toNode = -1;
    float n = 0;
    uint8_t type = kTripCommute;
    int fromHood = -1, toHood = -1, homeHood = -1, workHood = -1;
};

struct Herd {
    uint32_t id = 0;
    int from = 0, to = 0;       // hoods the herd travels between
    int homeHood = 0, workHood = 0; // where the people live and where they go: the tenants of the herd come from these
    uint8_t type = kTripCommute;
    bool alive = false;
    float size = 0;             // vehicles
    float wait = 0;             // minutes before it leaves (spread inside the window)
    uint32_t route = ~0u;
    uint32_t linkIdx = 0;       // index into the route of the link the head-of-mean is on
    float progress = 0;         // metres of the route the herd's middle has covered
    float speedMean = 1;        // personality of the herd: x link speed
    float speedSd = 0;          // spread of the cars' speeds (fraction): the train stretches by this as it goes
    float delivered = 0;        // vehicles dissolved into the destination so far
    int64_t departMinute = 0;
    float distance = 0;         // route length, metres
};

struct HerdStats {
    float released[kTripTypes] = {};
    float delivered[kTripTypes] = {};
    uint64_t herdsReleased = 0;
};

class HerdSystem {
public:
    float carShare = 0.75f;      // of commutes made by car
    float schoolDropoff = 0.35f; // of school children driven
    float chainShare = 0.6f;     // of the drop-off vehicles that go on to work
    float speedSdMin = 0.06f, speedSdMax = 0.16f;
    float bprAlpha = 0.15f, bprBeta = 4.0f;
    float streamVehicles = 3.0f; // a node link's window releases as herds of about this many vehicles,
    int streamMax = 6;           // at most this many, spread over the window
    uint64_t seed = 1;

    DemandCurves curves;
    RouteStore routes;
    std::vector<Herd> herds;      // live and dead (dead slots are reused)
    std::vector<float> loadCur, loadPrev, loadDay; // vehicles that entered each link this window / last window / today
    std::vector<float> arrivedDay; // per hood: vehicles delivered today
    HerdStats stats;
    std::function<void(int hood, float n, const Herd&)> onArrive; // optional

    const LinkTable* table = nullptr;

    // New link table (a road edit): herds in flight and their routes are dropped, pending trips are kept.
    void SetTable(const LinkTable* t) {
        table = t;
        herds.clear();
        free_.clear();
        routes.Clear();
        const size_t n = t ? t->links.size() : 0;
        loadCur.assign(n, 0.0f), loadPrev.assign(n, 0.0f), loadDay.assign(n, 0.0f);
    }
    void NewDay() {
        std::fill(loadDay.begin(), loadDay.end(), 0.0f);
        std::fill(arrivedDay.begin(), arrivedDay.end(), 0.0f);
        stats = HerdStats();
    }
    size_t Live() const {
        size_t n = 0;
        for (const Herd& h : herds) n += h.alive;
        return n;
    }
    float PendingTotal() const {
        double s = 0;
        for (const auto& [k, v] : pending_) s += v;
        for (const auto& [k, v] : pendingNode_) s += v.n;
        return (float)s;
    }

    // BPR: free speed / (1 + a (flow / capacity)^b), flow in vehicles per hour from the entries of the window.
    float LinkSpeed(int link) const {
        const LinkInfo& l = table->links[(size_t)link];
        const float vph = std::max(loadPrev[(size_t)link], loadCur[(size_t)link]) * (60.0f / kWindowMinutes);
        const float vc = vph / std::max(1.0f, l.cap);
        return l.freeMpm / (1.0f + bprAlpha * std::pow(vc, bprBeta));
    }
    float LinkLoadVph(int link) const { return std::max(loadPrev[(size_t)link], loadCur[(size_t)link]) * (60.0f / kWindowMinutes); }

    // Scheduler system, period 15 game minutes: `tod` is the minute of the day the window starts at.
    // `nodes` (optional): node-to-node trips, released after the hood links (see NodeLink).
    void ReleaseHerds(int64_t minute, int tod, const std::vector<Hood>& hoods, const std::vector<HoodLink>& commute,
                      const std::vector<HoodLink>& school, const std::vector<HoodLink>& shop, const RouteFn& route,
                      const std::vector<NodeLink>* nodes = nullptr) {
        if (!table) return;
        const int w = ((tod % 1440) / kWindowMinutes) % kWindows;
        std::swap(loadPrev, loadCur);
        std::fill(loadCur.begin(), loadCur.end(), 0.0f);
        routeFn_ = route;
        hoodNode_.resize(hoods.size());
        for (size_t i = 0; i < hoods.size(); ++i) hoodNode_[i] = hoods[i].centroidNode;
        arrivedDay.resize(hoods.size(), 0.0f);
        // the best workplace of each home hood: where a parent drives on after the school drop-off
        topWork_.assign(hoods.size(), -1);
        {
            std::vector<float> best(hoods.size(), 0.0f);
            for (const HoodLink& l : commute)
                if (l.from != l.to && l.n > best[(size_t)l.from]) best[(size_t)l.from] = l.n, topWork_[(size_t)l.from] = l.to;
        }
        for (const HoodLink& l : commute) {
            if (l.from == l.to) continue;
            Add(l.from, l.to, kTripCommute, l.n * carShare * curves.c[kTripCommute][w]);
            Add(l.from, l.to, kTripWorkHome, l.n * carShare * curves.c[kTripWorkHome][w]);
        }
        for (const HoodLink& l : school) {
            if (l.from == l.to) continue;
            Add(l.from, l.to, kTripSchool, l.n * schoolDropoff * curves.c[kTripSchool][w]);
            Add(l.from, l.to, kTripSchoolHome, l.n * schoolDropoff * curves.c[kTripSchoolHome][w]);
        }
        for (const HoodLink& l : shop) {
            if (l.from == l.to) continue;
            Add(l.from, l.to, kTripShop, l.n * carShare * curves.c[kTripShop][w]);
            Add(l.from, l.to, kTripShopHome, l.n * carShare * curves.c[kTripShopHome][w]);
        }
        // release in a fixed order so the same inputs always give the same herds
        order_.clear();
        for (const auto& [k, v] : pending_)
            if (v >= 1.0f) order_.push_back(k);
        std::sort(order_.begin(), order_.end());
        for (uint64_t k : order_) {
            const int type = (int)(k & 15), home = (int)((k >> 4) & 0xFFFFFF), work = (int)((k >> 28) & 0xFFFFFF);
            const bool back = type == kTripWorkHome || type == kTripSchoolHome || type == kTripShopHome;
            const int from = back ? work : home, to = back ? home : work;
            const float size = pending_[k];
            if (Release(minute, from, to, home, work, type, size, w, k)) pending_[k] = 0;
            else pending_[k] = std::min(pending_[k], 400.0f); // an unreachable pair does not pile up a whole day
        }
        if (nodes) ReleaseNodes(minute, w, *nodes);
    }

    // Per tick (game minutes dt): move every herd along its route, dissolve arrivals.
    void AdvanceHerds(float dt) {
        if (!table) return;
        for (size_t i = 0; i < herds.size(); ++i) {
            Herd& h = herds[i];
            if (!h.alive) continue;
            if (h.wait > 0) {
                h.wait -= dt;
                if (h.wait > 0) continue;
                EnterLink(h, 0);
                // on the road from this tick, moving from the next: a trip shorter than a tick's drive (most trips inside a
                // town) would otherwise start and dissolve in one call and the children would never see it
                continue;
            }
            const RouteStore::Rec& r = routes.recs[h.route];
            const int link = routes.LinkAt(h.route, h.linkIdx);
            h.progress += LinkSpeed(link) * h.speedMean * dt;
            while (h.linkIdx + 1 < r.n && h.progress >= routes.EndAt(h.route, h.linkIdx)) EnterLink(h, h.linkIdx + 1);
            // the train: cars are spread evenly between tail and head; the head has met the end first
            const float head = h.progress * (1.0f + h.speedSd), tail = h.progress * (1.0f - h.speedSd);
            const float f = head <= r.len ? 0.0f : tail >= r.len ? 1.0f : (head - r.len) / std::max(1e-3f, head - tail);
            const float total = h.size * f, delta = total - h.delivered;
            if (delta > 0) {
                h.delivered = total;
                stats.delivered[h.type] += delta;
                if ((size_t)h.to < arrivedDay.size()) arrivedDay[(size_t)h.to] += delta;
                if (onArrive) onArrive(h.to, delta, h);
            }
            if (f >= 1.0f) {
                if (h.type == kTripSchool) chainQ_.push_back(h);
                h.alive = false;
                free_.push_back((uint32_t)i);
            }
        }
        for (size_t i = 0; i < chainQ_.size(); ++i) Chain(chainQ_[i]); // after the loop: spawning may move the herd array
        chainQ_.clear();
    }

    // Vehicles of a herd that are on the road right now: not yet dissolved.
    static float OnRoad(const Herd& h) { return h.alive && h.wait <= 0 ? h.size - h.delivered : 0.0f; }

    // Head and tail of a herd, metres along its route (clamped to the route).
    void Span(const Herd& h, float& tail, float& head) const {
        const float L = routes.recs[h.route].len;
        tail = std::clamp(h.progress * (1.0f - h.speedSd), 0.0f, L);
        head = std::clamp(h.progress * (1.0f + h.speedSd), 0.0f, L);
    }

private:
    std::unordered_map<uint64_t, float> pending_; // (type, home hood, work hood) -> trips not yet a vehicle
    struct NodePending {
        float n = 0;
        uint32_t seen = 0; // the release that last listed the link
        NodeLink link;
    };
    std::unordered_map<uint64_t, NodePending> pendingNode_; // (type, from node, to node) -> vehicles not yet released
    uint32_t nodeRelease_ = 0;
    std::vector<uint64_t> orderNode_;
    std::vector<uint32_t> free_;
    std::vector<uint64_t> order_;

    static uint64_t NodeKey(const NodeLink& l) { return (uint64_t)l.type | ((uint64_t)(uint32_t)l.fromNode << 4) | ((uint64_t)(uint32_t)l.toNode << 34); }

    // The node links' share of this window, then a stream of small herds for every pair that has a vehicle or more.
    void ReleaseNodes(int64_t minute, int w, const std::vector<NodeLink>& links) {
        ++nodeRelease_;
        for (const NodeLink& l : links) {
            if (l.fromNode < 0 || l.toNode < 0 || l.fromNode == l.toNode || !(l.n > 0) || l.type >= kTripTypes) continue;
            NodePending& p = pendingNode_[NodeKey(l)];
            p.n += l.n * curves.c[l.type][w];
            p.seen = nodeRelease_;
            p.link = l;
        }
        orderNode_.clear();
        for (const auto& [k, v] : pendingNode_)
            if (v.n >= 1.0f) orderNode_.push_back(k);
        std::sort(orderNode_.begin(), orderNode_.end());
        for (uint64_t k : orderNode_) {
            NodePending& p = pendingNode_[k];
            if (ReleaseNode(minute, w, k, p)) p.n = 0;
            else p.n = std::min(p.n, 400.0f);
        }
        // settled pairs and pairs the builder no longer lists leave the map (a live one is added back next window)
        for (auto it = pendingNode_.begin(); it != pendingNode_.end();) {
            if (it->second.n <= 0.0f || (it->second.seen != nodeRelease_ && it->second.n < 1.0f)) it = pendingNode_.erase(it);
            else ++it;
        }
    }
    bool ReleaseNode(int64_t minute, int w, uint64_t key, const NodePending& p) {
        const NodeLink& l = p.link;
        if (!routeFn_ || !table || l.fromNode >= table->nodes || l.toNode >= table->nodes) return true; // nowhere to drive: dropped
        RouteChoice ch[kMaxAlternatives];
        const int n = routeFn_(l.fromNode, l.toNode, ch);
        if (n <= 0) return false;
        int big = 0;
        for (int i = 1; i < n; ++i)
            if (ch[i].share > ch[big].share) big = i;
        float folded = 0;
        for (int i = 0; i < n; ++i)
            if (i != big && p.n * ch[i].share < 0.25f) folded += ch[i].share, ch[i].share = 0;
        ch[big].share += folded;
        for (int i = 0; i < n; ++i) {
            if (ch[i].share <= 0 || ch[i].route == ~0u) continue;
            const float part = p.n * ch[i].share;
            const int pieces = std::clamp((int)std::ceil(part / std::max(0.5f, streamVehicles)), 1, std::max(1, streamMax));
            for (int k = 0; k < pieces; ++k) { // evenly over the window, a little jitter inside each slice
                SplitMix64 r(SimSeed(seed ^ 0x57EAull, key * 131 + (uint64_t)i, (uint64_t)w * 977 + (uint64_t)k + (uint64_t)(minute / 1440) * 7919));
                const float wait = std::max(0.01f, ((float)k + (float)(r.Below(1000) / 1000.0)) * (float)kWindowMinutes / (float)pieces);
                Spawn(minute, l.fromHood, l.toHood, l.homeHood, l.workHood, l.type, part / (float)pieces, ch[i].route, w, key, i * 16 + k, wait);
            }
        }
        stats.released[l.type] += p.n;
        return true;
    }
    std::vector<Herd> chainQ_;
    std::vector<int> hoodNode_, topWork_;
    RouteFn routeFn_;

    static uint64_t Key(int home, int work, int type) { return (uint64_t)type | ((uint64_t)home << 4) | ((uint64_t)work << 28); }
    void Add(int home, int work, int type, float n) {
        if (n <= 0) return;
        pending_[Key(home, work, type)] += n;
    }
    void EnterLink(Herd& h, uint32_t idx) {
        h.linkIdx = idx;
        const int link = routes.LinkAt(h.route, idx);
        loadCur[(size_t)link] += h.size;
        loadDay[(size_t)link] += h.size;
    }

    bool Release(int64_t minute, int from, int to, int home, int work, int type, float size, int window, uint64_t key) {
        if ((size_t)from >= hoodNode_.size() || (size_t)to >= hoodNode_.size()) return false;
        const int a = hoodNode_[(size_t)from], b = hoodNode_[(size_t)to];
        if (a < 0 || b < 0 || a == b || !routeFn_) return true; // nowhere to drive: the trips stay local (dropped)
        RouteChoice ch[kMaxAlternatives];
        const int n = routeFn_(a, b, ch);
        if (n <= 0) return false; // unreachable for now: keep the pending trips, try the next window
        // alternatives that would be under a quarter vehicle fold into the biggest one
        int big = 0;
        for (int i = 1; i < n; ++i)
            if (ch[i].share > ch[big].share) big = i;
        float folded = 0;
        for (int i = 0; i < n; ++i)
            if (i != big && size * ch[i].share < 0.25f) folded += ch[i].share, ch[i].share = 0;
        ch[big].share += folded;
        for (int i = 0; i < n; ++i) {
            if (ch[i].share <= 0 || ch[i].route == ~0u) continue;
            Spawn(minute, from, to, home, work, type, size * ch[i].share, ch[i].route, window, key, i);
        }
        stats.released[type] += size;
        return true;
    }
    void Spawn(int64_t minute, int from, int to, int home, int work, int type, float size, uint32_t route, int window, uint64_t key, int alt, float waitOverride = -1.0f) {
        Herd h;
        h.id = nextId_++;
        h.from = from, h.to = to, h.homeHood = home, h.workHood = work;
        h.type = (uint8_t)type;
        h.alive = true;
        h.size = size;
        SplitMix64 r(SimSeed(seed, key * 131 + (uint64_t)alt, (uint64_t)window * 977 + (uint64_t)(minute / 1440)));
        h.wait = (float)(r.Below(kWindowMinutes * 100)) / 100.0f; // leaves somewhere inside the window
        if (waitOverride >= 0) h.wait = waitOverride; // a chained herd leaves at once: the drop-off took the minutes
        h.route = route;
        h.speedMean = 0.92f + 0.16f * (float)(r.Below(1000) / 1000.0);
        h.speedSd = speedSdMin + (speedSdMax - speedSdMin) * (float)(r.Below(1000) / 1000.0);
        h.departMinute = minute;
        h.distance = routes.recs[route].len;
        ++stats.herdsReleased;
        if (!free_.empty()) {
            const uint32_t slot = free_.back();
            free_.pop_back();
            herds[slot] = h;
        } else {
            herds.push_back(h);
        }
    }
    // After the school drop-off a share of the vehicles drives on to the parents' work.
    void Chain(const Herd& h) {
        const int work = (size_t)h.homeHood < topWork_.size() ? topWork_[(size_t)h.homeHood] : -1;
        if (work < 0 || work == h.to || !routeFn_) return;
        const int a = hoodNode_[(size_t)h.to], b = hoodNode_[(size_t)work];
        if (a < 0 || b < 0 || a == b) return;
        RouteChoice ch[kMaxAlternatives];
        const int n = routeFn_(a, b, ch);
        if (n <= 0) return;
        int big = 0;
        for (int i = 1; i < n; ++i)
            if (ch[i].share > ch[big].share) big = i;
        if (ch[big].route == ~0u) return;
        Spawn(h.departMinute, h.to, work, h.homeHood, work, kTripChain, h.size * chainShare, ch[big].route, 0, Key(h.homeHood, work, kTripChain) ^ h.id, 0, 0.5f);
    }
    uint32_t nextId_ = 1;
};

} // namespace city
} // namespace tbd
