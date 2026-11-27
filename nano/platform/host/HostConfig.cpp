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

// See HostConfig.h.

#include "HostConfig.h"

#include <cstdint>
#include <initializer_list>

#include <thimsy/json.h>

namespace nanohost {

namespace {

using thimsy::json::Value;

class Reader {
 public:
    explicit Reader(std::string* err) : err_(err) {}

    bool fail(const std::string& where, const std::string& what) {
        if (err_) *err_ = where + ": " + what;
        return false;
    }

    bool object(const Value& v, const std::string& where, std::initializer_list<const char*> known) {
        if (!v.isObject()) return fail(where, "not an object");
        for (const auto& m : v.members()) {
            bool ok = false;
            for (const char* k : known) ok = ok || m.key == k;
            if (!ok) return fail(where, "unknown member \"" + m.key + "\"");
        }
        return true;
    }

    bool str(const Value& obj, const char* key, const std::string& where, std::string* out) {
        const Value* v = obj.find(key);
        if (!v) return true;
        if (!v->isString()) return fail(where + "." + key, "not a string");
        *out = v->asString();
        return true;
    }

    template <class T>
    bool integer(const Value& obj, const char* key, const std::string& where, int64_t lo, int64_t hi, T* out) {
        const Value* v = obj.find(key);
        if (!v) return true;
        if (!v->isInt()) return fail(where + "." + key, "not an integer");
        const int64_t n = v->asInt();
        if (n < lo || n > hi) return fail(where + "." + key, "out of range");
        *out = (T)n;
        return true;
    }

    bool boolean(const Value& obj, const char* key, const std::string& where, bool* out) {
        const Value* v = obj.find(key);
        if (!v) return true;
        if (!v->isBool()) return fail(where + "." + key, "not a boolean");
        *out = v->asBool();
        return true;
    }

    bool ints(const Value& obj, const char* key, const std::string& where, int64_t lo, int64_t hi,
              std::vector<int>* out) {
        const Value* v = obj.find(key);
        if (!v) return true;
        if (!v->isArray()) return fail(where + "." + key, "not an array");
        out->clear();
        for (size_t i = 0; i < v->size(); ++i) {
            const Value& e = (*v)[i];
            if (!e.isInt() || e.asInt() < lo || e.asInt() > hi)
                return fail(where + "." + key + "[" + std::to_string(i) + "]", "not an integer in range");
            out->push_back((int)e.asInt());
        }
        return true;
    }

