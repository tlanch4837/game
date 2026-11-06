// Audio: miniaudio (src/third_party/miniaudio.h, MIT-0 / public domain) behind a handful of free functions.
//
// Everything miniaudio does runs on one audio thread that Start() spawns and Stop() joins; the game thread only queues
// small commands and the latest listener pose, so a slow device open or a big file decode never stalls a frame.
//
// Sounds are named relative to <project>\Assets\audio without an extension: "sfx/footstep" finds footstep.wav, .ogg,
// .mp3 or .flac there, and also footstep_1 ... footstep_8 as random variations. A sound that is not on disk is silent:
// no error, no log. If Assets\audio does not exist, Start() does not even open the sound device.
//
// Buses: Master, Music, Sfx, Ambience (one volume each, 0..1). Positions are in world metres; the listener is the camera.
// Nothing starts in a TBD_SHOT / TBD_CAPTURE run (or with TBD_NOAUDIO set): Start() returns false and every call is a no-op.
//
// Zone ambience: Assets\audio\zones.txt, one rule per line, '#' starts a comment:
//     default ambience/wind_desert 0.7            the bed heard everywhere that no zone covers
//     zone <name> <x> <z> <radius> <sound> [volume]    a bed that fades in over the outer 30% of the radius
// Without the file, "ambience/wind" is the default bed. Ambience, footsteps and the music start only while Play mode runs
// (the `playing` argument of UpdateFrame); music is "music/theme" if the file exists.
#pragma once
#include <string>

#include "vecmath.h"

namespace tbd::audio {

enum class Bus { Master = 0, Music = 1, Sfx = 2, Ambience = 3 };

// Spawns the audio thread (engine and device are opened on it). False when audio is off for this run or there is no
// Assets\audio folder; every other call then does nothing. Call once, after the assets folder is known.
bool Start(const std::wstring& assetsRoot);
// Fades nothing: stops the thread, frees every sound and closes the device. Safe to call when Start returned false.
void Stop();

void SetBusVolume(Bus bus, float volume);

// Once per frame. The listener follows (eye, forward, up). `playing` is Play mode; `onFoot` is Play mode's walking, in
// which case a footstep is queued every stride of horizontal travel at the feet, eyeHeight below the eye.
void UpdateFrame(const Vec3& eye, const Vec3& forward, const Vec3& up, bool playing, bool onFoot, float eyeHeight);

// A sound at a world position, heard with distance attenuation and panning. Silent beyond ~80 m or if the file is missing.
void PlayOneShot(const char* name, const Vec3& position, float volume = 1.0f, float pitch = 1.0f, Bus bus = Bus::Sfx);
// A looping positioned sound. Returns an id for MoveSound / StopSound (never 0), also when the file turns out missing.
unsigned PlayLoop(const char* name, const Vec3& position, float volume = 1.0f, Bus bus = Bus::Sfx);
void MoveSound(unsigned id, const Vec3& position);
void StopSound(unsigned id, float fadeSeconds = 0.2f);

// Streamed from disk, not decoded up front. Replaces the current track with a cross-fade.
void PlayMusic(const char* name, float volume = 1.0f, bool loop = true, float fadeSeconds = 1.5f);
void StopMusic(float fadeSeconds = 1.5f);

} // namespace tbd::audio
