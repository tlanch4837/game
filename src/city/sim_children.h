// Visible cars (docs/city_sim_design.md section 6). Header only, headless (tests\sim_children_test.cpp,
// tests\sim_cars_consistency_test.cpp).
//
// The herds (sim_herds.h) move on game time, about 120x faster than a car on screen, so the cars the player sees are not the
// herds themselves: every route the herds use becomes a steady STREAM of trips on the cars' own clock (tau, the seconds the
// cars have driven). A stream's rate is the vehicles the herds released on that route per game second (x densityScale), so
// the cars on a road are its flow divided by its speed, the same density the herds and the flow solve give it.
//
//   Trips      trip j of route r leaves when the stream's cumulative count N(tau) passes j + jitter. N is kept at fixed
//              sample times (a short ring per stream), so a trip's departure never moves once it is on the road. Where a trip
//              is at time tau is a pure function of (r, j, tau): free-flow time along the route x the trip's speed factor.
//              It starts at a driveway on the route's first link (or at the map edge, for an outside connection) and ends at
//              a driveway on its last link (or the map edge): never at a junction, never mid-road out of nothing.
//   Lanes      right-hand traffic. A trip picks its lane for each link by the turn at the link's end (right: the curb lane,
//              left: the inner lane, through: it keeps its lane) and changes lane only through a junction's turn path.
//   Junctions  every link join is a turn path: a cubic from the lane end on the way in to the lane start on the way out, so a
//              car's position is continuous through the node. A junction (3+ roads) has a stop line; a car enters only when
//              its path is free of conflicting cars in the box and no car with priority is coming (the bigger road wins, then
//              who is nearer, then a fixed order), and there is room on the way out. 4+ roads with two multi-lane approaches
//              have a fixed two-phase signal; left turns on green yield to oncoming traffic.
//   Detail     near the camera trips are MATERIALISED as cars: the Intelligent Driver Model (car following, standstill gap,
//              no overlap), speed by road class, curvature and turns, stop lines. A car is materialised exactly where its
//              trip is, and only (a) beyond the view radius (in the band out to the detail radius: never seen appearing),
//              (b) at its driveway or the map edge when it departs (fading in), or (c) with a fade when a place first comes
//              into view (zoom, a jump of the camera, the first frame of a new road network). Past the detail radius a car is
//              dropped and goes back to being its trip; how far it was behind its trip (queues) is remembered, so a car you
//              look at again is where it should be.
//   Budget     the trips near the camera are thinned by a stable per-trip rank to the visible budget (maxChildren); a car
//              once shown is never thinned. No allocation per frame once the pool, the streams and the tables are sized.
//   Click      the host turns a car's tenant slot into a named person with SimPeople::Materialize; its herd id names the trip.
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

enum TurnKind : int8_t { kTurnThrough = 0, kTurnRight = 1, kTurnLeft = 2, kTurnU = 3 };

constexpr float kNoSpot = -3.0f;   // Child::startAt / stopAt: none (a test car: it simply leaves at the link's end)
constexpr float kEdgeSpot = -1.0f; // the map edge (an outside connection): fades in / out over the first / last metres
constexpr uint32_t kNoTrip = 0xFFFFFFFFu; // Child::trip of a car that is no stream's trip (tests)

struct Child {
    bool active = false;
    bool promoted = false;  // followed by the camera: never dropped by distance
    bool parked = false;    // turning into its driveway (the info card says "parked")
    bool hold = false;      // a stopped car (tests, breakdowns)
    bool emergency = false, visible = true;
    bool committed = false; // has the junction ahead (or is in its box)
    uint32_t herdId = 0;    // the latest herd on its route: names the trip for the info card
    uint32_t route = ~0u, routeIdx = 0; // its route (RouteStore id) and where on it this car is
    uint32_t trip = kNoTrip; // trip number in the route's stream
    uint32_t serial = 0;    // one per materialisation: the car's identity across frames (CarInstance::id)
    uint32_t laneHash = 0;
    int link = -1, prevLink = -1; // prevLink: the link it came from while it is still in the junction's turn path
    float s = 0, v = 0;     // metres along the link, m/s
    uint8_t lane = 0, prevLane = 0, nextLane = 0;
    float lat = 0, latTarget = 0; // sideways offset beyond the lane (+ right), metres: pulling over for a siren
    TenantSlot slot;
    Look look;
    int destHood = -1, fromHood = -1, destNode = -1;
    uint8_t tripType = kTripCommute;
    float startAt = kNoSpot; // metres on the first link of its driveway (kEdgeSpot: the map edge)
    float stopAt = kNoSpot;  // metres on the last link of its driveway (kEdgeSpot: it drives off the map)
    float thO = 0;           // free-flow seconds of its origin along the route
    double depart = 0;       // tau its trip left at
    float fade = 1;          // time fade-in (a car materialised in view)
    float wait = 0;          // seconds stopped at a stop line
    float length = 4.5f;
    float watch = 0;         // kept for the host's stats
    double parkedUntil = 0;
};

struct ChildParams {
    int maxChildren = 300;
    float radius = 600.0f;      // the view: cars are drawn inside it, shrinking away over its last tenth
    float bandScale = 1.3f, bandExtra = 80.0f; // detail radius = radius x bandScale + bandExtra: cars appear and go in the band
    float densityScale = 0.3f;  // visible cars per vehicle in the herds
    float laneWidth = 3.2f;
    int models = 8;
    int maxPromoted = 32;
    float watchRadius = 45.0f;  // (kept for the host)
    float watchSeconds = 1.0f;
    float parkMinutes = 300.0f;
    int spawnsPerFrame = 32;
    int despawnsPerFrame = 6;
    uint64_t citySeed = 1;
    float aComfort = 2.0f, s0 = 2.0f; // IDM: braking, minimum gap
    float signalCycle = 42.0f;  // s: two 18 s phases and 3 s all-red between
    float sampleSeconds = 4.0f; // stream counts are kept every this many seconds of tau
    float fadeSeconds = 0.6f;
};

struct ChildStats {
    int active = 0, promoted = 0, parked = 0, visible = 0, streams = 0;
    uint64_t spawned = 0, despawned = 0, promotions = 0, arrivals = 0;
    uint64_t departures = 0, dropped = 0, fadedIn = 0, skipped = 0, waits = 0, forced = 0;
    uint64_t clamps = 0; // steps where the gap, not the IDM, stopped a car (should stay 0)
};

class ChildSystem {
public:
    ChildParams params;
    ChildStats stats;
    std::vector<Child> pool;

    // hooks the host (or a test) provides
    std::function<bool(const Herd&, bool home, uint64_t rnd, TenantSlot&)> tenant; // empty: slot-less children (hash of the trip)
    std::function<int(int node, int destNode)> nextHop;                           // (kept: the routes are the trips' own)
    std::function<int(int hood)> hoodNode;                                         // (kept)
    std::function<bool(V2 pos, double minute)> signalsOn;                          // optional: false turns lights off

    void SetTable(const LinkTable* t, const RouteStore* r) {
        // the cars shown last are kept a moment as ghosts and shrink away: a road edit never pops them
        ghosts_.assign(lastDrawn_.begin(), lastDrawn_.end());
        ghostAlpha_ = ghosts_.empty() ? 0.0f : 1.0f;
        table_ = t;
        routes_ = r;
        BuildGeometry();
        pool.assign((size_t)params.maxChildren, Child());
        order_.assign((size_t)params.maxChildren, 0);
        entries_.clear(), entries_.reserve((size_t)params.maxChildren);
        entryOf_.assign((size_t)params.maxChildren, -1);
        posCache_.assign((size_t)params.maxChildren, V2{});
        crossers_.clear(), crossers_.reserve(64);
        lastDrawn_.clear(), lastDrawn_.reserve((size_t)params.maxChildren);
        first_.assign(kFirstSlots, FirstE{});
        live_.assign(kLiveSlots, LiveE{});
        delay_.assign(kDelaySlots, DelayE{});
        turn_.clear(), cumT_.clear(), routeDone_ = 0;
        slotOf_.clear(), streams_.clear(), ring_.clear(), csrOff_.clear(), csrItem_.clear();
        slotOf_.reserve(4096), streams_.reserve(1024), ring_.reserve((size_t)1024 * kRing), csrItem_.reserve(8192);
        turn_.reserve(16384), cumT_.reserve(16384);
        const size_t L = t ? t->links.size() : 0;
        linkRate_.assign(L, 0.0f);
        csrOff_.assign(L + 1, 0);
        near_.clear(), near_.reserve(L);
        tau_ = 0, lastK_ = -1, lastGameMinute_ = -1;
        freshUntil_ = 1e300;
        budget_ = 1, budgetSet_ = false;
        prevRv_ = 0, prevFocus_ = V2{};
        serial_ = 0, frame_ = 0;
        stats = ChildStats();
    }
    // Capacity of everything sized at SetTable: unchanged after warm-up means no allocation per frame.
    size_t Fingerprint() const {
        return pool.capacity() + order_.capacity() + entries_.capacity() + entryOf_.capacity() + posCache_.capacity() + lastDrawn_.capacity() + near_.capacity() +
               streams_.capacity() + ring_.capacity() + csrItem_.capacity() + csrOff_.capacity() + first_.capacity() + live_.capacity() + delay_.capacity();
    }
    int Add(const Child& c) { // tests: put a car in the pool (route ~0u: it drives its link and leaves at the end)
        for (size_t i = 0; i < pool.size(); ++i)
            if (!pool[i].active) {
                pool[i] = c;
                pool[i].active = true;
                if (pool[i].serial == 0) pool[i].serial = ++serial_;
                return (int)i;
            }
        return -1;
    }
    // Tests: a car on route r at link index idx, s metres in, with the route's lane plan (it drives to the route's end).
    int AddOnRoute(uint32_t r, uint32_t idx, float s, float v, bool promoted = true) {
        if (!routes_ || r >= routes_->recs.size() || idx >= routes_->recs[r].n) return -1;
        EnsureRoutes();
        Child c;
        c.route = r, c.routeIdx = idx, c.link = routes_->LinkAt(r, idx), c.s = s, c.v = v, c.promoted = promoted, c.trip = kNoTrip;
        c.startAt = kNoSpot, c.stopAt = kEdgeSpot;
        c.laneHash = (uint32_t)SimSeed(params.citySeed, r, idx);
        LanePlan(r, idx, c.laneHash, false, c.prevLane, c.lane, c.nextLane);
        c.look.speedFactor = 1;
        if (idx > 0 && s < RS(c.link)) c.prevLink = routes_->LinkAt(r, idx - 1), c.committed = Junction(table_->links[(size_t)c.link].from);
        return Add(c);
    }

