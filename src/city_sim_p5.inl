// ------------------------------------------------------------------- city sim Phase 5: public transport, mode choice, taxis, outside connections
// Included by city_sim_phases.inl (CS2 sim roadmap, section 4). Phase 5 owns this file; the hub calls the five hooks at
// the end, whose signatures are frozen by Phase 0. The model is headless: src/city/sim_modes.h, sim_transit_*.h
// (tests\sim_transit_test); this file is its host:
//   zones      the CitySim supercells (centre, residents, jobs, wealth mix) plus one zone per outside connection
//   links      the commute links (CitySim::ForEachLink, the records' real jobs) plus the tourists from the connections
//   lines      the player's passenger lines (transit.h TransitWorld routes; bus, tram, train, ferry, plane, subway)
//   system     "transit", every game minute, deferred: :00 zones, :20 links, :40 parking and flags, the day's book at
//              midnight; every minute at most move.linksPerTick dirty links are evaluated (clean ones cost nothing)
//   traffic    the commute flow trips (city_sim_flow.inl SimBuildTrips) take each link's car + taxi share, and the herds
//              (city_herds.inl) the city's mean car share, instead of the old constants
//   vehicles   each line's vehicle count (the Transit panel's slider, else its length) drives the ridership capacity;
//              the vehicles drawn are that count scaled by the preset (g_simPreset.carDensity), TransitWorld draws them
//              (placeholder boxes in city_transport.inl: there are no bus / tram / metro models in assets yet)
//   flags      P5's bits (no public transport, no parking) in the shared g_simFlagTable
//   UI         the Transit panel (p5.transit), the Outside connections panel (p5.outside), the Transit ridership overlay
//   save       LINE MODE OUTC (sim_transit_save.h)
// Tuning: compiled defaults (TransitParams); "move.enabled" switches the phase off. P6 is read, never written: its
// yesterday's ledger (sim_p6::world.last) gives the cargo through the connections, and P6 can read the capacity back
// through SimTransitCargoCapacity().

// city/sim_transit_save.h (and through it every sim_transit*.h and sim_modes.h) comes from the host TUs (main.cpp,
// game_host.cpp): game_core.inl sits in an anonymous namespace.

namespace herds_host { // city_herds.inl (after the phase hub) owns it; P5 sets the herds' car share from the mode model
extern tbd::city::HerdSystem herds;
}
namespace sim_p6 { // city_sim_p6.inl (after this file) owns it; P5 only reads yesterday's trade
extern tbd::city::GoodsWorld world;
}

