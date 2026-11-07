// The player in Play mode: Wyatt's five entities on a Jolt capsule, a third-person camera that stops at walls, clips blended by
// the speed he actually moves at (so running into a wall stops his legs), and his wake in the water. Part of main's includes;
// the renderer is not touched. PlayerScript is TBD_PLAYTEST: fixed-step scripted input that writes frames and a trace.
#pragma once
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "camera.h"
#include "physics.h"
#include "ragdoll_actor.h"
#include "scene.h"
#include "skeletal_anim.h"
#include "vecmath.h"

namespace tbd {

struct PlayerInput {
    float forward = 0.0f, right = 0.0f; // -1..1, relative to the camera
    bool run = false, jump = false;
    float lookX = 0.0f, lookY = 0.0f;   // pixels
};

class PlayerCharacter {
public:
    using WaterHeightFn = bool (*)(float x, float z, float& y);
    using WaterImpulseFn = void (*)(Vec3 pos, float radius, float strength);
    using FootstepFn = void (*)(Vec3 pos, float volume, float pitch);
    FootstepFn footstep = nullptr; // called at each foot contact on the ground

    static constexpr float kFootOffset = 0.79f;        // entity origin above the boot soles: the rig's Root sits below it
    static constexpr float kWalkSpeed = 1.6f, kRunSpeed = 3.8f;
    static constexpr float kWalkClipSpeed = 1.47f;     // walk_loop's root travel, m/s
    static constexpr float kRunClipSpeed = 2.74f;      // run has no root travel: how fast the stance foot sweeps back
    static constexpr float kStep = 1.0f / 60.0f;
    static constexpr float kRunPhaseOffset = 0.14f;    // run's left foot lands at phase 0.29, walk's at 0.15: lines the blend up

    bool Active() const { return !entities_.empty(); }
    bool Placed() const { return placed_; }
    bool OwnsEntity(size_t index) const {
        for (const Saved& s : entities_)
            if ((size_t)s.entity == index) return true;
        return false;
    }
    const CharacterState& State() const { return state_; }
    float Depth() const { return depth_; }
    bool FirstPerson() const { return firstPerson_; }

    // Ragdoll (ragdoll.h). A request is applied at the next Update: 1 toggle ragdoll / stand up, 2 blast from behind, 3 knock out /
    // wake, 4 pop the left elbow, 5 fracture the right shin, 6 hit the head. R, B, K in Play and TBD_PLAYTEST commands send them.
    void Request(int what) { requests_.push_back(what); }
    bool Ragdolling() const { return ragdoll_.Active(); }
    RagdollActor& Ragdoll() { return ragdoll_; }
    RagdollTuning ragdollTuning;

    // V: the camera moves into his eyes; his head and hat shrink away so they do not fill the view, and he faces where it looks.
    void ToggleView() { toggleView_ = true; } // applied at the next Update
    void ApplyView(Scene& scene) {
        toggleView_ = false;
        firstPerson_ = !firstPerson_;
        for (const Saved& s : entities_) {
            Entity& e = scene.entities[(size_t)s.entity];
            if (e.name == "Wyatt Head" || e.name == "Hat") e.scale = firstPerson_ ? Vec3{0.001f, 0.001f, 0.001f} : s.scale;
        }
    }

