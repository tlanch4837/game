// ------------------------------------------------------------------- info overlays (sim gameplay pass, phase C)
// Included by main.cpp after city_sim.inl. The toggles and legend are src/gui/city_overlays.h; this file builds the tint.
//
// One static unlit mesh (one draw call), the same way the zone cells and the industry heat map draw:
//   ground   one terrain-following 8 x 8 patch per 128 m sim cell, coloured by the cell's value
//   roofs    power / water / sewage: a flat cap over each built lot's roof, green when served, red when not
//   ribbons  traffic: a strip over each road segment, coloured by the flow solve's class
// The colours are computed at most twice a second while an overlay is on; the mesh is rebuilt (and uploaded, outside the
// frame) only when they changed. Heights are sampled once per cell and kept until the roads change.
//
// Changed from the brief: no RGBA texture + tint shader. That needs a new pipeline in the renderers (Track R's files);
// the vertex-coloured mesh gives the same one-texel-per-cell look with no renderer or shader change. Building dimming is
// not done for the same reason (it would be a hook in city_buildings.inl).

tbd::gui::OverlayHudState g_overlayHud;

namespace sim_overlay {
using tbd::Vertex;
inline tbd::MeshId mesh = tbd::kInvalidMesh;
inline std::vector<uint32_t> sig;  // the colours the mesh was built from (plus the overlay kind)
inline int builtFor = -2;
inline uint64_t lastCheck = 0;
constexpr int kPatch = 8; // quads per cell side (16 m)
inline std::unordered_map<uint64_t, std::array<float, (kPatch + 1) * (kPatch + 1)>> heights;
inline uint64_t heightsFor = ~0ull;

inline uint32_t Pack(tbd::gui::Rgb c) { return ((uint32_t)(c.r * 255) << 16) | ((uint32_t)(c.g * 255) << 8) | (uint32_t)(c.b * 255); }
inline tbd::Vec3 Unpack(uint32_t p) { return {((p >> 16) & 255) / 255.0f, ((p >> 8) & 255) / 255.0f, (p & 255) / 255.0f}; }

inline const std::array<float, (kPatch + 1) * (kPatch + 1)>& CellHeights(tbd::city::CellKey k) {
    const uint64_t key = ((uint64_t)(uint32_t)k.x << 32) | (uint32_t)k.z;
    auto it = heights.find(key);
    if (it != heights.end()) return it->second;
    std::array<float, (kPatch + 1) * (kPatch + 1)> h{};
    const float x0 = k.x * tbd::city::kCityCellSize, z0 = k.z * tbd::city::kCityCellSize, step = tbd::city::kCityCellSize / kPatch;
    for (int j = 0; j <= kPatch; ++j)
        for (int i = 0; i <= kPatch; ++i) {
            const float x = x0 + i * step, z = z0 + j * step;
            float y = RoadsGround(x, z);
            for (int d = 0; d < 4; ++d) y = std::max(y, RoadsGround(x + (d & 1 ? 3.0f : -3.0f), z + (d & 2 ? 3.0f : -3.0f))); // over local relief
            h[(size_t)(j * (kPatch + 1) + i)] = y + 0.35f;
        }
    return heights.emplace(key, h).first->second;
}

// Per cell value in [0, 1] (or < 0: leave the cell out), per lot served flag (1 served, 0 not, -1 skip).
inline void Values(tbd::gui::Overlay o, std::vector<float>& cell, std::vector<int8_t>& lot) {
    using namespace tbd::city;
    using tbd::gui::Overlay;
    cell.assign(g_citySim.sim.size(), -1.0f);
    lot.clear();
    if (o == Overlay::Traffic) return;
    if ((int)o >= (int)Overlay::FirstProvided) { // a planned layer: its phase's per-cell provider (none: nothing)
        if (!tbd::gui::FindOverlayProvider(o)) return;
        for (size_t c = 0; c < g_citySim.sim.size(); ++c) {
            if (g_citySim.cells.cells[c].lots.empty()) continue;
            const CellKey k = g_citySim.cells.cells[c].key;
            cell[c] = tbd::gui::OverlayProvidedValue(o, (int)c, (k.x + 0.5f) * kCityCellSize, (k.z + 0.5f) * kCityCellSize);
        }
        return;
    }
    if (o == Overlay::LandValue || o == Overlay::Coverage) {
        for (size_t c = 0; c < g_citySim.sim.size(); ++c) {
            const CellSim& cs = g_citySim.sim[c];
            if (g_citySim.cells.cells[c].lots.empty()) continue;
            cell[c] = o == Overlay::LandValue ? cs.landValue / 255.0f
                                              : (cs.coverage[kFire] + cs.coverage[kPolice] + cs.coverage[kHealth] + cs.coverage[kSchool]) / (4.0f * 255.0f);
        }
        return;
    }
    const uint8_t bit = o == Overlay::Power ? kLotPower : o == Overlay::Water ? kLotWater : kLotSewage;
    lot.assign(g_zones.lots.size(), -1);
    std::vector<int> served(g_citySim.sim.size(), 0), total(g_citySim.sim.size(), 0);
    for (size_t i = 0; i < g_zones.lots.size(); ++i) {
        const Lot& l = g_zones.lots[i];
        if (!l.alive || !l.Built()) continue;
        const bool ok = i < g_simLotUtil.size() ? (g_simLotUtil[i] & bit) != 0 : true;
        lot[i] = ok ? 1 : 0;
        const int c = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
        if (c >= 0) ++total[(size_t)c], served[(size_t)c] += ok;
    }
    for (size_t c = 0; c < cell.size(); ++c)
        if (total[c]) cell[c] = (float)served[c] / (float)total[c];
}

inline void Quad(std::vector<Vertex>& v, std::vector<uint32_t>& idx, tbd::Vec3 a, tbd::Vec3 b, tbd::Vec3 c, tbd::Vec3 d, tbd::Vec3 col) {
    const uint32_t base = (uint32_t)v.size();
    for (const tbd::Vec3& p : {a, b, c, d}) v.push_back({p.x, p.y, p.z, 0, 0, 0, col.x, col.y, col.z, 1.0f});
    for (uint32_t t : {0u, 1u, 2u, 0u, 2u, 1u, 0u, 2u, 3u, 0u, 3u, 2u}) idx.push_back(base + t); // both windings
}

// Outside the frame (it may create a mesh). Cheap while no overlay is on.
inline void Sync(tbd::Renderer& r) {
    using namespace tbd::city;
    using tbd::gui::Overlay;
    const Overlay o = tbd::gui::OverlayAvailable(g_overlayHud.on) ? g_overlayHud.on : Overlay::None; // no provider: no tint
    if (o == Overlay::None) {
        if (mesh != tbd::kInvalidMesh) r.RetireMesh(mesh), mesh = tbd::kInvalidMesh;
        builtFor = -1;
        return;
    }
    const uint64_t now = tbd::platform::TickCountMs();
    if ((int)o == builtFor && now - lastCheck < 500) return;
    lastCheck = now;
    if (heightsFor != g_roads.net.version) heights.clear(), heightsFor = g_roads.net.version; // roads level the ground
    static std::vector<float> cell;
    static std::vector<int8_t> lot;
    Values(o, cell, lot);
    // the signature: the kind, then every colour that goes in
    static std::vector<uint32_t> next;
    next.clear();
    next.push_back((uint32_t)o);
    for (size_t c = 0; c < cell.size(); ++c)
        if (cell[c] >= 0) next.push_back((uint32_t)c), next.push_back(Pack(tbd::gui::OverlayRamp(o, cell[c])));
    for (size_t i = 0; i < lot.size(); ++i)
        if (lot[i] >= 0) next.push_back((uint32_t)i << 1 | (uint32_t)lot[i]);
    if (o == Overlay::Traffic)
        for (size_t s = 0; s < g_segFlow.size() && s < g_roads.net.segs.size(); ++s) next.push_back((uint32_t)s << 2 | g_segFlow[s].cls);
    next.push_back((uint32_t)g_roads.net.version);
    if ((int)o == builtFor && next == sig) return;
    sig.swap(next);
    builtFor = (int)o;

    static std::vector<Vertex> v;
    static std::vector<uint32_t> idx;
    v.clear();
    idx.clear();
    const float step = kCityCellSize / kPatch;
    for (size_t c = 0; c < cell.size(); ++c) { // ground patches
        if (cell[c] < 0) continue;
        const CellKey k = g_citySim.cells.cells[c].key;
        const auto& h = CellHeights(k);
        const tbd::gui::Rgb rc = tbd::gui::OverlayRamp(o, cell[c]);
        const tbd::Vec3 col{rc.r * 0.85f, rc.g * 0.85f, rc.b * 0.85f}; // the ground a shade darker than the roofs
        const uint32_t base = (uint32_t)v.size();
        const float x0 = k.x * kCityCellSize, z0 = k.z * kCityCellSize;
        for (int j = 0; j <= kPatch; ++j)
            for (int i = 0; i <= kPatch; ++i)
                v.push_back({x0 + i * step, h[(size_t)(j * (kPatch + 1) + i)], z0 + j * step, 0, 0, 0, col.x, col.y, col.z, 1.0f});
        for (uint32_t j = 0; j < (uint32_t)kPatch; ++j)
            for (uint32_t i = 0; i < (uint32_t)kPatch; ++i) {
                const uint32_t a = base + j * (kPatch + 1) + i, b = a + 1, cc = a + kPatch + 2, d = a + kPatch + 1;
                for (uint32_t t : {a, b, cc, a, cc, b, a, cc, d, a, d, cc}) idx.push_back(t);
            }
    }
    for (size_t i = 0; i < lot.size(); ++i) { // roof caps
        if (lot[i] < 0) continue;
        const Lot& l = g_zones.lots[i];
        const tbd::gui::Rgb rc = tbd::gui::OverlayRamp(o, lot[i] ? 1.0f : 0.0f);
        const float y = l.y + ZoneModelHeight(l.path) + 0.6f;
        const V2 side{l.front.z, -l.front.x};
        const float hw = l.w * kCell * 0.45f, hd = l.d * kCell * 0.45f;
        auto P = [&](float a, float b) { const V2 p = l.c + side * a + l.front * b; return tbd::Vec3{p.x, y, p.z}; };
        Quad(v, idx, P(-hw, -hd), P(hw, -hd), P(hw, hd), P(-hw, hd), {rc.r, rc.g, rc.b});
    }
    if (o == Overlay::Traffic) { // ribbons over the roads
        const Network& net = g_roads.net;
        for (size_t s = 0; s < g_segFlow.size() && s < net.segs.size(); ++s) {
            if (!net.segs[s].alive) continue;
            const tbd::gui::Rgb rc = tbd::gui::OverlayRamp(o, g_segFlow[s].cls / 3.0f);
            const Cubic cu = net.Curve((int)s);
            const float half = net.Prof((int)s).Half() * 0.8f;
            const float ya = net.nodes[net.segs[s].a].p.y, yb = net.nodes[net.segs[s].b].p.y;
            constexpr int N = 8;
            for (int k = 0; k < N; ++k) {
                const float t0 = (float)k / N, t1 = (float)(k + 1) / N;
                const V2 p0 = cu.At(t0), p1 = cu.At(t1), n0 = LeftOf(cu.Tan(t0)) * half, n1 = LeftOf(cu.Tan(t1)) * half;
                const float y0 = ya + (yb - ya) * t0 + 0.45f, y1 = ya + (yb - ya) * t1 + 0.45f;
                Quad(v, idx, {p0.x - n0.x, y0, p0.z - n0.z}, {p0.x + n0.x, y0, p0.z + n0.z}, {p1.x + n1.x, y1, p1.z + n1.z}, {p1.x - n1.x, y1, p1.z - n1.z},
                     {rc.r, rc.g, rc.b});
            }
        }
    }
    if (mesh != tbd::kInvalidMesh) r.RetireMesh(mesh);
    mesh = v.empty() ? tbd::kInvalidMesh : r.CreateMeshNoWait(v.data(), (uint32_t)v.size(), idx.data(), (uint32_t)idx.size());
}
} // namespace sim_overlay

