# Handoff: rebuild the Clothes Maker as real cloth (written 2026-11-05)

The owner's verdict on the current shirt: it "sucks". They are right. It is not a garment. This document says why, what to
build instead, and how to know it is done. Work in `C:\custom windows apps\gameengine`. Nothing is committed. Read the tail of
`AIchat.txt` first and append to it (append only, `[YYYY-MM-DD HH:MM:SS] Claude OVOA (<model>): ...`).

## 1. The goal

The Character Creator's CLOTHES tab builds a **button-up collared work shirt** for the character from a recipe:
- faded blue pinstripe chambray, sleeves rolled to just below the elbow, top button undone
- tucked into the scene's jeans (`WesternGameIOS\export\clothing\cowboy_jeans\cowboy_jeans.glb`)
- worn snug: it hugs the body

It must read as **real fabric**: a garment with its own shape, sewn from panels, hanging off the body.

The owner said exactly what the wrinkles should be: they **come from tucking it in and pulling it tight vertically**.
- Spare width at the waist gathers into folds that run up from the waistband.
- Tension runs straight from the chest and shoulder blades down to the belt.
- The rolled sleeve bunches above the roll.
- **Nothing** follows the body's anatomy: no pecs, no nipples, no breasts ("remove the saggy tits"), no navel, no spine groove.

This shirt is the first garment. The same pipeline must later make other shirts, coats and trousers, so keep the pattern, sewing
and simulation steps general.

## 2. Why the current one is wrong (do not repeat this)

`src\character\clothes_maker.cpp` (~1700 lines) **remeshes the body skin**: it cuts the body's triangles, subdivides them, pushes
them outward along the body normal, then fakes folds with sine waves and noise. Every complaint traces back to that:

| Symptom the owner saw | Cause |
|---|---|
| Breasts and saggy pecs, shirt shrink-wrapped to the chest | The surface **is** the body surface, offset. Later patches (chest hull, taut column hull) only hid it. |
| Wrinkles look fake or invisible | Folds are sine/noise displacements with no excess fabric behind them. The 48-step PBD drape mostly smoothed them away. |
| Jagged sawtooth edges at the pockets, armpits and collar | The cut follows body triangles. The pockets are raised patches with stepped walls. |
| Stripes shimmer and ignore the cut | Box-projection UVs, not pattern UVs. Stripes do not follow the grain or change direction at the yoke and pockets. |
| Collar is a thin flap, no seams, no stitching | Nothing is modelled as cloth pieces. |
| 48k triangles and still looks low-detail | Body-mesh resolution with no baked detail. |

The parts worth keeping are covered in section 6.

## 3. How a 3D artist makes it (the target method)

This is the Marvelous Designer / CLO workflow, and the rebuild should copy it:
1. **Flat sewing pattern.** Front L/R, back, yoke, sleeves, collar stand and collar, cuffs, placket, pockets and flaps, each drawn
   in 2D from body measurements plus ease.
2. **Even triangles on each flat panel** (about 8–10 mm edges for the simulation).
3. **Arrange** the panels around the avatar, then **sew** the matching edges together.
4. **Simulate the cloth:** real stretch, shear and bend stiffness for cotton shirting, gravity, collision with the body and jeans,
   and self-collision. The folds appear by themselves.
5. **Style it by simulating with pins:** tuck the hem into the jeans and pull it down, push the sleeve up and roll the cuff, and
   fold the collar along its roll line.
6. **Game mesh:** keep or retopologise the simulated mesh at about 12–20k triangles, give it real thickness (about 1.5 mm, with
   visible rims at hems and edges), and bake the high-detail folds, seams and stitching into normal and AO maps.
7. **UVs are the flat pattern itself**, so stripes follow the grain of each panel. Textures add weave, pinstripe, stitching
   along seams, buttonholes, and wear driven by baked curvature and AO (collar edge, cuffs, placket).
8. **Skin to the body:** transfer weights from the nearest body surface, smooth them, keep 4 influences, and export with the
   body's own skeleton.

## 4. Decision: Blender's cloth simulator as the backend (v1)

