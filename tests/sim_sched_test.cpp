// Headless checks for the city sim scheduler (src/city/sim.h):
//   two runs with the same game minutes give the same tick sequence; a frame over budget stops and carries the rest over;
//   pause stops ticking (backlog included); the game speed (minutes per frame) does not change what each minute computes;
//   staggered systems visit every slice once per period; the backlog cap drops minutes instead of catching up forever.
// Build and run: tests\sim_sched_test.bat
#include <cstdio>
#include <vector>

#include "city/sim.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// A fake clock: every system call costs `cost` ms; the scheduler's own calls are free.
struct FakeClock {
    double t = 0.0;
};

// A tiny deterministic "sim": per-cell counters updated by two staggered systems, hashed into a log.
struct Toy {
    std::vector<uint64_t> cells = std::vector<uint64_t>(37, 0);
    std::vector<uint64_t> log;
    void Attach(SimScheduler& s, FakeClock* clock, double costMs) {
        s.Register("growth", 60, 6, [this, clock, costMs](const SimTick& t) {
            for (size_t i = (size_t)t.slice; i < cells.size(); i += (size_t)t.sliceCount) {
                SplitMix64 r(SimSeed(42, i, (uint64_t)t.minute));
                cells[i] += r.Below(10);
            }
            if (clock) clock->t += costMs;
            log.push_back((uint64_t)t.minute * 1000 + (uint64_t)t.slice);
        });
        s.Register("decay", 1, 1, [this, clock, costMs](const SimTick& t) {
            cells[(size_t)(t.minute % (int64_t)cells.size())] /= 2;
            if (clock) clock->t += costMs;
        });
    }
    uint64_t Hash() const {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t c : cells) h = (h ^ c) * 1099511628211ull;
        for (uint64_t l : log) h = (h ^ l) * 1099511628211ull;
        return h;
    }
};

static uint64_t RunWithFrames(const std::vector<double>& minutesPerFrame, double costMs, double budgetMs, int64_t* ticksOut = nullptr, int64_t topUpTo = 0) {
    FakeClock clock;
    SimScheduler s;
    s.nowMs = [&clock] { return clock.t; };
    s.budgetMs = budgetMs;
    s.maxBacklogMinutes = 1e9;
    Toy toy;
    toy.Attach(s, &clock, costMs);
    for (double m : minutesPerFrame) s.Frame(m);
    for (int i = 0; i < 100000 && s.Backlog() >= 1.0; ++i) s.Frame(0.0); // drain
    if (ticksOut) *ticksOut = s.Minute();
    if (topUpTo > s.Minute()) s.RunTicks(topUpTo - s.Minute()); // float sums of the frame minutes may end one short
    return toy.Hash();
}

