#include "../src/floating_origin.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
int main() {
    using namespace tbd;
    WorldPosition origin{};
    WorldPosition fixed{20000.125, -4.5, -21000.25};
    assert(!PlanOriginShift(origin, {512,0,0}, 512).needed);
    assert(!PlanOriginShift(origin, {1000,0,0}, 0).needed);
    assert(!PlanOriginShift(origin, {NAN,0,0}, 512).needed);
    int count = 0;
    for (int i = 1; i <= 100; ++i) {
        WorldPosition focus{250.0 * i + 0.125, 3.0, -250.0 * i};
        Vec3 local = ToLocal(origin, focus);
        OriginShift shift = PlanOriginShift(origin, local, 512);
        if (shift.needed) {
            WorldPosition before = ToWorld(origin, ToLocal(origin, fixed));
            origin = shift.next;
            WorldPosition after = ToWorld(origin, ToLocal(origin, fixed));
            assert(std::fabs(before.x - after.x) < 0.002);
            assert(std::fabs(before.z - after.z) < 0.002);
            ++count;
        }
    }
    assert(count > 20);
    // A stationary world point must project to the same previous clip position
    // after a change of local coordinates, including perspective's w component.
    Mat4 previous = PerspectiveRH(0.9f, 1.5f, 0.1f, 40000.0f) * LookAtRH({13, 7, 21}, {9, 4, 8}, {0, 1, 0});
    Vec3 offset{128, -64, 256};
    Vec4 oldPoint{132, -59, 242, 1};
    Vec4 newPoint{oldPoint.x - offset.x, oldPoint.y - offset.y, oldPoint.z - offset.z, 1};
    Vec4 oldClip = previous * oldPoint;
    Vec4 newClip = ShiftPreviousViewProjection(previous, offset) * newPoint;
    assert(std::fabs(oldClip.x - newClip.x) < 0.0001f);
    assert(std::fabs(oldClip.y - newClip.y) < 0.0001f);
    assert(std::fabs(oldClip.z - newClip.z) < 0.0001f);
    assert(std::fabs(oldClip.w - newClip.w) < 0.0001f);
    const float far = static_cast<float>(20000.0003);
    const float near = ToLocal({20000,0,0}, {20000.0003,0,0}).x;
    assert(std::fabs((double)near - 0.0003) < std::fabs((double)far - 20000.0003));
    std::printf("%d shifts; absolute float error %.9f m; local error %.9f m\n", count,
                std::fabs((double)far - 20000.0003), std::fabs((double)near - 0.0003));
}
