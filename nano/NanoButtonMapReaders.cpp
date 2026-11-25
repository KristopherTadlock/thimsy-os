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

// Each reader follows its emulator's source at the version Thimsy ships; the file:line references
// are in button_map_overlay_plan.md §3.

#include "NanoButtonMapReaders.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/md5.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "NanoAppFiles.h"
#include "NanoBiosUtil.h"
#include "NanoJson.h"

namespace android {
namespace bmap {

namespace {

constexpr size_t kMaxLines = 20000;   // per config file: more is not an emulator's file

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep, size_t maxParts = 4096) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size() && out.size() < maxParts) {
        size_t e = s.find(sep, i);
        if (e == std::string::npos) e = s.size();
        out.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return out;
}

bool parseInt(const std::string& s, int* out) {
    const std::string t = trim(s);
    if (t.empty() || t.size() > 11) return false;
    char* end = nullptr;
    const long v = strtol(t.c_str(), &end, 10);
    if (!end || *end != '\0' || v < -2147483647L || v > 2147483647L) return false;
    *out = (int)v;
    return true;
}

// Device paths as root reads them: shared storage and a card at their raw folders (NanoMenu's
// rawStoragePath), /data/user/0 as /data/data. Any other /storage path gives "" (nothing read).
std::string rawPath(const std::string& p) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"/storage/emulated/0", "/data/media/0"}, {"/sdcard", "/data/media/0"},
        {"/storage/self/primary", "/data/media/0"}, {"/data/user/0", "/data/data"}};
    for (const auto& m : kMap) {
        const size_t n = strlen(m.first);
        if (p.compare(0, n, m.first) == 0 && (p.size() == n || p[n] == '/')) return m.second + p.substr(n);
    }
    static const std::string kStorage = "/storage/";
    if (p.compare(0, kStorage.size(), kStorage) != 0) return p;
    // A card's volume: vold names it by the filesystem's serial or UUID ("1234-ABCD"), or by the
    // volume's id ("public:179,1") when it has none.
    const size_t end = std::min(p.find('/', kStorage.size()), p.size());
    const std::string vol = p.substr(kStorage.size(), end - kStorage.size());
    const bool card = !vol.empty() && vol.size() <= 64 && vol != "emulated" && vol != "self"
                      && std::all_of(vol.begin(), vol.end(), [](char c) {
                             return isalnum((unsigned char)c) || c == '-' || c == ':' || c == ',';
                         });
    return card ? "/mnt/media_rw/" + p.substr(kStorage.size()) : std::string();
}

// rawPath under the test root; "" (which no read accepts) for a path rawPath refuses.
std::string fullPath(const ReaderEnv& env, const std::string& p) {
    const std::string raw = rawPath(p);
    return raw.empty() ? std::string() : env.root + raw;
}

std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string baseOf(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }

std::string stemOf(const std::string& p) {
    std::string b = baseOf(p);
    const size_t dot = b.find_last_of('.');
    if (dot != std::string::npos && dot > 0) b.resize(dot);
    return b;
}

// Directory names a file may be looked up under: an app could name a file "../x".
bool safeName(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find('/') == std::string::npos && n.size() < 256;
}

// Bytes [off, off+n) of a large file (a disc image), no link followed at the leaf.
bool readAt(const ReaderEnv& env, const std::string& devicePath, uint64_t off, size_t n, std::string* out) {
    const std::string full = fullPath(env, devicePath);
    if (!nanobios::safePath(full)) return false;
    const int fd = open(full.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && off + n <= (uint64_t)st.st_size;
    if (ok) {
        out->resize(n);
        size_t got = 0;
        while (got < n) {
            const ssize_t r = pread(fd, &(*out)[got], n - got, (off_t)(off + got));
            if (r <= 0) break;
            got += (size_t)r;
        }
        ok = got == n;
    }
    close(fd);
    return ok;
}

// ---- SharedPreferences XML ----

struct Prefs {
    std::map<std::string, std::string> values;               // int, long, boolean, string, float
    std::map<std::string, std::vector<std::string>> sets;    // <set>
    bool has(const std::string& k) const { return values.count(k) || sets.count(k); }
    std::string get(const std::string& k, const std::string& def = std::string()) const {
        auto it = values.find(k);
        return it == values.end() ? def : it->second;
    }
    int getInt(const std::string& k, int def) const {
        int v;
        return parseInt(get(k), &v) ? v : def;
    }
};

std::string xmlDecode(const std::string& v) {
    static const std::pair<const char*, char> kEnt[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size();) {
        bool hit = false;
        if (v[i] == '&') {
            for (const auto& e : kEnt) {
                const size_t n = strlen(e.first);
                if (v.compare(i, n, e.first) == 0) { out += e.second; i += n; hit = true; break; }
            }
        }
        if (!hit) out += v[i++];
    }
    return out;
}

std::string xmlAttr(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    const size_t a = tag.find(key);
    if (a == std::string::npos) return std::string();
    const size_t b = tag.find('"', a + key.size());
    if (b == std::string::npos) return std::string();
    return xmlDecode(tag.substr(a + key.size(), b - a - key.size()));
}

Prefs parsePrefs(const std::string& xml) {
    Prefs p;
    size_t i = 0;
    size_t count = 0;
    while ((i = xml.find('<', i)) != std::string::npos && count++ < 100000) {
        const size_t e = xml.find('>', i);
        if (e == std::string::npos) break;
        const std::string tag = xml.substr(i, e - i + 1);
        i = e + 1;
        if (tag.size() < 3 || tag[1] == '/' || tag[1] == '?') continue;
        const size_t sp = tag.find_first_of(" />", 1);
        const std::string type = tag.substr(1, sp - 1);
        const std::string name = xmlAttr(tag, "name");
        if (name.empty()) continue;
        if (type == "int" || type == "long" || type == "boolean" || type == "float") {
            p.values[name] = xmlAttr(tag, "value");
        } else if (type == "string") {
            if (tag.size() >= 2 && tag[tag.size() - 2] == '/') { p.values[name] = std::string(); continue; }
            const size_t close = xml.find("</string>", i);
            if (close == std::string::npos) break;
            p.values[name] = xmlDecode(xml.substr(i, close - i));
            i = close + 9;
        } else if (type == "set") {
            std::vector<std::string>& s = p.sets[name];
            if (tag.size() >= 2 && tag[tag.size() - 2] == '/') continue;
            const size_t close = xml.find("</set>", i);
            if (close == std::string::npos) break;
            const std::string body = xml.substr(i, close - i);
            size_t j = 0;
            while ((j = body.find("<string>", j)) != std::string::npos && s.size() < 256) {
                const size_t k = body.find("</string>", j);
                if (k == std::string::npos) break;
                s.push_back(xmlDecode(body.substr(j + 8, k - j - 8)));
                j = k + 9;
            }
            i = close + 6;
        }
    }
    return p;
}

// ---- RetroArch-style key = "value" ----

std::map<std::string, std::string> parseCfg(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t n = 0;
    for (const std::string& raw : split(text, '\n', kMaxLines)) {
        if (++n > kMaxLines) break;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"') {
            const size_t q = v.find('"', 1);
            v = q == std::string::npos ? v.substr(1) : v.substr(1, q - 1);
        }
        if (!k.empty()) out[k] = v;
    }
    return out;
}

// ---- inputs ----

Input hatDir(const std::string& d) {
    if (d == "up") return Input::axis(aaxis::kHatY, -1);
    if (d == "down") return Input::axis(aaxis::kHatY, 1);
    if (d == "left") return Input::axis(aaxis::kHatX, -1);
    if (d == "right") return Input::axis(aaxis::kHatX, 1);
    return Input();
}

void addIn(std::vector<Input>* v, const Input& in) {
    if (in.none()) return;
    if (std::find(v->begin(), v->end(), in) == v->end()) v->push_back(in);
}

EmuBind* bindFor(std::vector<EmuBind>* binds, const std::string& id, const std::string& name = std::string()) {
    for (auto& b : *binds)
        if (b.id == id && b.name == name) return &b;
    binds->push_back(EmuBind());
    binds->back().id = id;
    binds->back().name = name;
    return &binds->back();
}

const char* const kDirs[4] = {"up", "down", "left", "right"};

}  // namespace

// ---- shared pieces ----

