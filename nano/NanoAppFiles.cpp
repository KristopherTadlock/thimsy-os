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

#include "NanoAppFiles.h"
#include "NanoBiosUtil.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace android {

using nanobios::canonical;
using nanobios::pkgAfter;
using nanobios::rawView;
using nanobios::under;

std::string sdCanon(const std::string& p) {
    std::string c = rawView(canonical(rawView(p)));
    while (c.size() > 1 && c.back() == '/') c.pop_back();
    return c;
}

bool sdUnder(const std::string& a, const std::string& b) {
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/');
}

bool sdTooBroad(const std::string& src, bool esdeItself) {
    const std::string c = sdCanon(src);
    auto isRoot = [](const std::string& d) {
        return d == "/data/media/0" || (d.compare(0, 14, "/mnt/media_rw/") == 0 && d.find('/', 14) == std::string::npos);
    };
    if (isRoot(c) || c == "/data/media" || c == "/mnt/media_rw" || c == "/" || c == "/data") return true;
    const size_t sl = c.find_last_of('/');
    const std::string parent = c.substr(0, sl);
    std::string base = c.substr(sl + 1);
    for (auto& ch : base) ch = (char)tolower((unsigned char)ch);
    if (isRoot(parent)) {
        static const char* const kStd[] = {"download", "dcim", "pictures", "music", "movies", "documents", "android",
                                           "roms", "alarms", "notifications", "podcasts", "ringtones", "recordings",
                                           "audiobooks", "bios"};
        for (const char* s : kStd) if (base == s) return true;
    }
    if (parent.size() >= 8 && parent.compare(parent.size() - 8, 8, "/Android") == 0
        && (base == "data" || base == "media" || base == "obb"))
        return true;
    if (sdUnder(sdCanon(appdef::kBackupRoot), c)) return true;
    if (!esdeItself && sdUnder(sdCanon(appdef::kEsdeInternal), c)) return true;
    return false;
}

std::string sdSourceRoot(const std::string& src, const std::string& pkg, const std::string& root) {
    const std::string c = sdCanon(src);
    if (!nanobios::safePath(c)) return std::string();
    const std::string media = root + "/data/media/0", ext = media + "/Android/data", obb = media + "/Android/obb";
    const std::string card = root + "/mnt/media_rw", data = root + "/data/data";
    if (under(c, ext)) {   // only the emulator's own
        const std::string own = ext + "/" + pkg;
        return !pkg.empty() && pkgAfter(c, ext) == pkg && under(c, own) ? own : std::string();
    }
    if (c == ext || c == obb || under(c, obb)) return std::string();
    if (under(c, media)) return media;
    if (under(c, card)) {
        const std::string vol = card + "/" + pkgAfter(c, card);
        if (!under(c, vol)) return std::string();
        // A card's Android/data and Android/obb as internal storage's (any case: a card is FAT).
        std::vector<std::string> parts;   // the path under the volume, by folder
        for (size_t a = vol.size() + 1; a <= c.size() && parts.size() < 4;) {
            const size_t b = std::min(c.find('/', a), c.size());
            parts.push_back(c.substr(a, b - a));
            a = b + 1;
        }
        if (strcasecmp(parts[0].c_str(), "android") == 0 && parts.size() >= 2) {
            if (strcasecmp(parts[1].c_str(), "obb") == 0) return std::string();
            if (strcasecmp(parts[1].c_str(), "data") == 0) {   // only the emulator's own, inside it
                if (pkg.empty() || parts.size() < 4 || parts[2] != pkg) return std::string();
                return vol + "/" + parts[0] + "/" + parts[1] + "/" + parts[2];
            }
        }
        return vol;
    }
    if (!pkg.empty() && under(c, data + "/" + pkg)) return data + "/" + pkg;
    return std::string();
}

