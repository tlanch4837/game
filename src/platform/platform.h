#pragma once
// The portable platform layer: the few OS services the shared game code needs.
// Windows: platform_win.cpp. iOS: platform_ios.mm. No OS headers here.
// Keep this as small as real callers justify (docs/platform_split_audit.md).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
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

// Background worker threads (job system): below normal priority so the frame thread wins.
void LowerThreadPriority(std::thread& t);

} // namespace tbd::platform
