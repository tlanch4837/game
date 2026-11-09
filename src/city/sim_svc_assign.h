// Phase 4: the coverage pass itself (SvcCover, see sim_svc_coverage.h). Header only, headless. Pure: the same input
// gives the same bytes on any thread.
#pragma once

#include <algorithm>
#include <tuple>

#include "sim_svc_coverage.h"

namespace tbd {
namespace city {

inline void SvcCover(const SvcCovIn& in, SvcCovOut& out) {
    const size_t C = in.cells.size(), S = in.sites.size();
    out.cov.assign(C, {});
    out.nearest.assign(C * kServicesV2 * kSvcNear, SvcNear());
    out.served.assign(S, 0);
    out.slotHasSite.assign(kServicesV2, 0);
    // node -> cells (CSR)
    const int N = in.graph ? in.graph->nodes : 0;
    std::vector<int> nodeOff((size_t)N + 1, 0), nodeCells;
    for (const SvcCellIn& c : in.cells) if (c.node >= 0 && c.node < N) ++nodeOff[(size_t)c.node + 1];
    for (int n = 0; n < N; ++n) nodeOff[(size_t)n + 1] += nodeOff[(size_t)n];
    nodeCells.resize((size_t)nodeOff[(size_t)N]);
    {
        std::vector<int> fill(nodeOff.begin(), nodeOff.end() - 1);
        for (size_t c = 0; c < C; ++c)
            if (in.cells[c].node >= 0 && in.cells[c].node < N) nodeCells[(size_t)fill[(size_t)in.cells[c].node]++] = (int)c;
    }
    struct Pair { float m; uint32_t site, cell; };
    std::vector<Pair> pairs[kServicesV2];
    std::vector<float> minutes;
    std::vector<int> touched;
    for (size_t s = 0; s < S; ++s) {
        const SvcSite& site = in.sites[s];
        const int slot = site.spec.slot;
        if (slot < 0 || slot >= kServicesV2) continue;
        out.slotHasSite[(size_t)slot] = 1;
        const float reach = site.spec.reachMin;
        if (site.spec.road && in.graph && site.node >= 0) {
            in.graph->Reach(site.node, reach, minutes, touched);
            for (int n : touched)
                for (int k = nodeOff[(size_t)n]; k < nodeOff[(size_t)n + 1]; ++k)
                    pairs[slot].push_back({minutes[(size_t)n], (uint32_t)s, (uint32_t)nodeCells[(size_t)k]});
        } else if (!site.spec.road || !in.graph) { // crow flies (telecom, parks; everything before there are roads)
            const float metres = reach * kSvcCrowMetresPerMin;
            for (size_t c = 0; c < C; ++c) {
                const float dx = in.cells[c].x - site.x, dz = in.cells[c].z - site.z;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d <= metres) pairs[slot].push_back({d / kSvcCrowMetresPerMin, (uint32_t)s, (uint32_t)c});
            }
        }
    }
    std::vector<uint32_t> capLeft(S), needLeft(C);
    std::vector<float> acc(C);
    for (int slot = 0; slot < kServicesV2; ++slot) {
        std::vector<Pair>& P = pairs[slot];
        if (P.empty()) continue;
        std::sort(P.begin(), P.end(), [](const Pair& a, const Pair& b) { return std::tie(a.m, a.site, a.cell) < std::tie(b.m, b.site, b.cell); });
        for (size_t s = 0; s < S; ++s) capLeft[s] = in.sites[s].spec.serves;
        for (size_t c = 0; c < C; ++c) needLeft[c] = in.cells[c].people, acc[c] = 0.0f;
        for (const Pair& p : P) {
            const SvcCellIn& cell = in.cells[p.cell];
            const SvcSpec& sp = in.sites[p.site].spec;
            SvcNear* nr = &out.nearest[((size_t)p.cell * kServicesV2 + (size_t)slot) * kSvcNear];
            for (int k = 0; k < kSvcNear; ++k)
                if (nr[k].site < 0) { nr[k] = {(int32_t)p.site, p.m}; break; }
            const float f = SvcFalloff(p.m, sp.reachMin);
            if (cell.people == 0) { acc[p.cell] = std::max(acc[p.cell], f); continue; } // empty land: as if served
            const uint32_t take = std::min(capLeft[p.site], needLeft[p.cell]);
            if (!take) continue;
            capLeft[p.site] -= take, needLeft[p.cell] -= take, out.served[p.site] += take;
            acc[p.cell] += f * (float)take / (float)cell.people;
        }
        for (size_t c = 0; c < C; ++c) out.cov[c][(size_t)slot] = (uint8_t)std::clamp((int)std::lround(acc[c] * 255.0f), 0, 255);
    }
}

} // namespace city
} // namespace tbd
