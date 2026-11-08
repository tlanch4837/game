// Headless checks for the city pedestrians (src/city/sim_walkers.h):
//   the sidewalk graph runs on the sidewalks (every non-crossing point outside every carriageway), crossings only at
//   junction crosswalks; walkers stay on the sidewalk strips while they walk (only crossings touch the carriageway);
//   they appear only at their door or out of view and disappear only at a door or out of view (nobody pops in view, the
//   camera panning included); the same trip at the same time is in the same place however the camera got there; the
//   counts follow the hour (few at night, more at lunch, lunch trips at noon); never more than the cap.
// Build and run: tests\sim_walkers_test.bat
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "city/sim_walkers.h"

using namespace tbd;
using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                                \
    do {                                                                                        \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; }     \
    } while (0)

// A street grid: (n+1) x (n+1) nodes `block` metres apart (two-lane roads with 2.5 m sidewalks), a dead-end spur off
// the east side, and a pedestrian path across one block.
struct Town {
    Network net;
    std::vector<WalkDoor> doors;
    int n = 4;
    float block = 90.0f;
    Town(int blocks = 4, float people = 6.0f, float b = 90.0f) : n(blocks), block(b) {
        const int road = ProfileIndex("Two-Lane Road"), wide = ProfileIndex("Four-Lane Road"), path = ProfileIndex("Pedestrian Path");
        std::vector<int> id((size_t)((n + 1) * (n + 1)));
        for (int z = 0; z <= n; ++z)
            for (int x = 0; x <= n; ++x) id[(size_t)(z * (n + 1) + x)] = net.AddNode({x * block, 0, z * block}, true);
        auto seg = [&](int a, int b, int prof) {
            const V2 pa = XZ(net.nodes[(size_t)a].p), pb = XZ(net.nodes[(size_t)b].p);
            const Cubic c = Cubic::Line(pa, pb);
            return net.AddSeg(a, b, c, prof);
        };
        for (int z = 0; z <= n; ++z)
            for (int x = 0; x < n; ++x) seg(id[(size_t)(z * (n + 1) + x)], id[(size_t)(z * (n + 1) + x + 1)], z == n / 2 ? wide : road);
        for (int x = 0; x <= n; ++x)
            for (int z = 0; z < n; ++z) seg(id[(size_t)(z * (n + 1) + x)], id[(size_t)((z + 1) * (n + 1) + x)], road);
        const int spur = net.AddNode({n * block + 70.0f, 0, block}, true); // a dead end off the east side
        seg(id[(size_t)(n + 1 + n)], spur, road);
        // a gently bent street off the west side (a seamless mitre joint, then a dead end)
        const int w1 = net.AddNode({-60.0f, 0, block * 2.0f + 6.0f}, true), w2 = net.AddNode({-120.0f, 0, block * 2.0f + 20.0f}, true);
        seg(id[(size_t)(2 * (n + 1))], w1, road);
        seg(w1, w2, road);
        const int p0 = net.AddNode({block * 0.5f, 0, block * 0.2f}, true), p1 = net.AddNode({block * 0.5f, 0, block * 0.8f}, true);
        seg(p0, p1, path);
        // doors: along every street side, every 16 m, at the back of the sidewalk, facing the street
        uint64_t next = 1;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive) continue;
            const Profile& p = net.Prof((int)s);
            if (SidewalkGraph::FreeProfile(p)) continue;
            const Cubic c = net.Curve((int)s);
            const float L = c.Length();
            for (float d = 18.0f; d < L - 18.0f; d += 16.0f) {
                const float t = d / L;
                const V2 at = c.At(t), nrm = LeftOf(c.Tan(t));
                for (int side = -1; side <= 1; side += 2) {
                    WalkDoor w;
                    w.id = next++;
                    w.p = at + nrm * ((p.Half() + 0.6f) * (float)side);
                    w.out = nrm * (-(float)side);
                    const uint64_t h = walk::Hash(w.id, 77);
                    const int k = (int)(h % 10);
                    w.kind = k < 5 ? DoorKind::Home : k < 7 ? DoorKind::Shop : k < 9 ? DoorKind::Work : DoorKind::School;
                    w.people = people * (w.kind == DoorKind::School ? 4.0f : 1.0f);
                    doors.push_back(w);
                }
            }
        }
        // two bus stops on the wide street
        for (int k = 0; k < 2; ++k) {
            WalkDoor w;
            w.id = 100000 + (uint64_t)k;
            const float x = block * (0.5f + 2.0f * (float)k), z = block * (float)(n / 2);
            w.p = {x, z + 9.0f}, w.out = {0, -1}, w.kind = DoorKind::Stop;
            doors.push_back(w);
        }
    }
    V2 Centre() const { return {n * block * 0.5f, n * block * 0.5f}; }
};

