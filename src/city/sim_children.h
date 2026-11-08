// Visible children and promotion (docs/city_sim_design.md section 6). Header only, headless (tests\sim_children_test.cpp).
//
// Near the camera the herds are made visible: every link within `radius` of the focus gets as many cars as the herds on it
// put there (vehicles per metre of the herd's span x densityScale), within a fixed budget. Nothing is allocated after
// SetTable: the pool, the sort order and the per-link tables are sized once.
//
//   Tenants     a child first picks a real tenant slot (a resident of the herd's home hood, cell + slot index); its look, car
//               and driver personality are a hash of (city seed, cell address, slot), never the other way round.
//   Car-follow  the Intelligent Driver Model: v0 = LinkSpeed x personality, a = aMax (1 - (v/v0)^4 - (s*/gap)^2). The leader is
//               the next car in the lane, or the first car on the next link; at a junction the stop line is the leader while
//               the light is red, and a car waits for its entry gap on green. A car behind an emergency vehicle pulls aside
//               (lateral offset) and rejoins.
//   Promotion   a child in the middle of the screen for a second (up to maxPromoted at a time) is bound to its slot: it keeps
//               its own destination, takes the next road from the destination's route tree at every junction, stops at its
//               destination and parks (a strip beside the road at the hood's centroid, until its parked time is up). Out of
//               range it is not drawn and moves on the link speed alone, so it arrives when its personality says.
//   Click       the host turns a child's slot into a named person with SimPeople::Materialize.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "road_network.h"
#include "sim.h"
#include "sim_agents.h"
#include "sim_herds.h"
#include "sim_hoods.h"

namespace tbd {
namespace city {

struct TenantSlot {
    int cell = -1;       // CitySim cell index
    uint32_t index = 0;  // resident slot inside the cell (SimPeople::Materialize's slot)
    uint64_t address = 0; // PackKey of the cell: the stable part of the hash
};

// A resident (home) or a workplace of a hood, picked in proportion to the adults who live there / jobs filled there.
inline bool PickTenantSlot(const CitySim& s, const SimHoods& hoods, int hood, bool home, uint64_t rnd, TenantSlot& out) {
    if (hood < 0 || (size_t)hood >= hoods.hoods.size()) return false;
    const Hood& H = hoods.hoods[(size_t)hood];
    uint64_t total = 0;
    for (int c : H.cells) total += home ? s.sim[(size_t)c].residents[kAdults] : (uint64_t)s.sim[(size_t)c].employed[0] + s.sim[(size_t)c].employed[1] + s.sim[(size_t)c].employed[2];
    if (!total) return false;
    uint64_t pick = (rnd >> 16) % total;
    for (int c : H.cells) {
        const CellSim& cs = s.sim[(size_t)c];
        const uint64_t n = home ? cs.residents[kAdults] : (uint64_t)cs.employed[0] + cs.employed[1] + cs.employed[2];
        if (pick < n) {
            out.cell = c, out.index = (uint32_t)pick, out.address = PackKey(s.cells.cells[(size_t)c].key);
            return true;
        }
        pick -= n;
    }
    return false;
}

// Everything the driver is, from the slot alone.
struct Look {
    uint8_t model = 0;
    uint8_t colour = 0;
    float speedFactor = 1;  // x the link speed
    float aMax = 1.5f;      // m/s^2
    float timeGap = 1.2f;   // s
};
inline Look LookFor(uint64_t citySeed, uint64_t address, uint32_t slot, int models) {
    SplitMix64 r(SimSeed(citySeed, address, (uint64_t)slot + 0xCA9));
    Look l;
    l.model = (uint8_t)r.Below((uint32_t)std::max(1, models));
    l.colour = (uint8_t)r.Below(8);
    l.speedFactor = 0.88f + 0.30f * (float)(r.Below(1000) / 1000.0);
    l.aMax = 1.2f + 0.9f * (float)(r.Below(1000) / 1000.0);
    l.timeGap = 0.9f + 0.9f * (float)(r.Below(1000) / 1000.0);
    return l;
}

struct Child {
    bool active = false;
    bool promoted = false, parked = false, hold = false; // hold: a stopped car (tests, breakdowns)
    bool emergency = false, visible = true;
    uint32_t herdId = 0;
    uint32_t route = ~0u, routeIdx = 0; // the herd's route and where on it this car is
    int link = -1;
    float s = 0, v = 0;         // metres along the link, m/s
    uint8_t lane = 0;
    float lat = 0, latTarget = 0; // sideways offset beyond the lane (+ right), metres
    TenantSlot slot;
    Look look;
    float watch = 0;            // seconds in the middle of the screen
    int destHood = -1, destNode = -1;
    float stopAt = -1;          // promoted, final link: metres at which it parks
    double parkedUntil = 0;     // game minute
    float length = 4.5f;
};

struct ChildParams {
    int maxChildren = 300;
    float radius = 600.0f;
    float densityScale = 0.3f;  // visible cars per vehicle in the herds
    float laneWidth = 3.2f;
    int models = 8;
    int maxPromoted = 32;
    float watchRadius = 45.0f;  // the middle of the screen, metres
    float watchSeconds = 1.0f;
    float parkMinutes = 300.0f;
    int spawnsPerFrame = 24;
    int despawnsPerFrame = 6;
    uint64_t citySeed = 1;
    float aComfort = 2.0f, s0 = 2.0f; // IDM: braking, minimum gap
    float signalCycle = 42.0f;  // s: two 18 s phases and 3 s all-red between
};

struct ChildStats {
    int active = 0, promoted = 0, parked = 0, visible = 0;
    uint64_t spawned = 0, despawned = 0, promotions = 0, arrivals = 0;
    uint64_t clamps = 0; // steps where the gap, not the IDM, stopped a car (should stay 0)
};

class ChildSystem {
public:
    ChildParams params;
    ChildStats stats;
    std::vector<Child> pool;

