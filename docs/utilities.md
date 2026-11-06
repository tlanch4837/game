# Utilities: electricity, water, sewage

Code: `src/city/utilities.h` (platform free), `src/gui/utility_toolbar.h`, `src/city_utilities.inl` (host), the no-road clock in
`src/city/zoning.h`. Headless test: `tests\utilities_test.bat`.

## The rules

- **Every building needs electricity, water, sewage and a road.** Zoned buildings lose none of them silently: each missing
  thing shows a bubble over the building (no electricity, no water, no sewage, no road connection).
- **Roads carry the utilities.** Every road that is not a highway, a path or a railway has power, water and sewage under it by
  default. A building is connected through the road in front of it, to everything that road network reaches. Nothing is drawn
  for this in the normal view; the underground view shows it.
- **Sources are drag-and-drop buildings.** Power plants, pumps, sewage outlets, treatment: they snap beside the nearest road and
  face it, and feed the road network they sit on. Supply is the sum of the sources on a network, demand the sum of the built
  buildings on it; buildings are served in the order they were built until it runs out. Emergency generators and batteries only
  cover a shortfall.
- **Lines for where there is no road.** Electricity line, water pipe, sewage pipe. A line joins what its ends touch: a road, a
  source, another line's end, a building. The elevation (Page Up / Down, 10 m steps):
  - electricity at **0 m: small wooden poles**, every 36 m, with a cross arm and three wires;
  - electricity above ground: steel pylons every 110 m;
  - below ground: a buried cable. Pipes are always buried (-10 m default).
  Buried things only show in the underground view.
- **No road: 10 days, then 5.** A building (zoned, or a source) with no road in front of it keeps standing but is flagged; after
  10 game days it is condemned, 5 days later it is demolished. A road back in front of it stops and resets the clock. A new road
  built *over* a plot takes the building at once. The clock is `Lot::noRoadMinutes` (zoning.h) and `UtilBuilding::noRoadMinutes`.

## The tool

"Utilities" launcher (bottom centre, left of Transport). Tabs: Electricity, Water & Sewage. Tiles: the sources of the tab, then
its line. Options: rotation (`,` `.`), elevation (Page Up / Down), snapping to roads, marker, **Underground view**, bulldoze.
Left click places / lays (click again to continue a line), right click or Esc ends the line, then drops the item, then closes.

## Numbers

Source capacities come from `assets/buildings/{power,water}/catalog.json` (generators: nameplate x capacity factor). Demand per
8 m cell of a lot by zone type is `UtilDemandPerCell` (e.g. a low density home of 6 cells: 0.012 MW, 3 m3/day).

## Not done yet

- The shoreline / groundwater / geothermal placement rules (the cards say so; the placement does not enforce them).
- Money: costs are shown, nothing is charged. Upkeep, pollution, wind / sun variation, battery charge state.
- Saving: placed sources and lines are not saved with the project (the scene file would keep the entities only).
- Transport and industry buildings are not yet on the road / utility rules (only zoned lots and the sources here).
- A line only connects at its ends (not part-way along another line).

## Captures

`TBD_UTIL_DEMO=<letters>` with `TBD_ROADS=demo`: `z` paint all zones, `p` wind turbine + pump + outlet, `w` wind turbine only,
`l` lines (wooden poles, pylons, buried pipes), `u` open the tool in the underground view, `t` open with the first source chosen.
