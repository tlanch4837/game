// ------------------------------------------------------------------- city sim: flow traffic (S5) and cosmetic cars (S7)
// Included by city_sim.inl (moved out of it, CS2 sim roadmap Phase 0; owned by Phase 5 from here on).

// ---- S5: flow traffic. The graph snapshot and the trips go to a background worker; the result swaps in at a tick.
struct SimFlowJob {
    std::shared_ptr<const tbd::city::FlowGraph> graph;
    std::vector<tbd::city::Trip> trips;
    tbd::city::FlowSolver solver; // only the worker touches it while busy (warm start: the last flows)
    std::vector<tbd::city::SegFlow> result;
    size_t segCount = 0;
    std::atomic<bool> busy{false}, ready{false};
};
std::shared_ptr<SimFlowJob> g_simFlowJob = std::make_shared<SimFlowJob>();
std::vector<tbd::city::SegFlow> g_segFlow; // per road segment, the last finished solve (empty before the first)
uint64_t g_segFlowVersion = 0;               // network version g_segFlow belongs to
tbd::city::SegGrid g_simSegGrid;
double g_simFlowSolveMs = 0;
int64_t g_simFlowLastMinute = -1000000;

// Road edits: the cells the changed segments cross are dirty (render and flow).
void SimDirtyRoadCells() {
    static std::vector<uint8_t> alive;
    const auto& net = g_roads.net;
    std::vector<tbd::city::CellKey> keys;
    for (size_t s = 0; s < net.segs.size(); ++s) {
        const uint8_t now = net.segs[s].alive ? 1 : 0;
        const uint8_t was = s < alive.size() ? alive[s] : 0;
        if (now == was) continue;
        tbd::city::SegGrid::CellsOf(net, (int)s, keys);
        for (const tbd::city::CellKey& k : keys) {
            const int c = g_citySim.cells.Find(k);
            if (c >= 0) g_citySim.cells.Touch(c, tbd::city::kDirtyRender | tbd::city::kDirtyFlow);
        }
    }
    alive.resize(net.segs.size());
    for (size_t s = 0; s < net.segs.size(); ++s) alive[s] = net.segs[s].alive ? 1 : 0;
}

void SimAppendTownTrips(const tbd::city::FlowGraph& g, std::vector<tbd::city::Trip>& trips); // city_herds.inl (town traffic)

