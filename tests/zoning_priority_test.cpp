// Headless checks for the zoning precedence at junctions (src/city/zoning.h ZoneRoadRank, ZoneGrid::Rebuild):
//   a medium (four-lane) road crossed by a small (two-lane) one: the junction's cells belong to the medium road, no two
//   cells overlap (nothing double claims the ground), the medium road keeps its whole 6-deep strip right up to the
//   small road's edge (its frontage continuous), and the small road's cells begin beyond the medium strip; between two
//   roads of one class the old order is kept (depth first). The rezone path: a small road zoned and built up, then a
//   medium road across it: the paint follows the ground to the medium road's cells, and no built lot goes except the
//   ones the new road is built over. The load path: a saved lot whose cells moved is adopted, not dropped.
// Build and run: tests\zoning_priority_test.bat
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "city/road_tool.h"
#include "city/zoning.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static float Flat(float, float) { return 0.0f; }

// Two zone cells' squares overlap (more than touching): the same separating-axis test the grid uses.
static bool Overlap(const ZoneCell& a, const ZoneCell& b) {
    const float h = kCell * 0.5f - 0.25f;
    const V2 d = b.c - a.c;
    for (V2 u : {a.dir, a.out, b.dir, b.out}) {
        const float ra = h * (std::fabs(Dot2(a.dir, u)) + std::fabs(Dot2(a.out, u)));
        const float rb = h * (std::fabs(Dot2(b.dir, u)) + std::fabs(Dot2(b.out, u)));
        if (std::fabs(Dot2(d, u)) >= ra + rb) return false;
    }
    return true;
}
static int Overlaps(const ZoneGrid& z) {
    int n = 0;
    for (size_t i = 0; i < z.cells.size(); ++i)
        for (size_t j = i + 1; j < z.cells.size(); ++j)
            if (Len(z.cells[i].c - z.cells[j].c) < kCell * 1.5f && Overlap(z.cells[i], z.cells[j])) ++n;
    return n;
}
static bool LotsConsistent(const ZoneGrid& z) {
    for (size_t li = 0; li < z.lots.size(); ++li) {
        if (!z.lots[li].alive) continue;
        for (int ci : z.lots[li].cells)
            if (ci < 0 || ci >= (int)z.cells.size() || z.cells[(size_t)ci].lot != (int)li) return false;
    }
    for (const ZoneCell& c : z.cells)
        if (c.lot >= (int)z.lots.size()) return false;
    return true;
}
static Cat CatOf(const Network& net, const ZoneCell& c) { return net.Prof(c.seg).cat; }

