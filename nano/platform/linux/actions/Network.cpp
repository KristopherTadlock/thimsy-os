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

// The Linux Wi-Fi and Bluetooth (Network.h).

#define LOG_TAG "ThimsyNetwork"

#include "Network.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <log/log.h>

#include "SystemActions.h"  // validMac
#include "WpaCtrl.h"

namespace nanolinux {
namespace net {

namespace {

using actions::RunResult;

constexpr int kConnectWaitMs = 25000;   // a saved network's SELECT_NETWORK to COMPLETED
constexpr int kJoinWaitMs = 12000;      // a join's first wait (the firmware's ~12 s), then its retry's
constexpr int kJoinRetryWaitMs = 10000;
constexpr int kPollStepMs = 500;
constexpr int kBtQuickMs = 3000;        // show, devices, info
constexpr int kBtRemoveMs = 4000;       // on the menu's input thread
constexpr int kBtPowerMs = 10000;
constexpr int kBtConnectMs = 20000;
constexpr int kBtPairMs = 60000;
constexpr int kBtBackoffMs = 30000;
constexpr int kBtScanMax = 32;          // devices looked up after an inquiry

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(std::move(l));
        p = e + 1;
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
        size_t t = line.find('\t', p);
        f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
        if (t == std::string::npos) return f;
        p = t + 1;
    }
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

bool readFile(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "re");
    if (!f) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out->size() < (1u << 20)) out->append(buf, n);
    fclose(f);
    return true;
}

std::string baseName(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// A reply of key=value lines (STATUS, SIGNAL_POLL).
std::map<std::string, std::string> keyValues(const std::string& reply) {
    std::map<std::string, std::string> kv;
    for (const std::string& l : lines(reply)) {
        const size_t eq = l.find('=');
        if (eq != std::string::npos && eq > 0) kv[l.substr(0, eq)] = l.substr(eq + 1);
    }
    return kv;
}

std::string get(const std::map<std::string, std::string>& kv, const char* key) {
    auto it = kv.find(key);
    return it == kv.end() ? std::string() : it->second;
}

bool isFail(const std::string& reply) { return reply.compare(0, 4, "FAIL") == 0; }

// A string value for SET_NETWORK: quoted; wpa_supplicant takes everything up to the last quote.
std::string quoted(const std::string& s) { return "\"" + s + "\""; }

bool allHex(const std::string& s) {
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return !s.empty();
}

// bluetoothctl's output without its colours and prompts' carriage returns.
std::string plainText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++;
            continue;
        }
        if (s[i] == '\r' || s[i] == '\x01' || s[i] == '\x02') continue;
        out += s[i];
    }
    return out;
}

// "Device AA:BB:CC:DD:EE:FF Name" lines, wherever bluetoothctl puts them ("[NEW] Device ...").
std::vector<nanoplat::BtDevEntry> deviceLines(const std::string& out) {
    std::vector<nanoplat::BtDevEntry> devs;
    for (const std::string& l : lines(plainText(out))) {
        const size_t p = l.find("Device ");
        if (p == std::string::npos || l.size() < p + 7 + 17) continue;
        const std::string mac = l.substr(p + 7, 17);
        if (!actions::validMac(mac)) continue;
        bool seen = false;
        for (const auto& d : devs) seen = seen || d.address == mac;
        if (seen) continue;
        nanoplat::BtDevEntry d;
        d.address = mac;
        d.name = trim(l.size() > p + 25 ? l.substr(p + 25) : std::string());
        devs.push_back(std::move(d));
    }
    return devs;
}

// "\tKey: value" lines (show, info).
std::map<std::string, std::string> fields(const std::string& out) {
    std::map<std::string, std::string> f;
    for (const std::string& raw : lines(plainText(out))) {
        const std::string l = trim(raw);
        const size_t c = l.find(": ");
        if (c != std::string::npos && c > 0 && !f.count(l.substr(0, c))) f[l.substr(0, c)] = trim(l.substr(c + 2));
    }
    return f;
}

bool btFailed(const RunResult& r) {
    const std::string t = plainText(r.out);
    return r.status != 0 || r.timedOut || t.find("Failed") != std::string::npos ||
           t.find("not available") != std::string::npos || t.find("Invalid") != std::string::npos;
}

