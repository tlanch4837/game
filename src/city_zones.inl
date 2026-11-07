// ------------------------------------------------------------------- city zoning (src/city/zoning.h, gui/zone_toolbar.h)
// Included by main.cpp after the road tool's host code. The zone grid follows the road network; painted cells grow
// buildings (scene entities of the assets\buildings GLBs, at native size) through a construction animation:
//   low density homes   slab, timber wall frame, pitched roof trusses, then the house;
//   everything else     foundation, the building rising storey by storey inside a scaffold, a tower crane slewing
//                       beside it, then the scaffold comes down.
// The game clock (bottom centre: time, pause, speed) drives construction and growth.

tbd::city::ZoneGrid g_zones;
tbd::gui::ZoneToolState g_zoneTool;
tbd::gui::GameClock g_clock;
float g_clockBoost = 1.0f; // dev only (TBD_CLOCK_BOOST, read in city_sim.inl): multiplies the game clock's rate
struct ZoneInput {
    bool held = false;
    bool have = false;   // cursor on the ground
    tbd::Vec3 hit{};
    tbd::city::V2 boxStart{}, boxDir{1, 0};
} g_zoneIn;
// TBD_ZONE_ENTITIES=1: the old path, one scene entity (and mesh upload) per lot. Otherwise city_buildings.inl draws lots per CityCell.
const bool g_zoneEntities = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_ZONE_ENTITIES", v, sizeof(v)) > 0 && v[0] == '1'; }();
std::unordered_map<uint64_t, std::string> g_zoneEntity; // lot id -> its entity's name
std::unordered_map<uint64_t, std::string> g_zoneEntityPath; // lot id -> the model its entity shows (a level change swaps it)
namespace zones_overlay { inline void Sync(tbd::Renderer& r); } // the zone cell overlay mesh (below ZonesDraw)

void ZonesCatalog() {
    static bool done = false;
    if (done) return;
    done = true;
    namespace fs = std::filesystem;
    const auto& types = tbd::city::ZoneTypes();
    for (int t = 0; t < (int)types.size(); ++t) {
        const fs::path dir = fs::path(tbd::ExeDirectory()) / L".." / L"assets" / L"buildings" / types[t].dir / L"glb";
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            const std::string name = f.path().filename().string();
            int w, d, level;
            if (!tbd::city::ParseModelName(name, types[t].prefix, w, d, level) || level > 2) continue;
            g_zones.models.push_back({t, w, d, level, tbd::WideToUtf8(fs::weakly_canonical(f.path(), ec).wstring())});
        }
    }
    tbd::Log("Zoning: %zu building models", g_zones.models.size());
}

int ZoneEntityIndex(uint64_t lot) {
    auto it = g_zoneEntity.find(lot);
    if (it == g_zoneEntity.end()) return -1;
    auto& ents = g_engine.scene.entities;
    for (size_t i = 0; i < ents.size(); ++i)
        if (ents[i].name == it->second) return (int)i;
    return -1;
}

float ZoneModelHeight(const std::string& path) {
    auto it = tbd::ImportedHalfExtents().find(path + "#native");
    return it != tbd::ImportedHalfExtents().end() ? it->second.y * 2.0f : 8.0f;
}

// The building's height scale for a construction progress p (0 = hidden).
float ZoneBuildScale(const tbd::city::Lot& l, float H) {
    const float p = l.progress;
    if (tbd::city::ZoneTypes()[l.type].pitched) {
        if (p < 0.78f) return 0.0f;
        const float k = std::min(1.0f, (p - 0.78f) / 0.14f);
        return 0.55f + 0.45f * k * k * (3 - 2 * k);
    }
    if (p < 0.1f) return 0.0f;
    const float k = std::min(1.0f, (p - 0.1f) / 0.78f);
    const float storey = 3.2f / std::max(H, 3.2f);
    return std::max(storey, std::min(1.0f, std::ceil(k / storey) * storey));
}

void ZonesPlace(tbd::Entity& e, const tbd::city::Lot& l, float sy) {
    e.position = {l.c.x, sy > 0 ? l.y : l.y - 2000.0f, l.c.z}; // hidden below the world until it starts to show
    e.rotation = tbd::QuatAxisAngle({0, 1, 0}, std::atan2(l.front.x, l.front.z));
    e.scale = {1, std::max(sy, 0.02f), 1};
}

