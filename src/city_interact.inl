// ------------------------------------------------------------------- interacting with the city [interact]
// Included by game_core.inl after city_buildings.inl (it reads every tool and sim above). The logic that can be headless is
// src/city/city_interact.h (pick, demolish + undo, the world plane, the rotate dial) and src/game/touch_gesture.h (tap vs
// drag); this file wires them to the game:
//
//   Placing a building on touch (utilities, transport / services, industry hubs, waste buildings; roads and rails are the
//   road tool's): one finger never builds by itself. A drag pans the camera; a tap puts the ghost where it landed (the
//   ghost waits in the middle of the view until then); a tap on the ghost, or the placement bar's check, builds it there.
//   With a mouse a click still builds at the cursor (the ghost follows the hover).
//   Rotate: the placement bar's Rotate toggle. The ghost locks where it is; a ring around it shows a green line to the way
//   it faced when the ring came up and a blue one to the way it faces now, with the turn in degrees. A drag anywhere turns
//   it (by the change of the finger's bearing around the ghost). The Snap switch beside it is shown every time: on = 15
//   degree steps (90 for a building snapped to a road), off = free (a utility then leaves the road snap; industry hubs and
//   waste buildings always face a road, so they keep quarter turns). Check keeps the angle, cross goes back. R / Shift+R and
//   Ctrl+wheel still turn it on the PC.
//   Bulldoze: the dock's own button. Tap mode: hover (PC) / a tap (touch) marks what would go in red: a zoned building
//   (its zone stays and grows again), a utility / service / transport / industry / waste building, a road or rail
//   segment, a utility line, a zoned cell. A click demolishes it at once; on touch the first tap marks it and a second tap on
//   the same thing, or the check, demolishes it. Zone area mode: a drag paints a box on the world plane of its first cell;
//   every zoned cell in it is cleared on release (mouse) or on the check (touch). Undo (the bar's undo) puts back the last
//   demolition while nothing it depends on changed since; there are no refunds (the construction charges never refund).
//   Info: with no tool out a tap / click picks a car, a bus, a person, a building, a road, a line, a zone cell or the bare
//   ground and opens a compact card with what the sim really has about it (src/gui/info_card.h). The lot keeps its yellow
//   outline (CityPickedOutline); the rest get a light outline here.

namespace gdock { bool ToolActive(); void CloseTools(); } // glass_dock_host.inl
bool g_touchInput = false; // the touch host (game_host.cpp TouchBuilds) says so: taps pick / place, one finger pans

using tbd::city::PickHit;
using tbd::city::PickKind;

// ---------------------------------------------------------------- the ghost of the building being placed
struct PlacePin {
    bool on = false;
    tbd::Vec3 at{};
    int key = 0; // which tool / item it was set for (a new pick drops it)
} g_placePin;
struct LastGhost { bool valid = false; tbd::Vec3 at{}; int key = 0; } g_lastGhost; // where a ghost last stood (the mouse may be on the bar now)

int PlaceGhostKey() {
    if (g_utilTool.active && g_utilTool.def >= 0) return 1000 + g_utilTool.def;
    if (g_transportTool.active && g_transportTool.def >= 0) return 2000 + g_transportTool.def;
    if (g_indTool.active && g_indTool.mode == IndustryTool::Place) return 3000 + g_indTool.kind * 10 + g_indTool.point;
    return 0;
}

