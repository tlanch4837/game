// In-engine profiler overlay (F3, View > Profiler Overlay): a frame time graph of the last 240 frames with 8.33 ms (120 fps)
// and 16.6 ms (60 fps) lines and the worst frame highlighted, CPU and cull time, draw/triangle/instance counts and the
// streamer's numbers. Drawn through the renderer's transient 2D path like the other viewport overlays, in the editor theme's
// palette (ui_theme.h). Header only; main.cpp feeds it a Sample per frame.
//
// Costs nothing when off: BeginFrame() and Draw() return at once, and main.cpp only gathers the cull numbers while
// Enabled() is true. It can never turn on during a TBD_SHOT or TBD_CAPTURE run (Blocked()), so captures stay clean.
//
// The pixel font has no '%', '=' or '>': labels are written around that.
#pragma once
#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "editor_draw.h"
#include "ui_theme.h"

namespace tbd {
namespace profiler {

constexpr int kHistory = 240;                  // frames in the graph
constexpr double kMs120 = 1000.0 / 120.0;      // 8.33 ms
constexpr double kMs60 = 1000.0 / 60.0;        // 16.6 ms
constexpr double kMs30 = 1000.0 / 30.0;        // 33.3 ms: past this a frame is drawn as a hitch

// What main.cpp counted this frame (scene numbers only; the profiler's own quads are never counted).
struct Sample {
    double cullMs = 0.0;
    uint32_t draws = 0;
    uint64_t triangles = 0;
    uint32_t entities = 0, entitiesDrawn = 0, sceneDraws = 0;
    uint32_t instancesDrawn = 0, instancesTotal = 0, instanceRanges = 0, casterInstances = 0;
    // Draws and triangles by kind (main.cpp's DrawCats, the last finished frame).
    struct Cat {
        const char* name = "";
        uint32_t draws = 0;
        uint64_t triangles = 0;
    };
    std::vector<Cat> cats;
    // The city sim scheduler (city_sim.inl SimProfile): totals, then one row per registered system.
    struct SimRow {
        std::string name;
        double ms = 0.0, avgMs = 0.0;
    };
    bool simActive = false;
    double simMs = 0.0, simAvgMs = 0.0, simBudgetMs = 0.0, simBacklog = 0.0;
    uint32_t simTicks = 0;
    std::vector<SimRow> sim;
    std::vector<std::pair<std::string, std::string>> simInfo; // label, value (demand, population, levels)
};

// The streamer's state (world_streaming.inl fills it in Streamer::Snapshot()).
struct StreamStats {
    bool active = false;
    uint32_t total = 0, loaded = 0, loading = 0, denied = 0, failed = 0;
    bool committing = false;
    uint64_t usedBytes = 0, budgetBytes = 0;
    double lastPumpMs = 0.0, worstPumpMs = 0.0, worstFrameMs = 0.0;
    uint32_t over33 = 0, over100 = 0;
};

// Per-pass GPU time of the last completed frame, ms. The renderers time these (see LogTimers in renderer_*.cpp) but do not
// expose them yet; Draw() takes nullptr until a Renderer accessor exists and then points the reader at engine.log.
constexpr int kGpuPasses = 8;
inline const char* const kGpuPassNames[kGpuPasses] = {"SHADOW", "SCENE", "AO", "CLOUDS", "COMPOSITE", "TAA", "TONEMAP", "OVERLAYS"};
struct GpuPasses {
    double ms[kGpuPasses] = {};
    double total = 0.0;
};

namespace detail {

inline Vec3 Rgb(COLORREF c) { return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f}; }

inline std::string Grouped(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

inline std::string Fmt(const char* format, double a) {
    char b[48];
    snprintf(b, sizeof(b), format, a);
    return b;
}

} // namespace detail

class Overlay {
public:
    // TBD_SHOT / TBD_CAPTURE runs are for clean captures and benchmarks: the profiler refuses to run in them.
    static bool Blocked() {
        static const bool blocked = GetEnvironmentVariableA("TBD_SHOT", nullptr, 0) > 0 || GetEnvironmentVariableA("TBD_CAPTURE", nullptr, 0) > 0;
        return blocked;
    }

    bool Enabled() const { return on_ && !Blocked(); }

    // F3 and the View menu. Returns the new state; stays off when blocked.
    bool Toggle() {
        if (Blocked()) return false;
        on_ = !on_;
        count_ = head_ = 0;
        last_ = {};
        pendingCpu_ = 0.0;
        return on_;
    }

