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
#include "NanoJson.h"
#include "nano_test_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace nanolaunch;
namespace njson = android::njson;

namespace {

// ---------------------------------------------------------------------------------------------
// Frozen copies of the launch builders as they were before NanoLaunch (test 22), used to prove
// that ordinary ROM paths still produce byte-identical intents.
// ---------------------------------------------------------------------------------------------

std::string oldEncodeSeg(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c == ' ') o += "%20";
        else if (c == '(') o += "%28";
        else if (c == ')') o += "%29";
        else if (c == '&') o += "%26";
        else if (c == '+') o += "%2B";
        else if (c == '!') o += "%21";
        else if (c == '\'') o += "%27";
        else o += c;
    }
    return o;
}

// NanoMenuXmb.cpp buildSafTree (known prefixes; the overlay copy is identical for those).
std::string oldSafUri(const std::string& fullRomPath) {
    std::string filename = fullRomPath;
    { size_t ls = filename.rfind('/'); if (ls != std::string::npos) filename = filename.substr(ls + 1); }
    std::string outFilename = oldEncodeSeg(filename);
    std::string volumeId = "primary", relPath, work;
    bool external = false;
    if (fullRomPath.rfind("/data/media/0/", 0) == 0) relPath = fullRomPath.substr(14);
    else if (fullRomPath.rfind("/sdcard/", 0) == 0) relPath = fullRomPath.substr(8);
    else if (fullRomPath.rfind("/storage/emulated/0/", 0) == 0) relPath = fullRomPath.substr(20);
    else if (fullRomPath.rfind("/mnt/media_rw/", 0) == 0) { work = fullRomPath.substr(14); external = true; }
    else if (fullRomPath.rfind("/storage/", 0) == 0) { work = fullRomPath.substr(9); external = true; }
    if (external) {
        size_t sl1 = work.find('/');
        if (sl1 != std::string::npos) {
            volumeId = work.substr(0, sl1);
            size_t lastSl = work.rfind('/');
            relPath = (lastSl > sl1) ? work.substr(sl1 + 1, lastSl - sl1 - 1) : "";
        }
    } else {
        size_t lastSl = relPath.rfind('/');
        relPath = (lastSl != std::string::npos) ? relPath.substr(0, lastSl) : "";
    }
    std::string relDir;
    size_t pos = 0;
    while (pos <= relPath.size()) {
        size_t sl = relPath.find('/', pos);
        std::string seg = relPath.substr(pos, (sl == std::string::npos ? relPath.size() : sl) - pos);
        if (!seg.empty()) { if (!relDir.empty()) relDir += "%2F"; relDir += oldEncodeSeg(seg); }
        if (sl == std::string::npos) break;
        pos = sl + 1;
    }
    std::string treeRoot = volumeId + "%3A" + relDir;
    return "content://com.android.externalstorage.documents/tree/" + treeRoot + "/document/" +
           treeRoot + "%2F" + outFilename;
}

std::string oldSpaceSplitTabbed(const std::string& intent) {
    std::string tab;
    const char* p = intent.c_str();
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (!tab.empty()) tab += '\t';
        const char* s = p;
        while (*p && *p != ' ') p++;
        tab.append(s, p - s);
    }
    return tab;
}

// launchXmbGame, system list.
std::string oldHomeIntent(const std::string& tmpl, const std::string& args, const std::string& uri,
                          const std::string& path) {
    std::string intent = tmpl;
    if (!args.empty()) intent += " " + args;
    auto subst = [&](const char* token, const std::string& val) {
        size_t p, tl = strlen(token);
        while ((p = intent.find(token)) != std::string::npos) intent.replace(p, tl, val);
    };
    subst("{file.uri}", uri);
    subst("{file.path}", path);
    subst("{file.mime}", "application/octet-stream");
    return oldSpaceSplitTabbed(intent);
}

// launchXmbGame, Recently Played: first {file.uri} only.
std::string oldRecentsIntent(const std::string& tmpl, const std::string& uri) {
    std::string intent = tmpl;
    size_t pos = intent.find("{file.uri}");
    if (pos != std::string::npos) intent.replace(pos, 10, uri);
    return oldSpaceSplitTabbed(intent);
}

// overlayLaunchGame: the `am start` argument string (first {file.uri} only, shell-quoted).
std::string oldOverlayArgs(const std::string& tmpl, const std::string& uri) {
    std::string intent = tmpl;
    size_t pos = intent.find("{file.uri}");
    if (pos != std::string::npos) intent.replace(pos, 10, shellQuote(uri));
    return intent;
}

// mupenDirectIntent's join.
std::string oldMupenJoin(const std::vector<std::string>& tok, bool tabbed) {
    std::string out;
    for (size_t i = 0; i < tok.size(); i++) {
        if (tabbed) {
            if (i) out += '\t';
            out += tok[i];
        } else {
            if (i) out += ' ';
            if (tok[i] == "-es") { out += "--es"; continue; }
            out += (tok[i].compare(0, 1, "-") == 0) ? tok[i] : shellQuote(tok[i]);
        }
    }
    return out;
}

// NanoMenuPS3EmuCatalog.cpp normalizeWs: how a Daijishou template becomes launchIntent.
std::string normalizeWs(const std::string& in) {
    std::string s;
    bool sp = false;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') sp = true;
        else { if (sp && !s.empty()) s += ' '; sp = false; s += c; }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------

// The argv /bin/sh makes of an argument string (each arg terminated by \x1f).
std::string shellArgv(const std::string& args) {
    std::string cmd = "printf '%s\\037' " + args;
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "<popen failed>";
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}

std::string argvOf(const std::vector<std::string>& toks) {
    std::string out;
    for (const auto& t : toks) out += t + '\x1f';
    return out;
}

std::string exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p = buf;
    size_t sl = p.rfind('/');
    return sl == std::string::npos ? "." : p.substr(0, sl);
}

std::string daijishouDir() {
    if (const char* e = getenv("NANO_DAIJISHOU_DIR")) return e;
    return exeDir() + "/ps3xmb/daijishou";
}

struct Template {
    std::string where;
    std::string tmpl;
};