// Where the building being placed stands: the pinned point (a touch tap, the rotate dial), else on touch the middle of the
// view, else the mouse's ground point (city_utilities / city_transport / city_industry .inl call this from their cursors).
bool PlaceCursorHit(tbd::Vec3& hit) {
    if (BuildingGhostKind() != 0) {
        if (g_placePin.on && g_placePin.key == PlaceGhostKey()) return hit = g_placePin.at, true;
        if (g_touchInput)
            return GroundHit(g_engine.camera.ScreenRay((float)g_engine.vpW * 0.5f, (float)g_engine.vpH * 0.5f, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    }
    const bool overGui = g_gui.WantsMouseAt((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY);
    const bool have = g_terrainEdit.mouseX >= 0 && !overGui &&
                      GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    if (have && BuildingGhostKind() != 0) g_lastGhost.valid = true, g_lastGhost.at = hit, g_lastGhost.key = PlaceGhostKey();
    return have;
}

// The ghost as last planned: centre, front, size (metres), ground. False when none is shown.
bool GhostNow(tbd::city::V2& c, tbd::city::V2& front, float& W, float& D, float& y) {
    using namespace tbd::city;
    if (g_utilTool.active && g_utilTool.def >= 0 && !g_utilTool.bulldoze) {
        if (!g_utilIn.have) return false;
        const UtilDef& d = UtilDefs()[g_utilTool.def];
        c = g_utilIn.plan.c, front = g_utilIn.plan.front, W = d.W(), D = d.D(), y = g_utilIn.plan.y;
        return true;
    }
    if (g_transportTool.active && g_transportTool.def >= 0 && !g_transportTool.lines && !g_transportTool.bulldoze) {
        if (!g_transportIn.have) return false;
        const BuildingDef& d = Defs()[g_transportTool.def];
        c = g_transportIn.plan.c, front = g_transportIn.plan.front, W = d.W(), D = d.D(), y = g_transportIn.plan.y;
        return true;
    }
    if (g_indTool.active && g_indTool.mode == IndustryTool::Place && g_indTool.have) {
        if (g_indTool.point) {
            if (!g_wasteGhost.shown) return false;
            const WasteType& t = kWasteTypes[g_wasteGhost.b.type];
            c = g_wasteGhost.b.c, front = g_wasteGhost.b.front, W = t.w, D = t.d, y = g_wasteGhost.b.y;
            return true;
        }
        if (!g_indTool.ghostShown) return false;
        c = g_indTool.ghost.c, front = g_indTool.ghost.front, W = kHubW, D = kHubD, y = g_indTool.ghost.y;
        return true;
    }
    return false;
}
float* GhostRotation() {
    if (g_utilTool.active && g_utilTool.def >= 0) return &g_utilTool.rotation;
    if (g_transportTool.active && g_transportTool.def >= 0) return &g_transportTool.rotation;
    if (g_indTool.active && g_indTool.mode == IndustryTool::Place) return &g_indTool.rotation;
    return nullptr;
}
tbd::Ray InteractRay(int x, int y) { return g_engine.camera.ScreenRay((float)x, (float)y, (float)g_engine.vpW, (float)g_engine.vpH); }
float TouchPad() { return std::clamp(g_engine.camera.distance * 0.012f, 2.0f, 14.0f); } // metres of slack for a finger

// Builds the ghost where it stands now, through the tool's own click (its validation, toasts, charges). True when placed.
bool PlaceGhostNow() {
    const size_t before = g_util.items.size() + g_transport.items.size() + g_ind.sites.size() + g_wasteB.size();
    if (g_utilTool.active && g_utilTool.def >= 0) UtilClick();
    else if (g_transportTool.active && g_transportTool.def >= 0) TransportClick();
    else if (g_indTool.active && g_indTool.mode == IndustryTool::Place) IndustryPress();
    else return false;
    const bool placed = g_util.items.size() + g_transport.items.size() + g_ind.sites.size() + g_wasteB.size() != before;
    if (placed) g_placePin.on = false; // the next one waits in the middle again
    return placed;
}

// ---------------------------------------------------------------- the rotate dial
struct DialState {
    tbd::city::RotateDial d;
    tbd::city::V2 origFront{0, 1};
    int kind = 0;           // BuildingGhostKind when it came up
    bool snapRoadWas = true; // the utility tool's road snap (Snap off lets a utility leave the road)
    bool pinWas = false;    // the ghost was pinned before (touch): stays pinned after
    bool origPending = false; // the original facing comes from the first frame that plans the ghost
    float y = 0;
} g_dial;
bool g_dialSnap = true; // the Snap switch: remembered every time the dial comes up
float g_dialRelWas = 0;  // [snap] the tool's rotation before the dial (road-relative quarter turns when road-snapped)
bool g_dialWasSnapped = false; // [snap] the ghost stood beside a road when the dial came up

// [snap] While the Rotate dial is out every placement tool's road snap is off (building_snap.h): the ghost stays where it
// stood and turns freely (the Snap switch: 15 degree steps); closing the dial (Rotate again) returns to the road snap.
bool BuildingFreePlace() { return g_dial.d.on; }
// [snap tune] A fast drag (over ~1400 pt/s on the screen) is heading somewhere else: no road snap (building_snap.h
// SnapBesideRoadLatched lets go). Sampled at most once per frame's worth of time.
bool BuildingSnapFast() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static float lx = 0, ly = 0;
    static bool fast = false;
    const Clock::time_point now = Clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    if (dt < 1.0f / 240.0f) return fast;
    const tbd::gui::Input& in = g_gui.In();
    const float s = g_gui.Scale() > 0 ? g_gui.Scale() : 1.0f;
    const float v = std::hypot(in.mouseX - lx, in.mouseY - ly) / s / std::max(dt, 1e-3f);
    fast = in.down && dt < 0.25f && v > 1400.0f;
    lx = in.mouseX, ly = in.mouseY, last = now;
    return fast;
}
namespace gdock { bool Touch(); } // game/glass_dock_host.inl
// [snap tune] The capture's cap: a fingertip (28 pt) on touch, 24 px with the mouse, in metres at the screen centre.
float BuildingSnapReach() {
    const float cx = (float)g_engine.vpW * 0.5f, cy = (float)g_engine.vpH * 0.5f, d = 40.0f;
    tbd::Vec3 a, b;
    if (!GroundHit(g_engine.camera.ScreenRay(cx, cy, (float)g_engine.vpW, (float)g_engine.vpH), a) ||
        !GroundHit(g_engine.camera.ScreenRay(cx + d, cy, (float)g_engine.vpW, (float)g_engine.vpH), b))
        return -1.0f;
    const float mpp = std::hypot(b.x - a.x, b.z - a.z) / d;
    return mpp * (gdock::Touch() ? 28.0f * (g_gui.Scale() > 0 ? g_gui.Scale() : 1.0f) : 24.0f);
}
bool DialCanLeaveRoad() { return g_utilTool.active && g_utilTool.def >= 0; } // only the utilities' snap is a tool option
float DialStepNow() { return tbd::city::DialStep(g_dialSnap, false, true); }    // [snap] free: 15 degree steps or none
void DialApplySnap() { // the angle follows the Snap switch's steps
    if (float* rot = GhostRotation()) *rot = g_dial.d.deg = tbd::city::Norm360(tbd::city::SnapDeg(*rot, DialStepNow()));
}
void DialBegin() {
    using namespace tbd::city;
    const int kind = BuildingGhostKind();
    if (!kind) return;
    if (kind == 3) { BuildingRotate(90.0f); return; } // harbours / stations: the coast or the track sets it (its toast says so)
    tbd::Vec3 hit;
    if (!PlaceCursorHit(hit)) { // the mouse is on the placement bar: where the ghost last stood
        if (!g_lastGhost.valid || g_lastGhost.key != PlaceGhostKey()) { Toast(g_touchInput ? "TAP THE MAP TO PUT IT THERE FIRST" : "POINT AT THE MAP FIRST"); return; }
        hit = g_lastGhost.at;
    }
    V2 c{hit.x, hit.z}, f = FrontFromDeg(*GhostRotation());
    float W = 0, D = 0, y = hit.y;
    g_dial.origPending = !GhostNow(c, f, W, D, y); // not planned this frame: the first frame that plans it gives the facing
    g_dial.pinWas = g_placePin.on && g_placePin.key == PlaceGhostKey();
    // [snap] free from here on: the ghost locks where it stands (a road-snapped one at its snapped middle, not the cursor)
    // and its rotation becomes the absolute heading it has now
    g_placePin.on = true, g_placePin.at = g_dial.origPending ? hit : tbd::Vec3{c.x, y, c.z}, g_placePin.key = PlaceGhostKey();
    g_dial.kind = kind, g_dial.origFront = f, g_dial.y = y, g_dial.snapRoadWas = g_utilTool.snapRoad;
    g_dialRelWas = *GhostRotation();
    g_dialWasSnapped = !g_dial.origPending && Dot2(f, FrontFromDeg(g_dialRelWas)) < 0.999f; // its facing came from a road
    if (!g_dial.origPending) *GhostRotation() = DegOfFront(f);
    g_dial.d.Begin(*GhostRotation(), c);
    g_dial.d.origDeg = g_dialRelWas; // Cancel goes back to the road-relative rotation
    DialApplySnap();
}
void DialEnd(bool keep) {
    if (!g_dial.d.on) return;
    // [snap] closing the dial returns to the road snap: the road-relative rotation from before (the dialled heading only
    // lasts while the dial is out; the check builds the building as dialled, InteractBarAction)
    if (float* rot = GhostRotation()) *rot = !keep ? g_dial.d.origDeg : g_dialWasSnapped ? g_dialRelWas : *rot; // free-standing: keeps the dialled heading
    if (DialCanLeaveRoad()) g_utilTool.snapRoad = g_dial.snapRoadWas;
    g_dial.d.on = false, g_dial.d.dragging = false;
    if (!g_dial.pinWas && !g_touchInput) g_placePin.on = false; // the mouse: the ghost follows the cursor again
}
// The finger's point on the ghost's ground plane.
bool DialGround(const tbd::Ray& ray, tbd::city::V2& p) {
    tbd::Vec3 hit;
    if (!tbd::city::RayPlaneY(ray, g_dial.y, hit) && !GroundHit(ray, hit)) return false;
    p = {hit.x, hit.z};
    return true;
}

// ---------------------------------------------------------------- bulldoze
struct DozeTool {
    bool active = false;
    bool area = false;         // Zone area mode
    PickHit hover, pending;    // what a click would take (mouse) / the touch tap's mark, waiting for the confirm
    bool dragging = false;
    tbd::city::PlaneLock plane;
    tbd::city::V2 a{}, b{}, dir{1, 0};
    std::vector<int> cells;    // the box's zoned cells (dragging, or waiting for the confirm on touch)
    bool areaPending = false;
    struct Entry { tbd::city::DozeUndo u; bool waste = false; WasteBuilding wb; };
    std::vector<Entry> undo;
} g_doze;

int WasteAt(tbd::city::V2 p) {
    for (size_t i = 0; i < g_wasteB.size(); ++i)
        if (WasteContains(g_wasteB[i], p, 1.0f)) return (int)i;
    return -1;
}

tbd::city::PickWorld PickWorldNow(const tbd::Ray& ray, bool bulldoze, const std::vector<tbd::city::PickPoint>* agents) {
    tbd::city::PickWorld w;
    w.net = &g_roads.net, w.zones = &g_zones, w.util = &g_util, w.transport = &g_transport, w.ind = &g_ind;
    w.agents = agents;
    w.waste = WasteAt;
    w.linesVisibleOnly = !g_utilTool.underground;
    if (bulldoze) w.agentsOn = w.terrainOn = w.emptyCellsOn = false;
    uint64_t lot = 0;
    float t = 0;
    if (CityPickLot(ray, lot, t))
        for (size_t i = 0; i < g_zones.lots.size(); ++i)
            if (g_zones.lots[i].id == lot && g_zones.lots[i].alive) w.lotUnderRay = (int)i;
    return w;
}

std::string DozeName(const PickHit& h) {
    using namespace tbd::city;
    switch (h.kind) {
    case PickKind::Lot: return h.index < (int)g_zones.lots.size() ? ZoneTypes()[g_zones.lots[(size_t)h.index].type].name : "building";
    case PickKind::Util: return h.index < (int)g_util.items.size() ? UtilDefs()[g_util.items[(size_t)h.index].def].name : "building";
    case PickKind::Transport: return h.index < (int)g_transport.items.size() ? Defs()[g_transport.items[(size_t)h.index].def].name : "building";
    case PickKind::IndSite: return h.index < (int)g_ind.sites.size() ? IndTypeOf(g_ind.sites[(size_t)h.index].kind).name : "industry";
    case PickKind::Waste: return h.index < (int)g_wasteB.size() ? kWasteTypes[g_wasteB[(size_t)h.index].type].name : "building";
    case PickKind::Road: return h.index < (int)g_roads.net.segs.size() ? g_roads.net.Prof(h.index).name : "road";
    case PickKind::UtilLine: return h.index < (int)g_util.lines.size() ? (g_util.lines[(size_t)h.index].kind == Util::Electricity ? "power line" : "pipe") : "line";
    case PickKind::ZoneCell: return "zone";
    default: return PickKindName(h.kind);
    }
}

void DozeDirtyApply(const tbd::city::DozeDirty& d) {
    if (d.blockers) TransportRefreshZoning(); // the zone grid keeps clear of what is left (and grows back where it went)
    if (d.industryId) g_indCache.erase(d.industryId);
    if (d.industry) g_indTool.sel = -1, g_indTool.wsel = -1;
    if (d.zones && g_pickedLot) {
        bool alive = false;
        for (const auto& l : g_zones.lots) alive |= l.id == g_pickedLot && l.alive;
        if (!alive) ++g_pickedLotSerial, g_pickedLot = 0;
    }
}

void DozeDo(const PickHit& h) {
    if (h.kind == PickKind::None) return;
    DozeTool::Entry e;
    const std::string name = DozeName(h);
    if (h.kind == PickKind::Waste) {
        if (h.index < 0 || h.index >= (int)g_wasteB.size()) return;
        e.waste = true, e.wb = g_wasteB[(size_t)h.index];
        WasteRemove(h.index);
    } else {
        tbd::city::DozeWorld w{&g_roads, &g_zones, &g_util, &g_transport, &g_ind};
        tbd::city::DozeDirty dirty;
        if (!tbd::city::Demolish(w, h, e.u, dirty)) return;
        DozeDirtyApply(dirty);
    }
    g_doze.undo.push_back(std::move(e));
    if (g_doze.undo.size() > 32) g_doze.undo.erase(g_doze.undo.begin());
    Toast(RoadsUpper("Demolished: " + name));
}
void DozeArea(const std::vector<int>& cells) {
    DozeTool::Entry e;
    tbd::city::DozeDirty dirty;
    if (!tbd::city::DezoneCells(g_zones, cells, e.u, dirty)) { Toast("NO ZONED CELLS IN THE BOX"); return; }
    DozeDirtyApply(dirty);
    char b[64];
    std::snprintf(b, sizeof(b), "%zu ZONE CELLS CLEARED", e.u.cells.size());
    g_doze.undo.push_back(std::move(e));
    if (g_doze.undo.size() > 32) g_doze.undo.erase(g_doze.undo.begin());
    Toast(b);
}
void DozeUndoLast() {
    if (g_doze.undo.empty()) { Toast("NOTHING TO UNDO"); return; }
    DozeTool::Entry e = std::move(g_doze.undo.back());
    g_doze.undo.pop_back();
    bool ok = false;
    if (e.waste) ok = WasteBuild(e.wb);
    else {
        tbd::city::DozeWorld w{&g_roads, &g_zones, &g_util, &g_transport, &g_ind};
        tbd::city::DozeDirty dirty;
        ok = tbd::city::Restore(w, e.u, dirty);
        if (ok) DozeDirtyApply(dirty);
        if (ok && dirty.roads) sim_build::laneMetres = -1.0; // the road comes back free: no second charge for its lane-metres
    }
    Toast(ok ? "DEMOLITION UNDONE" : "CAN'T UNDO: THE CITY CHANGED SINCE");
}
void DozeClose() {
    g_doze.active = g_doze.dragging = g_doze.areaPending = false;
    g_doze.pending = g_doze.hover = {};
    g_doze.cells.clear();
    g_doze.plane.End();
}
void DozeBoxCells() {
    g_doze.cells.clear();
    std::vector<int> all;
    g_zones.BoxCells(g_doze.a, g_doze.b, g_doze.dir, all);
    for (int c : all)
        if (g_zones.cells[(size_t)c].zone != tbd::city::kNoZone) g_doze.cells.push_back(c);
}

// ---------------------------------------------------------------- info: what a tap with no tool picked
struct InfoSel {
    bool open = false;
    PickHit hit;
    uint64_t id = 0;   // the thing's own id where it has one (lots, buildings, lines, vehicles, people), to find it again
    uint64_t zonesBuilt = 0;
    tbd::Vec3 at{};    // where it was picked (terrain; cosmetic cars, which have no identity)
    float speed = 0;   // cosmetic car: its road's congested speed at the pick
} g_info;

// The things that move, at this moment: herd cars (or the flow's cosmetic cars), transit vehicles, people on foot.
std::vector<tbd::city::PickPoint> g_infoAgents;
std::vector<tbd::city::CarInstance> g_infoCosmetic;
void InfoAgents() {
    using namespace tbd::city;
    g_infoAgents.clear();
    if (g_herdsOwnCars) {
        for (size_t i = 0; i < herds_host::children.pool.size(); ++i) {
            const Child& c = herds_host::children.pool[i];
            CarInstance ci;
            if (c.active && c.visible && herds_host::children.Place(c, ci)) g_infoAgents.push_back({{ci.p.x, ci.p.z}, 2.6f, PickKind::Vehicle, (int)i, 0});
        }
    } else if (!g_simCars.empty() && g_segFlow.size() == g_roads.net.segs.size() && g_segFlowVersion == g_roads.net.version && g_engine.camera.distance <= 1200.0f) {
        CosmeticCars(g_roads.net, g_simSegGrid, g_segFlow, {g_engine.camera.target.x, g_engine.camera.target.z}, g_simAgentTime, g_simAgentParams, g_infoCosmetic);
        for (size_t i = 0; i < g_infoCosmetic.size(); ++i) g_infoAgents.push_back({{g_infoCosmetic[i].p.x, g_infoCosmetic[i].p.z}, 2.6f, PickKind::Vehicle, (int)i, 1});
    }
    for (size_t i = 0; i < g_transit.vehicles.size(); ++i) {
        const Vehicle& v = g_transit.vehicles[i];
        if (!v.alive) continue;
        const float r = v.mode == TMode::Train ? 26.0f : v.mode == TMode::Tram || v.mode == TMode::Subway ? 12.0f : v.mode == TMode::Plane ? 14.0f : v.mode == TMode::Boat ? 9.0f : 5.5f;
        g_infoAgents.push_back({v.pos, r, PickKind::Vehicle, (int)i, 2});
    }
    for (size_t i = 0; i < g_transit.citizens.size(); ++i)
        if (g_transit.citizens[i].alive) g_infoAgents.push_back({g_transit.citizens[i].pos, 1.1f, PickKind::Person, (int)i, 3});
    for (size_t i = 0; i < walkers_host::sim.walkers.size(); ++i) // [walkers] the people on the sidewalks
        g_infoAgents.push_back({walkers_host::sim.walkers[i].pos, 1.1f, PickKind::Person, (int)i, 4});
}

uint64_t InfoIdOf(const PickHit& h) {
    switch (h.kind) {
    case PickKind::Lot: return g_zones.lots[(size_t)h.index].id;
    case PickKind::Util: return g_util.items[(size_t)h.index].id;
    case PickKind::UtilLine: return g_util.lines[(size_t)h.index].id;
    case PickKind::Transport: return g_transport.items[(size_t)h.index].id;
    case PickKind::IndSite: return g_ind.sites[(size_t)h.index].id;
    case PickKind::Waste: return g_wasteB[(size_t)h.index].id;
    case PickKind::Vehicle: return h.sub == 0 ? herds_host::children.pool[(size_t)h.index].herdId : h.sub == 2 ? g_transit.vehicles[(size_t)h.index].id : 0;
    case PickKind::Person: return h.sub == 4 ? walkers_host::sim.walkers[(size_t)h.index].id : g_transit.citizens[(size_t)h.index].id; // [walkers]
    default: return 0;
    }
}

void InfoPick(const tbd::Ray& ray) {
    tbd::Vec3 hit;
    const bool ground = GroundHit(ray, hit);
    InfoAgents();
    tbd::city::PickWorld w = PickWorldNow(ray, false, &g_infoAgents);
    if (!ground && w.lotUnderRay < 0) { g_info.open = false; return; }
    if (!ground) hit = ray.origin; // the lot under the ray is all that counts
    const PickHit h = tbd::city::Pick(w, {hit.x, hit.z}, TouchPad() * (g_touchInput ? 1.0f : 0.4f));
    if (h.kind == PickKind::None) { g_info.open = false; return; }
    g_info.open = true, g_info.hit = h, g_info.at = hit, g_info.zonesBuilt = g_zones.builtFrom;
    g_info.id = InfoIdOf(h);
    if (h.kind == PickKind::Vehicle && h.sub == 1 && h.index < (int)g_infoCosmetic.size()) { // the flow's speed on its road
        float t, d;
        const int s = g_roads.net.SegmentAt({hit.x, hit.z}, 3.0f, t, d);
        g_info.speed = s >= 0 && (size_t)s < g_segFlow.size() ? tbd::city::FlowGraph::Speed(g_roads.net.Prof(s).cat) * std::max(0.1f, g_segFlow[(size_t)s].speedRatio) : 0.0f;
        g_info.at = g_infoCosmetic[(size_t)h.index].p;
    }
    if (h.kind == PickKind::Lot) { // the residents' list (SimShowLot) and the yellow outline
        ++g_pickedLotSerial, g_pickedLot = g_info.id;
    } else if (g_pickedLot) ++g_pickedLotSerial, g_pickedLot = 0;
}

// Finds the picked thing again (lists change under it). False: it is gone, the card closes.
bool InfoResolve() {
    PickHit& h = g_info.hit;
    auto byId = [&](auto& list, auto alive) {
        if (h.index >= 0 && h.index < (int)list.size() && list[(size_t)h.index].id == g_info.id && alive(list[(size_t)h.index])) return true;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == g_info.id && alive(list[i])) return h.index = (int)i, true;
        return false;
    };
    auto isAlive = [](const auto& x) { return x.alive; };
    auto always = [](const auto&) { return true; };
    switch (h.kind) {
    case PickKind::Lot: return byId(g_zones.lots, isAlive);
    case PickKind::Util: return byId(g_util.items, isAlive);
    case PickKind::UtilLine: return byId(g_util.lines, isAlive);
    case PickKind::Transport: return byId(g_transport.items, isAlive);
    case PickKind::IndSite: return byId(g_ind.sites, always);
    case PickKind::Waste: return byId(g_wasteB, always);
    case PickKind::Road: return h.index >= 0 && h.index < (int)g_roads.net.segs.size() && g_roads.net.segs[(size_t)h.index].alive;
    case PickKind::ZoneCell: return g_zones.builtFrom == g_info.zonesBuilt && h.index >= 0 && h.index < (int)g_zones.cells.size();
    case PickKind::Person: return h.sub == 4 ? byId(walkers_host::sim.walkers, always) : byId(g_transit.citizens, isAlive); // [walkers]
    case PickKind::Vehicle:
        if (h.sub == 0) {
            const auto& pool = herds_host::children.pool;
            return g_herdsOwnCars && h.index >= 0 && h.index < (int)pool.size() && pool[(size_t)h.index].active && pool[(size_t)h.index].herdId == g_info.id;
        }
        if (h.sub == 2) return byId(g_transit.vehicles, isAlive);
        return true; // a cosmetic car: what was seen at the pick
    default: return true;
    }
}

std::string Fmt(const char* f, double a, double b = 0, double c = 0) {
    char s[96];
    std::snprintf(s, sizeof(s), f, a, b, c);
    return s;
}
std::string Pct(double v) { return Fmt("%.0f%%", v * 100.0); }
std::string MoneyStr(long long v) { // $1,234
    std::string d = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return (v < 0 ? "-$" : "$") + o;
}
std::string HoodName(int h) { return h >= 0 && (size_t)h < herds_host::hoods.hoods.size() ? herds_host::hoods.hoods[(size_t)h].name : "-"; }
std::string UtilFlags(const tbd::city::UtilStatus& s) {
    std::string o;
    auto add = [&](const char* n, bool ok) { o += (o.empty() ? "" : ", ") + std::string(n) + (ok ? " ok" : " missing"); };
    add("power", s.elec), add("water", s.water), add("sewage", s.sewage);
    return o;
}
void AreaRows(tbd::gui::InfoCardData& d, float x, float z) { // the city cell's (the sim's 64 m block) shared numbers
    const int ci = g_citySim.cells.Find(tbd::city::CellKeyAt(x, z));
    if (ci < 0 || (size_t)ci >= g_citySim.sim.size()) return;
    const tbd::city::CellSim& c = g_citySim.sim[(size_t)ci];
    d.rows.push_back({"Block", Fmt("happiness %.0f%%, land value %.0f", c.happiness / 2.55, c.landValue)});
    if (SimEnvOn()) { // sim Phase 3: the block's pollution (0 .. 255 factor steps) and any flood
        const tbd::city::EnvCellOut e = SimEnvCell(ci);
        char pb[96];
        std::snprintf(pb, sizeof(pb), "air %d, noise %d, ground %d, tap water %d", e.air, e.noise, e.ground, e.water);
        std::string p = pb;
        if (e.floodMm) p += Fmt(", flooded %.1f m", e.floodMm / 1000.0);
        d.rows.push_back({"Pollution", p});
    }
}
// sim Phase 6: the company of a building (its rows in the card)
void GoodsRows(tbd::gui::InfoCardData& d, tbd::city::BOrigin o, uint64_t id) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (SimGoodsInspect(g_simBuildings.Find(o, id), rows))
        for (auto& r : rows) d.rows.push_back({r.first, r.second});
}