    // Finds the character (the entities bound to wyatt_* clips). False when the scene has none.
    bool Begin(Scene& scene, const std::vector<MeshId>& meshes, PhysicsWorld& physics, SkeletalAnimator& animator) {
        End(scene);
        std::vector<Saved> found;
        for (size_t i = 0; i < scene.entities.size() && i < meshes.size(); ++i) {
            const std::string& clip = scene.entities[i].animClip;
            if (clip.find("wyatt_") != std::string::npos && meshes[i] != kInvalidMesh)
                found.push_back({(int)i, meshes[i], scene.entities[i].position, scene.entities[i].rotation, scene.entities[i].scale});
        }
        if (found.empty()) return false;
        const std::string first = scene.entities[(size_t)found[0].entity].animClip;
        const size_t cut = first.find_last_of("\\/");
        const std::string dir = cut == std::string::npos ? "" : first.substr(0, cut + 1);
        const std::vector<std::string> clips = {dir + "wyatt_idle.glb", dir + "wyatt_walk_loop.glb", dir + "wyatt_run.glb", dir + "wyatt_jump.glb"};
        for (const Saved& s : found)
            if (!animator.SetBlendClips(s.mesh, clips, "Root")) Log("Player: the blend clips did not apply to %s", scene.entities[(size_t)s.entity].name.c_str());
        walkDuration_ = animator.ClipDuration(clips[1]);
        runDuration_ = animator.ClipDuration(clips[2]);
        idleDuration_ = animator.ClipDuration(clips[0]);
        physics_ = &physics;
        animator_ = &animator;
        entities_ = std::move(found);
        firstPerson_ = false;
        const Vec3 start = scene.entities[(size_t)entities_[0].entity].position;
        prevFeet_ = feet_ = {start.x, start.y - kFootOffset, start.z};
        facing_ = 0.0f;
        const Vec3 forward = Rotate(scene.entities[(size_t)entities_[0].entity].rotation, {0, 0, 1});
        facing_ = std::atan2(forward.x, forward.z);
        placed_ = false;
        speed_ = 0.0f;
        phase_ = idleTime_ = accumulator_ = air_ = jumpWeight_ = 0.0f;
        wasWet_ = false;
        cameraDistance_ = 3.2f;
        return true;
    }

    // Back to the editor's pose and the scene's own clip.
    void End(Scene& scene) {
        ragdoll_.End();
        for (const Saved& s : entities_) {
            if (s.entity < (int)scene.entities.size()) {
                scene.entities[(size_t)s.entity].position = s.position;
                scene.entities[(size_t)s.entity].rotation = s.rotation;
                scene.entities[(size_t)s.entity].scale = s.scale;
            }
            if (animator_) animator_->ClearBlend(s.mesh);
        }
        entities_.clear();
        if (physics_) physics_->DestroyCharacter();
    }

    void Teleport(Vec3 at) {
        feet_ = prevFeet_ = at;
        placed_ = false;
    }

    // A frame: holds until the ground around him has loaded, then fixed steps of the capsule; the entities, clips and water follow.
    void Update(Scene& scene, const PlayerInput& input, float dt, OrbitCamera& camera, WaterHeightFn waterHeight, WaterImpulseFn waterImpulse) {
        if (!Active() || !physics_) return;
        if (toggleView_) ApplyView(scene);
        if (!requests_.empty()) { const std::vector<int> now = std::move(requests_); requests_.clear(); for (int what : now) Handle(scene, what); }
        camera.yaw -= input.lookX * 0.004f;
        camera.pitch = std::clamp(camera.pitch + input.lookY * 0.004f, firstPerson_ ? -1.3f : -0.5f, firstPerson_ ? 1.3f : 1.25f);
        if (ragdoll_.Active()) { UpdateRagdoll(scene, camera, dt); return; }
        physics_->Update(feet_, dt);

        if (!placed_) {
            if (!physics_->ReadyAround(feet_, 6.0f)) { Pose(scene, feet_, dt, camera); return; }
            RayHit hit;
            Vec3 at = feet_;
            // Ground right below where he stood; after a teleport, the highest surface there (nothing says how high it is).
            if (physics_->Raycast(at + Vec3{0, 1.0f, 0}, {0, -1, 0}, 3.0f, hit)) at.y = hit.point.y + 0.02f;
            else if (physics_->Raycast({at.x, 5000.0f, at.z}, {0, -1, 0}, 10000.0f, hit)) at.y = hit.point.y + 0.02f;
            else Log("Player: no ground found at %.1f %.1f", at.x, at.z);
            physics_->CreateCharacter(at);
            prevFeet_ = feet_ = at;
            state_ = physics_->Character();
            placed_ = true;
            Log("Player: stands at %.2f %.2f %.2f", at.x, at.y, at.z);
        }

        // Wanted motion, from the camera's flat forward.
        const Vec3 forward = {-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw)};
        const Vec3 right = {-forward.z, 0.0f, forward.x};
        Vec3 wish = forward * input.forward + right * input.right;
        const float amount = std::sqrt(Dot(wish, wish));
        if (amount > 1.0f) wish = wish * (1.0f / amount);
        float top = input.run ? kRunSpeed : kWalkSpeed;

