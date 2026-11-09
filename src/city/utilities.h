// Utilities (Cities: Skylines II style): electricity, water and sewage.
//
//   Every building needs all three and a road. Roads that are not highways (or paths / railways) carry the utilities
//   under them by default, so a building on such a road is connected to everything that road network reaches. The
//   sources are drag-and-drop buildings (power plants, pumps, sewage outlets and treatment plants); a source feeds the
//   network of roads next to it. Where there is no road the player lays a line by hand: an electricity line (wooden
//   poles at 0 m, steel pylons above ground, a buried cable below ground) or a water / sewage pipe (always buried).
//   Lines join whatever they end at: a road, a source, another line's end, a building.
//
//   Each network (the connected set of roads, lines and sources) has a supply (sum of its sources) and a demand (sum of the
//   buildings on it). Buildings are served in the order they were built until the supply runs out; the rest are short.
//   Emergency generators and batteries only cover a shortfall.
//
//   Roads: a building without a road in front of it is flagged; after 10 game days it is condemned and 5 days later it is
//   demolished (zoning.h keeps the clock). A road back in front of it stops and resets that.
//
// Platform free: the host feeds the road network, the zone grid and the ground, and draws the results.
#pragma once
#include <cstring>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "placement_rotation.h"
#include "road_network.h"
#include "transport.h"
#include "zoning.h"

