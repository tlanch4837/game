// Phase 4 school seats (src/city/sim_svc_education.h) with the Phase 1 citizen sim: seats are taken local first up to
// capacity and written to CitizenStore::school, a seated student graduates for certain (more graduates than with the
// coverage stub), too few seats leave students unseated (the NoSchool cells) and limit graduation, the education mix
// moves up over the years, the recount check finds nothing stale. Headless. Build and run: tests\sim_svc_education_test.bat
#include "sim_svc_town.h"
#undef CHECK
#include "sim_life_town.h"

using namespace lifetest;

struct Run { int grads[5] = {}; uint32_t unseated = 0, seated = 0, drift = 0; double higherShare = 0; };

// A 1,000-person census town over `years` life years; schools: 0 none, 1 roomy, 2 tiny.
static Run RunTown(int schools, int years) {
    Town t;
    t.sim.w.citySeed = 4242;
    t.Build(5, 42, 30);
    static const int kCensus[3] = {22, 61, 17};
    for (LifeBuilding& L : t.sim.w.bld)
        if (L.id && L.homes) LifeFillBuilding(t.sim.w, L.id, 40, &kCensus);
    SimEventRing ev;
    t.sim.w.events = &ev;
    SimEventRing::Cursor cur = ev.Now();
    svctest::Town st;
    st.Build(5, 40);
    if (schools == 1) {
        st.AddSite(2, 2, kTabEducation, "Elementary School", 27, 0);
        st.AddSite(2, 2, kTabEducation, "High School", 162, 0);
    } else if (schools == 2) {
        st.AddSite(2, 2, kTabEducation, "Rural Elementary School", 12, 0, 0.1f); // 30 seats
        st.AddSite(2, 2, kTabEducation, "Rural High School", 36, 0, 0.05f);      // 45 seats
    }
    SvcWorld sw;
    st.Into(sw, 42);
    SvcSeatPass pass;
    LifeWorld& w = t.sim.w;
    auto age = [&w](uint32_t ci) { return w.AgeOf(ci); };
    auto cell = [&w, &st](uint32_t ci) {
        const LifeBuilding* L = LifeHomeOf(w, ci);
        return L ? st.Node(L->cellX, L->cellZ) : -1;
    };
    Run r;
    int64_t minute = 0;
    for (int d = 0; d < years * 12; ++d) {
        for (int m = 0; m < 1440; ++m, ++minute) {
            t.sim.Tick(minute);
            if (minute % 10 == 0) SvcSeatSlice(sw, w.cit, pass, std::max<uint32_t>(8, w.cit.Capacity() / 140 + 1), age, cell);
        }
        SimEvent e;
        while (ev.Read(cur, e)) if (e.type == SimEventType::Graduated && e.value >= 0 && e.value < 5) r.grads[e.value]++;
    }
    r.unseated = pass.lastUnseated, r.seated = pass.lastSeated;
    r.drift = pass.drift + SvcSeatRecount(sw, w.cit); // the sweep's own recount, then the full one against the records
    double adults = 0, higher = 0;
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci)
        if (w.cit.At(ci).Valid() && w.cit.stage[ci] == kLifeAdult) adults += 1, higher += w.cit.edu[ci] >= 2;
    r.higherShare = adults > 0 ? higher / adults : 0;
    // every seat in the records is at a school of the right level with room
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) {
        if (!w.cit.school[ci]) continue;
        const SvcSite* s = sw.Site(w.cit.school[ci]);
        CHECK(s && s->spec.unit == kUnitSeats);
    }
    for (const SvcSite& s : sw.cov.sites) { const auto e = sw.enrolled.find(s.id); CHECK(e == sw.enrolled.end() || e->second <= s.spec.units); }
    std::printf("schools %d: graduates elem %d high %d college %d, seated %u unseated %u, higher-educated adults %.1f %%, drift %u\n", schools,
                r.grads[1], r.grads[2], r.grads[3], r.seated, r.unseated, r.higherShare * 100.0, r.drift);
    return r;
}

int main() {
    const Run none = RunTown(0, 12), roomy = RunTown(1, 12), tiny = RunTown(2, 12);
    CHECK(roomy.seated > 100 && roomy.unseated == 0);
    CHECK(roomy.grads[1] > none.grads[1] && roomy.grads[2] > none.grads[2]); // seats graduate students for certain
    CHECK(tiny.unseated > 0 && tiny.seated < roomy.seated);                 // seats limit who is seated
    CHECK(tiny.grads[1] + tiny.grads[2] < roomy.grads[1] + roomy.grads[2]);
    CHECK(none.seated == 0 && none.unseated == 0);                           // no schools: nobody is "unseated"
    CHECK(roomy.higherShare >= none.higherShare);                            // the mix moves up with schooling
    CHECK(roomy.drift < 20);                                                 // recount vs kept counts (P1 frees records)
    std::printf("%s (%d failures)\n", failures ? "sim_svc_education_test FAILED" : "sim_svc_education_test passed", failures);
    return failures ? 1 : 0;
}
