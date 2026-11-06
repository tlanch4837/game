# Prompt: fix full_map stutter and frame rate

You are working in `C:\custom windows apps\gameengine` (TBD engine, C++, Direct3D 12 + Vulkan).
Scene: `testproject/WesternGame/Scenes/full_map.tbdscene` (1575 entities, the Port Lazare town).
Goal: **steady 120–165 fps (6–8 ms frame) at 1615x812, with no hitch over 16 ms**, on an RTX 4060.

Rules:
- Every renderer change must land on **both** D3D12 (`src/render/renderer_d3d12.cpp`) and Vulkan. No backend-only features.
- Do not change how the scene looks except for distant level of detail that is not noticeable in motion.
- Measure before and after each change using the engine's own log lines (`bin/engine.log`): the `Streaming:` lines and the `GPU frame ... (60-frame mean)` per-pass breakdown. Report the numbers.
- Do not commit. Leave the changes in the working tree with a summary.

## Problem 1: streaming hitches (the stutter)

Evidence from the log:
```
Streaming: slow mesh, 240 vertices: 85.8 ms
Streaming: 135 ms frame; the pump before it took 125.2 ms
Streaming: idle 67.15 s after start; 1287 of 1643 loaded, 2063 MB estimated;
  1112 frames, worst 202.9 ms, longest pump 149.1 ms, over 33 ms: 738, over 100 ms: 237
```
A 240-vertex mesh can't legitimately take 85 ms, so the pump is blocking on something: a lock, a GPU fence/queue wait, a staging buffer flush, pipeline or descriptor creation, or a file read on the main thread.

Tasks:
1. Read `src/world_streaming.inl`, `src/surface_async.inl`, `src/core/jobs.h`, `src/mesh_lod.h` and the upload paths in both renderers. Add scoped timers inside the pump to find exactly which call takes the time.
2. Move all file I/O, decoding, mesh building and LOD generation to job threads. The main thread should only do the final handoff.
3. Make GPU uploads asynchronous: a staging ring buffer, a copy/transfer queue, and fence polling (never a wait) on both backends. Resources become visible a frame or more later.
4. Put a time budget on the main-thread pump (for example 1.5 ms per frame) and spread work across frames.
5. Find out why streaming goes idle at **1287 of 1643** with 356 never loaded (budget, distance, failed loads?). Fix it or log the reason per entity.
6. Done when: a fly-through of the whole town logs **0 frames over 33 ms** from streaming, and every entity that should load does load.

## Problem 2: GPU cost of the scene pass (the low fps)

Evidence (whole town on screen vs looking away):
```
town:  GPU frame 44.6 ms: shadow 3.6  scene 34.7  composite 3.7  taa 1.4
away:  GPU frame 12.1 ms: shadow 4.3  scene 1.2   composite 4.2  taa 1.3
peaks: shadow up to 12.7 ms during streaming
```
The cost scales with how much of the town is visible, so it is draws and triangles, not resolution.

Tasks:
1. Log draw-call count, triangle count and pipeline/descriptor binds for the scene and shadow passes. Find out whether the cost comes from the CPU (submission) or the GPU (vertex or pixel bound), and check overdraw.
2. **Level of detail:** use `src/mesh_lod.h` and `Assets/LodCache` so buildings switch to lower-detail meshes with distance and screen size. Balconies, railings, shutters, chimneys and the interior parts (log: "466 interior parts in building meshes") should drop out or merge at range. Don't draw interior parts from outside unless the camera is near or inside.
3. **Culling:** tight per-instance frustum culling, plus occlusion culling (a Hi-Z pass from the previous frame's depth, or GPU-driven culling) so buildings hidden behind the first rows cost nothing.
4. **Batching:** sort by pipeline and material, and use instancing or indirect draws for repeated buildings and props. Merge static small parts per building.
5. **Shadows:** cull shadow casters per cascade, use lower-detail meshes in shadow maps, stop small far objects casting shadows, and cache or update far cascades less often.
6. Check whether composite (about 4 ms) and TAA (about 1.4 ms) can be trimmed. They are secondary.
7. Done when: the same camera as the screenshot (target 4294 31.76 2999, yaw -62, pitch 18, distance 8) runs at **6–8 ms GPU frame** on both D3D12 and Vulkan, with scene pass at 4 ms or less and shadows at 1.5 ms or less.

## Deliverable
A short report with before/after numbers for both problems on both backends, the files changed, and anything left over.
