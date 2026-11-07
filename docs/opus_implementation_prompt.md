# Implementation brief: city simulation + chunk-baked rendering

You are implementing two tracks in the cityengine C++ codebase (Windows, D3D12 + Vulkan, MSVC 2022, `/std:c++20`).
**Track S** is a cheap, scalable city simulation. **Track R** is a draw-call and triangle-efficient LOD system built on
baked city chunks. Both share one spatial unit, the **CityCell**.

Read this whole brief before touching code. Every "FACT" below came from a read-only survey of the working tree at
HEAD `fceaa42`. Re-verify any fact you depend on (file and line numbers drift) and say so when one is wrong.
Every number marked *(est.)* is an estimate, not a measurement. Do not report iPhone numbers: there is no Metal backend.

---

## 0. Ground rules

1. **Branch.** Create a new branch off `rail-spom-depot`. The working tree has many uncommitted changes from the user
   (see `git status`). Never revert, reformat or "clean up" files you were not asked to change. **Do not commit unless asked.**
2. **Do not use or extend** `src/character/`, `src/mocap/`, face / hair / clothes / photo-fit code or their docs. The user does
   not want them in the city engine. Do not delete them either; just do not depend on them. Do not make the city sim depend on
   `src/ai/npc_system.*` (it is Play-mode only and unrelated to city citizens).
3. **Both renderer backends.** Anything touching `src/render/renderer.h` must be implemented in `renderer_d3d12.cpp` **and**
   `renderer_vulkan.cpp`, and shaders need entry points in both the D3D12 list (`renderer_d3d12.cpp` ~:148) and `spirv.bat`.
   Keep the API backend-agnostic so a Metal backend can be added later.
4. **Headless-testable sim.** Put all Track S logic in headers under `src/city/` with no renderer, window or scene
   dependencies, like `utilities.h`, so it can be tested the way `tests/utilities_test` is (a `.bat` that builds with `cl`
   into `obj\<name>` and runs). Host glue goes in `.inl` files included by `main.cpp`.
5. **Measure, then optimise.** Every phase has an exit check. Log numbers (ms, draws, triangles, MB) in the phase report.
   If a measurement contradicts this brief, stop and report; do not force the design.
6. **Build:** `build.bat` (single translation unit: `main.cpp` includes the `.inl` city files, so any `.inl` change rebuilds
   it). Shaders are copied to `bin\shaders`; D3D12 compiles HLSL at startup (SM 5.1, cache in `bin\shadercache`); Vulkan uses
   `spirv.bat` (`dxc -spirv`, `-DVULKAN`).
7. **Determinism.** Core sim uses integer or fixed-point math and per-entity seeded RNG (SplitMix64 or PCG, one stream per
   entity, never a shared global stream). Same inputs and seed must give the same results.
8. **Report format** after each phase: what changed (files), exit-check result with numbers, what is *not* done or not
   verified, and any decision you made that was not in this brief.

---

## 1. Product constraints

- **Sandbox city builder**, top-down camera that can see much of the city when zoomed out. Player content is arbitrary:
  meshes, materials and textures cannot all be authored or predicted.
- **Eventual target: iPhone 15** (A16, 5-core GPU, 6 GB shared RAM, tile-based GPU, no hardware ray tracing), ~25 fps, internal
  resolution ~960x540 upscaled. Design for that even though only PC runs today.
- **Scale target:** 50k population, ~10k buildings, hundreds of visible people and vehicles, with the sim costing about
  1-2 ms per frame average *(est.)* on one performance core.
- **Frame budget** (from `docs/city_draw_budget_plan.md`, all est.): <= 400 draws all passes, <= ~1M triangles, <= ~1 GB GPU
  memory, buildings' share <= 80 draws / 300k triangles / 120 MB.
- The look matters: SPOM-style relief detail on a low-poly proxy is a deliberate part of the visual identity.

---

## 2. Facts about the current code (verify before relying)

### Zoning (`src/city/zoning.h`, `src/city_zones.inl`)
- No global grid. `ZoneGrid::Rebuild` (zoning.h:122) generates 8 m cells (`kCell` :54), 6 deep (`kZoneDepth` :55), along every
  non-highway, non-path road, both sides. `ZoneCell` (:57) = `c, dir, out, seg, side, col, depth, zone, lot` + four corner
  heights. Segments whose mid-height differs from ground by > 2 m (bridges, tunnels) get no cells (:145).
- **No block struct.** A "block" is only a flood-filled set of same-zone cells (`Fill()` :391). `BlockRect` (:66) is a no-zone
  exclusion rectangle for transport and utility buildings, not a zoning block.
