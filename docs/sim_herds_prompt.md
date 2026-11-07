# Prompt: neighbourhoods + herds + visible children (first slice)

Paste this whole file as the prompt for one new session.

## Context
`cityengine` is a custom C++ engine (D3D12 + Vulkan, iOS/Metal port in progress) for a Cities: Skylines II–style city
builder. iPhone 15 is the floor: no per-frame allocations, few draw calls, sim work under the scheduler's per-frame budget.
Read `docs/city_sim_design.md` first: it is the agreed design. This task builds its **build order steps 1–4 as a first
slice**. Existing pieces to reuse, not rewrite:
- `src/city/sim.h` scheduler, `sim_cells.h` per-cell counters + home→job supercell links (`ForEachLink`), `Match`
- `src/city/sim_flow.h` flow solver (worker thread in `city_sim.inl`), `sim_agents.h` cosmetic cars (one instanced draw per model)
- `src/city/sim_people.h` tracked people (`Materialize`), `road_network.h`, `road_names.h`
- tests in `tests/sim_*_test.cpp` + `.bat` (copy an existing `.bat`)

## Rules
1. Own git worktree + branch cut from `sim-gameplay-pass` (e.g. `sim-herds`). Don't commit to other branches; don't push.
2. Don't edit Track R files (`city_buildings.inl`, `mesh_lod.h`, `render/*`, `shaders/*`, `world_streaming.inl`,
   `surface_async.inl`) or platform-split files (`renderer.h`, `stats.h`, `play_mode.h`, `undo*.h`, `audio.cpp`,
   `city_industry.inl`, `log.cpp`). New code in new files; hooks into shared files as one-line call sites, listed in the report.
3. No Win32 in `src/city/*` or `src/gui/*`. Timing via the scheduler / `std::chrono`.
4. Headless sim code in `src/city/*.h` with a test for each new header. Host glue in a new `src/city_herds.inl`.
5. Commit per step with the repo's attribution line.

## Pseudocode

```
// ---------- step 1: neighbourhoods (src/city/sim_hoods.h)
struct Hood { id; name; cells[]; neighbours[]; centroidNode;
              pop[AGE_GROUPS]; workers; jobs[3]; schoolSeats[2]; }
BuildHoods(net, citySim):                       // on road-network change only
    graph = blocks as nodes, edges where blocks touch without crossing a major road / rail / water
    flood-fill into clusters of ~6–20 blocks; merge tiny ones
    name = NameGen(seed, hood.id)               // reuse road_names.h vocabulary
    hood.neighbours = clusters sharing a border; centroidNode = nearest road node
RollUp(hoods, citySim):                         // every 10 game min, O(cells)
    zero totals; for cell: hood[cellHood[cell]] += cell counters

// local-first matching, reused for jobs / school seats / services
MatchLocalFirst(demand[hood], supply[hood], hoodTravelTime):
    for h: take from own supply
    for ring in 1..3: for n in neighbours-by-travel-time(h, ring): take from n's spare supply
    leftover -> unmet[h]                         // shows as unemployment / no seat

// ---------- step 2: herds over the day (src/city/sim_herds.h)
DemandCurve[TRIP_TYPE][96]                       // 15-min windows; commute out 6–9, home 15–18, school, shop
struct Herd { from, to; type; size; departWindow; route; progress; speedMean; speedSd; }
ReleaseHerds(window):                            // scheduler system, period 15 game min
    for (h, j, n) in hoodJobLinks: size = n * DemandCurve[COMMUTE][window] * carShare
        if size >= 1: herds.push({h, j, COMMUTE, size, window, RoutePick(h, j)})
    school run: households with kids * dropoffShare -> herd to the assigned school hood, then chain to work
AdvanceHerds(dtMinutes):                         // per tick, O(herds)
    for herd: speed = LinkSpeed(herd.route.current) ; herd.progress += speed * dt
        on link end: next link (lookup) ; on arrival: dissolve into destination lots by job share
        add herd.size to linkLoad[link][window]
LinkSpeed(link) = freeSpeed / (1 + 0.15 * (load / capacity)^4)   // BPR curve

// ---------- step 3: routes (src/city/sim_routes.h)
RouteTree(destHood): one Dijkstra backwards from destHood.centroidNode on a worker -> nextSeg[node]
    LRU cache of trees for active destinations; drop all on net.version change
Alternatives(h, j): up to 3 = k-shortest with penalty on reused segments (highway 1, highway 2, local)
Highway pipe: a run of highway segments = one link; travel time = sum LinkSpeed; nobody simulated inside
RoutePick(h, j): logit over current travel times of the alternatives, smoothed: share = lerp(share, new, 0.3) per window

// ---------- step 4: visible children + promotion (src/city/sim_children.h)
SpawnChildren(camera, budget=300):                // per frame, no allocation (fixed pools)
    for link within 600 m of camera: want = herdsOn(link).size * density scale
        spawn/despawn to match want; child = { herdId, slot = PickTenantSlot(herd), lane, s }
    look/car/personality = Hash(citySeed, slot.address, slot.index)
StepChildren(dt):                                 // IDM car-following, per frame near camera
    v0 = LinkSpeed(link) * personality.speedFactor
    a  = aMax * (1 - (v/v0)^4 - (sStar(v, dv)/gap)^2) ; v += a*dt ; s += v*dt
    intersections: stop line + gap acceptance; emergency vehicle behind: lateral offset, rejoin
Promote(child):                                   // watched: centre radius >= 1 s; max 32 promoted
    bind to slot; own route from RouteTree(slot.job hood) to slot.job lot; on arrival park in lot or nearest strip
    offscreen: arrival time = herd timing with its personality; on click: SimPeople::Materialize(slot)
Draw: one DrawInstancedTransient per car model (as sim_agents.h); parked cars as static instances
```

## Tests
- `tests/sim_hoods_test`: clustering is stable for the same roads; every cell in exactly one hood; local-first matching
  fills own hood first, spills to neighbours by travel time, leaves the rest unmet.
- `tests/sim_herds_test`: herd sizes over a day sum to the link counts × car share; morning flow points to job hoods, evening
  reverses; BPR slows a loaded link; arrivals spread with speedSd > 0; deterministic for the same seed.
- `tests/sim_routes_test`: route tree next-hops reach the destination; cache invalidates on net.version; logit split moves
  toward the faster alternative and does not flip-flop over 20 windows.
- `tests/sim_children_test`: IDM never collides and queues behind a stopped car; same slot → same look and car; spawn counts
  track herd load; no allocation after warm-up (count pool size).
- All existing `tests/sim_*_test.bat` still pass.

## Verify in game
Build with `build_city.bat`; run with `TBD_CITY_GROW=400` and the remote pipe (`sim|...` commands exist in
`city_overlays.inl`; add `sim|herds` stats and `sim|follow`). Screenshots to `shots/`: morning rush into the office area,
evening reverse, a jammed highway with cars diverting to the alternative, a promoted car parked at its office.
Log sim cost per frame (`TBD_SIM_LOG=1`); target under 1 ms average on desktop at 400 lots.

## Report
What was built per step, hooks added to shared files (file and line), what was stubbed, measured cost.
