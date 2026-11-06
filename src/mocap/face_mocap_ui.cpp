#define UNICODE
#define _UNICODE
#include "face_mocap_ui.h"

#include <commctrl.h>
#include <commdlg.h>

#include <atomic>
#include <string>
#include <thread>

#include "face_preview.h"
#include "face_tracker.h"
#include "video_reader.h"

namespace tbd {
namespace {

const wchar_t* kClass = L"TBDFaceMocapProgress";
constexpr UINT WM_MOCAP_PROGRESS = WM_APP + 1; // wparam = percent
constexpr UINT WM_MOCAP_DONE = WM_APP + 2;     // lparam = JobResult* (owned by the receiver)

struct JobResult {
    bool ok = false;
    std::wstring message;
};

struct Job {
    std::wstring video, out, modelDir;
    std::atomic<bool> cancel{false};
    std::thread worker;
    HWND hwnd = nullptr, bar = nullptr, label = nullptr, cancelBtn = nullptr;
};

std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    return p.substr(0, p.find_last_of(L'\\'));
}

bool PickFile(HWND owner, bool save, const wchar_t* filter, const wchar_t* title, const wchar_t* defExt,
              std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    wcsncpy_s(buf, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    path = buf;
    return true;
}

void RunJob(Job* job) {
    auto* res = new JobResult;
    FaceTracker tracker;
    VideoReader reader;
    std::string err;
    if (!tracker.Init(job->modelDir.c_str(), &err)) {
        res->message = L"Could not load the face models from\n" + job->modelDir;
    } else if (!reader.Open(job->video.c_str())) {
        res->message = L"Could not open the video. Windows needs a codec for it (H.264 MP4 always works).";
    } else {
        FaceClip clip;
        clip.fps = (float)reader.Fps();
        clip.width = reader.Width();
        clip.height = reader.Height();
        std::vector<uint8_t> frame;
        double t = 0;
        int found = 0, last = -1;
        while (!job->cancel && reader.ReadFrame(frame, t)) {
            FaceFrame ff;
            ff.time = t;
            tracker.Process(frame.data(), reader.Width(), reader.Height(), ff);
            found += ff.valid;
            clip.frames.push_back(ff);
            if (reader.Duration() > 0) {
                int pct = (int)(100.0 * t / reader.Duration());
                if (pct != last) PostMessageW(job->hwnd, WM_MOCAP_PROGRESS, (WPARAM)pct, 0), last = pct;
            }
        }
        if (job->cancel) {
            res->message = L"Cancelled.";
        } else {
            FaceTracker::Smooth(clip.frames);
            FaceTracker::NormalizeChannels(clip.frames);
            if (!clip.Save(job->out.c_str())) {
                res->message = L"Could not write\n" + job->out;
            } else {
                res->ok = true;
                res->message = L"Tracked " + std::to_wstring(clip.frames.size()) + L" frames, face found in " +
                               std::to_wstring(found) + L".\n\nSaved " + job->out;
            }
        }
    }
    PostMessageW(job->hwnd, WM_MOCAP_DONE, 0, (LPARAM)res);
}

LRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Job* job = reinterpret_cast<Job*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (job && (HWND)lparam == job->cancelBtn) {
            job->cancel = true;
            EnableWindow(job->cancelBtn, FALSE);
            SetWindowTextW(job->label, L"Cancelling...");
        }
        return 0;
    case WM_MOCAP_PROGRESS:
        if (job) SendMessageW(job->bar, PBM_SETPOS, wparam, 0);
        return 0;
    case WM_MOCAP_DONE: {
        std::unique_ptr<JobResult> res(reinterpret_cast<JobResult*>(lparam));
        HWND owner = GetWindow(hwnd, GW_OWNER);
        HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        const std::wstring clip = job ? job->out : L"", video = job ? job->video : L"";
        if (job && job->worker.joinable()) job->worker.join();
        DestroyWindow(hwnd);
        MessageBoxW(owner, res->message.c_str(), L"Face Mocap", MB_OK | (res->ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        if (res->ok) FaceMocapPreview_Open(owner, instance, clip.c_str(), video.c_str());
        return 0;
    }
    case WM_CLOSE:
        if (job) job->cancel = true; // the worker posts WM_MOCAP_DONE, which closes the window
        return 0;
    case WM_NCDESTROY:
        delete job;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void FaceMocap_Open(HWND owner, HINSTANCE instance) {
    std::wstring video, out;
    if (!PickFile(owner, false, L"Video\0*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv;*.webm\0All files\0*.*\0",
                  L"Face Mocap: choose a video", nullptr, video))
        return;
    out = video.substr(0, video.find_last_of(L'.')) + L".tbdface";
    if (!PickFile(owner, true, L"Face clip (*.tbdface)\0*.tbdface\0", L"Face Mocap: save clip as", L"tbdface", out))
        return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ProgressProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&icc);
        registered = true;
    }

    auto* job = new Job;
    job->video = video;
    job->out = out;
    job->modelDir = ExeDir() + L"\\models\\face";

    const int dpi = (int)GetDpiForWindow(owner);
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    RECT r{0, 0, S(380), S(110)};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    job->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Face Mocap", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr,
                                instance, nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    job->label = CreateWindowW(L"STATIC", L"Tracking face...", WS_CHILD | WS_VISIBLE, S(14), S(12), S(350), S(18),
                               job->hwnd, nullptr, instance, nullptr);
    job->bar = CreateWindowW(PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, S(14), S(36), S(350), S(18), job->hwnd,
                             nullptr, instance, nullptr);
    job->cancelBtn = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, S(284), S(68), S(80),
                                   S(26), job->hwnd, nullptr, instance, nullptr);
    SendMessageW(job->label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(job->cancelBtn, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowLongPtrW(job->hwnd, GWLP_USERDATA, (LONG_PTR)job);
    job->worker = std::thread(RunJob, job);
}

} // namespace tbd
