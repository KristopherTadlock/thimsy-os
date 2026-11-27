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

// The Linux settings table and capabilities (Settings.h, Capabilities.h; W6-caps) on a scratch
// property store: Nano's preferences round-trip, Android's settings tables and the keys another
// Android process acts on are unsupported, an unknown key fails closed, and no capability is
// claimed that Linux does not implement.

#include "Capabilities.h"
#include "Settings.h"

#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "compat_test_util.h"

using compat_test::ScratchDir;
using nanoplat::SettingSource;

namespace {

std::string prop(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "<unset>");
    return v;
}

class LinuxSettingsTest : public ::testing::Test {
 protected:
    void SetUp() override {
        std::string err;
        ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir), &err)) << err;
        settings = nanolinux::settings::create();
        ASSERT_TRUE(settings != nullptr);
    }

    ScratchDir dir;
    std::unique_ptr<nanoplat::Settings> settings;
};

TEST_F(LinuxSettingsTest, NanoPreferenceRoundTrips) {
    const std::string key = "persist.gammaos.nano.nav_sounds";
    EXPECT_TRUE(settings->supports(SettingSource::kProp, key));
    EXPECT_EQ(settings->read(SettingSource::kProp, key, "1"), "1");   // unset: the default
    EXPECT_TRUE(settings->write(SettingSource::kProp, key, "0"));
    EXPECT_EQ(prop("persist.gammaos.nano.nav_sounds"), "0");           // in the store, as is
    EXPECT_EQ(settings->read(SettingSource::kProp, key, "1"), "0");
    // The scraper's settings and Nano's language are Nano's own too.
    EXPECT_TRUE(settings->write(SettingSource::kProp, "persist.gammaos.scraper.region", "eu"));
    EXPECT_EQ(settings->read(SettingSource::kProp, "persist.gammaos.scraper.region", "us"), "eu");
    EXPECT_TRUE(settings->supports(SettingSource::kProp, "persist.sys.locale"));
}

TEST_F(LinuxSettingsTest, AValueTheStoreRefusesFails) {
    const std::string key = "persist.gammaos.nano.system_name";
    EXPECT_FALSE(settings->write(SettingSource::kProp, key, std::string(200, 'x')));   // over 91 bytes
    EXPECT_EQ(settings->read(SettingSource::kProp, key, "none"), "none");
}

TEST_F(LinuxSettingsTest, AndroidSettingsTablesAreUnsupported) {
    struct Row {
        SettingSource source;
        const char* key;
        const char* def;
    };
    const Row rows[] = {
        {SettingSource::kGlobal, "airplane_mode_on", "0"},
        {SettingSource::kGlobal, "adb_enabled", "0"},
        {SettingSource::kSecure, "ui_night_mode", "1"},
        {SettingSource::kSecure, "location_mode", "0"},
        {SettingSource::kSystem, "accelerometer_rotation", "0"},
        {SettingSource::kSystem, "time_12_24", "12"},
    };
    for (const Row& r : rows) {
        EXPECT_FALSE(settings->supports(r.source, r.key)) << r.key;
        EXPECT_EQ(settings->read(r.source, r.key, r.def), r.def) << r.key;
        EXPECT_FALSE(settings->write(r.source, r.key, "1")) << r.key;
        EXPECT_FALSE(settings->write(r.source, r.key, "1")) << r.key;   // again: still refused, logged once
        EXPECT_EQ(prop(r.key), "<unset>") << r.key;                      // nothing stored under any name
    }
    // The same names as properties are not Nano's preferences either.
    EXPECT_FALSE(settings->supports(SettingSource::kProp, "font_scale"));
    EXPECT_FALSE(settings->supports(SettingSource::kNone, "persist.gammaos.nano.nav_sounds"));
}

TEST_F(LinuxSettingsTest, SystemSettingsLinuxImplementsAreKeptAsNanoReadsThem) {
    EXPECT_TRUE(settings->supports(SettingSource::kSystem, "font_scale"));
    EXPECT_EQ(settings->read(SettingSource::kSystem, "font_scale", "1.0"), "1.0");
    EXPECT_TRUE(settings->write(SettingSource::kSystem, "font_scale", "1.15"));
    EXPECT_EQ(prop("persist.gammaos.nano.fontscale"), "1.15");   // what the render loop applies
    EXPECT_EQ(settings->read(SettingSource::kSystem, "font_scale", "1.0"), "1.15");
    EXPECT_EQ(prop("font_scale"), "<unset>");
    EXPECT_FALSE(settings->supports(SettingSource::kGlobal, "font_scale"));   // only the system table's

    EXPECT_TRUE(settings->supports(SettingSource::kSystem, "screen_off_timeout"));
    EXPECT_TRUE(settings->write(SettingSource::kSystem, "screen_off_timeout", "300000"));
    EXPECT_EQ(settings->read(SettingSource::kSystem, "screen_off_timeout", "60000"), "300000");
    EXPECT_EQ(nanolinux::settings::nativeStoreKey(SettingSource::kSystem, "screen_off_timeout"),
              "persist.thimsy.system.screen_off_timeout");
    EXPECT_EQ(nanolinux::settings::nativeStoreKey(SettingSource::kProp, "font_scale"), "");
}

