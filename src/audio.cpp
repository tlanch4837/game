// Audio on miniaudio; see audio.h for the contract. One audio thread owns the ma_engine and every ma_sound, the game thread
// only appends to a command queue and stores the latest listener pose (both under g_mutex).
#define NOMINMAX
#include "audio.h"

#include "platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// miniaudio decodes WAV, MP3 and FLAC itself. OGG Vorbis needs stb_vorbis: when src/third_party/stb_vorbis.c exists it is
// included the way miniaudio documents (header-only first, implementation after miniaudio's).
#pragma warning(push, 0)
#if __has_include("third_party/stb_vorbis.c")
#define TBD_AUDIO_VORBIS 1
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"
#endif
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
#ifdef TBD_AUDIO_VORBIS
#undef STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"
#endif
#pragma warning(pop)

namespace tbd::audio {
namespace {

constexpr float kMaxDistance = 80.0f;  // beyond this a positioned sound is not started, and a loop is silent
constexpr float kMinDistance = 1.5f;   // full volume inside this
constexpr size_t kMaxOneShots = 48;
constexpr float kFootstepStride = 2.1f; // metres of walking per step

struct Cmd {
    enum Type { Play, Loop, Move, Stop, Music, StopMusic, BusVolume, Playing } type = Play;
    unsigned id = 0;
    std::string name;
    Vec3 pos = {0, 0, 0};
    float volume = 1.0f, pitch = 1.0f, seconds = 0.0f;
    bool loop = false;
    int bus = 0;
};

struct Listener {
    Vec3 eye = {0, 0, 0}, forward = {0, 0, -1}, up = {0, 1, 0};
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<Cmd> g_queue;       // under g_mutex
Listener g_listener;            // under g_mutex
std::thread g_thread;
std::atomic<bool> g_started{false}, g_failed{false}, g_quit{false};
std::atomic<unsigned> g_nextId{1};

// Game-thread-only state (UpdateFrame, footsteps, music edge).
bool g_wasPlaying = false;
bool g_haveFoot = false;
Vec3 g_lastFoot = {0, 0, 0};
float g_stride = 0.0f;
uint32_t g_rng = 0x9e3779b9u;

float Random01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xffffffu) / 16777216.0f;
}

bool Running() { return g_started.load() && !g_failed.load(); }

void Push(Cmd c) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() > 4096) return; // the thread is not draining (device stuck): drop rather than grow
        g_queue.push_back(std::move(c));
    }
    g_cv.notify_one();
}

std::wstring Widen(const std::string& s) { return tbd::platform::Utf8ToWide(s); }

