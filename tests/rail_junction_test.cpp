// Headless checks for the rail junctions (src/city/rail_junction.h, rail_junction_mesh.h):
//   ClassifyArms: turnout left / right (straight and diverging route), wye, 3-way, diamond; refusals (too sharp, too
//   flat, overlapping, too many tracks); angle limits and auto-adjust (AdjustTurnout, PlanJoin onto a node, a segment's
//   middle, a rail yard track end; too close to another switch);
//   routing (RailCanPass / RailReachable): a train takes either route of a turnout / wye / 3-way, never reverses
//   through the points, crosses a diamond straight and never changes line on it (also at 30 deg, which the old 60 deg
//   rule let through), a single slip adds its one route; crossovers and scissors connect both tracks;
//   level crossings (rail x road found, minimum angle, clear of junctions), diamonds too flat refused;
//   the models: every catalog variant builds, LOD 1 is lighter, at most five draws, deterministic, frogs where rails
//   cross, parameters clamped to the network's limits. Prints the triangle table.
// Build and run: tests\rail_junction_test.bat
#include <cmath>
#include <cstdio>
#include <cstring>

#include "city/rail_junction_place.h"
#include "city/rail_yard.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static V2 Dir(float deg) { return {std::cos(Rad(deg)), std::sin(Rad(deg))}; }
static const int kRail = ProfileIndex("Single Track Railway");
static const int kRoad = ProfileIndex("Two-Lane Road");

static int MkNode(Network& n, V2 p) { return n.AddNode({p.x, 0, p.z}, false); }
static int Line(Network& n, int a, int b, int prof = kRail) {
    return n.AddSeg(a, b, Cubic::Line(XZ(n.nodes[a].p), XZ(n.nodes[b].p)), prof);
}

// A star: centre node with straight arms to the given directions (degrees, x towards z), 60 m long.
struct Star {
    Network net;
    int centre = -1;
    std::vector<int> segs;
};
static Star MakeStar(std::initializer_list<float> degs) {
    Star s;
    s.centre = MkNode(s.net, {0, 0});
    for (float d : degs) s.segs.push_back(Line(s.net, s.centre, MkNode(s.net, Dir(d) * 60.0f)));
    return s;
}

static void TestClassify() {
    // turnout: stem west (180), straight east (0), diverging 12 deg to +z (right of east)
    {
        const JunctionInfo J = ClassifyArms({Dir(180), Dir(0), Dir(12)});
        CHECK(J.type == JType::Turnout && J.stem == 0 && J.routes.size() == 2 && J.routes[0] == 1 && J.routes[1] == 2);
        CHECK(J.hand == Hand::Right && std::fabs(J.angleDeg - 12) < 0.1f);
        const JunctionInfo L = ClassifyArms({Dir(0), Dir(-12), Dir(180)});
        CHECK(L.type == JType::Turnout && L.stem == 2 && L.hand == Hand::Left);
    }
    // wye: both legs diverge (+-8)
    {
        const JunctionInfo J = ClassifyArms({Dir(180), Dir(8), Dir(-8)});
        CHECK(J.type == JType::Wye && J.stem == 0 && std::fabs(J.angleDeg - 16) < 0.1f);
        CHECK(J.routes[0] == 2 && J.routes[1] == 1); // left (-z) first
    }
    // 3-way
    {
        const JunctionInfo J = ClassifyArms({Dir(-12), Dir(0), Dir(12), Dir(180)});
        CHECK(J.type == JType::ThreeWay && J.stem == 3 && J.routes[0] == 0 && J.routes[1] == 1 && J.routes[2] == 2);
    }
    // diamonds at 90 and 30 deg
    for (float a : {90.0f, 30.0f}) {
        const JunctionInfo J = ClassifyArms({Dir(0), Dir(180), Dir(a), Dir(a + 180)});
        CHECK(J.type == JType::Diamond && std::fabs(J.angleDeg - a) < 0.2f);
    }
    // refusals
    CHECK(ClassifyArms({Dir(180), Dir(0), Dir(45)}).type == JType::Invalid);              // diverging too sharp
    CHECK(ClassifyArms({Dir(0), Dir(180), Dir(6), Dir(186)}).type == JType::Invalid);     // diamond too flat
    CHECK(ClassifyArms({Dir(180), Dir(0), Dir(1)}).type == JType::Invalid);               // overlapping
    CHECK(ClassifyArms({Dir(180), Dir(0), Dir(10), Dir(-10), Dir(20)}).type == JType::Invalid); // five tracks
    CHECK(ClassifyArms({Dir(0), Dir(180)}).type == JType::Plain);
    CHECK(ClassifyArms({Dir(0), Dir(120)}).type == JType::Invalid); // a kink: no train turns 60 deg
    CHECK(ClassifyArms({Dir(0)}).type == JType::End);
    // limits
    float a = 50, r = 30;
    CHECK(AdjustTurnout(a, r) && a == kTurnoutMaxDeg && r == kRailMinRadius);
    a = 12, r = 150;
    CHECK(!AdjustTurnout(a, r));
    CHECK(std::fabs(TurnoutLead(12, 150) - 150 * std::sin(Rad(12))) < 1e-3f);
}

