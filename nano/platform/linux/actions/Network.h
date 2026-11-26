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

// The Linux platform's nanoplat::Network (docs/linux/IMPLEMENTATION.md B.6, G7-net).
//
// Wi-Fi: wpa_supplicant's control socket (WpaCtrl.h), the first interface socket in D30net's
// /run/wpa_supplicant. No socket means wpa_supplicant is not running: the radio is off.
// - The state: STATUS (wpa_state, ssid, id, ip_address, address), SIGNAL_POLL's RSSI for the bars.
// - The lists: LIST_NETWORKS (and GET_NETWORK <id> key_mgmt for each saved network's security),
//   SCAN_RESULTS; their SSIDs are wpa_supplicant's printf-escaped text (decodeWpaText). SCAN asks
//   for a fresh scan.
// - Connecting a saved network: SELECT_NETWORK <id>, waiting for STATUS to show it COMPLETED, then
//   ENABLE_NETWORK all (SELECT_NETWORK disables the others).
// - Joining: the saved network with that SSID, else ADD_NETWORK; SET_NETWORK ssid (as hex), key_mgmt
//   and the key (psk, sae_password or wep_key0, quoted); SELECT_NETWORK; the wait (and one retry
//   when the key was not refused); ENABLE_NETWORK all and SAVE_CONFIG. A key is never logged.
// - A wrong password: the network is [TEMP-DISABLED] in LIST_NETWORKS (wpa_supplicant's response to
//   an authentication failure; it does not say which).
// - The addresses: STATUS's ip_address and address (else /sys/class/net/<if>/address); the default
//   gateway from /proc/net/route; the DNS servers from /etc/resolv.conf. "Validated" (the Internet
//   test) is an address plus a default route on the interface plus a nameserver: no probe is sent.
//
// Bluetooth: bluetoothctl (BlueZ), run on demand through the runner (Run.h), never a shell: show
// (Powered), devices Paired (paired-devices before BlueZ 5.65) checked by info <mac> (Name, Class,
// Paired, Connected), --timeout N scan on then devices for an inquiry, --agent NoInputNoOutput pair,
// trust and connect for a pairing, remove, connect, disconnect, power on|off. A PIN or the "--ask"
// prompt has no Linux path yet: pairing goes ahead without it (logged); inbound requests are not
// accepted (logged). When bluetoothctl is missing or does not answer (no bluetoothd), Bluetooth
// reads as unknown and bluetoothctl is not run again for 30 s.
//
// Every call runs on the caller's thread and is bounded: each control request by wpaReplyMs, each
// bluetoothctl by its own deadline (a few seconds; a scan its length plus 5 s; a pairing 60 s), and
// the waits for a connection by about 25 s. The calls the menu makes on its render or input thread
// (wifiRadioEnabled, wifiAddresses, wifiMacAndIp: one or two requests; unpairBt: bluetoothctl remove
// with a 4 s deadline) stay well under its 8 s watchdog. Any thread; no state is shared but the
// Bluetooth back-off.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"
#include "Run.h"

namespace nanolinux {
namespace net {

struct Options {
    std::string root;                              // prefix for every path ("" on the device)
    std::string wpaDir = "/run/wpa_supplicant";    // D30net's ctrl_interface
    std::string wpaLocalDir = "/tmp";              // where the client binds its socket
    std::string procRoute = "/proc/net/route";
    std::string resolvConf = "/etc/resolv.conf";
    std::string sysNet = "/sys/class/net";
    int wpaReplyMs = 3000;                         // each control request's deadline
    // Runs bluetoothctl; empty: actions::runProgram.
    std::function<actions::RunResult(const std::vector<std::string>& argv, int timeoutMs)> run;
    // The waits between looks at a connection in progress; empty: usleep.
    std::function<void(int ms)> sleep;
    // CLOCK_MONOTONIC milliseconds, for the Bluetooth back-off; empty: clock_gettime.
    std::function<long long()> nowMs;
};

// wpa_supplicant's printf-escaped text (printf_encode: \\ \" \n \r \t \e \xNN, octal) as bytes.
std::string decodeWpaText(const std::string& s);

// `s` as lower-case hex ("616263" for "abc"), as SET_NETWORK takes an SSID unquoted.
std::string hexOf(const std::string& s);

// The security of a scan result's flags ("[WPA2-PSK-CCMP][ESS]"): WifiNetEntry's 0-4.
int securityOfFlags(const std::string& flags);

// The security of a saved network's key_mgmt ("WPA-PSK", "SAE", "NONE"...), WEP when key_mgmt is
// NONE and the network has a WEP key.
int securityOfKeyMgmt(const std::string& keyMgmt, bool hasWepKey);

// A dBm level as the HUD's 0-4 bars (NanoMenuNet.cpp's thresholds).
int barsOfRssi(int rssi);

std::unique_ptr<nanoplat::Network> create(Options options = Options());

}  // namespace net
}  // namespace nanolinux