void defaultSleep(int ms) { usleep((useconds_t)ms * 1000); }

long long monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// wpa_supplicant's warnings, at most one each 30 s: the net poll asks every few seconds.
bool wpaWarnNow() {
    static std::atomic<long long> last{-1};
    const long long now = monotonicMs();
    long long prev = last.load();
    return (prev < 0 || now - prev >= 30000) && last.compare_exchange_strong(prev, now);
}

// One connection to wpa_supplicant for a call's requests.
class Wpa {
 public:
    explicit Wpa(const Options& o) : o_(o) {
        const std::vector<std::string> socks = wpa::sockets(o.root + o.wpaDir);
        if (socks.empty()) return;
        present_ = true;
        ifname_ = baseName(socks.front());
        std::string err;
        if (!c_.open(socks.front(), o.root + o.wpaLocalDir, &err) && wpaWarnNow())
            ALOGW("wpa_supplicant: %s", err.c_str());
    }
    // A control socket is there (wpa_supplicant runs), whether or not it answers.
    bool present() const { return present_; }
    const std::string& ifname() const { return ifname_; }

    // `cmd`'s reply, or false (logged with the command's first word only: SET_NETWORK carries keys).
    bool request(const std::string& cmd, std::string* reply) {
        if (!c_.isOpen()) return false;
        std::string err;
        reply->clear();
        if (c_.request(cmd, reply, o_.wpaReplyMs, &err)) return true;
        if (wpaWarnNow())
            ALOGW("wpa_supplicant %s: %s", cmd.substr(0, cmd.find(' ')).c_str(),
                  err.compare(0, 12, "no reply to ") == 0 ? "no reply" : err.c_str());
        return false;
    }
    bool ok(const std::string& cmd) {
        std::string reply;
        return request(cmd, &reply) && wpa::isOk(reply);
    }

    struct Status {
        bool answered = false;
        std::map<std::string, std::string> kv;
        std::string state() const { return get(kv, "wpa_state"); }
        bool completed() const { return state() == "COMPLETED"; }
        std::string ssid() const { return completed() ? decodeWpaText(get(kv, "ssid")) : std::string(); }
    };
    Status status() {
        Status s;
        std::string reply;
        if (request("STATUS", &reply) && !isFail(reply)) {
            s.answered = true;
            s.kv = keyValues(reply);
        }
        return s;
    }

    struct Saved {
        int id;
        std::string ssid;
        std::string flags;
    };
    std::vector<Saved> listNetworks() {
        std::vector<Saved> out;
        std::string reply;
        if (!request("LIST_NETWORKS", &reply) || isFail(reply)) return out;
        bool header = true;
        for (const std::string& l : lines(reply)) {
            if (header) { header = false; continue; }   // "network id / ssid / bssid / flags"
            const std::vector<std::string> f = splitTabs(l);
            if (f.size() < 2 || f[0].empty() || !isdigit((unsigned char)f[0][0])) continue;
            out.push_back({atoi(f[0].c_str()), decodeWpaText(f[1]), f.size() > 3 ? f[3] : std::string()});
        }
        return out;
    }

 private:
    const Options& o_;
    wpa::Ctrl c_;
    bool present_ = false;
    std::string ifname_;
};

class LinuxNetwork : public nanoplat::Network {
 public:
    explicit LinuxNetwork(Options o) : o_(std::move(o)) {
        if (!o_.run) o_.run = actions::runProgram;
        if (!o_.sleep) o_.sleep = defaultSleep;
        if (!o_.nowMs) o_.nowMs = monotonicMs;
    }

    // D30net brings Wi-Fi up at boot from persist.thimsy.wifi.enabled, which the radio toggle
    // writes: the last on/off is already remembered.
    void autoEnableWifi() override {}

