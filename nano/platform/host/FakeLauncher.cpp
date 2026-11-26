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

// The host's fake launcher (FakeLauncher.h).

#define LOG_TAG "NanoHost"

#include "FakeLauncher.h"

#include <signal.h>
#include <stdint.h>

#include <mutex>
#include <optional>
#include <string>

#include <cutils/properties.h>
#include <log/log.h>

#include "HostControl.h"
#include "NanoLaunchCommit.h"
#include "NanoMenuUtils.h"   // setLaunchRomPath, setLaunchGame

namespace nanohost {

namespace {

constexpr int64_t kSameLaunchMs = 3000;   // as the control keys' debounce on Linux
constexpr int kFakePid = 4242;

std::mutex gRecordMu;
LaunchRecord gRecord;  // guarded by gRecordMu

std::string prop(const char* name) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get((std::string("sys.gammaos.nano.") + name).c_str(), v, "");
    return v;
}

// The request in the store's handshake keys, as Linux reads them when the trigger fires.
nanoplat::LaunchRequest requestFromStore(const char* trigger) {
    nanoplat::LaunchRequest r;
    r.package = prop("launch_app");
    r.core = prop("launch_core");
    r.rom = prop("launch_rom");
    if (r.rom.empty()) r.rom = prop("launch_game");
    r.system = prop("launch_system");
    r.intent = prop("launch_intent");
    r.trigger = trigger;
    r.kind = r.core.empty() ? nanoplat::LaunchRequest::Kind::Package : nanoplat::LaunchRequest::Kind::Core;
    return r;
}

bool sameLaunch(const nanoplat::LaunchRequest& a, const nanoplat::LaunchRequest& b) {
    return a.kind == b.kind && a.package == b.package && a.core == b.core && a.rom == b.rom &&
           a.system == b.system && a.intent == b.intent;
}

bool isTrigger(const android::LaunchTuple::Write& w) {
    return (w.key == android::LaunchKey::DoLaunch || w.key == android::LaunchKey::Retroarch) && w.value == "1";
}

class FakeLauncher : public nanoplat::Launcher {
 public:
    explicit FakeLauncher(const FakeLauncherOptions& options) : options_(options) {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord = LaunchRecord();
    }

    bool nanoExitsOnLaunch() const override { return options_.exitsOnLaunch; }

    void commit(const android::LaunchTuple& tuple) override {
        for (const android::LaunchTuple::Write& w : tuple.writes) {
            switch (w.key) {
                case android::LaunchKey::RomPath:
                    android::setLaunchRomPath(w.value);
                    break;
                case android::LaunchKey::Game:
                    android::setLaunchGame(w.value, w.rom, w.core);
                    break;
                default:
                    property_set(android::launchKeyProperty(w.key), w.value.c_str());
                    break;
            }
            if (isTrigger(w)) fire(android::launchKeyProperty(w.key));
        }
    }

    bool launch(const nanoplat::LaunchRequest& request, std::string* error) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            // A child past its scripted end has ended, reported or not (nothing polls on the host).
            if (running_ && (quit_ || monotonicMs() - startedMs_ < options_.childExitAfterMs)) {
                if (error) *error = "a game is already running";
                return false;
            }
            running_ = true;
            quit_ = false;
            startedMs_ = monotonicMs();
        }
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.requests.push_back(request);
        }
        const bool core = request.kind == nanoplat::LaunchRequest::Kind::Core;
        event("launch", std::string("\"kind\":") + (core ? "\"Core\"" : "\"Package\"") +
                            ",\"package\":" + jsonQuote(request.package) + ",\"core\":" + jsonQuote(request.core) +
                            ",\"rom\":" + jsonQuote(request.rom) + ",\"system\":" + jsonQuote(request.system) +
                            ",\"intent\":" + jsonQuote(request.intent) + ",\"trigger\":" + jsonQuote(request.trigger));
        ALOGI("fake launcher: %s launch of %s by %s", core ? "core" : "package",
              (core ? request.core : request.package).c_str(), request.trigger.c_str());
        return true;
    }

    bool childRunning() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return running_;
    }

    std::optional<nanoplat::ChildExit> pollChild() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return std::nullopt;
        nanoplat::ChildExit e;
        e.pid = kFakePid;
        if (quit_) {
            e.signaled = true;
            e.status = SIGTERM;
        } else if (monotonicMs() - startedMs_ >= options_.childExitAfterMs) {
            e.status = options_.childExitStatus;
        } else {
            return std::nullopt;
        }
        running_ = false;
        return e;
    }

    void requestChildQuit() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            quit_ = true;
        }
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.quits++;
        }
        event("launch_quit");
    }

 private:
    // A trigger was written: one launch, unless the last one was this request moments ago (Nano
    // often writes do_launch and then nano_retroarch for one launch).
    void fire(const char* trigger) {
        const nanoplat::LaunchRequest request = requestFromStore(trigger);
        const int64_t now = monotonicMs();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (last_ && now - lastMs_ < kSameLaunchMs && sameLaunch(*last_, request)) return;
            last_ = request;
            lastMs_ = now;
        }
        std::string error;
        if (!launch(request, &error)) ALOGW("fake launcher: launch by %s not started: %s", trigger, error.c_str());
    }

    const FakeLauncherOptions options_;
    mutable std::mutex mu_;
    bool running_ = false;
    bool quit_ = false;
    int64_t startedMs_ = 0;
    std::optional<nanoplat::LaunchRequest> last_;
    int64_t lastMs_ = 0;
};

}  // namespace

std::unique_ptr<nanoplat::Launcher> createFakeLauncher(const FakeLauncherOptions& options) {
    return std::unique_ptr<nanoplat::Launcher>(new FakeLauncher(options));
}

LaunchRecord launchRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

}  // namespace nanohost
