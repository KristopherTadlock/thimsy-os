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

// The Linux nanoplat::Settings (Settings.h).

#define LOG_TAG "ThimsySettings"

#include "Settings.h"

#include <string.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

namespace nanolinux {
namespace settings {

namespace {

using nanoplat::SettingSource;

// Nano's own preferences that settings rows or the dispatcher's callers read and write: their only
// reader is Nano itself (docs/linux/audit-2026-11-26/inv_props.md class 1a, checked against each
// key's readers). Sorted by strcmp, for the binary search (settings_test checks it). Left out on
// purpose, though named like Nano's: persist.gammaos.nano.orientation and orient_mode
// (WindowManager), ctrl_screen (inputflinger), mode_quick_settings (PhoneWindowManager),
// display.reading and display.cal_r/g/b (LiveDisplay, through the framework bridge),
// display.saturation (Android's colour matrix), screencal.* (the RK356x display controller,
// baseparameter and the Rockchip composer), browser_pkg (an Android app), emu_sd (moves Android
// emulators' files), pspclock.freezeapp and pspclock.liveapp (the app under the clock).
constexpr const char* kNanoPreferences[] = {
    "persist.gammaos.nano.battpct",
    "persist.gammaos.nano.boot_sound",
    "persist.gammaos.nano.boot_to",
    "persist.gammaos.nano.brightness",
    "persist.gammaos.nano.cc.doubletapwake",
    "persist.gammaos.nano.cc.sleeptimeout",
    "persist.gammaos.nano.esde.settingstheme",
    "persist.gammaos.nano.esde.videoaudio",
    "persist.gammaos.nano.face_glyphs",
    "persist.gammaos.nano.face_swap",
    "persist.gammaos.nano.iptv",
    "persist.gammaos.nano.iptv.url",
    "persist.gammaos.nano.kiosk",
    "persist.gammaos.nano.minima.bg",
    "persist.gammaos.nano.minima.namescroll",
    "persist.gammaos.nano.nav_sounds",
    "persist.gammaos.nano.nds.ambiance",
    "persist.gammaos.nano.nds.dark",
    "persist.gammaos.nano.nds.romtitle",
    "persist.gammaos.nano.nds.tileicon",
    "persist.gammaos.nano.ndstheme",
    "persist.gammaos.nano.ndstheme.gap",
    "persist.gammaos.nano.ndstheme.stack",
    "persist.gammaos.nano.ps3xmb.bottomclock",
    "persist.gammaos.nano.ps3xmb.bottomclock.fps",
    "persist.gammaos.nano.ps3xmb.halfres.clock",
    "persist.gammaos.nano.ps3xmb.halfres.wave",
    "persist.gammaos.nano.ps3xmb.wave",
    "persist.gammaos.nano.pspclock",
    "persist.gammaos.nano.pspclock.parallax",
    "persist.gammaos.nano.pspclock.parallax.dir",
    "persist.gammaos.nano.pspclock.parallax.strength",
    "persist.gammaos.nano.quick_resume",
    "persist.gammaos.nano.radio",
    "persist.gammaos.nano.radio.url",
    "persist.gammaos.nano.rom.m3u_group",
    "persist.gammaos.nano.rom.recursive",
    "persist.gammaos.nano.rom.show_display_names",
    "persist.gammaos.nano.search_engine",
    "persist.gammaos.nano.system_name",
    "persist.gammaos.nano.touch_input",
    "persist.gammaos.nano.wp.scrim",
    "persist.gammaos.scraper.boxart",
    "persist.gammaos.scraper.engine",
    "persist.gammaos.scraper.fanart",
    "persist.gammaos.scraper.media.3dbox",
    "persist.gammaos.scraper.media.backcover",
    "persist.gammaos.scraper.media.manual",
    "persist.gammaos.scraper.media.marquee",
    "persist.gammaos.scraper.media.miximage",
    "persist.gammaos.scraper.media.physicalmedia",
    "persist.gammaos.scraper.media.screenshot",
    "persist.gammaos.scraper.media.titlescreen",
    "persist.gammaos.scraper.media.video",
    "persist.gammaos.scraper.overwrite",
    "persist.gammaos.scraper.region",
    "persist.gammaos.scraper.sspass",
    "persist.gammaos.scraper.ssuser",
    "persist.gammaos.scraper.tgdbkey",
    // Nano's language: Nano's strings, theme engine and BIOS installer read it (NanoMenuStrings.cpp).
    "persist.sys.locale",
};

// Read-only system state a settings row shows and Linux provides: About Device > Device Name's
// ro.product.model, which thimsy-prop boot writes from the device's description (libthimsy). Read
// through the store; a write is refused. The other About rows name what Linux does not have
// (ro.build.version.release, ro.build.display.id, ro.lineage.version, ro.gammaos.kernel.version,
// gsm.version.baseband: Android's and LineageOS's versions, the firmware's build and kernel strings,
// the modem's), so they stay hidden.
constexpr const char* kReadOnly[] = {
    "ro.product.model",
};

// The system settings Linux implements itself, each kept as a property Nano reads.
struct NativeSetting {
    SettingSource source;
    const char* key;
    const char* storeKey;
};
constexpr NativeSetting kNative[] = {
    // Font Size: the render loop reads persist.gammaos.nano.fontscale every frame and scales every
    // theme's text (on the firmware, the Font Size hook mirrors Android's font_scale into it).
    {SettingSource::kSystem, "font_scale", "persist.gammaos.nano.fontscale"},
    // Screen Timeout: Nano's own idle sleep reads it through the dispatcher (NanoMenuInput.cpp) and
    // asks the session to sleep (dosleep, which the control keys route).
    {SettingSource::kSystem, "screen_off_timeout", "persist.thimsy.system.screen_off_timeout"},
};

// The radio switches, Android's global wifi_on and bluetooth_on: the Quick Menu's Wi-Fi and
// Bluetooth rows are bound to them. Linux implements both itself (actions/SystemActions.h). Each
// switch is kept in a key of its own, which D30net and D35bluetooth act on at boot; a write is
// SystemActions::setWifiEnabled or setBluetoothEnabled, the action Nano's own Wi-Fi and Bluetooth
// switches take (the key at once, the script on the actions' worker), never Android's table. A read
// is the switch's state in that key ("1" on; "0" off or unset, as both scripts default off), not the
// radio's live state: that needs wpa_supplicant or bluetoothctl and can block, and the menus read
// these on the render thread. Bluetooth only on a device whose description says it has Bluetooth.
struct RadioSetting {
    const char* key;
    const char* switchKey;
    bool bluetooth;
};
constexpr RadioSetting kRadios[] = {
    {"wifi_on", "persist.thimsy.wifi.enabled", false},
    {"bluetooth_on", "persist.thimsy.bluetooth.enabled", true},
};
// device.json's "bluetooth" (thimsy-prop boot), as Network.cpp's btDaemonsUp reads it.
constexpr const char kDeviceBluetoothKey[] = "ro.thimsy.device.bluetooth";

bool lessCStr(const char* a, const char* b) {
    return strcmp(a, b) < 0;
}

bool propIs(const char* key, const char* want) {
    char buf[PROPERTY_VALUE_MAX] = {};
    property_get(key, buf, "");
    return strcmp(buf, want) == 0;
}

// The radio switch (source, key) names, or nullptr: only Android's global table holds them.
const RadioSetting* radioFor(SettingSource source, const std::string& key) {
    if (source != SettingSource::kGlobal) return nullptr;
    for (const RadioSetting& r : kRadios) {
        if (key == r.key) return &r;
    }
    return nullptr;
}

const char* sourceName(SettingSource source) {
    switch (source) {
    case SettingSource::kNone:
        return "none";
    case SettingSource::kProp:
        return "prop";
    case SettingSource::kGlobal:
        return "global";
    case SettingSource::kSecure:
        return "secure";
    case SettingSource::kSystem:
        return "system";
    }
    return "?";
}

// The property (source, key) is kept in, or "" when Linux does not support it.
std::string storeKeyFor(SettingSource source, const std::string& key) {
    if (key.empty()) return "";
    if (source == SettingSource::kProp)
        return (isNanoPreference(key) || isReadOnlySetting(source, key)) ? key : std::string();
    return nativeStoreKey(source, key);
}

class LinuxSettings : public nanoplat::Settings {
 public:
    explicit LinuxSettings(nanoplat::SystemActions* radios) : radios_(radios) {}

