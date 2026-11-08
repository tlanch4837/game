// Headless checks for interacting with the city (src/city/city_interact.h, src/game/touch_gesture.h):
//   tap vs drag     a finger is a tap only when it lifts soon without travelling 10 pt; a drag stays a drag; a hold is neither;
//   world plane     a zone drag locked to the plane of its first cell meets that plane exactly, and the box's cells are the
//                   ones whose centres lie in the world rectangle (not a screen rectangle) whatever the camera tilt;
//   bulldoze        each target goes through the existing removal path and moves the versions / removed lists the host and
//                   the sims watch; undo puts it back (and refuses once the world moved on);
//   rotate dial     bearings, wrap, snap steps, the drag follows the finger's change of bearing, cancel's origin;
//   pick            every info target is found at its spot, small things first, the lot under the ray first among buildings.
// Build and run: tests\interact_test.bat
#include <cmath>
#include <cstdio>
#include <string>

#include "city/city_interact.h"
#include "game/touch_gesture.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static float Flat(float, float) { return 0.0f; }
static int UDef(const char* name) {
    const auto& d = UtilDefs();
    for (size_t i = 0; i < d.size(); ++i)
        if (std::string(d[i].name) == name) return (int)i;
    return -1;
}
static int FlatTransportDef() {
    for (size_t i = 0; i < Defs().size(); ++i)
        if (Defs()[i].site == Site::Flat) return (int)i;
    return -1;
}
static int LiveLots(const ZoneGrid& z) {
    int n = 0;
    for (const Lot& l : z.lots) n += l.alive;
    return n;
}

// A road along x with low density houses grown on its south side for x < 0 (the north side and x > 0 stay free).
struct Town {
    RoadTool roads;
    ZoneGrid zones;
    UtilWorld util;
    TransportWorld transport;
    IndustryWorld ind;
    Town() {
        roads.BuildRoad(Cubic::Line({-300, 0}, {300, 0}), 0, 0, ProfileIndex("Two-Lane Road"));
        zones.models.push_back({0, 1, 1, 1, "test.glb"});
        zones.maxConstructing = 100000;
        zones.Rebuild(roads.net, Flat);
        for (size_t i = 0; i < zones.cells.size(); ++i)
            if (zones.cells[i].c.x < -40.0f && zones.cells[i].c.z < 0.0f) zones.Paint((int)i, 0);
        for (int i = 0; i < 400 && LiveLots(zones) < 20; ++i) zones.Tick(5.0f);
        for (int i = 0; i < 60; ++i) zones.Tick(10.0f);
    }
    PlaceContext Ctx() const {
        PlaceContext cx;
        cx.net = &roads.net, cx.zones = &zones, cx.ground = Flat;
        return cx;
    }
    DozeWorld Doze() { return DozeWorld{&roads, &zones, &util, &transport, &ind}; }
    PickWorld World() const {
        PickWorld w;
        w.net = &roads.net, w.zones = &zones, w.util = &util, w.transport = &transport, w.ind = &ind;
        return w;
    }
};

