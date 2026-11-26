// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>

#include "thimsy/atomic_file.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_ioprio_set
#if defined(__aarch64__)
#define SYS_ioprio_set 30
#elif defined(__x86_64__)
#define SYS_ioprio_set 251
#endif
#endif

extern char** environ;

namespace thimsy {
namespace session {
namespace {

void childWrite(int fd, const std::string& s) {
    ssize_t n = write(fd, s.data(), s.size());
    (void)n;
}

}  // namespace

pid_t spawn(const SpawnSpec& spec, std::string* err) {
    if (spec.argv.empty()) {
        if (err) *err = "nothing to run";
        return -1;
    }
    // Everything the child needs is built before fork.
    std::vector<std::string> envStore;
    for (char** e = environ; e && *e; ++e) {
        std::string kv = *e;
        bool overridden = false;
        for (const std::string& x : spec.env) {
            size_t eq = x.find('=');
            if (eq != std::string::npos && kv.compare(0, eq + 1, x, 0, eq + 1) == 0) overridden = true;
        }
        if (!overridden) envStore.push_back(kv);
    }
    for (const std::string& x : spec.env) envStore.push_back(x);
    std::vector<char*> envp;
    for (std::string& s : envStore) envp.push_back(&s[0]);
    envp.push_back(nullptr);
    std::vector<std::string> argStore = spec.argv;
    std::vector<char*> argv;
    for (std::string& s : argStore) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    std::string logFile = spec.logFile.empty() ? std::string("/dev/null") : spec.logFile;

    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = std::string("fork: ") + strerror(errno);
        return -1;
    }
    if (pid > 0) return pid;

    // The child. The session blocks the signals it reads through signalfd; undo that.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE}) signal(s, SIG_DFL);
    if (spec.newSession) setsid();
    // O_CLOEXEC, so the originals never reach the program (dup2's copies on 0-2 are inheritable).
    int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (out < 0) out = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (in >= 0) dup2(in, 0);
    if (out >= 0) {
        dup2(out, 1);
        dup2(out, 2);
    }
    if (in > 2) close(in);
    if (out > 2) close(out);
    if (spec.nice) {
        errno = 0;
        int r = nice(spec.nice);
        (void)r;
    }
    if (spec.oomScoreAdj) {
        int fd = open("/proc/self/oom_score_adj", O_WRONLY);
        if (fd >= 0) {
            childWrite(fd, std::to_string(spec.oomScoreAdj));
            close(fd);
        }
    }
#ifdef SYS_ioprio_set
    if (spec.ioprioIdle) {
        // IOPRIO_WHO_PROCESS, this process, best-effort class (2) level 7.
        syscall(SYS_ioprio_set, 1, 0, (2 << 13) | 7);
    }
#endif
    if (spec.preExec) {
        std::string why;
        if (!spec.preExec(&why)) {
            childWrite(2, "thimsy-session: not starting " + spec.argv[0] + ": " + why + "\n");
            _exit(126);
        }
    }
    environ = envp.data();
    execvp(argv[0], argv.data());
    childWrite(2, "thimsy-session: exec " + spec.argv[0] + ": " + strerror(errno) + "\n");
    _exit(127);
}

int exitCodeOf(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

int runWait(const SpawnSpec& spec, int timeoutMs, std::string* err) {
    pid_t pid = spawn(spec, err);
    if (pid < 0) return -1;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        if (r < 0 && errno != EINTR) return -1;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (timeoutMs >= 0 && ms > timeoutMs) {
            kill(spec.newSession ? -pid : pid, SIGKILL);
            waitpid(pid, &st, 0);
            if (err) *err = "timed out";
            return 128 + SIGKILL;
        }
        poll(nullptr, 0, 5);
    }
}

bool alive(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

pid_t readPidFile(const std::string& path) {
    std::string s;
    if (!readFile(path, &s)) return 0;
    long v = 0;
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && v < 1000000000L) v = v * 10 + (s[i++] - '0');
    if (i == 0) return 0;
    return static_cast<pid_t>(v);
}

std::string cmdline(pid_t pid, const std::string& procRoot) {
    std::string s;
    if (!readFile(procRoot + "/proc/" + std::to_string(pid) + "/cmdline", &s)) return "";
    for (char& c : s) {
        if (c == '\0') c = ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int pidfdOpen(pid_t pid) {
    long fd = syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return -1;
    fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC);
    return static_cast<int>(fd);
}

}  // namespace session
}  // namespace thimsy
