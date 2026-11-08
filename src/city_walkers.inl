// ------------------------------------------------------------------- city pedestrians on the sidewalks [walkers]
// (src/city/sim_walkers.h). Included by game_core.inl after city_buildings.inl, before city_interact.inl (the info card reads
// the walkers). People step out of homes, shops, workplaces, schools and stations near the camera, walk the sidewalks,
// cross at the junction crosswalks, wait at bus stops and go in at another door, following the hour and the sim's counters.
//
// Drawing: CPU skinning does not scale to hundreds of people, so the walk cycle is baked once at load into static meshes (no
// skinning at run time, no new asset files): each of 6 looks (human_1, 2, 4, 5, 9, 11.glb: clothes and skin colours) is posed
// with skeletal_anim.h's own skinning at 8 phases of anim_walk.glb plus one anim_idle_happy.glb pose, and decimated with
// mesh_lod.h into three levels. Every walker is one 3x4 instance row in the bucket (level, look, frame) it shows; each bucket
// is one instanced draw:
//   level 0  < 35 m from the eye   1500 triangles (the file's mesh)  8 walk frames + idle
//   level 1  < 90 m                ~375 triangles                    4 walk frames + idle
//   level 2  < 220 m               ~190 triangles                    2 walk frames + idle; none past 220 m
// The frame is the stride phase (metres walked / the clip's measured stride), so feet do not slide whatever the walking
// speed. Near people (< 60 m) cast sun shadows: the level-1 frames of one look (the shapes are the same), 5 draws at most.
// TBD_WALKERS=0 turns them off; TBD_WALKERS_MAX=<n> sets the cap (default 320).
// src/city/sim_walkers.h is included by the translation units (main.cpp, game_host.cpp, probe_game.cpp) before game_core.inl,
// which they open inside an anonymous namespace.

