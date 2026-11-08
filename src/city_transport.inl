// ------------------------------------------------------------------- transport buildings (src/city/transport.h, transport_models.h,
// gui/transport_toolbar.h). Included by main.cpp after city_zones.inl. The Transport button opens a picker (Rail, Ships, Air);
// the chosen building follows the cursor as a blue hologram (red with the reason when the spot is refused: shipyards and
// harbours only ever fit on a qualifying coast). A click places it: it stays a hologram while "Planned", then a construction
// site rises, then it stands. Railway tracks are laid with the road tool (the Rail tab hands over to it).

void TransitCursor();
void TransitClick();
void TransitSync(float realDt);
void CitizensSync();
tbd::city::TransportWorld g_transport;
tbd::gui::TransportToolState g_transportTool;
struct TransportInput {
    bool have = false; // cursor on the ground
    tbd::Vec3 hit{};
    tbd::city::Placement plan;
    int hover = -1; // bulldoze: the building under the cursor
} g_transportIn;
struct TransportCache {
    std::vector<tbd::Vertex> tris;
    bool built = false;
};
std::unordered_set<uint64_t> g_transportGlbShown; // GLB buildings whose finished model is in the scene (or failed to load: not retried)
std::unordered_map<uint64_t, TransportCache> g_transportCache; // finished buildings: their world-space geometry, made once

tbd::city::PlaceContext TransportContext() {
    tbd::city::PlaceContext cx;
    cx.net = &g_roads.net;
    cx.zones = &g_zones;
    cx.ground = RoadsGround;
    cx.sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1 ? tbd::IslandMap::kSeaLevel : -1e9f; // only the island has a sea
    return cx;
}

// The zone grid keeps clear of the buildings (and drops cells under them).
void TransportRefreshZoning() {
    g_zones.blockers = g_transport.Blockers();
    UtilAppendBlockers(g_zones.blockers); // and the utility sources (city_utilities.inl)
    g_zones.builtFrom = ~0ull; // ZonesSync rebuilds the cells next frame
}

void TransportCursor() {
    tbd::Vec3 hit;
    g_transportIn.have = PlaceCursorHit(hit); // [interact] was the cursor's ground hit (not over the GUI); the same for everything but a touch ghost
    g_transportIn.hover = -1;
    if (!g_transportIn.have) return;
    g_transportIn.hit = hit;
    if (g_transportTool.lines) { TransitCursor(); return; } // the Lines page plans checkpoints, not buildings
    const tbd::city::V2 p{hit.x, hit.z};
    if (g_transportTool.bulldoze) g_transportIn.hover = g_transport.HitTest(p);
    else if (g_transportTool.def >= 0) {
        tbd::city::PlanOptions o = g_transportTool.Options();
        if (BuildingFreePlace()) o.snapRoad = false; // [snap] the Rotate dial is out: free position and heading
        g_transportIn.plan = g_transport.Plan(TransportContext(), g_transportTool.def, p, g_transportTool.rotation, o);
    }
}

// ------------------------------------------------------------------- rail yards (src/city/rail_yard.h): track ends, trains
tbd::city::RailYardWorld g_railYards;
std::unordered_map<uint64_t, std::string> g_railYardNote; // the last status message shown per yard
// Every yard's track ends go to the rail tool (its "yard" connection snaps); the trains run; a yard that loses its road or
// rail (or never had one) says so once.
void RailYardSync(float realDt) {
    using namespace tbd::city;
    static uint64_t netV = 0, twV = 0;
    if (netV != g_roads.net.version || twV != g_transport.version) { // the ends move only with the roads (the entrance) or the yards
        netV = g_roads.net.version, twV = g_transport.version;
        g_roads.ports.clear();
        for (const Placed& b : g_transport.items)
            if (b.alive) {
                const std::vector<RailPort> v = TransportWorld::PortsOf(&g_roads.net, b);
                g_roads.ports.insert(g_roads.ports.end(), v.begin(), v.end());
            }
    }
    if (g_clock.Rate() > 0) g_railYards.Tick(g_roads.net, g_transport, realDt);
    for (const YardStatus& st : g_railYards.status) {
        const std::string m = st.Message();
        std::string& last = g_railYardNote[st.yard];
        if (m != last && !m.empty()) Toast(RoadsUpper(m));
        last = m;
    }
}

// The placeholder trains: a loco and three cars as boxes (no train model in assets), each car on the curve it is on.
void RailYardDraw(std::vector<tbd::Vertex>& tris);

// Levels the terrain under a new building's land (a coast's quay level is just over the sea; a station sits on its track).
void TransportLevel(const tbd::city::Placed& b) {
    using namespace tbd::city;
    const BuildingDef& d = Defs()[b.def];
    if (d.site == Site::Track || !g_roads.flatten) return;
    const Frame fr(b.c, b.front);
    const float W = d.W(), D = d.D();
    const float zMax = d.site == Site::Coast ? D * (0.5f - d.dock) + 3.0f : D * 0.5f;
    const float step = W * D > 20000.0f ? 14.0f : 10.0f, hard = step * 0.7f, radius = step * 1.15f;
    for (int pass = 0; pass < 2; ++pass)
        for (float x = -W * 0.5f + step * 0.5f; x < W * 0.5f; x += step)
            for (float z = -D * 0.5f + step * 0.5f; z < zMax; z += step) {
                const V2 q = fr.World(x, z);
                float t, dist;
                if (g_roads.net.SegmentAt(q, radius, t, dist) >= 0) continue; // keep the brush off every road bed
                g_roads.flatten(q.x, q.z, hard, radius, b.y);
            }
    g_zones.Reground(RoadsGround);
}

