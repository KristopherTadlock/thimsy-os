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

// The Linux launcher: see Launcher.h.

#define LOG_TAG "ThimsyLauncher"

#include "Launcher.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mutex>
#include <utility>

#include <cutils/properties.h>
#include <log/log.h>

#include "NanoLaunchCommit.h"
#include "NanoMenuUtils.h"   // setLaunchRomPath, setLaunchGame

extern char** environ;

namespace nanolinux {
namespace launcher {

const char kRetroarchPackage[] = "com.retroarch.aarch64";

namespace {

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string key(const controlkeys::LaunchRequest& h, const char* name) {
    auto it = h.keys.find(std::string("sys.gammaos.nano.") + name);
    return it == h.keys.end() ? std::string() : it->second;
}

std::string joined(const std::vector<std::string>& argv) {
    std::string out;
    for (const std::string& a : argv) {
        if (!out.empty()) out += ' ';
        out += a;
    }
    return out;
}

// The highest descriptor the child closes before execve, taken before fork.
int closeLimit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 65536) return 65536;
    return (int)rl.rlim_cur;
}

// The forked child, up to execve: async-signal-safe calls only (Nano has many threads).
[[noreturn]] void childExec(char* const* argv, int errFd, int maxFd) {
    setsid();
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    for (int s = 1; s < NSIG; s++) sigaction(s, &dfl, nullptr);  // SIGKILL, SIGSTOP and libc's own refuse
    for (int fd = 3; fd < maxFd; fd++) {
        if (fd != errFd) close(fd);
    }
    execve(argv[0], argv, environ);
    int e = errno;
    ssize_t w = write(errFd, &e, sizeof e);
    (void)w;
    _exit(127);
}

class LinuxLauncher : public nanoplat::Launcher {
 public:
    LinuxLauncher(nanoplat::Display* display, Options options) : display_(display), options_(std::move(options)) {}

    bool nanoExitsOnLaunch() const override { return false; }

    // As the firmware writes the handshake (platform/firmware/Launcher.cpp).
    void commit(const android::LaunchTuple& tuple) override {
        for (const android::LaunchTuple::Write& w : tuple.writes) {
            switch (w.key) {
                case android::LaunchKey::RomPath:
                    android::setLaunchRomPath(w.value);
                    break;
                case android::LaunchKey::Game:
                    android::setLaunchGame(w.value, w.rom, w.core);
                    break;
                default:
                    property_set(android::launchKeyProperty(w.key), w.value.c_str());
                    break;
            }
        }
    }

    bool launch(const nanoplat::LaunchRequest& request, std::string* error) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (pid_ > 0) return fail(error, "a game is already running (pid " + std::to_string(pid_) + ")");
        std::vector<std::string> argv;
        if (!argvFor(request, options_, &argv, error)) return false;
        if (access(argv[0].c_str(), X_OK) != 0) return fail(error, argv[0] + ": " + strerror(errno));
        if (request.kind == nanoplat::LaunchRequest::Kind::Core && access(argv[2].c_str(), R_OK) != 0)
            return fail(error, "core " + argv[2] + ": " + strerror(errno));

