#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "face_live.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

#include "face_phone.h"
#include "face_tracker.h"
#include "video_reader.h"

namespace tbd {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kRangeWindow = 450; // frames the channel ranges are measured over

float Percentile(std::deque<float> v, float p) {
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (v.size() - 1))];
}

// Pose and channels for a live stream: the channel weights have no whole clip to be scaled against, so each is
// scaled by the 5th..95th percentile of its recent history.
struct LiveNormalizer {
    std::deque<float> history[kFaceChannels];
    float lo[kFaceChannels] = {}, hi[kFaceChannels] = {};
    int sinceUpdate = 0;
    bool ready = false;

    void Apply(FaceFrame& f) {
        for (int c = 0; c < kFaceChannels; ++c) {
            history[c].push_back(f.raw[c]);
            if (history[c].size() > kRangeWindow) history[c].pop_front();
        }
        if (history[0].size() >= 30 && (!ready || ++sinceUpdate >= 8)) {
            sinceUpdate = 0;
            ready = true;
            for (int c = 0; c < kFaceChannels; ++c) {
                lo[c] = Percentile(history[c], 0.05f);
                hi[c] = Percentile(history[c], 0.95f);
            }
        }
        for (int c = 0; c < kFaceChannels; ++c)
            f.weight[c] = ready ? std::clamp((f.raw[c] - lo[c]) / std::max(hi[c] - lo[c], 1e-4f), 0.0f, 1.0f) : 0.0f;
    }
};

} // namespace

LiveFace::~LiveFace() { Stop(); }

bool LiveFace::Start(int cameraIndex, const wchar_t* videoFile, const wchar_t* modelDir, std::string* error) {
    Stop();
    stop_ = false;
    startState_ = 0;
    startError_.clear();
    calibrated_ = false;
    wantCalibrate_ = true;
    calibrator_.Reset();
    calibProgress_ = 0;
    calibHint_ = nullptr;
    seq_ = 0;
    phone_ = false;
    thread_ = std::thread(&LiveFace::Run, this, std::wstring(modelDir), std::wstring(videoFile ? videoFile : L""),
                          cameraIndex);
    while (startState_ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5)); // models + device open
    if (startState_ < 0) {
        thread_.join();
        if (error) *error = startError_;
        return false;
    }
    return true;
}

void LiveFace::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    recording_ = false;
}

void LiveFace::Run(std::wstring modelDir, std::wstring videoFile, int cameraIndex) {
    FaceTracker tracker;
    VideoReader reader;
    std::string err;
    auto fail = [&](const std::string& why) {
        startError_ = why;
        startState_ = -1;
    };
    if (!tracker.Init(modelDir.c_str(), &err)) return fail("CANNOT LOAD FACE MODELS");
    if (!videoFile.empty()) {
        if (!reader.Open(videoFile.c_str())) return fail("CANNOT OPEN VIDEO");
        sourceName_ = L"video file (real time)";
    } else {
        const auto cams = VideoReader::ListCameras();
        if (cameraIndex < 0)
            for (int i = 0; i < (int)cams.size() && cameraIndex < 0; ++i)
                if (cams[i].find(L"IR") == std::wstring::npos) cameraIndex = i;
        if (cameraIndex < 0 && !cams.empty()) cameraIndex = 0;
        if (cameraIndex < 0) return fail("NO CAMERA FOUND");
        if (!reader.OpenCamera(cameraIndex)) return fail("CANNOT OPEN CAMERA (IN USE?)");
        sourceName_ = cams[cameraIndex];
    }
    running_ = true;
    startState_ = 1;

    FaceSmoother smoother(FaceSmoother::DefaultScales());
    LiveNormalizer normalizer;
    std::vector<uint8_t> frame;
    const auto t0 = Clock::now();
    auto prev = t0;
    auto loopStart = t0;
    float fpsAvg = 0;
    bool prevValid = false;

    while (!stop_) {
        double srcTime = 0;
        if (!reader.ReadFrame(frame, srcTime)) {
            if (videoFile.empty()) break; // camera went away
            reader.Seek(0); // loop the test video
            loopStart = Clock::now();
            smoother.Reset();
            continue;
        }
        if (!videoFile.empty()) // play a test file at its own speed
            std::this_thread::sleep_until(loopStart + std::chrono::duration_cast<Clock::duration>(
                                                          std::chrono::duration<double>(srcTime)));
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        const double time = std::chrono::duration<double>(now - t0).count();

        FaceFrame ff;
        ff.time = time;
        tracker.Process(frame.data(), reader.Width(), reader.Height(), ff);
        if (!ff.valid || !prevValid) smoother.Reset();
        prevValid = ff.valid;
        if (ff.valid) {
            smoother.Apply(ff, dt);
            normalizer.Apply(ff);
        }
        fpsAvg = fpsAvg * 0.9f + (dt > 0 ? 1.0f / dt : 0.0f) * 0.1f;
        fps_ = fpsAvg;

        Publish(ff, time, &frame, reader.Width(), reader.Height());
    }
    running_ = false;
}

