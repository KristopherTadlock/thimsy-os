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

// Control keys on Linux: see ControlKeys.h.

#define LOG_TAG "ThimsyControlKeys"

#include "ControlKeys.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>
#include <nanocompat/nanocompat.h>

extern char** environ;

namespace nanolinux {
namespace controlkeys {

// The table. Every row is also a row of tools/nano-tree/control-keys.json (test_control_keys.py
// checks both ways); keep one row per line in this exact form.
// BEGIN control-keys
const Row kControlRows[] = {
    {"sys.gammaos.nano.dosleep", "1", Action::Session, "sleep", "0"},
    {"sys.gammaos.nano.dowake", "1", Action::Reset, "", "0"},
    {"sys.gammaos.nano.park_restart", "1", Action::Session, "restart-nano", "0"},
    {"sys.gammaos.nano.fixedperf", "1", Action::Hook, "fixedperf-on", ""},
    {"sys.gammaos.nano.fixedperf", "0", Action::Hook, "fixedperf-off", ""},
    {"sys.gammaos.nano.do_launch", "1", Action::Launch, "", ""},
    {"service.bootanim.nano_retroarch", "1", Action::Launch, "", ""},
    {"sys.gammaos.nano.drm_active", "1", Action::FirstFrame, "", ""},
    {"sys.gammaos.ota.prestage", "1", Action::Refuse, "", ""},
    {"sys.gammaos.drastic_nano.start", "1", Action::Refuse, "", ""},
    {"sys.gammaos.drastic_nano.start", "2", Action::Refuse, "", ""},
    {"ctl.start", "", Action::Refuse, "", ""},
    {"ctl.stop", "", Action::Refuse, "", ""},
    {"ctl.restart", "", Action::Refuse, "", ""},
    {"service.bootanim.nano_action", "", Action::Refuse, "", ""},
    {"service.bootanim.nano_action", "reboot", Action::Session, "reboot", ""},
    {"service.bootanim.nano_action", "shutdown", Action::Session, "poweroff", ""},
    {"sys.gammaos.bt.pair_reply", "", Action::Refuse, "", ""},
    {"sys.gammaos.bt.pair_request", "", Action::Refuse, "", ""},
    {"sys.gammaos.bt_autopair", "", Action::Refuse, "", ""},
    {"sys.gammaos.nano.qs_summon", "1", Action::Refuse, "", ""},
    {"sys.gammaos.nano.show_overlay", "1", Action::Refuse, "", ""},
    {"sys.gammaos.nano.restart", "1", Action::Refuse, "", ""},
};
// END control-keys
const size_t kControlRowCount = sizeof(kControlRows) / sizeof(kControlRows[0]);

namespace {

constexpr const char* kRequestKey = "sys.thimsy.session.request";
constexpr const char* kFirstFrameKey = "sys.thimsy.nano.first_frame";
constexpr const char* kBootCompletedKey = "sys.boot_completed";
constexpr long long kLaunchDebounceMs = 3000;

// The launch handshake (NanoMenu.cpp's launch paths; IMPLEMENTATION B.2).
constexpr const char* kLaunchKeys[] = {
    "sys.gammaos.nano.launch_app",    "sys.gammaos.nano.launched_pkg",  "sys.gammaos.nano.launch_intent",
    "sys.gammaos.nano.launch_core",   "sys.gammaos.nano.launch_rom",    "sys.gammaos.nano.launch_game",
    "sys.gammaos.nano.launch_game_core", "sys.gammaos.nano.launch_system", "sys.gammaos.nano.return_recent",
};

std::mutex gMu;
Handlers gHandlers;
std::vector<int> gObserverIds;
std::set<std::string> gRefused;
bool gFirstFrameSeen = false;
LaunchRequest gLastLaunch;
long long gLastLaunchMs = -1;
std::mutex gRequestMu;   // the read and write of the request key

std::string getProp(const char* key, const char* def = "") {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, def);
    return v;
}

long long monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void defaultSessionRequest(const std::string& verb) {
    std::lock_guard<std::mutex> l(gRequestMu);
    std::string req = nextRequest(getProp(kRequestKey), verb);
    if (property_set(kRequestKey, req.c_str()) != 0)
        ALOGE("could not write %s=%s", kRequestKey, req.c_str());
    else
        ALOGI("session request %s", req.c_str());
}

void defaultLaunch(const LaunchRequest& r) {
    std::string desc;
    for (const auto& kv : r.keys) {
        if (kv.second.empty()) continue;
        if (!desc.empty()) desc += ' ';
        desc += kv.first.substr(strlen("sys.gammaos.nano.")) + "=" + kv.second;
    }
    // thimsy-nano's main gives launcher::launchFromHandshake (Launcher.h, G7-launch-b); without it,
    // only this line.
    ALOGW("launch requested by %s, but no launcher was given: %s", r.trigger.c_str(), desc.c_str());
}

std::string familyHookPath() {
    std::string family;
    if (FILE* f = fopen("/run/thimsy/family", "re")) {
        char buf[64] = {};
        if (fgets(buf, sizeof buf, f)) family = buf;
        fclose(f);
        while (!family.empty() && (family.back() == '\n' || family.back() == ' ')) family.pop_back();
    }
    if (family.empty()) family = getProp("ro.thimsy.family");
    if (family.empty() || family.find('/') != std::string::npos || family[0] == '.') return "";
    return "/usr/share/thimsy/family/" + family + "/hook";
}

void defaultRunHook(const std::string& action) {
    std::string hook = familyHookPath();
    if (hook.empty() || access(hook.c_str(), R_OK) != 0) {
        static bool logged = false;
        if (!logged) ALOGW("no family hook (%s); %s ignored", hook.empty() ? "no family" : hook.c_str(), action.c_str());
        logged = true;
        return;
    }
    // In the background, so a slow hook never stalls the render thread that set the key.
    std::thread([hook, action]() {
        const char* argv[] = {"sh", hook.c_str(), action.c_str(), nullptr};
        pid_t pid = -1;
        int err = posix_spawn(&pid, "/bin/sh", nullptr, nullptr, const_cast<char* const*>(argv), environ);
        if (err != 0) {
            ALOGE("hook %s %s: spawn failed: %s", hook.c_str(), action.c_str(), strerror(err));
            return;
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            ALOGW("hook %s %s: exit status %d", hook.c_str(), action.c_str(), status);
    }).detach();
}

Handlers withDefaults(Handlers h) {
    if (!h.sessionRequest) h.sessionRequest = defaultSessionRequest;
    if (!h.launch) h.launch = defaultLaunch;
    if (!h.runHook) h.runHook = defaultRunHook;
    if (!h.nowMs) h.nowMs = monotonicMs;
    return h;
}

Handlers handlers() {
    std::lock_guard<std::mutex> l(gMu);
    return gHandlers;
}

bool isWildcard(const Row& r) { return !*r.value || strcmp(r.value, "*") == 0; }

bool matches(const Row& r, std::string_view value) {
    if (isWildcard(r)) return true;
    return value == r.value;
}

// A row for this exact value overrides the key's catch-all row (nano_action=reboot is a request;
// nano_action=android stays refused).
bool hasExactRow(std::string_view key, std::string_view value) {
    for (size_t i = 0; i < kControlRowCount; i++) {
        const Row& r = kControlRows[i];
        if (key == r.key && !isWildcard(r) && value == r.value) return true;
    }
    return false;
}

void fireLaunch(const Row& r) {
    Handlers h = handlers();
    LaunchRequest req;
    req.trigger = r.key;
    for (const char* k : kLaunchKeys) req.keys[k] = getProp(k);
    long long now = h.nowMs();
    {
        std::lock_guard<std::mutex> l(gMu);
        // Nano often sets do_launch and then nano_retroarch for one launch: one request.
        if (gLastLaunchMs >= 0 && now - gLastLaunchMs < kLaunchDebounceMs && gLastLaunch.keys == req.keys)
            return;
        gLastLaunch = req;
        gLastLaunchMs = now;
    }
    h.launch(req);
}

void firstFrame() {
    {
        std::lock_guard<std::mutex> l(gMu);
        if (gFirstFrameSeen) return;
        gFirstFrameSeen = true;
    }
    property_set(kFirstFrameKey, "1");
    // No Android framework comes up after Nano on Linux: the system is "booted" once Nano shows
    // its first frame. Nano gates its idle work, its background scan and real sleep (dosleep) on it.
    if (getProp(kBootCompletedKey, "0") != "1") property_set(kBootCompletedKey, "1");
    ALOGI("first frame: %s=1, %s=1", kFirstFrameKey, kBootCompletedKey);
}

void refuse(const Row& r, std::string_view value) {
    std::string id = std::string(r.key) + "=" + std::string(value);
    {
        std::lock_guard<std::mutex> l(gMu);
        if (!gRefused.insert(r.key).second) return;
    }
    ALOGW("refused (Android-only, logged once per key): %s", id.c_str());
}

void onWrite(std::string_view key, std::string_view value) {
    const bool exact = hasExactRow(key, value);
    for (size_t i = 0; i < kControlRowCount; i++) {
        const Row& r = kControlRows[i];
        if (key != r.key || !matches(r, value)) continue;
        if (exact && isWildcard(r)) continue;
        if (*r.reset) property_set(r.key, r.reset);
        switch (r.action) {
            case Action::Session:
                handlers().sessionRequest(r.arg);
                break;
            case Action::Launch:
                fireLaunch(r);
                break;
            case Action::Hook:
                handlers().runHook(r.arg);
                break;
            case Action::FirstFrame:
                firstFrame();
                break;
            case Action::Reset:
                break;
            case Action::Refuse:
                refuse(r, value);
                break;
        }
    }
}

}  // namespace

std::string nextRequest(const std::string& current, const std::string& verb) {
    unsigned long long seq = 0;
    size_t i = 0;
    while (i < current.size() && current[i] >= '0' && current[i] <= '9' && i < 18) {
        seq = seq * 10 + (unsigned long long)(current[i] - '0');
        i++;
    }
    if (i == 0 || i >= current.size() || current[i] != ':') seq = 0;
    return std::to_string(seq + 1) + ":" + verb;
}

void sessionRequest(const std::string& verb) {
    Handlers h = handlers();
    if (h.sessionRequest) {
        h.sessionRequest(verb);
    } else {
        defaultSessionRequest(verb);
    }
}

void install(Handlers h) {
    uninstall();
    std::set<std::string> keys;
    for (size_t i = 0; i < kControlRowCount; i++) keys.insert(kControlRows[i].key);
    std::lock_guard<std::mutex> l(gMu);
    gHandlers = withDefaults(std::move(h));
    gRefused.clear();
    gFirstFrameSeen = false;
    gLastLaunchMs = -1;
    gLastLaunch = LaunchRequest();
    for (const std::string& k : keys) gObserverIds.push_back(nanocompat::addPropertyObserver(k, onWrite));
}

void setLaunchHandler(std::function<void(const LaunchRequest&)> launch) {
    std::lock_guard<std::mutex> l(gMu);
    gHandlers.launch = launch ? std::move(launch) : defaultLaunch;
}

void uninstall() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> l(gMu);
        ids.swap(gObserverIds);
    }
    for (int id : ids) nanocompat::removePropertyObserver(id);
}

size_t refusedCount() {
    std::lock_guard<std::mutex> l(gMu);
    return gRefused.size();
}

}  // namespace controlkeys
}  // namespace nanolinux