        // Everything the child needs, made before fork: it may not allocate.
        std::vector<char*> av;
        for (std::string& a : argv) av.push_back(&a[0]);
        av.push_back(nullptr);
        const int maxFd = closeLimit();
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) != 0) return fail(error, std::string("pipe2: ") + strerror(errno));

        const bool released = display_ != nullptr && display_->releaseForChild();
        ALOGI("launch (%s, by %s): %s%s", request.kind == nanoplat::LaunchRequest::Kind::Core ? "core" : "package",
              request.trigger.c_str(), joined(argv).c_str(), released ? "" : " (the panel was not Nano's)");
        const pid_t pid = fork();
        if (pid == 0) childExec(av.data(), pipeFds[1], maxFd);
        const int forkErr = errno;
        close(pipeFds[1]);
        if (pid < 0) {
            close(pipeFds[0]);
            if (released) display_->reacquire();
            return fail(error, std::string("fork: ") + strerror(forkErr));
        }
        // EOF: execve worked (the pipe closed on exec). An int: execve's errno.
        int childErr = 0;
        ssize_t n;
        do {
            n = read(pipeFds[0], &childErr, sizeof childErr);
        } while (n < 0 && errno == EINTR);
        close(pipeFds[0]);
        if (n == (ssize_t)sizeof childErr) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            if (released) display_->reacquire();
            return fail(error, "execve " + argv[0] + ": " + strerror(childErr));
        }
        pid_ = pid;
        ALOGI("launched pid %d", (int)pid);
        return true;
    }

    bool childRunning() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return pid_ > 0;
    }

    std::optional<nanoplat::ChildExit> pollChild() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (pid_ <= 0) return std::nullopt;
        int status = 0;
        const pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == 0 || (r < 0 && errno == EINTR)) return std::nullopt;
        nanoplat::ChildExit e;
        e.pid = pid_;
        if (r < 0) {
            ALOGW("waitpid %d: %s; taken as ended", (int)pid_, strerror(errno));
            e.status = -1;
        } else if (WIFSIGNALED(status)) {
            e.signaled = true;
            e.status = WTERMSIG(status);
        } else {
            e.status = WEXITSTATUS(status);
        }
        ALOGI("pid %d ended: %s %d", (int)pid_, e.signaled ? "signal" : "status", e.status);
        pid_ = -1;
        return e;
    }

    void requestChildQuit() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (pid_ <= 0) return;
        // launch() returned after execve, so the child's setsid has made its group.
        if (kill(-pid_, SIGTERM) != 0 && errno == ESRCH) kill(pid_, SIGTERM);
        ALOGI("SIGTERM to process group %d", (int)pid_);
    }

 private:
    nanoplat::Display* const display_;
    const Options options_;
    mutable std::mutex mu_;
    pid_t pid_ = -1;  // the child not yet reported by pollChild, or -1
};

}  // namespace

std::string coreName(const std::string& corePath) {
    const size_t slash = corePath.rfind('/');
    std::string name = slash == std::string::npos ? corePath : corePath.substr(slash + 1);
    for (const char* suffix : {"_libretro_android.so", "_libretro.so", ".so"}) {
        if (endsWith(name, suffix)) {
            name.resize(name.size() - strlen(suffix));
            break;
        }
    }
    if (name.empty() || name[0] == '.') return "";
    return name;
}

bool argvFor(const nanoplat::LaunchRequest& request, const Options& options, std::vector<std::string>* argv,
             std::string* error) {
    argv->clear();
    if (request.kind == nanoplat::LaunchRequest::Kind::Core) {
        const std::string name = coreName(request.core);
        if (name.empty()) return fail(error, "no core in \"" + request.core + "\"");
        *argv = {options.retroarch, "-L", options.coreDir + "/" + name + "_libretro.so", "--config",
                 options.retroarchConfig};
        if (!request.rom.empty()) argv->push_back(request.rom);
        return true;
    }
    if (request.package == kRetroarchPackage) {
        *argv = {options.retroarch, "--config", options.retroarchConfig};
        return true;
    }
    // TODO(plan 6.4): the emulator definitions' Linux block names the emulator for a package.
    return fail(error, "no Linux emulator for package \"" + request.package + "\"");
}

nanoplat::LaunchRequest requestFromHandshake(const controlkeys::LaunchRequest& handshake) {
    nanoplat::LaunchRequest r;
    r.package = key(handshake, "launch_app");
    r.core = key(handshake, "launch_core");
    r.rom = key(handshake, "launch_rom");
    if (r.rom.empty()) r.rom = key(handshake, "launch_game");
    r.system = key(handshake, "launch_system");
    r.intent = key(handshake, "launch_intent");
    r.trigger = handshake.trigger;
    r.kind = r.core.empty() ? nanoplat::LaunchRequest::Kind::Package : nanoplat::LaunchRequest::Kind::Core;
    return r;
}

std::unique_ptr<nanoplat::Launcher> create(nanoplat::Display* display, const Options& options) {
    return std::unique_ptr<nanoplat::Launcher>(new LinuxLauncher(display, options));
}

void launchFromHandshake(const controlkeys::LaunchRequest& handshake) {
    const nanoplat::LaunchRequest request = requestFromHandshake(handshake);
    std::string error;
    if (!nanoplat::get().launcher->launch(request, &error))
        ALOGW("launch by %s not started: %s", handshake.trigger.c_str(), error.c_str());
}

std::optional<nanoplat::ChildExit> waitForChild(nanoplat::Launcher& launcher, int pollMs) {
    while (launcher.childRunning()) {
        if (std::optional<nanoplat::ChildExit> e = launcher.pollChild()) return e;
        usleep((useconds_t)(pollMs > 0 ? pollMs : 1) * 1000);
    }
    return std::nullopt;
}

}  // namespace launcher
}  // namespace nanolinux