void TransportClick() {
    TransportCursor();
    if (!g_transportIn.have) return;
    if (g_transportTool.lines) { TransitClick(); return; }
    if (g_transportTool.bulldoze) {
        if (g_transportIn.hover >= 0 && g_transport.Remove(g_transportIn.hover)) {
            TransportRefreshZoning();
            Toast("BUILDING REMOVED");
        }
        return;
    }
    if (g_transportTool.def < 0) return;
    const tbd::city::Placement& p = g_transportIn.plan;
    if (!p.valid) {
        Toast(RoadsUpper(p.error));
        return;
    }
    const int i = g_transport.Place(p, g_transportTool.def);
    if (i < 0) return;
    TransportLevel(g_transport.items[i]);
    TransportRefreshZoning();
    Toast(RoadsUpper(std::string(tbd::city::Defs()[g_transportTool.def].name) + " PLANNED"));
}

// Per frame: game minutes advance the construction.
void TransportSync(float realDt) {
    static bool started = false;
    if (!started) { // TBD_TRANSPORT=<building index>: open the tool with it chosen (captures)
        started = true;
        char v[16] = {};
        if (tbd::platform::GetEnvA("TBD_TRANSPORT", v, sizeof(v)) > 0) {
            g_transportTool.active = true;
            const int d = std::clamp(atoi(v), 0, (int)tbd::city::Defs().size() - 1);
            g_transportTool.Select(d);
        }
    }
    static bool demoDone = false;
    if (!demoDone && g_engine.terrain.params.enabled) { // TBD_TRANSPORT_DEMO="def x z progress;...": placed buildings for captures (progress < 0 planned, 0..1 building, 1 done)
        demoDone = true;
        char v[512] = {};
        if (tbd::platform::GetEnvA("TBD_TRANSPORT_DEMO", v, sizeof(v)) > 0) {
            RoadsInit(); // the terrain levelling hook (TransportLevel) is set there
            for (char* tok = strtok(v, ";"); tok; tok = strtok(nullptr, ";")) {
                int d = 0;
                float x = 0, z = 0, prog = 1;
                int force = 0; // a 5th value 1 places it even where the rules refuse (the levelling flattens the lot)
                if (sscanf(tok, "%d %f %f %f %d", &d, &x, &z, &prog, &force) < 3 || d < 0 || d >= (int)tbd::city::Defs().size()) continue;
                tbd::city::Placement p = g_transport.Plan(TransportContext(), d, {x, z}, 0, {});
                if (force && !p.valid) p.valid = true, p.y = RoadsGround(x, z);
                const int i = g_transport.Place(p, d);
                tbd::Log("Transport demo: %s at (%.0f, %.0f): %s", tbd::city::Defs()[d].name, x, z, p.valid ? "placed" : p.error.c_str());
                if (i < 0) continue;
                tbd::city::Placed& b = g_transport.items[i];
                if (prog >= 0) b.state = prog >= 1 ? tbd::city::BState::Done : tbd::city::BState::Building, b.progress = std::min(1.0f, prog);
                TransportLevel(b);
            }
            TransportRefreshZoning();
        }
    }
    const float minutes = realDt * g_clock.Rate();
    if (minutes > 0) g_transport.Tick(minutes);
    TransitSync(realDt);
    RailYardSync(realDt);
    CitizensSync();
    for (uint64_t id : g_transport.removed) {
        g_transportCache.erase(id);
        if (g_transportGlbShown.erase(id)) UtilDeleteEntity("transport_" + std::to_string(id));
    }
    for (const tbd::city::Placed& b : g_transport.items) { // a finished GLB building stands in the scene as an entity
        const tbd::city::BuildingDef& d = tbd::city::Defs()[b.def];
        if (!b.alive || b.state != tbd::city::BState::Done || !d.glb || g_transportGlbShown.count(b.id)) continue;
        g_transportGlbShown.insert(b.id);
        const std::string name = "transport_" + std::to_string(b.id);
        int idx;
        if (!UtilEntityIndex(name, idx) && !UtilSpawnEntity(name, d.glb, b.c, b.y, b.front, {1, 1, 1})) tbd::Log("Transport: could not load %s", d.glb);
    }
    g_transport.removed.clear();
    if (g_zones.blockers.size() != (size_t)g_transport.Count() + UtilBlockerCount()) TransportRefreshZoning(); // e.g. after a load
}

// ------------------------------------------------------------------- transit lines, outside connections, traffic (src/city/transit.h)
tbd::city::TransitWorld g_transit;
void CitizensSync();
struct TransitInput {
    tbd::city::StopPlan stop;   // the checkpoint the next click would make
    bool outValid = false;
    tbd::city::V2 outAt{}, outDir{1, 0};
    float outY = 0;
    std::string outErr;
} g_transitIn;
uint64_t g_transitNetVersion = 0;
std::unordered_map<uint64_t, std::string> g_stopEntity; // bus / tram checkpoints that stand as GLB models (a shelter, or a pole for cargo stops)

// Bus and tram checkpoints are the modelled bus shelter / pole: a scene entity beside the road, made once, removed with the stop.
void TransitStopModels() {
    using namespace tbd::city;
    for (const Stop& s : g_transit.stops) {
        if (!s.alive || s.outside >= 0 || (s.mode != TMode::Bus && s.mode != TMode::Tram) || g_stopEntity.count(s.id)) continue;
        const std::string name = "stop_" + std::to_string(s.id);
        const char* glb = s.kind == TKind::Passenger ? "assets/buildings/transport/bus_stop/glb/BS_1x1_bus_shelter_L1.glb" : "assets/buildings/transport/bus_stop/glb/BS_1x1_bus_stop_pole_L1.glb";
        const V2 side = RightOf(s.dir);                 // the kerb is on the right of the way of travel
        const V2 at = s.p + side * 6.0f, front = side * -1.0f; // its front faces the road
        int idx;
        if (UtilEntityIndex(name, idx) || UtilSpawnEntity(name, glb, at, RoadsGround(at.x, at.z), front, {1, 1, 1})) g_stopEntity[s.id] = name;
        else g_stopEntity[s.id] = ""; // could not load: not retried
    }
    for (auto it = g_stopEntity.begin(); it != g_stopEntity.end();) {
        const Stop* alive = g_transit.FindStop(it->first);
        if (alive) { ++it; continue; }
        if (!it->second.empty()) UtilDeleteEntity(it->second);
        it = g_stopEntity.erase(it);
    }
}


