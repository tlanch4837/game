// A synthetic town for the Phase 5 tests (sim_transit_test): zones on a grid, commute links by gravity, lines along
// rows of zones. Header only.
#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "city/sim_transit_save.h"

using namespace tbd::city;

static int g_fails = 0;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fails; } \
    } while (0)

struct TransitTown {
    int side = 8;
    float spacing = 512.0f;
    std::vector<TransitZoneIn> zones;
    std::vector<TransitLinkIn> links;
    std::vector<LineInput> lines;
    uint64_t rng = 0x1234567ull;
    uint32_t Rand() { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(rng >> 33); }

    static uint64_t Key(int x, int z) { return (uint64_t)(uint32_t)x << 32 | (uint32_t)z; }
    void Build(int n, float sp, int linksPer) {
        side = n, spacing = sp, zones.clear(), links.clear();
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                TransitZoneIn q;
                q.key = Key(x, z), q.x = x * sp, q.z = z * sp;
                q.pop = 400 + Rand() % 1200, q.jobs = 200 + Rand() % 900;
                const int rich = (x + z) % 4;
                for (int k = 0; k < 4; ++k) q.wealth[k] = (float)(k == rich ? 3 : 1);
                zones.push_back(q);
            }
        for (int a = 0; a < n * n; ++a)
            for (int k = 0; k < linksPer; ++k) {
                const int b = (int)(Rand() % (uint32_t)(n * n));
                if (b == a) continue;
                links.push_back({zones[(size_t)a].key, zones[(size_t)b].key, (float)(20 + Rand() % 120)});
            }
    }
    // A line along zone row `row` (every zone centre a stop), `vehicles` vehicles, mode 0 bus.
    LineInput Row(uint64_t id, int row, int vehicles, int mode = 0) const {
        LineInput l;
        l.id = id, l.mode = (uint8_t)mode, l.vehicles = vehicles;
        for (int x = 0; x < side; ++x) l.x.push_back(x * spacing), l.z.push_back(row * spacing);
        return l;
    }
    LineInput Column(uint64_t id, int col, int vehicles, int mode = 0) const {
        LineInput l;
        l.id = id, l.mode = (uint8_t)mode, l.vehicles = vehicles;
        for (int z = 0; z < side; ++z) l.x.push_back(col * spacing), l.z.push_back(z * spacing);
        return l;
    }
    void Feed(TransitSim& s) const { s.SetZones(zones), s.SetLinks(links), s.SetLines(lines); }
    void Churn(double amount) { // populations and wealth drift (some past the dirty threshold)
        for (TransitZoneIn& q : zones) {
            const double f = 1.0 + amount * (((double)(Rand() % 2001) - 1000.0) / 1000.0);
            q.pop = (uint32_t)std::max(0.0, q.pop * f);
            q.wealth[Rand() % 4] += (float)(amount * 2.0 * (Rand() % 3));
        }
        for (TransitLinkIn& l : links) l.n = std::max(1.0f, l.n * (float)(1.0 + amount * (((double)(Rand() % 2001) - 1000.0) / 1000.0)));
    }
};

static std::vector<uint8_t> SaveMode(const TransitSim& s) {
    ByteWriter w;
    WriteTransitMode(w, s);
    return w.bytes;
}
static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
