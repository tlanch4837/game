// Specialized industry (Cities: Skylines II style): a hub building on a road (3 x 4 cells) plus a work area the player
// draws as a polygon. The area starts from the hub's back edge (the side away from the road); each click adds a corner.
// Natural resources are heat maps over the whole (unbounded) world: fertile land, forest, oil and ore. A site's
// efficiency is the mean resource value under its area, so where the player draws decides how well it produces.
// Platform free: the host supplies the road network and a ground function, and draws everything.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "building_snap.h"
#include "placement_rotation.h"
#include "road_network.h"

namespace tbd {
namespace city {

enum class Res : uint8_t { Fertile, Forest, Oil, Ore, None };
enum class IndKind : uint8_t { Grain, Livestock, Timber, Oil, Ore, Landfill, Count };

struct IndType {
    const char* name;
    Res res;
    const char* resName;
    const char* product;
    float ratePerHa; // tonnes per game month per hectare at 100% efficiency
    Vec3 color;      // area + UI colour
};
inline const IndType& IndTypeOf(IndKind k) {
    static const IndType t[] = {
        {"Grain Farm", Res::Fertile, "Fertile land", "Grain", 14.0f, {0.91f, 0.76f, 0.35f}},
        {"Livestock Farm", Res::None, "None (any flat land)", "Livestock", 6.0f, {0.55f, 0.77f, 0.36f}},
        {"Forestry", Res::Forest, "Forest", "Timber", 9.0f, {0.25f, 0.48f, 0.29f}},
        {"Oil Drilling", Res::Oil, "Oil deposits", "Crude oil", 22.0f, {0.23f, 0.21f, 0.19f}},
        {"Ore Mining", Res::Ore, "Ore deposits", "Metal ore", 17.0f, {0.75f, 0.40f, 0.25f}},
        // A landfill works the same way: hub on the road, cells drawn as the area. ratePerHa = garbage it can take in.
        {"Landfill", Res::None, "None (any land)", "Garbage disposal", 260.0f, {0.52f, 0.44f, 0.30f}},
    };
    return t[(int)k];
}
// The heat map's high colour per resource (the low end fades toward the terrain).
inline Vec3 ResColor(Res r) {
    switch (r) {
    case Res::Fertile: return {0.72f, 0.88f, 0.18f};
    case Res::Forest: return {0.12f, 0.55f, 0.22f};
    case Res::Oil: return {0.55f, 0.18f, 0.62f};
    case Res::Ore: return {0.90f, 0.42f, 0.16f};
    default: return {1, 1, 1};
    }
}

// A landfill's area is its storage: tonnes it can hold per hectare of drawn cells before it is full.
constexpr float kLandfillTPerHa = 45000.0f;

constexpr float kHubW = 24.0f, kHubD = 32.0f; // 3 x 4 cells of 8 m: width along the road, depth away from it

// ------------------------------------------------------------------------------------------------ resource heat maps
namespace resnoise {
inline float Hash(int x, int z, uint32_t s) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffffff) / 16777215.0f;
}
inline float Value(float x, float z, uint32_t s) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = (int)fx, iz = (int)fz;
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx), tz = tz * tz * (3 - 2 * tz);
    const float a = Hash(ix, iz, s), b = Hash(ix + 1, iz, s), c = Hash(ix, iz + 1, s), d = Hash(ix + 1, iz + 1, s);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}
inline float Fbm(float x, float z, uint32_t s) {
    float a = 0, amp = 0.5f, f = 1;
    for (int i = 0; i < 4; ++i) a += Value(x * f + i * 17.3f, z * f - i * 9.1f, s + i * 101) * amp, amp *= 0.5f, f *= 2.03f;
    return a / 0.9375f;
}
inline float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
} // namespace resnoise

struct ResourceField {
    uint32_t seed = 20261106;
    GroundFn ground; // optional: fertile land fades on slopes
    // 0..1 at a world position.
    float At(Res r, float x, float z) const {
        using namespace resnoise;
        switch (r) {
        case Res::Fertile: {
            float v = Smooth(0.47f, 0.68f, Fbm(x / 420.0f, z / 420.0f, seed + 1));
            if (ground && v > 0) {
                const float sl = (std::fabs(ground(x + 4, z) - ground(x - 4, z)) + std::fabs(ground(x, z + 4) - ground(x, z - 4))) / 16.0f;
                v *= std::clamp(1.0f - (sl - 0.06f) * 6.0f, 0.0f, 1.0f);
            }
            return v;
        }
        case Res::Forest: return Smooth(0.52f, 0.70f, Fbm(x / 300.0f, z / 300.0f, seed + 2));
        case Res::Oil: return Smooth(0.60f, 0.78f, Fbm(x / 360.0f, z / 360.0f, seed + 3));
        case Res::Ore: return Smooth(0.61f, 0.78f, Fbm(x / 260.0f, z / 260.0f, seed + 4));
        default: return 1.0f;
        }
    }
};

