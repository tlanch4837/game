# Platform split audit (Step 0: no code changed)

Branch scanned: `city-sim-lod` working tree, 2026-11-07, uncommitted sim/LOD work included.
Method: grep for Windows headers and Win32 identifiers (`HWND HANDLE DWORD LARGE_INTEGER QueryPerformance* GetAsyncKeyState GetCursorPos CreateFile* WaitForSingleObject FindFirstFile Interlocked* Sleep MessageBox ComPtr`) across `src/` excluding `third_party`.

## Working alongside the sim / LOD work

This branch has a large set of uncommitted sim and LOD changes (`city/sim_*.h`, `city_sim.inl`, `city_buildings.inl`, `mesh_lod.h`, `render/chunk_faces.h`, renderers, `mesh.hlsl`). The split must not fight them:

* **Commit or stash the sim/LOD work first.** The platform split starts on a clean tree, so each split commit only holds portability changes and the two can be reviewed and bisected apart.
* **The sim is already portable.** `src/city/*.h` (all `sim_*.h`, `zoning.h`, `transport*.h`, `utilities.h`, `road_network.h`, `news_feed.h`) and `src/gui/*.h` have **zero** Win32 hits. The sim tests (`tests/sim_*_test.cpp`) already build without Windows. Keep it that way: new sim/LOD code uses the platform clock (Step 1) instead of `LARGE_INTEGER`.
* **The LOD path is where they meet.** `city_buildings.inl` (Track R building LODs), `world_streaming.inl`, `surface_async.inl` and `mesh_lod.h` measure time budgets with `QueryPerformanceCounter`. These are the first callers moved to `platform::Now()` in Step 1, so the LOD budgets keep identical behaviour on Windows and work on iOS. Touch those files in one small commit right after the sim/LOD work lands, to avoid merge conflicts.
* **Shaders.** `mesh.hlsl` and the new `chunk_faces.hlsli` are modified, so the committed `src/shaders/msl/*.metal` are stale. Regenerate with `msl.bat` after the LOD shader work is final, not before.

## 1. Files and their Windows dependencies

### (a) Shared game code to make portable

| File | Dependency (line) | Fix |
|---|---|---|
| `render/renderer.h` | `<windows.h>` (10); `Init(HWND, …)` (194); `LARGE_INTEGER`/QPC (445) | `Init(void* nativeSurface, …)`; time via platform |
| `core/jobs.h` | `<windows.h>` (3); `SetThreadPriority(native_handle, BELOW_NORMAL)` (55) | `platform::LowerThreadPriority(std::thread&)` |
| `stats.h` | `<windows.h>`; `LARGE_INTEGER` QPC (32, 60–61) | platform clock |
| `profiler_overlay.h` | `<windows.h>` (13); QPC (120–147, 326) | platform clock |
| `mesh_lod.h` | `<windows.h>` (17), no Win32 calls found beyond it | drop include, verify |
| `cooked_asset.h` | `CreateFileW` + file mapping (40–61) | `platform::MapFile` (mmap on iOS) |
| `project.h` | `CreateFileW/ReadFile/WriteFile` (31–52) | `std::ifstream/ofstream` (same bytes) |
| `image_decode.h` | WIC + `ComPtr` (5–30) | keep WIC on Windows; stb_image or ImageIO on iOS behind one function |
| `audio.cpp` | `GetFileAttributesW`, `CreateFileW` (105, 352, 442) | `std::filesystem` / `std::ifstream`; miniaudio needs iOS config only |
| `physics.cpp` | `<windows.h>`, QPC (495, 675) | platform clock |
| `ai/nav_mesh.cpp` | QPC (181) | platform clock |
| `ai/npc_system.cpp` | QPC (132, 630) | platform clock |
| `play_mode.h` | `Begin(HWND …)` (177), `GetCursorPos` (240), `GetAsyncKeyState` (254) | input queue + pointer query |
| `undo.h` | toast window `HWND`, `ToastProc` (24–88) | split: undo stack portable, Win32 toast stays Windows |
| `render/log.cpp` | `GetModuleFileNameW`, minidump crash handler, `Interlocked*` | log sink portable; crash handler Windows-only (`log_apple.cpp` exists) |
| `city_buildings.inl` | `LARGE_INTEGER` (294, 492, 616); BMP debug dump with `BITMAPFILEHEADER`/`DWORD` (717) | platform clock; write the BMP header struct by hand |
| `world_streaming.inl` | `LARGE_INTEGER` throughout (137–1089), `g_engine.tickFreq` | platform clock (`double` seconds / `uint64_t` ticks) |
| `surface_async.inl` | QPC (167) | platform clock |
| `undo_scene.inl` | `GetAsyncKeyState(VK_*BUTTON)` (112), `HWND host` (148) | pointer-button query; host goes through shell |
| `city_industry.inl` | 1 identifier hit (Win32 type in a timing/helper line) | platform clock |
| `main.cpp` (game part) | see section 2 | split into `game/` + Windows shell |

