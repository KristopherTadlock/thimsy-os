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

#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include <cutils/properties.h>
#include <log/log.h>

#include "NanoLaunchCommit.h"
#include "NanoMenuUtils.h"   // setLaunchRomPath, setLaunchGame
#include "ShellGate.h"       // platformExecve

extern char** environ;

namespace nanolinux {
namespace launcher {

const char kRetroarchPackage[] = "com.retroarch.aarch64";
const char kGamePidKey[] = "sys.thimsy.game.pid";
const char kAppLaunchedKey[] = "sys.gammaos.nano.app_launched";

namespace {

constexpr const char kKeyPrefix[] = "sys.gammaos.nano.";

// What Android's framework clears when it returns to Nano (nanoStopAppToHome), in its order.
constexpr const char* kClearedAtEnd[] = {
    "sys.gammaos.nano.launch_app",
    "sys.gammaos.nano.launch_intent",
    "sys.gammaos.nano.launch_core",
    "sys.gammaos.nano.launch_rom",
};

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string key(const controlkeys::LaunchRequest& h, const char* name) {
    auto it = h.keys.find(std::string(kKeyPrefix) + name);
    return it == h.keys.end() ? std::string() : it->second;
}

std::string getProp(const char* name) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(name, v, "");
    return v;
}

// The request from the handshake's keys, each read through `value` ("launch_app" and the rest).
nanoplat::LaunchRequest requestFrom(const std::function<std::string(const char*)>& value, const std::string& trigger) {
    nanoplat::LaunchRequest r;
    r.package = value("launch_app");
    r.core = value("launch_core");
    r.rom = value("launch_rom");
    if (r.rom.empty()) r.rom = value("launch_game");
    r.system = value("launch_system");
    r.intent = value("launch_intent");
    r.trigger = trigger;
    r.kind = r.core.empty() ? nanoplat::LaunchRequest::Kind::Package : nanoplat::LaunchRequest::Kind::Core;
    return r;
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
    // The platform's own start (OA-5), not through the shell gate, which refuses by default.
    nanolinux::shellgate::platformExecve(argv[0], argv, environ);
    int e = errno;
    ssize_t w = write(errFd, &e, sizeof e);
    (void)w;
    _exit(127);
}

// The launcher whose commit() is running on this thread, if any: the control keys call
// launchFromHandshake on the writing thread, inside the trigger's property_set.
thread_local const nanoplat::Launcher* tCommitting = nullptr;

class LinuxLauncherImpl : public LinuxLauncher {
 public:
    LinuxLauncherImpl(nanoplat::Display* display, Options options) : display_(display), options_(std::move(options)) {}

    // The intended contract (Launcher.h): a resident Nano. Nothing reads it yet.
    bool nanoExitsOnLaunch() const override { return false; }

    // As the firmware writes the handshake (platform/firmware/Launcher.cpp), then each key's full
    // value is noted beside what the store kept of it.
    void commit(const android::LaunchTuple& tuple) override {
        const nanoplat::Launcher* outer = tCommitting;
        tCommitting = this;
        for (const android::LaunchTuple::Write& w : tuple.writes) {
            switch (w.key) {
                case android::LaunchKey::RomPath:
                    android::setLaunchRomPath(w.value);
                    note("sys.gammaos.nano.launch_rom", w.value);
                    note("sys.gammaos.nano.launch_system", "");
                    note("sys.gammaos.nano.launch_game", "");
                    break;
                case android::LaunchKey::Game:
                    android::setLaunchGame(w.value, w.rom, w.core);
                    note("sys.gammaos.nano.launch_system", w.value);
                    note("sys.gammaos.nano.launch_game", w.rom);
                    break;
                default:
                    property_set(android::launchKeyProperty(w.key), w.value.c_str());
                    note(android::launchKeyProperty(w.key), w.value);
                    break;
            }
        }
        tCommitting = outer;
    }

    nanoplat::LaunchRequest requestFor(const controlkeys::LaunchRequest& handshake) const override {
        std::lock_guard<std::mutex> lock(mu_);
        return requestFrom(
                [&](const char* name) {
                    const std::string full = std::string(kKeyPrefix) + name;
                    const std::string stored = key(handshake, name);
                    auto it = written_.find(full);
                    return it != written_.end() && it->second.stored == stored ? it->second.full : stored;
                },
                handshake.trigger);
    }

    bool inCommit() const override { return tCommitting == this; }

