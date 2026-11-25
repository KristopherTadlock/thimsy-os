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

#ifndef GAMMAOS_NANO_MENU_UTILS_H
#define GAMMAOS_NANO_MENU_UTILS_H

#include <string>
#include <cstdio>       // rename() for the durable writePathFile
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <utils/Log.h>

#include "NanoRaConfig.h"

namespace android {

// The OSK keyboard layout tables now live in the generated NanoOskLayouts.h
// (kOskKb[] / kOskPopups[]) and the runtime is NanoOsk.cpp. The old fixed
// 5x10 ASCII grid that used to live here has been removed.

// Case-insensitive substring search
inline bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needle.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// GammaOS: path props routinely exceed PROP_VALUE_MAX (92 bytes) when
// ROMs live on external SD at /storage/<UUID>/..., so the prop set
// silently fails. Mirror the path into a plain text file alongside so
// readers can fall back when the prop is empty. Empty values unlink
// the file so a stale path can never resurrect in the fallback read.
inline void writePathFile(const char* path, const std::string& value) {
    if (value.empty()) {
        unlink(path);
        return;
    }
    // Durable write: write a temp file, fsync it, then atomically rename over the
    // target. A plain O_TRUNC write can be truncated by a hard power cut (or a
    // Quick Resume power off racing sys.powerctl), leaving a half-written path
    // that resolves to the wrong game or nothing; the resume-target files are the
    // source of truth, so they must land atomically.
    // Always a fresh temp: one left by another user (nano_cache.sh) or planted as a link in
    // /data/system must never be opened, since Nano writes these files as root.
    std::string tmp = std::string(path) + ".tmp";
    int fd = -1;
    for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
        unlink(tmp.c_str());
        fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    }
    if (fd < 0) return;
    ssize_t n = write(fd, value.c_str(), value.size());
    if (n != (ssize_t)value.size()) {
        close(fd);
        unlink(tmp.c_str());
        return;
    }
    fchmod(fd, 0666);
    fsync(fd);
    close(fd);
    if (rename(tmp.c_str(), path) != 0) {
        // Rename can fail if the target is owned by another user; replace it.
        unlink(path);
        if (rename(tmp.c_str(), path) != 0) { unlink(tmp.c_str()); return; }
    }
    // fsync the parent directory so the rename (a new directory entry) is durable
    // across a hard cut -- prepareShutdown now writes the resume ROM on the
    // power-off path, right before init issues sys.powerctl.
    {
        std::string dir(path);
        size_t sl = dir.find_last_of('/');
        dir = (sl == std::string::npos) ? std::string(".") : dir.substr(0, sl);
        int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }
}

inline std::string readPathFile(const char* path) {
    // O_NONBLOCK: a FIFO in its place must not stall the caller (the render thread for some).
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::string();
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return std::string(); }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return std::string();
    std::string s(buf, (size_t)n);
    // Strip trailing whitespace/newlines to tolerate shell-written files.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

// launch_game when the path is too long for a property (PROP_VALUE_MAX, 92 bytes, is short of many
// card paths): "<launch_time>\n<path>", so a copy left from another launch or boot is never read
// (bmap::launchStateFrom).
inline constexpr const char* kLaunchGameFile = "/data/system/nano_launch_game.txt";

inline void setLaunchRomPath(const std::string& romPath) {
    android::base::SetProperty("sys.gammaos.nano.launch_rom", romPath);
    writePathFile("/data/system/nano_launch_rom.txt", romPath);
    // Every launch passes here: the button map's record of the game (setLaunchGame) starts over.
    android::base::SetProperty("sys.gammaos.nano.launch_system", "");
    android::base::SetProperty("sys.gammaos.nano.launch_game", "");
    writePathFile(kLaunchGameFile, "");
    android::base::SetProperty("sys.gammaos.nano.launch_game_core", "");
    android::base::SetProperty("sys.gammaos.nano.launch_time", std::to_string((long long)time(nullptr)));
}

