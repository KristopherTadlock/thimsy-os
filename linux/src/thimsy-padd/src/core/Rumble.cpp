// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Rumble.h"

#include <algorithm>
#include <cerrno>

namespace padd {
namespace {
constexpr int64_t kMs = 1000000;
}

bool Rumble::activeAt(const Slot& s, int64_t t, int64_t* changeNs) const {
    *changeNs = -1;
    if (!s.playing) return false;
    int64_t delay = static_cast<int64_t>(s.effect.delayMs) * kMs;
    int64_t length = static_cast<int64_t>(s.effect.lengthMs) * kMs;
    int64_t elapsed = t - s.startNs;
    if (elapsed < 0) {
        *changeNs = s.startNs + delay;
        return false;
    }
    if (length == 0) {
        if (elapsed < delay) {
            *changeNs = s.startNs + delay;
            return false;
        }
        return true;
    }
    int64_t period = delay + length;
    int64_t rep = elapsed / period;
    if (rep >= s.count) return false;
    int64_t within = elapsed - rep * period;
    if (within < delay) {
        *changeNs = s.startNs + rep * period + delay;
        return false;
    }
    *changeNs = s.startNs + (rep + 1) * period;
    return true;
}

void Rumble::update(int64_t nowNs, std::vector<VibCommand>* out) {
    now_ = nowNs;
    uint32_t strong = 0, weak = 0;
    int64_t soonestEnd = -1;
    for (auto& kv : slots_) {
        Slot& s = kv.second;
        if (!s.playing) continue;
        int64_t change = -1;
        bool active = activeAt(s, nowNs, &change);
        if (!active && change < 0) {
            s.playing = false;  // all repeats done
            continue;
        }
        if (active) {
            strong += s.effect.strong;
            weak += s.effect.weak;
            if (change >= 0 && (soonestEnd < 0 || change < soonestEnd)) soonestEnd = change;
        }
    }
    auto scale = [&](uint32_t v) {
        v = std::min<uint32_t>(v, 0xffff);
        return static_cast<uint16_t>((static_cast<uint64_t>(v) * gain_ + 0x7fff) / 0xffff);
    };
    VibCommand c;
    c.strong = scale(strong);
    c.weak = scale(weak);
    c.on = c.strong != 0 || c.weak != 0;
    if (!c.on) {
        c.strong = c.weak = 0;
    } else if (soonestEnd >= 0) {
        c.durationMs = static_cast<uint32_t>((soonestEnd - nowNs + kMs - 1) / kMs);
    }
    bool changed = c.on != last_.on || c.strong != last_.strong || c.weak != last_.weak;
    // The last command ran out on its own while the sum stayed the same: renew it.
    bool expired = c.on && last_.on && last_.durationMs != 0 && lastExpiryNs_ <= nowNs;
    if (changed || expired) {
        out->push_back(c);
        last_ = c;
        lastExpiryNs_ = c.on && c.durationMs ? nowNs + static_cast<int64_t>(c.durationMs) * kMs : -1;
    }
}

int Rumble::upload(const RumbleEffect& e, int64_t nowNs, std::vector<VibCommand>* out) {
    if (e.id < 0 || e.id >= maxEffects_) return -EINVAL;
    Slot& s = slots_[e.id];
    s.effect = e;
    update(nowNs, out);
    return 0;
}

int Rumble::erase(int id, int64_t nowNs, std::vector<VibCommand>* out) {
    if (!slots_.erase(id)) return -EINVAL;
    update(nowNs, out);
    return 0;
}

void Rumble::play(int id, int count, int64_t nowNs, std::vector<VibCommand>* out) {
    auto it = slots_.find(id);
    if (it == slots_.end()) return;
    if (count > 0) {
        it->second.playing = true;
        it->second.startNs = nowNs;
        it->second.count = count;
    } else {
        it->second.playing = false;
    }
    update(nowNs, out);
}

void Rumble::setGain(uint16_t gain, int64_t nowNs, std::vector<VibCommand>* out) {
    gain_ = gain;
    update(nowNs, out);
}

void Rumble::tick(int64_t nowNs, std::vector<VibCommand>* out) { update(nowNs, out); }

int64_t Rumble::nextDeadlineNs() const {
    int64_t next = -1;
    for (const auto& kv : slots_) {
        int64_t change = -1;
        activeAt(kv.second, now_, &change);
        if (change >= 0 && (next < 0 || change < next)) next = change;
    }
    if (last_.on && lastExpiryNs_ >= 0 && (next < 0 || lastExpiryNs_ < next)) next = lastExpiryNs_;
    return next;
}

void Rumble::stopAll(int64_t nowNs, std::vector<VibCommand>* out) {
    for (auto& kv : slots_) kv.second.playing = false;
    update(nowNs, out);
}

}  // namespace padd
