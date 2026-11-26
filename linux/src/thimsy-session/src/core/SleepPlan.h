// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Sleep and wake as an ordered list of steps (IMPLEMENTATION F.1, plan 5.2: the NextUI and
// FlipBase sequence). Pure: the context says what the device has; the loop runs the steps in
// order. The Suspend step is the write to /sys/power/state, which returns only after the device
// has woken; the wake steps follow it.
//
// Sleep: tell Nano (sys.thimsy.power.state=sleeping); SIGSTOP the game's process group; stop Wi-Fi
// (the deferred D30net script, when wpa_supplicant runs); backlight to 0; sync; the family hook's
// sleep-pre; mem_sleep; state.
// Wake, the reverse: the hook's sleep-post; backlight back; SIGCONT the game; Wi-Fi again; tell
// Nano (awake).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace thimsy {
namespace session {

struct SleepContext {
    int gamePgid = 0;               // the running game's process group; 0 for none
    bool wifiRunning = false;       // /run/wpa_supplicant.pid names a live process
    bool hasFamilyHook = false;     // /usr/share/thimsy/family/<family>/hook exists
    bool hasBacklight = false;      // a /sys/class/backlight device exists
    std::string memSleepOffered;    // /sys/power/mem_sleep, e.g. "s2idle [deep]"; empty if absent
    std::string stateOffered;       // /sys/power/state, e.g. "freeze mem"; empty if absent
    std::string preferredMemSleep;  // device.json power.mem_sleep; empty means "deep"
};

enum class StepKind {
    SetProp,           // target = key, value
    SignalGroup,       // pgid, signal
    Script,            // target = a deferred script under /etc/init.d/deferred, value = start|stop
    Hook,              // value = the family hook's action
    BacklightOff,
    BacklightRestore,
    Sync,
    WriteSysfs,        // target = path under /sys, value
    Suspend,           // target = /sys/power/state, value = mem|freeze; blocks until wake
};

struct Step {
    Step(StepKind k, std::string t = {}, std::string v = {})
        : kind(k), target(std::move(t)), value(std::move(v)) {}
    StepKind kind;
    std::string target;
    std::string value;
    int pgid = 0;
    int signal = 0;
};

struct SleepPlan {
    std::vector<Step> sleep;
    std::vector<Step> wake;
};

SleepPlan planSleep(const SleepContext& ctx);

// The mem_sleep mode to write: the preferred one ("deep" by default) if the kernel offers it, else
// "s2idle" if offered, else empty (leave the file alone).
std::string chooseMemSleep(const std::string& offered, const std::string& preferred);
// "mem" if /sys/power/state offers it, else "freeze", else empty (the device can't sleep).
std::string chooseState(const std::string& offered);

// One line for the log, e.g. "signal group 1234 STOP", "write /sys/power/state mem".
std::string describe(const Step& s);

}  // namespace session
}  // namespace thimsy
