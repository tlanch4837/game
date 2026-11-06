# cityengine

Fork of `gameengine` (engine only, no projects/assets) for an iOS city simulation game.

## What was copied
- `src/` engine source, `game/`, `tests/`, build scripts, `docs/*.md` + `docs/api`
- `third_party/`: JoltPhysics (MIT), luau, recastnavigation

Left out: `testproject`, `Assets`, `experiments`, `gameenginetools`, `bin*`/`obj*`, MB-Lab, onnxruntime, mediapipe.

## iOS gaps to resolve (the engine is Windows-only today)
- Renderers are Direct3D 12 and Vulkan. iOS needs Metal: either a new Metal renderer, or Vulkan through MoltenVK (shaders are HLSL -> SPIR-V -> MSL).
- Windowing/input/audio in `src/main.cpp`, `src/audio.cpp` are Win32: need UIKit/touch replacements.
- Build is MSVC `.bat`: iOS needs CMake + Xcode (macOS required).
- Strip character/face/mocap/hub code (`src/character`, `src/mocap`, `src/hub`) as not needed for a city sim.
- City-sim needs: top-down camera, grid/roads, agents, economy, touch UI.