std::string sdPrefsGet(const std::string& prefs, const std::string& name) {
    const std::string key = "<string name=\"" + name + "\">";
    const size_t a = prefs.find(key);
    if (a == std::string::npos) return std::string();
    const size_t b = prefs.find("</string>", a);
    if (b == std::string::npos) return std::string();
    std::string v = prefs.substr(a + key.size(), b - a - key.size());
    for (const auto& e : {std::make_pair("&lt;", "<"), std::make_pair("&gt;", ">"), std::make_pair("&quot;", "\""),
                          std::make_pair("&apos;", "'"), std::make_pair("&amp;", "&")})   // &amp; last
        for (size_t p = v.find(e.first); p != std::string::npos; p = v.find(e.first, p + 1)) v.replace(p, strlen(e.first), e.second);
    return v;
}

// A file never written, or an empty <map />, made one.
bool sdPrefsSet(std::string* prefs, const std::string& name, const std::string& value) {
    if (prefs->find("<map") == std::string::npos)
        *prefs = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n</map>\n";
    const size_t empty = prefs->find("<map />");
    if (empty != std::string::npos) prefs->replace(empty, 7, "<map>\n</map>");
    std::string esc;
    for (char c : value) esc += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
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

void sdPrefsRemove(std::string* prefs, const std::string& name) {
    const size_t a = prefs->find("<string name=\"" + name + "\">");
    if (a == std::string::npos) return;
    const size_t b = prefs->find("</string>", a);
    if (b == std::string::npos) return;
    size_t from = a, to = b + 9;
    while (from > 0 && ((*prefs)[from - 1] == ' ' || (*prefs)[from - 1] == '\t')) from--;
    if (to < prefs->size() && (*prefs)[to] == '\n') to++;
    prefs->erase(from, to - from);
}

int sdPrefsGetInt(const std::string& prefs, const std::string& name, int def) {
    const std::string key = "<int name=\"" + name + "\" value=\"";
    const size_t a = prefs.find(key);
    if (a == std::string::npos) return def;
    const char* v = prefs.c_str() + a + key.size();
    char* end = nullptr;
    const long n = strtol(v, &end, 10);
    return end != v && *end == '"' && n >= INT32_MIN && n <= INT32_MAX ? (int)n : def;
}

bool sdPrefsSetInt(std::string* prefs, const std::string& name, int value) {
    if (prefs->find("<map") == std::string::npos)
        *prefs = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n</map>\n";
    const size_t empty = prefs->find("<map />");
    if (empty != std::string::npos) prefs->replace(empty, 7, "<map>\n</map>");
    const std::string tag = "<int name=\"" + name + "\" value=\"" + std::to_string(value) + "\" />";
    const size_t a = prefs->find("<int name=\"" + name + "\"");
    if (a != std::string::npos) {
        const size_t b = prefs->find("/>", a);
        if (b == std::string::npos) return false;
        prefs->replace(a, b + 2 - a, tag);
        return true;
    }
    const size_t end = prefs->rfind("</map>");
    if (end == std::string::npos) return false;
    prefs->insert(end, "    " + tag + "\n");
    return true;
}

std::string sdFileUri(const std::string& path) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string u = "file://";
    for (unsigned char c : path) {
        if (isalnum(c) || strchr("/-_.~", c)) u += (char)c;
        else { u += '%'; u += kHex[c >> 4]; u += kHex[c & 15]; }
    }
    return u;
}

std::string sdUriDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string sdTrimDir(std::string d) {
    while (!d.empty() && (d.back() == ' ' || d.back() == '\r' || d.back() == '\t')) d.pop_back();
    while (!d.empty() && (d[0] == ' ' || d[0] == '\t')) d.erase(0, 1);
    if (d.size() >= 2 && (d[0] == '"' || d[0] == '\'') && d.back() == d[0]) d = d.substr(1, d.size() - 2);
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    if (d == "/sdcard" || d.compare(0, 8, "/sdcard/") == 0) d = "/storage/emulated/0" + d.substr(7);
    if (d.compare(0, 13, "/data/user/0/") == 0) d = "/data/data/" + d.substr(13);
    return d;
}