// The trips of the peak hour: commutes from the supercell links, shopping from homes to commercial (gravity).
void SimBuildTrips(const tbd::city::FlowGraph& g, std::vector<tbd::city::Trip>& trips) {
    using namespace tbd::city;
    trips.clear();
    const int nsc = g_citySim.Supercells();
    // a supercell's road node: the network node nearest its people and jobs
    std::vector<int> node((size_t)nsc, -1);
    std::vector<V2> centre((size_t)nsc);
    std::vector<double> pop((size_t)nsc, 0), comm((size_t)nsc, 0);
    for (int s = 0; s < nsc; ++s) {
        double w = 0, x = 0, z = 0;
        for (int c : g_citySim.SuperCellList(s)) {
            const CellSim& cs = g_citySim.sim[(size_t)c];
            const double cw = (double)cs.Population() + cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
            const CellKey k = g_citySim.cells.cells[(size_t)c].key;
            x += cw * (k.x + 0.5) * kCityCellSize, z += cw * (k.z + 0.5) * kCityCellSize, w += cw;
            pop[(size_t)s] += cs.Population();
            comm[(size_t)s] += cs.jobCap[kJobCommercial];
        }
        if (w <= 0) continue;
        centre[(size_t)s] = {(float)(x / w), (float)(z / w)};
        float t, d;
        const int seg = g_simSegGrid.Nearest(g_roads.net, centre[(size_t)s], 800.0f, t, d);
        if (seg < 0) continue;
        const Segment& sg = g_roads.net.segs[(size_t)seg];
        node[(size_t)s] = t < 0.5f ? sg.a : sg.b;
        if (node[(size_t)s] >= g.nodes) node[(size_t)s] = -1;
    }
    const float commute = (float)(g_simTune.flowCarShare * g_simTune.flowPeakShare);
    g_citySim.ForEachLink([&](int h, int j, int32_t n) {
        if (h == j || node[(size_t)h] < 0 || node[(size_t)j] < 0 || node[(size_t)h] == node[(size_t)j]) return;
        trips.push_back({node[(size_t)h], node[(size_t)j], (float)n * commute, (int)(SimSeed(7, (uint64_t)h * 131 + (uint64_t)j) % 3)});
    });
    for (int h = 0; h < nsc; ++h) {
        if (node[(size_t)h] < 0 || pop[(size_t)h] <= 0) continue;
        double wsum = 0;
        std::vector<std::pair<int, double>> w;
        for (int c = 0; c < nsc; ++c) {
            if (c == h || node[(size_t)c] < 0 || comm[(size_t)c] <= 0) continue;
            const double d = Len(centre[(size_t)c] - centre[(size_t)h]);
            if (d > 3.0 * g_simTune.flowShopDecayMetres) continue;
            const double v = comm[(size_t)c] * std::exp(-d / std::max(1.0, g_simTune.flowShopDecayMetres));
            w.push_back({c, v});
            wsum += v;
        }
        const double total = pop[(size_t)h] * g_simTune.flowShopPerResident;
        for (const auto& [c, v] : w)
            if (node[(size_t)c] != node[(size_t)h]) trips.push_back({node[(size_t)h], node[(size_t)c], (float)(total * v / wsum), 2});
    }
    SimAppendTownTrips(g, trips); // trips inside one hood and from the outside connections (city_herds.inl, sim_town_traffic.h)
}

void SimFlow(const tbd::city::SimTick& t) {
    using namespace tbd::city;
    SimFlowJob& job = *g_simFlowJob;
    if (job.ready.load(std::memory_order_acquire)) { // a finished solve: swap it in at this tick
        g_segFlow = job.result;
        g_segFlowVersion = job.graph ? job.graph->version : 0;
        g_simFlowSolveMs = job.solver.lastMs;
        job.ready.store(false, std::memory_order_release);
    }
    if (g_simSegGrid.builtFrom != g_roads.net.version) {
        g_simSegGrid.Build(g_roads.net);
        SimDirtyRoadCells();
    }
    if (job.busy.load(std::memory_order_acquire)) return;
    static uint64_t kNet = ~0ull, kZones = ~0ull;
    const bool changed = kNet != g_roads.net.version || kZones != g_zones.version;
    if (!changed && t.minute - g_simFlowLastMinute < (int64_t)g_simTune.flowSolveMinutes) return;
    kNet = g_roads.net.version, kZones = g_zones.version;
    g_simFlowLastMinute = t.minute;
    if (!job.graph || job.graph->version != g_roads.net.version) job.graph = std::make_shared<const FlowGraph>(FlowGraph::Build(g_roads.net));
    SimBuildTrips(*job.graph, job.trips);
    if (job.trips.empty()) {
        g_segFlow.assign(g_roads.net.segs.size(), SegFlow{});
        g_segFlowVersion = g_roads.net.version;
        return;
    }
    job.segCount = g_roads.net.segs.size();
    job.busy.store(true, std::memory_order_release);
    std::shared_ptr<SimFlowJob> keep = g_simFlowJob; // the worker holds it even if the city is reset meanwhile
    g_engine.jobs->RunBackground([keep] {
        keep->solver.Solve(*keep->graph, keep->trips);
        keep->solver.Segments(*keep->graph, keep->segCount, keep->result);
        keep->ready.store(true, std::memory_order_release);
        keep->busy.store(false, std::memory_order_release);
    });
}