        // Water: slows him with depth; deep enough and he floats with his chest at the surface.
        float surface = 0.0f;
        const bool overWater = waterHeight && waterHeight(feet_.x, feet_.z, surface);
        depth_ = overWater ? std::max(0.0f, surface - feet_.y) : 0.0f;
        top *= 1.0f - 0.65f * std::clamp(depth_ / 1.2f, 0.0f, 1.0f);

        const Vec3 target = wish * top;
        accumulator_ = std::min(accumulator_ + dt, kStep * 4.0f);
        bool jump = input.jump;
        while (accumulator_ >= kStep) {
            accumulator_ -= kStep;
            const float ease = 1.0f - std::exp(-(state_.onGround ? 12.0f : 2.0f) * kStep);
            moveVelocity_ = moveVelocity_ + (target - moveVelocity_) * ease;
            CharacterStep step;
            step.velocity = moveVelocity_;
            step.jump = jump;
            step.swim = overWater && depth_ > 1.35f;
            step.swimFeetY = surface - 1.35f;
            jump = false;
            prevFeet_ = feet_;
            state_ = physics_->StepCharacter(step, kStep);
            feet_ = state_.feet;
        }
        const float blend = accumulator_ / kStep;
        const Vec3 drawn = prevFeet_ + (feet_ - prevFeet_) * blend;

        const float actual = std::sqrt(state_.velocity.x * state_.velocity.x + state_.velocity.z * state_.velocity.z);
        const float moved = std::sqrt((feet_.x - prevFeet_.x) * (feet_.x - prevFeet_.x) + (feet_.z - prevFeet_.z) * (feet_.z - prevFeet_.z)) / kStep;
        const float groundSpeed = std::min(actual, moved + 0.01f); // what he covered, not what he wished: walls stop his legs
        speed_ += (groundSpeed - speed_) * (1.0f - std::exp(-10.0f * dt));
        if (firstPerson_) facing_ = camera.yaw + 3.14159265f; // the body turns with the view
        else if (amount > 0.05f) { // turns toward where he is told to go
            const float want = std::atan2(wish.x, wish.z);
            float delta = std::fmod(want - facing_ + 3.14159265f * 3.0f, 3.14159265f * 2.0f) - 3.14159265f;
            facing_ += delta * (1.0f - std::exp(-10.0f * dt));
        }
        Pose(scene, drawn, dt, camera);
        // A foot comes down twice a cycle (walk_loop's left foot lands at phase 0.15, its right half a cycle later).
        const float beat = std::fmod(phase_ + 1.0f - 0.15f, 0.5f) * 2.0f;
        footfall_ = beat < lastBeat_;
        lastBeat_ = beat;
        if (footfall_ && state_.onGround && speed_ > 0.3f && depth_ < 1.0f) {
            ++steps_;
            if (footstep) footstep(feet_, depth_ > 0.05f ? 0.45f : 0.6f, 0.94f + 0.03f * (float)(steps_ % 5));
        }
        Water(waterHeight, waterImpulse, dt);
    }

    float Speed() const { return speed_; }

private:
    struct Saved { int entity; MeshId mesh; Vec3 position; Quat rotation; Vec3 scale; };

