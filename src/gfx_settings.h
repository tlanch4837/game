// Player graphics options: a preset (Low / Med / High / Ultra) or Custom, and the individual settings behind it. Platform free (no
// OS or renderer headers): the Glass Dock settings page edits a Settings, the game reads it through tbd::gfx::g, and the host saves
// it as text. While `userSet` is false nothing here applies and the TBD_* environment switches rule as before.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace tbd::gfx {

// The 3D scene's internal render resolution, percent of the screen's native pixels (the last step, "Native", is 100%). The GUI
// is never scaled by it: it draws on the full-size drawable after the scene is upscaled (renderer: the overlay pass).
// Version 2 of the steps (settings.ini "scalev=2"); version 1 was 50 / 67 / 75 / 85 / 100, each the nearest of the new ones.
constexpr int kScaleCount = 5, kScalePct[kScaleCount] = {50, 60, 75, 90, 100};
constexpr int kScaleVersion = 2;
inline const char* ScaleLabel(int i) {
    static const char* const k[kScaleCount] = {"50%", "60%", "75%", "90%", "Native"};
    return k[std::clamp(i, 0, kScaleCount - 1)];
}
// Each preset's resolution step. iOS draws at the screen's native pixels (2x or 3x points), four times the pixels of the
// 1x drawable it had before the GUI sharpness fix, so its presets keep the scene nearer that cost; the player can still pick
// any step, Native included, on the Graphics page.
#if defined(__APPLE__)
constexpr int kPresetScale[4] = {0, 1, 1, 2}; // Low 50%, Med 60%, High 60%, Ultra 75%
#else
constexpr int kPresetScale[4] = {1, 2, 3, 4}; // Low 60%, Med 75%, High 90%, Ultra native
#endif
constexpr int kDefaultScale = kPresetScale[2]; // before the player has a settings file: the High preset's step
constexpr int kDistanceCount = 3, kRadius[kDistanceCount] = {5, 8, 12};       // terrain chunks drawn: Near, Medium, Far
constexpr int kTiers = 5;                                                     // lighting/post tier and building detail: Low, Low/Med, Med, High, Ultra

enum Preset { kLow = 0, kMed = 1, kHigh = 2, kUltra = 3, kCustom = 4 };

struct Settings {
    int preset = kHigh;
    int scale = kDefaultScale; // index into kScalePct
    int look = 3;      // lighting and post tier 0..4 (shadow taps, AO, bloom)
    int detail = 3;    // building detail tier 0..4 (the zoned-building draw and triangle budget)
    int distance = 1;  // index into kRadius
    int shadows = 1;   // sun shadows
    int lighting = 0;  // 0 full, 1 fast (no ray tracing, probes, clouds, relief), 2 unlit
};

inline void ApplyPreset(Settings& s, int preset) {
    //                      scale look detail dist shadows lighting
    static const int t[4][6] = {{0, 0, 0, 0, 1, 1},   // Low: fast lighting
                                {0, 1, 1, 1, 1, 0},   // Med
                                {0, 3, 3, 1, 1, 0},   // High
                                {0, 4, 4, 2, 1, 0}};  // Ultra (resolution: kPresetScale)
    preset = std::clamp(preset, 0, 3);
    s.preset = preset;
    s.scale = kPresetScale[preset], s.look = t[preset][1], s.detail = t[preset][2];
    s.distance = t[preset][3], s.shadows = t[preset][4], s.lighting = t[preset][5];
}

// Any single setting changed by hand makes the whole thing Custom.
inline void Clamp(Settings& s) {
    s.preset = std::clamp(s.preset, 0, 4);
    s.scale = std::clamp(s.scale, 0, kScaleCount - 1);
    s.look = std::clamp(s.look, 0, kTiers - 1);
    s.detail = std::clamp(s.detail, 0, kTiers - 1);
    s.distance = std::clamp(s.distance, 0, kDistanceCount - 1);
    s.shadows = s.shadows ? 1 : 0;
    s.lighting = std::clamp(s.lighting, 0, 2);
}

inline Settings g;          // what the game reads
inline bool userSet = false; // a settings file was loaded or the page was used

inline float ScaleFraction() { return (float)kScalePct[std::clamp(g.scale, 0, kScaleCount - 1)] / 100.0f; }

// "key=value" lines. Unknown keys are ignored so older files keep loading.
inline std::string Serialize(const Settings& s) {
    std::ostringstream o;
    o << "preset=" << s.preset << "\nscale=" << s.scale << "\nlook=" << s.look << "\ndetail=" << s.detail << "\ndistance=" << s.distance
      << "\nshadows=" << s.shadows << "\nlighting=" << s.lighting << "\nscalev=" << kScaleVersion << '\n';
    return o.str();
}

// Reads one key out of a settings text; false when absent.
inline bool Find(const std::string& text, const char* key, int& out) {
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while ((at = text.find(k, at)) != std::string::npos) {
        if (at == 0 || text[at - 1] == '\n') {
            out = std::atoi(text.c_str() + at + k.size());
            return true;
        }
        at += k.size();
    }
    return false;
}

inline void Parse(const std::string& text, Settings& s) {
    Find(text, "preset", s.preset), Find(text, "scale", s.scale), Find(text, "look", s.look), Find(text, "detail", s.detail);
    Find(text, "distance", s.distance), Find(text, "shadows", s.shadows), Find(text, "lighting", s.lighting);
    int version = 1;
    Find(text, "scalev", version);
    Clamp(s);
    // an older file: a preset takes its current resolution step; a Custom pick keeps its index (the nearest new step)
    if (version < kScaleVersion && s.preset < kCustom) s.scale = kPresetScale[s.preset];
}

} // namespace tbd::gfx
