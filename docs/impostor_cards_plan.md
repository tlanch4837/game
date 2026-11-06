# Impostor Cards: implementation plan

An LOD system for tbd_engine: a dense mesh is baked once from six axis-aligned views, and at distance it is
replaced by a camera-facing card whose pixel shader rebuilds a view-dependent, depth-correct, lit approximation
from that baked data.

This is an independent design inspired by the public description of Feral3D's *Imposter Cards* (Blender add-on).
Its shader source is not public, so nothing here claims to match its internals.

## 1. What the engine looks like today (and what that forces)

| Fact (from the code) | Consequence for this feature |
|---|---|
| `Renderer` (`src/render/renderer.h`) has **no textures, samplers, descriptors or offscreen targets**. Only swapchain + depth. | Phase 0 must add texture arrays, render-to-texture and sampling to **both** D3D12 and Vulkan backends (the header promises both draw "the same thing the same way"). |
| One shader (`mesh.hlsl`); lighting is done **in the vertex shader** (`kSunDir`, `kSkyColor`, ...). | The card shades per pixel, so the light constants move to a shared `lighting.hlsli` that both shaders include. |
| Push/root constants are exactly 128 bytes, vertex-stage only (`mvp` + `model`). | The impostor pipeline gets its own layout. Per-object data goes in a per-instance vertex buffer, not push constants. |
| Meshes are unindexed 40-byte `Vertex` lists. `DrawTransient` already has a 16 MB per-frame upload ring. | Instance data can live in the transient ring. No new buffer type is needed. |
| `EndFrame` waits for the GPU (D3D12) / next `BeginFrame` waits on the fence (Vulkan). One frame in flight. | A synchronous bake with its own command list/buffer is simple and safe between frames. |
| Only `Cube / Plane / Sphere` exist, 12-720 triangles each. The content browser lists `.obj/.fbx/.gltf` but there is no importer. | **There is nothing dense to impostor yet.** A dense test asset is a prerequisite or the feature can't be judged. |
| `StartRenderer` rebuilds the renderer on API switch / pop-out and calls `RebuildSceneMeshes()`. | Baked atlases die with the renderer, so the impostor system must rebake on that same path. |
| Scene files are pipe-separated text, `E|...` parser accepts `f.size() >= 7`. | New per-entity fields can be appended as `f[7]...` without breaking old files or bumping `TBDSCENE 1`. |

## 2. Technique (what gets built)

**Bake (once per unique source mesh, at scene load / renderer start).** Render the mesh orthographically from the
six axis directions (+X, -X, +Y, -Y, +Z, -Z) into tiles of `T x T` (default 128). The mesh is first fitted to its
AABB mapped to `[-1,1]^3`, so every impostor shares one coordinate convention. Each face writes:

| Target | Format | Content |
|---|---|---|
| Albedo | RGBA8 | vertex color rgb; a = coverage |
| Normal | RGBA8 | object-space normal `*0.5+0.5`; a reserved (roughness later) |
| Depth | R16_UNORM | first-surface distance from that face's plane, 0..1 across the box; **1.0 = empty** |

