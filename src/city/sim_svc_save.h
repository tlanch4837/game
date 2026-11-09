// Phase 4 .tbdsim chunks (CS2 sim roadmap 3.4). Header only, headless (tests\sim_svc_save_test).
//   SERV v1  sites (id, busy, used, graves, handled), seats taken, swept, rubble timers, crime pressure, today's stats
//   INCD v1  next id, the incident slots field by field, the free list (reuse order)
//   GARB v1  piles (lot id, tonnes), last visit per lot, truckloads in the making, facility buckets, last refill
// Every map is written sorted by key, so the same state gives the same bytes. The coverage pass is not saved: the host
// recomputes it right after the load (its inputs are the city itself); incidents keep their site ids.
#pragma once

#include <algorithm>

#include "sim_save.h"
#include "sim_svc_education.h"
#include "sim_svc_garbage.h"

namespace tbd {
namespace city {

constexpr uint16_t kSvcServVersion = 1, kSvcIncdVersion = 1, kSvcGarbVersion = 1;

template <class K, class V, class F>
inline void SvcPutMap(ByteWriter& w, const std::unordered_map<K, V>& m, F&& put) {
    std::vector<K> keys;
    keys.reserve(m.size());
    for (const auto& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    w.U32((uint32_t)keys.size());
    for (const K& k : keys) put(k, m.at(k));
}
template <class F>
inline bool SvcGetCount(ByteReader& r, size_t minBytes, F&& get) {
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * minBytes > r.Remaining()) return false;
    for (uint32_t i = 0; i < n && r.ok; ++i) get();
    return r.ok;
}

inline void SvcPutStats(ByteWriter& w, const SvcDayStats& s) {
    for (int t = 0; t < kIncTypes; ++t) w.U32(s.n[t]), w.U32(s.unanswered[t]);
    w.U32(s.destroyed), w.F64(s.responseMin), w.U32(s.responses);
}
inline void SvcGetStats(ByteReader& r, SvcDayStats& s) {
    for (int t = 0; t < kIncTypes; ++t) s.n[t] = r.U32(), s.unanswered[t] = r.U32();
    s.destroyed = r.U32(), s.responseMin = r.F64(), s.responses = r.U32();
}

inline void SvcSaveServ(ByteWriter& w, const SvcWorld& s) {
    SvcPutMap(w, s.site, [&](uint32_t id, const SvcSiteState& v) { w.U32(id), w.U16(v.busy), w.U32(v.used), w.U32(v.graves), w.U32(v.handled); });
    SvcPutMap(w, s.enrolled, [&](uint32_t id, uint32_t v) { w.U32(id), w.U32(v); });
    w.U8(s.eduSwept ? 1 : 0);
    SvcPutMap(w, s.destroyedUntil, [&](uint32_t id, int64_t v) { w.U32(id), w.I64(v); });
    SvcPutMap(w, s.crimeAcc, [&](uint64_t k, uint16_t v) { w.U64(k), w.U16(v); });
    SvcPutStats(w, s.today), SvcPutStats(w, s.yesterday);
    // the per-cell factors this phase keeps (crime, fire risk, unseated students), by cell key
    const size_t n = std::min({s.cov.cells.size(), s.crimeF.size(), s.fireF.size(), s.unseated.size()});
    w.U32((uint32_t)n);
    for (size_t c = 0; c < n; ++c) w.I32(s.cov.cells[c].key.x), w.I32(s.cov.cells[c].key.z), w.U8(s.crimeF[c]), w.U8(s.fireF[c]), w.U16(s.unseated[c]);
}
inline bool SvcLoadServ(ByteReader& r, SvcWorld& s) {
    s.site.clear(), s.enrolled.clear(), s.destroyedUntil.clear(), s.crimeAcc.clear();
    bool ok = SvcGetCount(r, 18, [&] { const uint32_t id = r.U32(); SvcSiteState& v = s.site[id]; v.busy = r.U16(), v.used = r.U32(), v.graves = r.U32(), v.handled = r.U32(); });
    ok = ok && SvcGetCount(r, 8, [&] { const uint32_t id = r.U32(); s.enrolled[id] = r.U32(); });
    s.eduSwept = r.U8() != 0;
    ok = ok && SvcGetCount(r, 12, [&] { const uint32_t id = r.U32(); s.destroyedUntil[id] = r.I64(); });
    ok = ok && SvcGetCount(r, 10, [&] { const uint64_t k = r.U64(); s.crimeAcc[k] = r.U16(); });
    SvcGetStats(r, s.today), SvcGetStats(r, s.yesterday);
    // into a key-only cell list: the next SvcApplyCoverage carries the values over to the real cells by key
    const uint32_t n = r.U32();
    if (!ok || !r.ok || (uint64_t)n * 12 > r.Remaining()) return false;
    s.cov = SvcCovIn(), s.out = SvcCovOut();
    s.cov.cells.resize(n), s.crimeF.resize(n), s.fireF.resize(n), s.unseated.resize(n);
    for (uint32_t c = 0; c < n; ++c) s.cov.cells[c].key.x = r.I32(), s.cov.cells[c].key.z = r.I32(), s.crimeF[c] = r.U8(), s.fireF[c] = r.U8(), s.unseated[c] = r.U16();
    return r.ok;
}

inline void SvcSaveIncd(ByteWriter& w, const SvcWorld& s) {
    w.U32(s.inc.nextId);
    w.U32((uint32_t)s.inc.inc.size());
    for (const SvcIncident& i : s.inc.inc) {
        w.U32(i.id), w.U8(i.type), w.U8(i.state), w.U8(i.carried), w.U8(i.flags), w.U16(i.step), w.U16(i.severity);
        w.U32(i.building), w.I32(i.key.x), w.I32(i.key.z), w.I32(i.node), w.U32(i.responder), w.F32(i.travel), w.I64(i.start), w.I64(i.due);
    }
    const std::vector<uint32_t>& fl = s.inc.FreeList();
    w.PutVec(fl);
}
inline bool SvcLoadIncd(ByteReader& r, SvcWorld& s) {
    s.inc.Clear();
    const uint32_t next = r.U32();
    const uint32_t n = r.U32();
    if (!r.ok || (uint64_t)n * 48 > r.Remaining()) return false;
    s.inc.inc.resize(n);
    for (SvcIncident& i : s.inc.inc) {
        i.id = r.U32(), i.type = r.U8(), i.state = r.U8(), i.carried = r.U8(), i.flags = r.U8(), i.step = r.U16(), i.severity = r.U16();
        i.building = r.U32(), i.key.x = r.I32(), i.key.z = r.I32(), i.node = r.I32(), i.responder = r.U32(), i.travel = r.F32();
        i.start = r.I64(), i.due = r.I64();
        if (i.type >= kIncTypes || i.state > kIsHeld) return false;
    }
    std::vector<uint32_t> fl;
    if (!r.GetVec(fl)) return false;
    s.inc.nextId = std::max<uint32_t>(next, 1);
    s.inc.Rebuild(fl);
    return r.ok;
}

using SvcGarbLoaded = std::unordered_map<uint64_t, std::pair<double, int64_t>>; // lot -> (pile, last visit)

inline void SvcSaveGarb(ByteWriter& w, const SvcGarbage& g) {
    std::vector<const SvcGarbLot*> all;
    all.reserve(g.Lots());
    for (const auto& grp : g.groups) for (const SvcGarbLot& l : grp) all.push_back(&l);
    std::sort(all.begin(), all.end(), [](const SvcGarbLot* a, const SvcGarbLot* b) { return a->lot < b->lot; });
    w.U32((uint32_t)all.size());
    for (const SvcGarbLot* l : all) w.U64(l->lot), w.F64(l->pile), w.I64(l->last);
    SvcPutMap(w, g.truckAcc, [&](uint64_t k, float v) { w.U64(k), w.F32(v); });
    SvcPutMap(w, g.bucket, [&](uint32_t k, double v) { w.U32(k), w.F64(v); });
    w.I64(g.bucketAt);
}
// The piles go to `loaded` (SvcGarbage::SetLots takes them when the lots are back); the rest straight into g.
inline bool SvcLoadGarb(ByteReader& r, SvcGarbLoaded& loaded, SvcGarbage& g) {
    loaded.clear(), g.Clear();
    bool ok = SvcGetCount(r, 24, [&] { const uint64_t k = r.U64(); const double p = r.F64(); loaded[k] = {p, r.I64()}; });
    ok = ok && SvcGetCount(r, 12, [&] { const uint64_t k = r.U64(); g.truckAcc[k] = r.F32(); });
    ok = ok && SvcGetCount(r, 12, [&] { const uint32_t k = r.U32(); g.bucket[k] = r.F64(); });
    g.bucketAt = r.I64();
    return ok && r.ok;
}

// After a load: what is derived from the state (incidents not yet attended per building, every building's flags).
inline void SvcRecount(SvcWorld& w) {
    w.active.clear();
    for (const SvcIncident& i : w.inc.inc)
        if (i.id && (i.state == kIsWaiting || i.state == kIsEnRoute)) w.active[SvcWorld::ActiveKey(i.building, i.type)]++;
    w.flagSweep = 0;
}

} // namespace city
} // namespace tbd
