// Rail junctions: what a rail node is (plain line, turnout left / right, wye, 3-way, diamond crossing, single slip),
// which way a train may go through it, where a new track may join or cross the rail graph (turnout angle and radius
// limits, auto-adjusted when close), crossovers / scissors between parallel tracks, and level crossings with roads.
// Header only, headless (tests\rail_junction_test.cpp). The models of these are rail_junction_mesh.h.
//
//   The graph stays road_network.h's: a junction is a rail node and its arms (the outward directions of its segments).
//   ClassifyArms reads the type from the arms alone, so it needs no stored state and is deterministic:
//     2 arms within 35 deg of straight on         Plain (a bend in the line)
//     3 arms: a stem and two routes off it        Turnout (one route straight on, the other diverges left / right)
//                                                 or Wye (both routes diverge, either side)
//     4 arms: a stem and three routes             ThreeWay (left, straight, right)
//     4 arms: two straight pairs                  Diamond (two lines crossing; trains never change line on it)
//   A Diamond listed in RailJunctions::slips is a SingleSlip: one extra route between the two lines, on one side.
//   RailCanPass is the one rule for train routing through a node (rail_yard.h RailPathTo uses it).
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "rail_port.h"
#include "road_network.h"

namespace tbd {
namespace city {

constexpr float kRailGauge = 1.435f;        // standard gauge, metres between the rails' inner faces
constexpr float kRailMinRadius = 45.0f;     // the road tool's rail curve limit (road_tool.h Validate)
constexpr float kTurnoutMinDeg = 4.0f;      // shallower than this a diverging track overlaps the line (a 1:14 frog)
constexpr float kTurnoutMaxDeg = 35.0f;     // = road_tool.h kRailSwitchCos: the sharpest a train turns through a switch
constexpr float kStraightTolDeg = 3.0f;     // a route this close to straight on is the "straight" route
constexpr float kDiamondMinDeg = 10.0f;     // flatter crossings need impossibly long frogs: use a switch instead
constexpr float kSlipMaxDeg = 20.0f;        // a single slip's turn (its curve lies inside the diamond)
constexpr float kLevelCrossMinDeg = 30.0f;  // rail across a road at grade: at least this angle
constexpr float kLevelCrossClear = 8.0f;    // a level crossing keeps this far from a road junction / rail switch
constexpr float kSwitchGap = 10.0f;         // two switches on one line: at least this far apart
constexpr float kCrossoverMinSpacing = 3.8f, kCrossoverMaxSpacing = 12.0f; // parallel track centres

enum class JType : int { End, Plain, Turnout, Wye, ThreeWay, Diamond, SingleSlip, Crossover, Scissors, LevelCrossing, Invalid, Count };
enum class Hand : int { None, Left, Right };

inline const char* JTypeName(JType t) {
    static const char* const k[] = {"End", "Plain", "Turnout", "Wye", "3-way", "Diamond", "Single slip", "Crossover", "Scissors crossover", "Level crossing", "Invalid"};
    return k[(int)t];
}
inline float Deg(float rad) { return rad * 57.2957795f; }
inline float Rad(float deg) { return deg * 0.0174532925f; }
inline float AngleBetween(V2 a, V2 b) { return Deg(std::acos(std::clamp(Dot2(Norm(a), Norm(b)), -1.0f, 1.0f))); }
inline V2 Rotate2(V2 d, float rad) { // positive: towards LeftOf
    const float c = std::cos(rad), s = std::sin(rad);
    const V2 l = LeftOf(d);
    return d * c + l * s;
}

struct JunctionInfo {
    JType type = JType::Invalid;
    int stem = -1;           // arm index of the stem (Turnout / Wye / ThreeWay), -1 otherwise
    std::vector<int> routes; // Turnout: {straight, diverging}; Wye: {left, right}; ThreeWay: {left, straight, right};
                             // Diamond: {a, partner of a, b, partner of b}
    Hand hand = Hand::None;  // Turnout: the diverging side, looking from the stem
    float angleDeg = 0;      // Turnout / Wye: between the two routes; ThreeWay: the widest leg; Diamond: the crossing
    std::string reason;      // Invalid: why
};

// Turn (degrees) going in along arm i (travelling against arms[i]) and out along arm j.
inline float ArmTurn(const std::vector<V2>& arms, int i, int j) { return AngleBetween(arms[i] * -1.0f, arms[j]); }
inline bool ArmThrough(const std::vector<V2>& arms, int i, int j) { return i != j && ArmTurn(arms, i, j) <= kTurnoutMaxDeg + 0.5f; }

// The side of arm j, looking along the line out of the stem k.
inline Hand ArmSide(const std::vector<V2>& arms, int k, int j) {
    const V2 f = arms[k] * -1.0f;
    return Dot2(arms[j], LeftOf(f)) > 0 ? Hand::Left : Hand::Right;
}

// What a rail node with these arms (outward unit directions of its segments) is. Pure, deterministic.
inline JunctionInfo ClassifyArms(const std::vector<V2>& arms) {
    JunctionInfo J;
    const int n = (int)arms.size();
    auto bad = [&](const char* why) { J.type = JType::Invalid, J.reason = why; return J; };
    if (n <= 1) { J.type = JType::End; return J; }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (AngleBetween(arms[i], arms[j]) < kTurnoutMinDeg - 0.5f) return bad("Track overlaps a track");
    if (n == 2) {
        if (!ArmThrough(arms, 0, 1)) return bad("Track joins need a switch: within 35 deg of the line");
        J.type = JType::Plain, J.angleDeg = ArmTurn(arms, 0, 1);
        return J;
    }
    if (n > 4) return bad("Too many tracks at one switch");
    // a stem: every other arm goes on from it within the switch limit
    for (int k = 0; k < n; ++k) {
        std::vector<int> o;
        for (int j = 0; j < n; ++j)
            if (j != k && ArmThrough(arms, k, j)) o.push_back(j);
        if ((int)o.size() != n - 1) continue;
        J.stem = k;
        if (n == 3) {
            const int a = o[0], b = o[1];
            const float ta = ArmTurn(arms, k, a), tb = ArmTurn(arms, k, b);
            J.angleDeg = AngleBetween(arms[a], arms[b]);
            if (std::min(ta, tb) <= kStraightTolDeg) {
                const int st = ta <= tb ? a : b, dv = ta <= tb ? b : a;
                J.type = JType::Turnout, J.routes = {st, dv}, J.hand = ArmSide(arms, k, dv);
            } else if (ArmSide(arms, k, a) != ArmSide(arms, k, b)) {
                const bool aLeft = ArmSide(arms, k, a) == Hand::Left;
                J.type = JType::Wye, J.routes = {aLeft ? a : b, aLeft ? b : a};
            } else { // both on one side: a curved turnout (the outer route is the "straight" one)
                const int st = ta <= tb ? a : b, dv = ta <= tb ? b : a;
                J.type = JType::Turnout, J.routes = {st, dv}, J.hand = ArmSide(arms, k, dv);
            }
            return J;
        }
        // n == 4: left, straight, right
        int L = -1, S = -1, R = -1;
        for (int j : o) {
            const float t = ArmTurn(arms, k, j);
            if (t <= kStraightTolDeg) S = j;
            else if (ArmSide(arms, k, j) == Hand::Left) L = j;
            else R = j;
        }
        if (L < 0 || S < 0 || R < 0) return bad("A 3-way switch needs a left, a straight and a right track");
        J.type = JType::ThreeWay, J.routes = {L, S, R};
        J.angleDeg = std::max(ArmTurn(arms, k, L), ArmTurn(arms, k, R));
        return J;
    }
    if (n == 4) { // two straight pairs: a diamond
        int p0 = -1;
        for (int j = 1; j < 4; ++j)
            if (ArmTurn(arms, 0, j) <= kStraightTolDeg) p0 = j;
        if (p0 < 0) return bad("Tracks must cross straight: a diamond needs two straight lines");
        int b = -1, pb = -1;
        for (int j = 1; j < 4; ++j)
            if (j != p0) (b < 0 ? b : pb) = j;
        if (ArmTurn(arms, b, pb) > kStraightTolDeg) return bad("Tracks must cross straight: a diamond needs two straight lines");
        float a = AngleBetween(arms[0], arms[b]);
        a = std::min(a, 180.0f - a);
        if (a < kDiamondMinDeg) return bad("Tracks cross too flat: at least 10 deg, or use a switch");
        J.type = JType::Diamond, J.routes = {0, p0, b, pb}, J.angleDeg = a;
        return J;
    }
    return bad("Track joins need a switch: within 35 deg of the line");
}

// ---------------------------------------------------------------------------------------------------- per-node extras
// The one thing geometry cannot tell: a diamond that also has a slip route (segments a and b of the node, on two lines).
struct RailSlip {
    int node = -1, segA = -1, segB = -1;
};
struct RailJunctions {
    std::vector<RailSlip> slips;
    const RailSlip* SlipAt(int node) const {
        for (const RailSlip& s : slips)
            if (s.node == node) return &s;
        return nullptr;
    }
};

// The rail arms of node n: outward directions, and their segments in the same order (the node's own order).
inline std::vector<V2> RailArms(const Network& net, int n, std::vector<int>* segsOut = nullptr) {
    std::vector<V2> arms;
    if (segsOut) segsOut->clear();
    if (n < 0 || n >= (int)net.nodes.size() || !net.nodes[n].alive) return arms;
    for (int s : net.nodes[n].segs) {
        if (!net.segs[s].alive || !net.IsRailSeg(s)) continue;
        arms.push_back(net.OutDir(s, n));
        if (segsOut) segsOut->push_back(s);
    }
    return arms;
}

inline JunctionInfo ClassifyNode(const Network& net, int n, const RailJunctions* extra = nullptr) {
    JunctionInfo J = ClassifyArms(RailArms(net, n));
    if (J.type == JType::Diamond && extra && extra->SlipAt(n)) J.type = JType::SingleSlip;
    return J;
}

// May a train arriving at node n on segment in leave on segment out? Through a switch any route within the switch
// limit (both routes of a turnout / wye / 3-way); across a diamond only straight on (the crossing line is not
// reachable), plus the slip route of a single slip. Unclassifiable nodes keep the old rule (within 60 deg).
inline bool RailCanPass(const Network& net, int n, int in, int out, const RailJunctions* extra = nullptr) {
    if (in == out) return false;
    std::vector<int> ss;
    const std::vector<V2> arms = RailArms(net, n, &ss);
    int i = -1, o = -1;
    for (int k = 0; k < (int)ss.size(); ++k) {
        if (ss[k] == in) i = k;
        if (ss[k] == out) o = k;
    }
    if (i < 0 || o < 0) return false;
    const JunctionInfo J = ClassifyArms(arms);
    if (J.type == JType::Diamond) {
        const int partner = J.routes[0] == i ? J.routes[1] : J.routes[1] == i ? J.routes[0] : J.routes[2] == i ? J.routes[3] : J.routes[2];
        if (o == partner) return true;
        const RailSlip* sl = extra ? extra->SlipAt(n) : nullptr;
        return sl && ((sl->segA == in && sl->segB == out) || (sl->segB == in && sl->segA == out)) && ArmTurn(arms, i, o) <= kSlipMaxDeg + 0.5f;
    }
    if (J.type == JType::Invalid) return ArmTurn(arms, i, o) <= 60.0f;
    return ArmTurn(arms, i, o) <= kTurnoutMaxDeg + 0.5f;
}

// The switch angles the junction layouts are baked for (rail_junction_plane.h JunctionLayouts: turnout_08 / _12 / _20,
// threeway_12). The road tool turns a track leaving a line through a switch to the nearest one, so the junction drawn
// there is the graph's own (SnapSwitchDir). Straight on (within kStraightTolDeg) and sharper than the limit stay as drawn.
constexpr float kSwitchLayoutDeg[3] = {8.0f, 12.0f, 20.0f};
inline V2 SnapSwitchDir(const std::vector<V2>& arms, V2 d) {
    if (arms.size() < 2) return d;
    d = Norm(d);
    int from = -1;
    float least = 1e9f;
    for (int k = 0; k < (int)arms.size(); ++k) {
        const float turn = AngleBetween(arms[(size_t)k] * -1.0f, d);
        if (turn < least) least = turn, from = k;
    }
    if (least <= kStraightTolDeg || least > kTurnoutMaxDeg) return d;
    float want = kSwitchLayoutDeg[0];
    if (arms.size() >= 3) want = 12.0f; // a third route: a 3-way switch (threeway_12)
    else
        for (float a : kSwitchLayoutDeg)
            if (std::fabs(a - least) < std::fabs(want - least)) want = a;
    const V2 line = arms[(size_t)from] * -1.0f;
    const float side = Cross2(line, d) >= 0 ? 1.0f : -1.0f, c = std::cos(Rad(want)), s = std::sin(Rad(want)) * side;
    return Norm({line.x * c - line.z * s, line.x * s + line.z * c});
}

// Turnout geometry: the diverging route leaves the points tangent to the line on a curve of radius R and turns angle a.
// Its lead (points to the end of the curve, along the line) and the side offset there.
inline float TurnoutLead(float angleDeg, float radius) { return radius * std::sin(Rad(angleDeg)); }
inline float TurnoutOffset(float angleDeg, float radius) { return radius * (1.0f - std::cos(Rad(angleDeg))); }
// Clamp a requested turnout into what can be built; true when it had to change. reason says what was wrong.
inline bool AdjustTurnout(float& angleDeg, float& radius, std::string* reason = nullptr) {
    const char* why = nullptr;
    if (angleDeg > kTurnoutMaxDeg) angleDeg = kTurnoutMaxDeg, why = "Switch angle limited to 35 deg";
    if (angleDeg < kTurnoutMinDeg) angleDeg = kTurnoutMinDeg, why = "Switch angle at least 4 deg";
    if (radius < kRailMinRadius) radius = kRailMinRadius, why = "Switch curve radius at least 45 m";
    if (why && reason) *reason = why;
    return why != nullptr;
}

// ---------------------------------------------------------------------------------------------------- crossings
struct RailCrossing {
    JType type = JType::Diamond; // Diamond (rail x rail) or LevelCrossing (rail x road)
    int seg = -1;                // the crossed existing segment
    int rail = -1, road = -1;    // FindLevelCrossings: both segments
    float tNew = 0, tOld = 0;    // parameters on the new curve / the existing segment
    V2 p{};
    float angleDeg = 90;         // acute angle between the two
};

// Where two curves cross (polyline approximation, N spans each): f(ta, tb, point). Ends within `endPad` m skipped.
template <class F>
inline void CurveHits(const Cubic& a, const Cubic& b, int N, float endPad, F&& f) {
    std::vector<V2> pa(N + 1), pb(N + 1);
    for (int i = 0; i <= N; ++i) pa[i] = a.At((float)i / N), pb[i] = b.At((float)i / N);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            const V2 r = pa[i + 1] - pa[i], q = pb[j + 1] - pb[j];
            const float den = Cross2(r, q);
            if (std::fabs(den) < 1e-6f) continue;
            const V2 w = pb[j] - pa[i];
            const float u = Cross2(w, q) / den, v = Cross2(w, r) / den;
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            const V2 P = pa[i] + r * u;
            if (Len(P - a.p0) < endPad || Len(P - a.p3) < endPad) continue;
            f((i + u) / N, (j + v) / N, P);
        }
}
inline float AcuteDeg(V2 a, V2 b) {
    const float d = AngleBetween(a, b);
    return std::min(d, 180.0f - d);
}
// A rail node with three or more rail arms (a switch or a diamond).
inline bool IsRailJunctionNode(const Network& net, int n) { return RailArms(net, n).size() >= 3; }
inline bool IsRoadJunctionNode(const Network& net, int n) {
    int k = 0;
    for (int s : net.nodes[n].segs) k += net.segs[s].alive && !net.IsRailSeg(s);
    return k >= 3;
}

// Checks a new curve (rail when `rail`) against the network before it is built: every same-level crossing it makes.
// Rail x rail: a diamond, at least kDiamondMinDeg and clear of switches. Rail x road: a level crossing, at least
// kLevelCrossMinDeg and kLevelCrossClear from road junctions and rail switches. false + reason when one is impossible.
inline bool CheckCrossings(const Network& net, const Cubic& c, float ya, float yb, bool rail, std::string& reason,
                           std::vector<RailCrossing>* out = nullptr) {
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!net.segs[s].alive) continue;
        const bool otherRail = net.IsRailSeg((int)s);
        if (!rail && !otherRail) continue; // road x road: the road tool's own business
        const Cubic e = net.Curve((int)s);
        bool fail = false;
        CurveHits(c, e, 48, 3.0f, [&](float ta, float tb, V2 P) {
            if (fail) return;
            if (std::fabs(ya + (yb - ya) * ta - net.YAt(net.segs[s], tb)) > 4.0f) return; // a bridge or tunnel passes
            RailCrossing x;
            x.type = rail && otherRail ? JType::Diamond : JType::LevelCrossing;
            x.seg = (int)s, x.tNew = ta, x.tOld = tb, x.p = P, x.angleDeg = AcuteDeg(c.Tan(ta), e.Tan(tb));
            x.rail = otherRail ? (int)s : -1, x.road = otherRail ? -1 : (int)s;
            const float minDeg = x.type == JType::Diamond ? kDiamondMinDeg : kLevelCrossMinDeg;
            if (x.angleDeg < minDeg) {
                reason = x.type == JType::Diamond ? "Tracks cross too flat: at least 10 deg, or use a switch" : "Rail must cross a road at 30 deg or more";
                fail = true;
                return;
            }
            const float clear = x.type == JType::Diamond ? kSwitchGap * 0.5f : kLevelCrossClear;
            for (int end : {net.segs[s].a, net.segs[s].b}) {
                const bool junction = otherRail ? IsRailJunctionNode(net, end) : IsRoadJunctionNode(net, end);
                if (junction && Len(XZ(net.nodes[end].p) - P) < clear) {
                    reason = x.type == JType::Diamond ? "Crossing too close to a switch" : "Level crossing too close to a junction";
                    fail = true;
                    return;
                }
            }
            if (out) out->push_back(x);
        });
        if (fail) return false;
    }
    return true;
}

