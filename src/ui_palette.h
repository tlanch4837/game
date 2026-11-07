// The editor's colour palette, portable: 0x00BBGGRR values, the same layout as Windows' COLORREF / RGB().
// ui_theme.h (Win32 panels) and profiler_overlay.h (drawn by the renderer) both use it.
#pragma once
#include <cstdint>

namespace tbd {
namespace theme {

constexpr uint32_t Rgb8(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16); }
constexpr uint8_t RedOf(uint32_t c) { return (uint8_t)(c & 0xff); }
constexpr uint8_t GreenOf(uint32_t c) { return (uint8_t)((c >> 8) & 0xff); }
constexpr uint8_t BlueOf(uint32_t c) { return (uint8_t)((c >> 16) & 0xff); }

inline constexpr uint32_t kWindow = Rgb8(0x1a, 0x1a, 0x1a);  // gaps between panels, tab strips, text fields
inline constexpr uint32_t kPanel = Rgb8(0x24, 0x24, 0x24);   // panel body
inline constexpr uint32_t kStripe = Rgb8(0x27, 0x27, 0x27);  // every other list row
inline constexpr uint32_t kRaised = Rgb8(0x30, 0x30, 0x30);  // toggled-on and pressed-in flat buttons
inline constexpr uint32_t kHover = Rgb8(0x3a, 0x3a, 0x3a);   // flat button / row under the cursor
inline constexpr uint32_t kLine = Rgb8(0x0f, 0x0f, 0x0f);    // panel edges
inline constexpr uint32_t kRule = Rgb8(0x3a, 0x3a, 0x3a);    // thin separators inside a panel
inline constexpr uint32_t kText = Rgb8(0xcc, 0xcc, 0xcc);
inline constexpr uint32_t kTextBright = Rgb8(0xf2, 0xf2, 0xf2);
inline constexpr uint32_t kMuted = Rgb8(0x8c, 0x8c, 0x8c);
inline constexpr uint32_t kDisabled = Rgb8(0x5c, 0x5c, 0x5c);
inline constexpr uint32_t kAccent = Rgb8(0x00, 0x70, 0xe0);  // the one accent: selection, active tool, focus

} // namespace theme
} // namespace tbd
