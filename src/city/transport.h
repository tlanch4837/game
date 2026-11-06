// Transport buildings: the special, one-off buildings of the rail, ship and air networks (railway depot, stations, cargo
// terminal, shipyard and harbours, airfields). Unlike zoned lots they are never grown from painted cells: the player picks
// one and places it on the map, like Cities: Skylines II's airport and harbours.
//
//   Preview     blue hologram that follows the cursor (red when the spot is refused, with the reason);
//   Placed      the hologram stays for a short planning time ("Planned"), then it is built (a construction site that
//               rises), then it stands;
//   Sites       Flat   any dry, reasonably level ground clear of roads, rails, other buildings and zoned lots;
//               Track  a train station: it snaps onto a straight stretch of railway and lines up with the track;
//               Coast  harbours and shipyards. MUST sit on a qualifying coast: solid land behind the shoreline, the front
//                      of the lot on the shore, and a deep, open water apron in front of it (see PlanCoast). Never
//                      placeable inland, however the player rotates or moves it.
// Platform free: the host feeds the ground, the sea level, the road network and the zone grid (PlaceContext).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "road_network.h"
#include "zoning.h"

namespace tbd {
namespace city {

enum class TTab : int { Rail, Ship, Air, Count };
enum class Site : uint8_t { Flat, Track, Coast };
enum class Model : uint8_t { RailDepot, TrainStation, CargoTerminal, Shipyard, CargoHarbor, PassengerHarbor, SmallAirfield, RegionalAirport };

struct BuildingDef {
    const char* name;
    const char* shortName;
    const char* desc;
    TTab tab;
    Model model;
    Site site;
    int w, d;              // lot size in 8 m cells: w across the building, d from back to front (the front faces the road / water)
    int cost, upkeep, workplaces, vehicles;
    const char* noise;
    float height;          // tallest part, metres (also scales the construction rise)
    float buildMinutes;    // game minutes to build
    Vec3 tint;             // the tile's colour
    float apron = 0;       // coast only: how far the deep water has to reach past the lot's front edge
    float draft = 0;       // coast only: how deep (m under the sea) that water must be
    float dock = 0.3f;     // coast only: the share of the lot's depth that sits over the water
    float W() const { return w * kCell; }
    float D() const { return d * kCell; }
};

inline const std::vector<BuildingDef>& Defs() {
    static const std::vector<BuildingDef> v = {
        {"Railway Depot", "Rail Depot",
         "A large complex used for the storing, sorting, and maintenance of trains. Can be upgraded with extra tracks and a maintenance hall.",
         TTab::Rail, Model::RailDepot, Site::Flat, 18, 9, 120000, 12000, 24, 8, "Medium", 12, 600, {0.35f, 0.50f, 0.85f}},
        {"Train Station", "Station",
         "Passengers board and alight here. Place it on a long, straight stretch of railway: it lines itself up with the track.",
         TTab::Rail, Model::TrainStation, Site::Track, 24, 6, 60000, 6000, 12, 0, "Low", 10, 360, {0.45f, 0.62f, 0.90f}},
        {"Cargo Train Terminal", "Cargo Terminal",
         "Freight trains are loaded and unloaded here. A gantry crane moves containers between rail, road and the storage yard.",
         TTab::Rail, Model::CargoTerminal, Site::Flat, 28, 12, 180000, 18000, 40, 6, "High", 16, 720, {0.80f, 0.45f, 0.30f}},
        {"Shipyard", "Shipyard",
         "Builds, repairs and stores ships. It must be built on a qualifying coast: solid land behind the shoreline and deep, open water in front.",
         TTab::Ship, Model::Shipyard, Site::Coast, 20, 12, 150000, 15000, 60, 6, "High", 28, 840, {0.30f, 0.65f, 0.80f}, 70.0f, 4.0f, 0.30f},
        {"Cargo Harbor", "Cargo Harbor",
         "Ships dock here to load and unload containers and bulk cargo. Needs a qualifying coast with deep water for large ships.",
         TTab::Ship, Model::CargoHarbor, Site::Coast, 24, 14, 200000, 20000, 70, 8, "High", 38, 900, {0.85f, 0.60f, 0.25f}, 90.0f, 6.0f, 0.34f},
        {"Passenger Harbor", "Passenger Harbor",
         "Ferries and cruise ships call here. Needs a qualifying coast with some depth in front of the pier.",
         TTab::Ship, Model::PassengerHarbor, Site::Coast, 12, 8, 90000, 9000, 20, 4, "Low", 14, 480, {0.40f, 0.78f, 0.85f}, 50.0f, 3.0f, 0.36f},
        {"Small Airfield", "Airfield",
         "A short runway with a tower, a hangar and a small terminal for light aircraft and regional flights. Needs a big, level, dry site.",
         TTab::Air, Model::SmallAirfield, Site::Flat, 48, 14, 400000, 40000, 40, 10, "High", 22, 1200, {0.60f, 0.75f, 0.55f}},
        {"Regional Airport", "Regional Airport",
         "A full airport: a long runway, parallel taxiway, terminal with gates, control tower and hangars. Needs a very large, level, dry site.",
         TTab::Air, Model::RegionalAirport, Site::Flat, 64, 28, 1200000, 110000, 180, 24, "Very High", 30, 2400, {0.70f, 0.70f, 0.90f}},
    };
    return v;
}
inline const char* TTabName(TTab t) {
    static const char* const k[] = {"Rail", "Ships", "Air"};
    return k[(int)t];
}
inline const char* SiteRule(Site s) {
    static const char* const k[] = {"Place on level, dry ground", "Must be placed on a straight railway", "Must be built on a qualifying coast"};
    return k[(int)s];
}

// ---------------------------------------------------------------------------------------------------- geometry
// A footprint's local frame: x across the building, z from its back to its front.
struct Frame {
    V2 c, front, side;
    Frame(V2 c_, V2 f) : c(c_), front(f), side{f.z, -f.x} {}
    V2 World(float x, float z) const { return c + side * x + front * z; }
    V2 Local(V2 p) const {
        const V2 d = p - c;
        return {Dot2(d, side), Dot2(d, front)};
    }
};
inline BlockRect RectOf(V2 c, V2 front, const BuildingDef& d) { return {c, front, d.W() * 0.5f, d.D() * 0.5f}; }
// Do two oriented rectangles overlap (separating axis test), each grown by pad?
inline bool Overlaps(const BlockRect& a, const BlockRect& b, float pad = 0.0f) {
    const V2 d = b.c - a.c;
    const V2 axes[4] = {a.front, a.Side(), b.front, b.Side()};
    for (const V2& u : axes) {
        const float ra = (a.hw + pad) * std::fabs(Dot2(a.Side(), u)) + (a.hd + pad) * std::fabs(Dot2(a.front, u));
        const float rb = (b.hw + pad) * std::fabs(Dot2(b.Side(), u)) + (b.hd + pad) * std::fabs(Dot2(b.front, u));
        if (std::fabs(Dot2(d, u)) > ra + rb) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- planning
struct PlaceContext {
    const Network* net = nullptr;
    const ZoneGrid* zones = nullptr;
    GroundFn ground;
    float sea = -1e9f; // the water surface; very low = this map has no sea
    float Ground(float x, float z) const { return ground ? ground(x, z) : 0.0f; }
    float Ground(V2 p) const { return Ground(p.x, p.z); }
    bool HasWater() const { return sea > -1e8f; }
    bool IsWater(V2 p, float margin = 0.0f) const { return HasWater() && Ground(p) < sea - margin; }
};
struct PlanOptions {
    bool snapTrack = true; // stations snap to the nearest railway
    bool snapCoast = true; // harbours and shipyards snap to the nearest shore and face the water
};
struct Placement {
    bool valid = false;
    std::string error;
    V2 c{}, front{0, 1};
    float y = 0;                 // the level the building sits at
    int track = -1;              // station: the railway segment it sits on
    float platformInner = 3.0f;  // station: distance from the track's middle to the platform edge
    bool snapped = false;
    bool haveShore = false;
    V2 shore{};                  // coast: the shoreline point at the lot's waterside
    float relief = 0;            // highest minus lowest ground under the footprint
    float waterDepth = 0;        // coast: the shallowest depth sampled across the apron
};

enum class BState : uint8_t { Planned, Building, Done };
constexpr float kPlanMinutes = 12.0f; // the hologram stays up this long before the construction site opens
struct Placed {
    uint64_t id = 0;
    int def = 0;
    V2 c{}, front{0, 1};
    float y = 0;
    int track = -1;
    float platformInner = 3.0f;
    BState state = BState::Planned;
    float timer = 0;    // game minutes in the current state
    float progress = 0; // construction 0..1
    bool alive = true;
    bool haveShore = false;
    V2 shore{};
};

class TransportWorld {
public:
    std::vector<Placed> items;
    uint64_t version = 1;
    std::vector<uint64_t> removed; // demolished since the host last emptied this (it drops their drawing caches)

    // ---- the best placement for def with the cursor at p
    Placement Plan(const PlaceContext& cx, int def, V2 cursor, float rotationDeg, const PlanOptions& opt = {}) const {
        const BuildingDef& D = Defs()[def];
        Placement r;
        const float a = rotationDeg * 0.01745329f;
        r.front = {std::sin(a), std::cos(a)};
        r.c = cursor;
        r.y = cx.Ground(cursor);
        if (D.site == Site::Track && !PlanTrack(cx, D, cursor, opt, r)) return r;
        if (D.site == Site::Coast && opt.snapCoast && !PlanCoast(cx, D, cursor, r)) return r;
        Validate(cx, D, r);
        r.valid = r.error.empty();
        return r;
    }

    // Commits a valid placement; returns the new building's index (or -1).
    int Place(const Placement& p, int def) {
        if (!p.valid) return -1;
        Placed b;
        b.id = nextId_++;
        b.def = def;
        b.c = p.c, b.front = p.front, b.y = p.y, b.track = p.track, b.platformInner = p.platformInner;
        b.haveShore = p.haveShore, b.shore = p.shore;
        items.push_back(b);
        ++version;
        return (int)items.size() - 1;
    }

    // game minutes pass
    void Tick(float minutes) {
        for (Placed& b : items) {
            if (!b.alive || b.state == BState::Done) continue;
            b.timer += minutes;
            if (b.state == BState::Planned) {
                if (b.timer >= kPlanMinutes) b.state = BState::Building, b.timer = 0, ++version;
            } else {
                b.progress = std::min(1.0f, b.progress + minutes / Defs()[b.def].buildMinutes);
                if (b.progress >= 1.0f) b.state = BState::Done, ++version;
            }
        }
    }

    // the building under a ground point, or -1
    int HitTest(V2 p) const {
        for (size_t i = items.size(); i-- > 0;)
            if (items[i].alive && Rect(items[i]).Contains(p, 1.0f)) return (int)i;
        return -1;
    }
    bool Remove(int i) {
        if (i < 0 || i >= (int)items.size() || !items[i].alive) return false;
        items[i].alive = false;
        removed.push_back(items[i].id);
        ++version;
        return true;
    }
    int Count() const {
        int n = 0;
        for (const Placed& b : items) n += b.alive ? 1 : 0;
        return n;
    }
    // zoning keeps out of these
    std::vector<BlockRect> Blockers() const {
        std::vector<BlockRect> v;
        for (const Placed& b : items)
            if (b.alive) v.push_back(Rect(b));
        return v;
    }
    static BlockRect Rect(const Placed& b) { return RectOf(b.c, b.front, Defs()[b.def]); }
    // Is a rail node within reach of the depot / terminal's front (the lot is "connected")?
    static bool RailLink(const Network& net, V2 c, V2 front, const BuildingDef& d) {
        const V2 gate = c + front * (d.D() * 0.5f + 14.0f);
        for (size_t n = 0; n < net.nodes.size(); ++n)
            if (net.nodes[n].alive && net.IsRailNode((int)n) && Len(XZ(net.nodes[n].p) - gate) < d.W() * 0.5f + 20.0f) return true;
        return false;
    }

private:
    uint64_t nextId_ = 1;

    static void Fail(Placement& r, const char* msg) {
        if (r.error.empty()) r.error = msg;
    }

    // ---- stations: onto a straight, long enough, ground-level stretch of railway
    bool PlanTrack(const PlaceContext& cx, const BuildingDef& D, V2 cursor, const PlanOptions& opt, Placement& r) const {
        if (!cx.net) return Fail(r, "No railway"), false;
        const Network& net = *cx.net;
        int best = -1;
        float bt = 0, bd = opt.snapTrack ? 45.0f : 8.0f;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || !net.IsRailSeg((int)s)) continue;
            float t, d;
            net.Nearest((int)s, cursor, t, d);
            if (d < bd) bd = d, bt = t, best = (int)s;
        }
        if (best < 0) return Fail(r, "Stations must be placed on a railway"), false;
        const Cubic c = net.Curve(best);
        const float len = c.Length();
        const float need = D.W() + 8.0f;
        if (len < need) {
            char m[96];
            std::snprintf(m, sizeof(m), "Track too short for a station: it needs %.0f m in one piece", need);
            return Fail(r, m), r.c = c.At(bt), false;
        }
        const float half = (D.W() * 0.5f) / len, margin = (D.W() * 0.5f + 4.0f) / len;
        const float t = std::clamp(bt, margin, 1.0f - margin);
        r.c = c.At(t);
        const V2 T = c.Tan(t);
        const float sideSign = Dot2(cursor - r.c, LeftOf(T)) >= 0 ? 1.0f : -1.0f;
        r.front = LeftOf(T) * sideSign;
        r.track = best;
        r.snapped = true;
        r.y = net.YAt(net.segs[best], t);
        // the track must lie on the ground, straight and nearly level over the whole platform
        const V2 p0 = c.At(t - half), p1 = c.At(t + half), chord = Norm(p1 - p0);
        float dev = 0;
        for (int i = 0; i <= 12; ++i) {
            const V2 q = c.At(t - half + 2 * half * i / 12.0f);
            dev = std::max(dev, std::fabs(Cross2(q - p0, chord)));
        }
        const float dy = std::fabs(net.YAt(net.segs[best], t + half) - net.YAt(net.segs[best], t - half));
        if (std::fabs(r.y - cx.Ground(r.c)) > 2.0f) Fail(r, "The track must be on the ground");
        else if (dev > 2.5f) Fail(r, "The track is too curved here for a station");
        else if (dy / D.W() > 0.03f) Fail(r, "The track is too steep here for a station");
        // platforms start just outside the outermost track
        const Profile& p = net.Prof(best);
        float edge = p.Half(), outer = 0;
        for (const Strip& s : p.strips) {
            const float a0 = edge, a1 = edge - s.w;
            if (s.kind == Kind::Track) outer = std::max(outer, std::max(std::fabs(a0), std::fabs(a1)));
            edge = a1;
        }
        r.platformInner = outer + 0.9f;
        return true;
    }

    // ---- coasts: find the shoreline near the cursor, face the water, sit the lot's front on it
    static bool FindShore(const PlaceContext& cx, V2 cursor, V2& shore, V2& normal) {
        if (!cx.HasWater()) return false;
        const bool curWater = cx.IsWater(cursor);
        V2 q{};
        bool found = false;
        for (float rad = 4.0f; rad <= 160.0f && !found; rad += 4.0f) {
            const int n = std::max(8, (int)(6.2831853f * rad / 6.0f));
            float bestD = 1e30f;
            for (int k = 0; k < n; ++k) {
                const float a = 6.2831853f * k / n;
                const V2 p = cursor + V2{std::cos(a), std::sin(a)} * rad;
                if (cx.IsWater(p) != curWater) {
                    const float d = Len(p - cursor);
                    if (d < bestD) bestD = d, q = p, found = true;
                }
            }
        }
        if (!found) return false;
        V2 lo = cursor, hi = q; // bisect cursor -> q for the water line
        for (int i = 0; i < 14; ++i) {
            const V2 m = (lo + hi) * 0.5f;
            if (cx.IsWater(m) == curWater) lo = m;
            else hi = m;
        }
        shore = (lo + hi) * 0.5f;
        // the way to the water: the pull of every water sample around the shore point (straight coasts: the perpendicular)
        V2 sum{};
        int cnt = 0;
        for (float dz = -40; dz <= 40; dz += 5)
            for (float dx = -40; dx <= 40; dx += 5) {
                const V2 p = shore + V2{dx, dz};
                if (Len(p - shore) <= 40.0f && cx.IsWater(p)) sum = sum + (p - shore), ++cnt;
            }
        if (cnt < 6 || Len(sum) < 1e-3f) return false;
        normal = Norm(sum);
        return true;
    }
    bool PlanCoast(const PlaceContext& cx, const BuildingDef& D, V2 cursor, Placement& r) const {
        V2 shore, n;
        if (!FindShore(cx, cursor, shore, n)) return Fail(r, "Must be built on a coast: no shoreline nearby"), false;
        r.front = n;
        r.c = shore - n * (D.D() * (0.5f - D.dock));
        r.shore = shore;
        r.haveShore = true;
        r.snapped = true;
        return true;
    }

    // ---- everything the spot has to satisfy
    void Validate(const PlaceContext& cx, const BuildingDef& D, Placement& r) const {
        const Frame fr(r.c, r.front);
        const float W = D.W(), Dp = D.D();
        const BlockRect me = RectOf(r.c, r.front, D);
        // ground samples over the footprint
        const int nx = std::max(2, (int)std::ceil(W / 12.0f)), nz = std::max(2, (int)std::ceil(Dp / 12.0f));
        const float shoreZ = Dp * (0.5f - D.dock); // coast: the lot's z where the shoreline should be
        float lo = 1e30f, hi = -1e30f, sum = 0;
        int cnt = 0;
        bool wet = false, backWet = false;
        for (int j = 0; j <= nz; ++j)
            for (int i = 0; i <= nx; ++i) {
                const float x = -W * 0.5f + W * i / nx, z = -Dp * 0.5f + Dp * j / nz;
                const V2 p = fr.World(x, z);
                const float g = cx.Ground(p);
                if (D.site == Site::Coast) {
                    if (z <= shoreZ - 6.0f) { // the land behind the quay (a gentle beach is fine: the quay level is built up over it)
                        if (g < cx.sea - 0.1f) backWet = true;
                        lo = std::min(lo, g), hi = std::max(hi, g), sum += g, ++cnt;
                    }
                    continue;
                }
                if (cx.HasWater() && g < cx.sea + 0.3f) wet = true;
                lo = std::min(lo, g), hi = std::max(hi, g), sum += g, ++cnt;
            }
        r.relief = cnt ? hi - lo : 0.0f;
        if (D.site == Site::Coast) {
            if (!cx.HasWater()) Fail(r, "Must be built on a coast: this map has no sea");
            else if (backWet) Fail(r, "Not a qualifying coast: not enough solid land behind the shoreline");
            else if (r.relief > 9.0f) Fail(r, "Not a qualifying coast: the shore is too steep");
            // the apron in front of the lot: open water all the way (no sandbars, islets or rocks), and real ship depth at its
            // seaward end (a gently shelving beach is fine: the approach is dredged, the ships need the depth out there)
            if (r.error.empty()) {
                int total = 0, clearWater = 0, tail = 0, tailDeep = 0;
                float shallowestTail = 1e30f;
                const int cols = std::max(1, (int)(W / 12.0f));
                for (float z = Dp * 0.5f + 6.0f; z <= Dp * 0.5f + D.apron + 0.01f; z += 10.0f)
                    for (int i = 0; i <= cols; ++i) {
                        const float x = -W * 0.5f + W * i / cols;
                        const float depth = cx.sea - cx.Ground(fr.World(x, z));
                        ++total;
                        if (depth >= 0.2f) ++clearWater;
                        if (z >= Dp * 0.5f + D.apron * 0.66f) {
                            ++tail;
                            if (depth >= D.draft) ++tailDeep;
                            shallowestTail = std::min(shallowestTail, depth);
                        }
                    }
                r.waterDepth = tail ? shallowestTail : 0.0f;
                if (total && clearWater < (int)(total * 0.95f)) Fail(r, "Not a qualifying coast: the water in front is blocked by shallows or rock");
                else if (tail && tailDeep < (int)(tail * 0.85f)) Fail(r, "Not a qualifying coast: the water in front is too shallow for ships");
            }
            r.y = cx.sea + 2.4f;
        } else {
            if (wet) Fail(r, "Cannot be built on water");
            else if (r.relief > (D.model == Model::SmallAirfield || D.model == Model::RegionalAirport ? 9.0f : 7.0f) && D.site == Site::Flat) {
                char m[64];
                std::snprintf(m, sizeof(m), "Ground too uneven (%.0f m of relief)", r.relief);
                Fail(r, m);
            }
            if (D.site == Site::Flat) r.y = cnt ? sum / cnt : 0.0f;
        }
        // roads, rails (a station sits on its own track)
        if (cx.net && r.error.empty() && BlockedByNet(cx, me, r.track)) Fail(r, D.site == Site::Track ? "A road or another track is in the way" : "A road or railway is in the way");
        // other transport buildings
        if (r.error.empty())
            for (const Placed& b : items)
                if (b.alive && Overlaps(me, Rect(b), 2.0f)) {
                    Fail(r, "Overlaps another building");
                    break;
                }
        // zoned buildings and lots under construction
        if (cx.zones && r.error.empty()) {
            const float rad = std::sqrt(me.hw * me.hw + me.hd * me.hd) + kCell;
            cx.zones->CellsNear(r.c, rad, [&](int j) {
                const ZoneCell& z = cx.zones->cells[j];
                if (z.lot >= 0 && me.Contains(z.c, kCell * 0.6f)) Fail(r, "Overlaps a zoned building");
            });
        }
    }

    bool BlockedByNet(const PlaceContext& cx, const BlockRect& me, int hostSeg) const {
        const Network& net = *cx.net;
        const float reach = std::sqrt(me.hw * me.hw + me.hd * me.hd);
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || (int)s == hostSeg) continue;
            const Cubic c = net.Curve((int)s);
            const float hw = net.Prof((int)s).Half() + 1.0f;
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (V2 p : {c.p0, c.p1, c.p2, c.p3}) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), z0 = std::min(z0, p.z), z1 = std::max(z1, p.z);
            if (me.c.x + reach + hw < x0 || me.c.x - reach - hw > x1 || me.c.z + reach + hw < z0 || me.c.z - reach - hw > z1) continue;
            const int n = std::max(4, (int)std::ceil(c.Length() / 4.0f));
            for (int i = 0; i <= n; ++i) {
                const float t = (float)i / n;
                const V2 p = c.At(t);
                const float y = net.YAt(net.segs[s], t), g = cx.Ground(p);
                if (y > g + 8.0f || y < g - 5.0f) continue; // a bridge or tunnel passes over / under
                if (me.Contains(p, hw)) return true;
            }
        }
        return false;
    }
};

