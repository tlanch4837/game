// Temporary debug recording for the Face Mocap Live tab: while active, every presented frame pairs the newest camera
// picture with the engine's own render, so the footage shows what the user actually saw, delay included. Needs ffmpeg on
// PATH. Writes to a new folder, on this machine only:
//   clean.mp4    the camera, untouched (can be replayed through face_videolog)
//   compare.mp4  camera | camera + landmarks | engine viewport
//   frames.csv   per tracked frame: time, face found, score, pose, the 52 weights
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "face_live.h"

namespace tbd {

class FaceSessionLog {
public:
    ~FaceSessionLog() { Stop(); }
    bool Active() const { return live_ != nullptr; }

    // `root` gets a new time-stamped folder. Encoders open on the first frame, when the camera size is known.
    bool Start(LiveFace* live, const std::filesystem::path& root) {
        Stop();
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[32];
        swprintf(name, 32, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        dir_ = root / name;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) return false;
        csv_ = _wfopen((dir_ / L"frames.csv").c_str(), L"w");
        if (!csv_) return false;
        fprintf(csv_, "seq,time,valid,score,pitch,yaw,roll");
        for (int c = 1; c < kBlendshapes; ++c) fprintf(csv_, ",%s", kBlendshapeNames[c]);
        fputc('\n', csv_);
        live_ = live;
        t0_ = Clock::now();
        written_ = 0;
        lastSeq_ = 0;
        return true;
    }

    // Stops and returns the folder ("" when not recording).
    std::wstring Stop() {
        if (!live_) return L"";
        live_ = nullptr;
        clean_.Close();
        compare_.Close();
        if (csv_) fclose(csv_);
        csv_ = nullptr;
        return dir_.wstring();
    }

