// ------------------------------------------------------------------- the .tbdgame text from a snapshot (city_game.inl)
// Included by city_game.inl before SaveCityGameFiles. The main thread copies what the text reads (CityTakeTextSnap: plain
// vector copies, the header, T, M and Q lines formatted at once); CitySaveText formats the rest anywhere, so an autosave
// does it on a worker (the formatting was most of the ~31 ms autosave hitch at 30k lots). Same bytes as before.

struct CityTextSnap {
    std::string head;   // TBDGAME, H and T lines
    std::string simLines; // SimSaveLines (M and Q records)
    std::vector<tbd::city::Node> nodes;
    std::vector<tbd::city::Segment> segs;
    std::vector<tbd::city::ZoneCell> cells;
    std::vector<tbd::city::Placed> transport;
    std::vector<tbd::city::UtilBuilding> utils;
    std::vector<tbd::city::UtilLine> lines;
    std::vector<tbd::city::IndSite> sites;
    std::vector<tbd::city::Stop> stops;
    std::vector<tbd::city::Route> routes;
    std::vector<tbd::city::Outside> outsides;
    std::vector<tbd::city::Lot> lots;
    long long nextId = 0;
};

std::shared_ptr<CityTextSnap> CityTakeTextSnap() {
    auto s = std::make_shared<CityTextSnap>();
    std::ostringstream o;
    o.precision(9);
    const auto& tp = g_engine.terrain.params;
    o << "TBDGAME 1\n";
    o << "H|" << tp.mapKind << '|' << tp.seed << '|' << g_clock.minutes << '|' << g_clock.speed << '|' << g_engine.camera.target.x << ' ' << g_engine.camera.target.y << ' '
      << g_engine.camera.target.z << '|' << g_engine.camera.distance << '|' << g_engine.camera.pitch << '|' << g_engine.camera.yaw << '|' << g_transit.residents << '|'
      << g_transit.commuters << '|' << g_transit.totalPeople << '|' << g_transit.totalCargo << '\n';
    o << "T|" << tp.relief << '\n'; // the map's relief (a save without it is the original map, relief 1)
    s->head = o.str();
    std::ostringstream m;
    m.precision(9);
    SimSaveLines(m);
    s->simLines = m.str();
    s->nodes = g_roads.net.nodes, s->segs = g_roads.net.segs, s->cells = g_zones.cells;
    s->transport = g_transport.items, s->utils = g_util.items, s->lines = g_util.lines, s->sites = g_ind.sites;
    s->stops = g_transit.stops, s->routes = g_transit.routes, s->outsides = g_transit.outsides;
    s->lots.reserve(g_zones.lots.size());
    for (const tbd::city::Lot& l : g_zones.lots)
        if (l.alive && !l.cells.empty()) s->lots.push_back(l);
    s->nextId = (long long)g_transit.NextId();
    return s;
}

std::string CitySaveText(const CityTextSnap& s) {
    using namespace tbd::city;
    std::ostringstream o;
    o.precision(9);
    o << s.head;
    for (const Node& n : s.nodes) o << "N|" << (n.alive ? 1 : 0) << '|' << n.p.x << ' ' << n.p.y << ' ' << n.p.z << '|' << (n.grounded ? 1 : 0) << '\n';
    for (const Segment& g : s.segs)
        o << "S|" << (g.alive ? 1 : 0) << '|' << g.a << '|' << g.b << '|' << g.c1.x << ' ' << g.c1.z << '|' << g.c2.x << ' ' << g.c2.z << '|' << Profiles()[g.profile].name << '\n';
    for (const ZoneCell& c : s.cells)
        if (c.zone != kNoZone) o << "Z|" << c.c.x << ' ' << c.c.z << '|' << (int)c.zone << '\n';
    for (const Placed& b : s.transport)
        if (b.alive)
            o << "B|" << b.def << '|' << b.c.x << ' ' << b.c.z << '|' << b.front.x << ' ' << b.front.z << '|' << b.y << '|' << b.track << '|' << b.platformInner << '|' << (int)b.state << '|'
              << b.timer << '|' << b.progress << '|' << (b.haveShore ? 1 : 0) << '|' << b.shore.x << ' ' << b.shore.z << '\n';
    for (const UtilBuilding& u : s.utils)
        if (u.alive) o << "U|" << u.def << '|' << u.c.x << ' ' << u.c.z << '|' << u.front.x << ' ' << u.front.z << '|' << u.y << '\n';
    for (const UtilLine& l : s.lines)
        if (l.alive) o << "L|" << (int)l.kind << '|' << l.a.x << ' ' << l.a.z << '|' << l.b.x << ' ' << l.b.z << '|' << l.elev << '\n';
    for (const IndSite& d : s.sites) {
        o << "D|" << (int)d.kind << '|' << d.c.x << ' ' << d.c.z << '|' << d.front.x << ' ' << d.front.z << '|' << d.y << '|' << d.stock << '|';
        for (size_t k = 0; k < d.area.size(); ++k) o << (k ? ";" : "") << d.area[k].x << ' ' << d.area[k].z;
        o << '\n';
    }
    for (const Stop& p : s.stops)
        o << "P|" << p.id << '|' << (int)p.mode << '|' << (int)p.kind << '|' << p.p.x << ' ' << p.p.z << '|' << p.dir.x << ' ' << p.dir.z << '|' << p.y << '|' << p.outside << '|' << (p.alive ? 1 : 0) << '\n';
    for (const Route& r : s.routes) {
        o << "R|" << r.id << '|' << (int)r.mode << '|' << (int)r.kind << '|' << (r.closed ? 1 : 0) << '|' << (r.alive ? 1 : 0) << '|' << r.name << '|';
        for (size_t k = 0; k < r.stops.size(); ++k) o << (k ? "," : "") << r.stops[k];
        o << '\n';
    }
    for (const Outside& q : s.outsides)
        o << "O|" << q.id << '|' << (int)q.kind << '|' << q.p.x << ' ' << q.p.z << '|' << q.dir.x << ' ' << q.dir.z << '|' << q.y << '|' << (q.alive ? 1 : 0) << '|' << q.stop << '|' << q.name << '|'
          << q.peopleIn << ' ' << q.peopleOut << ' ' << q.cargoIn << ' ' << q.cargoOut << '\n';
    o << "X|" << s.nextId << '\n';
    // zone-grown buildings: saved as they stand (not regrown), so a loaded city looks the way it was saved
    for (const Lot& l : s.lots) {
        o << "G|" << l.id << '|' << l.type << '|' << l.w << ' ' << l.d << '|' << l.level << '|' << l.progress << ' ' << l.duration << '|' << l.c.x << ' ' << l.c.z << '|'
          << l.front.x << ' ' << l.front.z << '|' << l.y << '|' << l.path << '|';
        for (size_t k = 0; k < l.cells.size(); ++k) {
            const ZoneCell& c = s.cells[(size_t)l.cells[k]];
            o << (k ? ";" : "") << c.c.x << ' ' << c.c.z;
        }
        o << '\n';
    }
    o << s.simLines;
    return o.str();
}
