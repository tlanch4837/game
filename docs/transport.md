# Transport (`src/city/transport*.h`, `src/city_transport.inl`, `src/gui/transport_toolbar.h`)

Railways, plus the special one-off buildings of the rail, ship and air networks. Unlike zoned lots they are never grown
from painted cells: pick one in the **Transport** panel and place it on the map (like CS2's airport and harbours).

| File | What |
|---|---|
| `src/city/transport.h` | the catalog (`Defs()`), planning / validation (`TransportWorld::Plan`), the planned -> building -> done life cycle |
| `src/city/transport_models.h` | procedural models of all 8 buildings, the construction site, the hologram conversion |
| `src/gui/transport_toolbar.h` | the CS2-style panel: info card (name, price, description, lot size, workplaces, upkeep, vehicles, noise), tabs, tiles, options |
| `src/city_transport.inl` | host glue: cursor, click, terrain levelling, drawing, labels (included by `main.cpp` after `city_zones.inl`) |
| `tests/transport_test.cpp` | headless checks (`tests\transport_test.bat`) |

## Buildings

Rail: Railway Depot (34x15 cells, the CS2 numbers), Train Station, Cargo Train Terminal. Ships: Shipyard, Cargo Harbor,
Passenger Harbor. Air: Small Airfield, Regional Airport. Each has a site rule:

- **Flat**: dry ground, at most 7 m (airports 9 m) of relief (the terrain is levelled under it), clear of roads, rails,
  other buildings and zoned lots. Zoning keeps out of placed buildings (`ZoneGrid::blockers`).
- **Track** (stations): snaps onto the nearest railway, centres on it and lines up across it; needs one straight, nearly
  level, ground-level segment at least 200 m long.
- **Coast** (shipyard, harbours): **must** be on a qualifying coast, whatever the cursor or rotation: solid land behind the
  shoreline, the lot's front on the shore (the waterside 30-36 % of the lot is a pier on piles), open water across the apron
  in front of it and real ship depth (4 / 6 / 3 m) at the apron's seaward end. Snapping (on by default) finds the shore
  within 160 m of the cursor and faces the water; with it off the lot is still checked. Maps without a sea refuse them.
  The reason is shown beside the cursor ("Must be built on a coast...", "...too steep", "...too shallow for ships").

## Road snap and entrances (`src/city/building_snap.h`)

Every flat-site building (depots, terminals, services, telecom, deathcare; utilities, industry hubs and waste use the same
helper) snaps beside the nearest road while it is placed: the **entrance** (`BuildingDef::entrance`, the front / +Z side by
default) faces the road, the lot sits 2 m behind the kerb (clear of the carriageway and sidewalks) and slides along the road
with the cursor. The reach is a fingertip (28 m from the road's edge) or the building's own half size for big lots. `R` /
the toolbar arrows turn it in quarter turns from facing the road. The ghost's outline is green when snapped, with a door mark
and an arrow on the entrance. Far from roads it is placed freely as before. While the **Rotate dial** is out the snap is off
(free position and heading, the dial's Snap switch gives 15 degree steps); the check builds it as dialled, closing the dial
returns to the snap. Harbours keep the coast rule, stations their track.

## Rail yards (`src/city/rail_yard.h`)

The Railway Depot and the Cargo Train Terminal are rail yards: their own tracks (`YardTracks`: the depot's five shed roads
5 m apart and two sidings, the terminal's three loading tracks; the model draws the same table) run across the lot and leave
it at both side edges. A yard **must** have a road at its entrance ("Rail yard entrance must touch a road"). Every track end
is a rail connection point of its own (`RailPort`, 14 on the depot); a yard turned side-on offers none on its road side.
With a railway out the road tool shows a small marker at each end (yellow arrow: free, green ring: joined) and snaps a new
track to the nearest one ("Rail yard track" snap, the green join ring); it carries the yard track straight on, must leave
within 35 degrees of it and never runs back over it. The railway node on the end makes the yard part of the rail graph.

A finished depot owns `vehicles / 4` trains (2). A train only ever appears on the yard's own track, the whole consist
inside the lot, leaves by a joined track end, runs along the connected rail (through switches, at most 60 degrees at a
node) to the nearest Train Station (stops by the platforms) or Cargo Train Terminal (runs in on its track), dwells 40 s,
runs back into the yard and is stabled for 90 s before the next run. Speed: 30 m/s at most, sqrt(0.9 R) in curves, less on
grades, 8 m/s on yard tracks, braking for the stop. A yard with no road, no joined track end or no station on its rail
runs no trains and says so (a toast once, a red chip over it while the Transport tool is open). The trains are placeholder
boxes (a loco and three cars): there is no train model in `assets`. Tests: `tests\rail_yard_test.bat`, `tests\building_snap_test.bat`.

## Using it

**Transport** button (bottom centre, left of Roads). Pick a tab (Rail / Ships / Air) and a tile; the building follows the
cursor as a **blue hologram** (red with the reason when refused). Click places it: it stays a hologram while *Planned* (12 game
minutes), then a construction site rises with a tower crane, then it stands. `,` `.` rotate (not for stations / coast
snapping), right click or Esc drops the building, the bulldozer tool removes one. The Rail tab also lists the railway tracks:
picking one opens the road tool on its **Railways** tab.

## Railways (road tool)

Single and Double Track Railway live in the road network with their own rules: rails never join road nodes or split roads
(a crossing at the same level is a level crossing), grade at most 6 %, curve radius at least 45 m, rails and roads cannot
be swapped (Replace / Swap). Track meshes have sleepers and steel rails. Zoning ignores rails.

## Rail junctions (`src/city/rail_junction.h`, `rail_junction_mesh.h`)

A junction is a rail node of the shared graph; `ClassifyArms` reads its type from the arms' directions alone (no stored
state): Plain, Turnout left / right (one route straight, one diverging 4..35 deg), Wye, 3-way, Diamond (two straight
lines crossing at 10 deg or more). `RailJunctions::slips` turns a diamond of 20 deg or less into a single slip.
`RailCanPass` is the routing rule (`RailPathTo` uses it): either route of a switch, straight over a diamond, never
reversing through the points. `PlanJoin` is the snap: a new track's end onto a free yard track end, a rail node or the
middle of a rail segment, the junction it makes, and the leaving direction auto-adjusted up to 15 deg past the limits;
switches keep 10 m apart. `CheckCrossings` vets a new curve's same-level crossings (diamond at least 10 deg, level
crossing at least 30 deg and 8 m from road junctions), `FindLevelCrossings` lists rail x road crossings,
`BuildCrossover` adds a crossover or scissors between parallel single tracks 3.8..12 m apart.

Engine path (`rail_junction_plane.h`): like the plain track, a junction is one flat SPOM box (10 triangles) drawn by
the track's parallax shader with a baked junction picture instead of the 4.4 x 0.6 m track tile. `JunctionLayouts()` is
the small set of canonical layouts (turnout 8 / 12 / 20 deg, wye 8, 3-way 12, diamond 30 / 90, single slip 15,
crossover and scissors 10, level crossing 90 / 60 / 45); left-handed junctions mirror the right-handed picture and other
angles use the nearest layout (`NearestLayout`). Every route is a whole number of sleeper pitches (0.6 m) long, so plain
track continues in phase. Only what sticks up is geometry (`JunctionProps`): point machines, and at level crossings the
ramps (road asphalt, `JMat::Road`, uv = metres / 4 as `Mat::Asphalt`), markings (`JMat::Paint`), gate masts and booms
(`gate` 0 down .. 1 up), cantilevers on 2+ lane roads, a centre gate past 7.5 m half width, the relay cabinet.

Textures: `tools\rail_junction_tex\dump_layouts.bat` writes the layouts from the C++ (routes, `TieStations`,
`PlanFrogs`, rect, size) to `obj/rail_junction_layouts.json`; `python tools/rail_junction_tex/gen_junction_tex.py`
bakes `assets/rail/junction/tex/<id>_{albedo.jpg,height.png,normal.png}` in the plain track's frame (`gen_track.py`:
height 0 = rail head, -0.55 = formation; height byte 0 = hole) at ~2 cm per texel (long side <= 2048): plain parts are
stamped from the plain track tile, switch zones get long concrete bearers, gaps between close tracks get ballast, level
crossings a deck in the road's asphalt with flangeways. 13 layouts: 53 MB as RGBA8 with mips, 13 MB ASTC 4x4, 6 MB
ASTC 6x6. Preview: `docs/rail_junction_spom.html` (GLSL port of TrackPS, plain track at every open end).

The per-triangle models (`BuildJunctionMesh`, gauge / angle / radius / stem / tail / spacing / road width, LOD 0 / 1)
stay for reference and tests (`docs/rail_junction_preview.html`). Not drawn in the game yet. Tests:
`tests\rail_junction_test.bat`.

## Captures

`TBD_TRANSPORT=<building index>` opens the tool with that building; `TBD_TRANSPORT_DEMO="def x z progress;..."` places
buildings at startup (progress < 0 planned, 0..1 building, 1 done).

Not yet: a train model (placeholder boxes), ships / planes from their buildings, saving the yard trains (they restart
from the yard after a load), money and upkeep.
