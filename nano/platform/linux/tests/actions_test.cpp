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

// The Linux system actions (actions/SystemActions.cpp), the program runner (actions/Run.cpp) and
// the wpa_supplicant control client (actions/WpaCtrl.cpp), against a fake wpa_supplicant socket, a
// fake zoneinfo tree and a recording runner.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cutils/properties.h>

#include "actions/Run.h"
#include "actions/SystemActions.h"
#include "actions/WpaCtrl.h"
#include "compat_test_util.h"

using nanolinux::actions::RunResult;

namespace {

std::string prop(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "");
    return v;
}

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

void put(const std::string& path, const std::string& text) {
    mkdirs(path.substr(0, path.rfind('/')));
    FILE* f = fopen(path.c_str(), "w");
    ASSERT_TRUE(f != nullptr) << path;
    fputs(text.c_str(), f);
    fclose(f);
}

std::string readLink(const std::string& path) {
    char buf[512];
    ssize_t n = readlink(path.c_str(), buf, sizeof(buf));
    return n < 0 ? std::string() : std::string(buf, (size_t)n);
}

// wpa_supplicant's side of the control socket: answers each command from `replies` (by its first
// word; "OK\n" otherwise), sending `events` first, and records the commands.
class FakeWpa {
 public:
    explicit FakeWpa(const std::string& path) : path_(path) {
        fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
        ok_ = fd_ >= 0 && bind(fd_, (struct sockaddr*)&a, sizeof(a)) == 0;
        if (ok_) thread_ = std::thread([this] { loop(); });
    }
    ~FakeWpa() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
        unlink(path_.c_str());
    }
    bool ok() const { return ok_; }
    std::vector<std::string> commands() {
        std::lock_guard<std::mutex> l(mu_);
        return commands_;
    }
    void reply(const std::string& firstWord, const std::string& text) {
        std::lock_guard<std::mutex> l(mu_);
        replies_.push_back({firstWord, text});
    }
    void sendEventFirst(const std::string& e) {
        std::lock_guard<std::mutex> l(mu_);
        events_.push_back(e);
    }
    void mute() { mute_ = true; }

 private:
    void loop() {
        while (!stop_) {
            struct pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            char buf[512];
            struct sockaddr_un from;
            socklen_t len = sizeof(from);
            ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0, (struct sockaddr*)&from, &len);
            if (n <= 0) continue;
            std::string cmd(buf, (size_t)n), answer = "OK\n";
            std::vector<std::string> events;
            {
                std::lock_guard<std::mutex> l(mu_);
                commands_.push_back(cmd);
                const std::string word = cmd.substr(0, cmd.find(' '));
                for (const auto& r : replies_)
                    if (r.first == word) answer = r.second;
                events.swap(events_);
            }
            if (mute_) continue;
            for (const std::string& e : events) sendto(fd_, e.data(), e.size(), 0, (struct sockaddr*)&from, len);
            sendto(fd_, answer.data(), answer.size(), 0, (struct sockaddr*)&from, len);
        }
    }

    std::string path_;
    int fd_ = -1;
    bool ok_ = false;
    std::atomic<bool> stop_{false}, mute_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> commands_, events_;
    std::vector<std::pair<std::string, std::string>> replies_;
};

class ActionsTest : public compat_test::StoreTest {
 protected:
    void SetUp() override {
        compat_test::StoreTest::SetUp();
        mkdirs(dir_ / "root/run/wpa_supplicant");
        mkdirs(dir_ / "root/tmp");
        mkdirs(dir_ / "root/storage/.config/thimsy");
        o.root = dir_ / "root";
        put(o.root + o.btScript, "#!/bin/sh\n");  // run through o.run below; it only has to exist
        o.background = false;  // on this thread: the checks below follow each call
        o.run = [this](const std::vector<std::string>& argv, int timeoutMs) {
            runs.push_back(argv);
            timeouts.push_back(timeoutMs);
            return next;
        };
        zones.clear();
        o.followZone = [this](const std::string& z) { zones.push_back(z); };
    }
    std::unique_ptr<nanoplat::SystemActions> make() { return nanolinux::actions::create(o); }

