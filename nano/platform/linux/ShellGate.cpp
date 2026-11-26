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

// The shell gate: see ShellGate.h and docs/linux/IMPLEMENTATION.md B.5.

#define LOG_TAG "ThimsyShellGate"

#include "ShellGate.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <cutils/properties.h>
#include <log/log.h>

extern "C" {
int __real_system(const char* command);
FILE* __real_popen(const char* command, const char* mode);
int __real_pclose(FILE* stream);
int __real_execv(const char* path, char* const argv[]);
int __real_execvp(const char* file, char* const argv[]);
int __real_execve(const char* path, char* const argv[], char* const envp[]);
}

namespace nanolinux {
namespace shellgate {
namespace {

// ---------------------------------------------------------------------------------------- tables
// One table, read by both the shell-string and the exec decisions. A row's word is a bare command
// name; an absolute Android path (/system/bin/am) is looked up by its last component.

enum class Kind { Refuse, RouteSetprop, RouteGetprop, RouteReboot, RouteSvc, Noop, GammaosNet, Nsenter };

struct ToolRow {
    const char* word;
    Kind kind;
};

// Android-only tools (B.5). Anything listed and not routed is refused.
constexpr ToolRow kAndroidTools[] = {
    {"am", Kind::Refuse},          {"cmd", Kind::Refuse},        {"settings", Kind::Refuse},
    {"svc", Kind::RouteSvc},       {"pm", Kind::Refuse},         {"dumpsys", Kind::Refuse},
    {"appops", Kind::Refuse},      {"input", Kind::Refuse},      {"setprop", Kind::RouteSetprop},
    {"getprop", Kind::RouteGetprop}, {"screencap", Kind::Refuse}, {"ime", Kind::Refuse},
    {"locksettings", Kind::Refuse}, {"gammaos-net", Kind::GammaosNet}, {"nsenter", Kind::Nsenter},
    {"monkey", Kind::Refuse},      {"wm", Kind::Refuse},         {"content", Kind::Refuse},
    {"service", Kind::Refuse},     {"logcat", Kind::Refuse},     {"bmgr", Kind::Refuse},
    {"sm", Kind::Refuse},          {"vdc", Kind::Refuse},        {"ndc", Kind::Refuse},
    {"setenforce", Kind::Refuse},  {"getenforce", Kind::Refuse}, {"start", Kind::Refuse},
    {"stop", Kind::Refuse},        {"dumpstate", Kind::Refuse},  {"bugreport", Kind::Refuse},
    {"bugreportz", Kind::Refuse},  {"lshal", Kind::Refuse},      {"uiautomator", Kind::Refuse},
    {"app_process", Kind::Refuse}, {"app_process64", Kind::Refuse}, {"dalvikvm", Kind::Refuse},
    {"reboot", Kind::RouteReboot}, {"restorecon", Kind::Noop},   {"chcon", Kind::Noop},
};

// Tools a Linux image has under the same name: /system/bin/<tool> becomes <tool> (found on PATH).
constexpr const char* kPosixTools[] = {
    "awk", "basename", "cat", "chmod", "chown", "cmp", "cp", "curl", "cut", "date", "dd", "df",
    "dirname", "du", "echo", "env", "expr", "false", "find", "free", "grep", "gunzip", "gzip",
    "head", "hostname", "id", "ifconfig", "ip", "kill", "killall", "ln", "ls", "md5sum", "mkdir",
    "mount", "mv", "nice", "pidof", "ping", "printf", "ps", "readlink", "realpath", "rm", "rmdir",
    "sed", "sh", "sha1sum", "sha256sum", "sleep", "sort", "stat", "sync", "tail", "tar", "tee",
    "test", "timeout", "top", "touch", "tr", "true", "umount", "uname", "uniq", "unzip", "uptime",
    "wc", "wget", "which", "xargs",
};

// Absolute roots that belong to Android. A program under them passes only if the image has it.
constexpr const char* kAndroidRoots[] = {
    "/system/", "/vendor/", "/product/", "/odm/", "/apex/", "/system_ext/",
};

const ToolRow* findTool(const char* word) {
    for (const ToolRow& r : kAndroidTools)
        if (strcmp(r.word, word) == 0) return &r;
    return nullptr;
}

bool isPosixTool(const char* word) {
    for (const char* t : kPosixTools)
        if (strcmp(t, word) == 0) return true;
    return false;
}

bool underAndroidRoot(const char* path) {
    for (const char* r : kAndroidRoots)
        if (strncmp(path, r, strlen(r)) == 0) return true;
    return false;
}

// /system/bin/<x> or /system/xbin/<x>: the tool directories Nano names.
bool inSystemBin(const char* path) {
    return strncmp(path, "/system/bin/", 12) == 0 || strncmp(path, "/system/xbin/", 13) == 0;
}

const char* baseName(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char gRoot[PATH_MAX] = "";

// Whether root()+path is an executable file. Async-signal-safe.
bool imageHas(const char* path) {
    char buf[PATH_MAX];
    size_t rl = strlen(gRoot), pl = strlen(path);
    if (rl + pl + 1 > sizeof buf) return false;
    memcpy(buf, gRoot, rl);
    memcpy(buf + rl, path, pl + 1);
    return access(buf, X_OK) == 0;
}

// ------------------------------------------------------------------------------ state and logging

std::mutex gMu;
std::vector<std::string> gRefused;            // once per tool, in order
std::set<std::string> gRefusedSet;
Recorder gRecorder;
Routes gRoutes;
std::map<FILE*, int> gSynthetic;              // popen() streams the gate made: their wait status

void noteRefused(const std::string& tool, const std::string& what) {
    bool first;
    {
        std::lock_guard<std::mutex> l(gMu);
        first = gRefusedSet.insert(tool).second;
        if (first) gRefused.push_back(tool);
    }
    if (first)
        ALOGW("refused (Android-only, logged once per tool): %s: %s", tool.c_str(), what.c_str());
}

bool routeSet(const std::string& key, const std::string& value) {
    Routes r;
    {
        std::lock_guard<std::mutex> l(gMu);
        r = gRoutes;
    }
    if (r.setProperty) return r.setProperty(key, value);
    return property_set(key.c_str(), value.c_str()) == 0;
}

std::string routeGet(const std::string& key, const std::string& def) {
    Routes r;
    {
        std::lock_guard<std::mutex> l(gMu);
        r = gRoutes;
    }
    if (r.getProperty) return r.getProperty(key, def);
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key.c_str(), v, def.c_str());
    return v;
}

// ------------------------------------------------------------------------------- shell parsing

struct Token {
    enum Type { Word, Sep, Redir } type;
    std::string text;      // Word: after quote removal; Sep: the operator
    size_t begin, end;     // span in the source
};

bool isOpChar(char c) { return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '`' || c == '\n'; }

std::vector<Token> lexShell(std::string_view s) {
    std::vector<Token> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#') {   // a comment to the end of the line
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '$' && i + 1 < n && s[i + 1] == '(') {
            out.push_back({Token::Sep, "$(", i, i + 2});
            i += 2;
            continue;
        }
        if (c == '>' || c == '<' || (c == '&' && i + 1 < n && s[i + 1] == '>')) {
            size_t b = i;
            if (c == '&') i++;
            i++;
            while (i < n && (s[i] == '>' || s[i] == '<' || s[i] == '&' || s[i] == '|')) i++;
            out.push_back({Token::Redir, std::string(s.substr(b, i - b)), b, i});
            continue;
        }
        if (isOpChar(c)) {
            size_t b = i;
            if ((c == '&' || c == '|' || c == ';') && i + 1 < n && s[i + 1] == c) i++;
            i++;
            out.push_back({Token::Sep, std::string(s.substr(b, i - b)), b, i});
            continue;
        }
        // A word: up to an unquoted blank or operator.
        Token t{Token::Word, "", i, i};
        bool digitsOnly = true;
        while (i < n) {
            c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || isOpChar(c) || c == '>' || c == '<') break;
            if (c == '$' && i + 1 < n && s[i + 1] == '(') break;
            if (c == '\\' && i + 1 < n) {
                t.text += s[i + 1];
                i += 2;
                digitsOnly = false;
            } else if (c == '\'') {
                size_t e = s.find('\'', i + 1);
                if (e == std::string_view::npos) e = n;
                t.text.append(s.substr(i + 1, e - i - 1));
                i = e < n ? e + 1 : n;
                digitsOnly = false;
            } else if (c == '"') {
                i++;
                while (i < n && s[i] != '"') {
                    if (s[i] == '\\' && i + 1 < n && strchr("\"\\$`\n", s[i + 1])) {
                        t.text += s[i + 1];
                        i += 2;
                    } else {
                        t.text += s[i++];
                    }
                }
                if (i < n) i++;
                digitsOnly = false;
            } else {
                if (c < '0' || c > '9') digitsOnly = false;
                t.text += c;
                i++;
            }
        }
        t.end = i;
        // "2>": a descriptor number glued to a redirection belongs to it.
        if (digitsOnly && i < n && (s[i] == '>' || s[i] == '<')) {
            size_t b = t.begin;
            while (i < n && (s[i] == '>' || s[i] == '<' || s[i] == '&')) i++;
            out.push_back({Token::Redir, std::string(s.substr(b, i - b)), b, i});
            continue;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool isAssignment(const std::string& w) {
    if (w.empty() || !(isalpha((unsigned char)w[0]) || w[0] == '_')) return false;
    for (size_t k = 1; k < w.size(); k++) {
        if (w[k] == '=') return true;
        if (!(isalnum((unsigned char)w[k]) || w[k] == '_')) return false;
    }
    return false;
}

struct Edit {
    size_t begin, end;
    std::string with;
};

struct Simple {
    std::vector<const Token*> words;   // in order, redirections and their targets dropped
    std::string connector;             // the separator before it ("" for the first)
};

std::vector<Simple> splitSimple(const std::vector<Token>& toks) {
    std::vector<Simple> out;
    Simple cur;
    bool skipNext = false;
    for (const Token& t : toks) {
        if (t.type == Token::Sep) {
            if (!cur.words.empty()) out.push_back(cur);
            cur = Simple();
            cur.connector = t.text;
            skipNext = false;
            continue;
        }
        if (t.type == Token::Redir) {
            // The lexer leaves every redirection's target ("/dev/null", the "1" of "2>&1") as the
            // next word.
            skipNext = true;
            continue;
        }
        if (skipNext) { skipNext = false; continue; }
        cur.words.push_back(&t);
    }
    if (!cur.words.empty()) out.push_back(cur);
    return out;
}

Decision classifyImpl(std::string_view s, int depth);

// Decides one simple command; appends rewrites of the top-level string to *edits.
Command decideSimple(const Simple& sc, std::vector<Edit>* edits, int depth) {
    Command c;
    for (const Token* t : sc.words) c.words.push_back(t->text);
    size_t k = 0;
    const auto& w = sc.words;
    // Leading assignments, then wrappers that run their arguments.
    for (;;) {
        while (k < w.size() && isAssignment(w[k]->text)) k++;
        if (k >= w.size()) return c;
        const std::string& x = w[k]->text;
        if (x == "exec" || x == "nohup" || x == "command" || x == "busybox") { k++; continue; }
        if (x == "toybox" || x == "toolbox") {   // Android's multi-call binaries: the applet itself
            if (edits) edits->push_back({w[k]->begin, w[k]->end, ""});
            k++;
            continue;
        }
        if (x == "env") {
            k++;
            while (k < w.size() && (w[k]->text.size() > 1 && w[k]->text[0] == '-')) k++;
            continue;
        }
        if (x == "nice") {
            k++;
            if (k < w.size() && w[k]->text == "-n") k += 2;
            else if (k < w.size() && w[k]->text.size() > 1 && w[k]->text[0] == '-') k++;
            continue;
        }
        if (x == "timeout") {
            k++;
            while (k < w.size() && w[k]->text.size() > 1 && w[k]->text[0] == '-') k++;
            if (k < w.size()) k++;   // the duration
            continue;
        }
        break;
    }
    if (k >= w.size()) return c;
    const Token* first = w[k];
    std::string word = first->text;
    std::vector<std::string> args;
    for (size_t j = k + 1; j < w.size(); j++) args.push_back(w[j]->text);

    std::string name = word;
    if (word.find('/') != std::string::npos) {
        if (!underAndroidRoot(word.c_str())) return c;   // a Linux path
        name = baseName(word.c_str());
        if (inSystemBin(word.c_str()) && isPosixTool(name.c_str()) && !findTool(name.c_str())) {
            if (edits) edits->push_back({first->begin, first->end, name});
            word = name;   // fall through to the sh -c check below
        } else if (!findTool(name.c_str())) {
            if (imageHas(word.c_str())) return c;   // a wrapper in the curated /system/bin
            c.tool = word;
            c.verdict = Verdict::Refuse;
            return c;
        }
    }
    // sh -c STRING: decide the string too (it is not rewritten).
    if ((word == "sh" || word == "bash") && depth < 4) {
        for (size_t j = 0; j + 1 < args.size(); j++) {
            if (args[j] == "-c") {
                Decision inner = classifyImpl(args[j + 1], depth + 1);
                if (inner.verdict != Verdict::Pass) {
                    c.tool = inner.tool;
                    c.verdict = Verdict::Refuse;   // routes are not taken inside sh -c
                }
                break;
            }
        }
        return c;
    }
    const ToolRow* row = findTool(name.c_str());
    if (!row) return c;
    c.tool = name;
    switch (row->kind) {
        case Kind::RouteSetprop:
            c.verdict = args.empty() ? Verdict::Refuse : Verdict::Route;
            c.route = "setprop";
            break;
        case Kind::RouteGetprop:
            c.verdict = args.empty() ? Verdict::Refuse : Verdict::Route;
            c.route = "getprop";
            break;
        case Kind::RouteReboot:
            c.verdict = Verdict::Route;
            c.route = "powerctl";
            break;
        case Kind::RouteSvc:
            if (args.size() >= 2 && args[0] == "power" && (args[1] == "reboot" || args[1] == "shutdown")) {
                c.verdict = Verdict::Route;
                c.route = "powerctl";
            } else {
                c.verdict = Verdict::Refuse;
                c.tool = args.empty() ? "svc" : "svc " + args[0];
            }
            break;
        case Kind::Noop:
            c.verdict = Verdict::Route;
            c.route = "noop";
            break;
        default:
            c.verdict = Verdict::Refuse;
            break;
    }
    // Keep only the tool and its arguments in words, for the routes.
    c.words.assign(c.words.begin() + k, c.words.end());
    return c;
}

Decision classifyImpl(std::string_view s, int depth) {
    Decision d;
    std::vector<Token> toks = lexShell(s);
    std::vector<Simple> simple = splitSimple(toks);
    std::vector<Edit> edits;
    bool anyRoute = false, anyPass = false, complex = false;
    for (const Simple& sc : simple) {
        Command c = decideSimple(sc, depth == 0 ? &edits : nullptr, depth);
        if (c.verdict == Verdict::Refuse && d.verdict != Verdict::Refuse) {
            d.verdict = Verdict::Refuse;
            d.tool = c.tool;
        }
        if (c.verdict == Verdict::Route) {
            anyRoute = true;
            if (d.tool.empty()) d.tool = c.tool;
        }
        if (c.verdict == Verdict::Pass) anyPass = true;
        if (!sc.connector.empty() && sc.connector != ";" && sc.connector != "&&" && sc.connector != "||" &&
            sc.connector != "\n")
            complex = true;
        d.commands.push_back(std::move(c));
    }
    if (d.verdict != Verdict::Refuse && anyRoute) {
        if (anyPass || complex) {
            // A route mixed with Linux commands, a pipe or a subshell: not taken apart.
            d.verdict = Verdict::Refuse;
        } else {
            d.verdict = Verdict::Route;
        }
    }
    if (d.verdict == Verdict::Pass) {
        d.tool.clear();
        std::string out(s);
        std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin > b.begin; });
        for (const Edit& e : edits) out.replace(e.begin, e.end - e.begin, e.with);
        d.command = out;
    }
    return d;
}

// Runs a Route decision: (exit code, standard output).
std::pair<int, std::string> runRoutes(const Decision& d) {
    // In order; the status is the last command's. A route string only has ; && || and newlines
    // between its commands (classifyImpl refuses the rest), and every route but a failed setprop
    // succeeds, so && and || are not told apart.
    int last = 0;
    std::string out;
    for (size_t i = 0; i < d.commands.size(); i++) {
        const Command& c = d.commands[i];
        const std::vector<std::string>& a = c.words;
        int rc = 0;
        if (c.route == "setprop") {
            rc = routeSet(a.size() > 1 ? a[1] : "", a.size() > 2 ? a[2] : "") ? 0 : 1;
        } else if (c.route == "getprop") {
            out += routeGet(a.size() > 1 ? a[1] : "", a.size() > 2 ? a[2] : "");
            out += "\n";
        } else if (c.route == "powerctl") {
            std::string v;
            if (c.tool == "svc") {   // the tool's name: also /system/bin/svc
                v = a[2] == "shutdown" ? "shutdown" : "reboot";
                if (a.size() > 3) v += "," + a[3];
            } else {   // reboot [-p | reason]
                v = "reboot";
                for (size_t j = 1; j < a.size(); j++) {
                    if (a[j] == "-p") v = "shutdown";
                    else if (!a[j].empty() && a[j][0] != '-') v += "," + a[j];
                }
            }
            ALOGI("routed %s to sys.powerctl=%s", a[0].c_str(), v.c_str());
            rc = routeSet("sys.powerctl", v) ? 0 : 1;
        }
        last = rc;
    }
    return {last, out};
}

FILE* syntheticStream(const char* mode, const std::string& output, int waitStatus) {
    FILE* f = nullptr;
    if (mode && mode[0] == 'w') {
        f = fopen("/dev/null", "we");
    } else {
        f = tmpfile();
        if (f) {
            if (!output.empty()) fwrite(output.data(), 1, output.size(), f);
            rewind(f);
        }
    }
    if (!f) return nullptr;
    std::lock_guard<std::mutex> l(gMu);
    gSynthetic[f] = waitStatus;
    return f;
}

std::string joinArgv(char* const argv[]) {
    std::string s;
    for (size_t i = 0; argv && argv[i]; i++) {
        if (i) s += ' ';
        s += argv[i];
    }
    return s;
}

// Logs a refused exec from a forked child: one write(2), no allocation.
void writeRefusedExec(const char* tool) {
    char buf[512];
    const char* pre = "thimsy-shellgate: refused exec (Android-only): ";
    size_t n = 0;
    for (const char* p = pre; *p && n < sizeof buf - 2; p++) buf[n++] = *p;
    for (const char* p = tool ? tool : "?"; *p && n < sizeof buf - 2; p++) buf[n++] = *p;
    buf[n++] = '\n';
    ssize_t r = write(2, buf, n);
    (void)r;
}

}  // namespace