bool sdIniKey(const std::string& line, const char* key, std::string* value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string k = line.substr(0, eq);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    while (!k.empty() && (k[0] == ' ' || k[0] == '\t')) k.erase(0, 1);
    if (strcasecmp(k.c_str(), key) != 0) return false;
    *value = sdTrimDir(line.substr(eq + 1));
    return true;
}

std::string flycastHomeFrom(const std::string& prefs) {
    const std::string key = "<string name=\"home_directory\">";
    const size_t a = prefs.find(key);
    if (a == std::string::npos) return appdef::kFlycastDefHome;
    const size_t b = prefs.find("</string>", a);
    std::string v = prefs.substr(a + key.size(), b == std::string::npos ? std::string::npos : b - a - key.size());
    while (v.size() > 1 && v.back() == '/') v.pop_back();
    return v.empty() || v[0] != '/' ? std::string() : v;
}

// "/fp<length of the parent><parent>/<name>" for a folder ("/ss..." is one picked through Android,
// a content:// address: "" then).
std::string drasticSystemDirFrom(const std::string& prefs) {
    const std::string v = sdPrefsGet(prefs, "_SystemPath");
    if (v.empty()) return appdef::kDrasticDefDir;
    if (v.compare(0, 3, "/fp") != 0) return std::string();
    size_t i = 3, n = 0;
    while (i < v.size() && isdigit((unsigned char)v[i])) {
        n = n * 10 + (size_t)(v[i++] - '0');
        if (n > v.size()) return std::string();   // longer than the value itself (and never wraps)
    }
    if (i == 3 || n > v.size() - i) return std::string();
    std::string dir = v.substr(i, n) + v.substr(i + n);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return !dir.empty() && dir[0] == '/' ? dir : std::string();
}

std::string drasticSystemPathValue(const std::string& dir) {
    const size_t sl = dir.find_last_of('/');
    const std::string parent = dir.substr(0, sl);
    return "/fp" + std::to_string(parent.size()) + parent + dir.substr(sl);
}

// M64Plus FZ keeps game data in its app folder and, set to external storage, copies each game's
// data from and back to <folder>/GameData around play. The folder: a file:// address ("" = a
// content:// one picked through Android, or internal storage when *internal).
std::string mupenDataDirFrom(const std::string& prefs, bool* internal) {
    const std::string type = sdPrefsGet(prefs, "gameDataStorageType");
    std::string path = sdPrefsGet(prefs, "gameDataStoragePath");
    *internal = type == "internal";
    if (*internal) return std::string();
    if (path.empty()) return appdef::kMupenDefDir;   // its default (made at its next start)
    if (path.compare(0, 7, "file://") != 0) return std::string();
    std::string dir = sdUriDecode(path.substr(7));
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return !dir.empty() && dir[0] == '/' ? dir : std::string();
}

std::string armsx2SystemDirFrom(const std::string& prefs, bool* chosen) {
    const std::string v = sdTrimDir(sdPrefsGet(prefs, "systemDir"));
    *chosen = !v.empty();
    return *chosen ? v : std::string(appdef::kArmsx2DefDir);
}

std::string ppssppMemstickFrom(const std::string& text, bool* set) {
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    *set = !t.empty();
    return *set && t[0] == '/' ? t : std::string();
}

std::string vita3kPrefPath(const std::string& cfg) {
    size_t pos = 0;
    while (pos < cfg.size()) {
        size_t end = cfg.find('\n', pos);
        if (end == std::string::npos) end = cfg.size();
        const std::string line = cfg.substr(pos, end - pos);
        if (line.compare(0, 10, "pref-path:") == 0) return sdTrimDir(line.substr(10));
        pos = end + 1;
    }
    return std::string();
}

