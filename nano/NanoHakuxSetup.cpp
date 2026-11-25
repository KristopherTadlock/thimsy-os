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

#define LOG_TAG "NanoMenu"

#include "NanoHakuxSetup.h"

#include "NanoAppFiles.h"

#include <dirent.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>

namespace android {
namespace nanohakux {

namespace {

bool isVolumeUuid(const std::string& v) {
    return !v.empty() && v.find_first_not_of("0123456789ABCDEFabcdef-") == std::string::npos;
}

bool isFolderName(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

std::string folderKey(const Folder& f) { return f.volume + ":" + nanobios::lowerStr(f.rel); }

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : "\n") + s;
    return out;
}

}  // namespace

bool folderOf(const std::string& path, Folder* out) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (!nanobios::safePath(p)) return false;
    std::string volume, rest;
    for (const char* pre : {"/data/media/0/", "/sdcard/", "/storage/emulated/0/", "/storage/self/primary/"}) {
        if (p.compare(0, strlen(pre), pre) == 0) {
            volume = "primary";
            rest = p.substr(strlen(pre));
            break;
        }
    }
    if (volume.empty()) {
        for (const char* pre : {"/storage/", "/mnt/media_rw/"}) {
            if (p.compare(0, strlen(pre), pre) != 0) continue;
            const size_t sl = p.find('/', strlen(pre));
            if (sl == std::string::npos) return false;
            volume = p.substr(strlen(pre), sl - strlen(pre));
            rest = p.substr(sl + 1);
            break;
        }
        if (!isVolumeUuid(volume)) return false;
    }
    const size_t sl = rest.find('/');
    if (sl == std::string::npos || strcasecmp(rest.substr(0, sl).c_str(), "roms") != 0) return false;
    const std::string name = rest.substr(sl + 1);
    if (!isFolderName(name)) return false;
    const Folder f{volume, "ROMs/" + name};
    // "<sequence>|uri_revoke|<package>|<volume>|<rel>" in a PROPERTY_VALUE_MAX (92) property.
    if (strlen(kPkg) + f.volume.size() + f.rel.size() + 2 + 25 >= 92) return false;
    *out = f;
    return true;
}

std::string folderUri(const Folder& f) { return azaharTreeUri(f.volume, f.rel); }

bool uriFolder(const std::string& uri, Folder* out) {
    const std::string prefix = appdef::kExternalStorageTree;
    if (uri.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string id = sdUriDecode(uri.substr(prefix.size()));
    const size_t colon = id.find(':');
    if (colon == std::string::npos) return false;
    const Folder f{id.substr(0, colon), id.substr(colon + 1)};
    if ((f.volume != "primary" && !isVolumeUuid(f.volume)) || f.rel.compare(0, 5, "ROMs/") != 0
        || !isFolderName(f.rel.substr(5)) || folderUri(f) != uri)
        return false;
    *out = f;
    return true;
}

std::vector<Folder> resolveFolders(const Settings& s, const std::vector<Folder>& previous,
                                   const std::function<bool(const std::string& volume)>& mounted,
                                   const std::function<bool(const std::string& path)>& exists) {
    std::set<std::string> named;
    for (const auto& c : s.candidates) {
        Folder f;
        if (folderOf(c, &f)) named.insert(folderKey(f));
    }
    std::vector<Folder> out;
    std::set<std::string> have;
    auto add = [&](const Folder& f) {
        if (have.insert(folderKey(f)).second) out.push_back(f);
    };
    for (const auto& c : s.candidates) {
        Folder f;
        if (folderOf(c, &f) && mounted(f.volume) && (exists(c) || contains(s.makeable, c))) add(f);
    }
    for (const auto& p : previous) {
        if (named.count(folderKey(p))
            || (s.defaultScan && !mounted(p.volume) && named.count(folderKey(Folder{"primary", p.rel}))))
            add(p);
    }
    std::stable_sort(out.begin(), out.end(), [](const Folder& a, const Folder& b) {
        const bool ap = a.volume != "primary", bp = b.volume != "primary";
        return ap != bp ? !ap : (ap && a.volume < b.volume);
    });
    return out;
}

std::string prefsText(const std::string& prefs, const std::string& name) {
    const std::string key = "<string name=\"" + name + "\">";
    const size_t a = prefs.find(key);
    if (a == std::string::npos) return std::string();
    const size_t b = prefs.find("</string>", a);
    if (b == std::string::npos) return std::string();
    const std::string v = prefs.substr(a + key.size(), b - a - key.size());
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        const size_t semi = v[i] == '&' ? v.find(';', i) : std::string::npos;
        if (semi == std::string::npos || semi - i > 10) {
            out += v[i];
            continue;
        }
        const std::string ent = v.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            char* end = nullptr;
            const long c = strtol(ent.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
            if (!end || *end || c <= 0 || c > 0x7f) { out += v[i]; continue; }   // only the ASCII ones Android writes
            out += (char)c;
        } else {
            out += v[i];
            continue;
        }
        i = semi;
    }
    return out;
}

