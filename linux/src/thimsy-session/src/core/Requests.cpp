// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Requests.h"

namespace thimsy {
namespace session {
namespace {

struct VerbName {
    const char* name;
    Verb verb;
    bool needsArg;
};

constexpr VerbName kVerbs[] = {
    {"sleep", Verb::Sleep, false},
    {"poweroff", Verb::PowerOff, false},
    {"reboot", Verb::Reboot, false},
    {"quit-game", Verb::QuitGame, false},
    {"eject", Verb::Eject, true},
    {"scrape", Verb::Scrape, false},
    {"restart-nano", Verb::RestartNano, false},
    {"brightness", Verb::Brightness, true},
    {"take-ownership", Verb::TakeOwnership, true},
};

bool allDigits(std::string_view s) {
    if (s.empty() || s.size() > 18) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

}  // namespace

const char* toString(Verb v) {
    for (const VerbName& n : kVerbs) {
        if (n.verb == v) return n.name;
    }
    return "?";
}

bool parseRequest(std::string_view value, Request* out, std::string* err) {
    std::string_view s = trim(value);
    Request r;
    size_t colon = s.find(':');
    if (colon != std::string_view::npos && allDigits(s.substr(0, colon))) {
        r.seq = std::string(s.substr(0, colon));
        s.remove_prefix(colon + 1);
    }
    std::string_view verb = s;
    std::string_view arg;
    bool hasArg = false;
    colon = s.find(':');
    if (colon != std::string_view::npos) {
        verb = s.substr(0, colon);
        arg = s.substr(colon + 1);
        hasArg = true;
    }
    const VerbName* found = nullptr;
    for (const VerbName& n : kVerbs) {
        if (verb == n.name) found = &n;
    }
    if (!found) return fail(err, "unknown request '" + std::string(verb) + "'");
    if (found->needsArg && (!hasArg || arg.empty())) return fail(err, std::string(found->name) + " needs an argument");
    if (!found->needsArg && hasArg) return fail(err, std::string(found->name) + " takes no argument");
    r.verb = found->verb;
    r.arg = std::string(arg);
    if (r.verb == Verb::Brightness) {
        if (!allDigits(r.arg) || r.arg.size() > 3 || std::stoi(r.arg) > 100) {
            return fail(err, "brightness must be 0 to 100");
        }
    }
    if (r.verb == Verb::Eject || r.verb == Verb::TakeOwnership) {
        for (char c : r.arg) {
            bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
            if (!ok) return fail(err, "bad volume id");
        }
    }
    *out = r;
    return true;
}

bool parsePowerctl(std::string_view value, Request* out) {
    std::string_view s = trim(value);
    std::string_view word = s.substr(0, s.find(','));
    Request r;
    if (word == "shutdown") {
        r.verb = Verb::PowerOff;
    } else if (word == "reboot") {
        r.verb = Verb::Reboot;
    } else {
        return false;
    }
    *out = r;
    return true;
}

std::string doneValue(const Request& r, std::string_view result) {
    std::string v = (r.seq.empty() ? std::string(toString(r.verb)) : r.seq) + ":" + std::string(result);
    if (v.size() > 91) v.resize(91);
    return v;
}

bool RequestFilter::accept(const Request& r) {
    if (r.seq.empty()) return true;
    if (r.seq == lastSeq_) return false;
    lastSeq_ = r.seq;
    return true;
}

}  // namespace session
}  // namespace thimsy
