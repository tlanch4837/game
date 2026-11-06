// Per-frame statistics: FPS, draw calls and polygon (triangle) count.
// The renderer calls AddDrawCall() for every draw it submits; the engine calls BeginFrame() once per frame.
#pragma once
#include <windows.h>

#include <cstdint>

namespace tbd {

struct FrameStats {
    double fps = 0.0;
    double frameMs = 0.0;
    uint32_t drawCalls = 0;
    uint64_t polys = 0;
};

class Stats {
public:
    Stats() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&windowStart_);
    }

    // Call at the start of every frame. Publishes the draw/poly counts of the frame that just
    // finished and refreshes FPS twice a second so the number is readable.
    void BeginFrame() {
        shown_.drawCalls = cur_.drawCalls;
        shown_.polys = cur_.polys;
        cur_ = {};

        ++frames_;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = double(now.QuadPart - windowStart_.QuadPart) / double(freq_.QuadPart);
        if (elapsed >= kWindowSeconds) {
            shown_.fps = frames_ / elapsed;
            shown_.frameMs = elapsed * 1000.0 / frames_;
            frames_ = 0;
            windowStart_ = now;
        }
    }

    // Renderer hook: one call per draw submitted, with the triangles that draw covers.
    void AddDrawCall(uint64_t polys = 0) {
        ++cur_.drawCalls;
        cur_.polys += polys;
    }

    // Same, for a renderer that already totalled the frame's counted draws.
    void AddDraws(uint32_t draws, uint64_t polys) {
        cur_.drawCalls += draws;
        cur_.polys += polys;
    }

    const FrameStats& Shown() const { return shown_; }

private:
    static constexpr double kWindowSeconds = 0.5;

    LARGE_INTEGER freq_{};
    LARGE_INTEGER windowStart_{};
    uint32_t frames_ = 0;
    FrameStats cur_;   // being accumulated by the frame in progress
    FrameStats shown_; // last completed frame, plus smoothed FPS
};

} // namespace tbd
