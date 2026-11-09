// Phase 6: shipments (trucks), the ledger, outside connections and the seller index. Header only, headless.
//
// A shipment is one truck: it leaves with the units (taken from the seller's stock at once, so nothing is sold twice)
// and adds them to the buyer at arrival. Travel time is the straight distance at truck speed plus loading (a proxy for
// the route cost: the herds route the visible trucks on the real roads). Imports start at an outside connection, exports
// end at one. Mass ledger: stocks + in flight == produced + imported - consumed - exported - lost, at every tick.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "sim_goods_companies.h"

namespace tbd {
namespace city {

struct OutsideLink { int32_t x = 0, z = 0, node = -1; }; // a road (or cargo) outside connection

struct Shipment { // all int32: saved raw
    int32_t good = 0, units = 0;
    int32_t from = 0, to = 0;      // CompanyId raw; 0 = the outside world (import / export)
    int32_t fromNode = -1, toNode = -1;
    int32_t depart = 0, arrive = 0; // sim minutes
};

struct GoodsLedger { // units per good; money in whole currency
    std::array<int64_t, kGoodCount> produced{}, used{}, consumed{}, imported{}, exported{}, lost{}; // used: inputs processed by industry
    int64_t importCost = 0, exportIncome = 0, trucks = 0;
    void Clear() { *this = GoodsLedger(); }
    void Add(const GoodsLedger& o) {
        for (int g = 0; g < kGoodCount; ++g) {
            const size_t i = (size_t)g;
            produced[i] += o.produced[i], used[i] += o.used[i], consumed[i] += o.consumed[i], imported[i] += o.imported[i];
            exported[i] += o.exported[i], lost[i] += o.lost[i];
        }
        importCost += o.importCost, exportIncome += o.exportIncome, trucks += o.trucks;
    }
};

inline int64_t Dist2(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
    const int64_t dx = (int64_t)ax - bx, dz = (int64_t)az - bz;
    return dx * dx + dz * dz;
}
inline int32_t ISqrt(int64_t v) { // floor(sqrt(v)), exact for the distances here
    if (v <= 0) return 0;
    int64_t r = (int64_t)std::sqrt((double)v); // corrected below: the result does not depend on the FPU
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return (int32_t)r;
}

// The nearest outside connection to a point, -1 if there is none.
inline int NearestOutside(const std::vector<OutsideLink>& outs, int32_t x, int32_t z) {
    int best = -1;
    int64_t bd = 0;
    for (size_t i = 0; i < outs.size(); ++i) {
        const int64_t d = Dist2(outs[i].x, outs[i].z, x, z);
        if (best < 0 || d < bd) best = (int)i, bd = d;
    }
    return best;
}

// Sellers by good on a 1 km grid (a dense CSR grid per good over the box of its producers); the nearest with stock wins
// (ties: the lower company index). It lists every producer whatever its stock, so it depends only on the company set:
// rebuilt when that changes (and after a load); every pick checks the live stock, road and open state.
class SellerIndex {
public:
    static constexpr int32_t kBucket = 1024, kMaxSide = 256;
    void Build(CompanyStore& cs) {
        for (Grid& gr : grid_) gr.Reset();
        cs.ForEach([&](uint32_t, const CompanyRec& c) { // pass 1: the boxes
            const Good g = CompanyOutput(c);
            if (Listed(c, g)) grid_[(size_t)g].Grow(Cell(c.x), Cell(c.z));
        });
        for (Grid& gr : grid_) gr.Alloc();
        cs.ForEach([&](uint32_t, const CompanyRec& c) { // pass 2: counts
            const Good g = CompanyOutput(c);
            if (Listed(c, g)) ++grid_[(size_t)g].start[grid_[(size_t)g].Slot(Cell(c.x), Cell(c.z)) + 1];
        });
        for (Grid& gr : grid_) {
            for (size_t k = 1; k < gr.start.size(); ++k) gr.start[k] += gr.start[k - 1];
            gr.items.assign(gr.start.empty() ? 0 : gr.start.back(), 0), gr.fill.assign(gr.start.begin(), gr.start.end());
        }
        cs.ForEach([&](uint32_t i, const CompanyRec& c) { // pass 3: the companies, in index order inside each bucket
            const Good g = CompanyOutput(c);
            if (Listed(c, g)) { Grid& gr = grid_[(size_t)g]; gr.items[gr.fill[gr.Slot(Cell(c.x), Cell(c.z))]++] = i; }
        });
    }
    // The nearest seller of g within maxR metres of (x, z) that still has stock, other than `self`; 0 if none. Rings of
    // buckets outward, clipped to the box; it stops once no farther ring can hold a nearer seller.
    uint32_t Nearest(CompanyStore& cs, Good g, int32_t x, int32_t z, int32_t maxR, uint32_t self) const {
        const Grid& gr = grid_[(size_t)g];
        if (gr.items.empty()) return 0;
        const int32_t bx = Cell(x), bz = Cell(z), rings = maxR / kBucket + 1;
        uint32_t best = 0;
        int64_t bd = (int64_t)maxR * maxR + 1;
        auto visit = [&](int32_t cx, int32_t cz) {
            if (cx < gr.x0 || cx > gr.x1 || cz < gr.z0 || cz > gr.z1) return;
            const uint32_t k = gr.Slot(cx, cz);
            for (uint32_t j = gr.start[k]; j < gr.start[k + 1]; ++j) {
                const uint32_t i = gr.items[j];
                if (i == self || i >= cs.recs.size() || !cs.pool.At(i).Valid()) continue; // built up to an hour ago: may be gone
                const CompanyRec& c = cs.recs[i];
                if (c.stock[(size_t)g] <= 0 || !Sells(c, g)) continue;
                const int64_t d = Dist2(c.x, c.z, x, z);
                if (d < bd || (d == bd && i < best)) bd = d, best = i;
            }
        };
        for (int32_t r = 0; r <= rings; ++r) {
            if (bx - r < gr.x0 && bx + r > gr.x1 && bz - r < gr.z0 && bz + r > gr.z1) break; // the box is all visited
            const int32_t zl = std::max(bz - r, gr.z0), zh = std::min(bz + r, gr.z1);
            for (int32_t cz = zl; cz <= zh; ++cz) {
                if (cz == bz - r || cz == bz + r)
                    for (int32_t cx = std::max(bx - r, gr.x0); cx <= std::min(bx + r, gr.x1); ++cx) visit(cx, cz);
                else {
                    visit(bx - r, cz);
                    if (r) visit(bx + r, cz);
                }
            }
            if (best && bd <= (int64_t)r * kBucket * r * kBucket) break; // nothing in a farther ring can be nearer
        }
        return best;
    }

private:
    static bool Sells(const CompanyRec& c, Good g) { return g != Good::None && CompanyOutput(c) == g && c.node >= 0 && c.closedDays == 0 && c.stock[(size_t)g] > 0; }
    static bool Listed(const CompanyRec& c, Good g) { return g != Good::None && CompanyOutput(c) == g; }
    static int32_t Cell(int32_t v) { return v >= 0 ? v / kBucket : -((-v + kBucket - 1) / kBucket); }
    struct Grid {
        int32_t x0 = 0, x1 = -1, z0 = 0, z1 = -1;
        bool any = false;
        std::vector<uint32_t> start, fill, items;
        void Reset() { x0 = z0 = 0, x1 = z1 = -1, any = false, start.clear(), items.clear(); }
        void Grow(int32_t x, int32_t z) {
            if (!any) { x0 = x1 = x, z0 = z1 = z, any = true; return; }
            x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
        }
        void Alloc() {
            if (!any) return;
            x1 = std::min(x1, x0 + kMaxSide - 1), z1 = std::min(z1, z0 + kMaxSide - 1); // a 256 km city: clamped at the edge
            start.assign((size_t)(x1 - x0 + 1) * (size_t)(z1 - z0 + 1) + 1, 0);
        }
        uint32_t Slot(int32_t x, int32_t z) const {
            x = std::clamp(x, x0, x1), z = std::clamp(z, z0, z1);
            return (uint32_t)((z - z0) * (x1 - x0 + 1) + (x - x0));
        }
    };
    std::array<Grid, kGoodCount> grid_{};
};

} // namespace city
} // namespace tbd
