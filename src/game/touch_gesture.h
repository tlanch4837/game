#pragma once
// One finger on the city: a tap or a drag (src/platform/ios/main_ios.mm uses it; headless, tests\interact_test.cpp).
// A finger that travels more than kTapSlopPt from where it landed is a drag (the camera pans) from then on, even if it
// comes back; one that lifts within kTapMaxSeconds without having travelled is a tap (it selects / places); one held
// longer without moving is a hold, which does nothing (a slow, unsure press never builds).
#include <cmath>

namespace tbd {
namespace touch {

constexpr float kTapSlopPt = 10.0f;
constexpr double kTapMaxSeconds = 0.45;

enum class Gesture { Undecided, Tap, Drag, Hold };

// travelPt: the farthest the finger got from where it landed, in points; heldSec: how long it has been down; lifted: it is up.
inline Gesture Classify(float travelPt, double heldSec, bool lifted, float slopPt = kTapSlopPt, double maxSec = kTapMaxSeconds) {
    if (travelPt > slopPt) return Gesture::Drag;
    if (!lifted) return heldSec > maxSec ? Gesture::Hold : Gesture::Undecided;
    return heldSec <= maxSec ? Gesture::Tap : Gesture::Hold;
}

// Device pixels to points for the travel.
inline float TravelPt(float dxPx, float dyPx, float pixelsPerPoint) {
    const float s = pixelsPerPoint > 0.0f ? pixelsPerPoint : 1.0f;
    const float dx = dxPx / s, dy = dyPx / s;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace touch
} // namespace tbd