std::vector<std::string> prefsLines(const std::string& prefs, const std::string& name) {
    std::vector<std::string> out;
    const std::string text = prefsText(prefs, name);
    for (size_t a = 0; a < text.size();) {
        const size_t b = std::min(text.find('\n', a), text.size());
        const std::string l = text.substr(a, b - a);
        if (l.find_first_not_of(" \t\r") != std::string::npos && !contains(out, l)) out.push_back(l);
        a = b + 1;
    }
    return out;
}

bool prefsSetText(std::string* prefs, const std::string& name, const std::string& value) {
    if (prefs->find("<map") == std::string::npos)
        *prefs = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n</map>\n";
    const size_t empty = prefs->find("<map />");
    if (empty != std::string::npos) prefs->replace(empty, 7, "<map>\n</map>");
    std::string esc;   // FastXmlSerializer's escapes
    for (unsigned char c : value) {
        if (c < 0x20) esc += "&#" + std::to_string(c) + ";";
        else if (c == '"') esc += "&quot;";
        else if (c == '&') esc += "&amp;";
        else if (c == '<') esc += "&lt;";
        else if (c == '>') esc += "&gt;";
        else esc += (char)c;
    }
    const std::string key = "<string name=\"" + name + "\">";
    const size_t a = prefs->find(key);
    if (a != std::string::npos) {
        const size_t b = prefs->find("</string>", a);
        if (b == std::string::npos) return false;
        prefs->replace(a + key.size(), b - a - key.size(), esc);
        return true;
    }
    const size_t end = prefs->rfind("</map>");
    if (end == std::string::npos) return false;
    prefs->insert(end, "    " + key + esc + "</string>\n");
    return true;
}

Plan planSync(const std::string& prefs, const std::vector<std::string>& thimsyIn) {
    std::vector<std::string> thimsy;
    for (const auto& t : thimsyIn)
        if (!contains(thimsy, t)) thimsy.push_back(t);
    const std::vector<std::string> before = prefsLines(prefs, kThimsyKey);
    std::vector<std::string> listed = prefsLines(prefs, "gamesFolders");
    if (listed.empty()) listed = prefsLines(prefs, "gamesFolderUri");   // as hakuX reads them
    std::vector<std::string> next = thimsy;
    for (const auto& g : listed)
        if (!contains(before, g) && !contains(next, g)) next.push_back(g);
    // Thimsy's own: those it wrote before, and new ones hakuX doesn't list yet. A folder the user had
    // already picked in hakuX stays the user's, so it is never taken back.
    std::vector<std::string> owned;
    for (const auto& t : thimsy)
        if (contains(before, t) || !contains(listed, t)) owned.push_back(t);

    Plan plan;
    const std::string first = next.empty() ? std::string() : next[0];
    plan.write = prefsText(prefs, "gamesFolders") != joinLines(next) || prefsText(prefs, "gamesFolderUri") != first
                 || before != owned || (!thimsy.empty() && nanobios::prefsGetBool(prefs, "setup_complete") != 1);
    if (!plan.write) return plan;
    for (const auto& t : owned)
        if (!contains(before, t) || !contains(listed, t)) plan.grant.push_back(t);
    for (const auto& b : before)
        if (!contains(thimsy, b)) plan.revoke.push_back(b);
    plan.prefs = prefs;
    if (next.empty()) {
        sdPrefsRemove(&plan.prefs, "gamesFolders");
        sdPrefsRemove(&plan.prefs, "gamesFolderUri");
    } else if (!prefsSetText(&plan.prefs, "gamesFolders", joinLines(next))
               || !prefsSetText(&plan.prefs, "gamesFolderUri", first)
               || !nanobios::prefsSetBool(&plan.prefs, "setup_complete", true)) {
        plan.write = false;
    }
    if (plan.write && !prefsSetText(&plan.prefs, kThimsyKey, joinLines(owned))) plan.write = false;
    if (!plan.write) {
        plan.grant.clear();
        plan.revoke.clear();
    }
    return plan;
}

