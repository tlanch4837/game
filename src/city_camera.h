// The city camera (CS2 style) on top of OrbitCamera: tilt from the horizon to straight down, distance from street level to the
// whole 8 km map, a pan that drags the ground under the finger, and a floor that keeps the eye above the terrain or the sea by
// lifting the pivot (never by changing the player's tilt, so street level can look at the horizon). Plain math, no engine
// state: game_core.inl calls it with the terrain's height, tests/city_camera_test.cpp runs it headless.
#pragma once
#include <algorithm>
#include <cmath>

#include "camera.h"

namespace tbd::citycam {

constexpr float kMinPitch = -0.10f;     // a little above level: the horizon across the screen, towers' tops in view (street view)
constexpr float kMaxPitch = 1.5533f;    // 89 degrees: straight down (90 exactly would make LookAt's world up degenerate)
constexpr float kMinDistance = 1.0f;    // eye to pivot at street level
constexpr float kMaxDistance = 9000.0f; // the whole +-4 km map in view from above (fovY 0.9: 4000 / tan(0.45) ~ 8300)
constexpr float kEyeClearance = 2.0f;   // the eye never comes lower than this over the ground or the sea: a person's eye height

inline float ClampPitch(float p) { return std::clamp(p, kMinPitch, kMaxPitch); }
// maxD: Settings > Controls > Max zoom distance (game/control_settings.h; the default is kMaxDistance)
inline float ClampDistance(float d, float maxD = kMaxDistance) { return std::clamp(d, kMinDistance, std::clamp(maxD, kMinDistance, kMaxDistance)); }

// Positive = towards straight down.
inline void Tilt(OrbitCamera& cam, float radians) { cam.pitch = ClampPitch(cam.pitch + radians); }

// The wheel's zoom (game_core.inl, VpEv::Wheel): `notches` positive = closer, each one 0.9^0.5 of the distance. A pinch
// spreading the fingers by f arrives as notches = -2 ln f / ln 0.9, so the distance is divided by f.
inline float ZoomedDistance(float from, float notches, float maxD = kMaxDistance) { return ClampDistance(from * std::pow(0.9f, notches * 0.5f), maxD); }

// The pivot rides the ground under it (`ground(x, z)`: terrain or sea, whichever is higher); when the eye would come within
// `clearance` of the ground at its own spot or a few metres ahead, the pivot is lifted instead (the tilt stays the player's).
// Also clamps tilt and distance to the city's range. Stateless: the same camera and ground always give the same result.
template <class GroundFn>
void GroundFloor(OrbitCamera& cam, GroundFn ground, float clearance = kEyeClearance, float maxD = kMaxDistance) {
    cam.pitch = ClampPitch(cam.pitch);
    cam.distance = ClampDistance(cam.distance, maxD);
    cam.target.y = ground(cam.target.x, cam.target.z);
    const Vec3 eye = cam.Eye();
    const float fx = -std::sin(cam.yaw), fz = -std::cos(cam.yaw); // the view's heading over the ground
    const float floorY = std::max(ground(eye.x, eye.z), ground(eye.x + fx * 3.0f, eye.z + fz * 3.0f)) + clearance;
    if (eye.y < floorY) cam.target.y += floorY - eye.y;
}

// Drags the ground: the ground point under (x0, y0) ends under (x1, y1) (viewport pixels, origin top-left), on the plane
// y = groundY (the ground under the pivot). The camera moves only over the ground, never up or down. Where a finger is near
// or above the horizon the ray misses the plane; there the pan is screen-relative at the pivot's depth (at least 30 m).
inline void PanGround(OrbitCamera& cam, float x0, float y0, float x1, float y1, float width, float height, float groundY) {
    if (width < 1.0f || height < 1.0f) return;
    auto hit = [&](float px, float py, Vec3& p) {
        const Ray r = cam.ScreenRay(px, py, width, height);
        if (r.dir.y > -0.02f || r.origin.y <= groundY) return false;
        p = r.origin + r.dir * ((groundY - r.origin.y) / r.dir.y);
        return true;
    };
    Vec3 a, b;
    Vec3 move;
    if (hit(x0, y0, a) && hit(x1, y1, b)) {
        move = a - b;
        move.y = 0.0f;
        const float len = std::sqrt(move.x * move.x + move.z * move.z), cap = std::max(cam.distance, 20.0f) * 1.5f; // grazing rays
        if (len > cap) move = move * (cap / len);
    } else {
        const float perPixel = 2.0f * std::max(cam.distance, 30.0f) * std::tan(cam.fovY * 0.5f) / height;
        const Vec3 fwd = {-std::sin(cam.yaw), 0.0f, -std::cos(cam.yaw)}, right = {std::cos(cam.yaw), 0.0f, -std::sin(cam.yaw)};
        move = right * (-(x1 - x0) * perPixel) + fwd * ((y1 - y0) * perPixel);
    }
    cam.target = cam.target + move;
}

} // namespace tbd::citycam
