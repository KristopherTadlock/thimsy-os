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

// The host's fake system actions (FakePower.h).

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "FakePower.h"
#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gMu;
std::vector<std::string> gCalls;  // guarded by gMu

class FakeActions : public nanoplat::SystemActions {
 public:
    FakeActions() {
        std::lock_guard<std::mutex> lock(gMu);
        gCalls.clear();
    }

    void setWifiEnabled(bool on) override { record(on ? "wifi on" : "wifi off"); }
    void forgetWifiNetwork(int savedNetId) override { record("forget " + std::to_string(savedNetId)); }
    void setBluetoothEnabled(bool on) override { record(on ? "bt on" : "bt off"); }
    void connectBluetooth(const std::string& mac) override { record("bt connect " + mac); }
    void setTimeFormat(bool h12) override { record(h12 ? "time 12h" : "time 24h"); }
    void setTimeZone(const std::string& tz) override { record("zone " + tz); }

 private:
    static void record(const std::string& call) {
        {
            std::lock_guard<std::mutex> lock(gMu);
            gCalls.push_back(call);
        }
        event("action", "\"call\":" + jsonQuote(call));
    }
};

}  // namespace

std::unique_ptr<nanoplat::SystemActions> createFakeActions() {
    return std::unique_ptr<nanoplat::SystemActions>(new FakeActions);
}

std::vector<std::string> actionsRecord() {
    std::lock_guard<std::mutex> lock(gMu);
    return gCalls;
}

}  // namespace nanohost
