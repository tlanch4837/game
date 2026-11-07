# Controls & QOL improvement plan

Based on a pass over `src/main.cpp` (OnKeyDown ~4480-4600, WndProc mouse ~4680-4850, accelerators ~4928), the tool `.inl` files and `gui/*_toolbar.h` / `city_hud.h`.

## What's wrong today

1. **Keys are hard-coded and scattered.** Every tool has its own `switch` in `main.cpp`, plus more in `city_industry.inl` and `undo_scene.inl`. Nothing can be rebound and there is no one place that lists all the keys.
2. **Keys clash.**
   - F2 is bound to the Vulkan accelerator, and also to road rename (`main.cpp:4486`).
   - F1 switches the renderer to D3D12. Players expect F1 to open help.
   - Backspace means *undo* in the road tool but *delete selection* in the industry tool.
3. **Undo is split up.** Ctrl+Z/Y works for roads (`main.cpp:4553`) and through the scene undo (`undo_scene.inl`). Zoning, utilities, transport and industry have no undo.
4. **Debug keys are live in the game.** R/B/K/H (ragdoll, blast, knockout, punch), N/P/I (NPC authoring) and F8/F11 (terrain save/load) all work during normal play.
5. **The city camera is mouse-only.** WASD only works in fly mode. There are no arrow keys, no edge scroll and no Q/E rotate. Mouse zoom also seems to go to the screen centre rather than the cursor (not confirmed).
6. **Game speed is click-only.** The HUD icon cycles through the speeds (`city_hud.h:124`). There are no 1/2/3 keys.
7. **No tool hotkeys.** Every tool in the bottom toolbar has to be picked with the mouse.
8. **Help text is uneven.** Transport and utilities show hint lines. Roads, zones and terrain don't, and nothing shows the key for an action.
9. **No settings file.** Things like brush radius, camera sensitivity, invert axes and UI scale are not saved.
10. **No touch scheme**, even though iPhone 15 is the target. All input goes through mouse and keyboard.

## Proposed changes (in order of value)

### P0: Foundation
- **Input action layer.** Add `src/input/actions.h`: an `enum Action { CamForward, ToolRoads, Undo, Rotate, RaiseHeight, Cancel, Confirm, SpeedPause, ... }`, a table of default bindings, and `bool Pressed(Action)` / `Held(Action)`. Then move every `wparam == 'X'` check onto it. This fixes clashes in one place and makes rebinding and touch possible later.
- **`settings.ini`** saved next to the exe: bindings, camera sensitivity, invert, edge scroll on/off, UI scale, autosave interval.
- **Dev-only gate.** Put the debug and authoring keys behind a `devMode` toggle (for example Ctrl+Shift+D, or in builds compiled with the flag). Move the graphics-API and profiler F-keys into that set too.

### P1: Consistency across all tools
- The same verbs in every tool:

| Key | Action |
|---|---|
| Esc | cancel / leave tool |
| Enter | finish |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Delete | remove selection |
| , and . | rotate |
| PgUp / PgDn | height |
| B | bulldoze toggle |
| Right-click while placing | cancel the current step |

- **Global undo stack** shared by all tools (each tool pushes commands), not just roads and the scene.
- **Context hint bar** in every tool, built from the binding table (for example `[,][.] rotate  [PgUp] raise  [Del] remove`), so it stays correct after a rebind.
- **F1 = controls overlay** that lists the bindings for the current tool.

### P2: Camera & time
- City camera: WASD and arrow keys pan, Q/E rotate, R/F tilt, Shift for fast. Optional edge scroll. Zoom towards the cursor. Smooth camera movement.
- Number keys for speed: 1/2/3, with Space for pause (Space already pauses).
- Home resets the camera (already works). Ctrl+1..9 saves a camera bookmark and 1..9 jumps to it. That needs remapping from speed, or use Alt+1..9 for speed and Shift+1..9 for bookmarks.

### P3: Building QOL
- Tool hotkeys (Z zones, R roads, U utilities, T transport, I industry), but only once the debug keys are out of the way.
- Snapping toggles: grid, angle (15°/45°), and parallel to a road. Hold Alt for free placement.
- Live length, cost and slope readout next to the cursor while drawing roads and lines.
- Repeat last tool with a middle-click tap, or pipette (pick the hovered object's tool and type).
- Drag-paint and drag-bulldoze areas (rectangle select).
- Show build cost before placing, and grey out what you can't afford.

### P4: Game QOL
- Autosave every N minutes with rotating slots, plus quick save / load (F5/F9 once those are freed from shader reload).
- Toast history or a notification log, with click-to-jump to where the event happened.
- Info views (power, water, traffic) bound to keys, with the legend on screen.
- Clicking a building, vehicle or citizen opens an info panel, with a follow-camera button.

### P5: Touch (iPhone)
- Map each Action to a gesture: one-finger drag pans, two-finger pinch zooms, two-finger twist rotates, two-finger vertical drag tilts. Tap selects or places, long-press opens a context menu, and an on-screen ✓/✕ confirms or cancels.
- Bigger hit targets in the toolbars, behind a `uiScale` setting.

## Suggested order
P0 action layer + settings → P1 consistent verbs, hint bar and F1 overlay → P2 camera and speed keys → P3/P4 picked by playtest pain → P5 alongside the iOS/Metal port.