// Every level crossing in the network (rail segment x road segment at the same level), in a deterministic order.
inline std::vector<RailCrossing> FindLevelCrossings(const Network& net) {
    std::vector<RailCrossing> v;
    for (size_t r = 0; r < net.segs.size(); ++r) {
        if (!net.segs[r].alive || !net.IsRailSeg((int)r)) continue;
        const Cubic c = net.Curve((int)r);
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || net.IsRailSeg((int)s)) continue;
            const Cubic e = net.Curve((int)s);
            CurveHits(c, e, 32, 0.0f, [&](float ta, float tb, V2 P) {
                if (std::fabs(net.YAt(net.segs[r], ta) - net.YAt(net.segs[s], tb)) > 4.0f) return;
                RailCrossing x;
                x.type = JType::LevelCrossing, x.rail = (int)r, x.road = (int)s, x.seg = (int)s;
                x.tNew = ta, x.tOld = tb, x.p = P, x.angleDeg = AcuteDeg(c.Tan(ta), e.Tan(tb));
                v.push_back(x);
            });
        }
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------- joining (snap)
// A new track's end at p, the track leaving it along dir. PlanJoin finds what it would join (a free rail yard track
// end, a rail node, or the middle of a rail segment, nearest first within `reach`), what junction that makes, and
// auto-adjusts the leaving direction when it is just outside the switch limits (up to kAdjustDeg more).
constexpr float kAdjustDeg = 15.0f;
struct JoinPlan {
    bool ok = false, snapped = false, adjusted = false;
    JType type = JType::End;  // what the joined node becomes (Plain: the line carried on)
    Hand hand = Hand::None;
    int node = -1, seg = -1, port = -1;
    float t = 0;              // on seg
    V2 at{}, dir{1, 0};       // the join point and the (adjusted) leaving direction
    float angleDeg = 0;       // the switch angle
    std::string reason;
};

