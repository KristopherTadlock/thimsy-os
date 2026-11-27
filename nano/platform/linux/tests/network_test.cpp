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

// The Linux Wi-Fi and Bluetooth (actions/Network.cpp, G7-net): Wi-Fi against a fake wpa_supplicant
// control socket (a Unix datagram server answering from the test's script), Bluetooth against
// canned bluetoothctl output through the injectable runner, the addresses from a fake /proc and
// /etc under the test's root.

#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cutils/properties.h>
#include <gtest/gtest.h>

#include "actions/Network.h"
#include "actions/Run.h"
#include "compat_test_util.h"

using nanolinux::actions::RunResult;
using nanoplat::Network;

namespace {

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

// wpa_supplicant's side of the control socket: each command is answered by `answer` ("OK\n" when it
// returns ""), and recorded.
class FakeWpaD {
 public:
    using Answer = std::function<std::string(const std::string& cmd)>;
    FakeWpaD(const std::string& path, Answer answer) : path_(path), answer_(std::move(answer)) {
        fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
        ok_ = fd_ >= 0 && path.size() < sizeof(a.sun_path) && bind(fd_, (struct sockaddr*)&a, sizeof(a)) == 0;
        if (ok_) thread_ = std::thread([this] { loop(); });
    }
    ~FakeWpaD() {
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
    int count(const std::string& prefix) {
        int n = 0;
        for (const std::string& c : commands())
            if (c.compare(0, prefix.size(), prefix) == 0) n++;
        return n;
    }

 private:
    void loop() {
        while (!stop_) {
            struct pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            char buf[4096];
            struct sockaddr_un from;
            socklen_t len = sizeof(from);
            ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0, (struct sockaddr*)&from, &len);
            if (n <= 0) continue;
            const std::string cmd(buf, (size_t)n);
            {
                std::lock_guard<std::mutex> l(mu_);
                commands_.push_back(cmd);
            }
            std::string reply = answer_(cmd);
            if (reply.empty()) reply = "OK\n";
            sendto(fd_, reply.data(), reply.size(), 0, (struct sockaddr*)&from, len);
        }
    }

    std::string path_;
    Answer answer_;
    int fd_ = -1;
    bool ok_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> commands_;
};

const char kListHeader[] = "network id / ssid / bssid / flags\n";
const char kScanHeader[] = "bssid / frequency / signal level / flags / ssid\n";

class NetworkTest : public compat_test::StoreTest {
 protected:
    void SetUp() override {
        compat_test::StoreTest::SetUp();   // setBtRadio writes persist.thimsy.bluetooth.enabled
        root_ = dir_ / "root";
        mkdirs(root_ + "/run/wpa_supplicant");
        mkdirs(root_ + "/tmp");
        o.root = root_;
        o.wpaReplyMs = 1000;
        o.sleep = [this](int ms) { slept += ms; };
        o.nowMs = [this] { return now; };
        // Bluetooth is on: the device has it, the switch is on, D35bluetooth's bus runs.
        props["ro.thimsy.device.bluetooth"] = "true";
        props["persist.thimsy.bluetooth.enabled"] = "1";
        put(root_ + "/run/dbus/system_bus_socket", "");
        put(root_ + "/etc/init.d/deferred/D35bluetooth", "#!/bin/sh\n");   // run through o.run below
        o.prop = [this](const char* key) { return props[key]; };
        o.run = [this](const std::vector<std::string>& argv, int timeoutMs) {
            std::string key;
            for (size_t i = 1; i < argv.size(); i++) key += (i > 1 ? " " : "") + argv[i];
            runs.push_back(key);
            timeouts[key] = timeoutMs;
            if (!argv.empty() && argv[0] == "/bin/sh") return script;   // D35bluetooth
            if (argv.empty() || argv[0] != "bluetoothctl") return RunResult{127, false, ""};
            auto it = bt.find(key);
            if (it != bt.end()) return it->second;
            return btDefault;
        };
    }
    std::string sock() const { return root_ + "/run/wpa_supplicant/wlan0"; }
    std::unique_ptr<Network> make() { return nanolinux::net::create(o); }
    int ran(const std::string& key) const {
        int n = 0;
        for (const std::string& r : runs) n += r == key;
        return n;
    }

