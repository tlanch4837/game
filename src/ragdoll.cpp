// See ragdoll.h. Jolt side of the human ragdoll. Bodies sit at the joint frames of the skeleton, so a joint's body rotation is the
// skin joint's rotation; that makes the animation's local rotations directly usable as motor targets.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "ragdoll.h"

namespace tbd {
namespace {

using JPH::Mat44;
using JQuat = JPH::Quat;
using JPH::RVec3;
using JVec3 = JPH::Vec3;
using JVec4 = JPH::Vec4;

constexpr float kPi = 3.14159265f;

enum Dir { None, Fwd, Back, Up, Down };
enum Kind { Root, Hinge, Ball };
enum Shape { Cap, Box, Ball3 };

// One body of the human. Lengths come from the skeleton; offsets and half sizes are in metres for a 1.8 m person along the
// anatomical axes (L left, U up, F forward), and scaled with the skeleton's size.
struct Def {
    const char* name;
    const char* joint;
    const char* end;     // the joint the bone runs toward; null = unused
    int parent;
    Shape shape;
    float radius;        // Cap, Ball3
    float half[3];       // Box half sizes along L, U, F (U replaced by the bone half-length when < 0)
    float offset[3];     // shape centre from the joint along L, U, F, (Cap: unused, centre is mid-bone)
    float mass;          // fraction of body mass
    Kind kind;
    Dir flex;            // the direction the joint bends toward (positive hinge angle, wide side of a cone)
    float flexMax, hyper;            // Hinge: range is [-hyper, flexMax]
    float cone[2];                   // Ball: half angles [lateral, sagittal]
    float bias;                      // Ball: cone centre tilt toward `flex`
    float twist[2];                  // Ball: min, max
    float freq, torque;              // muscle spring (Hz) and strongest torque (N m)
    float dislocate;                 // N m through the stop that pops the joint
    float fracture;                  // N s on this bone
    float effMass;                   // how much of the body stands behind this part in an impact
    float brace[2];                  // protective pose: bend toward `flex` and toward the midline (rad)
    float friction;                  // N m of joint friction when the muscles are off
};

constexpr Def kDefs[kRagdollParts] = {
    // name, joint, end, parent, shape, radius, half, offset, mass, kind, flex, flexMax,hyper, cone, bias, twist, freq, torque, dislocate, fracture, effMass, brace, friction
    {"pelvis", "pelvis", "spine_01", -1, Box, 0, {0.150f, 0.095f, 0.105f}, {0, 0.025f, 0}, 0.142f, Root, None, 0, 0, {0, 0}, 0, {0, 0}, 0, 0, 0, 0, 1.2f, {0, 0}, 0},
    {"abdomen", "spine_01", "spine_03", 0, Box, 0, {0.125f, -1, 0.095f}, {0, 0.065f, 0}, 0.139f, Ball, Fwd, 0, 0, {0.42f, 0.50f}, 0.12f, {-0.30f, 0.30f}, 4.0f, 200, 450, 0, 1.2f, {0.30f, 0}, 9},
    {"chest", "spine_03", "neck_01", 1, Box, 0, {0.165f, 0.165f, 0.115f}, {0, 0.150f, 0.005f}, 0.216f, Ball, Fwd, 0, 0, {0.36f, 0.42f}, 0.10f, {-0.35f, 0.35f}, 4.0f, 220, 450, 0, 1.2f, {0.30f, 0}, 9},
    {"neck", "neck_01", "head", 2, Cap, 0.050f, {0, 0, 0}, {0, 0, 0}, 0.014f, Ball, Fwd, 0, 0, {0.60f, 0.75f}, 0.25f, {-0.80f, 0.80f}, 4.5f, 55, 450, 0, 1.4f, {0.55f, 0}, 3},
    {"head", "head", nullptr, 3, Ball3, 0.097f, {0, 0, 0}, {0, 0.108f, 0.020f}, 0.067f, Ball, Fwd, 0, 0, {0.45f, 0.50f}, 0.12f, {-0.70f, 0.70f}, 4.5f, 45, 450, 60, 1.8f, {0.25f, 0}, 3},
    // arms: left then right (right mirrored from the skeleton)
    {"upperarm_l", "upperarm_l", "lowerarm_l", 2, Cap, 0.047f, {0, 0, 0}, {0, 0, 0}, 0.028f, Ball, Fwd, 0, 0, {1.45f, 2.10f}, 1.05f, {-1.20f, 1.20f}, 3.0f, 110, 160, 55, 1.8f, {1.55f, 0.55f}, 3},
    {"lowerarm_l", "lowerarm_l", "hand_l", 5, Cap, 0.037f, {0, 0, 0}, {0, 0, 0}, 0.016f, Hinge, Fwd, 2.50f, 0.06f, {0, 0}, 0, {0, 0}, 3.0f, 75, 90, 38, 2.4f, {2.10f, 0}, 1.5f},
    {"hand_l", "hand_l", "middle_01_l", 6, Cap, 0.030f, {0, 0, 0}, {0, 0, 0}, 0.006f, Ball, Fwd, 0, 0, {0.45f, 0.85f}, 0.05f, {-1.00f, 1.00f}, 2.5f, 22, 25, 24, 3.5f, {0, 0}, 0.5f},
    {"upperarm_r", "upperarm_r", "lowerarm_r", 2, Cap, 0.047f, {0, 0, 0}, {0, 0, 0}, 0.028f, Ball, Fwd, 0, 0, {1.45f, 2.10f}, 1.05f, {-1.20f, 1.20f}, 3.0f, 110, 160, 55, 1.8f, {1.55f, 0.55f}, 3},
    {"lowerarm_r", "lowerarm_r", "hand_r", 8, Cap, 0.037f, {0, 0, 0}, {0, 0, 0}, 0.016f, Hinge, Fwd, 2.50f, 0.06f, {0, 0}, 0, {0, 0}, 3.0f, 75, 90, 38, 2.4f, {2.10f, 0}, 1.5f},
    {"hand_r", "hand_r", "middle_01_r", 9, Cap, 0.030f, {0, 0, 0}, {0, 0, 0}, 0.006f, Ball, Fwd, 0, 0, {0.45f, 0.85f}, 0.05f, {-1.00f, 1.00f}, 2.5f, 22, 25, 24, 3.5f, {0, 0}, 0.5f},
    // legs
    {"thigh_l", "thigh_l", "calf_l", 0, Cap, 0.080f, {0, 0, 0}, {0, 0, 0}, 0.100f, Ball, Fwd, 0, 0, {0.75f, 1.175f}, 0.825f, {-0.65f, 0.65f}, 3.5f, 260, 650, 130, 1.6f, {1.30f, 0}, 6},
    {"calf_l", "calf_l", "foot_l", 11, Cap, 0.057f, {0, 0, 0}, {0, 0, 0}, 0.0465f, Hinge, Back, 2.45f, 0.04f, {0, 0}, 0, {0, 0}, 3.5f, 220, 260, 75, 2.2f, {1.90f, 0}, 3},
    {"foot_l", "foot_l", "ball_l", 12, Box, 0, {0.045f, 0.040f, 0.125f}, {0, -0.040f, 0.065f}, 0.0145f, Ball, Down, 0, 0, {0.35f, 0.575f}, 0.225f, {-0.40f, 0.40f}, 3.0f, 100, 700, 38, 3.0f, {0, 0}, 2},
    {"thigh_r", "thigh_r", "calf_r", 0, Cap, 0.080f, {0, 0, 0}, {0, 0, 0}, 0.100f, Ball, Fwd, 0, 0, {0.75f, 1.175f}, 0.825f, {-0.65f, 0.65f}, 3.5f, 260, 650, 130, 1.6f, {1.30f, 0}, 6},
    {"calf_r", "calf_r", "foot_r", 14, Cap, 0.057f, {0, 0, 0}, {0, 0, 0}, 0.0465f, Hinge, Back, 2.45f, 0.04f, {0, 0}, 0, {0, 0}, 3.5f, 220, 260, 75, 2.2f, {1.90f, 0}, 3},
    {"foot_r", "foot_r", "ball_r", 15, Box, 0, {0.045f, 0.040f, 0.125f}, {0, -0.040f, 0.065f}, 0.0145f, Ball, Down, 0, 0, {0.35f, 0.575f}, 0.225f, {-0.40f, 0.40f}, 3.0f, 100, 700, 38, 3.0f, {0, 0}, 2},
};

// Neighbours that must not collide (their shapes overlap at the joint).
constexpr int kNoCollide[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {2, 4}, {2, 5}, {2, 8}, {5, 6}, {6, 7}, {8, 9}, {9, 10},
                                 {0, 11}, {0, 14}, {11, 12}, {12, 13}, {14, 15}, {15, 16}};

inline JVec3 V(const float* a) { return JVec3(a[0], a[1], a[2]); }
inline JVec3 ToJ(tbd::Vec3 v) { return JVec3(v.x, v.y, v.z); }
inline tbd::Vec3 FromJ(JVec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }

Mat44 MatFrom(const float* a) { return Mat44(JVec4(a[0], a[1], a[2], a[3]), JVec4(a[4], a[5], a[6], a[7]), JVec4(a[8], a[9], a[10], a[11]), JVec4(a[12], a[13], a[14], a[15])); }
void MatTo(const Mat44& m, float* a) {
    for (int c = 0; c < 4; ++c) {
        const JVec4 col = m.GetColumn4((JPH::uint)c);
        a[c * 4] = col.GetX(); a[c * 4 + 1] = col.GetY(); a[c * 4 + 2] = col.GetZ(); a[c * 4 + 3] = col.GetW();
    }
}
// A rigid copy (unit axes, no shear), so quaternions and inverses are safe on animated matrices.
Mat44 Rigid(const Mat44& m) {
    const JVec3 x = m.GetAxisX().Normalized();
    JVec3 y = m.GetAxisY();
    y = (y - x * x.Dot(y)).Normalized();
    return Mat44(JVec4(x, 0), JVec4(y, 0), JVec4(x.Cross(y), 0), JVec4(m.GetTranslation(), 1));
}
inline JQuat RotOf(const Mat44& m) { return Rigid(m).GetQuaternion().Normalized(); }

// Rotates `v` toward `target` about the axis they span; the shortest arc.
JQuat FromTo(JVec3 a, JVec3 b) {
    a = a.NormalizedOr(JVec3::sAxisY());
    b = b.NormalizedOr(JVec3::sAxisY());
    const float d = a.Dot(b);
    if (d < -0.9999f) {
        JVec3 axis = a.Cross(JVec3::sAxisX());
        if (axis.LengthSq() < 1e-4f) axis = a.Cross(JVec3::sAxisZ());
        return JQuat::sRotation(axis.Normalized(), kPi);
    }
    const JVec3 c = a.Cross(b);
    return JQuat(c.GetX(), c.GetY(), c.GetZ(), 1.0f + d).Normalized();
}
JQuat Nlerp(JQuat a, JQuat b, float t) {
    if (a.Dot(b) < 0.0f) b = -b;
    return (a * (1.0f - t) + b * t).Normalized();
}
float SmoothStep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float HashFloat(uint32_t a, uint32_t b) { // -1..1, repeatable
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h & 0xFFFFFF) / (float)0x7FFFFF - 1.0f;
}

int FindJoint(const RagdollSkeleton& s, const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < s.names.size(); ++i)
        if (s.names[i] == name) return (int)i;
    return -1;
}

