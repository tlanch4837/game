// [title] The front end on the city game: the black loading screen, the title screen and the ten-slot save / load picker
// (gui/title_screen.h), the save slots themselves (game/save_slots.h) and the bundled example city that becomes Save 1.
// Included by game_core.inl after glass_dock_host.inl. Off with TBD_GUI_LEGACY=1; on Windows also off when a capture /
// test switch is set (TBD_CAPTURE, TBD_SHOT, TBD_CITY_STRESS, ...) unless TBD_TITLE=1; TBD_TITLE=0 turns it off.
//
// Start: StartTerrainSandbox calls TitleBoot: old city_slotN saves move into the numbered slots, the example city is
// copied into an empty Save 1 (once), the splash shows while the most recent save loads behind it (that city is the
// title's backdrop, so Continue just fades in), then the title. New Game / Load Game show the loading screen while the
// map is made / read. The pause menu's Save and Load open the picker over the game; its Main menu opens the title.
// TBD_TITLE_SHOT=splash|title|load|save|loading|settings holds one screen (captures: shots/title).
// TBD_MAP=example TBD_EXAMPLE_SAVE=<seconds>: build the Bayside example city, run that long, write it as the template
// (assets_src/saves_template/bayside.*) and quit.
// gui/title_screen.h and game/save_slots.h are included by the translation unit (main.cpp, game_host.cpp): this file sits in its anonymous namespace

