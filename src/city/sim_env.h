// Phase 3 environment, the whole pass (CS2 sim roadmap, Phase 3). Header only, headless (tests\sim_env_*).
//
// EnvSim holds the state (pollution fields, water table, flood depth, which cells were flooded). Once a game hour the
// host snapshots an EnvInput (the hour's weather, the sources, the pumps, the CitySim cell keys) and runs EnvSim::Step on
// a copy of the state on a worker; the result (state + EnvResult) is applied at the NEXT hour boundary, whatever the
// thread timing, so the sim stays deterministic. Tests call Step directly.
// EnvResult is per CitySim cell (index order of input.cityKeys): the four factors P3 writes (air noise ground water),
// the deepest flood and the P3 flag bits (polluted, noisy, flooded), plus city stats for the panel.
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_buildings.h"
#include "sim_env_pollution.h"
#include "sim_env_water.h"
#include "sim_events.h"
#include "sim_factors.h"
#include "sim_flags.h"

namespace tbd {
namespace city {

struct EnvParams {
    EnvPollutionParams poll;
    EnvWaterParams water;
    uint8_t airLimit = 90, groundLimit = 80, waterLimit = 60, noiseLimit = 55; // flag thresholds (factor steps)
};

struct EnvInput {
    int64_t minute = 0;
    EnvWeather weather;
    float sea = -1e9f;
    std::vector<EnvSource> sources;
    std::vector<EnvPump> pumps;
    std::vector<CellKey> cityKeys;   // CitySim cell keys, by cell index
    std::vector<uint8_t> cityWaterOk;// per CitySim cell: served by the water network (1) or not
    Field<float> heights;            // terrain heights of cells new to the water fields (or all, after a terrain edit)
};

struct EnvCellOut {
    uint8_t air = 0, noise = 0, ground = 0, water = 0;
    uint16_t floodMm = 0;
    uint32_t flags = 0;
};
struct EnvResult {
    int64_t minute = -1;
    std::vector<CellKey> keys;     // the CityCell of each entry (input.cityKeys: CitySim cell index order at the pass)
    std::vector<EnvCellOut> cells; // by CitySim cell index
    std::vector<int> newlyFlooded; // CitySim cells that went under water this pass (Flood events)
    double meanAir = 0, meanNoise = 0, meanGround = 0, meanTable = 0;
    uint8_t supply = 0;            // pollution of the pumped water (factor steps)
    double pumpEff = 1.0;          // capacity-weighted groundwater pump efficiency
    int flooded = 0, polluted = 0, noisy = 0;
};

struct EnvSim {
    EnvPollution poll;
    EnvWater water;
    std::vector<uint8_t> wasFlooded; // per CitySim cell
    int64_t lastMinute = -1;         // the last pass's minute

    void Clear() { poll.Clear(), water.Clear(), wasFlooded.clear(), lastMinute = -1; }