    nanolinux::actions::Options o;
    std::vector<std::vector<std::string>> runs;
    std::vector<int> timeouts;
    std::vector<std::string> zones;
    RunResult next{0, false, ""};
};

}  // namespace

// --- Run.cpp ---------------------------------------------------------------------------------------

TEST(Run, StatusAndOutput) {
    RunResult r = nanolinux::actions::runProgram({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"}, 5000);
    EXPECT_EQ(r.status, 3);
    EXPECT_FALSE(r.timedOut);
    EXPECT_NE(r.out.find("out\n"), std::string::npos);
    EXPECT_NE(r.out.find("err\n"), std::string::npos);
    EXPECT_EQ(nanolinux::actions::runProgram({"true"}, 5000).status, 0);  // found on PATH
}

TEST(Run, NotFoundIs127) {
    EXPECT_EQ(nanolinux::actions::runProgram({"thimsy-no-such-program-xyz"}, 5000).status, 127);
    EXPECT_EQ(nanolinux::actions::runProgram({}, 5000).status, -1);
}

TEST(Run, KilledAtTheDeadline) {
    RunResult r = nanolinux::actions::runProgram({"/bin/sh", "-c", "echo started; exec sleep 5"}, 300);
    EXPECT_TRUE(r.timedOut);
    EXPECT_EQ(r.status, 128 + 9);
    EXPECT_EQ(r.out, "started\n");
}

TEST(Run, ADaemonKeepingThePipeDoesNotHang) {
    // The shell exits at once; its background child keeps the write end of the pipe for 3 s.
    const auto t0 = std::chrono::steady_clock::now();
    RunResult r = nanolinux::actions::runProgram({"/bin/sh", "-c", "sleep 3 & echo done"}, 5000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.out, "done\n");
    EXPECT_LT(ms, 2500);
}

TEST(Run, TheChildGetsNoneOfTheCallersDescriptors) {
    // As Nano's evdev fds: no O_CLOEXEC. A daemon a script leaves running must not hold one (an
    // EVIOCGRAB belongs to the open file and would outlive Nano).
    const int leak = open("/dev/null", O_RDONLY);
    ASSERT_GE(leak, 3);
    const int high = fcntl(leak, F_DUPFD, 200);  // above any the test itself has open
    ASSERT_GE(high, 200);
    const std::string probe = "for n in " + std::to_string(leak) + " " + std::to_string(high) +
                              "; do [ -e /proc/self/fd/$n ] && echo open $n; done; echo checked";
    RunResult r = nanolinux::actions::runProgram({"/bin/sh", "-c", probe}, 5000);
    close(high);
    close(leak);
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.out, "checked\n");
}

TEST(Run, TheChildHasASessionDefaultSignalsAndAnEmptyMask) {
    // Its own session: the session's kill(-nanoPid) must not reach what a script leaves running.
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &old), 0);
    struct sigaction ign, oldAct;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    ASSERT_EQ(sigaction(SIGUSR2, &ign, &oldAct), 0);
    RunResult r = nanolinux::actions::runProgram(
        {"/bin/sh", "-c", "cat /proc/$$/stat; grep -E '^(SigBlk|SigIgn):' /proc/$$/status; echo pid $$"}, 5000);
    sigaction(SIGUSR2, &oldAct, nullptr);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    ASSERT_EQ(r.status, 0) << r.out;
    // /proc/<pid>/stat: "pid (comm) state ppid pgrp session ...".
    const size_t paren = r.out.rfind(')');
    ASSERT_NE(paren, std::string::npos) << r.out;
    long pid = 0, ppid = 0, pgrp = 0, session = 0;
    char state = 0;
    ASSERT_EQ(sscanf(r.out.c_str(), "%ld", &pid), 1) << r.out;
    ASSERT_EQ(sscanf(r.out.c_str() + paren + 1, " %c %ld %ld %ld", &state, &ppid, &pgrp, &session), 4) << r.out;
    EXPECT_EQ(pgrp, pid);
    EXPECT_EQ(session, pid);
    EXPECT_NE((pid_t)session, getsid(0));
    // SIGUSR1 (bit 10) blocked and SIGUSR2 (bit 12) ignored here; neither in the child.
    const size_t blk = r.out.find("SigBlk:");
    const size_t ignAt = r.out.find("SigIgn:");
    ASSERT_NE(blk, std::string::npos) << r.out;
    ASSERT_NE(ignAt, std::string::npos) << r.out;
    const unsigned long long blocked = strtoull(r.out.c_str() + blk + 7, nullptr, 16);
    const unsigned long long ignored = strtoull(r.out.c_str() + ignAt + 7, nullptr, 16);
    EXPECT_EQ(blocked & (1ull << (SIGUSR1 - 1)), 0ull) << r.out;
    EXPECT_EQ(ignored & (1ull << (SIGUSR2 - 1)), 0ull) << r.out;
}

