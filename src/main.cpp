// TBD Engine: startup, main loop, shutdown. A window with a GPU viewport (Direct3D 12 or Vulkan) that can
// be popped out into its own window, a menu strip and toolbar on top, an outliner over a details panel on
// the right, a content drawer for the project's assets at the bottom, streamed open-world terrain, and live stats.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <future>
#include <map>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

#include "surface_maps.h"
#include "material_lab.h"
#include <tuple>
#include "audio.h"
#include "bmp_header.h"
#include "camera.h"
#include "content_browser.h"
#include <set>
#include <tuple>

#include <atomic>
#include <mutex>
#include "core/jobs.h"
#include "glb_bounds.h"
#include "hot_reload.h"
#include "script/api_registry.h"
#include "script/luau_host.h"
#include "script/script_remote.h"
#include "render/cloud_noise.h"
#include "render/mesh_weld.h"
#include "culling.h"
#include "mesh_lod.h"
#include "render/chunk_faces.h"
#include "editor_draw.h"
#include "gui/gui.h"             // in-game GUI (portable, no Win32); see docs/gui.md
#include "gui/terrain_toolbar.h" // its first user: the terrain sandbox toolbar
#include "gui/city_hud.h"      // the CS2-style bottom toolbar, status bar and side icons
#include "gui/zone_toolbar.h"     // zoning (src/city/zoning.h): zone painting, growth, the game clock
#include "gui/transport_toolbar.h" // transport buildings (src/city/transport.h): depots, stations, harbours, airports
#include "gui/utility_toolbar.h"   // utilities: electricity, water, sewage (city_utilities.inl hosts them)
#include "city/transport_models.h"
#include "city/industry.h"         // specialized industry (city_industry.inl hosts it)
#include "city/utilities.h"        // utilities (city_utilities.inl hosts them)
#include "city/sim.h"              // the city sim's fixed-step scheduler (city_sim.inl hosts it)
#include "city/sim_cells.h"        // the city sim's per-cell counters: homes, jobs, residents, employment
#include "city/sim_growth.h"       // the city sim's demand, demand-gated growth, level up / down, abandonment
#include "city/sim_coverage.h"     // the city sim's utilities per cell and service coverage
#include "city/sim_economy.h"      // the city sim's treasury, taxes and upkeep
#include "city/sim_flow.h"         // the city sim's flow traffic: vehicles per hour per road segment
#include "city/sim_agents.h"       // cosmetic cars driven by the flow (not simulated)
#include "city/sim_people.h"       // tracked people: materialized on click, consistent with the counters
#include "gui/road_toolbar.h"    // the city road tool (src/city): CS2-style road building
#include "city/road_names.h"     // road / street / highway names, house numbers (chip under the road tool's cursor, F2 renames)
#include "undo.h" // engine-wide undo stack (shared with the Character Creator)
#include "gizmo.h"
#include "inspector.h"
#include "mesh_import.h"
#include "window_portals.h"
#include "skeletal_anim.h"
#include "mocap/face_driver.h"
#include "mocap/face_live_window.h"
#include "mocap/face_live_tab.h"
#include "mocap/face_lipsync.h"
#include "mocap/face_session_log.h"
#include "mocap/face_map.h"
#include "mocap/face_retarget.h"
#include "mocap/face_phone.h"
#include "character/creator.h" // Character creator panel (F7)
#include "image_decode.h"
#include "outliner.h"
#include "play_mode.h"
#include "player_character.h"
#include "ai/npc_system.h"
#include "profiler_overlay.h"
#include "project.h"
#include "remote_control.h"
#include "render/renderer.h"
#include "scene.h"
#include "scene_io.h"
#include "stats.h"
#include "sun.h"
#include "terrain.h"
#include "tool_panel.h"
#include "tools.h"
#include "ui_theme.h"
#include "water.h"

float g_brightness = 1.4f; // 1 = old look; F9 darker, F10 brighter
float g_sceneExposure = 0.0f; // the open scene's fixed exposure, 0 = auto (Scene::exposure)
#include <commctrl.h>
// Modeless brightness slider (Graphics > Brightness...). 40..300 maps to 0.40x..3.00x.
static LRESULT CALLBACK BrightnessProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HSCROLL) { g_brightness = (float)SendMessageW((HWND)l, TBM_GETPOS, 0, 0) / 100.0f; return 0; }
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void ShowBrightnessSlider(HWND owner) {
    static HWND dlg = nullptr;
    if (dlg && IsWindow(dlg)) { SetForegroundWindow(dlg); return; }
    static bool reg = false;
    if (!reg) {
        InitCommonControls();
        WNDCLASSW wc = {}; wc.lpfnWndProc = BrightnessProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TbdBrightness"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    dlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TbdBrightness", L"Brightness", WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                          200, 200, 320, 90, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND tb = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS, 8, 8, 290, 36, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(40, 300));
    SendMessageW(tb, TBM_SETTICFREQ, 20, 0);
    SendMessageW(tb, TBM_SETPOS, TRUE, (LPARAM)(g_brightness * 100.0f + 0.5f));
}
namespace {

const wchar_t* kWindowClass = L"TBDEngineWindow";
const wchar_t* kViewportClass = L"TBDViewport";
const wchar_t* kFloatClass = L"TBDViewportFloat";
const wchar_t* kWindowTitle = L"TBD Engine";
const int kStartWidth = 1280;
const int kStartHeight = 800;
const COLORREF kClearColor = RGB(26, 26, 26);

enum class Drag { None, Orbit, Pan, Zoom, Transform, Fly, Terrain };
enum class Api { D3D12, Vulkan };

enum Command : UINT {
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
    CreateDirectoryW(g_lodCacheDir.c_str(), nullptr);
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
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND viewportWindow = nullptr; // child window owned by the current renderer
    HWND floatWindow = nullptr;    // top-level host of the viewport while it is popped out
    HWND toolbar = nullptr;   // top: menu strip and toolbar
    HWND outliner = nullptr;  // right, upper: the scene's entities
    HWND inspector = nullptr; // right, lower: the Details panel
    HWND contentBrowser = nullptr;
    HMENU menu = nullptr;
    HACCEL accel = nullptr;
    HFONT uiFont = nullptr;
    bool running = false;
    int dpi = 96;

