# Platform split report

Branches: `platform-split` (worktree `C:\custom windows apps\cityengine-platform`), merged into `ios-metal`
(worktree `C:\custom windows apps\cityengine-ios`, remote `codemagic` -> github.com/tlanch4837/game).
Audit: [platform_split_audit.md](platform_split_audit.md).

## What moved where

| Piece | Where | Notes |
|---|---|---|
| Platform layer | `src/platform/platform.h`, `platform_win.cpp`, `platform_ios.mm` | clock, futex wait, thread priority, cache folder, wide fopen, UTF-8/wide, env vars, tick count, MulDiv, key state, image decode, process memory, physical RAM, case-insensitive compare |
| The game | `src/game/game_core.inl` | engine core, scene loading, terrain/LOD, city tools and sim (all city `.inl`), buildings, RenderFrame, viewport input, play mode; textually included by the host |
| Game -> host calls | `src/game/shell.h` | alerts, title, focus, quit, scene dialog, Play transport, per-frame editor / dev / screenshot hooks, Character Creator hooks |
| Windows host | `src/main.cpp` | windows, menus, docking, inspector / outliner / toolbar / content browser, dialogs, Character Creator, face mocap, hot reload, remote pipe, startup; includes `game_core.inl` |
| Game without the editor | `src/game/game_host.h/.cpp` | editor-free `tbd::shell`, `tbd::host` API (Init, AttachRenderer, StartCity, Resize, Frame, touch / zoom / key input) |
| iOS shell | `src/platform/ios/main_ios.mm` | UIKit scene, CAMetalLayer, display link drives `tbd::host`; one finger = left button, two-finger pan = camera pan, pinch = zoom |
| Renderer interface | `src/render/renderer.h` | no `windows.h`; `Init(void* nativeWindow, ...)`; `g_sceneExposure` declared here |
| Portable small headers | `ui_palette.h`, `bmp_header.h` | palette and BMP headers, checked against the Windows structs with `static_assert` |

## Stays Windows-only

`src/main.cpp` (the editor shell), `renderer_d3d12.cpp`, `renderer_vulkan.cpp`, `src/mocap/*`, `src/character/*`,
`inspector`, `outliner`, `content_browser`, `tool_panel`, `ui_theme.h`, `hot_reload.h`, `remote_control.h`, `undo_toast_win.h`,
`undo_scene.inl`, `render/log.cpp` (crash handler; Apple uses `log_apple.cpp`), `cooked_asset.h` (not used by the engine yet),
the hub and `facemocap.exe`.

## Proof that the shared game code needs no Windows headers

- `tools/probe_game/probe_game.bat` compiles `src/game/game_core.inl` with only portable headers and
  `#error`s if `windows.h` leaks in: 0 errors.
- `tools/probe_game/link_game.bat` compiles `src/game/game_host.cpp` without Windows headers and links it with
  physics, ragdoll, ai, script, audio, Jolt, Recast and Luau: links, and the exe runs.
- The same source set compiles under MSVC `/permissive-`.
- `grep` (run on 2026-11-07): no file in the iOS source list includes `windows.h`; the only `_WIN32` blocks in shared
  headers are the include guards of `image_decode.h` and `undo.h` for the Windows-only callers.

## Windows behaviour

Every step was built with `build.bat` and run. Each Win32 call that moved behind the platform layer is the same call on
Windows (static_asserts check key codes, palette and BMP layouts). Checked after the changes: a `TBD_CAPTURE` run renders
the default scene and exits cleanly; `TBD_CITY_STRESS=400` gives the same population (4768), workers (3109), jobs (3817)
and cells (40) as before the split, and `TBD_CITY_STRESS=200` matches the original exe (2604 / 1699 / 2170 / 31).

## Not verified

- Editor mouse and keyboard by hand (select, drags, wheel, Play, the panels). A synthetic-message test could not drive the
  viewport for either the old or the new exe, so input changes were checked by reading the diff only.
- The nav-mesh cache, the streaming RAM cap and the process-start log line were not reached in the test runs.
- iOS: see the Codemagic section below. Nothing has run on a device yet.

## Codemagic

Workflow `ios-sideload-ipa` on branch `ios-metal`, 2026-11-07:

| Build | Commit | Result | What it showed |
|---|---|---|---|
| #6 | 248a972 | failed (compile) | `std::ifstream` from `std::wstring` (MSVC extension) in mesh_import.h, mesh_lod.h, physics.cpp; `_wfopen` in glb_bounds.h |
| #7 | c0b111d | failed (compile) | `std::filesystem::path::c_str()` is narrow on Apple (city_stress.inl) |
| #8 | 0edd544 | failed (link) | everything compiled; the Jolt / Recast / Luau static libraries were not found because build_ios.sh moves CONFIGURATION_BUILD_DIR |
| #9 | 0c6923a | **passed** | `cityengine.ipa` produced (unsigned, arm64, iOS 16+), with the game module, Jolt, Recast, Luau and the Metal backend |

Not yet seen: the app running on the iPad. Install the build #9 IPA with Sideloadly and check that it launches, draws the
city map (terrain, roads; no models or textures without assets), and that touch, pan and pinch move the camera.

## Next steps

1. Assets on iOS: `assets/` (2.2 GB) is not in git, so the app has no models, textures, UI font or audio. Choose a subset
   (ui, sim tuning, a few building and road kits) and bundle it, or download on first start.
2. Paths: about 30 places build paths with `\`; on iOS those name the wrong files. Switch to `/` (Windows accepts it) or
   `std::filesystem::path` once the asset layout is decided.
3. Touch UI: the in-game GUI is mouse-sized; tap targets, long press for right click, and a Play mode for touch.
4. Audio on device (miniaudio's iOS backend is compiled but untested).
5. Performance on device: render scale, shadow and chunk-bake costs; the Metal backend has no chunk atlas yet, so far
   buildings use the fallback path.
6. The full editor/game split (the rest of main.cpp) if the editor should ever leave Windows.
