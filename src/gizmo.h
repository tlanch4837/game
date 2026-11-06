// Transform gizmo: Move, Rotate and Scale handles for the selected entity. Pure math plus geometry
// generation; the tool selects the mode and the viewport feeds it mouse rays.
#pragma once
#include <cmath>
#include <vector>

#include "camera.h"
#include "render/renderer.h"
#include "scene.h"
#include "tools.h"

namespace tbd {

enum class GizmoMode { None, Move, Rotate, Scale };

inline GizmoMode GizmoModeForTool(ToolId tool) {
    switch (tool) {
    case ToolId::Move: return GizmoMode::Move;
    case ToolId::Rotate: return GizmoMode::Rotate;
    case ToolId::Scale: return GizmoMode::Scale;
    default: return GizmoMode::None;
    }
}

namespace gizmo_detail {

constexpr float kHandlePixels = 100.0f; // axis length on screen, at 96 dpi
constexpr float kGrabPixels = 9.0f;     // how close the cursor must be to grab a handle
constexpr int kRingSegments = 64;

inline Vec3 AxisColor(int axis) {
    static const Vec3 colors[3] = {{0.86f, 0.30f, 0.26f}, {0.42f, 0.78f, 0.35f}, {0.30f, 0.50f, 0.92f}};
    return colors[axis];
}
inline Vec3 WorldAxis(int axis) { return Vec3{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f}; }

inline void AddTriangle(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 color) {
    for (Vec3 p : {a, b, c}) out.push_back({p.x, p.y, p.z, 0, 0, 0, color.x, color.y, color.z, 1.0f});
}

// Two vectors perpendicular to `dir` and to each other.
inline void Perpendiculars(Vec3 dir, Vec3& s, Vec3& t) {
    const Vec3 helper = std::fabs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    s = Normalize(Cross(dir, helper));
    t = Cross(dir, s);
}

// Square-section bar from p0 to p1.
inline void AddBar(std::vector<Vertex>& out, Vec3 p0, Vec3 p1, float halfThickness, Vec3 color) {
    Vec3 s, t;
    Perpendiculars(Normalize(p1 - p0), s, t);
    s = s * halfThickness;
    t = t * halfThickness;
    const Vec3 a[4] = {p0 - s - t, p0 + s - t, p0 + s + t, p0 - s + t};
    const Vec3 b[4] = {p1 - s - t, p1 + s - t, p1 + s + t, p1 - s + t};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        AddTriangle(out, a[i], a[j], b[j], color);
        AddTriangle(out, a[i], b[j], b[i], color);
    }
    AddTriangle(out, a[0], a[1], a[2], color);
    AddTriangle(out, a[0], a[2], a[3], color);
    AddTriangle(out, b[0], b[2], b[1], color);
    AddTriangle(out, b[0], b[3], b[2], color);
}

inline void AddCone(std::vector<Vertex>& out, Vec3 baseCenter, Vec3 dir, float radius, float height, Vec3 color) {
    Vec3 s, t;
    Perpendiculars(dir, s, t);
    const Vec3 apex = baseCenter + dir * height;
    constexpr int kSides = 16;
    for (int i = 0; i < kSides; ++i) {
        const float a0 = 2.0f * kPi * (float)i / kSides, a1 = 2.0f * kPi * (float)(i + 1) / kSides;
        const Vec3 p0 = baseCenter + (s * std::cos(a0) + t * std::sin(a0)) * radius;
        const Vec3 p1 = baseCenter + (s * std::cos(a1) + t * std::sin(a1)) * radius;
        AddTriangle(out, apex, p0, p1, color);
        AddTriangle(out, baseCenter, p1, p0, color);
    }
}

// Parameter t of the point on the line (origin + dir * t) that is closest to the ray. dir is unit length.
inline bool ClosestParamOnLine(Vec3 lineOrigin, Vec3 lineDir, const Ray& ray, float& t) {
    const Vec3 w0 = lineOrigin - ray.origin;
    const float b = Dot(lineDir, ray.dir), d = Dot(lineDir, w0), e = Dot(ray.dir, w0);
    const float denom = 1.0f - b * b;
    if (denom < 1e-5f) return false; // line and ray are parallel
    t = (b * e - d) / denom;
    return true;
}

inline bool RayPlane(const Ray& ray, Vec3 point, Vec3 normal, Vec3& hit) {
    const float denom = Dot(ray.dir, normal);
    if (std::fabs(denom) < 0.05f) return false; // nearly edge-on
    const float t = Dot(point - ray.origin, normal) / denom;
    if (t < 0) return false;
    hit = ray.origin + ray.dir * t;
    return true;
}

} // namespace gizmo_detail

class Gizmo {
public:
    GizmoMode mode = GizmoMode::None;
    int hover = -1;   // axis under the cursor
    int active = -1;  // axis being dragged
    bool Dragging() const { return active >= 0; }

    // Handle length in world units: constant size on screen.
    float HandleLength(const OrbitCamera& cam, const Entity& e, float viewportH, float dpiScale) const {
        const float depth = std::fmax(0.1f, Dot(e.position - cam.Eye(), cam.Forward()));
        return gizmo_detail::kHandlePixels * dpiScale * cam.PixelWorldSize(depth, viewportH);
    }

    // Direction of an axis handle: Scale follows the object, Move and Rotate use the world.
    Vec3 AxisDir(int axis, const Entity& e) const {
        const Vec3 w = gizmo_detail::WorldAxis(axis);
        return mode == GizmoMode::Scale ? Normalize(Rotate(e.rotation, w)) : w;
    }