bool FileExists(const std::wstring& path) { // a file, not a folder
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

// ------------------------------------------------------------------------------------------------ audio thread

struct SoundDelete { // resetting or destroying a Voice uninitialises its sound first (and so before the engine in Run)
    void operator()(ma_sound* s) const {
        ma_sound_uninit(s);
        delete s;
    }
};

struct Voice {
    std::unique_ptr<ma_sound, SoundDelete> snd; // ma_sound must not move once initialised
    float base = 1.0f;             // the caller's volume
    float gain = 1.0f, gainTarget = 1.0f, rate = 1000.0f; // fade, per second
    bool spatial = false, loop = false, stopping = false;
    Vec3 pos = {0, 0, 0};
};

struct Bed { // one zone ambience sound
    Voice voice;
    bool tried = false, ok = false;
    float target = 0.0f;
};

struct Zone {
    float x = 0, z = 0, radius = 1, volume = 1;
    std::string sound;
};

class Mixer {
public:
    explicit Mixer(std::wstring root) : root_(std::move(root)) {}

    void Run() {
        if (ma_engine_init(nullptr, &engine_) != MA_SUCCESS) { g_failed = true; return; }
        for (int i = 1; i <= 3; ++i) ma_sound_group_init(&engine_, 0, nullptr, &groups_[i]);
        LoadZones();
        auto last = std::chrono::steady_clock::now();
        while (!g_quit.load()) {
            std::vector<Cmd> commands;
            Listener listener;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait_for(lock, std::chrono::milliseconds(15), [] { return g_quit.load() || !g_queue.empty(); });
                commands.swap(g_queue);
                listener = g_listener;
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.25f);
            last = now;
            for (const Cmd& c : commands) Handle(c);
            Tick(listener, dt);
        }
        voices_.clear();
        music_ = Voice();
        beds_.clear(); // every ma_sound is uninitialised by ~Voice, before the groups and the engine go
        for (int i = 1; i <= 3; ++i) ma_sound_group_uninit(&groups_[i]);
        ma_engine_uninit(&engine_);
    }

private:
    // ---------------------------------------------------------------- files
    // The files behind a sound name: name.ext and name_1..name_8 for ext in wav / ogg / mp3 / flac. Cached, also when empty.
    const std::vector<std::wstring>& Resolve(const std::string& name) {
        auto it = files_.find(name);
        if (it != files_.end()) return it->second;
        std::vector<std::wstring>& out = files_[name];
        if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos || name[0] == '/' || name[0] == '\\')
            return out; // stays inside Assets\audio
        std::string clean = name;
        std::replace(clean.begin(), clean.end(), '/', '\\');
        const std::wstring stem = root_ + L"\\" + Widen(clean);
        static const wchar_t* exts[] = {L".wav", L".ogg", L".mp3", L".flac"};
        auto probe = [&](const std::wstring& s) {
            for (const wchar_t* e : exts)
                if (FileExists(s + e)) { out.push_back(s + e); return; }
        };
        probe(stem);
        for (int i = 1; i <= 8; ++i) probe(stem + L"_" + std::to_wstring(i));
        if (out.empty() && FileExists(stem)) out.push_back(stem); // the name already carries its extension
        return out;
    }

    const std::wstring* Pick(const std::string& name) {
        const std::vector<std::wstring>& f = Resolve(name);
        if (f.empty()) return nullptr;
        size_t i = 0;
        if (f.size() > 1) {
            int& last = lastPick_[name];
            i = (size_t)(Random01Thread() * (float)f.size()) % f.size();
            if ((int)i == last) i = (i + 1) % f.size();
            last = (int)i;
        }
        return &f[i];
    }

    float Random01Thread() { // the audio thread's own generator; g_rng belongs to the game thread
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(rng_ & 0xffffffu) / 16777216.0f;
    }

    // ---------------------------------------------------------------- voices
    bool Open(Voice& v, const std::wstring& path, bool stream, int bus, bool spatial, const Vec3& pos, float volume, float pitch, bool loop) {
        const ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        ma_sound_group* group = bus >= 1 && bus <= 3 ? &groups_[bus] : nullptr;
        std::unique_ptr<ma_sound> raw = std::make_unique<ma_sound>();
        if (ma_sound_init_from_file_w(&engine_, path.c_str(), flags, group, nullptr, raw.get()) != MA_SUCCESS)
            return false; // an unreadable file is as silent as a missing one; nothing was initialised
        v.snd.reset(raw.release());
        v.base = volume;
        v.spatial = spatial;
        v.loop = loop;
        v.pos = pos;
        ma_sound_set_spatialization_enabled(v.snd.get(), spatial ? MA_TRUE : MA_FALSE);
        if (spatial) {
            ma_sound_set_attenuation_model(v.snd.get(), ma_attenuation_model_inverse);
            ma_sound_set_rolloff(v.snd.get(), 1.0f);
            ma_sound_set_min_distance(v.snd.get(), kMinDistance);
            ma_sound_set_max_distance(v.snd.get(), kMaxDistance);
            ma_sound_set_doppler_factor(v.snd.get(), 0.0f);
            ma_sound_set_position(v.snd.get(), pos.x, pos.y, pos.z);
        }
        ma_sound_set_pitch(v.snd.get(), pitch);
        ma_sound_set_looping(v.snd.get(), loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(v.snd.get(), 0.0f);
        ma_sound_start(v.snd.get());
        return true;
    }

    void Handle(const Cmd& c) {
        switch (c.type) {
        case Cmd::Play: {
            if (OneShotCount() >= kMaxOneShots || Distance(c.pos) > kMaxDistance) return;
            const std::wstring* path = Pick(c.name);
            if (!path) return;
            Voice v;
            if (!Open(v, *path, false, c.bus, true, c.pos, c.volume, c.pitch, false)) return;
            v.gain = v.gainTarget = 1.0f;
            voices_[c.id] = std::move(v);
            break;
        }
        case Cmd::Loop: {
            const std::wstring* path = Pick(c.name);
            if (!path) return;
            Voice v;
            if (!Open(v, *path, true, c.bus, true, c.pos, c.volume, 1.0f, true)) return;
            v.gain = 0.0f; // fades in
            v.gainTarget = 1.0f;
            v.rate = 5.0f;
            voices_[c.id] = std::move(v);
            break;
        }
        case Cmd::Move: {
            auto it = voices_.find(c.id);
            if (it != voices_.end() && it->second.snd) {
                it->second.pos = c.pos;
                ma_sound_set_position(it->second.snd.get(), c.pos.x, c.pos.y, c.pos.z);
            }
            break;
        }
        case Cmd::Stop: {
            auto it = voices_.find(c.id);
            if (it != voices_.end()) FadeOut(it->second, c.seconds);
            break;
        }
        case Cmd::Music: {
            if (music_.snd) { dying_.push_back(std::move(music_)); FadeOut(dying_.back(), c.seconds); music_ = Voice(); }
            const std::wstring* path = Pick(c.name);
            if (!path) return;
            if (!Open(music_, *path, true, 1, false, {0, 0, 0}, c.volume, 1.0f, c.loop)) return;
            music_.gain = 0.0f;
            music_.gainTarget = 1.0f;
            music_.rate = c.seconds > 0.01f ? 1.0f / c.seconds : 1000.0f;
            break;
        }
        case Cmd::StopMusic:
            if (music_.snd) { dying_.push_back(std::move(music_)); FadeOut(dying_.back(), c.seconds); music_ = Voice(); }
            break;
        case Cmd::BusVolume:
            if (c.bus == 0) ma_engine_set_volume(&engine_, c.volume);
            else if (c.bus >= 1 && c.bus <= 3) ma_sound_group_set_volume(&groups_[c.bus], c.volume);
            break;
        case Cmd::Playing:
            playing_ = c.loop;
            break;
        }
    }

    static void FadeOut(Voice& v, float seconds) {
        v.stopping = true;
        v.gainTarget = 0.0f;
        v.rate = seconds > 0.01f ? std::max(v.gain, 0.05f) / seconds : 1000.0f;
    }

    size_t OneShotCount() const {
        size_t n = 0;
        for (const auto& kv : voices_) n += kv.second.loop ? 0 : 1;
        return n;
    }

    float Distance(const Vec3& p) const {
        const float dx = p.x - listener_.eye.x, dy = p.y - listener_.eye.y, dz = p.z - listener_.eye.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Moves the fade, and sets the voice's actual volume. A looping positioned sound also dies away toward kMaxDistance,
    // because the inverse attenuation model never reaches zero.
    void Advance(Voice& v, float dt) {
        if (!v.snd) return;
        if (v.gain < v.gainTarget) v.gain = std::min(v.gain + v.rate * dt, v.gainTarget);
        else if (v.gain > v.gainTarget) v.gain = std::max(v.gain - v.rate * dt, v.gainTarget);
        float vol = v.base * v.gain;
        if (v.spatial && v.loop) vol *= std::clamp((kMaxDistance - Distance(v.pos)) / (kMaxDistance * 0.25f), 0.0f, 1.0f);
        ma_sound_set_volume(v.snd.get(), vol);
    }

    void Tick(const Listener& l, float dt) {
        listener_ = l;
        ma_engine_listener_set_position(&engine_, 0, l.eye.x, l.eye.y, l.eye.z);
        ma_engine_listener_set_direction(&engine_, 0, l.forward.x, l.forward.y, l.forward.z);
        ma_engine_listener_set_world_up(&engine_, 0, l.up.x, l.up.y, l.up.z);

        for (auto it = voices_.begin(); it != voices_.end();) {
            Voice& v = it->second;
            const bool done = !v.snd || (!v.loop && ma_sound_at_end(v.snd.get())) || (v.stopping && v.gain <= 0.0f);
            if (done) { it = voices_.erase(it); continue; }
            Advance(v, dt);
            ++it;
        }
        Advance(music_, dt);
        for (size_t i = 0; i < dying_.size();) {
            Advance(dying_[i], dt);
            if (dying_[i].gain <= 0.0f || !dying_[i].snd) dying_.erase(dying_.begin() + (std::ptrdiff_t)i);
            else ++i;
        }
        TickBeds(dt);
    }

    // ---------------------------------------------------------------- zone ambience
    void LoadZones() {
        defaultSound_ = "ambience/wind";
        defaultVolume_ = 0.7f;
        std::ifstream f(std::filesystem::path(root_ + L"\\zones.txt"), std::ios::binary);
        if (!f) return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream in(line);
            std::string word;
            if (!(in >> word)) continue;
            if (word == "default") {
                std::string sound;
                float v = 0.7f;
                if (in >> sound) { defaultSound_ = sound; if (in >> v) defaultVolume_ = v; }
            } else if (word == "zone") {
                std::string zname;
                Zone z;
                if (!(in >> zname >> z.x >> z.z >> z.radius >> z.sound) || !(z.radius > 0.0f)) continue;
                float v = 1.0f;
                if (in >> v) z.volume = v;
                zones_.push_back(z);
            }
        }
    }

    void TickBeds(float dt) {
        for (auto& kv : beds_) kv.second.target = 0.0f;
        if (playing_) {
            float strongest = 0.0f;
            for (const Zone& z : zones_) {
                const float d = std::sqrt((listener_.eye.x - z.x) * (listener_.eye.x - z.x) + (listener_.eye.z - z.z) * (listener_.eye.z - z.z));
                const float w = std::clamp((1.0f - d / z.radius) / 0.3f, 0.0f, 1.0f);
                if (w <= 0.0f) continue;
                strongest = std::max(strongest, w);
                Bed& b = beds_[z.sound];
                b.target = std::max(b.target, w * z.volume);
            }
            if (!defaultSound_.empty()) {
                Bed& b = beds_[defaultSound_];
                b.target = std::max(b.target, (1.0f - strongest) * defaultVolume_);
            }
        }
        const float k = 1.0f - std::exp(-dt / 1.2f);
        for (auto it = beds_.begin(); it != beds_.end();) {
            Bed& b = it->second;
            if (b.target > 0.001f && !b.tried) { // opened the first time it is wanted
                b.tried = true;
                const std::wstring* path = Pick(it->first);
                b.ok = path && Open(b.voice, *path, true, 3, false, {0, 0, 0}, 1.0f, 1.0f, true);
                if (b.ok) { b.voice.gain = 0.0f; b.voice.gainTarget = 0.0f; b.voice.rate = 1000.0f; }
            }
            if (b.ok && b.voice.snd) {
                b.voice.gain += (b.target - b.voice.gain) * k;
                if (b.target <= 0.0f && b.voice.gain < 0.003f) { b.voice = Voice(); b.ok = false; b.tried = false; }
                else { b.voice.gainTarget = b.voice.gain; Advance(b.voice, dt); }
            }
            ++it; // a missing file stays marked as tried, so nothing is retried every frame
        }
    }

    std::wstring root_;
    ma_engine engine_ = {};
    ma_sound_group groups_[4] = {};
    Listener listener_;
    std::unordered_map<unsigned, Voice> voices_;
    Voice music_;
    std::vector<Voice> dying_;
    std::map<std::string, Bed> beds_;
    std::vector<Zone> zones_;
    std::string defaultSound_;
    float defaultVolume_ = 0.7f;
    bool playing_ = false;
    std::map<std::string, std::vector<std::wstring>> files_;
    std::map<std::string, int> lastPick_;
    uint32_t rng_ = 0x2545f491u;
};

} // namespace

