# CS2 simulation roadmap (design + audit, 2026-11-08)

Branch base: `ios-metal` (the branch Codemagic ships to iPad). Every phase below is its own branch off `ios-metal` after
Phase 0 has merged, owns a **disjoint set of files**, and merges back on its own. Targets: iPhone 15 floor, the iOS presets
(Low/Med/High at 30 fps, Ultra at 40 fps), Metropolis scale (50k residents) as the reference city and 100k as the stress
city.

**Decision this doc asks for.** `docs/city_sim_design.md` (2026-11-07) says "no system ticks individual people" and plans a
cohort model. The new request is the whole CS2 simulation with individual citizens and households. This roadmap goes with
the new request: **every resident is a record** (about 24 bytes), ticked at life-event pace (once per game day, sliced),
never per frame. **Movement stays aggregate** (herds, flow), and only citizens near the camera get minute-by-minute logic.
That is cheap enough (see section 4.6) and it keeps the design doc's layer rule: herds city-wide, children on screen,
promoted when watched. If the user wants the cohort model after all, only Phase 1's store changes (the counters API it
feeds stays the same).

---

## 1. Audit: what really exists

| File | Real | Stub / missing |
|---|---|---|
| `sim.h` | Fixed-step scheduler: 1 tick = 1 game minute, per-frame budget (`budgetMs` 2 ms), backlog cap 240 min, systems with period + stagger slices, per-system timings. `SplitMix64`, `SimSeed`. Deterministic by tick number. | No calendar (day only, from `g_clock.minutes`). No months, seasons or years. No worker-thread systems. |
| `sim_cells.h` | Per 128 m cell counters: homes, jobs ×3 kinds, residents kids/adults/seniors, `working`, `employed`, edu ×3, wealth ×4, coverage[6], utility bits. Job matching on 4×4-cell supercells, own supercell then 2 rings. Who works where is kept only as supercell→supercell link counts (`byHome_` / `byJob_`). Fire / Quit are exact. Invariants are tested. | **Residents move in the moment a lot is built**, with a fixed 20/65/15 age, 30/50/20 education and 25/35/30/10 wealth split per lot (`Add`), and they leave the moment it goes. There is no ageing, no births, no deaths, no migration and no households. Education and wealth never change. |
| `sim_people.h` | A clicked resident is "materialised" from a slot (cell, slot) as a pure function of the counters. Up to 2000 named people. `LoseJob`, `MoveHouse`, `Sync`, a fixed 8–17 workday `Location()`. Saved as `Q` records. | Cosmetic: a person is invented from the counters, and nobody lives a life. |
| `sim_growth.h` | RCI(+Office) demand from unemployment, vacancies and residents' spending, smoothed over a day; it counts lots under construction. The spawn gate. Level up/down after N good/bad days, abandonment at L1 (demolition). Coarse land value per cell from the 3×3 cells around and from coverage. | Demand ignores rent, taxes per household, education levels and goods. No rent. Abandoned means demolished at once (no abandoned building state). `state_` (good/bad timers) is **not saved**. |
| `sim_economy.h`, `sim_budget.h` | Integer-cent treasury. Daily tax per resident by wealth band and per filled job, tax rates 0–30 % feed demand. Itemised upkeep, 3 loan tiers, 60-day history, bankruptcy warning, saved as a text field. | No household income or spending, no service fees, no policies, no districts, no import/export income. |
| `sim_coverage.h` | Utility flags per lot into cells. Service coverage = radius as the crow flies, limited to the same road component, linear falloff. Funding scales the radius. | **No capacity** (a clinic covers 10 or 10,000 people the same way), no travel time, no vehicles, no dispatch. Deathcare and comms cover nothing. No parks, post or telecom buildings in `TTab`. |
| `sim_flags.h` | 6 per-lot bits (no power, water or sewage, no workers, no customers, being abandoned) and an efficiency factor for tax. | Indexed by `zones.lots` vector order. No sick, crime, fire, garbage, goods or high-rent flags. |
| `sim_milestones.h` | 8 population milestones with unlock bits and rewards. | No XP, no development points. |
| `sim_tuning.h` | One X-macro list of tuning values plus `tuning.json` overrides. | One shared list: every phase that adds a value would conflict on it. |
| `sim_hoods.h` | Neighbourhoods (cells clustered and cut by major roads, rail and water), names, roll-up, hood→hood commute links from supercell links, `MatchLocalFirst`. | School seats are a stub (a kid in a school-covered cell gets a seat). |
| `sim_flow.h`, `sim_routes.h`, `sim_herds.h`, `sim_town_traffic.h`, `sim_children.h`, `sim_agents.h`, `sim_walkers.h` | The strongest part. CSR flow graph, BPR, logit loading and MSA on a worker. Link table with highway pipes, route trees (LRU), alternatives, logit route shares. 96-window demand curves, herds with speed spread and trip chains. Outside road connections (through traffic, commuters, visitors, trucks). Visible IDM cars with junctions and signals. Sidewalk pedestrians by pure schedule. | Herds are car trips only ("car share is already applied"). No modes, no transit riders, no goods herds from a production model. Walkers' and cars' people come from hashes, not from records. |
| `transit.h` | Lines (bus, tram, train, boat, plane, subway, passenger or cargo), stops, outside connections, vehicles driving their stops. | No riders, no mode choice, no fares, no ridership numbers. |
| `industry.h`, `city_industry.inl`, `city_garbage.inl` | Specialised-industry hubs with drawn areas and resource efficiency, producing tonnes into `stock`. Garbage produced per lot, collected by landfill, incinerator or recycling within range, piles up. | No chains (stock goes nowhere), no companies, no trucks from them, no import/export. Garbage piles are not saved. |
| `city_sim.inl` (host) | Registers cells (1), demand (10), growth (60/6), people (10), flow (5), economy (1440), services (10). Hoods (10), herds (15) and herdmove (1) are registered in `city_herds.inl`. Save: one `M|1|...` text line plus `Q` lines inside `.tbdgame`. A newer sim version than the build can read is ignored ("starts fresh"). | Economy, services, flow, people, HUD and save all live in this one 960-line file. No autosave. Cell counters are **rebuilt from lots on load** (only correct because residents are a pure function of the lots today). |
| Docs | `city_sim_design.md` (layers, herds, cohorts), `life_system_concept.md` (record/view tiers, iOS scale 2k–10k), `utilities.md`, `transport.md`, `cs2_feature_checklist.md`. | The checklist is out of date (economy, budget, milestones, utilities and coverage now exist). |