- 7 zone types in 4 categories (`ZoneTypes()` :36). `Lot` (:92): cells, centre, front, `w`/`d` in cells (metres = n*8), type,
  model path, `progress`/`duration` (game minutes), `hasRoad`, `frontSeg`, `noRoadMinutes`. Model files are
  `<PREFIX>_<W>x<D>[_kind]_L<level>.glb`, w and d <= 6 (:576).
- `Tick` (:445) tries `TrySpawn` every 1.5 game minutes, max `maxConstructing` = 8 (:117, :470). **Level is only chosen at spawn;
  there is no level-up or downgrade.** Lots with no road for 10 days are condemned and demolished 5 days later (:79-83, :447-458).
- `ZoneGrid::version` (:114) bumps on any change. Host glue `ZonesSync` (city_zones.inl:108) rebuilds when
  `net.version != builtFrom`, advances `g_clock.minutes`, and spawns **one scene entity per lot** ("Zoned building <id>", `#native`
  mesh). There is **no instancing or merging for zoned buildings**, which is the largest draw-call problem today.
- Population and jobs are **not stored per lot**. They are derived each pass in `TransitDemand()` (city_transport.inl:306-332):
  homes `area * {0.8, 1.4, 2.5, 5.0}[type]`, jobs `area * {office 2.5, industrial 2.0, commercial 1.5}`. Garbage and utility demand
  are also per-cell by zone type (`PerCell` city_garbage.inl:21, `UtilDemandPerCell` utilities.h:113).

### Road graph (`src/city/road_network.h`)
- `Node` (:277): `Vec3 p`, `segs`, `alive`, `grounded`. `Segment` (:283): `a, b, c1, c2, profile, alive` (cubic Bezier in xz).
  `Profile` (:39): `fwd`/`back` lane counts + `Strip` list. **No lane graph; lanes are only counts and strips.**
- Elevation is per-node (`NodeY` :299). **Bridges and tunnels are not a stored type**: inferred at mesh time by height vs
  ground (`kTunnelDepth` 4 m, `kElevated` 3 m, :445-446, :687-688).
- `Network::version` (:294) bumps on add, remove, split. Removed segments stay in the vector with `alive=false` (stable indices).
- `Nearest`, `NearestNode`, `SegmentAt`, `OutDir` are **linear scans**; there is no spatial index.

### Utilities and garbage
- `UtilWorld::Solve` (utilities.h:335) is a real union-find solver per utility kind, built to run on change, outputting
  per-lot status, totals and per-segment/line "live" flags. Sources/lines: `UtilDefs()` (:73), `UtilLine` (:122).
  Several placement rules are "not enforced yet" (:87, :96-106). Reuse it; do not rewrite it.
- `GarbageSync` (city_garbage.inl:76) runs **every frame**, looping all lots and facilities (O(lots x facilities)) with
  `unordered_map` lookups. It needs a dirty flag and a slower rate. No trucks or routing.
- No pollution, water quality or fire logic exists.

### Game loop and clock
- `GameClock` (src/gui/zone_toolbar.h:28): `minutes`, `paused`, `speed` 0-2; `Rate()` returns 2, 6 or 18 game minutes per real
  second. Global `g_clock` (city_zones.inl:11). Shown in `city_hud.h:110-126`.
- **No central tick and no fixed timestep.** `main.cpp:3340-3346` runs, per frame and in order: `ZonesSync`, `UtilSync`,
  `TransportSync`, `GameSync`, `IndustrySync`, `GarbageSync`; each computes `minutes = realDt * g_clock.Rate()`.
  `ZonesSync` is the one that increments `g_clock.minutes`. Draw calls: `main.cpp:4201-4205`.
- **No treasury.** Money and population HUD cells show "-" and open "not completed" panels (`city_hud.h:152-156`). Economy and
  Statistics toolbar items are tool = -1. `UtilDef` has cost/upkeep fields that nothing spends.
- `TransitWorld` (transit.h:269-271, :483-499) keeps float aggregates `residents`, `commuters`, `unemployedShare`.
  `CitizensSync` (city_transport.inl:194) animates a pool of entities. There are no households or individuals.
- Saves are plain-text `.tbdgame`. Header comment says garbage stock and zone-grown buildings are **not saved**
  (city_game.inl:5-6).

### Threading (`src/core/jobs.h`)
- `max(1, hardware_concurrency - 2)` workers, below-normal priority. `g_engine.jobs`. API: `Run(fn, JobCounter*)` (:72),
  `Wait(JobCounter&)` (:93), `ParallelFor(count, minBatch, fn)` (:114), `RunBackground(job)` (:104; limited to `workers - 3`;
  `Wait()` never runs these), `WorkerCount()` (:129). **No futures or dependencies.**

