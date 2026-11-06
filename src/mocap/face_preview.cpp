#define UNICODE
#define _UNICODE
#include "face_preview.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "face_clip.h"
#include "face_draw.h"
#include "video_reader.h"

namespace tbd {
namespace {

const wchar_t* kClass = L"TBDFacePreview";
constexpr UINT_PTR kTimer = 1;

const COLORREF kBg = RGB(32, 28, 24);
const COLORREF kPanel = RGB(24, 20, 16);
const COLORREF kText = RGB(228, 218, 202);
const COLORREF kMuted = RGB(150, 140, 125);
const COLORREF kAccent = RGB(176, 112, 48);
const COLORREF kGrid = RGB(60, 52, 44);
const COLORREF kMesh = RGB(90, 230, 120);

std::wstring ReplaceExt(const std::wstring& path, const wchar_t* ext) {
    size_t dot = path.find_last_of(L'.');
    return (dot == std::wstring::npos ? path : path.substr(0, dot)) + ext;
}

bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Same name next to the clip with a common video extension, else asks the user.
bool FindVideo(HWND owner, const std::wstring& clipPath, std::wstring& video) {
    for (const wchar_t* ext : {L".mp4", L".mov", L".m4v", L".wmv", L".avi", L".mkv", L".webm"})
        if (FileExists(ReplaceExt(clipPath, ext))) {
            video = ReplaceExt(clipPath, ext);
            return true;
        }
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Video\0*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv;*.webm\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Face Preview: choose the source video of this clip";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    video = buf;
    return true;
}

struct Preview {
    FaceClip clip;
    VideoReader reader;
    std::vector<uint8_t> frame; // current video frame, BGRA top-down
    int idx = 0;                // clip frame shown
    bool playing = false;
    bool scrubbing = false;
    double shownTime = 0;       // timestamp of the frame in `frame`
    std::chrono::steady_clock::time_point playWall;
    double playT0 = 0;
    HFONT font = nullptr, small = nullptr;
    int dpi = 96;
    RECT timeline{}; // curve area of the last paint, for hit testing

    double Duration() const { return clip.frames.empty() ? 0 : clip.frames.back().time; }

    int IndexAt(double t) const {
        auto it = std::lower_bound(clip.frames.begin(), clip.frames.end(), t,
                                   [](const FaceFrame& f, double v) { return f.time < v; });
        int i = (int)(it - clip.frames.begin());
        if (i > 0 && (i == (int)clip.frames.size() || t - clip.frames[i - 1].time < clip.frames[i].time - t)) --i;
        return std::clamp(i, 0, std::max((int)clip.frames.size() - 1, 0));
    }

    bool Next() { // decode the next video frame, loop at the end
        double t = 0;
        if (!reader.ReadFrame(frame, t)) {
            if (!reader.Seek(0) || !reader.ReadFrame(frame, t)) return false;
            playT0 = 0;
            playWall = std::chrono::steady_clock::now();
        }
        shownTime = t;
        idx = IndexAt(t);
        return true;
    }

    void SeekTo(double t) {
        t = std::clamp(t, 0.0, Duration());
        if (reader.Seek(t)) {
            // The decoder lands on the keyframe before `t`: decode forward to the wanted frame.
            const double half = 0.5 / std::max(clip.fps, 1.0f);
            double ts = 0;
            for (int n = 0; n < 900; ++n) {
                if (!reader.ReadFrame(frame, ts)) break;
                shownTime = ts;
                if (ts >= t - half) break;
            }
            idx = IndexAt(shownTime);
        }
        playT0 = shownTime;
        playWall = std::chrono::steady_clock::now();
    }

    void Step(int dir) {
        playing = false;
        int target = std::clamp(idx + dir, 0, (int)clip.frames.size() - 1);
        SeekTo(clip.frames[target].time);
    }

    void Tick() {
        if (!playing) return;
        double target = playT0 + std::chrono::duration<double>(std::chrono::steady_clock::now() - playWall).count();
        const double dur = 1.0 / std::max(clip.fps, 1.0f);
        if (target - shownTime > 0.5) { // fell behind (slow decode): resync instead of racing
            playT0 = shownTime;
            playWall = std::chrono::steady_clock::now();
            target = shownTime;
        }
        for (int n = 0; n < 4 && shownTime + dur <= target; ++n)
            if (!Next()) break;
    }