    void Update(double dt, double realDt, V2 focus, const HerdSystem& herds, double gameMinute) {
        if (!table_ || !routes_) return;
        ++frame_;
        EnsureRoutes();
        tau_ += std::max(0.0, dt);
        ghostAlpha_ = std::max(0.0f, ghostAlpha_ - (float)realDt / 0.5f);
        if (ghostAlpha_ <= 0.0f) ghosts_.clear();
        while ((double)(lastK_ + 1) * params.sampleSeconds <= tau_ + 1e-9) Sample(herds, gameMinute);
        for (double left = dt; left > 1e-6;) { // sub-steps keep IDM stable when the frame is long
            const double h = std::min(left, 0.2);
            Step((float)h, herds);
            left -= h;
        }
        const float fadeStep = (float)realDt / std::max(0.05f, params.fadeSeconds);
        for (Child& c : pool)
            if (c.active && c.fade < 1.0f) c.fade = std::min(1.0f, c.fade + fadeStep);
        Drop(focus);
        Spawn(focus);
        prevFocus_ = focus, prevRv_ = params.radius;
        Count();
    }

    // Instances for drawing: the cars within the view radius (shrinking over its last tenth), plus the ghosts of a road
    // edit. `parked` gets nothing from here (curbside cars are sim_town_traffic.h's); it is cleared for the host.
    void Collect(std::vector<CarInstance>& moving, std::vector<CarInstance>& parked, V2 focus) {
        moving.clear();
        parked.clear();
        lastDrawn_.clear();
        const float R = params.radius;
        for (const Child& c : pool) {
            if (!c.active) continue;
            CarInstance ci;
            if (!Place(c, ci)) continue;
            const float d = Len(V2{ci.p.x, ci.p.z} - focus);
            if (d > R) continue;
            ci.scale *= EdgeFade(d);
            if (ci.scale <= 0.01f) continue;
            moving.push_back(ci);
            lastDrawn_.push_back(ci);
        }
        for (const CarInstance& g : ghosts_) {
            CarInstance ci = g;
            ci.scale *= ghostAlpha_;
            if (ci.scale > 0.01f) moving.push_back(ci);
        }
    }
    float EdgeFade(float d) const { return std::clamp((params.radius - d) / std::max(1.0f, 0.1f * params.radius), 0.0f, 1.0f); }
    float DetailRadius() const { return params.radius * params.bandScale + params.bandExtra; }