int main() {
    using tbd::touch::Classify;
    using tbd::touch::Gesture;
    // ---- tap vs drag
    {
        CHECK(Classify(0.0f, 0.10, true) == Gesture::Tap);
        CHECK(Classify(9.5f, 0.30, true) == Gesture::Tap);      // a little wobble is still a tap
        CHECK(Classify(10.5f, 0.10, true) == Gesture::Drag);    // past the slop: a pan, never a placement
        CHECK(Classify(40.0f, 0.05, false) == Gesture::Drag);   // decided while still down
        CHECK(Classify(3.0f, 0.20, false) == Gesture::Undecided);
        CHECK(Classify(3.0f, 0.80, false) == Gesture::Hold);    // a slow, unsure press
        CHECK(Classify(3.0f, 0.80, true) == Gesture::Hold);     // ... lifted late: not a tap either
        CHECK(Near(tbd::touch::TravelPt(30.0f, 40.0f, 2.0f), 25.0f)); // device pixels to points (2x iPad)
        CHECK(Classify(tbd::touch::TravelPt(18.0f, 0.0f, 2.0f), 0.1, true) == Gesture::Tap);  // 18 px on a 2x screen = 9 pt
        CHECK(Classify(tbd::touch::TravelPt(18.0f, 0.0f, 1.0f), 0.1, true) == Gesture::Drag); // 18 pt on a 1x screen
    }

    // ---- the world plane of a zone drag
    {
        PlaneLock lock;
        tbd::Vec3 hit;
        CHECK(!lock.Hit(tbd::Ray{{0, 100, 0}, {0, -1, 0}}, hit)); // unlocked: no hit
        lock.Begin(12.0f);
        // a steep ray and a grazing ray (a tilted camera, the finger near the horizon): both meet y = 12 exactly
        const tbd::Vec3 eye{0, 200, -400};
        for (tbd::Vec3 to : {tbd::Vec3{10, 0, 0}, tbd::Vec3{300, 0, 900}}) {
            tbd::Vec3 d = to - eye;
            d = d * (1.0f / std::sqrt(tbd::Dot(d, d)));
            CHECK(lock.Hit(tbd::Ray{eye, d}, hit) && Near(hit.y, 12.0f, 1e-3f));
            const float t = (12.0f - eye.y) / d.y; // on the ray
            CHECK(Near(hit.x, eye.x + d.x * t, 1e-2f) && Near(hit.z, eye.z + d.z * t, 1e-2f));
        }
        CHECK(!lock.Hit(tbd::Ray{eye, {0, 1, 0}}, hit)); // looking up: never "hits" behind the eye
        lock.End();
        CHECK(!lock.on);
        // the box's cells are exactly the ones whose centres are inside the world rectangle (along the road and across it)
        Town t;
        const V2 a{-120, -4}, b{-60, -40}, dir{1, 0};
        std::vector<int> cells;
        t.zones.BoxCells(a, b, dir, cells);
        CHECK(!cells.empty());
        int inside = 0;
        for (size_t i = 0; i < t.zones.cells.size(); ++i) {
            const V2 c = t.zones.cells[i].c;
            const bool in = c.x >= -121.0f && c.x <= -59.0f && c.z <= -3.0f && c.z >= -41.0f;
            inside += in;
            if (in) CHECK(std::find(cells.begin(), cells.end(), (int)i) != cells.end());
        }
        CHECK(inside == (int)cells.size());
    }

    // ---- bulldoze: targets, dirty state, undo
    {
        Town t;
        DozeWorld w = t.Doze();
        DozeDirty dirty;
        DozeUndo u;
        // a utility building
        const int tower = UDef("Water Tower");
        CHECK(tower >= 0);
        const int ui = t.util.Place(t.util.Plan(t.Ctx(), tower, {60, 30}, 0, true), tower);
        CHECK(ui >= 0);
        const V2 towerAt = t.util.items[(size_t)ui].c;
        PickWorld pw = t.World();
        pw.agentsOn = pw.terrainOn = pw.emptyCellsOn = false;
        PickHit h = Pick(pw, towerAt, 1.0f);
        CHECK(h.kind == PickKind::Util && h.index == ui);
        uint64_t v = t.util.version;
        CHECK(Demolish(w, h, u, dirty) && dirty.util && dirty.blockers);
        CHECK(!t.util.items[(size_t)ui].alive && t.util.version > v && t.util.removed.size() == 1);
        CHECK(Pick(pw, towerAt, 1.0f).kind != PickKind::Util); // gone
        DozeDirty d2;
        v = t.util.version;
        CHECK(Restore(w, u, d2) && t.util.items[(size_t)ui].alive && t.util.version > v && t.util.removed.empty() && d2.blockers);
        CHECK(!Restore(w, u, d2)); // only once
        // a transport building
        const int td = FlatTransportDef();
        CHECK(td >= 0);
        Placement p;
        p.valid = true, p.c = {150, 120};
        const int ti = t.transport.Place(p, td);
        h = Pick(pw, {150, 120}, 1.0f);
        CHECK(h.kind == PickKind::Transport && h.index == ti);
        dirty = {};
        v = t.transport.version;
        CHECK(Demolish(w, h, u, dirty) && dirty.transport && dirty.blockers && t.transport.version > v && t.transport.Count() == 0);
        CHECK(t.transport.removed.size() == 1);
        CHECK(Restore(w, u, dirty) && t.transport.Count() == 1 && t.transport.removed.empty());
        // a road: through the road tool's undo, refused once the network changed again
        h = Pick(pw, {200, 0}, 1.0f);
        CHECK(h.kind == PickKind::Road);
        const uint64_t nv = t.roads.net.version;
        dirty = {};
        CHECK(Demolish(w, h, u, dirty) && dirty.roads && t.roads.net.version != nv && !t.roads.net.segs[(size_t)h.index].alive);
        CHECK(t.roads.CanUndo());
        CHECK(Restore(w, u, dirty) && t.roads.net.segs[(size_t)h.index].alive);
        dirty = {};
        CHECK(Demolish(w, Pick(pw, {200, 0}, 1.0f), u, dirty));
        t.roads.BuildRoad(Cubic::Line({0, 300}, {100, 300}), 0, 0, ProfileIndex("Two-Lane Road")); // the world moved on
        CHECK(!Restore(w, u, dirty));
        // a zoned lot: the building comes down, its zone stays; undo stands it up again
        int li = -1;
        for (size_t i = 0; i < t.zones.lots.size(); ++i)
            if (t.zones.lots[i].alive) { li = (int)i; break; }
        CHECK(li >= 0);
        const Lot lot = t.zones.lots[(size_t)li];
        h = Pick(pw, lot.c, 0.5f);
        CHECK(h.kind == PickKind::Lot && h.index == li);
        v = t.zones.version;
        dirty = {};
        CHECK(Demolish(w, h, u, dirty) && dirty.zones && !t.zones.lots[(size_t)li].alive && t.zones.version > v);
        CHECK(!t.zones.removed.empty() && t.zones.removed.back() == lot.id);
        CHECK(t.zones.cells[(size_t)lot.cells[0]].zone == 0 && t.zones.cells[(size_t)lot.cells[0]].lot < 0); // still zoned, free
        CHECK(Restore(w, u, dirty) && t.zones.lots[(size_t)li].alive && t.zones.cells[(size_t)lot.cells[0]].lot == li);
        // a bulldoze area: every zoned cell inside is cleared, its lots with it, as one undo
        std::vector<int> area;
        t.zones.BoxCells({-300, -2}, {-40, -60}, {1, 0}, area);
        int zonedBefore = 0;
        for (int c : area) zonedBefore += t.zones.cells[(size_t)c].zone != kNoZone;
        CHECK(zonedBefore > 0);
        const int livesBefore = LiveLots(t.zones);
        dirty = {};
        CHECK(DezoneCells(t.zones, area, u, dirty) && dirty.zones);
        int zonedAfter = 0;
        for (int c : area) zonedAfter += t.zones.cells[(size_t)c].zone != kNoZone;
        CHECK(zonedAfter == 0 && LiveLots(t.zones) < livesBefore && !u.lots.empty());
        CHECK(Restore(w, u, dirty) && LiveLots(t.zones) == livesBefore);
        zonedAfter = 0;
        for (int c : area) zonedAfter += t.zones.cells[(size_t)c].zone != kNoZone;
        CHECK(zonedAfter == zonedBefore);
        // an empty cell is not a bulldoze target; nothing is demolished there
        dirty = {};
        h = Pick(pw, {100, 30}, 0.5f); // north side, x > 0: unzoned cells
        CHECK(h.kind == PickKind::None);
        CHECK(!Demolish(w, h, u, dirty) && !dirty.zones && !dirty.util);
        // an industry site (a fresh town: the road above was bulldozed)
        Town t2;
        DozeWorld w2 = t2.Doze();
        PickWorld pw2 = t2.World();
        pw2.agentsOn = pw2.terrainOn = pw2.emptyCellsOn = false;
        IndSite s;
        CHECK(t2.ind.Snap(t2.roads.net, {-200, 40}, s, 0));
        s.id = t2.ind.nextId++;
        t2.ind.sites.push_back(s);
        h = Pick(pw2, s.c, 0.5f);
        CHECK(h.kind == PickKind::IndSite);
        v = t2.ind.version;
        dirty = {};
        CHECK(Demolish(w2, h, u, dirty) && dirty.industry && dirty.industryId == s.id && t2.ind.sites.empty() && t2.ind.version > v);
        CHECK(Restore(w2, u, dirty) && t2.ind.sites.size() == 1 && t2.ind.sites[0].id == s.id);
    }

    // ---- the rotate dial
    {
        CHECK(Near(BearingDeg({0, 0}, {0, 10}), 0.0f) && Near(BearingDeg({0, 0}, {10, 0}), 90.0f) && Near(BearingDeg({0, 0}, {0, -10}), 180.0f));
        CHECK(Near(WrapDeg180(190.0f), -170.0f) && Near(WrapDeg180(-190.0f), 170.0f) && Near(WrapDeg180(45.0f), 45.0f));
        CHECK(Near(Norm360(-15.0f), 345.0f) && Near(Norm360(360.0f), 0.0f) && Near(Norm360(725.0f), 5.0f));
        CHECK(Near(SnapDeg(37.0f, 15.0f), 30.0f) && Near(SnapDeg(38.0f, 15.0f), 45.0f) && Near(SnapDeg(37.4f, 0.0f), 37.4f));
        CHECK(Near(DialStep(true, false, true), 15.0f) && Near(DialStep(false, false, true), 1.0f));
        CHECK(Near(DialStep(true, true, true), 90.0f) && Near(DialStep(false, true, true), 1.0f) && Near(DialStep(false, true, false), 90.0f));
        RotateDial d;
        d.Begin(30.0f, {100, 100});
        CHECK(d.on && Near(d.deg, 30.0f) && Near(d.origDeg, 30.0f));
        d.Press({100, 120}); // the finger lands due "north" of the ghost: no jump
        CHECK(!d.Drag({100, 120}, 15.0f) && Near(d.deg, 30.0f));
        CHECK(d.Drag({120, 100}, 15.0f) && Near(d.deg, 120.0f)); // a quarter turn around it: +90
        CHECK(Near(d.Turn(), 90.0f));
        CHECK(d.Drag({114, 114.5f}, 15.0f) && Near(std::fmod(d.deg, 15.0f), 0.0f)); // snapped to 15
        CHECK(d.Drag({100, 80}, 1.0f) && Near(d.deg, 210.0f)); // free: the whole degree
        CHECK(Near(d.Turn(), 180.0f) || Near(d.Turn(), -180.0f));
        CHECK(d.Drag({88, 120}, 90.0f) && Near(std::fmod(d.deg, 90.0f), 0.0f)); // road-snapped: quarter turns
        d.Release();
        CHECK(!d.Drag({120, 100}, 1.0f)); // no finger down: nothing turns
        CHECK(!d.Drag({100.1f, 100.1f}, 1.0f)); // on the centre itself: no bearing, no change
        // a turned footprint is what the planners validate: the dial's angle goes through the same Plan()
        Town t;
        const int pump = UDef("Groundwater Pump");
        const Placement a = t.util.Plan(t.Ctx(), pump, {60, 30}, d.deg, true);
        CHECK(a.valid);
    }

    // ---- pick: each info target at its spot
    {
        Town t;
        const int tower = UDef("Water Tower");
        const int ui = t.util.Place(t.util.Plan(t.Ctx(), tower, {60, 30}, 0, true), tower);
        Placement p;
        p.valid = true, p.c = {150, 120};
        const int ti = t.transport.Place(p, FlatTransportDef());
        const int line = t.util.AddLine(Util::Electricity, {0, 200}, {0, 280}, 0);
        std::vector<PickPoint> agents = {{{-50, 1.5f}, 2.5f, PickKind::Vehicle, 7, 1}, {{-52, -9}, 1.0f, PickKind::Person, 3, 0}};
        PickWorld w = t.World();
        w.agents = &agents;
        w.waste = [](V2 q) { return Len(q - V2{-200, 200}) < 10.0f ? 0 : -1; };
        int li = -1;
        for (size_t i = 0; i < t.zones.lots.size(); ++i)
            if (t.zones.lots[i].alive) { li = (int)i; break; }
        CHECK(Pick(w, {-50, 1.5f}, 0.5f).kind == PickKind::Vehicle && Pick(w, {-50, 1.5f}, 0.5f).index == 7 && Pick(w, {-50, 1.5f}, 0.5f).sub == 1);
        CHECK(Pick(w, {-52.3f, -9.2f}, 0.5f).kind == PickKind::Person);  // a person on the pavement, in front of the house
        CHECK(Pick(w, t.zones.lots[(size_t)li].c, 0.5f).kind == PickKind::Lot);
        CHECK(Pick(w, t.util.items[(size_t)ui].c, 0.5f).kind == PickKind::Util);
        CHECK(Pick(w, {150, 120}, 0.5f).kind == PickKind::Transport && Pick(w, {150, 120}, 0.5f).index == ti);
        CHECK(Pick(w, {-200, 200}, 0.5f).kind == PickKind::Waste);
        CHECK(Pick(w, {200, 1}, 0.5f).kind == PickKind::Road);
        CHECK(Pick(w, {1, 240}, 0.5f).kind == PickKind::UtilLine && Pick(w, {1, 240}, 0.5f).index == line);
        const PickHit cell = Pick(w, {100, 20}, 0.5f);
        CHECK(cell.kind == PickKind::ZoneCell && t.zones.cells[(size_t)cell.index].zone == kNoZone); // an empty cell for info
        CHECK(Pick(w, {900, 900}, 0.5f).kind == PickKind::Terrain);
        // the lot the ray hit wins over the ground point (a tall building seen at an angle covers the road behind it)
        w.lotUnderRay = li;
        CHECK(Pick(w, {200, 1}, 0.5f).kind == PickKind::Lot);
        w.lotUnderRay = -1;
        // with agents off (bulldoze) the car's spot is the road under it
        w.agentsOn = false;
        CHECK(Pick(w, {-50, 1.5f}, 0.5f).kind == PickKind::Road);
        CHECK(std::string(PickKindName(PickKind::Lot)) == "building");
    }

    if (failures) {
        std::printf("interact_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("interact_test: all passed\n");
    return 0;
}
