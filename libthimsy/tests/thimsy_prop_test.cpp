// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-prop, run as a program (its path is in THIMSY_PROP).
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>

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
            "audio": {"card": "rk817_ext", "card_index": 1, "playback": {"control": "Playback Mux"}},
            "bluetooth": true,
            "pad": {"face_buttons": 6}, "modules": ["joypad", "rumble"],
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
    EXPECT_EQ(s->list("ro.thimsy.device.pad").size(), 0u);  // only top-level, panel and audio scalars
    // D20sound's card (linux/docs/init.md) and D35bluetooth's switch.
    EXPECT_EQ(s->get("ro.thimsy.device.audio.card"), "rk817_ext");
    EXPECT_EQ(s->get("ro.thimsy.device.audio.card_index"), "1");
    EXPECT_EQ(s->list("ro.thimsy.device.audio.playback").size(), 0u);
    EXPECT_EQ(s->get("ro.thimsy.device.bluetooth"), "true");
    // The names the init scripts, thimsy-session and Nano's control keys read.
    EXPECT_EQ(s->get("ro.thimsy.family"), "rk3566");
    EXPECT_EQ(s->get("ro.thimsy.device.family"), "rk3566");
    EXPECT_EQ(s->get("ro.thimsy.modules"), "joypad rumble");
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
    EXPECT_EQ(s->get("ro.thimsy.family"), "h700");
    EXPECT_EQ(s->get("ro.thimsy.modules"), "");                // no "modules": none
    EXPECT_TRUE(s->readOnlySealed());
}

TEST_F(PropTool, BootRefusesBadModuleNames) {
    thimsy_test::spit(dir / "build.props", "ro.thimsy.version=1\n");
    thimsy_test::spit(dir / "device.json", R"({"id": "dev", "family": "h700", "model": "M",
                                               "modules": ["ok_1", "bad name", 3, "dash-ok"]})");
    const auto r = run({"boot"});
    EXPECT_EQ(r.code(), 2) << r.err;
    EXPECT_NE(r.err.find("module name is ignored"), std::string::npos) << r.err;
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->get("ro.thimsy.modules"), "ok_1 dash-ok");
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

// OA-35: --defaults (S01props passes thimsy-data's copy of mods/nano-theme.props) stores each
// persist.* default persist.props lacks, as a choice made in Settings is stored.
TEST_F(PropTool, BootStoresTheDefaultsPersistPropsLacks) {
    writeBootFiles();
    {
        auto s = store();
        ASSERT_TRUE(s != nullptr);
        ASSERT_TRUE(s->set("persist.gammaos.nano.esde.themeset", "slate-es-de"));  // the user's choice
    }
    thimsy_test::spit(dir / "defaults.props",
                      "# the theme\n\n"
                      "persist.gammaos.nano.ndstheme=3\n"
                      "persist.gammaos.nano.esde.themeset=colorful-simplified-es-de\n"
                      "  persist.gammaos.nano.minima.bg = 1a1a2e \r\n"
                      "persist.gammaos.nano.empty=\n"
                      "persist.gammaos.nano.nav_sounds=0");  // no newline at the end
    const auto r = run({"boot", "--defaults", dir / "defaults.props"});
    EXPECT_EQ(r.code(), 0) << r.err;
    EXPECT_EQ(r.err, "");
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->readOnlySealed());
    EXPECT_EQ(s->get("ro.thimsy.version"), "0.1.0");
    EXPECT_EQ(s->get("persist.gammaos.nano.ndstheme"), "3");
    EXPECT_EQ(s->get("persist.gammaos.nano.esde.themeset"), "slate-es-de");  // never replaced
    EXPECT_EQ(s->get("persist.gammaos.nano.minima.bg"), "1a1a2e");
    EXPECT_EQ(s->get("persist.gammaos.nano.nav_sounds"), "0");
    EXPECT_EQ(s->list("persist.").size(), 4u);  // an empty default stores nothing
    const std::string text = thimsy_test::slurp(roots.persistFile);
    EXPECT_NE(text.find("persist.gammaos.nano.ndstheme=3\n"), std::string::npos) << text;
}