    // hooks the host (or a test) provides
    std::function<bool(const Herd&, bool home, uint64_t rnd, TenantSlot&)> tenant; // empty: slot-less children (hash of the herd)
    std::function<int(int node, int destNode)> nextHop;                           // route tree lookup: link to take from node
    std::function<int(int hood)> hoodNode;                                         // a hood's centroid node
    std::function<bool(V2 pos, double minute)> signalsOn;                          // optional: false turns lights off

    void SetTable(const LinkTable* t, const RouteStore* r) {
        table_ = t;
        routes_ = r;
        const size_t n = t ? t->links.size() : 0;
        want_.assign(n, 0.0f), span_lo_.assign(n, 0.0f), span_hi_.assign(n, 0.0f), have_.assign(n, 0), herd_.assign(n, -1), stamp_.assign(n, 0);
        centre_.assign(n, V2{}), reach_.assign(n, 0.0f);
        nearLo_.assign(n, 0.0f), nearHi_.assign(n, 0.0f), nearStamp_.assign(n, 0);
        for (size_t l = 0; l < n; ++l) {
            V2 p, d;
            float y;
            if (t->PointAt((int)l, t->links[l].len * 0.5f, p, y, d)) centre_[l] = p;
            reach_[l] = t->links[l].len * 0.5f + 5.0f;
        }
        pool.assign((size_t)params.maxChildren, Child());
        order_.assign((size_t)params.maxChildren, 0);
        touched_.clear();
        touched_.reserve(n + 16);
        scratchIdx_.clear();
        scratchIdx_.reserve((size_t)params.maxChildren);
        frame_ = 0;
        stats = ChildStats();
    }
    // Capacity of everything sized at SetTable: unchanged after warm-up means no allocation per frame.
    size_t Fingerprint() const {
        return pool.capacity() + order_.capacity() + touched_.capacity() + scratchIdx_.capacity() + want_.capacity() + have_.capacity();
    }
    int Add(const Child& c) { // tests: put a car in the pool
        for (size_t i = 0; i < pool.size(); ++i)
            if (!pool[i].active) {
                pool[i] = c;
                pool[i].active = true;
                return (int)i;
            }
        return -1;
    }

