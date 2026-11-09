// ------------------------------------------------------------------- city sim Phase 4: what the services read from the city
// Included by city_sim_p4.inl. Everything here runs on change only: the building list (registry or zones changed, at
// most every 2 real seconds), the coverage snapshot (a 10-minute tick after a change), the garbage lots (zones or roads
// changed; sorted and grouped on a worker) and the garbage facilities (every game hour, a handful).

namespace sim_p4 {
// The road node at the end of segment `seg` nearer to p (-1: none).
int NodeNear(int seg, V2 p) {
    const Network& net = g_roads.net;
    if (seg < 0 || seg >= (int)net.segs.size() || !net.segs[(size_t)seg].alive) return -1;
    const Segment& s = net.segs[(size_t)seg];
    const tbd::Vec3 a = net.nodes[(size_t)s.a].p, b = net.nodes[(size_t)s.b].p;
    const float da = (a.x - p.x) * (a.x - p.x) + (a.z - p.z) * (a.z - p.z), db = (b.x - p.x) * (b.x - p.x) + (b.z - p.z) * (b.z - p.z);
    return da <= db ? s.a : s.b;
}
// A transport-tab building's gate segment: the front edge's road, else any edge of a turned lot (as SimServices does).
int GateSeg(const Placed& b) {
    const BuildingDef& d = Defs()[(size_t)b.def];
    float t, dd;
    V2 mid[4], out[4];
    EdgeMids(b.c, b.front, d.W(), d.D(), mid, out);
    int seg = -1;
    for (int e = 0; e < 4 && seg < 0; ++e) seg = g_roads.net.SegmentAt(mid[e] + out[e] * 2.0f, 14.0f, t, dd);
    return seg;
}
SvcSpec SpecOf(const Placed& b) {
    const BuildingDef& d = Defs()[(size_t)b.def];
    return SvcSpecFor((int)d.tab, d.name, d.workplaces, d.vehicles, d.w, d.d);
}
int CityCell(int hint, CellKey k) {
    const auto& cells = g_citySim.cells.cells;
    if (hint >= 0 && (size_t)hint < cells.size() && cells[(size_t)hint].key == k) return hint;
    return g_citySim.cells.Find(k);
}

// The building list from the zones and the transport buildings (ids from the registry; homes and jobs from its rows,
// which Phase 1 fills).
void SyncBuildings(bool force) {
    const uint64_t key = g_simBuildings.version * 1000003ull ^ g_zones.version * 7919ull ^ g_transport.version;
    if (key == bldKey && !force) return; // called at the 10-minute ticks only (sim time, not real time)
    const double t0 = Now();
    bldKey = key, lastBldMs = tbd::platform::TickCountMs();
    std::vector<SvcBldg> next;
    next.reserve(g_zones.lots.size() + 64);
    const auto& types = ZoneTypes();
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        const BuildingId id = g_simBuildings.Find(BOrigin::Lot, l.id);
        const BuildingRow* r = id.Valid() ? g_simBuildings.Get(id) : nullptr;
        if (!r) continue;
        SvcBldg b;
        b.id = id.v, b.lot = l.id, b.key = CellKeyAt(l.c.x, l.c.z), b.cell = CityCell(r->cell, b.key), b.node = NodeNear(l.frontSeg, l.c);
        const ZoneCat cat = types[(size_t)l.type].cat;
        b.kind = cat == ZoneCat::Residential ? kBkHome : cat == ZoneCat::Commercial ? kBkShop : cat == ZoneCat::Office ? kBkOffice : kBkIndustry;
        b.level = (uint8_t)std::clamp(l.level, 1, 5), b.homes = r->homes;
        uint32_t jobs = 0;
        for (int e = 0; e < kEduLevels; ++e) jobs += r->jobSlots[e];
        b.jobs = (uint16_t)std::min<uint32_t>(jobs, 65535);
        next.push_back(b);
    }
    for (const Placed& p : g_transport.items) {
        if (!p.alive || p.state != BState::Done || !SpecOf(p).Valid()) continue;
        const BuildingId id = g_simBuildings.Find(BOrigin::Transport, p.id);
        if (!id.Valid()) continue;
        SvcBldg b;
        b.id = id.v, b.key = CellKeyAt(p.c.x, p.c.z), b.cell = CityCell(-1, b.key), b.node = NodeNear(GateSeg(p), p.c), b.kind = kBkService;
        next.push_back(b);
    }
    std::sort(next.begin(), next.end(), [](const SvcBldg& a, const SvcBldg& b) { return a.id < b.id; });
    SvcSetBuildings(w, std::move(next));
    bldMs = Now() - t0;
}

