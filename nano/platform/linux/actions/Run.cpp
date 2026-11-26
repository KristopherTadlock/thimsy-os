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
#include <spawn.h>
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

}  // namespace

RunResult runProgram(const std::vector<std::string>& argv, int timeoutMs) {
    RunResult r;
    if (argv.empty() || argv[0].empty()) return r;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return r;
    // Non-blocking: a program's own children may keep the pipe open after it exits.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    pid_t pid = -1;
    int err = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (err != 0) {
        ::close(fds[0]);
        r.status = (err == ENOENT || err == EACCES) ? 127 : -1;
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