// ------------------------------------------------------------------------------------- public API

Decision classifyCommand(std::string_view shellCommand) { return classifyImpl(shellCommand, 0); }

void classifyExec(const char* path, char* const argv[], ExecPlan* plan) {
    plan->action = ExecAction::Pass;
    plan->file = nullptr;
    plan->argv = nullptr;
    plan->tool = nullptr;
    if (!path || !*path) return;
    const bool absolute = strchr(path, '/') != nullptr;
    if (absolute && !underAndroidRoot(path)) return;   // a Linux path
    const char* name = baseName(path);
    const ToolRow* row = findTool(name);
    if (absolute && !row) {
        if (inSystemBin(path) && isPosixTool(name)) {
            plan->action = ExecAction::PathSearch;
            plan->file = name;
            plan->argv = argv;
            return;
        }
        if (imageHas(path)) return;
        plan->action = ExecAction::Refuse;
        plan->tool = path;
        return;
    }
    if (!row) return;   // a bare Linux command
    switch (row->kind) {
        case Kind::Noop:
            plan->action = ExecAction::Succeed;
            return;
        case Kind::Nsenter: {
            // nsenter -t 1 -m -- PROGRAM ARGS: Android's way into init's mount namespace. Linux has
            // one namespace for Nano's tools, so the program runs as it is (itself gated).
            size_t i = 1;
            while (argv && argv[i] && strcmp(argv[i], "--") != 0) i++;
            if (argv && argv[i] && argv[i + 1]) {
                char* const* tail = &argv[i + 1];
                classifyExec(tail[0], tail, plan);
                if (plan->action == ExecAction::Pass) {
                    plan->action = ExecAction::PathSearch;
                    plan->file = tail[0];
                    plan->argv = tail;
                }
                return;
            }
            plan->action = ExecAction::Refuse;
            plan->tool = path;
            return;
        }
        case Kind::GammaosNet:
            // gammaos-net download URL FILE: curl, which the image has.
            if (argv && argv[1] && strcmp(argv[1], "download") == 0 && argv[2] && argv[3]) {
                static const char* const kCurl[] = {"curl", "-fsSL", "--retry", "2", "-o"};
                size_t n = 0;
                for (const char* a : kCurl) plan->builtArgv[n++] = const_cast<char*>(a);
                plan->builtArgv[n++] = argv[3];
                plan->builtArgv[n++] = argv[2];
                plan->builtArgv[n] = nullptr;
                plan->action = ExecAction::Rewrite;
                plan->file = "curl";
                plan->argv = plan->builtArgv;
                return;
            }
            plan->action = ExecAction::Refuse;
            plan->tool = path;
            return;
        default:
            // setprop and the other routes need the property store, which a forked child must not
            // touch: refused here (Nano's exec sites never name them).
            plan->action = ExecAction::Refuse;
            plan->tool = path;
            return;
    }
}

