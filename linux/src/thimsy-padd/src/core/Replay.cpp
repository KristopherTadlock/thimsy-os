// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Replay.h"

#include <linux/input.h>

#include <cstdio>
#include <cstdlib>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <vector>

#include "core/InputCodes.h"
#include "core/Merger.h"
#include "core/PadSpec.h"
#include "core/Rumble.h"

namespace padd {
namespace {

constexpr int64_t kMs = 1000000;

// Splits on blanks; "double quotes" group words (also inside key="a b"); quotes are removed.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false, have = false;
    for (char c : line) {
        if (c == '"') {
            inQuote = !inQuote;
            have = true;
        } else if (!inQuote && (c == ' ' || c == '\t' || c == '\r')) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

bool parseInt(const std::string& s, int64_t* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    long long x = std::strtoll(s.c_str(), &end, 0);
    if (!end || *end) return false;
    *v = x;
    return true;
}

std::string hex(int v, int width) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%0*x", width, v);
    return buf;
}

class Replayer {
 public:
    Replayer(const std::string& dir, std::ostream& out, const std::string& deviceOverride)
        : dir_(dir), out_(out), device_(deviceOverride), deviceFixed_(!deviceOverride.empty()) {}

    bool line(const std::string& text, std::string* err);
    bool finish(std::string* err);

 private:
    bool header(const std::vector<std::string>& t, std::string* err);
    bool start(std::string* err);
    bool timed(int64_t ms, const std::vector<std::string>& t, std::string* err);
    int sourceOf(const std::string& label, int64_t ms);
    void print(int64_t ms, const std::vector<OutEvent>& evs);
    void printVib(int64_t ms, const std::vector<VibCommand>& cmds);
    void drainRumble(int64_t uptoMs);

    std::string dir_;
    std::ostream& out_;
    std::string device_;
    bool deviceFixed_;

    std::vector<std::pair<std::string, NodeInfo>> nodes_;
    std::map<std::string, SourceCaps> caps_;