namespace sim_p5 {
tbd::city::TransitSim world;
tbd::city::LineBook book;
tbd::city::OutsideWorld outside;
std::vector<tbd::city::OutsideIn> conns;
std::vector<int> superZone;               // per supercell: its zone index in `world` (-1 none), for the flow trips
uint64_t linesSig = 0;                     // the line inputs last given to the world
std::vector<float> overlay;                // per CitySim cell: riders from it (0..1), -1 no stop in reach
std::vector<uint8_t> superFlags;           // per supercell: P5 flag bits
size_t flagCursor = 0;                     // the lot pass, a group of lots a minute
float fareSlider = 1.0f;                   // the Transit panel: fare per boarding
float lineVeh[4] = {0, 0, 0, 0};           // the Transit panel: vehicles of the first four lines (0 = by length)
tbd::city::SimEventRing::Cursor migCursor = 0;
int64_t immigrantsToday = 0, emigrantsToday = 0;
double tickMs = 0, tickAvgMs = 0, tickWorstMs = 0;
int64_t lastDay = -1;
const tbd::city::SimFlags kMask = tbd::city::kFlagNoTransit | tbd::city::kFlagNoParking;

uint64_t SuperKey(int s) {
    const tbd::city::CellKey k = g_citySim.SuperKey(s);
    return (uint64_t)(uint32_t)k.x << 32 | (uint32_t)k.z;
}
uint64_t OutsideKey(uint64_t id) { return 1ull << 63 | id; }

// Vehicles a line runs in the sim: the slider's, else what its length asks for (transit.h BuildLegs' rule).
int SimVehicles(const tbd::city::Route& r) {
    const int set = book.Setting(r.id);
    if (set > 0) return set;
    float total = 0;
    for (float l : r.legLen) total += l;
    return std::clamp((int)(total / 350.0f) + 1, 1, 6);
}

// The passenger lines as LineInputs, in route order; the signature tells whether anything changed.
uint64_t BuildLines(std::vector<tbd::city::LineInput>& out) {
    using namespace tbd::city;
    out.clear();
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const Route& r : g_transit.routes) {
        if (!r.alive || r.kind != TKind::Passenger || r.stops.size() < 2) continue;
        LineInput li;
        li.id = r.id, li.mode = (uint8_t)r.mode, li.closed = r.closed, li.vehicles = SimVehicles(r);
        for (uint64_t sid : r.stops) {
            const Stop* st = g_transit.FindStop(sid);
            if (!st) { li.x.clear(); break; }
            li.x.push_back(std::round(st->p.x)), li.z.push_back(std::round(st->p.z));
        }
        if (li.x.size() < 2) continue;
        for (float l : r.legLen) li.legLen.push_back(std::round(l));
        mix(li.id), mix(li.mode), mix(li.closed), mix((uint64_t)li.vehicles);
        for (size_t i = 0; i < li.x.size(); ++i) mix((uint64_t)(int64_t)li.x[i]), mix((uint64_t)(int64_t)li.z[i]);
        for (float l : li.legLen) mix((uint64_t)(int64_t)l);
        out.push_back(std::move(li));
    }
    return h;
}

// The supercells as zones (centre of their people and jobs, residents, jobs, wealth mix) and the connections as zones
// (visitors without a car unless they came by road). shopOut: commercial jobs per zone input, for the tourists.
std::vector<double> zoneShop;
void BuildZones(std::vector<tbd::city::TransitZoneIn>& out) {
    using namespace tbd::city;
    out.clear(), zoneShop.clear();
    const int nsc = g_citySim.Supercells();
    for (int s = 0; s < nsc; ++s) {
        double w = 0, x = 0, z = 0, shop = 0;
        TransitZoneIn q;
        q.key = SuperKey(s);
        float wealth[4] = {0, 0, 0, 0};
        uint32_t pop = 0, jobs = 0;
        for (int c : g_citySim.SuperCellList(s)) {
            const CellSim& cs = g_citySim.sim[(size_t)c];
            const uint32_t cj = (uint32_t)cs.jobCap[0] + cs.jobCap[1] + cs.jobCap[2];
            const double cw = (double)cs.Population() + cj;
            const CellKey k = g_citySim.cells.cells[(size_t)c].key;
            x += cw * (k.x + 0.5) * kCityCellSize, z += cw * (k.z + 0.5) * kCityCellSize, w += cw;
            pop += cs.Population(), jobs += cj, shop += cs.jobCap[kJobCommercial];
            for (int b = 0; b < 4; ++b) wealth[b] += cs.wealth[b];
        }
        if (w <= 0) continue;
        q.x = std::round((float)(x / w)), q.z = std::round((float)(z / w)), q.pop = pop, q.jobs = jobs;
        for (int b = 0; b < 4; ++b) q.wealth[b] = wealth[b];
        out.push_back(q), zoneShop.push_back(shop);
    }
    for (const OutsideIn& o : conns) {
        TransitZoneIn q;
        q.key = OutsideKey(o.id), q.x = std::round(o.x), q.z = std::round(o.z);
        q.wealth[0] = 0, q.wealth[1] = 1, q.wealth[2] = 2, q.wealth[3] = 1; // visitors: comfortable on the whole
        q.carOwn = o.kind == 0 ? 1.0f : 0.0f;                              // by train, ship or plane: no car here
        out.push_back(q), zoneShop.push_back(0);
    }
}

