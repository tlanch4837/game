# Scripting system spec

Status: M0, M1, entity scripts in .tbdscene (S| lines), hot reload of .luau, the pipe/MCP commands and the inspector Scripts section (add, remove, edit declared props) are built, in Luau rather than Lua 5.4 (see docs/api/scripting_reference.md). Not yet: the DLL handle API, script console and profiler panels, generated smoke tests.

## 1. Goal

Gameplay is written in a small, safe, hot-reloadable language. Any engine feature, today or added later, becomes callable from scripts by registering it once. Nobody edits the script runtime, the binding layer or the docs to expose a new feature.

What exists now: `bin/game.dll` (C++, plain-C `TbdGameHost` table of 11 functions, `TBD_GAME_API_VERSION 1`). It works, but every new engine feature means a new struct field, a version bump and a DLL rebuild, and writing doors in C++ is slow for designers. The DLL stays (section 11); the script layer sits beside it.

## 2. Decisions

| Question | Choice | Why |
|---|---|---|
| Language | **Lua 5.4** (MIT), plain C API, vendored in `third_party/lua` | Small, fast to embed, coroutines suit game flow ("walk there, wait, say line"), meets the Apache/MIT/BSD rule. |
| Binding library | **None** (no sol2). Own thin registry (section 4) | One registry feeds Lua, the C++ DLL, the remote pipe, docs and tests. sol2 would bind only Lua and slow compiles. |
| LuaJIT | Not now | Stalled upstream, no Lua 5.4 features. Revisit only if a profile says scripts are the bottleneck. |
| Hot paths (physics step, animation eval, door springs at scale) | Stay in C++ (engine or `game.dll`) | Scripts decide, C++ crunches. |
| Backends | Scripts never see D3D12 or Vulkan | Rendering features are exposed as backend-neutral calls, so backend parity is automatic. |

## 3. Architecture

```
 script (.lua) ──┐
 game.dll (C++) ─┼──► Engine API Registry ◄── engine modules register themselves
 remote pipe/MCP ┘        (one table of typed functions, events, components)
                              │
                              ├─► generates: Lua bindings, C ABI table, docs/api/*.md, MCP tool list, tests
```

New files:

- `src/script/api_registry.h/.cpp`: the registry (section 4). No Lua dependency.
- `src/script/lua_host.h/.cpp`: owns the Lua states, loads scripts, runs hooks, hot reload.
- `src/script/api_*.cpp`: one file per engine area (`api_scene.cpp`, `api_input.cpp`, `api_audio.cpp`, ...). Each registers its functions at startup.
- `scripts/` (inside the project, e.g. `testproject/WesternGame/Scripts/`): the game's `.lua` files.
- `tools/gen_api_docs` (small, part of the build): writes `docs/api/scripting_reference.md` and `Scripts/tbd_api.d.lua` (editor stubs) from the registry.

## 4. Engine API Registry (the extension point)

Every callable thing is described once:

```cpp
// src/script/api_registry.h  (sketch)
TBD_API_FUNCTION(scene, find_entity,
    "Entity id for a name, or nil",
    (Ctx& c, std::string_view name) -> std::optional<EntityId>) { ... }

TBD_API_EVENT(scene, entity_spawned, "(EntityId)");
TBD_API_COMPONENT(Health, "hp:number, max:number");   // scriptable per-entity data
```

A registration records: **module** (`scene`, `physics`, `anim`, `audio`, `ui`, `ai`, `save`, ...), **name**, **typed signature**, **doc string**, **since-version**, **thread rule** (main-thread only unless marked), **availability** (`always`, `play_only`, `editor_only`).

Supported types: `bool`, `int`, `float`, `string`, `Vec3`, `Quat`, `Color`, `EntityId`, `AssetRef` (string path, validated), `array<T>`, `table` (string keys to the above), `optional<T>`, `callback`. No raw pointers cross the boundary.

Everything else derives from the registry:

1. **Lua**: `lua_host` walks the registry and builds `tbd.<module>.<name>` tables. Argument and return conversion is generated from the signature. A bad argument raises a Lua error naming the function and parameter.
2. **C ABI for `game.dll`**: the registry exports `tbd_call(const char* module, const char* fn, const TbdValue* args, int n, TbdValue* out)` plus a lookup-by-name that returns a stable handle. DLLs resolve what they need at `onLoad` and never depend on struct layout, so adding an API no longer bumps `TBD_GAME_API_VERSION`. The 11 existing `TbdGameHost` functions stay as a frozen compatibility shim.
3. **Remote pipe / MCP**: a `script.call|module|fn|json-args` command (and `script.eval`) on the existing pipe, so Claude and tools can call any registered function and read back JSON. The MCP server lists tools from the registry dump instead of a hand-kept list.
4. **Docs and editor stubs**: regenerated every build; CI fails if a registration has no doc string.
5. **Tests**: each registered function gets a generated smoke test (call with default args in a headless scene).

