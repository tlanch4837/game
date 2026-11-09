// ------------------------------------------------------------------- city sim Phase 4: the five hooks (frozen signatures)
// Included by city_sim_p4.inl.

namespace sim_p4 {
// The "svc." keys (sim_tuning_table.h SvcTuning; their defaults are SvcParams' compiled values) into the params.
void ApplyTuning(const tbd::city::SvcTuning& t, tbd::city::SvcParams& p) {
    p = tbd::city::SvcParams();
    p.crimePerK = (float)t.crimePerK, p.firePerKHomes = (float)t.firePerKHomes, p.sickPerK = (float)t.sickPerK;
    p.burnMinutes = std::max(10, (int)t.burnMinutes), p.spreadChance = (float)std::clamp(t.spreadChance, 0.0, 1.0);
    p.arrestShare = (float)std::clamp(t.arrestShare, 0.0, 1.0), p.bedMinutes = std::max(10, (int)t.bedMinutes);
    p.jailMinutes = std::max(10, (int)t.jailMinutes), p.flagCrime = std::clamp((int)t.flagCrime, 1, 255);
    p.yearDays = (float)std::max(1, g_simCalendar.lifeMonthsPerYear);
}
// Everything P4 holds starts over (a new map, a load before its chunks apply).
void Fresh() {
    const SvcParams keep = w.P;
    w = SvcWorld(), w.P = keep, w.seed = Seed(), w.events = &g_simEvents;
    w.delay = [](CellKey k) { const auto it = delay.find(PackKey(k)); return it == delay.end() ? 1.0f : it->second; };
    seats = SvcSeatPass(), garb.Clear(), garbLoaded.clear(), garbFac.clear(), tookHour.clear();
    for (SvcGarbStats& s : garbHour) s = SvcGarbStats();
    job.reset(), lotsJob.reset(), links.clear();
    covKey = bldKey = lotsKey = linksVersion = delayFrom = ~0ull, linksTrips = ~(size_t)0;
    events = g_simEvents.Now();
    g_simFlagTable.ClearAll(SimFlagPhaseMask(4));
}
} // namespace sim_p4

void SimP4Init() {
    using namespace sim_p4;
    on = true;
    ApplyTuning(g_simPhaseTune.svc, w.P);
    Fresh();
    g_sim.Register("svc", 1, 1, [](const tbd::city::SimTick& t) { sim_p4::Tick(t); });
    RegisterOverlays();
    tbd::Log("Sim P4: services on (coverage by travel time with capacity, incidents and dispatch, school seats, garbage%s)",
             GarbageOwned() ? " collection" : " off: city_garbage.inl collects");
}

void SimP4Frame(float /*realDt*/) {} // everything runs in the svc system (the scheduler's budget and backlog)

void SimP4Save(tbd::city::SimSaveRegistry& reg) {
    using namespace tbd::city;
    namespace s = sim_p4;
    SimChunkDef serv;
    serv.fourcc = simchunk::SERV, serv.version = kSvcServVersion;
    serv.save = [](ByteWriter& w) { SvcSaveServ(w, s::w); };
    serv.load = [](ByteReader& r, uint16_t) { return SvcLoadServ(r, s::w); };
    serv.fresh = [] { s::Fresh(); };
    reg.Register(serv);
    SimChunkDef incd;
    incd.fourcc = simchunk::INCD, incd.version = kSvcIncdVersion;
    incd.save = [](ByteWriter& w) { SvcSaveIncd(w, s::w); };
    incd.load = [](ByteReader& r, uint16_t) { return SvcLoadIncd(r, s::w); };
    incd.fresh = [] { s::w.inc.Clear(); };
    reg.Register(incd);
    SimChunkDef garb;
    garb.fourcc = simchunk::GARB, garb.version = kSvcGarbVersion;
    garb.save = [](ByteWriter& w) { SvcSaveGarb(w, s::garb); };
    garb.load = [](ByteReader& r, uint16_t) { return SvcLoadGarb(r, s::garbLoaded, s::garb); };
    garb.fresh = [] { s::garb.Clear(), s::garbLoaded.clear(); };
    reg.Register(garb);
}

// After a reset or a load: the incidents' derived counts, a flags sweep, and the first coverage pass at once (the
// dispatch of a loaded city needs its sites right away, as the saved city had them). The garbage piles wait for the
// lots (GarbageLots lists them with the loaded piles).
void SimP4Load(const tbd::city::SimLoadReport& rep) {
    using namespace tbd::city;
    namespace s = sim_p4;
    if (!s::on) return;
    auto ok = [&](uint32_t f) { const SimChunkResult* c = rep.Find(f); return c && c->status == SimChunkStatus::Loaded; };
    s::w.seed = s::Seed(), s::w.events = &g_simEvents, s::events = g_simEvents.Now();
    s::job.reset(), s::lotsJob.reset(), s::bldKey = s::covKey = s::lotsKey = ~0ull;
    SvcRecount(s::w);
    s::syncNext = true, s::loadMs = tbd::platform::TickCountMs();
    for (const auto& [lot, v] : s::garbLoaded) g_garbagePile[lot] = v.first; // the heaps show at once
    if (ok(simchunk::SERV) || ok(simchunk::INCD) || ok(simchunk::GARB))
        tbd::Log("Sim P4: services loaded (%u incidents, %zu sites, %zu garbage piles)", s::w.inc.Live(), s::w.site.size(), s::garbLoaded.size());
}

void SimP4Panels() {
    for (size_t i = 0; i < sizeof(sim_p4::kPanels) / sizeof(sim_p4::kPanels[0]); ++i) {
        tbd::gui::SimPanelEntry e;
        e.id = sim_p4::kPanels[i].id, e.name = sim_p4::kPanels[i].name, e.icon = sim_p4::kPanels[i].icon;
        e.fill = sim_p4::FillPanel, e.user = (void*)(intptr_t)i;
        tbd::gui::RegisterSimPanel(e);
    }
}
