// The game: engine core, scene loading, terrain, city tools and sim, frame rendering, viewport input.
// Textually included inside an anonymous namespace by the platform shell (Windows: src/main.cpp).
// No Windows code here: what the game needs from its host goes through src/game/shell.h and src/platform.

enum class Drag { None, Orbit, Pan, Zoom, Transform, Fly, Terrain };
enum class Api { D3D12, Vulkan, Metal };

enum Command : unsigned {
    IDM_NEW = 100, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXIT,
    IDM_DUPLICATE, IDM_DELETE,
    IDM_POPOUT, IDM_BROWSER, IDM_RESETCAM, IDM_FRAME,
    IDM_TERRAIN, IDM_NEWSEED, IDM_RADIUS_NEAR, IDM_RADIUS_MED, IDM_RADIUS_FAR,
    IDM_D3D12, IDM_VULKAN,
    IDM_GFX_FULL, IDM_GFX_FAST, IDM_GFX_UNLIT, IDM_GFX_CYCLE, IDM_BRIGHT_DN, IDM_BRIGHT_UP, IDM_BRIGHT_SLIDER,
    IDM_SUN_SKY, IDM_SUN_ANIMATE,
    IDM_SUN_SUNRISE, IDM_SUN_MORNING, IDM_SUN_NOON, IDM_SUN_AFTERNOON, IDM_SUN_SUNSET, IDM_SUN_NIGHT,
    IDM_SUN_SHADOWS,
    IDM_PROFILER,
    IDM_RELOADSHADERS,
    IDM_REBUILDGAME,
    IDM_GENERATE_LODS,
};

// One draw range of an imported mesh that has its own material textures.
struct MatPart {
    tbd::MeshId mesh = tbd::kInvalidMesh;
    size_t first = 0, count = 0; // vertex range in the entity's flat list
    tbd::TextureId albedo = tbd::kNoTexture, normal = tbd::kNoTexture, surface = tbd::kNoTexture;
    tbd::Sphere bounds; // in the entity's own space
    float lodMin = 0.0f, lodMax = 0.0f; // terrain sets: meters from the camera this part draws at (lodMax 0: always)
    bool caster = true;                 // terrain sets: only one set draws into the shadow maps
    bool lodChain = false;              // one level of an automatic LOD chain (mesh_lod.h); its levels share `bounds`
    bool interior = false;              // inside one of the building's rooms: no LODs, gone once the shell is past its full level
    float lodError = 0.0f, lodCoarser = -1.0f; // that level's error and the next coarser one's, own-space meters; -1: the last
    uint64_t roomMask = 0;              // interior parts: bit k set when the part reaches into the entity's room k (0: unknown, always draws)
    bool hair = false;                  // Wyatt's hair cards from the importer: never a shadow caster
    bool eye = false;                   // an eyeball part (Character Creator swaps its texture)
    bool opaque = false;                // nothing of it is ever cut out or discarded: it may go into the depth pre-pass
    std::vector<tbd::WindowPortal> portals; // glass panes in the entity's own space, for the probes (window_portals.h)
    std::vector<tbd::InteriorRoom> rooms;   // the building's storeys in the entity's own space, on the first part with panes
};

// Automatic LODs: a level draws while its error is at most TBD_LODPX pixels (default 1) and the next coarser one's is not, so
// exactly one level of a chain shows, chosen by its size on screen; the window height and field of view count, the distance
// alone does not. Off by default while building chains at load is slow; TBD_AUTOLOD=1 builds them for every static part.
// TBD_LODSTEP: the triangle ratio between levels (default 0.5).
inline bool AutoLodOn() {
    static const bool on = [] { char b[8] = {}; return tbd::platform::GetEnvA("TBD_AUTOLOD", b, 8) > 0 && b[0] == '1'; }();
    return on;
}
inline float EnvFloat(const char* name, float fallback, float lo, float hi) {
    char b[32] = {};
    if (tbd::platform::GetEnvA(name, b, sizeof(b)) == 0) return fallback;
    char* end = nullptr;
    const float f = strtof(b, &end);
    return end == b ? fallback : std::clamp(f, lo, hi);
}
inline float LodStep() { static const float step = EnvFloat("TBD_LODSTEP", 0.5f, 0.1f, 0.9f); return step; }
inline float LodPixels() { static const float px = EnvFloat("TBD_LODPX", 1.0f, 0.1f, 32.0f); return px; }
// `world` is the part's sphere under the entity's matrix, `pixelScale` pixels per meter at one meter.
inline bool LodShows(const MatPart& p, const tbd::Sphere& world, const tbd::Vec3& eye, float pixelScale) {
    if (!p.lodChain) return true;
    const tbd::Vec3 away = world.center - eye;
    const float d = std::max(std::sqrt(tbd::Dot(away, away)) - world.radius, 0.1f); // the nearest point of the part
    const float perMeter = (p.bounds.radius > 0.0f ? world.radius / p.bounds.radius : 1.0f) * pixelScale / d;
    return p.lodError * perMeter <= LodPixels() && (p.lodCoarser < 0.0f || p.lodCoarser * perMeter > LodPixels());
}

// `p` inside `room` (both in the same space): the room's x and z are half extents, as is its half height.
inline bool InsideRoom(const tbd::InteriorRoom& room, const tbd::Vec3& p, float margin = 0.0f) {
    const tbd::Vec3 d = p - tbd::Vec3{room.center[0], room.center[1], room.center[2]};
    const tbd::Vec3 x = {room.x[0], room.x[1], room.x[2]}, z = {room.z[0], room.z[1], room.z[2]};
    const float xx = tbd::Dot(x, x), zz = tbd::Dot(z, z);
    if (xx <= 0.0f || zz <= 0.0f) return false;
    const float lx = std::sqrt(xx), lz = std::sqrt(zz);
    return std::fabs(tbd::Dot(d, x)) / lx <= lx - margin && std::fabs(tbd::Dot(d, z)) / lz <= lz - margin && std::fabs(d.y) <= room.halfHeight - margin;
}
// A part that fits inside a room, a little clear of its walls, is furniture or fittings, not the shell.
inline bool PartIsInterior(const tbd::Sphere& bounds, const std::vector<tbd::InteriorRoom>& rooms) {
    for (const tbd::InteriorRoom& r : rooms)
        if (bounds.radius < r.halfHeight * 2.0f && InsideRoom(r, bounds.center, 0.25f)) return true;
    return false;
}

// Buildings and boats whose rooms were not found (no glass): their own box, the shell. A part counts as interior only when its
// whole sphere sits inside, clear of the box by 30 cm, so roof and wall pieces always stay with the shell.
inline bool IsShellAsset(const std::string& path) {
    return path.find("\\Buildings\\") != std::string::npos || path.find("\\Boats\\") != std::string::npos;
}
inline bool PartInsideShellBox(const tbd::Sphere& part, const tbd::Vec3& lo, const tbd::Vec3& hi) {
    const float m = part.radius + 0.3f;
    return part.center.x - m >= lo.x && part.center.x + m <= hi.x && part.center.y - m >= lo.y && part.center.y + m <= hi.y &&
           part.center.z - m >= lo.z && part.center.z + m <= hi.z;
}

// Mesh paths World > Generate LODs asked for; read by the build workers. See LodLevelsFor.
std::mutex g_lodMutex;
std::set<std::string> g_lodRequested;
std::wstring g_lodCacheDir; // <assets>\LodCache, made on first use
inline bool LodRequested(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_lodMutex);
    return g_lodRequested.count(path) != 0;
}

// The coarser levels of one welded part: from the LOD cache when a file exists, else generated (and saved) when the part's
// mesh was asked for or TBD_AUTOLOD=1, else none. Any thread.
inline std::vector<tbd::LodLevel> LodLevelsFor(const std::string& path, const std::vector<tbd::Vertex>& vertices, const std::vector<uint32_t>& indices, float radius) {
    std::vector<tbd::LodLevel> levels;
    if (indices.size() / 3 < 256 || g_lodCacheDir.empty()) return levels;
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.lod", (unsigned long long)tbd::LodContentHash(vertices, indices));
    const std::wstring file = g_lodCacheDir + L"\\" + name;
    if (tbd::LoadLodLevels(file, levels)) return levels;
    levels.clear();
    if (!AutoLodOn() && !LodRequested(path)) return levels;
    int64_t t0, t1;
    t0 = tbd::platform::Ticks();
    levels = tbd::BuildLodChain(vertices, indices, LodStep(), radius);
    const bool pieces = levels.empty(); // a kit of small hard-edged pieces: no edge may collapse, but whole pieces can go
    if (pieces) levels = tbd::BuildPieceLevels(vertices, indices, radius);
    t1 = tbd::platform::Ticks();
    { std::error_code ec; std::filesystem::create_directory(tbd::platform::FsPath(g_lodCacheDir), ec); } // like CreateDirectoryW: one level
    tbd::SaveLodLevels(file, levels);
    int64_t freq;
    freq = tbd::platform::TickFrequency();
    const double ms = (t1 - t0) * 1000.0 / (double)freq;
    tbd::Log("LOD: %s, %zu triangles: %zu %slevels%s in %.0f ms", path.c_str(), indices.size() / 3, levels.size(), pieces && !levels.empty() ? "piece " : "",
             levels.empty() ? "" : (" down to " + std::to_string(levels.back().indices.size() / 3)).c_str(), ms);
    return levels;
}

// What an entity or part is visible to this frame.
enum VisibleTo : uint8_t { kVisibleCamera = 1, kVisibleSun = 2, kVisibleTight = 4 };

// An InstanceSet on the GPU: one mesh (or its material parts), one instance buffer, split into spatial cells.
struct InstancedAsset {
    tbd::MeshId mesh = tbd::kInvalidMesh;
    tbd::InstanceBufferId buffer = tbd::kNoInstances;
    std::vector<tbd::InstanceCell> cells;
    float maxDistance = 0.0f, shadowDistance = 0.0f;
    struct Range { uint32_t first, count; };
    std::vector<Range> visible, casters; // per-frame, written by one cull batch each
};

// One draw of the scene's sorted list.
struct SceneDraw {
    tbd::MeshId mesh;
    tbd::TextureId albedo, normal, surface;
    uint32_t entity;
    uint32_t order; // near to far in steps, big terrain last
    bool opaque = false; // drawn in the depth pre-pass first (MatPart::opaque)
};

struct Engine {
    bool running = false;
    int dpi = 96;

    Api api = Api::D3D12;
    bool validation = false;
    std::unique_ptr<tbd::Renderer> renderer;
    std::unique_ptr<tbd::JobSystem> jobs; // worker threads for file import, image decode and per-frame matrices (never the renderer)
    std::vector<tbd::MeshId> meshes; // one per scene entity
    std::vector<tbd::Mat4> models;   // per-frame slots, one per scene entity: world matrix, filled in parallel
    std::vector<tbd::Mat4> mvps;     // per-frame slots: scene view-projection * model
    std::map<tbd::MeshId, std::vector<MatPart>> matParts; // textured entities draw these instead of the whole mesh
    std::map<std::string, std::vector<std::pair<tbd::TextureId, tbd::TextureId>>> partTextures; // per imported file
    std::map<uint64_t, tbd::TextureId> texturesByContent; // files often embed the same image: one GPU copy each
    std::map<tbd::MeshId, std::vector<MatPart>> sourceParts; // the file's own ranges of an entity whose matParts were merged
    std::vector<tbd::Sphere> meshBounds; // by mesh id, in the mesh's own space; only entity and instanced meshes are set
    std::vector<InstancedAsset> instanced;
    std::vector<uint8_t> entityVisible, partVisible; // per-frame VisibleTo flags; partVisible is flat, see partOffsets
    std::vector<uint32_t> partOffsets;               // per entity: where its parts start in partVisible
    std::vector<SceneDraw> sceneDraws;               // per-frame, reused
    tbd::TextureId skinSurface = tbd::kNoTexture; // the project's packed skin map, shared by all skin parts
    bool skinSurfaceTried = false;
    int vpW = 0, vpH = 0;

    tbd::Stats stats;
    std::vector<uint8_t> sunMoving; // per entity: animated, so never part of the kept sun map
    tbd::ToolSet tools;
    tbd::OrbitCamera camera;
    tbd::Scene scene;
    tbd::Terrain terrain;
    tbd::Sun sun;
    tbd::Gizmo gizmo;
    tbd::SkeletalAnimator animator; // poses and skins entities that have an animation clip
    float playStartHour = -1.0f; // toolbar Play: the sun's hour when playback started (Stop returns to it); < 0 = not playing

    std::wstring projectFolder;
    std::wstring assetsRoot;
    std::wstring scenePath; // empty = unsaved
    std::wstring titleBase;
    int nextEntityId = 1;

    float flySpeed = 14.0f; // world units per second
    int64_t lastTick = {};
    int64_t tickFreq = {};

    std::string toast;
    uint64_t toastUntil = 0;

    Drag drag = Drag::None;
    int dragButton = -1; // the button that started it: 0 left, 1 right, 2 middle
    int lastX = 0, lastY = 0;
};

Engine g_engine;
tbd::profiler::Overlay g_profiler; // F3; does nothing while off, and never on in a TBD_SHOT / TBD_CAPTURE run
// Draws and triangles by kind (F3's DRAWS BY KIND, TBD_DRAWCATS=1 HUD line, the 600-frame scene log): the renderer's counters
// read before and after each part of the scene. Shadow casters come from FrameCounts' own shadow counters. UI is what the
// frame drew after the scene (post passes, grid, gizmos, HUD, GUI). Shown one frame late, since UI ends after the overlay draws.
enum DrawCat : int { kCatTerrain, kCatVegetation, kCatGroundGrid, kCatProps, kCatZoned, kCatEntities, kCatPrepass, kCatRoads, kCatRail,
                     kCatCity, kCatAgents, kCatWater, kCatUi, kCatShadow, kCatCount };
inline const char* const kDrawCatNames[kCatCount] = {"TERRAIN", "VEGETATION", "GROUND GRID", "PROPS INSTANCED", "ZONED BUILDINGS", "OTHER ENTITIES",
                                                     "DEPTH PREPASS", "ROADS", "RAIL", "CITY TOOLS", "TRAFFIC", "WATER", "UI AND POST", "SHADOW"};
struct DrawCats {
    uint32_t draws[kCatCount] = {};
    uint64_t tris[kCatCount] = {};
    void Add(int cat, const tbd::FrameCounts& before, const tbd::FrameCounts& after) {
        draws[cat] += after.draws - before.draws;
        tris[cat] += after.triangles - before.triangles;
    }
};
DrawCats g_drawCats, g_drawCatsShown; // this frame's, and the last finished frame's
tbd::PlayMode g_play; // the toolbar's Play: first-person fly / walk camera, see play_mode.h
tbd::PhysicsWorld g_physics;     // made on the first Play, never in a TBD_SHOT / TBD_CAPTURE run; see physics.h
tbd::ai::NpcSystem g_npcs;       // AI agents while playing; see ai/npc_system.h
tbd::PlayerCharacter g_player;   // Wyatt on a capsule while playing; see player_character.h
tbd::PlayerScript g_playtest;    // TBD_PLAYTEST
tbd::script::LuauHost g_script;  // Luau gameplay scripts from <project>\Scripts; see docs/scripting_spec.md
std::vector<tbd::Entity> g_gamePlaySnapshot; // the scene's transforms when Play started, put back at Stop
double g_lastAnimMs = 0.0;       // poses and skinning, last frame, for the playtest's trace

const char* ApiName(Api api) { return api == Api::D3D12 ? "D3D12" : api == Api::Metal ? "METAL" : "VULKAN"; }
int Sx(int v) { return tbd::platform::MulDiv(v, g_engine.dpi, 96); }
float DpiScale() { return (float)g_engine.dpi / 96.0f; }

void Toast(const std::string& text) {
    g_engine.toast = text;
    g_engine.toastUntil = tbd::platform::TickCountMs() + 3500;
}

// ------------------------------------------------------------------- terrain editor
// T toggles it. Left drag does the toolbar's mode (raise by default); Shift digs and Ctrl flattens to the height first
// clicked whatever the mode. [ and ] change the radius. F8 saves the edits, F11 loads them (project folder\terrain_edits.hfd,
// or next to the exe without a project).
struct TerrainEdit {
    bool on = false;
    int mode = 0; // tbd::gui::TerrainBrushMode: 0 raise, 1 dig, 2 flatten (the in-game toolbar sets it)
    float radius = 30.0f;
    float metresPerSecond = 6.0f; // raise / dig speed at the brush centre
    float flattenTarget = 0.0f;
    int mouseX = -1, mouseY = -1; // last cursor position in the viewport, for the brush ring
} g_terrainEdit;

// The mode a left drag paints with right now: Ctrl and Shift still override the toolbar, as they did before it existed.
int TerrainBrushModeNow() {
    if (tbd::platform::KeyDownSync(tbd::platform::Key::Control)) return (int)tbd::gui::TerrainBrushMode::Flatten;
    if (tbd::platform::KeyDownSync(tbd::platform::Key::Shift)) return (int)tbd::gui::TerrainBrushMode::Dig;
    return g_terrainEdit.mode;
}

// ------------------------------------------------------------------- in-game GUI (src/gui, docs/gui.md)
// Shown in the terrain sandbox (and whenever terrain edit is on). U hides / shows it. Off in TBD_SHOT / TBD_CAPTURE runs
// so captures stay as they were, unless TBD_GUI=1. Window messages reach it first in ViewportProc (GuiMouse).
tbd::gui::Context g_gui;
tbd::gui::InputQueue g_guiInput;
bool g_guiVisible = [] {
    char v[8] = {};
    if (tbd::platform::GetEnvA("TBD_GUI", v, sizeof(v)) > 0) return v[0] != '0';
    return !tbd::platform::GetEnvA("TBD_SHOT", nullptr, 0) && !tbd::platform::GetEnvA("TBD_CAPTURE", nullptr, 0);
}();
bool g_guiOwnsButton = false;  // the GUI took the last left press: its moves and the release are not the engine's
bool g_terrainSandbox = false; // the project opened as a terrain sandbox

// ------------------------------------------------------------------- city roads (src/city, gui/road_toolbar.h)
// The road tool and network (platform free) plus their GPU side: one mesh per road material, the road textures from
// assets\roads\tex (tools/road_gen). Rebuilt before the frame opens whenever the network's version moves. The "Roads"
// button at the bottom of the game GUI opens the tool; TBD_ROADS=1 opens it at startup, TBD_ROADS=demo also builds a
// sample town (straights, curves, a roundabout, a bridge, a tunnel, a quay) for captures.
tbd::city::RoadTool g_roads;
struct RoadGpu {
    tbd::MeshId mesh[tbd::city::kMatCount];
    tbd::MeshId arrows = tbd::kInvalidMesh;
    tbd::TextureId albedo[tbd::city::kMatCount], normal[tbd::city::kMatCount];
    tbd::TextureId slab[tbd::city::tmodel::kSlabTexCount]; // SPOM building faces (transport_models.h kSlabTex)
    bool textures = false;
    uint64_t built = 0;
    size_t triangles = 0;
    RoadGpu() { Forget(); }
    void Forget() { // a new renderer: the old ids are gone with the old one
        for (int m = 0; m < tbd::city::kMatCount; ++m) mesh[m] = tbd::kInvalidMesh, albedo[m] = normal[m] = tbd::kNoTexture;
        for (tbd::TextureId& t : slab) t = tbd::kNoTexture;
        arrows = tbd::kInvalidMesh;
        textures = false;
        built = 0;
    }
} g_roadGpu;
tbd::Mat4 g_lastViewProj = tbd::Mat4::Identity(); // this frame's overlay matrix, for labels over the world

std::wstring TerrainEditPath() {
    const std::wstring dir = g_engine.projectFolder.empty() ? tbd::ExeDirectory() : g_engine.projectFolder;
    return tbd::Join(dir, L"terrain_edits.hfd");
}

// A project whose project.tbdproject contains "terrainSandbox": true opens as just terrain: no ground plane or box, no flat patch
// at the origin, the camera high over the middle of the map and the brush on.
bool ProjectWantsSandbox(const std::wstring& folder) {
    std::string text;
    if (folder.empty() || !tbd::ReadAllText(tbd::ProjectFilePath(folder), text)) return false;
    const size_t key = text.find("\"terrainSandbox\"");
    if (key == std::string::npos) return false;
    const size_t colon = text.find(':', key);
    return colon != std::string::npos && text.find("true", colon) != std::string::npos && text.find("true", colon) < colon + 8;
}

void SaveTerrainEdits();
void NewCityGame(int seed); // city_game.inl: the random north-land / south-sea map with the default corridor
void LoadTerrainEdits();
void RebuildSceneMeshes();

void StartTerrainSandbox() {
    g_terrainSandbox = true; // the in-game toolbar stays up even with edit off, so it can be turned back on
    g_engine.scene.entities.clear();
    g_engine.scene.selected = -1;
    g_engine.terrain.params.flatStart = false;
    g_engine.terrain.params.mapKind = 2; // the random city map (city_map.h); NewCityGame sets the seed and the default corridor
    RebuildSceneMeshes();
    g_terrainEdit.on = false;
    int seed = (int)(tbd::platform::TickCountMs() % 900000ull) + 100; // random every start; TBD_SEED=<n> pins it
    char sv[16] = {};
    if (tbd::platform::GetEnvA("TBD_SEED", sv, sizeof(sv)) > 0) seed = atoi(sv);
    NewCityGame(seed);
    { // TBD_BRUSHTEST="x z radius metres": one stroke at startup, so a capture can show the brush's effect without a mouse
        char v[96] = {};
        float x, z, radius, metres;
        if (tbd::platform::GetEnvA("TBD_BRUSHTEST", v, sizeof(v)) > 0 && sscanf(v, "%f %f %f %f", &x, &z, &radius, &metres) == 4)
            g_engine.terrain.Brush(x, z, radius, metres);
    }
}

void SaveTerrainEdits() {
    std::vector<uint8_t> bytes;
    g_engine.terrain.field.Save(bytes);
    FILE* f = tbd::platform::WOpen(TerrainEditPath().c_str(), L"wb");
    const bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f) fclose(f);
    Toast(ok ? "TERRAIN SAVED (" + std::to_string(g_engine.terrain.field.TileCount()) + " TILES)" : "TERRAIN SAVE FAILED");
}

void LoadTerrainEdits() {
    FILE* f = tbd::platform::WOpen(TerrainEditPath().c_str(), L"rb");
    if (!f) { Toast("NO SAVED TERRAIN"); return; }
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
    fclose(f);
    Toast(g_engine.terrain.field.Load(bytes.data(), bytes.size()) ? "TERRAIN LOADED" : "TERRAIN FILE IS DAMAGED");
}

// Draws only letters, digits and a few symbols, so file names are reduced to that.
std::string ToastName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    std::string s = tbd::WideToUtf8(cut == std::wstring::npos ? path : path.substr(cut + 1));
    for (char& c : s)
        if (c == '_') c = '-';
    return s;
}

// ----------------------------------------------------------------- renderer

// GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES: a file or folder is there.
bool FileExists(const std::wstring& path) { std::error_code ec; return std::filesystem::exists(tbd::platform::FsPath(path), ec); }

void UpdateTitle() {
    std::wstring title = g_engine.titleBase;
    if (!g_engine.scenePath.empty()) {
        const size_t cut = g_engine.scenePath.find_last_of(L"\\/");
        title += L" - " + g_engine.scenePath.substr(cut == std::wstring::npos ? 0 : cut + 1);
    }
    tbd::shell::SetTitle(title);
}

// Imported files, parsed once per path (duplicating an entity or reloading a scene doesn't re-read the file).
// A file that failed to load maps to an empty list; the entity then falls back to a cube.
std::map<std::string, tbd::ImportedModel>& ImportedModels() {
    static std::map<std::string, tbd::ImportedModel> models;
    return models;
}

std::map<std::string, std::vector<tbd::Vertex>>& ImportedVertexCache() {
    static std::map<std::string, std::vector<tbd::Vertex>> cache;
    return cache;
}

// Everything one file import produces. ImportFile fills it without touching any shared state, so it can run on a
// worker thread; PublishImport then moves it into the caches (and toasts) on the main thread.
bool SceneLoadActive(); // a scene's meshes are still arriving; see SceneLoadTick
void CompleteSceneLoad();
void GenerateOutdoorLods();
struct ImportResult {
    std::vector<tbd::Vertex> vertices;
    tbd::ImportedModel model;
    std::string error;
    bool ok = false;
    tbd::Vec3 halfExtents = {0.5f, 0.5f, 0.5f};
};

void ImportFile(const std::string& path, ImportResult& out) {
    // "<file>#native" keeps the file's own units and origin, so the parts of one character stay fitted together.
    const size_t nativeTag = path.rfind("#native");
    tbd::ImportOptions options;
    options.normalizeToUnitBox = nativeTag == std::string::npos;
    std::vector<tbd::Vertex>& v = out.vertices;
    if (!tbd::ImportMesh(path.substr(0, nativeTag), v, &out.error, options, &out.model)) return;
    // TBD_WRINKLE=0..1: a fixed wrinkle amount on all skin, for test captures of a face with no rig driving it.
    char wrinkle[16] = {};
    if (tbd::platform::GetEnvA("TBD_WRINKLE", wrinkle, sizeof(wrinkle)) > 0)
        for (tbd::Vertex& x : v)
            if (x.a == 0.5f) x.a = 0.5f + 0.09f * (float)atof(wrinkle);
    tbd::Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (const tbd::Vertex& x : v) {
        lo = {std::min(lo.x, x.px), std::min(lo.y, x.py), std::min(lo.z, x.pz)};
        hi = {std::max(hi.x, x.px), std::max(hi.y, x.py), std::max(hi.z, x.pz)};
    }
    out.halfExtents = {std::max((hi.x - lo.x) * 0.5f, 0.01f), std::max((hi.y - lo.y) * 0.5f, 0.01f),
                       std::max((hi.z - lo.z) * 0.5f, 0.01f)};
    out.ok = true;
}

// Main thread only: the only place the import caches, the half extents and the toast are written.
const std::vector<tbd::Vertex>& PublishImport(const std::string& path, ImportResult&& r) {
    std::vector<tbd::Vertex>& v = ImportedVertexCache()[path];
    v = std::move(r.vertices);
    ImportedModels()[path] = std::move(r.model); // a failed file keeps an (empty) entry, as before
    if (!r.ok) {
        Toast("IMPORT FAILED: " + r.error);
        return v;
    }
    tbd::ImportedHalfExtents()[path] = r.halfExtents;
    return v;
}

const std::vector<tbd::Vertex>& ImportedVertices(const std::string& path) {
    auto it = ImportedVertexCache().find(path);
    if (it != ImportedVertexCache().end()) return it->second;
    ImportResult r;
    ImportFile(path, r);
    return PublishImport(path, std::move(r));
}

// One image of an imported file, decoded ahead of time on a worker thread.
struct DecodedImage {
    std::shared_ptr<tbd::MipChain> mips; // made with the decode on a scene load's worker: the upload then never waits
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool ok = false;
};
// path -> image index -> pixels. Written and read on the main thread only; PartTextures consumes and drops an entry.
std::map<std::string, std::map<int, DecodedImage>>& DecodedImages() {
    static std::map<std::string, std::map<int, DecodedImage>> decoded;
    return decoded;
}

// FNV-1a over 8-byte words, plus the length: tells apart images, not adversaries.
uint64_t ContentHash(const std::vector<uint8_t>& bytes) {
    uint64_t h = 14695981039346656037ull ^ bytes.size();
    size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        uint64_t word;
        std::memcpy(&word, bytes.data() + i, 8);
        h = (h ^ word) * 1099511628211ull;
    }
    for (; i < bytes.size(); ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

std::vector<tbd::Vertex> EntityVertices(const tbd::Entity& e) {
    std::vector<tbd::Vertex> v;
    if (e.mesh == tbd::MeshKind::Imported) {
        v = ImportedVertices(e.meshPath);
        for (tbd::Vertex& x : v) if (!tbd::IsHairVertex(x.a)) { x.r *= e.color.x; x.g *= e.color.y; x.b *= e.color.z; }
        if (v.empty()) v = tbd::MakeMesh(tbd::MeshKind::Cube, e.color);
    } else {
        v = tbd::MakeMesh(e.mesh, e.color);
    }
    return v;
}
// An imported head's rest vertices and its closed-lid shape, <head>.lid_relaxed.glb beside it (FaceRig::SetLidRelax), both
// without hair cards (which the importer may add to either); false when there is no such file or the two do not line up.
bool LidRelaxedVertices(const tbd::Entity& e, std::vector<tbd::Vertex>* rest, std::vector<tbd::Vertex>* relaxed) {
    if (e.mesh != tbd::MeshKind::Imported) return false;
    std::string file = e.meshPath.substr(0, e.meshPath.find('#'));
    file = file.substr(0, file.find_last_of('.')) + ".lid_relaxed.glb";
    if (!FileExists(tbd::Utf8ToWide(file))) return false;
    tbd::Entity r = e;
    r.meshPath = file;
    const auto noHair = [](std::vector<tbd::Vertex> v) { v.erase(std::remove_if(v.begin(), v.end(), [](const tbd::Vertex& x) { return tbd::IsHairVertex(x.a); }), v.end()); return v; };
    *rest = noHair(EntityVertices(e));
    *relaxed = noHair(EntityVertices(r));
    return !rest->empty() && rest->size() == relaxed->size();
}

// Textures for each part of an imported file, decoded and uploaded once per renderer.
void RequestPartSurfaces(const tbd::ImportedModel& model, const std::vector<std::pair<tbd::TextureId, tbd::TextureId>>& textures);
const std::vector<std::pair<tbd::TextureId, tbd::TextureId>>& PartTextures(const std::string& path) {
    auto found = g_engine.partTextures.find(path);
    if (found != g_engine.partTextures.end()) return found->second;
    auto& out = g_engine.partTextures[path];
    auto mit = ImportedModels().find(path);
    if (mit == ImportedModels().end() || !g_engine.renderer) return out;
    const tbd::ImportedModel& model = mit->second;
    std::map<int, tbd::TextureId> made;
    const auto predecoded = DecodedImages().find(path); // filled by PrepareSceneImports; absent = decode here
    auto texture = [&](int image) {
        if (image < 0 || image >= (int)model.images.size()) return tbd::kNoTexture;
        auto it = made.find(image);
        if (it != made.end()) return it->second;
        const uint64_t content = ContentHash(model.images[(size_t)image]);
        if (const auto same = g_engine.texturesByContent.find(content); same != g_engine.texturesByContent.end()) {
            made[image] = same->second;
            return same->second;
        }
        tbd::TextureId id = tbd::kNoTexture;
        const DecodedImage* ready = nullptr;
        if (predecoded != DecodedImages().end()) {
            const auto d = predecoded->second.find(image);
            if (d != predecoded->second.end()) ready = &d->second;
        }
        if (ready) {
            if (ready->ok && ready->mips) id = g_engine.renderer->CreateTextureNoWait(ready->mips->levels, ready->mips->dims);
            else if (ready->ok) id = g_engine.renderer->CreateTexture(ready->rgba.data(), ready->w, ready->h);
        } else {
            std::vector<uint8_t> pixels;
            int w = 0, h = 0;
            if (tbd::DecodeImage(model.images[(size_t)image].data(), model.images[(size_t)image].size(), pixels, w, h))
                id = g_engine.renderer->CreateTexture(pixels.data(), w, h);
        }
        made[image] = id;
        g_engine.texturesByContent[content] = id;
        return id;
    };
    // Skin without its own normal map gets the project's tiling pore detail (the shader repeats it across the skin).
    tbd::TextureId pores = tbd::kNoTexture;
    bool poresTried = false;
    auto poreTile = [&]() {
        if (poresTried) return pores;
        poresTried = true;
        std::string bytes, unused;
        if (tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, L"skin\\skin_pores_tile.png"), bytes)) {
            std::vector<uint8_t> pixels;
            int w = 0, h = 0;
            if (tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), pixels, w, h))
                pores = g_engine.renderer->CreateTexture(pixels.data(), w, h);
        }
        return pores;
    };
    for (const tbd::ImportedPart& p : model.parts)
        out.push_back({texture(p.albedo), p.normal < 0 && p.skin ? poreTile() : texture(p.normal)});
    if (predecoded != DecodedImages().end()) DecodedImages().erase(path); // pixels live on the GPU now
    RequestPartSurfaces(model, out);
    return out;
}