// Rotate d so that it turns at most maxDeg away from straight on along `line` (d and line in the same sense).
inline V2 ClampTurn(V2 line, V2 d, float maxDeg) {
    const float a = AngleBetween(line, d);
    if (a <= maxDeg) return d;
    const float side = Cross2(line, d) >= 0 ? 1.0f : -1.0f;
    const float c = std::cos(Rad(maxDeg)), s = std::sin(Rad(maxDeg)) * side;
    return Norm({line.x * c - line.z * s, line.x * s + line.z * c});
}

inline JoinPlan PlanJoin(const Network& net, const std::vector<RailPort>& ports, V2 p, V2 dir, float reach = 6.0f) {
    JoinPlan J;
    J.at = p, J.dir = Norm(dir);
    const int rail = ProfileIndex("Single Track Railway");
    float bestD = reach;
    int port = -1, node = -1, seg = -1;
    float segT = 0;
    for (size_t i = 0; i < ports.size(); ++i)
        if (PortNode(net, ports[i]) < 0 && Len(ports[i].p - p) < bestD) bestD = Len(ports[i].p - p), port = (int)i;
    const int nn = net.NearestNode(p, reach, -1, rail);
    if (nn >= 0 && net.IsRailNode(nn) && Len(XZ(net.nodes[nn].p) - p) < bestD) bestD = Len(XZ(net.nodes[nn].p) - p), node = nn, port = -1;
    if (node < 0 && port < 0) {
        float t, d;
        const int s = net.SegmentAt(p, reach, t, d, rail);
        if (s >= 0 && d < reach) {
            const float len = net.Curve(s).Length();
            if (t * len < 1.0f) node = net.segs[s].a;
            else if ((1 - t) * len < 1.0f) node = net.segs[s].b;
            else seg = s, segT = t;
        }
    }
    std::vector<V2> arms;
    if (port >= 0) { // out of the yard along its track: straight on only
        J.snapped = true, J.port = port, J.at = ports[port].p;
        const float a = AngleBetween(ports[port].dir, J.dir);
        if (a > kTurnoutMaxDeg + kAdjustDeg) { J.reason = "Leave the rail yard along its track: within 35 deg"; return J; }
        J.adjusted = a > 0.5f, J.dir = ports[port].dir, J.type = JType::Plain, J.ok = true;
        return J;
    }
    if (node >= 0) J.snapped = true, J.node = node, J.at = XZ(net.nodes[node].p), arms = RailArms(net, node);
    else if (seg >= 0) {
        J.snapped = true, J.seg = seg, J.t = segT;
        const Cubic c = net.Curve(seg);
        J.at = c.At(segT), arms = {c.Tan(segT), c.Tan(segT) * -1.0f};
    } else { J.ok = true; return J; } // free end: nothing to join
    // the line the new track leaves from: the arm it turns least from
    int from = -1;
    float least = 1e9f;
    for (int k = 0; k < (int)arms.size(); ++k) {
        const float turn = AngleBetween(arms[k] * -1.0f, J.dir);
        if (turn < least) least = turn, from = k;
    }
    if (least > kTurnoutMaxDeg + kAdjustDeg) { J.reason = "Track joins need a switch: within 35 deg of the line"; return J; }
    if (least > kTurnoutMaxDeg) J.dir = ClampTurn(arms[from] * -1.0f, J.dir, kTurnoutMaxDeg), J.adjusted = true;
    else if (arms.size() >= 2 && least > kStraightTolDeg && least < kTurnoutMinDeg)
        J.dir = Rotate2(arms[from] * -1.0f, Rad(kTurnoutMinDeg) * (Cross2(arms[from] * -1.0f, J.dir) >= 0 ? -1.0f : 1.0f)), J.adjusted = true;
    // switches on the same line keep apart
    for (size_t i = 0; i < net.nodes.size(); ++i) {
        if ((int)i == node || !net.nodes[i].alive || !IsRailJunctionNode(net, (int)i)) continue;
        if (Len(XZ(net.nodes[i].p) - J.at) < kSwitchGap) { J.reason = "Too close to another switch"; return J; }
    }
    arms.push_back(J.dir);
    const JunctionInfo info = ClassifyArms(arms);
    if (info.type == JType::Invalid) { J.reason = info.reason; return J; }
    J.type = info.type, J.hand = info.hand, J.angleDeg = info.angleDeg;
    J.ok = true;
    return J;
}

