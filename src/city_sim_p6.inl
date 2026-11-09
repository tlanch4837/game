// ------------------------------------------------------------------- city sim Phase 6: production chains, companies, trucks, import and export
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 6 owns this file; the hub calls the five hooks below,
// whose signatures are frozen by Phase 0. The model is headless: src/city/sim_goods*.h (tests\sim_goods_test,
// tests\sim_goods_companies_test); this file is its host:
//   sites      every hour (and when the building registry changes): one GoodsSite per industry hub (raw), industrial lot
//              (processor), commercial lot (shop) and office lot, with workers from the CitySim cell counters, the
//              efficiency of the cell's power and water, the road node of its entrance; the residents; the outside
//              connections (placed ones and dead-end roads at the map edge)
//   system     "goods", every game minute (it works on 5-minute boundaries, a twelfth of the companies each time)
//   flags      P6's bits (no goods, no customers, cannot ship) in the hub's shared g_simFlagTable (by BuildingId); the
//              hub merges them into the per-lot g_simFlags; P6 takes the no customers bit over from ComputeSimFlags
//   factor     goodsAvailable per CitySim cell into the shared g_cellFactors (SimGoodsWriteFactors, hourly)
//   events     into the shared g_simEvents
//   trucks     yesterday's trucks between road nodes as freight NodeLinks (SimGoodsTruckLinks), drawn by the herds host
//   hubs       a raw company's stock shows on its hub (IndSite::stock, tonnes): the hubs' stock now has consumers
//   UI         the Production panel (p6.goods) and the Goods overlay
//   save       COMP GOOD TRAD (sim_goods_save.h)
// Tuning: the "goods." keys of tuning.json (sim_tuning_table.h GoodsTuning) override GoodsParams' compiled defaults.
// The headers (sim_factors.h, sim_goods_save.h) come from the host TUs.

