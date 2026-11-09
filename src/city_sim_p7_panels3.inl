// ------------------------------------------------------------------- city sim Phase 7: Districts (painting), Development panel, overlay (included by city_sim_p7_panels2.inl)
// Painting is touch first: switch Paint on, then pan the map; the neighbourhood under the screen's centre (the camera
// target) joins the selected district, whole. Erase takes it out of any district. Only a new hood under the centre does
// work (deferred: nothing happens while the camera stays inside one hood).

namespace sim_p7 {
struct DistrictUi {
    Knob count, selected, paint, erase;
    int sel = 1;
    bool painting = false, erasing = false;
    int lastHood = -1;
    uint64_t lastGen = ~0ull;
} dui;

int FocusHood() {
    const auto& hs = herds_host::hoods;
    const int c = g_citySim.cells.Find(tbd::city::CellKeyAt(g_engine.camera.target.x, g_engine.camera.target.z));
    return c >= 0 && (size_t)c < hs.cellHood.size() ? hs.cellHood[(size_t)c] : -1;
}
void PaintFrame() {
    const auto& hs = herds_host::hoods;
    if (!dui.painting && !dui.erasing) { dui.lastHood = -1; return; }
    const int h = FocusHood();
    if (h < 0 || (h == dui.lastHood && hs.generation == dui.lastGen)) return;
    dui.lastHood = h, dui.lastGen = hs.generation;
    std::vector<uint64_t> keys;
    for (int c : hs.hoods[(size_t)h].cells) keys.push_back(KeyOf(c));
    const int id = dui.erasing ? 0 : dui.sel;
    if (gov.districts.Paint(keys, id) > 0 && id > 0) SimNote(hs.hoods[(size_t)h].name + " joined " + gov.districts.Get(id)->name);
}

void FillDistricts(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.accent = tbd::gui::Rgba(255, 105, 180);
    const auto& hs = herds_host::hoods;
    const int h = FocusHood();
    dui.count.Sync((float)gov.districts.Count(), [&](float v) {
        const int want = std::clamp((int)std::lround(v), 0, GovDistricts::kMax);
        while (gov.districts.Count() < want) {
            const std::string base = h >= 0 ? hs.hoods[(size_t)h].name : "District " + std::to_string(gov.districts.Count() + 1);
            gov.districts.Add(base.substr(0, base.find(' ')) + " District " + std::to_string(gov.districts.Count() + 1));
        }
        while (gov.districts.Count() > want) gov.districts.RemoveLast();
        return (float)gov.districts.Count();
    });
    const int n = gov.districts.Count();
    dui.sel = std::clamp(dui.sel, 1, std::max(1, n));
    dui.selected.Sync((float)dui.sel, [&](float v) { dui.sel = std::clamp((int)std::lround(v), 1, std::max(1, n)); return (float)dui.sel; });
    dui.paint.Sync(dui.painting ? 1.0f : 0.0f, [&](float v) { dui.painting = v >= 0.5f && n > 0, dui.erasing &= !dui.painting; return dui.painting ? 1.0f : 0.0f; });
    dui.erase.Sync(dui.erasing ? 1.0f : 0.0f, [&](float v) { dui.erasing = v >= 0.5f, dui.painting &= !dui.erasing; return dui.erasing ? 1.0f : 0.0f; });
    const int under = h >= 0 && (size_t)h < gov.hoodDistrict.size() && gov.districts.Get(gov.hoodDistrict[(size_t)h]) ? gov.hoodDistrict[(size_t)h] : 0;
    p.sub = h >= 0 ? "Centre: " + hs.hoods[(size_t)h].name + (under ? " (" + gov.districts.Get(under)->name + ")" : "") : "Centre: no neighbourhood";
    p.Slider("Districts", &dui.count.v, 0.0f, (float)GovDistricts::kMax, 1.0f).value = std::to_string(n) + (n ? "" : " (slide to add one)");
    if (n > 0) p.Slider("Selected", &dui.selected.v, 1.0f, (float)n, 1.0f).value = gov.districts.Get(dui.sel)->name;
    p.Slider("Paint (pan the map)", &dui.paint.v, 0.0f, 1.0f, 1.0f).value = dui.painting ? "on" : "off";
    p.Slider("Erase (pan the map)", &dui.erase.v, 0.0f, 1.0f, 1.0f).value = dui.erasing ? "on" : "off";
    std::vector<std::pair<std::string, std::string>> rows;
    std::vector<uint32_t> hoods((size_t)n + 1, 0), pop((size_t)n + 1, 0);
    for (size_t k = 0; k < gov.hoodDistrict.size() && k < hs.hoods.size(); ++k)
        if (gov.hoodDistrict[k] <= n) ++hoods[gov.hoodDistrict[k]], pop[gov.hoodDistrict[k]] += hs.hoods[k].Population();
    for (int d = 1; d <= n; ++d) {
        int pol = 0;
        for (uint32_t m = gov.districts.Get(d)->policies; m; m &= m - 1) ++pol;
        rows.push_back({gov.districts.Get(d)->name, std::to_string(hoods[(size_t)d]) + " hoods, " + std::to_string(pop[(size_t)d]) + " residents, " + std::to_string(pol) + " policies"});
    }
    p.List("Districts", rows, GovDistricts::kMax);
    p.Caption("Policies per district: the Policies panel. Map: the Districts overlay.");
}

struct DevUi { Knob buy[32]; } vui;
void FillDevelopment(tbd::gui::SimPanelSpec& p, void*) {
    using namespace tbd::city;
    p.accent = tbd::gui::Rgba(191, 90, 242);
    GovProgress& g = gov.progress;
    const int64_t a = g.XpForLevel(g.level), b = g.NextLevelXp();
    p.sub = "City level " + std::to_string(g.level) + ", " + std::to_string(g.points) + " development points to spend";
    p.Bar("XP to the next level", b > a ? (float)(g.xp - a) / (float)(b - a) : 1.0f, std::to_string((long long)g.xp) + " / " + std::to_string((long long)b), 1);
    const Milestone* next = g_simMilestones.Next();
    p.Stat("Milestone", std::string(g_simMilestones.Current().name) + (next ? ", next " + std::string(next->name) + " at " + std::to_string(next->population) : ""));
    int n = 0;
    const GovNodeDef* d = GovNodes(n);
    for (int i = 0; i < n && i < 32; ++i) {
        if (i % 3 == 0) p.Caption(kGovBranch[i / 3]);
        const bool have = (g.bought >> i) & 1u;
        vui.buy[i].Sync(have ? 1.0f : 0.0f, [&](float v) {
            if (v >= 0.5f && !have && !g.Buy(i)) ToastOnce("NOT ENOUGH DEVELOPMENT POINTS, OR THE BRANCH'S FIRST NODE IS MISSING");
            if (g.bought != 0) gov.Touch();
            return ((g.bought >> i) & 1u) ? 1.0f : 0.0f;
        });
        const std::string v = have ? std::string("owned: ") + d[i].what : std::to_string(d[i].cost) + " pt: " + d[i].what;
        p.Slider(d[i].name, &vui.buy[i].v, 0.0f, 1.0f, 1.0f).value = v;
    }
}

float OverlayCell(void*, int cell, float, float) {
    const int d = gov.districts.Of(KeyOf(cell));
    return d > 0 ? (float)((d - 1) * 5 % tbd::city::GovDistricts::kMax) / (float)tbd::city::GovDistricts::kMax + 0.02f : -1.0f;
}
} // namespace sim_p7

void SimP7Panels() {
    using tbd::gui::SimPanelEntry;
    struct P { const char* id; const char* name; const char* icon; void (*fill)(tbd::gui::SimPanelSpec&, void*); };
    static const P k[] = {{"p7.budget", "Budget", "coin", sim_p7::FillBudget}, {"p7.policies", "Policies", "taxes", sim_p7::FillPolicies},
                          {"p7.districts", "Districts", "layers", sim_p7::FillDistricts}, {"p7.development", "Development", "star", sim_p7::FillDevelopment},
                          {"p7.stats", "Statistics", "stats", sim_p7::FillStats}};
    for (const P& x : k) {
        SimPanelEntry e;
        e.id = x.id, e.name = x.name, e.icon = x.icon, e.fill = x.fill;
        tbd::gui::RegisterSimPanel(e);
    }
}