// ---------------------------------------------------------------------------------------------------- crossovers
// Two parallel tracks (separate segments) joined by a diagonal: a turnout on each, the diagonal leaving both at angleDeg.
// A scissors crossover has two diagonals that cross in a diamond halfway (four turnouts and the diamond).
struct CrossoverResult {
    bool ok = false;
    std::string reason;
    std::vector<int> points;   // the switch nodes: {on A, on B} (scissors: {A1, B2, B1, A2})
    int diamond = -1;          // scissors: the crossing node
    float spacing = 0, length = 0; // track centres apart, and along the track from first to last switch
};

// A crossover's diagonal: out of the points on a curve (R, angle), straight, and back onto the other track on a mirror
// curve. The steepest angle two curves of radius R fit between tracks `spacing` apart, and the length along the tracks.
inline float CrossoverMaxAngle(float spacing, float R) { return Deg(std::acos(std::clamp(1.0f - spacing / (2.0f * R), -1.0f, 1.0f))); }
inline float CrossoverRun(float spacing, float angleDeg, float R) {
    angleDeg = std::min(angleDeg, CrossoverMaxAngle(spacing, R));
    const float a = Rad(angleDeg), o = TurnoutOffset(angleDeg, R);
    const float D = std::max(0.0f, spacing - 2.0f * o) / std::max(1e-3f, std::sin(a));
    return 2.0f * TurnoutLead(angleDeg, R) + D * std::cos(a);
}

