// Coordinate arithmetic for a future floating-origin integration. No runtime state is shifted here.
#pragma once
#include <cmath>
#include "vecmath.h"

namespace tbd {
struct WorldPosition { double x = 0, y = 0, z = 0; };

inline WorldPosition ToWorld(WorldPosition origin, Vec3 local) {
    return {origin.x + local.x, origin.y + local.y, origin.z + local.z};
}
inline Vec3 ToLocal(WorldPosition origin, WorldPosition world) {
    return {static_cast<float>(world.x - origin.x), static_cast<float>(world.y - origin.y),
            static_cast<float>(world.z - origin.z)};
}
// Previous-frame projection expressed in the new local coordinate system. If
// newLocal = oldLocal - offset, oldLocal = newLocal + offset.
inline Mat4 ShiftPreviousViewProjection(Mat4 previous, Vec3 offset) {
    for (int row = 0; row < 4; ++row)
        previous.At(row, 3) += previous.At(row, 0) * offset.x + previous.At(row, 1) * offset.y + previous.At(row, 2) * offset.z;
    return previous;
}

// A rebase is planned separately from applying it: every subsystem must consume the same
// offset at a frame boundary before the new origin becomes visible.
struct OriginShift { WorldPosition next; Vec3 offset; bool needed = false; };
inline OriginShift PlanOriginShift(WorldPosition origin, Vec3 focus, double threshold) {
    OriginShift result{origin, {}, false};
    if (!(threshold > 0.0) || !std::isfinite(threshold) || !std::isfinite(focus.x) ||
        !std::isfinite(focus.y) || !std::isfinite(focus.z)) return result;
    const double x = focus.x, y = focus.y, z = focus.z;
    if (x*x + y*y + z*z <= threshold*threshold) return result;
    result.next = ToWorld(origin, focus);
    result.offset = focus;
    result.needed = true;
    return result;
}
} // namespace tbd