Blender 5.2 is installed: `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`.

Its cloth modifier already does what's needed: sewing springs, pin groups, collisions, self-collision, and per-vertex-group
stiffness. It also exports skinned glTF. Writing a production cloth solver inside the engine first would cost weeks and probably
look worse.
- **Clean-room note:** Blender is an authoring tool here and does not ship with the game.
- **Later (optional):** an in-engine XPBD port of the same pattern and sewing data, once v1 looks right.

**Check the API first.** Blender 5.x has changed APIs before. Write a 20-line test: two sewn rectangles falling onto a cube,
`--background --factory-startup`, baked through the point cache. Confirm `bpy.ops.ptcache.bake_all` (or the 5.2 equivalent),
sewing springs (`cloth.settings.use_sewing_springs`), `vertex_group_mass` and `vertex_group_bending` behave as expected.

Build it as `gameenginetools\clothes_sim\`, offline Python run by Blender:
```
blender.exe -b --factory-startup --python gameenginetools\clothes_sim\build_garment.py -- ^
    --recipe <recipe.json> --body <body_torso.glb> --legs <body_legs.glb> --jeans <cowboy_jeans.glb> --out <dir>
```
- Print progress lines to stdout (`PROGRESS 0.35 draping`) so the engine can show status.
- Add `--selftest`, like the other tools in gameenginetools.

## 5. The pipeline in detail

### 5.1 Measure the body (Python, no Blender needed)
- **Input:** the skinned body GLB in its T-pose bind, in metres (`WesternGameIOS\export\body\body_torso.glb`; legs and head beside
  it).
- **Measure:** chest, waist and hip girth (horizontal slices); neck girth at the base; shoulder width (acromion to acromion);
  back length (neck base to waist); arm length (shoulder to wrist); bicep and forearm girth; waist height.
- **Joint positions:** take them from the inverse bind matrices. `LoadBody` in `clothes_maker.cpp` already does this.
- **Jeans:** take the waistband height and inner radius per angle from the jeans GLB. `LoadJeans` in `clothes_maker.cpp` builds a
  96-angle radius table by ray casting; port it.

### 5.2 Draft the pattern (2D polygons in metres)
A standard men's shirt block, from the measurements plus recipe ease:

| Piece | Notes |
|---|---|
| Fronts L/R | Button extension (~2 cm) for the placket. Armhole and neckline curves. Pocket placement marks. |
| Back | One piece, gathered or pleated under the yoke. |
| Yoke | Two layers; grain runs across. |
| Sleeves L/R | Cap curve matched to the armhole length (check that the lengths agree within 1 cm). |
| Cuffs and collar | Cuffs, collar stand, collar leaf (the leaf points spread per `collarSpread`). |
| Placket, pockets, flaps | Pocket shape matches the owner's reference: a pointed flap with a button. |

- **Ease, total over the girth:** chest about +8–10 cm (the owner wants it snug; make it a recipe value), waist about +6 cm. The
  spare width at the waist is what becomes the tuck folds. Do NOT remove it to make the shirt "tight". Snugness comes from the
  belt pulling it in.
- Each panel has **named edges**, and the seam list pairs them: side seams, shoulder/yoke, armholes, sleeve underarm, collar to
  neckline, cuffs. Assert that paired edges match in length (ease the sleeve cap within 1–1.5 cm).
- **Grain angle per panel:** vertical on the fronts, back and sleeves; across on the yoke; pockets straight (optionally bias).
- Write `pattern.json` and a `pattern.svg` preview to the output folder. Look at the SVG before simulating.

### 5.3 Triangulate the panels
- Isotropic triangles about 8–10 mm on each flat panel. Resample the boundaries so paired seam edges have the **same vertex
  count**; sewing needs vertex pairs.
- Generate them in Python: a jittered grid inside the polygon plus the resampled boundary, constrained-Delaunay or Blender
  `bmesh.ops.triangle_fill`, then beautify. No long thin triangles.
- **UV = the flat pattern coordinate,** rotated to the grain and packed into one atlas (shelf packing exists in
  `clothes_maker.cpp`). 1 m of fabric is about 1600–2000 px.

### 5.4 Arrange and sew (Blender)
- Import the body (and legs, head) as **collision** objects: outer thickness 2–3 mm, friction about 5–10. Import the jeans as a
  collision object too.
- Wrap each panel on a cylinder around its body part, about 4–8 cm out: fronts and back around the torso, sleeves around the
  arm axes (from the joints), collar around the neck.
- Join all panels into one mesh. Add loose edges between the paired seam vertices and turn on sewing springs (max sewing force
  tuned so seams close within ~20 frames).
- **Cloth settings, a starting point for cotton shirting (tune by looking):**
  - mass about 0.15 kg/m²
  - tension/compression 15–30; shear 5–10; bending 0.1–0.5
  - air damping 1; quality 10–15; collision quality 5
  - self-collision on, distance 2–3 mm
- **Stiffer areas with interfacing:** collar, collar stand, cuffs and placket get a bending vertex group with about 10× the
  bending stiffness.

### 5.5 Style by simulating (this is where the owner's wrinkles come from)
1. **Drape:** close the seams and let it settle on the T-pose body, untucked, about 60–120 frames.
2. **Tuck:**
   - Pin the bottom band (the hem up to `tuckDepth` below the waistband) to target positions **inside** the jeans waist
     radius, animated over about 40 frames. Then pull those targets **down 2–4 cm** (`tuckPull` in the recipe). That pull is
     the vertical tension.
   - Add a collision ring at the waistband (the belt) at the jeans' inner radius, so the extra width above it gathers into
     vertical folds instead of passing through.
   - Settle about 60 frames.
3. **Rolled sleeves:**
   - Model the roll as a sleeve band folded on itself, or as a separate 3-layer tube sewn to a shortened sleeve, with stiff
     bending.
   - Pin the sleeve end to a ring just below the elbow and simulate. The sleeve length above the pin bunches into the
     roll-up folds.
4. **Collar:** pre-fold the collar leaf along its roll line before simulating. Pin the stand to the neckline seam only.
   Unbutton per `openButtons` by leaving the top placket pair unsewn, so the front falls open into a V.
5. **Pose question:** the body is bound in a **T-pose** and the game plays clips (idle has the arms down). A shirt simulated in
   a T-pose will look stretched and smooth under the arms when posed.
   - Preferred: simulate in a **relaxed A-pose** (arms about 45° down). Get the A-pose by posing the body armature in Blender.
     Then bind the garment to the armature at that A-pose: the inverse bind matrices must come from the posed joints, and the
     glTF export handles that if the A-pose is applied as the rest pose.
   - Simplest acceptable v1: simulate in the T-pose, but with an extra underarm settle frame range while the arms rotate down
     to the A-pose, and accept the T-pose bind. Write down which you chose and why.

### 5.6 Game mesh
- **Triangle count:** decimate or retopologise to about 12–20k triangles. Keep seams and fold silhouettes; a 1 cm sim mesh
  might already be close.
- **Thickness:** solidify about 1.5 mm with rims, so hems, the collar edge, cuffs and the placket show thickness.
- **Bake** from the full sim mesh to the game mesh: tangent-space normal and AO. Use Cycles CPU **offline** (this is authoring,
  not the engine's real-time RT, which stays off per the owner's rules).
- **Add as real geometry or baked detail:**
  - topstitching 6 mm from seams (normal map lines)
  - buttonholes
  - buttons: separate small discs placed on the placket after the sim and skinned like the shirt; reuse the button code in
    `clothes_maker.cpp`

### 5.7 Textures (in pattern UV space)
- **Reuse the weave code:** chambray slub, twill normal and pinstripes from `Fabric()` / `TexCtx` in `clothes_maker.cpp`. Port
  it to Python/numpy, or keep it in C++ as a small exe. Stripes run along the grain, which is now correct per panel.
- **Wear masks** come from the baked curvature and AO, not world position: fade on collar edges, cuff folds and placket edges,
  light dust only.
  - The owner hated baked stains and blotches. No sweat patches, no large dark areas.
  - Colour defaults are in `src\character\clothes_maker.h` (`base`, `stripeA`, `stripeB`, spacing, width).

### 5.8 Skinning and export
- **Weights:** Data Transfer from the body (vertex groups, nearest face interpolated), then 2–3 smoothing passes. Limit to 4
  influences and normalise. Dense cloth rides one joint less badly when weights are smooth.
- **Skeleton:** export glTF with the **body's armature**: same joint names, same hierarchy, inverse bind matching the bind pose
  used. The engine binds clips by joint name (`skeletal_anim.h`), and every clothing GLB carries its own skin.
- Also export the **uncovered body skin** as an `M_Skin` part (forearms, hands, V of the neck), as the other clothing GLBs do.
  Otherwise the body under the shirt pokes through or the arms vanish. See how `BuildShirt` writes the skin part.
- **Output** to `Assets\clothes\<name>\`: `<name>.glb`, `albedo.png`, `normal.png`, `recipe.json`, `pattern.json`,
  `pattern.svg`.

### 5.9 Engine integration (CLOTHES tab)
- Keep the tab, its sliders and the recipe. `src\character\creator.cpp` has `StartClothBuild` (worker thread), `ClothPoll`
  (swaps or adds the "Shirt" entity, remembers the original path for Restore), `kClothSliders`, `RecipeDialog`, and the
  `TBD_CHARTAB` and `TBD_CLOTHBUILD` env hooks.
- Change `StartClothBuild` to launch Blender with the script above via `CreateProcess`: no console window, stdout piped, and
  `PROGRESS` lines parsed into `clothStatus`. When it ends, load the GLB as now.
  - Keep `Busy()` true while it runs; `main.cpp`'s capture wait already checks it.
  - Allow about 1–3 minutes per build.
  - Cache the drape stage keyed by pattern hash, so changing colours or wear does not re-simulate.
- **Recipe changes:**
  - Keep: name, sleeveLength, rolled, tucked, tuckDepth, openButtons, pockets, collarSpread, colours, wear, dirt, seed.
  - Replace the old fit fields (ease, thickness, drape, wrinkle, wrinkleScale, blouse) with pattern and fabric values:
    chestEase, waistEase, backLength, sleeveLength, rollWidth, tuckPull, fabricWeight, stretch, bend, collarStiffness.
  - `ShirtRecipe::Load` must ignore unknown or missing keys.
- The old C++ builder can stay behind a "Legacy" toggle or be deleted. Do not spend more time patching it.

## 6. Keep from the current code
- `LoadBody` (weld, joints from inverse bind) and `LoadJeans` (radius table, waistband line): measurements and the tuck target.
- The weave, pinstripe and normal texture code (`TexCtx`, `Fabric()`), the atlas packer, and the GLB writer (as a reference).
- The CLOTHES tab UI, worker-thread pattern, entity swap and Restore in `creator.cpp`.
- `Busy()` in `creator.h` and the capture wait in `main.cpp`.

## 7. Related engine changes from this session (uncommitted; do not undo)
- **`src\skeletal_anim.h`**
  - Dual-quaternion skinning, with linear blending for joints with scale. `TBD_SKIN_LBS=1` forces linear.
  - Clips made on another skeleton (`WesternGameIOS\export\anim\unreal\*.glb`) store that skeleton's bone offsets as constant
    translation keys. Played as is, they put the elbow and wrist about 12 cm off this body's bones (the broken swan-neck wrists
    the owner saw).
  - Below the pelvis, a translation track that never moves now plays the mesh's own offset (`AnimTrack::still`, set in
    `mesh_import.h`). The parent bone is turned so the child still sits where the clip put it (`AimBones`), so the pose angles
    are unchanged.
  - Clothing simulated or posed in Blender must use **this body's** bone lengths, not the clip's.
- **`src\mesh_import.h`:** `SoftenWrists()` softens hard forearm-to-hand weights at load. It skips files marked
  `asset.extras.tbd_wrist_soften`. A garment made with proper weight transfer won't need it, and it does no harm.
- **`gameenginetools\body_weights\soften_wrists.py`:** the same fix offline. Body and clothing GLBs in `WesternGameIOS\export` were
  rewritten with backups `*.before_wrist_soften.glb`.
- **`src\character\clothes_maker.cpp` (last patches):** chest bridge, taut columns, tighter fit, tuck pull lines.
  - The last edit (leaning, wider pull lines) is in the source but its build/capture was cut off, so it is unverified.
  - It's all superseded by this rebuild.

## 8. How to build and look (do this every iteration and LOOK at the images)
- **Build:** `build_clothes.bat` builds the side engine into `bin_clothes\` (D3D12 + Vulkan + SPIR-V).
  - **Never** build into or run `bin\`: the owner's editor runs from there.
  - Other Claudes edit `main.cpp`, `build.bat`, `renderer*`, `mesh_import.h` and `src\script\*`. Re-read before editing and
    keep hooks small.
- **Capture:** `gameenginetools\clothes_sim\shot.ps1`. It launches `bin_clothes` on `testproject\Western\Scenes\wyatt_lookdev.tbdscene`
  with the CLOTHES tab open, runs `TBD_CLOTHBUILD=1`, and writes `out\<Name>.png`.
  - The script lists the camera presets: front, back, three-quarter, side, chest, waist, arm.
  - Use `-Api vulkan` for the second backend.
- The scene plays `wyatt_idle.glb` (arms down), so captures show the shirt **posed**, not in its bind pose.
- If the GPU is lost (it has happened on this RTX 4060 laptop): `pnputil` restart plus a device scan brings it back without a
  reboot (see AIchat.txt).

## 9. Done means (all seen in captures, on D3D12 AND Vulkan)
1. **Front, side, three-quarter:**
   - The chest shows no anatomy (no pec mounds, nipples or underbust crease).
   - The fabric bridges the sternum and runs taut from the chest to the belt.
   - The silhouette is a shirt's, not a body's.
2. **Waist:**
   - Tucked into the jeans, with folds that **rise from the waistband**.
   - Each fold has a different length, leaning toward the fly at the sides.
   - At most about 1 cm of blouse over the band.
3. **Back:**
   - Yoke seam visible.
   - Folds gather from the shoulder blades down into the tuck.
   - No stains, blotches or dark patches.
4. **Sleeves:**
   - Rolled cuff with real thickness just below the elbow, cloth bunched above it.
   - Snug on the forearm; no funnel, no gap showing the inside.
   - Moves with the idle clip without the arm poking through.
5. **Collar:**
   - Stand and leaf with a proper roll and points, the top button open into a V.
   - Placket with buttons and buttonholes; pockets with pointed, buttoned flaps.
   - Seams and topstitching visible up close.
6. **Clean geometry and texture:**
   - No jagged or stair-stepped edges anywhere.
   - Pinstripes follow each panel's grain, change direction at the yoke and pockets, and don't shimmer.
7. **Budget and speed:**
   - 12–20k triangles for the shirt plus buttons.
   - Builds from the tab in under about 3 minutes; re-colouring alone in seconds.
8. Compare side by side with the owner's references (below). Report honestly what still differs.

**References:** the owner showed images of a blue pinstripe work shirt with rolled sleeves, tucked in, plus a short video.
They were pasted in the chat and are **not on disk**: ask the owner to drop them into `docs\refs\clothes\` if you need them.
They are look references only. Per the clean-room rule (`docs\hair_card_spec.md`), never import, trace or copy meshes,
textures or UVs from any game, and never feed game assets to AI tools.

## 10. Owner preferences (from this and earlier sessions)
- Results visible **in the engine**, checked by looking at captures before reporting. No reports instead of results, and no
  "acceptable?" without having looked.
- Be honest about what is unverified.
- Every renderer feature on both D3D12 and Vulkan. No ray tracing in normal runs.
- In-world text says Port Lazare, never a real city.
- `C:\custom windows apps\WesternGameIOS` has no git. Never overwrite or delete there without a backup next to the file.
- Commit only when the owner asks.
