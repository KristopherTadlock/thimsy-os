// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Requests from Nano (IMPLEMENTATION F.1). Nano writes the config-store key
// sys.thimsy.session.request; the session answers in sys.thimsy.session.done. The format
// (linux/docs/session.md is the contract):
//
//   request  [<seq>:]<verb>[:<arg>]     e.g. "17:eject:9602-C755", "sleep"
//   done     <seq>:ok | <seq>:error:<why>   (the verb instead of <seq> when none was given)
//
// <seq> is a decimal number Nano increments per request. A request is acted on at every write of
// the key (the session watches the file, not the value), except that a numbered request whose
// <seq> equals the last one handled is ignored. Verbs: sleep, poweroff, reboot, quit-game,
// eject:<volume id>, scrape, restart-nano, brightness:<0-100>.
//
// sys.powerctl, which Nano writes on Android ("shutdown[,reason]", "reboot[,reason]"), is read too:
// it becomes poweroff or reboot.
#pragma once

#include <string>
#include <string_view>

namespace thimsy {
namespace session {

enum class Verb { Sleep, PowerOff, Reboot, QuitGame, Eject, Scrape, RestartNano, Brightness };

const char* toString(Verb v);

struct Request {
    std::string seq;  // empty when the request had none
    Verb verb = Verb::Sleep;
    std::string arg;
};

// Parses a request value. False (with *err) for an unknown verb, a missing or unexpected argument,
// or a bad brightness.
bool parseRequest(std::string_view value, Request* out, std::string* err = nullptr);

// sys.powerctl: "shutdown", "shutdown,<reason>", "reboot", "reboot,<reason>".
bool parsePowerctl(std::string_view value, Request* out);

// The sys.thimsy.session.done value for a request and its result ("ok" or "error:<why>"), cut to
// the store's 91-byte limit.
std::string doneValue(const Request& r, std::string_view result);

// Drops a numbered request that repeats the last one handled.
class RequestFilter {
 public:
    bool accept(const Request& r);

 private:
    std::string lastSeq_;
};

}  // namespace session
}  // namespace thimsy
