// Headless checks for neighbourhoods (src/city/sim_hoods.h):
//   every occupied cell is in exactly one hood and hoods stay within the size cap; the same roads and lots give the same
//   hoods and names; a large road splits the hoods it runs through; roll-up totals equal the sim's; the hood commute links
//   keep the sim's link total; local-first matching fills the own hood first, spills to neighbours by travel time, and
//   leaves the rest unmet.
// Build and run: tests\sim_hoods_test.bat
#include <cstdio>
#include <set>
#include <vector>

#include "city/road_tool.h"
#include "city/sim_hoods.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static Lot MakeLot(uint64_t id, float x, float z, int type, int w, int d) {
    Lot l;
    l.id = id;
    l.c = {x, z};
    l.type = type, l.w = w, l.d = d;
    l.path = "T" + std::to_string(type) + "_" + std::to_string(w) + "x" + std::to_string(d) + "_L1.glb";
    l.progress = 2.0f;
    return l;
}
static int Seg(Network& net, int a, int b, int prof) {
    const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
    return net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
}

struct World {
    Network net;
    CitySim sim;
    SegGrid grid;
    std::vector<Lot> lots;
};
// 12 x 5 cells; the west 6 columns are housing, the east 6 offices; a six-lane road runs north-south between column 5 and 6
static void MakeWorld(World& w, bool withRoad) {
    uint64_t id = 1;
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 12; ++x) w.lots.push_back(MakeLot(id++, (x + 0.5f) * 128.0f, (z + 0.5f) * 128.0f, x < 6 ? 3 : 5, x < 6 ? 4 : 6, x < 6 ? 4 : 6));
    w.sim.Sync(w.lots, 1);
    const int local = ProfileIndex("Two-Lane Road");
    const int a = w.net.AddNode({0, 0, 64}, true), b = w.net.AddNode({1536, 0, 64}, true);
    Seg(w.net, a, b, local);
    if (withRoad) {
        const int n0 = w.net.AddNode({6 * 128.0f, 0, -100}, true), n1 = w.net.AddNode({6 * 128.0f, 0, 740}, true);
        Seg(w.net, n0, n1, ProfileIndex("Six-Lane Road"));
    }
    ++w.net.version;
    w.grid.Build(w.net);
}

static bool Spans(const World& w, const SimHoods& h) {
    for (const Hood& x : h.hoods) {
        bool west = false, east = false;
        for (int c : x.cells) (w.sim.cells.cells[(size_t)c].key.x < 6 ? west : east) = true;
        if (west && east) return true;
    }
    return false;
}

