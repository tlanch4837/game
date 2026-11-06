# Terrain plan: 20 x 20 km, fully terraformable

Status: plan only. Nothing here is implemented. Numbers marked (est.) are estimates to be measured, not results.

## 1. Where we are

`src/terrain.h` is an infinite procedural heightfield: `Terrain::HeightAt(x, z)` evaluates value noise (5 octaves plus ridged mountains) on every call. Meshes are 64 m CPU-built chunks (8/16/32 cells by distance, skirts for cracks) in a radius of 8 chunks, plus three coarse "far" rings rebuilt on the CPU as the camera moves. Draws go through `Renderer::CreateMesh` with `Vertex` data.

What that means for the goal:
- **No edits.** Height is a pure function, so there is nowhere to store a change. Terraforming needs a stored layer.
- **Every query re-evaluates noise.** Raycast steps and per-vertex sampling both call `HeightAt`. Edits plus roads, zoning and simulation will call it far more often.
- **LOD is by distance from the camera only.** The README says the game is a top-down iOS city sim. A zoomed-out camera sees a huge area at once, so LOD must follow view height/zoom, not just horizontal distance.
- **Few call sites.** Outside `terrain.h`, only `main.cpp` uses terrain: `Raycast` (line 2089), `HeightAt` (2184), and `Update`/`UpdateFar`/`Draw`/`DrawShadowCasters` (2918-3460). Swapping the height source is a contained change.
- **Renderer has compute but limited buffer updates.** It has `CreateGpuBuffer(ShaderData)`, `Dispatch` (4 read, 3 write buffers, 128-byte constants), instancing and indirect draws, but no writable float texture and no call to update part of an existing GPU buffer from the CPU. iOS (Metal or MoltenVK) is the eventual target, so assume no hardware tessellation and no mesh shaders. `ground_bake.inl` already makes the same "no tessellation, both backends run the same code" choice.
- **Floating origin is unfinished** (`docs/floating_origin.md`: arithmetic exists, not wired in). 20 km in `float` loses precision (about 1-2 mm at 10 km from the origin, coarser further out), so this must be solved or avoided for terrain (see 7).

## 2. Design goals and budgets

| Goal | Target |
|---|---|
| World size | 20,000 x 20,000 m, all editable |
| Edit resolution | 2 m cells (roads and building lots need this; CS2 is about 3.5 m) |
| Brush latency | Visible change in the same frame, 60 fps on a mid iPhone |
| Memory (resident) | Under about 150 MB for terrain on iOS (est.); never proportional to the 400 km² world |
| Save size | Proportional to what the player edited, not the world |
| Determinism | Same save gives the same heights on every machine |

A dense 2 m grid over 20 km is 10,000 x 10,000 = 100 M samples, which is 200 MB at 16 bits. That is too much to keep resident and wasteful to save, because most of the world is never touched. The design avoids ever holding it densely.

## 3. Core idea: height = base + sparse edits

```
Height(x, z) = Base(x, z) + Edit(x, z)
```

**Base** is the untouched world. It is read-only and replaceable behind one interface:
- `ProceduralBase`: the current noise, kept as is.
- `RasterBase`: an imported or authored 16-bit heightmap (for example 2500 x 2500 at 8 m, about 12 MB), bicubic-sampled, with the procedural noise added only as small-scale detail.
- Base tiles are generated on worker threads and cached (LRU). Nobody evaluates noise per query any more.

**Edit** is a sparse set of delta tiles:
- Tile = 256 m square, 128 x 128 samples at 2 m, `int16` deltas in 2 cm units (range about +-650 m, plenty for terraforming).
- A tile exists only if the player touched it. Untouched space returns a delta of 0 with no memory.
- A tile that is edited and then smoothed back to all zeros is dropped.
- Per tile: 32 KB raw. A city of 5 x 5 km is about 400 tiles, about 13 MB. Worst case, every tile edited: 78 x 78 = 6084 tiles, about 200 MB. That is saved on disk and paged, not resident.
- Each tile also stores a small **mip pyramid** (64, 32, 16, 8 ... samples) with min, max and average, built when the tile changes. LOD and culling read these, so far-away LOD never touches full-resolution data.

