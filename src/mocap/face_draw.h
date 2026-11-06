// GDI drawing shared by the clip preview and the live webcam window: video with mesh overlay, head pose, channel bars.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

#include "face_clip.h"

namespace tbd::facedraw {

constexpr COLORREF kBg = RGB(32, 28, 24);
constexpr COLORREF kPanel = RGB(24, 20, 16);
constexpr COLORREF kText = RGB(228, 218, 202);
constexpr COLORREF kMuted = RGB(150, 140, 125);
constexpr COLORREF kAccent = RGB(176, 112, 48);
constexpr COLORREF kMesh = RGB(90, 230, 120);

struct Style {
    HFONT font = nullptr, small = nullptr;
    int dpi = 96;
    int S(int v) const { return MulDiv(v, dpi, 96); }
};

void CreateFonts(Style& s);
void DestroyFonts(Style& s);

void Fill(HDC dc, int l, int t, int r, int b, COLORREF c);
void Text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, HFONT f, UINT align = TA_LEFT);

// Draws `bgra` (vw x vh, top-down) fitted into `area` on a panel background, with the mesh of `f` over it when
// `f` has a face. `mirror` flips it like a mirror (for a live camera). Returns where the picture landed.
RECT DrawVideo(HDC dc, const RECT& area, const uint8_t* bgra, int vw, int vh, const FaceFrame* f, bool mirror,
               const Style& st);

// "Head pose" and "Expression" blocks down the right-hand side. Returns the y below them.
int DrawPoseAndChannels(HDC dc, int x0, int y, int width, const FaceFrame& f, const Style& st);

} // namespace tbd::facedraw