inline int SplitNear(Network& net, int seg, V2 p) {
    float t, d;
    net.Nearest(seg, p, t, d);
    return net.Split(seg, t);
}

inline CrossoverResult BuildCrossover(Network& net, int segA, int segB, float tA, float angleDeg, bool scissors, float radius = 150.0f) {
    CrossoverResult R;
    if (segA < 0 || segB < 0 || segA == segB || !net.segs[segA].alive || !net.segs[segB].alive || !net.IsRailSeg(segA) || !net.IsRailSeg(segB)) {
        R.reason = "Pick two tracks";
        return R;
    }
    if (angleDeg > kTurnoutMaxDeg || angleDeg < kTurnoutMinDeg) { R.reason = "Crossover angle must be 4 to 35 deg"; return R; }
    const Cubic cA = net.Curve(segA), cB = net.Curve(segB);
    const V2 P1 = cA.At(tA), fA = cA.Tan(tA);
    float tB, d;
    net.Nearest(segB, P1, tB, d);
    if (AcuteDeg(fA, cB.Tan(tB)) > kStraightTolDeg) { R.reason = "Crossovers need parallel tracks"; return R; }
    if (d < kCrossoverMinSpacing || d > kCrossoverMaxSpacing) { R.reason = "Parallel tracks must be 3.8 to 12 m apart"; return R; }
    const float Lx = CrossoverRun(d, angleDeg, std::max(radius, kRailMinRadius)); // the model's length (rail_junction_mesh.h)
    if (Deg(std::atan2(d, Lx)) < kTurnoutMinDeg) { R.reason = "Crossover too flat: a tighter curve or wider spacing"; return R; }
    const float lenA = cA.Length();
    if (tA * lenA < kSwitchGap * 0.5f || (1 - tA) * lenA < Lx + kSwitchGap * 0.5f) { R.reason = "Not enough straight track for a crossover"; return R; }
    const V2 P3 = P1 + fA * Lx; // further along A
    float tb2, d2, tb4, d4;
    net.Nearest(segB, P3, tb2, d2), net.Nearest(segB, P1, tb4, d4);
    const V2 P2 = cB.At(tb2), P4 = cB.At(tb4);
    if (tb2 < 0.001f || tb2 > 0.999f || tb4 < 0.001f || tb4 > 0.999f) { R.reason = "Not enough straight track for a crossover"; return R; }
    for (V2 q : {P1, P2, P3, P4})
        for (size_t i = 0; i < net.nodes.size(); ++i)
            if (net.nodes[i].alive && IsRailJunctionNode(net, (int)i) && Len(XZ(net.nodes[i].p) - q) < kSwitchGap) { R.reason = "Too close to another switch"; return R; }
    const int prof = net.segs[segA].profile;
    // split each track at the far point first: the kept (a-side) piece still holds the near point
    const bool bFwd = Dot2(cB.Tan(tb2), fA) > 0; // B runs the same way as A
    int a1, a3 = -1, b2, b4 = -1;
    if (scissors) a3 = SplitNear(net, segA, P3);
    a1 = SplitNear(net, segA, P1);
    if (!scissors) b2 = SplitNear(net, segB, P2);
    else if (bFwd) b2 = SplitNear(net, segB, P2), b4 = SplitNear(net, segB, P4);
    else b4 = SplitNear(net, segB, P4), b2 = SplitNear(net, segB, P2);
    R.spacing = d, R.length = Lx, R.ok = true;
    auto pos = [&](int n) { return XZ(net.nodes[n].p); };
    if (!scissors) {
        net.AddSeg(a1, b2, Cubic::Line(pos(a1), pos(b2)), prof);
        R.points = {a1, b2};
        return R;
    }
    V2 X;
    LineHit(pos(a1), pos(b2) - pos(a1), pos(b4), pos(a3) - pos(b4), X);
    const float y = 0.5f * (net.NodeY(a1) + net.NodeY(b2));
    const int x = net.AddNode({X.x, y, X.z}, net.nodes[a1].grounded && net.nodes[b2].grounded);
    net.AddSeg(a1, x, Cubic::Line(pos(a1), X), prof), net.AddSeg(x, b2, Cubic::Line(X, pos(b2)), prof);
    net.AddSeg(b4, x, Cubic::Line(pos(b4), X), prof), net.AddSeg(x, a3, Cubic::Line(X, pos(a3)), prof);
    R.points = {a1, b2, b4, a3}, R.diamond = x;
    return R;
}