    std::string root_;
    nanolinux::net::Options o;
    int slept = 0;
    long long now = 1000;
    std::vector<std::string> runs;
    std::map<std::string, int> timeouts;
    std::map<std::string, RunResult> bt;
    RunResult btDefault{0, false, ""};
    RunResult script{0, false, ""};
    std::map<std::string, std::string> props;
};

RunResult out(const std::string& text, int status = 0) { return RunResult{status, false, text}; }

}  // namespace

// --- the helpers -------------------------------------------------------------------------------------

TEST(NetworkText, DecodesWpaSupplicantsEscapes) {
    using nanolinux::net::decodeWpaText;
    EXPECT_EQ(decodeWpaText("Home"), "Home");
    EXPECT_EQ(decodeWpaText("Caf\\xc3\\xa9 \\\"5G\\\""), "Caf\xc3\xa9 \"5G\"");
    EXPECT_EQ(decodeWpaText("a\\\\b\\tc\\nd\\e"), std::string("a\\b\tc\nd\x1b"));
    EXPECT_EQ(decodeWpaText("\\101\\0"), std::string("A\0", 2));
    EXPECT_EQ(decodeWpaText("x\\"), "x\\");        // a trailing backslash stays
    EXPECT_EQ(decodeWpaText("\\xZZ"), "\\xZZ");    // not hex: as it was
    EXPECT_EQ(nanolinux::net::hexOf("ab\x01\xff"), "616201ff");
}

TEST(NetworkText, SecurityAndBars) {
    using namespace nanolinux::net;
    EXPECT_EQ(securityOfFlags("[WPA2-PSK-CCMP][ESS]"), 2);
    EXPECT_EQ(securityOfFlags("[WPA-PSK-TKIP][WPA2-PSK+SAE-CCMP][ESS]"), 3);
    EXPECT_EQ(securityOfFlags("[RSN-OWE-CCMP][ESS]"), 4);
    EXPECT_EQ(securityOfFlags("[WEP][ESS]"), 1);
    EXPECT_EQ(securityOfFlags("[ESS]"), 0);
    EXPECT_EQ(securityOfKeyMgmt("WPA-PSK", false), 2);
    EXPECT_EQ(securityOfKeyMgmt("WPA-PSK SAE", false), 3);
    EXPECT_EQ(securityOfKeyMgmt("NONE", true), 1);
    EXPECT_EQ(securityOfKeyMgmt("NONE", false), 0);
    EXPECT_EQ(securityOfKeyMgmt("OWE", false), 4);
    EXPECT_EQ(barsOfRssi(-50), 4);
    EXPECT_EQ(barsOfRssi(-60), 3);
    EXPECT_EQ(barsOfRssi(-70), 2);
    EXPECT_EQ(barsOfRssi(-80), 1);
    EXPECT_EQ(barsOfRssi(-95), 0);
}

// --- Wi-Fi -----------------------------------------------------------------------------------------

TEST_F(NetworkTest, NoWpaSupplicantIsOff) {
    btDefault = out("", 127);   // no BlueZ either
    auto n = make();
    Network::WifiLevel w = Network::kWifiLevel_Unknown;
    Network::BtLevel b = Network::kBtLevel_Unknown;
    int bars = 0, btCount = 0;
    std::string ssid;
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Off);
    EXPECT_EQ(b, Network::kBtLevel_Unknown);
    EXPECT_FALSE(n->wifiRadioEnabled());
    EXPECT_FALSE(n->wifiListRadio().on);
    EXPECT_TRUE(n->wifiLists().saved.empty());
    EXPECT_FALSE(n->connectSavedWifi(0));
    n->joinWifi("Home", 2, "password1", false);   // logged; nothing to talk to
    EXPECT_EQ(n->checkWifiJoin("Home"), Network::JoinCheck::Pending);
    EXPECT_EQ(n->wifiAddresses(true).ip, "");
    n->autoEnableWifi();
    EXPECT_EQ(slept, 0);
    // bluetoothctl is missing: not run again for 30 s, then tried again.
    EXPECT_EQ(ran("show"), 1);
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_EQ(ran("show"), 1);
    now += 31000;
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_EQ(ran("show"), 2);
}

