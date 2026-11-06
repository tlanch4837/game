// The one place that turns engine world positions into AI positions and back.
//
// The navmesh, the agents and every stored path live in "AI space": world space minus a shared offset that starts at zero.
// When the world is rebased (floating origin) the engine calls ai::ShiftWorld(delta) with how far the world's origin moved;
// nothing built in AI space (tiles, paths, crowd agents) has to be touched. All public AI calls take and return WORLD positions.
#pragma once
#include "vecmath.h"

namespace tbd::ai {

inline Vec3& WorldOffset() {
    static Vec3 offset = {0, 0, 0};
    return offset;
}
inline Vec3 ToAi(Vec3 world) { return world - WorldOffset(); }
inline Vec3 FromAi(Vec3 ai) { return ai + WorldOffset(); }
// The world moved: a point that was at world W is now at W - delta, and its AI position is unchanged.
inline void ShiftWorld(Vec3 delta) { WorldOffset() = WorldOffset() - delta; }

} // namespace tbd::ai