#include "surface_async.inl"
#include "ground_bake.inl"
void RequestPartSurfaces(const tbd::ImportedModel& model, const std::vector<std::pair<tbd::TextureId, tbd::TextureId>>& textures) {
    const auto image = [&](int k) { return k >= 0 && k < (int)model.images.size() ? &model.images[(size_t)k] : nullptr; };
    for (size_t k = 0; k < model.parts.size() && k < textures.size(); ++k) {
        const tbd::ImportedPart& p = model.parts[k];
        if (p.skin || p.eye || p.hair || !image(p.albedo)) continue;
        if (p.groundSplat) ground_bake::NoteGround(textures[k].first);
        tbd::surface_maps::ExtraInputs extra;
        const auto share = [&](int k2) { return image(k2) ? std::make_shared<std::vector<uint8_t>>(*image(k2)) : std::shared_ptr<std::vector<uint8_t>>(); };
        extra.rough = share(p.rough); extra.occlusion = share(p.occlusion); extra.emissive = share(p.emissive);
        std::copy(p.emissiveFactor, p.emissiveFactor + 3, extra.emissiveFactor);
        extra.metallic = p.metallic;
        surface_async::Request(textures[k].first, textures[k].second, *image(p.albedo), image(p.normal), image(p.rough), p.art, extra);
    }
}


// Frees an entity's mesh and the per-material parts that go with it.
void DestroyEntityMesh(tbd::MeshId id) {
    if (!g_engine.renderer) return;
    g_engine.animator.Forget(id);
    auto it = g_engine.matParts.find(id);
    if (it != g_engine.matParts.end()) {
        for (const MatPart& p : it->second) g_engine.renderer->DestroyMesh(p.mesh);
        g_engine.matParts.erase(it);
    }
    g_engine.sourceParts.erase(id);
    if (id < g_engine.meshBounds.size()) g_engine.meshBounds[id] = tbd::Sphere{};
    g_engine.renderer->DestroyMesh(id);
}

void UploadMatParts(tbd::MeshId id, const std::vector<MatPart>& parts, const std::vector<tbd::Vertex>& v, bool dynamic = false) {
    std::vector<MatPart> made = parts;
    for (MatPart& p : made) {
        p.mesh = g_engine.renderer->CreateMesh(v.data() + p.first, (uint32_t)p.count, tbd::Topology::Triangles, dynamic);
        p.bounds = tbd::SphereAround(v.data() + p.first, p.count);
    }
    g_engine.matParts[id] = std::move(made);
}

void SetMeshBounds(tbd::MeshId id, const tbd::Sphere& bounds) {
    if (id == tbd::kInvalidMesh) return;
    if (id >= g_engine.meshBounds.size()) g_engine.meshBounds.resize((size_t)id + 1);
    g_engine.meshBounds[id] = bounds;
}

// A static entity's parts as few meshes as possible. Parts sharing a material become one mesh (a building's thousands of
// planks use a handful of textures), and a material with very many triangles is split into a grid so culling can drop most
// of it (terrain, baked scatter). Nothing is blended, so the changed draw order within an entity cannot show.
// Parts ready to upload: welded, made on any thread, then CreateMeshNoWait on the main thread.
struct BuiltPart { MatPart part; std::vector<tbd::Vertex> vertices; std::vector<uint32_t> indices; };

// Terrain-sized things split at 30k triangles into 128 m cells. A walkable building or boat (12 to 400 m across) splits much
// finer, in cells about a sixth of its size and at least 4 m, so a material that runs through every room is cut per room and
// only the rooms in view are drawn; small props stay one draw. TBD_ROOMSPLIT=0: the old whole-material parts, to compare.
inline void RoomSplit(const tbd::Sphere& whole, size_t& splitTriangles, float& splitCell) {
    static const bool on = [] { char f[4] = {}; return tbd::platform::GetEnvA("TBD_ROOMSPLIT", f, sizeof(f)) == 0 || f[0] != '0'; }();
    if (!on || whole.radius <= 6.0f || whole.radius >= 200.0f) return;
    splitTriangles = 2000;
    splitCell = std::max(4.0f, whole.radius / 3.0f);
}

// Coarser copies of each built part, appended after the full ones. Glass keeps its panes whole and hair cards stay as cut.
void AddAutoLods(std::vector<BuiltPart>& made, const std::string& path) {
    const size_t full = made.size();
    std::vector<tbd::InteriorRoom> rooms;
    for (const BuiltPart& b : made) rooms.insert(rooms.end(), b.part.rooms.begin(), b.part.rooms.end());
    const bool box = rooms.empty() && IsShellAsset(path);
    tbd::Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (size_t k = 0; box && k < full; ++k)
        for (const tbd::Vertex& x : made[k].vertices) { lo = {std::min(lo.x, x.px), std::min(lo.y, x.py), std::min(lo.z, x.pz)}; hi = {std::max(hi.x, x.px), std::max(hi.y, x.py), std::max(hi.z, x.pz)}; }
    for (size_t k = 0; k < full; ++k) {
        made[k].part.interior = made[k].part.portals.empty() && (box ? PartInsideShellBox(made[k].part.bounds, lo, hi) : PartIsInterior(made[k].part.bounds, rooms));
        if (made[k].part.interior) made[k].part.caster = false; // the shell shades the room; inside, the probes light it
        if (made[k].part.interior && !rooms.empty() && rooms.size() <= 64) // which rooms it reaches into, for the per-room culling
            for (size_t r = 0; r < rooms.size(); ++r)
                if (InsideRoom(rooms[r], made[k].part.bounds.center, -made[k].part.bounds.radius)) made[k].part.roomMask |= 1ull << r;
        if (made[k].part.interior || made[k].part.hair || !made[k].part.portals.empty() || made[k].part.lodMax > 0.0f) continue;
        std::vector<tbd::LodLevel> levels = LodLevelsFor(path, made[k].vertices, made[k].indices, made[k].part.bounds.radius);
        if (levels.empty()) continue;
        made[k].part.lodChain = true;
        made[k].part.lodCoarser = levels[0].error;
        const MatPart base = made[k].part;
        for (size_t l = 0; l < levels.size(); ++l) {
            BuiltPart b;
            b.part = base;
            b.part.lodError = levels[l].error;
            b.part.lodCoarser = l + 1 < levels.size() ? levels[l + 1].error : -1.0f;
            b.part.count = levels[l].indices.size();
            b.vertices = std::move(levels[l].vertices);
            b.indices = std::move(levels[l].indices);
            made.push_back(std::move(b));
        }
    }
}

void BuildMergedParts(const std::vector<MatPart>& parts, const std::vector<tbd::Vertex>& v, std::vector<BuiltPart>& made, const std::string& path) {
    size_t kSplitTriangles = 30000;
    float kSplitCell = 128.0f; // meters
    RoomSplit(tbd::SphereAround(v.data(), v.size()), kSplitTriangles, kSplitCell);
    using Material = std::tuple<tbd::TextureId, tbd::TextureId, tbd::TextureId>;
    std::map<Material, size_t> materialTriangles;
    for (const MatPart& p : parts) materialTriangles[{p.albedo, p.normal, p.surface}] += p.count / 3;
    // hair cards, never merged with skin; glass, kept whole for its panes; a distance band (hair detail levels) keeps its own part
    using Group = std::tuple<tbd::TextureId, tbd::TextureId, tbd::TextureId, int, int, bool, bool, float, float>;
    std::map<Group, std::vector<tbd::Vertex>> groups;
    for (const MatPart& p : parts) {
        const bool split = materialTriangles[{p.albedo, p.normal, p.surface}] > kSplitTriangles;
        Group lastKey{};
        std::vector<tbd::Vertex>* last = nullptr;
        for (size_t t = 0; t + 3 <= p.count; t += 3) {
            const tbd::Vertex* tri = v.data() + p.first + t;
            int cx = 0, cz = 0;
            if (split) {
                cx = (int)std::floor((tri[0].px + tri[1].px + tri[2].px) / (3.0f * kSplitCell));
                cz = (int)std::floor((tri[0].pz + tri[1].pz + tri[2].pz) / (3.0f * kSplitCell));
            }
            const Group key{p.albedo, p.normal, p.surface, cx, cz, p.hair, std::fabs(tri[0].a - tbd::kGlassAlpha) < 0.003f, p.lodMin, p.lodMax};
            if (!last || key != lastKey) { last = &groups[key]; lastKey = key; }
            last->insert(last->end(), tri, tri + 3);
        }
    }
    made.reserve(groups.size());
    for (const auto& [key, vertices] : groups) {
        BuiltPart built;
        built.part.albedo = std::get<0>(key);
        built.part.normal = std::get<1>(key);
        built.part.surface = std::get<2>(key);
        built.part.hair = std::get<5>(key);
        built.part.lodMin = std::get<7>(key);
        built.part.lodMax = std::get<8>(key);
        built.part.count = vertices.size();
        built.part.bounds = tbd::SphereAround(vertices.data(), vertices.size());
        built.part.portals = tbd::ExtractWindowPortals(vertices.data(), vertices.size());
        built.part.caster = !built.part.hair && built.part.portals.empty(); // glass lets the sun into a room
        tbd::WeldVertices(vertices.data(), (uint32_t)vertices.size(), built.vertices, built.indices);
        made.push_back(std::move(built));
    }
    for (BuiltPart& b : made)
        if (!b.part.portals.empty()) { b.part.rooms = tbd::ExtractInteriorRooms(v.data(), v.size()); break; }
    AddAutoLods(made, path);
}

// Main thread: one built part onto the GPU.
inline MatPart UploadBuiltPart(const BuiltPart& built) {
    MatPart part = built.part;
    part.mesh = g_engine.renderer->CreateMeshNoWait(built.vertices.data(), (uint32_t)built.vertices.size(), built.indices.data(), (uint32_t)built.indices.size());
    return part;
}

// Skin's packed roughness, cavity and scatter map, from the project's assets. kNoTexture (neutral shading) when absent.
tbd::TextureId SkinSurfaceMap() {
    if (g_engine.skinSurfaceTried) return g_engine.skinSurface;
    g_engine.skinSurfaceTried = true;
    std::string bytes;
    if (tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, L"skin\\skin_surface.png"), bytes)) {
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        if (tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), pixels, w, h))
            g_engine.skinSurface = g_engine.renderer->CreateTexture(pixels.data(), w, h);
    }
    return g_engine.skinSurface;
}

// A character folder may carry its own skin surface map (Assets\characters\regular\skin_surface.png: Wyatt's oil shine, cavity
// and blood maps packed for the default human); every other skin uses the project's shared one. One texture per folder.
std::map<std::string, tbd::TextureId> g_charSurfaces;
tbd::TextureId SkinSurfaceFor(const std::string& meshPath) {
    const std::string file = meshPath.substr(0, meshPath.find('#'));
    const size_t slash = file.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string() : file.substr(0, slash + 1);
    if (dir.empty() || !g_engine.renderer) return SkinSurfaceMap();
    auto it = g_charSurfaces.find(file); // per mesh first: a mesh with its own region mask has its own copy of the surface
    if (it == g_charSurfaces.end()) it = g_charSurfaces.find(dir);
    if (it != g_charSurfaces.end() && (it->first == file || !FileExists(tbd::Utf8ToWide(file.substr(0, file.find_last_of('.')) + ".skin_extra.png"))))
        return it->second != tbd::kNoTexture ? it->second : SkinSurfaceMap();
    // <mesh>.skin_extra.png beside the mesh: the extra map (SetSurfaceExtra) for that mesh's own UVs; its G picks the pore
    // tile per region (mesh.hlsl SkinPoreNormal). The surface is shared per folder, so such a mesh gets its own copy.
    std::string extraBytes;
    const bool hasExtra = tbd::ReadAllText(tbd::Utf8ToWide(file.substr(0, file.find_last_of('.')) + ".skin_extra.png"), extraBytes);
    tbd::TextureId id = tbd::kNoTexture;
    std::string bytes;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    // Such a mesh may also carry <mesh>.skin_surface.png in its own UVs, for when the folder's map is laid out for other meshes.
    const std::string own = file.substr(0, file.find_last_of('.')) + ".skin_surface.png";
    const bool ownSurface = hasExtra && tbd::ReadAllText(tbd::Utf8ToWide(own), bytes);
    if ((ownSurface || tbd::ReadAllText(tbd::Utf8ToWide(dir + "skin_surface.png"), bytes)) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), pixels, w, h)) {
        id = g_engine.renderer->CreateTexture(pixels.data(), w, h);
        tbd::Log("Skin surface: %s uses its own map (%dx%d)%s", ownSurface ? own.c_str() : dir.c_str(), w, h, hasExtra ? ", with a region mask" : "");
        if (hasExtra && tbd::DecodeImage((const uint8_t*)extraBytes.data(), extraBytes.size(), pixels, w, h))
            g_engine.renderer->SetSurfaceExtra(id, g_engine.renderer->CreateTexture(pixels.data(), w, h));
    }
    g_charSurfaces[hasExtra ? file : dir] = id;
    return id != tbd::kNoTexture ? id : SkinSurfaceMap();
}
bool IsSkinSurface(tbd::TextureId id) {
    if (id == tbd::kNoTexture) return false;
    if (id == SkinSurfaceMap()) return true;
    for (const auto& kv : g_charSurfaces) if (kv.second == id) return true;
    return false;
}

// Skin detail scale per character (Renderer::SetSkinDetailScale): how many times the pore normal repeats across that entity's skin
// atlas. Its skin materials are the parts that carry the skin surface map. Kept here by entity and applied again every frame,
// so it survives the renderer starting over and the entity's meshes being made again. Backend neutral; for scripts and tools.
std::map<int, float> g_skinDetailScale;
inline void ApplySkinDetailScale(int entity, float scale) {
    if (!g_engine.renderer || entity < 0 || (size_t)entity >= g_engine.meshes.size()) return;
    const tbd::TextureId skinMap = SkinSurfaceMap();
    const auto it = g_engine.matParts.find(g_engine.meshes[(size_t)entity]);
    if (skinMap == tbd::kNoTexture || it == g_engine.matParts.end()) return;
    size_t applied = 0;
    for (const MatPart& p : it->second)
        if (IsSkinSurface(p.surface) && p.albedo != tbd::kNoTexture) { g_engine.renderer->SetSkinDetailScale(p.albedo, scale); ++applied; }
    static std::set<int> logged;
    if (applied && logged.insert(entity).second)
        tbd::Log("Skin detail: %s, %zu skin materials at scale %.1f", g_engine.scene.entities[(size_t)entity].name.c_str(), applied, scale);
}
// scale <= 0 goes back to the default (48). False for an entity index that does not exist.
bool SetEntitySkinDetailScale(int entity, float scale) {
    if (entity < 0 || (size_t)entity >= g_engine.scene.entities.size()) return false;
    if (scale > 0.0f) g_skinDetailScale[entity] = scale;
    else g_skinDetailScale.erase(entity);
    ApplySkinDetailScale(entity, scale);
    return true;
}
// Pore depth per character (Renderer::SetSkinPoreDepth), kept and re-applied every frame like the detail scale above.
std::map<int, float> g_skinPoreDepth;
inline void ApplySkinPoreDepth(int entity, float depth) {
    if (!g_engine.renderer || entity < 0 || (size_t)entity >= g_engine.meshes.size()) return;
    const auto it = g_engine.matParts.find(g_engine.meshes[(size_t)entity]);
    if (SkinSurfaceMap() == tbd::kNoTexture || it == g_engine.matParts.end()) return;
    for (const MatPart& p : it->second)
        if (IsSkinSurface(p.surface) && p.albedo != tbd::kNoTexture) g_engine.renderer->SetSkinPoreDepth(p.albedo, depth);
}
// depth < 0 goes back to the default (1). False for an entity index that does not exist.
bool SetEntitySkinPoreDepth(int entity, float depth) {
    if (entity < 0 || (size_t)entity >= g_engine.scene.entities.size()) return false;
    if (depth >= 0.0f) g_skinPoreDepth[entity] = depth;
    else g_skinPoreDepth.erase(entity);
    ApplySkinPoreDepth(entity, depth);
    return true;
}
inline void ApplySkinDetailScales() {
    for (const auto& [entity, depth] : g_skinPoreDepth) ApplySkinPoreDepth(entity, depth);
    // TBD_SKINSCALE=<scale>: every entity's skin at that scale, for testing a look without a script.
    static const float all = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_SKINSCALE", v, sizeof(v)) > 0 ? (float)atof(v) : 0.0f; }();
    if (all > 0.0f)
        for (size_t i = 0; i < g_engine.meshes.size(); ++i) ApplySkinDetailScale((int)i, all);
    for (const auto& [entity, scale] : g_skinDetailScale) ApplySkinDetailScale(entity, scale);
}

#include "terrain_lods.inl"

float g_shrinkAtDistance = 0.0f; // meters, set around the upload of an instanced asset drawn to a short range
tbd::MeshId UploadEntityMesh(const tbd::Entity& e) {
    if (!g_engine.renderer) return tbd::kInvalidMesh;
    std::vector<tbd::Vertex> v;
    if (e.mesh == tbd::MeshKind::Imported) {
        v = ImportedVertices(e.meshPath);
        for (tbd::Vertex& x : v) if (!tbd::IsHairVertex(x.a)) { x.r *= e.color.x; x.g *= e.color.y; x.b *= e.color.z; } // color tints the file's (hair carries data there)
        if (g_shrinkAtDistance > 0.0f) // short-range instanced asset: the shader reads its reach from alpha (see VSInstanced)
            for (tbd::Vertex& x : v) x.a = 0.3f + g_shrinkAtDistance * 0.001f;
        if (v.empty()) v = tbd::MakeMesh(tbd::MeshKind::Cube, e.color);
    } else {
        v = tbd::MakeMesh(e.mesh, e.color);
    }
    if (e.mesh == tbd::MeshKind::Imported && !e.animClip.empty() && e.faceClip.empty()) {
        // Skinned and clip bound: one deformable mesh, its material parts sharing the vertices.
        const tbd::ImportedModel& model = ImportedModels()[e.meshPath];
        std::vector<tbd::SkinnedPartRange> ranges;
        const tbd::MeshId skinned = g_engine.animator.Add(*g_engine.renderer, e.animClip, model, v, ranges);
        if (skinned != tbd::kInvalidMesh) {
            // Culling must never drop a posed character: bind pose bounds, grown by half and by how far the clip moves joints.
            // Every part gets the whole sphere, since a pose can carry a part anywhere the character reaches.
            tbd::Sphere bounds = tbd::SphereAround(v.data(), v.size());
            bounds.radius = bounds.radius * 1.5f + g_engine.animator.ClipReach(e.animClip);
            SetMeshBounds(skinned, bounds);
            const auto& textures = PartTextures(e.meshPath);
            bool any = false;
            for (size_t k = 0; k < ranges.size() && k < textures.size(); ++k) any |= textures[k].first != tbd::kNoTexture || textures[k].second != tbd::kNoTexture;
            std::vector<MatPart> parts;
            for (size_t k = 0; any && k < ranges.size() && k < textures.size(); ++k) {
                MatPart p;
                p.first = ranges[k].firstIndex;
                p.count = ranges[k].indexCount;
                p.mesh = g_engine.renderer->CreateMeshPart(skinned, ranges[k].firstIndex, ranges[k].indexCount);
                p.bounds = bounds;
                p.albedo = textures[k].first;
                p.normal = textures[k].second;
                p.hair = model.parts[k].hair;
                p.eye = model.parts[k].eye;
                p.caster = !p.hair;
                p.lodMin = model.parts[k].lodMin;
                p.lodMax = model.parts[k].lodMax;
                if (model.parts[k].skin) p.surface = SkinSurfaceFor(e.meshPath);
                parts.push_back(p);
            }
            if (any) g_engine.matParts[skinned] = std::move(parts);
            return skinned;
        }
    }
    if (e.mesh == tbd::MeshKind::Imported && terrain_lods::Wanted(e, v)) {
        const auto& textures = PartTextures(e.meshPath);
        const tbd::MeshId id = g_engine.renderer->CreateMesh(v.data(), 3, tbd::Topology::Triangles); // only a handle, as for merged parts
        SetMeshBounds(id, tbd::SphereAround(v.data(), v.size()));
        std::vector<BuiltPart> built;
        terrain_lods::Build(v, textures.empty() ? tbd::kNoTexture : textures[0].first, textures.empty() ? tbd::kNoTexture : textures[0].second, tbd::kNoTexture, built);
        std::vector<MatPart> made;
        for (const BuiltPart& b : built) made.push_back(UploadBuiltPart(b));
        g_engine.sourceParts[id] = made;
        g_engine.matParts[id] = std::move(made);
        return id;
    }
    if (e.mesh == tbd::MeshKind::Imported) {
        const tbd::ImportedModel& model = ImportedModels()[e.meshPath];
        if (!model.parts.empty() && model.parts.back().first + model.parts.back().count == v.size()) {
            const auto& textures = PartTextures(e.meshPath);
            std::vector<MatPart> parts;
            bool any = false;
            for (size_t k = 0; k < model.parts.size() && k < textures.size(); ++k) {
                MatPart p;
                p.first = model.parts[k].first;
                p.count = model.parts[k].count;
                p.albedo = textures[k].first;
                p.normal = textures[k].second;
                p.hair = model.parts[k].hair;
                p.eye = model.parts[k].eye;
                p.caster = !p.hair;
                p.lodMin = model.parts[k].lodMin;
                p.lodMax = model.parts[k].lodMax;
                if (model.parts[k].skin) p.surface = SkinSurfaceFor(e.meshPath);
                any |= p.albedo != tbd::kNoTexture || p.normal != tbd::kNoTexture;
                parts.push_back(p);
            }
            if (any) { // the parts draw everything, shadows included: the whole mesh is only a handle, so it stays tiny
                const tbd::MeshId id = g_engine.renderer->CreateMesh(v.data(), 3, tbd::Topology::Triangles);
                SetMeshBounds(id, tbd::SphereAround(v.data(), v.size()));
                std::vector<BuiltPart> built;
                BuildMergedParts(parts, v, built, e.meshPath);
                std::vector<MatPart> made;
                for (const BuiltPart& b : built) made.push_back(UploadBuiltPart(b));
                g_engine.sourceParts[id] = parts;
                g_engine.matParts[id] = std::move(made);
                return id;
            }
        }
    }
    const tbd::MeshId id = g_engine.renderer->CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles);
    SetMeshBounds(id, tbd::SphereAround(v.data(), v.size()));
    return id;
}

// Replaces an entity's drawn mesh with deformed vertices (same layout as EntityVertices), keeping its materials.
void SetMaxGraphics(bool on);
extern bool g_maxGraphics;

void ReplaceEntityVertices(int index, const std::vector<tbd::Vertex>& v, const std::vector<std::pair<size_t, size_t>>& ranges) {
    CompleteSceneLoad();
    if (!g_engine.renderer || index < 0 || index >= (int)g_engine.meshes.size()) return;
    std::vector<MatPart> parts;
    const auto source = g_engine.sourceParts.find(g_engine.meshes[(size_t)index]);
    auto it = g_engine.matParts.find(g_engine.meshes[(size_t)index]);
    if (source != g_engine.sourceParts.end()) parts = source->second; // merged parts have no ranges in `v`
    else if (it != g_engine.matParts.end()) parts = it->second;
    DestroyEntityMesh(g_engine.meshes[(size_t)index]);
    for (size_t k = 0; k < parts.size() && k < ranges.size(); ++k) { // the file's parts, where the rig has moved them
        parts[k].first = ranges[k].first;
        parts[k].count = ranges[k].second;
    }
    if (!parts.empty() && parts.back().first + parts.back().count < v.size()) { // triangles added after the file's own (a mouth interior)
        MatPart tail;
        tail.first = parts.back().first + parts.back().count;
        tail.count = v.size() - tail.first;
        parts.push_back(tail);
    }
    const tbd::MeshId id = g_engine.renderer->CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles, true);
    if (!parts.empty()) UploadMatParts(id, parts, v, true);
    SetMeshBounds(id, tbd::SphereAround(v.data(), v.size()));
    g_engine.meshes[(size_t)index] = id;
}

// Each material part of entity `i` with its decoded color texture.

// Into the current shadow pass: entity `i`'s parts visible to `pass` (merged static parts hold the geometry), else its mesh.
void DrawEntityCaster(tbd::Renderer& r, size_t i, uint8_t pass) {
    const auto it = g_engine.matParts.find(g_engine.meshes[i]);
    if (it != g_engine.matParts.end() && !g_engine.sourceParts.count(g_engine.meshes[i]) &&
        std::any_of(it->second.begin(), it->second.end(), [](const MatPart& p) { return p.hair; })) {
        const std::vector<MatPart>& parts = it->second; // a skinned body with hair cards: its parts, not the whole mesh
        for (size_t k = 0; k < parts.size(); ++k) {
            if (parts[k].caster) r.DrawShadowCaster(parts[k].mesh, g_engine.models[i]);
            else if (parts[k].hair && pass == kVisibleTight && (g_engine.partVisible[g_engine.partOffsets[i] + k] & kVisibleTight))
                r.DrawShadowCasterCutout(parts[k].mesh, g_engine.models[i], parts[k].albedo);
        }
        return;
    }
    if (it == g_engine.matParts.end() || !g_engine.sourceParts.count(g_engine.meshes[i])) {
        r.DrawShadowCaster(g_engine.meshes[i], g_engine.models[i]);
        return;
    }
    const std::vector<MatPart>& parts = it->second;
    for (size_t k = 0; k < parts.size(); ++k)
        if (g_engine.partVisible[g_engine.partOffsets[i] + k] & pass) {
            if (parts[k].hair) r.DrawShadowCasterCutout(parts[k].mesh, g_engine.models[i], parts[k].albedo);
            else r.DrawShadowCaster(parts[k].mesh, g_engine.models[i]);
        }
}

void DrawInstancedCasters(tbd::Renderer& r) {
    for (const InstancedAsset& asset : g_engine.instanced) {
        if (asset.casters.empty()) continue;
        const auto it = g_engine.matParts.find(asset.mesh);
        for (const InstancedAsset::Range& c : asset.casters) {
            if (it == g_engine.matParts.end()) r.DrawShadowInstanced(asset.mesh, asset.buffer, c.first, c.count);
            else for (const MatPart& p : it->second)
                if (p.lodError == 0.0f) r.DrawShadowInstanced(p.mesh, asset.buffer, c.first, c.count); // full level only: no per-instance choice yet
        }
    }
}

void DestroyInstancedAssets() {
    if (g_engine.renderer)
        for (const InstancedAsset& asset : g_engine.instanced) {
            g_engine.renderer->DestroyInstanceBuffer(asset.buffer);
            DestroyEntityMesh(asset.mesh);
        }
    g_engine.instanced.clear();
}

#include "vegetation.inl"

// Each InstanceSet's mesh and matrices onto the GPU. A missing or unreadable file or mesh only loses that asset.
void CreateInstancedAsset(const tbd::InstanceSet& set) {
    {
        if (g_vegetation.Consumes(set.name)) return; // placed by the GPU now
        std::string bytes;
        if (!tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, tbd::Utf8ToWide(set.file)), bytes) || bytes.size() % (tbd::kInstanceFloats * sizeof(float)) != 0 || bytes.empty()) {
            tbd::Log("Instances: cannot use %s", set.file.c_str());
            return;
        }
        if (ImportedVertices(set.meshPath).empty()) return; // the toast has said why
        tbd::Entity e;
        e.mesh = tbd::MeshKind::Imported;
        e.meshPath = set.meshPath;
        e.color = {1, 1, 1};
        InstancedAsset asset;
        g_shrinkAtDistance = set.maxDistance <= 80.0f ? set.maxDistance : 0.0f;
        asset.mesh = UploadEntityMesh(e);
        g_shrinkAtDistance = 0.0f;
        if (asset.mesh == tbd::kInvalidMesh || asset.mesh >= g_engine.meshBounds.size()) { tbd::Log("Instances: no mesh for %s", set.file.c_str()); return; } // out of GPU memory, or the device lost
        asset.maxDistance = set.maxDistance;
        asset.shadowDistance = set.shadowDistance;
        std::vector<float> rows(bytes.size() / sizeof(float));
        std::memcpy(rows.data(), bytes.data(), bytes.size());
        asset.cells = tbd::BuildInstanceCells(rows, g_engine.meshBounds[asset.mesh], tbd::InstanceCellSize(set.maxDistance));
        asset.buffer = g_engine.renderer->CreateInstanceBuffer(rows.data(), (uint32_t)(rows.size() / tbd::kInstanceFloats));
        g_engine.instanced.push_back(std::move(asset));
    }
}

#include "world_streaming.inl"

// See water.h. Anything in the game may call AddWaterImpulse: positive strength pushes the surface down (metres per second).
tbd::WaterSystem g_water;
bool LoadGroundMesh(const std::string& path, std::vector<tbd::Vertex>& out);
void AddWaterImpulse(tbd::Vec3 pos, float radius, float strength) { g_water.AddImpulse(pos, radius, strength); }
bool WaterHeightAt(float x, float z, float& y) { return g_water.Active() && g_water.HeightAt(x, z, y); }

// A scene load, spread over frames so the window stays alive and the viewport shows the world arriving: files and images on background
// workers, then per entity the grouping and welding on a worker and the uploads on the main thread, without waiting for the GPU and
// within a time budget per frame. Entities appear in scene order; RenderFrame draws the ones that exist. See SceneLoadTick.
struct SceneLoadWork {
    std::vector<std::string> paths;
    std::vector<ImportResult> results;
    std::atomic<size_t> left{0};
    struct Image { std::string path; int image = 0; DecodedImage decoded; };
    std::vector<std::unique_ptr<Image>> images;
    std::atomic<bool> imagesListed{false};
};
struct EntityBuild {
    std::vector<BuiltPart> parts;
    std::vector<MatPart> source;
    tbd::Sphere bounds;
    tbd::Vertex first[3];
    bool terrain = false;
    std::atomic<bool> done{false};
};
struct SceneLoad {
    enum Phase { Idle, Imports, Decode, Entities, Instances } phase = Idle;
    std::shared_ptr<SceneLoadWork> work;
    size_t entity = 0, instance = 0, nextPart = 0;
    std::shared_ptr<EntityBuild> build;
    tbd::MeshId handle = tbd::kInvalidMesh;
    std::vector<MatPart> made;
    uint64_t started = 0;
};
SceneLoad g_load;
bool SceneLoadActive() { return g_load.phase != SceneLoad::Idle; }

