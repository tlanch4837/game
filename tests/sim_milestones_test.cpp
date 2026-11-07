// Headless checks for the city's milestones (src/city/sim_milestones.h):
//   the table is in rising population order and starts with the founding tools; reaching a threshold unlocks its
//   categories once (a falling population never locks them again); a big jump reaches several at once, in order, and
//   pays every reward; UnlockPopulation says what a locked tool needs; unlockAll opens everything.
// Build and run: tests\sim_milestones_test.bat
#include <cstdio>
#include <string_view>

#include "city/sim_milestones.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    int n = 0;
    const Milestone* m = Milestones(n);
    CHECK(n >= 6);
    CHECK(m[0].population == 0);
    for (int i = 1; i < n; ++i) CHECK(m[i].population > m[i - 1].population);
    uint32_t seen = 0;
    for (int i = 0; i < n; ++i) {
        CHECK((seen & m[i].unlocks) == 0); // every category is unlocked by exactly one milestone
        seen |= m[i].unlocks;
    }

    SimMilestones s;
    s.Reset();
    CHECK(s.Level() == 1);
    CHECK(s.Unlocked(kUnlockRoads | kUnlockZoning | kUnlockElectricity | kUnlockWater));
    CHECK(!s.Unlocked(kUnlockRail) && !s.Unlocked(kUnlockAirport));

    int64_t money = 0;
    CHECK(s.Update(100, &money) == 0 && money == 0);
    CHECK(s.Update(UnlockPopulation(kUnlockFire), &money) == 1);
    CHECK(s.Unlocked(kUnlockFire) && money == m[1].reward);
    CHECK(s.Update(UnlockPopulation(kUnlockFire), &money) == 0); // once only
    CHECK(s.Update(10, &money) == 0 && s.Unlocked(kUnlockFire));  // the population fell: still unlocked

    // a jump past several thresholds: each reached once, in order, every reward paid
    const int before = s.reached;
    const int64_t m0 = money;
    const int got = s.Update(UnlockPopulation(kUnlockRail), &money);
    CHECK(got >= 2 && s.reached == before + got);
    int64_t want = 0;
    for (int i = before; i < s.reached; ++i) want += m[i].reward;
    CHECK(money - m0 == want);
    CHECK(s.Unlocked(kUnlockRail) && s.Unlocked(kUnlockTransport) && !s.Unlocked(kUnlockAirport));
    CHECK(s.Next() && s.Next()->population > UnlockPopulation(kUnlockRail));

    CHECK(UnlockPopulation(kUnlockRoads) == 0);
    CHECK(UnlockPopulation(kUnlockAirport) == 20000);
    CHECK(UnlockPopulation(1u << 30) == UINT32_MAX);

    s.Update(1000000, &money);
    CHECK(s.reached == n && s.Next() == nullptr);
    CHECK(std::string_view(s.Current().name) == m[n - 1].name);

    SimMilestones dev;
    dev.Reset();
    dev.unlockAll = true;
    CHECK(dev.Unlocked(kUnlockAirport | kUnlockRail));

    if (failures) std::printf("sim_milestones_test: %d failure(s)\n", failures);
    else std::printf("sim_milestones_test: all passed\n");
    return failures ? 1 : 0;
}