// People on foot (Mixamo walk / idle clips on the rigged low-poly human, assets/characters, made by tools/anim_gen). A pool of
// scene entities, twelve looks; a citizen takes a free entity of its look and the rest sit hidden under the ground.
struct CitizenSlot { std::string name; int mesh = -1; bool used = false; };
std::vector<CitizenSlot> g_citizenSlots; // slot k has look k % 12
void CitizensSync() {
    using namespace tbd::city;
    auto& ents = g_engine.scene.entities;
    for (CitizenSlot& s : g_citizenSlots) s.used = false;
    static std::vector<std::string> clips;
    if (clips.empty()) clips = {UtilAbsolute("assets/characters/anim_idle_happy.glb"), UtilAbsolute("assets/characters/anim_walk.glb"),
                                  UtilAbsolute("assets/characters/anim_sit_down.glb"), UtilAbsolute("assets/characters/anim_sit_hold.glb"),
                                  UtilAbsolute("assets/characters/anim_sit_to_stand.glb")};
    const float now = (float)std::fmod((double)tbd::platform::TickCountMs() * 0.001, 10000.0);
    for (const Citizen& c : g_transit.citizens) {
        if (!c.alive) continue;
        int pick = -1;
        for (size_t k = 0; k < g_citizenSlots.size(); ++k)
            if (!g_citizenSlots[k].used && (int)(k % 12) == c.variant) { pick = (int)k; break; }
        if (pick < 0) { // grow the pool: slot index must keep its look
            if (g_citizenSlots.size() >= 96) continue;
            while (true) {
                const int k = (int)g_citizenSlots.size();
                CitizenSlot s;
                s.name = "citizen_" + std::to_string(k);
                tbd::Entity e;
                e.mesh = tbd::MeshKind::Imported;
                e.meshPath = UtilAbsolute(("assets/characters/human_" + std::to_string(k % 12) + ".glb").c_str()) + "#native";
                e.animClip = clips[0];
                e.name = s.name;
                e.position = {0, -10000.0f, 0};
                e.scale = {1, 1, 1};
                CompleteSceneLoad();
                const int keepSel = g_engine.scene.selected;
                g_engine.scene.Add(e);
                g_engine.scene.selected = keepSel;
                const tbd::MeshId id = UploadEntityMesh(e);
                g_engine.meshes.push_back(id);
                if (id != tbd::kInvalidMesh) g_engine.animator.SetBlendClips(id, clips, "mixamorig:Hips");
                g_citizenSlots.push_back(s);
                if (k % 12 == c.variant) { pick = k; break; }
            }
        }
        CitizenSlot& slot = g_citizenSlots[(size_t)pick];
        int idx;
        if (!UtilEntityIndex(slot.name, idx) || (size_t)idx >= g_engine.meshes.size()) continue;
        slot.used = true;
        tbd::Entity& e = ents[(size_t)idx];
        const float y = RoadsGround(c.pos.x, c.pos.z);
        e.position = {c.pos.x, y, c.pos.z};
        e.rotation = tbd::QuatAxisAngle({0, 1, 0}, std::atan2(c.dir.x, c.dir.z));
        const tbd::MeshId id = g_engine.meshes[(size_t)idx];
        if (id == tbd::kInvalidMesh) continue;
        static float dur[5] = {};
        if (dur[0] <= 0.0f) {
            for (int k = 0; k < 5; ++k) dur[k] = std::max(0.1f, g_engine.animator.ClipDuration(clips[(size_t)k]));
            g_transit.sitDur = dur[4]; // the sit-down clip is the stand-up one run backwards
        }
        const int a = std::clamp(c.anim, 0, 4);
        static const int which[5] = {0, 1, 2, 3, 4};
        float times[5] = {std::fmod(now + (float)c.id * 0.37f, dur[0]), std::fmod(c.s / 1.4f, dur[1]), std::min(c.animT, dur[2] - 0.01f), 0.0f, std::min(c.animT, dur[4] - 0.01f)};
        float weights[5] = {};
        weights[which[a]] = 1.0f;
        g_engine.animator.SetBlendPose(id, times, weights);
    }
    for (const CitizenSlot& s : g_citizenSlots) {
        int idx;
        if (!s.used && UtilEntityIndex(s.name, idx) && ents[(size_t)idx].position.y > -9000.0f) ents[(size_t)idx].position = {0, -10000.0f, 0};
    }
}

void TransitCursor() {
    using namespace tbd::city;
    if (!g_transportTool.lines || !g_transportIn.have) return;
    const V2 p{g_transportIn.hit.x, g_transportIn.hit.z};
    const PlaceContext cx = TransportContext();
    if (g_transportTool.lineBulldoze) return;
    if (g_transportTool.outKind >= 0) {
        g_transitIn.outErr.clear();
        g_transitIn.outValid = g_transit.PlanOutside(cx, (OutKind)g_transportTool.outKind, p, g_transitIn.outAt, g_transitIn.outDir, g_transitIn.outY, g_transitIn.outErr);
    } else {
        g_transit.bMode = g_transportTool.lmode, g_transit.bKind = g_transportTool.lkind;
        g_transitIn.stop = g_transit.PlanStop(cx, g_transport, g_transit.bMode, g_transit.bKind, p);
    }
}

