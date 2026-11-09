// ------------------------------------------------------------------- the example map: "Bayside", a bay archipelago with organic streets
// Included by main.cpp after city_game.inl. Terrain is CityMap::BayHeight (mapKind 3). The streets are grown, not drawn: points are
// scattered on the land (denser downtown), joined by a Gabriel graph (the irregular blocks of an old town), bent into gentle curves,
// with the busiest links widened and bridges thrown between the land masses. Two highways come in from the map edge.
// Zones are painted along the streets by district, and the water plants stand INLAND (they do not need a shore: an outfall pipe
// runs under the road). Start it with TBD_MAP=example, or g_gameAction = 4.
// [example map] It ships as the default Save 1 (assets_src/saves_template/bayside.*, copied by game/save_slots.h on first
// run); TBD_MAP=example TBD_EXAMPLE_SAVE=<seconds> rebuilds the template (game/title_host.inl).
#include <functional>
#include <queue>
#include <random>
#include <map>
#include <set>

namespace example_map {
using tbd::CityMap;
using tbd::city::V2;

struct Pt {
    V2 p;
    int mass = 0;           // 0 mainland, 1 north island, 2 downtown island, 3 south-east island, 4 west islet, 5 north-east islet
    std::vector<int> nb;    // neighbours (graph)
    int node = -1;          // road network node
    int comp = -1;
};
struct Edge {
    int a, b;
    bool arterial = false;
    bool collector = false;
};

inline float DowntownDist(V2 p) { return tbd::city::Len(p - V2{160.0f, -170.0f}); }
inline int MassOf(float x, float z, int seed) {
    const float sdM = CityMap::BayMainland(x, z, seed), sdI = CityMap::BayIslands(x, z, seed);
    if (sdM >= sdI) return 0;
    const float e[5] = {CityMap::Ell(x, z, -120.0f, -880.0f, 400.0f, 230.0f, -0.25f), CityMap::Ell(x, z, 160.0f, -170.0f, 560.0f, 330.0f, 0.35f),
                        CityMap::Ell(x, z, 800.0f, 500.0f, 270.0f, 190.0f, 0.5f), CityMap::Ell(x, z, -700.0f, -650.0f, 160.0f, 110.0f, 0.2f),
                        CityMap::Ell(x, z, 900.0f, -950.0f, 110.0f, 90.0f, 0.0f)};
    int best = 0;
    for (int i = 1; i < 5; ++i)
        if (e[i] < e[best]) best = i;
    return best + 1;
}
// Where streets may be: dry ground, off the beach, not steep, and (on the mainland) only a band along the bay.
inline bool Buildable(float x, float z, int seed) {
    if (std::fabs(x) > 2150.0f || z < -1900.0f || z > 1900.0f) return false;
    const float sdM = CityMap::BayMainland(x, z, seed), sdI = CityMap::BayIslands(x, z, seed);
    const float sd = std::max(sdM, sdI);
    if (sd < 38.0f) return false;
    if (sdM > sdI && sdM > 430.0f) return false;
    const float h = CityMap::BayHeight(x, z, seed);
    if (h < 2.0f) return false;
    const float hx = CityMap::BayHeight(x + 24.0f, z, seed), hz = CityMap::BayHeight(x, z + 24.0f, seed);
    return std::fabs(hx - h) < 4.5f && std::fabs(hz - h) < 4.5f;
}
inline float Spacing(float x, float z) {
    const float d = DowntownDist({x, z});
    return d < 480.0f ? 62.0f : 62.0f + std::min(1.0f, (d - 480.0f) / 900.0f) * 48.0f;
}
inline bool Dry(V2 a, V2 b, int seed) { // the straight link stays on ground that is not steep
    const float L = tbd::city::Len(b - a);
    float prev = CityMap::BayHeight(a.x, a.z, seed);
    for (float d = 14.0f; d < L; d += 14.0f) {
        const V2 q = tbd::city::Lerp2(a, b, d / L);
        const float h = CityMap::BayHeight(q.x, q.z, seed);
        if (h < 1.6f || std::fabs(h - prev) > 2.4f) return false;
        prev = h;
    }
    return true;
}

struct BayLayout {
    std::vector<Pt> pts;
    std::vector<Edge> edges;
};

inline BayLayout Grow(int seed) {
    using namespace tbd::city;
    std::mt19937 rng((uint32_t)seed * 2654435761u + 17u);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    BayLayout L;
    // ---- dart throwing with a radius that grows away from downtown
    const float cell = 40.0f;
    std::unordered_map<long long, std::vector<int>> grid;
    auto key = [&](int gx, int gz) { return ((long long)gx << 32) ^ (long long)(unsigned)gz; };
    auto gcoord = [&](float v) { return (int)std::floor(v / cell); };
    for (int tries = 0; tries < 260000; ++tries) {
        const float x = -2150.0f + U(rng) * 4300.0f, z = -1900.0f + U(rng) * 3800.0f;
        if (!Buildable(x, z, seed)) continue;
        const float r = Spacing(x, z);
        bool ok = true;
        const int g0 = gcoord(x), g1 = gcoord(z), reach = (int)std::ceil(r * 1.25f / cell);
        for (int dz = -reach; dz <= reach && ok; ++dz)
            for (int dx = -reach; dx <= reach && ok; ++dx) {
                const auto it = grid.find(key(g0 + dx, g1 + dz));
                if (it == grid.end()) continue;
                for (int j : it->second) {
                    const float rr = 0.5f * (r + Spacing(L.pts[j].p.x, L.pts[j].p.z));
                    if (Len(L.pts[j].p - V2{x, z}) < rr) { ok = false; break; }
                }
            }
        if (!ok) continue;
        Pt p;
        p.p = {x, z};
        p.mass = MassOf(x, z, seed);
        grid[key(g0, g1)].push_back((int)L.pts.size());
        L.pts.push_back(p);
    }
    // ---- Gabriel graph: i-j is a street when no other point lies inside the circle on i-j as its diameter
    auto nearby = [&](V2 c, float r, auto&& fn) {
        const int g0 = gcoord(c.x), g1 = gcoord(c.z), reach = (int)std::ceil(r / cell);
        for (int dz = -reach; dz <= reach; ++dz)
            for (int dx = -reach; dx <= reach; ++dx) {
                const auto it = grid.find(key(g0 + dx, g1 + dz));
                if (it != grid.end())
                    for (int j : it->second) fn(j);
            }
    };
    for (size_t i = 0; i < L.pts.size(); ++i) {
        nearby(L.pts[i].p, 220.0f, [&](int j) {
            if (j <= (int)i || L.pts[j].mass != L.pts[i].mass) return;
            const V2 a = L.pts[i].p, b = L.pts[j].p;
            const float len = Len(b - a), maxLen = 1.75f * std::max(Spacing(a.x, a.z), Spacing(b.x, b.z));
            if (len > maxLen) return;
            const V2 mid = (a + b) * 0.5f;
            bool empty = true;
            nearby(mid, len, [&](int k) {
                if (k == (int)i || k == j || !empty) return;
                if (Len(L.pts[k].p - mid) < len * 0.5f * 0.98f) empty = false;
            });
            if (!empty || !Dry(a, b, seed)) return;
            L.edges.push_back({(int)i, j});
        });
    }
    auto rebuildNb = [&] {
        for (Pt& p : L.pts) p.nb.clear();
        for (const Edge& e : L.edges) L.pts[e.a].nb.push_back(e.b), L.pts[e.b].nb.push_back(e.a);
    };
    // ---- drop dead-end stubs (twice), then keep only well-connected pieces
    for (int pass = 0; pass < 2; ++pass) {
        rebuildNb();
        std::vector<Edge> keep;
        for (const Edge& e : L.edges)
            if (L.pts[e.a].nb.size() > 1 && L.pts[e.b].nb.size() > 1) keep.push_back(e);
        L.edges = keep;
    }
    rebuildNb();
    { // thin the triangle mesh into blocks: drop links at busy junctions when the two ends stay joined some other way
        std::vector<int> order(L.edges.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<char> dead(L.edges.size(), 0);
        for (int ei : order) {
            const Edge& ed = L.edges[ei];
            if (L.pts[ed.a].nb.size() <= 2 || L.pts[ed.b].nb.size() <= 2) continue;
            if (L.pts[ed.a].nb.size() <= 3 && L.pts[ed.b].nb.size() <= 3) continue;
            if ((L.pts[ed.a].nb.size() <= 3 || L.pts[ed.b].nb.size() <= 3) && U(rng) < 0.5f) continue;
            // is b still reachable from a without this link (within a short loop)?
            std::vector<int> st{ed.a}, seen{ed.a};
            bool joined = false;
            for (size_t h = 0; h < st.size() && !joined && st.size() < 60; ++h)
                for (int v : L.pts[st[h]].nb) {
                    if ((st[h] == ed.a && v == ed.b) || (st[h] == ed.b && v == ed.a)) continue;
                    if (v == ed.b) { joined = true; break; }
                    if (std::find(seen.begin(), seen.end(), v) == seen.end()) seen.push_back(v), st.push_back(v);
                }
            if (!joined) continue;
            dead[ei] = 1;
            auto drop = [&](int x, int y) { auto& n = L.pts[x].nb; n.erase(std::find(n.begin(), n.end(), y)); };
            drop(ed.a, ed.b), drop(ed.b, ed.a);
        }
        std::vector<Edge> kept;
        for (size_t i = 0; i < L.edges.size(); ++i)
            if (!dead[i]) kept.push_back(L.edges[i]);
        L.edges = kept;
    }
    rebuildNb();
    { // no five-way (or worse) junctions: shed the longest links at busy nodes while the network stays joined
        auto joinedWithout = [&](int a, int b) {
            std::vector<int> st{a}, seen{a};
            for (size_t h = 0; h < st.size() && st.size() < 90; ++h)
                for (int v : L.pts[st[h]].nb) {
                    if ((st[h] == a && v == b) || (st[h] == b && v == a)) continue;
                    if (v == b) return true;
                    if (std::find(seen.begin(), seen.end(), v) == seen.end()) seen.push_back(v), st.push_back(v);
                }
            return false;
        };
        for (int pass = 0; pass < 3; ++pass)
            for (size_t i = 0; i < L.pts.size(); ++i) {
                while (L.pts[i].nb.size() > 4) {
                    int bestE = -1;
                    float bl = -1;
                    for (size_t k = 0; k < L.edges.size(); ++k) {
                        const Edge& ed = L.edges[k];
                        if (ed.a != (int)i && ed.b != (int)i) continue;
                        const float len = Len(L.pts[ed.a].p - L.pts[ed.b].p);
                        if (len > bl && L.pts[ed.a].nb.size() > 2 && L.pts[ed.b].nb.size() > 2 && joinedWithout(ed.a, ed.b)) bl = len, bestE = (int)k;
                    }
                    if (bestE < 0) break;
                    L.edges.erase(L.edges.begin() + bestE);
                    rebuildNb();
                }
            }
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<Edge> kept;
            for (const Edge& ed : L.edges)
                if (L.pts[ed.a].nb.size() > 1 && L.pts[ed.b].nb.size() > 1) kept.push_back(ed);
            L.edges = kept;
            rebuildNb();
        }
    }
    rebuildNb();
    int nc = 0;
    std::vector<int> compSize;
    for (size_t i = 0; i < L.pts.size(); ++i) {
        if (L.pts[i].comp >= 0 || L.pts[i].nb.empty()) continue;
        std::vector<int> st{(int)i};
        L.pts[i].comp = nc;
        int n = 0;
        while (!st.empty()) {
            const int c = st.back();
            st.pop_back();
            ++n;
            for (int j : L.pts[c].nb)
                if (L.pts[j].comp < 0) L.pts[j].comp = nc, st.push_back(j);
        }
        compSize.push_back(n);
        ++nc;
    }
    for (Pt& p : L.pts)
        if (p.comp >= 0 && compSize[p.comp] < 10) p.comp = -1;
    std::vector<Edge> keep;
    for (const Edge& e : L.edges)
        if (L.pts[e.a].comp >= 0 && L.pts[e.b].comp >= 0) keep.push_back(e);
    L.edges = keep;
    rebuildNb();
    return L;
}
} // namespace example_map