TEST_F(NetworkTest, PollReadsStatusAndSignal) {
    std::mutex stateMu;
    std::string state = "COMPLETED";
    auto setState = [&](const char* s) {
        std::lock_guard<std::mutex> l(stateMu);
        state = s;
    };
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        std::lock_guard<std::mutex> l(stateMu);
        if (c == "STATUS")
            return "bssid=aa:bb:cc:00:00:01\nfreq=2412\nssid=Caf\\xc3\\xa9 \\\"5G\\\"\nid=1\nmode=station\n"
                   "wpa_state=" + state + "\nip_address=192.168.1.20\naddress=02:11:22:33:44:55\n";
        if (c == "SIGNAL_POLL") return "RSSI=-60\nLINKSPEED=72\nNOISE=9999\nFREQUENCY=2412\n";
        return "FAIL\n";
    });
    ASSERT_TRUE(wpa.ok());
    bt["show"] = out("Controller 00:1A:7D:DA:71:13 (public)\n\tName: thimsy\n\tPowered: yes\n");
    bt["devices Paired"] = out("Device 11:22:33:44:55:66 Pad\nDevice 11:22:33:44:55:77 Phones\n");
    bt["info 11:22:33:44:55:66"] = out("Device 11:22:33:44:55:66 (public)\n\tName: Pad\n\tPaired: yes\n\tConnected: yes\n");
    bt["info 11:22:33:44:55:77"] = out("Device 11:22:33:44:55:77 (public)\n\tName: Phones\n\tPaired: yes\n\tConnected: no\n");
    auto n = make();
    Network::WifiLevel w = Network::kWifiLevel_Unknown;
    Network::BtLevel b = Network::kBtLevel_Unknown;
    int bars = 0, btCount = 0;
    std::string ssid;
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Connected);
    EXPECT_EQ(bars, 3);
    EXPECT_EQ(ssid, "Caf\xc3\xa9 \"5G\"");
    EXPECT_EQ(b, Network::kBtLevel_Connected);
    EXPECT_EQ(btCount, 1);

    setState("SCANNING");
    w = Network::kWifiLevel_Unknown;
    b = Network::kBtLevel_Unknown;
    bars = 0;
    btCount = 0;
    ssid.clear();
    bt["show"] = out("Controller 00:1A:7D:DA:71:13 (public)\n\tPowered: no\n");
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Disconnected);
    EXPECT_EQ(ssid, "");
    EXPECT_EQ(b, Network::kBtLevel_Off);
    EXPECT_TRUE(n->wifiRadioEnabled());

    setState("INTERFACE_DISABLED");
    w = Network::kWifiLevel_Unknown;
    bt["show"] = out("No default controller available\n");
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Off);
    EXPECT_EQ(b, Network::kBtLevel_Off);
    EXPECT_FALSE(n->wifiRadioEnabled());
    EXPECT_FALSE(n->wifiListRadio().on);
}

TEST_F(NetworkTest, ListsSavedAndScanned) {
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "STATUS") return "wpa_state=COMPLETED\nssid=Home\nid=0\n";
        if (c == "LIST_NETWORKS")
            return std::string(kListHeader) + "0\tHome\tany\t[CURRENT]\n1\tCafe\\x20Bar\tany\t[DISABLED]\n2\tOld\tany\t\n";
        if (c == "GET_NETWORK 0 key_mgmt") return "WPA-PSK";
        if (c == "GET_NETWORK 1 key_mgmt") return "NONE";
        if (c == "GET_NETWORK 1 wep_key0") return "FAIL\n";
        if (c == "GET_NETWORK 2 key_mgmt") return "NONE";
        if (c == "GET_NETWORK 2 wep_key0") return "*";
        if (c == "SCAN_RESULTS")
            return std::string(kScanHeader) + "aa:bb:cc:00:00:01\t2412\t-50\t[WPA2-PSK-CCMP][ESS]\tHome\n"
                   "aa:bb:cc:00:00:02\t5180\t-70\t[RSN-SAE-CCMP][ESS]\tOffice 5G\n"
                   "aa:bb:cc:00:00:03\t2437\t-40\t[ESS]\t\n"
                   "aa:bb:cc:00:00:04\t2437\t-41\t[ESS]\t\\x00\\x00\n"
                   "aa:bb:cc:00:00:05\t2462\t0\t[ESS]\tOpen\tcafe\n";
        return "FAIL\n";
    });
    ASSERT_TRUE(wpa.ok());
    auto n = make();
    const Network::WifiRadio r = n->wifiListRadio();
    EXPECT_TRUE(r.on);
    EXPECT_EQ(r.connectedSsid, "Home");
    const Network::WifiLists l = n->wifiLists();
    ASSERT_EQ(l.saved.size(), 3u);
    EXPECT_EQ(l.saved[0].ssid, "Home");
    EXPECT_EQ(l.saved[0].savedNetId, 0);
    EXPECT_EQ(l.saved[0].security, 2);
    EXPECT_EQ(l.saved[0].rssi, -127);
    EXPECT_EQ(l.saved[1].ssid, "Cafe Bar");
    EXPECT_EQ(l.saved[1].security, 0);
    EXPECT_EQ(l.saved[2].security, 1);   // a WEP key
    ASSERT_EQ(l.scanned.size(), 3u);      // the hidden networks are left out
    EXPECT_EQ(l.scanned[0].ssid, "Home");
    EXPECT_EQ(l.scanned[0].bssid, "aa:bb:cc:00:00:01");
    EXPECT_EQ(l.scanned[0].rssi, -50);
    EXPECT_EQ(l.scanned[0].security, 2);
    EXPECT_EQ(l.scanned[0].savedNetId, -1);
    EXPECT_EQ(l.scanned[1].ssid, "Office 5G");
    EXPECT_EQ(l.scanned[1].security, 3);
    EXPECT_EQ(l.scanned[2].ssid, "Open");
    EXPECT_EQ(l.scanned[2].rssi, -127);   // no level
    n->startWifiScan();
    EXPECT_EQ(wpa.count("SCAN"), 2);       // SCAN_RESULTS and SCAN
    EXPECT_EQ(wpa.commands().back(), "SCAN");
}

