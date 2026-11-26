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

#include "Run.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

extern char** environ;

namespace nanolinux {
namespace actions {

namespace {

constexpr size_t kMaxOut = 64 * 1024;

long long monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int statusOf(int ws) {
    if (WIFEXITED(ws)) return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws)) return 128 + WTERMSIG(ws);
    return -1;
}

// The highest descriptor the child closes before execve, taken before fork (as Launcher.cpp's).
int closeLimit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 65536) return 65536;
    return (int)rl.rlim_cur;
}

// argv[0] as execvp would find it, looked up before fork (the child may not allocate): itself when
// it has a slash, else the first executable regular file of that name on PATH. "" when none.
std::string resolve(const std::string& name) {
    if (name.find('/') != std::string::npos) return name;
    const char* env = getenv("PATH");
    const std::string path = env != nullptr ? env : "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
    size_t start = 0;
    for (;;) {
        const size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        const std::string candidate = dir + "/" + name;
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (colon == std::string::npos) return std::string();
        start = colon + 1;
    }
}

// The forked child, up to execve: async-signal-safe calls only (Nano has many threads). A session
// of its own, so a daemon the program leaves running (D30net's udhcpc, D35bluetooth's bus and
// bluetoothd) is not in Nano's process group, which the session signals when it restarts or stops
// Nano. Default signals and an empty mask, as a new program expects (an ignored signal would carry
// over exec, and the worker thread's mask is whatever Nano gave it). And no
// descriptor of Nano's but stdin, stdout and stderr: Nano's evdev fds have no O_CLOEXEC, and a
// grab belongs to the open file, so a daemon holding one would keep Nano's last EVIOCGRAB after
// Nano exits and the game would get no input.
[[noreturn]] void childExec(const char* path, char* const* argv, int outFd, int maxFd) {
    setsid();
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    for (int s = 1; s < NSIG; s++) sigaction(s, &dfl, nullptr);  // SIGKILL, SIGSTOP and libc's own refuse
    const int in = open("/dev/null", O_RDONLY);
    if (in < 0 || dup2(in, 0) < 0 || dup2(outFd, 1) < 0 || dup2(outFd, 2) < 0) _exit(127);
    for (int fd = 3; fd < maxFd; fd++) close(fd);
    // The system call itself: the link wraps execve for the shell gate (linker-wraps.txt), which is
    // for Nano's Android commands and is not async-signal-safe; these are the platform's own.
    syscall(SYS_execve, path, argv, environ);
    _exit(127);
}

}  // namespace

RunResult runProgram(const std::vector<std::string>& argv, int timeoutMs) {
    RunResult r;
    if (argv.empty() || argv[0].empty()) return r;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return r;
    // Non-blocking: a program's own children may keep the pipe open after it exits.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    // Everything the child needs, made before fork.
    const std::string path = resolve(argv[0]);
    if (path.empty()) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.status = 127;
        return r;
    }
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    const int maxFd = closeLimit();

    const pid_t pid = fork();
    if (pid == 0) childExec(path.c_str(), args.data(), fds[1], maxFd);
    ::close(fds[1]);
    if (pid < 0) {
        ::close(fds[0]);
        return r;
    }

    const long long deadline = monotonicMs() + (timeoutMs > 0 ? timeoutMs : 0);
    bool open = true;
    int ws = 0;
    bool reaped = false;
    while (!reaped) {
        if (open) {
            long long left = deadline - monotonicMs();
            struct pollfd p = {fds[0], POLLIN, 0};
            int n = poll(&p, 1, left > 0 ? (int)(left < 100 ? left : 100) : 0);
            if (n > 0) {
                char buf[4096];
                ssize_t got = ::read(fds[0], buf, sizeof(buf));
                if (got > 0) {
                    if (r.out.size() < kMaxOut) r.out.append(buf, (size_t)std::min(kMaxOut - r.out.size(), (size_t)got));
                } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
                    open = false;
                }
            }
        } else {
            struct timespec ts = {0, 20 * 1000000};
            nanosleep(&ts, nullptr);
        }
        pid_t w = waitpid(pid, &ws, WNOHANG);
        if (w == pid) {
            reaped = true;
        } else if (w < 0 && errno != EINTR) {
            break;
        } else if (monotonicMs() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {}
            reaped = true;
            r.timedOut = true;
        }
    }
    // What the program wrote just before it exited (what is in the pipe now; never waits).
    while (open) {
        char buf[4096];
        ssize_t got = ::read(fds[0], buf, sizeof(buf));
        if (got > 0) {
            if (r.out.size() < kMaxOut) r.out.append(buf, (size_t)std::min(kMaxOut - r.out.size(), (size_t)got));
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        open = false;
    }
    ::close(fds[0]);
    r.status = reaped ? statusOf(ws) : -1;
    return r;
}

}  // namespace actions
}  // namespace nanolinux