    bool supports(SettingSource source, const std::string& key) const override {
        return radio(source, key) != nullptr || !storeKeyFor(source, key).empty();
    }

    std::string read(SettingSource source, const std::string& key, const std::string& def) override {
        if (const RadioSetting* r = radio(source, key)) return propIs(r->switchKey, "1") ? "1" : "0";
        const std::string store = storeKeyFor(source, key);
        if (store.empty()) return def;
        char buf[PROPERTY_VALUE_MAX] = {};
        property_get(store.c_str(), buf, def.c_str());
        return buf;
    }

    bool write(SettingSource source, const std::string& key, const std::string& value) override {
        if (isReadOnlySetting(source, key)) {
            refusedOnce(source, key, "is read-only");
            return false;
        }
        if (const RadioSetting* r = radio(source, key)) return switchRadio(*r, value);
        const std::string store = storeKeyFor(source, key);
        if (store.empty()) {
            refusedOnce(source, key, "is not supported on Linux");
            return false;
        }
        if (property_set(store.c_str(), value.c_str()) != 0) {
            ALOGW("settings: %s %s: the store refused the value (%zu bytes)", sourceName(source), key.c_str(),
                  value.size());
            return false;
        }
        return true;
    }

 private:
    // The radio switch this platform can turn for (source, key), or nullptr: none without the system
    // actions, and Bluetooth only on a device that has it.
    const RadioSetting* radio(SettingSource source, const std::string& key) const {
        const RadioSetting* r = radioFor(source, key);
        if (r == nullptr || radios_ == nullptr) return nullptr;
        if (r->bluetooth && !propIs(kDeviceBluetoothKey, "true")) return nullptr;
        return r;
    }

