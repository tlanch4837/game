// ------------------------------------------------------------------- city sim Phase 4: seats, garbage, vehicles, the svc system
// Included by city_sim_p4_tick.inl.

namespace sim_p4 {
// One slice of the school-seat sweep (the whole citizen store once a game day). Phase 1 off: nothing to seat.
void SeatSlice() {
    if (!SimP1On()) return;
    LifeWorld& L = g_life.w;
    auto age = [&L](uint32_t ci) { return L.AgeOf(ci); };
    auto home = [&L](uint32_t ci) {
        const LifeBuilding* B = LifeHomeOf(L, ci);
        if (!B) return -1;
        const CellKey k{B->cellX, B->cellZ};
        return B->cell >= 0 && (size_t)B->cell < w.cov.cells.size() && w.cov.cells[(size_t)B->cell].key == k ? B->cell : w.CellOf(k);
    };
    SvcSeatSlice(w, L.cit, seats, L.cit.Capacity() / 144 + 1, age, home);
}

// garbage: one group a minute; the facilities and city_garbage.inl's totals every game hour
std::unordered_map<uint32_t, double> tookHour; // facility id -> tonnes this hour
std::vector<SvcTrip> garbTrips;                // scratch (moved into w.trips)
void GarbageHour() {
    garbage::Stats st;
    st.materials = g_garbage.materials;
    for (const SvcGarbStats& s : garbHour) st.producedPerMonth += (float)s.produced, st.uncollected += s.uncollected, st.piling += s.piling, st.lots += s.lots, st.unserved += s.unserved;
    double collected = 0;
    for (const SvcGarbFacility& f : garbFac) st.capacityPerMonth += f.perMonth, collected += tookHour.count(f.id) ? tookHour[f.id] : 0.0;
    st.collectedPerMonth = (float)(collected * 24.0 * 30.0);
    for (IndSite& s : g_ind.sites) { // landfills fill with what they took
        if (!s.IsLandfill()) continue;
        st.landfillUsed += s.stock, st.landfillCap += s.Capacity();
        const auto it = tookHour.find((uint32_t)(0x40000000u | (s.id & 0x3FFFFFFFu)));
        if (it == tookHour.end()) continue;
        const int step = (int)(s.Fill() * 40);
        s.stock = std::min<double>(s.stock + it->second, s.Capacity());
        if ((int)(s.Fill() * 40) != step) ++s.version, ++g_ind.version;
    }
    for (const WasteBuilding& b : g_wasteB) { // incinerators make power, recycling makes materials, as city_garbage.inl did
        const WasteType& T = kWasteTypes[b.type];
        const auto it = tookHour.find((uint32_t)(0x80000000u | (b.id & 0x3FFFFFFFu)));
        const double took = it == tookHour.end() ? 0.0 : it->second;
        const float busy = T.tPerMonth > 0 ? (float)std::min(1.0, took / (T.tPerMonth / (24.0 * 30.0))) : 0.0f;
        float& sm = g_wasteBusy[b.id];
        sm += (busy - sm) * 0.1f;
        if (b.type == 0) st.powerMW += T.mw * sm;
        if (b.type == 1) st.materials += took * 0.8;
    }
    g_garbage = st;
    tookHour.clear();
    GarbageFacilities();
}
void GarbageMinute(int64_t minute) {
    if (lotsJob && lotsReady.load(std::memory_order_acquire)) garb.Adopt(lotsJob), lotsJob.reset();
    GarbageLots();
    if (minute % 60 == 0 || garbFac.empty()) GarbageHour();
    const int g = (int)(minute % kGarbGroups);
    static std::vector<uint64_t> changed;
    static std::vector<std::pair<uint64_t, double>> mirror;
    changed.clear(), mirror.clear(), garbTrips.clear();
    SvcGarbStats st;
    garb.Visit(g, minute, garbFac, garbTrips, changed, mirror, st);
    garbHour[g] = st;
    for (const SvcGarbFacility& f : garbFac) if (f.took > 0) tookHour[f.id] += f.took;
    for (const auto& [lot, pile] : mirror) g_garbagePile[lot] = pile;
    for (uint64_t lot : changed) { const BuildingId b = g_simBuildings.Find(BOrigin::Lot, lot); if (b.Valid()) w.MarkDirty(b.v); }
    w.trips.insert(w.trips.end(), garbTrips.begin(), garbTrips.end());
    for (const SvcGarbLot& l : garb.groups[g]) // the garbage coverage factor: clean 255 .. piling up 0
        if (CellFactor* f = g_cellFactors.Mut(l.cell)) f->coverage[kSvcGarbage] = (uint8_t)(255.0f * (1.0f - std::min(1.0f, l.Level())));
}
float LotLevel(uint64_t lot) { const SvcGarbLot* l = garb.Find(lot); return l ? l->Level() : 0.0f; }

// The vehicles of the last game day as herd node links (one per trip; the herds spread them over the working day and
// the visible cars draw them with the existing models).
void Links() {
    if (linksTrips == w.trips.size() && linksVersion == herds_host::table.version) return;
    linksTrips = w.trips.size(), linksVersion = herds_host::table.version;
    std::map<std::pair<int, int>, float> n; // ordered: the same links in the same order every time
    for (const SvcTrip& t : w.trips) if (t.fromNode >= 0 && t.toNode >= 0 && t.fromNode != t.toNode) n[{t.fromNode, t.toNode}] += 1.0f;
    links.clear();
    const float share = (float)std::clamp(g_simPhaseTune.svc.vehicleDrawShare, 0.0, 4.0) * g_simPreset.truckShare;
    for (const auto& [k, v] : n) {
        NodeLink l;
        l.fromNode = k.first, l.toNode = k.second, l.n = v * share, l.type = (uint8_t)kTripFreight;
        if (l.n > 0) links.push_back(l);
    }
}

// The svc system, every game minute.
void Tick(const SimTick& t) {
    const double t0 = Now();
    const int64_t m = t.minute;
    if (syncNext && (!g_pendingZones.empty() || !g_pendingLots.empty()) && tbd::platform::TickCountMs() - loadMs < 30000) return; // a load's lots are not back yet
    if (m % 10 == 0 || syncNext) SyncBuildings(syncNext), CoverageTick(), DelayTick();
    ReadEvents(m);
    SvcTick(w, m, &g_cellFactors);
    if (m % 10 == 3) SeatSlice();
    if (GarbageOwned()) GarbageMinute(m);
    SvcFlushFlags(w, g_simFlagTable, m, [](uint64_t lot) { return LotLevel(lot); });
    if (m % kWindowMinutes == 0) Links();
    const double days = g_simPhaseTune.svc.verifyDays;
    if (days > 0 && m % (int64_t)(1440 * std::max(1.0, days)) == 720) { // the full recompute check (debug)
        verifyBad = SvcVerifyCounts(w);
        if (SvcFlagsSettled(w)) verifyBad += SvcVerifyFlags(w, g_simFlagTable, m, [](uint64_t lot) { return LotLevel(lot); });
        if (SimP1On()) verifyBad += (int)SvcSeatRecount(w, g_life.w.cit);
        tbd::Log("Sim P4: verify %s (%d stale)", verifyBad ? "FOUND STALE STATE" : "ok", verifyBad);
    }
    const double ms = Now() - t0;
    tickAvgMs += (ms - tickAvgMs) * 0.01, tickMaxMs = std::max(tickMaxMs * 0.999, ms);
}
} // namespace sim_p4

// city_herds.inl HerdsRelease: Phase 4's vehicles (fire engines, patrol cars, ambulances, hearses, garbage trucks).
const std::vector<tbd::city::NodeLink>& SimServiceVehicleLinks() { return sim_p4::links; }
