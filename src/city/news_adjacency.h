// What a newly opened building's neighbours say about it, for the city radio (ported from the sim-budget-milestones
// branch's event bus, sim_events.h, onto ios-metal's radio, city_radio.inl). Header only, headless (tests\sim_news_test).
//   BuildingKind      a catalog name ("Elementary School", "Coal Power Plant") to the kind key the "adj:<a>+<b>" lines of
//                     news_lines.json use ("school", "power_plant"); "" for buildings the news has no pairs for.
//   PickOpeningStory  one story per opening, never more: a bad neighbour pair first (the anchor roasts a school next to a
//                     factory), then the building's own opening trigger, then a good neighbour pair. A pair counts only
//                     when news_lines.json has a line for it. `voiced` says which pairs have recorded clips: a bad pair
//                     without one goes to the written feed while the own opening clip plays, so one event is still one
//                     clip and one headline (no filler, no second clip).
#pragma once

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "news_feed.h"
#include "news_gate.h" // how often any of it may run (city_radio.inl)

namespace city {

inline const char* BuildingKind(const char* name) {
    if (!name) return "";
    auto has = [name](const char* s) { return std::strstr(name, s) != nullptr; };
    if (has("School") || has("College") || has("University")) return "school";
    if (has("Hospital") || has("Clinic") || has("Medical") || has("Urgent Care")) return "hospital";
    if (has("Stadium")) return "stadium";
    if (has("Park") || has("Plaza")) return "park";
    if (has("Fire")) return "fire_station";
    if (has("Police") || has("Jail")) return "police";
    if (has("Tram Depot")) return "tram_depot";
    if (has("Taxi")) return "taxi_depot";
    if (has("Bus Depot")) return "bus_depot";
    if (has("Railway Depot")) return "rail_depot";
    if (has("Airport") || has("Airfield")) return "airport";
    if (has("Power Plant") || has("Power Station") || has("Wind Turbine")) return "power_plant";
    if (has("Water Tower")) return "water_tower";
    if (has("Landfill")) return "landfill";
    return "";
}

struct OpeningStory {
    std::string clip;     // the trigger to play (and to count toward the week), "" for none
    std::string headline; // the trigger whose written line goes into the feed, "" for none
};

// kind: BuildingKind of the new building; own: its own opening trigger ("" if it has none); neighbours: the kind keys
// of what stands near it (zones: "residential", "commercial", "industry"), each once.
inline OpeningStory PickOpeningStory(const std::string& kind, const std::string& own, const std::vector<std::string>& neighbours,
                                     const NewsFeed& feed, const std::set<std::string>& voiced) {
    std::string bad, good;
    if (!kind.empty())
        for (const std::string& n : neighbours) {
            if (n.empty() || n == kind) continue;
            for (const std::string& t : {"adj:" + kind + "+" + n, "adj:" + n + "+" + kind}) {
                if (!feed.Has(t)) continue;
                const int tone = feed.Tone(t);
                if (tone < 0 && bad.empty()) bad = t;
                else if (tone >= 0 && good.empty()) good = t; // mixed pairs count as the milder story
            }
            if (!bad.empty()) break;
        }
    if (!bad.empty()) return voiced.count(bad) ? OpeningStory{bad, bad} : OpeningStory{own, bad};
    if (!own.empty()) return {own, own};
    if (!good.empty()) return {voiced.count(good) ? good : std::string(), good};
    return {};
}

} // namespace city
