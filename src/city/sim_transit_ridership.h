// Phase 5 ridership: riders a day per line, boardings and alightings per stop, the load on every segment (per
// direction), and how much of it the line's vehicles can carry. Header only, headless (tests\sim_transit_test).
//
// Kept incrementally: a trip's riders are added along its path (Add) and taken off with negative riders, so the ledger
// is always the exact sum of the stored link results (integers: no drift). Conservation holds by construction: every
// rider boards once and alights once per leg, and a segment's load is the riders crossing it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim_transit_net.h"

namespace tbd {
namespace city {

struct LineRiders {
    std::vector<int64_t> board, alight; // per stop
    std::vector<int64_t> load;          // per segment (TransitLine::Segs order)
    int64_t riders = 0;                 // boardings on the line (a transfer counts on both lines)
    int64_t MaxLoad() const { int64_t m = 0; for (int64_t v : load) m = std::max(m, v); return m; }
};

class RidershipLedger {
public:
    std::vector<LineRiders> lines;

    void Reset(const TransitNet& net) {
        lines.assign(net.lines.size(), LineRiders());
        for (size_t l = 0; l < net.lines.size(); ++l) {
            const TransitLine& L = net.lines[l];
            lines[l].board.assign((size_t)L.Stops(), 0), lines[l].alight.assign((size_t)L.Stops(), 0);
            lines[l].load.assign((size_t)std::max(0, L.Segs()), 0);
        }
    }
    // riders along every leg of the path (negative: take them off again)
    void Add(const TransitNet& net, const TransitPath& path, int64_t riders) {
        if (!riders) return;
        for (int k = 0; k < path.legs && k < 2; ++k) {
            const int l = path.line[k], a = path.on[k], b = path.off[k];
            if (l < 0 || (size_t)l >= lines.size() || (size_t)l >= net.lines.size()) continue;
            const TransitLine& L = net.lines[(size_t)l];
            const int n = L.Stops();
            if (a < 0 || b < 0 || a >= n || b >= n || a == b) continue;
            LineRiders& R = lines[(size_t)l];
            R.board[(size_t)a] += riders, R.alight[(size_t)b] += riders, R.riders += riders;
            if (L.closed) {
                for (int s = a; s != b; s = (s + 1) % n) R.load[(size_t)s] += riders;
            } else if (b > a) {
                for (int s = a; s < b; ++s) R.load[(size_t)s] += riders;            // forward: seg s = stop s -> s+1
            } else {
                for (int s = b; s < a; ++s) R.load[(size_t)(n - 1 + s)] += riders;  // back: seg n-1+s = stop s+1 -> s
            }
        }
    }
    int64_t Boardings() const { int64_t n = 0; for (const LineRiders& r : lines) for (int64_t v : r.board) n += v; return n; }
    int64_t Alightings() const { int64_t n = 0; for (const LineRiders& r : lines) for (int64_t v : r.alight) n += v; return n; }
    // Share of the riders the line can carry (1: all; under 1 the busiest segment is over its capacity).
    static float Served(const TransitLine& L, const LineRiders& R) {
        const int64_t m = R.MaxLoad();
        return m <= L.capPerDir || m <= 0 ? 1.0f : (float)((double)L.capPerDir / (double)m);
    }
    // Conservation along a line: boardings = alightings = riders, and no segment carries a negative load.
    static bool Conserved(const TransitLine& L, const LineRiders& R) {
        if ((int)R.board.size() != L.Stops()) return false;
        int64_t b = 0, a = 0;
        for (size_t i = 0; i < R.board.size(); ++i) b += R.board[i], a += R.alight[i];
        if (b != a || b != R.riders) return false;
        for (int64_t v : R.load) if (v < 0) return false;
        return true;
    }
};

} // namespace city
} // namespace tbd
