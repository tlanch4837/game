// ------------------------------------------------------------------- utilities (src/city/utilities.h, gui/utility_toolbar.h)
// Included by main.cpp after city_zones.inl (the transport code adds this file's buildings to its zoning blockers).
// Electricity, water and sewage. Every building needs all three and a road; roads that are not highways carry the utilities
// under them, so a house on one is served from the sources on that road network. The sources (power plants, pumps, outlets,
// treatment) are drag-and-drop GLB buildings; where there is no road the player lays a line: an electricity line (wooden
// poles at 0 m, steel pylons above, a buried cable below) or a pipe (buried). The underground view shows what runs
// under the roads. A house that lacks something shows a notification bubble over it: no electricity, no water, no sewage,
// no road connection (10 game days without a road and it is condemned, 5 more and it is demolished).

tbd::city::UtilWorld g_util;
tbd::gui::UtilToolState g_utilTool;
tbd::city::UtilReport g_utilReport;
tbd::city::UtilSolveCache g_utilSolve; // when g_utilReport was last solved (versions of roads, zones, utilities)
struct UtilInput {
    bool have = false; // cursor on the ground
    tbd::Vec3 hit{};
    tbd::city::Placement plan;
    int hover = -1;     // bulldoze: the source under the cursor
    int hoverLine = -1; // bulldoze: the line under the cursor
    bool laying = false; // a line has been started
    tbd::city::V2 start{}, end{};
    bool endSnapped = false;
} g_utilIn;
std::unordered_map<uint64_t, std::string> g_utilEntity; // source id -> its entity's name
std::string g_utilGhost;                                // the entity that follows the cursor while a building is chosen
int g_utilGhostDef = -1;

void TransportRefreshZoning(); // city_transport.inl: sets the zone grid's blockers (it adds ours)
bool PlaceCursorHit(tbd::Vec3& hit); // [interact] city_interact.inl: the ghost's ground point (the touch pin, the dial, else the cursor)
bool BuildingFreePlace();            // [snap] city_interact.inl: the Rotate dial is out, every placement tool's road snap is off

void UtilAppendBlockers(std::vector<tbd::city::BlockRect>& out) {
    const auto b = g_util.Blockers();
    out.insert(out.end(), b.begin(), b.end());
}
size_t UtilBlockerCount() {
    size_t n = 0;
    for (const auto& b : g_util.items) n += b.alive;
    return n;
}

tbd::city::PlaceContext UtilContext() {
    tbd::city::PlaceContext cx;
    cx.net = &g_roads.net;
    cx.zones = &g_zones;
    cx.ground = RoadsGround;
    cx.sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1 ? tbd::IslandMap::kSeaLevel : -1e9f;
    return cx;
}

std::string UtilAbsolute(const char* file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return tbd::WideToUtf8(fs::weakly_canonical(fs::path(tbd::ExeDirectory()) / L".." / fs::path(file), ec).wstring());
}

bool UtilEntityIndex(const std::string& name, int& index) {
    auto& ents = g_engine.scene.entities;
    for (size_t i = 0; i < ents.size(); ++i)
        if (ents[i].name == name) return index = (int)i, true;
    return false;
}
void UtilDeleteEntity(const std::string& name) {
    int i;
    if (name.empty() || !UtilEntityIndex(name, i)) return;
    g_engine.scene.selected = i;
    DeleteSelected();
    g_engine.scene.selected = -1;
}
// A GLB at native size, standing at c on the ground level y facing `front`. False when the file would not load.
bool UtilSpawnEntity(const std::string& name, const char* file, tbd::city::V2 c, float y, tbd::city::V2 front, tbd::Vec3 tint) {
    tbd::Entity e;
    e.mesh = tbd::MeshKind::Imported;
    e.meshPath = UtilAbsolute(file) + "#native";
    if (ImportedVertices(e.meshPath).empty()) return false;
    e.name = name;
    e.color = tint;
    e.position = {c.x, y, c.z};
    e.rotation = tbd::QuatAxisAngle({0, 1, 0}, std::atan2(front.x, front.z));
    e.scale = {1, 1, 1};
    CompleteSceneLoad();
    const int keepSel = g_engine.scene.selected;
    g_engine.scene.Add(e);
    g_engine.scene.selected = keepSel; // placing must not steal the editor's selection
    g_engine.meshes.push_back(UploadEntityMesh(e));
    return true;
}

// Levels the terrain under a new source building's lot, like the zoned lots and the transport buildings do.
void UtilLevel(const tbd::city::UtilBuilding& b) {
    using namespace tbd::city;
    if (!g_roads.flatten) return;
    const UtilDef& D = UtilDefs()[b.def];
    const Frame fr(b.c, b.front);
    const float W = D.W(), H = D.D();
    const float step = W * H > 20000.0f ? 14.0f : 10.0f, hard = step * 0.7f, radius = step * 1.15f;
    for (int pass = 0; pass < 2; ++pass)
        for (float x = -W * 0.5f + step * 0.5f; x < W * 0.5f; x += step)
            for (float z = -H * 0.5f + step * 0.5f; z < H * 0.5f; z += step) {
                const V2 q = fr.World(x, z);
                float t, dist;
                if (g_roads.net.SegmentAt(q, radius, t, dist) >= 0) continue; // keep the brush off every road bed
                g_roads.flatten(q.x, q.z, hard, radius, b.y);
            }
    g_zones.Reground(RoadsGround);
}