int main() {
    // ---- partition, cap, determinism
    World w1, w2;
    MakeWorld(w1, true);
    MakeWorld(w2, true);
    SimHoods h1, h2;
    h1.Build(w1.sim, w1.net, w1.grid);
    h2.Build(w2.sim, w2.net, w2.grid);
    CHECK(!h1.hoods.empty());
    std::vector<int> count(w1.sim.cells.cells.size(), 0);
    for (const Hood& h : h1.hoods) {
        CHECK((int)h.cells.size() <= h1.maxCells);
        for (int c : h.cells) {
            ++count[(size_t)c];
            CHECK(h1.cellHood[(size_t)c] == h.id);
        }
    }
    for (size_t c = 0; c < count.size(); ++c) CHECK(count[c] == 1);
    CHECK(h1.hoods.size() == h2.hoods.size());
    for (size_t i = 0; i < h1.hoods.size() && i < h2.hoods.size(); ++i) {
        CHECK(h1.hoods[i].name == h2.hoods[i].name);
        CHECK(h1.hoods[i].cells == h2.hoods[i].cells);
        CHECK(h1.hoods[i].neighbours == h2.hoods[i].neighbours);
    }
    std::set<std::string> names;
    for (const Hood& h : h1.hoods) {
        names.insert(h.name);
        CHECK(!h.name.empty());
    }
    CHECK(names.size() == h1.hoods.size());
    for (const Hood& h : h1.hoods) // neighbours are symmetric and never include self
        for (int n : h.neighbours) {
            CHECK(n != h.id);
            const auto& nb = h1.hoods[(size_t)n].neighbours;
            CHECK(std::find(nb.begin(), nb.end(), h.id) != nb.end());
        }
    CHECK(!h1.Stale(w1.sim, w1.net));
    ++w1.net.version;
    CHECK(h1.Stale(w1.sim, w1.net));
    --w1.net.version;

    // ---- the large road is a wall: with it no hood spans both sides; without it one does; water walls too
    CHECK(!Spans(w1, h1));
    {
        World nw;
        MakeWorld(nw, false);
        SimHoods h;
        h.Build(nw.sim, nw.net, nw.grid);
        CHECK(Spans(nw, h));
        SimHoods hw;
        hw.Build(nw.sim, nw.net, nw.grid, [](V2 p) { return std::fabs(p.x - 6 * 128.0f) < 1.0f; });
        CHECK(!Spans(nw, hw));
    }
    for (const Hood& h : h1.hoods) CHECK(h.centroidNode >= 0); // a road node to route from

    // ---- roll-up equals the sim's totals
    h1.RollUp(w1.sim);
    uint64_t pop = 0, workers = 0, jobs = 0, emp = 0;
    for (const Hood& h : h1.hoods) pop += h.Population(), workers += h.workers, jobs += h.Jobs(), emp += h.employed;
    CHECK(pop == w1.sim.Population() && pop > 0);
    CHECK(workers == w1.sim.Working() && workers > 0);
    CHECK(jobs == w1.sim.JobCap());
    CHECK(emp == w1.sim.Employed());

    // ---- hood links keep the link total, and housing sends workers to the office hoods
    std::vector<HoodLink> links;
    h1.LinksFromSuper(w1.sim, links);
    double total = 0;
    for (const HoodLink& l : links) {
        total += l.n;
        CHECK(h1.hoods[(size_t)l.from].pop[kAdults] > 0 && h1.hoods[(size_t)l.to].Jobs() > 0);
    }
    CHECK(std::fabs(total - (double)w1.sim.Links()) < 0.5 + 1e-4 * total);

    // ---- local-first matching on a chain of three hoods 0 - 1 - 2 (0 and 2 are not neighbours)
    {
        std::vector<Hood> hs(3);
        for (int i = 0; i < 3; ++i) hs[(size_t)i].id = i, hs[(size_t)i].centre = {i * 1000.0f, 0};
        hs[0].neighbours = {1}, hs[1].neighbours = {0, 2}, hs[2].neighbours = {1};
        MatchResult r;
        MatchLocalFirst(hs, {4, 0, 0}, {4, 5, 0}, nullptr, r); // own first: hood 1's jobs are not touched
        CHECK(r.assign.size() == 1 && r.assign[0].from == 0 && r.assign[0].to == 0 && r.assign[0].n == 4);
        CHECK(r.unmet[0] == 0);
        MatchLocalFirst(hs, {8, 0, 0}, {0, 5, 2}, nullptr, r); // spill: ring 1, then ring 2 (hood 2), the rest unmet
        float to1 = 0, to2 = 0;
        for (const HoodLink& l : r.assign) (l.to == 1 ? to1 : to2) += l.n;
        CHECK(to1 == 5 && to2 == 2 && r.unmet[0] == 1);
        std::vector<Hood> star(3); // nearest by travel time first inside a ring
        for (int i = 0; i < 3; ++i) star[(size_t)i].id = i;
        star[0].neighbours = {1, 2}, star[1].neighbours = {0}, star[2].neighbours = {0};
        MatchLocalFirst(star, {3, 0, 0}, {0, 2, 2}, [](int, int b) { return b == 2 ? 1.0f : 9.0f; }, r);
        float f1 = 0, f2 = 0;
        for (const HoodLink& l : r.assign) (l.to == 1 ? f1 : f2) += l.n;
        CHECK(f2 == 2 && f1 == 1 && r.unmet[0] == 0);
        // a low id does not eat its neighbour's own supply: every hood fills locally before any spills
        MatchLocalFirst(hs, {3, 3, 0}, {0, 3, 3}, nullptr, r);
        float own1 = 0;
        for (const HoodLink& l : r.assign)
            if (l.from == 1 && l.to == 1) own1 += l.n;
        CHECK(own1 == 3);
        CHECK(r.unmet[0] == 0 && r.unmet[1] == 0); // hood 0 reaches hood 2 at ring 2
        float used = 0, un = 0; // conservation: assigned + unmet == demand
        for (const HoodLink& l : r.assign) used += l.n;
        for (float u : r.unmet) un += u;
        CHECK(std::fabs(used + un - 6.0f) < 1e-4f);
    }

    if (failures == 0) std::printf("sim_hoods_test: all checks passed (%zu hoods)\n", h1.hoods.size());
    return failures ? 1 : 0;
}
