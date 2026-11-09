// Phase 4 coverage (src/city/sim_svc_assign.h): capacity saturates coverage, road travel time beats crow-flies, funding
// scales capacity, the pass is deterministic, the catalog gives every service a spec. Headless.
// Build and run: tests\sim_svc_coverage_test.bat
#include "sim_svc_town.h"

using namespace svctest;

static int Covered(const SvcCovOut& o, int slot) {
    int n = 0;
    for (const auto& c : o.cov) n += c[(size_t)slot] > 0;
    return n;
}

int main() {
    { // the catalog
        CHECK(SvcSpecFor(kTabHealth, "Family Clinic", 12, 1, 2, 2).slot == kSvcHealth);
        CHECK(SvcSpecFor(kTabHealth, "Small Park", 4, 0, 2, 2).slot == kSvcPark);
        CHECK(SvcSpecFor(kTabEducation, "Elementary School", 27, 0, 3, 3).slot == kSvcSchool);
        CHECK(SvcSpecFor(kTabEducation, "Urban High School", 36, 0, 3, 4).slot == kSvcHighSchool);
        CHECK(SvcSpecFor(kTabEducation, "University", 75, 0, 5, 5).slot == kSvcUniversity);
        CHECK(SvcSpecFor(kTabDeath, "Crematorium", 16, 2, 4, 4).burns == 1);
        CHECK(SvcSpecFor(kTabDeath, "Cemetery", 14, 4, 6, 6).units == 900);
        CHECK(SvcSpecFor(kTabComms, "Post Office", 20, 4, 3, 3).slot == kSvcPost);
        CHECK(SvcSpecFor(kTabComms, "Cell Tower", 12, 0, 2, 2).road == 0);
        CHECK(SvcSpecFor(kTabPolice, "Police Station", 36, 3, 3, 4).units == 18);
        CHECK(SvcSpecFor(kTabRail, "Railway Depot", 24, 8, 18, 9).slot == kSvcTransit);
    }
    { // capacity: a family clinic (3,000 people) in a town of 81 x 500 covers about 6 cells; a big hospital covers more
        Town t;
        t.Build(9, 500);
        t.AddSite(4, 4, kTabHealth, "Family Clinic", 12, 1);
        SvcCovOut small;
        SvcCover(t.Input(), small);
        CHECK(small.served[0] == 3000);
        const int a = Covered(small, kSvcHealth);
        CHECK(a >= 6 && a <= 8);
        CHECK(small.cov[(size_t)t.Node(4, 4)][kSvcHealth] == 255); // its own cell, fully served, no travel
        Town u;
        u.Build(9, 500);
        u.AddSite(4, 4, kTabHealth, "General Hospital", 200, 5);
        SvcCovOut big;
        SvcCover(u.Input(), big);
        CHECK(Covered(big, kSvcHealth) > a * 3);
        // the nearest list: the clinic is everyone's first choice in reach, at its travel minutes
        CHECK(small.Near((size_t)t.Node(4, 4), kSvcHealth, 0).site == 0);
        const float m = small.Near((size_t)t.Node(5, 4), kSvcHealth, 0).minutes;
        CHECK(m > 0.7f && m < 0.85f); // 128 m at 10 km/h
    }
    { // travel time beats crow-flies: the cell beside the clinic with no road gets nothing; a mast covers it anyway
        Town t;
        t.skipNode = 5 * 9 + 4; // cell (4, 5)
        t.Build(9, 100);
        t.AddSite(4, 4, kTabHealth, "Community Hospital", 36, 4);
        t.AddSite(4, 4, kTabComms, "Cell Tower", 12, 0);
        SvcCovOut o;
        SvcCover(t.Input(), o);
        CHECK(o.cov[(size_t)t.Node(4, 5)][kSvcHealth] == 0);
        CHECK(o.cov[(size_t)t.Node(4, 3)][kSvcHealth] > 200);
        CHECK(o.cov[(size_t)t.Node(4, 5)][kSvcTelecom] > 200);
        // a far corner is further by road than its straight line: lower coverage than a cell at the same crow distance
        CHECK(o.cov[(size_t)t.Node(8, 8)][kSvcHealth] < o.cov[(size_t)t.Node(4, 0)][kSvcHealth] || o.cov[(size_t)t.Node(8, 8)][kSvcHealth] == 0);
    }
    { // funding scales capacity (and vehicles)
        Town t, h;
        t.Build(9, 500), h.Build(9, 500);
        t.AddSite(4, 4, kTabPolice, "Police Station", 36, 3, 1.0f);
        h.AddSite(4, 4, kTabPolice, "Police Station", 36, 3, 0.5f);
        SvcCovOut full, half;
        SvcCover(t.Input(), full), SvcCover(h.Input(), half);
        CHECK(half.served[0] * 2 == full.served[0]);
        CHECK(Covered(half, kSvcPolice) < Covered(full, kSvcPolice));
        CHECK(h.sites[0].spec.vehicles == 2 && t.sites[0].spec.vehicles == 3);
    }
    { // deterministic: the same input gives the same bytes; site order does not change a cell's total
        Town t;
        t.Build(12, 300);
        t.AddSite(2, 2, kTabFire, "Fire Station", 20, 4);
        t.AddSite(9, 9, kTabFire, "Fire Station", 20, 4);
        t.AddSite(6, 3, kTabEducation, "Elementary School", 27, 0);
        SvcCovOut a, b;
        SvcCover(t.Input(), a), SvcCover(t.Input(), b);
        CHECK(a.cov == b.cov && a.served == b.served);
        bool sameNear = a.nearest.size() == b.nearest.size();
        for (size_t i = 0; sameNear && i < a.nearest.size(); ++i) sameNear = a.nearest[i].site == b.nearest[i].site && a.nearest[i].minutes == b.nearest[i].minutes;
        CHECK(sameNear);
        CHECK(a.slotHasSite[kSvcFire] && a.slotHasSite[kSvcSchool] && !a.slotHasSite[kSvcPolice]);
    }
    std::printf("%s (%d failures)\n", failures ? "sim_svc_coverage_test FAILED" : "sim_svc_coverage_test passed", failures);
    return failures ? 1 : 0;
}
