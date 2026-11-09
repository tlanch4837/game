// Land value (CS2 sim roadmap Phase 2). Header only, headless (tests\sim_land_test).
//
// One value per CitySim cell (0..255) from what is in and around it, then a 32 m field (sim_field.h, 4 x 4 samples per
// cell) by bilinear blending between the cell centres, which the lots sample for rent and levelling.
//   raises  service coverage (mean of the services), parks (coverage or park access), transit coverage, a road, jobs
//           and residents in the 3 x 3 cells around (accessibility), a view of water
//   lowers  air, noise and ground pollution (P3), crime (P4), industry jobs in the 3 x 3 cells, long commutes (P1)
//
// Deferred: LandState keeps every cell's input signature. A pass recomputes only the cells whose own or neighbours'
// inputs changed (and the field samples around them); everything else costs nothing. LandFull computes everything from
// scratch, so a test (and the debug check) can compare the two bit for bit. Pure integer maths on the inputs: the result
// does not depend on the thread it runs on.
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_factors.h"
#include "sim_field.h"

namespace tbd {
namespace city {

struct LandParams {
    int base = 40;          // an empty cell with nothing around
    int coverage = 70;      // at full mean service coverage
    int park = 40, transit = 25, road = 10, water = 30;
    int jobs = 30, jobsFull = 400;  // at jobsFull jobs in the 3 x 3 cells
    int people = 25, peopleFull = 600;
    int air = 60, noise = 45, ground = 50, crime = 50; // at 255
    int industry = 55, industryFull = 200;            // at industryFull industry jobs in the 3 x 3 cells
    int commute = 20, commuteFrom = 15, commuteSpan = 30; // minutes over commuteFrom, full penalty at +commuteSpan
};

// What the host snapshots per cell (main thread, cheap copies of counters and factors).
struct LandCellIn {
    CellKey key{0, 0};
    uint16_t jobs = 0, industry = 0, people = 0, commuteMin = 0;
    uint8_t cov = 0;        // mean service coverage 0..255 (fire, police, health, school, deathcare, garbage, ...)
    uint8_t park = 0, transit = 0, air = 0, noise = 0, ground = 0, crime = 0;
    uint8_t road = 0, water = 0; // 1: a road in the cell / water in view
    uint64_t Sig() const {
        uint64_t h = 1469598103934665603ull;
        const uint32_t v[] = {jobs, industry, people, commuteMin, cov, park, transit, air, noise, ground, crime, road, water};
        for (uint32_t x : v) h = (h ^ x) * 1099511628211ull;
        return h;
    }
};
struct LandInput {
    std::vector<LandCellIn> cells;
    int64_t minute = 0;
};

// The applied state: per cell key its last inputs and value, and the field.
struct LandState {
    std::unordered_map<uint64_t, LandCellIn> in;   // PackKey -> last inputs
    std::unordered_map<uint64_t, uint8_t> value;   // PackKey -> cell value
    Field<uint8_t> field;
    int64_t minute = -1;
    void Clear() { in.clear(), value.clear(), field.Clear(), minute = -1; }
};
struct LandResult {
    std::vector<CellKey> changed;   // cells whose value changed in this pass (the lots in them re-check)
    int recomputed = 0, tiles = 0;  // work done (cells, field cells)
    double mean = 0;                // over the input cells
};

inline int LandTerm(int w, int v, int full) { return full <= 0 ? 0 : w * std::min(v, full) / full; }

// The value of one cell from its own inputs and the 3 x 3 sums.
inline uint8_t LandCellValue(const LandParams& p, const LandCellIn& c, int jobsNear, int peopleNear, int indNear) {
    int v = p.base + p.coverage * c.cov / 255 + p.park * c.park / 255 + p.transit * c.transit / 255;
    v += c.road ? p.road : 0;
    v += c.water ? p.water : 0;
    v += LandTerm(p.jobs, jobsNear, p.jobsFull) + LandTerm(p.people, peopleNear, p.peopleFull);
    v -= (p.air * c.air + p.noise * c.noise + p.ground * c.ground + p.crime * c.crime) / 255;
    v -= LandTerm(p.industry, indNear, p.industryFull);
    v -= LandTerm(p.commute, std::max(0, (int)c.commuteMin - p.commuteFrom), p.commuteSpan);
    return (uint8_t)std::clamp(v, 0, 255);
}

namespace land_detail {
inline uint64_t Key(int x, int z) { return PackKey({x, z}); }
inline bool KeyLess(CellKey a, CellKey b) { return a.z != b.z ? a.z < b.z : a.x < b.x; }
inline void SortKeys(std::vector<CellKey>& v) {
    std::sort(v.begin(), v.end(), KeyLess);
    v.erase(std::unique(v.begin(), v.end(), [](CellKey a, CellKey b) { return a.x == b.x && a.z == b.z; }), v.end());
}
// k and its 8 neighbours that the state knows.
template <class F>
void Around(const LandState& s, CellKey k, F&& f) {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (s.in.count(Key(k.x + dx, k.z + dz))) f(CellKey{k.x + dx, k.z + dz});
}
inline uint8_t ValueOr(const LandState& s, int x, int z, uint8_t own) {
    const auto it = s.value.find(Key(x, z));
    return it == s.value.end() ? own : it->second;
}
// The 16 samples of a cell: bilinear between its centre and the neighbour centres (a missing neighbour = itself).
inline void FillTile(LandState& s, CellKey k) {
    const uint8_t own = ValueOr(s, k.x, k.z, 0);
    int n[3][3]; // the 3 x 3 cell values around k (9 lookups per tile, not 48)
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) n[dz + 1][dx + 1] = (dx || dz) ? ValueOr(s, k.x + dx, k.z + dz, own) : own;
    auto& t = s.field.Cell(k);
    for (int sz = 0; sz < kFieldRes; ++sz)
        for (int sx = 0; sx < kFieldRes; ++sx) {
            const int ox = 2 * sx - 3, oz = 2 * sz - 3; // offset from the centre in 1/8 cell: -3 -1 1 3
            const int dx = ox < 0 ? -1 : 1, dz = oz < 0 ? -1 : 1, wx = ox < 0 ? -ox : ox, wz = oz < 0 ? -oz : oz;
            const int a = own, b = n[1][1 + dx], c = n[1 + dz][1], d = n[1 + dz][1 + dx];
            const int v = ((8 - wx) * (8 - wz) * a + wx * (8 - wz) * b + (8 - wx) * wz * c + wx * wz * d + 32) / 64;
            t[(size_t)(sz * kFieldRes + sx)] = (uint8_t)std::clamp(v, 0, 255);
        }
}
} // namespace land_detail

// One pass: the cells whose inputs changed (and those around them) get a new value; the field follows around the cells
// whose value changed. `force`: recompute every cell (LandFull, and a debug check).
inline void LandStep(LandState& s, const LandInput& in, const LandParams& p, LandResult& out, bool force = false) {
    using namespace land_detail;
    out = LandResult();
    if (force) s.in.reserve(in.cells.size()), s.value.reserve(in.cells.size());
    std::vector<CellKey> dirty;
    for (const LandCellIn& c : in.cells) {
        const uint64_t k = PackKey(c.key);
        const auto it = s.in.find(k);
        if (force || it == s.in.end() || it->second.Sig() != c.Sig()) dirty.push_back(c.key), s.in[k] = c;
    }
    std::vector<CellKey> todo;
    if (force) todo = dirty; // every cell anyway
    else for (CellKey k : dirty) Around(s, k, [&](CellKey n) { todo.push_back(n); });
    SortKeys(todo);
    for (CellKey k : todo) {
        int jobs = 0, people = 0, ind = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = s.in.find(Key(k.x + dx, k.z + dz));
                if (it != s.in.end()) jobs += it->second.jobs, people += it->second.people, ind += it->second.industry;
            }
        const uint8_t v = LandCellValue(p, s.in.find(PackKey(k))->second, jobs, people, ind);
        const auto it = s.value.find(PackKey(k));
        if (force || it == s.value.end() || it->second != v) s.value[PackKey(k)] = v, out.changed.push_back(k);
        ++out.recomputed;
    }
    std::vector<CellKey> tiles;
    if (force) tiles = out.changed;
    else for (CellKey k : out.changed) Around(s, k, [&](CellKey n) { tiles.push_back(n); });
    SortKeys(tiles);
    for (CellKey k : tiles) FillTile(s, k);
    out.tiles = (int)tiles.size();
    double sum = 0;
    for (const LandCellIn& c : in.cells) sum += ValueOr(s, c.key.x, c.key.z, 0);
    out.mean = in.cells.empty() ? 0.0 : sum / (double)in.cells.size();
    s.minute = in.minute;
}

// From scratch (the reference the incremental pass must equal).
inline LandState LandFull(const LandInput& in, const LandParams& p) {
    LandState s;
    LandResult r;
    LandStep(s, in, p, r, true);
    return s;
}
// True when two states give the same value and field for every input cell.
inline bool LandSame(const LandState& a, const LandState& b, const LandInput& in) {
    for (const LandCellIn& c : in.cells) {
        const uint64_t k = PackKey(c.key);
        const auto x = a.value.find(k), y = b.value.find(k);
        if (x == a.value.end() || y == b.value.end() || x->second != y->second) return false;
        const auto* ta = a.field.FindCell(c.key);
        const auto* tb = b.field.FindCell(c.key);
        if (!ta || !tb || *ta != *tb) return false;
    }
    return true;
}

} // namespace city
} // namespace tbd
