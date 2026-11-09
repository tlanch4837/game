// Phase 2 land value and rent (src/city/sim_land.h, sim_land_rent.h): a park raises nearby value, industry and pollution
// lower it, monotonic in coverage, the incremental pass equals a full recompute after random edits, deterministic, rent
// follows land value and density, affordability, and a bench. Headless. Build and run: tests\sim_land_test.bat
#include <chrono>
#include <cstdio>
#include <vector>

#include "city/sim.h"
#include "city/sim_land.h"
#include "city/sim_land_rent.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LandInput Town(int r) { // cells -r..r, a modest town everywhere
    LandInput in;
    for (int z = -r; z <= r; ++z)
        for (int x = -r; x <= r; ++x) {
            LandCellIn c;
            c.key = {x, z}, c.jobs = 20, c.people = 40, c.cov = 80, c.road = 1, c.commuteMin = 12;
            in.cells.push_back(c);
        }
    return in;
}
static LandCellIn& At(LandInput& in, int x, int z) {
    for (LandCellIn& c : in.cells) if (c.key.x == x && c.key.z == z) return c;
    return in.cells[0];
}
static uint8_t Val(const LandState& s, int x, int z) { return s.value.at(PackKey({x, z})); }
static double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

int main() {
    const LandParams p;
    // 1. a park raises the cell and (through the field) the samples next to it
    {
        LandInput in = Town(3);
        const LandState a = LandFull(in, p);
        At(in, 0, 0).park = 255;
        const LandState b = LandFull(in, p);
        std::printf("park: cell %d -> %d, east tile %d -> %d\n", Val(a, 0, 0), Val(b, 0, 0), a.field.At(140, 64), b.field.At(140, 64));
        CHECK(Val(b, 0, 0) > Val(a, 0, 0));
        CHECK(b.field.At(140.0f, 64.0f) > a.field.At(140.0f, 64.0f)); // the first sample of the next cell east
        CHECK(Val(b, 3, 3) == Val(a, 3, 3));                          // far away: unchanged
    }
    // 2. industry lowers the cell and its neighbours; pollution and crime lower the cell
    {
        LandInput in = Town(3);
        const LandState a = LandFull(in, p);
        At(in, 0, 0).industry = 250, At(in, 0, 0).jobs = 270;
        const LandState b = LandFull(in, p);
        std::printf("industry: cell %d -> %d, neighbour %d -> %d\n", Val(a, 0, 0), Val(b, 0, 0), Val(a, 1, 0), Val(b, 1, 0));
        CHECK(Val(b, 0, 0) < Val(a, 0, 0) && Val(b, 1, 0) < Val(a, 1, 0));
        LandInput q = Town(3);
        At(q, 0, 0).air = 200, At(q, 1, 0).noise = 200, At(q, 2, 0).crime = 200;
        const LandState c = LandFull(q, p);
        CHECK(Val(c, 0, 0) < Val(a, 0, 0) && Val(c, 1, 0) < Val(a, 1, 0) && Val(c, 2, 0) < Val(a, 2, 0));
    }
    // 3. monotonic in coverage
    {
        int last = -1;
        bool mono = true;
        for (int cov = 0; cov <= 255; cov += 5) {
            LandInput in = Town(1);
            At(in, 0, 0).cov = (uint8_t)cov;
            const int v = Val(LandFull(in, p), 0, 0);
            mono &= v >= last, last = v;
        }
        CHECK(mono);
    }
    // 4. incremental == full after random edits, and the incremental pass touches only what changed
    {
        LandInput in = Town(12);
        LandState inc;
        LandResult r;
        LandStep(inc, in, p, r);
        SplitMix64 gen(7);
        uint64_t rng = 0;
        bool same = true;
        int maxWork = 0;
        for (int it = 0; it < 300; ++it) {
            for (int e = 0; e < 3; ++e) {
                rng = gen.Next();
                LandCellIn& c = in.cells[(size_t)(rng % in.cells.size())];
                const int f = (int)((rng >> 20) % 5), v = (int)((rng >> 32) % 256);
                if (f == 0) c.park = (uint8_t)v; else if (f == 1) c.air = (uint8_t)v; else if (f == 2) c.jobs = (uint16_t)(v * 3);
                else if (f == 3) c.industry = (uint16_t)v; else c.cov = (uint8_t)v;
            }
            LandStep(inc, in, p, r);
            maxWork = std::max(maxWork, r.recomputed);
            if (it % 25 == 0) same &= LandSame(inc, LandFull(in, p), in);
        }
        same &= LandSame(inc, LandFull(in, p), in);
        std::printf("incremental: equals full %s, at most %d of %zu cells recomputed per pass\n", same ? "yes" : "NO", maxWork, in.cells.size());
        CHECK(same);
        CHECK(maxWork <= 27);
        LandStep(inc, in, p, r);
        CHECK(r.recomputed == 0 && r.changed.empty()); // nothing changed: nothing done
    }
    // 5. rent: up with land value, down with density, the policy factor; affordability
    {
        const RentParams rp;
        CHECK(RentBurden(rp, 200, kDensityLow) > RentBurden(rp, 60, kDensityLow));
        CHECK(RentBurden(rp, 110, kDensityHigh) < RentBurden(rp, 110, kDensityLow));
        CHECK(RentBurden(rp, 110, kDensityRow) == 64); // the reference: the flat rent
        CHECK(RentBurden(rp, 110, kDensityRow, 1.5f) > 64);
        CHECK(RentBurdened(rp, 500, 1000) && !RentBurdened(rp, 300, 1000) && RentBurdened(rp, 10, 0) && !RentBurdened(rp, 0, 0));
        CHECK(DensityOf(0) == kDensityLow && DensityOf(3) == kDensityHigh && DensityOf(4) == kDensityMedium);
        RentDay d;
        d.Count(5, true), d.Count(5, false), d.Count(6, false);
        CHECK(d.High(rp, 5) && !d.High(rp, 6) && !d.High(rp, 99));
    }
    // 6. bench: a 50k-resident city is about 2,500 cells; the worker's full pass and a typical hourly pass
    {
        LandInput in = Town(25); // 2,601 cells
        auto t0 = std::chrono::steady_clock::now();
        LandState s = LandFull(in, p);
        const double full = Ms(t0);
        for (int e = 0; e < 60; ++e) in.cells[(size_t)(e * 37 % in.cells.size())].air = (uint8_t)(e * 3);
        LandResult r;
        t0 = std::chrono::steady_clock::now();
        LandStep(s, in, p, r);
        const double inc = Ms(t0);
        std::printf("bench: %zu cells, full pass %.3f ms (worker), 60 edits %.3f ms (%d cells, %d field cells)\n", in.cells.size(), full, inc, r.recomputed, r.tiles);
        CHECK(full < 5.0 * 4); // roadmap: under 5 ms on the iPad (PC x 2 as the gate, generous here)
    }
    std::printf(g_fail ? "sim_land_test: %d FAILED\n" : "sim_land_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
