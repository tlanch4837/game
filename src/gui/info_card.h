// The info card: what a tap / click with no tool out picked (src/city_interact.inl fills it from the sim). Glass Dock+ style
// (src/gui/glass_dock.h's painter), compact, top left: 16 pt from the left edge (64 on a phone, clear of the Dynamic Island
// in landscape) and 60 pt from the top (under the Phone UI's status pill; the top 50 pt stay clear on the iPad). The news
// pill / card own the top right. The ✕ closes it; it never covers more than the top 60% of the screen.
// Platform free: the host passes the data, the card says when it was closed.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "glass_dock.h"

namespace tbd {
namespace gui {

struct InfoCardData {
    std::string title, sub, icon; // icon: an atlas name ("house", "power", "bus" ...)
    std::vector<std::pair<std::string, std::string>> rows; // label, value
    std::vector<std::string> notes; // free lines under the rows (residents, problems)
    Color accent = Rgba(10, 132, 255);
};

// Draws the card; true when its close button was tapped this frame. phone: the iPhone layout (wider left margin).
inline bool DrawInfoCard(Context& g, const dock::Atlas& atlas, const InfoCardData& d, bool phone, float textScale) {
    using namespace dock;
    const float s = g.Scale();
    Painter P(g, atlas, s);
    P.text = textScale;
    const Input& in = g.In();
    const float W = in.viewW / s, H = in.viewH / s;
    const float x = phone ? 64.0f : 16.0f, y = 60.0f;
    const float w = std::min(300.0f, W - x - 16.0f), pad = 12.0f, rowH = 18.0f * textScale, noteH = 16.0f * textScale;
    const float labelW = 104.0f;
    const float headH = 40.0f * textScale;
    const float maxH = std::max(140.0f, H * 0.6f - y);
    float h = pad + headH + 6.0f + pad;
    size_t rows = 0, notes = 0;
    for (; rows < d.rows.size() && h + rowH <= maxH; ++rows) h += rowH;
    if (!d.notes.empty() && h + 8.0f + noteH <= maxH) h += 8.0f;
    for (; notes < d.notes.size() && h + noteH <= maxH; ++notes) h += noteH;
    P.Glass(x, y, w, h, 18, kPanel, 22, 1.6f, true);
    // header: icon chip, title, subtitle, close
    const float iy = y + pad + headH * 0.5f;
    P.Box(x + pad, iy - 15.0f, 30.0f, 30.0f, 10.0f, Fade(d.accent, 0.35f), 1.0f, Fade(d.accent, 0.8f));
    P.Icon(d.icon.empty() ? "info" : d.icon, x + pad + 15.0f, iy, 18.0f, kInk, false);
    const float tx = x + pad + 30.0f + 9.0f, bs = 26.0f, bx = x + w - pad - bs + 4.0f, by = y + pad;
    const float textW = bx - 6.0f - tx;
    P.Text(d.title, tx, iy - 8.0f * textScale, 14.0f, kInk, true, 0.0f, textW);
    if (!d.sub.empty()) P.Text(d.sub, tx, iy + 9.0f * textScale, 11.5f, kMuted, false, 0.0f, textW);
    bool closed = false;
    {
        const Rect hr{bx * s, by * s, bs * s, bs * s};
        const bool inside = hr.Contains(in.mouseX, in.mouseY), pressedIn = hr.Contains(in.pressX, in.pressY);
        const bool held = in.down && pressedIn && inside;
        if (in.released && pressedIn && inside) closed = true;
        P.Box(bx, by, bs, bs, bs * 0.5f, held ? Rgba(255, 255, 255, 60) : kChip);
        P.Icon("x", bx + bs * 0.5f, by + bs * 0.5f, 14.0f, kInk, false);
    }
    float cy = y + pad + headH + 6.0f;
    P.Box(x + pad, cy - 3.0f, w - 2 * pad, 1.0f, 0, kBorder);
    for (size_t i = 0; i < rows; ++i) {
        const float my = cy + rowH * 0.5f;
        P.Text(d.rows[i].first, x + pad, my, 11.5f, kMuted, false, 0.0f, labelW - 6.0f);
        P.Text(d.rows[i].second, x + pad + labelW, my, 12.0f, kInk, true, 0.0f, w - 2 * pad - labelW);
        cy += rowH;
    }
    if (notes) cy += 8.0f;
    for (size_t i = 0; i < notes; ++i) {
        P.Text(d.notes[i], x + pad, cy + noteH * 0.5f, 11.0f, kBarInk, false, 0.0f, w - 2 * pad);
        cy += noteH;
    }
    return closed;
}

} // namespace gui
} // namespace tbd
