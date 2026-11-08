// The Glass Dock+ GUI (src/gui/glass_dock.h) on the city game: its catalogue is the game's own tools (roads, zones,
// utilities, transport and services, industry, terrain, info views), its status the sim's, and its actions open the same
// tools the CS2 HUD opens. Included by game_core.inl after HudOpenTool. TBD_GUI_LEGACY=1 keeps the CS2 HUD instead.
// TBD_GUI_DEVICE=phone|pad picks the layout and scale on Windows (iOS reads the screen); TBD_GUI_LAYOUT=pc|touch the UI.
#pragma once
// src/gui/glass_dock.h is included by the translation unit (main.cpp, game_host.cpp): this file sits inside its namespace

namespace gdock {
using tbd::gui::dock::Category;
using tbd::gui::dock::Group;
using tbd::gui::dock::Item;

enum Kind : int { kRoad = 1, kZone, kDezone, kUtilDef, kUtilLine, kTransport, kLine, kOutside, kIndustry, kWaste, kTerrain, kOverlay, kBudget, kInfo };
const char* const kInfoPanels[] = {"City Information", "Population Info", "Notifications", "Statistics"}; // gui/city_info_panels.h
inline int Id(Kind k, int i) { return (int)k << 16 | (i & 0xFFFF); }

tbd::gui::dock::State g_state;
tbd::gui::dock::Model g_model;
tbd::gui::dock::Atlas g_atlas;
bool g_tried = false, g_hidden = false; // photo mode: the GUI hides until the next tap
bool g_menuPaused = false;              // the pause menu paused the clock (and resumes it when it closes)
void TitleOpenPicker(bool save);        // [title] title_host.inl: the slot picker, the title screen, the logo cell
void TitleFromGame();
void TitleAppendLogo(std::vector<uint8_t>& rgba, int w, int& h, tbd::gui::dock::Atlas& atlas);

inline bool Legacy() {
    static const bool on = tbd::platform::GetEnvA("TBD_GUI_LEGACY", nullptr, 0) > 0;
    return on;
}

std::string Money(long long v) {
    char b[32];
    const long long a = v < 0 ? -v : v;
    if (a >= 1000000) std::snprintf(b, sizeof(b), "%s$%.2fM", v < 0 ? "-" : "", a / 1e6);
    else if (a >= 10000) std::snprintf(b, sizeof(b), "%s$%lldk", v < 0 ? "-" : "", a / 1000);
    else std::snprintf(b, sizeof(b), "%s$%lld", v < 0 ? "-" : "", a);
    return b;
}
std::string Thousands(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return (v < 0 ? "-" : "") + o;
}

// the mockup's icon for a road tab / building tab
const char* RoadTabIcon(tbd::city::Tab t) {
    using tbd::city::Tab;
    switch (t) {
    case Tab::Small: return "road_s";
    case Tab::Medium: return "road_m";
    case Tab::Large: return "road_l";
    case Tab::Highway: return "highway";
    case Tab::Quay: return "quay";
    case Tab::Path: return "path";
    case Tab::Rail: return "rail";
    case Tab::Intersection: return "xing";
    default: return "roundabout";
    }
}

void BuildCatalogue() {
    using namespace tbd::city;
    using tbd::gui::Rgba;
    auto& M = g_model;
    M.cats.clear();
    // Roads: the mockup's group order
    {
        Category c{"roads", "Roads", "road", Rgba(125, 133, 151), {}};
        const struct { Tab t; const char* name; } order[] = {{Tab::Small, "Small"}, {Tab::Medium, "Medium"}, {Tab::Large, "Large"}, {Tab::Highway, "Highways"},
                                                            {Tab::Path, "Paths"}, {Tab::Quay, "Quays"}, {Tab::Rail, "Railways"}, {Tab::Intersection, "Intersections"},
                                                            {Tab::Roundabout, "Roundabouts"}};
        const auto& A = Assets();
        for (const auto& o : order) {
            Group g{o.name, RoadTabIcon(o.t), {}, {}};
            for (size_t i = 0; i < A.size(); ++i)
                if (A[i].tab == o.t) {
                    char w[24];
                    std::snprintf(w, sizeof(w), "%.0f m wide", Profiles()[A[i].profile].width);
                    g.items.push_back({A[i].name, A[i].kind == AssetKind::Road ? w : "", RoadTabIcon(o.t), Id(kRoad, (int)i)});
                }
            if (!g.items.empty()) c.groups.push_back(std::move(g));
        }
        M.cats.push_back(std::move(c));
    }
    // Zoning
    {
        Category c{"zones", "Zoning", "zone", Rgba(79, 174, 91), {}};
        const char* const catIcon[] = {"house", "shop", "office", "industry"};
        const auto& Z = ZoneTypes();
        for (int k = 0; k < (int)ZoneCat::Count; ++k) {
            Group g{ZoneCatName((ZoneCat)k), catIcon[k], {}, {}};
            for (size_t i = 0; i < Z.size(); ++i)
                if ((int)Z[i].cat == k) g.items.push_back({Z[i].name, "", catIcon[k], Id(kZone, (int)i)});
            c.groups.push_back(std::move(g));
        }
        c.groups[0].items.push_back({"Dezone", "", "dezone", Id(kDezone, 0)});
        M.cats.push_back(std::move(c));
    }
    // Terrain
    {
        Category c{"terrain", "Terrain", "terr", Rgba(160, 122, 79), {}};
        c.groups.push_back({"Shape", "terr", {{"Raise", "", "raise", Id(kTerrain, 0)}, {"Dig", "", "dig", Id(kTerrain, 1)}, {"Flatten", "", "flatten", Id(kTerrain, 2)}}, {}});
        M.cats.push_back(std::move(c));
    }
    // Utilities
    {
        Category c{"util", "Utilities", "util", Rgba(242, 177, 28), {}};
        Group el{"Electricity", "power", {}, {}}, wa{"Water & Sewage", "water", {}, {}}, ga{"Garbage", "trash", {}, {}};
        const auto& U = UtilDefs();
        for (size_t i = 0; i < U.size(); ++i) (U[i].tab == UTab::Electricity ? el : wa).items.push_back({U[i].name, Money(U[i].cost), U[i].tab == UTab::Electricity ? "power" : "water", Id(kUtilDef, (int)i)});
        char b[24];
        std::snprintf(b, sizeof(b), "$%d/m", UtilLineCostPerMetre(Util::Electricity, 0));
        el.items.push_back({"Electricity Line", b, "pylon", Id(kUtilLine, 0)});
        std::snprintf(b, sizeof(b), "$%d/m", UtilLineCostPerMetre(Util::Water, -10));
        wa.items.push_back({"Water Pipe", b, "pipe", Id(kUtilLine, 1)});
        std::snprintf(b, sizeof(b), "$%d/m", UtilLineCostPerMetre(Util::Sewage, -10));
        wa.items.push_back({"Sewage Pipe", b, "pipe", Id(kUtilLine, 2)});
        ga.items.push_back({"Landfill", "", "landfill", Id(kWaste, 0xFFFF)});
        for (int i = 0; i < (int)(sizeof(kWasteTypes) / sizeof(kWasteTypes[0])); ++i) ga.items.push_back({kWasteTypes[i].name, "", i == 0 ? "incinerator" : i == 1 ? "recycle" : "hazard", Id(kWaste, i)});
        c.groups = {el, wa, ga};
        M.cats.push_back(std::move(c));
    }
    // Services and Transport: city::Defs() by tab
    const auto& D = Defs();
    auto defsOf = [&](TTab t, const char* icon) {
        std::vector<Item> v;
        for (size_t i = 0; i < D.size(); ++i)
            if (D[i].tab == t) v.push_back({D[i].name, Money(D[i].cost), icon, Id(kTransport, (int)i)});
        return v;
    };
    {
        Category c{"serv", "Services", "serv", Rgba(232, 70, 92), {}};
        Group health{"Healthcare", "health", {}, {{"Health", "health", defsOf(TTab::Health, "health"), {}}, {"Deathcare", "death", defsOf(TTab::Death, "death"), {}}}};
        c.groups.push_back(health);
        c.groups.push_back({"Fire & Rescue", "fire", defsOf(TTab::Fire, "fire"), {}});
        c.groups.push_back({"Police", "police", defsOf(TTab::Police, "police"), {}});
        c.groups.push_back({"Education", "school", defsOf(TTab::Education, "school"), {}});
        c.groups.push_back({"Communications", "comms", defsOf(TTab::Comms, "comms"), {}});
        M.cats.push_back(std::move(c));
    }
    {
        Category c{"transit", "Transport", "bus", Rgba(47, 143, 224), {}};
        Group lines{"Lines", "line", {}, {}};
        const struct { TMode m; TKind k; const char* name; const char* icon; } L[] = {
            {TMode::Bus, TKind::Passenger, "Bus Line", "bus"}, {TMode::Tram, TKind::Passenger, "Tram Line", "tram"},
            {TMode::Train, TKind::Passenger, "Train Line", "train"}, {TMode::Subway, TKind::Passenger, "Subway Line", "subway"},
            {TMode::Boat, TKind::Passenger, "Boat Line", "ship"}, {TMode::Plane, TKind::Passenger, "Plane Line", "plane"},
            {TMode::Train, TKind::Cargo, "Cargo Train Line", "cargo"}, {TMode::Boat, TKind::Cargo, "Cargo Line", "cargo"}};
        for (const auto& l : L) lines.items.push_back({l.name, "", l.icon, Id(kLine, (int)l.m * 2 + (int)l.k)});
        c.groups.push_back(lines);
        c.groups.push_back({"Road transit", "bus", defsOf(TTab::Bus, "bus"), {}});
        c.groups.push_back({"Rail", "train", defsOf(TTab::Rail, "train"), {}});
        c.groups.push_back({"Ship", "ship", defsOf(TTab::Ship, "ship"), {}});
        c.groups.push_back({"Air", "plane", defsOf(TTab::Air, "plane"), {}});
        Group out{"Connections", "outside", {}, {}};
        const char* const on[] = {"Road Connection", "Rail Connection", "Sea Connection", "Air Connection"};
        for (int k = 0; k < (int)OutKind::Count; ++k) out.items.push_back({on[k], "", "outside", Id(kOutside, k)});
        c.groups.push_back(out);
        M.cats.push_back(std::move(c));
    }
    {
        Category c{"industry", "Industry", "factory", Rgba(194, 124, 58), {}};
        Group g{"Specialized", "factory", {}, {}};
        const char* const icons[] = {"grain", "livestock", "forestry", "oil", "ore"};
        for (int k = 0; k < (int)IndKind::Landfill; ++k) g.items.push_back({IndTypeOf((IndKind)k).name, "", icons[k], Id(kIndustry, k)});
        c.groups.push_back(g);
        M.cats.push_back(std::move(c));
    }
    {
        Category c{"city", "City", "city", Rgba(139, 92, 246), {}};
        c.groups.push_back({"Economy", "coin", {{"Budget", "", "coin", Id(kBudget, 0)}, {"Taxes", "", "taxes", Id(kBudget, 1)}, {"Service Funding", "", "funding", Id(kBudget, 2)}}, {}});
        M.cats.push_back(std::move(c));
    }
    {
        Category c{"info", "Info", "info", Rgba(20, 184, 166), {}};
        c.groups.push_back({"City", "stats", {{"City Information", "", "info", Id(kInfo, 0)}, {"Population", "", "ppl", Id(kInfo, 1)},
                                                 {"Notifications", "", "bell", Id(kInfo, 2)}, {"Statistics", "", "stats", Id(kInfo, 3)}}, {}});
        M.cats.push_back(std::move(c));
    }
    {
        Category c{"views", "Views", "layers", Rgba(58, 58, 72), {}};
        Group g{"Overlays", "layers", {}, {}};
        const char* const icons[] = {"power", "water", "sewage", "landvalue", "traffic", "health"};
        for (int k = 0; k < (int)tbd::gui::Overlay::Count; ++k) g.items.push_back({tbd::gui::OverlayName((tbd::gui::Overlay)k), "", icons[k], Id(kOverlay, k)});
        c.groups.push_back(g);
        M.cats.push_back(std::move(c));
    }
    M.Resolve(g_atlas);
}

// The atlas texture + index: Assets\gui (iOS bundle: assets/gui), else the repo's assets_src\gui next to bin\.
void LoadAtlas(tbd::Renderer& r) {
    g_tried = true;
    std::string png, txt;
    for (const std::wstring& dir : {tbd::Join(g_engine.assetsRoot, L"gui"), tbd::Join(g_engine.assetsRoot, L"..\\assets_src\\gui")}) {
        if (tbd::ReadAllText(tbd::Join(dir, L"glass_atlas.png"), png) && tbd::ReadAllText(tbd::Join(dir, L"glass_atlas.txt"), txt)) break;
        png.clear(), txt.clear();
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (png.empty() || !tbd::DecodeImage((const uint8_t*)png.data(), png.size(), rgba, w, h)) {
        tbd::Log("Glass Dock: no gui/glass_atlas.png: icons and previews missing");
        return;
    }
    // straight alpha is what ui.hlsl expects; the PNG is straight already
    g_atlas.Parse(txt.data(), txt.size());
    TitleAppendLogo(rgba, w, h, g_atlas); // [title] the Optima Engine logo below the atlas: the "logo" cell (title_host.inl)
    const tbd::TextureId tex = r.CreateTexture(rgba.data(), w, h);
    r.SetUiImages(tex);
    tbd::Log("Glass Dock: atlas %dx%d, %zu cells", w, h, g_atlas.cells.size());
}

// The sim's numbers into the model.
void FillStatus(tbd::gui::CityHud& hud) {
    auto& M = g_model;
    M.money = Money(hud.money); // in the red it reads "-$12k", not "$0"
    M.pop = hud.population >= 0 ? Thousands(hud.population) : "0";
    long long income = 0;
    for (long long v : hud.incomeDay) income += v;
    const long long net = income - hud.upkeepDay;
    M.moneyUp = net == 0 ? "" : (net > 0 ? "+" : "-") + Money(net < 0 ? -net : net).substr(1);
    M.moneyBar = hud.money > 0 ? std::clamp((float)std::log10((double)hud.money + 1) / 7.0f, 0.05f, 1.0f) : 0.0f;
    M.popBar = hud.population > 0 ? std::clamp((float)std::log10((double)hud.population + 1) / 6.0f, 0.05f, 1.0f) : 0.0f;
    // happiness: the residents' mean (sim_cells.h, 0..255)
    double sum = 0, n = 0;
    for (const auto& c : g_citySim.sim) {
        const double p = c.Population();
        sum += p * c.happiness, n += p;
    }
    const float happy = n > 0 ? (float)(sum / n / 255.0) : 0.5f;
    char b[48];
    std::snprintf(b, sizeof(b), "%d%%", (int)std::lround(happy * 100));
    M.happy = b, M.happyBar = happy;
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int day = g_clock.Day() - 1;
    std::snprintf(b, sizeof(b), "%s %d \xC2\xB7 Y%d", kMonths[(day / 30) % 12], day % 30 + 1, day / 360 + 1);
    M.date = b;
    M.speed = g_clock.speed + 1, M.paused = g_clock.paused && !g_menuPaused;
    M.city = hud.cityName;
    static int alertFrame = 0;
    if (alertFrame++ % 30 == 0) { // the kinds of problem in the city (sim_flags.h)
        uint8_t any = 0;
        for (uint8_t f : g_simFlags) any |= f;
        int k = 0;
        for (int i = 0; i < tbd::city::kFlagCount; ++i) k += (any >> i) & 1;
        M.alerts = k;
    }
}

// The active tool, as the placement bar shows it.
void FillTool() {
    using namespace tbd::city;
    auto& M = g_model;
    M.tool.clear(), M.toolCost.clear(), M.toolIcon.clear(), M.modeName.clear(), M.modeIcon.clear(), M.toolOpts.clear();
    static const char* const kModeNames[] = {"Straight", "Simple curve", "Complex curve", "Continuous", "Grid", "Replace", "Swap", "Bulldoze"};
    static const char* const kModeIcons[] = {"straight", "curve", "complex", "continuous", "grid", "replace", "swap", "bulldoze"};
    if (g_roads.active) {
        const Asset& a = g_roads.Current();
        M.tool = a.name, M.toolIcon = RoadTabIcon(a.tab);
        if (a.kind == AssetKind::Road) M.modeName = kModeNames[(int)g_roads.mode], M.modeIcon = kModeIcons[(int)g_roads.mode];
        // [road toolkit] Lower / Raise first: the Phone UI shows only the first two options, and it used to show "Elevation"
        // (raise only) with "Lower" fourth, out of reach: on the iPad roads could go up but never down. The full controls
        // are in the toolkit panel (RoadKit).
        M.toolOpts = {{"Lower", "down"}, {"Raise", "up"}, {"Snapping", "snap"}, {"Parallel", "parallel"}, {"Bulldoze", "bulldoze"}};
    } else if (g_zoneTool.active) {
        static const char* const zm[] = {"Brush", "Fill block", "Box"};
        static const char* const zi[] = {"brush", "bucket", "box"};
        M.tool = g_zoneTool.erase ? "Dezone" : ZoneTypes()[g_zoneTool.type].name, M.toolIcon = g_zoneTool.erase ? "dezone" : "zone";
        M.modeName = zm[(int)g_zoneTool.mode], M.modeIcon = zi[(int)g_zoneTool.mode];
    } else if (g_utilTool.active && (g_utilTool.def >= 0 || g_utilTool.line >= 0)) {
        if (g_utilTool.def >= 0) M.tool = UtilDefs()[g_utilTool.def].name, M.toolCost = Money(UtilDefs()[g_utilTool.def].cost), M.toolIcon = "power";
        else M.tool = tbd::gui::util_ui::LineShort(g_utilTool.line), M.toolIcon = g_utilTool.line == 0 ? "pylon" : "pipe";
        if (g_utilTool.def >= 0) M.toolOpts = {{"Rotate", "rotate"}, {"Rotate back", "rotate~"}}; // 90 degrees a tap (city_industry.inl BuildingRotate)
        else M.toolOpts = {{"Elevation", "elev"}};
    } else if (g_transportTool.active && (g_transportTool.def >= 0 || g_transportTool.lines)) {
        if (g_transportTool.def >= 0) M.tool = Defs()[g_transportTool.def].name, M.toolCost = Money(Defs()[g_transportTool.def].cost), M.toolIcon = "bus";
        else M.tool = std::string(TModeName(g_transportTool.lmode)) + " Line", M.toolIcon = "line";
        if (g_transportTool.def >= 0 && !g_transportTool.lines) M.toolOpts = {{"Rotate", "rotate"}, {"Rotate back", "rotate~"}};
    } else if (g_indTool.active) {
        M.tool = g_indTool.waste && g_indTool.point > 0 ? kWasteTypes[g_indTool.point - 1].name : IndTypeOf((IndKind)g_indTool.kind).name, M.toolIcon = "factory";
        if (g_indTool.mode == IndustryTool::Place) M.toolOpts = {{"Rotate", "rotate"}, {"Rotate back", "rotate~"}};
    } else if (g_terrainEdit.on) {
        static const char* const tn[] = {"Raise", "Dig", "Flatten"};
        static const char* const ti[] = {"raise", "dig", "flatten"};
        M.tool = tn[std::clamp(g_terrainEdit.mode, 0, 2)], M.toolIcon = ti[std::clamp(g_terrainEdit.mode, 0, 2)];
    }
    InteractFillTool(M.tool, M.toolIcon, M.modeName, M.modeIcon, M.toolCost, M.toolOpts, M.toolOptOn); // [interact] bulldoze, Rotate toggle + Snap
    M.bulldozeOn = g_doze.active;
}

// Settings > Edge pan: a finger held at the screen's edge keeps a drag going (game_host.h EdgePan).
bool EdgePanOn() { return g_model.edgePan; }

// A build tool is out: one finger draws / places instead of moving the camera (iOS touch, game_host.h TouchBuilds).
bool ToolActive() {
    return g_roads.active || g_zoneTool.active || (g_utilTool.active && (g_utilTool.def >= 0 || g_utilTool.line >= 0)) ||
           (g_transportTool.active && (g_transportTool.def >= 0 || g_transportTool.lines)) || g_indTool.active || g_terrainEdit.on;
}

void CloseTools() {
    const bool utilWasOpen = g_utilTool.active;
    g_zoneTool.active = false;
    if (g_roads.active) g_roads.Close();
    g_utilTool.Close();
    g_transportTool.active = false, g_transportTool.def = -1, g_transportTool.lines = false;
    g_indTool.active = false, g_indTool.mode = IndustryTool::Pick, g_indTool.drawing = -1, g_indTool.wsel = -1, g_indTool.point = 0;
    g_terrainEdit.on = false;
    if (utilWasOpen) UtilToolClosed();
    InteractCloseTools(); // [interact] bulldoze, the rotate dial, the touch pin
}

void Pick(int id, tbd::gui::CityHud& hud) {
    using namespace tbd::city;
    const int k = id >> 16, i = id & 0xFFFF;
    if (k == kOverlay) { // views toggle, the tool stays
        g_overlayHud.on = (int)g_overlayHud.on == i ? tbd::gui::Overlay::None : (tbd::gui::Overlay)i;
        return;
    }
    if (k == kBudget) { hud.Dismiss(), hud.budget = true; return; }
    if (k == kInfo) { // the info panels (one at a time, over the map; the tool stays)
        const char* name = kInfoPanels[std::min(i, 3)];
        const bool was = hud.notDoneName && std::strcmp(hud.notDoneName, name) == 0;
        hud.Dismiss();
        if (!was) hud.ShowNotDone(name, "");
        return;
    }
    CloseTools();
    switch (k) {
    case kRoad: g_roads.Open(), g_roads.Select(i); break;
    case kZone: g_zoneTool.active = true, g_zoneTool.erase = false, g_zoneTool.type = i, g_zoneTool.tab = ZoneTypes()[i].cat; break;
    case kDezone: g_zoneTool.active = true, g_zoneTool.erase = true; break;
    case kUtilDef: g_utilTool.active = true, g_utilTool.Select(i); break;
    case kUtilLine: g_utilTool.active = true, g_utilTool.SelectLine(i); break;
    case kTransport: g_transportTool.active = true, g_transportTool.lines = false, g_transportTool.Select(i); break;
    case kLine: g_transportTool.active = true, g_transportTool.lines = true, g_transportTool.lmode = (TMode)(i / 2), g_transportTool.lkind = (TKind)(i % 2), g_transportTool.outKind = -1; break;
    case kOutside: g_transportTool.active = true, g_transportTool.lines = true, g_transportTool.outKind = i; break;
    case kIndustry: g_indTool.active = true, g_indTool.waste = false, IndustryStartPlace(i); break;
    case kWaste: g_indTool.active = true, g_indTool.waste = true, g_indTool.kind = (int)IndKind::Landfill, WasteStartPlace(i == 0xFFFF ? -1 : i); break;
    case kTerrain: g_terrainEdit.on = true, g_terrainEdit.mode = i; break;
    default: break;
    }
}

// The placement bar's buttons on the active tool.
void ToolButton(const tbd::gui::dock::Out& o) {
    using namespace tbd::city;
    if (InteractToolButton(o, o.opt >= 0 && o.opt < (int)g_model.toolOpts.size() ? g_model.toolOpts[(size_t)o.opt].first : std::string())) return; // [interact]
    if (o.cancel) { // Esc semantics: drop the pick, then close
        if (g_roads.active) g_roads.Close();
        CloseTools();
        return;
    }
    if (o.confirm && g_transportTool.active && g_transportTool.lines && g_transportTool.lineStops > 0) { g_transportTool.finishLine = true; return; }
    if (o.confirm) { CloseTools(); return; } // what was drawn stays; the tool closes
    if (o.undo && g_roads.active) g_roads.Undo();
    if (o.mode) {
        if (g_roads.active) g_roads.SetMode((ToolMode)(((int)g_roads.mode + 1) % 7));
        else if (g_zoneTool.active) g_zoneTool.mode = (tbd::gui::ZoneToolState::Mode)(((int)g_zoneTool.mode + 1) % 3);
    }
    if (o.opt >= 0) {
        const std::string& n = g_model.toolOpts[o.opt].first;
        if (n == "Rotate" || n == "Rotate back") BuildingRotate(n == "Rotate" ? 90.0f : -90.0f); // the held ghost, 90 degrees a tap
        else if (n == "Elevation") { if (g_roads.active) g_roads.Raise(1); else g_utilTool.Raise(1); }
        else if (n == "Lower") g_roads.Raise(-1);
        else if (n == "Raise") g_roads.Raise(1); // [road toolkit]
        else if (n == "Parallel") g_roads.parallel = !g_roads.parallel;
        else if (n == "Snapping") g_roads.snapAll = !g_roads.snapAll;
        else if (n == "Bulldoze") g_roads.SetMode(ToolMode::Bulldoze);
    }
}

#include "dock_news_stats.inl" // the Show stats switch and the City News pill / card (NewsCard, Load / SaveGuiPrefs)

// The player's settings file (saves\settings.ini beside the game saves): the Graphics page's options, sound, text size, edge pan.
// Until the file exists or the player touches a setting, the TBD_* environment switches rule the graphics as before.
static bool g_settingsTouched = false;
// The Sound page's volumes (percent) as settings.ini keys vol_master, vol_music, vol_ambience, vol_effects, vol_radio, and
// onto the audio buses (tbd::audio::SetUserVolume: on top of the game's own bus levels).
const char* const kVolumeKeys[5] = {"vol_master", "vol_music", "vol_ambience", "vol_effects", "vol_radio"};
void LoadVolumes(const std::string& text) {
    int v = 0;
    for (int i = 0; i < 5; ++i)
        if (tbd::gfx::Find(text, kVolumeKeys[i], v)) g_model.volume[i] = std::clamp(v, 0, 100);
    g_model.sound = g_model.volume[3] > 0;
}
std::string SaveVolumes() {
    std::string s;
    for (int i = 0; i < 5; ++i) s += std::string(kVolumeKeys[i]) + "=" + std::to_string(g_model.volume[i]) + "\n";
    return s;
}
void ApplyVolumes() {
    using tbd::audio::Bus;
    const Bus bus[5] = {Bus::Master, Bus::Music, Bus::Ambience, Bus::Sfx, Bus::Radio};
    for (int i = 0; i < 5; ++i) {
        const float k = (float)g_model.volume[i] / 100.0f;
        tbd::audio::SetUserVolume(bus[i], k * k); // a loudness-like curve: 50% is about -12 dB
    }
}
static std::string g_gfxPrev; // the Graphics page's options as last applied: a change here is what hands the graphics over to the player
std::wstring SettingsFilePath() {
    const std::wstring dir = tbd::Join(g_engine.projectFolder.empty() ? tbd::ExeDirectory() : g_engine.projectFolder, L"saves");
    { std::error_code ec; std::filesystem::create_directory(tbd::platform::FsPath(dir), ec); }
    return tbd::Join(dir, L"settings.ini");
}
void LoadSettingsFile() {
#if defined(__APPLE__)
    // An iPad is read at arm's length: Large text until the player picks otherwise (a saved "text=" below wins).
    if (g_state.pad) g_model.textSize = 1;
#endif
    FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[512];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    int v = 0;
    if (tbd::gfx::Find(text, "preset", v)) tbd::gfx::Parse(text, g_model.gfx), g_settingsTouched = true;
    if (tbd::gfx::Find(text, "sound", v)) g_model.sound = v != 0, g_model.volume[3] = v != 0 ? g_model.volume[3] : 0; // older files: on / off
    LoadVolumes(text);
    if (tbd::gfx::Find(text, "text", v)) g_model.textSize = std::clamp(v, 0, 1);
    if (tbd::gfx::Find(text, "edgepan", v)) g_model.edgePan = v != 0;
}
void SaveSettingsFile() {
    FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"wb");
    if (!f) return;
    const std::string text = tbd::gfx::Serialize(g_model.gfx) + "sound=" + (g_model.sound ? "1" : "0") + "\ntext=" + std::to_string(g_model.textSize) +
                             "\nedgepan=" + (g_model.edgePan ? "1" : "0") + "\n" + SaveVolumes();
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);

