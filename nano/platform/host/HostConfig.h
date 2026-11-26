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

// nano-headless's configuration: a JSON file the scenario runner writes from a scenario's "boot"
// step (tests/e2e/nano_scenario.py), named by --host-config PATH or THIMSY_HEADLESS_CONFIG. Every
// member is optional; an unknown member is an error, so a typo in the runner fails loudly.
//
//   {"events": "/run/thimsy/headless/events.jsonl",      the event log (HostControl.h)
//    "control": "/run/thimsy/headless/ctl",              the control FIFO
//    "persist_file": "/storage/.config/thimsy/persist.props",
//    "run_dir": "/run/thimsy/props",                     the property store's roots (libthimsy)
//    "log_file": "/run/thimsy/log/nano.log",             "" for stderr only
//    "display": {"width": 1280, "height": 720, "refresh_hz": 60, "count": 1, "rotation": 0,
//                "pace": true},
//    "barrier": false,                                   start with the frame barrier on
//    "devices": [{"path": "/dev/input/event0", "name": "Thimsy Gamepad", "phys": "", "uniq": "",
//                 "bus": 6, "vendor": 21576, "product": 1, "version": 1,
//                 "keys": [304, 305], "abs": [{"code": 16, "min": -1, "max": 1}], "rel": [],
//                 "props": []}],
//    "shell": {"rules": [{"match": "am start", "exact": false, "status": 0, "stdout": ""}],
//              "default_status": 127},
//    "record_props": [""]}                               key prefixes logged as {"ev":"prop"}
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "FakeEvdev.h"
#include "HostControl.h"
#include "ShellGateHost.h"

namespace nanohost {

struct HostConfig {
    std::string events;
    std::string control;
    std::string persistFile = "/storage/.config/thimsy/persist.props";
    std::string runDir = "/run/thimsy/props";
    std::string logFile = "/run/thimsy/log/nano.log";
    DisplayConfig display;
    bool barrier = false;
    std::vector<evdev::Device> devices;
    std::vector<shell::Rule> shellRules;
    int shellDefaultStatus = 127;
    std::vector<std::string> recordProps = {""};
};

// Parses `text` over the defaults in *out. false, with *err naming the member, on a syntax error,
// an unknown member or a value of the wrong type or range.
bool parseHostConfig(std::string_view text, HostConfig* out, std::string* err);

}  // namespace nanohost