constexpr int kMaxJoints = 256;

} // namespace

const char* RagdollPartName(int part) { return part >= 0 && part < kRagdollParts ? kDefs[part].name : "?"; }

struct RagdollWorld::Impl : JPH::ContactListener {
    struct Part {
        JPH::BodyID body;
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JQuat restRel;           // parent joint frame -> this joint frame, bind pose
        JVec3 hingeAxisLocal;    // parent joint frame
        JVec3 hingeNormalLocal;
        JVec3 boneDirLocal;      // this joint frame, toward the end joint
        JQuat braceDelta;        // parent-frame rotation applied to the bind-relative pose for the protective pose
        float braceHinge = 0;
        float mass = 1, hingeRef = 0, strength = 1; // strength: injury multiplier
        float coolDown = 0, load = 0, peakImpulse = 0;
        bool dislocated = false, fractured = false;
        float bindExtra[2] = {0, 0};
        Mat44 bind;             // joint frame in model space
        bool hasJoint = false;
        int jointIndex = -1;
    };
    struct Instance {
        uint32_t id = 0;
        RagdollTuning tuning;
        Part part[kRagdollParts];
        std::vector<int> owner;       // skin joint -> part
        std::vector<Mat44> followRel; // skin joint in its owner's joint frame
        std::vector<int> blendWith;   // spine_02: also follows this part, halfway
        std::vector<int> parentJoint;
        int jointCount = 0;
        JPH::Ref<JPH::GroupFilterTable> filter;
        uint32_t group = 0;
        JVec3 fwd, up, left;           // anatomical axes, model space
        float brace = 0, braceTarget = 0, airTime = 0, restTime = 0, wake = 1, wakeTarget = 1;
        float heightAbove = 10.0f;
        bool airborne = true;
        bool unconscious = false;
        uint32_t dislocatedMask = 0, fracturedMask = 0;
        JVec3 landing = JVec3::sZero();
        float landingReach = 0;
        uint32_t seed = 0;
        std::vector<float> lastPose;
    };
    struct Pending { uint32_t id; int part; float speed, impulse; JVec3 point; bool self; };

    JPH::PhysicsSystem* sys = nullptr;
    JPH::ObjectLayer moving = 1;
    std::unordered_map<uint32_t, std::unique_ptr<Instance>> items;
    std::unordered_map<uint32_t, std::pair<uint32_t, int>> bodyMap; // body index+sequence -> (instance, part)
    uint32_t next = 1, nextGroup = 4000;
    std::mutex mutex;
    std::vector<RagdollEvent> events;
    std::vector<Pending> pending;
    float lastSub = 1.0f / 120.0f;

    JPH::BodyInterface& Bodies() { return sys->GetBodyInterfaceNoLock(); }