TEST_F(NetworkTest, ConnectsASavedNetwork) {
    std::atomic<bool> selected{false};
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "SELECT_NETWORK 2") { selected = true; return ""; }
        if (c == "SELECT_NETWORK 9") return "FAIL\n";
        if (c == "STATUS") return selected ? "wpa_state=COMPLETED\nssid=Cafe\nid=2\n" : "wpa_state=SCANNING\n";
        return "";
    });
    ASSERT_TRUE(wpa.ok());
    auto n = make();
    EXPECT_TRUE(n->connectSavedWifi(2));
    EXPECT_EQ(wpa.count("ENABLE_NETWORK all"), 1);   // the others, which SELECT_NETWORK disabled
    EXPECT_EQ(wpa.count("SAVE_CONFIG"), 0);
    EXPECT_FALSE(n->connectSavedWifi(9));
    EXPECT_FALSE(n->connectSavedWifi(-1));
    EXPECT_LE(slept, 25000);
}

TEST_F(NetworkTest, JoinsANewNetwork) {
    std::atomic<bool> selected{false};
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "LIST_NETWORKS") return std::string(kListHeader) + "0\tHome\tany\t\n";
        if (c == "ADD_NETWORK") return "3\n";
        if (c == "SELECT_NETWORK 3") { selected = true; return ""; }
        if (c == "STATUS") return selected ? "wpa_state=COMPLETED\nssid=Caf\\xc3\\xa9\nid=3\n" : "wpa_state=SCANNING\n";
        return "";
    });
    ASSERT_TRUE(wpa.ok());
    auto n = make();
    n->joinWifi("Caf\xc3\xa9", 2, "pass \"word\"", false);
    std::vector<std::string> sets;
    for (const std::string& c : wpa.commands())
        if (c.compare(0, 12, "SET_NETWORK ") == 0) sets.push_back(c);
    EXPECT_EQ(sets, (std::vector<std::string>{"SET_NETWORK 3 ssid 436166c3a9", "SET_NETWORK 3 key_mgmt WPA-PSK",
                                               "SET_NETWORK 3 psk \"pass \"word\"\""}));
    EXPECT_EQ(wpa.count("SELECT_NETWORK 3"), 1);
    const std::vector<std::string> c = wpa.commands();
    ASSERT_GE(c.size(), 2u);
    EXPECT_EQ(c[c.size() - 2], "ENABLE_NETWORK all");
    EXPECT_EQ(c.back(), "SAVE_CONFIG");
    EXPECT_EQ(n->checkWifiJoin("Caf\xc3\xa9"), Network::JoinCheck::Connected);
}