    void pollState(WifiLevel& wifiLevel, int& wifiBars, std::string& wifiSsid, BtLevel& btLevel,
                   int& btConnected) override {
        {
            Wpa w(o_);
            if (!w.present()) {
                wifiLevel = kWifiLevel_Off;
            } else {
                const Wpa::Status st = w.status();
                if (st.answered) {
                    const std::string ssid = st.ssid();
                    if (st.state() == "INTERFACE_DISABLED") {
                        wifiLevel = kWifiLevel_Off;
                    } else if (!ssid.empty()) {
                        wifiLevel = kWifiLevel_Connected;
                        wifiSsid = ssid;
                        std::string sig;
                        const std::string rssi =
                                w.request("SIGNAL_POLL", &sig) && !isFail(sig) ? get(keyValues(sig), "RSSI") : "";
                        // Two bars when the level can't be read, as the firmware's poll does.
                        wifiBars = !rssi.empty() && atoi(rssi.c_str()) < 0 ? barsOfRssi(atoi(rssi.c_str())) : 2;
                    } else {
                        wifiLevel = kWifiLevel_Disconnected;
                    }
                }
            }
        }
        int on = powered();
        if (on < 0) return;   // no bluetoothctl or no bluetoothd: unknown
        if (!on) {
            btLevel = kBtLevel_Off;
            return;
        }
        btLevel = kBtLevel_On;
        for (const auto& d : paired())
            if (d.connected) btConnected++;
        if (btConnected > 0) btLevel = kBtLevel_Connected;
    }

    WifiRadio wifiListRadio() override {
        Wpa w(o_);
        const Wpa::Status st = w.status();
        WifiRadio r;
        r.on = st.answered && st.state() != "INTERFACE_DISABLED";
        r.connectedSsid = st.ssid();
        return r;
    }

    WifiLists wifiLists() override {
        Wpa w(o_);
        WifiLists l;
        for (const Wpa::Saved& s : w.listNetworks()) {
            if (s.ssid.empty()) continue;
            nanoplat::WifiNetEntry e;
            e.ssid = s.ssid;
            e.savedNetId = s.id;
            e.rssi = -127;
            std::string km, wep;
            const std::string id = std::to_string(s.id);
            if (!w.request("GET_NETWORK " + id + " key_mgmt", &km) || isFail(km)) km.clear();
            const bool none = trim(km) == "NONE";
            const bool hasWep = none && w.request("GET_NETWORK " + id + " wep_key0", &wep) && !isFail(wep) &&
                                !trim(wep).empty();
            e.security = securityOfKeyMgmt(trim(km), hasWep);
            l.saved.push_back(std::move(e));
        }
        std::string reply;
        if (w.request("SCAN_RESULTS", &reply) && !isFail(reply)) {
            bool header = true;
            for (const std::string& line : lines(reply)) {
                if (header) { header = false; continue; }   // "bssid / frequency / signal level / flags / ssid"
                const std::vector<std::string> f = splitTabs(line);
                if (f.size() < 5) continue;
                nanoplat::WifiNetEntry e;
                e.bssid = f[0];
                e.rssi = atoi(f[2].c_str());
                if (e.rssi >= 0 || e.rssi < -120) e.rssi = -127;
                e.security = securityOfFlags(f[3]);
                e.ssid = decodeWpaText(f[4]);
                // A hidden network's SSID is empty or NULs.
                if (e.ssid.empty() || e.ssid.find_first_not_of('\0') == std::string::npos) continue;
                l.scanned.push_back(std::move(e));
            }
        }
        return l;
    }

    void startWifiScan() override {
        Wpa w(o_);
        std::string reply;
        if (w.request("SCAN", &reply) && !wpa::isOk(reply) && reply.compare(0, 9, "FAIL-BUSY") != 0)
            ALOGW("Wi-Fi scan: %s", trim(reply).c_str());
    }

    bool connectSavedWifi(int savedNetId) override {
        if (savedNetId < 0) return false;
        Wpa w(o_);
        const std::string id = std::to_string(savedNetId);
        if (!w.ok("SELECT_NETWORK " + id)) {
            ALOGW("Wi-Fi connect %d: SELECT_NETWORK failed", savedNetId);
            return false;
        }
        bool connected = false;
        for (int waited = 0; waited < kConnectWaitMs && !connected; waited += kPollStepMs) {
            o_.sleep(kPollStepMs);
            const Wpa::Status st = w.status();
            connected = st.completed() && get(st.kv, "id") == id;
        }
        // SELECT_NETWORK disabled every other network; they stay saved and enabled.
        (void)w.ok("ENABLE_NETWORK all");
        if (!connected) ALOGW("Wi-Fi connect %d: not connected", savedNetId);
        return connected;
    }

