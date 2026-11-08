// ------------------------------------------------------------------- the city game: new random map, default infrastructure, game saves
// Included by main.cpp after city_industry.inl. A game save (saves\city_slotN.tbdgame, plain text, one record per line, easy to
// diff) holds the whole city: map seed, clock, camera, terrain edits (a .hfd beside it), the road / rail network, painted zones,
// transport buildings, utilities (sources + lines), industry sites, transit lines + outside connections + the people who moved in.
// Saved since the city sim (S8): the buildings the zones grew (G, as they stand), the sim's treasury, taxes, funding and clock
// (M, versioned) and the tracked people (Q); everyone else is a counter and comes back from the lots.
// Not saved (they restart): construction cranes, traffic vehicles in motion, garbage stock, the flow solve.
//   G|id|type|w d|level|progress duration|cx cz|fx fz|y|model path|cell x z;cell x z;...
//   M|simVersion|treasury cents|tax r c o i (tenths of a %)|funding x6|sim minute|demand r c o i|ups downs abandoned
//   Q|id|seed|home lot|band|age|edu|wealth|health|happiness|working|job cell x z|job kind|touched
//   H|mapKind|seed|minutes|speed|cam x y z|dist|pitch|yaw|residents|commuters|totalPeople|totalCargo|cityName
//   T|relief (CityMap::Height's hills scale; missing in older saves = 1, the original map)
//   N|alive|x y z|grounded                       S|alive|a|b|c1x c1z|c2x c2z|profile name
//   Z|x z|zone                                   B|def|cx cz|fx fz|y|track|platformInner|state|timer|progress|haveShore|sx sz
//   U|def|cx cz|fx fz|y                          L|kind|ax az|bx bz|elev
//   D|kind|cx cz|fx fz|y|stock|x z;x z;...       P|id|mode|kind|px pz|dx dz|y|outside|alive
//   R|id|mode|kind|closed|alive|name|id,id,...   O|id|kind|px pz|dx dz|y|alive|stopIndex|name|pIn pOut cIn cOut
//   X|transit next id
#include <fstream>

struct PendingZone {
    tbd::city::V2 p;
    uint8_t zone;
};
std::vector<PendingZone> g_pendingZones; // painted cells waiting for the zone grid to rebuild from the loaded roads
// Grown lots (G records) waiting for the zones to be restored: their cells are found by position, like the paint.
struct PendingLot {
    tbd::city::Lot lot;
    std::vector<tbd::city::V2> cellCentres;
};
std::vector<PendingLot> g_pendingLots;
// The city sim's records (M, Q): city_sim.inl writes them and takes them back after the sim has reset for the load.
void SimSaveLines(std::ostringstream& o);
void SimLoadLine(const std::vector<std::string>& f);
void SimLotsRestored();
int g_gameAction = 0, g_gameSlot = 0;

std::wstring GameSavePath(int slot, const wchar_t* ext) {
    const std::wstring dir = tbd::Join(g_engine.projectFolder.empty() ? tbd::ExeDirectory() : g_engine.projectFolder, L"saves");
    { std::error_code ec; std::filesystem::create_directory(tbd::platform::FsPath(dir), ec); }
    return tbd::Join(dir, std::wstring(L"city_slot") + std::to_wstring(slot + 1) + ext);
}

uint64_t g_cityGeneration = 0; // bumped when the city is wiped (new map, load): the city sim starts over (city_sim.inl)
// Lane-metres of the map's own corridor (DefaultInfrastructure): the city pays no upkeep for them (sim_budget.h
// PaidLaneMetres). Saved in the M record (city_sim.inl); a save from before that counts the corridor's profiles.
double g_cityMapLaneMetres = 0.0;
bool g_cityMapLaneKnown = false; // a load: the M record had it
double SimRoadLaneMetres();      // city_sim.inl

