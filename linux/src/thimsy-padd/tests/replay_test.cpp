// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The replay goldens (tests/traces/*.trace against *.golden) in-process, and the trace reader's
// own errors. The Makefile also runs padd-replay itself on every trace.
#include "core/Replay.h"

#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "padd_test_util.h"
#include "thimsy/atomic_file.h"

using namespace padd;

namespace {

std::vector<std::string> traces() {
    std::vector<std::string> out;
    DIR* d = opendir(padd_test::tracesDir().c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".trace") == 0) out.push_back(n.substr(0, n.size() - 6));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string replay(const std::string& text, std::string* err, const std::string& dir = "") {
    std::istringstream in(text);
    std::ostringstream out;
    *err = "";
    if (!runReplay(in, dir.empty() ? padd_test::tracesDir() : dir, out, err)) return "FAILED: " + out.str();
    return out.str();
}

const std::string kArc = "device ../../../../board/rk3566/devices/anbernic-rg-arc-d.json\n";

}  // namespace

TEST(Replay, GoldensMatch) {
    std::vector<std::string> names = traces();
    // The F.2 set: the ARC's three sources, a rocknix-joypad single source, an external pad.
    for (const char* want : {"arc-d", "rocknix-joypad", "external-pad", "qemu-virt"}) {
        EXPECT_TRUE(std::find(names.begin(), names.end(), want) != names.end()) << want;
    }
    for (const std::string& n : names) {
        SCOPED_TRACE(n);
        std::string text, golden;
        ASSERT_TRUE(thimsy::readFile(padd_test::tracesDir() + "/" + n + ".trace", &text));
        ASSERT_TRUE(thimsy::readFile(padd_test::tracesDir() + "/" + n + ".golden", &golden)) << "no golden";
        std::string err;
        std::string got = replay(text, &err);
        EXPECT_EQ(err, "");
        EXPECT_EQ(got, golden);
    }
}

TEST(Replay, LabelsBindBySourceIdWithoutNodeLines) {
    std::string err;
    std::string got = replay(kArc +
                                 "10 gpio-keys-control EV_KEY BTN_EAST 1\n"
                                 "10 gpio-keys-control EV_SYN SYN_REPORT 0\n",
                             &err);
    EXPECT_EQ(err, "");
    EXPECT_NE(got.find("bind gpio-keys-control -> gpio-keys-control\n10 pad EV_KEY BTN_SOUTH 1\n"
                       "10 pad EV_SYN SYN_REPORT 0\n"),
              std::string::npos)
        << got;
}

TEST(Replay, Errors) {
    std::string err;
    replay("10 x EV_KEY BTN_A 1\n", &err);
    EXPECT_EQ(err, "line 1: no device line");
    replay(kArc + "frobnicate\n", &err);
    EXPECT_EQ(err, "line 2: unknown statement 'frobnicate'");
    replay(kArc + "20 intercept 1\n10 intercept 0\n", &err);
    EXPECT_EQ(err, "line 3: time goes backwards");
    replay(kArc + "10 intercept 1\nnode a name=b\n", &err);
    EXPECT_EQ(err, "line 3: 'node' must come before the first timed line");
    replay(kArc + "10 gpio-keys-control EV_KEY BTN_NOPE 1\n", &err);
    EXPECT_EQ(err, "line 2: bad event 'EV_KEY BTN_NOPE 1'");
    replay(kArc + "caps x keys BTN_NOPE\n", &err);
    EXPECT_EQ(err, "line 2: caps: unknown key 'BTN_NOPE'");
    replay(kArc + "node a phys=b\n", &err);
    EXPECT_EQ(err, "line 2: node: name= is needed");
    replay("device ../../../../board/qemu-aarch64-virt/devices/qemu-virt.json\n10 ff play 0 1\n", &err);
    EXPECT_EQ(err, "line 2: ff: this pad has no rumble");
    replay("device nowhere.json\n", &err);
    EXPECT_NE(err.find("nowhere.json: can't read"), std::string::npos) << err;
}

TEST(Replay, QuotedNamesAndComments) {
    std::string err;
    std::string got = replay(
        "# a comment\n"
        "device ../../../../board/qemu-aarch64-virt/devices/qemu-virt.json\n"
        "node kbd name=\"QEMU Virtio Keyboard\"\n"
        "caps kbd keys KEY_ENTER\n"
        "   # an indented comment\n"
        "\n"
        "5 kbd EV_KEY KEY_ENTER 1\n"
        "5 kbd EV_SYN SYN_REPORT 0\n",
        &err);
    EXPECT_EQ(err, "");
    EXPECT_NE(got.find("bind kbd -> virtio-keyboard\n"), std::string::npos) << got;
    EXPECT_NE(got.find("5 pad EV_KEY BTN_SOUTH 1\n"), std::string::npos) << got;
}

TEST(Replay, RumbleRunsOutAfterTheLastLine) {
    std::string err;
    std::string got = replay(kArc + "10 ff upload 2 100 200 40\n10 ff play 2 1\n", &err);
    EXPECT_EQ(err, "");
    EXPECT_NE(got.find("10 vib pwm-vibrator on strong=0x0064 weak=0x00c8 for=40\n50 vib pwm-vibrator off\n"),
              std::string::npos)
        << got;
}
