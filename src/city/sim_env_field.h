// Phase 3 field helpers on top of sim_field.h (CS2 sim roadmap, Phase 3). Header only, headless (tests\sim_env_*).
//
// Fields here are Field<uint16_t> in 1/256 steps of the 0..255 factor scale (integer state: saves round trip exactly).
//   EnvGrid        each cell's 3 x 3 neighbour cells (indices into the field's cells), built once per pass, so a
//                  neighbour sample is an array read instead of a hash lookup
//   EnvDiffuse     the same arithmetic as sim_field.h Diffuse (bit for bit, the tests check it) at grid speed
//   EnvDecay       floor(v * keep): unlike a rounded decay it reaches 0, so a removed source's trace disappears
//   EnvStampAll    adds cones of radius r tiles around many points (cells made on demand)
//   EnvPutField / EnvGetField   a field in a save chunk: keys, then tiles, raw
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_field.h"
#include "sim_save.h"

namespace tbd {
namespace city {

using EnvField = Field<uint16_t>;
constexpr double kEnvUnit = 256.0; // field units per factor step

struct EnvGrid {
    std::vector<std::array<int32_t, 9>> nb; // [cell][(dz + 1) * 3 + (dx + 1)], -1 missing; [4] is the cell itself
    template <typename T>
    void Build(const Field<T>& f) {
        const auto& keys = f.Keys();
        nb.assign(keys.size(), {});
        for (size_t i = 0; i < keys.size(); ++i)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const typename Field<T>::Tile* t = f.FindCell({keys[i].x + dx, keys[i].z + dz});
                    nb[i][(size_t)((dz + 1) * 3 + dx + 1)] = t ? (int32_t)(t - f.Tiles().data()) : -1;
                }
    }
    // The sample at local tile (lx, lz) of cell ci, lx / lz in -4 .. 7 (one neighbour cell either way).
    template <typename T>
    T At(const Field<T>& f, size_t ci, int lx, int lz) const {
        const int cx = lx < 0 ? -1 : lx >= kFieldRes ? 1 : 0, cz = lz < 0 ? -1 : lz >= kFieldRes ? 1 : 0;
        const int32_t j = nb[ci][(size_t)((cz + 1) * 3 + cx + 1)];
        if (j < 0) return f.neutral;
        return f.Tiles()[(size_t)j][(size_t)((lz - cz * kFieldRes) * kFieldRes + (lx - cx * kFieldRes))];
    }
};

// sim_field.h Diffuse, same results (k.steps steps), in place, using the grid. Writes only existing cells (the halo of
// sources makes room). No Field copies: two tile buffers swap between the steps. g: the field's grid (null: built here).
template <typename T>
void EnvDiffuseInPlace(Field<T>& f, const DiffuseKernel& k, const EnvGrid* grid = nullptr) {
    EnvGrid own;
    if (!grid) own.Build(f), grid = &own;
    const EnvGrid& g = *grid;
    using Tiles = std::vector<typename Field<T>::Tile>;
    auto& live = const_cast<Tiles&>(f.Tiles()); // f is not const: writing its tiles through the list is well defined
    Tiles curT = live, nextT(live.size());
    const double lo = (double)std::numeric_limits<T>::lowest(), hi = (double)std::numeric_limits<T>::max();
    const auto& keys = f.Keys();
    const int wx = std::clamp(k.windX, -1, 1), wz = std::clamp(k.windZ, -1, 1);
    constexpr int kPad = kFieldRes + 4; // local -2 .. 5: the 3 x 3 kernel around a sample shifted by the wind
    T buf[kPad * kPad];
    for (int s = 0; s < std::max(1, k.steps); ++s) {
        for (size_t ci = 0; ci < keys.size(); ++ci) {
            auto& t = nextT[ci];
            // gather local -2 .. 5 from the 3 x 3 cells: cell c covers local c * 4 .. c * 4 + 3
            bool any = false;
            for (int cz = -1; cz <= 1; ++cz)
                for (int cx = -1; cx <= 1; ++cx) {
                    const int32_t j = g.nb[ci][(size_t)((cz + 1) * 3 + cx + 1)];
                    const int x0 = std::max(-2, cx * kFieldRes), x1 = std::min(kPad - 3, cx * kFieldRes + kFieldRes - 1);
                    const int z0 = std::max(-2, cz * kFieldRes), z1 = std::min(kPad - 3, cz * kFieldRes + kFieldRes - 1);
                    const T* src = j < 0 ? nullptr : curT[(size_t)j].data();
                    for (int z = z0; z <= z1; ++z)
                        for (int x = x0; x <= x1; ++x) {
                            const T v = src ? src[(z - cz * kFieldRes) * kFieldRes + (x - cx * kFieldRes)] : f.neutral;
                            buf[(z + 2) * kPad + x + 2] = v, any |= v != T(0);
                        }
                }
            if (!any && T(0) == f.neutral) { t.fill(T(0)); continue; } // nothing near: stays 0
            for (int sz = 0; sz < kFieldRes; ++sz)
                for (int sx = 0; sx < kFieldRes; ++sx) {
                    const int x = sx - wx + 2, z = sz - wz + 2;
                    auto A = [&](int dx, int dz) { return buf[(z + dz) * kPad + x + dx]; };
                    double v = k.centre * (double)A(0, 0);
                    v += k.side * ((double)A(-1, 0) + A(1, 0) + A(0, -1) + A(0, 1));
                    v += k.corner * ((double)A(-1, -1) + A(1, -1) + A(-1, 1) + A(1, 1));
                    v *= k.keep;
                    if (std::numeric_limits<T>::is_integer) v = std::floor(v + 0.5);
                    t[(size_t)(sz * kFieldRes + sx)] = (T)std::clamp(v, lo, hi);
                }
        }
        curT.swap(nextT);
    }
    live.swap(curT);
}
template <typename T>
Field<T> EnvDiffuse(const Field<T>& in, const DiffuseKernel& k) {
    Field<T> out = in;
    EnvDiffuseInPlace(out, k);
    return out;
}

inline void EnvDecay(EnvField& f, double keep) {
    const uint32_t q = (uint32_t)std::clamp(std::lround(keep * 65536.0), 0L, 65536L);
    for (CellKey k : f.Keys())
        for (uint16_t& v : f.Cell(k)) v = (uint16_t)(((uint32_t)v * q) >> 16);
}

// Many stamps at once (the hourly pass: thousands of road tiles). Each adds amount (field units) at the tile of (x, z),
// falling off linearly to 0 at r + 1 tiles (r <= 4). The cells a source needs (its own and `halo` rings, at least one
// when r > 0) are made first, once per distinct cell; then the cones are written through an EnvGrid, so a tile costs an
// array read instead of a hash lookup. maxBlend: a tile keeps the loudest stamp instead of the sum (noise).
struct EnvStampItem { float x, z; double amount; int r; };
inline void EnvStampAll(EnvField& f, const std::vector<EnvStampItem>& items, int halo, bool maxBlend) {
    if (items.empty()) return;
    static const auto kW = [] { // cone weights [r][dz + 4][dx + 4], -1 outside the disc
        std::array<std::array<double, 81>, 5> w{};
        for (int r = 0; r <= 4; ++r)
            for (int dz = -4; dz <= 4; ++dz)
                for (int dx = -4; dx <= 4; ++dx) {
                    const double d = std::sqrt((double)(dx * dx + dz * dz));
                    w[(size_t)r][(size_t)((dz + 4) * 9 + dx + 4)] = std::abs(dx) > r || std::abs(dz) > r || d > r + 0.5 ? -1.0 : 1.0 - d / (r + 1.0);
                }
        return w;
    }();
    std::vector<CellKey> cellOf(items.size());
    std::unordered_map<uint64_t, int> made;
    uint64_t lastKey = ~0ull;
    int lastH = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const EnvStampItem& it = items[i];
        const int gx = (int)std::floor(it.x / kFieldTile), gz = (int)std::floor(it.z / kFieldTile);
        const CellKey c{EnvField::FloorDiv(gx, kFieldRes), EnvField::FloorDiv(gz, kFieldRes)};
        cellOf[i] = c;
        const int h = std::max(halo, it.r > 0 ? 1 : 0);
        const uint64_t key = PackKey(c);
        if (key == lastKey && h <= lastH) continue; // consecutive sources in one cell (a road)
        auto [m, fresh] = made.try_emplace(key, h);
        if (fresh || m->second < h) {
            for (int dz = -h; dz <= h; ++dz)
                for (int dx = -h; dx <= h; ++dx) f.Cell({c.x + dx, c.z + dz});
            m->second = h;
        }
        lastKey = key, lastH = m->second;
    }
    EnvGrid g;
    g.Build(f);
    auto& tiles = const_cast<std::vector<EnvField::Tile>&>(f.Tiles()); // f is not const (see EnvDiffuse)
    lastKey = ~0ull;
    size_t ci = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const EnvStampItem& it = items[i];
        const CellKey c = cellOf[i];
        if (PackKey(c) != lastKey) lastKey = PackKey(c), ci = (size_t)(f.FindCell(c) - f.Tiles().data());
        const int gx = (int)std::floor(it.x / kFieldTile), gz = (int)std::floor(it.z / kFieldTile);
        const int sx = gx - c.x * kFieldRes, sz = gz - c.z * kFieldRes, r = std::clamp(it.r, 0, 4);
        const auto& w = kW[(size_t)r];
        for (int dz = -r; dz <= r; ++dz) {
            const int lz = sz + dz, cz = lz < 0 ? -1 : lz >= kFieldRes ? 1 : 0;
            for (int dx = -r; dx <= r; ++dx) {
                const double wt = w[(size_t)((dz + 4) * 9 + dx + 4)];
                if (wt < 0) continue;
                const int lx = sx + dx, cx = lx < 0 ? -1 : lx >= kFieldRes ? 1 : 0;
                const int32_t j = g.nb[ci][(size_t)((cz + 1) * 3 + cx + 1)];
                if (j < 0) continue;
                uint16_t& v = tiles[(size_t)j][(size_t)((lz - cz * kFieldRes) * kFieldRes + (lx - cx * kFieldRes))];
                const double add = std::floor(it.amount * wt + 0.5);
                v = (uint16_t)std::min(65535.0, maxBlend ? std::max((double)v, add) : v + add);
            }
        }
    }
}

inline double EnvFieldSum(const EnvField& f) {
    double s = 0;
    for (const auto& t : f.Tiles()) for (uint16_t v : t) s += v;
    return s;
}

template <typename T>
void EnvPutField(ByteWriter& w, const Field<T>& f) {
    w.Put(f.neutral);
    w.PutVec(f.Keys());
    w.PutVec(f.Tiles());
}
template <typename T>
bool EnvGetField(ByteReader& r, Field<T>& f) {
    std::vector<CellKey> keys;
    std::vector<typename Field<T>::Tile> tiles;
    const T neutral = r.Get<T>();
    if (!r.GetVec(keys) || !r.GetVec(tiles) || keys.size() != tiles.size()) return false;
    f.Clear();
    f.neutral = neutral;
    for (size_t i = 0; i < keys.size(); ++i) f.Cell(keys[i]) = tiles[i];
    return r.ok;
}

} // namespace city
} // namespace tbd