// Everything the city owns goes back to empty (the map stays).
void ClearCityWorld() {
    ++g_cityGeneration;
    g_cityMapLaneMetres = 0.0, g_cityMapLaneKnown = false;
    for (const tbd::city::Placed& b : g_transport.items) {
        g_transportCache.erase(b.id);
        if (g_transportGlbShown.erase(b.id)) UtilDeleteEntity("transport_" + std::to_string(b.id));
    }
    for (const auto& se : g_stopEntity)
        if (!se.second.empty()) UtilDeleteEntity(se.second);
    g_stopEntity.clear();
    g_transport = tbd::city::TransportWorld();
    g_transportGlbShown.clear();
    g_transit.Clear();
    g_transit.demand = tbd::city::Demand();
    g_transitNetVersion = 0;
    {
        tbd::city::UtilWorld fresh;
        for (const auto& u : g_util.items)
            if (u.alive) fresh.removed.push_back(u.id);
        fresh.version = g_util.version + 1;
        g_util = fresh;
    }
    g_ind.sites.clear();
    ++g_ind.version;
    g_roads.Cancel();
    g_roads.net.nodes.clear();
    g_roads.net.segs.clear();
    ++g_roads.net.version;
    g_pendingZones.clear();
    g_pendingLots.clear();
    // The zone grid carries paint and lots over a rebuild by position: a new map or a load would keep the old city's
    // buildings wherever cells line up. Everything goes (ZonesSync removes the buildings); a load paints and builds anew.
    for (size_t i = 0; i < g_zones.lots.size(); ++i) g_zones.Demolish((int)i);
    for (tbd::city::ZoneCell& c : g_zones.cells) c.zone = tbd::city::kNoZone;
    g_zones.builtFrom = ~0ull;
    TransportRefreshZoning();
}

// ---- the default corridor: a highway, a railway and a power-line across the whole map, a second highway beside them, and an
// exit flyover that is cut off in mid-air. Plus the outside connections at both ends (east: Otherville, west: Westmere).
void DefaultInfrastructure() {
    using namespace tbd::city;
    RoadsInit();
    Network& net = g_roads.net;
    const int seed = g_engine.terrain.params.seed;
    const auto& P = Profiles();
    const int pHwy = ProfileIndex("Three-Lane Two-Way Highway"), pPar = ProfileIndex("Two-Lane Two-Way Highway");
    const int pRail = ProfileIndex("Double Track Railway"), pRamp = ProfileIndex("Two-Lane One-Way Highway");
    const float railHalf = P[pRail].Half(), hwyHalf = P[pHwy].Half(), parHalf = P[pPar].Half();
    const float offHwy = -(railHalf + hwyHalf + 14.0f), offPyl = railHalf + 9.0f, offPar = railHalf + 18.0f + parHalf;
    constexpr int kN = 20;
    const float x0 = -tbd::CityMap::kHalf, step = 2.0f * tbd::CityMap::kHalf / kN;
    auto centre = [&](float x) { return V2{x, tbd::CityMap::CorridorZ(x, seed)}; };
    auto tangent = [&](float x) { return Norm({1.0f, tbd::CityMap::CorridorSlope(x, seed)}); };
    auto at = [&](float x, float off) { return centre(x) + RightOf(tangent(x)) * off; };
    // a chain of grounded nodes along the corridor at a lateral offset; returns the node indices
    auto chain = [&](float off, int profile) {
        std::vector<int> ids;
        for (int i = 0; i <= kN; ++i) {
            const V2 p = at(x0 + step * i, off);
            ids.push_back(net.AddNode({p.x, RoadsGround(p.x, p.z), p.z}, true));
        }
        for (int i = 0; i < kN; ++i) {
            const float xa = x0 + step * i, xb = xa + step;
            const V2 a = at(xa, off), b = at(xb, off);
            const float L = Len(b - a);
            net.AddSeg(ids[i], ids[i + 1], a + tangent(xa) * (L / 3.0f), b - tangent(xb) * (L / 3.0f), profile);
        }
        return ids;
    };
    const std::vector<int> hwy = chain(offHwy, pHwy), rail = chain(0.0f, pRail), par = chain(offPar, pPar);
    (void)par;
    for (int i = 0; i < kN; ++i) g_util.AddLine(Util::Electricity, at(x0 + step * i, offPyl), at(x0 + step * (i + 1), offPyl), 1.0f); // pylons
    // the exit flyover: leaves the highway at node 6, climbs over the railway, the pylons and the second highway, and stops
    {
        const int from = 6;
        const float xs = x0 + step * from;
        struct R { float du, v, up; }; // along, lateral offset, metres above the ground
        const R pts[] = {{170, offHwy + 26, 3.5f}, {300, offHwy + 70, 9.5f}, {420, offPar - 20, 10.5f}, {520, offPar + 40, 10.5f}, {600, offPar + 95, 10.5f}};
        std::vector<V2> ps;
        std::vector<int> ids{hwy[from]};
        ps.push_back(at(xs, offHwy));
        for (const R& r : pts) {
            const V2 p = at(xs + r.du, r.v);
            ps.push_back(p);
            ids.push_back(net.AddNode({p.x, RoadsGround(p.x, p.z) + r.up, p.z}, false));
        }
        for (size_t k = 0; k + 1 < ids.size(); ++k) {
            const V2 a = ps[k], b = ps[k + 1];
            const float L = Len(b - a);
            const V2 ta = k == 0 ? tangent(xs) : Norm(ps[k + 1] - ps[k - 1]);
            const V2 tb = k + 2 < ps.size() ? Norm(ps[k + 2] - ps[k]) : Norm(b - a);
            net.AddSeg(ids[k], ids[k + 1], a + ta * (L / 3.0f), b - tb * (L / 3.0f), pRamp);
        }
    }
    // outside connections: all four kinds at each end
    const PlaceContext cx = TransportContext();
    for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? 1.0f : -1.0f;
        const char* name = side == 0 ? "Otherville" : "Westmere";
        const float xe = side == 0 ? x0 + step * kN : x0;
        g_transit.AddOutside(cx, OutKind::Road, at(xe, offHwy), name);
        g_transit.AddOutside(cx, OutKind::Rail, at(xe, 0.0f), name);
        g_transit.AddOutside(cx, OutKind::Sea, {sx * 3600.0f, 1100.0f}, name);
        g_transit.AddOutside(cx, OutKind::Air, {sx * 3500.0f, -700.0f}, name);
    }
    for (Outside& o : g_transit.outsides) o.name = o.name.empty() ? "" : o.name;
    g_roads.net.version++;
    g_cityMapLaneMetres = SimRoadLaneMetres(), g_cityMapLaneKnown = true; // the corridor is the map's, not the city's: no upkeep
    tbd::Log("Default infrastructure: %zu nodes, %zu segments, %d outside connections", net.nodes.size(), net.segs.size(), g_transit.OutsideCount());
}

