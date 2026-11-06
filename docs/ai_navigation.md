# AI navigation and NPCs

Branch `worker/navmesh-npc` (from `western-realism` 088c395). Recast/Detour v1.6.0 (zlib licence, `third_party/recastnavigation/License.txt`) is vendored; `build.bat` builds it once into `obj\recast\recast.lib`.

## Files

| File | Role |
|---|---|
| `src/ai/ai_world.h` | The only place world <-> AI position conversion happens. AI space = world minus an offset. A floating-origin rebase calls `ai::ShiftWorld(delta)`; tiles, paths and agents are untouched. |
| `src/ai/nav_mesh.h/.cpp` | Tiled navmesh (64 m tiles), builds, disk cache, queries, debug geometry. |
| `src/ai/npc_system.h/.cpp` | Detour crowd, behaviour table, level of detail, rigs, test report, debug draw. |
| `src/physics.h/.cpp` | Added `CollectStaticTriangles`, kinematic capsules (`CreateKinematicCapsule`, `MoveKinematic`, `DestroyKinematic`). |
| `src/scene.h`, `src/scene_io.h` | `NpcInfo` on `Entity`, `N|` record. |
| `src/main.cpp` | Glue: rig creation from Wyatt's entities, Play hooks, debug draw call, editor keys, report. |

## Navmesh