### Profiling
- HUD lines: push strings into `lines` at main.cpp:4300-4311. Counters: `Stats::AddDrawCall/AddDraws`, fed from
  `r.Counts()` at main.cpp:4449. F3 overlay `profiler::Overlay`: add a field to `Sample` (profiler_overlay.h:34), a `row(...)` in
  `Draw` (~:227), and fill it near main.cpp:4218. It is hard-coded, not a registry. `FrameMark("name")` (main.cpp:2891) logs hitches
  > 50 ms. GPU pass timings exist (`Renderer::GpuPasses()`, renderer.h:376) but are passed as `nullptr`.

### Renderer, SPOM, LOD
- `renderer.h` has `DrawInstanced(mesh, instanceBuffer, first, count, viewProj)` (:326), `CreateInstanceBuffer` (float3x4 rows),
  `DrawTransient` (:328), `FrameCounts` (:168). **Render-to-texture and texture arrays are not in the public API.**
  `BeginCapture/EndCapture` (:320) is an internal lit capture for probes. `CreateTexture` is 2D only.
- `docs/impostor_cards_plan.md` says Phase 0 done: both backends have an **internal impostor store** (three texture arrays,
  per-layer render targets, bake tile depth, descriptors, sampler) at renderer_d3d12.cpp:3136 and renderer_vulkan.cpp:3300,
  validated by `TBD_TEXTURE_SELFTEST=1`. Phases 1-4 are not implemented and `renderer.h` has no impostor functions. Verify
  whether the store functions exist as members before assuming you can expose them.
- **SPOM today** (mesh.hlsl:1118-1220, `SpomPS`): marked by glTF material name containing "spom" -> vertex alpha 0.8
  (mesh_import.h:1717, :1792). Vertex normal = facade outward normal; `uv` = (x, y) metres on the wall; `color.rgb` = width,
  height, vertex depth out from the wall; `color.b > 5` marks a single-quad volume. Relief is a solid interval per texel:
  `gAlbedo.a = end / kSpomDepth`, `gNormalMap.a = start / kSpomDepth`; rgb of both = colour and fine detail.
  **`kSpomDepth = 1.2` m is hard-coded** (:1130). March is 64-256 steps at ~2 cm + 6 bisection steps (:1168-1182), no `SV_Depth`
  write, 4 shadow taps. Cows' far LOD is a 12-triangle SPOM volume (`tools/building_gen/gen_cattle.py`,
  `src/city/industry_spom.h`).
- LOD/culling: `mesh_lod.h` (quadric edge collapse, up to 4 levels to ~64 tris, disk-cached by content hash; `AddAutoLods`
  ~main.cpp:848). `BuildInstanceCells` (culling.h:102): 16 m cells if max distance <= 80 m else 64 m, with per-cell sphere
  culling and fade (main.cpp:3890-3905). `RoomSplit` (main.cpp:852). Terrain `kChunkSize` = 64 m (terrain.h:33).
  `world_streaming.inl`: `kWholeMapCell` 2048 m, `kAlwaysRadius` 5000 m, default load radius 1000 m.
- Tests exist in `tests/` (`road_test`, `transport_test`, `utilities_test`, `terrain_field_test`, `gui_test`, ...), each with a
  `.bat`. **No zoning, garbage or sim tests exist.**

---

## 3. Shared foundation: the CityCell

Because zoning has no block struct (see facts), use a **fixed world grid of 128 m cells** (two terrain chunks) as the unit for
both baking and sim aggregation. Cell key = `(floor(x/128), floor(z/128))`.

```cpp
struct CellKey { int32_t x, z; };            // hash + equality
struct CityCell {
    CellKey key;
    std::vector<uint32_t> lots;              // lot ids whose centre lies here
    uint32_t simVersion, renderVersion;      // bumped on any change that affects each side
    uint8_t  dirty;                          // bit0 render, bit1 sim, bit2 flow
    // Track S fields live in CellSim (S1); Track R fields in CellRender (R1).
};
```

- One `CityCells` registry owns the map. A lot is assigned to a cell by its centre. Spawn, level change, demolish and
  recolour set the cell's dirty bits **once**. Road edits dirty all cells the edited segment crosses (use a cell-grid spatial index
  you build over segments; see S5).
- Keep this registry independent of the renderer (header in `src/city/city_cells.h`) so Track S can test it headlessly.
- Optional later: road-bounded blocks as a refinement. Do not build them now.

---

## 4. Track S: simulation

### Architecture
Three tiers, with only the last scaling with what the player sees:
1. **Aggregate** (whole city): counters per CityCell, ticked slowly and staggered.
2. **Flow** (traffic): vehicle volume per road segment, solved on a graph every few game minutes.
3. **Agent** (local): cosmetic cars and pedestrians near the camera, plus **tracked people** the player clicked.

