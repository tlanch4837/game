#pragma once
// Guided resting-face calibration. Header-only, no tracker/threading/Windows dependency.
// Feed it tracked frames; it only accepts a run of consecutive still, relaxed frames, and restarts the
// run (with a hint for the user) whenever the head moves or the face is not relaxed. Not thread-safe:
// call under the owner's lock.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "face_clip.h"

namespace tbd {

constexpr int kCalibWindowFrames = 45;    // consecutive good frames that finish calibration (~1.5 s)
constexpr int kCalibFallbackFrames = 300; // after this many frames, settle for the best window seen...
constexpr int kCalibMinFallback = 15;     // ...as long as it has at least this many frames
constexpr float kCalibMaxHeadDeg = 4.0f;  // each head angle must stay within this of the window's median
constexpr float kCalibMaxRelaxed = 0.35f; // jawOpen, eyeBlink and mouthSmile must each stay below this

class FaceCalibrator {
public:
    FaceCalibrator() { Reset(); }

    void Reset() {
        window_.clear();
        best_.clear();
        frames_ = 0;
        done_ = false;
        hint_ = nullptr;
        for (float& r : neutralRot_) r = 0.0f;
        for (float& b : neutralBs_) b = 0.0f;
    }

    // Returns true once calibration has finished (and on every call after, until Reset()).
    bool Add(const float rot[3], const float bs[kBlendshapes]) {
        if (done_) return true;
        ++frames_;

        const char* problem = nullptr;
        if (!Relaxed(bs))
            problem = "Relax your face";
        else if (!Still(rot))
            problem = "Hold your head still";

        if (problem) { // restart the window, remembering it if it was the longest so far
            if (window_.size() > best_.size()) best_ = window_;
            window_.clear();
            hint_ = problem;
        } else {
            hint_ = nullptr;
            Sample s;
            std::copy(rot, rot + 3, s.rot.begin());
            std::copy(bs, bs + kBlendshapes, s.bs.begin());
            window_.push_back(s);
            if ((int)window_.size() >= kCalibWindowFrames) return Finish(window_);
        }

        if (frames_ >= kCalibFallbackFrames) {
            const std::vector<Sample>& pick = window_.size() > best_.size() ? window_ : best_;
            if ((int)pick.size() >= kCalibMinFallback) return Finish(pick);
        }
        return false;
    }

    float Progress() const {
        return done_ ? 1.0f : std::min(1.0f, (float)window_.size() / (float)kCalibWindowFrames);
    }

    // Null while all is well; otherwise a short instruction for the user.
    const char* Hint() const { return done_ ? nullptr : hint_; }

    // Valid once Add() has returned true.
    const float* NeutralRot() const { return neutralRot_; }
    const float* BaseBlendshapes() const { return neutralBs_; }

private:
    struct Sample {
        std::array<float, 3> rot;
        std::array<float, kBlendshapes> bs;
    };

    static float Median(std::vector<float>& v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }

    static int Index(const char* name) {
        for (int k = 0; k < kBlendshapes; ++k)
            if (std::strcmp(kBlendshapeNames[k], name) == 0) return k;
        return -1;
    }

    static bool Relaxed(const float* bs) {
        static const int idx[] = {Index("jawOpen"),         Index("eyeBlinkLeft"),   Index("eyeBlinkRight"),
                                  Index("mouthSmileLeft"), Index("mouthSmileRight")};
        for (int k : idx)
            if (k >= 0 && bs[k] >= kCalibMaxRelaxed) return false;
        return true;
    }

    // Each angle within kCalibMaxHeadDeg of the running median of the current window.
    bool Still(const float* rot) const {
        if (window_.empty()) return true;
        for (int a = 0; a < 3; ++a) {
            std::vector<float> v;
            v.reserve(window_.size());
            for (const Sample& s : window_) v.push_back(s.rot[a]);
            if (std::fabs(rot[a] - Median(v)) > kCalibMaxHeadDeg) return false;
        }
        return true;
    }

    bool Finish(const std::vector<Sample>& w) {
        for (int a = 0; a < 3; ++a) {
            std::vector<float> v;
            for (const Sample& s : w) v.push_back(s.rot[a]);
            neutralRot_[a] = Median(v);
        }
        for (int k = 0; k < kBlendshapes; ++k) {
            std::vector<float> v;
            for (const Sample& s : w) v.push_back(s.bs[k]);
            neutralBs_[k] = Median(v);
        }
        window_.clear();
        best_.clear();
        done_ = true;
        hint_ = nullptr;
        return true;
    }

    std::vector<Sample> window_, best_;
    int frames_ = 0;
    float neutralRot_[3] = {};
    float neutralBs_[kBlendshapes] = {};
    bool done_ = false;
    const char* hint_ = nullptr;
};

} // namespace tbd
