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
#include "game/shell.h"
#include "camera.h"
#include "city_camera.h"
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
#include "gui/glass_dock.h"    // Glass Dock+: the mobile / tablet GUI (game/glass_dock_host.inl)
#include "gui/road_toolkit.h"  // [road toolkit] the road / rail touch panel + palette (game/glass_dock_host.inl RoadKit)
#include "gui/road_reticle.h"  // [road ux] the aim dot (game/glass_dock_host.inl RoadKit)
#include "gui/terrain_kit.h"   // [road ux] the compact terrain tools (game/glass_dock_host.inl TerrainKit)
#include "gui/city_hud.h"      // the CS2-style bottom toolbar, status bar and side icons
#include "gui/city_overlays.h"     // the info overlays' toggles and legend
#include "gui/zone_toolbar.h"     // zoning (src/city/zoning.h): zone painting, growth, the game clock
#include "gui/transport_toolbar.h" // transport buildings (src/city/transport.h): depots, stations, harbours, airports
#include "gui/utility_toolbar.h"   // utilities: electricity, water, sewage (city_utilities.inl hosts them)
#include "city/transport_models.h"
#include "city/industry.h"         // specialized industry (city_industry.inl hosts it)
#include "city/utilities.h"        // utilities (city_utilities.inl hosts them)
#include "city/sim.h"              // the city sim's fixed-step scheduler (city_sim.inl hosts it)
#include "city/sim_children.h"     // neighbourhoods, herds, routes, visible children (city_herds.inl hosts them)
#include "city/sim_routes.h"
#include "city/sim_town_traffic.h"   // local trips, outside connections, parked cars (city_herds.inl hosts them)
#include "city/sim_cells.h"        // the city sim's per-cell counters: homes, jobs, residents, employment
#include "city/sim_growth.h"       // the city sim's demand, demand-gated growth, level up / down, abandonment
#include "city/sim_coverage.h"     // the city sim's utilities per cell and service coverage
#include "city/sim_economy.h"      // the city sim's treasury, taxes and upkeep
#include "city/sim_budget.h"       // the city sim's ledger: itemised expenses, loans, bankruptcy
#include "city/sim_milestones.h"   // population milestones and the tools they unlock
#include "city/news_feed.h"        // news headlines for sim events (the radio reads its queue)
#include "city/news_adjacency.h"   // a new building's neighbours in its radio story (city_radio.inl)
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
#include "game/game_core.inl" // the game (portable); the Windows shell follows

const COLORREF kClearColor = RGB(26, 26, 26); // window background behind the viewport

tbd::hot::Shaders g_hotShaders;  // shader files changing (and F5) reload them on both backends; see hot_reload.h
tbd::hot::Game g_game;           // bin\game.dll, swapped in whenever it is rebuilt; see hot_reload.h
tbd::FaceSessionLog g_faceLog; // debug video of the Face Mocap Live tab
tbd::FaceDriver g_faceDriver;  // plays face mocap clips on entities

// Play's mouse look on Windows: the cursor is captured, hidden, clipped to the viewport and recentred every frame.
struct WinLookPointer : tbd::PlayMode::LookPointer {
    HWND viewport = nullptr;
    bool cursorHidden = false;
    WinLookPointer* For(HWND v) { viewport = v; return this; }
    void Grab() override {
        SetFocus(viewport);
        SetCapture(viewport);
        ShowCursor(FALSE);
        cursorHidden = true;
        const POINT centre = LockCursor();
        SetCursorPos(centre.x, centre.y);
    }
    void Release() override {
        ClipCursor(nullptr);
        if (cursorHidden) ShowCursor(TRUE);
        cursorHidden = false;
        if (GetCapture() == viewport) ReleaseCapture();
    }
    bool Lost() const override {
        return GetActiveWindow() != GetAncestor(viewport, GA_ROOT) && GetActiveWindow() != GetAncestor(viewport, GA_ROOTOWNER);
    }
    void Delta(float& dx, float& dy) override {
        if (GetCapture() != viewport) SetCapture(viewport); // a toolbar click still in flight can take it
        POINT centre = LockCursor();
        POINT p;
        if (GetCursorPos(&p)) {
            dx = (float)(p.x - centre.x);
            dy = (float)(p.y - centre.y);
            SetCursorPos(centre.x, centre.y);
        }
    }
    // Keeps the cursor inside the viewport and returns its centre in screen pixels.
    POINT LockCursor() {
        RECT rc = {};
        GetClientRect(viewport, &rc);
        POINT a = {rc.left, rc.top}, b = {rc.right, rc.bottom};
        ClientToScreen(viewport, &a);
        ClientToScreen(viewport, &b);
        const RECT screen = {a.x, a.y, b.x, b.y};
        ClipCursor(&screen);
        return POINT{(a.x + b.x) / 2, (a.y + b.y) / 2};
    }
};
WinLookPointer g_winLook;

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



