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

// The host config's parser and the host platform's createPlatform(): the property store at the
// configured roots, property writes as events, the control channel, the display and devices.
// Linked like nano-headless (the compat layer, libthimsy, the shell and ioctl wraps), without Nano.

#include <sys/stat.h>

#include <chrono>
#include <thread>

#include <cutils/properties.h>
#include <gtest/gtest.h>

#include "NanoLaunchCommit.h"
#include "NanoPlatform.h"
#include "headless_test_util.h"
#include "platform/host/FakeLauncher.h"
#include "platform/host/HostConfig.h"

using namespace nanohost;
using headless_test::TmpDir;

namespace {

TEST(HostConfig, Defaults) {
    HostConfig c;
    std::string err;
    ASSERT_TRUE(parseHostConfig("{}", &c, &err)) << err;
    EXPECT_EQ(c.persistFile, "/storage/.config/thimsy/persist.props");
    EXPECT_EQ(c.runDir, "/run/thimsy/props");
    EXPECT_EQ(c.display.width, 1280);
    EXPECT_EQ(c.display.refreshHz, 60);
    EXPECT_EQ(c.shellDefaultStatus, 127);
    ASSERT_EQ(c.recordProps.size(), 1u);
    EXPECT_EQ(c.recordProps[0], "");
}

TEST(HostConfig, Full) {
    const char* text = R"({
      "events": "/run/e.jsonl", "control": "/run/ctl", "persist_file": "/p/persist.props",
      "run_dir": "/p/run", "log_file": "", "barrier": true,
      "display": {"width": 640, "height": 480, "refresh_hz": 50, "count": 2, "rotation": 270, "pace": false},
      "devices": [{"path": "/dev/input/event0", "name": "Pad", "bus": 6, "vendor": 21576, "product": 1,
                   "keys": [304, 305], "abs": [{"code": 16, "min": -1, "max": 1}], "props": [0]}],
      "shell": {"rules": [{"match": "am ", "status": 0}, {"match": "id", "exact": true, "stdout": "uid=0\n"}],
                "default_status": 1},
      "record_props": ["sys.", "persist."]})";
    HostConfig c;
    std::string err;
    ASSERT_TRUE(parseHostConfig(text, &c, &err)) << err;
    EXPECT_EQ(c.events, "/run/e.jsonl");
    EXPECT_EQ(c.control, "/run/ctl");
    EXPECT_EQ(c.persistFile, "/p/persist.props");
    EXPECT_EQ(c.logFile, "");
    EXPECT_TRUE(c.barrier);
    EXPECT_EQ(c.display.width, 640);
    EXPECT_EQ(c.display.count, 2);
    EXPECT_EQ(c.display.rotationDeg, 270);
    EXPECT_FALSE(c.display.pace);
    ASSERT_EQ(c.devices.size(), 1u);
    EXPECT_EQ(c.devices[0].name, "Pad");
    EXPECT_EQ(c.devices[0].vendor, 21576);
    ASSERT_EQ(c.devices[0].keys.size(), 2u);
    ASSERT_EQ(c.devices[0].abs.size(), 1u);
    EXPECT_EQ(c.devices[0].abs[0].code, 16);
    EXPECT_EQ(c.devices[0].abs[0].minimum, -1);
    ASSERT_EQ(c.shellRules.size(), 2u);
    EXPECT_TRUE(c.shellRules[1].exact);
    EXPECT_EQ(c.shellRules[1].out, "uid=0\n");
    EXPECT_EQ(c.shellDefaultStatus, 1);
    ASSERT_EQ(c.recordProps.size(), 2u);
}

TEST(HostConfig, Errors) {
    const char* bad[] = {
        "[]", "{\"nope\": 1}", "{\"display\": {\"width\": 0}}", "{\"display\": {\"rotation\": 45}}",
        "{\"display\": {\"wide\": 1}}", "{\"devices\": [{\"name\": \"no path\"}]}",
        "{\"devices\": [{\"path\": \"/x\", \"keys\": [99999]}]}", "{\"shell\": {\"rules\": [{\"status\": 1}]}}",
        "{\"shell\": {\"rules\": [{\"match\": \"a\", \"status\": 256}]}}", "{\"barrier\": 1}", "{\"events\": 3}",
        "{\"record_props\": [1]}", "{",
    };
    for (const char* t : bad) {
        HostConfig c;
        std::string err;
        EXPECT_FALSE(parseHostConfig(t, &c, &err)) << t;
        EXPECT_FALSE(err.empty()) << t;
    }
}