// The footprint outline, the water apron (coast), the platforms (station) and the shoreline marker: lines for DrawTransient.
inline void MarkerLines(const BuildingDef& D, const Placement& p, const GroundFn& ground, std::vector<Vertex>& out, Vec3 col) {
    const Frame fr(p.c, p.front);
    auto Y = [&](V2 q) { return (ground ? ground(q.x, q.z) : 0.0f) + 0.5f; };
    auto line = [&](V2 a, V2 b, Vec3 c) {
        const int n = std::max(1, (int)(Len(b - a) / 6.0f));
        for (int i = 0; i < n; ++i) {
            const V2 s = Lerp2(a, b, (float)i / n), e = Lerp2(a, b, (float)(i + 1) / n);
            out.push_back({s.x, Y(s), s.z, 0, 0, 0, c.x, c.y, c.z, 1});
            out.push_back({e.x, Y(e), e.z, 0, 0, 0, c.x, c.y, c.z, 1});
        }
    };
    const float hw = D.W() * 0.5f, hd = D.D() * 0.5f;
    const V2 q[4] = {fr.World(-hw, -hd), fr.World(hw, -hd), fr.World(hw, hd), fr.World(-hw, hd)};
    for (int i = 0; i < 4; ++i) line(q[i], q[(i + 1) % 4], col);
    if (D.site == Site::Coast) {
        const Vec3 water{0.35f, 0.8f, 1.0f};
        const V2 a[4] = {fr.World(-hw, hd), fr.World(hw, hd), fr.World(hw, hd + D.apron), fr.World(-hw, hd + D.apron)};
        for (int i = 1; i < 4; ++i) line(a[i], a[(i + 1) % 4], water);
        if (p.haveShore) { // the shoreline under the lot's waterside
            line(fr.World(-hw, hd - D.D() * D.dock), fr.World(hw, hd - D.D() * D.dock), {1.0f, 0.9f, 0.4f});
        }
    }
}

} // namespace city
} // namespace tbd
