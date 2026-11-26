// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Supervisor.h"

#include <signal.h>

#include <algorithm>

namespace thimsy {
namespace session {

const char* toString(SupKind k) {
    switch (k) {
        case SupKind::SpawnNano: return "spawn-nano";
        case SupKind::SpawnPadd: return "spawn-padd";
        case SupKind::SpawnScrape: return "spawn-scrape";
        case SupKind::RunDeferred: return "run-deferred";
        case SupKind::SignalGroup: return "signal-group";
        case SupKind::WatchGame: return "watch-game";
    }
    return "?";
}

Supervisor::Supervisor(SupervisorConfig config) : config_(config) {}

void Supervisor::scheduleRestart(Restartable* r, Ms now) {
    r->pid = 0;
    if (now - r->startedAt >= config_.minUptimeMs) {
        r->backoff = 0;
        r->restartAt = now;
        return;
    }
    r->backoff = r->backoff == 0 ? config_.backoffStartMs : std::min(r->backoff * 2, config_.backoffMaxMs);
    r->restartAt = now + r->backoff;
}

std::vector<SupAction> Supervisor::start(Ms now, bool deferredAlreadyRan, bool paddAlive) {
    std::vector<SupAction> out;
    deferredDone_ = deferredAlreadyRan;
    nanoStarting_ = true;
    out.push_back({SupKind::SpawnNano});
    padd_.startedAt = now;
    if (!paddAlive) {
        paddStarting_ = true;
        out.push_back({SupKind::SpawnPadd});
    }
    return out;
}

std::vector<SupAction> Supervisor::nanoStarted(Ms now, int pid) {
    nanoStarting_ = false;
    nano_.startedAt = now;
    if (pid > 0) {
        nano_.pid = pid;
        if (firstNanoStart_ == kNever) firstNanoStart_ = now;
    } else {
        scheduleRestart(&nano_, now);
    }
    return {};
}

std::vector<SupAction> Supervisor::paddStarted(Ms now, int pid) {
    paddStarting_ = false;
    padd_.startedAt = now;
    if (pid > 0) {
        padd_.pid = pid;
    } else {
        scheduleRestart(&padd_, now);
    }
    return {};
}

void Supervisor::scrapeStarted(int pid) {
    scrapeStarting_ = false;
    scrapePid_ = pid > 0 ? pid : 0;
}

std::vector<SupAction> Supervisor::nanoExited(Ms now, int gamePid) {
    std::vector<SupAction> out;
    nano_.pid = 0;
    if (shutdown_) return out;
    if (restartRequested_) {
        restartRequested_ = false;
        nano_.restartAt = now;
        return out;
    }
    if (gamePid > 0) {
        waitingForGame_ = true;
        nano_.restartAt = kNever;
        SupAction a{SupKind::WatchGame};
        a.pid = gamePid;
        out.push_back(a);
        return out;
    }
    scheduleRestart(&nano_, now);
    return out;
}

std::vector<SupAction> Supervisor::gameExited(Ms now) {
    std::vector<SupAction> out;
    if (!waitingForGame_) return out;
    waitingForGame_ = false;
    if (shutdown_ || nano_.pid > 0 || nanoStarting_) return out;
    nano_.restartAt = kNever;
    nano_.backoff = 0;
    nanoStarting_ = true;
    (void)now;
    out.push_back({SupKind::SpawnNano});
    return out;
}

std::vector<SupAction> Supervisor::paddExited(Ms now) {
    padd_.pid = 0;
    if (!shutdown_) scheduleRestart(&padd_, now);
    return {};
}

void Supervisor::scrapeExited() { scrapePid_ = 0; }

std::vector<SupAction> Supervisor::firstFrame(Ms now) {
    (void)now;
    std::vector<SupAction> out;
    if (deferredDone_ || shutdown_) return out;
    deferredDone_ = true;
    out.push_back({SupKind::RunDeferred});
    return out;
}

std::vector<SupAction> Supervisor::scrapeRequested(Ms now) {
    (void)now;
    std::vector<SupAction> out;
    if (shutdown_ || scrapePid_ > 0 || scrapeStarting_) return out;
    scrapeStarting_ = true;
    out.push_back({SupKind::SpawnScrape});
    return out;
}

std::vector<SupAction> Supervisor::quitGame(Ms now, int pgid) {
    std::vector<SupAction> out;
    if (pgid <= 1) return out;
    SupAction cont{SupKind::SignalGroup, pgid, SIGCONT};
    SupAction term{SupKind::SignalGroup, pgid, SIGTERM};
    out.push_back(cont);
    out.push_back(term);
    quitPgid_ = pgid;
    quitKillAt_ = now + config_.quitKillMs;
    return out;
}

std::vector<SupAction> Supervisor::restartNano(Ms now) {
    std::vector<SupAction> out;
    if (shutdown_) return out;
    if (nano_.pid > 0) {
        restartRequested_ = true;
        out.push_back({SupKind::SignalGroup, nano_.pid, SIGTERM});
        return out;
    }
    if (!nanoStarting_) {
        waitingForGame_ = false;
        nano_.backoff = 0;
        nano_.restartAt = now;
    }
    return out;
}

void Supervisor::shutdown() { shutdown_ = true; }

std::vector<SupAction> Supervisor::onTick(Ms now) {
    std::vector<SupAction> out;
    if (!shutdown_ && nano_.pid == 0 && !nanoStarting_ && !waitingForGame_ && now >= nano_.restartAt) {
        nano_.restartAt = kNever;
        nanoStarting_ = true;
        out.push_back({SupKind::SpawnNano});
    }
    if (!shutdown_ && padd_.pid == 0 && !paddStarting_ && now >= padd_.restartAt) {
        padd_.restartAt = kNever;
        paddStarting_ = true;
        out.push_back({SupKind::SpawnPadd});
    }
    if (!deferredDone_ && !shutdown_ && firstNanoStart_ != kNever &&
        now >= firstNanoStart_ + config_.deferredFallbackMs) {
        deferredDone_ = true;
        out.push_back({SupKind::RunDeferred});
    }
    if (quitPgid_ > 0 && now >= quitKillAt_) {
        out.push_back({SupKind::SignalGroup, quitPgid_, SIGKILL});
        quitPgid_ = 0;
        quitKillAt_ = kNever;
    }
    return out;
}

Ms Supervisor::nextDeadline() const {
    Ms d = kNever;
    if (!shutdown_ && nano_.pid == 0 && !nanoStarting_ && !waitingForGame_) d = earliest(d, nano_.restartAt);
    if (!shutdown_ && padd_.pid == 0 && !paddStarting_) d = earliest(d, padd_.restartAt);
    if (!deferredDone_ && !shutdown_ && firstNanoStart_ != kNever) {
        d = earliest(d, firstNanoStart_ + config_.deferredFallbackMs);
    }
    if (quitPgid_ > 0) d = earliest(d, quitKillAt_);
    return d;
}

}  // namespace session
}  // namespace thimsy
