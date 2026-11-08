// ------------------------------------------------------------------- neighbourhoods, herds and visible children (host side)
// Included by main.cpp right after city_sim.inl. The logic is headless and tested: city/sim_hoods.h (neighbourhoods and
// local-first matching), sim_herds.h (demand over the day, herds), sim_routes.h (link table, route trees, alternatives),
// sim_children.h (cars near the camera, promotion). This file only wires them to the game:
//
//   systems (registered on the first HerdsSync, run by the scheduler in city_sim.inl):
//     hoods     every 10 game minutes: link table when the road network changed, hoods when roads or occupied cells changed,
//               roll-up of the cell counters, hood commute / school / shop links
//     herds     every 15 game minutes (a window): the day's demand curve becomes herds on their routes
//     herdmove  every game minute: herds advance by the link speeds (BPR from their own loads) and dissolve at the destination
//   per frame (HerdsSync, after SimSync): the cars near the camera (sim_children.h: every route's releases become a stream
//   of trips on the cars' own clock; trips near the camera are materialised as IDM cars out of view or at their driveway,
//   and dropped back to their trip out of view; one pool, no allocation), follow camera
//   drawing (HerdsDraw, next to SimAgentsDraw): one instanced draw per car model, fades as scale, near cars cast shadows;
//   it replaces the flow-driven cosmetic cars
//   town traffic (sim_town_traffic.h, rebuilt with the hoods): the trips inside one hood, the outside connections (dead-end
//   roads at the map edge and the player's road outside connections) and the parked cars. Its node links go to the herds
//   with the hood links and, as peak-hour trips, to the flow solve (SimAppendTownTrips, called by city_sim.inl).
//
// Environment (dev):
//   TBD_HERDS=0          leave the old cosmetic traffic in charge
// Remote pipe (city_overlays.inl forwards them): sim|herds  stats;  sim|follow[|off]  promote the car nearest the camera
// target and keep the camera on it;  sim|clock|<hour>  set the game clock.