namespace walkers_host {
constexpr int kLooks = 6;
constexpr int kLookFile[kLooks] = {1, 2, 4, 5, 9, 11};
constexpr int kLevels = 3;
constexpr int kFrames[kLevels] = {8, 4, 2};
constexpr int kSlots = 9;                       // per (level, look): up to 8 walk frames, then the idle pose
constexpr float kLevelRange[kLevels] = {35.0f, 90.0f, 220.0f};
constexpr float kShadowRange = 60.0f;
struct Level {
    tbd::MeshId mesh[kLooks][kSlots];
    uint32_t tris = 0;
};
Level levels[kLevels];
bool tried = false, ready = false;
float stride = 1.8f;                            // metres per walk cycle at scale 1 (measured from the clip at bake)
tbd::city::WalkerSim sim;
double clock = 0;                               // walk-clock seconds (real time while the game runs)
uint64_t netVersion = ~0ull, zonesVersion = ~0ull;
size_t buildingsSeen = 0, stopsSeen = 0;
float doorsTimer = 0;
std::vector<tbd::city::WalkDoor> doorScratch;
std::vector<std::string> doorLabels;            // per sim.doors entry: what the info card calls it
std::vector<float> rows[kLevels * kLooks * kSlots];
std::vector<float> shadowRows[5];
uint32_t drawn = 0, draws = 0;
bool On() {
    static const bool on = [] { char v[8] = {}; return !(tbd::platform::GetEnvA("TBD_WALKERS", v, sizeof(v)) > 0 && v[0] == '0'); }();
    return on;
}

// Welds a flat triangle list (vertex bytes equal: one vertex): unique vertices, a welded index per corner, and per welded
// vertex the first corner that made it.
struct Welded {
    std::vector<tbd::Vertex> verts;
    std::vector<uint32_t> index, first;
};
inline Welded Weld(const std::vector<tbd::Vertex>& flat) {
    Welded w;
    std::unordered_map<std::string, uint32_t> seen;
    seen.reserve(flat.size());
    for (size_t i = 0; i < flat.size(); ++i) {
        const std::string key(reinterpret_cast<const char*>(&flat[i]), sizeof(tbd::Vertex));
        const auto it = seen.try_emplace(key, (uint32_t)w.verts.size());
        if (it.second) w.verts.push_back(flat[i]), w.first.push_back((uint32_t)i);
        w.index.push_back(it.first->second);
    }
    return w;
}

// The walk clip's stride: the planted foot slides back under the hips at the walking speed, so its speed in the clip
// (the lowest vertex while it is down) times the cycle's length is the ground one cycle covers.
inline float MeasureStride(const std::string& clip, const tbd::ImportedModel& model, const std::vector<tbd::Vertex>& flat, float dur) {
    size_t foot = 0;
    float lowest = 1e30f;
    for (size_t i = 0; i < flat.size(); ++i)
        if (flat[i].px > 0.03f && flat[i].py < lowest) lowest = flat[i].py, foot = i;
    constexpr int kN = 48;
    std::vector<tbd::Vertex> posed;
    float y[kN + 1], z[kN + 1], ground = 1e30f;
    for (int k = 0; k <= kN; ++k) {
        if (!g_engine.animator.BakePose(clip, model, flat, dur * (float)k / kN, "mixamorig:Hips", posed)) return 0.0f;
        y[k] = posed[foot].py, z[k] = posed[foot].pz;
        ground = std::min(ground, y[k]);
    }
    std::vector<float> speeds;
    const float dt = dur / kN;
    for (int k = 0; k < kN; ++k)
        if (y[k] < ground + 0.03f && y[k + 1] < ground + 0.03f) speeds.push_back((z[k] - z[k + 1]) / dt);
    if (speeds.size() < 4) return 0.0f;
    std::sort(speeds.begin(), speeds.end());
    return std::fabs(speeds[speeds.size() / 2]) * dur;
}

// Bakes the poses into the static meshes (outside the frame: the uploads wait). False when the files are missing.
inline bool Bake() {
    const std::string walk = UtilAbsolute("assets/characters/anim_walk.glb"), idle = UtilAbsolute("assets/characters/anim_idle_happy.glb");
    const float dur = g_engine.animator.ClipDuration(walk), idleDur = g_engine.animator.ClipDuration(idle);
    if (dur <= 0.0f) { tbd::Log("Walkers: no walk clip (%s): none drawn", walk.c_str()); return false; }
    std::vector<tbd::Vertex> posed[kSlots], tri;
    for (int l = 0; l < kLooks; ++l) {
        const std::string path = UtilAbsolute(("assets/characters/human_" + std::to_string(kLookFile[l]) + ".glb").c_str()) + "#native";
        const std::vector<tbd::Vertex>& flat = ImportedVertices(path);
        if (flat.empty()) { tbd::Log("Walkers: cannot read %s: none drawn", path.c_str()); return false; }
        const tbd::ImportedModel& model = ImportedModels()[path];
        for (int f = 0; f < 8; ++f)
            if (!g_engine.animator.BakePose(walk, model, flat, dur * (float)f / 8.0f, "mixamorig:Hips", posed[f])) { tbd::Log("Walkers: %s has no usable skin", path.c_str()); return false; }
        if (!g_engine.animator.BakePose(idle, model, flat, std::min(0.6f, idleDur * 0.3f), "mixamorig:Hips", posed[8])) posed[8] = flat;
        if (l == 0) {
            const float s = MeasureStride(walk, model, flat, dur);
            stride = s > 0.8f && s < 3.2f ? s : 1.4f * dur;
            tbd::Log("Walkers: walk cycle %.2f s, stride %.2f m (%s), %zu triangles a body", dur, stride, s > 0.8f && s < 3.2f ? "measured" : "fallback", flat.size() / 3);
        }
        // the levels: decimated in the bind pose, the same collapses applied to every pose (vertices are copies of the welded ones)
        const Welded w = Weld(flat);
        const std::vector<tbd::LodLevel> chain = tbd::BuildLodChain(w.verts, w.index, 0.5f, 0.9f);
        std::unordered_map<std::string, uint32_t> byBytes;
        for (size_t i = 0; i < w.verts.size(); ++i) byBytes.emplace(std::string(reinterpret_cast<const char*>(&w.verts[i]), sizeof(tbd::Vertex)), (uint32_t)i);
        const tbd::LodLevel* pick[kLevels] = {nullptr, nullptr, nullptr};
        for (const tbd::LodLevel& lv : chain) {
            const size_t t = lv.indices.size() / 3;
            if (!pick[1] && t <= 450) pick[1] = &lv;
            if (!pick[2] && t <= 220) pick[2] = &lv;
        }
        if (!pick[1]) pick[1] = chain.empty() ? nullptr : &chain.front();
        if (!pick[2]) pick[2] = pick[1];
        for (int lv = 0; lv < kLevels; ++lv) {
            std::vector<uint32_t> corner; // flat corner (of the file's list) for each corner of this level
            if (lv == 0 || !pick[lv]) {
                corner.resize(flat.size());
                for (size_t i = 0; i < flat.size(); ++i) corner[i] = (uint32_t)i;
            } else {
                for (uint32_t i : pick[lv]->indices) {
                    const tbd::Vertex& v = pick[lv]->vertices[i];
                    const auto it = byBytes.find(std::string(reinterpret_cast<const char*>(&v), sizeof(tbd::Vertex)));
                    corner.push_back(it == byBytes.end() ? 0u : w.first[it->second]);
                }
            }
            levels[lv].tris = (uint32_t)(corner.size() / 3);
            for (int f = 0; f < kSlots; ++f) {
                const bool isIdle = f == kSlots - 1;
                if (!isIdle && f >= kFrames[lv]) { levels[lv].mesh[l][f] = tbd::kInvalidMesh; continue; }
                const std::vector<tbd::Vertex>& src = posed[isIdle ? 8 : f * (8 / kFrames[lv])];
                tri.clear();
                for (uint32_t c : corner) tri.push_back(src[c]);
                levels[lv].mesh[l][f] = g_engine.renderer->CreateMesh(tri.data(), (uint32_t)tri.size(), tbd::Topology::Triangles);
            }
        }
    }
    tbd::Log("Walkers: baked %d looks x (%d + %d + %d frames + idle): %u / %u / %u triangles", kLooks, kFrames[0], kFrames[1], kFrames[2], levels[0].tris, levels[1].tris, levels[2].tris);
    for (auto& r : rows) r.reserve(12 * 32);
    return true;
}

// Every door the city has now: lots (their street face), transport and service buildings, bus and tram stops.
inline void BuildDoors() {
    using namespace tbd::city;
    doorScratch.clear(), doorLabels.clear();
    g_roadNames.Update(g_roads.net);
    auto street = [](int seg) -> std::string {
        if (seg < 0) return "";
        if (const RoadChain* ch = g_roadNames.ChainOf(seg)) return ch->name.Full();
        return "";
    };
    const auto& types = ZoneTypes();
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        const LotCap cap = LotCapacity(l);
        float occ = 1.0f; // how full the sim says the cell is
        const int ci = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
        const ZoneCat cat = types[(size_t)l.type].cat;
        if (ci >= 0 && (size_t)ci < g_citySim.sim.size()) {
            const CellSim& cs = g_citySim.sim[(size_t)ci];
            if (cat == ZoneCat::Residential) occ = cs.homeCap ? std::min(1.0f, (float)cs.Population() / (float)cs.homeCap) : 0.0f;
            else {
                const float capJobs = (float)cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2], filled = (float)cs.employed[0] + cs.employed[1] + cs.employed[2];
                occ = capJobs > 0 ? std::min(1.0f, filled / capJobs) : 0.0f;
            }
        }
        WalkDoor d;
        d.id = l.id * 8 + 1;
        d.out = l.front;
        d.p = l.c + l.front * ((float)l.d * kCell * 0.5f);
        d.y = RoadsGround(d.p.x, d.p.z);
        d.kind = cat == ZoneCat::Residential ? DoorKind::Home : cat == ZoneCat::Commercial ? DoorKind::Shop : DoorKind::Work;
        d.people = (cat == ZoneCat::Residential ? (float)cap.homes : (float)cap.Jobs()) * occ;
        if (d.people <= 0 && d.kind != DoorKind::Home && d.kind != DoorKind::Shop) continue;
        doorScratch.push_back(d);
        const std::string road = street(l.frontSeg);
        doorLabels.push_back(std::string(types[(size_t)l.type].name) + (road.empty() ? "" : ", " + road));
    }
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& def = Defs()[(size_t)b.def];
        WalkDoor d;
        d.id = b.id * 8 + 2;
        d.out = b.front;
        d.p = b.c + b.front * (def.D() * 0.5f);
        d.y = RoadsGround(d.p.x, d.p.z);
        const bool passengers = def.model == Model::TrainStation || def.model == Model::PassengerHarbor || def.model == Model::SmallAirfield || def.model == Model::RegionalAirport;
        d.kind = def.tab == TTab::Education ? DoorKind::School : passengers ? DoorKind::Station : IsServiceTab(def.tab) ? DoorKind::Service : DoorKind::Work;
        d.people = (float)def.workplaces * (d.kind == DoorKind::School ? 8.0f : d.kind == DoorKind::Station ? 4.0f : 1.0f);
        doorScratch.push_back(d);
        doorLabels.push_back(def.name);
    }
    for (const Stop& s : g_transit.stops) {
        if (!s.alive || s.outside >= 0 || s.kind != TKind::Passenger || (s.mode != TMode::Bus && s.mode != TMode::Tram)) continue;
        WalkDoor d;
        d.id = s.id * 8 + 3;
        d.out = RightOf(s.dir) * -1.0f;
        d.p = s.p + RightOf(s.dir) * 5.0f; // in front of the shelter (TransitStopModels stands it 6 m off the centre line)
        d.y = RoadsGround(d.p.x, d.p.z);
        d.kind = DoorKind::Stop;
        doorScratch.push_back(d);
        doorLabels.push_back(std::string(TModeName(s.mode)) + " stop");
    }
    sim.SetDoors(doorScratch);
}

