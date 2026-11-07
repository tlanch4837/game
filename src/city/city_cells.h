// The CityCell: the fixed 128 m world grid both the city sim (aggregates) and the renderer (chunk bakes) work in. A lot
// belongs to the cell its centre lies in. Header only, no renderer or scene dependencies (tests\sim_cells_test.cpp).
//
// Dirty bits: bit0 render (a lot spawned, changed level, was demolished or recoloured), bit1 sim, bit2 flow. Each side
// clears its own bit when it has caught up; renderVersion / simVersion count every change so a cache can key on them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tbd {
namespace city {

constexpr float kCityCellSize = 128.0f; // two terrain chunks

struct CellKey {
    int32_t x = 0, z = 0;
    bool operator==(const CellKey& o) const { return x == o.x && z == o.z; }
    bool operator!=(const CellKey& o) const { return !(*this == o); }
};
inline uint64_t PackKey(CellKey k) { return ((uint64_t)(uint32_t)k.x << 32) | (uint32_t)k.z; }
inline CellKey CellKeyAt(float x, float z) { return {(int32_t)std::floor(x / kCityCellSize), (int32_t)std::floor(z / kCityCellSize)}; }

enum CellDirty : uint8_t { kDirtyRender = 1, kDirtySim = 2, kDirtyFlow = 4 };

struct CityCell {
    CellKey key;
    std::vector<uint64_t> lots; // ids of the lots whose centre lies here, in the order they arrived
    uint32_t simVersion = 0, renderVersion = 0;
    uint8_t dirty = 0;
};

class CityCells {
public:
    std::vector<CityCell> cells; // stable indices: a cell is never removed, only emptied

    int Find(CellKey k) const {
        const auto it = index_.find(PackKey(k));
        return it == index_.end() ? -1 : it->second;
    }
    // The cell at k, made if it is new. `made` tells the caller (the sim grows its parallel arrays).
    int Get(CellKey k, bool* made = nullptr) {
        const uint64_t p = PackKey(k);
        const auto it = index_.find(p);
        if (made) *made = it == index_.end();
        if (it != index_.end()) return it->second;
        CityCell c;
        c.key = k;
        cells.push_back(c);
        index_[p] = (int)cells.size() - 1;
        return (int)cells.size() - 1;
    }
    void AddLot(int cell, uint64_t id) { cells[(size_t)cell].lots.push_back(id); }
    void RemoveLot(int cell, uint64_t id) {
        auto& v = cells[(size_t)cell].lots;
        const auto it = std::find(v.begin(), v.end(), id);
        if (it != v.end()) v.erase(it);
    }
    void Touch(int cell, uint8_t bits) {
        CityCell& c = cells[(size_t)cell];
        c.dirty |= bits;
        if (bits & kDirtyRender) ++c.renderVersion;
        if (bits & (kDirtySim | kDirtyFlow)) ++c.simVersion;
    }
    void Clear() {
        cells.clear();
        index_.clear();
    }

private:
    std::unordered_map<uint64_t, int> index_;
};

} // namespace city
} // namespace tbd
