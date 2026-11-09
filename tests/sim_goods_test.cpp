// Phase 6 goods tests (headless): mass conservation across a chain, shortage flags, import / export balance,
// deterministic supplier picks and runs, the COMP / GOOD / TRAD save round trip, and the cost per tick (bench).
// Build and run: tests\sim_goods_test.bat
#include <chrono>
#include <cstring>

#include "sim_goods_town.h"

static void TestMassChain() {
    for (int outside = 0; outside < 2; ++outside) {
        GoodsTown t;
        GoodsWorld w;
        BuildChain(t, w, outside != 0);
        int64_t m = 0;
        w.SetSites(t.sites, m);
        bool ok = true;
        for (int h = 0; h < 24 * 6; ++h) {
            RunMinutes(w, m, 60);
            if (!MassOk(w, ok)) ok = false;
        }
        CHECK(ok);
        // a site taken away mid-run: its stock is written off, the ledger still balances
        t.sites.erase(t.sites.begin() + 6);
        w.SetSites(t.sites, m);
        RunMinutes(w, m, 1440);
        CHECK(MassOk(w, true));
        const GoodsLedger l = w.Running();
        CHECK(l.produced[(size_t)Good::Grain] > 0);
        CHECK(l.produced[(size_t)Good::Food] > 0);
        CHECK(l.produced[(size_t)Good::Goods] > 0);   // wood + petro + metals made it to the manufacturer
        CHECK(l.consumed[(size_t)Good::Goods] > 0);   // and on to the residents through the shops
        CHECK(l.consumed[(size_t)Good::Food] > 0);
        CHECK(l.used[(size_t)Good::Timber] > 0);
        CHECK(l.trucks > 0 && !w.TrucksLast().empty());
        std::printf("chain (outside %d): grain %lld made, goods %lld made %lld sold, food %lld sold, imports %lld exports %lld, trucks %lld\n",
                    outside, (long long)l.produced[0], (long long)l.produced[(size_t)Good::Goods], (long long)l.consumed[(size_t)Good::Goods],
                    (long long)l.consumed[(size_t)Good::Food], (long long)(l.imported[0] + l.imported[(size_t)Good::Goods]),
                    (long long)(l.exported[0] + l.exported[1] + l.exported[2] + l.exported[3] + l.exported[4]), (long long)l.trucks);
    }
}

static void TestShortage() {
    GoodsTown t;
    GoodsWorld w;
    t.Shop(0, 0, 10);
    t.Plant(kRecipeGoods, 500, 0, 10);
    w.residents = 1000;
    int64_t m = 0;
    w.SetSites(t.sites, m);
    RunMinutes(w, m, 1440);
    const CompanyRec* shop = w.cos.Get(w.cos.Find(t.sites[0].building));
    const CompanyRec* plant = w.cos.Get(w.cos.Find(t.sites[1].building));
    CHECK(shop && (shop->flags & kCoNoGoods));
    CHECK(plant && (plant->flags & kCoNoGoods));
    int shortEv = 0;
    for (const SimEvent& e : w.events) shortEv += e.type == SimEventType::GoodsShortage;
    CHECK(shortEv == 2);
    CHECK(w.shortages == 2);
    // an outside connection: the shortage is imported away
    w.outsides.push_back({3000, 0, 77});
    RunMinutes(w, m, 1440);
    shop = w.cos.Get(w.cos.Find(t.sites[0].building));
    plant = w.cos.Get(w.cos.Find(t.sites[1].building));
    CHECK(!(shop->flags & kCoNoGoods));
    CHECK(!(plant->flags & kCoNoGoods));
    CHECK(w.last.imported[(size_t)Good::Wood] > 0 && w.last.imported[(size_t)Good::Food] > 0);
    // no customers: a big shop in a town of ten people
    GoodsTown t2;
    GoodsWorld w2;
    t2.Shop(0, 0, 40, 0);
    w2.residents = 10;
    int64_t m2 = 0;
    w2.SetSites(t2.sites, m2);
    RunMinutes(w2, m2, 120);
    CHECK(w2.cos.recs[1].flags & kCoNoCustomers);
    w2.residents = 20000;
    RunMinutes(w2, m2, 120);
    CHECK(!(w2.cos.recs[1].flags & kCoNoCustomers));
    // cannot ship: a hub with no buyer and no outside connection fills up
    GoodsTown t3;
    GoodsWorld w3;
    t3.Hub(Good::Ore, 0, 0, 40);
    int64_t m3 = 0;
    w3.SetSites(t3.sites, m3);
    RunMinutes(w3, m3, 3 * 1440);
    CHECK(w3.cos.recs[1].flags & kCoNoFreight);
    CHECK(w3.noFreight == 1);
}