    void Update(double dt, double realDt, V2 focus, const HerdSystem& herds, double gameMinute) {
        if (!table_ || !routes_) return;
        ++frame_;
        minute_ = gameMinute;
        ComputeWanted(herds, focus);
        Reconcile(herds, focus);
        for (double left = dt; left > 1e-6;) { // sub-steps keep IDM stable when the frame is long
            const double h = std::min(left, 0.2);
            Step((float)h, herds, focus);
            left -= h;
        }
        WatchTimers((float)realDt, focus);
        Count();
    }

    // Instances for drawing: moving cars and parked ones. Only cars within the radius of the focus.
    void Collect(std::vector<CarInstance>& moving, std::vector<CarInstance>& parked, V2 focus) const {
        moving.clear();
        parked.clear();
        for (const Child& c : pool) {
            if (!c.active || !c.visible) continue;
            CarInstance ci;
            if (!Place(c, ci)) continue;
            if (Len(V2{ci.p.x, ci.p.z} - focus) > params.radius * 1.1f) continue;
            (c.parked ? parked : moving).push_back(ci);
        }
    }
    bool Place(const Child& c, CarInstance& out) const {
        V2 p, d;
        float y;
        if (c.link < 0 || !table_->PointAt(c.link, c.s, p, y, d)) return false;
        const LinkInfo& l = table_->links[(size_t)c.link];
        float off = ((float)std::min<int>(c.lane, l.lanes - 1) + 0.5f) * params.laneWidth + c.lat;
        if (c.parked) off = l.lanes * params.laneWidth + 1.4f; // the strip beside the driving lanes
        const V2 q = p + RightOf(d) * off;
        out.p = {q.x, y, q.z};
        out.dir = d;
        out.model = c.look.model % std::max(1, params.models);
        return true;
    }
    // The nearest active child to p within r metres (-1 none): the host's click and follow.
    int Nearest(V2 p, float r) const {
        int best = -1;
        float bd = r;
        for (size_t i = 0; i < pool.size(); ++i) {
            CarInstance ci;
            if (!pool[i].active || !pool[i].visible || !Place(pool[i], ci)) continue;
            const float d = Len(V2{ci.p.x, ci.p.z} - p);
            if (d < bd) bd = d, best = (int)i;
        }
        return best;
    }
    // Bind the child to its tenant slot for good (the watch timer does this by itself; the host's follow command forces it).
    bool Promote(int i) {
        if (i < 0 || (size_t)i >= pool.size() || !pool[(size_t)i].active || pool[(size_t)i].promoted || stats.promoted >= params.maxPromoted) return false;
        PromoteChild(i);
        return true;
    }

private:
    const LinkTable* table_ = nullptr;
    const RouteStore* routes_ = nullptr;
    std::vector<float> want_, span_lo_, span_hi_, reach_, nearLo_, nearHi_;
    std::vector<V2> centre_;
    std::vector<int> have_, herd_;
    std::vector<uint32_t> stamp_, nearStamp_;
    std::vector<int> touched_;
    std::vector<uint16_t> order_;
    std::vector<int> scratchIdx_;
    uint32_t frame_ = 0;
    double minute_ = 0;

    static constexpr float kLen = 4.5f;

