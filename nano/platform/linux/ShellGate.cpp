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
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

// ------------------------------------------------------------------------------------ allow-list
// What may still run on Linux (OA-4 migration step 1). Each entry is an exact shape: one pattern word
// per word of the simple command, assignments included. A pattern word is a literal, or a literal
// prefix followed by one class: {zone} (an Olson id: letters, digits, '/', '_', '+', '-'; at most 48
// characters, no ".."), {digits} (1 to 19 digits) or {url} (http:// or https://, no blank or quote).
// Nothing else matches: no extra word, option or wrapper.
//
// The list is short and temporary. Every entry names its call site and is removed by migration step 4
// (Http, FileOps and Archive as nanoplat interfaces) or the step that moves its feature behind the
// platform. tools/nano-tree/tests/test_shell_sites.py fails when an entry is added without both.
//
// Not on it (decisions W6S-1, docs/linux/IMPLEMENTATION.md B.5): every curl, because the image has no
// curl program (BR2_PACKAGE_LIBCURL_CURL is not set), so the streaming, IPTV, radio, clock and
// time-zone fetches and the ES-DE theme downloader's gammaos-net download could not work through the
// shell anyway (step 4's Http uses the libcurl Nano already links); the ES-DE downloader's mkdir, rm,
// unzip and mv, which only serve that download; chown -R system:system (an Android user);
// pidof <Android package>; restorecon and chcon (no SELinux: every caller ignores or only logs the
// result, so refusing them breaks nothing and stops a false success); nsenter.
enum Via : unsigned { kViaShell = 1, kViaExec = 2 };

struct AllowRule {
    const char* site;          // the call site (file:line function)
    unsigned via;              // kViaShell for system()/popen(), kViaExec for exec*()
    const char* words[10];     // the pattern, nullptr-terminated
};

constexpr AllowRule kAllow[] = {
    // NanoMenuSetupWizard.cpp:318 (NanoMenu::tzStandardOffset): a zone's UTC offset in January and
    // July, for placing the zone the wizard found; busybox date and the image's zoneinfo.
    // TODO(OA-4 step 4): remove (the offset comes from the platform, not a shell).
    {"NanoMenuSetupWizard.cpp:318 tzStandardOffset", kViaShell,
     {"TZ={zone}", "date", "-d", "@{digits}", "+%z", nullptr}},
};

bool isZone(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > 48 || strstr(s, "..")) return false;
    for (const char* p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '/' && *p != '_' && *p != '+' && *p != '-') return false;
    return true;
}

bool isDigits(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > 19) return false;
    for (const char* p = s; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

bool isUrl(const char* s) {
    if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0) return false;
    size_t n = strlen(s);
    if (n > 2000) return false;
    for (const char* p = s; *p; p++)
        if (isspace((unsigned char)*p) || *p == '\'' || *p == '"' || *p == '`' || *p == '\\') return false;
    return true;
}

// Async-signal-safe.
bool matchWord(const char* pat, const char* w) {
    const char* brace = strchr(pat, '{');
    if (!brace) return strcmp(pat, w) == 0;
    size_t pre = (size_t)(brace - pat);
    if (strncmp(pat, w, pre) != 0) return false;
    const char* rest = w + pre;
    const char* cls = brace + 1;
    if (strcmp(cls, "zone}") == 0) return isZone(rest);
    if (strcmp(cls, "digits}") == 0) return isDigits(rest);
    if (strcmp(cls, "url}") == 0) return isUrl(rest);
    return false;
}

// The entry WORDS (n of them, read through get(i)) match, or nullptr. Async-signal-safe.
template <typename Get>
const AllowRule* findAllowed(unsigned via, size_t n, Get get) {
    for (const AllowRule& r : kAllow) {
        if (!(r.via & via)) continue;
        size_t i = 0;
        bool ok = true;
        for (; r.words[i]; i++) {
            if (i >= n || !matchWord(r.words[i], get(i))) {
                ok = false;
                break;
            }
        }
        if (ok && i == n) return &r;
    }
    return nullptr;
}

