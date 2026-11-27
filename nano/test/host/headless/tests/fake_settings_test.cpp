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

// The capabilities and the settings dispatcher's stores (W6-caps) without NanoMenu: the host's
// fakes (platform/host/FakeCapabilities.h, FakeSettings.h), and the firmware's moved code
// (platform/firmware/Settings.cpp, Capabilities.cpp) against the property shims and the host's
// record-only shell gate, so Android's behaviour is checked where it now lives: kProp through the
// property API, the settings tables through `settings get` and `settings put`, and the post-write
// hooks. NanoScreenCal.cpp needs the DRM headers, so this program defines the two calls the moved
// Screen Calibration hook makes.

#include <memory>
#include <string>

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

#include "NanoPlatform.h"
#include "NanoScreenCal.h"
#include "headless_test_util.h"
#include "platform/firmware/Capabilities.h"
#include "platform/firmware/Settings.h"
#include "platform/host/FakeCapabilities.h"
#include "platform/host/FakeSettings.h"
#include "platform/host/HostControl.h"
#include "platform/host/ShellGateHost.h"

using namespace nanohost;
using headless_test::TmpDir;
using nanoplat::Cap;
using nanoplat::SettingSource;

namespace {
int gScreenCalApplied = 0;
}  // namespace

namespace android {
namespace screencal {
bool isCalibrationKey(const std::string& key) {
    return key.rfind(kPropPrefix, 0) == 0;
}
void applyFromProps() {
    gScreenCalApplied++;
}
}  // namespace screencal
}  // namespace android

namespace {

std::string prop(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "<unset>");
    return v;
}

class SettingsTest : public ::testing::Test {
 protected:
    void SetUp() override {
        resetForTest();
        nanocompat::InitOptions o;
        o.persistFile = dir / "persist.props";
        o.runDir = dir / "run";
        o.watch = false;
        std::string err;
        ASSERT_TRUE(nanocompat::init(o, &err)) << err;
        events = dir / "events.jsonl";
        ASSERT_TRUE(openEvents(events));
        gScreenCalApplied = 0;
    }
    void TearDown() override {
        shell::setRules({}, 127);
        resetForTest();
    }
    std::string log() { return headless_test::readFile(events); }
    bool logged(const std::string& text) { return log().find(text) != std::string::npos; }

    TmpDir dir;
    std::string events;
};

// --- The host's fakes ---------------------------------------------------------------------------

TEST_F(SettingsTest, FakeSupportsEverySettingByDefault) {
    std::unique_ptr<nanoplat::Settings> s = createFakeSettings(FakeSettingsConfig());
    EXPECT_TRUE(s->supports(SettingSource::kProp, "persist.gammaos.nano.kiosk"));
    EXPECT_TRUE(s->supports(SettingSource::kGlobal, "airplane_mode_on"));
    EXPECT_FALSE(s->supports(SettingSource::kNone, "persist.gammaos.nano.kiosk"));
    EXPECT_FALSE(s->supports(SettingSource::kProp, ""));
    // kProp through the property store.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.kiosk", "1"));
    EXPECT_EQ(prop("persist.gammaos.nano.kiosk"), "1");
    EXPECT_EQ(s->read(SettingSource::kProp, "persist.gammaos.nano.kiosk", "0"), "1");
    // The settings tables in memory, each write an event; no shell command.
    EXPECT_EQ(s->read(SettingSource::kSystem, "show_touches", "0"), "0");
    EXPECT_TRUE(s->write(SettingSource::kSystem, "show_touches", "1"));
    EXPECT_EQ(s->read(SettingSource::kSystem, "show_touches", "0"), "1");
    EXPECT_EQ(s->read(SettingSource::kGlobal, "show_touches", "0"), "0");   // another table
    EXPECT_EQ(prop("show_touches"), "<unset>");
    EXPECT_TRUE(logged("{\"ev\":\"setting\""));
    EXPECT_TRUE(logged("\"op\":\"put\",\"source\":\"system\",\"key\":\"show_touches\",\"value\":\"1\""));
    EXPECT_FALSE(logged("\"ev\":\"shell\""));
    const SettingsRecord r = settingsRecord();
    EXPECT_EQ(r.writes, 2);
    EXPECT_EQ(r.refused, 0);
    EXPECT_GE(r.reads, 4);
}