// A new lot: the terrain under it is levelled to the road's height at the middle of its frontage, so the building,
// its site and the zone cells sit flat and meet the road.
bool g_zonesNoLevel = false; // TBD_CITY_STRESS: thousands of lots at once skip the terrain levelling (it scans every segment)
void ZonesLevelLot(tbd::city::Lot& l) {
    if (g_zonesNoLevel) return;
    const tbd::city::ZoneCell& front = g_zones.cells[l.cells.front()];
    float target = l.y;
    if (front.seg >= 0 && front.seg < (int)g_roads.net.segs.size() && g_roads.net.segs[front.seg].alive) {
        const tbd::city::V2 frontMid = l.c + l.front * (l.d * tbd::city::kCell * 0.5f);
        float t, d;
        g_roads.net.Nearest(front.seg, frontMid, t, d);
        const tbd::city::V2 rp = g_roads.net.Curve(front.seg).At(t);
        target = RoadsGround(rp.x, rp.z); // the terrain the road sits on there, not the road deck
    }
    if (g_roads.flatten) {
        // Only roads that can matter: a segment counts when its edge is within 5 m (the stamp's largest radius), so a
        // grid over the segments gives the same answer as scanning all of them, at a fraction of the cost.
        static tbd::city::SegGrid roadGrid;
        if (roadGrid.builtFrom != g_roads.net.version) roadGrid.Build(g_roads.net);
        const tbd::city::V2 side{l.front.z, -l.front.x};
        for (int pass = 0; pass < 2; ++pass) // twice: the second settles the first's smoothing
            for (float a = -(l.w * 4.0f) + 1.0f; a <= l.w * 4.0f - 1.0f + 0.01f; a += 2.0f)
                for (float b = -(l.d * 4.0f) + 1.0f; b <= l.d * 4.0f + 1.0f; b += 2.0f) { // to the sidewalk's back edge
                    const tbd::city::V2 q = l.c + side * a + l.front * b;
                    float clear = 1e9f; // distance to the nearest road's edge: the stamp shrinks so it never touches a road bed
                    roadGrid.ForNear(q, 6.0f, [&](int s) {
                        if (!g_roads.net.segs[(size_t)s].alive) return;
                        float t, d;
                        g_roads.net.Nearest(s, q, t, d);
                        clear = std::min(clear, d - g_roads.net.Prof(s).Half());
                    });
                    if (clear < 0.3f) continue;
                    const float radius = std::min(5.0f, clear);
                    g_roads.flatten(q.x, q.z, radius * 0.7f, radius, target);
                }
    }
    l.y = RoadsGround(l.c.x, l.c.z); // seated on the ground as it came out (the brush eases, it may not hit the target exactly)
    g_zones.Reground(RoadsGround, l.c, std::max(l.w, l.d) * tbd::city::kCell + 16.0f); // only the cells the stamps can have moved
}