// Does a line end at p (of this kind) touch a network: a road, a source, another line's end or a building?
bool UtilTouches(tbd::city::V2 p, tbd::city::Util kind) {
    using namespace tbd::city;
    if (UtilRoadNear(g_roads.net, p, 6.0f) >= 0) return true;
    V2 e;
    if (g_util.SnapEnd(p, kind, 3.0f, e)) return true;
    for (const UtilBuilding& b : g_util.items) {
        const UtilDef& D = UtilDefs()[b.def];
        if (b.alive && D.kind == kind && UtilWorld::RectOf(b.c, b.front, D.w, D.d).Contains(p, 4.0f)) return true;
    }
    for (const Lot& l : g_zones.lots)
        if (l.alive && UtilWorld::RectOf(l.c, l.front, l.w, l.d).Contains(p, 6.0f)) return true;
    return false;
}

// ---- input
void UtilCursor() {
    tbd::Vec3 hit;
    g_utilIn.have = PlaceCursorHit(hit); // [interact] was the cursor's ground hit (not over the GUI); the same for everything but a touch ghost
    g_utilIn.hover = g_utilIn.hoverLine = -1;
    if (!g_utilIn.have) return;
    g_utilIn.hit = hit;
    const tbd::city::V2 p{hit.x, hit.z};
    if (g_utilTool.bulldoze) {
        g_utilIn.hover = g_util.HitTest(p);
        if (g_utilIn.hover < 0) g_utilIn.hoverLine = g_util.LineAt(p, 6.0f, !g_utilTool.underground);
    } else if (g_utilTool.def >= 0) {
        g_utilIn.plan = g_util.Plan(UtilContext(), g_utilTool.def, p, g_utilTool.rotation, g_utilTool.snapRoad && !BuildingFreePlace());
    } else if (g_utilTool.Laying()) {
        tbd::city::V2 e = p;
        g_utilIn.endSnapped = g_util.SnapEnd(p, (tbd::city::Util)g_utilTool.line, 10.0f, e);
        g_utilIn.end = e;
    }
}

void UtilClick() {
    UtilCursor();
    if (!g_utilIn.have) return;
    using namespace tbd::city;
    if (g_utilTool.bulldoze) {
        if (g_utilIn.hover >= 0 && g_util.Remove(g_utilIn.hover)) {
            Toast("BUILDING REMOVED");
            TransportRefreshZoning();
        } else if (g_utilIn.hoverLine >= 0 && g_util.RemoveLine(g_utilIn.hoverLine)) Toast("LINE REMOVED");
        return;
    }
    if (g_utilTool.def >= 0) {
        const Placement& p = g_utilIn.plan;
        if (!p.valid) {
            Toast(RoadsUpper(p.error));
            return;
        }
        const int i = g_util.Place(p, g_utilTool.def);
        if (i < 0) return;
        UtilLevel(g_util.items[i]);
        g_util.items[i].y = RoadsGround(p.c.x, p.c.z);
        TransportRefreshZoning();
        Toast(RoadsUpper(std::string(UtilDefs()[g_utilTool.def].name) + " PLACED"));
        return;
    }
    if (g_utilTool.Laying()) {
        const V2 p = g_utilIn.endSnapped ? g_utilIn.end : V2{g_utilIn.hit.x, g_utilIn.hit.z};
        if (!g_utilIn.laying) {
            g_utilIn.laying = true, g_utilIn.start = p;
            return;
        }
        const int i = g_util.AddLine((Util)g_utilTool.line, g_utilIn.start, p, g_utilTool.LineElevation());
        if (i < 0) return; // too short
        char t[64];
        std::snprintf(t, sizeof(t), "%s LAID  $%d", g_utilTool.line == 0 ? "LINE" : "PIPE", UtilLineCost(g_util.lines[i]));
        Toast(t);
        g_utilIn.start = p; // the next piece continues from here
    }
}

