// Phase 3 pollution: air, noise, ground, water (CS2 sim roadmap, Phase 3). Header only, headless (tests\sim_env_*).
//
// Sources are points with an emission per game hour in factor steps (the host builds them: industry lots and hubs by
// output, power plants from the utilities catalogue, roads by flow x speed, landfill, sewage outlets). Once a game hour:
//   air     emitted, then spread by the 3 x 3 kernel and carried downwind (whole tiles per step), then decays
//   noise   recomputed from the sources, the loudest per tile (no diffusion, no memory: a road removed is quiet at once)
//   ground  accumulates where it is emitted and decays slowly (no spread)
//   water   outlet plumes: emitted at the outlet, spread over several steps, decays (the shore pumps sit in it)
// Fields are EnvField (sim_env_field.h). Emissions are quantised by the host before the pass, so it is deterministic.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_env_field.h"
#include "sim_env_weather.h"

namespace tbd {
namespace city {

struct EnvSource {
    float x = 0, z = 0;
    float air = 0, noise = 0, ground = 0, water = 0; // factor steps per game hour (noise: the level at the source)
    uint8_t airR = 1, noiseR = 2, groundR = 1, waterR = 1; // stamp radius, tiles
};

struct EnvPollutionParams {
    double airKeep = 0.75, groundKeep = 0.985, waterKeep = 0.80; // per game hour
    int airSteps = 2, waterSteps = 3;
    double windStepMs = 5.0; // one downwind step per this many m/s (at most airSteps)
    DiffuseKernel airKernel{0.5f, 0.1f, 0.025f, 1.0f, 0, 0, 1};
    DiffuseKernel waterKernel{0.4f, 0.125f, 0.025f, 1.0f, 0, 0, 1};
};

// The stamps of one hour's emissions, kept while the sources do not change (they rarely do between flow solves): the
// hourly pass then adds a ready field instead of re-stamping thousands of road tiles. Derived, never saved: the same
// sources give the same field, so a rebuilt cache (after a load) changes nothing.
struct EnvEmitCache {
    uint64_t hash = 0;
    bool valid = false;
    EnvField field;
    const EnvField& Get(const std::vector<EnvStampItem>& items, int halo, bool maxBlend) {
        uint64_t h = 1469598103934665603ull ^ (uint64_t)halo ^ ((uint64_t)maxBlend << 8);
        for (const EnvStampItem& it : items) {
            uint32_t b[2];
            std::memcpy(&b[0], &it.x, 4), std::memcpy(&b[1], &it.z, 4);
            h = (h ^ b[0]) * 1099511628211ull, h = (h ^ b[1]) * 1099511628211ull;
            h = (h ^ (uint64_t)std::llround(it.amount * 16.0)) * 1099511628211ull, h = (h ^ (uint64_t)it.r) * 1099511628211ull;
        }
        if (!valid || h != hash) field.Clear(), EnvStampAll(field, items, halo, maxBlend), hash = h, valid = true;
        return field;
    }
};
// dst += e per tile (saturating), cells of e made in dst in e's order.
inline void EnvAddField(EnvField& dst, const EnvField& e) {
    for (size_t i = 0; i < e.Keys().size(); ++i) {
        auto& t = dst.Cell(e.Keys()[i]);
        const auto& s = e.Tiles()[i];
        for (size_t j = 0; j < t.size(); ++j) t[j] = (uint16_t)std::min<uint32_t>(65535u, (uint32_t)t[j] + s[j]);
    }
}

struct EnvPollution {
    EnvField air, noise, ground, water;
    EnvEmitCache emitAir, emitNoise, emitGround, emitWater; // not saved (see EnvEmitCache)

    void Clear() { *this = EnvPollution(); }

