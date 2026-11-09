// Phase 5 .tbdsim payloads (sim_save.h chunks LINE MODE OUTC). Header only, headless (tests\sim_transit_test).
//   MODE  the transit world: zones (with what their links were evaluated with), the lines the paths refer to, every
//         link's shares and paths, fare and taxis. The totals and the ledger are derived again (RebuildAfterLoad).
//   LINE  the line book: vehicle settings, yesterday per line, riders per day, money totals
//   OUTC  the outside connections' yesterday and running totals
// Every field is written one by one (no struct padding in the file), so equal state gives equal bytes.
#pragma once

#include "sim_save.h"
#include "sim_transit_lines.h"
#include "sim_transit_outside.h"
#include "sim_transit_world.h"

namespace tbd {
namespace city {

constexpr uint16_t kTransitModeVersion = 1, kTransitLineVersion = 1, kTransitOutcVersion = 1;

inline void WritePath(ByteWriter& w, const TransitPath& p) {
    w.F32(p.minutes), w.U8(p.legs);
    for (int k = 0; k < 2; ++k) w.Put<int16_t>(p.line[k]), w.Put<int16_t>(p.on[k]), w.Put<int16_t>(p.off[k]);
}
inline void ReadPath(ByteReader& r, TransitPath& p) {
    p.minutes = r.F32(), p.legs = r.U8();
    for (int k = 0; k < 2; ++k) p.line[k] = r.Get<int16_t>(), p.on[k] = r.Get<int16_t>(), p.off[k] = r.Get<int16_t>();
}

inline void WriteTransitMode(ByteWriter& w, const TransitSim& s) {
    w.F32(s.p.fare), w.F32(s.p.fareMul), w.I32(s.p.taxis);
    w.U32((uint32_t)s.zones.size());
    for (const TransitZone& z : s.zones) {
        w.U64(z.key), w.F32(z.x), w.F32(z.z), w.U32(z.pop), w.U32(z.jobs), w.F32(z.carOwn), w.U32(z.evalPop);
        for (int k = 0; k < 4; ++k) w.F32(z.wealth[k]), w.F32(z.evalWealth[k]);
        w.U32(z.accessSig), w.U8(z.parking);
    }
    w.U32((uint32_t)s.lineIn.size());
    for (const LineInput& l : s.lineIn) {
        w.U64(l.id), w.U8(l.mode), w.U8(l.closed ? 1 : 0), w.I32(l.vehicles);
        w.PutVec(l.x), w.PutVec(l.z), w.PutVec(l.legLen);
    }
    w.U32((uint32_t)s.links.size());
    for (const TransitLink& l : s.links) {
        w.I32(l.from), w.I32(l.to), w.I32(l.n), w.U8(l.evaluated);
        for (int m = 0; m < kModeCount; ++m) w.U16(l.share[m]);
        WritePath(w, l.out), WritePath(w, l.back);
    }
}

inline bool ReadTransitMode(ByteReader& r, TransitSim& s) {
    s.Clear();
    s.p.fare = r.F32(), s.p.fareMul = r.F32(), s.p.taxis = r.I32();
    const uint32_t nz = r.U32();
    if (!r.ok || nz > r.Remaining() / 40) return false;
    s.zones.resize(nz);
    for (TransitZone& z : s.zones) {
        z.key = r.U64(), z.x = r.F32(), z.z = r.F32(), z.pop = r.U32(), z.jobs = r.U32(), z.carOwn = r.F32(), z.evalPop = r.U32();
        for (int k = 0; k < 4; ++k) z.wealth[k] = r.F32(), z.evalWealth[k] = r.F32();
        z.accessSig = r.U32(), z.parking = (uint8_t)std::min<int>(3, r.U8());
    }
    const uint32_t nl = r.U32();
    if (!r.ok || nl > r.Remaining() / 14) return false;
    s.lineIn.resize(nl);
    for (LineInput& l : s.lineIn) {
        l.id = r.U64(), l.mode = r.U8(), l.closed = r.U8() != 0, l.vehicles = r.I32();
        r.GetVec(l.x), r.GetVec(l.z), r.GetVec(l.legLen);
    }
    const uint32_t nk = r.U32();
    if (!r.ok || nk > r.Remaining() / 21) return false;
    s.links.resize(nk);
    for (TransitLink& l : s.links) {
        l.from = r.I32(), l.to = r.I32(), l.n = r.I32(), l.evaluated = r.U8();
        for (int m = 0; m < kModeCount; ++m) l.share[m] = r.U16();
        ReadPath(r, l.out), ReadPath(r, l.back);
    }
    if (!r.ok) return false;
    for (size_t i = 1; i < s.links.size(); ++i) { // sorted and in range, or it is not ours
        const TransitLink &a = s.links[i - 1], &b = s.links[i];
        if (a.from > b.from || (a.from == b.from && a.to >= b.to)) return false;
    }
    s.RebuildAfterLoad();
    return true;
}

inline void WriteTransitLine(ByteWriter& w, const LineBook& b) {
    w.U32((uint32_t)b.settings.size());
    for (const LineSetting& s : b.settings) w.U64(s.id), w.I32(s.vehicles);
    w.U32((uint32_t)b.last.size());
    for (const LineDay& d : b.last) w.U64(d.id), w.I32(d.mode), w.I32(d.vehicles), w.I64(d.riders), w.I64(d.served), w.I64(d.fares), w.I64(d.upkeep);
    w.PutVec(b.ridersDays), w.I64(b.faresTotal), w.I64(b.upkeepTotal);
}
inline bool ReadTransitLine(ByteReader& r, LineBook& b) {
    b.Clear();
    const uint32_t ns = r.U32();
    if (!r.ok || ns > r.Remaining() / 12) return false;
    b.settings.resize(ns);
    for (LineSetting& s : b.settings) s.id = r.U64(), s.vehicles = r.I32();
    const uint32_t nd = r.U32();
    if (!r.ok || nd > r.Remaining() / 48) return false;
    b.last.resize(nd);
    for (LineDay& d : b.last) d.id = r.U64(), d.mode = r.I32(), d.vehicles = r.I32(), d.riders = r.I64(), d.served = r.I64(), d.fares = r.I64(), d.upkeep = r.I64();
    r.GetVec(b.ridersDays), b.faresTotal = r.I64(), b.upkeepTotal = r.I64();
    return r.ok;
}

inline void WriteTransitOutc(ByteWriter& w, const OutsideWorld& o) {
    w.U32((uint32_t)o.flows.size());
    for (const OutsideFlow& f : o.flows) {
        w.U64(f.id), w.U8(f.kind), w.F32(f.x), w.F32(f.z), w.I32(f.tourists), w.I32(f.immigrants), w.I32(f.emigrants);
        w.I64(f.cargoIn), w.I64(f.cargoOut), w.I64(f.cargoCap);
    }
    w.PutVec(o.touristDays), w.I64(o.touristsTotal), w.I64(o.immigrantsTotal), w.I64(o.emigrantsTotal), w.I64(o.cargoTotal);
}
inline bool ReadTransitOutc(ByteReader& r, OutsideWorld& o) {
    o.Clear();
    const uint32_t n = r.U32();
    if (!r.ok || n > r.Remaining() / 45) return false;
    o.flows.resize(n);
    for (OutsideFlow& f : o.flows) {
        f.id = r.U64(), f.kind = r.U8(), f.x = r.F32(), f.z = r.F32(), f.tourists = r.I32(), f.immigrants = r.I32(), f.emigrants = r.I32();
        f.cargoIn = r.I64(), f.cargoOut = r.I64(), f.cargoCap = r.I64();
    }
    r.GetVec(o.touristDays), o.touristsTotal = r.I64(), o.immigrantsTotal = r.I64(), o.emigrantsTotal = r.I64(), o.cargoTotal = r.I64();
    return r.ok;
}

} // namespace city
} // namespace tbd