TEST_F(NetworkTest, JoinSecurities) {
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "LIST_NETWORKS") return std::string(kListHeader) + "4\tLab\tany\t\n";
        if (c == "STATUS") return "wpa_state=COMPLETED\nssid=Lab\nid=4\n";
        return "";
    });
    ASSERT_TRUE(wpa.ok());
    auto n = make();
    auto setsOf = [&](int security, const std::string& pw) {
        const size_t before = wpa.commands().size();
        n->joinWifi("Lab", security, pw, true);   // force: even while connected to it
        std::vector<std::string> s;
        const std::vector<std::string> all = wpa.commands();
        for (size_t i = before; i < all.size(); i++)
            if (all[i].compare(0, 12, "SET_NETWORK ") == 0) s.push_back(all[i].substr(14));
        return s;
    };
    EXPECT_EQ(setsOf(3, "correct horse"), (std::vector<std::string>{"ssid 4c6162", "key_mgmt SAE WPA-PSK", "ieee80211w 1",
                                                                     "sae_password \"correct horse\"",
                                                                     "psk \"correct horse\""}));
    EXPECT_EQ(setsOf(1, "0123456789"), (std::vector<std::string>{"ssid 4c6162", "key_mgmt NONE", "wep_key0 0123456789",
                                                                  "wep_tx_keyidx 0", "auth_alg OPEN SHARED"}));
    EXPECT_EQ(setsOf(4, "ignored"), (std::vector<std::string>{"ssid 4c6162", "key_mgmt OWE", "ieee80211w 2"}));
    EXPECT_EQ(setsOf(0, ""), (std::vector<std::string>{"ssid 4c6162", "key_mgmt NONE"}));
    EXPECT_EQ(wpa.count("ADD_NETWORK"), 0);   // the saved one is reused
    // Not forced and already connected to it: nothing is changed.
    const size_t before = wpa.commands().size();
    n->joinWifi("Lab", 2, "password1", false);
    EXPECT_EQ(wpa.commands().size(), before + 1);   // STATUS
}

TEST_F(NetworkTest, ARefusedKeyRemovesTheNewNetwork) {
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "LIST_NETWORKS") return kListHeader;
        if (c == "ADD_NETWORK") return "5\n";
        if (c.compare(0, 17, "SET_NETWORK 5 psk") == 0) return "FAIL\n";   // under 8 characters
        if (c == "STATUS") return "wpa_state=DISCONNECTED\n";
        return "";
    });
    ASSERT_TRUE(wpa.ok());
    make()->joinWifi("Short", 2, "abc", false);
    EXPECT_EQ(wpa.count("REMOVE_NETWORK 5"), 1);
    EXPECT_EQ(wpa.count("SELECT_NETWORK"), 0);
    EXPECT_EQ(wpa.count("SAVE_CONFIG"), 0);
}

TEST_F(NetworkTest, WrongPasswordIsNotRetried) {
    std::atomic<bool> tempDisabled{false};
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "LIST_NETWORKS")
            return std::string(kListHeader) + "0\tHome\tany\t" + (tempDisabled ? "[TEMP-DISABLED]" : "") + "\n";
        if (c == "STATUS") return "wpa_state=SCANNING\n";
        return "";
    });
    ASSERT_TRUE(wpa.ok());
    auto n = make();
    EXPECT_EQ(n->checkWifiJoin("Home"), Network::JoinCheck::Pending);
    // No association and no refusal: one retry, both waits bounded.
    n->joinWifi("Home", 2, "password1", false);
    EXPECT_EQ(wpa.count("SELECT_NETWORK 0"), 2);
    EXPECT_EQ(slept, 22000);
    // The key was refused: no retry.
    tempDisabled = true;
    slept = 0;
    n->joinWifi("Home", 2, "password2", false);
    EXPECT_EQ(wpa.count("SELECT_NETWORK 0"), 3);
    EXPECT_EQ(slept, 12000);
    EXPECT_EQ(n->checkWifiJoin("Home"), Network::JoinCheck::WrongPassword);
    EXPECT_EQ(n->checkWifiJoin("Other"), Network::JoinCheck::Pending);
}