    // -------------------------------------------------------------- contacts (worker threads)
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        const auto fa = bodyMap.find(a.GetID().GetIndexAndSequenceNumber());
        const auto fb = bodyMap.find(b.GetID().GetIndexAndSequenceNumber());
        if (fa == bodyMap.end() && fb == bodyMap.end()) return;
        const JVec3 p = JVec3(m.GetWorldSpaceContactPointOn1(0));
        const JVec3 n = m.mWorldSpaceNormal;
        const float closing = (a.GetPointVelocity(RVec3(p)) - b.GetPointVelocity(RVec3(p))).Dot(n);
        const float speed = std::fabs(closing);
        if (speed < 0.8f) return;
        const float ia = a.IsDynamic() ? a.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f;
        const float ib = b.IsDynamic() ? b.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f;
        if (ia + ib <= 0.0f) return;
        const float reduced = 1.0f / (ia + ib);
        const bool self = fa != bodyMap.end() && fb != bodyMap.end() && fa->second.first == fb->second.first;
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto* f : {&fa, &fb}) {
            if (*f == bodyMap.end()) continue;
            const int part = (*f)->second.second;
            pending.push_back({(*f)->second.first, part, speed, reduced * speed * kDefs[part].effMass, p, self});
        }
    }

    // -------------------------------------------------------------- creation
    uint32_t Create(const RagdollSkeleton& sk, const float* pose, const float* modelToWorld, tbd::Vec3 velocity, const RagdollTuning& tuning) {
        const size_t n = sk.names.size();
        if (n == 0 || n > (size_t)kMaxJoints || sk.bind.size() < n * 16 || sk.parent.size() < n) return 0;
        auto inst = std::make_unique<Instance>();
        Instance& r = *inst;
        r.tuning = tuning;
        r.jointCount = (int)n;
        r.parentJoint = sk.parent;
        std::vector<Mat44> bind(n);
        for (size_t i = 0; i < n; ++i) bind[i] = Rigid(MatFrom(&sk.bind[i * 16]));

        // The joints a human needs.
        int jointOf[kRagdollParts], endOf[kRagdollParts];
        for (int p = 0; p < kRagdollParts; ++p) {
            jointOf[p] = FindJoint(sk, kDefs[p].joint);
            endOf[p] = FindJoint(sk, kDefs[p].end);
            if (jointOf[p] < 0) return 0;
        }
        const Mat44 m2w = Rigid(MatFrom(modelToWorld));
        const Mat44 w2m = m2w.InversedRotationTranslation();

        // Anatomical axes, from the bind pose. Left is toward thigh_l.
        {
            JVec3 left = (bind[(size_t)jointOf[(int)RagdollPart::ThighL]].GetTranslation() - bind[(size_t)jointOf[(int)RagdollPart::ThighR]].GetTranslation()).NormalizedOr(JVec3::sAxisX());
            JVec3 up = bind[(size_t)jointOf[(int)RagdollPart::Head]].GetTranslation() - bind[(size_t)jointOf[(int)RagdollPart::Pelvis]].GetTranslation();
            up = (up - left * left.Dot(up)).NormalizedOr(JVec3::sAxisY());
            r.left = left; r.up = up; r.fwd = left.Cross(up);
        }
        // Scale from the leg: 0.94 m hip height is the reference person.
        const float legLength = (bind[(size_t)jointOf[(int)RagdollPart::ThighL]].GetTranslation() - bind[(size_t)jointOf[(int)RagdollPart::FootL]].GetTranslation()).Length();
        const float scale = std::clamp(legLength / 0.861f, 0.5f, 2.0f);

        r.group = nextGroup++;
        r.filter = new JPH::GroupFilterTable(kRagdollParts);
        for (const auto& pair : kNoCollide) r.filter->DisableCollision((JPH::CollisionGroup::SubGroupID)pair[0], (JPH::CollisionGroup::SubGroupID)pair[1]);

        // Bodies at the bind pose first: the constraints take their rest from it. Then the bodies go to the start pose.
        for (int p = 0; p < kRagdollParts; ++p) {
            const Def& d = kDefs[p];
            Part& part = r.part[p];
            part.jointIndex = jointOf[p];
            part.hasJoint = true;
            part.bind = bind[(size_t)jointOf[p]];
            part.mass = tuning.massKg * d.mass;
            const Mat44 jointBind = part.bind;
            const JQuat rot = RotOf(jointBind);
            const JVec3 origin = jointBind.GetTranslation();
            const Mat44 invJoint = jointBind.InversedRotationTranslation();

            // Bone direction: toward the end joint, else up.
            JVec3 boneDir = r.up;
            if (endOf[p] >= 0) boneDir = (bind[(size_t)endOf[p]].GetTranslation() - origin).NormalizedOr(r.up);
            part.boneDirLocal = invJoint.Multiply3x3(boneDir);
            const float boneLen = endOf[p] >= 0 ? (bind[(size_t)endOf[p]].GetTranslation() - origin).Length() : 0.2f * scale;

            JPH::RefConst<JPH::Shape> shape;
            const JVec3 anat[3] = {r.left, r.up, r.fwd};
            const JVec3 offsetW = anat[0] * d.offset[0] * scale + anat[1] * d.offset[1] * scale + anat[2] * d.offset[2] * scale;
            if (d.shape == Cap) {
                float len = boneLen;
                if (p == (int)RagdollPart::HandL || p == (int)RagdollPart::HandR) len = boneLen * 1.75f;
                const float radius = d.radius * scale;
                const float halfCyl = std::max(len * 0.5f - radius * 0.65f, 0.01f);
                const JVec3 centreLocal = invJoint.Multiply3x3(boneDir) * (len * 0.5f);
                const JQuat toBone = FromTo(JVec3::sAxisY(), invJoint.Multiply3x3(boneDir));
                shape = new JPH::RotatedTranslatedShape(centreLocal, toBone, new JPH::CapsuleShape(halfCyl, radius));
            } else if (d.shape == Ball3) {
                shape = new JPH::RotatedTranslatedShape(invJoint.Multiply3x3(offsetW), JQuat::sIdentity(), new JPH::SphereShape(d.radius * scale));
            } else {
                JVec3 half(d.half[0] * scale, d.half[1] * scale, d.half[2] * scale);
                JVec3 centre = offsetW;
                if (d.half[1] < 0.0f) { // length of the bone
                    half.SetY(boneLen * 0.5f + 0.012f * scale);
                    centre = centre + boneDir * (boneLen * 0.5f);
                }
                // Box axes: x left, y up, z forward, expressed in the joint frame.
                const Mat44 axes(JVec4(r.left, 0), JVec4(r.up, 0), JVec4(r.fwd, 0), JVec4(0, 0, 0, 1));
                const JQuat local = (invJoint.GetRotation() * axes).GetQuaternion().Normalized();
                shape = new JPH::RotatedTranslatedShape(invJoint.Multiply3x3(centre), local, new JPH::BoxShape(half, std::min(0.02f * scale, std::min(half.GetX(), std::min(half.GetY(), half.GetZ())) * 0.9f)));
            }

            const Mat44 worldBind = m2w * jointBind;
            JPH::BodyCreationSettings bs(shape, RVec3(worldBind.GetTranslation()), worldBind.GetQuaternion().Normalized(), JPH::EMotionType::Dynamic, moving);
            bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bs.mMassPropertiesOverride.mMass = part.mass;
            bs.mFriction = (p == (int)RagdollPart::FootL || p == (int)RagdollPart::FootR) ? 1.0f : 0.75f;
            bs.mRestitution = 0.0f;
            bs.mLinearDamping = 0.02f;
            bs.mAngularDamping = 0.12f;
            bs.mMaxAngularVelocity = 35.0f;
            bs.mMotionQuality = JPH::EMotionQuality::LinearCast;
            bs.mNumVelocityStepsOverride = 12;
            bs.mNumPositionStepsOverride = 4;
            bs.mCollisionGroup = JPH::CollisionGroup(r.filter, r.group, (JPH::CollisionGroup::SubGroupID)p);
            part.body = Bodies().CreateAndAddBody(bs, JPH::EActivation::DontActivate);
            if (part.body.IsInvalid()) { Remove(r); return 0; }
        }

        // Joints, in world space at the bind pose.
        for (int p = 1; p < kRagdollParts; ++p) {
            const Def& d = kDefs[p];
            Part& part = r.part[p];
            const Part& par = r.part[d.parent];
            const Mat44 parentBind = par.bind;
            part.restRel = RotOf(parentBind).Conjugated() * RotOf(part.bind);
            const JVec3 pivot = (m2w * part.bind).GetTranslation();
            const JVec3 boneW = m2w.Multiply3x3(part.bind.Multiply3x3(part.boneDirLocal)).Normalized();
            JVec3 flexDir = d.flex == Fwd ? r.fwd : d.flex == Back ? -r.fwd : d.flex == Up ? r.up : d.flex == Down ? -r.up : r.fwd;
            flexDir = m2w.Multiply3x3(flexDir);
            JVec3 flexPerp = flexDir - boneW * boneW.Dot(flexDir);
            if (flexPerp.LengthSq() < 1e-4f) flexPerp = boneW.Cross(m2w.Multiply3x3(r.left));
            flexPerp = flexPerp.Normalized();
            const JPH::BodyID ids[2] = {par.body, part.body};
            JPH::BodyLockMultiWrite lock(sys->GetBodyLockInterfaceNoLock(), ids, 2);
            JPH::Body* b1 = lock.GetBody(0);
            JPH::Body* b2 = lock.GetBody(1);
            if (!b1 || !b2) { Remove(r); return 0; }
            if (d.kind == Hinge) {
                JPH::HingeConstraintSettings hs;
                hs.mSpace = JPH::EConstraintSpace::WorldSpace;
                hs.mPoint1 = hs.mPoint2 = RVec3(pivot);
                const JVec3 axis = boneW.Cross(flexPerp).Normalized(); // positive angle bends the bone toward flexPerp
                hs.mHingeAxis1 = hs.mHingeAxis2 = axis;
                hs.mNormalAxis1 = hs.mNormalAxis2 = flexPerp;
                hs.mLimitsMin = -d.hyper;
                hs.mLimitsMax = d.flexMax;
                hs.mMaxFrictionTorque = d.friction;
                hs.mMotorSettings = JPH::MotorSettings(d.freq, 1.0f);
                hs.mMotorSettings.SetTorqueLimit(d.torque);
                part.constraint = hs.Create(*b1, *b2);
                part.hingeAxisLocal = RotOf(parentBind).Conjugated() * V(&r.left.mF32[0]); // overwritten below
                const Mat44 pw = m2w * parentBind;
                part.hingeAxisLocal = pw.InversedRotationTranslation().Multiply3x3(axis);
                part.hingeNormalLocal = pw.InversedRotationTranslation().Multiply3x3(flexPerp);
            } else {
                JPH::SwingTwistConstraintSettings ss;
                ss.mSpace = JPH::EConstraintSpace::WorldSpace;
                ss.mPosition1 = ss.mPosition2 = RVec3(pivot);
                const float b = d.bias;
                ss.mTwistAxis2 = boneW;
                ss.mPlaneAxis2 = flexPerp;
                ss.mTwistAxis1 = (boneW * std::cos(b) + flexPerp * std::sin(b)).Normalized();
                ss.mPlaneAxis1 = (flexPerp * std::cos(b) - boneW * std::sin(b)).Normalized();
                ss.mSwingType = JPH::ESwingType::Cone;
                ss.mNormalHalfConeAngle = d.cone[0];
                ss.mPlaneHalfConeAngle = d.cone[1];
                ss.mTwistMinAngle = d.twist[0];
                ss.mTwistMaxAngle = d.twist[1];
                ss.mMaxFrictionTorque = d.friction;
                ss.mSwingMotorSettings = JPH::MotorSettings(d.freq, 1.0f);
                ss.mSwingMotorSettings.SetTorqueLimit(d.torque);
                ss.mTwistMotorSettings = ss.mSwingMotorSettings;
                part.constraint = ss.Create(*b1, *b2);
            }
            sys->AddConstraint(part.constraint);
        }

        // Brace pose deltas, in the parent's joint frame.
        for (int p = 1; p < kRagdollParts; ++p) {
            const Def& d = kDefs[p];
            Part& part = r.part[p];
            const JVec3 bone = part.restRel * part.boneDirLocal; // in the parent's frame
            const Part& par = r.part[d.parent];
            const Mat44 invPar = par.bind.InversedRotationTranslation();
            JVec3 flexDir = d.flex == Fwd ? r.fwd : d.flex == Back ? -r.fwd : d.flex == Up ? r.up : d.flex == Down ? -r.up : r.fwd;
            const JVec3 flexLocal = invPar.Multiply3x3(flexDir);
            // Toward the midline: from the left side, toward -left; from the right side toward +left.
            const bool leftSide = p >= (int)RagdollPart::UpperArmL && p <= (int)RagdollPart::HandL ? true : p >= (int)RagdollPart::ThighL && p <= (int)RagdollPart::FootL;
            const bool rightSide = (p >= (int)RagdollPart::UpperArmR && p <= (int)RagdollPart::HandR) || (p >= (int)RagdollPart::ThighR && p <= (int)RagdollPart::FootR);
            const JVec3 medial = invPar.Multiply3x3(r.left * (leftSide ? -1.0f : rightSide ? 1.0f : 0.0f));
            JQuat q = JQuat::sIdentity();
            if (d.kind == Hinge) part.braceHinge = d.brace[0];
            else {
                if (std::fabs(d.brace[0]) > 1e-4f) q = JQuat::sRotation(bone.Cross(flexLocal).NormalizedOr(JVec3::sAxisX()), d.brace[0]) * q;
                if (std::fabs(d.brace[1]) > 1e-4f && medial.LengthSq() > 0.1f) q = JQuat::sRotation(bone.Cross(medial).NormalizedOr(JVec3::sAxisZ()), d.brace[1]) * q;
            }
            part.braceDelta = q;
        }

        // Which part each skin joint rides on. Fingers, the ball of the foot and the clavicles follow their bone; the middle
        // spine joint is shared by the abdomen and the chest.
        r.owner.assign(n, (int)RagdollPart::Pelvis);
        r.blendWith.assign(n, -1);
        r.followRel.resize(n);
        for (size_t j = 0; j < n; ++j) {
            int walk = (int)j, owner = -1;
            while (walk >= 0 && owner < 0) {
                for (int p = 0; p < kRagdollParts; ++p)
                    if (jointOf[p] == walk) { owner = p; break; }
                walk = sk.parent[(size_t)walk];
            }
            if (owner < 0) owner = (int)RagdollPart::Pelvis;
            r.owner[j] = owner;
            if (sk.names[j] == "spine_02") { r.owner[j] = (int)RagdollPart::Abdomen; r.blendWith[j] = (int)RagdollPart::Chest; }
            // A clavicle moves with the chest, and the stretch of arm above the shoulder stays on the chest too.
            r.followRel[j] = r.part[r.owner[j]].bind.InversedRotationTranslation() * bind[j];
        }

        // Start pose.
        r.seed = next * 2654435761u;
        for (int p = 0; p < kRagdollParts; ++p) {
            const Part& part = r.part[p];
            const Mat44 w = m2w * Rigid(MatFrom(pose + (size_t)jointOf[p] * 16));
            Bodies().SetPositionAndRotation(part.body, RVec3(w.GetTranslation()), w.GetQuaternion().Normalized(), JPH::EActivation::Activate);
            Bodies().SetLinearAndAngularVelocity(part.body, ToJ(velocity), JVec3::sZero());
            bodyMap[part.body.GetIndexAndSequenceNumber()] = {next, p};
        }
        (void)w2m;
        r.id = next;
        items[next] = std::move(inst);
        return next++;
    }

    void Remove(Instance& r) {
        JPH::BodyInterface& bi = Bodies();
        for (int p = kRagdollParts - 1; p >= 0; --p) {
            Part& part = r.part[p];
            if (part.constraint) { sys->RemoveConstraint(part.constraint); part.constraint = nullptr; }
        }
        for (int p = 0; p < kRagdollParts; ++p) {
            Part& part = r.part[p];
            if (part.body.IsInvalid()) continue;
            bodyMap.erase(part.body.GetIndexAndSequenceNumber());
            if (bi.IsAdded(part.body)) bi.RemoveBody(part.body);
            bi.DestroyBody(part.body);
            part.body = JPH::BodyID();
        }
    }

    // -------------------------------------------------------------- the muscles
    // Signed rotation of `q` about `axis`.
    static float AngleAbout(JQuat q, JVec3 axis) {
        if (q.GetW() < 0.0f) q = -q;
        return 2.0f * std::atan2(q.GetXYZ().Dot(axis), q.GetW());
    }

    void SetMuscles(Instance& r, int p, float k) {
        const Def& d = kDefs[p];
        Part& part = r.part[p];
        if (!part.constraint) return;
        const float s = std::clamp(k * part.strength, 0.0f, 1.0f);
        const bool off = s < 0.03f;
        const float freq = d.freq * (0.35f + 0.65f * s);
        const float torque = d.torque * s * s * (part.mass > 0 ? 1.0f : 1.0f);
        if (d.kind == Hinge) {
            auto* h = static_cast<JPH::HingeConstraint*>(part.constraint.GetPtr());
            h->SetMotorState(off ? JPH::EMotorState::Off : JPH::EMotorState::Position);
            h->GetMotorSettings().mSpringSettings.mFrequency = freq;
            h->GetMotorSettings().mSpringSettings.mDamping = 1.0f;
            h->GetMotorSettings().SetTorqueLimit(std::max(torque, 0.01f));
        } else {
            auto* c = static_cast<JPH::SwingTwistConstraint*>(part.constraint.GetPtr());
            c->SetSwingMotorState(off ? JPH::EMotorState::Off : JPH::EMotorState::Position);
            c->SetTwistMotorState(off ? JPH::EMotorState::Off : JPH::EMotorState::Position);
            for (JPH::MotorSettings* m : {&c->GetSwingMotorSettings(), &c->GetTwistMotorSettings()}) {
                m->mSpringSettings.mFrequency = freq;
                m->mSpringSettings.mDamping = 1.0f;
                m->SetTorqueLimit(std::max(torque, 0.01f));
            }
        }
    }

    void Drive(Instance& r, const float* anim, float dt) {
        JPH::BodyInterface& bi = Bodies();
        const RagdollTuning& t = r.tuning;
        // Status from the bodies.
        const JPH::BodyID pelvisId = r.part[0].body;
        const JVec3 pelvisPos = JVec3(bi.GetCenterOfMassPosition(pelvisId));
        const JVec3 vel = bi.GetLinearVelocity(pelvisId);
        // Height above the ground under the pelvis and where a fall would end.
        {
            JPH::RRayCast ray(RVec3(pelvisPos), JVec3(0, -6.0f, 0));
            JPH::RayCastSettings rs;
            JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;
            sys->GetNarrowPhaseQuery().CastRay(ray, rs, hit, JPH::SpecifiedBroadPhaseLayerFilter(JPH::BroadPhaseLayer(0)), JPH::SpecifiedObjectLayerFilter(0));
            r.heightAbove = hit.HadHit() ? hit.mHit.mFraction * 6.0f : 10.0f;
        }
        // Lowest body above ground => airborne if even the lowest is high.
        r.airborne = r.heightAbove > 0.62f; // the pelvis stands about 0.1 m above the ground when lying, 0.9 m when upright
        r.airTime = r.airborne ? r.airTime + dt : 0.0f;

        // Wake: knockout fades the muscles over a third of a second, waking is quicker.
        r.wake += (r.wakeTarget - r.wake) * (1.0f - std::exp(-(r.wakeTarget < r.wake ? 9.0f : 4.0f) * dt));
        r.unconscious = r.wakeTarget < 0.5f;

        // Brace: builds when falling fast and not yet down, holds through the landing, fades when still.
        float target = 0.0f;
        if (t.reflexes && !r.unconscious && r.wake > 0.5f) {
            const float falling = SmoothStep(t.braceSpeed, t.braceSpeed + 3.0f, -vel.GetY());
            const float speed = vel.Length();
            const float high = SmoothStep(3.2f, 0.9f, r.heightAbove);
            const float tumbling = SmoothStep(4.0f, 8.0f, speed) * SmoothStep(2.5f, 0.6f, r.heightAbove);
            const bool ready = r.airTime > t.reactionTime;
            target = std::max(falling * high, tumbling) * (ready ? 1.0f : 0.0f);
            if (!r.airborne && speed > 2.5f) target = std::max(target, r.brace * 0.9f); // keep the pose through the impact
        }
        r.braceTarget = target;
        r.brace += (target - r.brace) * (1.0f - std::exp(-(target > r.brace ? 11.0f : 2.2f) * dt));

        // Landing spot for the hands: along the velocity, at the ground.
        r.landingReach = 0.0f;
        if (r.brace > 0.15f) {
            JVec3 dir = vel;
            if (dir.LengthSq() < 0.5f) dir = JVec3(0, -1, 0);
            dir = dir.Normalized();
            if (dir.GetY() > -0.2f) dir = (dir + JVec3(0, -0.8f, 0)).Normalized();
            JPH::RRayCast ray(RVec3(pelvisPos), dir * 3.0f);
            JPH::RayCastSettings rs;
            JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;
            sys->GetNarrowPhaseQuery().CastRay(ray, rs, hit, JPH::SpecifiedBroadPhaseLayerFilter(JPH::BroadPhaseLayer(0)), JPH::SpecifiedObjectLayerFilter(0));
            if (hit.HadHit()) { r.landing = pelvisPos + dir * (hit.mHit.mFraction * 3.0f); r.landingReach = 1.0f; }
        }

        // Muscle targets. The animation gives each joint's rotation in its parent's frame; the brace and reach blend over it.
        const float tension = std::clamp(t.tension, 0.0f, 1.0f) * r.wake;
        std::vector<JQuat>& animQ = animScratch;
        animQ.resize(kRagdollParts);
        for (int p = 0; p < kRagdollParts; ++p) {
            const Part& part = r.part[p];
            animQ[(size_t)p] = RotOf(MatFrom(anim + (size_t)part.jointIndex * 16));
        }
        for (int p = 1; p < kRagdollParts; ++p) {
            const Def& d = kDefs[p];
            Part& part = r.part[p];
            if (!part.constraint) continue;
            // Bracing makes the muscles harder than at rest.
            const float k = std::clamp(tension * (1.0f + 0.35f * r.brace), 0.0f, 1.0f);
            SetMuscles(r, p, k);
            JQuat rel = animQ[(size_t)d.parent].Conjugated() * animQ[(size_t)p];
            JQuat want = rel;
            const bool arm = (p == (int)RagdollPart::UpperArmL || p == (int)RagdollPart::UpperArmR);
            if (d.kind == Hinge) {
                const JQuat delta = rel * part.restRel.Conjugated();
                float angle = AngleAbout(delta, part.hingeAxisLocal);
                angle = angle + (part.braceHinge - angle) * r.brace;
                // Reaching hands keep the elbows nearly straight.
                if (r.landingReach > 0.0f && (p == (int)RagdollPart::LowerArmL || p == (int)RagdollPart::LowerArmR)) {
                    const int up = p == (int)RagdollPart::LowerArmL ? (int)RagdollPart::UpperArmL : (int)RagdollPart::UpperArmR;
                    const float reach = ReachAmount(r, up);
                    angle += (0.3f - angle) * reach;
                }
                auto* h = static_cast<JPH::HingeConstraint*>(part.constraint.GetPtr());
                h->SetTargetAngle(std::clamp(angle, -d.hyper, d.flexMax) + part.hingeRef);
            } else {
                want = Nlerp(rel, part.braceDelta * part.restRel, r.brace);
                if (arm && r.landingReach > 0.0f) {
                    const float reach = ReachAmount(r, p);
                    if (reach > 0.0f) {
                        const JPH::BodyID chest = r.part[d.parent].body;
                        const JQuat chestRot = bi.GetRotation(chest);
                        const JVec3 shoulder = JVec3(bi.GetPosition(part.body));
                        const JVec3 u = chestRot.Conjugated() * (r.landing - shoulder).Normalized();
                        const JVec3 v0 = part.restRel * part.boneDirLocal;
                        want = Nlerp(want, FromTo(v0, u) * part.restRel, reach);
                    }
                }
                auto* c = static_cast<JPH::SwingTwistConstraint*>(part.constraint.GetPtr());
                // The constraint wants body 1 -> body 2, which is what `want` is: the joint frames are the body frames.
                c->SetTargetOrientationBS(want.Normalized());
            }
            if (part.coolDown > 0.0f) part.coolDown -= dt;
        }

        // Status for the caller.
        const bool still = vel.Length() < 0.25f && bi.GetAngularVelocity(pelvisId).Length() < 0.6f && !r.airborne;
        r.restTime = still ? r.restTime + dt : 0.0f;
    }

    // 0..1: how well the hand on this arm can meet the landing spot.
    float ReachAmount(Instance& r, int upperArm) {
        if (r.landingReach <= 0.0f || r.brace < 0.2f) return 0.0f;
        const JVec3 shoulder = JVec3(Bodies().GetPosition(r.part[upperArm].body));
        const float reach = 0.60f * std::max(r.tuning.massKg > 0 ? 1.0f : 1.0f, 1.0f);
        const float dist = (r.landing - shoulder).Length();
        return SmoothStep(reach + 0.35f, reach - 0.05f, dist) * SmoothStep(0.2f, 0.7f, r.brace);
    }

    // -------------------------------------------------------------- injuries
    void Post(uint32_t id, RagdollEventKind kind, int part, float speed, float impulse, JVec3 point, int other = -1) {
        RagdollEvent e;
        e.kind = kind; e.part = part; e.other = other; e.speed = speed; e.impulse = impulse; e.point = FromJ(point); e.id = id;
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }

    void DoDislocate(Instance& r, int p, bool announce) {
        Part& part = r.part[p];
        const Def& d = kDefs[p];
        if (part.dislocated || p == 0 || !part.constraint) return;
        part.dislocated = true;
        r.dislocatedMask |= 1u << p;
        part.strength = std::min(part.strength, 0.30f);
        JPH::BodyInterface& bi = Bodies();
        const JVec3 at = JVec3(bi.GetPosition(part.body));
        if (d.kind == Hinge) {
            // The range moves into the hyper-extended zone and never comes back. Jolt wants min <= 0 <= max, so the hinge is
            // rebuilt with its zero turned, and the muscles' targets turn with it (hingeRef).
            auto* old = static_cast<JPH::HingeConstraint*>(part.constraint.GetPtr());
            const float phi = old->GetCurrentAngle() - part.hingeRef;
            const float lo = -2.20f, hi = -0.30f;
            const float ref = 1.20f;
            const Part& par = r.part[d.parent];
            const JPH::BodyID ids[2] = {par.body, part.body};
            {
                JPH::BodyLockMultiWrite lock(sys->GetBodyLockInterfaceNoLock(), ids, 2);
                JPH::Body* b1 = lock.GetBody(0);
                JPH::Body* b2 = lock.GetBody(1);
                if (b1 && b2) {
                    const JQuat q1 = b1->GetRotation();
                    const JVec3 axis = q1 * part.hingeAxisLocal;
                    const JVec3 n1 = q1 * part.hingeNormalLocal;
                    const JVec3 n2 = JQuat::sRotation(axis, phi + ref) * n1; // reports phi + ref in the new constraint
                    JPH::HingeConstraintSettings hs;
                    hs.mSpace = JPH::EConstraintSpace::WorldSpace;
                    hs.mPoint1 = hs.mPoint2 = RVec3(b2->GetPosition());
                    hs.mHingeAxis1 = hs.mHingeAxis2 = axis;
                    hs.mNormalAxis1 = n1;
                    hs.mNormalAxis2 = n2;
                    hs.mLimitsMin = lo + ref;
                    hs.mLimitsMax = hi + ref;
                    hs.mMaxFrictionTorque = d.friction;
                    hs.mMotorSettings = JPH::MotorSettings(d.freq, 1.0f);
                    hs.mMotorSettings.SetTorqueLimit(d.torque);
                    JPH::Ref<JPH::TwoBodyConstraint> fresh = hs.Create(*b1, *b2);
                    sys->RemoveConstraint(part.constraint);
                    part.constraint = fresh;
                    sys->AddConstraint(part.constraint);
                    part.hingeRef = ref;
                }
            }
        } else {
            auto* c = static_cast<JPH::SwingTwistConstraint*>(part.constraint.GetPtr());
            c->SetNormalHalfConeAngle(std::min(c->GetNormalHalfConeAngle() + 0.75f, 2.9f));
            c->SetPlaneHalfConeAngle(std::min(c->GetPlaneHalfConeAngle() + 0.55f, 2.9f));
            c->SetTwistMinAngle(std::max(c->GetTwistMinAngle() - 0.8f, -3.0f));
            c->SetTwistMaxAngle(std::min(c->GetTwistMaxAngle() + 0.8f, 3.0f));
        }
        // The jolt as it goes: both segments are thrown opposite ways about the joint.
        const JVec3 kick(HashFloat(r.seed, (uint32_t)p), HashFloat(r.seed, (uint32_t)p + 31), HashFloat(r.seed, (uint32_t)p + 77));
        bi.AddAngularImpulse(part.body, kick * part.mass * 0.35f);
        bi.AddAngularImpulse(r.part[d.parent].body, -kick * part.mass * 0.25f);
        if (announce) Post(r.id, RagdollEventKind::Dislocation, p, 0.0f, 0.0f, at);
    }

    void DoFracture(Instance& r, int p, float impulse) {
        Part& part = r.part[p];
        if (part.fractured || p == 0) return;
        part.fractured = true;
        r.fracturedMask |= 1u << p;
        part.strength = std::min(part.strength, 0.12f);
        // A broken bone cannot carry the next joint: the joint above it goes slack as well.
        const Def& d = kDefs[p];
        r.part[d.parent].strength = std::min(r.part[d.parent].strength, 0.7f);
        Post(r.id, RagdollEventKind::Fracture, p, 0.0f, impulse, JVec3(Bodies().GetPosition(part.body)));
    }

    void Process(float dt) {
        std::vector<Pending> now;
        {
            std::lock_guard<std::mutex> lock(mutex);
            now.swap(pending);
        }
        for (const Pending& e : now) {
            const auto it = items.find(e.id);
            if (it == items.end()) continue;
            Instance& r = *it->second;
            Part& part = r.part[e.part];
            const Def& d = kDefs[e.part];
            part.peakImpulse = std::max(part.peakImpulse, e.impulse);
            if (part.coolDown <= 0.0f) {
                // Impacts for sound: not on every contact frame.
                Post(e.id, RagdollEventKind::Impact, e.part, e.speed, e.impulse, e.point, e.self ? 1 : 0);
                part.coolDown = 0.08f;
            }
            if (!r.tuning.injuries || e.self) continue;
            const float tough = r.tuning.toughness;
            if (d.fracture > 0.0f && e.impulse > d.fracture * tough && e.part != (int)RagdollPart::Head) DoFracture(r, e.part, e.impulse);
            if (e.part == (int)RagdollPart::Head && e.impulse > d.fracture * tough && !r.unconscious) {
                r.wakeTarget = 0.0f;
                Post(e.id, RagdollEventKind::Knockout, e.part, e.speed, e.impulse, e.point);
            } else if ((e.part <= (int)RagdollPart::Chest) && e.impulse > 290.0f * tough && !r.unconscious) {
                r.wakeTarget = 0.0f;
                Post(e.id, RagdollEventKind::Knockout, e.part, e.speed, e.impulse, e.point);
            }
        }
        // Joint stops: the constraint's impulse through a limit is the load on the joint.
        for (auto& [id, ptr] : items) {
            Instance& r = *ptr;
            if (!r.tuning.injuries) continue;
            for (int p = 1; p < kRagdollParts; ++p) {
                Part& part = r.part[p];
                const Def& d = kDefs[p];
                if (!part.constraint || part.dislocated || d.dislocate <= 0.0f) continue;
                float load = 0.0f;
                if (d.kind == Hinge) {
                    // Only the hyper-extension stop pops a hinge; folding all the way shut is soft tissue meeting soft tissue.
                    auto* h = static_cast<JPH::HingeConstraint*>(part.constraint.GetPtr());
                    if (h->GetCurrentAngle() - part.hingeRef < -d.hyper + 0.15f) load = std::fabs(h->GetTotalLambdaRotationLimits());
                }
                else {
                    auto* c = static_cast<JPH::SwingTwistConstraint*>(part.constraint.GetPtr());
                    load = std::fabs(c->GetTotalLambdaTwist()) + std::fabs(c->GetTotalLambdaSwingY()) + std::fabs(c->GetTotalLambdaSwingZ());
                }
                const float torque = load / lastSub;
                part.load = std::max(part.load, torque);
                const float tolerance = d.dislocate * r.tuning.toughness * (0.7f + 0.3f * std::clamp(r.tuning.tension * r.wake, 0.0f, 1.0f));
                if (torque > tolerance) DoDislocate(r, p, true);
            }
        }
        (void)dt;
    }

    std::vector<JQuat> animScratch;
};