// ---- per frame
// TBD_UTIL_DEMO=<letters> for captures, once the town (TBD_ROADS=demo) and its zone cells exist:
//   z  paint every zone cell low density housing and run the clock fast     p  a wind turbine, a pump and an outlet on the roads
//   w  only a wind turbine (so the houses show no water / sewage)            l  a wooden pole line, a pylon line and a buried pipe
//   u  open the tool in the underground view                                 t  open the tool with the first source chosen
void UtilDemo() {
    static int frames = 0;
    static bool done = false;
    static char flags[32] = {};
    static const bool on = tbd::platform::GetEnvA("TBD_UTIL_DEMO", flags, sizeof(flags)) > 0;
    if (!on || done || ++frames < 40 || g_roads.net.segs.empty() || g_zones.cells.empty()) return;
    done = true;
    using namespace tbd::city;
    auto has = [&](char c) { return std::strchr(flags, c) != nullptr; };
    std::vector<int> carriers;
    for (size_t s = 0; s < g_roads.net.segs.size(); ++s)
        if (CarriesUtilities(g_roads.net, (int)s)) carriers.push_back((int)s);
    tbd::Log("Utility demo: %zu carrier roads", carriers.size());
    if (carriers.empty()) return;
    // a point beside a carrier road: t along it, `off` metres to its left (negative: right)
    auto beside = [&](int seg, float t, float off) {
        const Cubic c = g_roads.net.Curve(seg);
        V2 T = c.Tan(t);
        T = T * (1.0f / std::max(1e-4f, Len(T)));
        return c.At(t) + LeftOf(T) * off;
    };
    auto put = [&](const char* name, int seg, float t, float off) {
        int def = -1;
        for (size_t i = 0; i < UtilDefs().size(); ++i)
            if (std::string(UtilDefs()[i].name) == name) def = (int)i;
        const V2 at = beside(seg, t, off);
        const Placement p = g_util.Plan(UtilContext(), def, at, 0, true);
        const int i = g_util.Place(p, def);
        tbd::Log("Utility demo: %s at (%.0f, %.0f): %s", name, p.c.x, p.c.z, p.valid ? "placed" : p.error.c_str());
        if (i >= 0) UtilLevel(g_util.items[i]), g_util.items[i].y = RoadsGround(p.c.x, p.c.z);
    };
    const int a = carriers.front(), b = carriers.back();
    if (has('p') || has('w')) put("Wind Turbine", a, 0.15f, 30.0f);
    if (has('p')) {
        put("Groundwater Pump", a, 0.85f, 30.0f);
        put("Sewage Outlet", b, 0.5f, -30.0f);
    }
    if (has('l')) {
        const V2 p0 = beside(a, 0.5f, 90.0f);
        tbd::Log("Utility demo: lines start at (%.0f, %.0f)", p0.x, p0.z);
        g_util.AddLine(Util::Electricity, p0, p0 + V2{120, 0}, 0);
        g_util.AddLine(Util::Electricity, p0 + V2{120, 0}, p0 + V2{240, 40}, 0);
        g_util.AddLine(Util::Electricity, p0 + V2{0, 40}, p0 + V2{300, 40}, 20);
        g_util.AddLine(Util::Water, p0 + V2{0, 80}, p0 + V2{200, 80}, -10);
        g_util.AddLine(Util::Sewage, p0 + V2{0, 90}, p0 + V2{200, 90}, -10);
    }
    TransportRefreshZoning();
    if (has('z')) {
        g_zones.maxConstructing = 400;
        g_clock.speed = 2;
        for (size_t i = 0; i < g_zones.cells.size(); ++i) g_zones.Paint((int)i, 0);
    }
    if (has('u') || has('t')) g_roads.Close(), g_utilTool.active = true;
    if (has('u')) g_utilTool.underground = true;
    if (has('t')) g_utilTool.Select(0);
}

void UtilSync(float realDt) {
    UtilDemo();
    const float minutes = realDt * g_clock.Rate();
    if (minutes > 0) g_util.Tick(minutes);
    g_utilSolve.Update(g_util, g_roads.net, g_zones, g_utilReport); // only when roads, zones or utilities changed
    bool changed = false;
    for (uint64_t id : g_util.removed) { // demolished (bulldozed, or 15 days without a road): the building goes
        auto it = g_utilEntity.find(id);
        if (it != g_utilEntity.end()) UtilDeleteEntity(it->second), g_utilEntity.erase(it);
        changed = true;
    }
    g_util.removed.clear();
    for (const tbd::city::UtilBuilding& b : g_util.items) {
        if (!b.alive || g_utilEntity.count(b.id)) continue;
        const tbd::city::UtilDef& D = tbd::city::UtilDefs()[b.def];
        const std::string name = "Utility building " + std::to_string(b.id);
        g_utilEntity[b.id] = UtilSpawnEntity(name, D.file, b.c, b.y, b.front, {1, 1, 1}) ? name : "";
    }
    if (changed) TransportRefreshZoning();

    // the ghost: the chosen building follows the cursor (red when the spot is refused)
    const bool wantGhost = g_utilTool.active && g_utilTool.def >= 0 && g_utilIn.have && !g_play.active;
    if (!wantGhost || g_utilGhostDef != g_utilTool.def) {
        if (!g_utilGhost.empty()) UtilDeleteEntity(g_utilGhost), g_utilGhost.clear();
        g_utilGhostDef = -1;
    }
    if (wantGhost) {
        const tbd::city::Placement& p = g_utilIn.plan;
        const tbd::Vec3 tint = p.valid ? tbd::Vec3{0.65f, 1.0f, 0.8f} : tbd::Vec3{1.0f, 0.42f, 0.38f};
        int i;
        if (g_utilGhost.empty()) {
            const std::string name = "Utility ghost";
            if (UtilSpawnEntity(name, tbd::city::UtilDefs()[g_utilTool.def].file, p.c, p.y, p.front, tint)) g_utilGhost = name, g_utilGhostDef = g_utilTool.def;
        } else if (UtilEntityIndex(g_utilGhost, i)) {
            tbd::Entity& e = g_engine.scene.entities[i];
            e.position = {p.c.x, p.y, p.c.z};
            e.rotation = tbd::QuatAxisAngle({0, 1, 0}, std::atan2(p.front.x, p.front.z));
            e.color = tint;
        }
    }
}

void UtilToolClosed() { // the tool was closed: the ghost goes
    if (!g_utilGhost.empty()) UtilDeleteEntity(g_utilGhost), g_utilGhost.clear();
    g_utilGhostDef = -1;
    g_utilIn.laying = false;
}

// ---- notifications: a bubble over every building that lacks something
namespace ufx {
using tbd::Vec3;
using tbd::Vertex;
inline Vec3 Kind(tbd::city::Util u, bool live) {
    const Vec3 c = u == tbd::city::Util::Electricity ? Vec3{1.0f, 0.82f, 0.25f} : u == tbd::city::Util::Water ? Vec3{0.30f, 0.62f, 1.0f} : Vec3{0.70f, 0.52f, 0.30f};
    return live ? c : c * 0.38f;
}
inline float EaseOutBack(float x) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(x - 1.0f, 3.0f) + c1 * std::pow(x - 1.0f, 2.0f);
}
} // namespace ufx

