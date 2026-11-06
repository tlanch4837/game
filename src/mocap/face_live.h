// Live face capture: a worker thread reads a webcam (or, for testing, a video file played in real time and
// looped), tracks the face, smooths it, and publishes the newest frame. Can record what it sees as a FaceClip.
// Or it listens for an iPhone running Live Link Face (face_phone.h), which does the tracking itself and sends no picture.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "face_calib.h"
#include "face_clip.h"

namespace tbd {

class LiveFace {
public:
    ~LiveFace();

    // `cameraIndex` < 0 picks the first camera that is not an infrared one. `videoFile` (when not null) is
    // used instead of a camera. `modelDir` holds the face models. Returns false and sets `error` on failure.
    bool Start(int cameraIndex, const wchar_t* videoFile, const wchar_t* modelDir, std::string* error);
    // Listens for an iPhone on UDP `port`. On failure (the port is taken) returns false and leaves any running capture alone.
    bool StartPhone(int port, std::string* error);
    bool IsPhone() const { return phone_; }
    std::wstring PhoneStatus() const; // what to do next, or what is wrong: for the user to read
    void Stop();
    bool Running() const { return running_; }
    const std::wstring& SourceName() const { return sourceName_; }

    // Newest tracked frame, and (when `image` is not null) the camera picture it came from. `seq` counts
    // frames, so a caller can tell whether anything new arrived. False until the first frame.
    bool Latest(FaceFrame* out, std::vector<uint8_t>* image, int* width, int* height, uint64_t* seq);

    // Cheap: just the head angles (pitch, yaw, roll) and the neutral pose they are measured against.
    // Returns false when no face is currently tracked or the neutral pose is not measured yet.
    bool Pose(float rot[3], float neutral[3]);

    // Measures the neutral head pose again from the next ~1.5 s (look at the screen with a relaxed face).
    void Recalibrate();
    bool Calibrated() const { return calibrated_; }
    float CalibProgress() const { return calibProgress_; }   // 0..1 while calibrating
    const char* CalibHint() const { return calibHint_; }     // null, "Hold your head still" or "Relax your face"

    float TrackingFps() const { return fps_; }

    // Recording keeps every tracked frame from now until StopRecording, which fills `clip` (channels normalised).
    void StartRecording();
    bool Recording() const { return recording_; }
    bool StopRecording(FaceClip& clip);

private:
    void Run(std::wstring modelDir, std::wstring videoFile, int cameraIndex);
    void RunPhone(uintptr_t socket);
    void Publish(FaceFrame& ff, double time, const std::vector<uint8_t>* image, int width, int height); // calibrates, records, hands out

    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false}, calibrated_{false}, recording_{false};
    std::atomic<float> fps_{0};
    std::atomic<float> calibProgress_{0};
    std::atomic<const char*> calibHint_{nullptr}; // always a string literal (or null)
    std::atomic<bool> phone_{false}, phoneHeard_{false};
    std::atomic<int> phoneBadVersion_{0};
    int phonePort_ = 0;
    std::wstring phoneAddress_;
    std::wstring sourceName_;

    std::mutex mutex_; // guards everything below
    FaceFrame latest_;
    std::vector<uint8_t> image_;
    int width_ = 0, height_ = 0;
    uint64_t seq_ = 0;
    float neutral_[3] = {};
    FaceCalibrator calibrator_;
    float bsBase_[kBlendshapes] = {}; // the subject's resting face
    bool wantCalibrate_ = true;
    std::vector<FaceFrame> recorded_;
    double recordStart_ = 0;

    std::string startError_;
    std::atomic<int> startState_{0}; // 0 starting, 1 ok, -1 failed
};

} // namespace tbd