    bool StartRagdoll(Scene& scene, Vec3 extra) {
        if (!placed_ || ragdoll_.Active() || entities_.empty()) return false;
        std::vector<std::pair<int, MeshId>> bodies;
        for (const Saved& s : entities_) bodies.push_back({s.entity, s.mesh});
        physics_->DestroyCharacter();
        if (!ragdoll_.Begin(scene, bodies, *physics_, *animator_, state_.velocity + extra, ragdollTuning)) { placed_ = false; return false; }
        fallTime_ = 0.3f;
        return true;
    }
    // Back on his feet where the pelvis came to rest, standing on the ground below it.
    void StopRagdoll(Scene& scene) {
        if (!ragdoll_.Active()) return;
        const Vec3 pelvis = ragdoll_.Status().pelvis;
        ragdoll_.End();
        RayHit hit;
        Vec3 at = {pelvis.x, pelvis.y, pelvis.z};
        if (physics_->Raycast(at + Vec3{0, 0.5f, 0}, {0, -1, 0}, 4.0f, hit)) at.y = hit.point.y + 0.02f;
        else at.y -= 0.9f;
        facing_ = 0.0f;
        Teleport(at);
        moveVelocity_ = {0, 0, 0};
        (void)scene;
    }
    void Handle(Scene& scene, int what) {
        if (what == 1) {
            if (ragdoll_.Active()) StopRagdoll(scene);
            else StartRagdoll(scene, {0, 0, 0});
            return;
        }
        if (!ragdoll_.Active() && !StartRagdoll(scene, what == 2 ? Vec3{0, 1.0f, 0} : Vec3{0, 0, 0})) return;
        RagdollWorld& dolls = physics_->Ragdolls();
        const uint32_t id = ragdoll_.Id();
        const RagdollStatus st = dolls.Status(id);
        if (what == 2) { // an explosion at his back
            const Vec3 back = {-std::sin(facing_), 0.0f, -std::cos(facing_)};
            dolls.Blast(id, st.pelvis + back * 0.8f + Vec3{0, -0.5f, 0}, 13.0f, 5.0f);
        } else if (what == 3) dolls.Knockout(id, !st.unconscious);
        else if (what == 4) dolls.Dislocate(id, (int)RagdollPart::LowerArmL);
        else if (what == 5) dolls.Fracture(id, (int)RagdollPart::CalfR);
        else if (what == 6) dolls.Hit(id, st.head, Vec3{std::sin(facing_), 0.2f, std::cos(facing_)} * 90.0f);
    }
    void UpdateRagdoll(Scene& scene, OrbitCamera& camera, float dt) {
        RagdollWorld& dolls = physics_->Ragdolls();
        const RagdollStatus before = ragdoll_.Status();
        // The clips underneath are the muscles' target: the falling pose, played faster on the way up and slower coming down.
        fallTime_ = std::clamp(fallTime_ + dt * std::clamp(1.0f + before.velocity.y * 0.12f, 0.35f, 1.8f), 0.0f, 1.2f);
        const float times[4] = {0.0f, 0.0f, 0.0f, fallTime_};
        const float weights[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (const Saved& s : entities_) animator_->SetBlendPose(s.mesh, times, weights);
        ragdoll_.Drive(dt);
        physics_->Update(before.pelvis, dt);
        ragdoll_.Apply(scene);
        for (const RagdollEvent& e : dolls.TakeEvents()) {
            const Vec3 at = e.point;
            if (e.kind == RagdollEventKind::Impact) {
                if (footstep && e.other != 1 && e.impulse > 12.0f) footstep(at, std::clamp(e.impulse / 140.0f, 0.12f, 1.0f), 0.55f + 0.03f * (float)(e.part % 5));
            } else {
                const char* what = e.kind == RagdollEventKind::Dislocation ? "dislocated" : e.kind == RagdollEventKind::Fracture ? "fractured" : "knocked out";
                Log("Ragdoll: %s %s (%.0f N s)", RagdollPartName(e.part), what, e.impulse);
            }
        }
        const RagdollStatus now = ragdoll_.Status();
        feet_ = prevFeet_ = {now.pelvis.x, now.pelvis.y - 0.9f, now.pelvis.z};
        const float cp = std::cos(camera.pitch);
        camera.target = now.pelvis + Vec3{0, 0.4f, 0};
        camera.distance = 3.6f;
        (void)cp;
    }

    void Pose(Scene& scene, Vec3 feet, float dt, OrbitCamera& camera) {
        const Quat rotation = QuatAxisAngle({0, 1, 0}, facing_);
        for (const Saved& s : entities_) {
            Entity& e = scene.entities[(size_t)s.entity];
            e.position = {feet.x, feet.y + kFootOffset, feet.z};
            e.rotation = rotation;
        }
        // Clips by speed; the legs of walk and run share a phase, so a blend never crosses them.
        const float toWalk = std::clamp(speed_ / kWalkClipSpeed, 0.0f, 1.0f);
        const float toRun = std::clamp((speed_ - kWalkSpeed) / (kRunSpeed - kWalkSpeed), 0.0f, 1.0f);
        const float walkRate = speed_ / kWalkClipSpeed / std::max(walkDuration_, 0.01f), runRate = speed_ / kRunClipSpeed / std::max(runDuration_, 0.01f);
        phase_ = std::fmod(phase_ + dt * (walkRate + (runRate - walkRate) * toRun), 1.0f);
        idleTime_ += dt;
        const bool airborne = placed_ && !state_.onGround && depth_ < 0.8f;
        air_ = airborne ? air_ + dt : 0.0f;
        jumpTime_ = airborne && air_ > 0.1f ? std::min(jumpTime_ + dt, 1.2f) : 0.3f;
        jumpWeight_ += ((airborne && air_ > 0.1f ? 1.0f : 0.0f) - jumpWeight_) * (1.0f - std::exp(-12.0f * dt));
        const float g = 1.0f - jumpWeight_;
        const float times[4] = {std::fmod(idleTime_, std::max(idleDuration_, 0.01f)), phase_ * walkDuration_, std::fmod(phase_ + kRunPhaseOffset, 1.0f) * runDuration_, jumpTime_};
        const float weights[4] = {(1.0f - toWalk) * g, toWalk * (1.0f - toRun) * g, toWalk * toRun * g, jumpWeight_};
        for (const Saved& s : entities_) animator_->SetBlendPose(s.mesh, times, weights);

        if (firstPerson_) { // eyes 1.62 m up and a little ahead of the skull's centre; the orbit's target sits just in front of them
            const float cp = std::cos(camera.pitch);
            const Vec3 back = {cp * std::sin(camera.yaw), std::sin(camera.pitch), cp * std::cos(camera.yaw)};
            const Vec3 eye = Vec3{feet.x, feet.y + 1.62f, feet.z} + Vec3{std::sin(facing_), 0.0f, std::cos(facing_)} * 0.1f;
            camera.distance = 0.05f;
            camera.target = eye - back * camera.distance;
            return;
        }
        // Camera: over his shoulder, stopped short of whatever is between it and him.
        const Vec3 pivot = {feet.x, feet.y + 1.55f, feet.z};
        const float cp = std::cos(camera.pitch);
        const Vec3 back = {cp * std::sin(camera.yaw), std::sin(camera.pitch), cp * std::cos(camera.yaw)};
        constexpr float kWanted = 3.2f;
        float reach = kWanted;
        float hit = 0.0f;
        if (physics_->SphereCast(pivot, back, kWanted, 0.22f, hit)) reach = std::max(hit, 0.35f);
        cameraDistance_ = reach < cameraDistance_ ? reach : cameraDistance_ + (reach - cameraDistance_) * (1.0f - std::exp(-4.0f * dt));
        camera.target = pivot;
        camera.distance = cameraDistance_;
    }

    void Water(WaterHeightFn waterHeight, WaterImpulseFn waterImpulse, float dt) {
        if (!waterHeight || !waterImpulse) return;
        float surface = 0.0f;
        if (!waterHeight(feet_.x, feet_.z, surface)) { wasWet_ = false; return; }
        const float depth = surface - feet_.y;
        const bool wet = depth > 0.02f;
        const Vec3 at = {feet_.x, surface, feet_.z};
        if (wet && !wasWet_) { // a splash on the way in
            waterImpulse(at, 0.55f, std::clamp(std::fabs(state_.velocity.y) * 0.35f + speed_ * 0.1f, 0.15f, 1.5f));
            ++impulses_;
        }
        wasWet_ = wet;
        if (!wet || depth > 2.2f || speed_ < 0.1f) return;
        // A wake: water heaped up ahead, a hollow behind. Same shape as WaterSystem::Touch, with the character's own measure.
        const float share = std::min(1.0f, dt * 60.0f);
        const Vec3 ahead = {std::sin(facing_), 0.0f, std::cos(facing_)};
        const float push = std::min(speed_, 8.0f) * 0.03f * share;
        waterImpulse(at + ahead * 0.35f, 0.32f, -push);
        waterImpulse(at - ahead * 0.4f, 0.36f, push);
        impulses_ += 2;
        // Each footfall.
        if (footfall_ && depth < 1.0f) {
            waterImpulse(at, 0.28f, std::min(0.12f + speed_ * 0.07f, 0.5f));
            ++impulses_;
        }
    }

public:
    int WaterImpulses() const { return impulses_; }
    int Steps() const { return steps_; }

private:
    PhysicsWorld* physics_ = nullptr;
    SkeletalAnimator* animator_ = nullptr;
    std::vector<Saved> entities_;
    CharacterState state_;
    Vec3 feet_ = {0, 0, 0}, prevFeet_ = {0, 0, 0}, moveVelocity_ = {0, 0, 0};
    float facing_ = 0.0f, speed_ = 0.0f, phase_ = 0.0f, idleTime_ = 0.0f, accumulator_ = 0.0f;
    float walkDuration_ = 1.0f, runDuration_ = 1.0f, idleDuration_ = 1.0f;
    float air_ = 0.0f, jumpTime_ = 0.3f, jumpWeight_ = 0.0f, lastBeat_ = 0.0f, cameraDistance_ = 3.2f, depth_ = 0.0f;
    bool placed_ = false, wasWet_ = false, footfall_ = false, firstPerson_ = false, toggleView_ = false;
    int impulses_ = 0, steps_ = 0;
    RagdollActor ragdoll_;
    float fallTime_ = 0.3f;
    std::vector<int> requests_;
};

// TBD_PLAYTEST="script" runs the default test; any other value is its own script, commands split by ';':
//   wait s | walk s | run s | jump | view (first / third person) | yaw degrees | pitch degrees | teleport x z | shot name | ball height | hour h
// Yaw is where the camera faces: 0 = -z, 90 = -x. Time is fixed at 1/60 s a frame and starts when the ground has loaded, so runs
// repeat. Frames go to TBD_PLAYTEST_DIR\<name>_<api>.bmp, a trace to TBD_PLAYTEST_DIR\trace_<api>.txt; the program quits at the end.
class PlayerScript {
public:
    bool Active() const { return active_; }
    bool Finished() const { return active_ && next_ >= commands_.size() && remaining_ <= 0.0f; }