// Standalone (non-RetroArch-core) templates: the ones a system's launchIntent carries.
std::vector<Template> bundledTemplates() {
    std::vector<Template> out = {
        {"kXmbSystemDefs N64", "-n org.mupen64plusae.v3.fzurita/paulscode.android.mupen64plusae.SplashActivity -a android.intent.action.VIEW -d {file.uri}"},
        {"kXmbSystemDefs NDS", "-n com.dsemu.drastic/.DraSticActivity -a android.intent.action.VIEW -d {file.uri} --activity-clear-task --activity-clear-top"},
        {"kXmbSystemDefs PSP", "-n org.ppsspp.ppsspp/.PpssppActivity -a android.intent.action.VIEW -d {file.uri} -t application/octet-stream --activity-clear-task --activity-clear-top"},
        {"kXmbSystemDefs DC", "-n com.flycast.emulator/com.flycast.emulator.MainActivity -a android.intent.action.VIEW -d {file.uri}"},
    };
    const std::string dir = daijishouDir();
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> files;
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (const auto& n : files) {
        std::ifstream f(dir + "/" + n);
        std::stringstream ss;
        ss << f.rdbuf();
        njson::Value root;
        if (!njson::parse(ss.str(), &root) || !root.isObject()) continue;
        const njson::Value* pl = root.find("playerList");
        if (!pl || !pl->isArray()) continue;
        for (const njson::Value& p : pl->arr) {
            if (!p.isObject()) continue;
            std::string am = normalizeWs(p.getString("amStartArguments"));
            // {file.content} and quoted tokens are newer than the old builders these compare against
            // (see NanoLaunchFileId, NanoLaunchQuoted).
            if (am.empty() || usesFileContent(am, "") || am.find('"') != std::string::npos) continue;
            out.push_back({n + ": " + p.getString("name"), am});
        }
    }
    return out;
}

int count(const std::string& s, const std::string& sub) {
    int c = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) c++;
    return c;
}

