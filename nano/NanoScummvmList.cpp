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

#include "NanoScummvmList.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>

namespace android {
namespace nanoscummvm {

namespace {

constexpr size_t kMaxLines = 200000;
constexpr size_t kMaxGames = 10000;
constexpr size_t kMaxNameBytes = 150;
constexpr const char* kExt = ".scummvm";

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

// What ScummVM's command line takes as a target, and what the {file.content} launch passes on
// (nanolaunch::launchFileId).
bool validId(const std::string& s) {
    if (s.empty() || s.size() > 64 || s[0] == '-') return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

// "Beneath a Steel Sky (CD/DOS/English)" -> "Beneath a Steel Sky"; unchanged without a trailing group
// or when nothing would be left.
std::string stripVariant(const std::string& d) {
    const std::string t = trim(d);
    if (t.empty() || t.back() != ')') return t;
    int depth = 0;
    for (size_t i = t.size(); i-- > 0;) {
        if (t[i] == ')') depth++;
        else if (t[i] == '(' && --depth == 0) {
            const std::string rest = trim(t.substr(0, i));
            return rest.empty() ? t : rest;
        }
    }
    return t;
}

std::string fileSafe(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f) out += ' ';
        else if (c == ':') out += " -";
        else if (c == '/') out += ", ";
        else if (c == '\\' || c == '|') out += '-';
        else if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>') continue;
        else out += (char)c;
    }
    std::string squeezed;
    for (char c : out) {
        if (c == ' ' && !squeezed.empty() && squeezed.back() == ' ') continue;
        squeezed += c;
    }
    if (squeezed.size() > kMaxNameBytes) {
        size_t n = kMaxNameBytes;
        while (n > 0 && ((unsigned char)squeezed[n] & 0xC0) == 0x80) n--;   // not inside a UTF-8 character
        squeezed.resize(n);
    }
    size_t b = 0, e = squeezed.size();
    while (b < e && (squeezed[b] == ' ' || squeezed[b] == '.')) b++;   // a leading dot hides the file
    while (e > b && (squeezed[e - 1] == ' ' || squeezed[e - 1] == '.')) e--;
    return squeezed.substr(b, e - b);
}

// ScummVM's own sections of scummvm.ini, never games.
bool isOwnSection(const std::string& name) {
    const std::string n = lower(name);
    return n == "scummvm" || n == "keymapper" || n == "cloud" || n == "achievements";
}

bool readSmall(const std::string& path, size_t max, std::string* out) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }
    out->assign(max, '\0');
    size_t got = 0;
    while (got < max) {
        const ssize_t n = read(fd, &(*out)[got], max - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    out->resize(got);
    return true;
}

bool writeAtomic(const std::string& dir, const std::string& name, const std::string& text) {
    const std::string tmp = dir + "/." + name + "." + std::to_string((long)getpid()) + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    const bool ok = close(fd) == 0 && done == text.size() && rename(tmp.c_str(), (dir + "/" + name).c_str()) == 0;
    if (!ok) unlink(tmp.c_str());
    return ok;
}

}  // namespace

bool isScummvmPkg(const std::string& pkg) {
    return pkg == "org.scummvm.scummvm" || pkg == "org.scummvm.scummvm.debug";
}

bool listsGames(const std::string& romDir, const std::string& pkg) {
    return isScummvmPkg(pkg) && lower(romDir) == "scummvm";
}

std::string iniPath(const std::string& pkg) {
    return "/data/data/" + pkg + "/files/scummvm.ini";
}

std::string mirrorDir(const std::string& pkg) {
    return std::string(kMirrorRoot) + "/" + pkg + "/scummvm";
}

bool isMirrorPath(const std::string& path) {
    const std::string root = std::string(kMirrorRoot) + "/";
    return path.compare(0, root.size(), root) == 0;
}

std::vector<Game> parseGames(const std::string& iniText) {
    // ScummVM matches section and key names ignoring case (ConfigManager's maps).
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> sections;
    std::map<std::string, size_t> index;
    size_t cur = SIZE_MAX;
    size_t pos = iniText.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0, lines = 0;
    while (pos < iniText.size() && lines++ < kMaxLines) {
        size_t end = iniText.find('\n', pos);
        if (end == std::string::npos) end = iniText.size();
        const std::string line = trim(iniText.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            const std::string name = trim(close == std::string::npos ? line.substr(1) : line.substr(1, close - 1));
            auto it = index.find(lower(name));
            if (it != index.end()) {
                cur = it->second;
            } else {
                cur = sections.size();
                index[lower(name)] = cur;
                sections.push_back({name, {}});
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (cur == SIZE_MAX || eq == std::string::npos) continue;
        sections[cur].second[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    std::vector<Game> games;
    for (const auto& s : sections) {
        if (games.size() >= kMaxGames) break;
        if (!validId(s.first) || isOwnSection(s.first)) continue;
        auto gid = s.second.find("gameid");
        auto path = s.second.find("path");
        std::string gameid;
        if (gid != s.second.end()) gameid = gid->second;
        else if (path != s.second.end() && !path->second.empty()) gameid = s.first;
        if (gameid.empty()) continue;
        Game g;
        g.target = s.first;
        g.gameid = gameid;
        auto desc = s.second.find("description");
        if (desc != s.second.end()) g.description = desc->second;
        games.push_back(g);
    }
    return games;
}

std::vector<std::string> fileNames(const std::vector<Game>& games,
                                   const std::map<std::string, std::string>& current) {
    const size_t n = games.size();
    std::vector<std::string> full(n), stripped(n), names(n);
    for (size_t i = 0; i < n; i++) {
        const Game& g = games[i];
        full[i] = fileSafe(!g.description.empty() ? g.description : g.gameid);
        if (full[i].empty()) full[i] = g.target;
        stripped[i] = fileSafe(stripVariant(!g.description.empty() ? g.description : g.gameid));
        if (stripped[i].empty()) stripped[i] = g.target;
        names[i] = stripped[i];
    }
    auto shared = [&]() {
        std::map<std::string, int> count;
        for (const auto& nm : names) count[lower(nm)]++;
        std::vector<bool> out(n);
        for (size_t i = 0; i < n; i++) out[i] = count[lower(names[i])] > 1;
        return out;
    };
    std::vector<bool> dup = shared();
    for (size_t i = 0; i < n; i++) if (dup[i]) names[i] = full[i];
    dup = shared();
    for (size_t i = 0; i < n; i++) if (dup[i]) names[i] += " [" + games[i].target + "]";

    // A name this game could have now, with or without a " (k)" added for uniqueness.
    auto fits = [&](size_t i, const std::string& name) {
        if (!endsWithNoCase(name, kExt)) return false;
        std::string stem = lower(name.substr(0, name.size() - std::string(kExt).size()));
        if (!stem.empty() && stem.back() == ')') {
            const size_t open = stem.rfind(" (");
            if (open != std::string::npos && open + 3 < stem.size()) {
                bool digits = true;
                for (size_t k = open + 2; k + 1 < stem.size(); k++) digits = digits && isdigit((unsigned char)stem[k]);
                if (digits) stem.resize(open);
            }
        }
        const std::string tag = " [" + lower(games[i].target) + "]";
        return stem == lower(stripped[i]) || stem == lower(full[i]) || stem == lower(stripped[i]) + tag ||
               stem == lower(full[i]) + tag;
    };
    std::vector<std::string> out(n);
    std::set<std::string> taken;
    for (size_t i = 0; i < n; i++) {
        auto it = current.find(games[i].target);
        if (it != current.end() && fits(i, it->second) && taken.insert(lower(it->second)).second) out[i] = it->second;
    }
    for (size_t i = 0; i < n; i++) {
        if (!out[i].empty()) continue;
        std::string nm = names[i];
        for (int k = 2; !taken.insert(lower(nm + kExt)).second; k++) nm = names[i] + " (" + std::to_string(k) + ")";
        out[i] = nm + kExt;
    }
    return out;
}

bool iniComplete(const std::string& iniText) {
    size_t pos = iniText.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (pos < iniText.size()) {
        size_t end = iniText.find('\n', pos);
        if (end == std::string::npos) end = iniText.size();
        const std::string line = trim(iniText.substr(pos, end - pos));
        pos = end + 1;
        if (line.size() >= 2 && line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos && lower(trim(line.substr(1, close - 1))) == "scummvm") return true;
        }
    }
    return false;
}

std::string fileText(const Game& g) {
    return g.target + "\n" + g.gameid + "\n";
}

std::string mirrorGameId(const std::string& path, const std::string& root) {
    std::string text;
    const std::string prefix = root + "/";
    if (path.compare(0, prefix.size(), prefix) != 0 || !readSmall(path, 512, &text)) return std::string();
    const size_t nl = text.find('\n');
    if (nl == std::string::npos) return std::string();
    const size_t end = text.find('\n', nl + 1);
    const std::string id = trim(text.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1));
    return validId(id) ? id : std::string();
}

bool syncDir(const std::string& dir, const std::vector<Game>& games, bool removeStale) {
    static std::mutex mu;   // the scan thread and the menu's own scan
    std::lock_guard<std::mutex> lock(mu);
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    // Each game's file now, by the target on its first line.
    std::map<std::string, std::string> current;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || !endsWithNoCase(name, kExt)) continue;
            std::string text;
            if (!readSmall(dir + "/" + name, 512, &text)) continue;
            const std::string target = trim(text.substr(0, text.find('\n')));
            if (validId(target)) current.emplace(target, name);
        }
        closedir(d);
    }
    const std::vector<std::string> names = fileNames(games, current);
    std::set<std::string> want;
    for (size_t i = 0; i < games.size(); i++) {
        want.insert(names[i]);
        const std::string text = fileText(games[i]);
        std::string have;
        if (readSmall(dir + "/" + names[i], 4096, &have) && have == text) continue;
        writeAtomic(dir, names[i], text);
    }
    DIR* d = opendir(dir.c_str());
    if (!d) return true;
    const time_t now = time(nullptr);
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = dir + "/" + name;
        struct stat est;
        if (lstat(path.c_str(), &est) != 0 || !S_ISREG(est.st_mode)) continue;
        // Another Nano process may be writing its own temporary file: only an old one is left over.
        if (name[0] == '.' && endsWithNoCase(name, ".tmp")) {
            if (now - est.st_mtime > 600) unlink(path.c_str());
        } else if (removeStale && endsWithNoCase(name, kExt) && !want.count(name)) {
            unlink(path.c_str());
        }
    }
    closedir(d);
    return true;
}

}  // namespace nanoscummvm
}  // namespace android
