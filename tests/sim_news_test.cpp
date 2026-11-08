// Headless checks for the radio's opening stories (src/city/news_adjacency.h, src/city/news_feed.h):
//   catalog names map to the news kinds of the "adj:<a>+<b>" lines; NewsFeed::Has / Tone; one story per opening (a bad
//   neighbour pair beats the opening line, which beats a good pair); a bad pair without a recorded clip goes to the
//   written feed while the own opening clip plays; nothing at all when there is nothing to say. The gate
//   (src/city/news_gate.h): routine placements never make the news, the first of a kind does, the global gap
//   (game days and real seconds), per-trigger / per-kind / roast cooldowns, a held Major story, a busy player's 60 days.
// Build and run: tests\sim_news_test.bat
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "city/news_adjacency.h"

using namespace city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main(int argc, char** argv) {
    CHECK(std::string(BuildingKind("Elementary School")) == "school");
    CHECK(std::string(BuildingKind("University")) == "school");
    CHECK(std::string(BuildingKind("General Hospital")) == "hospital");
    CHECK(std::string(BuildingKind("Family Clinic")) == "hospital");
    CHECK(std::string(BuildingKind("Compact Bus Depot")) == "bus_depot");
    CHECK(std::string(BuildingKind("Compact Tram Depot")) == "tram_depot");
    CHECK(std::string(BuildingKind("Railway Depot")) == "rail_depot");
    CHECK(std::string(BuildingKind("Fire Headquarters")) == "fire_station");
    CHECK(std::string(BuildingKind("Police Station")) == "police");
    CHECK(std::string(BuildingKind("Coal Power Plant")) == "power_plant");
    CHECK(std::string(BuildingKind("Wind Turbine")) == "power_plant");
    CHECK(std::string(BuildingKind("Water Tower")) == "water_tower");
    CHECK(std::string(BuildingKind("Sewage Outlet")).empty());
    CHECK(std::string(BuildingKind("Small Antenna")).empty());
    CHECK(std::string(BuildingKind(nullptr)).empty());

    NewsFeed f;
    f.Add({"adj:school+industry", "bad", "a"});
    f.Add({"adj:school+industry", "bad", "b"});
    f.Add({"adj:school+industry", "good", "c"});
    f.Add({"adj:residential+school", "good", "d"});
    f.Add({"adj:fire_station+commercial", "bad", "e"});
    f.Add({"adj:park+residential", "good", "f"});
    f.Add({"adj:park+residential", "bad", "g"});
    f.Add({"school_built", "good", "h"});
    CHECK(f.Has("school_built") && !f.Has("nothing"));
    CHECK(f.Tone("adj:school+industry") < 0 && f.Tone("adj:residential+school") > 0 && f.Tone("adj:park+residential") == 0 && f.Tone("nothing") == 0);

    const std::set<std::string> voiced = {"adj:school+industry"};
    const std::set<std::string> none;
    // a school beside industry: the roast beats the ribbon cutting, voiced, so it plays
    OpeningStory s = PickOpeningStory("school", "school_built", {"residential", "industry"}, f, voiced);
    CHECK(s.clip == "adj:school+industry" && s.headline == "adj:school+industry");
    // the same pair without a clip: the opening clip plays, the roast is the written line
    s = PickOpeningStory("school", "school_built", {"industry"}, f, none);
    CHECK(s.clip == "school_built" && s.headline == "adj:school+industry");
    // among homes only (a good pair): the plain opening
    s = PickOpeningStory("school", "school_built", {"residential"}, f, voiced);
    CHECK(s.clip == "school_built" && s.headline == "school_built");
    // the pair is found in either order
    s = PickOpeningStory("residential", "", {"school"}, f, none);
    CHECK(s.clip.empty() && s.headline == "adj:residential+school");
    // no own opening and a bad pair without a clip: a written line only (no clip, so no airtime spent)
    s = PickOpeningStory("fire_station", "", {"commercial"}, f, none);
    CHECK(s.clip.empty() && s.headline == "adj:fire_station+commercial");
    // a mixed pair counts as the milder story
    s = PickOpeningStory("park", "", {"residential"}, f, none);
    CHECK(s.clip.empty() && s.headline == "adj:park+residential");
    // nothing to say: nothing at all
    s = PickOpeningStory("", "", {"residential"}, f, voiced);
    CHECK(s.clip.empty() && s.headline.empty());
    s = PickOpeningStory("stadium", "", {"industry"}, f, voiced);
    CHECK(s.clip.empty() && s.headline.empty());
    // a kind next to its own kind is no story
    s = PickOpeningStory("school", "school_built", {"school"}, f, voiced);
    CHECK(s.clip == "school_built");

    // the real file: it loads, and the pairs the radio has clips for are bad news in it
    std::string json;
    {
        std::ifstream in(argc > 1 ? argv[1] : "src/city/news_lines.json", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        json = ss.str();
    }
    NewsFeed real;
    const int lines = LoadNewsLines(real, json);
    CHECK(lines > 100);
    for (const char* t : {"adj:residential+industry", "adj:residential+landfill", "adj:school+industry"}) CHECK(real.Has(t) && real.Tone(t) < 0);
    s = PickOpeningStory(BuildingKind("Elementary School"), "school_built", {"industry"}, real, {"adj:school+industry"});
    CHECK(s.clip == "adj:school+industry");

    // ---- the gate (news_gate.h): the news is rare, not "every time I do the thing"
    {
        using namespace news_tuning;
        const double day = kDayMinutes;
        auto story = [](const char* trigger, const char* kind = "", bool opening = false, bool big = false, bool roast = false) {
            NewsStory st;
            st.trigger = trigger, st.kind = kind, st.opening = opening, st.big = big, st.roast = roast;
            return st;
        };
        NewsGate gate;
        double t = 10 * day, rs = 1000.0;
        // routine: roads, bus stops never make the news
        CHECK(!gate.Offer(story("road_bulldozed"), t, rs));
        CHECK(!gate.Offer(story("bus_stop_opened"), t, rs));
        CHECK(gate.Tier(story("road_bulldozed")) == NewsTier::Skip);
        // the first school is news; a second small school right after (or much later) is not
        CHECK(gate.Offer(story("school_built", "school", true), t, rs));
        CHECK(!gate.Offer(story("school_built", "school", true), t + 30 * day, rs + 9000));
        CHECK(gate.Tier(story("school_built", "school", true)) == NewsTier::Skip);
        // a big one of a known kind may run again, but only every kSameKindDays
        CHECK(!gate.Offer(story("hospital_built", "school", true, true), t + 3 * day, rs + 600)); // kind cooldown
        // the global gap: anything within kAnyGapDays game days, or within kAnyGapRealSec rs seconds, is dropped
        CHECK(!gate.Offer(story("tax_raised"), t + 1 * day, rs + 600));          // 1 game day later
        CHECK(!gate.Offer(story("tax_raised"), t + 3 * day, rs + 60));           // 3 days, but a minute of rs time (fast clock)
        CHECK(gate.Offer(story("tax_raised"), t + 3 * day, rs + kAnyGapRealSec)); // both passed
        t += 3 * day, rs += kAnyGapRealSec;
        // the same trigger waits kSameTriggerDays even when the gap is open
        CHECK(!gate.Offer(story("tax_raised"), t + 5 * day, rs + 1000));
        CHECK(gate.Offer(story("tax_lowered"), t + 5 * day, rs + 1000));
        t += 5 * day, rs += 1000;
        // a big repeat after the kind cooldown: allowed
        CHECK(gate.Offer(story("school_built", "school", true, true), t + kSameKindDays * day, rs + 1000));
        t += kSameKindDays * day, rs += 1000;
        // a clearly bad neighbour pair: a roast, rate limited (kRoastGapDays) on top of the gap
        CHECK(gate.Offer(story("adj:residential+industry", "factory", false, false, true), t + 3 * day, rs + 1000));
        t += 3 * day, rs += 1000;
        CHECK(!gate.Offer(story("adj:school+industry", "power_plant", false, false, true), t + 3 * day, rs + 1000)); // < 5 days
        CHECK(gate.Offer(story("adj:school+industry", "power_plant", false, false, true), t + kRoastGapDays * day, rs + 1000));
        t += kRoastGapDays * day, rs += 1000;
        // a Major story (a milestone) that hits the gap is held, not lost, and runs once the gap opens
        bool held = false;
        CHECK(!gate.Offer(story("population_1k"), t + 1 * day, rs + 10, &held));
        CHECK(held && gate.Holding());
        NewsStory out;
        CHECK(!gate.TakeHeld(t + 1.5 * day, rs + 20, out)); // still closed
        CHECK(gate.TakeHeld(t + kAnyGapDays * day, rs + kAnyGapRealSec, out) && out.trigger == "population_1k");
        CHECK(!gate.Holding() && !gate.TakeHeld(t + 9 * day, rs + 9999, out)); // once
        t += kAnyGapDays * day, rs += kAnyGapRealSec;
        // a held story that waits past kHoldDays is dropped
        CHECK(!gate.Offer(story("budget_deficit"), t + 0.5 * day, rs + 10, &held) && held);
        CHECK(!gate.TakeHeld(t + 0.5 * day + (kHoldDays + 1) * day, rs + 99999, out));
        // a load: kinds already standing are not "the first"; the clock may go backwards without locking the news out
        gate.Reset();
        gate.MarkKindSeen("school");
        CHECK(!gate.Offer(story("school_built", "school", true), 1 * day, 5.0));
        CHECK(gate.Offer(story("fire_station_built", "fire_station", true), 1 * day, 5.0));
        // a simulated busy player: 60 game days, a building every game hour of 12 kinds, roads, stops, taxes: the
        // feed gets at most one story per 2 game days (and nothing for the routine stuff)
        NewsGate busy;
        int stories = 0;
        const char* kinds[] = {"school", "hospital", "police", "fire_station", "park", "bus_depot",
                               "power_plant", "water_tower", "landfill", "stadium", "airport", "rail_depot"};
        double lastStory = -1e18;
        bool spacingOk = true;
        for (int h = 0; h < 60 * 24; ++h) {
            const double m = h * 60.0, r = h * 20.0; // one game hour = 20 real seconds (a fast clock)
            const char* k = kinds[h % 12];
            const bool ran = busy.Offer(story("school_built", k, true, (h % 7) == 0), m, r) | busy.Offer(story("road_bulldozed"), m, r) |
                             busy.Offer(story("bus_stop_opened"), m, r) | busy.Offer(story(h % 2 ? "tax_raised" : "tax_lowered"), m, r);
            if (ran) {
                if (m - lastStory < kAnyGapDays * day) spacingOk = false;
                lastStory = m, ++stories;
            }
        }
        CHECK(spacingOk);
        CHECK(stories > 0 && stories <= 30);
        std::printf("  busy player, 60 game days: %d stories (was one per placement)\n", stories);
    }

    if (failures) {
        std::printf("sim_news_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_news_test: all passed (%d lines in news_lines.json)\n", lines);
    return 0;
}
