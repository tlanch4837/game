// Plays face mocap clips on scene entities: the clip's head rotation turns the entity.
// (The clip's expression channels have no consumer yet: the engine's meshes are rigid, with no morph targets or bones.)
#pragma once
#include <map>
#include <string>

#include "../scene.h"
#include "face_clip.h"
#include "face_live.h"

namespace tbd {

// Head pose -> entity rotation. `pose` and `neutral` are {pitch, yaw, roll} in degrees as stored in a clip;
// `neutral` is the subject's resting pose, so a relaxed head maps to no rotation. The entity's front is +Z.
// pitch>0 (looking up) tips the front up, yaw>0 (head turned to its own left) turns it toward +X, roll>0
// (clockwise on screen) rolls it clockwise seen from the front. `mirror` flips yaw and roll, like a mirror.
Quat FaceHeadRotation(const float pose[3], const float neutral[3], bool mirror);

// Entity::faceClip value meaning "driven by the live webcam" instead of a file.
inline const char* const kLiveFaceSource = "@webcam";

class FaceDriver {
public:
    bool mirror = true; // like a mirror: turn your head left and the character turns toward its right
    LiveFace live; // the webcam capture that entities bound to kLiveFaceSource follow
    // While on, entities bound to kLiveFaceSource follow this pose instead of the webcam (a clip playing in the face tab).
    bool poseOverride = false;
    float overridePose[3] = {}, overrideNeutral[3] = {};

    // Loads `clipPath` and starts driving `e` from the next Update. Returns false and sets `error` on failure.
    bool Bind(Entity& e, const std::wstring& clipPath, std::string* error);

    // Drives `e` from the live webcam (see `live`). Start `live` separately.
    void BindLive(Entity& e);

    // Stops the webcam and lets go of every entity it was driving.
    void StopLive(Scene& scene);

    // Stops driving `e` and puts its rotation back.
    void Unbind(Entity& e);

    // Advances the shared clock by `dt` seconds and poses every driven entity (clips loop).
    void Update(float dt, Scene& scene);

private:
    struct Cached {
        FaceClip clip;
        float neutral[3] = {};
        bool ok = false;
    };
    Cached* Get(const std::string& pathUtf8);
    static bool Sample(const Cached& c, double t, float pose[3]);

    std::map<std::string, Cached> cache_;
    double time_ = 0;
};

} // namespace tbd
