// The city game's HUD, laid out like Cities: Skylines II:
//   bottom toolbar   one row of icon buttons (zoning, roads, electricity, water, garbage, ...), tooltips on hover;
//                    working ones open their tool, the rest open a "Not completed yet" panel;
//   status bar       under it: pause / date and time / speed, temperature and season, the city name, population, money;
//   right column     city info, citizens, notes, advisor (all "not completed");
//   Purchase Map Tiles  a dialog opened from the toolbar's map button (the numbers are zero: tiles are not implemented).
// The tools themselves (src/gui/*_toolbar.h) only draw their options and picker panels; this file launches them.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "zone_toolbar.h"

namespace tbd {
namespace gui {

// What a toolbar button launches. Hud* >= 0 && implemented: main.cpp opens that tool.
enum class HudTool : int { Zoning, Roads, Electricity, Water, Garbage, Transport, Industry, Terrain, Fire, Health, Police, Education, Comms, Count };

struct HudItem {
    const char* name;
    Icon icon;
    int tool;               // HudTool, or -1 when the feature is not completed
    const char* blurb;      // what it will do (shown in the not-completed panel)
};

inline const HudItem* HudItems(int& n) {
    static const HudItem k[] = {
        {"Terrain", Icon::Raise, (int)HudTool::Terrain, "Raise, dig and flatten the ground."},
        {"Zoning", Icon::Zones, (int)HudTool::Zoning, "Paint residential, commercial, office and industrial zones."},
        {"Roads", Icon::Roads, (int)HudTool::Roads, "Build roads, highways, paths and railway track."},
        {"Electricity", Icon::Bolt, (int)HudTool::Electricity, "Power plants, transformers and power lines."},
        {"Water & Sewage", Icon::Drop, (int)HudTool::Water, "Water towers, pumps, treatment and pipes."},
        {"Garbage Management", Icon::Drain, (int)HudTool::Garbage, "Landfills, incinerators and recycling."},
        {"Specialized Industry", Icon::Utilities, (int)HudTool::Industry, "Farming, forestry, oil and ore hubs."},
        {"Transportation", Icon::Transport, (int)HudTool::Transport, "Depots, stations, harbours and airports."},
        {"Healthcare & Deathcare", Icon::Health, (int)HudTool::Health, "Clinics and hospitals; the Deathcare tab has cemeteries and crematoriums."},
        {"Education & Research", Icon::Education, (int)HudTool::Education, "Schools, colleges, universities and libraries."},
        {"Fire & Rescue", Icon::Fire, (int)HudTool::Fire, "Fire stations and rescue helicopters."},
        {"Police & Administration", Icon::Police, (int)HudTool::Police, "Police stations, prisons and city hall."},
        {"Parks & Recreation", Icon::Park, -1, "Parks, plazas, sports and tourist attractions."},
        {"Communications", Icon::Comms, (int)HudTool::Comms, "Post offices, telecom and broadcasting."},
        {"Economy", Icon::Economy, -1, "Taxes, budget, loans and service funding."},
        {"Map Tiles", Icon::MapTiles, -2, "Buy neighbouring map tiles to expand the city."},
        {"Statistics", Icon::Chart, -1, "Graphs of population, jobs, traffic and finances."},
    };
    n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

inline const float* HudSizeValues() { static const float k[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}; return k; }

struct CityHud {
    int request = -1;          // out: a working tool the user clicked this frame (HudTool), else -1
    int notDone = -1;          // index into HudItems of the open "not completed" panel, -1 none
    const char* notDoneName = nullptr; // for the right column / status bar items that are not in HudItems
    const char* notDoneBlurb = nullptr;
    bool options = false;      // the Options panel
    int sizeIdx = 1;           // interface size: index into kHudSizes
    int slot = 0;              // the save slot picked in Options
    int gameAction = 0;        // out: 1 save, 2 load, 3 a new random map (the host consumes it)
    bool tiles = false;        // the Purchase Map Tiles dialog
    bool terrainOpen = false;  // in: the terrain panel is shown (highlights its button)
    int selected = -1;         // in: HudTool of the tool that is open now (highlights its button)
    std::string cityName = "Shortville";
    // in, from the city sim (city_sim.inl SimHudFill); -1 while the sim has nothing to show
    long long population = -1, money = -1; // money in whole currency units
    long long adults = 0, working = 0, jobs = 0;
    // the Budget panel (Economy button, or the money cell): in/out with the city sim (city_sim.inl SimHudFill / SimHudRead)
    bool budget = false;
    float tax[4] = {10, 10, 10, 10};                  // percent: residential, commercial, office, industrial
    float funding[6] = {100, 100, 100, 100, 100, 100}; // percent: fire, police, health, school, park, transit
    long long incomeDay[4] = {}, upkeepDay = 0;        // whole currency, the last game day
    // the residents of the selected zoned building (tracked people, S6); the panel closes with `citizensClosed`
    std::string citizenTitle;
    std::vector<std::string> citizens;
    std::string citizensClosedFor; // the title the player closed: the panel stays shut until another building is picked
    int milestone = 1;
    bool Pending() const { return notDoneName != nullptr; }
    void ShowNotDone(const char* name, const char* blurb) { notDoneName = name, notDoneBlurb = blurb, tiles = false; }
    void Dismiss() { notDoneName = notDoneBlurb = nullptr, tiles = false, budget = false; }
};

inline const char* HudSeason(int month) { return month == 11 || month <= 1 ? "Winter" : month <= 4 ? "Spring" : month <= 7 ? "Summer" : "Autumn"; }

inline void DrawSun(DrawList& dl, float cx, float cy, float r, Color c) {
    dl.Circle(cx, cy, r * 0.5f, c);
    for (int i = 0; i < 8; ++i) {
        const float a = i * 0.7853982f;
        dl.Line(cx + std::cos(a) * r * 0.75f, cy + std::sin(a) * r * 0.75f, cx + std::cos(a) * r, cy + std::sin(a) * r, 1.4f, c);
    }
}

// A dark rounded cell of the status bar with `text` in it. Returns the rectangle.
inline void HudCell(Context& g, const Rect& r, const char* text, Color ink, float align = 0.5f) {
    const Theme& t = g.Style();
    g.Draw().Rect(r.x, r.y, r.x + r.w, r.y + r.h, t.well, t.well, t.widgetRadius, t.border, t.wellBorder);
    g.TextIn({r.x + t.textPad * 0.5f, r.y, r.w - t.textPad, r.h}, text, ink, align, t.fontSize);
}

// Draws the HUD. `lift`: unscaled distance of the status bar from the bottom edge (clear of the toast row). Returns the
// unscaled height the bottom bars take, so the host can stack the tools' panels above them.
inline float CityHudFrame(Context& g, CityHud& h, GameClock& clock, float lift) {
    using namespace road_ui;
    const Theme& th = g.Style();
    const float s = g.Scale();
    const float viewW = g.In().viewW / s;
    static Tip tip;
    tip.on = false;
    h.request = -1;
    int n = 0;
    const HudItem* items = HudItems(n);

    // ------------------------------------------------------------ status bar (bottom)
    const float statusH = 44.0f, toolbarH = 62.0f;
    g.BeginPanel("##hud status", Anchor::BottomCenter, 0.0f, lift, viewW, statusH, 5.0f);
    {
        const float cell = std::round(34.0f * s), gap = std::round(5.0f * s);
        const Rect row = g.Reserve(cell, viewW * s - 10.0f * s);
        float x = row.x;
        // pause, date and time, speed
        const Rect pauseR = {x, row.y, cell, cell};
        if (IconBox(g, tip, "##pause", pauseR, clock.paused ? Icon::Play : Icon::Pause, clock.paused, clock.paused ? "Resume (Space)" : "Pause (Space)")) clock.paused = !clock.paused;
        x += cell + gap;
        const int day0 = clock.Day() - 1;
        const int month = (day0 / 30) % 12, year = 2023 + day0 / 360;
        static const char* const kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02d:%02d  %s %d", clock.Hour(), clock.Minute(), kMonth[month], year);
        const float dateW = std::round(150.0f * s);
        HudCell(g, {x, row.y, dateW, cell}, buf, clock.paused ? th.muted : th.textBright);
        x += dateW + gap;
        static const Icon kSpeed[] = {Icon::Speed1, Icon::Speed2, Icon::Speed3};
        static const char* const kTip[] = {"Speed: normal", "Speed: fast", "Speed: fastest"};
        if (IconBox(g, tip, "##speed", {x, row.y, cell, cell}, kSpeed[clock.speed], clock.speed > 0, kTip[clock.speed])) {
            clock.speed = (clock.speed + 1) % 3;
            clock.paused = false;
        }
        x += cell + gap * 3;
        // temperature and season
        const char* season = HudSeason(month);
        const float base = month == 11 || month <= 1 ? 2.0f : month <= 4 ? 12.0f : month <= 7 ? 22.0f : 13.0f;
        const float temp = base + 5.0f * std::sin(((clock.Hour() + clock.Minute() / 60.0f) - 9.0f) / 24.0f * 6.2831853f);
        const float weatherW = std::round(230.0f * s);
        {
            const Rect r = {x, row.y, weatherW, cell};
            g.Draw().Rect(r.x, r.y, r.x + r.w, r.y + r.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
            DrawSun(g.Draw(), r.x + cell * 0.5f, r.y + r.h * 0.5f, cell * 0.28f, Rgba(255, 205, 90));
            std::snprintf(buf, sizeof(buf), "%.0f C", temp);
            g.TextIn({r.x + cell, r.y, std::round(70.0f * s), r.h}, buf, th.textBright, 0.0f, th.fontSize);
            g.TextIn({r.x + cell + std::round(70.0f * s), r.y, r.w - cell - std::round(70.0f * s), r.h}, season, th.text, 0.5f, th.fontSize);
        }
        x += weatherW + gap;
        // right side, built from the right edge: money, population; the city name takes the middle
        const float moneyW = std::round(190.0f * s), popW = std::round(150.0f * s);
        const float right = row.x + row.w;
        const Rect moneyR = {right - moneyW, row.y, moneyW, cell}, popR = {moneyR.x - gap - popW, row.y, popW, cell};
        const float nameW = std::round(240.0f * s);
        const Rect nameR = {std::max(x, std::round((row.x + row.w * 0.5f) - nameW * 0.5f)), row.y, nameW, cell};
        if (nameR.x + nameR.w < popR.x) {
            if (g.ClickArea("##cityname", nameR)) h.ShowNotDone("City Name", "Renaming the city and city details are not completed yet.");
            HudCell(g, nameR, h.cityName.c_str(), th.textBright);
        }
        bool popHot = false;
        if (g.ClickArea("##population", popR, &popHot)) h.ShowNotDone("Population", "Population statistics are not completed yet; the count comes from the city sim.");
        if (h.population >= 0) {
            std::string pop = std::to_string(h.population);
            for (int i = (int)pop.size() - 3; i > 0; i -= 3) pop.insert((size_t)i, ",");
            HudCell(g, popR, pop.c_str(), th.textBright);
            if (popHot) {
                tip.on = true;
                tip.x = popR.x + popR.w * 0.5f, tip.y = popR.y - std::round(16.0f * s);
                std::snprintf(tip.text, sizeof(tip.text), "%lld adults, %lld working, %lld jobs", h.adults, h.working, h.jobs);
            }
        } else {
            HudCell(g, popR, "-", th.muted);
        }
        g.Draw().Circle(popR.x + std::round(16.0f * s), popR.y + popR.h * 0.5f, 3.0f * s, th.muted);
        if (g.ClickArea("##money", moneyR)) {
            const bool was = h.budget;
            h.Dismiss();
            h.budget = !was;
        }
        if (h.money != -1 && h.population >= 0) {
            const long long m = h.money;
            std::string v = std::to_string(m < 0 ? -m : m);
            for (int i = (int)v.size() - 3; i > 0; i -= 3) v.insert((size_t)i, ",");
            v = (m < 0 ? "-$" : "$") + v;
            HudCell(g, moneyR, v.c_str(), m < 0 ? Rgba(235, 90, 80) : th.textBright);
        } else {
            HudCell(g, moneyR, "-", th.muted);
        }
        DrawIcon(g.Draw(), Icon::Economy, moneyR.x + std::round(16.0f * s), moneyR.y + moneyR.h * 0.5f, std::round(16.0f * s), th.iconStroke, th.muted);
    }
    g.EndPanel();

    // ------------------------------------------------------------ toolbar (above the status bar)
    g.BeginPanel("##hud toolbar", Anchor::BottomCenter, 0.0f, lift + statusH + 4.0f, 0.0f, toolbarH, 6.0f);
    {
        const float size = std::round(48.0f * s), gap = std::round(6.0f * s), sep = std::round(14.0f * s);
        const int groupAfter[] = {2, 7, 14}; // thin rules between: building, city services, management
        float total = n * size + (n - 1) * gap;
        for (int a : groupAfter) total += sep - gap + (a >= 0 ? 0 : 0);
        const Rect row = g.Reserve(size, total);
        float x = row.x;
        // left: the milestone / city badge
        for (int i = 0; i < n; ++i) {
            const HudItem& it = items[i];
            const bool open = it.tool >= 0 && (it.tool == (int)HudTool::Terrain ? h.terrainOpen : it.tool == h.selected);
            const bool economy = it.tool == -1 && std::strcmp(it.name, "Economy") == 0; // the Budget panel (S4)
            const bool on = open || (it.tool == -1 && h.notDone == i) || (it.tool == -2 && h.tiles) || (economy && h.budget);
            char id[24];
            std::snprintf(id, sizeof(id), "##hudbtn%d", i);
            if (IconBox(g, tip, id, {x, row.y, size, size}, it.icon, on, it.name, it.tool == -1 && !economy)) {
                if (economy) {
                    const bool was = h.budget;
                    h.Dismiss();
                    h.notDone = -1;
                    h.budget = !was;
                } else if (it.tool >= 0) h.request = it.tool, h.Dismiss();
                else if (it.tool == -2) {
                    const bool was = h.tiles;
                    h.Dismiss();
                    h.tiles = !was;
                    h.notDone = -1;
                } else {
                    const bool was = h.notDone == i;
                    h.Dismiss();
                    if (!was) h.ShowNotDone(it.name, it.blurb), h.notDone = i;
                }
            }
            x += size + gap;
            for (int a : groupAfter) {
                if (i == a) {
                    const float lx = x - gap * 0.5f + sep * 0.0f;
                    g.Draw().Rect(lx, row.y + size * 0.15f, lx + std::max(1.0f, th.border), row.y + size * 0.85f, th.rule, 0.0f);
                    x += sep - gap;
                }
            }
        }
    }
    g.EndPanel();

    // ------------------------------------------------------------ right column
    g.BeginPanel("##hud right", Anchor::TopRight, 8.0f, 8.0f, 60.0f, 0.0f, 6.0f);
    {
        const float size = std::round(48.0f * s);
        struct Side { const char* id; Icon icon; const char* name; const char* blurb; };
        static const Side k[] = {
            {"##side0", Icon::Info, "City Information", "Happiness, health, crime and other city-wide readings are not completed yet."},
            {"##side1", Icon::Citizens, "Population Info", "Citizen lists and demographics are not completed yet."},
            {"##side2", Icon::Notes, "Notifications", "The notification log is not completed yet."},
            {"##side3", Icon::Chat, "Advisor", "The advisor is not completed yet."},
        };
        if (IconBox(g, tip, "##side4", g.Reserve(size, size), Icon::Gear, h.options, "Options")) {
            const bool was = h.options;
            h.Dismiss();
            h.options = !was;
        }
        for (const Side& e : k)
            if (IconBox(g, tip, e.id, g.Reserve(size, size), e.icon, h.notDoneName == e.name, e.name)) {
                const bool was = h.notDoneName == e.name;
                h.Dismiss();
                if (!was) h.ShowNotDone(e.name, e.blurb);
            }
    }
    g.EndPanel();

    // ------------------------------------------------------------ Options
    if (h.options) {
        g.BeginPanel("Options", Anchor::TopRight, 76.0f, 8.0f, 360.0f, 0.0f, 12.0f);
        static const char* const kNames[] = {"75%", "100%", "125%", "150%", "200%"};
        g.Text(TextStyle::Muted, "Interface size");
        g.Choice("Interface size", kNames, 5, &h.sizeIdx);
        g.Spacing(4.0f);
        static const char* const kSlots[] = {"Slot 1", "Slot 2", "Slot 3"};
        g.Text(TextStyle::Muted, "Game save");
        g.Choice("Save slot", kSlots, 3, &h.slot);
        if (g.Button("Save Game")) h.gameAction = 1;
        if (g.Button("Load Game")) h.gameAction = 2;
        if (g.Button("New Random Map")) h.gameAction = 3;
        g.EndPanel();
    }

    // ------------------------------------------------------------ the selected building's residents (tracked people)
    if (!h.citizens.empty() && h.citizensClosedFor != h.citizenTitle) {
        g.BeginPanel("##hud citizens", Anchor::TopLeft, 8.0f, 120.0f, 420.0f, 0.0f, 10.0f);
        g.Text(TextStyle::Bright, "%s", h.citizenTitle.c_str());
        for (const std::string& line : h.citizens) g.Text(TextStyle::Normal, "%s", line.c_str());
        g.NextWidth(100.0f);
        if (g.Button("Close", Icon::Close)) h.citizensClosedFor = h.citizenTitle;
        g.EndPanel();
    }

    // ------------------------------------------------------------ Budget (the city sim's economy)
    if (h.budget) {
        g.BeginPanel("Budget", Anchor::TopCenter, 0.0f, 60.0f, 460.0f, 0.0f, 12.0f); // from the top: it is tall
        long long income = 0;
        for (long long v : h.incomeDay) income += v;
        g.Text(TextStyle::Bright, "Income %lld / day   Upkeep %lld / day   Net %+lld", income, h.upkeepDay, income - h.upkeepDay);
        g.Text(TextStyle::Muted, "Taxes: residential %lld, commercial %lld, office %lld, industrial %lld", h.incomeDay[0], h.incomeDay[1], h.incomeDay[2], h.incomeDay[3]);
        g.Spacing(4.0f);
        g.Text(TextStyle::Muted, "Tax rates (higher rates lower demand)");
        static const char* const kTax[] = {"Residential", "Commercial", "Office", "Industrial"};
        for (int c = 0; c < 4; ++c) g.SliderFloat(kTax[c], &h.tax[c], 0.0f, 30.0f, "%.0f %%");
        g.Spacing(4.0f);
        g.Text(TextStyle::Muted, "Service funding (scales coverage radius and upkeep)");
        static const char* const kSvc[] = {"Fire & Rescue", "Police", "Healthcare", "Education", "Parks", "Transit"};
        for (int k = 0; k < 6; ++k) g.SliderFloat(kSvc[k], &h.funding[k], 50.0f, 150.0f, "%.0f %%");
        g.NextWidth(120.0f);
        if (g.Button("Close", Icon::Close)) h.budget = false;
        g.EndPanel();
    }

    // ------------------------------------------------------------ "not completed" panel, above the toolbar
    if (h.notDoneName) {
        g.BeginPanel("##hud notdone", Anchor::BottomCenter, 0.0f, lift + statusH + toolbarH + 14.0f, 440.0f, 0.0f, 12.0f);
        g.Text(TextStyle::Bright, "%s", h.notDoneName);
        g.Text(TextStyle::Muted, "%s", h.notDoneBlurb ? h.notDoneBlurb : "");
        g.Spacing(2.0f);
        const Rect r = g.Reserve(std::round(30.0f * s), 0.0f);
        const Color c = Rgba(255, 190, 70);
        g.Draw().Rect(r.x, r.y, r.x + r.w, r.y + r.h, Rgba(60, 46, 20, 230), Rgba(60, 46, 20, 230), th.widgetRadius, th.border, Rgba(150, 110, 40));
        g.TextIn(r, "NOT COMPLETED YET", c, 0.5f, th.fontSize, true);
        g.NextWidth(120.0f);
        if (g.Button("Close", Icon::Close)) h.Dismiss(), h.notDone = -1;
        g.EndPanel();
    }

    // ------------------------------------------------------------ Purchase Map Tiles
    if (h.tiles) {
        g.BeginPanel("Purchase Map Tiles", Anchor::TopCenter, 0.0f, 60.0f, 700.0f, 0.0f, 12.0f);
        static const struct { const char* name; const char* unit; Color c; } k[] = {
            {"BUILDABLE AREA", "m2", Rgba(90, 200, 110)}, {"GROUNDWATER", "m3", Rgba(70, 160, 240)},
            {"FERTILE LAND", "m2", Rgba(220, 190, 80)},  {"WOOD", "kg", Rgba(110, 190, 90)},
            {"OIL DEPOSITS", "kg", Rgba(160, 160, 170)}, {"ORE DEPOSITS", "kg", Rgba(190, 120, 90)},
        };
        for (int row = 0; row < 3; ++row) {
            const Rect r = g.Reserve(std::round(22.0f * s), 0.0f);
            const float half = r.w * 0.5f, colGap = std::round(20.0f * s);
            for (int c = 0; c < 2; ++c) {
                const auto& e = k[row * 2 + c];
                const float x0 = r.x + c * half + (c ? colGap * 0.5f : 0.0f), w = half - colGap * 0.5f;
                g.Draw().Circle(x0 + 6.0f * s, r.y + r.h * 0.5f, 4.0f * s, e.c);
                g.TextIn({x0 + 20.0f * s, r.y, w * 0.6f, r.h}, e.name, th.text, 0.0f, th.smallSize);
                char v[32];
                std::snprintf(v, sizeof(v), "0 %s", e.unit);
                g.TextIn({x0 + w * 0.5f, r.y, w * 0.5f, r.h}, v, th.textBright, 1.0f, th.smallSize);
            }
        }
        g.Spacing(4.0f);
        const Rect r = g.Reserve(std::round(34.0f * s), 0.0f);
        const float bw = std::round(150.0f * s), cw = std::round(110.0f * s);
        const Rect purchase = {r.x + r.w - bw, r.y, bw, r.h}, close = {purchase.x - 8.0f * s - cw, r.y, cw, r.h};
        g.TextIn({r.x, r.y, close.x - r.x - 8.0f * s, r.h}, "No selection - map tiles are not completed yet", Rgba(255, 190, 70), 0.0f, th.fontSize);
        g.Draw().Rect(purchase.x, purchase.y, purchase.x + purchase.w, purchase.y + purchase.h, th.well, th.well, th.widgetRadius, th.border, th.wellBorder);
        g.TextIn(purchase, "PURCHASE", th.muted, 0.5f, th.fontSize);
        bool hot = false, held = false;
        if (g.ClickArea("##tiles close", close, &hot, &held)) h.Dismiss();
        g.Draw().Rect(close.x, close.y, close.x + close.w, close.y + close.h, hot ? th.widgetHoverTop : th.widgetTop, hot ? th.widgetHoverBottom : th.widgetBottom, th.widgetRadius, th.border, th.widgetBorder);
        g.TextIn(close, "CLOSE", th.textBright, 0.5f, th.fontSize);
        g.EndPanel();
    }

    if (tip.on) g.Chip(tip.x, tip.y, tip.text);
    return statusH + toolbarH + 8.0f;
}

} // namespace gui
} // namespace tbd