    // ---- how many cars each link near the camera should show
    void ComputeWanted(const HerdSystem& hs, V2 focus) {
        for (int l : touched_) want_[(size_t)l] = 0.0f, herd_[(size_t)l] = -1;
        touched_.clear();
        for (size_t hi = 0; hi < hs.herds.size(); ++hi) {
            const Herd& h = hs.herds[hi];
            if (!h.alive || h.wait > 0 || h.route == ~0u) continue;
            const float onRoad = HerdSystem::OnRoad(h);
            if (onRoad < 0.5f) continue;
            float tail, head;
            hs.Span(h, tail, head);
            if (head - tail < 40.0f) { // a herd that just left (or a tight one): its cars take at least 40 m of road
                const float L = routes_->recs[h.route].len, mid = 0.5f * (head + tail);
                tail = std::max(0.0f, std::min(mid - 20.0f, L - 40.0f));
                head = std::min(L, tail + 40.0f);
            }
            const float span = std::max(head - tail, 40.0f);
            const float perM = onRoad / span;
            const RouteStore::Rec& r = routes_->recs[h.route];
            uint32_t i0;
            float into;
            routes_->Locate(*table_, h.route, tail, i0, into);
            for (uint32_t i = i0; i < r.n; ++i) {
                const int l = routes_->LinkAt(h.route, i);
                const float endM = routes_->EndAt(h.route, i), len = table_->links[(size_t)l].len, startM = endM - len;
                if (startM > head) break;
                if (Len(centre_[(size_t)l] - focus) > params.radius + reach_[(size_t)l]) continue;
                if (nearStamp_[(size_t)l] != frame_) nearStamp_[(size_t)l] = frame_, NearRange(l, focus);
                // only the stretch of the link inside the radius: a long highway pipe would otherwise put its cars km away
                const float lo = std::max(std::max(tail, startM), startM + nearLo_[(size_t)l]), hi2 = std::min(std::min(head, endM), startM + nearHi_[(size_t)l]);
                if (hi2 <= lo) continue;
                if (stamp_[(size_t)l] != frame_) { // first herd on this link this frame
                    stamp_[(size_t)l] = frame_;
                    touched_.push_back(l);
                    want_[(size_t)l] = 0.0f;
                    span_lo_[(size_t)l] = lo - startM, span_hi_[(size_t)l] = hi2 - startM;
                    herd_[(size_t)l] = (int)hi;
                }
                want_[(size_t)l] += perM * (hi2 - lo);
            }
        }
        float total = 0;
        for (int l : touched_) total += want_[(size_t)l] * params.densityScale;
        scale_ = total > (float)params.maxChildren ? (float)params.maxChildren / total : 1.0f;
        for (int l : touched_) want_[(size_t)l] *= params.densityScale * scale_;
    }
    float scale_ = 1;
    // The metres of link l within the radius of the focus (a chord around the nearest point; empty when it is all outside).
    void NearRange(int l, V2 focus) {
        float s, d;
        const float len = table_->links[(size_t)l].len;
        if (!table_->Project(l, focus, s, d)) { nearLo_[(size_t)l] = 0, nearHi_[(size_t)l] = len; return; }
        if (d >= params.radius) { nearLo_[(size_t)l] = 1, nearHi_[(size_t)l] = 0; return; }
        const float r = std::sqrt(params.radius * params.radius - d * d);
        nearLo_[(size_t)l] = std::max(0.0f, s - r), nearHi_[(size_t)l] = std::min(len, s + r);
    }

    int WantInt(int link) const {
        const float w = want_[(size_t)link];
        int n = (int)w;
        // a fractional car shows by a stable per-link coin, so the count does not flicker frame to frame
        if (SplitMix64(SimSeed(0xC41D, (uint64_t)link)).Below(1000) < (uint32_t)((w - (float)n) * 1000.0f)) ++n;
        return n;
    }