// ---- S7: cosmetic traffic. Eight car models (assets/vehicles/cars), uploaded once when there is traffic to show; every
// frame the cars near the camera come from the segment flows (sim_agents.h) and each model is one instanced draw.
struct SimCarModel {
    tbd::MeshId mesh = tbd::kInvalidMesh;
};
std::vector<SimCarModel> g_simCars;
bool g_simCarsTried = false;
double g_simAgentTime = 0; // seconds the cosmetic cars have driven (real time while the game runs)
tbd::city::AgentParams g_simAgentParams;
uint32_t g_simCarsDrawn = 0;
bool g_herdsOwnCars = false; // city_herds.inl: the herd children draw the cars and the flow-driven cosmetic ones step aside

void SimAgentsSync(float realDt) {
    if (!g_clock.paused) g_simAgentTime += realDt * (1.0 + 0.5 * g_clock.speed);
    if (g_simCarsTried || g_segFlow.empty() || !g_engine.renderer) return;
    g_simCarsTried = true; // outside the frame: the uploads may wait
    static const char* const kModels[] = {"CAR_sedan_white", "CAR_hatch_red", "CAR_suv_black", "CAR_wagon_silver",
                                          "CAR_coupe_blue", "CAR_pickup_grey", "CAR_van_white", "CAR_compact_green"};
    for (const char* m : kModels) {
        tbd::Entity e;
        e.mesh = tbd::MeshKind::Imported;
        e.meshPath = UtilAbsolute((std::string("assets/vehicles/cars/glb/") + m + ".glb").c_str()) + "#native";
        e.color = {1, 1, 1};
        if (ImportedVertices(e.meshPath).empty()) continue;
        SimCarModel c;
        c.mesh = UploadEntityMesh(e);
        if (c.mesh != tbd::kInvalidMesh) g_simCars.push_back(c);
    }
    g_simAgentParams.models = (int)g_simCars.size();
    tbd::Log("Sim: %zu car models for cosmetic traffic", g_simCars.size());
}