namespace herds_host {
tbd::city::SimHoods hoods;
std::vector<tbd::city::HoodLink> commute, school, shop;
tbd::city::LinkTable table;
tbd::city::LinkBuild build;
tbd::city::HerdSystem herds;
tbd::city::RoutePicker picker;
tbd::city::RouteCache trees;
tbd::city::ChildSystem children;
tbd::city::TownTraffic town; // local trips, outside connections, parked cars
std::vector<tbd::city::V2> townFlagged; // the road outside connections' points (reused)
constexpr int kMaxParked = 240;         // parked cars drawn near the camera
bool registered = false, enabled = true, tableReady = false;
uint64_t generation = ~0ull;
double childMs = 0, childAvgMs = 0, releaseMs = 0, moveAvgMs = 0, hoodMs = 0;
int follow = -1;
std::string followLine;
double releasedDay[tbd::city::kTripTypes] = {};
std::vector<tbd::city::CarInstance> moving, parked;
std::vector<float> rows;
uint32_t carsDrawn = 0;
float frameDt = 0; // HerdsSync's real seconds, for the curbside cars' fades in HerdsDraw

double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void Reset() {
    hoods = tbd::city::SimHoods();
    commute.clear(), school.clear(), shop.clear();
    table = tbd::city::LinkTable();
    tableReady = false;
    herds.SetTable(nullptr);
    picker.SetTable(nullptr);
    trees.Drop(0);
    children.pool.clear();
    {
        const tbd::city::TownTrafficParams keep = town.params;
        town = tbd::city::TownTraffic();
        town.params = keep;
    }
    follow = -1;
}

// Hood-level shopping: residents drive to the hoods with shop jobs, nearer and bigger first.
void BuildShopLinks() {
    using namespace tbd::city;
    shop.clear();
    const auto& H = hoods.hoods;
    for (const Hood& h : H) {
        const double trips = h.Population() * 0.25; // a quarter of a car trip per resident a day
        if (trips < 1.0) continue;
        double wsum = 0;
        for (const Hood& c : H)
            if (c.id != h.id && c.jobs[kJobCommercial] > 0) {
                const double d = Len(c.centre - h.centre);
                if (d < 3.0 * g_simTune.flowShopDecayMetres) wsum += c.jobs[kJobCommercial] * std::exp(-d / std::max(1.0, g_simTune.flowShopDecayMetres));
            }
        if (wsum <= 0) continue;
        for (const Hood& c : H)
            if (c.id != h.id && c.jobs[kJobCommercial] > 0) {
                const double d = Len(c.centre - h.centre);
                if (d >= 3.0 * g_simTune.flowShopDecayMetres) continue;
                const double w = c.jobs[kJobCommercial] * std::exp(-d / std::max(1.0, g_simTune.flowShopDecayMetres));
                shop.push_back({h.id, c.id, (float)(trips * w / wsum)});
            }
    }
}

// Kids to schools: local first. (Stub: school seats come from the school coverage of a cell, see SimHoods::RollUp.)
void BuildSchoolLinks() {
    using namespace tbd::city;
    school.clear();
    std::vector<float> need(hoods.hoods.size()), seats(hoods.hoods.size());
    for (size_t i = 0; i < hoods.hoods.size(); ++i) need[i] = (float)hoods.hoods[i].pop[kKids], seats[i] = (float)hoods.hoods[i].schoolSeats[0];
    MatchResult r;
    MatchLocalFirst(hoods.hoods, need, seats, nullptr, r);
    for (const HoodLink& l : r.assign)
        if (l.from != l.to) school.push_back(l);
}

void RebuildTable() {
    using namespace tbd::city;
    const auto& net = g_roads.net;
    FlowGraph local;
    const FlowGraph* g = g_simFlowJob->graph && g_simFlowJob->graph->version == net.version ? g_simFlowJob->graph.get() : nullptr;
    if (!g) local = FlowGraph::Build(net), g = &local;
    BuildLinkTable(net, *g, table, &build);
    town.SetTable(net, *g, table, build);
    herds.SetTable(&table);
    picker.SetTable(&table);
    picker.herds = &herds;
    trees.Drop(table.version);
    children.params.models = std::max(1, (int)g_simCars.size());
    children.SetTable(&table, &herds.routes);
    follow = -1;
    tableReady = !table.links.empty();
    tbd::Log("Herds: link table %zu links (%d pipes) from %zu segments", table.links.size(), (int)std::count_if(table.links.begin(), table.links.end(), [](const LinkInfo& l) { return l.pipe != 0; }),
             net.segs.size());
}

// system "hoods": every 10 game minutes
void HoodsTick(const tbd::city::SimTick&) {
    using namespace tbd::city;
    const double t0 = Now();
    const auto& net = g_roads.net;
    if (g_simSegGrid.builtFrom != net.version || net.segs.empty()) return; // the flow system builds the grid first
    if (table.version != net.version || !tableReady) RebuildTable();
    if (hoods.Stale(g_citySim, net)) {
        hoods.Build(g_citySim, net, g_simSegGrid);
        tbd::Log("Herds: %zu neighbourhoods (%s, %s ...)", hoods.hoods.size(), hoods.hoods.empty() ? "-" : hoods.hoods[0].name.c_str(), hoods.hoods.size() > 1 ? hoods.hoods[1].name.c_str() : "-");
    }
    hoods.RollUp(g_citySim);
    hoods.LinksFromSuper(g_citySim, commute);
    BuildSchoolLinks();
    BuildShopLinks();
    // town traffic: trips inside a hood, the outside connections, the parked cars
    townFlagged.clear();
    for (const Outside& o : g_transit.outsides)
        if (o.alive && o.kind == OutKind::Road) townFlagged.push_back(o.p);
    town.params.shopDecayMetres = (float)g_simTune.flowShopDecayMetres;
    town.params.models = std::max(1, (int)g_simCars.size());
    const int connsWere = town.stats.conns;
    town.Build(g_citySim, hoods, commute, net, g_simSegGrid, table, townFlagged, tbd::CityMap::kHalf, herds.carShare, herds.curves);
    if (town.stats.conns != connsWere)
        tbd::Log("Traffic: %d outside connections; a day: %.0f through, %.0f in, %.0f out, %.0f local vehicles (%zu node links)", town.stats.conns,
                 town.stats.throughPerDay, town.stats.inPerDay, town.stats.outPerDay, town.stats.localPerDay, town.stats.links);
    hoodMs = Now() - t0;
}

// system "herds": every 15 game minutes, the window's herds leave
void HerdsRelease(const tbd::city::SimTick& t) {
    using namespace tbd::city;
    if (!tableReady || (hoods.hoods.empty() && town.links.empty())) return; // an empty map still has its through traffic
    const double t0 = Now();
    const int tod = (int)std::fmod(g_clock.minutes, 1440.0);
    if (tod < kWindowMinutes) herds.NewDay();
    picker.BeginWindow(t.minute / kWindowMinutes);
    herds.ReleaseHerds(t.minute, tod, hoods.hoods, commute, school, shop, [](int a, int b, RouteChoice* out) { return picker.Pick(a, b, out); }, &town.links);
    releaseMs = Now() - t0;
}

void HerdsMove(const tbd::city::SimTick&) {
    if (!tableReady) return;
    const double t0 = Now();
    herds.AdvanceHerds(1.0f);
    moveAvgMs += (Now() - t0 - moveAvgMs) * 0.05;
}

void Init() {
    registered = true;
    enabled = sim_host::EnvDouble("TBD_HERDS", 1.0) > 0.0;
    picker.params.maxNewPerWindow = 32; // town traffic adds node pairs (local, outside): route them within the first windows
    g_sim.Register("hoods", 10, 1, HoodsTick); // with the herds off too: its town traffic feeds the flow's cosmetic cars
    if (!enabled) return;
    herds.seed = children.params.citySeed = 0x5EED;
    children.params.densityScale = (float)sim_host::EnvDouble("TBD_HERD_DENSITY", 1.0); // visible cars per vehicle in the herds
    g_sim.Register("herds", tbd::city::kWindowMinutes, 1, HerdsRelease);
    g_sim.Register("herdmove", 1, 1, HerdsMove);
    children.tenant = [](const tbd::city::Herd& h, bool home, uint64_t rnd, tbd::city::TenantSlot& out) {
        return tbd::city::PickTenantSlot(g_citySim, hoods, home ? h.homeHood : h.workHood, home, rnd, out);
    };
    children.hoodNode = [](int h) { return (size_t)h < hoods.hoods.size() ? hoods.hoods[(size_t)h].centroidNode : -1; };
    children.nextHop = [](int node, int dest) {
        if (!tableReady || node < 0 || dest < 0) return -1;
        const tbd::city::RouteTree& tr = trees.Ensure(table, dest); // a tree per destination, cached (LRU)
        return (size_t)node < tr.nextLink.size() ? tr.nextLink[(size_t)node] : -1;
    };
}
} // namespace herds_host