static WalkView ViewAt(V2 focus, float dist = 120.0f) {
    WalkView v;
    v.eye = {focus.x, dist * 0.55f, focus.z + dist * 0.85f};
    v.viewProj = PerspectiveRH(Radians(50.0f), 1.5f, 0.5f, 3000.0f) * LookAtRH(v.eye, {focus.x, 0, focus.z}, {0, 1, 0});
    v.range = 220.0f;
    v.valid = true;
    return v;
}
static float DiskRadius(const WalkView& v) {
    const float r = v.range + 70.0f;
    return v.eye.y < r ? std::sqrt(r * r - v.eye.y * v.eye.y) : 0.0f;
}

// The carriageway as the road mesh draws it (road_network.h): each segment's lanes between its junction cuts, each junction's
// asphalt fan (inside the curb curves, as BuildJunction makes its loop) and each dead end's half disk.
struct Carriageway {
    const Network* net = nullptr;
    std::vector<SegTrim> trims;
    std::vector<std::vector<Arm>> arms;
    std::vector<std::vector<V2>> fans; // per node: the fan's boundary (empty: none)
    explicit Carriageway(const Network& n) : net(&n) {
        trims = ComputeTrims(n, &arms);
        fans.assign(n.nodes.size(), {});
        for (size_t i = 0; i < n.nodes.size(); ++i) {
            const std::vector<Arm>& A = arms[i];
            if (A.size() < 2) continue;
            bool cars = false;
            for (const Arm& a : A) cars = cars || SidewalkGraph::CarriageHalf(n.Prof(a.seg), 0) > 0;
            if (!cars) continue;
            const V2 C = XZ(n.nodes[i].p);
            for (size_t k = 0; k < A.size(); ++k) {
                const Arm& a = A[k];
                const Arm& b = A[(k + 1) % A.size()];
                const V2 aInner = a.end - a.left * (a.hw - a.sideR), bInner = b.end + b.left * (b.hw - b.sideL);
                V2 q;
                const float maxR = 3.0f * std::max(a.hw, b.hw) + 10.0f;
                if (!LineHit(aInner, a.u, bInner, b.u, q) || Len(q - C) > maxR || Dot2(q - aInner, a.u) > 0.5f) q = (aInner + bInner) * 0.5f;
                fans[i].push_back(a.end + a.left * (a.hw - a.sideL));
                for (int j = 0; j <= 10; ++j) {
                    const float t = (float)j / 10.0f, u = 1 - t;
                    fans[i].push_back(aInner * (u * u) + q * (2 * u * t) + bInner * (t * t));
                }
            }
        }
    }
    static bool InPoly(const std::vector<V2>& poly, V2 p) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            if ((poly[i].z > p.z) != (poly[j].z > p.z) && p.x < (poly[j].x - poly[i].x) * (p.z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x) in = !in;
        return in;
    }
    // Is p in the carriageway (by more than slack)? `who`: the segment, or -1 - node for a junction / dead end.
    bool Inside(V2 p, int& who, float slack = 0.05f) const {
        for (size_t s = 0; s < net->segs.size(); ++s) {
            if (!net->segs[s].alive) continue;
            const Profile& pr = net->Prof((int)s);
            float t, d;
            net->Nearest((int)s, p, t, d);
            if (t < trims[s].t0 + 1e-3f || t > trims[s].t1 - 1e-3f) continue; // the junction's part
            const Cubic c = net->Curve((int)s);
            const int side = Dot2(p - c.At(t), LeftOf(c.Tan(t))) >= 0 ? 0 : 1;
            const float half = SidewalkGraph::CarriageHalf(pr, side);
            if (half > 0 && d < half - slack) { who = (int)s; return true; }
        }
        for (size_t i = 0; i < fans.size(); ++i) {
            if (fans[i].empty()) continue;
            if (InPoly(fans[i], p)) {
                // slack: not within `slack` of the fan's edge
                float edge = 1e30f;
                const std::vector<V2>& f = fans[i];
                for (size_t k = 0; k < f.size(); ++k) {
                    const V2 a = f[k], b = f[(k + 1) % f.size()], d = b - a;
                    const float l2 = Dot2(d, d), t = l2 > 1e-8f ? std::clamp(Dot2(p - a, d) / l2, 0.0f, 1.0f) : 0.0f;
                    edge = std::min(edge, Len(a + d * t - p));
                }
                if (edge > slack) { who = -1 - (int)i; return true; }
            }
        }
        for (size_t i = 0; i < net->nodes.size(); ++i) { // dead ends: the cap's road half disk
            if (!net->nodes[i].alive || net->nodes[i].segs.size() != 1) continue;
            const int s = net->nodes[i].segs[0];
            const Profile& pr = net->Prof(s);
            if (SidewalkGraph::CarriageHalf(pr, 0) <= 0) continue;
            const V2 C = XZ(net->nodes[i].p), u = net->OutDir(s, (int)i);
            const float inner = std::max(0.5f, pr.Half() - std::max(pr.sideL, pr.sideR));
            if (Len(p - C) < inner - slack && Dot2(p - C, u) <= 0) { who = -1 - (int)i; return true; }
        }
        return false;
    }
};
static bool InCarriageway(const Network& net, V2 p, int& who, float slack = 0.05f) {
    static const Network* cachedFor = nullptr;
    static uint64_t cachedVersion = 0;
    static std::unique_ptr<Carriageway> cw;
    if (cachedFor != &net || cachedVersion != net.version || !cw) cw = std::make_unique<Carriageway>(net), cachedFor = &net, cachedVersion = net.version;
    return cw->Inside(p, who, slack);
}

