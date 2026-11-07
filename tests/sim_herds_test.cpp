// Headless checks for herds over the day (src/city/sim_herds.h):
//   the day's herd sizes add up to the link counts x car share (released + still pending); the morning flow points to the
//   job hood and the evening reverses it; a loaded link is slower (BPR); a herd with a speed spread dissolves over several
//   minutes, one without arrives in one tick; heavier demand takes longer; school drop-offs chain on to work; the same
//   seed gives the same day.
// Build and run: tests\sim_herds_test.bat
#include <cstdio>
#include <vector>

#include "city/sim_herds.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// suburb (node 0) - ramp (node 1) - downtown (node 2): a 3 km local street, a 10 km highway pipe, both ways
struct Scene {
    LinkTable t;
    std::vector<Hood> hoods;
    std::vector<HoodLink> commute, school, shop;
    uint32_t r[3][3];
    HerdSystem hs;
    Scene() {
        t.AddStraight(0, 1, {0, 0}, {3000, 0}, 50, 1200);               // 0
        t.AddStraight(1, 2, {3000, 0}, {13000, 0}, 90, 3600, 2, true);  // 1
        t.AddStraight(2, 1, {13000, 4}, {3000, 4}, 90, 3600, 2, true);  // 2
        t.AddStraight(1, 0, {3000, 4}, {0, 4}, 50, 1200);               // 3
        t.nodes = 3;
        t.BuildAdjacency();
        hoods.resize(3); // 0 suburb, 1 downtown, 2 the ramp (a school there)
        hoods[0].id = 0, hoods[0].centroidNode = 0;
        hoods[1].id = 1, hoods[1].centroidNode = 2;
        hoods[2].id = 2, hoods[2].centroidNode = 1;
        hs.SetTable(&t);
        for (auto& row : r)
            for (uint32_t& v : row) v = ~0u;
        r[0][1] = hs.routes.Intern(t, {0, 1}); // node pairs are (node a, node b) -> index by node
        r[2][0] = hs.routes.Intern(t, {2, 3});
        r[0][2] = r[0][1];
        r[0][1] = hs.routes.Intern(t, {0});
        r[1][0] = hs.routes.Intern(t, {3});
        r[1][2] = hs.routes.Intern(t, {1});
        r[2][1] = hs.routes.Intern(t, {2});
    }
    RouteFn Fn() {
        return [this](int a, int b, RouteChoice* out) {
            if (a < 0 || a > 2 || b < 0 || b > 2 || r[a][b] == ~0u) return 0;
            out[0].route = r[a][b];
            out[0].share = 1;
            return 1;
        };
    }
};

struct DayResult {
    float releasedCommute = 0, releasedHome = 0, pending = 0, deliveredCommute = 0, deliveredHome = 0, deliveredChain = 0;
    float arrivedJobAt9 = 0, arrivedHomeAt9 = 0, arrivedJobAt1830 = 0, arrivedHomeAt1830 = 0, jobAt15 = 0, homeAt15 = 0;
    double meanCommuteMinutes = 0;
    HerdStats stats;
    std::vector<float> loadDay;
};

static DayResult RunDay(float workers, uint64_t seed, float sd = -1, float schoolKids = 0) {
    Scene s;
    s.hs.seed = seed;
    if (sd >= 0) s.hs.speedSdMin = s.hs.speedSdMax = sd;
    s.commute = {{0, 1, workers}};
    if (schoolKids > 0) s.school = {{0, 2, schoolKids}};
    int64_t now = 0;
    double dur = 0, vehicles = 0;
    s.hs.onArrive = [&](int, float n, const Herd& h) {
        if (h.type == kTripCommute) dur += (double)n * (double)(now - h.departMinute), vehicles += n;
    };
    DayResult r;
    for (now = 0; now < 1440 + 400; ++now) {
        if (now < 1440 && now % kWindowMinutes == 0) s.hs.ReleaseHerds(now, (int)now, s.hoods, s.commute, s.school, s.shop, s.Fn());
        s.hs.AdvanceHerds(1.0f);
        if (now == 9 * 60) r.arrivedJobAt9 = s.hs.arrivedDay[1], r.arrivedHomeAt9 = s.hs.arrivedDay[0];
        if (now == 15 * 60) r.jobAt15 = s.hs.arrivedDay[1], r.homeAt15 = s.hs.arrivedDay[0];
        if (now == 18 * 60 + 30) r.arrivedJobAt1830 = s.hs.arrivedDay[1] - r.jobAt15, r.arrivedHomeAt1830 = s.hs.arrivedDay[0] - r.homeAt15;
    }
    r.stats = s.hs.stats;
    r.releasedCommute = s.hs.stats.released[kTripCommute], r.releasedHome = s.hs.stats.released[kTripWorkHome];
    r.pending = s.hs.PendingTotal();
    r.deliveredCommute = s.hs.stats.delivered[kTripCommute], r.deliveredHome = s.hs.stats.delivered[kTripWorkHome];
    r.deliveredChain = s.hs.stats.delivered[kTripChain];
    r.meanCommuteMinutes = vehicles > 0 ? dur / vehicles : 0;
    r.loadDay = s.hs.loadDay;
    return r;
}

