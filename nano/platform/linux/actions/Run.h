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

// Running a Linux program for the system actions (G7-power): an argv, never a shell string, started
// with posix_spawnp (not one of the shell gate's wrapped functions: these are the platform's own
// Linux commands, not Nano's Android ones), its output collected, and killed at a deadline.
#pragma once

#include <string>
#include <vector>

namespace nanolinux {
namespace actions {

struct RunResult {
    // The exit status; 128 + the signal when it was killed by one; 127 when the program could not
    // be started (not found), as a shell reports it; -1 on any other failure.
    int status = -1;
    bool timedOut = false;  // killed (SIGKILL) at the deadline
    std::string out;        // stdout and stderr together, at most 64 KiB
};

// Runs argv[0] (looked up on PATH unless it has a slash) with stdin on /dev/null. Blocks until the
// program exits or `timeoutMs` has passed.
RunResult runProgram(const std::vector<std::string>& argv, int timeoutMs);

}  // namespace actions
}  // namespace nanolinux