    // Checks the request and queues it: startQueued() starts it once Nano's menu has ended.
    bool launch(const nanoplat::LaunchRequest& request, std::string* error) override {
        std::lock_guard<std::mutex> lock(mu_);
        requested_ = true;
        if (pid_ > 0) return fail(error, "a game is already running (pid " + std::to_string(pid_) + ")");
        if (queued_) return fail(error, "a launch is already queued: " + joined(queued_->argv));
        std::vector<std::string> argv;
        if (!argvFor(request, options_, &argv, error)) return false;
        if (access(argv[0].c_str(), X_OK) != 0) return fail(error, argv[0] + ": " + strerror(errno));
        if (request.kind == nanoplat::LaunchRequest::Kind::Core && access(argv[2].c_str(), R_OK) != 0)
            return fail(error, "core " + argv[2] + ": " + strerror(errno));
        ALOGI("launch (%s, by %s) queued until Nano's menu has ended: %s",
              request.kind == nanoplat::LaunchRequest::Kind::Core ? "core" : "package", request.trigger.c_str(),
              joined(argv).c_str());
        queued_.reset(new Queued{request, std::move(argv)});
        return true;
    }

    bool queued() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return queued_ != nullptr;
    }

    bool startQueued(std::string* error) override {
        bool ok = false;
        bool cleared = false;
        pid_t started = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::unique_ptr<Queued> q = std::move(queued_);
            const bool requested = requested_;
            if (!q) {
                if (error) error->clear();
                cleared = requested;  // a launch was asked for and refused: it is over
            } else {
                ok = start(*q, error);
                cleared = !ok;
                if (ok) started = pid_;
            }
            if (cleared) requested_ = false;
        }
        if (started > 0) setGamePid(started);
        if (cleared) clearHandshake();
        return ok;
    }

    Leave startAndLeave(int* pidOut, std::string* error) override {
        Leave end = Leave::NoLaunch;
        pid_t started = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::unique_ptr<Queued> q = std::move(queued_);
            if (error) error->clear();
            if (q) {
                if (start(*q, error)) {
                    end = Leave::Started;
                    started = pid_;
                    pid_ = -1;  // left running: Nano exits, and the session watches it
                } else {
                    end = Leave::NotStarted;
                }
            } else if (requested_) {
                end = Leave::NotStarted;  // asked for and refused
            }
            requested_ = false;
        }
        if (started > 0) {
            setGamePid(started);
            ALOGI("pid %d left running: Nano exits, thimsy-session watches it (%s)", (int)started, kGamePidKey);
        }
        if (end != Leave::NoLaunch) clearHandshake();
        if (pidOut) *pidOut = started;
        return end;
    }

    bool childRunning() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return pid_ > 0;
    }

    std::optional<nanoplat::ChildExit> pollChild() override {
        nanoplat::ChildExit e;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (pid_ <= 0) return std::nullopt;
            int status = 0;
            const pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == 0 || (r < 0 && errno == EINTR)) return std::nullopt;
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
            requested_ = false;
        }
        if (getProp(kGamePidKey) == std::to_string(e.pid)) property_set(kGamePidKey, "");
        clearHandshake();
        return e;
    }

    void requestChildQuit() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (pid_ <= 0) return;
        // startQueued() returned after execve, so the child's setsid has made its group.
        if (kill(-pid_, SIGTERM) != 0 && errno == ESRCH) kill(pid_, SIGTERM);
        ALOGI("SIGTERM to process group %d", (int)pid_);
    }

 private:
    struct Queued {
        nanoplat::LaunchRequest request;
        std::vector<std::string> argv;
    };
    struct Written {
        std::string full;    // the value commit() wrote
        std::string stored;  // what the store held right after
    };

    void note(const char* name, const std::string& full) {
        const std::string stored = getProp(name);
        std::lock_guard<std::mutex> lock(mu_);
        written_[name] = Written{full, stored};
    }

    // The session's view of the game (Launcher.h): outside mu_, as an observer may run.
    static void setGamePid(pid_t pid) { property_set(kGamePidKey, std::to_string(pid).c_str()); }

    // The launch is over: the keys the framework clears when it returns to Nano.
    void clearHandshake() {
        for (const char* k : kClearedAtEnd) {
            property_set(k, "");
            std::lock_guard<std::mutex> lock(mu_);
            written_.erase(k);
        }
        ALOGI("launch over: launch_app, launch_intent, launch_core and launch_rom cleared");
    }

    // The hand-off and the fork, under mu_.
    bool start(Queued& q, std::string* error) {
        std::vector<std::string>& argv = q.argv;
        // Everything the child needs, made before fork: it may not allocate.
        std::vector<char*> av;
        for (std::string& a : argv) av.push_back(&a[0]);
        av.push_back(nullptr);
        const int maxFd = closeLimit();
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) != 0) return fail(error, std::string("pipe2: ") + strerror(errno));

        // Usually false: threadLoop's exit path has already stopped the display (drmStop), and
        // releaseForChild says so itself; nothing is taken back after a failure then.
        const bool released = display_ != nullptr && display_->releaseForChild();
        ALOGI("launch (%s, by %s): %s", q.request.kind == nanoplat::LaunchRequest::Kind::Core ? "core" : "package",
              q.request.trigger.c_str(), joined(argv).c_str());
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

    nanoplat::Display* const display_;
    const Options options_;
    mutable std::mutex mu_;
    pid_t pid_ = -1;                       // the child not yet reported by pollChild, or -1
    std::unique_ptr<Queued> queued_;       // launch() accepted it; startQueued() starts it
    bool requested_ = false;               // launch() was called and the launch is not over
    std::map<std::string, Written> written_;  // by property name
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
    // launch_app "": RetroArch, as Nano reads it (launched_pkg's default) and the framework starts it.
    if (request.package.empty() || request.package == kRetroarchPackage) {
        *argv = {options.retroarch, "--config", options.retroarchConfig};
        return true;
    }
    // TODO(plan 6.4): the emulator definitions' Linux block names the emulator for a package.
    return fail(error, "no Linux emulator for package \"" + request.package + "\"");
}

