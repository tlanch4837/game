# Hot reload

The engine picks these changes up while it runs (code in `src/hot_reload.h`):

| You change | What happens |
|---|---|
| a file in `src/shaders/` | Direct3D 12 reloads the shaders right away. The SPIR-V is recompiled in the background (`spirv.bat`), then a Vulkan renderer restarts in place and keeps the scene. **F5** does the same by hand. |
| a file in `game/` | `build_game.bat` rebuilds `bin/game.dll` in the background, and the new DLL is swapped in. Its state is kept. **Ctrl+F5** rebuilds by hand. |

If a compile or build fails, the engine keeps running the old code and writes the errors to `bin/engine.log`. Shader errors also go to `obj/spirv_reload.log` and gameplay errors to `obj/game_build.log`.

## Gameplay DLL

- Interface: `src/game_api.h`. The game only uses plain C types, and the engine provides a host table with these functions: log, entities (find, get or set transform), keys, player position, sun hour, sound.
- Keep all state inside the memory block passed to `onLoad`, because it survives every swap. If you change the struct layout, bump `kLayout` in `game/game.cpp`.
- `setTransform` only works during Play, and Stop puts every entity back where it was.
- If the game crashes, the engine unloads the DLL and keeps running. The next build loads it again with fresh state.
- Vulkan reload needs `VULKAN_SDK` set, because it uses dxc. The gameplay build needs Visual Studio 2022.
- The existing player, NPC and play-mode code is still built into the exe. Code you move into `game/` becomes hot-reloadable.
