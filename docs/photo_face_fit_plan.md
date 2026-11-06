# Photo-to-face: pin fitting and skin colour from photos (plan)

Goal: load photos of a person, drop the same few pins on the base head and on each photo, and get (1) a head shaped like them and (2) skin colouring read from the photos, with no lighting baked in.
Every renderer change lands on both D3D12 and Vulkan. Reference screenshot: a wireframe grid laid over the photo, red pins on the mesh, blue-cyan dots where they should go, "Pin Mode" label.

## What already exists (use it, don't rebuild)

| Piece | Where | Use |
|---|---|---|
| 468-point face tracker, lifted onto the mesh | `src/mocap/face_fit.*`, `face_tracker.*` | Auto-suggest pin positions on a photo; the user only corrects them |
| `landmarks` block in `morphs.json` | `character/creator.cpp` | Named base-head landmarks already stored in mesh space |
| Software head rasteriser (`RenderHeadView/Crop`) | `face_fit.cpp` | Draws the wireframe overlay for the 2D compare view without GPU work |
| Skin paint layers (tint, spots, shine) over the head atlas | `character/skin_paint.h` | Output target for the photo colour pass |
| Landmark-zone colour bake plan (melanin, blood, beard) | `docs/skin_realism_plan.md` §1 | The photo pass supplies its parameters |
| Hair/brow/lash cards fitted to landmarks | `hair_paint.h`, `brow_cards.py` | Follow the new shape through the existing landmark block |

## 1. Pin set (the "super easy to mark accurately" part)

A small fixed set, each pin on a feature that is a corner or a point, not a smooth area. About 24 pins, ordered by how sure you can be of them:

- Eyes (per side): inner corner, outer corner, pupil centre, upper-lid mid, lower-lid mid.
- Brows (per side): inner end, arch peak, outer end.
- Nose: tip, left and right alar edge, base of septum (subnasale), bridge at the eye line (nasion).
- Mouth: left and right corner, upper-lip peak (Cupid's bow centre), lower-lip centre, upper lip's vermilion border mid.
- Chin bottom (menton), jaw angle left and right, ear top and ear lobe bottom per side, hairline centre.

Rules to keep marking accurate:
- Each pin has a name, a one-line hint, and a small zoom inset (the loupe) while dragging; arrow keys nudge one pixel.
- Pins on the base head are *pre-placed* (stored in `morphs.json`, never marked by hand) and only move if the user wants to refine.
- Per-photo, a pin can be marked **hidden** (not visible from that angle) instead of guessed. The fit then ignores it.
- Auto-suggest button: run the existing tracker on the photo and snap all pins; the user fixes outliers. Works for front and 3/4. Profile views are marked by hand (the tracker fails past ~60° yaw).
- Mirror and symmetry toggle for the front view only (never silently mirror a real asymmetry).

## 2. Multi-photo workflow and the angle selector

- Photo list on the left: each photo has a label (front, left 3/4, right 3/4, left profile, right profile, other) and a yaw estimate.
- Click a photo to make it the active view. The 3D head turns to that photo's solved camera so overlay and photo match, and the pins on the head are the same ones, shown for that angle.
- Camera per photo is solved, not chosen: take the pins marked on that photo (≥6 visible), solve the pose and focal length from the base head's 3D pins (PnP, then refine focal length). Show reprojection error per pin as a coloured halo so bad marks stand out.
- Controls: opacity slider for photo vs mesh, side-by-side vs blend, wireframe grid overlay (quad grid in screen space from UVs, like the reference), toggle each region's pins, lock a photo.

## 3. Pin Mode tools (look like the reference)

- **Pin Mode** toggle: red dot = pin on the mesh at its current place, cyan dot = target on the photo, a thin line between. Drag the cyan dot; the mesh follows live.
- **Region brush** (soft falloff) to say "how far this pin's pull spreads", with presets per region (eye, mouth, nose, jaw). Default falloff comes from geodesic distance on the mesh so the nose pulls the nose, not the cheek.
- **Fit** button runs the solve (below); **Reset region**, **Undo/Redo** per pin move, **Lock** a region once happy.
- **Grid overlay**: UV iso-lines drawn over the photo, like the reference screenshot, so you can see where the surface sits.
- **Measure**: show interocular distance, nose width, mouth width as numbers and as a ruler on the photo, to catch scale errors.
- **Sliders under the pins**: after a fit, the fitted morph weights show as sliders so the user can tweak the result in Character Creator's normal morphs.

## 4. Shape solve (pins → head)

Two stages, the first is mandatory:

1. **Morph-target fit.** The head already has morph weights. Solve for the weights that minimise the reprojection error of all marked pins over all photos, with a prior (weights near zero, bounded), a symmetry soft-term, and the camera refined together (alternate: camera, then weights, ~5 rounds). Small linear least-squares per round, so it runs interactively.
2. **Residual warp (optional, later).** After the morphs saturate, fit the remaining pin offsets with a smooth RBF/thin-plate warp, strength-capped so the topology stays clean (eyelids, lips, ears keep their edge flow). Output as a new "personal" morph target.

Depth is the weak point: one front photo gives no nose length or brow depth. Rules: front photo fixes width and height; each side/3-4 photo adds depth; with only a front photo the depth morphs stay at the prior (and the UI says so).

## 5. Skin colour from photos (no baked lighting)

The photo is **not** pasted on. It is read for colour; the texture is generated by the zone bake in `skin_realism_plan.md` §1, so pores, normals and spec stay ours.

Pipeline:

1. **Sample.** With the camera solved, project each photo onto the head UV atlas (per-texel visibility, normal·view weight, discard texels past ~70° from the view and any texel inside hair/brow/lash/eye/lip masks). That gives a *raw projected albedo* per photo.
2. **De-light.** Remove illumination before reading colour:
   - Estimate lighting as low-order spherical harmonics (order 2) fitted to the luminance of skin texels with the mesh normals; divide it out.
   - Remove specular: highlight texels (high luminance, low saturation) are masked out and inpainted from neighbours.
   - White balance: sclera and teeth are a neutral reference; a "pick neutral" tool lets the user click something neutral in the photo as a fallback. A grey-card tool is the accurate one when the photo has one.
   - Everything runs in linear space; photos are sRGB, so convert first.
3. **Blend photos** with weights = visibility × cosine, feathered over seams (Laplacian pyramid or Poisson) so the left/right/front joins don't show.
4. **Read the colour, not the pixels.** For each landmark zone in the colour-bake plan (forehead, cheek, nose, under-eye, chin, jaw, neck, lips) take a robust median of the de-lit texels. Fit the melanin and haemoglobin amounts (Jimenez model) per zone so that the bake reproduces those medians. This gives the person's real skin tone, their redness at the nose and cheeks, under-eye shadow, and stubble shadow.
5. **Optional detail layer.** Low-frequency residual (blur radius ≈ 6 mm) between the bake and the de-lit photo is added back at small strength, to bring in real moles, birthmarks and blotches the bake wouldn't invent. A mask-based "Keep marks" brush picks which ones; spots listed explicitly go to the existing spots layer as moles (position + size).
6. **Outputs** into the existing skin paint layers: `melanin.png`, `blood.png`, `beard_shadow.png`, a tint layer for under-eye and flush, a spots list, and the final albedo. Everything stays editable by hand afterwards.

Quality guard: show the resulting albedo under flat light and a coloured swatch table next to the photo's swatches (ΔE per zone), so a wrong tone is visible as a number.

## 6. Architecture

- `src/character/photo_fit.{h,cpp}` — pin sets, photo list, camera solve, morph solve. No GPU or window code; unit-testable.
- `src/character/photo_skin.{h,cpp}` — projection, de-light, blend, zone stats. CPU, on the job system (`core/jobs.h`); a 4096² atlas projection is a few seconds, so it runs async with a progress bar.
- `src/character/photo_fit_ui.cpp` — the Pin Mode window and photo list; sits as a new tab in the Character Creator HEAD section.
- `gameenginetools/skin_tex/skin_colour_bake.py` stays the single colour bake; photo stats are passed to it as a JSON file (`skin_zones.json`), so command-line and UI use the same path.
- Data on disk, next to the character: `photo_fit.json` (pins per photo, camera, solved weights), photos copied under the character's folder. No photos go into the repo or builds (same rule as the RDR2 clean-room: the user's photos are personal data).
- Face recognition/identity: none; the tracker only finds landmarks. Photos stay local.

