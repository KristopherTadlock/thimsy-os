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

// See HostControl.h.

#include "HostControl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace nanohost {

namespace {

std::mutex gEventMu;          // serialises event lines
int gEventFd = -1;

std::mutex gDisplayMu;
DisplayConfig gDisplay;

std::mutex gFrameMu;          // the barrier, grants, capture and quit state
std::condition_variable gFrameCv;
uint64_t gPresents = 0;
bool gBarrier = false;
uint64_t gGrants = 0;
bool gQuitting = false;
std::string gCapturePath;
std::function<void()> gQuitHandler;
int64_t gNextVblankUs = 0;    // the pacer's next deadline (render thread only)

int64_t monotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

bool writeAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t utf8Len(std::string_view s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

void pace(int refreshHz) {
    if (refreshHz <= 0) return;
    const int64_t period = 1000000 / refreshHz;
    int64_t now = monotonicUs();
    if (gNextVblankUs == 0) {
        gNextVblankUs = now + period;
        return;
    }
    if (now < gNextVblankUs) {
        usleep((useconds_t)(gNextVblankUs - now));
        now = monotonicUs();
    }
    // A late frame starts a new cadence from now instead of catching up (as drmPaceWithoutVsync).
    gNextVblankUs = (now > gNextVblankUs + period) ? now + period : gNextVblankUs + period;
}

void controlThread(int fd) {
    std::string pending;
    char buf[1024];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            event("control_error", "\"error\":" + jsonQuote(strerror(errno)));
            return;
        }
        if (n == 0) {  // can't happen while we hold the write side too; don't spin if it does
            usleep(20000);
            continue;
        }
        pending.append(buf, (size_t)n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handleControl(line);
        }
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::string jsonQuote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            const size_t n = utf8Len(s, i);
            if (n == 0) {
                out += "\\ufffd";
                ++i;
            } else {
                out.append(s.data() + i, n);
                i += n;
            }
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += (char)c;
                }
        }
        ++i;
    }
    out += '"';
    return out;
}

int64_t monotonicMs() { return monotonicUs() / 1000; }

bool openEvents(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lk(gEventMu);
    if (gEventFd >= 0) {
        ::close(gEventFd);
        gEventFd = -1;
    }
    if (path.empty()) return true;
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (err) *err = "open " + path + ": " + strerror(errno);
        return false;
    }
    gEventFd = fd;
    return true;
}

int eventsFd() { return gEventFd; }

void event(std::string_view kind, std::string_view fields) {
    std::string line = "{\"ev\":" + jsonQuote(kind) + ",\"t_ms\":" + std::to_string(monotonicMs());
    if (!fields.empty()) {
        line += ',';
        line.append(fields.data(), fields.size());
    }
    line += "}\n";
    std::lock_guard<std::mutex> lk(gEventMu);
    if (gEventFd >= 0) writeAll(gEventFd, line.data(), line.size());
}

void setDisplayConfig(const DisplayConfig& config) {
    std::lock_guard<std::mutex> lk(gDisplayMu);
    gDisplay = config;
}

DisplayConfig displayConfig() {
    std::lock_guard<std::mutex> lk(gDisplayMu);
    return gDisplay;
}

uint64_t present(const PresentInfo& info, const CaptureFn& capture) {
    uint64_t n;
    std::string capturePath;
    {
        std::lock_guard<std::mutex> lk(gFrameMu);
        n = ++gPresents;
        capturePath.swap(gCapturePath);
    }
    if (!capturePath.empty()) {
        const bool ok = capture && capture(capturePath);
        event("capture", "\"n\":" + std::to_string(n) + ",\"path\":" + jsonQuote(capturePath) +
                             ",\"ok\":" + (ok ? "true" : "false") + ",\"w\":" + std::to_string(info.width) +
                             ",\"h\":" + std::to_string(info.height));
    }
    event("present", "\"n\":" + std::to_string(n) + ",\"slot\":" + std::to_string(info.slot) +
                         ",\"w\":" + std::to_string(info.width) + ",\"h\":" + std::to_string(info.height) +
                         (info.draws >= 0 ? ",\"draws\":" + std::to_string(info.draws) : std::string()));
    const DisplayConfig d = displayConfig();
    if (d.pace) pace(d.refreshHz);
    std::unique_lock<std::mutex> lk(gFrameMu);
    gFrameCv.wait(lk, [] { return !gBarrier || gGrants > 0 || gQuitting; });
    if (gBarrier && gGrants > 0 && !gQuitting) --gGrants;
    return n;
}

uint64_t presentCount() {
    std::lock_guard<std::mutex> lk(gFrameMu);
    return gPresents;
}

void setBarrier(bool on) {
    std::lock_guard<std::mutex> lk(gFrameMu);
    gBarrier = on && !gQuitting;
    gGrants = 0;
    gFrameCv.notify_all();
}

void grant(uint64_t frames) {
    std::lock_guard<std::mutex> lk(gFrameMu);
    gGrants += frames;
    gFrameCv.notify_all();
}

void requestCapture(const std::string& path) {
    std::lock_guard<std::mutex> lk(gFrameMu);
    gCapturePath = path;
}

void setQuitHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lk(gFrameMu);
    gQuitHandler = std::move(handler);
}

bool handleControl(std::string_view raw) {
    std::string_view line = trim(raw);
    std::string_view cmd = line.substr(0, line.find(' '));
    std::string_view arg = cmd.size() < line.size() ? trim(line.substr(cmd.size())) : std::string_view();
    if (cmd == "barrier" && (arg == "on" || arg == "off")) {
        setBarrier(arg == "on");
        return true;
    }
    if (cmd == "grant" && !arg.empty()) {
        char* end = nullptr;
        const std::string a(arg);
        const unsigned long long n = strtoull(a.c_str(), &end, 10);
        if (end && *end == '\0' && a[0] != '-') {
            grant(n);
            return true;
        }
    }
    if (cmd == "capture" && !arg.empty()) {
        requestCapture(std::string(arg));
        return true;
    }
    if (cmd == "ping") {
        event("pong", "\"token\":" + jsonQuote(arg) + ",\"presents\":" + std::to_string(presentCount()));
        return true;
    }
    if (cmd == "quit" && arg.empty()) {
        std::function<void()> handler;
        {
            std::lock_guard<std::mutex> lk(gFrameMu);
            gQuitting = true;
            gBarrier = false;
            handler = gQuitHandler;
            gFrameCv.notify_all();
        }
        event("quit_requested");
        if (handler) handler();
        return true;
    }
    event("control_error", "\"line\":" + jsonQuote(line));
    return false;
}

bool startControl(const std::string& fifoPath, std::string* err) {
    int fd = ::open(fifoPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = "open " + fifoPath + ": " + strerror(errno);
        return false;
    }
    std::thread(controlThread, fd).detach();
    return true;
}

void resetForTest() {
    openEvents("");
    std::lock_guard<std::mutex> lk(gFrameMu);
    gPresents = 0;
    gBarrier = false;
    gGrants = 0;
    gQuitting = false;
    gCapturePath.clear();
    gQuitHandler = nullptr;
    gNextVblankUs = 0;
    gFrameCv.notify_all();
}

}  // namespace nanohost
