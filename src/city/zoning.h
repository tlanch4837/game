// Zoning, modelled on Cities: Skylines II: every ordinary road gets 8 m zone cells on both sides, up to 6 deep, along
// its whole length. Cells are painted with a zone type (Residential low density / row housing / medium / high,
// Commercial, Office, Industrial); painted cells grow buildings on lots picked from the building catalog, with a bias
// for bigger lots. Platform free: the host feeds the road network, the catalog and game time, and draws the results.
//
// Overlap rule (blocks between parallel roads, junction corners, curves): all candidate cells of all roads are sorted
// shallow first (depth 0 = on the road), wider roads first within a depth, then accepted one by one. A candidate is
// dropped when any of its points lies on a road surface or its square overlaps an already accepted cell (spatial hash
// + oriented-square SAT test); once a column (road, side, column) loses a cell, everything behind it goes too, so a
// block's cells always connect to their road and a block between two roads splits evenly down the middle.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "road_network.h"

namespace tbd {
namespace city {

enum class ZoneCat : uint8_t { Residential, Commercial, Office, Industrial, Count };
struct ZoneType {
    const char* name;
    ZoneCat cat;
    const char* dir;    // assets/buildings/<dir>/glb
    const char* prefix; // file prefix, e.g. LD_2x3_L1.glb
    Vec3 color;
    bool pitched; // low density homes: the timber frame + pitched roof construction; the rest get scaffold + crane
};
inline const std::vector<ZoneType>& ZoneTypes() {
    static const std::vector<ZoneType> v = {
        {"Low Density Housing", ZoneCat::Residential, "low_density", "LD", {0.50f, 0.88f, 0.38f}, true},
        {"Row Housing", ZoneCat::Residential, "row_housing", "RH", {0.33f, 0.76f, 0.30f}, false},
        {"Medium Density Housing", ZoneCat::Residential, "medium_apartments", "MD", {0.20f, 0.62f, 0.26f}, false},
        {"High Density Housing", ZoneCat::Residential, "high_density", "HD", {0.10f, 0.46f, 0.20f}, false},
        {"Commercial", ZoneCat::Commercial, "low_commercial", "CL", {0.32f, 0.64f, 0.96f}, false},
        {"Office", ZoneCat::Office, "low_office", "OL", {0.64f, 0.42f, 0.92f}, false},
        {"Industrial", ZoneCat::Industrial, "industry", "IN", {0.96f, 0.80f, 0.20f}, false},
    };
    return v;
}
inline const char* ZoneCatName(ZoneCat c) {
    static const char* const k[] = {"Residential", "Commercial", "Office", "Industrial"};
    return k[(int)c];
}

constexpr uint8_t kNoZone = 255;
constexpr float kCell = 8.0f;
constexpr int kZoneDepth = 6;

struct ZoneCell {
    V2 c, dir, out; // centre; along the road; away from the road
    int seg = -1, side = 0, col = 0, depth = 0;
    uint8_t zone = kNoZone;
    int lot = -1;
    float y[4] = {}; // ground at the corners (c -dir -out, c +dir -out, c +dir +out, c -dir +out)
    float yc = 0;
};
struct BuildingModel {
    int type, w, d, level;
    std::string path;
};
struct Lot {
    uint64_t id = 0;
    std::vector<int> cells;
    V2 c, front; // front: the way the building's +Z (its street face) points, toward the road
    float y = 0;
    int w = 0, d = 0, type = 0;
    std::string path;
    float progress = 0, duration = 60; // game minutes
    bool alive = true;
    bool Built() const { return progress >= 1.0f; }
};

class ZoneGrid {
public:
    std::vector<ZoneCell> cells;
    std::vector<Lot> lots;
    std::vector<BuildingModel> models;
    uint64_t version = 1;           // changes whenever cells, zones or lots change (the host redraws the overlay)
    uint64_t builtFrom = ~0ull;     // the road network version the cells were made from
    std::vector<uint64_t> removed;  // lots demolished since the host last emptied this (it deletes their buildings)
    int maxConstructing = 8;
    std::mt19937 rng{20261106};