void TransitClick() {
    using namespace tbd::city;
    TransitCursor();
    if (!g_transportIn.have) return;
    const V2 p{g_transportIn.hit.x, g_transportIn.hit.z};
    const PlaceContext cx = TransportContext();
    if (g_transportTool.lineBulldoze) {
        int i;
        if ((i = g_transit.OutsideAt(p)) >= 0) g_transit.RemoveOutside(i), Toast("OUTSIDE CONNECTION REMOVED");
        else if ((i = g_transit.StopAt(p)) >= 0) {
            // removing a checkpoint takes every line through it
            const uint64_t id = g_transit.stops[i].id;
            for (size_t r = 0; r < g_transit.routes.size(); ++r)
                if (g_transit.routes[r].alive && std::find(g_transit.routes[r].stops.begin(), g_transit.routes[r].stops.end(), id) != g_transit.routes[r].stops.end()) g_transit.RemoveRoute((int)r);
            g_transit.stops[i].alive = false;
            Toast("CHECKPOINT REMOVED");
        } else if ((i = g_transit.RouteNear(p)) >= 0) g_transit.RemoveRoute(i), Toast("LINE REMOVED");
        return;
    }
    if (g_transportTool.outKind >= 0) {
        if (!g_transitIn.outValid) return Toast(RoadsUpper(g_transitIn.outErr));
        if (g_transit.AddOutside(cx, (OutKind)g_transportTool.outKind, p) < 0) return Toast("AN OUTSIDE CONNECTION IS ALREADY HERE");
        Toast(RoadsUpper(std::string(OutKindName((OutKind)g_transportTool.outKind)) + " CONNECTION PLACED"));
        return;
    }
    g_transit.bMode = g_transportTool.lmode, g_transit.bKind = g_transportTool.lkind;
    const LineEvent e = g_transit.Click(cx, g_transport, p);
    if (e.what != LineEvent::None) Toast(RoadsUpper(e.text));
}

// Why anyone comes (transit.h Demand): homes and jobs from the built lots and the transport buildings.
void TransitDemand() {
    using namespace tbd::city;
    Demand d;
    const auto& types = ZoneTypes();
    int nHome = 0, nJob = 0;
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        const ZoneCat c = types[l.type].cat;
        const LotCap cap = LotCapacity(l); // the sim's capacity (sim_cells.h): the two can never disagree
        if (c == ZoneCat::Residential) {
            d.homes += cap.homes;
            if (++nHome % 3 == 0 && d.homePts.size() < 64) d.homePts.push_back(l.c);
        } else {
            const float jobs = (float)cap.Jobs();
            d.jobs += jobs;
            if (c != ZoneCat::Office) d.goods += jobs;
            if (++nJob % 3 == 0 && d.jobPts.size() < 64) d.jobPts.push_back(l.c);
        }
    }
    for (const Placed& b : g_transport.items)
        if (b.alive && b.state == BState::Done) {
            d.jobs += (float)Defs()[b.def].workplaces;
            if (d.jobPts.size() < 96) d.jobPts.push_back(b.c);
        }
    g_transit.demand = d;
}

void TransitSync(float realDt) {
    using namespace tbd::city;
    if (g_transportTool.cancelLine || (!g_transportTool.active && !g_transit.building.empty())) g_transit.Cancel(), g_transportTool.cancelLine = false;
    if (g_transportTool.finishLine) {
        g_transportTool.finishLine = false;
        const LineEvent e = g_transit.Finish(TransportContext());
        if (e.what != LineEvent::None) Toast(RoadsUpper(e.text));
    }
    g_transportTool.lineStops = (int)g_transit.building.size();
    TransitStopModels();
    if (g_roads.net.version != g_transitNetVersion) { // roads changed: lines re-find their way
        g_transitNetVersion = g_roads.net.version;
        g_transit.Rebuild(TransportContext());
    }
    static float demandT = 0;
    demandT -= realDt;
    if (demandT <= 0) demandT = 1.0f, TransitDemand();
    if (g_clock.Rate() > 0) g_transit.Tick(TransportContext(), realDt);
    if (g_transportTool.active && g_transportTool.lines) {
        const auto& d = g_transit.demand;
        const float employed = std::min(g_transit.residents * 0.6f, d.jobs);
        std::snprintf(g_transportTool.status, sizeof(g_transportTool.status), "Residents %d   Commuters %d   Open jobs %d   Vacant homes %d   Jam %d%%   Lines %d   Outside %d",
                      (int)g_transit.residents, (int)g_transit.commuters, (int)std::max(0.0f, d.jobs - employed - g_transit.commuters), (int)std::max(0.0f, d.homes - g_transit.residents),
                      (int)(g_transit.jam * 100.0f), g_transit.RouteCount(), g_transit.OutsideCount());
    }
}

