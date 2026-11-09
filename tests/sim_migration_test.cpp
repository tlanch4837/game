// Headless checks for migration and household money (src/city/sim_migration.h, sim_household_econ.h):
//   a more attractive city (services, parks against pollution and crime) draws more immigrants; a jobs crash brings
//   emigration after a lag (savings run out first), and net migration does not oscillate within 6 months of it;
//   instantFill fills every free home at once; past the record cap homes fill with statistical residents; working
//   households save, jobless ones run down their savings.
// Build and run: tests\sim_migration_test.bat
#include <cstdio>

#include "sim_life_town.h"

using namespace lifetest;

static int32_t Immigrants(Town& t, CellFactors& cf, int months) {
    t.sim.w.citySeed = 4;
    t.Build(12, 40, 40);
    cf.Fit(64 * 64);
    t.sim.w.factors = &cf;
    int64_t minute = 0;
    t.RunDays(minute, months);
    int32_t n = 0; // months 2.. : the first month's quota is set before anyone lives there to judge the place
    for (size_t k = 1; k < t.sim.w.history.size(); ++k) n += t.sim.w.history[k].immigrants;
    return n + t.sim.w.cur.immigrants;
}
static void TestAttractiveness() {
    Town good, bad;
    CellFactors fg, fb;
    fg.Fit(64 * 64), fb.Fit(64 * 64);
    for (CellFactor& c : fg.cells) for (int s = 0; s < 6; ++s) c.coverage[s] = 220, c.parkAccess = 220;
    for (CellFactor& c : fb.cells) c.air = 180, c.crime = 200, c.noise = 150;
    const int32_t g = Immigrants(good, fg, 4), b = Immigrants(bad, fb, 4);
    std::printf("  attractiveness: months 2-4 of an empty town, good city %d immigrants (attract %.2f), bad city %d (attract %.2f)\n", g,
                good.sim.mig.attract, b, bad.sim.mig.attract);
    CHECK(g > b * 3 / 2 && b > 0);
    std::string why;
    CHECK(good.sim.w.Verify(&why) && bad.sim.w.Verify(&why));
}

static void TestJobsCrash() {
    Town t;
    t.sim.w.citySeed = 8;
    t.sim.w.tune.instantFill = 1;
    t.Build(6, 40, 40);
    int64_t minute = 0;
    t.RunDays(minute, 1);
    t.sim.w.tune.instantFill = 0;
    t.RunDays(minute, 6);
    LifeWorld& w = t.sim.w;
    const uint32_t before = w.Population();
    t.Resync([](uint64_t, BuildingRow& r) { uint32_t j = 0; for (int e = 0; e < kEduLevels; ++e) j += r.jobSlots[e]; return j == 0; });
    const size_t crash = w.history.size();
    t.RunDays(minute, 12);
    int signChanges = 0, prevSign = 0, firstMonth = 0, later = 0;
    for (size_t k = crash; k < w.history.size(); ++k) {
        const LifeMonthStats& m = w.history[k];
        const int net = m.immigrants - m.emigrants, sign = net > 0 ? 1 : net < 0 ? -1 : 0;
        if (k < crash + 6 && sign && prevSign && sign != prevSign) ++signChanges;
        if (sign) prevSign = sign;
        if (k == crash) firstMonth = m.emigrants;
        else if (k < crash + 6) later = std::max(later, m.emigrants);
        std::printf("    month %zu after the crash: immigrants %d emigrants %d (workers %d)\n", k - crash + 1, m.immigrants, m.emigrants, m.workers);
    }
    std::printf("  jobs crash: %u residents before, %u a year after; emigrants month 1 %d, peak months 2-6 %d, sign changes %d\n", before,
                w.Population(), firstMonth, later, signChanges);
    CHECK(w.Working() == 0);
    CHECK(later > firstMonth * 2 && later > 0);      // a lag: savings first
    CHECK(w.Population() < before * 9 / 10);          // they do leave
    CHECK(signChanges <= 1);
    std::string why;
    CHECK(w.Verify(&why));
}

static void TestInstantFillAndCap() {
    Town t;
    t.sim.w.citySeed = 2;
    t.sim.w.tune.instantFill = 1;
    t.sim.w.tune.recordCap = 1000;
    t.Build(6, 40, 20);
    t.sim.Tick(0);
    LifeWorld& w = t.sim.w;
    uint32_t free = 0, homes = 0;
    for (const LifeBuilding& L : w.bld) free += L.id ? L.FreeHomes() : 0, homes += L.id ? L.homes : 0;
    std::printf("  instant fill: %u of %u homes taken, %u records (cap 1000), %d statistical\n", homes - free, homes, w.cit.Live(), w.statResidents);
    CHECK(free == 0);
    CHECK(w.cit.Live() <= 1000 && w.statResidents > 0 && w.Population() == homes);
    std::string why;
    CHECK(w.Verify(&why));
}

static void TestMoney() {
    Town t;
    const uint64_t home = t.Add(1, 1, 10, 0), job = t.Add(1, 2, 0, 10);
    t.Resync([](uint64_t, BuildingRow&) { return true; });
    LifeWorld& w = t.sim.w;
    const uint32_t b = t.reg.Find(BOrigin::Lot, home).v;
    const HouseholdId a = w.NewHousehold(2000, 1), z = w.NewHousehold(2000, 1);
    const CitizenId wa = w.AddMember(a, -30 * 12, 2, false, 1);
    w.AddMember(z, -30 * 12, 2, true, 2);
    w.Place(a, b), w.Place(z, b);
    w.SetWork(wa.Index(), t.reg.Find(BOrigin::Lot, job).v, 1);
    for (int m = 0; m < 6; ++m) LifeHouseholdMonth(w, a.Index()), LifeHouseholdMonth(w, z.Index());
    std::printf("  money: worker household savings %d (income %d rent %d), jobless %d (poor months %d)\n", w.hh.savings[a.Index()],
                w.hh.incomeMonth[a.Index()], w.hh.rentMonth[a.Index()], w.hh.savings[z.Index()], w.hh.poorMonths[z.Index()]);
    CHECK(w.hh.savings[a.Index()] > 2000 && w.hh.savings[z.Index()] < 0 && w.hh.poorMonths[z.Index()] > 0);
    CHECK(w.hh.wealth[z.Index()] == 0);
    std::string why;
    CHECK(w.Verify(&why));
}

int main() {
    TestAttractiveness();
    TestJobsCrash();
    TestInstantFillAndCap();
    TestMoney();
    if (failures) { std::printf("sim_migration_test: %d FAILED\n", failures); return 1; }
    std::printf("sim_migration_test: all passed\n");
    return 0;
}