    // One game hour. Sources in a fixed order (the host's list order); w: the weather of the hour.
    void Step(const std::vector<EnvSource>& src, const EnvWeather& w, const EnvPollutionParams& p) {
        std::vector<EnvStampItem> items;
        auto emit = [&](EnvEmitCache& c, float EnvSource::*amount, uint8_t EnvSource::*radius, int halo, bool maxBlend) -> const EnvField& {
            items.clear();
            for (const EnvSource& s : src)
                if (s.*amount > 0) items.push_back({s.x, s.z, s.*amount * kEnvUnit, s.*radius});
            return c.Get(items, halo, maxBlend);
        };
        // air: emit, diffuse (some steps downwind), decay
        EnvAddField(air, emit(emitAir, &EnvSource::air, &EnvSource::airR, 2, false));
        const int windSteps = std::clamp((int)(w.windMs / std::max(0.1, p.windStepMs)), 0, p.airSteps);
        EnvGrid g;
        g.Build(air); // diffusion makes no cells: one grid for every step
        for (int i = 0; i < p.airSteps; ++i) {
            DiffuseKernel k = p.airKernel;
            k.steps = 1;
            if (i < windSteps) k.windX = w.windX, k.windZ = w.windZ;
            EnvDiffuseInPlace(air, k, &g);
        }
        EnvDecay(air, p.airKeep * (w.rainMmH > 0 ? 0.85 : 1.0)); // rain washes the air
        // noise: rebuilt
        noise = emit(emitNoise, &EnvSource::noise, &EnvSource::noiseR, 0, true); // levels do not add up: the loudest wins
        // ground: accumulate and decay
        EnvAddField(ground, emit(emitGround, &EnvSource::ground, &EnvSource::groundR, 0, false));
        EnvDecay(ground, p.groundKeep);
        // water: plumes
        EnvAddField(water, emit(emitWater, &EnvSource::water, &EnvSource::waterR, 3, false));
        if (water.CellCount()) {
            DiffuseKernel k = p.waterKernel;
            k.steps = p.waterSteps;
            EnvDiffuseInPlace(water, k);
        }
        EnvDecay(water, p.waterKeep);
    }
};

// A field's value in factor steps (0..255) at a point / a cell mean.
inline uint8_t EnvFactorAt(const EnvField& f, float x, float z) { return (uint8_t)std::min(255.0, f.At(x, z) / kEnvUnit); }
inline uint8_t EnvFactorCell(const EnvField& f, CellKey k) { return (uint8_t)std::min(255.0, f.CellMean(k) / kEnvUnit); }
inline uint8_t EnvFactorCellMax(const EnvField& f, CellKey k) {
    const EnvField::Tile* t = f.FindCell(k);
    if (!t) return (uint8_t)std::min(255.0, f.neutral / kEnvUnit);
    uint16_t m = 0;
    for (uint16_t v : *t) m = std::max(m, v);
    return (uint8_t)std::min(255.0, m / kEnvUnit);
}

// ---------------------------------------------------------------- source tables (the host maps its buildings onto these)
// Industrial zone lot: by level and lot size (8 m cells).
inline EnvSource EnvIndustryLotSource(float x, float z, int level, int cells) {
    EnvSource s;
    s.x = x, s.z = z;
    const float size = std::clamp(cells / 16.0f, 0.5f, 3.0f);
    s.air = EnvQuant(6.0f * size * (1.0f + 0.25f * (level - 1)), 0.25f);
    s.noise = EnvQuant(std::min(120.0f, 45.0f * size), 1.0f);
    s.ground = EnvQuant(0.4f * size, 0.05f);
    return s;
}
inline EnvSource EnvCommercialLotSource(float x, float z, int cells) {
    EnvSource s;
    s.x = x, s.z = z, s.noise = EnvQuant(std::clamp(cells * 1.5f, 10.0f, 40.0f), 1.0f), s.noiseR = 1;
    return s;
}
// Industry hub kinds (industry.h IndKind order: Grain, Livestock, Timber, Oil, Ore, Landfill).
inline EnvSource EnvIndustrySiteSource(float x, float z, int kind, float eff) {
    static const float kAir[6] = {2, 4, 3, 24, 16, 6}, kNoise[6] = {30, 25, 70, 80, 90, 50}, kGround[6] = {0.5f, 1.25f, 0.25f, 2.5f, 2.0f, 3.0f};
    EnvSource s;
    const int k = std::clamp(kind, 0, 5);
    const float e = std::clamp(eff, 0.25f, 1.5f);
    s.x = x, s.z = z, s.air = EnvQuant(kAir[k] * e, 0.25f), s.noise = kNoise[k], s.ground = EnvQuant(kGround[k] * e, 0.25f);
    s.airR = 2, s.noiseR = 3, s.groundR = k == 5 ? 3 : 2;
    return s;
}
// Utility buildings, utilities.h UtilDefs() order.
inline EnvSource EnvUtilitySource(float x, float z, int def) {
    static const float kAir[15] = {8, 0, 0, 60, 30, 4, 120, 0, 0, 0, 0, 0, 0, 0, 3};
    static const float kNoise[15] = {60, 45, 5, 90, 70, 50, 110, 60, 10, 30, 20, 40, 50, 20, 40};
    static const float kWater[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 180, 25};
    EnvSource s;
    if (def < 0 || def >= 15) return s;
    s.x = x, s.z = z, s.air = kAir[def], s.noise = kNoise[def], s.water = kWater[def];
    s.airR = def == 6 || def == 3 ? 3 : 1, s.noiseR = def == 6 || def == 3 ? 3 : 2, s.waterR = 2;
    return s;
}
// A road sample (one per 32 m along a segment): vehicles per hour and the speed ratio from the flow solve.
inline EnvSource EnvRoadSource(float x, float z, float flowVph, float speedRatio) {
    EnvSource s;
    s.x = x, s.z = z;
    const float f = std::max(0.0f, flowVph);
    s.noise = EnvQuant(std::min(140.0f, 12.0f * std::log2(1.0f + f / 50.0f) * (0.6f + 0.4f * std::clamp(speedRatio, 0.0f, 1.0f))), 1.0f);
    s.air = EnvQuant(std::min(8.0f, f / 400.0f * (1.5f - 0.5f * std::clamp(speedRatio, 0.0f, 1.0f))), 0.25f);
    s.noiseR = f > 1500 ? 2 : 1, s.airR = 1;
    return s;
}

} // namespace city
} // namespace tbd