// The hills' scale of a new map: CityMap::kDefaultRelief, or TBD_TERRAIN_RELIEF=<0.05..2> (1 = the original hilly map).
float NewMapRelief() {
    char v[24] = {};
    float r = tbd::CityMap::kDefaultRelief;
    if (tbd::platform::GetEnvA("TBD_TERRAIN_RELIEF", v, sizeof(v)) > 0) r = (float)atof(v);
    return std::isfinite(r) && r > 0.0f ? std::clamp(r, 0.05f, 2.0f) : tbd::CityMap::kDefaultRelief;
}

// A fresh random map: north land, south ocean, the default corridor, a clean city.
void NewCityGame(int seed) {
    g_engine.terrain.params.enabled = true;
    g_engine.terrain.params.flatStart = false;
    g_engine.terrain.params.mapKind = 2;
    g_engine.terrain.params.seed = seed;
    g_engine.terrain.params.relief = NewMapRelief(); // a new map is the flatter one (city_map.h kDefaultRelief)
    g_engine.terrain.field.Clear();
    g_engine.sun.showSky = false;
    // An afternoon sun, low and leaning south (docs/cs2_look_pass.md): near noon the houses' shadows hid under them and every wall read
    // the same; at 15:36 with a 30 degree lean they fall across the street, as in CS2's screenshots. The editor keeps its own sun.
    if (LookOn()) { g_engine.sun.hour = 15.6f; g_engine.sun.tiltDeg = 30.0f; } // TBD_LOOK=0: the sun as it was
    ClearCityWorld();
    g_clock.minutes = 8 * 60;
    DefaultInfrastructure();
    g_engine.camera.target = {0.0f, 0.0f, -1000.0f};
    g_engine.camera.distance = 3200.0f;
    g_engine.camera.pitch = 1.1f;
    g_engine.camera.yaw = 0.0f;
    Toast("NEW MAP - SEED " + std::to_string(seed));
    tbd::Log("New city map, seed %d", seed);
}

