// Text measurement and glyph lookup for the in-game GUI's SDF font (assets/ui/ui_font_sdf.png, metrics generated into
// ui_font_metrics.h by tools/bake_ui_font.py). Platform free and headless: layout and the unit tests measure text with the
// same numbers the renderer draws with.
#pragma once
#include <cstddef>

#include "ui_font_metrics.h"

namespace tbd {
namespace gui {

enum class FontFace { Regular = 0, Bold = 1 };

inline const font_data::Face& Face(FontFace face) { return font_data::kFaceList[(int)face]; }

// The glyph for c, or nullptr outside ASCII 32..126 (drawn as nothing, advancing like a space).
inline const font_data::Glyph* FindSdfGlyph(FontFace face, char c) {
    const int code = (unsigned char)c;
    if (code < font_data::kFirst || code > font_data::kLast) return nullptr;
    return &Face(face).glyphs[code - font_data::kFirst];
}

// Pair kerning in em (the tables are sorted; empty when the baker found none).
inline float SdfKerning(FontFace face, char a, char b) {
    const font_data::Face& f = Face(face);
    int lo = 0, hi = f.kerningCount - 1;
    const int key = ((unsigned char)a << 8) | (unsigned char)b;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int k = (f.kerning[mid].a << 8) | f.kerning[mid].b;
        if (k == key) return f.kerning[mid].em;
        if (k < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0.0f;
}

inline float SdfAdvance(FontFace face, char c) {
    const font_data::Glyph* g = FindSdfGlyph(face, c);
    return g ? g->advance : FindSdfGlyph(face, ' ')->advance;
}

// Width in pixels of n characters at `size` pixels per em: advances plus kerning.
inline float MeasureSdfText(const char* s, size_t n, float size, FontFace face = FontFace::Regular) {
    float w = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        w += SdfAdvance(face, s[i]);
        if (i + 1 < n) w += SdfKerning(face, s[i], s[i + 1]);
    }
    return w * size;
}

inline float SdfCapHeight(float size, FontFace face = FontFace::Regular) { return Face(face).capHeight * size; }

} // namespace gui
} // namespace tbd
