// Headless checks for the visible cars (src/city/sim_children.h; the consistency checks are in sim_cars_consistency_test):
//   the IDM never collides and queues behind a stopped car; the same tenant slot always gives the same look and car; the
//   herds' releases on a route become a stream whose cars track the release rate (and the budget caps them); cars go when
//   the camera leaves; no allocation after warm-up; a followed (promoted) car is kept and drives to its driveway.
// Build and run: tests\sim_children_test.bat
#include <cstdio>
#include <set>
#include <vector>

#include "city/sim_children.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// two 2 km links in a row along +x: nodes 0 -> 1 -> 2, `lanes` car lanes
struct Scene {
    LinkTable t;
    HerdSystem hs;
    ChildSystem cs;
    uint32_t route = 0;
    explicit Scene(int lanes = 1, int maxChildren = 300) {
        t.AddStraight(0, 1, {0, 0}, {2000, 0}, 50, 1200 * lanes, lanes);
        t.AddStraight(1, 2, {2000, 0}, {4000, 0}, 50, 1200 * lanes, lanes);
        t.nodes = 3;
        t.BuildAdjacency();
        hs.SetTable(&t);
        hs.rateMinutes = 15; // a release is a window's rate at once (the clock stands still here)
        route = hs.routes.Intern(t, {0, 1});
        cs.params.maxChildren = maxChildren;
        cs.params.radius = 600;
        cs.SetTable(&t, &hs.routes);
        cs.nextHop = [this](int node, int dest) { return node == 0 && dest > 0 ? 0 : node == 1 && dest == 2 ? 1 : -1; };
    }
    // A window's herd on the route (commuters from the map edge to hood 5's driveways): `size` vehicles over 15 game minutes.
    void SetHerd(float size) {
        Herd h;
        h.size = size, h.route = route, h.from = -1, h.to = 5, h.homeHood = 3, h.workHood = 5; // in from the map edge (node 0)
        hs.Inject(h);
    }
    int Active(bool promotedToo = false) const {
        int n = 0;
        for (const Child& c : cs.pool) n += c.active && (promotedToo || !c.promoted);
        return n;
    }
};