    SaveGuiPrefs(); // dock_news_stats.inl adds its keys (stats=, news=)
}

void ApplySettings(tbd::Renderer& r) {
    (void)r; // (the render scale is applied in UpdateDynamicResolution, outside the frame: SetRenderScale remakes the back buffers)
    if (g_settingsTouched) { // the Graphics page rules from here on; LookTier, RenderScale and PresetBudget read tbd::gfx::g
        tbd::gfx::Settings& s = g_model.gfx;
        tbd::gfx::Clamp(s);
        tbd::gfx::g = s;
        tbd::gfx::userSet = true;
        g_gfxQuality = s.lighting; // 0 full, 1 fast, 2 unlit
        g_engine.sun.shadows = s.shadows != 0;
        g_engine.terrain.params.radius = tbd::gfx::kRadius[s.distance];
        SetMaxGraphics(s.preset == tbd::gfx::kUltra && s.lighting == 0); // Ultra keeps the MAX look
    }
    tbd::audio::SetBusVolume(tbd::audio::Bus::Sfx, g_model.sound ? 1.0f : 0.0f);
    ApplyVolumes();
}

// The device the layout is for, and its points-to-pixels scale.
void DeviceAndScale(float& scale) {
    static int device = [] { // 0 unknown (Windows desktop: iPad sizes at the DPI scale), 1 phone, 2 pad
        char v[16] = {};
        if (tbd::platform::GetEnvA("TBD_GUI_DEVICE", v, sizeof(v)) > 0) return strcmp(v, "phone") == 0 ? 1 : 2;
#if defined(__APPLE__)
        return 3; // iOS: from the screen's shape
#else
        return 0;
#endif
    }();
    const float w = (float)g_engine.vpW, h = (float)g_engine.vpH;
    bool pad = true;
    if (device == 1) pad = false;
    else if (device == 3) pad = w / std::max(h, 1.0f) < 1.7f; // iPad 1.44, iPhone 2.17
    g_state.pad = pad;
    if (device == 0) scale = DpiScale() * g_guiScaleSel;
    else scale = h / (pad ? 820.0f : 393.0f); // the mockup's point sizes: iPad Air 11" 1180 x 820, iPhone 15 852 x 393
    static bool first = true;
    if (first) { // iPad starts in PC UI, iPhone in Phone UI; TBD_GUI_LAYOUT overrides
        first = false;
        char v[16] = {};
        g_state.pcUi = tbd::platform::GetEnvA("TBD_GUI_LAYOUT", v, sizeof(v)) > 0 ? strcmp(v, "pc") == 0 : pad;
    }
}

