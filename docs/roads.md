# City roads (`src/city`, `src/gui/road_toolbar.h`)

A Cities: Skylines II style road builder inside the engine. Platform free (no Win32): `main.cpp` only feeds it the
cursor's ground point, clicks and keys, uploads its meshes and draws its preview.

| File | What |
|---|---|
| `src/city/road_network.h` | the 51 road profiles (ported 1:1 from `assets/roads/roadlib.js`, the CS2 road list), the node / Bezier segment graph, splitting, terrain following, and the mesh builder |
| `src/city/road_tool.h` | the tool: modes, snapping, elevation, parallel mode, prefab intersections / roundabouts, validation, committing, undo |
| `src/gui/road_toolbar.h` | the GUI: tool options panel and asset picker (CS2 layout) |
| `tests/road_test.cpp` | headless checks (`tests\road_test.bat`) |

## Using it

The **Roads** button at the bottom of the screen opens the tool (Esc or the X closes it). Bottom left:

- **Tool Mode**: Straight (2 clicks), Simple Curve (3: start, bend, end), Complex Curve (4: start, two handles, end),
  Continuous (curves that stay tangent to the last one), Grid (3: one side, then drag out the other), Replace (click a
  road to swap it for the selected one; the same road flips its direction). Drawing chains from the end of the last
  road; **right click** cancels.
- **Elevation**: arrows or **Page Up / Page Down** in steps of 10 / 5 / 2.5 / 1.25 m (the bars button). More than 3 m
  above the ground is a bridge (deck, parapets, pillars every ~36 m); more than 4 m under it a tunnel (hidden, with
  portals where it dives in, and shown as dashed lines while the tool is open).
- **Parallel Mode**: also places a copy offset to the right (offset adjustable).
- **Snapping**: all on / off, cell length (8 m), existing geometry (nodes and road surfaces), straight directions
  (15 degree steps from the connected road), net side (whole 8 m cells from the nearest road), guidelines (extensions
  and perpendiculars of nearby roads, and their crossings), zone grid, contour lines (drawn around the cursor).
- **Tools**: Bulldoze (click a road) and Undo (also Ctrl+Z / Backspace; Ctrl+Y redo).

Bottom centre: category tabs (small / medium / large roads, highways, quays, paths, intersections, roundabouts) and
the tiles (each road tile is its cross-section). Prefab intersections and roundabouts place with one click and rotate
with **,** and **.**.

While drawing, the ghost is blue when it can be built and red with the reason when not (too short, too steep (>32%),
curve too sharp for the road's width, angle under ~16 degrees to a road at the same node). Chips show length, slope,
the angle to the connected road and the elevation.

## How it works

- A road crossing another at about the same height (within 4 m) splits both and makes a junction; otherwise it passes
  over / under. Ending on a road splits it into a T. Roundabouts remove what was inside the ring.
- Ground nodes (elevation 0) read the terrain live and their roads follow it, smoothed over ~30 m; placing a road levels
  a plateau under it (`Terrain::Brush` Flatten with a new `hardRadius`) and re-levels the roads and junction pads it
  touches. Bridges, tunnels and ramps run straight between their end heights.
- Junctions trim each arm far enough to clear its neighbours plus a corner radius, then fill the middle and round the
  sidewalk corners with curbs; arms ending at 3+ way junctions get crosswalks.
- Meshes: one per material (the `assets/roads/tex` textures), rebuilt before the frame whenever the network changes.

Captures: `TBD_ROADS=1` opens the tool at startup, `TBD_ROADS=demo` builds a sample town on the driest patch near the
origin, `TBD_ROADS_START="x z [mode]"` adds a first click there (with `TBD_GUI=1` and `TBD_GUI_POINTER` to show the ghost).

Not yet: cars / lane routing, saving the network with the project, upgrade of individual lanes, zoning.