int main() {
    const int medium = ProfileIndex("Four-Lane Road"), small = ProfileIndex("Two-Lane Road");
    CHECK(medium >= 0 && small >= 0);
    const float mh = Profiles()[medium].Half(), sh = Profiles()[small].Half(); // 10 m, 5.5 m
    const float strip = mh + kZoneDepth * kCell;                                // the medium road's zone strip: 58 m from its centre line
    CHECK(ZoneRoadRank(Profiles()[medium]) > ZoneRoadRank(Profiles()[small]));

    // ------------------------------------------------------------------ medium (along x) crossed by small (along z)
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, medium);
        t.BuildRoad(Cubic::Line({0, -250}, {0, 250}), 0, 0, small);
        ZoneGrid z;
        z.Rebuild(t.net, Flat);
        CHECK(!z.cells.empty());
        CHECK(Overlaps(z) == 0); // nothing double claims a square of ground
        int junctionCells = 0, junctionWrong = 0, smallInStrip = 0, mediumFront = 0, mediumShort = 0;
        for (const ZoneCell& c : z.cells) {
            const Cat k = CatOf(t.net, c);
            // the junction: within the medium road's strip and near the small road
            if (std::fabs(c.c.z) < strip - 1.0f && std::fabs(c.c.x) < 70.0f) {
                ++junctionCells;
                if (k != Cat::Medium) ++junctionWrong;
            }
            if (k == Cat::Small && std::fabs(c.c.z) - kCell * 0.5f < strip - 0.5f) ++smallInStrip; // must start beyond the strip
            // the medium road's frontage near the junction: every column clear of the small road is 6 deep
            if (k == Cat::Medium && c.depth == 0 && std::fabs(c.c.x) < 80.0f) {
                ++mediumFront;
                int deepest = 0;
                for (const ZoneCell& d : z.cells)
                    if (d.seg == c.seg && d.side == c.side && d.col == c.col) deepest = std::max(deepest, d.depth);
                if (deepest < kZoneDepth - 1) ++mediumShort;
            }
        }
        std::printf("  medium x small: %zu cells, %d at the junction (%d not the medium road's), %d small cells in the medium strip, "
                    "%d medium front cells near it (%d short of 6 deep)\n",
                    z.cells.size(), junctionCells, junctionWrong, smallInStrip, mediumFront, mediumShort);
        CHECK(junctionCells > 40 && junctionWrong == 0);
        CHECK(smallInStrip == 0);
        CHECK(mediumFront >= 16 && mediumShort == 0);
        // the frontage is continuous: on each side, depth-0 cells every 8 m from the small road's edge outwards
        for (int side = 0; side < 2; ++side)
            for (int sx = -1; sx <= 1; sx += 2) {
                std::vector<float> xs;
                for (const ZoneCell& c : z.cells)
                    if (CatOf(t.net, c) == Cat::Medium && c.depth == 0 && (c.c.z > 0) == (side == 0) && c.c.x * sx > 0 && std::fabs(c.c.x) < 100.0f)
                        xs.push_back(std::fabs(c.c.x));
                std::sort(xs.begin(), xs.end());
                CHECK(!xs.empty() && xs.front() - kCell * 0.5f < sh + kCell); // starts within a cell of the small road's edge
                for (size_t i = 1; i < xs.size(); ++i) CHECK(std::fabs(xs[i] - xs[i - 1] - kCell) < 0.5f);
            }
        // the small road still zones beyond the strip
        int smallCells = 0;
        for (const ZoneCell& c : z.cells) smallCells += CatOf(t.net, c) == Cat::Small;
        CHECK(smallCells > 300);
        // newly painted zoning on the result: a box over the junction paints medium cells, never two cells for one square
        CHECK(z.Box({-40, 12}, {40, 50}, {1, 0}, 0));
        int painted = 0, paintedSmall = 0;
        for (const ZoneCell& c : z.cells)
            if (c.zone == 0) ++painted, paintedSmall += CatOf(t.net, c) == Cat::Small;
        CHECK(painted > 20 && paintedSmall == 0);
    }

    // ------------------------------------------------------------------ equal class: the old order (depth first)
    {
        // a two-lane road crossed by a (wider) three-lane one-way road, both small: the narrow road's front cells still win
        // over the wider road's deeper cells near the junction, as before the precedence rule
        RoadTool t;
        const int wide = ProfileIndex("Three-Lane One-Way Road");
        CHECK(ZoneRoadRank(Profiles()[wide]) == ZoneRoadRank(Profiles()[small]));
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, wide);
        t.BuildRoad(Cubic::Line({0, -250}, {0, 250}), 0, 0, small);
        ZoneGrid z;
        z.Rebuild(t.net, Flat);
        const float wstrip = Profiles()[wide].Half() + kZoneDepth * kCell;
        int narrowInStrip = 0;
        for (const ZoneCell& c : z.cells)
            if (t.net.segs[(size_t)c.seg].profile == small && std::fabs(c.c.z) < wstrip - kCell) ++narrowInStrip;
        std::printf("  small x small: %zu cells, %d of the narrow road's cells inside the wide road's strip (shared, as before)\n", z.cells.size(), narrowInStrip);
        CHECK(narrowInStrip > 0);
        CHECK(Overlaps(z) == 0);
        // and depth first: every depth-0 cell either road could have is there (no front lost to the other road's depth)
        int fronts = 0;
        for (const ZoneCell& c : z.cells) fronts += c.depth == 0;
        CHECK(fronts > 100);
    }

    // ------------------------------------------------------------------ rezone: a built-up small road, then a medium road across it
    {
        RoadTool t;
        t.BuildRoad(Cubic::Line({0, -250}, {0, 250}), 0, 0, small);
        ZoneGrid z;
        z.models.push_back({0, 2, 3, 1, "test.glb"});
        z.maxConstructing = 100000;
        z.Rebuild(t.net, Flat);
        for (size_t i = 0; i < z.cells.size(); ++i) z.Paint((int)i, 0);
        for (int i = 0; i < 600; ++i) z.Tick(5.0f);
        for (int i = 0; i < 60; ++i) z.Tick(10.0f);
        int before = 0;
        for (const Lot& l : z.lots) before += l.alive && l.Built();
        CHECK(before > 30);
        // the lots the medium road will be built over, and the paint, as they stand
        std::vector<uint64_t> spared;
        for (const Lot& l : z.lots)
            if (l.alive && std::fabs(l.c.z) - l.w * kCell * 0.5f > mh + 0.5f) spared.push_back(l.id);
        ZoneGrid saved = z; // for the load path below
        t.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, medium);
        z.Rebuild(t.net, Flat);
        int lost = 0;
        for (uint64_t id : spared) {
            bool alive = false;
            for (const Lot& l : z.lots) alive |= l.id == id && l.alive;
            lost += alive ? 0 : 1;
        }
        int junctionSmall = 0, mediumPainted = 0;
        for (const ZoneCell& c : z.cells) {
            if (std::fabs(c.c.z) < strip - 1.0f && std::fabs(c.c.x) < 50.0f && CatOf(t.net, c) != Cat::Medium) ++junctionSmall;
            if (CatOf(t.net, c) == Cat::Medium && std::fabs(c.c.x) < sh + 3 * kCell && c.zone == 0) ++mediumPainted;
        }
        std::printf("  rezone: %d built lots, %zu clear of the new road, %d of those lost; %d medium cells took the old paint\n", before, spared.size(), lost,
                    mediumPainted);
        CHECK(lost == 0);                 // no built lot goes but the ones under the new road
        CHECK(junctionSmall == 0);        // the junction's cells are the medium road's now
        CHECK(mediumPainted > 10);        // the paint followed the ground to the medium road's cells
        CHECK(Overlaps(z) == 0 && LotsConsistent(z));

        // the load path: a fresh grid on the new roads, the saved paint and lots back by position (city_game.inl)
        ZoneGrid fresh;
        fresh.models = saved.models;
        fresh.Rebuild(t.net, Flat);
        for (const ZoneCell& c : saved.cells) {
            if (c.zone == kNoZone) continue;
            const int j = fresh.NearestCell(c.c, 5.0f);
            if (j >= 0) fresh.Paint(j, c.zone);
        }
        int back = 0, adopted = 0, want = 0;
        for (const Lot& l : saved.lots) {
            if (!l.alive || !(std::fabs(l.c.z) - l.w * kCell * 0.5f > mh + 0.5f)) continue;
            ++want;
            Lot copy = l;
            copy.cells.clear();
            for (int ci : l.cells) copy.cells.push_back(fresh.NearestCell(saved.cells[(size_t)ci].c, 3.0f));
            if (fresh.AddLoadedLot(copy)) ++back;
            else if (fresh.AdoptLoadedLot(copy, t.net)) ++back, ++adopted;
        }
        std::printf("  load: %d of %d saved lots back (%d adopted: their cells moved to the medium road)\n", back, want, adopted);
        CHECK(back == want);
        CHECK(LotsConsistent(fresh));
    }

    if (failures) {
        std::printf("zoning_priority_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("zoning_priority_test: all passed\n");
    return 0;
}