std::string vita3kSetPrefPath(const std::string& cfg, const std::string& dir) {
    const std::string line = "pref-path: " + dir + "/";
    std::string out;
    bool done = false;
    size_t pos = 0;
    while (pos < cfg.size()) {
        size_t end = cfg.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) end = cfg.size();
        std::string l = cfg.substr(pos, end - pos);
        if (l.compare(0, 10, "pref-path:") == 0) { l = line; done = true; }
        else if (!done && (l == "..." || l == "...\r")) { out += line + "\n"; done = true; }   // before the end of the document
        out += l;
        if (!last) out += "\n";
        pos = end + 1;
    }
    if (!done) out += std::string(out.empty() || out.back() == '\n' ? "" : "\n") + line + "\n";
    return out;
}

// savepath in [scummvm] of its scummvm.ini, else its app folder's saves. "" when it is a folder
// picked through Android (/saf/...).
std::string scummvmSavePath(const std::string& ini) {
    std::string section, value;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        const std::string line = ini.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        if (strcasecmp(section.c_str(), "scummvm") == 0 && sdIniKey(line, "savepath", &value) && !value.empty())
            return value.compare(0, 5, "/saf/") == 0 || value[0] != '/' ? std::string() : value;
    }
    return appdef::kScummvmDefSaves;
}

// savepath in [scummvm] set to dir, and a game's own savepath in the old folder moved with it.
std::string scummvmSetSavePath(const std::string& ini, const std::string& from, const std::string& dir) {
    std::string out, section, value;
    bool inMain = false, done = false, haveMain = false;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) end = ini.size();
        std::string line = ini.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line[0] == '[') {
            if (inMain && !done) { out += "savepath=" + dir + "\n"; done = true; }
            section = line.substr(1, line.find(']') - 1);
            inMain = strcasecmp(section.c_str(), "scummvm") == 0;
            haveMain = haveMain || inMain;
        } else if (sdIniKey(line, "savepath", &value)) {
            if (inMain) { line = "savepath=" + dir; done = true; }
            else if (sdUnder(value, from)) line = "savepath=" + dir + value.substr(from.size());
        }
        out += line;
        if (!last) out += "\n";
    }
    if (inMain && !done) out += std::string(out.empty() || out.back() == '\n' ? "" : "\n") + "savepath=" + dir + "\n";
    if (!haveMain) out = "[scummvm]\nsavepath=" + dir + "\n" + out;
    return out;
}

// A folder picked through Android: a tree of the external storage provider, primary:<path> on
// internal storage, <volume>:<path> on a card. "" for any other address.
static std::string storageTreeDir(const std::string& uri) {
    const size_t n = strlen(appdef::kExternalStorageTree);
    if (uri.compare(0, n, appdef::kExternalStorageTree) != 0) return std::string();
    std::string id = uri.substr(n);
    const size_t sl = id.find('/');   // .../tree/<id>/document/<id>
    id = sdUriDecode(sl == std::string::npos ? id : id.substr(0, sl));
    const size_t colon = id.find(':');
    if (colon == std::string::npos || colon == 0) return std::string();
    const std::string vol = id.substr(0, colon), rel = id.substr(colon + 1);
    const std::string root = vol == "primary" ? std::string("/storage/emulated/0") : "/storage/" + vol;
    return sdTrimDir(rel.empty() ? root : root + "/" + rel);
}

// CITRA_DIRECTORY, a folder picked through Android.
std::string azaharDir(const std::string& prefs, std::string* uri) {
    *uri = sdPrefsGet(prefs, "CITRA_DIRECTORY");
    return storageTreeDir(*uri);
}

std::string azaharTreeUri(const std::string& volume, const std::string& rel) {   // as Android writes it
    static const char kHex[] = "0123456789ABCDEF";
    std::string u = appdef::kExternalStorageTree;
    for (unsigned char c : volume + ":" + rel) {
        if (isalnum(c) || strchr("_-!.~'()*", c)) u += (char)c;
        else { u += '%'; u += kHex[c >> 4]; u += kHex[c & 15]; }
    }
    return u;
}