**Net:** the traffic side is close to CS2 in kind. The people side is a set of counters that never change once a lot is
built. Nothing produces a birth, a death, a graduate, a migrant, a pollution value, a crime, a fire, a patient or a
shipment.

## 2. Things in the existing code that block or endanger this plan

1. **The residents are a pure function of the lots** (`CitySim::Add` / `Remove`). Anything that changes a person
   (ageing, a death, a move) breaks "demolishing a lot takes away exactly what it brought", which `sim_cells_test`
   enforces. It also breaks rebuilding the counters from lots on load. Phase 1 has to make the counters a view of the
   records, and the records must then be saved.
2. **`city_sim.inl` is one file that every phase would edit**: services, economy, flow, people, save, HUD fill. The
   single unity build (`game_core.inl` includes every `city_*.inl`) makes it worse. Phase 0 splits it by owner before
   anyone branches.
3. **Per-lot arrays are keyed by `zones.lots` vector position** (`g_simLotUtil`, `g_simFlags`, the `lotUtil` arguments).
   Compaction moves positions. New per-building state must key on a stable id (Phase 0 `BuildingId`).
4. **No binary save, no autosave, no app-lifecycle save.** `.tbdgame` is a text file read line by line. 50k citizens as
   text is about 5 MB of parsing on the iPad. `src/game/shell.h` has no background or terminate callback, so iOS can
   kill the app and lose the session. Growth timers, garbage piles and herd state are not saved today.
5. **No calendar.** One game day is 80 real seconds at the fastest speed (18 game min/s). At real-time ageing a
   generation would take weeks of play. Phase 0 sets the life calendar: **1 game day = 1 life month** (tunable,
   `lifeDaysPerYear = 12`), so a year passes in about 16 minutes at speed 3. That is CS2's approach.
6. **Determinism gaps.** `ZoneGrid::rng` is a shared `std::mt19937` that is not saved, so spawns diverge after a load.
   `sim_flow.h` uses floats ("nothing the player owns is derived from flow yet"). Once commute time drives happiness and
   moving, flow output becomes gameplay input, so it must be quantised (whole seconds) before use.
7. **Service catalog has no capacity.** `BuildingDef` has only `workplaces` and `vehicles`. There are no `TTab` entries
   for parks, post or telecom (Comms exists but covers nothing). New service buildings need models, and per the user's
   rule models are previewed in HTML on localhost and approved before engine work. That gates the Phase 4 content.
8. **Herds are cars only.** Mode choice (Phase 5) has to split releases by mode inside `sim_herds.h`. Children and
   walkers read herd releases, so Phase 5 owns those three files together.
9. **Budget accounting covers only the scheduler.** `HerdsSync` (children), walkers and the info fill run per frame
   outside `g_sim.budgetMs`. The per-phase budgets below need the F3 profiler to show the total sim cost per frame,
   which Phase 0 adds.
10. **Counter widths.** `CellSim` uses uint16 with saturating adds: fine per 128 m cell (worst case about 2,600 homes),
    but nothing may sum city totals in uint16. Hood totals are uint32 already.

## 3. Target architecture

### 3.1 Tiers (one record, many views)

| Tier | Who | What runs | Rate |
|---|---|---|---|
| **Record** | every citizen, household, building, company | life events: ageing, school progress, job search, health, happiness, moving, birth, death | each entity once per game day, in 96 slices (one per 15-min window) |
| **Aggregate** | cells, hoods, districts | counters (`CellSim`, `Hood`) kept **incrementally** from record changes; fields (pollution, land value); demand; herds and flow | 10–60 game minutes, or on change |
| **Active** | citizens of buildings within ~350 m of the camera, at most 512 | a schedule state machine each game minute (home, commute, work, school, shop, leisure, sleep), bound to visible cars and walkers | every tick, only for the active set |
| **Promoted** | clicked or followed, at most 32 | the full trip: route lookups, parking, walk to the door; the info card shows needs and the reasons for them | per frame (existing `sim_children` promotion) |