namespace tbd {
namespace city {

enum class Util : uint8_t { Electricity, Water, Sewage, Count };
constexpr int kUtilKinds = (int)Util::Count;
inline const char* UtilName(Util u) {
    static const char* const k[] = {"Electricity", "Water", "Sewage"};
    return k[(int)u];
}
inline const char* UtilUnit(Util u) {
    static const char* const k[] = {"MW", "m3/day", "m3/day"};
    return k[(int)u];
}
// "No electricity", the line of a notification.
inline const char* UtilProblem(Util u) {
    static const char* const k[] = {"No electricity", "No water", "No sewage"};
    return k[(int)u];
}

enum class UTab : int { Electricity, Water, Count };
inline const char* UTabName(UTab t) { return t == UTab::Electricity ? "Electricity" : "Water & Sewage"; }

// A drag-and-drop source building. The numbers come from assets/buildings/{power,water}/catalog.json; `supply` is what the
// network can actually draw (nameplate x capacity factor for generators).
struct UtilDef {
    const char* name;
    const char* shortName;
    const char* desc;
    UTab tab;
    Util kind;         // the network it feeds
    const char* file;  // GLB, relative to the project root
    int w, d;          // lot in 8 m cells: w across, d from back to front (the front faces the road)
    float height;      // metres
    float supply;      // MW, or m3 per day
    bool backup;       // only covers a shortfall (emergency generator, battery)
    int cost, upkeep;
    const char* rule;  // the placement rule, shown on the card
    Vec3 tint;
    float W() const { return w * kCell; }
    float D() const { return d * kCell; }
};

inline const std::vector<UtilDef>& UtilDefs() {
    static const std::vector<UtilDef> v = {
        // ---- electricity
        {"Emergency Generator", "Generator", "Diesel backup. Only runs when the grid is short; burns fuel and pollutes while on.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_1x2_emergency.glb", 1, 2, 4.6f, 5.0f, true, 40000, 400, "Backup: covers a shortfall only", {0.80f, 0.62f, 0.25f}},
        {"Wind Turbine", "Wind", "Clean but variable. Output follows wind speed; better on hills and coasts.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_2x2_wind.glb", 2, 2, 83.0f, 2.1f, false, 120000, 900, "Place on level ground", {0.62f, 0.85f, 0.95f}},
        {"Solar Power Station", "Solar", "Large clean farm. Produces only in daylight, peaks at noon.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_6x6_solar.glb", 6, 6, 3.0f, 8.8f, false, 420000, 2400, "Place on level ground", {0.35f, 0.55f, 0.90f}},
        {"Small Coal Power Plant", "Small Coal", "Cheap early base load with heavy air pollution.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_4x4_coal_small.glb", 4, 4, 55.0f, 51.0f, false, 300000, 3000, "Heavy pollution", {0.45f, 0.45f, 0.48f}},
        {"Gas Power Plant", "Gas", "Efficient base load; ramps quickly to cover peaks. Moderate pollution.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_5x4_gas.glb", 5, 4, 32.0f, 120.0f, false, 900000, 6500, "Moderate pollution", {0.85f, 0.55f, 0.30f}},
        {"Geothermal Power Plant", "Geothermal", "Clean, steady base load. Wants a geothermal hotspot.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_4x4_geothermal.glb", 4, 4, 18.0f, 108.0f, false, 1100000, 5500, "Geothermal hotspot (not enforced yet)", {0.80f, 0.40f, 0.30f}},
        {"Coal Power Plant", "Coal", "Big, cheap base load. Worst pollution in the game.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_6x6_coal.glb", 6, 6, 110.0f, 255.0f, false, 1500000, 11000, "Heavy pollution", {0.30f, 0.30f, 0.32f}},
        {"Nuclear Power Plant", "Nuclear", "Massive clean base load. Very expensive; small accident risk if underfunded.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_14x11_nuclear.glb", 14, 11, 54.0f, 736.0f, false, 6000000, 30000, "Flat land; wants water nearby", {0.55f, 0.80f, 0.55f}},
        {"Battery Station", "Battery", "Charges from surplus power and discharges when demand exceeds supply.", UTab::Electricity, Util::Electricity,
         "assets/buildings/power/glb/PW_3x3_battery.glb", 3, 3, 4.0f, 60.0f, true, 500000, 2500, "Backup: covers a shortfall only", {0.55f, 0.85f, 0.45f}},
        // ---- water and sewage
        {"Groundwater Pump", "Pump", "Small pump over a groundwater deposit. Contaminated if ground pollution reaches the wells.", UTab::Water, Util::Water,
         "assets/buildings/water/glb/WT_2x2_groundwater.glb", 2, 2, 4.5f, 4000.0f, false, 30000, 500, "Wants groundwater (not enforced yet)", {0.30f, 0.60f, 0.95f}},
        {"Water Tower", "Water Tower", "Compact groundwater source for dense areas.", UTab::Water, Util::Water,
         "assets/buildings/water/glb/WT_2x2_tower.glb", 2, 2, 29.0f, 2500.0f, false, 45000, 450, "Wants groundwater (not enforced yet)", {0.40f, 0.70f, 0.95f}},
        {"Water Pumping Station", "Pumping", "Pumps surface water from a river, lake or sea. Keep it upstream of sewage outlets.", UTab::Water, Util::Water,
         "assets/buildings/water/glb/WT_4x3_pumping.glb", 4, 3, 8.6f, 20000.0f, false, 200000, 2000, "Shoreline: back of lot on the water", {0.25f, 0.55f, 0.85f}},
        {"Large Water Pumping Station", "Large Pumping", "High-capacity shoreline station with extension pump houses.", UTab::Water, Util::Water,
         "assets/buildings/water/glb/WT_6x4_pumping_large.glb", 6, 4, 14.2f, 80000.0f, false, 750000, 6500, "Shoreline: back of lot on the water", {0.20f, 0.45f, 0.80f}},
        {"Sewage Outlet", "Outlet", "Dumps untreated sewage straight into the water. Cheap but pollutes downstream.", UTab::Water, Util::Sewage,
         "assets/buildings/water/glb/WT_2x2_sewage_outlet.glb", 2, 2, 3.3f, 20000.0f, false, 60000, 600, "Shoreline: back of lot on the water", {0.60f, 0.50f, 0.30f}},
        {"Wastewater Treatment Plant", "Treatment", "Cleans sewage before releasing it. Much less water pollution than an outlet.", UTab::Water, Util::Sewage,
         "assets/buildings/water/glb/WT_6x6_treatment.glb", 6, 6, 16.0f, 60000.0f, false, 900000, 7000, "Level ground near a road; no shoreline needed", {0.45f, 0.65f, 0.40f}},
    };
    return v;
}
inline bool UtilDefBackup(int def) { return UtilDefs()[def].backup; }
// Surface-water buildings (pumping stations, the sewage outlet) take or dump water: the back of the lot must touch the sea.
// Only rules that start with exactly "Shoreline:" opt in; the Wastewater Treatment Plant cleans sewage and pipes it on, so it
// places anywhere valid (level ground, no road / building in the way) and never needs the shore.
inline bool UtilDefShore(int def) { const char* r = UtilDefs()[def].rule; return r && std::string(r).rfind("Shoreline:", 0) == 0; }

// What a building takes from each network, per 8 m cell of its lot, by zone type (ZoneTypes() order).
inline float UtilDemandPerCell(Util u, int zoneType) {
    static const float elec[] = {0.002f, 0.003f, 0.006f, 0.012f, 0.006f, 0.008f, 0.020f}; // MW
    static const float water[] = {0.5f, 1.0f, 2.0f, 3.75f, 1.0f, 1.0f, 3.0f};           // m3 per day (sewage is the same flow back out)
    const int n = (int)(sizeof(elec) / sizeof(elec[0]));
    const int z = std::clamp(zoneType, 0, n - 1);
    return u == Util::Electricity ? elec[z] : water[z];
}

// A line the player lays: electricity (poles / pylons / buried cable) or a water / sewage pipe (buried).
struct UtilLine {
    uint64_t id = 0;
    Util kind = Util::Electricity;
    V2 a{}, b{};
    float elev = 0; // metres over the ground: 0 wooden poles, above that steel pylons, below that buried
    bool alive = true;
    float Length() const { return Len(b - a); }
    bool Buried() const { return elev < -0.01f; }
    bool Poles() const { return kind == Util::Electricity && std::fabs(elev) <= 0.01f; }
    bool Pylons() const { return kind == Util::Electricity && elev > 0.01f; }
};
// Per metre of line.
inline int UtilLineCostPerMetre(Util u, float elev) {
    if (u == Util::Electricity) return elev < -0.01f ? 90 : elev > 0.01f ? 60 : 25;
    return u == Util::Water ? 40 : 45;
}
constexpr float kPoleSpacing = 36.0f;    // wooden poles along a line at 0 m
constexpr float kPylonSpacing = 110.0f;  // steel pylons
constexpr float kPoleHeight = 9.0f;
inline float PylonHeight(float elev) { return 24.0f + elev; }

// A source building placed on the map.
struct UtilBuilding {
    uint64_t id = 0;
    int def = 0;
    V2 c{}, front{0, 1};
    float y = 0;
    bool alive = true;
    bool hasRoad = true;     // a road that carries utilities runs along its front
    float noRoadMinutes = 0;
    bool Condemned() const { return !hasRoad && noRoadMinutes >= kNoRoadMinutes; }
    float DemolishInMinutes() const { return std::max(0.0f, kNoRoadMinutes + kDemolishMinutes - noRoadMinutes); }
};

// Is there a utility road along p (the middle of a building's front edge)? The segment, or -1.
inline int UtilRoadNear(const Network& net, V2 p, float reach, float* gapOut = nullptr) {
    int best = -1;
    float bestGap = 1e30f;
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!CarriesUtilities(net, (int)s)) continue;
        float t, d;
        net.Nearest((int)s, p, t, d);
        const float gap = d - net.Prof((int)s).Half();
        if (gap <= reach && gap < bestGap) bestGap = gap, best = (int)s;
    }
    if (gapOut) *gapOut = bestGap;
    return best;
}

