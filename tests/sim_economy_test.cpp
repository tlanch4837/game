// Headless checks for the city sim's economy (src/city/sim_economy.h):
//   a deterministic ledger over a synthetic month; a city with nobody in it earns nothing (and pays its upkeep);
//   tax rates scale income and move demand; no overflow for a huge city over decades; upkeep per day from monthly upkeep.
// Build and run: tests\sim_economy_test.bat
#include <cstdio>
#include <vector>

#include "city/sim.h"
#include "city/sim_economy.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static Lot MakeLot(uint64_t id, float x, int type, int w, int d) {
    Lot l;
    l.id = id;
    l.c = {x, 10};
    l.type = type, l.w = w, l.d = d;
    l.path = "m";
    l.progress = 2.0f;
    return l;
}

static int64_t Month(uint64_t seed, int64_t* income = nullptr) {
    SplitMix64 r(seed);
    std::vector<Lot> lots;
    for (uint64_t i = 0; i < 300; ++i) lots.push_back(MakeLot(i + 1, (float)r.Below(1000), (int)r.Below(7), 1 + (int)r.Below(4), 1 + (int)r.Below(4)));
    CitySim s;
    s.Sync(lots, 1);
    SimTuning t;
    SimEconomy e;
    e.Reset(t);
    int64_t in = 0;
    for (int d = 0; d < 30; ++d) {
        e.Day(s, t, UpkeepPerDay(3000, 1.0, 1.0) * 3);
        in += e.LastIncome();
    }
    if (income) *income = in;
    return e.treasury;
}

int main() {
    // Upkeep: 3000 a month is 10,000 cents a day.
    CHECK(UpkeepPerDay(3000, 1.0, 1.0) == 10000);
    CHECK(UpkeepPerDay(3000, 0.5, 1.0) == 5000);
    CHECK(UpkeepPerDay(3000, 1.0, 0.0) == 0);

    // Deterministic month.
    int64_t income = 0;
    const int64_t a = Month(5, &income), b = Month(5);
    CHECK(a == b);
    CHECK(income > 0);
    std::printf("month: income %lld cents, treasury %lld -> %lld\n", (long long)income, (long long)SimTuning().startingMoney, (long long)a);

    // Nobody lives here: no income, upkeep still paid.
    {
        CitySim s;
        SimTuning t;
        SimEconomy e;
        e.Reset(t);
        for (int d = 0; d < 30; ++d) e.Day(s, t, 10000);
        CHECK(e.LastIncome() == 0);
        CHECK(e.treasury == (int64_t)t.startingMoney - 30 * 10000);
    }

    // Tax rates: 20 % doubles residential income compared with 10 %; demand offset sign.
    {
        std::vector<Lot> lots = {MakeLot(1, 10, 3, 6, 6), MakeLot(2, 20, 4, 4, 4)};
        CitySim s;
        s.Sync(lots, 1);
        SimTuning t;
        SimEconomy e;
        int64_t base[4], high[4];
        e.DayIncome(s, t, base);
        e.taxRate10[0] = 200;
        e.DayIncome(s, t, high);
        CHECK(base[0] > 0 && high[0] == base[0] * 2);
        CHECK(e.DemandOffset(0, t) < 0);
        e.taxRate10[0] = 50;
        CHECK(e.DemandOffset(0, t) > 0);
        e.taxRate10[0] = 100;
        CHECK(e.DemandOffset(0, t) == 0.0);
    }

    // Overflow: a 2 M resident, 1 M job city at 25 % for 50 years stays well inside int64.
    {
        std::vector<Lot> lots;
        uint64_t id = 1;
        for (int i = 0; i < 20000; ++i) lots.push_back(MakeLot(id++, (float)(i % 4000) * 3.0f, 3, 6, 6)); // high density towers
        for (int i = 0; i < 6000; ++i) lots.push_back(MakeLot(id++, (float)(i % 4000) * 3.0f, 5, 6, 6));  // offices
        CitySim s;
        s.Sync(lots, 1);
        SimTuning t;
        SimEconomy e;
        e.Reset(t);
        for (int c = 0; c < 4; ++c) e.taxRate10[c] = 250;
        for (int d = 0; d < 365 * 50; ++d) e.Day(s, t, 0);
        std::printf("huge: %llu residents, %llu working, treasury after 50 years %lld cents\n", (unsigned long long)s.Population(), (unsigned long long)s.Working(),
                    (long long)e.treasury);
        CHECK(e.treasury > 0);
        CHECK(e.LastIncome() > 0);
    }

    if (failures) {
        std::printf("sim_economy_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_economy_test: all passed\n");
    return 0;
}