// ------------------------------------------------------------------------------------------------ polygon helpers
inline bool PolyContains(const std::vector<V2>& p, V2 q) {
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        if ((p[i].z > q.z) != (p[j].z > q.z) && q.x < (p[j].x - p[i].x) * (q.z - p[i].z) / (p[j].z - p[i].z) + p[i].x) in = !in;
    return in;
}
inline float PolyArea(const std::vector<V2>& p) {
    float s = 0;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) s += Cross2(p[j], p[i]);
    return std::fabs(s) * 0.5f;
}
inline bool SegCross(V2 a, V2 b, V2 c, V2 d) {
    auto o = [](V2 p, V2 q, V2 r) { const float v = Cross2(q - p, r - p); return (v > 0) - (v < 0); };
    return o(a, b, c) * o(a, b, d) < 0 && o(c, d, a) * o(c, d, b) < 0;
}
inline bool PolySimple(const std::vector<V2>& p) {
    const size_t n = p.size();
    if (n < 4) return true;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;
            if (SegCross(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return false;
        }
    return true;
}
inline float SegDist(V2 p, V2 a, V2 b) {
    const V2 d = b - a;
    const float l = Dot2(d, d), t = l > 0 ? std::clamp(Dot2(p - a, d) / l, 0.0f, 1.0f) : 0.0f;
    return Len(p - (a + d * t));
}

// ------------------------------------------------------------------------------------------------ sites
struct IndSite {
    uint64_t id = 0;
    IndKind kind = IndKind::Grain;
    V2 c, front;          // hub centre; front points from the hub toward its road
    float y = 0;          // hub ground height
    std::vector<V2> area; // corners the player added (the back edge's two corners come first implicitly)
    float eff = 0, hectares = 0, perMonth = 0;
    double stock = 0;     // tonnes produced, waiting for pickup (landfill: tonnes buried so far)
    uint64_t version = 1; // bumps on every change (the host rebuilds its geometry)

    V2 Side() const { return LeftOf(front); }
    void BackEdge(V2& a, V2& b) const {
        const V2 back = c - front * (kHubD * 0.5f);
        a = back - Side() * (kHubW * 0.5f), b = back + Side() * (kHubW * 0.5f);
    }
    std::vector<V2> Poly(const V2* extra = nullptr) const {
        V2 a, b;
        BackEdge(a, b);
        std::vector<V2> p{a, b};
        p.insert(p.end(), area.begin(), area.end());
        if (extra) p.push_back(*extra);
        return p;
    }
    bool IsLandfill() const { return kind == IndKind::Landfill; }
    double Capacity() const { return hectares * kLandfillTPerHa; }
    float Fill() const { return IsLandfill() && Capacity() > 0 ? (float)std::min(1.0, stock / Capacity()) : 0.0f; }
    bool InHub(V2 q, float pad = 0) const {
        const V2 d = q - c;
        return std::fabs(Dot2(d, Side())) < kHubW * 0.5f + pad && std::fabs(Dot2(d, front)) < kHubD * 0.5f + pad;
    }
};

struct IndustryWorld {
    std::vector<IndSite> sites;
    ResourceField field;
    uint64_t nextId = 1, version = 1;

    // Efficiency, hectares and output of a polygon for a kind (sampled on a 4 m grid).
    void Evaluate(IndKind k, const std::vector<V2>& poly, float& eff, float& ha, float& perMonth) const {
        eff = ha = perMonth = 0;
        if (poly.size() < 3) return;
        const IndType& t = IndTypeOf(k);
        ha = PolyArea(poly) / 10000.0f;
        float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        for (V2 p : poly) x0 = std::min(x0, p.x), z0 = std::min(z0, p.z), x1 = std::max(x1, p.x), z1 = std::max(z1, p.z);
        double sum = 0;
        int n = 0;
        for (float z = z0 + 2; z < z1; z += 4)
            for (float x = x0 + 2; x < x1; x += 4)
                if (PolyContains(poly, {x, z})) sum += field.At(t.res, x, z), ++n;
        eff = n ? (float)(sum / n) : 0.0f;
        perMonth = ha * t.ratePerHa * eff;
    }
    void Refresh(IndSite& s) {
        Evaluate(s.kind, s.Poly(), s.eff, s.hectares, s.perMonth);
        ++s.version, ++version;
    }
    // A hub snapped beside the nearest ordinary road to p. False when no road is near. quarter turns it from facing the
    // road by 90 degree steps (placement_rotation.h RoadSnap: its road-side edge stays half a metre off the kerb).
    bool Snap(const Network& net, V2 p, IndSite& s, int quarter = 0) const {
        const BuildingSnap r = SnapBesideRoad(net, p, kHubW, kHubD, quarter, 0, 0.5f, 60.0f); // [snap] building_snap.h
        if (!r.snapped) return false;
        s.c = r.c, s.front = r.front;
        return true;
    }
    // The hub's footprint is off every road and clear of other hubs.
    bool Free(const Network& net, const IndSite& s) const {
        float t, d;
        const V2 sd = s.Side(), f = s.front;
        for (float a : {-0.5f, 0.0f, 0.5f})
            for (float b : {-0.5f, 0.0f, 0.42f})
                if (net.SegmentAt(s.c + sd * (a * kHubW) + f * (b * kHubD), 0.0f, t, d) >= 0) return false;
        for (const IndSite& o : sites)
            if (o.id != s.id && Len(o.c - s.c) < kHubD + 2.0f) return false;
        return true;
    }
    int SiteAt(V2 p) const {
        for (size_t i = 0; i < sites.size(); ++i)
            if (sites[i].InHub(p) || (sites[i].area.size() >= 1 && PolyContains(sites[i].Poly(), p))) return (int)i;
        return -1;
    }
    // Game time: every site produces into its stock (a landfill is filled by the garbage trucks instead).
    void Tick(float minutes) {
        for (IndSite& s : sites) {
            if (s.IsLandfill()) continue; // fed by the garbage simulation (city_garbage.inl): only what the trucks bring
            s.stock += s.perMonth * minutes / (30.0 * 1440.0);
        }
    }
};

} // namespace city
} // namespace tbd
