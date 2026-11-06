# Task: make the in-game GUI look good (smooth UI draw path)

You built the in-game GUI (`src\gui\gui.h`, `src\gui\terrain_toolbar.h`, `docs\gui.md`). It works, but the user says it is **ugly**. The causes: a blocky 5x7 pixel font, square hard-edged boxes, fully opaque panels, no shadows, flat pressed/hover states. Fix that properly. The user chose the full "smooth UI" route, which needs new renderer work on both backends. This UI path will also be used on iOS later, so keep the GUI core platform-independent.

You continue in the same checkout: `C:\custom windows apps\cityengine`. No git. Do not touch `third_party\`. The engine may be running: stop it (`Get-Process tbd_engine | Stop-Process -Force`) before every build. Scratch files go in the scratchpad you already use.

## Goal

A UI that looks like a modern game HUD: anti-aliased rounded rectangles with borders and soft drop shadows, semi-transparent panels, a smooth anti-aliased font at any size, round slider thumbs, pill-shaped toggles, small drawn icons, smooth hover and press transitions. No pixel-font blockiness in the in-game GUI.

## Design the renderer path (you decide the details, keep it small)

Add one new draw call to `Renderer` (src\render\renderer.h), for example `DrawUi(const UiVertex* v, uint32_t count, const Mat4& ortho)`, plus whatever texture handle it needs, implemented in **both** `renderer_d3d12.cpp` and `renderer_vulkan.cpp` and one new shader file `src\shaders\ui.hlsl`. Requirements:
- **Alpha blended, no depth test**, drawn in the overlay phase after `ResolveScene()` like `DrawTransient`.
- Two primitive kinds in one pipeline, switched by a per-vertex mode:
  1. **Rounded rectangle by signed distance in the pixel shader** (anti-aliased edge via `fwidth`/derivatives): fill colour, optional border width and colour, corner radius, and an optional soft shadow (blur radius + offset) drawn as a separate larger quad. Also circles (radius = half size) for slider thumbs and toggle knobs.
  2. **Text from a signed-distance-field glyph atlas** (smooth at any scale, optional thin outline or none).
- Use the existing 48-byte `Vertex` layout if it can carry what you need (position, normal slot, colour with real alpha, uv) to avoid a new vertex format, or define a new one if that is cleaner. Note: in the *old* paths vertex alpha is a material marker, so the UI path must be its own pipeline where alpha is real alpha.
- Keep the old `DrawTransient` path untouched. If the UI pipeline fails to build (shader compile error, unsupported), the GUI must fall back to the existing quad + pixel-font path (`TBD_GUI_CLASSIC=1` should force it, useful for before/after comparison).
- D3D12 compiles `.hlsl` at startup from `bin\shaders` (the build script copies them). Vulkan needs SPIR-V from `spirv.bat` (Vulkan SDK is at `C:\VulkanSDK\1.4.363.0`; `build_city.bat` calls it when `VULKAN_SDK` is set). Make sure the new shader goes through both. Look at how `water.hlsl` / `post.hlsl` do it for the pattern, including the `-DVULKAN` bindings convention.
- Both APIs must render it identically. You can launch the engine on Vulkan: find the command-line switch in `ParseArgs` in `main.cpp`.

## The font

You need a **freely redistributable** font and must not download anything from the internet without telling me. Search the machine first for one with a permissive licence, for example DejaVu Sans (Bitstream Vera licence, allows embedding with its notice) inside Python packages such as matplotlib (`mpl-data\fonts\ttf`), or any OFL font that ships with a tool you can find. Do **not** use Segoe UI, Arial or other Windows system fonts: they cannot be redistributed in a game. If no suitable font exists locally, stop and tell me exactly what you need instead of using a Windows font.
- Bake offline with a small Python script in `tools\` (Pillow is installed; numpy may be): render ASCII 32..126 at a large size (for example 64 px), compute a signed distance field per glyph (an exact or approximate Euclidean distance transform is fine; write it so it runs in a few seconds), pack into an atlas (for example 512x512), and write the atlas as a PNG asset plus a small generated header with the glyph metrics (advance, bearing, atlas rect, line height, ascent) and kerning if easy. Commit the generated files to `src\gui\` and `assets\ui\` or similar; the runtime does not need Pillow. Include the font's licence text next to it.
- The GUI core needs text **measurement** from those metrics (`TextWidth` equivalent) so layout still works headless in the unit tests.
- Load the atlas at startup with the existing texture creation (`image_decode.h`, `Renderer::CreateTexture`) or embed it as data; keep it simple and portable.

## Changes to the GUI core and the toolbar

- `gui.h` should emit backend-agnostic UI primitives (rect, rounded rect with radius/border/shadow, circle, text run, simple icon shapes) into a command/vertex list the new draw call consumes. Keep the public widget API and layout/hit-test behaviour the same, so `main.cpp` integration and the existing tests need minimal changes. Still no `windows.h` in the core.
- **Visual spec** (tune it by looking at screenshots, do not just follow numbers):
  - Panels: dark charcoal at about 88 percent opacity, 1 px lighter border, 8 px corner radius, soft shadow, title bar with a thin accent line, section spacing on an 8 px grid.
  - Text: smooth sans font, base about 15 px at DPI 1 (scaled by DPI), titles small caps style or semibold look (use a slightly larger size and a brighter colour if the font has one weight), values right-aligned in a dimmer colour, no all-caps shouting.
  - Buttons: 6 px radius, subtle top-lighter fill, clear hover, pressed and selected (accent) states with a short colour transition (a few frames of easing is enough).
  - Choice (segmented): one rounded container with the selected segment as a rounded accent pill.
  - Slider: thin rounded track, filled part in accent colour, round thumb with a soft shadow, value in text.
  - Toggle: pill switch with a sliding round knob (animate).
  - Icons: draw small icons for RAISE (up chevron or mountain), DIG (down chevron), FLATTEN (flat bar with arrows) and SAVE/LOAD from shapes (lines, triangles, rounded rects) rather than a texture; keep them crisp and consistent in stroke weight. Put the icon next to or above the label in the toolbar.
  - Status line: a rounded chip with the same style.
  - Minimum touch target 44 px and the DPI scaling rule stay.
- Keep everything the existing tests assert (hit areas, anchors, capture rules, 44 px). Update the tests where the text-measurement or sizes legitimately change and add tests for the new measurement and any new layout rules.

## Verification (required)

1. `tests\gui_test.bat` passes (update or extend it).
2. `build_city.bat` builds with 0 errors; shaders compile for **both** D3D12 and Vulkan.
3. Screenshots, looked at with the Read tool (captures are BMP; convert with Pillow): `TBD_GUI=1 TBD_SHOT="12 0 0 0 3000 0.0 0.9" TBD_CAPTURE=<path>.bmp` on **D3D12 and on Vulkan**, at two window sizes if you can, plus the scripted-pointer states (`TBD_GUI_POINTER`) for hover, pressed and a dragged slider. Also one `TBD_GUI_CLASSIC=1` capture for before/after. Show me you actually looked: describe what you saw and fix what looks wrong (text clipping, blurry text, shadow banding, icon misalignment, alpha fringe at edges).
4. The old behaviour is unchanged: no GUI in captures without `TBD_GUI=1`, key bindings, terrain painting, camera, `U` to hide.
5. Be honest about what you could not verify (real mouse, a real high-DPI display, anything on iOS).

## Constraints

- No third-party libraries. Keep the renderer additions small and well commented; do not refactor existing backend code. If something in the Vulkan backend turns out impractical, do **not** leave the two backends inconsistent: either finish it, or make the Vulkan path report "UI pipeline unavailable" so the classic fallback is used, and say so clearly.
- Add new source files to `build.bat` and `build_city.bat` if they are not header-only.
- Update `docs\gui.md` (new API surface, the font pipeline and how to re-bake it, the fallback switch, the licence).

## Report back (concise)

Files added/changed; the renderer API addition; how the font was produced and its licence; test results; the screenshot paths you viewed (D3D12 and Vulkan, before/after) and what they showed; what you could not verify; known limits.