// ------------------------------------------------------------------------------------------------ public

RagdollWorld::RagdollWorld(void* system, unsigned moving) : impl_(std::make_unique<Impl>()) {
    impl_->sys = static_cast<JPH::PhysicsSystem*>(system);
    impl_->moving = (JPH::ObjectLayer)moving;
    impl_->sys->SetContactListener(impl_.get());
}
RagdollWorld::~RagdollWorld() {
    if (!impl_) return;
    Clear();
    if (impl_->sys && impl_->sys->GetContactListener() == impl_.get()) impl_->sys->SetContactListener(nullptr);
}

uint32_t RagdollWorld::Create(const RagdollSkeleton& s, const float* pose, const float* m2w, tbd::Vec3 v, const RagdollTuning& t) { return impl_->Create(s, pose, m2w, v, t); }
void RagdollWorld::Destroy(uint32_t id) {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return;
    impl_->Remove(*it->second);
    impl_->items.erase(it);
}
void RagdollWorld::Clear() {
    for (auto& [id, ptr] : impl_->items) impl_->Remove(*ptr);
    impl_->items.clear();
    impl_->bodyMap.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pending.clear();
}
bool RagdollWorld::Exists(uint32_t id) const { return impl_->items.count(id) != 0; }
size_t RagdollWorld::Count() const { return impl_->items.size(); }
int RagdollWorld::Substeps(float dt) { return std::clamp((int)std::ceil(dt * 120.0f - 1e-3f), 1, 4); }