// The utility road a source building of W x D metres reaches from any edge of its (possibly turned) lot: the front edge
// first, then the sides and the back (placement_rotation.h EdgeMids). The segment, or -1.
inline int UtilRoadAccess(const Network& net, V2 c, V2 front, float W, float D, float reach) {
    V2 mid[4], out[4];
    EdgeMids(c, front, W, D, mid, out);
    for (int e = 0; e < 4; ++e) {
        const int s = UtilRoadNear(net, mid[e], reach);
        if (s >= 0) return s;
    }
    return -1;
}

// What the solver says about one building.
struct UtilStatus {
    bool road = true, elec = true, water = true, sewage = true; // false: the building lacks it
    bool Ok() const { return road && elec && water && sewage; }
    bool Has(Util u) const { return u == Util::Electricity ? elec : u == Util::Water ? water : sewage; }
    int Missing() const { return (!road) + (!elec) + (!water) + (!sewage); }
};
struct UtilReport {
    std::vector<UtilStatus> lots;     // per zone lot (zones.lots order); everything fine for lots still under construction
    std::vector<UtilStatus> buildings; // per placed source: only the road matters
    // per network kind
    float supply[kUtilKinds] = {};   // everything the sources can give
    float demand[kUtilKinds] = {};   // everything the built lots ask for
    float used[kUtilKinds] = {};     // what was actually delivered
    int unserved[kUtilKinds] = {};   // lots short of it
    int noRoad = 0;                  // lots (and sources) without a road
    // per road segment: is the network it belongs to supplied (it has a source on it)?
    std::vector<uint8_t> segLive[kUtilKinds];
    // per line: does its network have a source?
    std::vector<uint8_t> lineLive;
};

class UtilWorld {
public:
    std::vector<UtilBuilding> items;
    std::vector<UtilLine> lines;
    uint64_t version = 1;
    std::vector<uint64_t> removed; // sources demolished since the host last emptied this (it deletes their entities)
    // Cities: Skylines 1 style conduction: a building passes electricity to buildings (and sources) within this many metres
    // of its lot, so a block touching a powered one is powered too, roads or not. 0 = off (roads and lines only).
    float conductRange = 0.0f;
    // The weather's share of the catalog output of solar and wind sources (the catalog `supply` is the average; the city
    // sim's Phase 3 sets these from the sun and the wind now, relative to their running mean). outputVersion bumps on a
    // change, so UtilSolveCache solves again.
    float solarScale = 1.0f, windScale = 1.0f;
    uint64_t outputVersion = 0;
    float OutputScale(const UtilDef& D) const {
        if (D.kind != Util::Electricity || !D.shortName) return 1.0f;
        if (std::strcmp(D.shortName, "Solar") == 0) return solarScale;
        if (std::strcmp(D.shortName, "Wind") == 0) return windScale;
        return 1.0f;
    }

