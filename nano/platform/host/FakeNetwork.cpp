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

// See FakeNetwork.h.

#include "FakeNetwork.h"

#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gRecordMu;
std::vector<std::string> gRecord;  // guarded by gRecordMu

void record(const std::string& call) {
    {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.push_back(call);
    }
    event("net", "\"call\":" + jsonQuote(call));
}

int bars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    if (rssi >= -88) return 1;
    return 0;
}

class FakeNetwork : public nanoplat::Network {
 public:
    explicit FakeNetwork(FakeNetConfig c) : c_(std::move(c)) {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.clear();
    }

    void autoEnableWifi() override {
        std::lock_guard<std::mutex> l(mu_);
        if (c_.wifiOn) return;
        for (const FakeWifiNet& n : c_.networks) {
            if (n.savedId < 0) continue;
            c_.wifiOn = true;
            record("wifi auto-enable");
            return;
        }
    }

    void pollState(WifiLevel& wifiLevel, int& wifiBars, std::string& wifiSsid, BtLevel& btLevel,
                   int& btConnected) override {
        std::lock_guard<std::mutex> l(mu_);
        if (!c_.wifiOn) {
            wifiLevel = kWifiLevel_Off;
        } else if (const FakeWifiNet* n = connectedNet()) {
            wifiLevel = kWifiLevel_Connected;
            wifiBars = bars(n->rssi);
            wifiSsid = n->ssid;
        } else {
            wifiLevel = kWifiLevel_Disconnected;
        }
        if (!c_.btOn) {
            btLevel = kBtLevel_Off;
            return;
        }
        for (const FakeBtDevice& d : c_.btDevices)
            if (d.bonded && d.connected) btConnected++;
        btLevel = btConnected > 0 ? kBtLevel_Connected : kBtLevel_On;
    }

    WifiRadio wifiListRadio() override {
        std::lock_guard<std::mutex> l(mu_);
        WifiRadio r;
        r.on = c_.wifiOn;
        r.connectedSsid = c_.wifiOn && connectedNet() ? c_.connected : std::string();
        return r;
    }

    WifiLists wifiLists() override {
        std::lock_guard<std::mutex> l(mu_);
        WifiLists out;
        if (!c_.wifiOn) return out;
        for (const FakeWifiNet& n : c_.networks) {
            if (n.savedId >= 0) {
                nanoplat::WifiNetEntry e;
                e.ssid = n.ssid;
                e.savedNetId = n.savedId;
                e.security = n.security;
                out.saved.push_back(std::move(e));
            }
            if (n.inRange) {
                nanoplat::WifiNetEntry e;
                e.ssid = n.ssid;
                e.bssid = n.bssid;
                e.rssi = n.rssi;
                e.security = n.security;
                out.scanned.push_back(std::move(e));
            }
        }
        return out;
    }

    void startWifiScan() override { record("wifi scan"); }

    bool connectSavedWifi(int savedNetId) override {
        record("wifi connect " + std::to_string(savedNetId));
        std::lock_guard<std::mutex> l(mu_);
        for (const FakeWifiNet& n : c_.networks) {
            if (n.savedId != savedNetId || savedNetId < 0) continue;
            if (!c_.wifiOn || !n.inRange) return false;
            c_.connected = n.ssid;
            return true;
        }
        return false;
    }

    void joinWifi(const std::string& ssid, int security, const std::string& password, bool force) override {
        record("wifi join " + ssid + " " + std::to_string(security) + (force ? " force" : ""));
        std::lock_guard<std::mutex> l(mu_);
        if (!c_.wifiOn) return;
        if (!force && c_.connected == ssid && connectedNet()) return;
        FakeWifiNet* n = find(ssid);
        if (!n) {
            c_.networks.push_back(FakeWifiNet());
            n = &c_.networks.back();
            n->ssid = ssid;
            n->inRange = false;
            n->password = password;
        }
        if (n->savedId < 0) n->savedId = nextId();
        n->security = security;
        const bool keyOk = security == 0 || security == 4 || n->password.empty() || n->password == password;
        if (n->inRange && keyOk) {
            c_.connected = ssid;
            wrongKey_.erase(ssid);
        } else if (n->inRange) {
            wrongKey_.insert(ssid);
        }
    }

    JoinCheck checkWifiJoin(const std::string& ssid) override {
        std::lock_guard<std::mutex> l(mu_);
        if (c_.wifiOn && c_.connected == ssid && connectedNet()) return JoinCheck::Connected;
        return wrongKey_.count(ssid) ? JoinCheck::WrongPassword : JoinCheck::Pending;
    }

    bool wifiRadioEnabled() override {
        std::lock_guard<std::mutex> l(mu_);
        return c_.wifiOn;
    }

    WifiAddresses wifiAddresses(bool connected) override {
        std::lock_guard<std::mutex> l(mu_);
        WifiAddresses a;
        a.mac = c_.mac;
        if (c_.wifiOn && connectedNet()) a.ip = c_.ip;
        if (connected) {
            a.gw = c_.gateway;
            a.dns = c_.dns;
            a.dns2 = c_.dns2;
        }
        return a;
    }

    WifiMacAndIp wifiMacAndIp() override {
        std::lock_guard<std::mutex> l(mu_);
        WifiMacAndIp m;
        m.mac = c_.mac;
        if (c_.wifiOn && connectedNet()) m.ip = c_.ip;
        return m;
    }

