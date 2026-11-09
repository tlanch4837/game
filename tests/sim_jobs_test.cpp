// Headless checks for jobs (src/city/sim_jobs.h):
//   local first (own hood fills before a neighbour's), spill to the nearest hood by travel time, never past
//   life.maxCommute; education levels (no job above one's schooling; an overqualified worker is hired and pays in
//   happiness); a long commute makes a worker switch to a nearer job when one opens; retirement frees the job; the
//   invariants working == employed == commute links, filled <= slots, and the views equal a recount.
// Build and run: tests\sim_jobs_test.bat
#include <cstdio>

#include "sim_life_town.h"

using namespace lifetest;

static uint32_t Bld(Town& t, uint64_t originId) { return t.reg.Find(BOrigin::Lot, originId).v; }
static uint32_t Person(Town& t, uint32_t b, int age, int edu) {
    LifeWorld& w = t.sim.w;
    const HouseholdId h = w.NewHousehold(5000, 2);
    const CitizenId c = w.AddMember(h, (int32_t)(w.month - age * 12), edu, false, 1);
    w.Place(h, b);
    return c.Index();
}
static void QueueAll(LifeWorld& w) {
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci)
        if (w.cit.At(ci).Valid() && LifeIsWorkingAge(w, ci) && !w.cit.work[ci]) LifeQueueSeeker(w, ci);
}
static void Invariants(const LifeWorld& w) {
    uint32_t working = 0, employed = 0, links = 0, filled = 0;
    for (const LifeCell& c : w.cells) working += c.working, employed += c.employed[0] + c.employed[1] + c.employed[2];
    for (const auto& kv : w.commute) links += (uint32_t)kv.second;
    for (const LifeBuilding& L : w.bld)
        for (int e = 0; e < kEduLevels; ++e) {
            filled += L.filled[e];
            CHECK(L.filled[e] <= L.slots[e]);
        }
    CHECK(working == employed && employed == links && links == filled);
    std::string why;
    CHECK(w.Verify(&why));
    if (!why.empty()) std::printf("  verify: %s\n", why.c_str());
}
// Workers living in building `home` whose job is in building `job`.
static int WorkersAt(const LifeWorld& w, uint32_t home, uint32_t job) {
    int n = 0;
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci)
        if (w.cit.At(ci).Valid() && w.cit.work[ci] == job && w.hh.home[LifeWorld::Ix(w.cit.household[ci])] == home) ++n;
    return n;
}

static void TestLocalFirst() {
    Town t;
    // hood 0 (cells 0..3), hood 1 next door (cells 4..7), hood 16*? far away: cell (60, 60) is hood 15*16+15
    const uint64_t homeA = t.Add(1, 1, 200, 0), jobA = t.Add(2, 1, 0, 100), jobB = t.Add(5, 1, 0, 100), jobFar = t.Add(60, 60, 0, 500);
    t.Resync([](uint64_t, BuildingRow&) { return true; });
    LifeWorld& w = t.sim.w;
    w.tune.maxCommute = 20; // the far workplace is 25 minutes away
    t.sim.TravelChanged();
    const uint32_t hA = Bld(t, homeA), jA = Bld(t, jobA), jB = Bld(t, jobB), jF = Bld(t, jobFar);
    for (int i = 0; i < 150; ++i) Person(t, hA, 30, 4); // university: any level fits
    QueueAll(w);
    LifeMatchJobs(w);
    const int inA = WorkersAt(w, hA, jA), inB = WorkersAt(w, hA, jB), far = WorkersAt(w, hA, jF);
    std::printf("  local first: own hood %d, neighbour %d, far (%d min) %d\n", inA, inB, w.CommuteBetween(*w.B(hA), *w.B(jF)), far);
    CHECK(inA == 100 && inB == 50 && far == 0);
    Invariants(w);
}

