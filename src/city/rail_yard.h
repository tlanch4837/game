// Trains of the rail yards (transport.h: the Railway Depot's tracks and their track ends, RailPort). Header only, headless
// (tests\rail_yard_test.cpp).
//
//   Fleet       a finished Railway Depot owns kTrainsPerYard trains (its level-1 capacity: Defs().vehicles / 4, 1..4).
//   Spawn       a train only ever appears on the yard's own track, the whole consist inside the lot (it rolls out of the
//               shed), never in the middle of a line: the yard is where trains come from.
//   Route       from a track end that a railway joins, along the connected rail (through switches only: a train turns at
//               most 60 deg at a node, rail_port.h / road_tool.h build them within 35), to the nearest reachable Train
//               Station (it stops on the station's track by the platforms) or Cargo Train Terminal (it runs in on one of
//               the terminal's own tracks). It dwells there, then runs back and into the yard, where it is stabled for a
//               layover before the next run.
//   Speed       30 m/s at most, slower through curves (lateral 0.9 m/s^2: v = sqrt(0.9 R)), on grades and on the yard
//               tracks (8 m/s), braking for the stop ahead.
//   No rail     a yard with no road at its entrance, no railway on any track end, or no station on the rail it reaches
//               runs no trains; its Status says why (the host shows it as a notification).
// Trains are the placeholder box consist (a loco and three cars, transit's train boxes): there is no train model in assets.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "transport.h"