Storage is one `Texture2DArray` per channel, **layer = slot * 6 + face**, rather than a literal packed 2D atlas.
Same "one material, one draw for every impostor" benefit, but no tile padding/bleed handling and trivial slot
allocation. (Deviation from the add-on's atlas; change only if you specifically want one 2D image.)

**Card (per instance, one instanced draw for all impostors).** A quad built from `SV_VertexID`, expanded in the
camera's right/up around the object center to cover the bounding box, so it works at any entity rotation.

**Pixel shader, "tri-planar hull ray cast".**
1. Transform the eye ray into box space (`[-1,1]^3`), clip to the box, early-out on miss.
2. March ~16-24 steps inside the box, then 4 bisection steps. A point `p` is *inside* if, for each axis `a`, looking
   up the `+a` and `-a` depth tiles at the other two coordinates gives an interval `[ -1+2*d_neg , 1-2*d_pos ]`
   containing `p[a]`. Empty texel (`d >= 1`) = outside. This uses all six views and is the intersection of three slabs.
3. On hit, take albedo + normal from whichever of the three viewer-facing faces has the smallest depth error at `p`
   (smooth blend near seams).
4. Light per pixel with the same sun/sky/bounce model as `mesh.hlsl` (normal rotated by the instance rotation).
5. **Write `SV_Depth` from the hit point** so the card intersects terrain and neighbors correctly. This also stands in for
   the add-on's extra "tri-planar" planes (which it uses to fix clipping/reflections). `discard` on miss, so no blending
   and no sorting.

**Known limits (document in the UI, not fixable here):** concavity along an axis beyond the first surface is
approximated; silhouettes come from six views only; no self-shadowing; no animation/deformation; close-ups look wrong,
so this is an LOD, not a replacement.

## 3. New / changed files

| File | Change |
|---|---|
| `src/procmesh.h` (new) | Dense procedural test assets: `MakeTreeMesh` (trunk + foliage blobs, ~20-60k tris, vertex-colored) and `MakeRockMesh`. Plus an AABB helper. |
| `src/scene.h`, `scene_io.h` | `MeshKind::Tree/Rock`; `Entity::lod` (`useImpostor`, `switchPx`); serialize as optional trailing fields `f[7..8]`. |
| `src/tools.h`, `src/main.cpp` | Create tool + shortcut for Tree; **World** menu items: Impostor LOD on/off, Force impostors (debug), Scatter N trees (stress test). |
| `src/shaders/lighting.hlsli` (new) | Shared sun/sky/bounce constants + `Lit(n, albedo)`. `mesh.hlsl` switches to it with identical output. |
| `src/shaders/impostor.hlsl` (new) | `ImpVS`, `ImpPS` (ray cast above); `impostor_bake.hlsl` (new) for the 3-target bake pass. |
| `build.bat` | Copy the new `.hlsl/.hlsli`; add `-I bin\shaders` and SPIR-V compile lines for the new entry points. |
| `src/render/renderer.h` | New API (section 4). |
| `src/render/renderer_d3d12.cpp` | SRV heap, texture arrays, per-slice RTV/DSV, bake PSO (3 RTs), impostor root signature + PSO (static sampler, instance slot), fxc include handler. |
| `src/render/renderer_vulkan.cpp` | `CreateImage` helper (memory type selection mirrors `CreateBuffer`), descriptor pool/set layout, sampler, bake + impostor pipelines (dynamic rendering, 3 color attachments), layout transitions via existing `ImageBarrier`. |
| `src/impostor.h` (new) | `ImpostorSystem`: registry (bake key -> slot), per-frame LOD selection + instance gathering, hysteresis, counters. |
| `src/stats.h` / HUD lines | Show `IMPOSTORS n` and count the impostor draw as 1 draw + `2*n` triangles. |

## 4. Renderer API additions

```cpp
struct ImpostorInstance {            // 48 bytes, per-instance vertex stream
    float center[3]; uint32_t slot;  // world-space box center, atlas slot
    float rot[4];                    // entity rotation quaternion
    float half[3];   float tint;     // world half-extents (box half * entity scale)
};

virtual bool CreateImpostorStore(uint32_t maxObjects, uint32_t tileRes) = 0;
// Synchronous: renders 6 faces into the store. Call between frames. Returns slot or -1.
virtual int  BakeImpostor(MeshId mesh, Vec3 boxMin, Vec3 boxMax) = 0;
virtual void FreeImpostor(int slot) = 0;
virtual void DrawImpostors(const ImpostorInstance* inst, uint32_t count,
                           const Mat4& viewProj, Vec3 eye, Vec3 camRight, Vec3 camUp, bool counted) = 0;
// Debug: dump a slot's six tiles to BMP.
virtual bool DumpImpostor(int slot, const wchar_t* path) = 0;
```

## 5. Phases (each has an exit check; do them in order)

**Phase 0 - Prerequisites.**
0a. `procmesh.h` + `Tree`/`Rock` kinds, create tool, save/load. *Exit:* place a 40k-tri tree, save/reload the scene.
0b. `lighting.hlsli` extraction, `build.bat` + include handling in both compile paths. *Exit:* image identical to before
    on both APIs.
0c. Texture-array + render-to-texture + sampler plumbing in D3D12 and Vulkan, proven with a throwaway test that
    fills a layer with a color and samples it onto a quad. *Exit:* same output on both APIs, validation layers clean
    (the engine already has a `validation` flag).

**Phase 1 - Bake.** Bake pipelines, ortho face matrices, `BakeImpostor`, `DumpImpostor`. *Exit:* BMP dumps of the six
tiles of the tree look correct (mirrored axes are the most likely bug: define the per-face (u,v) <-> axis table once,
in `impostor.h`, and use it in both bake and shader).

**Phase 2 - Card.** `impostor.hlsl`, instance stream, `DrawImpostors`. Start with albedo-only, no depth test
(view-dependent card), then add the ray cast, normals + lighting, then `SV_Depth`. *Exit:* an orbiting camera around one
card shows convincing parallax; the card intersects the ground correctly.

**Phase 3 - Engine integration.** `ImpostorSystem`: bake key = (mesh kind + color), shared slots across identical
entities; per frame, project each entity's bounding sphere to pixels, switch to the card below `switchPx` (default
~140 px diameter) with ±12% hysteresis; gather all cards into **one** `DrawImpostors`. Rebake from
`RebuildSceneMeshes`; clear on `StopRenderer`. *Exit:* 2,000 scattered trees: nearby ones are meshes, far ones cards;
draws and POLYS in the HUD drop accordingly; switching D3D12 <-> Vulkan rebakes without a crash.