TEST_F(SettingsTest, FakeRefusesWhatTheConfigTurnsOff) {
    FakeSettingsConfig c;
    c.unsupported = {"global", "secure:location_mode", "prop:persist.gammaos.rgb.effect"};
    c.values["global:airplane_mode_on"] = "1";
    c.values["secure:ui_night_mode"] = "2";
    std::unique_ptr<nanoplat::Settings> s = createFakeSettings(c);
    EXPECT_FALSE(s->supports(SettingSource::kGlobal, "airplane_mode_on"));   // the whole table
    EXPECT_EQ(s->read(SettingSource::kGlobal, "airplane_mode_on", "0"), "0");   // the default, not the value
    EXPECT_FALSE(s->write(SettingSource::kGlobal, "airplane_mode_on", "1"));
    EXPECT_FALSE(s->supports(SettingSource::kSecure, "location_mode"));   // one key
    EXPECT_TRUE(s->supports(SettingSource::kSecure, "ui_night_mode"));
    EXPECT_EQ(s->read(SettingSource::kSecure, "ui_night_mode", "1"), "2");   // seeded
    EXPECT_FALSE(s->write(SettingSource::kProp, "persist.gammaos.rgb.effect", "none"));
    EXPECT_EQ(prop("persist.gammaos.rgb.effect"), "<unset>");   // nothing stored
    EXPECT_TRUE(logged("\"op\":\"refused\",\"source\":\"global\",\"key\":\"airplane_mode_on\""));
    EXPECT_TRUE(logged("\"op\":\"refused\",\"source\":\"prop\",\"key\":\"persist.gammaos.rgb.effect\""));
    const SettingsRecord r = settingsRecord();
    EXPECT_EQ(r.refused, 2);
    EXPECT_EQ(r.writes, 0);
}

TEST_F(SettingsTest, FakeKeepsLinuxsSystemSettingsAsProperties) {
    FakeSettingsConfig c;
    c.values["system:screen_off_timeout"] = "300000";
    std::unique_ptr<nanoplat::Settings> s = createFakeSettings(c);
    EXPECT_EQ(s->read(SettingSource::kSystem, "screen_off_timeout", "60000"), "300000");   // seeded
    // Font Size lands where Linux keeps it (and the render loop reads it), and is still an event.
    EXPECT_TRUE(s->write(SettingSource::kSystem, "font_scale", "1.15"));
    EXPECT_EQ(prop("persist.gammaos.nano.fontscale"), "1.15");
    EXPECT_EQ(s->read(SettingSource::kSystem, "font_scale", "1.0"), "1.15");
    EXPECT_TRUE(logged("\"op\":\"put\",\"source\":\"system\",\"key\":\"font_scale\",\"value\":\"1.15\""));
    EXPECT_TRUE(s->write(SettingSource::kSystem, "screen_off_timeout", "120000"));
    EXPECT_EQ(prop("persist.thimsy.system.screen_off_timeout"), "120000");
    EXPECT_EQ(s->read(SettingSource::kSystem, "screen_off_timeout", "60000"), "120000");
    // Only those two: another system setting stays in memory, and the same key in another table is
    // not Linux's.
    EXPECT_TRUE(s->write(SettingSource::kGlobal, "font_scale", "2.0"));
    EXPECT_EQ(prop("persist.gammaos.nano.fontscale"), "1.15");
    EXPECT_EQ(prop("font_scale"), "<unset>");
    EXPECT_FALSE(logged("\"ev\":\"shell\""));
    EXPECT_EQ(settingsRecord().writes, 3);
}

TEST_F(SettingsTest, FakeRefusesLinuxsSystemSettingsWhenTheConfigTurnsThemOff) {
    FakeSettingsConfig c;
    c.unsupported = {"system:font_scale"};
    std::unique_ptr<nanoplat::Settings> s = createFakeSettings(c);
    EXPECT_FALSE(s->write(SettingSource::kSystem, "font_scale", "1.15"));
    EXPECT_EQ(prop("persist.gammaos.nano.fontscale"), "<unset>");   // nothing stored
    EXPECT_EQ(s->read(SettingSource::kSystem, "font_scale", "1.0"), "1.0");
}

TEST(SettingSpec, StoresAndKeys) {
    EXPECT_TRUE(validSettingSpec("prop"));
    EXPECT_TRUE(validSettingSpec("system:font_scale"));
    EXPECT_FALSE(validSettingSpec("none"));
    EXPECT_FALSE(validSettingSpec("global:"));
    EXPECT_FALSE(validSettingSpec("registry:x"));
    EXPECT_STREQ(settingStoreName(SettingSource::kSecure), "secure");
}

TEST(FakeCapabilities, TrueUnlessAnswered) {
    std::unique_ptr<nanoplat::Capabilities> all = createFakeCapabilities(CapAnswers());
    for (int i = 0; i < static_cast<int>(Cap::Count); ++i) EXPECT_TRUE(all->has(static_cast<Cap>(i))) << i;
    EXPECT_FALSE(all->has(Cap::Count));
    CapAnswers answers = {{Cap::FactoryReset, false}, {Cap::Mtp, false}, {Cap::Mtp, true}};
    std::unique_ptr<nanoplat::Capabilities> some = createFakeCapabilities(answers);
    EXPECT_FALSE(some->has(Cap::FactoryReset));
    EXPECT_TRUE(some->has(Cap::Mtp));   // the later answer wins
    EXPECT_TRUE(some->has(Cap::RebootRecovery));
}

