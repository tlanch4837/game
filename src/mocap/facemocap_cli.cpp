// Command-line face mocap: facemocap <video> <out.tbdface> [--models <dir>] [--debug <dir> [every-n-frames]]
// Used to test the tracker without the editor; the editor's Face Mocap tool uses the same classes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "face_lipsync.h"
#include "face_live.h"
#include "face_live_window.h"
#include "face_preview.h"
#include "face_tracker.h"
#include "video_reader.h"

using namespace tbd;

namespace {

void DrawDot(std::vector<uint8_t>& img, int w, int h, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    reinterpret_cast<uint32_t*>(img.data())[(size_t)y * w + x] = color;
}

bool WriteBmp(const std::wstring& path, const std::vector<uint8_t>& bgra, int w, int h) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)bgra.size();
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h; // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: facemocap <video> <out.tbdface> [--models <dir>] [--debug <dir> [every-n]]\n"
                         L"       facemocap --preview-bmp <video> <clip.tbdface> <out.bmp> [frame]\n");
        return 2;
    }
    if (std::wstring(argv[1]) == L"--mic-test" && argc >= 3) { // --mic-test <seconds> [out.wav]: is the microphone reachable?
        MicRecorder mic;
        if (!mic.Start()) { fwprintf(stderr, L"could not open a microphone\n"); return 1; }
        Sleep((DWORD)(_wtof(argv[2]) * 1000));
        const std::vector<int16_t> pcm = mic.Stop();
        int peak = 0;
        for (int16_t s : pcm) peak = std::max(peak, std::abs((int)s));
        printf("%zu samples, peak %d\n", pcm.size(), peak);
        if (argc >= 4) WriteWav16k(argv[3], pcm);
        return 0;
    }
    if (std::wstring(argv[1]) == L"--lips" && argc >= 4) { // --lips <in.wav> <out.tbdlips> [words.txt]: the lip sync without a recording
        std::vector<int16_t> pcm;
        if (!ReadWav16k(argv[2], pcm)) { fwprintf(stderr, L"need a 16 kHz mono 16-bit wav\n"); return 1; }
        std::string report;
        const LipTrack t = AnalyzeLips(pcm, argc >= 5 ? ReadLipWords(argv[4]) : std::vector<LipWord>{}, &report);
        printf("%s\n", report.c_str());
        return t.Save(argv[3]) ? 0 : 1;
    }
    if (std::wstring(argv[1]) == L"--live-test" && argc >= 4) { // --live-test <camera|video> <out.bmp> [seconds]
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\models\\face";
        const bool cam = std::wstring(argv[2]) == L"camera";
        const int secs = argc > 4 ? _wtoi(argv[4]) : 6;
        for (const auto& n : VideoReader::ListCameras()) printf("camera: %ls\n", n.c_str());
        LiveFace live;
        std::string err;
        if (!live.Start(-1, cam ? nullptr : argv[2], dir.c_str(), &err)) {
            fprintf(stderr, "live start failed: %s\n", err.c_str());
            return 1;
        }
        printf("source: %ls\n", live.SourceName().c_str());
        live.StartRecording();
        Sleep(secs * 1000);
        float rot[3], neutral[3];
        const bool pose = live.Pose(rot, neutral);
        printf("tracking %.1f fps, calibrated %d, pose %s", live.TrackingFps(), (int)live.Calibrated(), pose ? "yes" : "no");
        if (pose) printf(" pitch %+.1f yaw %+.1f roll %+.1f (neutral %.1f %.1f %.1f)", rot[0], rot[1], rot[2], neutral[0], neutral[1], neutral[2]);
        FaceClip clip;
        const bool rec = live.StopRecording(clip);
        printf("\nrecorded %zu frames%s\n", rec ? clip.frames.size() : 0, rec ? "" : " (none)");
        bool ok = FaceLiveWindow_RenderToBmp(live, 960, 620, argv[3]);
        live.Stop();
        return ok ? 0 : 1;
    }
    if (std::wstring(argv[1]) == L"--preview-bmp" && argc >= 5) { // --preview-bmp <video> <clip> <out.bmp> [frame]
        return FaceMocapPreview_RenderToBmp(argv[3], argv[2], argc > 5 ? _wtoi(argv[5]) : 0, 1000, 760, argv[4]) ? 0 : 1;
    }
    std::wstring video = argv[1], outPath = argv[2], models, debugDir, rawOut;
    int debugEvery = 30;
    bool noSmooth = false;
    FaceTracker::SmoothParams smooth;
    for (int i = 3; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--models" && i + 1 < argc) models = argv[++i];
        else if (a == L"--no-smooth") noSmooth = true;
        else if (a == L"--smooth" && i + 2 < argc) {
            smooth.minCutoff = (float)_wtof(argv[++i]);
            smooth.beta = (float)_wtof(argv[++i]);
        }
        else if (a == L"--save-unsmoothed" && i + 1 < argc) rawOut = argv[++i];
        else if (a == L"--debug" && i + 1 < argc) {
            debugDir = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != L'-') debugEvery = _wtoi(argv[++i]);
        }
    }
    if (models.empty()) { // default: <exe dir>\models\face
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe;
        models = p.substr(0, p.find_last_of(L'\\')) + L"\\models\\face";
    }
    if (!debugDir.empty()) CreateDirectoryW(debugDir.c_str(), nullptr);

    FaceTracker tracker;
    std::string err;
    if (!tracker.Init(models.c_str(), &err)) {
        fprintf(stderr, "model load failed: %s\n", err.c_str());
        return 1;
    }
    VideoReader reader;
    if (!reader.Open(video.c_str())) {
        fprintf(stderr, "cannot open video\n");
        return 1;
    }
    printf("%dx%d @ %.2f fps, %.1fs\n", reader.Width(), reader.Height(), reader.Fps(), reader.Duration());

    FaceClip clip;
    clip.fps = (float)reader.Fps();
    clip.width = reader.Width();
    clip.height = reader.Height();

    std::vector<uint8_t> frame;
    double t = 0;
    int n = 0, found = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (reader.ReadFrame(frame, t)) {
        FaceFrame ff;
        ff.time = t;
        tracker.Process(frame.data(), reader.Width(), reader.Height(), ff);
        found += ff.valid;
        if (!debugDir.empty() && n % debugEvery == 0 && ff.valid) {
            for (int i = 0; i < kFaceLandmarks; ++i)
                DrawDot(frame, reader.Width(), reader.Height(), (int)(ff.lm[i * 3] * reader.Width()),
                        (int)(ff.lm[i * 3 + 1] * reader.Height()), 0xFF00FF00u);
            WriteBmp(debugDir + L"\\frame_" + std::to_wstring(n) + L".bmp", frame, reader.Width(), reader.Height());
        }
        clip.frames.push_back(ff);
        if (++n % 60 == 0) printf("\r%d frames", n), fflush(stdout);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!noSmooth) {
        if (!rawOut.empty()) { // keep the unsmoothed clip for comparison
            FaceClip raw = clip;
            FaceTracker::NormalizeChannels(raw.frames);
            raw.Save(rawOut.c_str());
        }
        FaceTracker::Smooth(clip.frames, smooth);
    }
    FaceTracker::NormalizeChannels(clip.frames);
    printf("\r%d frames, face found in %d (%.0f%%), %.1f fps processing\n", n, found, n ? 100.0 * found / n : 0.0,
           secs > 0 ? n / secs : 0.0);

    if (!clip.Save(outPath.c_str())) {
        fprintf(stderr, "cannot write %ls\n", outPath.c_str());
        return 1;
    }
    printf("wrote %ls\n", outPath.c_str());
    return 0;
}