- Agent: radius 0.4 m, height 1.8 m, step 0.4 m, slope 45 deg. Cell 0.25 m, height 0.2 m.
- Source geometry is the Jolt **static** collision (what physics streams in), not the render meshes. Whatever physics has no body for (vegetation, `collision: none` assets, the procedural terrain in `terrain.h`) is not in the navmesh.
- `NavMesh::Update` runs each frame on the main thread: for tiles within 75 m of the focus (the player) whose collision has loaded (`PhysicsWorld::ReadyAround`), it gathers triangles from Jolt (one tile per frame), then a background job (`JobSystem::RunBackground`) rasterizes and meshes it, and the main thread adds the result to the Detour mesh. At most 3 builds in flight. Tiles further than 150 m are removed.
- Cache: `bin\navcache\<hash>.nav`. The key is an order-independent hash of the tile's triangles plus tile coordinates and `kCacheVersion`; change the build settings and bump the version.
- Roofs: flat roofs are walkable to Recast. They are not connected to the street, so agents never get there (random points are picked by flood from the agent's own polygon), and the debug view draws unreachable polygons red.

## API (`ai::NavMesh`, world-space positions)

```
PathResult FindPath(from, to)                    // sync, main thread
uint32_t   FindPathAsync(jobs, from, to)         // worker; poll with TakePath(ticket, result) on later frames
bool       Raycast(from, to, hit)                // true = blocked, hit = where
bool       NearestPoint(p, halfExtents, out)
bool       RandomPointAround(from, radius, out, seed)   // reachable from `from`
uint32_t   Version()                             // changes when a tile is added or removed
```

A destination on an unloaded tile (or off the walkable ground) returns a **partial** path (`PathResult::partial`) to the nearest point on a loaded tile, with `goal` set to the real destination. Ask again when `Version()` changes; cheap-tier NPCs do this themselves. Async queries use one `dtNavMeshQuery` per worker and a reader lock; tile add/remove take the writer lock.

## NPCs

- Each NPC is an `ai::Npc` in AI space with a routine, a state (`idle, wander, patrol, follow, flee, indoors`) and a goal. The routine table in `npc_system.cpp` (`townsfolk, guard, loiterer, follower, skittish`) holds the idle times, activity weights, and whether they face the player, flee, follow, or go indoors at night.
- Day clock: `NpcFrame::hour` is `sun.hour`. Routines with `nightIndoors` and an indoors point walk to it between about 20:30-22:00 and 05:30-07:00 (staggered per NPC), disappear, and come out in the morning.
- Reaction: an idle NPC with `faces` turns toward the player inside `reactRadius`. `skittish` runs away when the player is inside 0.7 x `reactRadius`. `follower` follows within 60 m.
- Level of detail, decided every 0.3 s by distance to the player: **full** (within `TBD_NPC_RADIUS`, default 70 m, and covered by a loaded tile) = Detour crowd agent, kinematic Jolt capsule (the player and raycasts see it), one skinned rig; **cheap** = stored path followed in a line (async path request), no skeleton, no capsule, no crowd. Caps: `TBD_NPC_RIGS` (12) and `TBD_NPC_AGENTS` (48). The player is a non-steering crowd agent so NPCs walk around him.
- Rigs are clones of Wyatt's entities (`CreateNpcRig` in `main.cpp`) with the same idle/walk/run blend, driven by speed with the same constants as `PlayerCharacter`. They are created on demand, parked at y = -10000 when unused, and removed when Play stops. A scene with no Wyatt gets agents without bodies.

## Authoring

Select an entity in the editor viewport and press **N** to cycle its NPC routine (townsfolk, guard, loiterer, follower, skittish, off), **P** to add the camera target as a patrol point, **I** to set the camera target as the night door. A toast confirms. Saved as `N|entity index|routine|wander|react|indoors or -|x y z;x y z` after the `E` lines; old scenes load unchanged, and old engines ignore the line. In Play the marker entity is moved away and restored on Stop. There is no inspector panel or outliner badge yet (the inspector is a fixed Win32 transform panel).

## Environment variables

| Variable | Meaning |
|---|---|
| `TBD_NPC_COUNT` | NPCs added around the player on top of scene NPCs (max 256). |
| `TBD_NPC_RIGS`, `TBD_NPC_AGENTS`, `TBD_NPC_RADIUS` | Caps and full-simulation radius. |
| `TBD_NAVDEBUG=1` | Draw navmesh polygons (green reachable, red unreachable), agent paths, capsules (colour = state, red = stuck), cheap-tier agents as orange diamonds. |
| `TBD_NAVDEBUG_CAM=<m>` | Camera distance, looking down, for captures. |
| `TBD_NPC_REPORT=<file>` | Written when a `TBD_PLAYTEST` script ends. |

Debug drawing uses `Renderer::DrawTransient`, which both backends implement, so there is no per-API code. `TBD_CAPTURE`/`TBD_SHOT` runs do not start physics (so no navmesh); use `TBD_PLAYTEST` with a `shot <name>` command instead, which writes `<name>_<api>.bmp` from the same readback.

Test command (the 60 s run):

```
set TBD_VIEWPORT=1920 1200 & set TBD_NOVSYNC=1 & set TBD_NPC_COUNT=20 & set TBD_NAVDEBUG=1 & set TBD_NAVDEBUG_CAM=45
set TBD_PLAYTEST=hour 12;wait 62;shot nav60 & set TBD_PLAYTEST_DIR=<dir> & set TBD_NPC_REPORT=<dir>\report.txt
bin\tbd_engine.exe --api=d3d12 --scene "<abs>\perdition.tbdscene" "<abs>\WesternGame"
```
The script clock starts once NPCs are placed (`NpcSystem::Pending`).

## Results (Perdition, 20 NPCs, 60 simulated seconds, 1920x1200, uncapped)

Other engines may have been running on the machine; frame times are not a clean benchmark. See `docs/ai_nav_report_*.txt`, `docs/ai_nav_d3d12.png`, `docs/ai_nav_vulkan.png`.

| | D3D12 | Vulkan |
|---|---|---|
| tiles | 4 loaded (cold run: 4 built, 79 ms avg, 108 ms max; later runs 4 from cache, ~8-11 ms total) | 4 from cache |
| path requests / partial / failures | 28 / 0 / 0 | 28 / 0 / 0 |
| move requests | 61 | 61 |
| agents stuck over 5 s | 0 | 0 |
| frame p50 / p99 | 10.3 / 16.8 ms | 10.7 / 15.8 ms |
| NPC update p50 | 0.08 ms | 0.07 ms |

A 45 s night run (hour 23) put 12 of 20 NPCs indoors (hidden) and 7 in the full tier on D3D12.

What the captures show (top-down, same view on both APIs): the navmesh covers the open ground of the plaza and streets; polygons stop at building walls, porches and props (holes around the water tower, wagon, crates); flat rooftops are separate red islands, not connected to the ground. No navmesh inside walls was seen.

## Known gaps / untested

- Only four tiles exist in Perdition because physics collision streams within 120 m of the player; the navmesh follows that. Bigger `full_world` coverage, a destination in a tile that is really unloaded (partial path then continue as tiles load), and tile eviction while walking a long distance were **not exercised in a run**; partial-path code is unit-untested.
- A scene-authored guard (`N|` record on an added marker entity, 3 patrol points, Vulkan, 40 s) was placed from the scene and walked with 0 stuck. The N/P/I editor keys themselves were not tested in the editor; the scene round trip was also checked with a small program.
- Cheap-tier NPCs outside any loaded tile simply wait; no off-navmesh simulation.
- No doors/interiors: "indoors" is a point on the navmesh where the NPC disappears.
- Doesn't use terrain.h's procedural terrain (not part of the Jolt static bodies). Vegetation is not an obstacle.
- Rigs are plain Wyatt clones (no variety), 5 skinned meshes per rig; skinning is on the CPU, so rig count is the cost cap. The frame-time table does not separate NPC cost from the rest of Play.
- Agent-vs-agent separation and the player-as-agent trick were not examined or measured; the captures were not checked for overlaps.
- Floating origin: positions are in AI space and `ShiftWorld` exists, but no rebase was run against it.
