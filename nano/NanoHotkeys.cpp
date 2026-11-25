#include "NanoHotkeys.h"
#include "NanoAppFiles.h"
#include "NanoRaConfig.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <map>
#include <set>

#include <cutils/properties.h>
#include <utils/Log.h>

namespace android {
namespace nanohotkeys {
namespace {

const char* const kDirs[] = {"/system/etc/gammaos/hotkeys", "/data/system/gammaos/hotkeys"};
const char* const kBom = "\xEF\xBB\xBF";

// Nano's own files (profiles, /proc).
bool readFile(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "re");
    if (!f) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

// An emulator's settings file, which may sit in an app's own folder: nano runs as root, so the
// app must not be able to steer it elsewhere. The folder is opened once, a step at a time with no
// links followed, and everything after is done relative to that open folder; the file must be a
// plain file with one link and a sane size.
struct Target {
    std::string path;            // for the log
    std::string name;            // the file's name in the folder
    int dfd = -1;                // the folder
    struct stat dirSt = {};
    struct stat readSt = {};     // the file as read, to see whether the emulator wrote it meanwhile
    bool haveRead = false;
    ~Target() { if (dfd >= 0) close(dfd); }
};

constexpr off_t kMaxSettingsFile = 1 << 20;

// Opens the folder of `path` without following any link on the way. false when it does not exist
// (the emulator has not been set up) or is reached through a link.
bool openTarget(const std::string& path, Target* t) {
    const size_t slash = path.rfind('/');
    if (path.empty() || path[0] != '/' || slash == std::string::npos || slash + 1 >= path.size()) return false;
    t->path = path;
    t->name = path.substr(slash + 1);
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    size_t from = 1;
    while (fd >= 0 && from < slash) {
        size_t to = path.find('/', from);
        if (to == std::string::npos || to > slash) to = slash;
        const std::string part = path.substr(from, to - from);
        from = to + 1;
        if (part.empty() || part == ".") continue;
        const int next = part == ".." ? -1 : openat(fd, part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    if (fd < 0) return false;
    if (fstat(fd, &t->dirSt) != 0 || !S_ISDIR(t->dirSt.st_mode)) { close(fd); return false; }
    t->dfd = fd;
    return true;
}

bool readTarget(Target& t, std::string* out) {
    t.haveRead = false;
    const int fd = openat(t.dfd, t.name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size > kMaxSettingsFile) {
        ALOGW("hotkeys: %s is not a plain settings file; left alone", t.path.c_str());
        close(fd);
        return false;
    }
    out->clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out->append(buf, (size_t)n);
        if ((off_t)out->size() > kMaxSettingsFile) { close(fd); return false; }
    }
    close(fd);
    if (n < 0) return false;
    t.readSt = st;
    t.haveRead = true;
    return true;
}

// A profile value as text: a string, or a number or true/false written without quotes.
std::string valueText(const njson::Value& v) {
    if (v.isNumber()) {
        char buf[32];
        if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15) snprintf(buf, sizeof(buf), "%lld", (long long)v.num);
        else snprintf(buf, sizeof(buf), "%.17g", v.num);
        return buf;
    }
    if (v.isBool()) return v.b ? "true" : "false";
    return v.asString();
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string expandPath(std::string p) {
    const std::string var = "${sdcard}";
    size_t at = p.find(var);
    if (at != std::string::npos) p.replace(at, var.size(), "/storage/emulated/0");
    return p;
}

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) { out.push_back(s); return out; }
    size_t from = 0;
    for (;;) {
        size_t at = s.find(sep, from);
        out.push_back(trim(s.substr(from, at == std::string::npos ? std::string::npos : at - from)));
        if (at == std::string::npos) break;
        from = at + sep.size();
    }
    return out;
}

// The value a rule wants for a key that has `cur` (present: the key exists). Returns false to
// leave it. Modes: "add" (default) sets a missing or empty key; "append" also adds the value to a
// key bound to something else, joined with `sep`; "ifUnset" only sets a missing key; "set"
// always sets it.
bool wantValue(const std::string& mode, const std::string& sep, bool present, const std::string& cur,
               const std::string& want, std::string* out) {
    if (present && cur == want) return false;
    if (mode == "set") { *out = want; return true; }
    if (mode == "ifUnset") { if (present) return false; *out = want; return true; }
    if (!present || cur.empty()) { *out = want; return true; }
    if (mode == "append") {
        auto parts = split(cur, sep);
        std::string v = cur;
        for (const auto& w : split(want, sep))
            if (std::find(parts.begin(), parts.end(), w) == parts.end()) v += sep + w;
        if (v == cur) return false;
        *out = v;
        return true;
    }
    return false;
}

// Replaces the file through a temporary one in the same open folder. The new file belongs to the
// folder's owner, with the old file's permission bits (rw only; 0660 for a new file). `fresh`: a
// new file; otherwise nothing is written if the emulator changed the file since it was read.
bool writeLines(Target& t, const std::vector<std::string>& lines, bool fresh = false) {
    const bool haveBefore = !fresh && t.haveRead;
    if (!fresh && !haveBefore) return false;
    // One provisioning run at a time (provision's lock), so one fixed name, cleared first so a run
    // cut short leaves nothing behind, and created exclusively, never through a link. Not the name
    // RetroArch's folder move (NanoRaConfig.cpp) uses for the same config.
    const std::string tmp = t.name + ".nanohotkeys";
    unlinkat(t.dfd, tmp.c_str(), 0);
    const int fd = openat(t.dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* o = fdopen(fd, "w");
    if (!o) { close(fd); unlinkat(t.dfd, tmp.c_str(), 0); return false; }
    for (const auto& l : lines) fprintf(o, "%s\n", l.c_str());
    bool ok = fflush(o) == 0 && !ferror(o) && fsync(fileno(o)) == 0;
    struct stat mine;
    if (fstat(fileno(o), &mine) == 0) {
        if ((mine.st_uid != t.dirSt.st_uid || mine.st_gid != t.dirSt.st_gid)
            && fchown(fileno(o), t.dirSt.st_uid, t.dirSt.st_gid) != 0)
            ALOGW("hotkeys: chown %s failed", t.path.c_str());
        const mode_t mode = haveBefore ? (t.readSt.st_mode & 0666) : 0660;
        if ((mine.st_mode & 07777) != mode) fchmod(fileno(o), mode);
    }
    ok = fclose(o) == 0 && ok;
    struct stat now;
    if (ok && haveBefore && (fstatat(t.dfd, t.name.c_str(), &now, AT_SYMLINK_NOFOLLOW) != 0
                             || now.st_ino != t.readSt.st_ino || now.st_size != t.readSt.st_size
                             || now.st_mtim.tv_sec != t.readSt.st_mtim.tv_sec
                             || now.st_mtim.tv_nsec != t.readSt.st_mtim.tv_nsec)) {
        ALOGW("hotkeys: %s changed while binding keys; left for next time", t.path.c_str());
        ok = false;
    }
    if (ok && fresh && fstatat(t.dfd, t.name.c_str(), &now, AT_SYMLINK_NOFOLLOW) == 0) ok = false;   // appeared meanwhile
    if (!ok || renameat(t.dfd, tmp.c_str(), t.dfd, t.name.c_str()) != 0) {
        unlinkat(t.dfd, tmp.c_str(), 0);
        return false;
    }
    ALOGI("hotkeys: bound keys in %s", t.path.c_str());
    return true;
}

// The packages with a running process (an emulator still open, or still closing).
std::set<std::string> runningPackages() {
    std::set<std::string> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string cmd;
        if (!readFile(std::string("/proc/") + e->d_name + "/cmdline", &cmd) || cmd.empty()) continue;
        cmd = cmd.c_str();                        // the first argument: the process name
        out.insert(cmd.substr(0, cmd.find(':')));  // "com.app:service" belongs to com.app
    }
    closedir(d);
    return out;
}

std::vector<std::string> toLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t from = 0;
    while (from < text.size()) {
        size_t nl = text.find('\n', from);
        std::string l = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    return lines;
}

// Line-based settings files: "ini" ([section] then key = value) and "retroarch_cfg"
// (key = "value", no sections).
bool applyLines(Target& t, const njson::Value& rule) {
    std::string text;
    if (!readTarget(t, &text)) return false;
    const std::string format = rule.getString("format", "ini");
    const bool quoted = format == "retroarch_cfg";
    const std::string section = quoted ? "" : rule.getString("section");
    const std::string mode = rule.getString("mode", "add");
    const std::string sep = rule.getString("separator", ",");
    const std::string assign = rule.getString("assign", " = ");
    const njson::Value* values = rule.find("values");
    if (!values || !values->isObject()) return false;

    // A byte order mark (PPSSPP writes one) is kept, but not matched as part of the first line.
    const bool bom = text.compare(0, 3, kBom) == 0;
    std::vector<std::string> lines = toLines(bom ? text.substr(3) : text);
    // The section's lines: [begin, end). With no section, the lines before the first one.
    size_t begin = 0, end = lines.size();
    bool found = section.empty();
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = trim(lines[i]);
        if (t.size() < 2 || t.front() != '[' || t.back() != ']') continue;
        if (found) { end = i; break; }
        if (t.substr(1, t.size() - 2) == section) { found = true; begin = i + 1; }
    }
    if (!found) {
        lines.push_back("[" + section + "]");
        begin = end = lines.size();
    }
    bool changed = !found;
    for (const auto& kv : values->obj) {
        const std::string want = valueText(kv.second);
        bool present = false;
        size_t at = 0;
        std::string cur;
        for (size_t i = begin; i < end; i++) {
            size_t eq = lines[i].find('=');
            if (eq == std::string::npos || trim(lines[i].substr(0, eq)) != kv.first) continue;
            present = true; at = i;
            cur = trim(lines[i].substr(eq + 1));
            if (quoted && cur.size() >= 2 && cur.front() == '"' && cur.back() == '"') cur = cur.substr(1, cur.size() - 2);
            break;
        }
        std::string v;
        if (!wantValue(mode, sep, present, cur, want, &v)) continue;
        const std::string line = kv.first + assign + (quoted ? "\"" + v + "\"" : v);
        if (present) {
            lines[at] = line;
        } else {
            // After the section's last setting, so a blank line before the next section stays.
            size_t ins = end;
            while (ins > begin && trim(lines[ins - 1]).empty()) ins--;
            lines.insert(lines.begin() + ins, line);
            end++;
        }
        changed = true;
    }
    if (!changed) return true;
    if (bom) { if (lines.empty()) lines.push_back(""); lines[0] = kBom + lines[0]; }
    return writeLines(t, lines);
}

// Flycast controller mappings: "bindN = <keycode>:<action>" lines under [digital], numbered from 0
// without gaps. Each wanted binding not there yet goes on the next number.
bool applyFlycast(Target& t, const njson::Value& rule) {
    std::string text;
    if (!readTarget(t, &text)) return false;
    std::vector<std::string> lines = toLines(text);
    const njson::Value* binds = rule.find("binds");
    if (!binds || !binds->isArray()) return false;
    size_t sec = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = trim(lines[i]);
        if (t == "[digital]") sec = i;
        else if (sec < lines.size() && i > sec && !t.empty() && t.front() == '[') { end = i; break; }
    }
    if (sec == lines.size()) { lines.push_back("[digital]"); sec = lines.size() - 1; end = lines.size(); }
    std::vector<std::string> have;
    int next = 0;
    for (size_t i = sec + 1; i < end; i++) {
        size_t eq = lines[i].find('=');
        const std::string k = eq == std::string::npos ? "" : trim(lines[i].substr(0, eq));
        if (k.compare(0, 4, "bind") != 0) continue;
        have.push_back(trim(lines[i].substr(eq + 1)));
        next = std::max(next, atoi(k.c_str() + 4) + 1);
    }
    bool changed = false;
    for (const auto& b : binds->arr) {
        const std::string want = valueText(b);
        // The keycode is already bound (to this action or to another one the user chose): leave it.
        const std::string kc = want.substr(0, want.find(':') + 1);
        if (std::any_of(have.begin(), have.end(), [&](const std::string& h) { return h.compare(0, kc.size(), kc) == 0; }))
            continue;
        size_t ins = end;
        while (ins > sec + 1 && trim(lines[ins - 1]).empty()) ins--;
        lines.insert(lines.begin() + ins, "bind" + std::to_string(next++) + " = " + want);
        end++;
        have.push_back(want);
        changed = true;
    }
    return !changed || writeLines(t, lines);
}