bool Start(const std::wstring& assetsRoot) {
    if (g_started.load()) return Running();
    if (tbd::platform::GetEnvA("TBD_SHOT", nullptr, 0) > 0 || tbd::platform::GetEnvA("TBD_CAPTURE", nullptr, 0) > 0 ||
        tbd::platform::GetEnvA("TBD_NOAUDIO", nullptr, 0) > 0)
        return false;
    const std::wstring root = assetsRoot + L"\\audio";
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(root), ec)) return false; // nothing to play: leave the device alone
    g_quit = false;
    g_failed = false;
    g_started = true;
    try {
        g_thread = std::thread([root] {
            try {
                Mixer mixer(root);
                mixer.Run();
            } catch (...) { g_failed = true; }
        });
    } catch (...) {
        g_started = false;
        return false;
    }
    return true;
}

void Stop() {
    if (!g_started.load()) return;
    g_quit = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    }
    g_started = false;
}

void SetBusVolume(Bus bus, float volume) {
    if (!Running()) return;
    Cmd c;
    c.type = Cmd::BusVolume;
    c.bus = (int)bus;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    Push(std::move(c));
}

void PlayOneShot(const char* name, const Vec3& position, float volume, float pitch, Bus bus) {
    if (!Running() || !name) return;
    Cmd c;
    c.type = Cmd::Play;
    c.id = g_nextId++;
    c.name = name;
    c.pos = position;
    c.volume = std::max(volume, 0.0f);
    c.pitch = std::clamp(pitch, 0.25f, 4.0f);
    c.bus = (int)bus;
    Push(std::move(c));
}

