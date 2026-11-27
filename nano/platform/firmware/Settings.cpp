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

// The firmware's nanoplat::Settings (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 3, the
// settings-dispatcher seam): NanoMenuSettingsTree.cpp's readSettingValue and writeSettingValue,
// moved verbatim with their post-write hooks (the Font Size mirror, Screen Calibration, the display
// saturation, the orientation's auto-rotate coupling and the gamepad config_version bump). The
// Parallax recomposition stays in NanoMenuSettingsTree.cpp's writeSettingValue, after this write: it
// is Nano's own model (one Nano preference recomposed from three), the same on every platform.
// Every setting is supported, and a write always reports success, as before the move.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <cutils/properties.h>

#include "NanoLaunch.h"   // shellQuote
#include "NanoPlatform.h"
#include "NanoScreenCal.h"
#include "Settings.h"

namespace android {
namespace {

using nanoplat::SettingSource;

std::string readSettingValue(SettingSource src, const std::string& key,
                             const std::string& def) {
    if (key.empty()) return def;
    switch (src) {
    case SettingSource::kProp: {
        char buf[PROPERTY_VALUE_MAX] = {};
        property_get(key.c_str(), buf, def.c_str());
        return buf;
    }
    case SettingSource::kGlobal:
    case SettingSource::kSecure:
    case SettingSource::kSystem: {
        const char* ns = (src == SettingSource::kGlobal) ? "global"
                       : (src == SettingSource::kSecure) ? "secure" : "system";
        const std::string cmd = std::string("settings get ") + ns + " " + nanolaunch::shellQuote(key) +
                                " 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return def;
        char buf[256] = {};
        if (fgets(buf, sizeof(buf), f)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
                buf[--len] = '\0';
        }
        pclose(f);
        std::string val(buf);
        if (val.empty() || val == "null") return def;
        return val;
    }
    default:
        return def;
    }
}

void writeSettingValue(SettingSource src, const std::string& key,
                       const std::string& val) {
    if (key.empty()) return;
    switch (src) {
    case SettingSource::kProp:
        property_set(key.c_str(), val.c_str());
        break;
    case SettingSource::kGlobal:
    case SettingSource::kSecure:
    case SettingSource::kSystem: {
        const char* ns = (src == SettingSource::kGlobal) ? "global"
                       : (src == SettingSource::kSecure) ? "secure" : "system";
        const std::string cmd = std::string("settings put ") + ns + " " + nanolaunch::shellQuote(key) +
                                " " + nanolaunch::shellQuote(val) + " 2>/dev/null";
        (void)system(cmd.c_str());
        break;
    }
    default:
        break;
    }
    // Font Size: the System font_scale setting was a no-op in nano (nano never read it back). Mirror
    // it to a fast prop the render loop reads every frame (property_get) and applies as a global text
    // scale across all themes. The Font Size row exists in both the legacy tree and the PS3-XMB
    // binding; both write font_scale, so this single hook covers both front-ends.
    if (src == SettingSource::kSystem && key == "font_scale") {
        property_set("persist.gammaos.nano.fontscale", val.c_str());
    }
    // RG DS / RG DS Plus Screen Calibration: the display controller applies it, so every change
    // is pushed to the panels at once (and mirrored into baseparameter by the module).
    if (src == SettingSource::kProp && screencal::isCalibrationKey(key)) {
        screencal::applyFromProps();
    }
    // Display saturation rides the display colour matrix, which has no persisted setting of its
    // own, so applying it is an explicit call. nano re-applies the prop on boot.
    if (src == SettingSource::kProp && key == "persist.gammaos.nano.display.saturation") {
        int lvl = atoi(val.c_str());
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "cmd color_display set-saturation %d 2>/dev/null", lvl);
        (void)system(cmd);
    }
    // GammaOS Nano orientation: the Screen Orientation setting is the SINGLE control
    // for accelerometer_rotation (there is no separate Auto-Rotate toggle). "auto"
    // enables the sensor; any fixed orientation disables it so the nano force
    // (WMS mapOrientationRequest) engages. Recursion is safe - the inner write's key
    // is accelerometer_rotation, which does not match this guard.
    if (src == SettingSource::kProp && key == "persist.gammaos.nano.orientation") {
        writeSettingValue(SettingSource::kSystem, "accelerometer_rotation",
                          (val == "auto") ? "1" : "0");
    }
    // The gammapad daemon watches persist.gammaos.gamepad.config_version (it polls
    // it every ~1s) and does a lightweight transform reload when it changes. Every
    // SystemUI gamepad tile bumps it after writing a transform/calibration prop, so
    // mirror that here for ANY gamepad prop write or the change never applies live.
    // Do NOT touch full_reload (that forces a heavy device release/re-grab); the
    // lightweight bump is what the tiles do for the toggle/cycle path.
    if (src == SettingSource::kProp &&
        key.rfind("persist.gammaos.gamepad.", 0) == 0 &&
        key != "persist.gammaos.gamepad.config_version" &&
        key != "persist.gammaos.gamepad.full_reload") {
        char vb[PROPERTY_VALUE_MAX] = "0";
        property_get("persist.gammaos.gamepad.config_version", vb, "0");
        long v = strtol(vb, nullptr, 10);
        char nb[16]; snprintf(nb, sizeof(nb), "%ld", v + 1);
        property_set("persist.gammaos.gamepad.config_version", nb);
    }
}

class FirmwareSettings : public nanoplat::Settings {
 public:
    bool supports(SettingSource, const std::string&) const override { return true; }

    std::string read(SettingSource source, const std::string& key, const std::string& def) override {
        return readSettingValue(source, key, def);
    }

    bool write(SettingSource source, const std::string& key, const std::string& value) override {
        writeSettingValue(source, key, value);
        return true;
    }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::Settings> nanofirmware::createSettings() {
    return std::unique_ptr<nanoplat::Settings>(new android::FirmwareSettings);
}