extern std::vector<uint8_t> g_simFlags; // city_sim.inl: SimFlag bits per lot (sim_flags.h)

void UtilWorldLabels() {
    using namespace tbd::city;
    using tbd::gui::Rgba;
    using tbd::gui::Fade;
    if (g_utilReport.lots.size() != g_zones.lots.size()) return; // the grid changed this frame: the next solve catches up
    auto project = [](tbd::Vec3 p, float& x, float& y) {
        const tbd::Vec4 c = g_lastViewProj * tbd::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * (float)g_engine.vpW;
        y = (0.5f - c.y / c.w * 0.5f) * (float)g_engine.vpH;
        return true;
    };
    struct Item {
        float dist;
        tbd::Vec3 at;
        uint64_t key;
        UtilStatus st;
        float noRoad;    // minutes without a road
        bool condemned;
        float demolishIn; // minutes
        bool lot;
        uint8_t flags; // the sim's other problems: no workers, no customers, being abandoned
    };
    const tbd::Vec3 eye = g_engine.camera.Eye();
    std::vector<Item> items;
    constexpr uint8_t kSimOnly = kFlagNoWorkers | kFlagNoCustomers | kFlagAbandoned;
    auto consider = [&](tbd::Vec3 at, uint64_t key, const UtilStatus& st, float noRoad, bool condemned, float demolishIn, bool lot, uint8_t flags = 0) {
        if (st.Ok() && !(flags & kSimOnly)) return;
        const tbd::Vec3 d = at - eye;
        const float dist = std::sqrt(tbd::Dot(d, d));
        if (dist > 420.0f) return;
        items.push_back({dist, at, key, st, noRoad, condemned, demolishIn, lot, (uint8_t)(flags & kSimOnly)});
    };
    for (size_t i = 0; i < g_zones.lots.size(); ++i) {
        const Lot& l = g_zones.lots[i];
        if (!l.alive || !l.Built()) continue;
        consider({l.c.x, l.y + ZoneModelHeight(l.path) + 5.0f, l.c.z}, l.id, g_utilReport.lots[i], l.noRoadMinutes, l.Condemned(), l.DemolishInMinutes(), true,
                 i < g_simFlags.size() ? g_simFlags[i] : (uint8_t)0);
    }
    for (size_t i = 0; i < g_util.items.size() && i < g_utilReport.buildings.size(); ++i) {
        const UtilBuilding& b = g_util.items[i];
        if (!b.alive) continue;
        consider({b.c.x, b.y + std::min(UtilDefs()[b.def].height, 30.0f) + 5.0f, b.c.z}, (1ull << 62) | b.id, g_utilReport.buildings[i], b.noRoadMinutes, b.Condemned(), b.DemolishInMinutes(), false);
    }
    if (items.empty()) return;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
    if (items.size() > 90) items.resize(90);

    // when each bubble first showed (the pop-up), forgotten once it stops showing
    static std::unordered_map<uint64_t, double> born;
    static std::unordered_map<uint64_t, double> bornNext;
    const double now = (double)tbd::platform::TickCountMs() / 1000.0;
    bornNext.clear();

    tbd::gui::Context& g = g_gui;
    tbd::gui::DrawList& dl = g.Draw();
    const float s = g.Scale();
    int labelled = 0;
    for (const Item& it : items) {
        float x, y;
        if (!project(it.at, x, y)) continue;
        auto b = born.find(it.key);
        const double first = b == born.end() ? now : b->second;
        bornNext[it.key] = first;
        const float k = std::clamp((float)((now - first) / 0.4), 0.0f, 1.0f);
        const float pop = ufx::EaseOutBack(k);
        // fades and shrinks a little with distance
        const float fadeOut = std::clamp(1.0f - (it.dist - 250.0f) / 170.0f, 0.0f, 1.0f);
        const float size = std::round(24.0f * s * (0.75f + 0.25f * std::clamp(1.0f - it.dist / 420.0f, 0.0f, 1.0f)) * pop);
        if (size < 2.0f || fadeOut <= 0.0f) continue;

        struct Badge {
            tbd::gui::Icon icon;
            tbd::gui::Color color;
            char text[72];
        };
        std::vector<Badge> badges;
        if (!it.st.road) {
            Badge bd{tbd::gui::Icon::Roads, Rgba(235, 90, 70), {}};
            const float days = it.condemned ? it.demolishIn / kDayMinutes : (kNoRoadMinutes - it.noRoad) / kDayMinutes;
            if (it.condemned) std::snprintf(bd.text, sizeof(bd.text), "No road: demolished in %.0f day%s", std::ceil(days), std::ceil(days) == 1.0f ? "" : "s");
            else std::snprintf(bd.text, sizeof(bd.text), "No road connection: condemned in %.0f days", std::ceil(days));
            badges.push_back(bd);
        }
        if (it.lot) // buildings that make power / water do not need the networks themselves
            for (int u = 0; u < kUtilKinds; ++u)
                if (!it.st.Has((Util)u)) {
                    Badge bd{tbd::gui::Icon::Bolt, Rgba(235, 90, 70), {}};
                    bd.icon = u == 0 ? tbd::gui::Icon::Bolt : u == 1 ? tbd::gui::Icon::Drop : tbd::gui::Icon::Drain;
                    bd.color = u == 0 ? Rgba(240, 180, 40) : u == 1 ? Rgba(60, 140, 235) : Rgba(160, 120, 70);
                    std::snprintf(bd.text, sizeof(bd.text), "%s", UtilProblem((Util)u));
                    badges.push_back(bd);
                }
        static const struct { uint8_t bit; tbd::gui::Icon icon; int r, g, b; } kSim[] = {
            {kFlagNoWorkers, tbd::gui::Icon::Citizens, 120, 110, 220}, {kFlagNoCustomers, tbd::gui::Icon::Cart, 60, 170, 120},
            {kFlagAbandoned, tbd::gui::Icon::Skull, 90, 90, 96}};
        for (const auto& f : kSim)
            if (it.flags & f.bit) {
                Badge bd{f.icon, Rgba(f.r, f.g, f.b), {}};
                int bit = 0;
                while ((1 << bit) != f.bit) ++bit;
                std::snprintf(bd.text, sizeof(bd.text), "%s", SimFlagText(bit));
                badges.push_back(bd);
            }
        if (badges.empty()) continue;
        // the bubble: one disc per problem in a pill, with a pointer toward the building
        const float gap = std::round(3.0f * s), pad = std::round(3.0f * s);
        const float w = pad * 2 + badges.size() * size + (badges.size() - 1) * gap, h = size + pad * 2;
        const float x0 = std::round(x - w * 0.5f), y1 = std::round(y - 8.0f * s - (1.0f - k) * 14.0f * s), y0 = y1 - h;
        const tbd::gui::Color fill = Fade(Rgba(24, 26, 32, 235), fadeOut), edge = Fade(Rgba(235, 90, 70, 255), fadeOut);
        dl.Shadow(x0, y0, x0 + w, y1, h * 0.5f, 6.0f * s, Fade(Rgba(0, 0, 0, 110), fadeOut));
        dl.Rect(x0, y0, x0 + w, y1, fill, fill, h * 0.5f, std::max(1.0f, 1.5f * s), edge);
        dl.Line(x, y1 + 0.5f, x, y1 + 8.0f * s, std::max(1.0f, 1.5f * s), edge);
        for (size_t i = 0; i < badges.size(); ++i) {
            const float cx = x0 + pad + size * 0.5f + i * (size + gap), cy = y0 + pad + size * 0.5f;
            dl.Circle(cx, cy, size * 0.5f, Fade(badges[i].color, fadeOut));
            tbd::gui::DrawIcon(dl, badges[i].icon, cx, cy, size * 0.58f, std::max(1.2f, 1.8f * s), Fade(Rgba(255, 255, 255), fadeOut));
        }
        if (it.dist < 150.0f && labelled < 10 && k >= 1.0f) { // close by: say what it is
            ++labelled;
            float ty = y0 - 12.0f * s;
            for (size_t i = badges.size(); i-- > 0;) {
                g.Chip(x, ty, badges[i].text, Rgba(120, 36, 30, 225));
                ty -= 22.0f * s;
            }
        }
    }
    born.swap(bornNext);
}

