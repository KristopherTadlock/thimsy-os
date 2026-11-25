// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-prop, run as a program (its path is in THIMSY_PROP).
#include <gtest/gtest.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"
#include "thimsy/config.h"

using thimsy::ConfigStore;
using thimsy_test::ScratchDir;

namespace {

class PropTool : public ::testing::Test {
 protected:
    void SetUp() override {
        const char* p = getenv("THIMSY_PROP");
        if (!p || !*p) GTEST_SKIP() << "THIMSY_PROP is not set";
        tool = p;
        roots.persistFile = dir / "storage/.config/thimsy/persist.props";
        roots.runDir = dir / "run/thimsy/props";
        env = {"THIMSY_PERSIST_FILE=" + roots.persistFile, "THIMSY_RUN_DIR=" + roots.runDir,
               "THIMSY_DEVICE_JSON=" + (dir / "device.json"), "THIMSY_BUILD_PROPS=" + (dir / "build.props"),
               "THIMSY_INSTALL_JSON=" + (dir / "install.json")};
    }
    thimsy_test::ToolResult run(std::vector<std::string> args) {
        args.insert(args.begin(), tool);
        return thimsy_test::runTool(args, env);
    }
    std::unique_ptr<ConfigStore> store() { return ConfigStore::open(roots); }
    void writeBootFiles() {
        thimsy_test::spit(dir / "build.props", "# build facts\nro.thimsy.version=0.1.0\nro.build.id = T1\n\n");
        thimsy_test::spit(dir / "device.json", R"({
            "id": "anbernic-rg-arc-d", "family": "rk3566", "model": "Anbernic RG ARC-D", "ram_mb": 2048,
            "panel": {"width": 480, "height": 640, "rotation": 90},
            "pad": {"face_buttons": 6},
            "product": {"manufacturer": "Anbernic", "model": "RG ARC-D"},
            "props": {"ro.gammaos.device": "arc-d"}
        })");
        thimsy_test::spit(dir / "install.json", R"({"profile": "small", "ro.product.model": "not me"})");
    }

    ScratchDir dir;
    std::string tool;
    ConfigStore::Roots roots;
    std::vector<std::string> env;
};

}  // namespace

TEST_F(PropTool, SetGetAndList) {
    EXPECT_EQ(run({"set", "sys.thimsy.a", "1"}).code(), 0);
    EXPECT_EQ(run({"get", "sys.thimsy.a"}).out, "1\n");
    EXPECT_EQ(run({"get", "sys.thimsy.none", "dflt"}).out, "dflt\n");
    EXPECT_EQ(run({"get", "sys.thimsy.none"}).out, "\n");
    EXPECT_EQ(run({"set", "persist.gammaos.scraper.ssdevpw", "hunter2"}).code(), 0);
    EXPECT_EQ(run({"set", "persist.gammaos.lines", "a\nb"}).code(), 0);
    const auto list = run({"list"});
    EXPECT_EQ(list.code(), 0);
    EXPECT_EQ(list.out,
              "persist.gammaos.lines=a\\nb\n"
              "persist.gammaos.scraper.ssdevpw=<redacted>\n"
              "sys.thimsy.a=1\n");
    EXPECT_EQ(run({"list", "sys."}).out, "sys.thimsy.a=1\n");
    // get is explicit: it prints the value, for scripts that need it.
    EXPECT_EQ(run({"get", "persist.gammaos.scraper.ssdevpw"}).out, "hunter2\n");
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->get("persist.gammaos.lines"), "a\nb");
}

TEST_F(PropTool, RefusalsAndUsage) {
    const auto tooLong = run({"set", "sys.x", std::string(92, 'v')});
    EXPECT_EQ(tooLong.code(), 1);
    EXPECT_NE(tooLong.err.find("91"), std::string::npos) << tooLong.err;
    EXPECT_EQ(run({"set", "bad..key", "1"}).code(), 1);
    EXPECT_EQ(run({}).code(), 64);
    EXPECT_EQ(run({"frob"}).code(), 64);
    EXPECT_EQ(run({"set", "only-key"}).code(), 64);
    EXPECT_EQ(run({"--help"}).code(), 0);
}

