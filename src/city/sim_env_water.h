// Phase 3 water table and floods (CS2 sim roadmap, Phase 3). Header only, headless (tests\sim_env_water_test).
//
// On the 32 m tiles of every city cell (the host lists them), three aligned fields (same keys, same order):
//   table   groundwater saturation 0..10000 (natural level 6000). Rain that soaks in recharges it, it relaxes towards
//           the natural level, summer evaporation lowers it, and groundwater pumps draw a cone down around them.
//   depth   surface water, mm. Rain adds to it; it soaks in (less when the ground is saturated), storm drains take a
//           fixed rate, and it runs to the lowest neighbour by water surface; tiles under the sea level drain away.
//   height  the terrain height of the tile centre, metres (sampled by the host; not saved, re-sampled after a load)
// A tile is flooded at depth >= floodMm. Pumps report their efficiency (the table under them over the natural level).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sim_env_field.h"

namespace tbd {
namespace city {

struct EnvWaterParams {
    int natural = 6000;           // table level with no pumping
    double relax = 0.03;          // share of the gap to the natural level closed per game hour
    double rechargePerMm = 40.0;  // table steps per mm soaked in
    double summerDryPerHour = 12; // table steps lost per game hour in summer
    double infilMmH = 6.0;        // what dry ground soaks in per hour (scaled by 1 - table / 10000)
    double drainMmH = 3.0;        // storm drains, per tile per hour
    double drainShare = 0.04;     // plus this share of the depth per hour (run-off, evaporation): floods recede
    double flowShare = 0.45;      // share of the surface difference that runs to the lowest neighbour per substep
    int substeps = 3;
    int floodMm = 300;
    int pumpRadius = 3;           // tiles
    double pumpDrawPerUnit = 0.005;// table steps per hour per m3/day of pump capacity at the centre
};

struct EnvPump {
    float x = 0, z = 0;
    float capacity = 0;  // m3 per day
    bool ground = true;  // a groundwater pump (draws the table); false: a surface (shore) intake
};

struct EnvWater {
    Field<uint16_t> table, depth;
    Field<float> height;

    EnvWater() { table.neutral = 6000, depth.neutral = 0, height.neutral = 0; }
    void Clear() { table.Clear(), depth.Clear(), height.Clear(); }

    // Adds a cell to all three fields (the same position in each). sampleHeight(x, z) gives the terrain height.
    template <typename H>
    void EnsureCell(CellKey k, int natural, H&& sampleHeight) {
        if (table.FindCell(k)) return;
        table.neutral = (uint16_t)natural;
        table.Cell(k), depth.Cell(k);
        auto& h = height.Cell(k);
        for (int sz = 0; sz < kFieldRes; ++sz)
            for (int sx = 0; sx < kFieldRes; ++sx)
                h[(size_t)(sz * kFieldRes + sx)] = sampleHeight((k.x * kFieldRes + sx + 0.5f) * kFieldTile, (k.z * kFieldRes + sz + 0.5f) * kFieldTile);
    }
    bool Aligned() const { return table.CellCount() == depth.CellCount() && table.CellCount() == height.CellCount(); }

