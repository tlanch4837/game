// ------------------------------------------------------------------- city render stress scene (Track R, phase R0)
// Included by main.cpp after city_sim.inl. TBD_CITY_STRESS=<n> builds, once the city map is up, a grid of two-lane roads
// north of the default corridor, paints every zone cell with a mix of zone types, spawns lots until there are n (or the
// grid is full) and finishes them at once, so ZonesSync gives each one its building exactly as the game does (one scene
// entity per lot). Plus the deliberately bad player-style cases:
//   a dense mesh   one ~2 M triangle OBJ (generated into obj\stress\dense.obj on first use);
//   a bridge       an elevated two-lane road crossing one of the grid roads at 8 m.
// (A building with many unique materials is not generated yet: it needs distinct textures on disk.)
//
// Then it walks the camera through three views, 12 s each, and logs which one is up; run with TBD_DRAWCATS=1 so the
// draws-by-kind lines land in engine.log between those markers:
//   street   40 m from the grid centre, low
//   mid      600 m, 45 degrees
//   city     the whole grid in view, steep
// TBD_CITY_STRESS_HOLD=<seconds> changes the 12 s; TBD_CITY_STRESS_QUIT=1 closes the engine after the last view.

namespace city_stress {

struct State {
    int target = 0;
    int phase = 0; // 0 waiting, 1 roads built (waiting for the zone grid), 2 lots spawned, 3 views, 4 done
    tbd::city::V2 centre{};
    float size = 0;
    uint64_t phaseStart = 0;
    int view = -1;
    int waitFrames = 0;
    bool grow = false;        // TBD_CITY_GROW=<lots>: a district sized for that many lots, painted, left to grow
    uint64_t growLast = 0, growEnd = 0;
};
inline State g;

inline int EnvInt(const char* name, int fallback) {
    char v[32] = {};
    return tbd::platform::GetEnvA(name, v, sizeof(v)) > 0 ? atoi(v) : fallback;
}

// A dense UV sphere, about 2 M triangles, written once.
inline std::string DenseObj() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(tbd::ExeDirectory()) / L".." / L"obj" / L"stress";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / L"dense.obj";
    if (!fs::exists(file, ec)) {
        FILE* f = tbd::platform::WOpen(file.wstring().c_str(), L"wb");
        if (!f) return "";
        constexpr int kU = 1000, kV = 1000; // 2 M triangles
        const float R = 12.0f;
        for (int v = 0; v <= kV; ++v)
            for (int u = 0; u <= kU; ++u) {
                const float th = 3.14159265f * v / kV, ph = 6.2831853f * u / kU;
                fprintf(f, "v %.3f %.3f %.3f\n", R * std::sin(th) * std::cos(ph), R + R * std::cos(th), R * std::sin(th) * std::sin(ph));
            }
        for (int v = 0; v < kV; ++v)
            for (int u = 0; u < kU; ++u) {
                const int a = v * (kU + 1) + u + 1, b = a + 1, c = a + kU + 1, d = c + 1;
                fprintf(f, "f %d %d %d\nf %d %d %d\n", a, c, b, b, c, d);
            }
        fclose(f);
        tbd::Log("City stress: wrote %ls", file.c_str());
    }
    return tbd::WideToUtf8(fs::weakly_canonical(file, ec).wstring());
}

inline void AddEntity(const std::string& path, tbd::Vec3 at, const char* name) {
    tbd::Entity e;
    e.mesh = tbd::MeshKind::Imported;
    e.meshPath = path + "#native";
    if (ImportedVertices(e.meshPath).empty()) {
        tbd::Log("City stress: could not import %s", path.c_str());
        return;
    }
    e.name = name;
    e.color = {0.8f, 0.8f, 0.8f};
    e.position = at;
    CompleteSceneLoad();
    const int keepSel = g_engine.scene.selected;
    g_engine.scene.Add(e);
    g_engine.scene.selected = keepSel;
    g_engine.meshes.push_back(UploadEntityMesh(e));
}

