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

// The Linux system actions (SystemActions.h).

#define LOG_TAG "ThimsyActions"

#include "SystemActions.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

#include "WpaCtrl.h"

namespace nanolinux {
namespace actions {

namespace {

const char kWifiEnabledKey[] = "persist.thimsy.wifi.enabled";
const char kBluetoothEnabledKey[] = "persist.thimsy.bluetooth.enabled";
const char kTimezoneKey[] = "persist.sys.timezone";
constexpr int kNetScriptMs = 30000;   // D30net start waits up to 5 s for the interface
constexpr int kBtPowerMs = 10000;
constexpr int kBtScriptMs = 20000;   // D35bluetooth start waits up to 5 s for the controller
constexpr int kBtConnectMs = 20000;   // a profile connect (A2DP, HID) takes seconds
constexpr int kWpaReplyMs = 3000;

void defaultFollowZone(const std::string& zone) {
    // glibc and musl re-read the zone only when TZ changes; the id is a file under zoneinfo to both.
    setenv("TZ", zone.c_str(), 1);
    tzset();
}

class LinuxSystemActions : public nanoplat::SystemActions {
 public:
    explicit LinuxSystemActions(Options o) : o_(std::move(o)) {
        if (!o_.run) o_.run = runProgram;
        if (!o_.followZone) o_.followZone = defaultFollowZone;
    }

    // Runs what is queued, then stops the worker (the platform itself is never destroyed).
    ~LinuxSystemActions() override {
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void setWifiEnabled(bool on) override {
        // The setting at once, so it reads back; the script on the worker.
        if (property_set(kWifiEnabledKey, on ? "1" : "0") != 0) ALOGE("could not write %s", kWifiEnabledKey);
        post([this, on] { wifiScript(on); });
    }

    void forgetWifiNetwork(int savedNetId) override {
        if (savedNetId < 0) return;
        post([this, savedNetId] { forget(savedNetId); });
    }

    void setBluetoothEnabled(bool on) override {
        // As Wi-Fi: the setting at once (D35bluetooth reads it at boot); the daemons and the radio on
        // the worker. On: D-Bus and bluetoothd first, then the radio; off: the radio, then the daemons.
        if (property_set(kBluetoothEnabledKey, on ? "1" : "0") != 0) ALOGE("could not write %s", kBluetoothEnabledKey);
        post([this, on] {
            if (on) {
                if (!btScript(true)) return;
                bluetoothctl({"bluetoothctl", "power", "on"}, kBtPowerMs);
            } else {
                bluetoothctl({"bluetoothctl", "power", "off"}, kBtPowerMs);
                btScript(false);
            }
        });
    }

    void connectBluetooth(const std::string& mac) override {
        if (!validMac(mac)) {
            ALOGW("Bluetooth connect: not an address: \"%s\"", mac.c_str());
            return;
        }
        post([this, mac] { bluetoothctl({"bluetoothctl", "connect", mac}, kBtConnectMs); });
    }

    void setTimeFormat(bool /*h12*/) override {}

    // On the caller's thread, as Android's setprop is: the zone reads back as soon as this returns.
    void setTimeZone(const std::string& tz) override {
        if (!validZoneId(tz)) {
            ALOGW("time zone: not a zone id: \"%s\"", tz.c_str());
            return;
        }
        const std::string target = o_.zoneinfo + "/" + tz;   // the link's text: the device's path
        struct stat st;
        if (stat((o_.root + target).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            ALOGW("time zone %s: no %s", tz.c_str(), target.c_str());
            return;
        }
        const std::string link = o_.root + o_.configDir + "/localtime";
        const std::string tmp = link + ".new";
        ::unlink(tmp.c_str());
        if (symlink(target.c_str(), tmp.c_str()) != 0 || rename(tmp.c_str(), link.c_str()) != 0) {
            ALOGE("time zone %s: %s: %s", tz.c_str(), link.c_str(), strerror(errno));
            ::unlink(tmp.c_str());
            return;
        }
        if (property_set(kTimezoneKey, tz.c_str()) != 0) ALOGE("could not write %s", kTimezoneKey);
        o_.followZone(tz);
        ALOGI("time zone %s", tz.c_str());
    }

 private:
    // The Wi-Fi script, the control socket and bluetoothctl can take seconds, and Nano calls these
    // from its render thread (a menu row's press): they run on one worker, in the order asked.
    void post(std::function<void()> f) {
        if (!o_.background) {
            f();
            return;
        }
        std::lock_guard<std::mutex> l(mu_);
        q_.push_back(std::move(f));
        if (!worker_.joinable()) worker_ = std::thread([this] { loop(); });
        cv_.notify_one();
    }

    void loop() {
        // Started from the render thread, it would inherit SCHED_FIFO and its nice level, and so
        // would every program it runs (NanoMenu.h's nanoThreadNormalPriority does the same).
        sched_param sp = {};
        sp.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        setpriority(PRIO_PROCESS, (int)syscall(SYS_gettid), 0);
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;
                f = std::move(q_.front());
                q_.pop_front();
            }
            f();
        }
    }