bool HasDone(tbd::city::Model m) {
    for (const tbd::city::Placed& b : g_transport.items)
        if (b.alive && b.state == tbd::city::BState::Done && tbd::city::Defs()[b.def].model == m) return true;
    return false;
}
void BuildConns() {
    using namespace tbd::city;
    conns.clear();
    const bool station = HasDone(Model::TrainStation), harbour = HasDone(Model::PassengerHarbor);
    const bool air = HasDone(Model::SmallAirfield) || HasDone(Model::RegionalAirport);
    for (const Outside& o : g_transit.outsides) {
        if (!o.alive) continue;
        OutsideIn c;
        c.id = o.id, c.kind = (uint8_t)o.kind, c.linked = o.linked, c.x = o.p.x, c.z = o.p.z;
        c.terminal = o.kind == OutKind::Road || (o.kind == OutKind::Rail && station) || (o.kind == OutKind::Sea && harbour) || (o.kind == OutKind::Air && air);
        conns.push_back(c);
    }
}
int CountTaxis() {
    int n = 0;
    for (const tbd::city::Placed& b : g_transport.items)
        if (b.alive && b.state == tbd::city::BState::Done && std::strstr(tbd::city::Defs()[b.def].name, "Taxi")) n += tbd::city::Defs()[b.def].vehicles;
    return n;
}

// The commute links (both ends supercells), then the tourists: yesterday's per connection, to the four zones with the
// most shops by distance (largest-remainder split).
void BuildLinks(const std::vector<tbd::city::TransitZoneIn>& zin, std::vector<tbd::city::TransitLinkIn>& out) {
    using namespace tbd::city;
    out.clear();
    g_citySim.ForEachLink([&](int h, int j, int32_t n) {
        if (h != j && n > 0) out.push_back({SuperKey(h), SuperKey(j), (float)n});
    });
    for (const OutsideFlow& f : outside.flows) {
        if (f.tourists <= 0) continue;
        std::vector<std::pair<double, size_t>> best;
        for (size_t i = 0; i < zin.size(); ++i)
            if (zoneShop[i] > 0) best.push_back({-zoneShop[i] * std::exp(-std::hypot(zin[i].x - f.x, zin[i].z - f.z) / 3000.0), i});
        std::sort(best.begin(), best.end());
        if (best.size() > 4) best.resize(4);
        std::vector<double> w;
        for (const auto& b : best) w.push_back(-b.first);
        std::vector<int64_t> part;
        Apportion(f.tourists, w, part);
        for (size_t k = 0; k < best.size(); ++k)
            if (part[k] > 0) out.push_back({OutsideKey(f.id), zin[best[k].second].key, (float)part[k]});
    }
}

std::vector<tbd::city::TransitZoneIn> zin; // the hour's zones (:00), reused by the links (:20)
std::vector<tbd::city::TransitLinkIn> lin;
std::vector<tbd::city::LineInput> lineIn;
uint64_t linesTransitVer = ~0ull, bookVer = 0, linesBookVer = ~0ull, linesRoadVer = ~0ull;
bool quietLines = true; // the first lines after a reset or load are what the city had: no "line opened" news

