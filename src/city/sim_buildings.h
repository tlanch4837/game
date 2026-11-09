// BuildingRegistry (CS2 sim roadmap 3.2, Phase 0; frozen interface). Header only, headless (tests\sim_save_test).
//
// One row per sim-relevant building of any origin: a zoned lot (Lot::id), a transport or service building (Placed::id),
// a utility source (UtilBuilding::id) or an industry hub (IndSite::id). Rows are keyed by (origin, originId), never by a
// vector position, so a BuildingId survives a lots compaction and a save / load (the BLDG chunk). The host rebuilds it
// when the zone, transport, utility or industry versions change: BeginSync, Upsert every live building, EndSync. Rows not
// upserted in a sync are freed (their handle's generation is bumped) unless they are reserved from a load and not yet
// claimed (the lots come back a few frames after the load; ReleaseUnclaimed drops what never came back).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_ids.h"

namespace tbd {
namespace city {

enum class BOrigin : uint8_t { None = 0, Lot = 1, Transport = 2, Utility = 3, Industry = 4 };
constexpr int kEduLevels = 5; // 0 none, 1 elementary, 2 high school, 3 college, 4 university (CS2's levels)

struct BuildingRow {
    BOrigin origin = BOrigin::None;
    uint64_t originId = 0;
    int32_t kind = -1;          // Lot: zone type; Transport: BuildingDef index; Utility: UtilDef index; Industry: IndKind
    int32_t cellX = 0, cellZ = 0; // the CityCell (city_cells.h CellKey) the building's centre lies in
    int32_t cell = -1;          // CitySim cell index (or -1)
    int32_t hood = -1;          // sim_hoods.h hood id (or -1)
    int32_t entranceNode = -1;  // road network node nearest the entrance (or -1)
    uint8_t level = 1;
    uint8_t alive = 0;          // 1 live, 0 freed, 2 reserved from a load and not yet claimed
    uint16_t homes = 0;
    uint16_t jobSlots[kEduLevels] = {};
};

// The default split of a workplace's jobs over education levels (CS2-like: higher levels and offices want more schooling).
// Phase 1 reads jobSlots; Phase 6 may refine it per company. cat: 0 residential, 1 commercial, 2 industrial, 3 office,
// -1 a service building.
inline void SplitJobSlots(uint32_t jobs, int cat, int level, uint16_t out[kEduLevels]) {
    static const uint8_t kShare[4][kEduLevels] = {{40, 35, 15, 8, 2}, {20, 35, 30, 10, 5}, {5, 20, 35, 25, 15}, {10, 20, 30, 25, 15}};
    const int row = cat == 2 ? 0 : cat == 1 ? 1 : cat == 3 ? 2 : 3;
    int shift = level >= 3 ? 1 : 0; // a high-level workplace moves a share up one level
    uint32_t used = 0;
    for (int e = 0; e < kEduLevels; ++e) {
        uint32_t s = kShare[row][e];
        if (shift && e > 0) s = (s + kShare[row][e - 1] / 4u);
        if (shift && e < kEduLevels - 1) s -= s / 4u;
        out[e] = (uint16_t)(jobs * s / 100u);
        used += out[e];
    }
    out[1] = (uint16_t)(out[1] + (jobs > used ? jobs - used : 0)); // remainder to elementary
}

class BuildingRegistry {
public:
    std::vector<BuildingRow> rows; // indexed by BuildingId::Index(); row 0 unused
    uint64_t version = 0;          // bumps on every EndSync that changed anything

    static uint64_t Key(BOrigin o, uint64_t id) { return (uint64_t)o << 56 | (id & 0x00FFFFFFFFFFFFFFull); }

    BuildingId Find(BOrigin o, uint64_t originId) const {
        const auto it = index_.find(Key(o, originId));
        return it == index_.end() ? BuildingId() : BuildingId(it->second);
    }
    bool Alive(BuildingId b) const { return pool_.Alive(b) && rows[b.Index()].alive == 1; }
    const BuildingRow* Get(BuildingId b) const { return pool_.Alive(b) ? &rows[b.Index()] : nullptr; }
    BuildingRow* Get(BuildingId b) { return pool_.Alive(b) ? &rows[b.Index()] : nullptr; }
    uint32_t Capacity() const { return pool_.Capacity(); }
    uint32_t Live() const { return pool_.Live(); }
    BuildingId At(uint32_t index) const { return BuildingId(pool_.At(index)); }

