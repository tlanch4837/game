// Headless tests for src/audio_director.h: the city music playlist, the radio duck, the ambience bed mix.
#include "audio_director.h"

#include <cmath>
#include <cstdio>
#include <set>

using namespace tbd::audio;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static float Db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

static float Rand(uint32_t& s) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return (float)(s & 0xffffffu) / 16777216.0f;
}

static const char* kList =
    "# comment line\n"
    "theme theme 72.0 Shortville (Main Theme)\n"
    "morning_1 morning 156.5 Harbour Morning\n"
    "morning_2 morning 150 First Light\n"
    "day_1 day 145.5 Main Street\n"
    "day_2 day 160 Zoning Board\n"
    "evening_1 evening 140 Golden Hour\n"
    "night_1 night 144 Streetlights\n"
    "night_2 night 146 Quiet Blocks\n"
    "broken line\n"
    "bad_period noon 100 x\n";

int main() {
    // periods
    CHECK(PeriodOf(4.99f) == Period::Night);
    CHECK(PeriodOf(5.0f) == Period::Morning);
    CHECK(PeriodOf(9.99f) == Period::Morning);
    CHECK(PeriodOf(10.0f) == Period::Day);
    CHECK(PeriodOf(17.0f) == Period::Evening);
    CHECK(PeriodOf(21.0f) == Period::Night);
    CHECK(PeriodOf(23.9f) == Period::Night);
    CHECK(PeriodOf(-1.0f) == Period::Night);
    CHECK(PeriodOf(36.0f) == Period::Day); // wraps

    // playlist parsing
    const std::vector<MusicTrack> tracks = ParsePlaylist(kList);
    CHECK(tracks.size() == 8);
    CHECK(tracks[0].name == "theme" && tracks[0].period == Period::Theme && std::fabs(tracks[0].seconds - 72.0f) < 1e-3f);
    CHECK(tracks[3].name == "day_1" && tracks[3].period == Period::Day && std::fabs(tracks[3].seconds - 145.5f) < 1e-3f);

    // the director: the theme first, then the hour's tracks, never the same twice in a row, never one of the last two
    // when the period has more than two
    {
        MusicDirector d;
        d.SetTracks(tracks);
        uint32_t s = 12345;
        CHECK(d.Next(12.0f, Rand(s)) == 0); // theme
        int last = 0, prev = -1;
        std::set<int> seenDay;
        for (int k = 0; k < 300; ++k) {
            const int i = d.Next(12.0f, Rand(s));
            CHECK(i > 0); // the theme does not come back
            CHECK(i != last);
            CHECK(tracks[(size_t)i].period == Period::Day); // two day tracks: they alternate
            seenDay.insert(i);
            prev = last, last = i;
        }
        (void)prev;
        CHECK(seenDay.size() == 2);
        // the night: two tracks, alternate; the evening: one track, so the neighbours lend theirs instead of a repeat
        last = -1;
        for (int k = 0; k < 50; ++k) {
            const int i = d.Next(18.0f, Rand(s));
            CHECK(i != last);
            CHECK(tracks[(size_t)i].period != Period::Theme);
            last = i;
        }
        // a new city plays the theme again
        d.Restart();
        CHECK(d.Next(3.0f, Rand(s)) == 0);
        CHECK(tracks[(size_t)d.Next(3.0f, Rand(s))].period == Period::Night);
    }
    // with three tracks in a period none of the last two repeats
    {
        std::vector<MusicTrack> t = {{"a", Period::Day, 10}, {"b", Period::Day, 10}, {"c", Period::Day, 10}};
        MusicDirector d;
        d.SetTracks(t);
        uint32_t s = 99;
        int a = d.Next(12.0f, Rand(s)), b = d.Next(12.0f, Rand(s));
        CHECK(a != b);
        for (int k = 0; k < 100; ++k) {
            const int c = d.Next(12.0f, Rand(s));
            CHECK(c != a && c != b); // a cycle of three
            a = b, b = c;
        }
    }
    // one track only: it may repeat (nothing else to play), and an empty list gives -1
    {
        MusicDirector d;
        d.SetTracks({{"solo", Period::Night, 10}});
        uint32_t s = 7;
        CHECK(d.Next(12.0f, Rand(s)) == 0);
        CHECK(d.Next(12.0f, Rand(s)) == 0);
        MusicDirector e;
        CHECK(e.Next(12.0f, 0.5f) == -1);
    }
    // gaps between tracks
    for (float r = 0.0f; r < 1.0f; r += 0.01f) {
        const float g = MusicDirector::GapAfter(r);
        CHECK(g >= 0.0f && g <= 20.0f);
    }
    CHECK(MusicDirector::GapAfter(0.1f) == 0.0f);

    // the duck: -12 dB within about half a second of talking, held through short gaps, back within ~2.5 s, no overshoot
    {
        Ducker k(-12.0f);
        const float dt = 1.0f / 60.0f;
        float t = 0.0f, g = 1.0f;
        for (; t < 0.6f; t += dt) g = k.Update(true, dt);
        CHECK(Db(g) < -10.5f && Db(g) >= -12.01f);
        for (t = 0.0f; t < 2.0f; t += dt) g = k.Update(true, dt);
        CHECK(std::fabs(Db(g) + 12.0f) < 0.2f);
        // a 0.5 s gap between clips: stays down
        for (t = 0.0f; t < 0.5f; t += dt) g = k.Update(false, dt);
        CHECK(Db(g) < -11.5f);
        // talking stops for good: back up, monotonically, never above 1
        float prevG = g;
        bool mono = true;
        for (t = 0.0f; t < 4.0f; t += dt) {
            g = k.Update(false, dt);
            mono = mono && g >= prevG - 1e-6f && g <= 1.0f + 1e-6f;
            prevG = g;
        }
        CHECK(mono);
        CHECK(Db(g) > -1.0f);
        // a frame hitch (dt 0.25 s) does not jump past the target
        g = k.Update(true, 0.25f);
        CHECK(g >= k.Floor() - 1e-6f && g <= 1.0f);
    }

    // the ambience mix
    {
        float w[kBedCount];
        CityView v;
        // night, street level, empty land by the sea: crickets, air, water; no birds, hum, traffic, crowd
        v.hour = 23.0f, v.height = 10.0f, v.built = 0.0f, v.traffic = 0.0f, v.water = 1.0f;
        CityBedMix(v, w);
        CHECK(w[kBedCrickets] > 0.4f && w[kBedBirds] == 0.0f);
        CHECK(w[kBedHum] == 0.0f && w[kBedTraffic] == 0.0f && w[kBedCrowd] == 0.0f);
        CHECK(w[kBedWater] > 0.6f && w[kBedAir] > 0.2f);
        // noon, street level in a dense busy city: birds fewer, traffic and crowd up, crickets off
        v.hour = 12.0f, v.built = 1.0f, v.traffic = 1.0f, v.water = 0.0f;
        CityBedMix(v, w);
        CHECK(w[kBedCrickets] == 0.0f && w[kBedBirds] > 0.0f && w[kBedBirds] < 0.4f);
        CHECK(w[kBedTraffic] > 0.7f && w[kBedCrowd] > 0.5f && w[kBedWater] == 0.0f);
        const float airLow = w[kBedAir], humLow = w[kBedHum];
        // the same city from 800 m up: no street detail, more air, more of the city's hum
        v.height = 800.0f;
        CityBedMix(v, w);
        CHECK(w[kBedTraffic] < 0.01f && w[kBedCrowd] < 0.01f);
        CHECK(w[kBedAir] > airLow + 0.3f && w[kBedHum] > humLow);
        // dusk crosses over smoothly: at 19:30 both birds and crickets are partly on
        v.height = 10.0f, v.built = 0.0f, v.hour = 19.5f;
        CityBedMix(v, w);
        CHECK(w[kBedBirds] > 0.05f && w[kBedCrickets] > 0.05f);
        // every level in 0..1 everywhere
        bool inRange = true;
        for (float h = 0.0f; h < 24.0f; h += 0.5f)
            for (float z : {0.0f, 30.0f, 200.0f, 1000.0f, 5000.0f}) {
                v.hour = h, v.height = z, v.built = 0.7f, v.traffic = 0.6f, v.water = 0.4f;
                CityBedMix(v, w);
                for (int i = 0; i < kBedCount; ++i) inRange = inRange && w[i] >= 0.0f && w[i] <= 1.0f;
            }
        CHECK(inRange);
        CHECK(std::string(CityBedName(kBedAir)) == "ambience/city_air");
        CHECK(std::string(CityBedName(kBedCount)).empty());
    }

    if (g_fail) { std::printf("audio_director_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("audio_director_test: all passed\n");
    return 0;
}