// The game a launch runs, for the button map over it (NanoButtonMap.h, OverAppState): its system
// (romDir, which tells Dolphin's GameCube from its Wii), ROM and RetroArch core ("" standalone).
// Unlike launch_core, which the framework clears and the overlay's launches never set, this stays
// with the app until the next launch. After setLaunchRomPath.
inline void setLaunchGame(const std::string& systemId, const std::string& romPath,
                          const std::string& corePath = std::string()) {
    android::base::SetProperty("sys.gammaos.nano.launch_system", systemId);
    if (!android::base::SetProperty("sys.gammaos.nano.launch_game", romPath))
        writePathFile(kLaunchGameFile,
                      android::base::GetProperty("sys.gammaos.nano.launch_time", "") + "\n" + romPath);
    android::base::SetProperty("sys.gammaos.nano.launch_game_core", corePath);
}

inline void setQrRomPath(const std::string& romPath) {
    android::base::SetProperty("persist.gammaos.nano.qr_rom", romPath);
    writePathFile("/data/system/nano_qr_rom.txt", romPath);
}

// Quick Resume loop guard (drastic). Every boot that hands off to a resumed DS
// session bumps persist.gammaos.nano.qr_chain (qrResumeChainAdvance); drastic-nano
// resets it to 0 the moment the user presses a button or touches the panel. So a
// count of 2 at boot means the last TWO resumed sessions ended (power off, reboot,
// crash) without the user ever getting an input in: a resume the user cannot
// interact with, which they can only leave through the power button, and which the
// power-off path then re-arms. Users reported exactly this as being "stuck in a
// quick resume loop". Break it: boot to the home instead and disarm the resume.
// A resume the user merely glanced at and powered off costs one count, not the
// resume, so normal use never trips it.
inline bool qrResumeChainBlocked() {
    int chain = android::base::GetIntProperty("persist.gammaos.nano.qr_chain", 0);
    if (chain < 2) return false;
    property_set("persist.gammaos.nano.qr_prepared", "0");
    property_set("persist.gammaos.nano.qr_chain", "0");
    ALOGW("Quick Resume: %d resumed sessions in a row saw no user input, "
          "not resuming again (booting to the home)", chain);
    return true;
}

inline void qrResumeChainAdvance() {
    int chain = android::base::GetIntProperty("persist.gammaos.nano.qr_chain", 0);
    char v[16];
    snprintf(v, sizeof v, "%d", chain + 1);
    property_set("persist.gammaos.nano.qr_chain", v);
}

inline std::string getQrRomPath() {
    // Prefer the file over the persist prop. External SD paths
    // (e.g. /storage/<UUID>/nds/<long name>.nds) routinely exceed
    // PROP_VALUE_MAX (92 bytes) and the prop write silently fails,
    // leaving a stale short path from a previous session. The file
    // is always written regardless of length, so it's the source of
    // truth. Fall back to the prop only when the file is missing
    // (e.g. first boot, or the file was manually deleted).
    std::string p = readPathFile("/data/system/nano_qr_rom.txt");
    if (!p.empty()) return p;
    return android::base::GetProperty(
            "persist.gammaos.nano.qr_rom", "");
}

// Drastic nano ROM path -- separate from QR ROM path so drastic nano
// and QR drastic can coexist without overwriting each other's state.
// The getter lives in main.cpp (separate compilation unit) where it's
// used for boot-time ROM discovery.
// drastic-nano, the built-in DS player on DraStic's closed library, is in this image. Thimsy OS
// ships neither: a DraStic the user installs is then an ordinary emulator app, never run through
// Nano's DS player or its in-process Quick Resume (which loads the library and patches it).
inline bool drasticNanoAvailable() {
    static const bool have = access("/system/bin/drastic-nano", X_OK) == 0;
    return have;
}

inline void setDrasticNanoRomPath(const std::string& romPath) {
    writePathFile("/data/system/nano_drastic_nano_rom.txt", romPath);
}