void ApplyLines(int64_t minute) {
    using namespace tbd::city;
    const bool due = minute % 10 == 3; // and every 10 minutes: the legs re-route a frame after a road edit
    if (!due && linesTransitVer == g_transit.version && linesBookVer == bookVer && linesRoadVer == g_roads.net.version) return;
    linesTransitVer = g_transit.version, linesBookVer = bookVer, linesRoadVer = g_roads.net.version; // roads: the legs re-route
    const uint64_t sig = BuildLines(lineIn);
    if (sig == linesSig && world.lineIn.size() == lineIn.size()) return;
    linesSig = sig;
    const bool quiet = quietLines;
    quietLines = false;
    for (const LineInput& l : lineIn) { // opened: an id the world did not have
        bool had = false;
        for (const LineInput& o : world.lineIn) had |= o.id == l.id;
        if (!had && !quiet) g_simEvents.Push(SimEventType::LineOpened, minute, 0, 0, (int64_t)l.id);
    }
    for (const LineInput& o : world.lineIn) {
        bool has = false;
        for (const LineInput& l : lineIn) has |= o.id == l.id;
        if (!has && !quiet) g_simEvents.Push(SimEventType::LineClosed, minute, 0, 0, (int64_t)o.id);
    }
    world.SetLines(lineIn);
    book.Keep(world.net);
}

// P5's bits per supercell (no stop in reach while the city has lines; drivers cannot park), then onto the lots in
// fifteen groups (minutes :41 .. :55), through the shared table by BuildingId.
void SuperFlagsPass() {
    using namespace tbd::city;
    const int nsc = g_citySim.Supercells();
    superFlags.assign((size_t)nsc, 0);
    superZone.assign((size_t)nsc, -1);
    const bool lines = !world.net.lines.empty();
    for (int s = 0; s < nsc; ++s) {
        const int z = world.Zone(SuperKey(s));
        superZone[(size_t)s] = z;
        if (z < 0) continue;
        const TransitZone& Z = world.zones[(size_t)z];
        superFlags[(size_t)s] = (uint8_t)((lines && Z.pop > 0 && Z.access.empty() ? 1 : 0) | (Z.parking >= 2 ? 2 : 0));
    }
}
void FlagGroup(int group, int groups) {
    using namespace tbd::city;
    const auto& lots = g_zones.lots;
    const auto& types = ZoneTypes();
    const size_t per = (lots.size() + (size_t)groups - 1) / (size_t)groups;
    for (size_t i = (size_t)group * per; i < lots.size() && i < (size_t)(group + 1) * per; ++i) {
        const Lot& l = lots[i];
        if (!l.alive || !l.Built() || l.type < 0 || l.type >= (int)types.size()) continue;
        const BuildingId b = g_simBuildings.Find(BOrigin::Lot, l.id);
        if (!b.Valid()) continue;
        const int c = g_citySim.cells.Find(CellKeyAt(l.c.x, l.c.z));
        const int s = c >= 0 ? g_citySim.SuperOf(c) : -1;
        const uint8_t f = s >= 0 && (size_t)s < superFlags.size() ? superFlags[(size_t)s] : 0;
        const ZoneCat cat = types[(size_t)l.type].cat;
        SimFlags bits = 0;
        if ((f & 1) && cat != ZoneCat::Industrial) bits |= kFlagNoTransit;
        if ((f & 2) && (cat == ZoneCat::Commercial || cat == ZoneCat::Office)) bits |= kFlagNoParking;
        g_simFlagTable.Write(b, kMask, bits);
    }
}

// The ridership overlay: riders a day from each supercell (both ways), relative to the busiest; -1 out of reach.
void OverlayPass() {
    using namespace tbd::city;
    std::vector<double> riders(world.zones.size(), 0.0);
    for (const TransitLink& l : world.links) {
        const double r = (double)world.TransitRiders(l);
        riders[(size_t)l.from] += r, riders[(size_t)l.to] += r;
    }
    double mx = 1.0;
    for (double r : riders) mx = std::max(mx, r);
    overlay.assign(g_citySim.sim.size(), -1.0f);
    for (size_t c = 0; c < overlay.size(); ++c) {
        const int s = g_citySim.SuperOf((int)c);
        const int z = s >= 0 && (size_t)s < superZone.size() ? superZone[(size_t)s] : -1;
        if (z >= 0 && !world.zones[(size_t)z].access.empty()) overlay[c] = (float)(riders[(size_t)z] / mx);
    }
}

