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

// The host's fake Wi-Fi and Bluetooth (platform/host/FakeNetwork.h, G7-net): the config's scripted
// networks and devices, parsed by HostConfig, answered and changed as the screens drive them, and
// the {"ev":"net"} events.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/FakeNetwork.h"
#include "platform/host/HostConfig.h"
#include "platform/host/HostControl.h"

using namespace nanohost;
using nanoplat::Network;

namespace {

const char kConfig[] = R"({"network": {"wifi_on": true, "connected": "Home", "ip": "10.0.0.7", "dns2": "9.9.9.9",
    "networks": [{"ssid": "Home", "bssid": "aa:bb:cc:00:00:01", "rssi": -50, "security": 2, "saved_id": 0},
                 {"ssid": "Cafe", "rssi": -80, "security": 3, "password": "espresso"},
                 {"ssid": "Away", "saved_id": 4, "in_range": false}],
    "bt_on": true,
    "bt_devices": [{"address": "11:22:33:44:55:66", "name": "Pad", "bonded": true, "connected": true, "cod": 1288},
                   {"address": "11:22:33:44:55:77", "name": "Speaker"},
                   {"address": "11:22:33:44:55:88", "in_range": false}]}})";

FakeNetConfig parsed() {
    HostConfig c;
    std::string err;
    EXPECT_TRUE(parseHostConfig(kConfig, &c, &err)) << err;
    return c.network;
}

}  // namespace

TEST(FakeNetworkConfig, ParsesTheScript) {
    const FakeNetConfig n = parsed();
    EXPECT_TRUE(n.wifiOn);
    EXPECT_EQ(n.connected, "Home");
    EXPECT_EQ(n.ip, "10.0.0.7");
    EXPECT_EQ(n.gateway, "192.168.50.1");   // the default
    ASSERT_EQ(n.networks.size(), 3u);
    EXPECT_EQ(n.networks[0].savedId, 0);
    EXPECT_EQ(n.networks[1].savedId, -1);
    EXPECT_EQ(n.networks[1].password, "espresso");
    EXPECT_FALSE(n.networks[2].inRange);
    ASSERT_EQ(n.btDevices.size(), 3u);
    EXPECT_EQ(n.btDevices[0].cod, 1288);
    EXPECT_TRUE(n.btDevices[1].inRange);
    HostConfig empty;
    std::string err;
    ASSERT_TRUE(parseHostConfig("{}", &empty, &err)) << err;
    EXPECT_TRUE(empty.network.networks.empty());
    EXPECT_TRUE(empty.network.wifiOn);
    const char* bad[] = {"{\"network\": []}", "{\"network\": {\"wifi\": true}}",
                         "{\"network\": {\"networks\": [{\"rssi\": -50}]}}",
                         "{\"network\": {\"networks\": [{\"ssid\": \"A\", \"security\": 7}]}}",
                         "{\"network\": {\"bt_devices\": [{\"name\": \"x\"}]}}",
                         "{\"network\": {\"bt_on\": 1}}"};
    for (const char* t : bad) {
        HostConfig b;
        EXPECT_FALSE(parseHostConfig(t, &b, &err)) << t;
    }
}

TEST(FakeNetwork, AnswersFromTheScript) {
    std::unique_ptr<Network> n = createFakeNetwork(parsed());
    Network::WifiLevel w = Network::kWifiLevel_Unknown;
    Network::BtLevel b = Network::kBtLevel_Unknown;
    int bars = 0, btCount = 0;
    std::string ssid;
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Connected);
    EXPECT_EQ(bars, 4);
    EXPECT_EQ(ssid, "Home");
    EXPECT_EQ(b, Network::kBtLevel_Connected);
    EXPECT_EQ(btCount, 1);
    const Network::WifiRadio r = n->wifiListRadio();
    EXPECT_TRUE(r.on);
    EXPECT_EQ(r.connectedSsid, "Home");
    const Network::WifiLists l = n->wifiLists();
    ASSERT_EQ(l.saved.size(), 2u);     // Home and Away
    EXPECT_EQ(l.saved[1].savedNetId, 4);
    EXPECT_EQ(l.saved[1].rssi, -127);
    ASSERT_EQ(l.scanned.size(), 2u);   // Home and Cafe
    EXPECT_EQ(l.scanned[1].security, 3);
    EXPECT_EQ(l.scanned[1].savedNetId, -1);
    EXPECT_TRUE(n->wifiRadioEnabled());
    const Network::WifiAddresses a = n->wifiAddresses(true);
    EXPECT_EQ(a.ip, "10.0.0.7");
    EXPECT_EQ(a.dns2, "9.9.9.9");
    EXPECT_EQ(n->wifiAddresses(false).gw, "");
    EXPECT_EQ(n->wifiMacAndIp().mac, "02:00:00:00:00:01");
    EXPECT_TRUE(n->wifiLink().validated);
    const Network::BtList bl = n->btListState();
    EXPECT_TRUE(bl.on);
    ASSERT_EQ(bl.devices.size(), 1u);
    EXPECT_EQ(bl.devices[0].name, "Pad");
    EXPECT_EQ(n->discoverBt().size(), 2u);   // the in-range ones
    EXPECT_TRUE(networkRecord() == (std::vector<std::string>{"bt scan 8"}));
}