namespace title_host {
namespace ti = tbd::gui::title;
namespace sv = tbd::saves;

ti::State g_st;
ti::Model g_m;
int g_currentSlot = -1;     // the slot the city being played came from or was last saved to (-1: a new city)
bool g_resumable = false;   // the city in memory is the one Continue means (loaded at boot, or the one being played)
bool g_inGame = false;      // the player has been in the game since the front end last showed
enum class Pending { None, Load, New, Example };
Pending g_pending = Pending::None;
int g_pendingSlot = -1, g_pendingSeed = 0;
uint64_t g_loadStart = 0;   // ms: the load ran (the settle wait starts)
float g_shownProgress = 0;
tbd::OrbitCamera g_cam;     // the camera to give back when the title's drift ends
bool g_camSaved = false;
std::string g_shot;         // TBD_TITLE_SHOT

sv::Store Store() { return sv::Store(tbd::platform::FsPath(GameSavesDir())); }

bool Enabled() {
    static const bool on = [] {
        if (gdock::Legacy()) return false;
#if defined(__APPLE__)
        return true;
#else
        char v[16] = {};
        if (tbd::platform::GetEnvA("TBD_TITLE", v, sizeof(v)) > 0) return v[0] != '0';
        for (const char* k : {"TBD_CAPTURE", "TBD_SHOT", "TBD_CITY_STRESS", "TBD_GUI_STATE", "TBD_PLAYTEST", "TBD_FEATURECOST", "TBD_BENCH_FRAMES",
                              "TBD_GUI_POINTER", "TBD_CHUNK_CHECK", "TBD_MAP", "TBD_ROADS", "TBD_GUI_PREVIEW_DUMP"})
            if (tbd::platform::GetEnvA(k, nullptr, 0) > 0) return false;
        return true;
#endif
    }();
    return on;
}
bool Active() { return Enabled() && g_st.screen != ti::Screen::None; }

// The example city template: assets\saves_template (iOS: the bundle's copy in Documents/assets), else the repo's assets_src
// (beside the assets root, or beside bin\ when a project folder has its own Assets).
std::filesystem::path TemplateDir() {
    for (const std::wstring& d : {tbd::Join(g_engine.assetsRoot, L"saves_template"), tbd::Join(g_engine.assetsRoot, L"..\\assets_src\\saves_template"),
                                  tbd::Join(tbd::ExeDirectory(), L"..\\assets_src\\saves_template")}) {
        std::error_code ec;
        const std::filesystem::path p = tbd::platform::FsPath(d);
        if (std::filesystem::is_regular_file(p / "bayside.tbdgame", ec)) return p;
    }
    return tbd::platform::FsPath(tbd::Join(g_engine.assetsRoot, L"saves_template"));
}

std::string MapName() { return g_engine.terrain.params.mapKind == 3 ? "Bayside" : gdock::g_model.city; }
std::string MapLine() {
    return g_engine.terrain.params.mapKind == 3 ? "Bayside archipelago" : "Random map, seed " + std::to_string(g_engine.terrain.params.seed);
}

// The slot list into the model.
void RefreshSlots() {
    const sv::Store st = Store();
    const std::vector<sv::Meta> list = st.List();
    for (int i = 0; i < sv::kSlots; ++i) {
        const sv::Meta& m = list[(size_t)i];
        ti::SlotRow& r = g_m.slots[(size_t)i];
        r = ti::SlotRow{};
        r.used = m.used, r.example = m.example, r.current = i == g_currentSlot;
        if (!m.used) continue;
        r.name = m.name.empty() ? sv::SlotLabel(i) : m.name;
        r.when = sv::RealDate(m.saved) + (m.saved ? "  -  " : "") + sv::GameDate(m.minutes);
        r.stats = (m.population >= 0 ? sv::Thousands(m.population) + " citizens  -  " : std::string()) + sv::Money(m.money);
    }
    g_m.continueSlot = st.MostRecent();
    if (g_m.continueSlot >= 0) {
        const sv::Meta m = list[(size_t)g_m.continueSlot];
        g_m.continueLine = sv::SlotLabel(g_m.continueSlot) + "  -  " + (m.name.empty() ? "City" : m.name) + "  -  " + sv::RealDate(m.saved);
    }
    if (g_inGame && g_resumable) g_m.continueSlot = std::max(g_m.continueSlot, 0), g_m.continueLine = "Back to " + MapName();
}

// Called with every save (city_game.inl SaveCityGame): the slot's metadata for the list.
void WriteMeta(int slot) {
    sv::Meta m;
    m.used = true;
    m.name = MapName(), m.map = MapLine();
    m.saved = (int64_t)std::time(nullptr);
    m.minutes = g_clock.minutes;
    m.population = (long long)g_simTotals.population;
    m.money = (long long)(g_simEcon.treasury / 100);
    Store().WriteMeta(slot, m);
}

void SaveCam() { g_cam = g_engine.camera, g_camSaved = true; }
void RestoreCam() {
    if (g_camSaved) g_engine.camera = g_cam;
    g_camSaved = false;
}

void Show(ti::Screen s) {
    if (g_st.screen == ti::Screen::None) g_clock.paused = true; // the sim waits under the front end
    g_st.Go(s);
}
// Fades the front end out to the game.
void Leave() {
    RestoreCam();
    g_st.leaveT = 0;
}
void StartLoading(Pending p, int slot, int seed) {
    g_pending = p, g_pendingSlot = slot, g_pendingSeed = seed;
    g_shownProgress = 0;
    Show(ti::Screen::Loading);
}

// What is still arriving after a load, and how far along it is (0..1).
bool Settled(float& progress, std::string& step) {
    if (g_pending != Pending::None) { progress = 0.04f, step = g_pending == Pending::Load ? "Reading the save" : "Planning the map"; return false; }
    const int r = std::max(1, g_engine.terrain.params.radius);
    int want = 0;
    for (int z = -r; z <= r; ++z)
        for (int x = -r; x <= r; ++x) want += x * x + z * z <= r * r ? 1 : 0;
    const float terrain = g_engine.terrain.params.enabled ? std::clamp((float)g_engine.terrain.LoadedChunks() / (float)std::max(1, want), 0.0f, 1.0f) : 1.0f;
    const bool city = g_pendingZones.empty() && !g_exampleFounding, scene = !SceneLoadActive(), stream = !g_stream.Active() || g_stream.IsIdle();
    const bool ground = ground_bake::Settled();
    progress = 0.1f + 0.5f * terrain + (city ? 0.15f : 0.0f) + (scene && stream ? 0.1f : 0.0f) + (ground ? 0.15f : 0.0f);
    char b[96];
    if (terrain < 0.98f) std::snprintf(b, sizeof(b), "Shaping the terrain  %d%%", (int)(terrain * 100)), step = b;
    else if (!city) step = "Laying out the city";
    else if (!scene) step = SceneLoadStatus();
    else if (!stream) step = "Streaming the world";
    else if (!ground) step = "Baking the ground";
    else step = "Ready";
    return terrain >= 0.98f && city && scene && stream && ground;
}

void RunPending() {
    const Pending p = g_pending;
    g_pending = Pending::None;
    g_camSaved = false; // the new city's own camera, not the title's drift
    gdock::CloseTools();
    if (p == Pending::Load) {
        if (LoadCityGame(g_pendingSlot)) g_currentSlot = g_pendingSlot, g_resumable = true;
    } else if (p == Pending::New) NewCityGame(g_pendingSeed), g_currentSlot = -1, g_resumable = true;
    else if (p == Pending::Example) NewExampleGame(g_pendingSeed), g_currentSlot = -1, g_resumable = true;
    g_loadStart = tbd::platform::TickCountMs();
}

// TBD_MAP=example TBD_EXAMPLE_SAVE=<seconds>: writes the example city as the bundled template, then quits.
void ExampleSaveTick() {
    static const int seconds = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_EXAMPLE_SAVE", v, sizeof(v)) > 0 ? std::max(1, atoi(v)) : 0; }();
    if (!seconds || g_exampleFounding) return;
    static uint64_t start = 0;
    if (!start) start = tbd::platform::TickCountMs();
    if (tbd::platform::TickCountMs() - start < (uint64_t)seconds * 1000ull) return;
    if (!SaveCityGame(sv::kScratchSlot)) { tbd::shell::Quit(); return; }
    std::error_code ec;
    const std::filesystem::path to = tbd::platform::FsPath(tbd::Join(tbd::ExeDirectory(), L"..\\assets_src\\saves_template")); // the repo beside bin
    std::filesystem::create_directories(to, ec);
    const sv::Store st = Store();
    for (const char* ext : {".tbdgame", ".hfd"}) std::filesystem::copy_file(st.File(sv::kScratchSlot, ext), to / (std::string("bayside") + ext), std::filesystem::copy_options::overwrite_existing, ec);
    sv::Meta m;
    m.used = true, m.name = "Bayside", m.map = "Bayside archipelago", m.saved = (int64_t)std::time(nullptr), m.minutes = g_clock.minutes;
    m.population = (long long)g_simTotals.population, m.money = (long long)(g_simEcon.treasury / 100), m.example = true;
    sv::WriteFile(to / "bayside.meta", sv::SerializeMeta(m));
    tbd::Log("Example city written as the Save 1 template: %zu nodes, %zu lots, population %lld", g_roads.net.nodes.size(), g_zones.lots.size(), m.population);
    tbd::shell::Quit();
}
} // namespace title_host

