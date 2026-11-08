// Interaction with the built city: what a tap / click is on (info card, bulldoze), demolishing it (and undoing that), the
// world plane a zone drag stays on, and the rotate dial's angle math. Header only, headless (tests\interact_test.cpp);
// the host side is src\city_interact.inl.
//
//   Pick       the thing at a ground point, nearest first among small things (vehicles, people), then buildings (a zoned
//              lot the ray hit, else any lot / utility / transport / industry footprint under the point), the road, a
//              utility line, a zone cell, the bare terrain. Bulldoze asks for the same minus vehicles, people, terrain and
//              empty cells.
//   Demolish   the existing removal paths (UtilWorld::Remove, TransportWorld::Remove, ZoneGrid::Demolish / Paint,
//              Network::RemoveSeg through RoadTool::PushUndo, the industry site list), so every version and `removed`
//              list the host and the sims watch moves exactly as with the tools' own bulldozers. Each returns a DozeUndo;
//              Restore puts it back while nothing it depends on changed since (no refunds and no second charge: the
//              construction charges key on the building ids and lane-metres, see city_sim.inl SimConstructionSync).
//   PlaneLock  a drag that starts on the ground keeps intersecting the horizontal world plane through that first point, so
//              the zone box is a rectangle on the world grid whatever the camera angle or the terrain under the finger.
//   Dial       the rotate toggle: the ghost stays put, a drag anywhere turns it by the change of the finger's bearing
//              around the ghost; Snap on = 15 degree steps (road-snapped: 90), Snap off = free (road-snapped buildings whose
//              tool can leave the road do so; the others keep quarter turns).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "industry.h"
#include "placement_rotation.h"
#include "road_tool.h"
#include "transport.h"
#include "utilities.h"
#include "zoning.h"