// city_sim.inl SimBuildTrips: the town traffic's links as peak-hour flow trips (only when built on this graph's network).
void SimAppendTownTrips(const tbd::city::FlowGraph& g, std::vector<tbd::city::Trip>& trips) {
    herds_host::town.FlowTrips(herds_host::herds.curves, g, trips);
}

// main.cpp: once per frame, after SimSync.
void HerdsSync(float realDt) {
    using namespace herds_host;
    if (!registered) Init();
    if (generation != g_cityGeneration) { // new map or a load wiped the city
        generation = g_cityGeneration;
        Reset();
    }
    if (!enabled) { g_herdsOwnCars = false; return; }
    g_herdsOwnCars = tableReady;
    frameDt = realDt;
    if (!tableReady || g_engine.camera.distance > 1400.0f) return;
    children.params.models = std::max(1, (int)g_simCars.size());
    const tbd::city::V2 focus{g_engine.camera.target.x, g_engine.camera.target.z};
    const double dt = g_clock.paused ? 0.0 : (double)realDt * (1.0 + 0.5 * g_clock.speed);
    children.params.radius = std::clamp(g_engine.camera.distance * 1.2f, 150.0f, 700.0f);
    children.params.watchRadius = std::clamp(g_engine.camera.distance * 0.08f, 25.0f, 80.0f);
    const double t0 = Now();
    children.Update(dt, realDt, focus, herds, g_clock.minutes);
    childMs = Now() - t0;
    childAvgMs += (childMs - childAvgMs) * 0.05;
    if (follow >= 0) { // the follow camera rides with the promoted car
        tbd::city::CarInstance ci;
        const tbd::city::Child& c = children.pool[(size_t)follow];
        if (c.active && children.Place(c, ci)) {
            g_engine.camera.target = {ci.p.x, ci.p.y, ci.p.z};
            g_engine.camera.distance = std::min(g_engine.camera.distance, 120.0f);
        } else {
            follow = -1;
        }
    }
}

