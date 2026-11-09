// Phase 5: TransitSim's lines, the deferred tick and the evaluation of a link (included by sim_transit_world.h).
#pragma once

#include "sim_transit_world.h"

namespace tbd {
namespace city {

inline void TransitSim::Evaluate(const TransitLink& l, uint16_t share[kModeCount], TransitPath& out, TransitPath& back) const {
    static const float kParkMin[4] = {0.0f, 3.0f, 8.0f, 16.0f};
    const TransitZone& A = zones[(size_t)l.from];
    const TransitZone& B = zones[(size_t)l.to];
    ModeTrip t;
    t.metres = std::hypot(B.x - A.x, B.z - A.z);
    out = TransitPath(), back = TransitPath();
    if (net.Best(A.access, B.access, out)) {
        net.Best(B.access, A.access, back);
        t.transit = true, t.transitMin = out.minutes, t.fare = p.fare * p.fareMul * (float)out.legs;
    }
    t.taxi = p.taxis > 0;
    t.parkingMin = kParkMin[B.parking & 3];
    ModeParams mp = p.mode;
    for (int k = 0; k < 4; ++k) mp.carOwn[k] *= A.carOwn;
    QuantiseShares(ChooseModes(mp, t, A.evalWealth), share);
}

inline void TransitSim::RefreshAccess(size_t z) {
    TransitZone& Z = zones[z];
    net.AccessAt(Z.x, Z.z, Z.access);
    const uint32_t sig = net.AccessSig(Z.access);
    if (sig != Z.accessSig) Z.accessSig = sig, MarkZone((int)z);
}

inline void TransitSim::SetLines(const std::vector<LineInput>& in) {
    TransitNet nn;
    nn.p = p.net;
    nn.Build(in);
    std::vector<int16_t> remap(net.lines.size(), -1);
    for (size_t i = 0; i < net.lines.size(); ++i)
        for (size_t j = 0; j < nn.lines.size(); ++j) {
            const TransitLine &a = net.lines[i], &b = nn.lines[j];
            if (a.id == b.id && a.x == b.x && a.z == b.z && a.closed == b.closed) { remap[i] = (int16_t)j; break; }
        }
    auto fix = [&](TransitPath& q) {
        for (int k = 0; k < q.legs; ++k) {
            if (q.line[k] < 0 || (size_t)q.line[k] >= remap.size() || remap[(size_t)q.line[k]] < 0) return false;
            q.line[k] = remap[(size_t)q.line[k]];
        }
        return true;
    };
    for (size_t i = 0; i < links.size(); ++i) {
        TransitLink& l = links[i];
        if (!fix(l.out) || !fix(l.back)) l.out = TransitPath(), l.back = TransitPath(), MarkLink(i); // its line went
    }
    for (TransitZone& z : zones) { // until the sweep reaches a zone its old list stays usable: same lines, new indices
        size_t w = 0;
        for (const TransitAccess& a : z.access)
            if (a.line >= 0 && (size_t)a.line < remap.size() && remap[(size_t)a.line] >= 0) z.access[w] = a, z.access[w++].line = remap[(size_t)a.line];
        z.access.resize(w);
    }
    net = std::move(nn);
    lineIn = in;
    ledger.Reset(net);
    for (const TransitLink& l : links)
        if (l.evaluated && l.out.legs) ledger.Add(net, l.out, TransitRiders(l)), ledger.Add(net, l.back.legs ? l.back : l.out, TransitRiders(l));
    accessCursor = 0, ++version; // every zone's access is looked at again, a group a minute
}

inline void TransitSim::SetFare(float fare, float mul) {
    if (fare == p.fare && mul == p.fareMul) return;
    p.fare = fare, p.fareMul = mul;
    for (size_t i = 0; i < links.size(); ++i) if (links[i].out.legs) MarkLink(i); // only trips that can ride care
}

inline void TransitSim::SetTaxis(int taxis) {
    const bool had = p.taxis > 0;
    p.taxis = std::max(0, taxis);
    if (had != (p.taxis > 0)) for (size_t i = 0; i < links.size(); ++i) MarkLink(i);
}

inline bool SamePath(const TransitPath& a, const TransitPath& b) {
    if (a.legs != b.legs) return false;
    for (int k = 0; k < a.legs && k < 2; ++k)
        if (a.line[k] != b.line[k] || a.on[k] != b.on[k] || a.off[k] != b.off[k]) return false;
    return true;
}

inline void TransitSim::Tick(int64_t) {
    for (int k = 0; accessCursor < zones.size() && k < p.zonesPerTick; ++k) RefreshAccess(accessCursor++);
    const size_t n = links.size();
    int done = 0;
    for (size_t scanned = 0; dirtyCount > 0 && done < p.linksPerTick && scanned < n; ++scanned) {
        const size_t i = dirtyCursor_++ % n;
        if (!dirty[i]) continue;
        TransitLink& l = links[i];
        uint16_t sh[kModeCount];
        TransitPath o, b;
        Evaluate(l, sh, o, b);
        const bool same = l.evaluated && std::equal(sh, sh + kModeCount, l.share) && SamePath(o, l.out) && SamePath(b, l.back);
        if (!same) {
            Contribute(l, -1);
            std::copy(sh, sh + kModeCount, l.share), l.out = o, l.back = b, l.evaluated = 1;
            Contribute(l, +1);
            ++version;
        }
        l.out.minutes = o.minutes, l.back.minutes = b.minutes;
        dirty[i] = 0, --dirtyCount, ++done, ++evaluations;
    }
    if (dirtyCursor_ >= n) dirtyCursor_ = n ? dirtyCursor_ % n : 0;
}

// Parking: a zone's spaces against the cars arriving a day; a level up past 1, 1.5, 2.5 times, down only at 80 % of
// that (no flip-flop). A changed level re-evaluates the links that drive into the zone.
inline void TransitSim::SummarizeParking() {
    static const float kUp[3] = {1.0f, 1.5f, 2.5f};
    for (size_t z = 0; z < zones.size(); ++z) {
        TransitZone& Z = zones[z];
        const float cap = p.parkBase + (float)Z.jobs * p.parkPerJob + (float)Z.pop * p.parkPerHome;
        const float ratio = (float)((double)parkMille[z] / 1000.0) / std::max(1.0f, cap);
        int lv = Z.parking;
        while (lv < 3 && ratio >= kUp[lv]) ++lv;
        while (lv > 0 && ratio < kUp[lv - 1] * 0.8f) --lv;
        if (lv == Z.parking) continue;
        Z.parking = (uint8_t)lv;
        for (int32_t i : adj_[z]) if (links[(size_t)i].to == (int32_t)z) MarkLink((size_t)i);
    }
}

} // namespace city
} // namespace tbd

#include "sim_transit_verify.h"
