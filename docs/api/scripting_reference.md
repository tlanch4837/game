# Scripting reference

Generated from the Engine API Registry (`tbd_script_test.exe --dump-docs`). Do not edit by hand.

## tbd.log

- `tbd.log.info(text: string)`  (since 1.0)
  Writes a line to bin/engine.log and the editor console.

## tbd.scene

- `tbd.scene.count() -> number`  (since 1.0)
  Number of entities in the scene.
- `tbd.scene.find(name: string) -> entity`  (since 1.0)
  Index of the entity with this exact name, or nil.
- `tbd.scene.find_all(prefix: string) -> number[]`  (since 1.0)
  Indices of every entity whose name starts with the prefix (for example "Doors/").
- `tbd.scene.name(entity: entity) -> string`  (since 1.0)
  Name of an entity, or an empty string for a bad index.
- `tbd.scene.position(entity: entity) -> vec3`  (since 1.0)
  World position of an entity.
- `tbd.scene.rotation(entity: entity) -> quat`  (since 1.0)
  Rotation of an entity as a quaternion.
- `tbd.scene.scale(entity: entity) -> vec3`  (since 1.0)
  Scale of an entity.
- `tbd.scene.set_position(entity: entity, position: vec3)`  (since 1.0) [play only]
  Moves an entity. Only takes effect in Play; Stop puts it back.
- `tbd.scene.set_rotation(entity: entity, rotation: quat)`  (since 1.0) [play only]
  Rotates an entity (quaternion x y z w). Only takes effect in Play.
- `tbd.scene.set_scale(entity: entity, scale: vec3)`  (since 1.0) [play only]
  Scales an entity. Only takes effect in Play.

## tbd.input

- `tbd.input.down(key: number) -> bool`  (since 1.0)
  True while a key is held, in Play. Key is a Windows virtual-key code or the character code of 'A'..'Z'.

## tbd.player

- `tbd.player.feet() -> vec3?`  (since 1.0)
  Position of the walking player's feet, or nil when there is none.

## tbd.world

- `tbd.world.hour() -> number`  (since 1.0)
  Time of day in hours, 0..24.
- `tbd.world.set_hour(hour: number)`  (since 1.0)
  Sets the time of day in hours, 0..24.

## tbd.character

- `tbd.character.set_skin_detail_scale(entity: entity, scale: number) -> bool`  (since 1.1)
  How many times the skin pore detail normal repeats across this entity's skin UV atlas (default 48; 0 or less resets to 48, 1 = untiled). Returns false for a bad entity index.

## tbd.audio

- `tbd.audio.play(name: string, position: vec3, volume: number?, pitch: number?)`  (since 1.0)
  Plays a one-shot sound at a world position. Volume and pitch default to 1.

## tbd.test

- `tbd.test.check(ok: bool, message: string?)`  (since 1.0)
  Records a pass or fail. A failure is printed with the message and makes the exit code nonzero.
- `tbd.test.check_near(actual: number, expected: number, eps: number, message: string?)`  (since 1.0)
  Passes when two numbers differ by at most eps.
- `tbd.test.add_entity(name: string, position: vec3) -> number`  (since 1.0)
  Adds an entity to the fake world at a position. Returns its index.
- `tbd.test.set_player(feet: vec3?)`  (since 1.0)
  Places the walking player's feet. Call with no argument to remove the player.
- `tbd.test.play()`  (since 1.0)
  Loads Scripts/manifest.luau, starts Play and runs on_start of every global script.
- `tbd.test.load(path: string) -> bool`  (since 1.0)
  Loads one script file (relative to Scripts) as a global script.
- `tbd.test.start()`  (since 1.0)
  Runs on_start on every loaded script (use after tbd.test.load instead of tbd.test.play).
- `tbd.test.step(seconds: number)`  (since 1.0)
  Advances the game by this many seconds at a fixed 1/60 s step.
- `tbd.test.attach(entity: entity, path: string, props: string?) -> bool`  (since 1.0)
  Attaches an entity script to a fake-world entity; props is "key=value;key=value".
- `tbd.test.write_script(path: string, text: string)`  (since 1.0)
  Writes a file under the Scripts folder (a test edits a script, then calls tbd.test.reload).
- `tbd.test.remove_script(path: string)`  (since 1.0)
  Deletes a file under the Scripts folder.
- `tbd.test.remote(line: string) -> string`  (since 1.0)
  Runs one pipe line (for example script.call|world|set_hour|[7]) through the same code the editor's pipe uses. Returns 'OK <reply>' or 'ERR <reply>'.
- `tbd.test.props(path: string) -> string`  (since 1.0)
  The declared props of a script as 'name:type:default[:min][:max]' joined by ';' (what the inspector shows).
- `tbd.test.list_scripts() -> string`  (since 1.0)
  Script files the inspector's Add menu offers, joined by ';'.
- `tbd.test.script_exists(path: string) -> bool`  (since 1.0)
  Whether a script file exists under Scripts.
- `tbd.test.last_skin_scale() -> number`  (since 1.0)
  The scale the stubbed engine last received from character.set_skin_detail_scale.
- `tbd.test.reload() -> number`  (since 1.0)
  Runs the hot-reload check now. Returns how many scripts were reloaded.
- `tbd.test.set_marker(text: string)`  (since 1.0)
  Records a string a test can read back with tbd.test.marker.
- `tbd.test.marker() -> string`  (since 1.0)
  The last string recorded with tbd.test.set_marker.
- `tbd.test.log_count(text: string) -> number`  (since 1.0)
  How many log lines so far contain this text.
- `tbd.test.eval_fails(source: string) -> bool`  (since 1.0)
  True when running this source raises an error.
- `tbd.test.stop()`  (since 1.0)
  Stops Play: on_stop runs, coroutines are cancelled.