// Called by SaveCityGame (city_game.inl) after a slot was written.
void SaveSlotMeta(int slot) {
    if (slot >= 0 && slot < tbd::saves::kSlots) title_host::WriteMeta(slot), title_host::g_currentSlot = slot;
}

// StartTerrainSandbox: the slots, then the splash over the most recent save (or a new map); without the front end, as before.
void TitleBoot(int seed) {
    using namespace title_host;
    const sv::Store st = Store();
    const int moved = st.MigrateLegacy();
    const int exampleSlot = st.CopyTemplate(TemplateDir(), "bayside"); // Save 1, or the first empty slot when Save 1 is taken
    tbd::Log("Saves: %s, %d old save(s) moved into the numbered slots, example city %s%d", tbd::WideToUtf8(GameSavesDir()).c_str(), moved,
             exampleSlot >= 0 ? "copied into Save " : "not copied (done before, no empty slot or no template), slot ", exampleSlot + 1);
    char mv[16] = {};
    const bool example = tbd::platform::GetEnvA("TBD_MAP", mv, sizeof(mv)) > 0 && mv[0] == 'e';
    if (!Enabled()) {
        if (example) NewExampleGame(tbd::platform::GetEnvA("TBD_SEED", nullptr, 0) > 0 ? seed : 2026);
        else NewCityGame(seed);
        return;
    }
    char shot[24] = {};
    if (tbd::platform::GetEnvA("TBD_TITLE_SHOT", shot, sizeof(shot)) > 0) g_shot = shot;
    const int recent = st.MostRecent();
    if (recent >= 0) g_pending = Pending::Load, g_pendingSlot = recent;
    else g_pending = Pending::New, g_pendingSeed = seed;
    NewCityGame(seed); // something under the splash until the pending load runs (its first frames)
    g_resumable = false;
    Show(ti::Screen::Splash);
}

