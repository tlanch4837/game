#define UNICODE
#define _UNICODE
#include "face_draw.h"

#include <algorithm>
#include <cstdio>

namespace tbd::facedraw {

void CreateFonts(Style& s) {
    s.font = CreateFontW(-s.S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s.small = CreateFontW(-s.S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

void DestroyFonts(Style& s) {
    if (s.font) DeleteObject(s.font);
    if (s.small) DeleteObject(s.small);
    s.font = s.small = nullptr;
}

void Fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void Text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, HFONT f, UINT align) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
}

RECT DrawVideo(HDC dc, const RECT& area, const uint8_t* bgra, int vw, int vh, const FaceFrame* f, bool mirror,
               const Style& st) {
    Fill(dc, area.left, area.top, area.right, area.bottom, kPanel);
    RECT fit = area;
    if (!bgra || vw <= 0 || vh <= 0 || area.right <= area.left || area.bottom <= area.top) return fit;

    const float s = std::min((float)(area.right - area.left) / vw, (float)(area.bottom - area.top) / vh);
    const int dw = (int)(vw * s), dh = (int)(vh * s);
    fit.left = (area.left + area.right - dw) / 2;
    fit.top = (area.top + area.bottom - dh) / 2;
    fit.right = fit.left + dw;
    fit.bottom = fit.top + dh;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), vw, -vh, 1, 32, BI_RGB};
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    if (mirror) StretchDIBits(dc, fit.right - 1, fit.top, -dw, dh, 0, 0, vw, vh, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
    else StretchDIBits(dc, fit.left, fit.top, dw, dh, 0, 0, vw, vh, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);

    if (f && f->valid) {
        const int d = std::max(1, st.S(1) - 1);
        HBRUSH br = CreateSolidBrush(kMesh);
        for (int i = 0; i < kFaceLandmarks; ++i) {
            float u = f->lm[i * 3];
            if (mirror) u = 1.0f - u;
            const int x = fit.left + (int)(u * dw), y = fit.top + (int)(f->lm[i * 3 + 1] * dh);
            RECT dot{x - d / 2, y - d / 2, x - d / 2 + d + 1, y - d / 2 + d + 1};
            FillRect(dc, &dot, br);
        }
        DeleteObject(br);
    } else {
        Text(dc, fit.left + st.S(8), fit.top + st.S(8), L"no face", RGB(255, 120, 100), st.font);
    }
    return fit;
}

int DrawPoseAndChannels(HDC dc, int x0, int y, int width, const FaceFrame& f, const Style& st) {
    const int lh = st.S(18);
    wchar_t buf[96];

    Text(dc, x0, y, L"Head pose", kAccent, st.font);
    y += lh;
    const wchar_t* poseName[3] = {L"Pitch", L"Yaw", L"Roll"};
    for (int i = 0; i < 3; ++i) {
        Text(dc, x0, y, poseName[i], kMuted, st.small);
        swprintf_s(buf, L"%+.1f\x00B0", f.valid ? f.rot[i] : 0.0f);
        Text(dc, x0 + width - st.S(20), y, buf, kText, st.small, TA_RIGHT);
        y += lh - st.S(2);
    }
    y += st.S(8);

    Text(dc, x0, y, L"Expression", kAccent, st.font);
    y += lh;
    const int barX = x0 + st.S(104), barW = width - st.S(130);
    for (int c = 0; c < kFaceChannels; ++c) {
        wchar_t name[40];
        MultiByteToWideChar(CP_UTF8, 0, kFaceChannelNames[c], -1, name, 40);
        Text(dc, x0, y, name, kMuted, st.small);
        Fill(dc, barX, y + st.S(3), barX + barW, y + lh - st.S(4), kPanel);
        const float w = f.valid ? f.weight[c] : 0.0f;
        Fill(dc, barX, y + st.S(3), barX + (int)(barW * w), y + lh - st.S(4), kAccent);
        y += lh;
    }
    return y;
}

} // namespace tbd::facedraw