    // ------------------------------------------------------------------------------------------ placing a source
    // The best placement for def with the cursor at p. With snapRoad the building jumps to the nearest utility road within
    // 45 m, faces it (turned by rotationDeg's nearest quarter turns: 90 puts its side to the road) and sits with its
    // road-side edge a metre off the kerb (placement_rotation.h RoadSnap). Free, rotationDeg is its heading.
    Placement Plan(const PlaceContext& cx, int def, V2 cursor, float rotationDeg, bool snapRoad) const {
        const UtilDef& D = UtilDefs()[def];
        Placement r;
        r.front = FrontFromDeg(rotationDeg);
        r.c = cursor;
        if (snapRoad && cx.net) { // [snap] the shared helper (building_snap.h): entrance (the front) to the road
            const Network& net = *cx.net;
            const BuildingSnap sr = SnapBesideRoad(net, cursor, D.W(), D.D(), QuarterTurns(rotationDeg), 0, 1.0f, std::max(45.0f, SnapRangeFor(D.W(), D.D())),
                                                   [&](int s) { return CarriesUtilities(net, s); });
            if (sr.snapped) r.front = sr.front, r.c = sr.c, r.snapped = true;
        }
        r.y = cx.Ground(r.c);
        Validate(cx, D, r);
        r.valid = r.error.empty();
        return r;
    }

    int Place(const Placement& p, int def) {
        if (!p.valid) return -1;
        UtilBuilding b;
        b.id = nextId_++;
        b.def = def;
        b.c = p.c, b.front = p.front, b.y = p.y;
        items.push_back(b);
        ++version;
        return (int)items.size() - 1;
    }
    bool Remove(int i) {
        if (i < 0 || i >= (int)items.size() || !items[i].alive) return false;
        items[i].alive = false;
        removed.push_back(items[i].id);
        ++version;
        return true;
    }
    int HitTest(V2 p) const {
        for (size_t i = 0; i < items.size(); ++i) {
            if (!items[i].alive) continue;
            const UtilDef& D = UtilDefs()[items[i].def];
            if (RectOf(items[i].c, items[i].front, D.w, D.d).Contains(p, 1.0f)) return (int)i;
        }
        return -1;
    }
    static BlockRect RectOf(V2 c, V2 front, int w, int d) { return {c, front, w * kCell * 0.5f, d * kCell * 0.5f}; }
    std::vector<BlockRect> Blockers() const {
        std::vector<BlockRect> out;
        for (const UtilBuilding& b : items)
            if (b.alive) out.push_back(RectOf(b.c, b.front, UtilDefs()[b.def].w, UtilDefs()[b.def].d));
        return out;
    }

    // ------------------------------------------------------------------------------------------ lines
    int AddLine(Util kind, V2 a, V2 b, float elev) {
        if (Len(b - a) < 2.0f) return -1;
        UtilLine l;
        l.id = nextId_++;
        l.kind = kind, l.a = a, l.b = b;
        l.elev = kind == Util::Electricity ? elev : std::min(elev, -2.0f); // pipes are always buried
        lines.push_back(l);
        ++version;
        return (int)lines.size() - 1;
    }
    // The nearest line end to p within r (so a new line can join it), or false.
    bool SnapEnd(V2 p, Util kind, float r, V2& out) const {
        float best = r;
        bool any = false;
        for (const UtilLine& l : lines) {
            if (!l.alive || l.kind != kind) continue;
            for (V2 e : {l.a, l.b})
                if (Len(e - p) < best) best = Len(e - p), out = e, any = true;
        }
        return any;
    }
    // The nearest line (by its middle part) to p within r; -1 if none.
    int LineAt(V2 p, float r, bool onlyVisible = false) const {
        int best = -1;
        float bd = r;
        for (size_t i = 0; i < lines.size(); ++i) {
            const UtilLine& l = lines[i];
            if (!l.alive || (onlyVisible && l.Buried())) continue;
            const float d = DistToSeg(p, l.a, l.b);
            if (d < bd) bd = d, best = (int)i;
        }
        return best;
    }
    bool RemoveLine(int i) {
        if (i < 0 || i >= (int)lines.size() || !lines[i].alive) return false;
        lines[i].alive = false;
        ++version;
        return true;
    }
    static float DistToSeg(V2 p, V2 a, V2 b) {
        const V2 d = b - a;
        const float l2 = Dot2(d, d);
        const float t = l2 > 0 ? std::clamp(Dot2(p - a, d) / l2, 0.0f, 1.0f) : 0.0f;
        return Len(p - (a + d * t));
    }

