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

#include "NanoMonitorMode.h"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace android {
namespace monmode {

namespace {

// A whole number of 1 to 5 digits, as a width or height; -1 for anything else.
int dimension(const std::string& s) {
    if (s.empty() || s.size() > 5) return -1;
    for (char c : s)
        if (c < '0' || c > '9') return -1;
    return atoi(s.c_str());
}

// A rate as the framework writes it: digits, at most one dot, a digit on each side of it.
bool rateText(const std::string& s) {
    if (s.empty() || s.size() > 8) return false;
    int dots = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '.') {
            if (++dots > 1 || i == 0 || i + 1 == s.size()) return false;
        } else if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

// The words of a line, split on spaces.
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

}  // namespace

float Mode::hz() const {
    return rate.empty() ? 0.0f : strtof(rate.c_str(), nullptr);
}

std::string Mode::text() const {
    if (!valid()) return std::string();
    return std::to_string(width) + "x" + std::to_string(height) + "@" + rate;
}

bool parseMode(const std::string& value, Mode* out) {
    const size_t x = value.find('x'), at = value.find('@');
    if (x == std::string::npos || at == std::string::npos || at < x) return false;
    const int w = dimension(value.substr(0, x)), h = dimension(value.substr(x + 1, at - x - 1));
    const std::string r = value.substr(at + 1);
    if (w <= 0 || h <= 0 || !rateText(r) || strtof(r.c_str(), nullptr) < 1.0f) return false;
    out->width = w;
    out->height = h;
    out->rate = r;
    return true;
}

bool sameSize(const Mode& a, const Mode& b) {
    return a.valid() && b.valid() && a.width == b.width && a.height == b.height;
}

bool sameMode(const Mode& a, const Mode& b) {
    return sameSize(a, b) && std::fabs(a.hz() - b.hz()) < 0.5f;
}

bool parseProp(const std::string& value, Mode* held, std::string* why) {
    const size_t semi = value.find(';');
    if (semi == std::string::npos) return false;
    Mode m;
    if (!parseMode(value.substr(0, semi), &m)) return false;
    *held = m;
    *why = value.substr(semi + 1);
    return true;
}

bool parseList(const std::string& text, List* out) {
    List l;
    bool held = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> w = words(line);
        if (w.size() < 2) continue;
        Mode m;
        const bool isMode = parseMode(w[1], &m);
        if (w[0] == "held" && w.size() >= 3) {
            held = true;
            if (isMode) l.held = m;
            l.why = w[2];
        } else if (w[0] == "active" && isMode) {
            l.active = m;
        } else if (w[0] == "auto" && isMode) {
            l.autoMode = m;
        } else if (w[0] == "stored" && isMode) {
            l.stored = m;
        } else if (w[0] == "mode" && isMode) {
            l.modes.push_back(m);
        }
    }
    if (!held) return false;
    *out = l;
    return true;
}

bool parseTrial(const std::string& text, Trial* out) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> w = words(line);
        if (w.size() != 4 || w[0] != "trial") continue;
        Trial t;
        const int secs = dimension(w[2]);
        if (!parseMode(w[1], &t.mode) || secs < 0 || !parseMode(w[3], &t.revert)) continue;
        t.seconds = secs;
        *out = t;
        return true;
    }
    return false;
}

std::vector<Mode> offered(const std::vector<Mode>& modes, bool dualScreen) {
    std::vector<Mode> out;
    for (const Mode& m : modes)
        if (m.valid() && (!dualScreen || m.hz() >= kDualMinHz - 0.5f)) out.push_back(m);
    return out;
}

std::vector<Mode> sizeChoices(const std::vector<Mode>& modes, const Mode& current) {
    std::vector<Mode> out;
    for (const Mode& m : modes) {
        bool seen = false;
        for (const Mode& o : out) seen = seen || sameSize(o, m);
        if (seen) continue;
        // This size's mode: the current rate if it has it, else the rate nearest 60 Hz.
        const Mode* pick = nullptr;
        for (const Mode& c : modes) {
            if (!sameSize(c, m)) continue;
            if (current.valid() && std::fabs(c.hz() - current.hz()) < 0.5f) { pick = &c; break; }
            const float d = std::fabs(c.hz() - 60.0f);
            const float best = pick ? std::fabs(pick->hz() - 60.0f) : 1e9f;
            if (d < best - 0.001f || (std::fabs(d - best) <= 0.001f && pick && c.hz() > pick->hz())) pick = &c;
        }
        out.push_back(*pick);
    }
    return out;
}

std::vector<Mode> rateChoices(const std::vector<Mode>& modes, const Mode& current) {
    std::vector<Mode> out;
    for (const Mode& m : modes)
        if (sameSize(m, current)) out.push_back(m);
    return out;
}

int indexOf(const std::vector<Mode>& choices, const Mode& current, bool sizesOnly) {
    for (size_t i = 0; i < choices.size(); i++)
        if (sizesOnly ? sameSize(choices[i], current) : sameMode(choices[i], current)) return (int)i;
    return -1;
}

std::string sizeText(const Mode& m) {
    return std::to_string(m.width) + " x " + std::to_string(m.height);
}

}  // namespace monmode
}  // namespace android