    WifiLink wifiLink() override {
        std::lock_guard<std::mutex> l(mu_);
        WifiLink k;
        if (c_.wifiOn && connectedNet()) {
            k.ip = c_.ip;
            k.validated = c_.validated;
        }
        return k;
    }

    BtList btListState() override {
        std::lock_guard<std::mutex> l(mu_);
        BtList b;
        b.on = c_.btOn;
        if (b.on) b.devices = bonded();
        return b;
    }

    std::vector<nanoplat::BtDevEntry> discoverBt() override { return inquiry("bt scan 8"); }

    bool pairBt(const std::string& mac) override { return pair("bt pair " + mac, mac); }

    bool unpairBt(const std::string& mac) override { return unpair("bt unpair " + mac, mac); }

    bool btRadioOn() override {
        std::lock_guard<std::mutex> l(mu_);
        return c_.btOn;
    }

    std::vector<nanoplat::BtDevEntry> bondedBt() override {
        std::lock_guard<std::mutex> l(mu_);
        return bonded();
    }

    void setBtRadio(bool on) override {
        record(on ? "bt radio on" : "bt radio off");
        std::lock_guard<std::mutex> l(mu_);
        c_.btOn = on;
    }

    std::vector<nanoplat::BtDevEntry> btWizScan() override { return inquiry("bt scan 15"); }

    bool btWizPair(const std::string& a, const std::string& p) override {
        return pair("bt pair " + a + (p.empty() ? "" : p == "--ask" ? " --ask" : " pin"), a);
    }

    void btConfirmInbound(const std::string& a, int v) override {
        record("bt confirm " + a + " " + std::to_string(v));
    }

    bool btWizConnect(const std::string& a) override {
        record("bt connect " + a);
        std::lock_guard<std::mutex> l(mu_);
        FakeBtDevice* d = device(a);
        if (!c_.btOn || !d || !d->bonded || !d->inRange) return false;
        d->connected = true;
        return true;
    }

    bool btWizDisconnect(const std::string& a) override {
        record("bt disconnect " + a);
        std::lock_guard<std::mutex> l(mu_);
        FakeBtDevice* d = device(a);
        if (!d || !d->connected) return false;
        d->connected = false;
        return true;
    }

    bool btWizUnpair(const std::string& a) override { return unpair("bt unpair " + a, a); }

 private:
    // Under mu_.
    const FakeWifiNet* connectedNet() const {
        if (c_.connected.empty()) return nullptr;
        for (const FakeWifiNet& n : c_.networks)
            if (n.ssid == c_.connected && n.inRange) return &n;
        return nullptr;
    }
    FakeWifiNet* find(const std::string& ssid) {
        for (FakeWifiNet& n : c_.networks)
            if (n.ssid == ssid) return &n;
        return nullptr;
    }
    int nextId() const {
        int id = 0;
        for (const FakeWifiNet& n : c_.networks)
            if (n.savedId >= id) id = n.savedId + 1;
        return id;
    }
    FakeBtDevice* device(const std::string& address) {
        for (FakeBtDevice& d : c_.btDevices)
            if (d.address == address) return &d;
        return nullptr;
    }
    static nanoplat::BtDevEntry entry(const FakeBtDevice& d) {
        nanoplat::BtDevEntry e;
        e.address = d.address;
        e.name = d.name.empty() ? d.address : d.name;
        e.bonded = d.bonded;
        e.connected = d.connected;
        e.cod = d.cod;
        return e;
    }
    std::vector<nanoplat::BtDevEntry> bonded() const {
        std::vector<nanoplat::BtDevEntry> out;
        for (const FakeBtDevice& d : c_.btDevices)
            if (d.bonded) out.push_back(entry(d));
        return out;
    }

    std::vector<nanoplat::BtDevEntry> inquiry(const std::string& call) {
        record(call);
        std::lock_guard<std::mutex> l(mu_);
        std::vector<nanoplat::BtDevEntry> out;
        if (!c_.btOn) return out;
        for (const FakeBtDevice& d : c_.btDevices) {
            if (!d.inRange) continue;
            nanoplat::BtDevEntry e = entry(d);
            e.connected = false;   // as an inquiry reports it
            out.push_back(std::move(e));
        }
        return out;
    }

    bool pair(const std::string& call, const std::string& mac) {
        record(call);
        std::lock_guard<std::mutex> l(mu_);
        FakeBtDevice* d = device(mac);
        if (!c_.btOn || !d || !d->inRange) return false;
        d->bonded = true;
        d->connected = true;
        return true;
    }

    bool unpair(const std::string& call, const std::string& mac) {
        record(call);
        std::lock_guard<std::mutex> l(mu_);
        FakeBtDevice* d = device(mac);
        if (!d || !d->bonded) return false;
        d->bonded = false;
        d->connected = false;
        return true;
    }

    std::mutex mu_;
    FakeNetConfig c_;                 // guarded by mu_
    std::set<std::string> wrongKey_;  // guarded by mu_: joins whose key was not the scripted one
};

}  // namespace

std::unique_ptr<nanoplat::Network> createFakeNetwork(FakeNetConfig config) {
    return std::unique_ptr<nanoplat::Network>(new FakeNetwork(std::move(config)));
}

std::vector<std::string> networkRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

}  // namespace nanohost
