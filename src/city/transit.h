// Transit lines + outside connections + the first traffic system. Platform free (the host feeds PlaceContext).
//
//   Lines     buses, trams, trains, boats, planes, subways; each line carries either passengers or cargo (never mixed:
//             a passenger line only stops at passenger checkpoints, a cargo line only at cargo checkpoints).
//             Build a line by clicking checkpoints one after another; click the FIRST checkpoint again to close it as a
//             loop. Enter finishes it as an out-and-back line instead. Right click / Esc cancels the line in progress.
//   Outside   two-way arrows at the edge of the city: the roads / rails / sea lanes / air lanes to cities we never see
//             but trade with. Every connection carries people and cargo both ways. THIS is how the city gets its people.
//   Traffic   line vehicles drive their stops (headway keeping, dwell time); outside road / rail connections also spawn
//             arrivals that drive across the street network and leave again, so congestion has something to build on.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "transport.h"

namespace tbd {
namespace city {

enum class TMode : uint8_t { Bus, Tram, Train, Boat, Plane, Subway, Count };
enum class TKind : uint8_t { Passenger, Cargo };
enum class OutKind : uint8_t { Road, Rail, Sea, Air, Count };

inline const char* TModeName(TMode m) {
    static const char* const k[] = {"Bus", "Tram", "Train", "Boat", "Plane", "Subway"};
    return k[(int)m];
}
inline const char* TKindName(TKind k) { return k == TKind::Passenger ? "Passenger" : "Cargo"; }
inline const char* OutKindName(OutKind k) {
    static const char* const n[] = {"Road", "Rail", "Sea lane", "Air lane"};
    return n[(int)k];
}
inline TMode OutMode(OutKind k) { // the line mode that serves a connection
    static const TMode m[] = {TMode::Bus, TMode::Train, TMode::Boat, TMode::Plane};
    return m[(int)k];
}
inline Vec3 TModeColor(TMode m) {
    static const Vec3 c[] = {{1.00f, 0.62f, 0.15f}, {0.30f, 0.80f, 0.45f}, {0.35f, 0.55f, 1.00f}, {0.25f, 0.85f, 0.95f}, {0.90f, 0.35f, 0.80f}, {0.95f, 0.30f, 0.30f}};
    return c[(int)m];
}
inline float TModeSpeed(TMode m) { // metres per second
    static const float s[] = {11.0f, 12.0f, 30.0f, 8.0f, 70.0f, 20.0f};
    return s[(int)m];
}

struct Stop {
    uint64_t id = 0;
    TMode mode = TMode::Bus;
    TKind kind = TKind::Passenger;
    V2 p{};
    V2 dir{1, 0};
    float y = 0;
    int outside = -1; // index of the outside connection this checkpoint is, or -1
    float waiterT = 0; // game seconds until the next passenger comes to wait here
    bool alive = true;
};
struct Route {
    uint64_t id = 0;
    TMode mode = TMode::Bus;
    TKind kind = TKind::Passenger;
    std::vector<uint64_t> stops;
    bool closed = false;     // a loop (the first checkpoint was clicked again)
    bool connected = true;   // every leg found a path along the roads / rails
    bool alive = true;
    std::string name;
    std::vector<std::vector<V2>> legs; // legs[i]: stop i -> stop i+1 (closed: the last leg returns to stop 0)
    std::vector<float> legLen;
    int vehicles = 0;
};
struct Outside {
    uint64_t id = 0;
    OutKind kind = OutKind::Road;
    V2 p{};
    V2 dir{1, 0};      // pointing out of the city
    float y = 0;
    bool alive = true;
    bool linked = false; // reaches the city's own network (road / rail near, a harbour / airport exists)
    float peopleIn = 0, peopleOut = 0, cargoIn = 0, cargoOut = 0;
    float acc = 0, carAcc = 0, walkAcc = 0;
    std::string name;    // the city beyond the map this link leads to
    int stop = -1;       // its checkpoint (index into stops)
};
struct Vehicle {
    uint64_t id = 0;
    int route = -1;      // index into routes; -1: an arrival that crosses the city on its own
    TMode mode = TMode::Bus;
    TKind kind = TKind::Passenger;
    int leg = 0;
    int dirSign = 1;     // open lines run out and back
    float s = 0;         // metres along the current polyline
    float speed = 0;
    float dwell = 0;
    std::vector<V2> path; // arrivals only
    V2 pos{}, dir{1, 0};
    float y = 0;
    bool alive = true;
};

// A person on foot. People arrive from the outside cities through any connection (road, rail, sea, air) and step out at the
// curb (or the quay / the terminal) to walk the last stretch to a home or a workplace, then go inside.
struct Citizen {
    uint64_t id = 0;
    std::vector<V2> path;   // the walk (at least two points)
    float s = 0;            // metres walked
    float len = 0;
    float wait = 0;         // game seconds still to stand before walking (stepping out of the vehicle)
    float stand = 0;        // game seconds spent standing at the door before going inside
    int variant = 0;        // which of the pedestrian models
    TMode from = TMode::Bus; // how they came
    V2 pos{}, dir{1, 0};
    bool walking = true;
    bool alive = true;
    // animation the host plays: 0 idle, 1 walk, 2 sit down, 3 seated, 4 stand up; animT is seconds into the sit / stand clip
    int anim = 0;
    float animT = 0;
    // a passenger waiting at a stop (stopId != 0): phase 0 walks to the bench, 1 sits down, 2 sits, 3 stands up, 4 walks to the pole
    uint64_t stopId = 0;
    int phase = 0, seat = 0;
    float patience = 0;
};
// The bench beside a bus / tram stop: three seats in a row along the line's direction, facing the pole.
inline V2 StopSide(const V2& dir) { return V2{dir.z, -dir.x}; }
inline V2 BenchCentre(V2 p, V2 dir) { return p + StopSide(dir) * 4.5f; }
inline V2 SeatPos(V2 p, V2 dir, int seat) { return BenchCentre(p, dir) + dir * (1.0f * (float)(seat - 1)); }
inline V2 SeatFacing(V2 dir) { return StopSide(dir) * -1.0f; }
inline bool HasBench(TMode m) { return m == TMode::Bus || m == TMode::Tram; }

// ---------------------------------------------------------------------------------------------------- path finding
// Shortest way between two ground points along road (rail=false) or railway segments.
inline void SampleSeg(const Network& net, int s, float t0, float t1, std::vector<V2>& out) {
    const Cubic c = net.Curve(s);
    const int n = std::max(2, (int)(std::fabs(t1 - t0) * 10.0f));
    for (int i = 0; i <= n; ++i) out.push_back(c.At(t0 + (t1 - t0) * i / n));
}
inline bool FindPath(const Network& net, V2 a, V2 b, bool rail, std::vector<V2>& out) {
    auto nearestSeg = [&](V2 p, float& t) {
        int best = -1;
        float bd = 60.0f;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || net.IsRailSeg((int)s) != rail) continue;
            float ti, di;
            net.Nearest((int)s, p, ti, di);
            if (di < bd) bd = di, best = (int)s, t = ti;
        }
        return best;
    };
    float ta = 0, tb = 0;
    const int sa = nearestSeg(a, ta), sb = nearestSeg(b, tb);
    if (sa < 0 || sb < 0) return false;
    out.clear();
    out.push_back(a);
    if (sa == sb) {
        SampleSeg(net, sa, ta, tb, out);
        out.push_back(b);
        return true;
    }
    const size_t N = net.nodes.size();
    std::vector<float> dist(N, 1e30f);
    std::vector<int> prevSeg(N, -1), prevNode(N, -1), src(N, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    const float la = net.Curve(sa).Length();
    const int starts[2] = {net.segs[sa].a, net.segs[sa].b};
    const float sCost[2] = {ta * la, (1.0f - ta) * la};
    for (int k = 0; k < 2; ++k) {
        dist[starts[k]] = sCost[k], src[starts[k]] = k;
        pq.push({sCost[k], starts[k]});
    }
    // adjacency by scan (the networks are small); built once
    std::vector<std::vector<int>> adj(N);
    std::vector<float> slen(net.segs.size(), 0.0f);
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!net.segs[s].alive || net.IsRailSeg((int)s) != rail) continue;
        adj[net.segs[s].a].push_back((int)s), adj[net.segs[s].b].push_back((int)s);
        slen[s] = net.Curve((int)s).Length();
    }
    const float lb = net.Curve(sb).Length();
    const int goals[2] = {net.segs[sb].a, net.segs[sb].b};
    const float gCost[2] = {tb * lb, (1.0f - tb) * lb};
    while (!pq.empty()) {
        const auto [d, n] = pq.top();
        pq.pop();
        if (d > dist[n]) continue;
        for (int s : adj[n]) {
            const int m = net.segs[s].a == n ? net.segs[s].b : net.segs[s].a;
            const float nd = d + slen[s];
            if (nd < dist[m]) dist[m] = nd, prevSeg[m] = s, prevNode[m] = n, src[m] = src[n], pq.push({nd, m});
        }
    }
    int bg = -1;
    float best = 1e30f;
    for (int k = 0; k < 2; ++k)
        if (dist[goals[k]] + gCost[k] < best) best = dist[goals[k]] + gCost[k], bg = k;
    if (bg < 0 || best >= 1e29f) return false;
    // unwind goal -> start
    std::vector<std::pair<int, int>> chain; // (segment, from node)
    int n = goals[bg];
    while (prevSeg[n] >= 0) chain.push_back({prevSeg[n], prevNode[n]}), n = prevNode[n];
    // start leg: a's segment to its start node
    const int k0 = src[goals[bg]];
    SampleSeg(net, sa, ta, k0 == 0 ? 0.0f : 1.0f, out);
    for (size_t i = chain.size(); i-- > 0;) {
        const int s = chain[i].first;
        const bool fwd = net.segs[s].a == chain[i].second;
        SampleSeg(net, s, fwd ? 0.0f : 1.0f, fwd ? 1.0f : 0.0f, out);
    }
    SampleSeg(net, sb, bg == 0 ? 0.0f : 1.0f, tb, out);
    out.push_back(b);
    return true;
}
inline float PolyLen(const std::vector<V2>& p) {
    float l = 0;
    for (size_t i = 1; i < p.size(); ++i) l += Len(p[i] - p[i - 1]);
    return l;
}
inline void PolyAt(const std::vector<V2>& p, float s, V2& pos, V2& dir) {
    for (size_t i = 1; i < p.size(); ++i) {
        const float l = Len(p[i] - p[i - 1]);
        if (s <= l || i + 1 == p.size()) {
            dir = l > 1e-4f ? (p[i] - p[i - 1]) * (1.0f / l) : dir;
            pos = p[i - 1] + dir * std::min(s, l);
            return;
        }
        s -= l;
    }
    if (!p.empty()) pos = p[0];
}