static void TestPlanJoin() {
    Network net;
    const int a = MkNode(net, {0, 0}), b = MkNode(net, {200, 0});
    Line(net, a, b);
    std::vector<RailPort> none;
    // onto the middle of the line, leaving at 20 deg: a right-hand turnout
    JoinPlan J = PlanJoin(net, none, {100, 0.5f}, Dir(20));
    CHECK(J.ok && J.snapped && J.seg >= 0 && J.type == JType::Turnout && J.hand == Hand::Right && !J.adjusted);
    CHECK(std::fabs(J.at.z) < 0.05f && std::fabs(J.angleDeg - 20) < 0.5f);
    // 45 deg: auto-adjusted to the 35 deg limit
    J = PlanJoin(net, none, {100, 0}, Dir(-45));
    CHECK(J.ok && J.adjusted && J.hand == Hand::Left && std::fabs(AngleBetween(J.dir, Dir(0)) - kTurnoutMaxDeg) < 0.2f);
    // 70 deg: refused
    J = PlanJoin(net, none, {100, 0}, Dir(70));
    CHECK(!J.ok && !J.reason.empty());
    // 2 deg: nearly on the line, refused as overlapping (and 3.5 deg rounds up to the 4 deg minimum)
    J = PlanJoin(net, none, {100, 0}, Dir(2));
    CHECK(!J.ok);
    // the line's end: carried on (Plain) within 35 deg, refused beyond the adjust range
    J = PlanJoin(net, none, {200.5f, 0}, Dir(10));
    CHECK(J.ok && J.node == b && J.type == JType::Plain);
    J = PlanJoin(net, none, {200.5f, 0}, Dir(90));
    CHECK(!J.ok);
    // build the turnout, then a second switch 6 m on is refused, 30 m on is fine
    const int m = net.Split(0, 0.5f);
    const int d = MkNode(net, Dir(12) * 80.0f + V2{100, 0});
    Line(net, m, d);
    CHECK(ClassifyNode(net, m).type == JType::Turnout);
    J = PlanJoin(net, none, {107, 0}, Dir(-15));
    CHECK(!J.ok && J.reason == "Too close to another switch");
    J = PlanJoin(net, none, {130, 0}, Dir(-15));
    CHECK(J.ok && J.type == JType::Turnout && J.hand == Hand::Left);
    // the turnout's node itself: a third route makes a 3-way
    J = PlanJoin(net, none, XZ(net.nodes[m].p), Dir(-12));
    CHECK(J.ok && J.node == m && J.type == JType::ThreeWay);
    // a rail yard track end: straight out of the yard, the direction snapped to the track
    RailPort port;
    port.p = {0, 50}, port.dir = {1, 0};
    J = PlanJoin(net, {port}, {0.5f, 50.5f}, Dir(20));
    CHECK(J.ok && J.port == 0 && J.type == JType::Plain && J.adjusted && Len(J.dir - V2{1, 0}) < 1e-4f);
    J = PlanJoin(net, {port}, {0.5f, 50.5f}, Dir(80));
    CHECK(!J.ok);
}

