---
name: citygame3dmodel
description: Model any asset for the cityengine game (building, vehicle, facility, infrastructure) from a one-line name like "bus depot". Applies the user's low-poly, SPOM-detail, CS2-fidelity rules and the repo's generator pipeline.
argument-hint: <asset name, e.g. "bus depot">
---

# /citygame3dmodel `<asset>`

The user will pass just a name ("bus depot", "fire station", "oil refinery"). Do not ask for a spec. Research, decide, build, show. Ask only if the name is truly ambiguous in CS2.

Full preferences: `docs/modeling_preferences_and_requirements.md`. Read it if anything below is unclear.

## 1. Research first (fast)
- Find the CS2 equivalent (cs2.paradoxwikis.com via WebFetch/WebSearch). Get: the building/vehicle types, plot sizes (NxM grid), levels (L1..), and what it connects to.
- Check what already exists: `assets/**/glb/`, `tools/building_gen/`, `tools/vehicle_gen/`, `asset_inventory.html`. Don't duplicate; reuse the track, train, road and vehicle families.
- Tell the user the list in 5-10 lines (type, size, level) and the tri budget, then build it. Do not wait for approval unless the list is large (more than ~8 models) or ambiguous. If CS2 has fewer than you'd guess, model only CS2's set.

## 2. Hard rules (the user has rejected work for breaking these)
- **Ultra low poly, high detail.** Detail comes from textures + normal maps + silhouette POM (SPOM), not geometry. Follow section 2a (SPOM-first ladder): slabs for walls/roofs, SPOM volumes for protrusions, 2-tri cards for thin/tall/repeated things. Tall thin things become one baked quad (2 polys). Linear features (track, road markings, fences) are a texture/spline, not meshes.
- **Calibrate against existing models.** Open 2-3 comparable GLBs, report their tri counts, and stay at or below them. Use the section 2a targets, which are tighter than older models; aim below the existing ones. If the user says "too many polys", cut harder and move the detail into textures.
- **Realistic, not cartoonish.** Real-world (American where relevant) proportions and references. Each variant unique: different massing, windows, roof, colour. No identical L1s.
- **Glass** gets reflective/rough textures. Ground floors get real lobbies and bigger windows. No flat blank bases.
- **Lot discipline.** Footprint fits its NxM plot exactly; row/attached buildings abut with no gaps. Leave sensible room for the facility's function (big for sorting centres, plants, schools).
- **Never bake in:** trees, cars, planes, trains, outer roads, sidewalks, helicopters, spinning turbines, lamp posts and fine props. Internal facility roads and a small front parking lot are fine when the building needs them.
- **Signs/text:** leave blank panels so any lettering or texture can be applied.
- **Textures:** procedural (PIL) in the generator, or Gemini via the Chrome extension (`/chatgpt-app` style flow with Claude in Chrome) for textures ONLY. Never use Gemini to model. Keep tries minimal.
- **Performance target:** iPhone 15, 25 fps, upscaled to 720p. Few materials per model (share a material/atlas where possible), few draw calls, no per-pixel heavy loops, bake groups to cut draws.
- **Animation (only when asked or standard for the class):** low-density homes get the pitched-roof construction animation, everything else the crane/scaffold build animation, contained in the lot. Planes animate only gear plus the flight path.

## 2a. Geometry doctrine: SPOM-first, triangles last (read before modeling)
The goal is the lowest triangle count that still reads right in silhouette. Every feature must justify itself against "can SPOM draw this for 0-2 polys?"