void RagdollWorld::Drive(uint32_t id, const float* animated, float dt) {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return;
    impl_->lastSub = std::max(dt, 1.0f / 240.0f) / (float)Substeps(dt);
    impl_->Drive(*it->second, animated, dt);
}
void RagdollWorld::PostStep(float dt) { impl_->Process(dt); }

bool RagdollWorld::Pose(uint32_t id, const float* modelToWorld, std::vector<float>& out) const {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return false;
    const Impl::Instance& r = *it->second;
    const Mat44 w2m = Rigid(MatFrom(modelToWorld)).InversedRotationTranslation();
    JPH::BodyInterface& bi = impl_->Bodies();
    Mat44 world[kRagdollParts];
    for (int p = 0; p < kRagdollParts; ++p) {
        const JPH::BodyID b = r.part[p].body;
        world[p] = Mat44::sRotationTranslation(bi.GetRotation(b), JVec3(bi.GetPosition(b)));
    }
    out.resize((size_t)r.jointCount * 16);
    for (int j = 0; j < r.jointCount; ++j) {
        Mat44 m = world[r.owner[(size_t)j]] * r.followRel[(size_t)j];
        if (r.blendWith[(size_t)j] >= 0) {
            const Mat44 other = world[r.blendWith[(size_t)j]] * r.part[r.blendWith[(size_t)j]].bind.InversedRotationTranslation() * r.part[r.owner[(size_t)j]].bind * r.followRel[(size_t)j];
            // Halfway between the two bones: the same joint carried by each.
            const JQuat q = Nlerp(m.GetQuaternion(), other.GetQuaternion(), 0.5f);
            const JVec3 pos = (m.GetTranslation() + other.GetTranslation()) * 0.5f;
            m = Mat44::sRotationTranslation(q, pos);
        }
        MatTo(w2m * m, &out[(size_t)j * 16]);
    }
    return true;
}