// sim Phase 2: land value, rent and development of a lot (city_sim_p2.inl)
bool SimLandInspect(uint64_t lot, std::vector<std::pair<std::string, std::string>>& rows);
void LandRows(tbd::gui::InfoCardData& d, uint64_t lot) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (SimLandInspect(lot, rows))
        for (auto& r : rows) d.rows.push_back({r.first, r.second});
}

// The card's text from the sim, honest: only what it keeps.
void InfoCardFill(tbd::gui::InfoCardData& d) {
    using namespace tbd::city;
    using tbd::gui::Rgba;
    const PickHit& h = g_info.hit;
    d = {};
    switch (h.kind) {
    case PickKind::Lot: {
        const Lot& l = g_zones.lots[(size_t)h.index];
        const ZoneType& zt = ZoneTypes()[l.type];
        static const char* const kIcon[] = {"house", "shop", "office", "industry"};
        d.title = zt.name, d.icon = kIcon[(int)zt.cat], d.accent = Rgba(79, 174, 91);
        d.sub = std::string(ZoneCatName(zt.cat)) + " - level " + std::to_string(l.level) + " - " + std::to_string(l.w) + " x " + std::to_string(l.d) + " cells";
        d.rows.push_back({"Status", l.Condemned() ? Fmt("condemned: no road, down in %.0f days", std::ceil(l.DemolishInMinutes() / kDayMinutes))
                                    : !l.Built() ? "under construction " + Pct(std::min(1.0f, l.progress))
                                    : !l.hasRoad ? "no road in front" : "built"});
        const LotCap cap = LotCapacity(l);
        if (cap.homes) {
            int cell = -1;
            uint32_t first = 0, count = 0;
            const int records = SimP1LotResidents(l.id); // Phase 1: the citizen records (-1: off, the old counters)
            const bool known = records >= 0 || SimPeople::LotSlots(g_citySim, l.id, cell, first, count);
            if (records >= 0) count = (uint32_t)records;
            d.rows.push_back({"Residents", (known ? std::to_string(count) : std::string("0")) + " of " + std::to_string(cap.homes)});
        }
        if (cap.Jobs()) d.rows.push_back({"Jobs", std::to_string(cap.Jobs()) + (zt.cat == ZoneCat::Commercial ? " (shop)" : zt.cat == ZoneCat::Office ? " (office)" : " (industry)")});
        if ((size_t)h.index < g_utilReport.lots.size()) d.rows.push_back({"Utilities", UtilFlags(g_utilReport.lots[(size_t)h.index])});
        if ((size_t)h.index < g_simFlags.size() && g_simFlags[(size_t)h.index]) {
            std::string p;
            for (int b = 0; b < kFlagCount; ++b)
                if (g_simFlags[(size_t)h.index] & (1u << b)) p += (p.empty() ? "" : ", ") + std::string(SimFlagText(b));
            d.rows.push_back({"Problems", p});
        }
        AreaRows(d, l.c.x, l.c.z);
        GoodsRows(d, BOrigin::Lot, l.id);
        LandRows(d, l.id);
        if (g_simCitizenTitle.rfind("Lot " + std::to_string(l.id), 0) == 0)
            for (const std::string& line : g_simCitizenLines) d.notes.push_back(line);
        break;
    }
    case PickKind::Util: {
        const UtilBuilding& b = g_util.items[(size_t)h.index];
        const UtilDef& D = UtilDefs()[b.def];
        const int k = (int)D.kind;
        static const char* const kKind[] = {"Electricity", "Water", "Sewage"};
        static const char* const kUnit[] = {"MW", "m3 a day", "m3 a day"};
        d.title = D.name, d.icon = D.kind == Util::Electricity ? "power" : D.kind == Util::Water ? "water" : "sewage", d.accent = Rgba(242, 177, 28);
        d.sub = std::string(kKind[k]) + (D.backup ? " - backup" : "");
        d.rows.push_back({"Status", b.Condemned() ? Fmt("condemned: no road, down in %.0f days", std::ceil(b.DemolishInMinutes() / kDayMinutes)) : b.hasRoad ? "operating" : "no road beside it"});
        d.rows.push_back({"Capacity", Fmt("%.0f ", D.supply) + kUnit[k]});
        if (g_util.OutputScale(D) != 1.0f || (D.shortName && (std::strcmp(D.shortName, "Solar") == 0 || std::strcmp(D.shortName, "Wind") == 0)))
            d.rows.push_back({"Output now", Fmt("%.0f ", D.supply * g_util.OutputScale(D)) + kUnit[k] + (std::strcmp(D.shortName, "Solar") == 0 ? " (sun)" : " (wind)")});
        d.rows.push_back({"City network", Fmt("%.0f of %.0f used", g_utilReport.used[k], g_utilReport.supply[k]) + std::string(" ") + kUnit[k]});
        d.rows.push_back({"Demand", Fmt("%.0f ", g_utilReport.demand[k]) + kUnit[k] + (g_utilReport.unserved[k] ? Fmt(", %.0f lots short", g_utilReport.unserved[k]) : "")});
        d.rows.push_back({"Upkeep", MoneyStr(D.upkeep) + " a month"});
        break;
    }
    case PickKind::Transport: {
        const Placed& b = g_transport.items[(size_t)h.index];
        const BuildingDef& D = Defs()[b.def];
        d.title = D.name, d.icon = "bus", d.accent = Rgba(47, 143, 224), d.sub = TTabName(D.tab);
        d.rows.push_back({"Status", b.state == BState::Planned ? std::string("planned") : b.state == BState::Building ? "under construction " + Pct(b.progress) : std::string("operating")});
        if (D.workplaces) d.rows.push_back({"Workplaces", std::to_string(D.workplaces)});
        if (D.vehicles) d.rows.push_back({"Vehicles", std::to_string(D.vehicles)});
        if (D.noise && *D.noise) d.rows.push_back({"Noise", D.noise});
        d.rows.push_back({"Upkeep", MoneyStr(D.upkeep) + " a month"});
        break;
    }
    case PickKind::IndSite: {
        const IndSite& s = g_ind.sites[(size_t)h.index];
        const IndType& t = IndTypeOf(s.kind);
        d.title = t.name, d.icon = "factory", d.accent = Rgba(194, 124, 58), d.sub = s.IsLandfill() ? "Landfill" : std::string("Produces ") + t.product;
        d.rows.push_back({"Area", Fmt("%.2f ha", s.hectares)});
        if (s.IsLandfill()) {
            d.rows.push_back({"Takes", Fmt("%.0f t a month", s.perMonth)});
            d.rows.push_back({"Filled", Pct(s.Fill()) + Fmt(" of %.0f t", s.Capacity())});
        } else {
            d.rows.push_back({"Efficiency", Pct(s.eff)});
            d.rows.push_back({"Output", Fmt("%.1f t a month", s.perMonth)});
            d.rows.push_back({"Stock", Fmt("%.1f t waiting", s.stock)});
            d.rows.push_back({"Needs", t.resName});
            GoodsRows(d, BOrigin::Industry, s.id);
        }
        AreaRows(d, s.c.x, s.c.z);
        break;
    }
    case PickKind::Waste: {
        const WasteBuilding& b = g_wasteB[(size_t)h.index];
        const WasteType& t = kWasteTypes[b.type];
        d.title = t.name, d.icon = "trash", d.accent = Rgba(242, 177, 28), d.sub = t.what;
        d.rows.push_back({"Capacity", Fmt("%.0f t a month", t.tPerMonth)});
        if (t.mw > 0) d.rows.push_back({"Electricity", Fmt("%.0f MW", t.mw)});
        break;
    }
    case PickKind::Road: {
        const Network& net = g_roads.net;
        const Profile& p = net.Prof(h.index);
        const bool rail = net.IsRailSeg(h.index);
        d.icon = rail ? "rail" : "road_m", d.accent = Rgba(125, 133, 151);
        d.title = p.name, d.sub = rail ? "Railway" : "Road";
        g_roadNames.Update(net);
        if (!rail)
            if (const RoadChain* ch = g_roadNames.ChainOf(h.index)) d.title = ch->name.Full(), d.sub = std::string(RoadNames::ClassLabel(ch->cls)) + " - " + p.name;
        if (!rail) d.rows.push_back({"Lanes", p.OneWay() ? Fmt("%.0f, one way", p.fwd) : Fmt("%.0f + %.0f", p.fwd, p.back)});
        d.rows.push_back({"Length", Fmt("%.0f m", net.Curve(h.index).Length())});
        if (!rail) {
            const float free = FlowGraph::Speed(p.cat);
            d.rows.push_back({"Speed limit", Fmt("%.0f km/h", free)});
            if ((size_t)h.index < g_segFlow.size() && g_segFlowVersion == net.version) {
                const SegFlow& f = g_segFlow[(size_t)h.index];
                static const char* const kCls[] = {"free", "busy", "congested", "jammed"};
                d.rows.push_back({"Traffic", Fmt("%.0f vehicles an hour, ", f.flow) + kCls[std::min<int>(f.cls, 3)]});
                d.rows.push_back({"Driving at", Fmt("%.0f km/h (peak)", free * f.speedRatio)});
            } else d.rows.push_back({"Traffic", "not solved yet"});
            d.rows.push_back({"Carries", CarriesUtilities(net, h.index) ? "power, water, sewage" : "no utilities"});
        }
        break;
    }
    case PickKind::UtilLine: {
        const UtilLine& l = g_util.lines[(size_t)h.index];
        d.icon = l.kind == Util::Electricity ? "pylon" : "pipe", d.accent = Rgba(242, 177, 28);
        d.title = l.kind == Util::Electricity ? "Electricity Line" : l.kind == Util::Water ? "Water Pipe" : "Sewage Pipe";
        d.sub = l.Buried() ? "buried" : l.Pylons() ? "steel pylons" : "wooden poles";
        d.rows.push_back({"Length", Fmt("%.0f m", l.Length())});
        if ((size_t)h.index < g_utilReport.lineLive.size()) d.rows.push_back({"Network", g_utilReport.lineLive[(size_t)h.index] ? "supplied" : "no source on it"});
        break;
    }
    case PickKind::ZoneCell: {
        const ZoneCell& c = g_zones.cells[(size_t)h.index];
        d.icon = "zone", d.accent = Rgba(79, 174, 91);
        if (c.zone == kNoZone) d.title = "Not zoned", d.sub = "Zone cell beside a road";
        else {
            const ZoneType& zt = ZoneTypes()[c.zone];
            d.title = zt.name, d.sub = std::string(ZoneCatName(zt.cat)) + " zone, empty";
            const int cat = (int)zt.cat;
            if (cat >= 0 && cat < tbd::city::SimGrowth::kCats) d.rows.push_back({"City demand", Fmt("%+.2f", g_simGrowth.demand[cat]) + (g_simGrowth.demand[cat] > 0 ? " (grows)" : " (no growth now)")});
        }
        d.rows.push_back({"Ground", Fmt("%.1f m", c.yc)});
        AreaRows(d, c.c.x, c.c.z);
        break;
    }
    case PickKind::Terrain: {
        d.icon = "terr", d.accent = Rgba(160, 122, 79), d.title = "Terrain";
        const bool sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1;
        d.sub = sea && g_info.at.y < tbd::IslandMap::kSeaLevel ? "Under the sea" : "Open ground";
        d.rows.push_back({"Height", Fmt("%.1f m", g_info.at.y) + (sea ? Fmt(" (%.1f m over the sea)", g_info.at.y - tbd::IslandMap::kSeaLevel) : "")});
        d.rows.push_back({"Position", Fmt("%.0f m E, %.0f m N", g_info.at.x, -g_info.at.z)});
        break;
    }
    case PickKind::Person: {
        if (h.sub == 4) { WalkerInfo(d, walkers_host::sim.walkers[(size_t)h.index]); break; } // [walkers] city_walkers.inl
        const Citizen& c = g_transit.citizens[(size_t)h.index];
        d.icon = "ppl", d.accent = Rgba(94, 234, 212), d.title = "Pedestrian";
        d.sub = std::string("Arrived by ") + TModeName(c.from);
        d.rows.push_back({"Doing", c.stopId ? (c.phase == 2 ? "waiting at a stop, seated" : "waiting at a stop") : c.walking ? "walking" : "at the door"});
        if (c.len > 0) d.rows.push_back({"Walk", Fmt("%.0f of %.0f m", c.s, c.len)});
        break;
    }
    case PickKind::Vehicle: {
        d.icon = "bus", d.accent = Rgba(47, 143, 224);
        if (h.sub == 0) {
            const Child& c = herds_host::children.pool[(size_t)h.index];
            static const char* const kTrip[] = {"commute to work", "going home from work", "school run", "home from school", "shopping",
                                                "home from the shops", "school run, then work", "through traffic", "freight"};
            const Herd* herd = nullptr;
            for (const Herd& hd : herds_host::herds.herds)
                if (hd.alive && hd.id == c.herdId) { herd = &hd; break; }
            d.title = herd && herd->type == kTripFreight ? "Truck" : "Car";
            d.sub = herd ? kTrip[std::min<int>(herd->type, kTripTypes - 1)] : "on its way";
            if (herd) d.rows.push_back({"From", HoodName(herd->from)});
            d.rows.push_back({"To", HoodName(c.destHood >= 0 ? c.destHood : herd ? herd->to : -1)});
            d.rows.push_back({"Speed", c.parked ? std::string("parked") : Fmt("%.0f km/h", c.v * 3.6)});
        } else if (h.sub == 2) {
            const Vehicle& v = g_transit.vehicles[(size_t)h.index];
            d.title = std::string(TModeName(v.mode)) + (v.kind == TKind::Cargo ? " (cargo)" : "");
            d.sub = v.route >= 0 && (size_t)v.route < g_transit.routes.size() ? "Line: " + g_transit.routes[(size_t)v.route].name : "Arriving from outside the city";
            d.rows.push_back({"Speed", v.dwell > 0 ? std::string("at a stop") : Fmt("%.0f km/h", v.speed * 3.6)});
        } else {
            d.title = "Car", d.sub = "Traffic from the flow solve";
            float t, dd;
            const int s = g_roads.net.SegmentAt({g_info.at.x, g_info.at.z}, 3.0f, t, dd);
            if (s >= 0) d.rows.push_back({"On", g_roads.net.Prof(s).name});
            d.rows.push_back({"Speed", Fmt("%.0f km/h (its road's flow)", g_info.speed)});
        }
        break;
    }
    default: break;
    }
}