// The pause menu's Save / Load: the picker over the game (the pause menu comes back when it closes).
void gdock::TitleOpenPicker(bool save) {
    using namespace title_host;
    if (!Enabled()) { g_gameAction = save ? 1 : 2; return; }
    g_st.pick = save ? ti::Pick::Save : ti::Pick::Load;
    g_st.back = ti::Screen::None;
    g_inGame = true, g_resumable = true;
    RefreshSlots();
    g_st.Go(ti::Screen::Slots);
}
// The pause menu's Main menu: the title over the city being played.
void gdock::TitleFromGame() {
    using namespace title_host;
    if (!Enabled()) { g_gameAction = 3; return; }
    gdock::g_state.menu = false;
    g_inGame = true, g_resumable = true;
    SaveCam();
    RefreshSlots();
    Show(ti::Screen::Title);
}
// The logo below the GUI atlas (gui/glass_dock_host.inl LoadAtlas): the texture grows, every cell's v is rescaled.
void gdock::TitleAppendLogo(std::vector<uint8_t>& rgba, int w, int& h, tbd::gui::dock::Atlas& atlas) {
    std::string png;
    for (const std::wstring& dir : {tbd::Join(g_engine.assetsRoot, L"branding"), tbd::Join(g_engine.assetsRoot, L"..\\assets_src\\branding"),
                                    tbd::Join(tbd::ExeDirectory(), L"..\\assets_src\\branding")}) // a project's Assets, else the repo beside the exe
        if (tbd::ReadAllText(tbd::Join(dir, L"optima_engine_1024.png"), png)) break;
    std::vector<uint8_t> logo;
    int lw = 0, lh = 0;
    if (png.empty() || !tbd::DecodeImage((const uint8_t*)png.data(), png.size(), logo, lw, lh) || lw > w || lw <= 0) {
        tbd::Log("Title: no branding/optima_engine_1024.png: the logo is drawn as text");
        return;
    }
    const int top = h + 2, H2 = top + lh + 2;
    rgba.resize((size_t)w * (size_t)H2 * 4, 0);
    for (int y = 0; y < lh; ++y) std::memcpy(&rgba[((size_t)(top + y) * (size_t)w) * 4], &logo[(size_t)y * (size_t)lw * 4], (size_t)lw * 4);
    const float k = (float)h / (float)H2;
    for (tbd::gui::dock::Sprite& sp : atlas.cells) sp.v0 *= k, sp.v1 *= k;
    atlas.index["logo"] = (int)atlas.cells.size();
    atlas.cells.push_back({0.0f, (float)top / H2, (float)lw / w, (float)(top + lh) / H2, (float)lw, (float)lh});
    h = H2;
    tbd::Log("Title: logo %dx%d appended to the GUI atlas (now %dx%d)", lw, lh, w, h);
}