    void Draw(Renderer& r, const OrbitCamera& cam, const Mat4& viewProj, const Entity& e, float viewportH,
              float dpiScale) const {
        using namespace gizmo_detail;
        if (mode == GizmoMode::None) return;
        const float len = HandleLength(cam, e, viewportH, dpiScale);
        std::vector<Vertex> v;
        for (int axis = 0; axis < 3; ++axis) {
            const bool lit = axis == hover || axis == active;
            const Vec3 color = lit ? Vec3{1.0f, 0.92f, 0.3f} : AxisColor(axis);
            const Vec3 dir = AxisDir(axis, e);
            const Vec3 c = e.position;
            switch (mode) {
            case GizmoMode::Move:
                AddBar(v, c, c + dir * (len * 0.78f), len * 0.022f, color);
                AddCone(v, c + dir * (len * 0.78f), dir, len * 0.07f, len * 0.22f, color);
                break;
            case GizmoMode::Scale:
                AddBar(v, c, c + dir * (len * 0.86f), len * 0.022f, color);
                AddBar(v, c + dir * (len * 0.86f), c + dir * len, len * 0.06f, color);
                break;
            case GizmoMode::Rotate: {
                Vec3 s, t;
                Perpendiculars(dir, s, t);
                const float radius = len * 0.8f;
                Vec3 prev = c + s * radius;
                for (int i = 1; i <= kRingSegments; ++i) {
                    const float a = 2.0f * kPi * (float)i / kRingSegments;
                    const Vec3 p = c + (s * std::cos(a) + t * std::sin(a)) * radius;
                    AddBar(v, prev, p, len * 0.012f, color);
                    prev = p;
                }
                break;
            }
            case GizmoMode::None: break;
            }
        }
        r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Triangles, viewProj, Mat4::Identity(), false);
    }

    // Which axis handle is under the ray, or -1.
    int HitTest(const Ray& ray, const OrbitCamera& cam, const Entity& e, float viewportH, float dpiScale) const {
        using namespace gizmo_detail;
        if (mode == GizmoMode::None) return -1;
        const float len = HandleLength(cam, e, viewportH, dpiScale);
        const float depth = std::fmax(0.1f, Dot(e.position - cam.Eye(), cam.Forward()));
        const float grab = kGrabPixels * dpiScale * cam.PixelWorldSize(depth, viewportH);

        int best = -1;
        float bestScore = 1e30f;
        for (int axis = 0; axis < 3; ++axis) {
            const Vec3 dir = AxisDir(axis, e);
            float score;
            if (mode == GizmoMode::Rotate) {
                Vec3 hit;
                if (!RayPlane(ray, e.position, dir, hit)) continue;
                score = std::fabs(Length(hit - e.position) - len * 0.8f);
            } else {
                float t;
                if (!ClosestParamOnLine(e.position, dir, ray, t)) continue;
                t = std::fmax(0.0f, std::fmin(len, t));
                const Vec3 p = e.position + dir * t;
                const float along = Dot(p - ray.origin, ray.dir);
                if (along < 0) continue;
                score = Length(p - (ray.origin + ray.dir * along));
            }
            if (score < grab && score < bestScore) {
                bestScore = score;
                best = axis;
            }
        }
        return best;
    }

    // Starts dragging `axis` with the entity's current transform as the reference.
    bool Begin(int axis, const Ray& ray, const OrbitCamera& cam, const Entity& e, float viewportH, float dpiScale) {
        using namespace gizmo_detail;
        axisDir_ = AxisDir(axis, e);
        startPosition_ = e.position;
        startRotation_ = e.rotation;
        startScale_ = e.scale;
        handleLength_ = HandleLength(cam, e, viewportH, dpiScale);
        if (mode == GizmoMode::Rotate) {
            Vec3 hit;
            if (!RayPlane(ray, e.position, axisDir_, hit)) return false;
            startVector_ = Normalize(hit - e.position);
        } else if (!ClosestParamOnLine(e.position, axisDir_, ray, startParam_)) {
            return false;
        }
        active = axis;
        return true;
    }

    // Applies the drag to the entity.
    void Update(const Ray& ray, Entity& e) const {
        using namespace gizmo_detail;
        if (active < 0) return;
        switch (mode) {
        case GizmoMode::Move: {
            float t;
            if (ClosestParamOnLine(startPosition_, axisDir_, ray, t)) e.position = startPosition_ + axisDir_ * (t - startParam_);
            break;
        }
        case GizmoMode::Scale: {
            float t;
            if (!ClosestParamOnLine(startPosition_, axisDir_, ray, t)) break;
            const float factor = std::fmax(0.01f, 1.0f + (t - startParam_) / handleLength_);
            e.scale = startScale_;
            float* s[3] = {&e.scale.x, &e.scale.y, &e.scale.z};
            *s[active] = std::fmax(0.01f, *s[active] * factor);
            break;
        }
        case GizmoMode::Rotate: {
            Vec3 hit;
            if (!RayPlane(ray, startPosition_, axisDir_, hit)) break;
            const Vec3 v = Normalize(hit - startPosition_);
            const float angle = std::atan2(Dot(axisDir_, Cross(startVector_, v)), Dot(startVector_, v));
            e.rotation = Normalize(QuatAxisAngle(axisDir_, angle) * startRotation_);
            break;
        }
        case GizmoMode::None: break;
        }
    }

    void End() { active = -1; }

private:
    static float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

    Vec3 axisDir_, startPosition_, startScale_, startVector_;
    Quat startRotation_;
    float startParam_ = 0.0f, handleLength_ = 1.0f;
};

} // namespace tbd