Rule: aggregates are **derived from records**, never the other way round. The one exception is overflow: past the record
cap (120k), extra homes fill with "statistical households" that exist only in the counters, like today.

### 3.2 Data layout (SoA, handle-based)

`sim_ids.h` (Phase 0): `struct Handle { uint32_t v; }` = 24-bit index + 8-bit generation. Typed aliases `CitizenId`,
`HouseholdId`, `BuildingId`, `CompanyId`, `IncidentId`. `kNone = 0`. Free lists, and a slot is reused only after its
generation has been bumped.

**CitizenStore** (`sim_citizens.h`, Phase 1). Hot part, 24 B per citizen:
```
std::vector<uint32_t> household;   // HouseholdId
std::vector<uint32_t> work;        // BuildingId of the job, kNone if none
std::vector<int32_t>  bornMonth;   // life month of birth (age = (nowMonth - bornMonth) / 12)
std::vector<uint32_t> school;      // BuildingId of the school seat (Phase 4 writes), kNone
std::vector<uint8_t>  edu;         // 0 none, 1 elementary, 2 high school, 3 college, 4 university (CS2's levels)
std::vector<uint8_t>  jobLevel;    // edu level of the workplace slot held (0..4)
std::vector<uint8_t>  health;      // 0..255
std::vector<uint8_t>  happy;       // 0..255
std::vector<uint8_t>  flags;       // alive, female, student, retired, sick, criminal, pinned, leaving
std::vector<uint8_t>  gen;         // handle generation
std::vector<uint16_t> commuteMin;  // last known commute, whole minutes (from route costs)
```
Cold part, 8 B: `seed` (name, looks, personality; walkers and cars take their look from it) and `nextInHousehold`.
100k citizens come to 3.2 MB.

**HouseholdStore**, 32 B: `home` (BuildingId), `firstMember`, `size`, `adults`, `savings` (int32 whole currency),
`incomeMonth`, `rentMonth`, `spendMonth`, `carCount`, `flags` (looking to move, leaving), `movedInMonth`,
`unhappyMonths`. 40k households come to 1.3 MB.

**BuildingRegistry** (`sim_buildings.h`, Phase 0): one row per sim-relevant building of any origin (zoned lot, transport
or service `Placed`, `UtilBuilding`, `IndSite`). Columns: `origin`, `originId` (Lot::id / index), `kind`, `cell`,
`hood`, `entranceNode`, `level`, `homes`, `jobSlots[5]` (by education level), `alive`. It is rebuilt when the zone,
transport, utility or industry versions change. Rows are matched by origin id, so a BuildingId survives compaction and
loads.

**Occupancy** (Phase 1): `homesUsed[b]`, `jobsFilled[b][5]`, `firstHousehold[b]` with an intrusive list through the
households. Workplaces are counts per building per level; there is **no per-job record**. A citizen's job is
`(work, jobLevel)`.

**Fields** (`sim_field.h`, Phase 0): sparse 32 m tiles aligned to CityCells (4×4 samples per 128 m cell, uint8). One
field = 16 B per occupied cell. Air, noise, ground, water and land value: 80 B per cell, under 0.5 MB for any city.
`Diffuse(field, kernel, wind)` runs on a worker on a snapshot copy, and the result is swapped in on the main thread.

**Factors** (`sim_factors.h`, Phase 0): the one table that decouples the phases. A per-cell SoA of everything that
moves health, happiness, demand or land value, with all fields declared up front:
`air, noise, ground, water, crime, fireRisk, coverage[kServicesV2], telecom, parkAccess, rentBurden, commuteMin,
unemployment, goodsAvailable, taxBurden`. Each field has **exactly one writer phase** (named in a comment) and any
number of readers. A field nobody writes yet keeps its neutral default, so a reader phase can merge before its writer.
`PolicyMods` (multipliers, default 1.0) per hood or district lives here too.

**Events** (`sim_events.h`, Phase 0): a fixed ring of 4096 `SimEvent {type, minute, building, citizen, value}`. Types
cover birth, death, move-in, move-out, graduate, hired, fired, crime, fire, sick, disaster, bankrupt company, line
opened, and so on. Producers push, consumers (news, radio, chronicle, deathcare, dispatch) read by cursor. It never
allocates.

### 3.3 Scheduling

- The scheduler stays main-thread and deterministic. New systems register through `SimPhasesInit()` (Phase 0 hub), each
  phase in its own `city_sim_pN.inl`.
- Record systems use period 1440 with stagger 96: each slice visits `index % 96 == slice`. At 50k citizens that is
  about 520 citizens per slice, once per 15 game minutes.
- Heavy pure work (field diffusion, coverage by travel time, flow solve, route trees) goes to `g_engine.jobs` workers
  on immutable snapshots. Results are applied at the next tick boundary in the order they were requested, never by
  completion order, so the result does not depend on thread timing.
- Thermal: `budgetMs` comes from the preset (Low 1.0, Med 1.5, High/Ultra 2.0) and is halved at the "serious" thermal
  state. Under pressure the sim slips behind (existing backlog behaviour). It never drops record ticks unless the
  240-minute cap is hit.
- Randomness: `SplitMix64(SimSeed(citySeed, entityHandle, monthSalt))` per decision. Never a shared stream. Money is
  integer. Gameplay-relevant floats are quantised before they are stored.