// ---------------------------------------------------------------------------------------------------- save
bool SaveCityGame(int slot) {
    using namespace tbd::city;
    std::ostringstream o;
    o.precision(9);
    const auto& tp = g_engine.terrain.params;
    o << "TBDGAME 1\n";
    o << "H|" << tp.mapKind << '|' << tp.seed << '|' << g_clock.minutes << '|' << g_clock.speed << '|' << g_engine.camera.target.x << ' ' << g_engine.camera.target.y << ' '
      << g_engine.camera.target.z << '|' << g_engine.camera.distance << '|' << g_engine.camera.pitch << '|' << g_engine.camera.yaw << '|' << g_transit.residents << '|'
      << g_transit.commuters << '|' << g_transit.totalPeople << '|' << g_transit.totalCargo << '\n';
    o << "T|" << tp.relief << '\n'; // the map's relief (a save without it is the original map, relief 1)
    const Network& net = g_roads.net;
    for (const Node& n : net.nodes) o << "N|" << (n.alive ? 1 : 0) << '|' << n.p.x << ' ' << n.p.y << ' ' << n.p.z << '|' << (n.grounded ? 1 : 0) << '\n';
    for (const Segment& s : net.segs)
        o << "S|" << (s.alive ? 1 : 0) << '|' << s.a << '|' << s.b << '|' << s.c1.x << ' ' << s.c1.z << '|' << s.c2.x << ' ' << s.c2.z << '|' << Profiles()[s.profile].name << '\n';
    for (const ZoneCell& c : g_zones.cells)
        if (c.zone != kNoZone) o << "Z|" << c.c.x << ' ' << c.c.z << '|' << (int)c.zone << '\n';
    for (const Placed& b : g_transport.items)
        if (b.alive)
            o << "B|" << b.def << '|' << b.c.x << ' ' << b.c.z << '|' << b.front.x << ' ' << b.front.z << '|' << b.y << '|' << b.track << '|' << b.platformInner << '|' << (int)b.state << '|'
              << b.timer << '|' << b.progress << '|' << (b.haveShore ? 1 : 0) << '|' << b.shore.x << ' ' << b.shore.z << '\n';
    for (const UtilBuilding& u : g_util.items)
        if (u.alive) o << "U|" << u.def << '|' << u.c.x << ' ' << u.c.z << '|' << u.front.x << ' ' << u.front.z << '|' << u.y << '\n';
    for (const UtilLine& l : g_util.lines)
        if (l.alive) o << "L|" << (int)l.kind << '|' << l.a.x << ' ' << l.a.z << '|' << l.b.x << ' ' << l.b.z << '|' << l.elev << '\n';
    for (const IndSite& s : g_ind.sites) {
        o << "D|" << (int)s.kind << '|' << s.c.x << ' ' << s.c.z << '|' << s.front.x << ' ' << s.front.z << '|' << s.y << '|' << s.stock << '|';
        for (size_t k = 0; k < s.area.size(); ++k) o << (k ? ";" : "") << s.area[k].x << ' ' << s.area[k].z;
        o << '\n';
    }
    for (const Stop& s : g_transit.stops)
        o << "P|" << s.id << '|' << (int)s.mode << '|' << (int)s.kind << '|' << s.p.x << ' ' << s.p.z << '|' << s.dir.x << ' ' << s.dir.z << '|' << s.y << '|' << s.outside << '|' << (s.alive ? 1 : 0) << '\n';
    for (const Route& r : g_transit.routes) {
        o << "R|" << r.id << '|' << (int)r.mode << '|' << (int)r.kind << '|' << (r.closed ? 1 : 0) << '|' << (r.alive ? 1 : 0) << '|' << r.name << '|';
        for (size_t k = 0; k < r.stops.size(); ++k) o << (k ? "," : "") << r.stops[k];
        o << '\n';
    }
    for (const Outside& q : g_transit.outsides)
        o << "O|" << q.id << '|' << (int)q.kind << '|' << q.p.x << ' ' << q.p.z << '|' << q.dir.x << ' ' << q.dir.z << '|' << q.y << '|' << (q.alive ? 1 : 0) << '|' << q.stop << '|' << q.name << '|'
          << q.peopleIn << ' ' << q.peopleOut << ' ' << q.cargoIn << ' ' << q.cargoOut << '\n';
    o << "X|" << g_transit.NextId() << '\n';
    // zone-grown buildings: saved as they stand (not regrown), so a loaded city looks the way it was saved
    for (const Lot& l : g_zones.lots) {
        if (!l.alive || l.cells.empty()) continue;
        o << "G|" << l.id << '|' << l.type << '|' << l.w << ' ' << l.d << '|' << l.level << '|' << l.progress << ' ' << l.duration << '|' << l.c.x << ' ' << l.c.z << '|'
          << l.front.x << ' ' << l.front.z << '|' << l.y << '|' << l.path << '|';
        for (size_t k = 0; k < l.cells.size(); ++k) {
            const ZoneCell& c = g_zones.cells[(size_t)l.cells[k]];
            o << (k ? ";" : "") << c.c.x << ' ' << c.c.z;
        }
        o << '\n';
    }
    SimSaveLines(o);
    const std::string text = o.str();
    FILE* f = tbd::platform::WOpen(GameSavePath(slot, L".tbdgame").c_str(), L"wb");
    const bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) fclose(f);
    if (ok) { // the terrain edits beside it
        std::vector<uint8_t> bytes;
        g_engine.terrain.field.Save(bytes);
        FILE* t = tbd::platform::WOpen(GameSavePath(slot, L".hfd").c_str(), L"wb");
        if (t) {
            fwrite(bytes.data(), 1, bytes.size(), t);
            fclose(t);
        }
    }
    Toast(ok ? "GAME SAVED - SLOT " + std::to_string(slot + 1) : "GAME SAVE FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------------- load
bool LoadCityGame(int slot) {
    using namespace tbd::city;
    using namespace tbd::scene_io_detail;
    std::ifstream in(tbd::platform::FsPath(GameSavePath(slot, L".tbdgame")), std::ios::binary);
    if (!in) {
        Toast("NO SAVED GAME IN SLOT " + std::to_string(slot + 1));
        return false;
    }
    std::string line;
    if (!std::getline(in, line) || line.rfind("TBDGAME", 0) != 0) {
        Toast("SAVE FILE IS DAMAGED");
        return false;
    }
    // the map first (the roads follow its ground)
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    float hv[12] = {};
    int mapKind = 2, seed = 1337, speed = 0;
    double minutes = 480;
    for (const std::string& l : lines) {
        if (l.rfind("H|", 0) != 0) continue;
        const auto f = Split(l, '|');
        if (f.size() < 10) break;
        mapKind = atoi(f[1].c_str()), seed = atoi(f[2].c_str()), minutes = atof(f[3].c_str()), speed = atoi(f[4].c_str());
        Floats(f[5], hv, 3);
        hv[3] = (float)atof(f[6].c_str()), hv[4] = (float)atof(f[7].c_str()), hv[5] = (float)atof(f[8].c_str());
        hv[6] = (float)atof(f[9].c_str());
        if (f.size() > 13) hv[7] = (float)atof(f[10].c_str()), hv[8] = (float)atof(f[11].c_str()), hv[9] = (float)atof(f[12].c_str()), hv[10] = (float)atof(f[13].c_str());
        break;
    }
    g_engine.terrain.params.enabled = true;
    g_engine.terrain.params.flatStart = false;
    g_engine.terrain.params.mapKind = mapKind;
    g_engine.terrain.params.seed = seed;
    g_engine.terrain.params.relief = 1.0f; // saves from before the relief scale: the original map
    for (const std::string& l : lines)
        if (l.rfind("T|", 0) == 0) {
            const float r = (float)atof(l.c_str() + 2);
            if (std::isfinite(r) && r > 0.0f) g_engine.terrain.params.relief = r;
            break;
        }
    g_engine.terrain.field.Clear();
    {
        FILE* t = tbd::platform::WOpen(GameSavePath(slot, L".hfd").c_str(), L"rb");
        if (t) {
            std::vector<uint8_t> bytes;
            uint8_t chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), t)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
            fclose(t);
            if (!bytes.empty()) g_engine.terrain.field.Load(bytes.data(), bytes.size());
        }
    }
    ClearCityWorld();
    RoadsInit();
    g_clock.minutes = minutes, g_clock.speed = std::clamp(speed, 0, 2);
    g_engine.camera.target = {hv[0], hv[1], hv[2]};
    g_engine.camera.distance = hv[3], g_engine.camera.pitch = hv[4], g_engine.camera.yaw = hv[5];
    Network& net = g_roads.net;
    uint64_t transitNext = 1;
    std::vector<std::vector<std::string>> routeRows;
    for (const std::string& l : lines) {
        const auto f = Split(l, '|');
        if (f.empty() || f[0].size() != 1) continue;
        float v[4];
        switch (f[0][0]) {
        case 'N': {
            if (f.size() < 4 || !Floats(f[2], v, 3)) break;
            Node n;
            n.p = {v[0], v[1], v[2]};
            n.alive = f[1] == "1", n.grounded = f[3] == "1";
            net.nodes.push_back(n);
            break;
        }
        case 'S': {
            if (f.size() < 7) break;
            Segment s;
            s.alive = f[1] == "1", s.a = atoi(f[2].c_str()), s.b = atoi(f[3].c_str());
            float c1[2], c2[2];
            if (!Floats(f[4], c1, 2) || !Floats(f[5], c2, 2) || s.a < 0 || s.b < 0 || s.a >= (int)net.nodes.size() || s.b >= (int)net.nodes.size()) break;
            s.c1 = {c1[0], c1[1]}, s.c2 = {c2[0], c2[1]};
            s.profile = ProfileIndex(f[6].c_str());
            net.segs.push_back(s);
            if (s.alive) {
                net.nodes[s.a].segs.push_back((int)net.segs.size() - 1);
                net.nodes[s.b].segs.push_back((int)net.segs.size() - 1);
            }
            break;
        }
        case 'Z': {
            if (f.size() < 3 || !Floats(f[1], v, 2)) break;
            g_pendingZones.push_back({{v[0], v[1]}, (uint8_t)atoi(f[2].c_str())});
            break;
        }
        case 'B': {
            if (f.size() < 13) break;
            const int def = atoi(f[1].c_str());
            if (def < 0 || def >= (int)Defs().size()) break;
            float c[2], fr[2], sh[2] = {0, 0};
            if (!Floats(f[2], c, 2) || !Floats(f[3], fr, 2)) break;
            Placement p;
            p.valid = true, p.c = {c[0], c[1]}, p.front = {fr[0], fr[1]}, p.y = (float)atof(f[4].c_str()), p.track = atoi(f[5].c_str()), p.platformInner = (float)atof(f[6].c_str());
            Floats(f[12], sh, 2);
            p.haveShore = f[11] == "1", p.shore = {sh[0], sh[1]};
            const int i = g_transport.Place(p, def);
            if (i < 0) break;
            Placed& b = g_transport.items[i];
            b.state = (BState)std::clamp(atoi(f[7].c_str()), 0, 2), b.timer = (float)atof(f[8].c_str()), b.progress = (float)atof(f[9].c_str());
            break;
        }
        case 'U': {
            if (f.size() < 5) break;
            const int def = atoi(f[1].c_str());
            float c[2], fr[2];
            if (def < 0 || def >= (int)UtilDefs().size() || !Floats(f[2], c, 2) || !Floats(f[3], fr, 2)) break;
            Placement p;
            p.valid = true, p.c = {c[0], c[1]}, p.front = {fr[0], fr[1]}, p.y = (float)atof(f[4].c_str());
            g_util.Place(p, def);
            break;
        }
        case 'L': {
            if (f.size() < 5) break;
            float a[2], b[2];
            if (!Floats(f[2], a, 2) || !Floats(f[3], b, 2)) break;
            g_util.AddLine((Util)atoi(f[1].c_str()), {a[0], a[1]}, {b[0], b[1]}, (float)atof(f[4].c_str()));
            break;
        }
        case 'D': {
            if (f.size() < 7) break;
            IndSite s;
            float c[2], fr[2];
            if (!Floats(f[2], c, 2) || !Floats(f[3], fr, 2)) break;
            s.id = g_ind.nextId++;
            s.kind = (IndKind)atoi(f[1].c_str());
            s.c = {c[0], c[1]}, s.front = {fr[0], fr[1]}, s.y = (float)atof(f[4].c_str()), s.stock = atof(f[5].c_str());
            if (!f[6].empty())
                for (const std::string& pt : Split(f[6], ';')) {
                    float q[2];
                    if (Floats(pt, q, 2)) s.area.push_back({q[0], q[1]});
                }
            g_ind.sites.push_back(s);
            g_ind.Refresh(g_ind.sites.back());
            g_ind.sites.back().stock = s.stock;
            break;
        }
        case 'P': {
            if (f.size() < 9) break;
            Stop s;
            float p[2], d[2];
            if (!Floats(f[4], p, 2) || !Floats(f[5], d, 2)) break;
            s.id = strtoull(f[1].c_str(), nullptr, 10), s.mode = (TMode)atoi(f[2].c_str()), s.kind = (TKind)atoi(f[3].c_str());
            s.p = {p[0], p[1]}, s.dir = {d[0], d[1]}, s.y = (float)atof(f[6].c_str()), s.outside = atoi(f[7].c_str()), s.alive = f[8] == "1";
            g_transit.stops.push_back(s);
            break;
        }
        case 'R': routeRows.push_back(f); break;
        case 'O': {
            if (f.size() < 10) break;
            Outside q;
            float p[2], d[2], fl[4] = {};
            if (!Floats(f[3], p, 2) || !Floats(f[4], d, 2)) break;
            Floats(f[9], fl, 4);
            q.id = strtoull(f[1].c_str(), nullptr, 10), q.kind = (OutKind)atoi(f[2].c_str()), q.p = {p[0], p[1]}, q.dir = {d[0], d[1]}, q.y = (float)atof(f[5].c_str());
            q.alive = f[6] == "1", q.stop = atoi(f[7].c_str()), q.name = f[8];
            q.peopleIn = fl[0], q.peopleOut = fl[1], q.cargoIn = fl[2], q.cargoOut = fl[3];
            g_transit.outsides.push_back(q);
            break;
        }
        case 'X': transitNext = f.size() > 1 ? strtoull(f[1].c_str(), nullptr, 10) : 1; break;
        case 'G': {
            if (f.size() < 11) break;
            PendingLot pl;
            Lot& L = pl.lot;
            float wd[2], pr[2], c[2], fr[2];
            if (!Floats(f[3], wd, 2) || !Floats(f[5], pr, 2) || !Floats(f[6], c, 2) || !Floats(f[7], fr, 2)) break;
            L.id = strtoull(f[1].c_str(), nullptr, 10), L.type = atoi(f[2].c_str()), L.w = (int)wd[0], L.d = (int)wd[1], L.level = atoi(f[4].c_str());
            L.progress = pr[0], L.duration = pr[1], L.c = {c[0], c[1]}, L.front = {fr[0], fr[1]}, L.y = (float)atof(f[8].c_str()), L.path = f[9];
            for (const std::string& pt : Split(f[10], ';')) {
                float q[2];
                if (Floats(pt, q, 2)) pl.cellCentres.push_back({q[0], q[1]});
            }
            if (!pl.cellCentres.empty()) g_pendingLots.push_back(std::move(pl));
            break;
        }
        case 'M':
        case 'Q': SimLoadLine(f); break;
        default: break;
        }
    }
    for (const auto& f : routeRows) { // after every stop exists
        if (f.size() < 8) continue;
        Route r;
        r.id = strtoull(f[1].c_str(), nullptr, 10), r.mode = (TMode)atoi(f[2].c_str()), r.kind = (TKind)atoi(f[3].c_str());
        r.closed = f[4] == "1", r.alive = f[5] == "1", r.name = f[6];
        for (const std::string& id : Split(f[7], ','))
            if (!id.empty()) r.stops.push_back(strtoull(id.c_str(), nullptr, 10));
        g_transit.routes.push_back(r);
    }
    if (!g_cityMapLaneKnown) { // a save from before the M record had it: the corridor's own profiles count as the map's
        static const char* const kCorridor[] = {"Three-Lane Two-Way Highway", "Two-Lane Two-Way Highway", "Double Track Railway", "Two-Lane One-Way Highway"};
        double sum = 0.0;
        for (size_t i = 0; i < net.segs.size(); ++i) {
            if (!net.segs[i].alive) continue;
            const Profile& p = net.Prof((int)i);
            bool corridor = false;
            for (const char* n : kCorridor) corridor |= p.name == n;
            if (corridor) sum += RoadLaneMetres(net.Curve((int)i).Length(8), p.cat == Cat::Rail ? 2 : p.fwd + p.back);
        }
        g_cityMapLaneMetres = sum, g_cityMapLaneKnown = true;
    }
    g_transit.SetNextId(std::max<uint64_t>(transitNext, 1));
    g_transit.residents = hv[7], g_transit.commuters = hv[8], g_transit.totalPeople = hv[9], g_transit.totalCargo = hv[10];
    ++net.version;
    g_transit.Rebuild(TransportContext());
    g_transitNetVersion = net.version;
    for (const Placed& b : g_transport.items) // finished buildings that were GLBs respawn through TransportSync
        (void)b;
    TransportRefreshZoning();
    Toast("GAME LOADED - SLOT " + std::to_string(slot + 1));
    tbd::Log("Loaded city slot %d: %zu nodes, %zu segs, %zu transport, %zu zones painted", slot + 1, net.nodes.size(), net.segs.size(), g_transport.items.size(), g_pendingZones.size());
    return true;
}

