// Phase 6: companies (CS2 sim roadmap 4). Header only, headless (tests\sim_goods_companies_test).
//
// One company per industry hub (raw), industrial lot (processor), commercial lot (shop) and office lot. The host (or a
// test) describes the buildings as GoodsSites; CompanyStore::Sync keeps one company per site, keyed by BuildingId, with
// a CompanyId handle (sim_ids.h) that survives a save. A company record is all int32 (no padding: it is saved raw).
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim_goods.h"
#include "sim_ids.h"

namespace tbd {
namespace city {

enum class CompanyKind : uint8_t { Raw = 0, Processor = 1, Shop = 2, Office = 3 };

// A building as the goods sim sees it (filled by the host from the BuildingRegistry, lots, hubs and CellSim counters).
struct GoodsSite {
    uint32_t building = 0;      // BuildingId raw value (the key)
    CompanyKind kind = CompanyKind::Shop;
    uint8_t recipe = 0;         // processor: RecipeId
    Good raw = Good::None;      // raw hub: what it makes
    int32_t x = 0, z = 0;       // metres
    int32_t node = -1;          // road node of its entrance (-1: no road, it cannot ship or receive)
    int32_t cell = -1;          // CitySim cell (-1 none)
    uint16_t jobs = 0, workers = 0;
    uint16_t effPermille = 1000; // power and water (sim_flags.h CellEfficiencyPermille)
    int32_t rawMilliPerHour = 0; // raw hub output at full efficiency
    int32_t localResidents = 0;  // shop: residents of its cell (customers weigh the nearest shops more)
};

// Company bits (CompanyRec::flags); the host maps them onto sim_flags.h's P6 bits.
enum : int32_t { kCoNoGoods = 1, kCoNoCustomers = 2, kCoNoFreight = 4 };

struct CompanyRec {
    // from the site (refreshed every sync)
    int32_t building = 0, kind = 0, recipe = 0, rawGood = -1, x = 0, z = 0, node = -1, cell = -1;
    int32_t jobs = 0, workers = 0, eff = 1000, rawMilli = 0, localRes = 0;
    // state
    int32_t frac = 0, frac2 = 0;        // milli-units made or sold but not yet whole
    int32_t cash = 0, profitToday = 0, profitLast = 0;
    int32_t lossDays = 0, closedDays = 0; // closedDays > 0: bankrupt, the lot waits for a new company
    int32_t flags = 0, demandMilli = 0;  // shop: its customers' spending per hour (milli-units), set by the match pass
    int32_t madeToday = 0, soldToday = 0, madeLast = 0, soldLast = 0;
    std::array<int32_t, kGoodCount> stock{}, incoming{};
};
static_assert(sizeof(CompanyRec) == (26 + 2 * kGoodCount) * 4, "CompanyRec must stay padding free (saved raw)");

inline Good CompanyOutput(const CompanyRec& c) {
    if (c.kind == (int)CompanyKind::Raw) return c.rawGood >= 0 ? (Good)c.rawGood : Good::None;
    if (c.kind == (int)CompanyKind::Processor) return RecipeOf(c.recipe).out;
    return Good::None;
}
// The goods a company buys (count returned, up to kMaxInputs).
inline int CompanyInputs(const CompanyRec& c, Good out[kMaxInputs]) {
    if (c.kind == (int)CompanyKind::Processor) {
        const Recipe& r = RecipeOf(c.recipe);
        for (int i = 0; i < r.inputs; ++i) out[i] = r.in[i];
        return r.inputs;
    }
    if (c.kind == (int)CompanyKind::Shop) { out[0] = kRetailGoods[0], out[1] = kRetailGoods[1]; return 2; }
    return 0;
}
// Full-rate work per hour in milli-units: workers x rate x efficiency (a raw hub: its resource rate x efficiency).
inline int32_t CompanyRateMilli(const CompanyRec& c, const GoodsParams& p) {
    if (c.kind == (int)CompanyKind::Raw) return (int32_t)((int64_t)c.rawMilli * c.eff / 1000);
    return (int32_t)((int64_t)c.workers * p.workerMilliPerHour * c.eff / 1000);
}
// Daily fixed cost (rent and upkeep): owed with or without workers.
inline int32_t CompanyUpkeep(const CompanyRec& c) { return c.kind == (int)CompanyKind::Raw ? 40 : 30 + c.jobs * 4; }

class CompanyStore {
public:
    HandlePool pool;
    std::vector<CompanyRec> recs; // by CompanyId::Index(); slot 0 unused
    std::vector<CompanyId> opened, removed; // since the last TakeChanges (events)

