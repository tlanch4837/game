#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "face_lipsync.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace tbd {
namespace {

#include "lip_profile.inl"

constexpr float kPi = 3.14159265f;
constexpr int kWindow = 1024, kMels = 30, kMfcc = 12;
constexpr int kProfileSil = 5, kProfileS = 6; // positions in kLipProfile: A I U E O - S
constexpr float kSharpen = 12.0f, kAudioVote = 0.2f;
constexpr float kOpenAmount = 0.6f; // everyday speech is small: the open shapes at full size look shouted

void Fft(std::vector<std::complex<float>>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2 * kPi / len;
        const std::complex<float> step(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int k = 0; k < len / 2; ++k, w *= step) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

float ToMel(float hz) { return 1127 * std::log(hz / 700 + 1); }
float ToHz(float mel) { return 700 * (std::exp(mel / 1127) - 1); }

// uLipSync's LipSyncJob, minus the resampling (we record at its target rate).
void Mfcc(const float* in, float out[kMfcc]) {
    float d[kWindow];
    d[0] = in[0];
    for (int i = 1; i < kWindow; ++i) d[i] = in[i] - 0.97f * in[i - 1];
    float peak = 0;
    for (int i = 0; i < kWindow; ++i) {
        d[i] *= 0.54f - 0.46f * std::cos(2 * kPi * i / (kWindow - 1));
        peak = std::max(peak, std::abs(d[i]));
    }
    if (peak > 1e-6f) for (float& v : d) v /= peak;
    std::vector<std::complex<float>> c(d, d + kWindow);
    Fft(c);
    const float fMax = kLipSampleRate / 2.0f, df = fMax / (kWindow / 2), dMel = ToMel(fMax) / (kMels + 1);
    float mel[kMels];
    for (int n = 0; n < kMels; ++n) {
        const float fB = ToHz(dMel * n), fC = ToHz(dMel * (n + 1)), fE = ToHz(dMel * (n + 2));
        const int iC = (int)std::round(fC / df);
        float sum = 0;
        for (int i = (int)std::ceil(fB / df) + 1; i <= (int)std::floor(fE / df); ++i) {
            const float f = df * i;
            const float a = (i < iC ? (f - fB) / (fC - fB) : (fE - f) / (fE - fC)) / ((fE - fB) * 0.5f);
            sum += a * std::abs(c[i]);
        }
        mel[n] = 10 * std::log10(std::max(sum, 1e-10f));
    }
    for (int i = 1; i <= kMfcc; ++i) {
        float sum = 0;
        for (int j = 0; j < kMels; ++j) sum += mel[j] * std::cos((j + 0.5f) * i * kPi / kMels);
        out[i - 1] = sum;
    }
}

float Cosine(const float* a, const float* b) {
    float p = 0, na = 0, nb = 0;
    for (int i = 0; i < kMfcc; ++i) { p += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
    return std::max(p / std::max(std::sqrt(na * nb), 1e-6f), 0.0f);
}

struct Phone { int viseme; float length; };

// Rough letter-to-sound rules: enough to say which mouth shapes a word goes through, and roughly when.
std::vector<Phone> WordPhones(const std::string& word) {
    std::string s;
    for (char ch : word)
        if (ch > 0 && std::isalpha((unsigned char)ch)) s += (char)std::tolower((unsigned char)ch);
    std::vector<Phone> out;
    static const struct { const char* letters; int viseme; float length; } kDigraphs[] = {
        {"oo", LV_U, 2}, {"ou", LV_O, 2}, {"ow", LV_O, 2}, {"oa", LV_O, 2}, {"oi", LV_O, 2}, {"oy", LV_O, 2},
        {"ee", LV_I, 2}, {"ea", LV_I, 2}, {"ie", LV_I, 2}, {"ai", LV_E, 2}, {"ay", LV_E, 2}, {"ey", LV_E, 2},
        {"au", LV_O, 2}, {"aw", LV_O, 2}, {"th", LV_S, 1}, {"sh", LV_U, 1}, {"ch", LV_U, 1}, {"ph", LV_FV, 1},
        {"wh", LV_U, 1}, {"qu", LV_U, 1}};
    for (size_t i = 0; i < s.size();) {
        bool hit = false;
        for (const auto& d : kDigraphs)
            if (s.compare(i, 2, d.letters) == 0) { out.push_back({d.viseme, d.length}); i += 2; hit = true; break; }
        if (hit) continue;
        const char c = s[i];
        const bool last = i + 1 == s.size();
        switch (c) {
        case 'a': out.push_back({LV_A, 2}); break;
        case 'e': if (!(last && s.size() > 2)) out.push_back({LV_E, 2}); break; // a final e is usually silent
        case 'i': out.push_back({LV_I, 2}); break;
        case 'o': out.push_back({LV_O, 2}); break;
        case 'u': case 'w': out.push_back({LV_U, c == 'u' ? 2.0f : 1.0f}); break;
        case 'y': out.push_back({last ? LV_I : LV_E, last ? 2.0f : 1.0f}); break;
        case 'b': case 'm': case 'p': out.push_back({LV_MBP, 1}); break;
        case 'f': case 'v': out.push_back({LV_FV, 1}); break;
        case 'h': case 'l': case 'r': out.push_back({-1, 1}); break; // no lip shape of its own: the neighbours decide
        default: out.push_back({LV_S, 1}); break;
        }
        ++i;
    }
    return out;
}

void Smooth(std::vector<float>& v, float sigmaFrames) {
    const int r = (int)std::ceil(sigmaFrames * 2.5f);
    std::vector<float> k(2 * r + 1), o(v.size());
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5f * i * i / (sigmaFrames * sigmaFrames));
    for (int i = 0; i < (int)v.size(); ++i) {
        float a = 0;
        for (int j = -r; j <= r; ++j) a += k[j + r] * v[std::clamp(i + j, 0, (int)v.size() - 1)];
        o[i] = a / sum;
    }
    v.swap(o);
}

} // namespace

bool LipTrack::Save(const wchar_t* path) const {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const uint32_t hdr[3] = {0x53504C54, (uint32_t)kLipVisemes, (uint32_t)frames.size()}; // "TLPS"
    bool ok = fwrite(hdr, 4, 3, f) == 3 && fwrite(&rate, 4, 1, f) == 1;
    if (ok && !frames.empty()) ok = fwrite(frames.data(), sizeof(frames[0]), frames.size(), f) == frames.size();
    return fclose(f) == 0 && ok;
}

bool LipTrack::Load(const wchar_t* path) {
    frames.clear();
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    uint32_t hdr[3];
    bool ok = fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x53504C54 && hdr[1] == kLipVisemes && fread(&rate, 4, 1, f) == 1;
    if (ok) {
        frames.resize(hdr[2]);
        ok = fread(frames.data(), sizeof(frames[0]), frames.size(), f) == frames.size();
    }
    fclose(f);
    if (!ok) frames.clear();
    return ok;
}

void LipTrack::Sample(double seconds, float out[kLipVisemes]) const {
    std::fill(out, out + kLipVisemes, 0.0f);
    if (frames.empty()) return;
    const double p = std::max(seconds, 0.0) * rate;
    const size_t i = std::min((size_t)p, frames.size() - 1), j = std::min(i + 1, frames.size() - 1);
    const float u = (float)(p - std::floor(p));
    for (int k = 0; k < kLipVisemes; ++k) out[k] = frames[i][k] + (frames[j][k] - frames[i][k]) * u;
}

MicRecorder::~MicRecorder() { if (Running()) Stop(); }

bool MicRecorder::Start() {
    if (Running()) return false;
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, kLipSampleRate, kLipSampleRate * 2, 2, 16, 0};
    HWAVEIN in = nullptr;
    if (waveInOpen(&in, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    wave_ = in;
    samples_.clear();
    stop_ = false;
    thread_ = std::thread([this, in] {
        constexpr int kBuffers = 6, kSamples = kLipSampleRate / 10;
        std::vector<int16_t> mem(kBuffers * kSamples);
        WAVEHDR hdr[kBuffers] = {};
        for (int i = 0; i < kBuffers; ++i) {
            hdr[i].lpData = (LPSTR)(mem.data() + i * kSamples);
            hdr[i].dwBufferLength = kSamples * 2;
            waveInPrepareHeader(in, &hdr[i], sizeof(WAVEHDR));
            waveInAddBuffer(in, &hdr[i], sizeof(WAVEHDR));
        }
        waveInStart(in);
        int next = 0;
        auto drain = [&](bool requeue) {
            while (hdr[next].dwFlags & WHDR_DONE) {
                WAVEHDR& h = hdr[next];
                const int16_t* p = (const int16_t*)h.lpData;
                samples_.insert(samples_.end(), p, p + h.dwBytesRecorded / 2);
                h.dwFlags &= ~WHDR_DONE;
                if (requeue) waveInAddBuffer(in, &h, sizeof(WAVEHDR));
                next = (next + 1) % kBuffers;
            }
        };
        while (!stop_) { drain(true); Sleep(10); }
        waveInReset(in); // hands back the part-filled buffers, oldest first
        drain(false);
        for (int i = 0; i < kBuffers; ++i) waveInUnprepareHeader(in, &hdr[i], sizeof(WAVEHDR));
        waveInClose(in);
    });
    return true;
}

std::vector<int16_t> MicRecorder::Stop() {
    if (!Running()) return {};
    stop_ = true;
    thread_.join();
    wave_ = nullptr;
    return std::move(samples_);
}

bool WriteWav16k(const wchar_t* path, const std::vector<int16_t>& pcm) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const uint32_t bytes = (uint32_t)pcm.size() * 2, riff = 36 + bytes, fmtLen = 16, rate = kLipSampleRate, byteRate = rate * 2;
    const uint16_t pcmTag = 1, channels = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcmTag, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    const bool ok = pcm.empty() || fwrite(pcm.data(), 2, pcm.size(), f) == pcm.size();
    return fclose(f) == 0 && ok;
}

bool ReadWav16k(const wchar_t* path, std::vector<int16_t>& pcm) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    char tag[12];
    bool ok = fread(tag, 1, 12, f) == 12 && !memcmp(tag, "RIFF", 4) && !memcmp(tag + 8, "WAVE", 4), found = false;
    uint16_t fmt[8] = {};
    uint32_t rate = 0;
    while (ok && !found) {
        char id[4];
        uint32_t len = 0;
        if (fread(id, 1, 4, f) != 4 || fread(&len, 4, 1, f) != 1) { ok = false; break; }
        if (!memcmp(id, "fmt ", 4) && len >= 16) {
            fread(fmt, 2, 8, f);
            memcpy(&rate, fmt + 2, 4);
            fseek(f, len - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            ok = fmt[1] == 1 && fmt[7] == 16 && rate == kLipSampleRate;
            if (ok) { pcm.resize(len / 2); ok = fread(pcm.data(), 2, pcm.size(), f) == pcm.size(); }
            found = true;
        } else fseek(f, len + (len & 1), SEEK_CUR);
    }
    fclose(f);
    return ok && found;
}

bool TranscribeWav(const std::wstring& wavPath, const std::wstring& wordsPath, std::string* error) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\')); // ...\bin
    std::wstring cmd = L"python \"" + dir + L"\\..\\gameenginetools\\lipsync\\transcribe.py\" \"" + wavPath + L"\" \"" + wordsPath + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (error) *error = "could not start python";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0 && error) *error = "the transcription script failed";
    return code == 0;
}