static void TestGraph() {
    Town town;
    SidewalkGraph g;
    g.Build(town.net);
    int along = 0, crossings = 0, corners = 0, caps = 0, links = 0, bad = 0, badCross = 0;
    for (size_t e = 0; e < g.edges.size(); ++e) {
        const WalkEdge& E = g.edges[e];
        along += E.kind == WalkEdgeKind::Along, crossings += E.kind == WalkEdgeKind::Crossing, corners += E.kind == WalkEdgeKind::Corner;
        caps += E.kind == WalkEdgeKind::Cap, links += E.kind == WalkEdgeKind::Link;
        if (E.kind == WalkEdgeKind::Crossing) {
            // a crossing sits at a junction (3+ arms) or on a path
            int arms = 0;
            for (int s : town.net.nodes[(size_t)E.node].segs) arms += town.net.segs[(size_t)s].alive;
            if (arms < 3 && !SidewalkGraph::FreeProfile(town.net.Prof(E.seg))) ++badCross;
            continue;
        }
        for (uint32_t i = E.first; i < E.first + E.count; ++i) {
            int who;
            if (InCarriageway(town.net, g.pts[i].p, who)) {
                if (bad < 5) std::printf("  edge %zu (kind %d) point (%.2f, %.2f) in the carriageway of segment %d\n", e, (int)E.kind, g.pts[i].p.x, g.pts[i].p.z, who);
                ++bad;
            }
        }
    }
    std::printf("graph: %zu edges (%d along, %d corners, %d crossings, %d caps, %d links), %zu points\n", g.edges.size(), along, corners, crossings, caps, links, g.pts.size());
    CHECK(along > 40);
    CHECK(corners > 20);
    CHECK(crossings > 20);
    CHECK(caps >= 3);   // the two dead ends, and the path's two ends
    CHECK(links >= 2);  // the bent street's joint, both sides
    CHECK(bad == 0);
    CHECK(badCross == 0);
    // the walking line of a two-lane road: half the road (8.5 m wide: 2 x 3 m lanes + 2 x 2.5 m sides... 11 m) less half the sidewalk
    const Profile& two = Profiles()[(size_t)ProfileIndex("Two-Lane Road")];
    float off = 0;
    CHECK(SidewalkGraph::SideWalk(two, 0, off) && std::fabs(off - (two.Half() - 1.125f)) < 1e-3f);
    CHECK(SidewalkGraph::CarriageHalf(two, 0) > 0 && SidewalkGraph::CarriageHalf(two, 0) < off);
    // every vertex the graph uses is connected
    int lonely = 0;
    for (const WalkVert& v : g.verts) lonely += v.used && v.adjCount == 0;
    CHECK(lonely == 0);
}

struct Run {
    std::set<uint64_t> seen;
    int popsIn = 0, popsOut = 0, offSidewalk = 0, samples = 0, crossingSamples = 0;
};