static void TestLevels() {
    Town t;
    const uint64_t home = t.Add(1, 1, 100, 0), job = t.Add(1, 2, 0, 0);
    t.Resync([&](uint64_t id, BuildingRow& r) { // 20 university slots and 20 elementary slots only
        if (id == job) { for (int e = 0; e < kEduLevels; ++e) r.jobSlots[e] = 0; r.jobSlots[4] = 20, r.jobSlots[1] = 20; }
        return true;
    });
    LifeWorld& w = t.sim.w;
    const uint32_t h = Bld(t, home), j = Bld(t, job);
    std::vector<uint32_t> low, high;
    for (int i = 0; i < 30; ++i) low.push_back(Person(t, h, 25, 1));
    for (int i = 0; i < 30; ++i) high.push_back(Person(t, h, 25, 4));
    QueueAll(w);
    LifeMatchJobs(w);
    int lowHired = 0, lowAtTop = 0, highTop = 0, highOver = 0;
    for (uint32_t ci : low) lowHired += w.cit.work[ci] == j, lowAtTop += w.cit.work[ci] && w.cit.jobLevel[ci] > 1;
    for (uint32_t ci : high) highTop += w.cit.work[ci] && w.cit.jobLevel[ci] == 4, highOver += w.cit.work[ci] && w.cit.jobLevel[ci] == 1;
    std::printf("  levels: elementary seekers hired %d (above level 1: %d); university seekers at level 4 %d, overqualified at 1 %d\n", lowHired, lowAtTop,
                highTop, highOver);
    CHECK(lowAtTop == 0 && highTop == 20 && lowHired + highOver == 20);
    for (uint32_t ci : high)
        if (w.cit.work[ci] && w.cit.jobLevel[ci] == 1) {
            LifeTerms terms;
            LifeHappyTerms(w, ci, terms);
            CHECK(terms.v[kWhyOverqualified] < 0);
            break;
        }
    Invariants(w);
}

// A worker with a long commute (over life.quitCommute) switches when a nearer job opens; nobody else is touched.
static void TestCommuteSwitch() {
    Town t;
    const uint64_t home = t.Add(0, 0, 50, 0), farJob = t.Add(63, 63, 0, 40);
    t.Resync([](uint64_t, BuildingRow&) { return true; });
    LifeWorld& w = t.sim.w;
    w.tune.quitCommute = 15, w.tune.quitRate = 1.0f;
    t.sim.TravelChanged();
    const uint32_t h = Bld(t, home), jf = Bld(t, farJob);
    for (int i = 0; i < 30; ++i) Person(t, h, 30, 2);
    QueueAll(w);
    LifeMatchJobs(w);
    CHECK(WorkersAt(w, h, jf) == 30);
    const uint16_t before = w.cit.commuteMin[1];
    const uint64_t nearJob = t.Add(1, 0, 0, 20);
    t.Resync([](uint64_t, BuildingRow&) { return true; });
    const uint32_t jn = Bld(t, nearJob);
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) if (w.cit.At(ci).Valid()) LifeCitizenMonth(w, ci); // their monthly visit queues them
    LifeMatchJobs(w);
    const int moved = WorkersAt(w, h, jn), stayed = WorkersAt(w, h, jf);
    std::printf("  commute: %d of 30 switched from %d min to a near job (%d slots fit their schooling), %d stayed, nobody jobless: %d\n", moved,
                before, (int)w.B(jn)->slots[0] + w.B(jn)->slots[1] + w.B(jn)->slots[2], stayed, moved + stayed == 30);
    CHECK(before > 15 && moved > 0 && moved + stayed == 30);
    Invariants(w);
}

// Turning 65 retires: the job is freed and the next seeker takes it.
static void TestRetirement() {
    Town t;
    const uint64_t home = t.Add(2, 2, 20, 0), job = t.Add(2, 3, 0, 10);
    t.Resync([](uint64_t, BuildingRow&) { return true; });
    LifeWorld& w = t.sim.w;
    const uint32_t h = Bld(t, home), j = Bld(t, job);
    const uint32_t old = Person(t, h, 64, 2);
    w.cit.bornMonth[old] = (int32_t)(w.month - 64 * 12 - 11); // 65 next month
    QueueAll(w);
    LifeMatchJobs(w);
    CHECK(w.cit.work[old] == j);
    w.month += 1;
    LifeCitizenMonth(w, old);
    CHECK(w.cit.Alive(w.cit.At(old)) ? (w.cit.work[old] == 0 && (w.cit.flags[old] & kCitRetired)) : true);
    std::printf("  retirement: worker at 65 retired, job freed (%s)\n", w.cit.work[old] == 0 ? "ok" : "still working");
    Invariants(w);
}

int main() {
    TestLocalFirst();
    TestLevels();
    TestCommuteSwitch();
    TestRetirement();
    if (failures) { std::printf("sim_jobs_test: %d FAILED\n", failures); return 1; }
    std::printf("sim_jobs_test: all passed\n");
    return 0;
}
