// How often the city radio and the City News feed may speak (city_radio.inl asks this before every clip or headline).
// Header only, headless (tests\sim_news_test). The user's rule: the news is rare, never "every time I do the thing".
//
//   Major   milestones (population marks, milestone_reached), bankruptcy and recovery (budget_deficit / budget_surplus),
//           disasters (flood, fire, outage, shortage). Run when the gaps allow; if the global gap is closed the latest
//           one is held (one slot) and runs as soon as it opens, within kHoldDays.
//   Minor   the first of each kind of building (first school, first power plant, first bus depot, ...), a repeat of a
//           kind only for a big lot (kBigLotCells) and only every kSameKindDays, a neighbour roast for a clearly bad pair
//           (every kRoastGapDays), taxes, jobs, a new transit line, a loan. Dropped when a gap is closed.
//   Skip    routine placements: roads (built or bulldozed), bus stops, repeat small buildings, good neighbour pairs.
//           Never a clip, never a headline.
// Every clip or headline (any tier) waits kAnyGapDays game days AND kAnyGapRealSec real seconds after the last one,
// and one trigger waits kSameTriggerDays. The Weekly Rundown (city_radio.inl) is separate, once a game week.
// Deterministic, no allocation per frame (the maps grow by one entry per trigger / kind ever seen).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace city {

// ---- tunables
namespace news_tuning {
constexpr double kDayMinutes = 1440.0;     // game minutes in a game day
constexpr double kAnyGapDays = 2.0;        // any clip or headline: at most one every 2 game days ...
constexpr double kAnyGapRealSec = 240.0;   // ... and every 4 real minutes, whatever the clock speed (both must pass)
constexpr double kSameTriggerDays = 10.0;  // the same trigger ("tax_raised", "power_plant_built") at most every 10 game days
constexpr double kSameKindDays = 10.0;     // the same kind of building story (an opening or a roast about it) every 10 days
constexpr double kRoastGapDays = 5.0;      // between two neighbour roasts, whatever the pair
constexpr double kHoldDays = 7.0;          // a held Major story is dropped if the gap has not opened within a week
constexpr int kBigLotCells = 24;           // a repeat opening of a kind is news only on a lot of this many 8 m cells or more
} // namespace news_tuning

enum class NewsTier : uint8_t { Skip, Minor, Major };

// The tier of a trigger by itself (openings and roasts are decided by NewsGate::Tier with their context).
inline NewsTier TriggerTier(const std::string& t) {
    static const char* const major[] = {"population_1k", "population_10k", "population_50k", "milestone_reached", "budget_deficit",
                                        "budget_surplus", "bankrupt", "flood", "fire_outbreak", "power_outage", "water_shortage",
                                        "disaster_recovered"};
    static const char* const skip[] = {"road_bulldozed", "bus_stop_opened", "random", "zone_residential_filled",
                                       "zone_commercial_filled", "zone_industrial_filled", "tree_planted"};
    for (const char* m : major)
        if (t == m) return NewsTier::Major;
    for (const char* s : skip)
        if (t == s) return NewsTier::Skip;
    return t.empty() ? NewsTier::Skip : NewsTier::Minor;
}

struct NewsStory {
    std::string trigger;    // the clip / cooldown key
    std::string kind;       // the building kind the story is about ("school", or the catalog name), "" for none
    bool opening = false;   // a building opened (the first-of-kind rule)
    bool big = false;       // its lot is kBigLotCells or more
    bool roast = false;     // a clearly bad neighbour pair (the caller checks the pair's tone)
};

class NewsGate {
public:
    // A new map or a load: forget every time (the clock may have gone backwards) and every kind seen.
    void Reset() {
        lastAny_ = lastRoast_ = -1e18, lastReal_ = -1e18;
        lastTrigger_.clear(), lastKind_.clear(), seenKinds_.clear();
        haveHeld_ = false;
    }
    // What stands in the city already is not "the first" of its kind.
    void MarkKindSeen(const std::string& kind) {
        if (!kind.empty()) seenKinds_.insert(kind);
    }
    bool KindSeen(const std::string& kind) const { return seenKinds_.count(kind) != 0; }

    NewsTier Tier(const NewsStory& s) const {
        const NewsTier t = TriggerTier(s.trigger);
        if (t == NewsTier::Skip) return t;
        if (s.roast) return NewsTier::Minor;
        if (s.opening) return !KindSeen(s.kind) || s.big ? NewsTier::Minor : NewsTier::Skip;
        return t;
    }

    bool GapOpen(double minute, double realSec) const {
        using namespace news_tuning;
        return minute - lastAny_ >= kAnyGapDays * kDayMinutes && realSec - lastReal_ >= kAnyGapRealSec;
    }

    // True: post it now (clip and / or headline; the gate has recorded it). False: skipped, on a cooldown, or held.
    // heldOut (optional) is set when a Major story was held for later.
    bool Offer(const NewsStory& s, double minute, double realSec, bool* heldOut = nullptr) {
        using namespace news_tuning;
        if (heldOut) *heldOut = false;
        const NewsTier tier = Tier(s);
        if (s.opening) MarkKindSeen(s.kind); // seen even when the story does not run: the next one is not "the first"
        if (tier == NewsTier::Skip) return false;
        if (Within(lastTrigger_, s.trigger, minute, kSameTriggerDays)) return false;
        if (!s.kind.empty() && Within(lastKind_, s.kind, minute, kSameKindDays)) return false;
        if (s.roast && minute - lastRoast_ < kRoastGapDays * kDayMinutes) return false;
        if (!GapOpen(minute, realSec)) {
            if (tier == NewsTier::Major) {
                held_ = s, heldAt_ = minute, haveHeld_ = true;
                if (heldOut) *heldOut = true;
            }
            return false;
        }
        Record(s, minute, realSec);
        return true;
    }

    // A held Major story, once the gap has opened (true once, the story in out). Cheap to call every frame.
    bool TakeHeld(double minute, double realSec, NewsStory& out) {
        if (!haveHeld_) return false;
        if (minute - heldAt_ > news_tuning::kHoldDays * news_tuning::kDayMinutes || minute < heldAt_) {
            haveHeld_ = false;
            return false;
        }
        if (!GapOpen(minute, realSec)) return false;
        haveHeld_ = false;
        Record(held_, minute, realSec);
        out = held_;
        return true;
    }
    bool Holding() const { return haveHeld_; }

    // Something else took the airtime (The Weekly Rundown, a dev test clip): the gap starts again from now.
    void NoteAired(double minute, double realSec) { lastAny_ = minute, lastReal_ = realSec; }

private:
    static bool Within(const std::unordered_map<std::string, double>& m, const std::string& k, double minute, double days) {
        auto it = m.find(k);
        return it != m.end() && minute - it->second < days * news_tuning::kDayMinutes;
    }
    void Record(const NewsStory& s, double minute, double realSec) {
        lastAny_ = minute, lastReal_ = realSec;
        lastTrigger_[s.trigger] = minute;
        if (!s.kind.empty()) lastKind_[s.kind] = minute;
        if (s.roast) lastRoast_ = minute;
    }

    double lastAny_ = -1e18, lastReal_ = -1e18, lastRoast_ = -1e18;
    std::unordered_map<std::string, double> lastTrigger_, lastKind_;
    std::unordered_set<std::string> seenKinds_;
    NewsStory held_;
    double heldAt_ = 0.0;
    bool haveHeld_ = false;
};

} // namespace city
