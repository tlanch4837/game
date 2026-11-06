#define UNICODE
#define _UNICODE
#include "face_live_window.h"

#include <commdlg.h>

#include <cstdio>
#include <string>
#include <vector>

#include "face_draw.h"

namespace tbd {
namespace {

using namespace facedraw;

const wchar_t* kClass = L"TBDFaceLive";
constexpr UINT_PTR kTimer = 1;

struct LiveView {
    LiveFace* live = nullptr;
    Style st;
    FaceFrame frame;
    std::vector<uint8_t> image;
    int w = 0, h = 0;
    uint64_t seq = 0;
    bool have = false;

    void Pull() {
        FaceFrame f;
        uint64_t s = 0;
        if (live->Latest(&f, &image, &w, &h, &s) && s != seq) {
            frame = f;
            seq = s;
            have = true;
        }
    }

    void Render(HDC dc, int W, int H) {
        const int m = st.S(10), side = st.S(270), lh = st.S(18);
        Fill(dc, 0, 0, W, H, kBg);
        SetBkMode(dc, TRANSPARENT);
        DrawVideo(dc, RECT{m, m, W - side - 2 * m, H - m}, have ? image.data() : nullptr, w, h, have ? &frame : nullptr,
                  true, st);

        const int x0 = W - side - m + st.S(6);
        int y = m;
        wchar_t buf[128];
        Text(dc, x0, y, L"Live webcam", kText, st.font);
        y += lh;
        swprintf_s(buf, L"%s", live->SourceName().c_str());
        Text(dc, x0, y, buf, kMuted, st.small);
        y += lh;
        swprintf_s(buf, L"%.0f fps   %s", live->TrackingFps(), have && frame.valid ? L"face tracked" : L"no face");
        Text(dc, x0, y, buf, kMuted, st.small);
        y += lh;
        if (!live->Calibrated()) Text(dc, x0, y, L"Calibrating: look at the screen, relaxed...", kAccent, st.small);
        else Text(dc, x0, y, L"Neutral pose set", kMuted, st.small);
        y += lh + st.S(8);

        FaceFrame none;
        y = DrawPoseAndChannels(dc, x0, y, side, have ? frame : none, st);
        y += st.S(10);
        if (live->Recording()) {
            Fill(dc, x0, y + st.S(3), x0 + st.S(10), y + st.S(13), RGB(220, 60, 50));
            Text(dc, x0 + st.S(16), y, L"Recording...  R to stop", RGB(240, 140, 120), st.small);
        } else {
            Text(dc, x0, y, L"R  record a clip", kMuted, st.small);
        }
        Text(dc, x0, y + lh - 4, L"C  re-measure neutral pose", kMuted, st.small);
        Text(dc, x0, y + 2 * (lh - 4), L"Esc  close this window", kMuted, st.small);
    }
};

HWND g_window = nullptr;

void SaveRecording(HWND hwnd, LiveFace& live) {
    FaceClip clip;
    if (!live.StopRecording(clip)) {
        MessageBoxW(hwnd, L"Nothing was recorded.", L"Face Mocap", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t buf[MAX_PATH] = L"webcam_capture.tbdface";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save the recorded face clip";
    ofn.lpstrDefExt = L"tbdface";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!clip.Save(buf)) MessageBoxW(hwnd, L"Could not write the file.", L"Face Mocap", MB_OK | MB_ICONWARNING);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LiveView* v = reinterpret_cast<LiveView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_TIMER:
        if (v) {
            if (!v->live->Running()) { // capture was stopped from the editor
                DestroyWindow(hwnd);
                return 0;
            }
            const uint64_t before = v->seq;
            v->Pull();
            if (v->seq != before) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (!v) break;
        if (wparam == 'R') {
            if (v->live->Recording()) SaveRecording(hwnd, *v->live);
            else v->live->StartRecording();
        } else if (wparam == 'C') v->live->Recalibrate();
        else if (wparam == VK_ESCAPE) DestroyWindow(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (v && rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            v->Render(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        if (g_window == hwnd) g_window = nullptr;
        break;
    case WM_NCDESTROY:
        if (v) {
            DestroyFonts(v->st);
            delete v;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void FaceLiveWindow_Open(HWND owner, HINSTANCE instance, LiveFace* live) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }
    auto* v = new LiveView;
    v->live = live;
    v->st.dpi = owner ? (int)GetDpiForWindow(owner) : 96;
    CreateFonts(v->st);
    g_window = CreateWindowExW(0, kClass, L"Face Mocap - Live Webcam", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                               CW_USEDEFAULT, v->st.S(960), v->st.S(620), owner, nullptr, instance, nullptr);
    if (!g_window) {
        DestroyFonts(v->st);
        delete v;
        return;
    }
    SetWindowLongPtrW(g_window, GWLP_USERDATA, (LONG_PTR)v);
    SetTimer(g_window, kTimer, 15, nullptr);
    SetFocus(g_window);
}

bool FaceLiveWindow_RenderToBmp(LiveFace& live, int width, int height, const wchar_t* bmpPath) {
    LiveView v;
    v.live = &live;
    CreateFonts(v.st);
    v.Pull();

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    v.Render(mem, width, height);
    GdiFlush();

    bool ok = false;
    if (FILE* f = _wfopen(bmpPath, L"wb")) {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + (DWORD)(width * height * 4);
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        ok = fwrite(bits, 4, (size_t)width * height, f) == (size_t)width * height;
        fclose(f);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DestroyFonts(v.st);
    return ok;
}

} // namespace tbd
