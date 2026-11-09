// Sparse scalar fields on 32 m tiles (CS2 sim roadmap 3.2, Phase 0; frozen interface). Header only, headless.
//
// A Field<T> stores, per occupied CityCell (128 m, city_cells.h), 4 x 4 samples of 32 m each: 16 values, 16 B for uint8.
// Cells are added on first write and never removed (Clear drops all). Pollution (P3) and land value (P2) live here.
// Heavy work goes on a worker: copy the field (Snapshot), Diffuse the copy, swap it in on the main thread at a tick.
// Everything is deterministic: cells are visited in key order, never hash order.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "city_cells.h"

namespace tbd {
namespace city {

constexpr int kFieldRes = 4;                                   // samples per cell side
constexpr float kFieldTile = kCityCellSize / (float)kFieldRes; // 32 m

template <typename T>
class Field {
public:
    using Tile = std::array<T, kFieldRes * kFieldRes>;
    T neutral = T(0); // the value of an unwritten sample

    int CellCount() const { return (int)tiles_.size(); }
    const Tile* FindCell(CellKey k) const { const auto it = index_.find(PackKey(k)); return it == index_.end() ? nullptr : &tiles_[(size_t)it->second]; }
    Tile& Cell(CellKey k) {
        const uint64_t p = PackKey(k);
        const auto it = index_.find(p);
        if (it != index_.end()) return tiles_[(size_t)it->second];
        index_[p] = (int)tiles_.size();
        keys_.push_back(k);
        Tile t;
        t.fill(neutral);
        tiles_.push_back(t);
        return tiles_.back();
    }
    const std::vector<CellKey>& Keys() const { return keys_; }
    const std::vector<Tile>& Tiles() const { return tiles_; }
    void Clear() { tiles_.clear(), keys_.clear(), index_.clear(); }

    // The sample a world point falls in (no interpolation: a 32 m tile is the field's resolution).
    static void SampleOf(float x, float z, CellKey& k, int& sx, int& sz) {
        const int gx = (int)std::floor(x / kFieldTile), gz = (int)std::floor(z / kFieldTile);
        k = {FloorDiv(gx, kFieldRes), FloorDiv(gz, kFieldRes)};
        sx = gx - k.x * kFieldRes, sz = gz - k.z * kFieldRes;
    }
    T At(float x, float z) const {
        CellKey k; int sx, sz;
        SampleOf(x, z, k, sx, sz);
        const Tile* t = FindCell(k);
        return t ? (*t)[(size_t)(sz * kFieldRes + sx)] : neutral;
    }
    void Set(float x, float z, T v) {
        CellKey k; int sx, sz;
        SampleOf(x, z, k, sx, sz);
        Cell(k)[(size_t)(sz * kFieldRes + sx)] = v;
    }
    // Sample at global tile coordinates (gx, gz) = floor(world / 32 m).
    T AtTile(int gx, int gz) const {
        const CellKey k{FloorDiv(gx, kFieldRes), FloorDiv(gz, kFieldRes)};
        const Tile* t = FindCell(k);
        return t ? (*t)[(size_t)((gz - k.z * kFieldRes) * kFieldRes + (gx - k.x * kFieldRes))] : neutral;
    }
    // The mean of a cell's 16 samples (the neutral value for an unwritten cell).
    double CellMean(CellKey k) const {
        const Tile* t = FindCell(k);
        if (!t) return (double)neutral;
        double s = 0;
        for (T v : *t) s += (double)v;
        return s / (double)t->size();
    }
    Field Snapshot() const { return *this; }

    static int FloorDiv(int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

private:
    std::vector<Tile> tiles_;
    std::vector<CellKey> keys_;
    std::unordered_map<uint64_t, int> index_;
};

// A 3 x 3 diffusion step: each sample becomes centre * self + side * (4 neighbours) + corner * (4 diagonals), shifted by
// the wind (whole tiles per step, -1..1 each axis: the sample takes its value from upwind), then decays by `keep`
// (1 = no decay). Applied `steps` times. Writes only cells that already exist in `in` (sources make cells); a value that
// drifts off the occupied cells is lost, which is the decay at the city's edge.
struct DiffuseKernel {
    float centre = 0.5f, side = 0.1f, corner = 0.025f;
    float keep = 1.0f;
    int windX = 0, windZ = 0;
    int steps = 1;
};

template <typename T>
Field<T> Diffuse(const Field<T>& in, const DiffuseKernel& k) {
    Field<T> cur = in;
    const double lo = (double)std::numeric_limits<T>::lowest(), hi = (double)std::numeric_limits<T>::max();
    std::vector<size_t> order(in.Keys().size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { const CellKey& p = in.Keys()[a]; const CellKey& q = in.Keys()[b]; return p.z != q.z ? p.z < q.z : p.x < q.x; });
    for (int s = 0; s < std::max(1, k.steps); ++s) {
        Field<T> next = cur;
        for (size_t oi : order) {
            const CellKey key = cur.Keys()[oi];
            auto& t = next.Cell(key);
            for (int sz = 0; sz < kFieldRes; ++sz)
                for (int sx = 0; sx < kFieldRes; ++sx) {
                    const int gx = key.x * kFieldRes + sx - k.windX, gz = key.z * kFieldRes + sz - k.windZ;
                    double v = k.centre * (double)cur.AtTile(gx, gz);
                    v += k.side * ((double)cur.AtTile(gx - 1, gz) + cur.AtTile(gx + 1, gz) + cur.AtTile(gx, gz - 1) + cur.AtTile(gx, gz + 1));
                    v += k.corner * ((double)cur.AtTile(gx - 1, gz - 1) + cur.AtTile(gx + 1, gz - 1) + cur.AtTile(gx - 1, gz + 1) + cur.AtTile(gx + 1, gz + 1));
                    v *= k.keep;
                    if (std::numeric_limits<T>::is_integer) v = std::floor(v + 0.5);
                    t[(size_t)(sz * kFieldRes + sx)] = (T)std::clamp(v, lo, hi);
                }
        }
        cur = std::move(next);
    }
    return cur;
}

} // namespace city
} // namespace tbd
