// Physics: Jolt (third_party/JoltPhysics, MIT) behind a header with no Jolt types, so only physics.cpp sees it.
//
// Nothing here exists until Play starts (setup is lazy) and nothing runs during a TBD_SHOT / TBD_CAPTURE: see OnTransport.
// Static collision streams around a focus point like world_streaming.inl does for drawing, but on its own (shorter) radius:
// scene entities and scattered rocks become bodies, built on background jobs, added on the main thread. layout.json's
// `collision` field picks the shape: complex = triangle mesh, simple = convex hull, none = no body. Whole-map pieces are split
// into tiles so only the ground near the player costs anything.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "ragdoll.h"
#include "render/renderer.h"
#include "vecmath.h"

namespace tbd {

class Scene;

struct RayHit {
    Vec3 point = {0, 0, 0}, normal = {0, 1, 0};
    float distance = 0.0f;
};

struct CharacterStep {
    Vec3 velocity = {0, 0, 0}; // wanted; y is ignored
    bool jump = false;         // takes effect only on the ground
    bool swim = false;         // true: hold the feet at swimFeetY instead of falling
    float swimFeetY = 0.0f;
};

struct CharacterState {
    Vec3 feet = {0, 0, 0}, velocity = {0, 0, 0};
    bool onGround = false, steep = false;
};

struct PhysicsStats {
    size_t items = 0, bodies = 0, building = 0;
    double updateMs = 0.0;    // last Update: streaming scan, adding bodies, dynamic bodies
    double characterMs = 0.0; // last character step
};

class PhysicsWorld {
public:
    using LoadFn = bool (*)(const std::string& path, std::vector<Vertex>& out); // unindexed triangles, safe on a worker

    PhysicsWorld();
    ~PhysicsWorld();

    // Starts over with this scene's static collision. Main thread; the work happens on background jobs.
    void SetScene(JobSystem* jobs, const Scene& scene, const std::wstring& assetsRoot, LoadFn load);
    // Once per frame: streams collision around `focus`, and steps dynamic bodies when there are any.
    void Update(Vec3 focus, float dt);
    // True when everything within `radius` of `at` that should collide has its body, so a character there has ground.
    bool ReadyAround(Vec3 at, float radius) const;

    // At a frame boundary, translate Jolt coordinates by -offset. Streaming placements
    // stay in scene coordinates, including placements still being built on workers.
    void ShiftOrigin(Vec3 offset);

    // For gameplay. `dir` need not be unit length. The character is not hit.
    bool Raycast(Vec3 origin, Vec3 dir, float maxDistance, RayHit& hit) const;
    // A ball swept from `origin`; false when nothing is in the way, else `distance` is how far the ball's centre got.
    bool SphereCast(Vec3 origin, Vec3 dir, float maxDistance, float radius, float& distance) const;

    // The capsule (0.3 m radius, 1.8 m tall, origin at the feet). One character for now.
    void CreateCharacter(Vec3 feet);
    void DestroyCharacter();
    void TeleportCharacter(Vec3 feet);
    CharacterState StepCharacter(const CharacterStep& step, float dt);
    CharacterState Character() const;

    // A dynamic ball, which exercises the solver on the engine's job threads. Returns an id for BallPosition.
    uint32_t AddBall(Vec3 centre, float radius);
    Vec3 BallPosition(uint32_t id) const;

    // Navigation support (src/ai). Main thread only, like every other call here.
    // Static collision triangles overlapping the box, in world space, appended as nine floats per triangle (the winding is the
    // file's own). Dynamic bodies and the character are not included.
    void CollectStaticTriangles(Vec3 lo, Vec3 hi, std::vector<float>& out) const;
    // A kinematic capsule (feet origin) that the player and raycasts see. 0 = none made. Moved by MoveKinematic, never simulated.
    uint32_t CreateKinematicCapsule(Vec3 feet, float radius, float height);
    void MoveKinematic(uint32_t id, Vec3 feet);
    void DestroyKinematic(uint32_t id);
    size_t KinematicCount() const;

    // Human ragdolls (ragdoll.h). Created with the first call, after SetScene. Drive each one before Update.
    RagdollWorld& Ragdolls();
    bool HasRagdolls() const;

    PhysicsStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tbd