// One frame of the front end, from DrawGameGui before the dock. True: it owns the screen this frame (no dock).
bool TitleGuiFrame(tbd::Renderer& r, tbd::gui::CityHud& hud) {
    using namespace title_host;
    (void)hud;
    ExampleSaveTick(); // (with the front end off too: TBD_MAP turns it off)
    if (!Enabled()) return false;
    if (g_st.screen == ti::Screen::None) return false;
    static bool inited = false;
    if (!inited) { // the dock's settings before its first frame (sound, text size, the Settings pages): as gdock::Frame's first frame
        inited = true;
        gdock::BuildCatalogue();
        gdock::LoadSettingsFile();
        gdock::LoadGuiPrefs();
        gdock::g_gfxPrev = tbd::gfx::Serialize(gdock::g_model.gfx);
        gdock::ApplySettings(r);
        RefreshSlots();
    }
    gdock::FillSafe(g_gui.Scale());
    g_m.safe = gdock::g_model.safe;
    g_m.sound = gdock::g_model.sound;
    g_m.pad = gdock::g_state.pad;
    g_m.text = gdock::g_model.textSize ? 1.15f : 1.0f;
    g_m.logoCell = gdock::g_atlas.Find("logo");
#if defined(__APPLE__)
    g_m.canQuit = false;
#else
    g_m.canQuit = true;
#endif
    g_m.inGame = g_inGame;
    g_m.backdrop = g_resumable && g_pending == Pending::None;
    g_m.version = "Optima Engine  -  " __DATE__;
    // ---- held screens for captures
    if (!g_shot.empty()) {
        static bool set = false;
        if (!set) {
            set = true;
            if (g_shot == "title" || g_shot == "settings") g_st.Go(ti::Screen::Title);
            if (g_shot == "settings") gdock::g_state.menu = gdock::g_state.settings = true, g_st.Go(ti::Screen::Settings);
            if (g_shot == "load" || g_shot == "save") g_st.pick = g_shot == "save" ? ti::Pick::Save : ti::Pick::Load, g_currentSlot = 0, RefreshSlots(), g_st.Go(ti::Screen::Slots);
            if (g_shot == "loading") g_st.Go(ti::Screen::Loading), g_st.t = 1.0f;
        }
        if (g_pending != Pending::None) RunPending(), RefreshSlots();
        if (g_shot == "splash" || g_shot == "loading") g_st.t = std::max(g_st.t, 1.4f), g_m.progress = 0.62f, g_m.step = "Shaping the terrain  62%";
    }
    // ---- loading: run the pending load once the screen is black, then wait for the world to settle
    if ((g_st.screen == ti::Screen::Splash || g_st.screen == ti::Screen::Loading) && g_shot.empty()) {
        if (g_pending != Pending::None && g_st.t > (g_st.screen == ti::Screen::Splash ? 0.15f : 0.3f)) RunPending();
        float p = 0;
        std::string step;
        const bool done = Settled(p, step);
        g_shownProgress = std::max(g_shownProgress, g_shownProgress + (p - g_shownProgress) * std::min(1.0f, g_gui.In().dt * 6.0f));
        g_m.progress = g_shownProgress, g_m.step = step;
        const uint64_t since = g_loadStart ? tbd::platform::TickCountMs() - g_loadStart : 0;
        const bool timeUp = since > 25000; // never stuck: a world that keeps streaming opens anyway
        const float minT = g_st.screen == ti::Screen::Splash ? 1.8f : 0.9f;
        if (g_pending == Pending::None && g_loadStart && g_st.leaveT < 0 && g_st.t > minT && (done || timeUp)) {
            if (g_st.screen == ti::Screen::Splash) {
                SaveCam();
                RefreshSlots();
                g_inGame = false;
                g_st.Go(ti::Screen::Title);
            } else {
                g_inGame = true;
                g_m.progress = 1.0f;
                Leave();
            }
        }
    }
    // ---- the title's backdrop: a slow drift round the city
    if ((g_st.screen == ti::Screen::Title || g_st.screen == ti::Screen::Settings || g_st.screen == ti::Screen::Slots) && g_camSaved && g_m.backdrop) {
        static float drift = 0;
        drift += g_gui.In().dt;
        g_engine.camera = g_cam;
        g_engine.camera.yaw = g_cam.yaw + drift * 0.025f;
    }
    ti::Out o;
    ti::Frame(g_gui, g_st, g_m, gdock::g_atlas, o);
    for (float p : o.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
    if (g_st.screen == ti::Screen::Settings) { // the dock's Settings pages over the dimmed title
        tbd::gui::dock::Out mo;
        tbd::gui::dock::MenuFrame(g_gui, gdock::g_state, gdock::g_model, gdock::g_atlas, mo, true);
        for (float p : mo.sfx) tbd::audio::PlayUi("sfx/ui_category_select", 0.6f, p);
        if (mo.settingsChanged) {
            const std::string now = tbd::gfx::Serialize(gdock::g_model.gfx);
            if (now != gdock::g_gfxPrev) gdock::g_gfxPrev = now, gdock::g_settingsTouched = true;
            gdock::ApplySettings(r);
            gdock::SaveSettingsFile();
        }
        if (!gdock::g_state.settings || !gdock::g_state.menu) gdock::g_state.menu = gdock::g_state.settings = false, g_st.Go(ti::Screen::Title);
    }
    if (o.left) {
        g_st.screen = ti::Screen::None;
        if (!gdock::g_menuPaused) g_clock.paused = false;
        return false;
    }
    const int randomSeed = (int)(tbd::platform::TickCountMs() % 900000ull) + 100;
    if (o.cont) {
        if (g_resumable) Leave(), g_inGame = true;
        else if (g_m.continueSlot >= 0) StartLoading(Pending::Load, g_m.continueSlot, 0);
    }
    if (o.newGame) StartLoading(Pending::New, -1, randomSeed);
    if (o.settings) gdock::g_state.menu = gdock::g_state.settings = true, gdock::g_state.gfx = gdock::g_state.snd = false, gdock::g_state.menuT = 0, g_st.Go(ti::Screen::Settings);
    if (o.quit) tbd::shell::Quit();
    if (o.load >= 0) gdock::g_state.menu = false, StartLoading(Pending::Load, o.load, 0);
    if (o.save >= 0) {
        const bool ok = SaveCityGame(o.save); // writes the metadata too (SaveSlotMeta)
        RefreshSlots();
        if (ok && g_st.back == ti::Screen::None) g_st.screen = ti::Screen::None; // back to the pause menu
    }
    if (o.del >= 0) {
        Store().Delete(o.del);
        if (g_currentSlot == o.del) g_currentSlot = -1;
        Toast("SAVE " + std::to_string(o.del + 1) + " DELETED");
        RefreshSlots();
    }
    if (o.back) {
        if (g_st.back == ti::Screen::None) g_st.screen = ti::Screen::None; // the pause menu is under it still
        else g_st.Go(g_st.back);
    }
    return g_st.screen != ti::Screen::None;
}
