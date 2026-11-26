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

// The Linux power (Power.cpp) over a fake sysfs tree, with ControlKeys.cpp's request sequence.

#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "ControlKeys.h"
#include "Power.h"
#include "compat_test_util.h"

using nanolinux::power::BatteryReading;

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

class PowerTest : public compat_test::StoreTest {
 protected:
    std::string supply(const std::string& name) { return dir_ / ("root/sys/class/power_supply/" + name); }
    std::string root() { return dir_ / "root"; }
};

}  // namespace

TEST_F(PowerTest, NoSuppliesLeavesTheValues) {
    BatteryReading r = nanolinux::power::readBattery(root());
    EXPECT_FALSE(r.found);
    nanolinux::power::Options o;
    o.root = root();
    std::unique_ptr<nanoplat::Power> p = nanolinux::power::create(o);
    int pct = 42;
    bool charging = true;
    p->readBattery(pct, charging);
    EXPECT_EQ(pct, 42);
    EXPECT_TRUE(charging);
}

TEST_F(PowerTest, ReadsCapacityAndStatus) {
    put(supply("ac/type"), "Mains\n");
    put(supply("ac/online"), "0\n");
    put(supply("battery/type"), "Battery\n");
    put(supply("battery/capacity"), "87\n");
    put(supply("battery/status"), "Discharging\n");
    BatteryReading r = nanolinux::power::readBattery(root());
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.percent, 87);
    EXPECT_FALSE(r.charging);
    EXPECT_EQ(r.supply, "battery");

    put(supply("battery/status"), "Charging\n");
    EXPECT_TRUE(nanolinux::power::readBattery(root()).charging);
    put(supply("battery/status"), "Full\n");
    EXPECT_TRUE(nanolinux::power::readBattery(root()).charging);
    put(supply("battery/status"), "Not charging\n");
    EXPECT_FALSE(nanolinux::power::readBattery(root()).charging);

    nanolinux::power::Options o;
    o.root = root();
    std::unique_ptr<nanoplat::Power> p = nanolinux::power::create(o);
    int pct = -1;
    bool charging = true;
    p->readBattery(pct, charging);
    EXPECT_EQ(pct, 87);
    EXPECT_FALSE(charging);
}

TEST_F(PowerTest, ClampsAndFallsBackToChargeOrEnergy) {
    put(supply("BAT0/type"), "Battery\n");
    put(supply("BAT0/capacity"), "104\n");
    EXPECT_EQ(nanolinux::power::readBattery(root()).percent, 100);
    put(supply("BAT0/capacity"), "-3\n");
    EXPECT_EQ(nanolinux::power::readBattery(root()).percent, 0);
    put(supply("BAT0/capacity"), "junk\n");
    put(supply("BAT0/charge_now"), "1500000\n");
    put(supply("BAT0/charge_full"), "2000000\n");
    EXPECT_EQ(nanolinux::power::readBattery(root()).percent, 75);
    unlink(supply("BAT0/charge_now").c_str());
    put(supply("BAT0/energy_now"), "1\n");
    put(supply("BAT0/energy_full"), "3\n");
    EXPECT_EQ(nanolinux::power::readBattery(root()).percent, 33);
    put(supply("BAT0/energy_full"), "0\n");
    EXPECT_FALSE(nanolinux::power::readBattery(root()).found);
}

TEST_F(PowerTest, SkipsAbsentAndNonBatterySupplies) {
    put(supply("a-usb/type"), "USB\n");
    put(supply("a-usb/capacity"), "10\n");  // not a battery: never read as one
    put(supply("b-bat/type"), "Battery\n");
    put(supply("b-bat/present"), "0\n");
    put(supply("b-bat/capacity"), "20\n");
    put(supply("c-bat/type"), "Battery\n");
    put(supply("c-bat/capacity"), "30\n");
    put(supply("d-bat/type"), "Battery\n");
    put(supply("d-bat/capacity"), "40\n");
    BatteryReading r = nanolinux::power::readBattery(root());
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.supply, "c-bat");
    EXPECT_EQ(r.percent, 30);
}

TEST_F(PowerTest, NoStatusUsesTheExternalSupply) {
    put(supply("battery/type"), "Battery\n");
    put(supply("battery/capacity"), "50\n");
    put(supply("usb/type"), "USB\n");
    put(supply("usb/online"), "0\n");
    EXPECT_FALSE(nanolinux::power::readBattery(root()).charging);
    put(supply("usb/online"), "1\n");
    EXPECT_TRUE(nanolinux::power::readBattery(root()).charging);
}

TEST_F(PowerTest, BrightnessIsTheStoresLevel) {
    std::unique_ptr<nanoplat::Power> p = nanolinux::power::create();
    EXPECT_FALSE(p->setBrightnessViaHal(128));
    EXPECT_EQ(p->savedBrightness(), -1);
    ASSERT_EQ(property_set("persist.gammaos.nano.brightness", "77"), 0);
    EXPECT_EQ(p->savedBrightness(), 77);
    p->mirrorBrightness(90);  // no second record on Linux: the store's key is unchanged
    EXPECT_EQ(p->savedBrightness(), 77);
    for (const char* bad : {"0", "256", "-1", "12x", ""}) {
        ASSERT_EQ(property_set("persist.gammaos.nano.brightness", bad), 0);
        EXPECT_EQ(p->savedBrightness(), -1) << bad;
    }
    EXPECT_EQ(nanolinux::power::parseLevel(" 255\n"), 255);
    EXPECT_EQ(nanolinux::power::parseLevel("1"), 1);
}

TEST_F(PowerTest, PowerActionsAreSessionRequests) {
    std::vector<std::string> verbs;
    nanolinux::power::Options o;
    o.sessionRequest = [&](const std::string& v) { verbs.push_back(v); };
    std::unique_ptr<nanoplat::Power> p = nanolinux::power::create(o);
    p->powerAction("reboot");
    p->powerAction("shutdown");
    p->powerAction("recovery");
    p->powerAction("safemode");
    p->powerAction("android");
    p->powerAction(nullptr);
    EXPECT_EQ(verbs, (std::vector<std::string>{"reboot", "poweroff"}));
    EXPECT_EQ(nanolinux::power::sessionVerbFor("shutdown"), "poweroff");
    EXPECT_EQ(nanolinux::power::sessionVerbFor("Reboot"), "");
}

TEST_F(PowerTest, DefaultRequestSharesTheControlKeysSequence) {
    // Without install(): the default writes the request key, one past its current number.
    std::unique_ptr<nanoplat::Power> p = nanolinux::power::create();
    p->powerAction("reboot");
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.thimsy.session.request", v, "");
    EXPECT_STREQ(v, "1:reboot");
    ASSERT_EQ(property_set("sys.gammaos.nano.dosleep", "1"), 0);  // no observer installed: just a write
    p->powerAction("shutdown");
    property_get("sys.thimsy.session.request", v, "");
    EXPECT_STREQ(v, "2:poweroff");

    // With install(): the installed handler, which the control keys' own requests also use.
    std::vector<std::string> seen;
    nanolinux::controlkeys::Handlers h;
    h.sessionRequest = [&](const std::string& verb) { seen.push_back(verb); };
    nanolinux::controlkeys::install(h);
    p->powerAction("reboot");
    ASSERT_EQ(property_set("sys.gammaos.nano.dosleep", "1"), 0);
    nanolinux::controlkeys::uninstall();
    EXPECT_EQ(seen, (std::vector<std::string>{"reboot", "sleep"}));
}
