# Shared building atlas

One material per zoned building, so a building costs one draw (and one material bind) instead of one per texture. Plan:
`docs/city_draw_budget_plan.md` sections 3.1 and 5, step 2. This is the format, the tool that builds it, and what a model generator must
write to use it.

## Format

| | |
|---|---|
| Page | 4096 x 4096, an 8 x 8 grid of 512 px cells: 64 cells a page |
| Cell | one tile of a texture, resampled to 480 px, wrapped 16 px past every edge (the tile's own continuation) |
| Albedo page | `<page>_albedo.png`, RGBA8: rgb albedo (sRGB, as authored), **a = roughness** (0..255) |
| Normal page | `<page>_normal.png`, RGBA8: rgb tangent-space normal (+Y up, as the other building normals), **a = height** (255 high; cavity only) |
| Mips | made by the engine on upload. Cells are 512-aligned, so its 2 x 2 box mips keep cells apart down to mip 8; the shader caps its LOD at 4 (the 16 px pad is one texel there), so no sample ever blends two cells |
| Table | `assets/buildings/atlas/building_atlas.json`: `cells[name] = {page, index, alpha, metresPerTile, roughness}`, each page's file names and content hash, and a table hash |
| Shader layout | `src/shaders/building_atlas.hlsli` (generated): page size, grid, pad, LOD cap. The same for every page |

Pages today (`tools/atlas/building_atlas_pages.json`):

| Page | Cells | From |
|---|---|---|
| `bld_residential` | 30 | `low_density` and `row_housing` L1–L5 wall, roof, detail |
| `bld_commercial` | 48 | `low_commercial`, `low_office` (wall, roof, shingle, trim, detail, asphalt, pavers, brand, grass), `high_density` facades |
| `bld_industry` | 64 (full) | `industry` |

SPOM parts (balconies, canopies: alpha 0.8, `SpomPS`), SPOM slabs (`SlabPS`) and windows (`roomglass`, `RoomGlassPS`) are not on the
atlas: they are other shaders with their own textures and stay separate parts.

## Building the atlas

```bash
python tools/atlas/pack_atlas.py
```

- Rerunnable: edit a source texture or the manifest and run it again. A page whose inputs are unchanged (same content hash over the layout,
  its cells' parameters and the bytes of every source file) is skipped.
- `--check` packs nothing and exits 1 when a page is out of date; `--force` repacks everything.
- It also rewrites `building_atlas.json` (with each page's `hash` and a table `hash`) and `src/shaders/building_atlas.hlsli`.
  Bake caches (the Track R chunk bakes) should take the table hash into their key, so a repacked page rebakes.
- Add a cell by appending it to the end of a page in the manifest. **Inserting or reordering changes the indices** of every cell after it,
  and every mesh on that page must then be regenerated.
- About 13 s for all three pages.

## What a generator writes per model

1. **One material part per atlas page** (one for most buildings), named so it contains `atlas`, for example `atlas_bld_residential`:
   - `baseColorTexture` = `assets/buildings/atlas/<page>_albedo.png`;
   - `normalTexture` = `<page>_normal.png`;
   - `roughnessFactor` / `metallicFactor` are ignored (roughness is per texel, in the albedo's alpha).
   - Windows (`roomglass`), SPOM volumes and SPOM slabs stay separate parts, as now.
2. **COLOR_0** (float VEC4) on every vertex of that part:
   - rgb = tint, linear multiplier; 1 1 1 = as authored, a brand colour on brand faces;
   - **a = `cells[name].alpha`** from the table, which is (index + 0.5) / 64. The importer (`mesh_import.h`, material name
     contains `atlas`) moves it to 3 + a, the band `mesh.hlsl` reads. glTF stays in 0..1.
3. **UVs in tiles, unwrapped**: u, v = surface metres / the texture's metres per tile, as the generators already write them for tiled
   walls (`tile_maps(..., k)`). They may run far past 1: the shader wraps each tile inside its cell. Detail sheets (window and door
   sprites at sub-cell positions) keep their 0..1 sheet UVs. glTF v runs down the image, as now.
4. One draw: merge every atlas face of the building into that one part. A building that mixes pages (rare) is one part per page.

## Shader (`src/shaders/mesh.hlsl`)

- `PSMain` routes vertex alpha in [3, 4) to `AtlasBuildingPS` (before the foliage mark at > 1.5).
  - It samples the cell with the unwrapped uv's gradients (a tile seam is not a mip seam), capped at the LOD the padding covers.
  - It builds the tangent frame from screen derivatives, as `Material()` does.
  - It applies the world-surface treatment (cavity from height up close, sky tilt, `Weather`), lit by `LitPbr`.
  - No POM: at 1 cm it was invisible past a few metres, and its march is the costliest part on a phone.
- `ChunkBakePS` (Track R's far bakes) samples the same cell at mip 2, as the other building materials bake, so the far volumes keep
  the near meshes' colour.
- The unlit quality path (`FEATURE_OFF(256)`) samples the atlas cell.
- Shadows need nothing: buildings cast through `ShadowVS` / `ShadowInstancedVS`, which read no texture. `PSShadowCutout` is the
  alpha-tested hair path and never sees a building.
- No new bindings: the atlas is the material's albedo and normal, in the existing slots (Metal: argument buffer ids 0 and 1). Checked
  through `spirv.bat` and ios-metal's `tools/msl.ps1`.

## Memory

- A page is 2 x 4096² RGBA8 with mips, about 170 MB on the PC; three pages are about 510 MB.
- That is the same order as the 142 separate 512² texture pairs it replaces (about 2.8 MB each with mips, about 400 MB).
- On iOS the pages must be ASTC: 6x6 albedo, 5x5 normal + height. That is about 30 MB a page, done offline.
- The roughness-in-alpha and height-in-alpha channels need ASTC's RGBA modes.
- An array texture (one layer per material) was the alternative. It was rejected for now because it changes the material binding
  layout on all three backends (Metal's set 1 argument buffer has fixed Texture2D members).
