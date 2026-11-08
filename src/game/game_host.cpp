// The game without the editor (src/game/game_host.h): the game module plus the editor-free side of tbd::shell.
// Plain C++ so it builds for iOS and links on Windows too (tools/probe_game/link_game.bat checks that it links).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "platform/platform.h"
#include "game/shell.h"
#include "game/game_host.h"
#include "bmp_header.h"
#include "surface_maps.h"
#include "material_lab.h"
#include "audio.h"
#include "camera.h"
#include "city_camera.h"
#include "core/jobs.h"
#include "glb_bounds.h"
#include "render/cloud_noise.h"
#include "render/mesh_weld.h"
#include "culling.h"
#include "mesh_lod.h"
#include "render/chunk_faces.h"
#include "gui/gui.h"
#include "gui/terrain_toolbar.h"
#include "gui/glass_dock.h"    // Glass Dock+: the mobile / tablet GUI (game/glass_dock_host.inl)
#include "gui/city_hud.h"
#include "gui/city_overlays.h"
#include "gui/zone_toolbar.h"
#include "gui/transport_toolbar.h"
#include "gui/utility_toolbar.h"
#include "city/transport_models.h"
#include "city/industry.h"
#include "city/utilities.h"
#include "city/sim.h"
#include "city/sim_children.h"
#include "city/sim_routes.h"
#include "city/sim_cells.h"
#include "city/sim_growth.h"
#include "city/sim_coverage.h"
#include "city/sim_economy.h"
#include "city/sim_budget.h"
#include "city/sim_milestones.h"
#include "city/news_feed.h"
#include "city/news_adjacency.h"   // a new building's neighbours in its radio story (city_radio.inl)
#include "city/sim_flow.h"
#include "city/sim_agents.h"
#include "city/sim_people.h"
#include "gui/road_toolbar.h"
#include "city/road_names.h"
#include "undo.h"
#include "gizmo.h"
#include "mesh_import.h"
#include "skeletal_anim.h"
#include "image_decode.h"
#include "player_character.h"
#include "ai/npc_system.h"
#include "profiler_overlay.h"
#include "project.h"
#include "render/renderer.h"
#include "scene.h"
#include "scene_io.h"
#include "stats.h"
#include "sun.h"
#include "terrain.h"
#include "tools.h"
#include "water.h"
#include "window_portals.h"
#include "physics.h"
#include "script/api_registry.h"
#include "script/luau_host.h"
#include "script/script_remote.h"
#include "play_mode.h"

float g_brightness = 1.4f;    // the renderers read these (main.cpp defines them on Windows)
float g_sceneExposure = 0.0f; // the open scene's fixed exposure, 0 = auto (Scene::exposure)

namespace {
#include "game/game_core.inl"

// The pointer has no window to focus or capture: touches always arrive at the view.
struct NullPointerHost : PointerHost {
    void Focus() override {}
    void Capture() override {}
    void Release() override {}
};
NullPointerHost g_nullHost;
int g_buttonsDown = 0; // bit per Button, so a cancel can lift what is held

VpEv DownEvent(tbd::host::Button b) {
    return b == tbd::host::Button::Left ? VpEv::LeftDown : b == tbd::host::Button::Right ? VpEv::RightDown : VpEv::MiddleDown;
}
VpEv UpEvent(tbd::host::Button b) {
    return b == tbd::host::Button::Left ? VpEv::LeftUp : b == tbd::host::Button::Right ? VpEv::RightUp : VpEv::MiddleUp;
}
tbd::platform::Key ButtonKey(tbd::host::Button b) {
    return b == tbd::host::Button::Left ? tbd::platform::Key::MouseLeft
         : b == tbd::host::Button::Right ? tbd::platform::Key::MouseRight : tbd::platform::Key::MouseMiddle;
}
VpEvent At(VpEv type, float x, float y) {
    VpEvent e;
    e.type = type;
    e.x = (int)std::lround(x);
    e.y = (int)std::lround(y);
    return e;
}
} // namespace

// ---- tbd::shell without the editor: no panels, no dialogs, no hot reload, no remote pipe.
void tbd::shell::Alert(const std::wstring& text) { tbd::Log("%s", tbd::WideToUtf8(text).c_str()); }
bool tbd::shell::AppHasFocus() { return true; } // frames only run while the app is active
void tbd::shell::SetTitle(const std::wstring&) {}
void tbd::shell::OnSceneSaved() {}
void tbd::shell::OnToolChanged() {}
bool tbd::shell::PickSceneFile(bool, std::wstring&) { return false; }
void tbd::shell::Transport(int) {} // Play (first-person) needs mouse look: not on touch yet
bool tbd::shell::DevReloadTick() { return true; }
void tbd::shell::PumpRemote() {}
void tbd::shell::EditorTick(float) {}
void tbd::shell::RefreshInspector() {}
bool tbd::shell::CaptureThisFrame() { return false; }
void tbd::shell::AfterCapture(tbd::Renderer&) {}
void tbd::shell::AfterFrame(tbd::Renderer&) {}
void tbd::shell::GameplayTick(float, bool) {}
void tbd::shell::Quit() {} // a mobile app is never quit by its own code
float tbd::shell::EditorCameraScale() { return 1.0f; }
bool tbd::shell::EditorBusy() { return false; }
bool tbd::shell::EditorHidesHeadHair() { return false; }
bool tbd::shell::EditorBrushLines(std::vector<tbd::Vertex>&) { return false; }
bool tbd::shell::EditorClayDraw(std::vector<tbd::Vertex>&, std::vector<tbd::Vertex>&) { return false; }

