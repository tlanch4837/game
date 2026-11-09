// Phase 6 company tests (headless): profit falls with no workers, bankruptcy then recovery, the site sync (open, close,
// stale handles), the per-lot recipe pick. Build and run: tests\sim_goods_companies_test.bat
#include "sim_goods_town.h"

static void TestProfitWorkers() {
    // the same plant, staffed and empty, both with inputs on hand
    int32_t profit[2] = {0, 0};
    for (int staffed = 0; staffed < 2; ++staffed) {
        GoodsTown t;
        GoodsWorld w;
        t.Hub(Good::Timber, 0, 300, 200);
        t.Plant(kRecipeWood, 0, 0, staffed ? 20 : 0);
        t.Shop(0, -300, 4); // nobody buys wood in town: the outside does
        w.outsides.push_back({-2000, 0, 9});
        w.residents = 500;
        int64_t m = 0;
        w.SetSites(t.sites, m);
        RunMinutes(w, m, 3 * 1440);
        profit[staffed] = w.cos.Get(w.cos.Find(t.sites[1].building))->profitLast;
        CHECK(MassOk(w, true));
    }
    std::printf("plant profit a day: %d with workers, %d without\n", profit[1], profit[0]);
    CHECK(profit[1] > 0);
    CHECK(profit[0] < profit[1]);
    CHECK(profit[0] < 0); // the upkeep is still owed
}

static void TestBankruptRecover() {
    GoodsTown t;
    GoodsWorld w;
    w.p.startCash = 50;
    t.Plant(kRecipeFood, 0, 0, 0); // no workers, no inputs: only the upkeep
    int64_t m = 0;
    w.SetSites(t.sites, m);
    const uint32_t b = t.sites[0].building;
    int bankrupt = 0, opened = 0, day = 0;
    for (; day < 20 && !bankrupt; ++day) {
        RunMinutes(w, m, 1440);
        for (const SimEvent& e : w.events) bankrupt += e.type == SimEventType::CompanyBankrupt && e.building == b;
    }
    CHECK(bankrupt == 1);
    CHECK(w.bankruptTotal == 1);
    const CompanyRec* c = w.cos.Get(w.cos.Find(b));
    CHECK(c && c->closedDays > 0);
    std::printf("bankrupt after %d days\n", day);
    // workers and a supplier arrive; after the lot reopens the new company makes a profit
    w.events.clear();
    t.sites[0].workers = t.sites[0].jobs = 20;
    t.Hub(Good::Grain, 0, 200, 200), t.Hub(Good::Livestock, 200, 0, 200);
    w.outsides.push_back({-2000, 0, 9});
    w.SetSites(t.sites, m);
    for (int d = 0; d < w.p.reopenDays + 3; ++d) RunMinutes(w, m, 1440);
    for (const SimEvent& e : w.events) opened += e.type == SimEventType::CompanyOpened && e.building == b;
    c = w.cos.Get(w.cos.Find(b));
    CHECK(opened == 1);
    CHECK(c && c->closedDays == 0 && c->profitLast > 0 && c->lossDays == 0);
    CHECK(w.bankruptTotal == 1);
    CHECK(MassOk(w, true));
}

static void TestSync() {
    GoodsTown t;
    GoodsWorld w;
    t.Shop(0, 0, 5), t.Shop(100, 0, 5);
    int64_t m = 0;
    w.SetSites(t.sites, m);
    CHECK(w.cos.pool.Live() == 2);
    const CompanyId first = w.cos.Find(t.sites[0].building);
    int opened = 0;
    for (const SimEvent& e : w.events) opened += e.type == SimEventType::CompanyOpened;
    CHECK(opened == 2);
    w.cos.Get(first)->stock[(size_t)Good::Food] = 33;
    t.sites.erase(t.sites.begin());
    w.SetSites(t.sites, m);
    CHECK(w.cos.pool.Live() == 1);
    CHECK(!w.cos.Alive(first));         // a stale handle never matches again
    CHECK(w.today.lost[(size_t)Good::Food] == 33);
    t.Shop(200, 0, 5);
    w.SetSites(t.sites, m);
    CHECK(w.cos.pool.Live() == 2 && !w.cos.Alive(first)); // the slot is reused with a new generation
    // recipes: stable per lot id, every recipe appears
    int seen[kRecipeCount] = {};
    for (uint64_t id = 1; id <= 700; ++id) ++seen[RecipeForLot(id)];
    for (int r = 0; r < kRecipeCount; ++r) CHECK(seen[r] > 50);
    CHECK(RecipeForLot(12345) == RecipeForLot(12345));
}

int main() {
    TestProfitWorkers();
    TestBankruptRecover();
    TestSync();
    std::printf(g_fails ? "sim_goods_companies_test: %d FAILED\n" : "sim_goods_companies_test: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
