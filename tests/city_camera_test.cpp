// Tests for src/city_camera.h, the city's CS2-style camera: tilt reaches straight down and level (street view), pinch / wheel
// zoom works both ways over street level .. whole map, the floor keeps the eye 2 m above the ground without changing the
// tilt, and the pan drags the ground. Headless: no renderer, no engine.
#include <cmath>
#include <cstdio>

#include "../src/city_camera.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

using tbd::OrbitCamera;
using tbd::Vec3;
namespace cc = tbd::citycam;

static float Flat(float, float) { return 5.0f; }
static float Hills(float x, float z) { return 20.0f + 15.0f * std::sin(x * 0.01f) * std::cos(z * 0.013f); }
static float Ramp(float, float z) { return -z * 0.5f; } // rises toward -z (the view's heading at yaw 0)

static OrbitCamera CityStart() { // NewCityGame's view
    OrbitCamera c;
    c.target = {0.0f, 0.0f, -1000.0f};
    c.distance = 3200.0f;
    c.pitch = 1.1f;
    c.yaw = 0.0f;
    return c;
}

static void TiltReachesStraightDown() {
    OrbitCamera c = CityStart();
    for (int i = 0; i < 200; ++i) cc::Tilt(c, 0.01f); // two fingers dragged down a long way
    CHECK(Near(c.pitch, cc::kMaxPitch, 1e-6f));
    cc::GroundFloor(c, Flat);
    CHECK(c.Forward().y < -0.9998f); // within 1 degree of straight down
    const Vec3 r = c.Right(), u = c.Up();
    CHECK(std::isfinite(r.x) && std::isfinite(r.z) && std::isfinite(u.x) && std::isfinite(u.z));
    CHECK(Near(c.target.y, 5.0f, 1e-3f)); // high up: the pivot sits on the ground
}

static void TiltReachesStreetLevelHorizon() {
    OrbitCamera c = CityStart();
    for (int i = 0; i < 400; ++i) cc::Tilt(c, -0.01f); // fingers up
    CHECK(Near(c.pitch, cc::kMinPitch, 1e-6f));
    c.distance = 0.5f; // pinched all the way in
    cc::GroundFloor(c, Flat);
    CHECK(Near(c.distance, cc::kMinDistance, 1e-6f));
    CHECK(Near(c.pitch, cc::kMinPitch, 1e-6f)); // the floor did not tilt the view up: it lifted the pivot
    const Vec3 eye = c.Eye();
    CHECK(eye.y >= 5.0f + cc::kEyeClearance - 1e-3f);
    CHECK(eye.y <= 5.0f + cc::kEyeClearance + 0.2f); // street level: about 2 m up
    CHECK(c.Forward().y >= -0.01f);                  // level or a touch up: the horizon is on screen
    // a ray through the screen's top edge looks above the horizon, the bottom one at the ground
    const tbd::Ray top = c.ScreenRay(1000.0f, 0.0f, 2000.0f, 1500.0f), bottom = c.ScreenRay(1000.0f, 1500.0f, 2000.0f, 1500.0f);
    CHECK(top.dir.y > 0.0f && bottom.dir.y < 0.0f);
}

static void PinchZoomsBothWays() {
    // from the 3.2 km start view: out works (it used to snap in to 200 m) and in works
    CHECK(cc::ZoomedDistance(3200.0f, -4.0f) > 3200.0f);
    CHECK(cc::ZoomedDistance(3200.0f, 4.0f) < 3200.0f);
    // spreading the fingers by 2 halves the distance; pinching them to half doubles it (main_ios.mm's notches)
    const float spread2 = (float)(-2.0 * std::log(2.0) / std::log(0.9)), pinch2 = -spread2;
    CHECK(Near(cc::ZoomedDistance(100.0f, spread2), 50.0f, 0.01f));
    CHECK(Near(cc::ZoomedDistance(100.0f, pinch2), 200.0f, 0.02f));
    float d = 3200.0f;
    for (int i = 0; i < 100; ++i) d = cc::ZoomedDistance(d, -3.0f);
    CHECK(Near(d, cc::kMaxDistance, 1e-3f)); // the whole map
    CHECK(cc::kMaxDistance * std::tan(0.45f) >= 4000.0f); // ... is in view: +-4 km at fovY 0.9
    for (int i = 0; i < 200; ++i) d = cc::ZoomedDistance(d, 3.0f);
    CHECK(Near(d, cc::kMinDistance, 1e-6f)); // street level
}