std::string SceneLoadStatus() {
    char s[80] = {};
    switch (g_load.phase) {
    case SceneLoad::Imports: snprintf(s, sizeof(s), "LOADING SCENE: FILES %zu LEFT", g_load.work->left.load()); break;
    case SceneLoad::Decode: snprintf(s, sizeof(s), "LOADING SCENE: IMAGES %zu LEFT", g_load.work->left.load()); break;
    case SceneLoad::Entities: snprintf(s, sizeof(s), "LOADING SCENE: %zu OF %zu", g_load.entity, g_engine.scene.entities.size()); break;
    case SceneLoad::Instances: snprintf(s, sizeof(s), "LOADING SCENE: INSTANCES %zu OF %zu", g_load.instance, g_engine.scene.instances.size()); break;
    default: break;
    }
    return s;
}

void CancelSceneLoad() { // workers still running finish into memory only they hold
    for (const MatPart& p : g_load.made) if (g_engine.renderer) g_engine.renderer->DestroyMesh(p.mesh);
    if (g_load.handle != tbd::kInvalidMesh && g_engine.renderer) g_engine.renderer->DestroyMesh(g_load.handle);
    g_load = SceneLoad();
}

// The big static cases of UploadEntityMesh: decided here on the main thread, built on a worker. Null when this entity takes the plain path.
std::shared_ptr<EntityBuild> StartEntityBuild(const tbd::Entity& e) {
    if (e.mesh != tbd::MeshKind::Imported || !e.animClip.empty() || !e.faceClip.empty()) return nullptr;
    const std::vector<tbd::Vertex>& cached = ImportedVertices(e.meshPath);
    if (cached.size() < 3) return nullptr;
    auto build = std::make_shared<EntityBuild>();
    const bool terrain = terrain_lods::Wanted(e, cached);
    tbd::TextureId albedo = tbd::kNoTexture, normal = tbd::kNoTexture;
    if (terrain) {
        const auto& textures = PartTextures(e.meshPath);
        if (!textures.empty()) { albedo = textures[0].first; normal = textures[0].second; }
        build->terrain = true;
    } else {
        const tbd::ImportedModel& model = ImportedModels()[e.meshPath];
        if (model.parts.empty() || model.parts.back().first + model.parts.back().count != cached.size()) return nullptr;
        const auto& textures = PartTextures(e.meshPath);
        bool any = false;
        for (size_t k = 0; k < model.parts.size() && k < textures.size(); ++k) {
            MatPart p;
            p.first = model.parts[k].first;
            p.count = model.parts[k].count;
            p.albedo = textures[k].first;
            p.normal = textures[k].second;
            p.hair = model.parts[k].hair;
                p.eye = model.parts[k].eye;
            p.caster = !p.hair;
            p.lodMin = model.parts[k].lodMin;
            p.lodMax = model.parts[k].lodMax;
            if (model.parts[k].skin) p.surface = SkinSurfaceFor(e.meshPath);
            any |= p.albedo != tbd::kNoTexture || p.normal != tbd::kNoTexture;
            build->source.push_back(p);
        }
        if (!any) return nullptr;
    }
    const tbd::Vec3 color = e.color;
    const std::vector<tbd::Vertex>* source = &cached; // the import cache's entries never move
    const std::string path = e.meshPath;
    g_engine.jobs->RunBackground([build, source, color, albedo, normal, path] {
        try {
            std::vector<tbd::Vertex> v = *source;
            for (tbd::Vertex& x : v) if (!tbd::IsHairVertex(x.a)) { x.r *= color.x; x.g *= color.y; x.b *= color.z; }
            build->bounds = tbd::SphereAround(v.data(), v.size());
            std::copy(v.begin(), v.begin() + 3, build->first);
            if (build->terrain) terrain_lods::Build(v, albedo, normal, tbd::kNoTexture, build->parts);
            else BuildMergedParts(build->source, v, build->parts, path);
        } catch (...) { build->parts.clear(); tbd::Log("Scene load: a mesh failed to build"); }
        build->done = true;
    });
    return build;
}

struct DecodeModel { std::string path; const tbd::ImportedModel* model; };
void StartDecodes(const std::shared_ptr<SceneLoadWork>& work) { // gathers on the main thread; hashes and decodes on the workers
    std::vector<std::string> wanted, seen;
    for (const tbd::Entity& e : g_engine.scene.entities) if (e.mesh == tbd::MeshKind::Imported) wanted.push_back(e.meshPath);
    for (const tbd::InstanceSet& set : g_engine.scene.instances) wanted.push_back(set.meshPath);
    auto models = std::make_shared<std::vector<DecodeModel>>();
    for (const std::string& path : wanted) {
        if (g_engine.partTextures.count(path) || std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
        seen.push_back(path);
        const auto it = ImportedModels().find(path);
        if (it != ImportedModels().end()) models->push_back({path, &it->second}); // map nodes keep their address
    }
    auto known = std::make_shared<std::set<uint64_t>>();
    for (const auto& [content, id] : g_engine.texturesByContent) known->insert(content);
    tbd::JobSystem* jobs = g_engine.jobs.get();
    jobs->RunBackground([work, models, known, jobs] {
        struct Job { SceneLoadWork::Image* out; const std::vector<uint8_t>* bytes; };
        std::vector<Job> list;
        std::set<uint64_t> queued = *known; // an image embedded in many files is decoded once; see PartTextures
        for (const DecodeModel& m : *models) {
            std::set<int> slots;
            for (const tbd::ImportedPart& p : m.model->parts)
                for (int image : {p.albedo, p.normal})
                    if (image >= 0 && image < (int)m.model->images.size() && slots.insert(image).second && queued.insert(ContentHash(m.model->images[(size_t)image])).second) {
                        work->images.push_back(std::make_unique<SceneLoadWork::Image>());
                        work->images.back()->path = m.path;
                        work->images.back()->image = image;
                        list.push_back({work->images.back().get(), &m.model->images[(size_t)image]});
                    }
        }
        work->left = list.size();
        work->imagesListed = true;
        for (const Job& job : list)
            jobs->RunBackground([work, models, job] {
                DecodedImage& d = job.out->decoded;
                try {
                    d.ok = tbd::DecodeImage(job.bytes->data(), job.bytes->size(), d.rgba, d.w, d.h);
                    if (d.ok) { d.mips = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(d.rgba.data(), d.w, d.h)); std::vector<uint8_t>().swap(d.rgba); }
                } catch (...) { d.ok = false; d.mips.reset(); }
                --work->left;
            });
    });
}

// Once per frame while a load runs, on the main thread, before the frame begins. budgetMs <= 0: no limit (CompleteSceneLoad).
void SceneLoadTick(double budgetMs) {
    if (!SceneLoadActive() || !g_engine.renderer) return;
    int64_t begin, now, freq;
    begin = tbd::platform::Ticks();
    freq = tbd::platform::TickFrequency();
    const auto spent = [&] { now = tbd::platform::Ticks(); return budgetMs > 0.0 && (now - begin) * 1000.0 / freq > budgetMs; };
    if (g_load.phase == SceneLoad::Imports) {
        if (g_load.work->left.load() > 0) return;
        for (size_t i = 0; i < g_load.work->paths.size(); ++i) PublishImport(g_load.work->paths[i], std::move(g_load.work->results[i]));
        g_load.work->results.clear();
        StartDecodes(g_load.work);
        g_load.phase = SceneLoad::Decode;
    }
    if (g_load.phase == SceneLoad::Decode) {
        if (!g_load.work->imagesListed || g_load.work->left.load() > 0) return;
        for (auto& image : g_load.work->images) DecodedImages()[image->path][image->image] = std::move(image->decoded);
        g_load.work.reset();
        g_load.phase = SceneLoad::Entities;
    }
    while (g_load.phase == SceneLoad::Entities) {
        if (g_load.build) {
            if (!g_load.build->done) return;
            EntityBuild& b = *g_load.build;
            if (g_load.handle == tbd::kInvalidMesh) { // the parts draw everything, shadows included: the whole mesh is only a handle
                g_load.handle = g_engine.renderer->CreateMesh(b.first, 3, tbd::Topology::Triangles);
                SetMeshBounds(g_load.handle, b.bounds);
            }
            while (g_load.nextPart < b.parts.size()) {
                g_load.made.push_back(UploadBuiltPart(b.parts[g_load.nextPart++]));
                if (spent()) return;
            }
            g_engine.sourceParts[g_load.handle] = b.terrain ? g_load.made : b.source;
            g_engine.matParts[g_load.handle] = std::move(g_load.made);
            g_load.made.clear();
            g_engine.meshes.push_back(g_load.handle);
            g_load.handle = tbd::kInvalidMesh;
            g_load.build.reset();
            g_load.nextPart = 0;
            ++g_load.entity;
        } else if (g_load.entity < g_engine.scene.entities.size()) {
            const tbd::Entity& e = g_engine.scene.entities[g_load.entity];
            g_load.build = StartEntityBuild(e);
            if (!g_load.build) { g_engine.meshes.push_back(UploadEntityMesh(e)); ++g_load.entity; }
        } else {
            g_vegetation.Load();
            g_load.phase = SceneLoad::Instances;
            break;
        }
        if (spent()) return;
    }
    while (g_load.phase == SceneLoad::Instances) {
        if (g_load.instance >= g_engine.scene.instances.size()) {
            DecodedImages().clear(); // anything not consumed by a texture upload
            tbd::Log("Scene load: %llu ms", tbd::platform::TickCountMs() - g_load.started);
            g_load = SceneLoad();
            return;
        }
        CreateInstancedAsset(g_engine.scene.instances[g_load.instance++]);
        if (spent()) return;
    }
}

// Whatever must see the whole scene (an edit, Play) finishes the load first.
void CompleteSceneLoad() {
    while (SceneLoadActive() && g_engine.renderer) {
        SceneLoadTick(0.0);
        if (SceneLoadActive()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Recreates every entity's mesh (after loading a scene, or after the renderer was rebuilt). Returns before they exist: see SceneLoadTick.
void RebuildSceneMeshes() {
    CancelSceneLoad();
    g_water.Prepare(g_engine.renderer.get(), g_engine.jobs.get(), g_engine.scene, LoadGroundMesh);
    if (g_engine.renderer) {
        g_stream.Stop(*g_engine.renderer);
        for (tbd::MeshId id : g_engine.meshes) DestroyEntityMesh(id);
    }
    g_engine.meshes.clear();
    DestroyInstancedAssets();
    if (g_engine.renderer && g_engine.jobs && world_streaming::Streamer::Wanted(g_engine.scene)) { // see world_streaming.inl
        for (const tbd::Entity& e : g_engine.scene.entities)
            g_engine.meshes.push_back(world_streaming::Streamable(e) ? tbd::kInvalidMesh : UploadEntityMesh(e));
        g_stream.Start(*g_engine.renderer);
        return;
    }
    if (!g_engine.renderer || !g_engine.jobs) {
        g_engine.meshes.assign(g_engine.scene.entities.size(), tbd::kInvalidMesh);
        return;
    }
    g_load.started = tbd::platform::TickCountMs();
    g_load.work = std::make_shared<SceneLoadWork>();
    std::vector<std::string> wanted; // every imported file of the scene, entities first
    for (const tbd::Entity& e : g_engine.scene.entities)
        if (e.mesh == tbd::MeshKind::Imported) wanted.push_back(e.meshPath);
    for (const tbd::InstanceSet& set : g_engine.scene.instances) wanted.push_back(set.meshPath);
    for (const std::string& path : wanted)
        if (ImportedVertexCache().find(path) == ImportedVertexCache().end() && std::find(g_load.work->paths.begin(), g_load.work->paths.end(), path) == g_load.work->paths.end())
            g_load.work->paths.push_back(path);
    g_load.work->results.resize(g_load.work->paths.size());
    g_load.work->left = g_load.work->paths.size();
    g_load.phase = SceneLoad::Imports;
    for (size_t i = 0; i < g_load.work->paths.size(); ++i)
        g_engine.jobs->RunBackground([work = g_load.work, i] {
            try { ImportFile(work->paths[i], work->results[i]); } catch (...) { work->results[i].ok = false; work->results[i].error = "unexpected error"; }
            --work->left;
        });
    if (g_engine.scene.entities.empty() && g_engine.scene.instances.empty()) CompleteSceneLoad();
}

static float g_dynamicScale = 0.0f; // set by UpdateDynamicResolution; 0 = TBD_SCALE alone

// TBD_SCALE: the internal render resolution as a fraction of the window (0.5..1.0, default 1.0). The renderer and the
// TAA jitter both size from this, through tbd::ScaledSize.
float RenderScale() {
    static const float scale = [] {
        char v[32] = {};
#if defined(__APPLE__) // iOS draws at the screen's native pixels: without a settings file the scene starts at the High step
        const float unset = (float)tbd::gfx::kScalePct[tbd::gfx::kDefaultScale] / 100.0f;
#else
        const float unset = 1.0f;
#endif
        if (tbd::platform::GetEnvA("TBD_SCALE", v, sizeof(v)) == 0) return unset;
        char* end = nullptr;
        const float s = strtof(v, &end);
        return end == v ? 1.0f : tbd::ClampRenderScale(s);
    }();
    if (g_dynamicScale > 0.0f) return g_dynamicScale;
    return tbd::gfx::userSet ? tbd::ClampRenderScale(tbd::gfx::ScaleFraction()) : scale;
}

// TBD_DYNRES=<fps>: holds that frame rate by stepping the render scale, 0.7..1.0, after a second of evidence. Off by default:
// a step remakes the post targets, so TAA history and the probes restart, which shows for about half a second.
void UpdateDynamicResolution(tbd::Renderer& r, float dt) {
    { // the Graphics page's resolution: remade here, outside the frame, and only when it changed
        static float applied = -1.0f;
        if (tbd::gfx::userSet && g_dynamicScale <= 0.0f && RenderScale() != applied) {
            applied = RenderScale();
            r.SetRenderScale(applied);
        }
    }
    static const float target = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_DYNRES", v, sizeof(v)) > 0 ? (float)atof(v) : 0.0f; }();
    if (target <= 0.0f || dt <= 0.0f) return;
    static float sum = 0.0f;
    static int frames = 0;
    sum += dt;
    if (++frames < 90 || sum < 0.5f) return;
    const float ms = sum / (float)frames * 1000.0f, budget = 1000.0f / target, now = RenderScale();
    sum = 0.0f; frames = 0;
    float next = now;
    if (ms > budget * 1.04f) next = now - 0.05f;
    else if (ms < budget * 0.8f) next = now + 0.05f;
    next = std::clamp(next, 0.7f, 1.0f);
    if (next != now) {
        g_dynamicScale = next;
        r.SetRenderScale(next);
        tbd::Log("Dynamic resolution: %.1f ms against %.1f, scale %.2f", ms, budget, next);
    }
}

// (Re)creates the viewport window and a renderer for `api`, inside whichever window currently hosts the
// viewport. A window can only ever host one API, which is why switching APIs (or popping the viewport out)
// builds a fresh window.
// ------------------------------------------------------------------- in-game GUI: smooth or classic
// After every renderer start (F1 / F2 make a new one): the SDF font atlas (assets\ui\ui_font_sdf.png beside bin; made by
// tools\bake_ui_font.py) goes into the new renderer for its DrawUi path. Without the UI pipeline (shader missing or failed),
// without the atlas, or with TBD_GUI_CLASSIC=1, the GUI draws the classic way (flat quads and the pixel font, DrawTransient).
namespace gdock { void LoadAtlas(tbd::Renderer& r); } // game/glass_dock_host.inl

void UploadUiFont() {
    static const bool forceClassic = [] {
        char v[8] = {};
        return tbd::platform::GetEnvA("TBD_GUI_CLASSIC", v, sizeof(v)) > 0 && v[0] != '0';
    }();
    tbd::Renderer& r = *g_engine.renderer;
    bool smooth = !forceClassic && r.UiSupported();
    if (smooth) {
        std::string bytes;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        const std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\ui\\ui_font_sdf.png";
        smooth = tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h) &&
                 w == tbd::gui::font_data::kAtlasWidth && h == tbd::gui::font_data::kAtlasHeight;
        const tbd::TextureId atlas = smooth ? r.CreateTexture(rgba.data(), w, h) : tbd::kNoTexture;
        smooth = atlas != tbd::kNoTexture;
        if (smooth) r.SetUiAtlas(atlas);
        if (smooth) gdock::LoadAtlas(r); // the Glass Dock's icons and previews: between frames (a texture upload)
    }
    g_gui.SetClassic(!smooth);
    g_roadGpu.Forget();
    tbd::Log("Game GUI: %s", smooth ? "smooth (UI pipeline, SDF font)"
                                    : forceClassic ? "classic (TBD_GUI_CLASSIC=1)" : "classic (UI pipeline or font atlas unavailable)");
}

// -------------------------------------------------------------- scene commands

std::wstring ScenesFolder() {
    const std::wstring dir = tbd::Join(g_engine.assetsRoot, L"Scenes");
    { std::error_code ec; std::filesystem::create_directory(tbd::platform::FsPath(dir), ec); }
    return dir;
}

void NewScene() {
    g_engine.scene = tbd::Scene();
    g_skinDetailScale.clear(); g_skinPoreDepth.clear(); // entity indices of the old scene
    g_engine.terrain.params = tbd::TerrainParams();
    g_engine.scenePath.clear();
    RebuildSceneMeshes();
    UpdateTitle();
    Toast("NEW SCENE");
}

bool LoadScene(const std::wstring& path) {
    g_skinDetailScale.clear(); g_skinPoreDepth.clear(); // entity indices of the old scene
    if (!tbd::LoadSceneFile(path, g_engine.scene, g_engine.terrain.params)) {
        tbd::shell::Alert(L"Could not read this scene:\n" + path);
        return false;
    }
    g_engine.scenePath = path;
    if (g_engine.scene.hour >= 0.0f) { g_engine.sun.hour = g_engine.scene.hour; g_engine.sun.animate = false; } // a lit shot keeps its sun
    RebuildSceneMeshes();
    // Worlds rarely sit at the origin: start on the character when the scene has one.
    for (const tbd::Entity& e : g_engine.scene.entities)
        if (e.name == "Wyatt Head") {
            g_engine.camera.target = {e.position.x, e.position.y + 0.8f, e.position.z};
            g_engine.camera.distance = 8.0f;
            break;
        }
    UpdateTitle();
    Toast("OPENED " + ToastName(path));
    return true;
}

bool SaveScene(bool saveAs) {
    std::wstring path = g_engine.scenePath;
    if (saveAs || path.empty()) {
        if (path.empty()) path = L"Untitled.tbdscene";
        if (!tbd::shell::PickSceneFile(true, path)) return false;
    }
    if (!tbd::SaveSceneFile(path, g_engine.scene, g_engine.terrain.params)) {
        tbd::shell::Alert(L"The scene could not be saved there.");
        return false;
    }
    g_engine.scenePath = path;
    UpdateTitle();
    tbd::shell::OnSceneSaved();
    Toast("SAVED " + ToastName(path));
    return true;
}

// Drops an imported model at the camera's focus point, resting on it.
void PlaceImported(const std::wstring& path) {
    tbd::Entity e;
    e.mesh = tbd::MeshKind::Imported;
    e.meshPath = tbd::WideToUtf8(path);
    if (ImportedVertices(e.meshPath).empty()) return; // already toasted the reason
    const size_t cut = path.find_last_of(L"\\/");
    std::wstring base = cut == std::wstring::npos ? path : path.substr(cut + 1);
    if (const size_t dot = base.rfind(L'.'); dot != std::wstring::npos) base.resize(dot);
    e.name = tbd::WideToUtf8(base) + " " + std::to_string(g_engine.nextEntityId++);
    e.color = {1.0f, 1.0f, 1.0f}; // the file's own colors, untinted
    e.scale = {2, 2, 2};
    e.position = g_engine.camera.target;
    e.position.y += tbd::LocalHalfExtents(e).y * e.scale.y;
    CompleteSceneLoad();
    g_engine.scene.Add(e);
    g_engine.meshes.push_back(UploadEntityMesh(e));
    Toast("IMPORTED " + ToastName(path));
}

void OnOpenAsset(const std::wstring& path, void*) {
    const size_t dot = path.rfind(L'.');
    const wchar_t* ext = dot != std::wstring::npos ? path.c_str() + dot : L"";
    if (tbd::platform::WStrICmp(ext, tbd::kSceneExtension) == 0) LoadScene(path);
    else if (tbd::platform::WStrICmp(ext, L".obj") == 0 || tbd::platform::WStrICmp(ext, L".gltf") == 0 || tbd::platform::WStrICmp(ext, L".glb") == 0) PlaceImported(path);
    else Toast("NO EDITOR FOR " + ToastName(path));
}

void DuplicateSelected() {
    CompleteSceneLoad();
    if (const tbd::Entity* copy = g_engine.scene.DuplicateSelected()) {
        g_engine.meshes.push_back(UploadEntityMesh(*copy));
        Toast("DUPLICATED " + copy->name);
    }
}

void DeleteSelected() {
    CompleteSceneLoad();
    const int index = g_engine.scene.DeleteSelected();
    if (index < 0) return;
    if (g_engine.renderer && index < (int)g_engine.meshes.size()) DestroyEntityMesh(g_engine.meshes[index]);
    if (index < (int)g_engine.meshes.size()) g_engine.meshes.erase(g_engine.meshes.begin() + index);
    g_stream.OnEntityErased(g_engine.renderer.get(), index);
}

void FrameSelected() {
    if (const tbd::Entity* e = g_engine.scene.Selected()) {
        const float size = std::max({e->scale.x, e->scale.y, e->scale.z});
        g_engine.camera.target = e->position;
        g_engine.camera.distance = std::clamp(size * 3.0f + 2.0f, 3.0f, 200.0f);
    }
}

// Where a click lands on the world: the terrain when there is one, otherwise the y = 0 plane.
bool GroundHit(const tbd::Ray& ray, tbd::Vec3& hit) {
    if (g_engine.terrain.params.enabled && g_engine.terrain.Raycast(ray, 30000.0f, hit)) return true;
    if (ray.dir.y >= -1e-4f) return false;
    const float t = -ray.origin.y / ray.dir.y;
    if (t < 0) return false;
    hit = ray.origin + ray.dir * t;
    return true;
}

bool CreateKind(tbd::ToolId tool, tbd::MeshKind& kind) {
    switch (tool) {
    case tbd::ToolId::Cube: kind = tbd::MeshKind::Cube; return true;
    case tbd::ToolId::Sphere: kind = tbd::MeshKind::Sphere; return true;
    case tbd::ToolId::Plane: kind = tbd::MeshKind::Plane; return true;
    case tbd::ToolId::Tree: kind = tbd::MeshKind::Tree; return true;
    case tbd::ToolId::Rock: kind = tbd::MeshKind::Rock; return true;
    default: return false;
    }
}

void PlaceEntity(tbd::MeshKind kind, const tbd::Ray& ray) {
    tbd::Vec3 hit;
    if (!GroundHit(ray, hit)) return;
    tbd::Entity e;
    e.mesh = kind;
    e.name = std::string(tbd::MeshKindName(kind)) + " " + std::to_string(g_engine.nextEntityId++);
    e.position = hit;
    if (kind == tbd::MeshKind::Plane) {
        e.scale = {4, 1, 4};
        e.position.y += 0.03f;
        e.color = {0.62f, 0.56f, 0.46f};
    } else if (kind == tbd::MeshKind::Sphere) {
        e.position.y += 0.5f;
        e.color = {0.36f, 0.58f, 0.82f};
    } else if (kind == tbd::MeshKind::Tree) {
        e.scale = {6, 6, 6};
        e.position.y += 3.0f;
        e.color = {0.28f, 0.52f, 0.22f};
    } else if (kind == tbd::MeshKind::Rock) {
        e.scale = {2, 2, 2};
        e.position.y += 0.7f;
        e.color = {0.52f, 0.5f, 0.47f};
    } else {
        e.position.y += 0.5f;
        e.color = {0.85f, 0.56f, 0.28f};
    }
    CompleteSceneLoad();
    g_engine.scene.Add(e);
    g_engine.meshes.push_back(UploadEntityMesh(e));
}

// -------------------------------------------------------------------- frame

std::string Grouped(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

float FrameDelta() {
    int64_t now;
    now = tbd::platform::Ticks();
    float dt = 0.0f;
    if (g_engine.lastTick != 0)
        dt = (float)((double)(now - g_engine.lastTick) / (double)g_engine.tickFreq);
    g_engine.lastTick = now;
    return std::clamp(dt, 0.0f, 0.1f);
}

// WASD + Q/E while the right mouse button is held (Shift = faster; the wheel changes the speed).
void UpdateFly(float dt) {
    if (g_engine.drag != Drag::Fly) return;
    using PK = tbd::platform::Key; auto down = [](PK k) { return tbd::platform::KeyDownAsync(k); };
    const float speed = g_engine.flySpeed * (down(PK::Shift) ? 4.0f : 1.0f) * dt * tbd::shell::EditorCameraScale() * tbd::shell::EditorCameraScale(); // creator open: ~1/10 fly speed
    const float forward = (down(PK::W) ? 1.0f : 0.0f) - (down(PK::S) ? 1.0f : 0.0f);
    const float right = (down(PK::D) ? 1.0f : 0.0f) - (down(PK::A) ? 1.0f : 0.0f);
    const float up = ((down(PK::E) || down(PK::Space)) ? 1.0f : 0.0f) - ((down(PK::Q) || down(PK::C)) ? 1.0f : 0.0f);
    g_engine.camera.Fly(forward * speed, right * speed, up * speed);
}


// City camera from the keyboard (not flying, not walking): WASD / arrows slide the target over the ground,
// Q / E turn around it, Z / X tilt; Shift = 3x. The wheel zoom eases toward its goal and toward the cursor.
bool RoadRenameActive(); // defined with the road rename box
struct CamSmooth { float goal = -1.0f, lastSet = -1.0f; bool anchored = false; tbd::Vec3 anchor{}; };
CamSmooth g_camSmooth;
bool AppHasFocus() { return tbd::shell::AppHasFocus(); }
void UpdateCityCam(float dt) {
    tbd::OrbitCamera& cam = g_engine.camera;
    // smooth zoom, anchored on the ground point that was under the cursor
    if (g_camSmooth.goal > 0.0f) {
        if (g_camSmooth.lastSet >= 0.0f && std::fabs(cam.distance - g_camSmooth.lastSet) > 1e-5f) g_camSmooth.goal = -1.0f; // someone else moved it (Frame, Home)
        else {
            const float d = cam.distance, nd = d + (g_camSmooth.goal - d) * (1.0f - std::exp(-14.0f * dt));
            if (g_camSmooth.anchored && d > 1e-6f) cam.target = g_camSmooth.anchor + (cam.target - g_camSmooth.anchor) * (nd / d);
            cam.distance = nd;
            g_camSmooth.lastSet = nd;
            if (std::fabs(nd - g_camSmooth.goal) < g_camSmooth.goal * 0.002f) g_camSmooth.goal = -1.0f;
        }
    }
    if (g_engine.drag == Drag::Fly || g_play.active || RoadRenameActive() || !AppHasFocus()) return;
    using PK = tbd::platform::Key; auto down = [](PK k) { return tbd::platform::KeyDownAsync(k); };
    if (down(PK::Control) || down(PK::Alt)) return;
    const float fw = ((down(PK::W) || down(PK::Up)) ? 1.0f : 0.0f) - ((down(PK::S) || down(PK::Down)) ? 1.0f : 0.0f);
    const float rt = ((down(PK::D) || down(PK::Right)) ? 1.0f : 0.0f) - ((down(PK::A) || down(PK::Left)) ? 1.0f : 0.0f);
    const float turn = (down(PK::E) ? 1.0f : 0.0f) - (down(PK::Q) ? 1.0f : 0.0f);
    const float tilt = (down(PK::X) ? 1.0f : 0.0f) - (down(PK::Z) ? 1.0f : 0.0f);
    if (fw == 0.0f && rt == 0.0f && turn == 0.0f && tilt == 0.0f) return;
    const float fast = down(PK::Shift) ? 3.0f : 1.0f, k = tbd::shell::EditorCameraScale();
    tbd::Vec3 f = cam.Forward(); f.y = 0.0f;
    const float fl = std::sqrt(f.x * f.x + f.z * f.z);
    if (fl > 1e-4f) f = f * (1.0f / fl);
    tbd::Vec3 r = cam.Right(); r.y = 0.0f;
    const float pan = std::max(cam.distance, 0.5f) * 1.1f * fast * k * dt; // screen-relative: the same feel at every zoom
    cam.target = cam.target + (f * fw + r * rt) * pan;
    cam.yaw += turn * 1.6f * fast * k * dt;
    if (g_terrainSandbox) tbd::citycam::Tilt(cam, tilt * 0.9f * fast * k * dt); // the city: straight down to the horizon
    else cam.pitch = std::clamp(cam.pitch + tilt * 0.9f * fast * k * dt, 0.05f, 1.5f);
}

// Loads a terrain file for the walking ground grid (worker thread; ImportFile touches no shared state).
bool LoadGroundMesh(const std::string& path, std::vector<tbd::Vertex>& out) {
    ImportResult r;
    ImportFile(path, r);
    if (!r.ok) return false;
    out = std::move(r.vertices);
    return true;
}

// The ground under (x, z) for walking: the imported terrain's grid, else the procedural terrain, else the y = 0 plane
// (only when the scene has no imported terrain at all, so a still-loading grid never drops you to the plane).
bool PlayGround(float x, float z, float& y) {
    if (g_play.GroundHeight(x, z, y)) return true;
    if (g_play.GroundState() != 0) return false;
    y = g_engine.terrain.params.enabled ? g_engine.terrain.HeightAt(x, z) : 0.0f;
    return true;
}

// "Graphics: MAX": sky, clouds and sun shadows on, the sun low for portrait light, and no editor overlays over the picture. HDR,
// ambient occlusion, TAA and skin shading are always on; they only need the project's skin maps (testproject\\Western).
bool g_maxGraphics = false;
// Graphics > Quality: 0 full; 1 fast (modeling: textures and sun shadows only, no ray tracing, probes, clouds, sky, relief, grass);
// 2 unlit (vertex colours only, no shadows). F6 cycles.
int g_gfxQuality = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_QUALITY", v, sizeof(v)) > 0 ? std::clamp(atoi(v), 0, 2) : 0; }(); // TBD_QUALITY=0/1/2 starts there
// The graphics preset for lighting and post (docs/track_r_ios_platform_notes.md): TBD_CHUNK_PRESET, the one knob Track R's governor
// reads too. 0 low, 1 lowmed, 2 medium, 3 high, 4 ultra and pc (the default). TBD_LOOK=0: the look before the CS2 pass (no city air,
// no grade, every post pass at every preset), for before/after comparisons.
bool LookOn() { static const bool on = [] { char v[8] = {}; return !(tbd::platform::GetEnvA("TBD_LOOK", v, sizeof(v)) > 0 && v[0] == '0'); }(); return on; }
int LookTier() {
    if (tbd::gfx::userSet) return LookOn() ? tbd::gfx::g.look : 4; // the player's Graphics page (src/gfx_settings.h) rules once used
    static const int tier = [] {
        char v[16] = {};
        const std::string p = tbd::platform::GetEnvA("TBD_CHUNK_PRESET", v, sizeof(v)) > 0 ? v : "pc";
        return !LookOn() ? 4 : p == "low" ? 0 : p == "lowmed" ? 1 : p == "medium" ? 2 : p == "high" ? 3 : 4;
    }();
    return tier;
}
void SetMaxGraphics(bool on) {
    static float hour = 0, tilt = 0;
    static bool sky = true, shadows = true, animate = false;
    if (on == g_maxGraphics) return;
    g_maxGraphics = on;
    tbd::Sun& sun = g_engine.sun;
    if (on) {
        hour = sun.hour; tilt = sun.tiltDeg; sky = sun.showSky; shadows = sun.shadows; animate = sun.animate;
        // the sun's everyday path stays behind a head that faces +Z, so lean it round to the front: a key light
        sun.hour = 13.6f;
        sun.tiltDeg = 50.0f;
        sun.showSky = sun.shadows = true;
        sun.animate = false;
    } else {
        sun.hour = hour; sun.tiltDeg = tilt; sun.showSky = sky; sun.shadows = shadows; sun.animate = animate;
    }
}
extern int g_gfxQuality;

bool IndustryDemo(float x, float z, std::string& out); // city_industry.inl
bool IndustryDump(const std::string& path, std::string& out);

// Where a slow frame went: marks through RenderFrame; at the next frame's start a gap over 50 ms is logged by section
// (outside = between RenderFrame calls: the message loop). Costs a few counter reads a frame.
struct FrameSections {
    static constexpr int kMax = 16;
    const char* name[kMax] = {};
    int64_t at[kMax] = {};
    int n = 0;
    int64_t entry = {}, end = {};
    void Mark(const char* what) { if (n < kMax) { name[n] = what; at[n++] = tbd::platform::Ticks(); } }
};
FrameSections g_frameSections;
inline void FrameMark(const char* what) { g_frameSections.Mark(what); }

// ------------------------------------------------------------------- city roads: glue
float RoadsGround(float x, float z) { return g_engine.terrain.params.enabled ? g_engine.terrain.HeightAt(x, z) : 0.0f; }

// The city camera never goes under the ground or right up against it: the eye keeps kCamClearance above the terrain at its own
// spot (and a little more at the target), by tilting up first and lifting the target only when tilting is not enough. Buildings
// are not tested here; that needs a spatial query over the city cells.
// The city game (g_terrainSandbox) uses the CS2-style camera of src/city_camera.h instead: tilt from straight down to the
// horizon, street level to the whole map, the pivot on the ground and lifted (not tilted) to keep the eye 2 m up.
bool CityCamOn() { return g_terrainSandbox && !g_play.active && g_engine.drag != Drag::Fly; }
float CityCamGround(float x, float z) { // terrain or sea, whichever is higher
    const float sea = g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind >= 1 ? tbd::IslandMap::kSeaLevel : -1e9f;
    return std::max(RoadsGround(x, z), sea);
}
void CityCamGroundFloor() {
    if (g_play.active || g_engine.drag == Drag::Fly) return;
    if (CityCamOn()) { tbd::citycam::GroundFloor(g_engine.camera, CityCamGround); return; }
    constexpr float kCamClearance = 1.5f; // metres of the city's units
    tbd::OrbitCamera& cam = g_engine.camera;
    const tbd::Vec3 eye = cam.Eye();
    const float floorY = RoadsGround(eye.x, eye.z) + kCamClearance;
    if (eye.y >= floorY) return;
    const float need = (floorY - cam.target.y) / std::max(cam.distance, 1e-3f);
    if (need < 0.98f) cam.pitch = std::max(cam.pitch, std::asin(std::max(need, -1.0f)));
    else cam.target.y += floorY - eye.y; // the camera is too close to tilt clear: lift the target instead
}

std::string RoadsUpper(const std::string& s) { // the toast font has capitals only
    std::string out = s;
    for (char& c : out) c = (char)toupper((unsigned char)c);
    return out;
}

// Once: the ground / terrain levelling hooks and the TBD_ROADS startup options.
void RoadsInit() {
    static bool done = false;
    if (done) return;
    done = true;
    g_roads.ground = RoadsGround;
    g_roads.net.ground = RoadsGround;
    g_roads.flatten = [](float x, float z, float hard, float radius, float target) {
        if (!g_engine.terrain.params.enabled) return;
        g_engine.terrain.Brush(x, z, radius, 1.0f, tbd::HeightField::BrushMode::Flatten, target, hard);
    };
    char v[16] = {};
    if (tbd::platform::GetEnvA("TBD_ROADS", v, sizeof(v)) <= 0) return;
    g_roads.Open();
    if (strcmp(v, "demo") != 0) return;
    using tbd::city::Cubic;
    using tbd::city::V2;
    // the demo town goes on the driest, least steep 600 x 480 m patch near the origin (roads under the water plane drown)
    V2 O{0, 0};
    float best = -1e9f;
    for (float cz = -2400; cz <= 2400; cz += 150)
        for (float cx = -2400; cx <= 2400; cx += 150) {
            float lo = 1e9f, hi = -1e9f;
            for (int j = 0; j <= 8; ++j)
                for (int i = 0; i <= 10; ++i) {
                    const float h = RoadsGround(cx - 300 + i * 60.0f, cz - 240 + j * 60.0f);
                    lo = std::min(lo, h), hi = std::max(hi, h);
                }
            const float score = std::min(lo, 6.0f) - (hi - lo) * 0.25f - std::sqrt(cx * cx + cz * cz) * 0.001f;
            if (lo > 1.0f && score > best) best = score, O = {cx, cz};
        }
    tbd::Log("Roads demo town at (%.0f, %.0f)", O.x, O.z);
    auto prof = [](const char* n) { return tbd::city::ProfileIndex(n); };
    auto P = [&](float x, float z) { return V2{O.x + x, O.z + z}; };
    auto road = [&](V2 a, V2 b, const char* n, float ea = 0, float eb = 0) {
        a = P(a.x, a.z), b = P(b.x, b.z);
        g_roads.BuildRoad(Cubic::Line(a, b), RoadsGround(a.x, a.z) + ea, RoadsGround(b.x, b.z) + eb, prof(n));
    };
    auto curve = [&](Cubic c, const char* n) {
        c.p0 = P(c.p0.x, c.p0.z), c.p1 = P(c.p1.x, c.p1.z), c.p2 = P(c.p2.x, c.p2.z), c.p3 = P(c.p3.x, c.p3.z);
        g_roads.BuildRoad(c, RoadsGround(c.p0.x, c.p0.z), RoadsGround(c.p3.x, c.p3.z), prof(n));
    };
    road({-260, 0}, {260, 0}, "Four-Lane Divided Road");
    road({0, -220}, {0, 220}, "Two-Lane Road");
    road({120, 0}, {120, 160}, "Two-Lane Perpendicular Parking Road");
    road({-120, -140}, {-120, 0}, "Three-Lane One-Way Road");
    curve(Cubic::Quad({0, -140}, {150, -140}, {150, 0}), "Two-Lane Road");
    curve({{120, 160}, {170, 160}, {220, 110}, {250, 160}}, "Alley");
    road({-120, 0}, {-120, 220}, "Two-Lane Road");
    {
        const V2 c = P(-120, 110);
        g_roads.BuildAsset(tbd::city::RoadTool::AssetIndex("Medium Roundabout"), {c.x, RoadsGround(c.x, c.z), c.z});
    }
    road({-300, -90}, {300, -90}, "Two-Lane Two-Way Highway", 14, 14); // a bridge over the town
    road({60, 220}, {60, 60}, "Two-Lane Road", -22, 0);                 // out of a tunnel
    road({-260, 60}, {-170, 60}, "Medium Quay With Two-Way Road");
    road({30, 40}, {100, 120}, "Pedestrian Path");
    road({-300, 270}, {120, 270}, "Double Track Railway");                            // SPOM track: straight, then a curve
    curve(Cubic::Quad({120, 270}, {280, 270}, {280, 110}), "Double Track Railway");
    g_engine.camera.target = {O.x, RoadsGround(O.x, O.z), O.z};
    // TBD_ROADS_START="x z [mode]": a first click at a demo-relative point (and a tool mode), so a capture with
    // TBD_GUI_POINTER shows the live ghost, snapping and labels
    char st[64] = {};
    float sx, sz;
    int mode = 0;
    if (tbd::platform::GetEnvA("TBD_ROADS_START", st, sizeof(st)) > 0 && sscanf(st, "%f %f %d", &sx, &sz, &mode) >= 2) {
        g_roads.SetMode((tbd::city::ToolMode)std::clamp(mode, 0, 7));
        const V2 q = P(sx, sz);
        g_roads.Move(true, {q.x, RoadsGround(q.x, q.z), q.z});
        g_roads.Click();
    }
}

// Keeps the cursor's snapped point current (the GUI and Alt-orbit leave it alone).
void RoadsCursor() {
    tbd::Vec3 hit;
    const bool overGui = g_gui.WantsMouseAt((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY);
    const bool on = g_terrainEdit.mouseX >= 0 && !overGui &&
                    GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit);
    g_roads.Move(on, hit);
}

// The SPOM track texture (TrackPS in mesh.hlsl): track albedo with the baked height field in alpha (assets/rail/track/tex).
tbd::TextureId TrackSpomTexture(tbd::Renderer& r) {
    const std::wstring dir = tbd::ExeDirectory() + L"\\..\\assets\\rail\\track\\tex\\";
    std::string a, h;
    std::vector<uint8_t> rgba, hgt;
    int w = 0, ht = 0, w2 = 0, h2 = 0;
    if (!tbd::ReadAllText(dir + L"track_albedo.jpg", a) || !tbd::DecodeImage((const uint8_t*)a.data(), a.size(), rgba, w, ht) ||
        !tbd::ReadAllText(dir + L"track_height.png", h) || !tbd::DecodeImage((const uint8_t*)h.data(), h.size(), hgt, w2, h2) || w != w2 || ht != h2) {
        tbd::Log("Rail: missing SPOM track texture in %ls", dir.c_str());
        return tbd::kNoTexture;
    }
    for (size_t i = 0; i < (size_t)w * ht; ++i) rgba[i * 4 + 3] = hgt[i * 4]; // height (grey) into alpha
    return r.CreateTexture(rgba.data(), w, ht);
}

void CityRoadsDraw(tbd::Renderer& r, const tbd::Mat4& viewProj, bool rail);
void CityRoadsRebuild(tbd::Renderer& r, const tbd::city::MeshOut& out); // city_buildings.inl: the network split per CityCell (Track R9)
bool CityRoadsByCell(); // true: the roads draw per cell (CityRoadsDraw) and the whole-network meshes are not made
// Textures once per renderer; meshes whenever the network changed. Runs before BeginFrame (it creates resources).
void RoadsSync(tbd::Renderer& r) {
    RoadsInit();
    if (!g_roadGpu.textures) {
        g_roadGpu.textures = true;
        for (int m = 0; m < tbd::city::kMatCount; ++m) {
            const char* name = tbd::city::kMatFile[m];
            if (!name) continue;
            for (int k = 0; k < 2; ++k) {
                std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\roads\\tex\\";
                for (const char* c = name; *c; ++c) path += (wchar_t)*c;
                path += k ? L"_normal.png" : L"_albedo.png";
                std::string bytes;
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h))
                    (k ? g_roadGpu.normal[m] : g_roadGpu.albedo[m]) = r.CreateTexture(rgba.data(), w, h);
                else tbd::Log("Roads: missing texture %ls", path.c_str());
            }
        }
        g_roadGpu.albedo[(int)tbd::city::Mat::Track] = TrackSpomTexture(r);
        for (int k = 0; k < tbd::city::tmodel::kSlabTexCount; ++k) { // relief in alpha: tools/building_gen/depot_tex.py
            std::wstring path = tbd::ExeDirectory() + L"\\..\\assets\\rail\\depot\\tex\\";
            for (const char* c = tbd::city::tmodel::kSlabTex[k].file; *c; ++c) path += (wchar_t)*c;
            path += L".png";
            std::string bytes;
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (tbd::ReadAllText(path, bytes) && tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), rgba, w, h)) g_roadGpu.slab[k] = r.CreateTexture(rgba.data(), w, h);
            else tbd::Log("Transport: missing slab texture %ls", path.c_str());
        }
    }
    if (g_roads.net.version == g_roadGpu.built) return;
    g_roadGpu.built = g_roads.net.version;
    static tbd::city::MeshOut out;
    tbd::city::BuildNetwork(g_roads.net, RoadsGround, out);
    g_roadGpu.triangles = out.Triangles();
    for (int m = 0; m < tbd::city::kMatCount; ++m) {
        if (g_roadGpu.mesh[m] != tbd::kInvalidMesh) r.RetireMesh(g_roadGpu.mesh[m]);
        g_roadGpu.mesh[m] = tbd::kInvalidMesh;
        const std::vector<tbd::Vertex>& v = out.byMat[m];
        if (!v.empty() && !CityRoadsByCell()) g_roadGpu.mesh[m] = r.CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles);
    }
    if (g_roadGpu.arrows != tbd::kInvalidMesh) r.RetireMesh(g_roadGpu.arrows);
    g_roadGpu.arrows = out.arrows.empty() ? tbd::kInvalidMesh : r.CreateMesh(out.arrows.data(), (uint32_t)out.arrows.size(), tbd::Topology::Triangles);
    CityRoadsRebuild(r, out);
}

