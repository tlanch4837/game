// Windows implementation of src/platform/platform.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform.h"

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

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void LowerThreadPriority(std::thread& t) {
    SetThreadPriority((HANDLE)t.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}

} // namespace tbd::platform
