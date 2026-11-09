// Phase 3 cost bench: a synthetic city (default 40 x 40 CityCells = 5 x 5 km, the size of a 50k city) with industry,
// power plants, a landfill, sewage outlets, pumps and a road grid, run for one game day. Prints the worker's hourly pass
// (EnvSim::Step) and the main thread's share: the hourly apply (factors, flags over 20k buildings, the overlay index), a
// snapshot-sized copy of the input, and the weather every 10 minutes, as ms per tick on average. Gates (desktop; the
// iPad gate is x 2 until calibrated): main thread under 0.1 ms per tick average, the pass under 4 ms.
// Build and run: tests\sim_env_bench.bat [cells per side]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "city/sim_env.h"

using namespace tbd::city;

static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    const int side = argc > 1 ? std::max(4, std::atoi(argv[1])) : 40;
    EnvInput in;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            in.cityKeys.push_back({x, z}), in.cityWaterOk.push_back(1);
            auto& t = in.heights.Cell({x, z});
            for (int i = 0; i < 16; ++i) t[(size_t)i] = 10.0f + 0.002f * (float)((x * 4 + i % 4) * (z * 4 + i / 4) % 997);
        }
    const float W = side * kCityCellSize;
    // an industrial district in one corner, roads every 128 m, a few plants
    for (int i = 0; i < side * 20; ++i) in.sources.push_back(EnvIndustryLotSource(W * 0.1f + (i % 40) * 24.0f, W * 0.1f + (i / 40) * 24.0f, 1 + i % 3, 16));
    for (int z = 0; z < side; ++z)
        for (int t = 0; t < side * 4; ++t) {
            in.sources.push_back(EnvRoadSource(t * kFieldTile + 16, z * kCityCellSize + 16, 300.0f + 40.0f * (float)((t * 7 + z * 3) % 30), 0.8f));
            in.sources.push_back(EnvRoadSource(z * kCityCellSize + 16, t * kFieldTile + 16, 300.0f + 40.0f * (float)((t * 5 + z) % 30), 0.8f));
        }
    in.sources.push_back(EnvUtilitySource(W * 0.8f, W * 0.2f, 6));
    in.sources.push_back(EnvUtilitySource(W * 0.7f, W * 0.3f, 4));
    in.sources.push_back(EnvIndustrySiteSource(W * 0.9f, W * 0.9f, 5, 1.0f));
    in.sources.push_back(EnvUtilitySource(W * 0.05f, W * 0.95f, 13));
    for (int i = 0; i < 6; ++i) in.pumps.push_back({W * (0.2f + 0.1f * i), W * 0.6f, 4000.0f, true});
    in.pumps.push_back({W * 0.06f, W * 0.9f, 20000.0f, false});
    // buildings: 20k rows over the cells
    BuildingRegistry reg;
    reg.BeginSync();
    for (int i = 0; i < 20000; ++i) {
        BuildingRow r;
        r.cellX = i % side, r.cellZ = (i / side) % side;
        reg.Upsert(BOrigin::Lot, (uint64_t)i + 1, r);
    }
    reg.EndSync();
    std::printf("city: %d cells, %zu sources, %zu pumps, %u buildings\n", side * side, in.sources.size(), in.pumps.size(), reg.Live());

    EnvSim sim;
    EnvParams p;
    CellFactors factors;
    SimFlagTable flags;
    SimEventRing ring;
    const SimCalendar cal;
    WeatherChain wx;
    double stepMs = 0, stepWorst = 0, applyMs = 0, copyMs = 0, weatherMs = 0;
    const int hours = 24;
    for (int h = 0; h < hours; ++h) {
        // the weather every 10 minutes
        double t0 = NowMs();
        EnvWeather w;
        for (int m = 0; m < 60; m += 10) {
            const int64_t minute = (int64_t)h * 60 + m;
            wx = WeatherChain::Replay(minute / kWeatherStepMinutes, 7, cal, &wx);
            w = WeatherAt(wx, minute, 7, cal);
        }
        weatherMs += NowMs() - t0;
        // the snapshot (a copy of the input, as the host makes) and the pass
        t0 = NowMs();
        EnvInput job = in;
        job.minute = (int64_t)h * 60, job.weather = w;
        if (h > 0) job.heights.Clear(); // only the first pass brings the heights
        if (h % 6 == 1) job.weather.rainMmH = 25.0f; // storms
        EnvSim state = sim;
        copyMs += NowMs() - t0;
        t0 = NowMs();
        EnvResult res;
        state.Step(job, p, res);
        const double s = NowMs() - t0;
        stepMs += s, stepWorst = std::max(stepWorst, s);
        // the apply on the main thread
        t0 = NowMs();
        sim = std::move(state);
        WriteEnvFactors(res, factors);
        WriteEnvFlags(res, reg, flags);
        PushEnvEvents(res, ring);
        std::unordered_map<uint64_t, int> index;
        for (size_t i = 0; i < res.keys.size(); ++i) index[PackKey(res.keys[i])] = (int)i;
        applyMs += NowMs() - t0;
        if (h == hours - 1)
            std::printf("last pass: air mean %.1f, noise mean %.1f, %d polluted, %d noisy, %d flooded cells; air field %d cells\n", res.meanAir, res.meanNoise,
                        res.polluted, res.noisy, res.flooded, sim.poll.air.CellCount());
    }
    const double mainPerTick = (applyMs + copyMs + weatherMs) / (hours * 60.0);
    std::printf("worker pass: %.2f ms average, %.2f ms worst (gate 4 ms; iPad estimate x2 = %.2f ms)\n", stepMs / hours, stepWorst, 2 * stepMs / hours);
    std::printf("main thread: apply %.3f ms/h, snapshot copy %.3f ms/h, weather %.4f ms/h -> %.4f ms per tick (gate 0.10; iPad x2 = %.4f)\n", applyMs / hours,
                copyMs / hours, weatherMs / hours, mainPerTick, 2 * mainPerTick);
    const bool ok = mainPerTick < 0.1 && stepMs / hours < 4.0;
    std::printf(ok ? "sim_env_bench: within budget\n" : "sim_env_bench: OVER BUDGET\n");
    return ok ? 0 : 1;
}