static void TestTradeBalance() {
    // surplus hubs with nobody to sell to in town: everything is exported
    GoodsTown t;
    GoodsWorld w;
    t.Hub(Good::Oil, 0, 0, 60), t.Hub(Good::Timber, 400, 0, 60);
    w.outsides.push_back({-3000, 0, 50});
    int64_t m = 0;
    w.SetSites(t.sites, m);
    RunMinutes(w, m, 4 * 1440);
    CHECK(MassOk(w, true));
    const GoodsLedger l = w.Running();
    CHECK(l.exported[(size_t)Good::Oil] > 0 && l.exported[(size_t)Good::Timber] > 0);
    CHECK(l.importCost == 0 && l.exportIncome > 0);
    CHECK(w.cos.recs[1].flags == 0); // it can ship: not "cannot ship"
    CHECK(w.balance.size() == 4);
    int64_t bal = 0;
    for (int32_t b : w.balance) bal += b;
    CHECK(bal == w.total.exportIncome - w.total.importCost);
    // a plant with no local supplier imports all its input, at 1.25 x the price
    GoodsTown t2;
    GoodsWorld w2;
    t2.Plant(kRecipeMetals, 0, 0, 20);
    w2.outsides.push_back({2500, 0, 51});
    int64_t m2 = 0;
    w2.SetSites(t2.sites, m2);
    RunMinutes(w2, m2, 2 * 1440);
    CHECK(MassOk(w2, true));
    const GoodsLedger l2 = w2.Running();
    CHECK(l2.imported[(size_t)Good::Ore] > 0);
    CHECK(l2.importCost >= l2.imported[(size_t)Good::Ore] * GoodPrice(Good::Ore) && l2.importCost <= l2.imported[(size_t)Good::Ore] * GoodPrice(Good::Ore) * 125 / 100);
    CHECK(l2.exported[(size_t)Good::Metals] > 0); // no shop or manufacturer wants metals: they leave
    // local first: with a hub in town the same plant imports nothing (once the hub has stock)
    GoodsTown t3;
    GoodsWorld w3;
    t3.Hub(Good::Ore, 0, 300, 120);
    t3.Plant(kRecipeMetals, 0, 0, 20);
    w3.outsides.push_back({2500, 0, 51});
    int64_t m3 = 0;
    w3.SetSites(t3.sites, m3);
    RunMinutes(w3, m3, 3 * 1440);
    CHECK(w3.last.imported[(size_t)Good::Ore] == 0);
    CHECK(w3.last.used[(size_t)Good::Ore] > 0);
    std::printf("trade: hubs export %lld units for %lld; a plant imports %lld ore for %lld\n", (long long)(l.exported[3] + l.exported[2]),
                (long long)l.exportIncome, (long long)l2.imported[(size_t)Good::Ore], (long long)l2.importCost);
}

static void TestDeterminism() {
    // two equidistant sellers: the lower company index wins, every time
    GoodsTown t;
    GoodsWorld w;
    t.Hub(Good::Ore, -500, 0, 100), t.Hub(Good::Ore, 500, 0, 100);
    t.Plant(kRecipeMetals, 0, 0, 10);
    int64_t m = 0;
    w.SetSites(t.sites, m);
    RunMinutes(w, m, 600);
    bool firstOnly = !w.ships.empty() || w.Running().used[(size_t)Good::Ore] > 0;
    for (const Shipment& s : w.ships)
        if (s.good == (int)Good::Ore && s.to == (int32_t)w.cos.Find(t.sites[2].building).v) firstOnly &= s.from == (int32_t)w.cos.Find(t.sites[0].building).v;
    CHECK(firstOnly);
    // the same town twice: identical bytes after five days
    std::vector<uint8_t> a, b;
    for (int run = 0; run < 2; ++run) {
        GoodsTown tt;
        GoodsWorld ww;
        BuildChain(tt, ww, true, 10);
        int64_t mm = 0;
        ww.SetSites(tt.sites, mm);
        RunMinutes(ww, mm, 5 * 1440);
        (run ? b : a) = SaveWorld(ww);
    }
    CHECK(!a.empty() && a == b);
}

static void TestSaveRoundTrip() {
    GoodsTown t;
    GoodsWorld w;
    BuildChain(t, w, true, 8);
    int64_t m = 0;
    w.SetSites(t.sites, m);
    RunMinutes(w, m, 2 * 1440 + 437);
    const std::vector<uint8_t> file = SaveWorld(w);
    GoodsWorld v;
    const SimLoadReport rep = LoadWorld(v, file);
    CHECK(rep.Loaded() == 3);
    v.outsides = w.outsides, v.residents = w.residents; // the host refreshes these every pass
    v.SetSites(t.sites, m);
    w.SetSites(t.sites, m);
    CHECK(SaveWorld(v) == file);
    CHECK(MassOk(v, true));
    int64_t m2 = m;
    RunMinutes(w, m, 1440), RunMinutes(v, m2, 1440);
    CHECK(SaveWorld(v) == SaveWorld(w));
    // a corrupt GOOD chunk starts that system fresh
    std::vector<uint8_t> bad = file;
    bad[bad.size() / 2] ^= 0x5A;
    GoodsWorld z;
    const SimLoadReport rz = LoadWorld(z, bad);
    CHECK(rz.Loaded() < 3);
}