std::vector<LipWord> ReadLipWords(const wchar_t* path) {
    std::vector<LipWord> out;
    FILE* f = _wfopen(path, L"r");
    if (!f) return out;
    char line[512], text[400];
    LipWord w;
    while (fgets(line, sizeof(line), f))
        if (sscanf_s(line, "%f\t%f\t%399[^\r\n]", &w.start, &w.end, text, (unsigned)sizeof(text)) == 3) {
            w.text = text;
            out.push_back(w);
        }
    fclose(f);
    return out;
}

LipTrack AnalyzeLips(const std::vector<int16_t>& pcm, const std::vector<LipWord>& words, std::string* report) {
    LipTrack t;
    const int hop = kLipSampleRate / (int)t.rate;
    const int n = (int)(pcm.size() / hop) + 1;
    if (pcm.size() < kWindow) return t;

    // Scores per frame against both sample voices; whichever fits this speaker better is used.
    std::vector<std::array<float, kMfcc>> mfcc(n);
    std::vector<float> rms(n);
    std::vector<float> buf(kWindow);
    for (int f = 0; f < n; ++f) {
        const int start = f * hop - kWindow / 2;
        float e = 0;
        for (int i = 0; i < kWindow; ++i) {
            const int at = start + i;
            buf[i] = at >= 0 && at < (int)pcm.size() ? pcm[at] / 32768.0f : 0.0f;
            e += buf[i] * buf[i];
        }
        rms[f] = std::sqrt(e / kWindow);
        Mfcc(buf.data(), mfcc[f].data());
    }
    std::vector<float> sorted = rms;
    std::sort(sorted.begin(), sorted.end());
    const float floorRms = sorted[sorted.size() / 10] * 1.5f + 1e-4f, peakRms = sorted[sorted.size() * 97 / 100];
    std::vector<float> level(n);
    for (int f = 0; f < n; ++f)
        level[f] = peakRms > floorRms * 1.5f ? std::pow(std::clamp((rms[f] - floorRms) / (peakRms - floorRms), 0.0f, 1.0f), 0.6f) : 0.0f;
    float fit[2] = {};
    for (int f = 0; f < n; ++f) {
        if (level[f] < 0.3f) continue;
        for (int v = 0; v < 2; ++v) {
            float best = 0;
            for (int p = 0; p < 7; ++p) best = std::max(best, Cosine(mfcc[f].data(), kLipProfile[v][p]));
            fit[v] += best;
        }
    }
    const int voice = fit[1] > fit[0] ? 1 : 0;

    // What the transcript says the mouth is doing, frame by frame.
    std::vector<std::array<float, kLipVisemes>> plan(n, std::array<float, kLipVisemes>{});
    std::vector<uint8_t> inWord(n, 0);
    std::vector<std::vector<float>> closure(2, std::vector<float>(n, 0.0f));
    for (const LipWord& w : words) {
        const int a = std::clamp((int)(w.start * t.rate), 0, n - 1), b = std::clamp((int)(w.end * t.rate), a, n - 1);
        for (int f = a; f <= b; ++f) inWord[f] = 1;
        const std::vector<Phone> phones = WordPhones(w.text);
        float total = 0;
        for (const Phone& p : phones) total += p.length;
        float at = 0;
        for (const Phone& p : phones) {
            const float t0 = w.start + (w.end - w.start) * at / std::max(total, 1.0f);
            at += p.length;
            const float t1 = w.start + (w.end - w.start) * at / std::max(total, 1.0f);
            if (p.viseme < 0) continue;
            const int f0 = std::clamp((int)(t0 * t.rate), 0, n - 1), f1 = std::clamp((int)(t1 * t.rate), f0, n - 1);
            if (p.viseme == LV_MBP || p.viseme == LV_FV) {
                int c = (f0 + f1) / 2;
                if (p.viseme == LV_MBP) { // closed lips are quiet: find the dip near where the letters put it
                    int best = c;
                    for (int f = std::max(c - 4, 0); f <= std::min(c + 4, n - 1); ++f)
                        if (rms[f] < rms[best]) best = f;
                    c = best;
                }
                for (int f = std::max(c - 2, 0); f <= std::min(c + 2, n - 1); ++f) closure[p.viseme - LV_MBP][f] = 1;
            } else for (int f = f0; f <= f1; ++f) plan[f][p.viseme] = 1;
        }
    }

    static const int kOpenFromProfile[6][2] = {{LV_A, 0}, {LV_I, 1}, {LV_U, 2}, {LV_E, 3}, {LV_O, 4}, {LV_S, kProfileS}};
    std::vector<std::vector<float>> tracks(kLipVisemes, std::vector<float>(n, 0.0f));
    for (int f = 0; f < n; ++f) {
        // uLipSync's score, then the transcript's opinion on top. Its letter rules are rough, so audio keeps a vote.
        float score[6], sum = 0;
        for (int k = 0; k < 6; ++k) {
            const auto [vis, prof] = std::pair(kOpenFromProfile[k][0], kOpenFromProfile[k][1]);
            // uLipSync sharpens with a power of 100, tuned to a profile recorded from the speaker. We have sample voices,
            // so a softer power, or the nearest one takes all whether it fits or not.
            score[k] = std::pow(Cosine(mfcc[f].data(), kLipProfile[voice][prof]), words.empty() ? 100.0f : kSharpen);
            if (!words.empty()) score[k] *= 0.1f + plan[f][vis];
            sum += score[k];
        }
        // Silence competes only without a transcript: inside a word, the loudness already says how far the mouth opens.
        const float sil = words.empty() ? std::pow(Cosine(mfcc[f].data(), kLipProfile[voice][kProfileSil]), 100.0f) : 0.0f;
        const float open = level[f] * (words.empty() || inWord[f] ? 1.0f : 0.25f); // outside any word: breath, not speech
        // The sample voices rarely match a real speaker's vowels, so where the transcript names a shape it mostly decides,
        // and the audio supplies how far and when.
        float planned = 0;
        for (int k = 0; k < 6; ++k) planned += plan[f][kOpenFromProfile[k][0]];
        for (int k = 0; k < 6; ++k) {
            const int vis = kOpenFromProfile[k][0];
            float share = sum > 0 ? score[k] / (sum + sil) : 0.0f;
            if (planned > 0) share = kAudioVote * share + (1 - kAudioVote) * plan[f][vis] / planned;
            tracks[vis][f] = open * share;
        }
    }
    for (int k = 0; k < 6; ++k) Smooth(tracks[k], 2.0f); // lips lead and trail the sound
    tracks[LV_MBP] = closure[0];
    tracks[LV_FV] = closure[1];
    Smooth(tracks[LV_MBP], 1.2f);
    Smooth(tracks[LV_FV], 1.2f);
    t.frames.resize(n);
    float seen[kLipVisemes] = {};
    for (int f = 0; f < n; ++f)
        for (int k = 0; k < kLipVisemes; ++k) {
            t.frames[f][k] = std::clamp(tracks[k][f], 0.0f, 1.0f);
            seen[k] += t.frames[f][k] / t.rate;
        }
    if (report) {
        char b[256];
        snprintf(b, sizeof(b), "%d frames, %zu words, voice %s, seconds A %.2f I %.2f U %.2f E %.2f O %.2f S %.2f MBP %.2f FV %.2f", n, words.size(),
                 voice ? "female" : "male", seen[LV_A], seen[LV_I], seen[LV_U], seen[LV_E], seen[LV_O], seen[LV_S], seen[LV_MBP], seen[LV_FV]);
        *report = b;
        for (const LipWord& w : words) { // the shape each word spent most of its time in
            static const char* const kNames = "AIUEOSMF";
            float in[kLipVisemes] = {};
            for (int f = std::clamp((int)(w.start * t.rate), 0, n - 1); f <= std::clamp((int)(w.end * t.rate), 0, n - 1); ++f)
                for (int k = 0; k < kLipVisemes; ++k) in[k] += t.frames[f][k];
            *report += "\n  " + w.text + ": ";
            for (int k = 0; k < kLipVisemes; ++k)
                if (in[k] / t.rate > 0.015f) { snprintf(b, sizeof(b), "%c%.2f ", kNames[k], in[k] / t.rate); *report += b; }
        }
    }
    return t;
}