inline const std::string& DoorLabel(uint64_t id) {
    static const std::string gone = "a building that is gone";
    const int i = sim.DoorIndex(id);
    return i >= 0 && (size_t)i < doorLabels.size() ? doorLabels[(size_t)i] : gone;
}
inline float DrawRange() { return kLevelRange[kLevels - 1]; }
} // namespace walkers_host

// The info card for a tapped walker (city_interact.inl InfoCardFill, PickKind::Person sub 4): who, why, from where to where.
void WalkerInfo(tbd::gui::InfoCardData& d, const tbd::city::Walker& w) {
    using namespace tbd::city;
    namespace wh = walkers_host;
    Person p;
    p.seed = w.id;
    d.icon = "ppl", d.accent = tbd::gui::Rgba(94, 234, 212);
    d.title = SimPeople::Name(p);
    d.sub = WalkPurposeName(w.purpose);
    const char* doing = w.moving ? (w.legKind == WalkLegKind::Crossing ? "crossing the road" : w.legKind == WalkLegKind::Door ? (w.cur == 0 ? "stepping out" : "going in") : "walking")
                                 : w.legKind == WalkLegKind::Crossing ? "waiting to cross" : w.legKind == WalkLegKind::Stop ? "waiting at the stop"
                                 : w.cur == 0 ? "stepping out" : "going in";
    d.rows.push_back({"Doing", doing});
    d.rows.push_back({"From", wh::DoorLabel(w.fromId)});
    if (w.via >= 0) d.rows.push_back({"Via", wh::DoorLabel(w.viaId)});
    d.rows.push_back({"To", wh::DoorLabel(w.toId)});
    char t[64];
    std::snprintf(t, sizeof(t), "%.0f of %.0f m", std::min(w.walked, w.length), w.length);
    d.rows.push_back({"Walk", t});
    std::snprintf(t, sizeof(t), "%.1f km/h", w.speed * 3.6f);
    d.rows.push_back({"Pace", t});
    std::snprintf(t, sizeof(t), "body %d of %d (human_%d)", w.look % wh::kLooks + 1, wh::kLooks, wh::kLookFile[w.look % wh::kLooks]);
    d.rows.push_back({"Look", t});
}