Core rules: flat arrays (structure-of-arrays where hot), integer or fixed-point, **no per-citizen objects for anonymous
people**, edits queued and applied at tick boundaries, every system has a profiler line and a time budget.

### S0: Fixed-step scheduler and clock plumbing
- New `src/city/sim.h` with `struct SimScheduler`: a fixed sim tick of **1 game minute**. Per frame, accumulate
  `realDt * g_clock.Rate()` into a minute accumulator and run up to N ticks, stopping when the per-frame sim budget
  (default 2 ms, configurable) is spent; leftover time carries over, so the sim slips behind rather than hitching.
- Systems register as `{name, periodMinutes, phaseStagger, fn}`. A system with period P and stagger count K processes
  `1/K` of its cells each `P/K` minutes, selected by `cellIndex % K`.
- Integrate as a new `SimSync(realDt)` called **after** `GarbageSync` at main.cpp:3346. Do not rewrite the existing `*Sync`
  functions yet, but make `SimSync` the only place that advances new systems. Leave `ZonesSync` as the owner of
  `g_clock.minutes` unless moving it is trivially safe, and say what you chose.
- Add per-system timing (ms last frame, ms rolling average) and a row per system in the F3 overlay (add to `profiler::Sample`).
- **Tests:** `tests/sim_sched_test`: determinism across two runs, budget overrun carries over, pause stops ticking, speed changes
  do not change per-minute results.
- **Exit:** at 3x speed the scheduler holds its budget with the existing systems running; timings visible in F3.

### S1: CellSim aggregates, lot capacity, households and jobs
```cpp
struct CellSim {
    uint16_t homeCap, jobCap[3];           // capacity from lots (industrial, commercial, office)
    uint16_t residents[3];                 // kids, adults, seniors
    uint16_t employed[3], vacancies[3];
    uint16_t edu[3], wealth[4];            // simple bands
    uint8_t  happiness, landValue;
    uint8_t  powerOk:1, waterOk:1, sewageOk:1, garbageOk:1;   // from UtilReport
    uint8_t  coverage[6];                  // fire, police, health, school, park, transit (0..255)
};
```
(Field list is a proposal; keep it small and cache-friendly. Justify any change.)
- Centralise lot capacity in one function `LotCapacity(const Lot&)` using the same per-area factors as `TransitDemand`
  (city_transport.inl:306-332); make `TransitDemand` call it so they cannot diverge.
- Household/job matching is **event-driven and counter-based**: when a lot completes, is demolished, or its level changes, adjust
  the cell's capacities and re-balance residents/jobs through **per-district vacancy lists** (district = a group of cells; start with
  a 4x4 cell supercell). No per-person search. Unemployed residents fill vacancies in the same supercell first, then neighbours.
- Population = sum of residents. Wire the HUD population cell (`city_hud.h:152-153`).
- **Tests:** `tests/sim_cells_test`: conservation (residents >= employed, vacancies = jobCap - employed), demolish/rebuild
  keeps totals consistent, 50k synthetic residents across 10k lots runs a full matching pass under 1 ms *(target)*.

### S2: Demand, growth, level-up, abandonment
- Demand in [-1, 1] per category (R, C, I, Office), smoothed over ~a game day:
  R from vacancies and unemployment; C from residents' spending vs commercial capacity; I and Office from unemployment vs jobs.
  Put every constant in **one data file** (`assets/sim/tuning.json` or a constexpr header the user can edit) so tuning needs no rebuild
  if loaded at runtime.
- Growth: gate `ZoneGrid::TrySpawn` on category demand > threshold, and weight category choice by demand. Keep `maxConstructing`.
- **Level-up/downgrade (new):** a lot upgrades to level+1 (swap to a `_L<level+1>` model with the same `w x d` footprint, rerun the
  construction animation) when land value and service coverage exceed thresholds for N days; downgrades or empties on sustained
  negative demand, missing power/water, or no road. If no same-footprint next-level model exists, skip the upgrade and log once.
- Abandonment keeps the existing road-loss rule (zoning.h:79-83, :447-458) and adds utility and demand causes.
- **Tests:** `tests/sim_growth_test` with a synthetic network and fixed seed: demand rises when jobs exceed residents, lots spawn,
  lots level up under good coverage, abandon under sustained bad conditions, byte-identical results across runs.
- **Exit:** a painted district grows, levels and decays in a playable, non-oscillating way over several game months.

### S3: Utilities and services coverage (reuse `UtilWorld::Solve`)
- Drive `UtilWorld::Solve` from the dirty flag (network, source or lot change) and cache its `UtilReport`; write per-lot status into
  `CellSim.powerOk/waterOk/sewageOk`.