unsigned PlayLoop(const char* name, const Vec3& position, float volume, Bus bus) {
    if (!Running() || !name) return 0;
    Cmd c;
    c.type = Cmd::Loop;
    c.id = g_nextId++;
    c.name = name;
    c.pos = position;
    c.volume = std::max(volume, 0.0f);
    c.bus = (int)bus;
    const unsigned id = c.id;
    Push(std::move(c));
    return id;
}

void MoveSound(unsigned id, const Vec3& position) {
    if (!Running() || id == 0) return;
    Cmd c;
    c.type = Cmd::Move;
    c.id = id;
    c.pos = position;
    Push(std::move(c));
}

void StopSound(unsigned id, float fadeSeconds) {
    if (!Running() || id == 0) return;
    Cmd c;
    c.type = Cmd::Stop;
    c.id = id;
    c.seconds = fadeSeconds;
    Push(std::move(c));
}

void PlayMusic(const char* name, float volume, bool loop, float fadeSeconds) {
    if (!Running() || !name) return;
    Cmd c;
    c.type = Cmd::Music;
    c.name = name;
    c.volume = std::max(volume, 0.0f);
    c.loop = loop;
    c.seconds = fadeSeconds;
    Push(std::move(c));
}

void StopMusic(float fadeSeconds) {
    if (!Running()) return;
    Cmd c;
    c.type = Cmd::StopMusic;
    c.seconds = fadeSeconds;
    Push(std::move(c));
}

