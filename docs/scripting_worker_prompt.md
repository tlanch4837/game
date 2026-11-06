# Worker prompt: implement the scripting system

Paste everything below the line into the worker. It is self-contained. Give one worker one milestone at a time (M0 to M4); do not hand out M5.

---

You are implementing the scripting system for a custom Windows game engine (C++, Direct3D 12 and Vulkan) in `C:\custom windows apps\gameengine`. The engine is written and extended mainly by AI, so everything you build must be easy for another AI worker to use correctly, hard to misuse, and fast to get feedback from.

## 0. Read first (in this order, before writing any code)

1. `docs/scripting_spec.md`: the full design. **Where it says "Lua 5.4", use Luau instead** (decision below). Everything else in it stands.
2. `src/game_api.h`, `game/game.cpp`, `src/hot_reload.h`, `docs/hot_reload.md`: the existing C++ gameplay DLL and its hot reload. It stays and must keep working.
3. `src/remote_control.h` and `gameenginetools/engine_mcp/` (`tbd_mcp.py`, `README.md`): the named-pipe command server and the MCP tools built on it.
4. `src/main.cpp` around `g_game.Tick(...)` (line ~2371), `g_game.Init(&GameHost())` (~3973), and the Play start/stop code (~3645 to 3660, `g_gamePlaySnapshot`).
5. `src/scene.h` (the `Entity` struct and `Scene`), `src/play_mode.h`, `src/player_character.h`, `src/ai/npc_system.h`.
6. `HANDOFF.md` and `AIchat.txt` (the shared AI-to-AI log; read it, append to it when you finish, sign as `Claude OVOA (<your model>)`).

## 1. Decisions already made (do not reopen them)

- **Language: Luau** (the typed Lua dialect from Roblox, MIT licence), vendored as source in `third_party/luau`, built with the project. It has a static type checker (`luau-analyze`) and a sandbox. Keep the licence file in the folder.
- **C++ stays** for the engine and for performance-critical gameplay (`game.dll`). Luau is for everyday gameplay: doors, quests, triggers, dialogue, spawning, tuning.
- **No binding library** (no sol2 or similar). Build the **Engine API Registry** described in the spec, section 4. It is the single source of truth for what is callable.
- Only Apache, MIT or BSD licensed code. Do not add anything else.
- Every renderer-facing feature must work on both Direct3D 12 and Vulkan. Scripts never see backend objects: expose backend-neutral calls only. If you touch rendering code, you must implement both backends.
- Scripts run on the main thread, inside the existing frame order. Never call script code from worker threads.

## 2. The point of the design (keep this in your head)

Adding a future engine feature to scripting must be: write the C++, add `TBD_API_FUNCTION` / `TBD_API_EVENT` registration lines in one `api_<area>.cpp`, build. Nothing else. That one registration must automatically produce:

1. the Luau binding (`tbd.<module>.<name>`),
2. the name-based C call interface for `game.dll` (so the frozen `TbdGameHost` struct never grows again),
3. a pipe/MCP command (`script.call`) with the tool list taken from the registry,
4. generated reference docs and Luau type definitions (`docs/api/scripting_reference.md`, `Scripts/tbd_api.d.luau`),
5. a generated smoke test.

If your registry design does not deliver all five from one declaration, it is wrong. Prove it by adding one throwaway function and watching all five appear, then removing it.

## 3. Work breakdown

Do only the milestone you were assigned. Each is done only when its "Done when" is demonstrated, not claimed.

### M0: Luau in the build
- Vendor Luau into `third_party/luau` (source plus `LICENSE.txt`). Add it to `build.bat` (and check `build_game.bat` and `spirv.bat` are unaffected).
- Add `src/script/luau_smoke.cpp` or equivalent that creates a state, runs `print("hi")` and returns.
- Done when: the engine builds clean from `build.bat`, and `bin/engine.log` shows a Luau-printed line on startup behind an env var `TBD_SCRIPT_SMOKE=1`.

### M1: Registry, Luau binding, hooks, headless test mode
Create:
- `src/script/api_registry.h/.cpp`: registry with module, name, typed signature, doc string, `since` version, thread rule, availability (`always | play_only | editor_only`). Supported types: bool, int, float, string, Vec3, Quat, Color, EntityId, AssetRef, array<T>, table, optional<T>, callback. No raw pointers across the boundary.
- `src/script/luau_host.h/.cpp`: owns the Luau states, loads scripts from the project's `Scripts/` folder, runs hooks, error handling.
- `src/script/api_*.cpp`, one per area, porting the existing host functions: `api_log`, `api_scene` (find, find_all(prefix), entity name/position/rotation/scale get and set), `api_input` (key down), `api_player` (feet), `api_world` (hour get/set), `api_audio` (play).
- Hooks: `on_load(reloaded)`, `on_start`, `on_update(dt)`, `on_fixed_update(dt)` at 60 Hz, `on_late_update(dt)`, `on_stop`, `on_event`. All optional. Entity scripts (per-entity `self`, declared `props`) and global scripts (`Scripts/manifest.luau`, load order). Coroutines with `tbd.wait`, `wait_until`, `wait_event`, `wait_frames`; managed coroutines are cancelled on entity destroy or Play stop. Events: `tbd.events.emit/on/off`, depth limit 16.
- Wire into `main.cpp` at the Play start/stop and the frame where `g_game.Tick` runs. Keep Play/Stop transform restore behaviour exactly as now.
- Scene format: entities get a `scripts` list (path + prop overrides) saved in `.tbdscene`. Old scenes without it must still load, and saving must not break old readers where avoidable (check `scene_io.h`).
- **Headless test mode**: `bin\tbd_engine.exe --script-test <file.luau> [--scene <path>]` runs the script against a scene with a fixed time step, no window, no GPU if at all possible, exit code 0 on pass, nonzero on failure, results to stdout. Provide `tbd.test.assert`, `tbd.test.assert_near`, `tbd.test.step(seconds)`. This is the main feedback loop for other workers, so make it fast (under 2 s startup) and its output precise.
- Prove it by porting the swinging doors from `game/game.cpp` to `Scripts/door.luau` and writing `Scripts/tests/door_test.luau` that checks a door swings and settles. Leave the C++ version in place.
- Done when: the door test passes headless; Play in the editor runs `door.luau`; the generated reference doc exists; the throwaway-function proof from section 2 works.