int main() {
    // ---- IDM: queues behind a stopped car, never collides
    for (float v0 : {0.0f, 14.0f, 25.0f}) {
        Scene s;
        s.cs.params.radius = 5000;
        Child lead;
        lead.link = 0, lead.s = 1500, lead.hold = true, lead.promoted = true, lead.look.speedFactor = 1;
        Child f;
        f.link = 0, f.s = 1000, f.v = v0, f.promoted = true, f.look.speedFactor = 1, f.look.aMax = 1.5f, f.look.timeGap = 1.2f;
        s.cs.Add(lead);
        const int fi = s.cs.Add(f);
        float minGap = 1e9f;
        for (int i = 0; i < 1800; ++i) { // 180 s
            s.cs.Update(0.1, 0.1, {1200, 0}, s.hs, 0);
            const Child& A = s.cs.pool[0];
            const Child& B = s.cs.pool[(size_t)fi];
            minGap = std::min(minGap, A.s - B.s - 4.5f);
        }
        const Child& B = s.cs.pool[(size_t)fi];
        CHECK(minGap > 0.0f);
        CHECK(B.v < 0.2f);
        CHECK(1500 - B.s - 4.5f > 0.8f && 1500 - B.s - 4.5f < 4.5f); // stopped a car's length behind plus the standstill gap
        CHECK(s.cs.stats.clamps == 0);                              // the IDM braked in time; the hard gap clamp was never needed
    }
    // a free road: the car settles near its desired speed
    {
        Scene s;
        s.cs.params.radius = 5000;
        Child f;
        f.link = 0, f.s = 100, f.v = 0, f.promoted = true, f.look.speedFactor = 1;
        const int fi = s.cs.Add(f);
        for (int i = 0; i < 400; ++i) s.cs.Update(0.1, 0.1, {1200, 0}, s.hs, 0);
        const float v0 = s.hs.LinkSpeed(0) / 60.0f;
        CHECK(std::fabs(s.cs.pool[(size_t)fi].v - v0) < 0.7f);
    }
    // an emergency vehicle behind: the car ahead pulls aside, the siren passes, the car rejoins
    {
        Scene s;
        s.cs.params.radius = 5000;
        Child a;
        a.link = 0, a.s = 1000, a.v = 10, a.promoted = true;
        Child e;
        e.link = 0, e.s = 940, e.v = 16, e.promoted = true, e.emergency = true, e.look.speedFactor = 1.4f;
        const int ai = s.cs.Add(a), ei = s.cs.Add(e);
        float maxLat = 0;
        for (int i = 0; i < 300; ++i) {
            s.cs.Update(0.1, 0.1, {1100, 0}, s.hs, 0);
            maxLat = std::max(maxLat, s.cs.pool[(size_t)ai].lat);
        }
        CHECK(maxLat > 1.5f);
        CHECK(s.cs.pool[(size_t)ei].s > s.cs.pool[(size_t)ai].s); // it got past
        for (int i = 0; i < 200; ++i) s.cs.Update(0.1, 0.1, {1100, 0}, s.hs, 0);
        CHECK(s.cs.pool[(size_t)ai].lat < 0.3f);                  // and the car rejoined
    }

    // ---- looks come from the slot
    {
        std::set<int> models;
        bool same = true, differs = false;
        for (uint32_t i = 0; i < 400; ++i) {
            const Look a = LookFor(9, 1234, i, 8), b = LookFor(9, 1234, i, 8);
            same &= a.model == b.model && a.colour == b.colour && a.speedFactor == b.speedFactor && a.timeGap == b.timeGap;
            differs |= a.speedFactor != LookFor(9, 1234, i + 1, 8).speedFactor;
            models.insert(a.model);
            CHECK(a.speedFactor >= 0.88f && a.speedFactor <= 1.18f);
        }
        CHECK(same && differs && models.size() == 8);
        CHECK(LookFor(9, 1234, 3, 8).speedFactor != LookFor(10, 1234, 3, 8).speedFactor);
    }
    // a tenant slot comes from the hood's real residents, and the same roll gives the same slot
    {
        CitySim sim;
        std::vector<Lot> lots;
        for (int i = 0; i < 4; ++i) {
            Lot l;
            l.id = (uint64_t)i + 1, l.c = {(i + 0.5f) * 128.0f, 64.0f}, l.type = 3, l.w = 4, l.d = 4, l.progress = 2;
            l.path = "T3_4x4_L1.glb";
            lots.push_back(l);
        }
        sim.Sync(lots, 1);
        Network net;
        SegGrid grid;
        grid.Build(net);
        SimHoods hoods;
        hoods.Build(sim, net, grid);
        CHECK(!hoods.hoods.empty());
        TenantSlot a, b;
        CHECK(PickTenantSlot(sim, hoods, 0, true, 12345, a) && PickTenantSlot(sim, hoods, 0, true, 12345, b));
        CHECK(a.cell == b.cell && a.index == b.index && a.address == b.address);
        CHECK(a.cell >= 0 && a.index < sim.sim[(size_t)a.cell].residents[kAdults]);
        CHECK(!PickTenantSlot(sim, hoods, 0, false, 1, a)); // nobody works in a housing hood
    }

    // ---- the herds' releases on a route become a stream of trips: the cars on the road track the release rate, take their
    // look from a tenant slot, keep their trip's speed, and never overlap in a lane
    auto run = [](Scene& s, int frames, V2 focus) {
        for (int i = 0; i < frames; ++i) s.cs.Update(0.05, 0.05, focus, s.hs, 0);
    };
    int n450 = 0;
    {
        Scene s(2);
        s.cs.params.densityScale = 1;
        uint32_t next = 0;
        s.cs.tenant = [&](const Herd&, bool, uint64_t, TenantSlot& out) {
            out.cell = 1, out.index = next++, out.address = 0xABCD;
            return true;
        };
        s.SetHerd(450); // 450 vehicles in a 15-minute window: one every two seconds
        run(s, 40, {1000, 0});
        n450 = s.Active();
        const float want = s.cs.LinkRate(0) * 1720.0f / s.cs.FreeSpeed(0); // over the detail radius's 1720 m of road
        std::printf("stream of 0.5/s: %d cars near the camera (the stream says %.0f), budget x%.2f\n", n450, want, s.cs.BudgetScale());
        CHECK(std::fabs(s.cs.LinkRate(0) - 0.5f) < 1e-3f);
        CHECK(n450 > 0.75f * want && n450 < 1.15f * want);
        for (const Child& c : s.cs.pool)
            if (c.active) {
                const Look l = LookFor(s.cs.params.citySeed, c.slot.address, c.slot.index, s.cs.params.models);
                CHECK(c.look.model == l.model && c.look.speedFactor >= 0.9f && c.look.speedFactor <= 1.1f); // the trip's speed, the slot's car
                CHECK(c.slot.address == 0xABCD && c.route == s.route);
            }
        for (const Child& a : s.cs.pool)
            for (const Child& b : s.cs.pool)
                if (&a != &b && a.active && b.active && a.link == b.link && a.lane == b.lane) CHECK(std::fabs(a.s - b.s) > 4.5f);
        // the camera leaves: the cars go (past the detail radius, out of sight)
        run(s, 60, {-5000, 0});
        CHECK(s.Active() == 0);
    }
    {
        Scene s(2);
        s.cs.params.densityScale = 1;
        s.SetHerd(900);
        run(s, 40, {1000, 0});
        const int n900 = s.Active();
        std::printf("stream of 1/s: %d cars\n", n900);
        CHECK(n900 > n450 * 1.6 && n900 < n450 * 2.4);
    }
    {
        Scene s(1, 40); // budget of 40
        s.cs.params.densityScale = 1;
        s.SetHerd(9000);
        run(s, 80, {1500, 0});
        std::printf("budget 40: %d cars, scale x%.3f\n", s.Active(), s.cs.BudgetScale());
        CHECK(s.Active() <= 40 && s.Active() >= 24);
        CHECK(s.cs.BudgetScale() < 0.2f);
    }

    // ---- no allocation after warm-up: the pool and the tables keep their size under a moving camera and new releases
    {
        Scene s;
        s.cs.params.densityScale = 1;
        s.SetHerd(600);
        run(s, 20, {1000, 0});
        const size_t fp = s.cs.Fingerprint();
        CHECK(s.cs.pool.size() == 300);
        double minute = 0;
        for (int i = 0; i < 2000; ++i) {
            minute += 0.1;
            if (i % 150 == 0) s.SetHerd(100.0f + (float)((i / 150) * 70 % 500));
            s.cs.Update(0.05, 0.05, {1000.0f + (float)(i % 900) * 2.0f, 0}, s.hs, minute);
        }
        CHECK(s.cs.Fingerprint() == fp && s.cs.pool.size() == 300);
    }

    // ---- promotion: the followed car is never dropped; it drives on to its driveway and is gone
    {
        Scene s;
        s.cs.params.densityScale = 1;
        s.SetHerd(300);
        run(s, 20, {1000, 0});
        const int i = s.cs.Nearest({1000, 0}, 400);
        CHECK(i >= 0);
        CHECK(s.cs.Promote(i) && s.cs.stats.promoted == 1 && !s.cs.Promote(i));
        const uint32_t serial = s.cs.pool[(size_t)i].serial;
        run(s, 100, {-5000, 0}); // the camera looks elsewhere: the followed car stays
        CHECK(s.cs.pool[(size_t)i].active && s.cs.pool[(size_t)i].serial == serial);
        for (int k = 0; k < 6000 && s.cs.pool[(size_t)i].active; ++k) s.cs.Update(0.1, 0.1, {-5000, 0}, s.hs, 0);
        CHECK(!s.cs.pool[(size_t)i].active && s.cs.stats.promoted == 0 && s.cs.stats.arrivals > 0);
    }

    if (failures == 0) std::printf("sim_children_test: all checks passed\n");
    return failures ? 1 : 0;
}
