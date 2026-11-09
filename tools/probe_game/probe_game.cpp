// Portability probe: compiles the game (src/game/game_core.inl) the way the iOS target will, with only portable
// headers and no windows.h, on Windows. The errors it reports are what still ties the game to Windows.
// Build: tools\probe_game\probe_game.bat (compile only, nothing is linked or run).
#define TBD_NO_WIN32_HEADERS 1

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "platform/platform.h"
#include "game/shell.h"
#include "bmp_header.h"
#include "surface_maps.h"
#include "material_lab.h"
#include "audio.h"
#include "camera.h"
#include "city_camera.h"
#include "core/jobs.h"
#include "glb_bounds.h"
#include "render/cloud_noise.h"
#include "render/mesh_weld.h"
#include "culling.h"
#include "mesh_lod.h"
#include "render/chunk_faces.h"
#include "gui/gui.h"
#include "gui/terrain_toolbar.h"
#include "gui/glass_dock.h"    // Glass Dock+: the mobile / tablet GUI (game/glass_dock_host.inl)
#include "gui/road_toolkit.h"  // [road toolkit] the road / rail touch panel + palette (game/glass_dock_host.inl RoadKit)
#include "gui/road_reticle.h"  // [road ux] the aim dot (game/glass_dock_host.inl RoadKit)
#include "gui/terrain_kit.h"   // [road ux] the compact terrain tools (game/glass_dock_host.inl TerrainKit)
#include "gui/title_screen.h"  // [title] the loading screen, title screen and slot picker (game/title_host.inl)
#include "game/save_slots.h"   // [title] the ten save slots (city_game.inl, game/title_host.inl)
#include "gui/city_hud.h"
#include "gui/city_overlays.h"
#include "gui/zone_toolbar.h"
#include "gui/transport_toolbar.h"
#include "gui/utility_toolbar.h"
#include "city/transport_models.h"
#include "city/industry.h"
#include "city/utilities.h"
#include "city/sim.h"
#include "city/sim_children.h"
#include "city/sim_routes.h"
#include "city/sim_town_traffic.h"
#include "city/sim_cells.h"
#include "city/sim_growth.h"
#include "city/sim_coverage.h"
#include "city/sim_economy.h"
#include "city/sim_budget.h"
#include "city/sim_milestones.h"
#include "city/news_feed.h"
#include "city/news_adjacency.h"   // a new building's neighbours in its radio story (city_radio.inl)
#include "city/sim_flow.h"
#include "city/sim_agents.h"
#include "city/sim_people.h"
#include "city/sim_save.h"         // the CS2 sim's .tbdsim chunks and BuildingRegistry (city_sim_save.inl)
#include "city/sim_tuning_table.h" // the CS2 phases' prefixed tuning (city_sim.inl g_simPhaseTune)
#include "city/sim_calendar.h"     // minute -> day, life month, season (city_sim_save.inl)
#include "city/sim_walkers.h"      // [walkers] people on the sidewalks (city_walkers.inl hosts them; included here: game_core.inl sits in a namespace)
#include "city/sim_life_save.h"      // Phase 1 citizens, households, jobs, migration and their .tbdsim chunks (city_sim_p1.inl)
#include "city/sim_life_views.h"     // Phase 1 Population panel, building inspector, active tier
#include "city/sim_life_cells.h"     // Phase 1 records into CitySim cells and links
#include "city/sim_factors.h"        // the phases' shared CellFactors (sim roadmap 3.2)
#include "city/sim_env_save.h"       // Phase 3 environment and its .tbdsim chunks (city_sim_p3.inl)
#include "city/sim_goods_save.h"     // Phase 6 goods, companies, trade and their .tbdsim chunks (city_sim_p6.inl)
#include "city/sim_gov_save.h"       // Phase 7 budget, policies, districts, progression and their .tbdsim chunks (city_sim_p7.inl)
#include "city/sim_land_save.h"      // Phase 2 demand, land value, rent, levelling and their .tbdsim chunks (city_sim_p2.inl)
#include "city/sim_transit_save.h"   // Phase 5 transit lines, mode choice, outside connections and their .tbdsim chunks (city_sim_p5.inl)
#include "city/sim_svc.h"            // Phase 4 services, incidents, seats, garbage and their .tbdsim chunks (city_sim_p4.inl)
#include "city/sim_preset.h"         // the sim's per-preset and thermal scaling (city_sim.inl SimApplyPreset)
#include "gfx_settings.h"             // the player's graphics preset (the sim scales with it)
#include "gui/road_toolbar.h"
#include "city/road_names.h"
#include "city/city_interact.h" // [interact]
#include "gui/info_card.h"      // [interact]
#include "undo.h"
#include "gizmo.h"
#include "mesh_import.h"
#include "skeletal_anim.h"
#include "image_decode.h"
#include "player_character.h"
#include "ai/npc_system.h"
#include "profiler_overlay.h"
#include "project.h"
#include "render/renderer.h"
#include "scene.h"
#include "scene_io.h"
#include "stats.h"
#include "sun.h"
#include "max_look.h"
#include "terrain.h"
#include "tools.h"
#include "water.h"
#include "window_portals.h"
#include "physics.h"
#include "script/api_registry.h"
#include "script/luau_host.h"
#include "script/script_remote.h"
#include "play_mode.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS)
#error windows.h leaked into the portable includes
#endif

namespace {
#include "game/game_core.inl"
} // namespace
