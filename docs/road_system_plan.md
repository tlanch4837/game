# Road system plan (efficient by design)

Goal: a whole city of roads costs about one draw call, almost no memory, and no mesh rebuild when the player edits a road.
Flat first; heights later (see the last section).

## 1. Data: the network is the only source of truth

```
Node     { float2 pos; float height (0 for now); uint16 firstLink, linkCount; }       // 16 B
Segment  { uint32 nodeA, nodeB; float2 c1, c2;  // cubic Bezier controls (straight: on the line)
           uint16 profile; uint16 flags; float length; }                              // ~36 B
Profile  { strips[]: width, material layer, height (later), kind (lane/sidewalk/...); lanesFwd/Back }
```

- 10,000 segments are about 360 KB. The 20 road types are 20 profile rows.
- No mesh is stored per road. Everything below is generated from these three arrays.
- Editing a road means rewriting one Segment entry, plus its two nodes' intersections.

## 2. One quad across the whole road, not one per strip

A road slice is one quad spanning the full width. The pixel shader finds the material from the lateral coordinate `u` (0..1 across):

- `profileTex`: a small texture, one row per profile, holding the strip edges and material index.
  The shader binary-searches (or does a fixed 16-step loop over) the row to get the material layer at `u`.
- `roadMaterials`: one `Texture2DArray` (albedo BC1, normal BC5) holding asphalt, gravel, concrete, curb, pavers, grass and bike asphalt.
- Edges between strips are sharp (point lookups on the row), so no extra geometry is needed for curbs, medians or sidewalks.

Result: a **straight segment is 2 triangles no matter how many lanes it has**. Markings later come from the same `u`
(dashes = `frac(v / dashLength)`, lines = distance to strip edges), so decals add no geometry either.

## 3. No vertex buffers: the vertex shader builds the road

One instanced draw for every visible segment:
- `SV_InstanceID` → segment index (from a visible list); `SV_VertexID` → (slice i, left or right side).
- The vertex shader evaluates the Bezier at `t_i`, offsets ±width/2 along the normal, and outputs `u`, `v` (v = arc length).
- Slices per segment are chosen on the CPU when the segment changes, and stored in `flags`:
  - straight: 1 slice (2 triangles)
  - curve: `n = ceil(turnAngle / maxStep)`, where maxStep keeps the outer-edge error (sagitta) under ~2 cm.
    A 90° bend on a small road is about 12 slices, on a highway about 20.
- Arc length for `v` comes from a tiny per-segment lookup, so textures don't stretch on curves.

Draw cost: **1 indirect draw for all roads, plus 1 for all intersections.**

## 4. Intersections and end caps (node meshes)

- Each segment end is cut square at its node (already true in the preview).
- A node with 2+ links builds a small polygon from the incoming end edges plus corner curves.
  It's about 10–40 triangles, stored in one shared buffer and rebuilt only when that node changes.
- It uses the same shader, with `u` taken from distance to the curb line, so it matches the roads.
- Dead ends get a rounded cap of the same kind.

## 5. Culling and LOD

- Segments are bucketed into the world grid cells the engine already streams. A compute pass (or the CPU, for a start) tests cell
  and segment bounds against the frustum (`culling.h`) and writes the visible instance list for the indirect draw.
- LOD is driven by **screen-space error, not distance**: a level is used only when its error is under 1 pixel, so the switch can't be seen.
  - Curve slices: sagitta error projected to the screen stays under 0.5 px. Close up that means more slices; far away, fewer.
  - Normal maps are never switched off. Mipmapping fades them out naturally where the detail is smaller than a pixel.
  - Bake to ground texture: only where the baked texel is at most the size of a screen pixel (from about 600 m at 1080p on a normal FOV),
    with a 30 m crossfade, so no pop.

## 5b. Keeping ~99% of the detail

Every saving above removes work the eye can't see. Where a saving could cost detail, this is how it's prevented:

| Saving | Detail risk | How it's kept |
|---|---|---|
| One quad across the road, material from `u` | Jagged or blurred strip edges | Edges are anti-aliased in the shader with `fwidth(u)` (a 1-pixel blend), as sharp as real geometry and without stair steps |
| Few slices on curves | Faceted curves | Slice count from a sub-pixel error budget, and per-pixel normals come from the Bezier, so even lighting has no facets |
| Straight = 2 triangles | Texture stretching | `v` = true arc length; world-space UVs keep texel density fixed |
| Texture compression | Blocky colour | **BC7** for albedo (near-lossless, 1 byte/px) instead of BC1, BC5 for normals. About 18 MB instead of 11 MB, still well under 1/3 of PNG |
| Tiling textures | Visible repeats over long roads | Second-scale macro noise plus per-segment random UV offset/rotation, done in the shader, so no extra textures |
| Far bake | Blurry distant roads | Used only where a baked texel ≤ 1 screen pixel, with a crossfade |
| No vertex buffers | None | Same math, only computed on the GPU |

The only real loss is that curbs are flat until the heights phase. That's by choice, not a cost of the system.

## 6. Memory and per-frame budget (estimate for a large city, 10k segments)

| Item | Cost |
|---|---|
| Network data | ~0.4 MB |
| Intersections | ~2 MB of vertices |
| Textures (8 layers × 1024², BC7 + BC5, mips) | ~18 MB |
| Draw calls | 2 |
| Triangles drawn (visible, typical view) | ~20–60k |

## 7. Physics, AI, traffic all read the same data

- Collision: flat, so a straight segment is one box and a curve is a few boxes (or one static mesh per cell).
- Traffic lanes: the lane centerlines are offsets of the same Bezier (profile strip offsets). No extra data.
- Navmesh: sidewalks and paths are marked walkable from the profile `kind`.

## 8. Heights later (not now)

- `Node.height` plus per-strip heights from the profile (curb 0.15 m, barrier 0.85 m).
- Raised strips get side faces: a second instanced pass that draws only the step edges (2 triangles per edge per slice).
- Terrain following: the vertex shader samples the heightmap, and straight segments are split by slope instead of being 1 slice.
- Bridges and tunnels are flags on Segment, with pillars instanced along them.

## Build order

1. Network data plus CPU slicing in the preview (cheap to prove out): one quad per slice, material from `u`.
2. Port to the engine: data structs, the road shader, the Texture2DArray, one indirect draw.
3. Intersections and end caps.
4. Grid-cell culling and the far-distance bake into the ground texture.
5. Markings from `u`/`v`, then heights.