    bool Load(HWND owner, const std::wstring& clipPath, const wchar_t* videoPath) {
        if (!clip.Load(clipPath.c_str()) || clip.frames.empty()) {
            MessageBoxW(owner, L"Could not read the face clip.", L"Face Preview", MB_OK | MB_ICONWARNING);
            return false;
        }
        std::wstring video = videoPath ? videoPath : L"";
        if (video.empty() && !FindVideo(owner, clipPath, video)) return false;
        if (!reader.Open(video.c_str())) {
            MessageBoxW(owner, L"Could not open the source video.", L"Face Preview", MB_OK | MB_ICONWARNING);
            return false;
        }
        SeekTo(0);
        return true;
    }

    void Render(HDC dc, int W, int H);
};

int Sc(const Preview& p, int v) { return MulDiv(v, p.dpi, 96); }

void Text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, HFONT f, UINT align = TA_LEFT) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
}

void FillRectColor(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void Preview::Render(HDC dc, int W, int H) {
    const int m = Sc(*this, 10), side = Sc(*this, 270), rowH = Sc(*this, 19), labelW = Sc(*this, 112);
    const int tlTop = H - m - rowH * kFaceChannels - Sc(*this, 4);
    FillRectColor(dc, 0, 0, W, H, kBg);
    SetBkMode(dc, TRANSPARENT);

    // --- video + mesh
    facedraw::Style st;
    st.font = font;
    st.small = small;
    st.dpi = dpi;
    const FaceFrame& f = clip.frames[idx];
    const bool haveFrame = frame.size() >= (size_t)reader.Width() * reader.Height() * 4;
    facedraw::DrawVideo(dc, RECT{m, m, W - side - 2 * m, tlTop - m}, haveFrame ? frame.data() : nullptr, reader.Width(),
                        reader.Height(), &f, false, st);

    // --- side panel: status, pose, channel bars
    const int x0 = W - side - m + Sc(*this, 6), lh = Sc(*this, 18);
    int y = m;
    wchar_t buf[96];
    swprintf_s(buf, L"Frame %d / %d    %.2f s", idx + 1, (int)clip.frames.size(), shownTime);
    Text(dc, x0, y, buf, kText, font);
    y += lh;
    swprintf_s(buf, L"%s   confidence %.0f%%", playing ? L"Playing" : L"Paused", f.valid ? f.score * 100.0f : 0.0f);
    Text(dc, x0, y, buf, kMuted, small);
    y += lh + Sc(*this, 8);
    y = facedraw::DrawPoseAndChannels(dc, x0, y, side, f, st);
    y += Sc(*this, 10);
    Text(dc, x0, y, L"Space  play / pause", kMuted, small);
    Text(dc, x0, y + lh - 4, L"\x2190 \x2192  step frame", kMuted, small);
    Text(dc, x0, y + 2 * (lh - 4), L"Home  rewind    drag timeline", kMuted, small);

    // --- timeline: one curve per channel
    const int cx0 = m + labelW, cx1 = W - m;
    timeline = {cx0, tlTop, cx1, tlTop + rowH * kFaceChannels};
    const int n = (int)clip.frames.size();
    for (int c = 0; c < kFaceChannels; ++c) {
        const int top = tlTop + c * rowH;
        FillRectColor(dc, cx0, top, cx1, top + rowH - 1, kPanel);
        wchar_t name[40];
        MultiByteToWideChar(CP_UTF8, 0, kFaceChannelNames[c], -1, name, 40);
        Text(dc, m, top + Sc(*this, 2), name, kMuted, small);

        std::vector<POINT> pts;
        const int cw = cx1 - cx0;
        for (int px = 0; px < cw; ++px) {
            int i = n > 1 ? (int)((int64_t)px * (n - 1) / std::max(cw - 1, 1)) : 0;
            const FaceFrame& fr = clip.frames[i];
            float w = fr.valid ? fr.weight[c] : 0.0f;
            pts.push_back({cx0 + px, top + rowH - 3 - (int)(w * (rowH - 6))});
        }
        HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
        HGDIOBJ old = SelectObject(dc, pen);
        if (pts.size() > 1) Polyline(dc, pts.data(), (int)pts.size());
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    // frames where no face was found
    for (int px = 0; px < cx1 - cx0; ++px) {
        int i = n > 1 ? (int)((int64_t)px * (n - 1) / std::max(cx1 - cx0 - 1, 1)) : 0;
        if (!clip.frames[i].valid) FillRectColor(dc, cx0 + px, tlTop + rowH * kFaceChannels, cx0 + px + 1,
                                                 tlTop + rowH * kFaceChannels + Sc(*this, 3), RGB(200, 90, 70));
    }
    const int px = cx0 + (n > 1 ? (int)((int64_t)idx * (cx1 - cx0 - 1) / (n - 1)) : 0);
    FillRectColor(dc, px, tlTop, px + Sc(*this, 2), tlTop + rowH * kFaceChannels, kText);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Preview* p = reinterpret_cast<Preview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto scrubTo = [&](int x) {
        if (!p || p->timeline.right <= p->timeline.left) return;
        double u = std::clamp((double)(x - p->timeline.left) / (p->timeline.right - p->timeline.left), 0.0, 1.0);
        p->SeekTo(u * p->Duration());
        InvalidateRect(hwnd, nullptr, FALSE);
    };
    switch (msg) {
    case WM_TIMER:
        if (p && p->playing) {
            p->Tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (!p) break;
        if (wparam == VK_SPACE) {
            p->playing = !p->playing;
            p->playT0 = p->shownTime;
            p->playWall = std::chrono::steady_clock::now();
        } else if (wparam == VK_RIGHT) p->Step(+1);
        else if (wparam == VK_LEFT) p->Step(-1);
        else if (wparam == VK_HOME) p->SeekTo(0);
        else if (wparam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (p && GET_Y_LPARAM(lparam) >= p->timeline.top - 2) {
            p->scrubbing = true;
            SetCapture(hwnd);
            scrubTo(GET_X_LPARAM(lparam));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (p && p->scrubbing) scrubTo(GET_X_LPARAM(lparam));
        return 0;
    case WM_LBUTTONUP:
        if (p) p->scrubbing = false;
        ReleaseCapture();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (p && rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            p->Render(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: KillTimer(hwnd, kTimer); break;
    case WM_NCDESTROY:
        if (p) {
            DeleteObject(p->font);
            DeleteObject(p->small);
            delete p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void MakeFonts(Preview& p) {
    p.font = CreateFontW(-Sc(p, 14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    p.small = CreateFontW(-Sc(p, 12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

} // namespace

void FaceMocapPreview_Open(HWND owner, HINSTANCE instance, const wchar_t* clipPath, const wchar_t* videoPath) {
    auto p = std::make_unique<Preview>();
    p->dpi = owner ? (int)GetDpiForWindow(owner) : 96;
    if (!p->Load(owner, clipPath, videoPath)) return;
    MakeFonts(*p);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PreviewProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }
    const std::wstring title = L"Face Preview - " + std::wstring(clipPath);
    HWND hwnd = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, Sc(*p, 1000), Sc(*p, 760), owner, nullptr, instance, nullptr);
    if (!hwnd) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p.release());
    SetTimer(hwnd, kTimer, 8, nullptr);
    SetFocus(hwnd);
}

void FaceMocapPreview_Pick(HWND owner, HINSTANCE instance) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Face clip (*.tbdface)\0*.tbdface\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Face Preview: choose a clip";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) FaceMocapPreview_Open(owner, instance, buf, nullptr);
}

bool FaceMocapPreview_RenderToBmp(const wchar_t* clipPath, const wchar_t* videoPath, int frameIndex, int width,
                                  int height, const wchar_t* bmpPath) {
    Preview p;
    if (!p.clip.Load(clipPath) || p.clip.frames.empty() || !p.reader.Open(videoPath)) return false;
    MakeFonts(p);
    p.SeekTo(p.clip.frames[std::clamp(frameIndex, 0, (int)p.clip.frames.size() - 1)].time);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    p.Render(mem, width, height);
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
    DeleteObject(p.font);
    DeleteObject(p.small);
    return ok;
}

} // namespace tbd