TEST(FakeCapabilities, EveryCapabilityHasAName) {
    for (int i = 0; i < static_cast<int>(Cap::Count); ++i) {
        const Cap cap = static_cast<Cap>(i);
        const std::string name = capName(cap);
        ASSERT_FALSE(name.empty()) << "Cap " << i << " has no name in FakeCapabilities.cpp";
        Cap back = Cap::Count;
        ASSERT_TRUE(capFromName(name, &back)) << name;
        EXPECT_TRUE(back == cap) << name;
    }
    EXPECT_STREQ(capName(Cap::Count), "");
    EXPECT_FALSE(capFromName("", nullptr));
    EXPECT_FALSE(capFromName("Count", nullptr));
}

TEST(PlatformHas, AsksTheCapabilities) {
    nanoplat::Platform p;
    EXPECT_FALSE(p.has(Cap::Mtp));   // none set
    p.caps = createFakeCapabilities({{Cap::Mtp, false}});
    EXPECT_FALSE(p.has(Cap::Mtp));
    EXPECT_TRUE(p.has(Cap::Dnd));
}

// --- The firmware's moved code ------------------------------------------------------------------

TEST(FirmwareCapabilities, AndroidHasThemAll) {
    std::unique_ptr<nanoplat::Capabilities> caps = nanofirmware::createCapabilities();
    for (int i = 0; i < static_cast<int>(Cap::Count); ++i) EXPECT_TRUE(caps->has(static_cast<Cap>(i))) << i;
}

TEST_F(SettingsTest, FirmwareSupportsEveryKeyAndKeepsPropertiesAsIs) {
    std::unique_ptr<nanoplat::Settings> s = nanofirmware::createSettings();
    EXPECT_TRUE(s->supports(SettingSource::kProp, "persist.gammaos.nano.a_row_added_upstream"));
    EXPECT_TRUE(s->supports(SettingSource::kGlobal, "airplane_mode_on"));
    EXPECT_EQ(s->read(SettingSource::kProp, "persist.gammaos.nano.kiosk", "0"), "0");
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.kiosk", "1"));
    EXPECT_EQ(prop("persist.gammaos.nano.kiosk"), "1");
    EXPECT_EQ(s->read(SettingSource::kProp, "persist.gammaos.nano.kiosk", "0"), "1");
    EXPECT_EQ(s->read(SettingSource::kProp, "", "def"), "def");   // no key: the default, nothing run
    EXPECT_TRUE(s->write(SettingSource::kNone, "x", "1"));        // reports success, as before the move
    EXPECT_FALSE(logged("\"ev\":\"shell\""));
}

TEST_F(SettingsTest, FirmwareRunsTheSettingsTool) {
    shell::Rule get;
    get.match = "settings get global 'airplane_mode_on' 2>/dev/null";
    get.exact = true;
    get.out = "1\n";
    shell::Rule nul;
    nul.match = "settings get secure 'ui_night_mode'";
    nul.out = "null\n";
    shell::Rule put;
    put.match = "settings put ";
    shell::setRules({get, nul, put}, 127);
    std::unique_ptr<nanoplat::Settings> s = nanofirmware::createSettings();
    EXPECT_EQ(s->read(SettingSource::kGlobal, "airplane_mode_on", "0"), "1");
    EXPECT_EQ(s->read(SettingSource::kSecure, "ui_night_mode", "1"), "1");   // "null": the default
    EXPECT_TRUE(s->write(SettingSource::kSystem, "show_touches", "1"));
    EXPECT_TRUE(logged("settings put system 'show_touches' '1' 2>/dev/null")) << log();
    EXPECT_EQ(prop("show_touches"), "<unset>");
}

TEST_F(SettingsTest, FirmwareRunsThePostWriteHooks) {
    shell::Rule any;
    any.match = "";   // every command answers 0
    shell::setRules({any}, 127);
    std::unique_ptr<nanoplat::Settings> s = nanofirmware::createSettings();
    // Font Size: mirrored into the property the render loop reads.
    EXPECT_TRUE(s->write(SettingSource::kSystem, "font_scale", "1.15"));
    EXPECT_EQ(prop("persist.gammaos.nano.fontscale"), "1.15");
    // Screen Calibration: applied at once.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.screencal.top.red", "90"));
    EXPECT_EQ(gScreenCalApplied, 1);
    // Saturation: the colour matrix, clamped.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.display.saturation", "140"));
    EXPECT_TRUE(logged("cmd color_display set-saturation 100 2>/dev/null")) << log();
    // Orientation: the single auto-rotate control.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.orientation", "landscape"));
    EXPECT_TRUE(logged("settings put system 'accelerometer_rotation' '0' 2>/dev/null")) << log();
    // A gamepad property bumps the daemon's config_version; config_version itself does not.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.gamepad.abxy_swap", "1"));
    EXPECT_EQ(prop("persist.gammaos.gamepad.config_version"), "1");
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.gamepad.config_version", "5"));
    EXPECT_EQ(prop("persist.gammaos.gamepad.config_version"), "5");
    // The Parallax recomposition is not the firmware's: it stays in NanoMenuSettingsTree.cpp.
    EXPECT_TRUE(s->write(SettingSource::kProp, "persist.gammaos.nano.pspclock.parallax", "0"));
    EXPECT_EQ(prop("persist.gammaos.nano.pspclock.tilt.cal"), "<unset>");
}

}  // namespace