namespace tbd {
namespace city {

// ---------------------------------------------------------------------------------------------------- the world plane
inline bool RayPlaneY(const Ray& ray, float y, Vec3& hit) {
    if (std::fabs(ray.dir.y) < 1e-6f) return false;
    const float t = (y - ray.origin.y) / ray.dir.y;
    if (t <= 0.0f) return false;
    hit = ray.origin + ray.dir * t;
    return true;
}
struct PlaneLock {
    bool on = false;
    float y = 0.0f;
    void Begin(float groundY) { on = true, y = groundY; }
    void End() { on = false; }
    // Where the ray meets the locked plane (false when unlocked, or looking away from it).
    bool Hit(const Ray& ray, Vec3& out) const { return on && RayPlaneY(ray, y, out); }
};

// ---------------------------------------------------------------------------------------------------- the rotate dial
inline float WrapDeg180(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}
inline float Norm360(float d) {
    d = std::fmod(d, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d >= 359.995f ? 0.0f : d;
}
inline float SnapDeg(float d, float step) { return step > 0.0f ? std::round(d / step) * step : d; }
// The heading from c to p in the tools' sense (front = (sin a, cos a)).
inline float BearingDeg(V2 c, V2 p) { return DegOfFront(p - c); }
// The dial's step: road-snapped buildings turn in quarter turns while Snap is on (or when their tool cannot leave the
// road); free ones in 15 degrees; Snap off is free (whole degrees).
inline float DialStep(bool snap, bool roadSnapped, bool canLeaveRoad) {
    if (roadSnapped) return snap || !canLeaveRoad ? 90.0f : 1.0f;
    return snap ? 15.0f : 1.0f;
}
struct RotateDial {
    bool on = false;
    float origDeg = 0.0f; // the tool's rotation when the dial came up (Cancel goes back to it)
    float deg = 0.0f;     // the rotation now
    V2 centre{};          // the ghost's centre: bearings are measured around it
    bool dragging = false;
    float bearing0 = 0.0f, deg0 = 0.0f;
    void Begin(float toolDeg, V2 c) { on = true, origDeg = deg = Norm360(toolDeg), centre = c, dragging = false; }
    void Press(V2 ground) { dragging = true, bearing0 = BearingDeg(centre, ground), deg0 = deg; }
    // A drag to `ground`: the rotation follows the change of bearing, quantized by step. True when it changed.
    bool Drag(V2 ground, float step) {
        if (!dragging || Len(ground - centre) < 0.5f) return false;
        const float raw = deg0 + WrapDeg180(BearingDeg(centre, ground) - bearing0);
        const float next = Norm360(SnapDeg(raw, step));
        if (std::fabs(WrapDeg180(next - deg)) < 1e-3f) return false;
        deg = next;
        return true;
    }
    void Release() { dragging = false; }
    // The turn from where it started, -180 .. 180 (what the ring's label shows).
    float Turn() const { return WrapDeg180(deg - origDeg); }
};

// ---------------------------------------------------------------------------------------------------- picking
enum class PickKind : uint8_t { None, Vehicle, Person, Lot, Util, Transport, IndSite, Waste, Road, UtilLine, ZoneCell, Terrain };
inline const char* PickKindName(PickKind k) {
    static const char* const n[] = {"nothing", "vehicle", "person", "building", "utility building", "service building", "industry", "waste building",
                                    "road", "utility line", "zone cell", "terrain"};
    return n[(int)k];
}
// Something small the host knows the position of (a car, a bus, a person): picked when within `radius` of the point.
struct PickPoint {
    V2 p{};
    float radius = 3.0f;
    PickKind kind = PickKind::Vehicle;
    int index = -1; // the host's own index (its list is told apart by kind and `sub`)
    int sub = 0;    // which list: the host's choice (herd car, flow car, transit vehicle ...)
};
struct PickWorld {
    const Network* net = nullptr;
    const ZoneGrid* zones = nullptr;
    const UtilWorld* util = nullptr;
    const TransportWorld* transport = nullptr;
    const IndustryWorld* ind = nullptr;
    const std::vector<PickPoint>* agents = nullptr;
    std::function<int(V2)> waste;  // the host's point waste building under p, or -1
    int lotUnderRay = -1;          // a zoned lot the view ray hit (its box), index into zones->lots; wins over the ground point
    bool agentsOn = true, terrainOn = true, emptyCellsOn = true, linesVisibleOnly = true;
};
struct PickHit {
    PickKind kind = PickKind::None;
    int index = -1;
    int sub = 0;
    V2 at{};
    bool operator==(const PickHit& o) const { return kind == o.kind && index == o.index && sub == o.sub; }
    bool operator!=(const PickHit& o) const { return !(*this == o); }
};
inline bool LotContains(const Lot& l, V2 p, float pad) { return BlockRect{l.c, l.front, l.w * kCell * 0.5f, l.d * kCell * 0.5f}.Contains(p, pad); }

// The thing at ground point p. tol: metres of slack for a finger (roads, lines, footprints).
inline PickHit Pick(const PickWorld& w, V2 p, float tol) {
    PickHit h;
    h.at = p;
    if (w.agentsOn && w.agents) { // the nearest small thing within its radius
        float best = 1e30f;
        for (const PickPoint& a : *w.agents) {
            const float d = Len(a.p - p);
            if (d <= a.radius + tol && d < best) best = d, h.kind = a.kind, h.index = a.index, h.sub = a.sub;
        }
        if (h.kind != PickKind::None) return h;
    }
    if (w.zones && w.lotUnderRay >= 0 && w.lotUnderRay < (int)w.zones->lots.size() && w.zones->lots[(size_t)w.lotUnderRay].alive) {
        h.kind = PickKind::Lot, h.index = w.lotUnderRay;
        return h;
    }
    if (w.util) {
        const int i = w.util->HitTest(p);
        if (i >= 0) return h.kind = PickKind::Util, h.index = i, h;
    }
    if (w.transport) {
        const int i = w.transport->HitTest(p);
        if (i >= 0) return h.kind = PickKind::Transport, h.index = i, h;
    }
    if (w.waste) {
        const int i = w.waste(p);
        if (i >= 0) return h.kind = PickKind::Waste, h.index = i, h;
    }
    if (w.ind) {
        const int i = w.ind->SiteAt(p);
        if (i >= 0) return h.kind = PickKind::IndSite, h.index = i, h;
    }
    if (w.zones) // a lot by its footprint (the ray missed its box: a low house seen from above, a site still being built)
        for (size_t i = 0; i < w.zones->lots.size(); ++i)
            if (w.zones->lots[i].alive && LotContains(w.zones->lots[i], p, 0.0f)) return h.kind = PickKind::Lot, h.index = (int)i, h;
    if (w.net) {
        float t, d;
        const int s = w.net->SegmentAt(p, tol, t, d);
        if (s >= 0) return h.kind = PickKind::Road, h.index = s, h;
    }
    if (w.util) {
        const int l = w.util->LineAt(p, 4.0f + tol, w.linesVisibleOnly);
        if (l >= 0) return h.kind = PickKind::UtilLine, h.index = l, h;
    }
    if (w.zones) {
        const int c = w.zones->CellAt(p);
        if (c >= 0 && (w.emptyCellsOn || w.zones->cells[(size_t)c].zone != kNoZone)) return h.kind = PickKind::ZoneCell, h.index = c, h;
    }
    if (w.terrainOn) h.kind = PickKind::Terrain;
    return h;
}

// ---------------------------------------------------------------------------------------------------- demolishing
struct DozeWorld {
    RoadTool* roads = nullptr; // its network, and its undo stack (a bulldozed road is undone by RoadTool::Undo)
    ZoneGrid* zones = nullptr;
    UtilWorld* util = nullptr;
    TransportWorld* transport = nullptr;
    IndustryWorld* ind = nullptr;
};
// What changed, so the host refreshes exactly what the tools' own bulldozers refresh.
struct DozeDirty {
    bool roads = false, zones = false, util = false, transport = false, industry = false;
    bool blockers = false; // a utility / transport footprint went or came back: the zone grid's blockers (TransportRefreshZoning)
    uint64_t industryId = 0; // the industry site that went (the host drops its drawing cache)
};
struct DozeUndo {
    PickKind kind = PickKind::None;
    int index = -1;
    uint64_t id = 0;
    uint64_t netVersion = 0;   // roads: the network right after the removal (undo only while it is unchanged)
    uint64_t zonesBuilt = 0;   // zone cells / lots: the cell set they were indices into
    std::vector<std::pair<int, uint8_t>> cells; // zone cells: each one's zone before
    std::vector<std::pair<int, uint64_t>> lots; // lots that came down with them (index, id)
    IndSite site;              // an industry site, as it was
};

// Restores lot li (if it is still the same dead lot and its cells are free again).
inline bool RestoreLot(ZoneGrid& z, int li, uint64_t id) {
    if (li < 0 || li >= (int)z.lots.size() || z.lots[(size_t)li].id != id || z.lots[(size_t)li].alive) return false;
    Lot& l = z.lots[(size_t)li];
    for (int ci : l.cells)
        if (ci < 0 || ci >= (int)z.cells.size() || z.cells[(size_t)ci].lot >= 0 || z.cells[(size_t)ci].zone == kNoZone) return false;
    l.alive = true;
    for (int ci : l.cells) z.cells[(size_t)ci].lot = li;
    z.removed.erase(std::remove(z.removed.begin(), z.removed.end(), id), z.removed.end());
    ++z.version;
    return true;
}

// Removes what h points at. False (and nothing changed) when it is not something that can be demolished.
inline bool Demolish(DozeWorld& w, const PickHit& h, DozeUndo& u, DozeDirty& dirty) {
    u = DozeUndo{};
    u.kind = h.kind, u.index = h.index;
    switch (h.kind) {
    case PickKind::Lot: {
        if (!w.zones || h.index < 0 || h.index >= (int)w.zones->lots.size() || !w.zones->lots[(size_t)h.index].alive) return false;
        u.id = w.zones->lots[(size_t)h.index].id, u.zonesBuilt = w.zones->builtFrom;
        w.zones->Demolish(h.index); // the zone stays painted: it grows again (CS2)
        return dirty.zones = true, true;
    }
    case PickKind::Util:
        if (!w.util || h.index < 0 || h.index >= (int)w.util->items.size()) return false;
        u.id = w.util->items[(size_t)h.index].id;
        if (!w.util->Remove(h.index)) return false;
        return dirty.util = dirty.blockers = true, true;
    case PickKind::UtilLine:
        if (!w.util || h.index < 0 || h.index >= (int)w.util->lines.size()) return false;
        u.id = w.util->lines[(size_t)h.index].id;
        if (!w.util->RemoveLine(h.index)) return false;
        return dirty.util = true, true;
    case PickKind::Transport:
        if (!w.transport || h.index < 0 || h.index >= (int)w.transport->items.size()) return false;
        u.id = w.transport->items[(size_t)h.index].id;
        if (!w.transport->Remove(h.index)) return false;
        return dirty.transport = dirty.blockers = true, true;
    case PickKind::IndSite:
        if (!w.ind || h.index < 0 || h.index >= (int)w.ind->sites.size()) return false;
        u.site = w.ind->sites[(size_t)h.index], u.id = u.site.id;
        w.ind->sites.erase(w.ind->sites.begin() + h.index);
        ++w.ind->version;
        dirty.industryId = u.id;
        return dirty.industry = true, true;
    case PickKind::Road: {
        if (!w.roads || h.index < 0 || h.index >= (int)w.roads->net.segs.size() || !w.roads->net.segs[(size_t)h.index].alive) return false;
        w.roads->PushUndo();
        w.roads->net.RemoveSeg(h.index);
        u.netVersion = w.roads->net.version;
        return dirty.roads = true, true;
    }
    case PickKind::ZoneCell: {
        if (!w.zones || h.index < 0 || h.index >= (int)w.zones->cells.size() || w.zones->cells[(size_t)h.index].zone == kNoZone) return false;
        u.zonesBuilt = w.zones->builtFrom;
        const ZoneCell& c = w.zones->cells[(size_t)h.index];
        if (c.lot >= 0) u.lots.push_back({c.lot, w.zones->lots[(size_t)c.lot].id});
        u.cells.push_back({h.index, c.zone});
        w.zones->Paint(h.index, kNoZone);
        return dirty.zones = true, true;
    }
    default: return false;
    }
}

// Clears the zone of every listed cell (a bulldoze area), the lots on them coming down with it. One undo for all.
inline bool DezoneCells(ZoneGrid& z, const std::vector<int>& cells, DozeUndo& u, DozeDirty& dirty) {
    u = DozeUndo{};
    u.kind = PickKind::ZoneCell, u.zonesBuilt = z.builtFrom;
    for (int ci : cells) {
        if (ci < 0 || ci >= (int)z.cells.size() || z.cells[(size_t)ci].zone == kNoZone) continue;
        const ZoneCell& c = z.cells[(size_t)ci];
        if (c.lot >= 0 && std::none_of(u.lots.begin(), u.lots.end(), [&](const std::pair<int, uint64_t>& q) { return q.first == c.lot; }))
            u.lots.push_back({c.lot, z.lots[(size_t)c.lot].id});
        u.cells.push_back({ci, c.zone});
    }
    for (const auto& [ci, zone] : u.cells) (void)zone, z.Paint(ci, kNoZone);
    if (u.cells.empty()) return false;
    return dirty.zones = true, true;
}

// Puts back what u removed. False when the world moved on (a new road network, new zone cells, the site list changed):
// then nothing is touched.
inline bool Restore(DozeWorld& w, const DozeUndo& u, DozeDirty& dirty) {
    switch (u.kind) {
    case PickKind::Lot:
        if (!w.zones || w.zones->builtFrom != u.zonesBuilt || !RestoreLot(*w.zones, u.index, u.id)) return false;
        return dirty.zones = true, true;
    case PickKind::Util: {
        if (!w.util || u.index < 0 || u.index >= (int)w.util->items.size()) return false;
        UtilBuilding& b = w.util->items[(size_t)u.index];
        if (b.id != u.id || b.alive) return false;
        b.alive = true, b.noRoadMinutes = 0;
        w.util->removed.erase(std::remove(w.util->removed.begin(), w.util->removed.end(), u.id), w.util->removed.end());
        ++w.util->version;
        return dirty.util = dirty.blockers = true, true;
    }
    case PickKind::UtilLine: {
        if (!w.util || u.index < 0 || u.index >= (int)w.util->lines.size()) return false;
        UtilLine& l = w.util->lines[(size_t)u.index];
        if (l.id != u.id || l.alive) return false;
        l.alive = true;
        ++w.util->version;
        return dirty.util = true, true;
    }
    case PickKind::Transport: {
        if (!w.transport || u.index < 0 || u.index >= (int)w.transport->items.size()) return false;
        Placed& b = w.transport->items[(size_t)u.index];
        if (b.id != u.id || b.alive) return false;
        b.alive = true;
        w.transport->removed.erase(std::remove(w.transport->removed.begin(), w.transport->removed.end(), u.id), w.transport->removed.end());
        ++w.transport->version;
        return dirty.transport = dirty.blockers = true, true;
    }
    case PickKind::IndSite:
        if (!w.ind || u.index < 0 || u.index > (int)w.ind->sites.size()) return false;
        w.ind->sites.insert(w.ind->sites.begin() + u.index, u.site);
        ++w.ind->version;
        return dirty.industry = true, true;
    case PickKind::Road:
        if (!w.roads || w.roads->net.version != u.netVersion || !w.roads->Undo()) return false;
        return dirty.roads = true, true;
    case PickKind::ZoneCell:
        if (!w.zones || w.zones->builtFrom != u.zonesBuilt) return false;
        for (const auto& [ci, zone] : u.cells)
            if (ci >= 0 && ci < (int)w.zones->cells.size()) w.zones->Paint(ci, zone);
        for (const auto& [li, id] : u.lots) RestoreLot(*w.zones, li, id);
        return dirty.zones = true, true;
    default: return false;
    }
}

} // namespace city
} // namespace tbd