    void joinWifi(const std::string& ssid, int security, const std::string& password, bool force) override {
        if (ssid.empty()) return;
        Wpa w(o_);
        if (!w.present()) {
            ALOGW("Wi-Fi join: wpa_supplicant is not running");
            return;
        }
        // Not when already connected to it (force: a changed key always goes in).
        if (!force && w.status().ssid() == ssid) return;
        int id = -1;
        for (const Wpa::Saved& s : w.listNetworks())
            if (s.ssid == ssid) { id = s.id; break; }
        bool added = false;
        if (id < 0) {
            std::string reply;
            if (!w.request("ADD_NETWORK", &reply) || !isdigit((unsigned char)(reply.empty() ? 'x' : reply[0]))) {
                ALOGW("Wi-Fi join: ADD_NETWORK failed");
                return;
            }
            id = atoi(reply.c_str());
            added = true;
        }
        const std::string set = "SET_NETWORK " + std::to_string(id) + " ";
        std::vector<std::pair<std::string, std::string>> fieldsToSet = {{"ssid", hexOf(ssid)}};
        switch (security) {
            case 1:   // WEP: 10 or 26 hex digits are the key itself, anything else its text
                fieldsToSet.push_back({"key_mgmt", "NONE"});
                fieldsToSet.push_back({"wep_key0", (password.size() == 10 || password.size() == 26) && allHex(password)
                                                           ? password : quoted(password)});
                fieldsToSet.push_back({"wep_tx_keyidx", "0"});
                fieldsToSet.push_back({"auth_alg", "OPEN SHARED"});
                break;
            case 2:   // WPA/WPA2 personal: 64 hex digits are the PSK itself
                fieldsToSet.push_back({"key_mgmt", "WPA-PSK"});
                fieldsToSet.push_back({"psk", password.size() == 64 && allHex(password) ? password : quoted(password)});
                break;
            case 3:   // WPA3 personal, with WPA2 for a transition-mode network
                fieldsToSet.push_back({"key_mgmt", "SAE WPA-PSK"});
                fieldsToSet.push_back({"ieee80211w", "1"});
                fieldsToSet.push_back({"sae_password", quoted(password)});
                if (password.size() >= 8 && password.size() <= 63) fieldsToSet.push_back({"psk", quoted(password)});
                break;
            case 4:
                fieldsToSet.push_back({"key_mgmt", "OWE"});
                fieldsToSet.push_back({"ieee80211w", "2"});
                break;
            default:
                fieldsToSet.push_back({"key_mgmt", "NONE"});
                break;
        }
        for (const auto& f : fieldsToSet) {
            if (!w.ok(set + f.first + " " + f.second)) {
                ALOGW("Wi-Fi join: SET_NETWORK %s refused", f.first.c_str());
                if (added) (void)w.ok("REMOVE_NETWORK " + std::to_string(id));
                return;
            }
        }
        const std::string select = "SELECT_NETWORK " + std::to_string(id);
        (void)w.ok(select);
        bool connected = waitFor(w, ssid, kJoinWaitMs);
        if (!connected && !force && !tempDisabled(w, ssid)) {   // no association, and the key was not refused
            (void)w.ok(select);
            connected = waitFor(w, ssid, kJoinRetryWaitMs);
        }
        (void)w.ok("ENABLE_NETWORK all");
        if (!w.ok("SAVE_CONFIG")) ALOGW("Wi-Fi join: SAVE_CONFIG failed");
        ALOGI("Wi-Fi join: network %d %s", id, connected ? "connected" : "not connected");
    }

    JoinCheck checkWifiJoin(const std::string& ssid) override {
        Wpa w(o_);
        if (w.status().ssid() == ssid && !ssid.empty()) return JoinCheck::Connected;
        return tempDisabled(w, ssid) ? JoinCheck::WrongPassword : JoinCheck::Pending;
    }

