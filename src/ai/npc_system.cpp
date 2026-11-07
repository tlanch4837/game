// See npc_system.h. The behaviour table, the crowd and the level-of-detail rules are all in this file.
#define NOMINMAX
#include "ai/npc_system.h"

#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "ai/ai_world.h"
#include "player_character.h"

namespace tbd::ai {
namespace {

constexpr float kWalkSpeed = 1.35f, kRunSpeed = 3.4f;
constexpr float kPi = 3.14159265f;
constexpr float kTierPeriod = 0.3f;   // seconds between level-of-detail decisions
constexpr float kStuckReport = 5.0f;  // an agent that wants to move but does not for this long is reported
constexpr float kStuckRescue = 8.0f;  // and given a new goal at this point
constexpr int kMaxFrameSamples = 20000;

const char* kStateNames[] = {"idle", "wander", "patrol", "follow", "flee", "indoors"};

// What each kind of NPC does, as data. Weights are the chance, when an idle spell ends, of starting that activity.
struct Routine {
    const char* name;
    float idleMin, idleMax;
    float wander, patrol;
    bool faces, flees, follows, nightIndoors;
    float walk;
};
const Routine kRoutines[] = {
    //  name         idle     wander patrol faces  flees  follows night  walk
    {"townsfolk", 3.0f, 8.0f, 0.80f, 0.0f, true, false, false, true, kWalkSpeed},
    {"guard", 2.0f, 5.0f, 0.00f, 1.0f, true, false, false, false, 1.5f},
    {"loiterer", 6.0f, 15.0f, 0.15f, 0.0f, true, false, false, false, 1.2f},
    {"follower", 1.0f, 2.0f, 0.00f, 0.0f, true, false, true, false, kWalkSpeed},
    {"skittish", 2.0f, 6.0f, 0.70f, 0.0f, false, true, false, true, kWalkSpeed},
};
const Routine& FindRoutine(const std::string& name) {
    for (const Routine& r : kRoutines)
        if (name == r.name) return r;
    return kRoutines[0];
}

uint32_t Next(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}
float Rand01(uint32_t& s) { return (float)Next(s) / 16777216.0f; }
float RandRange(uint32_t& s, float a, float b) { return a + (b - a) * Rand01(s); }

float Flat(Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }
float TurnToward(float from, float to, float rate, float dt) {
    const float delta = std::fmod(to - from + kPi * 3.0f, kPi * 2.0f) - kPi;
    return from + delta * (1.0f - std::exp(-rate * dt));
}

struct Npc {
    int id = 0;
    NpcInfo info;
    const Routine* routine = &kRoutines[0];
    uint32_t rng = 1;
    float sleepAt = 21.0f, wakeAt = 6.0f; // staggered per NPC so a street does not empty in one second

    Vec3 pos = {0, 0, 0};  // AI space, feet
    Vec3 home = {0, 0, 0};
    Vec3 door = {0, 0, 0};
    std::vector<Vec3> patrol; // AI space
    float yaw = 0.0f, speed = 0.0f;

    NpcState state = NpcState::Idle;
    float idleLeft = 1.0f;
    bool hasGoal = false;
    Vec3 goal = {0, 0, 0};
    float goalSpeed = kWalkSpeed;
    int patrolNext = 0;
    float followRetarget = 0.0f;
    float fleeLeft = 0.0f;
    bool inside = false;

    bool full = false;
    int agent = -1, rig = -1;
    uint32_t body = 0;

    std::vector<Vec3> path; // cheap tier, AI space
    size_t pathAt = 0;
    uint32_t ticket = 0;
    bool waitingPath = false, pathPartial = false;
    uint32_t pathVersion = 0;
    float repathCooldown = 0.0f;

    float phase = 0.0f, idleTime = 0.0f;
    float stuck = 0.0f;
    int marker = -1;
    Vec3 markerSaved;
};

struct Rig {
    std::vector<RigPart> parts;
    int owner = -1;
};

} // namespace

const char* StateName(NpcState s) { return kStateNames[(int)s]; }

struct NpcSystem::Impl {
    NpcOptions options;
    RigHost host;
    NavMesh nav;
    dtCrowd* crowd = nullptr;
    dtNavMeshQuery* query = nullptr;
    int ghost = -1; // the player as a crowd agent that does not steer, so NPCs walk round him
    std::vector<Npc> npcs;
    std::vector<Rig> rigs;
    bool active = false, spawned = false, wantsSpawn = false;
    float waitedForNav = 0.0f, tierTimer = 0.0f, simTime = 0.0f;
    uint32_t seenVersion = 0;
    NpcStats stats;
    Vec3 playerAi = {0, 0, 0};
    bool hasPlayer = false;

    // test report
    std::vector<double> frameMs, updateMs;
    int64_t lastFrame = 0;
    struct StuckEvent { int id; Vec3 at; float time; };
    std::vector<StuckEvent> stuckEvents;
    float tierTimeFull = 0.0f, tierTimeCheap = 0.0f;

