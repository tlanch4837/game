// Headless checks for the radio's opening stories (src/city/news_adjacency.h, src/city/news_feed.h):
//   catalog names map to the news kinds of the "adj:<a>+<b>" lines; NewsFeed::Has / Tone; one story per opening (a bad
//   neighbour pair beats the opening line, which beats a good pair); a bad pair without a recorded clip goes to the
//   written feed while the own opening clip plays; nothing at all when there is nothing to say.
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

    if (failures) {
        std::printf("sim_news_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sim_news_test: all passed (%d lines in news_lines.json)\n", lines);
    return 0;
}