// Midnight: the day's book (fares into the treasury), the outside connections' day, crowded lines.
void Day(int64_t minute) {
    using namespace tbd::city;
    OutsideDayIn d;
    for (const TransitZoneIn& q : zin) d.residents += q.pop;
    for (double s : zoneShop) d.shopJobs += (int64_t)s;
    d.immigrants = immigrantsToday, d.emigrants = emigrantsToday;
    if (g_simPhaseTune.goods.enabled != 0.0)
        for (int g = 0; g < kGoodCount; ++g) d.cargoImport += sim_p6::world.last.imported[(size_t)g], d.cargoExport += sim_p6::world.last.exported[(size_t)g];
    BuildConns();
    outside.Day(conns, d);
    immigrantsToday = emigrantsToday = 0;
    book.Day(world.net, world.ledger, world.p.fare * world.p.fareMul);
    g_simEcon.treasury += book.FaresYesterday(); // fares only: the vehicles' running cost is shown, P7 owns expenses
    for (const LineDay& l : book.last)
        if (l.riders > 0 && l.served * 10 < l.riders * 9) g_simEvents.Push(SimEventType::LineCrowded, minute, 0, 0, (int64_t)l.id);
}

void CountMigration() {
    tbd::city::SimEvent e;
    while (g_simEvents.Read(migCursor, e)) {
        if (e.type == tbd::city::SimEventType::Immigrated) immigrantsToday += std::max<int64_t>(1, e.value);
        else if (e.type == tbd::city::SimEventType::Emigrated) emigrantsToday += std::max<int64_t>(1, e.value);
    }
}

// The "transit" system, every game minute. Each step runs at its own minute of the hour (no two in one tick).
void Tick(const tbd::city::SimTick& t) {
    const double a = g_sim.nowMs();
    const int64_t m = t.minute;
    const int mh = (int)(((m % 60) + 60) % 60);
    CountMigration();
    ApplyLines(m);
    const float mul = g_cellFactors.Mods(0).transitFare;
    world.SetFare(fareSlider, mul);
    if (mh == 0) {
        BuildConns();
        BuildZones(zin);
        world.SetZones(zin);
        world.SetTaxis(CountTaxis());
    } else if (mh == 20) {
        if (zin.empty()) BuildConns(), BuildZones(zin), world.SetZones(zin);
        BuildLinks(zin, lin);
        world.SetLinks(lin);
    } else if (mh == 40) {
        world.SummarizeParking();
        SuperFlagsPass();
        OverlayPass();
    } else if (mh > 40 && mh <= 55) {
        FlagGroup(mh - 41, 15);
    }
    world.Tick(m);
    if (m / 1440 != lastDay && mh == 5) { // just after midnight's tick (P1's day rolled over)
        if (lastDay >= 0) Day(m);
        lastDay = m / 1440;
    }
    const double ms = g_sim.nowMs() - a;
    tickMs = ms, tickAvgMs += (ms - tickAvgMs) * 0.01, tickWorstMs = std::max(tickWorstMs, ms);
}

// Dev check (TBD_SIM_VERIFY=1): once a game day, a drained world must equal a full recompute.
void VerifyDaily(int64_t m) {
    static const bool on = sim_host::EnvDouble("TBD_SIM_VERIFY", 0.0) > 0.0;
    if (!on || m % 1440 != 720 || world.Pending()) return;
    const double a = g_sim.nowMs();
    const int bad = world.Verify();
    tbd::Log("Transit: verify %s (%d mismatches, %zu links, %.2f ms)", bad ? "FAILED" : "ok", bad, world.links.size(), g_sim.nowMs() - a);
}
} // namespace sim_p5