    bool Start() {
        char v[1024] = {};
        if (tbd::platform::GetEnvA("TBD_PLAYTEST", v, sizeof(v)) == 0) return false;
        std::string text = v;
        if (text == "script" || text == "1") text = kDefault;
        size_t at = 0;
        while (at <= text.size()) {
            const size_t end = text.find(';', at);
            std::string one = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            while (!one.empty() && one.front() == ' ') one.erase(one.begin());
            if (!one.empty()) commands_.push_back(one);
            if (end == std::string::npos) break;
            at = end + 1;
        }
        char d[512] = {};
        dir_ = tbd::platform::GetEnvA("TBD_PLAYTEST_DIR", d, sizeof(d)) > 0 ? d : ".";
        active_ = true;
        return true;
    }

    void OpenTrace(const char* api) {
        api_ = api;
        const std::string path = dir_ + "\\trace_" + api_ + ".txt";
        trace_ = std::fopen(path.c_str(), "w");
    }
    ~PlayerScript() { if (trace_) fclose(trace_); }

    // A file name when this frame should be written, after the frame's input is decided.
    std::string TakeShot() { std::string s = std::move(shot_); shot_.clear(); return s.empty() ? s : dir_ + "\\" + s + "_" + api_ + ".bmp"; }

    float Hour() const { return hour_; }