// Road names: generated per chain of segments (city/road_names.h). F2 over a road renames its whole chain.
tbd::city::RoadNames g_roadNames;
struct RoadRename {
    bool active = false;
    int seg = -1;
    std::string text;
} g_roadRename;
bool RoadRenameActive() { return g_roadRename.active; }
int g_hoverSeg = -1; // the road under the road tool's cursor (set while its labels draw)
float g_hoverT = 0;
int g_hoverSide = 1;

void RoadRenameBegin() {
    if (g_hoverSeg < 0) return;
    g_roadNames.Update(g_roads.net);
    g_roadRename = {true, g_hoverSeg, g_roadNames.Label(g_hoverSeg)};
}
// Keys while the rename box is open: returns true (it swallows every key so tool shortcuts don't fire while typing).
bool RoadRenameKey(int key) {
    if (key == (int)tbd::platform::Key::Escape) g_roadRename.active = false;
    else if (key == (int)tbd::platform::Key::Return) {
        g_roadRename.active = false;
        g_roadNames.Rename(g_roads.net, g_roadRename.seg, g_roadRename.text); // empty text clears back to the generated name
    } else if (key == (int)tbd::platform::Key::Back && !g_roadRename.text.empty()) g_roadRename.text.pop_back();
    return true;
}
void RoadRenameChar(uint32_t ch) {
    if (ch >= 32 && ch < 127 && g_roadRename.text.size() < 32) g_roadRename.text.push_back((char)ch);
}

// Length / slope / angle / elevation chips over the world, and the reason a placement is refused, beside the cursor.
void RoadsWorldLabels() {
    auto project = [](tbd::Vec3 p, float& x, float& y) {
        const tbd::Vec4 c = g_lastViewProj * tbd::Vec4{p.x, p.y, p.z, 1.0f};
        if (c.w <= 0.01f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * (float)g_engine.vpW;
        y = (0.5f - c.y / c.w * 0.5f) * (float)g_engine.vpH;
        return true;
    };
    static std::vector<tbd::city::WorldLabel> labels;
    g_roads.Labels(labels);
    float x, y;
    for (const tbd::city::WorldLabel& l : labels)
        if (project(l.p, x, y)) g_gui.Chip(x, y, l.text);
    if (!g_roads.error.empty() && g_roads.haveCursor && project(g_roads.cursor.p, x, y))
        g_gui.Chip(x, y + 34.0f * DpiScale(), g_roads.error.c_str(), tbd::gui::Rgba(170, 40, 34, 230));
    else if (g_roads.haveCursor && g_roads.Current().kind == tbd::city::AssetKind::Road && !g_roads.Pending() &&
             g_roads.mode != tbd::city::ToolMode::Replace && g_roads.mode != tbd::city::ToolMode::Swap &&
             g_roads.mode != tbd::city::ToolMode::Bulldoze && project(g_roads.cursor.p, x, y))
        g_gui.Chip(x, y + 34.0f * DpiScale(), tbd::city::ModeName(g_roads.mode));

    // the road under the cursor: name, class, house number; F2 renames
    g_hoverSeg = -1;
    if (g_roads.haveCursor && !g_roads.Pending() && !g_roadRename.active) {
        const tbd::city::Network& net = g_roads.net;
        const tbd::city::V2 p{g_roads.cursor.p.x, g_roads.cursor.p.z};
        float t = 0, d = 0;
        const int s = net.SegmentAt(p, 0.5f, t, d);
        if (s >= 0 && !net.IsRailSeg(s)) {
            g_roadNames.Update(net);
            if (const tbd::city::RoadChain* ch = g_roadNames.ChainOf(s)) {
                const tbd::city::Cubic c = net.Curve(s);
                g_hoverSeg = s, g_hoverT = t;
                g_hoverSide = tbd::city::Dot2(tbd::city::LeftOf(c.Tan(t)), p - c.At(t)) > 0 ? 1 : -1;
                std::string line = ch->name.Full() + "  -  " + tbd::city::RoadNames::ClassLabel(ch->cls);
                if (ch->cls != tbd::city::RoadClass::Highway && ch->cls != tbd::city::RoadClass::Path)
                    line += "  -  No. " + std::to_string(g_roadNames.HouseNumber(net, s, t, g_hoverSide));
                if (project(g_roads.cursor.p, x, y)) g_gui.Chip(x, y - 34.0f * DpiScale(), (line + "  [F2 rename]").c_str());
            }
        }
    }
    if (g_roadRename.active && project(g_roads.cursor.p, x, y)) {
        const std::string box = "Rename road: " + g_roadRename.text + "_   [Enter ok, Esc cancel, empty = auto]";
        g_gui.Chip(x, y - 34.0f * DpiScale(), box.c_str(), tbd::gui::Rgba(30, 90, 150, 235));
    }
}

#include "city_zones.inl"
#include "city_utilities.inl" // utilities: power, water, sewage sources, lines, underground view, notifications
#include "city_transport.inl" // transport buildings: depots, stations, harbours, airports (hologram, then construction)
#include "city_industry.inl" // specialized industry: hubs, drawn work areas, resource heat maps
#include "city_garbage.inl"  // waste management: garbage produced by lots, collected by landfills, incinerators, recycling
#include "city_game.inl"     // the city game: new random map, default corridor, game saves
#include "city_sim.inl"      // the city sim scheduler: fixed 1 game minute ticks under a per-frame budget
#include "city_radio.inl"    // the city radio: Elton reacts to what the player did, plus The Weekly Rundown
#include "city_herds.inl"    // neighbourhoods, herds over the day and the visible cars made from them (src/city/sim_hoods.h ...)
#include "city_overlays.inl" // info overlays: power / water / sewage / land value / traffic / coverage tint
#include "city_stress.inl"   // TBD_CITY_STRESS=<n>: a road grid with n zoned lots and the bad cases, for render baselines
#include "city_buildings.inl" // zoned buildings per CityCell: instanced near, chunk bakes and volumes or merged proxies far (Track R)
#include "gui_preview_dump.inl" // TBD_GUI_PREVIEW_DUMP: the procedural build items as triangles for tools/gui_atlas/render.html

// Which HUD button the open transport panel belongs to: Transportation, or the service tab's own (Fire, Healthcare, Police, ...).
int HudForTransportTab() {
    using tbd::gui::HudTool;
    switch (g_transportTool.tab) {
    case tbd::city::TTab::Fire: return (int)HudTool::Fire;
    case tbd::city::TTab::Health: case tbd::city::TTab::Death: return (int)HudTool::Health;
    case tbd::city::TTab::Police: return (int)HudTool::Police;
    case tbd::city::TTab::Education: return (int)HudTool::Education;
    case tbd::city::TTab::Comms: return (int)HudTool::Comms;
    default: return (int)HudTool::Transport;
    }
}

// The HUD toolbar's launch: closes whatever tool is open, then opens `tool` (a gui::HudTool); the same button again closes it.
void HudOpenTool(int tool) {
    using tbd::gui::HudTool;
    const bool utilWasOpen = g_utilTool.active;
    const int open = g_zoneTool.active ? (int)HudTool::Zoning
                     : g_roads.active ? (int)HudTool::Roads
                     : g_utilTool.active ? (g_utilTool.tab == tbd::city::UTab::Electricity ? (int)HudTool::Electricity : (int)HudTool::Water)
                     : g_transportTool.active ? HudForTransportTab()
                     : g_indTool.active ? (g_indTool.waste ? (int)HudTool::Garbage : (int)HudTool::Industry) : -1;
    g_zoneTool.active = false;
    if (g_roads.active) g_roads.Close();
    g_utilTool.Close();
    g_transportTool.active = false, g_transportTool.def = -1;
    g_indTool.active = false, g_indTool.mode = IndustryTool::Pick, g_indTool.drawing = -1, g_indTool.wsel = -1, g_indTool.point = 0;
    if (utilWasOpen) UtilToolClosed();
    if (open == tool) return; // toggled off
    g_terrainEdit.on = false;
    switch ((HudTool)tool) {
    case HudTool::Zoning: g_zoneTool.active = true; break;
    case HudTool::Roads: g_roads.Open(); break;
    case HudTool::Electricity: g_utilTool.active = true, g_utilTool.SelectTab(tbd::city::UTab::Electricity); break;
    case HudTool::Water: g_utilTool.active = true, g_utilTool.SelectTab(tbd::city::UTab::Water); break;
    case HudTool::Transport: g_transportTool.active = true; if (tbd::city::IsServiceTab(g_transportTool.tab)) g_transportTool.SelectTab(tbd::city::TTab::Rail); break;
    case HudTool::Fire: g_transportTool.active = true; g_transportTool.SelectTab(tbd::city::TTab::Fire); break;
    case HudTool::Health: g_transportTool.active = true; g_transportTool.SelectTab(tbd::city::TTab::Health); break;
    case HudTool::Police: g_transportTool.active = true; g_transportTool.SelectTab(tbd::city::TTab::Police); break;
    case HudTool::Education: g_transportTool.active = true; g_transportTool.SelectTab(tbd::city::TTab::Education); break;
    case HudTool::Comms: g_transportTool.active = true; g_transportTool.SelectTab(tbd::city::TTab::Comms); break;
    case HudTool::Industry:
    case HudTool::Garbage:
        g_indTool.active = true;
        g_indTool.waste = tool == (int)HudTool::Garbage;
        g_indTool.kind = g_indTool.waste ? (int)tbd::city::IndKind::Landfill : 0;
        break;
    default: break;
    }
}

float g_guiScaleSel = 1.0f; // the HUD Options menu: interface size multiplier
#include "glass_dock_host.inl" // the Glass Dock+ GUI (src/gui/glass_dock.h) on the city game

// ------------------------------------------------------------------- in-game GUI, per frame
// Runs the GUI frame (input queued by GuiMouse since the last frame), the terrain toolbar, and draws it over everything.
// TBD_GUI_POINTER="x y [down]": a scripted pointer for captures (there is no mouse in a TBD_CAPTURE run); with "down" the
// button goes down on the 30th GUI frame (once the layout has settled) and stays down, showing the pressed / dragged state.
void DrawGameGui(tbd::Renderer& r, float topInset) {
    static bool terrainPanelOpen = false; // the HUD's Terrain button
    static float terrainSlide = 0.0f;     // 0 hidden .. 1 shown, eased
    const bool show = g_guiVisible && !g_play.active; // the road tool's "Roads" button is always there
    if (!show) {
        g_gui.Reset(); // hidden panels must stop taking clicks at once
        return;
    }
    struct ScriptedPointer {
        bool on = false, down = false;
        int frames = 0; // the press waits for a settled layout: auto-sized panels take a frame to find their place
        float x = 0.0f, y = 0.0f;
    };
    static ScriptedPointer pointer = [] {
        ScriptedPointer p;
        char v[64] = {}, word[16] = {};
        if (tbd::platform::GetEnvA("TBD_GUI_POINTER", v, sizeof(v)) > 0) {
            const int n = sscanf(v, "%f %f %15s", &p.x, &p.y, word);
            p.on = n >= 2;
            p.down = n == 3 && strcmp(word, "down") == 0;
        }
        return p;
    }();
    if (pointer.on) {
        g_guiInput.Move(pointer.x, pointer.y);
        if (pointer.down && ++pointer.frames == 30) g_guiInput.Down(pointer.x, pointer.y);
        g_terrainEdit.mouseX = (int)pointer.x, g_terrainEdit.mouseY = (int)pointer.y; // the status line reads the cursor
    }
    g_gui.SetInsets(0.0f, topInset, 0.0f, 0.0f);
    static int64_t last = {};
    int64_t now;
    now = tbd::platform::Ticks();
    const float guiDt = last ? (float)((now - last) / (double)g_engine.tickFreq) : 1.0f / 60.0f; // eases transitions
    last = now;
    static bool previewDumped = false; // TBD_GUI_PREVIEW_DUMP=<file.js> (gui_preview_dump.inl), once the world is up
    if (!previewDumped) {
        previewDumped = true;
        char dumpPath[512] = {};
        if (tbd::platform::GetEnvA("TBD_GUI_PREVIEW_DUMP", dumpPath, sizeof(dumpPath)) > 0) {
            std::string msg;
            GuiPreviewDump(dumpPath, msg);
            tbd::Log("GUI preview dump: %s", msg.c_str());
        }
    }
    float guiScale = DpiScale() * g_guiScaleSel;
    if (!gdock::Legacy()) gdock::DeviceAndScale(guiScale); // the dock's point sizes: the device's scale
    g_gui.BeginFrame(g_guiInput.Take((float)g_engine.vpW, (float)g_engine.vpH, guiScale, guiDt));
    tbd::gui::TerrainStatus status;
    status.tiles = g_engine.terrain.field.TileCount();
    tbd::Vec3 hit;
    if (g_terrainEdit.mouseX >= 0 &&
        GroundHit(g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH), hit)) {
        status.cursorOnGround = true;
        status.east = hit.x;
        status.north = -hit.z; // world z points south (island_map.h)
        status.height = hit.y;
    }
    const tbd::gui::TerrainToolbarState state{&g_terrainEdit.on, &g_terrainEdit.mode, &g_terrainEdit.radius, &g_terrainEdit.metresPerSecond};
    // The status line rides above the toast chip row (DrawStatsText: 8 px margin, a 1.5 px-cell chip with 4 px padding, a gap).
    const float toastLift = 8.0f + 7.0f * 1.5f + 8.0f + 8.0f;
    terrainSlide += ((terrainPanelOpen && g_engine.terrain.params.enabled ? 1.0f : 0.0f) - terrainSlide) * std::min(1.0f, guiDt * 12.0f);
    if (std::fabs(terrainSlide - (terrainPanelOpen ? 1.0f : 0.0f)) < 0.003f) terrainSlide = terrainPanelOpen ? 1.0f : 0.0f;
    const float ease = terrainSlide * terrainSlide * (3.0f - 2.0f * terrainSlide); // smoothstep
    const tbd::gui::TerrainAction action = gdock::Legacy() ? tbd::gui::TerrainToolbar(g_gui, state, status, 4.0f + 106.0f, ease) : tbd::gui::TerrainAction::None;
    const bool roadsWereOpen = g_roads.active;
    if (g_roads.active) RoadsWorldLabels(); // under the panels
    if (g_transportTool.active) TransportWorldLabels();
    UtilWorldLabels();   // the no electricity / water / sewage / road bubbles over buildings
    UtilToolLabels();    // the utilities tool's cursor chip
    static tbd::gui::CityHud hud;
    hud.selected = g_zoneTool.active ? (int)tbd::gui::HudTool::Zoning
                   : g_roads.active ? (int)tbd::gui::HudTool::Roads
                   : g_utilTool.active ? (g_utilTool.tab == tbd::city::UTab::Electricity ? (int)tbd::gui::HudTool::Electricity : (int)tbd::gui::HudTool::Water)
                   : g_transportTool.active ? HudForTransportTab()
                   : g_indTool.active ? (g_indTool.waste ? (int)tbd::gui::HudTool::Garbage : (int)tbd::gui::HudTool::Industry) : -1;
    hud.terrainOpen = terrainPanelOpen;
    SimHudFill(hud);
    RadioHudFill(hud); // the City News feed
    if (g_simRemoteBudget >= 0) hud.budget = g_simRemoteBudget == 1, g_simRemoteBudget = -1;
    if (!gdock::Legacy()) { // Glass Dock+: its bar, panels and placement bar replace the HUD and the tools' panels
        gdock::Frame(r, hud);
        RadioHudRead(hud);
        SimHudRead(hud);
        g_gui.EndFrame();
        const tbd::Mat4 ortho = tbd::OrthoPixels((float)g_engine.vpW, (float)g_engine.vpH);
        const std::vector<tbd::UiVertex>& v = g_gui.UiVertices();
        if (!g_gui.Classic() && !v.empty()) {
            r.CaptureUiBackdrop(); // the one shared blur source of every glass panel
            r.DrawUi(v.data(), (uint32_t)v.size(), ortho);
        } else if (g_gui.Classic() && !g_gui.Vertices().empty())
            r.DrawTransient(g_gui.Vertices().data(), (uint32_t)g_gui.Vertices().size(), tbd::Topology::Triangles, ortho, tbd::Mat4::Identity(), false);
        return;
    }
    g_guiScaleSel = tbd::gui::HudSizeValues()[std::clamp(hud.sizeIdx, 0, 4)];
    const float hudH = tbd::gui::CityHudFrame(g_gui, hud, g_clock, 4.0f);
    tbd::gui::CityNewsPanel(g_gui, hud);
    RadioHudRead(hud);
    SimHudRead(hud); // the Budget panel's tax rates and funding into the city sim
    OverlaysHud(); // the info view toggles and legend, right column under the side icons
    g_gameSlot = hud.slot;
    if (hud.gameAction) g_gameAction = hud.gameAction, hud.gameAction = 0; // bottom centre, tight to the edge; the tools sit above it
    if (hud.request == (int)tbd::gui::HudTool::Terrain) {
        terrainPanelOpen = !terrainPanelOpen;
        if (!terrainPanelOpen) g_terrainEdit.on = false; // hidden: left clicks are no longer the brush's
    } else if (hud.request >= 0) HudOpenTool(hud.request);
    const float toolLift = 4.0f + hudH;
    if (!g_zoneTool.active && !g_indTool.active && !g_transportTool.active && !g_utilTool.active) tbd::gui::RoadToolbar(g_gui, g_roads, toolLift);
    if (!g_roads.active && !g_indTool.active && !g_transportTool.active && !g_utilTool.active && tbd::gui::ZoneToolbar(g_gui, g_zoneTool, toolLift)) g_terrainEdit.on = false;
    if (!g_roads.active && !g_zoneTool.active && !g_transportTool.active && !g_utilTool.active && IndustryToolbar(toolLift)) g_terrainEdit.on = false;
    if (!g_roads.active && !g_zoneTool.active && !g_indTool.active && !g_transportTool.active) {
        const bool utilWasOpen = g_utilTool.active;
        if (tbd::gui::UtilityToolbar(g_gui, g_utilTool, g_utilReport, toolLift)) g_terrainEdit.on = false;
        if (utilWasOpen && !g_utilTool.active) UtilToolClosed();
    }
    if (!g_roads.active && !g_zoneTool.active && !g_indTool.active && !g_utilTool.active) {
        if (tbd::gui::TransportToolbar(g_gui, g_transportTool, toolLift)) g_terrainEdit.on = false;
        if (g_transportTool.openRoad >= 0) { // a railway track was picked: the road tool lays it
            g_transportTool.active = false;
            g_transportTool.def = -1;
            g_roads.Open();
            g_roads.Select(g_transportTool.openRoad);
            g_terrainEdit.on = false;
        }
    }
    if (g_roads.active && !roadsWereOpen) g_terrainEdit.on = false; // left clicks are the road tool's now
    g_gui.EndFrame();
    if (action == tbd::gui::TerrainAction::Save) SaveTerrainEdits();
    else if (action == tbd::gui::TerrainAction::Load) LoadTerrainEdits();
    const tbd::Mat4 ortho = tbd::OrthoPixels((float)g_engine.vpW, (float)g_engine.vpH);
    if (g_gui.Classic()) { // the fallback: flat quads and the pixel font on the old 2D path
        const std::vector<tbd::Vertex>& v = g_gui.Vertices();
        if (!v.empty()) r.DrawTransient(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles, ortho, tbd::Mat4::Identity(), false);
    } else { // smooth: rounded boxes, shadows and SDF text on the UI pipeline (Renderer::DrawUi, shaders/ui.hlsl)
        const std::vector<tbd::UiVertex>& v = g_gui.UiVertices();
        if (!v.empty()) r.DrawUi(v.data(), (uint32_t)v.size(), ortho);
    }
}