**Why this fixes the worries from earlier:** polygon count is not tied to how creative the terrain is. Detail lives in data (delta tiles) and mesh density is chosen per view from the pyramid.

## 4. Edit pipeline

1. **Brush stroke** (raise, lower, smooth, flatten-to-height, slope, noise): the tool computes the affected 2 m cells and the tiles they fall in.
2. **Apply** to the CPU delta tiles. The CPU copy is the single source of truth, because roads, raycasts, the nav mesh, vegetation and the sim all read it.
3. **Constraints** as the stroke is applied: clamp vertical range and slope, and skip or dim cells under protected things (locked roads, building footprints, water-body floors) if the design wants it.
4. **Mark dirty**: tile rectangle plus a 1-sample border so neighbours' normals and edges stay correct.
5. **Rebuild only dirty tiles**: re-derive that tile's pyramid, normals and render data on a worker (`core/jobs.h`), then publish to the renderer at a frame boundary.
6. **Notify dependents** with a "terrain changed in this rectangle" event: roads and rails re-fit, vegetation re-scatters, the nav mesh re-bakes that region, water re-solves, building plots re-check. This event is the contract between terrain and the city sim.
7. **Undo/redo**: store the before-delta of each touched tile per stroke (compressed). `undo.h` already exists in the engine to hook into.

Roads carve through a separate **road-influence layer** (a distance-field mask near each spline) blended at render time, rather than writing flat sections into the player's heights. Deleting or moving a road then doesn't destroy the player's sculpting and doesn't need a full tile rebuild.

## 5. Rendering

Two stages, so the system works early and gets faster later.

