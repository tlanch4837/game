// The city view's music and ambience decisions, without any audio code (headless; tests\audio_director_test.cpp).
// src/audio.cpp's audio thread owns one of each and applies what they say to its voices.
//
//   MusicDirector  the playlist: assets\audio\music\playlist.txt (tools/audio_gen writes it), one "name period seconds
//                  title" per line. The theme plays first, then tracks of the game clock's period (morning 5-10 h, day
//                  10-17, evening 17-21, night 21-5), shuffled, never the same track twice in a row nor one of the last
//                  two when there is a choice, with a short pause between some tracks, crossfaded.
//   Ducker         music (and a little of the ambience) dips about 12 dB while the radio talks and comes back slowly after.
//   CityBedMix     how loud each ambience bed is, from the hour, the camera's height, how built-up and how busy the view
//                  is, and how much water is near.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace tbd::audio {

enum class Period { Theme = -1, Morning = 0, Day = 1, Evening = 2, Night = 3 };

inline Period PeriodOf(float hour) {
    hour = std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f);
    if (hour >= 5.0f && hour < 10.0f) return Period::Morning;
    if (hour >= 10.0f && hour < 17.0f) return Period::Day;
    if (hour >= 17.0f && hour < 21.0f) return Period::Evening;
    return Period::Night;
}

inline bool ParsePeriod(const std::string& s, Period& p) {
    if (s == "theme") p = Period::Theme;
    else if (s == "morning") p = Period::Morning;
    else if (s == "day") p = Period::Day;
    else if (s == "evening") p = Period::Evening;
    else if (s == "night") p = Period::Night;
    else return false;
    return true;
}

struct MusicTrack {
    std::string name;    // under assets\audio\music, without extension ("day_1")
    Period period = Period::Day;
    float seconds = 0.0f; // 0: unknown (then the track plays to its end, no crossfade)
};

// "name period seconds [title...]" lines; '#' comments. Unknown periods are skipped.
inline std::vector<MusicTrack> ParsePlaylist(const std::string& text) {
    std::vector<MusicTrack> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream in(line);
        MusicTrack t;
        std::string period;
        if (!(in >> t.name >> period)) continue;
        if (!ParsePeriod(period, t.period)) continue;
        float s = 0.0f;
        if (in >> s) t.seconds = std::max(0.0f, s);
        out.push_back(t);
    }
    return out;
}

class MusicDirector {
public:
    static constexpr float kCrossfade = 6.0f;   // seconds the end of one track overlaps the next
    void SetTracks(std::vector<MusicTrack> t) { tracks_ = std::move(t); recent_.clear(); themePlayed_ = false; }
    const std::vector<MusicTrack>& Tracks() const { return tracks_; }
    bool Empty() const { return tracks_.empty(); }
    void Restart() { themePlayed_ = false; } // a new city: the theme again first

    // The next track for this hour: -1 when there is none. r01: a uniform random number in [0, 1).
    int Next(float hour, float r01) {
        if (tracks_.empty()) return -1;
        if (!themePlayed_) {
            themePlayed_ = true;
            for (size_t i = 0; i < tracks_.size(); ++i)
                if (tracks_[i].period == Period::Theme) return Remember((int)i);
        }
        const Period want = PeriodOf(hour);
        std::vector<int> pool = Pool(want, 2);
        if (pool.empty()) pool = Pool(want, 1);
        if (pool.empty()) { // nothing new for this period: borrow from the neighbouring periods
            const int w = (int)want;
            for (int d : {1, 3, 2})
                if (pool.empty()) pool = PoolPeriod((Period)((w + d) % 4), 1);
        }
        if (pool.empty()) pool = AnyBut(recent_.empty() ? -1 : recent_.back());
        if (pool.empty()) pool.push_back(0);
        const int pick = pool[std::min(pool.size() - 1, (size_t)(r01 * (float)pool.size()))];
        return Remember(pick);
    }

    // Seconds of silence after a track ends before the next starts (0 = crossfade straight in): mostly none, sometimes a
    // breath where only the city is heard.
    static float GapAfter(float r01) { return r01 < 0.55f ? 0.0f : r01 < 0.8f ? 8.0f : 18.0f; }

private:
    int Remember(int i) {
        recent_.push_back(i);
        if (recent_.size() > 2) recent_.erase(recent_.begin());
        return i;
    }
    bool Recent(int i, size_t depth) const {
        for (size_t k = 0; k < std::min(depth, recent_.size()); ++k)
            if (recent_[recent_.size() - 1 - k] == i) return true;
        return false;
    }
    std::vector<int> PoolPeriod(Period p, size_t depth) const {
        std::vector<int> out;
        for (size_t i = 0; i < tracks_.size(); ++i)
            if (tracks_[i].period == p && !Recent((int)i, depth)) out.push_back((int)i);
        return out;
    }
    std::vector<int> Pool(Period p, size_t depth) const { return PoolPeriod(p, depth); }
    std::vector<int> AnyBut(int last) const {
        std::vector<int> out;
        for (size_t i = 0; i < tracks_.size(); ++i)
            if ((int)i != last && tracks_[i].period != Period::Theme) out.push_back((int)i);
        return out;
    }
    std::vector<MusicTrack> tracks_;
    std::vector<int> recent_; // the last two picks, oldest first
    bool themePlayed_ = false;
};

