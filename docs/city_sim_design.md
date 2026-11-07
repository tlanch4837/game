# City simulation design: neighbourhoods, herds, and what the player sees

Status: agreed direction (2026-11-07). Nothing below is built yet unless marked **exists**.
Target: iPhone 15 floor (see the iOS performance presets). The rule for every system: **cost scales with what the player is
looking at, never with city size.**

## 1. Principles

1. **Everything is a percentage applied to totals.** Births, deaths, crime, fire, sickness, education, mode choice, demand:
   each is a rate per year (or per time window) applied to a neighbourhood's counts. No system ticks individual people.
2. **Real data is the baseline.** Every rate starts from a real-world figure (table in section 11), then policies, services
   and the economy move it up or down. Tuning lives in `assets/sim/tuning.json` (**exists**: `sim_tuning.h`).
3. **Local first.** Jobs, schools, services and suppliers are looked for in the own neighbourhood, then neighbouring ones by
   travel time, then further out, else the demand waits (and shows as unemployment, no school place, untreated patients).
4. **Specific events only when one happens.** A rate decides *that* a crime or fire happens this hour in a neighbourhood;
   only then is a place picked (a random home, shop or intersection, weighted by what is there) and a vehicle dispatched.
5. **Detail only where the player looks.** Three layers:
   - **Herds** (the whole city): groups of people or goods moving between neighbourhoods per time window.
   - **Children** (on screen): visible cars and pedestrians spawned from the herds near the camera, with real car-following.
   - **Promoted** (watched or clicked): bound to a real tenant slot and simulated as a person from then on.
6. **Looks come from people, never people from looks.** A visible agent first picks a real tenant slot (home, job), then
   takes that slot's appearance and car, derived from a hash. Nothing is invented that the counters do not contain.
7. **Slow, capped feedback.** Every factor moves gradually and has limits, so economy → crime → happiness → emigration cannot
   spiral within a few days. The advisor names the factor driving a change.

## 2. Neighbourhoods (the unit of everything)

- The city is split automatically into neighbourhoods: clusters of blocks bounded by major roads, rail, water or open land.
  Each gets a generated name (alongside `road_names.h`, **exists**). They can double as player districts for policies.
- Each neighbourhood holds totals: population by age group, households, education, wealth, workers, jobs by type, school
  places, service capacity, happiness, crime rate, sickness, land value.
- The existing per-cell counters (**exists**: `sim_cells.h`, 128 m cells, supercells) roll up into neighbourhoods.
- Matching (jobs, schools, services, suppliers) is local first, then adjacent neighbourhoods by travel time, ring by ring,
  else wait. The job matcher already works this way on supercells (**exists**: `CitySim::Match`); it moves to neighbourhoods
  and travel time, and is reused for schools and services.
- Zoning jobs next to an area with unemployed residents gives them first claim automatically.

## 3. Households and demographics (cohort model)

- Population per neighbourhood is counts per **age group**: 0–5, 6–12, 13–17, 18–29, 30–44, 45–64, 65–79, 80+.
  (Today: kids / adults / seniors per lot, **exists**; teens and the finer groups are new.)
- Once a year (or in monthly slices) each group ages: a share moves to the next group.
- **Births** = couples aged 20–44 × birth rate (shifted by wealth, housing space, policies).
- **Deaths** = per age group × base mortality, raised by sickness, poor health coverage and low wellbeing.
- **Education**: a share of each school-age group completes each level, limited by school places and quality, so the
  workforce's education changes over years (decides offices vs factories).
- **Moving in / out / within**: attractiveness (jobs, happiness, rent, services) decides immigration and emigration; people
  move between neighbourhoods towards better conditions they can afford.
- A new suburb of young families becomes a neighbourhood of teens ten years later, then empty nesters: school traffic rises
  and falls on its own.

## 4. Happiness and wellbeing

- **Happiness** per neighbourhood = service availability (fire, police, health, school, parks, transit) + economy (jobs,
  income, taxes); later commute time, pollution and noise as smaller terms.
- **Wellbeing** (named so the player can see the chain): economy → diet and living conditions → wellbeing → sickness and
  death. Bad economy, worse food, higher death rate.

## 5. Events and emergency services

| Event | Rate driven by | Where it happens |
|---|---|---|
| Crime | income relative to the city average, unemployment, happiness, police coverage, time of day | random home, shop or intersection, weighted (shops: theft; nightlife: night trouble) |
| Fire | base rate per building, age, density, industry, fire coverage | a building |
| Sickness | pollution nearby (industry), density, health coverage, wealth, wellbeing | per area; patients are trips to clinics |
| Death | age groups, sickness, health coverage, wellbeing | a home; a deathcare trip |

- Use **relative** income (vs the city average) plus unemployment for crime, not a fixed dollar line, so inflation or a rich
  city does not break the tuning.
- Each event dispatches a real vehicle along the pre-computed routes, so traffic delays it.
- **Sirens**: emergency vehicles move at free speed in the herd model; near the camera, visible cars just ahead pull aside
  (car-following with a lateral offset) and rejoin.
- Today coverage is a radius along roads (**exists**: `sim_coverage.h`); it stays as the input to the event rates.

## 6. Traffic: herds, routes and children

### Demand over the day
- Daily demand curves per trip type: commute out 6–9, home 15–18, school runs, shopping midday and evening, night shifts.
- **Trip chains**: home → school drop-off → work, with the school stop's odds from kids per household.
- Destinations come from the real links: job links (**exists**: supercell home→job links), school assignments, shops.

### Herds
- A herd = everyone with the same origin neighbourhood, destination neighbourhood and departure window (15 min). It has a
  size, a route and a progress. Herds bound for the same destination share a route and only split where destinations differ.