// ---------------------------------------------------------------- [road ux] safe area, touch, the compact kits, the aim dot
// The platform's safe area in device pixels (left, top, right, bottom, corner radius): game_host.h SetSafeArea (iOS).
float g_safePx[5] = {0, 0, 0, 0, 0};
// A touch screen: iOS, or TBD_GUI_TOUCH=1 on Windows (screenshots of the touch layout: the mini kit, the aim dot).
bool Touch() {
#if defined(__APPLE__)
    return true;
#else
    static const bool on = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_GUI_TOUCH", v, sizeof(v)) > 0 && v[0] != '0'; }();
    return on;
#endif
}
// The safe area in the dock's points this frame: TBD_GUI_SAFE="top,left,bottom,right[,corner]" (points) wins, for
// Windows screenshots of a notched device; else the platform's pixels at the dock's scale.
void FillSafe(float scale) {
    static tbd::gui::SafeInsets env;
    static const bool fromEnv = [] {
        char v[64] = {};
        return tbd::platform::GetEnvA("TBD_GUI_SAFE", v, sizeof(v)) > 0 && tbd::gui::SafeInsets::Parse(v, env);
    }();
    g_model.safe = fromEnv ? env : tbd::gui::SafeInsets::FromPixels(g_safePx, scale);
}
// The safe area in pixels (for the GUI's own panels: Context::SetInsets, and the toasts).
void SafePixels(float& l, float& t, float& r, float& b) {
    const float s = g_gui.Scale() > 0 ? g_gui.Scale() : 1.0f;
    l = g_model.safe.L() * s, t = g_model.safe.T() * s, r = g_model.safe.R() * s, b = g_model.safe.B() * s;
}