namespace tbd {
namespace city {

constexpr float kTrainMaxSpeed = 30.0f;   // m/s
constexpr float kTrainYardSpeed = 8.0f;   // on the yard's own tracks
constexpr float kTrainAccel = 0.6f, kTrainBrake = 0.9f, kTrainLateral = 0.9f;
constexpr float kTrainDwell = 40.0f;      // game seconds at the station
constexpr float kTrainLayover = 90.0f;    // game seconds stabled in the yard between runs
constexpr int kTrainCars = 4;             // loco + 3
constexpr float kTrainCarLen = 13.0f;     // metres per car (centre to centre)
inline float TrainLength() { return kTrainCars * kTrainCarLen + 6.0f; }
inline int TrainsPerYard(const BuildingDef& d) { return std::clamp(d.vehicles / 4, 1, 4); }

// ---------------------------------------------------------------------------------------------------- the route search
// The shortest way along railways from startNode, leaving it along startDir, to a segment `goal` accepts (it gives the
// stopping point's parameter on it). A train goes on through a node only to a segment within 60 deg of straight on.
inline bool RailPathTo(const Network& net, int startNode, V2 startDir, const std::function<bool(int seg, float& t)>& goal,
                       std::vector<V2>& pts, std::vector<float>& ys) {
    const size_t S = net.segs.size();
    if (startNode < 0 || startNode >= (int)net.nodes.size() || !net.nodes[startNode].alive) return false;
    std::vector<float> len(S, 0.0f), g(S * 2, 1e30f);
    std::vector<int> prev(S * 2, -1);
    for (size_t s = 0; s < S; ++s)
        if (net.segs[s].alive && net.IsRailSeg((int)s)) len[s] = net.Curve((int)s).Length();
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    for (int s : net.nodes[startNode].segs) {
        if (!net.segs[s].alive || !net.IsRailSeg(s) || Dot2(net.OutDir(s, startNode), startDir) < 0.5f) continue;
        const int st = s * 2 + (net.segs[s].a == startNode ? 0 : 1);
        g[st] = 0, pq.push({0.0f, st});
    }
    float best = 1e30f, bestT = 0;
    int bestSt = -1;
    while (!pq.empty()) {
        const auto [d, st] = pq.top();
        pq.pop();
        if (d > g[st] || d >= best) continue;
        const int s = st / 2;
        const bool fwd = (st & 1) == 0;
        float t;
        if (goal(s, t)) {
            const float along = (fwd ? t : 1.0f - t) * len[s];
            if (d + along < best) best = d + along, bestSt = st, bestT = t;
        }
        const int exitN = fwd ? net.segs[s].b : net.segs[s].a;
        const V2 arrive = net.OutDir(s, exitN) * -1.0f;
        for (int s2 : net.nodes[exitN].segs) {
            if (s2 == s || !net.segs[s2].alive || !net.IsRailSeg(s2) || Dot2(net.OutDir(s2, exitN), arrive) < 0.5f) continue;
            const int st2 = s2 * 2 + (net.segs[s2].a == exitN ? 0 : 1);
            if (d + len[s] < g[st2]) g[st2] = d + len[s], prev[st2] = st, pq.push({g[st2], st2});
        }
    }
    if (bestSt < 0) return false;
    std::vector<int> chain;
    for (int st = bestSt; st >= 0; st = prev[st]) chain.push_back(st);
    std::reverse(chain.begin(), chain.end());
    pts.clear(), ys.clear();
    for (size_t i = 0; i < chain.size(); ++i) {
        const int s = chain[i] / 2;
        const bool fwd = (chain[i] & 1) == 0;
        const float t0 = fwd ? 0.0f : 1.0f, t1 = i + 1 == chain.size() ? bestT : (fwd ? 1.0f : 0.0f);
        const Cubic c = net.Curve(s);
        const int n = std::max(2, (int)(std::fabs(t1 - t0) * len[s] / 4.0f));
        for (int k = i ? 1 : 0; k <= n; ++k) {
            const float t = t0 + (t1 - t0) * k / n;
            pts.push_back(c.At(t)), ys.push_back(net.YAt(net.segs[s], t));
        }
    }
    return pts.size() >= 2;
}

// ---------------------------------------------------------------------------------------------------- trains
struct YardTrain {
    uint64_t id = 0, yard = 0;  // the depot it belongs to
    int phase = 0;              // 0 out to the stop, 1 dwelling there, 2 back to the yard, 3 stabled (hidden)
    std::vector<V2> path;       // the current run: on the yard's track, out of its track end, along the rail to the stop
    std::vector<float> ys, cum, limit; // per point: height, metres from the start, speed limit (curves, grades, yard)
    float s = 0, speed = 0, timer = 0;
    V2 spawn{};                 // where its head stood when it appeared (always on the yard's track, inside the lot)
    int destination = -1;       // index into the transport items: the station / terminal it serves
    bool alive = true;
    V2 car[kTrainCars]{}, carDir[kTrainCars]{};
    float carY[kTrainCars]{};
    V2 Head() const { return car[0]; }
    float Length() const { return cum.empty() ? 0.0f : cum.back(); }
};
struct YardStatus {
    uint64_t yard = 0;
    bool road = false;      // a road reaches its entrance
    int railEnds = 0;       // track ends a railway joins
    bool destination = false; // a station / terminal is reachable along the rail
    int trains = 0;         // running now
    std::string Message() const {
        if (!road) return "Rail yard not connected to a road";
        if (!railEnds) return "Rail yard not connected to rail";
        if (!destination) return "Rail yard: no Train Station or Cargo Terminal on its rail";
        return "";
    }
};

class RailYardWorld {
public:
    std::vector<YardTrain> trains;
    std::vector<YardStatus> status; // per finished yard, refreshed on every Tick
    float replanEvery = 5.0f;       // game seconds between the yards' checks