void setRoot(const char* root) {
    snprintf(gRoot, sizeof gRoot, "%s", root ? root : "");
    size_t n = strlen(gRoot);
    while (n > 0 && gRoot[n - 1] == '/') gRoot[--n] = '\0';
}

void setRecorder(Recorder recorder) {
    std::lock_guard<std::mutex> l(gMu);
    gRecorder = std::move(recorder);
}

void setRoutes(Routes routes) {
    std::lock_guard<std::mutex> l(gMu);
    gRoutes = std::move(routes);
}

std::vector<std::string> refusedTools() {
    std::lock_guard<std::mutex> l(gMu);
    return gRefused;
}

void resetForTests() {
    std::lock_guard<std::mutex> l(gMu);
    gRefused.clear();
    gRefusedSet.clear();
    gRecorder = nullptr;
    gRoutes = Routes();
    gRoot[0] = '\0';
}

// -------------------------------------------------------------------------------------- wrappers

namespace {

Recorder recorder() {
    std::lock_guard<std::mutex> l(gMu);
    return gRecorder;
}

}  // namespace

}  // namespace shellgate
}  // namespace nanolinux

using namespace nanolinux::shellgate;

extern "C" int __wrap_system(const char* command) {
    if (!command) return __real_system(nullptr);   // "is there a shell?"
    Decision d = classifyCommand(command);
    if (Recorder r = recorder()) {
        Request q;
        q.kind = Request::System;
        q.command = command;
        q.decision = d;
        return waitStatusFor(r(q).exitStatus);
    }
    switch (d.verdict) {
        case Verdict::Pass:
            return __real_system(d.command.c_str());
        case Verdict::Route: {
            auto res = runRoutes(d);
            if (!res.second.empty()) {
                fwrite(res.second.data(), 1, res.second.size(), stdout);
                fflush(stdout);
            }
            return waitStatusFor(res.first);
        }
        case Verdict::Refuse:
        default:
            noteRefused(d.tool, command);
            return waitStatusFor(127);
    }
}