tbd::gui::road_kit::State g_kit = [] { // [road ux] the mini strips and the aim dot by default on touch
    tbd::gui::road_kit::State k;
    k.mini = k.dotAvail = Touch();
    return k;
}();
tbd::gui::reticle::State g_dot;
tbd::gui::terrain_kit::State g_terrainKit;
float g_dockBottom = 0; // points the dock and the kits took from the bottom edge last frame (the toasts ride above them)

// The aim dot is the road tool's cursor (touch, the kit's toggle, not Swap): RoadsCursor reads the screen centre and a
// one-finger touch on the map moves the camera (game_host.cpp TouchBuilds).
bool ReticleOn() { return !Legacy() && tbd::gui::reticle::Wanted(g_roads, Touch(), g_kit.dotOn); }

// The road tool's snap radius on touch: about a fingertip (56 px) at the screen centre, in metres on the ground.
void TouchSnapRadius() {
    if (!Touch()) return;
    const float cx = (float)g_engine.vpW * 0.5f, cy = (float)g_engine.vpH * 0.5f, d = 40.0f;
    tbd::Vec3 a, b;
    if (GroundHit(g_engine.camera.ScreenRay(cx, cy, (float)g_engine.vpW, (float)g_engine.vpH), a) &&
        GroundHit(g_engine.camera.ScreenRay(cx + d, cy, (float)g_engine.vpW, (float)g_engine.vpH), b)) {
        const float mpp = std::hypot(b.x - a.x, b.z - a.z) / d;
        g_roads.snapRadius = tbd::gui::reticle::SnapRadiusMetres(mpp, 28.0f * g_gui.Scale()); // a fingertip: 28 pt
    }
}