TEST(Run, ARelativeNameIsLookedUpOnPath) {
    EXPECT_EQ(nanolinux::actions::runProgram({"sh", "-c", "exit 4"}, 5000).status, 4);
    compat_test::ScratchDir dir;
    const std::string file = dir / "not-executable";
    put(file, "#!/bin/sh\nexit 0\n");
    chmod(file.c_str(), 0644);
    EXPECT_EQ(nanolinux::actions::runProgram({file}, 5000).status, 127);  // execve fails: as a shell says
}

// --- WpaCtrl.cpp -----------------------------------------------------------------------------------

TEST(WpaCtrl, RequestSkipsEventsAndCleansUp) {
    compat_test::ScratchDir dir;
    FakeWpa wpa(dir / "wlan0");
    ASSERT_TRUE(wpa.ok());
    wpa.reply("PING", "PONG\n");
    wpa.sendEventFirst("<3>CTRL-EVENT-SCAN-RESULTS ");
    nanolinux::wpa::Ctrl c;
    std::string err, reply;
    ASSERT_TRUE(c.open(dir / "wlan0", dir.path(), &err)) << err;
    ASSERT_TRUE(c.request("PING", &reply, 2000, &err)) << err;
    EXPECT_EQ(reply, "PONG\n");
    ASSERT_TRUE(c.request("SAVE_CONFIG", &reply, 2000, &err)) << err;
    EXPECT_TRUE(nanolinux::wpa::isOk(reply));
    EXPECT_EQ(wpa.commands(), (std::vector<std::string>{"PING", "SAVE_CONFIG"}));
    // The client's own socket is in the directory while open, and not listed as a daemon's.
    EXPECT_EQ(nanolinux::wpa::sockets(dir.path()), (std::vector<std::string>{dir / "wlan0"}));
    c.close();
    EXPECT_FALSE(c.isOpen());
    int left = 0;
    if (DIR* d = opendir(dir.path().c_str())) {
        while (struct dirent* e = readdir(d))
            if (strncmp(e->d_name, "thimsy-wpa-", 11) == 0) left++;
        closedir(d);
    }
    EXPECT_EQ(left, 0);
}

TEST(WpaCtrl, TimeoutAndMissingDaemon) {
    compat_test::ScratchDir dir;
    nanolinux::wpa::Ctrl c;
    std::string err, reply;
    EXPECT_FALSE(c.open(dir / "wlan0", dir.path(), &err));
    EXPECT_NE(err.find("connect"), std::string::npos);
    FakeWpa wpa(dir / "wlan0");
    ASSERT_TRUE(wpa.ok());
    wpa.mute();
    ASSERT_TRUE(c.open(dir / "wlan0", dir.path(), &err)) << err;
    EXPECT_FALSE(c.request("PING", &reply, 150, &err));
    EXPECT_NE(err.find("no reply"), std::string::npos);
    EXPECT_FALSE(nanolinux::wpa::isOk("FAIL\n"));
}

