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

## Captures

`TBD_TRANSPORT=<building index>` opens the tool with that building; `TBD_TRANSPORT_DEMO="def x z progress;..."` places
buildings at startup (progress < 0 planned, 0..1 building, 1 done).

Not yet: saving placed buildings with the project, trains / ships / planes, connecting depots to rails automatically,
money and upkeep.
