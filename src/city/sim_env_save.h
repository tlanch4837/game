// Phase 3's .tbdsim chunks (CS2 sim roadmap 3.4: POLL WTHR WATR). Header only, headless (tests\sim_env_save_test).
//
// The hourly pass runs on a worker and is applied at the next hour, so a save may fall between a pass and its apply.
// To load into exactly the same future, a chunk carries the applied state, the applied result (what the factors and
// flags show) and, when a pass is in flight, its finished state and result (the save waits for the worker).
//   POLL v1  applied air noise ground water, applied result, u8 pending [pending fields, pending result]
//   WATR v1  applied table depth wasFlooded lastMinute, u8 pending [pending table depth wasFlooded lastMinute]
//   WTHR v1  chain step, kind, wind direction, last season seen
// Heights are not saved: the load makes zero height tiles in the table's order and the host re-samples the terrain.
#pragma once

#include <cstdint>
#include <vector>

#include "sim_env.h"
#include "sim_save.h"

namespace tbd {
namespace city {

constexpr uint16_t kEnvPollVersion = 1, kEnvWaterVersion = 1, kEnvWeatherVersion = 1;

inline void EnvPutResult(ByteWriter& w, const EnvResult& r) {
    w.I64(r.minute);
    w.PutVec(r.keys);
    w.U32((uint32_t)r.cells.size());
    for (const EnvCellOut& c : r.cells) w.U8(c.air), w.U8(c.noise), w.U8(c.ground), w.U8(c.water), w.U16(c.floodMm), w.U32(c.flags);
    w.PutVec(r.newlyFlooded);
    w.F64(r.meanAir), w.F64(r.meanNoise), w.F64(r.meanGround), w.F64(r.meanTable);
    w.U8(r.supply), w.F64(r.pumpEff), w.I32(r.flooded), w.I32(r.polluted), w.I32(r.noisy);
}
inline bool EnvGetResult(ByteReader& r, EnvResult& o) {
    o = EnvResult();
    o.minute = r.I64();
    if (!r.GetVec(o.keys)) return false;
    const uint32_t n = r.U32();
    if (!r.ok || n != o.keys.size() || (uint64_t)n * 10 > r.Remaining()) return false;
    o.cells.resize(n);
    for (EnvCellOut& c : o.cells) c.air = r.U8(), c.noise = r.U8(), c.ground = r.U8(), c.water = r.U8(), c.floodMm = r.U16(), c.flags = r.U32();
    r.GetVec(o.newlyFlooded);
    o.meanAir = r.F64(), o.meanNoise = r.F64(), o.meanGround = r.F64(), o.meanTable = r.F64();
    o.supply = r.U8(), o.pumpEff = r.F64(), o.flooded = r.I32(), o.polluted = r.I32(), o.noisy = r.I32();
    return r.ok;
}
inline void EnvPutPoll(ByteWriter& w, const EnvPollution& p) { EnvPutField(w, p.air), EnvPutField(w, p.noise), EnvPutField(w, p.ground), EnvPutField(w, p.water); }
inline bool EnvGetPoll(ByteReader& r, EnvPollution& p) { return EnvGetField(r, p.air) && EnvGetField(r, p.noise) && EnvGetField(r, p.ground) && EnvGetField(r, p.water); }

inline void EnvPutWater(ByteWriter& w, const EnvSim& s) {
    EnvPutField(w, s.water.table), EnvPutField(w, s.water.depth);
    w.PutVec(s.wasFlooded);
    w.I64(s.lastMinute);
}
inline bool EnvGetWater(ByteReader& r, EnvSim& s) {
    if (!EnvGetField(r, s.water.table) || !EnvGetField(r, s.water.depth) || !r.GetVec(s.wasFlooded)) return false;
    s.lastMinute = r.I64();
    if (s.water.table.Keys().size() != s.water.depth.Keys().size()) return false;
    for (size_t i = 0; i < s.water.table.Keys().size(); ++i)
        if (s.water.table.Keys()[i] != s.water.depth.Keys()[i]) return false;
    s.water.height.Clear();
    for (CellKey k : s.water.table.Keys()) s.water.height.Cell(k).fill(0.0f); // the host re-samples them
    return r.ok;
}

// What a chunk save needs: the applied state and result, and the in-flight pass if any.
struct EnvSaveView {
    const EnvSim* cur = nullptr;
    const EnvResult* curResult = nullptr;
    const EnvSim* pending = nullptr;       // null: no pass in flight
    const EnvResult* pendingResult = nullptr;
};
struct EnvLoadInto {
    EnvSim cur, pending;
    EnvResult curResult, pendingResult;
    bool hasPollPending = false, hasWaterPending = false;
};

inline void EnvSavePoll(ByteWriter& w, const EnvSaveView& v) {
    EnvPutPoll(w, v.cur->poll);
    EnvPutResult(w, *v.curResult);
    w.U8(v.pending ? 1 : 0);
    if (v.pending) EnvPutPoll(w, v.pending->poll), EnvPutResult(w, *v.pendingResult);
}
inline bool EnvLoadPoll(ByteReader& r, EnvLoadInto& o) {
    if (!EnvGetPoll(r, o.cur.poll) || !EnvGetResult(r, o.curResult)) return false;
    o.hasPollPending = r.U8() != 0;
    if (o.hasPollPending && (!EnvGetPoll(r, o.pending.poll) || !EnvGetResult(r, o.pendingResult))) return false;
    return r.ok;
}
inline void EnvSaveWater(ByteWriter& w, const EnvSaveView& v) {
    EnvPutWater(w, *v.cur);
    w.U8(v.pending ? 1 : 0);
    if (v.pending) EnvPutWater(w, *v.pending);
}
inline bool EnvLoadWater(ByteReader& r, EnvLoadInto& o) {
    if (!EnvGetWater(r, o.cur)) return false;
    o.hasWaterPending = r.U8() != 0;
    if (o.hasWaterPending && !EnvGetWater(r, o.pending)) return false;
    return r.ok;
}

struct EnvWeatherState {
    WeatherChain chain;
    int lastSeason = -1;
    uint8_t lastKind = kWxClear;
};
inline void EnvSaveWeather(ByteWriter& w, const EnvWeatherState& s) {
    w.I64(s.chain.step), w.U8(s.chain.kind), w.U8(s.chain.windDir), w.I32(s.lastSeason), w.U8(s.lastKind);
}
inline bool EnvLoadWeather(ByteReader& r, EnvWeatherState& s) {
    s.chain.step = r.I64(), s.chain.kind = r.U8(), s.chain.windDir = r.U8(), s.lastSeason = r.I32(), s.lastKind = r.U8();
    return r.ok && s.chain.kind < kWxKinds && s.chain.windDir < 8 && s.chain.step >= 0;
}

} // namespace city
} // namespace tbd