    bool viewportFloating = false;
    RECT floatRect = {};
    bool floatRectValid = false;
    bool showBrowser = true;
    int browserH = 0; // device pixels
    bool splitDrag = false;
    int dockW = 0;    // right dock width in device pixels; 0 = the default
    bool dockDrag = false;

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
    tbd::FaceDriver faceDriver; // plays face mocap clips on entities
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
    ULONGLONG toastUntil = 0;

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
tbd::hot::Shaders g_hotShaders;  // shader files changing (and F5) reload them on both backends; see hot_reload.h
tbd::hot::Game g_game;           // bin\game.dll, swapped in whenever it is rebuilt; see hot_reload.h
tbd::script::LuauHost g_script;  // Luau gameplay scripts from <project>\Scripts; see docs/scripting_spec.md
std::vector<tbd::Entity> g_gamePlaySnapshot; // the scene's transforms when Play started, put back at Stop
double g_lastAnimMs = 0.0;       // poses and skinning, last frame, for the playtest's trace
tbd::FaceSessionLog g_faceLog; // debug video of the Face Mocap Live tab

const char* ApiName(Api api) { return api == Api::D3D12 ? "D3D12" : "VULKAN"; }
int Sx(int v) { return MulDiv(v, g_engine.dpi, 96); }
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

// ------------------------------------------------------------------- layout

struct Areas {
    RECT toolbar;   // menu strip and toolbar across the top
    RECT tabStrip;  // browser-style tab strip above the viewport (empty while no extra tab is open)
    RECT viewport;  // where the viewport window lives while docked
    RECT splitter;
    RECT dockSplit; // draggable strip between the viewport column and the right dock
    RECT browser;   // the content drawer under the viewport
    RECT dock;      // right column, full height under the toolbar: outliner over details
};

Areas ComputeAreas() {
    RECT rc;
    GetClientRect(g_engine.window, &rc);
    Areas a = {};
    const int top = tbd::Toolbar_Height(g_engine.dpi);
    const int dockSplitW = Sx(5);
    const int minDock = Sx(220), maxDock = std::max(minDock, (int)rc.right - Sx(320) - dockSplitW);
    const int dockW = std::clamp(g_engine.dockW > 0 ? g_engine.dockW : Sx(300), minDock, maxDock);
    const int mainW = std::max(1, (int)rc.right - dockW - dockSplitW);
    a.toolbar = {0, 0, rc.right, top};
    a.dockSplit = {mainW, top, mainW + dockSplitW, std::max(top, (int)rc.bottom)};
    a.dock = {mainW + dockSplitW, top, mainW + dockSplitW + dockW, std::max(top, (int)rc.bottom)};
    int browserH = 0, splitH = 0;
    if (g_engine.showBrowser && g_engine.contentBrowser) {
        splitH = Sx(5);
        const int minH = tbd::ContentBrowser_MinHeight(g_engine.dpi);
        browserH = std::clamp(g_engine.browserH, minH, std::max(minH, (int)rc.bottom - top - Sx(160) - splitH));
    }
    const int tabH = g_engine.viewportFloating ? 0 : tbd::FaceTab_StripHeight(g_engine.dpi);
    a.tabStrip = {0, top, mainW, top + tabH};
    a.viewport = {0, top + tabH, mainW, std::max(top + tabH + 1, (int)rc.bottom - browserH - splitH)};
    tbd::character::Dock(a.viewport, g_engine.dpi); // the Character Creator workspace takes the viewport's left while open
    a.splitter = {0, a.viewport.bottom, mainW, a.viewport.bottom + splitH};
    a.browser = {0, a.splitter.bottom, mainW, std::max((int)a.splitter.bottom, (int)rc.bottom)};
    return a;
}

HWND ViewportHost() { return g_engine.viewportFloating && g_engine.floatWindow ? g_engine.floatWindow : g_engine.window; }

void ViewportSize(int& w, int& h) {
    if (g_engine.viewportFloating && g_engine.floatWindow) {
        RECT rc;
        GetClientRect(g_engine.floatWindow, &rc);
        w = std::max(1, (int)rc.right);
        h = std::max(1, (int)rc.bottom);
        return;
    }
    const Areas a = ComputeAreas();
    w = std::max(1, (int)(a.viewport.right - a.viewport.left));
    h = std::max(1, (int)(a.viewport.bottom - a.viewport.top));
}

// The toolbar spans the top; the outliner and details panel are stacked on the right edge; the viewport and the
// content drawer share the rest.
void Layout() {
    if (!g_engine.toolbar) return;
    RECT rc;
    GetClientRect(g_engine.window, &rc);
    const Areas a = ComputeAreas();
    MoveWindow(g_engine.toolbar, a.toolbar.left, a.toolbar.top, a.toolbar.right - a.toolbar.left,
               a.toolbar.bottom - a.toolbar.top, TRUE);
    const int dockW = (int)(a.dock.right - a.dock.left), dockH = (int)(a.dock.bottom - a.dock.top);
    int detailsH = std::clamp(dockH * 55 / 100, tbd::Inspector_Height(g_engine.dpi), Sx(340));
    if (g_engine.inspector) detailsH = std::max(detailsH, tbd::Inspector_DesiredHeight(g_engine.inspector)); // script rows need room
    detailsH = std::min(detailsH, std::max(0, dockH - Sx(100))); // the outliner keeps at least 100 px
    const int outlinerH = dockH - detailsH;
    if (g_engine.outliner) MoveWindow(g_engine.outliner, a.dock.left, a.dock.top, dockW, outlinerH, TRUE);
    if (g_engine.inspector) MoveWindow(g_engine.inspector, a.dock.left, a.dock.top + outlinerH, dockW, detailsH, TRUE);
    if (g_engine.contentBrowser) {
        MoveWindow(g_engine.contentBrowser, a.browser.left, a.browser.top, a.browser.right - a.browser.left,
                   a.browser.bottom - a.browser.top, TRUE);
        ShowWindow(g_engine.contentBrowser, g_engine.showBrowser ? SW_SHOWNA : SW_HIDE);
    }
    int w, h;
    ViewportSize(w, h);
    int vx = g_engine.viewportFloating ? 0 : (int)a.viewport.left, vy = g_engine.viewportFloating ? 0 : (int)a.viewport.top;
    RECT live;
    if (!g_engine.viewportFloating && tbd::FaceTab_ViewportRect(a.viewport, &live)) { // the tab shows the live character
        vx = live.left;
        vy = live.top;
        w = std::max(1, (int)(live.right - live.left));
        h = std::max(1, (int)(live.bottom - live.top));
    }
    // TBD_VIEWPORT="w h": render at a fixed size whatever the window's size, for benchmarks.
    char forced[32];
    int forcedW = 0, forcedH = 0;
    if (tbd::platform::GetEnvA("TBD_VIEWPORT", forced, sizeof(forced)) > 0 && sscanf_s(forced, "%d %d", &forcedW, &forcedH) == 2 && forcedW > 0 && forcedH > 0) {
        w = forcedW;
        h = forcedH;
    }
    g_engine.vpW = w;
    g_engine.vpH = h;
    if (g_engine.viewportWindow) MoveWindow(g_engine.viewportWindow, vx, vy, w, h, TRUE);
    tbd::character::PlaceDock();
    if (tbd::FaceTab_IsOpen() && !g_engine.viewportFloating)
        tbd::FaceTab_Layout(a.tabStrip, a.viewport, g_engine.viewportWindow);
    else if (g_engine.viewportWindow)
        ShowWindow(g_engine.viewportWindow, SW_SHOWNA);
    if (g_engine.renderer && rc.right > 0 && rc.bottom > 0) g_engine.renderer->Resize(w, h);
    InvalidateRect(g_engine.window, nullptr, FALSE);
}

// ----------------------------------------------------------------- renderer

void UpdateTitle() {
    std::wstring title = g_engine.titleBase;
    if (!g_engine.scenePath.empty()) {
        const size_t cut = g_engine.scenePath.find_last_of(L"\\/");
        title += L" - " + g_engine.scenePath.substr(cut == std::wstring::npos ? 0 : cut + 1);
    }
    SetWindowTextW(g_engine.window, title.c_str());
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
    if (GetFileAttributesW(tbd::Utf8ToWide(file).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
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
    if (it != g_charSurfaces.end() && (it->first == file || GetFileAttributesW(tbd::Utf8ToWide(file.substr(0, file.find_last_of('.')) + ".skin_extra.png").c_str()) == INVALID_FILE_ATTRIBUTES))
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
bool EntityFitParts(int i, std::vector<tbd::FitPart>* out) {
    if (i < 0 || i >= (int)g_engine.scene.entities.size()) return false;
    const tbd::Entity& e = g_engine.scene.entities[i];
    if (e.mesh != tbd::MeshKind::Imported) return false;
    static std::map<std::string, std::vector<tbd::FitPart>> cache;
    auto it = cache.find(e.meshPath);
    if (it == cache.end()) {
        std::vector<tbd::FitPart> parts;
        const tbd::ImportedModel& model = ImportedModels()[e.meshPath];
        std::map<int, std::pair<std::vector<uint8_t>, std::pair<int, int>>> decoded;
        for (const tbd::ImportedPart& p : model.parts) {
            tbd::FitPart fp;
            fp.first = p.first;
            fp.count = p.count;
            fp.hair = p.hair;
            if (p.albedo >= 0 && p.albedo < (int)model.images.size()) {
                auto d = decoded.find(p.albedo);
                if (d == decoded.end()) {
                    std::vector<uint8_t> px;
                    int w = 0, h = 0;
                    tbd::DecodeImage(model.images[(size_t)p.albedo].data(), model.images[(size_t)p.albedo].size(), px, w, h);
                    d = decoded.emplace(p.albedo, std::make_pair(std::move(px), std::make_pair(w, h))).first;
                }
                fp.rgba = d->second.first;
                fp.w = d->second.second.first;
                fp.h = d->second.second.second;
            }
            parts.push_back(std::move(fp));
        }
        it = cache.emplace(e.meshPath, std::move(parts)).first;
    }
    *out = it->second;
    return !out->empty();
}

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
    ULONGLONG started = 0;
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
        if (SceneLoadActive()) Sleep(1);
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

void StopRenderer() {
    CancelSceneLoad();
    g_stream.Forget();
    g_vegetation.Forget();
    g_water.Forget();
    g_engine.animator.Clear();
    g_engine.meshes.clear();
    g_engine.matParts.clear();
    g_engine.sourceParts.clear();
    g_engine.meshBounds.clear();
    g_engine.instanced.clear();
    g_engine.partTextures.clear(); // texture ids die with the renderer
    g_engine.texturesByContent.clear(); surface_async::Reset();
    DecodedImages().clear();
    g_engine.skinSurface = tbd::kNoTexture;
    g_engine.skinSurfaceTried = false;
    g_charSurfaces.clear(); // per-character skin maps die with the renderer too
    g_engine.terrain.ForgetMeshes();
    g_engine.renderer.reset(); // before the window it renders into
    if (g_engine.viewportWindow) {
        DestroyWindow(g_engine.viewportWindow);
        g_engine.viewportWindow = nullptr;
    }
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static float g_dynamicScale = 0.0f; // set by UpdateDynamicResolution; 0 = TBD_SCALE alone

// TBD_SCALE: the internal render resolution as a fraction of the window (0.5..1.0, default 1.0). The renderer and the
// TAA jitter both size from this, through tbd::ScaledSize.
float RenderScale() {
    static const float scale = [] {
        char v[32] = {};
        if (tbd::platform::GetEnvA("TBD_SCALE", v, sizeof(v)) == 0) return 1.0f;
        char* end = nullptr;
        const float s = strtof(v, &end);
        return end == v ? 1.0f : tbd::ClampRenderScale(s);
    }();
    return g_dynamicScale > 0.0f ? g_dynamicScale : scale;
}

// TBD_DYNRES=<fps>: holds that frame rate by stepping the render scale, 0.7..1.0, after a second of evidence. Off by default:
// a step remakes the post targets, so TAA history and the probes restart, which shows for about half a second.
void UpdateDynamicResolution(tbd::Renderer& r, float dt) {
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
    }
    g_gui.SetClassic(!smooth);
    g_roadGpu.Forget();
    tbd::Log("Game GUI: %s", smooth ? "smooth (UI pipeline, SDF font)"
                                    : forceClassic ? "classic (TBD_GUI_CLASSIC=1)" : "classic (UI pipeline or font atlas unavailable)");
}

bool StartRenderer(Api api) {
    StopRenderer();
    int w, h;
    ViewportSize(w, h);
    g_engine.viewportWindow = CreateWindowExW(0, kViewportClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w, h,
                                              ViewportHost(), nullptr, g_engine.instance, nullptr);
    if (!g_engine.viewportWindow) return false;

    if (api == Api::D3D12) { // the first start after a shader change compiles them all: the window stays alive and says so
        std::atomic<int> done{0}, total{0};
        std::atomic<bool> stop{false};
        std::thread compile([&] { tbd::PrecompileD3D12Shaders(done, total, stop); done = total.load() + 1; });
        wchar_t was[256] = {};
        GetWindowTextW(g_engine.window, was, 256);
        ULONGLONG shown = 0;
        while (total == 0 || done <= total) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { g_engine.running = false; stop = true; PostQuitMessage(0); break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!g_engine.running) stop = true;
            const ULONGLONG now = tbd::platform::TickCountMs();
            if (done > 0 && now - shown > 200 && g_engine.window) {
                wchar_t title[320];
                swprintf(title, 320, L"%s - compiling shaders %d of %d (first start only)", was, done.load(), total.load());
                SetWindowTextW(g_engine.window, title);
                shown = now;
            }
            Sleep(10);
        }
        compile.join();
        if (g_engine.window) SetWindowTextW(g_engine.window, was);
        if (!g_engine.running) return false;
    }
    std::unique_ptr<tbd::Renderer> r = api == Api::D3D12 ? tbd::CreateD3D12Renderer() : tbd::CreateVulkanRenderer();
    if (r) r->SetRenderScale(RenderScale());
    if (!r || !r->Init(g_engine.viewportWindow, w, h, g_engine.validation)) {
        tbd::Log("%s: renderer failed to start", ApiName(api));
        DestroyWindow(g_engine.viewportWindow);
        g_engine.viewportWindow = nullptr;
        return false;
    }
    g_engine.renderer = std::move(r);
    g_engine.api = api;
    g_engine.vpW = w;
    g_engine.vpH = h;
    RebuildSceneMeshes();
    UploadUiFont(); // in-game GUI: smooth path and its font, or the classic fallback
    tbd::Log("%s: renderer started (%d x %d)", ApiName(api), w, h);
    return true;
}

void SwitchApi(Api api) {
    if (api == g_engine.api && g_engine.renderer) return;
    const Api previous = g_engine.api;
    if (!StartRenderer(api)) {
        MessageBoxW(g_engine.window, L"That graphics API could not be started on this machine; staying on the previous one.",
                    kWindowTitle, MB_ICONWARNING);
        StartRenderer(previous);
    }
}

// ------------------------------------------------------------ viewport pop-out

bool CreateFloatWindow() {
    RECT main;
    GetWindowRect(g_engine.window, &main);
    int x = main.left + Sx(60), y = main.top + Sx(60), w = Sx(1100), h = Sx(680);
    if (g_engine.floatRectValid) {
        x = g_engine.floatRect.left;
        y = g_engine.floatRect.top;
        w = g_engine.floatRect.right - g_engine.floatRect.left;
        h = g_engine.floatRect.bottom - g_engine.floatRect.top;
    }
    // Owned by the main window, so it stays in front of it and minimizes with it.
    g_engine.floatWindow = CreateWindowExW(0, kFloatClass, L"Viewport - TBD Engine  (F4 docks it back)",
                                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, g_engine.window, nullptr,
                                           g_engine.instance, nullptr);
    return g_engine.floatWindow != nullptr;
}

void DestroyFloatWindow() {
    if (!g_engine.floatWindow) return;
    GetWindowRect(g_engine.floatWindow, &g_engine.floatRect);
    g_engine.floatRectValid = true;
    DestroyWindow(g_engine.floatWindow); // its WM_DESTROY clears g_engine.floatWindow
}

void SetViewportFloating(bool floating) {
    if (floating == g_engine.viewportFloating) return;
    StopRenderer(); // takes the viewport window with it
    if (floating) {
        if (!CreateFloatWindow()) {
            StartRenderer(g_engine.api);
            return;
        }
        g_engine.viewportFloating = true;
        ShowWindow(g_engine.floatWindow, SW_SHOW);
    } else {
        g_engine.viewportFloating = false;
        DestroyFloatWindow();
    }
    Layout();
    if (!StartRenderer(g_engine.api)) { // could not rebuild in the new place: go back
        if (g_engine.viewportFloating) {
            g_engine.viewportFloating = false;
            DestroyFloatWindow();
        }
        Layout();
        StartRenderer(g_engine.api);
    }
    Layout();
    if (g_engine.viewportWindow) SetFocus(g_engine.viewportWindow);
    InvalidateRect(g_engine.window, nullptr, TRUE);
}

// ---------------------------------------------------------------- inspector

// Typed values from the inspector go straight onto the selected entity.
void OnInspectorEdit(const tbd::InspectorEdit& e, void*) {
    tbd::Entity* sel = g_engine.scene.Selected();
    if (!sel) return;
    if (e.set[0]) sel->position.x = e.value[0];
    if (e.set[1]) sel->position.y = e.value[1];
    if (e.set[2]) sel->position.z = e.value[2];
    if (e.set[3] || e.set[4] || e.set[5]) {
        tbd::Vec3 euler = tbd::EulerFromQuat(sel->rotation);
        if (e.set[3]) euler.x = e.value[3];
        if (e.set[4]) euler.y = e.value[4];
        if (e.set[5]) euler.z = e.value[5];
        sel->rotation = tbd::QuatFromEuler(euler);
    }
    if (e.set[6]) sel->scale.x = std::fmax(0.01f, e.value[6]);
    if (e.set[7]) sel->scale.y = std::fmax(0.01f, e.value[7]);
    if (e.set[8]) sel->scale.z = std::fmax(0.01f, e.value[8]);
}

// ---- scripts on the selected entity (Details > Scripts). The entity stores path + "key=value;key=value" overrides.
std::vector<std::pair<std::string, std::string>> ParseScriptProps(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const size_t eq = text.find('=', at);
        if (eq != std::string::npos && eq < end) out.push_back({text.substr(at, eq - at), text.substr(eq + 1, end - eq - 1)});
        at = end + 1;
    }
    return out;
}

void OnInspectorScriptEdit(const tbd::InspectorScriptEdit& e, void*) {
    tbd::Entity* sel = g_engine.scene.Selected();
    if (!sel) return;
    if (e.kind == tbd::InspectorScriptEdit::Add) {
        const std::vector<std::string> files = g_script.ListScripts();
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < files.size(); ++i) AppendMenuW(menu, MF_STRING, (UINT_PTR)i + 1, tbd::Utf8ToWide(files[i]).c_str());
        if (files.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No scripts in the project's Scripts folder");
        POINT pt;
        GetCursorPos(&pt);
        const int pick = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_engine.window, nullptr);
        DestroyMenu(menu);
        if (pick >= 1 && pick <= (int)files.size()) sel->scripts.push_back({files[pick - 1], ""});
        return;
    }
    if (e.script < 0 || e.script >= (int)sel->scripts.size()) return;
    if (e.kind == tbd::InspectorScriptEdit::Remove) {
        sel->scripts.erase(sel->scripts.begin() + e.script);
        return;
    }
    // SetProp: keep only text the declared type accepts; a value equal to the default is not stored.
    tbd::EntityScript& sc = sel->scripts[e.script];
    for (const auto& info : g_script.DeclaredProps(sc.path)) {
        if (info.name != e.prop) continue;
        std::string value = e.value;
        for (char& c : value)
            if (c == ';' || c == '=' || c == '|' || c == '\n' || c == '\r') c = ' ';
        if (info.type == "number") {
            char* end = nullptr;
            const double v = strtod(value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite(v)) return; // not a number: the field shows the old value again
            char buf[40];
            snprintf(buf, sizeof(buf), "%.9g", v);
            value = buf;
        } else if (info.type == "bool") {
            value = (value == "true" || value == "1" || value == "yes") ? "true" : "false";
        }
        auto props = ParseScriptProps(sc.props);
        props.erase(std::remove_if(props.begin(), props.end(), [&](const auto& p) { return p.first == e.prop; }), props.end());
        const bool same = value == info.defaultText || (info.type == "number" && atof(value.c_str()) == atof(info.defaultText.c_str()));
        if (!same) props.push_back({e.prop, value});
        sc.props.clear();
        for (const auto& p : props) sc.props += (sc.props.empty() ? "" : ";") + p.first + "=" + p.second;
        return;
    }
}

void UpdateInspectorScripts(const tbd::Entity* sel) {
    struct Cached { unsigned generation = ~0u; bool exists = false; std::vector<tbd::script::LuauHost::PropInfo> props; };
    static std::map<std::string, Cached> cache;
    std::vector<tbd::InspectorScript> list;
    if (sel) {
        for (const tbd::EntityScript& sc : sel->scripts) {
            tbd::InspectorScript out;
            out.path = sc.path;
            Cached& c = cache[sc.path];
            if (c.generation != g_script.Generation()) {
                c.exists = g_script.ScriptExists(sc.path);
                c.props = c.exists ? g_script.DeclaredProps(sc.path) : std::vector<tbd::script::LuauHost::PropInfo>();
                c.generation = g_script.Generation();
            }
            out.missing = !c.exists;
            const auto overrides = ParseScriptProps(sc.props);
            for (const auto& info : c.props) {
                tbd::InspectorScriptProp pr;
                pr.name = info.name;
                pr.type = info.type;
                pr.value = info.defaultText;
                for (const auto& o : overrides)
                    if (o.first == info.name) { pr.value = o.second; pr.overridden = true; }
                out.props.push_back(pr);
            }
            list.push_back(std::move(out));
        }
    }
    tbd::Inspector_ShowScripts(g_engine.inspector, sel != nullptr, list);
    static int lastDesired = 0;
    const int desired = tbd::Inspector_DesiredHeight(g_engine.inspector);
    if (desired != lastDesired) { lastDesired = desired; Layout(); } // the panel grows to show every script row
}

void UpdateInspector() {
    if (!g_engine.inspector) return;
    const tbd::Entity* sel = g_engine.scene.Selected();
    float values[9] = {};
    if (sel) {
        const tbd::Vec3 euler = tbd::EulerFromQuat(sel->rotation);
        const float v[9] = {sel->position.x, sel->position.y, sel->position.z, euler.x, euler.y,
                            euler.z, sel->scale.x, sel->scale.y, sel->scale.z};
        std::copy(v, v + 9, values);
    }
    tbd::Inspector_Show(g_engine.inspector, sel ? sel->name.c_str() : nullptr, values);
    UpdateInspectorScripts(sel);
}

// -------------------------------------------------------------- scene commands

std::wstring ScenesFolder() {
    const std::wstring dir = tbd::Join(g_engine.assetsRoot, L"Scenes");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool PickSceneFile(bool save, std::wstring& path) {
    wchar_t buf[tbd::platform::kMaxPath] = {};
    if (save) wcsncpy_s(buf, path.empty() ? L"Untitled.tbdscene" : path.c_str(), _TRUNCATE);
    const std::wstring dir = ScenesFolder();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_engine.window;
    ofn.lpstrFilter = L"TBD scene (*.tbdscene)\0*.tbdscene\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = tbd::platform::kMaxPath;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"tbdscene";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    path = buf;
    return true;
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
        MessageBoxW(g_engine.window, (L"Could not read this scene:\n" + path).c_str(), kWindowTitle, MB_ICONWARNING);
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
        if (!PickSceneFile(true, path)) return false;
    }
    if (!tbd::SaveSceneFile(path, g_engine.scene, g_engine.terrain.params)) {
        MessageBoxW(g_engine.window, L"The scene could not be saved there.", kWindowTitle, MB_ICONWARNING);
        return false;
    }
    g_engine.scenePath = path;
    UpdateTitle();
    if (g_engine.contentBrowser) tbd::ContentBrowser_Refresh(g_engine.contentBrowser);
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
    if (_wcsicmp(ext, tbd::kSceneExtension) == 0) LoadScene(path);
    else if (_wcsicmp(ext, L".obj") == 0 || _wcsicmp(ext, L".gltf") == 0 || _wcsicmp(ext, L".glb") == 0) PlaceImported(path);
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

#include "undo_scene.inl" // Ctrl+Z / Ctrl+Y: scene checkpoints and the undo popup

// ---- Character creator (src/character/creator.h): what the panel may touch. Made once, on the first frame.
void CharacterCreatorHost() {
    if (tbd::character::HostSet() || !g_engine.window) return;
    tbd::character::Host h;
    h.instance = g_engine.instance;
    h.parent = g_engine.window;
    h.entityCount = [] { return (int)g_engine.scene.entities.size(); };
    h.selectedIndex = [] { return g_engine.scene.selected < (int)g_engine.scene.entities.size() ? g_engine.scene.selected : -1; };
    h.entityName = [](int i) { return i >= 0 && i < (int)g_engine.scene.entities.size() ? g_engine.scene.entities[(size_t)i].name : std::string(); };
    h.entityPath = [](int i) { return i >= 0 && i < (int)g_engine.scene.entities.size() && g_engine.scene.entities[(size_t)i].mesh == tbd::MeshKind::Imported ? g_engine.scene.entities[(size_t)i].meshPath : std::string(); };
    h.entityMesh = [](int i, std::vector<tbd::Vertex>* out) { *out = EntityVertices(g_engine.scene.entities[(size_t)i]); return !out->empty(); };
    h.entityModel = [](int i) -> const tbd::ImportedModel* { const auto it = ImportedModels().find(g_engine.scene.entities[(size_t)i].meshPath); return it == ImportedModels().end() ? nullptr : &it->second; };
    h.setMesh = [](int i, const std::vector<tbd::Vertex>& v) { ReplaceEntityVertices(i, v, {}); };
    h.restoreMesh = [](int i) { CompleteSceneLoad(); if (!g_engine.renderer || i < 0 || i >= (int)g_engine.meshes.size()) return; DestroyEntityMesh(g_engine.meshes[(size_t)i]); g_engine.meshes[(size_t)i] = UploadEntityMesh(g_engine.scene.entities[(size_t)i]); };
    h.faceLandmarks = [](int i, std::vector<float>* out) {
        if (i < 0 || i >= (int)g_engine.scene.entities.size()) return false;
        std::vector<tbd::FitPart> parts, fitParts;
        std::vector<tbd::Vertex> fitVerts;
        const std::vector<tbd::Vertex> v = EntityVertices(g_engine.scene.entities[(size_t)i]);
        if (v.empty() || !EntityFitParts(i, &parts)) return false;
        tbd::WithoutHair(v, parts, fitVerts, fitParts);
        out->assign((size_t)tbd::kFaceLandmarks * 3, 0.0f);
        std::string err;
        const bool ok = tbd::FitFaceLandmarks(fitVerts, fitParts, (tbd::ExeDirectory() + L"\\models\\face").c_str(), out->data(), &err);
        if (!ok) tbd::Log("photo fit: %s", err.c_str());
        return ok;
    };
    h.sceneReady = [] { return !SceneLoadActive(); };
    h.viewportFloating = [] { return g_engine.viewportFloating; }; // Character Creator: the 2D view fills the freed space
    h.relayout = [] { Layout(); };
    // The same file imported again (its hair style changed on disk): drop every cache keyed by its path, then rebuild the mesh.
    h.reloadEntity = [](int i) {
        if (i < 0 || i >= (int)g_engine.scene.entities.size()) return;
        CompleteSceneLoad();
        const std::string path = g_engine.scene.entities[(size_t)i].meshPath;
        ImportedVertexCache().erase(path);
        ImportedModels().erase(path);
        DecodedImages().erase(path);
        g_engine.partTextures.erase(path);
        if (!g_engine.renderer || i >= (int)g_engine.meshes.size()) return;
        DestroyEntityMesh(g_engine.meshes[(size_t)i]);
        g_engine.meshes[(size_t)i] = UploadEntityMesh(g_engine.scene.entities[(size_t)i]);
    };
    h.setEntityPath = [](int i, const std::string& path) { g_engine.scene.entities[(size_t)i].meshPath = path; CompleteSceneLoad(); if (!g_engine.renderer || i >= (int)g_engine.meshes.size()) return; DestroyEntityMesh(g_engine.meshes[(size_t)i]); g_engine.meshes[(size_t)i] = UploadEntityMesh(g_engine.scene.entities[(size_t)i]); };
    h.addEntity = [](int from, const std::string& name, const std::string& path) { CompleteSceneLoad(); tbd::Entity e = from >= 0 ? g_engine.scene.entities[(size_t)from] : tbd::Entity{}; e.mesh = tbd::MeshKind::Imported; e.name = name; e.meshPath = path; g_engine.meshes.push_back(UploadEntityMesh(e)); g_engine.scene.entities.push_back(e); return (int)g_engine.scene.entities.size() - 1; };
    h.toWorld = [](int i, const float* p, float* out) { const tbd::Vec4 q = g_engine.scene.entities[(size_t)i].Model() * tbd::Vec4{p[0], p[1], p[2], 1.0f}; out[0] = q.x; out[1] = q.y; out[2] = q.z; };
    h.setEntityScale = [](int i, float s) { g_engine.scene.entities[(size_t)i].scale = {s, s, s}; };
    h.entityRay = [](int i, int x, int y, float* o, float* d) {
        if (i < 0 || i >= (int)g_engine.scene.entities.size()) return false;
        const tbd::Ray r = g_engine.camera.ScreenRay((float)x, (float)y, (float)g_engine.vpW, (float)g_engine.vpH);
        const tbd::Mat4 inv = tbd::Inverse(g_engine.scene.entities[(size_t)i].Model());
        const tbd::Vec4 a = inv * tbd::Vec4{r.origin.x, r.origin.y, r.origin.z, 1.0f}, b = inv * tbd::Vec4{r.dir.x, r.dir.y, r.dir.z, 0.0f};
        o[0] = a.x; o[1] = a.y; o[2] = a.z; d[0] = b.x; d[1] = b.y; d[2] = b.z;
        return true;
    };
    h.skinAlbedo = [](int i) {
        const auto it = i >= 0 && i < (int)g_engine.meshes.size() ? g_engine.matParts.find(g_engine.meshes[(size_t)i]) : g_engine.matParts.end();
        if (it != g_engine.matParts.end()) for (const MatPart& p : it->second) if (IsSkinSurface(p.surface) && p.albedo != tbd::kNoTexture) return p.albedo;
        return tbd::kNoTexture;
    };
    h.swapAlbedo = [](int i, tbd::TextureId from, tbd::TextureId to) {
        if (i < 0 || i >= (int)g_engine.meshes.size()) return;
        for (auto* parts : {&g_engine.matParts, &g_engine.sourceParts})
            if (const auto it = parts->find(g_engine.meshes[(size_t)i]); it != parts->end()) for (MatPart& p : it->second) if (p.albedo == from) p.albedo = to;
    };
    h.partAlbedo = [](int i, size_t first) {
        (void)first; // the eye parts are told apart by their flag, not by vertex offset (skinned meshes count indices)
        if (i < 0 || i >= (int)g_engine.meshes.size()) return tbd::kNoTexture;
        if (first >= SIZE_MAX - 15) { // SIZE_MAX - n: the n-th hair-card part (the brow maker's cards), in import order
            for (auto* parts : {&g_engine.sourceParts, &g_engine.matParts}) { // a replaced mesh (setMesh) has only matParts
                size_t n = SIZE_MAX - first;
                if (const auto it = parts->find(g_engine.meshes[(size_t)i]); it != parts->end())
                    for (const MatPart& p : it->second) if (p.hair && n-- == 0) return p.albedo;
            }
            return tbd::kNoTexture;
        }
        for (auto* parts : {&g_engine.sourceParts, &g_engine.matParts}) // merged parts lose the flag: the file's own ranges keep it
            if (const auto it = parts->find(g_engine.meshes[(size_t)i]); it != parts->end()) for (const MatPart& p : it->second) if (p.eye && p.albedo != tbd::kNoTexture) return p.albedo;
        return tbd::kNoTexture;
    };
    h.skinSurface = [](int i) {
        const auto it = i >= 0 && i < (int)g_engine.meshes.size() ? g_engine.matParts.find(g_engine.meshes[(size_t)i]) : g_engine.matParts.end();
        if (it != g_engine.matParts.end()) for (const MatPart& p : it->second) if (IsSkinSurface(p.surface)) return p.surface;
        return tbd::kNoTexture;
    };
    h.swapSurface = [](int i, tbd::TextureId from, tbd::TextureId to) {
        if (i < 0 || i >= (int)g_engine.meshes.size()) return;
        for (auto* parts : {&g_engine.matParts, &g_engine.sourceParts})
            if (const auto it = parts->find(g_engine.meshes[(size_t)i]); it != parts->end()) for (MatPart& p : it->second) if (p.surface == from) p.surface = to;
        const std::string key = "<paint>" + std::to_string(i); // IsSkinSurface must keep recognising the painted map as skin's
        g_charSurfaces.erase(key);
        bool known = to == SkinSurfaceMap();
        for (const auto& kv : g_charSurfaces) known |= kv.second == to;
        if (!known && to != tbd::kNoTexture) g_charSurfaces[key] = to;
    };
    h.skinSurfaceImage = [](int i, std::vector<uint8_t>* px, int* w, int* hh) {
        if (i < 0 || i >= (int)g_engine.scene.entities.size() || !px) return false;
        std::string dir = g_engine.scene.entities[(size_t)i].meshPath.substr(0, g_engine.scene.entities[(size_t)i].meshPath.find('#'));
        const size_t slash = dir.find_last_of("\\/");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        std::string bytes;
        if (dir.empty() || !tbd::ReadAllText(tbd::Utf8ToWide(dir + "skin_surface.png"), bytes))
            if (!tbd::ReadAllText(tbd::Join(g_engine.assetsRoot, L"skin\\skin_surface.png"), bytes)) return false;
        return tbd::DecodeImage((const uint8_t*)bytes.data(), bytes.size(), *px, *w, *hh);
    };
    h.createTexture = [](const uint8_t* rgba, int w, int hh) { return g_engine.renderer ? g_engine.renderer->CreateTexture(rgba, w, hh) : tbd::kNoTexture; };
    h.destroyTexture = [](tbd::TextureId t) { if (g_engine.renderer && t != tbd::kNoTexture) g_engine.renderer->DestroyTexture(t); };
    h.frame = [](const float* at, float distance) { g_engine.camera.target = {at[0], at[1], at[2]}; g_engine.camera.distance = distance; g_engine.camera.yaw = 0.0f; g_engine.camera.pitch = 0.05f; };
    tbd::character::SetHost(h);
}
// ---- end Character creator

// Mocap tools that act on the selected entity: bind a face clip to it, or let it go.
void OnToolAction(tbd::ToolId id) {
    tbd::Entity* sel = g_engine.scene.Selected();
    if (id == tbd::ToolId::FaceLive) { // toggle the live webcam; the selected entity (if any) follows the head
        if (tbd::FaceTab_IsOpen()) { // already open: close it (stops the webcam)
            tbd::FaceTab_Close();
            return;
        }
        if (!g_engine.faceDriver.live.Running()) {
            std::string error;
            if (!g_engine.faceDriver.live.Start(-1, nullptr, (tbd::ExeDirectory() + L"\\models\\face").c_str(), &error)) {
                Toast(error);
                return;
            }
        }
        tbd::FaceTabHost host;
        host.instance = g_engine.instance;
        host.parent = g_engine.window;
        host.live = &g_engine.faceDriver.live;
        host.relayout = [] { Layout(); };
        host.selectedIndex = [] { return g_engine.scene.selected < (int)g_engine.scene.entities.size() ? g_engine.scene.selected : -1; };
        host.entityName = [](int i) {
            return i >= 0 && i < (int)g_engine.scene.entities.size() ? g_engine.scene.entities[i].name : std::string();
        };
        host.entityMesh = [](int i, std::vector<tbd::Vertex>* out) {
            if (i < 0 || i >= (int)g_engine.scene.entities.size()) return false;
            *out = EntityVertices(g_engine.scene.entities[i]);
            tbd::character::MorphEntityVertices(i, *out); // the rig starts from the Character panel's face
            return !out->empty();
        };
        host.lidRelaxed = [](int i, std::vector<tbd::Vertex>* rest, std::vector<tbd::Vertex>* relaxed) {
            return i >= 0 && i < (int)g_engine.scene.entities.size() && LidRelaxedVertices(g_engine.scene.entities[i], rest, relaxed);
        };
        host.setMesh = [](int i, const std::vector<tbd::Vertex>& v, const std::vector<std::pair<size_t, size_t>>& ranges) { ReplaceEntityVertices(i, v, ranges); };
        host.restoreMesh = [](int i) {
            CompleteSceneLoad();
            if (!g_engine.renderer || i < 0 || i >= (int)g_engine.meshes.size()) return;
            DestroyEntityMesh(g_engine.meshes[i]);
            g_engine.meshes[i] = UploadEntityMesh(g_engine.scene.entities[i]);
        };
        host.driveHead = [](int i, bool on) {
            if (i < 0 || i >= (int)g_engine.scene.entities.size()) return;
            tbd::Entity& e = g_engine.scene.entities[i];
            if (on) g_engine.faceDriver.BindLive(e);
            else g_engine.faceDriver.Unbind(e);
        };
        host.overrideHeadPose = [](const float* pose, const float* neutral) {
            tbd::FaceDriver& d = g_engine.faceDriver;
            d.poseOverride = pose && neutral;
            if (d.poseOverride)
                for (int k = 0; k < 3; ++k) { d.overridePose[k] = pose[k]; d.overrideNeutral[k] = neutral[k]; }
        };
        host.modelDir = tbd::ExeDirectory() + L"\\models\\face";
        host.entityParts = [](int i, std::vector<tbd::FitPart>* out) { return EntityFitParts(i, out); };
        host.frameHead = [](int i, const float* mid, float faceWidth) { // face the head from the front, filling the live view
            if (i < 0 || i >= (int)g_engine.scene.entities.size()) return;
            const tbd::Vec4 q = g_engine.scene.entities[i].Model() * tbd::Vec4{mid[0], mid[1], mid[2], 1.0f};
            g_engine.camera.target = {q.x, q.y, q.z};
            g_engine.camera.distance = 3.0f * faceWidth;
            g_engine.camera.yaw = 0.0f; // eye on the +Z side, where the head faces
            g_engine.camera.pitch = 0.05f;
        };
        host.setMaxGraphics = [](bool on) { SetMaxGraphics(on); };
        host.maxGraphicsOn = [] { return g_maxGraphics; };
        host.setMirror =[](bool m) { g_engine.faceDriver.mirror = m; };
        host.stopCapture = [] { g_engine.faceDriver.StopLive(g_engine.scene); Toast("WEBCAM OFF"); };
        host.startVideoLog = [live = host.live] {
            wchar_t exe[tbd::platform::kMaxPath] = {};
            GetModuleFileNameW(nullptr, exe, tbd::platform::kMaxPath);
            if (!g_engine.renderer || !g_faceLog.Start(live, std::filesystem::path(exe).parent_path() / L"facelog")) return false;
            g_engine.renderer->SetFrameCapture(true);
            return true;
        };
        host.stopVideoLog = [] {
            if (g_engine.renderer) g_engine.renderer->SetFrameCapture(false);
            return g_faceLog.Stop();
        };
        if (!sel) // nothing selected: take the first imported mesh (the head, in a face scene)
            for (int i = 0; i < (int)g_engine.scene.entities.size(); ++i)
                if (g_engine.scene.entities[i].mesh == tbd::MeshKind::Imported) { g_engine.scene.selected = i; sel = &g_engine.scene.entities[i]; break; }
        tbd::FaceTab_Open(host);
        Toast(sel ? "FACE MOCAP LIVE: PRESS USE SELECTED" : std::string("FACE MOCAP LIVE: SELECT A HEAD, THEN USE SELECTED"));
        return;
    }
    if (!sel) {
        Toast("SELECT AN ENTITY FIRST");
        return;
    }
    if (id == tbd::ToolId::FaceDriveStop) {
        g_engine.faceDriver.Unbind(*sel);
        Toast("STOPPED DRIVING " + sel->name);
        return;
    }
    wchar_t buf[tbd::platform::kMaxPath] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_engine.window;
    ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = tbd::platform::kMaxPath;
    ofn.lpstrTitle = L"Drive the selected entity with a face clip";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string error;
    if (g_engine.faceDriver.Bind(*sel, buf, &error)) Toast("DRIVING " + sel->name + " WITH " + ToastName(buf));
    else Toast(error);
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
    const float speed = g_engine.flySpeed * (down(PK::Shift) ? 4.0f : 1.0f) * dt * tbd::character::CameraScale() * tbd::character::CameraScale(); // creator open: ~1/10 fly speed
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
bool AppHasFocus() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}
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
    const float fast = down(PK::Shift) ? 3.0f : 1.0f, k = tbd::character::CameraScale();
    tbd::Vec3 f = cam.Forward(); f.y = 0.0f;
    const float fl = std::sqrt(f.x * f.x + f.z * f.z);
    if (fl > 1e-4f) f = f * (1.0f / fl);
    tbd::Vec3 r = cam.Right(); r.y = 0.0f;
    const float pan = std::max(cam.distance, 0.5f) * 1.1f * fast * k * dt; // screen-relative: the same feel at every zoom
    cam.target = cam.target + (f * fw + r * rt) * pan;
    cam.yaw += turn * 1.6f * fast * k * dt;
    cam.pitch = std::clamp(cam.pitch + tilt * 0.9f * fast * k * dt, 0.05f, 1.5f);
}

void OnTransport(int action, void*); // defined with the toolbar hooks below

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

// Engine self test (--face-shot): poses the scene's head with set expressions and saved clip moments and writes what
// each renderer really drew to BMP files, with no webcam and no input, then quits. Only the engine's own frames are
// read, never the screen.
struct FaceShot {
    struct Case { std::string name; float w[tbd::kBlendshapes] = {}; float head[3] = {}; };
    bool active = false;
    std::wstring clipPath, dir;
    tbd::FaceRig rig;
    int target = -1;
    std::vector<Case> cases;
    size_t index = 0;
    bool video = false; // --face-video: every frame of the clip goes to an mp4, as the renderer drew it
    std::wstring videoPath;
    HANDLE encoder = nullptr, encoderInput = nullptr;
    int videoW = 0, videoH = 0;
    float videoFps = 30, faceMid[3] = {}, faceWidth = 1;
    int shot = 0, wait = 0;
    float mouth[3] = {}, eyeY = 0;
    double poseMs = 0, uploadMs = 0;
    int posed = 0;
} g_shot;

void FaceShotStart() {
    FaceShot& fs = g_shot;
    for (int i = 0; i < (int)g_engine.scene.entities.size() && fs.target < 0; ++i)
        if (g_engine.scene.entities[i].mesh == tbd::MeshKind::Imported) fs.target = i;
    std::vector<tbd::Vertex> v;
    std::vector<tbd::FitPart> parts;
    float lm[tbd::kFaceLandmarks * 3];
    std::string err;
    if (fs.target < 0 || !g_engine.renderer) { tbd::Log("face shot: no imported mesh"); PostQuitMessage(1); return; }
    v = EntityVertices(g_engine.scene.entities[fs.target]);
    EntityFitParts(fs.target, &parts);
    std::vector<tbd::Vertex> fitVerts;
    std::vector<tbd::FitPart> fitParts;
    tbd::WithoutHair(v, parts, fitVerts, fitParts);
    if (!tbd::FitFaceLandmarks(fitVerts, fitParts, (tbd::ExeDirectory() + L"\\models\\face").c_str(), lm, &err)) { tbd::Log("face shot: %s", err.c_str()); PostQuitMessage(1); return; }
    const tbd::RigExtras ex = tbd::RigExtrasFromParts(parts);
    fs.rig.Init(v, lm, &ex);
    {
        std::vector<tbd::Vertex> rest, relaxed;
        if (LidRelaxedVertices(g_engine.scene.entities[fs.target], &rest, &relaxed)) fs.rig.SetLidRelax(rest, relaxed);
        tbd::Log("face shot: closed-lid shape %s", relaxed.empty() ? "none" : rest.size() == relaxed.size() ? "applied" : "does not line up with the head");
    }
    fs.mouth[0] = (lm[13 * 3] + lm[14 * 3]) * 0.5f;
    fs.mouth[1] = (lm[13 * 3 + 1] + lm[14 * 3 + 1]) * 0.5f;
    fs.mouth[2] = (lm[13 * 3 + 2] + lm[14 * 3 + 2]) * 0.5f;
    fs.eyeY = (lm[159 * 3 + 1] + lm[386 * 3 + 1]) * 0.5f;
    fs.faceWidth = fs.rig.FaceWidth();
    for (int k = 0; k < 3; ++k) fs.faceMid[k] = lm[6 * 3 + k];
    auto add = [&](const char* name, std::initializer_list<std::pair<int, float>> vals) {
        FaceShot::Case c;
        c.name = name;
        for (const auto& p : vals) c.w[p.first] = p.second;
        fs.cases.push_back(c);
    };
    if (!fs.video) {
        add("rest", {});
        add("jaw", {{tbd::BS_jawOpen, 1}});
        add("smile", {{tbd::BS_mouthSmileLeft, 1}, {tbd::BS_mouthSmileRight, 1}, {tbd::BS_mouthUpperUpLeft, 0.9f}, {tbd::BS_mouthUpperUpRight, 0.9f}});
        add("pucker", {{tbd::BS_mouthPucker, 1}});
        for (int v = 0; v < tbd::kLipVisemes; ++v) { // the lip sync's mouth shapes, at full strength
            static const char* const kNames[tbd::kLipVisemes] = {"lip_a", "lip_i", "lip_u", "lip_e", "lip_o", "lip_s", "lip_mbp", "lip_fv"};
            add(kNames[v], {});
            float vis[tbd::kLipVisemes] = {};
            vis[v] = 1;
            tbd::ApplyLips(vis, 1, fs.cases.back().w);
        }
        add("blink", {{tbd::BS_eyeBlinkLeft, 1}, {tbd::BS_eyeBlinkRight, 1}});
        add("squint", {{tbd::BS_eyeSquintLeft, 1}, {tbd::BS_eyeSquintRight, 1}});
        add("wide", {{tbd::BS_eyeWideLeft, 1}, {tbd::BS_eyeWideRight, 1}});
        const auto addHead = [&](const char* name, float pitch, float yaw, float roll) {
            add(name, {});
            FaceShot::Case& c = fs.cases.back();
            c.head[0] = pitch; c.head[1] = yaw; c.head[2] = roll;
        };
        addHead("turn", 0, 90, 0); // past the limit on purpose: the rig must stop it
        addHead("tilt", 0, 0, 60);
        addHead("nod", 60, 0, 0);
        addHead("jaw_side", 0, 30, 0); // how deep the teeth sit shows from the side
        fs.cases.back().w[tbd::BS_jawOpen] = 0.7f;
        addHead("lip_a_side", 0, 30, 0);
        { const float vis[tbd::kLipVisemes] = {1}; tbd::ApplyLips(vis, 1, fs.cases.back().w); }
    }
    tbd::FaceClip clip;
    if (!fs.clipPath.empty() && clip.Load(fs.clipPath.c_str())) {
        std::vector<std::vector<float>> calib(tbd::kBlendshapes);
        int n = 0;
        for (const tbd::FaceFrame& f : clip.frames) {
            if (!f.valid) continue;
            for (int k = 0; k < tbd::kBlendshapes; ++k) calib[k].push_back(f.bs[k]);
            if (++n >= 45) break;
        }
        float base[tbd::kBlendshapes] = {};
        for (int k = 0; k < tbd::kBlendshapes; ++k) {
            if (calib[k].empty()) continue;
            std::nth_element(calib[k].begin(), calib[k].begin() + calib[k].size() / 2, calib[k].end());
            base[k] = calib[k][calib[k].size() / 2];
        }
        if (fs.video) {
            tbd::FaceRetarget retarget;
            double prev = 0, first = -1, last = 0;
            for (const tbd::FaceFrame& f : clip.frames) {
                FaceShot::Case c;
                if (f.valid) {
                    for (int k = 0; k < tbd::kBlendshapes; ++k) c.w[k] = std::clamp((f.bs[k] - base[k]) / std::max(1.0f - base[k], 0.5f), 0.0f, 1.0f);
                    retarget.Apply(c.w, first < 0 ? 0.0f : (float)(f.time - prev));
                    if (first < 0) first = f.time;
                    prev = last = f.time;
                }
                fs.cases.push_back(c);
            }
            if (last > first && first >= 0) fs.videoFps = (float)(fs.cases.size() / (last - first));
        }
        for (const double t : {1.70, 3.40, 5.10, 9.03}) {
            if (fs.video) break;
            const tbd::FaceFrame* best = nullptr;
            for (const tbd::FaceFrame& f : clip.frames)
                if (f.valid && (!best || std::abs(f.time - t) < std::abs(best->time - t))) best = &f;
            if (!best) continue;
            FaceShot::Case c;
            char nm[32];
            snprintf(nm, sizeof(nm), "at_%.2f", t);
            c.name = nm;
            for (int k = 0; k < tbd::kBlendshapes; ++k) c.w[k] = std::clamp((best->bs[k] - base[k]) / std::max(1.0f - base[k], 0.5f), 0.0f, 1.0f);
            fs.cases.push_back(c);
        }
    }
    g_engine.scene.selected = -1;
    g_engine.faceDriver.Unbind(g_engine.scene.entities[fs.target]); // a clip bound in the scene would turn the head
    g_engine.scene.entities[fs.target].rotation = {0, 0, 0, 1};
    g_engine.renderer->SetFrameCapture(true);
    fs.active = true;
}

// Called after every presented frame: a pose is shown for a few frames, written from the head and from the mouth, then the next.
void FaceShotStep(tbd::Renderer& r) {
    FaceShot& fs = g_shot;
    static std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    const bool got = r.CapturedFrame(pixels, w, h);
    if (fs.index >= fs.cases.size()) {
        if (fs.encoder) {
            CloseHandle(fs.encoderInput);
            WaitForSingleObject(fs.encoder, 120000);
            CloseHandle(fs.encoder);
        }
        if (fs.posed) tbd::Log("face shot: Pose %.2f ms, mesh upload %.2f ms per pose, %d poses", fs.poseMs / fs.posed, fs.uploadMs / fs.posed, fs.posed);
        r.SetFrameCapture(false);
        fs.active = false;
        DestroyWindow(g_engine.window);
        return;
    }
    const tbd::Entity& e = g_engine.scene.entities[fs.target];
    if (fs.wait == 0) { // set up this pose and view
        std::vector<tbd::Vertex> posed;
        int64_t t0, t1, t2, hz;
        hz = tbd::platform::TickFrequency();
        t0 = tbd::platform::Ticks();
        fs.rig.Pose(fs.cases[fs.index].w, posed, fs.cases[fs.index].head);
        t1 = tbd::platform::Ticks();
        ReplaceEntityVertices(fs.target, posed, fs.rig.PartRanges());
        t2 = tbd::platform::Ticks();
        fs.poseMs += 1000.0 * (t1 - t0) / hz;
        fs.uploadMs += 1000.0 * (t2 - t1) / hz;
        ++fs.posed;
        g_engine.camera.yaw = 0.0f;
        g_engine.camera.pitch = 0.05f;
        if (fs.video || fs.shot == 0) {
            const tbd::Vec4 q = e.Model() * tbd::Vec4{fs.faceMid[0], fs.faceMid[1], fs.faceMid[2], 1.0f};
            g_engine.camera.target = {q.x, q.y, q.z};
            g_engine.camera.distance = (fs.video ? 2.6f : 3.0f) * fs.faceWidth;
        } else {
            const bool eyes = fs.shot == 2;
            const tbd::Vec4 q = e.Model() * tbd::Vec4{fs.mouth[0], eyes ? fs.eyeY : fs.mouth[1], fs.mouth[2], 1.0f};
            g_engine.camera.target = {q.x, q.y, q.z};
            g_engine.camera.distance = (eyes ? 1.3f : 1.05f) * fs.faceWidth;
        }
    }
    if (++fs.wait < 4 || !got || w <= 0) return;
    fs.wait = 0;
    if (fs.video) {
        if (!fs.encoder) { // ffmpeg reads raw frames on stdin, with no console window
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            HANDLE read = nullptr;
            if (!CreatePipe(&read, &fs.encoderInput, &sa, 1 << 24)) { PostQuitMessage(1); return; }
            SetHandleInformation(fs.encoderInput, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = read;
            si.hStdOutput = si.hStdError = INVALID_HANDLE_VALUE;
            wchar_t rate[32];
            swprintf(rate, 32, L"%.3f", fs.videoFps);
            std::wstring cmd = L"ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" -r " + rate +
                               L" -i - -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 16 -pix_fmt yuv420p \"" + fs.videoPath + L"\"";
            PROCESS_INFORMATION pi{};
            const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(read);
            if (!ok) { tbd::Log("face video: cannot start ffmpeg"); PostQuitMessage(1); return; }
            CloseHandle(pi.hThread);
            fs.encoder = pi.hProcess;
            fs.videoW = w;
            fs.videoH = h;
        }
        if (w == fs.videoW && h == fs.videoH) {
            DWORD done = 0;
            WriteFile(fs.encoderInput, pixels.data(), (DWORD)pixels.size(), &done, nullptr);
        }
        ++fs.index;
        return;
    }
    wchar_t path[tbd::platform::kMaxPath];
    swprintf(path, tbd::platform::kMaxPath, L"%s\\%S_%s_%s.bmp", fs.dir.c_str(), r.Name(), std::wstring(fs.cases[fs.index].name.begin(), fs.cases[fs.index].name.end()).c_str(), fs.shot == 0 ? L"head" : fs.shot == 1 ? L"mouth" : L"eyes");
    if (FILE* f = tbd::platform::WOpen(path, L"wb")) {
        tbd::BmpFileHeader fh{};
        tbd::BmpInfoHeader bi{sizeof(bi), w, -h, 1, 32, tbd::kBmpRgb};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi);
        fh.bfSize = fh.bfOffBits + (uint32_t)pixels.size();
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(pixels.data(), 1, pixels.size(), f);
        fclose(f);
    }
    if (++fs.shot >= 3) {
        fs.shot = 0;
        ++fs.index;
    }
}

// ------------------------------------------------------------------- live control (remote_control.h)
// Each request runs here between frames, on the editor's thread, exactly as the menus and gizmos would change the scene.
// Commands and arguments: gameenginetools\engine_mcp\README.md. Off with TBD_NOREMOTE=1; never in a TBD_SHOT / TBD_CAPTURE run.
void OnCommand(UINT id);
extern int g_gfxQuality;
struct RemoteShot { tbd::remote::Request* request = nullptr; std::string path; int wait = 0; };
static RemoteShot g_remoteShot;
static double g_remoteFrameMs = 0.0;

static std::wstring RemoteWide(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
    return w;
}
static std::string RemoteNum(float v) { char b[32]; snprintf(b, sizeof(b), "%.4g", v); return b; }
static std::string RemoteEntityLine(int i) {
    const tbd::Entity& e = g_engine.scene.entities[i];
    const tbd::Vec3 eu = tbd::EulerFromQuat(e.rotation);
    std::string s = std::to_string(i) + "|" + e.name + "|" + tbd::MeshKindName(e.mesh) + "|pos " + RemoteNum(e.position.x) + " " + RemoteNum(e.position.y) + " " +
                    RemoteNum(e.position.z) + "|rot " + RemoteNum(eu.x) + " " + RemoteNum(eu.y) + " " + RemoteNum(eu.z) + "|scale " + RemoteNum(e.scale.x) + " " +
                    RemoteNum(e.scale.y) + " " + RemoteNum(e.scale.z) + "|color " + RemoteNum(e.color.x) + " " + RemoteNum(e.color.y) + " " + RemoteNum(e.color.z);
    if (!e.meshPath.empty()) s += "|" + e.meshPath;
    return s;
}
static bool RemoteVec(const std::string& text, tbd::Vec3& v) { float x, y, z; if (sscanf(text.c_str(), "%f %f %f", &x, &y, &z) != 3) return false; v = {x, y, z}; return true; }

bool IndustryDemo(float x, float z, std::string& out); // city_industry.inl
bool IndustryDump(const std::string& path, std::string& out);
// The command's reply, or false with the reason. A screenshot answers later, from RemoteShotAfterFrame.
static bool RemoteRun(tbd::remote::Request* q, std::string& out) {
    const std::vector<std::string>& a = q->args;
    const std::string& cmd = a[0];
    auto num = [&](size_t i, float fallback) { return i < a.size() && !a[i].empty() ? (float)atof(a[i].c_str()) : fallback; };
    auto entity = [&](size_t i) -> int {
        if (i >= a.size()) return -1;
        const std::string& key = a[i];
        if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) {
            const int idx = atoi(key.c_str());
            return idx < (int)g_engine.scene.entities.size() ? idx : -1;
        }
        for (int k = 0; k < (int)g_engine.scene.entities.size(); ++k)
            if (g_engine.scene.entities[k].name == key) return k;
        return -1;
    };
    tbd::OrbitCamera& cam = g_engine.camera;
    auto cameraLine = [&] {
        return "target " + RemoteNum(cam.target.x) + " " + RemoteNum(cam.target.y) + " " + RemoteNum(cam.target.z) + " distance " + RemoteNum(cam.distance) +
               " yaw " + RemoteNum(cam.yaw * 57.29578f) + " pitch " + RemoteNum(cam.pitch * 57.29578f);
    };
    if (cmd == "ping") { out = "pong"; return true; }
    if (cmd.rfind("script.", 0) == 0) return tbd::script::RemoteScriptCommand(g_script, a, out); // script.list | script.call | script.eval
    if (cmd == "info") {
        out = "scene " + tbd::WideToUtf8(g_engine.scenePath) + "\nentities " + std::to_string(g_engine.scene.entities.size()) + "\nselected " +
              std::to_string(g_engine.scene.selected) + "\napi " + (g_engine.api == Api::D3D12 ? "d3d12" : "vulkan") + "\nquality " + std::to_string(g_gfxQuality) +
              "\nframe_ms " + RemoteNum((float)g_remoteFrameMs) + "\nloading " + (SceneLoadActive() ? "yes" : "no") + "\nhour " + RemoteNum(g_engine.sun.hour) +
              "\ncamera " + cameraLine() + "\nviewport " + std::to_string(g_engine.vpW) + "x" + std::to_string(g_engine.vpH);
        return true;
    }
    if (cmd == "list") { // list|filter|max
        const std::string filter = a.size() > 1 ? a[1] : "";
        const int max = a.size() > 2 && !a[2].empty() ? atoi(a[2].c_str()) : 200;
        int shown = 0, matched = 0;
        for (int i = 0; i < (int)g_engine.scene.entities.size(); ++i) {
            if (!filter.empty() && g_engine.scene.entities[i].name.find(filter) == std::string::npos) continue;
            if (++matched <= max) { out += RemoteEntityLine(i) + "\n"; ++shown; }
        }
        out += std::to_string(shown) + " of " + std::to_string(matched) + " matching";
        return true;
    }
    if (cmd == "get") {
        const int i = entity(1);
        if (i < 0) { out = "no entity " + (a.size() > 1 ? a[1] : std::string()); return false; }
        out = RemoteEntityLine(i);
        return true;
    }
    if (cmd == "spawn") { // spawn|Cube/Plane/Sphere/Tree/Rock or a .glb/.gltf/.obj path|x y z|name
        CompleteSceneLoad();
        tbd::Entity e;
        const std::string kind = a.size() > 1 && !a[1].empty() ? a[1] : "Cube";
        const tbd::MeshKind kinds[] = {tbd::MeshKind::Cube, tbd::MeshKind::Plane, tbd::MeshKind::Sphere, tbd::MeshKind::Tree, tbd::MeshKind::Rock};
        bool primitive = false;
        for (tbd::MeshKind k : kinds)
            if (_stricmp(kind.c_str(), tbd::MeshKindName(k)) == 0) { e.mesh = k; primitive = true; }
        if (!primitive) {
            e.mesh = tbd::MeshKind::Imported;
            e.meshPath = kind;
            if (ImportedVertices(e.meshPath).empty()) { out = "could not load mesh " + kind; return false; }
            e.color = {1, 1, 1};
        }
        e.position = cam.target;
        if (a.size() > 2 && !a[2].empty() && !RemoteVec(a[2], e.position)) { out = "position must be \"x y z\""; return false; }
        e.name = a.size() > 3 && !a[3].empty() ? a[3] : std::string(primitive ? tbd::MeshKindName(e.mesh) : "Mesh") + " " + std::to_string(g_engine.nextEntityId++);
        const int idx = g_engine.scene.Add(e);
        g_engine.meshes.push_back(UploadEntityMesh(e));
        out = RemoteEntityLine(idx);
        return true;
    }
    if (cmd == "set") { // set|entity|pos/rot/scale/color/name|value
        const int i = entity(1);
        if (i < 0 || a.size() < 4) { out = "usage: set|entity|pos, rot, scale, color or name|value"; return false; }
        tbd::Entity& e = g_engine.scene.entities[i];
        float x = 0, y = 0, z = 0;
        const int n = sscanf(a[3].c_str(), "%f %f %f", &x, &y, &z);
        const std::string& field = a[2];
        if (field == "name") e.name = a[3];
        else if (field == "pos" && n == 3) e.position = {x, y, z};
        else if (field == "rot" && n == 3) e.rotation = tbd::QuatFromEuler({x, y, z});
        else if (field == "rot" && n == 1) e.rotation = tbd::QuatFromEuler({0, x, 0});
        else if (field == "scale" && n == 3) e.scale = {x, y, z};
        else if (field == "scale" && n == 1) e.scale = {x, x, x};
        else if (field == "color" && n == 3) e.color = {x, y, z};
        else { out = "bad field or value: " + field + " " + a[3]; return false; }
        out = RemoteEntityLine(i);
        return true;
    }
    if (cmd == "delete") {
        const int i = entity(1);
        if (i < 0) { out = "no entity"; return false; }
        const std::string name = g_engine.scene.entities[i].name;
        g_engine.scene.selected = i;
        DeleteSelected();
        out = "deleted " + name + " (later indices moved down by one)";
        return true;
    }
    if (cmd == "select") {
        const int i = entity(1);
        g_engine.scene.selected = i;
        out = i < 0 ? "selection cleared" : RemoteEntityLine(i);
        return true;
    }
    if (cmd == "focus") { // focus|entity|distance
        const int i = entity(1);
        if (i < 0) { out = "no entity"; return false; }
        g_engine.scene.selected = i;
        FrameSelected();
        if (a.size() > 2 && !a[2].empty()) cam.distance = std::max(0.2f, num(2, cam.distance));
        out = cameraLine();
        return true;
    }
    if (cmd == "camera") { // camera|tx ty tz|distance|yaw deg|pitch deg; an empty field keeps the current value
        if (a.size() > 1 && !a[1].empty() && !RemoteVec(a[1], cam.target)) { out = "target must be \"x y z\""; return false; }
        cam.distance = std::max(0.2f, num(2, cam.distance));
        cam.yaw = num(3, cam.yaw * 57.29578f) / 57.29578f;
        cam.pitch = std::clamp(num(4, cam.pitch * 57.29578f) / 57.29578f, -1.55f, 1.55f);
        out = cameraLine();
        return true;
    }
    if (cmd == "time") {
        g_engine.sun.hour = std::fmod(std::max(0.0f, num(1, g_engine.sun.hour)), 24.0f);
        g_engine.sun.animate = false;
        out = "hour " + RemoteNum(g_engine.sun.hour);
        return true;
    }
    if (cmd == "quality") { g_gfxQuality = std::clamp((int)num(1, 0), 0, 2); out = "quality " + std::to_string(g_gfxQuality); return true; }
    if (cmd == "api") {
        SwitchApi(a.size() > 1 && a[1] == "vulkan" ? Api::Vulkan : Api::D3D12);
        out = g_engine.api == Api::D3D12 ? "d3d12" : "vulkan";
        return true;
    }
    if (cmd == "open") {
        if (a.size() < 2 || a[1].empty()) { out = "usage: open|path"; return false; }
        if (!LoadScene(RemoteWide(a[1]))) { out = "could not open " + a[1]; return false; }
        out = "opened, " + std::to_string(g_engine.scene.entities.size()) + " entities (meshes keep streaming in: see info, loading)";
        return true;
    }
    if (cmd == "save") { // save|path (empty: the scene's own file)
        CompleteSceneLoad();
        const std::wstring path = a.size() > 1 && !a[1].empty() ? RemoteWide(a[1]) : g_engine.scenePath;
        if (path.empty()) { out = "the scene has no file yet: give a path"; return false; }
        if (!tbd::SaveSceneFile(path, g_engine.scene, g_engine.terrain.params)) { out = "could not write " + tbd::WideToUtf8(path); return false; }
        g_engine.scenePath = path;
        UpdateTitle();
        out = "saved " + tbd::WideToUtf8(path);
        return true;
    }
    if (cmd == "reload_shaders") { OnCommand(IDM_RELOADSHADERS); out = "shaders reloaded (engine.log lists any errors)"; return true; }
    if (cmd == "rebuild_game") { OnCommand(IDM_REBUILDGAME); out = "gameplay DLL rebuild started"; return true; }
    if (cmd == "industry_demo") { // industry_demo|x z: one site of each industry in a row, areas drawn, for screenshots
        float x = cam.target.x, z = cam.target.z;
        if (a.size() > 1) sscanf(a[1].c_str(), "%f %f", &x, &z);
        return IndustryDemo(x, z, out);
    }
    if (cmd == "industry_dump") return IndustryDump(a.size() > 1 ? a[1] : std::string("industry_models_data.js"), out); // industry_dump|path.js
    if (cmd == "screenshot") { // screenshot|path.bmp|frames to wait: answered once that frame is drawn
        if (g_remoteShot.request) { out = "a screenshot is already pending"; return false; }
        g_remoteShot.request = q;
        g_remoteShot.path = a.size() > 1 && !a[1].empty() ? a[1] : tbd::WideToUtf8(tbd::ExeDirectory()) + "\\remote_shot.bmp";
        g_remoteShot.wait = std::clamp((int)num(2, 3), 1, 600); // let a change settle (TAA); more for probe lighting
        return true;
    }
    out = "unknown command " + cmd + " (ping info list get spawn set delete select focus camera time quality api open save reload_shaders rebuild_game screenshot script.list script.call script.eval)";
    return false;
}

static void RemotePump() {
    static const bool on = !tbd::platform::GetEnvA("TBD_NOREMOTE", nullptr, 0) && !tbd::platform::GetEnvA("TBD_SHOT", nullptr, 0) &&
                           !tbd::platform::GetEnvA("TBD_CAPTURE", nullptr, 0);
    if (!on) return;
    tbd::remote::Server& server = tbd::remote::Instance();
    server.Start();
    static int64_t last = {}, hz = {};
    int64_t now;
    now = tbd::platform::Ticks();
    if (!hz) hz = tbd::platform::TickFrequency();
    if (last) g_remoteFrameMs = g_remoteFrameMs * 0.9 + 0.1 * (double)(now - last) * 1000.0 / (double)hz;
    last = now;
    for (int k = 0; k < 16; ++k) {
        tbd::remote::Request* q = server.Next();
        if (!q) break;
        std::string out;
        bool ok = false;
        try { ok = RemoteRun(q, out); } catch (...) { out = "the command threw"; }
        if (q != g_remoteShot.request) server.Finish(q, ok, out);
    }
}

// Before EndFrame: whether this frame is the one to read back.
static bool RemoteShotThisFrame() { return g_remoteShot.request && --g_remoteShot.wait <= 0; }
static void RemoteShotAfterFrame(tbd::Renderer& r) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    bool ok = r.CapturedFrame(px, w, h);
    r.SetFrameCapture(false);
    if (ok) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        tbd::BmpFileHeader fh = {0x4D42, (uint32_t)(sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader) + px.size()), 0, 0, sizeof(tbd::BmpFileHeader) + sizeof(tbd::BmpInfoHeader)};
        tbd::BmpInfoHeader ih = {sizeof(ih), w, -h, 1, 32, tbd::kBmpRgb};
        FILE* f = tbd::platform::WOpen(RemoteWide(g_remoteShot.path).c_str(), L"wb");
        ok = f != nullptr;
        if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
    }
    tbd::remote::Instance().Finish(g_remoteShot.request, ok, ok ? g_remoteShot.path + " " + std::to_string(w) + "x" + std::to_string(h) : "could not read back or write the frame");
    g_remoteShot = {};
}

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
        if (!v.empty()) g_roadGpu.mesh[m] = r.CreateMesh(v.data(), (uint32_t)v.size(), tbd::Topology::Triangles);
    }
    if (g_roadGpu.arrows != tbd::kInvalidMesh) r.RetireMesh(g_roadGpu.arrows);
    g_roadGpu.arrows = out.arrows.empty() ? tbd::kInvalidMesh : r.CreateMesh(out.arrows.data(), (uint32_t)out.arrows.size(), tbd::Topology::Triangles);
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
#include "city_stress.inl"   // TBD_CITY_STRESS=<n>: a road grid with n zoned lots and the bad cases, for render baselines
#include "city_buildings.inl" // zoned buildings per CityCell: instanced near, chunk bakes and volumes or merged proxies far (Track R)

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
    g_gui.BeginFrame(g_guiInput.Take((float)g_engine.vpW, (float)g_engine.vpH, DpiScale() * g_guiScaleSel, guiDt));
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
    const tbd::gui::TerrainAction action = tbd::gui::TerrainToolbar(g_gui, state, status, 4.0f + 106.0f, ease);
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
    g_guiScaleSel = tbd::gui::HudSizeValues()[std::clamp(hud.sizeIdx, 0, 4)];
    const float hudH = tbd::gui::CityHudFrame(g_gui, hud, g_clock, 4.0f);
    SimHudRead(hud); // the Budget panel's tax rates and funding into the city sim
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
        static ULONGLONG lastFrame = 0;
        const ULONGLONG t = tbd::platform::TickCountMs();
        if (lastFrame && t - lastFrame >= (ULONGLONG)hitchMs) tbd::Log("Hitch: %llu ms between frames at %llu ms", t - lastFrame, t);
        lastFrame = t;
    }
    SceneLoadTick(16.0);
    FrameMark("sceneload");
    surface_async::Pump();
    FrameMark("surface-pump");
    ground_bake::Tick();
    FrameMark("ground-bake");
    if (!g_engine.running || !g_engine.renderer) return;
    if (g_hotShaders.Tick(g_engine.renderer.get(), g_engine.api == Api::Vulkan)) { // new SPIR-V: the Vulkan renderer starts again around the scene
        tbd::Log(StartRenderer(Api::Vulkan) ? "Vulkan: shaders reloaded" : "Vulkan: restart after shader reload failed");
        if (!g_engine.renderer) StartRenderer(Api::D3D12);
        if (!g_engine.renderer) return;
    }
    FrameMark("hot-reload");
    RemotePump();
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
    FrameMark("sim-sync");
    CityStressSync(); // TBD_CITY_STRESS only
    CityChunkCheckSync(); // TBD_CHUNK_CHECK only
    // TBD_PLAYTEST: Play starts itself, on a fixed clock; see PlayerScript.
    static bool playtestTried = false;
    if (!playtestTried && !g_engine.scenePath.empty() && !SceneLoadActive()) { // the scene is open
        playtestTried = true;
        if (g_playtest.Start()) {
            g_playtest.OpenTrace(r.Name());
            OnTransport(0, nullptr);
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
        if (g_play.Lost()) OnTransport(2, nullptr); // alt-tab or a dialog: back to the editor, mouse released
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
    g_game.Tick(g_play.active ? dt : 0.0f, g_play.active);
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
    g_engine.faceDriver.Update(dt, g_engine.scene);
    tbd::FaceTab_Tick();
    CharacterCreatorHost(); tbd::character::Tick(); // Character panel: at most one re-deform per frame; TBD_CHARPRESET
    scene_undo::Checkpoint(); // an editor edit that just finished becomes one undo step
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
                if (++step == (int)std::size(steps)) { fclose(out); out = nullptr; PostQuitMessage(0); step = 0; }
            }
            featureOff = steps[step].bits;
        }
    }
    const int qualityOff = g_gfxQuality > 0 ? (1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 512 | 1024 | (g_gfxQuality > 1 ? 256 : 0)) : 0;
    light.lightDebug[2] = (float)(featureOff | qualityOff); // the spare slot; mesh shaders only
    light.lightDebug[3] = g_engine.scene.ambientFill + 1.0f; // +1: an unset 0 reads as the default (lighting.hlsli AmbientFill)
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
    if (captureFramesLeft > 0 && (SceneLoadActive() || !g_stream.CaptureMayStart() || !shotTour.Done() || !ground_bake::Settled() || tbd::character::Busy())) captureFramesLeft = benchFrames + 61 + settleFrames;

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
                if (parts[k].hair && tbd::character::HideHeadHair()) partFlags = 0; // Hair Maker: the clay is being worked
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
            if (g_roadGpu.mesh[m] == tbd::kInvalidMesh) continue;
            const tbd::FrameCounts roadMark = r.Counts();
            r.SetMaterial(g_roadGpu.albedo[m], g_roadGpu.normal[m]);
            r.Draw(g_roadGpu.mesh[m], sceneViewProj, tbd::Mat4::Identity(), true, true);
            g_drawCats.Add(m == (int)tbd::city::Mat::Track ? kCatRail : kCatRoads, roadMark, r.Counts());
            any = true;
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
    TransportDraw(r, sceneViewProj); // transport buildings, the hologram being placed, lot markers
    UtilDraw(r, sceneViewProj); // poles, pylons, wires, the underground view, the utilities cursor
    IndustryDraw(r, sceneViewProj); // industry hubs, work areas, the resource heat map
    GarbageDraw(r, sceneViewProj);  // garbage heaps, the garbage info view
    const tbd::FrameCounts agentsMark = r.Counts();
    g_drawCats.Add(kCatCity, cityMark, agentsMark);
    SimAgentsDraw(r, sceneViewProj); // cosmetic traffic from the flow solve: one instanced draw per car model
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
    if (static std::vector<tbd::Vertex> brush; !testShot && !g_play.active && (brush.clear(), tbd::character::BrushLines(brush))) // Character Creator hair brush
        r.DrawTransient(brush.data(), (uint32_t)brush.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), false);
    if (static std::vector<tbd::Vertex> clayTris, clayLines; !g_play.active && (clayTris.clear(), clayLines.clear(), tbd::character::ClayDraw(clayTris, clayLines))) { // Hair Maker clay + comb
        if (!clayTris.empty()) r.DrawTransient(clayTris.data(), (uint32_t)clayTris.size(), tbd::Topology::Triangles, viewProj, tbd::Mat4::Identity(), true);
        if (!clayLines.empty()) r.DrawTransient(clayLines.data(), (uint32_t)clayLines.size(), tbd::Topology::Lines, viewProj, tbd::Mat4::Identity(), true);
    }
    g_engine.gizmo.mode = tbd::GizmoModeForTool(g_engine.tools.Active());
    if (const tbd::Entity* sel = overlays ? g_engine.scene.Selected() : nullptr) {
        tbd::DrawSelectionOutline(r, viewProj, *sel);
        g_engine.gizmo.Draw(r, g_engine.camera, viewProj, *sel, (float)g_engine.vpH, DpiScale());
    }
    UpdateInspector();
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
    const bool remoteShot = RemoteShotThisFrame();
    if (remoteShot) r.SetFrameCapture(true);
    traceMark(kTraceEnd);
    FrameMark("overlays");
    r.EndFrame();
    FrameMark("end-frame");
    CityChunkCheckAfterFrame(r);
    if (remoteShot) RemoteShotAfterFrame(r);
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
        PostQuitMessage(0);
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
        PostQuitMessage(0);
    }
    if (g_faceLog.Active()) {
        static std::vector<uint8_t> shot;
        int shotW = 0, shotH = 0;
        if (r.CapturedFrame(shot, shotW, shotH)) g_faceLog.OnFrame(shot, shotW, shotH);
    }
    if (g_shot.active) FaceShotStep(r);
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