    // dtSec: game seconds. The yards are the finished Railway Depots of tw; the rail is net.
    void Tick(const Network& net, const TransportWorld& tw, float dtSec) {
        if (dtSec <= 0) return;
        dtSec = std::min(dtSec, 5.0f);
        checkT_ -= dtSec;
        if (checkT_ <= 0 || net.version != netVersion_ || tw.version != twVersion_) {
            checkT_ = replanEvery, netVersion_ = net.version, twVersion_ = tw.version;
            Refresh(net, tw);
        }
        for (YardTrain& t : trains) Move(net, tw, t, dtSec);
        trains.erase(std::remove_if(trains.begin(), trains.end(), [](const YardTrain& t) { return !t.alive; }), trains.end());
    }
    const YardStatus* StatusOf(uint64_t yard) const {
        for (const YardStatus& s : status)
            if (s.yard == yard) return &s;
        return nullptr;
    }
    int Running() const {
        int n = 0;
        for (const YardTrain& t : trains) n += t.alive && t.phase != 3;
        return n;
    }
    void Clear() { trains.clear(), status.clear(), checkT_ = 0; }

    // A run from a yard: the train's head starts TrainLength() in from the track's far end (the whole consist on the
    // yard's own track), rolls out of `port` and along the rail to the nearest stop. False when none is reachable.
    static bool PlanRun(const Network& net, const TransportWorld& tw, const Placed& yard, const RailPort& port, YardTrain& t) {
        const int pn = PortNode(net, port);
        if (pn < 0) return false;
        const BuildingDef& D = Defs()[yard.def];
        const std::vector<YardTrack> tr = YardTracks(D);
        if (port.track < 0 || port.track >= (int)tr.size()) return false;
        const V2 side{yard.front.z, -yard.front.x};
        const V2 farEnd = yard.c + side * (port.end ? tr[port.track].x0 : tr[port.track].x1) + yard.front * tr[port.track].z;
        int dest = -1;
        std::vector<V2> rp;
        std::vector<float> ry;
        std::vector<V2> tail; // a terminal: in along its own track
        auto goal = [&](int s, float& tt) {
            for (size_t i = 0; i < tw.items.size(); ++i) {
                const Placed& b = tw.items[i];
                if (!b.alive || b.state != BState::Done || b.id == yard.id) continue;
                const Model m = Defs()[b.def].model;
                if (m == Model::TrainStation) {
                    float d;
                    net.Nearest(s, b.c, tt, d);
                    if (d < 6.0f) return dest = (int)i, true;
                } else if (m == Model::CargoTerminal) {
                    for (const RailPort& q : TransportWorld::PortsOf(&net, b)) {
                        const int qn = PortNode(net, q);
                        if (qn < 0 || (net.segs[s].a != qn && net.segs[s].b != qn)) continue;
                        tt = net.segs[s].a == qn ? 0.0f : 1.0f;
                        tail = {q.p - q.dir * (Defs()[b.def].W() * 0.5f)};
                        return dest = (int)i, true;
                    }
                }
            }
            return false;
        };
        if (!RailPathTo(net, pn, port.dir, goal, rp, ry)) return false;
        // the run: the yard's track from its far end to the track end, the rail, a terminal's own track
        t.path.clear(), t.ys.clear();
        const int inN = std::max(2, (int)(Len(port.p - farEnd) / 6.0f));
        for (int k = 0; k < inN; ++k) t.path.push_back(Lerp2(farEnd, port.p, (float)k / inN)), t.ys.push_back(port.y);
        for (size_t k = 0; k < rp.size(); ++k) t.path.push_back(rp[k]), t.ys.push_back(ry[k]);
        for (const V2& q : tail) t.path.push_back(q), t.ys.push_back(t.ys.back());
        t.destination = dest;
        Limits(t, (float)inN);
        t.s = std::min(TrainLength(), t.Length() * 0.5f);
        t.speed = 0, t.phase = 0, t.timer = 0;
        Place(t);
        t.spawn = t.Head();
        return true;
    }

private:
    float checkT_ = 0;
    uint64_t netVersion_ = 0, twVersion_ = 0, nextId_ = 1;
    unsigned rr_ = 0; // which track end the next train leaves by

