// FrameFlag: the render loop's "something to draw" flag (see NanoMenu.h). Header-only and free of
// Android dependencies so the host tests can exercise it.
#pragma once

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

namespace android {

// Something to draw. Setting it, from any thread, also wakes the render loop's idle wait (an eventfd)
// and advances seq(), which the loop compares with the value at its last frame. A set made while
// render() runs only latches renderWants(): the screen asks for another frame (throttled when settled).
class FrameFlag {
public:
    explicit FrameFlag(bool v = false) : mV(v) {}
    FrameFlag& operator=(bool v) {
        mV.store(v, std::memory_order_relaxed);
        if (v) wake();
        return *this;
    }
    operator bool() const { return mV.load(std::memory_order_relaxed); }
    static void wake() {
        if (tInRender) { sRenderWants.store(true, std::memory_order_relaxed); return; }
        sSeq.fetch_add(1, std::memory_order_acq_rel);
        const int fd = sWakeFd.load(std::memory_order_relaxed);
        // One write until the loop next waits (it re-arms): repeated sets cost no syscall.
        if (fd >= 0 && !sArmed.exchange(true, std::memory_order_acq_rel)) {
            const uint64_t one = 1; ssize_t n = ::write(fd, &one, sizeof(one)); (void)n;
        }
    }
    static uint32_t seq() { return sSeq.load(std::memory_order_acquire); }
    static bool takeRenderWants() { return sRenderWants.exchange(false, std::memory_order_relaxed); }
    static bool renderWants() { return sRenderWants.load(std::memory_order_relaxed); }
    static void rearm() { sArmed.store(false); }   // seq_cst: ordered against nudge()'s count and flag
    static void wakeThrottled() {   // a worker's progress: at most about 4 frames a second
        const int64_t now = sNowMs();
        int64_t last = sLastThrottled.load(std::memory_order_relaxed);
        if (now - last < 250 || !sLastThrottled.compare_exchange_strong(last, now)) return;
        wake();
    }
    static void nudge() {   // wake the loop to run its polls; no frame by itself
        sNudges.fetch_add(1);   // counted first: a nudge whose write the armed flag skips is still seen
        const int fd = sWakeFd.load(std::memory_order_relaxed);
        if (fd >= 0 && !sArmed.exchange(true)) {
            const uint64_t one = 1; ssize_t n = ::write(fd, &one, sizeof(one)); (void)n;
        }
    }
    static uint32_t nudges() { return sNudges.load(); }
    static int64_t monotonicMs() {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    }
    static inline std::atomic<int> sWakeFd{-1};
    static inline thread_local bool tInRender = false;   // set during render()
    static inline int64_t (*sNowMs)() = &monotonicMs;    // wakeThrottled's clock; tests replace it
private:
    std::atomic<bool> mV;
    static inline std::atomic<uint32_t> sSeq{0};
    static inline std::atomic<uint32_t> sNudges{0};
    static inline std::atomic<bool> sArmed{false};
    static inline std::atomic<bool> sRenderWants{false};
    static inline std::atomic<int64_t> sLastThrottled{0};
};

}  // namespace android
