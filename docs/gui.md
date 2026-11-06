# In-game GUI (`src/gui`)

A small immediate-mode GUI for game screens: panels, labels, buttons, toggles, segmented choices and sliders, with
stroked icons and eased hover / press / slide transitions. It is separate from the editor GUI (native Windows controls),
because those will not exist in the iOS build.

| File | What |
|---|---|
| `src/gui/gui.h` | the core: input, ids, layout, hit testing, widgets, theme, icons, animations |
| `src/gui/draw_list.h` | backend-agnostic primitives: rounded rect (gradient, border, radius), shadow, circle, capsule line, text run |
| `src/gui/ui_font.h` | SDF font lookup and text measurement (headless) |
| `src/gui/ui_font_metrics.h` | generated glyph metrics (do not edit; see "Font") |
| `src/gui/terrain_toolbar.h` | the terrain sandbox toolbar, the first user |
| `src/render/ui_vertex.h` | `UiVertex`, the smooth path's 48-byte vertex |
| `src/shaders/ui.hlsl` | the smooth path's shader (D3D12 at runtime via fxc, Vulkan via `spirv.bat`) |
| `assets/ui/ui_font_sdf.png`, `assets/ui/LICENSE_DejaVu.txt` | the font atlas and its licence |
| `tools/bake_ui_font.py` | bakes the atlas and the metrics header |

The core has **no platform headers** (`tests/gui_test.cpp` fails to compile if `windows.h` sneaks in). Its only
dependencies are `ui_draw.h` (the classic pixel font), `render/vertex.h`, `render/ui_vertex.h` and `vecmath.h`.

## Two output modes

- **Smooth** (default): every primitive is one `UiVertex` quad. `Renderer::DrawUi` draws them in one alpha-blended,
  depth-less pipeline whose pixel shader turns each quad into an anti-aliased shape by signed distance (rounded box with
  border, soft shadow, circle, capsule) or samples the SDF font atlas. Panels are semi-transparent with soft shadows.
- **Classic** (fallback): the same calls become flat `Vertex` quads and the 5x7 pixel font, drawn with `DrawTransient`
  like before: no alpha, no shadows, square corners. Used when the renderer reports `UiSupported() == false` (ui.hlsl
  missing or failed to compile, SPIR-V missing), when the atlas cannot be loaded, or when **`TBD_GUI_CLASSIC=1`** forces it
  (handy for before / after comparisons). The engine log says which mode is on (`Game GUI: smooth ...` / `classic ...`).

Layout and hit areas are identical in both modes (only text measurement follows the font that is drawn), and so are the
widget rules; the tests run the toolbar in both.

## API

```cpp
#include "gui/gui.h"
using namespace tbd::gui;

Context g;        // hover / active ids, panel sizes, animation slots, the reused vertex vectors
InputQueue q;     // fed from window messages or touch events between frames
g.SetClassic(!renderer.UiSupported());

// once per frame
g.BeginFrame(q.Take(viewW, viewH, dpiScale, dt));
g.BeginPanel("Terrain", Anchor::CenterLeft, 8, 0, 296);       // title, anchor, offset, width (0 = fit), height (0 = fit)
static const char* const modes[] = {"Raise", "Dig", "Flatten"};
static const Icon icons[] = {Icon::Raise, Icon::Dig, Icon::Flatten};
g.Choice("Mode", modes, 3, &mode, icons);                     // segmented choice with a sliding pill, true when changed
g.SliderFloat("Brush size", &radius, 4, 300, "%.0f m", SliderScale::Log);
g.Toggle(on ? "Editing on##edit" : "Editing off##edit", &on); // "##..." is hashed, not drawn: a stable id
g.BeginRow(2);                                                // the next 2 widgets share a row
if (g.Button("Save", Icon::Save)) Save();                     // true on release inside after a press inside
if (g.Button("Load", Icon::Load)) Load();
g.EndRow();
g.EndPanel();
g.EndFrame();                                                 // WantsMouse() is valid from here
if (g.Classic()) r.DrawTransient(g.Vertices().data(), ..., Topology::Triangles, OrthoPixels(w, h), Mat4::Identity(), false);
else r.DrawUi(g.UiVertices().data(), (uint32_t)g.UiVertices().size(), OrthoPixels(w, h));
```

