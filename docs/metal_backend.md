# Metal backend (iOS)

`src/render/renderer_metal.mm` implements `tbd::Renderer` (`src/render/renderer.h`) with Metal through
[metal-cpp](../third_party/metal-cpp) (Apple's 26 release), drawing into a `CAMetalLayer`. It follows the Vulkan backend
(`renderer_vulkan.cpp`) feature for feature, except hardware ray-traced ambient occlusion.

> **Status: written on Windows, not yet built or run.** Metal can't be compiled on Windows. What was checked here:
>
> * Every shader converts to MSL (`msl.bat`: 40 of 40), and the binding rules in `tools/msl.ps1` pass.
> * `renderer_metal.mm` and `src/platform/apple/log_apple.cpp` pass the clang front end (via `clang-tidy`, `-Wall -Wextra
>   -Wshadow`, no diagnostics) against the real metal-cpp headers, with stub Apple system headers. That run was C++ with
>   blocks, not Objective-C++. It catches type and signature mistakes in metal-cpp calls, but not mistakes in how Metal
>   behaves at run time.
> * The Windows build still compiles with the `renderer.h` change: `main.cpp`, both Windows backends and `log.cpp`
>   compiled with MSVC `/W4`, no errors.
>
> Not checked: the `.metal` files have not been compiled by `xcrun metal`, the CMake project has not been configured, and
> nothing has run on a device. See [What needs a Mac](#what-needs-a-mac).

## Files

| File | What |
|---|---|
| `src/render/renderer_metal.mm` | The backend. It is the only translation unit that defines `NS_/MTL_/CA_PRIVATE_IMPLEMENTATION`. Compiled with `-fno-objc-arc`. |
| `src/render/renderer.h` | Now portable: on non-Windows `HWND` is `void*` (the `CAMetalLayer*`), and `TraceNowMs` uses `std::chrono`. Adds `CreateMetalRenderer()`. |
| `msl.bat`, `tools/msl.ps1` | HLSL → SPIR-V (`spirv.bat`, dxc) → MSL 2.3 for iOS (`spirv-cross --msl --msl-ios --msl-version 20300`). |
| `src/shaders/msl/*.metal` | The converted shaders. They are committed, so a Mac build needs no Vulkan SDK. Run `msl.bat` again after changing any `.hlsl`/`.hlsli`. |
| `CMakeLists.txt` | Generates an Xcode iOS project. It compiles the `.metal` files into `default.metallib` and also copies them into the bundle as a fallback. |
| `src/platform/ios/main_ios.mm`, `Info.plist.in` | Smoke-test app: a UIKit scene with a full-screen `CAMetalLayer` and a procedural triangle. |
| `src/platform/apple/log_apple.cpp` | Apple versions of `Log`, `ExeDirectory`, `LogCounts` and `UploadSteps::Done`. The log goes to stderr and to `Documents/engine.log`. |

Build on a Mac:

```
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
cmake --build build-ios --config Release -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO
```

The second command gives an unsigned `.app` for Sideloadly. To sign in Xcode instead, pass `-DTBD_TEAM_ID=...`. The
deployment target is iOS 16 because `recommendedMaxWorkingSetSize` and `CAFrameRateRange` need it.

## Binding scheme

The HLSL is not changed. The Vulkan `[[vk::binding(n, set)]]` numbers become Metal slots by fixed rules (`tools/msl.ps1`,
mirrored in `namespace slot` in the backend):

| Vulkan | Metal | How |
|---|---|---|
| set 0, binding *N* (buffer / texture / sampler) | `[[buffer(N)]]` / `[[texture(N)]]` / `[[sampler(N)]]` | `--msl-decoration-binding`, set 0 kept discrete. The backend binds the same numbers the Vulkan backend writes. |
| set 1 (mesh material: albedo 0, normal 1, sampler 2, surface 3, extra 4) | **argument buffer** at `[[buffer(1)]]`, members `[[id(binding)]]` | `--msl-argument-buffers --msl-argument-buffer-tier 0 --msl-discrete-descriptor-set 0` |
| push constants (`pc`, `wc`) | `[[buffer(29)]]` | spirv-cross picks the slot itself; `msl.ps1` rewrites it to 29. Sent with `set{Vertex,Fragment}Bytes`. |
| vertex stream 0 (`Vertex` / `UiVertex`) | buffer 30 | vertex descriptor; `[[attribute(n)]]` = the Vulkan location |
| vertex stream 1 (instance rows, or a skinned mesh's previous vertices) | buffer 28 | vertex descriptor, per instance or per vertex |
| compute: 4 reads, 3 writes | buffers 0–3, 4–6 | as Vulkan bindings 0–6; constants at 29 |

`msl.ps1` fails the conversion if a shader uses buffer 28 or 30, puts anything but push constants at 29, or goes past
30 textures or 15 samplers.

Per pipeline family:

* Mesh: light buffer at 0 and motion at 6 (vertex and fragment). Textures: shadow 1, tight 3, probes 4, cloud shadow 5.
  Comparison sampler at 2. Material at 1.
* Post: `PostParams` at 0. Textures 1–8 and 11–21 (`post.hlsl` `BIND(n)`). Samplers: clamp 9, wrap 10.
* Water: buffers post 0, light 1, waves 7, ripple 8, sky 9. Textures 2, 3, 4, 10, 11. Samplers 5, 6.
* UI: atlas at texture 0, sampler 1.

Entry points are renamed to the file stem (`mesh_vs`, `post_taa_ps`, `water_CSRipple`, …). That way every `.metal` links
into one `default.metallib`, and the backend asks for functions by the same names as the Vulkan `.spv` files.

**Material argument buffers have two layouts.** spirv-cross only declares the set-1 members that a shader uses:

* `mesh_ps`, `mesh_overlay_ps` and `mesh_motion_ps` declare all five.
* `mesh_shadow_cut_ps` and `mesh_veg_depth_ps` declare only albedo and the sampler.

`--msl-force-active-argument-buffer-resources` does not help, because dxc has already removed the unused globals. So the
backend keeps one `MTLArgumentEncoder` per layout. Each (albedo, normal, surface) triple gets one argument buffer per
layout it is used with, encoded on first use and cached like Vulkan's material descriptor sets. They live in 256-byte
slots of 256 KB shared pages. Binding one also calls `useResources` on its textures. The material sampler is created
with `supportArgumentBuffers`.

## What's implemented

Everything in the `Renderer` interface. Each part follows the Vulkan backend, with these Metal-specific differences:

* **Frames**: two in flight, paced by a dispatch semaphore that the command buffer's completion handler signals. The
  drawable is fetched as late as possible, just before the tone map.
* **Render passes** replace dynamic rendering:
  * The scene pass opens lazily. It clears only the first time in a frame and loads after a break (shadow, compute,
    deformable copies).
  * The tone map and the overlays share one render pass over the drawable.
  * Post passes that overwrite their whole target use `LoadActionDontCare`. The two temporal blends (probe blend,
    exposure) use `Load`.
* **Memoryless (tile-only) attachments**:
  * the capture's depth and second colour target (512²);
  * the window-sized overlay depth used when render scale is below 1.

  Both fall back to private memory if memoryless storage fails. The scene's depth, HDR colour and velocity
  ("G-buffer") are sampled by the post chain, so they cannot be memoryless. The same goes for the shadow maps.
* **Instancing**: per-instance vertex streams with `baseInstance`. Vegetation uses
  `drawIndexedPrimitives(indirectBuffer:)`, which reads the same five-uint record.
* **Depth pre-pass**: a depth-only pipeline with no fragment function, then the normal pipeline with a `GreaterEqual`
  test and no depth writes. Shadows set their bias per pass (`setDepthBias`) instead of per pipeline, so the sun map
  and the tight map share pipelines.
* **Sun shadow cache, lit capture, probes, clouds, AO (screen space), TAA, exposure, bloom, water, UI, skinned motion
  vectors, vegetation three-pass, deformable meshes, frame capture, texture self-test**: as in Vulkan.
* **Water** samples a blit copy of the scene depth (`depthCopy_`) while the real depth is its attachment. Metal must not
  sample a texture it is rendering to. For the same reason the post passes swap any texture that is the pass's own
  target for a 1×1 dummy.
* **Memory**: Apple GPUs share memory with the CPU.
  * Meshes, instance buffers and compute buffers are shared-storage buffers that the CPU writes once. So
    `CreateMeshNoWait` copies nothing on the GPU.
  * Textures are private (so they get lossless compression) and are uploaded by blit on a command buffer of their own.
    `NoWait` uploads don't wait; the queue's commit order runs them before the next frame.
* **Lifetime**: anything freed while a frame may still use it goes on a retire list keyed by frame serial, and is
  released once that frame completes (at most 2 ms of releases per frame). So `Destroy*` never stalls, where the Vulkan
  backend calls `vkDeviceWaitIdle`.
* **Shaders**: from `default.metallib`. If a function is missing there, `msl/<name>.metal` from the bundle is compiled
  at run time.

## Known gaps

* **Ray-traced AO**: not implemented, by design. `RayTracingSupported()` is false and `post_ao_rt_ps` is not converted
  (spirv-cross can emit `metal_raytracing` for it, but the backend has no acceleration structures). The AO pass is
  always the screen-space `AoPS`.
* **GPU timers**: only whole-frame GPU time (`GPUStartTime`/`GPUEndTime`), logged every 60 frames. The per-pass split in
  `GpuPasses()` stays zero; it would need `MTLCounterSampleBuffer` sampling at encoder boundaries.
* **`ReloadShaders`**: not supported (returns false), the same as Vulkan.
* **Compute threadgroup sizes**: hard-coded from the HLSL `[numthreads]` (CSPlace 16×16, the water kernels 8×8, the rest
  64), because MSL keeps no record of them. Keep them in step with `vegetation.hlsl` and `water.hlsl`.
* **App shell**: `main_ios.mm` renders a procedural triangle and pauses when the scene becomes inactive. Still to do:
  * touch input, game loop and world;
  * GUI, audio and asset loading;
  * memory warnings.

  The rest of the engine (city, ai, physics, script, gui) isn't in `CMakeLists.txt` yet. It joins as each part builds
  without Win32 (`src/main.cpp` is the Win32 shell it replaces).
* **Validation**: Metal API validation comes from the Xcode scheme (`MTL_DEBUG_LAYER`), not from Init's `validation`
  flag.

## What needs a Mac

In order:

1. **Compile the shaders**: `xcrun metal -std=metal3.0` on all 40 files (CMake does this). Watch for:
   * `spvDepthCast` (spirv-cross's reinterpretation of `texture2d<float>` as `depth2d<float>` for `SampleCmp` on the
     shadow maps);
   * the argument-buffer structs;
   * the fragment shaders that `discard` in a function returning outputs (`mesh_*_ps`, `water_ps`, `post_probeblend_ps`).
2. **Compile the backend** in Objective-C++ (`.mm`, `-fno-objc-arc`). The syntax check above ran as plain C++ with blocks.
3. **Configure CMake with the Xcode generator.** Check that the explicit shader SDK matches the Xcode destination,
   and that `msl/` and `default.metallib` land in the app bundle. See [iOS build](ios_build.md).
4. **Run with API validation on (`MTL_DEBUG_LAYER=1`) and the shader validation layer.** Check:
   * vertex descriptors against each function's attributes (the backend filters by `vertexAttributes()`);
   * pipelines without a fragment function that still declare colour attachments (pre-pass, vegetation depth);
   * that no texture is sampled while it is an attachment;
   * the argument-buffer encoding;
   * `useResources` residency.
5. **Correctness against the Windows backends**:
   * Depth bias units: Vulkan's constant 2000/1000 and slope 2.0/1.5 are reused as-is.
   * Y orientation: Metal is D3D-style, so the viewports are not flipped.
   * Reversed Z: clear 0, compare `Greater`.
   * Shadow comparison sampler: `LessEqual`.
   * The temporal blend states.
   * Indirect args.
   * The texture self-test (`TBD_TEXTURE_SELFTEST=1`).
6. **Hazards**: the backend relies on Metal's automatic hazard tracking (no heaps, every resource tracked). That
   covers compute → vertex/indirect reads and the deformable-mesh blits that overwrite a copy the previous frame read.
   Confirm with Xcode's GPU frame capture.
7. **Performance (Xcode GPU tools)**:
   * that the memoryless attachments really are tile-only;
   * load/store actions per pass;
   * render pass count per frame (scene breaks for shadow/compute cost a store and a load each);
   * the write-combined CPU cache mode on mesh buffers;
   * whether textures should move to `MTLHeap`s with `useHeap` instead of per-material `useResources`.
