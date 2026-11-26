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

// The firmware's nanoplat::Network (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6, G7-net): the
// Wi-Fi and Bluetooth commands of NanoMenuSettings.cpp's screens and NanoMenuNet.cpp's poll, and the
// parsers of their output, moved verbatim with those files' LOG_TAG. The parsers build the
// platform's entries (nanoplat::WifiNetEntry, BtDevEntry), which have NanoMenu's fields. The
// Settings commands still go through NanoMenuSettings.cpp's runCmd and runCmdUnlocked (its lock
// and its timeouts), shellQuote and nanoNetBridgeCmd, declared here; the poll's through its own
// runCmdShellout, which moved here with it.

#define LOG_TAG "GammaOSNano"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

#include "NanoPlatform.h"
#include "Network.h"

namespace android {

bool nanoNetBridgeCmd(const char* verb, const std::string& arg);          // NanoMenuSettings.cpp
std::string runCmd(const std::string& cmdline, int timeoutSecs = 3);        // NanoMenuSettings.cpp
std::string runCmdUnlocked(const std::string& cmdline, int timeoutSecs);    // NanoMenuSettings.cpp
std::string shellQuote(const std::string& s);                               // NanoMenuSettings.cpp

namespace {

// popen() wrapper that caps total output at 16 KiB and total time at
// about 4 s so a hung service call can't freeze the worker thread or
// bloat memory. Returns the collected stdout; stderr is redirected
// into it because a lot of the `cmd` shell-outs mix warnings into
// stdout anyway.
std::string runCmdShellout(const char* command) {
    std::string out;
    out.reserve(1024);
    FILE* f = popen(command, "r");
    if (!f) return out;
    char buf[512];
    const size_t kMax = 16 * 1024;
    while (fgets(buf, sizeof(buf), f)) {
        out.append(buf);
        if (out.size() > kMax) {
            out.resize(kMax);
            break;
        }
    }
    pclose(f);
    return out;
}

bool strContainsCI(const std::string& hay, const char* needle) {
    size_t n = strlen(needle);
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); i++) {
        size_t k = 0;
        while (k < n
               && tolower((unsigned char)hay[i + k]) ==
                      tolower((unsigned char)needle[k])) {
            k++;
        }
        if (k == n) return true;
    }
    return false;
}

// Parse the SSID and RSSI out of the free-form `cmd wifi status` output.
// The relevant line looks like:
//   mWifiInfo SSID: <name>, BSSID: ..., MAC: ..., Security type: ...,
//       Supplicant state: COMPLETED, Rssi: -55, Link speed: ...
// SSID can contain commas, so we stop at ", BSSID:". Rssi is a signed
// integer; 0 or positive means uninitialised (treat as -127).
void parseWifiInfo(const std::string& text,
                   std::string* outSsid, int* outRssi) {
    outSsid->clear();
    *outRssi = -127;

    // SSID
    static const char* kSsidKey = "SSID:";
    size_t p = text.find(kSsidKey);
    while (p != std::string::npos) {
        size_t s = p + strlen(kSsidKey);
        while (s < text.size() && text[s] == ' ') s++;
        size_t e = text.find(", BSSID:", s);
        if (e == std::string::npos) e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string ssid = text.substr(s, e - s);
        // Strip surrounding quotes if present.
        if (ssid.size() >= 2 && ssid.front() == '"' && ssid.back() == '"') {
            ssid = ssid.substr(1, ssid.size() - 2);
        }
        if (!ssid.empty()
                && ssid != "<unknown ssid>"
                && ssid != "<none>") {
            *outSsid = ssid;
            break;
        }
        p = text.find(kSsidKey, p + 1);
    }

    // RSSI
    static const char* kRssiKey = "Rssi:";
    p = text.find(kRssiKey);
    if (p != std::string::npos) {
        size_t s = p + strlen(kRssiKey);
        while (s < text.size() && text[s] == ' ') s++;
        int rssi = atoi(text.c_str() + s);
        if (rssi < 0) *outRssi = rssi;
    }
}

// Map an RSSI in dBm to a 0..4 bars level. Matches the Android stock
// ScanResult.calculateSignalLevel thresholds closely enough for a HUD.
int rssiToBars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    if (rssi >= -88) return 1;
    return 0;
}


int securityFromFlags(const std::string& flags) {
    // Scan-result "flags" column looks like: [WPA2-PSK-CCMP][ESS] or [ESS] (open)
    // We just pick the strongest hint.
    std::string f = flags;
    for (auto& c : f) c = (char)toupper((unsigned char)c);
    if (f.find("WPA3") != std::string::npos
            || f.find("SAE") != std::string::npos) return 3;
    if (f.find("WPA2") != std::string::npos
            || f.find("WPA") != std::string::npos
            || f.find("PSK") != std::string::npos) return 2;
    if (f.find("WEP") != std::string::npos) return 1;
    if (f.find("OWE") != std::string::npos) return 4;
    return 0;
}