### M2: Sandbox, limits, error isolation, hot reload
- Sandbox: Luau `luaL_sandbox`, open only safe libs, no `io/os/package/debug`; `require` limited to files under `Scripts/`, no `..`, no absolute paths. `AssetRef` and file arguments validated against the project root.
- Limits: per-frame script time budget (default 2 ms, configurable), instruction/interrupt hook to abort runaway loops, per-state memory cap (default 64 MB) with a custom allocator. A violation disables that script instance, logs it, and never hangs or crashes the engine.
- Error isolation: any script error logs `file:line`, the stack, and for a failed registry call the function's signature. The engine keeps running; the instance gets no hooks until its file is saved again.
- Hot reload: extend `hot_reload.h` to watch `Scripts/**/*.luau`, debounce ~100 ms, compile in a scratch state first (syntax error keeps old code and reports), then swap hooks while keeping each instance's `self`, call `on_load(true)`. Running coroutines finish on old code. Prop changes: new props get defaults, removed props dropped.
- Done when: the sandbox tests in spec section 14 all pass headless (infinite loop, huge allocation, `io.open`, `require("../x")`, deep recursion, error inside a coroutine, error inside `on_update`), and saving a script during Play changes behaviour without leaving Play and without losing `self` fields.

### M3: Pipe/MCP and the DLL handle API
- Add pipe commands in the existing `remote_control.h` flow (they run on the editor thread between frames, like current commands): `script.call|module|fn|<json args>` returning JSON, `script.eval|code`, `script.list` (the registry dump). `script.eval` only in editor builds, local pipe only.
- Update `gameenginetools/engine_mcp/tbd_mcp.py` so its script tools are built from `script.list` at startup; do not hand-write a tool per function.
- DLL side: add `tbd_call` plus lookup-by-name (returns a stable handle) to a new `src/script_c_api.h`. The existing `TbdGameHost` v1 table stays frozen and working; do not bump `TBD_GAME_API_VERSION` for existing DLLs. Provide one new example in `game/` that calls a registered function by name.
- Done when: through MCP (or a direct pipe call), `tbd.world.set_hour` is callable with no hand-written tool code, and a `game.dll` calls it via the handle API.

### M4: Editor integration and tooling
- Inspector: script slots on an entity, editable declared `props`, add/remove script. Console view of script logs with `file:line`. Per-script and per-hook timing in `profiler_overlay.h`.
- `TBD_SCRIPT_TRACE=1` logs every hook call.
- Generated `Scripts/tbd_api.d.luau` type definitions and a documented `luau-analyze` command line, so workers can type-check a script without running the engine. Add the check to the headless runner as an option (`--check`).
- Done when: a scene with 3 scripted entities saves, reloads, runs headless, and shows correct props in the inspector.

## 4. Rules for how you work

- **Use a branch or worktree.** `main` has many uncommitted changes from other work; do not commit on `main`, do not reset, stash, discard or reformat files you did not change. Do not touch files outside the scripting work unless the milestone requires it (`main.cpp`, `scene_io.h`, `hot_reload.h`, `remote_control.h`, build scripts are expected).
- **Build and test before saying anything is done.** Build with `build.bat`. Run the headless tests. Report only what you ran, with the actual output, including failures. If you did not run it, say so.
- **Engine processes:** always start with a timeout and close every `tbd_engine.exe` you start, even on failure. Never kill an instance you did not start. Do not take screenshots of, or click on, the owner's real screen.
- **Do not weaken tests to pass.** If a test fails, fix the code or report the failure.
- **Match the codebase.** Header-heavy C++ with `.inl` files, namespace `tbd`, short comments that explain why, no emojis, no marketing language. Keep new code readable by another AI with no context: small files, one area per file.
- **Do not guess APIs.** The registry's generated docs and the real headers are the truth. If a signature you need is missing, add it through the registry.
- **Never break existing behaviour:** the current `game.dll`, hot reload of shaders and DLL, Play/Stop restore, `TBD_PLAYTEST`, and existing scenes must keep working. Run the existing tests in `tests/` and the two scenes the owner uses (`full_map`, `full_world`) through a quick load if time allows.
- **Out of scope:** physics, animation state machine, particles, in-game UI, save system, gamepad, networking. Do not implement these. If you see a spot where they will plug in, leave a one-line comment pointing to the spec module name.
- **Security:** nothing from the pipe or from script files may reach the filesystem or process APIs outside the rules in section M2. The facial capture footage and `*.tbdface` files are private; do not read, upload or send them anywhere.

## 5. Report format (what to hand back)

1. Milestone, branch name, final commit hash.
2. What changed, as a short list of files and what each does.
3. The exact commands you ran and their results (build, headless tests, pipe calls). Paste real output, trimmed to what matters.
4. "Done when" checklist with pass/fail for each item and evidence.
5. Anything you could not verify, any deviation from the spec, and why.
6. Open problems or decisions the next worker or the owner must handle.
7. Append a short entry to `AIchat.txt`, signed `Claude OVOA (<your model>)`.