nanoplat::LaunchRequest requestFromHandshake(const controlkeys::LaunchRequest& handshake) {
    return requestFrom([&](const char* name) { return key(handshake, name); }, handshake.trigger);
}

std::unique_ptr<LinuxLauncher> create(nanoplat::Display* display, const Options& options) {
    return std::unique_ptr<LinuxLauncher>(new LinuxLauncherImpl(display, options));
}

LinuxLauncher* linuxLauncher() {
    return dynamic_cast<LinuxLauncher*>(nanoplat::get().launcher.get());
}

void launchFromHandshake(const controlkeys::LaunchRequest& handshake) {
    LinuxLauncher* launcher = linuxLauncher();
    if (!launcher) {
        ALOGW("launch by %s not started: the platform's launcher is not Linux's", handshake.trigger.c_str());
        return;
    }
    if (!launcher->inCommit()) {
        // Another process wrote the trigger: no Nano launch site is about to exit, so there is no
        // menu end to start it at, and the panel is Nano's.
        ALOGW("launch by %s refused: the trigger was not written by Nano's commitLaunch()",
              handshake.trigger.c_str());
        return;
    }
    const nanoplat::LaunchRequest request = launcher->requestFor(handshake);
    std::string error;
    if (!launcher->launch(request, &error))
        ALOGW("launch by %s not started: %s", handshake.trigger.c_str(), error.c_str());
    // Android's framework sets app_launched when it starts the app, while the home still runs its
    // launch site; threadLoop's exit path waits up to ~10 s for it. On Linux the game starts only
    // once the menu has ended (leaveQueued), and Nano exits after a refused launch too, so the
    // launch is as far as it gets while the menu runs: say so now, and the exit path ends at once.
    // thimsy-session clears the key when the game ends and before it starts Nano again.
    if (getProp(kAppLaunchedKey) != "1") property_set(kAppLaunchedKey, "1");
}

std::optional<nanoplat::ChildExit> waitForChild(nanoplat::Launcher& launcher, int pollMs) {
    while (launcher.childRunning()) {
        if (std::optional<nanoplat::ChildExit> e = launcher.pollChild()) return e;
        usleep((useconds_t)(pollMs > 0 ? pollMs : 1) * 1000);
    }
    return std::nullopt;
}

LinuxLauncher::Leave leaveQueued(int* pid) {
    if (pid) *pid = -1;
    LinuxLauncher* launcher = linuxLauncher();
    if (!launcher) return LinuxLauncher::Leave::NoLaunch;
    std::string error;
    const LinuxLauncher::Leave end = launcher->startAndLeave(pid, &error);
    if (end == LinuxLauncher::Leave::NotStarted && !error.empty())
        ALOGW("the queued launch did not start: %s", error.c_str());
    return end;
}

std::optional<nanoplat::ChildExit> runQueued(int pollMs) {
    LinuxLauncher* launcher = linuxLauncher();
    if (!launcher) return std::nullopt;
    std::string error;
    if (!launcher->startQueued(&error)) {
        if (!error.empty()) ALOGW("the queued launch did not start: %s", error.c_str());
        return std::nullopt;
    }
    return waitForChild(*launcher, pollMs);
}

}  // namespace launcher
}  // namespace nanolinux