    bool started_ = false;
    PadSpec spec_;
    std::unique_ptr<Merger> merger_;
    Rumble rumble_;
    std::map<std::string, int> bound_;          // label -> source
    std::set<std::string> ignored_;
    std::vector<bool> taken_;
    int64_t lastMs_ = 0;
};

bool Replayer::line(const std::string& text, std::string* err) {
    std::vector<std::string> t = tokenize(text);
    if (t.empty() || t[0][0] == '#') return true;
    int64_t ms;
    if (parseInt(t[0], &ms)) {
        if (!started_ && !start(err)) return false;
        if (ms < lastMs_) {
            *err = "time goes backwards";
            return false;
        }
        drainRumble(ms);
        lastMs_ = ms;
        t.erase(t.begin());
        return timed(ms, t, err);
    }
    if (started_) {
        *err = "'" + t[0] + "' must come before the first timed line";
        return false;
    }
    return header(t, err);
}

bool Replayer::header(const std::vector<std::string>& t, std::string* err) {
    if (t[0] == "device" && t.size() == 2) {
        if (!deviceFixed_) device_ = t[1][0] == '/' ? t[1] : dir_ + "/" + t[1];
        return true;
    }
    if (t[0] == "node" && t.size() >= 3) {
        NodeInfo n;
        bool named = false;
        for (size_t i = 2; i < t.size(); ++i) {
            size_t eq = t[i].find('=');
            if (eq == std::string::npos) {
                *err = "node: expected key=value, got '" + t[i] + "'";
                return false;
            }
            std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
            int64_t num = 0;
            if (k == "name") {
                n.name = v;
                named = true;
            } else if (k == "phys") {
                n.phys = v;
            } else if (k == "uniq") {
                n.uniq = v;
            } else if ((k == "bus" || k == "vendor" || k == "product") && parseInt(v, &num)) {
                (k == "bus" ? n.bus : k == "vendor" ? n.vendor : n.product) = static_cast<int>(num);
            } else {
                *err = "node: bad '" + t[i] + "'";
                return false;
            }
        }
        if (!named) {
            *err = "node: name= is needed";
            return false;
        }
        nodes_.push_back({t[1], n});
        return true;
    }
    if (t[0] == "caps" && t.size() >= 3) {
        SourceCaps& c = caps_[t[1]];
        if (t[2] == "keys") {
            for (size_t i = 3; i < t.size(); ++i) {
                int code = eventCodeByName(EV_KEY, t[i]);
                if (code < 0) {
                    *err = "caps: unknown key '" + t[i] + "'";
                    return false;
                }
                c.keys.insert(static_cast<uint16_t>(code));
            }
            return true;
        }
        if (t[2] == "abs" && (t.size() == 6 || t.size() == 8)) {
            int code = eventCodeByName(EV_ABS, t[3]);
            int64_t v[4] = {0, 0, 0, 0};
            bool ok = code >= 0;
            for (size_t i = 4; i < t.size() && ok; ++i) ok = parseInt(t[i], &v[i - 4]);
            if (!ok) {
                *err = "caps: bad abs line";
                return false;
            }
            AbsInfo a;
            a.min = static_cast<int32_t>(v[0]);
            a.max = static_cast<int32_t>(v[1]);
            a.fuzz = static_cast<int32_t>(v[2]);
            a.flat = static_cast<int32_t>(v[3]);
            c.abs[static_cast<uint16_t>(code)] = a;
            return true;
        }
        if (t[2] == "ff" && t.size() == 4 && t[3] == "FF_RUMBLE") {
            c.ff.insert(FF_RUMBLE);
            return true;
        }
    }
    *err = "unknown statement '" + t[0] + "'";
    return false;
}

bool Replayer::start(std::string* err) {
    started_ = true;
    if (device_.empty()) {
        *err = "no device line";
        return false;
    }
    std::vector<std::string> warnings;
    if (!loadPadSpec(device_, &spec_, err, &warnings)) return false;
    for (const std::string& w : warnings) out_ << "# warning: " << w << "\n";
    taken_.assign(spec_.sources.size(), false);
    std::vector<std::optional<SourceCaps>> caps(spec_.sources.size());
    for (const auto& n : nodes_) {
        auto c = caps_.find(n.first);
        SourceCaps sc = c == caps_.end() ? SourceCaps{} : c->second;
        std::string why;
        int s = pickSource(spec_, n.second, sc, taken_, &why);
        if (s < 0) {
            out_ << "ignore " << n.first << ": " << why << "\n";
            ignored_.insert(n.first);
            continue;
        }
        taken_[s] = true;
        bound_[n.first] = s;
        caps[s] = sc;
        out_ << "bind " << n.first << " -> " << spec_.sources[s].id << "\n";
    }
    // Labels with caps but no node line are sources by id.
    for (const auto& c : caps_) {
        if (bound_.count(c.first) || ignored_.count(c.first)) continue;
        int s = spec_.sourceIndex(c.first);
        if (s >= 0 && !taken_[s]) caps[s] = c.second;
    }
    merger_.reset(new Merger(spec_, caps));
    rumble_ = Rumble();

    const Layout& l = merger_->layout();
    auto ident = [&](const char* dev, const char* name, uint16_t product) {
        out_ << "setup " << dev << " name=\"" << name << "\" bus=" << hex(kBusVirtual, 2)
             << " vendor=" << hex(kVendor, 4) << " product=" << hex(product, 4) << " version=" << kVersion
             << "\n";
    };
    ident("pad", kPadName, kPadProduct);
    out_ << "setup pad keys";
    for (uint16_t k : l.padKeys) out_ << " " << eventCodeName(EV_KEY, k);
    out_ << "\n";
    for (const auto& a : l.padAbs) {
        out_ << "setup pad abs " << eventCodeName(EV_ABS, a.first) << " " << a.second.min << " "
             << a.second.max << "\n";
    }
    if (l.ff) out_ << "setup pad ff FF_RUMBLE FF_GAIN effects=" << rumble_.maxEffects() << "\n";
    if (!l.keysKeys.empty()) {
        ident("keys", kKeysName, kKeysProduct);
        out_ << "setup keys keys";
        for (uint16_t k : l.keysKeys) out_ << " " << eventCodeName(EV_KEY, k);
        out_ << "\n";
    }
    return true;
}

int Replayer::sourceOf(const std::string& label, int64_t ms) {
    auto it = bound_.find(label);
    if (it != bound_.end()) return it->second;
    if (ignored_.count(label)) return -1;
    int s = spec_.sourceIndex(label);
    bool declared = false;
    for (const auto& n : nodes_) declared |= n.first == label;
    if (s >= 0 && !taken_[s] && !declared) {
        taken_[s] = true;
        bound_[label] = s;
        out_ << ms << " bind " << label << " -> " << spec_.sources[s].id << "\n";
        return s;
    }
    out_ << ms << " ignore " << label << ": matches no source\n";
    ignored_.insert(label);
    return -1;
}

void Replayer::print(int64_t ms, const std::vector<OutEvent>& evs) {
    for (const OutEvent& e : evs) {
        const char* dev = e.dev == Dev::Pad ? "pad" : e.dev == Dev::Keys ? "keys" : "tap";
        out_ << ms << " " << dev << " " << eventTypeName(e.type) << " " << eventCodeName(e.type, e.code)
             << " " << e.value << "\n";
    }
}

void Replayer::printVib(int64_t ms, const std::vector<VibCommand>& cmds) {
    std::string src = spec_.rumbleSource >= 0 ? spec_.sources[spec_.rumbleSource].id : "-";
    for (const VibCommand& c : cmds) {
        if (c.on) {
            out_ << ms << " vib " << src << " on strong=" << hex(c.strong, 4) << " weak=" << hex(c.weak, 4)
                 << " for=" << c.durationMs << "\n";
        } else {
            out_ << ms << " vib " << src << " off\n";
        }
    }
}

void Replayer::drainRumble(int64_t uptoMs) {
    for (int guard = 0; guard < 10000; ++guard) {
        int64_t d = rumble_.nextDeadlineNs();
        if (d < 0 || d > uptoMs * kMs) return;
        std::vector<VibCommand> cmds;
        rumble_.tick(d, &cmds);
        printVib(d / kMs, cmds);
    }
}

bool Replayer::timed(int64_t ms, const std::vector<std::string>& t, std::string* err) {
    std::vector<OutEvent> evs;
    if (t.empty()) {
        *err = "nothing after the time";
        return false;
    }
    if (t[0] == "intercept" && t.size() == 2 && (t[1] == "1" || t[1] == "0")) {
        bool on = t[1] == "1";
        out_ << ms << " intercept " << (on ? "on" : "off") << "\n";
        merger_->setIntercept(on, &evs);
        print(ms, evs);
        return true;
    }
    if (t[0] == "sync" && t.size() >= 2) {
        int s = sourceOf(t[1], ms);
        std::set<uint16_t> keys;
        std::map<uint16_t, int32_t> abs;
        for (size_t i = 2; i < t.size(); ++i) {
            size_t eq = t[i].find('=');
            if (eq == std::string::npos) {
                int code = eventCodeByName(EV_KEY, t[i]);
                if (code < 0) {
                    *err = "sync: unknown key '" + t[i] + "'";
                    return false;
                }
                keys.insert(static_cast<uint16_t>(code));
            } else {
                int code = eventCodeByName(EV_ABS, t[i].substr(0, eq));
                int64_t v;
                if (code < 0 || !parseInt(t[i].substr(eq + 1), &v)) {
                    *err = "sync: bad '" + t[i] + "'";
                    return false;
                }
                abs[static_cast<uint16_t>(code)] = static_cast<int32_t>(v);
            }
        }
        if (s >= 0) merger_->syncSource(s, keys, abs, &evs);
        print(ms, evs);
        return true;
    }
    if (t[0] == "gone" && t.size() == 2) {
        int s = sourceOf(t[1], ms);
        if (s >= 0) {
            merger_->dropSource(s, &evs);
            for (auto it = bound_.begin(); it != bound_.end();) {
                it = it->second == s ? bound_.erase(it) : std::next(it);
            }
            taken_[s] = false;
        }
        print(ms, evs);
        return true;
    }
    if (t[0] == "ff" && t.size() >= 3) {
        if (spec_.rumbleSource < 0) {
            *err = "ff: this pad has no rumble";
            return false;
        }
        std::vector<int64_t> n;
        for (size_t i = 2; i < t.size(); ++i) {
            int64_t v;
            if (!parseInt(t[i], &v)) {
                *err = "ff: bad number '" + t[i] + "'";
                return false;
            }
            n.push_back(v);
        }
        std::vector<VibCommand> cmds;
        int64_t now = ms * kMs;
        int rc = 0;
        if (t[1] == "upload" && (n.size() == 4 || n.size() == 5)) {
            RumbleEffect e;
            e.id = static_cast<int>(n[0]);
            e.strong = static_cast<uint16_t>(n[1]);
            e.weak = static_cast<uint16_t>(n[2]);
            e.lengthMs = static_cast<uint32_t>(n[3]);
            e.delayMs = n.size() == 5 ? static_cast<uint32_t>(n[4]) : 0;
            rc = rumble_.upload(e, now, &cmds);
        } else if (t[1] == "play" && n.size() == 2) {
            rumble_.play(static_cast<int>(n[0]), static_cast<int>(n[1]), now, &cmds);
        } else if (t[1] == "stop" && n.size() == 1) {
            rumble_.play(static_cast<int>(n[0]), 0, now, &cmds);
        } else if (t[1] == "erase" && n.size() == 1) {
            rc = rumble_.erase(static_cast<int>(n[0]), now, &cmds);
        } else if (t[1] == "gain" && n.size() == 1) {
            rumble_.setGain(static_cast<uint16_t>(n[0]), now, &cmds);
        } else {
            *err = "ff: unknown form";
            return false;
        }
        if (rc != 0) out_ << ms << " ff " << t[1] << " " << n[0] << " -> " << rc << "\n";
        printVib(ms, cmds);
        return true;
    }
    if (t.size() == 4) {
        int type = eventTypeByName(t[1]);
        int code = type < 0 ? -1 : eventCodeByName(type, t[2]);
        int64_t value;
        if (type < 0 || code < 0 || !parseInt(t[3], &value)) {
            *err = "bad event '" + t[1] + " " + t[2] + " " + t[3] + "'";
            return false;
        }
        int s = sourceOf(t[0], ms);
        if (s >= 0) {
            SourceEvent e;
            e.source = s;
            e.type = static_cast<uint16_t>(type);
            e.code = static_cast<uint16_t>(code);
            e.value = static_cast<int32_t>(value);
            e.timeNs = ms * kMs;
            merger_->process(e, &evs);
            if (merger_->needsSync(s)) out_ << ms << " needs-sync " << t[0] << "\n";
        }
        print(ms, evs);
        return true;
    }
    *err = "unknown statement '" + t[0] + "'";
    return false;
}

bool Replayer::finish(std::string* err) {
    if (!started_ && !start(err)) return false;
    drainRumble(INT64_MAX / kMs);
    return true;
}

}  // namespace

bool runReplay(std::istream& trace, const std::string& traceDir, std::ostream& out, std::string* err,
               const std::string& deviceOverride) {
    Replayer r(traceDir.empty() ? "." : traceDir, out, deviceOverride);
    std::string text;
    int n = 0;
    std::string why;
    while (std::getline(trace, text)) {
        ++n;
        if (!r.line(text, &why)) {
            if (err) *err = "line " + std::to_string(n) + ": " + why;
            return false;
        }
    }
    if (!r.finish(&why)) {
        if (err) *err = "end: " + why;
        return false;
    }
    return true;
}

}  // namespace padd