TEST_F(PropTool, DefaultsOnALaterBootKeepTheUsersChoices) {
    writeBootFiles();
    thimsy_test::spit(dir / "defaults.props", "persist.gammaos.nano.nav_sounds=0\npersist.gammaos.nano.ndstheme=3\n");
    ASSERT_EQ(run({"boot", "--defaults", dir / "defaults.props"}).code(), 0);
    ASSERT_EQ(run({"set", "persist.gammaos.nano.nav_sounds", "1"}).code(), 0);  // chosen in Settings
    struct stat before;
    ASSERT_EQ(stat(roots.persistFile.c_str(), &before), 0);
    // The next boot: a fresh run dir (tmpfs on a device), the same persist.props.
    ScratchDir nextBoot;
    for (auto& e : env) {
        if (e.rfind("THIMSY_RUN_DIR=", 0) == 0) e = "THIMSY_RUN_DIR=" + (nextBoot / "props");
    }
    const auto r = run({"boot", "--defaults", dir / "defaults.props"});
    EXPECT_EQ(r.code(), 0) << r.err;
    EXPECT_EQ(run({"get", "persist.gammaos.nano.nav_sounds"}).out, "1\n");
    EXPECT_EQ(run({"get", "persist.gammaos.nano.ndstheme"}).out, "3\n");
    struct stat after;
    ASSERT_EQ(stat(roots.persistFile.c_str(), &after), 0);
    EXPECT_EQ(after.st_ino, before.st_ino);  // every key had a value: persist.props was not rewritten
}

TEST_F(PropTool, BadDefaultsLinesAreWarnings) {
    writeBootFiles();
    thimsy_test::spit(dir / "defaults.props",
                      "persist.gammaos.nano.ndstheme=3\n"
                      "ro.not.persist=1\n"
                      "sys.thimsy.x=1\n"
                      "not a line\n"
                      "persist..bad=1\n"
                      "persist.gammaos.nano.long=" + std::string(92, 'v') + "\n"
                      "persist.gammaos.nano.nav_sounds=0\n");
    const auto r = run({"boot", "--defaults", dir / "defaults.props"});
    EXPECT_EQ(r.code(), 2) << r.err;  // sealed and seeded, with warnings
    for (const char* want : {"\"ro.not.persist=1\" ignored", "\"sys.thimsy.x=1\" ignored", "\"not a line\" ignored",
                             "\"persist..bad=1\" ignored", "persist.gammaos.nano.long ignored"}) {
        EXPECT_NE(r.err.find(want), std::string::npos) << want << "\n" << r.err;
    }
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->readOnlySealed());
    EXPECT_EQ(s->get("persist.gammaos.nano.ndstheme"), "3");
    EXPECT_EQ(s->get("persist.gammaos.nano.nav_sounds"), "0");
    EXPECT_EQ(s->get("persist.gammaos.nano.long"), "");
    EXPECT_EQ(s->get("ro.not.persist"), "");
    EXPECT_EQ(s->get("sys.thimsy.x"), "");
}

TEST_F(PropTool, MissingDefaultsFileIsAWarning) {
    writeBootFiles();
    EXPECT_EQ(run({"boot", "--defaults"}).code(), 64);
    const auto r = run({"boot", "--defaults", dir / "absent.props"});
    EXPECT_EQ(r.code(), 2);
    EXPECT_NE(r.err.find("absent.props: missing"), std::string::npos) << r.err;
    auto s = store();
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->readOnlySealed());
    EXPECT_EQ(s->get("ro.thimsy.version"), "0.1.0");
    EXPECT_EQ(s->list("persist.").size(), 0u);
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