const wchar_t* kWindowClass = L"TBDEngineWindow";
const wchar_t* kViewportClass = L"TBDViewport";
const wchar_t* kFloatClass = L"TBDViewportFloat";
const wchar_t* kWindowTitle = L"TBD Engine";
const int kStartWidth = 1280;
const int kStartHeight = 800;

// The Windows editor shell's state: windows, menu, fonts, the docked / floating viewport and the panel layout.
// Not part of the game (src/game); only main.cpp's window code should touch it.
struct Shell {
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

    bool viewportFloating = false;
    RECT floatRect = {};
    bool floatRectValid = false;
    bool showBrowser = true;
    int browserH = 0; // device pixels
    bool splitDrag = false;
    int dockW = 0;    // right dock width in device pixels; 0 = the default
    bool dockDrag = false;
};
Shell g_shell;

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
    GetClientRect(g_shell.window, &rc);
    Areas a = {};
    const int top = tbd::Toolbar_Height(g_engine.dpi);
    const int dockSplitW = Sx(5);
    const int minDock = Sx(220), maxDock = std::max(minDock, (int)rc.right - Sx(320) - dockSplitW);
    const int dockW = std::clamp(g_shell.dockW > 0 ? g_shell.dockW : Sx(300), minDock, maxDock);
    const int mainW = std::max(1, (int)rc.right - dockW - dockSplitW);
    a.toolbar = {0, 0, rc.right, top};
    a.dockSplit = {mainW, top, mainW + dockSplitW, std::max(top, (int)rc.bottom)};
    a.dock = {mainW + dockSplitW, top, mainW + dockSplitW + dockW, std::max(top, (int)rc.bottom)};
    int browserH = 0, splitH = 0;
    if (g_shell.showBrowser && g_shell.contentBrowser) {
        splitH = Sx(5);
        const int minH = tbd::ContentBrowser_MinHeight(g_engine.dpi);
        browserH = std::clamp(g_shell.browserH, minH, std::max(minH, (int)rc.bottom - top - Sx(160) - splitH));
    }
    const int tabH = g_shell.viewportFloating ? 0 : tbd::FaceTab_StripHeight(g_engine.dpi);
    a.tabStrip = {0, top, mainW, top + tabH};
    a.viewport = {0, top + tabH, mainW, std::max(top + tabH + 1, (int)rc.bottom - browserH - splitH)};
    tbd::character::Dock(a.viewport, g_engine.dpi); // the Character Creator workspace takes the viewport's left while open
    a.splitter = {0, a.viewport.bottom, mainW, a.viewport.bottom + splitH};
    a.browser = {0, a.splitter.bottom, mainW, std::max((int)a.splitter.bottom, (int)rc.bottom)};
    return a;
}

HWND ViewportHost() { return g_shell.viewportFloating && g_shell.floatWindow ? g_shell.floatWindow : g_shell.window; }

