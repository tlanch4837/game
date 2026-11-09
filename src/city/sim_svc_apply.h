// Phase 4: the host's two inputs into SvcWorld: the buildings (from the registry, on its change) and a finished
// coverage pass (at a tick boundary). The heavy half of a pass's install (lookups, cell groups, the cell -> buildings
// lists) is SvcPrepare, which the worker runs after SvcCover; SvcInstall on the main thread only moves it in, carries
// the per-cell factors over by key and starts a flags sweep (spread over the next ticks by SvcFlushFlags).
#pragma once

#include "sim_svc_world.h"

namespace tbd {
namespace city {

struct SvcPrepared {
    SvcCovIn cov;
    SvcCovOut out;
    std::vector<SvcCellStat> stats;
    std::unordered_map<uint64_t, int> cellIndex;
    std::unordered_map<uint32_t, int> siteIndex;
    std::vector<std::vector<int>> groups;
    std::vector<SvcBldg> bldSnap;         // the buildings the cell lists were built from (ids, keys, hints)
    std::vector<int> cellBldOff, cellBldIdx;
};

// Cell -> buildings (CSR) for `bld` over the cells of `cov` / `cellIndex`.
inline void SvcCellLists(const SvcCovIn& cov, const std::unordered_map<uint64_t, int>& cellIndex, const std::vector<SvcBldg>& bld,
                         std::vector<int>& off, std::vector<int>& idx) {
    const size_t C = cov.cells.size();
    off.assign(C + 1, 0);
    std::vector<int> cellOf(bld.size(), -1);
    for (size_t i = 0; i < bld.size(); ++i) {
        const SvcBldg& b = bld[i];
        int c = b.cell >= 0 && (size_t)b.cell < C && cov.cells[(size_t)b.cell].key == b.key ? b.cell : -1;
        if (c < 0) { const auto it = cellIndex.find(PackKey(b.key)); c = it == cellIndex.end() ? -1 : it->second; }
        if ((cellOf[i] = c) >= 0) ++off[(size_t)c + 1];
    }
    for (size_t c = 0; c < C; ++c) off[c + 1] += off[c];
    idx.assign((size_t)off[C], 0);
    std::vector<int> fill(off.begin(), off.end() - 1);
    for (size_t i = 0; i < bld.size(); ++i)
        if (cellOf[i] >= 0) idx[(size_t)fill[(size_t)cellOf[i]]++] = (int)i;
}

// Worker side (pure): everything SvcInstall needs that is O(cells + buildings).
inline void SvcPrepare(SvcPrepared& p) {
    p.stats.resize(p.cov.cells.size());
    p.cellIndex.clear(), p.siteIndex.clear();
    p.cellIndex.reserve(p.cov.cells.size());
    for (size_t c = 0; c < p.cov.cells.size(); ++c) p.cellIndex[PackKey(p.cov.cells[c].key)] = (int)c;
    for (size_t s = 0; s < p.cov.sites.size(); ++s) p.siteIndex[p.cov.sites[s].id] = (int)s;
    p.groups.assign(kSvcGroups, {});
    for (size_t c = 0; c < p.cov.cells.size(); ++c) p.groups[SvcGroupOf(p.cov.cells[c].key)].push_back((int)c);
    SvcCellLists(p.cov, p.cellIndex, p.bldSnap, p.cellBldOff, p.cellBldIdx);
}

inline bool SvcSameIds(const std::vector<SvcBldg>& a, const std::vector<SvcBldg>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].id != b[i].id || !(a[i].key == b[i].key)) return false;
    return true;
}

inline void SvcInstall(SvcWorld& w, SvcPrepared&& p) {
    // the per-cell values this phase keeps follow their cells by key into the new order
    std::unordered_map<uint64_t, uint32_t> keep;
    for (size_t c = 0; c < w.cov.cells.size() && c < w.crimeF.size(); ++c)
        keep[PackKey(w.cov.cells[c].key)] = (uint32_t)w.crimeF[c] | (uint32_t)w.fireF[c] << 8 | (uint32_t)w.unseated[c] << 16;
    w.cov = std::move(p.cov), w.out = std::move(p.out), w.cellStat = std::move(p.stats);
    w.cellIndex = std::move(p.cellIndex), w.siteIndex = std::move(p.siteIndex), w.cellGroups = std::move(p.groups);
    const size_t C = w.cov.cells.size();
    w.crimeF.assign(C, 0), w.fireF.assign(C, 0), w.unseated.assign(C, 0);
    for (size_t c = 0; c < C; ++c) {
        const auto it = keep.find(PackKey(w.cov.cells[c].key));
        if (it != keep.end()) w.crimeF[c] = (uint8_t)it->second, w.fireF[c] = (uint8_t)(it->second >> 8), w.unseated[c] = (uint16_t)(it->second >> 16);
    }
    for (auto it = w.site.begin(); it != w.site.end();) // sites gone: their state goes (vehicles out come home to nothing)
        it = w.siteIndex.count(it->first) ? std::next(it) : w.site.erase(it);
    for (const SvcSite& s : w.cov.sites) w.site.try_emplace(s.id);
    if (SvcSameIds(p.bldSnap, w.bld)) w.cellBldOff = std::move(p.cellBldOff), w.cellBldIdx = std::move(p.cellBldIdx);
    else SvcCellLists(w.cov, w.cellIndex, w.bld, w.cellBldOff, w.cellBldIdx); // the buildings changed meanwhile (rare)
    w.flagSweep = 0;
    ++w.version;
}

// Both halves at once (tests, and the host's synchronous fallback).
inline void SvcApplyCoverage(SvcWorld& w, SvcCovIn&& in, SvcCovOut&& out, std::vector<SvcCellStat>&& stats) {
    SvcPrepared p;
    p.cov = std::move(in), p.out = std::move(out), p.stats = std::move(stats), p.bldSnap = w.bld;
    SvcPrepare(p);
    SvcInstall(w, std::move(p));
}

// New building list (sorted by id by the host). New or changed buildings get a flags look; gone ones lose their rubble
// timer (their incidents finish on their own).
inline void SvcSetBuildings(SvcWorld& w, std::vector<SvcBldg>&& next) {
    std::vector<int> at;
    uint32_t maxIdx = 0;
    for (const SvcBldg& b : next) maxIdx = std::max(maxIdx, b.id & Handle::kIndexMask);
    at.assign((size_t)maxIdx + 1, -1);
    for (size_t i = 0; i < next.size(); ++i) {
        at[next[i].id & Handle::kIndexMask] = (int)i;
        const SvcBldg* old = w.Bldg(next[i].id);
        if (!old || old->kind != next[i].kind || !(old->key == next[i].key) || old->homes != next[i].homes || old->lot != next[i].lot) w.MarkDirty(next[i].id);
    }
    for (auto it = w.destroyedUntil.begin(); it != w.destroyedUntil.end();) {
        const uint32_t i = it->first & Handle::kIndexMask;
        it = i < at.size() && at[i] >= 0 && next[(size_t)at[i]].id == it->first ? std::next(it) : w.destroyedUntil.erase(it);
    }
    w.bld = std::move(next), w.bldAt = std::move(at);
    SvcCellLists(w.cov, w.cellIndex, w.bld, w.cellBldOff, w.cellBldIdx);
    ++w.version;
}

} // namespace city
} // namespace tbd