### 3.4 Save format (`.tbdsim`, Phase 0)

The `.tbdsim` file sits beside `.tbdgame` and `.hfd`, binary, little-endian:
```
"TBDSIM\0\0"  u32 formatVersion(1)  u32 chunkCount
chunk: u32 fourcc  u16 version  u16 flags  u32 bytes  u32 crc32  payload
```
- Each phase owns its chunk ids (table below). A reader skips an unknown fourcc. A chunk whose version is newer than the
  build, or whose CRC is bad, makes **that system start fresh** and log it (today's `M` record behaviour, per subsystem).
- SoA vectors are written raw (count + bytes). A Metropolis save is about 6 MB, written in under 20 ms on the iPad.
- **Autosave** (Phase 0): every 10 real minutes, plus when the app goes to the background (a new `shell.h` lifecycle
  hook for iOS `applicationDidEnterBackground`). It writes to rotating autosave slots. The snapshot is a memcpy of the
  stores on the main thread (under 2 ms) and is serialised on a worker.
- **Old saves** (no `.tbdsim`): Phase 1 bootstraps records from the lots with the old fixed split (20/65/15 and so on),
  so existing cities load populated. `M` and `Q` lines keep being read. The `Q` people map onto bootstrapped citizens.

| Chunk | Owner | Contents |
|---|---|---|
| `HEAD` | P0 | sim minute, life month, city seed, record cap, autosave counter |
| `BLDG` | P0 | BuildingId ↔ origin id map (so handles survive the load) |
| `CITZ` `HHLD` `OCCU` `MIGR` | P1 | citizens, households, occupancy, migration accumulators, pins |
| `GROW` `LAND` `DMND` | P2 | lot good/bad timers (fixes the lost `state_`), land value, rent, demand |
| `POLL` `WTHR` `WATR` | P3 | pollution fields, weather and season state, water table and flood depth |
| `SERV` `INCD` `GARB` | P4 | service building state (enrolled, patients, bodies, vehicles), incidents, crime accumulators, garbage piles |
| `LINE` `MODE` `OUTC` | P5 | line ridership, fares, vehicles, mode shares, outside connection flows |
| `COMP` `GOOD` `TRAD` | P6 | companies, stocks, shipments in flight, trade balance |
| `BUDG` `PLCY` `DIST` `MILE` | P7 | budget (replaces the `B1` text field; still reads it), policies, districts, XP and unlocks |

### 3.5 Shared interface headers (owned by Phase 0, frozen after it merges)

| Header | Purpose |
|---|---|
| `src/city/sim_ids.h` | handles, generations, free lists |
| `src/city/sim_calendar.h` | `SimCalendar`: minute → day, life month and year, weekday, season (4 × 3 life months), daylight from `sun.h` |
| `src/city/sim_buildings.h` | `BuildingRegistry` (3.2) |
| `src/city/sim_field.h` | `Field<T>` on 32 m tiles, snapshot, diffuse, sample at a point or cell |
| `src/city/sim_factors.h` | `CellFactors`, `PolicyMods`, the writer of each field |
| `src/city/sim_events.h` | `SimEvent` ring and cursors |
| `src/city/sim_save.h` | `.tbdsim` writer and reader, CRC, chunk registry |
| `src/city/sim_tuning_table.h` | per-phase tuning structs via one X-macro template. Keys are prefixed (`life.`, `land.`, `env.`, `svc.`, `move.`, `goods.`, `gov.`) and every struct ignores the others' prefixes, so the phases never share a tuning list. The existing `SimTuning` stays as the unprefixed set. |
| `src/city/sim_flags.h` | widened to uint32 with **all bits reserved now** (sick, crime, fire, garbage, no goods, high rent, no school, no students, polluted, flooded, no telecom, no customers...). Each phase writes only its bits, in its own `ComputeXFlags`. Keyed by BuildingId. |
| `src/gui/sim_panels.h` | generic panel model: title, stat rows, bars, stacked bar, sparkline, list, slider rows. It is drawn by both HUDs (`city_hud.h` and Glass Dock+ bottom sheets, touch first). A phase only fills a `SimPanelSpec` in its own `.inl`, so no phase edits `glass_dock_host.inl` or `city_hud.h`. |
| `src/gui/city_overlays.h` | the `Overlay` enum extended with every planned layer (air, noise, ground, water pollution, crime, health, education, garbage, telecom, transit ridership, goods, districts). Each layer gets a per-cell scalar provider callback that its phase registers. |

#### The one calendar (decided 2026-11-09)

The weather's seasons ran on `SimCalendar` (a year of 12 game days) while both HUDs printed 30-day months of the
clock (a "January" lasted 30 game days, two and a half years of seasons). Decision: **there is one calendar, `SimCalendar`
of the sim minute, and every date the player sees is printed from it.**

- **1 game day = 1 life month**, 12 life months a year (tunable `lifeDaysPerMonth`, `lifeMonthsPerYear`). Ages, births,
  the monthly visits, the seasons, the weather chain and the HUD's month all count the same months.
- **The HUD date is month and year only**: `"14:05  Mar Y3"` (PC HUD), `"Mar · Y3"` (Glass Dock+), `"Mar Y3"` (the save
  list). A life month has no days to show; the time of day beside it is the clock's (the sun). The season chip and the
  month always agree (Dec-Feb winter, Mar-May spring, Jun-Aug summer, Sep-Nov autumn). `FormatSimDate` and
  `MonthNameAt` in `sim_calendar.h` are the only formatters.
- **The sim minute starts at the clock** on a new map (`SimOnCityReset`: Jan Y1 at 08:00), so the sim minute and the
  clock's time of day agree; a load restores the save's sim minute (M record). They differ only by the scheduler's
  backlog (at most a few minutes) or after a dropped backlog.
- Still on 30-day months: the budget panel's ledger stamps (`city_sim_budget.inl`, Phase 7's file) should switch to
  `FormatSimDate` when Phase 7 next touches it.