// Android SharedPreferences XML: <int name="k" value="v" /> entries ("type": int, long, boolean)
// or <string name="k">v</string>.
bool applyXmlPrefs(Target& t, const njson::Value& rule) {
    std::string text;
    if (!readTarget(t, &text)) return false;
    const std::string type = rule.getString("type", "string");
    const std::string mode = rule.getString("mode", "add");
    const njson::Value* values = rule.find("values");
    if (!values || !values->isObject()) return false;
    std::vector<std::string> lines = toLines(text);
    bool changed = false;
    for (const auto& kv : values->obj) {
        const std::string want = xmlEscape(valueText(kv.second));
        const std::string nameAttr = "name=\"" + xmlEscape(kv.first) + "\"";
        bool present = false;
        size_t at = 0;
        std::string cur;
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].find(nameAttr) == std::string::npos) continue;
            present = true; at = i;
            size_t v = lines[i].find("value=\"");
            if (v != std::string::npos) {
                v += 7;
                cur = lines[i].substr(v, lines[i].find('"', v) - v);
            } else {
                size_t a = lines[i].find('>'), b = lines[i].find("</");
                if (a != std::string::npos && b != std::string::npos && b > a) cur = lines[i].substr(a + 1, b - a - 1);
            }
            break;
        }
        // An int keycode of 0 (KEYCODE_UNKNOWN) is unbound. A string set is only ever added.
        if (present && type == "int" && cur == "0") cur.clear();
        std::string v;
        if (type == "set") {
            if (present) continue;
            v = want;
        } else if (!wantValue(mode, ",", present, cur, want, &v)) {
            continue;
        }
        const std::string line = type == "string" ? "    <string " + nameAttr + ">" + v + "</string>"
            : type == "set" ? "    <set " + nameAttr + "><string>" + v + "</string></set>"
            : "    <" + type + " " + nameAttr + " value=\"" + v + "\" />";
        if (present) {
            lines[at] = line;
        } else {
            size_t end = lines.size();
            for (size_t i = 0; i < lines.size(); i++) if (lines[i].find("</map>") != std::string::npos) { end = i; break; }
            if (end == lines.size()) return false;
            lines.insert(lines.begin() + end, line);
        }
        changed = true;
    }
    return !changed || writeLines(t, lines);
}