void RenderFrame() {
    {
        FrameSections& f = g_frameSections;
        int64_t now;
        now = tbd::platform::Ticks();
        const auto ms = [](int64_t a, int64_t b) { return (double)(b - a) * 1000.0 / (double)g_engine.tickFreq; };
        if (f.entry && g_engine.tickFreq && ms(f.entry, now) > 50.0) {
            char line[768];
            int pos = snprintf(line, sizeof(line), "Frame: %.1f ms:", ms(f.entry, now));
            int64_t prev = f.entry;
            for (int i = 0; i < f.n && pos > 0 && pos < (int)sizeof(line); ++i) {
                const double d = ms(prev, f.at[i]);
                if (d >= 1.0) pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s %.1f", f.name[i], d);
                prev = f.at[i];
            }
            if (pos > 0 && pos < (int)sizeof(line)) snprintf(line + pos, sizeof(line) - (size_t)pos, " outside %.1f", ms(prev, now));
            tbd::Log("%s", line);
        }
        f.n = 0;
        f.entry = now;
    }
    // TBD_HITCHLOG=<ms>: logs every gap between frames longer than that, in any run: the way to catch a freeze in the editor or in Play.
    static const int hitchMs = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_HITCHLOG", v, sizeof(v)) > 0 ? atoi(v) : 0; }();
    if (hitchMs > 0) {
        static uint64_t lastFrame = 0;
        const uint64_t t = tbd::platform::TickCountMs();
        if (lastFrame && t - lastFrame >= (uint64_t)hitchMs) tbd::Log("Hitch: %llu ms between frames at %llu ms", t - lastFrame, t);
        lastFrame = t;
    }
    SceneLoadTick(16.0);
    FrameMark("sceneload");
    surface_async::Pump();
    FrameMark("surface-pump");
    ground_bake::Tick();
    FrameMark("ground-bake");
    if (!g_engine.running || !g_engine.renderer) return;
    if (!tbd::shell::DevReloadTick()) return; // shader hot reload (Windows dev builds); false: no renderer left
    FrameMark("hot-reload");
    tbd::shell::PumpRemote();
    FrameMark("remote");
    if (!g_engine.renderer) return;
    tbd::Renderer& r = *g_engine.renderer;
    g_engine.stats.BeginFrame();
    g_profiler.BeginFrame();
    static int64_t frameCpuStart;
    frameCpuStart = tbd::platform::Ticks(); // TBD_DRAWCATS: CPU ms of the frame = this to the end, less the fence wait and Present
    float dt = FrameDelta();
    FrameMark("frame-start");
    ZonesSync(dt); // the game clock, zone growth and construction
    FrameMark("zones-sync");
    UtilSync(dt); // utilities: the no-road clock, the networks, the source buildings
    FrameMark("util-sync");
    TransportSync(dt);
    FrameMark("transport-sync");
    GameSync(); // HUD save / load / new map, restored zone paint // transport buildings: planned -> building -> done on the same clock
    IndustrySync(dt); // specialized industry output
    FrameMark("industry-sync");
    GarbageSync(dt);  // garbage produced, collected and processed
    SimSync(dt);      // the city sim: fixed game-minute ticks under a per-frame budget (reads the clock ZonesSync advanced)
    HerdsSync(dt);    // the cars near the camera made from the herds (city_herds.inl)
    OverlaysSync();   // the info overlay mesh, rebuilt only when its colours changed
    RadioSync(dt);    // the city radio reacts to what changed this frame
    FrameMark("sim-sync");
    CityStressSync(); // TBD_CITY_STRESS only
    CityChunkCheckSync(); // TBD_CHUNK_CHECK only
    // TBD_PLAYTEST: Play starts itself, on a fixed clock; see PlayerScript.
    static bool playtestTried = false;
    if (!playtestTried && !g_engine.scenePath.empty() && !SceneLoadActive()) { // the scene is open
        playtestTried = true;
        if (g_playtest.Start()) {
            g_playtest.OpenTrace(r.Name());
            tbd::shell::Transport(0);
            g_engine.sun.animate = false;
        }
    }
    if (g_playtest.Active()) dt = 1.0f / 60.0f;
    UpdateDynamicResolution(r, dt);
    static wchar_t capturePath[tbd::platform::kMaxPath] = {};
    // TBD_BENCH_FRAMES=<n>: how many frames a benchmark measures after the warm-up (default 179). The count restarts whenever a capture still
    // has to wait for loading, so those waits are never measured and a run reports exactly the frames asked for.
    static const int benchFrames = [] {
        char v[16] = {};
        return tbd::platform::GetEnvA("TBD_BENCH_FRAMES", v, sizeof(v)) > 0 ? std::max(10, atoi(v)) : 179;
    }();
    // TBD_SETTLE=<frames>: more frames before a capture, for the irradiance probes to converge (192 of 18432 a frame, blending
    // 15% each pass: about 1500 frames). gameenginetools\lighting_reference\compare.ps1 sets it.
    static const int settleFrames = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_SETTLE", v, sizeof(v)) > 0 ? std::max(0, atoi(v)) : 0; }();
    static int captureFramesLeft = tbd::platform::GetEnvW(L"TBD_CAPTURE", capturePath, tbd::platform::kMaxPath) > 0 ? benchFrames + 61 + settleFrames : -1;
    // TBD_FRAMELOG=<file.csv>: one row of section times per measured frame, to find what a slow frame spent its time on.
    enum TraceMark { kTraceStart, kTracePre, kTraceBegin, kTraceAnim, kTraceShadow, kTraceScene, kTraceEnd, kTraceMarks };
    int64_t traceTick[kTraceMarks] = {};
    const auto traceMark = [&](TraceMark m) { traceTick[m] = tbd::platform::Ticks(); };
    traceMark(kTraceStart);
    // TBD_SHOT="hour tx ty tz distance yaw pitch": a fixed test view without editor overlays, for captures.
    static float shot[7];
    static const bool testShot = [] {
        char v[128] = {};
        return tbd::platform::GetEnvA("TBD_SHOT", v, sizeof(v)) > 0 &&
               sscanf(v, "%f %f %f %f %f %f %f", shot, shot + 1, shot + 2, shot + 3, shot + 4, shot + 5, shot + 6) == 7;
    }();
    static world_streaming::ShotTour shotTour;
    if (testShot) shotTour.Advance(shot + 1, dt, !g_stream.Active() || g_stream.IsIdle());
    if (testShot) { // every frame: mouse input reaching the window must not move a capture's camera
        g_engine.sun.hour = shot[0];
        g_engine.camera.target = {shot[1], shot[2], shot[3]};
        g_engine.camera.distance = shot[4];
        g_engine.camera.yaw = shot[5];
        g_engine.camera.pitch = shot[6];
    }
    if (g_play.active) {
        if (g_play.Lost()) tbd::shell::Transport(2); // alt-tab or a dialog: back to the editor, mouse released
        else if (g_play.player) {
            tbd::PlayerInput input;
            if (g_playtest.Active()) {
                const bool settled = g_player.Placed() && (!g_stream.Active() || g_stream.IsIdle());
                g_playtest.Tick(input, g_player, g_physics, g_engine.camera, !settled || g_npcs.Pending(), dt, g_lastAnimMs);
                g_engine.sun.hour = g_playtest.Hour();
            } else {
                g_play.PollMouse(input.lookX, input.lookY);
                using PK = tbd::platform::Key; auto down = [](PK k) { return tbd::platform::KeyDownAsync(k); };
                input.forward = (down(PK::W) ? 1.0f : 0.0f) - (down(PK::S) ? 1.0f : 0.0f);
                input.right = (down(PK::D) ? 1.0f : 0.0f) - (down(PK::A) ? 1.0f : 0.0f);
                input.run = down(PK::Shift);
                static bool spaceWas = false;
                const bool space = down(PK::Space);
                input.jump = space && !spaceWas;
                spaceWas = space;
            }
            g_player.footstep = [](tbd::Vec3 at, float volume, float pitch) { tbd::audio::PlayOneShot("sfx/footstep", at, volume, pitch); };
            g_player.Update(g_engine.scene, input, dt, g_engine.camera, WaterHeightAt, AddWaterImpulse);
        }
        else g_play.Update(g_engine.camera, dt, PlayGround);
    }
    tbd::shell::GameplayTick(g_play.active ? dt : 0.0f, g_play.active); // the hot-reloaded gameplay DLL (Windows)
    g_script.PollReload(); // saving a .luau file swaps its code in, in or out of Play (self kept); about 4 checks a second
    if (g_play.active) g_script.Update(dt);
    if (g_play.active && g_npcs.Active() && g_engine.jobs) {
        tbd::ai::NpcFrame nf;
        nf.scene = &g_engine.scene;
        nf.animator = &g_engine.animator;
        nf.physics = &g_physics;
        nf.jobs = g_engine.jobs.get();
        nf.hasPlayer = g_player.Placed();
        nf.focus = nf.hasPlayer ? g_player.State().feet : g_engine.camera.target;
        nf.dt = dt;
        nf.hour = g_engine.sun.hour;
        if (!g_play.player) g_physics.Update(nf.focus, dt); // the player's update does this otherwise
        g_npcs.Update(nf);
        static const float overview = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_NAVDEBUG_CAM", v, sizeof(v)) > 0 ? (float)atof(v) : 0.0f; }();
        if (overview > 0.0f) { g_engine.camera.distance = overview; g_engine.camera.pitch = 1.25f; } // TBD_NAVDEBUG_CAM=<meters>: look down from above for captures
    }
    tbd::audio::UpdateFrame(g_engine.camera.Eye(), g_engine.camera.Forward(), g_engine.camera.Up(), g_play.active,
                            g_play.active && g_play.walking, tbd::kPlayEyeHeight); // no-op unless audio.h's Start succeeded
    UpdateFly(dt);
    UpdateCityCam(dt);
    CityCamGroundFloor();
    tbd::shell::EditorTick(dt); // editor panels: face tab, Character Creator, the scene's undo checkpoint
    if (g_engine.sun.animate) g_engine.sun.Advance(dt);
    // The sun's shadow map follows the camera's focus; it covers more ground the farther the camera is.
    tbd::Lighting light = tbd::ComputeLighting(g_engine.sun);
    static const float lightDebug = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_LIGHTDEBUG", v, sizeof(v)) > 0 ? (float)atof(v) : 0.0f; }();
    static uint32_t lightFrame = 0;
    light.lightDebug[1] = (float)(lightFrame++ & 1023); // the shadow filter's noise moves each frame; mesh shaders only, the post block overwrites it
    // TBD_FEATURECOST=<file>: with a TBD_SHOT, turns features off one at a time and writes the GPU's scene pass and whole frame for each, then quits.
    // A real feature comparison on one loaded scene, on either API. Bits: FEATURE_OFF in the shaders, and the grid and the grass below.
    static wchar_t costPath[tbd::platform::kMaxPath] = {};
    static const bool costOn = tbd::platform::GetEnvW(L"TBD_FEATURECOST", costPath, tbd::platform::kMaxPath) > 0;
    static int featureOff = 0;
    if (costOn) {
        struct Step { const char* name; int bits; };
        static const Step steps[] = {{"all on", 0}, {"pom off", 1}, {"pcss off", 2}, {"surface relief off", 4}, {"weathering off", 8}, {"probes off", 16}, {"cloud sun off", 32}, {"ground layers off", 64},
            {"tight map off", 128}, {"near grid off", 512}, {"grass off", 1024}, {"flat shading", 256}, {"all on again", 0}};
        static int step = 0, seen = 0;
        static double scene = 0, frame = 0;
        static FILE* out = tbd::platform::WOpen(costPath, L"w");
        if (!SceneLoadActive() && ground_bake::Settled() && g_stream.CaptureMayStart() && out) {
            constexpr int kSettle = 45, kCount = 120;
            if (seen >= kSettle) { const double* gpu = r.GpuPasses(); double all = 0; for (int k = 0; k < 8; ++k) all += gpu[k]; scene += gpu[1]; frame += all; }
            if (++seen == kSettle + kCount) {
                fprintf(out, "%-20s scene %6.2f frame %6.2f\n", steps[step].name, scene / kCount, frame / kCount);
                fflush(out);
                seen = 0; scene = frame = 0;
                if (++step == (int)std::size(steps)) { fclose(out); out = nullptr; tbd::shell::Quit(); step = 0; }
            }
            featureOff = steps[step].bits;
        }
    }
    const int qualityOff = g_gfxQuality > 0 ? (1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 512 | 1024 | (g_gfxQuality > 1 ? 256 : 0)) : 0;
    const int presetOff = LookTier() == 0 ? 2 : 0; // Low: one hardware-filtered shadow tap instead of PCSS's fifteen
    light.lightDebug[2] = (float)(featureOff | qualityOff | presetOff); // the spare slot; mesh shaders only
    // The city map's key light (docs/cs2_look_pass.md): CS2's sunlit and shaded walls differ about 15:1, ours were near 3:1, so the sky
    // and probe fill drops there. Real meshes and the baked far volumes both light through LitPbr, so they move together.
    const bool cityLight = LookOn() && !(g_engine.sun.showSky && g_gfxQuality == 0) && g_gfxQuality == 0 && g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind == 2;
    light.lightDebug[3] = (cityLight ? g_engine.scene.ambientFill * 0.55f : g_engine.scene.ambientFill) + 1.0f; // +1: an unset 0 reads as the default (lighting.hlsli AmbientFill)
    g_sceneExposure = g_engine.scene.exposure; // both renderers pass it to post.hlsl ExposureValue
    light.lightDebug[0] = lightDebug; // 1 indirect diffuse, 2 sun, 3 reflections, 4 haze only, 8 surface light (vs tbd_reference); see lighting.hlsli and post.hlsl
    const tbd::Vec3 shadowFocus = g_engine.camera.target;
    static const bool probesOn = tbd::platform::GetEnvA("TBD_NOPROBES", nullptr, 0) == 0;
    tbd::ComputeProbeGrid(shadowFocus, probesOn && g_gfxQuality == 0, light);
    static const float envWet = [] { char v[16] = {}; return tbd::platform::GetEnvA("TBD_WETNESS", v, sizeof(v)) > 0 ? (float)atof(v) : -1.0f; }();
    if (envWet >= 0.0f) g_engine.sun.wetness = envWet;
    light.probePrev[3] = g_engine.sun.wetness; // the spare slot: mesh.hlsl reads it as gLight.probePrev.w
    static const bool cloudShadowOn = tbd::platform::GetEnvA("TBD_NOCLOUDSHADOW", nullptr, 0) == 0;
    // TBD_LIGHTDEBUG=8 compares with tbd_reference (gameenginetools\lighting_reference), which has no clouds.
    tbd::ComputeCloudMap(shadowFocus, cloudShadowOn && g_gfxQuality == 0 && g_engine.sun.showSky && light.sunDir[1] > 0.0f && lightDebug != 8.0f, light);
    // At MAX the map closes in on what the camera is looking at: 20 m across a 4096 map is a centimetre per texel, which saws a face's shadows.
    const float shadowRadius = g_maxGraphics ? std::clamp(g_engine.camera.distance * 2.0f + 1.0f, 1.5f, 120.0f)
                                             : std::clamp(g_engine.camera.distance * 1.5f + 15.0f, 60.0f, 120.0f); // never less: a close camera must still see the far side of the street shadowed
    const bool shadows = g_engine.sun.shadows && g_gfxQuality < 2 && tbd::ComputeShadow(g_engine.sun, shadowFocus, shadowRadius, light);
    r.SetLighting(light);

    const float aspect = (float)g_engine.vpW / (float)std::max(1, g_engine.vpH);
    const tbd::Mat4 view = g_engine.camera.View();
    const tbd::Mat4 viewProj = g_engine.camera.Projection(aspect) * view;
    g_lastViewProj = viewProj;
    const tbd::Vec3 eye = g_engine.camera.Eye();
    // The scene is drawn sub-pixel jittered (Halton 2,3) for temporal anti-aliasing; overlays use the steady matrix.
    tbd::Mat4 sceneViewProj = viewProj;
    float waterJitter[2] = {0.0f, 0.0f};
    {
        static uint32_t jitterIndex = 0;
        auto halton = [](uint32_t i, uint32_t base) {
            float f = 1.0f, out = 0.0f;
            for (; i > 0; i /= base) { f /= (float)base; out += f * (float)(i % base); }
            return out;
        };
        jitterIndex = jitterIndex % 8 + 1;
        tbd::Mat4 jitter = tbd::Mat4::Identity();
        jitter.At(0, 3) = (halton(jitterIndex, 2) - 0.5f) * 2.0f / (float)tbd::ScaledSize(g_engine.vpW, RenderScale()); // one pixel of the render size
        jitter.At(1, 3) = (halton(jitterIndex, 3) - 0.5f) * 2.0f / (float)tbd::ScaledSize(g_engine.vpH, RenderScale());
        sceneViewProj = jitter * viewProj;
        waterJitter[0] = jitter.At(0, 3);
        waterJitter[1] = jitter.At(1, 3);
        static float cloudTime = 0.0f;
        // TBD_CLOUDTIME=<s>: clouds held at that time, for comparing captures.
        static const float fixedCloudTime = [] { char v[32] = {}; return tbd::platform::GetEnvA("TBD_CLOUDTIME", v, sizeof(v)) > 0 ? (float)atof(v) : -1.0f; }();
        // Clouds keep the day clock's pace: a game hour blows them about 0.6 km, whatever the day length. True time-lapse
        // speed would flicker at short days. With the clock stopped, or at long days, they drift at the real wind.
        const float cloudRate = g_engine.sun.animate ? std::max(1.0f, 45.0f * 24.0f / g_engine.sun.dayLengthSeconds) : 1.0f;
        cloudTime = fixedCloudTime >= 0.0f ? fixedCloudTime : cloudTime + dt * cloudRate;
        tbd::FrameView frameView;
        frameView.viewProj = viewProj;
        frameView.invJittered = tbd::Inverse(sceneViewProj);
        frameView.eye = eye;
        frameView.time = cloudTime;
        frameView.sky = g_engine.sun.showSky && g_gfxQuality == 0;
        // The city map: no marched sky, its cheap air instead (post.hlsl CityComposite; docs/cs2_look_pass.md).
        frameView.cityAir = LookOn() && !frameView.sky && g_gfxQuality == 0 && g_engine.terrain.params.enabled && g_engine.terrain.params.mapKind == 2;
        frameView.postTier = LookTier();
        frameView.dt = dt;
        static const bool motionOff = [] { char b[8] = {}; return tbd::platform::GetEnvA("TBD_TAAMV", b, 8) > 0 && b[0] == '0'; }(); // test: the old depth-only reprojection
        frameView.motionVectors = !motionOff;
        r.SetFrameView(frameView);
    }
    light.eye[0] = eye.x; light.eye[1] = eye.y; light.eye[2] = eye.z;
    r.SetLighting(light);

    FrameMark("setup");
    ApplySkinDetailScales(); // a few map writes; see SetEntitySkinDetailScale
    if (g_engine.drag == Drag::Terrain && g_terrainEdit.on && g_engine.terrain.params.enabled) { // the brush, once per frame so speed does not depend on mouse movement
        tbd::Vec3 hit;
        const tbd::Ray ray = g_engine.camera.ScreenRay((float)g_engine.lastX, (float)g_engine.lastY, (float)g_engine.vpW, (float)g_engine.vpH);
        if (GroundHit(ray, hit)) {
            const int mode = TerrainBrushModeNow(); // the toolbar's mode; Ctrl / Shift override it
            if (mode == (int)tbd::gui::TerrainBrushMode::Flatten) g_engine.terrain.Brush(hit.x, hit.z, g_terrainEdit.radius, std::min(1.0f, 3.0f * dt), tbd::HeightField::BrushMode::Flatten, g_terrainEdit.flattenTarget);
            else g_engine.terrain.Brush(hit.x, hit.z, g_terrainEdit.radius, (mode == (int)tbd::gui::TerrainBrushMode::Dig ? -1.0f : 1.0f) * g_terrainEdit.metresPerSecond * dt);
        }
    }
    { // TBD_BRUSHSTREAM="x z radius metres": one stroke every frame, sweeping 400 m east and back, to time a brush drag without a mouse
        static float sx, sz, sr, sm;
        static const bool stream = [] { char v[96] = {}; return tbd::platform::GetEnvA("TBD_BRUSHSTREAM", v, sizeof(v)) > 0 && sscanf(v, "%f %f %f %f", &sx, &sz, &sr, &sm) == 4; }();
        static int streamFrame = 0;
        if (stream && g_engine.terrain.params.enabled) {
            const float phase = (float)(streamFrame++ % 240) / 240.0f, sweep = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
            g_engine.terrain.Brush(sx + sweep * 400.0f, sz, sr, sm * dt);
        }
    }
    g_engine.terrain.Update(r, eye);
    if (g_engine.terrain.params.enabled) g_engine.terrain.UpdateFar(r, eye);
    FrameMark("terrain");
    g_stream.Update(r, eye);
    FrameMark("streaming");
    // A capture's frames start counting only when everything near the camera is loaded, so captures stay deterministic.
    { // TBD_GENLODS=1: World > Generate LODs once the first streaming pass is idle, for an unattended test of the command
        static bool genLods = tbd::platform::GetEnvA("TBD_GENLODS", nullptr, 0) > 0;
        if (genLods && !SceneLoadActive() && (!g_stream.Active() || g_stream.IsIdle())) {
            genLods = false;
            GenerateOutdoorLods();
            if (captureFramesLeft > 0) captureFramesLeft = benchFrames + 61;
        }
    }
    if (captureFramesLeft > 0 && (SceneLoadActive() || !g_stream.CaptureMayStart() || !shotTour.Done() || !ground_bake::Settled() || tbd::shell::EditorBusy())) captureFramesLeft = benchFrames + 61 + settleFrames;

    const float clear[4] = {24.0f / 255.0f, 20.0f / 255.0f, 16.0f / 255.0f, 1.0f}; // matches kClearColor
    // World and model-view-projection matrices of every entity, into preallocated slots (no locks: each index is
    // written by exactly one batch). Drawing below stays serial and in entity order.
    const size_t entityCount = std::min(g_engine.scene.entities.size(), g_engine.meshes.size());
    g_engine.models.resize(entityCount);
    g_engine.mvps.resize(entityCount);
    auto computeMatrices = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            g_engine.models[i] = g_engine.scene.entities[i].Model();
            g_engine.mvps[i] = sceneViewProj * g_engine.models[i];
        }
    };
    if (entityCount > 64 && g_engine.jobs) g_engine.jobs->ParallelFor(entityCount, 32, computeMatrices);
    else computeMatrices(0, entityCount);

    // The tight character shadow: a small, sharp map around the camera's focus (faces, clothes) that only entities within
    // 2.5 m of the focus draw into. An entity counts when its bounding sphere (its pickable box's circumsphere) touches the
    // focus sphere. With no such entity, or with shadows off or the sun down, the shaders use the sun's map alone.
    // Close up the map shrinks with the camera's distance: at 2.5 m a texel is 2.4 mm, which tears the shadow lines on a face.
    // Casters are still chosen within 2.5 m: an entity's origin can be far from the part of it in view (a head's, at the hips).
    constexpr float kCharacterShadowRadius = 2.5f;
    const float tightMapRadius = std::clamp(g_engine.camera.distance * 0.9f, 0.5f, kCharacterShadowRadius);
    static std::vector<size_t> tightCasters; // reused, so the frame doesn't allocate
    tightCasters.clear();
    if (shadows) {
        for (size_t i = 0; i < entityCount; ++i) {
            const tbd::Entity& e = g_engine.scene.entities[i];
            const tbd::Vec3 half = tbd::LocalHalfExtents(e);
            const tbd::Vec3 reach = {half.x * std::fabs(e.scale.x), half.y * std::fabs(e.scale.y), half.z * std::fabs(e.scale.z)};
            const tbd::Vec3 away = e.position - shadowFocus;
            const float touch = kCharacterShadowRadius + std::sqrt(tbd::Dot(reach, reach));
            if (tbd::Dot(away, away) <= touch * touch) tightCasters.push_back(i);
        }
        if (!tightCasters.empty() && !tbd::ComputeCharacterShadow(g_engine.sun, shadowFocus, tightMapRadius, light))
            tightCasters.clear();
    }
    // The lit capture the probe rays read: taken again only when the focus has moved well inside it, the hour changed, what is loaded changed,
    // or its texture is new; a second take a moment after the first holds the probes' light too.
    static tbd::Mat4 captureVP = tbd::Mat4::Identity();
    static tbd::Vec3 captureFocus = {1e9f, 0.0f, 0.0f};
    static float captureHour = -100.0f;
    static uint32_t captureEpoch = 0xFFFFFFFFu, captureLoaded = 0, captureAge = 0;
    static bool captureValid = false;
    constexpr float kCaptureRadius = 120.0f;
    bool takeCapture = false;
    if (shadows && !g_engine.sun.animate) {
        uint32_t loaded = 0;
        for (size_t i = 0; i < entityCount; ++i) loaded += g_engine.meshes[i] != tbd::kInvalidMesh ? 1u : 0u;
        const tbd::Vec3 moved = shadowFocus - captureFocus;
        ++captureAge;
        takeCapture = tbd::Dot(moved, moved) > 900.0f || captureHour != g_engine.sun.hour || captureEpoch != r.CaptureEpoch() || captureLoaded != loaded || captureAge == 90;
        if (takeCapture) {
            tbd::Lighting fit;
            tbd::ComputeShadow(g_engine.sun, shadowFocus, kCaptureRadius, fit);
            tbd::Mat4 flip = tbd::Mat4::Identity(); // z' = w - z: the scene's pipelines want reversed Z
            flip.At(2, 2) = -1.0f; flip.At(2, 3) = 1.0f;
            std::copy(fit.shadowVP, fit.shadowVP + 16, captureVP.m);
            captureVP = flip * captureVP;
            if (tbd::Dot(moved, moved) > 900.0f || captureHour != g_engine.sun.hour || captureEpoch != r.CaptureEpoch()) captureAge = 0;
            captureFocus = shadowFocus; captureHour = g_engine.sun.hour; captureEpoch = r.CaptureEpoch(); captureLoaded = loaded;
            captureValid = true;
        }
    } else if (!shadows) captureValid = false;
    if (captureValid) std::copy(captureVP.m, captureVP.m + 16, light.captureVP);
    { // the glass panes nearest the camera's focus, which the probes treat as the openings of a room
        std::vector<std::pair<float, tbd::WindowPortal>> panes;
        for (size_t i = 0; i < entityCount; ++i) {
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) continue;
            const tbd::Mat4& m = g_engine.models[i];
            const auto dir = [&](const float* a) { return tbd::Vec3{m.At(0, 0) * a[0] + m.At(0, 1) * a[1] + m.At(0, 2) * a[2], m.At(1, 0) * a[0] + m.At(1, 1) * a[1] + m.At(1, 2) * a[2], m.At(2, 0) * a[0] + m.At(2, 1) * a[1] + m.At(2, 2) * a[2]}; };
            for (const MatPart& part : it->second)
                for (const tbd::WindowPortal& local : part.portals) {
                    const tbd::Vec3 c = dir(local.center) + tbd::Vec3{m.At(0, 3), m.At(1, 3), m.At(2, 3)}, u = dir(local.u), v = dir(local.v);
                    const tbd::Vec3 off = c - shadowFocus;
                    const float d2 = tbd::Dot(off, off);
                    if (d2 > 80.0f * 80.0f) continue;
                    panes.push_back({d2, {{c.x, c.y, c.z}, {u.x, u.y, u.z}, {v.x, v.y, v.z}}});
                }
        }
        std::sort(panes.begin(), panes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<tbd::WindowPortal> nearest;
        for (size_t k = 0; k < panes.size() && k < tbd::kMaxPortals; ++k) nearest.push_back(panes[k].second);
        r.SetWindowPortals(nearest.data(), (uint32_t)nearest.size());
    }
    { // the building interiors nearest the focus, whose probes trace their own walls
        std::vector<std::pair<float, tbd::InteriorRoom>> rooms;
        for (size_t i = 0; i < entityCount; ++i) {
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) continue;
            const tbd::Mat4& m = g_engine.models[i];
            const auto dir = [&](float x, float y, float z) { return tbd::Vec3{m.At(0, 0) * x + m.At(0, 1) * y + m.At(0, 2) * z, m.At(1, 0) * x + m.At(1, 1) * y + m.At(1, 2) * z, m.At(2, 0) * x + m.At(2, 1) * y + m.At(2, 2) * z}; };
            for (const MatPart& part : it->second)
                for (const tbd::InteriorRoom& local : part.rooms) {
                    const tbd::Vec3 c = dir(local.center[0], local.center[1], local.center[2]) + tbd::Vec3{m.At(0, 3), m.At(1, 3), m.At(2, 3)};
                    const tbd::Vec3 x = dir(local.x[0], local.x[1], local.x[2]), z = dir(local.z[0], local.z[1], local.z[2]), up = dir(0, local.halfHeight, 0);
                    const tbd::Vec3 off = c - shadowFocus;
                    const float d2 = tbd::Dot(off, off);
                    if (d2 > 80.0f * 80.0f) continue;
                    rooms.push_back({d2, {{c.x, c.y, c.z}, std::sqrt(tbd::Dot(up, up)), {x.x, x.y, x.z}, {z.x, z.y, z.z}}});
                }
        }
        std::sort(rooms.begin(), rooms.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<tbd::InteriorRoom> nearest;
        for (size_t k = 0; k < rooms.size() && k < tbd::kMaxRooms; ++k) nearest.push_back(rooms[k].second);
        static const bool roomsOn = tbd::platform::GetEnvA("TBD_NOROOMS", nullptr, 0) == 0;
        r.SetInteriorRooms(nearest.data(), roomsOn ? (uint32_t)nearest.size() : 0u);
    }
    if (r.RayTracingSupported() && g_gfxQuality == 0) { // what the ray traced occlusion can hit: the static parts near the camera, glass and hair left out
        constexpr float kRayReach = 60.0f;
        static std::vector<tbd::RayInstance> rays;
        rays.clear();
        const tbd::Vec3 eye = g_engine.camera.Eye();
        const float rayPixelScale = (float)g_engine.vpH / (2.0f * std::tan(g_engine.camera.fovY * 0.5f));
        for (size_t i = 0; i < entityCount; ++i) {
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) continue;
            const tbd::Mat4& m = g_engine.models[i];
            for (const MatPart& part : it->second) {
                if (part.mesh == tbd::kInvalidMesh || part.hair || !part.portals.empty()) continue;
                const tbd::Sphere world = tbd::TransformSphere(part.bounds, m);
                const tbd::Vec3 away = world.center - eye;
                const float d = std::sqrt(tbd::Dot(away, away));
                if (d - world.radius > kRayReach) continue;
                if (part.lodMax > 0.0f && (d < part.lodMin || d >= part.lodMax)) continue; // the terrain set drawn here
                if (!LodShows(part, world, eye, rayPixelScale)) continue;
                tbd::RayInstance ri{part.mesh, {}};
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col) ri.rows[row * 4 + col] = m.At(row, col);
                rays.push_back(ri);
            }
        }
        r.SetRayScene(rays.data(), (uint32_t)rays.size());
    }
    r.SetLighting(light); // takes effect at BeginFrame, now with the tight map's matrix (or without it)

    // Visibility: each batch writes only its own entities' and assets' slots, so no locks.
    int64_t cullStart, cullEnd;
    cullStart = tbd::platform::Ticks();
    const auto matrixOf = [](const float* m) { tbd::Mat4 out; std::copy(m, m + 16, out.m); return out; };
    const tbd::ViewVolume cameraVolume = tbd::ViewVolume::FromMatrix(viewProj, false);
    const tbd::ViewVolume sunVolume = tbd::ViewVolume::FromMatrix(matrixOf(light.shadowVP), true);
    const tbd::ViewVolume tightVolume = tbd::ViewVolume::FromMatrix(matrixOf(light.tightVP), true);
    // Pixels per meter of radius at one meter. A thing whose bounding sphere is under TBD_CULLPX pixels of radius on screen
    // (default 4, about 8 pixels across) is not drawn, shadow included. Size against distance: on a 1080p window a pool ball
    // (2.8 cm) goes at about 6 m, a barrel at about 100 m, a house at about 2 km. The editor's camera and the game's are this one.
    const float pixelScale = (float)g_engine.vpH / (2.0f * std::tan(g_engine.camera.fovY * 0.5f));
    static const float kMinPixels = EnvFloat("TBD_CULLPX", 4.0f, 0.0f, 64.0f);
    const auto cameraSees = [&](const tbd::Sphere& s) {
        if (s.radius < 0.0f) return true;
        if (!cameraVolume.Touches(s)) return false;
        const tbd::Vec3 away = s.center - eye;
        return s.radius * pixelScale >= kMinPixels * std::sqrt(tbd::Dot(away, away));
    };
    g_engine.partOffsets.resize(entityCount);
    uint32_t partTotal = 0;
    for (size_t i = 0; i < entityCount; ++i) {
        g_engine.partOffsets[i] = partTotal;
        const auto it = g_engine.matParts.find(g_engine.meshes[i]);
        if (it != g_engine.matParts.end()) partTotal += (uint32_t)it->second.size();
    }
    g_engine.entityVisible.resize(entityCount);
    g_engine.partVisible.resize(partTotal);
    // Interiors: a building's furniture, its own interior parts and the prop entities standing in its rooms, draw only while the
    // building's shell is at its full level. The shell's first switch is the coarsest-reaching full level's next error; with no
    // chain yet, the shell counts as near while it is over ~100 pixels in radius. Which room holds which prop is redone every
    // 30 frames (things stream in and out); doors stay, so a shell never shows a hole.
    const auto meshSphere = [](tbd::MeshId id) { return id < g_engine.meshBounds.size() ? g_engine.meshBounds[id] : tbd::Sphere{}; };
    static std::vector<int> interiorOwner;
    static std::vector<uint64_t> roomAllowed; // per entity: the rooms of its building the camera can see into; 0: no restriction
    static uint32_t ownerFrame = 0;
    if (interiorOwner.size() != entityCount || ++ownerFrame % 30 == 0) {
        interiorOwner.assign(entityCount, -1);
        struct Room { tbd::InteriorRoom room; int owner; };
        std::vector<Room> worldRooms;
        for (size_t i = 0; i < entityCount; ++i) {
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) continue;
            const tbd::Mat4& m = g_engine.models[i];
            const auto dir = [&](const float* a) { return tbd::Vec3{m.At(0, 0) * a[0] + m.At(0, 1) * a[1] + m.At(0, 2) * a[2], m.At(1, 0) * a[0] + m.At(1, 1) * a[1] + m.At(1, 2) * a[2], m.At(2, 0) * a[0] + m.At(2, 1) * a[1] + m.At(2, 2) * a[2]}; };
            const float up = std::sqrt(m.At(0, 1) * m.At(0, 1) + m.At(1, 1) * m.At(1, 1) + m.At(2, 1) * m.At(2, 1));
            for (const MatPart& part : it->second)
                for (const tbd::InteriorRoom& r : part.rooms) {
                    const tbd::Vec3 c = dir(r.center) + tbd::Vec3{m.At(0, 3), m.At(1, 3), m.At(2, 3)}, x = dir(r.x), z = dir(r.z);
                    worldRooms.push_back({{{c.x, c.y, c.z}, r.halfHeight * up, {x.x, x.y, x.z}, {z.x, z.y, z.z}}, (int)i});
                }
        }
        for (size_t i = 0; i < entityCount && !worldRooms.empty(); ++i) {
            if (g_engine.meshes[i] == tbd::kInvalidMesh || i >= g_engine.scene.entities.size()) continue;
            const tbd::Entity& e = g_engine.scene.entities[i];
            if (e.mesh != tbd::MeshKind::Imported || !e.animClip.empty() || e.meshPath.find("\\Doors\\") != std::string::npos) continue;
            const tbd::Sphere world = tbd::TransformSphere(meshSphere(g_engine.meshes[i]), g_engine.models[i]);
            for (const Room& r : worldRooms)
                if (r.owner != (int)i && world.radius < r.room.halfHeight * 2.0f && InsideRoom(r.room, world.center)) { interiorOwner[i] = r.owner; break; }
        }
    }
    if (ownerFrame % 600 == 0) {
        size_t props = 0, parts = 0, rooms = 0;
        for (int o : interiorOwner) props += o >= 0 ? 1 : 0;
        for (const auto& [mesh, list] : g_engine.matParts)
            for (const MatPart& p : list) { parts += p.interior ? 1 : 0; rooms += p.rooms.size(); }
        size_t masked = 0, restricted = 0;
        for (const auto& [mesh, list] : g_engine.matParts)
            for (const MatPart& p : list) masked += p.roomMask ? 1 : 0;
        for (uint64_t a : roomAllowed) restricted += a ? 1 : 0;
        tbd::Log("Interiors: %zu rooms, %zu props in them, %zu interior parts in building meshes (%zu tied to rooms; %zu buildings restricted to the camera's room)", rooms, props, parts, masked, restricted);
    }
    static std::vector<uint8_t> shellNear;
    shellNear.assign(entityCount, 1);
    for (size_t i = 0; i < entityCount; ++i) {
        const auto it = g_engine.matParts.find(g_engine.meshes[i]);
        if (it == g_engine.matParts.end() || std::none_of(it->second.begin(), it->second.end(), [](const MatPart& p) { return p.interior || !p.rooms.empty(); })) continue;
        const tbd::Sphere local = meshSphere(g_engine.meshes[i]);
        float shell = 1e30f;
        for (const MatPart& p : it->second)
            if (p.lodChain && p.lodError == 0.0f && p.lodCoarser > 0.0f && !p.interior) shell = std::min(shell, p.lodCoarser);
        if (shell > 1e29f) shell = std::max(local.radius, 0.0f) * 0.01f;
        const tbd::Sphere world = tbd::TransformSphere(local, g_engine.models[i]);
        const tbd::Vec3 away = world.center - eye;
        const float d = std::max(std::sqrt(tbd::Dot(away, away)) - world.radius, 0.1f);
        const float scale = local.radius > 0.0f ? world.radius / local.radius : 1.0f;
        shellNear[i] = shell * scale * pixelScale / d > LodPixels() ? 1 : 0;
    }
    // Per-room culling: with the camera inside one of a building's rooms, its interior parts draw only for that room and the rooms
    // touching it (through doors and windows); everything else in the building is behind walls. 0: no restriction (camera outside
    // every room, or the building has none). TBD_ROOMCULL=0 turns it off.
    static const bool roomCull = [] { char f[4] = {}; return tbd::platform::GetEnvA("TBD_ROOMCULL", f, sizeof(f)) == 0 || f[0] != '0'; }();
    roomAllowed.assign(entityCount, 0);
    for (size_t i = 0; roomCull && i < entityCount; ++i) {
        const auto it = g_engine.matParts.find(g_engine.meshes[i]);
        if (it == g_engine.matParts.end()) continue;
        const MatPart* holder = nullptr;
        for (const MatPart& p : it->second) if (!p.rooms.empty()) { holder = &p; break; }
        if (!holder || holder->rooms.size() > 64) continue;
        const tbd::Sphere shell = tbd::TransformSphere(meshSphere(g_engine.meshes[i]), g_engine.models[i]);
        const tbd::Vec3 toEye = eye - shell.center;
        if (tbd::Dot(toEye, toEye) > shell.radius * shell.radius) continue;
        const tbd::Mat4& m = g_engine.models[i];
        const auto dir = [&](const float* a) { return tbd::Vec3{m.At(0, 0) * a[0] + m.At(0, 1) * a[1] + m.At(0, 2) * a[2], m.At(1, 0) * a[0] + m.At(1, 1) * a[1] + m.At(1, 2) * a[2], m.At(2, 0) * a[0] + m.At(2, 1) * a[1] + m.At(2, 2) * a[2]}; };
        const float up = std::sqrt(m.At(0, 1) * m.At(0, 1) + m.At(1, 1) * m.At(1, 1) + m.At(2, 1) * m.At(2, 1));
        std::vector<tbd::InteriorRoom> world;
        for (const tbd::InteriorRoom& r : holder->rooms) {
            const tbd::Vec3 c = dir(r.center) + tbd::Vec3{m.At(0, 3), m.At(1, 3), m.At(2, 3)}, x = dir(r.x), z = dir(r.z);
            world.push_back({{c.x, c.y, c.z}, r.halfHeight * up, {x.x, x.y, x.z}, {z.x, z.y, z.z}});
        }
        uint64_t inside = 0;
        for (size_t a = 0; a < world.size(); ++a) if (InsideRoom(world[a], eye)) inside |= 1ull << a;
        if (!inside) continue;
        // Touching: any of b's sample points (centre, corners, edge midpoints, at three heights) within a wall's thickness of a, or a's of b.
        const auto samples = [](const tbd::InteriorRoom& r) {
            std::vector<tbd::Vec3> pts;
            const tbd::Vec3 c = {r.center[0], r.center[1], r.center[2]}, x = {r.x[0], r.x[1], r.x[2]}, z = {r.z[0], r.z[1], r.z[2]};
            for (int h = -1; h <= 1; ++h)
                for (int sx = -1; sx <= 1; ++sx)
                    for (int sz = -1; sz <= 1; ++sz)
                        pts.push_back(c + x * (float)sx + z * (float)sz + tbd::Vec3{0.0f, r.halfHeight * (float)h, 0.0f});
            return pts;
        };
        constexpr float kWall = 0.9f; // meters of wall, floor or ceiling between neighbours
        uint64_t allowed = inside;
        for (size_t a = 0; a < world.size(); ++a) {
            if (!(inside & (1ull << a))) continue;
            const std::vector<tbd::Vec3> ptsA = samples(world[a]);
            for (size_t b = 0; b < world.size(); ++b) {
                if (allowed & (1ull << b)) continue;
                bool touch = false;
                for (const tbd::Vec3& p : samples(world[b])) if (InsideRoom(world[a], p, -kWall)) { touch = true; break; }
                for (size_t q = 0; !touch && q < ptsA.size(); ++q) touch = InsideRoom(world[b], ptsA[q], -kWall);
                if (touch) allowed |= 1ull << b;
            }
        }
        roomAllowed[i] = allowed;
    }
    const auto boundsOf = [](tbd::MeshId id) { return id < g_engine.meshBounds.size() ? g_engine.meshBounds[id] : tbd::Sphere{}; };
    // TBD_NOCULL=1: draw everything, to compare a capture against the culled one.
    static const bool noCull = tbd::platform::GetEnvA("TBD_NOCULL", nullptr, 0) > 0;
    const auto visibleTo = [&](const tbd::Sphere& world) {
        if (noCull) return (uint8_t)(kVisibleCamera | (shadows ? kVisibleSun : 0) | (tightCasters.empty() ? 0 : kVisibleTight));
        uint8_t flags = 0;
        if (cameraSees(world)) flags |= kVisibleCamera;
        if (shadows && sunVolume.Touches(world)) flags |= kVisibleSun;
        if (!tightCasters.empty() && tightVolume.Touches(world)) flags |= kVisibleTight;
        return flags;
    };
    const auto cullEntities = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (g_engine.meshes[i] == tbd::kInvalidMesh || g_water.IsSurface(i)) { g_engine.entityVisible[i] = 0; continue; } // streamed out, or drawn by the water pass
            const bool hiddenInside = !noCull && interiorOwner[i] >= 0 && !shellNear[(size_t)interiorOwner[i]];
            const uint8_t flags = hiddenInside ? 0 : visibleTo(tbd::TransformSphere(boundsOf(g_engine.meshes[i]), g_engine.models[i]));
            g_engine.entityVisible[i] = flags;
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) continue;
            const std::vector<MatPart>& parts = it->second;
            for (size_t k = 0; k < parts.size(); ++k) {
                const tbd::Sphere partWorld = tbd::TransformSphere(parts[k].bounds, g_engine.models[i]);
                uint8_t partFlags = flags ? (visibleTo(partWorld) & flags) : 0;
                if (parts[k].lodMax > 0.0f) { // one of a terrain tile's sets, chosen by distance
                    const tbd::Vec3 away = partWorld.center - eye;
                    const float d2 = tbd::Dot(away, away);
                    if (d2 < parts[k].lodMin * parts[k].lodMin || d2 >= parts[k].lodMax * parts[k].lodMax) partFlags &= (uint8_t)~kVisibleCamera;
                }
                if (parts[k].lodMax > 0.0f && parts[k].hair && !(partFlags & kVisibleCamera)) partFlags = 0; // another hair level casts
                // terrain's other sets; hair cards cast only into the tight character map, through their alpha (DrawShadowCasterCutout)
                if (!parts[k].caster) partFlags &= (uint8_t)~(parts[k].hair ? kVisibleSun : (kVisibleSun | kVisibleTight));
                if (parts[k].hair && tbd::shell::EditorHidesHeadHair()) partFlags = 0; // Hair Maker: the clay is being worked
                if (!noCull && !LodShows(parts[k], partWorld, eye, pixelScale)) partFlags = 0; // another level of its chain draws, shadows included
                if (!noCull && parts[k].interior && !shellNear[i]) partFlags = 0; // the shell has gone coarse: nobody sees in
                if (!noCull && parts[k].interior && roomAllowed[i] && parts[k].roomMask && !(parts[k].roomMask & roomAllowed[i])) partFlags = 0; // another room, behind walls
                g_engine.partVisible[g_engine.partOffsets[i] + k] = partFlags;
            }
        }
    };
    if (entityCount > 64 && g_engine.jobs) g_engine.jobs->ParallelFor(entityCount, 16, cullEntities);
    else cullEntities(0, entityCount);

    const auto cullInstances = [&](size_t begin, size_t end) {
        for (size_t a = begin; a < end; ++a) {
            InstancedAsset& asset = g_engine.instanced[a];
            asset.visible.clear();
            asset.casters.clear();
            for (const tbd::InstanceCell& cell : asset.cells) {
                const tbd::Vec3 fromEye = cell.bounds.center - eye;
                const float distance = std::max(0.0f, std::sqrt(tbd::Dot(fromEye, fromEye)) - cell.bounds.radius);
                if (distance <= asset.maxDistance && cameraVolume.Touches(cell.bounds)) {
                    // The cell's instances are shuffled, so its first ones are an even, thinner version of it far away.
                    const float fade = std::clamp((distance / asset.maxDistance - 0.4f) / 0.6f, 0.0f, 1.0f);
                    const uint32_t count = std::max(1u, (uint32_t)std::ceil((float)cell.count * (1.0f - 0.8f * fade)));
                    if (!asset.visible.empty() && asset.visible.back().first + asset.visible.back().count == cell.first) asset.visible.back().count += count;
                    else asset.visible.push_back({cell.first, count});
                }
                if (shadows && asset.shadowDistance > 0.0f) {
                    const tbd::Vec3 fromFocus = cell.bounds.center - shadowFocus;
                    if (std::sqrt(tbd::Dot(fromFocus, fromFocus)) - cell.bounds.radius <= asset.shadowDistance && sunVolume.Touches(cell.bounds)) {
                        if (!asset.casters.empty() && asset.casters.back().first + asset.casters.back().count == cell.first) asset.casters.back().count += cell.count;
                        else asset.casters.push_back({cell.first, cell.count});
                    }
                }
            }
        }
    };
    if (g_engine.instanced.size() > 8 && g_engine.jobs) g_engine.jobs->ParallelFor(g_engine.instanced.size(), 4, cullInstances);
    else cullInstances(0, g_engine.instanced.size());

    // The scene's draws, near to far so the depth test saves shading, then by material within a distance step to save binds;
    // big pieces (terrain) last, since they cover what is behind everything else. Ties keep entity order, so every run agrees.
    g_engine.sceneDraws.clear();
    const auto addDraw = [&](size_t i, tbd::MeshId mesh, tbd::TextureId albedo, tbd::TextureId normal, tbd::TextureId surface, const tbd::Sphere& local, bool opaque = false) {
        const tbd::Sphere world = tbd::TransformSphere(local, g_engine.models[i]);
        const tbd::Vec3 away = world.center - eye;
        const bool big = world.radius > 60.0f;
        const uint32_t step = (uint32_t)std::min(std::sqrt(tbd::Dot(away, away)) / 16.0f, 65535.0f);
        g_engine.sceneDraws.push_back({mesh, albedo, normal, surface, (uint32_t)i, (big ? 0x10000u : 0u) | step, opaque && !g_engine.animator.Animates(g_engine.meshes[i])});
    };
    for (size_t i = 0; i < entityCount; ++i) {
        if (!(g_engine.entityVisible[i] & kVisibleCamera)) continue;
        const auto it = g_engine.matParts.find(g_engine.meshes[i]);
        if (it == g_engine.matParts.end()) {
            addDraw(i, g_engine.meshes[i], tbd::kNoTexture, tbd::kNoTexture, tbd::kNoTexture, boundsOf(g_engine.meshes[i]));
            continue;
        }
        const std::vector<MatPart>& parts = it->second;
        for (size_t k = 0; k < parts.size(); ++k)
            if (g_engine.partVisible[g_engine.partOffsets[i] + k] & kVisibleCamera)
                addDraw(i, parts[k].mesh, parts[k].albedo, parts[k].normal, parts[k].surface, parts[k].bounds, parts[k].opaque);
    }
    std::stable_sort(g_engine.sceneDraws.begin(), g_engine.sceneDraws.end(), [](const SceneDraw& a, const SceneDraw& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.albedo != b.albedo) return a.albedo < b.albedo;
        if (a.normal != b.normal) return a.normal < b.normal;
        return a.surface < b.surface;
    });
    cullEnd = tbd::platform::Ticks();
    FrameMark("cull");

    // TBD_WATER_TEST="x z radius speed extent": a sphere through the water on a fixed path, stepping 1/60 s a frame so captures reproduce.
    // TBD_WATER_TEST_FRAMES=n: at least that many test frames before a capture may start, to catch the ripples at another moment.
    static float waterTest[5] = {};
    static const bool waterTestOn = [] {
        char v[128] = {};
        return tbd::platform::GetEnvA("TBD_WATER_TEST", v, sizeof(v)) > 0 && sscanf(v, "%f %f %f %f %f", waterTest, waterTest + 1, waterTest + 2, waterTest + 3, waterTest + 4) == 5;
    }();
    static const int waterTestFrames = [] { char v[32] = {}; return tbd::platform::GetEnvA("TBD_WATER_TEST_FRAMES", v, sizeof(v)) > 0 ? atoi(v) : 0; }();
    static int waterTestFrame = 0;
    static tbd::MeshId waterTestMesh = tbd::kInvalidMesh;
    static const tbd::Renderer* waterTestOwner = nullptr;
    const float waterDt = waterTestOn ? 1.0f / 60.0f : dt;
    tbd::WaterCollider waterExtra[2];
    uint32_t waterExtraCount = 0;
    tbd::Mat4 waterTestModel = tbd::Mat4::Identity();
    if (waterTestOn && g_water.Active()) {
        if (waterTestOwner != &r) { const auto v = tbd::MakeMesh(tbd::MeshKind::Sphere, {0.85f, 0.25f, 0.15f}); waterTestMesh = r.CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles); waterTestOwner = &r; }
        const float t = (float)waterTestFrame / 60.0f;
        const float x = waterTest[0] + waterTest[4] * std::sin(t * waterTest[3] / std::max(waterTest[4], 0.1f));
        float surface = 0.0f;
        if (!g_water.HeightAt(x, waterTest[1], surface)) surface = eye.y - 10.0f;
        const tbd::Vec3 at = {x, surface + std::sin(t * 1.7f) * waterTest[2] * 1.5f, waterTest[1]};
        waterExtra[waterExtraCount++] = {at, waterTest[2]};
        waterTestModel = tbd::TRS(at, tbd::Quat{}, {waterTest[2] * 2.0f, waterTest[2] * 2.0f, waterTest[2] * 2.0f});
        ++waterTestFrame;
        if (waterTestFrame < waterTestFrames && captureFramesLeft > 0) captureFramesLeft = std::max(captureFramesLeft, benchFrames + 61 + settleFrames);
    }
    if (g_play.active && !g_play.player) { // whoever walks or flies in Play: feet when walking, the camera when flying (the character makes his own)
        const tbd::Vec3 e = g_engine.camera.Eye();
        if (g_play.walking) waterExtra[waterExtraCount++] = {{e.x, e.y - tbd::kPlayEyeHeight + 0.5f, e.z}, 0.55f};
        else waterExtra[waterExtraCount++] = {e, 0.5f};
    }
    if (tbd::WaterSystem::Mode() != '0') {
        static std::vector<tbd::Sphere> waterBounds;
        waterBounds.assign(entityCount, tbd::Sphere{});
        if (g_water.Active())
            for (size_t i = 0; i < entityCount; ++i)
                if (g_engine.meshes[i] != tbd::kInvalidMesh && !g_player.OwnsEntity(i)) // the character makes his own wake, not five body-sized ones
                    waterBounds[i] = tbd::TransformSphere(boundsOf(g_engine.meshes[i]), g_engine.models[i]);
        g_water.Update(r, g_engine.scene, waterBounds, eye, waterDt, waterExtra, waterExtraCount);
        g_water.SetSun(light.sunDir);
    }

    traceMark(kTracePre);
    FrameMark("water");
    static const bool matLab = tbd::platform::GetEnvA("TBD_MATLAB", nullptr, 0) > 0; // see material_lab.h
    static tbd::material_lab::Lab materialLab;
    if (matLab) materialLab.Build(r); // makes textures, so it runs before the frame opens
    RoadsSync(r);                       // road textures and meshes: also before the frame opens
    CityBuildingsSync(r, viewProj, eye); // zoned buildings: cells, tiers, bake cache, proxies, instance rows (uploads: before the frame)
    r.BeginFrame(clear);
    traceMark(kTraceBegin);
    FrameMark("begin-frame");
    {
        int64_t animStart, animEnd;
        animStart = tbd::platform::Ticks();
        if (g_engine.jobs) g_engine.animator.Update(r, *g_engine.jobs, dt); // after BeginFrame: this frame's vertex copy is free
        animEnd = tbd::platform::Ticks();
        g_lastAnimMs = (animEnd - animStart) * 1000.0 / g_engine.tickFreq;
    }
    traceMark(kTraceAnim);
    FrameMark("animation");
    static float vegetationTime = 0.0f;
    vegetationTime += dt;
    g_vegetation.UpdateCharacters(eye);
    g_vegetation.Compute(r, viewProj, eye);
    FrameMark("vegetation");
    g_water.Compute(r, cameraVolume, waterDt);
    FrameMark("water-compute");

    if (shadows) {
        if (!tightCasters.empty()) { // drawn first: the sun pass's end closes the shadow timer
            r.BeginShadowPass(tbd::ShadowMap::Tight);
            for (size_t i : tightCasters)
                if (g_engine.entityVisible[i] & kVisibleTight) DrawEntityCaster(r, i, kVisibleTight);
            r.EndShadowPass();
        }
        // What doesn't move is drawn once and kept: while the sun, the map and those casters stay put, the pass starts from
        // that depth and draws only the animated casters. A key is stored once it holds for a second frame, so a moving
        // camera pays for no copies. TBD_SHADOW_CACHE=0 turns this off.
        static const bool sunCacheOn = [] {
            char v[8] = {};
            return tbd::platform::GetEnvA("TBD_SHADOW_CACHE", v, sizeof(v)) == 0 || v[0] != '0';
        }();
        uint64_t sunKey = 14695981039346656037ull;
        const auto mix = [&](const void* data, size_t bytes) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t k = 0; k < bytes; ++k) sunKey = (sunKey ^ p[k]) * 1099511628211ull;
        };
        mix(light.shadowVP, sizeof(light.shadowVP));
        const uint64_t terrainKey = g_engine.terrain.params.enabled ? g_engine.terrain.ShadowCastersKey(shadowFocus, shadowRadius) : 0;
        mix(&terrainKey, sizeof(terrainKey));
        const uint64_t buildingsKey = CityBuildingsShadowKey();
        mix(&buildingsKey, sizeof(buildingsKey));
        g_engine.sunMoving.resize(entityCount);
        for (size_t i = 0; i < entityCount; ++i) {
            g_engine.sunMoving[i] = g_engine.animator.Animates(g_engine.meshes[i]) ? 1 : 0;
            if (g_engine.sunMoving[i] || !(g_engine.entityVisible[i] & kVisibleSun)) continue;
            mix(&g_engine.meshes[i], sizeof(tbd::MeshId));
            mix(g_engine.models[i].m, sizeof(g_engine.models[i].m));
        }
        for (const InstancedAsset& asset : g_engine.instanced) {
            mix(&asset.mesh, sizeof(asset.mesh));
            if (!asset.casters.empty()) mix(asset.casters.data(), asset.casters.size() * sizeof(asset.casters[0]));
        }
        static uint64_t lastSunKey = 0;
        const bool sunCached = sunCacheOn && r.SunCacheHolds(sunKey);
        r.BeginShadowPass(tbd::ShadowMap::Sun, sunCached);
        if (!sunCached) {
            if (g_engine.terrain.params.enabled) g_engine.terrain.DrawShadowCasters(r, shadowFocus, shadowRadius);
            for (size_t i = 0; i < entityCount; ++i)
                if (!g_engine.sunMoving[i] && (g_engine.entityVisible[i] & kVisibleSun)) DrawEntityCaster(r, i, kVisibleSun);
            DrawInstancedCasters(r);
            CityBuildingsCasters(r);
            if (sunCacheOn && sunKey == lastSunKey) r.StoreSunCache(sunKey);
        }
        lastSunKey = sunKey;
        static uint32_t sunHits = 0, sunFrames = 0;
        sunHits += sunCached ? 1 : 0;
        ++sunFrames;
        if (captureFramesLeft == 1) tbd::Log("Sun cache: kept depth used on %u of %u frames", sunHits, sunFrames);
        for (size_t i = 0; i < entityCount; ++i)
            if (g_engine.sunMoving[i] && (g_engine.entityVisible[i] & kVisibleSun)) DrawEntityCaster(r, i, kVisibleSun);
        g_vegetation.DrawShadows(r, vegetationTime); // it sways, so it is never kept
        SimAgentsCasters(r);                         // near cosmetic cars: they move, so never kept either
        r.EndShadowPass();
    }
    if (takeCapture) {
        const tbd::ViewVolume captureVolume = tbd::ViewVolume::FromMatrix(captureVP, true);
        r.BeginCapture();
        for (size_t i = 0; i < entityCount; ++i) {
            if (g_engine.meshes[i] == tbd::kInvalidMesh || g_water.IsSurface(i)) continue;
            if (!captureVolume.Touches(tbd::TransformSphere(boundsOf(g_engine.meshes[i]), g_engine.models[i]))) continue;
            const tbd::Mat4 mvp = captureVP * g_engine.models[i];
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end()) {
                r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
                r.Draw(g_engine.meshes[i], mvp, g_engine.models[i], true, false);
                continue;
            }
            for (const MatPart& p : it->second) {
                if (p.lodMin > 0.0f || p.lodError > 0.0f) continue; // a terrain tile's nearest set, an LOD chain's full level, will do
                r.SetMaterial(p.albedo, p.normal, p.surface);
                r.Draw(p.mesh, mvp, g_engine.models[i], true, false);
            }
        }
        r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
        r.EndCapture();
    }
    CityBuildingsBake(r, eye); // this frame's chunk bakes: after the shadows and the capture, before the scene
    traceMark(kTraceShadow);
    FrameMark("shadow");
    // Where the scene's triangles go, every 600 frames: by kind, and the entities that cost the most.
    static uint32_t drawStatsFrame = 0;
    const bool drawStats = ++drawStatsFrame % 600 == 0;
    uint64_t triTerrain = 0, triVegetation = 0, triGrid = 0, triInstanced = 0;
    const auto trisNow = [&] { return r.Counts().triangles; };
    uint64_t triMark = trisNow();
    g_drawCats = {};
    const tbd::FrameCounts sceneStart = r.Counts();
    if (g_engine.terrain.params.enabled) g_engine.terrain.Draw(r, sceneViewProj, eye, g_engine.camera.Forward());
    triTerrain = trisNow() - triMark;
    g_drawCats.Add(kCatTerrain, sceneStart, r.Counts());
    // Near entities, then the instances (scattered over the ground), then the big pieces.
    bool materialSet = false;
    tbd::TextureId boundAlbedo = tbd::kNoTexture, boundNormal = tbd::kNoTexture, boundSurface = tbd::kNoTexture;
    const auto useMaterial = [&](tbd::TextureId albedo, tbd::TextureId normal, tbd::TextureId surface) {
        if (materialSet && albedo == boundAlbedo && normal == boundNormal && surface == boundSurface) return;
        tbd::TextureId drawAlbedo = albedo, drawNormal = normal, drawSurface = surface;
        surface_async::Apply(albedo, drawNormal, drawSurface);
        ground_bake::Apply(drawAlbedo, drawNormal, drawSurface);
        r.SetMaterial(drawAlbedo, drawNormal, drawSurface);
        materialSet = true;
        boundAlbedo = albedo; boundNormal = normal; boundSurface = surface;
    };
    // Depth pre-pass: the opaque parts lay down depth first with no pixel shader, so the lit shader then runs about once per pixel
    // instead of four times (ground under street under cobbles, buildings behind buildings). TBD_PREPASS=0 turns it off.
    static const bool prepassOn = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_PREPASS", v, sizeof(v)) == 0 || v[0] != '0'; }();
    const auto drawInstanced = [&] {
        r.SetScenePrepass(0);
        uint64_t mark = trisNow();
        tbd::FrameCounts catMark = r.Counts();
        if (!((featureOff | qualityOff) & 1024)) g_vegetation.Draw(r, sceneViewProj, vegetationTime, useMaterial);
        triVegetation = trisNow() - mark;
        g_drawCats.Add(kCatVegetation, catMark, r.Counts());
        catMark = r.Counts();
        mark = trisNow();
        if (!((featureOff | qualityOff) & 512)) ground_bake::DrawGrid(r, sceneViewProj, useMaterial);
        triGrid = trisNow() - mark;
        g_drawCats.Add(kCatGroundGrid, catMark, r.Counts());
        mark = trisNow();
        struct InstancedDone { uint64_t* out; uint64_t mark; const std::function<uint64_t()> now; ~InstancedDone() { *out = now() - mark; } } instancedDone{&triInstanced, mark, trisNow};
        struct PropsDone { tbd::Renderer& r; tbd::FrameCounts mark; ~PropsDone() { g_drawCats.Add(kCatProps, mark, r.Counts()); } } propsDone{r, r.Counts()};
        for (const InstancedAsset& asset : g_engine.instanced) {
            if (asset.visible.empty()) continue;
            const auto it = g_engine.matParts.find(asset.mesh);
            if (it == g_engine.matParts.end()) {
                useMaterial(tbd::kNoTexture, tbd::kNoTexture, tbd::kNoTexture);
                for (const InstancedAsset::Range& v : asset.visible) r.DrawInstanced(asset.mesh, asset.buffer, v.first, v.count, sceneViewProj);
                continue;
            }
            for (const MatPart& p : it->second) {
                if (p.lodError > 0.0f) continue; // full level only: no per-instance LOD choice yet
                useMaterial(p.albedo, p.normal, p.surface);
                r.SetScenePrepass(prepassOn && p.opaque ? 2 : 0);
                for (const InstancedAsset::Range& v : asset.visible) r.DrawInstanced(p.mesh, asset.buffer, v.first, v.count, sceneViewProj);
            }
        }
        r.SetScenePrepass(0);
    };
    if (prepassOn) {
        const tbd::FrameCounts prepassMark = r.Counts();
        struct PrepassDone { tbd::Renderer& r; tbd::FrameCounts mark; ~PrepassDone() { g_drawCats.Add(kCatPrepass, mark, r.Counts()); } } prepassDone{r, prepassMark};
        r.SetScenePrepass(1);
        for (const SceneDraw& d : g_engine.sceneDraws)
            if (d.opaque) r.Draw(d.mesh, g_engine.mvps[d.entity], g_engine.models[d.entity], true, false);
        for (const InstancedAsset& asset : g_engine.instanced) {
            const auto it = asset.visible.empty() ? g_engine.matParts.end() : g_engine.matParts.find(asset.mesh);
            if (it == g_engine.matParts.end()) continue;
            for (const MatPart& p : it->second)
                if (p.lodError == 0.0f && p.opaque)
                    for (const InstancedAsset::Range& v : asset.visible) r.DrawInstanced(p.mesh, asset.buffer, v.first, v.count, sceneViewProj);
        }
        r.SetScenePrepass(0);
    }
    bool instancesDrawn = false;
    static std::vector<uint64_t> entityTris;
    if (drawStats) entityTris.assign(entityCount, 0);
    uint64_t triEntities = 0;
    for (const SceneDraw& d : g_engine.sceneDraws) {
        if (!instancesDrawn && d.order >= 0x10000u) {
            drawInstanced();
            instancesDrawn = true;
        }
        useMaterial(d.albedo, d.normal, d.surface);
        r.SetScenePrepass(prepassOn && d.opaque ? 2 : 0);
        const tbd::FrameCounts drawMark = r.Counts();
        r.Draw(d.mesh, g_engine.mvps[d.entity], g_engine.models[d.entity], true, true);
        const bool zoned = d.entity < g_engine.scene.entities.size() && g_engine.scene.entities[d.entity].name.compare(0, 15, "Zoned building ") == 0;
        g_drawCats.Add(zoned ? kCatZoned : kCatEntities, drawMark, r.Counts());
        if (drawStats && d.entity < entityTris.size()) { const uint64_t t = r.MeshIndexCount(d.mesh) / 3; entityTris[d.entity] += t; triEntities += t; }
    }
    if (!instancesDrawn) drawInstanced();
    r.SetScenePrepass(0);
    if (drawStats) {
        std::vector<size_t> order;
        for (size_t i = 0; i < entityTris.size(); ++i) if (entityTris[i]) order.push_back(i);
        const size_t top = std::min<size_t>(order.size(), 10);
        std::partial_sort(order.begin(), order.begin() + (ptrdiff_t)top, order.end(), [](size_t a, size_t b) { return entityTris[a] > entityTris[b]; });
        std::string line;
        char item[160];
        for (size_t k = 0; k < top; ++k) {
            snprintf(item, sizeof(item), "%s%s %.0fk", k ? ", " : "", g_engine.scene.entities[order[k]].name.c_str(), (double)entityTris[order[k]] / 1e3);
            line += item;
        }
        tbd::Log("Scene triangles: terrain %.2f M, vegetation %.2f M, ground grid %.2f M, instanced %.2f M, entities %.2f M in %zu draws (%zu entities); most: %s",
                 (double)triTerrain / 1e6, (double)triVegetation / 1e6, (double)triGrid / 1e6, (double)triInstanced / 1e6, (double)triEntities / 1e6, g_engine.sceneDraws.size(), order.size(), line.c_str());
    }
    if (materialSet) r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    if (matLab) materialLab.Draw(r, sceneViewProj, g_engine.camera.target);
    if (waterTestOn && waterTestMesh != tbd::kInvalidMesh) r.Draw(waterTestMesh, sceneViewProj * waterTestModel, waterTestModel, true, true);
    { // city roads: one draw per material
        bool any = false;
        for (int m = 0; m < tbd::city::kMatCount; ++m) {
            if (g_roadGpu.mesh[m] == tbd::kInvalidMesh || CityRoadsByCell()) continue; // per cell: CityRoadsDraw below
            const tbd::FrameCounts roadMark = r.Counts();
            r.SetMaterial(g_roadGpu.albedo[m], g_roadGpu.normal[m]);
            r.Draw(g_roadGpu.mesh[m], sceneViewProj, tbd::Mat4::Identity(), true, true);
            g_drawCats.Add(m == (int)tbd::city::Mat::Track ? kCatRail : kCatRoads, roadMark, r.Counts());
            any = true;
        }
        if (CityRoadsByCell()) { // per CityCell: meshes only for Near cells, the rest is in their bakes (R9)
            const tbd::FrameCounts roadMark = r.Counts();
            CityRoadsDraw(r, sceneViewProj, false);
            g_drawCats.Add(kCatRoads, roadMark, r.Counts());
            const tbd::FrameCounts railMark = r.Counts();
            CityRoadsDraw(r, sceneViewProj, true);
            g_drawCats.Add(kCatRail, railMark, r.Counts());
        }
        if (g_roads.active && g_roadGpu.arrows != tbd::kInvalidMesh) { // lane direction arrows: road editor only
            r.SetMaterial(g_roadGpu.albedo[(int)tbd::city::Mat::Paint], g_roadGpu.normal[(int)tbd::city::Mat::Paint]);
            r.Draw(g_roadGpu.arrows, sceneViewProj, tbd::Mat4::Identity(), true, true);
            any = true;
        }
        if (any) r.SetMaterial(tbd::kNoTexture, tbd::kNoTexture);
    }
    {
        const tbd::FrameCounts zonedMark = r.Counts();
        CityBuildingsDraw(r, sceneViewProj);
        g_drawCats.Add(kCatZoned, zonedMark, r.Counts());
    }
    const tbd::FrameCounts cityMark = r.Counts();
    ZonesDraw(r, sceneViewProj); // zone cells, construction sites, the zoning cursor
    OverlaysDraw(r, sceneViewProj); // the info overlay (one draw)
    TransportDraw(r, sceneViewProj); // transport buildings, the hologram being placed, lot markers
    UtilDraw(r, sceneViewProj); // poles, pylons, wires, the underground view, the utilities cursor
    IndustryDraw(r, sceneViewProj); // industry hubs, work areas, the resource heat map
    GarbageDraw(r, sceneViewProj);  // garbage heaps, the garbage info view
    const tbd::FrameCounts agentsMark = r.Counts();
    g_drawCats.Add(kCatCity, cityMark, agentsMark);
    SimAgentsDraw(r, sceneViewProj); // cosmetic traffic from the flow solve: one instanced draw per car model
    HerdsDraw(r, sceneViewProj);     // the herd children (replace the cosmetic cars when the herd system is running)
    g_drawCats.Add(kCatAgents, agentsMark, r.Counts());
    const tbd::FrameCounts waterMark = r.Counts();
    g_water.SubmitDraw(r, sceneViewProj, waterJitter);
    g_drawCats.Add(kCatWater, waterMark, r.Counts());
    const tbd::FrameCounts sceneEnd = r.Counts(); // everything after this is UI and post (closed at the end of the frame)
    {
        // The draws counted between the shadow pass and the terrain (sky, clouds...) and anything not wrapped above.
        uint32_t known = 0;
        uint64_t knownTris = 0;
        for (int c = 0; c < kCatCount; ++c) { known += g_drawCats.draws[c]; knownTris += g_drawCats.tris[c]; }
        const uint32_t sceneDrawsAll = sceneEnd.draws - sceneStart.draws;
        const uint64_t sceneTrisAll = sceneEnd.triangles - sceneStart.triangles;
        g_drawCats.draws[kCatEntities] += sceneDrawsAll > known ? sceneDrawsAll - known : 0;
        g_drawCats.tris[kCatEntities] += sceneTrisAll > knownTris ? sceneTrisAll - knownTris : 0;
    }
    tbd::profiler::Sample profSample; // the profiler overlay's numbers; only gathered while it is on
    if (g_profiler.Enabled()) {
        for (const InstancedAsset& asset : g_engine.instanced) {
            for (const tbd::InstanceCell& c : asset.cells) profSample.instancesTotal += c.count;
            for (const InstancedAsset::Range& v : asset.visible) profSample.instancesDrawn += v.count;
            for (const InstancedAsset::Range& c : asset.casters) profSample.casterInstances += c.count;
            profSample.instanceRanges += (uint32_t)asset.visible.size();
        }
        for (uint8_t f : g_engine.entityVisible) profSample.entitiesDrawn += (f & kVisibleCamera) ? 1 : 0;
        profSample.entities = (uint32_t)entityCount;
        profSample.sceneDraws = (uint32_t)g_engine.sceneDraws.size();
        profSample.cullMs = (cullEnd - cullStart) * 1000.0 / g_engine.tickFreq;
        profSample.draws = r.Counts().draws;
        profSample.triangles = r.Counts().triangles;
        SimProfile(profSample);
        for (int c = 0; c < kCatCount; ++c) profSample.cats.push_back({kDrawCatNames[c], g_drawCatsShown.draws[c], g_drawCatsShown.tris[c]});
    }
    if (captureFramesLeft == 1) { // the numbers behind a capture's frame time
        uint32_t drawnInstances = 0, instanceTotal = 0, instancedDraws = 0, castInstances = 0;
        for (const InstancedAsset& asset : g_engine.instanced) {
            for (const tbd::InstanceCell& c : asset.cells) instanceTotal += c.count;
            for (const InstancedAsset::Range& v : asset.visible) drawnInstances += v.count;
            for (const InstancedAsset::Range& c : asset.casters) castInstances += c.count;
            instancedDraws += (uint32_t)asset.visible.size();
        }
        const tbd::FrameCounts counts = r.Counts();
        uint32_t sunEntities = 0;
        for (uint8_t f : g_engine.entityVisible) sunEntities += (f & kVisibleSun) ? 1 : 0;
        tbd::Log("Cull: %.2f ms; entities %zu (sun %u), parts %u, scene draws %zu; instances %u of %u in %u ranges, casters %u; drawn %u draws, %llu triangles",
                 (cullEnd - cullStart) * 1000.0 / g_engine.tickFreq, entityCount, sunEntities, partTotal, g_engine.sceneDraws.size(),
                 drawnInstances, instanceTotal, instancedDraws, castInstances, counts.draws, (unsigned long long)counts.triangles);
    }
    traceMark(kTraceScene);
    FrameMark("scene");
    r.ResolveScene();
    const bool overlays = !testShot && !g_play.active && !g_maxGraphics; // Play hides the grid, outline, gizmos and the orientation cube
    if (overlays && !(g_terrainEdit.on && g_engine.terrain.params.enabled && !g_engine.terrain.params.flatStart)) tbd::DrawGridAndAxes(r, viewProj); // the 10 m grid is noise in a terrain sandbox
    if (overlays && g_terrainEdit.on && g_engine.terrain.params.enabled && g_terrainEdit.mouseX >= 0 &&
        (g_engine.drag == Drag::Terrain || !g_gui.WantsMouse())) { // brush ring: where and how big, hugging the ground (not under the GUI)
        tbd::Vec3 hit;
        const tbd::Ray ray = g_engine.camera.ScreenRay((float)g_terrainEdit.mouseX, (float)g_terrainEdit.mouseY, (float)g_engine.vpW, (float)g_engine.vpH);
        if (GroundHit(ray, hit)) {
            const int mode = TerrainBrushModeNow();
            const tbd::Vec3 color = mode == (int)tbd::gui::TerrainBrushMode::Flatten ? tbd::Vec3{0.3f, 0.6f, 1.0f}
                                    : mode == (int)tbd::gui::TerrainBrushMode::Dig   ? tbd::Vec3{1.0f, 0.35f, 0.3f}
                                                                                     : tbd::Vec3{1.0f, 0.9f, 0.3f};
            std::vector<tbd::Vertex> ring;
            const int segments = 64;
            for (float scale : {1.0f, 0.5f}) {
                tbd::Vec3 prev{};
                for (int i = 0; i <= segments; ++i) {
                    const float a = (float)i / segments * 6.2831853f;
                    const float x = hit.x + std::cos(a) * g_terrainEdit.radius * scale, z = hit.z + std::sin(a) * g_terrainEdit.radius * scale;
                    const tbd::Vec3 p{x, g_engine.terrain.HeightAt(x, z) + 0.4f, z};
                    if (i > 0) tbd::editor_draw::Line(ring, prev, p, color);
                    prev = p;
                }
            }
            r.DrawTransient(ring.data(), (uint32_t)ring.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), true);
        }
    }
    if (g_zoneTool.active && !g_play.active) ZonesCursor();
    if (g_utilTool.active && !g_play.active) UtilCursor();
    if (g_indTool.active && !g_play.active) IndustryCursor();
    if (g_roads.active && !g_play.active) { // the road tool's ghost, handles, guidelines and snap marker (through the ground: tunnels)
        RoadsCursor();
        static std::vector<tbd::Vertex> ghostTris, ghostLines;
        g_roads.Ghost(ghostTris, ghostLines);
        if (!ghostTris.empty()) r.DrawTransient(ghostTris.data(), (uint32_t)ghostTris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), false);
        if (!ghostLines.empty()) r.DrawTransient(ghostLines.data(), (uint32_t)ghostLines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
    }
    static const bool navDebug = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_NAVDEBUG", v, sizeof(v)) > 0 && v[0] != '0'; }();
    if (navDebug && !testShot && g_npcs.Active()) g_npcs.DebugDraw(r, viewProj, g_player.Placed() ? g_player.State().feet : g_engine.camera.target);
    if (static std::vector<tbd::Vertex> brush; !testShot && !g_play.active && (brush.clear(), tbd::shell::EditorBrushLines(brush))) // Character Creator hair brush
        r.DrawTransient(brush.data(), (uint32_t)brush.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
    if (static std::vector<tbd::Vertex> clayTris, clayLines; !g_play.active && (clayTris.clear(), clayLines.clear(), tbd::shell::EditorClayDraw(clayTris, clayLines))) { // Hair Maker clay + comb
        if (!clayTris.empty()) r.DrawTransient(clayTris.data(), (uint32_t)clayTris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        if (!clayLines.empty()) r.DrawTransient(clayLines.data(), (uint32_t)clayLines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), true);
    }
    g_engine.gizmo.mode = tbd::GizmoModeForTool(g_engine.tools.Active());
    if (const tbd::Entity* sel = overlays ? g_engine.scene.Selected() : nullptr) {
        tbd::DrawSelectionOutline(r, viewProj, *sel);
        g_engine.gizmo.Draw(r, g_engine.camera, viewProj, *sel, (float)g_engine.vpH, DpiScale());
    }
    tbd::shell::RefreshInspector();
    if (overlays) tbd::DrawOrientationGizmo(r, view, g_engine.vpW, g_engine.vpH, g_engine.dpi);

    const tbd::FrameStats& s = g_engine.stats.Shown();
    char fps[48];
    snprintf(fps, sizeof(fps), "FPS %.0f (%.1f MS)", s.fps, s.frameMs);
    char pos[64];
    snprintf(pos, sizeof(pos), "CAM %.0f %.0f %.0f", eye.x, eye.y, eye.z);
    const int sunHour = (int)g_engine.sun.hour, sunMinute = (int)((g_engine.sun.hour - (float)sunHour) * 60.0f);
    char sunTime[32];
    snprintf(sunTime, sizeof(sunTime), "SUN %02d:%02d", sunHour, sunMinute);
    std::vector<std::string> lines = {std::string("API ") + r.Name(), fps, "DRAWS " + Grouped(s.drawCalls),
                                      "POLYS " + Grouped(s.polys), pos, sunTime};
    if (g_engine.terrain.params.enabled)
        lines.push_back("TERRAIN " + std::to_string(g_engine.terrain.LoadedChunks()) + " CHUNKS");
    if (g_stream.Active()) lines.push_back(g_stream.StatusLine());
    static const bool drawCatsLine = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_DRAWCATS", v, sizeof(v)) > 0 && v[0] != '0'; }();
    if (drawCatsLine) { // the building budget's two numbers, and the shadow pass
        lines.push_back("ZONED " + Grouped(g_drawCatsShown.draws[kCatZoned]) + " DRAWS " + Grouped(g_drawCatsShown.tris[kCatZoned] / 1000) + "K TRIS");
        lines.push_back("SHADOW " + Grouped(g_drawCatsShown.draws[kCatShadow]) + " DRAWS " + Grouped(g_drawCatsShown.tris[kCatShadow] / 1000) + "K TRIS");
    }
    if (SceneLoadActive()) lines.push_back(SceneLoadStatus());
    if (g_engine.drag == Drag::Fly) {
        char speed[32];
        snprintf(speed, sizeof(speed), "FLY SPEED %.0f", g_engine.flySpeed);
        lines.push_back(speed);
    }
    if (tbd::platform::TickCountMs() < g_engine.toastUntil) lines.push_back(g_engine.toast);
    if (g_play.active) { // just the FPS chip and the controls
        char mode[48];
        snprintf(mode, sizeof(mode), "%s SPEED %.0f", g_play.player ? "PLAYER" : g_play.walking ? "WALK" : "FLY", g_play.player ? g_player.Speed() : g_play.Speed());
        lines = {fps, mode, g_player.Active() ? "WASD MOVE  SHIFT RUN  SPACE JUMP   G FLY   ESC STOP" : "G WALK FLY   ESC STOP"};
        if (tbd::platform::TickCountMs() < g_engine.toastUntil) lines.push_back(g_engine.toast);
    }
    float statsBottom = 0.0f; // where the top-left stats text ends: the in-game GUI keeps its panels below it
    if (!testShot && !g_maxGraphics && !g_playtest.Active()) statsBottom = tbd::DrawStatsText(r, g_engine.vpW, g_engine.vpH, g_engine.dpi, lines);
    if (!testShot && g_profiler.Enabled()) // nullptr: the per-pass GPU times are not exposed by the renderers yet
        g_profiler.Draw(r, g_engine.vpW, g_engine.vpH, g_engine.dpi, r.Name(), profSample, g_stream.Snapshot(), nullptr);
    DrawGameGui(r, statsBottom); // in-game GUI last, over every other overlay

    // TBD_CAPTURE=<file.bmp>: write the presented frame (read back from the renderer, not the screen) and quit.
    int& captureFrames = captureFramesLeft;
    if (captureFrames == 1) r.SetFrameCapture(true);
    const std::string playtestShot = g_playtest.Active() ? g_playtest.TakeShot() : std::string();
    if (!playtestShot.empty()) r.SetFrameCapture(true);
    const bool remoteShot = tbd::shell::CaptureThisFrame();
    if (remoteShot) r.SetFrameCapture(true);
    traceMark(kTraceEnd);
    FrameMark("overlays");
    r.EndFrame();
    FrameMark("end-frame");
    CityChunkCheckAfterFrame(r);
    if (remoteShot) tbd::shell::AfterCapture(r);
    if (!playtestShot.empty()) {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (r.CapturedFrame(px, w, h)) {
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            tbd::BmpFileHeader fh = {0x4D42, (uint32_t)(sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader) + px.size()), 0, 0, sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader)};
            tbd::BmpInfoHeader ih = {sizeof(ih), w, -h, 1, 32, tbd::kBmpRgb};
            if (FILE* f = fopen(playtestShot.c_str(), "wb")) {
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
        }
        r.SetFrameCapture(false);
        g_water.LogRipples(r);
    }
    // TBD_PLAYTEST with TBD_BENCH=<file>: frame times of the moving run, once the ground has loaded. A frame that wrote a shot is left out; the worst ones are listed.
    if (g_playtest.Active() && g_player.Placed()) {
        static wchar_t path[tbd::platform::kMaxPath] = {};
        static const bool wanted = tbd::platform::GetEnvW(L"TBD_BENCH", path, tbd::platform::kMaxPath) > 0;
        static std::vector<std::pair<double, int>> times;
        static int64_t last = {};
        static int frame = 0;
        int64_t now;
        now = tbd::platform::Ticks();
        ++frame;
        if (wanted && last && playtestShot.empty()) times.push_back({(now - last) * 1000.0 / g_engine.tickFreq, frame});
        last = now;
        if (wanted && g_playtest.Finished() && !times.empty()) {
            std::sort(times.begin(), times.end());
            double sum = 0;
            for (const auto& t : times) sum += t.first;
            if (FILE* f = tbd::platform::WOpen(path, L"w")) {
                const size_t n = times.size();
                fprintf(f, "%s frames=%zu avg_ms=%.2f p50_ms=%.2f p99_ms=%.2f p999_ms=%.2f max_ms=%.2f over_16ms=%zu over_33ms=%zu\n", r.Name(), n, sum / n, times[n / 2].first, times[n * 99 / 100].first, times[n * 999 / 1000].first, times.back().first,
                        (size_t)std::count_if(times.begin(), times.end(), [](const auto& t) { return t.first > 16.67; }), (size_t)std::count_if(times.begin(), times.end(), [](const auto& t) { return t.first > 33.3; }));
                for (size_t k = 0; k < 12 && k < n; ++k) fprintf(f, "worst %.1f ms at frame %d of %d\n", times[n - 1 - k].first, times[n - 1 - k].second, frame);
                fclose(f);
            }
        }
    }
    if (g_playtest.Finished()) {
        char reportPath[tbd::platform::kMaxPath] = {};
        if (g_npcs.Active() && tbd::platform::GetEnvA("TBD_NPC_REPORT", reportPath, tbd::platform::kMaxPath) > 0)
            if (FILE* f = fopen(reportPath, "w")) { g_npcs.WriteReport(f, r.Name()); fclose(f); }
        tbd::Log("Playtest: done");
        tbd::shell::Quit();
    }
    if (captureFrames == 1) { g_vegetation.LogCounts(r); g_water.LogRipples(r); }
    // TBD_BENCH=<file.txt> with TBD_CAPTURE: frame times after a 60 frame warm-up, written when the capture quits.
    static wchar_t benchPath[tbd::platform::kMaxPath] = {};
    static const bool bench = tbd::platform::GetEnvW(L"TBD_BENCH", benchPath, tbd::platform::kMaxPath) > 0;
    if (bench && captureFrames > 0) {
        static std::vector<double> times;
        static int64_t last = {};
        int64_t now;
        now = tbd::platform::Ticks();
        // The last frame reads the picture back, which no real frame does: it is not measured.
        const bool measured = last && captureFrames <= benchFrames + 1 && captureFrames > 1;
        const double frameMs = (now - last) * 1000.0 / g_engine.tickFreq;
        if (measured) times.push_back(frameMs);
        last = now;
        static wchar_t logPath[tbd::platform::kMaxPath] = {};
        static FILE* frameLog = tbd::platform::GetEnvW(L"TBD_FRAMELOG", logPath, tbd::platform::kMaxPath) > 0 ? tbd::platform::WOpen(logPath, L"w") : nullptr;
        static int64_t lastEnd = {};
        if (frameLog && measured) {
            const auto ms = [&](int64_t a, int64_t b) { return (b - a) * 1000.0 / g_engine.tickFreq; };
            const double* gpu = r.GpuPasses();
            double gpuTotal = 0;
            for (int k = 0; k < 8; ++k) gpuTotal += gpu[k];
            static bool header = (fputs("frame,total,outside,pre,begin,fence,anim,shadow,scene,overlay,end,present,gpu,gpu_shadow,gpu_scene\n", frameLog), true);
            (void)header;
            fprintf(frameLog, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", benchFrames - captureFrames, frameMs,
                    lastEnd ? ms(lastEnd, traceTick[kTraceStart]) : 0.0, ms(traceTick[kTraceStart], traceTick[kTracePre]),
                    ms(traceTick[kTracePre], traceTick[kTraceBegin]), r.FenceWaitMs(), ms(traceTick[kTraceBegin], traceTick[kTraceAnim]),
                    ms(traceTick[kTraceAnim], traceTick[kTraceShadow]), ms(traceTick[kTraceShadow], traceTick[kTraceScene]),
                    ms(traceTick[kTraceScene], traceTick[kTraceEnd]), ms(traceTick[kTraceEnd], now), r.PresentMs(), gpuTotal, gpu[0], gpu[1]);
        }
        lastEnd = now;
        if (frameLog && captureFrames == 1) { fclose(frameLog); frameLog = nullptr; }
        if (captureFrames == 1 && !times.empty()) {
            std::sort(times.begin(), times.end());
            double sum = 0;
            for (double t : times) sum += t;
            if (FILE* f = tbd::platform::WOpen(benchPath, L"w")) {
                fprintf(f, "%s frames=%zu avg_ms=%.2f p50_ms=%.2f p99_ms=%.2f fps=%.1f\n", r.Name(), times.size(), sum / times.size(),
                        times[times.size() / 2], times[times.size() * 99 / 100], 1000.0 * times.size() / sum);
                size_t over = 0;
                for (double t : times) over += t > 2.0 * times[times.size() / 2] ? 1 : 0;
                fprintf(f, "p999_ms=%.2f max_ms=%.2f over_2x_median=%zu\n", times[times.size() * 999 / 1000], times.back(), over);
                fclose(f);
            }
        }
    }
    if (captureFrames > 0 && --captureFrames == 0) {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (r.CapturedFrame(px, w, h)) {
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            tbd::BmpFileHeader fh = {0x4D42, (uint32_t)(sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader) + px.size()), 0, 0, sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader)};
            tbd::BmpInfoHeader ih = {sizeof(ih), w, -h, 1, 32, tbd::kBmpRgb};
            if (FILE* f = tbd::platform::WOpen(capturePath, L"wb")) {
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
        }
        tbd::shell::Quit();
    }
    tbd::shell::AfterFrame(r);
    g_engine.stats.AddDraws(r.Counts().draws, r.Counts().triangles);
    { // close the frame's draws by kind: UI and post is everything counted after the scene ended
        uint32_t scene = 0;
        uint64_t sceneTris = 0;
        for (int c = 0; c < kCatCount; ++c)
            if (c != kCatUi && c != kCatShadow) { scene += g_drawCats.draws[c]; sceneTris += g_drawCats.tris[c]; }
        const tbd::FrameCounts end = r.Counts();
        g_drawCats.draws[kCatUi] = end.draws > scene ? end.draws - scene : 0;
        g_drawCats.tris[kCatUi] = end.triangles > sceneTris ? end.triangles - sceneTris : 0;
        g_drawCats.draws[kCatShadow] = end.shadowDraws;
        g_drawCats.tris[kCatShadow] = end.shadowTriangles;
        g_drawCatsShown = g_drawCats;
        static const bool drawCatsLog = [] { char v[8] = {}; return tbd::platform::GetEnvA("TBD_DRAWCATS", v, sizeof(v)) > 0 && v[0] != '0'; }();
        static uint32_t drawCatsFrame = 0;
        if (drawCatsLog && ++drawCatsFrame % 300 == 0) {
            std::string line;
            char item[96];
            for (int c = 0; c < kCatCount; ++c) {
                if (!g_drawCats.draws[c] && !g_drawCats.tris[c]) continue;
                snprintf(item, sizeof(item), "%s%s %u/%.1fk", line.empty() ? "" : ", ", kDrawCatNames[c], g_drawCats.draws[c], (double)g_drawCats.tris[c] / 1e3);
                line += item;
            }
            const double* gpu = r.GpuPasses();
            double gpuAll = 0;
            for (int k = 0; k < 8; ++k) gpuAll += gpu[k];
            const tbd::FrameStats& fs = g_engine.stats.Shown();
            int64_t nowCpu;
            nowCpu = tbd::platform::Ticks();
            const double wallMs = (double)(nowCpu - frameCpuStart) * 1000.0 / (double)g_engine.tickFreq;
            const double cpuMs = wallMs - r.FenceWaitMs() - r.PresentMs();
            tbd::Log("Draws by kind: total %u draws %.2f M tris (shadow %u); frame %.2f ms, cpu %.2f ms (fence %.2f, present %.2f), gpu %.2f ms (scene %.2f, shadow %.2f); "
                     "cam %.0f %.0f %.0f dist %.0f; %s",
                     end.draws, (double)end.triangles / 1e6, end.shadowDraws, fs.frameMs, cpuMs, r.FenceWaitMs(), r.PresentMs(), gpuAll, gpu[1], gpu[0],
                     eye.x, eye.y, eye.z, g_engine.camera.distance, line.c_str());
        }
    }
}

