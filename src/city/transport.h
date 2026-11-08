// Transport buildings: the special, one-off buildings of the rail, ship and air networks (railway depot, stations, cargo
// terminal, shipyard and harbours, airfields). Unlike zoned lots they are never grown from painted cells: the player picks
// one and places it on the map, like Cities: Skylines II's airport and harbours.
//
//   Preview     blue hologram that follows the cursor (red when the spot is refused, with the reason);
//   Placed      the hologram stays for a short planning time ("Planned"), then it is built (a construction site that
//               rises), then it stands;
//   Sites       Flat   any dry, reasonably level ground clear of roads, rails, other buildings and zoned lots;
//               Track  a train station: it snaps onto a straight stretch of railway and lines up with the track;
//               Coast  harbours and shipyards. MUST sit on a qualifying coast: solid land behind the shoreline, the front
//                      of the lot on the shore, and a deep, open water apron in front of it (see PlanCoast). Never
//                      placeable inland, however the player rotates or moves it.
// Platform free: the host feeds the ground, the sea level, the road network and the zone grid (PlaceContext).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "building_snap.h"
#include "rail_port.h"
#include "road_network.h"
#include "zoning.h"

namespace tbd {
namespace city {

enum class TTab : int { Rail, Ship, Air, Bus, Fire, Health, Death, Police, Education, Comms, Count };
// Service tabs have their own HUD button and panel (Fire & Rescue, Healthcare, ...); the rest share Transportation.
inline bool IsServiceTab(TTab t) { return (int)t >= (int)TTab::Fire; }
// Tabs of one group share a panel and a HUD button (Deathcare is a sub-tab of Healthcare); Rail / Ships / Air / Buses share Transportation.
inline int TabGroup(TTab t) { return t == TTab::Death ? (int)TTab::Health : IsServiceTab(t) ? (int)t : 0; }
enum class Site : uint8_t { Flat, Track, Coast };
enum class Model : uint8_t { RailDepot, TrainStation, CargoTerminal, Shipyard, CargoHarbor, PassengerHarbor, SmallAirfield, RegionalAirport, GlbBuilding }; // GlbBuilding: a finished GLB (BuildingDef::glb) stands in the scene; the hologram and site are a plain shell

struct BuildingDef {
    const char* name;
    const char* shortName;
    const char* desc;
    TTab tab;
    Model model;
    Site site;
    int w, d;              // lot size in 8 m cells: w across the building, d from back to front (the front faces the road / water)
    int cost, upkeep, workplaces, vehicles;
    const char* noise;
    float height;          // tallest part, metres (also scales the construction rise)
    float buildMinutes;    // game minutes to build
    Vec3 tint;             // the tile's colour
    float apron = 0;       // coast only: how far the deep water has to reach past the lot's front edge
    float draft = 0;       // coast only: how deep (m under the sea) that water must be
    float dock = 0.3f;     // coast only: the share of the lot's depth that sits over the water
    const char* glb = nullptr; // the finished building's model (a scene entity), relative to the project root; null = procedural
    int entrance = 0;      // the lot edge with the door / driveway (building_snap.h EdgeMids order; 0: the front, +Z in model space)
    float W() const { return w * kCell; }
    float D() const { return d * kCell; }
};

inline const std::vector<BuildingDef>& Defs() {
    static const std::vector<BuildingDef> v = {
        {"Railway Depot", "Rail Depot",
         "A large complex used for the storing, sorting, and maintenance of trains. Can be upgraded with extra tracks and a maintenance hall.",
         TTab::Rail, Model::RailDepot, Site::Flat, 18, 9, 120000, 12000, 24, 8, "Medium", 12, 600, {0.35f, 0.50f, 0.85f}},
        {"Train Station", "Station",
         "Passengers board and alight here. Place it on a long, straight stretch of railway: it lines itself up with the track.",
         TTab::Rail, Model::TrainStation, Site::Track, 24, 6, 60000, 6000, 12, 0, "Low", 10, 360, {0.45f, 0.62f, 0.90f}},
        {"Cargo Train Terminal", "Cargo Terminal",
         "Freight trains are loaded and unloaded here. A gantry crane moves containers between rail, road and the storage yard.",
         TTab::Rail, Model::CargoTerminal, Site::Flat, 28, 12, 180000, 18000, 40, 6, "High", 16, 720, {0.80f, 0.45f, 0.30f}},
        {"Shipyard", "Shipyard",
         "Builds, repairs and stores ships. It must be built on a qualifying coast: solid land behind the shoreline and deep, open water in front.",
         TTab::Ship, Model::Shipyard, Site::Coast, 20, 12, 150000, 15000, 60, 6, "High", 28, 840, {0.30f, 0.65f, 0.80f}, 70.0f, 4.0f, 0.30f},
        {"Cargo Harbor", "Cargo Harbor",
         "Ships dock here to load and unload containers and bulk cargo. Needs a qualifying coast with deep water for large ships.",
         TTab::Ship, Model::CargoHarbor, Site::Coast, 24, 14, 200000, 20000, 70, 8, "High", 38, 900, {0.85f, 0.60f, 0.25f}, 90.0f, 6.0f, 0.34f,
         "assets/buildings/transport/harbor/glb/HB_24x14_cargo_port_L1.glb"},
        {"Passenger Harbor", "Passenger Harbor",
         "Ferries and cruise ships call here. Needs a qualifying coast with some depth in front of the pier.",
         TTab::Ship, Model::PassengerHarbor, Site::Coast, 12, 8, 90000, 9000, 20, 4, "Low", 14, 480, {0.40f, 0.78f, 0.85f}, 50.0f, 3.0f, 0.36f,
         "assets/buildings/transport/harbor/glb/HB_12x8_ferry_port_L1.glb"},
        {"Small Airfield", "Airfield",
         "A short runway with a tower, a hangar and a small terminal for light aircraft and regional flights. Needs a big, level, dry site.",
         TTab::Air, Model::SmallAirfield, Site::Flat, 48, 14, 400000, 40000, 40, 10, "High", 22, 1200, {0.60f, 0.75f, 0.55f}},
        {"Regional Airport", "Regional Airport",
         "A full airport: a long runway, parallel taxiway, terminal with gates, control tower and hangars. Needs a very large, level, dry site.",
         TTab::Air, Model::RegionalAirport, Site::Flat, 64, 28, 1200000, 110000, 180, 24, "Very High", 30, 2400, {0.70f, 0.70f, 0.90f}},
        // GLB buildings (tools/building_gen/gen_bus_depot.py, assets/rail/tram_depot): lot sizes are the models' own
        {"Bus Depot", "Bus Depot",
         "Buses are stored, cleaned and maintained here before they go out on their lines. Place it beside a road.",
         TTab::Bus, Model::GlbBuilding, Site::Flat, 4, 5, 60000, 4000, 14, 8, "Medium", 9, 360, {0.95f, 0.65f, 0.20f}, 0, 0, 0.3f,
         "assets/buildings/transport/bus_depot/glb/BD_4x5_bus_depot_L1.glb"},
        {"Compact Bus Depot", "Compact Depot",
         "A small bus garage for a few vehicles, for tight lots in a dense city.",
         TTab::Bus, Model::GlbBuilding, Site::Flat, 3, 3, 30000, 2000, 6, 4, "Low", 8, 240, {0.95f, 0.75f, 0.35f}, 0, 0, 0.3f,
         "assets/buildings/transport/bus_depot/glb/BD_3x3_compact_bus_depot_L1.glb"},
        {"Tram Depot", "Tram Depot",
         "A large depot where trams are stored, washed and maintained. Its yard connects to the tram lines.",
         TTab::Rail, Model::GlbBuilding, Site::Flat, 19, 17, 160000, 12000, 20, 8, "Medium", 12, 600, {0.40f, 0.70f, 0.55f}, 0, 0, 0.3f,
         "assets/rail/tram_depot/glb/TRAMDEPOT_19x17_tram_depot_L1.glb"},
        {"Compact Tram Depot", "Compact Tram",
         "A refurbished small tram depot for a handful of vehicles.",
         TTab::Rail, Model::GlbBuilding, Site::Flat, 10, 8, 80000, 6000, 10, 4, "Low", 10, 420, {0.50f, 0.78f, 0.62f}, 0, 0, 0.3f,
         "assets/rail/tram_depot/glb/TRAMDEPOT_10x8_compact_refurbished_L1.glb"},
        {"Taxi Depot", "Taxi Depot",
         "Taxis wait, refuel and are dispatched from here. Place it beside a road.",
         TTab::Bus, Model::GlbBuilding, Site::Flat, 4, 3, 50000, 3500, 10, 10, "Low", 6, 300, {0.95f, 0.80f, 0.15f}, 0, 0, 0.3f,
         "assets/buildings/transport/taxi_depot/glb/TD_4x3_taxi_depot_L1.glb"},
        {"Small Taxi Depot", "Small Taxi",
         "A small taxi rank with a dispatch office, for a handful of cabs.",
         TTab::Bus, Model::GlbBuilding, Site::Flat, 3, 2, 25000, 1800, 5, 4, "Low", 5, 200, {0.97f, 0.86f, 0.35f}, 0, 0, 0.3f,
         "assets/buildings/transport/taxi_depot/glb/TD_3x2_small_taxi_depot_L1.glb"},
        // fire stations (assets/buildings/fire/gen_fire.py): lot sizes are the models' own
        {"Neighbourhood Firehouse", "Firehouse",
         "Historic single-bay brick firehouse with a hose tower. Short radius, 1 engine.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 2, 2, 40000, 3000, 8, 1, "Low", 14.6f, 300, {0.85f, 0.18f, 0.12f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_2x2_firehouse_L1.glb"},
        {"Volunteer Fire Station", "Volunteer",
         "Modern double-bay volunteer station with an office annex. Short radius, 2 engines.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 2, 3, 60000, 4000, 10, 2, "Low", 7.0f, 300, {0.88f, 0.25f, 0.15f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_2x3_volunteer_L1.glb"},
        {"Fire Station", "Fire Station",
         "A standard station with four apparatus bays. Covers a district, 4 engines.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 3, 3, 120000, 8000, 20, 4, "Medium", 19.6f, 420, {0.82f, 0.15f, 0.1f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_3x3_station_L2.glb"},
        {"Modern Fire Station", "Modern Station",
         "A modern station with a training yard and six engines.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 3, 4, 180000, 12000, 30, 6, "Medium", 15.1f, 480, {0.9f, 0.28f, 0.18f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_3x4_modern_L2.glb"},
        {"Fire Headquarters", "Fire HQ",
         "Command centre of the fire service with ten engines and a drill tower.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 4, 4, 320000, 22000, 50, 10, "Medium", 25.2f, 600, {0.78f, 0.12f, 0.08f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_4x4_hq_L3.glb"},
        {"Regional Fire Station", "Regional",
         "A large regional station with eight engines and a rescue unit.",
         TTab::Fire, Model::GlbBuilding, Site::Flat, 4, 5, 280000, 19000, 44, 8, "Medium", 19.0f, 600, {0.92f, 0.32f, 0.2f}, 0, 0, 0.3f,
         "assets/buildings/fire/glb/FS_4x5_regional_L3.glb"},
        // health (assets/buildings/health): lot sizes are the models' own
        {"Family Clinic", "Family Clinic",
         "Single-storey brick GP clinic with porch and roadside cross sign. Small radius, walk-in only.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 2, 2, 135000, 9450, 12, 1, "Medium", 4.9f, 360, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_2x2_clinic_L1.glb"},
        {"Medical Centre", "Medical Centre",
         "Two-storey medical office with ground-floor pharmacy. Small radius, 1 ambulance.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 2, 3, 156000, 10920, 18, 2, "Medium", 8.2f, 420, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_2x3_medical_L1.glb"},
        {"Urgent Care Centre", "Urgent Care Centre",
         "Three-storey urgent care with side ER entrance and covered ambulance bay. Medium radius, 2 ambulances.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 3, 3, 304000, 21280, 27, 3, "Medium", 15.8f, 510, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_3x3_urgent_L2.glb"},
        {"Community Hospital", "Community Hospital",
         "L-shaped brick hospital: four-storey ward wing, outpatient block, ER canopy. Medium radius, 4 ambulances.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 3, 4, 406000, 28420, 36, 4, "Medium", 20.1f, 600, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_3x4_community_L2.glb"},
        {"General Hospital", "General Hospital",
         "Podium plus eight-storey ward tower with rooftop helipad. Large radius, 8 ambulances, medevac.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 4, 4, 416000, 29120, 48, 5, "Medium", 41.3f, 720, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_4x4_general_L3.glb"},
        {"Regional Medical Centre", "Regional",
         "Twin ward blocks over a glazed atrium, ER wing, ambulance yard and helipad. Large radius, 12 ambulances, boosts nearby clinics.",
         TTab::Health, Model::GlbBuilding, Site::Flat, 4, 5, 598000, 41860, 60, 6, "Medium", 23.9f, 840, {0.30f, 0.75f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/health/glb/HC_4x5_regional_L3.glb"},
        // police (assets/buildings/police): lot sizes are the models' own
        {"Police Post", "Police Post",
         "Neighbourhood police office. Short coverage radius, dispatches 1 patrol car.",
         TTab::Police, Model::GlbBuilding, Site::Flat, 2, 2, 88000, 6160, 12, 1, "Medium", 5.5f, 360, {0.30f, 0.40f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/police/glb/PS_2x2_post_L1.glb"},
        {"Police Station", "Police Station",
         "Precinct station with sally port and secure lot. Medium radius, 5 cars, holding cells.",
         TTab::Police, Model::GlbBuilding, Site::Flat, 3, 4, 264000, 18480, 36, 3, "Medium", 20.1f, 600, {0.30f, 0.40f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/police/glb/PS_3x4_station_L2.glb"},
        {"Police Headquarters", "Police",
         "City headquarters. Large radius, 12 cars, boosts nearby stations.",
         TTab::Police, Model::GlbBuilding, Site::Flat, 4, 5, 506000, 35420, 60, 5, "Medium", 33.9f, 840, {0.30f, 0.40f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/police/glb/PS_4x5_hq_L3.glb"},
        {"County Jail", "County Jail",
         "Long-term detention. Frees station holding cells.",
         TTab::Police, Model::GlbBuilding, Site::Flat, 5, 5, 715000, 50050, 75, 6, "Medium", 8.4f, 990, {0.30f, 0.40f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/police/glb/PS_5x5_jail_X.glb"},
        {"Police Academy", "Police Academy",
         "Trains officers; raises police effectiveness citywide.",
         TTab::Police, Model::GlbBuilding, Site::Flat, 4, 4, 458000, 32060, 48, 4, "Medium", 18.3f, 720, {0.30f, 0.40f, 0.80f}, 0, 0, 0.3f,
         "assets/buildings/police/glb/PS_4x4_academy_X.glb"},
        // education (assets/buildings/education): lot sizes are the models' own
        {"Rural Elementary School", "Rural",
         "One-room-style white schoolhouse: gable tin roof, bell cupola, porch, swings and play tower out back.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 2, 2, 72000, 5040, 12, 0, "Low", 9.5f, 360, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_2x2_elem_rural_L1.glb"},
        {"Elementary School", "Elementary School",
         "One-storey brick classroom wings with a taller gym hall, entry canopy, school-bus loop and playground.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 3, 3, 162000, 11340, 27, 0, "Low", 9.0f, 510, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_3x3_elem_L1.glb"},
        {"Urban Elementary School", "Urban",
         "Four-storey brick public school on a tight lot with a fenced rooftop playground.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 2, 3, 108000, 7560, 18, 0, "Low", 17.8f, 420, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_2x3_elem_urban_L1.glb"},
        {"Rural High School", "Rural High School",
         "Single-storey brick school with gabled gym, practice field with bleachers and a bus loop.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 3, 4, 216000, 15120, 36, 0, "Low", 8.2f, 600, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_3x4_high_rural_L2.glb"},
        {"High School", "High School",
         "Sprawling two-storey suburban high school: classroom wings round a courtyard, gym, auditorium with fly tower, lit football stadium with track, tennis courts, student lot and bus loop.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 6, 9, 972000, 68040, 162, 0, "Low", 16.0f, 1860, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_6x9_high_L2.glb"},
        {"Urban High School", "Urban High School",
         "Six-storey high school with glazed atrium entrance and a fenced rooftop basketball court.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 3, 4, 216000, 15120, 36, 0, "Low", 27.1f, 600, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_3x4_high_urban_L2.glb"},
        {"College", "College",
         "Community-college campus: two gabled academic halls around a lawn quad, library with columned portico.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 4, 4, 288000, 20160, 48, 0, "Low", 13.6f, 720, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_4x4_college_L3.glb"},
        {"Urban College", "Urban College",
         "Downtown commuter campus: six- and four-storey concrete blocks with sun-shade fins, arcade, sky bridge, library, fountain plaza and lawn courtyard.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 4, 5, 360000, 25200, 60, 0, "Low", 27.0f, 840, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_4x5_college_urban_L3.glb"},
        {"University", "University",
         "Main hall with clock tower and portico, gabled science and humanities halls, domed rotunda, fountain quad and gate.",
         TTab::Education, Model::GlbBuilding, Site::Flat, 5, 5, 450000, 31500, 75, 0, "Low", 40.6f, 990, {0.85f, 0.65f, 0.25f}, 0, 0, 0.3f,
         "assets/buildings/education/glb/ED_5x5_university_L4.glb"},
        // telecom (assets/buildings/telecom): lot sizes are the models' own
        {"Small Antenna", "Small Antenna",
         "Cheap monopole that fills small coverage gaps. Low capacity.",
         TTab::Comms, Model::GlbBuilding, Site::Flat, 1, 1, 14000, 980, 3, 0, "Low", 15.4f, 270, {0.60f, 0.60f, 0.70f}, 0, 0, 0.3f,
         "assets/buildings/telecom/glb/TC_1x1_antenna.glb"},
        {"Cell Tower", "Cell Tower",
         "Lattice cell tower with sector antennas. The everyday workhorse of wireless coverage.",
         TTab::Comms, Model::GlbBuilding, Site::Flat, 2, 2, 56000, 3920, 12, 0, "Low", 36.8f, 360, {0.60f, 0.60f, 0.70f}, 0, 0, 0.3f,
         "assets/buildings/telecom/glb/TC_2x2_cell_tower.glb"},
        {"Radio Mast", "Radio Mast",
         "Guyed broadcast mast. Huge range but modest capacity per area.",
         TTab::Comms, Model::GlbBuilding, Site::Flat, 4, 4, 224000, 15680, 48, 0, "Low", 98.4f, 720, {0.60f, 0.60f, 0.70f}, 0, 0, 0.3f,
         "assets/buildings/telecom/glb/TC_4x4_radio_mast.glb"},
        {"Telecom Center", "Telecom Center",
         "Exchange and data hall. Big capacity, wired service along roads, relay tower and satellite link.",
         TTab::Comms, Model::GlbBuilding, Site::Flat, 6, 4, 437000, 30590, 72, 0, "Low", 48.4f, 960, {0.60f, 0.60f, 0.70f}, 0, 0, 0.3f,
         "assets/buildings/telecom/glb/TC_6x4_center.glb"},
        // deathcare (assets/buildings/deathcare/gen_deathcare.py via tools/building_gen): shown on the Deathcare sub-tab of Healthcare
        {"Small Cemetery", "Small Cemetery",
         "A churchyard cemetery with a stone chapel and three rows of graves. Holds the dead of a small town; bodies are buried here.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 4, 4, 60000, 2000, 6, 2, "Low", 5.0f, 300, {0.42f, 0.55f, 0.40f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CM_4x4_small_cemetery_L1.glb"},
        {"Cemetery", "Cemetery",
         "A large cemetery with a chapel and bell tower, a mausoleum and eight rows of graves in two walled-in plots.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 6, 6, 180000, 6000, 14, 4, "Low", 12.0f, 480, {0.36f, 0.50f, 0.36f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CM_6x6_cemetery_L2.glb"},
        {"Crematorium", "Crematorium",
         "A chapel hall with a cremator wing and a brick chimney. Burns the dead so they need no grave; the ashes are kept in the garden.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 4, 4, 120000, 9000, 16, 2, "Medium", 22.0f, 420, {0.62f, 0.42f, 0.34f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CR_4x4_crematorium_L1.glb"},
        {"Large Crematorium", "Large Crematorium",
         "A bigger crematorium with a columbarium wall, a funeral chapel and a tall stack. Serves a whole district.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 5, 5, 260000, 18000, 34, 4, "Medium", 27.0f, 540, {0.58f, 0.38f, 0.30f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CR_5x5_large_crematorium_L2.glb"},
        {"Crematorium Campus", "Crematorium Campus",
         "A brick chapel with an entrance tower, porte-cochere and a columbarium cloister, a cremator wing with a tall stack, and a car park. Serves a whole city.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 8, 12, 480000, 30000, 60, 6, "Medium", 34.0f, 720, {0.60f, 0.40f, 0.32f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CR_8x12_crematorium_campus_L3.glb"},
        {"Memorial Cemetery", "Memorial Cemetery",
         "A walled cemetery with an avenue, a war memorial, four mausoleums, a chapel of rest, rows of headstones, an office and a hearse bay.",
         TTab::Death, Model::GlbBuilding, Site::Flat, 13, 12, 520000, 26000, 50, 8, "Low", 9.0f, 780, {0.34f, 0.48f, 0.34f}, 0, 0, 0.3f,
         "assets/buildings/deathcare/glb/CM_13x12_memorial_cemetery_L3.glb"},
    };
    return v;
}
inline const char* TTabName(TTab t) {
    static const char* const k[] = {"Rail", "Ships", "Air", "Buses", "Fire & Rescue", "Healthcare", "Deathcare", "Police", "Education", "Communications"};
    return k[(int)t];
}
inline const char* SiteRule(Site s) {
    static const char* const k[] = {"Place on level, dry ground", "Must be placed on a straight railway", "Must be built on a qualifying coast"};
    return k[(int)s];
}

// ---------------------------------------------------------------------------------------------------- rail yards
// The Railway Depot and the Cargo Train Terminal are rail yards: their own tracks (drawn by the model) run across the lot
// along its local x and leave it at both side edges. Each track end is a rail connection point (RailPort). The entrance
// (the driveway the road vehicles use) is the lot edge that faces the road, the front by default (the depot's office and
// fuel tanks, the terminal's container stacks); a track end on the entrance side (a yard turned side-on to its road) is
// not offered. One table, read by the model (transport_models.h) and the connection points, so they always agree.
struct YardTrack {
    float z;      // local z of the track's centre line
    float x0, x1; // where it starts and ends (local x)
};
inline bool IsRailYard(const BuildingDef& d) { return d.model == Model::RailDepot || d.model == Model::CargoTerminal; }
inline std::vector<YardTrack> YardTracks(const BuildingDef& d) {
    const float hw = d.W() * 0.5f;
    std::vector<YardTrack> v;
    if (d.model == Model::RailDepot) { // the five shed roads (5 m apart, through the shed's portals), then the two outer sidings
        for (float z : {-10.0f, -5.0f, 0.0f, 5.0f, 10.0f}) v.push_back({z, -hw, hw});
        for (float z : {-27.0f, -33.0f}) v.push_back({z, -hw, hw});
    } else if (d.model == Model::CargoTerminal) { // the three loading tracks under the gantry
        for (float z : {-6.0f, 8.0f, 22.0f}) v.push_back({z, -hw, hw});
    }
    return v;
}
// The track ends of a yard standing at c facing `front`, its entrance edge facing entranceDir (zero: none known). The
// ends on the entrance side are left out.
inline std::vector<RailPort> YardPorts(const BuildingDef& d, V2 c, V2 front, float y, V2 entranceDir = {}, uint64_t owner = 0) {
    std::vector<RailPort> v;
    const V2 side{front.z, -front.x};
    const std::vector<YardTrack> tr = YardTracks(d);
    for (size_t i = 0; i < tr.size(); ++i)
        for (int e = 0; e < 2; ++e) {
            RailPort p;
            const float x = e ? tr[i].x1 : tr[i].x0;
            p.p = c + side * x + front * tr[i].z;
            p.dir = e ? side : side * -1.0f;
            p.y = y, p.owner = owner, p.track = (int)i, p.end = e;
            if (Len(entranceDir) > 0.5f && Dot2(p.dir, entranceDir) > 0.7f) continue; // the road is there
            v.push_back(p);
        }
    return v;
}

// ---------------------------------------------------------------------------------------------------- geometry
// A footprint's local frame: x across the building, z from its back to its front.
struct Frame {
    V2 c, front, side;
    Frame(V2 c_, V2 f) : c(c_), front(f), side{f.z, -f.x} {}
    V2 World(float x, float z) const { return c + side * x + front * z; }
    V2 Local(V2 p) const {
        const V2 d = p - c;
        return {Dot2(d, side), Dot2(d, front)};
    }
};
inline BlockRect RectOf(V2 c, V2 front, const BuildingDef& d) { return {c, front, d.W() * 0.5f, d.D() * 0.5f}; }
// Do two oriented rectangles overlap (separating axis test), each grown by pad?
inline bool Overlaps(const BlockRect& a, const BlockRect& b, float pad = 0.0f) {
    const V2 d = b.c - a.c;
    const V2 axes[4] = {a.front, a.Side(), b.front, b.Side()};
    for (const V2& u : axes) {
        const float ra = (a.hw + pad) * std::fabs(Dot2(a.Side(), u)) + (a.hd + pad) * std::fabs(Dot2(a.front, u));
        const float rb = (b.hw + pad) * std::fabs(Dot2(b.Side(), u)) + (b.hd + pad) * std::fabs(Dot2(b.front, u));
        if (std::fabs(Dot2(d, u)) > ra + rb) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- planning
struct PlaceContext {
    const Network* net = nullptr;
    const ZoneGrid* zones = nullptr;
    GroundFn ground;
    float sea = -1e9f; // the water surface; very low = this map has no sea
    float Ground(float x, float z) const { return ground ? ground(x, z) : 0.0f; }
    float Ground(V2 p) const { return Ground(p.x, p.z); }
    bool HasWater() const { return sea > -1e8f; }
    bool IsWater(V2 p, float margin = 0.0f) const { return HasWater() && Ground(p) < sea - margin; }
};
struct PlanOptions {
    bool snapTrack = true; // stations snap to the nearest railway
    bool snapCoast = true; // harbours and shipyards snap to the nearest shore and face the water
    bool snapRoad = true;  // flat-site buildings snap beside the nearest road, entrance to it (building_snap.h); the host
                           // turns it off while the Rotate dial is out (free position and heading)
};
struct Placement {
    bool valid = false;
    std::string error;
    V2 c{}, front{0, 1};
    float y = 0;                 // the level the building sits at
    int track = -1;              // station: the railway segment it sits on
    float platformInner = 3.0f;  // station: distance from the track's middle to the platform edge
    bool snapped = false;
    bool haveShore = false;
    V2 shore{};                  // coast: the shoreline point at the lot's waterside
    float relief = 0;            // highest minus lowest ground under the footprint
    float waterDepth = 0;        // coast: the shallowest depth sampled across the apron
    bool roadSnapped = false;    // flat site: stood beside a road by the road snap
    int roadSeg = -1;            // the road its entrance reaches (-1: none)
    int entranceEdge = -1;       // the edge with road access (EdgeMids order; -1: none)
    V2 entrance{}, entranceDir{}; // that edge's middle and outward normal (toward the road)
};

enum class BState : uint8_t { Planned, Building, Done };
constexpr float kPlanMinutes = 12.0f; // the hologram stays up this long before the construction site opens
struct Placed {
    uint64_t id = 0;
    int def = 0;
    V2 c{}, front{0, 1};
    float y = 0;
    int track = -1;
    float platformInner = 3.0f;
    BState state = BState::Planned;
    float timer = 0;    // game minutes in the current state
    float progress = 0; // construction 0..1
    bool alive = true;
    bool haveShore = false;
    V2 shore{};
};

class TransportWorld {
public:
    std::vector<Placed> items;
    uint64_t version = 1;
    std::vector<uint64_t> removed; // demolished since the host last emptied this (it drops their drawing caches)

    // ---- the best placement for def with the cursor at p
    Placement Plan(const PlaceContext& cx, int def, V2 cursor, float rotationDeg, const PlanOptions& opt = {}) const {
        const BuildingDef& D = Defs()[def];
        Placement r;
        const float a = rotationDeg * 0.01745329f;
        r.front = {std::sin(a), std::cos(a)};
        r.c = cursor;
        r.y = cx.Ground(cursor);
        if (D.site == Site::Track && !PlanTrack(cx, D, cursor, opt, r)) return r;
        if (D.site == Site::Coast && opt.snapCoast && !PlanCoast(cx, D, cursor, r)) return r;
        if (D.site == Site::Flat && opt.snapRoad && cx.net) { // [snap] beside the nearest road, entrance to it
            const BuildingSnap s = SnapBesideRoad(*cx.net, cursor, D.W(), D.D(), QuarterTurns(rotationDeg), D.entrance);
            if (s.snapped) r.c = s.c, r.front = s.front, r.snapped = r.roadSnapped = true;
        }
        Validate(cx, D, r);
        if (D.site == Site::Flat && cx.net) { // road access through an edge (the entrance edge first)
            int seg = -1;
            const int e = RoadAtEdge(*cx.net, r.c, r.front, D.W(), D.D(), D.entrance, kRoadReach, seg);
            if (e >= 0) {
                V2 mid[4], out[4];
                EdgeMids(r.c, r.front, D.W(), D.D(), mid, out);
                r.entranceEdge = e, r.roadSeg = seg, r.entrance = mid[e], r.entranceDir = out[e];
            } else if (IsRailYard(D)) Fail(r, "Rail yard entrance must touch a road: place it beside a road");
        }
        r.valid = r.error.empty();
        return r;
    }
    static constexpr float kRoadReach = 6.0f; // metres from a lot edge to a road's kerb that still count as road access

    // A placed building's entrance (the edge with road access) and its outward normal; false: no road reaches it.
    static bool EntranceOf(const Network& net, const Placed& b, V2& at, V2& dir) {
        const BuildingDef& D = Defs()[b.def];
        int seg = -1;
        const int e = RoadAtEdge(net, b.c, b.front, D.W(), D.D(), D.entrance, kRoadReach, seg);
        if (e < 0) return false;
        V2 mid[4], out[4];
        EdgeMids(b.c, b.front, D.W(), D.D(), mid, out);
        at = mid[e], dir = out[e];
        return true;
    }
    // A placed rail yard's track ends (none on its entrance side; with no road the front counts as the entrance).
    static std::vector<RailPort> PortsOf(const Network* net, const Placed& b) {
        const BuildingDef& D = Defs()[b.def];
        if (!IsRailYard(D)) return {};
        V2 at, dir = b.front;
        if (net) EntranceOf(*net, b, at, dir);
        return YardPorts(D, b.c, b.front, b.y, dir, b.id);
    }

    // Commits a valid placement; returns the new building's index (or -1).
    int Place(const Placement& p, int def) {
        if (!p.valid) return -1;
        Placed b;
        b.id = nextId_++;
        b.def = def;
        b.c = p.c, b.front = p.front, b.y = p.y, b.track = p.track, b.platformInner = p.platformInner;
        b.haveShore = p.haveShore, b.shore = p.shore;
        items.push_back(b);
        ++version;
        return (int)items.size() - 1;
    }

    // game minutes pass
    void Tick(float minutes) {
        for (Placed& b : items) {
            if (!b.alive || b.state == BState::Done) continue;
            b.timer += minutes;
            if (b.state == BState::Planned) {
                if (b.timer >= kPlanMinutes) b.state = BState::Building, b.timer = 0, ++version;
            } else {
                b.progress = std::min(1.0f, b.progress + minutes / Defs()[b.def].buildMinutes);
                if (b.progress >= 1.0f) b.state = BState::Done, ++version;
            }
        }
    }

    // the building under a ground point, or -1
    int HitTest(V2 p) const {
        for (size_t i = items.size(); i-- > 0;)
            if (items[i].alive && Rect(items[i]).Contains(p, 1.0f)) return (int)i;
        return -1;
    }
    bool Remove(int i) {
        if (i < 0 || i >= (int)items.size() || !items[i].alive) return false;
        items[i].alive = false;
        removed.push_back(items[i].id);
        ++version;
        return true;
    }
    int Count() const {
        int n = 0;
        for (const Placed& b : items) n += b.alive ? 1 : 0;
        return n;
    }
    // zoning keeps out of these
    std::vector<BlockRect> Blockers() const {
        std::vector<BlockRect> v;
        for (const Placed& b : items)
            if (b.alive) v.push_back(Rect(b));
        return v;
    }
    static BlockRect Rect(const Placed& b) { return RectOf(b.c, b.front, Defs()[b.def]); }
    // Is a rail node within reach of the depot / terminal's front (the lot is "connected")?
    static bool RailLink(const Network& net, V2 c, V2 front, const BuildingDef& d) {
        const V2 gate = c + front * (d.D() * 0.5f + 14.0f);
        for (size_t n = 0; n < net.nodes.size(); ++n)
            if (net.nodes[n].alive && net.IsRailNode((int)n) && Len(XZ(net.nodes[n].p) - gate) < d.W() * 0.5f + 20.0f) return true;
        return false;
    }

private:
    uint64_t nextId_ = 1;

    static void Fail(Placement& r, const char* msg) {
        if (r.error.empty()) r.error = msg;
    }

    // ---- stations: onto a straight, long enough, ground-level stretch of railway
    bool PlanTrack(const PlaceContext& cx, const BuildingDef& D, V2 cursor, const PlanOptions& opt, Placement& r) const {
        if (!cx.net) return Fail(r, "No railway"), false;
        const Network& net = *cx.net;
        int best = -1;
        float bt = 0, bd = opt.snapTrack ? 45.0f : 8.0f;
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || !net.IsRailSeg((int)s)) continue;
            float t, d;
            net.Nearest((int)s, cursor, t, d);
            if (d < bd) bd = d, bt = t, best = (int)s;
        }
        if (best < 0) return Fail(r, "Stations must be placed on a railway"), false;
        const Cubic c = net.Curve(best);
        const float len = c.Length();
        const float need = D.W() + 8.0f;
        if (len < need) {
            char m[96];
            std::snprintf(m, sizeof(m), "Track too short for a station: it needs %.0f m in one piece", need);
            return Fail(r, m), r.c = c.At(bt), false;
        }
        const float half = (D.W() * 0.5f) / len, margin = (D.W() * 0.5f + 4.0f) / len;
        const float t = std::clamp(bt, margin, 1.0f - margin);
        r.c = c.At(t);
        const V2 T = c.Tan(t);
        const float sideSign = Dot2(cursor - r.c, LeftOf(T)) >= 0 ? 1.0f : -1.0f;
        r.front = LeftOf(T) * sideSign;
        r.track = best;
        r.snapped = true;
        r.y = net.YAt(net.segs[best], t);
        // the track must lie on the ground, straight and nearly level over the whole platform
        const V2 p0 = c.At(t - half), p1 = c.At(t + half), chord = Norm(p1 - p0);
        float dev = 0;
        for (int i = 0; i <= 12; ++i) {
            const V2 q = c.At(t - half + 2 * half * i / 12.0f);
            dev = std::max(dev, std::fabs(Cross2(q - p0, chord)));
        }
        const float dy = std::fabs(net.YAt(net.segs[best], t + half) - net.YAt(net.segs[best], t - half));
        if (std::fabs(r.y - cx.Ground(r.c)) > 2.0f) Fail(r, "The track must be on the ground");
        else if (dev > 2.5f) Fail(r, "The track is too curved here for a station");
        else if (dy / D.W() > 0.03f) Fail(r, "The track is too steep here for a station");
        // platforms start just outside the outermost track
        const Profile& p = net.Prof(best);
        float edge = p.Half(), outer = 0;
        for (const Strip& s : p.strips) {
            const float a0 = edge, a1 = edge - s.w;
            if (s.kind == Kind::Track) outer = std::max(outer, std::max(std::fabs(a0), std::fabs(a1)));
            edge = a1;
        }
        r.platformInner = outer + 0.9f;
        return true;
    }

    // ---- coasts: find the shoreline near the cursor, face the water, sit the lot's front on it
    static bool FindShore(const PlaceContext& cx, V2 cursor, V2& shore, V2& normal) {
        if (!cx.HasWater()) return false;
        const bool curWater = cx.IsWater(cursor);
        V2 q{};
        bool found = false;
        for (float rad = 4.0f; rad <= 160.0f && !found; rad += 4.0f) {
            const int n = std::max(8, (int)(6.2831853f * rad / 6.0f));
            float bestD = 1e30f;
            for (int k = 0; k < n; ++k) {
                const float a = 6.2831853f * k / n;
                const V2 p = cursor + V2{std::cos(a), std::sin(a)} * rad;
                if (cx.IsWater(p) != curWater) {
                    const float d = Len(p - cursor);
                    if (d < bestD) bestD = d, q = p, found = true;
                }
            }
        }
        if (!found) return false;
        V2 lo = cursor, hi = q; // bisect cursor -> q for the water line
        for (int i = 0; i < 14; ++i) {
            const V2 m = (lo + hi) * 0.5f;
            if (cx.IsWater(m) == curWater) lo = m;
            else hi = m;
        }
        shore = (lo + hi) * 0.5f;
        // the way to the water: the pull of every water sample around the shore point (straight coasts: the perpendicular)
        V2 sum{};
        int cnt = 0;
        for (float dz = -40; dz <= 40; dz += 5)
            for (float dx = -40; dx <= 40; dx += 5) {
                const V2 p = shore + V2{dx, dz};
                if (Len(p - shore) <= 40.0f && cx.IsWater(p)) sum = sum + (p - shore), ++cnt;
            }
        if (cnt < 6 || Len(sum) < 1e-3f) return false;
        normal = Norm(sum);
        return true;
    }
    bool PlanCoast(const PlaceContext& cx, const BuildingDef& D, V2 cursor, Placement& r) const {
        V2 shore, n;
        if (!FindShore(cx, cursor, shore, n)) return Fail(r, "Must be built on a coast: no shoreline nearby"), false;
        r.front = n;
        r.c = shore - n * (D.D() * (0.5f - D.dock));
        r.shore = shore;
        r.haveShore = true;
        r.snapped = true;
        return true;
    }

    // ---- everything the spot has to satisfy
    void Validate(const PlaceContext& cx, const BuildingDef& D, Placement& r) const {
        const Frame fr(r.c, r.front);
        const float W = D.W(), Dp = D.D();
        const BlockRect me = RectOf(r.c, r.front, D);
        // ground samples over the footprint
        const int nx = std::max(2, (int)std::ceil(W / 12.0f)), nz = std::max(2, (int)std::ceil(Dp / 12.0f));
        const float shoreZ = Dp * (0.5f - D.dock); // coast: the lot's z where the shoreline should be
        float lo = 1e30f, hi = -1e30f, sum = 0;
        int cnt = 0;
        bool wet = false, backWet = false;
        for (int j = 0; j <= nz; ++j)
            for (int i = 0; i <= nx; ++i) {
                const float x = -W * 0.5f + W * i / nx, z = -Dp * 0.5f + Dp * j / nz;
                const V2 p = fr.World(x, z);
                const float g = cx.Ground(p);
                if (D.site == Site::Coast) {
                    if (z <= shoreZ - 6.0f) { // the land behind the quay (a gentle beach is fine: the quay level is built up over it)
                        if (g < cx.sea - 0.1f) backWet = true;
                        lo = std::min(lo, g), hi = std::max(hi, g), sum += g, ++cnt;
                    }
                    continue;
                }
                if (cx.HasWater() && g < cx.sea + 0.3f) wet = true;
                lo = std::min(lo, g), hi = std::max(hi, g), sum += g, ++cnt;
            }
        r.relief = cnt ? hi - lo : 0.0f;
        if (D.site == Site::Coast) {
            if (!cx.HasWater()) Fail(r, "Must be built on a coast: this map has no sea");
            else if (backWet) Fail(r, "Not a qualifying coast: not enough solid land behind the shoreline");
            else if (r.relief > 9.0f) Fail(r, "Not a qualifying coast: the shore is too steep");
            // the apron in front of the lot: open water all the way (no sandbars, islets or rocks), and real ship depth at its
            // seaward end (a gently shelving beach is fine: the approach is dredged, the ships need the depth out there)
            if (r.error.empty()) {
                int total = 0, clearWater = 0, tail = 0, tailDeep = 0;
                float shallowestTail = 1e30f;
                const int cols = std::max(1, (int)(W / 12.0f));
                for (float z = Dp * 0.5f + 6.0f; z <= Dp * 0.5f + D.apron + 0.01f; z += 10.0f)
                    for (int i = 0; i <= cols; ++i) {
                        const float x = -W * 0.5f + W * i / cols;
                        const float depth = cx.sea - cx.Ground(fr.World(x, z));
                        ++total;
                        if (depth >= 0.2f) ++clearWater;
                        if (z >= Dp * 0.5f + D.apron * 0.66f) {
                            ++tail;
                            if (depth >= D.draft) ++tailDeep;
                            shallowestTail = std::min(shallowestTail, depth);
                        }
                    }
                r.waterDepth = tail ? shallowestTail : 0.0f;
                if (total && clearWater < (int)(total * 0.95f)) Fail(r, "Not a qualifying coast: the water in front is blocked by shallows or rock");
                else if (tail && tailDeep < (int)(tail * 0.85f)) Fail(r, "Not a qualifying coast: the water in front is too shallow for ships");
            }
            r.y = cx.sea + 2.4f;
        } else {
            if (wet) Fail(r, "Cannot be built on water");
            else if (r.relief > (D.model == Model::SmallAirfield || D.model == Model::RegionalAirport ? 9.0f : 7.0f) && D.site == Site::Flat) {
                char m[64];
                std::snprintf(m, sizeof(m), "Ground too uneven (%.0f m of relief)", r.relief);
                Fail(r, m);
            }
            if (D.site == Site::Flat) r.y = cnt ? sum / cnt : 0.0f;
        }
        // roads, rails (a station sits on its own track)
        if (cx.net && r.error.empty() && BlockedByNet(cx, me, r.track)) Fail(r, D.site == Site::Track ? "A road or another track is in the way" : "A road or railway is in the way");
        // other transport buildings
        if (r.error.empty())
            for (const Placed& b : items)
                if (b.alive && Overlaps(me, Rect(b), 2.0f)) {
                    Fail(r, "Overlaps another building");
                    break;
                }
        // zoned buildings and lots under construction
        if (cx.zones && r.error.empty()) {
            const float rad = std::sqrt(me.hw * me.hw + me.hd * me.hd) + kCell;
            cx.zones->CellsNear(r.c, rad, [&](int j) {
                const ZoneCell& z = cx.zones->cells[j];
                if (z.lot >= 0 && me.Contains(z.c, kCell * 0.6f)) Fail(r, "Overlaps a zoned building");
            });
        }
    }

    bool BlockedByNet(const PlaceContext& cx, const BlockRect& me, int hostSeg) const {
        const Network& net = *cx.net;
        const float reach = std::sqrt(me.hw * me.hw + me.hd * me.hd);
        for (size_t s = 0; s < net.segs.size(); ++s) {
            if (!net.segs[s].alive || (int)s == hostSeg) continue;
            const Cubic c = net.Curve((int)s);
            const float hw = net.Prof((int)s).Half() + 1.0f;
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (V2 p : {c.p0, c.p1, c.p2, c.p3}) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), z0 = std::min(z0, p.z), z1 = std::max(z1, p.z);
            if (me.c.x + reach + hw < x0 || me.c.x - reach - hw > x1 || me.c.z + reach + hw < z0 || me.c.z - reach - hw > z1) continue;
            const int n = std::max(4, (int)std::ceil(c.Length() / 4.0f));
            for (int i = 0; i <= n; ++i) {
                const float t = (float)i / n;
                const V2 p = c.At(t);
                const float y = net.YAt(net.segs[s], t), g = cx.Ground(p);
                if (y > g + 8.0f || y < g - 5.0f) continue; // a bridge or tunnel passes over / under
                if (me.Contains(p, hw)) return true;
            }
        }
        return false;
    }
};

// The footprint outline, the water apron (coast), the platforms (station) and the shoreline marker: lines for DrawTransient.
inline void MarkerLines(const BuildingDef& D, const Placement& p, const GroundFn& ground, std::vector<Vertex>& out, Vec3 col) {
    const Frame fr(p.c, p.front);
    auto Y = [&](V2 q) { return (ground ? ground(q.x, q.z) : 0.0f) + 0.5f; };
    auto line = [&](V2 a, V2 b, Vec3 c) {
        const int n = std::max(1, (int)(Len(b - a) / 6.0f));
        for (int i = 0; i < n; ++i) {
            const V2 s = Lerp2(a, b, (float)i / n), e = Lerp2(a, b, (float)(i + 1) / n);
            out.push_back({s.x, Y(s), s.z, 0, 0, 0, c.x, c.y, c.z, 1});
            out.push_back({e.x, Y(e), e.z, 0, 0, 0, c.x, c.y, c.z, 1});
        }
    };
    const float hw = D.W() * 0.5f, hd = D.D() * 0.5f;
    const V2 q[4] = {fr.World(-hw, -hd), fr.World(hw, -hd), fr.World(hw, hd), fr.World(-hw, hd)};
    for (int i = 0; i < 4; ++i) line(q[i], q[(i + 1) % 4], col);
    if (D.site == Site::Coast) {
        const Vec3 water{0.35f, 0.8f, 1.0f};
        const V2 a[4] = {fr.World(-hw, hd), fr.World(hw, hd), fr.World(hw, hd + D.apron), fr.World(-hw, hd + D.apron)};
        for (int i = 1; i < 4; ++i) line(a[i], a[(i + 1) % 4], water);
        if (p.haveShore) { // the shoreline under the lot's waterside
            line(fr.World(-hw, hd - D.D() * D.dock), fr.World(hw, hd - D.D() * D.dock), {1.0f, 0.9f, 0.4f});
        }
    }
    if (D.site == Site::Flat) { // [snap] the entrance: a door mark and the way in from the road (the default front without one)
        const bool have = p.entranceEdge >= 0;
        const V2 n = have ? p.entranceDir : p.front, mid = have ? p.entrance : fr.World(0, hd);
        const float edgeLen = std::fabs(Dot2(n, p.front)) > 0.5f ? D.W() : D.D();
        EntranceMark(mid, n, edgeLen, ground, out, have ? Vec3{0.35f, 1.0f, 0.55f} : Vec3{1.0f, 0.85f, 0.3f}); // green: on a road
    }
    if (IsRailYard(D)) { // the track ends: a short arrow out of each one the rail tool can join
        const Vec3 tc{1.0f, 0.85f, 0.3f};
        for (const RailPort& rp : YardPorts(D, p.c, p.front, p.y, p.entranceEdge >= 0 ? p.entranceDir : p.front)) {
            const V2 tip = rp.p + rp.dir * 6.0f, sd{rp.dir.z, -rp.dir.x};
            line(rp.p, tip, tc);
            line(tip, tip - rp.dir * 2.0f + sd * 1.2f, tc);
            line(tip, tip - rp.dir * 2.0f - sd * 1.2f, tc);
        }
    }
}

} // namespace city
} // namespace tbd