    // debug
    std::vector<Vertex> debugTris, debugLines;
    uint32_t debugVersion = 0;
    int debugCellX = 1 << 30, debugCellZ = 1 << 30;
    size_t debugSeeds = 0;

    void FreeCrowd() {
        if (crowd) dtFreeCrowd(crowd);
        crowd = nullptr;
        if (query) dtFreeNavMeshQuery(query);
        query = nullptr;
        ghost = -1;
    }
    void MakeCrowd() {
        FreeCrowd();
        crowd = dtAllocCrowd();
        crowd->init(options.maxAgents + 1, kAgentRadius + 0.1f, nav.Raw());
        dtObstacleAvoidanceParams oa;
        std::memcpy(&oa, crowd->getObstacleAvoidanceParams(0), sizeof(oa));
        oa.velBias = 0.4f;
        oa.adaptiveDivs = 7; oa.adaptiveRings = 2; oa.adaptiveDepth = 3;
        crowd->setObstacleAvoidanceParams(0, &oa);
        query = dtAllocNavMeshQuery();
        query->init(nav.Raw(), 512);
    }

    dtCrowdAgentParams AgentParams(float maxSpeed) const {
        dtCrowdAgentParams ap;
        std::memset(&ap, 0, sizeof(ap));
        ap.radius = kAgentRadius;
        ap.height = kAgentHeight;
        ap.maxAcceleration = 6.0f;
        ap.maxSpeed = maxSpeed;
        ap.collisionQueryRange = ap.radius * 12.0f;
        ap.pathOptimizationRange = ap.radius * 30.0f;
        ap.separationWeight = 2.0f;
        ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        ap.obstacleAvoidanceType = 0;
        return ap;
    }

    bool PolyAt(Vec3 ai, dtPolyRef& ref, float out[3]) const {
        const float p[3] = {ai.x, ai.y, ai.z}, ext[3] = {1.5f, 4.0f, 1.5f};
        ref = 0;
        if (!query) return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(1);
        query->findNearestPoly(p, ext, &filter, &ref, out);
        return ref != 0;
    }

    // ---- decisions ------------------------------------------------------------------------------------------------------

    void SetGoal(Npc& n, Vec3 goalAi, float speed) {
        n.hasGoal = true;
        n.goal = goalAi;
        n.goalSpeed = speed;
        n.stuck = 0.0f;
        ++stats.moveRequests;
        if (n.full && n.agent >= 0) {
            dtPolyRef ref;
            float p[3];
            if (PolyAt(goalAi, ref, p)) {
                crowd->requestMoveTarget(n.agent, ref, p);
                dtCrowdAgentParams ap = crowd->getAgent(n.agent)->params;
                ap.maxSpeed = speed;
                crowd->updateAgentParameters(n.agent, &ap);
            } else { n.hasGoal = false; ++stats.goalFailures; }
        } else {
            n.path.clear();
            n.pathAt = 0;
            n.pathPartial = false;
            n.waitingPath = true;
            n.pathVersion = nav.Version();
            n.ticket = 0; // requested by the caller that has the job system
        }
    }

    bool Arrived(const Npc& n) const {
        if (!n.hasGoal) return true;
        if (n.full) return Flat(n.pos, n.goal) < 0.7f;
        return !n.waitingPath && n.pathAt >= n.path.size() && Flat(n.pos, n.goal) < 1.5f;
    }

    void StopMoving(Npc& n) {
        n.hasGoal = false;
        n.waitingPath = false;
        n.path.clear();
        if (n.full && n.agent >= 0) {
            crowd->resetMoveTarget(n.agent);
        }
    }

    void Idle(Npc& n, float lo, float hi) {
        StopMoving(n);
        n.state = NpcState::Idle;
        n.idleLeft = RandRange(n.rng, lo, hi);
    }

    bool PickAround(Npc& n, Vec3 centreAi, float radius, Vec3& outAi) {
        Vec3 world;
        if (!nav.RandomPointAround(FromAi(centreAi), radius, world, (uint32_t)n.id * 977u + (uint32_t)(Next(n.rng) & 0xffff))) return false;
        outAi = ToAi(world);
        return true;
    }