// Builds the Bayside example city: streets, bridges, highways, zones, utilities. Terrain must be mapKind 3 already.
void BuildExampleCity(int seed) {
    using namespace tbd::city;
    using namespace example_map;
    RoadsInit();
    g_exampleSegZone.clear();
    Network& net = g_roads.net;
    const int pSmall = ProfileIndex("Two-Lane Road"), pMed = ProfileIndex("Four-Lane Road"), pDiv = ProfileIndex("Four-Lane Divided Road"), pSix = ProfileIndex("Six-Lane Divided Road"), pRampHw = ProfileIndex("Two-Lane Two-Way Highway");
    const int pHwy = ProfileIndex("Three-Lane Two-Way Highway");
    BayLayout L = Grow(seed);
    std::vector<Pt>& pts = L.pts;
    const auto& P = Profiles();
    // nodes
    for (Pt& p : pts)
        if (p.comp >= 0) p.node = net.AddNode({p.p.x, RoadsGround(p.p.x, p.p.z), p.p.z}, true);
    // ---- hubs and arterials: join the hubs of each piece along the shortest street paths
    std::vector<std::vector<int>> hubsOfComp;
    int ncomp = 0;
    for (const Pt& p : pts) ncomp = std::max(ncomp, p.comp + 1);
    hubsOfComp.resize(ncomp);
    auto nearestPt = [&](V2 t, int mass) { // closest street point to t (on the given land mass, or any with mass < 0)
        int best = -1;
        float bd = 1e30f;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (pts[i].comp < 0 || (mass >= 0 && pts[i].mass != mass)) continue;
            const float d = Len(pts[i].p - t);
            if (d < bd) bd = d, best = (int)i;
        }
        return best;
    };
    // ---- bridges: between the pieces, shortest links first (a spanning set), then a few extra loops
    struct Cand { int a, b; float len; };
    std::vector<Cand> cands;
    {
        std::vector<int> bestA(ncomp * ncomp, -1), bestB(ncomp * ncomp, -1);
        std::vector<float> bestD(ncomp * ncomp, 1e30f);
        for (size_t i = 0; i < pts.size(); ++i) {
            if (pts[i].comp < 0) continue;
            for (size_t j = i + 1; j < pts.size(); ++j) {
                if (pts[j].comp < 0 || pts[j].comp == pts[i].comp) continue;
                const float d = Len(pts[i].p - pts[j].p);
                if (d > 1150.0f) continue;
                const int k = std::min(pts[i].comp, pts[j].comp) * ncomp + std::max(pts[i].comp, pts[j].comp);
                if (d < bestD[k]) bestD[k] = d, bestA[k] = (int)i, bestB[k] = (int)j;
            }
        }
        for (int k = 0; k < ncomp * ncomp; ++k)
            if (bestA[k] >= 0) {
                // a bridge needs open water between its ends
                const V2 a = pts[bestA[k]].p, b = pts[bestB[k]].p;
                int wet = 0;
                for (int s = 1; s < 20; ++s) {
                    const V2 q = Lerp2(a, b, s / 20.0f);
                    wet += CityMap::BayHeight(q.x, q.z, seed) < 0.0f;
                }
                if (wet >= 5) cands.push_back({bestA[k], bestB[k], bestD[k]});
            }
        std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.len < y.len; });
    }
    std::vector<int> dsu(ncomp);
    for (int i = 0; i < ncomp; ++i) dsu[i] = i;
    std::function<int(int)> find = [&](int x) { return dsu[x] == x ? x : dsu[x] = find(dsu[x]); };
    std::vector<Cand> bridges;
    int extra = 0;
    for (const Cand& c : cands) {
        const int ra = find(pts[c.a].comp), rb = find(pts[c.b].comp);
        if (ra != rb) dsu[ra] = rb, bridges.push_back(c);
        else if (extra < 5 && c.len < 900.0f) bridges.push_back(c), ++extra;
    }
    std::vector<std::pair<int, int>> bridgeSegs;
    for (const Cand& c : bridges) {
        const V2 a = pts[c.a].p, b = pts[c.b].p;
        const float len = Len(b - a), ya = net.nodes[pts[c.a].node].p.y, yb = net.nodes[pts[c.b].node].p.y;
        const float clear = len > 320.0f ? 13.0f : 9.0f;
        const int pieces = len > 560.0f ? 6 : len > 320.0f ? 4 : 2;
        std::vector<int> ids{pts[c.a].node};
        std::vector<V2> ps{a};
        for (int s = 1; s < pieces; ++s) {
            const float t = (float)s / pieces;
            const V2 q = Lerp2(a, b, t);
            const float y = ya + (yb - ya) * t + clear * std::pow(std::sin(3.14159265f * t), 0.55f);
            ids.push_back(net.AddNode({q.x, y, q.z}, false));
            ps.push_back(q);
        }
        ids.push_back(pts[c.b].node);
        ps.push_back(b);
        const V2 T = Norm(b - a);
        for (size_t k = 0; k + 1 < ids.size(); ++k) {
            const float Lk = Len(ps[k + 1] - ps[k]);
            net.AddSeg(ids[k], ids[k + 1], ps[k] + T * (Lk / 3.0f), ps[k + 1] - T * (Lk / 3.0f), pDiv);
        }
        bridgeSegs.push_back({c.a, c.b});
        // power across the water on pylons
        g_util.AddLine(Util::Electricity, a + T * 20.0f, b - T * 20.0f, 20.0f);
        hubsOfComp[pts[c.a].comp].push_back(c.a);
        hubsOfComp[pts[c.b].comp].push_back(c.b);
    }
    // hubs: the heart of each piece, plus the bridge landings
    for (int c = 0; c < ncomp; ++c) {
        int best = -1;
        float bd = 1e30f;
        V2 sum{0, 0};
        int n = 0;
        for (const Pt& p : pts)
            if (p.comp == c) sum = sum + p.p, ++n;
        if (!n) continue;
        const V2 mean = sum * (1.0f / n);
        for (size_t i = 0; i < pts.size(); ++i)
            if (pts[i].comp == c && Len(pts[i].p - mean) < bd) bd = Len(pts[i].p - mean), best = (int)i;
        hubsOfComp[c].insert(hubsOfComp[c].begin(), best);
    }
    // Dijkstra from each piece's first hub, mark the paths to its other hubs
    for (int c = 0; c < ncomp; ++c) {
        if (hubsOfComp[c].empty()) continue;
        const int src = hubsOfComp[c][0];
        std::vector<float> dist(pts.size(), 1e30f);
        std::vector<int> prev(pts.size(), -1);
        std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>, std::greater<>> q;
        dist[src] = 0, q.push({0.0f, src});
        while (!q.empty()) {
            const auto [d, u] = q.top();
            q.pop();
            if (d > dist[u]) continue;
            for (int v : pts[u].nb) {
                const float nd = d + Len(pts[u].p - pts[v].p);
                if (nd < dist[v]) dist[v] = nd, prev[v] = u, q.push({nd, v});
            }
        }
        std::set<std::pair<int, int>> art;
        for (size_t h = 1; h < hubsOfComp[c].size(); ++h)
            for (int v = hubsOfComp[c][h]; prev[v] >= 0; v = prev[v]) art.insert({std::min(v, prev[v]), std::max(v, prev[v])});
        for (Edge& e : L.edges)
            if (art.count({std::min(e.a, e.b), std::max(e.a, e.b)})) e.arterial = true;
    }
    // collectors: every ~450 m cell sends its most central street to the nearest arterial along the street graph
    {
        std::vector<float> dist(pts.size(), 1e30f);
        std::vector<int> prev(pts.size(), -1);
        std::vector<char> onArt(pts.size(), 0);
        std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>, std::greater<>> q;
        for (const Edge& ed : L.edges)
            if (ed.arterial) onArt[ed.a] = onArt[ed.b] = 1;
        for (size_t i = 0; i < pts.size(); ++i)
            if (onArt[i]) dist[i] = 0, q.push({0.0f, (int)i});
        while (!q.empty()) {
            const auto [d, u] = q.top();
            q.pop();
            if (d > dist[u]) continue;
            for (int v : pts[u].nb) {
                const float nd = d + Len(pts[u].p - pts[v].p);
                if (nd < dist[v]) dist[v] = nd, prev[v] = u, q.push({nd, v});
            }
        }
        std::map<std::pair<int, int>, std::pair<float, int>> cellBest;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (pts[i].comp < 0 || onArt[i] || dist[i] > 1e29f) continue;
            const int cx = (int)std::floor(pts[i].p.x / 450.0f), cz = (int)std::floor(pts[i].p.z / 450.0f);
            const float dc = Len(pts[i].p - V2{(cx + 0.5f) * 450.0f, (cz + 0.5f) * 450.0f});
            auto it = cellBest.find({cx, cz});
            if (it == cellBest.end() || dc < it->second.first) cellBest[{cx, cz}] = {dc, (int)i};
        }
        std::set<std::pair<int, int>> col;
        for (const auto& kv : cellBest)
            for (int v = kv.second.second; prev[v] >= 0; v = prev[v]) col.insert({std::min(v, prev[v]), std::max(v, prev[v])});
        for (Edge& ed : L.edges)
            if (!ed.arterial && col.count({std::min(ed.a, ed.b), std::max(ed.a, ed.b)})) ed.collector = true;
    }
    // ---- the streets: a cubic per link, bent a little; tangents agree across two-way nodes so the curves flow
    std::mt19937 rng((uint32_t)seed * 31u + 7u);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);
    auto tangentOut = [&](int node, int toward) { // unit direction leaving `node` toward `toward`
        const Pt& p = pts[node];
        const V2 d = Norm(pts[toward].p - p.p);
        if (p.nb.size() == 2) {
            V2 t = Norm(pts[p.nb[1]].p - pts[p.nb[0]].p);
            if (Dot2(t, d) < 0) t = t * -1.0f;
            return Norm(d * 0.35f + t * 0.65f);
        }
        const float a = U(rng) * 0.16f, c = std::cos(a), s = std::sin(a);
        return V2{d.x * c - d.z * s, d.x * s + d.z * c};
    };
    std::vector<int> streetSeg(L.edges.size(), -1);
    for (size_t k = 0; k < L.edges.size(); ++k) {
        const Edge& e = L.edges[k];
        const V2 a = pts[e.a].p, b = pts[e.b].p;
        const float len = Len(b - a);
        const V2 ta = tangentOut(e.a, e.b), tb = tangentOut(e.b, e.a);
        int prof = e.collector ? pMed : pSmall;
        if (e.arterial) prof = DowntownDist(a) < 420.0f && DowntownDist(b) < 420.0f ? pSix : pDiv;
        streetSeg[k] = net.AddSeg(pts[e.a].node, pts[e.b].node, a + ta * (len / 3.0f), b + tb * (len / 3.0f), prof);
    }
    // ---- the through highway: map edge to map edge, west mainland -> over the bay -> across downtown on a viaduct -> over the bay -> east
    // mainland. Streets pass beneath it; a few ramps tie it to the arterials.
    std::vector<int> hwNodes;
    std::vector<V2> hwPos;
    std::vector<float> hwUp;
    {
        const V2 ctrl[] = {{-3700.0f, CityMap::BayHwyWestZ(-3700.0f)}, {-2700.0f, CityMap::BayHwyWestZ(-2700.0f)}, {-2060.0f, CityMap::BayHwyWestZ(-2060.0f)},
                           {-1760.0f, -285.0f}, {-1000.0f, -250.0f}, {-250.0f, -330.0f}, {160.0f, -170.0f}, {560.0f, -30.0f}, {1000.0f, 150.0f},
                           {1700.0f, 430.0f}, {2060.0f, CityMap::BayHwyEastZ(2060.0f)}, {2700.0f, CityMap::BayHwyEastZ(2700.0f)}, {3700.0f, CityMap::BayHwyEastZ(3700.0f)}};
        const int nc = (int)(sizeof(ctrl) / sizeof(ctrl[0]));
        auto cr = [](V2 p0, V2 p1, V2 p2, V2 p3, float t) {
            const float t2 = t * t, t3 = t2 * t;
            return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
        };
        for (int i = 0; i + 1 < nc; ++i) {
            const V2 p0 = ctrl[std::max(0, i - 1)], p1 = ctrl[i], p2 = ctrl[i + 1], p3 = ctrl[std::min(nc - 1, i + 2)];
            const int n = std::max(1, (int)std::ceil(Len(p2 - p1) / 150.0f));
            for (int s = 0; s < n; ++s) hwPos.push_back(cr(p0, p1, p2, p3, (float)s / n));
        }
        hwPos.push_back(ctrl[nc - 1]);
        for (const V2& p : hwPos) {
            const float g = RoadsGround(p.x, p.z);
            const bool wet = g < 1.0f;
            const float up = wet ? 14.0f : 9.0f * std::clamp((2350.0f - std::fabs(p.x)) / 350.0f, 0.0f, 1.0f);
            const bool lifted = wet || up > 0.8f;
            hwUp.push_back(lifted ? up : 0.0f);
            hwNodes.push_back(net.AddNode({p.x, wet ? 14.0f : g + (lifted ? up : 0.0f), p.z}, !lifted));
        }
        for (size_t k = 0; k + 1 < hwNodes.size(); ++k) {
            const V2 ta = Norm(hwPos[std::min(k + 1, hwPos.size() - 1)] - hwPos[k == 0 ? 0 : k - 1]);
            const V2 tb = Norm(hwPos[std::min(k + 2, hwPos.size() - 1)] - hwPos[k]);
            const float Lk = Len(hwPos[k + 1] - hwPos[k]);
            net.AddSeg(hwNodes[k], hwNodes[k + 1], hwPos[k] + ta * (Lk / 3.0f), hwPos[k + 1] - tb * (Lk / 3.0f), pHwy);
        }
        g_transit.AddOutside(TransportContext(), OutKind::Road, hwPos.front(), "Westmere");
        g_transit.AddOutside(TransportContext(), OutKind::Road, hwPos.back(), "Otherville");
        // ramps: arterial junctions 60-220 m from a lifted stretch, at least 650 m apart
        std::vector<V2> used;
        int ramps = 0;
        for (size_t i = 0; i < pts.size() && ramps < 8; ++i) {
            bool onArt = false;
            for (const Edge& ed : L.edges)
                if (ed.arterial && (ed.a == (int)i || ed.b == (int)i)) onArt = true;
            if (!onArt) continue;
            int bk = -1;
            float bd = 1e30f;
            for (size_t k = 0; k < hwPos.size(); ++k)
                if (hwUp[k] > 7.0f && hwUp[k] < 10.0f && Len(hwPos[k] - pts[i].p) < bd) bd = Len(hwPos[k] - pts[i].p), bk = (int)k;
            if (bk < 0 || bd < 60.0f || bd > 220.0f) continue;
            bool spaced = true;
            for (const V2& u : used) spaced &= Len(u - pts[i].p) > 650.0f;
            if (!spaced) continue;
            used.push_back(pts[i].p);
            const V2 hp = hwPos[bk], sp = pts[i].p, m = Lerp2(hp, sp, 0.5f), T = Norm(sp - hp);
            const int mid = net.AddNode({m.x, RoadsGround(m.x, m.z) + 4.5f, m.z}, false);
            const float h2 = Len(sp - hp) * 0.5f;
            net.AddSeg(hwNodes[bk], mid, hp + T * (h2 / 3.0f), m - T * (h2 / 3.0f), pRampHw);
            net.AddSeg(mid, pts[i].node, m + T * (h2 / 3.0f), sp - T * (h2 / 3.0f), pRampHw);
            ++ramps;
        }
        tbd::Log("Bayside: through highway %zu nodes, %d ramps", hwNodes.size(), ramps);
    }
    g_transit.AddOutside(TransportContext(), OutKind::Sea, {260.0f, -3300.0f}, "Northport");
    g_transit.AddOutside(TransportContext(), OutKind::Sea, {-300.0f, -3500.0f}, "Farhaven");
    net.version++;

    // ---- zones: painted along every street, by district. Each pending cell snaps to the zone grid once it has been rebuilt.
    struct RoadPt { V2 p; float half; };
    std::unordered_map<long long, std::vector<RoadPt>> rg;
    auto rkey = [](float x, float z) { return ((long long)std::floor(x / 24.0f) << 32) ^ (long long)(unsigned)(int)std::floor(z / 24.0f); };
    for (size_t s = 0; s < net.segs.size(); ++s) {
        const Cubic c = net.Curve((int)s);
        const float half = net.Prof((int)s).Half();
        for (float t = 0; t <= 1.0f; t += 0.04f) {
            const V2 p = c.At(t);
            rg[rkey(p.x, p.z)].push_back({p, half});
        }
    }
    auto clearOfRoads = [&](V2 p, int self) {
        (void)self;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = rg.find(rkey(p.x + dx * 24.0f, p.z + dz * 24.0f));
                if (it == rg.end()) continue;
                for (const RoadPt& r : it->second)
                    if (Len(r.p - p) < r.half + 6.0f) return false;
            }
        return true;
    };
    int painted = 0;
    for (size_t k = 0; k < L.edges.size(); ++k) {
        const Edge& e = L.edges[k];
        const int seg = streetSeg[k];
        if (seg < 0) continue;
        const V2 mid = (pts[e.a].p + pts[e.b].p) * 0.5f;
        const int mass = pts[e.a].mass;
        const float dt = DowntownDist(mid);
        const float rnd = std::fabs(std::sin((float)k * 12.9898f + (float)seed)) ;
        if (rnd > 0.88f) continue; // a few streets stay green
        uint8_t zone;
        if (mass == 2 && dt < 250.0f) zone = e.arterial ? 4 : 5;                       // downtown core: shops on the avenues, offices behind
        else if (mass == 2 && dt < 470.0f) zone = e.arterial ? 4 : 3;                  // inner ring: high-density flats
        else if (mass == 2) zone = rnd < 0.5f ? 2 : 1;
        else if (mass == 0 && mid.x > 250.0f && mid.z > 550.0f) zone = 6;               // the south-east shore: industry and the docks' hinterland
        else if (mass == 1) zone = e.arterial ? 4 : (rnd < 0.5f ? 2 : 1);
        else zone = e.arterial ? (rnd < 0.3f ? 4 : 2) : (rnd < 0.3f ? 2 : rnd < 0.65f ? 1 : 0);
        const int depthCells = zone == 3 || zone == 5 ? 4 : (mass == 0 ? 2 : 3);
        if ((size_t)seg >= g_exampleSegZone.size()) g_exampleSegZone.resize((size_t)seg + 1, {kNoZone, 0});
        g_exampleSegZone[seg] = {zone, (uint8_t)depthCells};
        ++painted;
    }
    tbd::Log("Bayside: %zu street points, %zu streets, %zu bridges, %d zone cells queued, %zu nodes, %zu segments", pts.size(), L.edges.size(), bridges.size(), painted, net.nodes.size(), net.segs.size());

    // ---- utilities: pipes and cables along the arterials (and across the bridges), plants INLAND beside them. The water plants
    // do not need a shore: the outfall is a buried pipe, so they go where the land is cheap and quiet.
    for (const Edge& e : L.edges) {
        if (!e.arterial) continue;
        const V2 a = pts[e.a].p, b = pts[e.b].p;
        g_util.AddLine(Util::Electricity, a, b, 0.0f);
        g_util.AddLine(Util::Water, a, b, -3.0f);
        g_util.AddLine(Util::Sewage, a, b, -3.0f);
    }
    auto place = [&](const char* name, V2 target, int mass) {
        int def = -1;
        for (size_t i = 0; i < UtilDefs().size(); ++i)
            if (std::string(UtilDefs()[i].name) == name) def = (int)i;
        if (def < 0) return;
        std::vector<std::pair<float, int>> ranked; // arterial links on that land mass, nearest to the target first
        for (size_t k = 0; k < L.edges.size(); ++k) {
            const Edge& e = L.edges[k];
            if (!e.arterial || (mass >= 0 && pts[e.a].mass != mass)) continue;
            ranked.push_back({Len((pts[e.a].p + pts[e.b].p) * 0.5f - target), (int)k});
        }
        std::sort(ranked.begin(), ranked.end());
        for (size_t r = 0; r < ranked.size() && r < 12; ++r) {
            const Edge& e = L.edges[ranked[r].second];
            const V2 a = pts[e.a].p, b = pts[e.b].p, d = Norm(b - a);
            for (float off = 30.0f; off < 90.0f; off += 20.0f)
                for (int side = -1; side <= 1; side += 2)
                    for (float f : {0.5f, 0.3f, 0.7f}) {
                        const V2 at = Lerp2(a, b, f) + LeftOf(d) * ((float)side * off);
                        const Placement p = g_util.Plan(UtilContext(), def, at, 0, true);
                        if (!p.valid) continue;
                        const int i = g_util.Place(p, def);
                        if (i >= 0) UtilLevel(g_util.items[i]), g_util.items[i].y = RoadsGround(p.c.x, p.c.z);
                        tbd::Log("Bayside: %s at (%.0f, %.0f)", name, p.c.x, p.c.z);
                        return;
                    }
        }
        tbd::Log("Bayside: could not place %s near (%.0f, %.0f)", name, target.x, target.z);
    };
    TransportRefreshZoning();
    place("Wastewater Treatment Plant", {-1750.0f, 350.0f}, 0);   // inland on the west mainland, no shore
    place("Wastewater Treatment Plant", {1500.0f, 950.0f}, 0);    // south-east, by the industry
    place("Wastewater Treatment Plant", {-60.0f, -880.0f}, 1);    // the north island's own, in the middle of it
    place("Wastewater Treatment Plant", {420.0f, -60.0f}, 2);     // downtown island, tucked inland
    place("Large Water Pumping Station", {-1600.0f, -100.0f}, 0);
    place("Water Pumping Station", {700.0f, 520.0f}, 3);
    place("Water Tower", {200.0f, -260.0f}, 2);
    place("Water Tower", {-100.0f, -820.0f}, 1);
    place("Gas Power Plant", {1700.0f, 700.0f}, 0);
    place("Solar Power Station", {-1300.0f, 900.0f}, 0);
    place("Wind Turbine", {1800.0f, -400.0f}, 0);
    place("Wind Turbine", {-1800.0f, -500.0f}, 0);
}

