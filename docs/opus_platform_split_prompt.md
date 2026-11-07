# Opus brief: split the game from Win32 so it can build for iOS (Metal)

You are working in the `cityengine` repo (C++20, Windows, D3D12 + Vulkan). A separate worktree/branch `ios-metal`
(`C:\custom windows apps\cityengine-ios`, remote `codemagic` -> github.com/tlanch4837/game) already builds an unsigned iOS
app through Codemagic: `CMakeLists.txt` (Xcode generator), `src/render/renderer_metal.mm`, `src/platform/ios/main_ios.mm`
(UIKit scene + CADisplayLink), `src/platform/apple/log_apple.cpp`, 40 committed `src/shaders/msl/*.metal`, and
`tools/ci/build_ios.sh`. It currently draws one triangle and that is verified working on an iPad (iPadOS 26.0).

## Goal
Make the shared game code free of Win32 so it can be compiled into the iOS target, WITHOUT rewriting the game and
WITHOUT breaking the Windows build. One game codebase; a small platform shell per OS; the renderer backend per OS
(D3D12/Vulkan on Windows, Metal on iOS).

## Ground rules
- The Windows build (`build.bat`) must keep working after every step. Build and run it after each change. Do not
  leave it broken between commits.
- Small commits, one logical step each. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not touch the face mocap / character-maker code (`src/mocap/*`, `src/character/*`): they stay Windows-only and
  must NOT be added to the iOS target. Do not touch third-party code (`src/third_party/*`) except to configure it.
- Do not touch the simulation/render design docs. This task is only about portability.
- Do not delete behaviour. Where a Win32 call is replaced, the Windows behaviour must be identical.
- The iOS build can only be compiled on Codemagic (Mac). You cannot run it locally. Keep iOS-facing code small,
  conservative, and free of anything you cannot reason about from the headers. Do not claim an iOS build works until a
  Codemagic build has actually succeeded. Codemagic minutes are limited (about 200 left): batch changes, push rarely.
- Never put the user's credentials anywhere. Never push to a remote without being asked.

## Known facts (from a first scan; verify, do not trust blindly)
- Files under `src/city/*` and `src/city_*.inl` do not include Windows headers directly, but the `.inl` files are
  textually included by `src/main.cpp`, so they depend on whatever `main.cpp` defines (globals, helpers, the renderer).
- Files that include Windows/D3D headers or use Win32 types include: `main.cpp` (about 37 hits), `render/log.cpp`,
  `render/renderer.h`, `core/jobs.h`, `physics.cpp`, `audio.cpp`, `image_decode.h`, `hot_reload.h`, `cooked_asset.h`,
  `mesh_lod.h`, `stats.h`, `profiler_overlay.h`, `ui_theme.h`, `project.h`, `remote_control.h`, `undo.h`,
  `play_mode.h`, `ai/nav_mesh.cpp`, `ai/npc_system.cpp`. Editor-only UI (probably stays Windows-only):
  `inspector`, `outliner`, `content_browser`, `tool_panel`.
- `src/render/renderer_d3d12.cpp` and `renderer_vulkan.cpp` stay Windows-only. `renderer_metal.mm` is the iOS backend.
  `src/render/renderer.h` is the shared renderer interface and must become platform-neutral (it currently pulls in
  Win32 headers).
- `miniaudio.h` already supports iOS; it needs configuring, not porting.
- Shaders: HLSL is the source of truth (`src/shaders/*.hlsl`). `msl.bat` (Windows) regenerates
  `src/shaders/msl/*.metal` through spirv-cross. The committed MSL may be stale relative to the current
  `mesh.hlsl`; regenerate it with `msl.bat` after HLSL changes and commit the result.

## Step 0: audit first, change nothing
Produce `docs/platform_split_audit.md` containing:
1. Every file in the proposed iOS target with the exact Win32 or Windows-only dependency in it (type, function, header,
   line), grouped as: (a) shared game code to make portable, (b) Windows-only, stays out of iOS, (c) third-party.
