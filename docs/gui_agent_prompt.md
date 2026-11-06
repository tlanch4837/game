# Task: add an in-game GUI system to the engine, then use it for a terrain toolbar

You are working in the engine at `C:\custom windows apps\cityengine` (C++20, MSVC, Windows, Direct3D 12 and Vulkan renderers). It is the base for an iOS-targeted city-building game; the game project is `C:\custom windows apps\citysim`, a terrain sandbox on a hand-made island. Not a git repository: do not run git. Do not touch `third_party\`.

## Why

The engine has an editor GUI made of native Windows controls (menus, toolbar, outliner, inspector, content browser) and a tiny immediate-mode 2D layer, `src\ui_draw.h` (filled quads and a built-in 5x7 pixel font, drawn as plain `Vertex` lists with `OrthoPixels`). It has **no in-game GUI toolkit**: no buttons, sliders, panels, layout, hit testing, or a way to stop viewport clicks from passing through widgets. A game (and later the iOS build) needs one. Native Windows controls will not exist on iOS, so the new layer must not depend on them.

## What to build

### 1. A small immediate-mode GUI core: `src\gui\gui.h` (+ `gui.cpp` if you want)

Requirements:
- **No platform headers** in the core (no `windows.h`). It takes input as a plain struct each frame and produces `std::vector<Vertex>` for the existing renderer. This keeps it portable to iOS and unit-testable headless.
- Input struct fed by the caller each frame: mouse position (pixels), left button down, pressed-this-frame and released-this-frame, wheel delta, viewport size in pixels, DPI scale. Design it so touch (one pointer) maps onto it.
- Drawing: build on `ui_draw.h` (`AppendQuad`, `AppendText`, `TextWidth`) and the look in `src\ui_theme.h` / `src\editor_draw.h` (dark charcoal surfaces, thin separators, one blue accent `#0070e0`). Check which characters `detail::FindGlyph` supports; if lowercase letters or needed symbols are missing, extend the font (5x7 rows) rather than avoiding them.
- Widgets, at least: **panel** (optional title, fixed or auto-size), **label**, **button** (returns true on click), **toggle/checkbox**, **radio/segmented choice** (for modes), **horizontal slider** (float, min/max, with value text), **separator**, **spacing**. Hover and pressed visual states. Minimum hit size about 44 px at DPI scale 1.0 for buttons and sliders (touch friendly), and do not rely on hover for anything essential.
- Simple **layout**: vertical and horizontal stacking inside a panel with padding and gaps, widths either fixed or filling the panel. Panels can be anchored to screen corners/edges with an offset (top-left, top-right, bottom-left, bottom-center, ...), recomputed each frame from the viewport size.
- **Mouse capture rule**: after the frame's GUI calls, `gui.WantsMouse()` is true when the pointer is over any panel or a widget is being dragged (a slider drag must keep working when the pointer leaves the panel until release). The engine must not act on clicks or drags that the GUI wants.
- IDs: stable widget identity for hover/active state, from labels plus a push/pop id scope, in the usual immediate-mode way. Keep it simple and well commented; no heap churn per frame beyond the vertex vector (reuse it).
- A **theme struct** (colours, padding, font cell size) with sensible defaults, so the iOS build can scale it.

### 2. Engine integration in `src\main.cpp`

- Feed the GUI from the viewport window procedure (mouse move, button down/up, wheel). Find where `WM_LBUTTONDOWN`, `WM_MOUSEMOVE`, `WM_MOUSEWHEEL` are handled for the viewport (search `case WM_LBUTTONDOWN`; the viewport proc is the one using `HandleKey`, `BeginDrag`, `LeftClickInViewport`). The GUI must see the event first and consume it when `WantsMouse()`; otherwise existing behaviour is unchanged. Mind that GUI hit testing needs the frame's widget rectangles, which are built during drawing: use the previous frame's rectangles for hit testing (standard immediate-mode practice) or equivalent, and document the choice.
- Draw the GUI once per frame near the end of rendering, after `r.ResolveScene()`, with `r.DrawTransient(..., Topology::Triangles, OrthoPixels(w, h), Mat4::Identity(), false)`, the same way `profiler_overlay.h` and `editor_draw.h` draw 2D. Use the viewport size the engine already uses (`g_engine.vpW`, `g_engine.vpH`) and `DpiScale()`.
- It must also draw in the terrain sandbox and be hideable (a key, for example `F2`... check what is free; `F1`..`F7`, `F9`, `F10` are taken by accelerators; pick something free and document it) and must be off in `TBD_SHOT` / `TBD_CAPTURE` runs unless an env var turns it on (`TBD_GUI=1`), so existing capture workflows are unchanged.
- Existing keyboard and mouse behaviour must keep working: T toggles terrain edit, left-drag raises, Shift digs, Ctrl flattens, `[` `]` radius, F8/F11 save/load, camera orbit/fly/pan.