    void Think(Npc& n, float dt, float hour) {
        const Routine& R = *n.routine;
        const float dPlayer = hasPlayer ? Flat(playerAi, n.pos) : 1e9f;
        const bool night = R.nightIndoors && n.info.hasIndoors && (hour >= n.sleepAt || hour < n.wakeAt);

        if (n.state == NpcState::Flee) {
            n.fleeLeft -= dt;
            if (n.fleeLeft <= 0.0f || (dPlayer > n.info.reactRadius * 3.0f && Arrived(n))) Idle(n, 1.0f, 3.0f);
        } else if (R.flees && !n.inside && dPlayer < n.info.reactRadius * 0.7f) {
            // away from the player, as far as the street allows
            Vec3 away = Normalize(Vec3{n.pos.x - playerAi.x, 0.0f, n.pos.z - playerAi.z});
            Vec3 target;
            Vec3 world;
            bool ok = nav.NearestPoint(FromAi(n.pos + away * 18.0f), {6.0f, 6.0f, 6.0f}, world);
            if (ok) target = ToAi(world);
            else ok = PickAround(n, n.pos, 25.0f, target);
            if (ok) {
                n.state = NpcState::Flee;
                n.fleeLeft = 8.0f;
                SetGoal(n, target, kRunSpeed);
            } else ++stats.goalFailures;
        }
        if (n.state == NpcState::Flee) return;

        if (night && n.state != NpcState::Indoors) {
            n.state = NpcState::Indoors;
            SetGoal(n, n.door, n.routine->walk);
        } else if (!night && n.state == NpcState::Indoors) {
            n.inside = false;
            n.pos = n.door;
            Idle(n, 0.5f, 2.0f);
        }

        switch (n.state) {
        case NpcState::Idle: {
            n.idleLeft -= dt;
            if (R.follows && hasPlayer && dPlayer < 60.0f && dPlayer > 3.0f) {
                n.state = NpcState::Follow;
                n.followRetarget = 0.0f;
                break;
            }
            if (n.idleLeft > 0.0f) break;
            const float roll = Rand01(n.rng);
            Vec3 target;
            if (!n.patrol.empty() && roll < R.patrol) {
                n.state = NpcState::Patrol;
                n.patrolNext %= (int)n.patrol.size();
                SetGoal(n, n.patrol[(size_t)n.patrolNext], R.walk);
            } else if (roll < R.wander + R.patrol) {
                if (PickAround(n, n.home, n.info.wanderRadius, target)) {
                    n.state = NpcState::Wander;
                    SetGoal(n, target, R.walk);
                } else { ++stats.goalFailures; Idle(n, R.idleMin, R.idleMax); }
            } else Idle(n, R.idleMin, R.idleMax);
            break;
        }
        case NpcState::Wander:
            if (Arrived(n)) Idle(n, R.idleMin, R.idleMax);
            else if (!n.hasGoal) Idle(n, 0.5f, 1.5f);
            break;
        case NpcState::Patrol:
            if (Arrived(n)) {
                n.patrolNext = n.patrol.empty() ? 0 : (n.patrolNext + 1) % (int)n.patrol.size();
                Idle(n, 1.0f, 3.0f);
            }
            break;
        case NpcState::Follow:
            if (!hasPlayer || dPlayer > 70.0f || dPlayer < 2.5f) { Idle(n, 0.5f, 1.5f); break; }
            n.followRetarget -= dt;
            if (n.followRetarget <= 0.0f) {
                n.followRetarget = 0.8f;
                Vec3 world;
                if (nav.NearestPoint(FromAi(playerAi), {2.0f, 4.0f, 2.0f}, world)) SetGoal(n, ToAi(world), dPlayer > 8.0f ? kRunSpeed : R.walk);
            }
            break;
        case NpcState::Indoors:
            if (!n.inside && Arrived(n)) { n.inside = true; StopMoving(n); }
            break;
        case NpcState::Flee: break;
        }
    }

    // ---- level of detail -------------------------------------------------------------------------------------------------

    int AcquireRig(int owner) {
        for (size_t i = 0; i < rigs.size(); ++i)
            if (rigs[i].owner < 0) { rigs[i].owner = owner; return (int)i; }
        if ((int)rigs.size() >= options.maxRigs || !host.create) return -1;
        Rig rig;
        if (!host.create(rig.parts)) return -1;
        rig.owner = owner;
        rigs.push_back(std::move(rig));
        return (int)rigs.size() - 1;
    }

    void Park(Rig& rig, Scene& scene) {
        rig.owner = -1;
        for (const RigPart& p : rig.parts)
            if (p.entity >= 0 && p.entity < (int)scene.entities.size()) scene.entities[(size_t)p.entity].position = {0, -10000.0f, 0};
    }

    void Promote(Npc& n, PhysicsWorld& physics) {
        dtPolyRef ref;
        float p[3];
        if (!PolyAt(n.pos, ref, p) || FullCount() >= options.maxAgents) return;
        const dtCrowdAgentParams ap = AgentParams(n.goalSpeed);
        const int agent = crowd->addAgent(p, &ap);
        if (agent < 0) return;
        n.agent = agent;
        n.full = true;
        n.pos = {p[0], p[1], p[2]};
        n.body = physics.CreateKinematicCapsule(FromAi(n.pos), kAgentRadius, kAgentHeight);
        n.path.clear();
        n.waitingPath = false;
        if (n.hasGoal) { const Vec3 g = n.goal; const float s = n.goalSpeed; SetGoal(n, g, s); }
    }

    void Demote(Npc& n, PhysicsWorld& physics, Scene& scene) {
        if (n.agent >= 0) crowd->removeAgent(n.agent);
        n.agent = -1;
        n.full = false;
        if (n.body) physics.DestroyKinematic(n.body);
        n.body = 0;
        if (n.rig >= 0) { Park(rigs[(size_t)n.rig], scene); n.rig = -1; }
        if (n.hasGoal) { const Vec3 g = n.goal; const float s = n.goalSpeed; SetGoal(n, g, s); }
    }