## 7. Phases

| Phase | Deliverable | Check |
|---|---|---|
| A | Pin set on the base head in `morphs.json`; pin data model; 2D view with one photo, draggable pins, loupe | Marking 24 pins on one photo in under 2 min |
| B | Camera solve from pins; head turns to photo; overlay + opacity; reprojection error halos | Error under 2 px on a good front photo |
| C | Morph solve, live in Pin Mode; undo; region falloff; symmetry | Visibly closer face on the front view within a few drags |
| D | Multi-photo: list, angle labels, depth from side views, auto-suggest from tracker | Front + 3/4 + profile give a correct nose profile |
| E | Photo projection, de-light, seam blend, zone stats → `skin_zones.json` | Colour table matches photo within ΔE ~6 |
| F | Colour bake driven by zones; detail layer; moles; flat-light preview | Side by side against the photo under neutral light |
| G | Residual warp, polish (ear shape, jaw), D3D12/Vulkan parity check of any new viewport draw | Parity screenshots |

## 8. Decisions (settled with you, 2026-11-05)

1. **Photos:** usually several angles per person. Multi-photo is the main path, not an extra.
2. **Bad lighting:** the tool warns and carries on. It never refuses.
3. **Texture modes:** "read colours, generate texture" (default) and a **Project photo** option (direct projection of the de-lit photo onto the face).
4. **Pins:** only super-identifiable markers, as many as are well defined (corners and points, no smooth-area guesses). Any subset can be marked; the fit uses whatever is given, and the UI says what is still unconstrained.
5. **Marking flow:** click a pin in the list, then click the spot on the photo. Re-click to move. No separate mesh-side dragging needed to start.
6. **Two views:** **Overlay** (mesh wireframe grid drawn over the photo, as in the reference) and **Side by side** (photo next to the rendered 3D head). A toggle switches between them.
7. **Where it lives:** its own tab ("PHOTO FIT") in the Character Creator tab row, beside HEAD, working on the head.
8. Base head mesh is never changed; only morph weights move.