// ---- for the other systems
// city_sim_flow.inl SimBuildTrips: the car + taxi share of the commute from supercell h to j (fallback: none known).
float SimTransitCommuteCarShare(int h, int j, float fallback) {
    using namespace sim_p5;
    if (g_simPhaseTune.move.enabled == 0.0 || world.links.empty()) return fallback;
    const int i = world.Link(SuperKey(h), SuperKey(j));
    if (i < 0 || !world.links[(size_t)i].evaluated) return fallback;
    const tbd::city::TransitLink& l = world.links[(size_t)i];
    return (float)(l.share[tbd::city::kModeCar] + l.share[tbd::city::kModeTaxi]) / 1000.0f;
}
// P6 may read: the freight capacity of the outside connections (0.1 t a day), and yesterday's flows per connection.
int64_t SimTransitCargoCapacity() {
    int64_t n = 0;
    for (const tbd::city::OutsideFlow& f : sim_p5::outside.flows) n += f.cargoCap;
    return n;
}
const std::vector<tbd::city::OutsideFlow>& SimTransitOutsideFlows() { return sim_p5::outside.flows; }
// P1 may read: where yesterday's immigrants arrived (one flow per connection, its point and count).
const tbd::city::OutsideWorld& SimTransitOutside() { return sim_p5::outside; }

