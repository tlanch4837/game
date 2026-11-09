// Phase 5: TransitNet's transfer table and best path (included by sim_transit_net.h; not on its own).
#pragma once

#include "sim_transit_net.h"

namespace tbd {
namespace city {

// For every ordered pair of lines: each stop of A with the nearest stop of B within the transfer radius.
inline void TransitNet::BuildTransfers() {
    const size_t L = lines.size();
    transfers.assign(L * L, {});
    const float r2 = p.transferRadius * p.transferRadius;
    for (size_t a = 0; a < L; ++a)
        for (size_t b = 0; b < L; ++b) {
            if (a == b) continue;
            const TransitLine& A = lines[a];
            const TransitLine& B = lines[b];
            for (int sa = 0; sa < A.Stops(); ++sa) {
                int best = -1;
                float bd = r2;
                for (int sb = 0; sb < B.Stops(); ++sb) {
                    const float dx = B.x[(size_t)sb] - A.x[(size_t)sa], dz = B.z[(size_t)sb] - A.z[(size_t)sa], d = dx * dx + dz * dz;
                    if (d < bd) bd = d, best = sb;
                }
                if (best >= 0) transfers[a * L + b].push_back({(int16_t)a, (int16_t)b, (int16_t)sa, (int16_t)best, std::sqrt(bd) * 1.25f / p.walkMetresPerMin});
            }
        }
}

// The quickest way from access list A to access list B: a direct line, else one transfer. False: no way. Ties keep the
// first found (lines in order, stops in order), so the answer never depends on anything but the inputs.
inline bool TransitNet::Best(const std::vector<TransitAccess>& A, const std::vector<TransitAccess>& B, TransitPath& out) const {
    out = TransitPath();
    float best = 1e30f;
    const size_t L = lines.size();
    for (const TransitAccess& a : A)
        for (const TransitAccess& b : B) {
            if (a.line == b.line) {
                if (a.stop == b.stop) continue; // the same stop: walking is the answer
                const float t = a.walkMin + WaitMin(a.line) + RideMin(a.line, a.stop, b.stop) + b.walkMin;
                if (t < best) {
                    best = t, out.legs = 1;
                    out.line[0] = a.line, out.on[0] = a.stop, out.off[0] = b.stop, out.line[1] = out.on[1] = out.off[1] = -1;
                }
                continue;
            }
            for (const Transfer& x : transfers[(size_t)a.line * L + (size_t)b.line]) {
                if (x.sa == a.stop || x.sb == b.stop) continue; // a leg with no ride
                const float t = a.walkMin + WaitMin(a.line) + RideMin(a.line, a.stop, x.sa) + x.walkMin + p.transferPenaltyMin +
                                WaitMin(b.line) + RideMin(b.line, x.sb, b.stop) + b.walkMin;
                if (t < best) {
                    best = t, out.legs = 2;
                    out.line[0] = a.line, out.on[0] = a.stop, out.off[0] = x.sa;
                    out.line[1] = b.line, out.on[1] = x.sb, out.off[1] = b.stop;
                }
            }
        }
    if (!out.legs) return false;
    out.minutes = best;
    return true;
}

} // namespace city
} // namespace tbd