// [road toolkit] The road / rail tool's CS2 panel (tool modes, elevation down / readout / up / step / ground level,
// parallel, snapping) and its road-type palette, above the placement controls (gui/road_toolkit.h); on touch the mini
// strips. Hidden while a catalogue panel or the pause menu is open. Then the aim dot and its tick / cross.
void RoadKit(bool menuOpen, float dockBottom) {
    g_kit.safe = g_model.safe;
    if (!g_roads.active || menuOpen || g_state.cat >= 0) return;
    // the panels' bottoms: over the PC UI placement bar (iPad / iPhone sizes), or over the Phone UI's corner buttons;
    // mini: right over the (slim) dock
    float bottom = g_state.pcUi ? (g_state.pad ? 146.0f : 116.0f) : 76.0f;
    if (g_kit.mini) bottom = dockBottom + 8.0f;
    tbd::gui::road_kit::Out ko;
    const tbd::gui::road_kit::Layout kl = tbd::gui::road_kit::Frame(g_gui, g_atlas, g_roads, g_kit, bottom, g_state.pad, ko, g_model.textSize ? 1.15f : 1.0f);
    g_dockBottom = std::max(g_dockBottom, g_gui.In().viewH / g_gui.Scale() - kl.top); // the toasts ride over the kit
    if (g_model.sound)
        for (float p : ko.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
    if (ko.close) {
        CloseTools();
        return;
    }
    TouchSnapRadius();
    if (ReticleOn()) {
        const float cx = (float)g_engine.vpW * 0.5f, cy = (float)g_engine.vpH * 0.5f;
        tbd::Vec3 hit;
        const bool on = GroundHit(g_engine.camera.ScreenRay(cx, cy, (float)g_engine.vpW, (float)g_engine.vpH), hit);
        tbd::gui::reticle::Out ro;
        tbd::gui::reticle::Frame(g_gui, g_atlas, g_roads, g_dot, g_model.safe, kl.rightTop, on, hit, ro);
        if (g_model.sound)
            for (float p : ro.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
    }
}

// [road ux] The terrain tools on the dock: the compact strip (gui/terrain_kit.h) over the slim bar.
void TerrainKit(bool menuOpen, float dockBottom) {
    g_terrainKit.safe = g_model.safe;
    if (!g_terrainEdit.on || menuOpen || g_state.cat >= 0) return;
    const tbd::gui::terrain_kit::Brush b{&g_terrainEdit.mode, &g_terrainEdit.radius, &g_terrainEdit.metresPerSecond};
    tbd::gui::terrain_kit::Out to;
    const tbd::gui::terrain_kit::Layout tl = tbd::gui::terrain_kit::Frame(g_gui, g_atlas, b, g_terrainKit, dockBottom + 8.0f, g_engine.drag == Drag::Terrain, to);
    g_dockBottom = std::max(g_dockBottom, g_gui.In().viewH / g_gui.Scale() - tl.top);
    if (g_model.sound)
        for (float p : to.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
    if (to.save) SaveTerrainEdits();
    if (to.load) LoadTerrainEdits();
    if (to.close) CloseTools();
}

// One frame of the dock: replaces the CS2 HUD and the tools' own panels. Returns true when it ran.
bool Frame(tbd::Renderer& r, tbd::gui::CityHud& hud) {
    static bool built = false;
    if (!built) { // the atlas came in with the font (outside a frame: InitGameGui)
        built = true;
        BuildCatalogue();
        LoadSettingsFile();
        LoadGuiPrefs(); // dock_news_stats.inl: stats=, news=
        g_gfxPrev = tbd::gfx::Serialize(g_model.gfx);
        ApplySettings(r);
    }
    static bool scripted = false; // TBD_GUI_STATE="pc|touch,open,cat=<id>,pick=<item name>,pause,settings": captures (shots/gui_1to1)
    if (!scripted) {
        scripted = true;
        char v[256] = {};
        if (tbd::platform::GetEnvA("TBD_GUI_STATE", v, sizeof(v)) > 0) {
            std::string all = v, tok;
            for (size_t i = 0; i <= all.size(); ++i) {
                if (i < all.size() && all[i] != ',') { tok += all[i]; continue; }
                if (tok == "pc") g_state.pcUi = true;
                else if (tok == "touch") g_state.pcUi = false;
                else if (tok == "open") g_state.open = true;
                else if (tok == "pause") g_state.menu = true;
                else if (tok == "settings") g_state.menu = g_state.settings = true;
                else if (tok == "sound") g_state.menu = g_state.settings = g_state.snd = true;
                else if (tok.rfind("cat=", 0) == 0) {
                    for (size_t c = 0; c < g_model.cats.size(); ++c)
                        if (g_model.cats[c].id == tok.substr(4)) g_state.cat = (int)c, g_state.catT = 0, g_state.open = true;
                } else if (tok.rfind("pick=", 0) == 0)
                    for (auto& c : g_model.cats)
                        for (auto& gr : c.groups)
                            for (auto& it : gr.items)
                                if (it.name == tok.substr(5)) Pick(it.id, hud), g_state.cat = -1;
                tok.clear();
            }
        }
    }
    if (g_hidden) { // photo mode: any tap brings the GUI back
        if (g_gui.In().released) g_hidden = false;
        return true;
    }
    FillStatus(hud);
    FillTool();
    // the Views items show which overlay is on
    for (auto& c : g_model.cats)
        if (c.id == "views")
            for (auto& it : c.groups[0].items) it.on = (int)g_overlayHud.on == (it.id & 0xFFFF);
    // [road ux] the safe area; a road kit in its mini strips or the terrain kit slims the bar and takes the placement bar's place
    FillSafe(g_gui.Scale());
    g_model.slim = g_model.hidePlaceBar = (g_roads.active && g_kit.mini) || g_terrainEdit.on;
    tbd::gui::dock::Out o;
    tbd::gui::dock::Frame(g_gui, g_state, g_model, g_atlas, o);
    g_dockBottom = o.bottomH;
    for (float p : o.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
    if (o.pickItem >= 0) {
        Pick(o.pickItem, hud);
        if ((o.pickItem >> 16) != kOverlay) g_state.lastCat = g_state.cat, g_state.cat = -1, g_state.catT = 0, g_state.closing = true, g_state.open = false;
    }
    ToolButton(o);
    if (o.bulldozePressed) InteractBulldozeButton(); // [interact] the dock's Bulldoze button
    if (o.toggledPause) g_clock.paused = !g_clock.paused;
    if (o.setSpeed) g_clock.speed = o.speed - 1, g_clock.paused = false;
    if (o.layersPressed) { // the layers button: the Views category
        for (size_t i = 0; i < g_model.cats.size(); ++i)
            if (g_model.cats[i].id == "views") g_state.cat = g_state.cat == (int)i ? -1 : (int)i, g_state.catT = 0, g_state.grp = 0;
    }
    if (o.cameraPressed || o.photo) g_hidden = true;
    if (o.alertsPressed) g_overlayHud.on = g_overlayHud.on == tbd::gui::Overlay::Power ? tbd::gui::Overlay::None : tbd::gui::Overlay::Power;
    if (o.save) TitleOpenPicker(true);  // [title] the ten-slot picker over the pause menu (title_host.inl)
    if (o.load) TitleOpenPicker(false); // [title]
    if (o.mainMenu) TitleFromGame();    // [title] the title screen
    if (o.settingsChanged) { // sound, text size and the interface switch must not hand the graphics over from the TBD_* switches
        const std::string now = tbd::gfx::Serialize(g_model.gfx);
        if (now != g_gfxPrev) g_gfxPrev = now, g_settingsTouched = true;
        ApplySettings(r);
        SaveSettingsFile();
    }
    // the pause menu pauses the sim under it
    if (o.menuOpen && !g_menuPaused && !g_clock.paused) g_menuPaused = true, g_clock.paused = true;
    if (!o.menuOpen && g_menuPaused) g_menuPaused = false, g_clock.paused = false;
    RoadKit(o.menuOpen, o.bottomH);    // [road toolkit] [road ux]
    TerrainKit(o.menuOpen, o.bottomH); // [road ux]
    NewsCard(hud, o.menuOpen);
    InteractGuiFrame(g_atlas, o.menuOpen, !g_state.pad, g_model.textSize ? 1.15f : 1.0f); // [interact] the info card, the dial's degrees, bulldoze's prompt
    if (hud.budget) tbd::gui::DrawBudgetPanel(g_gui, hud);
    if (tbd::gui::CityInfoPanel(g_gui, hud.notDoneName, hud.stats) == 2) hud.Dismiss();
    return true;
}
} // namespace gdock