int main() {
    // Determinism: the same frames twice give the same results.
    {
        std::vector<double> frames;
        for (int i = 0; i < 2000; ++i) frames.push_back(0.3 + 0.01 * (i % 7));
        CHECK(RunWithFrames(frames, 0.01, 2.0) == RunWithFrames(frames, 0.01, 2.0));
    }
    // Speed: 1x, 3x and 9x frames over the same 1440 game minutes compute the same thing.
    {
        auto frames = [](double perFrame, double total) {
            std::vector<double> f;
            for (double t = 0; t + 1e-9 < total; t += perFrame) f.push_back(perFrame);
            return f;
        };
        int64_t a = 0, b = 0, c = 0;
        const uint64_t h1 = RunWithFrames(frames(2.0 / 60.0, 1440), 0.01, 2.0, &a, 1440);
        const uint64_t h3 = RunWithFrames(frames(6.0 / 60.0, 1440), 0.01, 2.0, &b, 1440);
        const uint64_t h9 = RunWithFrames(frames(18.0 / 60.0, 1440), 0.01, 2.0, &c, 1440);
        CHECK(a >= 1439 && a <= 1440 && b >= 1439 && c >= 1439);
        CHECK(h1 == h3 && h3 == h9);
        // Budget does not change results either (a tight budget spreads the same ticks over more frames).
        CHECK(RunWithFrames(frames(18.0 / 60.0, 1440), 0.5, 1.0, nullptr, 1440) == h9);
    }
    // Budget overrun carries over: each tick costs 1 ms (decay 0.5 + growth 0.5 sometimes), budget 2 ms, 10 minutes due.
    {
        FakeClock clock;
        SimScheduler s;
        s.nowMs = [&clock] { return clock.t; };
        s.budgetMs = 2.0;
        Toy toy;
        toy.Attach(s, &clock, 1.0);
        const int ran = s.Frame(10.0);
        CHECK(ran >= 1 && ran <= 2);
        CHECK(s.Backlog() >= 8.0 - 1e-9);
        CHECK(s.FrameMs() <= 2.0 + 2.0 + 1e-9); // one tick may cross the line, never more
        int frames = 1;
        while (s.Backlog() >= 1.0 && frames < 100) { s.Frame(0.0); ++frames; }
        CHECK(s.Minute() == 10);
        CHECK(frames > 1);
    }
    // Pause stops ticking, backlog included; resume picks it up.
    {
        SimScheduler s;
        int calls = 0;
        s.Register("count", 1, 1, [&calls](const SimTick&) { ++calls; });
        s.Frame(3.0);
        CHECK(calls == 3);
        s.paused = true;
        s.Frame(5.0);
        CHECK(calls == 3);
        s.paused = false;
        s.Frame(0.0);
        CHECK(calls == 3); // minutes that passed while paused never entered the backlog
        s.Frame(2.0);
        CHECK(calls == 5);
    }
    // Stagger: period 60, stagger 6 -> one slice every 10 minutes, every slice once per period.
    {
        SimScheduler s;
        std::vector<int> seen(6, 0);
        std::vector<int64_t> minutes;
        s.Register("s", 60, 6, [&](const SimTick& t) {
            CHECK(t.sliceCount == 6);
            ++seen[(size_t)t.slice];
            minutes.push_back(t.minute);
        });
        s.RunTicks(120);
        for (int v : seen) CHECK(v == 2);
        CHECK(minutes.size() == 12);
        for (size_t i = 0; i < minutes.size(); ++i) CHECK(minutes[i] == (int64_t)i * 10);
        // stagger larger than the period clamps to one slice per minute
        SimScheduler s2;
        int n = 0;
        s2.Register("c", 4, 100, [&](const SimTick& t) { CHECK(t.sliceCount == 4); ++n; });
        s2.RunTicks(8);
        CHECK(n == 8);
    }
    // Backlog cap: a 10 hour stall at 240 minutes cap drops the rest.
    {
        SimScheduler s;
        s.maxBacklogMinutes = 240;
        s.maxTicksPerFrame = 0;
        s.Frame(600.0);
        CHECK(s.Backlog() <= 240.0 + 1e-9);
        CHECK(s.DroppedMinutes() == 360);
    }
    // Timing fields are filled.
    {
        FakeClock clock;
        SimScheduler s;
        s.nowMs = [&clock] { return clock.t; };
        Toy toy;
        toy.Attach(s, &clock, 0.25);
        s.Frame(1.0);
        CHECK(s.Systems()[0].frameMs == 0.25); // minute 0: growth runs
        CHECK(s.Systems()[1].frameMs == 0.25);
        CHECK(s.Systems()[0].calls == 1);
    }
    // Real clock: a 2 ms budget with 0.3 ms busy ticks stops near the budget.
    {
        SimScheduler s;
        s.budgetMs = 2.0;
        s.Register("busy", 1, 1, [&s](const SimTick&) {
            const double a = s.nowMs();
            while (s.nowMs() - a < 0.3) {}
        });
        s.Frame(100.0);
        std::printf("real clock: %d ticks in %.2f ms (budget 2.00), backlog %.0f\n", s.TicksLastFrame(), s.FrameMs(), s.Backlog());
        CHECK(s.FrameMs() < 2.0 + 0.6);
        CHECK(s.TicksLastFrame() >= 5 && s.TicksLastFrame() <= 8);
    }

    if (failures) {
        std::printf("sim_sched_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_sched_test: all passed\n");
    return 0;
}
