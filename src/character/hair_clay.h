#pragma once
// Hair Maker clay and comb (docs/hair_maker_spec.md sections 2 and 3): the hair's volume as a sculptable mesh in the head's own
// frame, with a comb direction on every vertex. Sculpt (inflate, deflate, smooth, grab) shapes where the hair is and how far it
// stands off the head; comb strokes set which way it flows. gameenginetools/hair_groom/clay_groom.py grows the cards from it, and
// cap.py decimates and shrinks this exact mesh into the cap under them.
// File (clay.json): {"v": 1, "pos": [x,y,z...], "idx": [...], "comb": [dx,dy,dz...]}, metres, head mesh space.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mesh_import.h"

namespace tbd::character {

class HairClay {
public:
    enum Tool { None = 0, Inflate, Deflate, Smooth, Grab, Comb, ToolCount };
    using V3 = std::array<float, 3>;

    std::vector<V3> pos, nrm, comb;
    std::vector<uint32_t> idx;
    std::vector<std::vector<uint32_t>> ring; // vertex neighbours
    std::string status;

    bool Empty() const { return pos.empty(); }

    bool Load(const std::wstring& path) {
        std::vector<uint8_t> bytes;
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { status = "no clay file"; return false; }
        fseek(f, 0, SEEK_END);
        bytes.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        Json doc;
        if (!JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc)) { status = "clay file is not JSON"; return false; }
        const Json *p = doc.Find("pos"), *i = doc.Find("idx"), *c = doc.Find("comb");
        if (!p || !i || p->a.size() % 3 || i->a.size() % 3) { status = "clay file has no mesh"; return false; }
        const size_t n = p->a.size() / 3;
        pos.resize(n);
        comb.assign(n, V3{0, -1, 0});
        for (size_t k = 0; k < n; ++k) pos[k] = {(float)p->a[k * 3].Num(), (float)p->a[k * 3 + 1].Num(), (float)p->a[k * 3 + 2].Num()};
        if (c && c->a.size() == p->a.size())
            for (size_t k = 0; k < n; ++k) comb[k] = {(float)c->a[k * 3].Num(), (float)c->a[k * 3 + 1].Num(), (float)c->a[k * 3 + 2].Num()};
        idx.resize(i->a.size());
        for (size_t k = 0; k < idx.size(); ++k) { idx[k] = (uint32_t)i->a[k].Num(); if (idx[k] >= n) { pos.clear(); status = "clay index out of range"; return false; } }
        BuildRings();
        Normals();
        undo_.clear();
        status = "clay " + std::to_string(idx.size() / 3) + " triangles";
        return true;
    }

