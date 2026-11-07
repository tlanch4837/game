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
#include "core/jobs.h"
#include "glb_bounds.h"
#include "render/cloud_noise.h"
#include "render/mesh_weld.h"
#include "culling.h"
#include "mesh_lod.h"
#include "render/chunk_faces.h"
#include "gui/gui.h"
#include "gui/terrain_toolbar.h"
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
#include "city/sim_cells.h"
#include "city/sim_growth.h"
#include "city/sim_coverage.h"
#include "city/sim_economy.h"
#include "city/sim_budget.h"
#include "city/sim_milestones.h"
#include "city/news_feed.h"
#include "city/sim_flow.h"
#include "city/sim_agents.h"
#include "city/sim_people.h"
#include "gui/road_toolbar.h"
#include "city/road_names.h"
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
