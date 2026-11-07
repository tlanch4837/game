// Neighbourhoods (the unit of everything, docs/city_sim_design.md section 2). Header only, headless (tests\sim_hoods_test.cpp).
//
//   Build      the occupied CityCells (128 m) are the "blocks". Two touching cells are linked unless a major road (large
//              road, highway), a railway or open water runs between them. A deterministic breadth-first flood fill cuts
//              the graph into clusters of at most maxCells; clusters under minCells merge into the neighbour they share
//              the most unblocked border with. Ids follow the lowest cell key, names are a hash of that key (so a
//              neighbourhood keeps its name while it grows) over the road_names.h vocabulary. Rebuilt on a road-network
//              or occupied-cell change only; same roads and same cells always give the same hoods.
//   RollUp     O(cells): zero the totals, add every cell's counters to its hood (every 10 game minutes).
//   Links      HoodLinksFromSuper turns the sim's supercell home -> job links (sim_cells.h ForEachLink) into
//              hood -> hood commute counts, keeping the totals exact.
//   Matching   MatchLocalFirst: own supply first, then neighbours ring by ring (nearest by travel time first), the rest
//              stays unmet. Used for jobs, school seats, services.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "road_names.h"
#include "road_network.h"
#include "sim.h"
#include "sim_cells.h"
#include "sim_flow.h"

namespace tbd {
namespace city {

struct Hood {
    int id = 0;
    std::string name;
    std::vector<int> cells;      // indices into CitySim::sim / cells.cells, ascending
    std::vector<int> neighbours; // hood ids within two cells, ascending
    V2 centre{0, 0};             // mean of the cell centres
    int centroidNode = -1;       // nearest car-road node to the centre, -1 when no road is near
    CellKey minKey{0, 0};
    // rolled up from the cells
    uint32_t pop[kAgeBands] = {};
    uint32_t workers = 0;        // adults living here who have a job (anywhere)
    uint32_t employed = 0;       // jobs here that are filled
    uint32_t jobs[kJobKinds] = {};
    uint32_t schoolSeats[2] = {}; // primary, secondary (stub: kids in a school-covered cell get a seat)
    uint32_t Population() const { return pop[0] + pop[1] + pop[2]; }
    uint32_t Jobs() const { return jobs[0] + jobs[1] + jobs[2]; }
};

struct HoodLink {
    int from = 0, to = 0;
    float n = 0; // workers (or kids, or seats) moving from -> to
};

using WaterFn = std::function<bool(V2)>; // optional: is this ground point open water

class SimHoods {
public:
    int minCells = 6, maxCells = 20;
    uint64_t seed = 1;

    std::vector<Hood> hoods;
    std::vector<int> cellHood; // per CitySim cell index: hood id, -1 for a cell with no lots
    uint64_t builtNet = ~0ull;
    size_t builtOccupied = ~(size_t)0;
    uint64_t generation = 0;   // bumped on every rebuild: ids may have moved

    // True when the road network or the set of occupied cells changed since the last Build.
    bool Stale(const CitySim& s, const Network& net) const { return builtNet != net.version || builtOccupied != Occupied(s); }

    static size_t Occupied(const CitySim& s) {
        size_t n = 0;
        for (const CityCell& c : s.cells.cells) n += !c.lots.empty();
        return n;
    }