    // "1" or "true" turns the radio on, "0" or "false" off; any other value is refused. A switch
    // already in that state is left alone: D30net start is not idempotent (a second wpa_supplicant
    // fails on the control socket and a second udhcpc starts).
    bool switchRadio(const RadioSetting& r, const std::string& value) {
        bool on = false;
        if (value == "1" || value == "true") {
            on = true;
        } else if (value == "0" || value == "false") {
            on = false;
        } else {
            ALOGW("settings: global %s: not an on or off value (%zu bytes); the write does nothing", r.key,
                  value.size());
            return false;
        }
        if (propIs(r.switchKey, "1") == on) return true;
        if (r.bluetooth) {
            radios_->setBluetoothEnabled(on);
        } else {
            radios_->setWifiEnabled(on);
        }
        return true;
    }

    void refusedOnce(SettingSource source, const std::string& key, const char* why) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!refused_.insert(std::string(sourceName(source)) + " " + key).second) return;
        ALOGW("settings: %s %s %s; the write does nothing", sourceName(source), key.c_str(), why);
    }

    nanoplat::SystemActions* const radios_;  // the platform's; may be null (then no radio is supported)
    std::mutex mu_;
    std::set<std::string> refused_;  // guarded by mu_: the settings already logged
};

}  // namespace

bool isNanoPreference(const std::string& key) {
    return std::binary_search(std::begin(kNanoPreferences), std::end(kNanoPreferences), key.c_str(), lessCStr);
}

std::vector<std::string> nanoPreferences() {
    return std::vector<std::string>(std::begin(kNanoPreferences), std::end(kNanoPreferences));
}

bool isReadOnlySetting(nanoplat::SettingSource source, const std::string& key) {
    if (source != nanoplat::SettingSource::kProp) return false;
    for (const char* k : kReadOnly) {
        if (key == k) return true;
    }
    return false;
}

std::string nativeStoreKey(nanoplat::SettingSource source, const std::string& key) {
    for (const NativeSetting& n : kNative) {
        if (n.source == source && key == n.key) return n.storeKey;
    }
    return "";
}

std::unique_ptr<nanoplat::Settings> create(nanoplat::SystemActions* radios) {
    return std::unique_ptr<nanoplat::Settings>(new LinuxSettings(radios));
}

}  // namespace settings
}  // namespace nanolinux