TEST(FakeNetwork, JoinsPairsAndRecords) {
    headless_test::TmpDir dir;
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    std::unique_ptr<Network> n = createFakeNetwork(parsed());
    // A wrong key: not connected, reported once the watch looks.
    n->joinWifi("Cafe", 3, "latte", false);
    EXPECT_EQ(n->checkWifiJoin("Cafe"), Network::JoinCheck::WrongPassword);
    n->joinWifi("Cafe", 3, "espresso", true);
    EXPECT_EQ(n->checkWifiJoin("Cafe"), Network::JoinCheck::Connected);
    EXPECT_EQ(n->wifiListRadio().connectedSsid, "Cafe");
    // It is saved now, under the next free id.
    bool saved = false;
    for (const auto& e : n->wifiLists().saved) saved = saved || (e.ssid == "Cafe" && e.savedNetId == 5);
    EXPECT_TRUE(saved);
    EXPECT_TRUE(n->connectSavedWifi(0));
    EXPECT_EQ(n->wifiListRadio().connectedSsid, "Home");
    EXPECT_FALSE(n->connectSavedWifi(4));   // out of range
    n->startWifiScan();
    // Bluetooth: pair, connect, disconnect, remove; the radio.
    EXPECT_TRUE(n->btWizPair("11:22:33:44:55:77", "--ask"));
    EXPECT_EQ(n->bondedBt().size(), 2u);
    EXPECT_TRUE(n->btWizDisconnect("11:22:33:44:55:77"));
    EXPECT_TRUE(n->btWizConnect("11:22:33:44:55:77"));
    EXPECT_TRUE(n->unpairBt("11:22:33:44:55:77"));
    EXPECT_FALSE(n->btWizUnpair("11:22:33:44:55:77"));   // no longer bonded
    EXPECT_FALSE(n->pairBt("11:22:33:44:55:88"));         // out of range
    n->btConfirmInbound("11:22:33:44:55:88", 0);
    n->setBtRadio(false);
    EXPECT_FALSE(n->btRadioOn());
    EXPECT_FALSE(n->btListState().on);
    EXPECT_TRUE(n->btWizScan().empty());
    const std::vector<std::string> want = {
            "wifi join Cafe 3", "wifi join Cafe 3 force", "wifi connect 0", "wifi connect 4", "wifi scan",
            "bt pair 11:22:33:44:55:77 --ask", "bt disconnect 11:22:33:44:55:77", "bt connect 11:22:33:44:55:77",
            "bt unpair 11:22:33:44:55:77", "bt unpair 11:22:33:44:55:77", "bt pair 11:22:33:44:55:88",
            "bt confirm 11:22:33:44:55:88 0", "bt radio off", "bt scan 15"};
    EXPECT_TRUE(networkRecord() == want);
    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"net\""), std::string::npos);
    EXPECT_NE(text.find("\"call\":\"wifi join Cafe 3\""), std::string::npos);
    EXPECT_EQ(text.find("espresso"), std::string::npos);   // a key is never recorded
    EXPECT_EQ(text.find("latte"), std::string::npos);
    ASSERT_TRUE(openEvents(""));
}

TEST(FakeNetwork, RadioOffAndAutoEnable) {
    FakeNetConfig c = parsed();
    c.wifiOn = false;
    std::unique_ptr<Network> n = createFakeNetwork(c);
    Network::WifiLevel w = Network::kWifiLevel_Unknown;
    Network::BtLevel b = Network::kBtLevel_Unknown;
    int bars = 0, btCount = 0;
    std::string ssid;
    n->pollState(w, bars, ssid, b, btCount);
    EXPECT_EQ(w, Network::kWifiLevel_Off);
    EXPECT_FALSE(n->wifiListRadio().on);
    EXPECT_TRUE(n->wifiLists().scanned.empty());
    EXPECT_EQ(n->wifiAddresses(false).ip, "");
    n->autoEnableWifi();   // saved networks: the radio comes on, as the firmware's poll does
    EXPECT_TRUE(n->wifiRadioEnabled());
    n->autoEnableWifi();
    EXPECT_TRUE(networkRecord() == (std::vector<std::string>{"wifi auto-enable"}));
}