TEST_F(PropTool, BootWritesReadOnlyKeysAndSeals) {
    writeBootFiles();
    const auto r = run({"boot"});
    EXPECT_EQ(r.code(), 0) << r.err;
    EXPECT_EQ(r.err, "");
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->readOnlySealed());
    // build.props
    EXPECT_EQ(s->get("ro.thimsy.version"), "0.1.0");
    EXPECT_EQ(s->get("ro.build.id"), "T1");
    // The match keys NanoDeviceKeys and device-keys.json read (I24).
    for (const char* k : {"ro.product.model", "ro.product.vendor.model", "ro.product.odm.model"}) {
        EXPECT_EQ(s->get(k), "RG ARC-D") << k;
    }
    for (const char* k : {"ro.product.manufacturer", "ro.product.vendor.manufacturer", "ro.product.odm.manufacturer"}) {
        EXPECT_EQ(s->get(k), "Anbernic") << k;
    }
    EXPECT_EQ(s->get("ro.product.device"), "anbernic-rg-arc-d");
    EXPECT_EQ(s->get("ro.board.platform"), "rk3566");
    EXPECT_EQ(s->get("ro.product.brand"), "");  // not given, not invented
    EXPECT_EQ(s->get("ro.gammaos.device"), "arc-d");
    // The device's own facts.
    EXPECT_EQ(s->get("ro.thimsy.device.id"), "anbernic-rg-arc-d");
    EXPECT_EQ(s->get("ro.thimsy.device.model"), "Anbernic RG ARC-D");
    EXPECT_EQ(s->get("ro.thimsy.device.ram_mb"), "2048");
    EXPECT_EQ(s->get("ro.thimsy.device.panel.rotation"), "90");
    EXPECT_EQ(s->list("ro.thimsy.device.pad").size(), 0u);  // only top-level and panel scalars
    // install.json may only add ro.thimsy.install.*.
    EXPECT_EQ(s->get("ro.thimsy.install.profile"), "small");
    EXPECT_EQ(s->get("ro.thimsy.install.ro_product_model"), "not me");

    const auto again = run({"boot"});
    EXPECT_EQ(again.code(), 1);
    EXPECT_NE(again.err.find("already sealed"), std::string::npos);
    EXPECT_EQ(run({"set", "ro.thimsy.late", "1"}).code(), 1);
    EXPECT_EQ(run({"get", "ro.product.model"}).out, "RG ARC-D\n");
}

TEST_F(PropTool, BootPrecedenceAndFallbacks) {
    thimsy_test::spit(dir / "build.props", "ro.product.model=FROM-BUILD\nsys.not.ro=1\n");
    thimsy_test::spit(dir / "device.json", R"({"id": "dev", "family": "h700", "model": "DT Model",
                                               "props": {"sys.x": "no", "ro.ok": "yes"}})");
    const auto r = run({"boot", "--install", dir / "absent.json"});
    EXPECT_EQ(r.code(), 2);  // sealed, with warnings
    EXPECT_NE(r.err.find("sys.not.ro"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("props.sys.x ignored"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("ro.product.model from"), std::string::npos) << r.err;
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->get("ro.product.model"), "FROM-BUILD");     // the build's identity wins
    EXPECT_EQ(s->get("ro.product.vendor.model"), "DT Model");  // falls back to the device-tree model
    EXPECT_EQ(s->get("ro.board.platform"), "h700");            // falls back to the family
    EXPECT_EQ(s->get("ro.ok"), "yes");
    EXPECT_TRUE(s->readOnlySealed());
}

TEST_F(PropTool, BootWithBrokenOrMissingFiles) {
    thimsy_test::spit(dir / "build.props", "ro.thimsy.version=1\n");
    thimsy_test::spit(dir / "device.json", "{\"id\": \"x\",}");
    auto r = run({"boot"});
    EXPECT_EQ(r.code(), 2);
    EXPECT_NE(r.err.find("line 1"), std::string::npos) << r.err;
    auto s = store();
    EXPECT_EQ(s->get("ro.thimsy.version"), "1");
    EXPECT_TRUE(s->readOnlySealed());

    ScratchDir other;
    std::vector<std::string> env2 = {"THIMSY_PERSIST_FILE=" + (other / "p"), "THIMSY_RUN_DIR=" + (other / "run")};
    r = thimsy_test::runTool({tool, "boot", "--device", other / "no.json", "--build", other / "no.props", "--install",
                              other / "no.json"},
                             env2);
    EXPECT_EQ(r.code(), 2);
    EXPECT_NE(r.err.find("missing"), std::string::npos);
}

TEST_F(PropTool, WatchPrintsAChange) {
    auto r = run({"watch", "--timeout", "100", "sys.never"});
    EXPECT_EQ(r.code(), 1);  // nothing changed in time
    EXPECT_EQ(r.out, "");

    // A writer that keeps changing the key until it is stopped; watch --once exits on the first.
    fflush(stdout);
    fflush(stderr);
    const pid_t writer = fork();
    if (writer == 0) {
        auto s = ConfigStore::open(roots);
        for (int i = 0; s && i < 400; ++i) {
            s->set("sys.thimsy.w", std::to_string(i));
            s->set("sys.thimsy.wpass", "hunter2-" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        _exit(0);
    }
    r = run({"watch", "--once", "--timeout", "8000", "sys.thimsy.w"});
    const auto secret = run({"watch", "--once", "--timeout", "8000", "sys.thimsy.wpass"});
    kill(writer, SIGKILL);
    waitpid(writer, nullptr, 0);
    EXPECT_EQ(r.code(), 0) << r.err;
    EXPECT_EQ(r.out.rfind("sys.thimsy.w=", 0), 0u) << r.out;
    EXPECT_EQ(std::count(r.out.begin(), r.out.end(), '\n'), 1);
    EXPECT_EQ(secret.out, "sys.thimsy.wpass=<redacted>\n");
}