- Fix `GarbageSync`: dirty-flag it, run at a low rate (once per game hour), and replace the O(lots x facilities) loop with a
  per-component facility list. Keep behaviour identical for existing saves.
- Services (fire, police, health, school, park) as **coverage fields**: each service building contributes a radius-limited influence
  to cells it can reach **along the road graph** (reuse the union-find components, then a bounded BFS or distance cap), stored in
  `CellSim.coverage[]`. Recompute only when a service building or road changes. Do not simulate vehicles.
- **Tests:** extend `utilities_test` for the cache and dirty behaviour; `tests/sim_coverage_test` for reachability and radius.

### S4: Economy
- Treasury as `int64_t` in cents. Daily income: residential/commercial/industrial/office tax from `residents * wealth factor`,
  `jobs filled`, etc. Daily expenses: `UtilDef` upkeep and service-building upkeep (add upkeep fields where missing).
- Minimal budget panel: tax rates per zone category and a service funding slider that scales coverage radius or capacity.
  Wire the HUD money cell (`city_hud.h:155-156`). Optional loans after the basics.
- **Tests:** deterministic ledger over a synthetic month; no overflow; zero-population city earns nothing.

### S5: Flow traffic
- Build a **CSR graph snapshot** from `Network` (segments as edges, nodes as vertices), versioned by `Network::version`. Edge
  capacity from `Profile` lane counts and a speed from road type. Build a **uniform-grid spatial index over segments** (also used to
  dirty cells on road edits and to replace the linear scans for `Nearest` in sim code).
- Trips come from a **gravity model** between cells: homes -> jobs and homes -> commercial, weighted by capacity and distance decay.
  Aggregate to **destination clusters** (cells with jobs).
- Assignment: for each destination cluster, one reverse Dijkstra over the CSR graph with a generalized cost
  `time*wT + distance*wD + turns*wC (+ toll*wM)` and a **BPR congestion** term `t0 * (1 + 0.15 * (v/c)^4)`. Split each origin's flow
  between the 2-3 cheapest routes with a **logit** share. Update **a fraction (0.1-0.2) of flow per solve** (method of successive
  averages) to damp oscillation. Use per-age cost weights (teens money, adults time, seniors comfort) as profile multipliers.
- Run solves on a worker (`RunBackground`) against the immutable snapshot; swap results in at a tick boundary. Cache by graph
  version; spread rebuilds over frames after a big road edit.
- Output per segment: flow, speed ratio, congestion class (for an overlay and for cosmetic cars). Gridlock relief: cap per-edge
  flow growth and flag the edge instead of letting the solution diverge.
- Bridges/tunnels: treat them as normal edges using the same `kElevated`/`kTunnelDepth` rules.
- **Tests:** `tests/sim_flow_test`: opening a faster highway shifts flow to it; a longer-but-slower road keeps a share; added
  congestion shifts flow back; a road edit invalidates and re-solves; determinism; a 10k-segment graph solves within a budgeted number
  of ms per frame when sliced *(report numbers)*.

### S6: Tracked people (click-to-materialize)
- Anonymous residents are counters. Give each cell numbered **resident and job slots**; a slot is anonymous or points to a stored
  `Person`.
- `Materialize(cell, slot)` samples a person **consistent with the cell's counters** (age band, job, education, wealth) using an RNG
  seeded from `(cellKey, slotIndex, generatorVersion)`, so clicking the same slot twice before storing gives the same person.
  Conservation: a materialized worker removes one anonymous worker from the job slot's counters and adds one named record.
- `Person` stores only what the sim decides with: home, job, age, education, wealth, health, happiness, and a seed for cosmetic
  details (name, traits). A **schedule function** `Location(person, gameMinute)` is a pure function of seed + job + time.
- `Collapse(person)` returns them to aggregates (keep seed and story facts) when untouched for a long time or over a cap
  (default 2000 tracked).
- Changes to a tracked person (job loss, move) update both cells' counters.
- Save/load stores tracked people in full; everyone else regenerates.
- **Tests:** `tests/sim_person_test`: same slot -> same person; materialize/collapse round trip preserves totals; moving house
  updates both cells; job loss raises unemployment; no negative counters under random operation sequences (fuzz with a fixed seed).

### S7: Cosmetic traffic and pedestrians
- Visible vehicles and walkers are **not simulated**. For each segment near the camera, spawn cars from the S5 flow and speed
  numbers. Position is a pure function `f(segment, laneOffset, hash(id), time)`, evaluated on the CPU first (hundreds of instances is
  cheap); a vertex-shader version is an optional later optimisation. Draw each class with **one** `DrawInstanced` call.
