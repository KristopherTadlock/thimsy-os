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

// The host's fake power (FakePower.h).

#include "FakePower.h"

#include <memory>
#include <mutex>
#include <string>

#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gMu;
PowerRecord gRecord;          // guarded by gMu
int gBatteryPercent = -1;     // guarded by gMu
bool gBatteryCharging = false;

class FakePower : public nanoplat::Power {
 public:
    FakePower() {
        std::lock_guard<std::mutex> lock(gMu);
        gRecord = PowerRecord();
    }

    void readBattery(int& percent, bool& charging) override {
        std::lock_guard<std::mutex> lock(gMu);
        gRecord.batteryReads++;
        if (gBatteryPercent < 0) return;
        percent = gBatteryPercent;
        charging = gBatteryCharging;
    }

    bool setBrightnessViaHal(int level) override {
        std::lock_guard<std::mutex> lock(gMu);
        gRecord.halCalls++;
        gRecord.halLevel = level;
        return false;
    }

    void mirrorBrightness(int level) override {
        {
            std::lock_guard<std::mutex> lock(gMu);
            gRecord.mirrors++;
            gRecord.mirrorLevel = level;
        }
        event("power", "\"op\":\"mirror\",\"level\":" + std::to_string(level));
    }

    int savedBrightness() override {
        std::lock_guard<std::mutex> lock(gMu);
        gRecord.savedReads++;
        return -1;
    }

    void powerAction(const char* action) override {
        const std::string a = action ? action : "";
        {
            std::lock_guard<std::mutex> lock(gMu);
            gRecord.actions.push_back(a);
        }
        event("power", "\"op\":\"action\",\"action\":" + jsonQuote(a));
    }
};

}  // namespace

void setFakeBattery(int percent, bool charging) {
    std::lock_guard<std::mutex> lock(gMu);
    gBatteryPercent = percent < 0 ? -1 : (percent > 100 ? 100 : percent);
    gBatteryCharging = charging;
}

std::unique_ptr<nanoplat::Power> createFakePower() {
    return std::unique_ptr<nanoplat::Power>(new FakePower);
}

PowerRecord powerRecord() {
    std::lock_guard<std::mutex> lock(gMu);
    return gRecord;
}

}  // namespace nanohost
