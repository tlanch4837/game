# Prompt: wire zoned expression wrinkles into the engine

You are working in `C:\custom windows apps\gameengine` (TBD engine, C++, Direct3D 12 + Vulkan).
Goal: when Wyatt's face moves (mocap, lipsync, clip playback or the character creator), each face zone wrinkles on its own, driven by the rig channels, with age lines on top. A frown wrinkles the glabella and not the forehead. A smile creases the nasolabial fold and the mouth corners. Raised brows wrinkle the forehead.

**The offline half is already done. Do not rebuild it.** The engine half does not exist yet (`creator.cpp` still shows "WRINKLES: NOT WIRED YET"). Your job is the renderer, the weight computation and the creator UI.

## Rules
- Every renderer change lands on **both** D3D12 (`src/render/renderer_d3d12.cpp`) and Vulkan (`src/render/renderer_vulkan.cpp`), plus the shared header `src/render/renderer.h`. No backend-only features. If a backend is not finished, the feature is not finished.
- No ray tracing in normal runs.
- Keep the old single-groove path (`SkinWrinkle`, vertex alpha 0.5 to 0.59) as the **fallback when the zone textures are not bound**. With no textures bound, the picture must be pixel-identical to today. Vertex alpha stays the skin flag.
- Everything is linear data (UNORM, never sRGB) for all four textures.
- Do not commit. Leave the changes in the working tree with a summary.
- Read `AIchat.txt` first. When you finish, append a short note at the end, signed with your model name (append only, never rewrite it).
- In-world text must say Port Lazare, never a real city. This rarely applies here.

## What exists (read these first)
- `gameenginetools/wrinkle_zones/zones.py` (header docstring explains the whole pipeline) and its output in `gameenginetools/wrinkle_zones/out/`:
  - `wrinkle_masks_a.png`: RGBA8, R forehead, G glabella, B crowsFeetL, A crowsFeetR
  - `wrinkle_masks_b.png`: RGBA8, R noseBridge, G nasolabial, B mouthCorners, A chinNeck
  - `wrinkle_compress_n.png`: RGBA8, RGB tangent normal of every zone's compress grooves at weight 1, A is height (128 flat)
  - `wrinkle_stretch_n.png`: the same for the stretch variants (nasolabial, mouthCorners, chinNeck only)
  - `wrinkle_zones.json`: zone table, rig channel drivers with gains, age curve, and the exact blend formula (key `formula`). **This JSON is the spec. Read all of it.**
  - 2048x2048, 8 gutter texels. Tangent space is glTF convention, +Y up the image, flat = (128, 128, 255).
- `docs/character_renderer_requests.md` section 2 is the original request. Ask: four linear RGBA8 textures (ideally one 4-slice `Texture2DArray`, because mips average slopes), 16 floats of zone weights per skin draw (`float4 wC[2]`, `float4 wS[2]`), added as weighted slopes on top of the pore normal using the existing tangent frame.
- `src/shaders/mesh.hlsl`: the skin branch near line 555 (`if (skin) {`) has the pore normal and the old `SkinWrinkle` groove. Line 873 uses wrinkle for a cavity term.
- `src/mocap/face_map.h` around line 1372 computes the old per-vertex squeeze into vertex alpha. Channel weights (52 ARKit-style names, `src/mocap/face_clip.h` `kBlendshapeNames`) exist there after gain and enable. Subject's left is +X.
- `src/character/creator.cpp` line ~1183 is the placeholder to replace.
- `src/main.cpp` line ~514 has `TBD_WRINKLE` for fixed test captures. Keep it working.

## Tasks

### 1. Textures and binding (both backends)
1. Load the four PNGs as linear UNORM, with a full mip chain, into one `Texture2DArray` with 4 slices (masks_a, masks_b, compress, stretch) or four textures if an array does not fit the engine's material system. Say which you chose and why.
2. Add them to the skin material's descriptor set / root signature (D3D12) and descriptor set layout (Vulkan). Update `spirv.bat`/`build.bat` only if needed.
3. Decide how a draw says "I have zone wrinkles": a per-material flag plus the textures bound. Not bound means the old path.

