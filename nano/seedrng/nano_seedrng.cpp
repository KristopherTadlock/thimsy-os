/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// nano-seedrng: carries the kernel's random number generator across boots, for devices whose
// hardware generator does not feed it (the RG Cube's sprd_trng). Without a seed the kernel takes
// about two seconds of interrupts to get ready, and anything reading random numbers early waits:
// the vendor's BoringSSL self tests block init's early-init for that long.
//
//   nano-seedrng load   (early-init) if the kernel is not ready yet, credits it with the seed the
//                       last boot saved, then saves a fresh one; does nothing when it is ready
//   nano-seedrng save   (boot completed) saves a fresh seed for the next boot
//   nano-seedrng shutdown  (shutdown) notes the hardware clock as a restart begins; the next load
//                       logs how long it was from there to the new kernel starting (the part of a
//                       restart no log sees: the end of shutdown, the bootloader)
//
// The seed lives on /metadata, mounted by the first stage. It is deleted as it is read, so it is
// never credited twice.

#include <errno.h>
#include <fcntl.h>
#include <linux/random.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static const char* const kDir = "/metadata/gammaos";
static const char* const kSeed = "/metadata/gammaos/random-seed";
static const char* const kShutdownMark = "/metadata/gammaos/shutdown-rtc";
static constexpr size_t kSize = 512;

static void klog(const char* fmt, ...) {   // logd is not up at early-init
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    dprintf(fd, "<6>nano-seedrng: %s\n", msg);
    close(fd);
}

static bool kernelReady() {
    unsigned char b;
    return getrandom(&b, 1, GRND_NONBLOCK) == 1;
}

static bool writeAll(int fd, const unsigned char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool save() {
    unsigned char buf[kSize];
    if (getrandom(buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) return false;
    mkdir(kDir, 0700);
    const std::string tmp = std::string(kSeed) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = writeAll(fd, buf, sizeof(buf)) && fsync(fd) == 0;
    close(fd);
    memset(buf, 0, sizeof(buf));
    if (!ok || rename(tmp.c_str(), kSeed) != 0) { unlink(tmp.c_str()); return false; }
    int dir = open(kDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) { fsync(dir); close(dir); }
    return true;
}

// The battery-backed clock, in whole seconds; it keeps counting through a restart.
static long long rtcSeconds() {
    char buf[32] = {};
    int fd = open("/sys/class/rtc/rtc0/since_epoch", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? atoll(buf) : -1;
}

static int markShutdown() {
    const long long now = rtcSeconds();
    if (now < 0) return 1;
    mkdir(kDir, 0700);
    int fd = open(kShutdownMark, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return 1;
    const bool ok = dprintf(fd, "%lld\n", now) > 0 && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : 1;
}

static void reportRestart() {
    int fd = open(kShutdownMark, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    char buf[32] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unlink(kShutdownMark);
    const long long then = n > 0 ? atoll(buf) : -1, now = rtcSeconds();
    struct timespec up {};
    if (then <= 0 || now < 0 || clock_gettime(CLOCK_BOOTTIME, &up) != 0) return;
    const long long kernelStart = now - up.tv_sec;   // to the second
    klog("restart: shutdown began about %lld s before this kernel started (rtc %lld, now %lld, up %ld.%03ld s)",
         kernelStart - then, then, now, (long)up.tv_sec, up.tv_nsec / 1000000);
}

static int load() {
    reportRestart();
    if (kernelReady()) return 0;
    int fd = open(kSeed, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { klog("no seed yet"); return 0; }
    std::vector<unsigned char> pool(sizeof(struct rand_pool_info) + kSize);
    auto* info = reinterpret_cast<struct rand_pool_info*>(pool.data());
    ssize_t n = read(fd, info->buf, kSize);
    close(fd);
    // Credited only once it is gone for good: if it cannot be deleted, or the deletion may not
    // survive a power cut, the same seed could come back, so it is only mixed in.
    bool gone = unlink(kSeed) == 0;
    int dir = open(kDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    gone = gone && dir >= 0 && fsync(dir) == 0;
    if (dir >= 0) close(dir);
    if (n < 64) { memset(pool.data(), 0, pool.size()); klog("seed too short"); return 0; }
    info->entropy_count = gone ? (int)n * 8 : 0;
    info->buf_size = (int)n;
    int rnd = open("/dev/urandom", O_RDWR | O_CLOEXEC);
    const bool added = rnd >= 0 && ioctl(rnd, RNDADDENTROPY, info) == 0;
    if (rnd >= 0) close(rnd);
    memset(pool.data(), 0, pool.size());
    klog("seed %s, kernel %s", !added ? "not added" : gone ? "credited" : "mixed in uncredited (not deleted)",
         kernelReady() ? "ready" : "not ready");
    if (kernelReady() && !save()) klog("could not save a new seed");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "load")) return load();
    if (argc == 2 && !strcmp(argv[1], "save")) return save() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "shutdown")) return markShutdown();
    fprintf(stderr, "usage: nano-seedrng load|save|shutdown\n");
    return 2;
}
