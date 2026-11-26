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

// The firmware's nanoplat::SystemActions (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6,
// G7-power): the Wi-Fi, Bluetooth, time-format and time-zone commands of NanoMenuSettings.cpp and
// NanoMenuSetupWizard.cpp, moved verbatim, with those files' LOG_TAG. Each method's parameters carry
// the names the moved lines used (on, savedNetId, mac, h12, tz), so the lines are the same. The
// commands still go through NanoMenuSettings.cpp's runCmd (its lock and its timeout) and
// nanoNetBridgeCmd, declared here as NanoSdMove.cpp declares the bridge's.

#define LOG_TAG "GammaOSNano"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "NanoPlatform.h"
#include "SystemActions.h"

namespace android {

bool nanoNetBridgeCmd(const char* verb, const std::string& arg);    // NanoMenuSettings.cpp
std::string runCmd(const std::string& cmdline, int timeoutSecs = 3);  // NanoMenuSettings.cpp

namespace {

class FirmwareSystemActions : public nanoplat::SystemActions {
 public:
    void setWifiEnabled(bool on) override {
        if (!nanoNetBridgeCmd(on ? "wifi_enable" : "wifi_disable", ""))
            (void)runCmd(on ? "cmd wifi set-wifi-enabled enabled"
                            : "cmd wifi set-wifi-enabled disabled");
    }

    void forgetWifiNetwork(int savedNetId) override {
        // Always take the root-gated shell path. The old code gated it behind
        // nanoNetBridgeCmd("wifi_forget"), but that helper is FIRE-AND-FORGET (it just
        // writes sys.gammaos.nano.net_cmd and returns true whenever the bridge is up,
        // never confirming the op), so when the bridge was up the reliable shell path
        // never ran and the whole thing hinged on the bridge handler - which used the
        // deprecated, always-false WifiManager.removeNetwork()+saveConfiguration() pair
        // and silently no-op'd, so "Forget This Network" did nothing. `cmd wifi
        // forget-network` runs from nano's root domain (gammaos-nano.rc user root, which
        // clears WifiShellCommand's ROOT_UID gate) and calls the modern working
        // mWifiService.forget(netId, listener). A WPA2/WPA3 transition profile is a single
        // WifiConfiguration under one netId, so forgetting that id removes it outright.
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "cmd wifi forget-network %d 2>&1", savedNetId);
        (void)runCmd(cmd);
    }

    void setBluetoothEnabled(bool on) override {
        if (!nanoNetBridgeCmd(on ? "bt_enable" : "bt_disable", ""))
            (void)runCmd(on ? "cmd bluetooth_manager enable"
                            : "cmd bluetooth_manager disable");
    }

    void connectBluetooth(const std::string& mac) override {
        // `cmd bluetooth_manager connect-profile <profile> <mac>` exists on
        // recent AOSP, but profile ids vary by HIDL version. Invoke a
        // profile-agnostic reconnect by toggling bonded-connect via
        // "cmd bluetooth_manager factory-reset"? No -- that wipes bonds.
        //
        // Practical compromise for now: issue a generic connect via
        // `cmd bluetooth_manager connect <mac>` (present on A13+) and
        // fall back to a disable+enable cycle if that subcommand is
        // missing. The radio auto-reconnects cached bonded audio/input
        // devices on re-enable.
        // Prefer the bridge: BluetoothDevice.connect() reconnects just this bonded device
        // (all profiles) without bouncing the radio and dropping every other connection.
        if (!nanoNetBridgeCmd("bt_connect", mac)) {
            // No AOSP CLI exposes direct connect; fall back to a radio toggle so
            // bonded audio/input devices re-pair on the next enable.
            (void)runCmd("cmd bluetooth_manager disable");
            usleep(400 * 1000);
            (void)runCmd("cmd bluetooth_manager enable");
        }
    }

    void setTimeFormat(bool h12) override {
        std::thread([h12] { system(h12 ? "settings put system time_12_24 12 2>/dev/null"
                                       : "settings put system time_12_24 24 2>/dev/null"); }).detach();
    }

    void setTimeZone(const std::string& tz) override {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "setprop persist.sys.timezone %s", tz.c_str());
        system(cmd);
    }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::SystemActions> nanofirmware::createSystemActions() {
    return std::unique_ptr<nanoplat::SystemActions>(new android::FirmwareSystemActions);
}