    bool Place(const Child& c, CarInstance& out) const {
        V2 p, d;
        float y, a;
        if (!PoseOf(c, p, y, d, a)) return false;
        out.p = {p.x, y, p.z};
        out.dir = d;
        out.model = c.look.model % std::max(1, params.models);
        out.scale = a;
        out.id = c.serial;
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
    // Keep the car whatever the distance (the host's follow camera rides with it).
    bool Promote(int i) {
        if (i < 0 || (size_t)i >= pool.size() || !pool[(size_t)i].active || pool[(size_t)i].promoted || stats.promoted >= params.maxPromoted) return false;
        pool[(size_t)i].promoted = true;
        ++stats.promotions, ++stats.promoted;
        return true;
    }

    // ---- inspection (tests, the host's stats)
    double Time() const { return tau_; }
    float BudgetScale() const { return budget_; }
    bool HasTraffic() const {
        for (const Stream& s : streams_)
            if (s.lambda > 0) return true;
        return false;
    }
    float StreamRate(uint32_t route) const { return route < slotOf_.size() && slotOf_[route] >= 0 ? streams_[(size_t)slotOf_[route]].lambda : 0.0f; }
    float LinkRate(int l) const { return (size_t)l < linkRate_.size() ? linkRate_[(size_t)l] : 0.0f; } // trips per second through l
    float FreeSpeed(int l) const { return vFree_[(size_t)l]; }
    float JunctionRadius(int n) const { return (size_t)n < nodeR_.size() ? nodeR_[(size_t)n] : 0.0f; }
    bool Junction(int n) const { return n >= 0 && (size_t)n < deg_.size() && deg_[(size_t)n] >= 3; }
    bool Signalised(int n) const { return n >= 0 && (size_t)n < signal_.size() && signal_[(size_t)n] != 0; }
    int Degree(int n) const { return (size_t)n < deg_.size() ? deg_[(size_t)n] : 0; }
    // Where trip j of route r is now by its trip alone (false: not on the road). Tests compare cars with their trips.
    bool TripPose(uint32_t r, uint32_t j, V2& p) const {
        Child c;
        if (!TripChild(r, j, -1, c)) return false;
        V2 d;
        float y, a;
        return PoseOf(c, p, y, d, a);
    }
    // Lane offset of a lane of link l from the polyline (+ right), and the driveway offset on its curb side.
    float LaneOff(int l, int lane) const { return base_[(size_t)l] + ((float)lane + 0.5f) * params.laneWidth; }
    float DriveOff(int l) const { return base_[(size_t)l] + (float)table_->links[(size_t)l].lanes * params.laneWidth + 5.0f; }

private:
    const LinkTable* table_ = nullptr;
    const RouteStore* routes_ = nullptr;

    // ---- geometry, built at SetTable
    std::vector<float> ptS_;       // per polyline point: metres from the link start (polyline metres)
    std::vector<V2> ptT_;          // per point: smoothed tangent
    std::vector<float> polyScale_; // per link: polyline metres per link metre
    std::vector<float> base_, vFree_, vCurve_, rank_, reach_;
    std::vector<V2> centre_;
    std::vector<float> nodeR_;
    std::vector<uint8_t> deg_, signal_;
    std::vector<V2> axis_;
    std::vector<float> phase_;
    std::vector<V2> startDir_, endDir_;

    // ---- per route (parallel to RouteStore::links), built lazily
    std::vector<int8_t> turn_;     // the turn at the end of each link of the route (the last: through)
    std::vector<float> cumT_;      // free-flow seconds from the route start to the end of each link
    size_t routeDone_ = 0;

    // ---- streams
    static constexpr int kRing = 96;
    struct Stream {
        uint32_t route = 0;
        float lambda = 0;          // trips per second of tau, constant between samples
        double N = 0;              // cumulative trips at the last sample
        double released = 0;       // the herds' released count at the last sample
        double lastActive = -1e300;
        int64_t createdK = 0;
        bool fresh = false;        // made with the first traffic of a new network: its trips are already on the road
        HerdSystem::RouteLast who;
    };
    std::vector<int> slotOf_;      // route -> stream slot
    std::vector<Stream> streams_;
    std::vector<float> ring_;      // slot * kRing + k % kRing: N at sample k
    std::vector<float> linkRate_;
    std::vector<int> csrOff_;
    struct Item {
        int slot;
        uint32_t idx;
    };
    std::vector<Item> csrItem_;
    double tau_ = 0;
    int64_t lastK_ = -1;
    double lastGameMinute_ = -1;
    double freshUntil_ = 1e300;

    // ---- per frame
    struct NearE {
        int link;
        float lo, hi;   // link metres inside the detail radius
        float ilo, ihi; // ... and inside the view (ilo >= ihi: none)
    };
    std::vector<NearE> near_;
    float budget_ = 1;
    bool budgetSet_ = false;
    V2 prevFocus_{};
    float prevRv_ = 0;
    uint32_t serial_ = 0, frame_ = 0;
    std::vector<uint16_t> order_;
    std::vector<V2> posCache_;
    std::vector<CarInstance> lastDrawn_, ghosts_;
    float ghostAlpha_ = 0;

    struct Entry {
        int node = -1;
        int car = -1;
        int inLink = -1, outLink = -1;
        uint8_t inLane = 0, outLane = 0;
        int8_t turn = kTurnThrough;
        bool occ = false;   // in the box (committed)
        float tLine = 0;    // seconds to the stop line
        float rank = 0;
        V2 inDir{};
    };
    std::vector<Entry> entries_;
    std::vector<int> entryOf_;

    static constexpr size_t kFirstSlots = 2048, kLiveSlots = 2048, kDelaySlots = 512;
    struct FirstE {
        uint64_t key = 0;
        uint32_t stamp = 0;
        int k = -1;
    };
    std::vector<FirstE> first_;   // (link, lane) -> sorted index of its first car, this sub-step
    uint32_t firstStamp_ = 0;
    struct LiveE {
        uint64_t key = 0;
        uint32_t stamp = 0;
    };
    std::vector<LiveE> live_;     // trip keys of the cars in the pool, this frame
    struct DelayE {
        uint64_t key = 0;
        float delay = 0;
        double when = -1;
    };
    std::vector<DelayE> delay_;   // trips dropped behind (or ahead of) their schedule

    static uint64_t Mix(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        return k;
    }
    static uint64_t TripKey(uint32_t r, uint32_t j) { return ((uint64_t)r << 32 | j) + 1; }
    static float U01(SplitMix64& r) { return (float)((r.Next() >> 40) * (1.0 / 16777216.0)); }
    static float Smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
    static float SmoothD(float x) { return x <= 0.0f || x >= 1.0f ? 0.0f : 6.0f * x * (1.0f - x); }

    float Len_(int l) const { return table_->links[(size_t)l].len; }
    float RE(int l) const { return std::min(nodeR_[(size_t)table_->links[(size_t)l].to], 0.4f * Len_(l)); }
    float RS(int l) const { return std::min(nodeR_[(size_t)table_->links[(size_t)l].from], 0.4f * Len_(l)); }
    uint32_t RouteN(uint32_t r) const { return routes_->recs[r].n; }
    int RouteLink(uint32_t r, uint32_t i) const { return routes_->LinkAt(r, i); }
    float CumT(uint32_t r, uint32_t i) const { return cumT_[routes_->recs[r].off + i]; }
    int8_t TurnAt(uint32_t r, uint32_t i) const { return turn_[routes_->recs[r].off + i]; }

    // ---------------------------------------------------------------------------------------------- geometry
    void BuildGeometry() {
        const size_t L = table_ ? table_->links.size() : 0;
        const size_t P = table_ ? table_->pts.size() / 3 : 0;
        const int N = table_ ? table_->nodes : 0;
        ptS_.assign(P, 0.0f), ptT_.assign(P, V2{1, 0});
        polyScale_.assign(L, 1.0f), base_.assign(L, 0.0f), vFree_.assign(L, 10.0f), vCurve_.assign(L, 60.0f), rank_.assign(L, 0.0f), reach_.assign(L, 0.0f);
        centre_.assign(L, V2{}), startDir_.assign(L, V2{1, 0}), endDir_.assign(L, V2{1, 0});
        nodeR_.assign((size_t)std::max(0, N), 0.0f), deg_.assign((size_t)std::max(0, N), 0), signal_.assign((size_t)std::max(0, N), 0);
        axis_.assign((size_t)std::max(0, N), V2{1, 0}), phase_.assign((size_t)std::max(0, N), 0.0f);
        if (!table_) return;
        const LinkTable& t = *table_;
        auto P3 = [&](uint32_t i) { return V2{t.pts[(size_t)i * 3], t.pts[(size_t)i * 3 + 2]}; };
        for (size_t l = 0; l < L; ++l) {
            const LinkInfo& li = t.links[l];
            vFree_[l] = std::max(1.0f, li.freeMpm / 60.0f);
            rank_[l] = li.cap;
            base_[l] = t.LaneBase((int)l, params.laneWidth);
            if (l + 1 >= t.ptOff.size()) continue;
            const uint32_t a = t.ptOff[l], b = t.ptOff[l + 1];
            if (b < a + 2) continue;
            float acc = 0;
            for (uint32_t i = a; i < b; ++i) {
                ptS_[i] = acc;
                if (i + 1 < b) acc += Len(P3(i + 1) - P3(i));
            }
            polyScale_[l] = acc > 1e-3f ? acc / std::max(1e-3f, li.len) : 1.0f;
            float minR = 1e9f;
            for (uint32_t i = a; i < b; ++i) {
                const bool hp = i > a, hn = i + 1 < b;
                const V2 dp = hp ? Norm(P3(i) - P3(i - 1)) : V2{}, dn = hn ? Norm(P3(i + 1) - P3(i)) : V2{};
                ptT_[i] = hp && hn ? Norm(dp + dn) : hp ? dp : dn;
                if (hp && hn) {
                    const float ang = std::acos(std::clamp(Dot2(dp, dn), -1.0f, 1.0f));
                    const float segs = 0.5f * (Len(P3(i) - P3(i - 1)) + Len(P3(i + 1) - P3(i)));
                    if (ang > 1e-3f) minR = std::min(minR, segs / ang);
                }
            }
            vCurve_[l] = std::clamp(std::sqrt(2.5f * minR), 6.0f, 60.0f);
            startDir_[l] = ptT_[a], endDir_[l] = ptT_[b - 1];
            V2 p, d;
            float y;
            if (t.PointAt((int)l, li.len * 0.5f, p, y, d)) centre_[l] = p;
            reach_[l] = li.len * 0.5f + 10.0f;
            for (uint32_t i = a; i < b; ++i) reach_[l] = std::max(reach_[l], Len(P3(i) - centre_[l]) + 5.0f);
        }
        // nodes: how many roads meet (distinct neighbours), the turn-path radius, signals
        for (int n = 0; n < N; ++n) {
            if ((size_t)n + 1 >= t.outOff.size() || (size_t)n + 1 >= t.inOff.size()) continue;
            int nb[24];
            int k = 0;
            float w = 0;
            int multiIn = 0;
            auto add = [&](int other) {
                for (int i = 0; i < k; ++i)
                    if (nb[i] == other) return;
                if (k < 24) nb[k++] = other;
            };
            for (int q = t.outOff[(size_t)n]; q < t.outOff[(size_t)n + 1]; ++q) {
                const int l = t.outIdx[(size_t)q];
                add(t.links[(size_t)l].to);
                const int rv = t.Reverse(l);
                w = std::max(w, ((float)t.links[(size_t)l].lanes + (rv >= 0 ? (float)t.links[(size_t)rv].lanes : 0.0f)) * params.laneWidth * 0.5f);
            }
            for (int q = t.inOff[(size_t)n]; q < t.inOff[(size_t)n + 1]; ++q) {
                const int l = t.inIdx[(size_t)q];
                add(t.links[(size_t)l].from);
                const int rv = t.Reverse(l);
                w = std::max(w, ((float)t.links[(size_t)l].lanes + (rv >= 0 ? (float)t.links[(size_t)rv].lanes : 0.0f)) * params.laneWidth * 0.5f);
                multiIn += t.links[(size_t)l].lanes >= 2;
            }
            deg_[(size_t)n] = (uint8_t)k;
            nodeR_[(size_t)n] = k >= 3 ? std::clamp(w + 2.5f, 5.0f, 14.0f) : k == 2 ? 3.0f : 0.0f;
            signal_[(size_t)n] = (uint8_t)(k >= 4 && multiIn >= 2);
            if (t.inOff[(size_t)n + 1] > t.inOff[(size_t)n]) axis_[(size_t)n] = endDir_[(size_t)t.inIdx[(size_t)t.inOff[(size_t)n]]];
            phase_[(size_t)n] = (float)(SplitMix64(SimSeed(0x516, (uint64_t)n)).Below(1000) / 1000.0) * params.signalCycle;
        }
    }
    // Point, height and tangent `s` link metres along link l, `off` metres to the right of its polyline (smoothed normals,
    // so a lane is continuous over the polyline's corners).
    bool LanePose(int l, float s, float off, V2& p, float& y, V2& tan) const {
        const LinkTable& t = *table_;
        if ((size_t)l + 1 >= t.ptOff.size()) return false;
        const uint32_t a = t.ptOff[(size_t)l], b = t.ptOff[(size_t)l + 1];
        if (b < a + 2) return false;
        const float sp = std::clamp(s, 0.0f, Len_(l)) * polyScale_[(size_t)l];
        const float* lo = ptS_.data() + a + 1;
        const float* hi = ptS_.data() + b - 1;
        const uint32_t i = a + (uint32_t)(std::upper_bound(lo, hi, sp) - lo); // first point past sp, minus one
        const uint32_t j = i + 1;
        const float seg = ptS_[j] - ptS_[i];
        const float u = seg > 1e-4f ? std::clamp((sp - ptS_[i]) / seg, 0.0f, 1.0f) : 0.0f;
        const V2 p0{t.pts[(size_t)i * 3], t.pts[(size_t)i * 3 + 2]}, p1{t.pts[(size_t)j * 3], t.pts[(size_t)j * 3 + 2]};
        const V2 n0 = RightOf(ptT_[i]), n1 = RightOf(ptT_[j]);
        p = p0 + (p1 - p0) * u + (n0 + (n1 - n0) * u) * off;
        y = t.pts[(size_t)i * 3 + 1] + (t.pts[(size_t)j * 3 + 1] - t.pts[(size_t)i * 3 + 1]) * u;
        tan = Norm(ptT_[i] + (ptT_[j] - ptT_[i]) * u);
        return true;
    }
    // The turn path from lane la of link a (its end) to lane lb of link b (its start), at u in [0, 1].
    bool ZonePose(int a, int la, int b, int lb, float u, V2& p, float& y, V2& dir) const {
        const float rE = RE(a), rS = RS(b);
        V2 p0, t0, p3, t3;
        float y0, y3;
        if (!LanePose(a, Len_(a) - rE, LaneOff(a, la), p0, y0, t0) || !LanePose(b, rS, LaneOff(b, lb), p3, y3, t3)) return false;
        const float h = (rE + rS) / 3.0f;
        const V2 p1 = p0 + t0 * h, p2 = p3 - t3 * h;
        u = std::clamp(u, 0.0f, 1.0f);
        const float v = 1.0f - u;
        p = p0 * (v * v * v) + p1 * (3 * v * v * u) + p2 * (3 * v * u * u) + p3 * (u * u * u);
        const V2 d = (p1 - p0) * (3 * v * v) + (p2 - p1) * (6 * v * u) + (p3 - p2) * (3 * u * u);
        dir = Len(d) > 1e-4f ? Norm(d) : Norm(t0 + (t3 - t0) * u);
        y = y0 + (y3 - y0) * u;
        return true;
    }
    int8_t Classify(int a, int b) const {
        if (!Junction(table_->links[(size_t)a].to)) return kTurnThrough;
        const V2 da = endDir_[(size_t)a], db = startDir_[(size_t)b];
        const float ang = std::atan2(Dot2(db, RightOf(da)), Dot2(db, da));
        if (std::fabs(ang) > 2.6f) return kTurnU;
        if (ang > 0.6f) return kTurnRight;
        if (ang < -0.6f) return kTurnLeft;
        return kTurnThrough;
    }

    // ---------------------------------------------------------------------------------------------- routes and lanes
    void EnsureRoutes() {
        if (routes_->recs.size() < routeDone_ || routes_->links.size() < turn_.size()) { // the store was cleared
            routeDone_ = 0, turn_.clear(), cumT_.clear();
            std::fill(slotOf_.begin(), slotOf_.end(), -1);
            streams_.clear(), ring_.clear();
        }
        for (; routeDone_ < routes_->recs.size(); ++routeDone_) {
            const RouteStore::Rec& r = routes_->recs[routeDone_];
            turn_.resize((size_t)r.off + r.n, kTurnThrough), cumT_.resize((size_t)r.off + r.n, 0.0f);
            float acc = 0;
            for (uint32_t i = 0; i < r.n; ++i) {
                const int l = routes_->links[r.off + i];
                acc += Len_(l) / vFree_[(size_t)l];
                cumT_[r.off + i] = acc;
                turn_[r.off + i] = i + 1 < r.n ? Classify(l, routes_->links[r.off + i + 1]) : (int8_t)kTurnThrough;
            }
        }
        if (slotOf_.size() < routes_->recs.size()) slotOf_.resize(routes_->recs.size(), -1);
    }
    // The lane on link i of route r given the lane on link i - 1 (-1: the first link).
    int ChooseLane(uint32_t r, uint32_t i, int prev, uint32_t laneHash, bool driveDest) const {
        const int lanes = std::max<int>(1, table_->links[(size_t)RouteLink(r, i)].lanes);
        if (i + 1 >= RouteN(r)) return driveDest ? lanes - 1 : prev >= 0 ? std::min(prev, lanes - 1) : (int)(laneHash % (uint32_t)lanes);
        switch (TurnAt(r, i)) {
        case kTurnRight: return lanes - 1;
        case kTurnLeft:
        case kTurnU: return 0;
        default: return prev >= 0 ? std::min(prev, lanes - 1) : (int)(laneHash % (uint32_t)lanes);
        }
    }
    void LanePlan(uint32_t r, uint32_t idx, uint32_t laneHash, bool driveDest, uint8_t& prev, uint8_t& cur, uint8_t& next) const {
        const uint32_t n = RouteN(r);
        int lane = -1;
        prev = cur = next = 0;
        for (uint32_t i = 0; i < n && i <= idx + 1; ++i) {
            lane = ChooseLane(r, i, lane, laneHash, driveDest);
            if (i + 1 == idx) prev = (uint8_t)lane;
            if (i == idx) cur = (uint8_t)lane;
            if (i == idx + 1) next = (uint8_t)lane;
        }
    }
    bool HasNext(const Child& c) const { return c.route != ~0u && c.routeIdx + 1 < RouteN(c.route); }
    int NextLinkOf(const Child& c) const { return HasNext(c) ? RouteLink(c.route, c.routeIdx + 1) : -1; }
    bool LastLink(const Child& c) const { return c.route == ~0u || c.routeIdx + 1 >= RouteN(c.route); }

    // ---------------------------------------------------------------------------------------------- where a car is
    bool PoseOf(const Child& c, V2& p, float& y, V2& dir, float& alpha) const {
        if (c.link < 0 || (size_t)c.link >= table_->links.size()) return false;
        alpha = c.fade;
        const int l = c.link;
        const float len = Len_(l);
        if (HasNext(c)) {
            const int nl = NextLinkOf(c);
            const float rE = RE(l), rS = RS(nl);
            if (rE + rS > 0.5f && c.s > len - rE) return ZonePose(l, c.lane, nl, c.nextLane, (c.s - (len - rE)) / (rE + rS), p, y, dir);
        }
        if (c.prevLink >= 0) {
            const float rE = RE(c.prevLink), rS = RS(l);
            if (rE + rS > 0.5f && c.s < rS) return ZonePose(c.prevLink, c.prevLane, l, c.lane, (rE + c.s) / (rE + rS), p, y, dir);
        }
        float off = LaneOff(l, c.lane) + c.lat, dlat = 0;
        const bool first = c.route == ~0u || c.routeIdx == 0, last = LastLink(c);
        if (first && c.startAt >= 0) { // pulling out of its driveway
            const float x = (c.s - c.startAt) / 12.0f;
            if (x < 1.0f) {
                const float w = DriveOff(l) - LaneOff(l, c.lane);
                off += w * (1.0f - Smooth(x)), dlat -= w * SmoothD(x) / 12.0f;
            }
            alpha *= std::clamp((c.s - c.startAt) / 6.0f, 0.0f, 1.0f);
        } else if (first && c.startAt == kEdgeSpot) {
            alpha *= std::clamp(c.s / 8.0f, 0.0f, 1.0f);
        }
        if (last && c.stopAt >= 0) { // turning into its driveway
            const float x = (c.stopAt - c.s) / 12.0f;
            if (x < 1.0f) {
                const float w = DriveOff(l) - LaneOff(l, c.lane);
                off += w * (1.0f - Smooth(x)), dlat += w * SmoothD(x) / 12.0f;
            }
            alpha *= std::clamp((c.stopAt - c.s) / 6.0f, 0.0f, 1.0f);
        } else if (last && c.stopAt == kEdgeSpot) {
            alpha *= std::clamp((len - c.s) / 8.0f, 0.0f, 1.0f);
        }
        // a link too short for the whole swing: the driveway offset still ends where the turn paths begin (continuous)
        if (off != LaneOff(l, c.lane) + c.lat) {
            const float rs = c.prevLink >= 0 || (c.route != ~0u && c.routeIdx > 0) ? RS(l) : 0.0f, re = HasNext(c) ? RE(l) : 0.0f;
            const float k = std::clamp((c.s - rs) / 2.0f, 0.0f, 1.0f) * std::clamp((len - re - c.s) / 2.0f, 0.0f, 1.0f);
            const float base = LaneOff(l, c.lane) + c.lat;
            off = base + (off - base) * k, dlat *= k;
        }
        V2 tan;
        if (!LanePose(l, c.s, off, p, y, tan)) return false;
        dir = dlat != 0.0f ? Norm(tan + RightOf(tan) * dlat) : tan;
        return true;
    }

    // ---------------------------------------------------------------------------------------------- streams
    float RingAt(int slot, int64_t k) const { return ring_[(size_t)slot * kRing + (size_t)(k % kRing)]; }
    int64_t OldestK() const { return std::max<int64_t>(0, lastK_ - kRing + 1); }
    double KTime(int64_t k) const { return (double)k * params.sampleSeconds; }
    // tau at which the stream's count passes x (false: not yet).
    bool DepartTime(int slot, double x, double& out) const {
        const Stream& st = streams_[(size_t)slot];
        const double Nl = st.N, tl = KTime(lastK_);
        if (x >= Nl) {
            if (st.lambda <= 0) return false;
            out = tl + (x - Nl) / st.lambda;
            return out <= tau_;
        }
        const int64_t k0 = OldestK();
        const double N0 = RingAt(slot, k0);
        if (x < N0) {
            const double rate = k0 < lastK_ ? (RingAt(slot, k0 + 1) - N0) / params.sampleSeconds : st.lambda;
            out = rate > 1e-9 ? KTime(k0) - (N0 - x) / rate : -1e300;
            return true;
        }
        int64_t lo = k0, hi = lastK_; // largest k with N_k <= x (N_lastK > x)
        while (hi - lo > 1) {
            const int64_t mid = (lo + hi) / 2;
            if (RingAt(slot, mid) <= x) lo = mid;
            else hi = mid;
        }
        const double a = RingAt(slot, lo), b = RingAt(slot, lo + 1);
        out = KTime(lo) + params.sampleSeconds * (b > a ? (x - a) / (b - a) : 0.0);
        return true;
    }
    // The stream's count at tau = t.
    double CountAt(int slot, double t) const {
        const Stream& st = streams_[(size_t)slot];
        const double tl = KTime(lastK_);
        if (t >= tl) return st.N + st.lambda * (t - tl);
        const int64_t k0 = OldestK();
        if (t <= KTime(k0)) {
            const double N0 = RingAt(slot, k0);
            const double rate = k0 < lastK_ ? (RingAt(slot, k0 + 1) - N0) / params.sampleSeconds : st.lambda;
            return N0 + rate * (t - KTime(k0));
        }
        const int64_t k = std::min<int64_t>(lastK_ - 1, (int64_t)std::floor(t / params.sampleSeconds));
        const double a = RingAt(slot, k), b = RingAt(slot, k + 1);
        return a + (b - a) * (t - KTime(k)) / params.sampleSeconds;
    }
    int SlotFor(uint32_t r) {
        if (slotOf_[r] >= 0) return slotOf_[r];
        Stream st;
        st.route = r;
        st.createdK = lastK_;
        st.fresh = tau_ < freshUntil_ || freshUntil_ >= 1e299;
        slotOf_[r] = (int)streams_.size();
        streams_.push_back(st);
        ring_.resize(streams_.size() * kRing, 0.0f);
        return slotOf_[r];
    }
    float RouteSeconds(uint32_t r) const { return RouteN(r) ? CumT(r, RouteN(r) - 1) : 0.0f; }

    // Every sampleSeconds of tau: the counts move on by the rates of the interval that ends, then the rates follow the herds.
    void Sample(const HerdSystem& hs, double gameMinute) {
        ++lastK_;
        const double dt = params.sampleSeconds;
        for (size_t i = 0; i < streams_.size(); ++i) {
            Stream& st = streams_[i];
            if (lastK_ > 0) st.N += st.lambda * dt;
            ring_[i * kRing + (size_t)(lastK_ % kRing)] = (float)st.N;
        }
        // the herds' release rate on each route (decayed over about an hour of game time) becomes its stream's rate
        lastGameMinute_ = gameMinute;
        const size_t R = std::min(hs.routeReleased.size(), routes_->recs.size());
        bool created = false;
        for (uint32_t r = 0; r < (uint32_t)R; ++r) {
            if (slotOf_[r] < 0 && hs.routeReleased[r] <= 0) continue;
            const bool isNew = slotOf_[r] < 0;
            const int slot = SlotFor(r);
            Stream& st = streams_[(size_t)slot];
            st.who = hs.routeLast[r];
            st.released = hs.routeReleased[r];
            st.lambda = (float)(params.densityScale * hs.RouteRate(r, gameMinute));
            if (st.lambda < 1e-6f) st.lambda = 0;
            if (isNew) {
                created = true;
                if (st.fresh) { // the first traffic of this network: the stream has been running for the whole ring
                    const double base = (double)st.lambda * kRing * dt + 1.0;
                    for (int m = 0; m < kRing; ++m) ring_[(size_t)slot * kRing + (size_t)((lastK_ - m + kRing * 4) % kRing)] = (float)(base - (double)st.lambda * m * dt);
                    st.N = base;
                }
            }
            if (st.lambda > 0) st.lastActive = KTime(lastK_);
        }
        if (created && freshUntil_ >= 1e299) freshUntil_ = tau_ + 3.0;
        // which streams have trips on the road: link -> (stream, index) and the trips per second through every link
        std::fill(linkRate_.begin(), linkRate_.end(), 0.0f);
        std::fill(csrOff_.begin(), csrOff_.end(), 0);
        csrItem_.clear();
        int live = 0;
        for (size_t i = 0; i < streams_.size(); ++i) {
            const Stream& st = streams_[i];
            if (!Live(st)) continue;
            ++live;
            const RouteStore::Rec& rr = routes_->recs[st.route];
            for (uint32_t k = 0; k < rr.n; ++k) {
                const int l = routes_->links[rr.off + k];
                ++csrOff_[(size_t)l + 1];
                linkRate_[(size_t)l] += st.lambda;
            }
        }
        stats.streams = live;
        for (size_t l = 0; l + 1 < csrOff_.size(); ++l) csrOff_[l + 1] += csrOff_[l];
        csrItem_.resize((size_t)csrOff_.back());
        std::vector<int>& fill = csrFill_;
        fill.assign(csrOff_.begin(), csrOff_.end());
        for (size_t i = 0; i < streams_.size(); ++i) {
            const Stream& st = streams_[i];
            if (!Live(st)) continue;
            const RouteStore::Rec& rr = routes_->recs[st.route];
            for (uint32_t k = 0; k < rr.n; ++k) {
                const int l = routes_->links[rr.off + k];
                csrItem_[(size_t)fill[(size_t)l]++] = Item{(int)i, k};
            }
        }
    }
    std::vector<int> csrFill_;
    bool Live(const Stream& st) const { return st.lambda > 0 || KTime(lastK_) - st.lastActive < RouteSeconds(st.route) / 0.9 + 30.0; }

    // ---------------------------------------------------------------------------------------------- trips
    struct TripP {
        float jit, f, rank, oFrac, dFrac;
        uint32_t laneHash;
    };
    // A trip's rank alone (the budget's thinning), one hash: most trips of a heavy stream are decided here.
    float Rank(uint32_t r, uint32_t j) const { return (float)((SimSeed(params.citySeed ^ 0x52A4ull, (uint64_t)r + 1, (uint64_t)j) >> 40) * (1.0 / 16777216.0)); }
    TripP Params(uint32_t r, uint32_t j) const {
        SplitMix64 g(SimSeed(params.citySeed ^ 0x7219ull, (uint64_t)r + 1, (uint64_t)j));
        TripP t;
        t.jit = U01(g), t.f = 0.9f + 0.2f * U01(g), t.rank = Rank(r, j), t.oFrac = U01(g), t.dFrac = U01(g);
        t.laneHash = (uint32_t)(g.Next() >> 32);
        return t;
    }
    // A driveway on link l (part -1: anywhere; 0 / 1: the first / second part, for a trip that starts and ends on it).
    float DriveS(int l, float frac, int part) const {
        const float len = Len_(l);
        // clear of both turn paths by the 12 m the car takes to swing between its lane and the driveway
        float lo = RS(l) + 14.0f, hi = len - RE(l) - 14.0f;
        if (hi - lo < 4.0f) return part == 0 ? len * 0.35f : part == 1 ? len * 0.65f : len * 0.5f;
        if (part == 0) hi = lo + 0.4f * (hi - lo);
        else if (part == 1) lo = lo + 0.6f * (hi - lo);
        return lo + frac * (hi - lo);
    }
    // Trip j of route r as a car where its schedule puts it now (wantIdx >= 0: only if it is on that link of the route).
    bool TripChild(uint32_t r, uint32_t j, int wantIdx, Child& c, const TripP* given = nullptr, float delay = 0) const {
        if (r >= slotOf_.size() || slotOf_[r] < 0) return false;
        const int slot = slotOf_[r];
        const Stream& st = streams_[(size_t)slot];
        const TripP tp = given ? *given : Params(r, j);
        double td;
        if (!DepartTime(slot, (double)j + tp.jit, td)) return false;
        const uint32_t n = RouteN(r);
        const int l0 = RouteLink(r, 0), lN = RouteLink(r, n - 1);
        const bool edgeO = st.who.from < 0 && deg_[(size_t)table_->links[(size_t)l0].from] <= 1;
        const bool edgeD = st.who.to < 0 && deg_[(size_t)table_->links[(size_t)lN].to] <= 1;
        const float sO = edgeO ? 0.0f : DriveS(l0, tp.oFrac, n == 1 ? 0 : -1);
        const float sD = edgeD ? Len_(lN) : DriveS(lN, tp.dFrac, n == 1 ? 1 : -1);
        const float thO = sO / vFree_[(size_t)l0];
        const float thD = (n > 1 ? CumT(r, n - 2) : 0.0f) + sD / vFree_[(size_t)lN];
        if (thD <= thO + 0.5f) return false;
        const double th = thO + (tau_ - delay - td) * tp.f;
        if (th < thO || th >= thD) return false;
        const float* cb = cumT_.data() + routes_->recs[r].off;
        const uint32_t idx = (uint32_t)std::min<size_t>(n - 1, (size_t)(std::lower_bound(cb, cb + n, (float)th) - cb));
        if (wantIdx >= 0 && (int)idx != wantIdx) return false;
        const int l = RouteLink(r, idx);
        c = Child();
        c.route = r, c.routeIdx = idx, c.link = l, c.trip = j;
        c.s = std::clamp((float)(th - (idx ? cb[idx - 1] : 0.0f)) * vFree_[(size_t)l], 0.0f, Len_(l));
        c.startAt = edgeO ? kEdgeSpot : sO;
        c.stopAt = edgeD ? kEdgeSpot : sD;
        c.thO = thO;
        c.depart = td;
        c.laneHash = tp.laneHash;
        c.look.speedFactor = tp.f;
        LanePlan(r, idx, tp.laneHash, !edgeD, c.prevLane, c.lane, c.nextLane);
        if (idx > 0 && c.s < RS(l)) c.prevLink = RouteLink(r, idx - 1);
        c.herdId = st.who.herd, c.destHood = st.who.to, c.fromHood = st.who.from, c.tripType = st.who.type;
        c.v = tp.f * vFree_[(size_t)l];
        c.active = true;
        return true;
    }

    // ---------------------------------------------------------------------------------------------- the hash tables
    bool LiveHas(uint64_t key) const {
        for (size_t i = Mix(key) & (kLiveSlots - 1), n = 0; n < 16; ++n, i = (i + 1) & (kLiveSlots - 1)) {
            if (live_[i].stamp != frame_) return false;
            if (live_[i].key == key) return true;
        }
        return false;
    }
    void LivePut(uint64_t key) {
        for (size_t i = Mix(key) & (kLiveSlots - 1), n = 0; n < 16; ++n, i = (i + 1) & (kLiveSlots - 1))
            if (live_[i].stamp != frame_ || live_[i].key == key) { live_[i] = LiveE{key, frame_}; return; }
    }
    DelayE* DelayFind(uint64_t key) {
        for (size_t i = Mix(key) & (kDelaySlots - 1), n = 0; n < 8; ++n, i = (i + 1) & (kDelaySlots - 1))
            if (delay_[i].when >= 0 && delay_[i].key == key) return &delay_[i];
        return nullptr;
    }
    bool DelayHas(uint64_t key) const {
        for (size_t i = Mix(key) & (kDelaySlots - 1), n = 0; n < 8; ++n, i = (i + 1) & (kDelaySlots - 1))
            if (delay_[i].when >= 0 && delay_[i].key == key) return true;
        return false;
    }
    void DelayPut(uint64_t key, float d) {
        size_t best = Mix(key) & (kDelaySlots - 1);
        for (size_t i = best, n = 0; n < 8; ++n, i = (i + 1) & (kDelaySlots - 1)) {
            if (delay_[i].when < 0 || delay_[i].key == key) { best = i; break; }
            if (delay_[i].when < delay_[best].when) best = i;
        }
        delay_[best] = DelayE{key, d, tau_};
    }
    int First(int link, int lane) const {
        const uint64_t key = (uint64_t)link * 16 + (uint64_t)lane + 1;
        for (size_t i = Mix(key) & (kFirstSlots - 1), n = 0; n < 32; ++n, i = (i + 1) & (kFirstSlots - 1)) {
            if (first_[i].stamp != firstStamp_) return -1;
            if (first_[i].key == key) return first_[i].k;
        }
        return -1;
    }
    void FirstPut(int link, int lane, int k) {
        const uint64_t key = (uint64_t)link * 16 + (uint64_t)lane + 1;
        for (size_t i = Mix(key) & (kFirstSlots - 1), n = 0; n < 32; ++n, i = (i + 1) & (kFirstSlots - 1)) {
            if (first_[i].stamp != firstStamp_) { first_[i] = FirstE{key, firstStamp_, k}; return; }
            if (first_[i].key == key) return; // the first in sorted order stays
        }
    }

    // ---------------------------------------------------------------------------------------------- junctions
    bool Green(int node, V2 inDir) const {
        if (signalsOn) {
            V2 p{};
            if (!signalsOn(p, tau_ / 60.0)) return true;
        }
        const float cyc = params.signalCycle;
        const float ph = std::fmod((float)std::fmod(tau_, 100000.0) + phase_[(size_t)node], cyc);
        const bool axisA = std::fabs(Dot2(inDir, axis_[(size_t)node])) >= 0.7071f;
        const float half = 0.5f * cyc, green = half - 3.0f;
        if (ph < green) return axisA;
        if (ph >= half && ph < half + green) return !axisA;
        return false; // all-red
    }
    static bool Conflict(const Entry& a, const Entry& b) {
        if (a.inLink == b.inLink) { // one queue (car following does it) unless two lanes swap or merge on the way through
            if (a.outLink != b.outLink || a.inLane == b.inLane) return false;
            return a.outLane == b.outLane || (a.inLane < b.inLane) != (a.outLane < b.outLane);
        }
        if (a.outLink == b.outLink) return true; // two ways into one road: their paths cross or merge
        if (a.turn == kTurnRight || b.turn == kTurnRight) return false;
        const bool opposite = Dot2(a.inDir, b.inDir) < -0.7f;
        if (opposite && a.turn == kTurnThrough && b.turn == kTurnThrough) return false;
        if (opposite && a.turn == kTurnLeft && b.turn == kTurnLeft) return false;
        return true;
    }
    static bool Prior(const Entry& o, const Entry& e) { // o goes before e
        if (o.rank > e.rank * 1.2f) return true;
        if (e.rank > o.rank * 1.2f) return false;
        if (o.tLine < e.tLine - 0.1f) return true;
        if (e.tLine < o.tLine - 0.1f) return false;
        return o.inLink < e.inLink;
    }
    void BuildEntries() {
        entries_.clear();
        std::fill(entryOf_.begin(), entryOf_.end(), -1);
        for (size_t i = 0; i < pool.size(); ++i) {
            const Child& c = pool[i];
            if (!c.active || c.link < 0) continue;
            Entry e;
            e.car = (int)i;
            if (c.prevLink >= 0 && c.committed && Junction(table_->links[(size_t)c.link].from)) { // in the box, on its way out
                e.node = table_->links[(size_t)c.link].from, e.inLink = c.prevLink, e.outLink = c.link, e.inLane = c.prevLane, e.outLane = c.lane;
                e.turn = c.route != ~0u && c.routeIdx > 0 ? TurnAt(c.route, c.routeIdx - 1) : (int8_t)kTurnThrough;
                e.occ = true, e.inDir = endDir_[(size_t)c.prevLink], e.rank = rank_[(size_t)c.prevLink];
            } else if (HasNext(c) && Junction(table_->links[(size_t)c.link].to)) {
                const float toLine = Len_(c.link) - RE(c.link) - c.s;
                if (!c.committed && toLine > 60.0f) continue;
                e.node = table_->links[(size_t)c.link].to, e.inLink = c.link, e.outLink = NextLinkOf(c), e.inLane = c.lane, e.outLane = c.nextLane;
                e.turn = TurnAt(c.route, c.routeIdx), e.occ = c.committed, e.inDir = endDir_[(size_t)c.link], e.rank = rank_[(size_t)c.link];
                e.tLine = std::max(0.0f, toLine) / std::max(1.0f, c.v);
            } else {
                continue;
            }
            entries_.push_back(e);
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.node != b.node ? a.node < b.node : a.car < b.car; });
        for (size_t k = 0; k < entries_.size(); ++k) entryOf_[(size_t)entries_[k].car] = (int)k;
    }
    // May the car of entry e enter the box now?
    bool Permit(const Child& c, int e) const {
        const Entry& E = entries_[(size_t)e];
        const int n = E.node;
        const bool sig = Signalised(n);
        if (sig && !Green(n, E.inDir)) return false;
        size_t lo = (size_t)e, hi = (size_t)e;
        while (lo > 0 && entries_[lo - 1].node == n) --lo;
        while (hi + 1 < entries_.size() && entries_[hi + 1].node == n) ++hi;
        for (size_t k = lo; k <= hi; ++k) {
            if ((int)k == e) continue;
            const Entry& O = entries_[k];
            if (!Conflict(E, O)) continue;
            if (O.occ) return false;
            if (sig) {
                if (!Green(n, O.inDir)) continue;
                if (E.turn == kTurnLeft && O.turn != kTurnLeft && O.tLine < 4.0f) return false; // a left turn waits for oncoming traffic
                continue;
            }
            if (c.wait > 20.0f || O.tLine > 4.0f) continue;
            if (Prior(O, E)) return false;
        }
        // room on the way out: never block the box
        const int f = First(E.outLink, E.outLane);
        if (f >= 0) {
            const Child& o = pool[order_[(size_t)f]];
            if (o.active && o.s - o.length < RS(E.outLink) + 1.5f && o.v < 3.0f) return false;
        }
        return true;
    }
    static float TurnSpeed(int8_t turn, float v0) {
        switch (turn) {
        case kTurnRight: return std::min(v0, 6.0f);
        case kTurnLeft: return std::min(v0, 8.0f);
        case kTurnU: return std::min(v0, 3.5f);
        default: return v0;
        }
    }