TEST(WpaCtrl, SocketsInterfacesFirst) {
    compat_test::ScratchDir dir;
    FakeWpa a(dir / "wlan1"), b(dir / "p2p-dev-wlan0"), c(dir / "wlan0");
    put(dir / "not-a-socket", "x");
    EXPECT_EQ(nanolinux::wpa::sockets(dir.path()),
              (std::vector<std::string>{dir / "wlan0", dir / "wlan1", dir / "p2p-dev-wlan0"}));
}

// --- SystemActions.cpp -----------------------------------------------------------------------------

TEST_F(ActionsTest, WifiOnAndOffRunD30net) {
    auto a = make();
    a->setWifiEnabled(true);
    EXPECT_EQ(prop("persist.thimsy.wifi.enabled"), "1");
    a->setWifiEnabled(false);
    EXPECT_EQ(prop("persist.thimsy.wifi.enabled"), "0");
    const std::string script = o.root + "/etc/init.d/deferred/D30net";
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0], (std::vector<std::string>{"/bin/sh", script, "start"}));
    EXPECT_EQ(runs[1], (std::vector<std::string>{"/bin/sh", script, "stop"}));
    EXPECT_GE(timeouts[0], 10000);
}

TEST_F(ActionsTest, ForgetRemovesAndSaves) {
    FakeWpa wpa(o.root + "/run/wpa_supplicant/wlan0");
    ASSERT_TRUE(wpa.ok());
    auto a = make();
    a->forgetWifiNetwork(4);
    a->forgetWifiNetwork(-1);  // no id: nothing sent
    EXPECT_EQ(wpa.commands(), (std::vector<std::string>{"REMOVE_NETWORK 4", "SAVE_CONFIG"}));
    EXPECT_TRUE(runs.empty());
}

TEST_F(ActionsTest, ForgetStopsAtAFailedRemove) {
    FakeWpa wpa(o.root + "/run/wpa_supplicant/wlan0");
    ASSERT_TRUE(wpa.ok());
    wpa.reply("REMOVE_NETWORK", "FAIL\n");
    make()->forgetWifiNetwork(9);
    EXPECT_EQ(wpa.commands(), (std::vector<std::string>{"REMOVE_NETWORK 9"}));
}

TEST_F(ActionsTest, ForgetWithoutWpaSupplicant) {
    make()->forgetWifiNetwork(1);  // logs; nothing to talk to
    EXPECT_TRUE(runs.empty());
}

TEST_F(ActionsTest, BluetoothThroughD35AndBluetoothctl) {
    auto a = make();
    const std::string script = o.root + "/etc/init.d/deferred/D35bluetooth";
    a->setBluetoothEnabled(true);
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "1");
    a->setBluetoothEnabled(false);
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "0");
    a->connectBluetooth("aa:BB:cc:00:11:22");
    a->connectBluetooth("aa:BB:cc:00:11");          // not an address
    a->connectBluetooth("aa:BB:cc:00:11:22; reboot");
    next.status = 1;                                // D35bluetooth could not start bluetoothd
    a->setBluetoothEnabled(true);
    ASSERT_EQ(runs.size(), 6u);
    EXPECT_EQ(runs[0], (std::vector<std::string>{"/bin/sh", script, "start"}));  // the daemons first
    EXPECT_EQ(runs[1], (std::vector<std::string>{"bluetoothctl", "power", "on"}));
    EXPECT_EQ(runs[2], (std::vector<std::string>{"bluetoothctl", "power", "off"}));  // the radio first
    EXPECT_EQ(runs[3], (std::vector<std::string>{"/bin/sh", script, "stop"}));
    EXPECT_EQ(runs[4], (std::vector<std::string>{"bluetoothctl", "connect", "aa:BB:cc:00:11:22"}));
    EXPECT_EQ(runs[5], (std::vector<std::string>{"/bin/sh", script, "start"}));  // failed: no bluetoothctl
    EXPECT_GE(timeouts[0], 10000);
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "1");  // status 1: it may work next time
}

