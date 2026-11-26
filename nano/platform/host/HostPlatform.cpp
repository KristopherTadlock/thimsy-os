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

// The host platform's createPlatform() (docs/linux/IMPLEMENTATION.md B.6, D.3): reads the host
// config (HostConfig.h), opens the property store at the jail's paths (the Linux image's), and
// wires the fakes to the runner's channel (HostControl.h): the display the DrmHost substitute shows,
// the fake input devices, the record-only shell gate, and a {"ev":"prop"} event for every property
// write Nano makes (the store keeps the value; the event keeps the order, which the launch
// handshake's assertions need). It keeps its configuration, and fills NanoPlatform.h's services
// with the host's fakes: the display (FakeDisplay.h, over DrmHost.cpp), the launcher
// (FakeLauncher.h: records each launch with a {"ev":"launch"} event) and the Volumes (the
// config's scripted list of drives, FakeVolumes.h).

#define LOG_TAG "NanoHost"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <log/log.h>
#include <nanocompat/nanocompat.h>

#include "FakeDisplay.h"  // G7-display
#include "FakeLauncher.h"  // G7-launch-b
#include "HostConfig.h"
#include "NanoPlatform.h"
#include "FakeAudio.h"  // G7-audio

namespace nanohost {

namespace {

class HostPlatform : public nanoplat::Platform {
 public:
    explicit HostPlatform(HostConfig c) : config(std::move(c)) {}
    const HostConfig config;
};

bool readFile(const std::string& path, std::string* out, std::string* err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = "open " + path + ": " + strerror(errno);
        return false;
    }
    out->clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            *err = "read " + path + ": " + strerror(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        out->append(buf, (size_t)n);
    }
    ::close(fd);
    return true;
}

std::string configPath(const nanoplat::PlatformArgs& args) {
    static const char kFlag[] = "--host-config";
    for (int i = 1; i < args.argc && args.argv; ++i) {
        const char* a = args.argv[i];
        if (!a) continue;
        if (strcmp(a, kFlag) == 0 && i + 1 < args.argc) return args.argv[i + 1];
        if (strncmp(a, kFlag, sizeof(kFlag) - 1) == 0 && a[sizeof(kFlag) - 1] == '=') return a + sizeof(kFlag);
    }
    const char* env = getenv("THIMSY_HEADLESS_CONFIG");
    return env ? env : "";
}

}  // namespace

}  // namespace nanohost

std::unique_ptr<nanoplat::Platform> nanoplat::createPlatform(const PlatformArgs& args) {
    using namespace nanohost;
    HostConfig cfg;
    const std::string path = configPath(args);
    std::string err;
    if (!path.empty()) {
        std::string text;
        if (!readFile(path, &text, &err) || !parseHostConfig(text, &cfg, &err)) {
            ALOGE("nano-headless: %s", err.c_str());
            return nullptr;
        }
    }

    nanocompat::InitOptions store;
    store.persistFile = cfg.persistFile;
    store.runDir = cfg.runDir;
    store.watch = true;
    if (!nanocompat::init(store, &err)) {
        ALOGE("nano-headless: property store: %s", err.c_str());
        return nullptr;
    }
    if (!cfg.logFile.empty()) nanocompat::setLogFile(cfg.logFile);
    if (!openEvents(cfg.events, &err)) {
        ALOGE("nano-headless: events: %s", err.c_str());
        return nullptr;
    }

    setDisplayConfig(cfg.display);
    evdev::setDevices(cfg.devices);
    evdev::setGrabObserver([](const std::string& node, bool grabbed) {
        event("grab", "\"path\":" + jsonQuote(node) + ",\"on\":" + (grabbed ? "true" : "false"));
    });
    shell::setRules(cfg.shellRules, cfg.shellDefaultStatus);
    setBarrier(cfg.barrier);
    for (const std::string& prefix : cfg.recordProps) {
        nanocompat::addPropertyObserver(prefix, [](std::string_view key, std::string_view value) {
            event("prop", "\"key\":" + jsonQuote(key) + ",\"value\":" + jsonQuote(value));
        });
    }
    if (!cfg.control.empty() && !startControl(cfg.control, &err)) {
        ALOGE("nano-headless: control: %s", err.c_str());
        return nullptr;
    }
    event("start", "\"pid\":" + std::to_string(getpid()) + ",\"config\":" + jsonQuote(path));
    ALOGI("nano-headless: host platform up (display %dx%d@%d, %zu input devices, %zu shell rules)",
          cfg.display.width, cfg.display.height, cfg.display.refreshHz, cfg.devices.size(), cfg.shellRules.size());
    std::unique_ptr<nanoplat::Volumes> volumes = makeFakeVolumes(cfg.volumes);   // G7-volumes
    std::unique_ptr<nanoplat::Platform> platform(new HostPlatform(std::move(cfg)));
    // The services, one line each (G7 steps).
    platform->display = createFakeDisplay(createHostDrm());  // G7-display
    platform->launcher = createFakeLauncher();  // G7-launch-b
    platform->audio.reset(new FakeAudio());
    platform->volumes = std::move(volumes);   // G7-volumes
    return platform;
}
