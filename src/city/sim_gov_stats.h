// Statistics history (CS2 sim roadmap Phase 7): one sample per game day of the city's headline numbers, kept for the
// Statistics panel's sparklines. Header only, headless (tests\sim_gov_test). Integers only (saved raw, deterministic).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tbd {
namespace city {

enum GovSeries : int {
    kStatPopulation, kStatJobs, kStatWorkers, kStatMoney, kStatIncome, kStatExpenses,
    kStatHappiness, // mean citizen happiness, permille
    kStatTraffic,   // share of the flowing roads that are slow, permille
    kStatPollution, // mean air pollution of the occupied cells, 0..255 x 10
    kStatSeries
};
inline const char* GovSeriesName(int s) {
    static const char* const k[kStatSeries] = {"Population", "Jobs", "Workers", "Money", "Income", "Expenses", "Happiness", "Traffic", "Pollution"};
    return s >= 0 && s < kStatSeries ? k[s] : "";
}

struct GovStats {
    static constexpr int kDays = 120;
    std::vector<int64_t> s[kStatSeries]; // oldest first, at most kDays each (all the same length)

    void Clear() { for (auto& v : s) v.clear(); }
    int Days() const { return (int)s[0].size(); }
    void Push(const int64_t (&day)[kStatSeries]) {
        for (int i = 0; i < kStatSeries; ++i) {
            s[i].push_back(day[i]);
            if ((int)s[i].size() > kDays) s[i].erase(s[i].begin());
        }
    }
    int64_t Last(int series) const { return s[series].empty() ? 0 : s[series].back(); }
    std::vector<float> Floats(int series, float scale = 1.0f) const {
        std::vector<float> f;
        f.reserve(s[series].size());
        for (int64_t v : s[series]) f.push_back((float)v * scale);
        return f;
    }
};

// Happiness is summed in the same 24 hourly groups as the taxes (one group per game hour, round robin).
struct GovHappyAccum {
    int64_t sum = 0, count = 0;
    void Clear() { sum = count = 0; }
    int64_t Permille() const { return count ? sum * 1000 / (count * 255) : 0; }
};

} // namespace city
} // namespace tbd