## 4. Phases

Each phase lists files it **creates** (new) and files it **owns** (the only branch allowed to edit them). A file not
listed is frozen to all phases. A needed change goes back through a small Phase 0 follow-up PR.

### Phase 0: Scaffold (lands first, small: about two days, one agent)

- **New:** every header in 3.5. `src/city_sim_phases.inl` (includes `city_sim_p1.inl` … `city_sim_p7.inl` and calls each
  phase's `Init / Frame / Save / Load / Panels`), seven empty `src/city_sim_pN.inl` stubs, `src/city_sim_save.inl`
  (`.tbdsim` and autosave), `tests/sim_save_test.cpp/.bat`, `tests/sim_calendar_test.cpp/.bat`,
  `tests/sim_bench.cpp/.bat` (a synthetic city of N lots runs one game day and prints ms per system; numbers × 2 are
  the iPad gate until calibrated on device).
- **Moves (pure, no behaviour change):** out of `city_sim.inl`, `SimServices` goes to `city_sim_services.inl` (later
  owned by P4), `SimFlow`/`SimBuildTrips`/agents go to `city_sim_flow.inl` (P5), economy, budget, milestones and the
  HUD budget fill go to `city_sim_budget.inl` (P7), and the demand and growth registrations go to `city_sim_growth.inl`
  (P2). `city_sim.inl` keeps the scheduler, cells, people, save hub and `SimSync` (P1).
- **Edits:** `game_core.inl` (one include line), `shell.h` plus the iOS shell (background hook), `city_game.inl` (calls
  the `.tbdsim` save and load beside `.tbdgame`), the profiler line for the total sim cost per frame.
- **Tests:** chunk round trip, an unknown chunk skipped, a newer version means fresh, a bad CRC means fresh; calendar
  boundaries; registry ids stable across a lots compaction.
- **UI:** the autosave toast; the F3 line "sim total ms/frame".
- **Budget:** under 0.05 ms/tick. Autosave main-thread copy under 2 ms, every 10 real minutes.

### Phase 1: Citizens, households, jobs, commuting, births, deaths, migration (the base for everything)

- **New:** `src/city/sim_citizens.h` (stores, handles, the bootstrap from lots), `src/city/sim_lifecycle.h` (ageing,
  life stages 0–5 / 6–12 / 13–17 / 18–64 / 65+, births, deaths, partnering, graduation using school seats from the
  `school` field when Phase 4 provides them, else a coverage-scaled stub rate), `src/city/sim_jobs.h` (workplace slots by
  education level, a seeker queue per hood, local-first matching through `MatchLocalFirst`, overqualified workers allowed
  at a happiness cost, quitting over long commutes, retirement), `src/city/sim_migration.h` (immigration from city
  attractiveness and vacant homes, arriving at outside connections; emigration after `unhappyMonths` or a long stretch
  of unemployment with no savings; moving within the city towards a better rent/commute/happiness score),
  `src/city/sim_household_econ.h` (wages by job level, pensions, rent from `CellFactors.rentBurden`'s inputs (a flat
  table until P2 lands), spending, savings), `src/city/sim_wellbeing.h` (health and happiness per citizen from
  `CellFactors` with their weights in `life.` tuning; the **only** place those formulas live; other phases add
  factors, not formulas).
- **Owns:** `sim_cells.h` (CellSim becomes an incremental view: `ApplyCitizenDelta`; `Add()` gives empty homes and job
  slots instead of moving people in; supercell links are kept from real job assignments, so `ForEachLink`, herds and
  hoods work unchanged), `sim_people.h` (becomes a view: `Peek` and `Materialize` return a real `CitizenId` and set
  `pinned`; the `Q` load maps onto records), `city_sim.inl`, `city_sim_p1.inl`, `sim_hoods.h` (school links from real
  enrolments).
- **Writes factors:** `unemployment`, `commuteMin`. **Reads:** everything else, with neutral defaults.
- **Commuting:** each worker's (home cell, job cell) is aggregated into a sparse commute matrix and the supercell links
  (herds read these, unchanged). `commuteMin` per citizen is refreshed monthly from `RouteCache` hood-pair costs (read
  only), quantised to whole minutes.
- **Active tier:** an active set of at most 512 citizens around the camera, ticked each minute through a schedule
  (wake, commute, work or school, shop, leisure, home) with a seeded jitter. `sim_children` and `sim_walkers` get their
  person through a host callback `TenantOf(building, slot) -> CitizenId` (added in `city_sim_p1.inl`; those headers
  already take the identity from the host), so the person in the car you tap is the record.