TEST_F(NetworkTest, AddressesRouteAndDns) {
    std::atomic<bool> withAddress{true};
    FakeWpaD wpa(sock(), [&](const std::string& c) -> std::string {
        if (c == "STATUS")
            return std::string("wpa_state=COMPLETED\nssid=Home\nip_address=192.168.1.20\n") +
                   (withAddress ? "address=02:11:22:33:44:55\n" : "");
        return "FAIL\n";
    });
    ASSERT_TRUE(wpa.ok());
    put(root_ + "/proc/net/route",
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t00000000\t0102000A\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
        "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
        "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n");
    put(root_ + "/etc/resolv.conf", "# udhcpc\nsearch lan\nnameserver 192.168.1.1\nnameserver  9.9.9.9 \nnameserver 1.1.1.1\n");
    put(root_ + "/sys/class/net/wlan0/address", "02:aa:bb:cc:dd:ee\n");
    auto n = make();
    Network::WifiAddresses a = n->wifiAddresses(true);
    EXPECT_EQ(a.ip, "192.168.1.20");
    EXPECT_EQ(a.gw, "192.168.1.1");
    EXPECT_EQ(a.dns, "192.168.1.1");
    EXPECT_EQ(a.dns2, "9.9.9.9");
    EXPECT_EQ(a.mac, "02:11:22:33:44:55");
    a = n->wifiAddresses(false);
    EXPECT_EQ(a.gw, "");
    EXPECT_EQ(a.dns, "");
    withAddress = false;   // STATUS without it: the interface's sysfs address
    const Network::WifiMacAndIp m = n->wifiMacAndIp();
    EXPECT_EQ(m.mac, "02:aa:bb:cc:dd:ee");
    EXPECT_EQ(m.ip, "192.168.1.20");
    Network::WifiLink l = n->wifiLink();
    EXPECT_EQ(l.ip, "192.168.1.20");
    EXPECT_TRUE(l.validated);
    put(root_ + "/etc/resolv.conf", "search lan\n");   // no nameserver: not validated
    EXPECT_FALSE(n->wifiLink().validated);
}

// --- Bluetooth -------------------------------------------------------------------------------------

TEST_F(NetworkTest, BondedDevicesAreCheckedByInfo) {
    bt["show"] = out("\x1b[0;94m[bluetooth]\x1b[0m# Controller 00:1A:7D:DA:71:13 (public)\r\n\tPowered: yes\r\n");
    // An older bluetoothctl: no filter on devices, and paired-devices lists them all.
    bt["devices Paired"] = out("Too many arguments: 1 > 0\n", 1);
    bt["paired-devices"] = out("Device 11:22:33:44:55:66 Pad\nDevice 11:22:33:44:55:77 Speaker\n");
    bt["info 11:22:33:44:55:66"] = out("Device 11:22:33:44:55:66 (public)\n\tName: Pad Pro\n\tClass: 0x00002508\n"
                                       "\tPaired: yes\n\tConnected: yes\n");
    bt["info 11:22:33:44:55:77"] = out("Device 11:22:33:44:55:77 (public)\n\tAlias: Speaker\n\tPaired: no\n");
    auto n = make();
    const Network::BtList l = n->btListState();
    EXPECT_TRUE(l.on);
    ASSERT_EQ(l.devices.size(), 1u);
    EXPECT_EQ(l.devices[0].address, "11:22:33:44:55:66");
    EXPECT_EQ(l.devices[0].name, "Pad Pro");
    EXPECT_EQ(l.devices[0].cod, 0x2508);
    EXPECT_TRUE(l.devices[0].bonded);
    EXPECT_TRUE(l.devices[0].connected);
    EXPECT_EQ(n->bondedBt().size(), 1u);
    EXPECT_TRUE(n->btRadioOn());
    bt["show"] = out("Controller 00:1A:7D:DA:71:13 (public)\n\tPowered: no\n");
    EXPECT_FALSE(n->btListState().on);
    EXPECT_TRUE(n->btListState().devices.empty());
}

TEST_F(NetworkTest, InquiryListsWhatWasFound) {
    bt["devices"] = out("Device 11:22:33:44:55:66 Pad\nDevice AA:BB:CC:DD:EE:01 AA-BB-CC-DD-EE-01\nnot a device\n");
    bt["info 11:22:33:44:55:66"] = out("\tName: Pad\n\tClass: 0x00002508\n\tPaired: yes\n\tConnected: yes\n");
    bt["info AA:BB:CC:DD:EE:01"] = out("Device AA:BB:CC:DD:EE:01 not available\n", 1);
    auto n = make();
    const std::vector<nanoplat::BtDevEntry> d = n->discoverBt();
    EXPECT_EQ(ran("--timeout 8 scan on"), 1);
    EXPECT_EQ(timeouts["--timeout 8 scan on"], 13000);
    ASSERT_EQ(d.size(), 2u);
    EXPECT_TRUE(d[0].bonded);
    EXPECT_FALSE(d[0].connected);   // as an inquiry reports it
    EXPECT_EQ(d[1].name, "AA-BB-CC-DD-EE-01");
    EXPECT_FALSE(d[1].bonded);
    (void)n->btWizScan();
    EXPECT_EQ(ran("--timeout 15 scan on"), 1);
}

