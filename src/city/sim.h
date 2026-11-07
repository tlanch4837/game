// The city simulation's fixed-step scheduler (Track S, phase S0). Header only, no renderer, window or scene dependencies,
// so tests\sim_sched_test.cpp builds it headless.
//
// One sim tick is one game minute. Per frame the host feeds the game minutes that passed (SimSync in city_sim.inl takes
// them from the change in g_clock.minutes, so the sim can never drift from the clock the HUD shows) and the scheduler runs
// whole ticks until either the backlog is empty or the per-frame budget (default 2 ms) is spent. What is left carries over
// to the next frame: the sim slips behind instead of hitching. A backlog past maxBacklogMinutes is dropped (counted in
// droppedMinutes) so a long stall cannot turn into minutes of catch-up.
//
// Systems register as {name, periodMinutes, stagger, fn}. A system with period P and stagger K runs one slice every P/K
// minutes, slices 0..K-1 in turn; the slice tells it which share of its cells to process (cellIndex % K == slice). Every
// result depends only on the tick number and the slice, never on the frame rate, the game speed or the budget: the same
// sequence of game minutes always runs the same ticks in the same order.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tbd {
namespace city {

// What one system call gets.
struct SimTick {
    int64_t minute = 0; // the tick being run (game minutes since the scheduler started counting)
    int slice = 0;      // 0..sliceCount-1: process the cells with index % sliceCount == slice
    int sliceCount = 1;
};

struct SimSystem {
    std::string name;
    int periodMinutes = 1; // every cell is visited once per period
    int stagger = 1;       // slices per period (clamped to 1..periodMinutes)
    std::function<void(const SimTick&)> fn;
    // Timing, for the F3 overlay.
    double frameMs = 0.0;  // time spent in this system during the last frame
    double avgMs = 0.0;    // rolling average of frameMs (per frame, including frames where it did not run)
    double worstMs = 0.0;  // worst single call since start
    uint64_t calls = 0;

    int SliceMinutes() const { return std::max(1, periodMinutes / std::max(1, std::min(stagger, periodMinutes))); }
    int Slices() const { return std::max(1, std::min(stagger, periodMinutes)); }
};

class SimScheduler {
public:
    double budgetMs = 2.0;               // per-frame cap on time spent ticking
    int maxTicksPerFrame = 600;          // hard cap even if the ticks are cheap (10 game hours)
    double maxBacklogMinutes = 240.0;    // past this the oldest minutes are dropped
    double avgAlpha = 0.05;              // rolling-average weight of the newest frame
    bool paused = false;                 // nothing ticks while paused, not even the backlog

    // Milliseconds since some fixed point. Tests replace it with a fake clock; the default is steady_clock.
    std::function<double()> nowMs = [] {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    };

    int Register(std::string name, int periodMinutes, int stagger, std::function<void(const SimTick&)> fn) {
        SimSystem s;
        s.name = std::move(name);
        s.periodMinutes = std::max(1, periodMinutes);
        s.stagger = std::max(1, stagger);
        s.fn = std::move(fn);
        systems_.push_back(std::move(s));
        return (int)systems_.size() - 1;
    }

    // Per frame: gameMinutes that passed on the game clock this frame (>= 0). Returns the ticks run.
    int Frame(double gameMinutes) {
        for (SimSystem& s : systems_) s.frameMs = 0.0;
        ticksLastFrame_ = 0;
        frameMs_ = 0.0;
        if (gameMinutes > 0.0 && !paused) backlog_ += gameMinutes;
        if (backlog_ > maxBacklogMinutes) {
            const double drop = std::floor(backlog_ - maxBacklogMinutes);
            droppedMinutes_ += (int64_t)drop;
            backlog_ -= drop;
        }
        if (!paused) {
            const double start = nowMs();
            while (backlog_ >= 1.0 && ticksLastFrame_ < maxTicksPerFrame) {
                RunTick();
                backlog_ -= 1.0;
                ++ticksLastFrame_;
                if (nowMs() - start >= budgetMs) break; // over budget: the rest waits for the next frame
            }
            frameMs_ = nowMs() - start;
        }
        for (SimSystem& s : systems_) s.avgMs += (s.frameMs - s.avgMs) * avgAlpha;
        avgFrameMs_ += (frameMs_ - avgFrameMs_) * avgAlpha;
        return ticksLastFrame_;
    }

    // Runs exactly n ticks now, ignoring the budget (tests, fast-forward).
    void RunTicks(int64_t n) {
        for (int64_t i = 0; i < n; ++i) RunTick();
    }

    // A loaded game or a new map: start counting again at minute 0 with nothing pending.
    void Reset(int64_t minute = 0) {
        minute_ = minute;
        backlog_ = 0.0;
        droppedMinutes_ = 0;
    }

    int64_t Minute() const { return minute_; }            // ticks run so far
    double Backlog() const { return backlog_; }           // game minutes waiting
    int64_t DroppedMinutes() const { return droppedMinutes_; }
    int TicksLastFrame() const { return ticksLastFrame_; }
    double FrameMs() const { return frameMs_; }
    double AvgFrameMs() const { return avgFrameMs_; }
    const std::vector<SimSystem>& Systems() const { return systems_; }

private:
    void RunTick() {
        const int64_t m = minute_;
        for (SimSystem& s : systems_) {
            const int every = s.SliceMinutes(), slices = s.Slices();
            if (m % every != 0) continue;
            SimTick t;
            t.minute = m;
            t.sliceCount = slices;
            t.slice = (int)((m / every) % slices);
            const double a = nowMs();
            if (s.fn) s.fn(t);
            const double ms = nowMs() - a;
            s.frameMs += ms;
            s.worstMs = std::max(s.worstMs, ms);
            ++s.calls;
        }
        ++minute_;
    }

    std::vector<SimSystem> systems_;
    int64_t minute_ = 0;
    double backlog_ = 0.0;
    int64_t droppedMinutes_ = 0;
    int ticksLastFrame_ = 0;
    double frameMs_ = 0.0, avgFrameMs_ = 0.0;
};

// Per-entity random streams (SplitMix64): seed one per entity from (city seed, entity id), never a shared global stream.
struct SplitMix64 {
    uint64_t state;
    explicit SplitMix64(uint64_t seed) : state(seed) {}
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t Below(uint32_t n) { return n ? (uint32_t)(((Next() >> 32) * (uint64_t)n) >> 32) : 0; } // [0, n)
};
inline uint64_t SimSeed(uint64_t citySeed, uint64_t id, uint64_t salt = 0) {
    SplitMix64 r(citySeed ^ (id * 0xD1B54A32D192ED03ull) ^ (salt * 0x8CB92BA72F3D8DD7ull));
    return r.Next();
}

} // namespace city
} // namespace tbd