extern "C" FILE* __wrap_popen(const char* command, const char* mode) {
    if (!command || !mode) return __real_popen(command, mode);
    Decision d = classifyCommand(command);
    if (Recorder r = recorder()) {
        Request q;
        q.kind = Request::Popen;
        q.command = command;
        q.decision = d;
        Result res = r(q);
        return syntheticStream(mode, res.output, waitStatusFor(res.exitStatus));
    }
    switch (d.verdict) {
        case Verdict::Pass:
            return __real_popen(d.command.c_str(), mode);
        case Verdict::Route: {
            auto res = runRoutes(d);
            return syntheticStream(mode, res.second, waitStatusFor(res.first));
        }
        case Verdict::Refuse:
        default:
            noteRefused(d.tool, command);
            return syntheticStream(mode, "", waitStatusFor(127));
    }
}

extern "C" int __wrap_pclose(FILE* stream) {
    int status = -1;
    bool mine = false;
    {
        std::lock_guard<std::mutex> l(gMu);
        auto it = gSynthetic.find(stream);
        if (it != gSynthetic.end()) {
            status = it->second;
            gSynthetic.erase(it);
            mine = true;
        }
    }
    if (!mine) return __real_pclose(stream);
    fclose(stream);
    return status;
}

namespace {

// What every exec wrapper does with a plan. Returns only when the exec failed or was refused.
int runExecPlan(const ExecPlan& plan, const char* path, char* const argv[], char* const envp[],
                int (*realExec)(const char*, char* const[], char* const[])) {
    switch (plan.action) {
        case ExecAction::Succeed:
            _exit(0);
        case ExecAction::PathSearch:
        case ExecAction::Rewrite:
            if (envp) return execvpe(plan.file, plan.argv, envp);
            return __real_execvp(plan.file, plan.argv);
        case ExecAction::Refuse:
            writeRefusedExec(plan.tool);
            errno = ENOENT;
            return -1;
        case ExecAction::Pass:
        default:
            return realExec(path, argv, envp);
    }
}

[[noreturn]] void recordExec(const Recorder& r, char* const argv[]) {
    Request q;
    q.kind = Request::Exec;
    q.command = joinArgv(argv);
    _exit(r(q).exitStatus);
}

int realExecv(const char* p, char* const a[], char* const*) { return __real_execv(p, a); }
int realExecvp(const char* p, char* const a[], char* const*) { return __real_execvp(p, a); }
int realExecve(const char* p, char* const a[], char* const e[]) { return __real_execve(p, a, e); }

}  // namespace

extern "C" int __wrap_execv(const char* path, char* const argv[]) {
    if (gRecorder) recordExec(gRecorder, argv);   // host only; never on the device
    ExecPlan plan;
    classifyExec(path, argv, &plan);
    return runExecPlan(plan, path, argv, nullptr, realExecv);
}

extern "C" int __wrap_execvp(const char* file, char* const argv[]) {
    if (gRecorder) recordExec(gRecorder, argv);
    ExecPlan plan;
    classifyExec(file, argv, &plan);
    return runExecPlan(plan, file, argv, nullptr, realExecvp);
}

extern "C" int __wrap_execve(const char* path, char* const argv[], char* const envp[]) {
    if (gRecorder) recordExec(gRecorder, argv);
    ExecPlan plan;
    classifyExec(path, argv, &plan);
    return runExecPlan(plan, path, argv, envp, realExecve);
}