static void FloorKeepsEyeAboveGround() {
    OrbitCamera c;
    int below = 0, tiltChanged = 0;
    for (int i = 0; i < 4000; ++i) {
        c.target = {(float)(i % 97) * 37.0f - 1800.0f, 0.0f, (float)(i % 89) * 41.0f - 1800.0f};
        c.yaw = (float)i * 0.37f;
        c.pitch = cc::kMinPitch + (cc::kMaxPitch - cc::kMinPitch) * (float)(i % 13) / 12.0f;
        c.distance = cc::kMinDistance * std::pow(cc::kMaxDistance / cc::kMinDistance, (float)(i % 17) / 16.0f);
        const float pitch = c.pitch;
        cc::GroundFloor(c, Hills);
        const Vec3 eye = c.Eye();
        if (eye.y < Hills(eye.x, eye.z) + cc::kEyeClearance - 1e-2f) ++below;
        if (c.pitch != pitch) ++tiltChanged;
        if (c.target.y < Hills(c.target.x, c.target.z) - 1e-3f) ++below; // the pivot is never under the ground either
    }
    CHECK(below == 0);
    CHECK(tiltChanged == 0);
    // ground rising ahead: the point 3 m ahead counts too
    c = OrbitCamera();
    c.target = {0.0f, 0.0f, 0.0f};
    c.yaw = 0.0f;
    c.pitch = 0.0f;
    c.distance = 1.0f;
    cc::GroundFloor(c, Ramp);
    const Vec3 eye = c.Eye();
    CHECK(eye.y >= Ramp(0.0f, eye.z - 3.0f) + cc::kEyeClearance - 1e-3f);
}

static Vec3 GroundUnder(const OrbitCamera& c, float px, float py, float groundY) {
    const tbd::Ray r = c.ScreenRay(px, py, 2000.0f, 1500.0f);
    return r.origin + r.dir * ((groundY - r.origin.y) / r.dir.y);
}

static void PanDragsTheGround() {
    OrbitCamera c = CityStart();
    cc::Tilt(c, 1.0f);
    cc::GroundFloor(c, Flat);
    const Vec3 before = GroundUnder(c, 900.0f, 700.0f, 5.0f);
    const float y = c.target.y;
    cc::PanGround(c, 900.0f, 700.0f, 1100.0f, 600.0f, 2000.0f, 1500.0f, 5.0f);
    const Vec3 after = GroundUnder(c, 1100.0f, 600.0f, 5.0f);
    CHECK(Near(before.x, after.x, 0.05f) && Near(before.z, after.z, 0.05f)); // the same ground point is under the finger
    CHECK(c.target.y == y);                                                  // the camera slid over the ground, no climb

    // street level, a finger dragged up across the horizon: the camera moves over the ground, never up into the sky
    c.pitch = 0.0f;
    c.distance = 1.0f;
    cc::GroundFloor(c, Flat);
    const float streetY = c.target.y;
    const Vec3 t0 = c.target;
    cc::PanGround(c, 1000.0f, 900.0f, 1000.0f, 100.0f, 2000.0f, 1500.0f, 5.0f);
    CHECK(c.target.y == streetY);
    CHECK(std::fabs(c.target.z - t0.z) > 0.01f || std::fabs(c.target.x - t0.x) > 0.01f);
    const float dz = c.target.z - t0.z;
    CHECK(std::isfinite(dz) && std::fabs(dz) < 100.0f); // bounded, even for rays that miss the ground
}

int main() {
    TiltReachesStraightDown();
    TiltReachesStreetLevelHorizon();
    PinchZoomsBothWays();
    FloorKeepsEyeAboveGround();
    PanDragsTheGround();
    std::printf("city_camera_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
