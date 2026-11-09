// Shared synthetic town for the Phase 4 service tests: side x side CitySim-sized cells (128 m), a road node at each
// cell centre joined to its four neighbours both ways (slow roads, so travel time matters), one home building per cell
// and optional service sites. Header only.
#pragma once

#include <cstdio>
#include <memory>
#include <vector>

#include "city/sim_svc.h"

namespace svctest {
using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

struct Town {
    int side = 0;
    float kmh = 10.0f;
    uint32_t people = 0;
    LinkTable table;
    std::shared_ptr<const SvcGraph> graph;
    std::vector<SvcSite> sites;
    int skipNode = -1;      // a cell whose node no road reaches (set before Build)

    static float Centre(int i) { return (i + 0.5f) * kCityCellSize; }
    int Node(int x, int z) const { return z * side + x; }
    void Build(int s, uint32_t peoplePerCell, float roadKmh = 10.0f) {
        side = s, people = peoplePerCell, kmh = roadKmh;
        table = LinkTable();
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                const V2 a{Centre(x), Centre(z)};
                if (Node(x, z) == skipNode) continue;
                if (x + 1 < side && Node(x + 1, z) != skipNode) table.AddStraight(Node(x, z), Node(x + 1, z), a, {Centre(x + 1), Centre(z)}, kmh, 800), table.AddStraight(Node(x + 1, z), Node(x, z), {Centre(x + 1), Centre(z)}, a, kmh, 800);
                if (z + 1 < side && Node(x, z + 1) != skipNode) table.AddStraight(Node(x, z), Node(x, z + 1), a, {Centre(x), Centre(z + 1)}, kmh, 800), table.AddStraight(Node(x, z + 1), Node(x, z), {Centre(x), Centre(z + 1)}, a, kmh, 800);
            }
        table.nodes = side * side;
        table.BuildAdjacency();
        graph = std::make_shared<const SvcGraph>(SvcGraph::From(table));
    }
    // A site at cell (x, z) of the catalog building (tab, name, workplaces, vehicles, w, d) with funding f.
    uint32_t AddSite(int x, int z, int tab, const char* name, int workplaces, int vehicles, float f = 1.0f, int w = 3, int d = 3) {
        SvcSite s;
        s.id = 100000u + (uint32_t)sites.size();
        s.spec = SvcFunded(SvcSpecFor(tab, name, workplaces, vehicles, w, d), f);
        s.x = Centre(x), s.z = Centre(z), s.node = Node(x, z);
        sites.push_back(s);
        return s.id;
    }
    SvcCovIn Input() const {
        SvcCovIn in;
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                SvcCellIn c;
                c.key = {x, z}, c.x = Centre(x), c.z = Centre(z), c.node = Node(x, z), c.people = people;
                in.cells.push_back(c);
            }
        in.sites = sites, in.graph = graph;
        return in;
    }
    std::vector<SvcCellStat> Stats(uint32_t homesPerCell) const {
        std::vector<SvcCellStat> st((size_t)(side * side));
        for (SvcCellStat& s : st) s.residents = people, s.homes = homesPerCell, s.jobs = people / 4;
        return st;
    }
    // A world with one home per cell (ids 1 .. side^2) and the coverage applied.
    void Into(SvcWorld& w, uint32_t homesPerCell) const {
        std::vector<SvcBldg> b;
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                SvcBldg h;
                h.id = (uint32_t)(Node(x, z) + 1), h.key = {x, z}, h.node = Node(x, z), h.kind = kBkHome, h.homes = (uint16_t)homesPerCell;
                h.jobs = (uint16_t)(people / 4), h.lot = h.id;
                b.push_back(h);
            }
        for (const SvcSite& s : sites) { // the service buildings themselves
            SvcBldg h;
            h.id = s.id, h.key = CellKeyAt(s.x, s.z), h.node = s.node, h.kind = kBkService;
            b.push_back(h);
        }
        SvcCovIn in = Input();
        SvcCovOut out;
        SvcCover(in, out);
        SvcSetBuildings(w, std::move(b));
        SvcApplyCoverage(w, std::move(in), std::move(out), Stats(homesPerCell));
    }
};

// A neutral factor table for side^2 cells (unemployment about 5 %, as in a healthy city).
inline CellFactors Neutral(int cells) {
    CellFactors f;
    f.Fit((size_t)cells);
    for (CellFactor& c : f.cells) c.unemployment = 13;
    return f;
}

} // namespace svctest
