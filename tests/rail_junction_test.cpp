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

#include "city/rail_junction_mesh.h"
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
        CHECK(m0.Draws() <= 5 && m1.Draws() <= 5 && m0.TrisOf(JMat::Tie) > 0 && m1.TrisOf(JMat::Tie) == 0);
        CHECK(m0.Tris() <= 1200); // a junction stays under a station's budget
        CHECK(SameMesh(m0, BuildJunctionMesh(cat[i])));
        for (const auto& v : m0.byMat)
            for (size_t t = 0; t + 2 < v.size(); t += 3) { // counter-clockwise around the normal: back-face culling keeps it
                const tbd::Vertex &A = v[t], &B = v[t + 1], &C = v[t + 2];
                CHECK(std::isfinite(A.px) && std::isfinite(A.pz) && A.py >= -0.01f && A.py < 4.5f);
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
    CHECK(std::fabs(end.x - (TurnoutLead(12, 150) + std::cos(Rad(12)) * t.tail)) < 0.05f);
    CHECK(end.z < 0 && std::fabs(-end.z - (TurnoutOffset(12, 150) + std::sin(Rad(12)) * t.tail)) < 0.05f);
    // a crossover's diagonal lands on the other track, parallel
    JunctionParams c;
    c.type = JType::Crossover, c.angleDeg = 10;
    const std::vector<jmesh::Route> C = JunctionRoutes(c);
    CHECK(std::fabs(C[2].back().c.z - c.spacing) < 0.02f && std::fabs(C[2].back().f.z) < 1e-3f);
    CHECK(std::fabs(C[2].back().c.x - CrossoverRun(5, 10, 150)) < 0.05f);
}

int main() {
    TestClassify();
    TestPlanJoin();
    TestRouting();
    TestCrossovers();
    TestCrossings();
    TestMeshes();
    if (failures) {
        std::printf("\nrail_junction_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("\nrail_junction_test: all passed\n");
    return 0;
}