    // The Hair Maker's start: a closed hairline loop drawn on the scalp (mesh space, on the skin) becomes the clay. The skin inside
    // the loop is copied (subdivided twice so it can be sculpted), its edge snapped onto the loop, and it is inflated into a smooth
    // dome `height` metres high that comes down to the scalp exactly at the loop: cartoon hair, touching the head all round.
    // The comb starts flowing out from the crown (the point furthest inside), leaning down. `head`: the head's flat vertices
    // (skin carries alpha 0.5).
    bool FromHairline(const std::vector<V3>& loop, const std::vector<Vertex>& head, float height) {
        if (loop.size() < 8) { status = "hairline loop too short"; return false; }
        // skin: welded positions and normals, and its triangles
        std::vector<V3> P, N;
        std::vector<uint32_t> T;
        {
            std::map<std::tuple<int, int, int>, uint32_t> weld;
            for (size_t k = 0; k + 2 < head.size(); k += 3) {
                if (std::fabs(head[k].a - 0.5f) > 0.1f || std::fabs(head[k + 1].a - 0.5f) > 0.1f || std::fabs(head[k + 2].a - 0.5f) > 0.1f) continue;
                for (int c = 0; c < 3; ++c) {
                    const Vertex& v = head[k + c];
                    const auto key = std::make_tuple((int)std::lround(v.px * 1e5f), (int)std::lround(v.py * 1e5f), (int)std::lround(v.pz * 1e5f));
                    auto it = weld.find(key);
                    if (it == weld.end()) { it = weld.emplace(key, (uint32_t)P.size()).first; P.push_back({v.px, v.py, v.pz}); N.push_back({0, 0, 0}); }
                    N[it->second] = Add(N[it->second], {v.nx, v.ny, v.nz});
                    T.push_back(it->second);
                }
            }
            for (V3& n : N) n = Norm(n);
        }
        if (T.empty()) { status = "the head has no skin"; return false; }
        // inside test: the loop seen along the axis from the skin's centre through the loop's centre
        V3 lc = {0, 0, 0}, sc = {0, 0, 0};
        for (const V3& p : loop) lc = Add(lc, p, 1.0f / loop.size());
        for (const V3& p : P) sc = Add(sc, p, 1.0f / P.size());
        const V3 A = Norm(Sub(lc, sc));
        const V3 U = Norm(Cross(std::fabs(A[1]) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}, A)), W = Cross(A, U);
        float lowest = 1e30f;
        std::vector<std::array<float, 2>> poly;
        for (const V3& p : loop) { const V3 d = Sub(p, sc); poly.push_back({Dot(d, U), Dot(d, W)}); lowest = std::min(lowest, Dot(d, A)); }
        const auto inside = [&](const V3& p) {
            const V3 d = Sub(p, sc);
            if (Dot(d, A) < lowest - 0.004f) return false;
            const float x = Dot(d, U), y = Dot(d, W);
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                if ((poly[i][1] > y) != (poly[j][1] > y) && x < (poly[j][0] - poly[i][0]) * (y - poly[i][1]) / (poly[j][1] - poly[i][1] + 1e-12f) + poly[i][0]) in = !in;
            return in;
        };
        // onto the skin: nearest skin vertex's tangent plane
        const auto onSkin = [&](V3 p, V3* nOut) {
            size_t best = 0;
            float bd = 1e30f;
            for (size_t k = 0; k < P.size(); ++k) { const V3 d = Sub(P[k], p); const float l = Dot(d, d); if (l < bd) { bd = l; best = k; } }
            const V3& n = N[best];
            if (nOut) *nOut = n;
            return Sub(p, {n[0] * Dot(Sub(p, P[best]), n), n[1] * Dot(Sub(p, P[best]), n), n[2] * Dot(Sub(p, P[best]), n)});
        };
        // the skin triangles near or inside the loop, subdivided twice, then only the pieces inside
        std::vector<V3> V;
        std::vector<uint32_t> F;
        {
            std::map<uint32_t, uint32_t> remap;
            for (size_t t = 0; t + 2 < T.size(); t += 3) {
                const V3 c = {(P[T[t]][0] + P[T[t + 1]][0] + P[T[t + 2]][0]) / 3, (P[T[t]][1] + P[T[t + 1]][1] + P[T[t + 2]][1]) / 3, (P[T[t]][2] + P[T[t + 1]][2] + P[T[t + 2]][2]) / 3};
                if (!inside(c) && !inside(P[T[t]]) && !inside(P[T[t + 1]]) && !inside(P[T[t + 2]])) continue;
                for (int e = 0; e < 3; ++e) {
                    auto it = remap.find(T[t + e]);
                    if (it == remap.end()) { it = remap.emplace(T[t + e], (uint32_t)V.size()).first; V.push_back(P[T[t + e]]); }
                    F.push_back(it->second);
                }
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> mids;
            const auto mid = [&](uint32_t a, uint32_t b) {
                const auto key = std::make_pair(std::min(a, b), std::max(a, b));
                auto it = mids.find(key);
                if (it != mids.end()) return it->second;
                V.push_back(onSkin({(V[a][0] + V[b][0]) / 2, (V[a][1] + V[b][1]) / 2, (V[a][2] + V[b][2]) / 2}, nullptr));
                return mids[key] = (uint32_t)V.size() - 1;
            };
            std::vector<uint32_t> F2;
            for (size_t t = 0; t + 2 < F.size(); t += 3) {
                const uint32_t a = F[t], b = F[t + 1], c = F[t + 2], ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
                const uint32_t q[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
                F2.insert(F2.end(), q, q + 12);
            }
            F.swap(F2);
        }
        {
            std::vector<uint32_t> F2;
            for (size_t t = 0; t + 2 < F.size(); t += 3) {
                const V3 c = {(V[F[t]][0] + V[F[t + 1]][0] + V[F[t + 2]][0]) / 3, (V[F[t]][1] + V[F[t + 1]][1] + V[F[t + 2]][1]) / 3, (V[F[t]][2] + V[F[t + 1]][2] + V[F[t + 2]][2]) / 3};
                if (inside(c)) F2.insert(F2.end(), {F[t], F[t + 1], F[t + 2]});
            }
            F.swap(F2);
        }
        if (F.size() < 30) { status = "the loop holds almost no scalp: draw it around the hair"; return false; }
        // compact
        std::vector<int> used(V.size(), -1);
        std::vector<V3> V2;
        for (uint32_t& i : F) { if (used[i] < 0) { used[i] = (int)V2.size(); V2.push_back(V[i]); } i = (uint32_t)used[i]; }
        V.swap(V2);
        pos = V;
        idx = F;
        BuildRings();
        // the open edge snaps onto the loop: the clay's edge IS the hairline
        std::vector<char> edge(pos.size(), 0);
        {
            std::map<std::pair<uint32_t, uint32_t>, int> count;
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
                for (int e = 0; e < 3; ++e) ++count[std::make_pair(std::min(idx[t + e], idx[t + (e + 1) % 3]), std::max(idx[t + e], idx[t + (e + 1) % 3]))];
            for (const auto& [k, c] : count) if (c == 1) edge[k.first] = edge[k.second] = 1;
        }
        const auto toLoop = [&](const V3& p, float* dist) {
            V3 best = loop[0];
            float bd = 1e30f;
            for (size_t i = 0; i < loop.size(); ++i) {
                const V3 &a = loop[i], &b = loop[(i + 1) % loop.size()], ab = Sub(b, a);
                const float t = std::clamp(Dot(Sub(p, a), ab) / std::max(Dot(ab, ab), 1e-12f), 0.0f, 1.0f);
                const V3 q = Add(a, ab, t), d = Sub(p, q);
                const float l = Dot(d, d);
                if (l < bd) { bd = l; best = q; }
            }
            if (dist) *dist = std::sqrt(bd);
            return best;
        };
        for (size_t k = 0; k < pos.size(); ++k) if (edge[k]) pos[k] = onSkin(toLoop(pos[k], nullptr), nullptr);
        for (int it = 0; it < 2; ++it) { // even out the triangles next to the snapped edge
            std::vector<V3> next = pos;
            for (size_t k = 0; k < pos.size(); ++k) {
                if (edge[k] || ring[k].empty()) continue;
                V3 avg = {0, 0, 0};
                for (uint32_t j : ring[k]) avg = Add(avg, pos[j], 1.0f / ring[k].size());
                next[k] = onSkin(Add(pos[k], Sub(avg, pos[k]), 0.5f), nullptr);
            }
            pos.swap(next);
        }
        // inflate: a dome over the scalp, 0 at the loop
        std::vector<float> dist(pos.size()), h(pos.size());
        std::vector<V3> sn(pos.size());
        size_t crown = 0;
        for (size_t k = 0; k < pos.size(); ++k) {
            toLoop(pos[k], &dist[k]);
            onSkin(pos[k], &sn[k]);
            if (dist[k] > dist[crown]) crown = k;
        }
        for (size_t k = 0; k < pos.size(); ++k) {
            const float x = std::clamp(dist[k] / 0.03f, 0.0f, 1.0f);
            h[k] = edge[k] ? 0.0f : height * std::sqrt(x * (2.0f - x)); // a rounded shoulder at the edge, flat on top
        }
        for (int it = 0; it < 4; ++it) {
            std::vector<float> next = h;
            for (size_t k = 0; k < pos.size(); ++k) {
                if (edge[k] || ring[k].empty()) continue;
                float a = 0;
                for (uint32_t j : ring[k]) a += h[j];
                next[k] = 0.5f * h[k] + 0.5f * a / ring[k].size();
            }
            h.swap(next);
        }
        for (size_t k = 0; k < pos.size(); ++k) pos[k] = Add(pos[k], sn[k], h[k]);
        // comb: out from the crown, along the surface, leaning down
        comb.assign(pos.size(), V3{0, -1, 0});
        Normals();
        for (size_t k = 0; k < pos.size(); ++k) {
            V3 d = Add(Norm(Sub(pos[k], pos[crown])), {0, -1, 0}, 0.35f);
            d = Sub(d, {nrm[k][0] * Dot(d, nrm[k]), nrm[k][1] * Dot(d, nrm[k]), nrm[k][2] * Dot(d, nrm[k])});
            comb[k] = Norm(d);
        }
        undo_.clear();
        status = "clay from the hairline: " + std::to_string(idx.size() / 3) + " triangles";
        return true;
    }

    // The painted scalp (the hair painter's density map over the head's UV atlas, N x N, 0..1) becomes the clay: the skin where the
    // mask is over half, subdivided twice so its edge follows the paint, the largest painted region only (brows and stray dabs are
    // not scalp), inflated `height` metres into a smooth dome that comes down onto the scalp at the painted edge: the hairline.
    bool FromMask(const std::vector<Vertex>& head, const std::vector<float>& mask, int N, float height) {
        if ((int)mask.size() != N * N || N < 2) { status = "no painted scalp"; return false; }
        const auto sample = [&](float u, float v) {
            const float x = std::clamp(u * N - 0.5f, 0.0f, (float)N - 1.001f), y = std::clamp(v * N - 0.5f, 0.0f, (float)N - 1.001f);
            const int ix = (int)x, iy = (int)y;
            const float fx = x - ix, fy = y - iy;
            const float* r0 = &mask[(size_t)iy * N], *r1 = &mask[(size_t)std::min(iy + 1, N - 1) * N];
            return (r0[ix] * (1 - fx) + r0[std::min(ix + 1, N - 1)] * fx) * (1 - fy) + (r1[ix] * (1 - fx) + r1[std::min(ix + 1, N - 1)] * fx) * fy;
        };
        // skin: welded positions (one vertex where UV seams meet), normals, and per-corner UVs
        std::vector<V3> P, N3;
        std::vector<uint32_t> F;
        std::vector<std::array<float, 2>> FU; // per triangle corner
        {
            std::map<std::tuple<int, int, int>, uint32_t> weld;
            for (size_t k = 0; k + 2 < head.size(); k += 3) {
                if (std::fabs(head[k].a - 0.5f) > 0.1f || std::fabs(head[k + 1].a - 0.5f) > 0.1f || std::fabs(head[k + 2].a - 0.5f) > 0.1f) continue;
                float m = 0;
                for (int c = 0; c < 3; ++c) m = std::max(m, sample(head[k + c].u, head[k + c].v));
                m = std::max(m, sample((head[k].u + head[k + 1].u + head[k + 2].u) / 3, (head[k].v + head[k + 1].v + head[k + 2].v) / 3));
                for (int c = 0; c < 3; ++c) {
                    const Vertex& v = head[k + c];
                    const auto key = std::make_tuple((int)std::lround(v.px * 1e5f), (int)std::lround(v.py * 1e5f), (int)std::lround(v.pz * 1e5f));
                    auto it = weld.find(key);
                    if (it == weld.end()) { it = weld.emplace(key, (uint32_t)P.size()).first; P.push_back({v.px, v.py, v.pz}); N3.push_back({0, 0, 0}); }
                    N3[it->second] = Add(N3[it->second], {v.nx, v.ny, v.nz});
                    if (m > 0.02f) { F.push_back(it->second); FU.push_back({v.u, v.v}); }
                }
            }
            for (V3& n : N3) n = Norm(n);
        }
        if (F.empty()) { status = "nothing painted: paint the scalp first (3D or 2D)"; return false; }
        const auto onSkin = [&](V3 p, V3* nOut) {
            size_t best = 0;
            float bd = 1e30f;
            for (size_t k = 0; k < P.size(); ++k) { const V3 d = Sub(P[k], p); const float l = Dot(d, d); if (l < bd) { bd = l; best = k; } }
            const V3& n = N3[best];
            if (nOut) *nOut = n;
            const float h = Dot(Sub(p, P[best]), n);
            return Sub(p, {n[0] * h, n[1] * h, n[2] * h});
        };
        // subdivide twice (positions shared through an edge map, UVs per corner), keep the pieces whose centre is painted
        std::vector<V3> V(P);
        for (int pass = 0; pass < 2; ++pass) {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> mids;
            const auto mid = [&](uint32_t a, uint32_t b) {
                const auto key = std::make_pair(std::min(a, b), std::max(a, b));
                if (auto it = mids.find(key); it != mids.end()) return it->second;
                V.push_back(onSkin({(V[a][0] + V[b][0]) / 2, (V[a][1] + V[b][1]) / 2, (V[a][2] + V[b][2]) / 2}, nullptr));
                return mids[key] = (uint32_t)V.size() - 1;
            };
            std::vector<uint32_t> F2;
            std::vector<std::array<float, 2>> U2;
            for (size_t t = 0; t + 2 < F.size(); t += 3) {
                const uint32_t a = F[t], b = F[t + 1], c = F[t + 2], ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
                const auto& ua = FU[t]; const auto& ub = FU[t + 1]; const auto& uc = FU[t + 2];
                const std::array<float, 2> uab = {(ua[0] + ub[0]) / 2, (ua[1] + ub[1]) / 2}, ubc = {(ub[0] + uc[0]) / 2, (ub[1] + uc[1]) / 2}, uca = {(uc[0] + ua[0]) / 2, (uc[1] + ua[1]) / 2};
                const uint32_t q[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
                const std::array<float, 2> qu[12] = {ua, uab, uca, uab, ub, ubc, uca, ubc, uc, uab, ubc, uca};
                F2.insert(F2.end(), q, q + 12);
                U2.insert(U2.end(), qu, qu + 12);
            }
            F.swap(F2);
            FU.swap(U2);
        }
        {
            std::vector<uint32_t> F2;
            for (size_t t = 0; t + 2 < F.size(); t += 3)
                if (sample((FU[t][0] + FU[t + 1][0] + FU[t + 2][0]) / 3, (FU[t][1] + FU[t + 1][1] + FU[t + 2][1]) / 3) > 0.5f) F2.insert(F2.end(), {F[t], F[t + 1], F[t + 2]});
            F.swap(F2);
        }
        // the largest connected painted region
        {
            std::vector<uint32_t> parent(V.size());
            for (uint32_t k = 0; k < parent.size(); ++k) parent[k] = k;
            const auto find = [&](uint32_t k) { while (parent[k] != k) k = parent[k] = parent[parent[k]]; return k; };
            for (size_t t = 0; t + 2 < F.size(); t += 3) { parent[find(F[t + 1])] = find(F[t]); parent[find(F[t + 2])] = find(F[t]); }
            std::map<uint32_t, size_t> size;
            for (size_t t = 0; t + 2 < F.size(); t += 3) ++size[find(F[t])];
            uint32_t best = 0;
            size_t most = 0;
            for (const auto& [r, n] : size) if (n > most) { most = n; best = r; }
            std::vector<uint32_t> F2;
            for (size_t t = 0; t + 2 < F.size(); t += 3) if (find(F[t]) == best) F2.insert(F2.end(), {F[t], F[t + 1], F[t + 2]});
            F.swap(F2);
        }
        if (F.size() < 60) { status = "the painted scalp is too small"; return false; }
        std::vector<int> used(V.size(), -1);
        pos.clear();
        for (uint32_t& i : F) { if (used[i] < 0) { used[i] = (int)pos.size(); pos.push_back(V[i]); } i = (uint32_t)used[i]; }
        idx = F;
        BuildRings();
        // the open edge: smoothed along itself (the paint's staircase gone), kept on the skin
        std::vector<char> edge(pos.size(), 0);
        std::vector<std::vector<uint32_t>> edgeNb(pos.size());
        {
            std::map<std::pair<uint32_t, uint32_t>, int> count;
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
                for (int e = 0; e < 3; ++e) ++count[std::make_pair(std::min(idx[t + e], idx[t + (e + 1) % 3]), std::max(idx[t + e], idx[t + (e + 1) % 3]))];
            for (const auto& [k, c] : count)
                if (c == 1) { edge[k.first] = edge[k.second] = 1; edgeNb[k.first].push_back(k.second); edgeNb[k.second].push_back(k.first); }
        }
        for (int it = 0; it < 4; ++it) {
            std::vector<V3> next = pos;
            for (size_t k = 0; k < pos.size(); ++k) {
                if (edge[k] && edgeNb[k].size() == 2) next[k] = onSkin(Add(pos[k], Sub({(pos[edgeNb[k][0]][0] + pos[edgeNb[k][1]][0]) / 2, (pos[edgeNb[k][0]][1] + pos[edgeNb[k][1]][1]) / 2, (pos[edgeNb[k][0]][2] + pos[edgeNb[k][1]][2]) / 2}, pos[k]), 0.5f), nullptr);
                else if (!edge[k] && !ring[k].empty()) {
                    V3 avg = {0, 0, 0};
                    for (uint32_t j : ring[k]) avg = Add(avg, pos[j], 1.0f / ring[k].size());
                    next[k] = onSkin(Add(pos[k], Sub(avg, pos[k]), 0.3f), nullptr);
                }
            }
            pos.swap(next);
        }
        // distance to the edge along the surface (Dijkstra over the edges), then the dome
        std::vector<float> dist(pos.size(), 1e30f);
        {
            using Item = std::pair<float, uint32_t>;
            std::vector<Item> heap;
            for (uint32_t k = 0; k < pos.size(); ++k) if (edge[k]) { dist[k] = 0; heap.push_back({0.0f, k}); }
            std::make_heap(heap.begin(), heap.end(), std::greater<Item>());
            while (!heap.empty()) {
                std::pop_heap(heap.begin(), heap.end(), std::greater<Item>());
                const auto [d, k] = heap.back();
                heap.pop_back();
                if (d > dist[k]) continue;
                for (uint32_t j : ring[k]) {
                    const float nd = d + Len(Sub(pos[j], pos[k]));
                    if (nd < dist[j]) { dist[j] = nd; heap.push_back({nd, j}); std::push_heap(heap.begin(), heap.end(), std::greater<Item>()); }
                }
            }
        }
        std::vector<float> h(pos.size());
        std::vector<V3> sn(pos.size());
        size_t crown = 0;
        for (size_t k = 0; k < pos.size(); ++k) {
            onSkin(pos[k], &sn[k]);
            if (dist[k] > dist[crown] && dist[k] < 1e29f) crown = k;
            const float x = std::clamp(dist[k] / 0.03f, 0.0f, 1.0f);
            h[k] = edge[k] ? 0.0f : height * std::sqrt(x * (2.0f - x));
        }
        for (int it = 0; it < 4; ++it) {
            std::vector<float> next = h;
            for (size_t k = 0; k < pos.size(); ++k) {
                if (edge[k] || ring[k].empty()) continue;
                float a = 0;
                for (uint32_t j : ring[k]) a += h[j];
                next[k] = 0.5f * h[k] + 0.5f * a / ring[k].size();
            }
            h.swap(next);
        }
        for (size_t k = 0; k < pos.size(); ++k) pos[k] = Add(pos[k], sn[k], h[k]);
        comb.assign(pos.size(), V3{0, -1, 0});
        Normals();
        for (size_t k = 0; k < pos.size(); ++k) {
            V3 d = Add(Norm(Sub(pos[k], pos[crown])), {0, -1, 0}, 0.35f);
            d = Sub(d, {nrm[k][0] * Dot(d, nrm[k]), nrm[k][1] * Dot(d, nrm[k]), nrm[k][2] * Dot(d, nrm[k])});
            comb[k] = Norm(d);
        }
        undo_.clear();
        status = "clay from the painted scalp: " + std::to_string(idx.size() / 3) + " triangles";
        return true;
    }

    bool Save(const std::wstring& path) const {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        fprintf(f, "{\"v\": 1, \"pos\": [");
        for (size_t k = 0; k < pos.size(); ++k) fprintf(f, "%s%.5f,%.5f,%.5f", k ? "," : "", pos[k][0], pos[k][1], pos[k][2]);
        fprintf(f, "], \"idx\": [");
        for (size_t k = 0; k < idx.size(); ++k) fprintf(f, "%s%u", k ? "," : "", idx[k]);
        fprintf(f, "], \"comb\": [");
        for (size_t k = 0; k < comb.size(); ++k) fprintf(f, "%s%.4f,%.4f,%.4f", k ? "," : "", comb[k][0], comb[k][1], comb[k][2]);
        fprintf(f, "]}\n");
        fclose(f);
        return true;
    }

    // Nearest hit of a mesh-space ray; false when it misses the clay.
    bool Raycast(const float o[3], const float d[3], V3& hit, V3& n) const {
        float best = 1e30f;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const V3 &a = pos[idx[t]], &b = pos[idx[t + 1]], &c = pos[idx[t + 2]];
            const V3 e1 = Sub(b, a), e2 = Sub(c, a), p = Cross({d[0], d[1], d[2]}, e2);
            const float det = Dot(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            const V3 tv = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
            const float u = Dot(tv, p) / det;
            if (u < 0 || u > 1) continue;
            const V3 q = Cross(tv, e1);
            const float v = Dot({d[0], d[1], d[2]}, q) / det;
            if (v < 0 || u + v > 1) continue;
            const float dist = Dot(e2, q) / det;
            if (dist > 1e-5f && dist < best) { best = dist; hit = {o[0] + d[0] * dist, o[1] + d[1] * dist, o[2] + d[2] * dist}; n = Norm(Cross(e1, e2)); }
        }
        return best < 1e29f;
    }

    void BeginStroke() {
        undo_.push_back({pos, comb});
        if (undo_.size() > 40) undo_.erase(undo_.begin());
        grabbing_ = false;
    }
    bool Undo() {
        if (undo_.empty()) return false;
        pos = undo_.back().first;
        comb = undo_.back().second;
        undo_.pop_back();
        Normals();
        return true;
    }

    // One dab at a mesh-space point. radius in metres, strength 0..1, hardness 0..1 (falloff shape). `move`: the stroke's step
    // since the last dab (comb direction); `skin`: the head's rest vertices, so deflate never pushes the clay into the scalp.
    void Dab(Tool tool, const V3& at, float radius, float strength, float hardness, const V3& move, bool mirror, const std::vector<Vertex>& skin) {
        DabOne(tool, at, radius, strength, hardness, move, skin);
        if (mirror && std::fabs(at[0]) > radius * 0.3f) DabOne(tool, {-at[0], at[1], at[2]}, radius, strength, hardness, {-move[0], move[1], move[2]}, skin);
        Normals();
    }

    // Grab: on the first call remember the vertices under the brush; then every call moves them by `delta` (mesh space) from where
    // they started, weighted by the falloff.
    void GrabTo(const V3& at, const V3& delta, float radius, float hardness, bool mirror) {
        if (!grabbing_) {
            grabbing_ = true;
            grabStart_ = pos;
            grabW_.assign(pos.size(), 0.0f);
            grabMirrorW_.assign(pos.size(), 0.0f);
            for (size_t k = 0; k < pos.size(); ++k) {
                grabW_[k] = Falloff(Len(Sub(pos[k], at)) / radius, hardness);
                if (mirror) grabMirrorW_[k] = Falloff(Len(Sub(pos[k], V3{-at[0], at[1], at[2]})) / radius, hardness);
            }
        }
        for (size_t k = 0; k < pos.size(); ++k) {
            const float w = grabW_[k], wm = grabMirrorW_[k];
            pos[k] = {grabStart_[k][0] + delta[0] * w - delta[0] * wm, grabStart_[k][1] + (delta[1]) * (w + wm), grabStart_[k][2] + delta[2] * (w + wm)};
        }
        Normals();
    }

    // Comb lines for the viewport: one short line per vertex along its comb, lifted a hair off the clay.
    void CombLines(std::vector<std::pair<V3, V3>>& out, float len) const {
        for (size_t k = 0; k < pos.size(); k += 2) {
            const V3 p = Add(pos[k], nrm[k], 0.0008f);
            out.push_back({p, Add(p, comb[k], len)});
        }
    }

    // How thick the clay is at each vertex: its height above the nearest skin vertex, along that vertex's normal (metres).
    // A 1 cm grid over the skin keeps it to a few cells per query, so it runs every frame while Inflate / Deflate is picked.
    void Thickness(const std::vector<Vertex>& skin, std::vector<float>& out) const {
        out.assign(pos.size(), 0.0f);
        constexpr float kCell = 0.01f;
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        const auto key = [](int x, int y, int z) { return ((int64_t)(x + 1000) << 42) | ((int64_t)(y + 1000) << 21) | (int64_t)(z + 1000); };
        const auto cell = [&](float v) { return (int)std::floor(v / kCell); };
        for (uint32_t k = 0; k < skin.size(); ++k)
            if (std::fabs(skin[k].a - 0.5f) <= 0.1f) grid[key(cell(skin[k].px), cell(skin[k].py), cell(skin[k].pz))].push_back(k);
        if (grid.empty()) return;
        for (size_t k = 0; k < pos.size(); ++k) {
            const V3& p = pos[k];
            const int cx = cell(p[0]), cy = cell(p[1]), cz = cell(p[2]);
            float best = 1e30f;
            const Vertex* s = nullptr;
            for (int r = 0; r <= 8; ++r) { // grow the shell of cells until the nearest found is closer than the next shell
                for (int x = -r; x <= r; ++x)
                    for (int y = -r; y <= r; ++y)
                        for (int z = -r; z <= r; ++z) {
                            if (std::max({std::abs(x), std::abs(y), std::abs(z)}) != r) continue;
                            const auto it = grid.find(key(cx + x, cy + y, cz + z));
                            if (it == grid.end()) continue;
                            for (uint32_t i : it->second) {
                                const Vertex& v = skin[i];
                                const float d = (v.px - p[0]) * (v.px - p[0]) + (v.py - p[1]) * (v.py - p[1]) + (v.pz - p[2]) * (v.pz - p[2]);
                                if (d < best) { best = d; s = &v; }
                            }
                        }
                if (s && std::sqrt(best) < r * kCell) break;
            }
            if (s) out[k] = std::max(0.0f, (p[0] - s->px) * s->nx + (p[1] - s->py) * s->ny + (p[2] - s->pz) * s->nz);
        }
    }

private:
    std::vector<std::pair<std::vector<V3>, std::vector<V3>>> undo_;
    bool grabbing_ = false;
    std::vector<V3> grabStart_;
    std::vector<float> grabW_, grabMirrorW_;

    static V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    static V3 Add(const V3& a, const V3& b, float s = 1.0f) { return {a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s}; }
    static V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
    static float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
    static V3 Norm(const V3& a) { const float l = Len(a); return l > 1e-12f ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 1, 0}; }
    static float Falloff(float x, float hardness) {
        if (x >= 1.0f) return 0.0f;
        const float edge = std::clamp(1.0f - hardness, 0.05f, 1.0f), t = std::clamp((1.0f - x) / edge, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void DabOne(Tool tool, const V3& at, float radius, float strength, float hardness, const V3& move, const std::vector<Vertex>& skin) {
        std::vector<V3> before = tool == Smooth ? pos : std::vector<V3>();
        const V3 moveDir = Norm(move);
        const bool moving = Len(move) > 1e-6f;
        for (size_t k = 0; k < pos.size(); ++k) {
            const float w = Falloff(Len(Sub(pos[k], at)) / radius, hardness) * strength;
            if (w <= 0.0f) continue;
            if (tool == Inflate || tool == Deflate) {
                pos[k] = Add(pos[k], nrm[k], (tool == Inflate ? 1.0f : -1.0f) * w * radius * 0.06f);
                if (tool == Deflate) KeepOffSkin(pos[k], skin);
            } else if (tool == Smooth) {
                if (ring[k].empty()) continue;
                V3 avg = {0, 0, 0}, cavg = {0, 0, 0};
                for (uint32_t j : ring[k]) { avg = Add(avg, before[j]); cavg = Add(cavg, comb[j]); }
                avg = {avg[0] / ring[k].size(), avg[1] / ring[k].size(), avg[2] / ring[k].size()};
                pos[k] = Add(pos[k], Sub(avg, pos[k]), std::min(1.0f, w * 0.8f));
                comb[k] = Norm(Add(comb[k], Sub(Norm(cavg), comb[k]), std::min(1.0f, w * 0.5f)));
            } else if (tool == Comb && moving) {
                // the stroke's direction laid into the clay's surface at this vertex
                const V3 t = Norm(Sub(moveDir, {nrm[k][0] * Dot(moveDir, nrm[k]), nrm[k][1] * Dot(moveDir, nrm[k]), nrm[k][2] * Dot(moveDir, nrm[k])}));
                comb[k] = Norm(Add(comb[k], Sub(t, comb[k]), std::min(1.0f, w * 1.2f)));
            }
        }
    }

    // never closer than 1.5 mm to the nearest skin vertex, along that vertex's normal
    static void KeepOffSkin(V3& p, const std::vector<Vertex>& skin) {
        float best = 1e30f;
        const Vertex* s = nullptr;
        for (const Vertex& v : skin) {
            if (std::fabs(v.a - 0.5f) > 0.1f) continue; // skin vertices carry alpha 0.5
            const float d = (v.px - p[0]) * (v.px - p[0]) + (v.py - p[1]) * (v.py - p[1]) + (v.pz - p[2]) * (v.pz - p[2]);
            if (d < best) { best = d; s = &v; }
        }
        if (!s) return;
        const float h = (p[0] - s->px) * s->nx + (p[1] - s->py) * s->ny + (p[2] - s->pz) * s->nz;
        if (h < 0.0015f) p = Add(p, {s->nx, s->ny, s->nz}, 0.0015f - h);
    }

    void BuildRings() {
        ring.assign(pos.size(), {});
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = idx[t + e], b = idx[t + (e + 1) % 3];
                if (std::find(ring[a].begin(), ring[a].end(), b) == ring[a].end()) ring[a].push_back(b);
                if (std::find(ring[b].begin(), ring[b].end(), a) == ring[b].end()) ring[b].push_back(a);
            }
    }

    void Normals() {
        nrm.assign(pos.size(), V3{0, 0, 0});
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const V3 n = Cross(Sub(pos[idx[t + 1]], pos[idx[t]]), Sub(pos[idx[t + 2]], pos[idx[t]]));
            for (int e = 0; e < 3; ++e) nrm[idx[t + e]] = Add(nrm[idx[t + e]], n);
        }
        for (V3& n : nrm) n = Norm(n);
    }
};

} // namespace tbd::character
