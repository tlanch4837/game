// Tiled navigation mesh for the open world: Recast builds, Detour answers. See docs/ai_navigation.md.
//
// Tiles are 64 m squares. Update() (main thread, once a frame) watches the physics streaming: a tile within kBuildRadius of the
// focus whose collision has loaded is gathered from Jolt on the main thread, then rasterized and meshed on a background job (or
// read from bin\navcache when its triangles hash to a file already there), and added to the Detour mesh on the main thread.
// Tiles far from the focus are removed. Recast and Detour types stay inside nav_mesh.cpp; every position here is WORLD space
// (ai_world.h converts to the fixed AI space the mesh is stored in).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "physics.h"
#include "render/renderer.h"
#include "vecmath.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace tbd::ai {

constexpr float kTileSize = 64.0f;
constexpr float kAgentRadius = 0.4f, kAgentHeight = 1.8f, kAgentStep = 0.4f, kAgentSlope = 45.0f;

struct NavStats {
    std::atomic<int> tilesBuilt{0}, tilesFromCache{0}, tilesEmpty{0}, tilesFailed{0};
    std::atomic<int> pathRequests{0}, pathFailures{0}, pathPartial{0};
    std::atomic<int> buildJobsRunning{0};
    std::atomic<double> buildMsTotal{0.0}, buildMsMax{0.0}, cacheMsTotal{0.0};
};

struct PathResult {
    std::vector<Vec3> points; // from the start to the end (or the last reachable point), world space
    bool found = false;       // at least a start point exists on the mesh and points is not empty
    bool partial = false;     // did not reach `to`: unloaded tile, unreachable, or search ran out
    Vec3 goal = {0, 0, 0};    // the destination that was asked for
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();

    // Forgets every tile (a new scene or Play started over).
    void Reset();
    // Main thread, each frame. `focus` is the player. Starts at most a few builds, adds finished tiles, drops far ones.
    void Update(PhysicsWorld& physics, JobSystem& jobs, Vec3 focus);

    // Counts up whenever a tile is added or removed: a partial path should be asked for again when it changes.
    uint32_t Version() const { return version_.load(std::memory_order_acquire); }
    size_t TileCount() const;
    bool HasTileAt(Vec3 world) const;
    // True when no build is in flight and nothing is waiting for its collision: the area near focus has settled.
    bool Settled() const;

    // Synchronous queries (main thread). A destination on an unloaded tile gives a partial path to the nearest loaded point.
    PathResult FindPath(Vec3 from, Vec3 to) const;
    // True when a straight walk is blocked; `hit` is where it is (or `to` when clear). Both points must be on the mesh.
    bool Raycast(Vec3 from, Vec3 to, Vec3& hit) const;
    bool NearestPoint(Vec3 p, Vec3 halfExtents, Vec3& out) const;
    // A random walkable point reachable from `from` within `radius`; false when `from` is off the mesh.
    bool RandomPointAround(Vec3 from, float radius, Vec3& out, uint32_t seed) const;

    // Async: the search runs on a worker. Poll the ticket on later frames; each ticket can be taken once.
    uint32_t FindPathAsync(JobSystem& jobs, Vec3 from, Vec3 to);
    bool TakePath(uint32_t ticket, PathResult& out);

    // Debug drawing (D3D12 and Vulkan alike: the renderer's transient draw takes these). Polygons within `radius` of `around`.
    // Reachable polygons are those connected to a seed; the rest (rooftops, courtyards) are drawn dim red.
    void DebugGeometry(Vec3 around, float radius, const std::vector<Vec3>& seeds, std::vector<Vertex>& triangles, std::vector<Vertex>& lines) const;

    // For the crowd (npc_system.cpp). Valid until the next Reset().
    dtNavMesh* Raw() const;
    // Reader lock for work that runs beside tile changes; the crowd updates on the main thread, which is the only writer.
    const NavStats& Stats() const { return stats_; }
    NavStats& MutableStats() { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<uint32_t> version_{1};
    mutable NavStats stats_;
};

} // namespace tbd::ai
