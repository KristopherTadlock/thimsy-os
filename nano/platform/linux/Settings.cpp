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

bool lessCStr(const char* a, const char* b) {
    return strcmp(a, b) < 0;
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
    if (source == SettingSource::kProp) return isNanoPreference(key) ? key : std::string();
    return nativeStoreKey(source, key);
}

class LinuxSettings : public nanoplat::Settings {
 public:
    bool supports(SettingSource source, const std::string& key) const override {
        return !storeKeyFor(source, key).empty();
    }

    std::string read(SettingSource source, const std::string& key, const std::string& def) override {
        const std::string store = storeKeyFor(source, key);
        if (store.empty()) return def;
        char buf[PROPERTY_VALUE_MAX] = {};
        property_get(store.c_str(), buf, def.c_str());
        return buf;
    }

    bool write(SettingSource source, const std::string& key, const std::string& value) override {
        const std::string store = storeKeyFor(source, key);
        if (store.empty()) {
            refusedOnce(source, key);
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
    void refusedOnce(SettingSource source, const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!refused_.insert(std::string(sourceName(source)) + " " + key).second) return;
        ALOGW("settings: %s %s is not supported on Linux; the write does nothing", sourceName(source),
              key.c_str());
    }

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

std::string nativeStoreKey(nanoplat::SettingSource source, const std::string& key) {
    for (const NativeSetting& n : kNative) {
        if (n.source == source && key == n.key) return n.storeKey;
    }
    return "";
}

std::unique_ptr<nanoplat::Settings> create() {
    return std::unique_ptr<nanoplat::Settings>(new LinuxSettings);
}

}  // namespace settings
}  // namespace nanolinux
