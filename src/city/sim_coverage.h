// The city sim's utilities status and service coverage per cell (Track S, phase S3). Header only, headless
// (tests\sim_coverage_test.cpp).
//
//   Utilities  UtilWorld::Solve (utilities.h) stays the solver; the host runs it on change and hands its per-lot result
//              here as flags (kLotPower | kLotWater | kLotSewage | kLotRoad). A cell is ok for a utility when every built
//              lot in it is; growth (sim_growth.h) reads the per-lot flags.
//   Coverage   each service building (fire, police, health, school, park, transit) covers the lots on its own road
//              network (connected components of the road graph, union-find) within its radius as the crow flies; the
//              value falls off linearly from 255 at the building to 0 at the radius, and a cell keeps the best value
//              any of its lots gets. `funding` (S4's budget slider) scales the radius. No vehicles are simulated.
//              Recomputed only when the services, the roads or the lots change (the host keys it on their versions).
#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "road_network.h"
#include "sim_cells.h"
#include "zoning.h"

namespace tbd {
namespace city {

enum LotUtil : uint8_t { kLotPower = 1, kLotWater = 2, kLotSewage = 4, kLotRoad = 8, kLotAll = 15 };
enum Service : int { kFire = 0, kPolice = 1, kHealth = 2, kSchool = 3, kPark = 4, kTransit = 5, kServices = 6 };

// Connected components of the road network: segments joined through their nodes. segComp[s] = component, -1 dead.
inline void RoadComponents(const Network& net, std::vector<int>& segComp) {
    const int S = (int)net.segs.size(), N = (int)net.nodes.size();
    std::vector<int> p((size_t)(S + N));
    std::iota(p.begin(), p.end(), 0);
    auto find = [&](int x) {
        while (p[(size_t)x] != x) x = p[(size_t)x] = p[(size_t)p[(size_t)x]];
        return x;
    };
    for (int s = 0; s < S; ++s)
        if (net.segs[s].alive) p[(size_t)find(s)] = find(S + net.segs[s].a), p[(size_t)find(s)] = find(S + net.segs[s].b);
    segComp.assign((size_t)S, -1);
    for (int s = 0; s < S; ++s)
        if (net.segs[s].alive) segComp[(size_t)s] = find(s);
}

// Per-lot utility flags (zones.lots order) into the cells: a cell has a utility when all its built lots do.
inline void ApplyUtilities(CitySim& s, const ZoneGrid& z, const std::vector<uint8_t>& lotFlags) {
    std::vector<uint8_t> acc(s.sim.size(), kLotAll);
    for (size_t li = 0; li < z.lots.size() && li < lotFlags.size(); ++li) {
        const Lot& l = z.lots[li];
        if (!l.alive || !l.Built()) continue;
        const int c = s.cells.Find(CellKeyAt(l.c.x, l.c.z));
        if (c >= 0) acc[(size_t)c] &= lotFlags[li];
    }
    for (size_t c = 0; c < s.sim.size(); ++c) {
        CellSim& cs = s.sim[c];
        cs.powerOk = (acc[c] & kLotPower) ? 1 : 0;
        cs.waterOk = (acc[c] & kLotWater) ? 1 : 0;
        cs.sewageOk = (acc[c] & kLotSewage) ? 1 : 0;
    }
}

struct ServiceSource {
    V2 at;
    int kind = kFire;
    float radius = 600;
    int comp = -1; // road component it sits on (-1: not on a road, covers nothing)
};

// Coverage of every cell from the sources. funding[kind] scales that service's radius (1 = as built).
inline void ComputeCoverage(CitySim& s, const ZoneGrid& z, const std::vector<int>& segComp, const std::vector<ServiceSource>& sources,
                            const float (&funding)[kServices]) {
    for (CellSim& c : s.sim) std::fill(std::begin(c.coverage), std::end(c.coverage), (uint8_t)0);
    std::unordered_map<int, std::vector<int>> byComp;
    for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].comp >= 0) byComp[sources[i].comp].push_back((int)i);
    if (byComp.empty()) return;
    for (const Lot& l : z.lots) {
        if (!l.alive) continue;
        const int comp = l.frontSeg >= 0 && l.frontSeg < (int)segComp.size() ? segComp[(size_t)l.frontSeg] : -1;
        const auto it = byComp.find(comp);
        if (it == byComp.end()) continue;
        const int c = s.cells.Find(CellKeyAt(l.c.x, l.c.z));
        if (c < 0) continue;
        CellSim& cs = s.sim[(size_t)c];
        for (int si : it->second) {
            const ServiceSource& src = sources[(size_t)si];
            const float r = src.radius * std::max(0.0f, funding[src.kind]);
            if (r <= 0) continue;
            const float d = Len(src.at - l.c);
            if (d >= r) continue;
            const uint8_t v = (uint8_t)std::clamp(255.0f * (1.0f - d / r), 0.0f, 255.0f);
            cs.coverage[src.kind] = std::max(cs.coverage[src.kind], v);
        }
    }
}

} // namespace city
} // namespace tbd