// ---------------------------------------------------------------------------------------------------- the world
struct StopPlan {
    bool valid = false;
    std::string error;
    V2 p{}, dir{1, 0};
    float y = 0;
    int existing = -1;   // an existing checkpoint (of this mode + kind) under the cursor
};
struct LineEvent { // what a click did, for the host's toast
    enum { None, Refused, Added, Closed, Finished } what = None;
    std::string text;
};

// Why anyone comes: the host sums these from the zones and workplaces every second.
//   People drive in only for a reason: to WORK here (a commuter, who leaves again) or to LIVE here (a mover, who stays) -
//   and a mover needs both a vacant home and a job to be had. Cargo only comes while shops / factories want goods.
struct Demand {
    float homes = 0;      // people the built housing holds
    float jobs = 0;       // workplaces of built commercial, office, industrial lots and transport / industry buildings
    float goods = 0;      // jobs in shops and factories: what the trade wants delivered
    std::vector<V2> homePts, jobPts; // where they head (a sample of the built lots)
};

class TransitWorld {
public:
    std::vector<Stop> stops;
    std::vector<Route> routes;
    std::vector<Outside> outsides;
    std::vector<Vehicle> vehicles;
    std::vector<Citizen> citizens;    // people on foot (capped: they are skinned, animated meshes)
    std::vector<uint64_t> building;   // the line being built (stop ids)
    TMode bMode = TMode::Bus;
    TKind bKind = TKind::Passenger;
    uint64_t version = 1;
    float totalPeople = 0, totalCargo = 0; // arrived through outside connections
    Demand demand;
    float residents = 0, commuters = 0, unemployedShare = 0;
    float jam = 0;                         // 0..1 share of road vehicles crawling
    float sitDur = 2.27f;                  // seconds of the sit-down / stand-up clips (the host sets this from the files)