TEST_F(ActionsTest, PowerOnIsTriedAgainWhileBluetoothdRegistersTheController) {
    std::vector<RunResult> answers;  // in order; then status 0
    std::vector<int> pauses;
    o.pause = [&pauses](int ms) { pauses.push_back(ms); };
    o.run = [this, &answers](const std::vector<std::string>& argv, int) {
        runs.push_back(argv);
        if (answers.empty()) return RunResult{0, false, ""};
        RunResult r = answers.front();
        answers.erase(answers.begin());
        return r;
    };
    auto a = make();
    const std::vector<std::string> on = {"bluetoothctl", "power", "on"};

    // "No default controller available" twice, then the radio comes on.
    answers = {{0, false, ""}, {1, false, ""}, {1, false, ""}};
    a->setBluetoothEnabled(true);
    ASSERT_EQ(runs.size(), 4u);
    EXPECT_EQ(runs[1], on);
    EXPECT_EQ(runs[3], on);
    EXPECT_EQ(pauses, (std::vector<int>{500, 500}));

    // Five tries at most.
    runs.clear();
    pauses.clear();
    answers = {{0, false, ""}, {1, false, ""}, {1, false, ""}, {1, false, ""}, {1, false, ""}, {1, false, ""}};
    a->setBluetoothEnabled(true);
    EXPECT_EQ(runs.size(), 6u);  // the script, then five
    EXPECT_EQ(pauses.size(), 4u);

    // Not on this image, or a timeout: no second try.
    for (RunResult end : {RunResult{127, false, ""}, RunResult{137, true, ""}}) {
        runs.clear();
        pauses.clear();
        answers = {{0, false, ""}, end};
        a->setBluetoothEnabled(true);
        EXPECT_EQ(runs.size(), 2u);
        EXPECT_TRUE(pauses.empty());
    }
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "1");
}

TEST_F(ActionsTest, ADeviceWithoutBluetoothTurnsTheSwitchBackOff) {
    next.status = 3;  // D35bluetooth: device.json, the kernel or the controller says there is none
    make()->setBluetoothEnabled(true);
    ASSERT_EQ(runs.size(), 1u);  // no bluetoothctl
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "0");
}

TEST_F(ActionsTest, AnImageWithoutD35bluetoothTurnsTheSwitchBackOff) {
    ASSERT_EQ(unlink((o.root + o.btScript).c_str()), 0);
    auto a = make();
    a->setBluetoothEnabled(true);
    EXPECT_TRUE(runs.empty());  // no script to run, and no bluetoothctl
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "0");
    a->setBluetoothEnabled(false);  // the radio off; no script to stop
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0], (std::vector<std::string>{"bluetoothctl", "power", "off"}));
    runs.clear();
    nanolinux::actions::switchBluetooth(true, o);
    EXPECT_TRUE(runs.empty());
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "0");
}

// switchBluetooth (Network::setBtRadio's) is the same switch on the caller's thread, background or not.
TEST_F(ActionsTest, SwitchBluetoothIsTheSameSwitchOnTheCallersThread) {
    const std::string script = o.root + "/etc/init.d/deferred/D35bluetooth";
    o.background = true;
    nanolinux::actions::switchBluetooth(true, o);
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "1");
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0], (std::vector<std::string>{"/bin/sh", script, "start"}));
    EXPECT_EQ(runs[1], (std::vector<std::string>{"bluetoothctl", "power", "on"}));
    nanolinux::actions::switchBluetooth(false, o);
    EXPECT_EQ(prop("persist.thimsy.bluetooth.enabled"), "0");
    ASSERT_EQ(runs.size(), 4u);
    EXPECT_EQ(runs[2], (std::vector<std::string>{"bluetoothctl", "power", "off"}));
    EXPECT_EQ(runs[3], (std::vector<std::string>{"/bin/sh", script, "stop"}));
}

