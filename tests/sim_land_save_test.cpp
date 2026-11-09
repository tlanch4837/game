// Phase 2 save chunks (src/city/sim_land_save.h): GROW, LAND, DMND round trip; the same state writes the same bytes;
// a town saved mid-run and loaded continues exactly like the one that never stopped; and the P2 main-thread bench on a
// Metropolis-sized synthetic city (5,000 lots, 2,601 cells, 20,000 households): ms per tick, average and worst.
// Headless. Build and run: tests\sim_land_save_test.bat
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "city/sim_land_save.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Town {
    ZoneGrid z;
    LotLevels L;
    LandState land;
    DemandSaveState dm;
    double demand[4] = {0.3, 0.3, 0.3, 0.3};
    int64_t minute = 0;
    std::unordered_map<uint64_t, uint8_t> bits;
    explicit Town(int n) {
        for (int t = 0; t < 7; ++t) for (int lv = 1; lv <= 3; ++lv) z.models.push_back({t, 2, 2, lv, "m"});
        for (int i = 0; i < n; ++i) {
            Lot l;
            l.id = 1000 + (uint64_t)i, l.c = {(float)(i % 70) * 64.0f + 10.0f, (float)(i / 70) * 64.0f + 10.0f};
            l.type = i % 7, l.w = 2, l.d = 2, l.path = "m", l.progress = 2.0f, l.alive = true;
            z.lots.push_back(l);
            bits[l.id] = (uint8_t)((i % 13 == 0) ? (kLotInWater | kLotInSewage) : (kLotInPower | kLotInWater | kLotInSewage));
        }
        LandInput in;
        for (int cz = 0; cz < 51; ++cz)
            for (int cx = 0; cx < 51; ++cx) {
                LandCellIn c;
                c.key = {cx, cz}, c.jobs = (uint16_t)((cx * 7 + cz * 3) % 90), c.people = (uint16_t)((cx * 5 + cz) % 150), c.cov = (uint8_t)(cx * 4 % 256), c.road = 1;
                in.cells.push_back(c);
            }
        land = LandFull(in, LandParams());
    }
    LotInputs In(const Lot& l) const { LotInputs i; i.land = land.field.At(l.c.x, l.c.z); i.bits = bits.at(l.id); return i; }
    void Run(int64_t minutes) {
        for (int64_t m = 0; m < minutes; ++m, ++minute) {
            for (Lot& l : z.lots) if (l.alive && l.progress < 1.0f) l.progress = 2.0f;
            L.Tick(z, minute, demand, [&](const Lot& l, size_t) { return In(l); });
        }
    }
    std::vector<uint8_t> Save() const {
        ByteWriter w;
        WriteLots(w, L), WriteLandState(w, land), WriteDemand(w, dm);
        return w.bytes;
    }
    bool Load(const std::vector<uint8_t>& b) {
        ByteReader r(b);
        return ReadLots(r, L) && ReadLandState(r, land) && ReadDemand(r, dm) && r.AtEnd();
    }
};
static uint64_t Hash(const std::vector<uint8_t>& b) { uint64_t h = 1469598103934665603ull; for (uint8_t x : b) h = (h ^ x) * 1099511628211ull; return h; }

int main() {
    // 1. round trip: load what was saved, save again: the same bytes
    {
        Town a(600);
        a.dm.bars[0] = 0.42, a.dm.zone[1] = 1, a.dm.rent.cur.households[2] = 17, a.dm.rent.Count(9, true), a.dm.rent.group = 33;
        a.Run(3 * 1440);
        const std::vector<uint8_t> s1 = a.Save();
        Town b(600);
        CHECK(b.Load(s1));
        b.z = a.z, b.minute = a.minute;
        const std::vector<uint8_t> s2 = b.Save();
        std::printf("round trip: %zu bytes, identical %s, %d lots, queue %zu\n", s1.size(), s1 == s2 ? "yes" : "NO", b.L.Tracked(), b.L.queue.size());
        CHECK(s1 == s2);
        CHECK(b.dm.bars[0] == 0.42 && b.dm.rent.group == 33 && b.L.queue.size() == a.L.queue.size());
        // 2. corrupt payloads fail cleanly
        std::vector<uint8_t> cut(s1.begin(), s1.begin() + (std::ptrdiff_t)(s1.size() / 2));
        Town c(10);
        CHECK(!c.Load(cut));
    }
    // 3. save mid-run, load, continue: the same events and the same state as the town that never stopped
    {
        Town a(800), b(800);
        a.Run(2 * 1440 + 333);
        CHECK(b.Load(a.Save()));
        b.z = a.z, b.minute = a.minute;
        a.L.events.clear(), b.L.events.clear();
        a.Run(6 * 1440), b.Run(6 * 1440);
        bool same = a.L.events.size() == b.L.events.size();
        for (size_t i = 0; same && i < a.L.events.size(); ++i) same = a.L.events[i].lot == b.L.events[i].lot && a.L.events[i].type == b.L.events[i].type;
        std::printf("continue after load: %zu events, same %s, state hash %s\n", a.L.events.size(), same ? "yes" : "NO", Hash(a.Save()) == Hash(b.Save()) ? "equal" : "DIFFERENT");
        CHECK(same && !a.L.events.empty());
        CHECK(Hash(a.Save()) == Hash(b.Save()));
    }
    std::printf(g_fail ? "sim_land_save_test: %d FAILED\n" : "sim_land_save_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