// Where the picked thing is now, for its outline: centre, front, size, ground, height. False: no outline (lots have theirs).
bool InfoShape(tbd::city::V2& c, tbd::city::V2& f, float& W, float& D, float& y, float& H) {
    using namespace tbd::city;
    const PickHit& h = g_info.hit;
    f = {0, 1};
    switch (h.kind) {
    case PickKind::Util: {
        const UtilBuilding& b = g_util.items[(size_t)h.index];
        const UtilDef& d = UtilDefs()[b.def];
        c = b.c, f = b.front, W = d.W(), D = d.D(), y = b.y, H = std::min(d.height, 40.0f);
        return true;
    }
    case PickKind::Transport: {
        const Placed& b = g_transport.items[(size_t)h.index];
        const BuildingDef& d = Defs()[b.def];
        c = b.c, f = b.front, W = d.W(), D = d.D(), y = b.y, H = std::min(d.height, 40.0f);
        return true;
    }
    case PickKind::IndSite: c = g_ind.sites[(size_t)h.index].c, f = g_ind.sites[(size_t)h.index].front, W = kHubW, D = kHubD, y = g_ind.sites[(size_t)h.index].y, H = 14.0f; return true;
    case PickKind::Waste: {
        const WasteBuilding& b = g_wasteB[(size_t)h.index];
        c = b.c, f = b.front, W = kWasteTypes[b.type].w, D = kWasteTypes[b.type].d, y = b.y, H = 18.0f;
        return true;
    }
    case PickKind::ZoneCell: {
        const ZoneCell& z = g_zones.cells[(size_t)h.index];
        c = z.c, f = z.out, W = D = kCell, y = z.yc, H = 0.0f;
        return true;
    }
    case PickKind::Vehicle: case PickKind::Person: {
        V2 p{g_info.at.x, g_info.at.z};
        float r = 3.0f;
        if (h.sub == 0) {
            CarInstance ci;
            if (herds_host::children.Place(herds_host::children.pool[(size_t)h.index], ci)) p = {ci.p.x, ci.p.z}, f = ci.dir;
        } else if (h.sub == 2) {
            const Vehicle& v = g_transit.vehicles[(size_t)h.index];
            p = v.pos, f = v.dir, r = v.mode == TMode::Train ? 26.0f : v.mode == TMode::Bus ? 6.0f : 10.0f;
        } else if (h.sub == 3) p = g_transit.citizens[(size_t)h.index].pos, r = 1.0f;
        else if (h.sub == 4) p = walkers_host::sim.walkers[(size_t)h.index].pos, f = walkers_host::sim.walkers[(size_t)h.index].dir, r = 1.0f; // [walkers]
        c = p, W = r * (h.kind == PickKind::Person ? 2.0f : 1.0f), D = h.kind == PickKind::Person ? r * 2.0f : r * 2.0f, y = RoadsGround(p.x, p.z), H = h.kind == PickKind::Person ? 2.0f : 1.8f;
        return true;
    }
    case PickKind::Terrain: c = {g_info.at.x, g_info.at.z}, W = D = 3.0f, y = g_info.at.y, H = 0.0f; return true;
    default: return false;
    }
}

