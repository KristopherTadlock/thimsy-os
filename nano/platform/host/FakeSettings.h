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

// The host's nanoplat::Settings (NanoPlatform.h; W6-caps), configured by the host config's
// "settings" object (HostConfig.h):
//
//   "settings": {"unsupported": ["global", "secure:location_mode", "prop:persist.gammaos.rgb.effect"],
//                "values": {"system:screen_off_timeout": "300000"}}
//
// Every setting is supported unless "unsupported" names its store ("global") or the store and key
// ("secure:location_mode"); the stores are prop, global, secure and system. kNone and an empty key
// are never supported. kProp settings live in the property store (so a write shows as the host's
// {"ev":"prop"} event); the global, secure and system tables are kept in memory, starting from
// "values", and each write to them is emitted as {"ev":"setting","op":"put","source":...,"key":...,
// "value":...}. A write to an unsupported setting stores nothing, is emitted as {"ev":"setting",
// "op":"refused","source":...,"key":...} and returns false; a read of one gives the default. No
// post-write hooks run (they are the firmware's). Every call is recorded (settingsRecord()). Any
// thread.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct FakeSettingsConfig {
    std::vector<std::string> unsupported;       // "<store>" or "<store>:<key>"
    std::map<std::string, std::string> values;  // "<store>:<key>" -> value, for global, secure, system
};

// What the fake settings have been asked, for tests.
struct SettingsRecord {
    int reads = 0;
    int writes = 0;      // writes the fake took
    int refused = 0;     // writes to unsupported settings
};

std::unique_ptr<nanoplat::Settings> createFakeSettings(const FakeSettingsConfig& config);

// The record of the most recently made fake settings (zeros before one is made).
SettingsRecord settingsRecord();

// The store's name in the config ("prop", "global", "secure", "system"; "none" for kNone).
const char* settingStoreName(nanoplat::SettingSource source);
// Whether `spec` is a valid "unsupported" entry: a store's name, or a store's name, ':' and a key.
bool validSettingSpec(const std::string& spec);

}  // namespace nanohost