    void wifiScript(bool on) {
        RunResult r = o_.run({"/bin/sh", o_.root + o_.netScript, on ? "start" : "stop"}, kNetScriptMs);
        if (r.status != 0)
            ALOGW("Wi-Fi %s: %s exited %d%s", on ? "on" : "off", o_.netScript.c_str(), r.status,
                  r.timedOut ? " (timed out)" : "");
        else
            ALOGI("Wi-Fi %s", on ? "on" : "off");
    }

    // D35bluetooth start|stop: true when it succeeded (start: bluetoothd runs).
    bool btScript(bool on) {
        RunResult r = o_.run({"/bin/sh", o_.root + o_.btScript, on ? "start" : "stop"}, kBtScriptMs);
        if (r.status != 0) {
            ALOGW("Bluetooth %s: %s exited %d%s", on ? "on" : "off", o_.btScript.c_str(), r.status,
                  r.timedOut ? " (timed out)" : "");
            return false;
        }
        return true;
    }

    void forget(int savedNetId) {
        const std::vector<std::string> socks = wpa::sockets(o_.root + o_.wpaDir);
        if (socks.empty()) {
            ALOGW("forget network %d: no wpa_supplicant control socket in %s", savedNetId, o_.wpaDir.c_str());
            return;
        }
        wpa::Ctrl c;
        std::string err, reply;
        if (!c.open(socks.front(), o_.root + o_.wpaLocalDir, &err)) {
            ALOGW("forget network %d: %s", savedNetId, err.c_str());
            return;
        }
        const std::string remove = "REMOVE_NETWORK " + std::to_string(savedNetId);
        if (!c.request(remove, &reply, kWpaReplyMs, &err) || !wpa::isOk(reply)) {
            ALOGW("forget network %d: %s", savedNetId, err.empty() ? reply.c_str() : err.c_str());
            return;
        }
        reply.clear();
        if (!c.request("SAVE_CONFIG", &reply, kWpaReplyMs, &err) || !wpa::isOk(reply))
            ALOGW("forget network %d: removed, not saved: %s", savedNetId, err.empty() ? reply.c_str() : err.c_str());
        else
            ALOGI("forgot network %d", savedNetId);
    }

    void bluetoothctl(const std::vector<std::string>& argv, int timeoutMs) {
        RunResult r = o_.run(argv, timeoutMs);
        std::string what;
        for (size_t i = 1; i < argv.size(); i++) what += (i > 1 ? " " : "") + argv[i];
        if (r.status == 127)
            ALOGW("Bluetooth %s: bluetoothctl is not on this image", what.c_str());
        else if (r.status != 0)
            ALOGW("Bluetooth %s: bluetoothctl exited %d%s", what.c_str(), r.status, r.timedOut ? " (timed out)" : "");
        else
            ALOGI("Bluetooth %s", what.c_str());
    }

    Options o_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;  // guarded by mu_
    bool stop_ = false;                    // guarded by mu_
    std::thread worker_;
};

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

}  // namespace

bool validZoneId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
                        c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    size_t start = 0;
    for (;;) {
        size_t slash = id.find('/', start);
        const std::string part = id.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

bool validMac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < mac.size(); i++) {
        if (i % 3 == 2 ? mac[i] != ':' : !isHex(mac[i])) return false;
    }
    return true;
}

std::unique_ptr<nanoplat::SystemActions> create(Options options) {
    return std::unique_ptr<nanoplat::SystemActions>(new LinuxSystemActions(std::move(options)));
}

}  // namespace actions
}  // namespace nanolinux
