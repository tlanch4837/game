// ------------------------------------------------------------------- city sim: utilities into the cells, flags, service coverage (S3)
// Included by city_sim.inl (moved out of it, CS2 sim roadmap Phase 0; owned by Phase 4 from here on).

// The service kind of a transport-tab building, -1 for none (deathcare and communications cover nothing yet).
int SimServiceKind(const tbd::city::BuildingDef& d) {
    using tbd::city::TTab;
    switch (d.tab) {
    case TTab::Fire: return tbd::city::kFire;
    case TTab::Police: return tbd::city::kPolice;
    case TTab::Health: return tbd::city::kHealth;
    case TTab::Education: return tbd::city::kSchool;
    case TTab::Bus: case TTab::Rail: return tbd::city::kTransit;
    default: return -1;
    }
}

void SimServices() {
    using namespace tbd::city;
    // utilities: the solve's per-lot result (lots under construction count as fine there)
    static uint64_t utilSolves = ~0ull, utilZones = ~0ull;
    if (utilSolves != g_utilSolve.solves || utilZones != g_zones.version) {
        utilSolves = g_utilSolve.solves, utilZones = g_zones.version;
        g_simLotUtil.assign(g_zones.lots.size(), kLotAll);
        if (g_utilReport.lots.size() == g_zones.lots.size())
            for (size_t i = 0; i < g_zones.lots.size(); ++i) {
                const UtilStatus& u = g_utilReport.lots[i];
                g_simLotUtil[i] = (uint8_t)((u.elec ? kLotPower : 0) | (u.water ? kLotWater : 0) | (u.sewage ? kLotSewage : 0) | (u.road ? kLotRoad : 0));
            }
        ApplyUtilities(g_citySim, g_zones, g_simLotUtil);
    }
    { // the per-building flags: O(lots), every 10 game minutes (abandonment creeps up with time, so no cheaper key)
        static std::vector<tbd::city::SimFlags> fresh;
        ComputeSimFlags(g_zones, g_citySim, g_simGrowth, g_simTune, g_simLotUtil, fresh);
        if (fresh != g_simFlags) g_simFlags.swap(fresh), ++g_simFlagsVersion;
    }
    // coverage: only when the services, the roads, the lots or the funding changed
    static uint64_t kTrans = ~0ull, kNet = ~0ull, kZones = ~0ull;
    static float kFund[kServices] = {};
    static std::vector<int> segComp;
    static uint64_t compFor = ~0ull;
    int done = 0;
    for (const Placed& b : g_transport.items) done += b.alive && b.state == BState::Done;
    const uint64_t transKey = g_transport.version * 1000003ull + (uint64_t)done;
    bool fundChanged = false;
    for (int k = 0; k < kServices; ++k) fundChanged |= kFund[k] != g_simFunding[k];
    if (transKey == kTrans && kNet == g_roads.net.version && kZones == g_zones.version && !fundChanged) return;
    kTrans = transKey, kNet = g_roads.net.version, kZones = g_zones.version;
    for (int k = 0; k < kServices; ++k) kFund[k] = g_simFunding[k];
    if (compFor != g_roads.net.version) compFor = g_roads.net.version, RoadComponents(g_roads.net, segComp);
    std::vector<ServiceSource> src;
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const BuildingDef& d = Defs()[b.def];
        const int kind = SimServiceKind(d);
        if (kind < 0) continue;
        ServiceSource s;
        s.at = b.c;
        s.kind = kind;
        s.radius = (float)std::min(g_simTune.serviceRadiusMax, g_simTune.serviceRadiusBase + g_simTune.serviceRadiusPerVehicle * d.vehicles);
        float t, dd;
        V2 mid[4], out[4]; // the gate: the front edge's road, else whichever edge of a turned lot has one (placement_rotation.h)
        EdgeMids(b.c, b.front, d.W(), d.D(), mid, out);
        int seg = -1;
        for (int e = 0; e < 4 && seg < 0; ++e) seg = g_roads.net.SegmentAt(mid[e] + out[e] * 2.0f, 14.0f, t, dd);
        s.comp = seg >= 0 && seg < (int)segComp.size() ? segComp[(size_t)seg] : -1;
        src.push_back(s);
    }
    ComputeCoverage(g_citySim, g_zones, segComp, src, g_simFunding);
    ++g_simCoverageRuns;
}

// SimInit's registration (same place in the order as before the move).
void SimRegisterServices() {
    // S3: utilities into the cells (from the cached solve) and service coverage, every 10 game minutes, only on change.
    g_sim.Register("services", 10, 1, [](const tbd::city::SimTick&) { SimServices(); });
}