    void Step(const EnvInput& in, const EnvParams& p, EnvResult& out) {
        // the water fields cover the city's cells; new ones (and re-sampled ones) bring their heights
        for (size_t i = 0; i < in.heights.Keys().size(); ++i) {
            const CellKey k = in.heights.Keys()[i];
            const auto& src = in.heights.Tiles()[i];
            water.EnsureCell(k, p.water.natural, [](float, float) { return 0.0f; });
            water.height.Cell(k) = src;
        }
        poll.Step(in.sources, in.weather, p.poll);
        water.Step(in.weather.rainMmH, in.weather.season, in.sea, in.pumps, p.water);
        lastMinute = in.minute;
        // pumped water: shore intakes drink the plume, groundwater pumps the ground pollution that seeps down
        double wsum = 0, psum = 0, esum = 0, gsum = 0;
        for (const EnvPump& pu : in.pumps) {
            if (pu.capacity <= 0) continue;
            const double eff = pu.ground ? water.PumpEfficiency(pu, p.water) : 1.0;
            const double pol = pu.ground ? 0.6 * EnvFactorAt(poll.ground, pu.x, pu.z) : EnvFactorAt(poll.water, pu.x, pu.z);
            wsum += pu.capacity * std::max(0.05, eff), psum += pu.capacity * std::max(0.05, eff) * pol;
            if (pu.ground) esum += pu.capacity * eff, gsum += pu.capacity;
        }
        out = EnvResult();
        out.minute = in.minute;
        out.keys = in.cityKeys;
        out.supply = (uint8_t)std::min(255.0, wsum > 0 ? psum / wsum : 0.0);
        out.pumpEff = gsum > 0 ? esum / gsum : 1.0;
        const size_t n = in.cityKeys.size();
        out.cells.resize(n);
        if (wasFlooded.size() < n) wasFlooded.resize(n, 0);
        for (size_t c = 0; c < n; ++c) {
            const CellKey k = in.cityKeys[c];
            EnvCellOut& o = out.cells[c];
            o.air = EnvFactorCell(poll.air, k), o.noise = EnvFactorCell(poll.noise, k), o.ground = EnvFactorCell(poll.ground, k);
            o.water = c < in.cityWaterOk.size() && in.cityWaterOk[c] ? out.supply : 0;
            o.floodMm = water.MaxDepth(k);
            if (o.air >= p.airLimit || o.ground >= p.groundLimit || o.water >= p.waterLimit) o.flags |= kFlagPolluted, ++out.polluted;
            if (o.noise >= p.noiseLimit) o.flags |= kFlagNoisy, ++out.noisy;
            const bool fl = o.floodMm >= p.water.floodMm;
            if (fl) o.flags |= kFlagFlooded, ++out.flooded;
            if (fl && !wasFlooded[c]) out.newlyFlooded.push_back((int)c);
            wasFlooded[c] = fl ? 1 : 0;
            out.meanAir += o.air, out.meanNoise += o.noise, out.meanGround += o.ground;
            out.meanTable += water.table.CellMean(k);
        }
        if (n) out.meanAir /= (double)n, out.meanNoise /= (double)n, out.meanGround /= (double)n, out.meanTable /= (double)n;
    }
};

// P3's factor columns only (air noise ground water). indexOf(key, i): the CitySim cell index of entry i now (-1 skips;
// after a load the cells come back in another order). True if anything changed (then version is bumped).
template <typename IndexOf>
bool WriteEnvFactors(const EnvResult& r, CellFactors& f, IndexOf&& indexOf) {
    bool changed = false;
    for (size_t i = 0; i < r.cells.size() && i < r.keys.size(); ++i) {
        const int c = indexOf(r.keys[i], i);
        if (c < 0) continue;
        f.Fit((size_t)c + 1);
        CellFactor& x = f.cells[(size_t)c];
        const EnvCellOut& o = r.cells[i];
        changed |= x.air != o.air || x.noise != o.noise || x.ground != o.ground || x.water != o.water;
        x.air = o.air, x.noise = o.noise, x.ground = o.ground, x.water = o.water;
    }
    if (changed) ++f.version;
    return changed;
}
inline bool WriteEnvFactors(const EnvResult& r, CellFactors& f) {
    return WriteEnvFactors(r, f, [](CellKey, size_t i) { return (int)i; });
}

// P3's flag bits (polluted, noisy, flooded) onto every live building, by the CityCell its row names.
inline void WriteEnvFlags(const EnvResult& r, const BuildingRegistry& reg, SimFlagTable& t) {
    const SimFlags mask = SimFlagPhaseMask(3);
    std::unordered_map<uint64_t, uint32_t> bits;
    for (size_t c = 0; c < r.cells.size() && c < r.keys.size(); ++c)
        if (r.cells[c].flags) bits[PackKey(r.keys[c])] = r.cells[c].flags;
    for (uint32_t i = 1; i < reg.rows.size(); ++i) {
        const BuildingRow& row = reg.rows[i];
        if (row.alive != 1) continue;
        const BuildingId b = reg.At(i);
        if (!bits.empty()) {
            const auto it = bits.find(PackKey({row.cellX, row.cellZ}));
            t.Write(b, mask, it == bits.end() ? 0u : it->second);
        } else if (t.Get(b) & mask) {
            t.Write(b, mask, 0u);
        }
    }
}

// Flood events (P4 dispatch reads them): one per cell that went under water, value = its deepest tile in mm.
inline void PushEnvEvents(const EnvResult& r, SimEventRing& ev) {
    for (int c : r.newlyFlooded) ev.Push(SimEventType::Flood, r.minute, kNone, kNone, r.cells[(size_t)c].floodMm);
}

} // namespace city
} // namespace tbd
