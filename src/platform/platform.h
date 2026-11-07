#pragma once
// The portable platform layer: the few OS services the shared game code needs.
// Windows: platform_win.cpp. iOS: platform_ios.mm. No OS headers here.
// Keep this as small as real callers justify (docs/platform_split_audit.md).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>

namespace tbd::platform {

// Monotonic clock. On Windows these are QueryPerformanceCounter / Frequency exactly.
int64_t Ticks();
int64_t TickFrequency(); // ticks per second, constant for the process

inline double TicksToMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)TickFrequency(); }
inline double TicksToSeconds(int64_t ticks) { return (double)ticks / (double)TickFrequency(); }
inline double NowMs() { return TicksToMs(Ticks()); }

// Futex-style wait: sleeps while *address still equals *expected (size bytes), until WakeAddressAll(address) or
// timeoutMs passes. May return early (spuriously); callers re-check. WakeAddressAll only uses the address as a key.
void WaitOnAddress(const volatile void* address, const void* expected, std::size_t size, uint32_t timeoutMs);
void WakeAddressAll(const void* address);

// Writable folder for rebuildable caches (nav meshes). Windows: the executable's folder. iOS: Library/Caches.
std::filesystem::path CacheRoot();
// fopen with a filesystem path (wide on Windows). Returns nullptr on failure.
FILE* OpenFile(const std::filesystem::path& path, const char* mode);

// UTF-8 <-> wide strings (UTF-16 on Windows, UTF-32 on Apple).
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& w);

// PNG / JPEG in memory -> top-down RGBA8, straight alpha. False if it cannot be decoded.
bool DecodeImage(const uint8_t* data, std::size_t size, std::vector<uint8_t>& rgba, int& width, int& height);

// Environment variables (the game's TBD_* test switches), with GetEnvironmentVariableA/W's contract: returns the
// value's length when it fits in `size` (written, NUL terminated), the size needed including the NUL when it does not,
// and 0 when unset or empty. `buf` may be null with size 0 to test for presence.
uint32_t GetEnvA(const char* name, char* buf, uint32_t size);
uint32_t GetEnvW(const wchar_t* name, wchar_t* buf, uint32_t size);

// Milliseconds since boot (Windows: GetTickCount64). For coarse timeouts such as toasts.
uint64_t TickCountMs();

// Path buffer size the game's fixed buffers use (Windows MAX_PATH).
constexpr int kMaxPath = 260;
// _wfopen: wide path and mode. nullptr on failure.
FILE* WOpen(const wchar_t* path, const wchar_t* mode);

// Keys and mouse buttons the game polls. Values are the Windows virtual-key codes, so the Windows side is a plain cast.
enum class Key : uint8_t {
    MouseLeft = 0x01, MouseRight = 0x02, MouseMiddle = 0x04,
    Shift = 0x10, Control = 0x11, Alt = 0x12, Space = 0x20,
    Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28,
    A = 'A', B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
};
// Down right now, whatever the message queue says (Windows: GetAsyncKeyState). For per-frame movement.
bool KeyDownAsync(Key k);
// Down as of the input message being handled (Windows: GetKeyState). For modifiers inside input handlers.
bool KeyDownSync(Key k);
// Shells without a system key state (iOS) feed it here; Windows ignores it.
void SetKeyDown(Key k, bool down);

// Background worker threads (job system): below normal priority so the frame thread wins.
void LowerThreadPriority(std::thread& t);

} // namespace tbd::platform