 private:
    std::string* err_;
};

bool parseDevice(Reader& r, const Value& v, const std::string& where, evdev::Device* d) {
    if (!r.object(v, where, {"path", "name", "phys", "uniq", "bus", "vendor", "product", "version", "keys",
                             "abs", "rel", "props"}))
        return false;
    if (!r.str(v, "path", where, &d->path) || !r.str(v, "name", where, &d->name) ||
        !r.str(v, "phys", where, &d->phys) || !r.str(v, "uniq", where, &d->uniq) ||
        !r.integer(v, "bus", where, 0, 0xffff, &d->bus) || !r.integer(v, "vendor", where, 0, 0xffff, &d->vendor) ||
        !r.integer(v, "product", where, 0, 0xffff, &d->product) ||
        !r.integer(v, "version", where, 0, 0xffff, &d->version) ||
        !r.ints(v, "keys", where, 0, 0x2ff, &d->keys) || !r.ints(v, "rel", where, 0, 0x0f, &d->rel) ||
        !r.ints(v, "props", where, 0, 0x1f, &d->props))
        return false;
    if (d->path.empty()) return r.fail(where + ".path", "missing");
    if (const Value* abs = v.find("abs")) {
        if (!abs->isArray()) return r.fail(where + ".abs", "not an array");
        for (size_t i = 0; i < abs->size(); ++i) {
            const std::string w = where + ".abs[" + std::to_string(i) + "]";
            const Value& a = (*abs)[i];
            evdev::AbsAxis axis;
            if (!r.object(a, w, {"code", "value", "min", "max", "fuzz", "flat", "resolution"}) ||
                !r.integer(a, "code", w, 0, 0x3f, &axis.code) ||
                !r.integer(a, "value", w, INT32_MIN, INT32_MAX, &axis.value) ||
                !r.integer(a, "min", w, INT32_MIN, INT32_MAX, &axis.minimum) ||
                !r.integer(a, "max", w, INT32_MIN, INT32_MAX, &axis.maximum) ||
                !r.integer(a, "fuzz", w, INT32_MIN, INT32_MAX, &axis.fuzz) ||
                !r.integer(a, "flat", w, INT32_MIN, INT32_MAX, &axis.flat) ||
                !r.integer(a, "resolution", w, INT32_MIN, INT32_MAX, &axis.resolution))
                return false;
            if (!a.find("code")) return r.fail(w + ".code", "missing");
            d->abs.push_back(axis);
        }
    }
    return true;
}

// "network" (FakeNetwork.h): the scripted Wi-Fi networks and Bluetooth devices.
bool parseNetwork(Reader& r, const Value& v, const std::string& where, FakeNetConfig* n) {
    if (!r.object(v, where, {"wifi_on", "connected", "ip", "mac", "gateway", "dns", "dns2", "validated", "networks",
                             "bt_on", "bt_devices"}) ||
        !r.boolean(v, "wifi_on", where, &n->wifiOn) || !r.str(v, "connected", where, &n->connected) ||
        !r.str(v, "ip", where, &n->ip) || !r.str(v, "mac", where, &n->mac) || !r.str(v, "gateway", where, &n->gateway) ||
        !r.str(v, "dns", where, &n->dns) || !r.str(v, "dns2", where, &n->dns2) ||
        !r.boolean(v, "validated", where, &n->validated) || !r.boolean(v, "bt_on", where, &n->btOn))
        return false;
    if (const Value* nets = v.find("networks")) {
        if (!nets->isArray()) return r.fail(where + ".networks", "not an array");
        n->networks.clear();
        for (size_t i = 0; i < nets->size(); ++i) {
            const std::string w = where + ".networks[" + std::to_string(i) + "]";
            const Value& e = (*nets)[i];
            FakeWifiNet net;
            if (!r.object(e, w, {"ssid", "bssid", "rssi", "security", "saved_id", "in_range", "password"}) ||
                !r.str(e, "ssid", w, &net.ssid) || !r.str(e, "bssid", w, &net.bssid) ||
                !r.integer(e, "rssi", w, -127, 0, &net.rssi) || !r.integer(e, "security", w, 0, 4, &net.security) ||
                !r.integer(e, "saved_id", w, -1, 65535, &net.savedId) || !r.boolean(e, "in_range", w, &net.inRange) ||
                !r.str(e, "password", w, &net.password))
                return false;
            if (net.ssid.empty()) return r.fail(w + ".ssid", "missing");
            n->networks.push_back(std::move(net));
        }
    }
    if (const Value* devs = v.find("bt_devices")) {
        if (!devs->isArray()) return r.fail(where + ".bt_devices", "not an array");
        n->btDevices.clear();
        for (size_t i = 0; i < devs->size(); ++i) {
            const std::string w = where + ".bt_devices[" + std::to_string(i) + "]";
            const Value& e = (*devs)[i];
            FakeBtDevice d;
            if (!r.object(e, w, {"address", "name", "bonded", "connected", "cod", "in_range"}) ||
                !r.str(e, "address", w, &d.address) || !r.str(e, "name", w, &d.name) ||
                !r.boolean(e, "bonded", w, &d.bonded) || !r.boolean(e, "connected", w, &d.connected) ||
                !r.integer(e, "cod", w, 0, 0xffffff, &d.cod) || !r.boolean(e, "in_range", w, &d.inRange))
                return false;
            if (d.address.empty()) return r.fail(w + ".address", "missing");
            n->btDevices.push_back(std::move(d));
        }
    }
    return true;
}

}  // namespace

bool parseHostConfig(std::string_view text, HostConfig* out, std::string* err) {
    Value root;
    std::string perr;
    if (!thimsy::json::parse(text, &root, &perr)) {
        if (err) *err = "host config: " + perr;
        return false;
    }
    Reader r(err);
    const std::string top = "host config";
    if (!r.object(root, top, {"events", "control", "persist_file", "run_dir", "log_file", "display", "barrier",
                              "devices", "shell", "record_props", "volumes", "network", "nano_owns_boot_screen",
                              "caps", "settings"}))
        return false;
    HostConfig c = *out;
    if (!r.str(root, "events", top, &c.events) || !r.str(root, "control", top, &c.control) ||
        !r.str(root, "persist_file", top, &c.persistFile) || !r.str(root, "run_dir", top, &c.runDir) ||
        !r.str(root, "log_file", top, &c.logFile) || !r.boolean(root, "barrier", top, &c.barrier) ||
        !r.boolean(root, "nano_owns_boot_screen", top, &c.nanoOwnsBootScreen))
        return false;
    if (const Value* d = root.find("display")) {
        const std::string w = top + ".display";
        if (!r.object(*d, w, {"width", "height", "refresh_hz", "count", "rotation", "pace"}) ||
            !r.integer(*d, "width", w, 1, 16384, &c.display.width) ||
            !r.integer(*d, "height", w, 1, 16384, &c.display.height) ||
            !r.integer(*d, "refresh_hz", w, 1, 1000, &c.display.refreshHz) ||
            !r.integer(*d, "count", w, 1, 4, &c.display.count) ||
            !r.integer(*d, "rotation", w, 0, 270, &c.display.rotationDeg) ||
            !r.boolean(*d, "pace", w, &c.display.pace))
            return false;
        if (c.display.rotationDeg % 90 != 0) return r.fail(w + ".rotation", "not 0, 90, 180 or 270");
    }
    if (const Value* devs = root.find("devices")) {
        if (!devs->isArray()) return r.fail(top + ".devices", "not an array");
        c.devices.clear();
        for (size_t i = 0; i < devs->size(); ++i) {
            evdev::Device dev;
            if (!parseDevice(r, (*devs)[i], top + ".devices[" + std::to_string(i) + "]", &dev)) return false;
            c.devices.push_back(std::move(dev));
        }
    }
    if (const Value* sh = root.find("shell")) {
        const std::string w = top + ".shell";
        if (!r.object(*sh, w, {"rules", "default_status"}) ||
            !r.integer(*sh, "default_status", w, 0, 255, &c.shellDefaultStatus))
            return false;
        if (const Value* rules = sh->find("rules")) {
            if (!rules->isArray()) return r.fail(w + ".rules", "not an array");
            c.shellRules.clear();
            for (size_t i = 0; i < rules->size(); ++i) {
                const std::string rw = w + ".rules[" + std::to_string(i) + "]";
                const Value& v = (*rules)[i];
                shell::Rule rule;
                if (!r.object(v, rw, {"match", "exact", "status", "stdout"}) || !r.str(v, "match", rw, &rule.match) ||
                    !r.boolean(v, "exact", rw, &rule.exact) || !r.integer(v, "status", rw, 0, 255, &rule.status) ||
                    !r.str(v, "stdout", rw, &rule.out))
                    return false;
                if (!v.find("match")) return r.fail(rw + ".match", "missing");
                c.shellRules.push_back(std::move(rule));
            }
        }
    }
    if (const Value* rp = root.find("record_props")) {
        if (!rp->isArray()) return r.fail(top + ".record_props", "not an array");
        c.recordProps.clear();
        for (size_t i = 0; i < rp->size(); ++i) {
            if (!(*rp)[i].isString()) return r.fail(top + ".record_props[" + std::to_string(i) + "]", "not a string");
            c.recordProps.push_back((*rp)[i].asString());
        }
    }
    if (const Value* vols = root.find("volumes")) {
        if (!vols->isArray()) return r.fail(top + ".volumes", "not an array");
        c.volumes.clear();
        for (size_t i = 0; i < vols->size(); ++i) {
            const std::string w = top + ".volumes[" + std::to_string(i) + "]";
            const Value& v = (*vols)[i];
            FakeVolume vol;
            if (!r.object(v, w, {"id", "path", "kind", "state", "boot_disk", "read_only"}) ||
                !r.str(v, "id", w, &vol.id) || !r.str(v, "path", w, &vol.path) || !r.str(v, "kind", w, &vol.kind) ||
                !r.str(v, "state", w, &vol.state) || !r.boolean(v, "boot_disk", w, &vol.bootDisk) ||
                !r.boolean(v, "read_only", w, &vol.readOnly))
                return false;
            if (vol.id.empty()) return r.fail(w + ".id", "missing");
            c.volumes.push_back(std::move(vol));
        }
    }
    if (const Value* n = root.find("network")) {
        if (!parseNetwork(r, *n, top + ".network", &c.network)) return false;
    }
    if (const Value* caps = root.find("caps")) {   // W6-caps
        const std::string w = top + ".caps";
        if (!caps->isObject()) return r.fail(w, "not an object");
        c.caps.clear();
        for (const auto& m : caps->members()) {
            nanoplat::Cap cap = nanoplat::Cap::Count;
            if (!capFromName(m.key, &cap)) return r.fail(w, "unknown capability \"" + m.key + "\"");
            if (!m.value.isBool()) return r.fail(w + "." + m.key, "not a boolean");
            c.caps.emplace_back(cap, m.value.asBool());
        }
    }
    if (const Value* st = root.find("settings")) {   // W6-caps
        const std::string w = top + ".settings";
        if (!r.object(*st, w, {"unsupported", "values"})) return false;
        if (const Value* un = st->find("unsupported")) {
            if (!un->isArray()) return r.fail(w + ".unsupported", "not an array");
            c.settings.unsupported.clear();
            for (size_t i = 0; i < un->size(); ++i) {
                const std::string ew = w + ".unsupported[" + std::to_string(i) + "]";
                if (!(*un)[i].isString()) return r.fail(ew, "not a string");
                if (!validSettingSpec((*un)[i].asString())) return r.fail(ew, "not <store> or <store>:<key>");
                c.settings.unsupported.push_back((*un)[i].asString());
            }
        }
        if (const Value* vals = st->find("values")) {
            if (!vals->isObject()) return r.fail(w + ".values", "not an object");
            c.settings.values.clear();
            for (const auto& m : vals->members()) {
                const std::string ew = w + ".values." + m.key;
                if (!m.value.isString()) return r.fail(ew, "not a string");
                const size_t colon = m.key.find(':');
                const std::string store = m.key.substr(0, colon);
                if (colon == std::string::npos || colon + 1 >= m.key.size() ||
                    (store != "global" && store != "secure" && store != "system"))
                    return r.fail(ew, "not global:<key>, secure:<key> or system:<key>");
                c.settings.values[m.key] = m.value.asString();
            }
        }
    }
    *out = std::move(c);
    return true;
}

}  // namespace nanohost