// The coverage snapshot: the CitySim cells (in their order, so factor indices match), each with the road node of its
// biggest building and its people, the service sites after funding. False when nothing changed since the last one.
bool CoverageInput(SvcPrepared& p, bool force) {
    int done = 0;
    for (const Placed& b : g_transport.items) done += b.alive && b.state == BState::Done;
    uint64_t key = g_transport.version * 1000003ull + (uint64_t)done;
    key = key * 31 + g_roads.net.version, key = key * 31 + g_zones.version, key = key * 31 + w.bld.size();
    key = key * 31 + (herds_host::tableReady ? herds_host::table.version : 0);
    for (int s = 0; s < kServicesV2; ++s) key = key * 31 + (uint64_t)std::lround(FundingOf(s) * 100.0f);
    if (key == covKey && !force) return false;
    covKey = key;
    if (herds_host::tableReady && (!graph || graph->version != herds_host::table.version || graph->nodes != herds_host::table.nodes))
        graph = std::make_shared<const SvcGraph>(SvcGraph::From(herds_host::table));
    p = SvcPrepared();
    p.cov.graph = herds_host::tableReady ? graph : nullptr;
    const auto& cells = g_citySim.cells.cells;
    const size_t C = std::min(cells.size(), g_citySim.sim.size());
    p.cov.cells.resize(C), p.stats.resize(C);
    std::vector<uint32_t> best(C, 0);
    for (size_t c = 0; c < C; ++c) {
        const CellSim& s = g_citySim.sim[c];
        SvcCellIn& in = p.cov.cells[c];
        in.key = cells[c].key, in.x = (in.key.x + 0.5f) * kCityCellSize, in.z = (in.key.z + 0.5f) * kCityCellSize;
        const uint32_t res = (uint32_t)s.residents[kKids] + s.residents[kAdults] + s.residents[kSeniors];
        const uint32_t workers = (uint32_t)s.employed[0] + s.employed[1] + s.employed[2];
        in.people = res + workers / 2;
        SvcCellStat& st = p.stats[c];
        st.residents = res, st.homes = s.homeCap, st.jobs = (uint32_t)s.jobCap[0] + s.jobCap[1] + s.jobCap[2], st.industryJobs = s.jobCap[kJobIndustrial];
    }
    for (const SvcBldg& b : w.bld) {
        const int c = b.cell;
        if (c < 0 || (size_t)c >= C || b.node < 0 || b.kind == kBkService) continue;
        const uint32_t wt = (uint32_t)b.homes + b.jobs + 1;
        if (wt > best[(size_t)c]) best[(size_t)c] = wt, p.cov.cells[(size_t)c].node = b.node;
        p.stats[(size_t)c].maxLevel = std::max(p.stats[(size_t)c].maxLevel, b.level);
    }
    for (const Placed& b : g_transport.items) {
        if (!b.alive || b.state != BState::Done) continue;
        const SvcSpec spec = SpecOf(b);
        if (!spec.Valid()) continue;
        const BuildingId id = g_simBuildings.Find(BOrigin::Transport, b.id);
        if (!id.Valid()) continue;
        SvcSite s;
        s.id = id.v, s.spec = SvcFunded(spec, FundingOf(spec.slot)), s.x = b.c.x, s.z = b.c.z, s.node = NodeNear(GateSeg(b), b.c);
        p.cov.sites.push_back(s);
    }
    std::sort(p.cov.sites.begin(), p.cov.sites.end(), [](const SvcSite& a, const SvcSite& b) { return a.id < b.id; });
    p.bldSnap = w.bld;
    return true;
}

