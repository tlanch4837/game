// News radio feed: game events post a trigger key, the feed picks a headline for it.
// Source lines live in news_lines.json (fields: trigger, tone, text).
// Triggers the simulation cannot fire yet are listed in docs/news_ideas_to_implement.md.
#pragma once
#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace city {

struct NewsLine {
    std::string trigger;  // e.g. "park_built", "adj:residential+industry", "random"
    std::string tone;     // "good" | "bad" | "random"
    std::string text;
};

class NewsFeed {
public:
    void Add(NewsLine line) { lines_.push_back(std::move(line)); }

    // Picks a headline for a trigger. Avoids repeating the last line used for that trigger
    // until every line for it has played once. Returns false if nothing matches.
    bool Post(const std::string& trigger, std::string& outText, std::string& outTone) {
        std::vector<int> pool;
        for (int i = 0; i < (int)lines_.size(); ++i)
            if (lines_[i].trigger == trigger) pool.push_back(i);
        if (pool.empty()) return false;

        auto& played = played_[trigger];
        std::vector<int> fresh;
        for (int i : pool)
            if (std::find(played.begin(), played.end(), i) == played.end()) fresh.push_back(i);
        if (fresh.empty()) { played.clear(); fresh = pool; }

        const int pick = fresh[rng_() % fresh.size()];
        played.push_back(pick);
        outText = lines_[pick].text;
        outTone = lines_[pick].tone;
        queue_.push_back({outTone, outText});
        return true;
    }

    // Random filler headline (the "random" tone), for quiet moments.
    bool PostRandom(std::string& outText, std::string& outTone) { return Post("random", outText, outTone); }

    struct Broadcast { std::string tone, text; };
    // Headlines waiting to be read on air, oldest first.
    std::vector<Broadcast>& Queue() { return queue_; }

    size_t Count() const { return lines_.size(); }

private:
    std::vector<NewsLine> lines_;
    std::unordered_map<std::string, std::vector<int>> played_;
    std::vector<Broadcast> queue_;
    std::mt19937 rng_{12345u};
};

// Reads news_lines.json (an array of flat {"trigger","tone","text"} objects) into the feed. Returns the lines added.
// Minimal on purpose: string values with \" and \ escapes, nothing nested.
inline int LoadNewsLines(NewsFeed& feed, const std::string& json) {
    int added = 0;
    size_t i = 0;
    auto str = [&](size_t& k, std::string& out) { // k at the opening quote
        out.clear();
        for (++k; k < json.size() && json[k] != '"'; ++k) {
            if (json[k] == '\\' && k + 1 < json.size()) ++k;
            out.push_back(json[k]);
        }
        ++k;
    };
    while ((i = json.find('{', i)) != std::string::npos) {
        const size_t end = json.find('}', i);
        if (end == std::string::npos) break;
        NewsLine line;
        size_t k = i + 1;
        while (k < end) {
            k = json.find('"', k);
            if (k == std::string::npos || k >= end) break;
            std::string key, val;
            str(k, key);
            k = json.find('"', k);
            if (k == std::string::npos) break;
            str(k, val);
            if (key == "trigger") line.trigger = val;
            else if (key == "tone") line.tone = val;
            else if (key == "text") line.text = val;
        }
        if (!line.trigger.empty() && !line.text.empty()) feed.Add(line), ++added;
        i = std::max(end, k) + 1;
    }
    return added;
}

} // namespace city