    // ---- where would a checkpoint of (mode) go with the cursor at p?
    StopPlan PlanStop(const PlaceContext& cx, const TransportWorld& tw, TMode mode, TKind kind, V2 p) const {
        StopPlan r;
        r.p = p, r.y = cx.Ground(p);
        for (size_t i = 0; i < stops.size(); ++i) // an existing one close by
            if (stops[i].alive && stops[i].mode == mode && stops[i].kind == kind && Len(stops[i].p - p) < 9.0f) {
                r.valid = true, r.existing = (int)i, r.p = stops[i].p, r.dir = stops[i].dir, r.y = stops[i].y;
                return r;
            }
        for (size_t i = 0; i < stops.size(); ++i)
            if (stops[i].alive && stops[i].outside >= 0 && OutMode(outsides[stops[i].outside].kind) == mode && Len(stops[i].p - p) < 14.0f) {
                r.valid = true, r.existing = (int)i, r.p = stops[i].p, r.dir = stops[i].dir, r.y = stops[i].y;
                return r;
            }
        switch (mode) {
        case TMode::Bus: case TMode::Tram: case TMode::Train: {
            if (!cx.net) return Fail(r, "No network"), r;
            const bool rail = mode == TMode::Train;
            int best = -1;
            float bt = 0, bd = 30.0f;
            for (size_t s = 0; s < cx.net->segs.size(); ++s) {
                if (!cx.net->segs[s].alive || cx.net->IsRailSeg((int)s) != rail) continue;
                float t, d;
                cx.net->Nearest((int)s, p, t, d);
                if (d < bd) bd = d, bt = t, best = (int)s;
            }
            if (best < 0) return Fail(r, rail ? "Train checkpoints go on a railway" : mode == TMode::Tram ? "Tram checkpoints go beside a road" : "Bus checkpoints go beside a road"), r;
            const Cubic c = cx.net->Curve(best);
            const V2 T = c.Tan(bt);
            r.p = c.At(bt), r.dir = T, r.y = cx.net->YAt(cx.net->segs[best], bt);
            if (rail && kind == TKind::Passenger && !NearBuilding(tw, r.p, Model::TrainStation, 80.0f)) return Fail(r, "Passenger trains stop at a Train Station"), r;
            if (rail && kind == TKind::Cargo && !NearBuilding(tw, r.p, Model::CargoTerminal, 80.0f)) return Fail(r, "Freight trains stop at a Cargo Train Terminal"), r;
            break;
        }
        case TMode::Boat: {
            const bool cargo = kind == TKind::Cargo;
            if (!NearBuilding(tw, p, cargo ? Model::CargoHarbor : Model::PassengerHarbor, 90.0f)) return Fail(r, cargo ? "Cargo boats dock at a Cargo Harbor" : "Ferries dock at a Passenger Harbor"), r;
            r.y = cx.sea > -1e8f ? cx.sea : r.y;
            break;
        }
        case TMode::Plane:
            if (!NearBuilding(tw, p, Model::SmallAirfield, 60.0f) && !NearBuilding(tw, p, Model::RegionalAirport, 60.0f)) return Fail(r, "Planes need an airfield or airport"), r;
            break;
        case TMode::Subway:
            if (cx.IsWater(p)) return Fail(r, "Subway stations go on land"), r;
            if (kind == TKind::Cargo) return Fail(r, "Subways carry passengers only"), r;
            break;
        default: break;
        }
        r.valid = true;
        return r;
    }

