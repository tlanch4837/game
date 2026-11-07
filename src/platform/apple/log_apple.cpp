// Apple counterparts of src/render/log.cpp (which is Win32: dbghelp crash dumps, GetModuleFileName): the log goes to
// stderr (Xcode's console) and to engine.log in the app's Documents folder; crash reporting is left to the OS (crash logs
// reach Xcode's Devices window and TestFlight).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "../../render/renderer.h"

namespace tbd {

namespace {
std::mutex& LogLock() { static std::mutex m; return m; }
FILE* LogFile() {
    static FILE* f = [] {
        const char* home = std::getenv("HOME"); // the app's sandbox on iOS
        const std::string path = std::string(home ? home : ".") + "/Documents/engine.log";
        return std::fopen(path.c_str(), "w");
    }();
    return f;
}
} // namespace

// Documents/bin, so the game's ExeDirectory() + "\\..\\assets\\..." paths (Windows: bin\ next to assets\) reach Documents/assets,
// the folder loaded onto the device (docs/ipad_loader.md). main_ios.mm creates it; ".." needs it to exist.
std::wstring ExeDirectory() {
    const char* home = std::getenv("HOME");
    const std::string s = std::string(home ? home : ".") + "/Documents/bin";
    return std::wstring(s.begin(), s.end());
}

void Log(const char* format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(LogLock());
    std::fprintf(stderr, "%s\n", line);
    if (FILE* f = LogFile()) { std::fprintf(f, "%s\n", line); std::fflush(f); }
}

void InstallCrashHandlers() {}
void MarkCleanExit() { Log("clean exit"); }

void Renderer::LogCounts(const char* api) const {
    Log("%s: draws: scene %u (%.2f M triangles), shadow %u (%.2f M triangles), material binds %u", api, lastCounts_.draws,
        (double)lastCounts_.triangles / 1e6, lastCounts_.shadowDraws, (double)lastCounts_.shadowTriangles / 1e6, lastCounts_.materialBinds);
}

void Renderer::UploadSteps::Done(const char* api, const char* what, uint64_t bytes, double slowMs) {
    const double total = TraceNowMs() - start;
    if (total < slowMs) return;
    char line[512];
    int pos = std::snprintf(line, sizeof(line), "%s: slow %s (%llu KB) %.1f ms:", api, what, (unsigned long long)(bytes >> 10), total);
    double prev = start;
    for (int i = 0; i < n && pos > 0 && pos < (int)sizeof(line); ++i) {
        pos += std::snprintf(line + pos, sizeof(line) - (size_t)pos, " %s %.1f", name[i], at[i] - prev);
        prev = at[i];
    }
    Log("%s", line);
}

} // namespace tbd
