// Lip sync from a recording's audio: microphone capture, GPU transcription (gameenginetools\lipsync\transcribe.py) and
// uLipSync's MFCC phoneme scoring, ported to C++. The transcript's word timings steer the vowels and add the lip
// closures (b, m, p, f, v) that audio scoring cannot hear. The result is a ".tbdlips" file beside the ".tbdface" clip.
#pragma once
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "face_clip.h"

namespace tbd {

enum LipViseme { LV_A, LV_I, LV_U, LV_E, LV_O, LV_S, LV_MBP, LV_FV, kLipVisemes };
constexpr int kLipSampleRate = 16000;

struct LipWord { float start = 0, end = 0; std::string text; };

struct LipTrack {
    float rate = 60; // frames per second
    std::vector<std::array<float, kLipVisemes>> frames;

    bool Save(const wchar_t* path) const;
    bool Load(const wchar_t* path);
    bool Loaded() const { return !frames.empty(); }
    void Clear() { frames.clear(); }
    void Sample(double seconds, float out[kLipVisemes]) const;
};

// Records the default microphone, 16 kHz mono.
class MicRecorder {
public:
    ~MicRecorder();
    bool Start();
    bool Running() const { return thread_.joinable(); }
    std::vector<int16_t> Stop();

private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
    HANDLE wave_ = nullptr;
    std::vector<int16_t> samples_;
};

bool WriteWav16k(const wchar_t* path, const std::vector<int16_t>& pcm);
bool ReadWav16k(const wchar_t* path, std::vector<int16_t>& pcm); // 16 kHz mono 16 bit only

// Runs the transcription script and waits for it. False sets `error`.
bool TranscribeWav(const std::wstring& wavPath, const std::wstring& wordsPath, std::string* error);
std::vector<LipWord> ReadLipWords(const wchar_t* path);

// `words` may be empty: then only the audio decides, and no closures are made. `report` (optional) receives a summary.
LipTrack AnalyzeLips(const std::vector<int16_t>& pcm, const std::vector<LipWord>& words, std::string* report = nullptr);

// Blends a viseme frame into mouth channels. Where the audio is silent the camera's weights stay; expressions such as a
// smile are only ever added to. `strength`: 0 = camera only, 1 = audio decides the mouth while speaking.
void ApplyLips(const float visemes[kLipVisemes], float strength, float weights[kBlendshapes]);

} // namespace tbd