// Parse `cmd wifi list-networks` tabular output.
// Typical line:
//    Network Id  SSID                            Security type
//     0          MyHomeWiFi                      WPA2_PSK
std::vector<nanoplat::WifiNetEntry> parseSavedNetworks(
        const std::string& text) {
    std::vector<nanoplat::WifiNetEntry> out;
    size_t p = 0;
    bool skippedHeader = false;
    while (p < text.size()) {
        size_t eol = text.find('\n', p);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(p, eol - p);
        p = eol + 1;
        // Strip trailing \r
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;
        // Skip the "Network Id" header line.
        if (!skippedHeader) {
            if (line.find("Network Id") != std::string::npos
                    || line.find("No networks") != std::string::npos) {
                skippedHeader = true;
                continue;
            }
            skippedHeader = true;
        }
        // Try to read an integer id at the start.
        size_t i = 0;
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        if (i >= line.size() || !isdigit((unsigned char)line[i])) continue;
        int id = atoi(line.c_str() + i);
        while (i < line.size() && !isspace((unsigned char)line[i])) i++;
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        if (i >= line.size()) continue;
        // SSID can contain spaces; consume up to the last run of
        // non-space characters for the Security column.
        size_t secEnd = line.size();
        while (secEnd > 0 && !isspace((unsigned char)line[secEnd - 1])) secEnd--;
        // Now secEnd is the start of the security token.
        size_t secStart = secEnd;
        while (secEnd < line.size() && !isspace((unsigned char)line[secEnd])) secEnd++;
        std::string secTok = line.substr(secStart, secEnd - secStart);
        // SSID spans from i up to secStart with trailing spaces trimmed.
        size_t ssidEnd = secStart;
        while (ssidEnd > i && isspace((unsigned char)line[ssidEnd - 1])) ssidEnd--;
        std::string ssid = line.substr(i, ssidEnd - i);
        if (ssid.empty()) continue;

        nanoplat::WifiNetEntry e{};
        e.ssid = ssid;
        e.savedNetId = id;
        e.rssi = -127;
        std::string sec = secTok;
        for (auto& c : sec) c = (char)toupper((unsigned char)c);
        if (sec.find("SAE") != std::string::npos || sec == "WPA3_PSK") e.security = 3;
        else if (sec.find("WPA") != std::string::npos || sec.find("PSK") != std::string::npos) e.security = 2;
        else if (sec.find("WEP") != std::string::npos) e.security = 1;
        else if (sec.find("OWE") != std::string::npos) e.security = 4;
        else e.security = 0;
        e.connected = false;
        out.push_back(std::move(e));
    }
    return out;
}

// Parse `cmd wifi list-scan-results` column output.
// Header line:
//    BSSID   Frequency       RSSI    Age(sec)        SSID    Flags
std::vector<nanoplat::WifiNetEntry> parseScanResults(const std::string& text) {
    std::vector<nanoplat::WifiNetEntry> out;
    size_t p = 0;
    bool sawHeader = false;
    while (p < text.size()) {
        size_t eol = text.find('\n', p);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(p, eol - p);
        p = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;
        if (!sawHeader) {
            if (line.find("BSSID") != std::string::npos
                    && line.find("SSID") != std::string::npos) {
                sawHeader = true;
                continue;
            }
            // Allow parsing even without the header (some tools skip it).
        }
        // The SSID column can contain spaces (real examples: "EE WiFi",
        // "SkyBox-5-Downstairs", "The Wireless Signal is a Lie"). A naive
        // whitespace column-split takes only the first word as the SSID, so
        // nano stored (and tried to connect to) "The" -- an SSID no AP
        // broadcasts, which fails with "network not found" and reads to the
        // user as a wrong password. The output is fixed-width: the first four
        // fields (BSSID, Frequency, RSSI, Age) are always single tokens, and
        // the trailing Flags column always begins with '[' (e.g.
        // "[WPA2-PSK-CCMP][ESS]"). So read the four leading tokens, then take
        // everything up to the Flags as the space-preserving SSID.
        size_t i = 0;
        std::string toks[4];
        bool haveTok = true;
        for (int t = 0; t < 4 && haveTok; t++) {
            while (i < line.size() && isspace((unsigned char)line[i])) i++;
            if (i >= line.size()) { haveTok = false; break; }
            size_t j = i;
            while (j < line.size() && !isspace((unsigned char)line[j])) j++;
            toks[t] = line.substr(i, j - i);
            i = j;
        }
        if (!haveTok) continue;
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        std::string rest = line.substr(i);          // "<SSID padding> <Flags>"
        size_t fl = rest.find('[');                 // Flags always start with '['
        if (fl == std::string::npos) continue;      // no flags column -> not a result row
        std::string ssid  = rest.substr(0, fl);     // may be empty (hidden AP)
        std::string flags = rest.substr(fl);
        while (!ssid.empty() && isspace((unsigned char)ssid.back())) ssid.pop_back();
        if (ssid.size() >= 2 && ssid.front() == '"' && ssid.back() == '"')
            ssid = ssid.substr(1, ssid.size() - 2);  // strip quotes if the tool emitted them

        nanoplat::WifiNetEntry e{};
        e.bssid = toks[0];
        e.rssi = atoi(toks[2].c_str());
        if (e.rssi >= 0 || e.rssi < -120) e.rssi = -127;
        e.ssid = ssid;
        e.security = securityFromFlags(flags);
        e.savedNetId = -1;
        e.connected = false;
        if (!e.ssid.empty() && e.ssid != "<unknown ssid>") {
            out.push_back(std::move(e));
        }
    }
    return out;
}

