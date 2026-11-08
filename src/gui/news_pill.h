// The City News card's collapsed / expanded state and its unread count (src/game/dock_news_stats.inl draws it).
// Header only, headless (tests\gui_test.cpp). The feed is newest first; a headline is keyed by its time and text.
//   Expanded  everything shown is read: the newest headline is remembered as seen.
//   Collapsed the pill counts the headlines newer than the last one seen (a fresh map's feed starts empty: nothing unread).
#pragma once
#include <string>
#include <vector>

namespace tbd {
namespace gui {

struct NewsPill {
    bool expanded = false; // the player's choice (the host may still fold the card while the build palette is out)
    std::string seen;      // the newest headline when the card was last on screen open ("" none)
    int unread = 0;

    static std::string Key(const std::string& when, const std::string& text) { return when + "|" + text; }
    // Per frame with the feed's keys, newest first; shownOpen: the open card is on screen this frame.
    void Update(const std::vector<std::string>& newestFirst, bool shownOpen) {
        if (shownOpen) {
            seen = newestFirst.empty() ? std::string() : newestFirst.front();
            unread = 0;
            return;
        }
        int n = 0;
        for (const std::string& k : newestFirst) {
            if (k == seen) break;
            ++n;
        }
        unread = n;
    }
    void Toggle() { expanded = !expanded; }
};

} // namespace gui
} // namespace tbd