    // Decides this frame's input; `waiting` holds time still (the ground has not loaded).
    void Tick(PlayerInput& input, PlayerCharacter& player, PhysicsWorld& physics, OrbitCamera& camera, bool waiting, float dt, double animMs) {
        if (waiting || !active_) return;
        while (remaining_ <= 0.0f && next_ < commands_.size() && shot_.empty()) Begin(commands_[next_++], player, physics, camera);
        if (remaining_ > 0.0f) remaining_ -= dt;
        input.forward = mode_ != Mode::Wait ? 1.0f : 0.0f;
        input.run = mode_ == Mode::Run;
        input.jump = jump_;
        jump_ = false;
        clock_ += dt;
        const CharacterState& c = player.State();
        if (trace_ && (++frame_ % 6) == 0) {
            const PhysicsStats st = physics.Stats();
            fprintf(trace_, "t=%.2f cmd=%s feet=%.3f %.3f %.3f vel=%.2f %.2f %.2f ground=%d speed=%.2f depth=%.2f wake=%d steps=%d bodies=%zu update_ms=%.3f char_ms=%.3f anim_ms=%.3f ball=%.3f\n", clock_, label_.c_str(),
                    c.feet.x, c.feet.y, c.feet.z, c.velocity.x, c.velocity.y, c.velocity.z, c.onGround ? 1 : c.steep ? 2 : 0, player.Speed(), player.Depth(), player.WaterImpulses(), player.Steps(), st.bodies, st.updateMs, st.characterMs, animMs,
                    ball_ ? physics.BallPosition(ball_).y : 0.0f);
        }
    }

private:
    enum class Mode { Wait, Walk, Run };
    static constexpr const char* kDefault =
        "hour 16;yaw 90;pitch 15;wait 0.6;shot 01_idle;ball 3;walk 1.5;shot 02_walk;run 1.5;shot 03_run;wait 0.4;yaw 180;walk 8;shot 04_rail;"
        "wait 0.5;jump;wait 0.3;shot 05_jump;wait 1.2;shot 06_landed;teleport -91.4 3;wait 0.5;walk 7;shot 07_livery;teleport -22 12;yaw 90;wait 0.5;walk 8;shot 08_wall;"
        "yaw 90;pitch 12;teleport 3320.6 -644.7;wait 0.5;shot 09_shore;walk 9;shot 10_wading;run 3;shot 11_wake;wait 1;shot 12_after";