    // First thing in RenderFrame: closes the previous frame's wall time (frame to frame, so it includes the present wait).
    void BeginFrame() {
        if (!Enabled()) return;
        const int64_t now = tbd::platform::Ticks();
        if (!freq_) freq_ = (double)tbd::platform::TickFrequency();
        if (last_) {
            const double ms = (double)(now - last_) * 1000.0 / freq_;
            if (ms < 1000.0) { // a longer gap is the window being idle or dragged, not a frame
                frame_[head_] = (float)ms;
                cpu_[head_] = (float)pendingCpu_;
                head_ = (head_ + 1) % kHistory;
                count_ = std::min(count_ + 1, kHistory);
            }
        }
        last_ = now;
        start_ = now;
    }

    // Right before EndFrame. Records this frame's CPU time (BeginFrame to here: the wait for the GPU is in EndFrame, so it
    // is not counted) and draws the panel under the FPS chip, top right.
    void Draw(Renderer& r, int w, int h, int dpi, const char* api, const Sample& smp, const StreamStats& st, const GpuPasses* gpu) {
        if (!Enabled()) return;
        using detail::Fmt;
        using detail::Grouped;
        using detail::Rgb;
        const int64_t now = tbd::platform::Ticks();
        pendingCpu_ = start_ ? (double)(now - start_) * 1000.0 / freq_ : 0.0;

        const float s = (float)dpi / 96.0f;
        const float cell = std::fmax(1.0f, std::round(1.5f * s * 2.0f) / 2.0f); // same tiny text as the stats
        const float cw = std::fmax(1.0f, std::round(s));                         // pixels per graph sample
        const float pad = std::round(8.0f * s), margin = std::round(8.0f * s), gap = std::round(6.0f * s);
        const float graphW = (float)kHistory * cw, graphH = std::round(72.0f * s);
        const float panelW = graphW + 2.0f * pad;
        if (panelW + 2.0f * margin > (float)w) return; // viewport too narrow for a readable panel
        const float rowH = std::round(9.0f * cell), rule = std::fmax(1.0f, std::round(s));
        const float px = (float)w - margin - panelW;
        const float py = margin + editor_draw::ChipHeight(cell, s) + gap;
        const float ix = px + pad, ir = px + panelW - pad; // inner left and right

        const Vec3 cPanel = Rgb(theme::kPanel), cWindow = Rgb(theme::kWindow), cLine = Rgb(theme::kLine), cRule = Rgb(theme::kRule);
        const Vec3 cText = Rgb(theme::kText), cBright = Rgb(theme::kTextBright), cMuted = Rgb(theme::kMuted);
        const Vec3 cRaised = Rgb(theme::kRaised), cAccent = Rgb(theme::kAccent);
        const Vec3 cGood = {0.30f, 0.62f, 0.35f}, cWarn = {0.92f, 0.66f, 0.16f}, cBad = {0.88f, 0.28f, 0.24f};

        std::vector<Vertex> bg, fg;
        float y = py;
        auto quad = [&](std::vector<Vertex>& out, float x0, float y0, float x1, float y1, Vec3 c) { AppendQuad(out, std::round(x0), std::round(y0), std::round(x1), std::round(y1), c); };
        auto textAt = [&](float x, float ty, const std::string& t, Vec3 c) { AppendText(fg, x, ty, cell, t, c); };
        auto textRight = [&](float xr, float ty, const std::string& t, Vec3 c) { AppendText(fg, xr - TextWidth(t, cell) + cell, ty, cell, t, c); };
        auto row = [&](const char* label, const std::string& value, Vec3 vc) {
            const float ty = y + std::round(1.0f * s);
            textAt(ix, ty, label, cMuted);
            textRight(ir, ty, value, vc);
            y += rowH;
        };
        auto section = [&](const char* title) {
            y += std::round(4.0f * s);
            textAt(ix, y, title, cMuted);
            y += rowH;
            quad(fg, ix, y, ir, y + rule, cRule);
            y += rule + std::round(3.0f * s);
        };
        auto msColor = [&](double ms) { return ms <= kMs60 + 0.5 ? cBright : (ms <= kMs30 + 0.1 ? cWarn : cBad); };

        // Header strip with the accent underline.
        const float headH = rowH + 2.0f * std::round(4.0f * s);
        quad(bg, px, y, px + panelW, y + headH, cWindow);
        quad(bg, px, y + headH, px + panelW, y + headH + std::round(2.0f * s), cAccent);
        textAt(ix, y + std::round(4.0f * s), "PROFILER", cBright);
        if (api && *api) textRight(ir, y + std::round(4.0f * s), api, cMuted);
        y += headH + std::round(2.0f * s) + std::round(2.0f * s);

        // History in time order, oldest first.
        const int n = count_;
        auto at = [&](int i) { return (head_ - n + i + 2 * kHistory) % kHistory; };
        double sum = 0.0, cpuSum = 0.0, worst = 0.0;
        int worstAt = -1;
        std::vector<float> sorted;
        sorted.reserve((size_t)n);
        for (int i = 0; i < n; ++i) {
            const float ms = frame_[at(i)];
            sum += ms;
            cpuSum += cpu_[at(i)];
            sorted.push_back(ms);
            if (ms >= worst) { worst = ms; worstAt = i; }
        }
        std::sort(sorted.begin(), sorted.end());
        const double avg = n ? sum / n : 0.0, cpuAvg = n ? cpuSum / n : 0.0;
        const double p99 = n ? sorted[(size_t)std::min(n - 1, n * 99 / 100)] : 0.0;

        section("FRAME TIME");
        {
            const double top = std::min(100.0, std::fmax(20.0, std::ceil(worst / 10.0) * 10.0));
            const float gx = ix, gy = y;
            quad(fg, gx, gy, gx + graphW, gy + graphH, cWindow);
            auto yOf = [&](double ms) { return gy + graphH - (float)(std::min(ms, top) / top) * graphH; };
            for (int i = 0; i < n; ++i) {
                const float x = gx + (float)(kHistory - n + i) * cw;
                const double ms = frame_[at(i)];
                if (i == worstAt) quad(fg, x - cw * 2.0f, gy, x + cw * 3.0f, gy + graphH, cRaised); // a faint column behind the worst frame
                const Vec3 c = i == worstAt ? cBright : (ms <= kMs60 + 0.5 ? cAccent : (ms <= kMs30 + 0.1 ? cWarn : cBad));
                quad(fg, x, yOf(ms), x + cw, gy + graphH, c);
            }
            struct Guide { double ms; const char* label; Vec3 color; };
            for (const Guide& g : {Guide{kMs120, "8.33", cGood}, Guide{kMs60, "16.6", cWarn}}) {
                const float ly = yOf(g.ms);
                quad(fg, gx, ly, gx + graphW, ly + rule, g.color);
                const std::string label = g.label;
                const float lw = TextWidth(label, cell), lx = gx + graphW - lw - std::round(2.0f * s);
                quad(fg, lx - std::round(2.0f * s), ly - 7.0f * cell - std::round(2.0f * s), gx + graphW, ly, cWindow);
                AppendText(fg, lx, ly - 7.0f * cell - std::round(1.0f * s), cell, label, g.color);
            }
            textAt(gx + std::round(2.0f * s), gy + std::round(2.0f * s), Fmt("%.0f MS", top), cMuted);
            y += graphH + std::round(4.0f * s);
        }
        row("AVERAGE", Fmt("%.1f MS", avg) + "   " + Fmt("%.0f FPS", avg > 0.0 ? 1000.0 / avg : 0.0), msColor(avg));
        row("WORST", Fmt("%.1f MS", worst), msColor(worst));
        row("99TH PCT", Fmt("%.1f MS", p99), msColor(p99));

        section("CPU");
        row("CPU FRAME", Fmt("%.1f MS", cpuAvg), msColor(cpuAvg));
        row("CULL", Fmt("%.2f MS", smp.cullMs), cText);
        if (gpu) {
            row("GPU FRAME", Fmt("%.2f MS", gpu->total), msColor(gpu->total));
            const Vec3 pal[kGpuPasses] = {{0.45f, 0.40f, 0.75f}, {0.0f, 0.44f, 0.88f}, {0.30f, 0.62f, 0.35f}, {0.45f, 0.70f, 0.85f},
                                          {0.85f, 0.55f, 0.25f}, {0.80f, 0.40f, 0.60f}, {0.75f, 0.75f, 0.35f}, {0.55f, 0.55f, 0.55f}};
            float bx = ix;
            const float barH = std::round(6.0f * s);
            for (int i = 0; i < kGpuPasses && gpu->total > 0.0; ++i) {
                const float bw = (float)(gpu->ms[i] / gpu->total) * (ir - ix);
                if (bw >= 1.0f) quad(fg, bx, y, std::min(bx + bw, ir), y + barH, pal[i]);
                bx += bw;
            }
            y += barH + std::round(3.0f * s);
            for (int i = 0; i < kGpuPasses; ++i) {
                quad(fg, ix, y + std::round(2.0f * s), ix + 4.0f * cell, y + std::round(2.0f * s) + 4.0f * cell, pal[i]);
                const float ty = y + std::round(1.0f * s);
                textAt(ix + 6.0f * cell, ty, kGpuPassNames[i], cMuted);
                textRight(ir, ty, Fmt("%.2f MS", gpu->ms[i]), cText);
                y += rowH;
            }
        } else {
            row("GPU PASSES", "SEE ENGINE.LOG", cMuted);
        }

        section("SCENE");
        row("DRAWS", Grouped(smp.draws), cText);
        row("TRIANGLES", Grouped(smp.triangles), cText);
        row("ENTITIES", Grouped(smp.entitiesDrawn) + " OF " + Grouped(smp.entities), cText);
        row("SCENE DRAWS", Grouped(smp.sceneDraws), cText);
        row("INSTANCES", Grouped(smp.instancesDrawn) + " OF " + Grouped(smp.instancesTotal), cText);
        row("INSTANCE RANGES", Grouped(smp.instanceRanges), cText);
        row("SHADOW INSTANCES", Grouped(smp.casterInstances), cText);

        if (!smp.cats.empty()) {
            section("DRAWS BY KIND");
            for (const Sample::Cat& c : smp.cats)
                if (c.draws || c.triangles) row(c.name, Grouped(c.draws) + "   " + Grouped(c.triangles / 1000) + "K TRIS", cText);
        }

        if (smp.simActive) {
            section("CITY SIM");
            const Vec3 simColor = smp.simMs <= smp.simBudgetMs + 0.25 ? cText : cWarn;
            row("SIM FRAME", Fmt("%.2f", smp.simMs) + " / " + Fmt("%.2f MS", smp.simBudgetMs), simColor);
            row("SIM AVERAGE", Fmt("%.2f MS", smp.simAvgMs), cText);
            row("TICKS", Grouped(smp.simTicks) + "   BEHIND " + Fmt("%.0f MIN", smp.simBacklog), smp.simBacklog >= 2.0 ? cWarn : cText);
            for (const Sample::SimRow& sys : smp.sim) {
                std::string label = sys.name;
                for (char& ch : label) ch = (char)std::toupper((unsigned char)ch);
                row(label.c_str(), Fmt("%.3f", sys.ms) + " / " + Fmt("%.3f MS", sys.avgMs), cText);
            }
            for (const auto& info : smp.simInfo) row(info.first.c_str(), info.second, cText);
        }

        if (st.active) {
            section("STREAMING");
            row("LOADED", Grouped(st.loaded) + " OF " + Grouped(st.total), cText);
            row("LOADING", Grouped(st.loading + (st.committing ? 1u : 0u)), st.loading ? cWarn : cText);
            row("DENIED BY BUDGET", Grouped(st.denied), st.denied ? cWarn : cText);
            if (st.failed) row("FAILED", Grouped(st.failed), cBad);
            row("MEMORY", Fmt("%.2f", (double)st.usedBytes / 1073741824.0) + " OF " + Fmt("%.2f GB", (double)st.budgetBytes / 1073741824.0), cText);
            row("PUMP", Fmt("%.1f", st.lastPumpMs) + " / " + Fmt("%.1f MS", st.worstPumpMs), msColor(st.worstPumpMs));
            row("WORST FRAME", Fmt("%.1f MS", st.worstFrameMs), msColor(st.worstFrameMs));
            row("OVER 33 MS", Grouped(st.over33), st.over33 ? cWarn : cText);
            row("OVER 100 MS", Grouped(st.over100), st.over100 ? cBad : cText);
        }
        y += std::round(4.0f * s);

        // Panel body and a one pixel border, drawn first.
        std::vector<Vertex> v;
        quad(v, px - rule, py - rule, px + panelW + rule, y + rule, cLine);
        quad(v, px, py, px + panelW, y, cPanel);
        v.insert(v.end(), bg.begin(), bg.end());
        v.insert(v.end(), fg.begin(), fg.end());
        r.DrawTransient(v.data(), (uint32_t)v.size(), Topology::Triangles, OrthoPixels((float)w, (float)h), Mat4::Identity(), false);
    }

private:
    bool on_ = false;
    float frame_[kHistory] = {}, cpu_[kHistory] = {};
    int head_ = 0, count_ = 0;
    double freq_ = 0.0, pendingCpu_ = 0.0;
    int64_t last_ = 0, start_ = 0;
};

} // namespace profiler
} // namespace tbd