// ---- UI: the Transit panel, the Outside connections panel, the ridership overlay
namespace sim_p5 {
const tbd::city::Route* RouteById(uint64_t id) {
    for (const tbd::city::Route& r : g_transit.routes) if (r.id == id && r.alive) return &r;
    return nullptr;
}
std::string Money(int64_t cents) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", (long long)(cents / 100));
    return b;
}
void FillTransit(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.sub = "Lines, riders and how people get around";
    char b[160];
    const int64_t riders = book.ridersDays.empty() ? 0 : book.ridersDays.back();
    std::snprintf(b, sizeof(b), "%zu lines, %lld riders yesterday", world.net.lines.size(), (long long)riders);
    p.Stat("Public transport", b);
    std::vector<float> rd;
    for (int64_t v : book.ridersDays) rd.push_back((float)v);
    p.Spark("Riders per day", rd, std::to_string((long long)riders));
    static const tbd::gui::Color kCol[kModeCount] = {tbd::gui::Rgba(74, 222, 128), tbd::gui::Rgba(255, 159, 10), tbd::gui::Rgba(10, 132, 255), tbd::gui::Rgba(255, 214, 10)};
    std::vector<tbd::gui::SimStackPart> parts;
    for (int m = 0; m < kModeCount; ++m) parts.push_back({TravelModeName(m), world.Share(m), kCol[m]});
    p.Stacked("How people travel", parts);
    p.Stat("Fares yesterday", "+" + Money(book.FaresYesterday()), book.FaresYesterday() > 0 ? 1 : 0);
    p.Stat("Vehicle running cost", Money(book.UpkeepYesterday()) + " a day (not charged yet)");
    p.Slider("Fare per boarding", &fareSlider, 0.0f, 5.0f, 0.25f, "%.2f");
    std::vector<std::pair<std::string, std::string>> rows;
    for (size_t l = 0; l < world.net.lines.size(); ++l) {
        const TransitLine& L = world.net.lines[l];
        const Route* r = RouteById(L.id);
        const LineRiders& R = world.ledger.lines[l];
        std::snprintf(b, sizeof(b), "%lld riders, busiest %d%% full, every %.0f min, %d vehicles", (long long)R.riders,
                      (int)std::lround(100.0 * (double)R.MaxLoad() / (double)std::max<int64_t>(1, L.capPerDir)), L.headwayMin, L.vehicles);
        rows.push_back({r ? r->name : std::string(TModeName((TMode)L.mode)), b});
        if (l < 4) {
            lineVeh[l] = (float)L.vehicles;
            p.Slider((r ? r->name : std::string("Line")) + " vehicles", &lineVeh[l], 1.0f, 20.0f, 1.0f);
        }
    }
    p.List("Lines", rows, 8);
    const int64_t taxiWant = world.modeMille[kModeTaxi] / 1000, taxiCan = (int64_t)world.p.taxis * world.p.tripsPerTaxi;
    std::snprintf(b, sizeof(b), "%d in service, %lld trips wanted, %lld possible", world.p.taxis, (long long)taxiWant, (long long)taxiCan);
    p.Stat("Taxis", b, world.p.taxis > 0 && taxiWant > taxiCan ? -1 : 0);
    int noPark = 0, noStop = 0;
    for (uint8_t f : superFlags) noPark += (f & 2) != 0, noStop += (f & 1) != 0;
    std::snprintf(b, sizeof(b), "%d areas without a stop, %d without parking", noStop, noPark);
    p.Stat("Problems", b, noStop + noPark ? -1 : 0);
    std::snprintf(b, sizeof(b), "transit tick %.3f ms avg, %.3f worst; %d links waiting, %lld evaluated", tickAvgMs, tickWorstMs, world.dirtyCount, (long long)world.evaluations);
    p.Caption(b);
}
void ChangedTransit(const tbd::gui::SimPanelSpec&, int, void*) {
    for (size_t l = 0; l < world.net.lines.size() && l < 4; ++l) {
        const int v = (int)std::lround(lineVeh[l]);
        if (v != world.net.lines[l].vehicles) book.Set(world.net.lines[l].id, std::clamp(v, 1, 20)), ++bookVer;
    }
}
void FillOutside(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.sub = "People and cargo through the outside connections (yesterday)";
    char b[200];
    int64_t im = 0, em = 0, tour = 0, ci = 0, co = 0;
    std::vector<std::pair<std::string, std::string>> rows;
    for (const OutsideFlow& f : outside.flows) {
        im += f.immigrants, em += f.emigrants, tour += f.tourists, ci += f.cargoIn, co += f.cargoOut;
        std::string name = OutKindName((OutKind)std::min<int>(f.kind, 3));
        for (const Outside& o : g_transit.outsides) if (o.id == f.id && !o.name.empty()) name += " " + o.name;
        std::snprintf(b, sizeof(b), "%d moved in, %d left, %d tourists, %.1f t in, %.1f t out", f.immigrants, f.emigrants, f.tourists, f.cargoIn / 10.0, f.cargoOut / 10.0);
        rows.push_back({name, b});
    }
    std::snprintf(b, sizeof(b), "%lld in, %lld out", (long long)im, (long long)em);
    p.Stat("People moving", b);
    p.Stat("Tourists", std::to_string((long long)tour));
    std::vector<float> td;
    for (int32_t v : outside.touristDays) td.push_back((float)v);
    p.Spark("Tourists per day", td);
    std::snprintf(b, sizeof(b), "%.1f t in, %.1f t out, room for %.0f t", ci / 10.0, co / 10.0, SimTransitCargoCapacity() / 10.0);
    p.Stat("Cargo", b);
    p.List("Connections", rows, 8);
}
float OverlayCell(void*, int cell, float, float) { return cell >= 0 && (size_t)cell < overlay.size() ? overlay[(size_t)cell] : -1.0f; }
} // namespace sim_p5

// ---- the hooks
void SimP5Init() {
    sim_p5::migCursor = g_simEvents.Now();
    g_sim.Register("transit", 1, 1, [](const tbd::city::SimTick& t) {
        sim_p5::Tick(t);
        sim_p5::VerifyDaily(t.minute);
    });
    tbd::gui::RegisterOverlayProvider(tbd::gui::Overlay::TransitRidership, sim_p5::OverlayCell, nullptr, "No stop  ..  Busiest (riders a day)");
    tbd::Log("Sim: P5 transit on (%d dirty links a minute at most, %d zones a minute)", sim_p5::world.p.linksPerTick, sim_p5::world.p.zonesPerTick);
}