**Stage A: CPU-built tiles (fits today's renderer, build first)**
- Keep chunk meshes via `CreateMeshNoWait`, but build them from the height tiles instead of calling noise.
- Choose LOD per chunk from camera **height and distance** (and the tile's min/max, so flat tiles use very few triangles; cliffs and edited areas get more). Use the pyramid levels for mid and far chunks.
- Keep skirts first, then switch to stitched edges if skirts show.
- Far view: rings built from the coarse base plus the tile pyramids, so changes far from the camera still show when zoomed out.
- Edit cost is one chunk rebuild on a worker per touched chunk. This is fast enough to validate everything else.

**Stage B: GPU geometry clipmaps (do after A works, needs renderer changes)**
This is the rendering method with published measurements (see section 10): Asirvatham & Hoppe, GPU Gems 2 ch. 2.
- Terrain is drawn as **nested square grids centred on the viewer**, each level twice the size and half the detail of the one inside it, all built from a few shared grid meshes drawn **instanced**. Per-frame triangle count stays roughly constant however large or rough the world is.
- Each level's heights live in a GPU **height texture or buffer** (a window onto the world, refilled incrementally as the viewer moves); the vertex shader fetches the height, so no per-chunk meshes are created or destroyed.
- **Edits** are a small upload of the dirty tile region into the level windows that cover it. Our sparse edit tiles feed this directly (the clipmap papers do not cover editing; this part is ours and is not yet proven).
- **Zoomed-out top-down camera:** clipmap levels follow camera height as well as position, so a high camera simply uses a coarser set of levels. Needs a prototype to confirm the look and cost at city zoom.
- Needs: a `Renderer::UpdateGpuBuffer(buffer, offset, data, bytes)` call (absent today; or a writable texture), a terrain vertex-shader variant on **both** backends, and later the iOS backend. Compute can build normals into a `ShaderData` buffer.
- The vertex-texture lookup was measured to cost about 40% of frame rate in the original paper's test (130 vs 185 fps), so measure it on iOS early.

Both stages use the same `HeightField` interface, so Stage B replaces only the presentation.

**Physics and AI:** terrain collision needs a heightfield shape per live tile (Jolt supports `HeightFieldShape`), updated for dirty tiles only. This is a risk item (see 8).

## 6. Storage and streaming

- **In memory:** a hash map of tile coordinate to tile, with a resident budget. Active = camera view plus a margin, plus any tile with recent edits still being processed.
- **On disk (save game):** one file or chunked archive of edited tiles only, delta-coded and run-length or zstd compressed. Most edited tiles are smooth, so this should be far smaller than 32 KB per tile (est.).
- **Page out** least recently used clean tiles; reload on demand on a worker; never block the main thread.
- **Base cache** is separate and disposable; it can always be regenerated. `tests/terrain_bench.cpp` measured 36.6 MB for 576 float32 base tiles (a 6 km area), which is too much for iOS: store base tiles as `int16` (half the size), evict by LRU, or sample the coarse raster base directly instead of caching dense tiles. Generating a tile took about 1.6 ms.

## 7. Precision at 20 km

- Do **not** put absolute world coordinates in `float` vertices. Tile meshes and the atlas use **tile-local** coordinates (0..256 m); the tile's placement is an `int` tile coordinate plus a per-frame camera-relative offset.
- Terrain code stores and returns heights relative to tile origin and does all absolute math in `double` or integer tile coordinates (`WorldPosition` in `floating_origin.h` already models this).
- This keeps terrain correct without waiting for the global floating-origin rebase to be finished, but physics and entities still need that work before characters can walk 10 km from the origin. That is a separate, already-documented task.

## 8. Risks and open questions

1. **Physics heightfield updates** at brush speed: rebuilding a Jolt shape per stroke may be too slow. Mitigation: update on stroke end, or use a coarser collision grid (4 m) than the visual one.
2. **Normals across tile borders** need the neighbour's border sample; get this wrong and you see lighting seams. Keep a 1-sample overlap.
3. **LOD popping and cracks** between different-resolution neighbours. Skirts hide this cheaply at first.
4. **Renderer API gaps** for Stage B (partial GPU buffer update, terrain shader). Needs changes in `renderer_d3d12.cpp` and `renderer_vulkan.cpp`, and in the iOS renderer when it exists.
5. **iOS memory/thermal limits** are unknown until we run on a device. The 150 MB budget is a guess.
6. **Base choice:** procedural, imported heightmap, or both. This affects art direction and save compatibility; decide before implementing `RasterBase`.
7. **What can the player edit under buildings and roads?** Rules for protected areas change the brush code and the notification flow.

## 9. Build order

1. **`HeightField` module**: tile storage, `Base + Edit` sampling (bilinear), mip pyramids, with unit tests (sampling, tile creation and drop, border continuity). No rendering changes.
2. **Wire it in**: `Terrain::HeightAt`/`Raycast` read the `HeightField`; behavior is identical with zero edits. Remove per-sample noise from the mesh builders by using cached base tiles.
3. **Brush tools + dirty/rebuild path** (Stage A), with a "terrain changed" event and one real consumer (vegetation or the raycast cursor).
4. **View-height-aware LOD** and far rings from pyramids; check zoom-out from city level to the whole map.
5. **Save/load and undo** of edited tiles.
6. **Collision tiles** for physics and the nav mesh.
7. **Stage B** GPU atlas and instanced grids, after renderer API additions.
8. **Profile** on an iOS-class budget and adjust tile size (128 vs 256 m), edit resolution (2 vs 3 m), and the memory budget.

Steps 1-3 give a playable terraformable prototype; 4-6 make it a city-sim foundation; 7-8 are performance work.

## 10. Evidence

What is measured, what is published, and what is still unproven. Nothing here compares against Cities: Skylines 2 (its source is not available to us).

**Measured here** (`tests/terrain_bench.cpp`, one thread, desktop CPU, CPU data only; no meshes, GPU, physics or iOS):
- Sparse base+edit tiles: 12.5 MB of edits for a 5 x 5 km city session (3000 strokes), against 190.7 MB for a dense 2 m grid; 4.4 MB for a light session. Save size 5.4 MB with simple zero-run compression.
- Brush stroke: about 0.3 to 0.9 ms average, 7 to 13 ms worst, including rebuilding the dirty tiles' heights, normals and mip min/max.
- Height query: 54 ns, against 98 ns for the engine's current per-call noise (1.8x).
- Weakness: if the player edits the whole map, resident edit memory (194 MB) is no better than the dense grid (190.7 MB). Tiles must page to disk; base cache must be reduced (section 6).

**Published, from primary sources:**
- Geometry clipmaps, GPU version (Asirvatham & Hoppe, GPU Gems 2 ch. 2): a 216,000 x 93,600 US height map (30 m spacing, about 20 billion samples) compressed to 355 MB; 130 fps (60 million triangles/s) with view-frustum culling at 1024x768 on a 2.4 GHz Pentium 4 and GeForce 6800 GT; about 87 fps while decompressing during movement. Worst-case level update: normal maps 11 ms to 0.6 ms and upsampling 3 ms to 1.0 ms, CPU version to GPU version. No frame-rate comparison with the CPU version, and no editing.
- Tile-based deformable terrain (Crause, Flower, Marais, SAICSIT 2011): deformations kept in per-tile textures with a streaming cache; coarse-method render time 15.4 ms on a 9600GT and about 1 ms on a GTX 580 at 1600x900; 30 fps minimum held on the weakest GPU for the parallax method. The deformation timings are only in a graph. A paper by its own authors, not an independent comparison.
- CDLOD (Strugar 2009) and Far Cry 5 (GDC 2018) describe GPU terrain LOD pipelines; no figures were retrieved from either.

**Unproven, to be measured before relying on them:**
1. Brush-to-screen latency in this engine's renderer.
2. Clipmap look and cost at top-down city zoom.
3. Memory and frame time on an iPhone-class device.
4. Collision and nav mesh update cost at brush speed.
5. Any claim of being faster than CS2 or any other shipped game.

## 11. Water (cheapest version)

Goal: water that reads well in a top-down city sim, costs almost nothing on iOS, and keeps working when the player terraforms.

**What the engine has now** (`src/water.h`, `src/shaders/water.hlsl`): Gerstner wave cascades in compute, a 512 x 512 ripple wave-equation simulation, a dense camera-near mesh split on a worker, Beer-Lambert absorption and flow maps. It only takes over scene entities named `map_water*` / `*_water_surface`, so it is not driven by terrain edits. I have not measured its cost. For a city builder it is more than we need, and the ripple sim and dense mesh are the parts to avoid on a phone.

**Cheapest design: water is a level, not a simulation.**
1. **Each water body has one flat height** (sea, lake, pond). Rivers are the exception: spline ribbons, see step 4.
2. **Depth comes from the terrain you already store:** `depth = waterLevel - terrainHeight(x, z)`. No separate water geometry per body is needed to know where water is.
3. **Draw water as one cheap pass:**
   - A flat plane or a few large tiles per body, drawn after opaque terrain, alpha blended.
   - Colour from depth (shallow to deep tint), a Fresnel term for sky reflection, a sun specular highlight, and 1 to 2 scrolling normal-map layers. A depth-fade edge instead of a foam simulation.
   - No Gerstner cascades, no ripple compute, no dense mesh, no refraction pass, no planar reflection (use the sky colour or a small cubemap).
   - In Stage A (CPU chunks) the depth can be baked into the terrain vertex colour/alpha so the plane needs no height lookup. In Stage B (clipmaps) the shader reads the same height texture.
4. **Rivers:** a ribbon mesh along a spline with a flow direction scrolled through the same normal map. No fluid solve. Width and bank come from the spline; depth comes from the terrain as above.
5. **Terraforming interaction (the part that needs a rule):**
   - Digging below a body's level next to that body should flood. On each terrain-changed event, flood-fill from the body's water cells over neighbours with `ground < level`, limited to the dirty tiles and a maximum cell count per event. This is a CPU job on the same worker as the tile rebuild.
   - Raising ground above the level removes water there automatically, because the depth goes to zero.
   - Digging a hole that is not connected to any body stays dry, unless you add an "aquifer" rule. That is a design choice, not a technical one.
6. **Gameplay reads:** `WaterSystem::HeightAt` already exists. Replace it with `level(body at x, z)` so buildings, roads, pathing and sim can ask "is this cell flooded?".

**Cost estimate (est., not measured):** one extra draw per visible body, a simple pixel shader, a flood fill on edit only. No per-frame compute.

**Upgrade path if it looks too flat:** add a single low-frequency animated normal, then shoreline foam from depth, then cheap vertex waves on the near plane. Skip the existing ripple simulation unless boats or physics objects need to disturb the surface.

**Open decisions:**
- Do bodies flood automatically when dug into (step 5), or only when the player places water?
- Does anything need to float or push water (boats)? If not, the ripple sim is not needed.
- Ocean at the map edge, lakes only, or both?

## 12. Water decision: sparse flow simulation (CS2-style behaviour, cheap)

Decision: simulate flowing water, but only where there is water. Rendering stays the cheap shader from section 11; this section replaces its "water is a fixed level" rule with a flow solver.

**Design**
- Grid of 8 m cells (2500 x 2500 for 20 km), split into 32 x 32 cell tiles (256 m, matching the terrain edit tiles so one "terrain changed" event covers both).
- "Virtual pipes" shallow-water model: per cell a depth and four edge flows; flow follows the surface height (`ground + depth`) difference; outflow is capped to the water available; depth is updated from net flow.
- **Sparse and sleeping:** a tile is simulated only if it has water or touches a wet tile. Dry tiles cost nothing and allocate nothing.
- Sources (rivers, rain) add water; the map edge drains to a sea level; sinks remove water.
- Terrain edits wake the tiles around the dirty rectangle, so digging a channel makes water flow in with no special flood-fill rule.
- Ground height comes from the same height field as everything else (base + edits, resampled to 8 m).

**Measured** (`tests/water_bench.cpp`, one thread, desktop CPU, 3 river sources flowing for 6000 steps = 50 simulated minutes on the engine's noise terrain):
- 27 to 63 active tiles (28k to 65k cells): 0.6 to 1.1 ms per step on average, about 11 to 18 ns per active cell.
- Memory at 63 active tiles: 1.2 MB (a dense grid for the whole map would be 119 MB).
- One step took 64 ms once (worst case); I did not find the cause. It may be an operating-system hiccup but it is not explained.
- The scenario is small: after 50 simulated minutes only 2381 cells (0.15 km²) were wet. It shows the per-cell cost, not a big flood.

**Extrapolation (not measured):** at about 11 to 18 ns per active cell, 1 million active cells (a flood across 64 km²) would cost roughly 11 to 18 ms per step. Water is slow, so running it at 5 to 10 steps per second on a worker thread instead of every frame would leave a large margin.

**Not yet shown:** the model's visual quality and stability (it needs a larger `dt` and bigger depths tested), GPU compute version, iOS speed, interaction with the 2 m terrain edits, and gameplay effects such as flooding buildings.

**Build order:** (1) `WaterGrid` with tile allocation and the solver, unit-tested for volume conservation; (2) sources, sinks and sea edge; (3) wake on terrain-changed events; (4) render depth and flow into the cheap water shader; (5) gameplay queries ("flooded cell?"); (6) measure on an iPhone-class device before choosing CPU or GPU.

## 13. Status (implemented so far)

Done (build order steps 1 and a first pass of 2):
- `src/terrain_field.h`: `HeightField`, the sparse edit layer. 256 m tiles of 128 x 128 `int16` deltas (2 m, 2 cm), bilinear `EditAt`, round brush (`Add`, `Flatten`), dirty-tile list, per-tile versions, `Compact`, save/load (zero-run coding, malformed data refused). No platform headers.
- `src/terrain.h`: `Terrain::HeightAt` = `BaseHeightAt` (the old noise, unchanged) + `field.EditAt`; `Terrain::Brush(...)`. With no edits the result is identical to before. Chunks rebuild when an edit tile under them changes; the far rings rebuild on any edit.
- `tests/terrain_field_test.cpp` (`tests/terrain_field_test.bat`): 41 checks, all passing (empty field, raise/lower symmetry, tile-border continuity, flatten, saturation, outside-map brushes, versions and dirty list, compact, save/load including damaged data, Terrain integration and raycast).
- `src/main.cpp` still compiles (syntax-only check, no errors). The full engine was not built or run, and no brush tool or key binding exists yet, so nothing can be edited in-game until step 3.

Known gaps in what exists:
- A brush that touches a tile rebuilds every chunk under that 256 m tile (up to 16), and any edit rebuilds the far rings. Fine for a prototype, wasteful for continuous brushing.
- No mip min/max pyramid, no collision, nav mesh or water hooks yet, no undo, no editor/in-game brush UI.
- Not thread-safe: do not read while a brush is applying.
- Precision at 20 km (section 7) is not addressed yet: edits use `float` world coordinates.

New project: `C:\custom windows apps\citysim` (`project.tbdproject`, `Assets\`, `Scenes\`), created in the engine's project format and empty.