// The starter town: the zones fill in right away (the way a stress city does) so the map opens lived in; the rest grows with demand.
void ExampleFound() {
    // Phase 1 paints and fills downtown and the north island, so the starter town is a town and not scattered houses; phase 2 paints
    // the rest of the map's zones, which the sim then grows on demand.
    auto paint = [&](bool core) {
        int painted = 0;
        for (size_t i = 0; i < g_zones.cells.size(); ++i) {
            const tbd::city::ZoneCell& c = g_zones.cells[i];
            if (c.seg < 0 || (size_t)c.seg >= g_exampleSegZone.size()) continue;
            const auto& sz = g_exampleSegZone[c.seg];
            if (sz.first == tbd::city::kNoZone || c.depth >= sz.second || c.zone != tbd::city::kNoZone) continue;
            const bool inCore = example_map::DowntownDist(c.c) < 620.0f || tbd::city::Len(c.c - tbd::city::V2{-120.0f, -880.0f}) < 480.0f;
            if (inCore != core) continue;
            if (g_zones.Paint((int)i, sz.first)) ++painted;
        }
        return painted;
    };
    const int core = paint(true);
    const bool gate = g_zones.gateSpawns;
    g_zones.gateSpawns = false;
    const size_t before = g_zones.lots.size();
    int fails = 0;
    while (g_zones.lots.size() - before < 1500 && fails < 600) {
        if (g_zones.SpawnNow()) fails = 0;
        else ++fails;
    }
    g_zones.gateSpawns = gate;
    for (size_t i = before; i < g_zones.lots.size(); ++i) g_zones.lots[i].progress = std::max(g_zones.lots[i].progress, 1.0f);
    const int rest = paint(false);
    tbd::Log("Bayside: starter town of %zu lots (%d core cells), %d more zone cells painted to grow", g_zones.lots.size() - before, core, rest);
}

// A fresh Bayside map: the archipelago terrain, grown streets, a clean treasury.
void NewExampleGame(int seed) {
    g_engine.terrain.params.enabled = true;
    g_engine.terrain.params.flatStart = false;
    g_engine.terrain.params.mapKind = 3;
    g_engine.terrain.params.seed = seed;
    g_engine.terrain.field.Clear();
    SetCitySun(); // as NewCityGame: under Ultra's MAX look, kept under it (game_core.inl)
    ClearCityWorld();
    g_clock.minutes = 8 * 60;
    BuildExampleCity(seed);
    g_exampleFounding = true;
    if (tbd::platform::GetEnvA("TBD_GROW", nullptr, 0) > 0) g_clock.speed = 2, g_clock.paused = false; // captures: let the zones grow

    g_engine.camera.target = {100.0f, 0.0f, -150.0f};
    g_engine.camera.distance = 3000.0f;
    g_engine.camera.pitch = 1.35f;
    g_engine.camera.yaw = 0.0f;
    Toast("EXAMPLE MAP - BAYSIDE");
    tbd::Log("Example map Bayside, seed %d", seed);
}
