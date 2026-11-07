// Headless checks for visible children and promotion (src/city/sim_children.h):
//   the IDM never collides and queues behind a stopped car; the same tenant slot always gives the same look and car; the
//   number of cars on a link tracks the herd load (and the budget caps it); cars leave when the herds are out of range; no
//   allocation after warm-up; a car watched for a second is promoted (at most 32), drives its own route and parks.
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
        route = hs.routes.Intern(t, {0, 1});
        cs.params.maxChildren = maxChildren;
        cs.params.radius = 600;
        cs.SetTable(&t, &hs.routes);
        cs.nextHop = [this](int node, int dest) { return node == 0 && dest > 0 ? 0 : node == 1 && dest == 2 ? 1 : -1; };
    }
    void SetHerd(float size, float progress = 1000, float sd = 0.1f) {
        hs.herds.clear();
        Herd h;
        h.id = 7, h.alive = true, h.size = size, h.route = route, h.progress = progress, h.speedSd = sd, h.to = 5, h.homeHood = 3, h.workHood = 5;
        h.distance = hs.routes.recs[route].len;
        hs.herds.push_back(h);
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

    // ---- spawn counts track herd load, take their look from the slot, and stay in budget
    {
        Scene s(2);
        uint32_t next = 0;
        s.cs.tenant = [&](const Herd&, bool, uint64_t, TenantSlot& out) {
            out.cell = 1, out.index = next++, out.address = 0xABCD;
            return true;
        };
        s.SetHerd(40, 1000, 0.25f);
        for (int i = 0; i < 80; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        const int n40 = s.Active();
        CHECK(n40 >= 10 && n40 <= 14); // 40 vehicles over a 200 m span x 0.3
        for (const Child& c : s.cs.pool)
            if (c.active) {
                const Look l = LookFor(s.cs.params.citySeed, c.slot.address, c.slot.index, s.cs.params.models);
                CHECK(c.look.model == l.model && c.look.speedFactor == l.speedFactor);
                CHECK(c.link == 0 && c.s >= 650 && c.s <= 1350);
            }
        s.SetHerd(80, 1000, 0.25f);
        for (int i = 0; i < 80; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        const int n80 = s.Active();
        CHECK(n80 > n40 * 1.5 && n80 <= n40 * 2 + 3);
        // cars never overlap in a lane
        for (const Child& a : s.cs.pool)
            for (const Child& b : s.cs.pool)
                if (&a != &b && a.active && b.active && a.link == b.link && a.lane == b.lane) CHECK(std::fabs(a.s - b.s) > 4.5f);
        // the herds are out of range: the cars go
        for (int i = 0; i < 60; ++i) s.cs.Update(0.05, 0.05, {-5000, 0}, s.hs, 0);
        CHECK(s.Active() == 0);
    }
    {
        Scene s(1, 40); // budget of 40
        s.SetHerd(4000, 2000, 0.4f);
        for (int i = 0; i < 80; ++i) s.cs.Update(0.05, 0.05, {1500, 0}, s.hs, 0);
        CHECK(s.Active() <= 40 && s.Active() >= 28);
    }

    // ---- no allocation after warm-up: the pool and the tables keep their size under changing loads
    {
        Scene s;
        s.SetHerd(60);
        for (int i = 0; i < 20; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        const size_t fp = s.cs.Fingerprint();
        CHECK(s.cs.pool.size() == 300);
        for (int i = 0; i < 400; ++i) {
            if (i % 50 == 0) s.SetHerd(10.0f + (float)((i / 50) * 70 % 500), 400.0f + 20.0f * (float)i);
            s.cs.Update(0.05, 0.05, {1000.0f + (float)(i % 90) * 10.0f, 0}, s.hs, 0);
        }
        CHECK(s.cs.Fingerprint() == fp && s.cs.pool.size() == 300);
        CHECK(s.cs.stats.clamps == 0);
    }

    // ---- promotion: watched in the middle for a second, capped at 32, then it drives to its destination and parks
    {
        Scene s(4);
        s.cs.params.watchRadius = 200;
        s.SetHerd(2500, 1000, 0.15f);
        for (int i = 0; i < 10; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        CHECK(s.cs.stats.promotions == 0); // half a second so far
        CHECK(s.Active(true) > 40);
        for (int i = 0; i < 40; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        CHECK(s.cs.stats.promoted == 32 && s.cs.stats.promotions == 32);
        for (int i = 0; i < 40; ++i) s.cs.Update(0.05, 0.05, {1000, 0}, s.hs, 0);
        CHECK(s.cs.stats.promoted <= 32);
        // they drive on: destination is the end of the herd's route (node 2); each ends parked on the last link
        for (int i = 0; i < 4000 && s.cs.stats.parked < 20; ++i) s.cs.Update(0.1, 0.05, {3000, 0}, s.hs, 0);
        int parked = 0;
        for (const Child& c : s.cs.pool)
            if (c.active && c.parked) {
                ++parked;
                CHECK(c.link == 1 && c.promoted && c.v == 0 && c.s > 1900 - 1 && c.s <= 2000);
            }
        CHECK(parked >= 20);
        CHECK(s.cs.stats.arrivals >= 20);
        std::vector<CarInstance> moving, park;
        s.cs.Update(0.1, 0.05, {3900, 0}, s.hs, 0); // the camera comes to the parking lot
        s.cs.Collect(moving, park, {3900, 0});
        CHECK(!park.empty());
        for (const CarInstance& ci : park) CHECK(std::fabs(ci.p.z) > 4 * 3.2f); // on the strip beside the four lanes
        // after the parked time the car leaves
        s.cs.Update(0.1, 0.05, {3000, 0}, s.hs, 1e6);
        CHECK(s.cs.stats.parked == 0);
    }

    if (failures == 0) std::printf("sim_children_test: all checks passed\n");
    return failures ? 1 : 0;
}