std::string seedlessSaveDir(const std::string& prefs, int* location, std::string* uri) {
    *location = sdPrefsGetInt(prefs, "_SaveLocation", kSeedlessSaveBesideGames);
    uri->clear();
    if (*location == kSeedlessSaveApp) return appdef::kSeedlessUserDir;
    if (*location != kSeedlessSaveFolder) return std::string();
    *uri = sdPrefsGet(prefs, "_SaveFolder");
    return storageTreeDir(*uri);
}

// A profile's folder: "p" and the hash of the profile's name.
static bool seedlessProfileDir(const std::string& n) {
    const size_t i = n.compare(0, 2, "p-") == 0 ? 2 : n.compare(0, 1, "p") == 0 ? 1 : 0;
    return i > 0 && i < n.size() && std::all_of(n.begin() + i, n.end(), [](char c) { return isdigit((unsigned char)c); });
}

static bool seedlessSavesIn(const std::string& dir, bool top) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool ok = true;
    while (ok) {
        const dirent* e = readdir(d);
        if (!e) break;
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        if (lstat((dir + "/" + n).c_str(), &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            ok = top && seedlessProfileDir(n) && seedlessSavesIn(dir + "/" + n, false);
            continue;
        }
        const size_t dot = n.find_last_of('.');
        const char* ext = dot == std::string::npos ? "" : n.c_str() + dot + 1;
        ok = S_ISREG(st.st_mode) && (!strcasecmp(ext, "dsv") || !strcasecmp(ext, "sav") || !strcasecmp(ext, "dss")
                                     || !strcasecmp(ext, "ra"));
    }
    closedir(d);
    return ok;
}

bool seedlessSavesOnly(const std::string& dir) { return seedlessSavesIn(dir, true); }

namespace {

std::vector<std::string> textLines(const std::string& text) {
    std::vector<std::string> lines;
    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return lines;   // a final newline leaves an empty last line, so join() gives the text back
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? "\n" : "") + lines[i];
    return out;
}

// key = value (spaces around '=' kept as they were); false for a section, comment or other line.
bool iniParts(const std::string& line, std::string* key, std::string* value, size_t* valueAt) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '[' || line[0] == '#' || line[0] == ';') return false;
    *key = line.substr(0, eq);
    while (!key->empty() && (key->back() == ' ' || key->back() == '\t')) key->pop_back();
    while (!key->empty() && ((*key)[0] == ' ' || (*key)[0] == '\t')) key->erase(0, 1);
    *valueAt = line.find_first_not_of(" \t", eq + 1);
    if (*valueAt == std::string::npos) *valueAt = line.size();
    *value = line.substr(*valueAt);
    while (!value->empty() && (value->back() == '\r' || value->back() == ' ' || value->back() == '\t')) value->pop_back();
    return true;
}

// dir moved by map ("" when no entry has it); the folder keeps its trailing slash.
std::string movedPath(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& map) {
    const bool slash = dir.size() > 1 && dir.back() == '/';
    const std::string d = sdTrimDir(dir);
    for (const auto& m : map) {
        if (m.first.empty() || !(d == m.first || d.compare(0, m.first.size() + 1, m.first + "/") == 0)) continue;
        return m.second + d.substr(m.first.size()) + (slash ? "/" : "");
    }
    return std::string();
}

}  // namespace