std::vector<std::string> tokensOf(const std::string& tmpl, const std::string& args,
                                  const std::string& uri, const std::string& path) {
    LaunchVars v;
    v.fileUri = uri;
    v.filePath = path;
    std::vector<std::string> t;
    EXPECT_TRUE(buildLaunchIntent(tmpl, args, v, t));
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Equivalence: every bundled template, ordinary ROM paths.
// ---------------------------------------------------------------------------------------------

TEST(NanoLaunchEquivalence, BundledTemplatesFound) {
    // 4 built-in + the Daijishou catalogue (several hundred players).
    EXPECT_GT(bundledTemplates().size(), 400u) << "Daijishou dir: " << daijishouDir();
}

TEST(NanoLaunchEquivalence, SafUriUnchangedForOrdinaryNames) {
    for (const char* p : {"/storage/emulated/0/ROMs/psx/Game.chd", "/data/media/0/ROMs/psx/Game.chd",
                          "/sdcard/ROMs/nes/Super Mario Bros. (World).nes",
                          "/sdcard/ROMs/gba/sub dir/Pokemon - Emerald (USA).gba",
                          "/mnt/media_rw/1234-ABCD/ROMs/psx/Game.chd",
                          "/storage/1234-ABCD/Roms/n64/Mario Kart 64 (USA).z64",
                          "/sdcard/ROMs/snes/Tom & Jerry + Friends! 'Rev 1'.sfc",
                          "/sdcard/ROMs/psx/Game.chd" /* top level of ROMs/<dir> */}) {
        EXPECT_EQ(oldSafUri(p), safContentUri(p, "")) << p;
        EXPECT_EQ(oldSafUri(p), safContentUri(p, "psx")) << p;
    }
}

// The old encoder left everything outside " ()&+!'" raw; the provider decodes either form to the
// same document id, but the URI text changes for these.
TEST(NanoLaunchEquivalence, SafUriNowEncodesReservedChars) {
    const std::string p = "/sdcard/ROMs/gba/Pokemon - Emerald (USA, Europe) [!] #1 100%.gba";
    EXPECT_NE(oldSafUri(p), safContentUri(p, ""));
    EXPECT_EQ(safContentUri(p, ""),
              "content://com.android.externalstorage.documents/tree/primary%3AROMs%2Fgba/document/"
              "primary%3AROMs%2Fgba%2FPokemon%20-%20Emerald%20%28USA%2C%20Europe%29%20%5B%21%5D%20%231%20100%25.gba");
}

TEST(NanoLaunchEquivalence, HomeSystemListPath) {
    const std::string full = "/data/media/0/ROMs/psx/Game.chd";   // sys.roms[] form
    const std::string path = appVisiblePath(full);                  // /sdcard/ROMs/psx/Game.chd
    const std::string ext = "/mnt/media_rw/1234-ABCD/ROMs/psx/Game.chd";
    int n = 0;
    for (const auto& t : bundledTemplates()) {
        for (const std::string& rom : {full, ext}) {
            const std::string uri = safContentUri(rom, "");
            ASSERT_EQ(oldSafUri(rom), uri);
            const std::string p = appVisiblePath(rom);
            EXPECT_EQ(oldHomeIntent(t.tmpl, "", uri, p), joinTabbed(tokensOf(t.tmpl, "", uri, p))) << t.where;
            EXPECT_EQ(oldHomeIntent(t.tmpl, "--ez nano true -e X y", uri, p),
                      joinTabbed(tokensOf(t.tmpl, "--ez nano true -e X y", uri, p))) << t.where;
        }
        n++;
    }
    EXPECT_EQ(path, "/sdcard/ROMs/psx/Game.chd");
    EXPECT_GT(n, 0);
}

// Recently Played used to substitute only the first {file.uri}; templates without {file.path} /
// {file.mime} must not change. The others used to hand the emulator the literal placeholder.
TEST(NanoLaunchEquivalence, HomeRecentsPath) {
    const std::string rom = "/sdcard/ROMs/psx/Game.chd";   // recents store the app-visible path
    const std::string uri = safContentUri(rom, "");
    int same = 0, fixed = 0;
    for (const auto& t : bundledTemplates()) {
        const std::string now = joinTabbed(tokensOf(t.tmpl, "", uri, rom));
        const std::string before = oldRecentsIntent(t.tmpl, uri);
        if (t.tmpl.find("{file.path}") == std::string::npos &&
            t.tmpl.find("{file.mime}") == std::string::npos && count(t.tmpl, "{file.uri}") <= 1) {
            EXPECT_EQ(before, now) << t.where;
            same++;
        } else {
            EXPECT_TRUE(before.find("{file.") != std::string::npos) << t.where;
            EXPECT_EQ(now.find("{file."), std::string::npos) << t.where;
            EXPECT_EQ(oldHomeIntent(t.tmpl, "", uri, rom), now) << t.where;   // now matches the system list
            fixed++;
        }
    }
    printf("[ recents ] %d templates unchanged, %d now substitute {file.path}/{file.mime}\n", same, fixed);
}

// The overlay ran `am start <template>` through /bin/sh; compare the argv am receives.
TEST(NanoLaunchEquivalence, OverlayPath) {
    const std::string rom = "/storage/emulated/0/ROMs/psx/Game.chd";
    const std::string uri = safContentUri(rom, "psx");
    int same = 0, fixed = 0;
    for (const auto& t : bundledTemplates()) {
        const auto toks = tokensOf(t.tmpl, "", uri, appVisiblePath(rom));
        const std::string now = shellArgv(joinShell(toks));
        EXPECT_EQ(argvOf(toks), now) << t.where;
        const std::string before = shellArgv(oldOverlayArgs(t.tmpl, uri));
        if (t.tmpl.find("{file.path}") == std::string::npos &&
            t.tmpl.find("{file.mime}") == std::string::npos) {
            EXPECT_EQ(before, now) << t.where;
            same++;
        } else {
            EXPECT_TRUE(before.find("{file.") != std::string::npos) << t.where;
            fixed++;
        }
    }
    printf("[ overlay ] %d templates unchanged, %d now substitute {file.path}/{file.mime}\n", same, fixed);
}

TEST(NanoLaunchEquivalence, MupenJoinOrdinaryValues) {
    const std::string K = "paulscode.android.mupen64plusae.ActivityHelper.Keys.";
    const std::string fu = fileUri("/storage/emulated/0/ROMs/n64/Mario Kart 64 (USA).z64");
    const std::vector<std::string> flat = {
        "-n", "org.mupen64plusae.v3.fzurita/paulscode.android.mupen64plusae.game.GameActivity",
        "-a", "android.intent.action.MAIN", "-d", fu,
        "-es", K + "ROM_PATH", fu, "-es", K + "ROM_MD5", "0123456789ABCDEF0123456789ABCDEF",
        "-es", K + "ROM_CRC", "3E5055B6 2E92DA52", "-es", K + "ROM_HEADER_NAME", "MARIOKART64",
        "-es", K + "ROM_GOOD_NAME", "Mario Kart 64 (U) [!]", "-es", K + "ROM_DISPLAY_NAME",
        "Mario Kart 64 (USA)", "-es", K + "ROM_ART_PATH", ""};
    std::vector<AmArg> args = {
        {"-n", {flat[1]}}, {"-a", {flat[3]}}, {"-d", {fu}},
        {"-es", {K + "ROM_PATH", fu}}, {"-es", {K + "ROM_MD5", flat[11]}},
        {"-es", {K + "ROM_CRC", flat[14]}}, {"-es", {K + "ROM_HEADER_NAME", flat[17]}},
        {"-es", {K + "ROM_GOOD_NAME", flat[20]}}, {"-es", {K + "ROM_DISPLAY_NAME", flat[23]}},
        {"-es", {K + "ROM_ART_PATH", ""}}};
    std::string tab, sh;
    ASSERT_TRUE(joinAmArgs(args, true, tab));
    ASSERT_TRUE(joinAmArgs(args, false, sh));
    EXPECT_EQ(oldMupenJoin(flat, true), tab);
    EXPECT_EQ(shellArgv(oldMupenJoin(flat, false)), shellArgv(sh));
    // The old file URI encoder.
    std::string old = "file://";
    for (unsigned char c : std::string("/storage/emulated/0/ROMs/n64/Mario Kart 64 (USA).z64")) {
        if (isalnum(c) || c == '/' || c == '.' || c == '-' || c == '_' || c == '~') old += (char)c;
        else { char e[4]; snprintf(e, sizeof e, "%%%02X", c); old += e; }
    }
    EXPECT_EQ(old, fu);
}

// ---------------------------------------------------------------------------------------------
// The findings.
// ---------------------------------------------------------------------------------------------

const char* kRaTmpl =
    "-n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture -e ROM {file.path} -e LIBRETRO cap32";

TEST(NanoLaunchIntent, SpaceInNameStaysOneToken) {
    auto t = tokensOf(kRaTmpl, "", "", "/storage/1234-ABCD/Roms/amstradcpc/Chase H.Q. (UK).dsk");
    ASSERT_EQ(t.size(), 8u);
    EXPECT_EQ(t[4], "/storage/1234-ABCD/Roms/amstradcpc/Chase H.Q. (UK).dsk");
    EXPECT_EQ(t[7], "cap32");
}

TEST(NanoLaunchIntent, NameCannotInjectComponent) {
    auto t = tokensOf(kRaTmpl, "", "",
                      "/storage/1234-ABCD/Roms/amstradcpc/x -n com.android.settings/.DevelopmentSettings .dsk");
    ASSERT_EQ(t.size(), 8u);
    EXPECT_EQ(t[1], "com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture");
    EXPECT_EQ(std::count(t.begin(), t.end(), std::string("-n")), 1);
}

TEST(NanoLaunchIntent, HashPercentKeptInPathEncodedInUri) {
    const std::string rom = "/sdcard/ROMs/psx/Game #1 100%.chd";
    const std::string uri = safContentUri(rom, "");
    EXPECT_EQ(uri, "content://com.android.externalstorage.documents/tree/primary%3AROMs%2Fpsx/document/"
                   "primary%3AROMs%2Fpsx%2FGame%20%231%20100%25.chd");
    auto t = tokensOf("-d {file.uri} -e P {file.path}", "", uri, rom);
    ASSERT_EQ(t.size(), 5u);
    EXPECT_EQ(t[1], uri);
    EXPECT_EQ(t[4], rom);
}

TEST(NanoLaunchIntent, TabOrNewlineRejected) {
    LaunchVars v;
    std::vector<std::string> t;
    for (const char* bad : {"/sdcard/a\tb.nes", "/sdcard/a\nb.nes", "/sdcard/a\rb.nes"}) {
        v.filePath = bad;
        EXPECT_FALSE(buildLaunchIntent(kRaTmpl, "", v, t)) << bad;
        EXPECT_TRUE(t.empty());
    }
    v.filePath = "/sdcard/ok.nes";
    v.fileUri = "content://x/a\nb";
    EXPECT_FALSE(buildLaunchIntent("-d {file.uri}", "", v, t));
}

// Only the values a template uses matter: the content URI is percent-encoded, so a tab in the
// file name is harmless to a {file.uri}-only template and refused for a {file.path} one.
TEST(NanoLaunchIntent, TabOnlyRefusedWhereUsed) {
    const std::string rom = "/sdcard/ROMs/psp/Tab\there.iso";
    LaunchVars v;
    v.fileUri = safContentUri(rom, "");
    v.filePath = rom;
    EXPECT_NE(v.fileUri.find("Tab%09here.iso"), std::string::npos);
    std::vector<std::string> t;
    ASSERT_TRUE(buildLaunchIntent(
        "-n org.ppsspp.ppsspp/.PpssppActivity -a android.intent.action.VIEW -d {file.uri}", "", v, t));
    EXPECT_EQ(t.back(), v.fileUri);
    EXPECT_FALSE(buildLaunchIntent(kRaTmpl, "", v, t));
    EXPECT_TRUE(t.empty());
    EXPECT_FALSE(buildLaunchIntent("-d {file.uri}", "-e ROM {file.path}", v, t));   // via launch args
}

TEST(NanoLaunchIntent, PlaceholderInNameIsNotExpanded) {
    const std::string rom = "/sdcard/ROMs/x/{file.path}{file.uri}.iso";
    auto t = tokensOf("-d {file.uri} -e ROM {file.path}", "", "content://u", rom);
    ASSERT_EQ(t.size(), 5u);
    EXPECT_EQ(t[1], "content://u");
    EXPECT_EQ(t[4], rom);
}

TEST(NanoLaunchIntent, ReplaceAllAdvances) {
    EXPECT_EQ(replaceAll("a{x}b{x}", "{x}", "{x}{x}"), "a{x}{x}b{x}{x}");
    EXPECT_EQ(replaceAll("aaa", "a", "aa"), "aaaaaa");
    EXPECT_EQ(replaceAll("abc", "", "z"), "abc");
}

TEST(NanoLaunchIntent, EveryPlaceholderEveryToken) {
    auto t = tokensOf("-d {file.uri} -t {file.mime} --es A {file.uri}|{file.path}", "-e B {file.path}",
                      "content://u", "/p q");
    std::vector<std::string> want = {"-d", "content://u", "-t", "application/octet-stream", "--es", "A",
                                     "content://u|/p q", "-e", "B", "/p q"};
    EXPECT_EQ(t, want);
}

TEST(NanoLaunchIntent, ShellJoinQuotesValues) {
    std::vector<std::string> t = {"-n", "a.b/.C", "-e", "ROM", "/x/it's $(id) `id`;.nes", "--activity-clear-task"};
    const std::string s = joinShell(t);
    EXPECT_EQ(s.compare(0, 3, "-n "), 0);
    EXPECT_NE(s.find("--activity-clear-task"), std::string::npos);
    EXPECT_EQ(shellArgv(s), argvOf(t));
}

TEST(NanoLaunchMupen, DashLeadingValuesQuoted) {
    std::vector<AmArg> args = {{"-n", {"org.mupen64plusae.v3.fzurita/x"}},
                               {"-es", {"k.ROM_HEADER_NAME", "-;touch PWNED;"}},
                               {"-es", {"k.ROM_DISPLAY_NAME", "-$(echo injected)"}},
                               {"-es", {"k.ROM_ART_PATH", ""}}};
    std::string sh;
    ASSERT_TRUE(joinAmArgs(args, false, sh));
    EXPECT_NE(sh.find("'-;touch PWNED;'"), std::string::npos) << sh;
    EXPECT_NE(sh.find("'-$(echo injected)'"), std::string::npos) << sh;
    EXPECT_EQ(shellArgv(sh), argvOf({"-n", "org.mupen64plusae.v3.fzurita/x", "--es", "k.ROM_HEADER_NAME",
                                     "-;touch PWNED;", "--es", "k.ROM_DISPLAY_NAME", "-$(echo injected)",
                                     "--es", "k.ROM_ART_PATH", ""}));
    std::string tab;
    ASSERT_TRUE(joinAmArgs(args, true, tab));
    EXPECT_EQ(tab, "-n\torg.mupen64plusae.v3.fzurita/x\t-es\tk.ROM_HEADER_NAME\t-;touch PWNED;\t-es\t"
                   "k.ROM_DISPLAY_NAME\t-$(echo injected)\t-es\tk.ROM_ART_PATH\t");
}

TEST(NanoLaunchMupen, TabInValueRejected) {
    std::string out = "stale";
    EXPECT_FALSE(joinAmArgs({{"-es", {"k", "a\tb"}}}, true, out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(joinAmArgs({{"-es", {"k", "a\nb"}}}, false, out));
}

TEST(NanoLaunchUri, UrlEncodeUnreservedOnly) {
    EXPECT_EQ(urlEncode("AZaz09-._~"), "AZaz09-._~");
    EXPECT_EQ(urlEncode(" #%&+!'()[],;=@$/?:"), "%20%23%25%26%2B%21%27%28%29%5B%5D%2C%3B%3D%40%24%2F%3F%3A");
    EXPECT_EQ(urlEncode("\xC3\xA9"), "%C3%A9");
    EXPECT_EQ(fileUri("/a b/c#.z64"), "file:///a%20b/c%23.z64");
}

TEST(NanoLaunchUri, SafSubfoldersAndVolumes) {
    EXPECT_EQ(safContentUri("/storage/ABCD-1234/Roms/n64/sub/G.z64", ""),
              "content://com.android.externalstorage.documents/tree/ABCD-1234%3ARoms%2Fn64%2Fsub/document/"
              "ABCD-1234%3ARoms%2Fn64%2Fsub%2FG.z64");
    EXPECT_EQ(safContentUri("/sdcard/G.z64", ""),
              "content://com.android.externalstorage.documents/tree/primary%3A/document/primary%3A%2FG.z64");
    // Unknown prefix: ROMs/<romDir> (overlay) or the path's own dirs (home).
    EXPECT_EQ(safContentUri("/mnt/smb/x/G.z64", "n64"),
              "content://com.android.externalstorage.documents/tree/primary%3AROMs%2Fn64/document/"
              "primary%3AROMs%2Fn64%2FG.z64");
    EXPECT_EQ(safContentUri("/mnt/smb/x/G.z64", ""),
              "content://com.android.externalstorage.documents/tree/primary%3Amnt%2Fsmb%2Fx/document/"
              "primary%3Amnt%2Fsmb%2Fx%2FG.z64");
}

TEST(NanoLaunchUri, AppVisiblePath) {
    EXPECT_EQ(appVisiblePath("/data/media/0/ROMs/a.nes"), "/sdcard/ROMs/a.nes");
    EXPECT_EQ(appVisiblePath("/mnt/media_rw/AB-CD/a.nes"), "/storage/AB-CD/a.nes");
    EXPECT_EQ(appVisiblePath("/storage/emulated/0/a.nes"), "/storage/emulated/0/a.nes");
}

// SeedlessDS's LaunchGame alias reads the ROM from the rom_path extra (a file path, not a URI).
TEST(NanoLaunchSeedlessDs, TemplateBuildsLaunchGameIntent) {
    std::vector<Template> found;
    for (const auto& t : bundledTemplates())
        if (t.where == "NintendoDS.json: nds - SeedlessDS" || t.where == "NintendoDSi.json: ndsi - SeedlessDS")
            found.push_back(t);
    ASSERT_EQ(found.size(), 2u);
    for (const auto& t : found) {
        for (const std::string& rom : {std::string("/data/media/0/ROMs/nds/Mario Kart DS (USA) [!].nds"),
                                       std::string("/mnt/media_rw/1234-ABCD/ROMs/nds/Game #1.zip")}) {
            const std::string path = appVisiblePath(rom);
            const auto toks = tokensOf(t.tmpl, "", safContentUri(rom, ""), path);
            const std::vector<std::string> want = {"-n", "com.seedlessds.app/com.seedlessds.app.LaunchGame",
                                                   "-e", "rom_path", path, "--activity-clear-task"};
            EXPECT_EQ(toks, want) << t.where;
            EXPECT_TRUE(isValidComponent(toks[1]));
            EXPECT_EQ(joinTabbed(toks), "-n\tcom.seedlessds.app/com.seedlessds.app.LaunchGame\t-e\trom_path\t" + path +
                                            "\t--activity-clear-task") << t.where;
            EXPECT_EQ(shellArgv(joinShell(toks)), argvOf(want)) << t.where;
        }
    }
}

TEST(NanoLaunchValidate, PackageNames) {
    for (const char* ok : {"com.retroarch.aarch64", "org.ppsspp.ppsspp", "a", "com.foo_bar.Baz9"})
        EXPECT_TRUE(isValidPackageName(ok)) << ok;
    for (const char* bad : {"", "a.b';touch PWNED_PKG;'", "a b", "a.b/c", "a.b\n", "$(id)", "a-b",
                            "a;b", "com.foo`id`"})
        EXPECT_FALSE(isValidPackageName(bad)) << bad;
    EXPECT_FALSE(isValidPackageName(std::string(256, 'a')));
    EXPECT_TRUE(isValidComponent("com.dsemu.drastic/.DraSticActivity"));
    EXPECT_TRUE(isValidComponent("a.b/a.b.Outer$Inner"));
    for (const char* bad : {"a.b", "a.b/", "/C", "a.b/C D", "a.b/C;id", "a b/C", "a.b/C/D"})
        EXPECT_FALSE(isValidComponent(bad)) << bad;
}

TEST(NanoLaunchValidate, RepoNames) {
    for (const char* ok : {"art-book-next-es-de", "slate-es-de", "Theme_2.0", "a"})
        EXPECT_TRUE(isValidRepoName(ok)) << ok;
    for (const char* bad : {"", ".", "..", ".dl_stage", "../x", "a/b", "a b", "a;b", "a\nb", "$(id)",
                            "a\\b"})
        EXPECT_FALSE(isValidRepoName(bad)) << bad;
    EXPECT_TRUE(isValidRepoName(std::string(64, 'a')));
    EXPECT_FALSE(isValidRepoName(std::string(65, 'a')));
}

TEST(NanoLaunchShell, QuoteRoundTrip) {
    for (const char* s : {"", "plain", "it's", "'", "$(id)", "a\nb", "-x", "`id`;|&<>"})
        EXPECT_EQ(shellArgv(shellQuote(s)), std::string(s) + '\x1f') << s;
}

// ---------------------------------------------------------------------------------------------
// Recently Played storage.
// ---------------------------------------------------------------------------------------------

static bool sameRecord(const RecentRecord& a, const RecentRecord& b) {
    return a.romPath == b.romPath && a.coreSo == b.coreSo && a.launchPkg == b.launchPkg &&
           a.launchIntent == b.launchIntent && a.displayName == b.displayName &&
           a.systemName == b.systemName && a.romDir == b.romDir;
}

TEST(NanoLaunchRecents, RoundTripOddNames) {
    std::vector<RecentRecord> in = {
        {"/data/media/0/ROMs/gba/a\nx\ncom.retroarch.aarch64\n-n x/.y; touch /data/pwned_as_root #.gba",
         "mgba_libretro_android.so", "", "", "a\n", "gba", "gba"},
        {"/sdcard/ROMs/n64/Tab\there\\back\\nslash\r.z64", "", "org.mupen64plusae.v3.fzurita",
         "-n org.mupen64plusae.v3.fzurita/paulscode.android.mupen64plusae.SplashActivity -d {file.uri}",
         "Tab\there", "N64", "n64"},
        {"/sdcard/ROMs/x/#nano-recent v2", "", "", "", "", "", ""},
        {"/sdcard/ROMs/psx/Game.chd", "", "", "", "", "", ""},
    };
    const std::string data = serializeRecents(in);
    EXPECT_EQ(data.compare(0, 16, "#nano-recent v2\n"), 0);
    EXPECT_EQ((size_t)std::count(data.begin(), data.end(), '\n'), in.size() + 1);
    auto out = parseRecents(data, 100);
    ASSERT_EQ(out.size(), in.size());
    for (size_t i = 0; i < in.size(); i++) EXPECT_TRUE(sameRecord(in[i], out[i])) << i;
    EXPECT_TRUE(out[0].launchPkg.empty());   // the newline trick no longer makes it standalone
    EXPECT_TRUE(out[0].launchIntent.empty());
    EXPECT_EQ(parseRecents(data, 2).size(), 2u);
    EXPECT_TRUE(parseRecents(serializeRecents({}), 10).empty());
}

TEST(NanoLaunchRecents, MigratesLegacyFormat) {
    const std::string legacy =
        "/sdcard/ROMs/nes/Mario.nes\nnestopia_libretro_android.so\n\n\nMario\nNES\nnes\n\n"
        "/sdcard/ROMs/psp/Game.iso\n\norg.ppsspp.ppsspp\n-n org.ppsspp.ppsspp/.PpssppActivity -d {file.uri}\n"
        "Game\nPSP\npsp\n\n";
    auto out = parseRecents(legacy, 100);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].romPath, "/sdcard/ROMs/nes/Mario.nes");
    EXPECT_EQ(out[0].coreSo, "nestopia_libretro_android.so");
    EXPECT_EQ(out[0].romDir, "nes");
    EXPECT_EQ(out[1].launchPkg, "org.ppsspp.ppsspp");
    EXPECT_EQ(out[1].launchIntent, "-n org.ppsspp.ppsspp/.PpssppActivity -d {file.uri}");
    EXPECT_EQ(out[1].systemName, "PSP");
    // Re-saved in the new format, nothing lost.
    auto again = parseRecents(serializeRecents(out), 100);
    ASSERT_EQ(again.size(), 2u);
    for (size_t i = 0; i < 2; i++) EXPECT_TRUE(sameRecord(out[i], again[i]));
}

TEST(NanoLaunchRecents, LegacyBlankLineDoesNotEatNextEntry) {
    // An extra blank line before an entry used to skip the next character as well ("/sdcard..."
    // became "sdcard...").
    const std::string legacy = "\n/sdcard/ROMs/nes/A.nes\na.so\n\n\nA\nNES\nnes\n\n\n"
                               "/sdcard/ROMs/nes/B.nes\nb.so\n\n\nB\nNES\nnes\n\n";
    auto out = parseRecents(legacy, 100);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].romPath, "/sdcard/ROMs/nes/A.nes");
    EXPECT_EQ(out[1].romPath, "/sdcard/ROMs/nes/B.nes");
}

