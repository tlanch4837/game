// ------------------------------------------------------------------- the city radio (Elton) and The Weekly Rundown
// Included by main.cpp after city_sim.inl. RadioSync runs once per frame after SimSync. It watches the city for things the
// player did (or what came of them), diffs that against the last frame, and posts a trigger. A posted trigger plays one
// clip from assets\audio\radio (tbd::audio::PlayRadio): "radio/<trigger>" picks one of <trigger>_1.._5, and sometimes a
// call-in (callin_<trigger>[_2]) or two residents talking (convo_<trigger>) follows it. Missing clips are silent.
//
// Rules (the user's): nothing plays as filler, only in answer to the player; a clip at most every kMinGapDays game days
// and kMinGapReal real seconds; the same trigger at most every kSameTriggerDays. Once a game week, if anything happened,
// The Weekly Rundown plays: theme + intro, the week's top stories with stings between, a call-in, sign-off over music.
//
// Environment (dev):
//   TBD_RADIO=0|1|2      0 off, 1 host only (Elton, no callers / residents), 2 everything (default)
//   TBD_RADIO_TEST=<t>   posts trigger <t> once, 3 s after the city loads (ignores the cooldowns); "weekly" plays an episode
//   TBD_RADIO_LOG=1      logs every trigger seen, played or held back

#include <deque>

