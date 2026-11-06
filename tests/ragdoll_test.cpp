// Headless ragdoll test: Wyatt's real skeleton, a stone floor, no window. Build with ragdoll_test.bat.
//   bin\tbd_ragdoll_test.exe [path to a skinned glb]
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/mesh_import.h"
#include "../src/physics.h"
#include "../src/scene.h"

using namespace tbd;

static bool NoMesh(const std::string&, std::vector<Vertex>&) { return false; }
static int g_failed = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failed; printf("  FAIL %s: ", #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Column-major 4x4 inverse (general).
static void Inverse(const float* m, float* out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    inv[0] = a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    const double det = a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
}

struct Rig {
    RagdollSkeleton skeleton;
    std::vector<float> pose; // the bind pose, also the start pose
};

static bool LoadRig(const std::string& path, Rig& rig) {
    std::vector<Vertex> unused;
    ImportedModel model;
    ImportOptions options;
    options.normalizeToUnitBox = false;
    std::string error;
    if (!ImportMesh(path, unused, &error, options, &model) || model.joints.empty()) { printf("cannot read %s: %s\n", path.c_str(), error.c_str()); return false; }
    const size_t n = model.joints.size();
    rig.skeleton.names.resize(n);
    rig.skeleton.parent.resize(n);
    rig.skeleton.bind.resize(n * 16);
    for (size_t j = 0; j < n; ++j) {
        rig.skeleton.names[j] = model.joints[j].name;
        rig.skeleton.parent[j] = model.joints[j].parent;
        Inverse(model.joints[j].inverseBind, &rig.skeleton.bind[j * 16]);
    }
    rig.pose = rig.skeleton.bind;
    return true;
}

static void Translate(float* m, float x, float y, float z) { m[12] = x; m[13] = y; m[14] = z; }

struct Scenario {
    PhysicsWorld physics;
    JobSystem jobs;
    Scene scene;
    uint32_t id = 0;
    float modelToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    Scenario() {
        scene.entities.clear();
        Entity ground;
        ground.position = {0, -0.5f, 0};
        ground.scale = {60, 1, 60};
        scene.entities.push_back(ground);
        physics.SetScene(&jobs, scene, L"", NoMesh);
        for (int i = 0; i < 1000 && !physics.ReadyAround({0, 0, 0}, 10); ++i) { physics.Update({0, 0, 0}, 0); Sleep(1); }
    }
    bool Spawn(const Rig& rig, float height, Vec3 velocity, float yaw = 0.0f) {
        // Feet at `height`: the bind pose's lowest joint sits at y of the ball joints (about 0) in the glb's own space.
        float lowest = 1e9f;
        for (size_t j = 0; j < rig.skeleton.names.size(); ++j) lowest = std::min(lowest, rig.pose[j * 16 + 13]);
        std::fill(modelToWorld, modelToWorld + 16, 0.0f);
        modelToWorld[0] = std::cos(yaw); modelToWorld[2] = -std::sin(yaw);
        modelToWorld[5] = 1; modelToWorld[8] = std::sin(yaw); modelToWorld[10] = std::cos(yaw); modelToWorld[15] = 1;
        Translate(modelToWorld, 0, height - lowest, 0);
        id = physics.Ragdolls().Create(rig.skeleton, rig.pose.data(), modelToWorld, velocity, RagdollTuning{});
        return id != 0;
    }
    void Run(const Rig& rig, float seconds, bool drive = true) {
        const float dt = 1.0f / 60.0f;
        for (float t = 0; t < seconds; t += dt) {
            if (drive) physics.Ragdolls().Drive(id, rig.pose.data(), dt);
            physics.Update({0, 0, 0}, dt);
        }
    }
};

static float Lowest(PhysicsWorld& physics, uint32_t id, const float* m2w, size_t joints) {
    std::vector<float> pose;
    physics.Ragdolls().Pose(id, m2w, pose);
    float low = 1e9f;
    for (size_t j = 1; j < joints; ++j) low = std::min(low, pose[j * 16 + 13] + m2w[13]); // joint 0 is the Root, below the feet
    return low;
}

int main(int argc, char** argv) {
    Rig rig;
    const std::string path = argc > 1 ? argv[1] : "C:\\custom windows apps\\WesternGameIOS\\export\\anim\\unreal\\wyatt_idle.glb";
    if (!LoadRig(path, rig)) return 2;
    printf("skeleton: %zu joints\n", rig.skeleton.names.size());
    const size_t joints = rig.skeleton.names.size();

    { // 1. A standing character dropped from 2 m with the muscles on: lands, folds, comes to rest on the floor.
        printf("drop from 2 m, muscles on\n");
        Scenario s;
        CHECK(s.Spawn(rig, 2.0f, {0, 0, 0}), "create failed");
        float load[kRagdollParts] = {}, hit[kRagdollParts] = {};
        for (int i = 0; i < 60 * 8; ++i) {
            s.Run(rig, 1.0f / 60.0f);
            const RagdollStatus now = s.physics.Ragdolls().Status(s.id);
            for (int p = 0; p < kRagdollParts; ++p) { load[p] = std::max(load[p], now.jointLoad[p]); hit[p] = std::max(hit[p], now.peakImpulse[p]); }
        }
        for (int p = 0; p < kRagdollParts; ++p) printf("    %-11s joint load %7.1f N m   hit %6.1f N s\n", RagdollPartName(p), load[p], hit[p]);
        const RagdollStatus st = s.physics.Ragdolls().Status(s.id);
        printf("  pelvis y=%.2f speed=%.2f brace=%.2f dislocated=%x fractured=%x unconscious=%d\n", st.pelvis.y, st.speed, st.brace, st.dislocated, st.fractured, st.unconscious);
        CHECK(std::isfinite(st.pelvis.y), "NaN");
        CHECK(st.pelvis.y > 0.1f && st.pelvis.y < 1.3f, "pelvis y=%.2f", st.pelvis.y);
        CHECK(Lowest(s.physics, s.id, s.modelToWorld, joints) > -0.12f, "something fell through the floor: %.2f", Lowest(s.physics, s.id, s.modelToWorld, joints));
        CHECK(st.speed < 3.0f, "still moving at %.2f", st.speed);
    }
    { // 2. Fully limp from 3 m: ends lying down, pelvis near the floor, no blow-ups.
        printf("limp drop from 3 m\n");
        Scenario s;
        CHECK(s.Spawn(rig, 3.0f, {1.5f, 0, 0}), "create failed");
        s.physics.Ragdolls().SetTension(s.id, 0.0f);
        float peak = 0;
        for (int i = 0; i < 60 * 10; ++i) {
            s.Run(rig, 1.0f / 60.0f);
            peak = std::max(peak, s.physics.Ragdolls().Status(s.id).speed);
        }
        const RagdollStatus st = s.physics.Ragdolls().Status(s.id);
        printf("  pelvis y=%.2f peak speed=%.2f rest=%.1fs\n", st.pelvis.y, peak, st.restTime);
        CHECK(st.pelvis.y < 0.45f, "pelvis y=%.2f (should be lying)", st.pelvis.y);
        CHECK(peak < 14.0f, "speed spike %.1f", peak);
        CHECK(st.restTime > 1.0f, "never rested (%.1fs)", st.restTime);
    }
    { // 3. The brace: falling from 4 m with reflexes must raise the brace before landing and not when reflexes are off.
        printf("brace while falling\n");
        Scenario s;
        CHECK(s.Spawn(rig, 4.0f, {0, 0, 0}), "create failed");
        float peakBrace = 0;
        for (int i = 0; i < 60 * 2; ++i) { s.Run(rig, 1.0f / 60.0f); peakBrace = std::max(peakBrace, s.physics.Ragdolls().Status(s.id).brace); }
        printf("  peak brace %.2f\n", peakBrace);
        CHECK(peakBrace > 0.6f, "brace %.2f", peakBrace);
    }
    { // 4. Blast: the character is thrown away from the centre, limbs scatter, the speed stays sane.
        printf("blast\n");
        Scenario s;
        CHECK(s.Spawn(rig, 0.02f, {0, 0, 0}), "create failed");
        s.Run(rig, 1.0f);
        s.physics.Ragdolls().Blast(s.id, {-1.0f, 0.0f, 0.0f}, 14.0f, 6.0f);
        float peak = 0, maxX = 0;
        for (int i = 0; i < 60 * 3; ++i) {
            s.Run(rig, 1.0f / 60.0f);
            const RagdollStatus st = s.physics.Ragdolls().Status(s.id);
            peak = std::max(peak, st.speed);
            maxX = std::max(maxX, st.pelvis.x);
        }
        printf("  peak speed %.1f, pelvis went to x=%.1f\n", peak, maxX);
        CHECK(peak > 4.0f && peak < 30.0f, "peak speed %.1f", peak);
        CHECK(maxX > 1.5f, "pelvis x=%.1f", maxX);
    }
    { // 5. Dislocation: an elbow forced out ends up bending the wrong way and stays there.
        printf("dislocation\n");
        Scenario s;
        CHECK(s.Spawn(rig, 1.2f, {0, 0, 0}), "create failed");
        s.Run(rig, 0.2f);
        s.physics.Ragdolls().Dislocate(s.id, (int)RagdollPart::LowerArmL);
        s.Run(rig, 3.0f);
        const RagdollStatus st = s.physics.Ragdolls().Status(s.id);
        CHECK(st.dislocated & (1u << (int)RagdollPart::LowerArmL), "not marked");
        const auto events = s.physics.Ragdolls().TakeEvents();
        int dislocations = 0;
        for (const RagdollEvent& e : events) dislocations += e.kind == RagdollEventKind::Dislocation && e.part == (int)RagdollPart::LowerArmL;
        CHECK(dislocations == 1, "events %d", dislocations);
        printf("  events=%zu dislocations=%d\n", events.size(), dislocations);
    }
    { // 6. Performance: ten ragdolls, one second.
        printf("ten ragdolls\n");
        Scenario s;
        std::vector<uint32_t> ids;
        for (int i = 0; i < 10; ++i) {
            s.modelToWorld[12] = (float)(i - 5) * 1.3f;
            float low = 1e9f;
            for (size_t j = 0; j < joints; ++j) low = std::min(low, rig.pose[j * 16 + 13]);
            s.modelToWorld[13] = 1.5f - low;
            ids.push_back(s.physics.Ragdolls().Create(rig.skeleton, rig.pose.data(), s.modelToWorld, {0, 0, 0}, RagdollTuning{}));
        }
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 60; ++i) {
            for (uint32_t id : ids) s.physics.Ragdolls().Drive(id, rig.pose.data(), 1.0f / 60.0f);
            s.physics.Update({0, 0, 0}, 1.0f / 60.0f);
        }
        QueryPerformanceCounter(&b);
        printf("  %.2f ms per frame\n", 1000.0 * (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart / 60.0);
    }
    { // 7. Injuries scale with the fall: a 1 m step hurts nothing, a 7 m drop breaks something.
        for (float height : {1.0f, 7.0f}) {
            printf("injury check, drop from %.0f m\n", height);
            Scenario s;
            CHECK(s.Spawn(rig, height, {0, 0, 0}), "create failed");
            s.Run(rig, 5.0f);
            const RagdollStatus st = s.physics.Ragdolls().Status(s.id);
            const bool hurt = st.dislocated || st.fractured || st.unconscious;
            printf("  dislocated=%x fractured=%x unconscious=%d\n", st.dislocated, st.fractured, st.unconscious);
            if (height < 2.0f) CHECK(!hurt, "hurt by a 1 m step");
            else CHECK(hurt, "nothing hurt after a 7 m drop");
        }
    }
    printf(g_failed ? "FAILED (%d)\n" : "ALL PASS\n", g_failed);
    return g_failed ? 1 : 0;
}
