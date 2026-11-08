// The HUD's information panels, from what the city sim already counts (no invented readings: no happiness, crime or
// pollution, which the sim does not compute): City Information, Population Info, the Notifications log, Statistics and the city name
// card. Ported from the sim-budget-milestones branch onto ios-metal's HUD (both the PC HUD, city_hud.h, and Glass Dock+,
// game/glass_dock_host.inl, open them by name). Platform free. The host fills HudCityStats (city_sim.inl SimInfoFill)
// only while one of these panels is open, at most twice a second, so the counting costs nothing otherwise.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gui.h"

namespace tbd {
namespace gui {

struct HudCityStats {
    std::string cityName, milestone, nextMilestone;
    long long nextPop = 0;
    long long population = 0, kids = 0, adults = 0, seniors = 0, working = 0, unemployed = 0;
    long long homes = 0, jobs = 0, jobsFilled = 0, jobsByKind[3] = {}, filledByKind[3] = {}; // industry, commercial, office
    long long edu[3] = {}, wealth[4] = {};
    float demand[4] = {};          // -1 .. 1: residential, commercial, office, industrial
    float landValue = 0;           // 0 .. 255, averaged over residents
    float powerShare = 1, waterShare = 1, sewageShare = 1; // of residents
    float coverage[6] = {};        // 0 .. 255 per service, averaged over residents
    long long levelUps = 0, levelDowns = 0, abandoned = 0;
    long long trips = 0, slowRoads = 0; // road segments with peak-hour flow, and the congested ones
    long long treasury = 0, net = 0;
    std::vector<long long> balance, pop; // the last days, oldest first (Statistics)
    std::vector<std::string> notes;      // the notification log, newest last
};

// True for the names CityInfoPanel draws (the HUD asks before it opens its "not completed" card).
inline bool IsCityInfoPanel(const char* name) {
    if (!name) return false;
    for (const char* k : {"City Information", "Population Info", "Population", "Notifications", "Statistics", "City Name"})
        if (std::strcmp(name, k) == 0) return true;
    return false;
}

namespace info_detail {
inline std::string Count(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return v < 0 ? "-" + s : s;
}
inline void Pair(Context& g, const char* label, const char* value, Color ink) {
    const Theme& th = g.Style();
    const Rect r = g.Reserve(std::round(19.0f * g.Scale()), 0.0f);
    g.TextIn({r.x, r.y, r.w * 0.62f, r.h}, label, th.text, 0.0f, th.smallSize);
    g.TextIn({r.x + r.w * 0.38f, r.y, r.w * 0.62f, r.h}, value, ink, 1.0f, th.smallSize);
}
inline void PairN(Context& g, const char* label, long long v) { Pair(g, label, Count(v).c_str(), g.Style().textBright); }
// A horizontal bar for a value in [lo, hi], centred on zero when lo < 0.
inline void Bar(Context& g, const char* label, float v, float lo, float hi, Color ink) {
    const Theme& th = g.Style();
    const float s = g.Scale();
    const Rect r = g.Reserve(std::round(19.0f * s), 0.0f);
    g.TextIn({r.x, r.y, r.w * 0.36f, r.h}, label, th.text, 0.0f, th.smallSize);
    const float x0 = r.x + r.w * 0.38f, x1 = r.x + r.w, cy = r.y + r.h * 0.5f, hh = std::round(4.0f * s);
    g.Draw().Rect(x0, cy - hh, x1, cy + hh, th.track, th.track, hh, th.border, th.trackBorder);
    const float t = (std::clamp(v, lo, hi) - lo) / (hi - lo), zero = lo < 0.0f ? (0.0f - lo) / (hi - lo) : 0.0f;
    const float a = x0 + (x1 - x0) * std::min(t, zero), b = x0 + (x1 - x0) * std::max(t, zero);
    if (b - a > 0.5f) g.Draw().Rect(a, cy - hh, b, cy + hh, ink, ink, hh);
}
inline void Spark(Context& g, const std::vector<long long>& v, Color ink, const char* empty) {
    const Theme& th = g.Style();
    const float s = g.Scale();
    const Rect r = g.Reserve(std::round(56.0f * s), 0.0f);
    DrawList& dl = g.Draw();
    dl.Rect(r.x, r.y, r.x + r.w, r.y + r.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
    const int n = (int)v.size();
    if (n < 2) {
        g.TextIn(r, empty, th.muted, 0.5f, th.smallSize);
        return;
    }
    long long lo = v[0], hi = v[0];
    for (int i = 1; i < n; ++i) lo = std::min(lo, v[(size_t)i]), hi = std::max(hi, v[(size_t)i]);
    if (hi == lo) hi = lo + 1;
    const float pad = std::round(5.0f * s), x0 = r.x + pad, x1 = r.x + r.w - pad, y0 = r.y + pad, y1 = r.y + r.h - pad;
    auto Y = [&](long long val) { return y1 - (float)((double)(val - lo) / (double)(hi - lo)) * (y1 - y0); };
    float px = x0, py = Y(v[0]);
    for (int i = 1; i < n; ++i) {
        const float qx = x0 + (x1 - x0) * (float)i / (float)(n - 1), qy = Y(v[(size_t)i]);
        dl.Line(px, py, qx, qy, std::max(1.2f, 1.6f * s), ink);
        px = qx, py = qy;
    }
    g.TextIn({r.x, r.y, r.w - pad, std::round(14.0f * s)}, Count(hi).c_str(), th.muted, 1.0f, th.smallSize * 0.85f);
}
} // namespace info_detail

// One of the info panels when `name` is one this file knows (IsCityInfoPanel); returns 0 when it is not (the HUD shows
// its "not completed" card), 1 when shown, 2 when the player closed it.
inline int CityInfoPanel(Context& g, const char* name, const HudCityStats& st) {
    using namespace info_detail;
    if (!IsCityInfoPanel(name)) return 0;
    const bool info = std::strcmp(name, "City Information") == 0, people = std::strcmp(name, "Population Info") == 0 || std::strcmp(name, "Population") == 0;
    const bool notes = std::strcmp(name, "Notifications") == 0, stats = std::strcmp(name, "Statistics") == 0, city = std::strcmp(name, "City Name") == 0;
    const Theme& th = g.Style();
    const Color warn = Rgba(255, 196, 80);
    char b[96];
    if (city || stats) g.BeginPanel(city ? (st.cityName.empty() ? "City" : st.cityName.c_str()) : "Statistics", Anchor::TopCenter, 0.0f, 60.0f, 420.0f, 0.0f, 12.0f);
    else g.BeginPanel(name, Anchor::TopRight, 76.0f, 8.0f, info ? 560.0f : 380.0f, 0.0f, 12.0f); // beside the right column
    if (city) {
        PairN(g, "Population", st.population);
        Pair(g, "Milestone", st.milestone.c_str(), th.textBright);
        if (st.nextPop > 0) {
            std::snprintf(b, sizeof(b), "%s at %s", st.nextMilestone.c_str(), Count(st.nextPop).c_str());
            Pair(g, "Next milestone", b, th.text);
            Bar(g, "Progress", (float)st.population, 0.0f, (float)st.nextPop, th.accent);
        }
        std::snprintf(b, sizeof(b), "$%s", Count(st.treasury).c_str());
        Pair(g, "Treasury", b, st.treasury < 0 ? Rgba(235, 90, 80) : th.textBright);
        g.Text(TextStyle::Muted, "Renaming the city is not completed yet.");
    } else if (info) {
        // two columns in one block (no widget gaps, so it fits a small window): demand and utilities | coverage and land
        const float sc = g.Scale(), rowH = std::round(17.0f * sc), colGap = std::round(18.0f * sc);
        const Rect blk = g.Reserve(rowH * 9.0f, 0.0f);
        const float colW = (blk.w - colGap) * 0.5f;
        auto head = [&](int col, int row, const char* t) { g.TextIn({blk.x + col * (colW + colGap), blk.y + row * rowH, colW, rowH}, t, th.muted, 0.0f, th.smallSize, true); };
        auto bar = [&](int col, int row, const char* label, float v, float lo, float hi, Color ink) {
            const float x = blk.x + col * (colW + colGap), y = blk.y + row * rowH;
            g.TextIn({x, y, colW * 0.5f, rowH}, label, th.text, 0.0f, th.smallSize);
            const float x0 = x + colW * 0.5f, x1 = x + colW, cy = y + rowH * 0.5f, hh = std::round(3.0f * sc);
            g.Draw().Rect(x0, cy - hh, x1, cy + hh, th.track, th.track, hh, th.border, th.trackBorder);
            const float t = (std::clamp(v, lo, hi) - lo) / (hi - lo), zero = lo < 0.0f ? -lo / (hi - lo) : 0.0f;
            const float a = x0 + (x1 - x0) * std::min(t, zero), bb = x0 + (x1 - x0) * std::max(t, zero);
            if (bb - a > 0.5f) g.Draw().Rect(a, cy - hh, bb, cy + hh, ink, ink, hh);
        };
        auto pct = [&](int col, int row, const char* label, float share) {
            const float x = blk.x + col * (colW + colGap), y = blk.y + row * rowH;
            char t[16];
            std::snprintf(t, sizeof(t), "%.0f %%", (double)share * 100.0);
            g.TextIn({x, y, colW * 0.6f, rowH}, label, th.text, 0.0f, th.smallSize);
            g.TextIn({x, y, colW, rowH}, t, share < 0.9f ? warn : th.textBright, 1.0f, th.smallSize);
        };
        static const char* const kZ[] = {"Residential", "Commercial", "Office", "Industrial"};
        static const Color kC[] = {Rgba(110, 200, 90), Rgba(90, 160, 240), Rgba(170, 120, 230), Rgba(240, 200, 60)};
        head(0, 0, "DEMAND");
        for (int z = 0; z < 4; ++z) bar(0, 1 + z, kZ[z], st.demand[z], -1.0f, 1.0f, kC[z]);
        head(0, 5, "UTILITIES (residents served)");
        pct(0, 6, "Electricity", st.powerShare);
        pct(0, 7, "Water", st.waterShare);
        pct(0, 8, "Sewage", st.sewageShare);
        head(1, 0, "SERVICE COVERAGE");
        static const char* const kS[] = {"Fire & Rescue", "Police", "Healthcare", "Education", "Parks", "Transit"};
        for (int k = 0; k < 6; ++k) bar(1, 1 + k, kS[k], st.coverage[k], 0.0f, 255.0f, th.accent);
        head(1, 7, "LAND VALUE");
        bar(1, 8, "Average", st.landValue, 0.0f, 255.0f, Rgba(200, 170, 110));
        std::snprintf(b, sizeof(b), "%s up, %s down, %s abandoned", Count(st.levelUps).c_str(), Count(st.levelDowns).c_str(), Count(st.abandoned).c_str());
        Pair(g, "Building levels", b, th.textBright);
        std::snprintf(b, sizeof(b), "%s roads in use, %s slow", Count(st.trips).c_str(), Count(st.slowRoads).c_str());
        Pair(g, "Peak hour traffic", b, st.slowRoads > 0 ? warn : th.textBright);
    } else if (people) {
        PairN(g, "Residents", st.population);
        std::snprintf(b, sizeof(b), "%s / %s / %s", Count(st.kids).c_str(), Count(st.adults).c_str(), Count(st.seniors).c_str());
        Pair(g, "Children / adults / seniors", b, th.textBright);
        PairN(g, "Working", st.working);
        const float unemp = st.adults > 0 ? (float)st.unemployed / (float)st.adults : 0.0f;
        std::snprintf(b, sizeof(b), "%s (%.1f %%)", Count(st.unemployed).c_str(), (double)unemp * 100.0);
        Pair(g, "Unemployed adults", b, unemp > 0.1f ? warn : th.textBright);
        std::snprintf(b, sizeof(b), "%s filled of %s", Count(st.jobsFilled).c_str(), Count(st.jobs).c_str());
        Pair(g, "Jobs", b, th.textBright);
        static const char* const kJ[] = {"  Industry", "  Commercial", "  Office"};
        for (int k = 0; k < 3; ++k) {
            std::snprintf(b, sizeof(b), "%s / %s", Count(st.filledByKind[k]).c_str(), Count(st.jobsByKind[k]).c_str());
            Pair(g, kJ[k], b, th.text);
        }
        std::snprintf(b, sizeof(b), "%s homes", Count(st.homes).c_str());
        Pair(g, "Housing", b, th.textBright);
        std::snprintf(b, sizeof(b), "%s / %s / %s", Count(st.edu[0]).c_str(), Count(st.edu[1]).c_str(), Count(st.edu[2]).c_str());
        Pair(g, "Education (basic/skilled/higher)", b, th.text);
        std::snprintf(b, sizeof(b), "%s / %s / %s / %s", Count(st.wealth[0]).c_str(), Count(st.wealth[1]).c_str(), Count(st.wealth[2]).c_str(), Count(st.wealth[3]).c_str());
        Pair(g, "Wealth (low to rich)", b, th.text);
        g.Text(TextStyle::Muted, "Tap a zoned building to meet its residents.");
    } else if (notes) {
        if (st.notes.empty()) g.Text(TextStyle::Muted, "Nothing to report yet.");
        const int n = (int)st.notes.size();
        for (int i = n - 1; i >= 0 && i >= n - 14; --i) g.Text(TextStyle::Small, "%s", st.notes[(size_t)i].c_str());
    } else { // statistics
        g.Text(TextStyle::Muted, "Population, last %d days", (int)st.pop.size());
        Spark(g, st.pop, Rgba(120, 210, 120), "The chart starts after the first game day");
        g.Text(TextStyle::Muted, "Treasury, last %d days", (int)st.balance.size());
        Spark(g, st.balance, Rgba(110, 180, 255), "The chart starts after the first game day");
        PairN(g, "Residents", st.population);
        std::snprintf(b, sizeof(b), "%s$%s / day", st.net < 0 ? "-" : "+", Count(st.net < 0 ? -st.net : st.net).c_str());
        Pair(g, "Net income", b, st.net < 0 ? Rgba(235, 90, 80) : Rgba(120, 210, 120));
    }
    g.NextWidth(120.0f);
    const bool close = g.Button("Close##info", Icon::Close);
    g.EndPanel();
    return close ? 2 : 1;
}

} // namespace gui
} // namespace tbd