TEST(NanoLaunchRecents, LegacyMaxAndTruncated) {
    const std::string legacy = "/a\nc\n\n\nA\nS\nd\n\n/b\nc\n\n\nB\nS\nd\n\n/c\nc";
    EXPECT_EQ(parseRecents(legacy, 1).size(), 1u);
    auto all = parseRecents(legacy, 10);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[2].romPath, "/c");
    EXPECT_EQ(all[2].coreSo, "c");
    EXPECT_TRUE(all[2].romDir.empty());
}

// ---------------------------------------------------------------------------------------------
// {file.content}: the game ID a launch file holds (ScummVM's <name>.scummvm files).
// ---------------------------------------------------------------------------------------------

TEST(NanoLaunchFileId, FirstLineTrimmed) {
    EXPECT_EQ(launchFileId("sky"), "sky");
    EXPECT_EQ(launchFileId("sky\n"), "sky");
    EXPECT_EQ(launchFileId("  sky \r\nsecond line"), "sky");
    EXPECT_EQ(launchFileId("monkey2-vga_en\n"), "monkey2-vga_en");
    EXPECT_EQ(launchFileId(std::string(64, 'a')), std::string(64, 'a'));
}

TEST(NanoLaunchFileId, RejectsWhatIsNotAnId) {
    const std::vector<std::string> bads = {"", "\n", "   \n", "\nsky", "sky tower", "-n evil/.Activity", "-n", "--help",
                                  "file:///sdcard/x", "content://x", "sky;reboot", "sky\tx",
                                  "../sky", std::string(65, 'a'), std::string("sk\0y", 4)};
    for (const std::string& bad : bads) {
        EXPECT_EQ(launchFileId(bad), "") << bad;
    }
}

