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

// The Linux power (Power.h): the battery from sysfs, power actions as thimsy-session requests.

#define LOG_TAG "ThimsyPower"

#include "Power.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

#include "ControlKeys.h"

namespace nanolinux {
namespace power {

namespace {

const char kSupplyDir[] = "/sys/class/power_supply";
const char kBrightnessKey[] = "persist.gammaos.nano.brightness";

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
    return s.substr(b, e - b);
}

// A small sysfs attribute, trimmed; false when it can't be read.
bool readAttr(const std::string& path, std::string* out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[128];
    ssize_t n;
    do {
        n = ::read(fd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);
    ::close(fd);
    if (n < 0) return false;
    *out = trimmed(std::string(buf, (size_t)n));
    return true;
}

// A whole decimal number (sysfs values are integers); false otherwise.
bool parseLong(const std::string& s, long long* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

bool readLong(const std::string& path, long long* out) {
    std::string s;
    return readAttr(path, &s) && parseLong(s, out);
}

std::vector<std::string> supplies(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = opendir(dir.c_str());
    if (!d) return names;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

// The charge of one battery supply, 0-100; false when it gives none.
bool chargeOf(const std::string& base, int* percent) {
    long long v = 0;
    if (readLong(base + "/capacity", &v)) {
        *percent = (int)std::max(0LL, std::min(100LL, v));
        return true;
    }
    static const char* const kPairs[][2] = {{"charge_now", "charge_full"}, {"energy_now", "energy_full"}};
    for (const auto& p : kPairs) {
        long long now = 0, full = 0;
        if (readLong(base + "/" + p[0], &now) && readLong(base + "/" + p[1], &full) && full > 0) {
            long long pct = (now * 100 + full / 2) / full;
            *percent = (int)std::max(0LL, std::min(100LL, pct));
            return true;
        }
    }
    return false;
}

class LinuxPower : public nanoplat::Power {
 public:
    explicit LinuxPower(Options o) : options_(std::move(o)) {
        if (!options_.sessionRequest) options_.sessionRequest = controlkeys::sessionRequest;
    }

    void readBattery(int& percent, bool& charging) override {
        BatteryReading r = power::readBattery(options_.root);
        if (!r.found) return;
        percent = r.percent;
        charging = r.charging;
    }

    bool setBrightnessViaHal(int /*level*/) override { return false; }

    void mirrorBrightness(int /*level*/) override {}

    int savedBrightness() override {
        char v[PROPERTY_VALUE_MAX] = {};
        property_get(kBrightnessKey, v, "");
        return parseLevel(v);
    }

    void powerAction(const char* action) override {
        const std::string a = action ? action : "";
        const std::string verb = sessionVerbFor(a);
        if (verb.empty()) {
            ALOGW("power action \"%s\": none on Linux, ignored", a.c_str());
            return;
        }
        ALOGI("power action %s: session request %s", a.c_str(), verb.c_str());
        options_.sessionRequest(verb);
    }

 private:
    Options options_;
};

}  // namespace

BatteryReading readBattery(const std::string& root) {
    BatteryReading r;
    const std::string dir = root + kSupplyDir;
    const std::vector<std::string> names = supplies(dir);
    bool externalOnline = false;  // a supply of another type (Mains, USB, ...) is online
    bool hasStatus = false;       // the battery read has a status attribute
    for (const std::string& n : names) {
        const std::string base = dir + "/" + n;
        std::string type;
        if (!readAttr(base + "/type", &type)) continue;
        if (type != "Battery") {
            long long online = 0;
            if (readLong(base + "/online", &online) && online > 0) externalOnline = true;
            continue;
        }
        if (r.found) continue;
        long long present = 1;
        if (readLong(base + "/present", &present) && present == 0) continue;
        int pct = -1;
        if (!chargeOf(base, &pct)) continue;
        r.found = true;
        r.percent = pct;
        r.supply = n;
        std::string status;
        hasStatus = readAttr(base + "/status", &status);
        if (hasStatus) r.charging = status == "Charging" || status == "Full";
    }
    if (r.found && !hasStatus) r.charging = externalOnline;
    return r;
}

std::string sessionVerbFor(const std::string& action) {
    if (action == "reboot") return "reboot";
    if (action == "shutdown") return "poweroff";
    return "";
}

int parseLevel(const std::string& text) {
    long long v = 0;
    if (!parseLong(trimmed(text), &v)) return -1;
    return (v >= 1 && v <= 255) ? (int)v : -1;
}

std::unique_ptr<nanoplat::Power> create(Options options) {
    return std::unique_ptr<nanoplat::Power>(new LinuxPower(std::move(options)));
}

}  // namespace power
}  // namespace nanolinux