// -------------------------------------------------------------------- input

Drag DragForTool(tbd::ToolId tool) {
    switch (tool) {
    case tbd::ToolId::Orbit: return Drag::Orbit;
    case tbd::ToolId::Pan: return Drag::Pan;
    case tbd::ToolId::Zoom: return Drag::Zoom;
    default: return Drag::None; // the other tools act on click
    }
}

// Game side of a drag: true when one started (the shell then captures the pointer) or ended (it releases it).
bool BeginDragAt(Drag mode, int button, int x, int y) {
    if (mode == Drag::None) return false;
    g_engine.drag = mode;
    g_engine.dragButton = button;
    g_engine.lastX = x;
    g_engine.lastY = y;
    return true;
}

bool EndDragAt(int button) {
    if (g_engine.drag == Drag::None || g_engine.dragButton != button) return false;
    g_engine.drag = Drag::None;
    g_engine.gizmo.End();
    return true;
}

tbd::Ray MouseRayAt(int x, int y) {
    return g_engine.camera.ScreenRay((float)x, (float)y, (float)g_engine.vpW, (float)g_engine.vpH);
}


// Left click with a non-view tool: a Create tool places a new entity; otherwise grab a gizmo handle if one
// is under the cursor, or select whatever entity is under the cursor (or nothing).
// True when a gizmo drag started (the shell captures the pointer).
bool LeftClickAt(int x, int y, int button) {
    const tbd::Ray ray = MouseRayAt(x, y);
    tbd::MeshKind kind;
    if (CreateKind(g_engine.tools.Active(), kind)) {
        PlaceEntity(kind, ray);
        return false;
    }
    g_engine.gizmo.mode = tbd::GizmoModeForTool(g_engine.tools.Active());
    tbd::Entity* sel = g_engine.scene.Selected();
    if (sel && g_engine.gizmo.mode != tbd::GizmoMode::None) {
        const int axis = g_engine.gizmo.HitTest(ray, g_engine.camera, *sel, (float)g_engine.vpH, DpiScale());
        if (axis >= 0 && g_engine.gizmo.Begin(axis, ray, g_engine.camera, *sel, (float)g_engine.vpH, DpiScale())) {
            g_engine.drag = Drag::Transform;
            g_engine.dragButton = button;
            return true;
        }
    }
    g_engine.scene.selected = g_engine.scene.Pick(ray);
    uint64_t lot = 0;
    float lotT = 0.0f;
    if (g_engine.scene.selected < 0 && CityPickLot(ray, lot, lotT)) ++g_pickedLotSerial, g_pickedLot = lot; // zoned buildings are not entities
    else if (g_pickedLot) ++g_pickedLotSerial, g_pickedLot = 0;
    return false;
}