// ---- drawing: unlit flat shapes (ribbons for lines, poles for checkpoints, arrows for outside connections, boxes for vehicles)
namespace transit_draw {
using tbd::Vertex;
using tbd::city::V2;
inline Vertex Vx(float x, float y, float z, tbd::Vec3 c, float a) { return {x, y, z, 0, 0, 0, c.x, c.y, c.z, a}; }
inline void Quad(std::vector<Vertex>& o, tbd::Vec3 a, tbd::Vec3 b, tbd::Vec3 c, tbd::Vec3 d, tbd::Vec3 col, float al) {
    o.push_back(Vx(a.x, a.y, a.z, col, al)), o.push_back(Vx(b.x, b.y, b.z, col, al)), o.push_back(Vx(c.x, c.y, c.z, col, al));
    o.push_back(Vx(a.x, a.y, a.z, col, al)), o.push_back(Vx(c.x, c.y, c.z, col, al)), o.push_back(Vx(d.x, d.y, d.z, col, al));
}
// an oriented box: centre c, forward dir, half length / width, y0..y1
inline void Box(std::vector<Vertex>& o, V2 c, V2 dir, float hl, float hw, float y0, float y1, tbd::Vec3 col, float al = 1.0f) {
    const V2 f = dir, sd = {dir.z, -dir.x};
    auto P = [&](float a, float b, float y) { return tbd::Vec3{c.x + f.x * a + sd.x * b, y, c.z + f.z * a + sd.z * b}; };
    const tbd::Vec3 shade[3] = {col, {col.x * 0.75f, col.y * 0.75f, col.z * 0.75f}, {col.x * 0.55f, col.y * 0.55f, col.z * 0.55f}};
    Quad(o, P(-hl, -hw, y1), P(hl, -hw, y1), P(hl, hw, y1), P(-hl, hw, y1), shade[0], al); // roof
    Quad(o, P(-hl, -hw, y0), P(hl, -hw, y0), P(hl, -hw, y1), P(-hl, -hw, y1), shade[1], al);
    Quad(o, P(-hl, hw, y1), P(hl, hw, y1), P(hl, hw, y0), P(-hl, hw, y0), shade[1], al);
    Quad(o, P(hl, -hw, y0), P(hl, hw, y0), P(hl, hw, y1), P(hl, -hw, y1), shade[2], al);
    Quad(o, P(-hl, hw, y0), P(-hl, -hw, y0), P(-hl, -hw, y1), P(-hl, hw, y1), shade[2], al);
}
// a flat arrow at height y, pointing along dir
inline void Arrow(std::vector<Vertex>& o, V2 c, V2 dir, float len, float w, float y, tbd::Vec3 col, float al = 1.0f) {
    const V2 sd = {dir.z, -dir.x};
    auto P = [&](float a, float b) { return tbd::Vec3{c.x + dir.x * a + sd.x * b, y, c.z + dir.z * a + sd.z * b}; };
    const float h = len * 0.5f, neck = h * 0.15f;
    Quad(o, P(-h, -w * 0.3f), P(neck, -w * 0.3f), P(neck, w * 0.3f), P(-h, w * 0.3f), col, al); // shaft
    const tbd::Vec3 a = P(neck, -w), b = P(h, 0), d = P(neck, w);
    o.push_back(Vx(a.x, y, a.z, col, al)), o.push_back(Vx(b.x, y, b.z, col, al)), o.push_back(Vx(d.x, y, d.z, col, al)); // head
}
// a ribbon along a polyline, lifted `lift` above the ground
inline void Ribbon(std::vector<Vertex>& o, const std::vector<V2>& p, float width, float lift, tbd::Vec3 col, float al, bool dashed = false) {
    float acc = 0;
    for (size_t i = 1; i < p.size(); ++i) {
        const V2 d = p[i] - p[i - 1];
        const float l = tbd::city::Len(d);
        if (l < 0.05f) continue;
        acc += l;
        if (dashed && std::fmod(acc, 12.0f) > 7.0f) continue;
        const V2 n = {-d.z / l * width * 0.5f, d.x / l * width * 0.5f};
        const float ya = RoadsGround(p[i - 1].x, p[i - 1].z) + lift, yb = RoadsGround(p[i].x, p[i].z) + lift;
        Quad(o, {p[i - 1].x - n.x, ya, p[i - 1].z - n.z}, {p[i].x - n.x, yb, p[i].z - n.z}, {p[i].x + n.x, yb, p[i].z + n.z}, {p[i - 1].x + n.x, ya, p[i - 1].z + n.z}, col, al);
    }
}
inline tbd::Vec3 OutColor(tbd::city::OutKind k) { return tbd::city::TModeColor(tbd::city::OutMode(k)); }
inline void DoubleArrow(std::vector<Vertex>& o, V2 c, V2 dir, float y, tbd::Vec3 col, float al) {
    const V2 sd = {dir.z, -dir.x};
    Arrow(o, c + sd * 5.5f, dir, 44.0f, 11.0f, y, col, al);         // out of the city
    Arrow(o, c - sd * 5.5f, dir * -1.0f, 44.0f, 11.0f, y, col, al); // into the city
}
} // namespace transit_draw

void TransitDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace tbd::city;
    namespace td = transit_draw;
    static std::vector<tbd::Vertex> tris;
    tris.clear();
    const float time = (float)std::fmod((double)tbd::platform::TickCountMs() * 0.001, 10000.0);
    const PlaceContext cx = TransportContext();
    const bool showAll = g_transportTool.active && g_transportTool.lines;
    for (const Route& rt : g_transit.routes) {
        if (!rt.alive) continue;
        const tbd::Vec3 col = TModeColor(rt.mode);
        const bool cargo = rt.kind == TKind::Cargo;
        for (const auto& leg : rt.legs)
            td::Ribbon(tris, leg, rt.mode == TMode::Plane ? 1.0f : 3.2f, rt.mode == TMode::Subway ? 0.3f : rt.mode == TMode::Train ? 0.9f : 0.6f,
                       cargo ? tbd::Vec3{col.x * 0.7f, col.y * 0.7f, col.z * 0.7f} : col, 0.9f, rt.mode == TMode::Subway || rt.mode == TMode::Plane || !rt.connected);
    }
    if (!g_transit.building.empty()) { // the line being built
        std::vector<V2> pts;
        for (uint64_t id : g_transit.building)
            if (const Stop* st = g_transit.FindStop(id)) pts.push_back(st->p);
        if (showAll && g_transportIn.have && g_transitIn.stop.valid) pts.push_back(g_transitIn.stop.p);
        td::Ribbon(tris, pts, 3.2f, 1.2f, TModeColor(g_transit.bMode), 0.75f, true);
    }
    for (const Stop& st : g_transit.stops) { // checkpoints: a pole with a sign
        if (!st.alive || st.outside >= 0) continue;
        if (const auto me = g_stopEntity.find(st.id); me != g_stopEntity.end() && !me->second.empty()) continue; // drawn as its model
        const tbd::Vec3 col = TModeColor(st.mode);
        const float y = st.y;
        td::Box(tris, st.p, st.dir, 0.4f, 0.4f, y, y + 7.0f, {0.85f, 0.87f, 0.9f});
        td::Box(tris, st.p, st.dir, 2.0f, 2.0f, y + 7.0f, y + 9.0f, st.kind == TKind::Cargo ? tbd::Vec3{col.x * 0.7f, col.y * 0.7f, col.z * 0.7f} : col);
        if (st.kind == TKind::Cargo) td::Box(tris, st.p, st.dir, 1.0f, 1.0f, y + 9.0f, y + 10.2f, {1, 1, 1});
        if (st.kind == TKind::Passenger && HasBench(st.mode)) { // the bench people sit on while they wait for the line
            const V2 bc = BenchCentre(st.p, st.dir), back = bc + StopSide(st.dir) * 0.32f;
            const float g = RoadsGround(bc.x, bc.z);
            td::Box(tris, bc, st.dir, 1.8f, 0.34f, g + 0.42f, g + 0.5f, {0.55f, 0.38f, 0.22f});
            td::Box(tris, back, st.dir, 1.8f, 0.04f, g + 0.5f, g + 0.95f, {0.5f, 0.34f, 0.2f});
            for (int k = -1; k <= 1; k += 2) td::Box(tris, bc + st.dir * (1.5f * (float)k), st.dir, 0.06f, 0.3f, g, g + 0.42f, {0.18f, 0.18f, 0.2f});
        }
    }
    for (const Outside& o : g_transit.outsides) { // two-way arrows; air lanes float and bob, the rest lie level
        if (!o.alive) continue;
        const tbd::Vec3 col = td::OutColor(o.kind);
        const float bob = o.kind == OutKind::Air ? std::sin(time * 1.3f + o.p.x * 0.01f) * 2.5f : 0.0f;
        const float y = (o.kind == OutKind::Air ? o.y : std::max(o.y, RoadsGround(o.p.x, o.p.z)) + 1.6f) + bob;
        td::DoubleArrow(tris, o.p, o.dir, y, o.linked ? col : tbd::Vec3{0.55f, 0.55f, 0.55f}, 1.0f);
        if (o.kind == OutKind::Air) td::Box(tris, o.p, o.dir, 0.5f, 0.5f, RoadsGround(o.p.x, o.p.z), y, {col.x * 0.6f, col.y * 0.6f, col.z * 0.6f}); // its mast
    }
    if (showAll && g_transportIn.have && !g_transportTool.lineBulldoze) { // what a click would place, translucent
        if (g_transportTool.outKind >= 0) {
            const tbd::Vec3 col = g_transitIn.outValid ? td::OutColor((OutKind)g_transportTool.outKind) : tbd::Vec3{1.0f, 0.3f, 0.25f};
            td::DoubleArrow(tris, g_transitIn.outAt, g_transitIn.outDir, g_transitIn.outY + (g_transportTool.outKind == (int)OutKind::Air ? 0.0f : 1.8f), col, 0.6f);
        } else {
            const StopPlan& sp = g_transitIn.stop;
            const tbd::Vec3 col = sp.valid ? tbd::Vec3{0.3f, 1.0f, 0.5f} : tbd::Vec3{1.0f, 0.3f, 0.25f};
            td::Box(tris, sp.p, sp.dir, 0.5f, 0.5f, sp.y, sp.y + 7.0f, col, 0.6f);
            td::Box(tris, sp.p, sp.dir, 2.4f, 2.4f, sp.y + 7.0f, sp.y + 9.2f, col, 0.6f);
        }
    }
    for (const Vehicle& v : g_transit.vehicles) {
        if (!v.alive) continue;
        const tbd::Vec3 col = v.route >= 0 ? TModeColor(v.mode) : (v.kind == TKind::Cargo ? tbd::Vec3{0.75f, 0.55f, 0.3f} : tbd::Vec3{0.8f, 0.82f, 0.88f});
        const float g0 = v.mode == TMode::Boat && cx.HasWater() ? cx.sea : RoadsGround(v.pos.x, v.pos.z);
        const V2 d = v.dir;
        switch (v.mode) {
        case TMode::Bus:
            if (v.route >= 0) td::Box(tris, v.pos, d, 5.5f, 1.3f, g0 + 0.3f, g0 + 3.1f, col);
            else td::Box(tris, v.pos, d, v.kind == TKind::Cargo ? 3.5f : 2.2f, 1.0f, g0 + 0.3f, g0 + (v.kind == TKind::Cargo ? 2.8f : 1.6f), col);
            break;
        case TMode::Tram: case TMode::Subway:
            for (int k = 0; k < 2; ++k) td::Box(tris, v.pos + d * (k ? 6.2f : -6.2f), d, 6.0f, 1.3f, g0 + 0.6f, g0 + 3.4f, col);
            break;
        case TMode::Train:
            for (int k = 0; k < 4; ++k) td::Box(tris, v.pos - d * (k * 13.0f - 19.0f), d, 6.0f, 1.4f, g0 + 0.9f, g0 + 4.2f, k == 0 ? tbd::Vec3{col.x * 0.7f, col.y * 0.7f, col.z * 0.7f} : col);
            break;
        case TMode::Boat:
            td::Box(tris, v.pos, d, 9.0f, 2.6f, g0 - 0.5f, g0 + 2.0f, col);
            td::Box(tris, v.pos - d * 3.0f, d, 3.0f, 2.0f, g0 + 2.0f, g0 + 5.0f, {0.95f, 0.95f, 0.95f});
            break;
        case TMode::Plane: {
            const float alt = g0 + 1.5f + std::min(1.0f, v.speed / TModeSpeed(v.mode)) * 45.0f;
            td::Box(tris, v.pos, d, 14.0f, 1.6f, alt, alt + 3.2f, {0.95f, 0.95f, 0.98f});
            td::Box(tris, v.pos, V2{d.z, -d.x}, 1.8f, 13.0f, alt + 1.0f, alt + 1.5f, col);
            break;
        }
        default: break;
        }
    }
    RailYardDraw(tris); // the rail yards' trains
    if (!tris.empty()) r.DrawTransient(tris.data(), (uint32_t)tris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
}