    // ------------------------------------------------------------------------------------------ time
    // Game minutes pass: sources without a road run the same condemned -> demolished clock the zoned lots do.
    void Tick(float minutes) {
        for (size_t i = 0; i < items.size(); ++i) {
            UtilBuilding& b = items[i];
            if (!b.alive) continue;
            if (b.hasRoad) {
                if (b.noRoadMinutes > 0) b.noRoadMinutes = 0, ++version;
                continue;
            }
            const bool was = b.Condemned();
            b.noRoadMinutes += minutes;
            if (b.noRoadMinutes >= kNoRoadMinutes + kDemolishMinutes) Remove((int)i);
            else if (was != b.Condemned()) ++version;
        }
    }

    // ------------------------------------------------------------------------------------------ the solver
    // Works out which networks exist, what each supplies and demands, and which buildings are served. Cheap enough to
    // run when the roads, the zone grid or the utility world change (not every frame).
    void Solve(const Network& net, const ZoneGrid& zones, UtilReport& out) {
        const int S = (int)net.segs.size(), N = (int)net.nodes.size();
        const int B = (int)items.size(), L = (int)lines.size(), Z = (int)zones.lots.size();
        out = UtilReport{};
        out.lots.assign((size_t)Z, UtilStatus{});
        out.buildings.assign((size_t)B, UtilStatus{});
        out.lineLive.assign((size_t)L, 0);
        // source roads, found once (the same for every network)
        std::vector<int> srcSeg(B, -1);
        for (int i = 0; i < B; ++i) {
            if (!items[i].alive) continue;
            const UtilDef& D = UtilDefs()[items[i].def];
            srcSeg[i] = UtilRoadAccess(net, items[i].c, items[i].front, D.W(), D.D(), 6.0f); // any edge of a turned lot
            const bool road = srcSeg[i] >= 0;
            if (items[i].hasRoad != road) {
                items[i].hasRoad = road;
                if (road) items[i].noRoadMinutes = 0;
            }
            out.buildings[i].road = road;
            out.noRoad += road ? 0 : 1;
        }
        for (int z = 0; z < Z; ++z) {
            const Lot& l = zones.lots[z];
            if (!l.alive || !l.Built()) continue;
            if (!l.hasRoad) out.lots[z].road = false, ++out.noRoad;
        }
        // the road segment under each line end: a scan of every segment per end, so it is kept while the roads and the
        // utility world stay the same (a growing city re-solves for its new lots many times a minute)
        if (endSegNet_ != net.version || endSegUtil_ != version || endSeg_.size() != (size_t)(2 * L)) {
            endSegNet_ = net.version, endSegUtil_ = version;
            endSeg_.assign((size_t)(2 * L), -1);
            for (int i = 0; i < L; ++i)
                if (lines[i].alive) endSeg_[(size_t)(2 * i)] = UtilRoadNear(net, lines[i].a, 6.0f), endSeg_[(size_t)(2 * i + 1)] = UtilRoadNear(net, lines[i].b, 6.0f);
        }
        // lines: which road segments, sources and lots each end touches (per kind the same geometry)
        const float lotPad = 6.0f;
        for (int k = 0; k < kUtilKinds; ++k) {
            const Util kind = (Util)k;
            // nodes: segs [0,S), road nodes [S,S+N), sources, line ends (2 per line), lots
            const int base = S + N, srcBase = base, endBase = srcBase + B, lotBase = endBase + 2 * L, total = lotBase + Z;
            Dsu dsu(total);
            for (int s = 0; s < S; ++s)
                if (CarriesUtilities(net, s)) dsu.Join(s, S + net.segs[s].a), dsu.Join(s, S + net.segs[s].b);
            for (int i = 0; i < B; ++i)
                if (items[i].alive && srcSeg[i] >= 0) dsu.Join(srcBase + i, srcSeg[i]);
            for (int i = 0; i < L; ++i) {
                const UtilLine& ln = lines[i];
                if (!ln.alive || ln.kind != kind) continue;
                dsu.Join(endBase + 2 * i, endBase + 2 * i + 1);
                for (int e = 0; e < 2; ++e) {
                    const V2 p = e ? ln.b : ln.a;
                    const int me = endBase + 2 * i + e;
                    const int s = endSeg_[(size_t)(2 * i + e)];
                    if (s >= 0) dsu.Join(me, s);
                    for (int j = 0; j < B; ++j) // a source: the end lies on (or a few metres off) its lot
                        if (items[j].alive && UtilDefs()[items[j].def].kind == kind &&
                            RectOf(items[j].c, items[j].front, UtilDefs()[items[j].def].w, UtilDefs()[items[j].def].d).Contains(p, 4.0f))
                            dsu.Join(me, srcBase + j);
                    for (int j = 0; j < i; ++j) { // another line's end at the same spot
                        const UtilLine& o = lines[j];
                        if (!o.alive || o.kind != kind) continue;
                        if (Len(o.a - p) < 3.0f) dsu.Join(me, endBase + 2 * j);
                        if (Len(o.b - p) < 3.0f) dsu.Join(me, endBase + 2 * j + 1);
                    }
                    for (int z = 0; z < Z; ++z) { // a lot (a house away from any road): the end reaches it
                        const Lot& l = zones.lots[z];
                        if (!l.alive) continue;
                        if (RectOf(l.c, l.front, l.w, l.d).Contains(p, lotPad)) dsu.Join(me, lotBase + z);
                    }
                }
            }
            for (int z = 0; z < Z; ++z) {
                const Lot& l = zones.lots[z];
                if (l.alive && l.hasRoad && l.frontSeg >= 0 && l.frontSeg < S && CarriesUtilities(net, l.frontSeg)) dsu.Join(lotBase + z, l.frontSeg);
            }
            if (kind == Util::Electricity && conductRange > 0.0f) Conduct(zones, B, srcBase, lotBase, dsu);
            if (kind == Util::Electricity) { // the networks, kept for Resupply (the weather moves solar and wind output)
                elec_.total = total;
                elec_.src.assign((size_t)B, -1), elec_.lot.assign((size_t)Z, -1), elec_.seg.assign((size_t)S, -1), elec_.line.assign((size_t)L, -1);
                for (int i = 0; i < B; ++i) elec_.src[(size_t)i] = dsu.Find(srcBase + i);
                for (int z = 0; z < Z; ++z) elec_.lot[(size_t)z] = dsu.Find(lotBase + z);
                for (int s = 0; s < S; ++s)
                    if (CarriesUtilities(net, s)) elec_.seg[(size_t)s] = dsu.Find(s);
                for (int i = 0; i < L; ++i)
                    if (lines[i].alive && lines[i].kind == kind) elec_.line[(size_t)i] = dsu.Find(endBase + 2 * i);
            }
            // supply and demand per network
            std::vector<float> supply(total, 0.0f), backup(total, 0.0f), demand(total, 0.0f);
            for (int i = 0; i < B; ++i) {
                if (!items[i].alive) continue;
                const UtilDef& D = UtilDefs()[items[i].def];
                if (D.kind != kind) continue;
                const float sup = D.supply * OutputScale(D); // solar and wind follow the weather
                (D.backup ? backup : supply)[dsu.Find(srcBase + i)] += sup;
                out.supply[k] += D.backup ? 0.0f : sup;
            }
            for (int z = 0; z < Z; ++z) {
                const Lot& l = zones.lots[z];
                if (!l.alive || !l.Built()) continue;
                const float dem = UtilDemandPerCell(kind, l.type) * (float)(l.w * l.d);
                demand[dsu.Find(lotBase + z)] += dem;
                out.demand[k] += dem;
            }
            // a shortfall calls in the backup
            std::vector<float> avail(total, 0.0f);
            for (int r = 0; r < total; ++r)
                if (supply[r] > 0 || backup[r] > 0) avail[r] = supply[r] + (demand[r] > supply[r] ? backup[r] : 0.0f);
            // serve the built lots in the order they were laid out
            for (int z = 0; z < Z; ++z) {
                const Lot& l = zones.lots[z];
                if (!l.alive || !l.Built()) continue;
                const int r = dsu.Find(lotBase + z);
                const float dem = UtilDemandPerCell(kind, l.type) * (float)(l.w * l.d);
                const bool ok = avail[r] + 1e-6f >= dem;
                if (ok) avail[r] -= dem, out.used[k] += dem;
                else ++out.unserved[k];
                bool& flag = k == 0 ? out.lots[z].elec : k == 1 ? out.lots[z].water : out.lots[z].sewage;
                flag = ok;
            }
            // which roads and lines sit on a supplied network (for the underground view)
            std::vector<uint8_t> live(total, 0);
            for (int r = 0; r < total; ++r) live[r] = (supply[r] > 0 || backup[r] > 0) ? 1 : 0;
            out.segLive[k].assign((size_t)S, 0);
            for (int s = 0; s < S; ++s)
                if (CarriesUtilities(net, s)) out.segLive[k][s] = live[dsu.Find(s)];
            for (int i = 0; i < L; ++i)
                if (lines[i].alive && lines[i].kind == kind && live[dsu.Find(endBase + 2 * i)]) out.lineLive[i] = 1;
        }
    }