RagdollStatus RagdollWorld::Status(uint32_t id) const {
    RagdollStatus s;
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return s;
    Impl::Instance& r = *it->second;
    for (int p = 0; p < kRagdollParts; ++p) {
        s.jointLoad[p] = r.part[p].load;
        s.peakImpulse[p] = r.part[p].peakImpulse;
        r.part[p].load = r.part[p].peakImpulse = 0.0f;
    }
    JPH::BodyInterface& bi = impl_->Bodies();
    s.alive = true;
    s.pelvis = FromJ(JVec3(bi.GetCenterOfMassPosition(r.part[0].body)));
    s.head = FromJ(JVec3(bi.GetCenterOfMassPosition(r.part[(int)RagdollPart::Head].body)));
    s.velocity = FromJ(bi.GetLinearVelocity(r.part[0].body));
    s.speed = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y + s.velocity.z * s.velocity.z);
    s.brace = r.brace;
    s.tension = r.tuning.tension * r.wake;
    s.restTime = r.restTime;
    s.airborne = r.airborne;
    s.unconscious = r.unconscious;
    s.dislocated = r.dislocatedMask;
    s.fractured = r.fracturedMask;
    return s;
}
RagdollTuning* RagdollWorld::Tuning(uint32_t id) {
    const auto it = impl_->items.find(id);
    return it == impl_->items.end() ? nullptr : &it->second->tuning;
}