bool waitFor(const std::function<bool()>& cond, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

TEST(HostPlatform, CreateWiresTheChannel) {
    TmpDir dir;
    ASSERT_EQ(mkdir((dir / "run").c_str(), 0755), 0);
    const std::string events = dir / "events.jsonl";
    const std::string ctl = dir / "ctl";
    ASSERT_EQ(mkfifo(ctl.c_str(), 0600), 0);
    const std::string cfg = dir / "host.json";
    ASSERT_TRUE(headless_test::writeFile(
            cfg, "{\"events\": \"" + events + "\", \"control\": \"" + ctl + "\", \"persist_file\": \"" +
                         (dir / "persist.props") + "\", \"run_dir\": \"" + (dir / "run") +
                         "\", \"log_file\": \"" + (dir / "nano.log") +
                         "\", \"display\": {\"width\": 320, \"height\": 240, \"pace\": false},"
                         " \"record_props\": [\"sys.gammaos.\", \"persist.\"]}"));
    char prog[] = "nano-headless";
    char flag[] = "--host-config";
    std::vector<char> path(cfg.begin(), cfg.end());
    path.push_back('\0');
    char* argv[] = {prog, flag, path.data(), nullptr};
    nanoplat::PlatformArgs args;
    args.argc = 3;
    args.argv = argv;
    std::unique_ptr<nanoplat::Platform> p = nanoplat::createPlatform(args);
    ASSERT_TRUE(p != nullptr);

    EXPECT_EQ(displayConfig().width, 320);
    // The services (G7): the fake display, here over host_drm_fake.cpp's HostDrm (no display).
    ASSERT_TRUE(p->display != nullptr);
    EXPECT_EQ(p->display->startScanout(-1, false, nullptr), -1);
    EXPECT_FALSE(p->display->releaseForChild());
    EXPECT_EQ(property_set("sys.gammaos.nano.launch_app", "com.example"), 0);
    EXPECT_EQ(property_set("persist.gammaos.nano.theme", "xmb"), 0);
    EXPECT_EQ(property_set("debug.unrecorded", "1"), 0);
    std::string log = headless_test::readFile(events);
    EXPECT_NE(log.find("{\"ev\":\"start\""), std::string::npos);
    EXPECT_NE(log.find("{\"ev\":\"display\""), std::string::npos);
    EXPECT_NE(log.find("\"op\":\"start\",\"w\":0,\"h\":0"), std::string::npos);
    EXPECT_NE(log.find("\"key\":\"sys.gammaos.nano.launch_app\",\"value\":\"com.example\""), std::string::npos);
    EXPECT_NE(log.find("\"key\":\"persist.gammaos.nano.theme\",\"value\":\"xmb\""), std::string::npos);
    EXPECT_EQ(log.find("debug.unrecorded"), std::string::npos);
    // The store wrote where the config says.
    EXPECT_NE(headless_test::readFile(dir / "persist.props").find("persist.gammaos.nano.theme=xmb"), std::string::npos);
    EXPECT_EQ(headless_test::readFile(dir / "run/sys.gammaos.nano.launch_app"), "com.example");

    // The control channel answers.
    int w = open(ctl.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(w, 0);
    ASSERT_EQ(write(w, "ping p1\n", 8), 8);
    EXPECT_TRUE(waitFor([&] { return headless_test::readFile(events).find("\"token\":\"p1\"") != std::string::npos; }));
    close(w);

    // The fake launcher (G7-launch-b): installed, a call site's commit reaches it and a trigger is
    // a launch event.
    ASSERT_TRUE(p->launcher != nullptr);
    EXPECT_FALSE(p->launcher->childRunning());
    nanoplat::install(std::move(p));
    (void)nanoplat::get();
    android::commitLaunch(android::LaunchTuple()
                                  .core("/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64")
                                  .doLaunch("1"));
    log = headless_test::readFile(events);
    EXPECT_NE(log.find("\"key\":\"sys.gammaos.nano.do_launch\",\"value\":\"1\""), std::string::npos);
    EXPECT_NE(log.find("{\"ev\":\"launch\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"kind\":\"Core\""), std::string::npos);
    ASSERT_EQ(launchRecord().requests.size(), 1u);
}

TEST(HostPlatform, BadConfigFails) {
    TmpDir dir;
    const std::string cfg = dir / "host.json";
    ASSERT_TRUE(headless_test::writeFile(cfg, "{\"display\": {\"width\": -1}}"));
    setenv("THIMSY_HEADLESS_CONFIG", cfg.c_str(), 1);
    nanoplat::PlatformArgs args;
    EXPECT_TRUE(nanoplat::createPlatform(args) == nullptr);
    setenv("THIMSY_HEADLESS_CONFIG", (dir / "missing.json").c_str(), 1);
    EXPECT_TRUE(nanoplat::createPlatform(args) == nullptr);
    unsetenv("THIMSY_HEADLESS_CONFIG");
}

}  // namespace