// Per frame, outside the scene pass (next to HerdsSync): the clock, the graph and doors when the city changed, the walkers.
void WalkersSync(float realDt) {
    namespace wh = walkers_host;
    if (!wh::On() || !g_engine.renderer || g_play.active) return;
    if (!g_clock.paused) wh::clock += (double)realDt * (1.0 + 0.5 * g_clock.speed);
    if (!wh::tried) {
        if (g_roads.net.segs.empty() || g_zones.lots.empty()) return; // nothing to walk yet: the files load with the first street
        wh::tried = true;
        char v[16] = {};
        if (tbd::platform::GetEnvA("TBD_WALKERS_MAX", v, sizeof(v)) > 0) wh::sim.params.maxWalkers = std::clamp(atoi(v), 0, 2000);
        wh::ready = wh::Bake();
    }
    if (!wh::ready) return;
    if (g_roads.net.version != wh::netVersion) {
        wh::netVersion = g_roads.net.version;
        wh::sim.RebuildGraph(g_roads.net);
        wh::doorsTimer = 0;
    }
    wh::doorsTimer -= realDt;
    const size_t buildings = g_transport.items.size(), stops = g_transit.stops.size();
    if (g_zones.version != wh::zonesVersion || buildings != wh::buildingsSeen || stops != wh::stopsSeen || wh::doorsTimer <= 0) {
        wh::zonesVersion = g_zones.version, wh::buildingsSeen = buildings, wh::stopsSeen = stops;
        wh::doorsTimer = 10.0f; // the occupancy moves slowly: a fresh list every ten seconds is plenty
        wh::BuildDoors();
    }
    tbd::city::WalkView view;
    view.viewProj = g_lastViewProj;
    view.eye = g_engine.camera.Eye();
    view.range = wh::DrawRange();
    view.valid = true;
    const float h = view.eye.y - RoadsGround(view.eye.x, view.eye.z), reach = view.range + 70.0f;
    const float radius = h < reach ? std::sqrt(reach * reach - std::max(0.0f, h) * std::max(0.0f, h)) : 0.0f;
    const float hour = (float)std::fmod(g_clock.minutes / 60.0, 24.0);
    wh::sim.Update(wh::clock, hour, {view.eye.x, view.eye.z}, radius, view);
    static float logT = 0;
    if ((logT -= realDt) <= 0) {
        logT = 10.0f;
        size_t homes = 0, people = 0;
        for (const tbd::city::WalkDoor& d : wh::sim.doors) homes += d.kind == tbd::city::DoorKind::Home, people += (size_t)d.people;
        tbd::Log("Walkers: %zu out (%u drawn in %u draws), %zu doors (%zu homes, %zu people), %.1f h, keep %.2f, disk %.0f m, eye %.0f m up; %llu planned, %llu hidden on arrival",
                 wh::sim.walkers.size(), wh::drawn, wh::draws, wh::sim.doors.size(), homes, people, hour, wh::sim.keep, radius, h,
                 (unsigned long long)wh::sim.planned, (unsigned long long)wh::sim.suppressed);
    }
}