### (b) Windows-only, stays out of the iOS target

`main.cpp` shell part, `render/renderer_d3d12.cpp`, `render/renderer_vulkan.cpp`, editor UI (`inspector.*`, `outliner.*`, `content_browser.*`, `tool_panel.*`, `ui_theme.h` menus), `hot_reload.h` (DLL hot reload, `FindFirstFileW`, `CreateProcess`), `remote_control.h` (named pipe), `hub/hub_main.cpp`, all `src/mocap/*`, all `src/character/*`, the brightness slider window in `main.cpp` (104–121), the crash handler in `render/log.cpp`.

`main.cpp` includes the mocap and character headers directly (72–80). The game module must not include them; the Windows shell does, and calls them through the existing `OnToolAction` hooks.

### (c) Third-party

`miniaudio.h` (iOS backend via CoreAudio: configure only), Luau (`script/*`, portable). No edits.

## 2. How `main.cpp` is structured (5791 lines)

* **Entry:** `wWinMain` (5741): crash handlers, DPI awareness, `ParseArgs`, project name check (`MessageBoxW`), `Engine_Init(instance, show, title)`, `audio::Start`, scene/sandbox load, shot/video setup, `Engine_Run()`, `Engine_Shutdown()`.
* **Windows:** main `WindowProc` (5351), child viewport `ViewportProc` (4886, created at 1681 and owned by the renderer), popped-out `FloatProc` (5447). Plus editor child windows (toolbar, outliner, inspector, content browser) held in `Engine` (285–293).
* **Message loop / pump:** `PeekMessageW` loop (1694); renderer switching recreates the viewport window.
* **Timing:** `g_engine.tickFreq`/`lastTick` (`LARGE_INTEGER`, 349); dt at 2335–2338; budgets via QPC everywhere.
* **Input:** two paths. (1) Messages in `ViewportProc` feed `g_guiInput.Move/Down/Up/Wheel` (4848–4879), which the in-game GUI consumes. (2) Polling with `GetAsyncKeyState` for camera/keys (2346, 2379, 3443) and `GetCursorPos` (1839), plus `GetForegroundWindow` focus check (2362).
* **Frame order (`RenderFrame`, 3323):** stats/profiler `BeginFrame` → camera/input update → GUI frame (`g_gui.BeginFrame` 3250 … `EndFrame` 3310) → sim tick under budget (city_sim.inl) → lighting/shadows → culling (3750–3983) → `r.BeginFrame` (4036) → animation update → draws → `r.EndFrame` (4425) → post-frame timing/screenshot capture.
* **`.inl` include order (textual, all depend on `g_engine` and helpers above them):**
  `surface_async` (822), `ground_bake` (823), `terrain_lods` (1076), `vegetation` (1288), `world_streaming` (1319), `undo_scene` (2044), then `city_zones`, `city_utilities`, `city_transport`, `city_industry`, `city_garbage`, `city_game`, `city_sim`, `city_stress`, `city_buildings` (3149–3157).