// main.cpp: in the scene, right after SimAgentsDraw. One instanced draw per car model (moving and parked cars together).
void HerdsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj) {
    using namespace herds_host;
    carsDrawn = 0;
    if (!enabled || !tableReady || g_simCars.empty() || g_engine.camera.distance > 1400.0f) return;
    const tbd::city::V2 focus{g_engine.camera.target.x, g_engine.camera.target.z};
    children.Collect(moving, parked, focus);
    // curbside cars: home at night, work by day; a spot that changes in view fades
    town.Parked(focus, children.params.radius, g_clock.minutes, parked, kMaxParked, g_clock.paused ? 0.0f : frameDt);
    moving.insert(moving.end(), parked.begin(), parked.end());
    // the near ones cast shadows like the cosmetic cars (city_sim.inl SimAgentsCasters draws these rows next frame)
    g_simCarShadowRows.resize(g_simCars.size());
    const tbd::Vec3 eye = g_engine.camera.Eye();
    for (int m = 0; m < (int)g_simCars.size(); ++m) {
        rows.clear();
        for (const tbd::city::CarInstance& c : moving) {
            if (c.model != m) continue;
            float row[12];
            tbd::city::CarRow(c, row);
            rows.insert(rows.end(), row, row + 12);
            const tbd::Vec3 at{row[3], row[7], row[11]};
            if (c.scale > 0.5f && tbd::Dot(at - eye, at - eye) < kSimCarShadowRange * kSimCarShadowRange)
                g_simCarShadowRows[(size_t)m].insert(g_simCarShadowRows[(size_t)m].end(), row, row + 12);
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
        carsDrawn += n;
    }
    r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
}

// city_overlays.inl's SimRemote forwards sim|herds, sim|follow and sim|clock here.
bool HerdsRemote(const std::string& sub, const std::string& arg, std::string& out) {
    using namespace herds_host;
    using namespace tbd::city;
    char b[900];
    if (sub == "density") {
        children.params.densityScale = (float)std::clamp(atof(arg.c_str()), 0.05, 4.0);
        out = "density " + arg;
        return true;
    }
    if (sub == "clock") {
        const double hour = std::clamp(atof(arg.c_str()), 0.0, 23.99);
        g_clock.minutes = std::floor(g_clock.minutes / 1440.0) * 1440.0 + hour * 60.0;
        out = "clock " + arg;
        return true;
    }
    if (sub == "follow") {
        if (arg == "off") { follow = -1; out = "follow off"; return true; }
        if (!tableReady) { out = "no herd system yet"; return false; }
        const V2 focus{g_engine.camera.target.x, g_engine.camera.target.z};
        int i = children.Nearest(focus, 250.0f);
        if (i < 0) { out = "no car within 250 m of the camera target"; return false; }
        if (!children.pool[(size_t)i].promoted && !children.Promote(i)) { out = "promotion cap reached"; return false; }
        follow = i;
        const Child& c = children.pool[(size_t)i];
        std::string who = "anonymous";
        if (c.slot.cell >= 0) {
            const int pi = g_simPeople.Materialize(g_citySim, c.slot.cell, c.slot.index, g_sim.Minute());
            if (pi >= 0) who = SimPeople::Name(g_simPeople.people[(size_t)pi]);
        }
        snprintf(b, sizeof(b), "following %s: model %d speed x%.2f from %s to %s (link %d, %.0f m)", who.c_str(), (int)c.look.model, c.look.speedFactor,
                 (size_t)c.destHood < hoods.hoods.size() ? "its hood" : "-", (size_t)c.destHood < hoods.hoods.size() ? hoods.hoods[(size_t)c.destHood].name.c_str() : "-", c.link, c.s);
        followLine = out = b;
        return true;
    }
    if (arg == "list" || arg == "goto") { // where the herds are (and, for goto, the camera on the biggest one)
        std::string s;
        const Herd* big = nullptr;
        V2 bigAt{};
        float bigY = 0;
        int n = 0;
        for (const Herd& h : herds.herds) {
            if (!h.alive || h.wait > 0 || h.route == ~0u) continue;
            uint32_t idx;
            float into;
            herds.routes.Locate(table, h.route, h.progress, idx, into);
            V2 p, d;
            float y;
            if (!table.PointAt(herds.routes.LinkAt(h.route, idx), into, p, y, d)) continue;
            if (n++ < 12) {
                snprintf(b, sizeof(b), "[t%d %.0f veh %.0f/%.0f m at %.0f %.0f hood %d->%d link %d%s] ", (int)h.type, h.size, h.progress, h.distance, p.x, p.z, h.from, h.to,
                         herds.routes.LinkAt(h.route, idx), table.links[(size_t)herds.routes.LinkAt(h.route, idx)].pipe ? " pipe" : "");
                s += b;
            }
            if (!big || h.size > big->size) big = &h, bigAt = p, bigY = y;
        }
        if (arg == "goto" && big) g_engine.camera.target = {bigAt.x, bigY, bigAt.z};
        out = s.empty() ? "no herds on the road" : s;
        return true;
    }
    // stats
    int jammed = 0;
    float worst = 1.0f;
    for (size_t l = 0; l < table.links.size() && l < herds.loadPrev.size(); ++l) {
        const float v = herds.LinkSpeed((int)l) / table.links[l].freeMpm;
        if (v < 0.7f) ++jammed;
        worst = std::min(worst, v);
    }
    const HerdStats& st = herds.stats;
    snprintf(b, sizeof(b),
             "hoods %zu links %zu (commute %zu school %zu shop %zu) | herds live %zu released today %llu | trips commute %.0f home %.0f school %.0f shop %.0f chain %.0f | slow links %d (worst x%.2f) | "
             "cars %d vis %d promoted %d parked %d spawned %llu | pairs %zu deferred %llu | ms children %.3f herds-move %.3f | drawn %u | clock %02d:%02d",
             hoods.hoods.size(), table.links.size(), commute.size(), school.size(), shop.size(), herds.Live(), (unsigned long long)st.herdsReleased, st.released[kTripCommute],
             st.released[kTripWorkHome], st.released[kTripSchool], st.released[kTripShop], st.delivered[kTripChain], jammed, worst, children.stats.active, children.stats.visible,
             children.stats.promoted, children.stats.parked, (unsigned long long)children.stats.spawned, picker.Pairs(), (unsigned long long)picker.pairsDeferred, childAvgMs, moveAvgMs, carsDrawn,
             g_clock.Hour(), g_clock.Minute());
    out = b;
    const TownTrafficStats& ts = town.stats;
    snprintf(b, sizeof(b), " | outside %d: a day %.0f through %.0f in %.0f out, local %.0f, %zu node links | through released %.0f freight %.0f | parked %d",
             ts.conns, ts.throughPerDay, ts.inPerDay, ts.outPerDay, ts.localPerDay, ts.links, st.released[kTripThrough], st.released[kTripFreight], ts.parkedShown);
    out += b;
    const ChildStats& cs = children.stats;
    snprintf(b, sizeof(b), " | streams %d budget x%.2f: departures %llu arrivals %llu dropped %llu faded in %llu skipped %llu stop-line waits %llu",
             cs.streams, children.BudgetScale(), (unsigned long long)cs.departures, (unsigned long long)cs.arrivals, (unsigned long long)cs.dropped,
             (unsigned long long)cs.fadedIn, (unsigned long long)cs.skipped, (unsigned long long)cs.waits);
    out += b;
    return true;
}