2. How `main.cpp` is structured: window creation, message loop, input polling, timing, the frame/update/render order,
   global state, and which `.inl` files it includes in what order. Identify what truly needs a window handle.
3. A proposed minimal platform interface (below), with the exact set of functions the shared code actually needs
   (grep for real usage; do not invent an API the code does not use).
4. A dependency order and an estimated size per step.
Stop after writing it and wait for approval before editing code.

## Step 1: thin platform layer
Create `src/platform/platform.h` (portable interface) with `platform_win.cpp` and `platform_ios.mm` implementations.
Keep it as small as the audit justifies. Likely contents:
- monotonic time (replace `QueryPerformance*`, `GetTickCount`),
- logging (replace `OutputDebugString`; `log_apple.cpp` already exists for Apple),
- file paths and file read/write (user data dir, bundle resource dir; replace `CreateFile`, `SHGetFolderPath`),
- threads, mutexes, job system primitives (replace `CreateThread`, `CRITICAL_SECTION`, `WaitForSingleObject`;
  prefer `std::thread`/`std::mutex`/`std::atomic` and delete the Win32 version where behaviour is identical),
- an input event queue: pointer/touch down-move-up, keys, scroll, and a "pointer position" query, replacing
  `GetAsyncKeyState` / `GetCursorPos` in shared code. The Windows shell feeds it from the message loop; the iOS shell
  feeds it from UIKit touches. Include multitouch (pinch, two-finger pan) because iOS needs it for the camera.
- window/surface size and DPI scale, and a "suspended/resumed" lifecycle callback.
Do not add a general abstraction beyond what the shared code calls.

## Step 2: make `renderer.h` and shared headers platform-neutral
Remove `#include <windows.h>`, `<d3d12.h>`, `<wrl.h>` and friends from every header that the shared code includes.
Use opaque handles or forward declarations. Win32/D3D types live only in the `.cpp` backends. Verify the Windows build.

## Step 3: split `main.cpp`
Extract the game loop and city setup into a platform-free module (for example `src/game/game.h/.cpp`):
`game_init(platform, renderer)`, `game_update(dt)`, `game_render()`, `game_on_input(event)`, `game_resize(w,h,scale)`,
`game_suspend()`, `game_resume()`. `src/main.cpp` becomes a thin Windows shell: create window, pump messages, translate
to platform events, call the game module. The `.inl` city files get included by the game module, not by the shell.
Keep globals if the audit shows untangling them is out of scope, but make sure they no longer require Win32 headers.
The Windows build must behave identically (same frame order, same input handling).

## Step 4: port the leftover shared files
For each remaining file in group (a): replace the Win32 calls with the platform layer or the standard library. Editor-only
files stay Windows-only; guard them with the build system (not scattered `#ifdef _WIN32` inside shared logic).

## Step 5: iOS target
- Extend `CMakeLists.txt` `TBD_SOURCES` with the now-portable files, adding them in small groups so a Codemagic failure
  points at one group. Do not add group (b) files.
- Replace the triangle in `main_ios.mm` with the game module: `main_ios.mm` feeds touches into the platform event
  queue and drives `game_update/game_render` from the display link. Keep the existing lifecycle behaviour (display
  link paused when inactive; scene delegate).
- Rebuild and re-verify `tools/ci/build_ios.sh` steps (arch, MinimumOSVersion 16.0, metallib and msl compare, IPA zip).
- Keep unsigned IPA output for Sideloadly. The target is arm64, iOS 16+, tested on an iPad on iPadOS 26.

## Step 6: report
Write `docs/platform_split_report.md`: what moved where, what stays Windows-only, what is still unverified on a
device, and the next steps (touch UI, audio, GUI, asset loading, performance on device).

## Definition of done
- Windows build passes, behaviour unchanged.
- Shared game code compiles with no Windows headers (prove with a grep in the report).
- A Codemagic `ios-sideload-ipa` build succeeds with the game module in the target, and the user can install it.
- Everything unverified is listed explicitly. Do not report an iOS success you did not see.