void ContinueDrag(int x, int y) {
    const float dx = (float)(x - g_engine.lastX), dy = (float)(y - g_engine.lastY);
    g_engine.lastX = x;
    g_engine.lastY = y;
    switch (g_engine.drag) {
    case Drag::Transform:
        if (tbd::Entity* sel = g_engine.scene.Selected()) g_engine.gizmo.Update(MouseRayAt(x, y), *sel);
        break;
    case Drag::Orbit: g_engine.camera.Orbit(dx * tbd::shell::EditorCameraScale(), dy * tbd::shell::EditorCameraScale()); break; // creator open: slower
    case Drag::Fly: g_engine.camera.Look(dx * tbd::shell::EditorCameraScale(), dy * tbd::shell::EditorCameraScale()); break;
    case Drag::Pan:
        if (CityCamOn()) // the city: the ground follows the finger / cursor, the camera never rises or sinks
            tbd::citycam::PanGround(g_engine.camera, (float)x - dx, (float)y - dy, (float)x, (float)y, (float)g_engine.vpW, (float)g_engine.vpH,
                                    CityCamGround(g_engine.camera.target.x, g_engine.camera.target.z));
        else g_engine.camera.Pan(dx * tbd::shell::EditorCameraScale(), dy * tbd::shell::EditorCameraScale(), (float)g_engine.vpH);
        break;
    case Drag::Zoom: g_engine.camera.Zoom(-dy * 0.1f * tbd::shell::EditorCameraScale()); break; // drag up = closer
    case Drag::Terrain: break; // the brush follows lastX / lastY, applied once per frame
    case Drag::None: break;
    }
}