### Recipe: expose a new engine feature (this is the "ready for future updates" promise)

1. Write the C++ feature normally.
2. In its own `api_<area>.cpp`, add `TBD_API_FUNCTION` / `TBD_API_EVENT` lines. Nothing else.
3. Build. It is now callable from Lua, from `game.dll`, from the pipe/MCP, appears in the reference docs, and has a smoke test.

Rule for engine PRs: a feature that gameplay might drive is not done until step 2 exists. For rendering features, expose settings and intent (`tbd.fx.spawn("dust_puff", pos)`, `tbd.camera.set_fov`), never D3D12/Vulkan objects.

## 5. Script model

### 5.1 Script files and entity scripts

- A script is a Lua file that returns a table of hooks: `return { on_start = ..., on_update = ..., on_event = ... }`.
- **Entity scripts**: an entity has a `scripts` list (paths + per-instance property overrides), saved in the `.tbdscene`. Declared properties show in the inspector:

```lua
-- Scripts/door.lua
local Door = {}
Door.props = {
  open_angle = { type = "number", default = 1.75, min = 0, max = 3.14 },
  locked     = { type = "bool",   default = false },
}
function Door:on_start()        self.angle = 0 end
function Door:on_update(dt)     ... self.entity:set_rotation(...) end
function Door:on_interact(who)  if self.locked then tbd.ui.toast("Locked") return end ... end
return Door
```

- **Global scripts** (game systems: quest manager, day/night, spawner) listed in `Scripts/manifest.lua` in load order.
- Each script instance gets a private `self` table (`self.entity`, `self.props`, user fields). `self` is what hot reload preserves (section 8).

### 5.2 Hooks (all optional)

| Hook | When |
|---|---|
| `on_load(reloaded)` | script (re)loaded; `reloaded` is true after a hot reload |
| `on_start()` | Play starts, or the entity spawns during Play |
| `on_update(dt)` | every frame in Play, variable dt |
| `on_fixed_update(dt)` | fixed 60 Hz step (physics-coupled logic) |
| `on_late_update(dt)` | after animation/physics, before render (cameras) |
| `on_stop()` | Play stops or the entity is destroyed |
| `on_event(name, ...)` | a subscribed event fires (also `self:on("name", fn)` for ad hoc subscriptions) |
| `on_collision(other, point, normal)`, `on_trigger_enter/exit(other)` | when physics exposes them |
| `on_interact(who)`, `on_damage(amount, source)` | gameplay events, defined by scripts and the registry's events |

### 5.3 Coroutines for flow

```lua
function Npc:on_start()
  self:run(function()                -- self:run starts a managed coroutine
    tbd.wait(2.0)
    self.entity:walk_to(tbd.scene.find("Saloon_Door"))
    tbd.wait_until(function() return self.entity:arrived() end)
    tbd.audio.play("line_howdy", self.entity:position())
    tbd.wait_event("player_entered_saloon")
  end)
end
```

Managed coroutines are cancelled when the entity is destroyed or Play stops. `tbd.wait`, `wait_until`, `wait_event`, `wait_frames` are the only yield points. Engine async work (scene load, navmesh build, asset stream) returns a handle the script can `await`.

### 5.4 Events

Two layers, one API: `tbd.events.emit(name, ...)`, `tbd.events.on(name, fn)` / `off`, plus the registry's typed `TBD_API_EVENT`s (engine to script). Engine events are queued and delivered on the main thread at a fixed point in the frame. Script events dispatch immediately, with a recursion depth limit of 16.

## 6. API surface, version 1

Everything below is a registry module. What exists in the engine today is **built** in M1; the rest are **planned** modules, listed so the names are reserved and consistent. A planned module appears the day its engine feature lands (recipe in section 4).