    // Only the sources' output changed (solarScale / windScale): the electricity networks of the last Solve serve their
    // lots again with the new supply. O(sources + lots + segments), no geometry: the same result Solve would give.
    // False when the last Solve does not match these lists (the caller then solves).
    bool Resupply(const ZoneGrid& zones, UtilReport& out) const {
        const int B = (int)items.size(), Z = (int)zones.lots.size(), L = (int)lines.size();
        if (elec_.total <= 0 || elec_.src.size() != (size_t)B || elec_.lot.size() != (size_t)Z || elec_.line.size() != (size_t)L ||
            out.lots.size() != (size_t)Z)
            return false;
        const int k = (int)Util::Electricity, total = elec_.total;
        std::vector<float> supply(total, 0.0f), backup(total, 0.0f), demand(total, 0.0f), avail(total, 0.0f);
        out.supply[k] = out.demand[k] = out.used[k] = 0.0f, out.unserved[k] = 0;
        for (int i = 0; i < B; ++i) {
            if (!items[i].alive) continue;
            const UtilDef& D = UtilDefs()[items[i].def];
            if (D.kind != Util::Electricity) continue;
            const float sup = D.supply * OutputScale(D);
            (D.backup ? backup : supply)[elec_.src[(size_t)i]] += sup;
            out.supply[k] += D.backup ? 0.0f : sup;
        }
        for (int z = 0; z < Z; ++z) {
            const Lot& l = zones.lots[z];
            if (!l.alive || !l.Built()) continue;
            const float dem = UtilDemandPerCell(Util::Electricity, l.type) * (float)(l.w * l.d);
            demand[elec_.lot[(size_t)z]] += dem, out.demand[k] += dem;
        }
        for (int r = 0; r < total; ++r)
            if (supply[r] > 0 || backup[r] > 0) avail[r] = supply[r] + (demand[r] > supply[r] ? backup[r] : 0.0f);
        for (int z = 0; z < Z; ++z) {
            const Lot& l = zones.lots[z];
            if (!l.alive || !l.Built()) continue;
            const int r = elec_.lot[(size_t)z];
            const float dem = UtilDemandPerCell(Util::Electricity, l.type) * (float)(l.w * l.d);
            const bool ok = avail[r] + 1e-6f >= dem;
            if (ok) avail[r] -= dem, out.used[k] += dem;
            else ++out.unserved[k];
            out.lots[z].elec = ok;
        }
        out.segLive[k].assign(elec_.seg.size(), 0);
        for (size_t s = 0; s < elec_.seg.size(); ++s)
            if (elec_.seg[s] >= 0) out.segLive[k][s] = (supply[elec_.seg[s]] > 0 || backup[elec_.seg[s]] > 0) ? 1 : 0;
        for (int i = 0; i < L; ++i)
            if (elec_.line[(size_t)i] >= 0) out.lineLive[i] = (supply[elec_.line[(size_t)i]] > 0 || backup[elec_.line[(size_t)i]] > 0) ? 1 : 0;
        return true;
    }

private:
    uint64_t nextId_ = 1;
    std::vector<int> endSeg_;                  // per line end: the segment UtilRoadNear found (cached, see Solve)
    uint64_t endSegNet_ = ~0ull, endSegUtil_ = ~0ull;
    struct ElecNets { int total = 0; std::vector<int> src, lot, seg, line; }; // DSU roots of the last Solve's electricity pass
    ElecNets elec_;