// Dev: TBD_SIM_OVERLAY=1 draws every road segment's solved flow over it: green free, yellow busy, orange congested, red
// jammed (line width by volume), one transient draw.
void SimFlowOverlay(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    static const bool on = sim_host::EnvDouble("TBD_SIM_OVERLAY", 0.0) > 0.0;
    if (!on || g_segFlow.size() != g_roads.net.segs.size()) return;
    static std::vector<tbd::Vertex> v;
    v.clear();
    static const tbd::Vec3 kCol[4] = {{0.2f, 0.85f, 0.3f}, {0.95f, 0.85f, 0.2f}, {0.98f, 0.5f, 0.1f}, {0.9f, 0.15f, 0.1f}};
    for (size_t s = 0; s < g_segFlow.size(); ++s) {
        const tbd::city::SegFlow& f = g_segFlow[s];
        if (f.flow <= 1.0f || !g_roads.net.segs[s].alive) continue;
        const tbd::city::Cubic cu = g_roads.net.Curve((int)s);
        const float w = std::clamp(f.flow / 400.0f, 0.6f, 6.0f);
        const tbd::Vec3 col = kCol[f.cls];
        tbd::city::V2 prev = cu.At(0);
        for (int i = 1; i <= 8; ++i) {
            const tbd::city::V2 q = cu.At(i / 8.0f);
            const tbd::city::V2 n = tbd::city::RightOf(tbd::city::Norm(q - prev)) * w;
            const float y0 = g_roads.net.YAt(g_roads.net.segs[s], (i - 1) / 8.0f) + 1.5f, y1 = g_roads.net.YAt(g_roads.net.segs[s], i / 8.0f) + 1.5f;
            zfx::Quad(v, {prev.x - n.x, y0, prev.z - n.z}, {prev.x + n.x, y0, prev.z + n.z}, {q.x + n.x, y1, q.z + n.z}, {q.x - n.x, y1, q.z - n.z}, {0, 0, 0}, col);
            prev = q;
        }
    }
    if (!v.empty()) r.DrawTransient(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
}

// Near cars cast shadows (Track R9): the ones within kSimCarShadowRange of the eye, drawn into the sun's map by SimAgentsCasters. The
// shadow pass runs before the scene, so it draws the rows the previous frame gathered here: one frame behind, a quarter metre at
// city speeds, under the shadow map's own softness.
constexpr float kSimCarShadowRange = 150.0f;
std::vector<std::vector<float>> g_simCarShadowRows; // per car model
void SimAgentsCasters(tbd::Renderer& r) {
    for (size_t m = 0; m < g_simCarShadowRows.size() && m < g_simCars.size(); ++m) {
        const std::vector<float>& rows = g_simCarShadowRows[m];
        if (rows.empty()) continue;
        const uint32_t n = (uint32_t)(rows.size() / 12);
        const tbd::MeshId mesh = g_simCars[m].mesh;
        const auto it = g_engine.matParts.find(mesh);
        if (it == g_engine.matParts.end()) { r.DrawShadowInstancedTransient(mesh, rows.data(), n); continue; }
        for (const MatPart& p : it->second)
            if (p.lodError == 0.0f && p.caster) r.DrawShadowInstancedTransient(p.mesh, rows.data(), n);
    }
}

void SimAgentsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    SimFlowOverlay(r, viewProj);
    g_simCarsDrawn = 0;
    g_simCarShadowRows.resize(g_simCars.size());
    for (auto& v : g_simCarShadowRows) v.clear();
    if (g_herdsOwnCars) return;
    if (g_simCars.empty() || g_segFlow.size() != g_roads.net.segs.size() || g_segFlowVersion != g_roads.net.version) return;
    if (g_engine.camera.distance > 1200.0f) return; // past this the cars are specks: none drawn
    static std::vector<tbd::city::CarInstance> cars;
    const tbd::city::V2 focus{g_engine.camera.target.x, g_engine.camera.target.z};
    g_simAgentParams.radius = std::clamp(g_engine.camera.distance * 1.2f, 150.0f, 700.0f);
    tbd::city::CosmeticCars(g_roads.net, g_simSegGrid, g_segFlow, focus, g_simAgentTime, g_simAgentParams, cars);
    static std::vector<float> rows;
    for (int m = 0; m < (int)g_simCars.size(); ++m) {
        rows.clear();
        for (const tbd::city::CarInstance& c : cars) {
            if (c.model != m) continue;
            float row[12];
            tbd::city::CarRow(c, row);
            rows.insert(rows.end(), row, row + 12);
            const tbd::Vec3 eye = g_engine.camera.Eye(), at{row[3], row[7], row[11]};
            if (tbd::Dot(at - eye, at - eye) < kSimCarShadowRange * kSimCarShadowRange) g_simCarShadowRows[(size_t)m].insert(g_simCarShadowRows[(size_t)m].end(), row, row + 12);
        }
        if (rows.empty()) continue;
        const uint32_t n = (uint32_t)(rows.size() / 12);
        const tbd::MeshId mesh = g_simCars[(size_t)m].mesh;
        const auto it = g_engine.matParts.find(mesh);
        if (it == g_engine.matParts.end()) {
            r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
            r.DrawInstancedTransient(mesh, rows.data(), n, viewProj);
        } else {
            for (const MatPart& p : it->second) {
                if (p.lodError > 0.0f) continue; // the full level only
                r.SetMaterial(p.albedo, p.normal, p.surface);
                r.DrawInstancedTransient(p.mesh, rows.data(), n, viewProj);
            }
        }
        g_simCarsDrawn += n;
    }
    r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
}

// SimInit's registration (same place in the order as before the move).
void SimRegisterFlow() {
    // S5: flow traffic, checked every 5 game minutes (solved on a worker when anything changed or every flowSolveMinutes).
    g_sim.Register("flow", 5, 1, [](const tbd::city::SimTick& t) { SimFlow(t); });
}