TEST(NanoLaunchFileId, ReadsFileAndRefusesMissing) {
    nanotest::TempDir tmp{"launchid"};
    const std::string f = tmp / "Beneath a Steel Sky.scummvm";
    {
        std::ofstream o(f);
        o << "sky\n";
    }
    EXPECT_EQ(readLaunchFileId(f), "sky");
    EXPECT_EQ(readLaunchFileId(tmp / "missing.scummvm"), "");
    EXPECT_EQ(readLaunchFileId(tmp.path()), "");   // a folder
    const std::string big = tmp / "big.scummvm";
    {
        std::ofstream o(big);
        o << "sky\n" << std::string(10000, 'x');
    }
    EXPECT_EQ(readLaunchFileId(big), "sky");
}

TEST(NanoLaunchFileId, ToleratesAByteOrderMark) {
    EXPECT_EQ(launchFileId("\xEF\xBB\xBFsky\r\n"), "sky");
    EXPECT_EQ(launchFileId("\xEF\xBB\xBF"), "");
    EXPECT_EQ(launchFileId("\xEF\xBBsky"), "");
}

TEST(NanoLaunchFileId, NeverWaitsOnAFifo) {
    nanotest::TempDir tmp{"launchfifo"};
    const std::string fifo = tmp / "game.scummvm";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_EQ(readLaunchFileId(fifo), "");
}

