// NPCs for the open world: Detour crowd agents on the tiled navmesh (nav_mesh.h), a small state machine per agent, the player's
// skinned character as the body. See docs/ai_navigation.md.
//
// Only agents near the player are simulated in full: a crowd agent, a kinematic Jolt capsule and one of a few skinned rigs
// (Wyatt's entities cloned by main.cpp through RigHost). The rest follow a stored path in a straight line with no skeleton, no
// capsule and no crowd. Everything is kept in AI space (ai_world.h); only the rig and the physics bodies are written in world space.
#pragma once
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ai/nav_mesh.h"
#include "physics.h"
#include "scene.h"
#include "skeletal_anim.h"

namespace tbd::ai {

enum class NpcState : uint8_t { Idle, Wander, Patrol, Follow, Flee, Indoors };
const char* StateName(NpcState state);

struct RigPart {
    int entity = -1;
    MeshId mesh = kInvalidMesh;
};

// What main.cpp provides to dress an NPC in the player's character. `create` appends the entities and their meshes to the scene.
struct RigHost {
    bool (*create)(std::vector<RigPart>& parts) = nullptr;
    void (*destroy)(std::vector<RigPart>& parts) = nullptr; // for every rig made, once, when Play stops
    float idleDuration = 1.0f, walkDuration = 1.0f, runDuration = 1.0f;
};

struct NpcFrame {
    Scene* scene = nullptr;
    SkeletalAnimator* animator = nullptr;
    PhysicsWorld* physics = nullptr;
    JobSystem* jobs = nullptr;
    Vec3 focus = {0, 0, 0};      // world: where the navmesh is wanted and who counts as near (the player)
    bool hasPlayer = false;
    float dt = 1.0f / 60.0f;
    float hour = 12.0f;          // the day clock, 0..24
};

struct NpcOptions {
    int count = 0;           // TBD_NPC_COUNT: NPCs added around the player on top of the ones placed in the scene
    int maxRigs = 12;        // TBD_NPC_RIGS: skinned bodies alive at once
    int maxAgents = 48;      // TBD_NPC_AGENTS: crowd agents (the cap on fully simulated NPCs)
    float fullRadius = 70.0f; // TBD_NPC_RADIUS
    int maxNpcs = 256;
};

struct NpcStats {
    int npcs = 0, full = 0, hidden = 0;
    int moveRequests = 0, stuckEvents = 0, goalFailures = 0;
    float maxStuckSeconds = 0.0f;
    double updateMs = 0.0;
};

class NpcSystem {
public:
    NpcSystem();
    ~NpcSystem();

    RigHost& Host();

    // Play starts: reads the TBD_NPC_* environment and the scene's NPC entities. End puts the entities back.
    void Begin(Scene& scene);
    void End(Scene& scene);
    bool Active() const;
    // True while NPCs are still to be placed (a test script should wait): the navmesh around the player is not there yet.
    bool Pending() const;

    void Update(const NpcFrame& frame);

    // TBD_NAVDEBUG: the navmesh polygons, agent paths and capsules. The same transient draw on both renderers.
    void DebugDraw(Renderer& r, const Mat4& viewProj, Vec3 focus);

    NavMesh& Nav();
    const NpcStats& Stats() const;
    // The scripted test's results (see TBD_NPC_REPORT in docs/ai_navigation.md).
    void WriteReport(FILE* out, const char* api) const;
    float SimulatedSeconds() const;
    // The world's origin moved: forwards to ai_world.h. Nothing built in AI space needs touching.
    static void ShiftWorld(Vec3 delta);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tbd::ai