    int FullCount() const {
        int c = 0;
        for (const Npc& n : npcs) c += n.full;
        return c;
    }

    void AssignTiers(const NpcFrame& f) {
        std::vector<Npc*> order;
        for (Npc& n : npcs) order.push_back(&n);
        std::sort(order.begin(), order.end(), [&](Npc* a, Npc* b) { return Flat(a->pos, playerAi) < Flat(b->pos, playerAi); });
        int taken = 0;
        const int cap = std::min(options.maxAgents, host.create ? options.maxRigs : options.maxAgents);
        std::vector<Npc*> promote;
        for (Npc* n : order) {
            const float d = Flat(n->pos, playerAi);
            const bool covered = nav.HasTileAt(FromAi(n->pos));
            const bool want = hasPlayer && !n->inside && covered && taken < cap && d < (n->full ? options.fullRadius * 1.2f : options.fullRadius);
            if (want) ++taken;
            if (!want && n->full) Demote(*n, *f.physics, *f.scene);
            else if (want && !n->full) promote.push_back(n);
        }
        for (Npc* n : promote) {
            Promote(*n, *f.physics);
            if (n->full && host.create) {
                n->rig = AcquireRig(n->id);
                if (n->rig < 0) Demote(*n, *f.physics, *f.scene); // no body to show: stay cheap
            }
        }
    }

    // ---- movement --------------------------------------------------------------------------------------------------------

    void Cheap(Npc& n, const NpcFrame& f) {
        if (n.waitingPath) {
            if (n.ticket == 0 && f.jobs) {
                n.ticket = nav.FindPathAsync(*f.jobs, FromAi(n.pos), FromAi(n.goal));
            } else if (n.ticket) {
                PathResult r;
                if (nav.TakePath(n.ticket, r)) {
                    n.waitingPath = false;
                    n.ticket = 0;
                    n.path.clear();
                    for (const Vec3& p : r.points) n.path.push_back(ToAi(p));
                    n.pathAt = n.path.size() > 1 ? 1 : n.path.size();
                    n.pathPartial = r.partial;
                    n.pathVersion = nav.Version();
                    if (!r.found) { ++stats.goalFailures; n.hasGoal = false; }
                }
            }
        } else if (n.pathPartial && nav.Version() != n.pathVersion && n.path.size() > 0) {
            // More of the map arrived since the path stopped at the edge of it: ask again.
            n.pathPartial = false;
            n.waitingPath = true;
            n.ticket = 0;
            ++stats.moveRequests;
        }
        float moved = 0.0f;
        if (n.hasGoal && !n.waitingPath && n.pathAt < n.path.size()) {
            Vec3 to = n.path[n.pathAt] - n.pos;
            const float d = std::sqrt(to.x * to.x + to.z * to.z);
            const float step = n.goalSpeed * f.dt;
            if (d <= step) { n.pos = n.path[n.pathAt]; ++n.pathAt; moved = d; }
            else {
                n.pos = n.pos + Vec3{to.x / d * step, to.y / d * step * (std::sqrt(to.x * to.x + to.z * to.z) > 0 ? 1.0f : 0.0f), to.z / d * step};
                moved = step;
            }
            if (d > 0.01f) n.yaw = std::atan2(to.x, to.z);
        }
        n.speed = f.dt > 0 ? moved / f.dt : 0.0f;
    }

    void FullStep(Npc& n, const NpcFrame& f) {
        const dtCrowdAgent* ag = crowd->getAgent(n.agent);
        if (!ag || !ag->active || ag->state == DT_CROWDAGENT_STATE_INVALID) { Demote(n, *f.physics, *f.scene); return; }
        n.pos = {ag->npos[0], ag->npos[1], ag->npos[2]};
        float h;
        const float p[3] = {ag->npos[0], ag->npos[1], ag->npos[2]};
        if (query && dtStatusSucceed(query->getPolyHeight(ag->corridor.getFirstPoly(), p, &h))) n.pos.y = h;
        const float v = std::sqrt(ag->vel[0] * ag->vel[0] + ag->vel[2] * ag->vel[2]);
        n.speed += (v - n.speed) * (1.0f - std::exp(-8.0f * f.dt));
        if (v > 0.25f) n.yaw = TurnToward(n.yaw, std::atan2(ag->vel[0], ag->vel[2]), 9.0f, f.dt);
        else if (n.state == NpcState::Idle && n.routine->faces && hasPlayer && Flat(playerAi, n.pos) < n.info.reactRadius)
            n.yaw = TurnToward(n.yaw, std::atan2(playerAi.x - n.pos.x, playerAi.z - n.pos.z), 5.0f, f.dt);
        if (n.body) f.physics->MoveKinematic(n.body, FromAi(n.pos));
        // Stuck: wants to go, is not going.
        const bool moving = n.hasGoal && n.state != NpcState::Idle && !Arrived(n);
        if (moving && v < 0.12f) {
            n.stuck += f.dt;
            if (n.stuck > kStuckReport && n.stuck - f.dt <= kStuckReport) {
                ++stats.stuckEvents;
                stuckEvents.push_back({n.id, FromAi(n.pos), simTime});
                Log("NPC: %d is stuck at %.1f %.1f %.1f in state %s", n.id, FromAi(n.pos).x, FromAi(n.pos).y, FromAi(n.pos).z, StateName(n.state));
            }
            stats.maxStuckSeconds = std::max(stats.maxStuckSeconds, n.stuck);
            if (n.stuck > kStuckRescue) { Idle(n, 0.2f, 0.5f); n.stuck = 0.0f; }
        } else n.stuck = 0.0f;
    }