TEST(NanoLaunchFileId, ScummvmTemplatePassesTheId) {
    const std::string dir = daijishouDir();
    std::ifstream f(dir + "/ScummVM.json");
    std::stringstream ss;
    ss << f.rdbuf();
    njson::Value root;
    ASSERT_TRUE(njson::parse(ss.str(), &root)) << dir;
    std::string tmpl;
    for (const njson::Value& p : root.find("playerList")->arr)
        if (p.getString("uniqueId") == "scummvm.org.scummvm.scummvm") tmpl = normalizeWs(p.getString("amStartArguments"));
    ASSERT_FALSE(tmpl.empty());
    EXPECT_TRUE(usesFileContent(tmpl, ""));

    LaunchVars v;
    v.filePath = "/sdcard/ROMs/scummvm/Beneath a Steel Sky.scummvm";
    v.fileUri = safContentUri("/data/media/0/ROMs/scummvm/Beneath a Steel Sky.scummvm", "");
    v.fileContent = "sky";
    std::vector<std::string> toks;
    ASSERT_TRUE(buildLaunchIntent(tmpl, "", v, toks));
    const std::vector<std::string> want = {"-n", "org.scummvm.scummvm/org.scummvm.scummvm.SplashActivity",
                                           "-a", "android.intent.action.MAIN",
                                           "-c", "android.intent.category.LAUNCHER", "-d", "sky"};
    EXPECT_EQ(toks, want);
    EXPECT_TRUE(isValidComponent(toks[1]));
    EXPECT_EQ(shellArgv(joinShell(toks)), argvOf(want));

    // No ID: refused, rather than starting ScummVM with "-d" swallowing nothing.
    v.fileContent.clear();
    EXPECT_FALSE(buildLaunchIntent(tmpl, "", v, toks));
    EXPECT_TRUE(toks.empty());
}