// GammaOS: returns true when ALL storage RetroArch needs is mounted.
//
// Two independent gates:
//
// 1. Emulated FUSE (/storage/emulated/0/): RetroArch always needs this
//    for config, saves, and its data dir, regardless of where the ROM
//    lives. On cold boot, vold's FUSE mount is deferred until after
//    user-0 CE storage is unlocked. If RetroArch launches before FUSE
//    is up, it gets ENOENT on its data dir and corrupts its config
//    paths (e.g. save_directory becomes garbage like "hg{/saves").
//
// 2. External SD raw mount (/mnt/media_rw/<UUID>/): only needed when
//    the ROM path is on external storage (/storage/<UUID>/...). vold
//    defers external SD scanning until after the secure keyguard step,
//    so this can take 3-5s after NanoMenu starts.
// Gate 0 + Gate 1 without the QR-ROM external-SD check: confirm the user's PRIMARY (emulated)
// external storage is actually served by the FUSE daemon, not just vold's early empty tmpfs
// placeholder at /storage/emulated/0. Any /sdcard write (e.g. the setup wizard's setup.sh)
// depends only on this, so gates not tied to a specific external-SD ROM should use this.
inline bool isPrimaryStorageReady() {
    // Gate 0: the framework must confirm external storage is MOUNTED for user 0. The probes
    // below run as root in nano's own mount namespace and can pass several seconds before the
    // storage session APPS see is actually served. The NanoRelaunchMonitor thread in
    // SystemServer publishes sys.gammaos.nano.ext_storage_ready the moment
    // Environment.getExternalStorageState() reports "mounted", the same signal apps get.
    {
        char fw[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.ext_storage_ready", fw, "0");
        if (fw[0] != '1') return false;
    }

    // Gate 1: emulated FUSE must be ACTUALLY MOUNTED, not just the tmpfs placeholder directory.
    // /storage/emulated/0 exists as an empty tmpfs from very early boot (created by vold), so a
    // plain stat() passes prematurely. Probe /storage/emulated/0/Android: it is populated only
    // after FUSE mounts on top of the placeholder and the user's /data/media/0 is visible.
    struct stat st;
    if (stat("/storage/emulated/0/Android", &st) != 0
            || !S_ISDIR(st.st_mode))
        return false;
    return true;
}

inline bool isQrRomStorageReady() {
    // Gate 0 + Gate 1: the primary emulated volume is actually served (see isPrimaryStorageReady).
    // Every handoff this function gates needs the framework running anyway, so requiring its
    // signal cannot deadlock a handoff that could otherwise succeed.
    if (!isPrimaryStorageReady()) return false;

    struct stat st;
    // Gate 2: if ROM is on external SD, its raw vold mount must exist.
    std::string qrRom = getQrRomPath();
    if (qrRom.empty()) return true;
    if (qrRom.find("/storage/") != 0) return true;
    std::string rest = qrRom.substr(9); // skip "/storage/"
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return true;
    std::string uuid = rest.substr(0, slash);
    if (uuid == "emulated" || uuid == "self") return true;
    std::string rawDir = "/mnt/media_rw/" + uuid;
    if (stat(rawDir.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// /proc/self/mounts as text ("" when unreadable).
inline std::string readSelfMounts() {
    std::string text;
    const int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return text;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0 && text.size() < (4u << 20)) text.append(buf, (size_t)n);
    close(fd);
    return text;
}

// Whether a RetroArch launch can pass the config in RetroArch's folder (setting `dir`): the
// folder's volume is mounted, as NanoMenu::retroarchConfigFile requires. Before that a launch gets
// the default config, which Move Emulator Files rewrote to point into the folder, so a card that
// mounts after the ROM's volume has to be waited for too.
inline bool raConfigVolumeReady(const std::string& dir, const std::string& mounts) {
    const std::string root = nanora::raVolumeRoot(dir);
    return root.empty() || nanora::raMountListed(mounts, root);
}

inline bool isRaConfigVolumeReady() {
    char d[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaDirProp, d, "");
    return raConfigVolumeReady(d, readSelfMounts());
}

} // namespace android

#endif // GAMMAOS_NANO_MENU_UTILS_H