- **Tests:** `tests/sim_citizens_test` (handles, generations, bootstrap equals the old counters exactly, delta view
  equals a full recount after 10k random ops), `tests/sim_lifecycle_test` (a 1,000-person town over 50 life years: the
  age pyramid stays within ±3 points of the census structure in the design doc's §11, births about 11 and deaths about
  9–10 per 1,000 a year, deterministic hash per seed), `tests/sim_jobs_test` (local first, education levels, the
  existing invariants: working == employed == links), `tests/sim_migration_test` (attractiveness up gives net
  immigration, a jobs crash gives emigration with a lag, no oscillation within 6 months). Update `sim_cells_test` and
  `sim_person_test`. Tests that need instant move-in set `life.instantFill = 1`.
- **UI:** Population panel (age pyramid, education mix, employment, births/deaths/migration per month as sparklines),
  building inspector (households, each member's name, age, education, job, commute, happiness and its top two
  reasons), citizen card (tapped car or walker), a demand tooltip naming the migration drivers.
- **Save:** `CITZ HHLD OCCU MIGR`.
- **Budget (iPad, 50k / 100k):** daily slice 0.15 / 0.3 ms per tick; monthly city-level migration slice under 1.0 ms
  worst; active set 0.1 ms per tick; job matching (every 10 min, seekers only) under 0.3 ms. Memory under 8 MB at 100k.

### Phase 2: Demand, land value, rent, levelling, abandonment, density

- **New:** `src/city/sim_land.h` (land value on the 32 m field: accessibility (from route costs), services, parks,
  pollution, crime and view of water; rent per household = f(land value, level, density)), `src/city/sim_demand.h` (CS2
  style demand from causes: R from free homes, migration pressure and rent affordability per wealth band; C from
  households' spending and goods availability; I and Office from unemployment by education level, the goods shortfall
  (P6) and taxes; per-factor breakdown for the UI).
- **Owns:** `sim_growth.h` (levelling from land value and building condition; **abandoned becomes a state** (empty,
  dark, a flag, can recover) before demolition; density tiers low / row / medium / high and mixed use from zone type),
  `zoning.h` (per-lot seeded spawn rng instead of the shared `mt19937`; an abandoned-lot state), `city_sim_growth.inl`,
  `city_sim_p2.inl`.
- **Writes factors:** `rentBurden`. Writes flags: high rent, abandoned.
- **Tests:** `tests/sim_land_test` (a park raises nearby value, industry lowers it, monotonic in coverage),
  `tests/sim_demand_test` (each driver moves the right bar, the breakdown sums to the bar), extend `sim_growth_test`
  (abandonment and recovery, no level-up waves).
- **UI:** RCI demand bars with a per-factor breakdown panel, land value overlay on the field, rent row in the building
  inspector.
- **Save:** `GROW LAND DMND`.
- **Budget:** 0.1 ms per tick average; land field rebuild on a worker under 5 ms every game hour; lot slices under
  0.8 ms worst.

### Phase 3: Environment: pollution (air, noise, ground, water), weather, seasons, water table, floods

- **New:** `src/city/sim_pollution.h` (sources: industry lots and hubs by output, power plants (the utilities catalog),
  roads by flow × speed for noise and air, landfill and sewage outlets for ground and water; air drifts with the wind,
  noise does not diffuse, ground decays slowly, water follows the coast and outlet plumes), `src/city/sim_weather.h`
  (season, temperature, rain, snow, wind from the calendar plus a seeded weather chain; solar and wind output factors for
  `utilities.h`, read through a getter), `src/city/sim_water_table.h` (groundwater per cell, pumps drawing it down,
  pollution of pumped water, flood depth from rain and terrain), `src/city_sim_p3.inl`.
- **Owns:** only the new files. Reads the registry, flow, `IndSite`, the utilities.
- **Writes factors:** `air noise ground water`. Events: flood (P4 dispatch reads them).
- **Tests:** `tests/sim_pollution_test` (point source falloff, wind shift, decay to zero after removal, deterministic
  diffuse), `tests/sim_weather_test` (seasons in order, the chain is deterministic per seed, solar is 0 at night).
- **UI:** four pollution overlays, a weather and season chip on the clock, a pollution row in the building inspector.
- **Save:** `POLL WTHR WATR`.
- **Budget:** main thread under 0.1 ms per tick; diffusion on a worker under 4 ms every 60 game minutes, as one job.

### Phase 4: City services with capacity and coverage; crime, fire, sickness, death, disasters; garbage, post, telecom

- **New:** `src/city/sim_services.h` (a side table `ServiceSpec` keyed by `BuildingDef` index: capacity (beds, students,
  cells, bodies, tonnes, letters, bandwidth), vehicles; coverage by **road travel time** from route trees (worker), and
  demand assigned local-first up to capacity, so an overloaded clinic covers less), `src/city/sim_incidents.h` (rates
  per cell from the design doc's §5 table: crime from relative income, unemployment, happiness and police; fire from
  building age, density, industry and fire coverage; sickness from pollution and health coverage; a death becomes a
  hearse request. A place is picked only when one happens; a vehicle is dispatched as a herd on the route tree, and
  traffic delays it. Disasters: fire spread, flood (from P3), storm), `src/city/sim_education.h` (school seat
  assignment by level and capacity, writing `CitizenStore::school` through the Phase 1 API), `src/city_sim_p4.inl`.