Also: `Label(text, TextStyle)`, `Text(style, printf...)` (stack buffer, no allocation), `Separator()`, `Spacing(px)`,
`NextWidth(px)`, `PushId(str|int)` / `PopId()`, `SetInsets(l, t, r, b)` (iOS safe area, the engine's stats text),
`Reset()` (hide: forget the shown panels at once), `MeasureText(text, style)`, `LastItem()`, `ShownPanels()`,
`Capturing()`, `WantsMouse()`, `WantsMouseAt(x, y)`, `BaseTheme()` / `Style()`, `Draw()` (the `DrawList`),
`DrawIcon(drawList, Icon, cx, cy, size, stroke, colour)`. `BeginPanel` takes an optional padding (the status chip uses 8).

### Renderer API addition (`render/renderer.h`)

```cpp
virtual bool UiSupported() const;                     // false: no UI pipeline, use DrawTransient (classic)
virtual void SetUiAtlas(TextureId atlas);             // the SDF atlas text samples (a CreateTexture texture), once after Init
virtual void DrawUi(const UiVertex* v, uint32_t count, const Mat4& ortho); // overlay phase only, after ResolveScene
```

Both backends implement it with their own small pipeline (D3D12: a root signature with 16 root constants, one SRV table
and a static trilinear clamp sampler, the atlas copied into a spare slot of the shader-visible heap; Vulkan: a 64-byte push
constant, one descriptor set with the atlas and the existing linear clamp sampler). Blending is SrcAlpha / InvSrcAlpha,
no depth test, into the display-space back buffer. The vertices go through the same per-frame transient upload buffer as
`DrawTransient`; afterwards the mesh root signature / descriptor sets are bound again so later overlays are unaffected.
`DrawTransient` itself is unchanged.

`UiVertex` (48 bytes): position; local coordinates (pixel offset from the shape's centre, or atlas uv for text); `a, b`
(half size, or the capsule's end minus start); corner radius / half thickness; border width; mode (rect, shadow, text,
line); softness (shadow blur, text sharpness); fill colour (RGBA8, per vertex, so vertical gradients interpolate); line
colour (border / text outline). The shaders use fine screen derivatives (`ddx_fine` / `ddy_fine`) for the one-pixel
anti-aliasing so both APIs produce the same edges.

### Layout

Widgets stack vertically inside a panel (padding around, a gap between) and fill its width, or a column of a
`BeginRow(n)` row (equal columns), or a fixed `NextWidth`. A panel is anchored to one of nine screen positions
(`TopLeft` ... `BottomRight`); the offsets are margins from the anchored edges (positive = towards the middle), and on a
centred axis they shift right / down. Every frame the position is recomputed from the viewport size and DPI scale, then
clamped into the viewport minus the insets (the top-left edge wins when a panel cannot fit). Sizes and offsets are given
unscaled and multiplied by the DPI scale.

Auto-sized panels (width or height 0) and anchors that depend on the size (right, bottom, centre) use the size measured on
the **previous** frame, so a panel that changes size settles one frame later. Fixed sizes are exact on the first frame.

### Theme and look

`Theme` holds the colours (RGBA8, straight alpha) and sizes at DPI scale 1.0, on an 8 px grid: text 15 px (values and the
status line 13 px, titles 15 px bold and brighter), padding 12, gap 8, **hit size 44** (everything pressable is at least
that tall: Apple's 44 pt), title strip 36, panel radius 8, widget radius 6, shadow blur 14 offset 5. Panels are charcoal at
about 88% opacity with a 1 px lighter border, a soft shadow, and a thin rule under the title with a short accent stroke.
Buttons have a subtle top-lighter gradient and turn accent when pressed; the segmented control is one dark well with a
sliding accent pill; sliders have a thin rounded track, an accent fill and a round thumb with a shadow; the toggle is a pill
switch with a sliding knob. `Context` scales the theme by `Input::dpiScale` every frame (`Theme::Scaled`); change
`BaseTheme()` to make everything bigger on a phone. Text that does not fit its widget shrinks in half-pixel steps down to
`minFontSize` (classic: one-pixel font cells), then is cut, so it never spills out.

Transitions (`Theme::animRate`, about 0.15 s) blend colours and slide the pill and knobs; they never move hit areas, and a
widget seen for the first time starts at its final state (no fade-in; captures are stable). `Input::dt` drives them.

Icons (`Icon::Raise`, `Dig`, `Flatten`, `Save`, `Load`) are drawn from capsule strokes of one weight (`iconStroke`), so they
stay crisp at any scale and need no texture.

## Font

**DejaVu Sans** and **DejaVu Sans Bold**, from the copy that ships with matplotlib on this machine
(`site-packages/matplotlib/mpl-data/fonts/ttf`). Licence: the Bitstream Vera / DejaVu licence (free to use, modify, embed
and redistribute, with the notice kept), copied to `assets/ui/LICENSE_DejaVu.txt`. No Windows system font is used and
nothing was downloaded.

Re-bake (Pillow and numpy, about 15 s):

    python tools/bake_ui_font.py [--font-dir <folder with DejaVuSans.ttf>]

It renders ASCII 32..126 of both weights at 4x of a 48 px em, measures the exact Euclidean distance to the outline at
atlas resolution (6 px spread each side), shelf-packs everything into one 512 x 1024 atlas
(`assets/ui/ui_font_sdf.png`, the distance in all four channels) and writes `src/gui/ui_font_metrics.h` (advance,
quad relative to the pen and baseline, atlas rectangle, ascent, descent, cap height). Kerning pairs are written when
Pillow reports them; Pillow's basic layout (no libraqm here) reports none, so the table is currently empty and measurement
uses advances only. The runtime needs neither Pillow nor numpy: the engine loads the PNG with `DecodeImage` from
`<exe>\..\assets\ui\` and uploads it with `CreateTexture` (mipmapped, sampled trilinear) after every renderer start.

## Input contract

`Input` (one per frame, pixels, origin top-left, y down): `mouseX/Y`, `down`, `pressed` (went down this frame **and the
host gave the press to the GUI**), `released`, `pressX/Y` (where it went down; widgets activate from this), `wheel`
(notches), `viewW/H`, `dpiScale`, `hover` (false for touch), `dt` (seconds, for transitions).

`InputQueue` builds it from events: `Move`, `Down(x, y, claim)`, `Up`, `Wheel`, `Leave`, `SetHover`, and
`Take(w, h, dpi, dt)` once per frame. A press and release that both arrive inside one frame both survive, so a fast click
is never lost. `Down(..., claim = false)` tells the GUI the button is down for someone else (an engine drag): nothing
activates and no widget lights up under that drag.

Rules: a widget activates on a press inside it; a button / toggle / segment clicks when released inside it; a slider
follows the pointer while held, also outside the panel and the window, until release; the wheel over a slider steps 2%
of its range. A press on a panel's empty space is held by the GUI until release (so it cannot become a camera drag) and
does nothing. Nothing essential depends on hover; hover only brightens.

`WantsMouse()` (after `EndFrame`): the pointer is over a panel, or the GUI holds a press. `WantsMouseAt(x, y)` (for window
messages arriving between frames): over a panel of the last **finished** frame, the one on screen, or holding a press.

**Hit-testing choice:** widgets test the pointer against the rectangles they get during the current frame; the host's
decision whether a message belongs to the GUI uses the previous frame's panels, i.e. exactly what the user sees when they
click. The standard immediate-mode approach; the only gap is a panel that moves between the click and the next frame.

## Engine integration (`src/main.cpp`, marked "in-game GUI")

- Globals after `g_terrainEdit`: `g_gui`, `g_guiInput`, `g_guiVisible`, `g_guiOwnsButton`, `g_terrainSandbox`.
  `TerrainEdit` gained `mode` (0 raise, 1 dig, 2 flatten); `TerrainBrushModeNow()` applies it, with **Ctrl still forcing
  flatten and Shift still forcing dig** whatever the toolbar says. The brush ring colour follows the same rule.
- `UploadUiFont()` runs at the end of every `StartRenderer` (startup, and F1 / F2 which make a new renderer): it picks
  smooth or classic (see above) and uploads the atlas.
- `GuiMouse()` runs first in `ViewportProc` (before the Character Creator hair brush and the engine's own handling):
  - `WM_LBUTTONDOWN` over a shown panel (no engine drag in progress, no Alt): queued as a claimed press, `SetCapture`,
    consumed. Otherwise queued unclaimed and passed on.
  - `WM_MOUSEMOVE`: always queued; consumed only while the GUI holds the press (a slider drag).
  - `WM_LBUTTONUP`: queued; consumed (and `ReleaseCapture`) when the GUI took the press.
  - `WM_RBUTTONDOWN` / `WM_MBUTTONDOWN` over a panel: consumed, so fly / pan do not start on the toolbar.
  - `WM_MOUSEWHEEL` over a panel (converted from screen coordinates): queued, consumed (no camera zoom).
- `DrawGameGui()` runs once per frame after `ResolveScene()` and every other overlay (stats text, profiler): it takes the
  queued input (with the frame's dt), runs the toolbar, performs Save / Load through `SaveTerrainEdits()` /
  `LoadTerrainEdits()`, and draws with `r.DrawUi(..., OrthoPixels(vpW, vpH))` (smooth) or `r.DrawTransient(...)` (classic)
  at `DpiScale()`. `DrawStatsText()` returns where its top-left block ends; that is the GUI's top inset.
- Shown when terrain is on and the project is a terrain sandbox (or terrain edit is on), never during Play.
- **U** hides / shows it (F1-F7, F9, F10 are menu accelerators, F8 / F11 save / load terrain, F12 breaks into an attached
  debugger). Hidden means `Reset()`: no panels, nothing consumed.
- Off in `TBD_SHOT` / `TBD_CAPTURE` runs unless `TBD_GUI=1` (or `TBD_GUI=0` forces it off anywhere).
- `TBD_GUI_CLASSIC=1`: the classic fallback even when the UI pipeline works.
- `TBD_GUI_POINTER="x y [down]"`: a scripted pointer for captures; without `down` it hovers; with `down` the button goes
  down on the 30th GUI frame (after auto-sized panels have settled) and stays down (pressed / dragging look). It also sets
  the cursor the status chip reports.

## Terrain toolbar

Left edge, vertically centred, below the stats text: the "Terrain" title, Mode (Raise / Dig / Flatten, each with an icon),
Brush size (4 to 300 m, log scale, bound to `g_terrainEdit.radius`; `[` `]` still work and the slider follows), Strength
(1 to 20 m/s, bound to `metresPerSecond`), the Editing on / off switch (bound to `g_terrainEdit.on`, same as T), Save and
Load (same as F8 / F11, with the usual toasts). Bottom right, above the toast row: a status chip with the cursor's east /
north (metres; north is -z) and ground height, and the edited tile count (`field.TileCount()`).

## Touch / iOS notes

- One pointer: touch began = `Down`, moved = `Move`, ended = `Up` then `Leave` (a lifted finger has no position), and
  `SetHover(false)` so only the pressed widget highlights.
- `dpiScale` is the screen scale (2 or 3 on iPhone); everything pressable is at least 44 points tall.
- `SetInsets` takes the safe area (notch, home indicator).
- The smooth path needs one alpha-blended pipeline with the `UiVertex` layout and `ui.hlsl`'s pixel shader (ported to
  Metal: the SDF formulas are a few lines) plus the atlas texture; the classic path needs only coloured triangles.

## Known limits

- No clipping, scrolling, text input, keyboard / gamepad focus, or nested panels; overlapping panels are not ordered
  for input (the first that contains the pointer counts as "over the GUI", widgets test only their own rectangle).
- Auto sizes and size-dependent anchors lag one frame after a size change; a panel that does not fit the viewport is
  clamped to the top-left and can run off the bottom / right edge.
- The font is ASCII 32..126 only (one atlas, two weights); no kerning (see "Font"); no rich text.
- Blending happens in the display's gamma space (the back buffer is UNORM, like every other overlay), the usual UI look.
- On D3D12 a shader reload (F5) rebuilds the UI pipeline too; on Vulkan the UI pipeline is built once per renderer start.
- The classic fallback ignores alpha, shadows and corner radii by design (the old 2D path has no blending).
- Tests: `tests\gui_test.bat` (headless, `cl` + `/I src`).
