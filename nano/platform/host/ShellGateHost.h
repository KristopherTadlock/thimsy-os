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

// The host's shell gate (docs/linux/IMPLEMENTATION.md B.5): nano-headless links with
// -Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve, and these
// wrappers are record-only. They never execute anything: each call is logged as a {"ev":"shell"}
// event (HostControl.h) and answered from the scenario's rules; the runner fails a scenario on a
// command no rule names, unless the scenario says otherwise.
//
//   system(cmd)       returns the rule's status as a wait status (status << 8)
//   popen(cmd, "r")   returns a stream that reads the rule's stdout; pclose returns the wait status
//   popen(cmd, "w")   returns a stream to /dev/null; pclose returns the wait status
//   exec*(path, argv) _exit(status) at once (the exec happens in a forked child)
//
// The command a rule matches is the system/popen string, or for exec* the path followed by
// argv[1..] joined with single spaces.
#pragma once

#include <string>
#include <vector>

namespace nanohost {
namespace shell {

struct Rule {
    std::string match;        // the command starts with this (or equals it when exact)
    bool exact = false;
    int status = 0;           // exit status, 0..255
    std::string out;          // what popen(..., "r") reads
};

// Replaces the rules. A command no rule matches gets `defaultStatus` (127: "command not found")
// and an empty output.
void setRules(std::vector<Rule> rules, int defaultStatus = 127);

// The rule `command` matches (its index), or -1.
int match(const std::string& command);

}  // namespace shell
}  // namespace nanohost