    // ---------------------------------------------------------------- cells from the road network
    void Rebuild(const Network& net, const GroundFn& ground) {
        builtFrom = net.version;
        struct Cand {
            ZoneCell cell;
            float roadW;
        };
        std::vector<Cand> cand;
        std::vector<std::array<float, 4>> boxes(net.segs.size()); // seg AABB grown by its half width
        for (size_t s = 0; s < net.segs.size(); ++s) {
            boxes[s] = {1e30f, 1e30f, -1e30f, -1e30f};
            if (!net.segs[s].alive) continue;
            const Cubic c = net.Curve((int)s);
            const float h = net.Prof((int)s).Half() + 0.5f;
            for (V2 p : {c.p0, c.p1, c.p2, c.p3})
                boxes[s] = {std::min(boxes[s][0], p.x - h), std::min(boxes[s][1], p.z - h), std::max(boxes[s][2], p.x + h), std::max(boxes[s][3], p.z + h)};
        }
        for (size_t s = 0; s < net.segs.size(); ++s) {
            const Segment& sg = net.segs[s];
            if (!sg.alive) continue;
            const Profile& p = net.Prof((int)s);
            if (p.cat == Cat::Highway || p.cat == Cat::Path || p.fwd + p.back == 0) continue;
            const Cubic c = net.Curve((int)s);
            const V2 mid = c.At(0.5f);
            if (ground && std::fabs(net.YAt(sg, 0.5f) - ground(mid.x, mid.z)) > 2.0f) continue; // bridges / tunnels
            // arc length table
            constexpr int N = 64;
            float acc[N + 1];
            acc[0] = 0;
            V2 prev = c.p0;
            for (int i = 1; i <= N; ++i) {
                const V2 q = c.At((float)i / N);
                acc[i] = acc[i - 1] + Len(q - prev);
                prev = q;
            }
            const float L = acc[N];
            const int n = (int)std::floor(L / kCell);
            if (n < 1) continue;
            const float s0 = (L - n * kCell) * 0.5f;
            for (int i = 0; i < n; ++i) {
                const float a = s0 + kCell * (i + 0.5f);
                int k = 1;
                while (k < N && acc[k] < a) ++k;
                const float t = ((k - 1) + (a - acc[k - 1]) / std::max(1e-4f, acc[k] - acc[k - 1])) / N;
                const V2 P = c.At(t);
                V2 T = c.Tan(t);
                const float tl = Len(T);
                if (tl < 1e-5f) continue;
                T = T * (1.0f / tl);
                for (int side = 0; side < 2; ++side) {
                    const V2 out = LeftOf(T) * (side ? -1.0f : 1.0f);
                    for (int d = 0; d < kZoneDepth; ++d) {
                        Cand cd;
                        cd.cell.c = P + out * (p.Half() + kCell * (d + 0.5f));
                        cd.cell.dir = T, cd.cell.out = out;
                        cd.cell.seg = (int)s, cd.cell.side = side, cd.cell.col = i, cd.cell.depth = d;
                        cd.roadW = p.width;
                        cand.push_back(cd);
                    }
                }
            }
        }
        std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
            if (a.cell.depth != b.cell.depth) return a.cell.depth < b.cell.depth;
            return a.roadW > b.roadW;
        });
        auto onRoad = [&](V2 q) {
            for (size_t s = 0; s < net.segs.size(); ++s) {
                const auto& b = boxes[s];
                if (q.x < b[0] || q.z < b[1] || q.x > b[2] || q.z > b[3]) continue;
                float t, d;
                net.Nearest((int)s, q, t, d);
                if (d < net.Prof((int)s).Half() + 0.1f) return true;
            }
            return false;
        };
        std::vector<ZoneCell> fresh;
        std::unordered_map<uint64_t, std::vector<int>> hash;
        std::unordered_set<uint64_t> blocked;
        for (const Cand& cd : cand) {
            const ZoneCell& z = cd.cell;
            const uint64_t colKey = ColKey(z.seg, z.side, z.col);
            if (blocked.count(colKey)) continue;
            bool ok = true;
            const float e = kCell * 0.5f - 0.4f;
            const V2 pts[5] = {z.c, z.c + z.dir * e + z.out * e, z.c - z.dir * e + z.out * e, z.c + z.dir * e - z.out * e, z.c - z.dir * e - z.out * e};
            for (const V2& q : pts)
                if (onRoad(q)) { ok = false; break; }
            if (ok) {
                const int bx = Bucket(z.c.x), bz = Bucket(z.c.z);
                for (int dz = -1; dz <= 1 && ok; ++dz)
                    for (int dx = -1; dx <= 1 && ok; ++dx) {
                        auto it = hash.find(HKey(bx + dx, bz + dz));
                        if (it == hash.end()) continue;
                        for (int j : it->second)
                            if (Overlap(z, fresh[j])) { ok = false; break; }
                    }
            }
            if (!ok) {
                blocked.insert(colKey);
                continue;
            }
            hash[HKey(Bucket(z.c.x), Bucket(z.c.z))].push_back((int)fresh.size());
            fresh.push_back(z);
        }
        for (ZoneCell& z : fresh) {
            const float h = kCell * 0.5f;
            const V2 q[4] = {z.c - z.dir * h - z.out * h, z.c + z.dir * h - z.out * h, z.c + z.dir * h + z.out * h, z.c - z.dir * h + z.out * h};
            for (int k = 0; k < 4; ++k) z.y[k] = ground ? ground(q[k].x, q[k].z) : 0.0f;
            z.yc = ground ? ground(z.c.x, z.c.z) : 0.0f;
        }
        // carry zones and lots over from the old cells (same place, same orientation)
        std::vector<int> map(cells.size(), -1);
        {
            std::unordered_map<uint64_t, std::vector<int>> fh;
            for (size_t i = 0; i < fresh.size(); ++i) fh[HKey(Bucket(fresh[i].c.x), Bucket(fresh[i].c.z))].push_back((int)i);
            for (size_t o = 0; o < cells.size(); ++o) {
                const ZoneCell& oc = cells[o];
                const int bx = Bucket(oc.c.x), bz = Bucket(oc.c.z);
                float best = 2.0f;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto it = fh.find(HKey(bx + dx, bz + dz));
                        if (it == fh.end()) continue;
                        for (int j : it->second) {
                            const float d = Len(fresh[j].c - oc.c);
                            if (d < best && std::fabs(Dot2(fresh[j].dir, oc.dir)) > 0.9f) best = d, map[o] = j;
                        }
                    }
                if (map[o] >= 0) fresh[map[o]].zone = oc.zone;
            }
        }
        for (Lot& l : lots) {
            if (!l.alive) continue;
            bool keep = true;
            std::vector<int> moved;
            for (int ci : l.cells) {
                const int ni = ci >= 0 && ci < (int)map.size() ? map[ci] : -1;
                if (ni < 0 || fresh[ni].lot >= 0 || fresh[ni].zone != l.type) { keep = false; break; }
                moved.push_back(ni);
            }
            if (keep) {
                l.cells = moved;
                for (int ni : moved) fresh[ni].lot = (int)(&l - lots.data());
            } else {
                l.alive = false;
                removed.push_back(l.id);
            }
        }
        cells = std::move(fresh);
        hash_.clear();
        colMap_.clear();
        for (size_t i = 0; i < cells.size(); ++i) {
            hash_[HKey(Bucket(cells[i].c.x), Bucket(cells[i].c.z))].push_back((int)i);
            colMap_[ColKey(cells[i].seg, cells[i].side, cells[i].col) * 8 + cells[i].depth] = (int)i;
        }
        Compact();
        ++version;
    }

    // ---------------------------------------------------------------- queries
    int CellAt(V2 p) const {
        auto it = hash_.find(HKey(Bucket(p.x), Bucket(p.z)));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                it = hash_.find(HKey(Bucket(p.x) + dx, Bucket(p.z) + dz));
                if (it == hash_.end()) continue;
                for (int j : it->second) {
                    const V2 d = p - cells[j].c;
                    if (std::fabs(Dot2(d, cells[j].dir)) <= kCell * 0.5f && std::fabs(Dot2(d, cells[j].out)) <= kCell * 0.5f) return j;
                }
            }
        return -1;
    }
    template <class F> void CellsNear(V2 p, float r, F&& f) const {
        const int k = (int)std::ceil(r / kCell) + 1, bx = Bucket(p.x), bz = Bucket(p.z);
        for (int dz = -k; dz <= k; ++dz)
            for (int dx = -k; dx <= k; ++dx) {
                auto it = hash_.find(HKey(bx + dx, bz + dz));
                if (it == hash_.end()) continue;
                for (int j : it->second) f(j);
            }
    }

    // ---------------------------------------------------------------- painting (zone = kNoZone erases)
    bool Paint(int cell, uint8_t zone) {
        if (cell < 0 || cells[cell].zone == zone) return false;
        if (cells[cell].lot >= 0) Demolish(cells[cell].lot);
        cells[cell].zone = zone;
        ++version;
        return true;
    }
    bool Brush(V2 p, float radius, uint8_t zone) {
        bool any = false;
        CellsNear(p, radius, [&](int j) {
            if (Len(cells[j].c - p) <= radius) any |= Paint(j, zone);
        });
        return any;
    }
    // Paint bucket: the connected cells that share the clicked cell's zone (a block is bounded by its roads).
    bool Fill(V2 p, uint8_t zone) {
        const int start = CellAt(p);
        if (start < 0 || cells[start].zone == zone) return false;
        const uint8_t from = cells[start].zone;
        std::vector<int> stack{start};
        std::vector<char> seen(cells.size(), 0);
        seen[start] = 1;
        std::vector<int> fill;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            fill.push_back(i);
            CellsNear(cells[i].c, kCell, [&](int j) {
                if (!seen[j] && cells[j].zone == from && Len(cells[j].c - cells[i].c) < kCell * 1.2f) {
                    seen[j] = 1;
                    stack.push_back(j);
                }
            });
        }
        for (int i : fill) Paint(i, zone);
        return true;
    }
    // Cells whose centre lies in the rectangle from a to b, measured along `dir` (the road under the press) and across it.
    void BoxCells(V2 a, V2 b, V2 dir, std::vector<int>& out) const {
        out.clear();
        const V2 n = LeftOf(dir), d = b - a;
        const float u = Dot2(d, dir), v = Dot2(d, n);
        const float u0 = std::min(0.0f, u) - 1.0f, u1 = std::max(0.0f, u) + 1.0f, v0 = std::min(0.0f, v) - 1.0f, v1 = std::max(0.0f, v) + 1.0f;
        const V2 mid = (a + b) * 0.5f;
        CellsNear(mid, Len(d) * 0.5f + kCell, [&](int j) {
            const V2 q = cells[j].c - a;
            const float qu = Dot2(q, dir), qv = Dot2(q, n);
            if (qu >= u0 && qu <= u1 && qv >= v0 && qv <= v1) out.push_back(j);
        });
    }
    bool Box(V2 a, V2 b, V2 dir, uint8_t zone) {
        std::vector<int> v;
        BoxCells(a, b, dir, v);
        bool any = false;
        for (int j : v) any |= Paint(j, zone);
        return any;
    }

    void Demolish(int li) {
        Lot& l = lots[li];
        if (!l.alive) return;
        l.alive = false;
        for (int ci : l.cells)
            if (ci >= 0 && ci < (int)cells.size() && cells[ci].lot == li) cells[ci].lot = -1;
        removed.push_back(l.id);
        ++version;
    }

    // ---------------------------------------------------------------- growth (dt in game minutes)
    void Tick(float minutes) {
        int building = 0;
        for (Lot& l : lots)
            if (l.alive && !l.Built()) {
                const bool was = l.Built();
                l.progress = std::min(1.0f, l.progress + minutes / l.duration);
                if (!was && l.Built()) ++version;
                building += l.Built() ? 0 : 1;
            }
        spawnTimer_ -= minutes;
        int tries = 0;
        while (spawnTimer_ < 0 && tries++ < 8) {
            spawnTimer_ += 1.5f;
            if (building < maxConstructing && TrySpawn()) ++building;
        }
        if (spawnTimer_ < 0) spawnTimer_ = 0;
    }

    static float ConstructMinutes(const BuildingModel& m) { return ZoneTypes()[m.type].pitched ? 50.0f : 45.0f + 4.0f * m.w * m.d; }