// Keys that don't tear down windows (those are menu accelerators). Returns true when the key was used.
bool HandleKey(int key, bool repeat) {
    if (g_roadRename.active) return RoadRenameKey(key);
    if (key == (int)tbd::platform::Key::F2 && g_roads.active && g_hoverSeg >= 0) {
        RoadRenameBegin();
        return true;
    }
    if (g_play.active) { // Play owns the keyboard: Esc stops, G walks / flies, WASD etc. are polled by PlayMode::Update
        if (key == (int)tbd::platform::Key::Escape) tbd::shell::Transport(2);
        else if (key == 'G' && !repeat) {
            if (g_player.Active()) { // the character waits where he is while the camera flies
                g_play.player = !g_play.player;
                Toast(g_play.player ? "PLAYER" : "FLYING");
            } else if (const char* message = g_play.ToggleWalk(g_engine.camera, PlayGround)) Toast(message);
        } else if (key == 'V' && !repeat && g_player.Active() && g_play.player) {
            g_player.ToggleView();
            Toast(g_player.FirstPerson() ? "THIRD PERSON" : "FIRST PERSON"); // it flips at the next frame
        } else if ((key == 'R' || key == 'B' || key == 'K' || key == 'H') && !repeat && g_player.Active() && g_play.player) {
            g_player.Request(key == 'R' ? 1 : key == 'B' ? 2 : key == 'K' ? 3 : 6); // ragdoll, explosion behind him, knock out, punch
            Toast(key == 'R' ? "RAGDOLL" : key == 'B' ? "BLAST" : key == 'K' ? "KNOCKOUT" : "PUNCH");
        }
        return true;
    }
    if (g_engine.drag == Drag::Fly) return true; // WASD belongs to the camera while flying
    const bool ctrl = tbd::platform::KeyDownSync(tbd::platform::Key::Control), alt = tbd::platform::KeyDownSync(tbd::platform::Key::Alt);
    if (key == (int)tbd::platform::Key::Space && !repeat && !ctrl && !alt) { // the game clock: pause / resume
        g_clock.paused = !g_clock.paused;
        return true;
    }
    if (g_indTool.active && IndustryKey(key)) return true;
    if (g_utilTool.active) { // the utilities tool's keys: Esc ends the line / drops the item / closes, Page Up / Down elevation, , . rotate
        switch (key) {
        case (int)tbd::platform::Key::Escape:
            if (g_utilIn.laying) g_utilIn.laying = false;
            else {
                g_utilTool.Escape();
                if (!g_utilTool.active) UtilToolClosed();
            }
            return true;
        case (int)tbd::platform::Key::Comma: g_utilTool.Rotate(15.0f); return true;
        case (int)tbd::platform::Key::Period: g_utilTool.Rotate(-15.0f); return true;
        case (int)tbd::platform::Key::PageUp: g_utilTool.Raise(1); return true;
        case (int)tbd::platform::Key::PageDown: g_utilTool.Raise(-1); return true;
        }
    }
    if (g_transportTool.active) { // the transport tool's keys: Esc drops the building / closes, , . rotate
        switch (key) {
        case (int)tbd::platform::Key::Escape: g_transportTool.Escape(); return true;
        case (int)tbd::platform::Key::Return: if (g_transportTool.lines) { g_transportTool.finishLine = true; return true; } break;
        case (int)tbd::platform::Key::Comma: g_transportTool.Rotate(15.0f); return true;
        case (int)tbd::platform::Key::Period: g_transportTool.Rotate(-15.0f); return true;
        }
    }
    if (g_zoneTool.active && key == (int)tbd::platform::Key::Escape) {
        if (g_zoneIn.held) g_zoneIn.held = false;
        else g_zoneTool.active = false;
        return true;
    }
    if (g_roads.active) { // the road tool's keys (CS2): Esc cancels / closes, Page Up / Down elevation, , . rotate, Ctrl+Z / Y
        switch (key) {
        case (int)tbd::platform::Key::Escape:
            if (g_roads.Pending()) g_roads.Cancel();
            else g_roads.Close();
            return true;
        case (int)tbd::platform::Key::PageUp: g_roads.Raise(1); return true;
        case (int)tbd::platform::Key::PageDown: g_roads.Raise(-1); return true;
        case (int)tbd::platform::Key::Comma: g_roads.Rotate(15.0f); return true;
        case (int)tbd::platform::Key::Period: g_roads.Rotate(-15.0f); return true;
        case (int)tbd::platform::Key::Back: g_roads.Undo(); return true;
        case 'Z': if (ctrl) { g_roads.Undo(); return true; } break;
        case 'Y': if (ctrl) { g_roads.Redo(); return true; } break;
        }
    }
    switch (key) {
    case (int)tbd::platform::Key::Home:
        g_engine.camera = tbd::OrbitCamera();
        return true;
    case (int)tbd::platform::Key::Delete:
        if (!repeat) DeleteSelected();
        return true;
    case (int)tbd::platform::Key::Escape:
        g_engine.scene.selected = -1;
        return true;
    }
    if (!repeat && key == (int)tbd::platform::Key::F8) { SaveTerrainEdits(); return true; }
    if (!repeat && ctrl && tbd::platform::KeyDownSync(tbd::platform::Key::Shift) && key == 'B') { CityBuildingsCycleForce(); return true; } // Track R debug
    if (!repeat && key == (int)tbd::platform::Key::F11) { LoadTerrainEdits(); return true; }
    if (ctrl || alt) return false;
    if (!repeat && key == 'T') {
        g_terrainEdit.on = !g_terrainEdit.on;
        Toast(g_terrainEdit.on ? "TERRAIN EDIT ON - DRAG PAINTS THE TOOLBAR MODE, SHIFT DIGS, CTRL FLATTENS" : "TERRAIN EDIT OFF");
        return true;
    }
    if (!repeat && key == 'U') { // in-game GUI (F1-F7, F9, F10 are menu accelerators, F8 / F11 save / load terrain)
        g_guiVisible = !g_guiVisible;
        Toast(g_guiVisible ? "GAME GUI ON (U HIDES)" : "GAME GUI OFF (U SHOWS)");
        return true;
    }
    if (g_terrainEdit.on && (key == (int)tbd::platform::Key::LeftBracket || key == (int)tbd::platform::Key::RightBracket)) {
        g_terrainEdit.radius = std::clamp(g_terrainEdit.radius * (key == (int)tbd::platform::Key::RightBracket ? 1.25f : 0.8f), 4.0f, 300.0f);
        Toast("BRUSH RADIUS " + std::to_string((int)g_terrainEdit.radius) + " M");
        return true;
    }
    if (key == 'F') {
        FrameSelected();
        return true;
    }
    if ((key == 'N' || key == 'P' || key == 'I') && !repeat) { // NPC authoring: N cycles the routine, P adds a patrol point, I sets the night door
        if (tbd::Entity* sel = g_engine.scene.Selected()) {
            tbd::NpcInfo& npc = sel->npc;
            static const char* routines[] = {"townsfolk", "guard", "loiterer", "follower", "skittish"};
            if (key == 'N') {
                int at = -1;
                for (int i = 0; i < 5; ++i) if (npc.enabled && npc.routine == routines[i]) at = i;
                if (at == 4) { npc = tbd::NpcInfo(); Toast("NPC OFF"); }
                else { npc.enabled = true; npc.routine = routines[at + 1]; Toast("NPC " + npc.routine); }
            } else if (npc.enabled && key == 'P') {
                npc.patrol.push_back(g_engine.camera.target);
                Toast("PATROL POINT " + std::to_string(npc.patrol.size()));
            } else if (npc.enabled) {
                npc.hasIndoors = true;
                npc.indoors = g_engine.camera.target;
                Toast("NPC NIGHT DOOR SET");
            } else Toast("PRESS N TO MAKE THIS AN NPC");
        }
        return true;
    }
    if (g_engine.tools.ActivateByKey((wchar_t)key)) { // Q W E R, 1 2 3, H O Z
        tbd::shell::OnToolChanged();
        return true;
    }
    return false;
}

// Mouse and keyboard input for the viewport window itself, wherever it is hosted. Left button runs the
// active View tool, or places / selects / grabs gizmo handles with the other tools. Right button flies
// (look + WASD), middle button pans, Alt + left button orbits, from any tool.
// World > Generate LODs (Outdoor): every loaded static mesh outside the building interiors gets an LOD chain, made on the
// workers as each object reloads and kept in the LOD cache, so later loads only read it. Props inside a room are left out:
// walls hide them long before they are small enough for a coarser level to matter.
void GenerateOutdoorLods() {
    CompleteSceneLoad();
    struct WorldRoom { tbd::Vec3 center, x, z; float halfHeight; };
    std::vector<WorldRoom> rooms;
    for (size_t i = 0; i < g_engine.meshes.size(); ++i) {
        const auto it = g_engine.matParts.find(g_engine.meshes[i]);
        if (it == g_engine.matParts.end()) continue;
        const tbd::Mat4& m = g_engine.models[i];
        const auto dir = [&](const float* a) { return tbd::Vec3{m.At(0, 0) * a[0] + m.At(0, 1) * a[1] + m.At(0, 2) * a[2], m.At(1, 0) * a[0] + m.At(1, 1) * a[1] + m.At(1, 2) * a[2], m.At(2, 0) * a[0] + m.At(2, 1) * a[1] + m.At(2, 2) * a[2]}; };
        for (const MatPart& part : it->second)
            for (const tbd::InteriorRoom& r : part.rooms)
                rooms.push_back({dir(r.center) + tbd::Vec3{m.At(0, 3), m.At(1, 3), m.At(2, 3)}, dir(r.x), dir(r.z), r.halfHeight * std::sqrt(m.At(0, 1) * m.At(0, 1) + m.At(1, 1) * m.At(1, 1) + m.At(2, 1) * m.At(2, 1))});
    }
    const auto inside = [&](const tbd::Vec3& p) {
        for (const WorldRoom& r : rooms) {
            const tbd::Vec3 d = p - r.center;
            const float xx = tbd::Dot(r.x, r.x), zz = tbd::Dot(r.z, r.z);
            if (xx > 0.0f && zz > 0.0f && std::fabs(tbd::Dot(d, r.x)) <= xx && std::fabs(tbd::Dot(d, r.z)) <= zz && std::fabs(d.y) <= r.halfHeight) return true;
        }
        return false;
    };
    std::set<int> reload;
    size_t indoor = 0;
    {
        std::lock_guard<std::mutex> lock(g_lodMutex);
        for (size_t i = 0; i < g_engine.scene.entities.size() && i < g_engine.meshes.size(); ++i) {
            const tbd::Entity& e = g_engine.scene.entities[i];
            if (e.mesh != tbd::MeshKind::Imported || !e.animClip.empty() || !e.faceClip.empty() || e.name.rfind("Terrain/", 0) == 0) continue;
            const auto it = g_engine.matParts.find(g_engine.meshes[i]);
            if (it == g_engine.matParts.end() || !g_engine.sourceParts.count(g_engine.meshes[i])) continue; // not loaded, or not merged static parts
            const tbd::Sphere world = tbd::TransformSphere(g_engine.meshBounds[g_engine.meshes[i]], g_engine.models[i]);
            if (it->second.front().rooms.empty() && inside(world.center)) { ++indoor; continue; } // a building's own mesh is never "inside"
            g_lodRequested.insert(e.meshPath);
            reload.insert((int)i);
        }
    }
    tbd::Log("Generate LODs: %zu outdoor objects queued, %zu inside %zu rooms left out; cache %ls", reload.size(), indoor, rooms.size(), g_lodCacheDir.c_str());
    if (g_stream.Active()) { g_stream.Reload(*g_engine.renderer, reload); return; }
    for (int i : reload) {
        DestroyEntityMesh(g_engine.meshes[(size_t)i]);
        g_engine.meshes[(size_t)i] = UploadEntityMesh(g_engine.scene.entities[(size_t)i]);
    }
}

// The in-game GUI sees viewport mouse messages first. Returns true when it consumed one: a press over a panel that is on
// screen (the last finished frame's panels, see gui.h), the moves and release of a press it took, the wheel over a panel.
// Everything else goes on to the engine unchanged; presses the engine keeps are still reported (claim = false) so the GUI
// does not light up widgets under an engine drag.
// The in-game GUI's share of pointer input (portable). Returns true when the GUI used it. `fx` says what the shell must do
// with the OS pointer: give the window focus, capture the pointer, or release it.
enum class GuiPtr { Move, LeftDown, LeftUp, OtherDown, Wheel, CaptureLost };
struct GuiPtrEffect { bool focus = false, capture = false, release = false; };
bool GuiPointer(GuiPtr ev, float x, float y, float wheelNotches, GuiPtrEffect& fx) {
    switch (ev) {
    case GuiPtr::Move:
        g_guiInput.Move(x, y);
        return g_guiOwnsButton; // a slider drag: the engine must not hover gizmos or paint underneath
    case GuiPtr::LeftDown: {
        const bool mine = g_engine.drag == Drag::None && !tbd::platform::KeyDownSync(tbd::platform::Key::Alt) && g_gui.WantsMouseAt(x, y);
        g_guiInput.Down(x, y, mine);
        if (!mine) return false;
        g_guiOwnsButton = true;
        fx.focus = true;   // shortcuts keep working after a toolbar click
        fx.capture = true; // a slider drag keeps going outside the window until release
        return true;
    }
    case GuiPtr::LeftUp:
        g_guiInput.Up(x, y);
        if (!g_guiOwnsButton) return false;
        g_guiOwnsButton = false;
        fx.release = true;
        return true;
    case GuiPtr::OtherDown: // no fly / pan starting on a panel
        return g_engine.drag == Drag::None && g_gui.WantsMouseAt(x, y);
    case GuiPtr::Wheel:
        if (g_engine.drag != Drag::None || !g_gui.WantsMouseAt(x, y)) return false;
        g_guiInput.Wheel(wheelNotches);
        return true;
    case GuiPtr::CaptureLost: // capture taken away mid-press: let go
        if (g_guiOwnsButton) {
            g_guiOwnsButton = false;
            g_guiInput.Up(-1e9f, -1e9f);
        }
        return false;
    }
    return false;
}

// ---- Viewport input (portable): the game's handling of pointer and key events over the 3D view.
// The host is the platform's pointer: Focus / Capture / Release are called exactly where the game needs them.
enum class VpEv { Char, KeyDown, LeftDown, RightDown, MiddleDown, LeftUp, RightUp, MiddleUp, OtherButton, CaptureLost, Move, Wheel };
struct VpEvent {
    VpEv type = VpEv::Move;
    int x = 0, y = 0;     // viewport coordinates (pixels)
    int key = 0;          // KeyDown: a tbd::platform::Key value
    bool repeat = false;  // KeyDown: auto-repeat
    uint32_t ch = 0;      // Char: the character
    float wheel = 0.0f;   // Wheel: notches, positive away from the user
};
struct PointerHost {
    virtual void Focus() = 0;   // keyboard focus to the view (shortcuts work after clicking a text field)
    virtual void Capture() = 0; // keep pointer events coming while a drag leaves the view
    virtual void Release() = 0;
};

void BeginDragHost(PointerHost& host, Drag mode, int button, int x, int y) {
    if (g_engine.drag != Drag::None) return;
    host.Focus(); // so shortcuts work after clicking in a text field
    if (BeginDragAt(mode, button, x, y)) host.Capture();
}

// True when the event was used.
bool ViewportInput(const VpEvent& e, PointerHost& host) {
    if (g_play.active) { // Play: the mouse is polled by PlayMode::Update; clicks must not select, drag or place anything
        if (e.type == VpEv::Wheel) {
            g_play.Wheel(e.wheel);
            return true;
        }
        if (e.type != VpEv::Char && e.type != VpEv::KeyDown && e.type != VpEv::CaptureLost) return true;
    }
    switch (e.type) {
    case VpEv::Char:
        if (g_roadRename.active) {
            RoadRenameChar(e.ch);
            return true;
        }
        break;
    case VpEv::KeyDown:
        if (HandleKey(e.key, e.repeat)) return true;
        break;
    case VpEv::LeftDown:
        if (tbd::platform::KeyDownSync(tbd::platform::Key::Alt)) {
            BeginDragHost(host, Drag::Orbit, 0, e.x, e.y);
        } else if (g_roads.active && g_engine.drag == Drag::None && !g_play.active) {
            host.Focus();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            RoadsCursor();
            g_roads.Click();
            if (g_roads.Dragging()) host.Capture(); // Swap: keep tracking the drag until release
            if (!g_roads.error.empty()) Toast(RoadsUpper(g_roads.error));
        } else if (g_utilTool.active && g_engine.drag == Drag::None && !g_play.active) {
            host.Focus();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            UtilClick();
        } else if (g_transportTool.active && g_engine.drag == Drag::None && !g_play.active) {
            host.Focus();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            TransportClick();
        } else if (g_zoneTool.active && g_engine.drag == Drag::None && !g_play.active) {
            host.Focus();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            ZonesPress();
            if (g_zoneIn.held) host.Capture();
        } else if (g_indTool.active && g_engine.drag == Drag::None && !g_play.active) {
            host.Focus();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            IndustryPress();
            if (g_indTool.dragIdx >= 0) host.Capture();
        } else if (g_terrainEdit.on && g_engine.drag == Drag::None && !g_play.active) {
            tbd::Vec3 hit;
            if (GroundHit(MouseRayAt(e.x, e.y), hit)) g_terrainEdit.flattenTarget = hit.y;
            BeginDragHost(host, Drag::Terrain, 0, e.x, e.y);
        } else if (DragForTool(g_engine.tools.Active()) != Drag::None) {
            BeginDragHost(host, DragForTool(g_engine.tools.Active()), 0, e.x, e.y);
        } else if (g_engine.drag == Drag::None) {
            host.Focus();
            if (LeftClickAt(e.x, e.y, 0)) host.Capture();
        }
        return true;
    case VpEv::RightDown:
        if (g_indTool.active && g_indTool.mode != IndustryTool::Pick && g_engine.drag == Drag::None) { IndustryRightClick(); return true; }
        if (g_roads.active && g_roads.Pending() && g_engine.drag == Drag::None) { g_roads.Cancel(); return true; } // CS2: right click cancels
        if (g_utilTool.active && (g_utilIn.laying || g_utilTool.def >= 0 || g_utilTool.Laying() || g_utilTool.bulldoze) && g_engine.drag == Drag::None) { // right click ends the line, then drops the item
            if (g_utilIn.laying) g_utilIn.laying = false;
            else g_utilTool.def = -1, g_utilTool.line = -1, g_utilTool.bulldoze = false;
            return true;
        }
        if (g_transportTool.active && g_transportTool.lines && (g_transportTool.lineStops > 0 || g_transportTool.outKind >= 0 || g_transportTool.lineBulldoze) && g_engine.drag == Drag::None) { // right click drops the line in progress / the connection tool
            g_transportTool.Escape();
            return true;
        }
        if (g_transportTool.active && (g_transportTool.def >= 0 || g_transportTool.bulldoze) && g_engine.drag == Drag::None) { // right click drops the chosen building
            g_transportTool.def = -1;
            g_transportTool.bulldoze = false;
            return true;
        }
        BeginDragHost(host, Drag::Fly, 1, e.x, e.y);
        return true;
    case VpEv::MiddleDown: BeginDragHost(host, Drag::Pan, 2, e.x, e.y); return true;
    case VpEv::LeftUp:
        if (g_indTool.dragIdx >= 0) {
            host.Release();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            IndustryRelease();
            return true;
        }
        if (g_zoneIn.held) { // zoning: the brush stops, the box is painted
            host.Release();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            ZonesRelease();
            return true;
        }
        if (g_roads.active && g_roads.Dragging()) { // Swap: the release sets the direction
            host.Release();
            g_terrainEdit.mouseX = e.x, g_terrainEdit.mouseY = e.y;
            RoadsCursor();
            g_roads.Release();
            return true;
        }
        if (EndDragAt(0)) host.Release();
        return true;
    case VpEv::RightUp: if (EndDragAt(1)) host.Release(); return true;
    case VpEv::MiddleUp: if (EndDragAt(2)) host.Release(); return true;
    case VpEv::CaptureLost:
        g_engine.drag = Drag::None;
        g_engine.gizmo.End();
        return true;
    case VpEv::Move:
        g_terrainEdit.mouseX = e.x;
        g_terrainEdit.mouseY = e.y;
        if (g_engine.drag != Drag::None) {
            ContinueDrag(e.x, e.y);
        } else {
            // Highlight the handle under the cursor.
            const tbd::Entity* sel = g_engine.scene.Selected();
            g_engine.gizmo.mode = tbd::GizmoModeForTool(g_engine.tools.Active());
            g_engine.gizmo.hover = sel ? g_engine.gizmo.HitTest(MouseRayAt(e.x, e.y), g_engine.camera, *sel,
                                                                  (float)g_engine.vpH, DpiScale())
                                       : -1;
        }
        return true;
    case VpEv::Wheel: {
        const float notches = e.wheel;
        if (g_engine.drag == Drag::Fly) g_engine.flySpeed = std::clamp(g_engine.flySpeed * std::pow(1.25f, notches), 1.0f, 3000.0f);
        else { // eased zoom toward the ground point under the cursor; Shift: fine steps; creator open: slower still
            const float n = notches * (tbd::platform::KeyDownSync(tbd::platform::Key::Shift) ? 0.12f : 0.5f) * tbd::shell::EditorCameraScale();
            const float from = g_camSmooth.goal > 0.0f ? g_camSmooth.goal : g_engine.camera.distance;
            // the city: street level (1 m) to the whole map (9 km); was 0.04 .. 200 there too, so a pinch out from the 3.2 km
            // start view snapped in to 200 m and zoom out never worked
            g_camSmooth.goal = CityCamOn() ? tbd::citycam::ZoomedDistance(from, 2.0f * n) : std::clamp(from * std::pow(0.9f, n), 0.04f, 200.0f);
            g_camSmooth.lastSet = g_engine.camera.distance;
            tbd::Vec3 hit; // e.x / e.y: the cursor in viewport coordinates
            g_camSmooth.anchored = !g_play.active &&
                GroundHit(g_engine.camera.ScreenRay((float)e.x, (float)e.y, (float)g_engine.vpW, (float)g_engine.vpH), hit);
            // near the horizon the point under the cursor can be kilometres away: zooming toward it would throw the camera
            if (g_camSmooth.anchored && CityCamOn() && std::sqrt(tbd::Dot(hit - g_engine.camera.target, hit - g_engine.camera.target)) > 4.0f * std::max(g_engine.camera.distance, 25.0f))
                g_camSmooth.anchored = false;
            if (g_camSmooth.anchored) g_camSmooth.anchor = hit;
        }
        return true;
    }
    }
    return false;
}

// ------------------------------------------------- toolbar and outliner hooks

// Play / Pause / Stop drive the one thing in the editor that runs over time, the sun's day cycle: Play starts it
// (remembering the hour it started from), Pause freezes it where it is, Stop freezes it and returns to the start hour.
// NPC bodies: copies of the player's skinned character (src/ai/npc_system.h asks for them as it needs them).
bool CreateNpcRig(std::vector<tbd::ai::RigPart>& parts) {
    std::vector<tbd::Entity> templates;
    for (const tbd::Entity& e : g_engine.scene.entities)
        if (e.animClip.find("wyatt_") != std::string::npos && e.name.rfind("NPC ", 0) != 0) templates.push_back(e);
    if (templates.empty() || !g_engine.renderer) return false;
    static int serial = 0;
    ++serial;
    const std::string first = templates[0].animClip;
    const size_t cut = first.find_last_of("\/");
    const std::string dir = cut == std::string::npos ? "" : first.substr(0, cut + 1);
    const std::vector<std::string> clips = {dir + "wyatt_idle.glb", dir + "wyatt_walk_loop.glb", dir + "wyatt_run.glb", dir + "wyatt_jump.glb"};
    for (const tbd::Entity& src : templates) {
        tbd::Entity e = src;
        e.name = "NPC " + std::to_string(serial) + " " + src.name;
        e.npc = tbd::NpcInfo();
        e.position = {0, -10000.0f, 0};
        const tbd::MeshId id = UploadEntityMesh(e);
        g_engine.scene.entities.push_back(e);
        g_engine.meshes.push_back(id);
        if (id != tbd::kInvalidMesh) g_engine.animator.SetBlendClips(id, clips, "Root");
        parts.push_back({(int)g_engine.scene.entities.size() - 1, id});
    }
    tbd::ai::RigHost& host = g_npcs.Host();
    host.idleDuration = g_engine.animator.ClipDuration(clips[0]);
    host.walkDuration = g_engine.animator.ClipDuration(clips[1]);
    host.runDuration = g_engine.animator.ClipDuration(clips[2]);
    return true;
}

// Rigs are the newest entities and go in reverse order of making, so each one is last when it is removed.
void DestroyNpcRig(std::vector<tbd::ai::RigPart>& parts) {
    for (size_t k = parts.size(); k-- > 0;) {
        const int index = parts[k].entity;
        if (index < 0 || index + 1 != (int)g_engine.scene.entities.size() || index + 1 != (int)g_engine.meshes.size()) continue;
        DestroyEntityMesh(g_engine.meshes[(size_t)index]);
        g_engine.meshes.pop_back();
        g_engine.scene.entities.pop_back();
        g_stream.OnEntityErased(g_engine.renderer.get(), index);
    }
    parts.clear();
}

// ---- Entry points for shells that host only the game (iOS). The Windows editor starts the same pieces from main.cpp.
// Order per frame: input events (ViewportInput / GuiPointer), then GameFrame.

// Engine services the game needs before a renderer: the clock and the worker threads.
void GameInit(const std::wstring& assetsRoot) {
    g_engine.assetsRoot = assetsRoot;
    g_lodCacheDir = tbd::Join(assetsRoot, L"LodCache");
    g_engine.tickFreq = tbd::platform::TickFrequency();
    if (!g_engine.jobs) g_engine.jobs = std::make_unique<tbd::JobSystem>();
    g_engine.running = true;
}

// Takes a renderer the shell created and initialised on its surface (StartRenderer's tail on Windows).
void GameAdoptRenderer(std::unique_ptr<tbd::Renderer> r, Api api, int w, int h) {
    r->SetRenderScale(RenderScale());
    g_engine.renderer = std::move(r);
    g_engine.api = api;
    g_engine.vpW = w;
    g_engine.vpH = h;
    RebuildSceneMeshes();
    UploadUiFont(); // in-game GUI: smooth path and its font, or the classic fallback
    tbd::Log("%s: renderer started (%d x %d)", ApiName(api), w, h);
}

// The city game on a new random map (what a project with "terrainSandbox" opens on Windows).
void GameStartCity() { StartTerrainSandbox(); }

// The drawable changed size (rotation, split view); device pixels.
void GameResize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    g_engine.vpW = w;
    g_engine.vpH = h;
    if (g_engine.renderer) g_engine.renderer->Resize(w, h);
}

// One frame: simulation, streaming and drawing, presented by the renderer.
void GameFrame() { RenderFrame(); }

// Before the app goes away: stop the renderer and join the workers.
void GameShutdown() {
    g_engine.running = false;
    g_engine.renderer.reset();
    g_engine.jobs.reset();
}