**Ladder, try in order, stop at the first that works:**
1. **Relief slab (best): one wall or roof face = one thin box, ~10 tris per face, whole facade in a relief texture.** Brick, pilasters, window reveals, doors, bay openings, cornices, parapets, skylights, vents, rails, ribbing and track portals are ray-marched silhouettes with self-shadowing. Use `SlabPS` / `Mesher::Slab`, `SlabBlock` (`src/shaders/mesh.hlsl`, `kSlabDepth` 0.8 m), textures from `tools/building_gen/depot_tex.py` (RGB albedo, A height, AO baked in). Proof: the Railway Depot went from 2288 to 326 tris this way. Tile a texture per repeat period instead of modelling bays.
2. **SPOM volume (2 tris/face, 5-face box):** things that stick out of a wall: balconies, canopies, awnings, porch rails, stair rails, loading docks, pipes, ladders, cable trays, signage frames. Use `tools/building_gen/spom.py` (`Layers.box/band`, two-layer relief, `kSpomDepth` 1.2 m). One quad can carry a whole balcony row or a full fence.
3. **Baked card, 2 tris:** thin tall or repeated things (poles, pylons, smoke stacks, chimneys, cranes' lattice, antennas, fences, tree-like silhouettes, rows of containers, tanks seen as cylinders at distance) become one textured quad (or two crossed). Rows of identical things = one quad with a repeated relief.
4. **Splines + texture:** track, roads, markings, curbs, platforms edges, pipes along the ground: a flat strip with SPOM, never extruded meshes.
5. **Real triangles, only for the primary silhouette:** the massing box, roof pitch, big setbacks, and anything a top-down player sees against the sky or ground that a relief can't fake at grazing angles. Keep to boxes, wedges and low-sided prisms. No bevels, no chamfers (bake them), no cylinders above 8 sides, no interior geometry, no hidden faces, no bottom faces.

**Geometry hygiene (always):**
- Delete every face the camera can never see: bottoms, back faces against neighbours, faces inside other solids, interior rooms. Merge coplanar quads. Weld vertices.
- One material per model where possible, two at most (opaque + glass). Merge meshes into one draw. Share textures/atlas across variants; vary with tint in vertex colour, not new textures.
- Repeating structure (bays, floors, windows, cars of a train, containers) is a tiled relief period, never copy-pasted geometry.
- Windows, doors, vents, grilles, signs, panel lines, rivets, roof tiles, shingles, siding, bricks: **never geometry**, always relief/normal.
- Decide the per-model tri budget up front from the comparable models, list it, and report the final count against it with a per-part breakdown (massing vs slab vs SPOM volume vs cards).
- If a model is over budget, convert the most expensive geometry up the ladder before cutting visual detail. If it still exceeds, cut detail from the least visible side first.
- Cargo boxes 12 tris; grouped cargo baked into a few meshes; repeated vehicles instanced.

**Per-asset targets (starting points, beat them if you can):** small facility 150-400 tris, large facility 300-800, house 100-300, apartment/office 200-600, vehicle 150-800 body, train car 100-300, ship hull under 500, station under 600.

## 3. Build
1. Copy the closest existing generator in `tools/building_gen/` (buildings) or `tools/vehicle_gen/` (vehicles, rail, stations) as `gen_<asset>.py`. Match its structure: procedural textures, `Mesh` helper, GLB output.
2. Output GLBs to `assets/<category>/<asset>/glb/` using the existing naming (`<PREFIX>_<N>x<M>_<name>_L<k>.glb`).
3. Run it with `python`. Keep bash/PowerShell calls short. Fix errors rather than rewriting.
4. Update `asset_inventory.html` / checklists only if the repo already lists the category.

## 4. Show, don't tell
- Make a preview HTML in the style of the existing ones (e.g. `rail_depot.html`, `makehuman_lod_preview.html`): a grid with **poly count next to each name**, orbit view, and a mesh/wireframe toggle. Open it in the built-in browser and screenshot.
- Report per model: name, plot size, tris, material/draw count.
- Self-check before reporting: no forbidden props, footprint fits, tris at or under comparable models, textures not blank or stretched, no clipping or floating geometry.

## 5. Iterate on feedback
The user is blunt. Fix exactly what they complained about (too many polys, too boring, too cartoony, wrong size); don't rework unrelated parts. "Too many tries" means get it right in fewer passes next time. Commit only when asked.
