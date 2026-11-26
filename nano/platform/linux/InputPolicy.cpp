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

// The Linux input policy (InputPolicy.h): padd's intercept and its tap.

#define LOG_TAG "ThimsyInput"

#include "InputPolicy.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

namespace nanolinux {
namespace input {

namespace {

// padd's TapRecord (linux/src/thimsy-padd/src/io/Control.h): 8 bytes on the wire, host order.
constexpr size_t kRecordSize = 8;
// The largest batch read at once: a whole pad's worth of changes and its SYN_REPORT fit many times.
constexpr size_t kMaxPacket = 512 * kRecordSize;

bool interceptOn() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kInterceptKey, v, "");
    return strcmp(v, "1") == 0;
}

}  // namespace

LinuxInputPolicy::LinuxInputPolicy(Options options) : options_(std::move(options)) {
    if (interceptOn()) {
        ALOGW("%s was left on by an earlier Nano: giving the pad back", kInterceptKey);
        property_set(kInterceptKey, "0");
    }
}

LinuxInputPolicy::~LinuxInputPolicy() {
    std::lock_guard<std::mutex> lock(mu_);
    if (silenced_) property_set(kInterceptKey, "0");
    silenced_ = false;
    closeTapLocked();
}

int LinuxInputPolicy::connectTap() const {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (options_.tapPath.empty() || options_.tapPath.size() >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, options_.tapPath.data(), options_.tapPath.size());
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    // A local socket connects at once or fails; a full backlog (EAGAIN) counts as a failure.
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void LinuxInputPolicy::closeTapLocked() {
    if (tap_ >= 0) close(tap_);
    tap_ = -1;
}

void LinuxInputPolicy::silenceGamePad(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    if (on == silenced_) return;
    if (on) {
        // The tap first, so the first batch padd sends it after the switch is not missed.
        int fd = connectTap();
        if (fd < 0) {
            ALOGW("silenceGamePad: padd's tap %s: %s; leaving the pad to the game", options_.tapPath.c_str(),
                  strerror(errno));
            return;
        }
        tap_ = fd;
        property_set(kInterceptKey, "1");
        silenced_ = true;
        ALOGI("silenceGamePad: on (padd intercept; the menu reads %s)", options_.tapPath.c_str());
    } else {
        property_set(kInterceptKey, "0");
        silenced_ = false;
        closeTapLocked();
        ALOGI("silenceGamePad: off");
    }
}

bool LinuxInputPolicy::silenced() const {
    std::lock_guard<std::mutex> lock(mu_);
    return silenced_;
}

int LinuxInputPolicy::tapFd() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tap_;
}

size_t LinuxInputPolicy::readTap(std::vector<struct input_event>* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!silenced_) return 0;
    if (tap_ < 0) {
        tap_ = connectTap();  // padd restarted: its new tap, still under the key we set
        if (tap_ < 0) return 0;
        ALOGI("readTap: reconnected to %s", options_.tapPath.c_str());
    }
    size_t added = 0;
    std::vector<unsigned char> buf(kMaxPacket);
    for (;;) {
        ssize_t n = recv(tap_, buf.data(), buf.size(), MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ALOGW("readTap: %s: %s", options_.tapPath.c_str(), strerror(errno));
                closeTapLocked();
            }
            break;
        }
        if (n == 0) {
            ALOGW("readTap: padd closed %s", options_.tapPath.c_str());
            closeTapLocked();
            break;
        }
        struct timeval now;
        gettimeofday(&now, nullptr);
        for (size_t off = 0; off + kRecordSize <= static_cast<size_t>(n); off += kRecordSize) {
            uint16_t type, code;
            int32_t value;
            memcpy(&type, &buf[off], sizeof type);
            memcpy(&code, &buf[off + 2], sizeof code);
            memcpy(&value, &buf[off + 4], sizeof value);
            struct input_event ev;
            memset(&ev, 0, sizeof ev);
            ev.input_event_sec = now.tv_sec;
            ev.input_event_usec = now.tv_usec;
            ev.type = type;
            ev.code = code;
            ev.value = value;
            out->push_back(ev);
            ++added;
        }
    }
    return added;
}

std::unique_ptr<LinuxInputPolicy> create(Options options) {
    return std::unique_ptr<LinuxInputPolicy>(new LinuxInputPolicy(std::move(options)));
}

}  // namespace input
}  // namespace nanolinux
