# Face Mocap Live: hand-off prompt

Paste everything below the line into a fresh Claude Code session started in `C:\custom windows apps\gameengine`.

---

You are picking up the **Face Mocap Live** system of a custom C++ game engine (TBD Engine, Windows, MSVC 2022, Direct3D 12; Vulkan backend is untextured). Your job: **work out how to make this system better, and prove each claim with evidence**, then give a prioritized plan. Do not start big refactors without asking.

## Why this matters
The owner wants to **sell** this (probably the face-capture-to-character tool rather than the whole engine). So quality you can demonstrate, clean licensing, and robustness on other people's webcams and heads all count.

## What exists (all working, built by `build.bat`, outputs in `bin\`)
Webcam -> tracker -> 52 expression weights + head pose -> rig deforms a CPU copy of a head mesh every frame -> re-uploaded to the GPU. A browser-style tab ("Scene" | "Face Mocap Live") hosts the UI; opened from Mocap -> Live Webcam.

| Area | Files (`src\mocap\` unless noted) | Notes |
|---|---|---|
| Tracker | `face_tracker.cpp/.h` | MediaPipe BlazeFace detector + 478-point landmark model (with irises) + 52-blendshape model, all ONNX Runtime. Falls back to the older 468-point model (iris synthesised) and to 9 geometric channels if models are missing. One-Euro smoothing. |
| Live capture | `face_live.cpp/.h`, `video_reader.cpp` | Worker thread, ~15 fps on the owner's camera, neutral-pose + blendshape baseline calibration (1.5 s median), recording to `.tbdface` (v2 stores the 52 weights). |
| Mesh auto-fit | `face_fit.cpp/.h` | Software-rasterises the head from the front with its textures, runs the tracker on that picture, lifts the 468 landmarks back onto the mesh. Also `RenderHeadView(verts, parts, yaw, bmp)` for offline renders at any yaw. |
| Rig | `face_map.h` (header-only, `FaceRig`) | Landmark-driven morphs: each of the 52 channels is a recipe over landmark groups with Gaussian falloff. Jaw = hinge rotation. **Mouth splice**: seam from inner-lip landmarks, lower triangles tear away from upper ones, appended cavity (roof/floor/back wall), individual teeth, gums, tongue. **Eyelids**: skin-coloured shells over the eyeballs that swing shut. Eye-look rotates the eyeball parts. Smooth normals recomputed per pose. Manual mode = per-channel gain/enable (`.tbdfacemap` files). |
| Tab UI | `face_live_tab.cpp/.h` | GDI page: webcam + landmarks, live 3D viewport embedded in the middle panel (or a labelled landmark map), 52-row channel list (mouse wheel), Auto/Manual, mirror (default on). |
| Engine glue | `src\main.cpp` | `FaceTabHost` callbacks, `ReplaceEntityVertices`, material parts, camera framing. `src\mocap\face_driver.cpp` = head rotation (`FaceHeadRotation`, mirror). |
| Rendering | `src\render\renderer_d3d12.cpp`, `src\shaders\mesh.hlsl`, `src\mesh_import.h`, `src\image_decode.h` | D3D12 textures + mips, normal-map sampling via screen-space tangents (no stored tangents), alpha cutout, glTF material import (WIC decodes PNG/JPEG). |
| Models | `bin\models\face\*.onnx` | Converted from Google's `face_landmarker.task` (Apache 2.0) with our own converters in `gameenginetools\face_mocap_tests\` (`convert.py`, `convert_landmarks.py`; they need a Python venv with `tflite onnx ai-edge-litert onnxruntime numpy`; originals in `third_party\mediapipe_dl\`). Converted outputs match TFLite to ~1e-6 (blendshapes) and ~4e-4 px (landmarks). |
| Tests/harnesses | `gameenginetools\face_mocap_tests\rig_test.cpp`, `fit_test.cpp` | Offline: import `WesternGameIOS\export\body\body_head.glb`, fit, pose the rig, write BMPs. See "How to test". |
| Remesher | `gameenginetools\face_remesher\` | Separate standalone isotropic remesher (not wired in). |

Test asset: `C:\custom windows apps\gameengine\testproject\Test\Scenes\wyatt_face.tbdscene` (head = `C:\custom windows apps\WesternGameIOS\export\body\body_head.glb`: 3 primitives, skin + two eye parts, skinned but we ignore skinning, has colour textures, **no normal map, no hair**). Sample video: `testproject\Test\face_demo.mp4`.

## Known gaps / suspected weaknesses (verify, don't trust)
1. **Tracking quality**: ~15 fps live; detection found a face in only 80% of the demo clip's frames; no per-person range calibration beyond the 1.5 s neutral median; blendshape model was trained on real irises/images, our input mesh points come from our own ROI crop; mirror/left-right swap and pitch/yaw/roll signs were fixed by trial, not proven.
2. **Deformation**: soft Gaussian morphs from guessed amplitudes (fractions of head width), not authored/transferred shapes. Eyelid skin still lighter than surrounding skin; teeth/tongue are crude shapes; no wrinkles/normal-map blending; neck cut is jagged in the source mesh; only tested on Wyatt's head.
3. **Cost**: every frame the whole mesh is re-created on the GPU (`DestroyMesh` + `CreateMesh`), CPU Pose is O(touched vertices x channels).
4. **Rendering/content**: no normal map for Wyatt, no hair, Vulkan untextured, tangents derived per pixel.
5. **Robustness**: failure cases (bad lighting, glasses, beard, other cameras, face partly out of frame, heads that are not +Z-facing or have no neck/ears), model-not-found paths, first-fit time (~1-2 s freeze).
6. **Productisation**: licences/notices (see below), installer, docs, per-user privacy messaging, saving the fitted rig with the scene.

## Constraints and house rules
- **Licensing**: only Apache/MIT/BSD-style dependencies. MediaPipe models (Apache 2.0) and ONNX Runtime (MIT) are fine; keep notices. Do **not** adopt non-commercial models (FLAME/DECA/EMOCA etc.). Record the source and licence of anything you add (also fix the missing provenance note in `bin\models\face\`).
- **Do not screenshot or click the user's real screen** (an earlier screenshot captured a private Discord call). Verify visuals with the offline renderer (`RenderHeadView`) or other in-process output only.
- **Do not kill or overwrite the user's running `tbd_engine.exe` without asking**; the build fails if it is running. Ask them to close it.
- Source files here use **CRLF** line endings; the Edit tool is fine, but shell `sed`/node patches must preserve them. Keep new code matching the surrounding style (terse comments explaining why).
- Never fabricate results: report what you measured, what you only reasoned about, and what failed.
- Webcam data stays local; keep it that way.

## How to build and test
```
cd "C:\custom windows apps\gameengine"
.\build.bat                         (closes nothing: stop tbd_engine.exe first)
.\bin\facemocap.exe testproject\Test\face_demo.mp4 out.tbdface --models bin\models\face     (headless tracker on a video)
bin\tbd_engine.exe --scene "testproject\Test\Scenes\wyatt_face.tbdscene" "testproject\Test"  (the editor)
```
Offline rig renders: compile `gameenginetools\face_mocap_tests\rig_test.cpp` with `src\mocap\face_fit.cpp` and `face_tracker.cpp`, include paths `src` and `third_party\onnxruntime\include`, link `onnxruntime.lib user32.lib gdi32.lib ole32.lib` (copy `onnxruntime*.dll` and the models next to it). It writes `rig_<case>.bmp`; convert to PNG to look at them.

## What to produce
1. **Measure first**: tracker fps and per-stage timings, detection rate, jitter, latency (camera -> mesh), per-frame CPU/GPU cost of the deformation and mesh re-upload, first-fit time. Numbers, not impressions.
2. **Quality review** with evidence (offline renders, clip statistics): which channels look wrong or weak, which landmark recipes need new amplitudes, where the splice/eyelids/teeth fail, how the system behaves on a second, different head (use any other head mesh you can find or generate; report if none).
3. A **prioritized improvement plan** (impact vs effort, risks, licence implications), each item with how it would be verified. Candidates worth evaluating: GPU morph targets/compute skinning instead of re-uploading, per-person calibration flow, tracking at higher fps or smarter ROI reuse, blendshape-driven normal-map/wrinkle blending, better teeth/tongue/eyelid assets, hair, saving the fitted rig with the scene, support for imported ARKit-shape heads, a retargeting path for skinned/bone rigs, Vulkan parity, packaging for sale.
4. Optionally prototype the **top one or two** items if they are small and safe, with before/after evidence. Ask before anything large or irreversible.

Keep a short running log of what you tried, so the next person can continue.
