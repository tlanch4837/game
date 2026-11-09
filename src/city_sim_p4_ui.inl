// ------------------------------------------------------------------- city sim Phase 4: overlays and panels
// Included by city_sim_p4.inl. Overlays: crime, health, education, garbage, telecom (gui/city_overlays.h providers, per
// cell 0..1). Panels (gui/sim_panels.h): one per service group with capacity used / total, coverage, vehicles, units,
// today's incidents and the response time, and a budget slider for the services the budget panel does not hold.

namespace sim_p4 {
int OverlayCell(float x, float z) { return w.CellOf(CellKeyAt(x, z)); }
template <int Which>
float OverlayValue(void*, int, float x, float z) {
    const int c = OverlayCell(x, z);
    if (c < 0 || (size_t)c >= w.out.cov.size()) return -1.0f;
    const auto& cv = w.out.cov[(size_t)c];
    switch (Which) {
    case 0: return std::min(1.0f, (float)w.crimeF[(size_t)c] / (float)std::max(1, w.P.flagCrime)); // red at the flag line
    case 1: return cv[kSvcHealth] / 255.0f;
    case 2: return std::max({cv[kSvcSchool], cv[kSvcHighSchool], cv[kSvcUniversity]}) / 255.0f;
    case 3: { // the worst lot of the cell (1: piling up)
        float worst = 0.0f;
        if ((size_t)c + 1 >= w.cellBldOff.size()) return 0.0f;
        for (int k = w.cellBldOff[(size_t)c]; k < w.cellBldOff[(size_t)c + 1]; ++k) {
            const SvcBldg& b = w.bld[(size_t)w.cellBldIdx[(size_t)k]];
            if (b.lot) worst = std::max(worst, LotLevel(b.lot));
        }
        return std::min(1.0f, worst);
    }
    default: return cv[kSvcTelecom] / 255.0f;
    }
}
void RegisterOverlays() {
    using tbd::gui::Overlay;
    tbd::gui::RegisterOverlayProvider(Overlay::Crime, OverlayValue<0>, nullptr, "Safe  ..  High crime");
    tbd::gui::RegisterOverlayProvider(Overlay::Health, OverlayValue<1>, nullptr, "No care  ..  Clinics and hospitals with room");
    tbd::gui::RegisterOverlayProvider(Overlay::Education, OverlayValue<2>, nullptr, "No schools  ..  School seats in reach");
    tbd::gui::RegisterOverlayProvider(Overlay::Garbage, OverlayValue<3>, nullptr, "Clean  ..  Piling up");
    tbd::gui::RegisterOverlayProvider(Overlay::Telecom, OverlayValue<4>, nullptr, "No signal  ..  Strong network");
}

// ---- panels
struct PanelDef { const char* id; const char* name; const char* icon; int slots[3]; int incident; };
const PanelDef kPanels[] = {
    {"p4.fire", "Fire & Rescue", "fire", {kSvcFire, -1, -1}, kIncFire},
    {"p4.police", "Police", "police", {kSvcPolice, -1, -1}, kIncCrime},
    {"p4.health", "Healthcare", "health", {kSvcHealth, kSvcDeathcare, -1}, kIncSick},
    {"p4.education", "Education", "school", {kSvcSchool, kSvcHighSchool, kSvcUniversity}, -1},
    {"p4.garbage", "Garbage", "trash", {kSvcGarbage, -1, -1}, -1},
    {"p4.comms", "Post & Telecom", "comms", {kSvcPost, kSvcTelecom, -1}, -1},
    {"p4.parks", "Parks", "stats", {kSvcPark, -1, -1}, -1},
};
const char* SlotName(int s) {
    static const char* const k[kServicesV2] = {"Fire", "Police", "Healthcare", "Elementary", "Parks", "Transit", "Deathcare", "Garbage", "Post", "Telecom",
                                               "High school", "University"};
    return s >= 0 && s < kServicesV2 ? k[s] : "";
}
const char* UnitName(int s) {
    return s == kSvcPolice ? "Jail cells" : s == kSvcHealth ? "Beds" : s == kSvcDeathcare ? "Body places" : s == kSvcPost ? "Letters a day"
         : s == kSvcTelecom ? "Bandwidth" : (s == kSvcSchool || s == kSvcHighSchool || s == kSvcUniversity) ? "Seats" : nullptr;
}
std::string Num(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.0f", v); return b; }

void FillSlot(tbd::gui::SimPanelSpec& sp, int slot) {
    const SvcSlotTotals t = SvcTotals(w, slot);
    sp.Caption(std::string(SlotName(slot)) + ": " + Num(t.sites) + (t.sites == 1 ? " building" : " buildings"));
    if (!t.sites) return;
    sp.Bar("Capacity used", t.serves ? (float)t.served / (float)t.serves : 0.0f, Num((double)t.served) + " of " + Num((double)t.serves) + " people",
           t.served >= t.serves ? -1 : 0);
    sp.Bar("Coverage", (float)t.coverage, Num(t.coverage * 100.0) + " %", t.coverage < 0.5 ? -1 : t.coverage > 0.85 ? 1 : 0);
    if (t.vehicles) sp.Stat("Vehicles out", Num(t.busy) + " of " + Num(t.vehicles), t.busy >= t.vehicles ? -1 : 0);
    if (const char* u = UnitName(slot)) if (t.units) sp.Bar(u, (float)t.used / (float)t.units, Num((double)t.used) + " of " + Num((double)t.units), t.used >= t.units ? -1 : 0);
    if (slot >= kServices && slot != kSvcHighSchool && slot != kSvcUniversity && SimGovServiceCapacity(slot) < 0.0f) sp.Slider("Budget", &funding[slot], 0.5f, 1.5f, 0.05f, "%.2f");
    else sp.Stat("Budget", Num(FundingOf(slot) * 100.0) + " % (Budget panel)");
}
void FillPanel(tbd::gui::SimPanelSpec& sp, void* user) {
    const PanelDef& d = kPanels[(size_t)(intptr_t)user];
    sp.title = d.name, sp.icon = d.icon;
    sp.sub = on ? std::string() : "Phase 4 is switched off (svc.enabled)";
    if (d.slots[0] == kSvcGarbage) {
        const garbage::Stats& g = g_garbage;
        sp.Stat("Garbage made", Num(g.producedPerMonth) + " t a month");
        sp.Bar("Collected", g.producedPerMonth > 0 ? g.collectedPerMonth / g.producedPerMonth : 0.0f, Num(g.collectedPerMonth) + " t a month", g.collectedPerMonth < g.producedPerMonth * 0.9f ? -1 : 1);
        sp.Stat("Capacity", Num(g.capacityPerMonth) + " t a month");
        sp.Stat("Waiting on lots", Num(g.uncollected) + " t", g.piling ? -1 : 0);
        sp.Stat("Piling up / unserved", Num(g.piling) + " / " + Num(g.unserved) + " of " + Num(g.lots), g.piling ? -1 : 0);
        sp.Stat("Garbage trucks today", Num(std::count_if(w.trips.begin(), w.trips.end(), [](const SvcTrip& t) { return t.slot == kSvcGarbage; })));
        if (SimGovServiceCapacity(kSvcGarbage) < 0.0f) sp.Slider("Budget", &funding[kSvcGarbage], 0.5f, 1.5f, 0.05f, "%.2f");
        else sp.Stat("Budget", Num(FundingOf(kSvcGarbage) * 100.0) + " % (Budget panel)");
        return;
    }
    for (int s : d.slots) if (s >= 0) FillSlot(sp, s);
    if (d.incident >= 0) {
        sp.Caption(std::string(SvcIncName(d.incident)) + " today (yesterday)");
        sp.Stat("Incidents", Num(w.today.n[d.incident]) + " (" + Num(w.yesterday.n[d.incident]) + ")");
        sp.Stat("Not answered", Num(w.today.unanswered[d.incident]) + " (" + Num(w.yesterday.unanswered[d.incident]) + ")", w.yesterday.unanswered[d.incident] ? -1 : 0);
        const SvcDayStats& y = w.yesterday.responses ? w.yesterday : w.today;
        if (y.responses) sp.Stat("Average response", Num(y.responseMin / y.responses) + " min");
        if (d.incident == kIncFire) sp.Stat("Buildings lost", Num(w.today.destroyed) + " (" + Num(w.yesterday.destroyed) + ")", w.yesterday.destroyed ? -1 : 0);
        if (d.incident == kIncSick) sp.Stat("Deaths collected", Num(w.today.n[kIncBody]) + " (" + Num(w.yesterday.n[kIncBody]) + ")");
    }
    if (d.slots[0] == kSvcSchool) sp.Stat("Students seated / without a seat", Num(seats.lastSeated) + " / " + Num(seats.lastUnseated), seats.lastUnseated ? -1 : 0);
    if (d.slots[0] == kSvcPark || d.slots[0] == kSvcPost) sp.Caption("Placeholder buildings: models not made yet");
    char b[96];
    std::snprintf(b, sizeof(b), "%.3f / %.2f ms, pass %.1f ms", tickAvgMs, tickMaxMs, covMs);
    sp.Stat("Sim cost (tick avg / max)", b);
}
} // namespace sim_p4