TEST_F(LinuxSettingsTest, UnknownKeysFailClosed) {
    const char* unknown[] = {
        "persist.gammaos.nano.a_row_added_upstream",   // named like Nano's, but not in the table
        "persist.example.anything",
        "sys.gammaos.nano.factory_reset",
    };
    for (const char* key : unknown) {
        EXPECT_FALSE(settings->supports(SettingSource::kProp, key)) << key;
        EXPECT_EQ(settings->read(SettingSource::kProp, key, "def"), "def") << key;
        EXPECT_FALSE(settings->write(SettingSource::kProp, key, "1")) << key;
        EXPECT_EQ(prop(key), "<unset>") << key;
    }
    EXPECT_FALSE(settings->supports(SettingSource::kProp, ""));
    EXPECT_FALSE(settings->write(SettingSource::kProp, "", "1"));
    EXPECT_EQ(settings->read(SettingSource::kProp, "", "def"), "def");
}

TEST_F(LinuxSettingsTest, KeysAnotherAndroidProcessActsOnAreUnsupported) {
    const char* others[] = {
        "persist.gammaos.nano.orientation",          // WindowManager (and accelerometer_rotation)
        "persist.gammaos.nano.display.saturation",   // Android's colour matrix (cmd color_display)
        "persist.gammaos.nano.display.reading",      // LiveDisplay, through the framework bridge
        "persist.gammaos.nano.screencal.top.red",    // the RK356x display controller and composer
        "persist.gammaos.nano.mode_quick_settings",  // PhoneWindowManager
        "persist.gammaos.gamepad.abxy_swap",         // the gammapad daemon
        "persist.gammaos.shader.enable",             // SurfaceFlinger's filters
        "persist.gammaos.rotate.enabled",            // WindowManager
        "persist.gammaos.hotkeys.enabled",           // PhoneWindowManager and drastic-nano
        "persist.gammaos.rgb.effect",                // the RGB light service
        "ro.build.version.release",                  // read-only, and Android's version
    };
    for (const char* key : others) {
        EXPECT_FALSE(settings->supports(SettingSource::kProp, key)) << key;
        EXPECT_FALSE(settings->write(SettingSource::kProp, key, "1")) << key;
        EXPECT_EQ(prop(key), "<unset>") << key;   // never stored as if it had worked
    }
    // No gamepad config_version bump either: the firmware's hook is the firmware's.
    EXPECT_EQ(prop("persist.gammaos.gamepad.config_version"), "<unset>");
}

TEST_F(LinuxSettingsTest, DeviceNameIsReadNotWritten) {
    ASSERT_EQ(property_set("ro.product.model", "RG ARC-D"), 0);   // as thimsy-prop boot writes it
    EXPECT_TRUE(nanolinux::settings::isReadOnlySetting(SettingSource::kProp, "ro.product.model"));
    EXPECT_TRUE(settings->supports(SettingSource::kProp, "ro.product.model"));   // About's row shows
    EXPECT_EQ(settings->read(SettingSource::kProp, "ro.product.model", ""), "RG ARC-D");
    EXPECT_FALSE(settings->write(SettingSource::kProp, "ro.product.model", "other"));
    EXPECT_EQ(prop("ro.product.model"), "RG ARC-D");
    // Only as a property: no system table holds it.
    EXPECT_FALSE(nanolinux::settings::isReadOnlySetting(SettingSource::kSystem, "ro.product.model"));
    EXPECT_FALSE(settings->supports(SettingSource::kSystem, "ro.product.model"));
    // The About rows Linux does not provide stay hidden.
    const char* hidden[] = {"ro.build.version.release", "ro.build.display.id", "ro.lineage.version",
                            "ro.gammaos.kernel.version", "gsm.version.baseband"};
    for (const char* key : hidden) {
        EXPECT_FALSE(nanolinux::settings::isReadOnlySetting(SettingSource::kProp, key)) << key;
        EXPECT_FALSE(settings->supports(SettingSource::kProp, key)) << key;
        EXPECT_EQ(settings->read(SettingSource::kProp, key, "def"), "def") << key;
    }
}

TEST(LinuxSettingsTable, IsSortedUniqueAndNanosOwn) {
    const std::vector<std::string> keys = nanolinux::settings::nanoPreferences();
    ASSERT_FALSE(keys.empty());
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_TRUE(nanolinux::settings::isNanoPreference(keys[i])) << keys[i];
        if (i > 0) {
            EXPECT_LT(strcmp(keys[i - 1].c_str(), keys[i].c_str()), 0) << keys[i];
        }
        const bool nanoFamily = keys[i].rfind("persist.gammaos.nano.", 0) == 0 ||
                                keys[i].rfind("persist.gammaos.scraper.", 0) == 0 || keys[i] == "persist.sys.locale";
        EXPECT_TRUE(nanoFamily) << keys[i];
    }
}

TEST(LinuxCapabilities, ClaimsNothingLinuxDoesNotImplement) {
    std::unique_ptr<nanoplat::Capabilities> caps = nanolinux::caps::create();
    ASSERT_TRUE(caps != nullptr);
    for (int i = 0; i < static_cast<int>(nanoplat::Cap::Count); ++i)
        EXPECT_FALSE(caps->has(static_cast<nanoplat::Cap>(i))) << "Cap " << i;
    EXPECT_FALSE(caps->has(nanoplat::Cap::FactoryReset));
    EXPECT_FALSE(caps->has(nanoplat::Cap::RebootRecovery));
    EXPECT_FALSE(caps->has(nanoplat::Cap::OtaUpdate));   // OA-16
}

}  // namespace
