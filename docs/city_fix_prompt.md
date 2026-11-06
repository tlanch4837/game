# Prompt: audit and fix the Port Lazare town (full_map)

You are working in `C:\custom windows apps\gameengine` (TBD engine). The scene is
`testproject/WesternGame/Scenes/full_map.tbdscene`, the Port Lazare town. Your job is to find and fix
geometry, placement and texture defects across the whole town, using code to find what code can measure and
screenshots to find what only looks wrong. You work alone and in order; if your tool can run parallel
sub-agents or tasks, use them only as described in "Parallel work" at the end.

## Rules
- **Back up first**: copy the scene to `full_map.backup_before_cityfix_<n>.tbdscene` in the same folder before
  each batch of edits. Never edit a backup.
- Do not commit. Leave changes in the working tree and finish with a summary.
- Do not change renderer or shader code. This is a scene and asset job. If you believe a defect is a renderer
  bug (for example, z-fighting that survives correct geometry), write it into `docs/cityfix/renderer_bugs.md`
  and move on.
- Every sign, label and stencil in the world says **Port Lazare**, never New Orleans or any other real city.
- Do not delete an entity unless it is an exact duplicate or lies fully inside another solid. Prefer move, scale or
  rotate. Log every deletion.
- Change only what a defect needs. Do not redesign streets, buildings or the layout.
- Fix systemic causes first (a rule that clears many entities), then single cases.

## The scene format
`.tbdscene` is text. Header `TBDSCENE 1`, then lines like
`E|name|kind|x y z|qx qy qz qw|sx sy sz|r g b|path#native`. Names are `Group/asset`
(`Terrain/…`, `Buildings/…`, `Doors/…`, `Props/…`, `Rocks/…`, `Vegetation/…`, `Boats/…`). Units are metres,
Y is up. Rotation is a quaternion. The source meshes are `.glb` files under the paths in each line; read them
(for example with Python and `trimesh` or `pygltflib`) to get bounds, triangles and UVs.

## Live engine control (use it, do not guess)
The editor listens on the pipe `\\.\pipe\tbd_engine`. From a shell:
`python gameenginetools/engine_mcp/tbd_mcp.py <command> <args...>`. See `gameenginetools/engine_mcp/README.md`.
Useful commands: `info`, `list <filter> <max>`, `get <entity>`, `set <entity> pos|rot|scale|color <value>`,
`camera "tx ty tz" <distance> <yaw> <pitch>`, `time <hour>`, `screenshot <path.bmp> <frames>`, `save`.
Set the time to noon (`time 12`) for all inspection shots so shadows do not hide defects. Save screenshots to
`docs/cityfix/shots/`. Edits through the pipe change the live scene; `save` writes it. Prefer editing live and
checking the result with a screenshot, then `save`.

## Phase 1: automatic audit (write `gameenginetools/city_audit/audit.py`)
Write one script that reads the scene and the meshes and writes `docs/cityfix/tickets.json`
(a list of `{id, type, entities:[index or name], world_pos:[x,y,z], detail, suggested_fix}`) plus a readable
`docs/cityfix/report.md`. Re-runnable, deterministic, finishes in under a few minutes. Checks:

1. **Coplanar overlap** between ground surfaces (road, sidewalk, plaza, terrain): two entities whose world
   triangles overlap in XZ and whose heights differ by less than 2 cm. These cause z-fighting and the see-through
   diamond patches on sidewalk corners. Fix by trimming, lowering the lower one by 3 to 5 cm, or removing the
   hidden duplicate area.
2. **Ground gaps**: a grid of probes (1 m spacing) across the walkable area, cast down. Flag any probe that hits
   nothing or hits only the base terrain where a road or sidewalk edge should be (the sawtooth dirt notches
   along road and sidewalk seams).
3. **Texel density outliers**: for every mesh, compute (UV area / world area) per triangle group. Compare each to
   the median for the same material. Flag anything more than 3x off in either direction. This finds stretched or
   smeared road, pole and trim textures. Fix by adjusting scale only if the mesh is a plain box or plane;
   otherwise ticket it for UV re-projection.
4. **Doors not matching openings**: for each `Doors/…` entity, find the nearest wall opening or door frame in
   the building meshes. Flag a door whose bounds are more than 10 cm from the opening's centre, wider or
   taller than the opening by more than 5 cm, or standing in front of or behind the wall plane by more than
   5 cm. Fix by snapping position, yaw and scale to the opening.
5. **Floating or buried props**: for each prop, cast down from its base. Flag a gap above the surface larger
   than 3 cm, or more than 15 cm of the prop below the surface. Fix by moving it vertically.
6. **Intersections**: props that overlap other props or building geometry by more than 10 cm of their own
   bounds (crates through steps, poles through sidewalks, lamp posts through walls). Fix by moving the smaller
   object to the nearest free spot along the street side.
7. **Duplicates**: entities with the same asset within 5 cm and the same rotation.
8. **Pole and lamp clusters**: several identical poles within 1 m of each other. Keep one, or spread them to
   their intended spacing.

Print a count per type. Do not start Phase 3 until Phase 1 and 2 are done and the ticket list is written.

## Phase 2: visual survey
Using `camera`, `time 12` and `screenshot`, take a sweep of the town: at street level (1.7 m eye height)
along every street in both directions, at each intersection, at roof height (about 12 m, pitch 35 degrees)
over every block, and one close shot per building front. Name files
`<district>_<x>_<z>_<yaw>_<pitch>_<dist>.bmp` so the camera pose is in the name. Look at every shot and write
anything the audit cannot measure as a ticket with the exact camera pose: wrong or missing materials, texture
tiling that visibly repeats or shows dark blotches (the arcade wall bricks), props that look wrong in scale,
holes in walls, missing frames, thin black gaps. Add them to `tickets.json` with type `visual`. Note
each uncertain call as `confidence: low`.

## Phase 3: fix in batches
Work district by district (split the town into blocks of about 60 m by 60 m). For each batch:
1. Back up the scene.
2. Fix the tickets for that block: systemic fixes first, then single ones.
3. For every ticket, take a **before** and **after** screenshot from the same camera pose
   (`docs/cityfix/shots/<ticket id>_before.bmp`, `_after.bmp`).
4. Look at the after shot. Keep the fix only if the defect is gone and nothing new looks wrong. Otherwise undo
   it (restore the entity values) and mark the ticket `needs_human` with a reason.
5. `save`, then re-run `audit.py` and confirm the batch's tickets are gone and the total did not rise.

## Phase 4: finish
- Re-run the full audit and the full survey sweep. Compare ticket counts before and after, by type.
- Write `docs/cityfix/summary.md`: what was fixed, the counts, every deletion, every `needs_human`
  ticket with its camera pose and shot, and any renderer bugs found.
- Done when the audit reports zero tickets of types 1, 2, 4, 5, 6, 7 and 8, texel outliers are fixed or ticketed
  for UV work, and every `visual` ticket is fixed or in `needs_human`.

## What not to do
- Do not use or copy anything from outside the repo as a reference for appearance.
- Do not leave the scene saved in a half-edited state. If a step fails, restore from the batch backup.
- Do not report something as fixed without an after shot you looked at.

## Parallel work (only if your tool supports sub-agents or parallel tasks)
After Phase 1 and 2 are finished and `tickets.json` is written by one agent: split Phase 3 by block so that
no two agents touch the same entity, give each agent only its block's tickets, its own backup file and its own
shot folder, and have the main agent merge by re-running the audit and doing Phase 4 alone. If your tool
cannot do this, do the blocks one after another.