inline void BuildRoads() {
    using namespace tbd::city;
    RoadsInit();
    Network& net = g_roads.net;
    const int prof = ProfileIndex("Two-Lane Road");
    // Measured: about 7.8 lots per 120 m grid segment (both sides, corners lost). A bx-by-bz grid has about 2*bx*bz segments;
    // the grid is twice as wide (x, along the map) as deep (z, toward the hills), with 10% margin on the lot count.
    constexpr float kStep = 120.0f;
    const int bz = std::clamp((int)std::ceil(std::sqrt((double)g.target * 1.1 / (7.8 * 4.0))), 2, 20), bx = 2 * bz;
    g.size = bx * kStep; // the long side, for the city view
    const float depth = bz * kStep;
    const int seed = g_engine.terrain.params.seed;
    g.centre = {0.0f, tbd::CityMap::CorridorZ(0.0f, seed) - 260.0f - depth * 0.5f}; // north of the corridor (north is -z)
    const float x0 = g.centre.x - g.size * 0.5f, z0 = g.centre.z - depth * 0.5f;
    std::vector<int> id((size_t)(bx + 1) * (bz + 1));
    auto node = [&](int i, int j) { return id[(size_t)j * (bx + 1) + i]; };
    for (int j = 0; j <= bz; ++j)
        for (int i = 0; i <= bx; ++i) {
            const float x = x0 + i * kStep, z = z0 + j * kStep;
            id[(size_t)j * (bx + 1) + i] = net.AddNode({x, RoadsGround(x, z), z}, true);
        }
    auto straight = [&](int a, int b) {
        const V2 pa{net.nodes[a].p.x, net.nodes[a].p.z}, pb{net.nodes[b].p.x, net.nodes[b].p.z};
        net.AddSeg(a, b, pa + (pb - pa) * (1.0f / 3.0f), pa + (pb - pa) * (2.0f / 3.0f), prof);
    };
    for (int j = 0; j <= bz; ++j)
        for (int i = 0; i < bx; ++i) straight(node(i, j), node(i + 1, j));
    for (int i = 0; i <= bx; ++i)
        for (int j = 0; j < bz; ++j) straight(node(i, j), node(i, j + 1));
    // The bridge: an elevated road across the middle of the first grid row's block, 8 m up (nodes not grounded).
    {
        const float x = x0 + kStep * 0.5f, za = z0 - 60.0f, zb = z0 + 60.0f;
        const int a = net.AddNode({x, RoadsGround(x, za) + 8.0f, za}, false), b = net.AddNode({x, RoadsGround(x, zb) + 8.0f, zb}, false);
        straight(a, b);
    }
    ++net.version;
    tbd::Log("City stress: %dx%d road grid (%.0f x %.0f m) at %.0f %.0f, %zu segments", bx, bz, g.size, depth, g.centre.x, g.centre.z, net.segs.size());
}

inline void SpawnLots() {
    using namespace tbd::city;
    const int types = (int)ZoneTypes().size();
    int painted = 0;
    for (size_t i = 0; i < g_zones.cells.size(); ++i) {
        const ZoneCell& c = g_zones.cells[i];
        if (std::fabs(c.c.x - g.centre.x) > g.size * 0.5f + 60.0f || std::fabs(c.c.z - g.centre.z) > g.size * 0.25f + 60.0f) continue;
        const uint64_t h = SimSeed(0x57E55, (uint64_t)c.seg * 4 + (uint64_t)c.side); // one zone per road side
        if (g_zones.Paint((int)i, (uint8_t)(h % (uint64_t)types))) ++painted;
    }
    if (g.grow) { // grow mode: painted only; the sim's demand grows it on the clock
        g_simTune.utilitiesRequired = 0; // the test city has no power plants or pumps: growth is measured without them
        tbd::Log("City stress (grow): utilities not required (no sources are placed)");
        tbd::Log("City stress (grow): painted %d cells; growing at speed %d x%.0f", painted, g_clock.speed, g_clockBoost);
        return;
    }
    const size_t before = g_zones.lots.size();
    int fails = 0;
    const bool gate = g_zones.gateSpawns;
    g_zones.gateSpawns = false; // the stress scene fills every zone, demand or not
    while ((int)(g_zones.lots.size() - before) < g.target && fails < 400) {
        if (g_zones.SpawnNow()) fails = 0;
        else ++fails;
    }
    g_zones.gateSpawns = gate;
    for (Lot& l : g_zones.lots) l.progress = std::max(l.progress, 1.0f);
    tbd::Log("City stress: painted %d cells, %zu lots (target %d)", painted, g_zones.lots.size() - before, g.target);
    g_zonesNoLevel = true;
    const std::string dense = DenseObj();
    if (!dense.empty()) AddEntity(dense, {g.centre.x + 30.0f, RoadsGround(g.centre.x + 30.0f, g.centre.z + 30.0f), g.centre.z + 30.0f}, "Stress dense mesh");
}