// ---- garbage: the lots (on zones or roads change; sorted on a worker) and the facilities (every game hour)
std::vector<int> segComp;
uint64_t segCompFor = ~0ull;
const std::vector<int>& SegComp() {
    if (segCompFor != g_roads.net.version) segCompFor = g_roads.net.version, RoadComponents(g_roads.net, segComp);
    return segComp;
}
void GarbageLots() {
    const uint64_t key = g_zones.version * 1000003ull ^ g_roads.net.version;
    if (key == lotsKey || lotsJob) return;
    lotsKey = key;
    const std::vector<int>& comp = SegComp();
    std::vector<SvcGarbLot> lots;
    lots.reserve(g_zones.lots.size());
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || !l.Built()) continue;
        SvcGarbLot g;
        g.lot = l.id, g.x = l.c.x, g.z = l.c.z, g.node = NodeNear(l.frontSeg, l.c), g.cell = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
        g.comp = l.frontSeg >= 0 && (size_t)l.frontSeg < comp.size() ? comp[(size_t)l.frontSeg] : -1;
        g.perMonth = garbage::PerCell(l.type) * (float)(l.w * l.d), g.industrial = garbage::Industrial(l.type) ? 1 : 0;
        lots.push_back(g);
    }
    if (garb.Lots() == 0) { garb.SetLots(std::move(lots), &garbLoaded); return; } // the first list (a load): at once
    lotsReady.store(false);
    auto job = std::make_shared<SvcGarbage::Prepared>();
    lotsJob = job;
    g_engine.jobs->RunBackground([job, lots = std::move(lots)]() mutable {
        *job = std::move(*SvcGarbage::Prepare(std::move(lots)));
        sim_p4::lotsReady.store(true, std::memory_order_release);
    });
}
void GarbageFacilities() {
    const std::vector<int>& comp = SegComp();
    auto compAt = [&](V2 p) { float t, d; const int s = g_roads.net.SegmentAt(p, 12.0f, t, d); return s >= 0 && (size_t)s < comp.size() ? comp[(size_t)s] : -1; };
    auto nodeAt = [&](V2 p) { float t, d; return NodeNear(g_roads.net.SegmentAt(p, 12.0f, t, d), p); };
    garbFac.clear();
    for (const IndSite& s : g_ind.sites) {
        if (!s.IsLandfill() || s.area.empty()) continue;
        const V2 gate = s.c + s.front * (kHubD * 0.5f + 2.0f);
        SvcGarbFacility f;
        f.id = (uint32_t)(0x40000000u | (s.id & 0x3FFFFFFFu)), f.x = gate.x, f.z = gate.z, f.comp = compAt(gate), f.node = nodeAt(gate);
        f.perMonth = s.Fill() >= 1.0f ? 0.0f : s.perMonth * FundingOf(kSvcGarbage), f.range = garbage::kRange;
        garbFac.push_back(f);
    }
    for (const WasteBuilding& b : g_wasteB) {
        const WasteType& T = kWasteTypes[b.type];
        const V2 gate = b.c + b.front * (T.d * 0.5f + 2.0f);
        SvcGarbFacility f;
        f.id = (uint32_t)(0x80000000u | (b.id & 0x3FFFFFFFu)), f.x = gate.x, f.z = gate.z, f.comp = compAt(gate), f.node = nodeAt(gate);
        f.hazardous = b.type == 2 ? 1 : 0, f.perMonth = T.tPerMonth * FundingOf(kSvcGarbage), f.range = f.hazardous ? garbage::kRange * 2 : garbage::kRange;
        garbFac.push_back(f);
    }
    std::sort(garbFac.begin(), garbFac.end(), [](const SvcGarbFacility& a, const SvcGarbFacility& b) { return a.id < b.id; });
}
} // namespace sim_p4