    void PoseRig(Npc& n, const NpcFrame& f) {
        if (n.rig < 0) return;
        Rig& rig = rigs[(size_t)n.rig];
        const Vec3 feet = FromAi(n.pos);
        const Quat rotation = QuatAxisAngle({0, 1, 0}, n.yaw);
        const float speed = n.speed;
        const float toWalk = std::clamp(speed / PlayerCharacter::kWalkClipSpeed, 0.0f, 1.0f);
        const float toRun = std::clamp((speed - PlayerCharacter::kWalkSpeed) / (PlayerCharacter::kRunSpeed - PlayerCharacter::kWalkSpeed), 0.0f, 1.0f);
        const float walkRate = speed / PlayerCharacter::kWalkClipSpeed / std::max(host.walkDuration, 0.01f);
        const float runRate = speed / PlayerCharacter::kRunClipSpeed / std::max(host.runDuration, 0.01f);
        n.phase = std::fmod(n.phase + f.dt * (walkRate + (runRate - walkRate) * toRun), 1.0f);
        n.idleTime += f.dt;
        const float times[4] = {std::fmod(n.idleTime, std::max(host.idleDuration, 0.01f)), n.phase * host.walkDuration,
                                std::fmod(n.phase + PlayerCharacter::kRunPhaseOffset, 1.0f) * host.runDuration, 0.3f};
        const float weights[4] = {1.0f - toWalk, toWalk * (1.0f - toRun), toWalk * toRun, 0.0f};
        for (const RigPart& p : rig.parts) {
            if (p.entity < 0 || p.entity >= (int)f.scene->entities.size()) continue;
            Entity& e = f.scene->entities[(size_t)p.entity];
            e.position = {feet.x, feet.y + PlayerCharacter::kFootOffset, feet.z};
            e.rotation = rotation;
            f.animator->SetBlendPose(p.mesh, times, weights);
        }
    }

    // ---- spawning --------------------------------------------------------------------------------------------------------

    Npc* AddNpc(const NpcInfo& info, Vec3 worldPos, uint32_t seed) {
        if ((int)npcs.size() >= options.maxNpcs) return nullptr;
        Npc n;
        n.id = (int)npcs.size();
        n.info = info;
        n.routine = &FindRoutine(info.routine);
        n.rng = seed * 2654435761u + 7u;
        const float jitter = Rand01(n.rng);
        n.sleepAt = 20.5f + jitter * 1.5f;
        n.wakeAt = 5.5f + Rand01(n.rng) * 1.5f;
        n.pos = n.home = ToAi(worldPos);
        n.yaw = Rand01(n.rng) * kPi * 2.0f - kPi;
        n.phase = Rand01(n.rng);
        n.idleTime = Rand01(n.rng) * 10.0f;
        n.idleLeft = RandRange(n.rng, 0.5f, n.routine->idleMax);
        for (const Vec3& p : info.patrol) n.patrol.push_back(ToAi(p));
        if (info.hasIndoors) n.door = ToAi(info.indoors);
        else n.door = n.home;
        npcs.push_back(std::move(n));
        return &npcs.back();
    }