// Per frame: the clock, growth, the grid following the roads, and the lots' buildings.
void ZonesSync(float realDt) {
    ZonesCatalog();
    if (g_roads.net.version != g_zones.builtFrom) g_zones.Rebuild(g_roads.net, RoadsGround);
    const float minutes = realDt * g_clock.Rate() * g_clockBoost;
    g_clock.minutes += minutes;
    if (minutes > 0) g_zones.Tick(minutes);
    for (uint64_t id : g_zones.removed) { // demolished: the building goes
        const int i = ZoneEntityIndex(id);
        if (i >= 0) {
            g_engine.scene.selected = i;
            DeleteSelected();
            g_engine.scene.selected = -1;
        }
        g_zoneEntity.erase(id);
        g_zoneEntityPath.erase(id);
    }
    g_zones.removed.clear();
    for (const tbd::city::Lot& l : g_zones.lots) {
        if (!l.alive) continue;
        if (const auto pit = g_zoneEntityPath.find(l.id); pit != g_zoneEntityPath.end() && pit->second != l.path) { // levelled: the old building goes
            const int i = ZoneEntityIndex(l.id);
            if (i >= 0) {
                g_engine.scene.selected = i;
                DeleteSelected();
                g_engine.scene.selected = -1;
            }
            g_zoneEntity.erase(l.id);
            g_zoneEntityPath.erase(pit);
        }
        if (!g_zoneEntities) { // city_buildings.inl draws the lot: level its ground once, settle it when built
            static std::unordered_set<uint64_t> levelled; // a lot whose model was swapped keeps the ground it was levelled to
            if (levelled.insert(l.id).second) ZonesLevelLot(const_cast<tbd::city::Lot&>(l));
            if (l.progress >= 1.0f && l.progress < 1.5f) const_cast<tbd::city::Lot&>(l).progress = 2.0f; // settled (Built() stays true)
            continue;
        }
        if (!g_zoneEntity.count(l.id)) {
            static std::unordered_set<uint64_t> levelled; // a lot whose model was swapped keeps the ground it was levelled to
            if (levelled.insert(l.id).second) ZonesLevelLot(const_cast<tbd::city::Lot&>(l));
            tbd::Entity e;
            e.mesh = tbd::MeshKind::Imported;
            e.meshPath = l.path + "#native";
            g_zoneEntityPath[l.id] = l.path;
            if (ImportedVertices(e.meshPath).empty()) {
                g_zoneEntity[l.id] = "";
                continue;
            }
            e.name = "Zoned building " + std::to_string(l.id);
            e.color = {1, 1, 1};
            ZonesPlace(e, l, ZoneBuildScale(l, ZoneModelHeight(l.path)));
            CompleteSceneLoad();
            const int keepSel = g_engine.scene.selected;
            g_engine.scene.Add(e);
            g_engine.scene.selected = keepSel; // growth must not steal the editor's selection
            g_engine.meshes.push_back(UploadEntityMesh(e));
            g_zoneEntity[l.id] = e.name;
            continue;
        }
        if (l.progress >= 1.0f && l.progress < 1.5f) { // finished: one last placement at full size
            const int i = ZoneEntityIndex(l.id);
            if (i >= 0) ZonesPlace(g_engine.scene.entities[i], l, 1.0f);
            const_cast<tbd::city::Lot&>(l).progress = 2.0f; // marks it settled (Built() stays true)
            continue;
        }
        if (l.Built()) continue;
        const int i = ZoneEntityIndex(l.id);
        if (i >= 0) ZonesPlace(g_engine.scene.entities[i], l, ZoneBuildScale(l, ZoneModelHeight(l.path)));
    }
    if (g_engine.renderer) zones_overlay::Sync(*g_engine.renderer); // outside the frame: the upload may not wait
}

