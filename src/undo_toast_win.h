// The undo popup (Windows only): a borderless owned window saying what was undone or redone.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace tbd::undo {

struct ToastState {
    HWND toast = nullptr;
    std::wstring toastText;
};
inline ToastState& TS() { static ToastState s; return s; }

// ---- the popup: a borderless owned window over the bottom right of `area` (screen coordinates), gone after 1.6 s
inline LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(36, 32, 28)), edge = CreateSolidBrush(RGB(196, 120, 52));
        FillRect(dc, &r, bg);
        RECT bar = {0, 0, 4, r.bottom};
        FillRect(dc, &bar, edge);
        DeleteObject(bg);
        DeleteObject(edge);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(236, 228, 216));
        HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT t = {14, 0, r.right - 8, r.bottom};
        DrawTextW(dc, TS().toastText.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_TIMER) { KillTimer(h, 1); ShowWindow(h, SW_HIDE); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

inline void Toast(HWND owner, const RECT& area, const std::string& text) {
    ToastState& s = TS();
    if (!s.toast) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"TBDUndoToast";
        RegisterClassW(&wc);
        s.toast = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, owner, nullptr, wc.hInstance, nullptr);
    }
    if (!s.toast) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    s.toastText.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), s.toastText.data(), n);
    const UINT dpi = GetDpiForWindow(owner);
    const int scale = dpi ? (int)dpi : 96;
    const int w = MulDiv(300, scale, 96), h = MulDiv(32, scale, 96), pad = MulDiv(14, scale, 96);
    SetWindowPos(s.toast, HWND_TOP, area.right - w - pad, area.bottom - h - pad, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s.toast, nullptr, TRUE);
    SetTimer(s.toast, 1, 1600, nullptr);
}

} // namespace tbd::undo