bool packageRunning(const std::string& pkg, const std::string& procRoot) {
    DIR* d = opendir(procRoot.c_str());
    if (!d) return false;
    bool found = false;
    while (!found) {
        const dirent* e = readdir(d);
        if (!e) break;
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const std::string path = procRoot + "/" + e->d_name + "/cmdline";
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char cmd[256] = {};
        const ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0) continue;
        found = strncmp(cmd, pkg.c_str(), pkg.size()) == 0 && (cmd[pkg.size()] == '\0' || cmd[pkg.size()] == ':');
    }
    closedir(d);
    return found;
}

Outcome sync(const Env& env, const Settings& settings) {
    const std::string prefsPath = env.root + kPrefs;
    nanobios::Packages pk;
    pk.load(env.root + "/data/system/packages.list");
    if (!pk.readable || !pk.uid.count(kPkg) || nanobios::appOwnerUid(prefsPath, env.root) < 0)
        return Outcome::kNotInstalled;
    std::string prefs;
    if (nanobios::pathExists(prefsPath) || nanobios::pathExists(prefsPath + ".bak")) {
        // Unreadable, or text that is not a settings file: never replaced by one holding only the folders.
        if (!env.read(prefsPath, &prefs) || (!prefs.empty() && prefs.find("<map") == std::string::npos))
            return Outcome::kWait;
    }
    std::vector<Folder> previous;
    for (const auto& u : prefsLines(prefs, kThimsyKey)) {
        Folder f;
        if (uriFolder(u, &f)) previous.push_back(f);
    }
    const std::vector<Folder> folders = resolveFolders(settings, previous, env.mounted, env.exists);
    std::vector<std::string> uris;
    for (const auto& f : folders) uris.push_back(folderUri(f));
    // A system folder made when missing, also once deleted after hakuX was given it.
    for (const auto& f : folders) {
        const std::string path = "/data/media/0/" + f.rel;
        if (f.volume != "primary" || !contains(settings.makeable, path) || env.exists(path)) continue;
        int fd;
        const int err = nanobios::openDir(env.root + path, &fd, &pk, std::string(), nullptr, env.root);
        if (err) {
            ALOGW("hakux: %s: %s", path.c_str(), strerror(err));
            return Outcome::kFailed;
        }
        close(fd);
    }
    const Plan plan = planSync(prefs, uris);
    if (!plan.write) return Outcome::kDone;
    if (env.running(kPkg)) return Outcome::kWait;   // it writes its settings back from memory

    std::vector<Folder> given;
    for (const auto& u : plan.grant) {
        Folder f;
        if (!uriFolder(u, &f) || !env.grant(f)) {
            ALOGW("hakux: %s was not given to hakuX", u.c_str());
            // The ones given so far are taken back: the settings, not written, don't list them as
            // Thimsy's, so nothing would take them back later. Those hakuX had before stay.
            for (const auto& g : given)
                if (!contains(prefsLines(prefs, kThimsyKey), folderUri(g))) env.revoke(g);
            return Outcome::kFailed;
        }
        given.push_back(f);
    }
    // Started while the folders were being given: its settings are its own again, and the ones just
    // given are taken back, as after a failure.
    if (env.running(kPkg)) {
        for (const auto& g : given)
            if (!contains(prefsLines(prefs, kThimsyKey), folderUri(g))) env.revoke(g);
        return Outcome::kWait;
    }
    if (!env.write(prefsPath, plan.prefs)) {
        ALOGW("hakux: %s could not be written", kPrefs);
        return Outcome::kFailed;
    }
    for (const auto& u : plan.revoke) {
        Folder f;
        if (!uriFolder(u, &f) || !env.revoke(f)) ALOGW("hakux: %s could not be taken back", u.c_str());
    }
    ALOGI("hakux: %zu games folder(s) of Game Settings, %zu given, %zu taken back", uris.size(), plan.grant.size(),
          plan.revoke.size());
    return Outcome::kDone;
}

}  // namespace nanohakux
}  // namespace android