void ApplyLips(const float v[kLipVisemes], float strength, float w[kBlendshapes]) {
    struct Shape { int channel; float amount[kLipVisemes]; };
    //                                   A     I     U     E     O     S     MBP   FV
    static const Shape kShapes[] = {
        {BS_jawOpen,            {0.45f, 0.12f, 0.12f, 0.25f, 0.28f, 0.05f, 0.00f, 0.05f}},
        {BS_mouthFunnel,        {0.00f, 0.00f, 0.35f, 0.00f, 0.70f, 0.00f, 0.00f, 0.00f}},
        {BS_mouthPucker,        {0.00f, 0.00f, 0.80f, 0.00f, 0.25f, 0.00f, 0.00f, 0.00f}},
        {BS_mouthStretchLeft,   {0.00f, 0.45f, 0.00f, 0.30f, 0.00f, 0.20f, 0.00f, 0.00f}},
        {BS_mouthStretchRight,  {0.00f, 0.45f, 0.00f, 0.30f, 0.00f, 0.20f, 0.00f, 0.00f}},
        {BS_mouthLowerDownLeft, {0.30f, 0.20f, 0.00f, 0.30f, 0.00f, 0.15f, 0.00f, 0.00f}},
        {BS_mouthLowerDownRight,{0.30f, 0.20f, 0.00f, 0.30f, 0.00f, 0.15f, 0.00f, 0.00f}},
        {BS_mouthUpperUpLeft,   {0.10f, 0.00f, 0.00f, 0.15f, 0.00f, 0.15f, 0.00f, 0.25f}},
        {BS_mouthUpperUpRight,  {0.10f, 0.00f, 0.00f, 0.15f, 0.00f, 0.15f, 0.00f, 0.25f}},
        {BS_mouthPressLeft,     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.60f, 0.00f}},
        {BS_mouthPressRight,    {0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.60f, 0.00f}},
        {BS_mouthRollLower,     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.20f, 0.60f}},
    };
    float speaking = 0;
    for (int k = 0; k < kLipVisemes; ++k) speaking += v[k];
    const float s = std::clamp(strength, 0.0f, 1.0f) * std::clamp(speaking * 3.0f, 0.0f, 1.0f);
    const float shut = 1 - std::clamp(v[LV_MBP] + 0.6f * v[LV_FV], 0.0f, 1.0f); // closures win over the open shapes
    for (const Shape& sh : kShapes) {
        float a = 0;
        for (int k = 0; k < kLipVisemes; ++k) a += sh.amount[k] * v[k] * (k < LV_MBP ? shut * kOpenAmount : 1.0f);
        a = std::clamp(a, 0.0f, 1.0f);
        w[sh.channel] += (a - w[sh.channel]) * s;
    }
}

} // namespace tbd