TEST_F(NetworkTest, PairTrustsAndConnects) {
    const std::string mac = "11:22:33:44:55:66";
    bt["info " + mac] = out("\tName: Pad\n\tPaired: yes\n");
    auto n = make();
    EXPECT_TRUE(n->pairBt(mac));
    EXPECT_EQ(runs, (std::vector<std::string>{"info " + mac, "--agent NoInputNoOutput pair " + mac, "info " + mac,
                                              "trust " + mac, "connect " + mac}));
    EXPECT_GE(timeouts["--agent NoInputNoOutput pair " + mac], 30000);
    runs.clear();
    EXPECT_TRUE(n->btWizPair(mac, "--ask"));   // no prompt on Linux yet: paired without it
    EXPECT_EQ(runs[1], "--agent NoInputNoOutput pair " + mac);
    runs.clear();
    bt["info " + mac] = out("\tName: Pad\n\tPaired: no\n");
    EXPECT_FALSE(n->pairBt(mac));
    EXPECT_EQ(runs.size(), 3u);   // info, pair and info: no trust or connect
    runs.clear();
    EXPECT_FALSE(n->pairBt("11:22:33:44:55"));
    EXPECT_FALSE(n->btWizPair("11:22:33:44:55:66; reboot", ""));
    EXPECT_TRUE(runs.empty());
}

TEST_F(NetworkTest, RemoveConnectDisconnectAndPower) {
    const std::string mac = "11:22:33:44:55:66";
    bt["remove " + mac] = out("[DEL] Device 11:22:33:44:55:66 Pad\nDevice has been removed\n");
    bt["connect " + mac] = out("Attempting to connect to 11:22:33:44:55:66\nFailed to connect: org.bluez.Error.Failed\n");
    bt["disconnect " + mac] = out("Attempting to disconnect from 11:22:33:44:55:66\nSuccessful disconnected\n");
    auto n = make();
    EXPECT_TRUE(n->unpairBt(mac));
    EXPECT_LE(timeouts["remove " + mac], 4000);   // the menu's input thread waits on it
    EXPECT_TRUE(n->btWizUnpair(mac));
    EXPECT_FALSE(n->btWizConnect(mac));
    EXPECT_TRUE(n->btWizDisconnect(mac));
    const size_t before = runs.size();
    n->btConfirmInbound(mac, 0);   // no inbound agent on Linux: logged
    EXPECT_EQ(runs.size(), before);
    EXPECT_FALSE(n->unpairBt("not-a-mac"));
    EXPECT_EQ(runs.size(), before);
}

TEST_F(NetworkTest, BluetoothdNotAnsweringBacksOff) {
    btDefault = RunResult{128 + 9, true, "Waiting to connect to bluetoothd...\n"};
    auto n = make();
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_TRUE(n->btListState().devices.empty());
    EXPECT_EQ(runs.size(), 1u);
}

// w5-linux-3: with Bluetooth off (the default, and a device without it) D35bluetooth runs neither the
// bus nor bluetoothd, and the net poll asks every few seconds: no bluetoothctl then.
TEST_F(NetworkTest, NoBluetoothctlWhileTheDaemonsAreNotMeantToRun) {
    bt["show"] = out("Controller 00:1A:7D:DA:71:13 (public)\n\tPowered: yes\n");
    struct Case {
        const char* what;
        const char* hasBt;
        const char* enabled;
        bool bus;
    };
    for (const Case& c : {Case{"off", "true", "0", false}, Case{"unset", "true", "", false},
                          Case{"no Bluetooth", "false", "1", false}, Case{"no bus yet", "true", "1", false},
                          Case{"no Bluetooth, bus", "", "1", true}}) {
        SCOPED_TRACE(c.what);
        props["ro.thimsy.device.bluetooth"] = c.hasBt;
        props["persist.thimsy.bluetooth.enabled"] = c.enabled;
        if (c.bus)
            put(root_ + "/run/dbus/system_bus_socket", "");
        else
            unlink((root_ + "/run/dbus/system_bus_socket").c_str());
        runs.clear();
        auto n = make();
        Network::WifiLevel wl = Network::kWifiLevel_Unknown;
        Network::BtLevel bl = Network::kBtLevel_Unknown;
        int bars = 0, connected = 0;
        std::string ssid;
        n->pollState(wl, bars, ssid, bl, connected);
        EXPECT_EQ(bl, Network::kBtLevel_Off);
        EXPECT_FALSE(n->btRadioOn());
        EXPECT_FALSE(n->btListState().on);
        EXPECT_TRUE(runs.empty());
    }
    // On, with the bus: asked.
    props["ro.thimsy.device.bluetooth"] = "true";
    props["persist.thimsy.bluetooth.enabled"] = "1";
    put(root_ + "/run/dbus/system_bus_socket", "");
    EXPECT_TRUE(make()->btRadioOn());
    EXPECT_EQ(ran("show"), 1);
}