static void TestRouting() {
    // turnout: stem 0 (west), straight 1 (east), diverging 2 (12 deg right)
    {
        Star s = MakeStar({180, 0, 12});
        CHECK(ClassifyNode(s.net, s.centre).type == JType::Turnout);
        CHECK(RailCanPass(s.net, s.centre, s.segs[0], s.segs[1]) && RailCanPass(s.net, s.centre, s.segs[0], s.segs[2]));
        CHECK(RailCanPass(s.net, s.centre, s.segs[2], s.segs[0]) && RailCanPass(s.net, s.centre, s.segs[1], s.segs[0]));
        CHECK(!RailCanPass(s.net, s.centre, s.segs[1], s.segs[2])); // no reversing through the points
        CHECK(RailReachable(s.net, s.segs[0], s.segs[1]) && RailReachable(s.net, s.segs[0], s.segs[2]));
        CHECK(!RailReachable(s.net, s.segs[1], s.segs[2]));
    }
    // wye and 3-way: every leg from the stem, no leg to leg
    {
        Star w = MakeStar({180, 9, -9});
        CHECK(ClassifyNode(w.net, w.centre).type == JType::Wye);
        CHECK(RailReachable(w.net, w.segs[0], w.segs[1]) && RailReachable(w.net, w.segs[0], w.segs[2]) && !RailReachable(w.net, w.segs[1], w.segs[2]));
        Star t = MakeStar({180, -12, 0, 12});
        CHECK(ClassifyNode(t.net, t.centre).type == JType::ThreeWay);
        for (int k = 1; k < 4; ++k) CHECK(RailReachable(t.net, t.segs[0], t.segs[k]));
        CHECK(!RailReachable(t.net, t.segs[1], t.segs[3]));
    }
    // diamonds: straight over, never onto the crossing line (30 deg would pass the old 60 deg rule)
    for (float ang : {90.0f, 30.0f}) {
        Star s = MakeStar({180, 0, ang, ang + 180});
        CHECK(ClassifyNode(s.net, s.centre).type == JType::Diamond);
        CHECK(RailCanPass(s.net, s.centre, s.segs[0], s.segs[1]) && RailCanPass(s.net, s.centre, s.segs[2], s.segs[3]));
        CHECK(!RailCanPass(s.net, s.centre, s.segs[0], s.segs[2]) && !RailCanPass(s.net, s.centre, s.segs[3], s.segs[1]));
        CHECK(RailReachable(s.net, s.segs[0], s.segs[1]) && !RailReachable(s.net, s.segs[0], s.segs[2]) && !RailReachable(s.net, s.segs[1], s.segs[3]));
        // single slip: only at a flat crossing, adds one route (west arm -> the 15 deg line's far arm)
        RailJunctions extra;
        std::string why;
        CHECK(AddSlip(extra, s.net, s.centre, s.segs[0], s.segs[2], &why) == false);
    }
    {
        Star s = MakeStar({180, 0, 15, 195});
        RailJunctions extra;
        CHECK(AddSlip(extra, s.net, s.centre, s.segs[0], s.segs[2]));
        CHECK(ClassifyNode(s.net, s.centre, &extra).type == JType::SingleSlip);
        CHECK(RailReachable(s.net, s.segs[0], s.segs[2], &extra) && !RailReachable(s.net, s.segs[0], s.segs[2]));
        CHECK(!RailReachable(s.net, s.segs[3], s.segs[1], &extra)); // the other side has no slip
        CHECK(!AddSlip(extra, s.net, s.centre, s.segs[1], s.segs[3])); // one slip per crossing
    }
}

static void TestCrossovers() {
    for (int sc = 0; sc < 2; ++sc) {
        Network net;
        const int a0 = MkNode(net, {0, 0}), a1 = MkNode(net, {300, 0}), b0 = MkNode(net, {0, 5}), b1 = MkNode(net, {300, 5});
        const int A = Line(net, a0, a1), B = Line(net, b0, b1);
        CHECK(!RailReachable(net, A, B));
        const CrossoverResult R = BuildCrossover(net, A, B, 0.3f, 10.0f, sc == 1);
        CHECK(R.ok);
        if (!R.ok) { std::printf("  crossover: %s\n", R.reason.c_str()); continue; }
        CHECK(std::fabs(R.spacing - 5) < 0.05f && std::fabs(R.length - CrossoverRun(5, 10, 150)) < 0.01f);
        CHECK(R.points.size() == (sc ? 4u : 2u));
        for (int p : R.points) CHECK(ClassifyNode(net, p).type == JType::Turnout);
        // A and B now end at the switches: their east halves hold the far ends
        auto segAt = [&](int node) {
            for (int s : net.nodes[node].segs)
                if (net.segs[s].alive) return s;
            return -1;
        };
        const int aEast = segAt(a1), bEast = segAt(b1);
        CHECK(RailReachable(net, A, bEast) && RailReachable(net, bEast, A)); // west on A over to east on B, and back
        if (sc) CHECK(RailReachable(net, B, aEast) && RailReachable(net, aEast, B)); // the scissors' second diagonal
        else CHECK(!RailReachable(net, B, aEast)); // a single crossover is only one way round (no reversing)
        if (sc) {
            CHECK(R.diamond >= 0 && ClassifyNode(net, R.diamond).type == JType::Diamond);
            // over the scissors' diamond a train stays on its diagonal
            std::vector<int> ds;
            RailArms(net, R.diamond, &ds);
            CHECK(ds.size() == 4);
            int straightPairs = 0;
            for (int i : ds)
                for (int j : ds) straightPairs += RailCanPass(net, R.diamond, i, j);
            CHECK(straightPairs == 4); // each of the 4 arms passes only to its partner
        }
        // the crossover's switches are spaced and too close a second one is refused
        CHECK(!BuildCrossover(net, A, B, 0.31f, 10.0f, false).ok);
    }
    // not parallel / too far apart / too wide an angle
    Network net;
    const int A = Line(net, MkNode(net, {0, 0}), MkNode(net, {300, 0})), B = Line(net, MkNode(net, {0, 30}), MkNode(net, {300, 30}));
    CHECK(!BuildCrossover(net, A, B, 0.3f, 10, false).ok);
    CHECK(!BuildCrossover(net, A, A, 0.3f, 10, false).ok);
    CHECK(!BuildCrossover(net, A, B, 0.3f, 50, false).ok);
}