int main() {
    // demand curves: each trip type's windows sum to 1; the morning commute peaks 6-9, the way home 15-18
    {
        DemandCurves c;
        for (int t = 0; t < kTripTypes; ++t) {
            float sum = 0;
            for (int w = 0; w < kWindows; ++w) sum += c.c[t][w];
            CHECK(t == kTripChain ? sum == 0 : std::fabs(sum - 1.0f) < 1e-4f);
        }
        float am = 0, pm = 0;
        for (int w = 24; w < 36; ++w) am += c.c[kTripCommute][w];
        for (int w = 60; w < 72; ++w) pm += c.c[kTripWorkHome][w];
        CHECK(am > 0.6f && pm > 0.6f);
    }

    // the day's herds add up to link counts x car share
    const float workers = 3000;
    const DayResult d = RunDay(workers, 5);
    const float target = workers * 0.75f;
    CHECK(std::fabs(d.releasedCommute + d.pending * 0.5f - target) < 1.5f || d.releasedCommute <= target);
    CHECK(d.releasedCommute <= target + 0.01f && d.releasedCommute > target - 1.01f);          // under a vehicle left pending
    CHECK(d.releasedHome <= target + 0.01f && d.releasedHome > target - 1.01f);
    CHECK(d.pending < 2.0f);
    CHECK(d.deliveredCommute > target - 1.1f && d.deliveredHome > target - 1.1f);               // everyone gets there by the end of the run
    // flow points to the job hood in the morning and reverses in the evening
    CHECK(d.arrivedJobAt9 > 0.7f * target);
    CHECK(d.arrivedHomeAt9 < 0.12f * target); // night-shift workers coming home
    CHECK(d.arrivedHomeAt1830 > 0.5f * target);
    CHECK(d.arrivedJobAt1830 < 0.05f * target);
    // the highway carried both directions' herds: the forward pipe saw the morning, the reverse pipe the evening
    CHECK(d.loadDay[1] > 0.9f * target && d.loadDay[2] > 0.9f * target);

    // BPR: speed falls with load on the link
    {
        Scene s;
        s.hs.loadPrev[0] = 0;
        const float free = s.hs.LinkSpeed(0);
        CHECK(std::fabs(free - 50.0f * 1000 / 60) < 1e-2f);
        s.hs.loadPrev[0] = 1200.0f / 4; // flow = capacity
        const float atCap = s.hs.LinkSpeed(0);
        CHECK(std::fabs(atCap - free / 1.15f) < 1e-2f);
        s.hs.loadPrev[0] = 2400.0f / 4;
        CHECK(s.hs.LinkSpeed(0) < atCap && std::fabs(s.hs.LinkSpeed(0) - free / (1.0f + 0.15f * 16.0f)) < 1e-2f);
    }
    // heavier demand takes longer on the same roads
    const DayResult light = RunDay(500, 5), heavy = RunDay(9000, 5);
    CHECK(light.meanCommuteMinutes > 0 && heavy.meanCommuteMinutes > light.meanCommuteMinutes * 1.1);

    // arrivals spread with the speed spread: one herd dissolves over several ticks with sd > 0, in one tick with sd == 0
    for (float sd : {0.0f, 0.15f}) {
        Scene s;
        s.hs.speedSdMin = s.hs.speedSdMax = sd;
        s.commute = {{0, 1, 20000.0f}};
        s.hs.ReleaseHerds(7 * 60, 7 * 60, s.hoods, s.commute, s.school, s.shop, s.Fn()); // 07:00 window
        CHECK(s.hs.Live() == 2); // the way there and the (later) way back
        int ticks = 0;
        for (int i = 0; i < 400; ++i) {
            const float before = s.hs.arrivedDay.empty() ? 0.0f : s.hs.arrivedDay[1];
            s.hs.AdvanceHerds(1.0f);
            if (s.hs.arrivedDay[1] > before + 1e-4f) ++ticks;
        }
        CHECK(s.hs.Live() == 0);
        if (sd == 0.0f) CHECK(ticks == 1);
        else CHECK(ticks >= 4);
    }

    // school drop-offs chain on to work
    const DayResult sch = RunDay(2000, 5, -1, 400);
    CHECK(sch.stats.released[kTripSchool] > 0 && sch.deliveredChain > 0);
    CHECK(sch.deliveredChain <= sch.stats.delivered[kTripSchool] + 0.01f);

    // deterministic for the same seed; a different seed changes the departures
    const DayResult a = RunDay(3000, 9), b = RunDay(3000, 9), c = RunDay(3000, 10);
    CHECK(a.loadDay == b.loadDay && a.stats.herdsReleased == b.stats.herdsReleased && a.meanCommuteMinutes == b.meanCommuteMinutes);
    CHECK(a.meanCommuteMinutes != c.meanCommuteMinutes);

    if (failures == 0) std::printf("sim_herds_test: all checks passed (%llu herds a day, commute %.1f min)\n", (unsigned long long)a.stats.herdsReleased, a.meanCommuteMinutes);
    return failures ? 1 : 0;
}