    void Refresh(const Network& net, const TransportWorld& tw) {
        status.clear();
        for (const Placed& b : tw.items) {
            if (!b.alive || b.state != BState::Done || Defs()[b.def].model != Model::RailDepot) continue;
            YardStatus st;
            st.yard = b.id;
            V2 at, dir;
            st.road = TransportWorld::EntranceOf(net, b, at, dir);
            std::vector<RailPort> joined;
            for (const RailPort& p : TransportWorld::PortsOf(&net, b))
                if (PortNode(net, p) >= 0) joined.push_back(p);
            st.railEnds = (int)joined.size();
            int have = 0;
            for (const YardTrain& t : trains) have += t.alive && t.yard == b.id;
            if (st.road && !joined.empty()) {
                // a run is planned from each joined end in turn; the first that reaches a stop decides
                for (size_t k = 0; k < joined.size(); ++k) {
                    YardTrain t;
                    const RailPort& p = joined[(rr_ + k) % joined.size()];
                    if (!PlanRun(net, tw, b, p, t)) continue;
                    st.destination = true;
                    if (have < TrainsPerYard(Defs()[b.def]) && !TrackBusy(b.id)) {
                        t.id = nextId_++, t.yard = b.id;
                        trains.push_back(std::move(t));
                        ++have, ++rr_;
                    }
                    break;
                }
            }
            if (!st.road || !st.railEnds) // cut off: its trains are recalled (they vanish into the yard)
                for (YardTrain& t : trains)
                    if (t.yard == b.id) t.alive = false;
            st.trains = have;
            status.push_back(st);
        }
        for (YardTrain& t : trains) // a demolished yard takes its trains
            if (!Yard(tw, t.yard)) t.alive = false;
    }
    static const Placed* Yard(const TransportWorld& tw, uint64_t id) {
        for (const Placed& b : tw.items)
            if (b.id == id && b.alive) return &b;
        return nullptr;
    }
    bool TrackBusy(uint64_t yard) const { // one train leaves at a time: the last one must be clear of the yard first
        for (const YardTrain& t : trains)
            if (t.alive && t.yard == yard && t.phase == 0 && t.s < TrainLength() * 2.0f + 80.0f) return true;
        return false;
    }
    // Speed limits along the path: curves, grades, the yard's own track (the first yardPts points).
    static void Limits(YardTrain& t, float yardPts) {
        const size_t n = t.path.size();
        t.cum.assign(n, 0.0f), t.limit.assign(n, kTrainMaxSpeed);
        for (size_t i = 1; i < n; ++i) t.cum[i] = t.cum[i - 1] + Len(t.path[i] - t.path[i - 1]);
        for (size_t i = 0; i < n; ++i) {
            float v = kTrainMaxSpeed;
            if (i > 0 && i + 1 < n) { // the radius through three points (from about 10 m either side)
                size_t a = i, b = i;
                while (a > 0 && t.cum[i] - t.cum[a] < 10.0f) --a;
                while (b + 1 < n && t.cum[b] - t.cum[i] < 10.0f) ++b;
                const V2 u = t.path[i] - t.path[a], w = t.path[b] - t.path[i];
                const float lu = Len(u), lw = Len(w);
                if (lu > 1e-3f && lw > 1e-3f) {
                    const float turn = std::acos(std::clamp(Dot2(u, w) / (lu * lw), -1.0f, 1.0f));
                    if (turn > 1e-3f) v = std::min(v, std::sqrt(kTrainLateral * (0.5f * (lu + lw)) / turn));
                }
                const float ds = t.cum[b] - t.cum[a];
                if (ds > 1.0f) {
                    const float grade = std::fabs(t.ys[b] - t.ys[a]) / ds;
                    if (grade > 0.01f) v *= std::max(0.55f, 1.0f - grade * 7.0f);
                }
            }
            if ((float)i <= yardPts) v = std::min(v, kTrainYardSpeed);
            t.limit[i] = std::max(3.0f, v);
        }
    }
    // The speed allowed at s: every limit ahead, reached by braking, and a stop at the end of the path.
    static float Allowed(const YardTrain& t) {
        const float end = t.Length();
        float v = std::sqrt(2.0f * kTrainBrake * std::max(0.0f, end - t.s)) + 0.3f;
        for (size_t i = 0; i < t.path.size(); ++i) {
            const float ahead = t.cum[i] - t.s;
            if (ahead < -TrainLength()) continue; // the whole train keeps to a limit until its tail is past
            if (ahead > 900.0f) break;
            v = std::min(v, std::sqrt(t.limit[i] * t.limit[i] + 2.0f * kTrainBrake * std::max(0.0f, ahead)));
        }
        return std::min(v, kTrainMaxSpeed);
    }
    static void At(const YardTrain& t, float s, V2& p, V2& d, float& y) {
        s = std::clamp(s, 0.0f, t.Length());
        const size_t n = t.path.size();
        size_t i = 1;
        while (i + 1 < n && t.cum[i] < s) ++i;
        const float l = t.cum[i] - t.cum[i - 1], f = l > 1e-4f ? (s - t.cum[i - 1]) / l : 0.0f;
        p = Lerp2(t.path[i - 1], t.path[i], f);
        d = l > 1e-4f ? (t.path[i] - t.path[i - 1]) * (1.0f / l) : V2{1, 0};
        y = t.ys[i - 1] + (t.ys[i] - t.ys[i - 1]) * f;
    }
    static void Place(YardTrain& t) {
        for (int k = 0; k < kTrainCars; ++k) At(t, t.s - (kTrainCarLen * 0.5f + k * kTrainCarLen), t.car[k], t.carDir[k], t.carY[k]);
    }
    // Back the way it came: the path reversed (trains run either way), into the yard to the track's far end.
    static void Reverse(YardTrain& t) {
        std::reverse(t.path.begin(), t.path.end());
        std::reverse(t.ys.begin(), t.ys.end());
        std::vector<float> lim(t.limit.rbegin(), t.limit.rend());
        const float L = t.Length();
        t.cum.assign(t.path.size(), 0.0f);
        for (size_t i = 1; i < t.path.size(); ++i) t.cum[i] = t.cum[i - 1] + Len(t.path[i] - t.path[i - 1]);
        t.limit = lim;
        t.s = std::clamp(L - t.s + TrainLength() - 6.0f, 0.0f, L); // the old tail leads now
    }
    void Move(const Network& net, const TransportWorld& tw, YardTrain& t, float dt) {
        if (!t.alive) return;
        switch (t.phase) {
        case 1: // at the stop
            t.timer += dt;
            if (t.timer >= kTrainDwell) Reverse(t), t.phase = 2, t.timer = 0;
            return;
        case 3: { // stabled: the next run when the layover is over (a fresh plan: the rail may have changed)
            t.timer += dt;
            if (t.timer < kTrainLayover) return;
            const Placed* y = Yard(tw, t.yard);
            if (!y || TrackBusy(t.yard)) return;
            std::vector<RailPort> joined;
            for (const RailPort& p : TransportWorld::PortsOf(&net, *y))
                if (PortNode(net, p) >= 0) joined.push_back(p);
            for (size_t k = 0; k < joined.size(); ++k) {
                YardTrain r;
                if (!PlanRun(net, tw, *y, joined[(rr_ + k) % joined.size()], r)) continue;
                r.id = t.id, r.yard = t.yard;
                t = std::move(r);
                ++rr_;
                return;
            }
            t.timer = 0;
            return;
        }
        default: break;
        }
        const float want = Allowed(t);
        t.speed += std::clamp(want - t.speed, -kTrainBrake * 2.0f * dt, kTrainAccel * dt);
        t.speed = std::max(0.0f, t.speed);
        t.s += t.speed * dt;
        if (t.s >= t.Length() - 0.5f) {
            t.s = t.Length(), t.speed = 0, t.timer = 0;
            t.phase = t.phase == 0 ? 1 : 3; // at the stop / home in the yard
        }
        Place(t);
    }
};

} // namespace city
} // namespace tbd
