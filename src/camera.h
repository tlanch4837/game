// Orbit camera: circles a target point. The View tools (Orbit, Pan, Zoom) and the mouse drive it.
#pragma once
#include <algorithm>
#include <cmath>

#include "vecmath.h"

namespace tbd {

class OrbitCamera {
public:
    Vec3 target = {0.0f, 0.5f, 0.0f};
    float distance = 7.0f;
    float yaw = 0.6f;    // radians around the Y axis
    float pitch = 0.45f; // radians above the ground plane
    float fovY = 0.9f;   // radians (about 52 degrees)
    float nearZ = 0.1f;
    float farZ = 40000.0f; // the far terrain reaches about 24 km

    Vec3 Eye() const {
        const float cp = std::cos(pitch);
        return target + Vec3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)} * distance;
    }

    Vec3 Forward() const { return Normalize(target - Eye()); }
    Vec3 Right() const { return Normalize(Cross(Forward(), Vec3{0, 1, 0})); }
    Vec3 Up() const { return Cross(Right(), Forward()); }

    Mat4 View() const { return LookAtRH(Eye(), target, Vec3{0, 1, 0}); }
    // Close up (skin pores, a few cm away) the near plane comes in with the distance, so the surface is not clipped away.
    Mat4 Projection(float aspect) const { return PerspectiveRH(fovY, aspect, std::clamp(distance * 0.1f, 0.004f, nearZ), farZ); }

    // World-space ray through a viewport pixel (origin top-left).
    Ray ScreenRay(float px, float py, float width, float height) const {
        const float t = std::tan(fovY * 0.5f);
        const float nx = (2.0f * px / width - 1.0f) * t * (width / height);
        const float ny = (1.0f - 2.0f * py / height) * t;
        return {Eye(), Normalize(Forward() + Right() * nx + Up() * ny)};
    }

    // World units covered by one pixel at `depth` in front of the camera.
    float PixelWorldSize(float depth, float viewportHeightPx) const {
        return 2.0f * depth * std::tan(fovY * 0.5f) / viewportHeightPx;
    }

    // Mouse drag in pixels: the scene follows the cursor.
    void Orbit(float dxPx, float dyPx) {
        yaw -= dxPx * 0.007f;
        pitch = std::clamp(pitch + dyPx * 0.007f, -1.5f, 1.5f);
    }

    // Free look: turns the view in place (the eye stays where it is, the target swings around it).
    void Look(float dxPx, float dyPx) {
        const Vec3 eye = Eye();
        yaw -= dxPx * 0.004f;
        pitch = std::clamp(pitch + dyPx * 0.004f, -1.5f, 1.5f);
        const float cp = std::cos(pitch);
        target = eye - Vec3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)} * distance;
    }

    // Fly: moves eye and target together, in world units along the view direction, its right and world up.
    void Fly(float forward, float right, float up) {
        target = target + Forward() * forward + Right() * right + Vec3{0, 1, 0} * up;
    }

    // Slides the target so the point under the cursor stays under the cursor.
    void Pan(float dxPx, float dyPx, float viewportHeightPx) {
        if (viewportHeightPx < 1.0f) return;
        const float perPixel = 2.0f * distance * std::tan(fovY * 0.5f) / viewportHeightPx;
        target = target - Right() * (dxPx * perPixel) + Up() * (dyPx * perPixel);
    }

    // Positive = closer. One mouse wheel notch is 1.0.
    void Zoom(float notches) { distance = std::clamp(distance * std::pow(0.9f, notches), 0.04f, 200.0f); }
};

} // namespace tbd