TEST(NanoLaunchFileId, OnlyWhereUsed) {
    EXPECT_FALSE(usesFileContent(kRaTmpl, ""));
    EXPECT_TRUE(usesFileContent("-n a/.B", "-e GAME {file.content}"));
    // A template without {file.content} never needs it.
    LaunchVars v;
    v.filePath = "/sdcard/ROMs/x/a.bin";
    std::vector<std::string> toks;
    EXPECT_TRUE(buildLaunchIntent(kRaTmpl, "", v, toks));
    // An ID in the file name is never expanded from the name itself.
    v.filePath = "/sdcard/ROMs/x/{file.content}.bin";
    v.fileContent = "sky";
    ASSERT_TRUE(buildLaunchIntent(kRaTmpl, "", v, toks));
    EXPECT_EQ(toks[4], "/sdcard/ROMs/x/{file.content}.bin");
}

// ---------------------------------------------------------------------------------------------
// Quoted tokens: an argument string the app splits again (MAME4droid's cli_params).
// ---------------------------------------------------------------------------------------------

namespace {

// MAME4droid's split_in_args (src/osd/myosd/droid/myosd_droid.cpp, MAME4droid-current), verbatim
// but for the types: how the cli_params extra becomes MAME's arguments.
std::vector<std::string> mame4droidSplit(const std::string& command) {
    std::vector<std::string> qargs;
    int len = (int)command.length();
    bool sqot = false;
    int arglen;
    for (int i = 0; i < len; i++) {
        int start = i;
        if (command[i] == '\'') sqot = true;
        if (sqot) {
            i++;
            start++;
            while (i < len && command[i] != '\'') i++;
            if (i < len) sqot = false;
            arglen = i - start;
            i++;
        } else {
            while (i < len && command[i] != ' ') i++;
            arglen = i - start;
        }
        qargs.push_back(command.substr(start, arglen));
    }
    return qargs;
}

const char kMameTmpl[] =
    "-n com.seleuco.mame4d2024/com.seleuco.mame4droid.MAME4droid -a android.intent.action.VIEW -d apple2gs "
    "-e cli_params \"-rompath '{file.storagedir};/storage/emulated/0/Android/data/com.seleuco.mame4d2024/files/roms' "
    "-gameio joy -flop3 '{file.storagepath}'\" --activity-clear-task --activity-clear-top";

bool mameTokens(const std::string& path, std::vector<std::string>& toks) {
    LaunchVars v;
    v.filePath = path;
    v.fileUri = safContentUri(path, "");
    return buildLaunchIntent(kMameTmpl, "", v, toks);
}

std::vector<std::string> mameArgs(const std::string& storageFile) {
    const size_t sl = storageFile.rfind('/');
    return {"-rompath", storageFile.substr(0, sl) + ";/storage/emulated/0/Android/data/com.seleuco.mame4d2024/files/roms",
            "-gameio", "joy", "-flop3", storageFile};
}

}  // namespace

TEST(NanoLaunchQuoted, StoragePath) {
    EXPECT_EQ(storagePath("/sdcard/ROMs/coco/a.ccc"), "/storage/emulated/0/ROMs/coco/a.ccc");
    EXPECT_EQ(storagePath("/data/media/0/ROMs/coco/a.ccc"), "/storage/emulated/0/ROMs/coco/a.ccc");
    EXPECT_EQ(storagePath("/mnt/media_rw/1234-ABCD/ROMs/coco/a.ccc"), "/storage/1234-ABCD/ROMs/coco/a.ccc");
    EXPECT_EQ(storagePath("/storage/emulated/0/ROMs/a.ccc"), "/storage/emulated/0/ROMs/a.ccc");
    EXPECT_EQ(storagePath("/storage/1234-ABCD/a.ccc"), "/storage/1234-ABCD/a.ccc");
    EXPECT_EQ(storagePath("/mnt/shares/nas/a.ccc"), "/mnt/shares/nas/a.ccc");
}

TEST(NanoLaunchQuoted, OneTokenThatSplitsIntoMameArguments) {
    std::vector<std::string> toks;
    ASSERT_TRUE(mameTokens("/sdcard/ROMs/apple2gs/Space Quest (USA).2mg", toks));
    ASSERT_EQ(toks.size(), 11u);
    EXPECT_EQ(toks[5], "apple2gs");
    EXPECT_EQ(toks[6], "-e");
    EXPECT_EQ(toks[7], "cli_params");
    EXPECT_EQ(mame4droidSplit(toks[8]), mameArgs("/storage/emulated/0/ROMs/apple2gs/Space Quest (USA).2mg"));
    EXPECT_EQ(toks[9], "--activity-clear-task");
    // Through the intent file (tab separated) and through /bin/sh, the same arguments.
    std::vector<std::string> tabbed;
    { std::stringstream ss(joinTabbed(toks)); std::string f; while (std::getline(ss, f, '\t')) tabbed.push_back(f); }
    EXPECT_EQ(tabbed, toks);
    EXPECT_EQ(shellArgv(joinShell(toks)), argvOf(toks));
    // A card's game.
    ASSERT_TRUE(mameTokens("/storage/1234-ABCD/ROMs/apple2gs/Game.2mg", toks));
    EXPECT_EQ(mame4droidSplit(toks[8]), mameArgs("/storage/1234-ABCD/ROMs/apple2gs/Game.2mg"));
}