- Visible-agent cap from a budget (default 300 people, 300 cars), scaled by real flow so density still tracks the sim. Nothing is
  drawn past a distance threshold. Gridlock relief: despawn or respawn stuck-looking cars instead of simulating them.
- Cars only react visually (slow down in density, stop at lights if lights exist later). They never decide anything.
- Reuse existing vehicle and citizen meshes where they exist (`transport_models.h`, `CitizensSync`); verify before adding assets.

### S8: Save/load and tooling
- Add a versioned `SIM` section to `.tbdgame`: cell counters, tracked people, treasury, tax settings, tuning overrides, city seed.
  Also fix the known gap that zone-grown buildings are not saved (city_game.inl:5-6) **or** regenerate them deterministically from
  the seed; state which and why.
- Debug: `TBD_SIM_STRESS=<population>` builds a synthetic city (lots, roads, residents) for profiling at 10k, 50k, 100k and 200k.
  An overlay for traffic flow and a cell inspector (counters, demand, coverage) behind a dev toggle.

### Track S: explicitly out of scope for now
Individual vehicle AI and lane changes, accidents, per-agent pathfinding, simulating anonymous people, weather and seasons,
disasters, full pollution/land-value diffusion (start with a coarse land-value grid only), production chains, transit line
scheduling (treat transit as a cost modifier on flow first), anything involving the character/mocap code.

---

## 5. Track R: rendering and LOD

### Architecture
Draws are bounded by the number of **CityCells in view**, not by what the player built. Per cell there are three states chosen by
**on-screen size in pixels** (not distance), with hysteresis and a dithered cross-fade:

| State | Content | Draws |
|---|---|---|
| Near | Real meshes, instanced / merged | a few per cell, only a few cells near the camera |
| Mid | SPOM box or displaced-grid proxy from the cell's bake, optionally a simplified merged mesh | 1 per cell |
| Far | Same bake, cheapest renderer tier | 1 per cell (or one instanced draw for all far cells) |

Separate classes are **never** baked into the ground height: bridges/elevated roads, vehicles, pedestrians, trains, trees (impostors
exist as a plan), and tall landmarks.

### R0: Measure and instrument (do this first)
- Add per-category draw and triangle counters (zoned buildings, roads, rail, terrain, water, vegetation, props, UI, shadow) to the
  F3 overlay and HUD lines, using `FrameCounts`.
- Add `TBD_CITY_STRESS=<n>` that places `n` zoned lots (10k target) with a mix of models, plus a few deliberately bad player-style
  cases: unique materials, a multi-million-triangle mesh, a bridge over a road.
- Record baseline: draws, triangles, CPU ms, GPU ms (use `GpuPasses()` if it works; wire it into the overlay if cheap).
- **Exit:** a table of baseline numbers for the 10k scene at street level, mid zoom and whole-city zoom.

### R1: Cell membership and dirty queue
- Reuse `CityCells` (section 3). Maintain `CellRender`: lot list, resident LOD0 meshes (if any), bake state, bake version.
- Dirty queue processed on a worker with a per-frame time budget; **at most one rebake per cell per ~1 s**.
- Prefer reusing existing instancing (`InstancedAsset`, `BuildInstanceCells`, culling.h:102) for the Near state **before** writing a
  merger. Only merge static meshes per cell if R0 proves instancing is not enough. If merging, use `CreateMeshNoWait`/`RetireMesh`
  (verify they exist) so swaps never stall.

### R2: Public render-to-texture and bake API
- Build on the internal impostor store (renderer_d3d12.cpp:3136, renderer_vulkan.cpp:3300) after verifying what it can do. Expose a
  minimal backend-agnostic API in `renderer.h`, for example:
  ```cpp
  virtual int  CreateBakeStore(uint32_t slots, uint32_t faceRes, BakeFormat fmt) = 0;
  virtual bool BeginBakeFace(int slot, int face, const Mat4& orthoViewProj) = 0; // renders the supplied draws
  virtual void EndBakeFace() = 0;
  virtual void FreeBakeSlot(int slot) = 0;
  virtual bool DumpBakeSlot(int slot, const wchar_t* bmpPath) = 0;  // debug
  ```
- Per face, write: **albedo** (RGBA8: rgb unlit colour, a = coverage), **normal** (RGBA8: rgb normal, a = baked AO),
  **depth/relief** (at least 12 bits; recommend RG16: start and end of the solid interval, in volume-relative units). **Do not**
  rely on the 8-bit alpha packing used by cow SPOM for chunk scale (40 m / 256 = ~16 cm steps).
- Bake **unlit** (no sun, no shadow baked in) so time-of-day still works; bake AO into a channel.
- Must work identically on D3D12 and Vulkan; keep `TBD_TEXTURE_SELFTEST=1` passing; validation layers clean.
- **Exit:** a test that bakes one cell's content and dumps the 5 faces to BMP that look correct. Mirrored axes are the likely bug:
  define the per-face (u,v) <-> axis table once in a shared header and use it in both the bake and the shader.

