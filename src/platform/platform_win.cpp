// Windows implementation of src/platform/platform.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform/platform.h"

#pragma comment(lib, "Synchronization.lib") // WaitOnAddress

namespace tbd::platform {

int64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t TickFrequency() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return freq;
}

void WaitOnAddress(const volatile void* address, const void* expected, std::size_t size, uint32_t timeoutMs) {
    ::WaitOnAddress(const_cast<volatile void*>(address), const_cast<void*>(expected), size, timeoutMs);
}

void WakeAddressAll(const void* address) { ::WakeByAddressAll(const_cast<void*>(address)); }

std::filesystem::path CacheRoot() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::filesystem::path exe = path;
    return exe.has_parent_path() ? exe.parent_path() : std::filesystem::path(L".");
}

FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
    wchar_t wmode[8] = {};
    for (int i = 0; i < 7 && mode[i]; ++i) wmode[i] = (wchar_t)mode[i];
    FILE* f = nullptr;
    return _wfopen_s(&f, path.c_str(), wmode) == 0 ? f : nullptr;
}

void LowerThreadPriority(std::thread& t) {
    SetThreadPriority((HANDLE)t.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}

} // namespace tbd::platform
