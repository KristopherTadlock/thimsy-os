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

// The host's fake power and system actions (platform/host/FakePower.h): what they record and report,
// and their {"ev":"power"} and {"ev":"action"} events.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/FakePower.h"
#include "platform/host/HostControl.h"

using namespace nanohost;

TEST(FakePower, BatteryIsScripted) {
    setFakeBattery(-1, false);
    std::unique_ptr<nanoplat::Power> p = createFakePower();
    int pct = -1;
    bool charging = false;
    p->readBattery(pct, charging);
    EXPECT_EQ(pct, -1);  // none scripted: left as it was
    setFakeBattery(64, true);
    p->readBattery(pct, charging);
    EXPECT_EQ(pct, 64);
    EXPECT_TRUE(charging);
    setFakeBattery(150, false);
    p->readBattery(pct, charging);
    EXPECT_EQ(pct, 100);
    EXPECT_FALSE(charging);
    setFakeBattery(-1, false);
    EXPECT_EQ(powerRecord().batteryReads, 3);
}

TEST(FakePower, RecordsBrightnessAndActions) {
    headless_test::TmpDir dir;
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    std::unique_ptr<nanoplat::Power> p = createFakePower();
    EXPECT_FALSE(p->setBrightnessViaHal(200));
    EXPECT_EQ(p->savedBrightness(), -1);
    p->mirrorBrightness(128);
    p->powerAction("shutdown");
    p->powerAction("reboot");
    PowerRecord r = powerRecord();
    EXPECT_EQ(r.halCalls, 1);
    EXPECT_EQ(r.halLevel, 200);
    EXPECT_EQ(r.mirrors, 1);
    EXPECT_EQ(r.mirrorLevel, 128);
    EXPECT_EQ(r.savedReads, 1);
    EXPECT_EQ(r.actions, (std::vector<std::string>{"shutdown", "reboot"}));
    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"power\""), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"mirror\",\"level\":128"), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"action\",\"action\":\"shutdown\""), std::string::npos);
    // A new fake starts a new record.
    std::unique_ptr<nanoplat::Power> q = createFakePower();
    EXPECT_TRUE(powerRecord().actions.empty());
    ASSERT_TRUE(openEvents(""));
}

TEST(FakeActions, RecordsEachCall) {
    headless_test::TmpDir dir;
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    std::unique_ptr<nanoplat::SystemActions> a = createFakeActions();
    a->setWifiEnabled(true);
    a->forgetWifiNetwork(3);
    a->setWifiEnabled(false);
    a->setBluetoothEnabled(true);
    a->connectBluetooth("AA:BB:CC:DD:EE:FF");
    a->setBluetoothEnabled(false);
    a->setTimeFormat(true);
    a->setTimeFormat(false);
    a->setTimeZone("Europe/London");
    EXPECT_EQ(actionsRecord(),
              (std::vector<std::string>{"wifi on", "forget 3", "wifi off", "bt on", "bt connect AA:BB:CC:DD:EE:FF",
                                        "bt off", "time 12h", "time 24h", "zone Europe/London"}));
    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"action\""), std::string::npos);
    EXPECT_NE(text.find("\"call\":\"zone Europe/London\""), std::string::npos);
    std::unique_ptr<nanoplat::SystemActions> b = createFakeActions();
    EXPECT_TRUE(actionsRecord().empty());
    ASSERT_TRUE(openEvents(""));
}