// ---- Windows side of the above: message decoding, focus and capture
int ButtonOf(UINT msg) {
    return msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK ? 1
         : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK ? 2 : 0;
}

void BeginDrag(HWND hwnd, Drag mode, UINT msg, LPARAM lparam) {
    if (g_engine.drag != Drag::None) return;
    SetFocus(hwnd); // so shortcuts work after clicking in a text field
    if (BeginDragAt(mode, ButtonOf(msg), GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) SetCapture(hwnd);
}

void EndDrag(UINT msg) {
    if (EndDragAt(ButtonOf(msg))) ReleaseCapture();
}

tbd::Ray MouseRay(LPARAM lparam) { return MouseRayAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); }

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
    return false;
}

void LeftClickInViewport(HWND hwnd, UINT msg, LPARAM lparam) {
    if (LeftClickAt(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), ButtonOf(msg))) SetCapture(hwnd);
}

void ContinueDrag(int x, int y) {
    const float dx = (float)(x - g_engine.lastX), dy = (float)(y - g_engine.lastY);
    g_engine.lastX = x;
    g_engine.lastY = y;
    switch (g_engine.drag) {
    case Drag::Transform:
        if (tbd::Entity* sel = g_engine.scene.Selected()) g_engine.gizmo.Update(MouseRayAt(x, y), *sel);
        break;
    case Drag::Orbit: g_engine.camera.Orbit(dx * tbd::character::CameraScale(), dy * tbd::character::CameraScale()); break; // creator open: slower
    case Drag::Fly: g_engine.camera.Look(dx * tbd::character::CameraScale(), dy * tbd::character::CameraScale()); break;
    case Drag::Pan: g_engine.camera.Pan(dx * tbd::character::CameraScale(), dy * tbd::character::CameraScale(), (float)g_engine.vpH); break;
    case Drag::Zoom: g_engine.camera.Zoom(-dy * 0.1f * tbd::character::CameraScale()); break; // drag up = closer
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
        if (key == (int)tbd::platform::Key::Escape) OnTransport(2, nullptr);
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
        tbd::Toolbar_Refresh(g_engine.toolbar);
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

// Windows side: decode the message, run GuiPointer, apply focus / capture in the order the GUI asked.
bool GuiMouse(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    float x = (float)GET_X_LPARAM(lparam), y = (float)GET_Y_LPARAM(lparam), wheel = 0.0f;
    GuiPtr ev;
    switch (msg) {
    case WM_MOUSEMOVE: ev = GuiPtr::Move; break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: ev = GuiPtr::LeftDown; break;
    case WM_LBUTTONUP: ev = GuiPtr::LeftUp; break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: ev = GuiPtr::OtherDown; break;
    case WM_MOUSEWHEEL: { // wheel messages carry screen coordinates
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        x = (float)pt.x;
        y = (float)pt.y;
        wheel = (float)GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
        ev = GuiPtr::Wheel;
        break;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lparam == hwnd) return false;
        ev = GuiPtr::CaptureLost;
        break;
    default: return false;
    }
    GuiPtrEffect fx;
    const bool used = GuiPointer(ev, x, y, wheel, fx);
    if (fx.focus) SetFocus(hwnd);
    if (fx.capture) SetCapture(hwnd);
    if (fx.release) ReleaseCapture();
    return used;
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (GuiMouse(hwnd, msg, wparam, lparam)) return 0; // in-game GUI first: it eats clicks, drags and wheel over its panels
    if ((msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && !tbd::platform::KeyDownSync(tbd::platform::Key::Alt) && !g_play.active &&
        tbd::character::ViewportMouse(hwnd, msg, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) return 0; // Character Creator hair brush
    if (g_play.active) { // Play: the mouse is polled by PlayMode::Update; clicks must not select, drag or place anything
        if (msg == WM_MOUSEWHEEL) {
            g_play.Wheel((float)GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA);
            return 0;
        }
        if ((msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) || msg == WM_MOUSEMOVE) return 0;
    }
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: // the render loop owns this window's contents
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_CHAR:
        if (g_roadRename.active) {
            RoadRenameChar((uint32_t)wparam);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (HandleKey((int)wparam, (lparam & (1 << 30)) != 0)) return 0;
        break;
    case WM_LBUTTONDOWN:
        if (tbd::platform::KeyDownSync(tbd::platform::Key::Alt)) {
            BeginDrag(hwnd, Drag::Orbit, msg, lparam);
        } else if (g_roads.active && g_engine.drag == Drag::None && !g_play.active) {
            SetFocus(hwnd);
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            RoadsCursor();
            g_roads.Click();
            if (g_roads.Dragging()) SetCapture(hwnd); // Swap: keep tracking the drag until release
            if (!g_roads.error.empty()) Toast(RoadsUpper(g_roads.error));
        } else if (g_utilTool.active && g_engine.drag == Drag::None && !g_play.active) {
            SetFocus(hwnd);
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            UtilClick();
        } else if (g_transportTool.active && g_engine.drag == Drag::None && !g_play.active) {
            SetFocus(hwnd);
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            TransportClick();
        } else if (g_zoneTool.active && g_engine.drag == Drag::None && !g_play.active) {
            SetFocus(hwnd);
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            ZonesPress();
            if (g_zoneIn.held) SetCapture(hwnd);
        } else if (g_indTool.active && g_engine.drag == Drag::None && !g_play.active) {
            SetFocus(hwnd);
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            IndustryPress();
            if (g_indTool.dragIdx >= 0) SetCapture(hwnd);
        } else if (g_terrainEdit.on && g_engine.drag == Drag::None && !g_play.active) {
            tbd::Vec3 hit;
            if (GroundHit(MouseRay(lparam), hit)) g_terrainEdit.flattenTarget = hit.y;
            BeginDrag(hwnd, Drag::Terrain, msg, lparam);
        } else if (DragForTool(g_engine.tools.Active()) != Drag::None) {
            BeginDrag(hwnd, DragForTool(g_engine.tools.Active()), msg, lparam);
        } else if (g_engine.drag == Drag::None) {
            SetFocus(hwnd);
            LeftClickInViewport(hwnd, msg, lparam);
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (g_indTool.active && g_indTool.mode != IndustryTool::Pick && g_engine.drag == Drag::None) { IndustryRightClick(); return 0; }
        if (g_roads.active && g_roads.Pending() && g_engine.drag == Drag::None) { g_roads.Cancel(); return 0; } // CS2: right click cancels
        if (g_utilTool.active && (g_utilIn.laying || g_utilTool.def >= 0 || g_utilTool.Laying() || g_utilTool.bulldoze) && g_engine.drag == Drag::None) { // right click ends the line, then drops the item
            if (g_utilIn.laying) g_utilIn.laying = false;
            else g_utilTool.def = -1, g_utilTool.line = -1, g_utilTool.bulldoze = false;
            return 0;
        }
        if (g_transportTool.active && g_transportTool.lines && (g_transportTool.lineStops > 0 || g_transportTool.outKind >= 0 || g_transportTool.lineBulldoze) && g_engine.drag == Drag::None) { // right click drops the line in progress / the connection tool
            g_transportTool.Escape();
            return 0;
        }
        if (g_transportTool.active && (g_transportTool.def >= 0 || g_transportTool.bulldoze) && g_engine.drag == Drag::None) { // right click drops the chosen building
            g_transportTool.def = -1;
            g_transportTool.bulldoze = false;
            return 0;
        }
        BeginDrag(hwnd, Drag::Fly, msg, lparam);
        return 0;
    case WM_MBUTTONDOWN: BeginDrag(hwnd, Drag::Pan, msg, lparam); return 0;
    case WM_LBUTTONUP:
        if (g_indTool.dragIdx >= 0) {
            ReleaseCapture();
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            IndustryRelease();
            return 0;
        }
        if (g_zoneIn.held) { // zoning: the brush stops, the box is painted
            ReleaseCapture();
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            ZonesRelease();
            return 0;
        }
        if (g_roads.active && g_roads.Dragging()) { // Swap: the release sets the direction
            ReleaseCapture();
            g_terrainEdit.mouseX = GET_X_LPARAM(lparam), g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
            RoadsCursor();
            g_roads.Release();
            return 0;
        }
        EndDrag(WM_LBUTTONDOWN);
        return 0;
    case WM_RBUTTONUP: EndDrag(WM_RBUTTONDOWN); return 0;
    case WM_MBUTTONUP: EndDrag(WM_MBUTTONDOWN); return 0;
    case WM_CAPTURECHANGED:
        g_engine.drag = Drag::None;
        g_engine.gizmo.End();
        return 0;
    case WM_MOUSEMOVE:
        g_terrainEdit.mouseX = GET_X_LPARAM(lparam);
        g_terrainEdit.mouseY = GET_Y_LPARAM(lparam);
        if (g_engine.drag != Drag::None) {
            ContinueDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        } else {
            // Highlight the handle under the cursor.
            const tbd::Entity* sel = g_engine.scene.Selected();
            g_engine.gizmo.mode = tbd::GizmoModeForTool(g_engine.tools.Active());
            g_engine.gizmo.hover = sel ? g_engine.gizmo.HitTest(MouseRay(lparam), g_engine.camera, *sel,
                                                                  (float)g_engine.vpH, DpiScale())
                                       : -1;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const float notches = (float)GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
        if (g_engine.drag == Drag::Fly) g_engine.flySpeed = std::clamp(g_engine.flySpeed * std::pow(1.25f, notches), 1.0f, 3000.0f);
        else { // eased zoom toward the ground point under the cursor; Shift: fine steps; creator open: slower still
            const float n = notches * (tbd::platform::KeyDownSync(tbd::platform::Key::Shift) ? 0.12f : 0.5f) * tbd::character::CameraScale();
            const float from = g_camSmooth.goal > 0.0f ? g_camSmooth.goal : g_engine.camera.distance;
            g_camSmooth.goal = std::clamp(from * std::pow(0.9f, n), 0.04f, 200.0f);
            g_camSmooth.lastSet = g_engine.camera.distance;
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &pt);
            tbd::Vec3 hit;
            g_camSmooth.anchored = !g_play.active &&
                GroundHit(g_engine.camera.ScreenRay((float)pt.x, (float)pt.y, (float)g_engine.vpW, (float)g_engine.vpH), hit);
            if (g_camSmooth.anchored) g_camSmooth.anchor = hit;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------- windows

void UpdateMenu() {
    if (!g_engine.menu) return;
    auto check = [](UINT id, bool on) { CheckMenuItem(g_engine.menu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    check(IDM_POPOUT, g_engine.viewportFloating);
    check(IDM_BROWSER, g_engine.showBrowser);
    check(IDM_PROFILER, g_profiler.Enabled());
    check(IDM_TERRAIN, g_engine.terrain.params.enabled);
    check(IDM_SUN_SKY, g_engine.sun.showSky);
    check(IDM_SUN_ANIMATE, g_engine.sun.animate);
    check(IDM_SUN_SHADOWS, g_engine.sun.shadows);
    CheckMenuRadioItem(g_engine.menu, IDM_GFX_FULL, IDM_GFX_UNLIT, IDM_GFX_FULL + g_gfxQuality, MF_BYCOMMAND);
    CheckMenuRadioItem(g_engine.menu, IDM_D3D12, IDM_VULKAN, g_engine.api == Api::D3D12 ? IDM_D3D12 : IDM_VULKAN, MF_BYCOMMAND);
    const int radius = g_engine.terrain.params.radius;
    CheckMenuRadioItem(g_engine.menu, IDM_RADIUS_NEAR, IDM_RADIUS_FAR,
                       radius <= 5 ? IDM_RADIUS_NEAR : (radius <= 8 ? IDM_RADIUS_MED : IDM_RADIUS_FAR), MF_BYCOMMAND);
}

HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW, L"New Scene\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"Open Scene...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"Save Scene\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"Save Scene As...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"Exit");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_DUPLICATE, L"Duplicate\tCtrl+D");
    AppendMenuW(edit, MF_STRING, IDM_DELETE, L"Delete\tDel");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_POPOUT, L"Pop Out Viewport\tF4");
    AppendMenuW(view, MF_STRING, IDM_BROWSER, L"Content Browser\tCtrl+B");
    AppendMenuW(view, MF_STRING, IDM_PROFILER, L"Profiler Overlay\tF3");
    AppendMenuW(view, MF_STRING, IDM_RELOADSHADERS, L"Reload Shaders\tF5");
    AppendMenuW(view, MF_STRING, IDM_REBUILDGAME, L"Rebuild Gameplay DLL\tCtrl+F5");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_FRAME, L"Frame Selected\tF");
    AppendMenuW(view, MF_STRING, IDM_RESETCAM, L"Reset Camera\tHome");
    HMENU world = CreatePopupMenu();
    AppendMenuW(world, MF_STRING, IDM_TERRAIN, L"Terrain");
    AppendMenuW(world, MF_STRING, IDM_NEWSEED, L"New Random Terrain");
    AppendMenuW(world, MF_STRING, IDM_GENERATE_LODS, L"Generate LODs (Outdoor)");
    AppendMenuW(world, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(world, MF_STRING, IDM_RADIUS_NEAR, L"Draw Distance: Near (5 chunks)");
    AppendMenuW(world, MF_STRING, IDM_RADIUS_MED, L"Draw Distance: Medium (8 chunks)");
    AppendMenuW(world, MF_STRING, IDM_RADIUS_FAR, L"Draw Distance: Far (12 chunks)");
    AppendMenuW(world, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(world, MF_STRING, IDM_SUN_SKY, L"Sky and Sun");
    AppendMenuW(world, MF_STRING, IDM_SUN_SHADOWS, L"Sun Shadows");
    AppendMenuW(world, MF_STRING, IDM_SUN_ANIMATE, L"Animate Day Cycle");
    AppendMenuW(world, MF_STRING, IDM_SUN_SUNRISE, L"Time: Sunrise (6:30)");
    AppendMenuW(world, MF_STRING, IDM_SUN_MORNING, L"Time: Morning (9:00)");
    AppendMenuW(world, MF_STRING, IDM_SUN_NOON, L"Time: Noon (12:00)");
    AppendMenuW(world, MF_STRING, IDM_SUN_AFTERNOON, L"Time: Afternoon (15:00)");
    AppendMenuW(world, MF_STRING, IDM_SUN_SUNSET, L"Time: Sunset (17:30)");
    AppendMenuW(world, MF_STRING, IDM_SUN_NIGHT, L"Time: Night (23:00)");
    HMENU gfx = CreatePopupMenu();
    AppendMenuW(gfx, MF_STRING, IDM_D3D12, L"Direct3D 12\tF1");
    AppendMenuW(gfx, MF_STRING, IDM_VULKAN, L"Vulkan\tF2");
    AppendMenuW(gfx, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(gfx, MF_STRING, IDM_GFX_FULL, L"Quality: Full\tF6 cycles");
    AppendMenuW(gfx, MF_STRING, IDM_GFX_FAST, L"Quality: Fast (modeling)");
    AppendMenuW(gfx, MF_STRING, IDM_GFX_UNLIT, L"Quality: Unlit (no lighting)");
    AppendMenuW(gfx, MF_STRING, IDM_BRIGHT_DN, L"Brightness down	F9");
    AppendMenuW(gfx, MF_STRING, IDM_BRIGHT_UP, L"Brightness up	F10");
    AppendMenuW(gfx, MF_STRING, IDM_BRIGHT_SLIDER, L"Brightness slider...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)world, L"World");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)gfx, L"Graphics");
    tbd::character::AddMenu(bar); // top-level "Character Creator"
    return bar;
}

HACCEL BuildAccelerators() {
    ACCEL keys[] = {
        {FCONTROL | FVIRTKEY, 'N', IDM_NEW},
        {FCONTROL | FVIRTKEY, 'O', IDM_OPEN},
        {FCONTROL | FVIRTKEY, 'S', IDM_SAVE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', IDM_SAVEAS},
        {FCONTROL | FVIRTKEY, 'D', IDM_DUPLICATE},
        {FCONTROL | FVIRTKEY, 'B', IDM_BROWSER},
        {FVIRTKEY, (int)tbd::platform::Key::F4, IDM_POPOUT},
        {FVIRTKEY, (int)tbd::platform::Key::F1, IDM_D3D12},
        {FVIRTKEY, (int)tbd::platform::Key::F2, IDM_VULKAN},
        {FVIRTKEY, (int)tbd::platform::Key::F3, IDM_PROFILER},
        {FVIRTKEY, (int)tbd::platform::Key::F5, IDM_RELOADSHADERS},
        {FVIRTKEY, (int)tbd::platform::Key::F6, IDM_GFX_CYCLE},
        {FVIRTKEY, (int)tbd::platform::Key::F9, IDM_BRIGHT_DN},
        {FVIRTKEY, (int)tbd::platform::Key::F10, IDM_BRIGHT_UP},
        {FVIRTKEY, (int)tbd::platform::Key::F7, (WORD)tbd::character::kMenuCommand},
        {FVIRTKEY | FCONTROL, (int)tbd::platform::Key::F5, IDM_REBUILDGAME},
    };
    return CreateAcceleratorTableW(keys, (int)(sizeof(keys) / sizeof(keys[0])));
}

void OnCommand(UINT id) {
    if (id >= IDM_SUN_SUNRISE && id <= IDM_SUN_NIGHT) g_engine.playStartHour = -1.0f; // a chosen time replaces the Play start
    switch (id) {
    case IDM_NEW: NewScene(); break;
    case IDM_OPEN: {
        std::wstring path;
        if (PickSceneFile(false, path)) LoadScene(path);
        break;
    }
    case IDM_SAVE: SaveScene(false); break;
    case IDM_SAVEAS: SaveScene(true); break;
    case IDM_EXIT: DestroyWindow(g_engine.window); break;
    case IDM_DUPLICATE: DuplicateSelected(); break;
    case IDM_DELETE: DeleteSelected(); break;
    case IDM_POPOUT: SetViewportFloating(!g_engine.viewportFloating); break;
    case IDM_BROWSER:
        g_engine.showBrowser = !g_engine.showBrowser;
        Layout();
        break;
    case IDM_PROFILER: g_profiler.Toggle(); break;
    case tbd::character::kMenuCommand: case tbd::character::kMenuBody: case tbd::character::kMenuHead: CharacterCreatorHost(); tbd::character::Command(id); break;
    case IDM_RELOADSHADERS: g_hotShaders.Reload(g_engine.renderer.get(), g_engine.api == Api::Vulkan); break;
    case IDM_REBUILDGAME: g_game.Rebuild(); break;
    case IDM_GENERATE_LODS: GenerateOutdoorLods(); break;
    case IDM_RESETCAM: g_engine.camera = tbd::OrbitCamera(); break;
    case IDM_FRAME: FrameSelected(); break;
    case IDM_TERRAIN: g_engine.terrain.params.enabled = !g_engine.terrain.params.enabled; break;
    case IDM_NEWSEED:
        g_engine.terrain.params.seed = (int)(tbd::platform::TickCountMs() & 0x7FFFFFFF);
        g_engine.terrain.params.enabled = true;
        break;
    case IDM_RADIUS_NEAR: g_engine.terrain.params.radius = 5; break;
    case IDM_RADIUS_MED: g_engine.terrain.params.radius = 8; break;
    case IDM_RADIUS_FAR: g_engine.terrain.params.radius = 12; break;
    case IDM_SUN_SKY: g_engine.sun.showSky = !g_engine.sun.showSky; break;
    case IDM_SUN_SHADOWS: g_engine.sun.shadows = !g_engine.sun.shadows; break;
    case IDM_SUN_ANIMATE: g_engine.sun.animate = !g_engine.sun.animate; break;
    case IDM_SUN_SUNRISE: g_engine.sun.hour = 6.5f; break;
    case IDM_SUN_MORNING: g_engine.sun.hour = 9.0f; break;
    case IDM_SUN_NOON: g_engine.sun.hour = 12.0f; break;
    case IDM_SUN_AFTERNOON: g_engine.sun.hour = 15.0f; break;
    case IDM_SUN_SUNSET: g_engine.sun.hour = 17.5f; break;
    case IDM_SUN_NIGHT: g_engine.sun.hour = 23.0f; break;
    case IDM_D3D12: SwitchApi(Api::D3D12); break;
    case IDM_VULKAN: SwitchApi(Api::Vulkan); break;
    case IDM_GFX_FULL: g_gfxQuality = 0; break;
    case IDM_GFX_FAST: g_gfxQuality = 1; break;
    case IDM_GFX_UNLIT: g_gfxQuality = 2; break;
    case IDM_GFX_CYCLE: g_gfxQuality = (g_gfxQuality + 1) % 3; break;
    case IDM_BRIGHT_DN: g_brightness = std::max(0.4f, g_brightness - 0.15f); break;
    case IDM_BRIGHT_UP: g_brightness = std::min(3.0f, g_brightness + 0.15f); break;
    case IDM_BRIGHT_SLIDER: ShowBrightnessSlider(g_engine.window); break;
    }
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

// Play also hands the viewport to the first-person camera (play_mode.h); Stop, or Esc in the viewport, gives it back with
// the editor camera as it was. The sun cycle is unchanged: Play starts it, Pause freezes it, Stop returns it to the start hour.
void OnTransport(int action, void*) {
    switch (action) {
    case 0:
        CompleteSceneLoad();
        if (!g_play.active && g_engine.viewportWindow && g_play.Begin(g_engine.viewportWindow, g_engine.camera)) {
            if (g_engine.drag != Drag::None) { // a drag in progress ends; Begin has taken the mouse capture
                g_engine.drag = Drag::None;
                g_engine.gizmo.End();
            }
            g_physics.SetScene(g_engine.jobs.get(), g_engine.scene, g_engine.assetsRoot, LoadGroundMesh);
            g_play.player = g_player.Begin(g_engine.scene, g_engine.meshes, g_physics, g_engine.animator);
            g_npcs.Host().create = CreateNpcRig;
            g_npcs.Host().destroy = DestroyNpcRig;
            g_npcs.Begin(g_engine.scene);
            if (!g_play.player) g_play.PrepareGround(g_engine.jobs.get(), g_engine.scene, LoadGroundMesh);
            Toast(g_play.player ? "PLAY  WASD MOVES  G FLIES  ESC STOPS" : "PLAY  G WALKS  ESC STOPS");
            g_gamePlaySnapshot = g_engine.scene.entities;
            g_script.LoadManifest();
            for (size_t i = 0; i < g_engine.scene.entities.size(); ++i)
                for (const tbd::EntityScript& sc : g_engine.scene.entities[i].scripts) g_script.AddEntityScript((int)i, sc.path, sc.props);
            g_script.Start();
        }
        if (g_engine.playStartHour < 0.0f) g_engine.playStartHour = g_engine.sun.hour;
        g_engine.sun.animate = true;
        break;
    case 1:
        g_engine.sun.animate = false;
        break;
    case 2:
        g_script.Stop();
        if (g_gamePlaySnapshot.size() == g_engine.scene.entities.size())
            for (size_t i = 0; i < g_gamePlaySnapshot.size(); ++i) { // what the gameplay DLL moved goes back
                g_engine.scene.entities[i].position = g_gamePlaySnapshot[i].position;
                g_engine.scene.entities[i].rotation = g_gamePlaySnapshot[i].rotation;
                g_engine.scene.entities[i].scale = g_gamePlaySnapshot[i].scale;
            }
        g_gamePlaySnapshot.clear();
        g_npcs.End(g_engine.scene);
        g_player.End(g_engine.scene);
        g_play.End(g_engine.camera);
        g_engine.sun.animate = false;
        if (g_engine.playStartHour >= 0.0f) g_engine.sun.hour = g_engine.playStartHour;
        g_engine.playStartHour = -1.0f;
        break;
    }
}

int TransportStateOf(void*) { return (g_engine.sun.animate || g_play.active) ? 1 : (g_engine.playStartHour >= 0.0f ? 2 : 0); }
bool BrowserShown(void*) { return g_engine.showBrowser; }
bool ViewportIsFloating(void*) { return g_engine.viewportFloating; }
void BeforeMenu(void*) { UpdateMenu(); } // the menus live outside any window now, so refresh their check marks by hand

int OutlinerCount(void*) { return (int)g_engine.scene.entities.size(); }
const char* OutlinerName(int index, void*) {
    return index >= 0 && index < (int)g_engine.scene.entities.size() ? g_engine.scene.entities[index].name.c_str() : "";
}
const char* OutlinerKind(int index, void*) {
    return index >= 0 && index < (int)g_engine.scene.entities.size() ? tbd::MeshKindName(g_engine.scene.entities[index].mesh) : "";
}
int OutlinerSelected(void*) {
    return g_engine.scene.selected < (int)g_engine.scene.entities.size() ? g_engine.scene.selected : -1;
}
void OutlinerSelect(int index, void*) { g_engine.scene.selected = index; }
void OnOutlinerCommand(tbd::OutlinerCommand command, void*) {
    switch (command) {
    case tbd::OutlinerCommand::Frame: FrameSelected(); break;
    case tbd::OutlinerCommand::Duplicate: DuplicateSelected(); break;
    case tbd::OutlinerCommand::Delete: DeleteSelected(); break;
    }
}

bool OverSplitter(int x, int y) {
    if (!g_engine.showBrowser) return false;
    RECT s = ComputeAreas().splitter;
    InflateRect(&s, 0, Sx(2));
    const POINT pt = {x, y};
    return PtInRect(&s, pt) != FALSE;
}

bool OverDockSplit(int x, int y) {
    RECT s = ComputeAreas().dockSplit;
    InflateRect(&s, Sx(2), 0);
    const POINT pt = {x, y};
    return PtInRect(&s, pt) != FALSE;
}

void PaintMainWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    const Areas a = ComputeAreas();
    {
        HBRUSH b = CreateSolidBrush(tbd::theme::kLine);
        FillRect(dc, &a.dockSplit, b);
        DeleteObject(b);
    }
    if (g_engine.showBrowser && g_engine.contentBrowser) {
        HBRUSH b = CreateSolidBrush(tbd::theme::kLine);
        FillRect(dc, &a.splitter, b);
        DeleteObject(b);
    }
    if (g_engine.viewportFloating) { // placeholder where the viewport used to be
        HBRUSH b = CreateSolidBrush(kClearColor);
        FillRect(dc, &a.viewport, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g_engine.uiFont);
        SetTextColor(dc, tbd::theme::kMuted);
        RECT text = a.viewport;
        DrawTextW(dc, L"The viewport is in its own window.\n\nClick here or press F4 to dock it back.", -1, &text,
                  DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        if (lparam == 0) { // menu or accelerator
            OnCommand(LOWORD(wparam));
            return 0;
        }
        break;
    case WM_INITMENUPOPUP:
        UpdateMenu();
        break;
    case WM_CHAR:
        if (g_roadRename.active) {
            RoadRenameChar((uint32_t)wparam);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (HandleKey((int)wparam, (lparam & (1 << 30)) != 0)) return 0;
        break;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
        if (OverSplitter(x, y)) {
            g_engine.splitDrag = true;
            SetCapture(hwnd);
        } else if (OverDockSplit(x, y)) {
            g_engine.dockDrag = true;
            SetCapture(hwnd);
        } else if (g_engine.viewportFloating) {
            const RECT v = ComputeAreas().viewport;
            const POINT pt = {x, y};
            if (PtInRect(&v, pt)) SetViewportFloating(false);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_engine.splitDrag) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int minH = tbd::ContentBrowser_MinHeight(g_engine.dpi);
            const int maxH = std::max(minH, (int)rc.bottom - tbd::Toolbar_Height(g_engine.dpi) - Sx(160) - Sx(5));
            g_engine.browserH = std::clamp((int)rc.bottom - GET_Y_LPARAM(lparam) - Sx(3), minH, maxH); // kept in range, so no dead zone past the limits
            Layout();
        } else if (g_engine.dockDrag) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_engine.dockW = (int)rc.right - GET_X_LPARAM(lparam) - Sx(2); // ComputeAreas clamps it
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_engine.splitDrag || g_engine.dockDrag) {
            g_engine.splitDrag = false;
            g_engine.dockDrag = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_engine.splitDrag = false;
        g_engine.dockDrag = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_engine.splitDrag || OverSplitter(pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (g_engine.dockDrag || OverDockSplit(pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_ERASEBKGND:
        return 1; // the viewport and panels cover the whole client area
    case WM_PAINT:
        PaintMainWindow(hwnd);
        return 0;
    case WM_SIZE:
        Layout();
        if (g_engine.toolbar) RenderFrame(); // keeps the view alive while the window is dragged
        return 0;
    case WM_DESTROY:
        g_engine.running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The pop-out window: closing it docks the viewport back rather than destroying anything.
LRESULT CALLBACK FloatProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CLOSE:
        SetViewportFloating(false);
        return 0;
    case WM_CHAR:
        if (g_roadRename.active) {
            RoadRenameChar((uint32_t)wparam);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (HandleKey((int)wparam, (lparam & (1 << 30)) != 0)) return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && g_engine.viewportFloating) {
            Layout();
            RenderFrame();
        }
        return 0;
    case WM_SETFOCUS: // keys go to the viewport window inside
        if (g_engine.viewportWindow) SetFocus(g_engine.viewportWindow);
        return 0;
    case WM_DESTROY:
        if (hwnd == g_engine.floatWindow) g_engine.floatWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------- startup

// What bin\game.dll can reach in the engine (game_api.h).
const TbdGameHost& GameHost() {
    static TbdGameHost host = [] {
        TbdGameHost h = {};
        h.version = TBD_GAME_API_VERSION;
        h.log = [](const char* text) { tbd::Log("%s", text); };
        h.entityCount = [] { return (int)g_engine.scene.entities.size(); };
        h.findEntity = [](const char* name) {
            for (size_t i = 0; i < g_engine.scene.entities.size(); ++i)
                if (g_engine.scene.entities[i].name == name) return (int)i;
            return -1;
        };
        h.entityName = [](int i) { return i >= 0 && i < (int)g_engine.scene.entities.size() ? g_engine.scene.entities[i].name.c_str() : ""; };
        h.getTransform = [](int i, float p[3], float q[4], float s[3]) {
            if (i < 0 || i >= (int)g_engine.scene.entities.size()) return;
            const tbd::Entity& e = g_engine.scene.entities[i];
            p[0] = e.position.x; p[1] = e.position.y; p[2] = e.position.z;
            q[0] = e.rotation.x; q[1] = e.rotation.y; q[2] = e.rotation.z; q[3] = e.rotation.w;
            s[0] = e.scale.x; s[1] = e.scale.y; s[2] = e.scale.z;
        };
        h.setTransform = [](int i, const float p[3], const float q[4], const float s[3]) {
            if (i < 0 || i >= (int)g_engine.scene.entities.size() || !g_play.active) return; // only in Play, which Stop undoes
            tbd::Entity& e = g_engine.scene.entities[i];
            e.position = {p[0], p[1], p[2]};
            e.rotation = {q[0], q[1], q[2], q[3]};
            e.scale = {s[0], s[1], s[2]};
        };
        h.keyDown = [](int vk) { return g_play.active && tbd::platform::KeyDownAsync((tbd::platform::Key)vk) ? 1 : 0; };
        h.playerFeet = [](float p[3]) {
            if (!g_play.active || !g_player.Placed()) return 0;
            const tbd::Vec3 f = g_player.State().feet;
            p[0] = f.x; p[1] = f.y; p[2] = f.z;
            return 1;
        };
        h.getHour = [] { return g_engine.sun.hour; };
        h.setHour = [](float hour) { g_engine.sun.hour = hour; };
        h.playSound = [](const char* name, const float p[3], float volume, float pitch) { tbd::audio::PlayOneShot(name, {p[0], p[1], p[2]}, volume, pitch); };
        return h;
    }();
    return host;
}

bool Engine_Init(HINSTANCE instance, int show, const std::wstring& title) {
    g_engine.instance = instance;
    g_engine.titleBase = title;
    g_engine.tickFreq = tbd::platform::TickFrequency();
    g_engine.jobs = std::make_unique<tbd::JobSystem>(); // every hardware thread but one (the main thread)
    g_engine.uiFont = CreateFontW(-Sx(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kClearColor);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = FloatProc;
    wc.lpszClassName = kFloatClass;
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = ViewportProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kViewportClass;
    if (!RegisterClassExW(&wc)) return false;

    // Size the outer frame so the client area is exactly kStartWidth x kStartHeight.
    RECT rect = {0, 0, kStartWidth, kStartHeight};
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    g_engine.menu = BuildMenu(); // not attached to the window: the toolbar draws its popups as the menu strip
    AdjustWindowRect(&rect, style, FALSE);

    g_engine.window = CreateWindowExW(0, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                                      nullptr);
    if (!g_engine.window) return false;
    g_engine.accel = BuildAccelerators();

    g_engine.browserH = tbd::ContentBrowser_DefaultHeight(g_engine.dpi);
    g_engine.tools.onAction = OnToolAction;
    tbd::ToolbarHost bar;
    bar.menu = g_engine.menu;
    bar.tools = &g_engine.tools;
    bar.cmdSave = IDM_SAVE;
    bar.cmdFrame = IDM_FRAME;
    bar.cmdBrowser = IDM_BROWSER;
    bar.cmdPopout = IDM_POPOUT;
    bar.beforeMenu = BeforeMenu;
    bar.transport = OnTransport;
    bar.transportState = TransportStateOf;
    bar.browserShown = BrowserShown;
    bar.viewportFloating = ViewportIsFloating;
    g_engine.toolbar = tbd::Toolbar_Create(g_engine.window, instance, g_engine.dpi, bar);
    tbd::OutlinerSource source;
    source.count = OutlinerCount;
    source.name = OutlinerName;
    source.kind = OutlinerKind;
    source.selected = OutlinerSelected;
    source.select = OutlinerSelect;
    source.command = OnOutlinerCommand;
    g_engine.outliner = tbd::Outliner_Create(g_engine.window, instance, g_engine.dpi, source);
    g_engine.inspector = tbd::Inspector_Create(g_engine.window, instance, g_engine.dpi, OnInspectorEdit, nullptr);
    if (g_engine.inspector) tbd::Inspector_SetScriptHandler(g_engine.inspector, OnInspectorScriptEdit, nullptr);
    tbd::ContentBrowserCallbacks callbacks;
    callbacks.openAsset = OnOpenAsset;
    g_engine.contentBrowser =
        tbd::ContentBrowser_Create(g_engine.window, instance, g_engine.dpi, g_engine.assetsRoot, callbacks);
    g_engine.running = true;
    Layout();

    // Requested API first, the other one as a fallback.
    const Api first = g_engine.api, second = first == Api::D3D12 ? Api::Vulkan : Api::D3D12;
    ShowWindow(g_engine.window, show); // before the renderer: a first D3D12 start compiles shaders for a while
    UpdateWindow(g_engine.window);
    if (!StartRenderer(first) && (!g_engine.running || !StartRenderer(second))) return false;
    Layout(); // the renderer missed the size the window took when shown
    g_hotShaders.Init();
    g_game.Init(&GameHost());
    tbd::script::Services().setSkinDetailScale = SetEntitySkinDetailScale;
    tbd::script::Services().setSkinPoreDepth = SetEntitySkinPoreDepth;
    tbd::script::RegisterBuiltinApi(&GameHost());
    g_script.Init(tbd::WideToUtf8(tbd::Join(g_engine.projectFolder, L"Scripts")), [](const char* text) { tbd::Log("%s", text); });
    return true;
}

void Engine_Run() {
    MSG msg;
    while (g_engine.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_engine.running = false;
            if (scene_undo::Key(msg)) continue; // Ctrl+Z / Ctrl+Y anywhere in the engine (not in a text field)
            // During Play Ctrl is "down", so Ctrl+S / Ctrl+D / Ctrl+N and F1 / F2 / F4 must not fire as editor shortcuts.
            if (!g_play.active && g_engine.window && g_engine.accel && TranslateAcceleratorW(g_engine.window, g_engine.accel, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_engine.running) break;
        const HWND host = ViewportHost();
        if (IsIconic(g_engine.window) || (host && IsIconic(host))) {
            Sleep(50);
            continue;
        }
        // Another app in front: at most 30 frames a second; the editor, outside Play, at most TBD_EDITORFPS (default 60, 0 for
        // the display's rate). The same image, fewer of them; vsync paces the loop otherwise. A message (input, a resize) wakes
        // the wait early. TBD_NOTHROTTLE and the scripted runs keep every frame.
        static const bool throttle = !tbd::platform::GetEnvA("TBD_NOTHROTTLE", nullptr, 0) && !tbd::platform::GetEnvA("TBD_SHOT", nullptr, 0) &&
                                     !tbd::platform::GetEnvA("TBD_CAPTURE", nullptr, 0) && !tbd::platform::GetEnvA("TBD_PLAYTEST", nullptr, 0);
        static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        static int64_t freq = [] { int64_t f; f = tbd::platform::TickFrequency(); return f; }(), last = {};
        DWORD foreground = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        static const LONGLONG editorFps = (LONGLONG)EnvFloat("TBD_EDITORFPS", 60.0f, 0.0f, 1000.0f);
        const LONGLONG cap = foreground != GetCurrentProcessId() ? 30 : (g_play.active ? 0 : editorFps);
        if (throttle && timer && cap > 0) {
            int64_t now;
            now = tbd::platform::Ticks();
            // Ticks first, and only a positive remainder is scaled: at first `last` is 0, so the difference is minus the whole uptime
            // in ticks, and times 1e7 that overflowed 64 bits after ~10 h of uptime, wrapped positive, and slept the loop forever.
            const LONGLONG leftTicks = last + freq / cap - now;
            const LONGLONG left100ns = leftTicks > 0 ? leftTicks * 10000000 / freq : 0;
            if (left100ns > 10000) { // over 1 ms to go
                LARGE_INTEGER due;
                due.QuadPart = -left100ns;
                // Never INFINITE: if the timer did not arm (SetWaitableTimer failing, or the high-resolution timer never firing)
                // the loop would sleep forever on a white window, and the remote pipe, which starts in the first frame, never opens.
                const bool armed = SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
                const DWORD capMs = (DWORD)std::min<LONGLONG>(left100ns / 10000 + 4, 100); // a frame is at most 33 ms away; 100 only guards a bad clock
                MsgWaitForMultipleObjects(armed ? 1 : 0, armed ? &timer : nullptr, FALSE, capMs, QS_ALLINPUT);
                continue;
            }
        }
        last = tbd::platform::Ticks();
        RenderFrame(); // presents with vsync, which also paces the loop
    }
}

void Engine_Shutdown() {
    g_script.Shutdown();
    tbd::character::Shutdown();
    g_game.Shutdown();
    tbd::audio::Stop();
    StopRenderer();
    g_engine.jobs.reset(); // joins the workers; nothing is queued by now
    if (g_engine.floatWindow) DestroyWindow(g_engine.floatWindow);
    if (g_engine.window) {
        DestroyWindow(g_engine.window);
        g_engine.window = nullptr;
    }
    if (g_engine.accel) DestroyAcceleratorTable(g_engine.accel);
    if (g_engine.menu) DestroyMenu(g_engine.menu);
    if (g_engine.uiFont) DeleteObject(g_engine.uiFont);
    UnregisterClassW(kViewportClass, g_engine.instance);
    UnregisterClassW(kFloatClass, g_engine.instance);
    UnregisterClassW(kWindowClass, g_engine.instance);
}

// Command line: [--api=d3d12|vulkan] [--validate] [--scene "<file>"] [--face-shot "<clip>" "<folder>"] ["<project folder>"]. The project manager passes
// only the folder.
struct Args {
    std::wstring projectFolder;
    Api api = Api::D3D12;
    bool validate = false;
    std::wstring scene; // --scene "<file.tbdscene>": opened right after start-up
    bool faceLive = false;          // --face-live: opens straight into Face Mocap Live on the webcam
    bool facePhone = false;         // --face-phone: opens straight into Face Mocap Live on the scene's head, listening for an iPhone
    std::wstring shotClip, shotDir; // --face-shot "<clip>" "<folder>": the face self test, see FaceShot
    std::wstring videoPath;         // --face-video "<clip>" "<out.mp4>": the clip through the renderer, with --max-graphics if wanted
    bool maxGraphics = false;       // --max-graphics
};

Args ParseArgs(const std::wstring& cmdline) {
    Args args;
    bool wantScene = false;
    int wantShot = 0;
    bool wantVideo = false;
    size_t i = 0;
    while (i < cmdline.size()) {
        while (i < cmdline.size() && cmdline[i] == L' ') ++i;
        if (i >= cmdline.size()) break;
        std::wstring token;
        if (cmdline[i] == L'"') {
            size_t close = cmdline.find(L'"', i + 1);
            token = cmdline.substr(i + 1, close == std::wstring::npos ? std::wstring::npos : close - i - 1);
            i = close == std::wstring::npos ? cmdline.size() : close + 1;
        } else {
            size_t end = cmdline.find(L' ', i);
            token = cmdline.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i);
            i = end == std::wstring::npos ? cmdline.size() : end;
        }
        if (wantScene) {
            args.scene = token;
            wantScene = false;
        } else if (wantShot > 0) {
            (wantShot == 2 ? args.shotClip : wantVideo ? args.videoPath : args.shotDir) = token;
            --wantShot;
        } else if (token == L"--face-shot") wantShot = 2;
        else if (token == L"--face-video") { wantShot = 2; wantVideo = true; }
        else if (token == L"--max-graphics") args.maxGraphics = true;
        else if (token == L"--face-phone") args.facePhone = true;
        else if (token == L"--face-live") args.faceLive = true;
        else if (token == L"--scene") wantScene = true;
        else if (token == L"--api=vulkan") args.api = Api::Vulkan;
        else if (token == L"--api=d3d12") args.api = Api::D3D12;
        else if (token == L"--validate") args.validate = true;
        else if (token.rfind(L"--", 0) != 0) args.projectFolder = token;
    }
    return args;
}

// The folder the content browser shows: <project>\Assets, or an Assets folder next to bin\ when the
// engine was started without a project.
std::wstring AssetsFolderFor(const std::wstring& projectFolder) {
    std::wstring base = projectFolder.empty() ? tbd::ExeDirectory() + L"\\..\\Assets" : tbd::Join(projectFolder, L"Assets");
    wchar_t full[tbd::platform::kMaxPath] = {};
    const DWORD n = GetFullPathNameW(base.c_str(), tbd::platform::kMaxPath, full, nullptr);
    return n > 0 && n < tbd::platform::kMaxPath ? std::wstring(full) : base;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int show) {
    tbd::InstallCrashHandlers();
    SetProcessDPIAware(); // crisp text on scaled displays
    g_engine.dpi = (int)GetDpiForSystem();
    const Args args = ParseArgs(cmdline);
    g_engine.api = args.api;
    g_engine.validation = args.validate;
    g_engine.projectFolder = args.projectFolder;

    std::wstring title = kWindowTitle;
    if (!args.projectFolder.empty()) {
        std::wstring name;
        if (!tbd::ReadProjectName(args.projectFolder, name)) {
            MessageBoxW(nullptr, (L"Not a TBD Engine project:\n" + args.projectFolder).c_str(), kWindowTitle,
                        MB_ICONERROR);
            return 1;
        }
        title += L" - " + name;
    }
    g_engine.assetsRoot = AssetsFolderFor(args.projectFolder);
    g_lodCacheDir = tbd::Join(g_engine.assetsRoot, L"LodCache");
    if (!Engine_Init(instance, show, title)) {
        if (!g_engine.running) return 0; // closed while the shaders compiled
        MessageBoxW(nullptr, L"Could not start a graphics API (see engine.log next to the executable).", kWindowTitle,
                    MB_ICONERROR);
        return 1;
    }
    tbd::audio::Start(g_engine.assetsRoot); // its own thread; does nothing in TBD_SHOT / TBD_CAPTURE runs
    if (!args.scene.empty()) LoadScene(args.scene);
    else if (ProjectWantsSandbox(args.projectFolder)) StartTerrainSandbox();
    if (args.maxGraphics) SetMaxGraphics(true);
    if (args.faceLive) OnToolAction(tbd::ToolId::FaceLive);
    if (args.facePhone) {
        std::string error;
        if (g_engine.faceDriver.live.StartPhone(tbd::kLiveLinkPort, &error)) OnToolAction(tbd::ToolId::FaceLive);
        else Toast(error);
    }
    if (!args.shotDir.empty() || !args.videoPath.empty()) {
        g_shot.clipPath = args.shotClip;
        g_shot.dir = args.shotDir;
        g_shot.video = !args.videoPath.empty();
        g_shot.videoPath = args.videoPath;
        if (!args.shotDir.empty()) std::filesystem::create_directories(args.shotDir);
        if (g_shot.video) SetWindowPos(g_engine.window, nullptr, 0, 0, 1800, 1100, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        FaceShotStart();
    }
    Engine_Run();
    Engine_Shutdown();
    tbd::MarkCleanExit();
    return 0;
}