    // `bgra` is the presented viewport, w*h*4, top-down. Called once per rendered frame.
    void OnFrame(const std::vector<uint8_t>& bgra, int w, int h) {
        if (!live_ || w <= 0 || h <= 0) return;
        uint64_t seq = 0;
        int cw = 0, ch = 0;
        if (!live_->Latest(&face_, &cam_, &cw, &ch, &seq)) return;
        if (seq != lastSeq_) {
            lastSeq_ = seq;
            fprintf(csv_, "%llu,%.4f,%d,%.3f,%.2f,%.2f,%.2f", (unsigned long long)seq, face_.time, face_.valid ? 1 : 0, face_.score,
                    face_.rot[0], face_.rot[1], face_.rot[2]);
            for (int c = 1; c < kBlendshapes; ++c) fprintf(csv_, ",%.3f", face_.bs[c]);
            fputc('\n', csv_);
        }
        if (cw < 2 || ch < 2) return; // an iPhone sends no picture: only the numbers are kept
        // the output clock is fixed, so frames repeat or skip to hold real time
        const double now = std::chrono::duration<double>(Clock::now() - t0_).count();
        if (written_ / kFps > now) return;

        if (!clean_.Open()) {
            cleanW_ = cw & ~1;
            cleanH_ = ch & ~1;
            panelW_ = (cw * kH / ch + 1) & ~1;
            canvasW_ = panelW_ * 2 + kViewW;
            canvas_.assign((size_t)canvasW_ * kH * 4, 255);
            cleanOut_.resize((size_t)cleanW_ * cleanH_ * 4);
            if (!clean_.Start(cleanW_, cleanH_, dir_ / L"clean.mp4") || !compare_.Start(canvasW_, kH, dir_ / L"compare.mp4")) {
                failed_ = true;
                Stop();
                return;
            }
        }
        for (int y = 0; y < cleanH_; ++y)
            memcpy(&cleanOut_[(size_t)y * cleanW_ * 4], &cam_[(size_t)y * cw * 4], (size_t)cleanW_ * 4);

        for (int y = 0; y < kH; ++y) {
            const uint8_t* src = &cam_[(size_t)(y * ch / kH) * cw * 4];
            uint8_t* a = &canvas_[(size_t)y * canvasW_ * 4];
            uint8_t* b = a + (size_t)panelW_ * 4;
            for (int x = 0; x < panelW_; ++x) memcpy(a + (size_t)x * 4, src + (size_t)(x * cw / panelW_) * 4, 4);
            memcpy(b, a, (size_t)panelW_ * 4);
        }
        if (face_.valid)
            for (int i = 0; i < kFaceLandmarks; ++i) {
                const int cx = panelW_ + (int)(face_.lm[i * 3] * panelW_), cy = (int)(face_.lm[i * 3 + 1] * kH);
                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = 0; dx <= 1; ++dx) {
                        const int x = cx + dx, y = cy + dy;
                        if (x < panelW_ || x >= panelW_ * 2 || y < 0 || y >= kH) continue;
                        uint8_t* o = &canvas_[((size_t)y * canvasW_ + x) * 4];
                        o[0] = 90; o[1] = 255; o[2] = 60;
                    }
            }
        // engine panel: fit the viewport
        const float s = std::min((float)kViewW / w, (float)kH / h);
        const int vw = std::max(1, (int)(w * s)), vh = std::max(1, (int)(h * s));
        const int ox = panelW_ * 2 + (kViewW - vw) / 2, oy = (kH - vh) / 2;
        for (int y = 0; y < kH; ++y)
            memset(&canvas_[((size_t)y * canvasW_ + panelW_ * 2) * 4], 0, (size_t)kViewW * 4);
        for (int y = 0; y < vh; ++y) {
            const uint8_t* src = &bgra[(size_t)(y * h / vh) * w * 4];
            uint8_t* o = &canvas_[((size_t)(y + oy) * canvasW_ + ox) * 4];
            for (int x = 0; x < vw; ++x, o += 4) {
                const uint8_t* p = src + (size_t)(x * w / vw) * 4;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255;
            }
        }
        do {
            clean_.Write(cleanOut_);
            compare_.Write(canvas_);
            ++written_;
        } while (written_ / kFps < now);
    }
    bool Failed() const { return failed_; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr double kFps = 30;
    static constexpr int kH = 540, kViewW = 720;

    // ffmpeg reading raw frames on stdin, with no console window: the editor is a GUI program.
    struct Encoder {
        HANDLE process = nullptr, input = nullptr;
        bool Start(int w, int h, const std::filesystem::path& path) {
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            HANDLE read = nullptr;
            if (!CreatePipe(&read, &input, &sa, 1 << 24)) return false;
            SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = read;
            si.hStdOutput = si.hStdError = INVALID_HANDLE_VALUE;
            std::wstring cmd = L"ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s " + std::to_wstring(w) + L"x" + std::to_wstring(h) +
                               L" -r 30 -i - -c:v libx264 -preset ultrafast -crf 20 -pix_fmt yuv420p \"" + path.wstring() + L"\"";
            PROCESS_INFORMATION pi{};
            const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(read);
            if (!ok) {
                CloseHandle(input);
                input = nullptr;
                return false;
            }
            CloseHandle(pi.hThread);
            process = pi.hProcess;
            return true;
        }
        bool Open() const { return process != nullptr; }
        void Write(const std::vector<uint8_t>& frame) {
            DWORD done = 0;
            if (input) WriteFile(input, frame.data(), (DWORD)frame.size(), &done, nullptr);
        }
        void Close() {
            if (input) CloseHandle(input); // end of stream: ffmpeg finishes the file
            input = nullptr;
            if (process) {
                WaitForSingleObject(process, 30000);
                CloseHandle(process);
            }
            process = nullptr;
        }
    };

    LiveFace* live_ = nullptr;
    std::filesystem::path dir_;
    FILE* csv_ = nullptr;
    Encoder clean_, compare_;
    Clock::time_point t0_;
    long long written_ = 0;
    uint64_t lastSeq_ = 0;
    bool failed_ = false;
    int cleanW_ = 0, cleanH_ = 0, panelW_ = 0, canvasW_ = 0;
    FaceFrame face_;
    std::vector<uint8_t> cam_, canvas_, cleanOut_;
};

} // namespace tbd