// ---- drawing: poles, pylons, wires, the underground view, the cursor
void UtilWire(std::vector<tbd::Vertex>& o, tbd::Vec3 a, tbd::Vec3 b, float sag, tbd::Vec3 col, float thick) {
    constexpr int N = 6;
    tbd::Vec3 prev = a;
    for (int i = 1; i <= N; ++i) {
        const float t = (float)i / N;
        tbd::Vec3 p = a + (b - a) * t;
        p.y -= sag * 4.0f * t * (1.0f - t);
        zfx::Beam(o, prev, p, thick, col);
        prev = p;
    }
}

// Wooden poles (a line at 0 m): a pole every 36 m with a cross arm and three wires.
void UtilBuildPoles(const tbd::city::UtilLine& l, std::vector<tbd::Vertex>& o) {
    using namespace tbd::city;
    using tbd::Vec3;
    const float L = l.Length();
    const int n = std::max(1, (int)std::ceil(L / kPoleSpacing));
    const V2 dir = (l.b - l.a) * (1.0f / std::max(1e-4f, L)), side{dir.z, -dir.x};
    const Vec3 wood{0.40f, 0.28f, 0.17f}, dark{0.10f, 0.10f, 0.11f}, ins{0.80f, 0.82f, 0.84f};
    std::vector<std::array<Vec3, 3>> attach;
    for (int i = 0; i <= n; ++i) {
        const V2 p = l.a + dir * (L * (float)i / n);
        const float y = RoadsGround(p.x, p.z);
        const Vec3 base{p.x, y, p.z}, top = base + Vec3{0, kPoleHeight, 0};
        zfx::Beam(o, base - Vec3{0, 0.3f, 0}, top, 0.34f, wood);
        const Vec3 arm = base + Vec3{0, kPoleHeight - 0.8f, 0}, off{side.x * 1.3f, 0, side.z * 1.3f};
        zfx::Beam(o, arm - off, arm + off, 0.16f, wood);
        const Vec3 a0 = top + Vec3{0, 0.45f, 0}, a1 = arm - off + Vec3{0, 0.45f, 0}, a2 = arm + off + Vec3{0, 0.45f, 0};
        for (const Vec3& a : {a0, a1, a2}) zfx::Beam(o, a - Vec3{0, 0.3f, 0}, a, 0.12f, ins); // insulators
        attach.push_back({a0, a1, a2});
    }
    const float span = L / n, sag = std::clamp(span * span / 1400.0f, 0.2f, 1.4f);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) UtilWire(o, attach[i][k], attach[i + 1][k], sag, dark, 0.05f);
}