inline void SetView(int v) {
    static const char* const kName[] = {"street", "mid", "city"};
    auto& cam = g_engine.camera;
    cam.target = {g.centre.x + 20.0f, RoadsGround(g.centre.x + 20.0f, g.centre.z + 20.0f), g.centre.z + 20.0f};
    cam.yaw = 0.6f;
    if (v == 0) { cam.distance = 40.0f; cam.pitch = 0.25f; }
    if (v == 1) { cam.distance = 600.0f; cam.pitch = 0.785f; }
    if (v == 2) { cam.distance = std::max(1200.0f, g.size * 1.25f); cam.pitch = 1.1f; }
    tbd::Log("City stress view: %s (distance %.0f, pitch %.2f), %zu entities", kName[v], cam.distance, cam.pitch, g_engine.scene.entities.size());
    tbd::Log("City stress sim: pop %llu, working %llu, jobs %llu, cells %zu; sim avg %.3f ms/frame, backlog %.1f", (unsigned long long)g_simTotals.population,
             (unsigned long long)g_simTotals.working, (unsigned long long)g_simTotals.jobs, g_citySim.sim.size(), g_sim.AvgFrameMs(), g_sim.Backlog());
    for (const tbd::city::SimSystem& s : g_sim.Systems())
        tbd::Log("City stress sim:   %-10s avg %.3f ms/frame, worst call %.3f ms, %llu calls", s.name.c_str(), s.avgMs, s.worstMs, (unsigned long long)s.calls);
    tbd::Log("City stress sim:   flow solve %.1f ms on a worker, %zu trips", g_simFlowSolveMs, g_simFlowJob->trips.size());
}

} // namespace city_stress