// A single slip on diamond node n between segments a and b (on the two lines, the turn within kSlipMaxDeg).
inline bool AddSlip(RailJunctions& J, const Network& net, int n, int a, int b, std::string* reason = nullptr) {
    std::vector<int> ss;
    const std::vector<V2> arms = RailArms(net, n, &ss);
    const JunctionInfo info = ClassifyArms(arms);
    auto no = [&](const char* why) { if (reason) *reason = why; return false; };
    if (info.type != JType::Diamond) return no("A slip goes on a diamond crossing");
    if (J.SlipAt(n)) return no("This crossing already has a slip");
    const int i = (int)(std::find(ss.begin(), ss.end(), a) - ss.begin()), o = (int)(std::find(ss.begin(), ss.end(), b) - ss.begin());
    if (i >= (int)ss.size() || o >= (int)ss.size()) return no("Pick two tracks of the crossing");
    if (ArmTurn(arms, i, o) > kSlipMaxDeg + 0.5f) return no("A slip needs a crossing of 20 deg or less");
    J.slips.push_back({n, a, b});
    return true;
}

// Is there a way by train from segment `from` (starting in either direction) to segment `to`?
// Directed search over (segment, direction) using RailCanPass. Deterministic; for tests and the tool's warnings.
inline bool RailReachable(const Network& net, int from, int to, const RailJunctions* extra = nullptr) {
    const size_t S = net.segs.size();
    if (from < 0 || to < 0 || from >= (int)S || to >= (int)S) return false;
    std::vector<char> seen(S * 2, 0);
    std::vector<int> q = {from * 2, from * 2 + 1};
    seen[from * 2] = seen[from * 2 + 1] = 1;
    for (size_t h = 0; h < q.size(); ++h) {
        const int s = q[h] / 2;
        if (s == to) return true;
        const int exitN = (q[h] & 1) == 0 ? net.segs[s].b : net.segs[s].a;
        for (int s2 : net.nodes[exitN].segs) {
            if (!net.segs[s2].alive || !net.IsRailSeg(s2) || !RailCanPass(net, exitN, s, s2, extra)) continue;
            const int st = s2 * 2 + (net.segs[s2].a == exitN ? 0 : 1);
            if (!seen[st]) seen[st] = 1, q.push_back(st);
        }
    }
    return false;
}

} // namespace city
} // namespace tbd