    void Spawn(const NpcFrame& f, Scene& scene) {
        spawned = true;
        int placed = 0;
        // Placed in the scene: snap each marker to the navmesh.
        for (size_t i = 0; i < scene.entities.size(); ++i) {
            Entity& e = scene.entities[i];
            if (!e.npc.enabled) continue;
            Vec3 snapped;
            if (!nav.NearestPoint(e.position, {3.0f, 10.0f, 3.0f}, snapped)) {
                Log("NPC: %s is not near the navmesh (%.1f %.1f %.1f), skipped", e.name.c_str(), e.position.x, e.position.y, e.position.z);
                continue;
            }
            Npc* n = AddNpc(e.npc, snapped, (uint32_t)(i * 31 + 5));
            if (!n) break;
            n->marker = (int)i;
            n->markerSaved = e.position;
            e.position = {e.position.x, -10000.0f, e.position.z};
            if (!e.npc.hasIndoors) n->door = n->home;
            ++placed;
        }
        // TBD_NPC_COUNT: scattered around the player on ground reachable from where the player stands.
        const int extra = options.count;
        for (int i = 0; i < extra; ++i) {
            Vec3 at;
            bool ok = false;
            for (int attempt = 0; attempt < 10 && !ok; ++attempt)
                ok = nav.RandomPointAround(f.focus, 55.0f, at, (uint32_t)(i * 7919 + attempt * 104729 + 1));
            if (!ok) { ++stats.goalFailures; continue; }
            NpcInfo info;
            info.enabled = true;
            const int kind = i % 6;
            info.routine = kind <= 2 ? "townsfolk" : kind == 3 ? "guard" : kind == 4 ? "loiterer" : "skittish";
            info.wanderRadius = 18.0f;
            info.reactRadius = 6.0f;
            Npc* n = AddNpc(info, at, (uint32_t)(1000 + i));
            if (!n) break;
            Vec3 other;
            if (kind != 4 && nav.RandomPointAround(at, 40.0f, other, (uint32_t)(i * 31 + 3))) {
                n->info.hasIndoors = true;
                n->door = ToAi(other);
            }
            if (kind == 3) {
                for (int k = 0; k < 3; ++k) {
                    Vec3 p;
                    if (nav.RandomPointAround(at, 30.0f, p, (uint32_t)(i * 131 + k * 17 + 9))) n->patrol.push_back(ToAi(p));
                }
            }
            ++placed;
        }
        stats.npcs = (int)npcs.size();
        Log("NPC: %d placed (%d from the scene, %d requested)", placed, placed - std::min(placed, extra), extra);
    }
};

NpcSystem::NpcSystem() : impl_(std::make_unique<Impl>()) {}
NpcSystem::~NpcSystem() { impl_->FreeCrowd(); }

static int EnvInt(const char* name, int fallback) {
    char v[32] = {};
    return GetEnvironmentVariableA(name, v, sizeof(v)) > 0 ? std::atoi(v) : fallback;
}

void NpcSystem::Begin(Scene& scene) {
    Impl& w = *impl_;
    End(scene);
    w.options = NpcOptions();
    w.options.count = std::clamp(EnvInt("TBD_NPC_COUNT", 0), 0, w.options.maxNpcs);
    w.options.maxRigs = std::clamp(EnvInt("TBD_NPC_RIGS", w.options.maxRigs), 0, 64);
    w.options.maxAgents = std::clamp(EnvInt("TBD_NPC_AGENTS", w.options.maxAgents), 1, 128);
    w.options.fullRadius = (float)std::clamp(EnvInt("TBD_NPC_RADIUS", 70), 10, 200);
    bool authored = false;
    for (const Entity& e : scene.entities) authored = authored || e.npc.enabled;
    w.wantsSpawn = authored || w.options.count > 0;
    w.spawned = false;
    w.npcs.clear();
    w.stats = NpcStats();
    w.stuckEvents.clear();
    w.frameMs.clear();
    w.updateMs.clear();
    w.lastFrame = {};
    w.simTime = 0.0f;
    w.waitedForNav = 0.0f;
    w.seenVersion = 0;
    w.tierTimer = 0.0f;
    if (!w.wantsSpawn) return;
    w.nav.Reset();
    w.MakeCrowd();
    w.active = true;
    Log("NPC: Play began with %d requested, %zu rigs max, %d agents max, full radius %.0f m", w.options.count, (size_t)w.options.maxRigs, w.options.maxAgents, w.options.fullRadius);
}

void NpcSystem::End(Scene& scene) {
    Impl& w = *impl_;
    for (Npc& n : w.npcs)
        if (n.marker >= 0 && n.marker < (int)scene.entities.size()) scene.entities[(size_t)n.marker].position = n.markerSaved;
    w.FreeCrowd();
    for (size_t i = w.rigs.size(); i-- > 0;) // newest first: the host removes them from the end of the scene
        if (w.host.destroy) w.host.destroy(w.rigs[i].parts);
    w.rigs.clear();
    w.npcs.clear();
    w.active = false;
}

RigHost& NpcSystem::Host() { return impl_->host; }
bool NpcSystem::Active() const { return impl_->active; }
bool NpcSystem::Pending() const { return impl_->active && !impl_->spawned; }
NavMesh& NpcSystem::Nav() { return impl_->nav; }
const NpcStats& NpcSystem::Stats() const { return impl_->stats; }
float NpcSystem::SimulatedSeconds() const { return impl_->simTime; }
void NpcSystem::ShiftWorld(Vec3 delta) { ai::ShiftWorld(delta); }

void NpcSystem::Update(const NpcFrame& f) {
    Impl& w = *impl_;
    if (!w.active || !f.scene || !f.physics || !f.jobs) return;
    const int64_t t0 = tbd::platform::Ticks();
    if (w.lastFrame && w.spawned && (int)w.frameMs.size() < kMaxFrameSamples)
        w.frameMs.push_back(tbd::platform::TicksToMs(t0 - w.lastFrame));
    w.lastFrame = t0;

    const float dt = std::min(f.dt, 0.1f);
    w.nav.Update(*f.physics, *f.jobs, f.focus);
    w.hasPlayer = f.hasPlayer;
    w.playerAi = ToAi(f.focus);

    if (!w.spawned) {
        w.waitedForNav += dt;
        if (w.nav.HasTileAt(f.focus) && (w.nav.Settled() || w.waitedForNav > 25.0f)) w.Spawn(f, *f.scene);
        else return;
    }

    w.simTime += dt;
    // Player as a non-steering crowd agent, so the others walk round him.
    if (w.crowd && f.hasPlayer) {
        dtPolyRef ref;
        float p[3];
        if (w.PolyAt(w.playerAi, ref, p)) {
            if (w.ghost < 0) {
                dtCrowdAgentParams ap = w.AgentParams(6.0f);
                ap.updateFlags = 0;
                ap.maxAcceleration = 60.0f;
                w.ghost = w.crowd->addAgent(p, &ap);
            }
            if (w.ghost >= 0) w.crowd->requestMoveTarget(w.ghost, ref, p);
        }
    }
    w.tierTimer -= dt;
    if (w.tierTimer <= 0.0f && f.hasPlayer) { w.tierTimer = kTierPeriod; w.AssignTiers(f); }

    for (Npc& n : w.npcs) w.Think(n, dt, f.hour);
    if (w.crowd) w.crowd->update(dt, nullptr);

    int full = 0, hidden = 0;
    for (Npc& n : w.npcs) {
        if (n.inside) { ++hidden; n.speed = 0.0f; if (n.rig >= 0 || n.full) w.Demote(n, *f.physics, *f.scene); continue; }
        NpcFrame local = f;
        local.dt = dt;
        if (n.full) { w.FullStep(n, local); w.PoseRig(n, local); ++full; w.tierTimeFull += dt; }
        else { w.Cheap(n, local); w.tierTimeCheap += dt; }
    }
    w.stats.full = full;
    w.stats.hidden = hidden;
    const int64_t t1 = tbd::platform::Ticks();
    w.stats.updateMs = tbd::platform::TicksToMs(t1 - t0);
    if ((int)w.updateMs.size() < kMaxFrameSamples) w.updateMs.push_back(w.stats.updateMs);
}

void NpcSystem::DebugDraw(Renderer& r, const Mat4& viewProj, Vec3 focus) {
    Impl& w = *impl_;
    if (!w.active) return;
    // The navmesh polygons are regenerated when tiles change, the focus has moved 10 m, or the agents list changed size.
    const Vec3 a = ToAi(focus);
    const int cx = (int)std::floor(a.x / 10.0f), cz = (int)std::floor(a.z / 10.0f);
    if (w.debugVersion != w.nav.Version() || cx != w.debugCellX || cz != w.debugCellZ) {
        w.debugVersion = w.nav.Version();
        w.debugCellX = cx;
        w.debugCellZ = cz;
        w.debugTris.clear();
        w.debugLines.clear();
        std::vector<Vec3> seeds;
        seeds.push_back(focus);
        for (const Npc& n : w.npcs) if (!n.inside) seeds.push_back(FromAi(n.pos));
        w.nav.DebugGeometry(focus, 70.0f, seeds, w.debugTris, w.debugLines);
        if (w.debugTris.size() > 150000) w.debugTris.resize(150000 / 3 * 3);
        if (w.debugLines.size() > 100000) w.debugLines.resize(100000 / 2 * 2);
    }
    std::vector<Vertex> lines = w.debugLines;
    auto line = [&](Vec3 p, Vec3 q, Vec3 c) {
        lines.push_back({p.x, p.y, p.z, 0, 0, 0, c.x, c.y, c.z, 1.0f});
        lines.push_back({q.x, q.y, q.z, 0, 0, 0, c.x, c.y, c.z, 1.0f});
    };
    const Vec3 colours[] = {{1.0f, 1.0f, 1.0f}, {0.2f, 0.9f, 1.0f}, {1.0f, 0.9f, 0.2f}, {1.0f, 0.3f, 1.0f}, {1.0f, 0.2f, 0.2f}, {0.6f, 0.6f, 0.6f}};
    for (const Npc& n : w.npcs) {
        if (n.inside) continue;
        const Vec3 feet = FromAi(n.pos);
        const Vec3 c = n.stuck > 1.0f ? Vec3{1.0f, 0.0f, 0.0f} : colours[(int)n.state];
        if (n.full) {
            const int seg = 12;
            for (int i = 0; i < seg; ++i) {
                const float a0 = (float)i / seg * kPi * 2.0f, a1 = (float)(i + 1) / seg * kPi * 2.0f;
                const Vec3 p0 = {std::cos(a0) * kAgentRadius, 0, std::sin(a0) * kAgentRadius}, p1 = {std::cos(a1) * kAgentRadius, 0, std::sin(a1) * kAgentRadius};
                line(feet + p0 + Vec3{0, 0.05f, 0}, feet + p1 + Vec3{0, 0.05f, 0}, c);
                line(feet + p0 + Vec3{0, kAgentHeight, 0}, feet + p1 + Vec3{0, kAgentHeight, 0}, c);
                if (i % 3 == 0) line(feet + p0 + Vec3{0, 0.05f, 0}, feet + p0 + Vec3{0, kAgentHeight, 0}, c);
            }
            const Vec3 fwd = {std::sin(n.yaw) * 0.7f, 0, std::cos(n.yaw) * 0.7f};
            line(feet + Vec3{0, 1.2f, 0}, feet + Vec3{0, 1.2f, 0} + fwd, {1, 1, 1});
            if (n.agent >= 0) {
                const dtCrowdAgent* ag = w.crowd->getAgent(n.agent);
                Vec3 prev = feet + Vec3{0, 0.2f, 0};
                for (int k = 0; ag && k < ag->ncorners; ++k) {
                    const Vec3 corner = FromAi({ag->cornerVerts[k * 3], ag->cornerVerts[k * 3 + 1] + 0.2f, ag->cornerVerts[k * 3 + 2]});
                    line(prev, corner, {1.0f, 0.85f, 0.1f});
                    prev = corner;
                }
            }
        } else {
            // a diamond, so the cheap tier is visible
            const Vec3 up = feet + Vec3{0, 1.2f, 0};
            const Vec3 d[4] = {{0.4f, 0, 0}, {0, 0, 0.4f}, {-0.4f, 0, 0}, {0, 0, -0.4f}};
            for (int i = 0; i < 4; ++i) {
                line(up + d[i], up + d[(i + 1) % 4], {1.0f, 0.55f, 0.1f});
                line(up + d[i], up + Vec3{0, 0.5f, 0}, {1.0f, 0.55f, 0.1f});
                line(up + d[i], up - Vec3{0, 0.5f, 0}, {1.0f, 0.55f, 0.1f});
            }
            Vec3 prev = feet + Vec3{0, 0.2f, 0};
            for (size_t k = n.pathAt; k < n.path.size(); ++k) {
                const Vec3 p = FromAi(n.path[k]) + Vec3{0, 0.2f, 0};
                line(prev, p, {1.0f, 0.55f, 0.1f});
                prev = p;
            }
        }
    }
    if (!w.debugTris.empty()) r.DrawTransient(w.debugTris.data(), (uint32_t)w.debugTris.size(), Topology::Triangles, viewProj, Mat4::Identity(), true);
    if (!lines.empty()) r.DrawTransient(lines.data(), (uint32_t)lines.size(), Topology::Lines, viewProj, Mat4::Identity(), true);
}

static double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))];
}