namespace city_radio {

constexpr double kDay = 1440.0;            // game minutes
constexpr double kMinGapDays = 1.0;        // between two radio clips
constexpr double kSameTriggerDays = 10.0;  // before one trigger may play again
constexpr double kMinGapReal = 75.0;       // real seconds between two clips, whatever the clock speed
constexpr double kWeekDays = 7.0;

int EnvInt(const char* name, int fallback) {
    char v[16] = {};
    return tbd::platform::GetEnvA(name, v, sizeof(v)) > 0 ? atoi(v) : fallback;
}
std::string EnvStr(const char* name) {
    char v[96] = {};
    return tbd::platform::GetEnvA(name, v, sizeof(v)) > 0 ? std::string(v) : std::string();
}

// Triggers that have call-ins / conversations on disk (the rest skip that step instead of trying a missing name).
const std::set<std::string>& WithCallin() {
    static const std::set<std::string> s = {
        "bus_route_empty", "adj:residential+industry", "strike", "power_outage", "transit_opened", "bus_depot_opened",
        "rail_depot_opened", "tram_depot_opened", "taxi_depot_opened", "bus_stop_opened", "tram_derailment",
        "vehicle_breakdown", "park_built", "school_built", "hospital_built", "zone_industrial_filled", "flood",
        "fire_outbreak", "population_1k", "population_10k", "population_50k", "unemployment_low", "unemployment_high",
        "happiness_high", "happiness_low", "rent_spike", "housing_good", "homeless_up", "school_grad_rate_up",
        "mayor_reelection_poll_up", "budget_surplus", "budget_deficit", "bankrupt_transit", "corruption_scandal",
        "tax_raised", "tax_lowered", "zone_residential_filled", "zone_commercial_filled", "crime_spike", "crime_drop",
        "traffic_jam", "traffic_flow_good", "road_bulldozed", "pollution_high", "industrial_smog", "noise_complaints",
        "depot_fire", "water_shortage", "sewage_leak", "disaster_recovered", "library_built", "stadium_built",
        "electricity_surplus", "water_surplus", "cleanup_done", "tree_planted", "river_cleaned", "landfill_full",
        "illegal_dumping", "fire_station_built", "police_station_built", "power_plant_built", "water_tower_built",
        "landfill_built", "bridge_built", "highway_built", "adj:residential+landfill", "adj:school+industry"};
    return s;
}
const std::set<std::string>& WithConvo() {
    static const std::set<std::string> s = {
        "bus_route_empty", "adj:residential+industry", "strike", "power_outage", "tram_derailment", "flood",
        "fire_outbreak", "park_built", "school_built", "hospital_built", "transit_opened", "taxi_depot_opened",
        "rent_spike", "tax_raised", "tax_lowered", "traffic_jam", "crime_drop", "pollution_high", "noise_complaints",
        "water_shortage", "stadium_built", "library_built", "tree_planted", "river_cleaned", "landfill_full",
        "bridge_built", "highway_built", "unemployment_low", "happiness_high", "population_10k"};
    return s;
}

// "adj:residential+industry" -> "adj_residential_industry" (the clip file names).
std::string FileStem(const std::string& trigger) {
    std::string out;
    for (char ch : trigger) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (ok) out += ch;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// ---- the written feed: src/city/news_lines.json (one {"trigger","tone","text"} object per line), headlines per trigger
struct FeedItem { double minute; std::string text; int tone; bool aired; };

// The headline when news_lines.json has none for a trigger: its name made readable.
std::string Fallback(const std::string& trigger) {
    std::string t = trigger;
    for (char& c : t) if (c == '_' || c == ':' || c == '+') c = ' ';
    if (!t.empty()) t[0] = (char)toupper((unsigned char)t[0]);
    return t + ".";
}

struct State {
    bool init = false;
    int mode = 2;
    bool log = false;
    std::string test;
    double testAt = -1.0;           // real seconds since load when the test trigger fires
    uint64_t generation = ~0ull;
    double realSinceLoad = 0.0, realSinceClip = 1e9;
    double lastClipMinute = -1e18;
    std::map<std::string, double> lastByTrigger; // game minute it last played
    uint32_t rng = 0x51f15e5du;
    // the week, for The Weekly Rundown
    double weekStart = -1.0;
    std::vector<std::string> week;  // triggers that happened this week, in order (each once)
    // what the last frame saw
    std::map<uint64_t, uint8_t> transportState;
    std::set<uint64_t> utilSeen, routeSeen, stopSeen;
    size_t roadSegs = 0;
    uint64_t population = 0;
    int unemployment = 0;           // -1 low, 0 normal, 1 high
    int budget = 0;                 // -1 in the red, 0, 1 surplus
    int taxSum = -1;
    double taxChangedAt = -1.0;
    int taxDir = 0;
    int stopsOpenedPosts = 0;
    // the feed
    ::city::NewsFeed news;
    bool newsLoaded = false;
    std::deque<FeedItem> feed; // newest first
};
State g;

float Rand01() {
    g.rng ^= g.rng << 13, g.rng ^= g.rng >> 17, g.rng ^= g.rng << 5;
    return (float)(g.rng & 0xffffffu) / 16777216.0f;
}

void NoteWeek(const std::string& t) {
    if (std::find(g.week.begin(), g.week.end(), t) == g.week.end()) g.week.push_back(t);
}

void LoadNews() {
    g.newsLoaded = true;
    for (const wchar_t* rel : {L"\\..\\src\\city\\news_lines.json", L"\\..\\assets\\news\\news_lines.json"}) {
        std::FILE* f = tbd::platform::WOpen((tbd::ExeDirectory() + rel).c_str(), L"rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
        tbd::Log("Radio: %d written headlines", ::city::LoadNewsLines(g.news, text));
        return;
    }
    tbd::Log("Radio: no news_lines.json; the feed uses plain headlines");
}

void Headline(const std::string& trigger, bool aired) {
    std::string text, tone;
    if (!g.news.Post(trigger, text, tone)) text = Fallback(trigger), tone = "random";
    g.news.Queue().clear(); // the feed keeps its own list
    g.feed.push_front({g_clock.minutes, text, tone == "good" ? 1 : tone == "bad" ? -1 : 0, aired});
    if (g.feed.size() > 30) g.feed.pop_back();
}

// Plays a trigger now if the cooldowns allow. Every trigger makes a headline and counts toward the week either way.
void Post(const std::string& trigger, bool force = false) {
    NoteWeek(trigger);
    if (g.mode == 0) { Headline(trigger, false); return; }
    const double now = g_clock.minutes;
    auto it = g.lastByTrigger.find(trigger);
    const bool sameOk = it == g.lastByTrigger.end() || now - it->second >= kSameTriggerDays * kDay;
    const bool gapOk = now - g.lastClipMinute >= kMinGapDays * kDay && g.realSinceClip >= kMinGapReal && !tbd::audio::RadioBusy();
    if (!force && !(sameOk && gapOk)) {
        if (g.log) tbd::Log("Radio: %s held back (cooldown)", trigger.c_str());
        Headline(trigger, false);
        return;
    }
    const std::string stem = FileStem(trigger);
    std::vector<std::string> seq = {"radio/" + stem};
    if (g.mode >= 2) {
        const float r = Rand01();
        if (r < 0.35f && WithCallin().count(trigger)) seq.push_back("radio/callin_" + stem);
        else if (r < 0.55f && WithConvo().count(trigger)) seq.push_back("radio/convo_" + stem);
    }
    tbd::audio::PlayRadio(seq, 0.9f);
    Headline(trigger, true);
    g.lastByTrigger[trigger] = now;
    g.lastClipMinute = now;
    g.realSinceClip = 0.0;
    if (g.log) tbd::Log("Radio: %s (%zu clips)", trigger.c_str(), seq.size());
}

// The Weekly Rundown: up to three of the week's stories (the latest ones), one call-in, and the two halves of a matched
// music pair (intro on one theme, sign-off on the other).
void PlayWeekly() {
    if (g.mode == 0 || g.week.empty()) return;
    const char* pair = Rand01() < 0.5f ? "a" : "b";
    std::vector<std::string> stories(g.week.end() - std::min<size_t>(g.week.size(), 3), g.week.end());
    std::vector<std::string> seq = {std::string("radio/pod_intro_") + pair};
    for (size_t i = 0; i < stories.size(); ++i) {
        if (i > 0) { seq.push_back("radio/pod_sting"); seq.push_back("radio/pod_bridge"); }
        seq.push_back("radio/" + FileStem(stories[i]));
    }
    if (g.mode >= 2)
        for (auto s = stories.rbegin(); s != stories.rend(); ++s)
            if (WithCallin().count(*s)) { seq.push_back("radio/pod_sting"); seq.push_back("radio/callin_" + FileStem(*s)); break; }
    seq.push_back(std::string("radio/pod_signoff_") + pair);
    tbd::audio::StopRadio(0.8f);
    tbd::audio::PlayRadio(seq, 0.5f);
    g.lastClipMinute = g_clock.minutes;
    g.realSinceClip = 0.0;
    if (g.log) tbd::Log("Radio: The Weekly Rundown, %zu stories, pair %s", stories.size(), pair);
}

// The trigger a finished transport-tab building announces ("" for none).
std::string TransportTrigger(const tbd::city::BuildingDef& d) {
    using tbd::city::TTab;
    const std::string n = d.name;
    switch (d.tab) {
    case TTab::Bus:
        if (n.find("Tram") != std::string::npos) return "tram_depot_opened";
        if (n.find("Taxi") != std::string::npos) return "taxi_depot_opened";
        return "bus_depot_opened";
    case TTab::Rail: return n.find("Cargo") != std::string::npos ? "" : "rail_depot_opened";
    case TTab::Fire: return "fire_station_built";
    case TTab::Police: return "police_station_built";
    case TTab::Health: return "hospital_built";
    case TTab::Education: return "school_built";
    default: return "";
    }
}

// Takes the city as it stands as "already announced" (a load or a new map must not read out every building).
void Baseline() {
    g.transportState.clear();
    for (const auto& b : g_transport.items)
        if (b.alive) g.transportState[b.id] = (uint8_t)b.state;
    g.utilSeen.clear();
    for (const auto& u : g_util.items)
        if (u.alive) g.utilSeen.insert(u.id);
    g.routeSeen.clear();
    for (const auto& r : g_transit.routes)
        if (r.alive) g.routeSeen.insert(r.id);
    g.stopSeen.clear();
    for (const auto& s : g_transit.stops)
        if (s.alive) g.stopSeen.insert(s.id);
    g.roadSegs = 0;
    for (const auto& s : g_roads.net.segs) g.roadSegs += s.alive ? 1 : 0;
    g.population = g_simTotals.population;
    g.taxSum = -1;
    g.week.clear();
    g.weekStart = g_clock.minutes;
}

void Watch() {
    using namespace tbd::city;
    // transport-tab buildings: announced when construction finishes
    for (const auto& b : g_transport.items) {
        if (!b.alive) continue;
        auto it = g.transportState.find(b.id);
        const uint8_t was = it == g.transportState.end() ? 0xff : it->second;
        g.transportState[b.id] = (uint8_t)b.state;
        if (b.state == BState::Done && was != (uint8_t)BState::Done) {
            const std::string t = TransportTrigger(Defs()[b.def]);
            if (!t.empty()) Post(t);
        }
    }
    // power and water buildings (not the backup ones)
    for (const auto& u : g_util.items) {
        if (!u.alive || !g.utilSeen.insert(u.id).second) continue;
        const UtilDef& D = UtilDefs()[u.def];
        if (D.backup) continue;
        if (D.kind == Util::Electricity) Post("power_plant_built");
        else if (D.kind == Util::Water) Post("water_tower_built");
    }
    // transit: a new line, the first bus stops
    for (const auto& r : g_transit.routes)
        if (r.alive && g.routeSeen.insert(r.id).second && r.stops.size() >= 2) Post("transit_opened");
    for (const auto& s : g_transit.stops)
        if (s.alive && g.stopSeen.insert(s.id).second && g.stopsOpenedPosts < 3) { ++g.stopsOpenedPosts; Post("bus_stop_opened"); }
    // roads: a bulldozed stretch (three or more segments gone since the last look)
    size_t segs = 0;
    for (const auto& s : g_roads.net.segs) segs += s.alive ? 1 : 0;
    if (segs + 3 <= g.roadSegs) { Post("road_bulldozed"); g.roadSegs = segs; }
    else if (segs > g.roadSegs) g.roadSegs = segs;
    // population milestones
    const uint64_t pop = g_simTotals.population;
    for (uint64_t mark : {1000ull, 10000ull, 50000ull})
        if (g.population < mark && pop >= mark) Post(mark == 1000 ? "population_1k" : mark == 10000 ? "population_10k" : "population_50k");
    g.population = pop;
    // jobs (with hysteresis so it does not flicker)
    if (g_simTotals.adults >= 200) {
        const double employed = (double)g_simTotals.working / (double)g_simTotals.adults;
        int u = g.unemployment;
        if (employed < 0.80) u = 1;
        else if (employed > 0.96) u = -1;
        else if (employed > 0.84 && employed < 0.93) u = 0;
        if (u != g.unemployment && u != 0) Post(u > 0 ? "unemployment_high" : "unemployment_low");
        g.unemployment = u;
    }
    // money
    int budget = g.budget;
    if (g_simEcon.treasury < 0) budget = -1;
    else if (g_simEcon.days > 3 && g_simEcon.LastNet() > 0) budget = 1;
    else if (g_simEcon.treasury > 0 && g_simEcon.LastNet() <= 0) budget = 0;
    if (budget != g.budget && budget != 0) Post(budget > 0 ? "budget_surplus" : "budget_deficit");
    g.budget = budget;
    // taxes: one story an hour after the player stops moving the sliders
    int taxSum = 0;
    for (int c = 0; c < SimEconomy::kCats; ++c) taxSum += g_simEcon.taxRate10[c];
    if (g.taxSum >= 0 && taxSum != g.taxSum) {
        g.taxDir += taxSum > g.taxSum ? 1 : -1;
        g.taxChangedAt = g_clock.minutes;
    }
    g.taxSum = taxSum;
    if (g.taxDir != 0 && g_clock.minutes - g.taxChangedAt >= 60.0) {
        Post(g.taxDir > 0 ? "tax_raised" : "tax_lowered");
        g.taxDir = 0;
    }
}

} // namespace city_radio

void RadioSync(float realDt) {
    using namespace city_radio;
    if (!g.init) {
        g.init = true;
        g.mode = std::clamp(EnvInt("TBD_RADIO", 2), 0, 2);
        g.log = EnvInt("TBD_RADIO_LOG", 0) != 0;
        g.test = EnvStr("TBD_RADIO_TEST");
        g.rng ^= (uint32_t)tbd::platform::TickCountMs();
        tbd::audio::SetBusVolume(tbd::audio::Bus::Radio, 1.0f);
        LoadNews();
    }
    if (g.generation != g_cityGeneration) { // new map or load: start counting from what is there now
        g.generation = g_cityGeneration;
        g.realSinceLoad = 0.0;
        g.testAt = g.test.empty() ? -1.0 : 3.0;
        tbd::audio::StopRadio(0.3f);
        g.feed.clear();
        Baseline();
        return;
    }
    g.realSinceLoad += realDt;
    g.realSinceClip += realDt;
    if (g.realSinceLoad < 2.0) { Baseline(); return; } // the loaded city is still settling in
    if (g.testAt >= 0.0 && g.realSinceLoad >= g.testAt) {
        g.testAt = -1.0;
        if (g.test == "weekly") {
            for (const char* t : {"bus_depot_opened", "bus_route_empty", "tax_raised"}) NoteWeek(t), Headline(t, true);
            PlayWeekly();
        }
        else Post(g.test, true);
    }
    Watch();
    if (g_clock.minutes - g.weekStart >= kWeekDays * kDay) {
        if (!g.week.empty()) PlayWeekly();
        g.week.clear();
        g.weekStart = g_clock.minutes;
    }
}

// The City News panel's numbers (main.cpp, before CityHudFrame) and the radio button it may have pressed (after).
void RadioHudFill(tbd::gui::CityHud& h) {
    using namespace city_radio;
    static bool first = true;
    if (first) first = false, h.radioMode = g.mode;
    h.onAir = tbd::audio::RadioBusy();
    h.news.clear();
    for (const FeedItem& f : g.feed) {
        char when[48];
        const int day = (int)(f.minute / 1440.0) + 1, hour = (int)std::fmod(f.minute / 60.0, 24.0), minute = (int)std::fmod(f.minute, 60.0);
        snprintf(when, sizeof(when), "Day %d, %02d:%02d", day, hour, minute);
        h.news.push_back({when, f.text, f.tone, f.aired});
    }
}
void RadioHudRead(const tbd::gui::CityHud& h) {
    using namespace city_radio;
    if (h.radioMode == g.mode) return;
    g.mode = std::clamp(h.radioMode, 0, 2);
    if (g.mode == 0) tbd::audio::StopRadio(0.5f);
}

// city_sim.inl's SimNews: the sim's own events (bankruptcy, ...) as radio triggers where a clip exists.
void RadioSimEvent(const char* trigger) {
    const std::string t = trigger;
    if (t == "bankrupt") city_radio::Post("budget_deficit");
    else if (t == "loan_taken") city_radio::Headline("loan_taken", false);
}
