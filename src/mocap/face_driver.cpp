#include "face_driver.h"

#include <algorithm>
#include <cmath>

#include "../project.h"

namespace tbd {

Quat FaceHeadRotation(const float pose[3], const float neutral[3], bool mirror) {
    const float pitch = pose[0] - neutral[0], yaw = pose[1] - neutral[1], roll = pose[2] - neutral[2];
    const float m = mirror ? -1.0f : 1.0f;
    return QuatFromEuler({pitch, yaw * m, -roll * m});
}

FaceDriver::Cached* FaceDriver::Get(const std::string& pathUtf8) {
    auto it = cache_.find(pathUtf8);
    if (it != cache_.end()) return &it->second;
    Cached c;
    if (c.clip.Load(Utf8ToWide(pathUtf8).c_str())) {
        for (int k = 0; k < 3; ++k) { // neutral = median of each angle over the frames with a face
            std::vector<float> v;
            for (const FaceFrame& f : c.clip.frames)
                if (f.valid) v.push_back(f.rot[k]);
            if (v.empty()) continue;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            c.neutral[k] = v[v.size() / 2];
        }
        c.ok = !c.clip.frames.empty() && c.clip.frames.back().time > 0;
    }
    return &(cache_[pathUtf8] = std::move(c)); // failures are cached too, so a missing file is not retried every frame
}

// Pose at time `t`: interpolated between the two frames around it, held at the last frame that had a face.
bool FaceDriver::Sample(const Cached& c, double t, float pose[3]) {
    const auto& fr = c.clip.frames;
    auto it = std::upper_bound(fr.begin(), fr.end(), t, [](double v, const FaceFrame& f) { return v < f.time; });
    int i = std::max((int)(it - fr.begin()) - 1, 0);
    while (i > 0 && !fr[i].valid) --i;
    if (!fr[i].valid) return false;
    const FaceFrame* b = i + 1 < (int)fr.size() && fr[i + 1].valid ? &fr[i + 1] : nullptr;
    const float u = b ? (float)std::clamp((t - fr[i].time) / std::max(b->time - fr[i].time, 1e-6), 0.0, 1.0) : 0.0f;
    for (int k = 0; k < 3; ++k) pose[k] = b ? fr[i].rot[k] + (b->rot[k] - fr[i].rot[k]) * u : fr[i].rot[k];
    return true;
}

bool FaceDriver::Bind(Entity& e, const std::wstring& clipPath, std::string* error) {
    const std::string path = WideToUtf8(clipPath);
    cache_.erase(path); // pick up a re-tracked clip
    const Cached* c = Get(path);
    if (!c->ok) {
        if (error) *error = "CANNOT READ FACE CLIP";
        return false;
    }
    if (!e.faceClip.empty()) Unbind(e);
    e.faceClip = path;
    e.faceBase = e.rotation;
    return true;
}

void FaceDriver::BindLive(Entity& e) {
    if (!e.faceClip.empty()) Unbind(e);
    e.faceClip = kLiveFaceSource;
    e.faceBase = e.rotation;
}

void FaceDriver::StopLive(Scene& scene) {
    live.Stop();
    for (Entity& e : scene.entities)
        if (e.faceClip == kLiveFaceSource) Unbind(e);
}

void FaceDriver::Unbind(Entity& e) {
    if (e.faceClip.empty()) return;
    e.rotation = e.faceBase;
    e.faceClip.clear();
}

void FaceDriver::Update(float dt, Scene& scene) {
    time_ += dt;
    for (Entity& e : scene.entities) {
        if (e.faceClip.empty()) continue;
        if (e.faceClip == kLiveFaceSource) { // follow the webcam: hold the last pose while no face is tracked
            float pose[3], neutral[3];
            if (poseOverride)
                e.rotation = Normalize(e.faceBase * FaceHeadRotation(overridePose, overrideNeutral, mirror));
            else if (live.Running() && live.Pose(pose, neutral))
                e.rotation = Normalize(e.faceBase * FaceHeadRotation(pose, neutral, mirror));
            continue;
        }
        const Cached* c = Get(e.faceClip);
        if (!c->ok) continue;
        const double dur = c->clip.frames.back().time;
        float pose[3];
        if (Sample(*c, std::fmod(time_, dur), pose)) e.rotation = Normalize(e.faceBase * FaceHeadRotation(pose, c->neutral, mirror));
    }
}

} // namespace tbd