    // ---- a click while the line tool is open
    LineEvent Click(const PlaceContext& cx, const TransportWorld& tw, V2 cursor) {
        LineEvent e;
        const StopPlan pl = PlanStop(cx, tw, bMode, bKind, cursor);
        if (!pl.valid) return e.what = LineEvent::Refused, e.text = pl.error, e;
        uint64_t id;
        if (pl.existing >= 0) id = stops[pl.existing].id;
        else id = AddStop(bMode, bKind, pl.p, pl.dir, pl.y, -1);
        if (!building.empty() && id == building.front() && building.size() >= 2) { // the start again: complete as a loop
            Complete(cx, true);
            return e.what = LineEvent::Closed, e.text = "LINE COMPLETE: LOOP", e;
        }
        if (!building.empty() && id == building.back()) return e.what = LineEvent::Refused, e.text = "Already the last checkpoint", e;
        if (std::find(building.begin(), building.end(), id) != building.end()) return e.what = LineEvent::Refused, e.text = "Click the first checkpoint to close the line", e;
        building.push_back(id);
        ++version;
        e.what = LineEvent::Added, e.text = "CHECKPOINT " + std::to_string(building.size());
        return e;
    }
    // Enter: finish an open line (out and back)
    LineEvent Finish(const PlaceContext& cx) {
        LineEvent e;
        if (building.size() < 2) return e.what = LineEvent::Refused, e.text = "A line needs two checkpoints", e;
        Complete(cx, false);
        return e.what = LineEvent::Finished, e.text = "LINE COMPLETE", e;
    }
    void Cancel() {
        if (building.empty()) return;
        PruneOrphans();
        building.clear();
        ++version;
    }

    // ---- outside connections
    // Road / rail ones snap to the end of the nearest road / railway; sea needs water, air floats.
    bool PlanOutside(const PlaceContext& cx, OutKind k, V2 p, V2& at, V2& dir, float& y, std::string& err) const {
        at = p, dir = {1, 0}, y = cx.Ground(p);
        const V2 out = Norm(p); // away from the map's centre
        dir = Len(p) > 1.0f ? out : V2{1, 0};
        switch (k) {
        case OutKind::Road: case OutKind::Rail: {
            if (!cx.net) return err = "No network", false;
            const bool rail = k == OutKind::Rail;
            int bn = -1;
            float bd = 90.0f;
            for (size_t n = 0; n < cx.net->nodes.size(); ++n) {
                if (!cx.net->nodes[n].alive) continue;
                if (rail ? !cx.net->IsRailNode((int)n) : !cx.net->HasRoadSeg((int)n)) continue;
                const float d = Len(XZ(cx.net->nodes[n].p) - p);
                if (d < bd) bd = d, bn = (int)n;
            }
            if (bn < 0) return err = rail ? "Lay a railway first: the connection snaps to a track end" : "Lay a road first: the connection snaps to a road end", false;
            at = XZ(cx.net->nodes[bn].p);
            y = cx.net->NodeY(bn);
            dir = Norm(at);
            break;
        }
        case OutKind::Sea:
            if (!cx.HasWater()) return err = "This map has no sea", false;
            if (!cx.IsWater(p, 1.5f)) return err = "Sea lanes must be on open water", false;
            y = cx.sea;
            break;
        case OutKind::Air:
            y = std::max(cx.Ground(p), cx.HasWater() ? cx.sea : -1e9f) + 140.0f;
            break;
        default: break;
        }
        return true;
    }
    int AddOutside(const PlaceContext& cx, OutKind k, V2 p, const char* name = "") {
        Outside o;
        std::string err;
        if (!PlanOutside(cx, k, p, o.p, o.dir, o.y, err)) return -1;
        for (const Outside& q : outsides)
            if (q.alive && q.kind == k && Len(q.p - o.p) < 25.0f) return -1;
        o.id = nextId_++;
        o.kind = k;
        o.name = name;
        outsides.push_back(o);
        const int i = (int)outsides.size() - 1;
        const TMode m = OutMode(k);
        // a checkpoint for each of passengers and cargo: lines run to it like any other stop
        outsides[i].stop = (int)stops.size();
        const uint64_t sid = AddStop(m, TKind::Passenger, o.p, o.dir, o.y, i);
        (void)sid;
        AddStop(m, TKind::Cargo, o.p, o.dir, o.y, i);
        ++version;
        return i;
    }
    bool RemoveOutside(int i) {
        if (i < 0 || i >= (int)outsides.size() || !outsides[i].alive) return false;
        outsides[i].alive = false;
        for (Stop& s : stops)
            if (s.outside == i) {
                s.alive = false;
                KillRoutesUsing(s.id);
            }
        ++version;
        return true;
    }
    int OutsideAt(V2 p, float r = 18.0f) const {
        for (size_t i = outsides.size(); i-- > 0;)
            if (outsides[i].alive && Len(outsides[i].p - p) < r) return (int)i;
        return -1;
    }
    int StopAt(V2 p, float r = 9.0f) const {
        for (size_t i = stops.size(); i-- > 0;)
            if (stops[i].alive && stops[i].outside < 0 && Len(stops[i].p - p) < r) return (int)i;
        return -1;
    }
    int RouteNear(V2 p, float r = 14.0f) const {
        for (size_t i = routes.size(); i-- > 0;) {
            if (!routes[i].alive) continue;
            for (const auto& leg : routes[i].legs)
                for (size_t k = 1; k < leg.size(); ++k) {
                    const V2 d = leg[k] - leg[k - 1];
                    const float l2 = Dot2(d, d);
                    const float t = l2 > 1e-4f ? std::clamp(Dot2(p - leg[k - 1], d) / l2, 0.0f, 1.0f) : 0.0f;
                    if (Len(leg[k - 1] + d * t - p) < r) return (int)i;
                }
        }
        return -1;
    }
    bool RemoveRoute(int i) {
        if (i < 0 || i >= (int)routes.size() || !routes[i].alive) return false;
        routes[i].alive = false;
        for (Vehicle& v : vehicles)
            if (v.route == i) v.alive = false;
        PruneOrphans();
        ++version;
        return true;
    }
    int RouteCount() const {
        int n = 0;
        for (const Route& r : routes) n += r.alive ? 1 : 0;
        return n;
    }
    int OutsideCount() const {
        int n = 0;
        for (const Outside& o : outsides) n += o.alive ? 1 : 0;
        return n;
    }
    const Stop* FindStop(uint64_t id) const {
        for (const Stop& s : stops)
            if (s.id == id && s.alive) return &s;
        return nullptr;
    }