    // One game hour. rainMmH from the weather; sea: tiles lower than this drain away (-1e9: no sea).
    void Step(double rainMmH, int season, float sea, const std::vector<EnvPump>& pumps, const EnvWaterParams& p) {
        if (!Aligned() || table.CellCount() == 0) return;
        const auto& keys = table.Keys();
        EnvGrid g;
        g.Build(table);
        // Field has no mutable tile list; the fields are not const objects, so writing through these is well defined
        auto& T = const_cast<std::vector<Field<uint16_t>::Tile>&>(table.Tiles());
        auto& D = const_cast<std::vector<Field<uint16_t>::Tile>&>(depth.Tiles());
        const auto& Hh = height.Tiles();
        const int sub = std::max(1, p.substeps);
        std::vector<Field<uint16_t>::Tile> next;
        for (int s = 0; s < sub; ++s) {
            next = D;
            for (size_t ci = 0; ci < keys.size(); ++ci)
                for (int i = 0; i < kFieldRes * kFieldRes; ++i) {
                    const int lx = i % kFieldRes, lz = i / kFieldRes;
                    const double h = Hh[ci][(size_t)i];
                    int32_t d = D[ci][(size_t)i];
                    if (h < sea) { next[ci][(size_t)i] = 0; continue; }
                    if (d <= 0) continue; // dry: nothing runs off (most tiles, most hours)
                    // to the lowest neighbour (by surface), from this substep's start values
                    const double surf = h + d / 1000.0;
                    double best = surf;
                    int bi = -1, bc = -1;
                    static const int kDx[4] = {-1, 1, 0, 0}, kDz[4] = {0, 0, -1, 1};
                    for (int n = 0; n < 4; ++n) {
                        const int x = lx + kDx[n], z = lz + kDz[n];
                        const int cx = x < 0 ? -1 : x >= kFieldRes ? 1 : 0, cz = z < 0 ? -1 : z >= kFieldRes ? 1 : 0;
                        const int32_t cj = g.nb[ci][(size_t)((cz + 1) * 3 + cx + 1)];
                        if (cj < 0) continue; // the city's edge is closed
                        const int j = (z - cz * kFieldRes) * kFieldRes + (x - cx * kFieldRes);
                        const double ns = Hh[(size_t)cj][(size_t)j] + D[(size_t)cj][(size_t)j] / 1000.0;
                        if (ns < best) best = ns, bi = j, bc = cj;
                    }
                    if (bi >= 0 && d > 0) {
                        const int32_t move = (int32_t)std::min<double>(d, std::floor((surf - best) * 1000.0 * p.flowShare));
                        if (move > 0) {
                            next[ci][(size_t)i] = (uint16_t)(next[ci][(size_t)i] - move);
                            next[(size_t)bc][(size_t)bi] = (uint16_t)std::min(65535, next[(size_t)bc][(size_t)bi] + move);
                        }
                    }
                }
            // rain, soaking in, drains (per substep share of the hour)
            for (size_t ci = 0; ci < keys.size(); ++ci)
                for (int i = 0; i < kFieldRes * kFieldRes; ++i) {
                    if (Hh[ci][(size_t)i] < sea) { next[ci][(size_t)i] = 0; continue; } // the sea takes what ran into it
                    double d = next[ci][(size_t)i] + rainMmH / sub;
                    const double sat = T[ci][(size_t)i] / 10000.0;
                    const double soak = std::min(d, p.infilMmH * std::max(0.0, 1.0 - sat) / sub);
                    d -= soak;
                    d = std::max(0.0, d - p.drainMmH / sub - d * p.drainShare / sub);
                    next[ci][(size_t)i] = (uint16_t)std::min(65535.0, std::floor(d + 0.5));
                    T[ci][(size_t)i] = (uint16_t)std::min(10000.0, T[ci][(size_t)i] + std::floor(soak * p.rechargePerMm + 0.5));
                }
            D.swap(next);
        }
        // the table: relax, summer, pumps
        for (size_t ci = 0; ci < keys.size(); ++ci)
            for (uint16_t& v : T[ci]) {
                double t = v + std::trunc((p.natural - (double)v) * p.relax);
                if (season == 1) t -= p.summerDryPerHour;
                v = (uint16_t)std::clamp(t, 0.0, 10000.0);
            }
        for (const EnvPump& pu : pumps) {
            if (!pu.ground || pu.capacity <= 0) continue;
            const int gx = (int)std::floor(pu.x / kFieldTile), gz = (int)std::floor(pu.z / kFieldTile), r = p.pumpRadius;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const double dd = std::sqrt((double)(dx * dx + dz * dz));
                    if (dd > r + 0.5) continue;
                    const CellKey k{EnvField::FloorDiv(gx + dx, kFieldRes), EnvField::FloorDiv(gz + dz, kFieldRes)};
                    if (!table.FindCell(k)) continue;
                    uint16_t& v = table.Cell(k)[(size_t)((gz + dz - k.z * kFieldRes) * kFieldRes + (gx + dx - k.x * kFieldRes))];
                    v = (uint16_t)std::max(0.0, v - std::floor(pu.capacity * p.pumpDrawPerUnit * (1.0 - dd / (r + 1.0))));
                }
        }
    }
    // 0..1: what a groundwater pump still gets (the table under it over the natural level).
    double PumpEfficiency(const EnvPump& pu, const EnvWaterParams& p) const {
        return std::clamp(table.At(pu.x, pu.z) / (double)std::max(1, p.natural), 0.0, 1.0);
    }
    uint16_t MaxDepth(CellKey k) const {
        const auto* t = depth.FindCell(k);
        uint16_t m = 0;
        if (t) for (uint16_t v : *t) m = std::max(m, v);
        return m;
    }
};

} // namespace city
} // namespace tbd