// w5-linux-1: the PS3 wizard's Turn On/Off is the whole switch (the key, D35bluetooth, the radio),
// as the legacy screen's: `power on` alone has no bluetoothd while Bluetooth is off.
TEST_F(NetworkTest, SetBtRadioIsTheWholeSwitch) {
    const std::string d35 = root_ + "/etc/init.d/deferred/D35bluetooth";
    auto n = make();
    n->setBtRadio(true);
    EXPECT_EQ(runs, (std::vector<std::string>{d35 + " start", "power on"}));
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("persist.thimsy.bluetooth.enabled", v, "");
    EXPECT_STREQ(v, "1");
    runs.clear();
    n->setBtRadio(false);
    EXPECT_EQ(runs, (std::vector<std::string>{"power off", d35 + " stop"}));   // the radio first
    property_get("persist.thimsy.bluetooth.enabled", v, "");
    EXPECT_STREQ(v, "0");

    // No Bluetooth on this device (D35bluetooth's 3): no bluetoothctl, and the key goes back to 0.
    runs.clear();
    script = RunResult{3, false, ""};
    n->setBtRadio(true);
    EXPECT_EQ(runs, (std::vector<std::string>{d35 + " start"}));
    property_get("persist.thimsy.bluetooth.enabled", v, "");
    EXPECT_STREQ(v, "0");

    // `power on` failing at once while bluetoothd registers the controller: tried again.
    runs.clear();
    script = RunResult{0, false, ""};
    bt["power on"] = out("No default controller available\n", 1);
    slept = 0;
    n->setBtRadio(true);
    EXPECT_EQ(ran("power on"), 5);
    EXPECT_EQ(slept, 4 * 500);

    // No D35bluetooth on this image: as no Bluetooth, nothing runs and the key goes back to 0.
    runs.clear();
    ASSERT_EQ(unlink(d35.c_str()), 0);
    n->setBtRadio(true);
    EXPECT_TRUE(runs.empty());
    property_get("persist.thimsy.bluetooth.enabled", v, "");
    EXPECT_STREQ(v, "0");
}

// The switch ends the back-off of a bluetoothd that did not answer: it was just started.
TEST_F(NetworkTest, SetBtRadioEndsTheBackOff) {
    btDefault = RunResult{128 + 9, true, "Waiting to connect to bluetoothd...\n"};
    auto n = make();
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_EQ(ran("show"), 1);
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_EQ(ran("show"), 1);   // backing off
    btDefault = RunResult{0, false, ""};
    bt["show"] = out("Controller 00:1A:7D:DA:71:13 (public)\n\tPowered: yes\n");
    n->setBtRadio(true);
    EXPECT_TRUE(n->btRadioOn());
    EXPECT_EQ(ran("show"), 2);
}

// w5-linux-9: BlueZ drops an unpaired device it found TemporaryTimeout (30 s) after it last saw it; a
// device chosen later is looked for again before the pairing.
TEST_F(NetworkTest, PairLooksAgainForADeviceBlueZDropped) {
    const std::string mac = "11:22:33:44:55:66";
    int infos = 0;
    o.run = [this, &infos, mac](const std::vector<std::string>& argv, int timeoutMs) {
        std::string key;
        for (size_t i = 1; i < argv.size(); i++) key += (i > 1 ? " " : "") + argv[i];
        runs.push_back(key);
        timeouts[key] = timeoutMs;
        if (key == "info " + mac && infos++ == 0) return out("Device " + mac + " not available\n", 1);
        if (key == "info " + mac) return out("\tName: Pad\n\tPaired: yes\n");
        return RunResult{0, false, ""};
    };
    EXPECT_TRUE(make()->btWizPair(mac, ""));
    EXPECT_EQ(runs, (std::vector<std::string>{"info " + mac, "--timeout 5 scan on", "--agent NoInputNoOutput pair " + mac,
                                              "info " + mac, "trust " + mac, "connect " + mac}));
    EXPECT_EQ(timeouts["--timeout 5 scan on"], 10000);
}