### 2. Zone weights per draw
1. Add `float4 wC[2]; float4 wS[2];` (zone order = `index` in the JSON) to the per-draw constants for skin on both backends. Mind HLSL cbuffer packing and the Vulkan layout. Keep alignment identical.
2. Write a small C++ module (for example `src/character/wrinkle_zones.h`) that:
   - loads `wrinkle_zones.json` (the engine already has a JSON parser in `mesh_import.h`),
   - each frame computes, per zone, `dynC`, `dynS`, `static` (age) and `wC`, `wS` **exactly** as the JSON's `formula.per_zone_per_frame` says, from the face rig's channel weights and the character's age,
   - exposes the 8+8 weights and takes a manual override per zone for the creator and for tests.
3. Hook it where the face driver output is available (`src/mocap/face_driver.h`, `face_map.h`, clip player, lipsync, live). It must work from every source, not only live capture.
4. Smooth weights over a few frames so mocap jitter does not make wrinkles flicker.

### 3. Shader (`src/shaders/mesh.hlsl`)
In the skin branch, after the pore normal and when zone textures are bound, implement `formula.per_pixel`:
```
m[8] = masks_a.rgba, masks_b.rgba
kC = dot(m, wC[8]); kS = dot(m, wS[8])
c = compress.rgb*2-1; s = stretch.rgb*2-1
slope = base.xy/base.z + kC*c.xy/c.z + kS*s.xy/s.z
tn = normalize(float3(slope, 1))
```
- Sample with the surface UV (`i.uv`), not `uv * SkinDetailScale()`. The wrinkle maps are atlas-space.
- Use the existing cotangent frame the way the pore normal does. The JSON `conventions` say it matches. Verify the groove direction is not flipped by comparing against the `preview_*.png` in `out/`.
- Fade the effect with distance or footprint so it never shimmers.
- Use the height alpha for the cavity term (the line 873 equivalent) so grooves darken the ambient light, scaled by `kC`.
- Keep `SkinWrinkle` as the fallback.
- Recompile both shader targets. The engine hot-reloads shaders, so `mcp__tbd-engine__engine_reload_shaders` works during iteration.

### 4. Character creator UI (`src/character/creator.cpp`, `creator.h`)
Replace the "NOT WIRED YET" note with a real WRINKLES section: an age slider, an enable toggle, and 8 zone sliders that override the rig weights for preview. Update the header comment in `creator.h` so it no longer lists Wrinkles as NOT WIRED. Save and load the settings with the character preset (`character_preset.h`) without breaking old preset files.

### 5. Verify (both backends, with screenshots)
Use the engine MCP tools (`engine_launch`, `engine_screenshot`, `engine_camera`, `engine_set`, `engine_reload_shaders`, `engine_quality`) or the existing test hooks. For each backend, capture a head close-up in these states:
1. Neutral, age 0.
2. Brows up (browInnerUp, browOuterUp both sides at ~1).
3. Frown (browDown both sides at ~1).
4. Smile (mouthSmile both sides at ~1).
5. Squint (eyeSquint both sides at ~1).
6. Neutral at old age (age line on top).
7. Textures not bound: identical to the pre-change image.

Compare against `gameenginetools/wrinkle_zones/out/preview_*.png` (the offline software renders). The right zones must wrinkle, grooves must have the right direction, and nothing may cross the face's midline seam (the bake handles the seam, the shader must not break it). D3D12 and Vulkan must match.

### 6. Report
- Per-draw cost of the added texture samples (use the engine log's `GPU frame` per-pass breakdown before and after, with the character on screen).
- Files changed, how each backend binds the textures, and anything you could not verify.
- Any problem with the mesh (zones not covering what they should) goes back to the tool in `gameenginetools/wrinkle_zones`, not into engine hacks.

## Out of scope
Lips (`gameenginetools/lips`, request 3), the skin detail-scale item (request 1), teeth/gums/tongue, a muscle solver, and any change to the bake tool's outputs unless a real bug blocks you.