void UpdateFrame(const Vec3& eye, const Vec3& forward, const Vec3& up, bool playing, bool onFoot, float eyeHeight) {
    if (!Running()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listener = {eye, forward, up};
    }
    g_cv.notify_one();

    if (playing != g_wasPlaying) { // entering Play starts the music and the beds, leaving fades them out
        g_wasPlaying = playing;
        Cmd c;
        c.type = Cmd::Playing;
        c.loop = playing;
        Push(std::move(c));
        if (playing) PlayMusic("music/theme");
        else StopMusic();
    }

    if (!playing || !onFoot) { g_haveFoot = false; return; }
    const Vec3 feet = {eye.x, eye.y - eyeHeight, eye.z};
    if (!g_haveFoot) { // the first stride starts half done, so the first step comes soon after moving off
        g_haveFoot = true;
        g_lastFoot = feet;
        g_stride = kFootstepStride * 0.5f;
        return;
    }
    const float dx = feet.x - g_lastFoot.x, dz = feet.z - g_lastFoot.z;
    g_lastFoot = feet;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d > 3.0f) return; // a jump in position (teleport, toggling walk), not walking
    g_stride += d;
    if (g_stride >= kFootstepStride) {
        g_stride -= kFootstepStride;
        PlayOneShot("sfx/footstep", feet, 0.55f + 0.2f * Random01(), 0.92f + 0.16f * Random01());
    }
}

} // namespace tbd::audio