// ---------------------------------------------------------------- input (game_core.inl ViewportInput)
// One finger is the left button on touch (game_host.cpp TouchBuilds) only for the tools that draw with it; a building being
// placed, bulldoze's tap mode, an industry area being drawn (corners by tap) and no tool at all pan with it instead.
bool TouchFingerDraws() {
    if (g_dial.d.on) return true; // the dial: a drag turns the building
    if (g_doze.active) return g_doze.area; // Zone area: a drag boxes cells; Tap: a drag pans
    if (BuildingGhostKind() != 0) return false;
    if (g_indTool.active && g_indTool.mode == IndustryTool::Draw) return false;
    return gdock::ToolActive();
}

// The left button went down at (x, y). 0: not ours; 1: used; 2: used, and the drag needs the pointer (capture).
int InteractLeftDown(int x, int y) {
    if (g_play.active || g_engine.drag != Drag::None || tbd::platform::KeyDownSync(tbd::platform::Key::Alt)) return 0;
    const tbd::Ray ray = InteractRay(x, y);
    g_terrainEdit.mouseX = x, g_terrainEdit.mouseY = y;
    if (g_dial.d.on) {
        tbd::city::V2 p;
        if (DialGround(ray, p)) {
            if (float* rot = GhostRotation()) g_dial.d.deg = *rot;
            g_dial.d.Press(p);
        }
        return 2;
    }
    if (g_doze.active) {
        if (g_doze.area) {
            tbd::Vec3 hit;
            if (!GroundHit(ray, hit)) return 1;
            g_doze.plane.Begin(hit.y); // the box stays on this world plane for the whole drag
            g_doze.a = g_doze.b = {hit.x, hit.z};
            const int c = g_zones.CellAt(g_doze.a);
            g_doze.dir = c >= 0 ? g_zones.cells[(size_t)c].dir : tbd::city::V2{1, 0};
            g_doze.dragging = true, g_doze.areaPending = false;
            DozeBoxCells();
            return 2;
        }
        tbd::Vec3 hit;
        const bool ground = GroundHit(ray, hit);
        const tbd::city::PickWorld w = PickWorldNow(ray, true, nullptr);
        if (!ground && w.lotUnderRay < 0) return 1;
        const PickHit h = tbd::city::Pick(w, {hit.x, hit.z}, g_touchInput ? TouchPad() * 0.5f : 0.5f);
        if (h.kind == PickKind::None) { g_doze.pending = {}; return 1; }
        if (g_touchInput && h != g_doze.pending) { // the first tap marks it; a second tap on it (or the check) takes it
            g_doze.pending = h;
            return 1;
        }
        DozeDo(h);
        g_doze.pending = {};
        return 1;
    }
    if (g_touchInput && BuildingGhostKind() != 0) {
        tbd::Vec3 hit;
        if (!GroundHit(ray, hit)) return 1;
        tbd::city::V2 c, f;
        float W, D, gy;
        if (GhostNow(c, f, W, D, gy) && tbd::city::BlockRect{c, f, W * 0.5f, D * 0.5f}.Contains({hit.x, hit.z}, TouchPad())) {
            PlaceGhostNow(); // a tap on the ghost builds it there
            return 1;
        }
        g_placePin.on = true, g_placePin.at = hit, g_placePin.key = PlaceGhostKey(); // the ghost goes where the tap landed
        return 1;
    }
    if (!gdock::ToolActive() && g_terrainSandbox) { // the city game, no tool out: what is there
        InfoPick(ray);
        return g_touchInput ? 1 : 0; // the editor (mouse) still selects its entities under it
    }
    return 0;
}
// The pointer moved (any button). True when the dial / the bulldoze box used it.
bool InteractMove(int x, int y) {
    if (g_dial.d.on && g_dial.d.dragging) {
        tbd::city::V2 p;
        if (DialGround(InteractRay(x, y), p) && g_dial.d.Drag(p, DialStepNow()))
            if (float* rot = GhostRotation()) *rot = g_dial.d.deg;
        return true;
    }
    if (g_doze.active && g_doze.dragging) {
        tbd::Vec3 hit;
        if (g_doze.plane.Hit(InteractRay(x, y), hit)) g_doze.b = {hit.x, hit.z}, DozeBoxCells();
        return true;
    }
    if (g_doze.active && !g_doze.area && !g_touchInput && x >= 0) { // the mouse's hover: what a click would take, in red
        const tbd::Ray ray = InteractRay(x, y);
        tbd::Vec3 hit;
        const bool ground = GroundHit(ray, hit);
        const tbd::city::PickWorld w = PickWorldNow(ray, true, nullptr);
        g_doze.hover = ground || w.lotUnderRay >= 0 ? tbd::city::Pick(w, {hit.x, hit.z}, 0.5f) : PickHit{};
        if (g_gui.WantsMouseAt((float)x, (float)y)) g_doze.hover = {};
    }
    return false;
}
// The left button came up. True when ours.
bool InteractLeftUp(int x, int y) {
    if (g_dial.d.on && g_dial.d.dragging) { g_dial.d.Release(); return true; }
    if (g_doze.active && g_doze.dragging) {
        InteractMove(x, y);
        g_doze.dragging = false;
        g_doze.plane.End();
        if (g_touchInput) g_doze.areaPending = !g_doze.cells.empty(); // touch: the check clears them
        else DozeArea(g_doze.cells), g_doze.cells.clear();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the placement bar (glass_dock_host.inl)
void InteractCloseTools() {
    DialEnd(false);
    DozeClose();
    g_placePin.on = false;
}
void InteractBulldozeButton() {
    const bool was = g_doze.active;
    gdock::CloseTools();
    if (!was) g_doze.active = true, Toast(g_touchInput ? "BULLDOZE: TAP SOMETHING, TAP AGAIN TO DEMOLISH" : "BULLDOZE: CLICK WHAT SHOULD GO");
}
// The bar's tool name, mode and options for bulldoze and the building tools' Rotate / Snap.
void InteractFillTool(std::string& tool, std::string& icon, std::string& modeName, std::string& modeIcon, std::string& cost,
                      std::vector<std::pair<std::string, std::string>>& opts, std::vector<bool>& on) {
    on.assign(opts.size(), false);
    if (g_doze.active) {
        tool = "Bulldoze", icon = "bulldoze", cost.clear();
        modeName = g_doze.area ? "Zone area" : "Tap", modeIcon = g_doze.area ? "box" : "bulldoze";
        opts.clear(), on.clear();
        return;
    }
    if (BuildingGhostKind() == 0) return;
    opts.erase(std::remove_if(opts.begin(), opts.end(), [](const std::pair<std::string, std::string>& o) { return o.first == "Rotate" || o.first == "Rotate back"; }), opts.end());
    opts.insert(opts.begin(), std::make_pair(std::string("Rotate"), std::string("rotate")));
    if (g_dial.d.on) opts.insert(opts.begin() + 1, std::make_pair(std::string("Snap"), std::string("snap")));
    on.assign(opts.size(), false);
    on[0] = g_dial.d.on;
    if (g_dial.d.on) on[1] = g_dialSnap;
}
// A placement bar button. True when it was ours (the tool's own handling is skipped).
bool InteractToolButton(const tbd::gui::dock::Out& o, const std::string& optName) {
    if (g_doze.active) {
        if (o.cancel) {
            if (g_doze.pending.kind != PickKind::None || g_doze.areaPending) g_doze.pending = {}, g_doze.areaPending = false, g_doze.cells.clear();
            else DozeClose();
        } else if (o.confirm) {
            if (g_doze.pending.kind != PickKind::None) DozeDo(g_doze.pending), g_doze.pending = {};
            else if (g_doze.areaPending) DozeArea(g_doze.cells), g_doze.cells.clear(), g_doze.areaPending = false;
            else DozeClose();
        } else if (o.undo) DozeUndoLast();
        else if (o.mode) {
            g_doze.area = !g_doze.area, g_doze.pending = {}, g_doze.areaPending = false, g_doze.cells.clear();
            Toast(g_doze.area ? "ZONE AREA: DRAG A BOX OVER ZONED CELLS" : "TAP: BUILDINGS, ROADS, LINES, ZONE CELLS");
        }
        return o.cancel || o.confirm || o.undo || o.mode;
    }
    if (g_dial.d.on) {
        if (o.confirm) { PlaceGhostNow(); DialEnd(true); } // [snap] the check builds it as dialled, then the road snap is back
        else if (o.opt >= 0 && optName == "Rotate") DialEnd(true);
        else if (o.cancel) DialEnd(false);
        else if (o.opt >= 0 && optName == "Snap") {
            g_dialSnap = !g_dialSnap;
            DialApplySnap();
            Toast(g_dialSnap ? "SNAP ON: 15 DEGREE STEPS" : "SNAP OFF: FREE ANGLE"); // [snap] the dial is free of the road
        }
        return o.confirm || o.cancel || o.undo || o.opt >= 0;
    }
    if (BuildingGhostKind() != 0) {
        if (o.opt >= 0 && optName == "Rotate") { DialBegin(); return true; }
        if (g_touchInput && o.confirm) { // touch: the check builds the ghost where it stands
            tbd::city::V2 c, f;
            float W, D, y;
            if (GhostNow(c, f, W, D, y)) PlaceGhostNow();
            else Toast("TAP THE MAP TO PUT IT THERE");
            return true;
        }
        if (o.cancel && g_placePin.on) { g_placePin.on = false; return true; } // the first cross drops the pin, the next closes
    }
    return false;
}

// ---------------------------------------------------------------- per frame
// TBD_INTERACT=<words> for captures (dev): touch (taps place / pick), doze (Bulldoze on), area (its Zone area mode), dial (the
// Rotate toggle on the ghost), turn (the dial turned 45 degrees), info (a tap in the middle of the view), 60 frames in.
void InteractDemo() {
    static int frames = 0;
    static char words[96] = {};
    static const bool on = tbd::platform::GetEnvA("TBD_INTERACT", words, sizeof(words)) > 0;
    if (!on || ++frames != 60) return;
    auto has = [&](const char* w) { return std::strstr(words, w) != nullptr; };
    if (has("touch")) g_touchInput = true;
    if (has("doze")) InteractBulldozeButton();
    if (has("area")) g_doze.area = true;
    if (has("dial")) DialBegin();
    if (has("turn") && g_dial.d.on)
        if (float* rot = GhostRotation()) *rot = g_dial.d.deg = tbd::city::Norm360(*rot + 45.0f);
    if (has("info")) InfoPick(InteractRay(g_engine.vpW / 2, g_engine.vpH / 2));
    tbd::Log("Interact demo: %s (dial %d, bulldoze %d, info %d %s)", words, (int)g_dial.d.on, (int)g_doze.active, (int)g_info.open, tbd::city::PickKindName(g_info.hit.kind));
}

void InteractSync() {
    InteractDemo();
    if (g_placePin.on && g_placePin.key != PlaceGhostKey()) g_placePin.on = false; // another item, or the tool closed
    if (g_dial.d.on && (BuildingGhostKind() == 0 || g_placePin.key != PlaceGhostKey())) g_dial.d.on = false, g_dial.d.dragging = false;
    if (g_dial.d.on) {
        tbd::city::V2 c, f;
        float W, D, y;
        if (GhostNow(c, f, W, D, y)) {
            g_dial.d.centre = c, g_dial.y = y;
            if (g_dial.origPending) g_dial.origPending = false, g_dial.origFront = f;
        }
        if (float* rot = GhostRotation()) g_dial.d.deg = *rot;
    }
    if (g_doze.active && gdock::ToolActive()) DozeClose(); // another tool took over
    if (g_doze.pending.kind != PickKind::None) { // still there?
        const PickHit& p = g_doze.pending;
        const bool ok = (p.kind == PickKind::Lot && p.index < (int)g_zones.lots.size() && g_zones.lots[(size_t)p.index].alive) ||
                        (p.kind == PickKind::Util && p.index < (int)g_util.items.size() && g_util.items[(size_t)p.index].alive) ||
                        (p.kind == PickKind::Transport && p.index < (int)g_transport.items.size() && g_transport.items[(size_t)p.index].alive) ||
                        (p.kind == PickKind::Road && p.index < (int)g_roads.net.segs.size() && g_roads.net.segs[(size_t)p.index].alive) ||
                        (p.kind == PickKind::UtilLine && p.index < (int)g_util.lines.size() && g_util.lines[(size_t)p.index].alive) ||
                        (p.kind == PickKind::IndSite && p.index < (int)g_ind.sites.size()) || (p.kind == PickKind::Waste && p.index < (int)g_wasteB.size()) ||
                        (p.kind == PickKind::ZoneCell && p.index < (int)g_zones.cells.size() && g_zones.cells[(size_t)p.index].zone != tbd::city::kNoZone);
        if (!ok) g_doze.pending = {};
    }
    if (g_info.open && (gdock::ToolActive() || g_doze.active || !InfoResolve())) g_info.open = false;
}

// ---------------------------------------------------------------- drawing: one line draw, one triangle draw
namespace ifx {
using tbd::Vec3;
using tbd::Vertex;
inline void L(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c) {
    o.push_back({a.x, a.y, a.z, 0, 0, 0, c.x, c.y, c.z, 1});
    o.push_back({b.x, b.y, b.z, 0, 0, 0, c.x, c.y, c.z, 1});
}
inline void T(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 col) {
    for (const Vec3& p : {a, b, c, a, c, b}) o.push_back({p.x, p.y, p.z, 0, 0, 0, col.x, col.y, col.z, 1}); // both windings
}
// An upright box over a footprint: its edges (lines), and the footprint filled (tris, when given).
inline void Box(std::vector<Vertex>& lines, std::vector<Vertex>* tris, tbd::city::V2 c, tbd::city::V2 f, float W, float D, float y, float H, Vec3 col) {
    tbd::city::V2 q[4];
    tbd::city::Corners(c, f, W, D, q);
    for (int k = 0; k < 4; ++k) {
        const tbd::city::V2 a = q[k], b = q[(k + 1) % 4];
        for (float lift : {0.35f, 0.7f}) L(lines, {a.x, y + lift, a.z}, {b.x, y + lift, b.z}, col);
        if (H > 0.5f) {
            L(lines, {a.x, y + H, a.z}, {b.x, y + H, b.z}, col);
            L(lines, {a.x, y + 0.35f, a.z}, {a.x, y + H, a.z}, col);
        }
    }
    if (tris) {
        const float h = y + 0.25f;
        T(*tris, {q[0].x, h, q[0].z}, {q[1].x, h, q[1].z}, {q[2].x, h, q[2].z}, col * 0.75f);
        T(*tris, {q[0].x, h, q[0].z}, {q[2].x, h, q[2].z}, {q[3].x, h, q[3].z}, col * 0.75f);
    }
}
inline void Circle(std::vector<Vertex>& o, tbd::city::V2 c, float r, float y, Vec3 col, int n = 64) {
    for (int i = 0; i < n; ++i) {
        const float a0 = i * 6.2831853f / n, a1 = (i + 1) * 6.2831853f / n;
        L(o, {c.x + std::sin(a0) * r, y, c.z + std::cos(a0) * r}, {c.x + std::sin(a1) * r, y, c.z + std::cos(a1) * r}, col);
    }
}
} // namespace ifx

// The zoned lot's model box (city_buildings.inl's picked outline, in a colour).
bool LotBox(uint64_t id, std::vector<tbd::Vertex>& lines, tbd::Vec3 col) {
    using namespace city_bld;
    const auto it = recs.find(id);
    if (it == recs.end() || it->second.model < 0 || !models[(size_t)it->second.model].ok) return false;
    const Model& m = models[(size_t)it->second.model];
    const float* w = it->second.row;
    const auto at = [&](float x, float y, float z) { return tbd::Vec3{w[0] * x + w[1] * y + w[2] * z + w[3], w[4] * x + w[5] * y + w[6] * z + w[7], w[8] * x + w[9] * y + w[10] * z + w[11]}; };
    tbd::Vec3 c[8];
    for (int k = 0; k < 8; ++k) c[k] = at((k & 1) ? m.half.x + 0.4f : -m.half.x - 0.4f, (k & 2) ? m.maxY + 0.4f : m.minY + 0.05f, (k & 4) ? m.half.z + 0.4f : -m.half.z - 0.4f);
    static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& p : e) ifx::L(lines, c[p[0]], c[p[1]], col);
    return true;
}

// The red mark of what bulldoze would take.
void DozeMark(const PickHit& h, std::vector<tbd::Vertex>& lines, std::vector<tbd::Vertex>& tris) {
    using namespace tbd::city;
    const tbd::Vec3 red{1.0f, 0.18f, 0.12f};
    switch (h.kind) {
    case PickKind::Lot: {
        const Lot& l = g_zones.lots[(size_t)h.index];
        LotBox(l.id, lines, red);
        ifx::Box(lines, &tris, l.c, l.front, l.w * kCell, l.d * kCell, l.y, 0.0f, red);
        break;
    }
    case PickKind::Road: {
        const Network& net = g_roads.net;
        const Cubic cu = net.Curve(h.index);
        const float hw = net.Prof(h.index).Half() + 0.3f;
        V2 prev = cu.At(0);
        for (int i = 1; i <= 16; ++i) {
            const V2 q = cu.At(i / 16.0f), n = RightOf(Norm(q - prev)) * hw;
            const float y0 = net.YAt(net.segs[(size_t)h.index], (i - 1) / 16.0f) + 0.45f, y1 = net.YAt(net.segs[(size_t)h.index], i / 16.0f) + 0.45f;
            ifx::T(tris, {prev.x - n.x, y0, prev.z - n.z}, {prev.x + n.x, y0, prev.z + n.z}, {q.x + n.x, y1, q.z + n.z}, red * 0.8f);
            ifx::T(tris, {prev.x - n.x, y0, prev.z - n.z}, {q.x + n.x, y1, q.z + n.z}, {q.x - n.x, y1, q.z - n.z}, red * 0.8f);
            prev = q;
        }
        break;
    }
    case PickKind::UtilLine: {
        const UtilLine& l = g_util.lines[(size_t)h.index];
        const float ya = RoadsGround(l.a.x, l.a.z) + std::max(l.elev, 0.0f) + (l.Poles() ? kPoleHeight : l.Pylons() ? PylonHeight(l.elev) : 0.5f);
        const float yb = RoadsGround(l.b.x, l.b.z) + std::max(l.elev, 0.0f) + (l.Poles() ? kPoleHeight : l.Pylons() ? PylonHeight(l.elev) : 0.5f);
        for (float o : {-0.6f, 0.0f, 0.6f}) ifx::L(lines, {l.a.x, ya + o, l.a.z}, {l.b.x, yb + o, l.b.z}, red);
        break;
    }
    case PickKind::ZoneCell: {
        const ZoneCell& z = g_zones.cells[(size_t)h.index];
        ifx::Box(lines, &tris, z.c, z.out, kCell * 0.9f, kCell * 0.9f, z.yc, 0.0f, red);
        break;
    }
    case PickKind::IndSite: {
        const IndSite& s = g_ind.sites[(size_t)h.index];
        ifx::Box(lines, &tris, s.c, s.front, kHubW, kHubD, s.y, 14.0f, red);
        const std::vector<V2> poly = s.Poly();
        for (size_t i = 0; i < poly.size(); ++i) {
            const V2 a = poly[i], b = poly[(i + 1) % poly.size()];
            ifx::L(lines, {a.x, RoadsGround(a.x, a.z) + 0.6f, a.z}, {b.x, RoadsGround(b.x, b.z) + 0.6f, b.z}, red);
        }
        break;
    }
    default: {
        const PickHit keep = g_info.hit;
        const tbd::Vec3 keepAt = g_info.at;
        g_info.hit = h; // the footprint shapes are the info outline's
        V2 c, f;
        float W, D, y, H;
        if (InfoShape(c, f, W, D, y, H)) ifx::Box(lines, &tris, c, f, W, D, y, H, red);
        g_info.hit = keep, g_info.at = keepAt;
        break;
    }
    }
}

void InteractDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace tbd::city;
    static std::vector<tbd::Vertex> lines, tris;
    lines.clear(), tris.clear();
    if (g_doze.active) {
        const PickHit& h = g_touchInput ? g_doze.pending : g_doze.hover;
        if (h.kind != PickKind::None) DozeMark(h, lines, tris);
        if (g_doze.area && (g_doze.dragging || g_doze.areaPending)) {
            const V2 a = g_doze.a, d = g_doze.dir, n = LeftOf(d), e = g_doze.b - a;
            const float u = Dot2(e, d), v = Dot2(e, n), y = g_doze.dragging ? g_doze.plane.y + 0.5f : RoadsGround(a.x, a.z) + 0.5f;
            const V2 q[4] = {a, a + d * u, a + d * u + n * v, a + n * v};
            for (int k = 0; k < 4; ++k) ifx::L(lines, {q[k].x, y, q[k].z}, {q[(k + 1) % 4].x, y, q[(k + 1) % 4].z}, {1, 1, 1});
            for (int c : g_doze.cells) {
                const ZoneCell& z = g_zones.cells[(size_t)c];
                ifx::Box(lines, &tris, z.c, z.out, kCell * 0.88f, kCell * 0.88f, z.yc, 0.0f, {1.0f, 0.2f, 0.14f});
            }
        }
    }
    if (g_info.open && g_info.hit.kind != PickKind::Lot) { // the lot has the yellow box (CityPickedOutline)
        V2 c, f;
        float W, D, y, H;
        if (g_info.hit.kind == PickKind::Road) {
            const Network& net = g_roads.net;
            const Cubic cu = net.Curve(g_info.hit.index);
            const float hw = net.Prof(g_info.hit.index).Half() + 0.4f;
            V2 prev = cu.At(0);
            for (int i = 1; i <= 16; ++i) {
                const V2 q = cu.At(i / 16.0f), n = RightOf(Norm(q - prev)) * hw;
                const float y0 = net.YAt(net.segs[(size_t)g_info.hit.index], (i - 1) / 16.0f) + 0.5f, y1 = net.YAt(net.segs[(size_t)g_info.hit.index], i / 16.0f) + 0.5f;
                for (float sgn : {-1.0f, 1.0f}) ifx::L(lines, {prev.x + n.x * sgn, y0, prev.z + n.z * sgn}, {q.x + n.x * sgn, y1, q.z + n.z * sgn}, {1.0f, 0.85f, 0.2f});
                prev = q;
            }
        } else if (InfoShape(c, f, W, D, y, H)) {
            if (g_info.hit.kind == PickKind::Vehicle || g_info.hit.kind == PickKind::Person) ifx::Circle(lines, c, std::max(W, D) * 0.75f + 1.0f, y + 0.4f, {1.0f, 0.85f, 0.2f}, 32);
            else ifx::Box(lines, nullptr, c, f, W, D, y, H, {1.0f, 0.85f, 0.2f});
        }
    }
    if (g_dial.d.on) { // the ring, the original facing (green) and the new one (blue), the snap steps
        V2 c, f;
        float W, D, y;
        if (GhostNow(c, f, W, D, y)) {
            const float R = 0.5f * std::sqrt(W * W + D * D) + 6.0f, ry = y + 0.8f;
            ifx::Circle(lines, c, R, ry, {0.92f, 0.95f, 1.0f});
            ifx::Circle(lines, c, R + 0.6f, ry, {0.92f, 0.95f, 1.0f});
            const float step = DialStepNow();
            if (step >= 15.0f)
                for (float a = 0; a < 359.9f; a += step) {
                    const V2 u = FrontFromDeg(a + DegOfFront(g_dial.origFront));
                    ifx::L(lines, {c.x + u.x * (R - 1.5f), ry, c.z + u.z * (R - 1.5f)}, {c.x + u.x * R, ry, c.z + u.z * R}, {0.7f, 0.75f, 0.85f});
                }
            auto ray = [&](V2 dir, tbd::Vec3 col, float len) {
                const V2 e = c + dir * len, s = LeftOf(dir);
                for (float o : {-0.25f, 0.0f, 0.25f}) ifx::L(lines, {c.x + s.x * o, ry + 0.05f, c.z + s.z * o}, {e.x + s.x * o, ry + 0.05f, e.z + s.z * o}, col);
                const V2 h = e - dir * 3.0f;
                ifx::L(lines, {e.x, ry, e.z}, {h.x + s.x * 1.8f, ry, h.z + s.z * 1.8f}, col);
                ifx::L(lines, {e.x, ry, e.z}, {h.x - s.x * 1.8f, ry, h.z - s.z * 1.8f}, col);
            };
            ray(g_dial.origFront, {0.2f, 0.95f, 0.35f}, R);
            ray(f, {0.3f, 0.7f, 1.0f}, R + 3.0f);
        }
    }
    if (!tris.empty()) r.DrawTransient(tris.data(), (uint32_t)tris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!lines.empty()) r.DrawTransient(lines.data(), (uint32_t)lines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
}

// The GUI side (gdock::Frame): the info card, the dial's degrees, bulldoze's prompt.
void InteractGuiFrame(const tbd::gui::dock::Atlas& atlas, bool menuOpen, bool phone, float textScale) {
    auto project = [](tbd::Vec3 p, float& x, float& y) {
        const tbd::Vec4 c = g_lastViewProj * tbd::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * (float)g_engine.vpW;
        y = (0.5f - c.y / c.w * 0.5f) * (float)g_engine.vpH;
        return true;
    };
    float x, y;
    if (g_dial.d.on) {
        tbd::city::V2 c, f;
        float W, D, gy;
        if (GhostNow(c, f, W, D, gy)) {
            const float R = 0.5f * std::sqrt(W * W + D * D) + 9.0f;
            const tbd::city::V2 e = c + f * R;
            const float turn = tbd::city::WrapDeg180(tbd::city::DegOfFront(f) - tbd::city::DegOfFront(g_dial.origFront));
            char b[64];
            std::snprintf(b, sizeof(b), "%+.0f deg   snap %s", turn, g_dialSnap ? (DialStepNow() >= 90.0f ? "90" : "15") : (DialStepNow() >= 90.0f ? "90 (keeps to the road)" : "off"));
            if (project({e.x, gy + 1.0f, e.z}, x, y)) g_gui.Chip(x, y, b, tbd::gui::Rgba(30, 90, 150, 235));
        }
    }
    if (g_doze.active && g_touchInput) {
        if (g_doze.pending.kind != PickKind::None) g_gui.Chip((float)g_engine.vpW * 0.5f, 90.0f * g_gui.Scale(), RoadsUpper("Demolish " + DozeName(g_doze.pending) + "? Tap it again or the check").c_str(), tbd::gui::Rgba(170, 40, 34, 235));
        else if (g_doze.areaPending) g_gui.Chip((float)g_engine.vpW * 0.5f, 90.0f * g_gui.Scale(), RoadsUpper("Clear " + std::to_string(g_doze.cells.size()) + " zone cells? The check clears them").c_str(), tbd::gui::Rgba(170, 40, 34, 235));
    }
    if (g_info.open && !menuOpen) {
        static tbd::gui::InfoCardData card;
        InfoCardFill(card);
        if (tbd::gui::DrawInfoCard(g_gui, atlas, card, phone, textScale)) {
            g_info.open = false;
            if (g_pickedLot) ++g_pickedLotSerial, g_pickedLot = 0;
        }
    }
}