TEST_F(ActionsTest, SlowActionsRunOnAWorkerInOrder) {
    std::mutex mu;
    std::vector<std::string> seen;
    std::atomic<bool> release{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> onCaller{false};
    o.background = true;
    o.run = [&](const std::vector<std::string>& argv, int) {
        if (std::this_thread::get_id() == caller) onCaller = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard<std::mutex> l(mu);
        seen.push_back(argv.back());
        return RunResult{0, false, ""};
    };
    {
        auto a = make();
        a->setWifiEnabled(true);    // the worker blocks in the first run until released
        a->setBluetoothEnabled(false);
        a->connectBluetooth("00:11:22:33:44:55");
        a->setWifiEnabled(false);
        // The calls returned at once; the setting is already written.
        EXPECT_EQ(prop("persist.thimsy.wifi.enabled"), "0");
        {
            std::lock_guard<std::mutex> l(mu);
            EXPECT_TRUE(seen.empty());
        }
        release = true;
    }  // destroying the actions runs what is queued, then stops the worker
    EXPECT_FALSE(onCaller);
    EXPECT_EQ(seen, (std::vector<std::string>{"start", "off", "stop", "00:11:22:33:44:55", "stop"}));
}

TEST_F(ActionsTest, TimeFormatIsTheStoresOnly) {
    make()->setTimeFormat(true);
    EXPECT_TRUE(runs.empty());
}

TEST_F(ActionsTest, TimeZoneRewritesTheLink) {
    put(o.root + "/usr/share/zoneinfo/Europe/London", "TZif");
    put(o.root + "/usr/share/zoneinfo/Etc/GMT+5", "TZif");
    const std::string link = o.root + "/storage/.config/thimsy/localtime";
    ASSERT_EQ(symlink("/usr/share/zoneinfo/Etc/UTC", link.c_str()), 0);
    auto a = make();
    a->setTimeZone("Europe/London");
    EXPECT_EQ(readLink(link), "/usr/share/zoneinfo/Europe/London");
    EXPECT_EQ(prop("persist.sys.timezone"), "Europe/London");
    a->setTimeZone("Etc/GMT+5");
    EXPECT_EQ(readLink(link), "/usr/share/zoneinfo/Etc/GMT+5");
    EXPECT_EQ(zones, (std::vector<std::string>{"Europe/London", "Etc/GMT+5"}));
    struct stat st;
    EXPECT_NE(lstat((link + ".new").c_str(), &st), 0);
}

TEST_F(ActionsTest, TimeZoneRefusesUnknownAndBadIds) {
    put(o.root + "/usr/share/zoneinfo/Europe/London", "TZif");
    mkdirs(o.root + "/usr/share/zoneinfo/Europe/Paris");   // a directory, not a zone
    const std::string link = o.root + "/storage/.config/thimsy/localtime";
    ASSERT_EQ(symlink("/usr/share/zoneinfo/Etc/UTC", link.c_str()), 0);
    auto a = make();
    for (const char* bad : {"Mars/Olympus", "Europe/Paris", "../etc/passwd", "Europe/../London", "/Europe/London",
                            "Europe//London", "Europe/London ", "", "Europe/London;reboot"}) {
        a->setTimeZone(bad);
        EXPECT_EQ(readLink(link), "/usr/share/zoneinfo/Etc/UTC") << bad;
    }
    EXPECT_EQ(prop("persist.sys.timezone"), "");
    EXPECT_TRUE(zones.empty());
    EXPECT_TRUE(nanolinux::actions::validZoneId("America/Argentina/Buenos_Aires"));
    EXPECT_TRUE(nanolinux::actions::validZoneId("Etc/GMT-14"));
    EXPECT_FALSE(nanolinux::actions::validZoneId(std::string(65, 'a')));
}