    bool wifiRadioEnabled() override {
        Wpa w(o_);
        const Wpa::Status st = w.status();
        return st.answered && st.state() != "INTERFACE_DISABLED";
    }

    WifiAddresses wifiAddresses(bool connected) override {
        Wpa w(o_);
        const Wpa::Status st = w.status();
        WifiAddresses a;
        a.ip = get(st.kv, "ip_address");
        a.mac = macOf(w, st);
        if (connected) {
            a.gw = gateway(w.ifname());
            const std::vector<std::string> dns = nameservers();
            if (!dns.empty()) a.dns = dns[0];
            if (dns.size() > 1) a.dns2 = dns[1];
        }
        return a;
    }

    WifiMacAndIp wifiMacAndIp() override {
        Wpa w(o_);
        const Wpa::Status st = w.status();
        WifiMacAndIp m;
        m.mac = macOf(w, st);
        m.ip = get(st.kv, "ip_address");
        return m;
    }

    WifiLink wifiLink() override {
        Wpa w(o_);
        const Wpa::Status st = w.status();
        WifiLink l;
        l.ip = st.completed() ? get(st.kv, "ip_address") : std::string();
        l.validated = !l.ip.empty() && !gateway(w.ifname()).empty() && !nameservers().empty();
        return l;
    }

    BtList btListState() override {
        BtList l;
        l.on = powered() == 1;
        if (l.on) l.devices = paired();
        return l;
    }

    std::vector<nanoplat::BtDevEntry> discoverBt() override { return inquiry(8); }

    bool pairBt(const std::string& mac) override { return pair(mac); }

    bool unpairBt(const std::string& mac) override {
        if (!actions::validMac(mac)) return false;
        const RunResult r = bt({"bluetoothctl", "remove", mac}, kBtRemoveMs);
        if (btFailed(r)) ALOGW("Bluetooth remove %s failed", mac.c_str());
        return !btFailed(r);
    }

    bool btRadioOn() override { return powered() == 1; }

    std::vector<nanoplat::BtDevEntry> bondedBt() override { return paired(); }

    void setBtRadio(bool on) override {
        const RunResult r = bt({"bluetoothctl", "power", on ? "on" : "off"}, kBtPowerMs);
        if (btFailed(r)) ALOGW("Bluetooth power %s failed", on ? "on" : "off");
    }

    std::vector<nanoplat::BtDevEntry> btWizScan() override { return inquiry(15); }

    bool btWizPair(const std::string& a, const std::string& p) override {
        if (!p.empty()) ALOGI("Bluetooth pair %s: no PIN entry on Linux yet; pairing without it", a.c_str());
        return pair(a);
    }

    void btConfirmInbound(const std::string& a, int /*v*/) override {
        ALOGW("Bluetooth %s: inbound pairing requests are not accepted on Linux", a.c_str());
    }

    bool btWizConnect(const std::string& a) override { return btSimple("connect", a, kBtConnectMs); }
    bool btWizDisconnect(const std::string& a) override { return btSimple("disconnect", a, kBtConnectMs); }
    bool btWizUnpair(const std::string& a) override { return btSimple("remove", a, kBtQuickMs); }

 private:
    bool waitFor(Wpa& w, const std::string& ssid, int ms) {
        for (int waited = 0; waited < ms; waited += kPollStepMs) {
            o_.sleep(kPollStepMs);
            if (w.status().ssid() == ssid) return true;
        }
        return false;
    }

    bool tempDisabled(Wpa& w, const std::string& ssid) {
        for (const Wpa::Saved& s : w.listNetworks())
            if (s.ssid == ssid && s.flags.find("[TEMP-DISABLED]") != std::string::npos) return true;
        return false;
    }

    std::string macOf(Wpa& w, const Wpa::Status& st) {
        std::string mac = get(st.kv, "address");
        if (mac.empty() && !w.ifname().empty()) {
            std::string text;
            if (readFile(o_.root + o_.sysNet + "/" + w.ifname() + "/address", &text)) mac = trim(text);
        }
        return mac;
    }