// Steps the sim from t0 to t1 with the camera at focus(t); checks the pop and sidewalk rules on the way.
template <class Focus>
static void Step(WalkerSim& sim, const Town& town, double t0, double t1, float hour, Focus focus, Run& run, bool checks = true, double dt = 0.2) {
    std::unordered_map<uint64_t, Walker> last;
    WalkView lastView;
    for (const Walker& w : sim.walkers) last[w.id] = w;
    for (double T = t0; T < t1; T += dt) {
        const WalkView view = ViewAt(focus(T));
        sim.Update(T, hour, {view.eye.x, view.eye.z}, DiskRadius(view), view);
        if (!checks) { lastView = view; continue; }
        std::unordered_map<uint64_t, Walker> now;
        for (const Walker& w : sim.walkers) {
            now[w.id] = w;
            if (!last.count(w.id)) { // new this frame: at its door, or out of view
                const WalkDoor* from = nullptr;
                for (const WalkDoor& d : sim.doors) if (d.id == w.fromId) from = &d;
                const bool atDoor = from && Len(w.pos - from->p) < 2.0f;
                if (!atDoor && view.InView(w.pos, w.y, 0.0f)) {
                    if (run.popsIn < 5) std::printf("  pop in: walker %llx at (%.1f, %.1f), %.1f s into its walk\n", (unsigned long long)w.id, w.pos.x, w.pos.z, (float)(T - w.start));
                    ++run.popsIn;
                }
                run.seen.insert(w.id);
            }
            ++run.samples;
            if (WalkerSim::OnCrossing(w)) { ++run.crossingSamples; continue; }
            int who;
            if (InCarriageway(town.net, w.pos, who)) {
                if (run.offSidewalk < 5) std::printf("  in the road: walker %llx at (%.2f, %.2f) leg %d kind %d (segment %d)\n", (unsigned long long)w.id, w.pos.x, w.pos.z, w.cur, (int)w.legKind, who);
                ++run.offSidewalk;
            }
        }
        for (const auto& [id, w] : last) { // gone this frame: into a door, or out of view
            if (now.count(id)) continue;
            const WalkDoor* to = nullptr;
            for (const WalkDoor& d : sim.doors) if (d.id == w.toId) to = &d;
            const bool atDoor = to && Len(w.pos - to->p) < 2.0f;
            if (!atDoor && lastView.valid && lastView.InView(w.pos, w.y, 0.0f)) ++run.popsOut;
        }
        last.swap(now);
        lastView = view;
    }
}

static void TestWalkAndPops() {
    Town town;
    WalkerSim sim;
    sim.RebuildGraph(town.net);
    sim.SetDoors(town.doors);
    int attached = 0;
    for (const WalkDoor& d : sim.doors) attached += d.edge >= 0;
    std::printf("doors: %d of %zu attached to a sidewalk\n", attached, sim.doors.size());
    CHECK(attached == (int)sim.doors.size());
    Run run;
    const V2 c = town.Centre();
    // the camera holds, then pans across the town and back
    Step(sim, town, 0.0, 600.0, 12.5f, [&](double T) {
        if (T < 300) return c;
        const float f = (float)std::fmin(1.0, (T - 300) / 150.0);
        return V2{c.x - 160.0f + 320.0f * f, c.z + 80.0f * std::sin(f * 3.14159f)};
    }, run);
    std::printf("walk: %zu people seen, %d samples (%d on crossings), %llu at doors, %llu hidden on arrival, %llu planned, %llu failed, %llu finished, %llu dropped; keep %.2f, %zu now\n",
                run.seen.size(), run.samples, run.crossingSamples, (unsigned long long)sim.spawnedAtDoor, (unsigned long long)sim.suppressed,
                (unsigned long long)sim.planned, (unsigned long long)sim.failedPlans, (unsigned long long)sim.finished, (unsigned long long)sim.dropped, sim.keep, sim.walkers.size());
    CHECK(run.seen.size() > 60);
    CHECK(run.crossingSamples > 0);  // people do cross, at the crosswalks
    CHECK(run.offSidewalk == 0);
    CHECK(run.popsIn == 0);
    CHECK(run.popsOut == 0);
    CHECK(sim.finished > 10);        // walks end at doors
    // the stop: someone waited there
    int stopVisits = 0;
    for (const Walker& w : sim.walkers) stopVisits += w.via >= 0;
    std::printf("walking via a stop now: %d\n", stopVisits);
}