void LiveFace::Publish(FaceFrame& ff, double time, const std::vector<uint8_t>* image, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ff.valid && wantCalibrate_) {
        const bool finished = calibrator_.Add(ff.rot, ff.bs);
        calibProgress_ = calibrator_.Progress();
        calibHint_ = calibrator_.Hint();
        if (finished) { // resting face = per-channel median of the calmest window
            for (int k = 0; k < 3; ++k) neutral_[k] = calibrator_.NeutralRot()[k];
            for (int k = 0; k < kBlendshapes; ++k) bsBase_[k] = calibrator_.BaseBlendshapes()[k];
            wantCalibrate_ = false;
            calibrated_ = true;
        }
    }
    if (recording_) {
        FaceFrame r = ff;
        r.time = time - recordStart_;
        recorded_.push_back(r);
    }
    if (ff.valid && calibrated_) // weights relative to the resting face
        for (int k = 0; k < kBlendshapes; ++k)
            ff.bs[k] = std::clamp((ff.bs[k] - bsBase_[k]) / std::max(1.0f - bsBase_[k], 0.5f), 0.0f, 1.0f);
    latest_ = ff;
    if (image) image_ = *image;
    else image_.clear();
    width_ = width;
    height_ = height;
    ++seq_;
}

bool LiveFace::StartPhone(int port, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("Networking is not available on this PC.");
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return fail("Could not open a network socket.");
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons((u_short)port);
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    if (bind(s, (const sockaddr*)&at, sizeof(at)) != 0) {
        closesocket(s);
        return fail("That network port is in use: is Unreal or another Live Link app open?");
    }
    // this PC's addresses, private networks first: what goes in the phone's target box
    std::wstring addresses;
    char host[256] = {};
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    if (gethostname(host, sizeof(host)) == 0 && getaddrinfo(host, nullptr, &hints, &list) == 0) {
        for (const addrinfo* i = list; i; i = i->ai_next) {
            wchar_t text[64];
            InetNtopW(AF_INET, &((const sockaddr_in*)i->ai_addr)->sin_addr, text, 64);
            const bool likely = wcsncmp(text, L"192.168.", 8) == 0 || wcsncmp(text, L"10.", 3) == 0;
            if (likely) addresses = addresses.empty() ? std::wstring(text) : std::wstring(text) + L" or " + addresses;
            else if (addresses.empty()) addresses = text;
        }
        freeaddrinfo(list);
    }
    Stop();
    stop_ = false;
    startError_.clear();
    calibrated_ = false;
    wantCalibrate_ = true;
    seq_ = 0;
    fps_ = 0;
    phone_ = true;
    phoneHeard_ = false;
    phoneBadVersion_ = 0;
    phonePort_ = port;
    phoneAddress_ = addresses;
    sourceName_ = L"iPhone (Live Link Face)";
    running_ = true;
    thread_ = std::thread(&LiveFace::RunPhone, this, (uintptr_t)s);
    return true;
}

std::wstring LiveFace::PhoneStatus() const {
    const int bad = phoneBadVersion_;
    if (bad) return L"The phone sends packet version " + std::to_wstring(bad) + L"; this build reads version " + std::to_wstring(kLiveLinkVersion) + L".";
    if (phoneHeard_) return L"Receiving from the iPhone.";
    return L"Waiting. In Live Link Face pick Live Link (ARKit) and target " + (phoneAddress_.empty() ? std::wstring(L"this PC") : phoneAddress_) + L", port " + std::to_wstring(phonePort_) + L". Same Wi-Fi.";
}

