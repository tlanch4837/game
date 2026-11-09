// Phase 1 save chunks (CS2 sim roadmap 3.4): CITZ citizens, HHLD households, OCCU occupancy, MIGR migration and
// cursors. Each is a plain byte vector; sim_save.h (Phase 0) wraps them in its chunk header and CRC. Little-endian hosts
// only (x64, arm64), so columns are written raw. The views (cells, commute matrix, hood vacancies) are NOT saved: they
// are a pure sum over the records and are recounted on load. Load returns false on any mismatch: the caller then
// starts the citizens fresh (LifeBootstrap), as the roadmap says for a bad or newer chunk.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "sim_life.h"

namespace tbd {
namespace city {

constexpr uint32_t LifeFourCC(char a, char b, char c, char d) { return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 | (uint32_t)(uint8_t)d << 24; }
constexpr uint32_t kChunkCITZ = LifeFourCC('C', 'I', 'T', 'Z'), kChunkHHLD = LifeFourCC('H', 'H', 'L', 'D');
constexpr uint32_t kChunkOCCU = LifeFourCC('O', 'C', 'C', 'U'), kChunkMIGR = LifeFourCC('M', 'I', 'G', 'R');
constexpr uint16_t kLifeChunkVersion = 1;

struct LifeChunks { std::vector<uint8_t> citz, hhld, occu, migr; };

// Appends without zero-filling first (insert, not resize + memcpy): the autosave runs this on the main thread, and the
// callers reserve each chunk once (LifeSave), so a save is one pass over the columns.
struct LifeWriter {
    std::vector<uint8_t>& out;
    template <class T>
    void Pod(const T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "pod");
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    template <class T>
    void Vec(const std::vector<T>& v) {
        static_assert(std::is_arithmetic<T>::value, "arithmetic columns only (no padding)");
        Pod((uint32_t)v.size());
        if (v.empty()) return;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
        out.insert(out.end(), p, p + v.size() * sizeof(T));
    }
};

struct LifeReader {
    const std::vector<uint8_t>& in;
    size_t at = 0;
    bool ok = true;
    template <class T>
    void Pod(T& v) {
        if (!ok || in.size() - at < sizeof(T) || at > in.size()) { ok = false; return; }
        std::memcpy(&v, in.data() + at, sizeof(T));
        at += sizeof(T);
    }
    template <class T>
    void Vec(std::vector<T>& v, uint32_t maxCount = 1u << 26) {
        uint32_t n = 0;
        Pod(n);
        if (!ok || n > maxCount || (in.size() - at) / sizeof(T) < n) { ok = false; return; }
        v.resize(n);
        if (n) std::memcpy(v.data(), in.data() + at, (size_t)n * sizeof(T));
        at += (size_t)n * sizeof(T);
    }
    bool Done() const { return ok && at == in.size(); }
};

inline void LifeSavePool(LifeWriter& o, const HandlePool& p) { o.Vec(p.Gens()), o.Vec(p.AliveBits()), o.Vec(p.FreeList()); }
inline bool LifeLoadPool(LifeReader& r, HandlePool& p) {
    std::vector<uint8_t> g, a;
    std::vector<uint32_t> f;
    r.Vec(g), r.Vec(a), r.Vec(f);
    return r.ok && p.SetRaw(std::move(g), std::move(a), std::move(f));
}

#define TBD_LIFE_CIT_COLS(X) X(household) X(work) X(bornMonth) X(school) X(edu) X(jobLevel) X(health) X(happy) X(flags) X(stage) X(commuteMin) X(seed) X(nextInHousehold)
#define TBD_LIFE_HH_COLS(X) X(home) X(firstMember) X(nextInHome) X(savings) X(incomeMonth) X(rentMonth) X(spendMonth) X(movedInMonth) \
    X(size) X(adults) X(carCount) X(flags) X(wealth) X(unhappyMonths) X(poorMonths)

// References between the loaded records are in range (a corrupt chunk must not index past a column).
inline bool LifeValidateRecords(const LifeWorld& w) {
    const auto liveHh = [&](uint32_t raw) { return w.hh.Alive(HouseholdId(raw)); };
    for (uint32_t ci = 1; ci < w.cit.Capacity(); ++ci) {
        if (!w.cit.At(ci).Valid()) continue;
        if (!liveHh(w.cit.household[ci]) || w.cit.edu[ci] >= kEduLevels || w.cit.jobLevel[ci] >= kEduLevels || w.cit.stage[ci] >= kLifeStages) return false;
        if (w.cit.nextInHousehold[ci] && !w.cit.Alive(CitizenId(w.cit.nextInHousehold[ci]))) return false;
    }
    for (uint32_t hi = 1; hi < w.hh.Capacity(); ++hi) {
        if (!w.hh.At(hi).Valid()) continue;
        if (w.hh.wealth[hi] > 3 || (w.hh.firstMember[hi] && !w.cit.Alive(CitizenId(w.hh.firstMember[hi])))) return false;
        if (w.hh.nextInHome[hi] && !liveHh(w.hh.nextInHome[hi])) return false;
        uint32_t n = 0;
        for (uint32_t c = w.hh.firstMember[hi]; c; c = w.cit.nextInHousehold[LifeWorld::Ix(c)])
            if (++n > 255 || w.cit.household[LifeWorld::Ix(c)] != w.hh.At(hi).v) return false;
    }
    for (const LifeBuilding& L : w.bld)
        if (L.id && (L.jobKind > 2 || (L.firstHousehold && !liveHh(L.firstHousehold)))) return false;
    return true;
}

// The views from the records (after a load): cells, commute, used / filled, hood vacancies, job lists, search order.
inline void LifeRebuildViews(LifeWorld& w) {
    w.cells.clear(), w.commute.clear(), w.statResidents = 0;
    int hoods = 1, maxCell = -1;
    for (LifeBuilding& L : w.bld) {
        if (!L.id) continue;
        L.used = 0;
        for (int e = 0; e < kEduLevels; ++e) L.filled[e] = 0;
        hoods = std::max(hoods, L.hood + 1), maxCell = std::max(maxCell, L.cell);
        w.statResidents += L.stat;
    }
    w.hoodCount = hoods;
    w.hoodVac.assign((size_t)hoods, {});
    w.hoodJobs.assign((size_t)hoods, {});
    for (uint32_t i = 1; i < w.bld.size(); ++i) {
        const LifeBuilding& L = w.bld[i];
        if (!L.id) continue;
        if (L.Slots()) w.hoodJobs[(size_t)L.hood].push_back(i);
        for (int e = 0; e < kEduLevels; ++e) w.hoodVac[(size_t)L.hood][(size_t)e] += L.slots[e];
    }
    if (maxCell >= 0) w.CellAt(maxCell);
    LifeCountAll(w, +1);
    w.seekers.resize((size_t)hoods);
    LifeComputeNear(w);
}

// One chunk's payload (0 CITZ, 1 HHLD, 2 OCCU, 3 MIGR): each reads only the sim, so the four can be made at once on
// different threads (the autosave's parallel copy, city_sim_save_io.inl). LifeSave makes all four: the same bytes.
inline void LifeSavePart(const LifeSim& s, int part, std::vector<uint8_t>& out) {
    const LifeWorld& w = s.w;
    out.clear();
    if (part == 0) { // one allocation per chunk (upper bounds: 33 B a citizen and 39 B a household of columns plus the pools' 6 B)
        out.reserve(256 + (size_t)w.cit.Capacity() * 48);
        LifeWriter o{out}; LifeSavePool(o, w.cit.pool);
#define TBD_W(c) o.Vec(w.cit.c);
        TBD_LIFE_CIT_COLS(TBD_W)
#undef TBD_W
    } else if (part == 1) {
        out.reserve(256 + (size_t)w.hh.Capacity() * 56);
        LifeWriter o{out}; LifeSavePool(o, w.hh.pool);
#define TBD_W(c) o.Vec(w.hh.c);
        TBD_LIFE_HH_COLS(TBD_W)
#undef TBD_W
    } else if (part == 2) {
        out.reserve(16 + w.bld.size() * 64);
        LifeWriter o{out};
        o.Pod((uint32_t)w.bld.size());
        // each building packed field by field into one buffer, appended once: the same bytes as a Pod per field
        constexpr size_t kB = sizeof(LifeBuilding::id) + sizeof(LifeBuilding::cell) + sizeof(LifeBuilding::hood) + sizeof(LifeBuilding::cellX) +
                              sizeof(LifeBuilding::cellZ) + sizeof(LifeBuilding::homes) + sizeof(LifeBuilding::stat) + sizeof(LifeBuilding::slots) +
                              sizeof(LifeBuilding::firstHousehold) + sizeof(LifeBuilding::level) + sizeof(LifeBuilding::jobKind);
        for (const LifeBuilding& L : w.bld) {
            uint8_t b[kB];
            uint8_t* q = b;
            auto put = [&q](const auto& v) { std::memcpy(q, &v, sizeof(v)), q += sizeof(v); };
            put(L.id), put(L.cell), put(L.hood), put(L.cellX), put(L.cellZ), put(L.homes), put(L.stat);
            put(L.slots); // the array: kEduLevels values in order, as the Pod loop wrote them
            put(L.firstHousehold), put(L.level), put(L.jobKind);
            out.insert(out.end(), b, b + kB);
        }
    } else if (part == 3) {
        LifeWriter o{out};
        static_assert(sizeof(LifeMonthStats) == 15 * sizeof(int32_t), "LifeMonthStats is 15 int32");
        o.Pod(w.minute), o.Pod(w.month), o.Pod(s.lastMonth), o.Pod(w.matchStart), o.Pod(w.homeCursor);
        o.Pod(s.mig.attract), o.Pod(s.mig.quota), o.Pod(s.mig.admitted), o.Pod(s.mig.avgHappy), o.Pod(s.mig.vacantJobs);
        o.Pod(s.mig.seekers), o.Pod(s.mig.freeHomes), o.Vec(s.mig.freeList);
        o.Pod(w.cur), o.Pod((uint32_t)w.history.size());
        for (const LifeMonthStats& m : w.history) o.Pod(m);
        o.Vec(w.jobCursor), o.Vec(w.wantsHome), o.Pod((uint32_t)w.seekers.size());
        for (const auto& q : w.seekers) o.Vec(q);
    }
}

inline void LifeSave(const LifeSim& s, LifeChunks& out) {
    out = LifeChunks();
    LifeSavePart(s, 0, out.citz), LifeSavePart(s, 1, out.hhld), LifeSavePart(s, 2, out.occu), LifeSavePart(s, 3, out.migr);
}

// Loads the four chunks into s (its tuning, calendar, seed and pointers stay). False: s is reset (start fresh).
inline bool LifeLoad(LifeSim& s, const LifeChunks& in) {
    LifeWorld& w = s.w;
    s.Reset(w.citySeed);
    bool ok = true;
    { LifeReader r{in.citz}; ok = ok && LifeLoadPool(r, w.cit.pool);
#define TBD_R(c) r.Vec(w.cit.c);
      TBD_LIFE_CIT_COLS(TBD_R)
#undef TBD_R
      ok = ok && r.Done() && w.cit.household.size() == w.cit.Capacity(); }
    { LifeReader r{in.hhld}; ok = ok && LifeLoadPool(r, w.hh.pool);
#define TBD_R(c) r.Vec(w.hh.c);
      TBD_LIFE_HH_COLS(TBD_R)
#undef TBD_R
      ok = ok && r.Done() && w.hh.home.size() == w.hh.Capacity(); }
    { LifeReader r{in.occu};
      uint32_t n = 0;
      r.Pod(n);
      if (n > (1u << 24)) r.ok = false;
      if (r.ok) w.bld.assign(n, LifeBuilding());
      for (uint32_t i = 0; i < n && r.ok; ++i) {
          LifeBuilding& L = w.bld[i];
          r.Pod(L.id), r.Pod(L.cell), r.Pod(L.hood), r.Pod(L.cellX), r.Pod(L.cellZ), r.Pod(L.homes), r.Pod(L.stat);
          for (int e = 0; e < kEduLevels; ++e) r.Pod(L.slots[e]);
          r.Pod(L.firstHousehold), r.Pod(L.level), r.Pod(L.jobKind);
          if (L.id && (L.hood < 0 || L.hood > 65535)) r.ok = false;
      }
      ok = ok && r.Done(); }
    { LifeReader r{in.migr};
      r.Pod(w.minute), r.Pod(w.month), r.Pod(s.lastMonth), r.Pod(w.matchStart), r.Pod(w.homeCursor);
      r.Pod(s.mig.attract), r.Pod(s.mig.quota), r.Pod(s.mig.admitted), r.Pod(s.mig.avgHappy), r.Pod(s.mig.vacantJobs);
      r.Pod(s.mig.seekers), r.Pod(s.mig.freeHomes), r.Vec(s.mig.freeList);
      uint32_t n = 0;
      r.Pod(w.cur), r.Pod(n);
      if (n > 120) r.ok = false;
      if (r.ok) w.history.resize(n);
      for (uint32_t i = 0; i < n && r.ok; ++i) r.Pod(w.history[i]);
      r.Vec(w.jobCursor), r.Vec(w.wantsHome), r.Pod(n);
      if (n > 65536) r.ok = false;
      if (r.ok) w.seekers.resize(n);
      for (uint32_t i = 0; i < n && r.ok; ++i) r.Vec(w.seekers[i]);
      ok = ok && r.Done(); }
    ok = ok && LifeValidateRecords(w);
    if (ok) LifeRebuildViews(w);
    if (!ok || !w.Verify()) { s.Reset(w.citySeed); return false; }
    return true;
}

} // namespace city
} // namespace tbd