void NpcSystem::WriteReport(FILE* out, const char* api) const {
    const Impl& w = *impl_;
    const NavStats& s = w.nav.Stats();
    fprintf(out, "NPC report (%s)\n", api);
    fprintf(out, "simulated_seconds=%.1f npcs=%d full_now=%d hidden_now=%d\n", w.simTime, w.stats.npcs, w.stats.full, w.stats.hidden);
    const int built = s.tilesBuilt.load();
    fprintf(out, "tiles_built=%d tiles_from_cache=%d tiles_empty=%d tiles_failed=%d tiles_loaded=%zu\n", built, s.tilesFromCache.load(), s.tilesEmpty.load(), s.tilesFailed.load(), w.nav.TileCount());
    fprintf(out, "build_ms_total=%.0f build_ms_avg=%.0f build_ms_max=%.0f cache_read_ms_total=%.1f\n", s.buildMsTotal.load(), built ? s.buildMsTotal.load() / built : 0.0, s.buildMsMax.load(), s.cacheMsTotal.load());
    fprintf(out, "path_requests=%d path_partial=%d path_failures=%d move_requests=%d goal_failures=%d\n", s.pathRequests.load(), s.pathPartial.load(), s.pathFailures.load(), w.stats.moveRequests, w.stats.goalFailures);
    fprintf(out, "stuck_over_5s=%d max_stuck_seconds=%.1f\n", w.stats.stuckEvents, w.stats.maxStuckSeconds);
    for (const Impl::StuckEvent& e : w.stuckEvents) fprintf(out, "  stuck npc=%d at=%.1f %.1f %.1f t=%.1f\n", e.id, e.at.x, e.at.y, e.at.z, e.time);
    fprintf(out, "frame_ms p50=%.2f p99=%.2f max=%.2f samples=%zu\n", Percentile(w.frameMs, 0.5), Percentile(w.frameMs, 0.99), Percentile(w.frameMs, 1.0), w.frameMs.size());
    fprintf(out, "npc_update_ms p50=%.3f p99=%.3f\n", Percentile(w.updateMs, 0.5), Percentile(w.updateMs, 0.99));
    fprintf(out, "npc_seconds_full=%.0f npc_seconds_cheap=%.0f rigs=%zu kinematic_bodies_now=%zu\n", w.tierTimeFull, w.tierTimeCheap, w.rigs.size(), (size_t)0);
}

} // namespace tbd::ai