void RagdollWorld::Blast(uint32_t id, tbd::Vec3 centre, float speed, float radius) {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end() || radius <= 0.0f) return;
    Impl::Instance& r = *it->second;
    JPH::BodyInterface& bi = impl_->Bodies();
    const JVec3 c = ToJ(centre);
    for (int p = 0; p < kRagdollParts; ++p) {
        Impl::Part& part = r.part[p];
        const JVec3 com = JVec3(bi.GetCenterOfMassPosition(part.body));
        JVec3 away = com - c;
        const float dist = away.Length();
        if (dist > radius) continue;
        away = dist > 1e-3f ? away / dist : JVec3(0, 1, 0);
        const float fall = (1.0f - dist / radius) * (1.0f - dist / radius);
        // The blast lifts as well as pushes, and light distal limbs are thrown harder than the trunk.
        const bool distal = p >= (int)RagdollPart::LowerArmL && p != (int)RagdollPart::ThighL && p != (int)RagdollPart::ThighR && p != (int)RagdollPart::Chest && p != (int)RagdollPart::Abdomen;
        const float limb = distal ? 1.0f + 0.5f * std::fabs(HashFloat(r.seed, (uint32_t)p + 5)) : 1.0f;
        JVec3 dir = (away + JVec3(0, 0.45f, 0)).Normalized();
        // Each limb also gets a sideways component of its own.
        const JVec3 side = JVec3(HashFloat(r.seed, (uint32_t)p), 0.25f * HashFloat(r.seed, (uint32_t)p + 9), HashFloat(r.seed, (uint32_t)p + 17));
        dir = (dir + side * (distal ? 0.45f : 0.12f)).Normalized();
        bi.AddImpulse(part.body, dir * (speed * fall * limb * part.mass));
        const JVec3 spin = JVec3(HashFloat(r.seed, (uint32_t)p + 40), HashFloat(r.seed, (uint32_t)p + 41), HashFloat(r.seed, (uint32_t)p + 42));
        bi.AddAngularImpulse(part.body, spin * (speed * fall * part.mass * 0.12f * limb));
        bi.ActivateBody(part.body);
    }
    r.airTime = 0.0f; // the surprise: the reflex starts after the reaction time
    r.seed = r.seed * 1664525u + 1013904223u;
}