namespace sim_p6 {
tbd::city::GoodsWorld world;
tbd::city::SimFlagTable& flags = g_simFlagTable; // the shared table; P6 writes only kMask (its bits)
tbd::city::SimEventRing& events = g_simEvents;    // P6's events (CompanyBankrupt, CompanyOpened, GoodsShortage, Import, Export)
std::vector<tbd::city::GoodsSite> sites;
std::vector<tbd::city::NodeLink> truckLinks;
std::vector<int32_t> hubCompany;         // per g_ind.sites index: the CompanyId raw of its raw company (0 none)
uint64_t sitesFor = ~0ull;               // registry version the sites were built from
int64_t sitesMinute = -1000000;
double tickMs = 0, tickAvgMs = 0, tickWorstMs = 0;

std::vector<int32_t> siteLot;  // per site: its index in g_zones.lots (-1 a hub), valid while g_zones.version == siteZones
uint64_t siteZones = ~0ull;
std::unordered_map<uint64_t, int32_t> nodeCache; // (origin key) -> road node, for the network version nodeNet
uint64_t nodeNet = ~0ull;

const tbd::city::SimFlags kMask = tbd::city::kFlagNoGoods | tbd::city::kFlagNoCustomers | tbd::city::kFlagNoFreight;

// The road node nearest a point (-1: no road within 200 m).
int NodeNear(tbd::city::V2 p, int frontSeg) {
    using namespace tbd::city;
    const auto& net = g_roads.net;
    float t = 0.5f, d = 0;
    int seg = frontSeg >= 0 && (size_t)frontSeg < net.segs.size() && net.segs[(size_t)frontSeg].alive ? frontSeg : -1;
    if (seg < 0) {
        if (g_simSegGrid.builtFrom != net.version) return -1; // the flow system builds the grid first
        seg = g_simSegGrid.Nearest(net, p, 200.0f, t, d);
    } else {
        const tbd::city::Segment& s = net.segs[(size_t)seg];
        const float da = (size_t)s.a < net.nodes.size() ? Len(XZ(net.nodes[(size_t)s.a].p) - p) : 1e9f;
        const float db = (size_t)s.b < net.nodes.size() ? Len(XZ(net.nodes[(size_t)s.b].p) - p) : 1e9f;
        t = da <= db ? 0.0f : 1.0f;
    }
    if (seg < 0) return -1;
    const tbd::city::Segment& s = net.segs[(size_t)seg];
    return t < 0.5f ? s.a : s.b;
}

// Outside connections: the placed ones (any kind: road, rail, harbour and airport cargo) and dead-end car roads at the
// map edge, in node order.
void BuildOutsides() {
    using namespace tbd::city;
    auto& outs = world.outsides;
    outs.clear();
    for (const Outside& o : g_transit.outsides)
        if (o.alive) outs.push_back({(int32_t)std::lround(o.p.x), (int32_t)std::lround(o.p.z), NodeNear(o.p, -1)});
    const auto& net = g_roads.net;
    const float edge = tbd::CityMap::kHalf - 100.0f;
    for (size_t n = 0; n < net.nodes.size(); ++n) {
        const Node& nd = net.nodes[n];
        if (!nd.alive) continue;
        const V2 q = XZ(nd.p);
        if (std::max(std::fabs(q.x), std::fabs(q.z)) < edge) continue;
        int car = 0;
        for (int s : nd.segs) {
            if (!net.segs[(size_t)s].alive) continue;
            const Profile& pr = net.Prof(s);
            if (pr.cat != Cat::Rail && pr.cat != Cat::Path && pr.fwd + pr.back > 0) ++car;
        }
        if (car == 1) outs.push_back({(int32_t)std::lround(q.x), (int32_t)std::lround(q.z), (int32_t)n});
    }
}

// One site per company-bearing building, in BuildingId order (the registry's), so the company ids come out the same.
void BuildSites() {
    using namespace tbd::city;
    sites.clear(), siteLot.clear();
    if (nodeNet != g_roads.net.version || g_simSegGrid.builtFrom != g_roads.net.version) nodeCache.clear(), nodeNet = g_roads.net.version;
    auto node = [](uint64_t key, V2 p, int frontSeg) {
        const auto it = nodeCache.find(key);
        if (it != nodeCache.end()) return it->second;
        const int n = NodeNear(p, frontSeg);
        if (g_simSegGrid.builtFrom == g_roads.net.version) nodeCache[key] = n; // the grid is up: the answer holds
        return n;
    };
    // registry row -> lot index from the registry sync's per-lot ids (sim_save::hintLot, checked here): a linear pass
    // instead of a hash map of every lot each hour; a row the hints miss falls back to the map (built only then)
    static std::vector<int32_t> rowLot;
    rowLot.assign(g_simBuildings.Capacity(), -1);
    for (size_t i = 0; i < g_zones.lots.size() && i < sim_save::hintLot.size(); ++i) {
        const BuildingRow* r = g_simBuildings.Get(sim_save::hintLot[i]);
        if (g_zones.lots[i].alive && r && r->origin == BOrigin::Lot && r->originId == g_zones.lots[i].id) rowLot[sim_save::hintLot[i].Index()] = (int32_t)i;
    }
    std::unordered_map<uint64_t, uint32_t> lotIdx, hubIdx;
    bool lotIdxBuilt = false;
    auto lotOf = [&](uint32_t row, uint64_t originId) -> int64_t {
        if (rowLot[row] >= 0) return rowLot[row];
        if (!lotIdxBuilt) {
            lotIdxBuilt = true;
            for (size_t i = 0; i < g_zones.lots.size(); ++i)
                if (g_zones.lots[i].alive) lotIdx[g_zones.lots[i].id] = (uint32_t)i;
        }
        const auto it = lotIdx.find(originId);
        return it == lotIdx.end() ? -1 : (int64_t)it->second;
    };
    for (size_t i = 0; i < g_ind.sites.size(); ++i) hubIdx[g_ind.sites[i].id] = (uint32_t)i;
    const auto& types = ZoneTypes();
    int64_t residents = 0;
    for (const CellSim& c : g_citySim.sim) residents += c.Population();
    world.residents = residents;
    for (uint32_t i = 1; i < g_simBuildings.Capacity(); ++i) {
        const BuildingId b = g_simBuildings.At(i);
        const BuildingRow* row = g_simBuildings.Get(b);
        if (!row || row->alive != 1) continue;
        GoodsSite s;
        s.building = b.v;
        if (row->origin == BOrigin::Lot) {
            const int64_t li = lotOf(i, row->originId);
            if (li < 0) continue;
            const Lot& l = g_zones.lots[(size_t)li];
            if (!l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
            const ZoneCat cat = types[(size_t)l.type].cat;
            int kind;
            if (cat == ZoneCat::Industrial) s.kind = CompanyKind::Processor, s.recipe = (uint8_t)RecipeForLot(l.id), kind = kJobIndustrial;
            else if (cat == ZoneCat::Commercial) s.kind = CompanyKind::Shop, kind = kJobCommercial;
            else if (cat == ZoneCat::Office) s.kind = CompanyKind::Office, kind = kJobOffice;
            else continue;
            const LotCap cap = LotCapacity(l);
            s.jobs = cap.jobs[kind];
            s.x = (int32_t)std::lround(l.c.x), s.z = (int32_t)std::lround(l.c.z);
            s.node = node(row->originId << 1, l.c, l.hasRoad ? l.frontSeg : -1);
            const int c = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
            s.cell = c;
            if (c >= 0) {
                const CellSim& cs = g_citySim.sim[(size_t)c];
                s.workers = cs.jobCap[kind] ? (uint16_t)((uint32_t)s.jobs * cs.employed[kind] / cs.jobCap[kind]) : 0;
                s.effPermille = (uint16_t)CellEfficiencyPermille(cs);
                s.localResidents = (int32_t)cs.Population();
            }
        } else if (row->origin == BOrigin::Industry) {
            const auto it = hubIdx.find(row->originId);
            if (it == hubIdx.end()) continue;
            const IndSite& h = g_ind.sites[it->second];
            const Good g = RawGoodOfHub((int)h.kind);
            if (g == Good::None) continue; // a landfill
            s.kind = CompanyKind::Raw, s.raw = g;
            s.rawMilliPerHour = (int32_t)std::lround((double)h.perMonth * world.p.rawScale * 1000.0 / 24.0);
            s.x = (int32_t)std::lround(h.c.x), s.z = (int32_t)std::lround(h.c.z);
            s.node = node(row->originId << 1 | 1, h.c + h.front * (tbd::city::kHubD * 0.5f), -1);
            s.cell = g_citySim.cells.Find(CellKeyAt(h.c.x, h.c.z));
        } else {
            continue;
        }
        sites.push_back(s);
        siteLot.push_back(row->origin == BOrigin::Lot ? (int32_t)lotOf(i, row->originId) : -1);
    }
}

// The hubs show their raw company's stock (tonnes). Old saves (no COMP chunk): a new raw company starts with the hub's
// stock from the .tbdgame, so nothing waiting for pickup is lost.
void SyncHubs(bool seedFromHubs) {
    using namespace tbd::city;
    hubCompany.assign(g_ind.sites.size(), 0);
    for (size_t i = 0; i < g_ind.sites.size(); ++i) {
        IndSite& h = g_ind.sites[i];
        const Good g = RawGoodOfHub((int)h.kind);
        if (g == Good::None) continue;
        const CompanyId id = world.cos.Find(g_simBuildings.Find(BOrigin::Industry, h.id).v);
        CompanyRec* c = world.cos.Get(id);
        if (!c) continue;
        hubCompany[i] = (int32_t)id.v;
        if (seedFromHubs && h.stock > 0 && c->stock[(size_t)g] == 0) {
            const int32_t units = (int32_t)std::min(1e8, h.stock * 10.0);
            c->stock[(size_t)g] = units, world.today.produced[(size_t)g] += units; // enters the ledger as made
        }
        h.stock = c->stock[(size_t)g] / 10.0;
    }
}

uint32_t flagsEpoch = 0, flagsEpochSeen = ~0u; // the goods system's hourly summary
bool seedPending = true;                       // the first sites after a load without COMP take the hubs' stock

tbd::city::SimFlags BitsOf(int32_t f) {
    using namespace tbd::city;
    return (f & kCoNoGoods ? kFlagNoGoods : 0u) | (f & kCoNoCustomers ? kFlagNoCustomers : 0u) | (f & kCoNoFreight ? kFlagNoFreight : 0u);
}

void RefreshSites() {
    BuildOutsides();
    BuildSites();
    siteZones = g_zones.version;
    world.SetSites(sites, g_sim.Minute());
    SyncHubs(seedPending);
    seedPending = false;
    sitesFor = g_simBuildings.version, sitesMinute = g_sim.Minute();
}

// P6's bits onto the BuildingIds of the shared table (the hub merges the table into the per-lot flags the icons and
// the info card read), and goodsAvailable into the shared factors: once per hourly summary of the world.
void SimGoodsWriteFactorsShared();
void MergeFlags() {
    using namespace tbd::city;
    if (flagsEpochSeen == flagsEpoch) return;
    flags.ClearAll(kMask);
    world.cos.ForEach([&](uint32_t, const CompanyRec& c) { flags.Write(BuildingId((uint32_t)c.building), kMask, BitsOf(c.flags)); });
    flagsEpochSeen = flagsEpoch;
    SimGoodsWriteFactorsShared();
}

// Yesterday's trucks as freight node links (vehicles a day) for the herds.
void BuildTruckLinks() {
    truckLinks.clear();
    for (const tbd::city::TruckLink& t : world.TrucksLast()) {
        tbd::city::NodeLink l;
        l.fromNode = t.fromNode, l.toNode = t.toNode, l.n = (float)t.trucks, l.type = tbd::city::kTripFreight;
        truckLinks.push_back(l);
    }
}

void DrainEvents() {
    for (const tbd::city::SimEvent& e : world.events) {
        events.Push(e);
        if (e.type == tbd::city::SimEventType::CompanyBankrupt) tbd::Log("Goods: a company went bankrupt (building %u, cash %lld)", e.building, (long long)e.value);
    }
    world.events.clear();
}
} // namespace sim_p6

// For the other phases and the herds host (see the P6 report: P0 / P5 wire these up).
const std::vector<tbd::city::NodeLink>& SimGoodsTruckLinks() { return sim_p6::truckLinks; }
tbd::city::SimEventRing& SimGoodsEvents() { return sim_p6::events; }
const tbd::city::SimFlagTable& SimGoodsFlags() { return sim_p6::flags; }
// goodsAvailable per cell: the shops' supply; a cell without shops gets the city's mean (255 with no shops at all).
void SimGoodsWriteFactors(tbd::city::CellFactors& f) {
    const auto& a = sim_p6::world.cellAvail;
    int64_t sum = 0, n = 0;
    for (int16_t v : a) if (v >= 0) sum += v, ++n;
    const uint8_t mean = n ? (uint8_t)(sum / n) : 255;
    f.Fit(g_citySim.sim.size());
    bool changed = false;
    for (size_t c = 0; c < f.cells.size(); ++c) {
        const uint8_t v = c < a.size() && a[c] >= 0 ? (uint8_t)a[c] : mean;
        if (f.cells[c].goodsAvailable != v) f.cells[c].goodsAvailable = v, changed = true;
    }
    if (changed) ++f.version;
}
void sim_p6::SimGoodsWriteFactorsShared() { SimGoodsWriteFactors(g_cellFactors); }

// The building inspector's goods rows (city_interact.inl): the company in this building. False: none (or P6 off).
bool SimGoodsInspect(tbd::city::BuildingId b, std::vector<std::pair<std::string, std::string>>& rows) {
    using namespace tbd::city;
    if (g_simPhaseTune.goods.enabled == 0.0 || !b.Valid()) return false;
    const CompanyRec* c = sim_p6::world.cos.Get(sim_p6::world.cos.Find(b.v));
    if (!c) return false;
    static const char* const kKind[] = {"raw goods", "industry", "shop", "office"};
    char t[160];
    if (c->closedDays > 0) std::snprintf(t, sizeof(t), "%s, bankrupt (closed %d days)", kKind[c->kind & 3], c->closedDays);
    else std::snprintf(t, sizeof(t), "%s, %d of %d workers, cash %d, profit %+d yesterday", kKind[c->kind & 3], c->workers, c->jobs, c->cash, c->profitLast);
    rows.push_back({"Company", t});
    std::string st;
    for (int g = 0; g < kGoodCount; ++g)
        if (c->stock[(size_t)g] > 0 || c->incoming[(size_t)g] > 0) {
            std::snprintf(t, sizeof(t), "%s%s %.1f t", st.empty() ? "" : ", ", GoodName((Good)g), c->stock[(size_t)g] / 10.0);
            st += t;
            if (c->incoming[(size_t)g] > 0) std::snprintf(t, sizeof(t), " (+%.1f t coming)", c->incoming[(size_t)g] / 10.0), st += t;
        }
    const Good out = CompanyOutput(*c);
    if (st.empty()) st = "empty";
    rows.push_back({"Goods in stock", st});
    std::snprintf(t, sizeof(t), "%.1f t made, %.1f t sold yesterday%s%s", c->madeLast / 10.0, c->soldLast / 10.0, out != Good::None ? " - makes " : "", out != Good::None ? GoodName(out) : "");
    rows.push_back({"Production", t});
    return true;
}

// ---- UI: the Production panel and the Goods overlay
namespace sim_p6 {
std::string Tonnes(int64_t units) {
    char b[32];
    if (units >= 100000 || units <= -100000) std::snprintf(b, sizeof(b), "%.1fk t", (double)units / 10000.0);
    else std::snprintf(b, sizeof(b), "%.1f t", (double)units / 10.0);
    return b;
}
std::string Money(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%s%lld", v < 0 ? "-" : "+", (long long)(v < 0 ? -v : v));
    return b;
}
void FillPanel(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.sub = "Companies, goods and trade (yesterday)";
    int kinds[4] = {}, closed = 0;
    world.cos.ForEach([&](uint32_t, const CompanyRec& c) { ++kinds[c.kind & 3], closed += c.closedDays > 0; });
    char b[96];
    std::snprintf(b, sizeof(b), "%d raw  %d industry  %d shops  %d offices", kinds[0], kinds[1], kinds[2], kinds[3]);
    p.Stat("Companies", b);
    const GoodsLedger& l = world.last;
    p.Stat("Trade balance", Money(l.exportIncome - l.importCost), l.exportIncome >= l.importCost ? 1 : -1);
    std::vector<float> bal;
    for (int32_t v : world.balance) bal.push_back((float)v);
    p.Spark("Balance per day", bal, world.balance.empty() ? "" : Money(world.balance.back()));
    std::snprintf(b, sizeof(b), "%lld yesterday, %zu on the road", (long long)l.trucks, world.ships.size());
    p.Stat("Trucks", b);
    std::vector<std::pair<std::string, std::string>> made, trade;
    for (int g = 0; g < kGoodCount; ++g) {
        const size_t i = (size_t)g;
        if (l.produced[i] || l.used[i] || l.consumed[i]) made.push_back({GoodName((Good)g), Tonnes(l.produced[i]) + " made, " + Tonnes(l.used[i] + l.consumed[i]) + " used"});
        if (l.imported[i] || l.exported[i]) trade.push_back({GoodName((Good)g), Tonnes(l.imported[i]) + " in, " + Tonnes(l.exported[i]) + " out"});
    }
    p.List("Produced and consumed", made, kGoodCount);
    p.List("Imported and exported", trade, kGoodCount);
    p.Stat("Short of goods", std::to_string(world.shortages), world.shortages ? -1 : 0);
    p.Stat("Not enough customers", std::to_string(world.noCustomers), world.noCustomers ? -1 : 0);
    p.Stat("Cannot ship goods", std::to_string(world.noFreight), world.noFreight ? -1 : 0);
    std::snprintf(b, sizeof(b), "%d closed now, %d ever", closed, world.bankruptTotal);
    p.Stat("Bankrupt", b, closed ? -1 : 0);
    std::snprintf(b, sizeof(b), "goods tick %.3f ms avg, %.3f worst", tickAvgMs, tickWorstMs);
    p.Caption(b);
}
float OverlayCell(void*, int cell, float, float) {
    const auto& a = world.cellAvail;
    return cell >= 0 && (size_t)cell < a.size() && a[(size_t)cell] >= 0 ? a[(size_t)cell] / 255.0f : -1.0f;
}
} // namespace sim_p6

// The "goods." keys (sim_tuning_table.h GoodsTuning; defaults = GoodsParams' compiled values) into the world's params.
void SimP6ApplyTuning(const tbd::city::GoodsTuning& t, tbd::city::GoodsParams& p) {
    auto i32 = [](double v, int lo) { return (int32_t)std::max<double>(lo, std::min(1e9, std::round(v))); };
    p = tbd::city::GoodsParams();
    p.workerMilliPerHour = i32(t.workerMilliPerHour, 0), p.rawScale = i32(t.rawScale, 0), p.stockHours = i32(t.stockHours, 1);
    p.outCapHours = i32(t.outCapHours, 1), p.truckUnits = i32(t.truckUnits, 1), p.truckMetresPerMinute = i32(t.truckMetresPerMinute, 1);
    p.loadMinutes = i32(t.loadMinutes, 0), p.matchRadiusM = i32(t.matchRadiusM, 0), p.exportAtPercent = i32(t.exportAtPercent, 1);
    p.residentMilliPerHour = i32(t.residentMilliPerHour, 0), p.wagePerDay = i32(t.wagePerDay, 0);
    p.retailMarkupPercent = i32(t.retailMarkupPercent, 0), p.officeValuePerDay = i32(t.officeValuePerDay, 0);
    p.bankruptDays = i32(t.bankruptDays, 1), p.reopenDays = i32(t.reopenDays, 0), p.noCustomersPercent = i32(t.noCustomersPercent, 0);
    p.noGoodsPercent = i32(t.noGoodsPercent, 0), p.startCash = i32(t.startCash, 0);
}

// ---- the hooks
void SimP6Init() {
    SimP6ApplyTuning(g_simPhaseTune.goods, sim_p6::world.p);
    g_sim.Register("goods", 1, 1, [](const tbd::city::SimTick& t) {
        const double a = g_sim.nowMs();
        sim_p6::world.Tick(t.minute);
        if (t.minute % 60 == 55) ++sim_p6::flagsEpoch; // the world summarised its flags (slice 11)
        const double ms = g_sim.nowMs() - a;
        sim_p6::tickMs = ms, sim_p6::tickAvgMs += (ms - sim_p6::tickAvgMs) * 0.01, sim_p6::tickWorstMs = std::max(sim_p6::tickWorstMs, ms);
    });
    tbd::gui::RegisterOverlayProvider(tbd::gui::Overlay::Goods, sim_p6::OverlayCell, nullptr, "No goods  ..  Supplied (shops)");
}

void SimP6Frame(float /*realDt*/) {
    using namespace sim_p6;
    const int64_t now = g_sim.Minute();
    if (sitesFor != g_simBuildings.version || now - sitesMinute >= 60 || now < sitesMinute) RefreshSites();
    DrainEvents();
    static int64_t day = -1, hubMinute = -1;
    if (now / 1440 != day) day = now / 1440, BuildTruckLinks();
    if (now != hubMinute) hubMinute = now, SyncHubs(false);
    MergeFlags();
}

void SimP6Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    auto fresh = [] {
        sim_p6::world.Clear(), sim_p6::flags.ClearAll(sim_p6::kMask), sim_p6::truckLinks.clear(), sim_p6::sites.clear(), sim_p6::siteLot.clear();
        sim_p6::sitesFor = ~0ull, sim_p6::sitesMinute = -1000000, sim_p6::seedPending = true, sim_p6::flagsEpochSeen = ~0u;
    };
    SimChunkDef comp, good, trad;
    comp.fourcc = simchunk::COMP, comp.version = kGoodsCompVersion;
    comp.save = [](ByteWriter& w) { WriteGoodsComp(w, sim_p6::world); };
    comp.load = [](ByteReader& r, uint16_t) { return ReadGoodsComp(r, sim_p6::world); };
    comp.fresh = fresh;
    good.fourcc = simchunk::GOOD, good.version = kGoodsGoodVersion;
    good.save = [](ByteWriter& w) { WriteGoodsGood(w, sim_p6::world); };
    good.load = [](ByteReader& r, uint16_t) { return ReadGoodsGood(r, sim_p6::world); };
    good.fresh = [] { sim_p6::world.ships.clear(), sim_p6::world.today.Clear(), sim_p6::world.total.Clear(); };
    trad.fourcc = simchunk::TRAD, trad.version = kGoodsTradVersion;
    trad.save = [](ByteWriter& w) { WriteGoodsTrad(w, sim_p6::world); };
    trad.load = [](ByteReader& r, uint16_t) { return ReadGoodsTrad(r, sim_p6::world); };
    trad.fresh = [] { sim_p6::world.balance.clear(), sim_p6::world.last.Clear(), tbd::city::GoodsSaveAccess::SetLast(sim_p6::world, {}); };
    reg.Register(comp), reg.Register(good), reg.Register(trad);
}