**Phase 4 - Editor surface.** Menu toggles, HUD `IMPOSTORS n`, optional inspector LOD fields (the inspector is
raw Win32 controls, so start with menu items and per-entity scene-file fields; add a checkbox only if wanted),
"show card bounds" debug overlay.

**Phase 5 - Quality & perf (only if Phase 3 measurements justify it).** In rough value order: bake a combined per-axis
"slab" map (RG16 near/far) so the march costs 3 fetches/step instead of 6; mips with min-filtered depth; dithered
crossfade at the LOD switch; baked AO channel; tighter card geometry than the bounding box; disk cache
`Assets/Impostors/*.tbdimp` to skip rebaking.

**Status: Phase 0 done.** Tree (28,032 tris) and Rock (8,800 tris) exist as tools `4`/`5`, saved and loaded in scenes.
`lighting.hlsli` is shared (Vulkan SPIR-V byte-identical to before). Both backends have the store (three texture arrays,
per-layer render targets, tile depth, descriptors, sampler) and pass `TBD_TEXTURE_SELFTEST=1` with validation layers
on. Self-test files: `src/shaders/selftest.hlsl`, `SelfTestTextures()` in each backend. Not yet viewed in the editor GUI.

## 6. Verification (there is no test suite in the repo)

- **Visual parity:** same scene on D3D12 and Vulkan; compare screenshots.
- **LOD pop check:** a debug "force impostor / force mesh" toggle, then capture both at the switch distance; compare
  side by side (and a simple per-pixel diff view if desired).
- **Correctness aids:** `DumpImpostor` BMPs; a debug view that colors by chosen face; step-count heat map.
- **Perf:** use the existing HUD (FPS, DRAWS, POLYS) on the 2,000-tree scene with LOD off vs on; record the numbers in
  this doc.
- **Robustness:** validation layers on for both backends; bake with 0 / degenerate-AABB meshes; exceed `maxObjects`
  (must fail soft, keep the real mesh); API switch mid-session.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Per-face mirroring/axis mistakes produce subtle wrong parallax. | Single shared face table; BMP dumps; a test cube with colored faces. |
| `SV_Depth` disables early-Z and the march is fragment-heavy. | Card only covers distant (small) pixels by construction; measure before optimizing; Phase 5 slab maps. |
| Doubling every GPU feature across D3D12 and Vulkan. | Do 0c as a minimal vertical slice on both before building anything on top. |
| Bounding-box card overdraws and can shimmer at silhouettes. | Accept in v1; tighten later. |
| Vulkan 128-byte push-constant minimum. | Impostor push block is 112 bytes (viewProj + right/up/eye); per-object data is in the instance stream. |

## 8. Decisions I made (say if you want any changed)

1. `Texture2DArray` per channel instead of one packed 2D atlas.
2. Dense test assets are procedural (tree/rock), **not** an OBJ/glTF importer. An importer is a separate feature; the
   bake takes any `MeshId` + AABB, so it plugs in later.
3. Albedo/normal/depth only for v1. The engine has no PBR or per-material data yet, so roughness/metallic channels are
   reserved, not built.
4. No shadows from or onto impostors in v1 (the engine has no shadow pass either).
