# Life system: concept (draft 1, 2026-11-06)

Goal: citizens who read as people, not traffic. Someone you walk up to at 3 pm is where their day says they should be, in the mood their week gave them, and they remember what happened to them.

## Where we start

What exists (see `docs/ai_navigation.md`):
- Detour crowd NPCs on a tiled navmesh, three LOD tiers in effect: full (crowd agent + Jolt capsule + skinned rig), cheap (stored path, no body), and hidden (indoors).
- A routine table (`townsfolk, guard, loiterer, follower, skittish`), a day clock, reaction to the player. 256 NPC cap.
- `NpcInfo` on entities, Luau scripting spec, face mocap, impostor-card plan, road network that AI/traffic is meant to read.

What is missing for "alive" (the doc's own known-gaps list): no interiors ("indoors" = vanish), no variety (every NPC body is the same placeholder rig), no identity, no memory, no relationships, no needs, 12 skinned rigs because skinning is on the CPU.

## What CS2 does, and where it stops feeling alive

CS2 citizens are real simulated entities (household, job, school, needs, a path, a mode of travel), but the player only ever sees them as cars and tiny figures. The depth is statistical: happiness is a weighted score of services and pollution, health and education are bars, and a citizen's story is not observable. There are no personalities, no friendships, no memory of events, no visible mood, no conversations, no interiors, no individual appearance you could recognise twice. Our difference is that the player can stand next to the simulation, so the depth has to be visible at human scale.

## Core principle: one record, many views

Every citizen is always a **record** (data). Where they are is a function of the record and the clock. Being drawn is only a view of that record.

```
Statistical  (all citizens)    record only; schedule says where; coarse tick (minutes of game time, bucketed)
Scheduled    (nearby-ish)      position along a route, no body; reacts to events cheaply
Embodied     (near the player) crowd agent + capsule + rig + smart-object use + conversation
```
Promotion and demotion must reconcile: promoting derives position/pose/activity from schedule and state; demoting writes back what happened (new memory, mood change, relationship change, money spent). No "teleport and reset" when you look away. This is the single hardest requirement, and the thing CS2 does not need because it never zooms in.

## The new features (none of these are in CS2)

### 1. Identity you can recognise
- Stable name, age, face, body, voice pitch, gait, clothing wardrobe, 3-5 personality axes (e.g. sociable, careful, thrifty, temper, curiosity), a short backstory seed.
- Appearance comes from a parametric genome owned by the life system itself (body proportions, skin/hair tint, hair style, clothing set drawn from a small asset pool), so families look related and neighbourhoods have a look. Children blend parents' genome. No external character-creation tool is assumed.
- Needs a rig-variety path: shared skeleton, per-citizen mesh/material parameters, impostor cards at distance (`docs/impostor_cards_plan.md`), GPU skinning to lift the 12-rig cap.

### 2. Needs and drives (utility AI, not state machine)
- Needs: energy, hunger, hygiene, social, fun, comfort, safety, money pressure, purpose.
- Each tick (statistical) or second (embodied) a citizen scores candidate activities by need urgency x personality weight x distance x time-of-day fit x who is involved. The current `Idle/Wander/Patrol` states become outputs of this, not inputs.
- Mood is derived (needs + recent memories), and **readable on screen**: posture, walking pace, head-down vs. head-up, gesture frequency, what they carry.

### 3. Places, outdoors only (no walkable interiors)
- Decision: no interiors. A citizen entering a building walks to its door and goes inside (hidden), exactly like CS2. The building keeps an occupancy record (who is in it and since when) so state stays consistent.
- Activities that stay visible happen outdoors at **outdoor slots**: bench, bus stop, stall, bar patio, well, doorstep, porch chair, queue spot. Each has an interaction point, animation, capacity and duration.
- Cheap cue, optional: lit windows at night driven by occupancy.

### 4. Schedules with texture
- Calendar (weekday, weekend, market day, holiday, season), shift work, sick days, late for work, long lunch, Friday drinks.
- Variance per person (a seeded jitter plus personality), so no 8:00 crowd that moves in lockstep.
- Weather and light change behaviour: umbrellas, running for cover, shade-seeking, fewer people out in the cold, everyone out for the festival.

### 5. Relationships and households
- Graph: family, partners, friends, coworkers, neighbours, rivals, acquaintances (weighted, with a history).
- Relationships drive schedules and routes ("meets Ana at the cafe on Thursdays", "avoids the Hendersons' street").
- Households share money, a home, a vehicle, and decisions (who picks up the kid).

### 6. Episodic memory and gossip
- Each citizen keeps a small ring buffer of memories: what, where, when, who, valence, salience. Events (a fire, a robbery, a new park, a rude player, a death) write memories to witnesses.
- Gossip: memories propagate through the relationship graph with decay and distortion, so an event changes mood across a neighbourhood hours later, and you can trace why a street turned against the mayor.
- Reputation: the player is a remembered entity with per-person and per-district standing.

### 7. Ambient social behaviour you can watch
- Pair and group conversations (they face each other, take turns, gesture, laugh), greetings that depend on relationship, queues, crowds at a stall, children playing, dog walkers, people sitting and people-watching.
- Face mocap clips become the performance library for expressions and gestures; the animation set (sit, eat, smoke, carry, sweep, sleep, wait, shop, argue) is a content need, listed below.
- Barks and short ambient lines selected from state (mood, topic, weather, time), not random.

### 8. Life stages and generations
- Birth, school, first job, partnership, moves, children, retirement, illness, death, funerals, inheritance of home and traits. A citizen's age is visible (body, gait, clothing), and a street changes over a decade.
- Notable-life tracking: the sim flags interesting people (longest-tenured shopkeeper, the family that has had three bad winters).

### 9. Individual economy tied to the city
- Wage, rent, savings, debt, spending patterns by class and personality. Money pressure is a need that bends everything else (skipping meals, a second job, moving, crime).
- City policy (rent, tax, wages, prices, service access) reaches the individual through that pressure, with a visible chain of causes.

### 10. Cause and effect you can inspect
- Click a citizen: current activity and why (top scoring options), mood breakdown, recent memories, key relationships, money, schedule. This is both a player feature and the main debugging tool.
- City-level aggregates are computed from the individual records (not the other way round), so a happiness number is always explainable by people.

### 11. Optional: grounded conversation with the player
- A citizen in conversation can answer from their own record (job, mood, memories, rumours, who they know), using a template engine first and an LLM later as an opt-in layer. The record is the source of truth either way, so nobody invents facts the sim does not hold.

### 12. A chronicle
- An event log with notable citizens and story seeds ("the baker's daughter left town after the flood") surfaced as a feed, which is the long-session hook and also a great way to test that the sim produces stories.

## Engine work this implies

| Area | Why | Notes |
|---|---|---|
| `CitizenStore` (SoA, hot/cold split) | 100k+ records | Hot: id, cell, activity, state, needs (packed), next-event time. Cold: identity, memory, graph edges. Time-sliced buckets. |
| World clock and calendar | Schedules | Today only `sun.hour`; add day, week, season, holidays, weather, and a speed multiplier decoupled from render. |
| Outdoor slots / smart objects | Visible activities | Authoring in the editor (like the N/P/I NPC keys, but a proper panel), saved in scenes. |
| Building occupancy records | Consistent indoors state | Replaces vanishing; no interior geometry. |
| Rig variety + GPU skinning + impostors | Visible individuals | Current CPU skinning caps rigs at 12; shared skeleton, per-citizen params. |
| Animation set and retargeting | Activities | See content list. |
| Social layer (conversations, queues, groups) | Visible sociality | Group walking, personal space, formation steering on top of Detour crowd. |
| Event bus and memory | Gossip and reactions | Events carry location, radius, valence; witnesses selected spatially. |
| Luau hooks | Moddable behaviour | Activities, traits, events scriptable per `docs/scripting_spec.md`. |
| Save/load | Persistence | Records are the save; embodiment is rebuilt. |
| Inspector UI | Player and debug | Why-panel, relationship view, timeline. |
| Road/traffic integration | Commutes by mode | Lane data from the road network (`road_system_plan.md` section 7); vehicles carry real citizens. |

## Content needs (animation and art)
Sit (chair, bench, stool, ground), eat/drink, sleep, talk (idle/gesture sets), laugh, argue, wave/greet, carry (box, bag, child), sweep/work loops per trade, queue, smoke, read, phone, umbrella/rain walk, cold hunch, elder gait, child run, mourn. Clothing variants per weather/job/class. A voice/bark bank if audio is wanted.

## Roadmap (each phase should be playable and measurable)

0. **Data and tick**: `CitizenStore`, world clock/calendar, spawn from a generator, 100k records ticking inside a frame budget. Test: no frame spikes, deterministic from seed.
1. **Schedules + needs + places**: utility AI over a handful of building types (home, work, shop, bar). Statistical tier only plus the existing cheap/full tiers reading it. Test: a day graph of where people are.
2. **Outdoor slots + occupancy**: benches, stops, stalls, queues, building occupancy records, optional lit windows. Test: follow one citizen through a full day.
3. **Visible variety**: genome-driven appearance, rig variety, GPU skinning/impostors.
4. **Social**: relationships, conversations, groups, greeting logic.
5. **Memory + gossip + reputation**: events, propagation, inspector why-panel.
6. **Life stages + economy**: ageing, households, money pressure, moves.
7. **Chronicle and dialogue**.

## Platform: iOS (decision: this runs on iPhone/iPad)

This changes the plan from "as much as possible" to "inside a budget".
- **Scale:** target a walkable-scale town, roughly 2k-10k citizen records, not 100k+. Hot record around 64 bytes; budget a few MB for the store, and keep memory records small and capped per citizen (for example 8-16).
- **Compute:** the statistical tier runs time-sliced in small buckets with a fixed per-frame millisecond budget (about 1 ms at 60 fps, less on thermal pressure). No per-citizen per-frame work outside the embodied tier. Drop the sim rate when the device throttles.
- **Embodied cap:** far fewer full NPCs than desktop (think 15-25 bodies, 8-12 crowd agents). CPU skinning is out; **GPU skinning and impostor cards are required**, not optional. Share meshes and materials, vary by per-instance parameters.
- **Graphics API:** iOS means Metal. The engine has D3D12 and Vulkan backends only, so a Metal path (or MoltenVK) is a prerequisite for anything to run there. The life system itself must stay renderer-agnostic and talk to the renderer through instance data only.
- **Scripting:** Luau runs interpreted on iOS (no JIT allowed), so keep hot paths in C++ and use scripts only for activities, traits and events.
- **Libraries:** Recast/Detour and Jolt are plain C++ and portable; check build settings, and rebuild the navmesh cache offline or in the background to protect load times.
- **Lifecycle:** the app can be backgrounded or killed at any time. Save = the citizen records plus the clock; embodiment is rebuilt on resume. Catch-up after a long absence is a coarse statistical fast-forward, capped, not a replay.
- **Input and UI:** touch only. The "why" inspector is tap-to-select with a bottom sheet, not a desktop panel. No hover, no keyboard debug keys.
- **Battery/heat:** the sim runs at a lower tick when the camera is idle or far from people; the chronicle and gossip features are cheap because they work on records.
- This is another reason for the no-interiors decision.

## Open questions for you
1. **Scale and camera**: with iOS as the target, I recommend a town of a few thousand citizens (Port Lazare / Perdition scale). Confirm, or tell me if you need more.
2. **Setting and era**: Western frontier (current content) or modern? It decides the needs list, the economy, and the animation set.
3. **Player role**: mayor/builder, on-foot character, or both? Reputation and conversation features only matter if you are on the ground.
4. **LLM dialogue**: acceptable as an optional layer, or templates only?
5. **Determinism**: should the sim be fully deterministic from a seed (replays, testing, multiplayer later)? Recommended yes, and it constrains how randomness is used.