* **What truly needs a window handle:** only `Renderer::Init` (surface creation), the Win32 shell (message procs, editor panels, dialogs, toasts), `play_mode.h` cursor capture, and `undo_scene.inl`'s popup host. Everything else uses `HWND` only to read size or DPI, which `game_resize(w, h, scale)` replaces.

## 3. Minimal platform interface (only what the code calls)

```cpp
namespace platform {
// time: replaces every QueryPerformanceCounter/Frequency and LARGE_INTEGER use
uint64_t Ticks();              // monotonic
double   TicksToSeconds(uint64_t);
// log: OutputDebugString / engine.log sink (log_apple.cpp already does Apple)
void Log(const char* utf8);
// files: exe/bundle dir, user data dir, mapped read (cooked_asset.h)
std::filesystem::path ResourceDir();   // GetModuleFileNameW dir / NSBundle resourcePath
std::filesystem::path UserDataDir();   // saves, LodCache
struct MappedFile { const uint8_t* data; size_t size; void* impl; };
bool MapFile(const std::filesystem::path&, MappedFile&); void Unmap(MappedFile&);
// threads: jobs.h worker priority
void LowerThreadPriority(std::thread&);
// input: queue fed by the shell, consumed by game_on_input; plus polled state
enum class Key : uint16_t; // only the VK_ codes the game polls
bool KeyDown(Key);  bool PointerButtonDown(int button);  bool PointerPos(float& x, float& y);
struct InputEvent { enum Type { PointerDown, PointerMove, PointerUp, Wheel, KeyDown, KeyUp, Pinch, Pan2 } type;
                    int pointerId; float x, y, dx, dy, scale; Key key; };
// image decode: WIC on Windows, ImageIO on iOS
bool DecodeImage(const uint8_t*, size_t, std::vector<uint8_t>& rgba, int& w, int& h);
}
```
Threads/mutexes: already `std::thread`/`std::mutex`/`std::atomic` in `jobs.h`; no wrapper needed. Lifecycle (`game_suspend/resume`) and size/DPI go through the game module calls, not the platform header.

## 4. Order and size

| Step | Work | Size |
|---|---|---|
| pre | Commit the sim/LOD work on `city-sim-lod` (separate commits) | user's call |
| 1a | `platform.h` + `platform_win.cpp`: clock, log, dirs, MapFile, thread priority | ~150 lines |
| 1b | Replace QPC in stats, profiler, physics, nav_mesh, npc_system, jobs.h | ~60 sites, mechanical |
| 1c | Replace QPC in LOD/streaming `.inl` (world_streaming, surface_async, city_buildings, city_industry) and `g_engine.tickFreq` | ~45 sites; coordinate with LOD work |
| 2 | `renderer.h` neutral (`void*` surface), drop `<windows.h>` from mesh_lod, stats, profiler, jobs, project, cooked_asset | ~10 files small |
| 3a | Input: shell fills an event queue + key/pointer state; replace `GetAsyncKeyState`/`GetCursorPos` in main, play_mode, undo_scene | ~15 sites |
| 3b | Extract `game/game.cpp` from `main.cpp` (Engine state, RenderFrame, city `.inl`s) and leave the Win32 shell + editor in `main.cpp` | largest: ~3000 lines moved, few changed |
| 4 | project.h/audio.cpp to std; image_decode split; undo toast split; log crash handler stays Win | ~6 files |
| 5 | CMake iOS groups, `main_ios.mm` drives the game module, regenerate MSL | 2–3 Codemagic runs |
| 6 | Report | doc |

Unverified: the `city_industry.inl` hit and `mesh_lod.h` include were not read line by line; confirm in Step 1. Nothing has been built for iOS beyond the existing triangle.

**Stopping here for approval before any code change.**
