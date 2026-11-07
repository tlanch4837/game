// iOS / Apple implementation of src/platform/platform.h.
// Not compiled on Windows; first compiled by the Codemagic iOS build (unverified until then).
#include "platform/platform.h"

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
