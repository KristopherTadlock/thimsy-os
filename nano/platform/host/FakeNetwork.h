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

// The host platform's Network (docs/linux/IMPLEMENTATION.md B.6, D.3, G7-net): the host config's
// scripted Wi-Fi networks and Bluetooth devices ("network", HostConfig.h), answered the way the
// screens expect, and changed by what Nano asks: a join saves the network (the next free id) and
// connects to it when it is in range and the key is the scripted one ("" takes any key); a wrong
// key makes checkWifiJoin report WrongPassword; a pairing bonds an in-range device; forget is
// SystemActions' (FakeActions.cpp) and is not seen here. Each operation is recorded (networkRecord())
// and emitted as {"ev":"net","call":...}; a key is never recorded. Any thread.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct FakeWifiNet {
    std::string ssid;
    std::string bssid;
    int rssi = -60;
    int security = 2;        // WifiNetEntry's 0-4
    int savedId = -1;        // >= 0: saved under that id
    bool inRange = true;     // in the scan's results
    std::string password;    // the key a join must give; "" takes any
};

struct FakeBtDevice {
    std::string address;
    std::string name;
    bool bonded = false;
    bool connected = false;
    int cod = 0;
    bool inRange = true;     // found by an inquiry, and can be paired
};

struct FakeNetConfig {
    bool wifiOn = true;
    std::string connected;   // the SSID connected at start ("" none)
    std::string ip = "192.168.50.20";
    std::string mac = "02:00:00:00:00:01";
    std::string gateway = "192.168.50.1";
    std::string dns = "192.168.50.1";
    std::string dns2;
    bool validated = true;   // the Internet test passes while connected
    std::vector<FakeWifiNet> networks;
    bool btOn = true;
    std::vector<FakeBtDevice> btDevices;
};

std::unique_ptr<nanoplat::Network> createFakeNetwork(FakeNetConfig config);

// The operations asked so far, in order ("wifi join Home 2", "bt pair AA:BB:..."), since the last
// createFakeNetwork().
std::vector<std::string> networkRecord();

}  // namespace nanohost