    // ---- game time: vehicles, arrivals, the flow of people and goods through the outside connections
    // dtSec: game seconds since the last call (already scaled by the clock).
    void Tick(const PlaceContext& cx, float dtSec) {
        if (dtSec <= 0) return;
        dtSec = std::min(dtSec, 5.0f);
        for (size_t r = 0; r < routes.size(); ++r) SpawnLineVehicles(cx, (int)r);
        // who comes, and why. People only arrive for work or to live; with neither on offer the roads stay empty.
        const float employed = std::min(residents * 0.6f, demand.jobs);
        commuters = std::max(0.0f, commuters - commuters * dtSec / 400.0f); // the working day ends: they drive home
        float openJobs = std::max(0.0f, demand.jobs - employed - commuters);
        float vacant = std::max(0.0f, demand.homes - residents);
        unemployedShare = residents > 0 ? std::max(0.0f, 1.0f - demand.jobs / (residents * 0.6f + 1e-3f)) : 0.0f;
        for (Outside& o : outsides) {
            if (!o.alive) continue;
            o.linked = Linked(cx, o);
            if (!o.linked) continue;
            const float rate = o.kind == OutKind::Rail ? 0.9f : o.kind == OutKind::Air ? 0.7f : o.kind == OutKind::Sea ? 0.6f : 0.5f; // the most this link brings per second
            const float cap = rate * dtSec;
            const float comm = std::min(cap * 0.6f, openJobs);                       // to work here
            openJobs -= comm;
            const float mover = openJobs + comm > 0.5f ? std::min(cap * 0.4f, vacant) : 0.0f; // to live here: a home AND work to be had
            vacant -= mover;
            const float cargo = demand.goods > 0 ? std::min(cap * 0.6f, demand.goods * 0.01f * dtSec) : 0.0f;
            commuters += comm, residents += mover;
            o.peopleIn += comm + mover, o.cargoIn += cargo;
            o.peopleOut += comm * 0.9f, o.cargoOut += cargo * 0.5f; // they and the goods go back out
            totalPeople += mover, totalCargo += cargo;
            if (o.kind == OutKind::Air || o.kind == OutKind::Sea) { // no vehicle drives in: the people step off the ship / plane on foot
                o.walkAcc += (comm + mover) * 0.5f;
                if (o.walkAcc >= 1.0f) { o.walkAcc -= 1.0f; SpawnWalkersAt(o.p, o.kind == OutKind::Air ? TMode::Plane : TMode::Boat, mover > comm, 1); }
            }
            o.carAcc += (comm + mover) * 0.25f + cargo * 0.25f; // a vehicle per four people
            if (o.carAcc >= 1.0f && (o.kind == OutKind::Road || o.kind == OutKind::Rail) && vehicles.size() < kMaxVehicles) {
                o.carAcc -= 1.0f;
                SpawnArrival(cx, o, mover > comm, cargo > comm + mover);
            }
        }
        // movement
        int slow = 0, roadN = 0;
        for (size_t i = 0; i < vehicles.size(); ++i) {
            Vehicle& v = vehicles[i];
            if (!v.alive) continue;
            const bool road = v.mode == TMode::Bus || v.mode == TMode::Tram || (v.route < 0 && v.mode == TMode::Bus);
            const std::vector<V2>* poly = nullptr;
            if (v.route >= 0) {
                if (v.route >= (int)routes.size() || !routes[v.route].alive || v.leg >= (int)routes[v.route].legs.size()) { v.alive = false; continue; }
                poly = &routes[v.route].legs[v.leg];
            } else poly = &v.path;
            if (poly->size() < 2) { v.alive = false; continue; }
            const float len = PolyLen(*poly);
            if (v.dwell > 0) { v.dwell -= dtSec, v.speed = 0; continue; }
            float want = TModeSpeed(v.mode);
            if (road) {
                ++roadN;
                // car following: slow behind any vehicle of the same kind of way that is just ahead
                for (size_t j = 0; j < vehicles.size(); ++j) {
                    if (j == i || !vehicles[j].alive || vehicles[j].mode != v.mode) continue;
                    const V2 d = vehicles[j].pos - v.pos;
                    const float dist = Len(d);
                    if (dist < 14.0f && Dot2(d, v.dir) > 0.5f * dist) { want = std::min(want, std::max(0.0f, (dist - 5.0f) * 0.8f)); }
                }
                if (want < TModeSpeed(v.mode) * 0.4f) ++slow;
            }
            v.speed += std::clamp(want - v.speed, -14.0f * dtSec, 3.0f * dtSec);
            v.s += v.speed * dtSec * (v.route >= 0 ? v.dirSign : 1);
            if (v.route >= 0 && v.dirSign < 0) {
                if (v.s <= 0) ArriveAtStop(v, -1);
            } else if (v.s >= len) ArriveAtStop(v, 1);
            if (!v.alive) continue;
            PolyDir(v, *poly);
        }
        TickCitizens(dtSec);
        jam = roadN > 0 ? (float)slow / (float)roadN : 0.0f;
        vehicles.erase(std::remove_if(vehicles.begin(), vehicles.end(), [](const Vehicle& v) { return !v.alive; }), vehicles.end());
    }