void RailYardDraw(std::vector<tbd::Vertex>& tris) {
    namespace td = transit_draw;
    for (const tbd::city::YardTrain& t : g_railYards.trains) {
        if (!t.alive || t.phase == 3) continue; // stabled in the shed
        for (int k = 0; k < tbd::city::kTrainCars; ++k) {
            const tbd::Vec3 col = k == 0 ? tbd::Vec3{0.62f, 0.18f, 0.14f} : tbd::Vec3{0.78f, 0.80f, 0.84f};
            const float y = t.carY[k];
            td::Box(tris, t.car[k], t.carDir[k], tbd::city::kTrainCarLen * 0.5f - 0.6f, 1.45f, y + 0.9f, y + 4.3f, col);
            td::Box(tris, t.car[k], t.carDir[k], tbd::city::kTrainCarLen * 0.5f - 0.4f, 1.0f, y + 0.25f, y + 0.9f, {0.16f, 0.16f, 0.18f}); // bogies
        }
    }
}

// The buildings, the hologram of the one being placed, and the lot markers.
void TransportDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace tbd::city;
    static std::vector<tbd::Vertex> solid, holoTris, holoLines, tmp, tmpEdges;
    solid.clear(), holoTris.clear(), holoLines.clear();
    const float time = (float)std::fmod((double)tbd::platform::TickCountMs() * 0.001, 10000.0);
    if (g_transportTool.active && !g_play.active) TransportCursor();
    for (const Placed& b : g_transport.items) {
        if (!b.alive) continue;
        tmp.clear(), tmpEdges.clear();
        if (b.state == BState::Planned) { // the hologram until the site opens
            tmodel::BuildPlaced(b, time, tmp, &tmpEdges);
            tmodel::Holographic(tmp, holoTris, time, true);
            tmodel::HolographicEdges(tmpEdges, holoLines, time, true);
        } else if (b.state == BState::Building) { // the site changes every frame (crane, rise)
            tmodel::BuildPlaced(b, time, tmp, nullptr);
            solid.insert(solid.end(), tmp.begin(), tmp.end());
        } else {
            if (tbd::city::Defs()[b.def].glb && g_transportGlbShown.count(b.id)) continue; // drawn as a scene entity
            TransportCache& c = g_transportCache[b.id];
            if (!c.built) {
                tmodel::BuildPlaced(b, time, c.tris, nullptr);
                c.built = true;
            }
            solid.insert(solid.end(), c.tris.begin(), c.tris.end());
        }
    }
    if (g_transportTool.active && !g_play.active && g_transportIn.have) {
        if (g_transportTool.bulldoze && g_transportIn.hover >= 0) { // the building the click would remove, in red
            const Placed& b = g_transport.items[g_transportIn.hover];
            tmp.clear(), tmpEdges.clear();
            Placed ghost = b;
            ghost.state = BState::Done;
            tmodel::BuildPlaced(ghost, time, tmp, &tmpEdges);
            tmodel::Holographic(tmp, holoTris, time, false);
            tmodel::HolographicEdges(tmpEdges, holoLines, time, false);
        } else if (g_transportTool.def >= 0) {
            const Placement& p = g_transportIn.plan;
            const BuildingDef& d = Defs()[g_transportTool.def];
            Placed pv;
            pv.def = g_transportTool.def;
            pv.c = p.c, pv.front = p.front, pv.platformInner = p.platformInner;
            pv.y = p.y != 0.0f ? p.y : RoadsGround(p.c.x, p.c.z);
            pv.state = BState::Done;
            tmp.clear(), tmpEdges.clear();
            tmodel::BuildPlaced(pv, time, tmp, &tmpEdges);
            tmodel::Holographic(tmp, holoTris, time, p.valid);
            tmodel::HolographicEdges(tmpEdges, holoLines, time, p.valid);
            if (g_transportTool.marker) // [snap] green outline: stood beside a road
                MarkerLines(d, p, RoadsGround, holoLines, !p.valid ? tbd::Vec3{1.5f, 0.5f, 0.4f} : p.roadSnapped ? tbd::Vec3{0.45f, 1.4f, 0.6f} : tbd::Vec3{0.5f, 0.9f, 1.4f});
        }
    }
    // SPOM pieces are drawn apart, each with its relief texture bound: track (alpha 0.83, Mesher::Track) and building slabs (kSlabTex
    // alphas, Mesher::Slab); the rest as lit vertex colour.
    constexpr int kSlabs = tbd::city::tmodel::kSlabTexCount;
    static std::vector<tbd::Vertex> split[kSlabs + 2]; // [0] plain, [1] track, [2..] slabs
    for (auto& v : split) v.clear();
    for (size_t k = 0; k + 2 < solid.size(); k += 3) {
        const float a = solid[k].a;
        int dst = std::fabs(a - 0.83f) < 0.004f ? 1 : 0;
        for (int s = 0; s < kSlabs && !dst; ++s)
            if (std::fabs(a - tbd::city::tmodel::kSlabTex[s].alpha) < 0.0009f) dst = 2 + s;
        split[dst].insert(split[dst].end(), solid.begin() + (ptrdiff_t)k, solid.begin() + (ptrdiff_t)k + 3);
    }
    if (!split[0].empty()) r.DrawTransient(split[0].data(), (uint32_t)split[0].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    bool bound = false;
    for (int s = 1; s < kSlabs + 2; ++s) {
        const tbd::TextureId tex = s == 1 ? g_roadGpu.albedo[(int)tbd::city::Mat::Track] : g_roadGpu.slab[s - 2];
        if (split[s].empty() || tex == tbd::kNoTexture) continue;
        r.SetMaterial(tex, tbd::kNoTexture);
        r.DrawTransient(split[s].data(), (uint32_t)split[s].size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        bound = true;
    }
    if (bound) r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    if (!holoTris.empty()) r.DrawTransient(holoTris.data(), (uint32_t)holoTris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!holoLines.empty()) r.DrawTransient(holoLines.data(), (uint32_t)holoLines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), true);
    TransitDraw(r, viewProj); // lines, checkpoints, outside connections, vehicles
}