- **Owns:** `sim_coverage.h` (keeps the radius path as a fallback), `city_sim_services.inl`, `city_garbage.inl`
  (garbage into the service model with capacity and trucks, piles saved).
- **Writes factors:** `coverage[] crime fireRisk telecom parkAccess`. Flags: sick, crime, fire, garbage, no school.
- **Content gate:** park, post and telecom buildings and models need `TTab` entries (a single edit to `transport.h`,
  made in a P0 follow-up) and the HTML-preview approval before the models are built.
- **Tests:** `tests/sim_services_test` (capacity saturates coverage, travel time beats crow-flies, funding scales
  capacity), `tests/sim_incidents_test` (rates match the §11 baselines ±20 % over 10 years in a neutral city, police
  coverage lowers crime, deterministic placement), `tests/sim_education_test` (seats limit graduation, the education
  mix moves over years).
- **UI:** a panel per service (capacity used / total, coverage %, budget slider row), crime, health, education,
  garbage and telecom overlays, incident icons over buildings, sirens (existing children pull aside).
- **Save:** `SERV INCD GARB`.
- **Budget:** 0.2 ms per tick average, under 1.0 ms worst; coverage rebuild on a worker under 20 ms on change (roads,
  services, funding), at most once per 10 game minutes.

### Phase 5: Public transport, mode choice, taxis, outside connections (road, rail, sea, air)

- **New:** `src/city/sim_modes.h` (per herd and per active citizen: car, bus, tram, metro, train, taxi, walk or bike,
  by logit on time, cost, wealth and car ownership; availability from stops within walking distance and parking),
  `src/city/sim_ridership.h` (riders per line and stop, boarding and alighting, crowding, fares into the budget,
  waiting times from headways), `src/city/sim_outside.h` (rail, sea and air connections carrying commuters, tourists,
  migrants (P1 reads the arrival points) and cargo (P6 reads capacity)), `src/city_sim_p5.inl`.
- **Owns:** `sim_herds.h` (mode per herd: car herds stay vehicles, transit herds become riders on lines), `sim_town_traffic.h`,
  `sim_children.h`, `sim_walkers.h` (riders walk to stops), `transit.h`, `city_herds.inl`, `city_sim_flow.inl`,
  `sim_flow.h` (quantised outputs).