static void TestCrossings() {
    Network net;
    const int r0 = MkNode(net, {-100, 0}), r1 = MkNode(net, {100, 0});
    const int rail = Line(net, r0, r1);
    const int road = Line(net, MkNode(net, {0, -60}), MkNode(net, {0, 60}), kRoad);
    const std::vector<RailCrossing> lx = FindLevelCrossings(net);
    CHECK(lx.size() == 1 && lx[0].rail == rail && lx[0].road == road && std::fabs(lx[0].angleDeg - 90) < 0.5f && Len(lx[0].p) < 0.1f);
    CHECK(FindLevelCrossings(net).size() == lx.size()); // deterministic
    std::string why;
    std::vector<RailCrossing> xs;
    // a new road over the rail at 20 deg: refused; at 60: a level crossing
    CHECK(!CheckCrossings(net, Cubic::Line({40, -20}, Dir(20) * 120 + V2{40, -20}), 0, 0, false, why) && !why.empty());
    CHECK(CheckCrossings(net, Cubic::Line({30, -40}, Dir(60) * 90 + V2{30, -40}), 0, 0, false, why, &xs));
    CHECK(xs.size() == 1 && xs[0].type == JType::LevelCrossing && std::fabs(xs[0].angleDeg - 60) < 1);
    // a new railway across the rail at 6 deg: too flat; at 45: a diamond; across the road it is a level crossing too
    xs.clear();
    CHECK(!CheckCrossings(net, Cubic::Line({-80, -4}, Dir(6) * 70 + V2{-80, -4}), 0, 0, true, why));
    CHECK(CheckCrossings(net, Cubic::Line({-40, -50}, Dir(45) * 120 + V2{-40, -50}), 0, 0, true, why, &xs));
    int diamonds = 0, levels = 0;
    for (const RailCrossing& x : xs) diamonds += x.type == JType::Diamond, levels += x.type == JType::LevelCrossing;
    CHECK(diamonds == 1 && levels == 1);
    // a bridge 6 m up crosses nothing
    xs.clear();
    CHECK(CheckCrossings(net, Cubic::Line({-40, -50}, Dir(45) * 120 + V2{-40, -50}), 6, 6, true, why, &xs) && xs.empty());
    // a level crossing right at a road junction is refused
    Network n2;
    const int j = MkNode(n2, {0, 0});
    for (float d : {0.0f, 90.0f, 180.0f}) Line(n2, j, MkNode(n2, Dir(d) * 60), kRoad);
    CHECK(!CheckCrossings(n2, Cubic::Line({-40, 3}, {40, 3}), 0, 0, true, why) && why == "Level crossing too close to a junction");
    CHECK(CheckCrossings(n2, Cubic::Line({-40, 30}, {40, 30}), 0, 0, true, why));
}

static bool SameMesh(const JunctionMesh& a, const JunctionMesh& b) {
    for (int k = 0; k < (int)JMat::Count; ++k) {
        if (a.byMat[k].size() != b.byMat[k].size()) return false;
        if (!a.byMat[k].empty() && std::memcmp(a.byMat[k].data(), b.byMat[k].data(), a.byMat[k].size() * sizeof(tbd::Vertex))) return false;
    }
    return true;
}
static int CountFrogs(const JunctionParams& p) {
    using namespace jmesh;
    JunctionMesh m;
    JunctionParams q = AdjustParams(p);
    std::vector<char> virt;
    const auto routes = JunctionRoutes(q, &virt);
    return Frogs(m, routes, virt, q.gauge);
}

