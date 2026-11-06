// Human ragdoll: 17 rigid bodies joined by anatomically limited joints, with muscle motors that chase an animation pose.
// Jolt stays behind ragdoll.cpp; everything here is plain data. PhysicsWorld owns one RagdollWorld (physics.h).
//
// What it models (see docs/ragdoll_spec.md):
//  * Winter/Dempster segment masses, capsule/box shapes, group filter so neighbours never fight but limbs, arms and torso collide.
//  * Knees and elbows are hinges with a hyper-extension stop; hips, shoulders, spine, neck, ankles and wrists are swing-twist
//    cones with a biased centre (a hip bends 120 deg forward but only 20 deg back).
//  * Muscles: a spring-damper motor per joint toward the animation's local rotation, with a torque ceiling taken from human
//    strength tables. `tension` 0..1 scales them all: 1 = bracing, 0 = rag doll.
//  * Reflexes: protective brace when falling (arms guard the head, knees tuck, chin down) and hands that reach for the
//    landing spot.
//  * Injury: a joint hit past its tolerance dislocates (its range moves into the hyper-extended zone and stays there), a long
//    bone hit past its tolerance fractures (the limb goes slack), a hard head blow knocks the character out.
//  * Events (impacts, dislocations, fractures, knockouts) are queued for sound and gameplay.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vecmath.h"

namespace tbd {

// The skin's joints in model space, 16 floats per joint, column-major with the translation in 12..14 (SkeletalAnimator's
// layout). `bind` is the rest pose the joint limits are measured from; the pose at creation can be anything.
struct RagdollSkeleton {
    std::vector<std::string> names;
    std::vector<int> parent; // -1 for roots
    std::vector<float> bind;
};

enum class RagdollPart : int {
    Pelvis, Abdomen, Chest, Neck, Head,
    UpperArmL, LowerArmL, HandL, UpperArmR, LowerArmR, HandR,
    ThighL, CalfL, FootL, ThighR, CalfR, FootR,
    Count
};
constexpr int kRagdollParts = (int)RagdollPart::Count;
const char* RagdollPartName(int part);

struct RagdollTuning {
    float massKg = 80.0f;
    float tension = 1.0f;        // 0..1 muscle strength
    float toughness = 1.0f;      // scales every injury threshold
    bool reflexes = true;        // brace and reach while falling
    bool injuries = true;        // dislocation, fracture, knockout
    float braceSpeed = 2.2f;     // downward m/s where the brace starts to build
    float reactionTime = 0.18f;  // seconds in the air before the reflex answers
};

enum class RagdollEventKind { Impact, Dislocation, Fracture, Knockout };
struct RagdollEvent {
    RagdollEventKind kind = RagdollEventKind::Impact;
    int part = 0;
    int other = -1;       // Impact: 1 = the other body is another part of this character
    float speed = 0.0f;   // m/s along the contact normal
    float impulse = 0.0f; // N s, estimated
    Vec3 point = {0, 0, 0};
    uint32_t id = 0;
};

struct RagdollStatus {
    bool alive = false;
    Vec3 pelvis = {0, 0, 0}, head = {0, 0, 0}, velocity = {0, 0, 0};
    float speed = 0.0f;
    float brace = 0.0f;       // 0..1 how far the protective pose is blended in
    float tension = 0.0f;     // effective, after knockout
    float restTime = 0.0f;    // seconds spent (nearly) still
    bool airborne = true;
    bool unconscious = false;
    uint32_t dislocated = 0;  // bit per RagdollPart
    uint32_t fractured = 0;
    float jointLoad[kRagdollParts] = {}; // N m through each joint's stops, strongest since the last Status call
    float peakImpulse[kRagdollParts] = {}; // N s, strongest hit on each part since the last Status call
};

class RagdollWorld {
public:
    // `system` is a JPH::PhysicsSystem*; `moving` is the object layer for dynamic bodies. Opaque so no Jolt types leak.
    RagdollWorld(void* system, unsigned moving);
    ~RagdollWorld();

    // False when the skeleton lacks the joints a human needs. `pose` is the start pose (joint worlds, model space) and
    // `modelToWorld` places the model in physics space. The start velocity is added to every body.
    uint32_t Create(const RagdollSkeleton& skeleton, const float* pose, const float* modelToWorld, Vec3 velocity, const RagdollTuning& tuning);
    void Destroy(uint32_t id);
    void Clear();
    bool Exists(uint32_t id) const;
    size_t Count() const;

    // Each frame before the physics step: the animation's pose (what the muscles aim for) and elapsed time.
    void Drive(uint32_t id, const float* animatedPose, float dt);
    // After the physics step.
    void PostStep(float dt);
    // How many collision steps to give Jolt for a frame of `dt` (120 Hz or better, at most 4).
    static int Substeps(float dt);
    // Joint worlds for the skin, in the model space of `modelToWorld`.
    bool Pose(uint32_t id, const float* modelToWorld, std::vector<float>& out) const;

    RagdollStatus Status(uint32_t id) const;
    RagdollTuning* Tuning(uint32_t id);
    // Explosion: every part within `radius` gets a velocity change of up to `speed` m/s away from `centre`, limbs a little more,
    // each with its own spin so arms and legs do not move as one.
    void Blast(uint32_t id, Vec3 centre, float speed, float radius);
    // A blow at a point (bullet, punch, fall): the nearest part takes `impulse` N s.
    void Hit(uint32_t id, Vec3 point, Vec3 impulse);
    void Knockout(uint32_t id, bool out);
    void SetTension(uint32_t id, float tension);
    // Forces an injury, for tests and scripted scenes.
    void Dislocate(uint32_t id, int part);
    void Fracture(uint32_t id, int part);

    std::vector<RagdollEvent> TakeEvents();
    // The floating origin moved: translate every body by -offset.
    void Shift(Vec3 offset);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tbd