// The reason a placement is refused, beside the cursor (the same chip the road tool shows), and the construction state.
void TransportWorldLabels() {
    if (!g_transportTool.active || !g_transportIn.have) return;
    auto project = [](tbd::Vec3 p, float& x, float& y) {
        const tbd::Vec4 c = g_lastViewProj * tbd::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * (float)g_engine.vpW;
        y = (0.5f - c.y / c.w * 0.5f) * (float)g_engine.vpH;
        return true;
    };
    float x, y;
    if (g_transportTool.lines) { // the outside connections' names and what they bring
        for (const tbd::city::Outside& o : g_transit.outsides) {
            if (!o.alive || !project({o.p.x, std::max(o.y, RoadsGround(o.p.x, o.p.z)) + 16.0f, o.p.z}, x, y)) continue;
            char t[96];
            std::snprintf(t, sizeof(t), "%s: %s  in %d", o.name.empty() ? "Outside" : o.name.c_str(), tbd::city::OutKindName(o.kind), (int)o.peopleIn);
            g_gui.Chip(x, y, t);
        }
    }
    if (g_transportTool.lines && !g_transportTool.lineBulldoze) {
        const bool out = g_transportTool.outKind >= 0;
        const bool ok = out ? g_transitIn.outValid : g_transitIn.stop.valid;
        const std::string& err = out ? g_transitIn.outErr : g_transitIn.stop.error;
        const tbd::city::V2 q = out ? g_transitIn.outAt : g_transitIn.stop.p;
        if (project({q.x, RoadsGround(q.x, q.z) + 12.0f, q.z}, x, y)) {
            if (!ok && !err.empty()) g_gui.Chip(x, y, err.c_str(), tbd::gui::Rgba(170, 40, 34, 230));
            else if (ok && !out) g_gui.Chip(x, y, g_transitIn.stop.existing >= 0 ? "EXISTING CHECKPOINT" : "CHECKPOINT");
        }
        return;
    }
    if (g_transportTool.def >= 0) {
        const tbd::city::Placement& p = g_transportIn.plan;
        const tbd::Vec3 at{p.c.x, RoadsGround(p.c.x, p.c.z) + 8.0f, p.c.z};
        if (!p.valid && !p.error.empty() && project(at, x, y)) g_gui.Chip(x, y, p.error.c_str(), tbd::gui::Rgba(170, 40, 34, 230));
        else if (p.valid && project(at, x, y)) {
            char cost[32], t[96];
            tbd::gui::transport_ui::Money(cost, sizeof(cost), tbd::city::Defs()[g_transportTool.def].cost);
            std::snprintf(t, sizeof(t), "%s   %s", tbd::city::Defs()[g_transportTool.def].name, cost);
            g_gui.Chip(x, y, t);
        }
    }
    for (const tbd::city::Placed& b : g_transport.items) {
        if (!b.alive || b.state == tbd::city::BState::Done) continue;
        const tbd::Vec3 at{b.c.x, b.y + tbd::city::Defs()[b.def].height + 6.0f, b.c.z};
        if (!project(at, x, y)) continue;
        char t[64];
        if (b.state == tbd::city::BState::Planned) std::snprintf(t, sizeof(t), "PLANNED  %.0f%%", b.timer / tbd::city::kPlanMinutes * 100.0f);
        else std::snprintf(t, sizeof(t), "BUILDING  %.0f%%", b.progress * 100.0f);
        g_gui.Chip(x, y, t);
    }
    for (const tbd::city::YardStatus& st : g_railYards.status) { // a rail yard that runs no trains says why
        const std::string m = st.Message();
        if (m.empty()) continue;
        for (const tbd::city::Placed& b : g_transport.items)
            if (b.alive && b.id == st.yard && project({b.c.x, b.y + tbd::city::Defs()[b.def].height + 6.0f, b.c.z}, x, y))
                g_gui.Chip(x, y, RoadsUpper(m).c_str(), tbd::gui::Rgba(170, 40, 34, 230));
    }
}