static void TestMeshes() {
    std::printf("\n%-20s %-6s %6s %4s | %6s %4s %5s %5s %4s %5s | %5s  (LOD1)\n", "variant", "hand", "angle", "lod", "tris", "drw", "bed", "ties", "steel", "props", "paint");
    const std::vector<JunctionParams> cat = JunctionCatalog();
    for (size_t i = 0; i < cat.size(); i += 2) {
        const JunctionMesh m0 = BuildJunctionMesh(cat[i]), m1 = BuildJunctionMesh(cat[i + 1]);
        const char* hand = cat[i].hand == Hand::Left ? "left" : cat[i].hand == Hand::Right ? "right" : "-";
        std::printf("%-20s %-6s %6.1f %4d | %6zu %4d %5zu %5zu %5zu %5zu | %5zu  %5zu\n", JTypeName(cat[i].type), hand, m0.angleDeg, 0, m0.Tris(), m0.Draws(),
                    m0.TrisOf(JMat::Ballast), m0.TrisOf(JMat::Tie), m0.TrisOf(JMat::Steel), m0.TrisOf(JMat::Prop), m0.TrisOf(JMat::Paint), m1.Tris());
        CHECK(m0.Tris() > 50 && m1.Tris() > 0 && m1.Tris() * 2 < m0.Tris());
        // level crossings add Road (ramps) and Paint, which join the road mesh's own draws in the game
        CHECK(m0.Draws() <= 6 && m1.Draws() <= 6 && m0.TrisOf(JMat::Tie) > 0 && m1.TrisOf(JMat::Tie) == 0);
        CHECK(m0.Tris() <= 1200); // a junction stays under a station's budget
        CHECK(SameMesh(m0, BuildJunctionMesh(cat[i])));
        for (const auto& v : m0.byMat)
            for (size_t t = 0; t + 2 < v.size(); t += 3) { // counter-clockwise around the normal: back-face culling keeps it
                const tbd::Vertex &A = v[t], &B = v[t + 1], &C = v[t + 2];
                CHECK(std::isfinite(A.px) && std::isfinite(A.pz) && A.py >= jmesh::kRoadY - 0.01f && A.py < 6.5f);
                const float ux = B.px - A.px, uy = B.py - A.py, uz = B.pz - A.pz, wx = C.px - A.px, wy = C.py - A.py, wz = C.pz - A.pz;
                const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
                if (nx * nx + ny * ny + nz * nz > 1e-10f) CHECK(nx * A.nx + ny * A.ny + nz * A.nz > 0);
            }
    }
    // frogs: one per turnout / wye, three on a 3-way, four on a diamond
    JunctionParams p;
    CHECK(CountFrogs(p) == 1);
    p.type = JType::Wye, p.angleDeg = 8;
    CHECK(CountFrogs(p) == 1);
    p.type = JType::ThreeWay, p.angleDeg = 12;
    CHECK(CountFrogs(p) == 3);
    p.type = JType::Diamond, p.angleDeg = 40;
    CHECK(CountFrogs(p) == 4);
    p.type = JType::Crossover, p.angleDeg = 10;
    CHECK(CountFrogs(p) == 2);
    // clamped to the network's limits
    JunctionParams q;
    q.angleDeg = 50, q.radius = 20;
    JunctionMesh m = BuildJunctionMesh(q);
    CHECK(m.adjusted && m.angleDeg == kTurnoutMaxDeg && m.radius == kRailMinRadius);
    q.type = JType::Diamond, q.angleDeg = 5;
    CHECK(BuildJunctionMesh(q).angleDeg == kDiamondMinDeg);
    q.type = JType::LevelCrossing, q.angleDeg = 10;
    CHECK(BuildJunctionMesh(q).angleDeg == kLevelCrossMinDeg);
    q.type = JType::Crossover, q.angleDeg = 30, q.radius = 150, q.spacing = 5;
    CHECK(BuildJunctionMesh(q).angleDeg <= CrossoverMaxAngle(5, 150) + 1e-3f);
    // the turnout's diverging track ends where the geometry says, on the hand's side
    JunctionParams t;
    t.hand = Hand::Left;
    const std::vector<jmesh::Route> R = JunctionRoutes(t);
    const V2 end = R[2].back().c;
    const V2 arcEnd{TurnoutLead(12, 150), -TurnoutOffset(12, 150)}, tailV = end - arcEnd;
    CHECK(end.z < 0 && Len(tailV) >= t.tail - 0.01f && std::fabs(Cross2(Norm(tailV), V2{std::cos(Rad(12)), -std::sin(Rad(12))})) < 1e-3f);
    // every route ends a whole number of sleeper pitches from its start (plain track meets it in phase)
    for (const auto& r : R) CHECK(std::fabs(r.back().s / jmesh::kTieStep - std::round(r.back().s / jmesh::kTieStep)) < 1e-3f);
    // a crossover's diagonal lands on the other track, parallel
    JunctionParams c;
    c.type = JType::Crossover, c.angleDeg = 10;
    const std::vector<jmesh::Route> C = JunctionRoutes(c);
    CHECK(std::fabs(C[2].back().c.z - c.spacing) < 0.02f && std::fabs(C[2].back().f.z) < 1e-3f);
    CHECK(std::fabs(C[2].back().c.x - CrossoverRun(5, 10, 150)) < 0.05f);
}

