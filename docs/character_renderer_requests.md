# Character creator: renderer requests (for the rendering Claude)

From the character-creator Claude (Sonnet 5.5) and four Opus helper agents. Nothing here has been run in the engine. Everything is offline tooling plus data files; no `src\` file was edited. Every item must land on BOTH Direct3D 12 and Vulkan (project rule). Per-draw values go in a per-draw cbuffer/UBO: Vulkan push constants are already 144 bytes.

Details and exact shader text live in each tool's folder; this file is the index.

## 1. Skin pore tile scale (small)
- `mesh.hlsl` ~line 547 samples the skin normal tile at `uv * 48.0`, hardcoded. Face island is about a quarter of the atlas, so face pores come out about 2x too big.
- Ask: make 48.0 a per-material value (default 48 so nothing changes). Optional: a second detail-normal that tiles at its own scale and blends over the baked atlas normal.
- Tile and derived normal: `testproject\WesternGame\Assets\characters\skin_tiles\skin_base_01.png` and `skin_base_01_n.png` (tools: `gameenginetools\skin_tex\skin_tile.py`).

## 2. Zoned expression wrinkles
- Today: one weight per vertex (vertex alpha 0.5 to 0.59, `SkinWrinkle`). All wrinkles fade together.
- Tool and spec: `gameenginetools\wrinkle_zones\` (`wrinkle_zones.json` has zones, rig channel drivers, gains, age curve).
- Ask: four linear RGBA8 textures (ideally one 4-slice `Texture2DArray`; mips average slopes), 16 floats of zone weights per skin draw (`float4 wC[2]`, `float4 wS[2]`), added as weighted slopes on top of the pore normal using the existing tangent frame. Keep the old single-groove path as a fallback when the textures are not bound. Vertex alpha stays the skin flag.

## 3. Lips
- Tool: `gameenginetools\lips\` (`lips_table.json` key `renderer_request` has the exact shader text).
- Ask: textures `gLipMap` (`out\lips_pack.png`, RGBA16 UNORM, linear), `gLipMaterial` (`out\lips_material.png`, RGBA8; could be a 5th slice of the wrinkle array), `gLipTile` (`testproject\WesternGame\Assets\characters\lip_tiles\lip_base_01_n.png`). Where the lip mask is > 0, replace the uv*48 pore normal with the crease tile sampled at the lip coordinates (cotangent frame from ddx/ddy of those coordinates); roughness and a thin-tissue subsurface wrap from `gLipMaterial`; per-draw `gLipCreaseW`, `gLipPerioralW`, float3 `gLipBlood`.
- Mesh note (not a renderer item): 2 extra edge loops in the mouth bag, 0 to 8 mm behind the lip seam, so the roll does not facet.

## 4. Teeth, gums, tongue, mouth
- Tool: `gameenginetools\mouth_gen\` (see `TEXTURE_NEEDS.md`). The shader already has `ToothPS` (alpha 0.995) and `MouthWetPS` (alpha 0.985) for FaceRig's runtime mouth.
- Ask: `mesh_import.h` maps `M_Teeth` to code 0.995 and `M_Gums`, `M_Tongue`, `M_MouthCavity` to 0.985; the tooth shader reads (edge-to-neck, per-tooth seed) from UV set 1 but the tool writes them in UV set 2; see-through enamel edge; per-material roughness and a sharp wet-gloss highlight; texture slots; the face rig should drive the new `jaw` joint (index 53, hinge about (0, 0.781, 0.076)) and skip its built-in mouth when a character has these parts.

## 5. Hair cards (for the importer side, not shader)
- Tool: `gameenginetools\hair_groom\`. The existing `scalp_cards.json` loads as-is. Per-card grouping, atlas cell, bake option, layer and per-vertex phase are in the sidecar `scalp_cards_meta.json`, and three detail levels are in `scalp_cards_lod0/1/2.json`. Engine needs to read the sidecar and pick a LOD by distance.

## 6. Face maker morphs (CPU path, no shader)
- Tool: `gameenginetools\face_maker\` (open `index.html` through a local server). Deltas are per-vertex, added to the rest vertices before `FaceRig::Build`. Hair cards must be re-bound after forehead and ear sliders change. Same CPU upload path on both backends.

Reply in `AIchat.txt` with which items you will take and in what order. Items 1 and 4 are the smallest.

## 7. Short hair, eyebrows and stubble drawn from painted maps (new, owner request)
- The Character Creator Hair tab paints a DENSITY map and a FLOW (direction) map over the head UVs. The creator bakes strands into a copy of the head albedo as a stopgap (flat, no depth).
- Ask: a strand pass that draws real short hair from those two maps: sample `testproject\WesternGame\Assets\characters\hair_tiles\short_strands_01.png` (white strands on black, vertical) with UVs rotated per cell by the flow angle, masked by density, tinted by the hair colour with root-dark to tip-light variation, with a few shell layers or decal strand layers that fade out with distance (close-up only). Needs two extra textures per head, a hair colour constant, and must exist on both Direct3D 12 and Vulkan.
- Shell hair was removed from the tree; this would be a new, smaller pass, not a revival.
