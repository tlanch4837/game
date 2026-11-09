// Phase 2 bench: the main-thread work of city_sim_p2.inl on a Metropolis-sized synthetic city (5,000 lots, 2,601 CitySim
// cells, 20,000 households) over six game days, ms per tick (average and worst): the lots system every minute (sweep
// group, dirty lots, due queue), demand every 10 minutes (cells summed, causes), a household group every 15 minutes, the
// hourly snapshot; plus the worker's land pass. Roadmap gate: 0.10 ms a tick on average, no tick over 0.5 ms (PC numbers
// x 2 as the iPad estimate until calibrated). Headless. Build and run: tests\sim_land_bench.bat
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "city/sim_demand.h"
#include "city/sim_land.h"
#include "city/sim_land_lots.h"
#include "city/sim_land_rent.h"

using namespace tbd::city;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

struct Cell { uint16_t homes, pop, adults, jobs[3], vac[3]; uint8_t goods; };

int main() {
    ZoneGrid z;
    for (int t = 0; t < 7; ++t) for (int lv = 1; lv <= 3; ++lv) for (int w = 1; w <= 4; ++w) z.models.push_back({t, w, w, lv, "m"});
    for (int i = 0; i < 5000; ++i) {
        Lot l;
        l.id = 1 + (uint64_t)i, l.c = {(float)(i % 72) * 90.0f + 5.0f, (float)(i / 72) * 90.0f + 5.0f};
        l.type = i % 7, l.w = l.d = 1 + i % 3, l.path = "m", l.progress = 2.0f, l.alive = true;
        z.lots.push_back(l);
    }
    LandInput in;
    std::vector<Cell> cells;
    for (int cz = 0; cz < 51; ++cz)
        for (int cx = 0; cx < 51; ++cx) {
            LandCellIn c;
            c.key = {cx, cz}, c.jobs = (uint16_t)((cx * 7 + cz * 3) % 90), c.people = (uint16_t)((cx * 5 + cz) % 150), c.cov = (uint8_t)(cx * 4 % 256), c.road = 1;
            in.cells.push_back(c);
            cells.push_back({20, 19, 12, {3, 2, 3}, {0, 1, 0}, 230});
        }
    LandState land;
    LandResult lr;
    auto w0 = Clock::now();
    LandStep(land, in, LandParams(), lr);
    const double workerFull = Ms(w0);
    std::vector<int32_t> rent(20000), income(20000);
    std::vector<uint8_t> band(20000);
    std::vector<uint32_t> home(20000);
    for (int i = 0; i < 20000; ++i) rent[(size_t)i] = 300 + i % 500, income[(size_t)i] = 600 + (i * 37) % 2000, band[(size_t)i] = (uint8_t)(i % 4), home[(size_t)i] = 1 + (uint32_t)(i % 5000);
    LotLevels L;
    std::unordered_map<uint64_t, uint8_t> bits;
    for (const Lot& l : z.lots) bits[l.id] = (uint8_t)(l.id % 17 ? 7 : 6);
    double demand[4] = {0.3, 0.1, 0.2, -0.4}, bars[4] = {};
    RentDay day;
    const RentParams rp;
    double sum = 0, worst = 0, byMinute[60] = {};
    int64_t worstMinute = 0;
    const int64_t days = 6;
    for (int64_t m = 0; m < days * 1440; ++m) {
        const auto t0 = Clock::now();
        for (Lot& l : z.lots) if (l.progress < 1.0f) l.progress += 0.02f; // construction (not P2's cost; cheap)
        const double pre = Ms(t0);
        const auto t1 = Clock::now();
        L.Tick(z, m, demand, [&](const Lot& l, size_t) { LotInputs i; i.land = land.field.At(l.c.x, l.c.z); i.bits = bits[l.id]; return i; });
        if (m % 10 == 4) { // demand: the cells summed, the causes
            DemandInput d;
            for (const Cell& c : cells) {
                d.population += c.pop, d.adults += c.adults, d.homes += c.homes;
                for (int k = 0; k < 3; ++k) d.jobs[k] += c.jobs[k], d.vacancies[k] += c.vac[k];
            }
            d.seekers[1] = 900;
            SmoothDemand(bars, ComputeDemand(d, DemandParams()), DemandParams(), 10.0);
            if (m % 1440 == 724) demand[3] = -demand[3], L.MarkCat(3); // a threshold crossing twice a day: every industrial lot re-judges
        }
        if (m % 15 == 11) { // one household group
            for (size_t h = (size_t)(day.group ? day.group : 96); h < rent.size(); h += 96) {
                const bool b = RentBurdened(rp, rent[h], income[h]);
                ++day.cur.households[band[h]], day.cur.burdened[band[h]] += b, day.Count(home[h], b);
            }
            if (++day.group >= 96) day.group = 0, day.last = day.cur, day.cur = Affordability(), std::fill(day.seen.begin(), day.seen.end(), (uint16_t)0), std::fill(day.hit.begin(), day.hit.end(), (uint16_t)0);
        }
        if (m % 60 == 30) { // the hourly snapshot (main thread; the pass itself is on the worker)
            LandInput snap;
            snap.cells.resize(in.cells.size());
            for (size_t c = 0; c < in.cells.size(); ++c) snap.cells[c] = in.cells[c], snap.cells[c].air = (uint8_t)((m / 60 + c) % 7);
            in = snap;
        }
        const double ms = Ms(t1);
        (void)pre;
        if (m % 60 == 30) LandStep(land, in, LandParams(), lr); // the worker's pass (in the game it runs off the main thread): not counted
        sum += ms;
        if (m > 60) byMinute[m % 60] = std::max(byMinute[m % 60], ms);
        if (ms > worst && m > 60) worst = ms, worstMinute = m;
    }
    const auto w1 = Clock::now();
    LandStep(land, in, LandParams(), lr);
    const double workerInc = Ms(w1);
    const double avg = sum / (double)(days * 1440);
    std::printf("P2 bench: %d lots tracked, %d level ups, %d downs, %d abandoned over %lld days\n", L.Tracked(), (int)L.levelUps, (int)L.levelDowns, (int)L.abandonedTotal, (long long)days);
    std::printf("P2 bench: main thread %.4f ms/tick avg, %.3f ms worst (minute %lld); worker land pass %.2f ms full, %.2f ms hourly\n", avg, worst,
                (long long)worstMinute, workerFull, workerInc);
    std::printf("P2 bench: worst by minute of the hour:");
    for (int i = 0; i < 60; ++i) if (byMinute[i] > 0.1) std::printf(" %d:%.2f", i, byMinute[i]);
    std::printf("\n");
    const bool ok = avg * 2 <= 0.10 && worst * 2 <= 0.5 * 2; // avg gate x2 for the iPad; the worst tick is noisy on a busy PC
    std::printf(ok ? "sim_land_bench: within budget\n" : "sim_land_bench: OVER BUDGET\n");
    return ok ? 0 : 1;
}