- At the destination a herd dissolves into lots in proportion to their jobs (or school places, shops).
- Each herd carries a **speed spread** (mean + variation), so it stretches out over a long trip and arrivals spread.
- Herd loads per road per window *are* the traffic flow (**exists**: the flow solver, `sim_flow.h`, on a worker thread; today
  it solves one peak-hour snapshot; it becomes per time window).

### Routes
- Per neighbourhood pair, 2–3 pre-computed alternatives (highway 1, highway 2, local roads), rebuilt only when roads change.
- Per destination, one shortest-path tree on a worker gives the next road from every junction: a car's route is a lookup.
  Kept in an LRU cache for active destinations.
- **Highways are pipes**: route-find to the on-ramp, no simulation inside, travel time from load (free up to a point, then
  steeply slower), route-find from the off-ramp to the destination lot.
- Route split per window by current travel times, smoothed over a few windows (no hard "90 % full" switch: it flip-flops).

### Children (on screen)
- Visible cars and pedestrians are spawned from the herds near the camera, within a budget (about 300 + 300).
- Real car-following (Intelligent Driver Model): desired speed = road's current speed × driver personality.
- Driver personality, appearance (skin, hair, clothes) and car (model, colour) are a hash of (city seed, address, tenant slot).
- Close-up life: intersections (lights or stop signs, queues, gaps), braking and brake lights, lane changes before exits,
  buses, delivery vans, garbage trucks, school buses, parking and walking to the door, crowds outside schools at 15:00.
- Pedestrians walk sidewalks as 1D lanes with two sub-lanes for passing; real avoidance only at crossings and doors.
  Animation by vertex animation textures (no CPU skinning).
- **Exists**: cosmetic cars driven by flow (`sim_agents.h`), one instanced draw per model; no parking, no pedestrians.

### Promotion
- **Watched** (in the centre of the screen about a second): pinned to its tenant slot, becomes a real trip with route
  lookups, parking and walking. A few dozen at a time. Off screen it falls back to the herd's timing.
- **Clicked**: materialised as a named person (**exists**: `SimPeople::Materialize`), saved with the game, living their day.

## 7. Mode choice

- Per herd: car, bus, train, walk, bike, by **wealth and time**, plus availability (a stop within walking distance, parking
  at the destination). Transit lines and stops **exist** (`transit.h`); nobody chooses between modes yet.
- Ridership and capacity: full buses, crowded platforms, lines that lose money. Parking demand pushes people to transit.

## 8. Companies and goods

- A modest number of companies (dozens to a few hundred) each own many buildings. New zoned buildings join a company in
  their sector or start one.
- Economy at company level: inputs, outputs, stock, production rate. Suppliers local first, then nearby, then imports
  through outside connections (highway, rail, harbour, airport).
- Shipments are goods herds on the same routes; their load counts as traffic.
- Trucks are children of goods herds; the destination building is picked by size and distance (not evenly).
- Keep the chain short at first: raw material → processed goods → shops. Industry hubs (**exist**: `city_industry.inl`) are
  the raw-material companies.

## 9. Utilities, economy, policies

- **Utilities by time of day**: power and water demand curves; solar only in daylight, wind variable; batteries and backup
  generators then matter. (**Exists**: networks, capacity, brownout order, conduction, `utilities.h`.)
- **Economy**: households earn and spend, companies produce and trade, the city taxes both. (**Exists**: taxes, itemised
  budget, loans, `sim_economy.h`, `sim_budget.h`.)
- **Policies** multiply the percentages (free clinics lower sickness, a curfew lowers night crime, free transit shifts mode
  choice) per neighbourhood or district.
- **Disasters**: later; they plug into the event and dispatch system.

## 10. Build order

1. Neighbourhoods (clustering, names, totals) and local-first matching.
2. Time-of-day demand and herds, with the speed spread; flow solver per time window.
3. Route alternatives, per-destination route trees, highway pipes.
4. Visible children with car-following and intersections; promotion; parking.
5. Cohort demographics, births and deaths, moving in and out, happiness.
6. Events and dispatch (crime, fire, sickness, death), sirens.
7. Mode choice and transit ridership. 8. Companies and goods. 9. Utility curves, policies. Disasters later.

First slice to prove the layer handoffs: one suburb, one highway, one downtown; time-of-day herds; visible cars from the herds
with car-following; one watched car followed from home to an office parking space.

## 11. Real-data baselines (starting values, tune from here)

Approximate figures from public statistics (mostly US / OECD, recent years); **verify against the cited source before
shipping** and keep them in `tuning.json` with the source next to each value.

| Quantity | Starting value | Source to check |
|---|---|---|
| Age structure | 0–17 about 22 %, 18–64 about 61 %, 65+ about 17 % | US Census ACS |
| Household size | about 2.5 people | US Census |
| Births | about 11 per 1,000 residents a year (fertility about 1.6 per woman) | CDC NCHS |
| Deaths | about 9–10 per 1,000 a year, rising steeply with age | CDC NCHS |
| Employment | about 60 % of people 16+ employed | BLS |
| Commute mode | about 75 % drive alone, 9 % carpool, 4–5 % transit, 2–3 % walk (rest work from home) | Census ACS |
| Peak hour | about 8–10 % of daily traffic in the busiest hour | FHWA |
| Home fires | about 2–3 per 1,000 homes a year | NFPA |
| Violent crime | about 4 per 1,000 residents a year | FBI UCR |
| Property crime | about 19–20 per 1,000 residents a year | FBI UCR |
| School levels | elementary ages 6–11, middle 12–14, high 15–18 | — |

(The early in-game split of 33 % adults / 30 % seniors / 25 % kids / 12 % teens overweights seniors; the census structure
above is the baseline, and neighbourhood types shift it: young suburbs, retirement areas, student districts.)
