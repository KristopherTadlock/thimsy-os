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

// See ShellGateHost.h.

#include "ShellGateHost.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "HostControl.h"

extern "C" {
int __real_pclose(FILE* stream);
int __wrap_system(const char* command);
FILE* __wrap_popen(const char* command, const char* mode);
int __wrap_pclose(FILE* stream);
int __wrap_execv(const char* path, char* const argv[]);
int __wrap_execvp(const char* file, char* const argv[]);
int __wrap_execve(const char* path, char* const argv[], char* const envp[]);
}

namespace nanohost {
namespace shell {

namespace {

struct RuleSet {
    std::vector<Rule> rules;
    int defaultStatus = 127;
};

// Rule sets are immutable once published and never freed, so a forked child can read the current
// one without a lock.
std::mutex gSetsMu;
std::vector<std::unique_ptr<RuleSet>> gSets;
std::atomic<const RuleSet*> gCurrent{nullptr};

std::mutex gStreamsMu;
std::map<FILE*, int> gStreams;  // gate streams -> exit status

int matchIn(const RuleSet* set, const char* command) {
    if (!set) return -1;
    for (size_t i = 0; i < set->rules.size(); ++i) {
        const Rule& r = set->rules[i];
        const bool hit = r.exact ? strcmp(command, r.match.c_str()) == 0
                                 : strncmp(command, r.match.c_str(), r.match.size()) == 0;
        if (hit) return (int)i;
    }
    return -1;
}

int statusOf(const RuleSet* set, int rule) {
    if (rule >= 0) return set->rules[(size_t)rule].status & 0xff;
    return (set ? set->defaultStatus : 127) & 0xff;
}

void record(const char* fn, const std::string& command, int rule, int status) {
    event("shell", "\"fn\":" + jsonQuote(fn) + ",\"cmd\":" + jsonQuote(command) + ",\"rule\":" +
                       std::to_string(rule) + ",\"status\":" + std::to_string(status));
}

// --- the exec wrappers run in a forked child: no locks, no allocation ---------------------------
struct Buf {
    char data[8192];
    size_t len = 0;
    bool put(const char* s, size_t n) {
        if (len + n >= sizeof(data)) return false;
        memcpy(data + len, s, n);
        len += n;
        data[len] = '\0';
        return true;
    }
    bool put(const char* s) { return put(s, strlen(s)); }
    bool putQuoted(const char* s) {
        if (!put("\"", 1)) return false;
        for (; *s; ++s) {
            const unsigned char c = (unsigned char)*s;
            char esc[8];
            if (c == '"' || c == '\\') {
                esc[0] = '\\';
                esc[1] = (char)c;
                if (!put(esc, 2)) return false;
            } else if (c < 0x20 || c == 0x7f) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                if (!put(esc, 6)) return false;
            } else if (!put((const char*)&c, 1)) {
                return false;
            }
        }
        return put("\"", 1);
    }
};

[[noreturn]] void refuseExec(const char* fn, const char* path, char* const argv[]) {
    Buf cmd;
    cmd.put(path ? path : "");
    for (int i = 1; argv && argv[i]; ++i) {
        cmd.put(" ", 1);
        cmd.put(argv[i]);
    }
    const RuleSet* set = gCurrent.load(std::memory_order_acquire);
    const int rule = matchIn(set, cmd.data);
    const int status = statusOf(set, rule);
    const int fd = eventsFd();
    if (fd >= 0) {
        Buf line;
        char num[64];
        line.put("{\"ev\":\"shell\",\"t_ms\":");
        snprintf(num, sizeof(num), "%lld", (long long)monotonicMs());
        line.put(num);
        line.put(",\"fn\":");
        line.putQuoted(fn);
        line.put(",\"cmd\":");
        line.putQuoted(cmd.data);
        snprintf(num, sizeof(num), ",\"rule\":%d,\"status\":%d,\"pid\":%d}\n", rule, status, (int)getpid());
        line.put(num);
        if (line.len > 0 && line.data[line.len - 1] == '\n') {
            ssize_t ignored = ::write(fd, line.data, line.len);
            (void)ignored;
        }
    }
    _exit(status);
}

}  // namespace

void setRules(std::vector<Rule> rules, int defaultStatus) {
    std::unique_ptr<RuleSet> set(new RuleSet);
    set->rules = std::move(rules);
    set->defaultStatus = defaultStatus;
    std::lock_guard<std::mutex> lk(gSetsMu);
    gCurrent.store(set.get(), std::memory_order_release);
    gSets.push_back(std::move(set));
}

int match(const std::string& command) {
    return matchIn(gCurrent.load(std::memory_order_acquire), command.c_str());
}

}  // namespace shell
}  // namespace nanohost

using namespace nanohost::shell;

extern "C" int __wrap_system(const char* command) {
    if (command == nullptr) return 1;  // "a shell is available": Nano may then call it
    const RuleSet* set = gCurrent.load(std::memory_order_acquire);
    const int rule = matchIn(set, command);
    const int status = statusOf(set, rule);
    record("system", command, rule, status);
    return status << 8;
}

extern "C" FILE* __wrap_popen(const char* command, const char* mode) {
    if (command == nullptr || mode == nullptr) {
        errno = EINVAL;
        return nullptr;
    }
    const RuleSet* set = gCurrent.load(std::memory_order_acquire);
    const int rule = matchIn(set, command);
    const int status = statusOf(set, rule);
    record("popen", command, rule, status);
    FILE* f = nullptr;
    const bool cloexec = strchr(mode, 'e') != nullptr;
    if (strchr(mode, 'w') != nullptr) {
        f = fopen("/dev/null", cloexec ? "we" : "w");
    } else {
        // The rule's output through a pipe: written whole before the stream is returned, so it has to
        // fit the pipe's buffer (64 KiB); anything longer is cut there.
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) return nullptr;
        if (rule >= 0) {
            const std::string& out = set->rules[(size_t)rule].out;
            fcntl(fds[1], F_SETFL, O_NONBLOCK);
            size_t off = 0;
            while (off < out.size()) {
                const ssize_t w = ::write(fds[1], out.data() + off, out.size() - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
        }
        ::close(fds[1]);
        if (!cloexec) fcntl(fds[0], F_SETFD, 0);
        f = fdopen(fds[0], "r");
        if (f == nullptr) ::close(fds[0]);
    }
    if (f != nullptr) {
        std::lock_guard<std::mutex> lk(gStreamsMu);
        gStreams[f] = status;
    }
    return f;
}

extern "C" int __wrap_pclose(FILE* stream) {
    int status = -1;
    {
        std::lock_guard<std::mutex> lk(gStreamsMu);
        auto it = gStreams.find(stream);
        if (it != gStreams.end()) {
            status = it->second;
            gStreams.erase(it);
        }
    }
    if (status < 0) return __real_pclose(stream);
    fclose(stream);
    return status << 8;
}

extern "C" int __wrap_execv(const char* path, char* const argv[]) { refuseExec("execv", path, argv); }

extern "C" int __wrap_execvp(const char* file, char* const argv[]) { refuseExec("execvp", file, argv); }

extern "C" int __wrap_execve(const char* path, char* const argv[], char* const envp[]) {
    (void)envp;
    refuseExec("execve", path, argv);
}
