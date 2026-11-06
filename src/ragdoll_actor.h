// A ragdoll on a scene character: reads the clips' pose from the animator as the muscles' target, steps the physics ragdoll, and puts
// its pose back on the skin. The entities that share the skeleton (body, head, hat) are moved with the pelvis so culling and
// shadows stay right. Part of main's includes through player_character.h.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "physics.h"
#include "ragdoll.h"
#include "scene.h"
#include "skeletal_anim.h"
#include "vecmath.h"

namespace tbd {

class RagdollActor {
public:
    bool Active() const { return id_ != 0; }
    uint32_t Id() const { return id_; }
    RagdollStatus Status() const { return id_ ? physics_->Ragdolls().Status(id_) : RagdollStatus{}; }

    // `bodies`: the entities (scene index, deformable mesh) that wear the skeleton. The first one defines the rig.
    bool Begin(Scene& scene, const std::vector<std::pair<int, MeshId>>& bodies, PhysicsWorld& physics, SkeletalAnimator& animator, Vec3 velocity,
               const RagdollTuning& tuning) {
        End();
        if (bodies.empty()) return false;
        RagdollSkeleton skeleton;
        if (!animator.Skeleton(bodies[0].second, skeleton)) { Log("Ragdoll: the mesh has no skin"); return false; }
        const std::vector<float>* pose = animator.AnimatedPose(bodies[0].second);
        if (!pose || pose->size() != skeleton.bind.size()) { Log("Ragdoll: no animated pose yet"); return false; }
        const Entity& e = scene.entities[(size_t)bodies[0].first];
        if (std::fabs(e.scale.x - 1.0f) + std::fabs(e.scale.y - 1.0f) + std::fabs(e.scale.z - 1.0f) > 0.02f)
            Log("Ragdoll: entity %s has scale %.2f %.2f %.2f; the ragdoll assumes 1", e.name.c_str(), e.scale.x, e.scale.y, e.scale.z);
        rotation_ = e.rotation;
        position_ = e.position;
        Matrix(m2w_);
        physics_ = &physics;
        animator_ = &animator;
        bodies_ = bodies;
        for (size_t j = 0; j < skeleton.names.size(); ++j)
            if (skeleton.names[j] == "pelvis") pelvisJoint_ = (int)j;
        if (pelvisJoint_ < 0) { Log("Ragdoll: the skeleton has no pelvis joint"); return false; }
        pelvisModel_ = {(*pose)[(size_t)pelvisJoint_ * 16 + 12], (*pose)[(size_t)pelvisJoint_ * 16 + 13], (*pose)[(size_t)pelvisJoint_ * 16 + 14]};
        jointCount_ = skeleton.names.size();
        id_ = physics.Ragdolls().Create(skeleton, pose->data(), m2w_, velocity, tuning);
        if (!id_) { Log("Ragdoll: could not be created (the skeleton needs pelvis, spine_01/03, neck_01, head, arms, legs)"); physics_ = nullptr; return false; }
        Log("Ragdoll: created, %zu joints, %zu skins", jointCount_, bodies.size());
        return true;
    }

    void End() {
        if (!id_) return;
        for (const auto& [entity, mesh] : bodies_) animator_->ClearPoseOverride(mesh);
        physics_->Ragdolls().Destroy(id_);
        id_ = 0;
        bodies_.clear();
    }

    // Before PhysicsWorld::Update: the muscles aim at the clips' pose.
    void Drive(float dt) {
        if (!id_) return;
        const std::vector<float>* pose = animator_->AnimatedPose(bodies_[0].second);
        if (pose && pose->size() == jointCount_ * 16) physics_->Ragdolls().Drive(id_, pose->data(), dt);
    }

    // After the physics step: the skin takes the ragdoll's pose, and the entities follow the pelvis.
    void Apply(Scene& scene) {
        if (!id_) return;
        std::vector<float> pose;
        if (!physics_->Ragdolls().Pose(id_, m2w_, pose)) return;
        // Where the pelvis is in the model now versus where it sits in the rest pose: slide the model so the body stays at the origin.
        const Vec3 now = {pose[(size_t)pelvisJoint_ * 16 + 12], pose[(size_t)pelvisJoint_ * 16 + 13], pose[(size_t)pelvisJoint_ * 16 + 14]};
        const Vec3 delta = now - pelvisModel_;
        for (size_t j = 0; j < jointCount_; ++j) {
            pose[j * 16 + 12] -= delta.x; pose[j * 16 + 13] -= delta.y; pose[j * 16 + 14] -= delta.z;
        }
        const Vec3 shift = Rotate(rotation_, delta);
        position_ = position_ + shift;
        m2w_[12] = position_.x; m2w_[13] = position_.y; m2w_[14] = position_.z;
        for (const auto& [entity, mesh] : bodies_) {
            scene.entities[(size_t)entity].position = position_;
            scene.entities[(size_t)entity].rotation = rotation_;
            animator_->SetPoseOverride(mesh, pose);
        }
    }

private:
    void Matrix(float* m) const {
        const Vec3 x = Rotate(rotation_, {1, 0, 0}), y = Rotate(rotation_, {0, 1, 0}), z = Rotate(rotation_, {0, 0, 1});
        m[0] = x.x; m[1] = x.y; m[2] = x.z; m[3] = 0;
        m[4] = y.x; m[5] = y.y; m[6] = y.z; m[7] = 0;
        m[8] = z.x; m[9] = z.y; m[10] = z.z; m[11] = 0;
        m[12] = position_.x; m[13] = position_.y; m[14] = position_.z; m[15] = 1;
    }

    PhysicsWorld* physics_ = nullptr;
    SkeletalAnimator* animator_ = nullptr;
    std::vector<std::pair<int, MeshId>> bodies_;
    uint32_t id_ = 0;
    int pelvisJoint_ = -1;
    size_t jointCount_ = 0;
    Vec3 pelvisModel_ = {0, 0, 0}, position_ = {0, 0, 0};
    Quat rotation_;
    float m2w_[16] = {};
};

} // namespace tbd
