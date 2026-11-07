# News radio: ideas to implement

These news triggers are in `src/city/news_lines.json` but the game cannot fire them yet.
The matching simulation system is missing or marked "not completed yet" in the code.

## Blocked on the economy (`gui/city_hud.h` says "Money, income and expenses are not completed yet")
- budget_surplus, budget_deficit, bankrupt_transit, corruption_scandal (needs a contracts system)

## Blocked on citizens / population (`city_hud.h`: "the simulation has no citizens")
- population_1k, population_10k, population_50k
- unemployment_low, unemployment_high
- happiness_high, happiness_low, rent_spike, housing_good, homeless_up
- mayor_reelection_poll_up (needs an approval/poll model)
- school_grad_rate_up

## Blocked on city stats (no pollution, crime, or fire simulation found in src)
- pollution_high, industrial_smog, noise_complaints
- crime_spike, crime_drop
- fire_outbreak, depot_fire (fire spread model)
- power_outage, water_shortage, sewage_leak (need outage events; `city/utilities.h` has status only)
- flood, disaster_recovered (no disaster system)
- traffic_jam, traffic_flow_good (needs a traffic-density measure per road)

## Blocked on building / zone events that are not posted yet
- park_built, school_built, hospital_built, library_built, stadium_built
- zone_residential_filled, zone_commercial_filled, zone_industrial_filled
- cleanup_done, tree_planted, river_cleaned, landfill_full, illegal_dumping
- adj:* pairs (e.g. adj:residential+industry): need a "building placed" event plus a nearby-building check.
  Hook point: placement code in `city/zoning.h` and `city/road_tool.h`. The adjacency scan in `city/transit.h` (line ~175) is a model.

## Blocked on transit events (partly possible)
- Possible now: transit_opened, bus_depot_opened, rail_depot_opened, tram_depot_opened, taxi_depot_opened, bus_stop_opened, bus_route_empty, tram_derailment, strike, vehicle_breakdown.
  Hook point: `TransitNetwork::Complete` in `city/transit.h` (line ~600) and `ArriveAtStop`. Not wired yet because `transit.h` has uncommitted changes.

## Blocked on utilities
- electricity_surplus, water_surplus: need a supply vs demand number from `city/utilities.h` (`UtilStatus`).

## Next steps
1. Build a "building placed" event bus. It unlocks park/school/hospital/adjacency news.
2. Add a simple budget ledger (income from zones, expenses from services). It unlocks budget and scandal news.
3. Add a citizen count. It unlocks population, jobs, and happiness news.
4. Add a per-tile pollution and traffic grid. It unlocks pollution and traffic news.
5. Wire `NewsFeed::Post` into the on-screen radio ticker (not built yet).