bool readAppFile(const ReaderEnv& env, const std::string& devicePath, std::string* out, size_t maxBytes) {
    const std::string full = fullPath(env, devicePath);
    if (!nanobios::safePath(full) || nanobios::appOwnerUid(full, env.root) == -2) return false;
    int dfd;
    if (nanobios::openDir(dirOf(full), &dfd, nullptr, std::string(), nullptr, env.root) != 0) return false;
    const int fd = openat(dfd, baseOf(full).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dfd);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size <= maxBytes;
    if (ok) {
        out->resize((size_t)st.st_size);
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t n = read(fd, &(*out)[got], out->size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        ok = got == out->size();
    }
    close(fd);
    return ok;
}

namespace {

// Where Move Emulator Files to microSD puts an emulator's folders: /storage/<card>/emulator_config/...
bool cardEmulatorFolder(const std::string& p) {
    if (p.empty() || p[0] != '/' || !nanobios::safePath(p) || rawPath(p).empty()) return false;
    const std::vector<std::string> parts = split(p.substr(1), '/', 64);
    if (parts.size() < 4) return false;
    for (const auto& part : parts) if (part.empty()) return false;
    return parts[0] == "storage" && parts[1] != "emulated" && parts[1] != "self" && parts[2] == "emulator_config";
}

}  // namespace

std::string throughMoveLinks(const ReaderEnv& env, const std::string& devicePath) {
    std::string p = devicePath;
    for (int hops = 0; hops < 4; hops++) {
        const std::string raw = rawPath(p);
        if (raw.empty() || !nanobios::safePath(raw)) return std::string();
        bool linked = false;
        // Each folder on the way, from the top; the file itself is never followed (readAppFile).
        for (size_t at = raw.find('/', 1); at != std::string::npos; at = raw.find('/', at + 1)) {
            const std::string dir = env.root + raw.substr(0, at);
            struct stat st;
            if (lstat(dir.c_str(), &st) != 0) return p;   // not there: the read finds nothing
            if (!S_ISLNK(st.st_mode)) continue;
            char buf[PATH_MAX];
            const ssize_t n = readlink(dir.c_str(), buf, sizeof(buf) - 1);
            if (n <= 0) return std::string();
            std::string target(buf, (size_t)n);
            while (target.size() > 1 && target.back() == '/') target.pop_back();
            if (!cardEmulatorFolder(target)) return std::string();
            p = target + raw.substr(at);
            linked = true;
            break;
        }
        if (!linked) return p;
    }
    return std::string();
}

bool readMovedAppFile(const ReaderEnv& env, const std::string& devicePath, std::string* out, size_t maxBytes) {
    const std::string p = throughMoveLinks(env, devicePath);
    return !p.empty() && readAppFile(env, p, out, maxBytes);
}

std::string cfgValue(const std::string& text, const std::string& key, bool* found) {
    const auto m = parseCfg(text);
    auto it = m.find(key);
    if (found) *found = it != m.end();
    return it == m.end() ? std::string() : it->second;
}

namespace {

// [section] -> key -> value. Keys are case-sensitive; a repeated key keeps the last.
using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini parseIni(const std::string& text) {
    Ini ini;
    std::string sec;
    ini[sec];
    size_t n = 0;
    for (const std::string& raw : split(text, '\n', kMaxLines)) {
        if (++n > kMaxLines) break;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t e = line.find(']');
            sec = e == std::string::npos ? line.substr(1) : line.substr(1, e - 1);
            ini[sec];
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ini[sec][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return ini;
}

}  // namespace

std::string iniValue(const std::string& text, const std::string& section, const std::string& key, bool* found) {
    const Ini ini = parseIni(text);
    auto s = ini.find(section);
    if (s != ini.end()) {
        auto k = s->second.find(key);
        if (k != s->second.end()) {
            if (found) *found = true;
            return k->second;
        }
    }
    if (found) *found = false;
    return std::string();
}

bool iniHasSection(const std::string& text, const std::string& section) {
    const Ini ini = parseIni(text);
    return ini.count(section) > 0;
}

std::string pspDiscId(const ReaderEnv& env, const std::string& devicePath) {
    std::string head;
    if (!readAt(env, devicePath, 0, 40, &head)) return std::string();
    auto u32 = [](const std::string& b, size_t o) {
        return o + 4 <= b.size() ? (uint32_t)(uint8_t)b[o] | (uint32_t)(uint8_t)b[o + 1] << 8 |
                                       (uint32_t)(uint8_t)b[o + 2] << 16 | (uint32_t)(uint8_t)b[o + 3] << 24
                                 : 0u;
    };
    auto u16 = [](const std::string& b, size_t o) {
        return o + 2 <= b.size() ? (uint32_t)(uint8_t)b[o] | (uint32_t)(uint8_t)b[o + 1] << 8 : 0u;
    };
    auto clean = [](std::string id) {
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        for (char c : id) if (!isalnum((unsigned char)c)) return std::string();
        return id.size() >= 4 && id.size() <= 12 ? id : std::string();
    };
    if (head.compare(0, 4, std::string("\0PBP", 4)) == 0) {
        const uint32_t sfo = u32(head, 8), end = u32(head, 12);
        if (end <= sfo || end - sfo > 65536) return std::string();
        std::string s;
        if (!readAt(env, devicePath, sfo, end - sfo, &s) || s.compare(0, 4, std::string("\0PSF", 4)) != 0) return std::string();
        const uint32_t keys = u32(s, 8), data = u32(s, 12), count = u32(s, 16);
        for (uint32_t i = 0; i < count && i < 256; i++) {
            const size_t e = 20 + i * 16;
            if (e + 16 > s.size()) break;
            const size_t ko = keys + u16(s, e), dl = u32(s, e + 4), dof = data + u32(s, e + 12);
            if (ko >= s.size() || dof + dl > s.size()) break;
            if (strncmp(s.c_str() + ko, "DISC_ID", s.size() - ko) == 0)
                return clean(std::string(s.c_str() + dof, strnlen(s.c_str() + dof, dl)));
        }
        return std::string();
    }
    // ISO 9660: the root folder's UMD_DATA.BIN starts "ULUS-10041|".
    std::string pvd;
    if (!readAt(env, devicePath, 16 * 2048, 2048, &pvd) || pvd.compare(1, 5, "CD001") != 0) return std::string();
    const uint32_t lba = u32(pvd, 156 + 2), len = std::min<uint32_t>(u32(pvd, 156 + 10), 65536);
    std::string dir;
    if (!readAt(env, devicePath, (uint64_t)lba * 2048, len, &dir)) return std::string();
    for (size_t o = 0; o + 33 < dir.size();) {
        const uint8_t rl = (uint8_t)dir[o];
        if (rl == 0) { o = (o / 2048 + 1) * 2048; continue; }
        const uint8_t nl = (uint8_t)dir[o + 32];
        if (o + 33 + nl <= dir.size() && nl >= 12 && dir.compare(o + 33, 12, "UMD_DATA.BIN") == 0) {
            std::string id;
            if (!readAt(env, devicePath, (uint64_t)u32(dir, o + 2) * 2048, 10, &id)) return std::string();
            return clean(id);
        }
        o += rl;
    }
    return std::string();
}

// ============================================================================================
// RetroArch
// ============================================================================================

namespace {

constexpr const char* kRaCfg = "/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg";
constexpr const char* kRaCfgDir = "/storage/emulated/0/RetroArch/config";
constexpr const char* kRaAutoconfig = "/data/user/0/com.retroarch.aarch64/autoconfig";

// RetroPad ids in RetroArch's order (input_remap_ids), and the remap files' names for them.
const char* const kRaSlots[24] = {
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "l2", "r2", "l3", "r3",
    "l_x_plus", "l_x_minus", "l_y_plus", "l_y_minus", "r_x_plus", "r_x_minus", "r_y_plus", "r_y_minus"};
const char* const kRaRemapNames[24] = {
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "l2", "r2", "l3", "r3",
    "l_x+", "l_x-", "l_y+", "l_y-", "r_x+", "r_x-", "r_y+", "r_y-"};

Input raButton(const std::string& v) {
    if (v.empty() || v == "nul") return Input();
    if (v.size() > 2 && v[0] == 'h' && isdigit((unsigned char)v[1])) {
        size_t i = 1;
        while (i < v.size() && isdigit((unsigned char)v[i])) i++;
        if (v.substr(1, i - 1) != "0") return Input();
        return hatDir(v.substr(i));
    }
    int k;
    return parseInt(v, &k) && k > 0 && k < 1024 ? Input::key(k) : Input();
}

Input raAxis(const std::string& v) {
    if (v.size() < 2 || (v[0] != '+' && v[0] != '-')) return Input();
    int n;
    if (!parseInt(v.substr(1), &n)) return Input();
    static const int kAxes[10] = {aaxis::kX, aaxis::kY, aaxis::kZ, aaxis::kRZ, -1, -1,
                                  aaxis::kLTrigger, aaxis::kRTrigger, aaxis::kBrake, aaxis::kGas};
    if (n < 0 || n > 9 || kAxes[n] < 0) return Input();
    return Input::axis(kAxes[n], v[0] == '+' ? 1 : -1);
}

using Cfg = std::map<std::string, std::string>;

std::string get(const Cfg& c, const std::string& k, const std::string& def = std::string()) {
    auto it = c.find(k);
    return it == c.end() ? def : it->second;
}

bool overlay(const ReaderEnv& env, const std::string& path, Cfg* into) {
    std::string t;
    if (!readAppFile(env, path, &t, 1u << 20)) return false;
    for (const auto& kv : parseCfg(t)) (*into)[kv.first] = kv.second;
    return true;
}

// The autoconfig profile RetroArch picks for the pad (task_autodetect.c): +30 for VID and PID,
// +20 for the exact device name; the android/ folder first, the highest score wins.
Cfg raAutoconfig(const ReaderEnv& env, const std::string& dir, std::string* picked) {
    Cfg best;
    int bestScore = 0;
    for (const std::string& sub : {std::string("/android"), std::string()}) {
        const std::string full = fullPath(env, dir + sub);
        int pfd;
        if (nanobios::appOwnerUid(full, env.root) == -2 ||
            nanobios::openDir(full, &pfd, nullptr, std::string(), nullptr, env.root) != 0)
            continue;
        const int dfd = openat(pfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        close(pfd);
        DIR* d = dfd < 0 ? nullptr : fdopendir(dfd);
        if (!d) {
            if (dfd >= 0) close(dfd);
            continue;
        }
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".cfg") == 0 && safeName(n)) names.push_back(n);
            if (names.size() > 2000) break;
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            std::string t;
            if (!readAppFile(env, dir + sub + "/" + n, &t, 64u << 10)) continue;
            Cfg c = parseCfg(t);
            int score = 0, vid = -1, pid = -1;
            parseInt(get(c, "input_vendor_id"), &vid);
            parseInt(get(c, "input_product_id"), &pid);
            if (vid > 0 && vid == env.dev.vendor && pid == env.dev.product) score += 30;
            if (!env.dev.name.empty() && get(c, "input_device") == env.dev.name) score += 20;
            if (score > bestScore) {
                bestScore = score;
                best = std::move(c);
                if (picked) *picked = n;
            }
        }
    }
    return best;
}

}  // namespace

ReadResult readRetroArch(const ReaderEnv& env, const Context& ctx, const Catalog& cat) {
    ReadResult res;
    const CoreMap* cm = cat.core(ctx.core);
    const std::string lib = cm ? cm->library : ctx.core;
    res.emulatorName = "RetroArch" + (lib.empty() ? std::string() : " " + lib);

    Cfg cfg;
    const bool haveCfg = overlay(env, env.raConfig.empty() ? std::string(kRaCfg) : env.raConfig, &cfg);
    res.source = haveCfg ? Source::kConfig : Source::kDefaults;
    std::string cfgDir = get(cfg, "rgui_config_directory");
    if (cfgDir.empty() || cfgDir == "default") cfgDir = kRaCfgDir;
    const std::string romBase = ctx.romPath.empty() ? std::string() : stemOf(ctx.romPath);
    const std::string contentDir = ctx.romPath.empty() ? std::string() : baseOf(dirOf(ctx.romPath));

    // Overrides: core, then content folder, then game; later keys win.
    if (get(cfg, "auto_overrides_enable", "true") == "true" && safeName(lib)) {
        const std::string base = cfgDir + "/" + lib + "/";
        if (overlay(env, base + lib + ".cfg", &cfg)) res.notes.push_back("Core override: " + lib + ".cfg");
        if (safeName(contentDir) && overlay(env, base + contentDir + ".cfg", &cfg))
            res.notes.push_back("Folder override: " + contentDir + ".cfg");
        if (safeName(romBase) && overlay(env, base + romBase + ".cfg", &cfg))
            res.notes.push_back("Game override: " + romBase + ".cfg");
    }

    std::string acDir = get(cfg, "joypad_autoconfig_dir");
    if (acDir.empty() || acDir == "default") acDir = kRaAutoconfig;
    std::string acName;
    const Cfg ac = raAutoconfig(env, acDir, &acName);

    // Each RetroPad id: the player's bind unless it is nul or absent, then the pad's profile.
    std::vector<Input> phys[24];
    for (int s = 0; s < 24; s++) {
        const std::string slot = kRaSlots[s];
        std::string b = get(cfg, "input_player1_" + slot + "_btn");
        if (b.empty() || b == "nul") b = get(ac, "input_" + slot + "_btn");
        std::string a = get(cfg, "input_player1_" + slot + "_axis");
        if (a.empty() || a == "nul") a = get(ac, "input_" + slot + "_axis");
        addIn(&phys[s], raButton(b));
        addIn(&phys[s], raAxis(a));
    }

    // Remaps: the first of game, content folder, core; they don't stack.
    int remap[24];
    for (int s = 0; s < 24; s++) remap[s] = s;
    int device = 1;
    int dpadMode = 0;
    parseInt(get(cfg, "input_player1_analog_dpad_mode", "0"), &dpadMode);
    std::string remapDir = get(cfg, "input_remapping_directory");
    if (remapDir.empty() || remapDir == "default") remapDir = cfgDir + "/remaps";
    if (get(cfg, "auto_remaps_enable", "true") == "true" && safeName(lib)) {
        std::vector<std::pair<std::string, std::string>> tries;
        if (safeName(romBase)) tries.emplace_back(romBase, "Game remap: ");
        if (safeName(contentDir)) tries.emplace_back(contentDir, "Folder remap: ");
        tries.emplace_back(lib, "Core remap: ");
        for (const auto& t : tries) {
            std::string text;
            if (!readAppFile(env, remapDir + "/" + lib + "/" + t.first + ".rmp", &text, 256u << 10)) continue;
            const Cfg rmp = parseCfg(text);
            for (int s = 0; s < 24; s++) {
                const std::string key = std::string(s < 16 ? "input_player1_btn_" : "input_player1_stk_") + kRaRemapNames[s];
                int v;
                if (parseInt(get(rmp, key), &v)) remap[s] = v < 0 || v >= 24 ? -1 : v;
            }
            parseInt(get(rmp, "input_libretro_device_p1"), &device);
            parseInt(get(rmp, "input_player1_analog_dpad_mode", std::to_string(dpadMode)), &dpadMode);
            res.notes.push_back(t.second + t.first + ".rmp");
            break;
        }
    }

    // The console: the core's table, its variants for this device, system and options.
    std::map<std::string, std::string> map;
    std::map<std::string, std::string> notes;
    res.console = ctx.systemId;
    if (cm) {
        const std::string own = coreSystem(*cm, res.console);
        res.console = !own.empty() ? own : cm->systems.empty() ? ctx.systemId : cm->systems[0];
        map = cm->map;
        notes = cm->notes;
        res.drawing = cm->drawing;
        Cfg opts;
        if (!cm->optionKeys.empty() && safeName(lib)) {
            const std::string base = cfgDir + "/" + lib + "/";
            const bool perGame = get(cfg, "game_specific_options", "true") == "true";
            bool got = (perGame && safeName(romBase) && overlay(env, base + romBase + ".opt", &opts)) ||
                       (perGame && safeName(contentDir) && overlay(env, base + contentDir + ".opt", &opts));
            if (!got && get(cfg, "global_core_options", "false") != "true") got = overlay(env, base + lib + ".opt", &opts);
            if (!got) overlay(env, cfgDir + "/retroarch-core-options.cfg", &opts);
        }
        for (const auto& v : cm->variants) {
            bool on = true;
            if (v.device >= 0 && v.device != device) on = false;
            if (v.option == "@system") on = on && v.value == res.console;
            else if (!v.option.empty()) on = on && get(opts, v.option) == v.value;
            if (!on) continue;
            for (const auto& kv : v.map) map[kv.first] = kv.second;
            if (!v.drawing.empty()) res.drawing = v.drawing;
            if (!v.note.empty()) res.notes.push_back(v.note);
        }
        if (cm->perGame) res.notes.push_back("Per-game layout");
    } else {
        // An unknown core: the RetroPad itself, drawn as the generic pad.
        res.drawing = "modern";
        res.notes.push_back("RetroPad layout (core not in the table)");
        map = {{"b", "btn-a"}, {"a", "btn-b"}, {"y", "btn-x"}, {"x", "btn-y"}, {"l", "btn-lb"}, {"r", "btn-rb"},
               {"l2", "btn-lt"}, {"r2", "btn-rt"}, {"l3", "btn-l3"}, {"r3", "btn-r3"}, {"start", "btn-start"},
               {"select", "btn-back"}, {"lstick", "stick-l"}, {"rstick", "stick-r"}};
    }
    // Expand the d-pad and sticks.
    const std::string dpadTo = map.count("dpad") ? map["dpad"] : std::string("dpad");
    for (int d = 0; d < 4; d++) {
        const std::string slot = kDirs[d];
        if (map.count(slot)) continue;
        if (dpadTo == "dpad") map[slot] = std::string("dpad-") + kDirs[d];
        else if (!dpadTo.empty()) map[slot] = dpadTo + ":" + kDirs[d];
    }
    for (const char* stick : {"lstick", "rstick"}) {
        auto it = map.find(stick);
        if (it == map.end() || it->second.empty()) continue;
        const std::string p = stick[0] == 'l' ? "l_" : "r_";
        const std::string a = it->second;
        if (!map.count(p + "x+")) map[p + "x+"] = a + ":right";
        if (!map.count(p + "x-")) map[p + "x-"] = a + ":left";
        if (!map.count(p + "y+")) map[p + "y+"] = a + ":down";
        if (!map.count(p + "y-")) map[p + "y-"] = a + ":up";
    }
    // Left stick as D-pad, for a core that never reads a stick (it turns off once one does).
    const bool coreUsesLeftStick = map.count("l_x+") && !map["l_x+"].empty();
    if (dpadMode == 1 && !coreUsesLeftStick) {
        static const int kFrom[4] = {19, 18, 17, 16};   // l_y_minus, l_y_plus, l_x_minus, l_x_plus
        for (int d = 0; d < 4; d++)
            for (const auto& in : phys[kFrom[d]]) addIn(&phys[4 + d], in);
    }

    auto anchorLike = [](const std::string& v) {
        static const char* const kPre[] = {"btn-", "dpad", "stick-", "c-", "key-", "rpad", "touch"};
        for (const char* p : kPre) if (v.compare(0, strlen(p), p) == 0) return true;
        return false;
    };
    for (int s = 0; s < 24; s++) {
        const int t = remap[s];
        if (t < 0 || phys[s].empty()) continue;
        auto it = map.find(kRaRemapNames[t]);
        if (it == map.end() || it->second.empty()) continue;
        EmuBind* b = anchorLike(it->second) ? bindFor(&res.binds, it->second) : bindFor(&res.binds, "", it->second);
        for (const auto& in : phys[s]) addIn(&b->in, in);
        auto n = notes.find(kRaRemapNames[t]);
        if (n != notes.end()) b->note = n->second;
    }
    // A console button nothing presses still gets its row ("—").
    for (const auto& kv : map) {
        if (kv.second.empty() || kv.first == "dpad" || kv.first == "lstick" || kv.first == "rstick") continue;
        if (anchorLike(kv.second)) bindFor(&res.binds, kv.second);
        else bindFor(&res.binds, "", kv.second);
    }

    // RetroArch's own hotkeys, with the hotkey-enable button held first.
    const Input enable = raButton(get(cfg, "input_enable_hotkey_btn"));
    static const std::pair<const char*, const char*> kHot[] = {
        {"input_save_state_btn", "Save state"}, {"input_load_state_btn", "Load state"},
        {"input_toggle_fast_forward_btn", "Fast forward"}, {"input_hold_fast_forward_btn", "Fast forward (hold)"},
        {"input_rewind_btn", "Rewind"}, {"input_menu_toggle_btn", "RetroArch menu"},
        {"input_exit_emulator_btn", "Quit RetroArch"}, {"input_pause_toggle_btn", "Pause"},
        {"input_reset_btn", "Reset"}, {"input_state_slot_increase_btn", "Next state slot"},
        {"input_state_slot_decrease_btn", "Previous state slot"}};
    for (const auto& h : kHot) {
        const Input in = raButton(get(cfg, h.first));
        if (in.none()) continue;
        EmuHotkey hk;
        hk.action = h.second;
        if (!enable.none() && !(enable == in)) hk.chord.push_back(enable);
        hk.chord.push_back(in);
        res.hotkeys.push_back(hk);
    }
    int combo = 0;
    if (parseInt(get(cfg, "input_menu_toggle_gamepad_combo"), &combo) && combo > 0) {
        // input_driver.c's combos, as RetroPad ids pressed through the player's binds.
        static const std::vector<std::vector<int>> kCombos = {
            {}, {5, 1, 10, 11}, {14, 15}, {10, 11, 3, 2}, {3, 2}, {14, 11}, {10, 11}, {}, {}, {5, 2}, {12, 13}};
        if (combo < (int)kCombos.size() && !kCombos[combo].empty()) {
            EmuHotkey hk;
            hk.action = "RetroArch menu";
            for (int s : kCombos[combo]) {
                if (phys[s].empty()) { hk.chord.clear(); break; }
                hk.chord.push_back(phys[s][0]);
            }
            if (!hk.chord.empty()) res.hotkeys.push_back(hk);
        }
    }
    if (!acName.empty() && haveCfg) res.notes.push_back("Pad profile: " + acName);
    return res;
}

// ============================================================================================
// PPSSPP
// ============================================================================================

namespace {

// "10-96,10-4033:10-97": binds separated by ',', a chord by ':'. Only gamepad binds (device 10).
void ppssppBinds(const std::string& value, std::vector<Input>* any, std::vector<std::vector<Input>>* chords) {
    for (const std::string& bind : split(value, ',')) {
        std::vector<Input> chord;
        bool pad = true;
        for (const std::string& part : split(bind, ':')) {
            const size_t dash = part.find('-');
            int dev, code;
            if (dash == std::string::npos || !parseInt(part.substr(0, dash), &dev) || !parseInt(part.substr(dash + 1), &code) ||
                dev != 10) {
                pad = false;
                break;
            }
            if (code >= 4000 && code < 4128) chord.push_back(Input::axis((code - 4000) / 2, (code - 4000) % 2 ? -1 : 1));
            else if (code > 0 && code < 4000) chord.push_back(Input::key(code));
            else pad = false;
        }
        if (!pad || chord.empty()) continue;
        if (chord.size() == 1) addIn(any, chord[0]);
        else if (chords) chords->push_back(chord);
    }
}

const std::pair<const char*, const char*> kPspButtons[] = {
    {"Up", "dpad-up"}, {"Down", "dpad-down"}, {"Left", "dpad-left"}, {"Right", "dpad-right"},
    {"Circle", "btn-circle"}, {"Cross", "btn-cross"}, {"Square", "btn-square"}, {"Triangle", "btn-triangle"},
    {"Start", "btn-start"}, {"Select", "btn-select"}, {"L", "btn-l"}, {"R", "btn-r"},
    {"An.Up", "stick-l:up"}, {"An.Down", "stick-l:down"}, {"An.Left", "stick-l:left"}, {"An.Right", "stick-l:right"}};
const std::pair<const char*, const char*> kPspHotkeys[] = {
    {"Fast-forward", "Fast forward (hold)"}, {"SpeedToggle", "Fast forward"}, {"Pause", "Pause menu"},
    {"Save State", "Save state"}, {"Load State", "Load state"}, {"Rewind", "Rewind"}, {"Next Slot", "Next state slot"},
    {"Previous Slot", "Previous state slot"}, {"Reset", "Reset"}, {"Exit App", "Quit PPSSPP"}};

}  // namespace

ReadResult readPpsspp(const ReaderEnv& env, const Context& ctx) {
    ReadResult res;
    res.emulatorName = "PPSSPP";
    res.console = "psp";
    std::string mem = ppssppMemstickAt(env.root);
    if (mem.empty()) mem = "/storage/emulated/0/Android/data/org.ppsspp.ppsspp/files";
    const std::string sys = mem + "/PSP/SYSTEM/";
    // A game's own settings file replaces controls.ini, its [ControlMapping] or, without one, the
    // built-in defaults (KeyMap::LoadFromIni restores the defaults first).
    std::string text;
    bool have = false;
    if (!ctx.romPath.empty()) {
        const std::string id = pspDiscId(env, ctx.romPath);
        if (!id.empty() && readAppFile(env, sys + id + "_ppsspp.ini", &text, 1u << 20)) {
            have = true;
            res.notes.push_back("Game settings: " + id);
        }
    }
    if (!have) have = readAppFile(env, sys + "controls.ini", &text, 1u << 20);
    const Ini ini = have ? parseIni(text) : Ini();
    auto sec = ini.find("ControlMapping");
    if (sec == ini.end()) {
        res.source = Source::kDefaults;
        auto key = [&](const char* id, std::vector<Input> in) { bindFor(&res.binds, id)->in = std::move(in); };
        key("btn-cross", {Input::key(akey::kButtonA)});
        key("btn-circle", {Input::key(akey::kButtonB)});
        key("btn-square", {Input::key(akey::kButtonX)});
        key("btn-triangle", {Input::key(akey::kButtonY)});
        key("dpad-up", {Input::axis(aaxis::kHatY, -1), Input::key(akey::kDpadUp)});
        key("dpad-down", {Input::axis(aaxis::kHatY, 1), Input::key(akey::kDpadDown)});
        key("dpad-left", {Input::axis(aaxis::kHatX, -1), Input::key(akey::kDpadLeft)});
        key("dpad-right", {Input::axis(aaxis::kHatX, 1), Input::key(akey::kDpadRight)});
        key("btn-start", {Input::key(akey::kStart)});
        key("btn-select", {Input::key(akey::kSelect)});
        key("btn-l", {Input::key(akey::kL1)});
        key("btn-r", {Input::key(akey::kR1)});
        key("stick-l:up", {Input::axis(aaxis::kY, -1)});
        key("stick-l:down", {Input::axis(aaxis::kY, 1)});
        key("stick-l:left", {Input::axis(aaxis::kX, -1)});
        key("stick-l:right", {Input::axis(aaxis::kX, 1)});
        res.hotkeys.push_back({"Fast forward (hold)", {Input::key(akey::kR2)}});
        res.hotkeys.push_back({"Pause menu", {Input::key(akey::kL2)}});
        return res;
    }
    res.source = Source::kConfig;
    for (const auto& b : kPspButtons) {
        EmuBind* e = bindFor(&res.binds, b.second);
        auto it = sec->second.find(b.first);
        if (it == sec->second.end()) continue;   // a key the section lacks is unbound
        std::vector<std::vector<Input>> chords;
        ppssppBinds(it->second, &e->in, &chords);
        for (const auto& c : chords) e->chords.push_back(c);
    }
    for (const auto& h : kPspHotkeys) {
        auto it = sec->second.find(h.first);
        if (it == sec->second.end()) continue;
        std::vector<Input> any;
        std::vector<std::vector<Input>> chords;
        ppssppBinds(it->second, &any, &chords);
        for (const auto& in : any) res.hotkeys.push_back({h.second, {in}});
        for (const auto& c : chords) res.hotkeys.push_back({h.second, c});
    }
    return res;
}

// ============================================================================================
// Dolphin (partial)
// ============================================================================================

namespace {

// Android.cpp's input names for the keycodes a pad sends.
Input dolphinInput(std::string term) {
    term.erase(std::remove(term.begin(), term.end(), '`'), term.end());
    // A qualified input: "Android/0/Name:`Button A`" (the qualifier inside or outside the quotes).
    const size_t colon = term.rfind(':');
    if (colon != std::string::npos) term = term.substr(colon + 1);
    term = trim(term);
    static const std::pair<const char*, int> kNames[] = {
        {"Button A", 96}, {"Button B", 97}, {"Button C", 98}, {"Button X", 99}, {"Button Y", 100}, {"Button Z", 101},
        {"Button L1", 102}, {"Button R1", 103}, {"Button L2", 104}, {"Button R2", 105}, {"Button L3", 106},
        {"Button R3", 107}, {"Start", 108}, {"Select", 109}, {"Up", 19}, {"Down", 20}, {"Left", 21}, {"Right", 22}};
    for (const auto& n : kNames) if (term == n.first) return Input::key(n.second);
    if (term.compare(0, 5, "Axis ") == 0 && term.size() > 6) {
        const char sign = term.back();
        int a;
        if ((sign == '+' || sign == '-') && parseInt(term.substr(5, term.size() - 6), &a) && a >= 0 && a < 64)
            return Input::axis(a, sign == '+' ? 1 : -1);
    }
    return Input();
}

void dolphinExpr(const std::string& expr, EmuBind* b) {
    // Operators count only outside the quoted names ("`Axis 1+`"); '/' is left out because device
    // qualifiers ("Android/0/Name:`Button A`") use it.
    std::string bare;
    bool quoted = false;
    for (char c : expr) {
        if (c == '`') quoted = !quoted;
        else if (!quoted) bare += c;
    }
    if (bare.find_first_of("&!()+*^") != std::string::npos) {
        b->in.push_back(Input::other(expr));
        b->note = "advanced";
        return;
    }
    for (const std::string& t : split(expr, '|')) addIn(&b->in, dolphinInput(t));
}

std::string discGameId(const ReaderEnv& env, const std::string& path) {
    std::string h;
    if (!readAt(env, path, 0, 0x60, &h)) return std::string();
    size_t off = 0;
    if (h.compare(0, 4, "RVZ\x01") == 0 || h.compare(0, 4, "WIA\x01") == 0) off = 0x58;   // the disc header copy
    std::string id = h.substr(off, 6);
    for (char c : id) if (!isalnum((unsigned char)c)) return std::string();
    return id;
}

}  // namespace

ReadResult readDolphin(const ReaderEnv& env, const Context& ctx) {
    ReadResult res;
    res.emulatorName = "Dolphin";
    const bool wii = ctx.systemId == "wii";
    res.console = wii ? "wii" : "gc";
    std::string dir;
    std::string text;
    for (const char* d : {"/storage/emulated/0/Android/data/org.dolphinemu.dolphinemu/files/Config",
                          "/storage/emulated/0/dolphin-emu/Config"}) {
        std::string t;
        if (readMovedAppFile(env, std::string(d) + (wii ? "/WiimoteNew.ini" : "/GCPadNew.ini"), &t, 512u << 10)) {
            dir = d;
            text = t;
            break;
        }
    }
    std::map<std::string, std::string> section;
    if (!dir.empty()) {
        const Ini ini = parseIni(text);
        auto s = ini.find(wii ? "Wiimote1" : "GCPad1");
        if (s != ini.end()) section = s->second;
        // A game's profile replaces the section.
        const std::string id = ctx.romPath.empty() ? std::string() : discGameId(env, ctx.romPath);
        std::string gs;
        const std::string base = dir.substr(0, dir.rfind('/'));   // safePath refuses ".."
        if (!id.empty() && readMovedAppFile(env, base + "/GameSettings/" + id + ".ini", &gs, 256u << 10)) {
            const std::string prof = iniValue(gs, "Controls", wii ? "WiimoteProfile1" : "PadProfile1");
            std::string pt;
            if (safeName(prof) &&
                readMovedAppFile(env, dir + "/Profiles/" + (wii ? "Wiimote/" : "GCPad/") + prof + ".ini", &pt, 256u << 10)) {
                const Ini pi = parseIni(pt);
                auto p = pi.find("Profile");
                if (p != pi.end()) {
                    section = p->second;
                    res.notes.push_back("Game profile: " + prof);
                }
            }
        }
    }
    static const std::pair<const char*, const char*> kGc[] = {
        {"Buttons/A", "btn-a"}, {"Buttons/B", "btn-b"}, {"Buttons/X", "btn-x"}, {"Buttons/Y", "btn-y"},
        {"Buttons/Z", "btn-z"}, {"Buttons/Start", "btn-start"}, {"Triggers/L", "btn-l"}, {"Triggers/R", "btn-r"},
        {"Triggers/L-Analog", "btn-l"}, {"Triggers/R-Analog", "btn-r"},
        {"Main Stick/Up", "stick-l:up"}, {"Main Stick/Down", "stick-l:down"}, {"Main Stick/Left", "stick-l:left"},
        {"Main Stick/Right", "stick-l:right"}, {"C-Stick/Up", "stick-r:up"}, {"C-Stick/Down", "stick-r:down"},
        {"C-Stick/Left", "stick-r:left"}, {"C-Stick/Right", "stick-r:right"},
        {"D-Pad/Up", "dpad-up"}, {"D-Pad/Down", "dpad-down"}, {"D-Pad/Left", "dpad-left"}, {"D-Pad/Right", "dpad-right"}};
    static const std::pair<const char*, const char*> kWii[] = {
        {"Buttons/A", "btn-a"}, {"Buttons/B", "btn-b"}, {"Buttons/1", "btn-1"}, {"Buttons/2", "btn-2"},
        {"Buttons/-", "btn-minus"}, {"Buttons/+", "btn-plus"}, {"Buttons/Home", "btn-home"},
        {"D-Pad/Up", "dpad-up"}, {"D-Pad/Down", "dpad-down"}, {"D-Pad/Left", "dpad-left"}, {"D-Pad/Right", "dpad-right"},
        {"Nunchuk/Buttons/C", "btn-c"}, {"Nunchuk/Buttons/Z", "btn-z"}, {"Nunchuk/Stick/Up", "stick-l:up"},
        {"Nunchuk/Stick/Down", "stick-l:down"}, {"Nunchuk/Stick/Left", "stick-l:left"},
        {"Nunchuk/Stick/Right", "stick-l:right"}};
    bool any = false;
    const auto* table = wii ? kWii : kGc;
    const size_t n = wii ? sizeof(kWii) / sizeof(kWii[0]) : sizeof(kGc) / sizeof(kGc[0]);
    for (size_t i = 0; i < n; i++) {
        EmuBind* b = bindFor(&res.binds, table[i].second);
        auto it = section.find(table[i].first);
        if (it == section.end() || trim(it->second).empty()) continue;
        dolphinExpr(it->second, b);
        any = any || !b->in.empty();
    }
    if (!any) {
        // Dolphin on Android binds no pad buttons by default (GCPadEmu.cpp LoadDefaults).
        res.source = Source::kNotMapped;
        res.notMappedWhere = wii ? "Dolphin > Settings > Wii Input" : "Dolphin > Settings > GameCube Input";
        return res;
    }
    res.source = Source::kConfig;
    // Device = Android/<id>/<name>: binds made for another pad don't apply to this one.
    const std::string device = section.count("Device") ? section["Device"] : std::string();
    const size_t slash = device.find('/', device.find('/') + 1);
    const std::string devName = slash == std::string::npos ? std::string() : device.substr(slash + 1);
    if (!devName.empty() && !env.dev.name.empty() && devName != env.dev.name) {
        res.notes.push_back("Bound to " + devName);
        for (auto& b : res.binds)
            if (!b.in.empty()) b.in = {Input::other(devName)};
    }
    if (wii && section.count("Extension") && section["Extension"] == "Classic")
        res.notes.push_back("Classic Controller binds not shown");
    return res;
}

// ============================================================================================
// Azahar
// ============================================================================================

ReadResult readAzahar(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "Azahar";
    res.console = "n3ds";
    std::string xml;
    const Prefs p = readAppFile(env, appdef::kAzaharPrefs, &xml, 1u << 20) ? parsePrefs(xml) : Prefs();
    static const std::pair<int, const char*> kButtons[] = {
        {700, "btn-a"}, {701, "btn-b"}, {702, "btn-x"}, {703, "btn-y"}, {704, "btn-start"}, {705, "btn-select"},
        {706, "btn-home"}, {707, "btn-zl"}, {708, "btn-zr"}, {709, "dpad-up"}, {710, "dpad-down"},
        {711, "dpad-left"}, {712, "dpad-right"}, {773, "btn-l"}, {774, "btn-r"}};
    auto buttonId = [&](int guest) -> const char* {
        for (const auto& b : kButtons) if (b.first == guest) return b.second;
        return nullptr;
    };
    for (const auto& b : kButtons) bindFor(&res.binds, b.second);
    for (const char* s : {"stick-l", "stick-r"})
        for (const char* d : kDirs) bindFor(&res.binds, std::string(s) + ":" + d);
    const std::string pre = "InputMapping_HostAxis_";
    bool any = false;
    std::set<std::string> names;
    for (const auto& kv : p.values) names.insert(kv.first);
    for (const auto& kv : p.sets) names.insert(kv.first);
    for (const std::string& name : names) {
        if (name.compare(0, pre.size(), pre) != 0) continue;
        const std::string rest = name.substr(pre.size());
        int host;
        if (parseInt(rest, &host)) {   // a button: keycode -> guest codes
            std::vector<int> guests;
            auto st = p.sets.find(name);
            if (st != p.sets.end()) {
                for (const auto& g : st->second) { int v; if (parseInt(g, &v)) guests.push_back(v); }
            } else {
                int v;
                if (parseInt(p.get(name), &v)) guests.push_back(v);
            }
            for (int g : guests)
                if (const char* id = buttonId(g)) { addIn(&bindFor(&res.binds, id)->in, Input::key(host)); any = true; }
            continue;
        }
        const std::string suffix = "_GuestButton";
        if (rest.size() <= suffix.size() || rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (!parseInt(rest.substr(0, rest.size() - suffix.size()), &host)) continue;
        const std::string axisPre = pre + std::to_string(host);
        const int guest = p.getInt(name, -1);
        const int orient = p.getInt(axisPre + "_GuestOrientation", 0);
        const bool inv = p.get(axisPre + "_Inverted") == "true";
        const int neg = inv ? 1 : -1, pos = -neg;
        if (guest == 713 || guest == 718 || guest == 780) {
            const std::string base = guest == 713 ? "stick-l:" : guest == 718 ? "stick-r:" : "dpad-";
            const std::string a = orient ? "up" : "left", b = orient ? "down" : "right";
            addIn(&bindFor(&res.binds, base + a)->in, Input::axis(host, neg));
            addIn(&bindFor(&res.binds, base + b)->in, Input::axis(host, pos));
            any = true;
        } else if (const char* id = buttonId(guest)) {
            addIn(&bindFor(&res.binds, id)->in, Input::axis(host, 1));
            any = true;
        }
    }
    if (!any) {
        res.source = Source::kNotMapped;
        res.notMappedWhere = "Azahar > Settings > Controls > Auto-map";
        return res;
    }
    res.source = Source::kConfig;
    return res;
}

// ============================================================================================
// Flycast
// ============================================================================================

namespace {

void flycastDefaults(ReadResult* res) {
    auto k = [&](const char* id, std::vector<Input> in) { bindFor(&res->binds, id)->in = std::move(in); };
    k("btn-a", {Input::key(akey::kButtonA)});
    k("btn-b", {Input::key(akey::kButtonB)});
    k("btn-x", {Input::key(akey::kButtonX)});
    k("btn-y", {Input::key(akey::kButtonY)});
    k("btn-start", {Input::key(akey::kStart)});
    k("dpad-up", {Input::key(akey::kDpadUp)});
    k("dpad-down", {Input::key(akey::kDpadDown)});
    k("dpad-left", {Input::key(akey::kDpadLeft)});
    k("dpad-right", {Input::key(akey::kDpadRight)});
    k("btn-l", {Input::axis(aaxis::kLTrigger, 1)});
    k("btn-r", {Input::axis(aaxis::kRTrigger, 1)});
    k("stick-l:up", {Input::axis(aaxis::kY, -1)});
    k("stick-l:down", {Input::axis(aaxis::kY, 1)});
    k("stick-l:left", {Input::axis(aaxis::kX, -1)});
    k("stick-l:right", {Input::axis(aaxis::kX, 1)});
    bindFor(&res->binds, "", "Z")->in = {Input::key(akey::kL1)};
    bindFor(&res->binds, "", "C")->in = {Input::key(akey::kR1)};
    res->hotkeys.push_back({"Flycast menu", {Input::key(akey::kSelect)}});
}

const char* flycastAction(const std::string& a, bool* hotkey) {
    static const std::pair<const char*, const char*> kAct[] = {
        {"btn_a", "btn-a"}, {"btn_b", "btn-b"}, {"btn_x", "btn-x"}, {"btn_y", "btn-y"}, {"btn_start", "btn-start"},
        {"btn_dpad1_up", "dpad-up"}, {"btn_dpad1_down", "dpad-down"}, {"btn_dpad1_left", "dpad-left"},
        {"btn_dpad1_right", "dpad-right"}, {"btn_trigger_left", "btn-l"}, {"btn_trigger_right", "btn-r"},
        {"axis_trigger_left", "btn-l"}, {"axis_trigger_right", "btn-r"}, {"btn_analog_up", "stick-l:up"},
        {"btn_analog_down", "stick-l:down"}, {"btn_analog_left", "stick-l:left"}, {"btn_analog_right", "stick-l:right"},
        {"axis_up", "stick-l:up"}, {"axis_down", "stick-l:down"}, {"axis_left", "stick-l:left"},
        {"axis_right", "stick-l:right"}};
    static const std::pair<const char*, const char*> kHot[] = {
        {"btn_menu", "Flycast menu"}, {"btn_fforward", "Fast forward"}, {"btn_quick_save", "Save state"},
        {"btn_jump_state", "Load state"}, {"btn_pause", "Pause"}, {"btn_escape", "Quit Flycast"}};
    *hotkey = false;
    for (const auto& k : kAct) if (a == k.first) return k.second;
    for (const auto& k : kHot) if (a == k.first) { *hotkey = true; return k.second; }
    return nullptr;
}

std::string flycastSanitize(std::string n) {
    for (char& c : n) if (strchr("/\\:?*|\"<>", c)) c = '-';
    return n;
}

}  // namespace

ReadResult readFlycast(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "Flycast";
    res.console = "dreamcast";
    std::string prefs;
    const std::string home = flycastHomeAt(env.root, &prefs);
    std::string mapDir = home + "/mappings";
    std::string emu;
    if (!home.empty() && readAppFile(env, home + "/emu.cfg", &emu, 256u << 10)) {
        const std::string mp = iniValue(emu, "config", "Dreamcast.MappingsPath");
        if (!mp.empty() && mp[0] == '/') mapDir = mp;
    }
    std::vector<std::string> names;
    if (!env.dev.name.empty()) {
        const std::string n = "Android_" + flycastSanitize(env.dev.name);
        if (!env.dev.descriptor.empty()) names.push_back(n + "-" + flycastSanitize(env.dev.descriptor) + ".cfg");
        names.push_back(n + ".cfg");
    }
    std::string text, used;
    for (const auto& n : names)
        if (safeName(n) && readAppFile(env, mapDir + "/" + n, &text, 256u << 10)) { used = n; break; }
    if (used.empty()) {
        res.source = Source::kDefaults;
        flycastDefaults(&res);
        return res;
    }
    const Ini ini = parseIni(text);
    int version = 1;
    auto em = ini.find("emulator");
    if (em != ini.end() && em->second.count("version")) parseInt(em->second.at("version"), &version);
    if (version < 3) {
        res.source = Source::kDefaults;
        res.notes.push_back("Older mapping file (" + used + "), defaults shown");
        flycastDefaults(&res);
        return res;
    }
    res.source = Source::kConfig;
    for (const char* id : {"btn-a", "btn-b", "btn-x", "btn-y", "btn-start", "dpad-up", "dpad-down", "dpad-left",
                           "dpad-right", "btn-l", "btn-r"})
        bindFor(&res.binds, id);
    for (const char* sec : {"digital", "analog"}) {
        auto s = ini.find(sec);
        if (s == ini.end()) continue;
        for (int i = 0; i < 512; i++) {
            auto it = s->second.find("bind" + std::to_string(i));
            if (it == s->second.end() || it->second.empty()) break;
            const std::string& v = it->second;
            const size_t colon = v.find(':');
            if (colon == std::string::npos || colon == 0) break;
            std::string action = v.substr(colon + 1);
            if (action.empty()) break;
            if (action.back() >= '1' && action.back() <= '3') continue;   // ports 2-4
            Input in;
            int code;
            if (sec[0] == 'd') {
                if (!parseInt(v.substr(0, colon), &code)) continue;
                in = Input::key(code);
            } else {
                if (colon < 2) break;
                if (!parseInt(v.substr(0, colon - 1), &code)) continue;
                in = Input::axis(code, v[colon - 1] == '+' ? 1 : -1);
            }
            bool hot;
            const char* id = flycastAction(action, &hot);
            if (!id) continue;
            if (hot) res.hotkeys.push_back({id, {in}});
            else addIn(&bindFor(&res.binds, id)->in, in);
        }
    }
    res.notes.push_back("Mapping: " + used);
    return res;
}

// ============================================================================================
// ARMSX2
// ============================================================================================

ReadResult readArmsx2(const ReaderEnv& env, const Context& ctx) {
    ReadResult res;
    res.emulatorName = "ARMSX2";
    res.console = "ps2";
    std::string xml;
    const bool have = readAppFile(env, appdef::kArmsx2Prefs, &xml, 2u << 20);
    const Prefs p = have ? parsePrefs(xml) : Prefs();
    res.source = have ? Source::kConfig : Source::kDefaults;
    const std::string game = ctx.romPath.empty() ? std::string() : "game." + stemOf(ctx.romPath) + ".";
    auto resolve = [&](const std::string& key, int def) {
        if (!game.empty() && p.has(game + key)) return p.getInt(game + key, def);
        return p.getInt(key, def);
    };
    static const struct { const char* id; const char* anchor; int def; } kActions[] = {
        {"dpad_up", "dpad-up", 19}, {"dpad_down", "dpad-down", 20}, {"dpad_left", "dpad-left", 21},
        {"dpad_right", "dpad-right", 22}, {"cross", "btn-cross", 96}, {"circle", "btn-circle", 97},
        {"square", "btn-square", 99}, {"triangle", "btn-triangle", 100}, {"l1", "btn-l1", 102}, {"r1", "btn-r1", 103},
        {"l2", "btn-l2", 104}, {"r2", "btn-r2", 105}, {"l3", "btn-l3", 106}, {"r3", "btn-r3", 107},
        {"select", "btn-select", 109}, {"start", "btn-start", 108}, {"analog", "", 0}};
    std::set<int> taken;   // the first action in list order takes a keycode
    for (const auto& a : kActions) {
        EmuBind* b = a.anchor[0] ? bindFor(&res.binds, a.anchor) : bindFor(&res.binds, "", "Analog");
        const int code = resolve(std::string("pad.map.") + a.id, a.def);
        if (code <= 0 || !taken.insert(code).second) continue;
        addIn(&b->in, Input::key(code));
        // LTRIGGER / BRAKE act as L2 and RTRIGGER / GAS as R2.
        if (code == akey::kL2) { addIn(&b->in, Input::axis(aaxis::kLTrigger, 1)); addIn(&b->in, Input::axis(aaxis::kBrake, 1)); }
        if (code == akey::kR2) { addIn(&b->in, Input::axis(aaxis::kRTrigger, 1)); addIn(&b->in, Input::axis(aaxis::kGas, 1)); }
    }
    auto stick = [&](const char* modeKey, const char* anchor, int ax, int ay) {
        std::string mode = p.get(game + modeKey, p.get(modeKey, "analog"));
        if (mode == "analog" || mode == "custom" || mode.empty()) {
            addIn(&bindFor(&res.binds, std::string(anchor) + ":up")->in, Input::axis(ay, -1));
            addIn(&bindFor(&res.binds, std::string(anchor) + ":down")->in, Input::axis(ay, 1));
            addIn(&bindFor(&res.binds, std::string(anchor) + ":left")->in, Input::axis(ax, -1));
            addIn(&bindFor(&res.binds, std::string(anchor) + ":right")->in, Input::axis(ax, 1));
        } else if (mode == "dpad") {
            addIn(&bindFor(&res.binds, "dpad-up")->in, Input::axis(ay, -1));
            addIn(&bindFor(&res.binds, "dpad-down")->in, Input::axis(ay, 1));
            addIn(&bindFor(&res.binds, "dpad-left")->in, Input::axis(ax, -1));
            addIn(&bindFor(&res.binds, "dpad-right")->in, Input::axis(ax, 1));
        } else {
            res.notes.push_back(std::string(anchor[6] == 'l' ? "Left" : "Right") + " stick: " + mode + " mode");
        }
    };
    stick("pad.lstick.mode", "stick-l", aaxis::kX, aaxis::kY);
    stick("pad.rstick.mode", "stick-r", aaxis::kZ, aaxis::kRZ);
    static const std::pair<const char*, const char*> kHot[] = {
        {"pad.menu.keycode", "Pause menu"}, {"pad.savestate.keycode", "Save state"},
        {"pad.loadstate.keycode", "Load state"}, {"pad.fastforward.keycode", "Fast forward (hold)"},
        {"pad.fastforwardtoggle.keycode", "Fast forward"}, {"pad.closegame.keycode", "Close game"}};
    for (const auto& h : kHot) {
        const int code = resolve(h.first, 0);
        if (code > 0) res.hotkeys.push_back({h.second, {Input::key(code)}});
    }
    for (const auto& kv : p.values)
        if (kv.first.find("pad.macro.") != std::string::npos) { res.notes.push_back("Macros not shown"); break; }
    return res;
}

// ============================================================================================
// Vita3K (partial: SDL sits between Android and the config)
// ============================================================================================

namespace {

std::vector<int> yamlIntList(const std::string& text, const std::string& key) {
    std::vector<int> out;
    const std::vector<std::string> lines = split(text, '\n', kMaxLines);
    for (size_t i = 0; i < lines.size() && i < kMaxLines; i++) {
        const std::string l = trim(lines[i]);
        if (l.compare(0, key.size() + 1, key + ":") != 0) continue;
        std::string rest = trim(l.substr(key.size() + 1));
        if (!rest.empty() && rest[0] == '[') {
            const size_t e = rest.find(']');
            for (const auto& v : split(rest.substr(1, e == std::string::npos ? std::string::npos : e - 1), ',')) {
                int n;
                if (parseInt(v, &n)) out.push_back(n);
            }
            return out;
        }
        for (size_t j = i + 1; j < lines.size() && out.size() < 64; j++) {
            const std::string t = trim(lines[j]);
            if (t.empty() || t[0] != '-') break;
            int n;
            if (parseInt(t.substr(1), &n)) out.push_back(n);
        }
        return out;
    }
    return out;
}

}  // namespace

ReadResult readVita3k(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "Vita3K";
    res.console = "vita";
    std::string text;
    const bool have = readAppFile(env, appdef::kVita3kConfig, &text, 1u << 20);
    res.source = have ? Source::kConfig : Source::kDefaults;
    std::vector<int> binds = have ? yamlIntList(text, "controller-binds") : std::vector<int>();
    std::vector<int> axes = have ? yamlIntList(text, "controller-axis-binds") : std::vector<int>();
    if (binds.size() != 15) { binds.clear(); for (int i = 0; i < 15; i++) binds.push_back(i); }
    if (axes.size() != 6) { axes.clear(); for (int i = 0; i < 6; i++) axes.push_back(i); }
    // SDL's Android table (SDL_sysjoystick.c): which keycodes make each SDL button.
    static const std::vector<std::vector<Input>> kSdl = {
        {Input::key(96)}, {Input::key(97)}, {Input::key(99)}, {Input::key(100)}, {Input::key(109)},
        {Input::key(110)}, {Input::key(108)}, {Input::key(106)}, {Input::key(107)}, {Input::key(102)},
        {Input::key(103)}, {Input::key(19)}, {Input::key(20)}, {Input::key(21)}, {Input::key(22)}};
    static const char* const kVita[15] = {"btn-cross", "btn-circle", "btn-square", "btn-triangle", "btn-select",
                                          "btn-home", "btn-start", "", "", "btn-l", "btn-r",
                                          "dpad-up", "dpad-down", "dpad-left", "dpad-right"};
    for (int i = 0; i < 15; i++) {
        if (!kVita[i][0]) continue;
        EmuBind* b = bindFor(&res.binds, kVita[i]);
        const int s = binds[i];
        if (s >= 0 && s < 15) for (const auto& in : kSdl[s]) addIn(&b->in, in);
    }
    static const int kSdlAxis[6] = {aaxis::kX, aaxis::kY, aaxis::kZ, aaxis::kRZ, aaxis::kLTrigger, aaxis::kRTrigger};
    static const char* const kVitaAxis[4] = {"stick-l", "stick-l", "stick-r", "stick-r"};
    for (int i = 0; i < 4; i++) {
        const int s = axes[i];
        if (s < 0 || s >= 6) continue;
        const bool x = i % 2 == 0;
        addIn(&bindFor(&res.binds, std::string(kVitaAxis[i]) + (x ? ":left" : ":up"))->in, Input::axis(kSdlAxis[s], -1));
        addIn(&bindFor(&res.binds, std::string(kVitaAxis[i]) + (x ? ":right" : ":down"))->in, Input::axis(kSdlAxis[s], 1));
    }
    return res;
}

// ============================================================================================
// WatermelonDS
// ============================================================================================

namespace {

const std::pair<const char*, const char*> kMelonButtons[] = {
    {"A", "btn-a"}, {"B", "btn-b"}, {"X", "btn-x"}, {"Y", "btn-y"}, {"UP", "dpad-up"}, {"DOWN", "dpad-down"},
    {"LEFT", "dpad-left"}, {"RIGHT", "dpad-right"}, {"L", "btn-l"}, {"R", "btn-r"}, {"START", "btn-start"},
    {"SELECT", "btn-select"}, {"HINGE", ""}};
const std::pair<const char*, const char*> kMelonHotkeys[] = {
    {"PAUSE", "Pause menu"}, {"FAST_FORWARD", "Fast forward"}, {"HOLD_FAST_FORWARD", "Fast forward (hold)"},
    {"QUICK_SAVE", "Save state"}, {"QUICK_LOAD", "Load state"}, {"REWIND", "Rewind"}, {"SWAP_SCREENS", "Swap screens"},
    {"EXIT_GAME", "Quit game"}, {"RESET", "Reset"}};
const char* const kMelonAll[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y", "DEBUG",
                                 "TOUCHSCREEN", "HINGE", "PAUSE", "FAST_FORWARD", "MICROPHONE", "RESET",
                                 "TOGGLE_SOFT_INPUT", "SWAP_SCREENS", "QUICK_SAVE", "QUICK_LOAD", "REWIND",
                                 "HOLD_FAST_FORWARD", "CYCLE_LAYOUT", "EXIT_GAME", "SCREENSHOT"};

// One assignment; type inferred from its fields when absent. *chord gets a key+modifier pair.
bool melonAssignment(const njson::Value* a, Input* in, std::vector<Input>* chord, bool* foreign) {
    *foreign = false;
    if (!a || !a->isObject()) return false;
    std::string type = a->getString("type");
    if (type.empty()) type = a->find("axisCode") ? "axis" : a->find("modifierKeyCode") ? "chord" : a->find("keyCode") ? "key" : "none";
    if (type == "none") return true;
    if (type == "key" || type == "chord") {
        const njson::Value* k = a->find("keyCode");
        if (!k || !k->isNumber()) return false;
        const njson::Value* m = a->find("modifierKeyCode");
        if (m && m->isNumber()) *chord = {Input::key(m->asInt()), Input::key(k->asInt())};
        else *in = Input::key(k->asInt());
        return true;
    }
    if (type == "axis") {
        const njson::Value* c = a->find("axisCode");
        if (!c || !c->isNumber()) return false;
        const std::string d = a->getString("direction");
        if (d != "POSITIVE" && d != "NEGATIVE") return false;
        *in = Input::axis(c->asInt(), d == "POSITIVE" ? 1 : -1);
        const njson::Value* dev = a->find("deviceId");
        *foreign = dev && dev->isNumber();   // axes honour deviceId; the pad's id is not known here
        return true;
    }
    return false;
}

void melonDefaults(ReadResult* res) {
    auto k = [&](const char* id, std::vector<Input> in) { bindFor(&res->binds, id)->in = std::move(in); };
    k("btn-a", {Input::key(akey::kButtonB)});
    k("btn-b", {Input::key(akey::kButtonA)});
    k("btn-x", {Input::key(akey::kButtonY)});
    k("btn-y", {Input::key(akey::kButtonX)});
    k("dpad-left", {Input::axis(aaxis::kHatX, -1), Input::axis(aaxis::kX, -1)});
    k("dpad-right", {Input::axis(aaxis::kHatX, 1), Input::axis(aaxis::kX, 1)});
    k("dpad-up", {Input::axis(aaxis::kHatY, -1), Input::axis(aaxis::kY, -1)});
    k("dpad-down", {Input::axis(aaxis::kHatY, 1), Input::axis(aaxis::kY, 1)});
    k("btn-l", {Input::key(akey::kL1)});
    k("btn-r", {Input::key(akey::kR1)});
    k("btn-start", {Input::key(akey::kStart)});
    k("btn-select", {Input::key(akey::kSelect)});
    res->hotkeys.push_back({"Pause menu", {Input::key(akey::kMode)}});
}

}  // namespace

ReadResult readWatermelonDs(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "WatermelonDS";
    res.console = "nds";
    std::string text;
    njson::Value v;
    const bool parsed = readAppFile(env, "/data/data/me.magnum.melondualds/files/controller_config.json", &text, 1u << 20) &&
                        njson::parse(text, &v) && v.isObject();
    const njson::Value* list = parsed ? v.find("inputMapper") : nullptr;
    bool ok = list && list->isArray();
    // The app parses the whole file or uses its defaults: an unknown input name fails it all.
    if (ok)
        for (const auto& e : list->arr) {
            const std::string name = e.isObject() ? e.getString("input") : std::string();
            if (std::find_if(std::begin(kMelonAll), std::end(kMelonAll), [&](const char* n) { return name == n; }) ==
                std::end(kMelonAll)) { ok = false; break; }
        }
    if (!ok) {
        res.source = Source::kDefaults;
        if (parsed) res.notes.push_back("Settings file not readable by WatermelonDS, defaults shown");
        melonDefaults(&res);
        return res;
    }
    res.source = Source::kConfig;
    for (const auto& b : kMelonButtons) if (b.second[0]) bindFor(&res.binds, b.second);
    std::set<int> keysTaken;   // a key press triggers the first input in list order bound to it
    for (const auto& e : list->arr) {
        const std::string name = e.getString("input");
        const char* anchor = nullptr;
        const char* hot = nullptr;
        for (const auto& b : kMelonButtons) if (name == b.first) anchor = b.second;
        for (const auto& h : kMelonHotkeys) if (name == h.first) hot = h.second;
        if (!anchor && !hot) continue;
        for (const char* field : {"assignment", "altAssignment"}) {
            const njson::Value* a = e.find(field);
            if (!a || a->isNull()) continue;   // both are optional (null when unset)
            Input in;
            std::vector<Input> chord;
            bool foreign;
            if (!melonAssignment(a, &in, &chord, &foreign)) {
                res.binds.clear();
                res.hotkeys.clear();
                res.source = Source::kDefaults;
                res.notes.push_back("Settings file not readable by WatermelonDS, defaults shown");
                melonDefaults(&res);
                return res;
            }
            if (foreign) in = Input::other("Another controller");
            if (in.kind == Input::kKey && !keysTaken.insert(in.code).second) continue;
            if (anchor && anchor[0]) {
                EmuBind* b = bindFor(&res.binds, anchor);
                if (!chord.empty()) b->chords.push_back(chord);
                else addIn(&b->in, in);
            } else if (anchor) {
                EmuBind* b = bindFor(&res.binds, "", "Hinge");
                addIn(&b->in, in);
            } else if (hot) {
                if (!chord.empty()) res.hotkeys.push_back({hot, chord});
                else if (!in.none()) res.hotkeys.push_back({hot, {in}});
            }
        }
    }
    return res;
}

// ============================================================================================
// drastic-nano and DraStic
// ============================================================================================

namespace {

// DraStic's action slots (drastic-nano/DrasticPrefs.h).
const char* dsAnchor(int a) {
    switch (a) {
        case 0: return "btn-x";
        case 1: return "btn-y";
        case 2: return "btn-b";
        case 3: return "btn-a";
        case 4: return "btn-r";
        case 5: return "btn-l";
        case 6: return "btn-start";
        case 7: return "btn-select";
        case 12: return "dpad-up";
        case 13: return "dpad-right";
        case 14: return "dpad-down";
        case 15: return "dpad-left";
    }
    return nullptr;
}
const char* dsHotkey(int a) {
    switch (a) {
        case 16: return "Swap screens";
        case 17: return "Fast forward";
        case 20: return "DraStic menu";
        case 28: return "Touch cursor";
        case 29: return "Save state";
        case 30: return "Load state";
        case 31: return "Close lid";
    }
    return nullptr;
}
const int kDsSeed[32] = {99, 100, 97, 96, 103, 102, 108, 109, -1, -1, -1, -1, 19, 22, 20, 21,
                         104, 105, -1, -1, 4, -1, -1, -1, -1, -1, -1, -1, 107, -1, -1, -1};

// drastic-nano's code -> evdev (DrasticPrefs.cpp kKeyTable, first entry for a code; 1000+ is raw).
int dsEvdev(int code) {
    static const std::pair<int, int> kTable[] = {
        {19, 103}, {20, 108}, {21, 105}, {22, 106}, {96, 304}, {97, 305}, {99, 307}, {100, 308}, {102, 310},
        {103, 311}, {104, 312}, {105, 313}, {106, 317}, {107, 318}, {108, 315}, {109, 314}, {110, 316}, {3, 102}, {82, 139}};
    if (code >= 1000) return code - 1000;
    for (const auto& e : kTable) if (e.first == code) return e.second;
    return -1;
}

}  // namespace

ReadResult readDrasticNano(const ReaderEnv& env, const Context& ctx) {
    ReadResult res;
    res.emulatorName = "drastic-nano";
    res.console = "nds";
    res.source = Source::kConfig;
    int keys[32];
    for (int a = 0; a < 32; a++) {
        keys[a] = kDsSeed[a];
        int v;
        if (parseInt(env.prop_("persist.gammaos.drastic_nano.key." + std::to_string(a)), &v)) keys[a] = v;
    }
    if (!ctx.romPath.empty()) {
        std::string user = env.prop_("persist.gammaos.drastic.data_dir");
        if (user.empty()) user = "/storage/emulated/0/drastic-nano";
        std::string text;
        const std::string rom = stemOf(ctx.romPath);
        if (safeName(rom) && readAppFile(env, user + "/overrides/" + rom + ".cfg", &text, 64u << 10)) {
            bool any = false;
            for (const auto& kv : parseCfg(text)) {
                int a, v;
                if (kv.first.compare(0, 4, "key.") == 0 && parseInt(kv.first.substr(4), &a) && a >= 0 && a < 32 &&
                    parseInt(kv.second, &v)) { keys[a] = v; any = true; }
            }
            if (any) res.notes.push_back("Game override: " + rom + ".cfg");
        }
    }
    // A code bound twice: the higher slot wins (applyPrefs). Back, the volume keys and Power never bind.
    std::map<int, int> codeToAction;
    for (int a = 0; a < 32; a++) {
        const int k = keys[a];
        if (k > 0 && k != 4 && k != 24 && k != 25 && k != 26) codeToAction[k] = a;
    }
    for (int a = 0; a < 16; a++) if (const char* id = dsAnchor(a)) bindFor(&res.binds, id);
    // drastic-nano plays a face button by position through ABXY Swap (nanoabxy::gameCode); the pad model
    // names gammapad's buttons by print, which the swap leaves alone.
    const GamepadState gp = gamepadStateFrom([&](const std::string& k) { return env.prop_(k); });
    const bool viaGammapad = gp.running && ((env.dev.vendor == gp.vid && env.dev.product == gp.pid) ||
                                            (!env.dev.vendor && !env.dev.product));
    auto padCode = [&](int ev) {
        if (!viaGammapad || !gp.swap) return ev;
        switch (ev) {
            case 304: return 305;
            case 305: return 304;
            case 307: return 308;
            case 308: return 307;
            default: return ev;
        }
    };
    for (const auto& ca : codeToAction) {
        const int ev = padCode(dsEvdev(ca.first));
        if (ev < 0) continue;
        if (const char* id = dsAnchor(ca.second)) addIn(&bindFor(&res.binds, id)->in, Input::evKey(ev));
        else if (const char* h = dsHotkey(ca.second)) res.hotkeys.push_back({h, {Input::evKey(ev)}});
    }
    // A pad code no slot takes still plays its fixed DS button (InputMap.cpp).
    static const std::pair<int, const char*> kFallback[] = {
        {304, "btn-a"}, {305, "btn-b"}, {307, "btn-x"}, {308, "btn-y"}, {310, "btn-l"}, {311, "btn-r"},
        {315, "btn-start"}, {314, "btn-select"}, {544, "dpad-up"}, {545, "dpad-down"}, {546, "dpad-left"},
        {547, "dpad-right"}};
    std::set<int> mappedEv;
    for (const auto& ca : codeToAction) mappedEv.insert(dsEvdev(ca.first));
    for (const auto& f : kFallback)
        if (!mappedEv.count(f.first)) addIn(&bindFor(&res.binds, f.second)->in, Input::evKey(padCode(f.first)));
    // The hat and the left stick always drive the D-pad.
    static const struct { const char* id; int abs; int dir; } kHat[] = {
        {"dpad-up", 17, -1}, {"dpad-down", 17, 1}, {"dpad-left", 16, -1}, {"dpad-right", 16, 1},
        {"dpad-up", 1, -1}, {"dpad-down", 1, 1}, {"dpad-left", 0, -1}, {"dpad-right", 0, 1}};
    for (const auto& h : kHat) addIn(&bindFor(&res.binds, h.id)->in, Input::evAbs(h.abs, h.dir));
    if (env.prop_("persist.gammaos.drastic_nano.portrait_controls") == "1")
        res.notes.push_back("Portrait controls turn the D-pad and buttons with the screen");
    return res;
}

ReadResult readDrastic(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "DraStic";
    res.console = "nds";
    std::string xml;
    const bool have = readAppFile(env, "/data/user/0/com.dsemu.drastic/shared_prefs/_Dra$t1c_Pref$_.xml", &xml, 1u << 20);
    const Prefs p = have ? parsePrefs(xml) : Prefs();
    res.source = have ? Source::kConfig : Source::kDefaults;
    // Which of DraStic's profiles is active is not stored where Nano can read it.
    if (have) res.notes.push_back("Mapping read from DraStic profile 1 (assumed)");
    for (int a = 0; a < 32; a++) {
        const int code = p.getInt("_KeyMapConfigs_0_" + std::to_string(a), kDsSeed[a]);
        if (const char* id = dsAnchor(a)) {
            EmuBind* b = bindFor(&res.binds, id);
            if (code > 0) addIn(&b->in, Input::key(code));
        } else if (const char* h = dsHotkey(a)) {
            if (code > 0 && code != 4) res.hotkeys.push_back({h, {Input::key(code)}});
        }
    }
    return res;
}

// ============================================================================================
// M64Plus FZ
// ============================================================================================

namespace {

// The app's built-in profiles (assets/mupen64plus_data/profiles/controller.cfg), the two a pad here
// meets: "Android Gamepad" is the default.
const std::pair<const char*, const char*> kM64BuiltIn[] = {
    {"Android Gamepad",
     "0:22,0:-31,1:21,1:-32,2:20,2:-33,3:19,3:-34,4:107,4:108,4:66,5:104,5:106,5:-35,5:-47,5:-37,5:40,6:99,6:42,"
     "7:96,7:41,8:-23,8:43,9:-24,9:38,10:-29,10:39,11:-30,11:37,12:103,12:44,13:102,13:55,16:-1,16:32,17:-2,17:29,"
     "18:-3,18:47,19:-4,19:51,32:97,32:100,32:111,"},
    {"Xbox", "0:-31,1:-32,2:-33,3:-34,4:108,5:-23,6:99,7:96,8:-25,9:-26,10:-27,11:-28,12:103,13:102,16:-1,17:-2,18:-3,19:-4,32:97,"},
};

const char* m64Anchor(int i) {
    static const char* const k[20] = {"dpad-right", "dpad-left", "dpad-down", "dpad-up", "btn-start", "btn-z", "btn-b",
                                      "btn-a", "c-right", "c-left", "c-down", "c-up", "btn-r", "btn-l", nullptr, nullptr,
                                      "stick-l:right", "stick-l:left", "stick-l:down", "stick-l:up"};
    return i >= 0 && i < 20 ? k[i] : nullptr;
}
const char* m64Hotkey(int i) {
    switch (i) {
        case 21: return "Save state";
        case 22: return "Load state";
        case 23: return "Reset";
        case 24: return "Quit game";
        case 25: return "Pause";
        case 26: return "Fast forward";
        case 32: return "M64Plus menu";
    }
    return nullptr;
}

std::string fileMd5(const ReaderEnv& env, const std::string& devicePath) {
    const std::string full = fullPath(env, devicePath);
    if (!nanobios::safePath(full)) return std::string();
    const int fd = open(full.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::string();
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (128 << 20)) { close(fd); return std::string(); }
    MD5_CTX c;
    MD5_Init(&c);
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) MD5_Update(&c, buf, (size_t)n);
    close(fd);
    unsigned char d[MD5_DIGEST_LENGTH];
    MD5_Final(d, &c);
    std::string hex;
    char h[3];
    for (unsigned char b : d) { snprintf(h, sizeof(h), "%02x", b); hex += h; }
    return hex;
}

}  // namespace

ReadResult readM64PlusFz(const ReaderEnv& env, const Context& ctx) {
    ReadResult res;
    res.emulatorName = "M64Plus FZ";
    res.console = "n64";
    const std::string prefsDir = "/data/data/org.mupen64plusae.v3.fzurita/shared_prefs/";
    std::string profile = "Android Gamepad";
    std::string xml;
    if (readAppFile(env, appdef::kMupenPrefs, &xml, 1u << 20)) {
        const Prefs g = parsePrefs(xml);
        if (g.values.count("controllerProfile1")) profile = g.get("controllerProfile1");
    }
    if (!ctx.romPath.empty()) {
        const std::string md5 = fileMd5(env, ctx.romPath);
        std::string gx;
        if (!md5.empty() && readAppFile(env, prefsDir + md5 + "_preferences.xml", &gx, 1u << 20)) {
            const Prefs gp = parsePrefs(gx);
            if (gp.values.count("controllerProfile1Game")) {
                profile = gp.get("controllerProfile1Game");
                res.notes.push_back("Game profile: " + profile);
                if (profile.empty()) {   // the game's controller is turned off
                    res.source = Source::kNotMapped;
                    res.notMappedWhere = "M64Plus FZ > game settings > Controller profile";
                    return res;
                }
            }
        }
    }
    std::string map;
    bool found = false;
    std::string user;
    if (readAppFile(env, "/data/data/org.mupen64plusae.v3.fzurita/files/Profiles/controller.cfg", &user, 512u << 10)) {
        const Ini ini = parseIni(user);
        auto s = ini.find(profile);
        if (s != ini.end() && s->second.count("map")) { map = s->second.at("map"); found = true; }
    }
    for (const auto& b : kM64BuiltIn)
        if (!found && profile == b.first) { map = b.second; found = true; }
    res.source = found ? Source::kConfig : Source::kDefaults;
    if (!found) {
        res.notes.push_back("Profile " + profile + " not known, default profile shown");
        map = kM64BuiltIn[0].second;
    } else {
        res.notes.push_back("Profile: " + profile);
    }
    // Pairs are command:code; a code mapped twice keeps the last (SerializableMap).
    std::map<int, int> codeToCmd;
    for (const auto& pair : split(map, ',')) {
        const size_t c = pair.find(':');
        int cmd, code;
        if (c == std::string::npos || !parseInt(pair.substr(0, c), &cmd) || !parseInt(pair.substr(c + 1), &code)) continue;
        codeToCmd[code] = cmd;
    }
    for (int i = 0; i < 20; i++) if (const char* id = m64Anchor(i)) bindFor(&res.binds, id);
    for (const auto& cc : codeToCmd) {
        Input in;
        if (cc.first >= 0) in = Input::key(cc.first);
        else in = Input::axis((-cc.first - 1) / 2, ((-cc.first) % 2) == 1 ? 1 : -1);
        if (const char* id = m64Anchor(cc.second)) addIn(&bindFor(&res.binds, id)->in, in);
        else if (const char* h = m64Hotkey(cc.second)) res.hotkeys.push_back({h, {in}});
    }
    return res;
}

// ============================================================================================
// ScummVM (partial: an engine may add its own keys)
// ============================================================================================

ReadResult readScummvm(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "ScummVM";
    res.console = "scummvm";
    res.drawing = "-";   // a pointer-and-click table, not a drawing
    std::string text;
    const bool have = readAppFile(env, appdef::kScummvmIni, &text, 1u << 20);
    const Ini ini = have ? parseIni(text) : Ini();
    auto km = ini.find("keymapper");
    res.source = km != ini.end() ? Source::kConfig : Source::kDefaults;
    auto hw = [](const std::string& id) -> Input {
        static const std::pair<const char*, Input> kHw[] = {
            {"JOY_A", Input::key(96)}, {"JOY_B", Input::key(97)}, {"JOY_X", Input::key(99)}, {"JOY_Y", Input::key(100)},
            {"JOY_LEFT_SHOULDER", Input::key(102)}, {"JOY_RIGHT_SHOULDER", Input::key(103)},
            {"JOY_LEFT_STICK", Input::key(106)}, {"JOY_RIGHT_STICK", Input::key(107)}, {"JOY_START", Input::key(108)},
            {"JOY_BACK", Input::key(109)}, {"JOY_GUIDE", Input::key(110)}, {"JOY_UP", Input::axis(aaxis::kHatY, -1)},
            {"JOY_DOWN", Input::axis(aaxis::kHatY, 1)}, {"JOY_LEFT", Input::axis(aaxis::kHatX, -1)},
            {"JOY_RIGHT", Input::axis(aaxis::kHatX, 1)}, {"JOY_LEFT_TRIGGER", Input::axis(aaxis::kLTrigger, 1)},
            {"JOY_RIGHT_TRIGGER", Input::axis(aaxis::kRTrigger, 1)}};
        for (const auto& h : kHw) if (id == h.first) return h.second;
        return id.empty() ? Input() : Input::other("Keyboard " + id);
    };
    // engine-default and the Android backend's keymaps (metaengine.cpp, android.cpp).
    static const struct { const char* key; const char* name; const char* def; } kActions[] = {
        {"keymap_engine-default_LCLK", "Left click", "JOY_A"},
        {"keymap_engine-default_RCLK", "Right click", "JOY_B"},
        {"keymap_engine-default_SKLI", "Skip line", "JOY_X"},
        {"keymap_engine-default_SKIP", "Skip", "JOY_Y"},
        {"keymap_engine-default_MENU", "Game menu", "JOY_LEFT_SHOULDER"},
        {"keymap_engine-default_PAUS", "Pause", ""},
        {"keymap_global_MENU", "ScummVM menu", "JOY_START"},
        {"keymap_global_VIRT", "Virtual keyboard", "JOY_BACK"},
        {"keymap_android-default_MOUSE_SLOW", "Slow mouse", "JOY_RIGHT_SHOULDER"}};
    for (const auto& a : kActions) {
        EmuBind* b = bindFor(&res.binds, "", a.name);
        std::string v = a.def;
        if (km != ini.end()) {
            auto it = km->second.find(a.key);
            if (it != km->second.end()) v = it->second;   // empty = unbound
        }
        for (const auto& id : split(v, ' ')) addIn(&b->in, hw(trim(id)));
    }
    EmuBind* mouse = bindFor(&res.binds, "", "Move the pointer");
    for (Input in : {Input::axis(aaxis::kX, -1), Input::axis(aaxis::kX, 1), Input::axis(aaxis::kY, -1), Input::axis(aaxis::kY, 1)})
        addIn(&mouse->in, in);
    res.notes.push_back("This game may add its own keys");
    return res;
}

// ============================================================================================
// SeedlessDS
// ============================================================================================

ReadResult readSeedlessDs(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "SeedlessDS";
    res.console = "nds";
    std::string xml;
    const bool have = readAppFile(env, "/data/data/com.seedlessds.app/shared_prefs/keymap.xml", &xml, 1u << 20);
    const Prefs p = have ? parsePrefs(xml) : Prefs();
    res.source = have ? Source::kConfig : Source::kDefaults;
    static const char* const kDs[12] = {"dpad-up", "dpad-down", "dpad-left", "dpad-right", "btn-a", "btn-b",
                                        "btn-x", "btn-y", "btn-l", "btn-r", "btn-start", "btn-select"};
    static const int kDef[12] = {19, 20, 21, 22, 96, 97, 99, 100, 102, 103, 108, 109};
    const int profile = std::clamp(p.getInt("_KeyMapId", 0), 0, 2);
    const bool old = p.getInt("_KeyMapVer", 0) < 2;   // the app erases every k_ key then
    const std::string pre = profile == 0 ? "k_" : "p" + std::to_string(profile) + "_k_";
    std::set<int> taken;   // the first in list order wins
    for (int i = 0; i < 12; i++) {
        EmuBind* b = bindFor(&res.binds, kDs[i]);
        const int code = old ? kDef[i] : p.getInt(pre + std::to_string(i), kDef[i]);
        if (code > 0 && taken.insert(code).second) addIn(&b->in, Input::key(code));
    }
    // Fixed in code: the stick and the hat drive the D-pad; L2/R2 press L/R with _AnalogTriggers.
    static const struct { int i; int axis; int dir; } kFixed[] = {
        {0, aaxis::kY, -1}, {1, aaxis::kY, 1}, {2, aaxis::kX, -1}, {3, aaxis::kX, 1},
        {0, aaxis::kHatY, -1}, {1, aaxis::kHatY, 1}, {2, aaxis::kHatX, -1}, {3, aaxis::kHatX, 1}};
    for (const auto& f : kFixed) addIn(&bindFor(&res.binds, kDs[f.i])->in, Input::axis(f.axis, f.dir));
    std::string cfg;
    const bool analogTriggers = !readAppFile(env, appdef::kSeedlessPrefs, &cfg, 1u << 20) ||
                                parsePrefs(cfg).get("_AnalogTriggers", "true") != "false";
    if (analogTriggers) {
        addIn(&bindFor(&res.binds, "btn-l")->in, Input::axis(aaxis::kLTrigger, 1));
        addIn(&bindFor(&res.binds, "btn-r")->in, Input::axis(aaxis::kRTrigger, 1));
    }
    if (profile) res.notes.push_back("Key map profile " + std::to_string(profile + 1));
    return res;
}

// ============================================================================================
// hakuX
// ============================================================================================

ReadResult readHakuX(const ReaderEnv& env, const Context&) {
    ReadResult res;
    res.emulatorName = "hakuX";
    res.console = "xbox";
    static const std::pair<const char*, const char*> kXbox[] = {
        {"A", "btn-a"}, {"B", "btn-b"}, {"X", "btn-x"}, {"Y", "btn-y"}, {"BLACK", "btn-black"}, {"WHITE", "btn-white"},
        {"START", "btn-start"}, {"BACK", "btn-back"}, {"LTHUMB", "btn-l3"}, {"RTHUMB", "btn-r3"},
        {"DPAD_UP", "dpad-up"}, {"DPAD_DOWN", "dpad-down"}, {"DPAD_LEFT", "dpad-left"}, {"DPAD_RIGHT", "dpad-right"},
        {"LT", "btn-lt"}, {"RT", "btn-rt"}};
    std::string text;
    njson::Value v;
    const njson::Value* bind = nullptr;
    std::string used;
    if (readAppFile(env, "/data/data/com.rfandango.haku_x/files/controllers.json", &text, 1u << 20) &&
        njson::parse(text, &v) && v.isObject()) {
        const njson::Value* profiles = v.find("profiles");
        char model[320];
        snprintf(model, sizeof(model), "m:%04x:%04x:%s", env.dev.vendor, env.dev.product, env.dev.name.c_str());
        for (const std::string& key : {"u:" + env.dev.descriptor, std::string(model)}) {
            if (!profiles || key == "u:") continue;
            const njson::Value* pr = profiles->find(key);
            if (pr && pr->find("bind") && pr->find("bind")->isObject()) { bind = pr->find("bind"); used = key; break; }
        }
    }
    auto parse = [](const std::string& s, std::vector<Input>* out, bool stickX, bool stickY, bool neg) {
        if (s.size() < 2) return;
        int n;
        if (s[0] == 'k' && parseInt(s.substr(1), &n)) { addIn(out, Input::key(n)); return; }
        if (s[0] != 'a') return;
        const char last = s.back();
        if (last == '+' || last == '-') {
            if (parseInt(s.substr(1, s.size() - 2), &n)) addIn(out, Input::axis(n, last == '+' ? 1 : -1));
        } else if (parseInt(s.substr(1), &n)) {
            addIn(out, Input::axis(n, (stickX || stickY) ? (neg ? -1 : 1) : 1));
        }
    };
    for (const auto& x : kXbox) bindFor(&res.binds, x.second);
    for (const char* s : {"stick-l", "stick-r"})
        for (const char* d : kDirs) bindFor(&res.binds, std::string(s) + ":" + d);
    if (!bind) {
        res.source = Source::kDefaults;
        auto k = [&](const char* id, std::vector<Input> in) { bindFor(&res.binds, id)->in = std::move(in); };
        k("btn-a", {Input::key(96)}); k("btn-b", {Input::key(97)}); k("btn-x", {Input::key(99)}); k("btn-y", {Input::key(100)});
        k("btn-black", {Input::key(103)}); k("btn-white", {Input::key(102)}); k("btn-start", {Input::key(108)});
        k("btn-back", {Input::key(109)}); k("btn-l3", {Input::key(106)}); k("btn-r3", {Input::key(107)});
        k("btn-lt", {Input::axis(aaxis::kLTrigger, 1), Input::key(104)});
        k("btn-rt", {Input::axis(aaxis::kRTrigger, 1), Input::key(105)});
        k("dpad-up", {Input::axis(aaxis::kHatY, -1)}); k("dpad-down", {Input::axis(aaxis::kHatY, 1)});
        k("dpad-left", {Input::axis(aaxis::kHatX, -1)}); k("dpad-right", {Input::axis(aaxis::kHatX, 1)});
        k("stick-l:up", {Input::axis(aaxis::kY, -1)}); k("stick-l:down", {Input::axis(aaxis::kY, 1)});
        k("stick-l:left", {Input::axis(aaxis::kX, -1)}); k("stick-l:right", {Input::axis(aaxis::kX, 1)});
        k("stick-r:up", {Input::axis(aaxis::kRZ, -1)}); k("stick-r:down", {Input::axis(aaxis::kRZ, 1)});
        k("stick-r:left", {Input::axis(aaxis::kZ, -1)}); k("stick-r:right", {Input::axis(aaxis::kZ, 1)});
        res.hotkeys.push_back({"hakuX menu", {Input::key(akey::kMode)}});
        return res;
    }
    res.source = Source::kConfig;
    res.notes.push_back(used.compare(0, 2, "u:") == 0 ? "Profile for this controller" : "Profile for this model");
    for (const auto& kv : bind->obj) {
        if (!kv.second.isArray()) continue;
        for (const auto& x : kXbox)
            if (kv.first == x.first)
                for (const auto& e : kv.second.arr) if (e.isString()) parse(e.str, &bindFor(&res.binds, x.second)->in, false, false, false);
        static const struct { const char* name; const char* anchor; bool x; } kSticks[] = {
            {"LS_X", "stick-l", true}, {"LS_Y", "stick-l", false}, {"RS_X", "stick-r", true}, {"RS_Y", "stick-r", false}};
        for (const auto& s : kSticks) {
            if (kv.first != s.name) continue;
            for (const auto& e : kv.second.arr) {
                if (!e.isString()) continue;
                const std::string a = std::string(s.anchor) + (s.x ? ":left" : ":up");
                const std::string b = std::string(s.anchor) + (s.x ? ":right" : ":down");
                const char last = e.str.empty() ? 0 : e.str.back();
                if (last == '+' || last == '-') {
                    parse(e.str, &bindFor(&res.binds, last == '-' ? a : b)->in, s.x, !s.x, last == '-');
                } else {
                    parse(e.str, &bindFor(&res.binds, a)->in, s.x, !s.x, true);
                    parse(e.str, &bindFor(&res.binds, b)->in, s.x, !s.x, false);
                }
            }
        }
    }
    return res;
}

// ============================================================================================

ReadResult readEmulator(const ReaderEnv& env, const Context& ctx, const Catalog& cat) {
    const std::string& e = ctx.emulator;
    if (e == "retroarch") return readRetroArch(env, ctx, cat);
    if (e == "ppsspp") return readPpsspp(env, ctx);
    if (e == "dolphin") return readDolphin(env, ctx);
    if (e == "azahar") return readAzahar(env, ctx);
    if (e == "flycast") return readFlycast(env, ctx);
    if (e == "armsx2") return readArmsx2(env, ctx);
    if (e == "vita3k") return readVita3k(env, ctx);
    if (e == "watermelonds") return readWatermelonDs(env, ctx);
    if (e == "drastic-nano") return readDrasticNano(env, ctx);
    if (e == "drastic") return readDrastic(env, ctx);
    if (e == "mupen64plus-fz") return readM64PlusFz(env, ctx);
    if (e == "scummvm") return readScummvm(env, ctx);
    if (e == "seedlessds") return readSeedlessDs(env, ctx);
    if (e == "hakux") return readHakuX(env, ctx);
    ReadResult res;
    res.emulatorName = e.empty() ? ctx.package : e;
    res.source = Source::kUnknown;
    res.notes.push_back("No reader for " + res.emulatorName);
    return res;
}

}  // namespace bmap
}  // namespace android