// melonDS for Android (WatermelonDS) controller_config.json: {"inputMapper": [{"input": NAME,
// "assignment": {...}}]}. Each wanted input with no assignment gets its key, unless the key is
// already bound to something else.
bool applyMelonds(Target& t, const njson::Value& rule) {
    std::string text;
    njson::Value root;
    if (!readTarget(t, &text) || !njson::parse(text, &root) || !root.isObject()) return false;
    const njson::Value* binds = rule.find("binds");
    njson::Value* mapper = nullptr;
    for (auto& kv : root.obj) if (kv.first == "inputMapper") mapper = &kv.second;
    if (!binds || !binds->isObject() || !mapper || !mapper->isArray()) return false;
    auto keyOf = [](const njson::Value& e) {
        const njson::Value* a = e.find("assignment");
        return a && a->getString("type") == "key" ? a->getInt("keyCode", -1) : -1;
    };
    bool changed = false;
    for (const auto& kv : binds->obj) {
        const int kc = atoi(valueText(kv.second).c_str());
        if (std::any_of(mapper->arr.begin(), mapper->arr.end(), [&](const njson::Value& e) { return keyOf(e) == kc; }))
            continue;
        njson::Value* entry = nullptr;
        for (auto& e : mapper->arr) if (e.getString("input") == kv.first) entry = &e;
        if (entry) {
            const njson::Value* a = entry->find("assignment");
            if (a && a->getString("type", "none") != "none") continue;
        } else {
            mapper->arr.push_back(njson::Value::makeObject());
            entry = &mapper->arr.back();
            entry->set("input") = njson::Value::makeString(kv.first);
            njson::Value& alt = entry->set("altAssignment");
            alt = njson::Value::makeObject();
            alt.set("type") = njson::Value::makeString("none");
        }
        njson::Value a = njson::Value::makeObject();
        a.set("type") = njson::Value::makeString("key");
        a.set("deviceId") = njson::Value();
        a.set("keyCode") = njson::Value::makeNumber(kc);
        entry->set("assignment") = a;
        changed = true;
    }
    return !changed || writeLines(t, {njson::serialize(root, false)});
}

}  // namespace

