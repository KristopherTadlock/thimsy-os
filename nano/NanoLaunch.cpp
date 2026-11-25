/*
 * Copyright (C) 2026 GammaOS
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

#include "NanoLaunch.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nanolaunch {

std::string shellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static void pctAppend(std::string& out, unsigned char c) {
    static const char hex[] = "0123456789ABCDEF";
    out += '%';
    out += hex[c >> 4];
    out += hex[c & 0x0F];
}

std::string urlEncode(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (unreserved(c)) out += (char)c;
        else pctAppend(out, c);
    }
    return out;
}

std::string fileUri(const std::string& path) {
    std::string out = "file://";
    for (unsigned char c : path) {
        if (c == '/' || unreserved(c)) out += (char)c;
        else pctAppend(out, c);
    }
    return out;
}

static bool startsWith(const std::string& s, const char* p) {
    return s.compare(0, strlen(p), p) == 0;
}

std::string appVisiblePath(const std::string& path) {
    if (startsWith(path, "/data/media/0/")) return "/sdcard/" + path.substr(14);
    if (startsWith(path, "/mnt/media_rw/")) return "/storage/" + path.substr(14);
    return path;
}

void safTree(const std::string& fullRomPath, const std::string& fallbackRomDir,
             std::string& treeRoot, std::string& encodedFilename) {
    std::string filename = fullRomPath;
    { size_t ls = filename.rfind('/');
      if (ls != std::string::npos) filename = filename.substr(ls + 1); }
    encodedFilename = urlEncode(filename);

    std::string volumeId = "primary";
    std::string relDirPath;   // directory relative to the volume root, '/'-joined
    std::string work;
    bool external = false;
    bool haveDir = false;
    if (startsWith(fullRomPath, "/data/media/0/")) {
        work = fullRomPath.substr(strlen("/data/media/0/"));
    } else if (startsWith(fullRomPath, "/sdcard/")) {
        work = fullRomPath.substr(strlen("/sdcard/"));
    } else if (startsWith(fullRomPath, "/storage/emulated/0/")) {
        work = fullRomPath.substr(strlen("/storage/emulated/0/"));
    } else if (startsWith(fullRomPath, "/mnt/media_rw/")) {
        work = fullRomPath.substr(strlen("/mnt/media_rw/")); external = true;
    } else if (startsWith(fullRomPath, "/storage/")) {
        work = fullRomPath.substr(strlen("/storage/")); external = true;
    } else if (!fallbackRomDir.empty()) {
        relDirPath = "ROMs/" + fallbackRomDir; haveDir = true;
    } else {
        size_t ls = fullRomPath.rfind('/');
        relDirPath = (ls != std::string::npos && ls > 0) ? fullRomPath.substr(1, ls - 1) : "";
        haveDir = true;
    }
    if (external) {
        // <volume>/<dirs...>/<file>
        size_t sl1 = work.find('/');
        if (sl1 != std::string::npos) {
            volumeId = work.substr(0, sl1);
            size_t lastSl = work.rfind('/');
            relDirPath = (lastSl > sl1) ? work.substr(sl1 + 1, lastSl - sl1 - 1) : "";
        }
    } else if (!haveDir) {
        size_t lastSl = work.rfind('/');
        relDirPath = (lastSl != std::string::npos) ? work.substr(0, lastSl) : "";
    }

    // Each directory segment is encoded on its own and joined with %2F, which is how the
    // provider's document id ("primary:ROMs/nes") appears inside the URI.
    std::string relDir;
    size_t pos = 0;
    while (pos <= relDirPath.size()) {
        size_t sl = relDirPath.find('/', pos);
        std::string seg = relDirPath.substr(pos, (sl == std::string::npos ? relDirPath.size() : sl) - pos);
        if (!seg.empty()) {
            if (!relDir.empty()) relDir += "%2F";
            relDir += urlEncode(seg);
        }
        if (sl == std::string::npos) break;
        pos = sl + 1;
    }
    treeRoot = urlEncode(volumeId) + "%3A" + relDir;
}

std::string safContentUri(const std::string& fullRomPath, const std::string& fallbackRomDir) {
    std::string treeRoot, file;
    safTree(fullRomPath, fallbackRomDir, treeRoot, file);
    return "content://com.android.externalstorage.documents/tree/" + treeRoot + "/document/" +
           treeRoot + "%2F" + file;
}

bool tokenSafe(const std::string& v) {
    for (char c : v)
        if (c == '\t' || c == '\n' || c == '\r' || c == '\0') return false;
    return true;
}

static bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string launchFileId(const std::string& text) {
    size_t end = text.find_first_of("\r\n");
    if (end == std::string::npos) end = text.size();
    size_t b = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0, e = end;   // a UTF-8 BOM (Notepad)
    while (b < e && isSpace(text[b])) b++;
    while (e > b && isSpace(text[e - 1])) e--;
    // A leading '-' would reach ScummVM's command line as an option.
    if (e == b || e - b > 64 || text[b] == '-') return "";
    for (size_t i = b; i < e; i++) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return "";
    }
    return text.substr(b, e - b);
}

std::string readLaunchFileId(const std::string& path) {
    // O_NONBLOCK: a FIFO in place of the file never holds up the launch.
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return "";
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return ""; }
    char buf[4096];
    size_t got = 0;
    while (got < sizeof(buf)) {
        const ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return launchFileId(std::string(buf, got));
}

bool usesFileContent(const std::string& tmpl, const std::string& extraArgs) {
    return tmpl.find("{file.content}") != std::string::npos ||
           extraArgs.find("{file.content}") != std::string::npos;
}

std::string storagePath(const std::string& path) {
    if (startsWith(path, "/sdcard/")) return "/storage/emulated/0/" + path.substr(8);
    if (startsWith(path, "/data/media/0/")) return "/storage/emulated/0/" + path.substr(14);
    if (startsWith(path, "/mnt/media_rw/")) return "/storage/" + path.substr(14);
    return path;
}

namespace {

struct TemplateToken {
    std::string text;
    bool quoted = false;   // a "..." token: an argument string the app splits again
};

// A quoted token's spaces separate arguments one at a time, and each '...' span is a whole
// argument: MAME4droid starts a span only at an argument's start, and steps over the character
// after its closing quote, which must therefore be the space before the next argument.
bool quotedTokenWellFormed(const std::string& g) {
    if (g.empty()) return false;
    bool inSpan = false;
    for (size_t i = 0; i < g.size(); i++) {
        const char c = g[i];
        if (c == '\'') {
            if (!inSpan && i > 0 && g[i - 1] != ' ') return false;
            if (inSpan && i + 1 < g.size() && g[i + 1] != ' ') return false;
            inSpan = !inSpan;
        } else if (!inSpan && isSpace(c)) {
            if (c != ' ' || i == 0 || i + 1 == g.size() || g[i + 1] == ' ') return false;
        }
    }
    return !inSpan;
}

bool splitTemplate(const std::string& s, std::vector<TemplateToken>& out) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) i++;
        if (i >= s.size()) break;
        TemplateToken t;
        if (s[i] == '"') {
            t.quoted = true;
            bool closed = false;
            for (i++; i < s.size(); i++) {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '"') { t.text += '"'; i++; continue; }
                if (s[i] == '"') { closed = true; i++; break; }
                t.text += s[i];
            }
            if (!closed || (i < s.size() && !isSpace(s[i])) || !quotedTokenWellFormed(t.text)) return false;
        } else {
            const size_t start = i;
            while (i < s.size() && !isSpace(s[i])) i++;
            t.text = s.substr(start, i - start);
        }
        out.push_back(std::move(t));
    }
    return true;
}

std::string parentDir(const std::string& path) {
    const size_t sl = path.rfind('/');
    if (sl == std::string::npos) return "";
    return sl == 0 ? std::string("/") : path.substr(0, sl);
}

}  // namespace

bool buildLaunchIntent(const std::string& tmpl, const std::string& extraArgs,
                       const LaunchVars& vars, std::vector<std::string>& tokens) {
    tokens.clear();
    // An empty ID would start the app on no game ("-d" with nothing after it takes the next option).
    if (vars.fileContent.empty() && usesFileContent(tmpl, extraArgs)) return false;
    std::vector<TemplateToken> parts;
    if (!splitTemplate(tmpl, parts) || !splitTemplate(extraArgs, parts)) return false;
    const std::string sPath = storagePath(vars.filePath), sDir = parentDir(sPath);
    const struct { const char* name; const std::string* value; } kVars[] = {
        {"{file.uri}", &vars.fileUri}, {"{file.path}", &vars.filePath}, {"{file.mime}", &vars.fileMime},
        {"{file.content}", &vars.fileContent}, {"{file.storagepath}", &sPath}, {"{file.storagedir}", &sDir},
    };
    // One pass per placeholder over the template's own text: a value that happens to contain
    // another placeholder (a file named "{file.uri}.iso") is left as it is.
    for (const TemplateToken& p : parts) {
        const std::string& t = p.text;
        std::string out;
        bool inSpan = false;
        size_t i = 0;
        while (i < t.size()) {
            if (t[i] == '{') {
                const std::string* value = nullptr;
                size_t len = 0;
                for (const auto& v : kVars) {
                    len = strlen(v.name);
                    if (t.compare(i, len, v.name) == 0) { value = v.value; break; }
                }
                if (value) {
                    // A leading '-' could make the argument an option where it is not an option's value.
                    if (p.quoted && (!inSpan || value->empty() || (*value)[0] == '-'
                                     || value->find('\'') != std::string::npos)) {
                        tokens.clear();
                        return false;
                    }
                    out += *value;
                    i += len;
                    continue;
                }
            }
            if (p.quoted && t[i] == '\'') inSpan = !inSpan;
            out += t[i++];
        }
        tokens.push_back(std::move(out));
    }
    // Only a substituted value (or a line break inside a quoted token) can make a token unsafe; a
    // value the template never uses (a tab in {file.path} under a {file.uri}-only template) is
    // irrelevant.
    for (const std::string& t : tokens) {
        if (!tokenSafe(t)) { tokens.clear(); return false; }
    }
    return true;
}

std::string joinTabbed(const std::vector<std::string>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i) out += '\t';
        out += tokens[i];
    }
    return out;
}

static bool plainOption(const std::string& t) {
    size_t i = 0;
    if (i < t.size() && t[i] == '-') i++;
    if (i < t.size() && t[i] == '-') i++;
    if (i == 0 || i >= t.size()) return false;
    if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z'))) return false;
    for (; i < t.size(); i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

std::string joinShell(const std::vector<std::string>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i) out += ' ';
        out += plainOption(tokens[i]) ? tokens[i] : shellQuote(tokens[i]);
    }
    return out;
}

bool joinAmArgs(const std::vector<AmArg>& args, bool tabbed, std::string& out) {
    out.clear();
    bool first = true;
    auto sep = [&]() {
        if (!first) out += tabbed ? '\t' : ' ';
        first = false;
    };
    for (const AmArg& a : args) {
        for (const std::string& v : a.values)
            if (!tokenSafe(v)) { out.clear(); return false; }
        sep();
        out += (!tabbed && a.flag == "-es") ? std::string("--es") : a.flag;
        for (const std::string& v : a.values) {
            sep();
            out += tabbed ? v : shellQuote(v);
        }
    }
    return true;
}

static bool pkgChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.';
}

bool isValidPackageName(const std::string& pkg) {
    if (pkg.empty() || pkg.size() > 255) return false;
    for (char c : pkg)
        if (!pkgChar(c)) return false;
    return true;
}

bool isValidComponent(const std::string& comp) {
    size_t sl = comp.find('/');
    if (sl == std::string::npos) return false;
    if (!isValidPackageName(comp.substr(0, sl))) return false;
    std::string cls = comp.substr(sl + 1);
    if (cls.empty() || cls.size() > 255) return false;
    for (char c : cls)
        if (!pkgChar(c) && c != '$') return false;
    return true;
}

bool isValidRepoName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static const char kRecentsHeader[] = "#nano-recent v2\n";

static void escAppend(std::string& out, const std::string& v) {
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
}

std::string serializeRecents(const std::vector<RecentRecord>& recents) {
    std::string out = kRecentsHeader;
    for (const RecentRecord& r : recents) {
        const std::string* f[] = {&r.romPath, &r.coreSo, &r.launchPkg, &r.launchIntent,
                                  &r.displayName, &r.systemName, &r.romDir};
        for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
            if (i) out += '\t';
            escAppend(out, *f[i]);
        }
        out += '\n';
    }
    return out;
}

static std::vector<std::string> splitEscapedLine(const std::string& line) {
    std::vector<std::string> fields(1);
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\t') { fields.emplace_back(); continue; }
        if (c == '\\' && i + 1 < line.size()) {
            char n = line[++i];
            if (n == 't') c = '\t';
            else if (n == 'n') c = '\n';
            else if (n == 'r') c = '\r';
            else c = n;
        }
        fields.back() += c;
    }
    return fields;
}

static RecentRecord fromFields(std::vector<std::string> f) {
    f.resize(7);
    RecentRecord r;
    r.romPath = f[0]; r.coreSo = f[1]; r.launchPkg = f[2]; r.launchIntent = f[3];
    r.displayName = f[4]; r.systemName = f[5]; r.romDir = f[6];
    return r;
}

std::vector<RecentRecord> parseRecents(const std::string& content, size_t max) {
    std::vector<RecentRecord> out;
    size_t pos = 0;
    auto readLine = [&]() -> std::string {
        if (pos >= content.size()) return {};
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        pos = eol + 1;
        return line;
    };
    const size_t hl = sizeof(kRecentsHeader) - 1;
    if (content.compare(0, hl, kRecentsHeader) == 0) {
        pos = hl;
        while (pos < content.size() && out.size() < max) {
            std::string line = readLine();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            RecentRecord r = fromFields(splitEscapedLine(line));
            if (r.romPath.empty()) continue;
            out.push_back(std::move(r));
        }
        return out;
    }
    // Legacy: romPath, coreSo, launchPkg, launchIntent, displayName, systemName, romDir on
    // seven lines, entries separated by a blank line.
    while (pos < content.size() && out.size() < max) {
        std::vector<std::string> f;
        f.push_back(readLine());
        if (f[0].empty()) continue;
        for (int i = 0; i < 6; i++) f.push_back(readLine());
        if (pos < content.size() && content[pos] == '\n') pos++;
        out.push_back(fromFields(std::move(f)));
    }
    return out;
}

}  // namespace nanolaunch
