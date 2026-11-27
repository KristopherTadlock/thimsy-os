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

// The host's fake settings (FakeSettings.h).

#define LOG_TAG "NanoHost"

#include "FakeSettings.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <cutils/properties.h>
#include <log/log.h>

#include "HostControl.h"

namespace nanohost {

namespace {

using nanoplat::SettingSource;

std::mutex gRecordMu;
SettingsRecord gRecord;  // guarded by gRecordMu

const SettingSource kStores[] = {SettingSource::kProp, SettingSource::kGlobal, SettingSource::kSecure,
                                 SettingSource::kSystem};

// The system settings Linux implements itself, kept as the properties Linux keeps them in
// (platform/linux/Settings.cpp's kNative; test_nano_ratchets checks the two lists agree), so a
// scenario sees such a write land in persist.props as it would on Linux.
struct LinuxNativeSetting {
    SettingSource source;
    const char* key;
    const char* storeKey;
};
constexpr LinuxNativeSetting kLinuxNative[] = {
    {SettingSource::kSystem, "font_scale", "persist.gammaos.nano.fontscale"},
    {SettingSource::kSystem, "screen_off_timeout", "persist.thimsy.system.screen_off_timeout"},
};

// The property (source, key) is also kept in, or nullptr.
const char* linuxStoreKey(SettingSource source, const std::string& key) {
    for (const LinuxNativeSetting& n : kLinuxNative) {
        if (n.source == source && key == n.key) return n.storeKey;
    }
    return nullptr;
}

class FakeSettings : public nanoplat::Settings {
 public:
    explicit FakeSettings(const FakeSettingsConfig& config)
        : unsupported_(config.unsupported.begin(), config.unsupported.end()), tables_(config.values) {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord = SettingsRecord();
    }

    bool supports(SettingSource source, const std::string& key) const override {
        if (source == SettingSource::kNone || key.empty()) return false;
        const std::string store = settingStoreName(source);
        return unsupported_.count(store) == 0 && unsupported_.count(store + ":" + key) == 0;
    }

    std::string read(SettingSource source, const std::string& key, const std::string& def) override {
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.reads++;
        }
        if (!supports(source, key)) return def;
        if (source == SettingSource::kProp) {
            char buf[PROPERTY_VALUE_MAX] = {};
            property_get(key.c_str(), buf, def.c_str());
            return buf;
        }
        if (const char* storeKey = linuxStoreKey(source, key)) {
            char buf[PROPERTY_VALUE_MAX] = {};
            property_get(storeKey, buf, "");
            if (buf[0] != '\0') return buf;   // else the config's seed, as for any table setting
        }
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tables_.find(std::string(settingStoreName(source)) + ":" + key);
        return it == tables_.end() || it->second.empty() ? def : it->second;
    }

    bool write(SettingSource source, const std::string& key, const std::string& value) override {
        const std::string fields = "\"source\":" + jsonQuote(settingStoreName(source)) + ",\"key\":" + jsonQuote(key);
        if (!supports(source, key)) {
            {
                std::lock_guard<std::mutex> lock(gRecordMu);
                gRecord.refused++;
            }
            event("setting", "\"op\":\"refused\"," + fields);
            return false;
        }
        if (source == SettingSource::kProp) {
            if (property_set(key.c_str(), value.c_str()) != 0) return false;
        } else {
            const char* storeKey = linuxStoreKey(source, key);
            if (storeKey && property_set(storeKey, value.c_str()) != 0) return false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                tables_[std::string(settingStoreName(source)) + ":" + key] = value;
            }
            event("setting", "\"op\":\"put\"," + fields + ",\"value\":" + jsonQuote(value));
        }
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.writes++;
        return true;
    }

 private:
    const std::set<std::string> unsupported_;
    std::mutex mu_;
    std::map<std::string, std::string> tables_;  // guarded by mu_: "<store>:<key>" -> value
};

}  // namespace

std::unique_ptr<nanoplat::Settings> createFakeSettings(const FakeSettingsConfig& config) {
    return std::unique_ptr<nanoplat::Settings>(new FakeSettings(config));
}

SettingsRecord settingsRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

const char* settingStoreName(nanoplat::SettingSource source) {
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
    return "none";
}

bool validSettingSpec(const std::string& spec) {
    const size_t colon = spec.find(':');
    const std::string store = spec.substr(0, colon);
    if (colon != std::string::npos && colon + 1 >= spec.size()) return false;   // "global:" names no key
    for (nanoplat::SettingSource s : kStores) {
        if (store == settingStoreName(s)) return true;
    }
    return false;
}

}  // namespace nanohost
