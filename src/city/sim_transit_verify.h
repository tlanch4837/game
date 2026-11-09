// Phase 5: TransitSim's stale-state check and its rebuild after a load (included by sim_transit_eval.h).
#pragma once

#include "sim_transit_world.h"

namespace tbd {
namespace city {

// Recomputes everything from scratch with the current inputs and counts what the incremental state has differently:
// zone access lists, every clean link's shares and paths, the mode totals, the parking arrivals, the line ledger.
// Meant for a drained world (Pending() false); a link still dirty counts as one mismatch.
inline int TransitSim::Verify() const {
    int bad = 0;
    std::vector<TransitAccess> acc;
    for (const TransitZone& z : zones) {
        net.AccessAt(z.x, z.z, acc);
        if (net.AccessSig(acc) != z.accessSig) ++bad;
    }
    int64_t mm[kModeCount] = {0, 0, 0, 0};
    std::vector<int64_t> pk(zones.size(), 0);
    RidershipLedger lg;
    lg.Reset(net);
    for (size_t i = 0; i < links.size(); ++i) {
        const TransitLink& l = links[i];
        if (dirty[i] || !l.evaluated) { ++bad; continue; }
        uint16_t sh[kModeCount];
        TransitPath o, b;
        Evaluate(l, sh, o, b);
        if (!std::equal(sh, sh + kModeCount, l.share) || !SamePath(o, l.out) || !SamePath(b, l.back)) ++bad;
        for (int m = 0; m < kModeCount; ++m) mm[m] += 2 * (int64_t)l.n * l.share[m];
        pk[(size_t)l.to] += (int64_t)l.n * l.share[kModeCar];
        if (l.out.legs) lg.Add(net, l.out, TransitRiders(l)), lg.Add(net, l.back.legs ? l.back : l.out, TransitRiders(l));
    }
    for (int m = 0; m < kModeCount; ++m) bad += mm[m] != modeMille[m];
    for (size_t z = 0; z < zones.size(); ++z) bad += z < parkMille.size() && pk[z] != parkMille[z];
    for (size_t l = 0; l < lg.lines.size() && l < ledger.lines.size(); ++l) {
        const LineRiders &a = lg.lines[l], &b = ledger.lines[l];
        bad += a.board != b.board || a.alight != b.alight || a.load != b.load || a.riders != b.riders;
    }
    bad += lg.lines.size() != ledger.lines.size();
    return bad;
}

// After a load: the saved zones and links are in (with their results), net holds the current lines. The derived
// state (index, adjacency, totals, ledger) is rebuilt; nothing is dirty; the access sweep runs (equal signatures: no work).
inline void TransitSim::RebuildAfterLoad() {
    net.p = p.net;
    net.Build(lineIn);
    IndexZones();
    BuildAdjacency();
    parkMille.assign(zones.size(), 0);
    for (int64_t& v : modeMille) v = 0;
    ledger.Reset(net);
    dirty.assign(links.size(), 0), dirtyCount = 0, dirtyCursor_ = 0;
    for (TransitLink& l : links) {
        if (l.from < 0 || l.to < 0 || (size_t)l.from >= zones.size() || (size_t)l.to >= zones.size()) { l.evaluated = 0; continue; }
        Contribute(l, +1);
    }
    for (size_t i = 0; i < links.size(); ++i) if (!links[i].evaluated) MarkLink(i);
    accessCursor = 0, ++version;
}

} // namespace city
} // namespace tbd
