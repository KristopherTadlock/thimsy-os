// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Processes: start (fork and exec, with a hook run in the child before exec), wait, signal, and
// watch a process that is not our child (pidfd).
#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

namespace thimsy {
namespace session {

struct SpawnSpec {
    std::vector<std::string> argv;   // argv[0] is the program: a path, or a name looked up in PATH
    std::vector<std::string> env;    // extra NAME=VALUE entries
    std::string logFile;             // stdout and stderr appended here; empty: /dev/null
    bool newSession = true;          // setsid(): its own process group (pgid == pid)
    int nice = 0;
    int oomScoreAdj = 0;             // written when non-zero
    bool ioprioIdle = false;         // best-effort class, lowest level (gammaos-nano.rc's ioprio be 7)
    // Runs in the child before exec; false (with *err) makes the child exit 126 without exec.
    std::function<bool(std::string* err)> preExec;
};

// Starts the program. Returns its pid, or -1 with *err (fork failed). An exec failure shows as
// the child exiting 127, with the reason in the log file.
pid_t spawn(const SpawnSpec& spec, std::string* err = nullptr);

// Starts and waits up to timeoutMs (then SIGKILL to its group). Returns the exit status, 128 + the
// signal for a signal death, or -1 if it could not be started.
int runWait(const SpawnSpec& spec, int timeoutMs, std::string* err = nullptr);

// An exit status from waitpid as one number: the exit code, or -1 for a death by signal.
int exitCodeOf(int waitStatus);

bool alive(pid_t pid);
// A pid read from a file ("123\n"); 0 when missing or malformed.
pid_t readPidFile(const std::string& path);
// /proc/<pid>/cmdline with NULs as spaces; empty if gone.
std::string cmdline(pid_t pid, const std::string& procRoot = "");
// A pidfd for any process (Linux 5.3+); -1 if unavailable.
int pidfdOpen(pid_t pid);

}  // namespace session
}  // namespace thimsy