void ZonesCursor() {
    tbd::Vec3 hit;
    const bool overGui = g_gui.WantsMouseAt((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY);
    g_zoneIn.have = g_terrainEdit.mouseX >= 0 && !overGui &&
                    GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    if (g_zoneIn.have) g_zoneIn.hit = hit;
    if (g_zoneIn.held && g_zoneIn.have && g_zoneTool.mode == tbd::gui::ZoneToolState::Brush)
        g_zones.Brush({hit.x, hit.z}, g_zoneTool.radius, g_zoneTool.Paint());
}

void ZonesPress() {
    ZonesCursor();
    if (!g_zoneIn.have) return;
    const tbd::city::V2 p{g_zoneIn.hit.x, g_zoneIn.hit.z};
    switch (g_zoneTool.mode) {
    case tbd::gui::ZoneToolState::Brush:
        g_zoneIn.held = true;
        g_zones.Brush(p, g_zoneTool.radius, g_zoneTool.Paint());
        break;
    case tbd::gui::ZoneToolState::Fill: g_zones.Fill(p, g_zoneTool.Paint()); break;
    case tbd::gui::ZoneToolState::Box: {
        g_zoneIn.held = true;
        g_zoneIn.boxStart = p;
        const int c = g_zones.CellAt(p);
        g_zoneIn.boxDir = c >= 0 ? g_zones.cells[c].dir : tbd::city::V2{1, 0}; // the box lines up with the road it starts on
        break;
    }
    }
}

void ZonesRelease() {
    if (!g_zoneIn.held) return;
    g_zoneIn.held = false;
    ZonesCursor();
    if (g_zoneTool.mode == tbd::gui::ZoneToolState::Box)
        g_zones.Box(g_zoneIn.boxStart, {g_zoneIn.hit.x, g_zoneIn.hit.z}, g_zoneIn.boxDir, g_zoneTool.Paint());
}

// ---- drawing
namespace zfx {
using tbd::Vec3;
using tbd::Vertex;
inline void Tri(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 n, Vec3 col) {
    o.push_back({a.x, a.y, a.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
    o.push_back({b.x, b.y, b.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
    o.push_back({c.x, c.y, c.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
    o.push_back({a.x, a.y, a.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f}); // and the other winding: no culling surprises
    o.push_back({c.x, c.y, c.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
    o.push_back({b.x, b.y, b.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
}
inline void Quad(std::vector<Vertex>& o, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, Vec3 col) { Tri(o, a, b, c, n, col), Tri(o, a, c, d, n, col); }
// A square beam from a to b, `t` thick.
inline void Beam(std::vector<Vertex>& o, Vec3 a, Vec3 b, float t, Vec3 col) {
    Vec3 d = b - a;
    const float len = std::sqrt(tbd::Dot(d, d));
    if (len < 1e-4f) return;
    d = d * (1.0f / len);
    const Vec3 ref = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = tbd::Normalize(tbd::Cross(d, ref)) * (t * 0.5f), v = tbd::Normalize(tbd::Cross(d, u)) * (t * 0.5f);
    const Vec3 c[8] = {a - u - v, a + u - v, a + u + v, a - u + v, b - u - v, b + u - v, b + u + v, b - u + v};
    const int f[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {3, 2, 1, 0}, {4, 5, 6, 7}};
    for (const auto& q : f) {
        const Vec3 n = tbd::Normalize(tbd::Cross(c[q[1]] - c[q[0]], c[q[2]] - c[q[0]]));
        Quad(o, c[q[0]], c[q[1]], c[q[2]], c[q[3]], n, col);
    }
}
// An upright box: centre (x, z), half sizes along the lot axes (fx = front, sx = side), from y0 to y1.
inline void Block(std::vector<Vertex>& o, Vec3 c, Vec3 fx, Vec3 sx, float hf, float hs, float y0, float y1, Vec3 col) {
    const Vec3 p[4] = {c + fx * hf + sx * hs, c - fx * hf + sx * hs, c - fx * hf - sx * hs, c + fx * hf - sx * hs};
    for (int i = 0; i < 4; ++i) {
        const Vec3 a = p[i], b = p[(i + 1) % 4], n = tbd::Normalize(((a + b) * 0.5f - c));
        Quad(o, {a.x, y0, a.z}, {b.x, y0, b.z}, {b.x, y1, b.z}, {a.x, y1, a.z}, {n.x, 0, n.z}, col);
    }
    Quad(o, {p[3].x, y1, p[3].z}, {p[2].x, y1, p[2].z}, {p[1].x, y1, p[1].z}, {p[0].x, y1, p[0].z}, {0, 1, 0}, col);
}
} // namespace zfx

// The construction site of one lot (nothing once it is built).
void ZoneSiteFx(const tbd::city::Lot& l, float time, std::vector<tbd::Vertex>& o) {
    using tbd::Vec3;
    const float p = l.progress;
    if (p >= 1.0f) return;
    const std::string key = l.path + "#native";
    auto it = tbd::ImportedHalfExtents().find(key);
    const Vec3 half = it != tbd::ImportedHalfExtents().end() ? it->second : Vec3{l.w * 3.5f, 4, l.d * 3.5f};
    const float H = half.y * 2.0f, y = l.y;
    const Vec3 c{l.c.x, y, l.c.z}, fz{l.front.x, 0, l.front.z}, fxv{l.front.z, 0, -l.front.x}; // building +Z, +X
    const float hx = std::max(1.5f, half.x), hz = std::max(1.5f, half.z);
    const Vec3 concrete{0.62f, 0.62f, 0.6f}, wood{0.74f, 0.56f, 0.34f}, steel{0.55f, 0.57f, 0.6f}, yellow{0.95f, 0.72f, 0.1f}, dirt{0.42f, 0.34f, 0.25f};
    auto at = [&](float x, float z, float h) { return c + fxv * x + fz * z + Vec3{0, h, 0}; };
    // the site: bare ground and a slab
    zfx::Block(o, c, fz, fxv, l.d * 4.0f - 0.4f, l.w * 4.0f - 0.4f, y - 0.2f, y + 0.06f, dirt);
    if (tbd::city::ZoneTypes()[l.type].pitched) {
        const float slab = std::min(1.0f, p / 0.12f);
        zfx::Block(o, c, fz, fxv, hz + 0.2f, hx + 0.2f, y, y + 0.3f * slab, concrete);
        if (p < 0.12f) return;
        const float wallH = std::max(2.6f, H * 0.5f);
        const float frame = std::min(1.0f, (p - 0.12f) / 0.36f); // studs go up around the walls, one after the other
        const float showFrame = p < 0.92f ? 1.0f : 0.0f;
        if (showFrame > 0) {
            std::vector<std::pair<float, float>> studs;
            for (float x = -hx; x <= hx + 0.01f; x += 1.2f) studs.push_back({x, -hz}), studs.push_back({x, hz});
            for (float z = -hz + 1.2f; z < hz - 0.01f; z += 1.2f) studs.push_back({-hx, z}), studs.push_back({hx, z});
            const size_t n = (size_t)(frame * studs.size());
            for (size_t i = 0; i < n; ++i) zfx::Beam(o, at(studs[i].first, studs[i].second, 0.3f), at(studs[i].first, studs[i].second, wallH), 0.14f, wood);
            if (frame >= 1.0f) { // top plates
                zfx::Beam(o, at(-hx, -hz, wallH), at(hx, -hz, wallH), 0.18f, wood);
                zfx::Beam(o, at(-hx, hz, wallH), at(hx, hz, wallH), 0.18f, wood);
                zfx::Beam(o, at(-hx, -hz, wallH), at(-hx, hz, wallH), 0.18f, wood);
                zfx::Beam(o, at(hx, -hz, wallH), at(hx, hz, wallH), 0.18f, wood);
            }
            // pitched roof: trusses marching along the ridge (ridge across the longer side), then the ridge beam
            if (p > 0.48f) {
                const float k = std::min(1.0f, (p - 0.48f) / 0.3f);
                const bool alongX = hx >= hz;
                const float span = alongX ? hz : hx, len = alongX ? hx : hz, rise = std::max(1.6f, span * 0.6f);
                const int count = std::max(2, (int)(2 * len / 1.2f) + 1);
                const int shown = (int)(k * count);
                for (int i = 0; i < shown; ++i) {
                    const float s = -len + 2 * len * i / (count - 1);
                    auto P = [&](float along, float acrossv, float h) { return alongX ? at(along, acrossv, h) : at(acrossv, along, h); };
                    const Vec3 l0 = P(s, -span - 0.3f, wallH - 0.1f), r0 = P(s, span + 0.3f, wallH - 0.1f), top = P(s, 0, wallH + rise);
                    zfx::Beam(o, l0, top, 0.16f, wood);
                    zfx::Beam(o, r0, top, 0.16f, wood);
                    zfx::Beam(o, l0, r0, 0.14f, wood);
                    zfx::Beam(o, P(s, 0, wallH), top, 0.1f, wood);
                }
                if (k >= 1.0f) {
                    auto P = [&](float along, float h) { return alongX ? at(along, 0, h) : at(0, along, h); };
                    zfx::Beam(o, P(-len, wallH + rise), P(len, wallH + rise), 0.2f, wood);
                }
            }
        }
        return;
    }
    // ---- scaffold + tower crane
    const float dig = std::min(1.0f, p / 0.1f);
    zfx::Block(o, c, fz, fxv, hz + 0.3f, hx + 0.3f, y - 0.1f, y + 0.4f * dig, concrete);
    if (p < 0.08f) return;
    const float k = std::min(1.0f, (p - 0.1f) / 0.78f);
    const float built = std::max(0.0f, k) * H;
    float scaffH = std::min(H + 1.0f, built + 2.0f);
    if (p > 0.9f) scaffH *= std::max(0.0f, 1.0f - (p - 0.9f) / 0.08f); // stripped top down
    const float sx = hx + 0.9f, sz = hz + 0.9f;
    if (scaffH > 0.5f) {
        for (float x = -sx; x <= sx + 0.01f; x += 2.5f)
            for (float zz : {-sz, sz}) zfx::Beam(o, at(x, zz, 0), at(x, zz, scaffH), 0.1f, steel);
        for (float zz = -sz + 2.5f; zz < sz - 0.01f; zz += 2.5f)
            for (float xx : {-sx, sx}) zfx::Beam(o, at(xx, zz, 0), at(xx, zz, scaffH), 0.1f, steel);
        for (float h = 2.0f; h <= scaffH; h += 2.0f) {
            zfx::Beam(o, at(-sx, -sz, h), at(sx, -sz, h), 0.08f, yellow);
            zfx::Beam(o, at(-sx, sz, h), at(sx, sz, h), 0.08f, yellow);
            zfx::Beam(o, at(-sx, -sz, h), at(-sx, sz, h), 0.08f, yellow);
            zfx::Beam(o, at(sx, -sz, h), at(sx, sz, h), 0.08f, yellow);
        }
    }
    if (p > 0.96f) return; // the crane leaves with the last of the scaffold
    // crane: a lattice mast at the back corner of the lot, a jib slewing over the building, a hook on the current storey
    const float lotHx = l.w * 4.0f - 1.5f, lotHz = l.d * 4.0f - 1.5f;
    const float mx = std::min(lotHx, hx + 2.6f), mz = -std::min(lotHz, hz + 2.6f) * 0.5f;
    const float mastH = H + 6.0f, jib = std::max(10.0f, std::sqrt(hx * hx + hz * hz) + 4.0f), w = 0.7f;
    for (int i = 0; i < 4; ++i) {
        const float ox = (i & 1) ? w : -w, oz = (i & 2) ? w : -w;
        zfx::Beam(o, at(mx + ox, mz + oz, 0), at(mx + ox, mz + oz, mastH), 0.14f, yellow);
    }
    for (float h = 0; h < mastH - 0.5f; h += 1.5f) {
        zfx::Beam(o, at(mx - w, mz - w, h), at(mx + w, mz - w, h + 1.5f), 0.07f, yellow);
        zfx::Beam(o, at(mx + w, mz + w, h), at(mx - w, mz + w, h + 1.5f), 0.07f, yellow);
        zfx::Beam(o, at(mx - w, mz + w, h + 1.5f), at(mx - w, mz - w, h), 0.07f, yellow);
        zfx::Beam(o, at(mx + w, mz - w, h + 1.5f), at(mx + w, mz + w, h), 0.07f, yellow);
    }
    const float slew = std::sin(time * 0.35f + (float)(l.id % 7)) * 1.2f + 2.4f; // swings back and forth over the building
    const Vec3 top = at(mx, mz, mastH);
    const Vec3 dir = tbd::Normalize(fxv * -std::cos(slew) + fz * std::sin(slew));
    const Vec3 tip = top + dir * jib, tail = top - dir * (jib * 0.3f);
    zfx::Beam(o, top, tip, 0.5f, yellow);
    zfx::Beam(o, top, tail, 0.5f, yellow);
    zfx::Block(o, tail - Vec3{0, 0.6f, 0}, fz, fxv, 0.9f, 0.9f, tail.y - 1.4f, tail.y - 0.2f, concrete); // counterweight
    zfx::Beam(o, top, top + Vec3{0, 3.0f, 0}, 0.4f, yellow);
    zfx::Beam(o, top + Vec3{0, 3.0f, 0}, tip, 0.06f, steel);
    const float trolley = 0.45f + 0.35f * std::sin(time * 0.6f + (float)l.id);
    const Vec3 hookTop = top + dir * (jib * trolley);
    const float hookY = y + built + 1.5f + 1.5f * (0.5f + 0.5f * std::sin(time * 0.9f));
    zfx::Beam(o, hookTop, {hookTop.x, hookY, hookTop.z}, 0.05f, steel);
    zfx::Block(o, {hookTop.x, hookY, hookTop.z}, fz, fxv, 0.6f, 0.6f, hookY - 1.0f, hookY, wood); // the crate it lifts
}

// The zone cell overlay as one static mesh. It used to be rebuilt (125 terrain samples per cell) and streamed through the
// transient buffer every frame: with tens of thousands of painted cells that was ~5 M vertices a frame (the buffer filled
// and dropped draws) and ~0.8 s per rebuild. Now: each cell's 5 x 5 terrain-following grid is sampled once per zone grid
// rebuild and kept; the mesh is remade (indexed, both windings) only when the grid's version changes, at most twice a
// second while the zone tool is closed (growth changes it constantly), and uploaded outside the frame (ZonesSync).
namespace zones_overlay {
inline tbd::MeshId mesh = tbd::kInvalidMesh;
inline std::vector<std::array<tbd::Vec3, 25>> geo;
inline std::vector<uint8_t> geoOk;
inline uint64_t geoFor = ~0ull, builtFor = ~0ull;
inline bool builtTool = false;
inline uint64_t lastBuild = 0;

inline const std::array<tbd::Vec3, 25>& Geo(size_t ci) {
    if (!geoOk[ci]) {
        const tbd::city::ZoneCell& z = g_zones.cells[ci];
        const float h = tbd::city::kCell * 0.5f - 0.35f; // the gaps read as the grid lines
        constexpr int S = 4;                            // 4 x 4 sub-quads following the terrain: no clipping into slopes
        for (int j = 0; j <= S; ++j)
            for (int i = 0; i <= S; ++i) {
                const tbd::city::V2 q = z.c + z.dir * (-h + 2 * h * i / S) + z.out * (-h + 2 * h * j / S);
                float y = RoadsGround(q.x, q.z);
                for (int k = 0; k < 4; ++k) { // the terrain mesh is coarser than the field: lift by its local relief
                    const float o = (k & 1 ? 1.0f : -1.0f) * 1.5f;
                    y = std::max(y, k < 2 ? RoadsGround(q.x + o, q.z) : RoadsGround(q.x, q.z + o));
                }
                geo[ci][(size_t)(j * (S + 1) + i)] = {q.x, y + 0.2f, q.z};
            }
        geoOk[ci] = 1;
    }
    return geo[ci];
}

inline void Sync(tbd::Renderer& r) {
    if (geoFor != g_zones.builtFrom || geo.size() != g_zones.cells.size()) { // new cells: heights sampled again on demand
        geo.assign(g_zones.cells.size(), {});
        geoOk.assign(g_zones.cells.size(), 0);
        geoFor = g_zones.builtFrom;
        builtFor = ~0ull;
    }
    if (builtFor == g_zones.version && builtTool == g_zoneTool.active) return;
    const uint64_t now = tbd::platform::TickCountMs();
    if (!g_zoneTool.active && builtTool == g_zoneTool.active && mesh != tbd::kInvalidMesh && now - lastBuild < 500) return;
    builtFor = g_zones.version, builtTool = g_zoneTool.active, lastBuild = now;
    static std::vector<tbd::Vertex> v;
    static std::vector<uint32_t> idx;
    v.clear();
    idx.clear();
    const tbd::Vec3 n{0, 0, 0}; // unlit: the colour as is
    for (size_t ci = 0; ci < g_zones.cells.size(); ++ci) {
        const tbd::city::ZoneCell& z = g_zones.cells[ci];
        if (z.lot >= 0) continue;
        if (!g_zoneTool.active && z.zone == tbd::city::kNoZone) continue;
        const tbd::Vec3 col = z.zone == tbd::city::kNoZone ? tbd::Vec3{0.62f, 0.64f, 0.62f}
                              : g_zoneTool.active        ? tbd::city::ZoneTypes()[z.zone].color
                                                         : tbd::city::ZoneTypes()[z.zone].color * 0.8f;
        const auto& g = Geo(ci);
        const uint32_t base = (uint32_t)v.size();
        if (!g_zoneTool.active) { // tool closed: one quad on the four (lifted) corners, 4 triangles a cell instead of 64
            for (int k : {0, 4, 24, 20}) v.push_back({g[(size_t)k].x, g[(size_t)k].y, g[(size_t)k].z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
            const uint32_t a = base, b = base + 1, c = base + 2, d = base + 3;
            for (uint32_t t : {a, b, c, a, c, b, a, c, d, a, d, c}) idx.push_back(t);
            continue;
        }
        for (const tbd::Vec3& p : g) v.push_back({p.x, p.y, p.z, n.x, n.y, n.z, col.x, col.y, col.z, 1.0f});
        for (uint32_t j = 0; j < 4; ++j)
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t a = base + j * 5 + i, b = a + 1, c = a + 6, d = a + 5;
                for (uint32_t t : {a, b, c, a, c, b, a, c, d, a, d, c}) idx.push_back(t); // both windings: no culling surprises
            }
    }
    if (mesh != tbd::kInvalidMesh) r.RetireMesh(mesh);
    mesh = v.empty() ? tbd::kInvalidMesh : r.CreateMeshNoWait(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
}
} // namespace zones_overlay

void ZonesDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    if (zones_overlay::mesh != tbd::kInvalidMesh) r.Draw(zones_overlay::mesh, viewProj, tbd::Mat4::Identity(), true, true);
    static std::vector<tbd::Vertex> fx;
    fx.clear();
    const float time = (float)std::fmod(g_clock.minutes, 100000.0) * 0.5f;
    for (const tbd::city::Lot& l : g_zones.lots)
        if (l.alive && l.progress < 1.0f) ZoneSiteFx(l, time, fx);
    if (!fx.empty()) r.DrawTransient(fx.data(), (uint32_t)fx.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!g_zoneTool.active || !g_zoneIn.have) return;
    // the cursor: the brush circle, or the box being dragged
    static std::vector<tbd::Vertex> lines;
    lines.clear();
    const tbd::Vec3 white{1, 1, 1};
    auto line = [&](tbd::city::V2 a, tbd::city::V2 b) {
        const float ya = RoadsGround(a.x, a.z) + 0.4f, yb = RoadsGround(b.x, b.z) + 0.4f;
        lines.push_back({a.x, ya, a.z, 0, 0, 0, white.x, white.y, white.z, 1});
        lines.push_back({b.x, yb, b.z, 0, 0, 0, white.x, white.y, white.z, 1});
    };
    const tbd::city::V2 c{g_zoneIn.hit.x, g_zoneIn.hit.z};
    if (g_zoneTool.mode == tbd::gui::ZoneToolState::Brush) {
        for (int i = 0; i < 48; ++i) {
            const float a0 = i * 6.2831853f / 48, a1 = (i + 1) * 6.2831853f / 48;
            line(c + tbd::city::V2{std::cos(a0), std::sin(a0)} * g_zoneTool.radius, c + tbd::city::V2{std::cos(a1), std::sin(a1)} * g_zoneTool.radius);
        }
    } else if (g_zoneTool.mode == tbd::gui::ZoneToolState::Box && g_zoneIn.held) {
        const tbd::city::V2 a = g_zoneIn.boxStart, d = g_zoneIn.boxDir, n = tbd::city::LeftOf(d), e = c - a;
        const float u = tbd::city::Dot2(e, d), v = tbd::city::Dot2(e, n);
        const tbd::city::V2 p1 = a + d * u, p3 = a + n * v;
        line(a, p1), line(p1, c), line(c, p3), line(p3, a);
    } else {
        const int ci = g_zones.CellAt(c);
        if (ci >= 0) {
            const auto& z = g_zones.cells[ci];
            const float h = tbd::city::kCell * 0.5f;
            const tbd::city::V2 q[4] = {z.c - z.dir * h - z.out * h, z.c + z.dir * h - z.out * h, z.c + z.dir * h + z.out * h, z.c - z.dir * h + z.out * h};
            for (int k = 0; k < 4; ++k) line(q[k], q[(k + 1) % 4]);
        }
    }
    if (!lines.empty()) r.DrawTransient(lines.data(), (uint32_t)lines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
}