    void Build(const CitySim& s, const Network& net, const SegGrid& grid, const WaterFn& water = nullptr) {
        builtNet = net.version;
        builtOccupied = Occupied(s);
        ++generation;
        hoods.clear();
        const size_t nc = s.cells.cells.size();
        cellHood.assign(nc, -1);
        std::vector<int> occ; // occupied cells, ordered by (z, x)
        std::unordered_map<uint64_t, int> at;
        for (size_t i = 0; i < nc; ++i)
            if (!s.cells.cells[i].lots.empty()) occ.push_back((int)i), at[PackKey(s.cells.cells[i].key)] = (int)i;
        std::sort(occ.begin(), occ.end(), [&](int a, int b) {
            const CellKey ka = s.cells.cells[(size_t)a].key, kb = s.cells.cells[(size_t)b].key;
            return ka.z != kb.z ? ka.z < kb.z : ka.x < kb.x;
        });
        if (occ.empty()) return;

        std::unordered_set<uint64_t> blocked;
        MarkBarriers(net, blocked);
        auto edgeKey = [](CellKey a, CellKey b) {
            const uint64_t pa = PackKey(a), pb = PackKey(b);
            return pa < pb ? pa * 0x9E3779B97F4A7C15ull ^ pb : pb * 0x9E3779B97F4A7C15ull ^ pa;
        };
        auto isBlocked = [&](CellKey a, CellKey b) {
            if (blocked.count(edgeKey(a, b))) return true;
            if (water) {
                const V2 m{((a.x + b.x) * 0.5f + 0.5f) * kCityCellSize, ((a.z + b.z) * 0.5f + 0.5f) * kCityCellSize};
                if (water(m)) return true;
            }
            return false;
        };
        static const int kDx[4] = {1, 0, -1, 0}, kDz[4] = {0, 1, 0, -1};
        auto step = [&](int cell, int d) { // the occupied 4-neighbour of `cell` in direction d, -1 none
            const CellKey k = s.cells.cells[(size_t)cell].key;
            const auto it = at.find(PackKey({k.x + kDx[d], k.z + kDz[d]}));
            return it == at.end() ? -1 : it->second;
        };

        // 1. flood fill into clusters of at most maxCells
        std::vector<int> label(nc, -1);
        std::vector<std::vector<int>> cl;
        std::vector<int> queue;
        for (int seedCell : occ) {
            if (label[(size_t)seedCell] >= 0) continue;
            const int id = (int)cl.size();
            cl.emplace_back();
            queue.assign(1, seedCell);
            label[(size_t)seedCell] = id;
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                const int c = queue[qi];
                cl[(size_t)id].push_back(c);
                if ((int)cl[(size_t)id].size() >= maxCells) { // full: the rest of the queue goes back to the pool
                    for (size_t r = qi + 1; r < queue.size(); ++r) label[(size_t)queue[r]] = -1;
                    break;
                }
                for (int d = 0; d < 4; ++d) {
                    const int n = step(c, d);
                    if (n < 0 || label[(size_t)n] >= 0) continue;
                    if (isBlocked(s.cells.cells[(size_t)c].key, s.cells.cells[(size_t)n].key)) continue;
                    label[(size_t)n] = id;
                    queue.push_back(n);
                }
            }
        }
        // 2. merge small clusters into the neighbour they share the most unblocked border with
        std::set<int> settled; // small clusters nothing can take (alone, or only walls around them)
        for (;;) {
            int small = -1;
            for (size_t i = 0; i < cl.size(); ++i)
                if (!cl[i].empty() && (int)cl[i].size() < minCells && !settled.count((int)i) && (small < 0 || cl[i].size() < cl[(size_t)small].size())) small = (int)i;
            if (small < 0) break;
            std::map<int, int> open, shut; // neighbour cluster -> border cells
            for (int c : cl[(size_t)small])
                for (int d = 0; d < 4; ++d) {
                    const int n = step(c, d);
                    if (n < 0 || label[(size_t)n] == small) continue;
                    (isBlocked(s.cells.cells[(size_t)c].key, s.cells.cells[(size_t)n].key) ? shut : open)[label[(size_t)n]]++;
                }
            // a few cells behind a wall still join the larger neighbour; a sizeable group keeps its own identity
            const std::map<int, int>& pick = !open.empty() ? open : ((int)cl[(size_t)small].size() * 2 < minCells ? shut : open);
            int best = -1, bn = 0;
            for (const auto& [k, v] : pick)
                if (v > bn || (v == bn && cl[(size_t)k].size() < cl[(size_t)best].size())) best = k, bn = v;
            if (best < 0) {
                settled.insert(small);
                continue;
            }
            for (int c : cl[(size_t)small]) label[(size_t)c] = best, cl[(size_t)best].push_back(c);
            cl[(size_t)small].clear();
        }
        // 3. ids by lowest cell key
        std::vector<int> order;
        for (size_t i = 0; i < cl.size(); ++i)
            if (!cl[i].empty()) order.push_back((int)i);
        auto lowest = [&](int c) {
            CellKey best = s.cells.cells[(size_t)cl[(size_t)c][0]].key;
            for (int cell : cl[(size_t)c]) {
                const CellKey k = s.cells.cells[(size_t)cell].key;
                if (k.z < best.z || (k.z == best.z && k.x < best.x)) best = k;
            }
            return best;
        };
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const CellKey ka = lowest(a), kb = lowest(b);
            return ka.z != kb.z ? ka.z < kb.z : ka.x < kb.x;
        });
        hoods.resize(order.size());
        std::set<std::string> used;
        for (size_t h = 0; h < order.size(); ++h) {
            Hood& H = hoods[h];
            H.id = (int)h;
            H.cells = cl[(size_t)order[h]];
            std::sort(H.cells.begin(), H.cells.end());
            H.minKey = lowest(order[h]);
            double x = 0, z = 0;
            for (int c : H.cells) {
                const CellKey k = s.cells.cells[(size_t)c].key;
                cellHood[(size_t)c] = (int)h;
                x += (k.x + 0.5) * kCityCellSize, z += (k.z + 0.5) * kCityCellSize;
            }
            H.centre = {(float)(x / H.cells.size()), (float)(z / H.cells.size())};
            H.name = Name(seed, H.minKey, used);
            float t, d;
            const int seg = grid.Nearest(net, H.centre, 900.0f, t, d);
            if (seg >= 0) H.centroidNode = t < 0.5f ? net.segs[(size_t)seg].a : net.segs[(size_t)seg].b;
        }
        // 4. neighbours: another hood within two cells
        for (Hood& H : hoods) {
            std::set<int> nb;
            for (int c : H.cells) {
                const CellKey k = s.cells.cells[(size_t)c].key;
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const auto it = at.find(PackKey({k.x + dx, k.z + dz}));
                        if (it != at.end() && cellHood[(size_t)it->second] != H.id) nb.insert(cellHood[(size_t)it->second]);
                    }
            }
            H.neighbours.assign(nb.begin(), nb.end());
        }
    }

    // Totals from the cells; cheap, O(cells).
    void RollUp(const CitySim& s) {
        for (Hood& H : hoods) {
            for (uint32_t& v : H.pop) v = 0;
            for (uint32_t& v : H.jobs) v = 0;
            H.workers = H.employed = 0;
            H.schoolSeats[0] = H.schoolSeats[1] = 0;
        }
        for (size_t c = 0; c < s.sim.size() && c < cellHood.size(); ++c) {
            if (cellHood[c] < 0) continue;
            Hood& H = hoods[(size_t)cellHood[c]];
            const CellSim& cs = s.sim[c];
            for (int b = 0; b < kAgeBands; ++b) H.pop[b] += cs.residents[b];
            H.workers += cs.working;
            for (int k = 0; k < kJobKinds; ++k) H.jobs[k] += cs.jobCap[k], H.employed += cs.employed[k];
            if (cs.coverage[3] > 0) H.schoolSeats[0] += cs.residents[kKids]; // stub until schools have seat counts
        }
    }

    // A name for a hood: hash of (seed, lowest cell key) over the road_names vocabulary, unique within `used`.
    static std::string Name(uint64_t seed, CellKey k, std::set<std::string>& used) {
        static const char* const kSuffix[] = {"Heights", "Park", "Village", "Hill", "Gardens", "Quarter", "Fields", "Green", "Commons", "Point", "Row", "Crossing"};
        const uint64_t h0 = SimSeed(seed, PackKey(k), 0x600D);
        const auto& pool = names_detail::Pool((int)(h0 % 5));
        const std::string base = pool[(h0 >> 8) % pool.size()];
        for (int attempt = 0; attempt < 40; ++attempt) {
            const uint64_t h = SimSeed(h0, (uint64_t)attempt + 1);
            const std::string n = base + " " + kSuffix[h % 12];
            if (attempt >= 12) { // vocabulary exhausted for this base: number it
                const std::string numbered = n + " " + std::to_string(attempt - 10);
                if (used.insert(numbered).second) return numbered;
                continue;
            }
            if (used.insert(n).second) return n;
        }
        return base;
    }

    // Hood -> hood commute counts from the sim's supercell links. Totals are kept: the sum of n equals Links().
    void LinksFromSuper(const CitySim& s, std::vector<HoodLink>& out) const {
        out.clear();
        std::map<std::pair<int, int>, double> acc;
        std::vector<std::pair<int, double>> hw, jw; // hood -> weight
        s.ForEachLink([&](int sh, int sj, int32_t n) {
            if (n <= 0) return;
            Weights(s, sh, false, hw);
            Weights(s, sj, true, jw);
            if (hw.empty() || jw.empty()) return;
            for (const auto& [h, a] : hw)
                for (const auto& [j, b] : jw) acc[{h, j}] += (double)n * a * b;
        });
        for (const auto& [k, v] : acc)
            if (v > 1e-3) out.push_back({k.first, k.second, (float)v});
    }