void LiveFace::RunPhone(uintptr_t socketHandle) {
    const SOCKET s = (SOCKET)socketHandle;
    const auto t0 = Clock::now();
    auto prev = t0, lastGood = t0;
    float fpsAvg = 0;
    bool anyPublished = false;
    std::vector<uint8_t> buffer(4096);
    // TBD_PHONE_LOG=<file.csv>: every packet's raw weights and head values, before any calibration. Numbers only, no picture.
    FILE* phoneLog = nullptr;
    wchar_t logPath[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"TBD_PHONE_LOG", logPath, MAX_PATH) > 0 && (phoneLog = _wfopen(logPath, L"w")) != nullptr) {
        fprintf(phoneLog, "time,pitch,yaw,roll");
        for (int c = 1; c < kBlendshapes; ++c) fprintf(phoneLog, ",%s", kBlendshapeNames[c]);
        fputc('\n', phoneLog);
    }
    while (!stop_) {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(s, &read);
        timeval wait{0, 100000};
        const int ready = select(0, &read, nullptr, nullptr, &wait);
        const auto now = Clock::now();
        const double time = std::chrono::duration<double>(now - t0).count();
        if (ready <= 0) {
            if (anyPublished && now - lastGood > std::chrono::seconds(1)) { // the phone went quiet: say so rather than freeze the face
                FaceFrame none;
                none.time = time;
                Publish(none, time, nullptr, 0, 0);
                anyPublished = false;
                phoneHeard_ = false;
                fps_ = 0;
            }
            continue;
        }
        const int got = recv(s, (char*)buffer.data(), (int)buffer.size(), 0);
        if (got <= 0) continue; // a port-unreachable report or an oversized packet; the stream carries on
        FaceFrame ff;
        int version = 0;
        const LiveLinkResult result = ParseLiveLinkPacket(buffer.data(), (size_t)got, ff, &version);
        if (result == LiveLinkResult::BadVersion) phoneBadVersion_ = version;
        if (result != LiveLinkResult::Ok) continue;
        phoneBadVersion_ = 0;
        phoneHeard_ = true;
        ff.valid = true;
        ff.score = 1;
        ff.time = time;
        if (phoneLog) {
            fprintf(phoneLog, "%.4f,%.4f,%.4f,%.4f", time, ff.rot[0], ff.rot[1], ff.rot[2]);
            for (int c = 1; c < kBlendshapes; ++c) fprintf(phoneLog, ",%.3f", ff.bs[c]);
            fputc('\n', phoneLog);
            fflush(phoneLog);
        }
        const float dt = std::chrono::duration<float>(now - prev).count();
        prev = lastGood = now;
        fpsAvg = fpsAvg * 0.9f + (dt > 0 ? 1.0f / dt : 0.0f) * 0.1f;
        fps_ = fpsAvg;
        Publish(ff, time, nullptr, 0, 0);
        anyPublished = true;
    }
    if (phoneLog) fclose(phoneLog);
    closesocket(s);
    running_ = false;
}

bool LiveFace::Latest(FaceFrame* out, std::vector<uint8_t>* image, int* width, int* height, uint64_t* seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (seq_ == 0) return false;
    if (out) *out = latest_;
    if (image) *image = image_;
    if (width) *width = width_;
    if (height) *height = height_;
    if (seq) *seq = seq_;
    return true;
}

bool LiveFace::Pose(float rot[3], float neutral[3]) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (seq_ == 0 || !latest_.valid || !calibrated_) return false;
    for (int k = 0; k < 3; ++k) {
        rot[k] = latest_.rot[k];
        neutral[k] = neutral_[k];
    }
    return true;
}

void LiveFace::Recalibrate() {
    std::lock_guard<std::mutex> lock(mutex_);
    calibrator_.Reset();
    calibProgress_ = 0;
    calibHint_ = nullptr;
    wantCalibrate_ = true;
    calibrated_ = false;
}

void LiveFace::StartRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    recorded_.clear();
    recordStart_ = latest_.time;
    recording_ = true;
}

bool LiveFace::StopRecording(FaceClip& clip) {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    if (recorded_.size() < 2) {
        recorded_.clear();
        return false;
    }
    clip = FaceClip{};
    clip.frames = std::move(recorded_);
    recorded_.clear();
    clip.width = (uint32_t)width_;
    clip.height = (uint32_t)height_;
    const double span = clip.frames.back().time - clip.frames.front().time;
    clip.fps = span > 0 ? (float)((clip.frames.size() - 1) / span) : 30.0f;
    FaceTracker::NormalizeChannels(clip.frames);
    return true;
}

} // namespace tbd