// A 50k-resident city's companies (roadmap budget: 0.15 ms per tick on average, 0.8 ms worst, on the iPad = desktop x 2).
static void BenchTown(GoodsTown& t, GoodsWorld& w, int residents) {
    const int k = residents / 1000; // per 1000 residents: 18 industrial, 20 shops, 12 offices
    const Good raws[5] = {Good::Grain, Good::Livestock, Good::Timber, Good::Oil, Good::Ore};
    for (int i = 0; i < 4 * 5; ++i) t.Hub(raws[i % 5], 8000 + (i % 5) * 300, (i / 5) * 400, 80);
    for (int i = 0; i < 18 * k; ++i) t.Plant(RecipeForLot((uint64_t)i + 1), (i % 40) * 150, 2000 + (i / 40) * 150, 16);
    for (int i = 0; i < 20 * k; ++i) t.Shop((i % 60) * 100, 6000 + (i / 60) * 100, 8);
    for (int i = 0; i < 12 * k; ++i) t.Add(CompanyKind::Office, (i % 30) * 120, 9000 + (i / 30) * 120, 25, 22);
    w.residents = residents;
    for (int i = 0; i < 4; ++i) w.outsides.push_back({i % 2 ? 12000 : -4000, i < 2 ? 0 : 12000, 100000 + i});
}

// The runs are deterministic, so the same tick repeats exactly: the minimum over three runs is its cost without the
// OS's interruptions (other processes, page faults).
static void Bench(int residents) {
    using clk = std::chrono::steady_clock;
    const int days = 3, n = days * 1440;
    std::vector<double> best((size_t)n, 1e30);
    double hostMs = 0;
    GoodsTown t;
    GoodsWorld w;
    for (int run = 0; run < 3; ++run) {
        t = GoodsTown(), w = GoodsWorld();
        BenchTown(t, w, residents);
        int64_t m = 0;
        w.SetSites(t.sites, m);
        RunMinutes(w, m, 1440); // warm up: stocks and trucks settle
        w.TrucksLast();
        for (int i = 0; i < n; ++i) {
            const auto a = clk::now();
            w.Tick(m++);
            best[(size_t)i] = std::min(best[(size_t)i], std::chrono::duration<double, std::milli>(clk::now() - a).count());
            if (m % 1440 == 0) w.TrucksLast(); // the host's frame after the day end: the truck links for the herds
            if (i % 60 == 0) { // the host's hourly refresh (workers, residents)
                const auto h = clk::now();
                w.SetSites(t.sites, m);
                if (run == 0) hostMs += std::chrono::duration<double, std::milli>(clk::now() - h).count();
            }
        }
    }
    double sum = 0, worst = 0, worstBy[3] = {0, 0, 0};
    std::vector<double> work;
    for (int i = 0; i < n; ++i) {
        const double ms = best[(size_t)i];
        const int64_t mm = 1440 + i;
        sum += ms, worst = std::max(worst, ms);
        if (mm % 5 && mm % 1440 != 1439) continue;
        work.push_back(ms);
        const int cls = mm % 1440 == 1439 ? 0 : mm % 60 == 0 ? 1 : 2;
        worstBy[cls] = std::max(worstBy[cls], ms);
    }
    const double avg = sum / n;
    const GoodsLedger rl = w.Running();
    int64_t imp = 0, exp = 0, made = 0, sold = 0;
    for (int g = 0; g < kGoodCount; ++g) imp += rl.imported[(size_t)g], exp += rl.exported[(size_t)g], made += rl.produced[(size_t)g], sold += rl.consumed[(size_t)g];
    std::printf("  ledger: made %lld sold %lld imported %lld exported %lld trucks %lld; shortages %d no customers %d cannot ship %d bankrupt %d\n",
                (long long)made, (long long)sold, (long long)imp, (long long)exp, (long long)rl.trucks, w.shortages, w.noCustomers, w.noFreight, w.bankruptTotal);
    std::printf("bench %d residents: %zu companies, %zu trucks in flight; avg %.4f ms/tick, worst %.3f ms (iPad x2: %.4f / %.3f; budget 0.15 / 0.8)\n",
                residents, t.sites.size(), w.ships.size(), avg, worst, avg * 2, worst * 2);
    std::sort(work.begin(), work.end());
    std::printf("  working ticks: median %.3f ms, p99 %.3f ms; worst day end %.3f, hour (demand) %.3f, slice %.3f; host SetSites %.3f ms/hour\n",
                work[work.size() / 2], work[work.size() * 99 / 100], worstBy[0], worstBy[1], worstBy[2], hostMs / (n / 60));
    CHECK(avg * 2 < 0.15);
    if (residents <= 50000) CHECK(worst * 2 < 0.8); // the roadmap's budget table is for 50k; 100k is reported
    CHECK(MassOk(w, true));
}

int main(int argc, char** argv) {
    TestMassChain();
    TestShortage();
    TestTradeBalance();
    TestDeterminism();
    TestSaveRoundTrip();
    Bench(argc > 1 ? std::atoi(argv[1]) : 50000);
    if (argc <= 1) Bench(100000);
    std::printf(g_fails ? "sim_goods_test: %d FAILED\n" : "sim_goods_test: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