    // The default route's gateway on `ifname` (/proc/net/route: Iface Destination Gateway Flags ...,
    // addresses as little-endian hex), "" when there is none.
    std::string gateway(const std::string& ifname) {
        std::string text;
        if (ifname.empty() || !readFile(o_.root + o_.procRoute, &text)) return "";
        for (const std::string& l : lines(text)) {
            char iface[64], dest[16], gw[16];
            unsigned flags = 0;
            if (sscanf(l.c_str(), "%63s %15s %15s %x", iface, dest, gw, &flags) != 4) continue;
            if (ifname != iface || strcmp(dest, "00000000") != 0 || !(flags & 0x2)) continue;   // RTF_GATEWAY
            const unsigned long g = strtoul(gw, nullptr, 16);
            char out[16];
            snprintf(out, sizeof(out), "%lu.%lu.%lu.%lu", g & 0xff, (g >> 8) & 0xff, (g >> 16) & 0xff, (g >> 24) & 0xff);
            return out;
        }
        return "";
    }

    std::vector<std::string> nameservers() {
        std::vector<std::string> out;
        std::string text;
        if (!readFile(o_.root + o_.resolvConf, &text)) return out;
        for (const std::string& l : lines(text)) {
            const std::string t = trim(l);
            if (t.compare(0, 11, "nameserver ") != 0) continue;
            const std::string a = trim(t.substr(11));
            if (!a.empty()) out.push_back(a);
        }
        return out;
    }