private:
    // The share of a supercell's workers (homes) or filled jobs that sits in each hood; shares sum to 1.
    void Weights(const CitySim& s, int super, bool jobs, std::vector<std::pair<int, double>>& out) const {
        out.clear();
        double total = 0;
        for (int c : s.SuperCellList(super)) {
            if ((size_t)c >= cellHood.size() || cellHood[(size_t)c] < 0) continue;
            const CellSim& cs = s.sim[(size_t)c];
            const double w = jobs ? (double)cs.employed[0] + cs.employed[1] + cs.employed[2] : (double)cs.working;
            if (w <= 0) continue;
            total += w;
            const int h = cellHood[(size_t)c];
            auto it = std::find_if(out.begin(), out.end(), [h](const auto& p) { return p.first == h; });
            if (it == out.end()) out.push_back({h, w});
            else it->second += w;
        }
        if (total > 0)
            for (auto& p : out) p.second /= total;
    }

    // Every link between two touching cell centres that a major road, highway or railway crosses: the wall is the road
    // itself, wherever it runs inside the cells.
    void MarkBarriers(const Network& net, std::unordered_set<uint64_t>& blocked) const {
        auto edgeKey = [](CellKey a, CellKey b) {
            const uint64_t pa = PackKey(a), pb = PackKey(b);
            return pa < pb ? pa * 0x9E3779B97F4A7C15ull ^ pb : pb * 0x9E3779B97F4A7C15ull ^ pa;
        };
        auto cross = [](V2 a, V2 b, V2 c, V2 d) { // do segments ab and cd properly cross
            const float d1 = Cross2(b - a, c - a), d2 = Cross2(b - a, d - a), d3 = Cross2(d - c, a - c), d4 = Cross2(d - c, b - c);
            return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
        };
        for (size_t sg = 0; sg < net.segs.size(); ++sg) {
            if (!net.segs[sg].alive) continue;
            const Cat c = net.Prof((int)sg).cat;
            if (c != Cat::Large && c != Cat::Highway && c != Cat::Rail) continue;
            const Cubic cu = net.Curve((int)sg);
            const int n = std::max(2, (int)std::ceil(cu.Length(8) / 24.0f));
            V2 prev = cu.At(0);
            for (int i = 1; i <= n; ++i) {
                const V2 q = cu.At((float)i / n);
                const CellKey k0 = CellKeyAt(std::min(prev.x, q.x), std::min(prev.z, q.z)), k1 = CellKeyAt(std::max(prev.x, q.x), std::max(prev.z, q.z));
                for (int cz = k0.z - 1; cz <= k1.z + 1; ++cz)
                    for (int cx = k0.x - 1; cx <= k1.x + 1; ++cx) {
                        const V2 mid{(cx + 0.5f) * kCityCellSize, (cz + 0.5f) * kCityCellSize};
                        const V2 east{mid.x + kCityCellSize, mid.z}, south{mid.x, mid.z + kCityCellSize};
                        if (cross(prev, q, mid, east)) blocked.insert(edgeKey({cx, cz}, {cx + 1, cz}));
                        if (cross(prev, q, mid, south)) blocked.insert(edgeKey({cx, cz}, {cx, cz + 1}));
                    }
                prev = q;
            }
        }
    }
};

