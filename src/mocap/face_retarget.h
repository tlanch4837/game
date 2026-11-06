// From what the tracker says to what a character should do. The tracker's 52 weights are honest about the camera's view
// of a face, not about intent, so one thing here keeps state between frames:
//  - Blink: looking down reads as half-closed eyes (correlation 0.90 on a real take), for seconds at a time. A real blink is
//    fast and a glance is slow, so a slow baseline of each eye's reading is taken off. Eyes held firmly shut stay shut.
//    Trackers rarely report a blink past 0.8, so the rest of the range is stretched to close the lids fully.
//    Eyes closing together are drawn together; a wink stays a wink.
//  - Mouth pairs: the tracker's left and right disagree for a mouth that is symmetric (lowerDown 0.22 / 0.56 in a plain
//    smile). They are drawn halfway together; a one-sided expression still reads as one-sided.
#pragma once
#include <algorithm>
#include <cmath>

#include "face_clip.h"

namespace tbd {

class FaceRetarget {
public:
    void Reset() { primed_ = false; }

    // `dt`: seconds since the previous frame of this stream; 0 when unknown.
    void Apply(float bs[kBlendshapes], float dt) {
        static constexpr int kPairs[][2] = {{BS_mouthSmileLeft, BS_mouthSmileRight}, {BS_mouthUpperUpLeft, BS_mouthUpperUpRight},
                                            {BS_mouthLowerDownLeft, BS_mouthLowerDownRight}, {BS_mouthPressLeft, BS_mouthPressRight},
                                            {BS_mouthStretchLeft, BS_mouthStretchRight}, {BS_mouthFrownLeft, BS_mouthFrownRight},
                                            {BS_mouthDimpleLeft, BS_mouthDimpleRight}};
        for (const auto& p : kPairs) {
            const float mid = (bs[p[0]] + bs[p[1]]) * 0.5f;
            bs[p[0]] += (mid - bs[p[0]]) * 0.5f;
            bs[p[1]] += (mid - bs[p[1]]) * 0.5f;
        }
        // The webcam model under-reads a pucker (0.42 at most for clearly pursed lips); the rig is sized for a true full pucker.
        bs[BS_mouthPucker] = std::min(bs[BS_mouthPucker] * 2.0f, 1.0f);
        const int eyes[2] = {BS_eyeBlinkLeft, BS_eyeBlinkRight};
        for (int e = 0; e < 2; ++e) {
            const float b = bs[eyes[e]];
            if (!primed_) base_[e] = b;
            if (b < base_[e]) base_[e] = b;
            else if (b < kHoldShut) base_[e] += (b - base_[e]) * (1.0f - std::exp(-std::max(dt, 0.0f) / kBaseSeconds));
            bs[eyes[e]] = std::clamp((b - base_[e]) / std::max(1.0f - base_[e], 0.4f) / kFullBlink, 0.0f, 1.0f);
        }
        // Two eyes closing together are one blink, however differently the tracker reads them. A wink leaves the other eye near 0.
        const float hi = std::max(bs[eyes[0]], bs[eyes[1]]), lo = std::min(bs[eyes[0]], bs[eyes[1]]);
        if (hi > 0) {
            const float together = std::clamp((lo / hi - 0.15f) / 0.3f, 0.0f, 1.0f) * 0.8f;
            bs[eyes[0]] += (hi - bs[eyes[0]]) * together;
            bs[eyes[1]] += (hi - bs[eyes[1]]) * together;
        }
        primed_ = true;
    }

private:
    // Chosen on saved takes: on the first webcam take the longest false "eye closed" (reading above 0.4) fell from 4.4 and
    // 11.5 s to 0.46 and 0.80 s, with the blinks still reaching full closure.
    static constexpr float kBaseSeconds = 0.7f, kHoldShut = 0.72f, kFullBlink = 0.62f;
    float base_[2] = {};
    bool primed_ = false;
};

} // namespace tbd