std::vector<Profile> loadProfiles() {
    std::map<std::string, Profile> byId;
    for (const char* dir : kDirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            std::string text;
            Profile p;
            if (!readFile(std::string(dir) + "/" + n, &text) || !njson::parse(text, &p.json) || !p.json.isObject()) {
                ALOGW("hotkeys: bad profile %s/%s", dir, n.c_str());
                continue;
            }
            p.id = p.json.getString("id", n);
            p.name = p.json.getString("name", p.id);
            if (const njson::Value* pk = p.json.find("packages"))
                for (const auto& v : pk->arr) p.packages.push_back(v.asString());
            byId[p.id] = std::move(p);
        }
    }
    std::vector<Profile> out;
    for (auto& kv : byId) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

void provision(const std::string& skipPkg) {
    if (!property_get_bool("persist.gammaos.hotkeys.enable", true)) return;
    // One run at a time, across the home and overlay processes; a later one waits (runs are cheap
    // and change nothing the second time), as an emulator may have closed since the first began.
    // At most one waits in each process: a run that hangs does not pile up threads.
    static std::atomic<bool> sWaiting{false};
    if (sWaiting.exchange(true)) return;
    const int lock = open(nanora::kRaConfigLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    const bool locked = lock >= 0 && flock(lock, LOCK_EX) == 0;
    sWaiting = false;
    if (!locked) { if (lock >= 0) close(lock); return; }
    // Not an emulator that is open (or still closing): it writes its settings when it closes.
    std::set<std::string> skip = runningPackages();
    if (!skipPkg.empty()) skip.insert(skipPkg);
    char raDir[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaDirProp, raDir, "");
    const std::string raConfig = nanora::raLaunchConfigNow(raDir, nullptr);
    const MovedFolders moved = movedFolders();
    for (const auto& p : loadProfiles()) {
        if (std::any_of(p.packages.begin(), p.packages.end(), [&](const std::string& pk) { return skip.count(pk) > 0; }))
            continue;
        const njson::Value* rules = p.json.find("provision");
        if (!rules || !rules->isArray()) continue;
        for (const auto& rule : rules->arr)
            for (const auto& path : ruleFiles(relocatedFile(expandPath(rule.getString("file")), moved), raConfig)) {
                // Dolphin's moved folders are links to the card: written there, by the card's own path.
                const std::string real = throughMoveLinks(path, isCardEmulatorFolder);
                if (!real.empty()) provisionFile(real, rule);
            }
    }
    close(lock);   // releases the lock
}

MovedFolders movedFolders() {
    MovedFolders m;
    m.ppssppMemstick = ppssppMemstickAt(std::string());
    std::string prefs;
    m.flycastHome = flycastHomeAt(std::string(), &prefs);
    return m;
}

std::string relocatedFile(const std::string& path, const MovedFolders& moved) {
    auto under = [&](const std::string& dir) {
        return path.compare(0, dir.size(), dir) == 0 && (path.size() == dir.size() || path[dir.size()] == '/');
    };
    static const std::string kPsp = "/storage/emulated/0/PSP";
    std::string ms = moved.ppssppMemstick;
    while (ms.size() > 1 && ms.back() == '/') ms.pop_back();
    if (under(kPsp) && ms.size() > 1 && ms[0] == '/' && ms != "/storage/emulated/0") {
        // The memory stick holds PSP/, or is itself a folder named PSP.
        const size_t sl = ms.find_last_of('/');
        const bool isPsp = strcasecmp(ms.c_str() + sl + 1, "PSP") == 0;
        return (isPsp ? ms : ms + "/PSP") + path.substr(kPsp.size());
    }
    const std::string flyDef = appdef::kFlycastDefHome;
    std::string home = moved.flycastHome;
    while (home.size() > 1 && home.back() == '/') home.pop_back();
    if (under(flyDef) && home.size() > 1 && home[0] == '/' && home != flyDef) return home + path.substr(flyDef.size());
    return path;
}

bool isCardEmulatorFolder(const std::string& path) {
    std::vector<std::string> parts;
    for (size_t from = 1; from <= path.size();) {
        size_t to = path.find('/', from);
        if (to == std::string::npos) to = path.size();
        parts.push_back(path.substr(from, to - from));
        from = to + 1;
    }
    if (path.empty() || path[0] != '/' || parts.size() < 4) return false;
    for (const auto& part : parts) if (part.empty() || part == "." || part == "..") return false;
    return parts[0] == "storage" && parts[1] != "emulated" && parts[1] != "self" && parts[2] == "emulator_config";
}

std::string throughMoveLinks(const std::string& path, const std::function<bool(const std::string&)>& allowed) {
    std::string p = path;
    for (int hops = 0; hops < 4; hops++) {
        bool linked = false;
        // Each folder on the way, from the top (the file itself is never a link: provisionFile refuses one).
        for (size_t at = p.find('/', 1); at != std::string::npos; at = p.find('/', at + 1)) {
            const std::string dir = p.substr(0, at);
            struct stat st;
            if (lstat(dir.c_str(), &st) != 0) return p;   // not there: provisionFile finds nothing to do
            if (!S_ISLNK(st.st_mode)) continue;
            char buf[PATH_MAX];
            const ssize_t n = readlink(dir.c_str(), buf, sizeof(buf) - 1);
            if (n <= 0) return std::string();
            std::string target(buf, (size_t)n);
            while (target.size() > 1 && target.back() == '/') target.pop_back();
            if (target.empty() || target[0] != '/' || !allowed(target)) {
                ALOGW("hotkeys: %s goes through a link to %s; left alone", path.c_str(), target.c_str());
                return std::string();
            }
            p = target + p.substr(at);
            linked = true;
            break;
        }
        if (!linked) return p;
    }
    return std::string();
}

std::vector<std::string> ruleFiles(const std::string& path, const std::string& raConfig) {
    std::vector<std::string> out{path};
    const std::string def = std::string(nanora::kRaDefCfgDir) + "/retroarch.cfg";
    if (path == def && !raConfig.empty() && raConfig != def) out.push_back(raConfig);
    return out;
}

void provisionFile(const std::string& path, const njson::Value& rule) {
    Target t;
    if (!openTarget(path, &t)) return;   // not set up yet, or reached through a link
    // A missing file is the emulator's to create, unless the rule brings a starting file
    // (the emulator's folder exists, so it has been set up).
    struct stat st;
    if (fstatat(t.dfd, t.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const std::string tmpl = rule.getString("template");
        if (tmpl.empty() || errno != ENOENT || !writeLines(t, toLines(tmpl), true)) return;
    }
    const std::string format = rule.getString("format", "ini");
    if (format == "ini" || format == "retroarch_cfg") applyLines(t, rule);
    else if (format == "flycast_mapping") applyFlycast(t, rule);
    else if (format == "xml_prefs") applyXmlPrefs(t, rule);
    else if (format == "melonds_mapper") applyMelonds(t, rule);
    else ALOGW("hotkeys: %s: unknown provision format %s", path.c_str(), format.c_str());
}

}  // namespace nanohotkeys
}  // namespace android
