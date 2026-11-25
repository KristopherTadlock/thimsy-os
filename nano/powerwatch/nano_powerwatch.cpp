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

// nano-powerwatch: the power button held down shuts the console down even when Android or Nano is
// stuck. PhoneWindowManager (and Nano while it grabs the input devices) shut down in order after a
// 7 s hold; this small service runs beside them, apart from system_server and Nano:
//
//   held 7 s     it starts watching for a shutdown
//   held 12 s    still no shutdown under way (system_server or Nano stuck): it asks init for one
//                (sys.powerctl), which needs neither
//   60 s later   the console is still running (a shutdown that stalled: a stuck driver, storage
//                or service): emergency sync and read-only remount, then power off from the kernel
//
// A shutdown is under way once PhoneWindowManager or Nano starts one (sys.gammaos.shutdown_started,
// set before the game is saved and closed, which can take a while), or init has been asked for one.
// The 60 s count from then, so a slow but working shutdown is left to finish.
//
// It reads the key's state from the kernel (EVIOCGKEY) rather than its events, so it still sees the
// key while Nano grabs the device. It keeps running through the shutdown (init's "shutdown
// critical"). A frozen kernel stops it too: only the power chip's own long-press cut-off covers that.
// At start it logs the Unisoc PMIC long-press reset settings, where the kernel exposes them.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <android-base/properties.h>
#include <log/log.h>

#undef LOG_TAG
#define LOG_TAG "nano-powerwatch"

namespace {

constexpr int64_t kWatchMs = 7000;        // the orderly shutdown's hold
constexpr int64_t kInitMs = 12000;        // no shutdown under way by then: ask init
constexpr int64_t kEmergencyMs = 60000;   // a shutdown under way this long: power off from the kernel
constexpr int kIdlePollMs = 500;
constexpr int kHeldPollMs = 100;
constexpr int64_t kRescanMs = 5000;

int64_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool testBit(const unsigned char* bits, int bit) {
    return bits[bit / 8] & (1 << (bit % 8));
}

// The input devices with a power key, and every node already looked at (by device number and inode,
// so a node re-created under the same name is looked at again).
struct Watch {
    std::vector<int> fds;
    std::set<std::pair<dev_t, ino_t>> seen;
};

// Opens the power-key devices not watched yet.
void scanPowerKeys(Watch& w) {
    DIR* d = opendir("/dev/input");
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + e->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) continue;
        if (!w.seen.insert({st.st_rdev, st.st_ino}).second) continue;
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char keys[KEY_MAX / 8 + 1] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && testBit(keys, KEY_POWER)) {
            char name[80] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            ALOGI("watching %s (%s)", path.c_str(), name);
            w.fds.push_back(fd);
        } else {
            close(fd);
        }
    }
    closedir(d);
}

// Whether the key is down. A device that has gone (ENODEV) is dropped.
bool powerHeld(std::vector<int>& fds) {
    bool held = false;
    for (size_t i = 0; i < fds.size();) {
        unsigned char state[KEY_MAX / 8 + 1] = {};
        if (ioctl(fds[i], EVIOCGKEY(sizeof(state)), state) < 0) {
            if (errno == ENODEV) { close(fds[i]); fds.erase(fds.begin() + (long)i); continue; }
        } else if (testBit(state, KEY_POWER)) {
            held = true;
        }
        i++;
    }
    return held;
}

void sysrq(char c) {
    int fd = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { ALOGE("sysrq-trigger: %s", strerror(errno)); return; }
    if (write(fd, &c, 1) != 1) ALOGE("sysrq %c: %s", c, strerror(errno));
    close(fd);
}

void emergencyPowerOff() {
    ALOGE("the console is still running: emergency power off");
    sysrq('s');   // sync (the kernel queues it)
    sysrq('u');   // remount read-only
    sleep(3);
    syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, nullptr);
    ALOGE("power off returned: %s", strerror(errno));
    sysrq('o');
}

// The Unisoc PMIC long-press reset settings, for checking what the chip does on a long press.
void logPmicLongPress() {
    static const char* const kDirs[] = {"/sys/class/misc/sprd_7sreset", "/sys/devices/virtual/misc/sprd_7sreset"};
    for (const char* dir : kDirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "uevent") || !strcmp(e->d_name, "dev")) continue;
            const std::string path = std::string(dir) + "/" + e->d_name;
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) continue;
            char buf[64] = {};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
            ALOGI("pmic long press: %s = %s", e->d_name, buf);
        }
        closedir(d);
        return;
    }
    ALOGI("pmic long press: no sprd_7sreset settings exposed");
}

bool shutdownUnderway() {
    return android::base::GetBoolProperty("sys.gammaos.shutdown_started", false)
        || !android::base::GetProperty("sys.powerctl", "").empty()
        || !android::base::GetProperty("sys.shutdown.requested", "").empty();
}

}  // namespace

int main() {
    signal(SIGTERM, SIG_IGN);
    logPmicLongPress();
    Watch watch;
    scanPowerKeys(watch);
    int64_t lastScan = nowMs();
    int64_t heldSince = -1;      // the current hold's start
    int64_t triggered = -1;      // the hold's start once it reached kWatchMs
    int64_t underwaySince = -1;  // when a shutdown was first seen under way after that
    int64_t lastPoll = nowMs();
    for (;;) {
        usleep((heldSince >= 0 || triggered >= 0 ? kHeldPollMs : kIdlePollMs) * 1000);
        const int64_t now = nowMs();
        // A gap means the device slept (the clock counts sleep): a hold seen before it is over, and
        // the press that woke it starts a new one.
        if (now - lastPoll > 2000) {
            heldSince = -1;
            if (underwaySince < 0) triggered = -1;
        }
        lastPoll = now;
        const bool held = powerHeld(watch.fds);
        // Devices come and go (a module loaded late, a node re-created): look for new ones now and then.
        if (!held && triggered < 0 && now - lastScan >= kRescanMs) {
            scanPowerKeys(watch);
            lastScan = now;
        }
        if (held) {
            if (heldSince < 0) heldSince = now;
        } else {
            heldSince = -1;
        }
        if (triggered < 0) {
            if (heldSince >= 0 && now - heldSince >= kWatchMs) {
                triggered = heldSince;
                ALOGW("power held 7 s");
            }
            continue;
        }
        if (underwaySince < 0 && shutdownUnderway()) underwaySince = now;
        if (underwaySince < 0 && now - triggered >= kInitMs) {
            if (heldSince != triggered) {   // let go before then with nothing under way: start over
                triggered = -1;
                continue;
            }
            ALOGE("power held 12 s and no shutdown under way: asking init");
            android::base::SetProperty("sys.powerctl", "shutdown,powerkey");
            underwaySince = now;
        }
        if (underwaySince >= 0 && now - underwaySince >= kEmergencyMs) {
            emergencyPowerOff();
            return 1;
        }
    }
}
