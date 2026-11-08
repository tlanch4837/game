// Headless checks for town traffic (src/city/sim_town_traffic.h) and the herds, children and flow it feeds:
//   a small town (one hood) gets no herd from the hood links alone (the zero-cars bug) and cars once its local links run;
//   the flow from the same links puts cosmetic cars on its roads; parked cars stand at home at night and at work at noon;
//   a highway that reaches both map edges carries through traffic across the whole map (herds, children and flow cars seen
//   near each end and in the middle), more of it as the city grows; no roads give no links and no cars; the same inputs
//   give the same links, herds and parked cars.
// Build and run: tests\sim_traffic_test.bat
#include <cstdio>
#include <vector>

#include "city/sim_children.h"
#include "city/sim_routes.h"
#include "city/sim_town_traffic.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static Lot MakeLot(uint64_t id, float x, float z, int type, int w, int d) {
    Lot l;
    l.id = id;
    l.c = {x, z};
    l.type = type, l.w = w, l.d = d;
    l.path = "T" + std::to_string(type) + "_" + std::to_string(w) + "x" + std::to_string(d) + "_L1.glb";
    l.progress = 2.0f;
    return l;
}
static int Seg(Network& net, int a, int b, int prof) {
    const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
    return net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
}

constexpr float kMapHalf = 4000.0f;

// Everything the host keeps, for one world.
struct World {
    Network net;
    CitySim sim;
    SegGrid grid;
    std::vector<Lot> lots;
    SimHoods hoods;
    std::vector<HoodLink> commute;
    FlowGraph g;
    LinkTable t;
    LinkBuild build;
    HerdSystem hs;
    RoutePicker picker;
    ChildSystem cs;
    TownTraffic town;

    void Finish(bool withTown = true) {
        ++net.version;
        grid.Build(net);
        if (!lots.empty()) sim.Sync(lots, 1);
        hoods.Build(sim, net, grid);
        hoods.RollUp(sim);
        hoods.LinksFromSuper(sim, commute);
        g = FlowGraph::Build(net);
        BuildLinkTable(net, g, t, &build);
        hs.seed = 0x5EED;
        hs.SetTable(&t);
        picker.SetTable(&t);
        picker.herds = &hs;
        picker.params.maxNewPerWindow = 64;
        cs.params.citySeed = 0x5EED;
        cs.params.densityScale = 1.0f;
        cs.SetTable(&t, &hs.routes);
        town.SetTable(net, g, t, build);
        if (withTown) town.Build(sim, hoods, commute, net, grid, t, {}, kMapHalf, hs.carShare, hs.curves);
    }
    // A game day of herds (hood links only, or with the town's node links); children watched around `focus` during
    // [watchFrom, watchTo) minutes. Returns the most children active at once.
    int Day(bool nodeLinks, V2 focus, int watchFrom = 6 * 60, int watchTo = 10 * 60) {
        const std::vector<HoodLink> none;
        int most = 0;
        for (int m = 0; m < 1440; ++m) {
            if (m % kWindowMinutes == 0) {
                if (m < kWindowMinutes) hs.NewDay();
                picker.BeginWindow(m / kWindowMinutes);
                hs.ReleaseHerds(m, m, hoods.hoods, commute, none, none, [this](int a, int b, RouteChoice* out) { return picker.Pick(a, b, out); },
                                nodeLinks ? &town.links : nullptr);
            }
            hs.AdvanceHerds(1.0f);
            if (m >= watchFrom && m < watchTo) {
                for (int k = 0; k < 4; ++k) cs.Update(0.5, 0.5, focus, hs, m + k * 0.25);
                most = std::max(most, cs.stats.active);
            }
        }
        return most;
    }
};