    // ---- spawn and despawn to match
    void Reconcile(const HerdSystem& hs, V2 focus) {
        std::fill(have_.begin(), have_.end(), 0);
        for (const Child& c : pool)
            if (c.active && !c.promoted && c.link >= 0) ++have_[(size_t)c.link];
        int spawns = 0, despawns = 0;
        for (int l : touched_) {
            const int want = WantInt(l), have = have_[(size_t)l];
            if (have < want) {
                for (int k = have; k < want && spawns < params.spawnsPerFrame; ++k)
                    if (Spawn(hs, l, k)) ++spawns, ++have_[(size_t)l];
            }
        }
        // links the herds have left, or that want fewer: remove the farthest cars first, never ones in the middle of the view
        for (size_t i = 0; i < pool.size() && despawns < params.despawnsPerFrame; ++i) {
            Child& c = pool[i];
            if (!c.active || c.promoted || c.link < 0) continue;
            const bool wanted = stamp_[(size_t)c.link] == frame_;
            const int want = wanted ? WantInt(c.link) : 0;
            if (have_[(size_t)c.link] <= want) continue;
            CarInstance ci;
            if (!Place(c, ci)) continue;
            const float d = Len(V2{ci.p.x, ci.p.z} - focus);
            if (d < params.radius * 0.35f) continue; // never pop a car in the middle of the view
            c.active = false;
            --have_[(size_t)c.link];
            ++despawns, ++stats.despawned;
        }
    }
    bool Spawn(const HerdSystem& hs, int link, int k) {
        const int hi = herd_[(size_t)link];
        if (hi < 0) return false;
        const Herd& h = hs.herds[(size_t)hi];
        int slot = -1;
        for (size_t i = 0; i < pool.size(); ++i)
            if (!pool[i].active) {
                slot = (int)i;
                break;
            }
        if (slot < 0) return false;
        const LinkInfo& L = table_->links[(size_t)link];
        SplitMix64 r(SimSeed(params.citySeed, (uint64_t)link * 131 + (uint64_t)k, frame_));
        Child c;
        c.link = link;
        c.lane = (uint8_t)r.Below((uint32_t)std::max<int>(1, L.lanes));
        // where: inside the herd's span on this link, with a free gap in the lane
        const float lo = span_lo_[(size_t)link], hi2 = std::max(span_hi_[(size_t)link], lo + 1.0f);
        bool placed = false;
        for (int attempt = 0; attempt < 6 && !placed; ++attempt) {
            c.s = std::clamp(lo + (hi2 - lo) * (float)(r.Below(1000) / 1000.0), 0.0f, L.len);
            placed = GapFree(link, c.lane, c.s);
        }
        if (!placed) return false;
        c.herdId = h.id;
        c.route = h.route;
        {
            // the route index of this link inside the herd's route (the link may appear once; routes have no repeats)
            const RouteStore::Rec& rr = routes_->recs[h.route];
            c.routeIdx = 0;
            for (uint32_t i = 0; i < rr.n; ++i)
                if (routes_->LinkAt(h.route, i) == link) {
                    c.routeIdx = i;
                    break;
                }
        }
        // who: a real tenant slot of the hood the herd's people live in, or a hash of the herd when there is no sim behind it
        const bool home = true;
        const int tenantHood = h.homeHood;
        (void)tenantHood;
        if (!tenant || !tenant(h, home, r.Next(), c.slot)) {
            c.slot.cell = -1;
            c.slot.index = (uint32_t)(r.Next() & 0xFFFF);
            c.slot.address = SimSeed(params.citySeed, h.id, (uint64_t)h.homeHood);
        }
        c.look = LookFor(params.citySeed, c.slot.address, c.slot.index, params.models);
        c.v = std::min(hs.LinkSpeed(link) / 60.0f * c.look.speedFactor, 25.0f) * 0.9f;
        c.destHood = h.to;
        c.visible = true;
        c.active = true;
        pool[(size_t)slot] = c;
        ++stats.spawned;
        return true;
    }
    bool GapFree(int link, int lane, float s) const {
        for (const Child& o : pool)
            if (o.active && o.link == link && o.lane == lane && std::fabs(o.s - s) < 11.0f) return false;
        return true;
    }

    // ---- junction signals: two phases on a fixed cycle, offset per node; green for the axis whose phase it is
    bool Green(int node, V2 heading, V2 where) const {
        if (signalsOn && !signalsOn(where, minute_)) return true;
        const float cyc = params.signalCycle;
        const float phase = std::fmod((float)(minute_ * 60.0) + (float)(SplitMix64(SimSeed(0x516, (uint64_t)node)).Below(1000) / 1000.0) * cyc, cyc);
        const bool ewAxis = std::fabs(heading.x) >= std::fabs(heading.z);
        if (phase < 18.0f) return ewAxis;
        if (phase >= 21.0f && phase < 39.0f) return !ewAxis;
        return false; // all-red
    }
    bool Junction(int node) const { return node >= 0 && node < table_->nodes && table_->outOff[(size_t)node + 1] - table_->outOff[(size_t)node] >= 3; }

    int NextLink(const Child& c) const {
        const LinkInfo& L = table_->links[(size_t)c.link];
        if (c.promoted) return nextHop && c.destNode >= 0 && L.to != c.destNode ? nextHop(L.to, c.destNode) : -1;
        if (c.route == ~0u) return -1;
        return c.routeIdx + 1 < routes_->recs[c.route].n ? routes_->LinkAt(c.route, c.routeIdx + 1) : -1;
    }