void RagdollWorld::Hit(uint32_t id, tbd::Vec3 point, tbd::Vec3 impulse) {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return;
    Impl::Instance& r = *it->second;
    JPH::BodyInterface& bi = impl_->Bodies();
    int best = 0;
    float bestDist = 1e9f;
    for (int p = 0; p < kRagdollParts; ++p) {
        const float d = (JVec3(bi.GetCenterOfMassPosition(r.part[p].body)) - ToJ(point)).LengthSq();
        if (d < bestDist) { bestDist = d; best = p; }
    }
    bi.AddImpulse(r.part[best].body, ToJ(impulse), RVec3(ToJ(point)));
    const float magnitude = std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y + impulse.z * impulse.z);
    // The blow is judged like a contact: it can break the bone or knock the character out.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending.push_back({id, best, magnitude / std::max(r.part[best].mass, 0.1f), magnitude, ToJ(point), false});
    }
}

void RagdollWorld::Knockout(uint32_t id, bool out) {
    const auto it = impl_->items.find(id);
    if (it == impl_->items.end()) return;
    it->second->wakeTarget = out ? 0.0f : 1.0f;
}
void RagdollWorld::SetTension(uint32_t id, float t) {
    if (RagdollTuning* tuning = Tuning(id)) tuning->tension = std::clamp(t, 0.0f, 1.0f);
}
void RagdollWorld::Dislocate(uint32_t id, int part) {
    const auto it = impl_->items.find(id);
    if (it != impl_->items.end() && part > 0 && part < kRagdollParts) impl_->DoDislocate(*it->second, part, true);
}
void RagdollWorld::Fracture(uint32_t id, int part) {
    const auto it = impl_->items.find(id);
    if (it != impl_->items.end() && part > 0 && part < kRagdollParts) impl_->DoFracture(*it->second, part, 0.0f);
}
void RagdollWorld::Shift(tbd::Vec3 offset) {
    JPH::BodyInterface& bi = impl_->Bodies();
    for (auto& [id, ptr] : impl_->items)
        for (int p = 0; p < kRagdollParts; ++p) bi.SetPosition(ptr->part[p].body, bi.GetPosition(ptr->part[p].body) - ToJ(offset), JPH::EActivation::DontActivate);
}
std::vector<RagdollEvent> RagdollWorld::TakeEvents() {
    std::vector<RagdollEvent> out;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.swap(impl_->events);
    return out;
}

} // namespace tbd
