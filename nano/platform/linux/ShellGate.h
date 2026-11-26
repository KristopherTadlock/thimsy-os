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
// (the list is nano/platform/linux/linker-wraps.txt), so every call Nano's files make to those
// functions reaches the __wrap_ functions in ShellGate.cpp. Each command is decided by the table in
// ShellGate.cpp:
//   pass    a Linux command: run as asked (after rewriting /system/bin/<posix tool> to <tool>);
//   route   an Android tool that has a Linux meaning: done here (setprop and getprop on the config
//           store, reboot and "svc power" through sys.powerctl, which thimsy-session acts on,
//           restorecon and chcon as successful no-ops: there is no SELinux);
//   refuse  any other Android tool (am, cmd, settings, svc, pm, dumpsys, ...), or a program under
//           /system, /vendor and the like that the image does not have: logged once per tool, and
//           the caller sees what a missing program gives: exit status 127 from system() and
//           pclose(), no output from popen(), and exec*() failing with ENOENT (each of Nano's
//           fork-and-exec sites then calls _exit(127) itself).
// A shell string is split into its simple commands (at ; & && || | newlines, ( ) $( ) and
// backquotes, honouring quotes): one refused command refuses the whole string, and routes are
// taken only when every command in the string is a route (setprop a 1; setprop b 2).
//
// The exec wrappers run in a forked child of a threaded process, so their decision allocates
// nothing and takes no lock (classifyExec); they log with a single write(2) to stderr.
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
    std::vector<std::string> words;   // after quote removal; assignments and redirections dropped
    std::string tool;                 // the word that decided (empty for a plain Linux command)
    Verdict verdict = Verdict::Pass;
    std::string route;                // for Route: "setprop", "getprop", "powerctl", "noop"
};

struct Decision {
    Verdict verdict = Verdict::Pass;
    std::string tool;                 // the Android tool that refused or routed it
    std::string command;              // Pass: the command to run (rewritten if needed)
    std::vector<Command> commands;    // every simple command found
};

// The decision for a system() or popen() command. Pure (apart from the existence check of an
// absolute Android path, made under root(); see setRoot).
Decision classifyCommand(std::string_view shellCommand);

// The decision for an exec. No allocation, no lock: safe between fork and exec.
enum class ExecAction {
    Pass,        // exec as asked
    PathSearch,  // exec *outFile through PATH with *outArgv (argv[0] kept)
    Rewrite,     // exec *outFile through PATH with the argv the gate built in *outArgv
    Succeed,     // a no-op route: _exit(0) in the child (restorecon, chcon)
    Refuse,      // fail with ENOENT
};
struct ExecPlan {
    ExecAction action = ExecAction::Pass;
    const char* file = nullptr;          // PathSearch, Rewrite: the program to look up on PATH
    char* const* argv = nullptr;         // PathSearch, Rewrite: the argv to pass
    const char* tool = nullptr;          // Refuse: the word that refused it (points into argv)
    char* builtArgv[16] = {};            // storage for Rewrite
};
void classifyExec(const char* path, char* const argv[], ExecPlan* plan);

// Where absolute Android paths are looked up (a wrapper in the image's curated /system/bin passes
// when it exists). "" (the default) is the real root; tests point it at a scratch tree. Set it
// before any thread runs.
void setRoot(const char* root);

// Record-only mode, for the host platform and tests: when set, system(), popen() and exec*() run
// nothing. system() returns the recorder's status; popen() gives a stream of its output, and
// pclose() its status; exec*() calls the recorder and then _exit()s with the status (it runs in a
// forked child). Not thread-safe to change while commands run.
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

// What was refused, in order, once per tool (for tests and the log).
std::vector<std::string> refusedTools();
void resetForTests();

// The status a refused or routed command gives system() and pclose() (as a shell's wait status).
inline int waitStatusFor(int exitCode) { return (exitCode & 0xff) << 8; }

}  // namespace shellgate
}  // namespace nanolinux