    // ---- one sub-step: sort, IDM, move, hand over at link ends
    void Step(float dt, const HerdSystem& hs, V2 focus) {
        int n = 0;
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].active) order_[(size_t)n++] = (uint16_t)i;
        std::sort(order_.begin(), order_.begin() + n, [&](uint16_t a, uint16_t b) {
            const Child &x = pool[a], &y = pool[b];
            if (x.link != y.link) return x.link < y.link;
            if (x.lane != y.lane) return x.lane < y.lane;
            return x.s < y.s;
        });
        for (int k = 0; k < n; ++k) {
            Child& c = pool[order_[(size_t)k]];
            const LinkInfo& L = table_->links[(size_t)c.link];
            CarInstance ci;
            const bool placed = Place(c, ci);
            const float dist = placed ? Len(V2{ci.p.x, ci.p.z} - focus) : 1e9f;
            c.visible = dist <= params.radius * 1.2f;
            if (c.parked) {
                if (minute_ >= c.parkedUntil) c.active = false, --stats.promoted, ++stats.despawned;
                continue;
            }
            // out of range: a promoted car is not drawn and moves on the link speed alone
            if (c.promoted && !c.visible) {
                c.v = hs.LinkSpeed(c.link) / 60.0f * c.look.speedFactor;
                c.s += c.v * dt;
                if (L.to == c.destNode && NextLink(c) < 0) { // its destination: parked out of sight, there when the camera comes
                    const float stop = StopAt(c);
                    if (c.s >= stop) c.s = stop, c.v = 0, c.parked = true, c.parkedUntil = minute_ + params.parkMinutes, ++stats.arrivals;
                    continue;
                }
                Handover(c, hs);
                continue;
            }
            const float v0 = std::max(0.5f, hs.LinkSpeed(c.link) / 60.0f * c.look.speedFactor);
            // the nearest thing ahead: gap (bumper to bumper) and its speed
            float gap = 1e9f, lv = c.v;
            bool pulledAside = false;
            for (int j = k + 1; j < n; ++j) {
                const Child& o = pool[order_[(size_t)j]];
                if (o.link != c.link || o.lane != c.lane) break;
                if (c.emergency && o.lat > 1.2f) continue; // it pulled over: pass it
                gap = o.s - c.s - kLen, lv = o.hold ? 0.0f : o.v;
                break;
            }
            // any emergency vehicle close behind in this lane: pull aside
            for (int j = k - 1; j >= 0 && !c.emergency; --j) {
                const Child& o = pool[order_[(size_t)j]];
                if (o.link != c.link || o.lane != c.lane) break;
                if (c.s - o.s > 80.0f) break;
                if (o.emergency) pulledAside = true;
            }
            c.latTarget = pulledAside ? 1.9f : 0.0f;
            c.lat += std::clamp(c.latTarget - c.lat, -1.5f * dt, 1.5f * dt);
            if (gap >= 1e8f) { // nobody ahead on the link: the first car on the next one, a stop line, or a parking spot
                const int nl = NextLink(c);
                const float toEnd = L.len - c.s;
                if (c.promoted && nl < 0 && L.to == c.destNode) { // final link: park
                    StopAt(c);
                    if (c.stopAt - c.s < 5.0f && c.v < 0.4f) {
                        c.parked = true, c.v = 0, c.parkedUntil = minute_ + params.parkMinutes, ++stats.arrivals;
                        continue;
                    }
                    gap = c.stopAt - c.s - 1.0f, lv = 0;
                } else if (nl >= 0 && Junction(L.to)) {
                    V2 p, d;
                    float y;
                    table_->PointAt(c.link, L.len, p, y, d);
                    if (!Green(L.to, d, p)) gap = toEnd - 2.0f, lv = 0; // red: the stop line is the leader
                    else if (toEnd < 40.0f) { // green: wait for the entry gap of the next link
                        const float entry = EntryGap(nl, c.lane);
                        if (entry < 12.0f) gap = toEnd + entry - kLen, lv = 0;
                    }
                } else if (nl >= 0 && toEnd < 60.0f) { // follow the first car of the next link
                    int best = -1;
                    for (int j = 0; j < n; ++j) {
                        const Child& o = pool[order_[(size_t)j]];
                        if (o.link == nl && (best < 0 || o.s < pool[order_[(size_t)best]].s) && o.lane == std::min<int>(c.lane, table_->links[(size_t)nl].lanes - 1)) best = j;
                    }
                    if (best >= 0) {
                        const Child& o = pool[order_[(size_t)best]];
                        gap = toEnd + o.s - kLen, lv = o.hold ? 0.0f : o.v;
                    }
                }
            }
            // IDM
            const float dv = c.v - lv;
            const float sStar = params.s0 + std::max(0.0f, c.v * c.look.timeGap + c.v * dv / (2.0f * std::sqrt(c.look.aMax * params.aComfort)));
            const float g = std::max(0.1f, gap);
            const float free = 1.0f - std::pow(c.v / v0, 4.0f);
            float acc = c.look.aMax * (free - (sStar / g) * (sStar / g));
            if (c.hold) acc = 0, c.v = 0;
            acc = std::max(acc, -9.0f);
            c.v = std::max(0.0f, c.v + acc * dt);
            float ds = c.v * dt;
            if (gap < 1e8f && ds > std::max(0.0f, gap - 0.5f)) ds = std::max(0.0f, gap - 0.5f), ++stats.clamps; // never into the car ahead
            c.s += c.hold ? 0.0f : ds;
            Handover(c, hs);
        }
    }
    // Where on its last link a promoted car parks (a stable spot from its slot).
    float StopAt(Child& c) const {
        if (c.stopAt < 0) c.stopAt = std::max(6.0f, table_->links[(size_t)c.link].len - 10.0f - (float)(SplitMix64(SimSeed(0x9A2, c.slot.address, c.slot.index)).Below(30)));
        return c.stopAt;
    }
    float EntryGap(int link, int lane) const {
        float best = 1e9f;
        const int ln = std::min<int>(lane, table_->links[(size_t)link].lanes - 1);
        for (const Child& o : pool)
            if (o.active && o.link == link && o.lane == ln) best = std::min(best, o.s - kLen);
        return best;
    }
    // Past the end of the link: on to the next one, or gone.
    void Handover(Child& c, const HerdSystem&) {
        for (int guard = 0; guard < 4 && c.active && c.link >= 0 && c.s >= table_->links[(size_t)c.link].len; ++guard) {
            const float len = table_->links[(size_t)c.link].len;
            const int nl = NextLink(c);
            if (nl < 0) {
                c.active = false;
                ++stats.despawned;
                if (c.promoted) --stats.promoted;
                return;
            }
            c.s -= len;
            c.link = nl;
            ++c.routeIdx;
            c.lane = (uint8_t)std::min<int>(c.lane, table_->links[(size_t)nl].lanes - 1);
        }
    }