// Extract currently-connected SSID from `cmd wifi status`.
std::string connectedSsidFromStatus(const std::string& text) {
    const char* key = "SSID:";
    size_t p = text.find(key);
    while (p != std::string::npos) {
        size_t s = p + strlen(key);
        while (s < text.size() && text[s] == ' ') s++;
        size_t e = text.find(", BSSID:", s);
        if (e == std::string::npos) e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string ssid = text.substr(s, e - s);
        if (ssid.size() >= 2 && ssid.front() == '"' && ssid.back() == '"')
            ssid = ssid.substr(1, ssid.size() - 2);
        if (!ssid.empty() && ssid != "<unknown ssid>" && ssid != "<none>")
            return ssid;
        p = text.find(key, p + 1);
    }
    return "";
}

// Parse `gammaos-net bt list-bonded` output. TSV lines:
//    AA:BB:CC:DD:EE:FF<TAB>Device Name<TAB>ClassOfDevice<TAB>Connected
// The Connected column is "1" if the ACL link is live (reflected via
// BluetoothDevice.isConnected()), "0" otherwise. Older output without
// the 4th column is accepted for forward-compat; connected defaults
// to false in that case.
std::vector<nanoplat::BtDevEntry> parseBondedDevices(const std::string& text) {
    std::vector<nanoplat::BtDevEntry> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t eol = text.find('\n', p);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(p, eol - p);
        p = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 17) continue;
        if (!isxdigit((unsigned char)line[0]) || !isxdigit((unsigned char)line[1]))
            continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        nanoplat::BtDevEntry d{};
        d.address = line.substr(0, t1);
        size_t t2 = line.find('\t', t1 + 1);
        size_t t3 = (t2 == std::string::npos) ? std::string::npos
                                              : line.find('\t', t2 + 1);
        if (t2 == std::string::npos) {
            d.name = line.substr(t1 + 1);
        } else {
            d.name = line.substr(t1 + 1, t2 - t1 - 1);
        }
        if (d.name.empty()) d.name = d.address;
        d.bonded = true;
        d.connected = false;
        // 3rd column = class-of-device, 4th = connected.
        if (t2 != std::string::npos) {
            const char* cp = line.c_str() + t2 + 1;
            char* ce = nullptr; long c = strtol(cp, &ce, 10);
            if (ce != cp) d.cod = (int)c;
        }
        if (t3 != std::string::npos) {
            const char* startp = line.c_str() + t3 + 1;
            char* endp = nullptr;
            long v = strtol(startp, &endp, 10);
            d.connected = (endp != startp) && v == 1;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// Parse `gammaos-net bt scan` output. TSV lines:
//    AA:BB:CC:DD:EE:FF<TAB>Name<TAB>RSSI<TAB>ClassOfDevice<TAB>BondState
// BondState is the android.bluetooth BOND_* int: 10=NONE, 11=BONDING, 12=BONDED.
std::vector<nanoplat::BtDevEntry> parseBtScanResults(const std::string& text) {
    std::vector<nanoplat::BtDevEntry> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t eol = text.find('\n', p);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(p, eol - p);
        p = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.size() < 17) continue;
        if (!isxdigit((unsigned char)line[0]) || !isxdigit((unsigned char)line[1]))
            continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        size_t t3 = (t2 == std::string::npos) ? std::string::npos
                                              : line.find('\t', t2 + 1);
        size_t t4 = (t3 == std::string::npos) ? std::string::npos
                                              : line.find('\t', t3 + 1);
        nanoplat::BtDevEntry d{};
        d.address = line.substr(0, t1);
        d.name = (t2 == std::string::npos) ? line.substr(t1 + 1)
                                           : line.substr(t1 + 1, t2 - t1 - 1);
        if (d.name.empty()) d.name = d.address;
        // 4th column = class-of-device (between t3 and t4).
        if (t3 != std::string::npos) {
            const char* cp = line.c_str() + t3 + 1;
            char* ce = nullptr; long c = strtol(cp, &ce, 10);
            if (ce != cp) d.cod = (int)c;
        }
        int bondState = 10; // BOND_NONE
        if (t4 != std::string::npos) {
            // -fno-exceptions in AOSP builds rules out std::stoi; strtol
            // silently returns 0 on a bad field, which we remap to BOND_NONE.
            const char* startp = line.c_str() + t4 + 1;
            char* endp = nullptr;
            long v = strtol(startp, &endp, 10);
            bondState = (endp != startp) ? static_cast<int>(v) : 10;
        }
        d.bonded = (bondState == 12); // BOND_BONDED
        d.connected = false;
        out.push_back(std::move(d));
    }
    return out;
}

