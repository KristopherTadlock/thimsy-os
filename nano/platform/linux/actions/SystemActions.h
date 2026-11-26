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

// The Linux platform's nanoplat::SystemActions (docs/linux/IMPLEMENTATION.md B.6, G7-power):
//
// - setWifiEnabled: persist.thimsy.wifi.enabled (1 or 0), which D30net reads at boot, then
//   `sh /etc/init.d/deferred/D30net start` or `stop` (wpa_supplicant on the first wlan interface,
//   with its control socket in /run/wpa_supplicant, and udhcpc).
// - forgetWifiNetwork: REMOVE_NETWORK <id> and SAVE_CONFIG on wpa_supplicant's control socket
//   (WpaCtrl.h); the id is wpa_supplicant's own network id.
// - setBluetoothEnabled: `bluetoothctl power on|off`; connectBluetooth: `bluetoothctl connect <mac>`,
//   run on demand (no daemon of Nano's own). An image without BlueZ logs that once per call.
// - setTimeFormat: nothing. persist.gammaos.nano.clock12, which Nano wrote just before, is the
//   only clock format on Linux.
// - setTimeZone: the zone's file must exist under /usr/share/zoneinfo; then the config's localtime
//   link (/etc/localtime links to /storage/.config/thimsy/localtime, a link Nano rewrites: the root
//   is read-only) is replaced atomically, persist.sys.timezone is set (Nano reads the zone back from
//   it, as on Android), and Nano's own process follows the new zone (TZ and tzset()).
//
// The Wi-Fi, forget and Bluetooth commands take seconds, and Nano calls them from its render thread
// (a menu row's press), so they run on a worker thread of their own, one at a time in the order
// asked (Options::background); persist.thimsy.wifi.enabled is written at once. The time zone is set
// on the caller's thread: it is quick, and Nano reads the zone back right after.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"
#include "Run.h"

namespace nanolinux {
namespace actions {

struct Options {
    std::string root;                                  // prefix for every path ("" on the device)
    std::string configDir = "/storage/.config/thimsy";  // holds the localtime link
    std::string zoneinfo = "/usr/share/zoneinfo";
    std::string wpaDir = "/run/wpa_supplicant";        // D30net's ctrl_interface
    std::string wpaLocalDir = "/tmp";                  // where this client binds its socket
    std::string netScript = "/etc/init.d/deferred/D30net";
    // Runs a program; empty: runProgram (Run.h).
    std::function<RunResult(const std::vector<std::string>& argv, int timeoutMs)> run;
    // Makes this process follow the zone just set; empty: setenv("TZ", zone) and tzset().
    std::function<void(const std::string& zone)> followZone;
    // The Wi-Fi script, the forget and bluetoothctl run on the actions' own worker thread, one at a
    // time in the order asked, so the caller (Nano's render thread, for a menu press) never waits on
    // them; false runs them on the caller's thread (tests).
    bool background = true;
};

// An Olson zone id Nano may name ("Europe/London", "Etc/GMT+5"): letters, digits, '/', '_', '-',
// '+', no empty, "." or ".." component, at most 64 bytes.
bool validZoneId(const std::string& id);

// "AA:BB:CC:DD:EE:FF" (either case).
bool validMac(const std::string& mac);

std::unique_ptr<nanoplat::SystemActions> create(Options options = Options());

}  // namespace actions
}  // namespace nanolinux