// Hostile file names: each either stays one MAME argument, one intent token and one shell word, or the
// launch is refused.
TEST(NanoLaunchQuoted, InjectionSafe) {
    const std::string dir = "/sdcard/ROMs/apple2gs/";
    const char* kept[] = {
        "a b  c.2mg", "$(echo injected).2mg", "`echo injected`.2mg", "a;echo injected;b.2mg", "a && echo injected.2mg", "a|b>c<d.2mg",
        "-flop1 x.2mg", "--x.2mg", "\"q\".2mg", "a\"b.2mg", "a\\\\b.2mg", "{file.storagepath}.2mg",
        "{file.path} -cart x.2mg", "a*?[b].2mg", "ünïcödé ゲーム.2mg", "$HOME.2mg", "~.2mg", "#x.2mg", "%s%n.2mg",
    };
    for (const char* n : kept) {
        SCOPED_TRACE(n);
        const std::string path = dir + n;
        std::vector<std::string> toks;
        ASSERT_TRUE(mameTokens(path, toks));
        ASSERT_EQ(toks.size(), 11u);
        EXPECT_EQ(mame4droidSplit(toks[8]), mameArgs("/storage/emulated/0/ROMs/apple2gs/" + std::string(n)));
        EXPECT_EQ(shellArgv(joinShell(toks)), argvOf(toks));
        const std::string tabbed = joinTabbed(toks);
        EXPECT_EQ(std::count(tabbed.begin(), tabbed.end(), '\t'), 10);
    }
    const char* refused[] = {
        "it's.2mg",                                            // ends the '...' argument
        "x' -autoboot_script '/sdcard/evil.lua",               // would add MAME options
        "x'",
        "a\nb.2mg", "a\rb.2mg", "a\tb.2mg",                    // not one intent token / line
    };
    for (const char* n : refused) {
        SCOPED_TRACE(n);
        std::vector<std::string> toks = {"stale"};
        EXPECT_FALSE(mameTokens(dir + n, toks));
        EXPECT_TRUE(toks.empty());
    }
    // A folder name can end the argument too.
    std::vector<std::string> toks;
    EXPECT_FALSE(mameTokens("/sdcard/ROMs/it's/Game.2mg", toks));
    EXPECT_FALSE(mameTokens(std::string("/sdcard/ROMs/a\nb/Game.2mg"), toks));
    // A value that would start the argument with '-' (not an absolute path) is refused in a quoted
    // token; the same value outside one is an ordinary token, as before.
    LaunchVars v;
    v.filePath = "-autoboot_script";
    EXPECT_FALSE(buildLaunchIntent("-n a/.B -e cli_params \"-cart '{file.path}'\"", "", v, toks));
    ASSERT_TRUE(buildLaunchIntent("-n a/.B -e ROM {file.path}", "", v, toks));
    EXPECT_EQ(toks.back(), "-autoboot_script");
    v.filePath.clear();   // empty: no argument at all
    EXPECT_FALSE(buildLaunchIntent("-n a/.B -e cli_params \"-cart '{file.path}'\"", "", v, toks));
}

TEST(NanoLaunchQuoted, MalformedTemplatesAreRefused) {
    LaunchVars v;
    v.filePath = "/sdcard/ROMs/coco/a.ccc";
    std::vector<std::string> toks;
    for (const char* t : {
             "-n a/.B -e cli_params \"-cart '{file.storagepath}'",          // unclosed
             "-n a/.B -e cli_params \"-cart '{file.storagepath}'\"x",       // text after the closing quote
             "-n a/.B -e cli_params \"-cart {file.storagepath}\"",          // placeholder outside '...'
             "-n a/.B -e cli_params \"-cart x'{file.storagepath}'\"",       // span not at an argument's start
             "-n a/.B -e cli_params \"-cart '{file.storagepath}'x\"",       // text after a span
             "-n a/.B -e cli_params \"-cart '{file.storagepath}\"",         // unclosed span
             "-n a/.B -e cli_params \"-cart  '{file.storagepath}'\"",       // two spaces: an empty argument
             "-n a/.B -e cli_params \" -cart '{file.storagepath}'\"",       // leading space
             "-n a/.B -e cli_params \"-cart '{file.storagepath}' \"",       // trailing space
             "-n a/.B -e cli_params \"-cart\t'{file.storagepath}'\"",       // a tab between arguments
             "-n a/.B -e cli_params \"\"",                                  // empty
         }) {
        SCOPED_TRACE(t);
        toks = {"stale"};
        EXPECT_FALSE(buildLaunchIntent(t, "", v, toks));
        EXPECT_TRUE(toks.empty());
    }
    // The extra launch args follow the same rules.
    EXPECT_FALSE(buildLaunchIntent("-n a/.B", "-e cli_params \"-cart {file.path}\"", v, toks));
    ASSERT_TRUE(buildLaunchIntent("-n a/.B", "-e cli_params \"-cart '{file.path}'\"", v, toks));
    EXPECT_EQ(mame4droidSplit(toks.back()), (std::vector<std::string>{"-cart", "/sdcard/ROMs/coco/a.ccc"}));
}

TEST(NanoLaunchQuoted, EscapedQuoteAndLiteralText) {
    LaunchVars v;
    v.filePath = "/sdcard/ROMs/oric/Zorgons Revenge.tap";
    std::vector<std::string> toks;
    ASSERT_TRUE(buildLaunchIntent(
        "-n a/.B -e cli_params \"-autoboot_delay '4' -autoboot_command 'cload\\\"\\\"\\n' -cass '{file.storagepath}'\" -d oric1",
        "", v, toks));
    ASSERT_EQ(toks.size(), 7u);
    EXPECT_EQ(toks[4], "-autoboot_delay '4' -autoboot_command 'cload\"\"\\n' -cass '/storage/emulated/0/ROMs/oric/Zorgons Revenge.tap'");
    EXPECT_EQ(mame4droidSplit(toks[4]),
              (std::vector<std::string>{"-autoboot_delay", "4", "-autoboot_command", "cload\"\"\\n", "-cass",
                                        "/storage/emulated/0/ROMs/oric/Zorgons Revenge.tap"}));
    EXPECT_EQ(toks[6], "oric1");
    // A '"' inside an unquoted token is still a literal character, as before.
    ASSERT_TRUE(buildLaunchIntent("-n a/.B -e X a\"b", "", v, toks));
    EXPECT_EQ(toks.back(), "a\"b");
    // Text with no placeholder in a quoted token needs no span.
    ASSERT_TRUE(buildLaunchIntent("-n a/.B -e cli_params \"-ui simple\"", "", v, toks));
    EXPECT_EQ(toks.back(), "-ui simple");
}