// A small town: one street of six two-lane segments (z = 40), houses in the west cells, shops and offices in the east.
static void SmallTown(World& w) {
    const int road = ProfileIndex("Two-Lane Road");
    std::vector<int> n;
    for (int i = 0; i <= 6; ++i) n.push_back(w.net.AddNode({20.0f + 128.0f * i, 0, 40}, true));
    for (int i = 0; i < 6; ++i) Seg(w.net, n[(size_t)i], n[(size_t)i + 1], road);
    uint64_t id = 1;
    for (int x = 0; x < 3; ++x) w.lots.push_back(MakeLot(id++, 64.0f + 128.0f * x, 64.0f, 2, 6, 6));     // medium housing
    w.lots.push_back(MakeLot(id++, 64.0f + 128.0f * 3, 64.0f, 4, 6, 6));                                  // shops
    for (int x = 4; x < 6; ++x) w.lots.push_back(MakeLot(id++, 64.0f + 128.0f * x, 64.0f, 5, 6, 6));     // offices
}

// A three-lane highway from the west map edge to the east one, in eight segments.
static void Highway(World& w, int& west, int& east) {
    const int hwy = ProfileIndex("Three-Lane Two-Way Highway");
    std::vector<int> n;
    for (int i = 0; i <= 8; ++i) n.push_back(w.net.AddNode({-kMapHalf + 1000.0f * i, 0, 600}, true));
    for (int i = 0; i < 8; ++i) Seg(w.net, n[(size_t)i], n[(size_t)i + 1], hwy);
    west = n.front(), east = n.back();
}

static std::vector<SegFlow> SolveFlow(const World& w) {
    std::vector<Trip> trips;
    w.town.FlowTrips(w.hs.curves, w.g, trips);
    FlowSolver solver;
    solver.Solve(w.g, trips);
    std::vector<SegFlow> out;
    solver.Segments(w.g, w.net.segs.size(), out);
    return out;
}

