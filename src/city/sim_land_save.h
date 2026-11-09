// Phase 2's .tbdsim chunks (CS2 sim roadmap 3.4): GROW (lot levelling states), LAND (land value inputs, values and
// field, plus a pass in flight), DMND (demand bars, threshold zones, the day's rent affordability). Header only, headless
// (tests\sim_land_save_test). Field by field (no struct padding in the file): the same state writes the same bytes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_demand.h"
#include "sim_land.h"
#include "sim_land_lots.h"
#include "sim_land_rent.h"
#include "sim_save.h"

namespace tbd {
namespace city {

constexpr uint16_t kLandGrowVersion = 1, kLandLandVersion = 1, kLandDemandVersion = 1;

inline void WriteLots(ByteWriter& w, const LotLevels& L) {
    std::vector<uint64_t> ids;
    for (const auto& kv : L.st) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    w.U32((uint32_t)ids.size());
    for (uint64_t id : ids) {
        const LotState& s = L.st.at(id);
        w.U64(s.id), w.I32(s.cellX), w.I32(s.cellZ);
        w.U8(s.cond), w.U8(s.level), w.U8(s.abandoned), w.U8(s.built), w.U8(s.dirty), w.U8(s.maxed), w.U8(s.cat);
        w.I64(s.since), w.I64(s.due), w.I64(s.abandonedAt), w.U32(s.seen);
    }
    w.U32((uint32_t)L.dirty.size()); // mark order matters (it is the evaluation order)
    for (uint64_t id : L.dirty) w.U64(id);
    w.U64(L.levelUps), w.U64(L.levelDowns), w.U64(L.abandonedTotal), w.U64(L.recovered), w.U64(L.demolished);
    w.I64(L.hour), w.I32(L.actionsThisHour), w.I32(L.sweepGroup), w.U32(L.cycle), w.I32(L.seenNow);
}
inline bool ReadLots(ByteReader& r, LotLevels& L) {
    L.Clear();
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 50 > r.Remaining()) return false;
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        LotState s;
        s.id = r.U64(), s.cellX = r.I32(), s.cellZ = r.I32();
        s.cond = r.U8(), s.level = r.U8(), s.abandoned = r.U8(), s.built = r.U8(), s.dirty = r.U8(), s.maxed = r.U8(), s.cat = r.U8();
        s.since = r.I64(), s.due = r.I64(), s.abandonedAt = r.I64(), s.seen = r.U32();
        L.st[s.id] = s;
    }
    const uint32_t d = r.U32();
    if (!r.ok || (uint64_t)d * 8 > r.Remaining()) return false;
    std::vector<uint64_t> dirty(d);
    for (uint32_t i = 0; i < d; ++i) dirty[i] = r.U64();
    L.levelUps = r.U64(), L.levelDowns = r.U64(), L.abandonedTotal = r.U64(), L.recovered = r.U64(), L.demolished = r.U64();
    L.hour = r.I64(), L.actionsThisHour = r.I32(), L.sweepGroup = r.I32(), L.cycle = r.U32(), L.seenNow = r.I32();
    if (!r.ok) return false;
    L.Rebuild(); // queue and cell index from the states; then the saved mark order
    L.dirty.clear();
    for (uint64_t id : dirty) if (L.st.count(id)) L.dirty.push_back(id);
    return true;
}

inline void WriteLandCell(ByteWriter& w, const LandCellIn& c) {
    w.I32(c.key.x), w.I32(c.key.z), w.U16(c.jobs), w.U16(c.industry), w.U16(c.people), w.U16(c.commuteMin);
    const uint8_t b[] = {c.cov, c.park, c.transit, c.air, c.noise, c.ground, c.crime, c.road, c.water};
    w.Raw(b, sizeof(b));
}
inline LandCellIn ReadLandCell(ByteReader& r) {
    LandCellIn c;
    c.key.x = r.I32(), c.key.z = r.I32(), c.jobs = r.U16(), c.industry = r.U16(), c.people = r.U16(), c.commuteMin = r.U16();
    uint8_t b[9] = {};
    r.Raw(b, sizeof(b));
    c.cov = b[0], c.park = b[1], c.transit = b[2], c.air = b[3], c.noise = b[4], c.ground = b[5], c.crime = b[6], c.road = b[7], c.water = b[8];
    return c;
}
// The state in key order (z, x): inputs, value, field tile.
inline void WriteLandState(ByteWriter& w, const LandState& s) {
    std::vector<CellKey> keys;
    for (const auto& kv : s.in) keys.push_back(kv.second.key);
    land_detail::SortKeys(keys);
    w.I64(s.minute), w.U32((uint32_t)keys.size());
    for (CellKey k : keys) {
        WriteLandCell(w, s.in.at(PackKey(k)));
        const auto v = s.value.find(PackKey(k));
        w.U8(v == s.value.end() ? 0 : v->second);
        const auto* t = s.field.FindCell(k);
        w.U8(t ? 1 : 0);
        if (t) w.Raw(t->data(), t->size());
    }
}
inline bool ReadLandState(ByteReader& r, LandState& s) {
    s.Clear();
    s.minute = r.I64();
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 27 > r.Remaining()) return false;
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        const LandCellIn c = ReadLandCell(r);
        s.in[PackKey(c.key)] = c;
        s.value[PackKey(c.key)] = r.U8();
        if (r.U8()) r.Raw(s.field.Cell(c.key).data(), kFieldRes * kFieldRes);
    }
    return r.ok;
}

// DMND: the smoothed bars, each category's threshold zone (lot re-checks fire when it changes), the rent day.
struct DemandSaveState {
    double bars[kDemandCats] = {};
    int8_t zone[kDemandCats] = {-2, -2, -2, -2}; // -2 unknown, -1 under badDemand, 0 between, 1 above 0
    RentDay rent;
};
inline void WriteAfford(ByteWriter& w, const Affordability& a) {
    for (int i = 0; i < 4; ++i) w.I32(a.households[i]), w.I32(a.burdened[i]);
    w.I64(a.spend), w.I64(a.income), w.I64(a.persons);
}
inline void ReadAfford(ByteReader& r, Affordability& a) {
    for (int i = 0; i < 4; ++i) a.households[i] = r.I32(), a.burdened[i] = r.I32();
    a.spend = r.I64(), a.income = r.I64(), a.persons = r.I64();
}
inline void WriteDemand(ByteWriter& w, const DemandSaveState& d) {
    for (int c = 0; c < kDemandCats; ++c) w.F64(d.bars[c]), w.Put<int8_t>(d.zone[c]);
    WriteAfford(w, d.rent.cur), WriteAfford(w, d.rent.last);
    w.I32(d.rent.group), w.PutVec(d.rent.seen), w.PutVec(d.rent.hit);
}
inline bool ReadDemand(ByteReader& r, DemandSaveState& d) {
    d = DemandSaveState();
    for (int c = 0; c < kDemandCats; ++c) d.bars[c] = std::clamp(r.F64(), -1.0, 1.0), d.zone[c] = r.Get<int8_t>();
    ReadAfford(r, d.rent.cur), ReadAfford(r, d.rent.last);
    d.rent.group = r.I32();
    r.GetVec(d.rent.seen), r.GetVec(d.rent.hit);
    return r.ok && d.rent.seen.size() == d.rent.hit.size() && d.rent.group >= 0 && d.rent.group < 96;
}

} // namespace city
} // namespace tbd
