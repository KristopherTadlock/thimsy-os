// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/SleepPlan.h"

#include <signal.h>

#include <sstream>

namespace thimsy {
namespace session {
namespace {

// The words of a sysfs choice file, brackets removed: "s2idle [deep]" -> {s2idle, deep}.
std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) {
        if (w.size() >= 2 && w.front() == '[' && w.back() == ']') w = w.substr(1, w.size() - 2);
        out.push_back(w);
    }
    return out;
}

bool offers(const std::string& text, const std::string& word) {
    for (const std::string& w : words(text)) {
        if (w == word) return true;
    }
    return false;
}

Step prop(const char* key, const char* value) {
    Step s(StepKind::SetProp, key, value);
    return s;
}

}  // namespace

std::string chooseMemSleep(const std::string& offered, const std::string& preferred) {
    std::string want = preferred.empty() ? "deep" : preferred;
    if (offers(offered, want)) return want;
    if (offers(offered, "s2idle")) return "s2idle";
    return "";
}

std::string chooseState(const std::string& offered) {
    if (offers(offered, "mem")) return "mem";
    if (offers(offered, "freeze")) return "freeze";
    return "";
}

SleepPlan planSleep(const SleepContext& ctx) {
    SleepPlan p;
    p.sleep.push_back(prop("sys.thimsy.power.state", "sleeping"));
    if (ctx.gamePgid > 0) {
        Step s(StepKind::SignalGroup);
        s.pgid = ctx.gamePgid;
        s.signal = SIGSTOP;
        p.sleep.push_back(s);
    }
    if (ctx.wifiRunning) p.sleep.push_back(Step(StepKind::Script, "D30net", "stop"));
    if (ctx.hasBacklight) p.sleep.push_back(Step(StepKind::BacklightOff));
    p.sleep.push_back(Step(StepKind::Sync));
    if (ctx.hasFamilyHook) p.sleep.push_back(Step(StepKind::Hook, "", "sleep-pre"));
    std::string mode = chooseMemSleep(ctx.memSleepOffered, ctx.preferredMemSleep);
    if (!mode.empty()) p.sleep.push_back(Step(StepKind::WriteSysfs, "/sys/power/mem_sleep", mode));
    std::string state = chooseState(ctx.stateOffered);
    if (!state.empty()) p.sleep.push_back(Step(StepKind::Suspend, "/sys/power/state", state));

    if (ctx.hasFamilyHook) p.wake.push_back(Step(StepKind::Hook, "", "sleep-post"));
    if (ctx.hasBacklight) p.wake.push_back(Step(StepKind::BacklightRestore));
    if (ctx.gamePgid > 0) {
        Step s(StepKind::SignalGroup);
        s.pgid = ctx.gamePgid;
        s.signal = SIGCONT;
        p.wake.push_back(s);
    }
    if (ctx.wifiRunning) p.wake.push_back(Step(StepKind::Script, "D30net", "start"));
    p.wake.push_back(prop("sys.thimsy.power.state", "awake"));
    return p;
}

std::string describe(const Step& s) {
    switch (s.kind) {
        case StepKind::SetProp: return "set " + s.target + "=" + s.value;
        case StepKind::SignalGroup:
            return "signal group " + std::to_string(s.pgid) + " " +
                   (s.signal == SIGSTOP ? "STOP" : s.signal == SIGCONT ? "CONT" : std::to_string(s.signal));
        case StepKind::Script: return "script " + s.target + " " + s.value;
        case StepKind::Hook: return "hook " + s.value;
        case StepKind::BacklightOff: return "backlight off";
        case StepKind::BacklightRestore: return "backlight restore";
        case StepKind::Sync: return "sync";
        case StepKind::WriteSysfs: return "write " + s.target + " " + s.value;
        case StepKind::Suspend: return "suspend " + s.target + " " + s.value;
    }
    return "?";
}

}  // namespace session
}  // namespace thimsy