void SimP6Load(const tbd::city::SimLoadReport& rep) {
    using namespace tbd::city;
    using namespace sim_p6;
    const SimChunkResult* c = rep.Find(simchunk::COMP);
    const bool comp = c && c->status == SimChunkStatus::Loaded;
    if (!comp) world.Clear(); // companies gone: the shipments and ledger that refer to them go too
    const SimChunkResult* gd = rep.Find(simchunk::GOOD);
    if (comp && !(gd && gd->status == SimChunkStatus::Loaded)) // the trucks were lost: nobody waits for them
        world.cos.ForEach([](uint32_t, CompanyRec& co) { co.incoming.fill(0); });
    seedPending = !comp, sitesFor = ~0ull, sitesMinute = -1000000, flagsEpochSeen = ~0u;
    // the scheduler's clock is the save's (the M record): a shipment can never be due farther out than a day
    const int64_t now = g_sim.Minute();
    for (Shipment& s : world.ships)
        if (s.arrive > now + 1440 || s.depart > now) s.depart = (int32_t)now, s.arrive = (int32_t)(now + 60);
    BuildTruckLinks();
    if (comp) tbd::Log("Goods: %u companies, %zu trucks on the road, %zu days of trade loaded", world.cos.pool.Live(), world.ships.size(), world.balance.size());
}

void SimP6Panels() {
    tbd::gui::SimPanelEntry e;
    e.id = "p6.goods", e.name = "Production", e.icon = "cargo", e.fill = sim_p6::FillPanel;
    tbd::gui::RegisterSimPanel(e);
}