// A gain that dips while something talks: down in about half a second, back up over about two after it stops (with a
// short hold, so the gaps between radio clips do not pump).
class Ducker {
public:
    explicit Ducker(float depthDb = -12.0f) : floor_(std::pow(10.0f, depthDb / 20.0f)) {}
    float Update(bool talking, float dt) {
        hold_ = talking ? 1.2f : std::max(0.0f, hold_ - dt);
        const float target = (talking || hold_ > 0.0f) ? floor_ : 1.0f;
        const float tau = target < gain_ ? 0.18f : 0.7f; // attack ~0.5 s to settle, release ~2 s
        gain_ += (target - gain_) * (1.0f - std::exp(-dt / tau));
        return gain_;
    }
    float Gain() const { return gain_; }
    float Floor() const { return floor_; }

private:
    float floor_ = 0.25f, gain_ = 1.0f, hold_ = 0.0f;
};

// ---------------------------------------------------------------- city ambience
// What the game knows about the view (CityScene in audio.h carries it to the audio thread).
struct CityView {
    float hour = 12.0f;    // game clock, 0..24
    float height = 120.0f; // camera eye above the ground, metres
    float built = 0.0f;    // 0..1 how built-up the ground under the view is
    float traffic = 0.0f;  // 0..1 roads and cars in view
    float water = 0.0f;    // 0..1 sea or lake near the view
};

enum CityBed { kBedAir, kBedBirds, kBedCrickets, kBedHum, kBedTraffic, kBedWater, kBedCrowd, kBedCount };
inline const char* CityBedName(int b) {
    static const char* const k[kBedCount] = {"ambience/city_air", "ambience/city_birds", "ambience/city_crickets", "ambience/city_hum",
                                              "ambience/city_traffic", "ambience/water", "ambience/crowd"};
    return b >= 0 && b < kBedCount ? k[b] : "";
}

inline float Smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// 1 in the day (7-18 h), 0 at night (21-4.5 h), smooth in between.
inline float Daylight(float hour) {
    hour = std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f);
    if (hour < 12.0f) return Smooth01((hour - 4.5f) / 2.5f);
    return 1.0f - Smooth01((hour - 18.0f) / 3.0f);
}

// 1 at street level, 0 from ~450 m up.
inline float Nearness(float height) { return 1.0f - Smooth01((height - 25.0f) / 425.0f); }

// Each bed's level, 0..1 (before the Ambience bus and the radio duck). At most seven streamed beds, usually four or five.
inline void CityBedMix(const CityView& v, float out[kBedCount]) {
    const float day = Daylight(v.hour), night = 1.0f - day;
    const float closeK = Nearness(v.height), farK = 1.0f - closeK;
    const float built = std::clamp(v.built, 0.0f, 1.0f), traffic = std::clamp(v.traffic, 0.0f, 1.0f), water = std::clamp(v.water, 0.0f, 1.0f);
    const float high = 1.0f - Smooth01((v.height - 900.0f) / 1500.0f); // the life of the ground thins out very high up
    out[kBedAir] = 0.32f + 0.5f * farK;                                              // open air: always, airier up high
    out[kBedBirds] = day * (0.18f + 0.55f * closeK) * (1.0f - 0.55f * built) * high;  // fewer birds over a dense city
    out[kBedCrickets] = night * (0.15f + 0.5f * closeK) * (1.0f - 0.6f * built) * high;
    out[kBedHum] = built * (0.25f + 0.4f * farK) * (0.65f + 0.35f * day);              // the city as a whole, from above
    out[kBedTraffic] = traffic * closeK * (0.45f + 0.55f * day) * 0.9f;                 // the streets, close
    out[kBedWater] = water * (0.2f + 0.6f * closeK) * high;
    out[kBedCrowd] = built * closeK * closeK * (0.25f + 0.75f * day) * 0.7f;               // people, only down among them
    for (int i = 0; i < kBedCount; ++i) out[i] = std::clamp(out[i], 0.0f, 1.0f);
}

} // namespace tbd::audio