// --------------------------------------------------------------------------------------- shapes

// A word that reads as a subcommand: "force-stop", "package", "get". Async-signal-safe.
bool isSubcommand(const char* w) {
    if (!w || !(*w >= 'a' && *w <= 'z')) return false;
    for (const char* p = w; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

// "<w0>[ <w1>]" into out (cap bytes, truncated). Async-signal-safe.
void shapeInto(char* out, size_t cap, const char* w0, const char* w1) {
    size_t n = 0;
    for (const char* p = w0 ? w0 : "?"; *p && n + 1 < cap; p++) out[n++] = *p;
    if (isSubcommand(w1)) {
        if (n + 1 < cap) out[n++] = ' ';
        for (const char* p = w1; *p && n + 1 < cap; p++) out[n++] = *p;
    }
    out[n] = '\0';
}

// ---------------------------------------------------------------------------- refusal registry
// Every refusal is counted and its shape logged once. Exec refusals happen in a forked child, so the
// registry lives in a shared anonymous mapping made before main (no fork has happened yet): a child's
// count and first-seen shape reach the parent. Updates take a spin lock in that mapping; a parent
// thread holding it at fork time still releases it (the memory is shared), and a child gives up
// after a bounded wait rather than hang (it then counts without logging).

constexpr size_t kShapeMax = sizeof(ExecPlan::shape);
constexpr size_t kSlots = 128;

struct Registry {
    int lock;
    unsigned total;
    unsigned used;
    char shapes[kSlots][kShapeMax];
};

Registry gLocalRegistry;   // if the shared mapping cannot be made

Registry* makeRegistry() {
    void* p = mmap(nullptr, sizeof(Registry), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return &gLocalRegistry;
    memset(p, 0, sizeof(Registry));
    return static_cast<Registry*>(p);
}

Registry* gRegistry = makeRegistry();

Registry* registry() {
    Registry* r = __atomic_load_n(&gRegistry, __ATOMIC_ACQUIRE);
    return r ? r : &gLocalRegistry;
}

bool lockRegistry(Registry* r) {
    for (long i = 0; i < 2000000; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&r->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
        if ((i & 63) == 63) sched_yield();
    }
    return false;
}

void unlockRegistry(Registry* r) { __atomic_store_n(&r->lock, 0, __ATOMIC_RELEASE); }

// Counts a refusal; true when SHAPE is new (the caller logs it). Async-signal-safe.
bool countRefusal(const char* shape) {
    Registry* r = registry();
    __atomic_add_fetch(&r->total, 1, __ATOMIC_SEQ_CST);
    if (!lockRegistry(r)) return false;
    bool first = true;
    unsigned used = r->used;
    for (unsigned i = 0; i < used && i < kSlots; i++) {
        if (strncmp(r->shapes[i], shape, kShapeMax - 1) == 0) {
            first = false;
            break;
        }
    }
    if (first && used < kSlots) {
        size_t n = 0;
        for (; shape[n] && n + 1 < kShapeMax; n++) r->shapes[used][n] = shape[n];
        r->shapes[used][n] = '\0';
        r->used = used + 1;
    }
    unlockRegistry(r);
    return first;
}

// Logs a refused exec from a forked child: one write(2), no allocation.
void writeRefusedExec(const char* shape) {
    char buf[256];
    const char* pre = "thimsy-shellgate: refused exec (OA-4: not on the allow-list): ";
    size_t n = 0;
    for (const char* p = pre; *p && n < sizeof buf - 2; p++) buf[n++] = *p;
    for (const char* p = shape; *p && n < sizeof buf - 2; p++) buf[n++] = *p;
    buf[n++] = '\n';
    ssize_t w = write(2, buf, n);
    (void)w;
}

// ------------------------------------------------------------------------------ state and routes

std::mutex gMu;
std::vector<std::string> gRouted;             // once per shape, in order
std::set<std::string> gRoutedSet;
Recorder gRecorder;
Routes gRoutes;
std::map<FILE*, int> gSynthetic;              // popen() streams the gate made: their wait status

void noteRefused(const std::string& shape, const std::string& what) {
    if (countRefusal(shape.c_str()))
        ALOGW("refused (OA-4: not on the allow-list; logged once per shape): %s: %s", shape.c_str(), what.c_str());
}

void noteRouted(const std::string& shape) {
    bool first;
    {
        std::lock_guard<std::mutex> l(gMu);
        first = gRoutedSet.insert(shape).second;
        if (first) gRouted.push_back(shape);
    }
    if (first) ALOGI("routed (OA-4 step 5): %s", shape.c_str());
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
// Only to name what is refused and to recognise the routes and the allow-listed shapes: anything the
// parser does not understand is refused.

struct Token {
    enum Type { Word, Sep, Redir } type;
    std::string text;      // Word: after quote removal; Sep: the operator; Redir: the operator
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
            out.push_back({Token::Sep, "$("});
            i += 2;
            continue;
        }
        if (c == '>' || c == '<' || (c == '&' && i + 1 < n && s[i + 1] == '>')) {
            size_t b = i;
            if (c == '&') i++;
            i++;
            while (i < n && (s[i] == '>' || s[i] == '<' || s[i] == '&' || s[i] == '|')) i++;
            out.push_back({Token::Redir, std::string(s.substr(b, i - b))});
            continue;
        }
        if (isOpChar(c)) {
            size_t b = i;
            if ((c == '&' || c == '|' || c == ';') && i + 1 < n && s[i + 1] == c) i++;
            i++;
            out.push_back({Token::Sep, std::string(s.substr(b, i - b))});
            continue;
        }
        // A word: up to an unquoted blank or operator.
        Token t{Token::Word, ""};
        size_t begin = i;
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
        // "2>": a descriptor number glued to a redirection belongs to it.
        if (digitsOnly && i < n && (s[i] == '>' || s[i] == '<')) {
            while (i < n && (s[i] == '>' || s[i] == '<' || s[i] == '&')) i++;
            out.push_back({Token::Redir, std::string(s.substr(begin, i - begin))});
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

struct Simple {
    std::vector<std::string> words;                        // redirections and their targets dropped
    std::vector<std::pair<std::string, std::string>> redirs;  // (operator, target)
};

std::vector<Simple> splitSimple(const std::vector<Token>& toks) {
    std::vector<Simple> out;
    Simple cur;
    bool target = false;
    for (const Token& t : toks) {
        if (t.type == Token::Sep) {
            if (!cur.words.empty() || !cur.redirs.empty()) out.push_back(cur);
            cur = Simple();
            target = false;
            continue;
        }
        if (t.type == Token::Redir) {
            // The lexer leaves every redirection's target ("/dev/null", the "1" of "2>&1") as the
            // next word.
            cur.redirs.push_back({t.text, ""});
            target = true;
            continue;
        }
        if (target) {
            cur.redirs.back().second = t.text;
            target = false;
            continue;
        }
        cur.words.push_back(t.text);
    }
    if (!cur.words.empty() || !cur.redirs.empty()) out.push_back(cur);
    return out;
}

// Redirections an allowed command or a route may carry: output thrown away, or stderr to stdout.
bool redirsHarmless(const Simple& sc) {
    for (const auto& r : sc.redirs) {
        const std::string& op = r.first;
        if (r.second == "/dev/null" && (op == ">" || op == "1>" || op == "2>" || op == "&>" || op == ">>" ||
                                        op == "2>>"))
            continue;
        if ((op == "2>&" || op == ">&") && r.second == "1") continue;
        return false;
    }
    return true;
}

bool hasOption(const std::string& w) { return w.size() > 1 && w[0] == '-'; }

// The index of the word that names the command, past assignments and the wrappers that run their
// arguments (only to name a refusal: a wrapper never passes). An option a wrapper does not have
// leaves the wrapper as the command.
size_t commandIndex(const std::vector<std::string>& w) {
    size_t k = 0;
    size_t lastWrapper = std::string::npos;
    for (;;) {
        while (k < w.size() && isAssignment(w[k])) k++;
        if (k >= w.size()) return lastWrapper != std::string::npos ? lastWrapper : k;   // a wrapper of nothing
        lastWrapper = k;
        const std::string& x = w[k];
        const size_t wrapper = k;
        if (x == "exec" || x == "nohup" || x == "command" || x == "busybox" || x == "toybox" || x == "toolbox") {
            k++;
            continue;
        }
        if (x == "env") {
            k++;
            while (k < w.size()) {
                const std::string& o = w[k];
                if (o == "-u" || o == "--unset") k += 2;
                else if (o == "-" || o == "-i" || o == "--ignore-environment" || o.compare(0, 8, "--unset=") == 0 ||
                         (o.size() > 2 && o.compare(0, 2, "-u") == 0))
                    k++;
                else if (isAssignment(o)) k++;
                else if (hasOption(o)) return wrapper;
                else break;
            }
            continue;
        }
        if (x == "nice") {
            k++;
            if (k < w.size()) {
                const std::string& o = w[k];
                if (o == "-n" || o == "--adjustment") k += 2;
                else if (o.compare(0, 2, "-n") == 0 || o.compare(0, 13, "--adjustment=") == 0) k++;
                else if (o.size() > 1 && o[0] == '-' && isdigit((unsigned char)o[1])) k++;
                else if (hasOption(o)) return wrapper;
            }
            continue;
        }
        if (x == "timeout") {
            // timeout [-s SIG | -sSIG | --signal=SIG | --signal SIG] [-k DUR | -kDUR | --kill-after=DUR |
            //         --kill-after DUR] [--preserve-status] [--foreground] [-v | --verbose] [--] DUR CMD...
            k++;
            for (;;) {
                if (k >= w.size()) return wrapper;
                const std::string& o = w[k];
                if (o == "--") { k++; break; }
                if (o == "-s" || o == "-k" || o == "--signal" || o == "--kill-after") { k += 2; continue; }
                if (o.compare(0, 9, "--signal=") == 0 || o.compare(0, 13, "--kill-after=") == 0 ||
                    o == "--preserve-status" || o == "--foreground" || o == "-v" || o == "--verbose" ||
                    o == "-f" || o == "-p") {
                    k++;
                    continue;
                }
                if (o.size() > 2 && o[0] == '-' && (o[1] == 's' || o[1] == 'k')) { k++; continue; }
                if (hasOption(o)) return wrapper;   // an option timeout does not have
                break;
            }
            if (k >= w.size()) return wrapper;
            k++;   // the duration
            continue;
        }
        return k;
    }
}

std::string baseName(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// setprop, getprop, reboot or svc, by name or as /system/bin/<name>.
std::string routeTool(const std::string& w) {
    std::string name = w;
    if (w.compare(0, 12, "/system/bin/") == 0 || w.compare(0, 13, "/system/xbin/") == 0) name = baseName(w);
    if (name == "setprop" || name == "getprop" || name == "reboot" || name == "svc") return name;
    return "";
}

Decision classifyImpl(std::string_view s, int depth);

const char* const kAssignmentShape = "(assignment only)";

Command decideSimple(const Simple& sc, int depth) {
    Command c;
    c.words = sc.words;
    const std::vector<std::string>& w = sc.words;
    const size_t k = commandIndex(w);
    if (w.empty()) {   // only redirections: refused
        c.shape = "(no command)";
        return c;
    }
    // Only assignments ("D=$(dumpsys ...)" splits into "D=" and the substitution): refused, but
    // classifyImpl names the string after a command that does something, when it has one.
    c.shape = k < w.size() ? shapeOf(std::vector<std::string>(w.begin() + k, w.end())) : kAssignmentShape;
    const bool harmless = redirsHarmless(sc);

    // An allow-listed shape, word for word.
    if (harmless) {
        if (const AllowRule* r = findAllowed(kViaShell, w.size(), [&](size_t i) { return w[i].c_str(); })) {
            c.verdict = Verdict::Pass;
            c.allow = r->site;
            return c;
        }
    }

    // A route: the tool first, with no assignment or wrapper before it.
    const std::string tool = routeTool(w[0]);
    if (!tool.empty() && harmless) {
        const size_t nargs = w.size() - 1;
        if ((tool == "setprop" || tool == "getprop") && nargs >= 1) {
            c.verdict = Verdict::Route;
            c.route = tool;
            c.shape = tool + " " + w[1];
        } else if (tool == "reboot") {
            c.verdict = Verdict::Route;
            c.route = "powerctl";
            c.shape = "reboot";
        } else if (tool == "svc" && nargs >= 2 && w[1] == "power" && (w[2] == "reboot" || w[2] == "shutdown")) {
            c.verdict = Verdict::Route;
            c.route = "powerctl";
            c.shape = "svc power " + w[2];
        }
        if (c.verdict == Verdict::Route) return c;
    }

    // Refused. sh -c STRING (or bash -c) is named after the first refused command inside it.
    if (k < w.size() && depth < 4) {
        const std::string name = baseName(w[k]);
        if (name == "sh" || name == "bash") {
            for (size_t j = k + 1; j + 1 < w.size(); j++) {
                if (w[j] == "-c") {
                    Decision inner = classifyImpl(w[j + 1], depth + 1);
                    if (inner.verdict == Verdict::Refuse) c.shape = w[k] + " -c: " + inner.shape;
                    break;
                }
            }
        }
    }
    return c;
}

Decision classifyImpl(std::string_view s, int depth) {
    Decision d;
    std::vector<Token> toks = lexShell(s);
    bool complex = false;   // a pipe, background job, subshell or command substitution
    for (const Token& t : toks) {
        if (t.type == Token::Sep && t.text != ";" && t.text != "&&" && t.text != "||" && t.text != "\n")
            complex = true;
    }
    std::vector<Simple> simple = splitSimple(toks);
    if (simple.empty()) {
        d.shape = "(empty)";
        return d;
    }
    bool anyRefuse = false, anyRoute = false, anyPass = false, namedRefuse = false;
    for (const Simple& sc : simple) {
        Command c = decideSimple(sc, depth);
        if (c.verdict == Verdict::Refuse && !namedRefuse) {
            // The first refused command, or a later one that runs something when the first only assigns.
            const bool named = c.shape != kAssignmentShape;
            if (!anyRefuse || named) d.shape = c.shape;
            anyRefuse = true;
            namedRefuse = named;
        }
        if (c.verdict == Verdict::Route && !anyRoute) {
            anyRoute = true;
            if (!anyRefuse) d.shape = c.shape;
        }
        if (c.verdict == Verdict::Pass) anyPass = true;
        d.commands.push_back(std::move(c));
    }
    if (anyRefuse) {
        d.verdict = Verdict::Refuse;
    } else if (complex || (anyRoute && anyPass)) {
        // Allowed shapes and routes run only as plain lists: never piped, in the background, in a
        // subshell or mixed with each other.
        d.verdict = Verdict::Refuse;
        d.shape = d.commands.front().shape + (complex ? " (in a pipe or subshell)" : " (mixed with a route)");
    } else if (anyRoute) {
        d.verdict = Verdict::Route;
    } else {
        d.verdict = Verdict::Pass;
        d.shape.clear();
        d.command = std::string(s);
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
        noteRouted(c.shape);
        int rc = 0;
        if (c.route == "setprop") {
            rc = routeSet(a.size() > 1 ? a[1] : "", a.size() > 2 ? a[2] : "") ? 0 : 1;
        } else if (c.route == "getprop") {
            out += routeGet(a.size() > 1 ? a[1] : "", a.size() > 2 ? a[2] : "");
            out += "\n";
        } else if (c.route == "powerctl") {
            std::string v;
            if (baseName(a[0]) == "svc") {   // svc power reboot|shutdown [reason]
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

}  // namespace

// ------------------------------------------------------------------------------------- public API

std::string shapeOf(const std::vector<std::string>& words) {
    if (words.empty()) return "(no command)";
    char buf[kShapeMax];
    shapeInto(buf, sizeof buf, words[0].c_str(), words.size() > 1 ? words[1].c_str() : nullptr);
    return buf;
}

Decision classifyCommand(std::string_view shellCommand) { return classifyImpl(shellCommand, 0); }

void classifyExec(const char* path, char* const argv[], ExecPlan* plan) {
    plan->action = ExecAction::Refuse;
    plan->allow = nullptr;
    plan->shape[0] = '\0';
    if (!path || !*path) {
        shapeInto(plan->shape, sizeof plan->shape, "(no program)", nullptr);
        return;
    }
    // The exec's words: the path as the command word, then argv[1...].
    size_t n = 1;
    while (argv && argv[0] && argv[n]) n++;
    const AllowRule* r = findAllowed(kViaExec, n, [&](size_t i) -> const char* { return i == 0 ? path : argv[i]; });
    if (r) {
        plan->action = ExecAction::Pass;
        plan->allow = r->site;
        return;
    }
    shapeInto(plan->shape, sizeof plan->shape, path, argv && argv[0] ? argv[1] : nullptr);
}

int platformExecve(const char* path, char* const argv[], char* const envp[]) { return __real_execve(path, argv, envp); }

std::vector<std::string> allowListSites() {
    std::vector<std::string> out;
    for (const AllowRule& r : kAllow) out.push_back(r.site);
    return out;
}

void setRecorder(Recorder recorder) {
    std::lock_guard<std::mutex> l(gMu);
    gRecorder = std::move(recorder);
}

void setRoutes(Routes routes) {
    std::lock_guard<std::mutex> l(gMu);
    gRoutes = std::move(routes);
}

unsigned refusalCount() { return __atomic_load_n(&registry()->total, __ATOMIC_SEQ_CST); }

std::vector<std::string> refusedShapes() {
    std::vector<std::string> out;
    Registry* r = registry();
    if (!lockRegistry(r)) return out;
    for (unsigned i = 0; i < r->used && i < kSlots; i++) out.push_back(r->shapes[i]);
    unlockRegistry(r);
    return out;
}

std::vector<std::string> routedShapes() {
    std::lock_guard<std::mutex> l(gMu);
    return gRouted;
}

void resetForTests() {
    {
        std::lock_guard<std::mutex> l(gMu);
        gRouted.clear();
        gRoutedSet.clear();
        gRecorder = nullptr;
        gRoutes = Routes();
    }
    Registry* r = registry();
    if (lockRegistry(r)) {
        r->used = 0;
        __atomic_store_n(&r->total, 0, __ATOMIC_SEQ_CST);
        memset(r->shapes, 0, sizeof r->shapes);
        unlockRegistry(r);
    }
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
            noteRefused(d.shape, command);
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
            noteRefused(d.shape, command);
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
    if (plan.action == ExecAction::Pass) return realExec(path, argv, envp);
    if (countRefusal(plan.shape)) writeRefusedExec(plan.shape);
    errno = ENOENT;
    return -1;
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
    if (gRecorder) recordExec(gRecorder, argv);   // tests only; never on the device
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
