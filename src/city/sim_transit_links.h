// Phase 5: TransitSim's inputs (zones, links, lines, fare, taxis) and the incremental totals (included by
// sim_transit_world.h; not on its own).
#pragma once

#include "sim_transit_world.h"

namespace tbd {
namespace city {

inline int TransitSim::Link(uint64_t fromKey, uint64_t toKey) const {
    const int a = Zone(fromKey), b = Zone(toKey);
    if (a < 0 || b < 0) return -1;
    const auto it = std::lower_bound(links.begin(), links.end(), std::make_pair(a, b),
                                     [](const TransitLink& l, const std::pair<int, int>& k) { return l.from != k.first ? l.from < k.first : l.to < k.second; });
    return it != links.end() && it->from == a && it->to == b ? (int)(it - links.begin()) : -1;
}

// A link's share of the totals in (sign 1) or out (-1). Uses only what the link stores, so out undoes in exactly.
inline void TransitSim::Contribute(const TransitLink& l, int sign) {
    if (!l.evaluated) return;
    for (int m = 0; m < kModeCount; ++m) modeMille[m] += (int64_t)sign * 2 * l.n * l.share[m];
    const int64_t r = TransitRiders(l) * sign;
    if (l.out.legs) ledger.Add(net, l.out, r), ledger.Add(net, l.back.legs ? l.back : l.out, r);
    if ((size_t)l.to < parkMille.size()) parkMille[(size_t)l.to] += (int64_t)sign * l.n * l.share[kModeCar];
}

inline bool TransitSim::ZoneMoved(const TransitZone& z) const {
    const int64_t d = std::llabs((int64_t)z.pop - (int64_t)z.evalPop);
    if (d > std::max<int64_t>(p.popDirtyMin, (int64_t)(p.popDirty * (float)z.evalPop))) return true;
    float a = 0, b = 0, diff = 0;
    for (int k = 0; k < 4; ++k) a += z.wealth[k], b += z.evalWealth[k];
    if (a <= 0 || b <= 0) return (a > 0) != (b > 0);
    for (int k = 0; k < 4; ++k) diff += std::fabs(z.wealth[k] / a - z.evalWealth[k] / b);
    return diff > p.popDirty;
}

inline void TransitSim::SetZones(const std::vector<TransitZoneIn>& in) {
    std::vector<uint8_t> seen(zones.size(), 0);
    for (const TransitZoneIn& q : in) {
        int i = Zone(q.key);
        const bool fresh = i < 0;
        if (fresh) {
            i = (int)zones.size();
            TransitZone z;
            z.key = q.key, z.x = q.x, z.z = q.z;
            zones.push_back(z), zoneIdx_[q.key] = i, adj_.emplace_back(), parkMille.push_back(0), seen.push_back(0);
        }
        TransitZone& z = zones[(size_t)i];
        seen[(size_t)i] = 1;
        z.pop = q.pop, z.jobs = q.jobs, z.carOwn = q.carOwn;
        for (int k = 0; k < 4; ++k) z.wealth[k] = q.wealth[k];
        if (fresh) {
            z.evalPop = z.pop;
            for (int k = 0; k < 4; ++k) z.evalWealth[k] = z.wealth[k];
        }
        if (z.x != q.x || z.z != q.z) z.x = q.x, z.z = q.z, z.accessSig = ~0u, accessCursor = std::min(accessCursor, (size_t)i);
    }
    for (size_t i = 0; i < zones.size(); ++i) {
        TransitZone& z = zones[i];
        if (!seen[i]) z.pop = 0, z.jobs = 0;
        if (ZoneMoved(z)) {
            z.evalPop = z.pop;
            for (int k = 0; k < 4; ++k) z.evalWealth[k] = z.wealth[k];
            MarkZone((int)i);
        }
    }
}

inline void TransitSim::BuildAdjacency() {
    adj_.assign(zones.size(), {});
    for (size_t i = 0; i < links.size(); ++i) {
        adj_[(size_t)links[i].from].push_back((int32_t)i);
        if (links[i].to != links[i].from) adj_[(size_t)links[i].to].push_back((int32_t)i);
    }
}

inline void TransitSim::SetLinks(const std::vector<TransitLinkIn>& in) {
    std::vector<TransitLink> next;
    next.reserve(in.size());
    uint64_t lastKey = ~0ull;
    int lastA = -1;
    for (const TransitLinkIn& q : in) {
        if (q.from != lastKey) lastKey = q.from, lastA = Zone(q.from); // the host's links come grouped by origin
        const int a = lastA, b = Zone(q.to);
        const int32_t n = (int32_t)std::lround(std::max(0.0f, q.n));
        if (a < 0 || b < 0 || a == b || n <= 0) continue;
        TransitLink l;
        l.from = a, l.to = b, l.n = n;
        next.push_back(l);
    }
    auto less = [](const TransitLink& x, const TransitLink& y) { return x.from != y.from ? x.from < y.from : x.to < y.to; };
    if (!std::is_sorted(next.begin(), next.end(), less)) std::sort(next.begin(), next.end(), less);
    size_t w = 0; // merge duplicates
    for (size_t i = 0; i < next.size(); ++i) {
        if (w && next[w - 1].from == next[i].from && next[w - 1].to == next[i].to) next[w - 1].n += next[i].n;
        else next[w++] = next[i];
    }
    next.resize(w);
    std::vector<uint8_t> nd(next.size(), 1);
    size_t o = 0;
    bool structural = next.size() != links.size();
    for (size_t i = 0; i < next.size(); ++i) { // walk both sorted lists: keep the results of the links that stay
        while (o < links.size() && (links[o].from < next[i].from || (links[o].from == next[i].from && links[o].to < next[i].to)))
            Contribute(links[o++], -1), structural = true;
        if (o < links.size() && links[o].from == next[i].from && links[o].to == next[i].to) {
            const int32_t n = next[i].n, was = links[o].n;
            next[i] = links[o], nd[i] = links[o].evaluated ? dirty[o] : 1;
            if (std::abs(n - was) > std::max<int32_t>(p.linkDirtyMin, (int32_t)(p.linkDirty * (float)was))) { // small drift: kept as it was
                Contribute(links[o], -1);
                next[i].n = n;
                Contribute(next[i], +1);
            }
            ++o;
        } else {
            structural = true;
        }
    }
    while (o < links.size()) Contribute(links[o++], -1), structural = true;
    links.swap(next), dirty.swap(nd);
    if (structural) {
        dirtyCount = 0;
        for (uint8_t d : dirty) dirtyCount += d;
        dirtyCursor_ = 0;
        BuildAdjacency();
    }
    ++version;
}

} // namespace city
} // namespace tbd