    struct Dsu {
        std::vector<int> p;
        explicit Dsu(int n) : p((size_t)n) { std::iota(p.begin(), p.end(), 0); }
        int Find(int x) {
            while (p[x] != x) x = p[x] = p[p[x]];
            return x;
        }
        void Join(int a, int b) { p[Find(a)] = Find(b); }
    };

    // Joins every lot (and electricity source) to the ones within conductRange of it, through a bucket grid: O(n).
    void Conduct(const ZoneGrid& zones, int B, int srcBase, int lotBase, Dsu& dsu) const {
        struct E { BlockRect r; int node; };
        std::vector<E> es;
        for (int z = 0; z < (int)zones.lots.size(); ++z) {
            const Lot& l = zones.lots[z];
            if (l.alive && l.Built()) es.push_back({RectOf(l.c, l.front, l.w, l.d), lotBase + z});
        }
        for (int i = 0; i < B; ++i)
            if (items[i].alive && UtilDefs()[items[i].def].kind == Util::Electricity)
                es.push_back({RectOf(items[i].c, items[i].front, UtilDefs()[items[i].def].w, UtilDefs()[items[i].def].d), srcBase + i});
        const float cell = 64.0f;
        std::unordered_map<uint64_t, std::vector<int>> grid;
        auto key = [](int x, int z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; };
        std::vector<std::array<int, 4>> span(es.size());
        for (size_t i = 0; i < es.size(); ++i) {
            const BlockRect& r = es[i].r;
            const float ext = std::sqrt(r.hw * r.hw + r.hd * r.hd) + conductRange, extz = ext; // the rect's circumcircle
            span[i] = {(int)std::floor((r.c.x - ext) / cell), (int)std::floor((r.c.z - extz) / cell), (int)std::floor((r.c.x + ext) / cell), (int)std::floor((r.c.z + extz) / cell)};
            for (int gz = span[i][1]; gz <= span[i][3]; ++gz)
                for (int gx = span[i][0]; gx <= span[i][2]; ++gx) grid[key(gx, gz)].push_back((int)i);
        }
        for (auto& [k, list] : grid)
            for (size_t a = 0; a < list.size(); ++a)
                for (size_t b = a + 1; b < list.size(); ++b) {
                    const E &ea = es[(size_t)list[a]], &eb = es[(size_t)list[b]];
                    if (dsu.Find(ea.node) != dsu.Find(eb.node) && Overlaps(ea.r, eb.r, conductRange)) dsu.Join(ea.node, eb.node);
                }
    }