int main() {
    CHECK(ProfileIndex("Three-Lane Two-Way Highway") >= 0 && ProfileIndex("Two-Lane Road") >= 0);
    // the new demand curves sum to one like the others
    {
        DemandCurves c;
        for (int t : {(int)kTripThrough, (int)kTripFreight}) {
            float sum = 0;
            for (int w = 0; w < kWindows; ++w) sum += c.c[t][w];
            CHECK(std::fabs(sum - 1.0f) < 1e-4f);
        }
    }

    // ---- a small town: the zero-cars bug, then cars
    {
        World w;
        SmallTown(w);
        w.Finish();
        std::printf("small town: %llu residents, %llu jobs, %zu hoods, %zu commute links, %zu town links (%.0f local trips a day), %d outside\n",
                    (unsigned long long)w.sim.Population(), (unsigned long long)w.sim.JobCap(), w.hoods.hoods.size(), w.commute.size(), w.town.links.size(),
                    w.town.stats.localPerDay, w.town.stats.conns);
        CHECK(w.sim.Population() > 0 && w.sim.Employed() > 0);
        CHECK(w.hoods.hoods.size() == 1);
        bool allLocal = !w.commute.empty();
        for (const HoodLink& l : w.commute) allLocal &= l.from == l.to;
        CHECK(allLocal); // one hood: every commute starts and ends in it
        // the bug: with the hood links alone, a whole day releases nothing and no car ever shows
        const int before = w.Day(false, {380, 40});
        std::printf("  hood links only: %llu herds, %d cars at most\n", (unsigned long long)w.hs.stats.herdsReleased, before);
        CHECK(w.hs.stats.herdsReleased == 0 && before == 0);
        CHECK(w.town.stats.conns == 0 && w.town.stats.throughPerDay == 0); // no road reaches the map edge
        CHECK(w.town.stats.localPerDay > 50.0f);
    }
    {
        World w;
        SmallTown(w);
        w.Finish();
        const int morning = w.Day(true, {380, 40}, 6 * 60 + 30, 9 * 60);
        const float commuted = w.hs.stats.released[kTripCommute], home = w.hs.stats.released[kTripWorkHome];
        std::printf("  with town links: %llu herds (commute %.0f, home %.0f, shop %.0f), %d cars at most in the morning\n",
                    (unsigned long long)w.hs.stats.herdsReleased, commuted, home, w.hs.stats.released[kTripShop], morning);
        CHECK(morning > 0);
        CHECK(commuted > 20.0f && home > 20.0f); // to work in the morning and back in the evening
        CHECK(w.hs.stats.released[kTripShop] > 0.0f);
        // morning and evening: the commute leaves before 10:00, the trip home after 15:00
        World e;
        SmallTown(e);
        e.Finish();
        const int evening = e.Day(true, {380, 40}, 16 * 60, 19 * 60);
        std::printf("  evening: %d cars at most\n", evening);
        CHECK(evening > 0);
        // the flow from the same links: cosmetic cars on the town's street
        const std::vector<SegFlow> f = SolveFlow(w);
        float sum = 0;
        int busy = 0;
        for (const SegFlow& s : f) sum += s.flow, busy += s.flow > 1.0f;
        std::vector<CarInstance> cars;
        AgentParams ap;
        CosmeticCars(w.net, w.grid, f, {380, 40}, 100.0, ap, cars);
        // a peak hour of a 250-resident town is a few cars a minute: the flow cars (TBD_HERDS=0) round most segments to none
        std::printf("  flow: %.0f veh/h over the street, %d busy segments, %zu cosmetic cars\n", sum, busy, cars.size());
        CHECK(sum > 0 && busy >= 4);
        // parked: home at night, at work at noon
        std::vector<CarInstance> night, noon;
        w.town.Parked({380, 40}, 600, 2 * 60, night, 300);
        w.town.Parked({380, 40}, 600, 12 * 60, noon, 300);
        int nightWest = 0, noonWest = 0;
        for (const CarInstance& c : night) nightWest += c.p.x < 384;
        for (const CarInstance& c : noon) noonWest += c.p.x < 384;
        std::printf("  parked: %zu at night (%d by the houses), %zu at noon (%d by the houses), %zu spots\n", night.size(), nightWest, noon.size(), noonWest,
                    w.town.SpotCount());
        CHECK(!night.empty() && !noon.empty());
        CHECK(nightWest > noonWest);                                   // the residents drove to work
        CHECK((int)noon.size() - noonWest > (int)night.size() - nightWest); // and parked by the offices
        for (const CarInstance& c : night) CHECK(std::fabs(c.p.z - 40.0f) > 3.0f && std::fabs(c.p.z - 40.0f) < 8.0f); // at the curb
    }

    // ---- an outside connection: a highway across the whole map
    {
        World w;
        int west = -1, east = -1;
        Highway(w, west, east);
        const int lane = ProfileIndex("Two-Lane Road");
        const int d0 = w.net.AddNode({0, 0, 1400}, true), d1 = w.net.AddNode({200, 0, 1400}, true);
        Seg(w.net, d0, d1, lane); // a dead end in the middle of the map: not a connection
        w.Finish();
        CHECK(w.town.stats.conns == 2);
        CHECK(w.town.conns.size() == 2 && w.town.conns[0].node == west && w.town.conns[1].node == east);
        int through = 0;
        for (const NodeLink& l : w.town.links) through += l.type == kTripThrough;
        CHECK(through == 2 && w.town.stats.throughPerDay > 1000.0f);
        int most[3] = {};
        const float xs[3] = {-3500.0f, 0.0f, 3500.0f};
        for (int k = 0; k < 3; ++k) {
            World v;
            int a, b;
            Highway(v, a, b);
            v.Finish();
            most[k] = v.Day(true, {xs[k], 600}, 7 * 60, 9 * 60);
        }
        w.Day(true, {0, 600});
        float longestM = 0;
        for (const RouteStore::Rec& r : w.hs.routes.recs) longestM = std::max(longestM, r.len);
        std::printf("highway: %.0f through a day, released %.0f; cars near the west end %d, the middle %d, the east end %d; route %.0f m\n",
                    w.town.stats.throughPerDay, w.hs.stats.released[kTripThrough], most[0], most[1], most[2], longestM);
        CHECK(w.hs.stats.released[kTripThrough] > 1000.0f);
        CHECK(longestM > 7900.0f); // the route runs edge to edge
        CHECK(most[0] > 0 && most[1] > 0 && most[2] > 0);
        // the flow: every highway segment carries it, and cosmetic cars show along it
        const std::vector<SegFlow> f = SolveFlow(w);
        bool all = true;
        for (size_t s = 0; s < 8; ++s) all &= f[s].flow > 10.0f;
        CHECK(all);
        AgentParams ap;
        for (float x : xs) {
            std::vector<CarInstance> cars;
            CosmeticCars(w.net, w.grid, f, {x, 600}, 50.0, ap, cars);
            CHECK(!cars.empty());
        }
        // a town on the highway: more through traffic, and commuters, visitors and trucks from the edge
        World c;
        int a, b;
        Highway(c, a, b);
        SmallTown(c);
        Seg(c.net, 9, 4, lane); // a link road from the street's west end (node 9) up to the highway's middle node (4)
        c.Finish();
        std::printf("  with a town: %.0f through, %.0f in, %.0f out a day\n", c.town.stats.throughPerDay, c.town.stats.inPerDay, c.town.stats.outPerDay);
        CHECK(c.town.stats.throughPerDay > w.town.stats.throughPerDay);
        CHECK(c.town.stats.inPerDay > 0);
    }

    // ---- no roads: no links, no flow, no cars
    {
        World w;
        uint64_t id = 1;
        for (int x = 0; x < 3; ++x) w.lots.push_back(MakeLot(id++, 64.0f + 128.0f * x, 64.0f, 2, 6, 6));
        w.lots.push_back(MakeLot(id++, 64.0f + 128.0f * 4, 64.0f, 5, 6, 6));
        w.Finish();
        CHECK(w.town.links.empty() && w.town.conns.empty() && w.town.SpotCount() == 0);
        const int most = w.Day(true, {200, 64});
        CHECK(most == 0 && w.hs.stats.herdsReleased == 0);
        std::vector<Trip> trips;
        w.town.FlowTrips(w.hs.curves, w.g, trips);
        CHECK(trips.empty());
        std::vector<CarInstance> parked;
        w.town.Parked({200, 64}, 600, 120, parked, 300);
        CHECK(parked.empty());
    }

    // ---- determinism: the same world twice gives the same links, herds and parked cars
    {
        World a, b;
        int x0, x1;
        Highway(a, x0, x1), SmallTown(a);
        Highway(b, x0, x1), SmallTown(b);
        Seg(a.net, 9, 4, ProfileIndex("Two-Lane Road"));
        Seg(b.net, 9, 4, ProfileIndex("Two-Lane Road"));
        a.Finish(), b.Finish();
        bool same = a.town.links.size() == b.town.links.size() && !a.town.links.empty();
        for (size_t i = 0; same && i < a.town.links.size(); ++i) {
            const NodeLink &p = a.town.links[i], &q = b.town.links[i];
            same = p.fromNode == q.fromNode && p.toNode == q.toNode && p.n == q.n && p.type == q.type;
        }
        CHECK(same);
        const int ma = a.Day(true, {400, 300}), mb = b.Day(true, {400, 300});
        CHECK(ma == mb && a.hs.stats.herdsReleased == b.hs.stats.herdsReleased);
        CHECK(a.hs.stats.released[kTripThrough] == b.hs.stats.released[kTripThrough]);
        std::vector<CarInstance> pa, pb;
        a.town.Parked({380, 40}, 600, 600, pa, 300);
        b.town.Parked({380, 40}, 600, 600, pb, 300);
        bool parkedSame = pa.size() == pb.size() && !pa.empty();
        for (size_t i = 0; parkedSame && i < pa.size(); ++i) parkedSame = pa[i].p.x == pb[i].p.x && pa[i].p.z == pb[i].p.z && pa[i].model == pb[i].model;
        CHECK(parkedSame);
        std::printf("determinism: %zu links, %llu herds, %d cars at most, %zu parked\n", a.town.links.size(), (unsigned long long)a.hs.stats.herdsReleased, ma, pa.size());
    }

    if (failures) {
        std::printf("sim_traffic_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_traffic_test: all passed\n");
    return 0;
}