private:
    std::unordered_map<uint64_t, std::vector<int>> hash_;
    std::unordered_map<uint64_t, int> colMap_;
    float spawnTimer_ = 0;
    uint64_t nextLot_ = 1;

    static int Bucket(float v) { return (int)std::floor(v / kCell); }
    static uint64_t HKey(int x, int z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; }
    static uint64_t ColKey(int seg, int side, int col) { return ((uint64_t)seg << 24) | ((uint64_t)side << 20) | (uint64_t)(col & 0xFFFFF); }
    static bool Overlap(const ZoneCell& a, const ZoneCell& b) {
        const float h = kCell * 0.5f - 0.25f; // touching edges and float noise are not an overlap
        const V2 d = b.c - a.c;
        for (V2 u : {a.dir, a.out, b.dir, b.out}) {
            const float ra = h * (std::fabs(Dot2(a.dir, u)) + std::fabs(Dot2(a.out, u)));
            const float rb = h * (std::fabs(Dot2(b.dir, u)) + std::fabs(Dot2(b.out, u)));
            if (std::fabs(Dot2(d, u)) >= ra + rb) return false;
        }
        return true;
    }
    int At(int seg, int side, int col, int depth) const {
        if (col < 0) return -1;
        auto it = colMap_.find(ColKey(seg, side, col) * 8 + depth);
        return it == colMap_.end() ? -1 : it->second;
    }
    // Drops dead lots, renumbering the cells' references.
    void Compact() {
        std::vector<Lot> keep;
        std::vector<int> remap(lots.size(), -1);
        for (size_t i = 0; i < lots.size(); ++i)
            if (lots[i].alive) remap[i] = (int)keep.size(), keep.push_back(std::move(lots[i]));
        for (ZoneCell& c : cells)
            if (c.lot >= 0) c.lot = c.lot < (int)remap.size() ? remap[c.lot] : -1;
        lots = std::move(keep);
    }
    bool Free(int ci, uint8_t zone) const { return ci >= 0 && cells[ci].zone == zone && cells[ci].lot < 0; }

    bool TrySpawn() {
        std::vector<int> fronts;
        for (size_t i = 0; i < cells.size(); ++i)
            if (cells[i].depth == 0 && cells[i].zone != kNoZone && cells[i].lot < 0) fronts.push_back((int)i);
        if (fronts.empty()) return false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            const ZoneCell& s = cells[fronts[rng() % fronts.size()]];
            const uint8_t zone = s.zone;
            // free depth of each column from s rightwards (and one step back left, so lots can start a cell earlier)
            int start = s.col;
            while (start > s.col - 2 && Free(At(s.seg, s.side, start - 1, 0), zone)) --start;
            int depthOf[6] = {};
            int maxW = 0;
            for (int k = 0; k < 6; ++k) {
                int d = 0;
                while (d < kZoneDepth && Free(At(s.seg, s.side, start + k, d), zone)) ++d;
                if (d == 0) break;
                depthOf[k] = d;
                maxW = k + 1;
            }
            if (maxW == 0) continue;
            std::vector<const BuildingModel*> fit;
            std::vector<float> weight;
            for (const BuildingModel& m : models) {
                if (m.type != zone || m.w > maxW) continue;
                int d = kZoneDepth;
                for (int k = 0; k < m.w; ++k) d = std::min(d, depthOf[k]);
                if (m.d > d) continue;
                fit.push_back(&m);
                weight.push_back(std::pow((float)(m.w * m.d), 1.5f) * (m.level == 1 ? 1.0f : 0.35f)); // bigger lots win more often
            }
            if (fit.empty()) continue;
            std::discrete_distribution<int> pick(weight.begin(), weight.end());
            const BuildingModel& m = *fit[pick(rng)];
            Lot l;
            l.id = nextLot_++;
            l.w = m.w, l.d = m.d, l.type = m.type, l.path = m.path;
            l.duration = ConstructMinutes(m);
            V2 sum{};
            float ysum = 0;
            for (int k = 0; k < m.w; ++k)
                for (int d = 0; d < m.d; ++d) {
                    const int ci = At(s.seg, s.side, start + k, d);
                    l.cells.push_back(ci);
                    sum = sum + cells[ci].c;
                    ysum += cells[ci].yc;
                }
            const float inv = 1.0f / (float)l.cells.size();
            l.c = sum * inv;
            l.y = ysum * inv;
            l.front = cells[l.cells.front()].out * -1.0f;
            const int li = (int)lots.size();
            for (int ci : l.cells) cells[ci].lot = li;
            lots.push_back(std::move(l));
            ++version;
            return true;
        }
        return false;
    }
};

// Parses "<PREFIX>_<W>x<D>[_kind]_L<level>.glb"; false for anything else.
inline bool ParseModelName(const std::string& file, const char* prefix, int& w, int& d, int& level) {
    const std::string pre = std::string(prefix) + "_";
    if (file.compare(0, pre.size(), pre) != 0) return false;
    if (std::sscanf(file.c_str() + pre.size(), "%dx%d", &w, &d) != 2) return false;
    const size_t l = file.rfind("_L");
    if (l == std::string::npos || std::sscanf(file.c_str() + l + 2, "%d", &level) != 1) return false;
    return w > 0 && d > 0 && w <= 6 && d <= kZoneDepth;
}

} // namespace city
} // namespace tbd