    void Validate(const PlaceContext& cx, const UtilDef& D, Placement& r) const {
        const BlockRect me = RectOf(r.c, r.front, D.w, D.d);
        const Frame fr(r.c, r.front);
        const float W = D.W(), H = D.D();
        const bool shore = UtilDefShore((int)(&D - UtilDefs().data()));
        if (shore && cx.HasWater()) { // the same idea as a harbour's coast: water right behind the lot (the map's sea)
            bool wet = false;
            for (float back = 0.0f; back <= 24.0f && !wet; back += 4.0f)
                for (int ix = 0; ix <= 4 && !wet; ++ix) wet = cx.IsWater(fr.World(W * (ix / 4.0f - 0.5f), -H * 0.5f - back), 0.3f);
            if (!wet) {
                r.error = "Must be built on the shoreline, back to the water";
                return;
            }
        }
        float lo = 1e9f, hi = -1e9f;
        for (int iz = 0; iz <= 4; ++iz)
            for (int ix = 0; ix <= 4; ++ix) {
                const V2 q = fr.World(W * (ix / 4.0f - 0.5f), H * (iz / 4.0f - 0.5f));
                const float g = cx.Ground(q);
                lo = std::min(lo, g), hi = std::max(hi, g);
                if (cx.IsWater(q, 0.3f) && !(shore && iz == 0)) { // a shore building's back edge may stand in the water
                    r.error = "Cannot build on water";
                    return;
                }
            }
        r.relief = hi - lo;
        if (r.relief > std::max(5.0f, std::max(W, H) * 0.12f)) {
            r.error = "Ground too steep";
            return;
        }
        if (cx.net)
            for (float z = -H * 0.5f; z <= H * 0.5f + 0.01f; z += H * 0.25f)
                for (float x = -W * 0.5f; x <= W * 0.5f + 0.01f; x += W * 0.25f) {
                    float t, d;
                    if (cx.net->SegmentAt(fr.World(x, z), 0.0f, t, d) >= 0) {
                        r.error = "A road is in the way";
                        return;
                    }
                }
        if (cx.zones) {
            for (const BlockRect& b : cx.zones->blockers)
                if (Overlaps(me, b, 0.5f)) {
                    r.error = "Overlaps another building";
                    return;
                }
            for (const Lot& l : cx.zones->lots)
                if (l.alive && Overlaps(me, RectOf(l.c, l.front, l.w, l.d), 0.5f)) {
                    r.error = "Overlaps a building";
                    return;
                }
        }
        for (const UtilBuilding& b : items)
            if (b.alive && Overlaps(me, RectOf(b.c, b.front, UtilDefs()[b.def].w, UtilDefs()[b.def].d), 0.5f)) {
                r.error = "Overlaps another utility building";
                return;
            }
    }
};

// Solves only when the road network, the zone grid or the utilities changed since the last solve; the report is the
// cache. The city sim reads its per-lot result (sim_coverage.h ApplyUtilities) without solving again.
struct UtilSolveCache {
    uint64_t net = ~0ull, zones = ~0ull, util = ~0ull, output = ~0ull;
    uint64_t solves = 0, resupplies = 0; // resupplies: re-served with new solar / wind output only (no geometry)
    bool Update(UtilWorld& w, const Network& n, const ZoneGrid& z, UtilReport& out) {
        if (net == n.version && zones == z.version && util == w.version && output == w.outputVersion) return false;
        if (net == n.version && zones == z.version && util == w.version && w.Resupply(z, out)) { // only the weather's output moved
            output = w.outputVersion, ++solves, ++resupplies;
            return true;
        }
        net = n.version, zones = z.version, output = w.outputVersion;
        w.Solve(n, z, out);
        util = w.version;
        ++solves;
        return true;
    }
};

// Dimensions of a worked-out line, for the cost chip.
inline int UtilLineCost(const UtilLine& l) { return (int)(l.Length() * UtilLineCostPerMetre(l.kind, l.elev)); }

} // namespace city
} // namespace tbd