### R3: Cell bake scheduler
- Volume = cell AABB in xz (128 m) and y from terrain minimum to max building top + margin. Five faces: top and four sides.
  Resolution defaults *(est.)*: top 256^2, sides 256x64 (about 1 MB per cell total); expose as settings. Stored depth measures from
  the **cell border inward** for sides and from the top plane downward for the top.
- **Trigger and priority:** when a cell's footprint crosses `bakeEnter` (~0.85 x render radius, with hysteresis), bake the **two
  faces the camera currently sees** first. After the cell leaves the render radius, bake the remaining three **slowly**: at most
  one face per frame and within a configurable GPU/CPU budget (default 1.0 ms). Never bake while the camera is moving fast.
- While a face is unbaked use a **stand-in** (the average colour of the top bake, tinted) so there is never a hole or a pop. If the
  camera orbits to reveal an unbaked side, show the stand-in until it completes.
- Exclusions from the ground bake: tag elevated/bridge segments (use the same `kElevated`/`kTunnelDepth` rules as
  road_network.h:445-446), vehicles, pedestrians, trains, trees (when impostors exist) and any entity marked landmark. They draw
  separately at distance as simplified geometry or are hidden.
- Cache baked results in a per-cell cache keyed by `renderVersion` (disk cache like `mesh_lod.h`'s content hash) so reloads and
  never-visited cells do not rebake.
- Dirty cells re-bake only when they next enter the bake band; far cells that change while distant use their old bake until then,
  then update.

### R4: Chunk SPOM renderer (High tier)
- Generalise `SpomPS` (mesh.hlsl:1118-1220) to a new entry point `ChunkVolPS`. Do **not** change cow/balcony SPOM behaviour; add a new
  marker (a new vertex-alpha code or a separate pipeline) and keep the old path intact. Check `mesh_import.h:1795-1802` for unused
  alpha codes.
- Per-volume constants (depth range, volume size, atlas slot) in per-instance data, not `kSpomDepth`.
- Proxy mesh: a 5-face box (10 triangles, no bottom). Draw only camera-facing faces.
- Ray march against **all five faces' relief** (hull intersection, as in the impostor plan's tri-planar ray cast): the top face gives
  the roofline and depth; side faces supply wall colour and overhangs, so walls are not smeared with roof colour.
- **Adaptive step count**: scale with distance and view angle, cap at ~24 steps *(est.)* (the cow path's 64-256 is far too
  expensive at chunk scale), plus 3-4 bisection steps. Skip the march where a baked "flatness" mask says the surface is flat.
- Lighting at runtime with the existing sun/sky model; AO from the baked channel; shadows between buildings come only from baked AO.
- No `SV_Depth` write initially; accept small intersection errors at distance. Measure overdraw; discard defeats tile-GPU
  hidden-surface removal, so keep the march short.
- Add the entry point to the D3D12 list and `spirv.bat`; verify visual parity across APIs.

### R5: Cheaper tiers (Low and Medium) from the same bake
- **Low:** flat textured box, no march (uses the top albedo/normal and a tint).
- **Medium:** displaced grid: one shared 32x32 grid mesh instanced per cell, vertex shader reads baked height; pixel shader uses
  baked albedo/normal, **no discard**, keeps early-Z. If vertex texture fetch through a texture array is awkward on a backend, use
  per-cell textures and say so.
- **Hybrid option (test it):** displaced grid for silhouette + a **short** relief march (<= 6 steps) for fine detail.
- Settings: `Chunk quality = Low | Medium | High | Auto`. Auto picks by measured frame time (R7). All tiers consume the same bake data,
  so changing the tier never re-bakes.
- **Exit:** same cell rendered by all tiers from overhead and 45 degrees, with screenshots, GPU ms and overdraw notes.

### R6: Transitions and LOD selection
- Choose the state per cell by **projected screen size in pixels**, not distance. Hysteresis of ~12%. Dithered cross-fade between
  Near and Mid so there is no pop. Match lighting/colour between baked and real; compare side by side at the switch point.
- A debug mode forcing Near / Mid / Far for all cells, and a per-cell overlay colouring the current state.

### R7: Budget governor and cost meter
- Track frame time and draw count each frame; **shrink the radius at which cells stay Near** when over budget and grow it back when
  there is headroom, with hysteresis. Also scale the visible people/vehicle caps and bake budget.
- Per-cell cost meter (triangles, materials, texture MB, bake ms) shown in the cell inspector; soft per-cell limits with a visible
  warning. Import limits for texture size and triangle count are a separate, optional step.