| Module | Contents | Source today |
|---|---|---|
| `tbd.log`, `tbd.time` | `info/warn/error`, `dt`, `time`, `frame`, `scale`, `pause` | host `log` |
| `tbd.scene` | `find`, `find_all(prefix)`, `spawn(prefab, xform)`, `destroy`, `count`, entity methods: `name`, `position/rotation/scale` get/set, `set_visible`, `parent` | host entity functions; spawn/destroy need `scene.h` work |
| `tbd.input` | `down(key)`, `pressed`, `released`, `axis(name)`, `mouse_delta`, action map, gamepad | host `keyDown`; gamepad has 1 hit, needs XInput work |
| `tbd.player` | `feet()`, `position`, `teleport`, `set_controls_enabled` | host `playerFeet`, `player_character.h` |
| `tbd.world` | `hour/set_hour`, weather, `time_scale` | host `getHour/setHour`, `sun.h` |
| `tbd.audio` | `play(name, pos, vol, pitch)`, `play_2d`, `music`, `stop(handle)`, buses | host `playSound`, `audio.h` |
| `tbd.camera` | `set_fov`, `shake`, `look_at`, `cut_to(entity)`, `blend` | camera.h |
| `tbd.physics` | `raycast`, `overlap_sphere`, `add_force`, `set_velocity`, bodies, triggers, ragdoll toggle | `physics.cpp`; Jolt is vendored |
| `tbd.anim` | `play(clip)`, `crossfade`, state machine parameters (`set_param`), `on_anim_event` | `skeletal_anim.h`; state machine is future work |
| `tbd.ai` | `set_routine`, `walk_to`, `arrived`, `patrol`, `perceive`, `navmesh_path` | `src/ai/*` |
| `tbd.fx` | `spawn(name, pos)`, `attach`, decals | particles do not exist yet |
| `tbd.ui` | `toast`, `dialog`, `choice`, HUD widgets, menu push/pop | in-game UI does not exist yet |
| `tbd.save` | `get/set(key)`, `slot_save/load`, versioned schema | save system does not exist yet |
| `tbd.quest`, `tbd.inventory` | Written in Lua first, in `Scripts/lib/`; promoted to C++ only if needed | none |
| `tbd.debug` | `draw_line/sphere/text`, `watch(name, value)`, `assert` | editor_draw.h |
| `tbd.fs` | Read-only access to project data files (`data/*.json`) | none |

## 7. Sandbox and safety

- Lua states open only `base` (minus `dofile`, `loadfile`, `load` with binary chunks), `string`, `table`, `math`, `coroutine`, `utf8`. No `io`, `os`, `package`, `debug` (a debug build can enable `debug`).
- `require` is replaced by a project-rooted loader: only files under `Scripts/`, no `..`.
- An instruction-count hook plus a per-frame time budget (default 2 ms for all scripts, configurable) aborts a runaway script, logs the stack, and disables that script instance until reload. The engine never hangs on a `while true`.
- Memory: custom allocator with a per-state cap (default 64 MB); exceeding it kills that script, not the engine.
- A script error never crashes the engine. It logs `file:line` + stack to `bin/engine.log` and the editor console, and the instance stops getting hooks until the next save.
- `AssetRef` and file arguments are validated against the project root; scripts cannot name arbitrary paths.
- Scripts come from the project folder only. The remote `script.eval` command is restricted to the local pipe (already `PIPE_REJECT_REMOTE_CLIENTS`) and only enabled in editor builds.

## 8. Hot reload

Extends `src/hot_reload.h` (already watches `game/` and shaders). Watch `Scripts/**/*.lua`.

1. File changes, so debounce ~100 ms, then compile the chunk in a scratch state. A syntax error is reported (file:line) and the old code keeps running.
2. On success, for each live instance of that script: keep `self` (user fields), replace the hook table (methods) via the new module's metatable, call `on_load(true)`. Fields are kept, code is swapped. No `on_start` rerun unless the script asks (`Door.reset_on_reload = true`).
3. Coroutines: running coroutines continue with the **old** code until they finish; new ones use the new code. (Simple and predictable; a "restart coroutines on reload" flag per script for dev.)
4. Declared `props` changes: new props get defaults, removed props are dropped, inspector refreshes.
5. Stopping Play restores transforms as it does today (`g_gamePlaySnapshot`); script `self` state is discarded on Stop, and `tbd.save` data is not touched.

## 9. Threading and performance

- All scripts run on the main thread, inside the existing frame order: input, `on_update`, `g_game.Tick` (DLL), physics, `on_fixed_update`, animation, `on_late_update`, render. (Exact insertion points: `main.cpp` near line 2371, where `g_game.Tick` runs.)
- The registry marks functions `thread_safe` only if trivially so; everything else is main-thread. Engine worker threads that need a script callback post to the event queue.
- Budget: 1000 scripted entities updating at 60 fps within the 2 ms budget on the dev machine. Scripts that miss it should batch (one manager iterating a list, not 1000 `on_update`s) or move that loop into C++.
- Cheap by default: entities with no `on_update` are never iterated; `on_update` can be throttled per instance (`Door.update_rate = 10`).
- Registry call overhead target: under 1 µs for scalar calls. Vec3/Quat are userdata with cached metatables, with no table allocation on the hot path (`entity:position_into(v)` variants exist for loops).

## 10. Tooling