    void Begin(const std::string& command, PlayerCharacter& player, PhysicsWorld& physics, OrbitCamera& camera) {
        char name[64] = {};
        float a = 0.0f, b = 0.0f;
        const int n = sscanf(command.c_str(), "%63s %f %f", name, &a, &b);
        const std::string op = name;
        label_ = command;
        for (char& ch : label_) if (ch == ' ') ch = '_';
        if (op == "wait") { mode_ = Mode::Wait; remaining_ = a; }
        else if (op == "walk") { mode_ = Mode::Walk; remaining_ = a; }
        else if (op == "run") { mode_ = Mode::Run; remaining_ = a; }
        else if (op == "jump") jump_ = true;
        else if (op == "yaw") camera.yaw = a * 3.14159265f / 180.0f;
        else if (op == "pitch") camera.pitch = a * 3.14159265f / 180.0f;
        else if (op == "hour") hour_ = a;
        else if (op == "view") player.ToggleView();
        else if (op == "ragdoll") player.Request(1);
        else if (op == "blast") player.Request(2);
        else if (op == "knockout") player.Request(3);
        else if (op == "dislocate") player.Request(4);
        else if (op == "fracture") player.Request(5);
        else if (op == "punch") player.Request(6);
        else if (op == "teleport" && n == 3) { player.Teleport({a, player.State().feet.y, b}); mode_ = Mode::Wait; }
        else if (op == "ball") {
            ball_ = physics.AddBall(player.State().feet + Vec3{1.0f, a, 0.0f}, 0.25f);
        } else if (op == "shot") {
            char rest[64] = {};
            sscanf(command.c_str(), "%*s %63s", rest);
            shot_ = rest;
        } else Log("Playtest: unknown command %s", command.c_str());
    }

    bool active_ = false;
    std::vector<std::string> commands_;
    size_t next_ = 0;
    float remaining_ = 0.0f, clock_ = 0.0f, hour_ = 16.0f;
    Mode mode_ = Mode::Wait;
    bool jump_ = false;
    std::string shot_, dir_, api_, label_ = "start";
    FILE* trace_ = nullptr;
    int frame_ = 0;
    uint32_t ball_ = 0;
};

} // namespace tbd
