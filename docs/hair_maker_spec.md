# Hair Maker: design spec

Clean room: follows `docs/hair_card_spec.md` (no study assets). Extends `gameenginetools/hair_groom/groom.py` (arrow locks + bake) into an interactive sculpt-and-paint tool. Every renderer feature below must exist on D3D12 and Vulkan.

## 1. Pipeline

1. **Clay volume**: rough hair shape, sculpted like clay.
2. **Scalp field maps**: painted regions carrying Combing and Length data.
3. **Guide strands**: sparse curves rooted on the scalp, shown as an xray overlay.
4. **Interpolated strands**: full strand set between guides.
5. **Cards**: generated from strands using the atlas (cards-per-strand configurable).
6. **Cap mesh**: the shrunk clay volume under the cards.
7. **Hairline shell**: fur shell pass, hairline band only.
8. **LODs and export**.

Live preview (guides, strands or cards) can be toggled on or off while working. Updates are incremental per dirty region.

## 2. Clay sculpt

- Dynamic mesh or voxel clay, base from a rough hair shape on the head.
- Tools: grab, push/pull, smooth, inflate, flatten, pinch, crease, mask, mirror symmetry (X), remesh, undo/redo.
- Paint data (combing, length, density, ramps) is stored **per vertex or voxel on the clay surface and the scalp**, never baked in a texture, so it survives sculpting and remeshing (reproject on remesh).
- Gravity/relax pass: lets long hair fall with head and shoulder collision, so clay and final hair agree.

## 3. Paint modes (toggle)

Two modes with separate colour-coded region layers.

- **Combing mode**: paint regions in different colours. Per region, draw direction lines (stroke = direction). Direction is stored as a smooth field on the scalp.
- **Length mode**: paint regions in different colours. Per region: length, lift/stick-out angle, and "straight out" option.
- **Same colour regions switch**: when on, length regions reuse the combing region colours and shapes. Off: the two layers are independent (red in combing means nothing in length).

### Fades (for every channel)

Applies to length, combing direction, curl, density and thickness.

- Pick a **start point** and an **end point**: start is a hard 1, end is a hard 0.
- Both can be moved and re-strengthened afterwards.
- The 0 point maps to any value (for example 0.5x or 2x length, or a direction angle).
- Distance is measured **along the scalp surface (geodesic)**, not straight line.
- Per-ramp ease curve (linear, smooth, custom).
- Direction fades blend the field across region borders, so combing does not snap where two regions meet. Smooth and keep it close to divergence free.

## 4. Scalp structure

- Part line, whorl/cowlick and crown whorl are first-class: they seed the direction field.
- Density map; hairline painting; baby hairs, sideburns, nape.
- Hairline edge uses soft-alpha cards so the handoff to the shell has no seam.
- **Shared scalp mesh and UV with Character Creator.** Hairline paint lives in the skin pipeline. Hair conforms when face/head morphs change (re-bind roots like the existing HairBind).

## 5. Strand character

- Guides interpolate to full strands (guide hair + interpolation).
- Controls: cards per strand, strand count, clump, frizz, wave, curl, flyaways, random length/width per card, seeded and repeatable.
- **Scissor / cut tool** and undercut mask.
- Layers: inner/outer card layers with draw-sort order.

## 6. Card generation

- Cards come from the atlas (archetype matched by curvature, as in groom.py).
- Atlas maps: alpha, flow/tangent, per-strand ID, depth, AO (darker roots). Mip-safe alpha.
- Colour: paint per-region colour, or set all one colour. Root-to-tip gradient, highlights, grey strands, per-card variation.
- **Cap mesh** (owner's rule, built in `gameenginetools/hair_groom/cap.py`): the **exact clay mesh**, decimated optimally (quadric error edge collapse with the hairline boundary held, no flips, link condition kept), then **shrunk along its own normals** to sit 1.5 mm under the innermost card layer (never closer than 0.6 mm to the skin). It gives the illusion of full, thick hair. Its open edge is the hairline: the cap meets the scalp there (height tapers to the skin, coverage fades to 0). It is shaded as hair in the scalp colour, with procedural strands along a flow parametrization solved from the cards. It is drawn at every distance and is the last LOD. Until the clay tool exists, the clay is the hair volume lifted from the installed cards; `--clay sculpt.obj` takes the real one.
- **Cards meet the cap** (owner's rule, `cap.py` conform): cards rooted on the hairline go into the scalp. Cards further in hug the cap: the innermost layer's roots sit 1.2 mm above it, and outer layers keep 0.8 of their stacking above that. They are tight enough to hide the cap and loose enough to read as depth. groom.py's placement is kept as `pos_groom`, so the step can be re-run.
- **Fine hairline** (`cap.py`): short soft-patch cards and a few flyaways along the cap's edge, rooted in the skin and combed with the flow, so the hairline frays instead of ending in card edges.

## 7. Hairline shell

- Fur shell **only on the painted hairline outer band** (hairline paint defines it).
- Higher quality than the full-head shells that were removed, because the area is small. Direction comes from the same combing field.
- **Camera-distance gated**: the shell pass only renders when the camera is close enough (start distance and fade-out distance are settings, with hysteresis so it does not pop). Beyond that distance it is skipped entirely, with no cost. Shell density follows the density map.
- Decision: shell hair is added back, for the hairline band only (overrides the removal noted in `docs/hair_card_spec.md` section 8). It needs `UpdateTexture`/density+flow strand pass from item 7 in `docs/character_renderer_requests.md`.

## 8. Rendering

- Anisotropic hair specular (flow map as tangent), translucency, root darkening.
- Self-shadowing (deep opacity or equivalent), alpha-to-coverage, TAA-friendly alpha, no sorting flicker.
- Both backends, same look.

## 9. Runtime

- Cards bound to bone chains with skin weights from root distance; collision capsules (head, neck, shoulders); wind.
- LOD ladder: full cards, fewer/wider cards, cap only. Shell is camera-distance gated (off when far).
- Hat/helmet hide or compress mask.
- Live budget meter: triangles, cards, overdraw, per LOD.

## 10. Tool basics

- Save/load groom, presets, reusable styles that refit to another head via scalp UV.
- Reuse for eyebrows, beards, lashes.
- Export: `scalp_cards.json` format plus sidecar (card grouping, atlas cell, layer, per-vertex phase), cap mesh, hairline paint, shell data.
- Per-card error and bake decisions stay as in groom.py (one global triangle budget).

## 11. Build order (suggested)

1. Scalp field data model (regions, direction field, length, ramps with geodesic distance).
2. Guide strand generation + xray overlay.
3. Interpolation + strand character controls.
4. Card generation from strands (reuse groom.py bake).
5. Clay sculpt + cap mesh.
6. Colour, LODs, physics binding.
7. Hairline shell (needs renderer work on both backends).