// Steel pylons (a line above ground): lattice towers every 110 m, two cross arms, six wires.
void UtilBuildPylons(const tbd::city::UtilLine& l, std::vector<tbd::Vertex>& o) {
    using namespace tbd::city;
    using tbd::Vec3;
    const float L = l.Length(), H = PylonHeight(l.elev);
    const int n = std::max(1, (int)std::ceil(L / kPylonSpacing));
    const V2 dir = (l.b - l.a) * (1.0f / std::max(1e-4f, L)), side{dir.z, -dir.x};
    const Vec3 steel{0.58f, 0.60f, 0.64f}, dark{0.10f, 0.10f, 0.11f}, ins{0.75f, 0.85f, 0.80f};
    std::vector<std::array<Vec3, 6>> attach;
    auto corner = [&](V2 p, float y, float w, int c) { // c: 0..3 around the tower
        const float a = (c & 1) ? 1.0f : -1.0f, b = (c & 2) ? 1.0f : -1.0f;
        return Vec3{p.x + (side.x * a + dir.x * b) * w, y, p.z + (side.z * a + dir.z * b) * w};
    };
    for (int i = 0; i <= n; ++i) {
        const V2 p = l.a + dir * (L * (float)i / n);
        const float y0 = RoadsGround(p.x, p.z);
        const float w0 = 3.6f, w1 = 0.8f, topY = y0 + H * 0.86f;
        const int panels = 6;
        for (int c = 0; c < 4; ++c) zfx::Beam(o, corner(p, y0, w0, c), corner(p, topY, w1, c), 0.32f, steel);
        for (int k = 0; k <= panels; ++k) {
            const float t = (float)k / panels, y = y0 + (topY - y0) * t, w = w0 + (w1 - w0) * t;
            for (int c = 0; c < 4; ++c) {
                const int nx = (c == 0 ? 1 : c == 1 ? 3 : c == 3 ? 2 : 0);
                if (k > 0) zfx::Beam(o, corner(p, y, w, c), corner(p, y, w, nx), 0.16f, steel);
                if (k < panels) { // diagonal bracing
                    const float t2 = (float)(k + 1) / panels, y2 = y0 + (topY - y0) * t2, w2 = w0 + (w1 - w0) * t2;
                    zfx::Beam(o, corner(p, y, w, c), corner(p, y2, w2, nx), 0.1f, steel);
                }
            }
        }
        const float arms[2] = {0.80f, 0.64f}, reach[2] = {5.2f, 6.4f};
        std::array<Vec3, 6> at;
        for (int a = 0; a < 2; ++a) {
            const float ay = y0 + H * arms[a];
            const Vec3 c{p.x, ay, p.z}, off{side.x * reach[a], 0, side.z * reach[a]};
            zfx::Beam(o, c - off, c + off, 0.3f, steel);
            for (int sgn = 0; sgn < 2; ++sgn) {
                const Vec3 tip = sgn ? c + off : c - off;
                zfx::Beam(o, tip, tip - Vec3{0, 1.6f, 0}, 0.14f, ins); // the insulator string
                at[a * 2 + sgn] = tip - Vec3{0, 1.6f, 0};
            }
        }
        const float ty = y0 + H * 0.93f;
        at[4] = {p.x, ty, p.z};
        at[5] = {p.x, ty, p.z};
        zfx::Beam(o, {p.x, topY, p.z}, {p.x, ty, p.z}, 0.2f, steel);
        attach.push_back(at);
    }
    const float span = L / n, sag = std::clamp(span * span / 2200.0f, 0.8f, 5.0f);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 5; ++k) UtilWire(o, attach[i][k], attach[i + 1][k], sag, dark, 0.09f);
}

// The underground view: a coloured ribbon per network under every utility road, and every line. Dim where no source feeds it.
void UtilBuildUnderground(std::vector<tbd::Vertex>& o) {
    using namespace tbd::city;
    using tbd::Vec3;
    const Network& net = g_roads.net;
    const float off[3] = {-1.7f, 0.0f, 1.7f}; // electricity, water, sewage lanes under the road
    auto ribbon = [&](Vec3 a, Vec3 b, Vec3 na, Vec3 nb, float w, Vec3 col) { // flat, wide: seen from above through the ground
        const tbd::Vec3 n{0, 0, 0};
        zfx::Quad(o, a - na * w, a + na * w, b + nb * w, b - nb * w, n, col);
    };
    for (size_t s = 0; s < net.segs.size(); ++s) {
        if (!CarriesUtilities(net, (int)s)) continue;
        const Cubic c = net.Curve((int)s);
        float len = 0;
        V2 prev = c.p0;
        for (int i = 1; i <= 16; ++i) len += Len(c.At(i / 16.0f) - prev), prev = c.At(i / 16.0f);
        const int steps = std::max(2, (int)(len / 8.0f));
        for (int k = 0; k < 3; ++k) {
            const Util kind = (Util)k;
            const bool live = s < g_utilReport.segLive[k].size() && g_utilReport.segLive[k][s];
            const Vec3 col = ufx::Kind(kind, live);
            Vec3 pa{}, na{};
            for (int i = 0; i <= steps; ++i) {
                const float t = (float)i / steps;
                const V2 p = c.At(t);
                V2 T = c.Tan(t);
                const float tl = Len(T);
                T = tl > 1e-5f ? T * (1.0f / tl) : V2{1, 0};
                const V2 sd = LeftOf(T);
                const float y = net.YAt(net.segs[s], t) - 2.2f;
                const Vec3 q{p.x + sd.x * off[k], y, p.z + sd.z * off[k]}, nq{sd.x, 0, sd.z};
                if (i > 0) ribbon(pa, q, na, nq, 0.4f, col);
                pa = q, na = nq;
            }
        }
    }
    for (size_t i = 0; i < g_util.lines.size(); ++i) { // lines the player laid (all of them: they are under the same view)
        const UtilLine& l = g_util.lines[i];
        if (!l.alive) continue;
        const bool live = i < g_utilReport.lineLive.size() && g_utilReport.lineLive[i];
        const Vec3 col = ufx::Kind(l.kind, live) * 1.15f;
        const V2 d = (l.b - l.a) * (1.0f / std::max(1e-4f, l.Length())), sd{d.z, -d.x};
        const int steps = std::max(1, (int)(l.Length() / 10.0f));
        Vec3 pa{}, na{sd.x, 0, sd.z};
        for (int k = 0; k <= steps; ++k) {
            const V2 p = l.a + (l.b - l.a) * ((float)k / steps);
            const Vec3 q{p.x, RoadsGround(p.x, p.z) + (l.elev < 0 ? l.elev : -1.0f), p.z};
            if (k > 0) ribbon(pa, q, na, na, 0.7f, col);
            pa = q;
        }
    }
}

void UtilDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace tbd::city;
    // poles and pylons: built once per change of the lines
    static std::vector<tbd::Vertex> overhead;
    static uint64_t overheadFor = 0;
    if (overheadFor != g_util.version) {
        overheadFor = g_util.version;
        overhead.clear();
        for (const UtilLine& l : g_util.lines) {
            if (!l.alive) continue;
            if (l.Poles()) UtilBuildPoles(l, overhead);
            else if (l.Pylons()) UtilBuildPylons(l, overhead);
        }
    }
    if (!overhead.empty()) r.DrawTransient(overhead.data(), (uint32_t)overhead.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);

    if (g_utilTool.active && g_utilTool.underground) {
        static std::vector<tbd::Vertex> under;
        static uint64_t kNet = ~0ull, kUtil = ~0ull;
        static size_t kLive = 0;
        size_t live = 0;
        for (int k = 0; k < kUtilKinds; ++k)
            for (uint8_t v : g_utilReport.segLive[k]) live += v;
        for (uint8_t v : g_utilReport.lineLive) live += v;
        if (kNet != g_roads.net.version || kUtil != g_util.version || kLive != live || under.empty()) {
            kNet = g_roads.net.version, kUtil = g_util.version, kLive = live;
            under.clear();
            UtilBuildUnderground(under);
        }
        if (!under.empty()) r.DrawTransient(under.data(), (uint32_t)under.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), false);
    }
    if (!g_utilTool.active) return;

    static std::vector<tbd::Vertex> lines;
    lines.clear();
    auto seg = [&](tbd::Vec3 a, tbd::Vec3 b, tbd::Vec3 col) {
        lines.push_back({a.x, a.y, a.z, 0, 0, 0, col.x, col.y, col.z, 1});
        lines.push_back({b.x, b.y, b.z, 0, 0, 0, col.x, col.y, col.z, 1});
    };
    auto lot = [&](V2 c, V2 front, int w, int d, tbd::Vec3 col) { // the lot's outline and a front arrow
        const Frame fr(c, front);
        const float hw = w * kCell * 0.5f, hd = d * kCell * 0.5f;
        const V2 q[4] = {fr.World(-hw, -hd), fr.World(hw, -hd), fr.World(hw, hd), fr.World(-hw, hd)};
        for (int i = 0; i < 4; ++i) {
            const V2 a = q[i], b = q[(i + 1) % 4];
            seg({a.x, RoadsGround(a.x, a.z) + 0.6f, a.z}, {b.x, RoadsGround(b.x, b.z) + 0.6f, b.z}, col);
        }
        const V2 f0 = fr.World(0, hd), f1 = fr.World(0, hd + 4.0f), f2 = fr.World(-2.0f, hd + 2.0f), f3 = fr.World(2.0f, hd + 2.0f);
        auto P = [&](V2 p) { return tbd::Vec3{p.x, RoadsGround(p.x, p.z) + 0.6f, p.z}; };
        seg(P(f0), P(f1), col), seg(P(f1), P(f2), col), seg(P(f1), P(f3), col);
    };
    const tbd::Vec3 good{0.35f, 1.0f, 0.45f}, bad{1.0f, 0.35f, 0.3f}, white{1, 1, 1};
    if (g_utilTool.marker && g_utilTool.underground) // the sources' lots, in their network's colour
        for (const UtilBuilding& b : g_util.items)
            if (b.alive) lot(b.c, b.front, UtilDefs()[b.def].w, UtilDefs()[b.def].d, ufx::Kind(UtilDefs()[b.def].kind, true));
    if (g_utilIn.have && !g_utilTool.bulldoze && g_utilTool.def >= 0 && g_utilTool.marker) {
        const Placement& p = g_utilIn.plan;
        lot(p.c, p.front, UtilDefs()[g_utilTool.def].w, UtilDefs()[g_utilTool.def].d, p.valid ? good : bad);
    }
    if (g_utilIn.have && g_utilTool.bulldoze) {
        if (g_utilIn.hover >= 0) {
            const UtilBuilding& b = g_util.items[g_utilIn.hover];
            lot(b.c, b.front, UtilDefs()[b.def].w, UtilDefs()[b.def].d, bad);
        } else if (g_utilIn.hoverLine >= 0) {
            const UtilLine& l = g_util.lines[g_utilIn.hoverLine];
            seg({l.a.x, RoadsGround(l.a.x, l.a.z) + 1.0f, l.a.z}, {l.b.x, RoadsGround(l.b.x, l.b.z) + 1.0f, l.b.z}, bad);
        }
    }
    static std::vector<tbd::Vertex> ghost;
    ghost.clear();
    if (g_utilIn.have && g_utilTool.Laying()) { // the line being laid: from the last click to the cursor
        const Util kind = (Util)g_utilTool.line;
        const V2 end = g_utilIn.endSnapped ? g_utilIn.end : V2{g_utilIn.hit.x, g_utilIn.hit.z};
        const bool touches = UtilTouches(end, kind);
        auto ring = [&](V2 c, tbd::Vec3 col) {
            for (int i = 0; i < 16; ++i) {
                const float a0 = i * 6.2831853f / 16, a1 = (i + 1) * 6.2831853f / 16;
                const V2 a = c + V2{std::cos(a0), std::sin(a0)} * 3.0f, b = c + V2{std::cos(a1), std::sin(a1)} * 3.0f;
                seg({a.x, RoadsGround(a.x, a.z) + 0.8f, a.z}, {b.x, RoadsGround(b.x, b.z) + 0.8f, b.z}, col);
            }
        };
        ring(end, touches ? good : tbd::Vec3{1.0f, 0.75f, 0.25f});
        if (g_utilIn.laying) {
            UtilLine pv;
            pv.kind = kind, pv.a = g_utilIn.start, pv.b = end, pv.elev = g_utilTool.LineElevation();
            ring(g_utilIn.start, white);
            if (pv.Length() > 2.0f) {
                if (pv.Poles()) UtilBuildPoles(pv, ghost);
                else if (pv.Pylons()) UtilBuildPylons(pv, ghost);
                else seg({pv.a.x, RoadsGround(pv.a.x, pv.a.z) - 1.0f + pv.elev * 0.0f, pv.a.z}, {pv.b.x, RoadsGround(pv.b.x, pv.b.z) - 1.0f, pv.b.z}, ufx::Kind(kind, true));
                seg({pv.a.x, RoadsGround(pv.a.x, pv.a.z) + 0.8f, pv.a.z}, {pv.b.x, RoadsGround(pv.b.x, pv.b.z) + 0.8f, pv.b.z}, white);
            }
        }
    }
    if (!ghost.empty()) r.DrawTransient(ghost.data(), (uint32_t)ghost.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
    if (!lines.empty()) r.DrawTransient(lines.data(), (uint32_t)lines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
}

// The chip next to the cursor: the building's name and cost, the refusal, or the line's length and cost.
void UtilToolLabels() {
    using namespace tbd::city;
    if (!g_utilTool.active || !g_utilIn.have) return;
    auto project = [](tbd::Vec3 p, float& x, float& y) {
        const tbd::Vec4 c = g_lastViewProj * tbd::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * (float)g_engine.vpW;
        y = (0.5f - c.y / c.w * 0.5f) * (float)g_engine.vpH;
        return true;
    };
    float x, y;
    if (g_utilTool.def >= 0) {
        const Placement& p = g_utilIn.plan;
        const UtilDef& D = UtilDefs()[g_utilTool.def];
        const tbd::Vec3 at{p.c.x, p.y + std::min(D.height, 40.0f) + 8.0f, p.c.z};
        if (!project(at, x, y)) return;
        if (!p.valid) g_gui.Chip(x, y, p.error.c_str(), tbd::gui::Rgba(170, 40, 34, 230));
        else {
            char cost[32], t[96];
            tbd::gui::transport_ui::Money(cost, sizeof(cost), D.cost);
            std::snprintf(t, sizeof(t), "%s   %s", D.name, cost);
            g_gui.Chip(x, y, t);
            // a building with no road along any edge of its (turned) lot is flagged from the start
            if (UtilRoadAccess(g_roads.net, p.c, p.front, D.W(), D.D(), 6.0f) < 0 && project({at.x, at.y + 4.0f, at.z}, x, y))
                g_gui.Chip(x, y - 18.0f * g_gui.Scale(), "No road beside it: it will be demolished after 10 days", tbd::gui::Rgba(150, 100, 20, 230));
        }
    } else if (g_utilTool.Laying() && g_utilIn.laying) {
        const V2 end = g_utilIn.endSnapped ? g_utilIn.end : V2{g_utilIn.hit.x, g_utilIn.hit.z};
        UtilLine pv;
        pv.kind = (Util)g_utilTool.line, pv.a = g_utilIn.start, pv.b = end, pv.elev = g_utilTool.LineElevation();
        if (project({end.x, RoadsGround(end.x, end.z) + 6.0f, end.z}, x, y)) {
            char cost[32], t[128];
            tbd::gui::transport_ui::Money(cost, sizeof(cost), UtilLineCost(pv));
            std::snprintf(t, sizeof(t), "%.0f m   %s   %s", pv.Length(), cost, UtilTouches(end, pv.kind) ? "connected" : "not connected");
            g_gui.Chip(x, y, t);
        }
    } else if (g_utilTool.bulldoze && (g_utilIn.hover >= 0 || g_utilIn.hoverLine >= 0)) {
        const V2 p{g_utilIn.hit.x, g_utilIn.hit.z};
        if (project({p.x, g_utilIn.hit.y + 6.0f, p.z}, x, y)) g_gui.Chip(x, y, "Remove", tbd::gui::Rgba(170, 40, 34, 230));
    }
}