// The scene pass (next to HerdsDraw): one instanced draw per (level, look, frame) bucket in use.
void WalkersDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    namespace wh = walkers_host;
    wh::drawn = wh::draws = 0;
    for (auto& v : wh::shadowRows) v.clear();
    if (!wh::ready || g_play.active || wh::sim.walkers.empty()) return;
    for (auto& v : wh::rows) v.clear();
    tbd::city::WalkView view;
    view.viewProj = viewProj;
    view.eye = g_engine.camera.Eye();
    view.range = wh::DrawRange();
    view.valid = true;
    for (const tbd::city::Walker& w : wh::sim.walkers) {
        const tbd::Vec3 d{w.pos.x - view.eye.x, w.y - view.eye.y, w.pos.z - view.eye.z};
        const float dist = std::sqrt(tbd::Dot(d, d));
        if (dist > view.range || !view.InView(w.pos, w.y, 0.15f)) continue;
        const int lv = dist < wh::kLevelRange[0] ? 0 : dist < wh::kLevelRange[1] ? 1 : 2;
        const float phase = w.walked / (wh::stride * w.scale);
        const float frac = phase - std::floor(phase);
        const int nf = wh::kFrames[lv];
        const int slot = w.moving ? std::min(nf - 1, (int)(frac * (float)nf)) : wh::kSlots - 1;
        const float s = w.dir.x * w.scale, k = w.dir.z * w.scale, sc = w.scale;
        const float row[12] = {k, 0, s, w.pos.x, 0, sc, 0, w.y, -s, 0, k, w.pos.z};
        std::vector<float>& dst = wh::rows[(size_t)((lv * wh::kLooks + w.look % wh::kLooks) * wh::kSlots + slot)];
        dst.insert(dst.end(), row, row + 12);
        if (dist < wh::kShadowRange) {
            const int sf = w.moving ? std::min(3, (int)(frac * 4.0f)) : 4;
            wh::shadowRows[sf].insert(wh::shadowRows[sf].end(), row, row + 12);
        }
        ++wh::drawn;
    }
    r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    for (int lv = 0; lv < wh::kLevels; ++lv)
        for (int l = 0; l < wh::kLooks; ++l)
            for (int f = 0; f < wh::kSlots; ++f) {
                const std::vector<float>& v = wh::rows[(size_t)((lv * wh::kLooks + l) * wh::kSlots + f)];
                const tbd::MeshId mesh = wh::levels[lv].mesh[l][f];
                if (v.empty() || mesh == tbd::kInvalidMesh) continue;
                r.DrawInstancedTransient(mesh, v.data(), (uint32_t)(v.size() / 12), viewProj);
                ++wh::draws;
            }
}

// The sun's shadow pass (next to SimAgentsCasters): last frame's near walkers, the level-1 shapes of one look.
void WalkersCasters(tbd::Renderer& r) {
    namespace wh = walkers_host;
    if (!wh::ready || g_play.active) return;
    for (int f = 0; f < 5; ++f) {
        const std::vector<float>& v = wh::shadowRows[f];
        const tbd::MeshId mesh = wh::levels[1].mesh[0][f == 4 ? wh::kSlots - 1 : f];
        if (v.empty() || mesh == tbd::kInvalidMesh) continue;
        r.DrawShadowInstancedTransient(mesh, v.data(), (uint32_t)(v.size() / 12));
    }
}