// Per frame: the vehicles each line draws (the sim's count x the preset's density, at least one), and the herds' car
// share from the mode model once it has results.
void SimP5Frame(float /*realDt*/) {
    using namespace sim_p5;
    for (tbd::city::Route& r : g_transit.routes) {
        if (!r.alive || r.kind != tbd::city::TKind::Passenger) continue;
        const int drawn = std::clamp((int)std::lround((float)SimVehicles(r) * g_simPreset.carDensity), 1, 12);
        if (r.vehicles != drawn) r.vehicles = drawn;
    }
    static uint64_t seen = ~0ull;
    if (world.version != seen && !world.links.empty()) {
        seen = world.version;
        const float car = world.Share(tbd::city::kModeCar) + world.Share(tbd::city::kModeTaxi);
        herds_host::herds.carShare = std::clamp(car, 0.05f, 1.0f);
    }
}

void SimP5Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    SimChunkDef line, mode, outc;
    mode.fourcc = simchunk::MODE, mode.version = kTransitModeVersion;
    mode.save = [](ByteWriter& w) { WriteTransitMode(w, sim_p5::world); };
    mode.load = [](ByteReader& r, uint16_t) { return ReadTransitMode(r, sim_p5::world); };
    mode.fresh = [] { sim_p5::world.Clear(); };
    line.fourcc = simchunk::LINE, line.version = kTransitLineVersion;
    line.save = [](ByteWriter& w) { WriteTransitLine(w, sim_p5::book); w.F32(sim_p5::fareSlider); };
    line.load = [](ByteReader& r, uint16_t) {
        if (!ReadTransitLine(r, sim_p5::book)) return false;
        sim_p5::fareSlider = std::clamp(r.F32(), 0.0f, 5.0f);
        return r.ok;
    };
    line.fresh = [] { sim_p5::book.Clear(), sim_p5::fareSlider = 1.0f; };
    outc.fourcc = simchunk::OUTC, outc.version = kTransitOutcVersion;
    outc.save = [](ByteWriter& w) { WriteTransitOutc(w, sim_p5::outside); };
    outc.load = [](ByteReader& r, uint16_t) { return ReadTransitOutc(r, sim_p5::outside); };
    outc.fresh = [] { sim_p5::outside.Clear(); };
    reg.Register(line), reg.Register(mode), reg.Register(outc);
}

void SimP5Load(const tbd::city::SimLoadReport& rep) {
    using namespace sim_p5;
    const tbd::city::SimChunkResult* m = rep.Find(tbd::city::simchunk::MODE);
    if (!(m && m->status == tbd::city::SimChunkStatus::Loaded)) world.Clear();
    // the lines are compared with the game's on the next tick (paths keep their line by id); zones and links refresh
    linesTransitVer = linesBookVer = linesRoadVer = ~0ull, linesSig = 0, ++bookVer, quietLines = true;
    zin.clear(), lin.clear(), superFlags.clear(), superZone.clear(), overlay.clear();
    lastDay = -1, immigrantsToday = emigrantsToday = 0, migCursor = g_simEvents.Now();
    tickWorstMs = 0;
    if (m && m->status == tbd::city::SimChunkStatus::Loaded)
        tbd::Log("Transit: %zu zones, %zu links, %zu lines loaded (car %.2f, transit %.2f)", world.zones.size(), world.links.size(), world.net.lines.size(),
                 world.Share(tbd::city::kModeCar), world.Share(tbd::city::kModeTransit));
}

void SimP5Panels() {
    tbd::gui::SimPanelEntry e;
    e.id = "p5.transit", e.name = "Transit", e.icon = "bus", e.fill = sim_p5::FillTransit, e.changed = sim_p5::ChangedTransit;
    tbd::gui::RegisterSimPanel(e);
    tbd::gui::SimPanelEntry o;
    o.id = "p5.outside", o.name = "Outside connections", o.icon = "cargo", o.fill = sim_p5::FillOutside;
    tbd::gui::RegisterSimPanel(o);
}