- **Writes factors:** `commuteMin` inputs (P1 reads the route costs per mode; the `commuteMin` field stays P1's).
- **Tests:** `tests/sim_modes_test` (free transit shifts the shares, no stop means no transit, wealth shifts to car),
  `tests/sim_ridership_test` (riders are conserved along a line, capacity caps boarding), extend `sim_herds_test` and
  `sim_traffic_test` (car share from the mode model, not a constant).
- **UI:** line info panel (riders/week, load per stop, income vs upkeep, fare and vehicle-count sliders), transit
  ridership overlay, outside connections panel (in/out people and cargo).
- **Save:** `LINE MODE OUTC`.
- **Budget:** 0.2 ms per tick average; the mode split happens at herd release (every 15 minutes) under 1.0 ms worst;
  flow solve stays on its worker.

### Phase 6: Production chains, companies, trucks, import and export

- **New:** `src/city/sim_companies.h` (a company per industrial, commercial or office lot cluster: inputs, outputs,
  stock, production from workers by level (CellSim and occupancy counts) and efficiency, profit, bankrupt, which makes
  the lot abandoned through a P2 flag request event), `src/city/sim_goods.h` (resources: grain, livestock, timber, oil,
  ore → food, wood, petrochemicals, metals → goods; suppliers local first by travel time, then outside connections;
  shipments as goods herds through a NodeLink source that Phase 0 adds to the herds host), `src/city_sim_p6.inl`.
- **Owns:** `industry.h`, `city_industry.inl` (the hubs become raw-material companies; `stock` gets consumers).
- **Writes factors:** `goodsAvailable`. Flags: no goods, no customers (with P2's demand).
- **Tests:** `tests/sim_goods_test` (mass balance across a chain, shortages import and surpluses export, deterministic
  supplier picks), `tests/sim_companies_test` (profit falls with no workers, bankruptcy then recovery).
- **UI:** production panel (resources: produced, consumed, imported, exported), company card in the inspector (inputs,
  outputs, stock, employees by level), goods overlay.
- **Save:** `COMP GOOD TRAD`.
- **Budget:** 0.15 ms per tick average, under 0.8 ms worst; supplier matching every 60 game minutes.

### Phase 7: Budget, taxes per household, service fees, policies, districts, milestones and progression, statistics

- **New:** `src/city/sim_policies.h` (policy catalog → `PolicyMods` per district: free clinics, curfew, free transit,
  recycling, heavy-traffic ban, rent control...), `src/city/sim_districts.h` (player-drawn districts over hoods; a cell
  belongs to one district), `src/city/sim_progress.h` (XP from population, buildings and happiness like CS2,
  development points, the skill tree), `src/city_sim_p7.inl`.
- **Owns:** `sim_economy.h` (residential tax from household income (P1) by education level, commercial/industrial/office
  tax from company profit (P6, with today's per-job formula as the fallback), service fees, import/export income),
  `sim_budget.h`, `sim_milestones.h`, `city_sim_budget.inl`, `gui/city_budget_panel.h`, `gui/city_info_panels.h`.
- **Writes factors:** `taxBurden`, `PolicyMods`.
- **Tests:** extend `sim_economy_test` and `sim_milestones_test`; `tests/sim_policies_test` (a policy changes only its
  district, mods multiply, removal restores).
- **UI:** budget (income per source including fees and trade, expenses per service), tax per zone and education level,
  policies panel per district, district tool (draw and name), progression tree, statistics with charts (population,
  money, happiness, jobs), and the news/Chirper feed from `sim_events`.
- **Save:** `BUDG PLCY DIST MILE` (the old `B1` field and `M` record are still read).
- **Budget:** 0.05 ms per tick average; the daily ledger sliced over 24 hourly slices, under 0.5 ms worst.

## 5. Order and parallelism

```
P0 ──┬── P1 ──┬── P2
     │        ├── P4
     │        ├── P5
     │        └── P7
     ├── P3 (only needs P0: lots, flow, industry, fields)
     └── P6 (only needs P0 + today's CellSim counters, which P1 keeps as an API)
```
- **Wave A, after P0:** P1, P3, P6 in parallel.
- **Wave B, after P1 merges:** P2, P4, P5, P7 in parallel (P2 and P4 read P3's factors, which stay neutral until P3
  lands).
- The merge order inside a wave does not matter, because the files are disjoint and the factors have one writer each.
  Every merge must keep the whole headless test suite green and run `sim_bench` at 50k and 100k.

## 6. Budget summary (iPad, iPhone 15 floor, 50k residents)

Frame: 33 ms at 30 fps (25 ms Ultra 40 fps). Scheduler cap `budgetMs` 2 ms per frame. At the fastest speed, 18 ticks per
second is 0.6 ticks per frame at 30 fps.

| System | avg ms/tick | worst tick | worker |
|---|---|---|---|
| existing (cells, demand, growth, people, flow, hoods, herds, herdmove) | 0.6 (measure in P0) | 2.0 | flow solve |
| P1 citizens | 0.25 | 1.0 | — |
| P2 land and demand | 0.10 | 0.8 | land field 5 ms / hour |
| P3 environment | 0.10 | 0.3 | diffuse 4 ms / hour |
| P4 services and incidents | 0.20 | 1.0 | coverage 20 ms on change |
| P5 transport and modes | 0.20 | 1.0 | flow (existing) |
| P6 goods | 0.15 | 0.8 | — |
| P7 budget and policies | 0.05 | 0.5 | — |
| **total** | **1.65** | slices staggered so that no two worst cases share a tick | |

1.65 ms per tick × 0.6 ticks per frame is about 1.0 ms per frame: inside the 2 ms cap, with room at Ultra and under
thermal halving. Per-frame visible work (children 300 cars, walkers 300, active set) stays outside the scheduler and is
budgeted at 1.5 ms per frame as today. Total sim memory is under 24 MB at 100k.

### 6.1 Spike pass (sim-perf, 2026-11-09): deferred work in P0, P1, P3, P6

The rule: an entity costs nothing unless it changed or is due; bulk work is split over frames; an incremental path
is checked against a full recompute. What each spike became (`tests\sim_bench.bat` measures them):

- **Building registry sync** (P0): one hint per source entry (`BuildingRegistry::Upsert(..., hint)`): an unchanged
  building is a linear pass with no hash lookup; `CheckIndex` (every 64th sync) checks the index against the rows.
- **P1 building sync**: three steps on three frames (rows, records' buildings, hoods' search order), the rows through
  the same hints; the job kind once per row. **P1 views**: the commute links are summed per supercell pair and compared
  with CitySim's links every apply (an exact full check); the link maps are rebuilt only when they differ.
- **P3 hourly apply**: the state copy for the next pass, the road tiles and freeing the replaced state moved to the
  worker (the job copies the applied state, `SimP3ReleaseBase` guards it); the main thread keeps the apply and the
  snapshot of the sources.
- **P6 sites**: registry row to lot through the hints (no hash map of every lot each hour); `CompanyStore::Sync` keeps
  a hint per site position.
- **Autosave**: the .tbdsim chunks of P0, P1, P3 and P6 are made at once on the workers (fork-join; P1's four chunks
  are independent parts now); the .tbdgame text is formatted on the worker from a snapshot of plain copies; the terrain
  edits' bytes are kept while the height field's version holds. The bytes on disk are unchanged.

Still per-tick or per-period loops over everything (fine at 15k lots, the next candidates): `ComputeSimFlags` and
the services pass (P4's file), demand and growth (P2's), the goods slice's seller search (`SellerIndex::Nearest`,
one twelfth of the companies every 5 minutes), P1's monthly slice and job matching (already 1/96 and capped), the hub
flag merge (every lot on a table change), `LifeFillRows` (every lot after a registry change) and the BLDG / CITZ copies.

## 7. Open questions for the user

1. Confirm individual records for every resident (this doc) rather than the cohort model in `city_sim_design.md` §3.
2. Confirm the life calendar: 1 game day = 1 life month (a year is about 16 minutes at the fastest speed).
3. Record cap 120k, with statistical households past it? The milestones go to 50k (Metropolis).
4. Parks, post and telecom buildings need new models: previewed in HTML on localhost first, as usual.
