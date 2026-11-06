# City buildings on iPhone 15: draw, triangle and memory plan

**Target:** iPhone 15 (A16 Bionic: 5-core GPU, 6 GB shared RAM, tile-based GPU, no hardware ray tracing) at **25 fps**
(40 ms per frame), rendered below 720p and **upscaled to 720p** with MetalFX. Top-down city sim (README), so a zoomed-out
camera can see most of the city at once.

The engine is Windows-only today (D3D12 + Vulkan). iOS needs Metal or Vulkan through MoltenVK (README "iOS gaps"). This plan
is written so the same data works on both, and every number here is an estimate until measured on a device.

## 1. Budgets (whole frame vs buildings' share)

| Resource | Whole frame on iPhone 15 (est.) | Buildings' share |
|---|---|---|
| Draw calls (all passes) | ≤ 400 (mobile CPU + thermals; Metal draws are cheap but the game's CPU is busy simulating) | **≤ 80** |
| Triangles submitted | ≤ ~1M | **≤ 300k** |
| GPU memory (textures + geometry) | ≤ ~1 GB resident (an iOS app is killed well before using all 6 GB) | **≤ 120 MB** |
| Internal resolution | ~960×540 → MetalFX upscale to 1280×720 | – |
| Pixel shading | the tile GPU's bandwidth is the limit: no heavy per-pixel loops | normal maps only by default |

## 2. What the engine does today (and why it can't scale)

| Fact (from the code) | Consequence |
|---|---|
| `SetMaterial(albedo, normal, surface)` binds one texture set per draw (`render/renderer.h`). | One draw per **material**. Our commercial GLBs have up to 7. |
| Instanced assets draw per material part **per visible spatial cell** (`main.cpp` ~3491-3512). | Draws = unique meshes × materials × visible cells. |
| Geometry draws in a depth prepass, the main pass and the shadow pass. | Each draw counts ~3×. On a tile GPU the prepass is wasted: it does hidden-surface removal in hardware. |
| Textures upload as uncompressed RGBA8 + mips. | 4× the memory and bandwidth iOS needs; iPhone has no BC formats, so it needs **ASTC**. |
| `Vertex` is 48 bytes with RGBA colour + UV. | Tint and an atlas cell index fit in the colour at no cost. |
| The ground shader already samples a padded, wrapped atlas (`GroundCell`, `mesh.hlsl:394`). | Atlas tiling is an established pattern. |
| `CreateMeshNoWait` / `RetireMesh` exist. | Meshes can be rebuilt on a worker thread and swapped without stalls. |

With instancing alone, 500 unique buildings in view would cost about **18,000 draws**: not viable on any platform,
let alone a phone.

## 3. Approach: baked blocks with zoom-based levels of detail

Options compared: plain instancing (fails), single-material instancing (still 1,000+ draws), a GPU-driven indirect pipeline
(lowest draw count, but it needs indirect multi-draw support on both the PC and the future iOS backend: later upgrade).
**Chosen: baked city blocks.** It works with the draw API that exists, costs a fixed number of draws per screen, and
suits a tile GPU.

### 3.1 One material per building (prerequisite)
- **One shared building atlas** for every zoned building: 4096², albedo + normal (height in alpha), cells padded for mips.
  It holds walls L1-5, roofs, asphalt, pavers, the detail sheet and the white brand sheet.
- **Vertex colour**: rgb = tint (white, or the business colour on brand faces), a = atlas cell index. UV stays cell-local and
  may exceed 1 so textures still tile.
- **Shader**: an atlas-tiled branch like `GroundCell` (wrap inside the cell, sample with the unwrapped gradients).
- **iOS textures**: ASTC 6×6 (~0.9 bits/px) for the atlas comes to **~4 MB with mips**. Normal maps use ASTC 5×5.
  The PC can stay RGBA8 or use BC7.

### 3.2 Three levels of detail (LOD), chosen by **zoom height**, not just distance
| LOD | Mesh per building | Used when | Shadows |
|---|---|---|---|
| **LOD0** full | today's model, 94-468 tris | camera low, cells near the focus | yes (near cells only) |
| **LOD1** box | walls + roof as boxes, roof tint, 12-30 tris | mid zoom, and the outer ring at low zoom | no |
| **LOD2** roof card | one textured quad per lot (2 tris), roof seen from above with baked AO edge | zoomed far out | no |

The generator writes LOD0, LOD1 and the LOD2 roof tile per building. LOD2 roof tiles go into a shared **roof atlas** (one small
tile per model, 64² ASTC: about 1165 × 2.6 KB ≈ 3 MB).

### 3.3 Block baking
- The city is split into **128 m cells**. Each cell keeps one merged mesh per LOD it currently needs. Each is **one draw**.
- At high zoom, cells join into **512 m super-blocks** of LOD1/LOD2 so a whole-city view stays tens of draws.
- **Dirty queue**: placing, levelling up, recolouring or demolishing a building marks its cell dirty. A worker thread merges
  it (under ~1 ms for 10k tris on an A16 performance core, to be measured), then swaps with `CreateMeshNoWait` / `RetireMesh`.
  At most one rebake per cell per ~1 s. Until the swap, the changed building draws alone and its old triangles are hidden.
- **Streaming**: only cells near the camera keep LOD0 meshes resident. Everything else keeps only LOD1/LOD2. Geometry memory
  then stays flat however big the city gets.

### 3.4 Pass rules on iOS
- **No depth prepass** (the tile GPU removes hidden surfaces in hardware).
- **Sun shadow**: LOD0 cells within ~250 m of the focus only. Far shadows are a baked dark edge in the LOD2 roof tile.
- **No ray tracing** (the A16 has none). Probe/GI features fall back to baked or ambient light on iOS.
- **No POM** on buildings by default. If wanted, a "near only" POM path for LOD0 cells within ~60 m.

## 4. Expected numbers on iPhone 15

| View | Cells in view | Building draws | Building tris | Geometry resident |
|---|---|---|---|---|
| Street level (LOD0 near, LOD1 ring) | ~15 LOD0 + ~20 LOD1 | ~15 + 15 shadow + 20 = **~50** | ~250k | ~30 MB |
| Mid zoom (LOD1 cells) | ~40 | **~40** | ~150k (5k buildings × 30) | ~20 MB |
| Whole city (LOD2 super-blocks, 10k buildings) | ~16-30 | **~30** | ~20k | ~2 MB |

Textures for all buildings: atlas ~4 MB + roof atlas ~3 MB (ASTC). **Total buildings well under 120 MB and 80 draws.**

## 5. Order of work
1. **Generator**: shared atlas, single-material LOD0, LOD1 boxes, LOD2 roof tiles, catalog entries for all three.
   Remove the 7-material layout.
2. **Shader**: atlas-tiled branch in `mesh.hlsl` (both backends) with a material marker.
3. **Engine**: city cell grid, building records, dirty queue, worker-thread merge, mesh swap, zoom-based LOD choice.
4. **Engine**: super-blocks, pass rules (no prepass on tile GPUs, shadow radius), draw/triangle counters per pass.
5. **Test scene**: 10k-building city on PC, with the frame's draws, triangles and memory logged against section 1.
6. **iOS**: texture pipeline to ASTC; Metal/MoltenVK backend (separate project, README); measure on an iPhone 15 and retune
   cell size, LOD switch heights and shadow radius.