void CityStressSync() {
    using namespace city_stress;
    if (g.phase == 4) return;
    if (g.phase == 0) {
        g.target = EnvInt("TBD_CITY_STRESS", 0);
        if (g.target <= 0 && EnvInt("TBD_SIM_STRESS", 0) > 0) { // a population: about 16 people per lot came out of the grow runs
            g.target = std::max(50, EnvInt("TBD_SIM_STRESS", 0) / 16);
            g_clock.speed = 2;
            static bool said = false;
            if (!said) said = true, tbd::Log("City stress: TBD_SIM_STRESS=%d -> %d lots", EnvInt("TBD_SIM_STRESS", 0), g.target);
        }
        if (g.target <= 0 && EnvInt("TBD_CITY_GROW", 0) > 0) g.target = EnvInt("TBD_CITY_GROW", 0), g.grow = true;
        if (g.target <= 0) { g.phase = 4; return; }
        // The city map must be up: terrain on and the default corridor built. The engine opens on a plain scene, so the
        // stress run starts a new city map itself, on a fixed seed so every run measures the same ground.
        if (SceneLoadActive()) return;
        if (!g_engine.terrain.params.enabled || g_roads.net.segs.empty()) {
            if (g.waitFrames++ == 5) NewCityGame(EnvInt("TBD_CITY_STRESS_SEED", 4242));
            return;
        }
        if (++g.waitFrames < 60) return;
        BuildRoads();
        g.phase = 1;
        return;
    }
    if (g.phase == 1) { // ZonesSync rebuilt the grid from the new roads at the start of this frame
        if (g_zones.builtFrom != g_roads.net.version || g_zones.models.empty()) return;
        SpawnLots();
        g.phase = g.grow ? 5 : 2;
        g.waitFrames = 0;
        if (g.grow) {
            g_clock.speed = 2;
            g_clock.paused = false;
            g.growLast = tbd::platform::TickCountMs();
            g.growEnd = g.growLast + (uint64_t)EnvInt("TBD_CITY_GROW_SECONDS", 300) * 1000ull;
            SetView(std::clamp(EnvInt("TBD_CITY_GROW_VIEW", 1), 0, 2));
        }
        return;
    }
    if (g.phase == 5) { // grow mode: a status line every 15 s, then quit (or stay) at the end
        const uint64_t now = tbd::platform::TickCountMs();
        if (now - g.growLast >= 15000) {
            g.growLast = now;
            int lots = 0, building = 0, l2 = 0;
            for (const tbd::city::Lot& l : g_zones.lots) {
                if (!l.alive) continue;
                ++lots;
                building += l.Built() ? 0 : 1;
                l2 += l.level >= 2;
            }
            int slow = 0;
            float top = 0;
            for (const tbd::city::SegFlow& f : g_segFlow) slow += f.cls >= 2, top = std::max(top, f.flow);
            tbd::Log("City grow: flow %zu trips, solve %.1f ms on a worker, %d slow segments, busiest %.0f veh/h; money %lld, net %lld / day",
                     g_simFlowJob->trips.size(), g_simFlowSolveMs, slow, top, (long long)(g_simEcon.treasury / 100), (long long)(g_simEcon.LastNet() / 100));
            tbd::Log("City grow: day %d, lots %d (building %d, L2 %d), pop %llu working %llu jobs %llu, demand R %+.2f C %+.2f O %+.2f I %+.2f, "
                     "ups %llu downs %llu abandoned %llu, sim avg %.3f ms worst-call cells %.3f growth %.3f, backlog %.1f, dropped %lld",
                     g_clock.Day(), lots, building, l2, (unsigned long long)g_simTotals.population, (unsigned long long)g_simTotals.working,
                     (unsigned long long)g_simTotals.jobs, g_simGrowth.demand[0], g_simGrowth.demand[1], g_simGrowth.demand[2], g_simGrowth.demand[3],
                     (unsigned long long)g_simGrowth.levelUps, (unsigned long long)g_simGrowth.levelDowns, (unsigned long long)g_simGrowth.abandoned,
                     g_sim.AvgFrameMs(), g_sim.Systems().size() > 0 ? g_sim.Systems()[0].worstMs : 0.0, g_sim.Systems().size() > 2 ? g_sim.Systems()[2].worstMs : 0.0,
                     g_sim.Backlog(), (long long)g_sim.DroppedMinutes());
        }
        // TBD_CITY_GROW_SAVETEST=1: two thirds in, save to slot 10, load it back, compare (S8).
        static int saveStep = 0;
        static uint64_t saveAt = 0;
        static uint64_t popBefore = 0, lotsBefore = 0;
        static int64_t moneyBefore = 0;
        static size_t peopleBefore = 0;
        auto liveLots = [] {
            uint64_t n = 0;
            for (const tbd::city::Lot& l : g_zones.lots) n += l.alive;
            return n;
        };
        static const uint64_t growStart = now;
        if (EnvInt("TBD_CITY_GROW_SAVETEST", 0) && saveStep == 0 && now >= growStart + (g.growEnd - growStart) * 2 / 3) {
            g_clock.paused = true; // a still city: the numbers before and after can be compared exactly
            for (const tbd::city::Lot& l : g_zones.lots) { // a few tracked people
                int cell;
                uint32_t first, count;
                if (g_simPeople.people.size() < 3 && l.alive && tbd::city::SimPeople::LotSlots(g_citySim, l.id, cell, first, count))
                    for (uint32_t k = 0; k < std::min<uint32_t>(count, 3); ++k) g_simPeople.Materialize(g_citySim, cell, first + k, g_sim.Minute());
            }
            popBefore = g_citySim.Population(), lotsBefore = liveLots(), moneyBefore = g_simEcon.treasury, peopleBefore = g_simPeople.people.size();
            tbd::Log("City save test: before: %llu lots, pop %llu, money %lld cents, %zu tracked", (unsigned long long)lotsBefore, (unsigned long long)popBefore,
                     (long long)moneyBefore, peopleBefore);
            SaveCityGame(tbd::saves::kScratchSlot); // [title] scratch.*, not the player's Save 10
            LoadCityGame(tbd::saves::kScratchSlot);
            saveStep = 1;
            saveAt = now;
        } else if (saveStep == 1 && now - saveAt > 8000) {
            tbd::Log("City save test: after: %llu lots, pop %llu, money %lld cents, %zu tracked (%s)", (unsigned long long)liveLots(),
                     (unsigned long long)g_citySim.Population(), (long long)g_simEcon.treasury, g_simPeople.people.size(),
                     liveLots() == lotsBefore && g_citySim.Population() == popBefore && g_simEcon.treasury == moneyBefore && g_simPeople.people.size() == peopleBefore
                         ? "MATCH" : "DIFFERENT");
            saveStep = 2;
            g_clock.paused = false;
        }
        if (now >= g.growEnd && saveStep != 1) {
            tbd::Log("City grow: done");
            g.phase = 4;
            if (EnvInt("TBD_CITY_STRESS_QUIT", 0)) tbd::shell::Quit();
        }
        return;
    }
    if (g.phase == 2) { // the next ZonesSync makes the entities (slow, one frame); give the uploads a few frames
        if (++g.waitFrames < 10) return;
        g.phase = 3;
        g.view = 0;
        SetView(0);
        g.phaseStart = tbd::platform::TickCountMs();
        return;
    }
    static const uint64_t hold = (uint64_t)EnvInt("TBD_CITY_STRESS_HOLD", 12) * 1000ull;
    if (tbd::platform::TickCountMs() - g.phaseStart < hold) return;
    if (++g.view < 3) {
        SetView(g.view);
        g.phaseStart = tbd::platform::TickCountMs();
        return;
    }
    tbd::Log("City stress: views done");
    g.phase = 4;
    if (EnvInt("TBD_CITY_STRESS_QUIT", 0)) tbd::shell::Quit();
}
