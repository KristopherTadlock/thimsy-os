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

// The shell gate (docs/linux/IMPLEMENTATION.md B.5): Nano's shell commands and programs on Linux.
//
// thimsy-nano links with
//   -Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve
// (the list is nano/platform/linux/linker-wraps.txt), so every call Nano's shared files make to those
// functions reaches the __wrap_ functions in ShellGate.cpp.
//
// Owner decision OA-4 (docs/linux/DECISIONS.md): shared code expresses intent through nanoplat, and no
// Android command ever runs on Linux, even when a program of the same name exists. The gate therefore
// REFUSES by default (migration step 1, docs/linux/ARCHITECTURE-AUDIT.md). A command runs only when
//   pass    every simple command in it matches an entry of the short, temporary allow-list in
//           ShellGate.cpp (kAllow: exact shapes that are Linux-meaningful, with Linux-meaningful
//           arguments, each naming its call site and "TODO(OA-4 step 4): remove"), with no
//           redirection but to /dev/null or 2>&1, and only ; && || or newlines between them; or
//   route   every simple command in it is setprop, getprop, reboot or "svc power reboot|shutdown",
//           which only touch Thimsy's own property store (sys.powerctl is read by thimsy-session).
//           Logged once per shape as "routed (OA-4 step 5)": migration step 5's Session interface
//           replaces them.
// Everything else is refused: Android tools (am, cmd, settings, pm, ...), Android paths (/system/bin/X,
// /vendor/...), restorecon and chcon (there is no SELinux; reporting success would be a lie), wrappers
// (timeout, nice, env, su, ...), shell keywords, and any Linux program not on the allow-list. The caller
// sees what a missing program gives: exit status 127 from system() and pclose(), no output from popen(),
// and exec*() failing with ENOENT (each of Nano's fork-and-exec sites then calls _exit(127) itself).
// Every refusal is counted (refusalCount(), also from forked children: the counter lives in a shared
// mapping) and logged once per distinct command shape (refusedShapes()). Only Nano's own process logs:
// a forked child (whose stderr may be /dev/null) records its refused shape, and Nano logs it through
// ALOG on its next system(), popen(), pclose(), refusalCount() or refusedShapes() call.
//
// The platform layer's own process starts (OA-5) never go through the gate: they use platformExecve()
// below (Launcher.cpp) or their own spawn (actions/Run.cpp, ControlKeys.cpp's posix_spawn).
//
// The exec wrappers run in a forked child of a threaded process, so their decision allocates nothing
// and takes no lock that a parent thread could hold (classifyExec); they record a refusal in the
// shared mapping and log nothing.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nanolinux {
namespace shellgate {

enum class Verdict { Pass, Route, Refuse };

// One simple command of a shell string, as the gate saw it.
struct Command {
    std::vector<std::string> words;   // after quote removal; redirections dropped; for a route, from the
                                      // tool on
    std::string shape;                // the command word and its subcommand ("cmd display", "am force-stop")
    Verdict verdict = Verdict::Refuse;
    std::string route;                // for Route: "setprop", "getprop", "powerctl"
    const char* allow = nullptr;      // for Pass: the allow-list entry's call site
};

struct Decision {
    Verdict verdict = Verdict::Refuse;
    std::string shape;                // Refuse: the first refused command's shape; Route: the first route's
    std::string command;              // Pass: the command to run (as given)
    std::vector<Command> commands;    // every simple command found
};

// The decision for a system() or popen() command. Pure.
Decision classifyCommand(std::string_view shellCommand);

// The shape of a command whose words are WORDS (the command word first): the word and, when the next
// word looks like a subcommand (lower-case letters, digits, '-' and '_'), that word too.
std::string shapeOf(const std::vector<std::string>& words);

// The decision for an exec. No allocation, no lock: safe between fork and exec.
enum class ExecAction {
    Pass,     // exec as asked (an allow-list entry matched)
    Refuse,   // fail with ENOENT
};
struct ExecPlan {
    ExecAction action = ExecAction::Refuse;
    const char* allow = nullptr;      // Pass: the allow-list entry's call site
    char shape[96] = {};              // Refuse: the shape, for the log
};
void classifyExec(const char* path, char* const argv[], ExecPlan* plan);

// The platform layer's own exec (OA-5: a platform process start is the platform doing its job, not
// shared code reaching the shell). Calls the real execve; for use in a forked child.
int platformExecve(const char* path, char* const argv[], char* const envp[]);

// The temporary allow-list, for tests and the log: each entry's call site.
std::vector<std::string> allowListSites();

// Record-only mode, for tests: when set, system(), popen() and exec*() run nothing. system() returns
// the recorder's status; popen() gives a stream of its output, and pclose() its status; exec*() calls
// the recorder and then _exit()s with the status (it runs in a forked child). Not thread-safe to
// change while commands run.
struct Request {
    enum Kind { System, Popen, Exec } kind = System;
    std::string command;              // System, Popen: the shell string; Exec: the argv joined by ' '
    Decision decision;                // System, Popen
};
struct Result {
    int exitStatus = 0;               // 0..255
    std::string output;               // Popen in "r" mode
};
using Recorder = std::function<Result(const Request&)>;
void setRecorder(Recorder recorder);

// The routes, replaceable by tests: property_set and property_get by default.
struct Routes {
    std::function<bool(const std::string& key, const std::string& value)> setProperty;
    std::function<std::string(const std::string& key, const std::string& def)> getProperty;
};
void setRoutes(Routes routes);

// Every refusal so far, counted once per call (exec refusals in forked children included).
unsigned refusalCount();
// The distinct shapes refused so far, in order (each logged once; at most 128, then the log says the
// table is full once and later new shapes are only counted). Both calls first log what forked
// children refused.
std::vector<std::string> refusedShapes();
// The distinct shapes routed so far, in order (each was logged once).
std::vector<std::string> routedShapes();
void resetForTests();
// Tests only: marks the registry's lock as held by PID (as if PID died holding it).
void setRegistryLockHolderForTests(int pid);

// The status a refused or routed command gives system() and pclose() (as a shell's wait status).
inline int waitStatusFor(int exitCode) { return (exitCode & 0xff) << 8; }

}  // namespace shellgate
}  // namespace nanolinux
