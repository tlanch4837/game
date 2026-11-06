# Cities: Skylines II feature checklist — what we have vs. what's missing

Legend: ✅ have · 🟡 partial / visuals only, no simulation · ❌ missing
(Snapshot 2026-11-06, from a scan of `src/city/*`, `src/city_*.inl`, `src/gui/*toolbar.h`, `assets_src/`.)

## 1. Map & terrain
- ✅ Terrain, streaming world, floating origin, LODs, vegetation, water shader, sea/coast
- ✅ Terrain editing toolbar, levelling under buildings
- ✅ Natural-resource heat maps (fertile, forest, oil, ore)
- ❌ Groundwater / water table, rivers that flow, flood sim
- ❌ Map tiles you buy to expand the playable area
- ❌ Seasons, weather, snow; 🟡 sun/day cycle exists (`sun.h`) but no sim day/night tie-in

## 2. Roads
- ✅ Road network + tool: lanes, curves, intersections, roundabouts, one-way, elevated, bridges, tunnels
- 🟡 Road upgrades (a little code), parking (mentioned)
- ❌ Traffic lights / stop signs / lane connections editor
- ❌ Road-service: maintenance, snow ploughing, toll booths, speed bumps
- ❌ Highway interchanges as presets, pedestrian paths, bike lanes

## 3. Zoning & growth
- ✅ Zone cats: Residential, Commercial, Office, Industrial; zone grid, blocks, lots, building models
- 🟡 Lot growth (some "grow"/level code), buildings spawn on lots
- ❌ RCI demand bars and the demand model behind them
- ❌ Density tiers (low/medium/high, row houses, mixed-use) and building levelling-up from land value
- ❌ Land value, rent, building abandonment
- ❌ Signature / unique buildings

## 4. Specialized industry
- ✅ Hub + drawn work area (Grain, Livestock, Timber, Oil, Ore), efficiency from resources
- ❌ Production chains (raw → processed → commercial), storage, cargo flows between buildings
- ❌ Import/export economy

## 5. Utilities
- 🟡 Power: models only (pole, transformer, pylon, substation) — ❌ power plants, grid sim, coverage
- ❌ Water pumping, water towers, sewage outlets/treatment, pipe networks
- 🟡 Waste: landfill works as an industry area; incinerator/recycling/hazardous models only — ❌ garbage collection sim
- ❌ Telecom/internet coverage

## 6. City services
- ❌ Healthcare (clinics, hospitals), deathcare (cemetery, crematorium)
- ❌ Police, prisons, crime
- ❌ Fire, disaster response, emergency shelters
- ❌ Education (elementary → university), graduates feeding the workforce
- ❌ Parks & recreation (some "park" refs, no service), tourism, hotels, attractions
- ❌ Communications (post office, mail), administration buildings

## 7. Public transport
- ✅ Railways + Train Station, Cargo Terminal, Railway Depot (placed, planned→building→done)
- ✅ Shipyard, Cargo Harbor, Passenger Harbor (coast validation)
- ✅ Small Airfield, Regional Airport
- ❌ Lines/routes tool, stops, schedules, ticket prices, ridership
- ❌ Bus, tram, metro, taxi, subway tunnels
- ❌ Outside connections (highway, rail, sea, air) that bring traffic in/out

## 8. Simulation core (the big gap)
- ❌ Citizens / households (age, job, education, wealth, happiness, health)
- ❌ Jobs ↔ workers matching, commuting
- ❌ Vehicle traffic agents on roads, pathfinding over the road graph, congestion
- 🟡 NPC system + nav mesh exist (`src/ai/`) — pedestrians, not city traffic
- ❌ Pollution (air/ground/water/noise) as maps; 🟡 noise shown on transport info cards only
- ❌ Happiness / wellbeing, birth/death, immigration/emigration
- ❌ Simulation clock + speed controls (pause, 1x/2x/3x)

## 9. Economy & management
- 🟡 Prices / upkeep shown on transport info cards
- ❌ Treasury, income/expenses, taxes per zone/level, budget sliders, loans
- ❌ Service fees, policies, city-wide ordinances
- ❌ Districts (draw + per-district policies)
- ❌ Milestones, XP, progression unlocks, development points/skill tree

## 10. UI / info
- ✅ CS2-style GUI, road / zone / terrain / transport toolbars, info cards
- ❌ Info views/overlays (traffic, power, water, land value, pollution, coverage)
- ❌ City stats panel, charts, Chirper-style notifications, building inspector with occupants
- ❌ Notification icons above buildings (no power, no water, no workers...)

## 11. Platform
- ✅ Save/load scene (`scene_io.h`), undo, hot reload, Luau scripting (mod-like), D3D12 + Vulkan
- 🟡 Audio engine exists, no city ambience
- ❌ Save game of sim state, autosave, map editor export, mod/asset workshop
- ❌ Photo mode, cinematic camera

## Rough completeness
| Area | Status |
|---|---|
| Engine/terrain/rendering | ~80% |
| Road building | ~70% |
| Zoning (painting) | ~50% |
| Transport buildings | ~35% (placement only, no lines/vehicles) |
| Utilities | ~10% (models) |
| Services | 0% |
| Citizen/traffic/economy simulation | ~0–5% |

## Suggested order to close the gap
1. Sim clock + citizens/households + jobs (everything else hangs on this)
2. RCI demand + growth/levelling + land value
3. Vehicle traffic + pathfinding on the road graph
4. Power + water/sewage grids (assets already exist)
5. Money: taxes, upkeep, budget
6. Info overlays + building notification icons
7. Services (fire/police/health/education/garbage), then public-transport lines
8. Milestones / unlocks, districts & policies
