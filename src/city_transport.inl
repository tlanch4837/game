// ------------------------------------------------------------------- transport buildings (src/city/transport.h, transport_models.h,
// gui/transport_toolbar.h). Included by main.cpp after city_zones.inl. The Transport button opens a picker (Rail, Ships, Air);
// the chosen building follows the cursor as a blue hologram (red with the reason when the spot is refused: shipyards and
// harbours only ever fit on a qualifying coast). A click places it: it stays a hologram while "Planned", then a construction
// site rises, then it stands. Railway tracks are laid with the road tool (the Rail tab hands over to it).

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
std::unordered_map<uint64_t, TransportCache> g_transportCache; // finished buildings: their world-space geometry, made once

tbd::city::PlaceContext TransportContext() {
    tbd::city::PlaceContext cx;
    cx.net = &g_roads.net;
    cx.zones = &g_zones;
    cx.ground = RoadsGround;
    cx.sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind == 1 ? tbd::IslandMap::kSeaLevel : -1e9f; // only the island has a sea
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
    const bool overGui = g_gui.WantsMouseAt((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY);
    g_transportIn.have = g_terrainEdit.mouseX >= 0 && !overGui &&
                         GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    g_transportIn.hover = -1;
    if (!g_transportIn.have) return;
    g_transportIn.hit = hit;
    const tbd::city::V2 p{hit.x, hit.z};
    if (g_transportTool.bulldoze) g_transportIn.hover = g_transport.HitTest(p);
    else if (g_transportTool.def >= 0) g_transportIn.plan = g_transport.Plan(TransportContext(), g_transportTool.def, p, g_transportTool.rotation, g_transportTool.Options());
}

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
        if (GetEnvironmentVariableA("TBD_TRANSPORT", v, sizeof(v)) > 0) {
            g_transportTool.active = true;
            const int d = std::clamp(atoi(v), 0, (int)tbd::city::Defs().size() - 1);
            g_transportTool.Select(d);
        }
    }
    static bool demoDone = false;
    if (!demoDone && g_engine.terrain.params.enabled) { // TBD_TRANSPORT_DEMO="def x z progress;...": placed buildings for captures (progress < 0 planned, 0..1 building, 1 done)
        demoDone = true;
        char v[512] = {};
        if (GetEnvironmentVariableA("TBD_TRANSPORT_DEMO", v, sizeof(v)) > 0) {
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
    for (uint64_t id : g_transport.removed) g_transportCache.erase(id);
    g_transport.removed.clear();
    if (g_zones.blockers.size() != (size_t)g_transport.Count() + UtilBlockerCount()) TransportRefreshZoning(); // e.g. after a load
}

// The buildings, the hologram of the one being placed, and the lot markers.
void TransportDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace tbd::city;
    static std::vector<tbd::Vertex> solid, holoTris, holoLines, tmp, tmpEdges;
    solid.clear(), holoTris.clear(), holoLines.clear();
    const float time = (float)std::fmod((double)GetTickCount64() * 0.001, 10000.0);
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
            if (g_transportTool.marker) MarkerLines(d, p, RoadsGround, holoLines, p.valid ? tbd::Vec3{0.5f, 0.9f, 1.4f} : tbd::Vec3{1.5f, 0.5f, 0.4f});
        }
    }
    // SPOM track (vertex alpha 0.83, Mesher::Track) is drawn apart, with the track texture bound; the rest as lit vertex colour
    static std::vector<tbd::Vertex> track, rest;
    track.clear(), rest.clear();
    for (size_t k = 0; k + 2 < solid.size(); k += 3) {
        std::vector<tbd::Vertex>& dst = std::fabs(solid[k].a - 0.83f) < 0.004f ? track : rest;
        dst.insert(dst.end(), solid.begin() + (ptrdiff_t)k, solid.begin() + (ptrdiff_t)k + 3);
    }
    if (!rest.empty()) r.DrawTransient(rest.data(), (uint32_t)rest.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!track.empty() && g_roadGpu.albedo[(int)tbd::city::Mat::Track] != tbd::kNoTexture) {
        r.SetMaterial(g_roadGpu.albedo[(int)tbd::city::Mat::Track], tbd::kNoTexture);
        r.DrawTransient(track.data(), (uint32_t)track.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    }
    if (!holoTris.empty()) r.DrawTransient(holoTris.data(), (uint32_t)holoTris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!holoLines.empty()) r.DrawTransient(holoLines.data(), (uint32_t)holoLines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), true);
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
}
