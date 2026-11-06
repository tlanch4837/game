// Single-face tracker: BlazeFace detector + 468-point face mesh (MediaPipe models run through ONNX Runtime),
// then head pose and expression measures from the mesh.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "face_clip.h"

namespace tbd {

// One-Euro smoothing of a live frame stream (see FaceTracker::SmoothParams).
class FaceSmoother {
public:
    struct SmoothParams {
        bool enabled = true;
        float minCutoff = 1.0f;
        float beta = 3.0f;
    };
    // `scales`: the typical range of every signal, in the order pos(3), rot(3), raw channels, mesh.
    FaceSmoother(std::vector<float> scales, const SmoothParams& params = {});
    static std::vector<float> DefaultScales();
    void Reset(); // call when the face was lost
    void Apply(FaceFrame& f, float dt);

private:
    struct OneEuro {
        bool primed = false;
        float x = 0, dx = 0;
        float Step(float v, float dt, float minCutoff, float beta);
    };
    std::vector<float> scale_;
    SmoothParams params_;
    std::vector<OneEuro> filters_;
};

class FaceTracker {
public:
    FaceTracker();
    ~FaceTracker();
    FaceTracker(const FaceTracker&) = delete;
    FaceTracker& operator=(const FaceTracker&) = delete;

    // `modelDir` holds face_detector.onnx and face_landmark_detector.onnx (with their .data files).
    bool Init(const wchar_t* modelDir, std::string* error);

    // Forgets the tracked face; the next frame runs the detector. Call between clips.
    void Reset();

    // `bgra` is width*height*4, top-down. Fills `out` (out.valid is false when no face is found).
    // Frames must be fed in order: the previous mesh seeds the next search region.
    void Process(const uint8_t* bgra, int width, int height, FaceFrame& out);

    // One-Euro smoothing of pose, raw channels and mesh, in place. Run it before NormalizeChannels.
    // Signals are filtered in units of their own range, so one pair of numbers fits all of them:
    // minCutoff (Hz) is the smoothing at rest (lower = steadier), beta lets fast motion through (higher = less lag).
    using SmoothParams = FaceSmoother::SmoothParams;
    static void Smooth(std::vector<FaceFrame>& frames, const SmoothParams& params = {});

    // Fills FaceFrame::weight for every valid frame from the clip's raw measures.
    static void NormalizeChannels(std::vector<FaceFrame>& frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tbd