void ViewportSize(int& w, int& h) {
    if (g_shell.viewportFloating && g_shell.floatWindow) {
        RECT rc;
        GetClientRect(g_shell.floatWindow, &rc);
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
    if (!g_shell.toolbar) return;
    RECT rc;
    GetClientRect(g_shell.window, &rc);
    const Areas a = ComputeAreas();
    MoveWindow(g_shell.toolbar, a.toolbar.left, a.toolbar.top, a.toolbar.right - a.toolbar.left,
               a.toolbar.bottom - a.toolbar.top, TRUE);
    const int dockW = (int)(a.dock.right - a.dock.left), dockH = (int)(a.dock.bottom - a.dock.top);
    int detailsH = std::clamp(dockH * 55 / 100, tbd::Inspector_Height(g_engine.dpi), Sx(340));
    if (g_shell.inspector) detailsH = std::max(detailsH, tbd::Inspector_DesiredHeight(g_shell.inspector)); // script rows need room
    detailsH = std::min(detailsH, std::max(0, dockH - Sx(100))); // the outliner keeps at least 100 px
    const int outlinerH = dockH - detailsH;
    if (g_shell.outliner) MoveWindow(g_shell.outliner, a.dock.left, a.dock.top, dockW, outlinerH, TRUE);
    if (g_shell.inspector) MoveWindow(g_shell.inspector, a.dock.left, a.dock.top + outlinerH, dockW, detailsH, TRUE);
    if (g_shell.contentBrowser) {
        MoveWindow(g_shell.contentBrowser, a.browser.left, a.browser.top, a.browser.right - a.browser.left,
                   a.browser.bottom - a.browser.top, TRUE);
        ShowWindow(g_shell.contentBrowser, g_shell.showBrowser ? SW_SHOWNA : SW_HIDE);
    }
    int w, h;
    ViewportSize(w, h);
    int vx = g_shell.viewportFloating ? 0 : (int)a.viewport.left, vy = g_shell.viewportFloating ? 0 : (int)a.viewport.top;
    RECT live;
    if (!g_shell.viewportFloating && tbd::FaceTab_ViewportRect(a.viewport, &live)) { // the tab shows the live character
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
    if (g_shell.viewportWindow) MoveWindow(g_shell.viewportWindow, vx, vy, w, h, TRUE);
    tbd::character::PlaceDock();
    if (tbd::FaceTab_IsOpen() && !g_shell.viewportFloating)
        tbd::FaceTab_Layout(a.tabStrip, a.viewport, g_shell.viewportWindow);
    else if (g_shell.viewportWindow)
        ShowWindow(g_shell.viewportWindow, SW_SHOWNA);
    if (g_engine.renderer && rc.right > 0 && rc.bottom > 0) g_engine.renderer->Resize(w, h);
    InvalidateRect(g_shell.window, nullptr, FALSE);
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
    if (g_shell.viewportWindow) {
        DestroyWindow(g_shell.viewportWindow);
        g_shell.viewportWindow = nullptr;
    }
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

bool StartRenderer(Api api) {
    StopRenderer();
    int w, h;
    ViewportSize(w, h);
    g_shell.viewportWindow = CreateWindowExW(0, kViewportClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w, h,
                                              ViewportHost(), nullptr, g_shell.instance, nullptr);
    if (!g_shell.viewportWindow) return false;

    if (api == Api::D3D12) { // the first start after a shader change compiles them all: the window stays alive and says so
        std::atomic<int> done{0}, total{0};
        std::atomic<bool> stop{false};
        std::thread compile([&] { tbd::PrecompileD3D12Shaders(done, total, stop); done = total.load() + 1; });
        wchar_t was[256] = {};
        GetWindowTextW(g_shell.window, was, 256);
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
            if (done > 0 && now - shown > 200 && g_shell.window) {
                wchar_t title[320];
                swprintf(title, 320, L"%s - compiling shaders %d of %d (first start only)", was, done.load(), total.load());
                SetWindowTextW(g_shell.window, title);
                shown = now;
            }
            Sleep(10);
        }
        compile.join();
        if (g_shell.window) SetWindowTextW(g_shell.window, was);
        if (!g_engine.running) return false;
    }
    std::unique_ptr<tbd::Renderer> r = api == Api::D3D12 ? tbd::CreateD3D12Renderer() : tbd::CreateVulkanRenderer();
    if (r) r->SetRenderScale(RenderScale());
    if (!r || !r->Init(g_shell.viewportWindow, w, h, g_engine.validation)) {
        tbd::Log("%s: renderer failed to start", ApiName(api));
        DestroyWindow(g_shell.viewportWindow);
        g_shell.viewportWindow = nullptr;
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
        MessageBoxW(g_shell.window, L"That graphics API could not be started on this machine; staying on the previous one.",
                    kWindowTitle, MB_ICONWARNING);
        StartRenderer(previous);
    }
}

// ------------------------------------------------------------ viewport pop-out

bool CreateFloatWindow() {
    RECT main;
    GetWindowRect(g_shell.window, &main);
    int x = main.left + Sx(60), y = main.top + Sx(60), w = Sx(1100), h = Sx(680);
    if (g_shell.floatRectValid) {
        x = g_shell.floatRect.left;
        y = g_shell.floatRect.top;
        w = g_shell.floatRect.right - g_shell.floatRect.left;
        h = g_shell.floatRect.bottom - g_shell.floatRect.top;
    }
    // Owned by the main window, so it stays in front of it and minimizes with it.
    g_shell.floatWindow = CreateWindowExW(0, kFloatClass, L"Viewport - TBD Engine  (F4 docks it back)",
                                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, g_shell.window, nullptr,
                                           g_shell.instance, nullptr);
    return g_shell.floatWindow != nullptr;
}

void DestroyFloatWindow() {
    if (!g_shell.floatWindow) return;
    GetWindowRect(g_shell.floatWindow, &g_shell.floatRect);
    g_shell.floatRectValid = true;
    DestroyWindow(g_shell.floatWindow); // its WM_DESTROY clears g_shell.floatWindow
}

void SetViewportFloating(bool floating) {
    if (floating == g_shell.viewportFloating) return;
    StopRenderer(); // takes the viewport window with it
    if (floating) {
        if (!CreateFloatWindow()) {
            StartRenderer(g_engine.api);
            return;
        }
        g_shell.viewportFloating = true;
        ShowWindow(g_shell.floatWindow, SW_SHOW);
    } else {
        g_shell.viewportFloating = false;
        DestroyFloatWindow();
    }
    Layout();
    if (!StartRenderer(g_engine.api)) { // could not rebuild in the new place: go back
        if (g_shell.viewportFloating) {
            g_shell.viewportFloating = false;
            DestroyFloatWindow();
        }
        Layout();
        StartRenderer(g_engine.api);
    }
    Layout();
    if (g_shell.viewportWindow) SetFocus(g_shell.viewportWindow);
    InvalidateRect(g_shell.window, nullptr, TRUE);
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
        const int pick = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_shell.window, nullptr);
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
    tbd::Inspector_ShowScripts(g_shell.inspector, sel != nullptr, list);
    static int lastDesired = 0;
    const int desired = tbd::Inspector_DesiredHeight(g_shell.inspector);
    if (desired != lastDesired) { lastDesired = desired; Layout(); } // the panel grows to show every script row
}

void UpdateInspector() {
    if (!g_shell.inspector) return;
    const tbd::Entity* sel = g_engine.scene.Selected();
    float values[9] = {};
    if (sel) {
        const tbd::Vec3 euler = tbd::EulerFromQuat(sel->rotation);
        const float v[9] = {sel->position.x, sel->position.y, sel->position.z, euler.x, euler.y,
                            euler.z, sel->scale.x, sel->scale.y, sel->scale.z};
        std::copy(v, v + 9, values);
    }
    tbd::Inspector_Show(g_shell.inspector, sel ? sel->name.c_str() : nullptr, values);
    UpdateInspectorScripts(sel);
}

bool PickSceneFile(bool save, std::wstring& path) {
    wchar_t buf[tbd::platform::kMaxPath] = {};
    if (save) wcsncpy_s(buf, path.empty() ? L"Untitled.tbdscene" : path.c_str(), _TRUNCATE);
    const std::wstring dir = ScenesFolder();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_shell.window;
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

#include "undo_scene.inl" // Ctrl+Z / Ctrl+Y: scene checkpoints and the undo popup

// ---- Character creator (src/character/creator.h): what the panel may touch. Made once, on the first frame.
void CharacterCreatorHost() {
    if (tbd::character::HostSet() || !g_shell.window) return;
    tbd::character::Host h;
    h.instance = g_shell.instance;
    h.parent = g_shell.window;
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
    h.viewportFloating = [] { return g_shell.viewportFloating; }; // Character Creator: the 2D view fills the freed space
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
        if (!g_faceDriver.live.Running()) {
            std::string error;
            if (!g_faceDriver.live.Start(-1, nullptr, (tbd::ExeDirectory() + L"\\models\\face").c_str(), &error)) {
                Toast(error);
                return;
            }
        }
        tbd::FaceTabHost host;
        host.instance = g_shell.instance;
        host.parent = g_shell.window;
        host.live = &g_faceDriver.live;
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
            if (on) g_faceDriver.BindLive(e);
            else g_faceDriver.Unbind(e);
        };
        host.overrideHeadPose = [](const float* pose, const float* neutral) {
            tbd::FaceDriver& d = g_faceDriver;
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
        host.setMirror =[](bool m) { g_faceDriver.mirror = m; };
        host.stopCapture = [] { g_faceDriver.StopLive(g_engine.scene); Toast("WEBCAM OFF"); };
        host.startVideoLog = [live = host.live] {
            if (!g_engine.renderer || !g_faceLog.Start(live, tbd::platform::CacheRoot() / L"facelog")) return false; // next to the exe
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
        g_faceDriver.Unbind(*sel);
        Toast("STOPPED DRIVING " + sel->name);
        return;
    }
    wchar_t buf[tbd::platform::kMaxPath] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_shell.window;
    ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = tbd::platform::kMaxPath;
    ofn.lpstrTitle = L"Drive the selected entity with a face clip";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string error;
    if (g_faceDriver.Bind(*sel, buf, &error)) Toast("DRIVING " + sel->name + " WITH " + ToastName(buf));
    else Toast(error);
}

void OnTransport(int action, void*); // defined with the toolbar hooks below

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
    g_faceDriver.Unbind(g_engine.scene.entities[fs.target]); // a clip bound in the scene would turn the head
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
        DestroyWindow(g_shell.window);
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
bool SimRemote(const std::vector<std::string>& a, std::string& out); // city_overlays.inl: the "sim" command
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
    if (cmd == "sim") return SimRemote(a, out); // sim|stats / overlay / budget / plant / boost (city_overlays.inl)
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

// ---- Windows: the viewport window's procedure turns messages into VpEvents.
struct WinPointerHost : PointerHost {
    HWND hwnd;
    explicit WinPointerHost(HWND h) : hwnd(h) {}
    void Focus() override { SetFocus(hwnd); }
    void Capture() override { SetCapture(hwnd); }
    void Release() override { ReleaseCapture(); }
};

bool TranslateViewportMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, VpEvent& e) {
    e.x = GET_X_LPARAM(lparam);
    e.y = GET_Y_LPARAM(lparam);
    switch (msg) {
    case WM_CHAR: e.type = VpEv::Char; e.ch = (uint32_t)wparam; return true;
    case WM_KEYDOWN: e.type = VpEv::KeyDown; e.key = (int)wparam; e.repeat = (lparam & (1 << 30)) != 0; return true;
    case WM_LBUTTONDOWN: e.type = VpEv::LeftDown; return true;
    case WM_RBUTTONDOWN: e.type = VpEv::RightDown; return true;
    case WM_MBUTTONDOWN: e.type = VpEv::MiddleDown; return true;
    case WM_LBUTTONUP: e.type = VpEv::LeftUp; return true;
    case WM_RBUTTONUP: e.type = VpEv::RightUp; return true;
    case WM_MBUTTONUP: e.type = VpEv::MiddleUp; return true;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: e.type = VpEv::OtherButton; return true; // only Play uses these (swallows them)
    case WM_CAPTURECHANGED: e.type = VpEv::CaptureLost; return true;
    case WM_MOUSEMOVE: e.type = VpEv::Move; return true;
    case WM_MOUSEWHEEL: { // wheel messages carry screen coordinates
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        e.x = pt.x;
        e.y = pt.y;
        e.type = VpEv::Wheel;
        e.wheel = (float)GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
        return true;
    }
    }
    return false;
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (GuiMouse(hwnd, msg, wparam, lparam)) return 0; // in-game GUI first: it eats clicks, drags and wheel over its panels
    if ((msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && !tbd::platform::KeyDownSync(tbd::platform::Key::Alt) && !g_play.active &&
        tbd::character::ViewportMouse(hwnd, msg, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) return 0; // Character Creator hair brush
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: // the render loop owns this window's contents
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    VpEvent e;
    if (TranslateViewportMessage(hwnd, msg, wparam, lparam, e)) {
        WinPointerHost host(hwnd);
        if (ViewportInput(e, host)) return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------- windows

void UpdateMenu() {
    if (!g_shell.menu) return;
    auto check = [](UINT id, bool on) { CheckMenuItem(g_shell.menu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    check(IDM_POPOUT, g_shell.viewportFloating);
    check(IDM_BROWSER, g_shell.showBrowser);
    check(IDM_PROFILER, g_profiler.Enabled());
    check(IDM_TERRAIN, g_engine.terrain.params.enabled);
    check(IDM_SUN_SKY, g_engine.sun.showSky);
    check(IDM_SUN_ANIMATE, g_engine.sun.animate);
    check(IDM_SUN_SHADOWS, g_engine.sun.shadows);
    CheckMenuRadioItem(g_shell.menu, IDM_GFX_FULL, IDM_GFX_UNLIT, IDM_GFX_FULL + g_gfxQuality, MF_BYCOMMAND);
    CheckMenuRadioItem(g_shell.menu, IDM_D3D12, IDM_VULKAN, g_engine.api == Api::D3D12 ? IDM_D3D12 : IDM_VULKAN, MF_BYCOMMAND);
    const int radius = g_engine.terrain.params.radius;
    CheckMenuRadioItem(g_shell.menu, IDM_RADIUS_NEAR, IDM_RADIUS_FAR,
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
    case IDM_EXIT: DestroyWindow(g_shell.window); break;
    case IDM_DUPLICATE: DuplicateSelected(); break;
    case IDM_DELETE: DeleteSelected(); break;
    case IDM_POPOUT: SetViewportFloating(!g_shell.viewportFloating); break;
    case IDM_BROWSER:
        g_shell.showBrowser = !g_shell.showBrowser;
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
    case IDM_BRIGHT_SLIDER: ShowBrightnessSlider(g_shell.window); break;
    }
}

// Play also hands the viewport to the first-person camera (play_mode.h); Stop, or Esc in the viewport, gives it back with
// the editor camera as it was. The sun cycle is unchanged: Play starts it, Pause freezes it, Stop returns it to the start hour.
void OnTransport(int action, void*) {
    switch (action) {
    case 0:
        CompleteSceneLoad();
        if (!g_play.active && g_shell.viewportWindow && g_play.Begin(g_winLook.For(g_shell.viewportWindow), g_engine.camera)) {
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
bool BrowserShown(void*) { return g_shell.showBrowser; }
bool ViewportIsFloating(void*) { return g_shell.viewportFloating; }
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
    if (!g_shell.showBrowser) return false;
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
    if (g_shell.showBrowser && g_shell.contentBrowser) {
        HBRUSH b = CreateSolidBrush(tbd::theme::kLine);
        FillRect(dc, &a.splitter, b);
        DeleteObject(b);
    }
    if (g_shell.viewportFloating) { // placeholder where the viewport used to be
        HBRUSH b = CreateSolidBrush(kClearColor);
        FillRect(dc, &a.viewport, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g_shell.uiFont);
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
            g_shell.splitDrag = true;
            SetCapture(hwnd);
        } else if (OverDockSplit(x, y)) {
            g_shell.dockDrag = true;
            SetCapture(hwnd);
        } else if (g_shell.viewportFloating) {
            const RECT v = ComputeAreas().viewport;
            const POINT pt = {x, y};
            if (PtInRect(&v, pt)) SetViewportFloating(false);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_shell.splitDrag) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int minH = tbd::ContentBrowser_MinHeight(g_engine.dpi);
            const int maxH = std::max(minH, (int)rc.bottom - tbd::Toolbar_Height(g_engine.dpi) - Sx(160) - Sx(5));
            g_shell.browserH = std::clamp((int)rc.bottom - GET_Y_LPARAM(lparam) - Sx(3), minH, maxH); // kept in range, so no dead zone past the limits
            Layout();
        } else if (g_shell.dockDrag) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_shell.dockW = (int)rc.right - GET_X_LPARAM(lparam) - Sx(2); // ComputeAreas clamps it
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_shell.splitDrag || g_shell.dockDrag) {
            g_shell.splitDrag = false;
            g_shell.dockDrag = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_shell.splitDrag = false;
        g_shell.dockDrag = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_shell.splitDrag || OverSplitter(pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (g_shell.dockDrag || OverDockSplit(pt.x, pt.y)) {
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
        if (g_shell.toolbar) RenderFrame(); // keeps the view alive while the window is dragged
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
        if (wparam != SIZE_MINIMIZED && g_shell.viewportFloating) {
            Layout();
            RenderFrame();
        }
        return 0;
    case WM_SETFOCUS: // keys go to the viewport window inside
        if (g_shell.viewportWindow) SetFocus(g_shell.viewportWindow);
        return 0;
    case WM_DESTROY:
        if (hwnd == g_shell.floatWindow) g_shell.floatWindow = nullptr;
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
    g_shell.instance = instance;
    g_engine.titleBase = title;
    g_engine.tickFreq = tbd::platform::TickFrequency();
    g_engine.jobs = std::make_unique<tbd::JobSystem>(); // every hardware thread but one (the main thread)
    g_shell.uiFont = CreateFontW(-Sx(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
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
    g_shell.menu = BuildMenu(); // not attached to the window: the toolbar draws its popups as the menu strip
    AdjustWindowRect(&rect, style, FALSE);

    g_shell.window = CreateWindowExW(0, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                                      nullptr);
    if (!g_shell.window) return false;
    g_shell.accel = BuildAccelerators();

    g_shell.browserH = tbd::ContentBrowser_DefaultHeight(g_engine.dpi);
    g_engine.tools.onAction = OnToolAction;
    tbd::ToolbarHost bar;
    bar.menu = g_shell.menu;
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
    g_shell.toolbar = tbd::Toolbar_Create(g_shell.window, instance, g_engine.dpi, bar);
    tbd::OutlinerSource source;
    source.count = OutlinerCount;
    source.name = OutlinerName;
    source.kind = OutlinerKind;
    source.selected = OutlinerSelected;
    source.select = OutlinerSelect;
    source.command = OnOutlinerCommand;
    g_shell.outliner = tbd::Outliner_Create(g_shell.window, instance, g_engine.dpi, source);
    g_shell.inspector = tbd::Inspector_Create(g_shell.window, instance, g_engine.dpi, OnInspectorEdit, nullptr);
    if (g_shell.inspector) tbd::Inspector_SetScriptHandler(g_shell.inspector, OnInspectorScriptEdit, nullptr);
    tbd::ContentBrowserCallbacks callbacks;
    callbacks.openAsset = OnOpenAsset;
    g_shell.contentBrowser =
        tbd::ContentBrowser_Create(g_shell.window, instance, g_engine.dpi, g_engine.assetsRoot, callbacks);
    g_engine.running = true;
    Layout();

    // Requested API first, the other one as a fallback.
    const Api first = g_engine.api, second = first == Api::D3D12 ? Api::Vulkan : Api::D3D12;
    ShowWindow(g_shell.window, show); // before the renderer: a first D3D12 start compiles shaders for a while
    UpdateWindow(g_shell.window);
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
            if (!g_play.active && g_shell.window && g_shell.accel && TranslateAcceleratorW(g_shell.window, g_shell.accel, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_engine.running) break;
        const HWND host = ViewportHost();
        if (IsIconic(g_shell.window) || (host && IsIconic(host))) {
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
    if (g_shell.floatWindow) DestroyWindow(g_shell.floatWindow);
    if (g_shell.window) {
        DestroyWindow(g_shell.window);
        g_shell.window = nullptr;
    }
    if (g_shell.accel) DestroyAcceleratorTable(g_shell.accel);
    if (g_shell.menu) DestroyMenu(g_shell.menu);
    if (g_shell.uiFont) DeleteObject(g_shell.uiFont);
    UnregisterClassW(kViewportClass, g_shell.instance);
    UnregisterClassW(kFloatClass, g_shell.instance);
    UnregisterClassW(kWindowClass, g_shell.instance);
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

// ---- tbd::shell (src/game/shell.h), Windows implementation
bool tbd::shell::PickSceneFile(bool save, std::wstring& path) { return ::PickSceneFile(save, path); }
void tbd::shell::Transport(int action) { OnTransport(action, nullptr); }
bool tbd::shell::DevReloadTick() {
    if (g_hotShaders.Tick(g_engine.renderer.get(), g_engine.api == Api::Vulkan)) { // new SPIR-V: the Vulkan renderer starts again around the scene
        tbd::Log(StartRenderer(Api::Vulkan) ? "Vulkan: shaders reloaded" : "Vulkan: restart after shader reload failed");
        if (!g_engine.renderer) StartRenderer(Api::D3D12);
        if (!g_engine.renderer) return false;
    }
    return true;
}
void tbd::shell::PumpRemote() { RemotePump(); }
void tbd::shell::EditorTick(float dt) {
    g_faceDriver.Update(dt, g_engine.scene);
    tbd::FaceTab_Tick();
    CharacterCreatorHost(); tbd::character::Tick(); // Character panel: at most one re-deform per frame; TBD_CHARPRESET
    scene_undo::Checkpoint(); // an editor edit that just finished becomes one undo step
}
void tbd::shell::RefreshInspector() { UpdateInspector(); }
bool tbd::shell::CaptureThisFrame() { return RemoteShotThisFrame(); }
void tbd::shell::AfterCapture(tbd::Renderer& r) { RemoteShotAfterFrame(r); }
void tbd::shell::AfterFrame(tbd::Renderer& r) {
    if (g_faceLog.Active()) {
        static std::vector<uint8_t> shot;
        int shotW = 0, shotH = 0;
        if (r.CapturedFrame(shot, shotW, shotH)) g_faceLog.OnFrame(shot, shotW, shotH);
    }
    if (g_shot.active) FaceShotStep(r);
}
void tbd::shell::GameplayTick(float dt, bool playing) { g_game.Tick(dt, playing); }
void tbd::shell::Quit() { PostQuitMessage(0); }
float tbd::shell::EditorCameraScale() { return tbd::character::CameraScale(); }
bool tbd::shell::EditorBusy() { return tbd::character::Busy(); }
bool tbd::shell::EditorHidesHeadHair() { return tbd::character::HideHeadHair(); }
bool tbd::shell::EditorBrushLines(std::vector<tbd::Vertex>& lines) { return tbd::character::BrushLines(lines); }
bool tbd::shell::EditorClayDraw(std::vector<tbd::Vertex>& tris, std::vector<tbd::Vertex>& lines) { return tbd::character::ClayDraw(tris, lines); }
void tbd::shell::SetTitle(const std::wstring& title) { SetWindowTextW(g_shell.window, title.c_str()); }
void tbd::shell::OnSceneSaved() { if (g_shell.contentBrowser) tbd::ContentBrowser_Refresh(g_shell.contentBrowser); }
void tbd::shell::OnToolChanged() { tbd::Toolbar_Refresh(g_shell.toolbar); }
void tbd::shell::Alert(const std::wstring& text) { MessageBoxW(g_shell.window, text.c_str(), kWindowTitle, MB_ICONWARNING); }
bool tbd::shell::AppHasFocus() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

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
        if (g_faceDriver.live.StartPhone(tbd::kLiveLinkPort, &error)) OnToolAction(tbd::ToolId::FaceLive);
        else Toast(error);
    }
    if (!args.shotDir.empty() || !args.videoPath.empty()) {
        g_shot.clipPath = args.shotClip;
        g_shot.dir = args.shotDir;
        g_shot.video = !args.videoPath.empty();
        g_shot.videoPath = args.videoPath;
        if (!args.shotDir.empty()) std::filesystem::create_directories(args.shotDir);
        if (g_shot.video) SetWindowPos(g_shell.window, nullptr, 0, 0, 1800, 1100, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        FaceShotStart();
    }
    Engine_Run();
    Engine_Shutdown();
    tbd::MarkCleanExit();
    return 0;
}