std::string flycastRewriteCfg(const std::string& cfg, const std::vector<std::pair<std::string, std::string>>& map) {
    std::vector<std::string> lines = textLines(cfg);
    bool changed = false;
    for (auto& line : lines) {
        std::string key, value;
        size_t at;
        if (!iniParts(line, &key, &value, &at)) continue;
        if (key.compare(0, 10, "Dreamcast.") != 0 || key.size() < 14 || key.compare(key.size() - 4, 4, "Path") != 0) continue;
        if (value.empty() || value[0] == '"') continue;   // a quoted list (a folder with ';' in it) is left as it is
        std::string out;
        bool any = false;
        for (size_t pos = 0; pos <= value.size();) {
            size_t end = value.find(';', pos);
            if (end == std::string::npos) end = value.size();
            std::string part = value.substr(pos, end - pos);
            const std::string moved = movedPath(part, map);
            if (!moved.empty()) { part = moved; any = true; }
            out += (pos ? ";" : "") + part;
            pos = end + 1;
        }
        if (any) {
            line = line.substr(0, at) + out + line.substr(at + value.size());
            changed = true;
        }
    }
    return changed ? joinLines(lines) : cfg;
}

std::string armsx2RewriteIni(const std::string& ini, const std::string& from, const std::string& to, const std::string& bios) {
    static const char* const kStay[] = {"bios", "cache", "resources", "logs"};
    std::vector<std::string> lines = textLines(ini);
    bool changed = false, inFolders = false;
    for (auto& line : lines) {
        if (!line.empty() && line[0] == '[') {
            inFolders = strncasecmp(line.c_str(), "[Folders]", 9) == 0;
            continue;
        }
        std::string key, value;
        size_t at;
        if (!inFolders || !iniParts(line, &key, &value, &at)) continue;
        const std::string d = sdTrimDir(value);
        const bool underFrom = !d.empty() && (d == from || d.compare(0, from.size() + 1, from + "/") == 0);
        std::string nv;
        if (strcasecmp(key.c_str(), "Bios") == 0) {
            if (d.empty() || d[0] != '/' || underFrom) nv = bios;
        } else if (underFrom) {
            const std::string rest = d.substr(from.size());
            const std::string top = rest.empty() ? rest : rest.substr(1, rest.find('/', 1) - 1);   // its first folder
            bool stays = false;
            for (const char* s : kStay) stays = stays || strcasecmp(top.c_str(), s) == 0;
            if (!stays) nv = to + rest;
        }
        if (nv.empty() || nv == value) continue;
        line = line.substr(0, at) + nv + line.substr(at + value.size());
        changed = true;
    }
    return changed ? joinLines(lines) : ini;
}

// memstick_dir.txt in its app folder, else the root of internal storage.
std::string ppssppMemstickAt(const std::string& root) {
    std::string text;
    if (nanobios::appFileRead(root + appdef::kPpssppMemstickFile, &text, root)) {
        bool set = false;
        const std::string dir = ppssppMemstickFrom(text, &set);
        if (set) return dir;
    }
    // Not set: PPSSPP's default, its app folder in newer versions, the storage root in older ones;
    // of two, the one it used last (its settings file written most recently).
    const std::string app = "/storage/emulated/0/Android/data/org.ppsspp.ppsspp/files";
    auto used = [&](const std::string& raw) {
        struct stat st;
        if (stat((root + raw + "/PSP/SYSTEM/ppsspp.ini").c_str(), &st) == 0) return (long long)st.st_mtime;
        return access((root + raw + "/PSP").c_str(), F_OK) == 0 ? 0LL : -1LL;
    };
    const long long a = used("/data/media/0/Android/data/org.ppsspp.ppsspp/files"), r = used("/data/media/0");
    return r > a ? std::string("/storage/emulated/0") : app;
}

std::string flycastHomeAt(const std::string& root, std::string* prefs) {
    if (!nanobios::appPrefsRead(root + appdef::kFlycastPrefs, prefs, root)) return appdef::kFlycastDefHome;
    return flycastHomeFrom(*prefs);
}

std::string drasticSystemDirAt(const std::string& root) {
    std::string prefs;
    if (!nanobios::appPrefsRead(root + appdef::kDrasticFsPrefs, &prefs, root)) return appdef::kDrasticDefDir;
    return drasticSystemDirFrom(prefs);
}

}  // namespace android