// ------------------------------------------------------------------------------------------------ local-first matching
using TravelFn = std::function<float(int, int)>; // minutes between two hoods

struct MatchResult {
    std::vector<HoodLink> assign; // from (demand side) -> to (supply side), n units
    std::vector<float> unmet;     // per hood: demand nobody could serve
};

// Demand (workers wanting a job, kids wanting a seat) meets supply: every hood takes from its own first; then ring 1 of
// neighbours nearest first, ring 2, ring 3. A pass is finished for every hood before the next ring starts, so a hood with
// a low id does not eat its neighbour's local supply. Leftover demand is `unmet` (shows as unemployment / no seat).
inline void MatchLocalFirst(const std::vector<Hood>& hoods, const std::vector<float>& demand, std::vector<float> supply, const TravelFn& travel,
                            MatchResult& out, int maxRing = 3) {
    const int n = (int)hoods.size();
    out.assign.clear();
    out.unmet.assign((size_t)n, 0.0f);
    std::vector<float> need(demand.begin(), demand.end());
    need.resize((size_t)n, 0.0f);
    supply.resize((size_t)n, 0.0f);
    auto tt = [&](int a, int b) {
        if (travel) return travel(a, b);
        return Len(hoods[(size_t)a].centre - hoods[(size_t)b].centre) / (40.0f * 1000.0f / 60.0f); // 40 km/h straight line
    };
    for (int h = 0; h < n; ++h) { // ring 0
        const float take = std::min(need[(size_t)h], supply[(size_t)h]);
        if (take <= 0) continue;
        out.assign.push_back({h, h, take});
        need[(size_t)h] -= take, supply[(size_t)h] -= take;
    }
    std::vector<std::vector<int>> frontier((size_t)n), seen((size_t)n);
    for (int h = 0; h < n; ++h) frontier[(size_t)h] = {h}, seen[(size_t)h] = {h};
    for (int ring = 1; ring <= maxRing; ++ring) {
        for (int h = 0; h < n; ++h) {
            std::vector<int> next;
            for (int f : frontier[(size_t)h])
                for (int nb : hoods[(size_t)f].neighbours)
                    if (std::find(seen[(size_t)h].begin(), seen[(size_t)h].end(), nb) == seen[(size_t)h].end()) seen[(size_t)h].push_back(nb), next.push_back(nb);
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            std::stable_sort(next.begin(), next.end(), [&](int a, int b) { return tt(h, a) < tt(h, b); });
            frontier[(size_t)h] = next;
            for (int t : next) {
                if (need[(size_t)h] <= 0) break;
                const float take = std::min(need[(size_t)h], supply[(size_t)t]);
                if (take <= 0) continue;
                out.assign.push_back({h, t, take});
                need[(size_t)h] -= take, supply[(size_t)t] -= take;
            }
        }
    }
    for (int h = 0; h < n; ++h) out.unmet[(size_t)h] = need[(size_t)h];
}

} // namespace city
} // namespace tbd
