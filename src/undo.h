// Engine-wide undo / redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z): one stack of named steps. Each step carries its own undo and redo.
// Who pushes: main.cpp's scene checkpoint (any entity edit: move / rotate / scale, rename, create, delete, properties; one step
// per finished mouse or key action) and the Character Creator (sliders, randomize, skin tone, hair strokes, fill / clear).
// A small popup at the viewport's bottom right says what was undone or redone ("Undid Move Head").
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace tbd::undo {

struct Step {
    std::string label;
    std::function<void()> undo, redo;
};

struct Stacks {
    std::vector<Step> done, undone;
    int absorb = 0; // > 0: the scene checkpoint takes the scene's next change as its new baseline without a step (a tool made it)
    HWND toast = nullptr;
    std::wstring toastText;
};
inline Stacks& S() { static Stacks s; return s; }

constexpr size_t kMaxSteps = 200;

inline void Push(const std::string& label, std::function<void()> undo, std::function<void()> redo) {
    Stacks& s = S();
    s.done.push_back({label, std::move(undo), std::move(redo)});
    if (s.done.size() > kMaxSteps) s.done.erase(s.done.begin());
    s.undone.clear();
}
// A tool changed the scene's entities itself and recorded (or did not want) its own step: the scene checkpoint must not add one.
inline void AbsorbSceneChange() { ++S().absorb; }

inline bool Undo(std::string* label) {
    Stacks& s = S();
    if (s.done.empty()) return false;
    Step st = std::move(s.done.back());
    s.done.pop_back();
    if (st.undo) st.undo();
    *label = st.label;
    s.undone.push_back(std::move(st));
    return true;
}
inline bool Redo(std::string* label) {
    Stacks& s = S();
    if (s.undone.empty()) return false;
    Step st = std::move(s.undone.back());
    s.undone.pop_back();
    if (st.redo) st.redo();
    *label = st.label;
    s.done.push_back(std::move(st));
    return true;
}

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
        DrawTextW(dc, S().toastText.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_TIMER) { KillTimer(h, 1); ShowWindow(h, SW_HIDE); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

inline void Toast(HWND owner, const RECT& area, const std::string& text) {
    Stacks& s = S();
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