static void TestLevelCrossing() {
    using namespace jmesh;
    for (float ang : {90.0f, 60.0f, 45.0f})
        for (float road : {4.0f, 9.0f, 14.0f, 24.0f}) {
            JunctionParams p;
            p.type = JType::LevelCrossing, p.angleDeg = ang, p.roadWidth = road;
            const JunctionMesh m = BuildJunctionMesh(p);
            const CrossingFrame F = CrossFrame(p);
            // deck flush with the rail head, ramps down to the road surface
            float deckY = -1, rampLo = 9;
            std::vector<tbd::Vertex> deck;
            CrossingDeck(deck, p);
            for (const tbd::Vertex& v : deck) {
                deckY = std::max(deckY, v.py);
                CHECK(std::fabs(v.py - kRailTop) < 1e-5f);
            }
            for (const tbd::Vertex& v : m.byMat[(int)JMat::Road]) rampLo = std::min(rampLo, v.py);
            CHECK(deckY == kRailTop && std::fabs(rampLo - (kRoadY + 0.01f)) < 1e-4f);
            // every prop (masts, booms, cabinet) stays off the deck and its ramps' crest: outside the rail corridor
            for (const tbd::Vertex& v : m.byMat[(int)JMat::Prop]) CHECK(std::fabs(v.pz) > kDeckHalf - 1e-3f || std::fabs(v.px) > 1.0f);
            // markings square to the road: every paint quad edge either along or across the road
            const auto& pt = m.byMat[(int)JMat::Paint];
            int square = 0;
            for (size_t t = 0; t + 2 < pt.size(); t += 6) {
                const V2 e{pt[t + 1].px - pt[t].px, pt[t + 1].pz - pt[t].pz};
                square += std::fabs(Dot2(Norm(e), F.r)) < 1e-3f || std::fabs(Cross2(Norm(e), F.r)) < 1e-3f;
            }
            CHECK(square >= 2); // the two stop lines (the RXR bars are diagonal)
            for (const tbd::Vertex& v : pt) CHECK(std::fabs(v.py - (kRoadY + 0.02f)) < 1e-4f);
            // wider roads: more lanes, a cantilever (2+ lanes) and a centre gate (half wider than 7.5 m)
            JunctionParams q = p;
            q.roadWidth = 9;
            if (road > 9) CHECK(m.TrisOf(JMat::Prop) > BuildJunctionMesh(q).TrisOf(JMat::Prop));
            // gates up: the booms stand
            JunctionParams up = p;
            up.gate = 1;
            float topDown = 0, topUp = 0;
            for (const tbd::Vertex& v : m.byMat[(int)JMat::Prop]) topDown = std::max(topDown, v.py);
            const JunctionMesh mu = BuildJunctionMesh(up); // a named mesh: a range-for over a temporary's member dangles
            for (const tbd::Vertex& v : mu.byMat[(int)JMat::Prop]) topUp = std::max(topUp, v.py);
            CHECK(topUp > topDown + 0.5f); // a 4 m road: a 4 m boom stands just over the 4.3 m mast
        }
    CHECK(CrossingLanes(9) == 1 && CrossingLanes(14) == 2 && CrossingLanes(24) == 3);
}

static void TestPlanes() {
    // every junction type has a canonical layout; the box is 10 triangles; the texture fits an iPad
    for (int t = (int)JType::Turnout; t <= (int)JType::LevelCrossing; ++t) {
        JunctionParams p;
        p.type = (JType)t, p.angleDeg = t == (int)JType::LevelCrossing ? 70.0f : 13.0f, p.hand = Hand::Left;
        const JunctionPlane J = BuildJunctionPlane(p);
        CHECK(J.layout >= 0 && J.mesh.TrisOf(JMat::Spom) == 10);
        if (J.layout < 0) continue;
        const JunctionParams& lp = JunctionLayouts()[J.layout].p;
        CHECK(lp.type == p.type);
        const PlaneRect r = JunctionPlaneRect(lp);
        int nA, nX;
        LayoutTexSize(r, nA, nX);
        CHECK(nA <= 2048 && nX <= 2048 && nA % 4 == 0 && nX % 4 == 0 && r.L() / nA < 0.035f && r.W() / nX < 0.035f);
        // mirrored for Left (handed types): the rect flips across
        CHECK(J.mirror); // asked left-handed
        CHECK(std::fabs(J.rect.z0 + r.z1) < 1e-4f && std::fabs(J.rect.z1 + r.z0) < 1e-4f);
        // every rail-carrying route's bed lies inside the rect, and every route is whole sleeper pitches long
        std::vector<char> virt;
        const auto routes = JunctionRoutes(lp, &virt);
        const bool inner = lp.type == JType::Crossover || lp.type == JType::Scissors || lp.type == JType::SingleSlip;
        for (size_t k = 0; k < routes.size(); ++k) {
            const auto& rt = routes[k];
            if (!inner || k < 2) // the routes that meet plain track (a crossover's diagonals and a slip's curve end on its own tracks)
                CHECK(std::fabs(rt.back().s / jmesh::kTieStep - std::round(rt.back().s / jmesh::kTieStep)) < 1e-3f);
            for (const auto& q : rt) CHECK(q.c.x >= r.x0 - 1e-3f && q.c.x <= r.x1 + 1e-3f && q.c.z >= r.z0 - 1e-3f && q.c.z <= r.z1 + 1e-3f);
        }
        // box faces wound outward (CCW from outside), as the plain track box
        const auto& b = J.mesh.byMat[(int)JMat::Spom];
        const float cx = 0.5f * (J.rect.x0 + J.rect.x1), cz = 0.5f * (J.rect.z0 + J.rect.z1), cy = jmesh::kRailTop - 0.3f;
        for (size_t k = 0; k + 2 < b.size(); k += 3) {
            const float ux = b[k + 1].px - b[k].px, uy = b[k + 1].py - b[k].py, uz = b[k + 1].pz - b[k].pz;
            const float wx = b[k + 2].px - b[k].px, wy = b[k + 2].py - b[k].py, wz = b[k + 2].pz - b[k].pz;
            const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            const float ox = (b[k].px + b[k + 1].px + b[k + 2].px) / 3 - cx, oy = (b[k].py + b[k + 1].py + b[k + 2].py) / 3 - cy, oz = (b[k].pz + b[k + 1].pz + b[k + 2].pz) / 3 - cz;
            CHECK(nx * ox + ny * oy + nz * oz > 0);
            CHECK(b[k].a == kJunctionSpomAlpha && b[k].nx == 1.0f);
        }
        CHECK(J.mesh.Draws() <= 4);
    }
    // the nearest canonical angle
    JunctionParams p;
    p.angleDeg = 10.5f;
    CHECK(JunctionLayouts()[NearestLayout(p)].id == "turnout_12");
    p.angleDeg = 17;
    CHECK(JunctionLayouts()[NearestLayout(p)].id == "turnout_20");
}