// Per frame: the HUD's save / load / new map requests, and zones waiting for the grid to rebuild from the loaded roads.
void GameSync() {
    using namespace tbd::city;
    if (g_gameAction) {
        const int a = g_gameAction;
        g_gameAction = 0;
        if (a == 1) SaveCityGame(g_gameSlot);
        else if (a == 2) LoadCityGame(g_gameSlot);
        else if (a == 3) NewCityGame((int)(tbd::platform::TickCountMs() % 900000ull) + 100);
    }
    if (g_pendingZones.empty() || g_zones.builtFrom != g_roads.net.version || g_zones.cells.empty()) return;
    std::unordered_map<long long, std::vector<int>> grid; // 16 m buckets
    auto key = [](float x, float z) { return ((long long)std::floor(x / 16.0f) << 32) ^ (long long)(unsigned)(int)std::floor(z / 16.0f); };
    for (size_t i = 0; i < g_zones.cells.size(); ++i) grid[key(g_zones.cells[i].c.x, g_zones.cells[i].c.z)].push_back((int)i);
    int applied = 0;
    for (const PendingZone& pz : g_pendingZones) {
        int best = -1;
        float bd = 5.0f;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = grid.find(key(pz.p.x + dx * 16.0f, pz.p.z + dz * 16.0f));
                if (it == grid.end()) continue;
                for (int i : it->second) {
                    const float d = Len(g_zones.cells[i].c - pz.p);
                    if (d < bd) bd = d, best = i;
                }
            }
        if (best >= 0 && g_zones.Paint(best, pz.zone)) ++applied;
    }
    tbd::Log("Zones restored: %d of %zu painted cells", applied, g_pendingZones.size());
    g_pendingZones.clear();
    // the grown buildings on the restored paint
    int lotsBack = 0;
    for (PendingLot& pl : g_pendingLots) {
        pl.lot.cells.clear();
        for (const V2& p : pl.cellCentres) pl.lot.cells.push_back(g_zones.NearestCell(p, 3.0f));
        lotsBack += g_zones.AddLoadedLot(pl.lot) ? 1 : 0;
    }
    if (!g_pendingLots.empty()) tbd::Log("Zone-grown buildings restored: %d of %zu", lotsBack, g_pendingLots.size());
    g_pendingLots.clear();
    SimLotsRestored();
}