// main.cpp: once per frame outside the render frame, after SimSync.
void OverlaysSync() {
    if (g_engine.renderer) sim_overlay::Sync(*g_engine.renderer);
}
// main.cpp: in the scene, after ZonesDraw. One draw call.
void OverlaysDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    if (sim_overlay::mesh != tbd::kInvalidMesh) r.Draw(sim_overlay::mesh, viewProj, tbd::Mat4::Identity(), true, true);
}
// main.cpp: in the HUD, after CityHudFrame: under the right column's five side icons.
void OverlaysHud() { tbd::gui::OverlayHudFrame(g_gui, g_overlayHud, 8.0f + 5 * 60.0f + 10.0f); }

// ---- the remote pipe's "sim" command (main.cpp RemoteRun), for driving captures and checks:
//   sim|stats               population, milestone, treasury, buildings flagged per SimFlag
//   sim|overlay|<-1..5>     switch an info overlay (-1 off)
//   sim|budget|<0|1>        close / open the Budget panel
//   sim|plant|<name>        place that utility building beside the first road that has room
//   sim|boost|<x>           the game clock boost (TBD_CLOCK_BOOST)
//   sim|play|<0..2>         unpause at that speed
//   sim|loan|<tier>         take a loan (0 small, 1 medium, 2 large)
//   sim|herds               neighbourhoods, herds, children and their cost (city_herds.inl)
//   sim|follow[|off]        promote the car nearest the camera target and follow it
//   sim|clock|<hour>        set the game clock to that hour of the current day
int g_simRemoteBudget = -1; // read by SimHudFill's caller: open (1) / close (0) the Budget panel once
bool SimRemote(const std::vector<std::string>& a, std::string& out) {
    using namespace tbd::city;
    const std::string sub = a.size() > 1 ? a[1] : "stats";
    const std::string arg = a.size() > 2 ? a[2] : "";
    if (sub == "stats") {
        int counts[kFlagCount] = {};
        for (uint32_t f : g_simFlags)
            for (int b = 0; b < kFlagCount; ++b) counts[b] += (f >> b) & 1;
        char b[320];
        std::snprintf(b, sizeof(b), "pop %llu milestone %d (%s) treasury %lld lots %zu | nopower %d nowater %d nosewage %d noworkers %d nocustomers %d abandoning %d | loans %zu bankrupt %d",
                      (unsigned long long)g_simTotals.population, g_simMilestones.Level(), g_simMilestones.Current().name, (long long)(g_simEcon.treasury / 100),
                      g_zones.lots.size(), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], g_simBudget.loans.size(), (int)g_simBudget.bankrupt);
        out = b;
        return true;
    }
    if (sub == "overlay") {
        g_overlayHud.on = (tbd::gui::Overlay)std::clamp(atoi(arg.c_str()), -1, (int)tbd::gui::Overlay::Count - 1);
        out = std::string("overlay ") + tbd::gui::OverlayName(g_overlayHud.on);
        return true;
    }
    if (sub == "play") { g_clock.paused = false, g_clock.speed = std::clamp(atoi(arg.c_str()), 0, 2); out = "playing"; return true; }
    if (sub == "loan") { const bool ok = g_simBudget.TakeLoan(atoi(arg.c_str()), g_simEcon); SimBudgetEvents(); out = ok ? "loan taken" : "no loan"; return ok; }
    if (sub == "budget") { g_simRemoteBudget = atoi(arg.c_str()) ? 1 : 0; out = "budget"; return true; }
    if (sub == "boost") { g_clockBoost = (float)std::clamp(atof(arg.c_str()), 0.1, 200.0); out = "boost " + arg; return true; }
    if (sub == "plant") {
        int def = -1;
        for (size_t i = 0; i < UtilDefs().size(); ++i)
            if (arg == UtilDefs()[i].name) def = (int)i;
        if (def < 0) { out = "no utility building named " + arg; return false; }
        for (size_t s = 0; s < g_roads.net.segs.size(); ++s) {
            if (!CarriesUtilities(g_roads.net, (int)s)) continue;
            const Cubic c = g_roads.net.Curve((int)s);
            for (float t = 0.1f; t < 0.95f; t += 0.2f)
                for (float off : {40.0f, -40.0f, 70.0f, -70.0f}) {
                    V2 T = c.Tan(t);
                    T = T * (1.0f / std::max(1e-4f, Len(T)));
                    const Placement p = g_util.Plan(UtilContext(), def, c.At(t) + LeftOf(T) * off, 0, true);
                    if (!p.valid) continue;
                    const int i = g_util.Place(p, def);
                    if (i < 0) continue;
                    UtilLevel(g_util.items[i]);
                    g_util.items[i].y = RoadsGround(p.c.x, p.c.z);
                    TransportRefreshZoning();
                    out = "placed " + arg + " at " + std::to_string((int)p.c.x) + " " + std::to_string((int)p.c.z);
                    return true;
                }
        }
        out = "no room beside any road";
        return false;
    }
    if (sub == "herds" || sub == "follow" || sub == "clock" || sub == "density") return HerdsRemote(sub, arg, out); // city_herds.inl
    out = "sim: stats overlay budget boost plant herds follow clock";
    return false;
}