    // ---- the watch timer
    void WatchTimers(float realDt, V2 focus) {
        for (size_t i = 0; i < pool.size(); ++i) {
            Child& c = pool[i];
            if (!c.active || c.promoted || !c.visible) continue;
            CarInstance ci;
            if (!Place(c, ci)) continue;
            const float d = Len(V2{ci.p.x, ci.p.z} - focus);
            if (d <= params.watchRadius) c.watch += realDt;
            else c.watch = std::max(0.0f, c.watch - 2.0f * realDt);
            if (c.watch >= params.watchSeconds && stats.promoted < params.maxPromoted) PromoteChild((int)i);
        }
    }
    void PromoteChild(int i) {
        Child& c = pool[(size_t)i];
        c.promoted = true;
        if (c.route != ~0u && routes_->recs[c.route].n) c.destNode = table_->links[(size_t)routes_->LinkAt(c.route, routes_->recs[c.route].n - 1)].to;
        ++stats.promotions, ++stats.promoted;
    }
    void Count() {
        stats.active = stats.parked = stats.visible = 0;
        for (const Child& c : pool) {
            if (!c.active) continue;
            ++stats.active;
            stats.parked += c.parked;
            stats.visible += c.visible;
        }
    }
};

} // namespace city
} // namespace tbd
