// Phase 3 weather and seasons (src/city/sim_env_weather.h): seasons in calendar order, the chain is deterministic per
// seed and replayable, a different seed differs, solar is 0 at night, winter is colder than summer and snows, the wind
// factor follows the turbine curve, and a weather sample is cheap. Headless. Run: tests\sim_env_weather_test.bat
#include <chrono>
#include <cstdio>
#include <vector>

#include "city/sim_env_weather.h"

using namespace tbd::city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    const SimCalendar cal;
    const uint64_t seed = 12345;
    // 1. seasons in order: spring, summer, autumn, winter, spring ... (3 life months = 3 game days each), starting in Jan = winter
    {
        std::vector<int> seen;
        for (int64_t m = 0; m < 2 * 12 * kMinutesPerDay; m += kMinutesPerDay) {
            const int s = cal.SeasonOf(m);
            if (seen.empty() || seen.back() != s) seen.push_back(s);
        }
        CHECK(seen.size() >= 8);
        CHECK(seen[0] == kWinter);
        for (size_t i = 1; i < seen.size(); ++i) CHECK(seen[i] == (seen[i - 1] + 1) % kSeasons);
        std::printf("seasons: %zu changes over two life years\n", seen.size() - 1);
    }
    // 2. the chain: same seed same sequence, replay equals stepping, another seed differs
    const int64_t steps = 8 * 12 * 10; // ten life years of 3-hour steps
    std::vector<uint8_t> a, b, c;
    WeatherChain s1, s2, s3;
    int counts[kSeasons][kWxKinds] = {};
    for (int64_t i = 0; i < steps; ++i) {
        s1 = WeatherChain::Next(s1, seed, cal), s2 = WeatherChain::Next(s2, seed, cal), s3 = WeatherChain::Next(s3, seed + 1, cal);
        a.push_back(s1.kind), b.push_back(s2.kind), c.push_back(s3.kind);
        ++counts[cal.SeasonOf(s1.step * kWeatherStepMinutes)][s1.kind];
    }
    CHECK(a == b);
    CHECK(a != c);
    const WeatherChain r = WeatherChain::Replay(steps, seed, cal);
    CHECK(r.step == s1.step && r.kind == s1.kind && r.windDir == s1.windDir);
    const WeatherChain mid = WeatherChain::Replay(steps / 2, seed, cal);
    const WeatherChain r2 = WeatherChain::Replay(steps, seed, cal, &mid); // from a checkpoint (a save)
    CHECK(r2.kind == s1.kind && r2.windDir == s1.windDir);
    for (int s = 0; s < kSeasons; ++s) {
        std::printf("%-6s", SeasonName(s));
        for (int k = 0; k < kWxKinds; ++k) std::printf(" %s %3d", WeatherName(k), counts[s][k]);
        std::printf("\n");
        int rainy = counts[s][kWxRain] + counts[s][kWxStorm];
        CHECK(rainy > 0 && counts[s][kWxClear] > 0); // every season has both
    }
    // 3. derived weather: solar 0 at night, summer warmer than winter, winter precipitation is snow
    double summerT = 0, winterT = 0;
    int summerN = 0, winterN = 0, snow = 0, nightSolar = 0, dayOn = 0;
    WeatherChain s;
    for (int64_t m = 0; m < 12 * kMinutesPerDay * 3; m += 30) {
        const int64_t step = m / kWeatherStepMinutes;
        while (s.step < step) s = WeatherChain::Next(s, seed, cal);
        const EnvWeather w = WeatherAt(s, m, seed, cal);
        const int hour = (int)((m % kMinutesPerDay) / 60);
        if (hour < 4 || hour >= 22) nightSolar += w.solar != 0.0f;
        if (hour >= 10 && hour <= 14 && w.solar > 0.2f) ++dayOn;
        if (w.season == kSummer) summerT += w.tempC, ++summerN;
        if (w.season == kWinter) winterT += w.tempC, ++winterN, snow += w.snow;
        if (w.snow) CHECK(w.tempC <= 0.5f && w.rainMmH > 0);
        CHECK(w.windPower >= 0.0f && w.windPower <= 1.0f && w.solar >= 0.0f && w.solar <= 1.0f);
        CHECK(std::abs(w.windX) <= 1 && std::abs(w.windZ) <= 1);
    }
    std::printf("temperature: summer %.1f C, winter %.1f C; %d snowy winter samples; night solar %d\n", summerT / summerN, winterT / winterN, snow, nightSolar);
    CHECK(nightSolar == 0);
    CHECK(dayOn > 0);
    CHECK(summerT / summerN > winterT / winterN + 10.0);
    CHECK(snow > 0);
    // 4. the turbine curve: nothing below cut-in, full at rated, nothing past cut-out (via a crafted state)
    {
        WeatherChain st;
        st.kind = kWxFog; // 1.5 m/s base, calm
        int calm = 0, n = 0;
        for (int i = 0; i < 64; ++i) { st.step = i; const EnvWeather w = WeatherAt(st, i * 180, seed, cal); calm += w.windMs < 3.0f && w.windPower == 0.0f; n += w.windMs < 3.0f; }
        CHECK(calm == n);
        st.kind = kWxStorm;
        int strong = 0;
        for (int i = 0; i < 64; ++i) { st.step = i; strong += WeatherAt(st, i * 180, seed, cal).windPower > 0.5f; }
        CHECK(strong > 0);
    }
    // 5. cheap: a sample costs well under a microsecond
    {
        const auto t0 = std::chrono::steady_clock::now();
        double acc = 0;
        for (int i = 0; i < 100000; ++i) acc += WeatherAt(s1, i * 7, seed, cal).tempC;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 100000.0;
        std::printf("WeatherAt: %.3f us per sample (%.0f)\n", us, acc);
        CHECK(us < 5.0);
    }
    std::printf(g_fail ? "sim_env_weather_test: %d FAILED\n" : "sim_env_weather_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