// ---- tbd::host
void tbd::host::Init(const std::wstring& assetsRoot) {
    GameInit(assetsRoot);
    // Sound reads <assets>\audio (the shell copies the bundled UI sfx into it on first run, so the folder exists). Without
    // this every audio:: call is a silent no-op; on Windows main.cpp starts it.
    if (!tbd::audio::Start(assetsRoot)) tbd::Log("Audio: not started (no %s\\audio folder, or TBD_NOAUDIO)", tbd::WideToUtf8(assetsRoot).c_str());
}

void tbd::host::ResumeAudio() { tbd::audio::Resume(); }

void tbd::host::AttachRenderer(std::unique_ptr<tbd::Renderer> renderer, int width, int height) {
    if (!renderer) return;
    GameAdoptRenderer(std::move(renderer), Api::Metal, width, height);
}

bool tbd::host::HasRenderer() { return g_engine.renderer != nullptr; }

void tbd::host::StartCity() { GameStartCity(); }

void tbd::host::Resize(int width, int height, float scale) {
    g_engine.dpi = std::max(96, (int)std::lround(96.0f * scale)); // the GUI scales with the screen like a Windows DPI
    GameResize(width, height);
}

void tbd::host::Frame() {
    if (g_engine.renderer) GameFrame();
}

void tbd::host::Shutdown() {
    GameShutdown();
    tbd::audio::Stop();
}

void tbd::host::PointerDown(Button b, float x, float y) {
    g_buttonsDown |= 1 << (int)b;
    tbd::platform::SetKeyDown(ButtonKey(b), true);
    GuiPtrEffect fx;
    if (GuiPointer(b == Button::Left ? GuiPtr::LeftDown : GuiPtr::OtherDown, x, y, 0.0f, fx)) return; // the GUI first, as on Windows
    ViewportInput(At(DownEvent(b), x, y), g_nullHost);
}

void tbd::host::PointerMove(float x, float y) {
    GuiPtrEffect fx;
    if (GuiPointer(GuiPtr::Move, x, y, 0.0f, fx)) return;
    ViewportInput(At(VpEv::Move, x, y), g_nullHost);
}

void tbd::host::PointerUp(Button b, float x, float y) {
    g_buttonsDown &= ~(1 << (int)b);
    tbd::platform::SetKeyDown(ButtonKey(b), false);
    GuiPtrEffect fx;
    if (b == Button::Left && GuiPointer(GuiPtr::LeftUp, x, y, 0.0f, fx)) return;
    ViewportInput(At(UpEvent(b), x, y), g_nullHost);
}

void tbd::host::PointerCancel() {
    GuiPtrEffect fx;
    GuiPointer(GuiPtr::CaptureLost, 0.0f, 0.0f, 0.0f, fx);
    ViewportInput(At(VpEv::CaptureLost, 0.0f, 0.0f), g_nullHost);
    for (int b = 0; b < 3; ++b)
        if (g_buttonsDown & (1 << b)) tbd::platform::SetKeyDown(ButtonKey((Button)b), false);
    g_buttonsDown = 0;
}

bool tbd::host::TouchBuilds(float x, float y) { return g_gui.WantsMouseAt(x, y) || gdock::ToolActive(); }

bool tbd::host::TouchOnGui(float x, float y) { return g_gui.WantsMouseAt(x, y); }

void tbd::host::EdgePan(float dxPx, float dyPx) {
    if (!gdock::EdgePanOn()) return;
    if (!CityCamOn()) { g_engine.camera.Pan(dxPx, dyPx, (float)g_engine.vpH); return; }
    const float cx = (float)g_engine.vpW * 0.5f, cy = (float)g_engine.vpH * 0.5f; // the middle drag's own pan (Drag::Pan), from the centre
    tbd::citycam::PanGround(g_engine.camera, cx, cy, cx + dxPx, cy + dyPx, (float)g_engine.vpW, (float)g_engine.vpH,
                            CityCamGround(g_engine.camera.target.x, g_engine.camera.target.z));
}

void tbd::host::Twist(float radians) { g_engine.camera.yaw -= radians; }

void tbd::host::Tilt(float radians) {
    if (g_play.active) return;
    if (g_terrainSandbox) tbd::citycam::Tilt(g_engine.camera, radians);
    else g_engine.camera.pitch = std::clamp(g_engine.camera.pitch + radians, -1.5f, 1.5f);
}

void tbd::host::Zoom(float notches, float x, float y) {
    GuiPtrEffect fx;
    if (GuiPointer(GuiPtr::Wheel, x, y, notches, fx)) return;
    VpEvent e = At(VpEv::Wheel, x, y);
    e.wheel = notches;
    ViewportInput(e, g_nullHost);
}

void tbd::host::KeyDown(int key, bool repeat, unsigned ch) {
    tbd::platform::SetKeyDown((tbd::platform::Key)key, true);
    VpEvent e;
    e.type = VpEv::KeyDown;
    e.key = key;
    e.repeat = repeat;
    ViewportInput(e, g_nullHost);
    if (ch) {
        VpEvent c;
        c.type = VpEv::Char;
        c.ch = ch;
        ViewportInput(c, g_nullHost);
    }
}

void tbd::host::KeyUp(int key) { tbd::platform::SetKeyDown((tbd::platform::Key)key, false); }