- **Editor**: a Script panel in the inspector (list scripts on the entity, edit `props`, "open in editor" via `mcp__ccd_host__open_in_editor` equivalent / shell open). Console view of script logs with file:line links.
- **Debugging**: `tbd.debug.watch` values shown in the profiler overlay; `TBD_SCRIPT_TRACE=1` logs every hook call; a REPL on the pipe (`script.eval|code`) for live poking.
- **Editor stubs** (`tbd_api.d.lua`) and the generated reference give completion and signatures in VS Code (lua-language-server).
- **Profiler**: per-script and per-hook time in `profiler_overlay.h`.
- **Headless test mode**: `tbd_engine --script-test <file.lua>` runs a script against a scene with fixed dt and asserts (`tbd.assert`), for CI. The existing `TBD_PLAYTEST` input scripts (`player_character.h` `PlayerScript`) become Lua over this mode.

## 11. Relationship to `game.dll`

Both stay supported.

| Use Lua for | Use `game.dll` for |
|---|---|
| Quests, dialogue, triggers, doors, pickups, UI flow, AI behaviour choice, tuning values | Tight simulation loops, heavy math, anything profiled as hot, anything needing C++ libraries |

The DLL gains `tbd_call`/lookup access to the same registry (section 4.2), and can call Lua functions through `tbd.script.call("module", "fn", ...)`, so a C++ system and a Lua quest can talk. `game/game.cpp`'s doors are the first candidate to port to `Scripts/door.lua` as the M1 proof (the C++ version remains as the performance comparison).

## 12. Versioning and compatibility

- `tbd.api_version` = `major.minor`. Adding functions bumps minor. Removing or changing a signature bumps major and needs a deprecation cycle.
- Each registration has `since` and optional `deprecated` ("use X instead", removal version). Deprecated calls log a one-time warning with the caller's `file:line`.
- Scripts declare `-- requires tbd >= 1.2` (checked on load).
- Saved scenes store script paths and prop overrides only, never engine pointers, so they survive API additions.
- `TbdGameHost` v1 table stays frozen; new DLLs use the registry handle API.

## 13. Milestones

| M | Deliverable | Done when |
|---|---|---|
| **M0** | Vendor Lua 5.4, check the licence file into `third_party/lua`, add to `build.bat`, run a hello script | `tbd.log.info("hi")` prints in `engine.log` |
| **M1** | Registry + Lua binding + generated docs. Port current host functions (log, scene, input, player, world, audio). Global scripts, entity scripts, hooks, coroutines, events | `door.lua` reproduces `game.cpp` door behaviour in Play; reference doc generated |
| **M2** | Sandbox, instruction/time/memory limits, error isolation, hot reload with `self` preserved | Infinite loop in a script does not hang the editor; saving a script changes behaviour live without leaving Play |
| **M3** | Pipe/MCP: `script.call`, `script.eval`, tool list from registry; `game.dll` registry handle API | Claude can call `tbd.world.set_hour` through MCP without any hand-written tool |
| **M4** | Editor: inspector script slots and props, console, profiler, `--script-test` headless mode | A scene with 3 scripted entities round-trips through save/load and runs headless in CI |
| **M5+** | Modules as engine features land: physics (ragdoll/triggers), anim state machine, `fx` particles, `ui`, `save`, gamepad | Each ships with its `api_*.cpp` in the same change |

## 14. Test plan

- Registry unit tests: type conversion round trips (Vec3, Quat, optional, array, table), bad-argument errors, since/deprecation behaviour.
- Generated smoke test per registered function, run headless.
- Sandbox tests: `while true do end`, huge allocation, `io.open`, `require("../x")`, deep recursion, error inside a coroutine, error inside `on_update`: engine stays alive, instance is disabled, log has `file:line`.
- Hot reload tests: edit a hook, edit a prop, introduce a syntax error, delete the file, rename it; `self` fields survive; running coroutines finish on old code.
- Play/Stop: transforms restored, script `self` reset, `tbd.save` untouched.
- Perf test: 1000 entities with `on_update` and a mix of `scene` and `physics` calls under the 2 ms budget.
- Backend parity: the same scripted scene run with `--api=d3d12` and `--api=vulkan` produces identical script logs (scripts never touch backend-specific state).

## 15. Open questions for the owner

1. **Lua versus a typed alternative** (Luau is MIT, adds types and a better sandbox, at a bigger integration cost). Default here is plain Lua 5.4. Say so if you want Luau.
2. **Who writes scripts**: only you and the AIs, or also non-programmers? That decides how much editor UI (visual props, node-graph later) is worth building in M4.
3. **Multiplayer in future?** If yes, the registry should mark functions `authority = server | client` now, since retrofitting is painful. It costs one field today.
4. **Mod support?** If players should ship mods, the sandbox rules (section 7) and a mod folder loader become requirements, not nice-to-haves.