    // ---------------------------------------------------------------------------------------------- one sub-step
    void Step(float dt, const HerdSystem& hs) {
        int n = 0;
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].active) order_[(size_t)n++] = (uint16_t)i;
        std::sort(order_.begin(), order_.begin() + n, [&](uint16_t a, uint16_t b) {
            const Child &x = pool[a], &y = pool[b];
            if (x.link != y.link) return x.link < y.link;
            if (x.lane != y.lane) return x.lane < y.lane;
            if (x.s != y.s) return x.s < y.s;
            return a < b;
        });
        ++firstStamp_;
        crossers_.clear();
        for (int k = 0; k < n; ++k) {
            const Child& c = pool[order_[(size_t)k]];
            FirstPut(c.link, c.lane, k);
            if (Crossing(c) && crossers_.size() < crossers_.capacity()) crossers_.push_back(order_[(size_t)k]);
        }
        BuildEntries();
        for (int k = 0; k < n; ++k) {
            const int ci = order_[(size_t)k];
            Child& c = pool[(size_t)ci];
            if (!c.active) continue;
            const LinkInfo& L = table_->links[(size_t)c.link];
            const float len = L.len;
            const bool hasNext = HasNext(c);
            const int nl = hasNext ? NextLinkOf(c) : -1;
            const float rE = hasNext ? RE(c.link) : 0.0f;
            const bool junction = hasNext && Junction(L.to);
            // desired speed: the link's (congested) speed x the driver, the road's curvature, the turn ahead or under it
            const float linkV = table_->links.size() == hs.loadPrev.size() && hs.table == table_ ? hs.LinkSpeed(c.link) / 60.0f : vFree_[(size_t)c.link];
            float v0 = std::max(0.5f, std::min(linkV * c.look.speedFactor, vCurve_[(size_t)c.link]));
            if (hasNext) {
                const float vT = junction ? TurnSpeed(TurnAt(c.route, c.routeIdx), v0) : v0;
                if (c.s > len - rE) v0 = std::min(v0, vT);
                else v0 = std::min(v0, std::sqrt(vT * vT + 2.0f * params.aComfort * (len - rE - c.s)));
            }
            if (c.prevLink >= 0 && c.s < RS(c.link) && Junction(L.from) && c.route != ~0u && c.routeIdx > 0) v0 = std::min(v0, TurnSpeed(TurnAt(c.route, c.routeIdx - 1), v0));
            if (LastLink(c) && c.stopAt >= 0) v0 = std::min(v0, std::sqrt(16.0f + 2.0f * params.aComfort * std::max(0.0f, c.stopAt - 12.0f - c.s)));
            v0 = std::max(v0, 0.5f);
            // the nearest thing ahead: gap (bumper to bumper) and its speed
            float gap = 1e9f, lv = c.v;
            bool pulledAside = false;
            for (int j = k + 1; j < n; ++j) {
                const Child& o = pool[order_[(size_t)j]];
                if (o.link != c.link || o.lane != c.lane) break;
                if (!o.active) continue;
                if (c.emergency && o.lat > 1.2f) continue; // it pulled over: pass it
                gap = o.s - c.s - 0.5f * (o.length + c.length), lv = o.hold ? 0.0f : o.v;
                break;
            }
            for (int j = k - 1; j >= 0 && !c.emergency; --j) { // an emergency vehicle close behind in this lane: pull aside
                const Child& o = pool[order_[(size_t)j]];
                if (o.link != c.link || o.lane != c.lane) break;
                if (c.s - o.s > 80.0f) break;
                if (o.active && o.emergency) pulledAside = true;
            }
            for (uint16_t x : crossers_) { // a car pulling out across this lane from its driveway is in the way too
                const Child& o = pool[x];
                if (&o == &c || !o.active || o.link != c.link || o.lane >= c.lane || o.s < c.s) continue;
                const float g = o.s - c.s - 0.5f * (o.length + c.length);
                if (g < gap) gap = g, lv = o.v;
            }
            c.latTarget = pulledAside ? 1.9f : 0.0f;
            c.lat += std::clamp(c.latTarget - c.lat, -1.5f * dt, 1.5f * dt);
            if (gap >= 1e8f && hasNext) { // nobody ahead on the link: the last car of the queue on the next one
                const int f = First(nl, c.nextLane);
                if (f >= 0) {
                    const Child& o = pool[order_[(size_t)f]];
                    if (o.active) gap = (len - c.s) + o.s - 0.5f * (o.length + c.length), lv = o.hold ? 0.0f : o.v;
                }
            }
            // the junction: a stop line until the car may enter
            if (junction && !c.committed && entryOf_[(size_t)ci] >= 0) {
                const float toLine = len - rE - c.s;
                const bool ok = Permit(c, entryOf_[(size_t)ci]);
                const float commitAt = std::min(30.0f, c.v * c.v / (2.0f * params.aComfort) + 4.0f);
                if (ok && toLine < commitAt) {
                    c.committed = true, c.wait = 0;
                    entries_[(size_t)entryOf_[(size_t)ci]].occ = true;
                } else if (!ok) {
                    const bool cantStop = c.v * c.v / (2.0f * 7.0f) > toLine + 0.5f;
                    if (cantStop && Signalised(L.to) && toLine < 15.0f) { // the light changed under it: it goes
                        c.committed = true, ++stats.forced;
                        entries_[(size_t)entryOf_[(size_t)ci]].occ = true;
                    } else {
                        const float g = toLine - 0.3f;
                        if (g < gap) gap = g, lv = 0;
                        if (c.v < 0.5f) {
                            if (c.wait == 0) ++stats.waits;
                            c.wait += dt;
                        }
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
            if (ds < c.v * dt) c.v = ds / std::max(1e-4f, dt);
            c.s += c.hold ? 0.0f : ds;
            Advance(c);
        }
    }
    // Pulling out of its driveway, still across the lanes to the curb of its own.
    bool Crossing(const Child& c) const {
        return c.startAt >= 0 && (c.route == ~0u || c.routeIdx == 0) && c.s - c.startAt < 12.0f && c.lane + 1 < table_->links[(size_t)c.link].lanes;
    }
    std::vector<uint16_t> crossers_;
    // Past the end of the link: on to the next one through the turn path, or arrived.
    void Advance(Child& c) {
        if (LastLink(c) && c.stopAt >= 0 && c.s >= c.stopAt) { Remove(c, true); return; } // in its driveway
        for (int guard = 0; guard < 4 && c.active && c.s >= Len_(c.link); ++guard) {
            if (!HasNext(c)) { Remove(c, true); return; } // off the map (or a test car at its link's end)
            const float len = Len_(c.link);
            const int nl = NextLinkOf(c);
            c.s -= len;
            c.prevLink = c.link, c.prevLane = c.lane;
            c.link = nl;
            ++c.routeIdx;
            c.lane = c.nextLane;
            c.nextLane = HasNext(c) ? (uint8_t)ChooseLane(c.route, c.routeIdx + 1, c.lane, c.laneHash, c.stopAt >= 0) : c.lane;
        }
        if (c.active && c.prevLink >= 0 && c.s >= RS(c.link)) c.prevLink = -1, c.committed = false; // out of the box
        if (c.active && LastLink(c) && c.stopAt >= 0 && c.s >= c.stopAt) Remove(c, true);
        else if (c.active && LastLink(c) && c.stopAt >= 0) c.parked = c.stopAt - c.s < 6.0f && c.v < 1.0f;
    }
    void Remove(Child& c, bool arrived) {
        if (!c.active) return;
        c.active = false;
        ++stats.despawned;
        if (arrived) ++stats.arrivals;
        if (c.promoted) --stats.promoted;
    }

    // ---------------------------------------------------------------------------------------------- drop, spawn
    void Drop(V2 focus) {
        const float Rd = DetailRadius();
        for (size_t i = 0; i < pool.size(); ++i) {
            Child& c = pool[i];
            if (!c.active) continue;
            V2 p, d;
            float y, a;
            if (!PoseOf(c, p, y, d, a)) { Remove(c, false); continue; }
            posCache_[i] = p;
            const float dist = Len(p - focus);
            c.visible = dist <= params.radius;
            if (c.promoted || dist <= Rd) continue;
            // back to its trip, remembering how far behind (or ahead of) its schedule it is
            if (c.route != ~0u && c.trip != kNoTrip && c.depart > -1e299) {
                const float thAct = (c.routeIdx ? CumT(c.route, c.routeIdx - 1) : 0.0f) + c.s / vFree_[(size_t)c.link];
                const double thAna = c.thO + (tau_ - c.depart) * c.look.speedFactor;
                const float dl = (float)std::clamp((thAna - thAct) / std::max(0.5f, c.look.speedFactor), -60.0, 600.0);
                if (std::fabs(dl) > 0.25f) DelayPut(TripKey(c.route, c.trip), dl);
                else if (DelayE* e = DelayFind(TripKey(c.route, c.trip))) e->when = -1;
            }
            Remove(c, false);
            ++stats.dropped;
        }
        for (DelayE& e : delay_) // a delay older than any trip: gone
            if (e.when >= 0 && tau_ - e.when > 900.0) e.when = -1;
    }
    // Room for a new car: nobody within a safe gap in its lane (a car pulling out of its driveway: in every lane it crosses
    // on its way, its own and the ones to the curb), nobody close in a turn path or a crossing road.
    bool GapFree(const Child& cand, V2 p, bool departing) const {
        const float need = std::max(cand.length + params.s0 + 1.5f, cand.v * 0.6f + cand.length);
        for (size_t i = 0; i < pool.size(); ++i) {
            const Child& o = pool[i];
            if (!o.active) continue;
            const bool inLane = o.lane == cand.lane || (departing && o.lane > cand.lane);
            if (o.link == cand.link && inLane) {
                if (std::fabs(o.s - cand.s) < need) return false;
                if (o.s < cand.s && cand.s - o.s < o.v * (departing ? 2.0f : 1.0f) + cand.length + params.s0) return false; // room for the car behind
            }
            if (o.link == cand.link && !inLane) continue; // side by side in the next lane is fine
            if (Len(posCache_[i] - p) < 5.0f) return false;           // elsewhere (a turn path, a crossing road): keep clear
        }
        return true;
    }
    void Spawn(V2 focus) {
        const float Rv = params.radius, Rd = DetailRadius();
        // Which trips to look at: in the band (between the view and the detail radius) always; inside the view only the ones
        // that just left their driveway, unless the whole view is new (the first frames of a network, a zoom out, a jump).
        const bool fullScan = !budgetSet_ || (tau_ < freshUntil_) || Rv > prevRv_ * 1.01f || Len(focus - prevFocus_) > 0.25f * Rv;
        const float Ri = std::max(0.0f, Rv - 15.0f); // the inner chord, a little short of the view: lanes sit off the polyline
        near_.clear();
        double expect = 0;
        for (size_t l = 0; l < table_->links.size(); ++l) {
            if (Len(centre_[l] - focus) > Rd + reach_[l]) continue;
            float s, d;
            if (!table_->Project((int)l, focus, s, d) || d >= Rd) continue;
            const float half = std::sqrt(Rd * Rd - d * d), sc = polyScale_[l];
            const float lo = std::max(0.0f, (s - half) / sc), hi = std::min(Len_((int)l), (s + half) / sc);
            if (hi <= lo) continue;
            NearE ne{(int)l, lo, hi, 1.0f, 0.0f};
            if (d < Ri) { // the stretch inside the view
                const float ih = std::sqrt(Ri * Ri - d * d);
                ne.ilo = std::max(lo, (s - ih) / sc), ne.ihi = std::min(hi, (s + ih) / sc);
            }
            near_.push_back(ne);
            expect += (double)linkRate_[l] * (hi - lo) / vFree_[l];
        }
        // the budget: trips near the camera thinned by their rank so the cars fit the pool
        const float target = expect > 1e-6 ? (float)std::min(1.0, 0.9 * params.maxChildren / expect) : 1.0f;
        budget_ = budgetSet_ ? budget_ + (target - budget_) * 0.1f : target;
        budgetSet_ = true;
        int free = 0;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i].active) {
                if (pool[i].route != ~0u && pool[i].trip != kNoTrip) LivePut(TripKey(pool[i].route, pool[i].trip));
            } else {
                ++free;
            }
        }
        if (free == 0) return;
        int spawns = 0;
        // trips that were dropped off their schedule first
        for (size_t k = 0; k < delay_.size() && spawns < params.spawnsPerFrame; ++k) {
            const DelayE e = delay_[k];
            if (e.when < 0) continue;
            const uint32_t r = (uint32_t)((e.key - 1) >> 32), j = (uint32_t)((e.key - 1) & 0xFFFFFFFFu);
            if (r >= slotOf_.size() || slotOf_[r] < 0) { delay_[k].when = -1; continue; }
            spawns += Try(r, j, -1, e.delay, focus, Rv, Rd);
        }
        for (const NearE& ne : near_) {
            if (spawns >= params.spawnsPerFrame) break;
            const int l = ne.link;
            for (int q = csrOff_[(size_t)l]; q < csrOff_[(size_t)l + 1] && spawns < params.spawnsPerFrame; ++q) {
                const Item it = csrItem_[(size_t)q];
                const uint32_t r = streams_[(size_t)it.slot].route;
                if (fullScan || ne.ihi <= ne.ilo) {
                    spawns += Scan(it, r, ne.lo, ne.hi, focus, Rv, Rd, params.spawnsPerFrame - spawns);
                } else {
                    spawns += Scan(it, r, ne.lo, ne.ilo, focus, Rv, Rd, params.spawnsPerFrame - spawns);
                    spawns += Scan(it, r, ne.ihi, ne.hi, focus, Rv, Rd, params.spawnsPerFrame - spawns);
                    if (it.idx == 0) { // the trips that left in the last moments: from their driveways in view
                        const double jLo = std::max(0.0, std::floor(CountAt(it.slot, tau_ - 2.5)) - 1.0), jHi = std::ceil(CountAt(it.slot, tau_)) + 1.0;
                        for (double jd = jLo; jd <= jHi && spawns < params.spawnsPerFrame; jd += 1.0) spawns += Try(r, (uint32_t)jd, 0, 0.0f, focus, Rv, Rd);
                    }
                }
            }
        }
    }
    // The trips of one stream that are on link metres [lo, hi] of its idx-th link now.
    int Scan(Item it, uint32_t r, float lo, float hi, V2 focus, float Rv, float Rd, int left) {
        if (hi <= lo || left <= 0) return 0;
        const int l = RouteLink(r, it.idx);
        const float c0 = it.idx ? CumT(r, it.idx - 1) : 0.0f, v = vFree_[(size_t)l];
        const float thLo = c0 + lo / v, thHi = c0 + hi / v, T0 = it.idx ? CumT(r, 0) : 0.0f; // an origin anywhere on the first link
        const double tdLo = tau_ - thHi / 0.9, tdHi = tau_ - std::max(0.0f, thLo - T0) / 1.1;
        double jLo = std::max(0.0, std::floor(CountAt(it.slot, tdLo)) - 1.0);
        const double jHi = std::ceil(CountAt(it.slot, tdHi)) + 1.0;
        if (jHi - jLo > 4000.0) jLo = jHi - 4000.0; // a stream far past any road's capacity: its oldest trips there are skipped
        int got = 0;
        for (double jd = jLo; jd <= jHi && got < left; jd += 1.0) {
            const uint32_t j = (uint32_t)jd;
            if (Rank(r, j) >= budget_) continue; // most trips of a heavy stream end here, at one hash
            got += Try(r, j, (int)it.idx, 0.0f, focus, Rv, Rd);
        }
        return got;
    }    // One trip: materialise it if it should be a car now and may appear where it is. Returns 1 when it did.
    int Try(uint32_t r, uint32_t j, int wantIdx, float delay, V2 focus, float Rv, float Rd) {
        const uint64_t key = TripKey(r, j);
        const TripP tp = Params(r, j);
        if (tp.rank >= budget_) return 0;
        if (LiveHas(key)) return 0;
        if (wantIdx >= 0 && DelayHas(key)) return 0; // its own schedule is the delayed one
        Child c;
        if (!TripChild(r, j, wantIdx, c, &tp, delay)) return 0;
        V2 p, d;
        float y, a;
        if (!PoseOf(c, p, y, d, a)) return 0;
        const float dist = Len(p - focus);
        if (dist > Rd) return 0;
        bool fadeIn = false, departing = false;
        if (dist > Rv) {
            // the band: never seen appearing
        } else if (c.routeIdx == 0 && (c.startAt >= 0 ? c.s - c.startAt : c.startAt == kEdgeSpot ? c.s : 1e9f) < 25.0f) {
            departing = true; // at its driveway or the map edge: it pulls out (from rest)
            c.s = c.startAt >= 0 ? c.startAt : 0.0f;
            c.v = 0;
            if (!PoseOf(c, p, y, d, a)) return 0;
        } else if (Len(p - prevFocus_) > prevRv_ || (streams_[(size_t)slotOf_[r]].fresh && tau_ < freshUntil_)) {
            fadeIn = true; // the place just came into view (or the network is new): it fades in where it is
        } else {
            ++stats.skipped;
            return 0;
        }
        // speed: its trip's, but never into a car ahead; a car in a turn path has the junction
        if (!departing) c.v = std::min(c.v, vCurve_[(size_t)c.link]);
        const LinkInfo& L = table_->links[(size_t)c.link];
        if (HasNext(c) && c.s > L.len - RE(c.link) && Junction(L.to)) c.committed = true;
        if (c.prevLink >= 0 && Junction(L.from)) c.committed = true;
        if (fadeIn && c.committed) return 0; // never fade in inside a junction: it shows once it is out (or never)
        if (!GapFree(c, p, departing)) return 0;
        int slot = -1;
        for (size_t i = 0; i < pool.size(); ++i)
            if (!pool[i].active) { slot = (int)i; break; }
        if (slot < 0) return 0;
        // who: a real tenant slot of the trip's home hood, or a hash of the trip when no sim is behind it
        SplitMix64 g(SimSeed(params.citySeed, key, 0x7E7));
        const Stream& st = streams_[(size_t)slotOf_[r]];
        Herd who;
        who.id = st.who.herd, who.from = st.who.from, who.to = st.who.to, who.homeHood = st.who.home, who.workHood = st.who.work, who.type = st.who.type;
        const bool home = st.who.home >= 0;
        if (!tenant || !(home || st.who.work >= 0) || !tenant(who, home, g.Next(), c.slot)) {
            c.slot.cell = -1;
            c.slot.index = (uint32_t)(g.Next() & 0xFFFF);
            c.slot.address = key;
        }
        const float f = c.look.speedFactor;
        c.look = LookFor(params.citySeed, c.slot.address, c.slot.index, params.models);
        c.look.speedFactor = f; // the trip's own: the car keeps the schedule its trip keeps
        c.fade = fadeIn ? 0.0f : 1.0f;
        c.serial = ++serial_;
        if (delay != 0.0f)
            if (DelayE* e = DelayFind(key)) e->when = -1; // back on the road: the car carries its delay now
        // c.depart stays the trip's own departure: when the car is dropped, its lag behind that is what is remembered
        pool[(size_t)slot] = c;
        posCache_[(size_t)slot] = p;
        LivePut(key);
        ++stats.spawned;
        stats.departures += departing;
        stats.fadedIn += fadeIn;
        return 1;
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