    void BeginSync() { ++epoch_; if (seen_.size() < rows.size()) seen_.resize(rows.size(), 0); changed_ = false; }
    // Adds or refreshes a building; returns its id. The row's origin and originId are set from the arguments.
    BuildingId Upsert(BOrigin o, uint64_t originId, const BuildingRow& data) {
        BuildingId b = Find(o, originId);
        if (!b.Valid()) {
            b = BuildingId(pool_.Alloc());
            if (!b.Valid()) return b;
            if (rows.size() < pool_.Capacity()) rows.resize(pool_.Capacity()), seen_.resize(pool_.Capacity(), 0);
            index_[Key(o, originId)] = b.v;
            changed_ = true;
        }
        BuildingRow& r = rows[b.Index()];
        BuildingRow next = data;
        next.origin = o, next.originId = originId, next.alive = 1;
        if (!Same(r, next)) r = next, changed_ = true;
        seen_[b.Index()] = epoch_;
        return b;
    }
    // The same with a per-source hint (the host keeps one BuildingId per entry of its source list): while the hint still
    // names this (origin, originId) the hash lookup is skipped, so a sync of an unchanged city is one linear pass. The
    // result is exactly Upsert's (a stale or empty hint falls back to it and is refreshed).
    BuildingId Upsert(BOrigin o, uint64_t originId, const BuildingRow& data, BuildingId& hint) {
        const uint32_t i = hint.Index();
        if (hint.Valid() && pool_.Alive(hint) && i < rows.size() && rows[i].originId == originId && rows[i].origin == o && rows[i].alive) {
            BuildingRow& r = rows[i];
            BuildingRow next = data;
            next.origin = o, next.originId = originId, next.alive = 1;
            if (!Same(r, next)) r = next, changed_ = true;
            seen_[i] = epoch_;
            return hint;
        }
        return hint = Upsert(o, originId, data);
    }
    // The check of the incremental path: every live or reserved row is in the index under its own key and the index
    // holds nothing else (a full recompute of what the hints rely on). True when consistent.
    bool CheckIndex() const {
        size_t n = 0;
        for (uint32_t i = 1; i < rows.size(); ++i) {
            if (!rows[i].alive) continue;
            ++n;
            const auto it = index_.find(Key(rows[i].origin, rows[i].originId));
            if (it == index_.end() || it->second != pool_.At(i).v) return false;
        }
        return n == index_.size();
    }
    // Frees every live row not upserted since BeginSync. Reserved rows stay until claimed or released.
    void EndSync() {
        for (uint32_t i = 1; i < rows.size(); ++i)
            if (rows[i].alive == 1 && seen_[i] != epoch_) FreeRow(i);
        if (changed_) ++version;
    }
    // After a load's buildings are all back: reserved rows nobody claimed are freed.
    void ReleaseUnclaimed() {
        bool any = false;
        for (uint32_t i = 1; i < rows.size(); ++i)
            if (rows[i].alive == 2) FreeRow(i), any = true;
        if (any) ++version;
    }
    void Clear() { rows.clear(), index_.clear(), seen_.clear(), pool_.Clear(), ++version; }

    // Save (BLDG): the pool's raw state and each row's (origin, originId). Load: Restore puts the same handles back as
    // reserved rows; the next syncs claim them by key.
    const HandlePool& Pool() const { return pool_; }
    bool Restore(const HandlePool& pool, const std::vector<BuildingRow>& saved) {
        Clear();
        if (saved.size() != pool.Capacity()) return false;
        if (!pool_.SetRaw(pool.Gens(), pool.AliveBits(), pool.FreeList())) return false;
        rows = saved, seen_.assign(rows.size(), 0);
        for (uint32_t i = 1; i < rows.size(); ++i) {
            if (!pool_.At(i).Valid()) { rows[i] = BuildingRow(); continue; }
            rows[i].alive = 2;
            index_[Key(rows[i].origin, rows[i].originId)] = pool_.At(i).v;
        }
        return true;
    }

private:
    static bool Same(const BuildingRow& a, const BuildingRow& b) {
        if (a.origin != b.origin || a.originId != b.originId || a.kind != b.kind || a.cellX != b.cellX || a.cellZ != b.cellZ || a.cell != b.cell ||
            a.hood != b.hood || a.entranceNode != b.entranceNode || a.level != b.level || a.alive != b.alive || a.homes != b.homes)
            return false;
        for (int e = 0; e < kEduLevels; ++e) if (a.jobSlots[e] != b.jobSlots[e]) return false;
        return true;
    }
    void FreeRow(uint32_t i) {
        index_.erase(Key(rows[i].origin, rows[i].originId));
        pool_.Free(pool_.At(i));
        rows[i] = BuildingRow();
        changed_ = true;
    }
    HandlePool pool_;
    std::unordered_map<uint64_t, uint32_t> index_;
    std::vector<uint32_t> seen_;
    uint32_t epoch_ = 0;
    bool changed_ = false;
};

} // namespace city
} // namespace tbd