    CompanyId Find(uint32_t building) const {
        const auto it = byBuilding_.find(building);
        return it == byBuilding_.end() ? CompanyId() : CompanyId(it->second);
    }
    bool Alive(CompanyId c) const { return pool.Alive(c); }
    CompanyRec* Get(CompanyId c) { return pool.Alive(c) ? &recs[c.Index()] : nullptr; }
    const CompanyRec* Get(CompanyId c) const { return pool.Alive(c) ? &recs[c.Index()] : nullptr; }
    uint32_t Capacity() const { return pool.Capacity(); }
    CompanyId At(uint32_t i) const { return CompanyId(pool.At(i)); }
    void Clear() { pool.Clear(), recs.clear(), byBuilding_.clear(), opened.clear(), removed.clear(), seen_.clear(), hint_.clear(); }

    // After a load: the building index from the records.
    void Reindex() {
        byBuilding_.clear();
        recs.resize(pool.Capacity());
        for (uint32_t i = 1; i < recs.size(); ++i)
            if (pool.At(i).Valid()) byBuilding_[(uint32_t)recs[i].building] = pool.At(i).v;
    }

    // One company per site; a site seen for the first time opens one, a company whose site is gone closes. The stock of a
    // closed company is lost (added to lost[good]: the mass ledger counts it). Sites are visited in the given order.
    void Sync(const std::vector<GoodsSite>& sites, const GoodsParams& p, std::array<int64_t, kGoodCount>& lost) {
        ++epoch_;
        if (hint_.size() != sites.size()) hint_.resize(sites.size());
        for (size_t si = 0; si < sites.size(); ++si) {
            const GoodsSite& s = sites[si];
            if (!s.building) continue;
            // the company of this site position at the last sync, if it still is this building's (no hash lookup)
            const CompanyId h = hint_[si];
            CompanyId id = h.Valid() && pool.Alive(h) && h.Index() < recs.size() && (uint32_t)recs[h.Index()].building == s.building ? h : Find(s.building);
            if (!id.Valid()) {
                id = CompanyId(pool.Alloc());
                if (!id.Valid()) continue;
                if (recs.size() < pool.Capacity()) recs.resize(pool.Capacity());
                recs[id.Index()] = CompanyRec();
                recs[id.Index()].cash = p.startCash;
                byBuilding_[s.building] = id.v;
                opened.push_back(id);
            }
            hint_[si] = id;
            CompanyRec& c = recs[id.Index()];
            c.building = (int32_t)s.building, c.kind = (int32_t)s.kind, c.recipe = s.recipe, c.rawGood = s.raw == Good::None ? -1 : (int32_t)s.raw;
            c.x = s.x, c.z = s.z, c.node = s.node, c.cell = s.cell;
            c.jobs = s.jobs, c.workers = s.workers > s.jobs ? s.jobs : s.workers, c.eff = s.effPermille, c.rawMilli = s.rawMilliPerHour;
            c.localRes = s.localResidents;
            if (seen_.size() < recs.size()) seen_.resize(recs.size(), 0);
            seen_[id.Index()] = epoch_;
        }
        if (seen_.size() < recs.size()) seen_.resize(recs.size(), 0);
        for (uint32_t i = 1; i < recs.size(); ++i) {
            const CompanyId id = At(i);
            if (!id.Valid() || seen_[i] == epoch_) continue;
            for (int g = 0; g < kGoodCount; ++g) lost[(size_t)g] += recs[i].stock[(size_t)g];
            byBuilding_.erase((uint32_t)recs[i].building);
            removed.push_back(id);
            pool.Free(id);
            recs[i] = CompanyRec();
        }
    }
    // Live company indices in increasing order (the deterministic visiting order).
    template <typename F>
    void ForEach(F&& f) {
        for (uint32_t i = 1; i < recs.size(); ++i)
            if (pool.At(i).Valid()) f(i, recs[i]);
    }

private:
    std::unordered_map<uint32_t, uint32_t> byBuilding_; // BuildingId raw -> CompanyId raw
    std::vector<CompanyId> hint_; // per position in the last Sync's sites: its company (checked before use)
    std::vector<uint32_t> seen_;
    uint32_t epoch_ = 0;
};

} // namespace city
} // namespace tbd