### 3. A simple game GUI: the terrain toolbar

Use the new system to replace the need for modifier keys and to show state, in the terrain sandbox (`g_terrainEdit` in `main.cpp`, the project has `"terrainSandbox": true`):
- A panel (top-left or left edge) titled e.g. TERRAIN with a **mode choice**: RAISE, DIG, FLATTEN. The chosen mode is what left-drag does; holding Shift/Ctrl may still override as today (document it).
- A **BRUSH SIZE** slider (4 to 300 m) bound to `g_terrainEdit.radius` (keep `[` `]` in sync).
- A **STRENGTH** slider (bound to `g_terrainEdit.metresPerSecond`, 1 to 20).
- **SAVE** and **LOAD** buttons (call the existing `SaveTerrainEdits` / `LoadTerrainEdits`).
- An **EDIT ON/OFF** toggle bound to `g_terrainEdit.on`.
- A small status line at the bottom of the screen: cursor world position (east/north in metres) and ground height under the cursor, plus the edited tile count (`g_engine.terrain.field.TileCount()`).
- Clicking on these must not paint terrain underneath.

## Verification (required, do not skip)

1. **Headless unit tests** in `tests\gui_test.cpp` with a `tests\gui_test.bat` like `tests\terrain_field_test.bat` (it compiles with `cl` after calling vcvars64, `/I src`): layout positions, hit testing, button click on press+release inside, slider drag value and capture outside the panel, toggle, `WantsMouse`, anchoring at two viewport sizes and two DPI scales, vertex output non-empty and finite. All checks pass.
2. **Build the engine**: run `build_city.bat` (it builds `bin\tbd_engine.exe` without ONNX Runtime; face tracking is stubbed with `TBD_NO_ONNX`). It first requires that no `tbd_engine.exe` is running: stop it with `Get-Process tbd_engine | Stop-Process -Force` before building. The build takes a few minutes, run it in the background and wait for it. Zero compile errors.
3. **Screenshot proof**: run the engine headless-capture style and look at the result. Example (PowerShell):
   `$env:TBD_GUI="1"; $env:TBD_SHOT="12 0 0 0 3000 0.0 0.9"; $env:TBD_CAPTURE="<scratch>\gui.bmp"; Start-Process bin\tbd_engine.exe -ArgumentList '"C:\custom windows apps\citysim"' -Wait`
   The capture is a **BMP** file even if named .png; convert with Python Pillow (`python -c "from PIL import Image; Image.open(p).convert('RGB').save(q)"`) and view it with the Read tool. Check the panel is legible at the capture's resolution (about 975x483, so check small-window layout: nothing overlapping, nothing off-screen, text not clipped). Capture at least two window sizes if you can (the capture size follows the window).
4. Be honest in your report about what you could and could not verify. You cannot click with a mouse; say so, and test the click paths through the unit tests and, if you want, an env-var driven scripted pointer (for example `TBD_GUI_POINTER="x y down"`) for a capture showing a hovered/pressed state.

## Constraints and style

- Match the existing code style (header-heavy, `namespace tbd`, comments that explain *why*). Keep files small and focused; put the toolbar code in its own file (for example `src\gui\terrain_toolbar.h`) and keep the `main.cpp` changes minimal and clearly marked.
- No third-party libraries. No exceptions across the GUI API. No allocation per widget per frame beyond reusing vectors.
- Add the new `.cpp` file (if any) to `build_city.bat` AND `build.bat` compile lists.
- Write `docs\gui.md`: the API with a short example, the input contract, how the main.cpp integration works, the touch/iOS notes, known limits.
- Do not delete or rewrite existing editor GUI code. Do not change rendering backends unless something forces you to; if you must, say exactly why.

## Report back (concise)

Files added and changed, the API in 10 lines, test results (numbers), the screenshot paths you looked at and what they showed, anything you could not verify, and known limitations.