    uint64_t NextId() const { return nextId_; }
    void SetNextId(uint64_t n) { nextId_ = n; }
    void Clear() { // a new game or a load starts from nothing
        stops.clear(), routes.clear(), outsides.clear(), vehicles.clear(), building.clear();
        totalPeople = totalCargo = jam = residents = commuters = 0;
        ++version;
    }
    // Replaces what was there with already-built records (the save file): the stop / route / outside vectors are filled by the caller.
    void Rebuild(const PlaceContext& cx) { // after a load or a changed network: recompute every route's legs
        for (Route& r : routes)
            if (r.alive) BuildLegs(cx, r);
    }

private:
    uint64_t nextId_ = 1;
    static constexpr size_t kMaxVehicles = 160;

    static bool Fail(StopPlan& r, const char* m) {
        if (r.error.empty()) r.error = m;
        return false;
    }
    static bool NearBuilding(const TransportWorld& tw, V2 p, Model m, float pad) {
        for (const Placed& b : tw.items) {
            if (!b.alive || b.state != BState::Done || Defs()[b.def].model != m) continue;
            if (TransportWorld::Rect(b).Contains(p, pad)) return true;
        }
        return false;
    }
    uint64_t AddStop(TMode m, TKind k, V2 p, V2 dir, float y, int outside) {
        Stop s;
        s.id = nextId_++;
        s.mode = m, s.kind = k, s.p = p, s.dir = dir, s.y = y, s.outside = outside;
        stops.push_back(s);
        return s.id;
    }
    void PruneOrphans() { // checkpoints of a cancelled line that no route uses
        for (Stop& s : stops) {
            if (!s.alive || s.outside >= 0) continue;
            bool used = std::find(building.begin(), building.end(), s.id) != building.end();
            for (const Route& r : routes)
                if (r.alive && std::find(r.stops.begin(), r.stops.end(), s.id) != r.stops.end()) used = true;
            if (!used) s.alive = false;
        }
    }
    void KillRoutesUsing(uint64_t stop) {
        for (size_t i = 0; i < routes.size(); ++i)
            if (routes[i].alive && std::find(routes[i].stops.begin(), routes[i].stops.end(), stop) != routes[i].stops.end()) RemoveRoute((int)i);
    }
    void Complete(const PlaceContext& cx, bool closed) {
        Route r;
        r.id = nextId_++;
        r.mode = bMode, r.kind = bKind, r.stops = building, r.closed = closed;
        char nm[48];
        std::snprintf(nm, sizeof(nm), "%s %s %d", TKindName(r.kind), TModeName(r.mode), (int)routes.size() + 1);
        r.name = nm;
        BuildLegs(cx, r);
        routes.push_back(r);
        building.clear();
        ++version;
    }
    void BuildLegs(const PlaceContext& cx, Route& r) {
        r.legs.clear(), r.legLen.clear(), r.connected = true;
        const size_t n = r.stops.size(), legs = r.closed ? n : n - 1;
        for (size_t i = 0; i < legs; ++i) {
            const Stop* a = FindStop(r.stops[i]);
            const Stop* b = FindStop(r.stops[(i + 1) % n]);
            std::vector<V2> poly;
            if (!a || !b) { r.legs.push_back({}), r.legLen.push_back(0); r.connected = false; continue; }
            const bool onNet = (r.mode == TMode::Bus || r.mode == TMode::Tram || r.mode == TMode::Train) && cx.net;
            if (!(onNet && FindPath(*cx.net, a->p, b->p, r.mode == TMode::Train, poly))) {
                poly = {a->p, b->p}; // boats, planes, subways go straight; a road line with no road between runs straight and is flagged
                if (onNet) r.connected = false;
            }
            r.legLen.push_back(PolyLen(poly));
            r.legs.push_back(poly);
        }
        float total = 0;
        for (float l : r.legLen) total += l;
        r.vehicles = std::clamp((int)(total / 350.0f) + 1, 1, 6);
    }
    void SpawnLineVehicles(const PlaceContext&, int ri) {
        Route& r = routes[ri];
        if (!r.alive || r.legs.empty()) return;
        int have = 0;
        for (const Vehicle& v : vehicles) have += v.alive && v.route == ri ? 1 : 0;
        if (have >= r.vehicles || vehicles.size() >= kMaxVehicles) return;
        Vehicle v;
        v.id = nextId_++;
        v.route = ri, v.mode = r.mode, v.kind = r.kind;
        v.leg = (int)(((size_t)have * r.legs.size()) / (size_t)std::max(1, r.vehicles)) % (int)r.legs.size(); // spread along the line
        v.s = 0;
        v.y = 0;
        vehicles.push_back(v);
    }
    // A vehicle from the connection to a reason to come: a home (mover), a workplace (commuter), or a shop / factory (freight).
    void SpawnArrival(const PlaceContext& cx, const Outside& o, bool moving, bool freight) {
        if (!cx.net) return;
        const bool rail = o.kind == OutKind::Rail;
        const std::vector<V2>& pts = moving ? demand.homePts : demand.jobPts;
        if (pts.empty()) return;
        for (int tries = 0; tries < 8; ++tries) {
            rng_ = rng_ * 1664525u + 1013904223u;
            const V2 dest = pts[(rng_ >> 8) % pts.size()];
            if (Len(dest - o.p) < 40.0f) continue;
            Vehicle v;
            if (!FindPath(*cx.net, o.p, dest, rail, v.path)) continue;
            v.id = nextId_++;
            v.route = -1, v.mode = rail ? TMode::Train : TMode::Bus;
            v.kind = freight ? TKind::Cargo : TKind::Passenger;
            v.pos = o.p;
            vehicles.push_back(v);
            return;
        }
    }
    static bool Linked(const PlaceContext& cx, const Outside& o) {
        if (o.kind == OutKind::Air || o.kind == OutKind::Sea) return true; // lanes always reach; harbours / airports decide what lands
        return cx.net != nullptr;
    }
    void ArriveAtStop(Vehicle& v, int sign) {
        if (v.route < 0) { // an arrival reached its destination: the passengers step out at the curb and walk in
            v.alive = false;
            if (v.kind == TKind::Passenger && v.path.size() >= 2) {
                rng_ = rng_ * 1664525u + 1013904223u;
                SpawnWalkers(v.path, v.mode, 1 + (int)((rng_ >> 10) % 3));
            }
            return;
        }
        Route& r = routes[v.route];
        const int legs = (int)r.legs.size();
        v.dwell = 6.0f;
        v.speed = 0;
        if (r.closed) {
            v.leg = (v.leg + 1) % legs, v.s = 0;
        } else if (sign > 0) {
            if (v.leg + 1 < legs) v.leg++, v.s = 0;
            else v.dirSign = -1, v.s = PolyLen(r.legs[v.leg]); // turn round at the end of the line
        } else {
            if (v.leg > 0) v.leg--, v.s = PolyLen(r.legs[v.leg]);
            else v.dirSign = 1, v.s = 0;
        }
    }
    void PolyDir(Vehicle& v, const std::vector<V2>&) {
        const std::vector<V2>& p = v.route >= 0 ? routes[v.route].legs[v.leg] : v.path;
        PolyAt(p, std::clamp(v.s, 0.0f, PolyLen(p)), v.pos, v.dir);
        if (v.route >= 0 && v.dirSign < 0) v.dir = v.dir * -1.0f;
    }
    void SpawnWalkers(const std::vector<V2>& drive, TMode from, int n) {
        // from the last road point (the curb) along the final stretch to the door
        const V2 door = drive.back();
        const V2 curb = drive[drive.size() >= 3 ? drive.size() - 2 : 0];
        for (int k = 0; k < n && citizens.size() < kMaxCitizens; ++k) {
            rng_ = rng_ * 1664525u + 1013904223u;
            const float side = ((rng_ >> 8) % 100) * 0.01f * 3.0f - 1.5f;
            const V2 d = door - curb;
            const float L = Len(d);
            const V2 n2 = L > 0.01f ? V2{-d.z / L, d.x / L} : V2{0, 1};
            Citizen c;
            c.id = nextId_++;
            c.variant = (int)((rng_ >> 16) % 12);
            c.from = from;
            c.wait = 1.0f + k * 1.3f + ((rng_ >> 4) % 100) * 0.01f;
            c.path = {curb + n2 * side, door + n2 * side * 0.4f};
            c.len = PolyLen(c.path);
            c.pos = c.path[0];
            if (c.len < 0.5f) continue;
            PolyAt(c.path, 0, c.pos, c.dir);
            citizens.push_back(std::move(c));
        }
    }
    // Sea / air arrivals: from the quay or terminal straight to the nearest place to live or work.
    void SpawnWalkersAt(V2 at, TMode from, bool moving, int n) {
        const std::vector<V2>& pts = moving ? demand.homePts : demand.jobPts;
        float best = 400.0f;
        const V2* pick = nullptr;
        for (const V2& p : pts) { const float d = Len(p - at); if (d > 12.0f && d < best) best = d, pick = &p; }
        if (!pick) return;
        SpawnWalkers({at, *pick}, from, n);
    }
    bool BusAt(const Stop& st) const { // a line vehicle standing at this stop
        for (const Vehicle& v : vehicles)
            if (v.alive && v.route >= 0 && v.dwell > 0 && HasBench(v.mode) && Len(v.pos - st.p) < 12.0f) return true;
        return false;
    }
    void SpawnWaiters(float dt) {
        for (Stop& st : stops) {
            if (!st.alive || st.outside >= 0 || st.kind != TKind::Passenger || !HasBench(st.mode)) continue;
            bool served = false;
            for (const Route& r : routes)
                if (r.alive && std::find(r.stops.begin(), r.stops.end(), st.id) != r.stops.end()) served = true;
            if (!served) continue;
            int used = 0, taken = 0;
            for (const Citizen& c : citizens)
                if (c.alive && c.stopId == st.id) ++used, taken |= 1 << c.seat;
            st.waiterT -= dt;
            if (st.waiterT > 0 || used >= 3 || citizens.size() >= kMaxCitizens) continue;
            rng_ = rng_ * 1664525u + 1013904223u;
            st.waiterT = 10.0f + (float)((rng_ >> 8) % 30);
            int seat = 0;
            while (seat < 3 && (taken >> seat & 1)) ++seat;
            Citizen c;
            c.id = nextId_++;
            c.variant = (int)((rng_ >> 16) % 12);
            c.from = st.mode;
            c.stopId = st.id, c.seat = seat;
            c.patience = 45.0f + (float)((rng_ >> 4) % 90);
            const V2 front = SeatPos(st.p, st.dir, seat) + SeatFacing(st.dir) * 2.0f;
            const float along = ((rng_ >> 12) % 2 ? 1.0f : -1.0f) * (9.0f + (float)((rng_ >> 20) % 8));
            c.path = {front + st.dir * along, front, SeatPos(st.p, st.dir, seat)};
            c.len = PolyLen(c.path);
            c.pos = c.path[0];
            PolyAt(c.path, 0, c.pos, c.dir);
            citizens.push_back(std::move(c));
        }
    }
    void TickWaiter(Citizen& c, float dt) {
        const Stop* st = FindStop(c.stopId);
        if (!st) { c.alive = false; return; }
        const V2 face = SeatFacing(st->dir);
        switch (c.phase) {
        case 0: case 4:
            c.anim = 1;
            c.s += 1.4f * dt;
            if (c.s >= c.len) {
                c.s = c.len;
                if (c.phase == 0) c.phase = 1, c.animT = 0, c.pos = SeatPos(st->p, st->dir, c.seat), c.dir = face;
                else c.alive = false; // got on the bus
            } else PolyAt(c.path, c.s, c.pos, c.dir);
            break;
        case 1:
            c.anim = 2, c.animT += dt, c.dir = face;
            if (c.animT >= sitDur) c.phase = 2, c.stand = 0;
            break;
        case 2:
            c.anim = 3, c.dir = face, c.stand += dt;
            if (BusAt(*st) || c.stand > c.patience) c.phase = 3, c.animT = 0; // the bus is in, or they give up waiting
            break;
        case 3:
            c.anim = 4, c.animT += dt, c.dir = face;
            if (c.animT >= sitDur) {
                c.phase = 4, c.s = 0;
                c.path = {SeatPos(st->p, st->dir, c.seat), st->p + StopSide(st->dir) * 1.6f};
                c.len = PolyLen(c.path);
            }
            break;
        }
    }
    void TickCitizens(float dt) {
        SpawnWaiters(dt);
        for (Citizen& c : citizens) {
            if (!c.alive) continue;
            if (c.stopId) { TickWaiter(c, dt); continue; }
            if (c.wait > 0) { c.wait -= dt; c.anim = 0; continue; }
            if (c.walking) {
                c.s += 1.4f * dt;
                if (c.s >= c.len) { c.s = c.len, c.walking = false; }
                PolyAt(c.path, std::clamp(c.s, 0.0f, c.len), c.pos, c.dir);
                c.anim = c.walking ? 1 : 0;
            } else {
                c.anim = 0;
                c.stand += dt;
                if (c.stand > 2.5f) c.alive = false; // inside
            }
        }
        citizens.erase(std::remove_if(citizens.begin(), citizens.end(), [](const Citizen& c) { return !c.alive; }), citizens.end());
    }
    static constexpr size_t kMaxCitizens = 48;
    uint32_t rng_ = 12345u;
};

} // namespace city
} // namespace tbd