### R8: Mid-distance fallback for complex cells
- For cells that are too complex or have non-bakeable content, fall back to a **simplified merged mesh** using `mesh_lod.h`
  (`BuildLodChain`, content-hash cache). Build the merged proxy on a worker and swap with `CreateMeshNoWait`/`RetireMesh`.
- Compare against the baked tiers at mid distance and let measurements decide where each wins; report the result.

### R9: Separate classes
- Bridges and elevated roads: generated ribbon with simplified geometry at distance, piers instanced or dropped; excluded from the
  ground bake; drawn as their own thin layer above the baked cell.
- Ground roads and rail: included in the bake (they are in the scene) so they cost no extra draws when far.
- People and vehicles: see S7 (one `DrawInstanced` per class, hidden past a distance, shadows only for near agents, blob shadow at
  mid range). Optional later: vertex-animation-texture walk cycles (check `tools/anim_gen/` first; do not assume it fits).
- Trees and props: use the impostor plan when it exists; until then instanced meshes with a distance cutoff.

### R10: Platform notes for iOS (document only; do not build)
No depth prepass on tile GPUs, shadows only for near cells, ASTC textures with offline pre-compression for catalog assets, no
runtime ASTC encode (keep runtime bakes in RGBA8 or RGB565 at the lower resolutions above), no hardware ray tracing. Note every
assumption that needs a device measurement.

### Track R: explicitly out of scope for now
GPU-driven indirect rendering (note it as an alternative if baking falls short), octahedral impostors beyond what the impostor plan
already specifies, a Metal backend, per-building SPOM box proxies (only if the cell bake looks insufficient in the mid band),
road-bounded blocks.

---

## 6. Budgets and gates (enforced by the HUD)

| Item | Target (est.) |
|---|---|
| Sim average | <= 2 ms/frame at 50k pop on one core; hard per-frame cap with carry-over |
| Flow solve | sliced so no frame exceeds the sim cap; graph snapshot off-thread |
| Draws, all passes | <= 400 |
| Cells in view, Mid/Far | ~30-60, one draw each |
| Cell bake | one face per frame, <= 1 ms on the bake budget; never while the camera moves fast |
| Baked texture memory | ~1 MB per cell at default resolution, <= ~30 MB for the cells in view (adjust by measurement) |
| Tracked people | <= 2000 (collapse beyond) |
| Visible agents | <= 300 people + 300 vehicles (governor-scaled) |

Do not move to the next phase while the current phase is over budget on the PC stress scene, unless you explain why and get a decision.

---

## 7. Order of work and checkpoints

1. **R0** and **S0** in parallel (they touch different code): baseline numbers and the scheduler.
2. **S1 -> S2** (aggregates, demand, growth, levels). **Checkpoint A:** show the user a growing district with F3 timings.
3. **R1 -> R2 -> R3** (cells, bake API, scheduler), then a **one-cell prototype** of **R4** compared against the real cell and the
   simplified-mesh proxy (R8). **Checkpoint B:** screenshots and numbers; the user decides whether to continue with SPOM or fall back.
4. **S3 -> S4** (services, economy), then **R5 -> R6 -> R7** (tiers, transitions, governor).
5. **S5** (flow) with its tests, then **S6** (tracked people), **S7** (cosmetic agents), **R9**.
6. **S8** (save/load, stress tools), then re-measure everything against section 6. **Checkpoint C:** final numbers and a list of
   what needs a real iPhone measurement.

At each checkpoint stop and report. Do not start the next track phase until the user answers.

---

## 8. Decisions already made (change only with a reason)

- Sim unit and bake unit are the same 128 m **CityCell**; road-bounded blocks are deferred.
- Fixed sim tick of 1 game minute, scheduler with per-frame budget and carry-over.
- Flow-based traffic with destination-cluster Dijkstra, logit splits and damped updates; cosmetic agents driven by the flow.
- Anonymous people are counters; named people are materialized on click and are consistent with the counters.
- Chunk bake uses 5 faces, unlit, with at least 12-bit relief depth; the two visible faces bake first and the other three slowly.
- All render tiers read the same bake data; quality tier is a setting plus an auto governor.
- Sim code is renderer-free, header-based and covered by headless tests.

## 9. Things to confirm with the user before starting

1. Branch name and whether to keep `ZonesSync` as the owner of `g_clock.minutes`.
2. Whether `assets/sim/tuning.json` should be loaded at runtime (hot-editable) or compiled in.
3. Default render radius and cell size if 128 m turns out wrong in R0 measurements.
4. Whether zone-grown buildings should be saved or regenerated (S8).

If anything in section 2 is wrong or has changed, state the correction and adjust the plan before coding.