    // bluetoothctl, unless it was missing or did not answer in the last 30 s.
    RunResult bt(const std::vector<std::string>& argv, int timeoutMs) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (o_.nowMs() < btSkipUntil_) return RunResult{127, false, ""};
        }
        RunResult r = o_.run(argv, timeoutMs);
        if (r.status == 127 || (r.timedOut && plainText(r.out).find("Waiting to connect") != std::string::npos)) {
            std::lock_guard<std::mutex> l(mu_);
            btSkipUntil_ = o_.nowMs() + kBtBackoffMs;
            ALOGW("Bluetooth: %s; not asked again for %d s",
                  r.status == 127 ? "bluetoothctl is not on this image" : "bluetoothd does not answer",
                  kBtBackoffMs / 1000);
        }
        return r;
    }

    // 1 powered, 0 off or no controller, -1 unknown (no bluetoothctl, no answer).
    int powered() {
        const RunResult r = bt({"bluetoothctl", "show"}, kBtQuickMs);
        if (r.status == 127 || r.timedOut) return -1;
        const std::string t = plainText(r.out);
        if (t.find("No default controller") != std::string::npos) return 0;
        const std::map<std::string, std::string> f = fields(r.out);
        auto it = f.find("Powered");
        if (it == f.end()) return r.status == 0 ? 0 : -1;
        return it->second == "yes" ? 1 : 0;
    }

    // Fills name, cod, bonded and connected from info; false when info gave nothing.
    bool info(nanoplat::BtDevEntry& d) {
        const RunResult r = bt({"bluetoothctl", "info", d.address}, kBtQuickMs);
        if (r.status == 127 || r.timedOut) return false;
        const std::map<std::string, std::string> f = fields(r.out);
        if (f.empty()) return false;
        auto field = [&](const char* k) {
            auto it = f.find(k);
            return it == f.end() ? std::string() : it->second;
        };
        if (!field("Name").empty()) d.name = field("Name");
        else if (d.name.empty() || d.name == d.address) d.name = field("Alias");
        const std::string cls = field("Class");
        if (!cls.empty()) d.cod = (int)strtol(cls.c_str(), nullptr, 0);
        d.bonded = field("Paired") == "yes" || field("Bonded") == "yes";
        d.connected = field("Connected") == "yes";
        return true;
    }

    // The paired devices, each checked by info (an older bluetoothctl lists every device).
    std::vector<nanoplat::BtDevEntry> paired() {
        RunResult r = bt({"bluetoothctl", "devices", "Paired"}, kBtQuickMs);
        if (r.status == 127 || r.timedOut) return {};
        const std::string t = plainText(r.out);
        if (r.status != 0 || t.find("Too many arguments") != std::string::npos ||
            t.find("Invalid") != std::string::npos)
            r = bt({"bluetoothctl", "paired-devices"}, kBtQuickMs);   // BlueZ before 5.65
        std::vector<nanoplat::BtDevEntry> out;
        for (nanoplat::BtDevEntry& d : deviceLines(r.out)) {
            if (!info(d) || !d.bonded) continue;
            if (d.name.empty()) d.name = d.address;
            out.push_back(std::move(d));
        }
        return out;
    }

    std::vector<nanoplat::BtDevEntry> inquiry(int secs) {
        const RunResult s = bt({"bluetoothctl", "--timeout", std::to_string(secs), "scan", "on"}, (secs + 5) * 1000);
        if (s.status == 127) return {};
        const RunResult r = bt({"bluetoothctl", "devices"}, kBtQuickMs);
        std::vector<nanoplat::BtDevEntry> out;
        for (nanoplat::BtDevEntry& d : deviceLines(r.out)) {
            if ((int)out.size() >= kBtScanMax) break;
            (void)info(d);
            d.connected = false;
            if (d.name.empty()) d.name = d.address;
            out.push_back(std::move(d));
        }
        return out;
    }

    bool pair(const std::string& mac) {
        if (!actions::validMac(mac)) return false;
        const RunResult r = bt({"bluetoothctl", "--agent", "NoInputNoOutput", "pair", mac}, kBtPairMs);
        nanoplat::BtDevEntry d;
        d.address = mac;
        const bool ok = info(d) && d.bonded;
        if (!ok) {
            ALOGW("Bluetooth pair %s failed%s", mac.c_str(), r.timedOut ? " (timed out)" : "");
            return false;
        }
        if (btFailed(bt({"bluetoothctl", "trust", mac}, kBtQuickMs))) ALOGW("Bluetooth trust %s failed", mac.c_str());
        if (btFailed(bt({"bluetoothctl", "connect", mac}, kBtConnectMs)))
            ALOGW("Bluetooth connect %s failed after pairing", mac.c_str());
        return true;
    }

    bool btSimple(const char* verb, const std::string& mac, int timeoutMs) {
        if (!actions::validMac(mac)) return false;
        const RunResult r = bt({"bluetoothctl", verb, mac}, timeoutMs);
        if (btFailed(r)) ALOGW("Bluetooth %s %s failed", verb, mac.c_str());
        return !btFailed(r);
    }

    Options o_;
    std::mutex mu_;
    long long btSkipUntil_ = 0;   // guarded by mu_
};

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string decodeWpaText(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        switch (c) {
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'e': out += '\x1b'; break;
            case 'x':
                if (i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
                    out += (char)(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                    i += 2;
                } else {
                    out += "\\x";
                }
                break;
            default:
                if (c >= '0' && c <= '7') {   // up to three octal digits
                    int v = c - '0', n = 1;
                    while (n < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') v = v * 8 + (s[++i] - '0'), n++;
                    out += (char)v;
                } else {
                    out += '\\';
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string hexOf(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += kHex[c >> 4];
        out += kHex[c & 0xf];
    }
    return out;
}

int securityOfFlags(const std::string& flags) {
    std::string f = flags;
    for (char& c : f) c = (char)toupper((unsigned char)c);
    if (f.find("SAE") != std::string::npos || f.find("WPA3") != std::string::npos) return 3;
    if (f.find("OWE") != std::string::npos) return 4;
    if (f.find("WPA") != std::string::npos || f.find("PSK") != std::string::npos || f.find("EAP") != std::string::npos)
        return 2;
    if (f.find("WEP") != std::string::npos) return 1;
    return 0;
}

int securityOfKeyMgmt(const std::string& keyMgmt, bool hasWepKey) {
    std::string k = keyMgmt;
    for (char& c : k) c = (char)toupper((unsigned char)c);
    if (k.find("SAE") != std::string::npos) return 3;
    if (k.find("OWE") != std::string::npos) return 4;
    if (k.find("WPA") != std::string::npos || k.find("PSK") != std::string::npos) return 2;
    return hasWepKey ? 1 : 0;
}

int barsOfRssi(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    if (rssi >= -88) return 1;
    return 0;
}

std::unique_ptr<nanoplat::Network> create(Options options) {
    return std::unique_ptr<nanoplat::Network>(new LinuxNetwork(std::move(options)));
}

}  // namespace net
}  // namespace nanolinux
