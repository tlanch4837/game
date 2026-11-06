// Plays a saved clip's expressions: looped sampling of a FaceClip, with the same resting-face correction the live
// capture applies. A clip has no calibration moment, so the resting face is estimated from the clip itself.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "face_clip.h"

namespace tbd {

class FaceClipPlayer {
public:
    bool Load(const wchar_t* path) {
        Clear();
        FaceClip c;
        if (!c.Load(path)) return false;
        std::vector<float> v;
        auto percentile = [&](auto get, float p) {
            v.clear();
            for (const FaceFrame& f : c.frames)
                if (f.valid) v.push_back(get(f));
            if (v.empty()) return 0.0f;
            std::sort(v.begin(), v.end());
            return v[(size_t)(p * (v.size() - 1))];
        };
        for (int k = 0; k < 3; ++k) neutral_[k] = percentile([k](const FaceFrame& f) { return f.rot[k]; }, 0.5f);
        // A low percentile, not the median as live uses: a clip is mostly performance, and a channel that is active
        // half the time would otherwise read as rest and vanish.
        for (int k = 0; k < kBlendshapes; ++k) base_[k] = percentile([k](const FaceFrame& f) { return f.bs[k]; }, 0.2f);
        bool any = false;
        for (const FaceFrame& f : c.frames) any |= f.valid;
        if (!any || c.frames.size() < 2 || c.frames.back().time <= c.frames.front().time) return false;
        frames_ = std::move(c.frames);
        return true;
    }
    void Clear() { frames_.clear(); }
    bool Loaded() const { return !frames_.empty(); }
    double Duration() const { return frames_.empty() ? 0 : frames_.back().time - frames_.front().time; }
    const float* Neutral() const { return neutral_; }
    // Clip time at `t` seconds of looped playback.
    double LocalTime(double t) const { return frames_.empty() ? 0 : frames_.front().time + std::fmod(std::max(t, 0.0), Duration()); }

    // The frame at `t` seconds (looping), interpolated, weights relative to the resting face. Where the clip lost the
    // face, the last tracked frame holds. False only before the first tracked frame.
    bool Sample(double t, FaceFrame& out) const {
        if (frames_.empty()) return false;
        const double local = frames_.front().time + std::fmod(std::max(t, 0.0), Duration());
        auto it = std::upper_bound(frames_.begin(), frames_.end(), local, [](double v, const FaceFrame& f) { return v < f.time; });
        int i = std::max((int)(it - frames_.begin()) - 1, 0);
        while (i > 0 && !frames_[i].valid) --i;
        if (!frames_[i].valid) return false;
        out = frames_[i];
        if (i + 1 < (int)frames_.size() && frames_[i + 1].valid) {
            const FaceFrame& b = frames_[i + 1];
            const float u = (float)std::clamp((local - out.time) / std::max(b.time - out.time, 1e-6), 0.0, 1.0);
            auto mix = [u](float* a, const float* c, int n) { for (int k = 0; k < n; ++k) a[k] += (c[k] - a[k]) * u; };
            mix(out.rot, b.rot, 3);
            mix(out.bs, b.bs, kBlendshapes);
            mix(out.weight, b.weight, kFaceChannels);
            mix(out.lm, b.lm, kFaceLandmarks * 3);
        }
        for (int k = 0; k < kBlendshapes; ++k)
            out.bs[k] = std::clamp((out.bs[k] - base_[k]) / std::max(1.0f - base_[k], 0.5f), 0.0f, 1.0f);
        return true;
    }

private:
    std::vector<FaceFrame> frames_;
    float neutral_[3] = {}, base_[kBlendshapes] = {};
};

} // namespace tbd
