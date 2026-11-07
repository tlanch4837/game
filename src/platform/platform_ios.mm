// iOS / Apple implementation of src/platform/platform.h.
// Not compiled on Windows; first compiled by the Codemagic iOS build (unverified until then).
#include "platform.h"

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <time.h>
#include <chrono>
#include <cstring>

namespace tbd::platform {

// Nanoseconds since boot, not counting sleep: monotonic, like QueryPerformanceCounter.
int64_t Ticks() { return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
int64_t TickFrequency() { return 1000000000; }

// No public futex with a timeout on Apple platforms: poll in short sleeps, bounded by the timeout. The job system's
// waiter also runs jobs itself between waits, so the extra latency is at most one poll step.
void WaitOnAddress(const volatile void* address, const void* expected, std::size_t size, uint32_t timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::memcmp(const_cast<const void*>(address), expected, size) == 0 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void WakeAddressAll(const void*) {}

std::filesystem::path CacheRoot() {
    @autoreleasepool {
        NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
        NSString* dir = dirs.count ? dirs[0] : NSTemporaryDirectory();
        return std::filesystem::path(dir.fileSystemRepresentation);
    }
}

FILE* OpenFile(const std::filesystem::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

// wchar_t is 32 bits here: plain UTF-8 <-> UTF-32. Invalid bytes become U+FFFD.
std::wstring Utf8ToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) { w += (wchar_t)0xFFFD; ++i; continue; }
        uint32_t cp = len == 1 ? c : c & (0x7F >> len);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        w += (wchar_t)cp;
        i += len;
    }
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        const uint32_t cp = (uint32_t)wc;
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    return s;
}

void LowerThreadPriority(std::thread& t) {
    // Lower the worker's scheduling priority a little; QoS classes can only be set on the calling thread.
    sched_param p{};
    int policy = 0;
    pthread_t h = t.native_handle();
    if (pthread_getschedparam(h, &policy, &p) == 0) {
        const int lo = sched_get_priority_min(policy);
        if (p.sched_priority > lo) --p.sched_priority;
        pthread_setschedparam(h, policy, &p);
    }
}

} // namespace tbd::platform