// Scan `dumpsys wifi` for a wrong-password disable reason co-located with the
// given SSID. AOSP spells the reason a few ways across versions; match them all.
static bool wifiDumpWrongPassword(const std::string& dump, const std::string& ssid) {
    if (ssid.empty()) return false;
    const std::string q = "\"" + ssid + "\"";
    size_t p = dump.find(q);
    while (p != std::string::npos) {
        // Look within a window after the SSID (the per-network block that carries
        // the NetworkSelectionStatus / recent-failure fields).
        size_t win = 700;
        size_t nl = dump.find("\n\n", p);
        if (nl != std::string::npos && nl - p < win) win = nl - p;
        std::string block = dump.substr(p, win);
        if (block.find("WRONG_PASSWORD") != std::string::npos
         || block.find("WRONG_PSWD")     != std::string::npos
         || block.find("BY_WRONG_PASSWORD") != std::string::npos
         || block.find("DISABLED_AUTHENTICATION_FAILURE") != std::string::npos)
            return true;
        p = dump.find(q, p + q.size());
    }
    return false;
}

// Trim leading/trailing whitespace + newlines from shell output.
static std::string netTrim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'
                       || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) b++;
    return s.substr(b);
}

// Parse the WIFI network's LinkProperties out of `dumpsys connectivity` for the
// default-route gateway and DNS servers. The legacy dhcp.wlan0.gateway / net.dns1
// system properties are unset on modern Android (DHCP results live in
// ConnectivityService's LinkProperties), and direct ip/ip route do not work in
// nano's domain - but dumpsys (binder) does.
static void netGwDnsFromDumpsys(std::string& gw, std::string& dns1, std::string& dns2) {
    std::string d = runCmd("dumpsys connectivity 2>/dev/null");
    size_t at = d.find("InterfaceName: wlan0");
    if (at == std::string::npos) return;
    size_t end = d.find("}  nc{", at);
    if (end == std::string::npos) end = d.find("NetworkAgentInfo{", at + 1);
    if (end == std::string::npos) end = d.size();
    std::string b = d.substr(at, end - at);
    auto splitCsv = [](const std::string& s) {
        std::vector<std::string> out; std::string cur;
        for (char c : s) { if (c == ',') { out.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    // DnsAddresses: [ 8.8.8.8,8.8.4.4 ]   (comma-joined, each may have a leading '/')
    { size_t p = b.find("DnsAddresses: [");
      if (p != std::string::npos) { p += 15; size_t e = b.find(']', p);
        std::string inside = b.substr(p, (e == std::string::npos ? b.size() : e) - p);
        int idx = 0;
        for (auto& s : splitCsv(inside)) {
            std::string t = netTrim(s);
            if (!t.empty() && t[0] == '/') t = t.substr(1);
            if (t.empty()) continue;
            if (idx == 0) dns1 = t; else { dns2 = t; break; }
            idx++;
        }
      }
    }
    // Routes: [ 0.0.0.0/0 -> 192.168.0.1 wlan0 mtu 0, ... ]
    { size_t p = b.find("Routes: [");
      if (p != std::string::npos) { p += 9; size_t e = b.find(']', p);
        std::string inside = b.substr(p, (e == std::string::npos ? b.size() : e) - p);
        const std::string v4p = "0.0.0.0/0 -> ", v6p = "::/0 -> ";
        std::string v4gw, v6gw;
        for (auto& r : splitCsv(inside)) {
            std::string t = netTrim(r);
            if (t.rfind(v4p, 0) == 0) {
                std::string g = t.substr(v4p.size());
                g = g.substr(0, g.find(' '));
                if (g != "0.0.0.0" && !g.empty()) v4gw = g;
            } else if (t.rfind(v6p, 0) == 0) {
                std::string g = t.substr(v6p.size());
                g = g.substr(0, g.find(' '));
                if (g != "::" && !g.empty()) v6gw = g;
            }
        }
        gw = !v4gw.empty() ? v4gw : v6gw;
      }
    }
}

class FirmwareNetwork : public nanoplat::Network {
 public:
    // NanoMenuNet.cpp: netPollThreadFunc()'s start.
    void autoEnableWifi() override {
        // Auto-enable Wi-Fi if it's off and we have saved networks -- mirrors
        // the "remember last on/off" behaviour of stock Android. We only
        // try this once at thread start so a user who explicitly disabled
        // Wi-Fi from the settings screen isn't fought every 2 seconds.
        std::string status = runCmdShellout("cmd wifi status 2>/dev/null");
        bool wifiOff = status.empty()
                    || strContainsCI(status, "Wifi is disabled")
                    || strContainsCI(status, "is disabled");
        if (wifiOff) {
            std::string saved = runCmdShellout(
                    "cmd wifi list-networks 2>/dev/null");
            // The header line itself ends with "Security type"; we want
            // to know if there's at least one numeric network id row
            // after the header.
            bool anySaved = false;
            size_t p = 0;
            while (p < saved.size()) {
                size_t eol = saved.find('\n', p);
                if (eol == std::string::npos) eol = saved.size();
                size_t q = p;
                while (q < eol && isspace((unsigned char)saved[q])) q++;
                if (q < eol && isdigit((unsigned char)saved[q])) {
                    anySaved = true;
                    break;
                }
                p = eol + 1;
            }
            if (anySaved) {
                ALOGI("GammaOS Nano: auto-enabling Wi-Fi (saved networks present)");
                (void)runCmdShellout(
                        "cmd wifi set-wifi-enabled enabled 2>/dev/null");
            }
        }
    }

    // NanoMenuNet.cpp: netPollThreadFunc()'s shell fallback, when the bridge has not published.
    void pollState(WifiLevel& wifiLevel, int& wifiBars, std::string& wifiSsid, BtLevel& btLevel,
                   int& btConnected) override {
        // --- WiFi (shell fallback) --------------------------------
        std::string status = runCmdShellout("cmd wifi status 2>/dev/null");
        if (status.empty()) {
            // Service missing or binder refused. Leave as Unknown.
        } else if (strContainsCI(status, "Wifi is disabled")
                || strContainsCI(status, "is disabled")) {
            wifiLevel = kWifiLevel_Off;
        } else {
            std::string ssid;
            int rssi = -127;
            parseWifiInfo(status, &ssid, &rssi);
            if (!ssid.empty()) {
                wifiLevel = kWifiLevel_Connected;
                // Fall back to a mid-strength 2-bar hint if the
                // output didn't include a parsable Rssi line --
                // the connection itself is real (SSID proves it),
                // we just don't know how strong.
                wifiBars = (rssi > -127) ? rssiToBars(rssi) : 2;
                wifiSsid = ssid;
            } else {
                wifiLevel = kWifiLevel_Disconnected;
            }
        }

        // --- Bluetooth (shell fallback) ---------------------------
        // Read the REAL adapter state from dumpsys. `settings get global
        // bluetooth_on` is NOT reliable: `cmd bluetooth_manager enable/disable`
        // (which the radio toggle uses) does not update that setting, so it
        // stays stale. dumpsys reports the live "enabled: true/false" and is
        // also the source for the connected count, so one query covers both.
        std::string d = runCmdShellout("dumpsys bluetooth_manager 2>/dev/null");
        bool on = (d.find("enabled: true") != std::string::npos);
        if (!on) {
            btLevel = kBtLevel_Off;
        } else {
            btLevel = kBtLevel_On;
            // Count connected devices. The relevant lines look like:
            //       Connected = true
            // mapping onto audio + input + wearable profiles.
            size_t p = 0;
            while ((p = d.find("Connected = true", p)) != std::string::npos) {
                btConnected++;
                p += 16;
            }
            if (btConnected > 0) btLevel = kBtLevel_Connected;
        }
    }

    // refreshWifiList()'s shell path.
    WifiRadio wifiListRadio() override {
        std::string statusText = runCmd("cmd wifi status");
        // Detect whether the Wi-Fi radio is currently on. When it is off,
        // list-networks / list-scan-results return empty; we still want
        // the toggle row visible so the user can flip it back on.
        bool radioOn = !statusText.empty()
                    && statusText.find("Wifi is disabled") == std::string::npos
                    && statusText.find("is disabled") == std::string::npos;
        std::string connSsid = connectedSsidFromStatus(statusText);
        return {radioOn, connSsid};
    }

    WifiLists wifiLists() override {
        std::string savedText = runCmd("cmd wifi list-networks");
        std::string scanText = runCmd("cmd wifi list-scan-results");
        auto saved = parseSavedNetworks(savedText);
        auto scanned = parseScanResults(scanText);
        return {std::move(saved), std::move(scanned)};
    }

    // wifiScanThreadFunc().
    void startWifiScan() override {
        if (!nanoNetBridgeCmd("wifi_scan", "")) (void)runCmd("cmd wifi start-scan");
    }

    // connectToSavedWifi()'s worker.
    bool connectSavedWifi(int savedNetId) override {
        char cmd[96];
        snprintf(cmd, sizeof(cmd),
                 "gammaos-net wifi connect-saved %d", savedNetId);
        std::string result = runCmd(cmd, 30);   // association + DHCP can take >3s
        bool ok = result.find("OK") != std::string::npos;
        if (!ok) {
            ALOGW("connectToSavedWifi id=%d failed: %s",
                  savedNetId, result.c_str());
        }
        return ok;
    }

    // addAndConnectWifi(): the command, then its worker (which then refreshes the list).
    void joinWifi(const std::string& ssid, int security, const std::string& password, bool force) override {
        // cmd wifi connect-network SSID <security> <password>
        // Accepts: open, owe, wpa2, wpa3, wep per WifiShellCommand#connectNetwork.
        const char* secTok = "open";
        switch (security) {
        case 0: secTok = "open"; break;
        case 1: secTok = "wep";  break;
        case 2: secTok = "wpa2"; break;
        case 3: secTok = "wpa3"; break;
        case 4: secTok = "owe";  break;
        }
        std::string cmdline = "cmd wifi connect-network "
                            + shellQuote(ssid) + " " + secTok;
        if (security != 0 && security != 4) {
            cmdline += " " + shellQuote(password);
        }
        std::string ssidCapture = ssid;
        // Don't tear down a working link: if we are already associated with this
        // SSID, skip the (disruptive) reconnect. connect-network re-creates the
        // profile and re-associates, which briefly drops an otherwise-good
        // connection - and the wizard runs the connectivity test right after.
        // force==true (an explicit "Change Password") always overwrites, even
        // while connected, so a deliberate key change is not silently dropped.
        std::string st = runCmd("cmd wifi status 2>/dev/null");
        if (force || connectedSsidFromStatus(st) != ssidCapture) {
            (void)runCmd(cmdline, 30);   // cmd wifi connect-network blocks until the association resolves
        }
        // Verify the association instead of firing a scan right away. A scan kicked
        // while the supplicant is still associating can abort the association on a
        // flaky driver - exactly the first-boot setup-wizard window - which left the
        // user "saved" but never actually connected (the wizard would still show
        // "Save completed" and offer the connectivity test). Poll the live SSID for
        // ~12s; if nothing associated and it was not a wrong-password failure
        // (wifiConnectWatch owns that re-prompt), retry the connect once for the
        // flaky first-boot case, then re-poll. Refresh from cached results at the end
        // (no start-scan) so the (Connected) marker updates without disturbing the link.
        bool connected = false;
        for (int i = 0; i < 24; i++) {
            usleep(500 * 1000);
            std::string s = runCmd("cmd wifi status 2>/dev/null");
            if (connectedSsidFromStatus(s) == ssidCapture) { connected = true; break; }
        }
        if (!connected && !force) {
            std::string dump = runCmd("dumpsys wifi 2>/dev/null");
            if (!wifiDumpWrongPassword(dump, ssidCapture)) {
                (void)runCmd(cmdline, 30);   // cmd wifi connect-network blocks until the association resolves                      // silent no-association: one retry
                for (int i = 0; i < 20; i++) {
                    usleep(500 * 1000);
                    std::string s = runCmd("cmd wifi status 2>/dev/null");
                    if (connectedSsidFromStatus(s) == ssidCapture) break;
                }
            }
        }
    }

    // wifiConnectWatch(): one poll.
    JoinCheck checkWifiJoin(const std::string& ssid) override {
        std::string st = runCmd("cmd wifi status 2>/dev/null");
        if (connectedSsidFromStatus(st) == ssid) return JoinCheck::Connected;
        std::string dump = runCmd("dumpsys wifi 2>/dev/null");
        if (wifiDumpWrongPassword(dump, ssid)) return JoinCheck::WrongPassword;
        return JoinCheck::Pending;
    }

    bool wifiRadioEnabled() override {
        std::string st = runCmd("cmd wifi status 2>/dev/null");
        // "Wifi is enabled" / "Wifi is connected to ..." vs "Wifi is disabled".
        if (st.find("Wifi is disabled") != std::string::npos) return false;
        return st.find("Wifi is enabled") != std::string::npos
            || st.find("is connected to") != std::string::npos;
    }

    // buildNetStatusBody().
    WifiAddresses wifiAddresses(bool connected) override {
        // IP from the framework status (binder); gateway + DNS from the connectivity
        // dump's LinkProperties (the legacy dhcp.*/net.dns* props are unset). MAC via
        // sysfs. Direct ip/ip route do not work in nano (see startNetTest).
        std::string st = runCmd("cmd wifi status 2>/dev/null");
        std::string ip;
        { size_t p = st.find("IP: /");
          if (p != std::string::npos) { p += 5; size_t e = p;
            while (e < st.size() && (isdigit((unsigned char)st[e]) || st[e] == '.')) e++;
            ip = st.substr(p, e - p); if (ip == "0.0.0.0") ip.clear(); } }
        std::string gw, dns, dns2;
        if (connected) netGwDnsFromDumpsys(gw, dns, dns2);
        std::string mac = netTrim(runCmd("cat /sys/class/net/wlan0/address 2>/dev/null"));
        return {ip, gw, dns, dns2, mac};
    }

    // buildSysInfoBody().
    WifiMacAndIp wifiMacAndIp() override {
        // Wi-Fi MAC via sysfs; IPv4 from the framework status (binder), exactly like
        // buildNetStatusBody (direct ip/route do not work in nano's restricted runtime).
        std::string mac = netTrim(runCmd("cat /sys/class/net/wlan0/address 2>/dev/null"));
        std::string st  = runCmd("cmd wifi status 2>/dev/null");
        std::string ip;
        { size_t p = st.find("IP: /");
          if (p != std::string::npos) { p += 5; size_t e = p;
            while (e < st.size() && (isdigit((unsigned char)st[e]) || st[e] == '.')) e++;
            ip = st.substr(p, e - p); if (ip == "0.0.0.0") ip.clear(); } }
        return {mac, ip};
    }

    // startNetTest(): one poll.
    WifiLink wifiLink() override {
        // Single source of truth that works in nano's restricted runtime: the
        // framework "cmd wifi status" (binder). Direct ip/ping do NOT work here -
        // ping needs CAP_NET_RAW (nano's init capabilities allowlist omits it) and
        // the RTNETLINK read for `ip` is unavailable to the bootanim domain, so
        // both return empty and every test row read "Failed" even when online.
        // `cmd wifi status` reports the DHCP IP and Android's own INTERNET /
        // VALIDATED capability (VALIDATED = the OS confirmed reachability + DNS).
        auto ipFromStatus = [](const std::string& st) -> std::string {
            size_t p = st.find("IP: /");
            if (p == std::string::npos) return "";
            p += 5; size_t e = p;
            while (e < st.size() && (isdigit((unsigned char)st[e]) || st[e] == '.')) e++;
            std::string v = st.substr(p, e - p);
            return (v == "0.0.0.0") ? std::string() : v;
        };
        std::string ip, st;
        bool validated = false;
        st = runCmd("cmd wifi status 2>/dev/null");
        ip = ipFromStatus(st);
        validated = st.find("VALIDATED") != std::string::npos;
        return {ip, validated};
    }

    // refreshBtList()'s shell path.
    BtList btListState() override {
        // gammaos-net bt list-bonded is the same getBondedDevices() call
        // Settings uses; it works whether or not `cmd bluetooth_manager`
        // ever shipped a list-bonded-devices subcommand. We still parse
        // dumpsys once to flip the connected bit on live links, because
        // BluetoothManager has no public "is this one connected right now"
        // query for classic HID/A2DP profiles.
        //
        // Detect radio state so the toggle row reflects reality. When BT is
        // off we still want the list to render with just the toggle row.
        std::string btState = runCmd("settings get global bluetooth_on");
        bool btOn = false;
        for (char c : btState) {
            if (c == '1') { btOn = true; break; }
            if (c == '0') { btOn = false; break; }
            if (c != ' ' && c != '\n' && c != '\r') break;
        }
        std::string bondText = btOn ? runCmd("gammaos-net bt list-bonded", 12)
                                    : std::string();
        auto devs = parseBondedDevices(bondText);
        // Connection state now rides the 4th TSV column from gammaos-net bt
        // list-bonded (BluetoothDevice.isConnected() on the framework side).
        // The old dumpsys scrape was slow (~500 ms) and brittle.
        return {btOn, std::move(devs)};
    }

    // discoverBtDevices().
    std::vector<nanoplat::BtDevEntry> discoverBt() override {
        std::string scanText = runCmd("gammaos-net bt scan 8", 30);
        auto scanned = parseBtScanResults(scanText);
        return scanned;
    }

    // pairBtDevice()'s worker, without the bridge.
    bool pairBt(const std::string& macCopy) override {
        bool ok;
        std::string cmd = "gammaos-net bt pair " + macCopy;
        std::string result = runCmd(cmd.c_str(), 80);   // a BT bond takes 30s+ (SSP + profiles); never a 3s cap
        ok = result.find("OK") != std::string::npos;
        if (!ok) ALOGW("pairBtDevice %s failed: %s",
                       macCopy.c_str(), result.c_str());
        return ok;
    }

    // unpairBtDevice(), without the bridge.
    bool unpairBt(const std::string& mac) override {
        bool ok;
        std::string cmd = "gammaos-net bt unpair " + mac;
        std::string result = runCmd(cmd.c_str(), 20);
        ok = result.find("OK") != std::string::npos;
        if (!ok) ALOGW("unpairBtDevice %s failed: %s",
                       mac.c_str(), result.c_str());
        return ok;
    }

    // btWizRefreshBondedAsync() and btWizToggleRadioAsync(): the radio's real state.
    bool btRadioOn() override {
        std::string d = runCmd("dumpsys bluetooth_manager 2>/dev/null");
        bool on = (d.find("enabled: true") != std::string::npos);
        return on;
    }

    // The Bluetooth wizard's bonded list (gammaos-net auto-enables the adapter).
    std::vector<nanoplat::BtDevEntry> bondedBt() override {
        std::string txt = runCmd("gammaos-net bt list-bonded", 12);
        auto devs = parseBondedDevices(txt);
        return devs;
    }

    // btWizToggleRadioAsync().
    void setBtRadio(bool on) override {
        // BluetoothAdapter.enable()/disable() via the helper PERSISTS bluetooth_on,
        // so the radio stays in the requested state - unlike `cmd bluetooth_manager`
        // from nano's context, which left bluetooth_on=1 and let the service
        // reconcile the radio back on.
        if (!nanoNetBridgeCmd(on ? "bt_enable" : "bt_disable", ""))
            runCmd(on ? "gammaos-net bt radio on" : "gammaos-net bt radio off", 20);   // waits ~10s for the adapter
    }

    // btWizScanAsync(), without the bridge.
    std::vector<nanoplat::BtDevEntry> btWizScan() override {
        // Legacy one-shot inquiry (15s) via the gammaos-net helper.
        std::string txt = runCmd("gammaos-net bt scan 15", 40);
        auto devs = parseBtScanResults(txt);
        return devs;
    }

    // btWizPairAsync()'s worker.
    bool btWizPair(const std::string& a, const std::string& p) override {
        std::string cmd = "gammaos-net bt pair " + a;
        if (!p.empty()) cmd += " " + p;           // a PIN, or --ask: the UI answers what the device asks
        // A BT bond blocks for the whole SSP + profile-connect exchange (5-30s+, worst case ~75s
        // with the transport fallback). runCmd's default 3s cap SIGTERMs it mid-bond, so the pair
        // returns no "OK" and the wizard shows "could not be registered" - the exact regression the
        // fb8b541745e watchdog-bound introduced. This runs on a detached worker, so a long cap is safe.
        // --ask can wait on the user for up to a minute per prompt (a PIN typed from the manual),
        // on each transport.
        std::string r = runCmdUnlocked(cmd, p == "--ask" ? 320 : 80);
        property_set("sys.gammaos.bt_autopair", "0");   // in case the helper was stopped mid-pair
        const bool ok = r.find("OK") != std::string::npos;
        if (!ok) ALOGW("btWizPair %s failed: %s", a.c_str(), r.c_str());
        return ok;
    }

    // btWizInboundAcceptAsync()'s worker: the accept (it then waits for the bond).
    void btConfirmInbound(const std::string& a, int v) override {
        // Apply the user's accept to the pending inbound request, then wait for
        // the bond to land (the BondStateMachine patch left it pending).
        std::string cmd = "gammaos-net bt confirm " + a + " accept";
        if (v == 0) cmd += " 0000";   // classic PIN inbound: default 0000
        runCmd(cmd.c_str(), 20);
    }

    // btWizConnectAsync()'s worker.
    bool btWizConnect(const std::string& a) override {
        std::string r = runCmd(("gammaos-net bt connect " + a).c_str(), 20);
        return r.find("OK") != std::string::npos;
    }

    // btWizDisconnectAsync()'s worker.
    bool btWizDisconnect(const std::string& a) override {
        std::string r = runCmd(("gammaos-net bt disconnect " + a).c_str(), 20);
        return r.find("OK") != std::string::npos;
    }

    // btWizUnpairAsync()'s worker.
    bool btWizUnpair(const std::string& a) override {
        std::string r = runCmd(("gammaos-net bt unpair " + a).c_str(), 20);
        return r.find("OK") != std::string::npos;
    }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::Network> nanofirmware::createNetwork() {
    return std::unique_ptr<nanoplat::Network>(new android::FirmwareNetwork);
}