// The junction planes in a network (rail_junction_place.h): sites, layouts, frames, where the tracks stop, phase.
static float CutDist(const Network& net, int s, float t, const std::vector<V2>& ends) { // the cut point to the nearest route end
    const V2 q = net.Curve(s).At(t);
    float best = 1e9f;
    for (V2 e : ends) best = std::min(best, Len(e - q));
    return best;
}
static std::vector<V2> WorldEnds(const JunctionSite& S) {
    std::vector<V2> v;
    for (const RouteEnd& e : LayoutEnds(S.layout)) v.push_back(S.World(e.p.x, e.p.z));
    return v;
}
static void TestPlacement() {
    for (float side : {1.0f, -1.0f}) { // a turnout either hand: the box set back to the IP, every arm cut at its route's end
        Network net;
        const int c = MkNode(net, {0, 0});
        const int sw = Line(net, MkNode(net, {-100, 0}), c), se = Line(net, c, MkNode(net, {100, 0})), sd = Line(net, c, MkNode(net, Dir(12 * side) * 80));
        const auto sites = FindJunctionSites(net, nullptr);
        CHECK(sites.size() == 1);
        if (sites.size() != 1) continue;
        const JunctionSite& S = sites[0];
        CHECK(S.type == JType::Turnout && JunctionLayouts()[S.layout].id == "turnout_12" && S.mirror == (side < 0));
        CHECK(std::fabs(S.f.x - 1) < 1e-4f && S.o.x < -10 && std::fabs(S.o.z) < 1e-3f); // points well before the node
        const RailCuts R = RailCutsFor(net, sites);
        // one gap per arm, from the node to where its route ends: the stem's at its b end, the others' at their a end
        CHECK(R.trackGaps[(size_t)sw].size() == 1 && R.trackGaps[(size_t)se].size() == 1 && R.trackGaps[(size_t)sd].size() == 1);
        if (R.trackGaps[(size_t)sw].empty() || R.trackGaps[(size_t)se].empty() || R.trackGaps[(size_t)sd].empty()) continue;
        const float tw = R.trackGaps[(size_t)sw][0].first, te = R.trackGaps[(size_t)se][0].second, td = R.trackGaps[(size_t)sd][0].second;
        CHECK(R.trackGaps[(size_t)sw][0].second == 1.0f && R.trackGaps[(size_t)se][0].first == 0.0f && tw > 0.5f && te > 0.05f && td > 0.05f);
        const auto ends = WorldEnds(S);
        CHECK(CutDist(net, sw, tw, ends) < 0.05f && CutDist(net, se, te, ends) < 0.05f);
        CHECK(CutDist(net, sd, td, ends) < 0.3f); // the diverging route's end lies on the graph's line
        // the plain track meets the box at a pitch boundary (sleeper phase continues)
        MeshOut out;
        BuildNetwork(net, nullptr, out, &R);
        const V2 cut = net.Curve(sw).At(tw);
        int near = 0;
        for (const tbd::Vertex& v : out.byMat[(int)Mat::Track])
            if (Len(V2{v.px, v.pz} - cut) < 2.3f) {
                ++near;
                const float ph = std::fmod(std::fabs(v.v), 0.6f);
                CHECK(std::min(ph, 0.6f - ph) < 1e-3f);
            }
        CHECK(near > 0);
        JunctionDraws d;
        BuildJunctionDraws(net, sites, d, &out);
        CHECK(d.sites == 1 && d.byLayout[(size_t)S.layout].size() == 30 && !d.props.empty());
    }
    { // a diamond at 45 deg, and a crossover between parallel tracks the layout's spacing apart
        Network net;
        const int c = MkNode(net, {0, 0});
        Line(net, MkNode(net, {-80, 0}), c), Line(net, c, MkNode(net, {80, 0}));
        Line(net, MkNode(net, Dir(225) * 80), c), Line(net, c, MkNode(net, Dir(45) * 80));
        const auto sites = FindJunctionSites(net, nullptr);
        CHECK(sites.size() == 1 && sites[0].type == JType::Diamond && JunctionLayouts()[sites[0].layout].id == "diamond_45");
    }
    {
        Network net;
        const JunctionParams lp = AdjustParams(JunctionLayouts()[(size_t)NearestLayout([] { JunctionParams p; p.type = JType::Crossover; return p; }())].p);
        const float run = CrossoverRun(lp.spacing, lp.angleDeg, lp.radius);
        const int a = MkNode(net, {0, 0}), b = MkNode(net, {run, lp.spacing});
        Line(net, MkNode(net, {-100, 0}), a), Line(net, a, MkNode(net, {150, 0}));
        Line(net, MkNode(net, {-100, lp.spacing}), b), Line(net, b, MkNode(net, {150, lp.spacing}));
        const int diag = Line(net, a, b);
        const auto sites = FindJunctionSites(net, nullptr);
        CHECK(sites.size() == 1 && sites[0].type == JType::Crossover && sites[0].node2 >= 0);
        const RailCuts R = RailCutsFor(net, sites);
        float covered = 0; // the diagonal is all inside the box: its gaps cover it
        for (const auto& g : R.trackGaps[(size_t)diag]) covered += g.second - g.first;
        CHECK(R.trackGaps[(size_t)diag].size() == 2 && covered >= 1.0f);
    }
    { // a level crossing: the rail's track cut along the box, ramps into the road's mesh, gates as booms
        Network net;
        const int rail = Line(net, MkNode(net, {-120, 0}), MkNode(net, {120, 0}));
        Line(net, MkNode(net, {0, -60}), MkNode(net, {0, 60}), kRoad);
        const auto sites = FindJunctionSites(net, nullptr);
        CHECK(sites.size() == 1 && sites[0].type == JType::LevelCrossing && JunctionLayouts()[sites[0].layout].id == "level_90_w6");
        if (sites.size() != 1) return;
        const RailCuts R = RailCutsFor(net, sites);
        CHECK(R.trackGaps[(size_t)rail].size() == 1);
        MeshOut out;
        BuildNetwork(net, nullptr, out, &R);
        const PlaneRect r = SiteRect(sites[0]);
        for (const tbd::Vertex& v : out.byMat[(int)Mat::Track]) {
            CHECK(v.px < r.x0 + 0.01f || v.px > r.x1 - 0.01f);
            if (!(v.px < r.x0 + 0.01f || v.px > r.x1 - 0.01f)) std::printf("  track vertex at x %.3f (box %.3f .. %.3f)\n", v.px, r.x0, r.x1);
        }
        const size_t asphalt = out.byMat[(int)Mat::Asphalt].size(), conc = out.byMat[(int)Mat::Concrete].size();
        JunctionDraws d;
        BuildJunctionDraws(net, sites, d, &out);
        CHECK(out.byMat[(int)Mat::Asphalt].size() > asphalt && out.byMat[(int)Mat::Concrete].size() > conc); // lanes and sidewalks ramp
        std::vector<tbd::Vertex> up, down;
        CrossingBooms(sites[0], 1.0f, up), CrossingBooms(sites[0], 0.0f, down);
        float yUp = -1e9f, yDown = -1e9f;
        for (const tbd::Vertex& v : up) yUp = std::max(yUp, v.py);
        for (const tbd::Vertex& v : down) yDown = std::max(yDown, v.py);
        CHECK(!up.empty() && up.size() == down.size() && yUp > yDown + 2.0f); // raised booms reach far higher
    }
    // switch angles snap to the baked layouts
    CHECK(AngleBetween(SnapSwitchDir({Dir(180), Dir(0)}, Dir(11)), Dir(12)) < 0.01f);
    CHECK(AngleBetween(SnapSwitchDir({Dir(180), Dir(0)}, Dir(-17)), Dir(-20)) < 0.01f);
    CHECK(AngleBetween(SnapSwitchDir({Dir(180), Dir(0)}, Dir(2)), Dir(2)) < 0.01f);
    CHECK(AngleBetween(SnapSwitchDir({Dir(180), Dir(0)}, Dir(40)), Dir(40)) < 0.01f);
}

int main() {
    TestPlacement();
    TestClassify();
    TestPlanJoin();
    TestRouting();
    TestCrossovers();
    TestCrossings();
    TestMeshes();
    TestLevelCrossing();
    TestPlanes();
    if (failures) {
        std::printf("\nrail_junction_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("\nrail_junction_test: all passed\n");
    return 0;
}
