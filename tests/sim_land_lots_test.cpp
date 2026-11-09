// Phase 2 lot levelling (src/city/sim_land_lots.h): level up after good days, no flip-flop at the new level
// (hysteresis), no levelling waves (hourly cap), level down, abandonment as a state, recovery, demolition, zero work when
// nothing is due or dirty, the full recompute agrees with the incremental state, determinism. Headless.
// Build and run: tests\sim_land_lots_test.bat
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "city/sim_land_lots.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Town {
    ZoneGrid z;
    LotLevels L;
    std::unordered_map<uint64_t, LotInputs> in;
    double demand[4] = {0.3, 0.3, 0.3, 0.3};
    int64_t minute = 0;
    explicit Town(int n, int type = 0) {
        for (int lv = 1; lv <= 3; ++lv) z.models.push_back({type, 2, 2, lv, "m" + std::to_string(lv)});
        for (int i = 0; i < n; ++i) {
            Lot l;
            l.id = 100 + (uint64_t)i, l.c = {(float)(i % 20) * 40.0f, (float)(i / 20) * 40.0f};
            l.type = type, l.w = 2, l.d = 2, l.path = "m1", l.progress = 2.0f, l.alive = true;
            z.lots.push_back(l);
        }
    }
    void SetAll(uint8_t land, uint8_t bits = kLotInPower | kLotInWater | kLotInSewage) {
        for (const Lot& l : z.lots) in[l.id] = {land, bits}, L.MarkDirty(l.id);
    }
    void Run(int64_t minutes) {
        for (int64_t m = 0; m < minutes; ++m, ++minute) {
            for (Lot& l : z.lots) if (l.alive && l.progress < 1.0f) l.progress = 2.0f; // construction is instant here
            L.Tick(z, minute, demand, [&](const Lot& l, size_t) { return in[l.id]; });
        }
    }
    int Count(LotEventType t) const { int n = 0; for (const LotEvent& e : L.events) n += e.type == t; return n; }
    int Check() const { return L.FullCheck(z, demand, [&](const Lot& l, size_t) { return in.at(l.id); }); }
};

int main() {
    // 1. good land: every lot one level up within 1.5 x upDays, then neutral at L2 (no further change, no flip-flop)
    {
        Town t(40);
        t.SetAll(80); // over L1's up threshold (70), under L2's (100), over L2's down threshold (50)
        t.Run(60);    // one sweep cycle: all tracked
        t.SetAll(80);
        t.Run(5 * 1440);
        int l2 = 0;
        for (const Lot& l : t.z.lots) l2 += l.level == 2;
        std::printf("level up: %d of 40 lots at L2 after 5 days, %d ups, %d downs\n", l2, t.Count(LotEventType::LevelUp), t.Count(LotEventType::LevelDown));
        CHECK(l2 == 40 && t.Count(LotEventType::LevelDown) == 0);
        t.Run(10 * 1440);
        CHECK(t.Count(LotEventType::LevelUp) == 40 && t.Count(LotEventType::LevelDown) == 0);
        CHECK(t.Check() == 0);
        // 2. nothing due, nothing dirty: no evaluation work
        t.L.queue.empty() ? (void)0 : (void)0;
        int work = 0;
        for (int m = 0; m < 600; ++m) t.Run(1), work += t.L.evaluated;
        std::printf("idle: %d evaluations in 600 minutes, queue %zu\n", work, t.L.queue.size());
        CHECK(work == 0);
    }
    // 3. no waves: 400 lots good at once level up at most `cap` an hour
    {
        Town t(400);
        t.Run(60);
        t.SetAll(80);
        int worst = 0, last = 0;
        for (int h = 0; h < 24 * 8; ++h) {
            t.Run(60);
            const int n = t.Count(LotEventType::LevelUp);
            worst = std::max(worst, n - last), last = n;
        }
        const int cap = std::max(t.L.p.maxActionsPerHour, 400 / t.L.p.actionsPerLots);
        std::printf("waves: %d ups in 8 days, at most %d in an hour (cap %d)\n", last, worst, cap);
        CHECK(worst <= cap && last > 0);
    }
    // 4. bad at L2: one level down; bad at L1: abandoned (a state), then recovery when things improve
    {
        Town t(10);
        for (Lot& l : t.z.lots) l.level = 2;
        t.Run(60);
        t.SetAll(30); // under L2's down threshold (50), over L1's (12)
        t.Run(9 * 1440);
        int l1 = 0;
        for (const Lot& l : t.z.lots) l1 += l.level == 1;
        CHECK(l1 == 10 && t.Count(LotEventType::Abandoned) == 0); // at L1 the same land is neutral: it stays
        t.SetAll(30, kLotInWater | kLotInSewage); // no power: bad
        t.Run(9 * 1440);
        const int ab = t.L.AbandonedNow();
        std::printf("abandon: %d of 10 abandoned, %d demolished\n", ab, t.Count(LotEventType::Demolished));
        CHECK(ab + t.Count(LotEventType::Demolished) == 10 && ab > 0);
        int alive = 0;
        for (const Lot& l : t.z.lots) alive += l.alive;
        t.SetAll(80); // power back
        t.Run(2 * 1440);
        std::printf("recover: %d recovered, %d still abandoned, %d alive\n", t.Count(LotEventType::Recovered), t.L.AbandonedNow(), alive);
        CHECK(t.L.AbandonedNow() == 0 && t.Count(LotEventType::Recovered) == alive);
        CHECK(t.Check() == 0);
    }
    // 5. abandoned and still bad: demolished after abandonDays; a bankrupt company counts as bad
    {
        Town t(10, 6);
        t.Run(60);
        t.SetAll(80, kLotInPower | kLotInWater | kLotInSewage | kLotInClosed);
        t.Run(14 * 1440);
        int alive = 0;
        for (const Lot& l : t.z.lots) alive += l.alive;
        std::printf("demolish: %d abandoned, %d demolished, %d alive, %d tracked\n", t.Count(LotEventType::Abandoned), t.Count(LotEventType::Demolished), alive, t.L.Tracked());
        CHECK(t.Count(LotEventType::Abandoned) == 10 && t.Count(LotEventType::Demolished) == 10 && alive == 0 && t.L.Tracked() == 0);
    }
    // 6. determinism: the same town twice gives the same events in the same order
    {
        auto run = [] {
            Town t(120);
            t.Run(60);
            for (size_t i = 0; i < t.z.lots.size(); ++i) t.in[t.z.lots[i].id] = {(uint8_t)(i % 3 ? 85 : 5), (uint8_t)(kLotInPower | kLotInWater | kLotInSewage)}, t.L.MarkDirty(t.z.lots[i].id);
            t.Run(12 * 1440);
            uint64_t h = 1469598103934665603ull;
            for (const LotEvent& e : t.L.events) h = (h ^ (e.lot * 8 + (uint64_t)e.type)) * 1099511628211ull;
            return h;
        };
        const uint64_t a = run(), b = run();
        CHECK(a == b);
    }
    std::printf(g_fail ? "sim_land_lots_test: %d FAILED\n" : "sim_land_lots_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
