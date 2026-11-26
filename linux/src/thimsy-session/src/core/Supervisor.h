// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// What runs, and when (IMPLEMENTATION F.1). Pure: the loop reports what happened, the supervisor
// answers with actions; time comes in as "now".
//
//   Nano      started at once. When it exits it is started again: at once if it ran for at least
//             minUptimeMs, else after a back-off that doubles from backoffStartMs to backoffMaxMs.
//             If it exits while the game it launched still runs (as Android's home exits when it
//             starts a game), it is started again when that game ends, without a back-off.
//   deferred  "rcD start" once per boot: at Nano's first frame (sys.thimsy.nano.first_frame=1), or
//             deferredFallbackMs after Nano was first started if no first frame came, so a broken
//             Nano still gets SSH and the drives. Never twice: a respawned session that finds the
//             marker of an earlier run starts with it done.
//   padd      S02padd starts thimsy-padd once; if it is not running when the session starts, or
//             when it dies, the session starts it, with the same back-off rule.
//   scrape    thimsy-nano-scrape on request (sys.gammaos.nano.scrape_run, or "scrape"), as
//             gammaos-nano.rc's oneshot service: a request while it runs does nothing (it reads the
//             queue itself).
//   quit      the game's group gets SIGCONT (it may be stopped) and SIGTERM, then SIGKILL after
//             quitKillMs.
//   shutdown  after poweroff or reboot is asked for, nothing is started again.
#pragma once

#include <vector>

#include "core/Types.h"

namespace thimsy {
namespace session {

struct SupervisorConfig {
    Ms minUptimeMs = 30000;
    Ms backoffStartMs = 1000;
    Ms backoffMaxMs = 30000;
    Ms deferredFallbackMs = 30000;
    Ms quitKillMs = 3000;
};

enum class SupKind {
    SpawnNano,
    SpawnPadd,
    SpawnScrape,
    RunDeferred,
    SignalGroup,  // pgid, sig
    WatchGame,    // pid: tell gameExited() when it ends
};

struct SupAction {
    SupAction(SupKind k, int p = 0, int s = 0) : kind(k), pid(p), sig(s) {}
    SupKind kind;
    int pid = 0;  // pgid for SignalGroup, pid for WatchGame
    int sig = 0;
};

const char* toString(SupKind k);

class Supervisor {
 public:
    explicit Supervisor(SupervisorConfig config = {});

    // deferredAlreadyRan: an earlier session of this boot ran rcD; paddAlive: thimsy-padd runs.
    std::vector<SupAction> start(Ms now, bool deferredAlreadyRan, bool paddAlive);

    // Results of the spawn actions (pid > 0) or their failure (pid <= 0, counted as a crash).
    std::vector<SupAction> nanoStarted(Ms now, int pid);
    std::vector<SupAction> paddStarted(Ms now, int pid);
    void scrapeStarted(int pid);

    // gamePid: the game Nano launched if it still runs, else 0.
    std::vector<SupAction> nanoExited(Ms now, int gamePid);
    std::vector<SupAction> gameExited(Ms now);
    std::vector<SupAction> paddExited(Ms now);
    void scrapeExited();

    std::vector<SupAction> firstFrame(Ms now);
    std::vector<SupAction> scrapeRequested(Ms now);
    std::vector<SupAction> quitGame(Ms now, int pgid);
    std::vector<SupAction> restartNano(Ms now);  // Nano is stopped and started again at once
    void shutdown();

    std::vector<SupAction> onTick(Ms now);
    Ms nextDeadline() const;

    bool nanoRunning() const { return nano_.pid > 0; }
    int nanoPid() const { return nano_.pid; }
    bool deferredDone() const { return deferredDone_; }
    bool shuttingDown() const { return shutdown_; }
    bool waitingForGame() const { return waitingForGame_; }
    Ms nanoBackoff() const { return nano_.backoff; }
    Ms nanoRestartAt() const { return nano_.restartAt; }

 private:
    struct Restartable {
        int pid = 0;
        Ms startedAt = 0;
        Ms backoff = 0;
        Ms restartAt = kNever;
    };
    // Schedules the next start of r after an exit at `now`.
    void scheduleRestart(Restartable* r, Ms now);

    SupervisorConfig config_;
    Restartable nano_;
    bool nanoStarting_ = false;
    bool waitingForGame_ = false;
    bool restartRequested_ = false;
    Ms firstNanoStart_ = kNever;
    bool deferredDone_ = false;
    Restartable padd_;
    bool paddStarting_ = false;
    int scrapePid_ = 0;
    bool scrapeStarting_ = false;
    int quitPgid_ = 0;
    Ms quitKillAt_ = kNever;
    bool shutdown_ = false;
};

}  // namespace session
}  // namespace thimsy
