// Phase 6's .tbdsim chunks (CS2 sim roadmap 3.4): COMP companies, GOOD stocks in flight and the running ledger, TRAD the
// trade balance history and yesterday's trucks. Header only, headless (tests\sim_goods_test round trip).
// Each Read* returns false on a short or inconsistent payload: the registry then starts that chunk fresh.
#pragma once

#include <algorithm>
#include <vector>

#include "sim_goods_world.h"
#include "sim_save.h"

namespace tbd {
namespace city {

constexpr uint16_t kGoodsCompVersion = 1, kGoodsGoodVersion = 1, kGoodsTradVersion = 1;

inline void WriteLedger(ByteWriter& w, const GoodsLedger& l) {
    for (const auto* a : {&l.produced, &l.used, &l.consumed, &l.imported, &l.exported, &l.lost})
        for (int64_t v : *a) w.I64(v);
    w.I64(l.importCost), w.I64(l.exportIncome), w.I64(l.trucks);
}
inline void ReadLedger(ByteReader& r, GoodsLedger& l) {
    for (auto* a : {&l.produced, &l.used, &l.consumed, &l.imported, &l.exported, &l.lost})
        for (int64_t& v : *a) v = r.I64();
    l.importCost = r.I64(), l.exportIncome = r.I64(), l.trucks = r.I64();
}

struct GoodsSaveAccess { // the world's private truck counters
    static std::map<uint64_t, int32_t>& Trucks(GoodsWorld& w) { return w.trucksToday_; }
    static void SetLast(GoodsWorld& w, std::vector<TruckLink> t) { w.trucksLast_ = std::move(t), w.trucksDone_.clear(), w.trucksPending_ = false; }
};

// COMP: the company handle pool and the records.
inline void WriteGoodsComp(ByteWriter& w, const GoodsWorld& g) {
    const HandlePool& p = g.cos.pool;
    const size_t cap = p.Capacity(), have = std::min(cap, g.cos.recs.size());
    w.bytes.reserve(w.bytes.size() + 3 * 4 + cap * 6 + cap * sizeof(CompanyRec) + 8); // one allocation (the autosave copy)
    w.PutVec(p.Gens()), w.PutVec(p.AliveBits()), w.PutVec(p.FreeList());
    // the records as PutVec writes them, resized to the pool (no copy of the vector first): the same bytes
    w.U32((uint32_t)cap);
    if (have) w.Raw(g.cos.recs.data(), have * sizeof(CompanyRec));
    for (size_t i = have; i < cap; ++i) { const CompanyRec empty; w.Raw(&empty, sizeof(CompanyRec)); }
    w.I32(g.bankruptTotal);
}
inline bool ReadGoodsComp(ByteReader& r, GoodsWorld& g) {
    std::vector<uint8_t> gens, alive;
    std::vector<uint32_t> freeList;
    std::vector<CompanyRec> recs;
    r.GetVec(gens), r.GetVec(alive), r.GetVec(freeList), r.GetVec(recs);
    const int32_t bankrupt = r.I32();
    if (!r.ok || recs.size() != gens.size()) return false;
    HandlePool pool;
    if (!pool.SetRaw(std::move(gens), std::move(alive), std::move(freeList))) return false;
    g.cos.Clear();
    g.cos.pool = std::move(pool), g.cos.recs = std::move(recs);
    g.cos.Reindex();
    g.bankruptTotal = bankrupt;
    return true;
}

// GOOD: shipments in flight, today's and the running ledger, today's trucks per node pair.
inline void WriteGoodsGood(ByteWriter& w, GoodsWorld& g) {
    w.PutVec(g.ships);
    WriteLedger(w, g.today), WriteLedger(w, g.total);
    std::vector<std::pair<uint64_t, int32_t>> t;
    for (const auto& [k, n] : GoodsSaveAccess::Trucks(g)) if (n) t.emplace_back(k, n); // key order: the same bytes on every run
    w.U32((uint32_t)t.size());
    for (const auto& [k, n] : t) w.U64(k), w.I32(n);
}
inline bool ReadGoodsGood(ByteReader& r, GoodsWorld& g) {
    std::vector<Shipment> ships;
    r.GetVec(ships);
    GoodsLedger today, total;
    ReadLedger(r, today), ReadLedger(r, total);
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 12 > r.Remaining()) return false;
    auto& trucks = GoodsSaveAccess::Trucks(g);
    trucks.clear();
    for (uint32_t i = 0; i < n; ++i) { const uint64_t k = r.U64(); trucks[k] = r.I32(); }
    for (const Shipment& s : ships)
        if (s.good < 0 || s.good >= kGoodCount || s.units < 0) return false;
    if (!r.ok) return false;
    g.ships = std::move(ships), g.today = today, g.total = total;
    return true;
}

// TRAD: the daily trade balance, yesterday's ledger and trucks.
inline void WriteGoodsTrad(ByteWriter& w, GoodsWorld& g) {
    w.PutVec(g.balance);
    WriteLedger(w, g.last);
    const std::vector<TruckLink>& last = g.TrucksLast();
    w.U32((uint32_t)last.size());
    for (const TruckLink& t : last) w.I32(t.fromNode), w.I32(t.toNode), w.I32(t.trucks);
}
inline bool ReadGoodsTrad(ByteReader& r, GoodsWorld& g) {
    std::vector<int32_t> balance;
    r.GetVec(balance);
    GoodsLedger last;
    ReadLedger(r, last);
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 12 > r.Remaining()) return false;
    std::vector<TruckLink> trucks(n);
    for (TruckLink& t : trucks) t.fromNode = r.I32(), t.toNode = r.I32(), t.trucks = r.I32();
    if (!r.ok) return false;
    g.balance = std::move(balance), g.last = last;
    GoodsSaveAccess::SetLast(g, std::move(trucks));
    return true;
}

} // namespace city
} // namespace tbd