static void TestDeterminism() {
    Town town;
    WalkerSim a, b;
    a.RebuildGraph(town.net), a.SetDoors(town.doors);
    b.RebuildGraph(town.net), b.SetDoors(town.doors);
    const V2 c = town.Centre();
    Run ra, rb;
    Step(a, town, 0.0, 500.0, 12.0f, [&](double) { return c; }, ra, false);
    // b looks somewhere else entirely (no doors near), then comes to the same place
    Step(b, town, 0.0, 440.0, 12.0f, [&](double) { return V2{c.x + 5000.0f, c.z}; }, rb, false);
    CHECK(b.walkers.empty());
    Step(b, town, 440.0, 500.0, 12.0f, [&](double) { return c; }, rb, false);
    int common = 0, same = 0;
    for (const Walker& w : b.walkers) {
        const Walker* o = a.Find(w.id);
        if (!o) continue;
        ++common;
        same += Len(o->pos - w.pos) < 1e-3f && std::fabs(o->y - w.y) < 1e-3f && o->look == w.look && o->toId == w.toId;
    }
    std::printf("determinism: %zu walkers in the continuous run, %zu after returning, %d in both, %d identical\n", a.walkers.size(), b.walkers.size(), common, same);
    CHECK(common > 5);
    CHECK(same == common);
    // a trip planned from scratch is the same walk: the same place at the same time
    int checked = 0, equal = 0;
    for (const Walker& w : a.walkers) {
        Walker fresh;
        if (!a.PlanTrip(w.id, a.DoorIndex(w.fromId), w.start, fresh)) continue;
        a.Evaluate(fresh, a.now);
        ++checked;
        equal += Len(fresh.pos - w.pos) < 1e-3f && fresh.nLegs == w.nLegs && std::fabs(fresh.dur - w.dur) < 1e-4f;
    }
    CHECK(checked > 5 && equal == checked);
}

static size_t CountAt(float hour, int& lunch) {
    Town town;
    WalkerSim sim;
    sim.RebuildGraph(town.net), sim.SetDoors(town.doors);
    Run run;
    Step(sim, town, 0.0, 500.0, hour, [&](double) { return town.Centre(); }, run, false, 0.25);
    lunch = 0;
    for (const Walker& w : sim.walkers) lunch += w.purpose == WalkPurpose::Lunch;
    return sim.walkers.size();
}
static void TestTimeOfDay() {
    int l3, l8, l12, l19;
    const size_t n3 = CountAt(3.0f, l3), n8 = CountAt(8.0f, l8), n12 = CountAt(12.5f, l12), n19 = CountAt(19.0f, l19);
    std::printf("time of day: 03:00 %zu, 08:00 %zu, 12:30 %zu (lunch %d), 19:00 %zu walkers\n", n3, n8, n12, l12, n19);
    CHECK(n3 * 4 < n12);
    CHECK(n3 * 3 < n8);
    CHECK(l12 > l8 && l12 > 0);
    CHECK(n3 < 15);
}

static void TestBudget() {
    Town town(8, 120.0f, 70.0f); // dense: far more scheduled than the cap
    WalkerSim sim;
    sim.params.maxWalkers = 150;
    sim.RebuildGraph(town.net), sim.SetDoors(town.doors);
    size_t most = 0;
    double worstMs = 0, totalMs = 0;
    int frames = 0;
    const V2 c = town.Centre();
    for (double T = 0; T < 400.0; T += 1.0 / 30.0) {
        const WalkView view = ViewAt(c);
        const auto t0 = std::chrono::steady_clock::now();
        sim.Update(T, 12.5f, {view.eye.x, view.eye.z}, DiskRadius(view), view);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (T > 2.0) worstMs = std::max(worstMs, ms); // the first frames plan the whole neighbourhood
        totalMs += ms, ++frames;
        most = std::max(most, sim.walkers.size());
    }
    std::printf("budget: at most %zu of a cap of %d walkers (schedule wanted %.0f, keep %.2f); Update %.3f ms average, %.2f ms worst (%zu doors)\n", most,
                sim.params.maxWalkers, sim.expected, sim.keep, totalMs / frames, worstMs, sim.doors.size());
    CHECK(most <= (size_t)sim.params.maxWalkers);
    CHECK(most > 100);
    CHECK(sim.keep < 1.0f);
}

int main() {
    TestGraph();
    TestWalkAndPops();
    TestDeterminism();
    TestTimeOfDay();
    TestBudget();
    if (failures) {
        std::printf("sim_walkers_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("sim_walkers_test: all passed\n");
    return 0;
}
