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

#ifndef GAMMAOS_NANO_MENU_H
#define GAMMAOS_NANO_MENU_H

#include <math.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "NanoFrameFlag.h"

namespace android {
// GammaOS: a std::thread started from the SCHED_FIFO 80 / nice -20 render thread
// inherits that policy, priority and nice, and so does every process it then
// popen()s or system()s: a "settings get" child ran at FIFO 80 and its
// system_server binder thread at nice -20, above every other system_server
// thread. Under the nano startup load that stalled system_server for tens of
// seconds (measured on the RG DS Plus), and the render thread waiting on such a
// child tripped its own watchdog. Call this first in any helper thread that is
// not itself latency critical.
static inline void nanoThreadNormalPriority() {
    sched_param sp = {};
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, (int)syscall(SYS_gettid), 0);
}
} // namespace android

#include "NanoMenuSettingsTree.h"
#include "NanoOsk.h"
#include "NanoTouch.h"
#include "NanoAudio.h"
#include "NanoTsDemux.h"
#include "NanoAviDemux.h"
#include "NanoScraper.h"
#include "NanoScrapeQueue.h"
#include "NanoJson.h"
#include "NanoEsdeTheme.h"
#include "NanoSystemsHeal.h"
#include "NanoSyncthing.h"  // Syncthing REST client + model (Settings > Syncthing)
#include "NanoNdsBanner.h"   // DS ROM banner icon + title (DSi theme)   // ES-DE theme engine model (fourth home theme)
#include "NanoArchive.h"     // File Explorer archive browse / extract / compress
#include "NanoBiosWorker.h"  // BIOS installer worker state
#include "NanoFileJob.h"     // copy engine (folder copies, microSD moves)
#include "NanoMonitorMode.h" // a monitor's output modes (External Display > Output Resolution)

#include <ft2build.h>
#include FT_FREETYPE_H

#include <sys/system_properties.h>   // prop_info (PropEdge)
#include <utils/Thread.h>
#include <binder/IBinder.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/sensor.h>   // ASensor NDK (PSP clock gyro/accel parallax)
#include <android/looper.h>   // ALooper for the sensor event queue

class NanoVideo;   // global; HW video decoder (NanoVideo.h). Forward-declared to keep gui/ headers out of NanoMenu.h.
class NanoDvbSub;  // global; DVB bitmap subtitle decoder (NanoDvbSub.h).
typedef struct AMediaFormat AMediaFormat;   // NDK media format (kept opaque here; pointer member only).
struct input_event;   // <linux/input.h>

namespace android {

class Surface;
class SurfaceComposerClient;
class SurfaceControl;
class GraphicBuffer;
struct NanoBmapUi;   // NanoMenuButtonMap.cpp
namespace bmap { struct MenuFocus; }

// FrameFlag (the "something to draw" flag that wakes the render loop) is in NanoFrameFlag.h.

// Release/restore the Bluetooth bluesleep LPM wakelock around the framework-owned
// (overlay / SurfaceFlinger) screen-off path so a held bluesleep does not block
// suspend-to-RAM while an app is foreground. Defined in NanoMenuInput.cpp; mirrors
// the enterDrmSleep fix for nano's own DRM-home sleep. `disabled` is the caller's latch.
void nanoBtLpmSuspendGate(bool screenOff, bool& disabled);

// Read a ~91-char-safe indexed sysprop package list ("pkg,pkg" in baseProp plus
// baseProp_1, baseProp_2, ... continuations). Defined in NanoMenuPS3Menu.cpp; shared so
// loadInstalledApps can honour persist.gammaos.nano.extra_apps (force-show vendor apps).
std::vector<std::string> nanoPkgListRead(const char* baseProp);

// Adapter that lets the shared NanoSliderHud spec draw through NanoMenu's
// private GL primitives (drawQuad/drawText/measureText). Defined in
// NanoMenuSystem.cpp; friended so the volume/brightness HUD matches the
// drastic-nano overlay 1:1.
struct NanoMenuSliderBackend;

static const int MAX_PARTICLES = 150;
static const int NUM_EFFECTS = 22; // total effect IDs (some disabled)

struct Particle {
    float x, y, vx, vy, size;
    float r, g, b, a;
    float life, phase;
};

struct GlyphInfo {
    float u0, v0, u1, v1;  // UV coords in atlas
    int bmpW, bmpH;        // raw bitmap size in atlas
    int bearingX, bearingY; // offset from baseline
    int advance;            // horizontal advance in pixels
    bool color;             // true for color emoji (BGRA)
    float scaleW, scaleH;  // display scale (for emoji normalization)
};

// Static, table-driven 1:1 copy of the web index.html DATA[] tree. One entry
// per menu item; children point into the same static array space. The tree is
// generated straight from the web source in NanoMenuPS3Data.h. Namespace scope
// so the static const tables in NanoMenuPS3Menu.cpp can reference them.
struct Ps3DataItem {
    const char* name;
    int icon;               // xmb_icon index (NNN); -1 = special (PS Store)
    const char* desc;       // may be null
    const char* value;      // may be null
    int action;             // 0 none, 1 dialog, 2 landing
    const Ps3DataItem* children;
    int childCount;
};
struct Ps3DataCat {
    const char* id;
    const char* name;
    int icon;               // category xmb_icon index
    const Ps3DataItem* items;
    int itemCount;
};

// Authentic PS3 XMB nav effect ids (firmware system_plugin). At namespace scope so both the file-static
// player array in NanoMenuPS3Boot.cpp and the nav call sites in NanoMenuPS3Menu.cpp can name them.
enum Ps3SfxId { PS3_SFX_CURSOR = 0, PS3_SFX_OK, PS3_SFX_BACK, PS3_SFX_CATEGORY, PS3_SFX_OPTION,
                PS3_SFX_ERROR, PS3_SFX_COUNT };

// DSi interactive SFX ids (nav_blip / app_launch / settings_nav / settings_back / settings_enter).
// At namespace scope so the file-static player array in NanoMenuPS3Boot.cpp and the call sites in
// other TUs (e.g. the Wi-Fi manage dialog in NanoMenuSettings.cpp) can name them.
enum NdsSfxId { NDS_SFX_NAV = 0, NDS_SFX_LAUNCH, NDS_SFX_SET_NAV, NDS_SFX_SET_BACK, NDS_SFX_SET_ENTER,
                NDS_SFX_COUNT };

// Minima (NextUI-inspired) interactive SFX ids: soft/premium nav/select/back/drill/error blips,
// composed for the minimal list theme. At namespace scope for the same reason as the sets above.
enum MinimaSfxId { MIN_SFX_CURSOR = 0, MIN_SFX_OK, MIN_SFX_BACK, MIN_SFX_DRILL, MIN_SFX_ERROR,
                   MIN_SFX_COUNT };

class NanoMenu : public Thread, public IBinder::DeathRecipient {
public:
    NanoMenu();
    virtual ~NanoMenu();

    // Shared volume/brightness HUD adapter (see NanoSliderHud.h).
    friend struct NanoMenuSliderBackend;
    // Recolouring adapter: draws the shared procedural HUD icons in a chosen ink colour
    // (for the DSi light-panel themed slider, where the white default would be invisible).
    friend struct IconInkBackend;

    sp<SurfaceComposerClient> session() const;

    // Overlay XMB: run as the power-hold in-game overlay (a translucent,
    // background-blurred SurfaceFlinger layer over the running app) instead of
    // the normal full-screen home. Set from main() before run() when invoked
    // with --overlay. See NanoMenuOverlay.cpp.
    void setOverlayMode(bool on) { mOverlayMode = on; }

    struct MenuItem {
        std::string label;
    };

    struct RecentEntry {
        std::string label;
        std::string romPath;
        std::string corePath;
        std::string coreName;
        std::string dbName;  // system/platform name from playlist
    };

    struct AppEntry {
        std::string packageName;
        std::string label;
    };

    // A user-chosen ROM scan location for a system. type 0 = raw filesystem
    // path, type 1 = SAF document-tree URI (resolved to rawHint for native
    // readdir, see the folder picker). Built-in systems with an empty
    // scanSources list fall back to the legacy default candidate paths.
    struct ScanSource {
        int type = 0;            // 0 = rawpath, 1 = safuri
        std::string value;       // raw absolute path, or content:// tree URI
        std::string rawHint;     // resolved raw mount for a safuri (optional)
        // 0 this folder only; 1, or -1 (added before the choice existed), with its subfolders.
        int subfolders = -1;
    };

    // Launch routing discriminator (persisted as launchType in the config).
    enum XmbLaunchType {
        XLT_LIBRETRO_CORE = 0,   // RetroArch native-core / QR cache route (today's RA path)
        XLT_RETROARCH_INTENT = 1,// RetroArch via an am intent (content URI / extras)
        XLT_CUSTOM_PACKAGE = 2,  // arbitrary package via am intent (Daijisho-style)
    };

    struct XmbSystem {
        std::string id;            // stable unique key; for built-ins == original romDir
        bool builtin = true;       // seeded from kXmbSystemDefs (enables "reset to default")
        bool enabled = true;       // disabled systems are hidden from Game and never scanned
        int order = 0;             // explicit sort key within the Game category
        std::string name;
        std::string shortname;
        std::string romDir;        // Directory name under ROMs/ (also default scan-path key)
        std::string coreSo;        // RetroArch core .so filename (empty for standalone)
        std::string launchPkg;     // Package name for standalone emulators
        std::string launchIntent;  // Intent template ({file.uri} placeholder)
        int launchType = XLT_LIBRETRO_CORE;
        std::string packageName;   // custom-package target (mirrors launchPkg)
        std::string launchArgs;    // extra am tokens appended to the intent (custom-package)
        std::string iconRef;       // builtin:N | retroarch:name | core:name | file:/abs | ""
        std::vector<ScanSource> scanSources; // user-chosen scan locations (empty = legacy default)
        // Default folder-name aliases the user has removed from this system's scan (lowercased,
        // e.g. "sfc"). Only consulted when scanSources is empty (default-scan mode); a removed alias
        // is skipped in buildScanCandidates so its ROMs/<alias> folders are no longer scanned.
        std::vector<std::string> disabledDefaultFolders;
        float iconR = 1.0f, iconG = 1.0f, iconB = 1.0f; // Icon color (tint)
        std::string acceptExts;    // Comma-separated accepted extensions
        std::string activePath;    // Primary path (largest collection) — for backward compat
        std::vector<std::string> activePaths; // ALL directories with ROMs for this system
        std::vector<std::string> roms;         // Sorted FULL PATHS (e.g., /storage/UUID/nes/game.nes)
        std::vector<std::string> displayNames; // Pre-stripped display names (parallel to roms)
        bool scanned = false;
        bool pathExists = false;
        int64_t lastScanTime = 0; // elapsedRealtime() of last scan - for periodic rescan
        uint32_t scanInputsGen = 0; // bumped when its folders or extensions change
        // Boxart/cover scraper per-system overrides (empty = inherit the global
        // Settings). scraperOverride: ""=default, "screenscraper", "thegamesdb",
        // "off". scrapeUser/scrapePass override the ScreenScraper account; scrapeKey
        // overrides the TheGamesDB key; scrapePlatform forces a ScreenScraper
        // systemeid when the auto romDir->platform map is wrong.
        std::string scraperOverride;
        std::string scrapeUser;
        std::string scrapePass;
        std::string scrapeKey;
        std::string scrapePlatform;
        bool isStandalone() const { return !launchPkg.empty(); }
    };

    struct SearchResult {
        int sysIdx;
        int gameIdx;
    };

    struct XmbRecentEntry {
        std::string romPath;       // Full ROM path for launch
        std::string coreSo;        // Core .so or empty for standalone
        std::string launchPkg;     // Package for standalone
        std::string launchIntent;  // Intent template for standalone
        std::string displayName;   // Game display name
        std::string systemName;    // System shortname (e.g. "NES")
        std::string romDir;        // ROM directory name
        bool standalone;
    };

    // A user Collection: a named group of games that can span systems (e.g. "TAC" holding
    // FF Tactics + Fire Emblem). Stored as ROM paths; resolved to the live system/rom at
    // display time so a collection game reuses the normal PS3_ROM launch/boxart/option menu.
    struct XmbCollection {
        std::string name;
        std::vector<std::string> roms;   // full ROM paths
        std::vector<std::string> esdeUnresolved;   // ES-DE collection lines naming no game here (kept as they are)
        std::vector<std::string> esdeRead;         // roms as read from its ES-DE file
        std::unordered_map<std::string, std::string> esdeLines;   // rom -> its line there
    };

    enum MenuState {
        MENU_MAIN = 0,
        MENU_RECENT = 1,
        MENU_APPS = 2,
        MENU_WIFI = 3,
        MENU_BT = 4,
        MENU_SETTINGS = 5,
        MENU_SETUP_WIZARD = 6,
    };

    enum SetupWizardStep {
        SETUP_WELCOME = 0,
        SETUP_LANGUAGE,
        SETUP_WIFI,
        SETUP_DATETIME,     // online: the date, time and formats found from the internet, to confirm
        SETUP_TIMEZONE,
        SETUP_INSTALLING,
        SETUP_SDCARD,       // a microSD card is in: keep ES-DE / emulator files on it (On/Off)
        SETUP_SDMOVE,       // the moves chosen there, without questions
        SETUP_BIOS,         // the BIOS dialog: install the files the emulators need from a folder
        SETUP_FINISH,
        SETUP_STEP_COUNT,
    };

    struct TimezoneEntry {
        std::string id;
        std::string display;
        int offsetMinutes;
        float lon = 0.0f;   // city longitude (deg) for the 3D globe selector
        float lat = 0.0f;   // city latitude (deg)
    };

    struct SettingsItem {
        std::string label;
        int action; // 0 = Wi-Fi screen, 1 = Bluetooth screen
    };

    struct WifiNetEntry {
        std::string ssid;
        std::string bssid;
        int rssi;            // dBm (e.g. -55)
        int security;        // 0=none, 1=wep, 2=wpa/wpa2, 3=wpa3, 4=owe
        int savedNetId;      // -1 if not saved
        bool connected;
    };

    struct BtDevEntry {
        std::string name;
        std::string address;
        bool bonded;
        bool connected;
        int  cod = 0;   // Bluetooth class-of-device (for the "Type" column)
    };

private:
    // Hold-to-repeat navigation direction. Declared here (before use in
    // member function signatures) so navPress/navRelease can reference it.
    enum class NavDir : int { None = 0, Up, Down, Left, Right };

    virtual bool        threadLoop();
    virtual status_t    readyToRun();
    virtual void        onFirstRef();
    virtual void        binderDied(const wp<IBinder>& who);

    // Input handling
    void openInputDevices();
    void checkInputHotplug();
    void pollInput();
    bool keyHeld(int code) const;
    // DRM-direct sleep: blank panels + backlights, drive PowerManager
    // suspend, block until a wake (power press or lid open), then
    // re-commit the modeset and restore. Shared by the power short-press
    // and the lid-close handlers. Returns false if the legacy
    // pre-boot_completed timeout initiated a shutdown (caller returns).
    bool enterDrmSleep();
    // In-process park across a drastic-nano DRM session (NanoMenuDrasticPark.cpp):
    // frees the panel/ring/memory for the game, blocks until session_done, then
    // takes the panel back on the same EGL context and resumes this menu state.
    bool drasticParkEnabled() const;
    bool drasticParkSession();
    void drasticParkPageOutAll();   // park: every mapping to zram, file-backed included
    std::atomic<bool> mDrasticParked{false};
    // CPU clock/governor while the screen is off: drop to powersave on screen-off and
    // re-apply the user's persisted performance mode on wake (NanoMenu.cpp).
    void nanoApplyPerfClock(const char* mode);   // run /vendor/bin/setclock_<mode>.sh (validated)
    void nanoRestorePerfClock();                 // re-apply persist.gammaos.performance_mode
    void handleUp();
    void handleDown();
    void handleLeft();
    void handleRight();
    // Hold-to-repeat helpers. navPress() fires the matching handle* once
    // and arms the repeat tick; navRelease() clears it (pass NavDir::None
    // to clear unconditionally, e.g. on HAT/stick return to center when
    // we don't know which specific axis direction was held).
    // tickNavRepeat() is invoked once per frame from pollInput() and
    // fires handle* at an accelerating cadence while a direction is held.
    void navPress(NavDir dir);
    void navRelease(NavDir dir);
    void tickNavRepeat();
    void handleSelect();
    void handleBack();
    void loadRecentPlaylist();
    void loadInstalledApps();

    // Installed web browsers for the "Default Browser" picker. Unlike mAppEntries
    // (which filters out com.android.* / com.gammaos.* / system apps), this list is
    // curated by the framework (SystemServer.writeNanoBrowserCache -> ACTION_VIEW https
    // handlers) and written to /data/system/nano_browsers.txt as "pkg|Label|component".
    // component is the flattened ACTION_VIEW activity (pkg/Activity) so launchUrl can
    // hand ANY browser a resolvable ACTION_VIEW intent.
    struct BrowserEntry {
        std::string packageName;
        std::string label;
        std::string component;   // flattened pkg/Activity for the ACTION_VIEW handler
    };
    std::vector<BrowserEntry> mBrowserEntries;
    bool mBrowsersLoaded = false;
    int  mBrowsersGen = -1;                 // last sys.gammaos.nano.browsers_generation seen
    void loadInstalledBrowsers();           // parse /data/system/nano_browsers.txt
    void ensureBrowserList();               // reload on browsers_generation change; seed if absent
    std::string browserLabelForPkg(const std::string& pkg);  // human label for the value column

    // Launchable activities for the gamepad "Launch Activity" remap-action picker.
    // Written by SystemServer.writeNanoActivityCache (ACTION_MAIN + LAUNCHER) to
    // /data/system/nano_activities.txt as "pkg|Label|pkg/Activity" lines.
    struct ActivityEntry {
        std::string packageName;
        std::string label;
        std::string component;   // flattened pkg/Activity for the daemon's -n launch
    };
    std::vector<ActivityEntry> mActivityEntries;
    bool mActivitiesLoaded = false;
    int  mActivitiesGen = -1;
    void loadInstalledActivities();   // parse /data/system/nano_activities.txt
    void ensureActivityList();        // reload on activities_generation change

    // Every activity of every launchable app (nano_pkg_activities.txt), grouped by package - feeds
    // the slide "Launch Target" app -> activity picker (all activities, not just launchers).
    std::vector<ActivityEntry> mPkgActivityEntries;
    bool mPkgActivitiesLoaded = false;
    int  mPkgActivitiesGen = -1;
    void loadPackageActivities();     // parse /data/system/nano_pkg_activities.txt
    void ensurePackageActivities();   // reload on activities_generation change
    std::string mLaunchPickPkg;       // the package whose activity list the launch-target picker shows
    std::string launchTargetLabel(const std::string& value);              // human summary for the bound row
    // buildLaunchTargetAppSubmenu / buildLaunchTargetActivitySubmenu are declared with the other
    // Ps3Level builders further down (Ps3Level is not defined yet here).

    // XMB mode
    void initXmbSystems();
    // Dynamic systems config (/data/system/nano_systems.json, DE storage).
    // loadSystemsConfig parses the JSON into mXmbSystems; seedSystemsConfig
    // builds the default config from kXmbSystemDefs (+ legacy props) and
    // writes it; saveSystemsConfig serializes mXmbSystems back atomically.
    bool loadSystemsConfig();
    void seedSystemsConfig();
    void saveSystemsConfig(bool quiet = false);   // quiet: no banner when it cannot be saved now
    // Cross-process config coherence: nanosecond mtime stamp of
    // nano_systems.json (-1 when absent) and the stamp of this process's last
    // load/save. The threadLoop polls the file and reloads when an external
    // nano (overlay vs DRM home) rewrote it.
    int64_t systemsConfigStamp() const;
    int64_t mSystemsCfgStamp = -1;
    // Render thread (NanoMenuHakux.cpp): hakuX's games folders kept to the Xbox system's ROM folders
    // (nanohakux::sync), after a systems save and when the home starts, as the RetroArch heal is.
    void hakuxFoldersFollow();
    // Render thread: gives systems saved with Nano's own table the build's cores, and catalog-made
    // systems whose player cannot run one that can, once RetroArch's cores can be read
    // (nanosys::staleCoreRepairs, nanosys::playerRepair). Once per config: a healed one says so
    // ("healed" in nano_systems.json, mSystemsHealDone), so a player the user picks later is kept.
    void systemsHealTick();
    bool mSystemsHealChecked = false;
    bool mSystemsHealDone = false;
    int64_t mSystemsHealNextMs = 0;
    // Render thread, from systemsHealTick: a system for each split-off folder holding games that no
    // system scans (nanosys::splitFolderAdds), once per config ("healVersion" 2, mSplitHealDone). The
    // folders are found on a worker (mSplitHealScan 1, then 2 with mSplitHealFound).
    void splitFoldersHealTick();
    bool mSplitHealDone = false;
    int64_t mSplitHealNextMs = 0;
    std::atomic<int> mSplitHealScan{0};
    std::mutex mSplitHealLock;
    std::vector<nanosys::FoundFolder> mSplitHealFound;   // guarded by mSplitHealLock
    std::vector<std::string> mSplitHealIds;              // the catalog platforms the worker matched
    // Home category order + visibility (/data/system/nano_categories.json, DE
    // storage). Each entry is a stable kPs3DataCats id ("settings","photo",...)
    // paired with its shown/hidden flag. buildPs3Cats iterates this instead of
    // the source order so the user can hide + reorder the six data categories
    // (Quick Menu stays pinned first; Settings can never be hidden). Loaded once
    // in initPs3Menu and re-polled cross-process like nano_systems.json.
    std::vector<std::pair<std::string,bool>> mCatOrder;
    int64_t mCatOrderCfgStamp = -1;
    bool mCatOrderStale = false;
    std::string catOrderPath() const { return "/data/system/nano_categories.json"; }
    int64_t catOrderConfigStamp() const;
    void loadCatOrder();
    void saveCatOrder();
    void catOrderToggle(int idx);               // X: flip shown/hidden (anti-lockout guarded)
    void catOrderReorder(int idx, int dir);     // L1/R1: move a category up (-1) / down (+1)
    void catOrderRebuildCats();                 // rebuild home cats, keep focus on the same column by name
    // Per-item show/hide for STATIC submenu rows (Theme Settings > Home Categories > drill a
    // category). Stored in the SAME nano_categories.json (a "hiddenItems" string array), so it
    // shares the atomic write + cross-process stamp reload. The compound id is
    // "<catId>/<parentPath>/<itemName>" using the static Ps3DataItem.name of every ancestor,
    // which is stable (never relabelled) and unique within its sibling list. Applied at the two
    // choke points buildPs3Cats (first-level rows) + buildDataSubmenu (nested rows), so a hide
    // covers all three home themes (XMB / DSi / Minima) with no per-theme render change.
    std::unordered_set<std::string> mHiddenItems;
    bool isItemHidden(const std::string& id) const;   // true if this compound id is in the hidden set
    void itemHideToggle(const std::string& id);       // flip hidden/shown for a submenu item (anti-lockout guarded)
    // Max directory depth for recursive ROM subfolder scanning when the
    // "Scan ROM Subfolders" toggle (persist.gammaos.nano.rom.recursive) is on.
    // 0 = top level only (toggle off = the legacy one-level behavior).
    // (kRomScanMaxDepth is a file-static const in NanoMenuXmb.cpp - only the scanners need it.)
    // Consolidated ROM scan candidate-path builder (replaces the duplicated
    // logic in scanRomPaths / scanOneSystemAsync / bgScanThreadFunc). Honors
    // scanSources when present, else reproduces the legacy default candidates.
    std::vector<std::string> buildScanCandidates(const XmbSystem& sys);
    // DE cache path for a system's ROM list, keyed on the stable id.
    std::string xmbCachePath(const XmbSystem& sys) const {
        return "/data/system/nano_xmb_cache/" + sys.id + ".list";
    }
    void scanRomPaths();
    void forceRescanAllSystems();
    // Drop the ROM caches + scanned flags and kick a fresh library rescan. Called
    // when the Scan ROM Subfolders / Group Multi-Disc (.m3u) toggles change so the
    // grouping baked into the cache is re-derived with the new setting.
    void romRescanFromSettings();
    bool scanOneSystemAsync(int sysIdx);
    void renderXmb();
    void launchXmbGame();
    void launchInstalledApp(const std::string& pkg, bool returnToApps);
    std::string appShortcutPackage(const std::string& romPath);
    bool launchSelectedAppShortcut();
    // The Android Games file naming pkg (a shown one before a hidden one), or "". hidden (may be
    // null) tells whether the returned file is hidden.
    std::string androidGameFile(const std::string& pkg, bool* hidden);
    void androidGameToggle(const std::string& pkg, const std::string& label);   // Applications options
    // The PNG is a game's icon the Android Games helper saved as its art (it marks what it writes).
    static bool androidGamesIcon(const std::string& path);
    void loadXmbRecent();
    void saveXmbRecent();
    void addXmbRecent(int sysIdx, int gameIdx);
    // True when the ROM is really there (content:// URIs are left to the framework's SAF layer).
    // Every launch path checks this so a deleted game reports itself instead of dying as a
    // "crashed" app launch.
    bool romFileExists(const std::string& romPath);
    // Launch-time existence check. Unlike romFileExists (which assumes a network share is present so
    // the render-thread pruner never blocks on a sleeping NAS), this DOES stat a share path - it runs
    // once, for the single game being launched, where a game deleted server-side must be caught before
    // the emulator black-screens on it (see the Recently Played launch guards).
    bool romLaunchExists(const std::string& romPath);
    bool romParentDirReachable(const std::string& romPath);  // true if the file's folder is stat-able
    void recentRemoveAt(int idx);     // drop one Recently Played row (a live stat proved it is gone)
    void showRomMissingMsg(const std::string& displayName);
    void showUnlaunchableNameMsg(const std::string& displayName);   // tab / line break in the file name
    // {file.content} for a launch: the ID in the game file when the template uses it ("" when it
    // does not). False, with a message shown, when it is used and the file holds no valid ID.
    bool launchFileIdFor(const std::string& tmpl, const std::string& args, const std::string& romPath,
                         const std::string& displayName, std::string& id);
    // Pre-launch emulator guard (ES-DE style, checked lazily at launch). coreSoExists stats the
    // RetroArch core .so; packageInstalled scans /data/system/packages.list for a standalone package.
    // Both fail OPEN (return true) if the check itself cannot be performed, so a transient FS state
    // never blocks a valid launch. showEmuMissingMsg surfaces a theme-agnostic toast and we do not launch.
    bool coreSoExists(const std::string& coreSo);
    bool packageInstalled(const std::string& pkg);
    void showEmuMissingMsg(const std::string& displayName, bool standalone);
    void pruneStaleRecentEntries();   // drop Recently Played rows whose ROM is gone
    void gamesRefresh();              // Settings > Game Settings > Rescan Games
    // Centred message on the home menu, reusing the launch toast's panel (which the main render
    // path draws, so it is visible here - the photo viewer's message helper is not).
    void showXmbMessage(const std::string& line1, const std::string& line2 = std::string(),
                        int frames = 180);
    std::string mBusyLine1, mBusyLine2;   // empty = the default "Booting up..." launch wording
    bool mRecentPrunePending = false; // prune the recents once a rescan's results land

    // On-screen keyboard. Native reimplementation of the Leanback IME keyboard
    // extended into a multi-script input method. Runtime state is mOsk
    // (NanoOskState); layout data is the generated kOskKb[] / kOskPopups[].
    // Implementation lives in NanoOsk.cpp.
    void openOsk();                 // search-mode OSK
    void closeOsk();                // dismiss (popup-aware)
    void oskType(char c);           // legacy thin ASCII insert, kept for callers
    void oskBackspace();            // delete one codepoint before the caret
    void oskConfirm();              // submit (search -> results; password -> callback)
    void updateSearchResults();
    void renderOsk();
    // New OSK runtime (NanoOsk.cpp):
    void oskApplyLocale();          // choose layout from the active UI language
    void oskSetLanguage(const char* code, const char* region); // layout + IME
    void oskMoveCursor(NavDir dir); // geometric nearest-in-direction focus move
    void oskActivateKey(const OskKey& key); // dispatch a grid key (A on key)
    void oskTouchFrame();                   // SYN_REPORT: normalize + dispatch the live touch (secondary-panel OSK)
    void oskTouchAt(float px, float py, bool tap); // hit-test a logical point against the keys
    bool mOskOnSecondary = false;           // the OSK draws on the secondary panel (render's oskOnSecondary)
    // XMB touch (NanoMenuPS3Menu.cpp, through mTouch): the home column and category bar, the
    // option panel, the dialogs and the pickers register their inputs as they draw.
    void  xmbCancelTouchScroll();             // a D-pad press takes over: stop a fling, drop a resting finger
    bool  mXmbTouchStepping = false;          // a touch drag or fling is stepping the menu itself
    bool  xmbHomeTouch() const;               // the home column / category bar take touch (no modal over them)
    bool  xmbTouchItemStep(int depth, int dir);
    bool  xmbTouchCatStep(int dir);
    void  xmbTouchCat(int cat);
    void  xmbTouchRow(int depth, int cat, int row, bool hold);
    void  xmbOptTouchPanel(float panelLeft, float pitch);
    bool  xmbOptTouchStep(int dir);
    void  xmbOptTouchRow(int row);
    void  xmbOptTouchSubRow(int parent, int row);
    bool  xmbDlgIs(int kind, int type) const;
    void  xmbDlgTouchBase();
    void  xmbDlgTouchSide(float panelLeft);
    bool  xmbDlgTouchStep(int kind, int type, int dir);
    void  xmbDlgTouchRow(const TouchRect& r, int idx, bool button = false);   // button: acts as the finger lands
    void  xmbDlgTouchList(const TouchRect& area, float left, float width, float top, float pitch, int first, int last);
    void  xmbDlgTouchScroll(const TouchRect& area, float pitch, int maxScroll);
    void  xmbDlgTouchSlider(const TouchRect& panel, const TouchRect& row, float trackL, float trackW, float knobX);
    void  pickerTouchList(bool tz, const TouchRect& area, float left, float width, float top, float pitch, int first, int last);
    bool  pickerTouchStep(bool tz, int dir);
    void  pickerTouchPick(bool tz, int idx);
    // A one-line text legend of button hints at (x, y): each part presses its code, in order.
    void  touchHintText(const std::string& text, float x, float y, float scale, std::initializer_list<uint16_t> codes);
    void  brightSliderTouch(const TouchRect& panel, const TouchRect& bar);   // Quick Menu brightness slider modal
    void  wizTouchList(const TouchRect& area, float left, float width, float top, float pitch, int first, int last, int n);
    void  wizTouchRow(const TouchRect& r, int idx);   // the XMB network wizard: tap picks and confirms
    // The media players register their controls as touch zones while they draw (touchZonesFrame);
    // only the photo viewer's pinch / pan / follow-the-finger swipe stay in a handler of their own.
    bool  touchLogicalPx(float& px, float& py);   // shared raw digitizer -> logical pixel (swap/flip; DRM+SF)
    bool  pvTouchGesture();                        // photo viewer pinch, pan and drag; true = it took the touch
    // A seek bar drawn at bar: a tap or a drag along it calls seek with the fraction under the finger.
    void  touchSeekBarZone(const TouchRect& bar, std::function<void(float)> seek);
    // A modal's two button hints drawn as one line at r: the left half presses a, the right half b.
    void  touchHintPairZone(const TouchRect& r, uint16_t a, uint16_t b);
    void  photoGridScrollTo(int topRow);           // clamp + set the grid's top visible row
    void  photoGridOpenAt(int idx);                // select + open a thumbnail into the viewer
    void  photoGridBack();                         // close the thumbnail grid (touch back button)
    bool  photoGridTouchStep(int dir, float px, float maxScroll);   // a drag scrolls the grid px at a time
    void  photoGridTouchCell(const TouchRect& r, int idx);          // tap opens, hold opens its options
    void oskAPress();               // A pressed while OSK active
    void oskARelease();             // A released while OSK active
    void oskTick();                 // per-frame: long-press popup + animation clock
    void oskToggleShift();          // off <-> on (from locked -> off)
    void oskToggleCaps();           // caps lock toggle
    void oskToggleSym();            // ABC <-> SYM page
    void oskOpenPopup(const OskKey& key);  // open accent/shift mini popup
    void oskClosePopup();
    void oskCommitPopupCell();      // commit the focused popup cell
    void oskCycleLanguage(int dir); // in-keyboard language switch
    void oskInsertCp(uint32_t cp);  // insert a codepoint at the caret (UTF-8)
    void oskInsertString(const std::string& s); // insert a UTF-8 string at the caret (paste)
    void oskPaste();                // Y: request the system clipboard, insert it on reply
    void oskCaretLeft();
    void oskCaretRight();
    OskBox oskLayoutBox();          // compute the aspect-aware keyboard box
    const char* oskToggleRect(const OskBox& b, float& x, float& y, float& w, float& h);
    void oskSetTouch(bool touch);
    float oskTouchKeyScale() const;
    bool oskButtonLeavesTouch();
    bool mLastInputTouch = false;   // the last input was a touch (a finger or a mouse click), not a button
    const OskKeyboard* oskCurrentKb() const; // current page's keyboard table

    // Settings column (WiFi + Bluetooth). The Settings entry lives as an
    // extra pseudo-system at index == mXmbSystems.size() in the XMB column
    // bar. Selecting it shows a vertical list of {"Wi-Fi", "Bluetooth"}.
    // Selecting an item from that list opens a full-screen sub-menu.
    void initSettingsItems();
    void renderSettingsList(float selIconX, float iconBarY, float iconSpacingV,
                            float iconSize, float sf,
                            float textScale, float selTextScale);
    bool isOnSettingsColumn() const;

    // Wi-Fi screen state + helpers
    void openWifiScreen();
    void closeWifiScreen();
    void refreshWifiList();              // blocking: list-networks + scan + list-scan-results
    void startWifiScanAsync();           // kick a background scan thread
    void wifiScanThreadFunc();
    int  wifiRealApCount();              // count of real (non-toggle) Wi-Fi entries in the live list
    void renderWifiScreen();
    void handleWifiScreenSelect();
    void handleWifiScreenUp();
    void handleWifiScreenDown();
    void handleWifiScreenX();            // manual rescan
    void handleWifiScreenY();            // forget / remove the selected saved network
    void connectToSavedWifi(int savedNetId);
    void addAndConnectWifi(const std::string& ssid, int security,
                           const std::string& password, bool force = false);
    void connectWithWizardSettings();   // applies the wizard's static IP/DNS/MTU/proxy
    void forgetWifiNetwork(int savedNetId);
    // Manage dialog for an already-saved network: Connect / Change Password /
    // Forget (Disconnect when connected). Opened from handleWifiScreenSelect
    // when a saved row is activated, so a wrong saved password can be corrected
    // in place instead of the framework silently retrying the old key.
    void openWifiManage(const WifiNetEntry& e);
    void wifiManageMove(int dir);        // -1 up / +1 down through the option rows
    void wifiManageActivate();           // run the selected option
    void wifiManageClose();
    bool wifiTouchStep(int dir);        // a drag along the Wi-Fi list moves the selection
    void renderWifiManage();            // themed overlay (XMB dark panel / DSi glossy card)
    // Watch a connect attempt and surface a clear "wrong password" result instead
    // of an endless silent retry. Runs on a detached thread. On a wrong-password
    // outcome for a secure network it shows a clear message and arms
    // mWifiRepromptPending so the WiFi screen re-opens the password OSK.
    void wifiConnectWatch(const std::string& ssid, int security);
    void toggleWifiRadio(bool on);
    bool wifiRadioEnabled();   // live Wi-Fi radio state (for the Internet Connection toggle)
    // Network Settings dialogs backed by the live system state:
    std::string buildNetStatusBody();    // real SSID/IP/gateway/DNS/MAC for the status list
    std::string buildSysInfoBody();      // real build/model/serial/MAC/IP/storage for System Information
    void startNetTest();                 // async connectivity test (IP / internet / DNS)
    void stopNetTest();                  // stop + join the test thread

    // Bluetooth screen state + helpers
    void openBtScreen();
    void closeBtScreen();
    void refreshBtList();                // blocking: list bonded + list scanned
    void discoverBtDevices();            // blocking: ~8 s discovery run
    void startBtScanAsync();
    void startBtDiscoveryAsync();
    void btScanThreadFunc();
    void btDiscoveryThreadFunc();
    void renderBtScreen();
    void handleBtScreenSelect();
    void handleBtScreenUp();
    void handleBtScreenDown();
    void handleBtScreenX();              // rescan
    void handleBtScreenY();              // unpair selected bonded device
    void pairBtDevice(const std::string& mac);
    void unpairBtDevice(const std::string& mac);
    void connectBtDevice(const std::string& mac);
    void toggleBtRadio(bool on);

    // OSK password extension: same visual OSK, but types into a password
    // field rendered with masked chars, and on Enter invokes a callback.
    void openOskForPassword(const std::string& prompt,
                            std::function<void(const std::string&)> onSubmit);
    std::string maskPassword(const std::string& s);

    // Hierarchical settings tree browser (MENU_SETTINGS)
    void buildSettingsTree();
    void openSettingsTree();
    void settingsTreePushCategory(int nodeIdx);
    bool settingsTreePop();
    void settingsTreeGetChildren(int parentIdx, std::vector<int>& out) const;
    std::string settingsTreeBreadcrumb() const;
    void renderSettingsTree();
    void handleSettingsTreeSelect();
    void handleSettingsTreeBack();
    void handleSettingsTreeUp();
    void handleSettingsTreeDown();
    void handleSettingsTreeLeft();
    void handleSettingsTreeRight();
    void startSettingsValueRefresh();
    std::string getSettingsCachedValue(int nodeIdx) const;
    void settingsToggleValue(int nodeIdx);
    void settingsCycleListValue(int nodeIdx, int direction);
    void settingsSetTextValue(int nodeIdx, const std::string& val);

    // Setup wizard (NanoMenuSetupWizard.cpp)
    void startSetupWizard();
    void finishSetupWizard();
    void renderSetupWizard();
    // Overlay a "Start: Skip" hint (top-right) on the Wireless / Bluetooth steps
    // so the user knows Start skips them. Drawn on top of the net/BT wizard.
    void drawSetupSkipHint();
    // DSi theme: draw the DSi home background field (or the panel wallpaper) plus a
    // dim + blue tint (matching the XMB dialog backdrop) behind the whole setup wizard,
    // so the forced-XMB wizard chrome stays readable and reads as the DSi theme.
    void renderSetupNdsBackdrop();
    void renderSetupWelcome();
    void renderSetupWifiStep();
    void renderSetupTimezone();
    void renderSetupDateTime();
    void renderSetupInstalling();
    void renderSetupFinish();
    void renderSetupProgressDots();
    void handleSetupSelect();
    void handleSetupBack();
    void handleSetupUp();
    void handleSetupDown();
    void handleSetupStart();
    void advanceSetupStep();
    void goBackSetupStep();
    void setupGoTo(SetupWizardStep step, bool back = false);
    bool setupOnline();
    bool setupWifiOn();
    void setupDateTimeTick();
    void setupDateTimeSelect();
    void setupDateTimeCycle(int dir);
    void updateSetupTransition();
    void buildTimezoneList();
    void startSetupScript();
    void stopSetupLogThread();
    void setupLogTailThreadFunc();
    void renderSetupLanguage();
    void handleSetupLanguageSelect();

    // 12/24-hour clock: nano (bootanim domain) cannot read Settings, so it follows the
    // persist.gammaos.nano.clock12 prop mirror (SystemServer ContentObserver + nano's own
    // Time Format toggle write it). property_get is a cheap shmem read, refreshed per frame.
    std::atomic<bool> mClock12h{false};
    void clockRefreshMaybe();                                     // re-read persist.gammaos.nano.clock12
    void formatClockHM(char* buf, size_t n, const struct tm& t);  // "3:22 PM" (12h) or "15:22" (24h) per the setting

    // Quick Resume
    void prepareShutdown(const char* action);
    bool isRetroArchRunning();

    // Brightness control
    void adjustBrightness(int direction);
    bool setBrightnessViaHal(int brightness);
    void applyBrightness();
    void syncBrightnessToAndroid();
    int readAndroidBrightness();
    void renderBrightnessBar();
    // DSi / Minima themed volume+brightness HUD (the XMB / in-app / drastic path keeps the shared
    // flat system slider from NanoSliderHud.h). isVolume picks the icon; slot stacks the panels.
    void renderThemedSliderHud(bool isVolume, int pct, int slot);

    // Volume control
    void adjustVolume(int direction);
    void renderVolumeBar();
    int readSysfsInt(const char* path, int fallback);
    void writeSysfsInt(const char* path, int value);

    // GammaOS Nano: launch-readiness gate. Returns true once the system
    // is far enough through boot for the home-launch path in
    // RootWindowContainer.startHomeOnTaskDisplayArea() to actually
    // accept a launch. Without this gate, pressing A on a game in the
    // few seconds between NanoMenu paint and user 0 unlock leaves the
    // screen blank: NanoMenu exits, bootanim exits, but RWC's nano
    // launch branch returns false (user not unlocking yet) so no app
    // ever takes over the display.
    bool isLaunchReady() const;
    // Arm the centred "Booting up" overlay AND mark the press as a
    // queued launch -- the main loop re-fires handleSelect() as soon
    // as isLaunchReady() flips to true, so the user does not have to
    // press A a second time after boot finishes.
    void showLaunchBusyToast();
    // Drop the queued launch (called from navigation handlers and
    // handleBack so navigating away cancels the pending launch).
    void cancelPendingLaunch();
    void renderLaunchBusyToast();

    // Battery HUD
    void pollBattery();
    void pollVolume();   // refresh mVolume from the PWM-published prop when not mid-burst
    // DSi top-screen status indicators (replace the username): wifi / bluetooth / audio.
    // Throttled sysfs+procfs poll so the status bar reflects the real radio/audio state.
    void pollNdsStatus();
    int  mNdsStatusPollTicks = 0;   // frames until next radio/audio state read
    int  mNdsWifiState = 0;         // 0 off, 1 on-not-associated, 2 connected
    bool mNdsBtOn = false;          // bluetooth radio unblocked
    bool mNdsAudioActive = false;   // real audio coming out of the speaker (ALSA pcm RUNNING)
    // Returns the right-edge X (in surface pixels) of the whole battery
    // indicator (icon + text). Network HUD chains its own icons from this
    // x so the layout scales cleanly with resolution / orientation.
    float renderBatteryIndicator();

    // Network HUD (WiFi + BT). Real polling happens on a background thread
    // because the underlying 'cmd wifi status' / dumpsys calls are slow
    // enough (100-400ms on a cold first call, 10-50ms steady state) that
    // running them on the render thread would spike the frame budget.
    // Render thread only reads the cached state behind mNetStateMutex.
    enum WifiLevel {
        kWifiLevel_Unknown = 0,
        kWifiLevel_Off,
        kWifiLevel_Disconnected,  // radio on, no network
        kWifiLevel_Connected,     // add mWifiBars for signal strength 0..4
    };
    enum BtLevel {
        kBtLevel_Unknown = 0,
        kBtLevel_Off,
        kBtLevel_On,
        kBtLevel_Connected,       // at least one device connected
    };
    void startNetPollThread();
    void stopNetPollThread();
    void netPollThreadFunc();
    // Draws WiFi then BT icons + short label starting at startX. Returns
    // the x position right after the last-drawn element so callers can
    // chain more HUDs to the right.
    float renderNetworkIndicators(float startX, float rowY, float rowH,
                                  float sf, float textScale);
    void drawWifiIcon(float x, float y, float sf, int bars,
                      float r, float g, float b, float a);
    void drawBtIcon(float x, float y, float sf,
                    float r, float g, float b, float a);

    // Rendering
    void initShaders();
    void initFonts();
    // Startup file work (font faces, icon PNG decode, systems/ROM-cache reads) run on a worker
    // while the render thread brings EGL up; initFonts/initIconTextures take its results.
    struct StartupPreload;
    std::shared_ptr<StartupPreload> mStartupPreload;
    void startupPreloadStart();
    StartupPreload* startupPreloadJoin();
    // Rasterize (if needed) and return the glyph cached at rasterPx device px.
    // Mono glyphs are rendered at that exact size (crisp, evenly hinted, no
    // fractional bitmap scaling); color emoji use their fixed strike normalized
    // to mFontSize regardless of rasterPx. Returns nullptr if unavailable.
    // fracEm > 0 (ES-DE render only) rasterises at the FRACTIONAL em via FT_Set_Char_Size to match
    // real ES-DE (es-core Font FT_Set_Char_Size(size*64)); 0 keeps the integer FT_Set_Pixel_Sizes path
    // used by XMB/DSi/Minima. For integer em the two are identical, so those modes are byte-unchanged.
    const GlyphInfo* ensureGlyph(uint32_t codepoint, int rasterPx, int preferFace = -1, float fracEm = 0.0f);
    // Toggle anti-aliased (mipmapped) minification on the glyph atlas. Scoped to
    // the home-XMB menu content only (see renderPs3Xmb); off for dialogs, OSK,
    // setup wizard and legacy menus. Filter is texture-object state, so one call
    // per region covers every drawText within it.
    void setGlyphAtlasAA(bool on);
    void ccHeader(const char* english, float x, float y, float sc, float gap);   // NanoControlCenter.cpp
    void drawText(const char* str, float px, float py, float scale,
                  float r, float g, float b, float a, int preferFace = -1);
    float measureText(const char* str, float scale, int preferFace = -1);
    // Logical->visual bidi/shaping front-end for the glyph pipeline (see
    // nanoBidiVisual in NanoOsk.h). Returns str itself on the fast path.
    const char* textForDisplay(const char* str);
    // Enable a scissor covering the LOGICAL rect (x,y,w,h), correct for any
    // panel rotation AND flip (transforms the rect through sDrmRotMat, the
    // same composed matrix the vertex shaders apply). See the definition for
    // why the old per-site rotation switches were wrong on flipped panels.
    void scissorLogicalRect(float x, float y, float w, float h);
    void render();
    void drawQuad(float x, float y, float w, float h,
                  float r, float g, float b, float a);
    // Selected-label glow: lay the glyphs out ONCE and emit all 15 offset copies
    // (8 outer ring + 6 inner ring + 1 centre) in a single draw, instead of 15
    // separate drawText calls that each re-decode and re-lay-out the same string.
    // Bit-identical: each copy re-runs drawText's exact advance accumulation from
    // its own (px+dx, py+dy), only the glyph cache lookups are shared.
    void drawTextGlow(const char* str, float px, float py, float scale,
                      float oR, float iR, float outerA, float innerA, float mainA);
    // Flat-colour batch: while active, drawQuad/drawTriangle accumulate their NDC
    // vertices + per-vertex colour into one buffer instead of issuing a draw each;
    // flushSolidBatch() submits them in one glDrawArrays through mParticleProgram
    // (same uRotation as mShaderProgram, set per frame). Same vertices, submission
    // order and blend as the immediate path, so the composite is identical. Used
    // to collapse the clock chrome's ~230 tiny draws into one.
    void beginSolidBatch();
    void flushSolidBatch();
    void endSolidBatch();
    // GammaOS: Create EGL surfaces on every non-primary physical display so the
    // post-HWC render loop can drive wallpaper-only rendering on those panels.
    // Idempotent — first call wires the surfaces, subsequent calls are no-ops.
    void setupSecondaryEglSurfaces();
    // Deferred SF init: create SurfaceComposerClient + SF surface when
    // transitioning from DRM boot path to app launch.
    void initSurfaceFlingerPath();

    // ---- Overlay XMB (NanoMenuOverlay.cpp) ----------------------------------
    // The power-hold in-game overlay: the same PS3 XMB renderer presented on a
    // translucent, SF-background-blurred layer over the running app (the app
    // keeps running, SurfaceFlinger composites the blur). Gated by
    // persist.gammaos.nano.overlay; the layer is created hidden and toggled by
    // sys.gammaos.nano.show_overlay (set by PhoneWindowManager on power-hold).
    bool mOverlayMode = false;        // this process is the overlay instance
    bool mOverlayShown = false;       // overlay layer is currently visible + grabbing input
    bool mOverlayPagesLocked = true;  // overlay RSS pinned by the startup mlockall; released by
                                      // munlockall once parked behind an app, then left reclaimable
                                      // (demand-faults from zram on raise; NOT re-locked - see overlayShow)
    bool mOverlayGpuParked = false;   // XMB GPU assets dropped while parked behind an app (overlayGpuPark);
                                      // rebuilt by overlayGpuUnpark on the next raise
    GLuint mParkedCatTex[7] = {};     // category icon handles at park time, so overlayGpuUnpark can point
                                      // the open levels that copied them at the reloaded icons
    // OSK-over-app: an app (e.g. WebView web fields) requests nano's lightweight OSK
    // because the framework leanback IME (~130MB) gets OOM-killed on this 1GB device
    // under a heavy WebView. We raise the overlay in an OSK-only mode and hand the
    // typed text back via a file + the sys.gammaos.nano.osk_done prop.
    bool mOskOverApp = false;         // overlay is up purely to host an app's OSK request
    std::string mOskAppReqId;         // request id echoed back in osk_done
    std::string mOskAppDir;           // requesting app's files dir (in/out text files)
    uint32_t mOskAppGen = 0;          // live-typing generation counter for the app OSK
    std::string mOskAppLastBuf;       // last buffer streamed to the app (per-frame change gate)
    void oskPublishLive();            // stream the current OSK buffer to the app for live typing
    bool mOverlayInited = false;      // one-time blur/hide transaction applied
    int  mOverlayInitTries = 0;       // init retries while SF surface control is still null
    int  mOverlayBlurPx = 0;          // background blur radius (px), 0 if unsupported
    bool mOverlayOpaque = false;      // persist.gammaos.nano.overlay.opaque: opaque layer
                                      // (HWC direct scanout -> 60fps) vs translucent+SF blur
    // Universal performance hints (so the vendor power/DVFS stack can run nano's
    // UI in a higher mode without device-specific frequency writes). mHintSession
    // is an APerformanceHintSession* (void to keep the NDK header out of this
    // header). See perfHintInit/perfHintReport in NanoMenu.cpp. Each hint is
    // independently prop-gated for A/B testing:
    //   persist.gammaos.nano.perf.framerate  - setFrameRate(60) on the overlay layer
    //   persist.gammaos.nano.perf.adpf        - ADPF render-thread hint session
    //   persist.gammaos.nano.perf.fixedperf   - IPower FIXED_PERFORMANCE while shown
    void*  mHintSession = nullptr;
    bool   mHintTried = false;
    void   perfHintInit(int tid);     // create the ADPF session for the render thread
    void   perfHintReport();          // report this frame's work duration to ADPF
    bool mOverlayWallpaper = false;   // overlay is showing the FULL PS3 wallpaper (no app behind,
                                      // or a submenu is open) vs the scrim-over-live-app top level
    bool mOverlayPendingShow = false; // defer the SF t.show() to after the first faded-out frame is
                                      // composited, so the entrance animates in (no stale-buffer flash)
    // Enable blend for UI chrome. In overlay (translucent SF) mode use a SEPARATE
    // alpha term (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) so the framebuffer alpha
    // accumulates toward 1 for opaque chrome - otherwise GL_SRC_ALPHA
    // UNDER-accumulates alpha and SurfaceFlinger bleeds the live app through
    // "white" text/icons. The home DRM path keeps the plain straight blend.
    void panelTextBlend();
    void setUiBlend();
    void overlayInitLayer();          // create the translucent layer + initial hide (once)
    void overlayPoll();               // watch show_overlay; drive show/hide each frame
    void overlayOskPoll();
    // Nano's keyboard (ShaderControl's NanoImeService, a client of overlayOskPoll's bridge) as the
    // system keyboard, unless persist.gammaos.nano.ime is 0.
    static void selectNanoIme();            // watch osk_req; host an app's OSK over the live app
    static void selectDefaultIme();         // the keyboard a finished setup leaves selected
    void overlayShow();               // raise layer + drop_input=1 + reset to XMB top
    void overlayHide();               // hide layer + drop_input=0
    bool mupenDirectIntent(const std::string& pkg, const std::string& romPath,
                           const std::string& contentUri, bool tabbed, std::string& out);
    void overlayGpuPark();            // drop the XMB GPU working set while an app is in front
    void overlayGpuUnpark();          // rebuild it (lazily where possible) before the overlay draws
    // A raise for the button map alone (qs_summon=3) leaves the XMB unread and unbuilt: on the T820
    // that work took most of a second before the map's first frame. overlayPrepareXmb catches up
    // the moment anything but the map is to be drawn.
    bool mOverlayXmbDeferred = false;
    void overlayPrepareXmb(int summon);
    void overlayPageOutSelf();        // push this process's anonymous pages to zram (parked overlay)
    static void nanoDropReclaimableCaches();   // free the kernel dentry/inode caches (app launch hand-off)
    // Apply the mode-dependent presentation state (layer opaque flag + EGL swap
    // interval) for the CURRENT mOverlayWallpaper. Must run on the render thread
    // (eglSwapInterval needs the context current). Called from overlayShow and
    // from every path that flips wallpaper/scrim without a hide+show cycle
    // (overlayQuitToHome's quit-to-launcher switch).
    void overlayApplyPresentMode();
    // Resolve the current foreground (resumed) package via ActivityManager, with
    // the same "real 3rd-party app" validation overlayPauseApp applies. Empty if
    // none. Populated on every overlayShow() so quit/launch always have a target
    // regardless of whether the pause feature is enabled.
    std::string overlayResolveForegroundPkg();
    // GammaOS Nano orientation control. orientationTick() runs on the ~0.5s
    // cadence in both the home and overlay processes; it computes a single
    // foreground-aware orientation token (nano's own setting when the menu or
    // overlay is foreground, a per-app override or "none" when an app is) and
    // publishes it to sys.gammaos.nano.force_orientation, which WindowManager
    // reads in mapOrientationRequest. appOrientGet returns the stored override
    // for a package (empty = none). See NanoMenuOrientation.cpp.
    void orientationTick();
    // SF overlay only: keep the overlay's render surface sized to the display's
    // current logical size, so when nano forces a portrait orientation the XMB
    // reflows to a real portrait layout instead of a rotated/truncated landscape
    // one. Queries the display's layerStackSpaceRect each shown frame and resizes
    // the SurfaceControl buffers + mWidth/mHeight when it changes.
    void overlayUpdateSurfaceSize();
    std::string appOrientGet(const std::string& pkg);
    void appOrientSet(const std::string& pkg, const std::string& value);
    void appOrientLoad();
    void appOrientSave();

    // Dual-Stack per-app allowlist (persist.gammaos.dualstack.pkgs [+ .pkgs_1, .pkgs_2 ...],
    // comma-separated, split across props to beat the ~91-char sysprop value cap; the framework
    // DualStackController reads them via DualStackPropertyUtils). Offered as a per-app context-menu
    // toggle on dual-screen devices (see openXmbOpt), mirroring the per-app Screen Orientation.
    bool dualstackHas(const std::string& pkg);
    void dualstackSet(const std::string& pkg, bool enable);
    bool primaryScreenHas(const std::string& pkg);   // per-app "Run on primary screen" allowlist
    void primaryScreenSet(const std::string& pkg, bool enable);
    // Per-app "keep running in the background" allowlist. When a package is on this list nano does NOT
    // force-stop it when the user exits it back to the nano menu (back-hold or quick-menu Close App);
    // the app stays alive so re-launching it resumes warm. Stored in the same ~91-char-safe indexed
    // sysprop list (persist.gammaos.nano.background_pkgs) and read by the framework exit path
    // (PhoneWindowManager) via DualStackPropertyUtils.isPackageInList. Explicit "Kill All / Kill
    // Background" actions still stop it.
    bool backgroundHas(const std::string& pkg);
    void backgroundSet(const std::string& pkg, bool enable);
    // Pinned apps: a Game-home shortcut list of user-chosen apps. Package-keyed, stored in the
    // same ~91-char-safe indexed sysprop list infra as primary_pkgs (persist.gammaos.nano.pinned_pkgs).
    bool isAppPinned(const std::string& pkg);         // package is in persist.gammaos.nano.pinned_pkgs
    void toggleAppPin(const std::string& pkg);        // add/remove the package, then rebuild the home cats
    void toggleAppPinFocused();                       // Y shortcut: pin/unpin the focused PS3_APP row (all themes)
    // Dual-SCREEN detect prompt: system_server publishes sys.gammaos.nano.dualscreen_detected when
    // an app is seen spanning both physical panels. On the home we offer to enable "Run on primary
    // screen" for it; "Don't ask again" adds it to persist.gammaos.nano.dualscreen_dismissed.
    void openDualScreenPrompt(const std::string& pkg);
    void pollDualScreenDetect();
    // NanoDisplayTarget.h: Nano draws on the display showing layer stack 0, a monitor while one is
    // plugged in. followDisplay0 moves the layer and resizes the surface when that display changes;
    // mTargetMonitor is true while it is a monitor (the touchscreen is off unless the UI is on the
    // panel beside it, below).
    void followDisplay0();
    bool resizeFlingerSurface(int w, int h);
    int64_t mDisplayFollowMs = 0;
    bool mTargetMonitor = false;
    bool mPanelRaisePending = false;   // dual screen: the Quick Menu comes up on the panel (parked loop)
    bool mDisplayShown = true;         // followDisplay0's last look found the screens on
    // The parked loop's last look: SurfaceFlinger's displays and the display mode (empty: none yet
    // since the overlay was last shown); the displays before the screens last went off, and since
    // when they are on again.
    std::vector<uint64_t> mParkIds;
    std::string mParkMode;
    std::vector<uint64_t> mParkSleepIds;
    int64_t mParkScreensOnMs = 0;
    int64_t mPanelRaiseArmMs = 0;
    // Dual screen (persist.gammaos.display.mode=dual, NanoDisplayTarget.h): the panel stays on beside
    // the monitor target as Nano's second screen. Nano's UI moves onto it while a menu is open at the
    // home, and whenever the overlay is up over a game (uiToPanel swaps mSurface and its size with the
    // panel's); otherwise the panel shows the theme's second screen.
    bool mDualPanel = false;
    uint64_t mDualPanelId = 0;     // its physical display id
    bool mUiOnPanel = false;
    bool panelsOff() const { return mTargetMonitor && !mDualPanel; }   // the panels are off for a monitor
    // The overlay is drawn over a running app, which shows through (not on the panel beside it).
    bool overAppScrim() const { return mOverlayMode && !mOverlayWallpaper && !mUiOnPanel; }
    void setDualPanel(bool on, uint64_t id);
    bool panelUiWanted() const;
    bool panelUiRaw(bool* overGame) const;
    void uiToPanel(bool on);
    void dualPanelTick();
    void releaseSecondarySurfaces();
    void resizeMainSurface(int w, int h);
    void swapUiSurface(bool on);
    int uiSwapInterval() const;
    void dualPanelReveal();
    std::pair<int, int> mMonitorUiSize{0, 0};   // the monitor surface's size while the UI is on the panel
    int mUiInterval = -1;                       // swap interval set on the UI's surface
    bool mMonitorFresh = true;                  // the monitor's surface holds a frame of the home
    bool mMonitorLayerShown = false;            // the overlay's monitor layer is shown
    bool mPanelLayerHidden = false;             // the home's panel layer is hidden for the panel's app
    bool mPanelRevealPending = false;           // show it after this frame's draws
    int64_t mPanelStackCheckMs = 0;
    void renderPanelBackdrop();
    // An app on the panel in dual screen mode (Applications, Android Settings), beside the home or a
    // game on the monitor. While it has the controller (focused) Nano reads only the Anbernic button,
    // which hands the controller back and opens the Quick Menu over it.
    std::string mPanelApp;
    bool mPanelAppFocused = false;
    bool mPanelFnDown = false;
    int64_t mPanelAppWatchMs = 0;
    std::atomic<uint32_t> mPanelAppGoneGen{~0u};   // the launch (mPanelAppGen) whose app has gone
    std::atomic<int> mPanelAppGoneStreak{0};
    std::atomic<uint32_t> mPanelAppGen{0};
    std::atomic<bool> mPanelPollBusy{false};
    // always: on the built-in screen whatever the app's screen and the defaults say.
    bool panelLaunch(const std::string& pkg, bool settings = false, bool always = false);
    // notice: a glow on the screen that takes the controller (sys.gammaos.nano.focus.notice).
    void panelAppFocus(bool focused, bool notice = false);
    void panelAppClose();
    void panelAppTick();
    bool panelAppInputGate();
    // Controller Screen, the screen chosen for each app and moving apps between the screens.
    bool panelOpenWanted(const std::string& pkg);
    bool gameOnMonitor() const;
    void panelAppToMonitor();
    bool monitorAppToPanel();
    void openAppOnScreen(const std::string& pkg, bool panel);
    void screenSwap();                   // the Switch Screen hotkey
    bool panelPinAllowed() const;
    void panelAppForget();
    void screenSwapPoll();               // ... sent by PhoneWindowManager; a touch on the panel's app
    void panelTouchSync();               // sys.gammaos.nano.touch_display
    // A property another process sets each time something happens (a new value each time).
    struct PropEdge { const char* name; const prop_info* pi = nullptr; uint32_t serial = 0; bool inited = false; };
    static bool propEdge(PropEdge& e);   // true once per change after the first read
    PropEdge mScreenSwapEdge{"sys.gammaos.nano.screen_swap"};
    PropEdge mPanelTouchEdge{"sys.gammaos.nano.panel_touch"};
    bool screenPanelHas(const std::string& pkg);
    bool screenMonitorHas(const std::string& pkg);
    void screenChoiceSet(const std::string& pkg, int which);   // 0 default, 1 built-in screen, 2 monitor
    int mPanelTouchDisp = -2;            // last value written to sys.gammaos.nano.touch_display
    uint32_t mFocusNoticeCount = 0;
    // True on a physical dual-screen device (e.g. RG DS): physical display count > 1, cached.
    bool hasSecondaryDisplay();
    std::string mLastOrientToken;                 // last force_orientation we wrote
    std::map<std::string,std::string> mAppOrient; // package -> orientation override
    bool mAppOrientLoaded = false;
    int64_t mAppOrientStamp = -1;                 // nano_app_orient.json mtime (cross-process reload)
    // Freeze/thaw the foreground app while the overlay is up (SIGSTOP/SIGCONT of
    // its process group), so gameplay + audio pause and its GPU/CPU is freed -
    // the real PS3 in-game XMB pauses the title. Gated by
    // persist.gammaos.nano.overlay.pause (default on); the frozen window keeps
    // its last frame for SurfaceFlinger to blur.
    void overlayPauseApp(bool pause);
    // Freeze App Under Clock: SIGSTOP (freeze=true) / SIGCONT (freeze=false) the over-app game in
    // place while the slide clock is open, via the crash-safe overlaySignalPackage. Async.
    void pspClockFreezeApp(bool freeze);
    std::string mOverlayPausedPkg;    // package frozen on show, thawed on hide
    // Overlay XMB actions (NanoMenuOverlay.cpp), invoked from the PS3 input
    // handlers when mOverlayMode. Resume = dismiss + thaw the running app; quit =
    // force-stop it and fall back to the home XMB; launch = force-stop the
    // current app and start a new package (the running one is replaced).
    void overlayResume();
    void overlayQuitToHome(bool keepApp = false);   // keepApp: it moved to the panel, still running
    bool overlayLaunchPackage(const std::string& pkg);
    // In-game URL launch: start the chosen browser on the URL via ACTION_VIEW.
    bool overlayLaunchUrl(const std::string& pkg, const std::string& comp,
                          const std::string& url);
    // Shared overlay launch primitive: cleanly exit whatever is running (ESC +
    // save-state wait for RetroArch/DraStic, force-stop for other apps), then run
    // the prebuilt `am start ...` command for the new target. Tracks launch_app +
    // app_launched=1 so the framework (RootWindowContainer / PhoneWindowManager)
    // detects the new app's exit and raises the overlay launcher again. Runs the
    // exit+launch on a detached thread so the overlay keeps animating; overlayPoll
    // dismisses onto the new app once it resumes.
    void overlayLaunchCommand(const std::string& pkg, const std::string& amCmd);
    void overlayCloseRunningApp(const std::string& old, const std::string& next = "");   // ESC + wait / force-stop the app under the overlay
    // Launch the currently selected XMB game/ROM from the overlay (resolves the
    // emulator package for the selected system/recent entry, then reuses
    // overlayLaunchPackage). Routed from ps3XmbSelect's PS3_ROM/PS3_RECENT cases
    // when mOverlayMode (the home-mode launchXmbGame() must not run in the overlay).
    void overlayLaunchGame();
    // True while mOverlayMode and at the XMB top level with no dialog/submenu, so
    // Back resumes the game rather than doing nothing.
    bool overlayAtTopLevel() const;
    // Capture the current screen (the just-frozen app) with screencap and load
    // it into mOverlayBgTex, used as the static, blurred+tinted opaque overlay
    // background (the task-switcher snapshot model). Implemented in
    // NanoMenuRender.cpp (next to the PNG loader).
    void overlayCaptureBackground();
    // Fast in-process display capture via SurfaceComposerClient (no screencap
    // spawn). Returns a new GL texture of the current screen (or 0 on failure,
    // caller falls back to the screencap binary). Outputs the captured size.
    GLuint overlayCaptureInProcess(int* outW, int* outH);
    // Resolve the "-d <physId> " argument for `screencap` so a Control Center screenshot targets the
    // CONTENT panel (where the launched app plays), not the CC's own panel. Empty string = let
    // screencap use its default display (single-screen device, or resolution failed).
    std::string ccContentScreencapArg();
    GLuint mOverlayBgTex = 0;         // captured app snapshot (colour), 0 = none
    int64_t mOverlayShowMs = 0;       // uptimeMillis() when the overlay was raised;
                                      // power events within a grace window after
                                      // this are ignored so the summoning hold's
                                      // own release does not instantly dismiss it
    // Deferred dismiss after launching another app from the overlay: keep the
    // overlay layer up (occluding the dying old app / blank) until the new app is
    // resumed, so the user never sees the old frame or black between the two.
    bool mOverlayLaunchPending = false;
    std::string mOverlayLaunchTarget;
    int64_t mOverlayLaunchStartMs = 0;
    int64_t mOverlayLaunchLastCheckMs = 0;
    float mOverlayEnterStart = -1.0f; // entrance state: -2 = pending (raised, awaiting
                                      // first render), >=0 = actively animating,
                                      // -1 = done/settled. Drives the cold-boot-style
                                      // fade/float-in of the XMB chrome (<0 = settled)
    float mOverlayEnterElapsed = 0.0f;// accumulated CLAMPED seconds into the entrance,
                                      // so a slow first frame (lazy load) cannot skip
                                      // the animation (it advances per rendered frame)

    // Menu
    void buildMenu();
    void rebuildDisplayItems();

    // Effects
    void initEffects();
    void resetParticle(int i);
    void updateEffect();
    void renderEffect();

    sp<SurfaceComposerClient> mSession;
    int         mWidth;
    int         mHeight;
    EGLDisplay  mDisplay;
    EGLContext  mContext;
    EGLSurface  mSurface;
    sp<IBinder> mDisplayToken;
    // Live SF display rotation as ui::Rotation cast to int (0/1/2/3 = ROTATION_0/
    // 90/180/270). Cached by overlayUpdateSurfaceSize; read by touchMapRaw so the
    // touch axis follows a forced-portrait rotation over a landscape-native panel.
    int mOverlayRotation = 0;
    uint32_t mAppliedLayerStack; // GammaOS: last layer stack applied to the nano surface
    std::vector<sp<IBinder>> mSecondaryDisplayTokens; // GammaOS: secondary display tokens
    int mDualScreenCache = -1;                         // -1 unknown, 0 single, 1 dual (physical display count > 1)
    std::vector<sp<SurfaceControl>> mSecondaryWallpaperControls; // GammaOS: wallpaper on secondaries
    std::vector<EGLSurface> mSecondaryEglSurfaces; // GammaOS: EGL surfaces for secondary wallpaper
    std::vector<sp<Surface>> mSecondarySurfaces; // GammaOS: keep refs alive
    std::vector<uint32_t> mSecondaryAppliedLayerStacks; // GammaOS: last layer stack applied to each secondary wallpaper SC
    std::vector<std::pair<int,int>> mSecondaryCreatedSize; // GammaOS: each secondary wallpaper SC's buffer size (physical mode res)
    std::vector<int> mSecondaryAppliedLssH;  // GammaOS: last logical-canvas height each secondary was stretched to (-1 = unset); see the Dual-Stack coverage stretch
    bool mNdsSecondaryShown = true; // GammaOS DSi overlay: is the secondary panel SC currently shown (hidden during a translucent in-game overlay so the app's bottom screen is not covered)
    sp<SurfaceControl> mFlingerSurfaceControl;
    sp<Surface> mFlingerSurface;

    // GL shader program
    GLuint mShaderProgram;
    GLint  mLocPosition;
    GLint  mLocColor;
    GLint  mLocRotation; // DRM orientation rotation

    // Batched particle shader (per-vertex color)
    GLuint mParticleProgram;
    GLint  mParticleLocPosition;
    GLint  mParticleLocColor;
    GLint  mParticleLocRotation;

    // Fullscreen effect shader
    GLuint mFxProgram;
    GLint  mFxLocPosition;
    GLint  mFxLocTime;
    GLint  mFxLocResolution;
    GLint  mFxLocEffect;
    GLint  mFxLocRotation;
    GLint  mFxLocCoordSwap;
    GLint  mFxLocYFlip;
    GLint  mFxLocXFlip;

    // XMB background shader (PS3-style volumetric ribbons, 60fps)
    GLuint mXmbProgram;
    GLint  mXmbLocPosition;
    GLint  mXmbLocTime;
    GLint  mXmbLocResolution;
    GLint  mXmbLocRotation;
    GLint  mXmbLocCoordSwap;
    GLint  mXmbLocYFlip;
    GLint  mXmbLocXFlip;

    // Menu state
    std::vector<MenuItem> mMenuItems;
    int mSelectedIndex;

    // Pre-computed display strings (rebuilt on state change, not every frame)
    std::vector<std::string> mDisplayItems;
    std::string mTitle;
    std::string mSubtitle;
    std::string mFooter;
    FrameFlag mDisplayDirty;   // true when display items need rebuild; setting it asks for a frame
    void requestFrame() { FrameFlag::wake(); }   // any thread: a result to show (wakes the idle wait)
    bool mClockSecondsShown = false;   // this frame drew a clock that changes each second (render sets it)
    // On-demand drawing (the run loop): what the last frame showed, the wait between frames, and the
    // frame-rate log (persist.thimsy.nano.fpslog).
    uint32_t mDrawnSeq = 0;
    int64_t mDrawnClockKey = -1, mDrawnStatusSig = -1, mDrawnGeomSig = -1;
    int64_t mLoopIter = 0, mLastDecisionIter = -2;
    int64_t mSettledMs = 0;                 // when the screen last settled (the 10 s redraw window)
    std::map<int, bool> mPollSkipFd;        // input fd -> left out of idleWait (error, hang-up, motion sensor)
    int   mIdleWaitReason = 0;              // 1 input, 2 requestFrame, 3 timeout; 4 a change, 5 motion; 6 nudge
    int   idleWait(int ms, uint32_t seenSeq);   // waits on the input fds, inotify and the frame eventfd
    void  paceWait(int ms);                 // the rest of a slowed frame: ends early only for input or a request
    void  fpsLogTick(bool drew);
    bool  frameTicksPending();

    // Input device fds
    std::vector<int> mInputFds;
    std::set<std::string> mOpenedDevices;
    std::map<int, std::string> mInputFdNames;  // fd -> EVIOCGNAME (Control Center bottom-touch routing)
    int mCurrentInputFd = -1;                  // fd of the event currently being dispatched
    // fds in mInputFds that are the POWER key node (axp2202-pek / KEY_POWER). The
    // overlay must NEVER EVIOCGRAB these: grabbing the power node blocks Android
    // EventHub from seeing the summon's power UP, which strands PhoneWindowManager's
    // single-key power gesture (mDownKeyCode stuck) so power only toggles every
    // OTHER press. Power is left ungrabbed so PWM owns the show/hide toggle.
    std::set<int> mPowerFds;
    int mInotifyFd;

    // Exit flag
    bool mExitRequested;
    bool mWaitForRelease; // wait for select key release before exiting
    // Home (non-overlay) launch transition: stamped (uptimeMillis) when the select
    // that triggered a launch is RELEASED; render() fades the XMB to black over
    // kLaunchFadeMs and the exit (hand-off to the app) is held until the fade
    // completes, so launching a game/app fades out instead of hard-cutting. 0 = idle.
    int64_t mLaunchFadeStart = 0;
    bool mDrasticNanoPending; // drastic nano: waiting for cache, then restart
    bool mDrmBootPath; // headless EGL + DRM direct (SF deferred until app launch)

    // Submenu state
    MenuState mMenuState;
    std::vector<RecentEntry> mRecentEntries;
    int mRecentSelectedIndex;
    bool mRecentLoaded;  // true if playlist file was readable
    bool mStorageReady;  // true once /data/media/0 is accessible (CE unlocked)

    // Applications submenu
    std::vector<AppEntry> mAppEntries;
    int mAppSelectedIndex;
    bool mAppsLoaded;

    // Scrolling text state for long game names in Recently Played / Applications
    float mScrollOffset;
    int   mScrollDir;       // 1 = scrolling left, -1 = scrolling right
    int   mScrollPause;     // frames to pause at each end before reversing
    int   mLastScrolledIdx; // which item index was scrolling (reset on change)

    // Vertical menu scroll for submenus with more items than fit on screen
    int mMenuScrollTop;     // first visible item index

    // Analog stick state
    bool mStickYTriggered; // prevents repeat until stick returns to center
    bool mStickXTriggered; // prevents repeat for horizontal axis

    // Gamepad Test / Calibration screens (Settings > Gamepad > Calibrate & Test).
    // While active, pollInput mirrors every raw button/axis into these maps so the
    // screens can visualise the live controller state.
    bool mGpTestActive = false;    // live Test Controller screen up
    bool mGpCalibActive = false;   // analog calibration wizard up
    long mGpScreenOpenMs = 0;      // when the screen opened (ignore the opening press)
    long mGpSelectDownMs = 0;      // Select held-since, for hold-to-exit
    std::map<int,int> mGpBtn;                       // evdev button code -> value (0/1)
    std::map<int,int> mGpAxis;                      // ABS code -> latest raw value
    std::map<int,std::pair<int,int>> mGpAxisRange;  // ABS code -> (min,max) from EVIOCGABS

    // ---- Full-screen d-pad HSV colour picker (LED / Left / Right Colour, all themes) ----
    // A theme-independent modal (shares the gamepad-test dialog chrome) opened from the
    // "Custom..." row of the @rgbcolor swatch chooser. D-pad drives Hue (X) + Brightness (Y),
    // L1/R1 adjust Saturation, A confirms, B cancels. Reuses mGpBtn/mGpAxis (via gpCaptureEvent)
    // for held-direction reads and mGpSelectDownMs for the hold-SELECT exit.
    bool  mCpActive = false;       // colour picker modal up
    long  mCpOpenMs = 0;           // open time (debounce the opening confirm press)
    float mCpHue = 210.0f;         // 0..360
    float mCpSat = 1.0f;           // 0..1
    float mCpVal = 1.0f;           // 0..1
    const Ps3SettingBinding* mCpBinding = nullptr;  // colour prop to write on confirm (mode 0)
    std::string mCpTitle;          // header label (the colour row name)
    int   mCpApplyMode = 0;        // 0 = write an RGB LED colour (applyRgbSolidColor), 1 = a theme accent (applyThemeAccentColor), 2 = the Background Colour
    // Calibration wizard (port of LineageParts GamepadCalibrationDialogFragment).
    // Steps: 0 centre, 1 left-stick range, 2 right-stick range, 3 triggers,
    // 4 deadzone, 5 sensitivity, 6 done.
    int mGpCalStep = 0;
    int mGpCalCen[4] = {0,0,0,0};                   // centre of X,Y,RX,RY
    int mGpCalMin[4], mGpCalMax[4];                 // captured min/max of X,Y,RX,RY
    int mGpCalTrigMax[2] = {0,0};                   // captured max of L2,R2
    int mGpCalDead = 8;                             // deadzone percent 0..40
    int mGpCalSens = 100;                           // sensitivity percent 50..200
    int mGpCalRsx = 3, mGpCalRsy = 4;               // right-stick axis codes (ABS_RX/RY default)
    long mGpCalStepMs = 0;                          // when the current step began (A debounce)
    int mGpCalNavLatch = 0;                         // edge latch for HAT/stick slider adjust

    // Hold-to-repeat navigation. Set when a dpad key / HAT axis / stick
    // axis enters its held state; cleared on release. pollInput() ticks
    // this each frame and calls handleUp/Down/Left/Right at an
    // accelerating cadence while a direction is held, so users don't
    // have to tap repeatedly to scroll long lists.
    NavDir  mNavHeldDir      = NavDir::None;
    int     mNavFd           = -1;   // the input device the held direction came from
    int64_t mNavHeldStartMs  = 0; // when the current direction was pressed
    int64_t mNavLastRepeatMs = 0; // when we last fired a repeat (or initial fire)
    int     mNavRepeatCount  = 0; // repeat fires so far, used for acceleration

    // uptimeMillis() of the last user input (any button event, a dpad/stick
    // direction press, or a scripted nav injection). The PS3 XMB idle frame-
    // rate drop in threadLoop only engages a full minute after this stamp.
    int64_t mLastInputMs = 0;
    int64_t mLastPressMs = 0;     // a button, D-pad or touch press (not a release or focus change)

    // Brightness / power
    bool mSelectSearchArmed = false;  // XMB SELECT tap pending: open the search OSK on release
    bool mStartHeld = false;   // for the START+SELECT PS3 boot-intro re-trigger
    int64_t mPowerPressTime;
    int64_t mPowerHoldMs = 0;   // a power hold that opened the Power menu (grabbing home)
    void powerHoldTick();
    bool anbernicPhysicallyDown(int* fdOut = nullptr);
    int mBrightness;
    int mMaxBrightness;
    bool mShowBrightnessBar;
    int mBrightnessBarTimer;
    bool mPs3BrightSlider = false;   // Quick Menu brightness slider modal: Left/Right adjust, any other key exits
    int mVolume;
    int mMaxVolume;
    bool mShowVolumeBar;
    int mVolumeBarTimer;

    // GammaOS Nano: launch-busy state. mShowLaunchBusy drives the
    // centred toast for a few seconds after a too-early press, while
    // mLaunchPending persists until either isLaunchReady() flips true
    // (auto re-fire of handleSelect) or the user navigates / backs out.
    bool mShowLaunchBusy;
    int mLaunchBusyTimer;
    bool mLaunchPending;

    // Battery state (cached, refreshed ~1/s from pollBattery())
    int mBatteryPercent;      // -1 if unknown / no battery
    bool mBatteryCharging;    // true when charging or full
    int mBatteryPollTicks;    // frames until next sysfs read
    int mVolumePollTicks = 0;  // frames until next volume-prop resync (DSi status-bar icon)

    // Network state (cached, refreshed from netPollThreadFunc at ~0.5 Hz).
    // All fields are guarded by mNetStateMutex; copy into locals before use.
    std::mutex mNetStateMutex;
    WifiLevel mWifiLevel;
    bool mWifiRadioOn = true;      // cached radio on/off for the Internet Connection row (no per-frame query)
    int mWifiBars;                 // 0..4 signal strength, meaningful only if Connected
    std::string mWifiSsid;         // connected SSID, empty otherwise
    BtLevel mBtLevel;
    int mBtConnectedCount;         // bonded AND currently connected devices
    // Bluetooth just turned on (1) or off (0) from the chooser: shown at once, and kept until the
    // radio reports it or mBtWantUntilMs (uptime ms) passes.
    std::atomic<int> mBtWant{-1};
    std::atomic<int64_t> mBtWantUntilMs{0};
    bool mNetPollInitialised;      // first poll landed, safe to draw
    // Worker thread handles for the net poller. Started from the ctor
    // after mSession is wired up; joined from the dtor.
    std::thread mNetPollThread;
    bool mNetPollThreadRunning;
    bool mNetPollExitRequested;

    // Frame timing
    int64_t mLastFrameNs;  // monotonic clock from previous frame
    float mFrameDt;        // seconds elapsed since last frame (clamped)

    // Effects
    int mCurrentEffect; // 0 = none, 1..21 = effect
    float mEffectTime;
    Particle mParticles[MAX_PARTICLES];

    // Quick Resume
    bool mQuickResumeEnabled;

    // XMB mode
    bool mXmbMode;
    // drawText() outline/shadow control:
    //   0 = default (single drop shadow in XMB mode, else 4-offset outline) at 0.8a
    //   1 = even 4-offset outline at mTextOutlineRatio*a (PS3 XMB: symmetric, subtle,
    //       brightness-scaled, and ONE draw call so it stays cheap on every label)
    //   2 = none (used around glow/halo white copies so they get no dark outline)
    int   mTextOutlineMode = 0;
    float mTextOutlineRatio = 0.8f;
    // Multiplies drawText's outline offset (stroke thickness). 1.0 normally;
    // raised briefly around the item subtitle so its dark stroke is heavier and
    // the small grey description reads clearly on small panels.
    float mTextOutlineWidthMul = 1.0f;
    // Small-panel readability boost for the fullscreen dialog/wizard body text
    // (System Update, Network Connection Settings). 1.0 on >=720p panels,
    // ramping up on small handheld screens where the 1:1 web sizes are too small.
    float ps3DlgFontBoost() const;
    std::vector<XmbRecentEntry> mXmbRecent; // Recently played from XMB
    int mXmbRecentMax;                       // Max entries to keep
    // User Collections (cross-system game groups). Persisted line-delimited to
    // /data/system/nano_collections.txt. A collection's game-list level carries its index in
    // Ps3Level.collectionIdx so a game's option menu there can offer Remove from Collection.
    std::vector<XmbCollection> mXmbCollections;
    void loadCollections();
    void saveCollections();
    // buildCollectionsSubmenu / buildCollectionSubmenu take Ps3Level& and MUST be declared after the
    // Ps3Level struct (see the note by buildRecentSubmenu) - they are, below.
    bool collectionResolveRom(const std::string& romPath, int* sysIdx, int* romIdx);
    int  collectionCreate(const std::string& name);     // create empty, returns index (or existing match)
    void collectionAddRom(int colIdx, const std::string& romPath);
    void collectionRemoveRom(int colIdx, const std::string& romPath);
    void openAddToCollectionDialog(const std::string& romPath);   // chooser: New + each collection
    std::string mPendingCollectionRom;   // rom awaiting an Add-to-Collection choice / new-collection name
    // Favourites: a single global, cross-system "starred games" list with a one-button toggle on any
    // game (NextUI-style). A flat list of full ROM paths, persisted one-per-line to
    // /data/system/nano_favorites.txt. Shown as its own "Favorites" entry under Game when non-empty.
    std::vector<std::string> mXmbFavorites;
    std::unordered_set<std::string> mXmbFavoriteKeys;   // hiddenKey of each, for isFavorite
    void loadFavorites();
    void saveFavorites();
    bool isFavorite(const std::string& romPath) const;
    void toggleFavorite(const std::string& romPath);   // add if absent, remove if present; persists
    // Games whose imported ES-DE <favorite> the user has switched off in the ES-DE theme.
    std::vector<std::string> mEsdeUnfavorites;
    // buildFavoritesSubmenu takes Ps3Level& so it is declared with the other builders below.
    // Hidden games: games taken out of every game list without deleting the file. Keyed by
    // hiddenKey (the same game reached through /data/media/0, /storage/emulated/0 or /sdcard is
    // one entry) and mapped to the path it was hidden by, persisted one per line to
    // /data/system/nano_hidden_games.txt. applyRomNameOverrides drops them wherever a system's
    // list is (re)built, so every theme, count, search, favourite and collection follows; they
    // are dropped from Recently Played too. Settings > Game Settings > Hidden Games unhides.
    std::map<std::string, std::string> mHiddenGames;
    bool loadHiddenGames();                            // true when the set changed
    void saveHiddenGames();
    static std::string hiddenKey(const std::string& romPath);
    static bool sameFile(const std::string& a, const std::string& b);
    bool isHiddenGame(const std::string& romPath) const;
    void hideGame(const std::string& romPath);         // persists, drops it from every open list
    // Delete Game: the confirmation (theme key 66: Cancel / Delete Game / Delete Game and Media),
    // then the game's files, its records everywhere and, when asked, its media.
    void gameDeleteOffer(int sysIdx, const std::string& romPath, const std::string& name);
    void gameDelete(bool withMedia);
    std::string mGameDeletePath;
    int mGameDeleteSys = -1;
    void unhideGame(const std::string& key);           // persists, rescans so it returns
    bool dropHiddenRecents();                          // true when Recently Played lost an entry
    void refreshListsAfterHide();                      // re-filter systems + rebuild open levels
    std::vector<XmbSystem> mXmbSystems;
    int mXmbSystemIndex;       // Currently selected system
    int mXmbGameIndex;         // Currently selected game in current system
    float mXmbAnimX;           // Animated horizontal position (lerps to mXmbSystemIndex)
    float mXmbAnimY;           // Animated vertical position (lerps to mXmbGameIndex)
    int mXmbGameScrollTop;     // First visible game in list
    bool mXmbRomScanDone;      // ROM paths have been scanned
    bool mXmbBootCompleted;    // true after sys.boot_completed=1

    // ===================================================================
    // PS3 XMB layout (NanoMenuPS3Menu.cpp). A faithful PS3 XrossMediaBar:
    // a horizontal category bar (Settings/Game/...) with a vertical item
    // list dropping out of the active category, submenu push/pop, the
    // authentic firmware geometry and the captured wave behind it. Gated
    // by persist.gammaos.nano.ps3xmb during build-up; the categories are
    // populated from the SAME nano content the carousel uses (emulator
    // systems + ROMs, recently played, applications, settings).
    // ===================================================================
    enum Ps3ItemKind {
        PS3_SYSTEM = 0,   // an emulator system -> ROM submenu (a = sysIdx)
        PS3_ROM,          // a ROM -> launch (a = sysIdx, b = romIdx)
        PS3_RECENT_LIST,  // "Recently Played" -> recent submenu
        PS3_RECENT,       // a recent entry -> launch (a = recent idx)
        PS3_APP_LIST,     // "Applications" -> app submenu
        PS3_APP,          // an installed app -> launch (a = app idx)
        PS3_COLLECTIONS_LIST, // "Collections" entry under Game -> the collections list
        PS3_COLLECTION,   // a user collection -> its games submenu (a = collection idx)
        PS3_COLLECTION_NEW,   // "New Collection..." row -> OSK name -> create an empty collection
        PS3_FAVORITES_LIST,   // "Favorites" entry under Game -> the global favourites list
        PS3_SETTING,      // a settings entry (a = action: 0 Wi-Fi, 1 Bluetooth, 2 settings tree)
        PS3_LAUNCH_PKG,   // launch a package (payloadStr = package name)
        PS3_DATA_SUBMENU, // a static DATA item with children -> submenu (data*)
        PS3_DATA_LEAF,    // a static DATA leaf (dialog / value / info, no action)
        PS3_QUICK,        // Quick Menu action; a = action code (QA_* in NanoMenuPS3Menu.cpp)
        PS3_CAT_SUBMENU,  // open a home category's item list as a pushed submenu (a = mPs3Cats index).
                          // Used by the ES-DE home's Nano Settings chooser to reach the Quick Menu and
                          // the Settings category the XMB/DSi/Minima carousels expose directly.
        PS3_GS_ROOT,      // "Game Systems" entry -> open the systems-list editor screen
        PS3_CATORDER_ROOT,// "Home Categories" entry -> open the category order/visibility editor
        PS3_CATORDER_ROW, // a category row in the Home Categories editor (a = mCatOrder index)
        PS3_GS_SYSTEM_ROW,// a system row in the Game Systems list (a = mXmbSystems index)
        PS3_GS_FIELD,     // a field row in the per-system editor (a = field id)
        PS3_GS_ADD,       // "Add New System" row in the Game Systems list
        PS3_GS_AUTOADD,   // "Auto-add Systems from Folder..." row (ES-DE style bulk import)
        PS3_GS_EMUROW,    // an emulator/core row in the emulator picker (a = catalog index)
        PS3_GS_EMU_CUSTOM,// "Custom..." row in the emulator picker (type a core/package by hand)
        PS3_GS_EMU_PLATFORM, // a platform row in the add-system emulator picker (payloadStr = uniqueId)
        PS3_GS_EMU_SEARCH,   // "Search All Emulators..." row in the emulator picker
        PS3_GS_SCANSRC,   // a configured scan-folder row (a = scanSources index; Y removes it)
        PS3_GS_DEFFOLDER,     // an active default scan folder (Y removes it; payloadStr = alias)
        PS3_GS_DEFFOLDER_OFF, // a removed default scan folder (Y restores it; payloadStr = alias)
        PS3_GS_ADDFOLDER, // "Add Folder..." row in the scan-folders screen
        PS3_GS_DIR,       // a directory row in the folder browser (payloadStr = path)
        PS3_GS_SELFOLDER, // "Select This Folder" row in the folder browser (payloadStr = path)
        PS3_GS_PICKFILE,  // a selectable image file row in the icon-import file browser (payloadStr = full path)
        // ---- Music player (PS3 XMB music port) ----
        PS3_MUSIC_ALBUM,    // an album folder in the Music column -> track submenu (a = album idx)
        PS3_MUSIC_TRACK,    // a track -> open the Now-Playing player (a = track idx in the view list)
        PS3_MUSIC_PLAYLIST, // a playlist -> its track submenu (a = playlist idx)
        PS3_MUSIC_PL_NEW,   // "Create New Playlist" row (OSK name)
        PS3_MUSIC_FOLDER_ROW,// a configured music scan-folder row (a = mMusicFolders idx; Y removes)
        PS3_MUSIC_REFRESH,  // "Refresh" row in the music folders screen -> rescan the library
        // ---- Photo viewer (PS3 XMB photo port) ----
        PS3_PHOTO_ALBUM,    // a photo group-folder in the Photo column -> thumbnail grid (a = group idx)
        PS3_PHOTO,          // an individual photo row (a = photo idx) -> open the viewer
        PS3_PHOTO_PLAYLIST, // a photo playlist -> its thumbnail grid (a = playlist idx)
        PS3_PHOTO_PL_NEW,   // "Create New Playlist" row in the photo playlists screen (OSK name)
        PS3_PHOTO_FOLDER_ROW,// a configured photo scan-folder row (a = mPhotoFolders idx; Y removes)
        PS3_PHOTO_REFRESH,  // "Refresh" row in the photo folders screen -> rescan the library
        // ---- Video player (PS3 XMB video port) ----
        PS3_VIDEO_FILE,     // a video file row (a = video idx in mVideos) -> open the player
        PS3_VIDEO_FOLDER,   // folder-view group in the Video column (payloadStr = dir) -> its files
        PS3_VIDEO_FOLDER_ROW,// a configured video scan-folder row (a = mVideoFolders idx; Y removes)
        PS3_VIDEO_REFRESH,  // "Refresh" row in the video folders screen -> rescan the library
        PS3_VIDEO_PLAYLIST, // a video playlist -> its file submenu (a = playlist idx)
        PS3_VIDEO_PL_NEW,   // "Create New Playlist" row in the video playlists screen (OSK name)
        // ---- IPTV (live channels from iptv-org index.m3u; nano addition) ----
        PS3_IPTV_GROUP,     // a category -> country submenu or channels (a = mIptvCats idx)
        PS3_IPTV_COUNTRY,   // a country within a category -> channel submenu (a = cat idx, b = country idx)
        PS3_IPTV_CHANNEL,   // a channel -> open the live stream player (a = mIptvChannels idx; payloadStr = stream URL)
        // ---- Internet Radio (live audio stations m3u; nano addition, Music category) ----
        PS3_RADIO_GROUP,    // a station group -> bucket submenu or stations (a = mRadioCats idx)
        PS3_RADIO_BUCKET,   // an alpha bucket within a group -> station submenu (a = cat idx, b = bucket idx)
        PS3_RADIO_STATION,  // a station -> open the audio stream in the music player (a = mRadioStations idx; payloadStr = URL)
        // ---- File Explorer (Settings > File Explorer; nano addition) ----
        PS3_FE_DIR,         // a directory row in the file explorer (payloadStr = full path) -> navigate into it
        PS3_FE_FILE,        // a file row in the file explorer (payloadStr = full path) -> X/Triangle for options
        // ---- Network Shares (Settings > Network Shares; nano addition) ----
        PS3_NS_SHARE,       // a configured share row in the shares list (a = slot 1..kMaxShares)
        PS3_NS_ADD,         // "Add Share" row in the shares list
        PS3_NS_FIELD,       // a field row in the per-share editor (a = NsField)
        PS3_ST_ROW,         // a Syncthing screen row (a = StRow, b = index, payloadStr = id)
        // ---- Home menu item show/hide (Theme Settings > Home Categories > <cat>) ----
        PS3_ITEMHIDE_ROW,   // a static submenu-item row in the item-visibility editor (payloadStr = compound id, value Shown/Hidden)
        // ---- Pinned Apps (a Game-home shortcut list of user-chosen apps) ----
        PS3_PINNED_APPS_LIST, // "Pinned Apps" entry under Game -> the pinned-apps submenu
        // ---- Hidden games (Settings > Game Settings > Hidden Games) ----
        PS3_HIDDEN_GAME,    // a hidden game; select shows it again (payloadStr = hiddenKey)
    };
    // Game Systems editor screen kinds (Ps3Level.screenKind). Used to route the
    // X / L1 / R1 / Y buttons contextually while a GS screen is on the nav stack.
    // MUSIC_FOLDER = the music library's folder-list screen (Search for Media Servers).
    enum GsScreenKind { GS_NONE = 0, GS_LIST = 1, GS_EDITOR = 2, GS_FOLDER = 3,
                        GS_ICONGRID = 4, GS_EMUPICK = 5, GS_FOLDERBROWSE = 6,
                        MUSIC_FOLDER = 7, PHOTO_FOLDER = 8, PHOTO_GRID = 9,
                        VIDEO_FOLDER = 10, IPTV_GROUPS = 11, RADIO_STATIONS = 12,
                        FE_BROWSE = 13, APP_INFO = 14, APP_STORAGE = 15, APP_PERMS = 16,
                        SHADER_BROWSE = 17, NS_LIST = 18, NS_EDITOR = 19,
                        CAT_ORDER = 20, ITEM_HIDE = 21,
                        ST_ROOT = 22, ST_FOLDERS = 23, ST_FOLDER = 24, ST_DEVICES = 25, ST_DEVICE = 26,
                        ST_PENDING = 27, ST_OPTIONS = 28, ST_LOG = 29, ST_SHARE = 30, ST_IGNORES = 31,
                        ST_LAST = ST_IGNORES, BOXART_SRC = 32, EXT_DISPLAY = 33 };
    struct Ps3Item {
        std::string label;
        std::string desc;
        std::string value;
        std::string payloadStr;
        GLuint iconTex = 0;     // flat fallback texture (mono-white), 0 = none
        GLuint nmapTex = 0;     // normal map for the glass shader, 0 = flat fallback
        float iconR = 1.0f, iconG = 1.0f, iconB = 1.0f;  // _ChangingColor tint
        // On the flat (DSi/Minima) themes an icon that carries a normal map normally draws as a
        // theme-tinted dark/light glyph. Set this so the flat draw keeps the item's OWN iconR/G/B
        // colour instead (used by the Favourites loveheart: glass on XMB, flat RED elsewhere).
        bool  flatOwnTint = false;
        int kind = 0;
        int a = 0, b = 0;
        // Multi-select / toggle rows (Slide Up/Down actions, Passthrough Blacklist,
        // Devices to Capture, App permissions) draw a real checkbox glyph at the left
        // of the row instead of a "[Selected]"/"[Blocked]" text suffix or a right-side
        // On/Off value. -1 = not a checkbox row (render exactly as a normal row, no
        // layout change); 0 = unchecked box; 1 = checked box. Set at item build time by
        // the toggle builders and refreshed whenever the level is rebuilt in place.
        int checkState = -1;
        const Ps3DataItem* data = nullptr;  // static DATA node (children + meta)
        int action = 0;                     // 0 none, 1 dialog, 2 landing
        // Settings binding resolved ONCE at item build (makeDataItem) from the
        // item's label against the static kPs3Bindings table, instead of re-scanning
        // that table by string-compare for every visible item every frame in
        // resolvePs3ItemValue. The label is fixed for the item's lifetime (items are
        // never relabeled in place) and kPs3Bindings is static const, so the pointer
        // stays valid. nullptr for any item whose label is not a bound setting
        // (ROM / app / recent / Game-Systems-editor rows never match).
        const Ps3SettingBinding* binding = nullptr;
    };
    struct Ps3Cat {
        std::string name;
        GLuint iconTex;
        GLuint nmapTex;     // category icon normal map (glass)
        std::vector<Ps3Item> items;
    };
    struct Ps3Level {       // a submenu level on the navigation stack
        std::string title;
        std::vector<Ps3Item> items;
        int sel;
        int screenKind = 0;   // 0 = normal submenu; GS_* for the Game Systems editor screens
        int sysIdx = -1;      // >=0 only for a ROM list level (set by buildRomSubmenu); lets a
                              // rescan rebuild the open ROM column in place. -1 for every other level.
        int collectionIdx = -1;  // >=0 only for a collection's game list (set by buildCollectionSubmenu);
                                 // lets a game's option menu there offer Remove from Collection.
        int playlistIdx = -1;    // >=0 only for a music/video playlist's contents (set by buildMusic/VideoPlaylistSubmenu);
                                 // lets a track/file's option menu there offer Remove from Playlist + Reorder.
        std::string itemHideCatId;  // the category id an ITEM_HIDE editor level is editing (set by
                                    // buildCatItemVisibilityList); lets itemHideToggle rebuild it in place.
        std::string emuPlatform;    // an emulator picker level showing one catalog platform's emulators
    };
    bool mPs3Xmb = false;         // persist.gammaos.nano.ps3xmb
    bool mNdsTheme = false;       // persist.gammaos.nano.ndstheme (DSi System Menu theme, takes priority)
    bool mNdsDark = false;        // persist.gammaos.nano.nds.dark: DSi theme dark variant (dark field + light ink/glyphs)
    // DSi/NDS theme palette. Light = the original DSi Menu look (light field, dark ink); dark = the
    // dark variant (dark field, light ink, lightened teal accents + light flat icon glyphs for
    // contrast). Every DSi renderer pulls its greys/inks from here instead of hardcoded literals so
    // the two variants stay in lockstep. Returned by ndsPal(); the teal accent frame is untouched.
    struct NdsPal {
        float field, dither, edge;                 // main surface, dither line, end-cap bracket grey
        float edgeShadow;                          // recessed screen-edge column (web #db, a hair DARKER than field)
        float bevel0, bevel1, bevel2, bevel3, bevel4;  // name-box bevel ramp (outer->interior)
        float ink, subInk;                         // primary text / inactive-secondary ink (status bar)
        float watermark;                           // "GammaOS" watermark grey
        float topBg;                               // top-screen background
        float mintBevel, mintInset, mintR, mintG, mintB;   // top/info mint photo panel
        float headR, headG, headB, subR, subG, subB, valR, valG, valB;  // teal head/sub/value text
        float glyphR, glyphG, glyphB;              // flat icon glyph tint
        float dlgBg, dlgInk;                       // message-box body + text
        float tile;                                // carousel tile "jewel case" pillow tint (1=white, dark=dim)
    };
    NdsPal ndsPal() const {
        if (mNdsDark) return NdsPal{
            0.114f, 0.153f, 0.290f,
            0.075f,   // edgeShadow: a hair DARKER than the dark field so the screen-edge columns recede
                      // (a subtle inset shadow) instead of reading as light lines (HandyMarco report)
            0.541f, 0.400f, 0.290f, 0.220f, 0.157f,
            0.878f, 0.451f,
            0.235f,
            0.106f,
            0.290f, 0.180f, 0.145f, 0.325f, 0.298f,
            0.42f, 0.78f, 0.72f, 0.55f, 0.85f, 0.80f, 0.68f, 0.90f, 0.86f,
            0.82f, 0.85f, 0.90f,
            0.118f, 0.898f,
            0.235f };   // dark tile pillow (light glyph reads on it)
        return NdsPal{
            0.953f, 0.922f, 0.859f,
            0.859f,   // edgeShadow == edge in the light theme (web #db columns, already blend on #f3)
            0.318f, 0.635f, 0.765f, 0.859f, 0.984f,
            0.255f, 0.741f,
            0.827f,
            0.965f,
            0.812f, 1.0f, 0.678f, 0.839f, 0.808f,
            0.235f, 0.463f, 0.427f, 0.349f, 0.635f, 0.604f, 0.145f, 0.325f, 0.298f,
            0.28f, 0.30f, 0.36f,
            0.97f, 0.255f,
            1.0f };   // white tile pillow (identity tint)
    }
    bool mMinimaTheme = false;    // persist.gammaos.nano.minima (Minima list theme, NextUI-inspired; rides the XMB
                                  // infrastructure like the DSi theme and swaps the home render/nav/sfx/boot)
    // ES-DE theme engine: the fourth home theme (persist.gammaos.nano.ndstheme=3). Parses
    // real ES-DE theme sets and renders the system/gamelist views over the shared game
    // model. Additive + gated; the built-ins are unaffected. See docs/THEME_ENGINE.md,
    // NanoThemeEngine.cpp and NanoEsdeTheme.{h,cpp}.
    bool mEsdeTheme = false;
    // Set by any renderEsde animation site (carousel slide, marquee, description scroll, media
    // decode, GIF) that needs the next frame drawn soon; reset each frame before renderEsde. When
    // it stays false and there has been no recent input, the frame governor paces the (static but
    // expensive) ES-DE home down to an idle rate instead of re-rendering the identical frame at
    // 60fps - the ES-DE render is text/element heavy (~0.7 core) where the XMB wave is ~0.08 core.
    bool mEsdeWantsFastFrame = false;
    nanoesde::Theme mEsdeDoc;                     // parsed theme for the selected set
    bool mEsdeLoaded = false;                     // latched after the first load attempt
    std::string mEsdeSetName;                     // active theme-set directory name
    // OA-37: the chosen set, while it is not installed and the default stands in for it (else empty),
    // and when it was last looked for (esdeMissingSetRecheck, NanoThemeEngine.cpp).
    std::string mEsdeMissingSet;
    int64_t mEsdeMissingCheckMs = 0;
    // OA-37 at boot: whether the first load waits for storage (0 not decided, 1 waiting, 2 not or no
    // longer), when the wait started, and when the boot screen's startup sound was ready during it.
    int mEsdeStorageWait = 0;
    int64_t mEsdeStorageWaitMs = 0, mEsdeStorageSoundMs = 0;
    // Theme parses run on worker threads (EsdeThemeBuilder, NanoThemeEngine.cpp): each builds a
    // separate nanoesde::Theme from an EsdeBuildKey (every input of Theme::load) and the render
    // thread swaps the finished one into mEsdeDoc. At startup the keys the last boot used
    // (kEsdePrebuildFile) are built before EGL is up; a key that no longer matches is never adopted.
    struct EsdeBuildKey {
        std::string set, dir, selVariant, variant, colorScheme, aspectRatio, fontSize, locale, sysTheme, sysName;
        float aspect = 0.0f;
        bool operator==(const EsdeBuildKey& o) const {
            return dir == o.dir && variant == o.variant && colorScheme == o.colorScheme &&
                   aspectRatio == o.aspectRatio && fontSize == o.fontSize && locale == o.locale &&
                   sysTheme == o.sysTheme && sysName == o.sysName && aspect == o.aspect;
        }
        bool operator!=(const EsdeBuildKey& o) const { return !(*this == o); }
    };
    struct EsdeThemeBuilder;
    std::shared_ptr<EsdeThemeBuilder> mEsdeBuilder;
    EsdeBuildKey mEsdeDocKey;                     // inputs of the adopted mEsdeDoc (dir empty: none)
    // The theme for the other aspect ratio drawn lately (a monitor and the panel beside it each draw
    // with their own): esdeDocForAspect swaps it in when the surface's aspect changes.
    nanoesde::Theme mEsdeAltDoc;
    EsdeBuildKey mEsdeAltDocKey;
    void esdeDocForAspect();
    EsdeBuildKey mEsdeAspectFailKey;              // an aspect's theme build that failed, not retried before
    int64_t mEsdeAspectFailUntilMs = 0;
    bool mEsdeDualPanelPass = false;              // dual screen: the panel's game list pass (it plays the video)
    bool mEsdeDualHomePass = false;               // dual screen: the monitor's system view pass
    bool esdeSplit() const;                       // dual screen: system view on the monitor, game list on the panel
    // The carousel camera and grid state of the view not under the controller (the system view on the
    // monitor in dual screen mode), swapped in around its pass so the two views don't animate each other.
    struct EsdeViewAnim {
        float camOffset = 0.0f, camStart = 0.0f, camTarget = 0.0f, camAnimDur = 0.0f;
        int64_t camAnimStart = 0;
        int camCursor = -1;
        float gridScroll = 0.0f, gridScrollStart = 0.0f, gridScrollTarget = 0.0f, gridAnimDur = 0.0f;
        int64_t gridAnimStart = 0;
        int gridCursor = -1, gridLastCursor = -1, gridColumns = 1;
        float gridTransFactor = 1.0f;
        std::string marqueeLbl, hScrollRom;
        int64_t marqueeStart = 0;
        std::map<std::string, int64_t> hScrollStart;
    };
    EsdeViewAnim mEsdeOtherViewAnim;
    void esdeSwapViewAnim();
    bool mEsdePrebuildOpen = false;               // startup builds may still be adopted
    bool mEsdeEverLoaded = false;                 // ensureEsdeTheme has loaded once
    int  mEsdeMediaSig = -1;                      // storage/ES-DE folder state the variants were chosen with
    std::vector<std::pair<std::string, EsdeBuildKey>> mEsdePredict;   // kEsdePrebuildFile records
    void esdePrebuildStart();
    bool esdeBuildTheme(const EsdeBuildKey& k, const char* why);   // build k, swap into mEsdeDoc
    std::string esdeVariantFor(const std::string& selVariant, int sysIdx, const std::string& sysTheme);
    std::string esdeStandInVariant(const std::string& selVariant, int sysIdx);   // OA-37: a stand-in's, per system
    void esdePredictRecord(const char* tag, const EsdeBuildKey* k);
    // User theme folder watcher: users drop a theme under kEsdeSdcardDir (/sdcard/ES-DE/themes/<name>)
    // and the picker picks it up live. esdeSdcardThemesTick() polls the folder mtime/size (and, when
    // the active set lives there, its theme.xml/capabilities.xml) every ~1.5s and, on a change,
    // re-enumerates the installed list and forces a reparse of the active set. Cheap stat-only poll.
    int64_t mEsdeSdcardPollMs = 0;                // last time the sdcard theme folder was polled
    uint64_t mEsdeSdcardSig = 0;                  // dir-listing signature (add/remove -> re-enumerate; 0 = unseeded)
    uint64_t mEsdeSdcardActiveSig = 0;            // active-set file signature (in-place edit -> reload that set)
    std::string mEsdeRepSysTheme;                 // representative system.theme folder used at load
                                                  // (swapped per carousel item for per-system logos)
    int  mEsdeLoadedSysIdx = -1;                  // system the per-system theme data is resolved for
    int  mEsdeDecodeBudget = 0;                   // theme-art texture uploads left this frame
    // Resolve a game's media for an ES-DE imageType (screenshot/marquee/cover/...) from the shared
    // ES-DE downloaded_media tree, falling back to nano's own scraped cover. Returns tex (0 until
    // ready / none) and sets *outAR to the native aspect ratio.
    GLuint esdeGameMediaTex(const std::string& romPath, const std::string& imageType, float* outAR);
    std::string esdeGameMediaPath(const std::string& romPath, const std::string& imageType);  // resolved file ("" = none)
    void esdeForgetGameMedia(const std::string& romPath);   // re-resolve the game's media on next use
    // Every medium of the game (gone: its other files, also deleted); the number of files deleted.
    int esdeDeleteGameMedia(const std::string& romPath, int sysIdx, const std::vector<std::string>& gone);
    bool esdeMediaLayout(const std::string& romPath, int sysIdx, std::vector<std::string>* sysDirs,
                         std::vector<std::string>* names, std::string* canon);
    bool esdeMediaStems(const std::string& romPath, int sysIdx, std::string* writeDir,
                        std::vector<std::string>* names, std::vector<std::string>* haveDirs);
    std::string esdeDefaultStarPath(bool filled);   // materialise ES-DE's built-in rating star svg, return its path
    std::unordered_map<std::string, std::string> mEsdeMediaPath;  // "rom\x1ftype" -> resolved file ("" = none)
    int64_t mEsdeMediaMissFlushMs = 0;            // when esdeGameMediaPath last checked the media folders
    uint32_t mEsdeMediaRootsPresent = 0;          // which media folders were readable then (bit per root)
    void esdeForgetMediaMisses();
    void esdeReloadForSystem(int sysIdx);         // re-parse per-system data (systeminfo/colours) on
                                                  // a settled system change (XML only, no GL reload)
    int  mEsdeSysSel = 0;                         // selected system (system view)
    int  mEsdeGameSel = 0;                        // selected game (gamelist view)
    bool mEsdeInGamelist = false;                 // system view vs gamelist view
    // ES-DE view transition (system <-> gamelist), see docs/theme-engine/VIEW_TRANSITIONS.md.
    // mEsdeXsActive gates it; mEsdeXsSlide picks slide vs the black-overlay fade; mEsdeXsToGamelist
    // is the direction; mEsdeXsStart is the uptimeMillis start; mEsdeXsSwapped tracks the mid-fade
    // view swap. mEsdeForceView (0 auto / 1 system / 2 gamelist) lets renderEsde draw either view
    // from the stored selection so both can be composited during a slide.
    bool mEsdeXsActive = false, mEsdeXsSlide = false, mEsdeXsToGamelist = false, mEsdeXsSwapped = false;
    bool mEsdeXsSnapped = false;                   // slide: both views captured to FBOs once
    bool mEsdeGotoDone = false;                    // debug: gotosys jump applied once per process
    int64_t mEsdeXsStart = 0;
    int  mEsdeForceView = 0;
    unsigned mEsdeXsFboA = 0, mEsdeXsTexA = 0, mEsdeXsFboB = 0, mEsdeXsTexB = 0;
    // Scratch FBO for a rotated <text> element: nano lays out glyphs axis-aligned, so a rotated text
    // is rendered here upright then blitted rotated through drawIconTex (see esdeDrawRotatedText).
    unsigned mEsdeTextRotFbo = 0, mEsdeTextRotTex = 0; int mEsdeTextRotW = 0, mEsdeTextRotH = 0;
    void esdeDrawRotatedText(const std::string& s, float pivotX, float pivotY, float sc, int face,
                             const float col[4], float rotDeg);
    void esdeBeginTransition(bool toGamelist);    // start a transition per the resolved animation
    void esdeApplyViewSwap(bool toGamelist);      // do the system<->gamelist state change
    void renderEsdeHome();                        // renderEsde() wrapped with the active transition
    // Carousel camera-offset slide animation (mirrors CarouselComponent::onCursorChanged):
    // eases from mEsdeCamStart to mEsdeCamTarget over mEsdeCamAnimDur ms with an ease-out
    // curve, choosing the shortest wrapped path so the strip loops seamlessly.
    float   mEsdeCamOffset = 0.0f;                // current animated cursor position (wrapped)
    float   mEsdeCamStart = 0.0f, mEsdeCamTarget = 0.0f;
    int64_t mEsdeCamAnimStart = 0;                // uptimeMillis at animation start
    float   mEsdeCamAnimDur = 0.0f;               // 0 when settled
    int     mEsdeCamCursor = -1;                  // cursor the current slide targets (-1 = snap)

    // ES-DE gamelist GRID (GridComponent): vertical row scroll + the focus scale/opacity ease.
    // All scalars (no per-frame allocation); rows are windowed so only visible covers are bound.
    float   mEsdeGridScroll = 0.0f;               // animated top-of-window row (float)
    float   mEsdeGridScrollStart = 0.0f, mEsdeGridScrollTarget = 0.0f;
    int64_t mEsdeGridAnimStart = 0;
    float   mEsdeGridAnimDur = 0.0f;              // 0 = settled
    int     mEsdeGridCursor = -1;                 // cursor the current scroll targets (-1 = snap)
    float   mEsdeGridTransFactor = 1.0f;          // 0..1 focus scale/opacity ease (mTransitionFactor)
    int     mEsdeGridLastCursor = -1;             // previous selection, for its inverse ease
    int     mEsdeGridColumns = 1;                 // computed each grid frame; nav reads it for up/down
    int     mEsdeGamelistGrid = -1;               // cache: gamelist primary is a grid (-1 unknown/0/1)
    bool    esdeGamelistIsGrid();                 // true if the loaded gamelist view's primary is a grid
    bool    esdeGamelistIsHorizontal();           // true if it is a horizontal carousel (L/R scroll games)
    // The single primary navigation element ES-DE would keep for a view: the textlist/carousel/grid
    // with the alphabetically-smallest name (ES-DE instantiates the first, skips the rest). Used so
    // the renderer and the nav model agree on one primary when a variant layers several.
    const nanoesde::Element* esdeChosenPrimary(const nanoesde::View* v);
    // Play a theme navigation sound (ES-DE <sound name=...>): 0=systembrowse 1=quicksysselect
    // 2=select 3=back 4=scroll 5=favorite 6=launch. Lazily loads the theme's wav into a shared
    // low-latency player (like the XMB/DSi/Minima SFX), no-op when the theme omits that sound.
    void esdeSfx(int which);
    // Absolute wav paths for the 7 ES-DE sounds, cached when the theme loads (ensureEsdeTheme) so
    // esdeSfx does not race the per-system doc reload on the render thread. Empty = sound absent.
    std::string mEsdeSoundPath[7];
    void esdeCacheSounds();   // populate mEsdeSoundPath from the loaded theme

    // Auto-scroll state for the gamelist description container (ES-DE ScrollableContainer):
    // pause 4.5s at the top, scroll up, pause 7s at the bottom, then reset. Keyed by the game
    // so it restarts when the selection changes.
    std::string mEsdeDescKey;                     // rom the current scroll state belongs to
    int64_t     mEsdeDescStart = 0;               // ms the current text became active (delay origin)
    int64_t     mEsdeDescEndStart = 0;            // ms the scroll reached the bottom
    bool        mEsdeDescAtEnd = false;

    // --- ES-DE start menu (opened with Start; inert unless mEsdeTheme). Mirrors ES-DE's
    // GuiMenu UI-settings subset; see NanoMenuEsdeMenu.cpp. ---
    // ESDE_PG_THEME..ESDE_PG_FONT MUST stay contiguous from ESDE_PG_THEME: a root option row opens
    // page (ESDE_PG_THEME + optRow) and Back maps it back via (page - ESDE_PG_THEME). The two
    // trailing pages (downloader, apps) are handled by name, so they can live at the end.
    enum EsdeMenuPage { ESDE_PG_ROOT, ESDE_PG_THEME, ESDE_PG_VARIANT, ESDE_PG_COLOR,
                        ESDE_PG_ASPECT, ESDE_PG_FONT, ESDE_PG_DOWNLOADER, ESDE_PG_APPS };
    struct EsdeInstalledSet { std::string dir, name; nanoesde::Capabilities caps; };
    struct EsdeDlEntry {
        std::string name, reponame, url, author;
        int variants = 0, colorSchemes = 0, aspectRatios = 0, fontSizes = 0;
        bool installed = false;
    };
    bool         mEsdeMenuActive = false;         // menu open
    // A settings screen opened from the menu: Back from its last level reopens the menu on row
    // mEsdeMenuReturnSel. Start, the Anbernic button and Home leave for the game list instead.
    bool         mEsdeMenuReturn = false;
    int          mEsdeMenuReturnSel = 0;
    bool esdeMenuInContext() const { return mEsdeMenuActive || (mEsdeMenuReturn && !mPs3Stack.empty()); }
    void esdeMenuExitAll();
    void esdeMenuReopen();
    void esdeDrawMenuBackdrop(float a);   // the menu's blurred, dimmed copy of the screen behind it
    // X in an ES-DE game list: the media viewer, ES-DE's full-screen browser of a game's images.
    bool mEsdeViewerActive = false;
    std::vector<std::string> mEsdeViewerFiles;   // the game's images, in viewing order
    std::vector<std::string> mEsdeViewerTypes;   // each image's media type, for the help bar
    int  mEsdeViewerIdx = 0;                     // 0 = the game's information, then each image
    // The shown image, decoded at screen size into its own texture (freed on browse and close).
    GLuint mEsdeViewerTex = 0;
    int   mEsdeViewerTexIdx = -1;
    float mEsdeViewerAR = 1.0f;
    // The game's manual (Up opens the PDF reader) and video (Down plays it in the video player).
    std::string mEsdeViewerManual, mEsdeViewerVideo;
    void esdeViewerOpen(int sysIdx, int romIdx);
    void esdePlayGameVideo(const std::string& path);
    void esdeOverlayCloseAll();           // close the media viewer and the game information panel
    GLuint esdeDecodeImageTex(const std::string& path, int* w, int* h);   // AImageDecoder (webp, heif...)
    void renderEsdeViewer();
    // Game Options > Information in the ES-DE theme: the game's details in an ES-DE menu panel.
    bool  mEsdeInfoActive = false;
    std::string mEsdeInfoRom;
    std::string mEsdeInfoLoc;   // as ES-DE's metadata editor subtitle: "subfolder/file.zip [SNES]"
    std::string esdeRomLocation(const std::string& rom);
    float mEsdeInfoScroll = 0.0f, mEsdeInfoMaxScroll = 0.0f, mEsdeInfoAnim = 0.0f;
    // The information panel's text lines (top, bottom of the glyphs, from the top of the content)
    // and the height of its window, for L1/R1 paging.
    std::vector<std::pair<float, float>> mEsdeInfoLines;
    float mEsdeInfoViewH = 0.0f;
    std::vector<float> mEsdeInfoPageBack;   // where each R1 started, for L1 to return to
    float mEsdeInfoLayoutH = 0.0f, mEsdeInfoLayoutContent = 0.0f;   // the layout those were taken in
    float esdeInfoPageScroll(int dir);
    // Paged text where each page starts with the previous page's last line.
    static int overlapPageCount(int total, int perPage) {
        if (total <= perPage) return 1;
        const int step = perPage > 1 ? perPage - 1 : 1;
        return 1 + (total - perPage + step - 1) / step;
    }
    static int overlapPageFirst(int page, int perPage) { return page * (perPage > 1 ? perPage - 1 : 1); }
    void esdeInfoOpen(int sysIdx, int romIdx);
    void renderEsdeInfo();
    void drawEsdeInfoPanel(float a, bool inViewer);   // mEsdeInfoRom's details (the viewer's first page too)
    bool esdeOverlayKey(int code);        // a key while the viewer or info panel is up; true = consumed
    bool esdeOverlayNav(NavDir dir);      // a direction while one is up; true = consumed
    void esdeDrawPlainHelp(const std::vector<std::pair<std::string, std::string>>& prompts, float a);
    void esdeViewerMediaHelp(std::vector<std::pair<std::string, std::string>>& prompts) const;
    // PDF reader (NanoMenuPdf.cpp) for game manuals, over the media viewer.
    struct PdfShared;
    struct PdfTex { int page = -1, zoom = -1; GLuint tex = 0; int w = 0, h = 0; uint64_t used = 0; };
    bool mPdfActive = false;
    std::shared_ptr<PdfShared> mPdf;      // shared with the page worker, which keeps its own reference
    int mPdfPage = 0, mPdfZoom = 0;       // zoom: 0 fit page, 1 fit width, 2 twice the width
    float mPdfPanX = 0.0f, mPdfPanY = 0.0f, mPdfShownW = 0.0f, mPdfShownH = 0.0f;
    std::vector<PdfTex> mPdfTex;
    uint64_t mPdfClock = 0;
    bool mPdfTouchDrag = false;
    float mPdfTouchX0 = 0.0f, mPdfTouchY0 = 0.0f, mPdfTouchX = 0.0f, mPdfTouchY = 0.0f;
    void pdfOpen(const std::string& path);
    void pdfClose();
    void renderPdf();
    void pdfViewBox(float* w, float* h) const;
    const PdfTex* pdfCurrentTex() const;
    int pdfPageCount() const;
    void pdfTurn(int dir, bool toBottom);
    bool pdfNav(NavDir dir);
    bool pdfKey(int code);
    void pdfTouchPan();                   // a drag pans the page; taps go to the zones renderPdf registers
    bool mNavRepeatHold = false;          // the held direction opened something: no repeats until released
    bool mAnbernicArmed = false;   // the Anbernic button is down with no other button pressed yet
    bool mAnbernicHeld = false;    // the Anbernic button is down (Anbernic + Volume is brightness)
    int mAnbernicFd = -1;          // the input device it was pressed on
    // The armed press is Android's HOME key (devkeys::isHomeFunction): when it went down, else 0.
    // Held 3 s it goes Home (functionHomeTick); let go sooner it opens the Quick Menu.
    int64_t mAnbernicHomeDownMs = 0;
    void functionHomeTick();
    void functionHomeHeld();
    // Start and Select act when they are let go, so both pressed together can open the Quick Menu.
    bool mSsHeld[2] = {false, false};   // Start, Select: down, with the press held back
    int mSsFd[2] = {-1, -1};            // the input device each was pressed on
    std::set<int> mInputResync;         // devices whose queue overflowed: skipping to the next SYN_REPORT
    bool mSsChord = false;              // both were down together
    bool mSsUsed = false;               // another key went down while one was held: neither acts
    // Back acts when it is let go, so a Back that becomes a chord or a hold never also goes back.
    bool mBackHeld = false;             // down, with the press held back
    bool mBackUsed = false;             // another key went down while it was held
    int mBackFd = -1;                   // the input device it was pressed on
    int64_t mBackDownMs = 0;
    // Touch zones (NanoTouch.h): what the last frame drew that takes touch. Screens that register
    // their inputs get touch through touchZonesFrame instead of a hit-tester of their own.
    TouchZones mTouch;
    void touchZonesFrame();
    void touchPressButton(uint16_t code);   // as if the button was pressed and let go
    // A help prompt (an ES-DE help id: "a", "b", "x", "y", "start", "back", "l", "r", "lt", "rt",
    // "thumbstickclick") drawn at r: tapping it presses that button.
    void touchHelpPrompt(const std::string& id, const TouchRect& r);
    void touchButtonZone(uint16_t code, const TouchRect& r, bool onRelease = false);   // any on-screen button hint (BTN_* code); onRelease: a tap
    struct ReplayKey { uint16_t code; int32_t value; bool touch = false; };   // touch: a tapped prompt
    std::deque<ReplayKey> mKeyReplay;   // held-back key events to handle now
    bool mKeyReplaying = false;
    bool mKeyReplayTouch = false;   // the event being handled is a tapped prompt's button
    bool startSelectKey(uint16_t code, int32_t value);
    bool backKey(int32_t value);
    void ssReleaseTaken(const ::input_event& ev);
    void inputEventsLost(int fd);
    bool nextInputEvent(int fd, ::input_event& ev);
    void quickMenuButtonReleased();
    // Anbernic + Start: the Applications view (buildAppSubmenu) as the only level, and closing it
    // returns to where it was opened from (mAppsReturn).
    void openApplicationsView();
    void closeApplicationsView();
    bool applicationsViewOpen() const {
        return !mPs3Stack.empty() && mPs3Stack.back().title == "Applications";
    }
    // Kiosk Mode (Quick Menu, persist.gammaos.nano.kiosk): on the ES-DE home Start opens
    // Applications instead of the ES-DE menu, and Select (Game Options) does nothing in a game list.
    // Off while the Anbernic button doesn't open the Quick Menu (devkeys::kioskInEffect).
    static bool kioskMode();
    static bool functionOpensQuickMenu();   // Toolbox's switch, or the button is HOME
    bool kioskAppsOpen() const {   // Applications opened by Start in Kiosk Mode is the only level open
        return mPs3Stack.size() == 1 && mPs3Stack.back().title == "Applications";
    }
    // Settings Theme "minima": the ES-DE home's settings screens are open in the Minima theme.
    bool         mEsdeSettingsMinima = false;
    void         esdeApplySettingsTheme();
    void         esdeSettingsThemeTick();
    void         esdeReturnHome();                // an app exited: the plain ES-DE home, no Nano menu left open
    bool         mEsdeMenuClosing = false;        // fade-out in progress
    float        mEsdeMenuAnim = 0.0f;            // 0..1 open ease
    EsdeMenuPage mEsdeMenuPage = ESDE_PG_ROOT;
    int          mEsdeMenuSel = 0;                // cursor within the current page
    int          mEsdeMenuScroll = 0;             // first visible row for long picker lists
    std::vector<EsdeInstalledSet> mEsdeInstalled; // rebuilt on menu open, not per frame
    std::vector<std::pair<std::string, std::string>> mEsdeMenuPickOpts;  // active picker options
    std::string  mEsdeMenuPickProp, mEsdeMenuPickTitle;                  // active picker target
    void esdeMenuOpen();
    void esdeMenuClose();
    void esdeMenuEnumerateInstalled();
    int  esdeMenuOptionCtx(int row, std::string& label, std::string& prop,
                           std::vector<std::pair<std::string, std::string>>& opts);
    int  esdeMenuPageRows();
    void esdeMenuMove(int dir);
    void esdeMenuCycle(int dir);
    void esdeMenuSelect();
    void esdeMenuBack();
    void esdeMenuToggleAndroidGame();   // Y in Applications
    void esdeMenuApplyOption(const char* prop, const std::string& value);
    // From the ES-DE start menu's "Nano Settings" row: close the ES-DE menu and open a chooser that
    // reaches the Quick Menu and the full Settings tree (the menus the XMB/DSi/Minima carousels expose
    // directly), so the ES-DE home has settings parity with the built-in themes.
    void esdeOpenNanoSettings();
    void renderEsdeMenu();
    // ES-DE menu help prompts (icon id, label) for the focused row - rendered as glyphs by
    // esdeDrawHelp at the bottom help bar when the menu is open, matching the control's help bar.
    void esdeMenuBuildHelpPrompts(std::vector<std::pair<std::string, std::string>>& out) const;
    // Theme downloader: fetch a themes list, install a chosen theme (zip archive of the repo)
    // under the data theme root, on a detached worker polled per frame. See NanoMenuEsdeMenu.cpp.
    std::vector<EsdeDlEntry> mEsdeDlList;
    std::mutex               mEsdeDlMutex;      // guards mEsdeDlList swap from the worker
    std::thread              mEsdeDlThread;
    std::atomic<bool>        mEsdeDlFetching{false};
    std::atomic<bool>        mEsdeDlInstalling{false};
    std::atomic<bool>        mEsdeDlDone{false};   // a worker finished; join + refresh on the UI thread
    std::atomic<int>         mEsdeDlProgress{0};   // 0..100 for the active install
    std::string              mEsdeDlError, mEsdeDlInstalledName;
    int                      mEsdeDlSel = 0, mEsdeDlScroll = 0;
    void esdeDlStartFetch();
    void esdeDlStartInstall(const EsdeDlEntry& e);
    std::string esdeDlDoInstall(const EsdeDlEntry& e);   // worker body; returns "" or an error
    void esdeDlTick();
    std::vector<int> mEsdeSysList;                // enabled + non-empty system indices (reused)
    std::map<std::string, GLuint> mEsdeTexCache;  // theme image path -> GL texture
    // ES-DE SVG logo rasterization (nanosvg): rasterized textures keyed by "path@WxH",
    // aspect-fit into the requested box. Freed with mEsdeTexCache on theme-set reload.
    // pending: no texture yet because a worker is still decoding it (not cached; ask again next
    // frame). A caller with a fallback (defaultImage, text label) should draw nothing meanwhile.
    struct EsdeSvg { GLuint tex = 0; int w = 0, h = 0; bool pending = false; };
    std::map<std::string, EsdeSvg> mEsdeSvgCache;
    EsdeSvg esdeRasterSvg(const std::string& path, int boxW, int boxH);
    // Resolve ${system.theme}/${system.name}/${system.fullName} in a raw theme path to a
    // given system's values (the parser leaves ${system.*} raw so art/logos resolve per
    // system: slate uses the folder, Art Book Next the filename stem).
    std::string esdeResolveSystemPath(const std::string& raw, int sysIdx);
    // Load a theme art path (SVG or PNG/JPG) and return the texture + its contain-fit size
    // within boxW x boxH. SVG goes through esdeRasterSvg; raster art uses mEsdeTexCache +
    // native dims (mEsdePngDims, one stbi_info per file). No double-free: raster textures
    // are owned by mEsdeTexCache, mEsdePngDims holds only sizes (cleared on reload).
    EsdeSvg esdeArtTex(const std::string& path, int boxW, int boxH);
    std::map<std::string, std::pair<int, int>> mEsdePngDims;
    // Theme-art decode workers (NanoThemeEngine.cpp). esdeArtTex queues raster decodes (and an
    // SVG's first raster) to a small pool that decodes into RGBA buffers; the render thread only
    // uploads finished buffers, within mEsdeDecodeBudget per frame. A result is applied only if
    // its ticket still matches mEsdeArtPending, so a cache clear (esdeArtCancelAll) or a replaced
    // file (esdeArtForget) can never receive a stale picture.
    struct EsdeArtPool;
    struct EsdeArtResult;
    struct EsdeArtPending { uint64_t ticket = 0; bool upgrade = false; };
    std::shared_ptr<EsdeArtPool> mEsdeArtPool;
    std::unordered_map<std::string, EsdeArtPending> mEsdeArtPending;   // cache key -> request in flight
    std::map<std::string, std::pair<int, int>> mEsdeTexDecDims;        // raster path -> decoded texture size
    uint64_t mEsdeArtTicket = 0;
    int mEsdeArtPendingFrame = 0;   // art asked for in this ES-DE frame that is still decoding
    void esdeArtSubmit(const std::string& key, const std::string& path, bool svg, int boxW, int boxH, bool upgrade);
    void esdeArtApply(EsdeArtResult& r);
    bool esdeArtTakeNow(const std::string& key);
    void esdeArtPump();
    void esdeArtCancelAll();
    void esdeArtForget(const std::string& path);
    void esdeArtStats(int* decodes, int64_t* decodeMs) const;
    // ES-DE animation element (animated GIF). Every frame is decoded once via AImageDecoder's
    // frame API into its own texture, downscaled to bound memory on A133-class parts, with each
    // frame's on-screen duration; the render branch advances by wall clock. Lottie (.json) is not
    // supported. Frame textures are freed with the other caches on theme-set reload.
    struct EsdeAnim { std::vector<GLuint> frames; std::vector<int> delaysMs;
                      int nw = 0, nh = 0; int totalMs = 0; };
    std::map<std::string, EsdeAnim> mEsdeAnimCache;
    const EsdeAnim* esdeAnimGet(const std::string& path);
    // ES-DE TextListComponent marquee: the selected entry scrolls horizontally when its text
    // overflows the element width. The scroll clock resets whenever the selected label changes.
    std::string mEsdeMarqueeLbl;
    int64_t     mEsdeMarqueeStart = 0;
    // ES-DE horizontal text container (containerType=horizontal): each overflowing single-line value
    // (e.g. linear's Developer/Publisher) marquee-scrolls independently. Keyed by rom+element so the
    // scroll clock restarts on a new game; the map is scoped to the current game (cleared on change).
    std::map<std::string, int64_t> mEsdeHScrollStart;
    std::string mEsdeHScrollRom;
    void renderEsde();
    void renderEsdeSecondary();
    void ensureEsdeTheme();
    void esdeSdcardThemesTick();                  // watch /sdcard/ES-DE/themes for user-dropped themes
    void esdeMissingSetRecheck(const char* why);  // OA-37: load the chosen set once it is back
    void esdeRebuildSysList();
    // The ES-DE system and game in focus at a launch, for the fresh home after it: the overlay home
    // starts at the first launch with no state of its own. esdeApplyReturn restores it once the
    // library has that system.
    void esdeSaveReturnPath();
    bool esdeApplyReturn();
    std::string mEsdeReturn;          // "systemId|inGamelist|cacheKey(rom path)", pending restore
    int64_t mEsdeReturnReadMs = 0;    // uptime ms it was read, 0 = not yet
    // ES-DE variant triggers (ViewController per-system scan): a system with no matching game
    // media renders with the selected variant's <override useVariant> instead of the variant
    // itself. nano keeps one scraped image per game (the box, reused for the video element), so
    // every image/video mediaType maps onto "has a scraped box". esdeSystemHasMedia scans a
    // system's roms for that; esdeEffectiveVariant resolves the per-system variant name.
    bool esdeSystemHasMedia(int sysIdx, const std::vector<std::string>& mediaTypes);
    std::string esdeEffectiveVariant(const std::string& selectedVariant, int sysIdx);
    // Shared ES-DE chrome: rounded background plate, help-icon path lookup, and the
    // helpsystem / systemstatus element renderers (see NanoThemeEngine.cpp).
    void esdeDrawPlate(const nanoesde::Element* e, float cx, float cy, float cw, float ch);
    std::string esdeHelpIconPath(const std::string& id);
    // menuOverlayPass=false is the normal home element pass. When the options menu is open the
    // menu's help bar must draw ON TOP of the dim/blur, so that pass only caches the matching
    // helpsystem element + view context (mEsdeHelp*) and renderEsdeMenu calls back with
    // menuOverlayPass=true to actually draw the menu legend over the panel.
    void esdeDrawHelp(const nanoesde::Element* e, bool gamelist,
                      const nanoesde::Element* primary, bool menuOverlayPass = false);
    const nanoesde::Element* mEsdeHelpElem = nullptr;     // helpsystem matching the current view
    const nanoesde::Element* mEsdeHelpPrimary = nullptr;  // its view primary (icon selection)
    bool mEsdeHelpGamelist = false;                       // cached view flag for the deferred draw
    void esdeDrawSystemStatus(const nanoesde::Element* e);
    void esdeDrawClock(const nanoesde::Element* e);
    // The theme's clock and status icons over Nano's own menus, as ES-DE draws them over its menus.
    void esdeDrawStatusBar();
    void esdeNav(int dx, int dy);
    // ES-DE home touch, through mTouch: a swipe changes system (across), opens the system's games
    // (up) or goes back (down); a tap opens a system or picks a game (a second tap launches it); a
    // drag scrolls the primary along its own axis, with fling.
    void esdeTouchListZone(const TouchRect& area, bool games, int axis, float pitch);
    void esdeTouchEntryZone(const TouchRect& area, const TouchRect& r, bool games, int idx);
    void esdeTouchEntry(bool games, int idx);
    bool esdeTouchStep(bool games, int axis, int dir);
    void esdeTouchSwipe(int axis, int dir);
    bool esdeHomeTouchLive() const;
    bool mEsdeTouchStepping = false;               // esdeNav from a drag or fling, not a button
    bool esdeSelect();
    bool esdeBack();
    bool esdeFocusedSystem(int& sysIdx);
    void esdeToggleFavorite();
    void esdeOpenGameInfo();
    void esdeQuickSystemSelect(int dir);
    void esdeJumpToEdge(int dir);
    void esdeRandomGame();
    bool mPs3BottomClock = false; // persist.gammaos.nano.ps3xmb.bottomclock (PSP clock on the bottom panel, dual-screen XMB)
    // Half Resolution (XMB theme only, three INDEPENDENT Theme Settings toggles). Each renders only its
    // subsystem at half resolution and sharp-linear upscales it; the rest stays full-res. Perf for weak
    // SoCs (A133P class). Read once at startup, applied live from closePs3Dialog.
    bool mPs3HalfResWave  = false;  // persist.gammaos.nano.ps3xmb.halfres.wave  (the moving wave wallpaper)
    bool mPs3HalfResIcons = false;  // persist.gammaos.nano.ps3xmb.halfres.icons (the glass home icons)
    bool mPs3HalfResClock = false;  // persist.gammaos.nano.ps3xmb.halfres.clock (the PSP-style clock)
    // Transient per-frame gates: set once at the top of render() (the shared XMB-home scope AND the matching
    // member), read by the wave push / drawGlassIcon / drawPspClock so they need no re-check.
    bool mWaveHalfActive  = false;
    bool mIconsHalfActive = false;
    bool mClockHalfActive = false;
    bool mControlCenterEnabled = false; // persist.gammaos.nano.ps3xmb.controlcenter (bottom-screen dashboard over a single-screen app)
    // Bottom-panel PSP clock reveal (own scalar, independent of the F12 summon mPspClockReveal).
    // 1 while the bottom clock is enabled (set once per frame in render()), 0 when it is turned off;
    // consumed by renderPspClockSecondary.
    float mPspBottomReveal   = 0.0f;
    // Bottom clock's OWN second-hand comet trail (persistent, accumulates across frames). The F12
    // summon's mPspTrail is save/restored inside renderPspClockSecondary and must never accumulate
    // the bottom clock's history, so the bottom clock keeps its own; advanced every frame (even the
    // 30fps-cap skip frames) so it stays smooth and correctly timed.
    float mPspBottomTrail[120] = {0};
    int   mPspBottomFrameCtr = 0;    // per-frame counter for the 30fps render cap
    // 30fps cap cache: the full composited secondary (wave + clock, panel-native) snapshotted on a
    // render frame and re-presented on the next skip frame, so the heavy clock passes run at 30fps.
    unsigned int mPspBottomCacheTex = 0;
    int   mPspBottomCacheW = 0, mPspBottomCacheH = 0;
    bool  mPspBottomCacheValid = false;
    // ---- DSi System Menu theme (NanoMenuNds; 1:1 port of /work/nds launcher) ----
    // Aspect-adaptive: the 256x192 DSi design letterboxes into any panel. On the dual-screen
    // RG DS the carousel goes to the bottom panel and the DSi top screen to the top; single
    // panels get the carousel scaled to fit.
    void renderNds();             // DSi launcher home, aspect-adaptive; orchestrates the panel layout
    void ensureNdsAssets();       // one-shot: load the DSi sprites + read the stack prop

    // ---- Minima theme (NextUI-inspired minimal list launcher; NanoMenuMinima.cpp) ----
    void renderMinima();                                             // orchestrator: pick single/dual-panel layout
    void renderMinimaList(float rx, float ry, float rw, float rh);   // the vertical list into a device-px rect
    void renderMinimaSecondary(float rx, float ry, float rw, float rh); // RG DS bottom panel: category + boxart
    void renderMinimaSidePanel(float rx, float ry, float rw, float rh); // option menu / list+slider choosers, Minima style
    void renderMinimaDialog(float rx, float ry, float rw, float rh);    // confirm / message dialog, Minima style
    void renderMinimaInfoPage(float rx, float ry, float rw, float rh);  // game/app Information page (cover+metadata+paged synopsis)
    bool  minimaBackdrop(float rx, float ry, float rw, float rh);       // home backdrop; true = in-game scrim left as is
    float minimaStatusPill(float rx, float ry, float rw, float rh, bool draw);   // top-right status pill; returns its left x
    // Minima Applications grid: the Applications level as a 5-column launcher grid with a search bar.
    // The level's sel stays the focused app, so launch / options / pin work on it unchanged.
    bool minimaAppGridActive() const;                    // Minima draws the stack and its top is Applications
    bool minimaAppGridOnTile() const;                    // the focus is a shown app tile (not the search bar)
    int  minimaAppGridPos() const;                       // sel's index in mAppGridIdx, -1 when filtered out
    void renderMinimaAppGrid(float rx, float ry, float rw, float rh);
    void minimaAppGridSync();                            // refilter when the query or the app list changed
    void minimaAppGridReveal();                          // scroll the selected row on screen
    void minimaAppGridMove(int dx, int dy);              // d-pad / stick: a column or a row (up from row 0 = search bar)
    void minimaAppGridPage(int dir);                     // L1/R1: a screenful of rows, keeping the column
    void minimaAppGridOpenSearch();                      // the keyboard for the search bar (filters live)
    void minimaAppGridClear();                           // empty the search
    void minimaAppGridReset();                           // a fresh Applications view: unfiltered, at the top
    std::vector<int>         mAppGridIdx;                // shown items (level indices) for the query
    std::vector<std::string> mAppGridKeys;               // lower-cased labels the filter matches
    std::vector<std::string> mAppGridLbl;                // fitted tile labels (line 1, line 2 per item)
    std::vector<uint8_t>     mAppGridLblOk;              // mAppGridLbl entry is fitted for the current layout
    std::string mAppGridQuery;                           // search text
    std::string mAppGridFiltered;                        // the query mAppGridIdx was built for
    bool   mAppGridSnap = true;                          // snap the scroll + highlight on the next frame
    bool   mAppGridOnSearch = false;                     // the search bar has the focus
    bool   mAppGridOskOpen = false;                      // the keyboard is open for the search bar
    bool   mAppGridDirty = true;                         // the level was rebuilt: refilter
    size_t mAppGridItemN = 0;                            // item count the filter was built for
    float  mAppGridLblKey = 0.0f;                        // layout the fitted labels were made for
    int    mAppGridTop = 0;                              // first visible row
    int    mAppGridCols = 5, mAppGridRows = 1;           // columns, fully visible rows (last layout)
    float  mAppGridScroll = 0.0f;                        // eased first visible row
    float  mAppGridHlX = 0.0f, mAppGridHlY = 0.0f;       // eased highlight cell (column, row)
    int  minimaListLevel(int** sel);                     // home list level: row count + live selection
    bool minimaOptSubOpen() const;                       // the option panel lists its open submenu
    bool minimaPanelStep(bool optSrc, bool subOpen, int dir);   // side-panel list drag: one row
    bool minimaSliderStep(int steps);                    // side-panel slider: move by steps
    void minimaAccent(float& r, float& g, float& b) const;          // accent RGB from the shared Colour setting
    NdsPal ndsTopPal() const;                                       // ndsPal() with the mint canvas + teal text recoloured to a non-Original accent
    bool   ndsEffectActive() const { return mNdsTheme && mCurrentEffect != 0; }   // a Background Effect replaces the DSi wallpaper/field
    void   drawNdsEffectBackdrop();                                 // render the effect (via its FBO) full-panel once per frame per panel
    bool   ndsFxOverlayReady() const;                               // this panel's effect texture was rendered this frame
    void   ndsFxOverlay(float x, float y, float w, float h, float alpha);   // re-blit the effect over a chrome band (= chrome at 1-alpha)
    void   drawRoundedRing(float x, float y, float w, float h, float radius, float thick,
                           float r, float g, float b, float a);
    uint64_t mNdsFxDrawnHb = ~0ULL;                                 // heartbeat of the last effect draw (per-frame guard)
    int      mNdsFxDrawnPanel = -1;                                 // panel that draw was for
    GLuint   mNdsFxTex[2] = {0, 0}, mNdsFxFbo[2] = {0, 0};          // per-panel offscreen effect render (window orientation)
    int      mNdsFxW[2] = {0, 0}, mNdsFxH[2] = {0, 0};
    void ndsAccentRGB(float& r, float& g, float& b) const;          // DSi accent RGB (reference azure at "Original")
    bool ndsAccentIsDefault() const;                                // true = Colour "Original" (keep the baked blue sprites)
    void ndsRecolor(float& r, float& g, float& b) const;            // hue-rotate a DSi blue shade toward the accent
    GLuint ndsFrameTexAccented();                                   // selection-frame sprite hue-rotated toward the accent (cached)
    bool minimaSolidBg(float* r, float* g, float* b);               // Theme > Background Colour: solid backdrop (else black)
    void minimaSfx(int which);                                      // trigger a Minima SFX (MIN_SFX_* id, NanoMenuPS3Boot.cpp)
    void minimaSfxTick();                                           // per-frame: fire nav/drill/back/launch by state diff
    // Minima render/animation state (NextUI feel: pill glides between rows, list windows to keep sel visible).
    bool    mMinimaWantsFrame = false; // set by the Minima renderer while anything still animates (scroll ease,
                                       // level transition, marquee); cleared by the loop before each render
    float   mMinimaSelAnim   = 0.0f;   // eased selected-row index (pill glide, ~3-frame linear)
    float   mMinimaScroll    = 0.0f;   // eased list scroll top (in rows)

    bool    mListWrapSnap    = false;  // set by ndsNavHoriz on a wrap-around jump; the Minima list +
                                       // DSi submenu snap their scroll to the new end (no glide-through)
    float   mMinimaMarquee   = 0.0f;   // marquee offset (px) for a long selected label
    int     mMinimaMarqueeSel = -1;    // which row the marquee offset belongs to (reset on change)
    int64_t mMinimaMarqueeStart = 0;   // uptimeMillis when the current long label settled (start-pause)
    int     mMinimaPrevDepth = -1;     // last nav depth (root/category/submenu) for the slide+fade transition
    int     mMinimaTransDir  = 0;      // +1 drill / -1 back, for the horizontal slide direction
    int64_t mMinimaTransStart= 0;      // uptimeMillis() when the level changed (drives the enter/back transition)
    int     mMinimaSfxDepth  = -1;     // ndsNavDepth snapshot for minimaSfxTick state-diff
    int     mMinimaSfxSel    = -1;     // ndsFocusSel snapshot for minimaSfxTick state-diff
    void renderNdsCarousel(float rx, float ry, float rw, float rh, bool singleFull = false);  // DSi bottom screen into a device rect (singleFull: lone panel -> status bar top, navbar bottom)
    void ndsCarouselRect(float& rx, float& ry, float& rw, float& rh);  // device rect the DSi carousel/list/panel renders into: bottom band when stacked, whole panel otherwise (single source of truth for render + touch)
    bool ndsMapTouchDs(float& dsX, float& dsY);                      // map the current raw touch into DS 256x192 coords using ndsCarouselRect; returns whether touchMapRaw resolved
    void renderNdsTop(float rx, float ry, float rw, float rh);       // DSi top screen (status bar + content)
    void drawNdsStatusBar(float cx, float offY, float scale);        // DSi status bar (radios + date/time + battery) at DS y2..17, shared by the top screen and the single-screen carousel top strip
    void drawNdsArrowBtn(float x0, float y0, float wpx, float hpx, int dir,
                         float outerDS = 3.0f, float innerDS = 1.5f);  // scrollbar L/R favColor pill; per-side corner radii (DS px)
    void drawNdsPillGrad(float x0, float y0, float wpx, float hpx,
                         float radLeftDS = 3.0f, float radRightDS = 3.0f,
                         bool flat = false, float fr = 0.0f, float fg = 0.0f, float fb = 0.0f);  // 21-stop favColor pill; per-side corner radii (DS px); flat=grey-rim mode
    void ndsTouchFrame();         // DSi carousel touch: tap a tile / L-R button, drag to scroll (bottom panel)
    void ndsSubmenuTouch();       // DSi submenu list touch: tap a row to enter, tap Back to pop
    void ndsPickerTouch();        // DSi picker list touch (Time Zone / System Language): tap a row to pick
    // The handlers above (and ndsSidePanelTouch, ndsDialogTouch, ndsWizTouch, gsearchTouch) serve the
    // RG DS bottom panel, a secondary display. On the primary panel the DSi screens register zones.
    bool ndsZonesLive() const;                           // this render pass's DSi zones take touch
    bool ndsTouchStep(int depth, int dir, bool fast);    // the carousel drawn at depth, one card on
    bool ndsListTouchLevel(std::vector<Ps3Item>*& items, int*& sel);   // the settings list level shown
    bool ndsListTouchScroll(int depth, int dir, int fitRows);   // the settings list one row on (false at an end)
    void ndsListTouchJump(int depth, int scrollRow, int fitRows);   // the settings list scrolled to a row
    void ndsListTouchTap(int depth, int row);            // a settings list row tapped: pick and enter it
    void ndsSliderTouchSet(float v);                     // the DSi slider chooser set to v (snapped)
    static constexpr float kNdsDlg3BtnX[3] = {22.0f, 94.0f, 166.0f}, kNdsDlg3BtnW = 68.0f;   // three dialog buttons (DS px)
    // DSi single/dual screen handling. mNdsStackMode is the persist.gammaos.nano.ndstheme.stack
    // request: 0=auto (stack on a single-screen device, dual on a two-panel device), 1=force
    // stacked, 2=force off (carousel-only on a single screen). mNdsStack is the EFFECTIVE value
    // computed per frame from the mode + whether a live secondary panel exists, so a single-screen
    // device shows BOTH DSi screens stacked instead of dropping the top screen.
    int  mNdsStackMode = 0;
    bool mNdsStack = false;       // effective: stack both DSi screens on one panel this frame
    bool mNdsHadSecondary = false;// latched: a secondary panel has been seen (never flips back,
                                  // so a transient glFbo=0 during the boot handoff can't briefly
                                  // toggle a dual device into single-screen stacked mode)
    // Stacked-carousel navigation (user redesign): the root carousel is the XMB CATEGORIES;
    // selecting one drills a level down (the parent carousel slides up + dims out of focus,
    // the child comes into focus below). Back walks up. Submenu levels are carousels too, not
    // the settings list. mNdsAtRoot = showing the categories; else showing a category/submenu.
    bool  mNdsAtRoot = true;               // true = the categories carousel is the focused level
    std::vector<Ps3Item> mNdsCatCards;     // the categories rendered as carousel cards (built once)
    bool  mNdsCatCardsBuilt = false;
    void  ndsBuildCatCards();              // populate mNdsCatCards from mPs3Cats
    void  ndsNavHoriz(int dir);            // cycle the focused carousel's selection
    void  ndsNavSelect(bool allowLaunch = true);  // enter/drill the focused card; allowLaunch=false (D-pad/buttons) navigates the hierarchy but never launches a leaf (launch is touch-only)
    void  ndsNavBack();                    // walk up one level (pop submenu / leave category / at root: noop)
    bool  ndsCurLevelIsList() const;       // current drill level is a settings screen -> DSi vertical list, not the carousel
    void  ndsBumperSkip(int dir);          // L1/R1 page-skip through a drilled DSi/Minima list (bumper scrolling)
    int   ndsNavDepth() const;             // 0 = root categories, 1 = a category, 2+ = submenu levels
    bool  ndsInModal() const;              // a chooser/dialog/OSK/player owns nav -> delegate to XMB handlers
    int   ndsFocusSel() const;             // current selection index of the focused carousel
    int   ndsFocusCount() const;           // number of cards in the focused carousel
    bool  ndsPlayerActive() const;         // a media player is up -> show the XMB player, not the carousel
    bool  ndsDlgIsSidePanel() const;       // the active dialog is a chooser/slider (list) vs a confirm (buttons)
    void  ndsSaveReturnPath();             // persist the nav path (cat + stack sels) before a launch-exit
    // How long the home holds the exit after a launch is armed (mLaunchFadeStart): none, a
    // launch hands off at once in every theme.
    int   launchFadeHoldMs() const { return 0; }
    // Origin of the DSi launch effect (tile lift / ring / white wash): always 0, launches have no
    // effect, as in the other themes.
    int64_t ndsLaunchOriginMs() const { return 0; }
    // Fire service.bootanim.nano_retroarch=1 (init turns it into do_launch, which starts the
    // app). On the DRM-direct home the app cannot be seen until nano exits, so it fires at
    // select time to give the cold start a head start. On the SurfaceFlinger-composited home
    // the app's window covers nano's layer the moment it draws, which cut the launch effect
    // short, so there it is deferred to the moment the effect completes (pollInput's exit gate).
    void  armAppLaunchTrigger(const std::string& syncPkg = std::string());   // syncPkg: else launch_app
    bool  mLaunchTriggerDeferred = false;
    // A browser launch whose default browser a worker is asking Android for: the render thread
    // writes the hand-off and arms it (urlLaunchTick) once mUrlLaunchComp is set; the exit waits.
    bool  mUrlLaunchPending = false;
    std::string mUrlLaunchUrl;
    std::atomic<bool> mUrlLaunchResolved{false};
    std::mutex mUrlLaunchMutex;
    std::string mUrlLaunchComp;             // under mUrlLaunchMutex
    void  urlLaunchTick();
    void  ndsSfxPreload(int which);        // decode a DSi clip ahead of its first trigger (idle)
    void  ndsRestoreReturnPath();          // on the fresh return process, drill back to the launched card
    float mNdsCamera = 3.0f;      // carousel scroll position (slot units, fractional while sliding)
    float mNdsSettleT = -1.0f;    // select-landing squash: frames (0..3) since the frame settled (-1 = idle)
    bool  mNdsCamMoving = false;   // was the carousel camera moving last frame (to detect a fresh landing)
    // DSi carousel touch gesture state (bottom panel; reuses touchMapRaw). The web
    // drives the camera 1:1 from the finger (launcher.scrub: camera = downCam - dx/65),
    // then a 0.85/frame momentum fling on release, then snaps (main.js menuTouch*).
    bool  mNdsTouchMoved = false;
    float mNdsTouchDownX = 0.0f, mNdsTouchDownY = 0.0f;  // DS-space touch-down point
    int   mNdsDragMode = 0;       // 0 none, 1 carousel drag, 2 thumb, 3/4 L/R arrow, 5 track jump, 6 swipe, 7 back; list: 8/9 arrow, 10 groove jump
    float mNdsDragDownCam = 0.0f; // camera at drag start (carousel finger-follow anchor)
    float mNdsDragLastCam = 0.0f; // camera last move (for release velocity)
    float mNdsFlingVel = 0.0f;    // active momentum fling velocity (slot units/frame), 0 = idle
    bool  mNdsScrubbing = false;  // finger (or fling) owns the camera -> skip the nav lerp
    // Carousel scrollbar pitch (DS px per card). The DSi rail is 5 px per card, which
    // saturates the pill travel (x19..208) after 38 cards; longer lists compress the
    // pitch so the pill reaches the right end exactly on the last card.
    static float ndsPillStep(int nItems) {
        if (nItems <= 1) return 5.0f;
        const float fit = 189.0f / (float)(nItems - 1);
        return fit < 5.0f ? fit : 5.0f;
    }
    // Ticks stay at least 5 px apart: on a compressed rail only every k-th card gets one.
    static int ndsTickEvery(float step) {
        if (step >= 5.0f) return 1;
        return (int)ceilf(5.0f / step);
    }
    bool  mNdsThumbHeld = false;  // scrollbar pill grabbed -> pressed light-blue window, frame hidden
    bool  mNdsFastScroll = false; // scrollbar blank-track press -> fast ease-out glide (launcher.scrollTo)
    // DSi boot->carousel entrance cascade (launcher._introFall): icons spring-fall in,
    // staggered from the centre outward, replayed each time the DSi home appears.
    int64_t mNdsIntroStart = 0;   // uptimeMillis the entrance began (0 = done/not started)
    // DSi "4x" redrawn vector fonts (DSVec letters / DSVecNum digits): loaded as extra
    // FT faces; ensureGlyph prefers them only while mNdsFontPref is set (NDS text only).
    int  mNdsFontIdx = -1;        // mFtFaces index of dsvec.ttf (letters), -1 = not loaded
    int  mNdsNumIdx  = -1;        // mFtFaces index of dsvecnum.ttf (digits), -1 = not loaded
    int  mEsdeDefaultFace = -1;  // mFtFaces index of Akrobat (ES-DE default for no-fontPath text), -1 = UI font
    bool mNdsFontPref = false;    // when true, ensureGlyph tries the DSVec faces first
    // ES-DE theme fonts: a text element carries a fontPath (e.g. Art Book Next's Mulish); the
    // ES-DE renderer resolves it to an mFtFaces index via esdeFontFace() and passes that as the
    // preferFace to drawText/measureText so the glyph is rasterised from the theme's own typeface
    // rather than the default UI font. Loaded on demand and cached by absolute path.
    std::map<std::string, int> mEsdeFontFaces;   // absolute .ttf path -> mFtFaces index (or -1)
    int esdeFontFace(const std::string& path);   // load-on-demand, returns the face index or -1
    // DSi "4x" SVG-rasterised sprites (the real firmware assets, redrawn as vectors):
    // cell_00 selection frame (transparent centre) + the white pillow tile. Loaded lazily.
    GLuint ndsLoadTex(const char* name);   // decode /data|/system nano_xmb/nds/<name>.png -> RGBA tex
    GLuint ndsLoadTexMem(const unsigned char* data, int len);   // decode an embedded PNG -> RGBA tex
    void   ensureNdsRing();                // lazy-load the 36 launch sparkle-ring frames
    // DSi boot->carousel entrance: vertical offset (DS px, from settled) of the tile at
    // screen offset `off` from centre at intro frame `f`; returns -1000 = not yet visible.
    float  ndsIntroFall(int off, float f);
    void   ndsCommitSelect(int slot);      // commit a scrub/fling landing as the selection
    // DSi System Settings submenu list (settings.js): a dark scanline screen with glossy
    // grey/blue button rows. Used for the nano XMB submenu levels (mPs3Stack non-empty).
    void   renderNdsSubmenu(float rx, float ry, float rw, float rh);
    // DSi-styled modal overlays for the stacked-carousel nav (user redesign): the option
    // menu + list/slider choosers render as the DSi System Settings glossy list (side panel),
    // and confirm dialogs (System Update, exit settings) render as the DSi message box.
    void   renderNdsSidePanel(float rx, float ry, float rw, float rh);  // choosers / option menu -> settings list
    void   renderNdsDialog(float rx, float ry, float rw, float rh);     // Yes/No confirm -> DSi message box
    // DSi-styled full-screen picker list (System Language / Time Zone): the dense scrollable
    // glossy-button list from the web Country/Language screens, replacing the XMB language list
    // and 3D globe when the DSi theme is active so those selectors match the rest of the menu.
    void   renderNdsPickerList(float rx, float ry, float rw, float rh, const char* title,
                               const std::vector<std::string>& labels, int sel);
    void   ndsSidePanelTouch();            // tap/scroll the DSi side-panel list
    void   ndsDialogTouch();               // tap the DSi dialog buttons
    void   drawNdsGlossyBtn(float x, float y, float w, float h, float r, bool sel);
    // Look up a colour-chooser swatch's RGB by index (the kPs3ColorOpts table is file-local to
    // NanoMenuPS3Menu.cpp; this lets the DSi/Minima renderers draw real colour swatches too).
    bool   ps3SwatchColor(int ci, float& r, float& g, float& b) const;
    // DSi scrolling-list scrollbar (settings.js _drawCountryBottom / _scrollArrowVec): a recessed
    // groove with a glossy favColor-blue up/down arrow button at each end and a glossy blue thumb
    // with a white grip. cx/offY/scale reconstruct the caller's DS->device mapping; trackTopDS..
    // trackBotDS is the full bar span in DS-y; thumbFrac = visible fraction, scrollFrac = 0..1 pos.
    // With step (one scroll row, false at an end) and jumpTo (a scroll row of 0..positions-1), its
    // arrows, thumb and groove take touch.
    void   drawNdsListScrollbar(float cx, float offY, float scale,
                               float trackTopDS, float trackBotDS, float thumbFrac, float scrollFrac,
                               int positions = 0, std::function<bool(int)> step = nullptr,
                               std::function<void(int)> jumpTo = nullptr);
    float  mNdsSubScroll = 0.0f;  // submenu list scroll offset (rows), smoothed toward the selection
    // DSi enter/back screen transition (settings.js press/fadeOut/hold/fadeIn): the new screen
    // fades in from black on every submenu enter or Back, masking the instant stack switch.
    int    mNdsPrevStackDepth = -1;
    int64_t mNdsSubTransStart = 0; // uptimeMillis the current enter/back transition began (0 = none)
    int    mNdsTransDir = 0;       // +1 = drilled down (cards fall in from top), -1 = backed up (rise from bottom)
    int    mNdsSubDownSel = 0;     // submenu selection at touch-down (vertical drag-scroll anchor)
    // DSi list momentum scroll + draggable scrollbar (web launcher.js scrub/fling model, 1:1): the
    // finger scrubs mNdsSubScroll directly, release flings it with a 0.85/frame decay, the scrollbar
    // thumb tracks the finger 1:1, and the selection is the row at the vertical centre of the band.
    float  mNdsListScrubDown = 0.0f; // mNdsSubScroll at touch-down (pixel-scroll drag anchor)
    float  mNdsListFlingVel = 0.0f;  // fling velocity (rows/frame), decays at 0.85 per frame; 0 = idle
    bool   mNdsListScrub = false;    // finger owns mNdsSubScroll (active content drag)
    bool   mNdsListThumb = false;    // scrollbar thumb grabbed (1:1 follow, no fling)
    GLuint mNdsFrameTex = 0;      // cell_00_blue frame sprite (blue border + START platform)
    // Accent-following copy of the selection frame: the blue sprite hue-rotated (value-preserving)
    // toward the Colour accent so it keeps its gloss/shadows instead of the old flat multiply that
    // crushed the shadows to black. Base pixels kept in memory for cheap re-tint on accent change.
    std::vector<uint8_t> mNdsFrameBasePx;   // decoded RGBA of nds_frame (original blue), cached once
    int    mNdsFrameBaseW = 0, mNdsFrameBaseH = 0;
    GLuint mNdsFrameTexAccent = 0;          // uploaded hue-rotated frame; 0 = not built
    int    mNdsFrameAccentIdx = -999;       // Colour idx mNdsFrameTexAccent was built for (-999 = none)
    float  mNdsFrameBaseHue = -1.0f;        // measured dominant hue of the base frame sprite (deg; -1 = unknown)
    GLuint mNdsTileTex  = 0;      // tile_white pillow sprite
    GLuint mNdsPhotoTex = 0;      // photo_U panel (grey/white bevel frame + mint field), top screen
    GLuint mNdsBattTex  = 0;      // spr_batt_full sprite (unknown-level fallback; the live battery is procedural + proportional)
    // Status-bar glyph textures: crisp framework SystemUI vector icons (rasterised to mono-white
    // PNGs) tinted per state, replacing the old hand-drawn procedural speaker/wifi/bt/note.
    GLuint mNdsSbSpeaker = 0, mNdsSbSpeakerMute = 0, mNdsSbWifi = 0, mNdsSbBt = 0, mNdsSbNote = 0;
    bool   mNdsSbIconsLoaded = false;
    // DSi top-screen game preview (#66): the focused ROM's scraped fanart, decoded async
    // into its own slot (SA_NDS_FAN) so it can cross-fade with the boxart in the mint panel.
    GLuint mNdsFanTex = 0; int mNdsFanW = 0, mNdsFanH = 0;
    std::string mNdsFanPath;      // fanart path currently loaded into mNdsFanTex ("" = none)
    std::string mNdsPreviewRom;   // ROM path the preview art is currently focused on
    float mNdsPreviewT0 = -1.0f;  // mEffectTime the current preview began (drives the box<->fan cross-fade)
    std::string mNdsPrevPreviewRom;  // outgoing game held for the game-to-game preview dissolve
    float mNdsGameXfadeStart = -1.0f; // mEffectTime the game->game preview transition began (-1 = settled)
    // web _displaySelected(): the name box (bottom) and the top-screen mint panel HARD-SWAP
    // the shown selection - the outgoing title stays crisp until the incoming card is 42/58
    // (~72%) of the way centred, then flips. Item->item never cross-fades (launcher.js only
    // dissolves populated<->empty). mNdsSlideFrom is the slot the current slide left;
    // mNdsDispSel is the slot currently shown, published by renderNdsCarousel and read by
    // renderNdsTop so both screens agree.
    int mNdsSlideFrom = -1;
    int mNdsDispSel = 0;
    GLuint mNdsRingTex[36] = {0}; // launcher_d cell_53..88 sparkle-ring frames (launch effect)
    bool   mNdsRingLoaded = false;// one-shot lazy load guard for the 36 ring frames
    bool   mNdsTexLoaded = false; // one-shot load guard
    bool mPs3MenuBuilt = false;
    float mPs3UiScale = 1.0f;     // persist.gammaos.nano.ps3xmb.uiscale (menu zoom)
    float mUserFontScale = 1.0f;  // user Font Size (persist.gammaos.nano.fontscale mirror of System font_scale)
    void  refreshUserFontScale(); // cheap per-frame prop read -> ps3::gFontScale (all themes)
    // ES-DE home on a cold boot: the first frames are drawn black so the theme art still
    // decoding (esdeArtTex's per-frame budget) is never seen, then the home shows at once.
    // Boot screen (NanoBootScreen.cpp): the logo and what the device is doing, until the home can draw.
    bool    mBootScreenActive = false;
    int     mBootScreenFrames = 0, mBootScreenReadyFrames = 0;
    int64_t mBootScreenStartMs = 0;
    GLuint  mBootScreenBaseTex = 0, mBootScreenStatusTex = 0;
    int     mBootScreenStatusRect[4] = {0, 0, 0, 0};
    std::string mBootScreenStatus;
    bool    mBootScreenHolding = false;   // this frame it only waits for the startup sound: no home under it
    void bootScreenArm(bool quickResume);
    void bootScreenFrameStart();
    bool bootScreenDefersHome() const;
    std::string bootScreenStatus() const;
    void bootScreenClose();
    void renderBootScreen();
    void bootSoundStart();        // NanoMenuPS3Boot.cpp: the ES-DE startup sound, made ready during boot
    int  bootSoundState() const;  // 0 getting ready, 1 ready, 2 none
    void bootSoundPlay();
    void bootSoundDrop();         // not played: the stream, if open, is closed
    // XMB chrome reveal multipliers (1 = fully shown): the in-game overlay entrance, the
    // standalone slide clock and the music player cross-fade scale these.
    float  mPs3ChromeLabelReveal = 1.0f; // category-label fade-in
    float  mPs3ChromeIconReveal = 1.0f;  // category-icon / item / clock pop-in
    // ---- PS3 Settings dialogs + Theme Settings (NanoMenuPS3Menu.cpp) ----
    // action='dialog' DATA leaves open either a side-panel chooser (Theme
    // Settings: Theme/Colour/Background/Font/Day-Night) or a fullscreen message
    // dialog (System Update, System Information, Format Utility, ...).
    bool   mPs3DlgActive = false;
    bool   mPs3DlgKeepOpen = false; // set by an accept handler that reconfigured the dialog in place (uninstall progress); closePs3Dialog then leaves it up
    int    mPs3DlgKind = 0;        // 0 = fullscreen message/chooser, 1 = side-panel theme chooser
    int    mPs3DlgThemeKey = 0;    // 0 none, 1 theme, 2 colour, 3 background, 4 font, 5 day/night
    std::string mPs3DlgTitle;
    std::string mPs3DlgBody;
    std::vector<std::string> mPs3DlgOptions;
    std::vector<int> mPs3DlgSwatch;   // COLOR_OPTIONS index per option for the Colour chooser, else -1
    int    mPs3DlgSel = 0;
    int    mPs3DlgOrigSel = 0;     // value at open, for revert on cancel
    float  mPs3DlgAnim = 0.0f;     // open slide/fade 0->1
    bool   mPs3DlgClosing = false; // side-panel dismiss fade-out in flight (kind 1)
    float  mPs3DlgCloseAnim = 0.0f;// close fade alpha 1->0 (reverse of mPs3DlgAnim)
    bool   mPs3DlgBlurValid = false;
    const Ps3SettingBinding* mPs3DlgBinding = nullptr;  // active settings-bound chooser (else null)
    std::unordered_map<std::string, std::string> mPs3BindCache;  // cached bound values by leaf label
    // Side-panel numeric slider (mPs3DlgKind==1, mPs3DlgThemeKey==0). For bound
    // settings whose options string is "slider:min:max:step[:scale]" (too many
    // values for a list - LED brightness, dB gains, float macro params). Left/Right
    // adjust by step; X commits the formatted value via writeSettingValue, O cancels.
    bool   mPs3DlgSlider = false;
    float  mPs3DlgSldMin  = 0.0f;
    float  mPs3DlgSldMax  = 1.0f;
    float  mPs3DlgSldStep = 1.0f;
    float  mPs3DlgSldVal  = 0.0f;
    int    mPs3DlgSldScale = 0;    // decimal places (0 = integer)

    // ---- GammaOS "System Update" (OTA) flow (NanoMenuOta.cpp) -----------------
    // nano owns the online check + download, drawn through the existing themed dialog
    // chrome (renderPs3Dialog for XMB, renderNdsDialog for DSi) via a dynamic dialog +
    // in-place reconfigure, then hands off to the gammaos-ota native flasher which draws
    // the themed flashing screens (it reads sys.gammaos.ota.theme). Because gammaos-ota
    // stops the framework and writes partitions, nano must release the DRM display first;
    // the handoff sets the OTA props + sys.gammaos.nano.start_ota=1 then _exit(0) (process
    // teardown releases DRM master; an init rule starts gammaos-ota once nano is stopped).
    bool        mOtaFlowActive = false;
    int         mOtaStage = 0;              // OtaStage constants (defined in NanoMenuOta.cpp)
    std::mutex  mOtaMutex;                  // guards the result fields + mOtaGen below
    uint32_t    mOtaGen = 0;                // worker generation, bumped on every spawn/cancel so a
                                            // stale worker (cancel + re-enter) publishes nothing
    bool        mOtaWorkerDone = false;     // guarded by mOtaMutex
    bool        mOtaWorkerOk = false;       // guarded by mOtaMutex
    bool        mOtaAvail = false;          // guarded by mOtaMutex
    std::string mOtaVersion, mOtaUrl, mOtaFilename, mOtaCurrentVer, mOtaError;  // guarded by mOtaMutex
    uint64_t    mOtaSize = 0;               // guarded by mOtaMutex
    std::atomic<int>  mOtaProgress{0};      // advisory 0..100 download progress (lock-free UI read)
    // In-nano storage-media browser: nano scans + lists + confirms the OTA package ITSELF
    // (owning the DRM panel), handing off to the flasher ONLY at Install. The *.zip list
    // reuses the chooser dialog chrome (mPs3Dlg*); mOtaBrowseFiles[mPs3DlgSel] = the current
    // selection. mOtaBrowseFiles is published by the scan worker under mOtaMutex.
    struct OtaFileEntry { std::string path; std::string label; uint64_t size; };
    std::vector<OtaFileEntry> mOtaBrowseFiles;   // discovered *.zip (guarded by mOtaMutex on publish)
    std::string mOtaSelectedZip;                 // chosen package path, handed off at Install
    std::string mOtaPeekVersion, mOtaPeekParts;  // manifest peek result (guarded by mOtaMutex)
    void startOtaFlow(bool internet);       // entry from the "System Update" method chooser accept
    void otaFlowTick();                     // per-frame: consume worker results + live download %
    bool otaDialogAccept();                 // from ps3XmbSelect when mOtaFlowActive; true = handled
    void otaEndFlow();                      // dismiss + invalidate any in-flight worker
    void otaSetDialog(const char* title, const std::string& body,
                      const std::vector<std::string>& opts, int type, int defSel);
    void otaStartFlash(const char* pkg);    // start the flasher pre-staging + show "Preparing" in nano
    bool otaInBrowse() const;               // true while the OTA package browser (list) is up (DSi list-style)
    bool otaInProgress() const;             // true on a non-interactive OTA progress screen (checking/downloading/reading/preparing) - suppress confirm buttons + footer hints
    bool otaInBrowseConfirm() const;        // true on the storage-media "Install this package?" confirm
    bool otaCopying() const;                // copying a storage-media package to the private folder
    void otaCancelCopy();                   // stop that copy (ends the flow); no-op otherwise
    void otaSleepGuardTick();
    void otaScreenOffTick();                // screen off during an update: wake it again               // per-frame: hold the nano_ota wakelock while otaInProgress()
    bool        mOtaWakeLockHeld = false;   // render thread only
    void otaBrowseInit();                   // scan storage + show the themed *.zip chooser dialog
    void otaBrowseShowList();               // (re)show the cached file list as a chooser dialog
    void otaBrowseSelect();                 // A on a file: peek its manifest off-thread
    void otaBrowseConfirm();                // show the themed Install/Cancel confirm dialog
    void scanOtaZips(std::vector<OtaFileEntry>& out) const;   // worker-thread safe (touches only out)
    bool peekOtaManifest(const std::string& zip, std::string& ver,
                         std::string& parts) const;           // worker-thread safe

    // ---- Home XMB option menu (Triangle / X) ----------------------------------
    // The web optMenu context "sidebar": pressing Triangle on a focused item opens
    // a small list of real per-item actions (Start, Play, Information). A separate
    // modal from the theme chooser so an action like Information can open a dialog
    // without colliding with its own state.
    bool   mPs3OptActive = false;
    bool   mPs3OptClosing = false;
    float  mPs3OptAnim = 0.0f;
    float  mPs3OptCloseAnim = 0.0f;
    bool   mPs3OptBlurValid = false;
    float  mPs3OptBlurT = 0.0f;
    std::vector<std::string> mPs3OptLabels;   // row labels
    std::vector<std::string> mPs3OptActs;     // parallel action ids
    std::vector<char>        mPs3OptStart;    // parallel: 1 = draw a START pill
    int    mPs3OptSel = 0;
    // Option side-panel marquee: the focused row's label horizontally scrolls (loop with a gap)
    // when it is too long for the panel's text column, instead of truncating at the screen edge.
    float   mPs3OptMarquee = 0.0f;       // current scroll offset (device px)
    int     mPs3OptMarqueeSel = -1;      // the row the offset belongs to (reset on selection change)
    int64_t mPs3OptMarqueeStart = 0;     // uptimeMillis when the current row got focus (read pause)
    // Snapshot of the item the menu was opened on (the column may not change while
    // the modal is up, but snapshotting keeps the action self-contained).
    int    mPs3OptCtxKind = 0;
    int    mPs3OptCtxA = 0, mPs3OptCtxB = 0;
    std::string mPs3OptCtxLabel, mPs3OptCtxPayload, mPs3OptCtxDesc;
    std::vector<Ps3Item> mPs3OptCtxList;
    int    mPs3OptCtxSel = 0;
    int    mPs3OptCtxPlaylist = -1;   // >=0 when the option menu opened over a playlist's contents (music/video/photo)
    // Nested side-panel submenu (web optMenu.subOpen/subRows): a parent row can
    // carry a list of sub-rows. Opening it slides the main column left and shows the
    // submenu in the right slot, the selected sub-row aligned to the parent. Single
    // level only. The vectors are populated only while the menu is open (no idle cost).
    struct Ps3OptSub {
        std::string label;
        int kind = 0;      // 0 = sort, 1 = group content, 2 = slideshow style, 3 = per-app orientation, 4 = dual-stack, 5 = run-on-primary-screen, 6 = keep-alive-in-background
        int field = 0;     // sort: 0 = film date, 1 = import date, 2 = name
        int dir = 1;       // sort: 0 = desc, 1 = asc
        int groupIdx = 0;  // group-content mode index
        int sstyle = 0;    // slideshow style 0..4
        std::string orient; // per-app orientation value ("" = default/none, else landscape/portrait/rev_*)
        bool dsEnable = false; // dual-stack toggle: true = add the package to the whitelist, false = remove
        bool psEnable = false; // run-on-primary-screen toggle: true = add to persist.gammaos.nano.primary_pkgs
        bool bgEnable = false; // keep-alive-in-background toggle: true = add to persist.gammaos.nano.background_pkgs
    };
    std::vector<char> mPs3OptSep;                       // parallel: 1 = separator row (skipped in nav)
    std::vector<char> mPs3OptHasSub;                    // parallel: 1 = row opens a submenu
    std::vector<int>  mPs3OptSubDef;                    // parallel: default sub-selection
    std::vector<std::vector<Ps3OptSub>> mPs3OptSubRows; // parallel: each row's submenu rows
    bool   mPs3OptSubOpen = false;
    int    mPs3OptSubSel = 0;
    void   openXmbOpt();          // build context rows + open (no-op if nothing useful)
    void   closeXmbOpt();
    int    xmbOptDefaultSel();    // first per-item action after the list-group separator
    void   xmbOptMove(int dir);
    void   xmbOptEnter();         // activate the highlighted row
    void   xmbOptOpenSub();       // open the focused row's submenu (Right / Cross)
    void   xmbOptCloseSub();      // close the open submenu back to the parent list (Left / Circle)
    void   xmbOptApplySub(const Ps3OptSub& sr);   // apply a chosen submenu row
    void   xmbOptAction(const std::string& act);
    void   renderXmbOpt();
    // GammaEQ audio preview: a looping PCM clip played via AAudio so the equalizer
    // is audible while adjusting it (the FastMixer EQs the speaker mix). mEqPrevPcm
    // is interleaved int16 at mEqPrevRate/mEqPrevChans; the callback owns mEqPrevPos.
    // mEqPreviewWanted is the user's intent; mEqPreviewOn is the actual play state.
    // The stream is opened on a detached worker so a not-yet-ready audio HAL never
    // blocks the UI/boot; eqPreviewTick() retries until the HAL is available.
    std::atomic<bool> mEqPreviewOn{false};
    std::atomic<bool> mEqPreviewWanted{false};
    std::atomic<bool> mEqPrevOpening{false};
    std::atomic<bool> mEqPrevPcmReady{false};    // the clip is loaded into mEqPrevPcm
    std::atomic<bool> mEqPrevLoadStarted{false}; // a background PCM load is in flight/done
    std::mutex mEqPrevMutex;           // guards mEqPrevStream commit / teardown
    float  mEqPrevRetryT = 0.0f;       // mEffectTime of the last open attempt
    void*  mEqPrevStream = nullptr;    // AAudioStream* (opaque here; AAudio.h is .cpp-only)
    std::vector<int16_t> mEqPrevPcm;
    volatile size_t mEqPrevPos = 0;
    int    mEqPrevRate  = 48000;
    int    mEqPrevChans = 2;
    int    mEqGammaEqDepth = -1;   // mPs3Stack depth of the GammaEQ submenu (-1 = not in it)
    void   startEqPreview();       // user toggled on (non-blocking)
    void   stopEqPreview();        // user toggled off / left the page / teardown
    void   tryStartEqPreviewAsync();
    void   eqPreviewOpenWorker();
    void   eqPreviewTick();        // per-frame retry while wanted but not playing
    void   ensureEqPcmAsync();     // background-load the clip (once)
    void   warmEqPreview();        // preload the clip when entering GammaEQ (no playback)
    void   freeEqPcm();            // release the 25MB clip when leaving GammaEQ
public:
    int32_t eqFillAudio(void* audioData, int32_t numFrames);   // called by the AAudio data callback (free fn)
private:
    // Render-thread watchdog: render() bumps mRenderHeartbeat every frame; a
    // background thread aborts (-> debuggerd tombstone with every thread's stack,
    // then init restarts us) if it stops advancing for ~8s. Turns a silent hang
    // into a diagnosable stack + an auto-recovery instead of a frozen device.
    std::atomic<uint64_t> mRenderHeartbeat{0};
    bool   mWatchdogStarted = false;
    // True while enterDrmSleep() intentionally parks the render thread (screen off /
    // system suspend). The render loop stops bumping the heartbeat then, so the
    // watchdog must skip its stall check or it would abort the whole process (which
    // also kills background music) on every power-button sleep.
    std::atomic<bool> mInDrmSleep{false};
    // Idle sleep after the Android Screen Timeout (Settings.System screen_off_timeout).
    // PowerManager pins the panel on while the nano menu is visible (menu_active: the
    // framework never sees nano's pad input), so nano times the idle itself, the boot
    // intro and its disclaimer included. Cached value in ms, -1 = Never, -2 = not read yet.
    std::atomic<int64_t> mIdleSleepTimeoutMs{-2};
    std::atomic<bool>    mIdleSleepReading{false};
    int64_t              mIdleSleepReadMs = 0;      // uptime of the last cache refresh
    void idleSleepTick();                           // NanoMenuInput.cpp; render thread, after pollInput()
    // True while opening a video on the render thread. NanoVideo::open() (and track
    // Async video open (no UI freeze on warmup). The blocking open work (extractor build,
    // codec create/configure/start, NanoHls network) runs on mVidOpenThread OFF the render
    // thread; the render thread shows a cancelable spinner and adopts the decoder once the
    // worker signals done (acquire/release handoff). The render thread is watchdog-protected
    // throughout an open (it spins the spinner); only a blocking TEARDOWN join of a wedged
    // worker (sleep/occlusion) is watchdog-exempt, via mVidTeardownExempt below.
    std::atomic<bool> mVidOpenInProgress{false};  // an open worker is live, decoder not yet adopted
    std::atomic<bool> mVidOpenDone{false};        // worker sets LAST (release); render reads (acquire)
    std::atomic<bool> mVidOpenOk{false};          // worker result (stored before mVidOpenDone)
    std::atomic<bool> mVidTeardownExempt{false};  // scoped watchdog exemption around a blocking teardown join
    std::thread mVidOpenThread;                    // per-open worker (never move-assigned while joinable)
    float  mVidOpenStartT = 0.0f;                  // mEffectTime at open begin (spinner delay + 30s deadline)
    bool   mVidOpenCancelReq = false;             // Back/sleep/occlusion/deadline asked to cancel
    bool   mVidStepActive = false;                // a next/prev/auto-advance step-retry chain is running
    int    mVidStepDir = 0;                        // its direction
    int    mVidStepTries = 0;                      // candidates left to try
    struct VidPending { bool isStream = false; std::string file; std::string url; int w = 0, h = 0;
                        int resumeChoice = -1; double resumeSec = 0.0; int vidIdx = 0; };
    VidPending mVidPending;                         // params captured at vidBeginOpen, applied on adopt
    std::string mVidSoloTitle;                      // title of a single file played outside the library (a game's video)
    void   vidBeginOpen(const VidPending& p);       // render: GL alloc + spawn the open worker
    bool   vidOpenTitleRun();                       // worker: blocking title open (reads mVidPending)
    bool   vidOpenStreamRun();                      // worker: blocking IPTV stream open
    void   vidAdoptOpen();                          // render: adopt a finished+joined successful open
    void   vidAbortOpen(const char* banner);        // render: tear down a failed/canceled open (then fade to XMB)
    void   vidStepRetryNext();                      // render: advance to the next step candidate (after prior joined)
    void   startRenderWatchdog();
    // Fullscreen dialog page (mPs3DlgKind==0). Mirrors web DIALOG_TEMPLATES +
    // drawDialog: a body type, an optional vector illustration, a notice line and
    // the source item's header icon.
    int    mPs3DlgType = 0;        // 0 info, 1 chooser, 2 chooser_illust, 3 confirm
    // Rich ROM Information page (a type-0 dialog variant): scraped cover + faint
    // fanart + metadata rows, with file path/size/core fallback when not scraped.
    bool   mPs3DlgRomInfo = false;
    std::string mPs3RomInfoSyn, mPs3RomInfoGenre, mPs3RomInfoPlayers, mPs3RomInfoRating;
    std::string mPs3RomInfoDate, mPs3RomInfoDev, mPs3RomInfoPub;     // scraped metadata
    std::string mPs3RomInfoFileName, mPs3RomInfoDir, mPs3RomInfoSize, mPs3RomInfoCore, mPs3RomInfoSystem;
    bool   mPs3RomInfoCoreIsApp = false;   // true = standalone app (label "App"), false = libretro core ("Core")
    int    mPs3RomInfoScroll = 0;  // first visible wrapped description line (Up/Down scroll)
    // DSi theme: a game Information page (scraped OR unscraped file-facts) is open. Routed to the
    // top-screen mint canvas on a dual-screen device (like the PS3 XMB) with L/R pagination.
    bool   mPs3DlgGameInfo = false;
    // The User Guide (Settings > User Guide): reuses the game-info page plumbing so DSi/Minima get
    // their paged full-screen renderers, but it is NOT a game - it has no cover/fanart/metadata and
    // wants a plain, full-width, readable text layout that SPANS BOTH DS screens (top = the first
    // half of a "spread", bottom = the continuation), instead of an empty mint canvas up top and a
    // cramped narrow body below. renderNdsInfoPage / renderMinimaInfoPage branch on this.
    bool   mPs3DlgHelp = false;
    int    mNdsInfoPage = 0;               // current description page (L/R paginate)
    int    mNdsInfoPageCount = 1;          // total pages (computed each render; 1 -> hide L/R)
    void   renderNdsInfoPage(float rx, float ry, float rw, float rh, int part);  // 0=full(single) 1=top(cover+meta) 2=bottom(description)
    void   ndsInfoPage(int dir);           // L/R: turn the info page (clamped)
    bool   ndsGameInfoActive() const { return mPs3DlgGameInfo && (mPs3DlgActive || mPs3DlgClosing); }
    // A kind-0 info dialog (single OK, no Yes/No, not a side-panel chooser) whose body overflows
    // one panel and therefore paginates with L/R (System Information, music tags, ...).
    bool   ndsDlgInfoPaged() const { return (mPs3DlgActive || mPs3DlgClosing) && !mPs3DlgGameInfo
                                          && !ndsDlgIsSidePanel() && (int)mPs3DlgOptions.size() <= 1
                                          && mNdsInfoPageCount > 1; }
    // App Information page (kind-0 dialog, filled asynchronously): "info" on an app sets
    // sys.gammaos.nano.appinfo_req=<pkg>#<n>, the framework writes the details file and
    // bumps sys.gammaos.nano.appinfo_gen, and we swap the "Loading..." body for it.
    bool        mPs3DlgAppInfo        = false;
    bool        mPs3DlgAppInfoPending = false;
    // Dual-SCREEN detect prompt (kind-0 confirm dialog): true while the "run on primary screen?"
    // offer is up; the target package is kept so closePs3Dialog can act on the chosen option.
    bool        mPs3DlgDualScreen     = false;
    std::string mPs3DlgDualScreenPkg;      // package the dual-screen prompt acts on
    std::string mDualScreenPromptedPkg;    // last pkg we opened the prompt for this session (de-dupe)
    std::string mPs3AppInfoNonce;         // "<pkg>#<n>" we asked for; must match the file's req| line
    int         mPs3AppInfoScroll     = 0;
    int         mPs3AppInfoWaitFrames = 0;
    std::string mPs3AppInfoPkg;            // package the open App Information level acts on
    // Parsed App Information (filled by parseAppInfo from the framework's tagged file); the
    // Information page and the Storage / Permissions submenus all build from these.
    struct AppPerm { std::string perm, label; bool granted; };
    bool                     mAppInfoLoaded = false;
    std::vector<std::string> mAppInfoFacts;              // "Label    value" display rows
    std::string              mAppInfoCacheSz, mAppInfoDataSz;
    std::vector<AppPerm>     mAppInfoPerms;
    bool readNanoAppInfo(const std::string& nonce, std::string& bodyOut);
    void parseAppInfo(const std::string& body);          // tagged file -> the members above
    void buildAppInfoLevel(Ps3Level& out);               // facts + Storage + Permissions rows
    void buildAppStorageLevel(Ps3Level& out);            // Clear Cache / Clear Data
    void buildAppPermsLevel(Ps3Level& out);              // per-permission grant/deny toggles
    void appInfoTick();                    // per-frame: async-refresh the App Information levels
    // Auto-scroll: glide the Applications cursor to a freshly installed app "as if the nav
    // button were held", reusing the accelerating nav cadence + the item ease.
    int     mPs3AutoScrollTarget = -1;     // >=0: step the cursor toward this row
    int64_t mPs3AutoScrollLastMs = 0;
    int     mPs3AutoScrollCount  = 0;
    void tickAutoScroll();
    // Set while an L1/R1 page-skip steps the cursor many rows at once: ps3NavSound()
    // no-ops so the burst does not fire (and additively stack) a cursor cue per step;
    // one cue is emitted for the whole jump. See ps3XmbBumperSkip.
    bool    mSuppressNavSfx = false;
    GLuint mPs3DlgFanTex = 0;       // fanart texture for the info page (freed on dialog close)
    int    mPs3DlgFanW = 0, mPs3DlgFanH = 0;
    GLuint mPs3DlgBoxTex = 0;       // cover texture for the info page (freed on dialog close)
    int    mPs3DlgBoxW = 0, mPs3DlgBoxH = 0;
    std::string mPs3DlgPendingFan, mPs3DlgPendingBox;  // async-decode target paths (drain routes results here)
    int    mPs3DlgIllust = 0;      // 0 none,1 hdmi_cable,2 av_multi,3 hdd_warning,4 globe,5 controller,6 bd_remote
    std::string mPs3DlgNotice;     // chooser_illust bottom notice line
    unsigned int mPs3DlgIconTex = 0;   // header item icon (flat fallback)
    unsigned int mPs3DlgIconNmap = 0;  // header item icon (glass normal map)
    float  mPs3DlgIconR = 1.0f, mPs3DlgIconG = 1.0f, mPs3DlgIconB = 1.0f;
    // Live Theme Settings selection indices (mirror the web themeIdx/colorIdx/
    // bgIdx/fontIdx/daynightIdx). They reflect the CURRENT applied-or-previewing
    // value so the menu rows show the selection inline (resolvePs3ItemValue),
    // exactly like the web resolveItemValue. Loaded from props at init.
    int    mPs3ThemeIdx = 0;
    int    mPs3ColorIdx = 0;   // 0..kPs3ColorCount-1 = preset; kPs3ColorCount = the "Custom..." accent (mPs3ColorCustomHex)
    std::string mPs3ColorCustomHex;   // "#RRGGBB" of the custom theme accent (persist.gammaos.nano.ps3xmb.color_custom)
    int    mPs3BgIdx = 0;
    int    mPs3FontIdx = 0;
    int    mPs3DayNightIdx = 5;   // default: Night (kPs3DayNightOpts index 5)
    // --- Custom wallpapers (user image/video behind the XMB + DSi menus) -----------------------------
    // A user-chosen still or video fills the home background per screen; the XMB wave defaults off when a
    // wallpaper is set. Textures are decoded once on load (photoDecodeTex) and drawn cover-fit by
    // drawWallpaperFill(); wallpaperActive(panel) gates the draw. Per-screen: 0 = primary/top, 1 = bottom
    // secondary (RG DS). All members declared up front so later stages need no NanoMenu.h recompile.
    GLuint mWpTexTop = 0, mWpTexBottom = 0;      // decoded still-image textures (0 = none)
    int    mWpTopW = 0, mWpTopH = 0, mWpBottomW = 0, mWpBottomH = 0;   // source px (for cover-fit)
    std::string mWpPathTop, mWpPathBottom;       // XMB wallpaper file paths (empty = use the wave)
    std::string mWpPathDsiTop, mWpPathDsiBottom; // DSi wallpaper file paths (empty = the two-tone field)
    bool   mXmbWave = true;                       // XMB wave visible (default derived: off when a wallpaper is set)
    bool   mXmbWaveExplicit = false;              // the user set the wave toggle explicitly (honour it verbatim)
    int    mRenderingPanel = 0;                   // which panel the current render pass targets (0 top, 1 bottom)
    std::string mWpVideoRefused;   // video wallpaper path refused this session (too large, or its decoder failed): never re-opened
    NanoVideo* mWpVideoTop = nullptr;             // the single video-wallpaper decoder (top/primary panel, v1)
    NanoVideo* mWpVideoBottom = nullptr;          // reserved (single HW decoder -> only the top plays video in v1)
    bool   mWpTopIsVideo = false;                 // the top wallpaper path is a video (play it instead of a still)
    std::string mWpVideoPath;                     // the video file currently loaded as the wallpaper
    std::thread mWpVideoThread;                   // async open worker (openAsyncRun off the render thread)
    std::atomic<bool> mWpVideoOpenDone{false};    // worker finished (release); render thread reads (acquire)
    std::atomic<bool> mWpVideoOpenOk{false};      // worker result (stored before mWpVideoOpenDone)
    bool   mWpVideoAdopted = false;               // open succeeded + play() started
    bool   wpIsVideoPath(const std::string& p) const;  // true if the path looks like a video file (by extension)
    void   wpVideoStart(const std::string& path); // begin an async open of a video wallpaper (render thread)
    void   wpVideoStop();                          // tear the video wallpaper down (join worker, free decoder)
    void   wpVideoTick();                          // per-frame: adopt a finished open, loop on end
    bool   drawTopVideoWallpaper();                // draw the current video frame cover-fit; true if it drew
    // ES-DE theme background video: a <video> element with a fixed file path (Adroit's Animated
    // backgroundvideo -> background.mp4). A DEDICATED decoder so it does not entangle the user's video
    // wallpaper; while it plays it borrows the SoC's single HW decoder (the wallpaper is stopped and
    // its re-open is gated on mEsdeBgVideo being null). Same async-open + adopt + loop model as wpVideo.
    NanoVideo* mEsdeBgVideo = nullptr;
    std::string mEsdeBgVideoPath;                  // video file currently loaded as the ES-DE background
    std::thread mEsdeBgVideoThread;
    std::atomic<bool> mEsdeBgVideoOpenDone{false};
    std::atomic<bool> mEsdeBgVideoOpenOk{false};
    bool   mEsdeBgVideoAdopted = false;
    void   esdeBgVideoTick(const std::string& wantPath, bool audio = false);  // start on path change, adopt, loop; "" stops
    bool   esdeBgVideoDraw(float x, float y, float w, float h, int fit = 1, float alpha = 1.0f);  // current frame into the rect
    float  esdeBgVideoAspect() const;      // width / height of the loaded video (0 = none yet)
    void   esdeBgVideoStop();
    // The decoder is shared by a theme's background video and the focused game's video: video
    // elements ask for a file while the home is drawn (esdeVideoWant; a game's video outranks a
    // background) and esdeVideoResolve plays the winner at the start of the next pass. A game's
    // video plays its sound through mEsdeBgAudio.
    std::string mEsdeVidWantPath;
    int     mEsdeVidWantPrio = -1;
    bool    mEsdeVidWantAudio = false;
    int64_t mEsdeVidLastWantMs = 0, mEsdeVidResolvedMs = 0;
    std::string mEsdeGameVidRom;           // the game whose video delay is running
    int64_t mEsdeGameVidSinceMs = 0;
    struct EsdeVidAudio;                   // the sound, opened on its own thread once the picture plays
    std::shared_ptr<EsdeVidAudio> mEsdeBgAudio;
    bool    mEsdeBgVideoWithAudio = false;
    int     mEsdeBgVideoPrio = -1;         // rank of the loaded video (1 = a game's)
    void    esdeVideoWant(const std::string& path, int prio, bool audio);
    void    esdeVideoResolve();
    void    esdeVideoIdleCheck();          // stop a theme video once the home is no longer drawn
    bool    esdeGameVideoAllowed() const;
    // Live hover preview in the Video Wallpaper picker: decode the currently-focused video and draw it
    // into its grid cell. Only ONE decoder runs at a time (the single HW decoder), so the tick stops the
    // video wallpaper while previewing and lets wpVideoTick re-adopt it on leaving the picker. Mirrors
    // the wpVideo* members/flow. A short focus-settle debounce avoids thrashing the decoder while scrolling.
    NanoVideo* mVidPreviewDec = nullptr;          // the picker hover-preview decoder (separate from wallpaper/player)
    std::string mVidPreviewPath;                  // the video currently loaded in the preview
    std::string mVidPreviewWant;                  // the focused cell's video path (target); changes reset the settle timer
    std::thread mVidPreviewThread;                // async open worker
    std::atomic<bool> mVidPreviewOpenDone{false};
    std::atomic<bool> mVidPreviewOpenOk{false};
    bool   mVidPreviewAdopted = false;
    double mVidPreviewSettleT = 0.0;              // mEffectTime when mVidPreviewWant last changed (debounce)
    void   vidPreviewStart(const std::string& path);
    void   vidPreviewStop();
    void   vidPreviewTick();                       // per-frame (render thread): manage the focused-cell preview
    bool   drawVidPreviewInto(float x, float y, float w, float h);  // draw the live preview frame into a cell
    float  wallpaperScrimAlpha() const;           // adjustable wallpaper dimming (Theme Settings "Wallpaper Dimming")
    void   loadWallpaperTextures();               // (re)decode the wallpaper stills from the props (frees old)
    void   wallpaperRetryIfNeeded();              // retry the wallpaper load until it succeeds (cold-boot storage race)
    int64_t mWpRetryLastMs = 0;                   // throttle stamp for wallpaperRetryIfNeeded
    void   drawWallpaperFill(int panel);          // cover-fit blit of the panel's wallpaper over the full panel
    void   compositeWallpaperIntoWorkTex(int panel);  // paint the panel's wallpaper into ps3bg's work texture so
                                                  // the frost backdrops, glass icons + PSP clock sample it, not the gradient
    void   fillWorkTexSolid(float r, float g, float b);  // clear ps3bg's work texture to a flat colour (theme clock backdrop)
    void   drawPspClockThemeBackdrop();           // DSi/Minima home slide clock: paint the theme backdrop to screen + workTex
    bool   wallpaperActive(int panel) const;      // true if this panel has a still (or video) wallpaper set
    int    mWpPickTarget = -1;                    // wallpaper picker in progress: -1 none, 0 top, 1 bottom
    void   openVideoWallpaperPicker();            // Theme Settings -> the video library as a wallpaper picker (top only)
    bool   mWpVideoPick = false;                  // the wallpaper grid is picking a VIDEO (film-badge cells; select short-circuits)
    std::vector<int> mWpPickVidList;              // when mWpVideoPick: mVideos indices, parallel to mPhotoGridList
    void   openWallpaperPicker(int target);       // Theme Settings -> the Photos album grid in wallpaper-pick mode
    void   wallpaperApplyPick(const std::string& file);  // write the chosen file to the target prop + reload + live
    void   clearWallpaper();                      // clear the active theme's wallpaper (both screens) + wave back on
    // Custom box art: a focused game's Triangle menu can pick any image and store it as that
    // game's cover in the scrape cache, so every theme (XMB / DSi / Minima / ES-DE) shows it.
    std::string mBoxartPickRom;                   // ROM path the boxart picker is targeting ("" = none)
    std::string mBoxartPickName;                  // its display name (seeds the manifest title if empty)
    void   openBoxartPicker();                    // Set Boxart: where the picture comes from (BOXART_SRC level)
    void   buildBoxartSources(Ps3Level& out);
    void   boxartApplyPick(const std::string& srcFile);  // copy the picked image into the cover cache + refresh
    void   resetBoxart(const std::string& romPath);      // remove a game's custom cover from the cache + manifest
    // Date and Time settings (functional). Date Format / Time Format are nano-
    // local display choices the clock honours; Daylight Saving reflects the real
    // current DST state (tm_isdst, refreshed each frame in drawPs3Clock) and the
    // toggle switches the timezone between the Olson zone (auto DST) and a fixed
    // standard-offset Etc/GMT zone (no DST). Set Manually's two OSK fields are
    // staged in mPs3DtDate / mPs3DtTime.
    int    mPs3DateFormatIdx = 2;   // 0=YYYY/MM/DD 1=MM/DD/YYYY 2=DD/MM/YYYY (web order)
    int    mPs3TimeFormatIdx = 1;   // 0=12-Hour 1=24-Hour (web order)
    bool   mPs3DstNow = false;      // Daylight Saving currently active (live from tm_isdst)
    // Clock digital-string cache: the formatted "D/M H:MM" string only changes on
    // a minute (or format) boundary, so rebuild the snprintf set only when the key
    // changes. measureText stays live (cheap, keeps the right-anchor correct under
    // resize/orientation with no extra invalidation).
    char   mPs3ClockStr[112] = {0};
    int    mPs3ClockKMin = -1, mPs3ClockKHour = -1, mPs3ClockKMday = -1,
           mPs3ClockKMon = -1, mPs3ClockKDateFmt = -1, mPs3ClockKTimeFmt = -1, mPs3ClockKLocale = -1;
    // Last-drawn XMB clock/status bar rect (device px), captured by drawPs3Clock so the confirmation
    // toast (drawPhotoBanner) can render as a matching open-right bar directly below it. mPs3ClockBarT
    // is mEffectTime of the capture (staleness guard; the toast only mirrors a fresh clock bar).
    float  mPs3ClockBarL = 0, mPs3ClockBarR = 0, mPs3ClockBarTop = 0, mPs3ClockBarBot = 0;
    float  mPs3ClockBarCorner = 0, mPs3ClockBarStamp = -1.0f, mPs3ClockBarBaseY = 0;
    // One-shot analog-hand spin on a menu context change (submenu enter/leave or a
    // dialog open/close), mirroring the web drawClock. mPs3ClockSpinSig is the last
    // seen context signature; a change restarts the spin at mPs3ClockSpinStart.
    int    mPs3ClockSpinSig = -1;
    float  mPs3ClockSpinStart = -1.0e9f;
    // Flat-colour batch state (see beginSolidBatch). When true, drawQuad/
    // drawTriangle accumulate instead of drawing.
    bool   mSolidBatchActive = false;
    // Glass-icon frame-invariant uniforms (lights, ambient/spec/refraction, the
    // rotation matrix, sampler unit indices) are constants; upload them once per
    // frame on the first glass icon instead of ~14 glUniform calls per icon.
    // Reset to false at the top of renderPs3Xmb each frame.
    bool   mGlassUniformsSet = false;
    std::string mPs3DtDate;         // "YYYY/MM/DD" staged in the Set Manually wizard
    std::string mPs3DtTime;         // "HH:MM"
    // Dynamic text drop shadow, scaled by the wallpaper brightness each frame:
    // strength 0 (dark wallpaper, minimal shadow) .. 1 (light wallpaper, strong).
    float  mPs3ShadowStrength = 0.5f;
    float  mPs3ShadowAlpha = 0.4f;
    // Clock drop-shadow device-y sign, derived from the panel orientation
    // (sDrmRotMat[3]); -1 on the 180 panel. The menu text/icons instead use an
    // even 8-direction outline (drawTextStroke), which needs no direction.
    float  mPs3ShadowDir = -1.0f;
    void   openPs3Dialog(const Ps3Item& it);
    void   closePs3Dialog(bool apply);
    void   renderPs3Dialog();
    // Fullscreen dialog page helpers (1:1 with web drawDialog/drawDialogOption/
    // drawIllustration). Coordinates are device px; ap = open-anim alpha.
    void   ps3DlgNav(int dir, bool horizontal);   // chooser scroll / confirm Yes-No toggle
    void   ps3DlgText(const char* s, float cxDev, float baselineDev, float fs,
                      float r, float g, float b, float a, int align);  // align 0 left,1 centre,2 right
    void   ps3DlgOption(const char* label, float cxDev, float baselineDev,
                        bool sel, bool leftAlign, float ap, float baseScale,
                        bool translate = true);   // translate=false keeps native
                                                  // names verbatim (language list)
    TouchRect ps3DlgHint(float slotCxDev, bool cross, const char* label,   // returns where it drew
                         float yDev, float baseScale, float ap);
    // Like ps3DlgHint but selects the button glyph: 0 = cross (X)/Confirm,
    // 1 = ring (O)/Cancel, 2 = Start (pill + play arrow). ps3DlgHint forwards to
    // this. The Confirm/Cancel glyphs follow the user's Button Prompts theme.
    TouchRect ps3DlgHintG(float slotCxDev, int glyph, const char* label,
                          float yDev, float baseScale, float ap);
    // Draw one themed face-button badge centred on (gcx,yDev): a letter (A/B/X/Y)
    // in the default theme, or a PlayStation glyph (cross/ring/square/triangle)
    // in the PlayStation theme. role: 0 = Confirm, 1 = Cancel, 2 = Square,
    // 3 = Triangle. The OK/Cancel relabel swap flips the Confirm/Cancel display.
    // r/g/b tint the glyph (ring + letter, or the vector shape). Defaults to white so
    // existing callers are unchanged; the Minima accent-pill legends pass the pill's
    // contrast colour so the glyph stays legible on a light accent (matches its label).
    void   drawFaceGlyph(int role, float gcx, float yDev, float glyphR, float lw, float ap,
                         float r = 1.0f, float g = 1.0f, float b = 1.0f);
    // Re-read persist.gammaos.nano.face_glyphs / face_swap (cheap shared-mem read).
    void   refreshFaceButtonPrefs();
    // Theme an on-screen legend that uses the canonical PlayStation face-button
    // words (Cross/Circle/Square/Triangle) into the user's Button Prompts theme:
    // letters map Cross->A, Circle->B, Square->Y, Triangle->X (the OK/Cancel swap
    // flips Cross/Circle); the PlayStation theme keeps the names. Applied after
    // trDyn so the translated action words are preserved.
    std::string themeButtonText(const char* in);
    bool   mFaceLetters = true;   // Button Prompts: letters (default) vs PlayStation
    bool   mFaceSwapOk  = false;  // OK Button: false = A/Cross, true = B/Circle
    void   ps3DlgIllustration(int kind, float cx, float cy, float sz, float ap);
    void   ps3FillCircle(float cx, float cy, float rad, float r, float g, float b, float a);
    void   ps3StrokeRing(float cx, float cy, float radX, float radY, float lw,
                         float r, float g, float b, float a);
    void   ps3ThickLine(float x0, float y0, float x1, float y1, float w,
                        float r, float g, float b, float a);
    void   ps3VGradRect(float x, float y, float w, float h,
                        float r0, float g0, float b0, float r1, float g1, float b1, float a);
    void   previewThemeSetting(int themeKey, int sel);   // apply live (no persist)
    void   applyThemeSetting(int themeKey, int sel);     // persist + apply
    void   loadPs3ThemeSettings();
    std::string resolvePs3ItemValue(const Ps3Item& it);  // live theme value for a row, else it.value
    // Re-resolve the frozen it.value snapshot for every bound row in the open levels after a
    // settings change committed in place. The XMB carousel re-resolves live each frame, but the
    // DSi/Minima list draws the snapshot, so without this a changed value keeps showing the old
    // (default) value until you leave and re-enter the submenu.
    void refreshBoundValuesInStack();
    void openBoundChooser(const Ps3SettingBinding* b);   // side chooser for a settings-bound leaf
    std::string ps3BoundValue(const Ps3SettingBinding* b);  // cached current value for a binding
    // Dark STROKE behind text/icons instead of a single drop shadow: a left/right
    // pair plus one panel-DOWN copy, all in panel space (offsets rotated through
    // the orientation), so it reads the same on any panel rotation and is subtle.
    // Early-returns when a is tiny, so dark wallpapers (low mPs3ShadowAlpha) skip it.
    void   drawTextStroke(const char* s, float x, float y, float scale, float a);
    void   drawIconStroke(unsigned int tex, float x, float y, float w, float h, float a);
    std::vector<Ps3Cat> mPs3Cats;
    std::vector<Ps3Level> mPs3Stack;   // empty = at category top level
    int mPs3CatIdx = -1;
    int mPs3QuickCatIdx = -1;     // runtime index of the Quick Menu category (-1 if absent)
    int mPs3SettingsCatIdx = -1;  // runtime index of the Settings category (-1 if absent); used by the
                                  // "Manage Game System" shortcut to jump into Game Settings from Game
    std::string mPs3PerfModeLabel = "Normal";  // cached persist.gammaos.performance_mode label (Quick Menu row value)
    std::string mPs3SystemName;                // cached System Name (persist.gammaos.nano.system_name, else ro.product.model)
    int mPs3ItemIdx = 0;          // selection in the top-level item list (per-category)
    std::vector<int> mPs3CatItemSel;   // remembered item selection per category
    // Timed animation state mirroring the web's catAnim / itemAnim model so the
    // motion language matches: a category slide rail with a fade-crossfade, and
    // easeOutBack item navigation. Progress (0 = just started, 1 = settled) is
    // driven by mFrameDt; the input handlers START an animation, render()
    // interpolates it.
    bool  mPs3CatAnimActive = false;
    float mPs3CatT = 1.0f;           // category slide progress
    float mPs3CatFromOffset = 0.0f;  // virtual-px bar offset at t=0 (eases to 0)
    int   mPs3CatOldIdx = 0;         // category slid away from (for the fade-out rail)
    int   mPs3CatOldSel = 0;
    // Item scroll matches the web: each d-pad step restarts a 200ms easeOutCubic
    // from the CURRENT animated position to the new index (mPs3ItemAnimFrom +
    // start time), so holding (with the accelerating auto-repeat) reads as a
    // smooth, snappy, continuously-accelerating scroll. Snapped on category /
    // submenu changes (mPs3ItemAnimStart < 0) so it does not animate across lists.
    float mPs3AnimItem = 0.0f;
    float mPs3ItemAnimFrom = 0.0f;     // animated position when the step started
    float mPs3ItemAnimStart = -1.0f;   // mEffectTime at the step start (<0 = snap)
    int   mPs3DescLinesTarget = 3;     // active item's wrapped subtitle line count (1..4), set by drawDesc; drives the dynamic active pad
    float mPs3ActivePad = 118.0f;      // eased active-item pad (virtual px) -> ps3::gActivePad, grows to fit the subtitle's actual line count
    float mPs3SubAnim = 0.0f;        // 0 = top level, 1 = in submenu (collapse factor)
    int   mPs3SubDir = 0;            // +1 entering, -1 exiting
    // Timed submenu collapse animation (mirrors the web submenuAnim: 250ms
    // easeOutCubic). On enter/exit we snapshot the parent list + the entered
    // index + the child list so drawParentLayer / the child slide can run
    // continuously through the animation even while the live stack is changing.
    float mPs3SubAnimStart = -1.0f;            // mEffectTime at anim start (<0 = settled)
    std::vector<Ps3Item> mPs3SubParentItems;   // parent (breadcrumb) list snapshot
    int   mPs3SubParentIdx = 0;                // entered index in the parent list
    std::vector<Ps3Item> mPs3SubChildItems;    // child list snapshot (for the exit slide-out)
    GLuint mPs3CatTex[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // PS3 category icons (flat)
    GLuint mPs3CatNmap[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // PS3 category icons (glass nmap)

    // --- PSP Go slide clock (NanoMenuPS3Clock.cpp) -----------------------------
    // Full-screen procedural analog clock, a 1:1 port of the web xmb PSP clock
    // (/work/ps3/xmb-app psp_clock.js + index.html sections 5.x). Gated by
    // persist.gammaos.nano.pspclock; shown while KEY_F12 (the swivel) is DOWN,
    // exited on UP. Everything is a pure function of mPspClockReveal (0..1).
    bool  mPspClockEnabled = false;   // cached persist.gammaos.nano.pspclock
    bool  mPspClockOn = false;        // F12 down = true (open), up = false (close)
    // Standalone summon: the clock was invoked over a running app with the overlay DOWN
    // (the framework raised the overlay + set sys.gammaos.nano.pspclock_summon). There is
    // no XMB behind it, so the icon blow-away is suppressed and nano self-lowers the
    // overlay when the clock finishes retracting. mPspClockRaisedOverlay = we raised it.
    bool  mPspClockStandalone = false;
    bool  mPspClockRaisedOverlay = false;
    // Freeze App Under Clock (persist.gammaos.nano.pspclock.freezeapp): once a game frame is captured
    // for the over-app slide clock, SIGSTOP the game IN PLACE (it stays the foreground task - we do
    // NOT go HOME, so the launcher is never brought up, which is wasteful on constrained devices and
    // flashes on resume) and hold its last frame as the backdrop. SIGCONT it the moment the clock
    // starts closing so it is rendering again by the time the overlay is fully gone; the still is held
    // right up to that point, so no launcher or stale frame ever shows.
    bool  mPspAppFrozen = false;      // the game is currently SIGSTOP'd by us (drives the still-hold)
    bool  mPspAppResumeSent = false;  // SIGCONT already sent this summon (at close-start)
    // The freeze must only fire after a FRESH game frame is captured THIS summon - mPspClockAppTexValid
    // stays set across summons, so without this gate the second+ open would freeze on the previous
    // summon's stale still (and stop the capture before it could refresh). mPspClockAppFrameSeq bumps
    // on every new capture upload; snapshot it at each open edge and require it to advance first.
    uint32_t mPspClockAppFrameSeq = 0;
    uint32_t mPspAppFreezeBaseSeq = 0;
    bool  mPspClockPrevOn = false;    // mPspClockOn from last drawPspClock, to detect the open edge
    // PSP slide clock in a DSi/Minima HOME (no app): the surround + glass disc show the THEME home
    // backdrop (Minima black/solid/wallpaper, DSi field/wallpaper), not the XMB wave. Set for the frame
    // by drawPspClockThemeBackdrop(); tells pspClockLens/pspClockSampleGlow the work texture is display
    // sRGB (skip the wave's LINEAR tonemap), matching the captured-app path.
    bool  mPspClockThemeBackdrop = false;
    bool  mPspClockThemeBackdropLight = false;  // theme home backdrop fed to the disc is bright -> dim the disc so the additive-white clock face/date read
    // Swipe-to-dismiss: while the clock is up a touch swipe drags the WHOLE clock up with the
    // finger and, past a threshold, flings it off the top + dismisses - all WITHOUT touching the
    // rotation, so the user can exit the clock but keep the device rotated. Released short, it
    // springs back. Tracks the touch-down point + peak travel, the live drag, and the smoothed
    // spring/fling follower that is added to mPspLensCy so the clock moves as one rigid body.
    float mPspSwipeDownY = 0.0f;
    float mPspSwipeRawPx = 0.0f;      // live finger drag in device px (negative = up); 0 when not dragging
    float mPspSwipeUpPx = 0.0f;       // the last drag's upward travel, kept past the release for the swipe zone
    float mPspSwipeOffset = 0.0f;     // smoothed follower actually added to the clock Y (spring/fling)
    float mPspSwipeVel = 0.0f;        // px/frame velocity for the spring + fling
    bool  mPspSwipeDragging = false;  // finger down and driving the drag
    bool  mPspSwipeFling = false;     // released past threshold: coasting off the top + dismissing
    float mPspClockReveal = 0.0f;     // 0..1 transition progress (open 5000ms / close 2700ms)
    float mPspDescent = -1.0f;        // smoothed vertical fraction (-1 off-top, 0 rest)
    float mPspDetailFade = 0.0f;      // trail/ticks/ambient-glyph gate (in once settled)
    float mPspTextFadeSmooth = 1.0f;  // smoothed XMB text alpha multiplier
    float mPspFloatT = 0.0f;          // idle-float phase accumulator (ms)
    float mPspGlow[3] = {150.0f, 232.0f, 255.0f};  // dominant-bg glow colour (fallback cyan)
    bool  mPspGlowValid = false;
    uint32_t mPspIconSeed = 0;        // per-open entrance-icon layout seed
    int   mPspPresentBoosted = -1;    // force-SF swap interval while the clock is open: -1 unset, 0 boosted (interval 0), 1 restored
    float mPspGlyphBurst = 0.0f;      // ambient glyph speed/brightness kick
    float mPspTrail[120] = {0};       // second-hand comet-trail alphas
    GLuint mPspLensProgram = 0;       // radial-refraction glass shader (built lazily)
    GLint  mPspLensLocPos = -1, mPspLensLocLocal = -1, mPspLensLocTex = -1,
           mPspLensLocRot = -1, mPspLensLocHalf = -1, mPspLensLocCenter = -1,
           mPspLensLocTexture = -1, mPspLensLocTonemap = -1, mPspLensLocZoom = -1,
           mPspLensLocAlpha = -1, mPspLensLocTilt = -1, mPspLensLocAppSrc = -1;
    float  mPspLensCx = 0, mPspLensCy = 0, mPspLensR = 0;  // disc lens in device px
    bool   mPspLensValid = false;
    // Gyro/accel parallax: tilting the device shifts the background sampled through the
    // glass disc (peek behind). Smoothed device tilt in UV units (mPspTilt*), driven by
    // the accelerometer via the ASensor NDK (libandroid), lazily opened while the clock
    // is up. persist.gammaos.nano.pspclock.tilt="x,y" overrides the sensor for testing.
    float  mPspTiltX = 0.0f, mPspTiltY = 0.0f;   // smoothed, already scaled to UV offset
    ASensorManager*    mPspSensorMgr = nullptr;
    ASensorEventQueue* mPspSensorQueue = nullptr;
    const ASensor*     mPspAccelSensor = nullptr;
    bool   mPspSensorEnabled = false;
    GLuint mPspGlyphTex[4] = {0, 0, 0, 0};   // baked numeral alpha textures (12,3,6,9) - SHARP core
    GLuint mPspGlyphGlowTex[4] = {0, 0, 0, 0};  // pre-blurred soft-edged copies for the diffuse halo
    float  mPspGlyphHXu[4] = {0}, mPspGlyphHYu[4] = {0};  // half-extents in glyph units (incl pad)
    bool   mPspGlyphBaked = false;
    GLuint mPspGlowFbo = 0, mPspGlowTex = 0;   // 8x8 downsample of the wallpaper for the glow colour
    // Clock-chrome glow: the numerals/ticks/hands/hub shapes are rendered white into
    // this full-viewport FBO, then a real separable Gaussian (blurGlassChain) turns
    // them into a soft halo composited additively in the glow colour - the faithful
    // GLES2 equivalent of the web's canvas shadowBlur (was discrete expanding copies
    // that read as a hard stroke). Sized to the current viewport (panel-native).
    GLuint mPspChromeGlowFbo = 0, mPspChromeGlowTex = 0;
    int    mPspChromeGlowW = 0, mPspChromeGlowH = 0;
    // Static glow cache (perf): the NUMERAL glow coverage is frame-invariant (the 12/3/6/9
    // glyph textures at fixed disc-relative positions and size - only the whole clock's Y
    // BOBS via mPspLensCy). So bake its blurred halo ONCE and, on later frames, skip the
    // shape-render + Gaussian pyramid entirely and re-composite the cached texture shifted
    // by the live bob delta (mPspLensCy - baked). The glow COLOUR + pulse alpha are applied
    // fresh at composite each frame, so day/night + breathing stay live. Invalidated when the
    // disc size (mPspLensR) or the glow buffer resolution changes (resize/rotate/glowres). Slot
    // 0 = numerals; slot 1 reserved. See pspClockChromeGlowPass / pspClockSnapshotGlow.
    GLuint mPspGlowCacheTex[2] = {0, 0};
    GLuint mPspGlowCacheFbo[2] = {0, 0};
    int    mPspGlowCacheW[2] = {0, 0}, mPspGlowCacheH[2] = {0, 0};
    float  mPspGlowCacheR[2] = {0.0f, 0.0f};    // baked mPspLensR (disc size at bake time)
    float  mPspGlowCacheCx[2] = {0.0f, 0.0f};   // baked disc centre X (constant; guards resize)
    float  mPspGlowCacheCy[2] = {0.0f, 0.0f};   // baked disc centre Y (the bob anchor)
    bool   mPspGlowCacheValid[2] = {false, false};
    // Reduced-resolution glass-lens target (perf): the disc refraction magnifies an already
    // low-frequency, frosted background, so rendering the heavy lens shader into a fraction-res
    // FBO and upscaling it over the disc is imperceptible while cutting that fill by ~1/scale^2.
    // The FBO holds the disc's logical refraction axis-aligned; the composite carries the live
    // sDrmRotMat on the disc bbox exactly like the direct draw, so it is correct under every panel
    // rotation and the PRIME scanout flip. persist.gammaos.nano.pspclock.lensres = percent
    // (default 100 = direct draw; 50..99 = reduced-res FBO path).
    GLuint mPspLensRedFbo = 0, mPspLensRedTex = 0;
    int    mPspLensRedW = 0, mPspLensRedH = 0;
    // 2x-supersampled clock-FACE target: the crisp cores + trail (raw drawTriangle geometry)
    // render into this 2x offscreen texture and composite down through GL_LINEAR, box-filtering
    // their hard polygon edges into clean anti-aliased ones. The soft Gaussian glow stays at 1x
    // (needs no AA, and reuses blurGlassChain which must not thrash resolution between passes).
    GLuint mPspFaceFbo = 0, mPspFaceTex = 0;
    int    mPspFaceW = 0, mPspFaceH = 0;
    // --- PSP clock live-app capture (#5). GL-side, render-thread-only state. ---
    // The lens can sample the LIVE app behind the overlay scrim instead of the XMB
    // wave. A detached background worker (see NanoMenuPS3Clock.cpp) does all the
    // binder/capture/SW-lock work on file-static state; the render thread only
    // uploads the latest CPU frame into this texture. Gated OFF by default via
    // persist.gammaos.nano.pspclock.liveapp (strict no-op when unset).
    GLuint mPspClockAppTex   = 0;     // RGBA tex holding the latest captured app frame
    int    mPspClockAppTexW  = 0;     // dims currently allocated in mPspClockAppTex
    int    mPspClockAppTexH  = 0;
    bool   mPspClockAppTexValid = false;    // a frame has been uploaded at least once
    bool   mPspClockCaptureRunning = false; // worker started this open-cycle (render-thread bool)
    // Dynamic darkening of the live-app disc + surround by the app's mean brightness, so a
    // bright game does not wash out the clock face. Sampled ~7Hz (pspClockSampleAppDim), smoothed.
    float  mPspAppDim = 0.72f;              // disc dim factor (smaller when the game is brighter)
    float  mPspAppBackdropDark = 0.62f;     // surround darken over the blurred app
    GLuint mPspAppLumFbo = 0, mPspAppLumTex = 0;   // 8x8 downsample FBO for the app mean brightness
    // Glass-icon resources (FS_ICON_GLASS). Normal maps are cached by xmb_icon
    // index (PS3 icons -> nmap_NNN.png) and by flat-icon texture id (console /
    // RetroArch icons -> a bevel normal generated from the alpha silhouette).
    std::map<int, GLuint>    mPs3NmapByIcon;     // xmb_icon index -> nmap tex
    std::map<int, GLuint>    mPs3IconTexByIndex; // xmb_icon index -> colour icon tex (DSi flat cards)
    std::map<int, GLuint>    mPs3BevelByIconIdx; // console icon idx (0..17) -> bevel nmap
    std::map<int, GLuint>    mGpGlassNmaps;      // gamepad-tester button shapes -> bevel nmap (by round<<20|aspect)
    // iconRef string -> (colour silhouette tex, glass bevel nmap) for retroarch:/core:/file: refs.
    std::map<std::string, std::pair<GLuint, GLuint>> mPs3IconRefCache;
    // Real per-app icons: package name -> full-colour GL texture, decoded from the
    // DE cache /data/system/nano_app_icons/<pkg>.png written by SystemServer. Loaded
    // lazily when the Applications submenu is built; only successes are cached so a
    // not-yet-populated cache is retried on the next open.
    std::map<std::string, GLuint> mPs3AppIcons;
    // Icon-glass shader (distinct from the frosted-glass blur chain's mGlass*).
    GLuint mIconGlassProgram = 0;
    GLint  mIconGlassLocPos = -1, mIconGlassLocIconUV = -1, mIconGlassLocBgUV = -1, mIconGlassLocRot = -1;
    GLint  mIconGlassLocNormal = -1, mIconGlassLocAmb = -1, mIconGlassLocEnv = -1, mIconGlassLocBg = -1;
    GLint  mIconGlassLocLight1 = -1, mIconGlassLocLight2 = -1, mIconGlassLocAmbient = -1;
    GLint  mIconGlassLocSpec = -1, mIconGlassLocRefr = -1, mIconGlassLocRefrScl = -1;
    GLint  mIconGlassLocAttn = -1, mIconGlassLocChanging = -1, mIconGlassLocBgExp = -1;
    GLint  mIconGlassLocBgRad = -1;
    GLint  mIconGlassLocWpLift = -1;   // uWallpaperLift: opacity/lightness boost for glass icons in wallpaper mode
    GLuint mIconGlassAmbTex = 0;     // icon_amb.png 16x12 ambient ramp
    GLuint mIconGlassEnvTex = 0;     // texenv.png 64x64 silver matcap
    bool   mIconGlassReady = false;
    bool   mIconGlassTried = false;

    void initPs3Menu();
    void buildPs3Cats();
    // Rebuild the cats after a background rescan changed ROM lists, re-finding
    // each category's selected item by label (no selection yank). Driven by
    // mPs3CatsStale from the threadLoop scan pickup, applied at the XMB root.
    void rebuildPs3CatsPreserveSel();
    bool mPs3CatsStale = false;
    // Runtime item from a static DATA node. hidePrefix (when non-empty) is the compound-id path of
    // this item's PARENT ("<catId>/.../"), so the item's own compound id ("<hidePrefix><name>") can
    // be stored in payloadStr for the show/hide feature; empty leaves payloadStr untouched.
    Ps3Item makeDataItem(const Ps3DataItem* d, const std::string& hidePrefix = std::string());
    void buildDataSubmenu(const Ps3DataItem* node, Ps3Level& out, const std::string& hidePrefix = std::string());
    bool themeSettingRowVisible(const char* name) const;   // hide theme-irrelevant appearance rows per active theme
    void buildRomSubmenu(int sysIdx, Ps3Level& out);
    std::string openRomLevelSelectedPath(int sysIdx) const;   // highlighted ROM path of an open ROM level (empty if none)
    void rebuildOpenRomLevels(int sysIdx, const std::string& keepPath);   // rebuild open ROM levels in place, highlight kept on keepPath
    // Quick Menu (nano legacy global actions): the Power submenu builder, the
    // performance-mode side-panel chooser, and the kill-apps backend.
    void buildQuickPowerSubmenu(Ps3Level& out);
    void buildQuickMenuItems(std::vector<Ps3Item>& out);
    void buildQuickDisplaySubmenu(Ps3Level& out);
    void buildQuickDeviceSubmenu(Ps3Level& out);
    void buildQuickControllerSubmenu(Ps3Level& out);
    Ps3Item quickActionItem(const char* label, int action, int icon);
    void quickLeafItem(std::vector<Ps3Item>& out, const char* label, const char* bindLabel, int icon,
                       const char* spec, const char* desc = nullptr);
    bool quickAppBehind() const;
    void quickMenuRefresh();   // Kiosk Mode moved its row
    void openQuickPowerMenu();   // power-hold (menu visible) -> Quick Menu Power submenu, all themes
    void toggleQuickMenu();   // the Anbernic button -> Quick Menu, all themes
    bool quickMenuBackResumes() const;
    bool quickMenuOpen() const;   // the Quick Menu, or a screen opened from it, is showing
    // Where the Anbernic button opened the Quick Menu from, restored when it closes it again.
    struct QsReturn {
        bool valid = false;
        std::vector<Ps3Level> stack;
        int cat = 0, item = 0;
        int itemKind = 0;          // the row at item, to find it again if the category changed
        std::string itemLabel;
        int appCat = -1, appCatSel = 0;   // the category Applications was opened from, and its row
        bool atRoot = true;
        float camera = 0.0f;
    };
    QsReturn mQsReturn;
    QsReturn mAppsReturn;        // where openApplicationsView was called from
    bool mAppsSummoned = false;  // Anbernic + Start raised the overlay on it over an app: closing resumes the app
    void launchAndroidSettings(); // Quick Menu -> launch the device's own Settings app (runtime-resolved)
    // Overlay-only per-app Orientation submenu for the foreground app.
    void buildAppOrientSubmenu(Ps3Level& out);
    // USB device-mode submenu (Charging/MTP/PTP/RNDIS) for a connected PC.
    void buildUsbSubmenu(Ps3Level& out);
    void startWifiScanWizard();   // the network wizard's scan list, to pick a Wi-Fi network
    // The Wi-Fi chooser's connected-network row (its first row while connected, else -1) and its
    // signal bars; ps3DlgRowIcon gives that row its signal icon in every theme's chooser.
    int mPs3DlgWifiRow = -1, mPs3DlgWifiBars = 0;
    GLuint ps3DlgRowIcon(int row);
    // Bluetooth chooser: Off / On, then the connected devices (mPs3DlgBtDevs, from mPs3DlgBtBase),
    // then "Pair Device" (both only while the radio is on). ps3DlgBtSync rebuilds the device rows
    // when the bonded list refreshes.
    bool mPs3DlgBtRows = false;
    int mPs3DlgBtBase = 0, mPs3DlgBtGen = -1;
    std::vector<BtDevEntry> mPs3DlgBtDevs;
    void ps3DlgBtAppendRows();
    void ps3DlgBtSync();
    void btQuickOpen(int dev);
    // A device or Pair Device opened from the Quick Menu Bluetooth chooser: leaving it goes back to
    // the chooser (focused on that device, or Pair Device when the address is empty).
    bool mBtQuickReturn = false;
    std::string mBtQuickReturnAddr;
    void btQuickReturnTick();
    // Pairing (gammaos-net bt pair --ask) asked for a PIN (variant 0 or 7), the pass key the device
    // shows (1) or a code comparison (2, mBtWizAskKey): the prompt is up until it is answered (-1 = none).
    std::atomic<bool> mBtWizPairRunning{false};
    int mBtWizAskVariant = -1;
    std::string mBtWizAskKey;
    int mBtWizAskSeq = 0;             // the request on screen (gammaos-net numbers them)
    bool mBtWizResuming = false;      // entering Registering from a prompt: do not start a pairing
    void btWizCloseOsk();
    void btWizPairReply(const std::string& reply);
    void buildGamepadSubmenu(Ps3Level& out);         // top-level Gamepad Settings section list
    // Gamepad Settings sub-sections (grouped for readability) + shared row helpers.
    bool gpLeaf(Ps3Level& out, const char* label, const char* bindLabel, int icon);   // false: not added
    void gpAct (Ps3Level& out, const char* label, int qa, int icon, const char* val);
    void buildGpControllers(Ps3Level& out);
    void buildGpSticks(Ps3Level& out);
    void buildGpButtons(Ps3Level& out);
    void buildGpCalTest(Ps3Level& out);
    void buildGpRumble(Ps3Level& out);
    void buildGpMapping(Ps3Level& out);
    void buildGpTouch(Ps3Level& out);
    void buildGpHotkeys(Ps3Level& out);
    void buildGpHotkeySupport(Ps3Level& out);
    void hotkeyTakeButton(const Ps3SettingBinding* b, const std::string& v);
    // The button map (NanoMenuButtonMap.cpp, button_map_overlay_plan.md): Anbernic + the bottom button,
    // and Start + Select + the bottom button in the menus, show how the pad maps onto the console.
    std::shared_ptr<NanoBmapUi> mBmap;
    bool bmapActive() const;
    bmap::MenuFocus bmapMenuFocus();                 // what has focus, in any theme
    bool bmapOverAppOnly() const;                    // the overlay is up for the map alone (qs_summon=3)
    void bmapOpenOverApp();
    void bmapClose(bool resumeApp = true);           // over an app the map alone was up: resume it
    bool bmapChordKey(int fd, int code, int value);  // the chords in the menus; true = taken
    bool screenChordKey(int fd, int code);           // the Switch Screen Button on this pad
    bool bmapKey(int fd, int type, int code, int value);   // while open: every key; true = taken
    int padModelCode(int fd, int code) const;
    void bmapTouchFrame();
    void bmapTick();
    void renderButtonMap();
    void bmapMigrateSetting();                       // Button Map and Switch Screen Buttons off a hotkey's button, at start
    void gamepadTestOpen();                          // enter the live controller test screen
    void gamepadCalibOpen();                         // enter the analog calibration wizard
    void gpCaptureEvent(int fd, int type, int code, int value); // mirror a raw event -> maps
    bool gpScreenHandleKey(int code, int value);     // Test/Calib key handling (exit, wizard step)
    void renderGamepadTest();                        // draw the live controller test screen
    void renderGamepadCalib();                       // draw the calibration wizard
    void gpDialogBackdrop(float ap);                 // frosted-wave + dim, System-Update dialog chrome
    void gpDialogHeader(const char* title, int iconIdx, float ap); // icon + title + top/bottom dividers
    float gpAxisNorm(int absCode);                   // latest axis value normalised to [-1,1]
    // Full-screen HSV colour picker (all themes) - see the mCp* state block above.
    void colorPickerOpen(const Ps3SettingBinding* b, const std::string& curHex, int applyMode = 0); // enter the picker (mode 0 = RGB LED, 1 = theme accent, 2 = Background Colour)
    void renderColorPicker();                        // draw the Hue/Brightness field + preview + hints
    bool colorPickerHandleKey(int code, int value);  // confirm / cancel / saturation / hold-exit (press edges)
    void colorPickerTick();                          // per-frame cursor move from held d-pad / stick / hat
    void colorPickerApply();                         // write the chosen colour (RGB LED or theme accent)
    void applyRgbSolidColor(const std::string& hex, const Ps3SettingBinding* b); // shared vivid solid-colour apply
    void applyThemeAccentColor(const std::string& hex); // commit a "Custom..." theme accent (all themes) + persist + live-apply
    bool customAccentRGB(float& r, float& g, float& b) const; // parse mPs3ColorCustomHex -> 0..1 RGB; false if unset/bad
    std::string customAccentHex() const;             // the stored custom hex (or a sensible default) to seed the picker
    void gpCalibTick();                              // per-frame min/max capture for range steps
    void gpCalibNext(int dir);                       // advance/adjust the wizard (A / left / right)
    void gpCalibSave();                              // write cal_axis props + bump config_version
    void buildLaunchTargetAppSubmenu(Ps3Level& out);                              // Slide Launch Target step 1: pick an app
    void buildLaunchTargetActivitySubmenu(Ps3Level& out, const std::string& pkg); // Slide Launch Target step 2: pick an activity
    void buildMouseSubmenu(Ps3Level& out);           // Mouse Mode cursor-speed settings
    void buildRemapSrcSubmenu(Ps3Level& out, bool axis);  // button/axis remap source list
    void buildRemapTargetSubmenu(Ps3Level& out);          // target chooser for mRemapSrc
    void buildDevicesSubmenu(Ps3Level& out);              // capture-device multi-select
    void buildFfDeviceSubmenu(Ps3Level& out);             // vibration-device single-select
    void screenCalSliderPreview();                        // Screen Calibration: live slider preview
    void buildSlideDeviceSubmenu(Ps3Level& out);          // Slide Behaviour: trigger-device single-select
    void buildSlideEventSubmenu(Ps3Level& out);           // Slide Behaviour: trigger event/code single-select
    void buildDefaultBrowserSubmenu(Ps3Level& out);       // "Default Browser" single-select picker
    void buildBootToSubmenu(Ps3Level& out);               // "Startup Menu" picker: Default / a category / a game system
    bool applyBootToTarget();                             // navigate to the saved startup target on a fresh build; true if it did
    void buildBlacklistSubmenu(Ps3Level& out);            // passthrough-blacklist button multi-select
    void buildSlideActionSubmenu(Ps3Level& out, bool up); // Slide Behaviour: multi-select of slide-down/up actions
    void buildComboSubmenu(Ps3Level& out);                // combo_map list editor (add-flow state machine)
    void buildAxisBtnSubmenu(Ps3Level& out);              // axis_btn list editor
    int mComboStage = 0; int mComboB1 = 0; int mComboB2 = 0;   // combo add-flow state
    int mAxbStage = 0;   int mAxbAxis = 0; int mAxbBtn = 0;    // axis_btn add-flow state
    std::string mRemapKey;        // remap prop being edited (remap_btn / remap_axis)
    bool mRemapAxis = false;      // axis (vs button) name table for the active remap picker
    int  mRemapSrc = 0;           // source code chosen, awaiting a target pick

    // --- Custom button-action editor (short/long press -> key/app/activity/prop/shell) ---
    // A polished capture-based flow: press the button to map, then bind its short
    // and long press to an action. Writes act_count/actN_* (global) or paN_act*
    // (per-app) which the gammapad daemon consumes.
    void buildActionMenu(Ps3Level& out);          // rule list for the active scope + Add
    void buildActionEdit(Ps3Level& out);          // short/long/hold/remove for mActionEditCode
    void buildActionTypeMenu(Ps3Level& out);      // action-type chooser for the active slot
    void buildActionKeyList(Ps3Level& out);       // pick a key/button target from the catalog
    void buildActionAppList(Ps3Level& out);       // pick an app to launch
    void buildActionActivityList(Ps3Level& out);  // pick an activity to launch
    void buildActionPerAppMenu(Ps3Level& out);    // pick an app for per-app scope
    void actionCaptureOpen(int purpose);          // full-screen live "press a button" capture
    void renderGamepadCapture();                  // draw the capture prompt screen
    void renderGamepadCaptureNds(float rx, float ry, float rw, float rh);  // DSi-themed capture chrome
    bool gpCaptureHandleKey(int code, int value); // latch the first press during capture
    void popToActionEdit();                       // pop pickers back to the edit level + rebuild
    std::string actScopePrefix();                 // prop prefix for the active scope
    int  actProfileIndexForPkg(const std::string& pkg, bool create);
    void actReadRule(int code, int& hold, std::string& s, std::string& l);
    void actSetSlot(int code, int slot, const std::string& spec);  // slot 0=short 1=long
    void actSetHold(int code, int hold);
    void actRemove(int code);
    std::string actionSummary(const std::string& spec);   // human label for a "type=arg"
    std::string actButtonName(int code);                  // human name for a source/target code
    std::string actionAppLabel(const std::string& pkg);   // app label from mAppEntries
    int  mActionScopePa = -1;        // per-app profile index, -1 = global scope
    std::string mActionScopePkg;     // package for per-app scope ("" = global)
    int  mActionEditCode = 0;        // source code being edited
    int  mActionEditSlot = 0;        // 0 = short, 1 = long
    bool mGpCaptureActive = false;   // full-screen press-to-capture up
    int  mGpCapturePurpose = 0;      // 0 = source button, 1 = key target
    long mGpCaptureOpenMs = 0;
    std::string mActionListFilter;   // live text filter for the target pickers
    int  mActionListKind = 0;        // active picker: 0=key, 1=app, 2=activity
    bool actionFilterMatch(const std::string& label);  // case-insensitive substring
    void rebuildActionPicker();      // rebuild the active picker in place with the filter
    void actAddSearchRow(Ps3Level& out, GLuint ic, GLuint nm);  // Search/Clear rows for a picker
    void buildNotificationsSubmenu(Ps3Level& out);   // re-reads the live list, then builds
    void buildNotificationsLevel(Ps3Level& out);     // builds rows from the current mNotifs (no read)
    // One active notification, parsed from `dumpsys notification --noredact` only
    // while the Notifications submenu is open (mNotifs is cleared on close).
    struct NanoNotif { std::string key, pkg, title, text; };
    std::vector<NanoNotif> mNotifs;
    void readNotifications(std::vector<NanoNotif>& out);
    // Secondary (external) display on/off, in-memory like the QS tile (default on,
    // not persisted; the real state lives in DisplayManagerService).
    bool mSecondaryDisplayOn = true;
    void openPerformanceChooser();
    // Generic On/Off side chooser (mirrors openPerformanceChooser) so an in-place toggle (Quick
    // Resume, External Display) instead presents the same DSi list dialog with the current state
    // highlighted; applyThemeSetting(themeKey, sel) commits it (sel 0 = On, 1 = Off).
    void openOnOffChooser(const char* title, int iconIdx, bool currentOn, int themeKey);
    // ---- GammaShader (display post-process shader control) ----------------------
    // Mirrors the ShaderControl app, driving the persist.gammaos.shader.* props and,
    // for custom presets, the /data/media/0/GammaShader/.shader_param_meta (native ->
    // us) + .shader_params (us -> native) file contract. Everything is chooser/slider
    // driven - the user never types text.
    struct ShaderParam {
        std::string id;      // custom: param id (in .shader_params); builtin: prop suffix after persist.gammaos.shader.
        std::string label;   // display label
        float mn = 0, mx = 1, step = 0.01f, cur = 0, def = 0;
        int   dec = 2;       // decimals for display
        bool  isFile = false;// true = custom param written to .shader_params; false = a persist.gammaos.shader.<id> prop
    };
    std::vector<ShaderParam> mShaderParams;   // active shader's parameters (rebuilt on open / shader change)
    int    mShaderParamEdit = -1;             // mShaderParams index of the slider currently open, else -1
    float  mShaderParamOrig = 0.0f;           // value at open, for revert on cancel
    std::string mShaderParamsPreset;          // custom preset path the loaded meta belongs to
    long   mShaderMetaDeadlineMs = 0;         // poll .shader_param_meta until this uptime (after a preset/type change)
    std::string mShaderOptKey;                // discrete-option chooser: the prop key being edited
    std::vector<std::pair<std::string,std::string>> mShaderOptVals;  // (value,label) for the open opt chooser
    int    mShaderPendingTypeSel = -1;        // kShaderTypes index awaiting the custom-shader disclaimer accept
    bool   mShaderDisclaimerArm = false;      // open the custom-shader disclaimer dialog on the next frame
    void   openShaderDisclaimer();            // confirm dialog shown before switching to a custom shader
    void shaderApplyParamLive(int idx);       // write mShaderParams[idx] live (prop or .shader_params)
    void shaderResetActive();                 // reset the active shader's params to defaults
    void buildShaderSubmenu(Ps3Level& out);          // top-level GammaShader submenu (dynamic by type)
    void buildExternalDisplaySubmenu(Ps3Level& out); // Docked / Dual Screen / Mirror (the active one starred), the monitor's rows
    void externalDisplayTick();                      // per-frame: rebuild the open External Display list after an outside change
    std::string externalDisplayMode();               // docked / dual / mirror
    long mExtDisplayCheckMs = 0;
    std::string mExtDisplaySig;                      // what the open External Display list was built from
    // External Display > Output Resolution, Refresh Rate and Forget Monitor Resolutions
    // (NanoMenuMonitorMode.cpp). One `cmd display gammaos-monitor-mode` call at a time runs on a
    // thread of its own; monitorModeTick takes its result on the render thread. Theme keys: 68 the
    // Output Resolution chooser, 69 the Refresh Rate chooser, 70 the Keep This Resolution? dialog.
    enum MonModeCall { MMC_NONE, MMC_LIST_SIZES, MMC_LIST_RATES, MMC_TRY, MMC_KEEP, MMC_REVERT, MMC_AUTO, MMC_FORGET };
    void monitorModeRows(Ps3Level& out, int qa, bool forget);   // forget: Forget Monitor Resolutions, else the two mode rows (a monitor listed)
    std::string monitorModeSig();                    // what those rows show: the held mode, and whether a game runs
    bool monitorModeLocked() const;                  // a game runs: the mode is not changed under it
    void monitorModeOpen(int row);                   // a row chosen: 0 Output Resolution, 1 Refresh Rate, 2 Forget
    bool monitorModeCall(int call, const std::string& args);   // false while another call runs
    void monitorModeTick();                          // per-frame: a call's result, the dialog's countdown
    void monitorModeResult(int call, bool ok, const std::string& out);
    void monitorModeChooser(int call, const monmode::List& list);   // the side panel (theme key 68 or 69)
    void monitorModeChosen(int themeKey, int sel);   // a chooser's pick: Auto, or a mode to try
    void monitorModeAsk(const monmode::Trial& trial);   // Keep This Resolution? (theme key 70)
    void monitorModeAnswer(bool keep);               // its answer; B, a close and the timeout revert
    void monitorModeRevert();
    void monitorModeBody(long nowMs);                // its text, with the seconds left
    bool monitorModeDlgGuard();                      // its first moments: input ignored, the cursor on Revert
    std::mutex mMonModeMutex;
    int mMonModeCall = MMC_NONE;                     // the call running (render thread only)
    bool mMonModeDone = false;                       // its result is in (under mMonModeMutex)
    bool mMonModeOk = false;                         // it exited 0 (under mMonModeMutex)
    std::string mMonModeOut;                         // what it printed (under mMonModeMutex)
    bool mMonModeRevertDue = false;                  // a revert asked for while another call ran
    std::vector<monmode::Mode> mMonModeChoices;      // the open chooser's modes, after its Auto row
    std::string mMonModeHeldWhy;                     // why the monitor ran at the mode it had when the chooser opened
    monmode::Mode mMonModeHeld;                      // that mode
    monmode::Mode mMonModeTried;                     // the mode the running try asks for
    bool mMonTrialOpen = false;                      // the Keep This Resolution? dialog is up
    long mMonTrialShownMs = 0, mMonTrialEndMs = 0;
    std::string mMonTrialNow, mMonTrialBack;         // its two modes, as the dialog says them
    void buildShaderParamsSubmenu(Ps3Level& out);    // the active shader's parameter rows
    void buildShaderBrowser(const std::string& path, Ps3Level& out);  // custom-preset file browser
    void shaderOpenBrowserDefault();                  // open the browser at the RetroArch subfolder for the active type (still traversable up)
    void shaderSelectPreset(const std::string& path);                 // point the shader loader at a custom preset (SF reads it in place) + arm meta poll
    void shaderChooseType(int sel);                  // pick a shader type (custom ones ask first)
    void openShaderOptChooser(const std::string& spec); // discrete-option chooser (key|opts|title)
    std::string shaderOptCurrentLabel(const std::string& spec); // display label of a shader-opt row's current value
    void openShaderParamSlider(int idx);             // live slider for mShaderParams[idx]
    void shaderApplyType(int sel);                   // commit a shader-type selection (enable+type) + rebuild
    void shaderLoadParams();                          // populate mShaderParams for the active shader
    void loadShaderParamMeta();                       // read .shader_param_meta (+ overlay .shader_params) into mShaderParams
    void writeShaderParams();                         // rewrite .shader_params from mShaderParams (custom)
    std::string shaderCurType();                      // active shader type ("" when disabled)
    bool isCustomShaderType(const std::string& t);    // true for custom/custom-vk/custom-gl
    std::string shaderTypeLabel();                    // friendly label of the active shader for the menu row
    void shaderRebuildOpenLevel();                    // rebuild the open GammaShader/Params level in place (keep cursor)
    void shaderMetaTick();                            // per-frame: pick up custom .shader_param_meta once SF publishes it
    void quickKillApps(bool includeForeground);
    void overlayKillAll();   // Quick Menu Kill All Apps (overlay): hard-stop every app incl the game, no relaunch
    void buildRecentSubmenu(Ps3Level& out);
    void buildAppSubmenu(Ps3Level& out);
    void buildPinnedAppsSubmenu(Ps3Level& out);            // the pinned-apps list (resolved PS3_APP rows)
    void buildCollectionsSubmenu(Ps3Level& out);            // the list of collections + New Collection...
    void buildCollectionSubmenu(int colIdx, Ps3Level& out); // one collection's games (resolved PS3_ROM rows)
    void buildFavoritesSubmenu(Ps3Level& out);              // the global favourites list (resolved PS3_ROM rows)
    void buildHiddenGamesSubmenu(Ps3Level& out);            // Game Settings > Hidden Games (PS3_HIDDEN_GAME rows)
    // Game Systems editor (dynamic systems config). The list screen shows every
    // configured system (enabled + disabled) with enable/disable + reorder; later
    // phases add the per-system editor, folder picker, and icon grid.
    void buildCatOrderList(Ps3Level& out);      // the "Home Categories" editor screen (declared here where Ps3Level is defined)
    void buildCatItemVisibilityList(const std::string& catId, Ps3Level& out);   // one category's static submenu rows, each Shown/Hidden (ITEM_HIDE screen)
    void buildGameSystemsList(Ps3Level& out);
    void gsToggleSystem(int sysIdx);            // flip enabled, persist, rebuild
    void gsReorderSystem(int sysIdx, int dir);  // move a system up (-1) / down (+1)
    void loadRomCacheForSystem(XmbSystem& sys); // reload a system's cached ROM list (DE)
    // Per-system editor: name/exts/tint/launch type+core+package+args.
    void buildGameSystemEditor(int sysIdx, Ps3Level& out);
    void gsEditField(int field);                // A on an editor field row -> open OSK / chooser
    void gsRefreshStackLevels();                // rebuild any GS list/editor levels on the stack after an edit
    void gsOpenLaunchTypeChooser();             // side-panel chooser (theme key 20)
    void gsOpenTintChooser();                   // colour-swatch chooser (theme key 21)
    void gsOpenResetConfirm();                  // Cancel / Reset-to-default chooser (theme key 22)
    void gsOpenScraperChooser();                // per-system scraper override (theme key 24)
    void gsEditScraperCred(bool masked);        // per-system scraper credential override via OSK
    bool resetSystemToBuiltinDefaults(int sysIdx);  // restore a built-in's config from kXmbSystemDefs
    int mGsEditIdx = -1;   // mXmbSystems index currently open in the editor (for chooser/OSK writeback)
    int mGsRemoveSrcIdx = -1;  // scan-source index awaiting a remove-confirm (dialog theme key 45)
    std::string mGsAddFolderPath;   // folder awaiting This Folder Only / Include Subfolders (key 61)
    void gsFolderAdd(const std::string& path, bool subfolders);
    float mGsTintOrigR = 1.0f, mGsTintOrigG = 1.0f, mGsTintOrigB = 1.0f;  // exact tint at chooser open (cancel restore)
    int  ps3TopScreenKind() const { return mPs3Stack.empty() ? 0 : mPs3Stack.back().screenKind; }

    // ---- Icon grid picker: the RetroArch 849-icon chooser ----
    std::vector<std::string> mIconGridNames;   // all icon names (no .png), loaded once
    std::vector<int>         mIconGridFiltered;// indices into mIconGridNames matching the filter
    std::string              mIconGridFilter;  // current OSK substring filter (lowercased)
    int    mIconGridCursor = 0;                // index into mIconGridFiltered
    int    mIconGridTop = 0;                   // first visible row (scroll)
    float  mIconGridAnim = 0.0f;               // open fade-in 0->1
    std::map<int, GLuint> mIconGridThumb;      // nameIdx -> glass bevel nmap (downscaled 64x64)
    std::vector<int>      mIconGridLru;        // LRU order of cached nameIdx (evict past the cap)
    void   loadIconGridNames();                // enumerate the bundled 849-icon set once
    void   applyIconGridFilter();              // rebuild mIconGridFiltered from mIconGridFilter
    void   openIconGridPicker();               // push the grid screen for mGsEditIdx
    void   closeIconGridPicker();              // free thumbnails + grid nav state
    GLuint iconGridThumb(int nameIdx);         // get/generate the cached glass bevel for a name
    void   renderIconGridPicker();             // draw the grid (called from renderPs3Xmb)
    void   iconGridNav(int dx, int dy);        // 2D cursor movement
    void   iconGridSelect();                   // assign the highlighted icon to the system
    void   iconGridResetCache();               // drop all thumbnail textures
    void   glassScratchFree();                 // release the half-res glass icon scratch FBO (parked overlay)
    void   gsOpenIconFilePicker();             // open the file browser to import a custom PNG icon (folder-picker target 6)
    void   gsIconFileSelect(const std::string& path); // copy the chosen image into nano_user_icons and set a file: iconRef

    // ---- Emulator catalog (Daijishou platform configs, NanoMenuPS3EmuCatalog.cpp) ----
    struct EmuCatEntry {
        std::string platform;       // platform display name ("Sony - PlayStation 2")
        std::string platformId;     // platform uniqueId ("ps2") -> conventional rom folder
        std::string player;         // player display name ("Aethersx2")
        std::string amArgs;         // amStartArguments (whitespace-normalized)
        std::string playerRegex;    // player acceptedFilenameRegex
        std::string platformRegex;  // platform acceptedFilenameRegex
    };
    std::vector<EmuCatEntry> mEmuCatalog;      // all players across all bundled platforms (loaded once)
    std::string mEmuPickFilter;                // current emulator-picker substring filter
    void loadEmuCatalog();                     // parse the bundled Daijishou JSONs once
    void buildEmulatorPicker(Ps3Level& out);   // build the filtered emulator list screen
    void gsOpenEmulatorPicker();               // open the emulator chooser for mGsEditIdx
    std::string emuPlatformFor(const XmbSystem& sys);   // the catalog platform a system plays ("" = none)
    std::vector<int> emuPlatformChoices(const std::string& uid, bool& noneReady);   // its runnable emulators
    std::vector<nanosys::Player> emuPlatformPlayers(const std::string& uid, std::vector<int>* catIdx);
    bool emuEntryUsable(const XmbSystem& launch, const std::set<std::string>& installed);
    std::string emuEntryLabel(int catIdx) const;   // a catalog emulator's picker name
    void emuPickerSearch();                    // the picker's search OSK
    void emuPickerOpenPlatform(const std::string& uid);   // add-system picker: a platform's emulators
    void applyEmulatorChoice(int catIdx);      // apply a catalog entry (edit, or add a new system)
    void applyEmuEntryToSystem(XmbSystem& s, const EmuCatEntry& e);  // set launch fields from a catalog entry
    std::string gsIconRefForPlatform(const std::string& platformId, const std::string& platformDisplay);
    bool mGsAddMode = false;                    // emulator picker opened to create a NEW system
    // Game Options > Emulator: games that open with another emulator than their system's, keyed by
    // the game's path (NanoMenuPS3EmuCatalog.cpp). label is the picker row, amArgs the catalog entry.
    struct GameEmulator { std::string label; std::string amArgs; };
    static std::string gameEmuKey(const std::string& romPath);   // a game's key in mGameEmus
    std::map<std::string, GameEmulator> mGameEmus;
    bool mGameEmusLoaded = false, mGameEmusLoadErr = false;
    void loadGameEmulators();
    void saveGameEmulators();
    const GameEmulator* gameEmulatorFor(const std::string& romPath);
    void setGameEmulator(const std::string& romPath, const GameEmulator* emu);   // nullptr = the system's
    bool applyGameEmulator(const std::string& romPath, XmbSystem& s);   // give s the game's emulator, if set
    int recentSystemIndex(const XmbRecentEntry& re) const;   // the system a recent entry belongs to, -1 if none
    void openGameEmulatorPicker(int sysIdx, const std::string& romPath);
    std::string mGameEmuRom;                    // the game the emulator picker is open for
    // Game Options (the ES-DE gamelist's Select): one game's settings as a menu level.
    void openGameOptions(int sysIdx, int romIdx);
    void buildGameOptions(Ps3Level& out);
    int mGameOptSys = -1, mGameOptRom = -1;     // the game Game Options is open for
    std::string mGameOptPath;                   // its path, which finds it again after a rename re-sorts its list
    bool gameOptResolve();                      // re-point mGameOptRom at mGameOptPath; false once it is gone
    static bool isGameOptionsLevel(const Ps3Level& lvl);
    void gsearchLeaveGameOptions();
    void esdeToggleFavoriteRom(const std::string& rom, int sysIdx = -1);
    std::unordered_set<int> mEsdeGamelistParsed;   // systems whose ES-DE gamelist has been read once
    bool esdeOpenGameOptions();                 // the focused gamelist game's Game Options
    void esdeOpenGameSettings(int page);        // ES-DE start menu: 0 = focused platform, 1 = game systems, 2 = scraper
    std::string esdeMenuPlatformLabel();

    // ---- Add / remove custom systems ----
    void gsAddSystem();                        // "Add New System" -> emulator picker in add mode
    void gsAddBlankSystem();                   // create a blank custom system + open its editor
    // ES-DE-style bulk import: scan the chosen ROMs root's immediate subfolders, match each name to
    // an emulator-catalog platform (via the folder-alias table), and add every match that actually
    // holds a ROM as a system + scan source in one pass. Shows an "Added N systems" summary.
    // The blocking folder probe runs on a detached worker (a slow NAS/FTP root must never freeze the
    // render thread and trip the watchdog); the render thread applies the results in gsAutoAddTick().
    // quiet: after an ES-DE move, no messages unless a system is added, existing systems' folders kept.
    void gsAutoAddFromRoot(const std::string& root, bool quiet = false);
    void gsAutoAddTick();                       // render thread: apply the worker's matched folders
    struct BulkAddCand { std::string folder; int catIdx = -1; };
    std::mutex               mBulkAddLock;
    std::vector<BulkAddCand> mBulkAddResults;   // worker output (guarded by mBulkAddLock)
    std::atomic<bool>        mBulkAddScanning{false};  // a scan is in flight (blocks re-entry, shows UI)
    std::atomic<bool>        mBulkAddDone{false};      // worker finished; results ready to apply
    bool                     mBulkAddQuiet = false;    // this scan is gsAutoAddFromRoot's quiet one
    std::string              mBulkAddNextRoot;         // a folder the user picked during a quiet scan, run after it
    std::string              mBulkAddNextQuietRoot;    // a quiet card scan asked for during the user's scan, run after it
    void gsRemoveSystem(int sysIdx);           // remove a custom system + its caches
    void gsOpenRemoveConfirm(int sysIdx);      // Cancel / Remove confirm chooser

    // ---- Native raw-path folder picker for scan sources ----
    std::string mGsFolderPath;                 // current folder-browser path ("" = storage roots)
    void buildScanFoldersScreen(Ps3Level& out);// the system's scan-source list (+ Add Folder)
    void gsOpenScanFolders();                  // open the scan-folders screen for mGsEditIdx
    void buildFolderBrowser(const std::string& path, Ps3Level& out);  // raw-path browser
    // Names of the live /mnt/shares/<n> mounts. const because it only reads the kernel mount table,
    // which lets the const media-root scanners call it to filter shares out of the /storage listing:
    // shares are bind-mounted there so apps can open them by path, so they would otherwise be
    // enumerated a second time and shown as memory cards.
    std::vector<std::string> mountedShareNames() const;
    void gsFolderSelect(const std::string& path); // add a folder as a rawpath scan source
    void drasticDataFolderSelect(const std::string& path); // #90: set persist.gammaos.drastic.data_dir (folder-picker target 5)
    void esdeMediaFolderSelect(const std::string& path);   // persist.gammaos.nano.esde.mediadir (folder-picker target 10)
    void esdeMediaFolderApply(const std::string& p);       // switch the media folder now
    std::string mPendingEsdeMediaDir;                      // picked folder awaiting the copy question ("@default" = default)
    // RetroArch's config and every folder of the user's own setup in one folder
    // (persist.gammaos.retroarch.dir, "" = RetroArch's defaults), set by Move Emulator Files.
    void retroarchFolderFinish();
    bool retroarchFolderRewrite();                         // worker: point RetroArch's configs at mFileJobTo
    static std::string retroarchConfigFile();              // the retroarch.cfg launches pass to RetroArch (NanoRaConfig.h)
    static void retroarchConfigWatchStart();               // publishes it for RootWindowContainer
    // Points RetroArch's configs at its folder on the card again where a session that could not
    // read them saved its internal defaults (worker thread): once a boot, after a move until a
    // RetroArch session left them right, and (force) whenever asked.
    static void retroarchHealStart(bool force);
    static bool esdeStoragePath(const std::string& p);   // user storage (/storage, /mnt/media_rw, ...), no ".."
    static std::string rawStoragePath(const std::string& p);   // /storage path -> its backing path (/data/media/0, /mnt/media_rw)
    static std::string retroarchDirRaw();                  // the RetroArch Folder's backing path ("" = default)
    // A folder copy (files already at the destination are kept) on a worker behind the scraper's
    // progress modal; fileJobTick finishes it on the render thread. Kind 1 = ES-DE media folder,
    // 2 = RetroArch folder. The engine is NanoFileJob.cpp (its Pair says what each field does).
    using FileCopyPair = nanofilejob::Pair;
    using FileConflict = nanofilejob::Conflict;
    std::vector<FileConflict> mFileJobConflicts;           // the last move's (render thread once it ended)
    std::string mFileJobBackupRoot;                        // where they are
    std::vector<int> mFileJobFailedGroups;                 // groups whose relocation failed (kept as they were)
    std::vector<std::string> mFileJobStopPkgs;             // closed before the job's files are copied
    bool mFileJobCommitted = false;                        // a move past its copy: no cancelling it now (under mScrapeMutex)
    bool mFileJobIncomplete = false;   // a move whose backup or deletion stopped part way (under mScrapeMutex)
    bool fileJobRelocate(int group);                       // worker: point a group's program at the copy
    // Move ES-DE / Emulator Settings to microSD (NanoSdMove.cpp).
    static std::string sdCardRoot();
    void esdeMoveOffer();
    void esdeMoveStart();
    void esdeAddCardSystems();                 // systems for the ES-DE ROM folder's system folders Nano has none for
    void emuMoveOffer();
    void emuMoveStart();
    void sdMoveFinish(int kind, bool cancelled, int failed);
    void sdMoveConflictDialog(int kind, const std::string& problem);   // the clashes of a move, and what to do with them
    void sdMoveConflictChoice(int sel);
    void openConfirmDialog(int key, const char* title, const std::string& body, const char* yes);
    void retroarchMovePairs(const std::string& to, const std::string& backup, int group, std::vector<FileCopyPair>* pairs);
    std::string mSdMoveRoot, mSdMoveCard, mSdMoveOldScrapeDir;
    std::vector<std::string> mSdMoveUnmapped;              // data of games in no system folder
    std::map<std::string, std::string> mSdMoveEmuDirs;     // emulator -> its new folder
    int mSdMoveConflictKind = 0;
    bool mSdMoveOverwriteEsde = false, mSdMoveOverwriteRa = false, mSdMoveRaIncluded = false;
    int  mFileJobKind = 0;
    std::string mFileJobFrom, mFileJobTo;                  // the folder setting's old and new value
    bool fileCopyStart(int kind, const char* title, std::vector<FileCopyPair> pairs);
    void fileCopyThreadFunc(std::vector<FileCopyPair> pairs);
    void fileJobTick();
    // BIOS installer (NanoBiosInstall.cpp): the files the emulators use (NanoBiosCatalog.cpp), found
    // by content in a chosen folder and copied where each emulator reads them, never over a file.
    // Strings in the rows are English keys (trDyn them); report lines come translated.
    struct BiosDialogFile { std::string name, sha1, desc; };            // one version of a file
    struct BiosDialogRow {                                                // one line of the dialog
        bool required = false; std::string qualifier;                     // "" or e.g. "Famicom Disk System games"
        std::string system, emulators;                                    // "Saturn", "YabaSanshiro, Beetle Saturn"
        std::vector<BiosDialogFile> recommended;                          // shown in the row (1..n files)
        std::vector<BiosDialogFile> alternates;                           // the Alternates dialog (empty = no link)
        std::string note;                                                 // e.g. "Free download from Sony, ..."
        std::string id;                                                   // the catalogue row ("ps2")
        int section = 0;                                                  // 0 required, 1 for some games, 2 optional
    };
    std::vector<BiosDialogRow> biosDialogRows() const;                    // listed rows: required, then optional
    std::string biosNeedNothingLine() const;                              // "PSP, Xbox, ... need no BIOS."
    // All Supported Files: the whole catalogue by system, as in BIOS.md. These come translated.
    struct BiosAllFile { std::string name, size, check, how; };          // check: SHA-1 or the matching rule
    struct BiosAllGroup { std::string system, emulators; std::vector<BiosAllFile> files; std::string note; };
    std::vector<BiosAllGroup> biosAllFiles() const;                       // the complete catalogue, same order as BIOS.md
    std::string biosAllFilesIntro() const;                                // the top paragraph
    bool biosInstallStart(const std::string& folder);                     // async; false if one is running
    using BiosInstallProgress = nanobios::InstallProgress;
    BiosInstallProgress biosInstallProgress();                            // thread-safe snapshot
    void biosReinstallFromManifest();                                     // async; after an emulator-files move
    std::string biosDefaultFolder() const;                                // the card's "bios" folder if it exists, else ""
    void biosInstallCancel();                                             // stops a running install (Vita3K finishes its own)
    using BiosShared = nanobios::InstallShared;   // the worker's state, shared with its thread (NanoBiosWorker.h)
    std::shared_ptr<BiosShared> mBios = std::make_shared<BiosShared>();
    std::mutex& mBiosMutex = mBios->mu;                                   // guards mBiosBusy
    bool& mBiosBusy = mBios->busy;
    bool mBiosStartupChecked = false;
    // No `this`: raConfig is retroarchConfigFile() when the run was asked for.
    static void biosWorker(std::shared_ptr<BiosShared> sh, std::string folder, bool fromManifest, std::string raConfig);
    void biosStartupTick();                                               // render thread: a reinstall left pending by an exit
    void drasticCheatsFolderSelect(const std::string& path); // persist.gammaos.drastic.cheats_dir (folder-picker target 8)
    void drasticPerfRestoreCheck();            // put back the global performance mode a per-game one parked
    void mouseModeHomeGuard();                 // gammapad mouse mode off whenever the home is the foreground
    int64_t mMouseGuardMs = 0;                 // its once-a-second throttle (uptime)
    void drasticFolderSelect(const char* prop, const char* what, const std::string& path);
    void gsRemoveScanSource(int srcIdx);       // drop a scan source from the edited system
    void gsDisableDefaultFolder(const std::string& alias);  // remove a default scan folder (alias) from the edited system
    void gsEnableDefaultFolder(const std::string& alias);   // restore a previously-removed default scan folder
    void invalidateSystemScan(XmbSystem& sys);              // drop its cache and any scan from before
    void gsAfterScanFolderChange(XmbSystem& sys);           // re-scan + persist + rebuild after a scan-folder edit
    void gsOpenRemoveScanSourceConfirm(int srcIdx);  // Cancel / Remove Folder confirm for a scan source
    const char* menuYAction() const;   // the Y button's action on the focused settings row, for its prompt
    // A folder row (Scan Folders, music / photo / video folders): its value ("Default, 12",
    // "Removed", "Subfolders") shows in the DSi and Minima lists.
    static bool ps3FolderRowKind(int kind) {
        return kind == PS3_GS_SCANSRC || kind == PS3_GS_DEFFOLDER || kind == PS3_GS_DEFFOLDER_OFF ||
               kind == PS3_MUSIC_FOLDER_ROW || kind == PS3_PHOTO_FOLDER_ROW || kind == PS3_VIDEO_FOLDER_ROW;
    }
    void drawXmbYPrompt();             // the XMB theme's prompt for it

    // ======================= File Explorer (Settings > File Explorer) =======================
    // A controller-first file manager that reuses the folder-picker navigation (opendir/readdir,
    // storage roots, ".." up) but lists files AND folders, and exposes Copy/Move/Delete/Rename/
    // Information through the shared XMB X/Triangle side menu (openXmbOpt). Implementation in
    // NanoMenuFileExplorer.cpp. Long-running copy/move/delete run on a detached worker (feOpWorker)
    // so the render thread never blocks (see nano_render_thread_blocking).
    std::string mFeBrowsePath;                  // current explorer directory ("" = storage roots)
    std::string mFeClipPath;                    // pending Copy/Move source (empty = clipboard clear)
    bool        mFeClipMove = false;            // true = Move (cut), false = Copy
    std::string mFeDeleteTarget;                // path awaiting the delete confirm (dialog themeKey 30)
    std::string mNanoUninstallPkg;              // pkg awaiting the uninstall confirm (dialog themeKey 31)
    std::string mNanoUninstallPending;          // pkg being uninstalled: the "Uninstalling..." dialog stays up until it is gone
    // Media option-menu real delete: files to unlink once the Cancel/Delete confirm (dialog themeKey 43)
    // is accepted, plus which library to rescan afterwards. lib: 0=video, 1=photo, 2=music.
    std::vector<std::string> mMediaDelPaths;
    int mMediaDelLib = -1;
    int mMusicDelPlIdx = -1;   // playlist index pending Delete-Playlist confirm (dialog themeKey 46)
    int mVideoDelPlIdx = -1;   // pending Delete-Playlist confirm for a video playlist (themeKey 47)
    int mPhotoDelPlIdx = -1;   // pending Delete-Playlist confirm for a photo playlist (themeKey 48)
    // Async copy/move/delete: the worker holds its OWN shared_ptr to this result block and touches
    // ONLY the block + value-captured paths (never `this`), so a teardown mid-op cannot use-after-free.
    // feTick polls done and reaps. One op at a time (mFeOp non-null = busy).
    struct FeOp {
        std::atomic<bool> done{false}; std::atomic<bool> ok{false}; int kind = 0; std::string name;
        // Extract / compress only: progress + Cancel shared with the worker, and the result text it
        // writes before setting done.
        nanoarc::Progress prog;
        std::string result;
    };
    std::shared_ptr<FeOp> mFeOp;
    // Archive browsing: inside an archive mFeBrowsePath is "<archive path>/<dir inside it>" and the
    // rows come from mFeArcIndex, which a worker reads (FeArcLoad) as fbRequestListing does for folders.
    struct FeArcLoad { std::atomic<bool> done{false}; std::string path; nanoarc::Index index; };
    enum { FE_ROW_ARCHIVE = 1, FE_ROW_RAR = 2 };  // Ps3Item.a on an archive row (a PS3_FE_DIR: A opens it)
    std::string mFeArcPath;                     // archive being browsed ("" = the real filesystem)
    std::shared_ptr<const nanoarc::Index> mFeArcIndex;
    std::shared_ptr<FeArcLoad> mFeArcLoad;
    std::string mFeExtractArc, mFeExtractInner; // Extract To... source while the folder picker is up
    bool        mFeProgDlg = false;             // the extract / compress progress dialog is up
    std::string mFeProgBody;                    // its last body (tells it apart from a dialog that replaced it)
    bool feInArchive() const { return !mFeArcPath.empty(); }
    static bool feCanCompress(const std::string& path);   // false for storage-root / share rows (RAM-backed parent)
    void feEnterArchive(const std::string& path);
    void feBuildArchiveListing(const std::string& cur, Ps3Level& out);
    void feArcLoadTick();                       // per-frame: adopt a finished archive index
    void feStartArchiveOp(int kind, const std::string& src, const std::string& inner, const std::string& dest,
                          bool newFolder);
    void feExtractToSelect(const std::string& dir);   // folder picker result (mFolderPickTarget 13)
    void feOpen();                              // open the explorer at storage roots (from the Settings leaf)
    void buildFileBrowser(const std::string& path, Ps3Level& out);  // list dirs + files at path
    void feRefresh();                           // rebuild the current top level in place after an op
    void feNavigate(const std::string& path);   // enter a directory (rebuild top level in place)
    bool feBack();                              // Circle: up one dir if not at root; true = handled, false = pop
    void feAction(const std::string& act);      // dispatch a side-menu action (fecopy/femove/...)
    void feStartOp(int kind, const std::string& src, const std::string& dst);  // spawn the async worker
    bool nanoRemovePath(const std::string& p);  // real recursive delete (wraps feRemoveRecursive) for the media menus
    void mediaDeleteConfirm(const std::string& title, const std::string& body);  // Cancel/Delete confirm (dialog themeKey 43)
    void mediaCopyToClipboard(const std::string& name);  // stash to the FE clipboard, open the File Explorer, tell the user
    void feTick();                              // per-frame: reap a finished worker, refresh, result dialog
    void feShowInfo(const std::string& path);   // open the Information page for a file/folder
    void feInfoDialog(const std::string& title, const std::string& body);  // generic XMB info dialog (kind 0)

    // ======================= Network Shares (Settings > Network Shares) =======================
    // Add/edit SMB, NFS, WebDAV and FTP shares. The share itself is served by the gammaos-sharefs
    // FUSE daemon at /mnt/shares/<name>; this is only the editor for the persist.gammaos.share.<n>.*
    // properties it reads, via the shared share_config.cpp so a share added here is identical to
    // one added in Settings or TvSettings. Implementation in NanoMenuShares.cpp.
    //
    // Field ids for the editor rows (Ps3Item.a on a PS3_NS_FIELD).
    enum NsField {
        NSF_ENABLED = 0, NSF_NAME, NSF_TYPE, NSF_HOST, NSF_PORT, NSF_PATH,
        NSF_USER, NSF_PASS, NSF_DOMAIN, NSF_TLS, NSF_READONLY, NSF_STATUS, NSF_DELETE,
        NSF_SELFSIGNED,
    };
    int  mNsEditSlot = 0;                       // slot being edited (1..kMaxShares), 0 = none
    bool mNsEditIsNew = false;                  // editing a share that is not saved yet
    // Mount-state follower. A share connects asynchronously (init starts the daemon, which then has
    // to reach the server), so without this the row the user just switched on would sit on
    // "Connecting..." until they left the screen and came back.
    float       mNsNextPoll = 0.0f;             // mEffectTime at which to re-read the mount table
    std::string mNsMountSig;                    // which shares were mounted at the last read
    void nsTick();                              // per-frame: rebuild only when the mount set changed
    void nsOpenList();                          // Settings leaf -> the shares list
    void buildSharesList(Ps3Level& out);        // configured shares + "Add Share"
    void nsOpenEditor(int slot, bool isNew);    // drill into one share
    void buildShareEditor(Ps3Level& out);       // the field rows for mNsEditSlot
    void nsEditField(int field);                // OSK / chooser / toggle for one field
    void nsSetType(int typeIdx);                // apply the type chooser result
    void nsToggleEnabled();                     // enable/disable the mount (validates first)
    void nsAddShare();                          // create a share in the first free slot
    void nsOpenRemoveConfirm();                 // confirm before clearing a slot
    void nsRemoveShare();                       // clear mNsEditSlot and pop back to the list
    void nsDiscardIfUnconfigured();             // Back out of a never-filled-in new share = drop it
    void nsRefreshStackLevels();                // rebuild any shares screen still on the nav stack

    // ======================= Syncthing (Settings > Syncthing) =======================
    // The nano client for the Syncthing daemon (external/gammaos-syncthing). Screens are plain
    // Ps3Level submenus so all four themes render them; the REST client is NanoSyncthing.h and
    // the screens live in NanoMenuSyncthing.cpp. Row ids (Ps3Item.a on a PS3_ST_ROW):
    enum StRow {
        STR_INFO = 0,
        STR_ENABLED, STR_THISDEVICE, STR_FOLDERS, STR_DEVICES, STR_PENDING, STR_OPTIONS, STR_WEBGUI, STR_RESTART, STR_LOG,
        STR_FOLDER_ROW, STR_FOLDER_ADD, STR_FOLDER_SAVE, STR_FOLDER_LABEL, STR_FOLDER_ID, STR_FOLDER_PATH, STR_FOLDER_TYPE,
        STR_FOLDER_SHARE, STR_FOLDER_VERSIONING, STR_FOLDER_VERSIONING_PARAM, STR_FOLDER_RESCAN, STR_FOLDER_WATCH,
        STR_FOLDER_IGNPERMS, STR_FOLDER_PAUSED, STR_FOLDER_RESCAN_NOW, STR_FOLDER_OVERRIDE, STR_FOLDER_REVERT,
        STR_FOLDER_IGNORES, STR_FOLDER_REMOVE,
        STR_DEVICE_ROW, STR_DEVICE_ADD, STR_DEVICE_SAVE, STR_DEVICE_NAME, STR_DEVICE_ADDR, STR_DEVICE_COMPRESSION,
        STR_DEVICE_INTRODUCER, STR_DEVICE_AUTOACCEPT, STR_DEVICE_SHARE, STR_DEVICE_PAUSED, STR_DEVICE_REMOVE,
        STR_SHARE_TOGGLE, STR_PENDING_DEVICE, STR_PENDING_FOLDER,
        STR_OPT_NAME, STR_OPT_LISTEN, STR_OPT_GLOBAL, STR_OPT_LOCAL, STR_OPT_RELAYS, STR_OPT_NAT, STR_OPT_RECV, STR_OPT_SEND,
        STR_OPT_LANLIMIT, STR_OPT_CONCURRENCY, STR_OPT_MINFREE, STR_OPT_UR,
        STR_IGNORE_ROW, STR_IGNORE_ADD,
    };
    std::mutex        mStMutex;                 // guards mStSnap
    nanost::Snapshot  mStSnap;                  // latest worker result
    nanost::Snapshot  mStShown;                 // what the open screens were built from (render thread)
    std::thread       mStWorker;
    std::atomic<bool> mStWorkerRun{false};
    std::atomic<bool> mStRefreshNow{false};     // a mutation asked for an immediate refresh
    nanost::FolderCfg mStFolderDraft;           // folder editor subject
    nanost::DeviceCfg mStDeviceDraft;           // device editor subject
    bool mStFolderIsNew = false, mStDeviceIsNew = false;
    bool mStShareFolderMode = true;             // ST_SHARE: devices for a folder (true) / folders for a device
    bool mStPendingIsDevice = true; int mStPendingIdx = 0;   // the pending row a chooser is open for
    std::vector<std::string> mStIgnores;        // ST_IGNORES working copy
    int  mStIgnoreIdx = -1;
    bool stInstalled() const;                   // the daemon binary is in this image
    bool stEnabled() const;                     // persist.gammaos.syncthing.enabled
    bool stScreenOpen() const;                  // a Syncthing screen is on top of the nav stack
    void stWorkerStart();
    void stWorkerStop();
    void stTick();                              // per frame: run/stop the worker, rebuild on new data
    void stRefreshSoon();
    void stRefreshStackLevels();
    void stPush(Ps3Level& lvl);
    static Ps3Item stRow(const std::string& label, int row, const std::string& value,
                         const std::string& desc = std::string(), int aux = 0, const std::string& payload = std::string());
    static Ps3Item stInfoRow(const std::string& label, const std::string& value, const std::string& desc = std::string());
    static Ps3Item stToggleRow(const std::string& label, int row, bool on, const std::string& desc = std::string(), int aux = 0);
    std::string stDeviceIp() const;
    void stOpenRoot();                          // Settings leaf -> the root screen
    void buildStRoot(Ps3Level& out);
    void buildStFolders(Ps3Level& out);
    void buildStFolder(Ps3Level& out);
    void buildStDevices(Ps3Level& out);
    void buildStDevice(Ps3Level& out);
    void buildStPending(Ps3Level& out);
    void buildStOptions(Ps3Level& out);
    void buildStLog(Ps3Level& out);
    void buildStShare(Ps3Level& out);
    void buildStIgnores(Ps3Level& out);
    void stOpenFolder(const std::string& id);
    void stAddFolder(const std::string& presetId, const std::string& presetLabel, const std::string& sharedWith);
    bool stCommitFolder();
    void stFolderPathSelect(const std::string& path);   // folder browser result (mFolderPickTarget 6)
    void stEnsureFolderDir(const std::string& path);      // create a new folder's directory writable by the daemon
    void stOpenDevice(const std::string& id);
    void stAddDevice(const std::string& presetId, const std::string& presetName, const std::string& presetAddress);
    bool stCommitDevice();
    void stShareToggle(int idx);
    bool stCommitOptions(const nanost::Options& o);
    void stOpenIgnores();
    void stSaveIgnores();
    void stOpenChooser(int key, const std::string& title, const std::vector<std::string>& opts, int sel);
    void stOpenConfirm(int key, const std::string& title, const std::string& body, const char* action);
    void stOpenText(const std::string& prompt, const std::string& prefill, std::function<void(const std::string&)> onSubmit);
    bool stDialogResult(int key, int sel);      // applyThemeSetting hands keys 50..59 here
    void stSelectRow(const Ps3Item& it);        // ps3XmbSelect on a PS3_ST_ROW

    // ======================= Music player (PS3 XMB port) =======================
    // Library model (nano_music.json), folder import (reuses the folder picker via
    // mFolderPickTarget), the scanner, and the Music-column content. The Now-Playing
    // screen + control panel + playlists are in NanoMenuMusic.cpp.
    struct MusicTrack {
        std::string file;        // absolute path
        std::string title;       // tag title (fallback: filename)
        std::string artist;
        std::string album;       // album group key (fallback: parent folder name)
        std::string codec;       // badge text (MP3/AAC/FLAC/...)
        double durationSec = 0.0;
        int trackNo = 0;
        int64_t mtime = 0;       // for incremental rescan
        bool albumHidden = false; // track lives in a folder that has an .m3u: shown via the playlist,
                                  // not as a duplicate folder-fallback album
    };
    struct MusicPlaylist {
        std::string name;
        std::vector<std::string> files;   // references MusicTrack.file
        std::string m3uPath;              // non-empty: derived from this .m3u (regenerated on scan);
                                          // empty: user-created (preserved across scans)
    };
    int mFolderPickTarget = 0;             // 0 = Game Systems scan source, 1 = Music library, 2 = Photo library, 3 = Video library, 4 = ES-DE bulk auto-add root, 5 = DraStic data folder, 6 = Syncthing folder path, 7 = boxart folder, 8 = DraStic cheats folder, 9 = system icon file (lists images), 10 = ES-DE media folder, 11 = RetroArch folder, 13 = File Explorer Extract To...
    std::vector<std::string> mMusicFolders;
    std::vector<MusicTrack>  mMusicTracks;
    std::vector<MusicPlaylist> mMusicPlaylists;
    int64_t mMusicCfgStamp = -1;           // mtime of nano_music.json (cross-process reload)
    // Metadata-parser schema version. Bumped whenever the way we read tags changes
    // so an existing library (mtime-cached) re-probes instead of keeping stale data.
    // v2: read real container tags (title/artist/album) via the fixed getFileFormat.
    // v3: parse .m3u/.m3u8 into playlists + hide the folder-fallback album for m3u dirs.
    static const int kMusicMetaVersion = 3;
    int mMusicCfgVersion = 0;              // version found in nano_music.json (0 = none)
    bool mMusicLoaded = false;             // library parsed once (lazy, first Music entry)
    bool mMusicCatsStale = false;          // a scan finished -> rebuild the Music column at root
    // scan worker
    std::mutex mMusicScanMutex;
    std::vector<MusicTrack> mMusicScanResults;
    std::vector<MusicPlaylist> mMusicScanPlaylists;   // m3u-derived playlists from the last scan
    bool mMusicScanReady = false;
    bool mMusicScanRunning = false;
    bool mMusicScanPending = false;        // scan deferred until external storage is mounted
    bool musicStorageReady() const;        // true when the imported folders are reachable
    void musicRefresh();                   // user-triggered rescan of the imported folders
    // Default media directories: nano always scans the standard media folders that
    // exist across every storage medium (internal + each mounted external volume),
    // in addition to any folders the user imported. kind: 0 = photo, 1 = video,
    // 2 = music. nanoDefaultMediaDirs returns the existing standard dirs; the *ScanDirs
    // helper merges them with the user folders (deduped). Both skip paths that do not
    // exist, so an unmounted card or missing folder is handled gracefully.
    std::vector<std::string> nanoDefaultMediaDirs(int kind) const;
    std::vector<std::string> nanoMediaScanDirs(int kind, const std::vector<std::string>& userFolders) const;

    // ---- Boxart / cover scraper (NanoMenuScraper.cpp) ---------------------
    // Manifest of scraped art keyed by ROM path -> on-disk cover/fanart files.
    // Tiny metadata; loaded lazily, kept resident. The GL textures it points to
    // are loaded/freed on demand (Phase 3/4), not here.
    struct ScrapeEntry {
        std::string box;     // cover file path ("" = none)
        std::string fan;     // fanart file path ("" = none)
        std::string title;   // matched game title
        std::string scraper; // "screenscraper" | "thegamesdb"
        long long when = 0;  // epoch seconds when scraped
        // Metadata for the Information screen (empty = no data).
        std::string synopsis, genre, players, rating, releaseDate, developer, publisher;
        // ES-DE gamelist play stats (raw gamelist.xml values): lastPlayed is ISO basic
        // YYYYMMDDThhmmss, playTime is a whole-second count, playCount a launch tally. Empty/unset
        // renders never / unknown / 0.
        std::string lastPlayed, playTime, playCount;
        // ES-DE badge metadata (gamelist.xml): the bool flags render the matching badge slot; the
        // controller string is a controller shortName whose icon overlays the controller badge; the
        // altemulator string enables the altemulator badge when non-empty.
        bool favorite = false, completed = false, kidgame = false, broken = false;
        std::string controller, altemulator;
        // Extra media (kExtraMedia bits) the service did not have when last asked, and when.
        int extrasMissing = 0;
        long long extrasCheckedAt = 0;
        // Scrape results only, not saved: the job fetched only extra media, and the extras it asked for.
        int artKept = 0;     // 1 cover, 2 fan art: a picture the ES-DE folder had, used and never deleted by Nano
        bool extrasOnly = false;
        bool metaForce = false;   // a scrape result replacing the ES-DE folder's metadata
        int extrasTried = 0;
        std::vector<std::string> extrasWritten;   // files saved, so stale cached textures of them go
    };
    std::unordered_map<std::string, ScrapeEntry> mScrapeIndex;
    bool mScrapeIndexLoaded = false;
    std::string mScrapeReloadToken;        // last sys.gammaos.nano.scrape_reload token handled
    void scraperEnsureLoaded();            // lazy-load index.json
    void scraperPollReload();              // per-frame: live-reload index/names on an external token bump
    void loadScrapeIndex();
    std::string scrapeIndexText(const std::unordered_map<std::string, ScrapeEntry>& index);
    void saveScrapeIndex();
    const ScrapeEntry* scrapeEntryFor(const std::string& romPath);
    const ScrapeEntry* scrapeIndexLookup(const std::string& romPath);   // the manifest entry alone (no cart art)
    // PICO-8 carts (*.p8.png) are their own cover: the cartridge image is the artwork. scrapeEntryFor
    // hands out a copy of the manifest entry with the cart as the cover unless the user picked one.
    static bool isPico8Cart(const std::string& path);
    // The cart's own label is its cover (shown even with Boxart off, like the DS banner icon),
    // unless the user picked a cover by hand.
    bool romUsesCartArt(const std::string& romPath);
    static constexpr int kCartLabelPx = 128;     // a cart's label is 128x128 PICO-8 pixels
    // Side of a cart label drawn to stand in for something `target` device px wide, kept to an
    // integer multiple (or an integer fraction) of the label and no larger than maxSide.
    static float cartPixelSize(float target, float maxSide);
    bool romBoxartIsPixel(const std::string& romPath);   // its cached cover is a cart label (draw integer-scaled)
    struct CartArt { ScrapeEntry entry; const ScrapeEntry* src = nullptr; long long srcWhen = 0; std::string srcBox, srcScraper; bool valid = false; };
    std::unordered_map<std::string, CartArt> mCartArt;
    // ES-DE per-game metadata read straight from ES-DE's own gamelist.xml files (the same source
    // real ES-DE displays), so the ES-DE theme engine shows the identical description / rating /
    // release date / developer / publisher / genre / players as the control, instead of nano's own
    // scrape store which can differ (e.g. a timezone-shifted release date). Loaded lazily per system
    // and merged with the scrape store's cover paths; absent gamelist.xml -> falls back to the store.
    std::unordered_map<std::string, ScrapeEntry> mEsdeGamelistMeta;  // full romPath -> gamelist metadata
    // Each gamelist.xml as last parsed (by path), re-checked every few seconds so an edited
    // gamelist, or games added to the system, show without a restart.
    struct EsdeGamelistStamp { long long mtime = -2, size = -2; size_t roms = 0; int64_t checkedMs = 0; };
    std::unordered_map<std::string, EsdeGamelistStamp> mEsdeGamelistStamp;
    // The system folder a rom was scanned in and its path below it ("Game/EBOOT.PBP"); false for
    // a rom outside every scanned folder. sysIdx < 0 searches every system.
    bool esdeRomSubPath(const std::string& romPath, int sysIdx, int* outSys, std::string* outDir, std::string* outRel);
    // Game media textures in mEsdeTexCache, least recently drawn dropped past 96 images / 128 MB.
    struct EsdeMediaUse { uint64_t used = 0; size_t bytes = 0; };
    std::unordered_map<std::string, EsdeMediaUse> mEsdeMediaUse;
    uint64_t mEsdeMediaClock = 0, mEsdeMediaFrameClock = 0, mEsdeMediaPrevFrameClock = 0;
    void esdeMediaTouch(const std::string& path);
    // Read gamelists/<system>/gamelist.xml on the gamelist worker (NanoThemeEngine.cpp).
    enum { kGlPrefetch = 0, kGlNow = 1, kGlWait = 2 };
    void esdeEnsureGamelistLoaded(int sysIdx, int mode = kGlNow);
    struct EsdeGamelistInput;
    struct EsdeGamelistPool;
    struct EsdeGlPending { uint64_t id = 0; bool first = false; };
    std::shared_ptr<EsdeGamelistPool> mEsdeGlPool;
    // XmbSystem::id -> its gamelist read in flight (by id: the systems can be re-ordered meanwhile)
    std::unordered_map<std::string, EsdeGlPending> mEsdeGlReading;
    uint64_t mEsdeGlNextId = 0;
    bool mEsdeGlPrefetched = false;
    EsdeGamelistInput esdeGamelistInput(int sysIdx, const std::string& path);
    void esdeGamelistAdopt();
    void esdeGamelistApply(int sysIdx, bool first, std::vector<std::pair<std::string, ScrapeEntry>> meta);
    void esdeGamelistPrefetch();
    // The ES-DE folder as Nano's store (NanoEsdeStore.cpp), once it was moved to the card.
    static std::string esdeDataDir();          // persist.gammaos.nano.esde.dir, else /storage/emulated/0/ES-DE
    static bool esdeStoreMode();               // the ES-DE folder on the card is Nano's store
    bool esdeStoreReady();                     // ... and the card is in
    std::string romNamesPath();
    std::string esdeSystemFolder(int sysIdx);  // the system's folder under gamelists/ and downloaded_media/
    const std::vector<std::string>& esdeSystemRoots(int sysIdx);
    bool esdeRomRef(const std::string& rom, std::string* gamelist, std::string* rel, std::string* folder);
    std::string esdeRootFolder(const std::string& root, int sysIdx);   // a system folder's ES-DE name
    bool esdeArtDest(const std::string& rom, int sysIdx, nanoscraper::ArtDest* d);
    void esdeGamelistSet(const std::string& rom, const char* field, const std::string& value);   // "" removes it
    void esdeGamelistTick();                   // per frame: write the edits that are due
    bool esdeGamelistFlush();
    void esdeStoreLoadFlags(std::vector<std::string>* favs, std::map<std::string, std::string>* hidden);
    void esdeStoreSaveFlag(const char* field, const std::map<std::string, std::string>& now,
                           std::map<std::string, std::string>& synced);
    std::vector<std::string> esdeRomPathCandidates();
    void esdeStoreLoadCollections();
    void esdeStoreSaveCollections();
    static std::string esdeSettingsGetIn(const std::string& file, const std::string& key);
    // type: the tag of a setting the file does not have yet (one it has keeps its own).
    static bool esdeSettingsSetIn(const std::string& file, const std::string& key, const std::string& value,
                                  const char* type = "string");
    std::string esdeSettingsGet(const std::string& key);
    bool esdeSettingsSet(const std::string& key, const std::string& value, const char* type = "string");
    std::string esdeRomDirectory();            // ES-DE's ROM folder (es_settings.xml ROMDirectory), "" = none
    std::string esdeRomDirectoryNow();
    std::string mEsdeRomDir, mEsdeRomDirFor;
    int64_t mEsdeRomDirMs = 0;
    static std::string esdeAppPathOf(const std::string& p);   // the path ES-DE (an app) sees
    static std::string esdeCollectionLine(const std::string& rom, const std::string& romDir, const std::string& esdeDir);
    void esdeGamelistPlayed(const std::string& rom);
    void esdeGamelistRemove(const std::string& rom);   // the game's <game> entry goes (store mode)
    static std::string esdeGamelistRating(const std::string& r);
    static std::string esdeGamelistDate(const std::string& d);
    void esdeGamelistPushScrape(const std::string& rom, const ScrapeEntry& e);
    void esdeStoreSaveNames();
    void esdeStoreTick();
    void esdeStorePlayedTick();
    int64_t mEsdePlayedCheckMs = 0;
    void esdeStoreReload(bool rescan);
    bool esdeStoreFollowCard();                // the configured card is out: the same ES-DE folder on the one in
    std::string esdeStoreIdentity();           // which card and folder the store is (a swap changes it)
    // What a gamelist holds for Nano, by <path> (relative to its system folders): read again when it changes.
    struct EsdeFlags {
        bool parsed = false; long long mtime = -1, size = -1;
        std::vector<std::string> favs, hidden;                        // resolved to the system folder holding them
        std::vector<std::pair<std::string, std::string>> names;      // rel, <name>
        std::vector<std::pair<std::string, std::string>> altEmu;     // rel, <altemulator>
        std::vector<std::pair<std::string, std::string>> played;     // rel, <lastplayed>
        std::string sysEmu;                                           // <alternativeEmulator><label>
        bool unreadable = false;                                      // there, but not a gamelist tinyxml2 reads
        bool resolved = false; size_t resolvedSig = 0;                // favs and hidden found, for that scan
        std::vector<std::string> favPaths, hiddenPaths;
    };
    std::unordered_map<std::string, EsdeFlags> mEsdeFlagsCache;   // per gamelist (and system folders)
    // Every gamelist's names, emulators and play times, gathered by esdeStoreLoadFlags.
    std::unordered_map<std::string, std::string> mEsdeGlNames;    // esdeKey(game) -> <name>
    struct EsdeAltEmu { std::string label, folder; int sys = -1; };
    std::unordered_map<std::string, EsdeAltEmu> mEsdeGlAltEmu;    // esdeKey(game) -> its <altemulator>
    std::map<std::string, std::vector<std::string>> mEsdeFolderRoots;   // ES-DE folder -> the system folders that are it
    std::map<std::string, std::string> mEsdeGlSysEmu;             // ES-DE folder -> <alternativeEmulator> label
    std::set<std::string> mEsdeGlUnreadable;                      // ES-DE folders whose gamelist cannot be read
    std::map<std::string, std::string> mEsdeSysEmuSynced;         // ES-DE folder -> label Nano and the gamelist agreed on
    bool mEsdeSysEmuSyncedLoaded = false;
    void esdeSysEmuSyncedLoad();
    void esdeSysEmuSyncedSave();
    struct EsdePlayed { std::string when; std::vector<std::string> paths; };   // a game in each folder it can be in
    std::vector<EsdePlayed> mEsdeGlPlayed;
    std::unordered_map<std::string, int> mEsdeFolderSys;          // hiddenKey(system folder) -> system index
    static std::string esdeKey(const std::string& path);          // hiddenKey, lowercase
    std::string esdeCanonRom(const std::string& path);            // the scanned spelling of a game (the card ignores case)
    void esdeRomCaseRefresh();
    std::string esdeFindGame(const std::string& root, const std::string& rel);
    std::unordered_map<std::string, std::string> mEsdeRomCase;    // lowercase hiddenKey -> scanned path
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> mEsdeDirCase;   // per load: folder listings
    bool mEsdePlayedRetry = true;      // the games that were in no system folder: tried again
    int64_t mEsdeCardGoneMs = 0;       // when esdeStoreFollowCard first saw the card out
    bool mEsdeFollowPending = false;   // esdeStoreFollowCard switched cards: reload before anything is saved
    bool mEsdeApplyAfterLoad = false;  // the card's systems were loaded: apply their gamelists' emulators
    size_t mEsdeRomCaseSig = 0;
    // The ES-DE folder's files, looked at every few seconds (esdeStoreSweep): a change made to them
    // elsewhere (ES-DE, a computer, another card) is read again.
    enum { kSwGamelist = 1, kSwCollections = 2, kSwSettings = 4, kSwSystems = 8, kSwRoms = 16, kSwNames = 32 };
    struct EsdeSweepItem { std::string path; int kind = 0; long long stamp = -1; std::string exts; long long games = 0; };
    std::vector<EsdeSweepItem> mEsdeSweep;
    int64_t mEsdeSweepMs = 0;
    std::string mEsdeSweepRoots, mEsdeStoreId;
    void esdeStoreSweep();
    void esdeStoreSweepBuild();
    void esdeNoteWrite(const std::string& path);   // a file Nano wrote: not a change made elsewhere
    void esdeStoreRefresh(int kinds);
    struct EsdePlaces {
        std::vector<std::string> level;                            // each open level's focused entry (esdePlaceKey)
        std::vector<std::string> levelColl;                        // each open level's collection, if it is one
        std::map<std::string, std::vector<std::string>> roms;     // each system's list (by id), before
        std::string ctxSys, ctx, esdeSys, esde;                    // the option menu's game, the ES-DE list's
        std::string ctxColl, ctxRecent;                            // the option menu's collection, recent game
    };
    std::string esdePlaceKey(const Ps3Item& it) const;
    EsdePlaces esdePlacesTake();
    void esdePlacesRestore(const EsdePlaces& p);
    // Emulators: a game's <altemulator>, a system's <alternativeEmulator>, both ES-DE command labels;
    // a game's choice ES-DE has no label for is kept in nano/emulators.json.
    void esdeGamelistSetSystem(const std::string& folder, const char* field, const std::string& value);
    std::string esdeEmulatorLabel(const std::string& esdeSys, const XmbSystem& launch);
    int esdeCatalogForLabel(int sysIdx, const std::string& esdeSys, const std::string& label,
                            const std::set<std::string>& installed);   // mEmuCatalog index, -1 = none that runs here
    std::set<std::string> emuInstalledPackages();
    std::vector<std::string> esdeSystemFolders(int sysIdx);      // its ES-DE folders, in its folders' order
    std::string esdeCommandSystem(const std::string& folder, int sysIdx);   // whose ES-DE commands they are
    void esdeStoreLoadGameEmus();
    void esdeStoreSaveGameEmu(const std::string& rom, const GameEmulator* emu);
    void esdeStoreApplySystemEmus();
    void esdeStoreSaveSystemEmus();
    void esdeStoreRebuildRecents();
    XmbRecentEntry recentEntryFor(int sysIdx, int gameIdx);
    // Nano's systems (nano/systems.json, custom_systems/es_systems.xml) and settings (es_settings.xml).
    void esdeStoreSaveSystems();
    bool esdeStoreAdoptSystems();
    void esdeStoreWriteCustomSystems();
    void esdeStoreApplySettings();
    void esdeStoreSettingChanged(const std::string& prop);
    static std::vector<std::pair<std::string, std::pair<std::string, const char*>>> esdeSettingsFromProps();
    std::string esdeSystemsJson();
    bool writeSystemsConfig(const std::vector<XmbSystem>& list);   // nano_systems.json, nothing else
    bool mEsdeSystemsAdopted = false;   // nano_systems.json holds the card's systems, not loaded yet
    njson::Value mEsdeKeptSystems;      // the card's systems Nano does not use, written back as they were
    std::string mEsdeKeptFor;           // ... for that ES-DE folder
    bool mEsdeKeptLoaded = false;
    void esdeKeptSystemsSave();
    bool mEsdeAdoptLater = false;       // a save found the card's systems changed elsewhere: read them
    std::string mEsdeStagingRomDir;     // a move to the card is staging: ES-DE's ROM folder there
    struct EsdeRoots { int64_t first = 0; std::string sig; std::vector<std::string> second; };
    std::unordered_map<std::string, EsdeRoots> mEsdeRootsCache;   // by system id
    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> mEsdeGlPending;
    int64_t mEsdeGlDueMs = 0;
    std::map<std::string, std::string> mEsdeStoreFavs, mEsdeStoreHidden;   // what the gamelists hold (hiddenKey -> path)
    std::unordered_map<std::string, std::string> mEsdeStoreNames;          // the titles they hold
    std::set<std::string> mEsdeCollectionFiles;                            // the collection files read
    struct EsdeCollCache { bool parsed = false; long long mtime = -1, size = -1; std::string roots; XmbCollection c; };
    std::map<std::string, EsdeCollCache> mEsdeCollCache;                   // each file, read again when it changes
    static std::string esdeCollectionFileName(const std::string& name);
    int64_t mEsdeStoreCheckedMs = 0;
    bool mEsdeStoreOk = false, mEsdeStoreWasOk = false, mEsdeStoreSeen = false, mEsdeFlagsLoadedReady = false;
    bool mEsdeNamesNeedLoad = false;
    bool mEsdeReloadAfterScan = false;   // systems were added: read the ES-DE folder again once they are scanned
    const ScrapeEntry* esdeMetaFor(const std::string& romPath);      // gamelist entry, else scrapeEntryFor
    bool esdeIsFavorite(const std::string& romPath, const ScrapeEntry* meta) const;
    void favoritesFirst(std::vector<std::string>& roms, std::vector<std::string>& displayNames);
    void favoritesResort(const std::string& romPath);
    void favoritesResort(int sysIdx, std::string keepRom, bool keepTop = false);
    // Systems whose ES-DE favourites changed: re-order them next frame (first: the gamelist's
    // first load, so its cursor stays at the top).
    std::vector<std::pair<int, bool>> mFavResortSys;
    bool mRomInfoEsdeMeta = false;   // the next "rominfo" page reads esdeMetaFor (ES-DE's X)
    // Per-game title override (Rename / Edit Title). A user-typed name that overrides
    // the basename-minus-extension display name everywhere (columns / recents / search /
    // Info) AND becomes the scraper search query so a corrected title can match. Stored
    // in a sidecar (names.json) next to the scrape manifest, keyed by ROM path.
    std::unordered_map<std::string, std::string> mRomNameOverride;
    bool mRomNameOverrideLoaded = false;
    void loadRomNameOverrides();
    void saveRomNameOverrides();
    const std::string* romNameOverrideFor(const std::string& romPath);   // lazy-load; alias-normalized
    void setRomNameOverride(const std::string& romPath, const std::string& name);
    void clearRomNameOverride(const std::string& romPath);
    void applyRomNameOverrides(std::vector<std::string>& roms,
                               std::vector<std::string>& displayNames);
    void applyRomNameOverrides(XmbSystem& sys);            // patch sys.displayNames from the map
    void applyRomNameOverridesToRecents();                 // patch mXmbRecent[].displayName from the map
    // Re-scrape one ROM (force overwrite). queryOverride, when non-empty, is used as the search
    // query for this scrape ONLY - a one-off "search with a different name" that does NOT persist
    // any rename (unlike the saved title override). Empty falls back to the saved override / filename.
    void scrapeOneRom(int sysIdx, int romIdx, const std::string& queryOverride = std::string());
    std::string focusedRomPath();                  // focused home item's ROM path (PS3_ROM / PS3_RECENT) or ""
    const ScrapeEntry* focusedScrapeEntry();       // scrape entry for the focused ROM if it has art, else null
    bool openInfoForFocusedItem();                 // Y shortcut: open Information for a focused scraped-art item
    bool scraperBoxartEnabled();           // persist.gammaos.scraper.boxart
    // ---- DSi theme: DS ROM banner icon + title (NanoMenuNdsBanner.cpp) ----
    // The banner every DS cartridge carries (32x32 icon + title) stands in for the
    // generic cartridge glyph and the file name on the DSi carousel when nothing scraped
    // exists. Parsed once per ROM (disk cache keyed by path/size/mtime), titles by the
    // scan thread before the list is sorted, icons uploaded lazily on first draw.
    struct NdsBannerTexEntry { GLuint tex = 0; };
    std::unordered_map<std::string, NdsBannerTexEntry> mNdsBannerTex;   // render thread only
    std::mutex mNdsBannerMu;                                             // guards the three below
    std::unordered_map<std::string, std::string> mNdsBannerTitle;        // path -> title ("" = none/failed)
    std::unordered_map<std::string, std::vector<uint8_t>> mNdsBannerPix; // path -> RGBA awaiting upload
    std::deque<std::string> mNdsBannerQueue;                             // draw-time requests for the worker
    std::unordered_map<std::string, uint64_t> mNdsBannerIdent;           // path -> size/mtime the result belongs to
    std::unordered_map<std::string, int64_t> mNdsBannerRetryAt;          // path unreachable: uptime ms of the next try
    std::vector<std::string> mNdsBannerTexDrop;                          // paths whose uploaded icon is stale (render thread drains)
    std::condition_variable mNdsBannerCv;
    bool mNdsBannerWorkerUp = false;
    std::atomic<bool> mNdsBannerLanded{false};                            // worker finished something: names may change
    void ndsBannerStartWorkerLocked();                                   // mNdsBannerMu held
    bool ndsBannerWantsParseLocked(const std::string& rom);              // mNdsBannerMu held
    void ndsBannerTick();                                                // render thread, per frame
    bool ndsRomTitleEnabled();                                           // persist.gammaos.nano.nds.romtitle (default on)
    bool ndsIsDsRomItem(const Ps3Item& it, std::string* romPath);        // DS system ROM or DS recent entry
    void ndsBannerLoad(const std::string& romPath);                      // sync parse via disk cache (any thread)
    void ndsBannerPrefetch(const std::vector<std::string>& roms);        // scan thread: titles before sorting
    std::string ndsBannerTitleFor(const std::string& romPath);           // "" if unknown or none
    GLuint ndsBannerTex(const std::string& romPath);                     // render thread; 0 until ready
    void ndsBannerFreeAll();                                             // drop GL textures (theme/GL teardown)
    bool scraperFanartEnabled();           // persist.gammaos.scraper.fanart
    nanoscraper::Credentials scraperCredsFor(int sysIdx);   // global + per-system override
    nanoscraper::Engine scraperEngineFor(int sysIdx, const nanoscraper::Credentials& cred);
    void scrapeAllSystems();               // Settings action: scrape every enabled system
    void openHelpPage();                    // Settings > User Guide: scrollable, themed help page
    void scrapeOneSystem(int sysIdx);      // Game Systems editor action
    // One ROM of work, fully snapshotted so the worker never touches mXmbSystems.
    using ScrapeJob = nanoscraper::QueueJob;
    std::vector<int> scrapeEnabledExtras() const;   // kExtraMedia indexes turned on in Content Settings
    void scrapeJobAddExtras(int sysIdx, ScrapeJob& j, const std::vector<int>& types, bool overwrite, bool recheck);
    void scrapeSystemsAsync(const std::vector<int>& sysIdxs);
    // Scrapes run in the gammaos-nano-scrape service (NanoScrapeQueue.h), so they go on while a
    // game runs and either home shows the same one. The modal's counters mirror its progress.
    void scraperStartBackground(std::vector<ScrapeJob> jobs, bool wantBox, bool wantFan);
    void scraperBgPoll(bool now);          // follow the service's progress (a few times a second)
    void scraperApplyResults();            // the games it finished since the last call -> mScrapePending
    void scraperReloadFromDisk();          // index + title overrides from disk, textures dropped
    void scraperJournalAtEnd();            // a manifest with no results record holds them all
    bool scraperRefuseWhileBusy();         // a scrape or copy runs: show it (or say so) and return true
    bool scraperCopyBusy();                // a copy or move runs: say so and return true
    void scraperOpenProgress(int total);   // the modal, counters at zero
    bool scraperOfferQueue(std::vector<ScrapeJob>& jobs, bool wantBox, bool wantFan, const std::string& emptyMsg);
    void scraperQueueAdd(std::vector<ScrapeJob> jobs, bool wantBox, bool wantFan);
    enum class QueueAdd { Done, Ended, Newer };   // added (or refused, said in a banner); the run ended; a newer one runs
    QueueAdd scraperQueueAddLocked(std::vector<ScrapeJob>& jobs, bool wantBox, bool wantFan);
    std::string scrapeStatusLabel();       // "Scraping 34/120" for the status bars while one runs, else ""
    void scraperDrainResults();            // render thread: merge finished art + progress
    void scraperCancel();
    void renderScrapeProgress();           // the progress / result modal
    // USB Settings (NanoMenuMtp.cpp): the chosen USB mode, kept up while a computer is connected.
    std::string usbMode() const;           // persist.gammaos.nano.usb_mode: mtp (default), ptp, rndis or none
    void setUsbMode(const std::string& mode);
    void usbModeTick();
    void typecStart();   // the USB-C port's power role for a monitor (NanoTypec.cpp), on its own thread
    void typecTick();
    int mTypecLockFd = -1;
    int64_t mTypecLockTryMs = 0, mTypecUnpluggedMs = 0, mTypecPortMs = 0, mTypecPartnerMs = 0;
    bool mTypecBootDone = false, mTypecDp = false, mTypecDpAtFlip = false, mTypecThreadStarted = false;
    std::string mTypecPort;                  // the port that negotiates (port1 on the RG Cube), "" not found yet
    std::vector<std::string> mTypecOthers;   // every port: any with a partner means something is plugged in
    void usbApplyAsync(const std::string& mode);
    std::atomic<bool> mUsbApplying{false};    // apply worker running
    std::atomic<bool> mUsbOnline{false};      // cable present (power_supply usb online)
    std::atomic<int>  mMtpMediaPid{0};        // pinned MediaProvider module pid (0 = none)
    std::atomic<int64_t> mUsbLastApplyMs{0};  // last apply attempt, 0 = apply at the next tick
    std::atomic<int>  mUsbPlugGen{0};         // bumps on every plug change
    bool mUsbSeen = false;                 // the first tick has read the cable state
    bool mUsbBootDone = false;             // the USB service is up (or boot completed): the USB helper can run
    int64_t mUsbLastPollMs = 0, mUsbLastWatchMs = 0;   // wall-clock pacing of usbModeTick
    int  mMtpMissCount = 0;                // consecutive watchdog checks with no host process
    // scrape worker state
    std::mutex mScrapeMutex;
    bool mScrapeRunning = false;           // worker alive
    bool mScrapeCancel = false;            // cancel requested
    bool mScrapeProgActive = false;        // modal shown
    bool mScrapeDoneFlag = false;          // worker finished -> show summary
    int  mScrapeDone = 0, mScrapeTotal = 0, mScrapeHits = 0, mScrapeFail = 0;
    std::string mScrapeStatus;             // "System / Game" current line (under mutex)
    std::string mScrapeWhat;               // the art downloading now ("Manual"), "" between files
    double mScrapeBps = 0.0;               // download speed of all worker threads, bytes per second
    bool mScrapeIndexDirty = false;        // results merged since index.json was last written
    int64_t mScrapeIndexSavedMs = 0;       // when it was (uptime)
    std::string mScrapeError;              // terminal message (creds missing / network)
    std::vector<std::pair<std::string, ScrapeEntry>> mScrapePending;  // finished -> merge on drain
    std::string mBgScrapeRun;              // the service run this home follows ("" = none)
    std::string mBgScrapeEnded;            // the last run it saw end (not followed again)
    bool mBgScrapeOwn = false;             // this home started it (its manifest is current)
    bool mBgScrapeKick = false;            // its start waits for the last service to exit
    bool mScrapeWasCancelled = false;      // the last run was cancelled (summary wording)
    int64_t mBgScrapeStartMs = 0, mBgScrapePollMs = 0;
    std::string mBgScrapeLostRun;          // a waiting run whose start was lost, started again
    int64_t mBgScrapeLostMs = 0;
    bool mScrapeBusyNote = false;          // the modal was opened by clearing art refused for this one
    std::vector<ScrapeJob> mScrapeQueuedJobs;   // a scrape the queue dialog (theme key 62) asks about
    bool mScrapeQueuedBox = true, mScrapeQueuedFan = true;
    // How much of the service's results file the manifest holds (saved with it in index.json).
    std::string mScrapeJournalRun;
    long long mScrapeJournalOffset = 0;
    // Where scraped covers/fanart + index.json live. Default is internal storage; the Boxart
    // Folder setting (persist.gammaos.scraper.dir) moves it to any browsable folder, typically the
    // SD card. names.json (title overrides) always stays in the default internal dir.
    static constexpr const char* kScrapeDefaultDir = "/data/system/nano_scrape";
    std::string mScrapeCacheDir = kScrapeDefaultDir;
    std::string scrapeIndexPath() const { return mScrapeCacheDir + "/index.json"; }
    bool mScrapeDirAvail = true;           // the configured folder is reachable (external storage mounted)
    int64_t mScrapeDirPollMs = 0;          // availability probe throttle (once a second, uptime)
    std::string mScrapeMoveSrc, mScrapeMoveDst;   // relocation in flight: old/new folder ("" = none)
    bool mScrapeMoveNoRoom = false;   // the relocation found too little space and moved nothing (under mScrapeMutex)
    bool mScrapeMoveMode = false;          // the progress modal fronts a relocation (wording), until the next scrape
    bool mScrapeCopyMode = false;          // ... a folder copy (mScrapeJobTitle; mScrapeSkip = files already there)
    std::string mScrapeJobTitle;
    int  mScrapeSkip = 0;
    void scraperRefreshDir();              // read persist.gammaos.scraper.dir into mScrapeCacheDir
    bool scraperDirAvailable() const;      // configured folder exists (default dir always counts)
    void scraperStorageTick();             // per-frame: load when the SD mounts at boot/hotplug, drop on unplug
    void boxartFolderSelect(const std::string& path);   // folder-picker target 7: relocate the cache
    std::string scraperMissingCredsMsg(bool needSsDev, bool needTgdb);
    void scrapeMoveThreadFunc(std::vector<std::pair<std::string, ScrapeEntry>> entries,
                              std::string src, std::string dst);
    void gsOpenClearArtConfirm();          // Game Systems editor: Clear Boxart confirm (theme key 49)
    void gsClearSystemArt(int sysIdx);     // delete the edited system's covers/fanart + manifest entries
    // Boxart icon textures: lazy per-ROM cover GL textures that replace the generic
    // cartridge icon, freed when leaving the Game category and on sleep/occlusion.
    // ar = width/height. pixel = a PICO-8 cart label: 128x128 pixel art uploaded NEAREST, drawn at an
    // integer multiple of its size (cartPixelSize) so every cart pixel stays square.
    struct BoxTex { GLuint tex = 0; float ar = 1.0f; bool pixel = false; uint64_t used = 0; size_t bytes = 0; };
    std::unordered_map<std::string, BoxTex> mRomBoxartCache;
    uint64_t mBoxartUseClock = 0;        // stamps BoxTex::used, for evicting the least recently drawn
    uint64_t mBoxartFrameClock = 0;      // mBoxartUseClock at the start of this frame
    uint64_t mBoxartPrevFrameClock = 0;  // ... and of the previous one (newer = drawn in the last two frames)
    bool mScrapeBoxartOn = false;        // per-frame cache of scraperBoxartEnabled()
    bool mNdsTileIcon = false;           // per-frame cache of persist.gammaos.nano.nds.tileicon (DS Icons On Tiles)
    // Data-loss guards: a loader sets its flag true when the on-disk file EXISTS but could not be
    // fully read (too big / IO error) or parsed. The matching saver then REFUSES to write, so a
    // failed load never overwrites (destroys) the user's real data. Absent file / clean load = false.
    bool mScrapeIndexLoadErr = false;    // index.json (scraped covers/fanart/metadata)
    bool mRomNamesLoadErr    = false;    // names.json (per-game title overrides)
    bool mMusicCfgLoadErr    = false;    // nano_music.json (music library + playlists)
    bool mVideoCfgLoadErr    = false;    // nano_video.json (video library + playlists)
    bool mPhotoCfgLoadErr    = false;    // nano_photo.json (photo library + playlists)
    GLuint romBoxartTex(const std::string& romPath, float* outAR);
    void scraperFreeBoxart();            // delete all cached cover textures
    void scraperArtTick();               // per-frame: toggle cache + free-on-leave-Game + saDrainArt (both themes)
    // Decode scraped art to a GL texture via stb_image (AImageDecoder silently
    // fails on the scrape PNGs on this device; stb_image works, same as the cinfo
    // bg). maxDim>0 downscales (nearest) to bound VRAM. Render thread only.
    GLuint scraperDecodeTex(const std::string& path, int maxDim, float* outAR);
    // ---- async scraper-art decode (mirrors the photo-viewer worker) ----------
    // ONE worker for ALL scraper art (boxart icons + hover fanart + Information
    // page art): decode RGBA off the render thread, upload GL in saDrainArt() on
    // the render thread, so opening a Game system or Information never hitches.
    // Lazy-started on the first request; fully stopped+joined (zero threads/CPU at
    // idle) by scraperFreeBoxart on leaving Game / occlusion / the 96-cache backstop.
    enum ScrapeArtTarget { SA_BOX = 0, SA_CINFO_FAN, SA_DLG_FAN, SA_DLG_BOX, SA_NDS_FAN, SA_CART_BOX };
    struct SaDecReq { std::string path; int maxDim = 0; int target = 0; std::string key; uint64_t gen = 0; };
    struct SaDecRes { std::string path; int target = 0; std::string key; int w = 0, h = 0; float ar = 1.0f;
                      uint64_t gen = 0; std::vector<uint8_t> px; };
    std::thread mSaDecThread;
    std::mutex mSaDecMutex;
    std::condition_variable mSaDecCv;
    std::deque<SaDecReq> mSaDecQueue;
    std::vector<SaDecRes> mSaDecDone;
    std::set<std::string> mSaDecInFlight;        // dedup key = target-tagged path
    std::atomic<bool> mSaDecStop{false};
    std::atomic<bool> mSaDecStarted{false};
    std::atomic<uint64_t> mSaDecGen{0};          // bumped on teardown to drop stale results
    bool scraperDecodeRGBACpu(const std::string& path, int maxDim, int* outW, int* outH,
                              float* outAR, std::vector<uint8_t>& out);   // GL-free
    void saStartArtWorker();
    void saStopArtWorker();
    void saArtThreadFunc();
    void saRequestArt(const std::string& path, int maxDim, int target, const std::string& key);
    void saDrainArt();                           // render thread: upload + route results
    // persistence + lazy load
    int64_t musicConfigStamp() const;
    bool loadMusicConfig();
    void saveMusicConfig();
    void musicEnsureLoaded();              // parse JSON + NanoAudio init + kick a stale scan (guarded)
    void musicOnCatFocus();                // lazy-load the library when the Music category is focused
    // scan
    void musicScanAsync();                 // detached worker over mMusicFolders
    void musicScanThreadFunc();            // the worker body
    static bool parseM3u(const std::string& m3uPath, MusicPlaylist& out);  // .m3u/.m3u8 -> playlist
    void musicDrainScanResults();          // render-thread: swap in finished results + rebuild
    // folder import (Search for Media Servers)
    void musicOpenFolders();               // push the music folders screen
    void buildMusicFoldersScreen(Ps3Level& out);
    void musicFolderSelect(const std::string& path);
    void musicRemoveFolder(int idx);
    // content
    void buildMusicColumnItems(std::vector<Ps3Item>& out);   // albums + playlists entry for the Music cat
    void buildMusicAlbumSubmenu(int albumIdx, Ps3Level& out);
    void buildMusicPlaylistsScreen(Ps3Level& out);
    void buildMusicPlaylistSubmenu(int plIdx, Ps3Level& out);
    // helpers
    std::vector<int> musicAlbumTrackIndices(const std::string& album) const;  // sorted track idxs
    std::vector<std::string> musicAlbumNames() const;                          // unique, ordered
    // Sort By (Y) on the Music column: orders the albums (within-album track order
    // stays on-disk by file path, deliberately - user 2026-06-15).
    int  mMusicSortField = 0;               // 0 = name, 1 = date (newest album track mtime), 2 = track count
    int  mMusicSortDir   = 1;               // 0 = desc, 1 = asc (name forced asc)
    int64_t musicAlbumNewestMtime(const std::string& album) const;
    std::string musicSortLabelCur() const;
    void musicSortCycleY();                 // Y on the Music column: cycle sort + banner
    // playlists
    void musicCreatePlaylist(const std::string& name);
    void musicAddTrackToPlaylist(int plIdx, const std::string& file);
    void musicRemoveTrackFromPlaylist(int plIdx, const std::string& file);   // drop a track from a playlist + save
    void musicMoveInPlaylist(int plIdx, const std::string& file, int dir);   // reorder a track within a playlist (dir -1/+1) + save
    void musicDeletePlaylist(int plIdx);   // erase a playlist (user JSON entry; m3u-derived also unlinks the .m3u) + save + rebuild
    // The audio engine instance (decode + AAudio + FFT). Lazy: init() on first Music
    // entry; open()/play() on first track play.
    NanoAudioPlayer mMusicPlayer;

    // AAudioStreamBuilder_openStream() BLOCKS until the audio service + HAL are up, which on a
    // cold boot can exceed the render watchdog (8s) and SIGABRT the whole process into a crash
    // loop. So the DSi ambiance stream is opened on a DETACHED thread, never on the render
    // thread. This atomic gates a single in-flight open so the render loop never touches the
    // player while its stream is being (asynchronously) opened.
    std::atomic<bool> mAmbianceOpening{false};
    bool mDirectAmbiancePlaying = false;   // the direct-PCM carousel BGM loop is the active bed (pre-boot)
    bool mDirectWarmHold = false;          // DSi: card held open (silence) from the first frame so the amp is up before the first sound
    bool mPs3DirectHolding = false;        // PS3 theme is holding card0 open through early boot (ps3EarlyAudioTick)
    bool mDirectHandedOff = false;         // the direct-PCM engine's card0 has been handed to the audio HAL (once, post-boot)
    void nanoDirectHandoffTick();  // guaranteed direct-PCM -> HAL handoff even when the home ticks never run (setup wizard)
    // Looping DSi home background ambiance (menu_ambiance.wav), looped by restart-on-ended,
    // stopped off-home.
    NanoAudioPlayer mAmbiancePlayer;
    std::atomic<bool> mAmbiancePlaying{false};
    void ndsAmbianceTick(bool wantOnHome);   // per-frame: start/loop/stop the carousel ambiance
    void ps3EarlyAudioTick();                // per-frame: PS3 theme holds card0 open through early boot (SFX audibility)
    // Pre-boot-complete menu-effect hook shared by the DSi + PS3 themes: while the RK3568 audio server
    // is still initialising, play the effect on the direct-ALSA one-shot path (mixed over any early
    // BGM) instead of blocked AAudio. Returns true if queued to the direct mixer, false -> use AAudio.
    bool earlySfxOneShot(const char* wavName, float master);
    void ndsSfxPlay(int which);              // trigger a DSi interactive SFX (NDS_SFX_* id, NanoMenuPS3Boot.cpp)
    void ndsSfxTick();                       // per-frame: diff menu state -> fire nav/drill/back/launch SFX
    // PS3 XMB cursor/enter sound (SE02_Cursor.wav): a dedicated low-latency SFX player, decoded once
    // and retriggered by a non-blocking atomic on each D-pad/touch move and on item enter. The audio
    // is mixed on the AAudio callback thread so it never touches nano's render performance.
    NanoSfxPlayer mNavSfx;
    std::atomic<bool> mNavSfxLoaded{false};
    std::atomic<bool> mNavSfxOpening{false};
    void ps3NavSound();                      // cursor move (SE02_Cursor); thin wrapper over ps3Sfx
    // Authentic PS3 XMB nav effects (firmware system_plugin: SE02 cursor / SE03 OK / SE04 back /
    // SE05 category / SE08 option / SE09 error). ps3Sfx(PS3_SFX_* id, the namespace-scope Ps3SfxId
    // above) plays via the early-boot direct mixer before the audio server is up, else the pre-loaded
    // AAudio player (NanoMenuPS3Boot.cpp). Enum is at namespace scope so the file-static player array
    // in NanoMenuPS3Boot.cpp can name PS3_SFX_COUNT.
    void ps3Sfx(int which);

    // ==== Video library (R4) ==============================================
    // Mirrors the Music library; HW playback via NanoVideo. Folder import reuses the
    // picker (mFolderPickTarget = 3). The web video section has no library/import/sort
    // (videos are flat demo items); these are a nano addition (user request) on top of
    // a 1:1 player UI. Persisted to nano_video.json. Lazy: parsed on first Video focus.
    struct VideoItem {
        std::string file;        // absolute path
        std::string name;        // display name (filename without extension)
        std::string vcodec;      // "AVC"/"HEVC"/... (column subtitle + Info)
        std::string acodec;
        double durationSec = 0.0;
        int w = 0, h = 0;
        int64_t sz = 0;          // file size in bytes
        int64_t mtime = 0;       // for incremental rescan
        double resumeSec = 0.0;  // last-played position for Resume (0 = none / start fresh)
        bool hasIcon = false;    // a custom Change-Icon poster was set (cached by path, kind 'v')
    };
    // A live stream reference (IPTV channel) carried by the player queue and stored in a
    // playlist alongside local files. Keeps the channel name so it survives an index refresh.
    struct VidStreamRef { std::string name; std::string url; std::string group; };
    struct VideoPlaylist {
        std::string name;
        std::vector<std::string> files;       // references VideoItem.file (user-only; preserved across scans)
        std::vector<VidStreamRef> streams;    // IPTV/live-stream channels added to this playlist
    };
    std::vector<std::string> mVideoFolders;
    std::vector<VideoItem>   mVideos;
    std::vector<VideoPlaylist> mVideoPlaylists;
    int64_t mVideoCfgStamp = -1;
    static const int kVideoMetaVersion = 5;   // 2 = per-file resume position ("pos"); 3-5 = re-probe TS so the descramble path replaces stale "SCRAMBLED" codec labels
    int  mVideoCfgVersion = 0;
    bool mVideoLoaded = false;              // library parsed once (lazy, first Video entry)
    bool mVideoCatsStale = false;           // a scan finished -> rebuild the Video column
    int  mVideoSortField = 0;               // 0 = name, 1 = date (mtime), 2 = duration
    int  mVideoSortDir = 1;                 // 0 = desc, 1 = asc (name forced asc)
    std::mutex mVideoScanMutex;
    std::vector<VideoItem> mVideoScanResults;
    bool mVideoScanReady = false;
    bool mVideoScanRunning = false;
    bool mVideoScanPending = false;
    bool videoStorageReady() const;
    int64_t videoConfigStamp() const;
    bool loadVideoConfig();
    void saveVideoConfig();
    void videoEnsureLoaded();               // parse JSON + kick a stale scan (guarded)
    void videoOnCatFocus();                 // lazy-load when the Video category is focused
    void videoScanAsync();
    void videoScanThreadFunc();
    void videoDrainScanResults();           // render-thread: swap in finished results + rebuild
    void videoRefresh();                    // user-triggered rescan
    void videoOpenFolders();                // push the video folders screen (import)
    void buildVideoFoldersScreen(Ps3Level& out);
    void videoFolderSelect(const std::string& path);
    void videoRemoveFolder(int idx);
    void buildVideoColumnItems(std::vector<Ps3Item>& out);   // scanned video files for the Video cat
    // Video "Change Icon": grab the current playback frame into the shared thumb cache (keyed by
    // path) and show it as the video's column icon instead of the film badge.
    bool   videoIconWrite(const std::string& videoPath, const uint8_t* rgba, int w, int h);  // (def in NanoMenuPhotos.cpp: reaches the static cache helpers)
    GLuint videoIconRead(const std::string& videoPath);                                       // (def in NanoMenuPhotos.cpp)
    bool   videoIconExists(const std::string& videoPath);   // 'v' poster present on disk? (def in NanoMenuPhotos.cpp)
    GLuint videoIconTexCached(const std::string& videoPath);   // memoised (caches misses as 0 too)
    void   videoIconInvalidate(const std::string& videoPath);  // drop + free a cached icon texture
    void   videoIconGrabCurrentFrame();                        // render thread, GL current: FBO grab of the live frame
    std::map<std::string, GLuint> mVidCustomIconCache;         // custom Change-Icon poster: video path -> loaded tex (0 = miss)
    bool mVidIconGrabPending = false;                          // a Change-Icon confirm is waiting for the next rendered frame
    std::string mVidIconGrabPath;                              // video PATH whose icon to set (re-resolved at grab time; index would go stale on a rescan)
    // Auto thumbnail generation: one-off decode of a single frame per fresh video, cached to the
    // shared 'v' thumb blob on disk (persists so it never regenerates for unchanged media). The
    // frame is only reachable via NanoVideo's GL path, so this is an INCREMENTAL render-thread state
    // machine driving a dedicated headless decoder (never mVideoTest) one video at a time across
    // frames. Gated hard on "no live decoder" - the SoC has ONE HW video decoder (see wpVideoTick).
    void videoThumbTick();                                     // render thread, GL current: advance the auto-thumb machine one step
    bool videoThumbContended();                                // another owner holds (or is about to take) the HW decoder
    void videoThumbEnqueueMissing();                           // after a drain: queue every fresh video with no 'v' poster on disk
    void videoThumbAbandon();                                  // cancel + async-free the headless decoder + reset to IDLE (decoder contended)
    enum VidThumbState { VT_IDLE = 0, VT_OPENING, VT_WAIT_FRAME, VT_CLOSE };
    NanoVideo* mVidThumbDec = nullptr;                         // headless single-frame decoder (separate from mVideoTest / mWpVideoTop)
    std::vector<std::string> mVidThumbQueue;                   // video paths still needing a poster (drained front to back)
    size_t mVidThumbQIdx = 0;                                  // next queue entry to process (queue is append-only within a session)
    int mVidThumbState = VT_IDLE;                              // VidThumbState
    std::string mVidThumbPath;                                 // the path currently being decoded
    double mVidThumbDeadline = 0.0;                            // mEffectTime past which the current decode is abandoned (bad/DRM/slow file)
    std::thread mVidThumbOpenThread;                           // blocking openAsyncRun worker for the headless decoder
    std::atomic<bool> mVidThumbOpenDone{false};               // worker published its result
    std::atomic<bool> mVidThumbOpenOk{false};                // worker open succeeded
    // Video playlists (a nano addition; the web video section has none) - mirror music/photo.
    void buildVideoPlaylistsScreen(Ps3Level& out);
    void buildVideoPlaylistSubmenu(int plIdx, Ps3Level& out);
    void videoCreatePlaylist(const std::string& name);
    void videoAddToPlaylist(int plIdx, const std::string& file);
    void videoRemoveFromPlaylist(int plIdx, const std::string& key, bool isStream);   // drop a file/stream from a video playlist + save
    void videoMoveInPlaylist(int plIdx, const std::string& key, bool isStream, int dir); // reorder a file/stream within its vector + save
    void videoDeletePlaylist(int plIdx);   // erase a video playlist (JSON-only) + save + rebuild
    bool  mVidPlChooserActive = false;
    std::vector<std::string> mVidPlChooserOpts;   // "New Playlist..." + existing names
    int   mVidPlChooserSel = 0;
    std::string mVidPlChooserFile;                // video file being added
    bool  mVidPlChooserIsStream = false;          // true = adding an IPTV channel (mVidPlChooserStream)
    VidStreamRef mVidPlChooserStream;             // the channel being added (when mVidPlChooserIsStream)
    float mVidPlChooserAnim = 0.0f;
    void vidOpenAddChooser(const std::string& file);
    void vidPlChooserMove(int dir);
    void vidPlChooserSelect();
    void vidPlChooserCancel();
    void drawVidPlChooser();
    void videoSortApply();                  // re-sort mVideos by the current field+dir
    void videoSortCycleY();                 // Y on the Video column: cycle sort + banner
    std::string videoSortLabelCur() const;
    bool videoSortLess(int a, int b) const;
    void videoAddStreamToPlaylist(int plIdx, const VidStreamRef& s);   // add an IPTV channel to a playlist
    void vidOpenAddStreamChooser(const VidStreamRef& s);               // "Add to Playlist" for a channel

    // ==== IPTV (live channels, nano addition) =============================
    // Channels parsed from the community iptv-org index.m3u (downloaded via device curl,
    // cached under /data, refreshed every 24h, cached copy kept if a refresh fails). Gated
    // by a first-use disclaimer (persist.gammaos.nano.iptv.agreed) and a Video Settings
    // toggle (persist.gammaos.nano.iptv). Grouped by the m3u group-title; activating a
    // channel streams its HLS URL through the video player (openIptvStream).
    // Two-level hierarchy: Group (the m3u group-title - a country or content group) ->
    // Sub (an alphabetical bucket, only when a group is large) -> Channels. A group small
    // enough to list directly has a single unnamed sub and skips straight to its channels.
    struct IptvChannel { std::string name; std::string url; std::string group; };
    struct IptvCountry { std::string name; std::vector<int> channels; };  // a sub-bucket (indices into mIptvChannels)
    struct IptvCat     { std::string title; std::vector<IptvCountry> countries; int total = 0; };
    std::vector<IptvChannel> mIptvChannels;          // published flat list (UI reads under mIptvMutex)
    std::vector<IptvCat>     mIptvCats;              // category -> country -> channels (sorted)
    std::mutex mIptvMutex;                            // guards the published channels/cats + status
    std::atomic<bool> mIptvScanRunning{false};       // a fetch/parse worker is in flight
    std::atomic<bool> mIptvReady{false};             // channels are loaded + usable
    std::atomic<bool> mIptvDirty{false};             // worker finished -> rebuild the open IPTV screen
    std::string mIptvStatus;                          // "Loading..."/error text (guarded by mIptvMutex)
    int mIptvEnabledCache = -1;                       // last-seen persist.gammaos.nano.iptv (live toggle-hide)
    void iptvOpen();                                  // disclaimer-passed entry: load + push the categories screen
    void iptvEnsureLoaded();                          // lazy: kick the (24h) refresh worker (async publish)
    void iptvEnsureLoadedSync();                      // parse the on-disk cache inline if not loaded (for search), then kick refresh
    void iptvFetchAsync();                            // spawn the download/parse worker (guarded)
    void iptvFetchThreadFunc();                       // worker: 24h-gated download -> parse -> publish (keep cache on fail)
    bool iptvDownloadIndex(const std::string& dst);   // curl the index.m3u to a temp file (true on non-empty success)
    bool iptvParseM3u(const std::string& path,
                      std::vector<IptvChannel>& outCh,
                      std::vector<IptvCat>& outCat);
    void iptvPublish(std::vector<IptvChannel>& ch, std::vector<IptvCat>& cat);
    void iptvDrain();                                 // render thread: rebuild the open IPTV screen; live toggle-hide
    void buildIptvCategoriesScreen(Ps3Level& out);    // IPTV -> category list (screenKind IPTV_GROUPS)
    void buildIptvCountrySubmenu(int catIdx, Ps3Level& out);          // category -> country list
    void buildIptvChannelSubmenu(int catIdx, int countryIdx, Ps3Level& out);  // (category,country) -> channels
    void openIptvStream(const std::vector<VidStreamRef>& queue, int startIdx);  // play a channel (queue = the group)
    // (vidOpenStream/vidOpenTitle are now the worker-thread vidOpenStreamRun/vidOpenTitleRun above)

    // ==== Internet Radio (live audio stations, nano addition - Music category) ============
    // Mirrors IPTV but audio-only: stations parsed from a community m3u (default the Pulham
    // Internet-Radio HQ list), downloaded via device curl, cached under /data, refreshed every
    // 24h (cached copy kept on a failed refresh). Gated by a first-use disclaimer
    // (persist.gammaos.nano.radio.agreed) and a Music Settings toggle (persist.gammaos.nano.radio).
    // The user can override the playlist URL (persist.gammaos.nano.radio.url). Grouped by the m3u
    // group-title; lists with no groups become one "All Stations" group split into alpha buckets.
    // Activating a station streams its URL through the MUSIC player (openRadioStation) so it gets
    // the Now-Playing screen, visualizers, clock-bar indicator and background playback.
    struct RadioStation { std::string name; std::string url; std::string group; };
    struct RadioBucket  { std::string name; std::vector<int> stations; };  // a sub-bucket (indices into mRadioStations)
    struct RadioCat     { std::string title; std::vector<RadioBucket> buckets; int total = 0; };
    std::vector<RadioStation> mRadioStations;         // published flat list (UI reads under mRadioMutex)
    std::vector<RadioCat>     mRadioCats;             // group -> bucket -> stations (sorted)
    std::mutex mRadioMutex;                            // guards the published stations/cats + status
    std::atomic<bool> mRadioScanRunning{false};       // a fetch/parse worker is in flight
    std::atomic<bool> mRadioReady{false};             // stations are loaded + usable
    std::atomic<bool> mRadioDirty{false};             // worker finished -> rebuild the open radio screen
    std::string mRadioStatus;                          // "Loading..."/error text (guarded by mRadioMutex)
    int mRadioEnabledCache = -1;                       // last-seen persist.gammaos.nano.radio (live toggle-hide)
    void radioOpen();                                  // disclaimer-passed entry: load + push the stations screen
    void radioEnsureLoaded();                          // lazy: kick the (24h) refresh worker (async publish)
    void radioEnsureLoadedSync();                      // parse the on-disk cache inline if not loaded (for search), then kick refresh
    void radioFetchAsync();                            // spawn the download/parse worker (guarded)
    void radioFetchThreadFunc();                       // worker: 24h-gated download -> parse -> publish (keep cache on fail)
    bool radioDownloadIndex(const std::string& dst);   // curl the m3u to a temp file (true on non-empty success)
    bool radioParseM3u(const std::string& path,
                       std::vector<RadioStation>& outSt,
                       std::vector<RadioCat>& outCat);
    void radioPublish(std::vector<RadioStation>& st, std::vector<RadioCat>& cat);
    void radioDrain();                                 // render thread: rebuild the open radio screen; live toggle-hide
    void buildRadioRootScreen(Ps3Level& out);          // Internet Radio -> group/bucket/station list (screenKind RADIO_STATIONS)
    void buildRadioBucketSubmenu(int catIdx, Ps3Level& out);                 // group -> bucket list
    void buildRadioStationSubmenu(int catIdx, int bucketIdx, Ps3Level& out); // (group,bucket) -> stations
    void openRadioStation(const std::vector<Ps3Item>& list, int listSel);    // play a station in the music player (queue = surrounding stations)

    // ---- Video player screen (R4 V2) - the full-screen 1:1 player (web drawVideoPlayer)
    bool mVidActive = false;                // the player is up
    std::vector<int> mVidList;              // mVideos indices in the player queue (all column videos)
    int  mVidIdx = 0;                       // current index into mVidList (or mVidStreamList for IPTV)
    // IPTV live-stream session: when true the player queue is mVidStreamList (channel URLs)
    // instead of mVidList/mVideos. Streams are live (no duration / seek / Resume).
    bool mVidIsStream = false;
    std::vector<VidStreamRef> mVidStreamList;
    float mVidEnterRaw = 0.0f;              // linear 0..1 (~400ms)
    float mVidEnterT = 0.0f;                // smoothstep of mVidEnterRaw (multiplies every layer's alpha)
    bool mVidPlaying = true;
    int  mVidScreenMode = 0;                // 0 Normal,1 Full Screen,2 Original,3 Zoom,4 Double Scale
    float mVidHintUntil = 0.0f;             // OSD bar auto-hide deadline (mEffectTime seconds)
    bool mVidOsd = false;                   // persistent Display toggle (keeps the bar visible)
    std::string mVidTransient; float mVidTransientUntil = 0.0f;   // top-center flash (FF/Rew/etc.)
    std::string mVidDispMode; float mVidDispModeUntil = 0.0f;     // screen-mode pill
    // Monotonic wall-clock safety net for the two timed OSD strings: mEffectTime is CLOCK_BOOTTIME mod
    // 500s (wraps) and only refreshes on a rendered frame, so a set-near-the-wrap or a stalled framebuffer
    // could leave "Audio: ..."/"Loading subtitles..." stuck. videoTick clears each past this real-time cap.
    int64_t mVidDispModeShownMs = 0; std::string mVidDispModePrev;
    int64_t mVidTransientShownMs = 0; std::string mVidTransientPrev;
    // resumeChoice: -1 = ask (direct Enter shows the Resume prompt for a watched title);
    // 1 = resume now (option-menu "Resume", no prompt); 0 = play from the start (option-menu
    // "Play from Beginning", caller clears the bookmark first).
    void openVideoPlayer(const std::vector<Ps3Item>& list, int listSel, int resumeChoice = -1);
    void closeVideoPlayer();                // release the decoder + fade out
    void videoTick();                       // enter/leave ease + end-of-stream auto-advance
    bool renderVideoPlayer();               // draws the player; true = it owns the screen
    void videoHardFree(bool sync = false);  // full decoder teardown; sync=true blocks (dtor), else async (sleep/occlusion)
    void vidSeek(double deltaSec);          // relative seek (D-pad L/R)
    void vidStepTitle(int dir);             // previous / next video in the queue
    void vidTogglePlay();
    void vidShowTransient(const std::string& text, float ms);

    // ---- Video transport extras (web vidScan/vidSlow/vidStepFrame/vidStop) -----
    // NanoVideo plays only at 1x, so every non-1x rate is timer-driven in videoTick:
    // native playback is paused and the position is seek()ed by rate*dt each frame.
    double mVidRate = 1.0;                   // scan/slow rate (1 = normal; <0 = reverse)
    bool   mVidStopped = false;             // Stop pressed (paused at t=0)
    int    mVidRepeat = 0;                  // 0 off,1 on,2 title,3 A-B,4 folder
    double mVidAbA = -1.0, mVidAbB = -1.0;  // A-B repeat points (seconds; -1 = unset)
    float  mVidVolume = 1.0f;               // 0..1 (applied to the video's audio track)
    int    mVidVolLevel = 4;                // Volume Control bar level -4..+4 (web v.volLevel); vol=(lvl+4)/8 on change. Seeded to +4 so the bar (9/9 full) agrees with the default applied audio (mVidVolume=1.0); level 0 would show "Normal"/0.5 while audio played at 1.0.
    // The video file's audio track: a second HW audio engine (AMediaExtractor/AMediaCodec ->
    // AAudio) opened on the same file, the picture is the master clock and the audio resnaps
    // when it drifts > 0.3s (web vidSyncAux). Lazy: opened on play, released on leave.
    NanoAudioPlayer mVidAudio;
    bool   mVidHasAudio = false;
    bool   mVidAudioStarted = false;        // audio held until the first video frame (avoids warmup desync)
    // Audio-track switch runs mVidAudio.release()+open() on a DETACHED worker, not the render thread:
    // release() joins the decode thread, which can be parked for seconds in a slow AMediaExtractor_seekTo
    // after a far seek. Doing it inline froze the UI (and used to trip the render watchdog). The render
    // thread claims the flag + spawns the worker and returns; vidAudioSwitchJoin() settles it (waits the
    // flag) before any player teardown/close so the worker's release() can never race the close release().
    std::atomic<bool> mVidAudSwitchDone{true};   // false while a switch worker is in flight
    void vidAudioSwitchJoin();                    // wait out an in-flight async audio-track switch (watchdog-exempt)
    float  mVidAudioResyncT = 0.0f;         // last A/V resync time (cooldown so resync never tight-loops)
    // .ts runs through the in-process single-pass demuxer: ONE read pointer feeds both the
    // HW video codec (NanoVideo fed mode) and the audio (liba52 -> mVidAudio fed ring), so
    // A/V stay in lockstep with no system extractor. Everything else uses the normal path.
    NanoTsDemux mVidTsDemux;
    bool   mVidTsMode = false;              // current title's PICTURE is fed by mVidTsDemux
    bool   mVidTsAudio = false;             // current title's audio is fed by mVidTsDemux
    NanoAviDemux mVidAviDemux;              // RIFF/AVI demuxer (NDK extractor has no AVI support)
    bool   mVidAviMode = false;            // current title's PICTURE is fed by mVidAviDemux (audio TODO)
    // The .ts video track's real AMediaFormat (captured by vidBuildTracks from the system
    // extractor): handed to NanoVideo::openFed so the HW decoder gets the full format (csd,
    // colour aspects) and cold-starts reliably. Owned here; freed right after openFed/on close.
    AMediaFormat* mVidTsVideoFmt = nullptr;
    // ---- multiple audio tracks + subtitles (built per opened title; web audioTracks/subList) ----
    // One ASS/SSA [V4+ Styles] entry (parsed from the Matroska CodecPrivate header). Colours are
    // pre-decoded from ASS &HAABBGGRR (BGR order, inverted alpha) into straight rgba 0..1.
    struct AssStyle {
        std::string name;                      // style key (matched by the event Style field)
        float fontSizePx = 63.0f;              // PlayRes px (V4+ "Fontsize")
        float pr = 1, pg = 1, pb = 1, pa = 1;  // PrimaryColour rgba
        float ro = 0, go = 0, bo = 0, ao = 1;  // OutlineColour rgba
        float outlinePx = 2.0f;                // "Outline" thickness (PlayRes px)
        bool  bold = false, italic = false;
        int   alignment = 2;                   // numpad 1..9 (SSA legacy converted at parse)
        int   marginL = 0, marginR = 0, marginV = 0;
    };
    // A rendered subtitle cue. For plain tracks (SRT/CEA-608/DVB) only t/d/text are set and styled=false
    // so the renderer uses the legacy bottom-centre path. For ASS the worker fills the typeset layout.
    struct VidCue {
        double t = 0.0, d = 0.0; std::string text;   // start, duration, display text (tags removed)
        bool  styled = false;                   // true => use the positioned/coloured ASS path
        int   alignment = 2;                    // numpad 1..9
        bool  hasPos = false;                   // \pos/\move present => posX/posY anchor, ignore margins
        float posX = -1.0f, posY = -1.0f;       // PlayRes px anchor when hasPos
        float fontSizePx = 0.0f;                // PlayRes px; 0 => style/default
        float pr = 1, pg = 1, pb = 1, pa = 1;   // primary fill rgba
        float ro = 0, go = 0, bo = 0, ao = 1;   // outline rgba
        float outlinePx = 0.0f;                 // PlayRes outline thickness
        bool  bold = false, italic = false;     // flags only (no bold/slant glyph primitive)
        int   marginL = 0, marginR = 0, marginV = 0;   // effective margins (ignored when hasPos)
        int   layer = 0, order = 0;             // draw order: layer asc, then order (ReadOrder/index) asc
    };
    struct VidAudTrk { int idx = 0; std::string name; };           // idx = extractor track index
    struct VidSubTrk { std::string name; bool external = false; std::string file; int embIdx = -1;
                       std::vector<VidCue> cues; bool dvb = false; int dvbPid = -1;
                       bool cea608 = false; int ccChannel = 0;
                       // Matroska embedded text sub: the track is listed cheaply at open, but its cues
                       // (a full-file cluster walk) are loaded LAZILY on first selection so the open never
                       // stalls. mkvNum = the MKV track number (>=0), mkvAss = ASS/SSA vs SubRip.
                       int mkvNum = -1; bool mkvAss = false;
                       // ASS header (Matroska CodecPrivate 0x63A2), parsed once at header build on the render
                       // thread; the lazy-cue worker reads a value copy of this (never indexes mVidSubTracks).
                       bool assParsed = false;
                       int  assPlayResX = 1440, assPlayResY = 1080;   // [Script Info] PlayResX/Y (ASS default 384x288 if absent)
                       int  assWrapStyle = 0;                          // WrapStyle (0/3 => \n behaves as a space)
                       bool assScaledBorder = true;                    // ScaledBorderAndShadow (default yes for V4+)
                       std::map<std::string, AssStyle> assStyles;      // keyed by Style Name
                     };  // live line-21 caption (via NanoTsDemux)
    std::vector<VidAudTrk> mVidAudTracks;   // all audio tracks in the current file
    std::vector<VidSubTrk> mVidSubTracks;   // embedded text subs + external SRT/VTT sidecars
    mutable std::vector<VidCue> mVidCcCues; // live CEA-608 cues snapshot (refreshed on read)
    int mVidAudCur = 0;                      // current index into mVidAudTracks
    int mVidSubCur = -1;                     // -1 = Off, else index into mVidSubTracks
    void vidBuildTracks(const std::string& file);   // enumerate audio + embedded text subs + sidecars
    void vidReadEmbeddedCues(const std::string& file, int trackIdx, std::vector<VidCue>& out);
    void vidReadMkvEmbeddedSubs(const std::string& file);   // list Matroska sub tracks at open (extractor exposes none)
    void vidLoadMkvSubCues(int subIdx);                     // lazily load one MKV sub track's cues (bg thread)
    void vidPublishMkvSubCues();                            // render thread: adopt a finished lazy cue load
    void drawAssCue(const VidCue& c, float W, float H, int prX, int prY,
                    float sx, float sy, bool scaledBorder, float et);   // positioned/coloured ASS cue
    std::vector<std::string> wrapTextToWidth(const std::string& s, float fs, float maxW);   // greedy word-wrap (keeps \n)
    // wrapTextToWidth, stepping fs and lineH down together until the lines fit maxH (never below minFs).
    std::vector<std::string> wrapTextToFit(const std::string& s, float& fs, float& lineH, float maxW, float maxH, float minFs);
    // ASS header + per-event parse (static: pure logic, no member state; access the private nested
    // VidCue/VidSubTrk/AssStyle types). parseAssHeader runs at track build (render thread);
    // parseAssEvent runs on the lazy-cue worker thread from a value-copied style snapshot.
    static void parseAssHeader(const std::string& codecPrivate, const std::string& codecId, VidSubTrk& out);
    static void parseAssEvent(const std::string& rawBlock, const std::map<std::string, AssStyle>& styles,
                              int wrapStyle, int fileOrder, VidCue& c);
    // Lazy MKV subtitle cue load (see vidLoadMkvSubCues): the bg worker fills mVidSubLoadCues then flips
    // mVidSubLoadReady; the render thread moves it into the track and clears busy. One load at a time.
    std::atomic<bool>   mVidSubLoadBusy{false};
    std::atomic<bool>   mVidSubLoadReady{false};
    int                 mVidSubLoadIdx = -1;
    std::vector<VidCue> mVidSubLoadCues;
    void vidSetAudioTrack(int ordinal);             // switch the active audio track (re-opens mVidAudio)
    void vidOpenTitleAudio(const std::string& file);   // open audio for a NON-.ts title (mVidAudio)
    // (vidOpenTitle is now vidOpenTitleRun above - runs on the open worker thread)
    void vidCloseTitleAudio(bool async = true);     // stop the demux (if any) + release mVidAudio (async off the render thread)
    void vidAudioSeek(double sec);                  // seek the audio, routed to the demux for .ts
    double vidDuration() const;                     // duration (s): demux for .ts, else NanoVideo
    std::vector<VidCue> vidParseSrt(const std::string& text);   // SRT/VTT cue parser (web vidParseCues)
    const std::vector<VidCue>* vidActiveSubCues() const;        // cues for the selected sub track, or null
    double mVidScanLastTick = -1.0;         // wall-clock anchor for timer-driven scan
    double mVidScanPos = 0.0;               // commanded scan clock (decoder position lags + snaps to keyframes)
    double mVidLastPos = -1.0;              // buffering detection: last seen playback position
    float  mVidLastPosT = 0.0f;            // time the position last advanced
    bool   mVidBuffering = false;           // no new frame while playing -> show the buffering spinner
    // ---- Resume at last timestamp (per-file, persisted in nano_video.json "pos") ----
    bool   mVidResumeDirty = false;         // resume position changed since the last save
    double mVidResumeSaveT = -1.0;          // last debounced save time (mEffectTime)
    bool   mVidResumeAsk = false;           // the Resume / Play-from-beginning prompt is up
    int    mVidResumeSel = 0;               // 0 = Resume (default), 1 = Play from beginning
    double mVidResumeAskSec = 0.0;          // the saved position offered by the prompt
    void vidCaptureResume();                // store the current position onto the playing VideoItem
    void vidResumeConfirm();                // apply the highlighted Resume-prompt choice
    void drawVideoResume(float et);         // render the Resume prompt
    // Video-player modal dialog (web drawDialog confirm/info/busy), mirroring the home XMB
    // message dialog: Delete + Change Icon confirms, the Creating-icon/result chain, and the
    // no-audio / no-subtitle / Go-To-over-limit errors (web shows centred modals, not the
    // transient flashes nano used). Exists only while open; no allocations.
    bool   mVidDlgActive = false;
    int    mVidDlgKind = 0;                 // 0 = info (OK), 1 = confirm (Yes/No), 2 = busy (no button)
    std::string mVidDlgBody;
    int    mVidDlgSel = 1;                  // confirm: 0 = Yes, 1 = No (web defaultSel = No)
    int    mVidDlgYesAct = 0;               // confirm Yes action: 1 = Delete, 2 = Change Icon
    float  mVidDlgBusyUntil = 0.0f;         // busy auto-advance time (Creating icon... -> result)
    void   vidDlgInfo(const std::string& body);
    void   vidDlgConfirm(const std::string& body, int yesAct);
    void   vidDlgActivate();                // Cross on the dialog
    void   vidDlgBack();                    // Circle/Back on the dialog
    void   drawVideoDialog(float et);       // render the modal
    void vidStop();                         // pause + rewind to 0
    void vidScan(int dir);                  // Fast Forward / Fast Reverse (steps 1.5/10/30/120)
    void vidSlow(int dir);                  // Slow Forward / Slow Reverse (+-0.5)
    void vidStepFrame(int dir);             // single-frame step (pause + seek 1/30s)
    void vidFlash(int dir);                 // Instant Replay / Advance (+-15s)
    void vidBeginning();                    // Return to Beginning (or Previous if near start)

    // ---- Video control panel (VID_CP, web drawVideoPanel) ----------------------
    bool  mVidCpOpen = false;
    int   mVidCpSel = 0;                    // index into kVidCp
    int   mVidCpSelPrev = -1;
    float mVidCpAnimStart = -1.0f;          // open slide/fade start
    float mVidCpFocusStart = -1.0f;         // focus-change ease start
    bool  mVidCpClosing = false;
    float mVidCpCloseStart = -1.0f;
    float mVidCpPressStart = -1.0f;         // button invoke flash
    int   mVidCpPressSel = -1;
    // Panel submenu (screen mode / repeat / volume / AV settings / audio / subtitle)
    bool  mVidSubOpen = false;
    int   mVidSubKind = 0;                  // 0 screenmode,1 repeat,2 volume,4 audio,5 subtitle
    std::string mVidSubLabel;
    std::vector<std::string> mVidSubOpts;
    int   mVidSubSel = 0;
    bool  mVidPanelTouch = false;           // panel opened by a screen tap -> enlarge icons
    // Debounced touch scrub: NanoAudioPlayer::seek() joins+restarts the decode thread
    // (heavy, blocking on the render thread), so a live drag that seeks every move
    // freezes the render heartbeat and trips the 8s watchdog. Instead the drag only
    // previews a pending target; the real vidSeek fires once the finger settles/lifts
    // (mirrors the music player's mMpSeekPending debounce).
    bool   mVidScrubPending = false;
    double mVidScrubTarget = 0.0;           // pending seek target (seconds)
    float  mVidScrubInputT = 0.0f;          // mEffectTime of the last scrub input
    void vidPanelToggle();                  // Triangle: open/close the control panel
    void vidPanelOpen(bool byTouch = false);
    float vidPanelUi();                     // vidUiScale, enlarged when opened by touch
    void vidPanelClose();
    void vidPanelMove(int dx, int dy);      // spatial grid nav (+ submenu wrap)
    void vidPanelActivate();                // Cross on the focused control / submenu row
    void vidPanelBack();                    // Circle: close submenu, else close panel
    void vidSubBuild(int kind);             // populate mVidSubOpts/Sel for a control
    void vidSubConfirm();                   // apply the highlighted submenu row
    void drawVideoPanel(float closeT);      // render the panel (closeT>=0 drives close anim)
    // Shared XMB-style media option dialog (video + photo option lists): a solid dark
    // rounded panel centred on screen with a title + touch-capable rows. One look/behaviour
    // for every media list. With pick, its rows are touch zones: a tap picks the row, a tap outside
    // the panel runs dismiss, and a drag over a windowed list runs step (as Up/Down would).
    struct MediaOptDlg { float cx, cy, top, w, h, titleH, rowH, titleFs, rowFs, pad; int n, first, vis; };
    MediaOptDlg mediaOptDlgGeom(const char* title, const std::vector<std::string>& opts, int sel);
    void drawMediaOptDialog(const char* title, const std::vector<std::string>& opts, int sel, float alpha,
                            std::function<void(int)> pick = nullptr, std::function<void()> dismiss = nullptr,
                            TouchZones::Step step = nullptr);
    GLuint vidIcon(int n);                  // load+cache a videoplayer icon (NanoMenuPS3Icons.cpp)
    float  vidIconAR(int n);                // cached aspect ratio (w/h)
    std::map<int, GLuint> mVidIconCache;
    std::map<int, float>  mVidIconAR;

    // ---- Go To (in-player H:MM:SS seek picker, web drawVideoGoTo) ---------------
    bool mVidGoToOpen = false;
    int  mVidGoToH = 0, mVidGoToM = 0, mVidGoToS = 0;
    int  mVidGoToField = 0;                 // 0 h, 1 m, 2 s
    void vidGoToOpen();
    void vidGoToMove(int dx);               // L/R: change field
    void vidGoToAdjust(int dy);             // U/D: change the focused digit
    void vidGoToActivate();                 // Cross: seek to the chosen time
    void vidGoToClose();
    void drawVideoGoTo();

    // ---- Scene Search (chapter grid, web vidScene* / drawVideoScene) ------------
    // Chapters parsed directly from the container (mp4/mov QT chapter track + Nero
    // chpl; mkv EBML Chapters) since the NDK extractor does not expose chapters.
    struct VidChapter { double t = 0.0; std::string title;
                        GLuint thumbTex = 0; int thumbW = 0, thumbH = 0; };   // per-chapter preview frame (render-thread GL)
    std::vector<VidChapter> mVidChapters;   // sorted by time; empty = "No chapters"
    void vidParseChapters(const std::string& file);   // fill mVidChapters from the file
    // Chapter preview thumbnails: the SoC has a single HW video decoder that mVideoTest holds while a
    // clip plays, so a chapter's frame cannot be decoded independently. Instead the current chapter's
    // thumbnail is grabbed opportunistically from the live mVideoTest frame during playback (zero extra
    // decode), so previews fill in for chapters as they are watched or jumped to. Render thread only.
    int  vidCurrentChapter(double pos) const;      // index of the chapter containing pos (-1 if none)
    void vidCaptureChapterThumb(int idx);          // grab mVideoTest's current frame into chapter idx
    void vidFreeChapterThumbs();                   // glDeleteTextures all chapter thumbs (render thread)
    void vidJumpToChapter(int idx);                // seek player+audio to chapter idx (test hook / shared)
    // Optional PRE-CACHE: at open, before playback, seek the player through each chapter to grab a preview,
    // bounded by an overall wall-clock timeout so a slow MKV seek can never hang startup (the rest fill in
    // opportunistically during playback). Gated by persist.gammaos.nano.video.chapter_precache (default on).
    void   vidChapterPrecacheBegin(double resumeAt);
    void   vidChapterPrecacheTick();               // render thread: drives the open-time seek sweep
    int    mVidChapPreState = 0;                    // 0 off, 1 seek, 2 settle-then-grab
    int    mVidChapPreIdx = 0;
    double mVidChapPreDeadline = 0.0;               // overall budget (mEffectTime)
    double mVidChapPreSeekAt = 0.0;                 // when the current chapter seek was issued
    double mVidChapPreLandedAt = -1.0;              // when the seek reached the target (position near it)
    double mVidChapPreResumeAt = 0.0;              // position to resume playback at when done
    bool  mVidSceneOpen = false;
    bool  mVidSceneClosing = false;
    int   mVidSceneSel = 0;
    int   mVidSceneSelPrev = 0;
    int   mVidSceneTopRow = 0;          // first grid row shown above the footer
    float mVidSceneAnimStart = -1.0f;       // open fade start
    float mVidSceneCloseStart = -1.0f;
    float mVidSceneFocusStart = -1.0f;      // selection-change ease start
    void vidSceneOpen();                    // Scene Search: open the chapter grid (or "No chapters")
    void vidSceneClose();
    void vidSceneMove(int dx, int dy);      // grid nav (L/R = +-1 wrap, U/D = +-cols)
    void vidSceneActivate();                // Cross: seek to the focused chapter
    void drawVideoScene(float closeT);      // render the grid (closeT>=0 drives close anim)

    // ---- DVB / bitmap subtitles (NanoDvbSub) -----------------------------------
    // Decoded off the render thread (a large .ts can take seconds to stream + the
    // render watchdog would abort on a synchronous decode). Lazy: created only when
    // the user selects the DVB track, the whole pipeline freed on deselect/close.
    NanoDvbSub*       mVidDvb = nullptr;        // decoded subtitle timeline (or null)
    std::thread       mVidDvbThread;            // background decode worker
    std::atomic<bool> mVidDvbReady{false};      // timeline decoded + safe to read
    std::atomic<bool> mVidDvbLoading{false};    // decode in progress (shows a hint)
    std::atomic<bool> mVidDvbAbort{false};      // cancel a long decode on deselect/teardown
    std::string       mVidDvbPath;              // .ts being decoded
    int               mVidDvbPid = -1;          // subtitle PID (from probe)
    GLuint            mVidDvbTex = 0;            // currently uploaded region bitmap
    int               mVidDvbTexRegion = -1;    // which region index mVidDvbTex holds
    void vidDvbSelect(const std::string& file, int pid);   // start the background decode
    void vidDvbFree();                                     // join the worker + free all

    // ---- Now-Playing screen state (control panel + visualizers in Phase 3-5) ----
    bool mMpActive = false;            // the Now-Playing fullscreen is up
    std::vector<int> mMpQueue;         // track indices (into mMusicTracks) being played
    std::vector<std::string> mMpQueueFiles;  // parallel file paths so the queue survives a
                                       // library reload (mMusicTracks is replaced by a finished
                                       // rescan / external edit; the indices would otherwise dangle)
    int  mMpIdx = 0;                   // position in mMpQueue
    int  mMpVis = 0;                   // 0 = XMB Waves, 1 = Canyon, 2 = Globe, 3.. = wallpaper effects
    float mMpCanyonAlpha = 0.0f;       // Waves<->Canyon crossfade (eased 0..1 over ~0.5s)
    float mMpGlobeAlpha = 0.0f;        // Globe crossfade (eased 0..1 over ~0.5s)
    float mMpGlobeLon = 0.0f;          // auto-rotating longitude for the Globe visualizer
    bool mMusicResumeShown = false;    // is the Quick Menu "Resume Audio Player" item present
    int  mMpRepeat = 0;                // 0 off / 1 all / 2 one
    bool mMpShuffle = false;
    std::vector<int> mMpOrder;         // playback order (indices into mMpQueue, or mMpRadioQueue when radio)
    // Internet Radio session: when true the player queue is mMpRadioQueue (live station URLs),
    // not mMusicTracks. The Now-Playing screen shows station name / group / LIVE (no seek bar,
    // no FF/REW, no auto-advance on end); L/R step stations. mMpIdx indexes mMpRadioQueue.
    bool mMpIsRadio = false;
    std::vector<RadioStation> mMpRadioQueue;   // the playing station queue (the surrounding group)
    bool mMpRadioErrShown = false;             // one-shot: "Could not open this station." already shown
    void openMusicPlayer(const std::vector<Ps3Item>& list, int listSel);
    void musicRemapQueueAfterReload(); // re-resolve mMpQueue indices by file path after mMusicTracks is replaced
    void closeMusicPlayer();           // full stop + release the audio engine
    void minimizeMusicPlayer();        // hide the Now-Playing UI but keep audio playing
    void freeMusicVisGl();             // free Canyon/Globe GL immediately (park point; idempotent)
    void resumeMusicPlayer();          // reopen the Now-Playing screen on the live queue
    void mpPlayCurrent();
    void mpRebuildOrder();
    void mpStep(int dir, bool isAuto);
    void mpNext();
    void mpPrev();
    void musicTick();                  // per-frame: auto-advance on EOS + fades
    void mpAutoAdvance();              // next track at end of stream (no GL: every loop pass, screen off too)
    bool mpIsOpening() const;          // a track/station is loading (for the open spinner)
    float mMpOpenStartT = 0.0f;        // when the current open began (spinner delay + spin)
    bool mMpOpening = false;           // an OpenPlay was issued and audio has not started/failed yet.
                                       // Latch: set in mpPlayCurrent, cleared in musicTick once the
                                       // player starts (isPlaying) or fails (openFailed). NOT mMpAdvancing:
                                       // that is cleared the moment the open begins (ended() goes false),
                                       // so it cannot track a slow network/HLS open.
    // Shared rotating loading/buffering spinner (icon 114); used by music + video overlays.
    void drawLoadingSpinner(float ccx, float ccy, float sz, float alpha);

    // Bluetooth AVRCP media control (NanoMediaBridge in system_server owns an
    // AVRCP-eligible MediaSession and bridges to these via system properties).
    void nanoMediaDispatch(const char* cmd);   // one-shot transport command -> active player
    void nanoPublishMediaState();              // publish now-playing state + metadata for the bridge
    void writeMediaMetaJson(const std::string& title, const std::string& artist,
                            const std::string& album, const char* kind, double dur);
    std::string mMediaLastState;               // last-published playback state (change-gated)
    std::string mMediaLastKind;                // last-published source kind
    std::string mMediaLastPlaying;             // last-published media_playing screen-keepalive flag
    std::string mMediaLastPos;                 // last-published whole-second position
    std::string mMediaLastDur;                 // last-published whole-second duration
    std::string mMediaMetaSig;                 // last-published title/artist/album/kind signature
    unsigned    mMediaMetaGen = 0;             // metadata generation counter (bumped on track change)
    // The metadata JSON write (open/write/fsync/rename on /data) must NOT run on the
    // render thread: under heavy I/O (a video/stream playing) + memory pressure the
    // rename can stall for seconds and trip the render watchdog (SIGABRT). The render
    // thread snapshots the metadata and hands it to this detached writer, which does
    // the file write THEN the gen-prop bump (that order, so the bridge watching the
    // prop always reads a current file). Single coalesced slot: only the latest matters.
    struct MediaMetaReq { std::string title, artist, album, kind; double dur; unsigned gen; };
    std::mutex mMediaWriteMutex;
    std::condition_variable mMediaWriteCv;
    MediaMetaReq mMediaWritePending;
    bool mMediaWriteHasPending = false;
    bool mMediaWriteStarted = false;
    void mediaMetaWriter();                    // detached worker: file write + gen-prop bump
    void queueMediaMetaWrite(const std::string& title, const std::string& artist,
                             const std::string& album, const char* kind, double dur, unsigned gen);

    // Now-Playing fullscreen render + control panel (1:1 web drawMusicPlayer / MP_CP).
    bool mMpFullInfo = true;           // Display toggle (counter/time/codec/seek cluster); shown by default
    // Coalesced (debounced) seek for Left/Right scrub: pressing only updates a target
    // + previews it; the heavy NanoAudio::seek() (it joins+restarts the decoder) is
    // committed once after input settles, so rapid/held scrubbing never blocks the
    // render thread (which would trip the watchdog).
    bool   mMpSeekPending = false;
    double mMpSeekTarget = 0.0;        // desired position (seconds) while scrubbing
    float  mMpSeekInputT = 0.0f;       // mEffectTime of the last scrub press
    // Async audio-control worker: NanoAudio control ops (play/pause/stop/seek/open)
    // can block on the audio server / codec; running them on the render thread risks
    // a watchdog abort or a hang. The render thread enqueues a command (cheap) and a
    // dedicated worker executes the blocking op, so the render loop never stalls.
    enum class MpAudioCmd { Play, Pause, Stop, Seek, OpenPlay, Release };
    struct MpAudioReq { MpAudioCmd cmd; double arg; std::string path; bool radio = false; };
    std::mutex mMpAudioMutex;
    std::condition_variable mMpAudioCv;
    std::deque<MpAudioReq> mMpAudioQueue;
    bool mMpAudioStarted = false;
    void mpAudioCmd(MpAudioCmd cmd, double arg = 0.0, const std::string& path = std::string(), bool radio = false);
    void mpAudioWorker();
    // True while an auto-advance OpenPlay is in flight. Since open() is async now,
    // ended() stays true until the worker loads the next track; without this the
    // per-frame auto-advance would fire repeatedly and skip tracks.
    bool mMpAdvancing = false;
    int  mMpVolLevel = 0;              // -4..+4 (web volLevel); maps to (lvl+4)/8 gain
    float mMpEnterT = 0.0f;            // player-presence fade 0..1 (bar fades in), smoothstep of mMpEnterRaw
    float mMpEnterRaw = 0.0f;          // linear 0..1 enter progress (~1.0s), eased into mMpEnterT (web mpEnterRaw)
    float mMpChromeT = 1.0f;           // XMB chrome presence during the player enter cross-fade (1 full, 0 hidden)
    float mMpFullInfoT = 0.0f;         // full-info cluster fade 0..1
    // control panel (TRIANGLE)
    bool  mMpCpOpen = false;
    bool  mMpCpTouch = false;          // panel opened by a screen tap -> enlarge icons
    int   mMpCpSel = 0;                // index into MP_CP
    int   mMpCpSelPrev = -1;
    float mMpCpAnimStart = -1.0f;      // open slide/fade start (mEffectTime)
    float mMpCpFocusStart = -1.0f;     // focus-change ease start
    bool  mMpCpClosing = false;
    float mMpCpCloseStart = -1.0f;
    float mMpCpPressStart = -1.0f;     // button invoke flash
    int   mMpCpPressSel = -1;
    bool  mMpVolSub = false;           // Volume Control submenu inside the panel
    // transient transport glyph (skip/back/ff/rew flash ~900ms)
    int   mMpTransientIcon = -1;
    float mMpTransientUntil = 0.0f;
    // visualizer-name banner + full-screen message chain
    std::string mMpBanner; float mMpBannerStart = -1.0f;
    std::string mMpMsg; float mMpMsgStart = -1.0f; float mMpMsgDur = 0.0f; int mMpMsgThen = 0;
    std::map<int, GLuint> mMpIconCache;
    std::map<int, float> mMpIconAR;   // audioplayer icon aspect ratios (w/h; pills are non-square)
    GLuint mMpJacketTex = 0;
    // Per-track album art (one slot, reloaded when the displayed track changes): a
    // <folder>/<trackname>.<img> overrides a <folder>/<foldername>.<img> album cover;
    // mMpArtTex==0 means "no art for this track -> use the note placeholder".
    int    mMpArtTi = -1;
    GLuint mMpArtTex = 0;
    GLuint mpIcon(int n);              // load+cache an audioplayer icon texture (NanoMenuPS3Icons.cpp)
    float  mpIconAR(int n);           // cached aspect ratio (w/h) of an audioplayer icon, 1.0 if unknown
    GLuint mpJacket();                 // load+cache the default jacket cover texture
    GLuint mpTrackArt(int ti);        // folder/per-track album art for track ti (0 if none)
    void   mpFreeArt();               // free the cached art texture (on close / track change)
    // Per-album folder art for the XMB Music column (embedded in the album icon, like
    // the web photo folders). Cached by album name; 0 means "no art / tried".
    //
    // Resolved on a WORKER, never in draw. Finding album art means probing candidate cover files
    // and, failing that, reading the first track's ID3v2 tag. Both are ordinary-fast on local
    // storage and take seconds per album on a network share, and mpAlbumArt() is called from the
    // draw path - which blocked the render thread long enough for startRenderWatchdog() to abort
    // nano outright the moment a user pointed Music at an FTP share. The render thread now only
    // ever looks up an already-decoded result and uploads it; the filesystem work happens off it.
    std::map<std::string, GLuint> mMpAlbumArt;   // render thread only: album -> texture (0 = none)
    GLuint mpAlbumArt(const std::string& albumName);   // non-blocking: queues, returns 0 until ready

    // One job type for both the per-album cover and the per-track cover in Now-Playing: they do the
    // same filesystem work and must both stay off the render thread.
    // Also carries photo-cover work: same shape (decode off-thread, upload on the render thread),
    // so it shares the worker rather than starting a third one.
    static constexpr int kPhotoCoverPx = 160;   // Photo column cover size
    // Thin wrappers so the art worker (a different translation unit) can decode and upload a photo
    // cover: the real helpers are file-static in NanoMenuPhotos.cpp.
    static bool photoDecodeCoverPixels(const std::string& path, int s, std::vector<uint8_t>* out);
    GLuint      photoUploadCover(const uint8_t* px, int w, int h);
    void        photoWriteCoverCache(const std::string& file, const uint8_t* px, int w, int h);
    struct MpArtJob    { std::string album; std::string track; bool isTrack = false; int ti = -1;
                         bool isPhoto = false; int photoIdx = -1; std::string cacheFile;
                         // wpSlot 0 = top, 1 = bottom; -1 = not a wallpaper job.
                         int wpSlot = -1; int decodePx = 0; };
    // Pixels rather than a texture: GL calls belong to the render thread, so the worker decodes
    // and the render thread uploads.
    struct MpArtResult { std::string album; int w = 0; int h = 0; std::vector<uint8_t> px;
                         bool isTrack = false; int ti = -1;
                         bool isPhoto = false; int photoIdx = -1; std::string cacheFile;
                         // wpSlot 0 = top, 1 = bottom; -1 = not a wallpaper job.
                         int wpSlot = -1; int decodePx = 0; };

    void mpRequestAlbumArt(const std::string& albumName);  // queue one album (idempotent)
    void mpDrainAlbumArt();                                // render thread: upload finished work
    void mpClearAlbumArt();                                // free the cached covers (on rescan)

    // Folder browser listing, resolved off the render thread.
    //
    // buildFolderBrowser() used to opendir() the directory and stat() every entry inline, on the
    // same thread that draws. That is the screen used to pick a media folder, so pointing it at a
    // network share meant one round trip per entry with the render thread held the whole time -
    // the watchdog kills nano at 8s. The listing now happens on a worker and the screen shows a
    // "Loading..." row until it lands. Only the roots screen stays synchronous: it reads /storage,
    // /mnt/media_rw and the mount table, none of which touch a server.
    struct FbEntry  { std::string name; bool isDir = false; long long size = 0; };
    struct FbResult { std::string path; std::vector<FbEntry> entries; bool ok = false; };
    void fbRequestListing(const std::string& path);
    void fbTick();                                         // render thread: swap in a finished listing
    void fbStopWorker();

    std::mutex               mFbLock;
    std::condition_variable  mFbCv;
    std::deque<std::string>  mFbQueue;      // guarded by mFbLock
    std::vector<FbResult>    mFbDone;       // guarded by mFbLock
    std::set<std::string>    mFbPending;    // guarded by mFbLock
    std::thread              mFbThread;
    std::atomic<bool>        mFbQuit{false};
    bool                     mFbStarted = false;
    // One-entry cache: the browser shows a single directory at a time, so this is all the memory
    // it needs to avoid re-requesting the listing on every rebuild of the same screen.
    std::string              mFbCachePath;
    std::vector<FbEntry>     mFbCacheEntries;
    bool                     mFbCacheValid = false;
    void mpStartArtWorker();
    void mpStopArtWorker();
    // preferTrackStem: when set, the track's own basename is tried before the folder's, which is
    // how a per-track cover overrides the album one.
    static bool mpResolveArtPixels(const std::string& firstTrackPath, int maxDim,
                                   int* w, int* h, std::vector<uint8_t>* px,
                                   bool preferTrackStem = false);

    std::mutex               mMpArtLock;
    std::condition_variable  mMpArtCv;
    std::deque<MpArtJob>     mMpArtQueue;     // guarded by mMpArtLock
    std::vector<MpArtResult> mMpArtDone;      // guarded by mMpArtLock
    std::set<std::string>    mMpArtPending;   // guarded by mMpArtLock: queued or in flight
    std::thread              mMpArtThread;
    std::atomic<bool>        mMpArtQuit{false};
    bool                     mMpArtStarted = false;
    void renderMusicPlayer();         // the Now-Playing fullscreen draw
    void openMpOpt(bool byTouch = false);  // open the control panel
    float mpPanelUi();                // mpUiScale, enlarged when opened by touch
    void closeMpOpt();                // close it (or the volume submenu first)
    void mpOptMove(int dx, int dy);   // grid nav (dx +right, dy +grid-up)
    void mpOptActivate();             // X on the focused control
    void mpOptBack();                 // Circle inside the panel (vol-sub -> panel -> close)
    void drawMpOpt(float closeT);     // render the panel (closeT>=0 drives the close anim)
    void drawMpVolMeter(float t);     // the Volume Control submenu meter
    void drawMpStatusRow(float ax, float fade);   // play-state/transport/repeat/shuffle row
    void mpShowMsg(const std::string& text, float durMs, int then);
    void mpCycleVis();                // SQUARE: cycle Waves/Canyon/Globe + wallpaper effects + banner
    int  mpVisCount() const;          // total music visualizers (3 + wallpaper-effect extras)
    int  mpVisEffectId() const;       // wallpaper-effect id for mMpVis>=3, else 0
    // Add-to-Playlist chooser (player): an XMB-style modal list to add the current
    // track to an existing playlist or create a new one (web mpOpenAddChooser).
    bool mMpPlChooserActive = false;
    std::vector<std::string> mMpPlChooserOpts;   // "New Playlist..." + existing names
    int   mMpPlChooserSel = 0;
    int   mMpPlChooserTrack = -1;                // track index being added
    float mMpPlChooserAnim = 0.0f;               // open fade
    void mpOpenAddChooser(int trackIdx = -1);   // build + open the add-to-playlist chooser (trackIdx <0 = the now-playing track)
    void mpPlChooserMove(int dir);    // up/down through the options
    void mpPlChooserSelect();         // commit the highlighted option
    void mpPlChooserCancel();         // dismiss without adding
    void drawMpPlChooser();           // render the modal list

    // In-player Delete confirm: deleting a track keeps the Now-Playing screen up (removes it from
    // the queue and advances to the next), instead of dropping back to the XMB. A small vertical
    // Delete/Cancel modal over the player, driven by the same nav keys as the playlist chooser.
    bool  mMpDelConfirmActive = false;
    int   mMpDelConfirmSel = 0;                  // 0 = Cancel (default), 1 = Delete
    float mMpDelConfirmAnim = 0.0f;              // open fade
    std::string mMpDelConfirmName;               // track title shown in the prompt
    void mpDelConfirmOpen();          // open the confirm for the current track
    void mpDelConfirmMove(int dir);   // toggle Delete / Cancel
    void mpDelConfirmSelect();        // act on the highlighted choice
    void mpDelConfirmCancel();        // dismiss without deleting
    void mpDeleteCurrent();           // unlink the current track, drop it from the queue, advance
    void drawMpDelConfirm();          // render the confirm modal

    // ======================= Photo viewer (PS3 XMB port) =======================
    // Library model (nano_photo.json), folder import (reuses the folder picker via
    // mFolderPickTarget = 2), the scanner, the Photo-column groups + thumbnail grid,
    // the full-screen viewer and the in-viewer control panel (the same icon style /
    // spacing / focus treatment as the Music MP_CP control panel). 1:1 source of
    // truth: /work/ps3/xmb-app/index.html (the Photo DATA category, photoViewer,
    // drawPhotoGrid, PV_CP / drawPvPanel, drawPvInfo). Implemented in NanoMenuPhotos.cpp.
    struct PhotoItem {
        std::string file;     // absolute path
        std::string name;     // filename without extension (display)
        std::string date;     // "YYYY-MM-DD HH:MM" capture date (file mtime proxy)
        int w = 0, h = 0;     // native pixel dimensions
        int64_t sz = 0;       // file size in bytes
        int64_t mtime = 0;    // for incremental rescan
    };
    struct PhotoPlaylist {
        std::string name;
        std::vector<std::string> files;   // references PhotoItem.file
    };
    std::vector<std::string>   mPhotoFolders;
    std::vector<PhotoItem>     mPhotos;
    std::vector<PhotoPlaylist> mPhotoPlaylists;
    int64_t mPhotoCfgStamp = -1;          // mtime of nano_photo.json (cross-process reload)
    static const int kPhotoMetaVersion = 1;
    int  mPhotoCfgVersion = 0;
    bool mPhotoLoaded = false;            // library parsed once (lazy, first Photo entry)
    bool mPhotoCatsStale = false;         // a scan finished -> rebuild the Photo column at root
    int  mPhotoGroupIdx = 0;              // 0 By Month, 1 By Year, 2 By Album, 3 All
    // Sort By (web photoSortBy, 5 options): field 0 = film(EXIF) date, 1 = import
    // (file mtime) date, 2 = image name. dir 0 = desc, 1 = asc (name forced asc).
    // Default = Film Date ascending (web subDef 1, the live firmware default).
    int  mPhotoSortField = 0;
    int  mPhotoSortDir   = 1;
    bool photoSortLess(int a, int b) const;   // compare two photo indices by the current field+dir
    void photoApplySort();                    // re-group the column + re-sort the open grid in place
    void photoSetSort(int field, int dir);    // set + apply (option-menu submenu)
    void photoSortCycleY();                   // Y on the grid/folder: cycle the 5 sort options + banner
    std::string photoSortLabelCur() const;    // current sort label for the option-menu "Sort By" row
    // Transient centered banner over the photo column/grid (Sort By / Group Content
    // change feedback, web showGroupBanner). Empty start = inactive (no idle cost).
    std::string mPhotoBanner; float mPhotoBannerStart = -1.0f;
    void photoShowBanner(const std::string& text);
    void drawPhotoBanner();

    // ==== Media folder view (Y toggles) ==================================
    // Y on the Photo / Video / Music column root toggles a FOLDER VIEW: the column regroups by the
    // media file's parent directory (each folder drills into its files) instead of the default view
    // (Photo date groups / flat Video list / Music albums). Persisted per library so it survives a
    // reboot. Sort stays reachable via the Triangle option menu. Read live by the column builders.
    bool mPhotoFolderView = false;
    bool mVideoFolderView = false;
    bool mMusicFolderView = false;
    void photoToggleFolderView();     // Y on the Photo column
    void videoToggleFolderView();     // Y on the Video column
    void musicToggleFolderView();     // Y on the Music column
    void buildVideoFolderSubmenu(const std::string& dir, Ps3Level& out);  // videos in one folder

    // ==== Game category system ordering (Y cycles) =======================
    // Y on the Game category re-orders the system tiles (below the fixed Recently Played /
    // Applications / Favorites / Collections rows). mXmbSystems is NEVER reordered (its order is
    // the user's Game Systems editor order and is persisted separately); buildPs3Cats emits the
    // system tiles through a sorted local index vector. persist.gammaos.nano.gamesort survives a
    // reboot. Modes: 0 Default (editor order), 1 A-Z, 2 Most Games, 3 By Manufacturer.
    int  mGameSortMode = 0;
    void gameSortCycleY();                       // Y: step the mode, persist, rebuild, banner

    // ==== Per-game Display Name view (persist.gammaos.nano.rom.show_display_names) =========
    // ON (default): each game shows its scraped / manually set Display Name (games with no such
    // name fall back to the ROM file name) and the list is ordered by that shown name, so the
    // fall-back items sort alongside the named ones. OFF: games show and are ordered by the raw
    // ROM file name. Read at startup and on the settings toggle; drives applyRomNameOverrides.
    bool mShowDisplayNames = true;
    std::string gameSortLabelCur() const;        // banner label for the current mode
    std::string systemManufacturer(const XmbSystem& s) const;  // best-effort maker for grouping

    // ==== Global search (Select on the home XMB) ==========================
    // A categorized results overlay across every XMB section (Games, Music, Photos,
    // Videos). Lazy: nothing is built until the user runs a search; the result list
    // is freed on close so an idle launcher holds no search state. Reuses the OSK for
    // the query and each section's existing open/launch path to activate a result.
    struct GSearchResult {
        int section = 0;          // 0 Games, 1 Music, 2 Photo, 3 Video
        std::string label;        // primary display text
        std::string sub;          // secondary text (system / artist+album / date / codec)
        int kind = 0;             // dispatch kind (PS3_ROM / PS3_RECENT / PS3_APP / PS3_MUSIC_TRACK / PS3_VIDEO_FILE / PS3_PHOTO)
        int a = 0, b = 0;         // dispatch indices (kind specific)
        std::string payload;      // dispatch payload (app package)
    };
    bool mGSearchActive = false;
    std::string mGSearchQuery;
    std::vector<GSearchResult> mGSearchResults;
    int mGSearchSel = 0;          // selected result index (into mGSearchResults)
    int mGSearchScrollRow = 0;    // first visible visual row (results + section headers)
    float mGSearchAnim = 0.0f;    // open ease
    void gsearchOpen();           // Select pressed on the home XMB: open the query OSK
    void gsearchBuild(const std::string& q);  // run the search, populate results
    void gsearchClose();
    void gsearchMove(int dir);    // up/down through results
    void gsearchActivate();       // launch / open the selected result
    int  gsearchVisRow(int resultIdx) const;  // visual row of a result (counts preceding headers)
    int  gsearchTotalVisRows() const;         // total visual rows (section headers + item rows)
    int  gsearchResultAtVisRow(int visRow) const;  // result index at a visual row, -1 for a header/oob
    void renderGlobalSearch();    // draw the overlay (returns nothing; caller gates on mGSearchActive)
    void gsearchTouch();          // DSi theme: tap a result row to select+activate, tap Back to cancel
    bool gsearchTouchScroll(int dir);   // DSi results scrolled one row, the selection kept in view

    // VIDEO (R4) - the HW decoder instance for the player. Lazily created in
    // openVideoPlayer, fully torn down in closeVideoPlayer/videoTick so an idle launcher
    // holds no video resources.
    NanoVideo* mVideoTest = nullptr;
    // Decoders being torn down asynchronously (releaseAsync): the worker join + the
    // blocking OMX stop run entirely off the render thread, and vidReapDying frees each
    // wrapper once its background teardown completes. A LIST (not a single pointer): the
    // render thread must never block waiting on an in-flight teardown, so it can never
    // synchronously release a previous one to make room - it just queues another and
    // reaps them as they finish. Drains to empty when idle (no leak).
    std::vector<NanoVideo*> mVidDying;
    void vidAsyncFree(NanoVideo* v);   // hand v to async teardown + queue it for reaping
    void vidReapDying();               // free any queued decoder whose async teardown is done
    // Single HW video decoder: a new title's codec must not be created until the previous
    // title's codec has fully released it, or the create wedges (second-video-hangs-on-switch).
    // mVidPrevCodecFreed is true when no video-codec teardown is pending (mVidDying drained);
    // false on vidAsyncFree, back to mVidDying.empty() in vidReapDying (render thread). The open
    // WORKER is not spawned until it is true: vidBeginOpen DEFERS the spawn (mVidOpenDeferred)
    // and videoTick spawns it once the decoder is free. Nothing blocks the render thread.
    std::atomic<bool> mVidPrevCodecFreed{true};
    bool mVidOpenDeferred = false;     // open worker held until the prev codec frees the HW decoder
    void vidSpawnOpenWorker();         // spawn the blocking open worker (vidOpenTitle/StreamRun)
    // scan worker
    std::mutex mPhotoScanMutex;
    std::vector<PhotoItem> mPhotoScanResults;
    bool mPhotoScanReady = false;
    bool mPhotoScanRunning = false;
    bool mPhotoScanPending = false;       // scan deferred until external storage is mounted
    bool photoStorageReady() const;
    void photoRefresh();
    // persistence + lazy load
    int64_t photoConfigStamp() const;
    bool loadPhotoConfig();
    void savePhotoConfig();
    void photoEnsureLoaded();
    void photoOnCatFocus();
    // scan
    void photoScanAsync();
    void photoScanThreadFunc();
    void photoDrainScanResults();
    // image decode (AImageDecoder scaled-on-decode; NanoMenuPhotos.cpp)
    static bool photoProbeDims(const std::string& path, int* w, int* h, int64_t* sz);
    GLuint photoDecodeTex(const std::string& path, int maxDim, int* outW, int* outH);
    GLuint musicEmbeddedArt(const std::string& mp3path, int maxDim);   // ID3v2 APIC cover
    // Same cover as pixels, for the art worker: GL belongs to the render thread.
    static bool musicEmbeddedArtPixels(const std::string& mp3path, int maxDim,
                                       int* outW, int* outH, std::vector<uint8_t>* out);
    // folder import (Search for Media Servers; mFolderPickTarget = 2)
    void photoOpenFolders();
    void buildPhotoFoldersScreen(Ps3Level& out);
    void photoFolderSelect(const std::string& path);
    void photoRemoveFolder(int idx);
    // column content + grouping
    void buildPhotoColumnItems(std::vector<Ps3Item>& out);
    void photoCycleGroup();               // SQUARE in the Photo column -> next Group Content mode
    void photoSetGroup(int mode);         // option-menu Group Content submenu -> a specific mode
    struct PhotoGroup { std::string name; std::vector<int> idx; };
    std::vector<PhotoGroup> photoGroups() const;   // groups per the current mode
    static std::string fmtPhotoDate(const std::string& iso);   // "YYYY-MM-DD HH:MM" -> "D/M/YYYY H:MM"
    static std::string fmtFileSize(int64_t b);
    // thumbnail grid (screenKind PHOTO_GRID)
    std::vector<int> mPhotoGridList;      // photo indices in the open album/grid
    int  mPhotoGridCursor = 0;
    int  mPhotoGridTop = 0;               // top visible row (scroll, kept in sync with the pixel scroll)
    float mPhotoGridScrollY = 0.0f;       // smooth pixel scroll (finger-driven; source of truth for the render)
    std::string mPhotoGridTitle;
    float mPhotoGridAnim = 0.0f;          // open fade-in
    float mPhotoGridFocusStart = -1.0f;   // focus grow tween start
    int  mPhotoGridCursorPrev = -1;
    int  mPhotoGridFromPl = -1;           // playlist index if opened from a playlist, else -1
    std::map<int, GLuint> mPhotoThumbCache;   // photo idx -> thumbnail texture
    std::map<int, float>  mPhotoThumbAR;      // photo idx -> thumbnail aspect (w/h)
    void openPhotoGrid(const std::vector<int>& list, const std::string& title, int fromPl);
    void closePhotoGrid();
    void photoGridNav(int dx, int dy);
    void photoGridSelect();
    void renderPhotoGrid();
    GLuint photoThumb(int photoIdx);      // lazy thumbnail (maxDim ~256), cached
    void photoThumbEvict();               // bound the thumb cache around the cursor
    void photoFreeThumbs();               // drop the whole thumb cache (on grid close)
    // Group-folder cover: a centre-square thumbnail of the group's first photo,
    // drawn as the column icon (like the Music album art / the web photo folders).
    std::map<int, GLuint> mPhotoCoverCache;   // photo idx -> 160px square cover
    GLuint photoGroupCover(int photoIdx);
    void photoFreeCovers();
    // Draw a folder-shaped column icon (silver folder + tab) with an optional cover
    // photo / album art inset in the body, matching the web XMB photo folders. Shared
    // by the Photo group folders and the Music album folders.
    void drawFolderIcon(float ix, float iy, float dsz, float alpha, GLuint coverTex);

    // full-screen viewer
    bool  mPvActive = false;
    std::vector<int> mPvList;             // photo indices being viewed
    int   mPvIdx = 0;
    int   mPvRot = 0;                     // 0 / 90 / 180 / 270
    float mPvZoom = 1.0f;
    float mPvPanX = 0.0f, mPvPanY = 0.0f;
    float mPvEnterT = 0.0f, mPvEnterRaw = 0.0f;   // enter/exit fade from black (~0.4s)
    float mPvHintUntil = 0.0f;
    std::string mPvEffect;               // "Normal" / "Slide" / "Fade" (Change Effect)
    // photo-to-photo transition (Slide/Fade)
    bool  mPvTrans = false; int mPvTransFrom = -1; float mPvTransStart = 0.0f;
    int   mPvTransDir = 1; std::string mPvTransEffect;
    // interactive touch swipe: the photo tracks the finger horizontally, then either
    // completes to the neighbour or springs back on release (mobile-gallery feel).
    bool  mPvDragActive = false;          // finger currently dragging the photo
    float mPvDragDx = 0.0f;               // live horizontal offset (logical px)
    bool  mPvDragSettle = false;          // releasing: easing dx toward the target
    float mPvDragFrom = 0.0f, mPvDragTo = 0.0f, mPvDragSettleStart = 0.0f;
    int   mPvDragCommitDir = 0;           // step to apply when a commit settle finishes
    bool  mPvPanning = false;             // single-finger pan in progress (while zoomed)
    void  pvGoTo(int newIdx);             // jump to a photo with no Slide/Fade transition
    // decoded viewer textures (current +/- neighbours), keyed by photo idx
    std::map<int, GLuint> mPvTexCache;
    std::map<int, int> mPvTexW, mPvTexH;  // decoded texture dims
    GLuint pvTex(int photoIdx, int* w, int* h);
    void pvPrefetch();
    void pvFreeTextures();
    // Async display-size decode worker: the multi-MP decode runs OFF the render
    // thread (worker produces CPU RGBA); pvDrainDecodes uploads to GL on the render
    // thread each frame. Keeps slideshow/navigation at a locked 60fps and pre-decodes
    // direction-ahead neighbours so each advance is instant. (issue: viewer 60fps)
    struct PvDecReq { int idx; std::string path; int maxDim; uint64_t gen; };
    struct PvDecRes { int idx; int w; int h; uint64_t gen; std::vector<uint8_t> px; };
    std::thread mPvDecThread;
    std::mutex mPvDecMutex;
    std::condition_variable mPvDecCv;
    std::deque<PvDecReq> mPvDecQueue;
    std::vector<PvDecRes> mPvDecDone;
    std::set<int> mPvDecInFlight;
    std::atomic<bool> mPvDecStop{false};
    std::atomic<bool> mPvDecStarted{false};
    std::atomic<int>  mPvFocusIdx{0};        // photo idx the worker decodes first
    std::atomic<uint64_t> mPvDecGen{0};      // bumped on open/free to drop stale results
    int mPvMaxDim = 1920;
    static const int kPvTexCap = 5;          // current +/- 2 uploaded display textures
    void pvDecodeThreadFunc();
    void pvRequestDecode(int photoIdx);
    void pvDrainDecodes();
    void pvStartDecodeWorker();
    void pvStopDecodeWorker();               // stop + join (called from ~NanoMenu)
    void openPhotoViewer(const std::vector<int>& list, int idx);
    void closePhotoViewer();
    void pvStep(int d);
    void pvShow3D();
    // slideshow
    bool  mPvSlideshow = false, mPvPaused = false, mPvRepeat = false;
    float mPvSlideNext = 0.0f, mPvSlideMs = 4000.0f;
    int   mPvSlideStyle = 0;
    std::string mPvSlideSpeed;            // "Slow" / "Normal" / "Fast"
    // info / display mode
    bool  mPvInfo = false;
    std::string mPvDispModeName;          // "Zoom" / "Normal"
    float mPvDispModeUntil = 0.0f;
    // Set as Wallpaper / Trimming range selector
    bool  mPvWpMode = false, mPvTrimMode = false;
    float mPvWpZoom = 1.0f;
    void  pvWallpaperConfirm();
    void  pvShowDeleteConfirm();
    // transient full-screen message (Delete / 2D-3D / wallpaper-set), music-style
    std::string mPvMsg; float mPvMsgStart = -1.0f; float mPvMsgDur = 0.0f;
    void  pvShowMsg(const std::string& text, float durMs);
    // control panel (TRIANGLE) - mirrors the Music MP_CP look
    bool  mPvPanel = false;
    bool  mPvPanelTouch = false;   // panel opened by a screen tap -> enlarge icons for touch
    int   mPvCpSel = 0, mPvCpSelPrev = -1;
    float mPvCpAnimStart = -1.0f, mPvCpFocusStart = -1.0f;
    bool  mPvCpClosing = false; float mPvCpCloseStart = -1.0f;
    float mPvCpPressStart = -1.0f; int mPvCpPressSel = -1;
    // control submenu (Change Effect / Slideshow Speed / Slideshow Style)
    bool  mPvCpSub = false; std::string mPvCpSubKind; std::vector<std::string> mPvCpSubOpts; int mPvCpSubSel = 0;
    void renderPhotoViewer();
    void openPvPanel(bool byTouch = false);
    float pvPanelUi();   // pvUiScale, enlarged when the panel was opened by touch
    void closePvPanel();
    void pvPanelMove(int dx, int dy);
    void pvPanelActivate();
    void pvPanelBack();
    void drawPvPanel(float closeT);
    void drawPvInfo();
    void drawPvDispModePill();
    void drawPvWallpaperSel();
    // photoviewer icons (lazy, /data override + /system/etc fallback)
    std::map<int, GLuint> mPvIconCache;
    std::map<int, float>  mPvIconAR;
    GLuint pvIcon(int n);
    float  pvIconAR(int n);
    // photo playlists
    void buildPhotoPlaylistsScreen(Ps3Level& out);
    void buildPhotoPlaylistGridList(int plIdx, std::vector<int>& out, std::string& title);
    void photoCreatePlaylist(const std::string& name);
    void photoAddToPlaylist(int plIdx, const std::string& file);
    void photoRemoveFromPlaylist(int plIdx, const std::string& file);   // drop a photo from a playlist + save
    void photoMoveInPlaylist(int plIdx, const std::string& file, int dir); // reorder a photo within a playlist (dir -1/+1) + save
    void photoDeletePlaylist(int plIdx);   // erase a photo playlist (JSON-only) + save + rebuild
    // add-to-playlist chooser (viewer + grid)
    bool  mPvPlChooserActive = false;
    std::vector<std::string> mPvPlChooserOpts;   // "New Playlist..." + existing names
    int   mPvPlChooserSel = 0;
    std::string mPvPlChooserFile;                // photo file being added
    float mPvPlChooserAnim = 0.0f;
    void pvOpenAddChooser(const std::string& file);
    void pvPlChooserMove(int dir);
    void pvPlChooserSelect();
    void pvPlChooserCancel();
    void drawPvPlChooser();
    void pvSlideshowStart(const std::vector<int>& list, int idx, int style);
    void photoTick();                     // per-frame: viewer enter fade + slideshow + timers
    // Multi-select (Delete Multiple) checkbox screen (web photoMulti):
    // checkbox + thumbnail + name + date rows, Select All / Clear All / OK buttons.
    bool mPhotoMultiActive = false;
    int  mPhotoMultiMode = 0;             // 0 = delete, 1 = copy
    std::vector<int> mPhotoMultiItems;    // photo indices in the open album/grid
    int  mPhotoMultiSel = 0;
    int  mPhotoMultiBtn = -1;             // -1 list, 0 Select All, 1 Clear All, 2 OK
    std::set<int> mPhotoMultiChecked;     // checked row indices
    void photoMultiOpen(int mode);
    void photoMultiMove(int d);           // up/down (list or button column)
    void photoMultiLR(int d);             // left = list, right = the side buttons
    void photoMultiActivate();            // X
    void photoMultiClose();
    void renderPhotoMulti();
    void drawPhotoMsg();                  // shared transient-message render (viewer/grid/multi)

    std::vector<Ps3Item>& ps3CurItems();   // current visible item list (top or submenu)
    int& ps3CurSel();
    // True when the row drills into a deeper submenu list (the same predicate that
    // draws the right-edge ">" chevron). A directional drill (XMB RIGHT, DSi DOWN/
    // RIGHT) only fires for these rows; a leaf (toggle / action like Reboot) stays
    // X/A-only so a drifting stick or temperamental d-pad diagonal cannot confirm it.
    bool ps3ItemOpensSubmenu(const Ps3Item& it) const;
    bool ps3FocusOpensSubmenu();           // the current selection opens a submenu
    void renderPs3Xmb();
    void drawPs3Clock(float fadeMul);   // U-frame + analog face + DD/M H:MM

    // --- PSP Go slide clock (NanoMenuPS3Clock.cpp) --------------------------
    void  drawPspClock(float dtMs);            // per-frame orchestrator (advance + all passes)
    void  renderPspClockSecondary();           // reveal-driven PSP clock on the secondary (bottom) panel
    void  advanceBottomTrail(float dtMs);      // advance the bottom clock's own comet trail (every frame)
    void  bottomClockCacheSnapshot(int w, int h); // snapshot the composited secondary for the 30fps cap
    void  bottomClockCacheBlit();              // re-present the cached secondary frame (30fps skip frame)
    // True while the bottom clock's glow is actively overwriting the shared mGlassBlurTex each frame,
    // so every primary frost consumer (submenu backdrop, dialogs, System Update) must re-capture the
    // wave rather than reuse a stale cache (else the clock's halo bleeds into the frost = flicker).
    bool  frostBufferSharedWithClock() const;

    // --- bottom-screen Control Center (NanoControlCenter.cpp) -----------------
    // Live dashboard on the bottom panel while a single-screen (non-dual-stack) app runs on top.
    bool  controlCenterActive();           // prop + overlay + app-launched + single-screen + dual-screen gate
    void  renderControlCenterFrame();      // EGL: set up + present the CC on the secondary panel
    void  ccHideForIme();                  // hide the CC secondary layer so the pinned bottom-panel IME shows
    void  hideControlCenterLayer();        // hide the CC secondary layer on teardown
    void  renderControlCenterUI();         // all-immediate dashboard (fallback if the static cache fails)
    void  renderCcPass(int pass);          // shared body; draws the STATIC and/or DYNAMIC layers by mask
    void  renderCcStatic();                // bake pass: frame-invariant layers only
    void  renderCcDynamic();               // per-frame pass: live layers only, over the composited cache
    void  pollControlCenterStats();        // refresh live sysfs stats (throttled)
    void  ccPollTouch();                   // read the BOTTOM digitizer + dispatch (called in the park loop)
    void  ccTouchFrame();                  // decode one touch frame -> tap / wake
    void  ccOnTap(float px, float py);     // hit-test tiles -> toggle actions
    int   ccSliderAt(float px, float py);  // which left-card slider is under a press (-1 = none)
    void  ccApplySlider(int i, float py);  // set a grabbed slider's value from the touch Y (grab + drag)
    void  ccUpdateSleep();                 // graceful bottom-screen dim-to-off / wake ramp (per frame)
    void  ccBeginSleep();                  // start the graceful dim-to-off (Sleep tile tap AND 30s idle auto-sleep)
    void  ccRestoreBacklightIfSlept();     // on CC teardown, restore the bottom backlight if it was slept/dimming
    void  ccSendVolume(int v);             // issue the media_session --set (forks a shell); debounced by callers
    void  ccSetDisplayVolMap(int bot, int top);  // write the per-display volume map (Settings.Global); debounced by callers
    // CC touch + interactive state (render-thread only; no mutex).
    int   mCcRawX = 0, mCcRawY = 0;        // last bottom-digitizer raw coords (0..640 x 0..480)
    float mCcRawMaxX = 0.0f, mCcRawMaxY = 0.0f; // bottom digitizer ABS range (EVIOCGABS, read once; 0 = not yet read)
    bool  mCcTouchDownRaw = false;         // BTN_TOUCH state
    bool  mCcTouchWas = false;             // previous-frame down (edge detect)
    float mCcDownX = -1.0f, mCcDownY = -1.0f;  // press position (tap detection)
    int   mCcHeldSlider = -1;              // slider grabbed by the current touch (-1 = none), for drag
    int   mCcVolLastSet = -1;              // last volume value actually pushed via media_session --set
    int64_t mCcVolLastSetMs = 0;           // when it was pushed (debounce the fork during a drag)
    int   mCcMapLastBot = -1, mCcMapLastTop = -1;  // last per-display volume map values written (debounce)
    int64_t mCcMapLastSetMs = 0;           // when the per-display volume map was last written (debounce the fork)
    bool  mCcSleeping = false;             // bottom screen slept (backlight1 ramping / at 0)
    int   mCcSleepFromBri = 128;           // backlight1 value to restore on wake
    float mCcSleepRamp = 1.0f;             // 1 = full brightness, 0 = off (eased)
    int   mCcSleepDir = 0;                 // -1 dimming to off, +1 ramping back up, 0 idle
    int64_t mCcLastTouchMs = 0;            // monotonic ms of the last bottom-touch activity (idle auto-sleep)
    bool  mCcActiveSeeded = false;         // false until the CC-active edge seeds mCcLastTouchMs (re-arms per activation)
    int64_t mCcWakeTapMs = 0;              // "Double Tap" wake: monotonic ms of the first arming tap while slept (0 = none)
    float mCcFadeIn = 1.0f;                // 0 = full black, 1 = fully revealed; reset to 0 each time the CC comes up
    // CC paging: page 0 = the dashboard, page 1 = the app-launcher grid. mCcPage is the target; mCcPageOffset
    // eases toward it (0 = dashboard, 1 = app grid) for the horizontal slide. mCcPassXoff is the device-px
    // horizontal translation the render adds to X() so a pass can be drawn shifted for the transition.
    int   mCcPage = 0;
    float mCcPageOffset = 0.0f;
    float mCcPassXoff = 0.0f;
    int   mCcAppScroll = 0;                // first app row shown in the grid (vertical scroll)
    void   renderCcApps(bool st, bool dy); // the app-launcher grid page (icons + labels)
    void   renderCcSettings();             // the screen-options page (double-tap wake, idle timeout)
    void   ccOnSettingsTap(float px, float py);  // options-page hit-test (design space) -> toggle the prop
    void   renderCcPageDots();             // pagination dots (dashboard <-> apps <-> options) at the CC bottom edge
    void   ccEnsureAppList();              // (re)load the installed-app list + apps_generation gate
    GLuint loadColorIconTexAbs(const char* absPath);  // full-colour PNG -> GL texture (real APK icons)
    int    ccAppAt(float px, float py);    // app-grid hit-test (design space) -> app index or -1
    void   ccLaunchBottomApp(const std::string& pkg);  // launch on the bottom panel (or dual-stack branch)
    void   ccPollBottomAppExit();          // watch the launched bottom app; clear + return to CC on exit
    void   ccEndBottomApp(bool stopApp);   // tear down bottom-app state (force-stop optional), restore drop_input
    bool   ccDrainBottomTouch();           // drain the BOTTOM digitizer; true if a touch-DOWN occurred this poll
    void   ccTouchResync();                // the CC's bottom-digitizer contacts as the device holds them now
    bool   ccPollTopTapDown();             // drain the TOP digitizer (gt9xx-1); true if a touch-DOWN occurred
    // Controller-focus pin: point sys.gammaos.nano.focus.display at the panel that should own the gamepad.
    // The Control Center itself never takes focus (it is touch-only); the pin is the top panel whenever the
    // CC is up with no bottom app, the bottom panel while a bottom app runs, and cleared (-1) when the CC is
    // not active. The framework (RootWindowContainer) honours the pin only if that display has a focused app.
    void   ccSetFocusDisplay(int disp);
    int    mCcFocusDisplay = -1;           // last value written to sys.gammaos.nano.focus.display
    // Focus RING: a ~1s glowing edge pulse on the screen that just took the controller (SF/app mode
    // only). Stamped in ccSetFocusDisplay on a real focus change; driven by uptimeMillis() so it is
    // independent of which park branch renders (mFrameDt is not refreshed on the bottom-app branch).
    int     mCcRingDisp    = -1;           // display id whose ring is pulsing (-1 none; 0 bottom / 2 top)
    int64_t mCcRingStartMs = 0;            // uptimeMillis() the pulse began
    bool    mTopRingShown  = false;        // the top overlay layer was t.show()n for a ring pulse
    bool    ccRingActive();                // a pulse is running (< ~1s since the stamp)
    float   ccRingT();                     // pulse progress 0..1
    void    drawFocusRing(float t01);      // draw the glowing edge frame (transparent centre) at progress t01
    void    renderTopFocusRing();          // present a ring frame on the TOP overlay surface (mSurface)
    void    renderBottomFocusRing();       // present a ring frame on the BOTTOM secondary surface
    void    hideTopFocusRing();            // hide the top overlay layer after a top-ring pulse (guarded)
    // A bottom-screen app launched from the app grid. While set: the CC is hidden (the app owns the bottom
    // panel) and a watcher polls for its exit to re-show the CC. Written only by the render thread.
    std::string mCcBottomApp;              // launched bottom-app package ("" = none, CC visible)
    std::atomic<bool> mCcBottomAppGone{false};  // exit watcher -> render thread: the bottom app exited
    std::atomic<int>  mCcBottomGoneStreak{0};   // consecutive "gone" polls (debounce transient backgrounding)
    std::atomic<uint32_t> mCcBottomGen{0};      // bumped on every launch/teardown; a poll result from a stale
                                                // generation is dropped (no cross-relaunch streak corruption)
    std::atomic<bool> mCcBottomPollBusy{false}; // an exit-watcher poll thread is in flight (serialize polls)
    int64_t mCcBottomWatchMs = 0;          // last exit-watcher poll (throttle)
    // KEY_ALL_APPLICATIONS force-show: a hardware button toggles the Control Center visible on the bottom
    // panel over ANY running app, including a dual-stack app (which controlCenterActive() normally excludes)
    // or a grid-launched bottom app (which normally hides the CC). Toggled by ccPollAllAppsKey() draining the
    // gamepad key device in the park loop; folded into controlCenterActive() and the bottom-app park branch.
    bool   mCcForceVisible = false;        // KEY_ALL_APPLICATIONS override: keep the CC shown over an app
    bool   ccPollAllAppsKey();             // drain the key device; true on a KEY_ALL_APPLICATIONS down-edge
    // CC static-layer cache: the frame-invariant dashboard (background, card bodies, headers, slider
    // TRACKS, speaker/sun icons, clock face + ticks, gauge TRACK rings, fixed labels, and every tile
    // icon/label/background) is baked once into mCcStaticTex, then composited as one full-panel quad each
    // frame; only the live elements redraw over it. Rebuilt only when the signature changes (tile 0/1/2
    // states, date, panel size). Lives on the SECONDARY EGL context; freed in hideControlCenterLayer.
    struct CcStaticSig {
        int   w = -1, h = -1;         // panel size -> layout scale (resize forces a rebuild)
        uint8_t tile[8] = {0};        // per-tile ccActState() 0/1/2 (bg + icon colour + shape + perf label)
        int   wday = -1, mday = -1;   // clock date "MON 20"
        int   locale = -1;            // the UI language (the labels are baked translated)
        bool operator==(const CcStaticSig& o) const {
            if (w != o.w || h != o.h || wday != o.wday || mday != o.mday || locale != o.locale) return false;
            for (int i = 0; i < 8; i++) if (tile[i] != o.tile[i]) return false;
            return true;
        }
    };
    CcStaticSig ccStaticSignature() const; // capture the current static-state signature
    CcStaticSig mCcStaticSig;              // signature the current mCcStaticTex was baked with
    bool   mCcStaticValid = false;         // a valid bake exists for mCcStaticSig
    GLuint mCcStaticTex = 0;               // RGBA8 cache, mWidth x mHeight (0 = not created)
    GLuint mCcStaticFbo = 0;               // FBO wrapping mCcStaticTex (0 = not created)
    void   ccEnsureStaticCache();          // (re)bake the static layers when the signature changes
    void   ccFreeStaticCache();            // delete the FBO + texture (teardown / GPU failure)
    void  pspClockPollInput();
    void  pspClockTouchDrag();                 // the clock follows a dragging finger (dismiss: pspClockTouchZones' swipe)
    void  pspClockTouchZones();                // end of frame: the clock's block + swipe up over everything                 // reads the F12 gate prop into mPspClockEnabled
    void  pspClockPollTilt(bool active);       // accel -> smoothed mPspTilt* parallax (gyro peek)
    float pspClockTextFade() const;            // smoothed XMB-text alpha multiplier (5.4)
    float pspClockChromeFade() const;          // whole-canvas backstop opacity (5.5, window 0.58..0.72)
    bool  pspClockBlowCat(int i, float& bx, float& by, float& brot) const;  // category icon blow-away (5.5)
    bool  pspClockBlowItem(int i, float& xShift, float& yLift, float& rot) const; // item icon blow-away (5.5)
  private:
    void  pspClockBackdropBlur(float amt);     // 5.10 (stage 1)
    void  pspClockLens(float cr);              // 5.9 glass refraction disc (stage 2)
    void  pspClockFace(float reveal, float floatY, float descentFrac);  // clock face (stage 3/4)
    void  pspClockDateText();                  // clock date readout (TEXT; kept full-res under Half Resolution: Clock)
    // Soft Gaussian glow for the clock chrome (numerals/ticks/hands/hub): render the
    // shapes white into mPspChromeGlowFbo, blur with blurGlassChain, composite additively
    // in the glow colour. drawShapes is a caller-supplied lambda that draws the shapes
    // (in whatever colour/alpha it is handed); downLevels/gaussIters set the halo width.
    void  pspClockChromeGlowPass(const std::function<void(float,float,float,float)>& drawShapes,
                                 int downLevels, int gaussIters,
                                 float gr, float gg, float gb, float alpha,
                                 int cacheSlot = -1, bool cacheable = false);
    void  pspClockSnapshotGlow(int slot);      // copy mGlassBlurTex into the persistent glow cache
    void  pspClockEntrance(float sc, float ox, float oy, float reveal, float angOff, float alphaMul); // 5.11
    void  pspClockEntranceIcons(float sc, float ox, float oy, float reveal, float alphaMul, uint32_t seed);
    void  pspClockAmbientGlyphs(float dtMs);   // 5.7
    void  pspClockGlow(float& r, float& g, float& b) const;  // current glow colour 0..1
    void  pspClockBakeGlyphs();                // rasterize numeral outlines -> alpha textures
    void  pspClockSampleGlow();                // dominant wallpaper colour -> mPspGlow (5.9.4)
    // --- live-app capture (#5) ---
    bool  pspClockLiveAppEnabled() const;      // master gate (persist prop, default OFF)
    bool  pspClockLiveBackdropOn() const;      // Clock Live Backdrop toggle: ON=live app, OFF=dark
    bool  pspClockUseAppSource() const;        // lens should sample the captured app, not the wave
    void  pspClockSampleAppDim();              // app mean brightness -> mPspAppDim / mPspAppBackdropDark
    void  pspClockAppCaptureTick();            // render-thread: start/stop worker + upload latest frame
    static void pspClockCaptureWorker();       // detached bg worker; touches ONLY file-static state
    // Continuous virtual-display mirror (live 60fps, zero-copy) - the preferred live-app
    // backdrop source; supersedes the captureDisplay worker. All render-thread, no mutex.
    bool  pspClockMirrorStart();               // stand up the mirror; false on failure (-> worker)
    void  pspClockMirrorStop();                // tear down + un-flag the overlay layer
    void  pspClockMirrorTick(bool want);       // lifecycle + per-frame newest-buffer pump
    void  pspClockMirrorImportAndBlit(const sp<GraphicBuffer>& buf); // zero-copy import + V-flip
  public:
    // Signal the detached capture worker to stop and (optionally) wait a bounded
    // time for it to exit its loop. Safe to call from teardown; join-free (the worker
    // is detached). Static because it only touches file-static worker state.
    static void pspClockStopCaptureWorker(int drainMs);
  private:
  public:
    // Content-info hover background + description (web HOVER_BG/CINFO_DESC): dwell on a
    // mapped item ("Photo Gallery" is the only one reachable in nano) fades a full-frame
    // bg image + firmware title/description in over the wave, under the chrome.
    // fanFile non-empty => draw a scraped ROM fanart background (no description)
    // instead of the Photo Gallery cinfo bg; the two are mutually exclusive.
    void drawPs3CinfoBg(const char* focusLabel, const std::string& fanFile = std::string());
    GLuint mCinfoTex = 0;            // lazily-loaded cinfo background texture
    bool   mCinfoTexTried = false;   // load attempted (don't retry on failure)
    int    mCinfoTexW = 0, mCinfoTexH = 0;
    float  mCinfoAlpha = 0.0f;       // current visible alpha (0 .. 0.85)
    std::string mCinfoFocusKey;      // currently focused mapped item name (or empty)
    float  mCinfoDwellStart = -1.0f; // mEffectTime when focus moved to the mapped item
    // The texture actually on screen last frame, so a focus move to an item with NO
    // cinfo (a game without fanart) fades OUT whatever was shown instead of cross-
    // showing the Photo Gallery bg. Not owned (aliases mFanartTex / mCinfoTex).
    GLuint mCinfoShownTex = 0;
    int    mCinfoShownW = 0, mCinfoShownH = 0;
    // Scraped ROM fanart hover background (Phase 4): one texture at a time, reloaded
    // when the focused ROM changes, freed on fade-out / leaving Game / occlusion.
    GLuint mFanartTex = 0;
    int    mFanartTexW = 0, mFanartTexH = 0;
    std::string mFanartPath;         // path currently loaded into mFanartTex
    float mFanartPanStart = -1.0f;   // mEffectTime the focused fanart began its slow Ken-Burns pan (-1 = disarmed)
    // Set each frame by drawPs3CinfoBg to the shown scraped ROM fanart texture (0 when
    // none). The option side panel reads it to decide NOT to draw its wave frost - it
    // lets the fanart show through the gradient scrim instead (user: don't blur games
    // with a background).
    GLuint mFanartFrostTex = 0;
    void ps3XmbLeft();
    void ps3XmbRight();
    void ps3XmbUp();
    void ps3XmbDown();
    void ps3XmbBumperSkip(int dir);
    void ps3XmbSelect();
    void ps3XmbBack();
    // Time Zone 3D-globe selector (NanoMenuPS3Globe.cpp). Shared 1:1 web tzglobe
    // screen used by BOTH the XMB Date and Time -> Time Zone view (mPs3TzActive)
    // and the first-run setup wizard timezone step.
    void openTimezoneGlobe();              // open the XMB Time Zone view
    void closeTimezoneGlobe(bool apply);   // close it (apply = set persist.sys.timezone)
    void tzGlobeNav(int dir);              // move the zone selection + re-aim the globe
    void renderTimezoneGlobe();            // full-screen globe + zone-list chrome (both flows)
    float ps3TzFadeAlpha();                // 0..1 XMB->globe cross-fade (360ms smoothstep)
    void beginTzGlobeFade();               // (re)start the cross-fade + aim the globe at mTzSelected
    // System Language picker (NanoMenuPS3Menu.cpp). Settings -> System Settings ->
    // System Language opens the SAME fullscreen language list as the first-run
    // setup wizard, with live locale preview as the cursor moves and
    // persist.sys.locale applied on confirm. mPs3LangActive gates the XMB view.
    void openLanguagePicker();             // open from the System Language item
    void closeLanguagePicker(bool apply);  // close (apply = persist + keep the locale)
    void langPickerNav(int dir);           // move the highlighted language (clamped)
    void renderLanguagePicker();           // XMB entry: live preview + the shared list
    void renderLanguageList(const char* title, float alpha);  // shared chrome (wizard + XMB)
    // Glass icon pipeline (NanoMenuPS3Icons.cpp).
    void initGlassIcons();                 // compile program, load amb/env textures
    GLuint nmapForIcon(int iconIndex);     // load+cache nmap_NNN.png
    GLuint iconTexForIcon(int iconIndex);  // load+cache xmb_icon_NNN.png colour tex (DSi flat cards)
    GLuint uiIconTexForIcon(int iconIndex);// framework UI icon (idx>=kUiIconBase): mono silhouette as a plain tex (else 0)
    GLuint bevelForIconIdx(int iconIdx);   // bevel normal from a console icon's alpha
    GLuint bevelFromRGBA(const uint8_t* px, int w, int h);   // bevel normal from any silhouette buffer
    // Favourites loveheart icon, rasterised procedurally from the implicit heart curve (no PNG
    // asset needed): a white-on-alpha silhouette uploaded as the flat colour tex (DSi/Minima tint
    // it red) and beveled into a normal map for the XMB glass relight. Lazily built + cached.
    GLuint mHeartIconTex = 0, mHeartNmapTex = 0;
    void   ensureHeartIcon();               // build mHeartIconTex + mHeartNmapTex once
    // Wi-Fi signal icons for 0..4 bars (the Quick Menu's network row), rasterised the same way.
    GLuint mWifiIconTex[5] = {}, mWifiNmapTex[5] = {};
    void   ensureWifiIcons();
    GLuint gpGlassNmap(bool round, float wpx, float hpx);    // cached bevel nmap for a gamepad-tester button shape
    // Resolve a system iconRef (builtin:/retroarch:/core:/file:) to a (colour
    // tex, glass bevel nmap) pair, cached by ref string. See NanoMenuPS3Icons.cpp.
    void resolveSystemIcon(const std::string& ref, GLuint* outTex, GLuint* outNmap);
    // Decode a RetroArch icon (by name, no .png) to a mono-white RGBA buffer.
    // Resolves the dev override then the bundled set. Used by the grid picker.
    bool decodeRetroIconRGBA(const std::string& name, std::vector<uint8_t>* outPx, int* w, int* h);
    GLuint loadPs3NmapTex(const char* file);             // RGBA normal-map loader
    // Draw an icon with the glass shader (device px coords, like drawIconTex).
    // Refraction samples the live wave (ps3bg work texture) behind the icon.
    void drawGlassIcon(GLuint nmapTex, float x, float y, float w, float h,
                       float cr, float cg, float cb, float alpha, float rot = 0.0f);

    // Background scan thread — scans ROM paths off the render thread
    struct BgScanResult {
        std::vector<std::string> roms;
        std::vector<std::string> displayNames;
        std::vector<std::string> activePaths;
        std::string activePath;
        bool valid;              // true if scan found at least one path
        uint32_t scanInputsGen = 0;  // the system's scanInputsGen when the scan read it
        std::string id;              // the system scanned (a delete or reorder moves systems)
    };
    std::mutex mBgScanMutex;
    std::vector<BgScanResult> mBgScanResults; // one per system, guarded by mutex
    bool mBgScanResultReady;                  // set by thread, cleared by render loop
    bool mBgScanThreadRunning;                // true while thread is active
    bool mBgScanAgain = false;                // a rescan was asked for while one was running
    void bgScanThreadFunc();                  // the thread entry point

    // Settings column. Pinned as the leftmost XMB column at sentinel index -2.
    // Order: -2 Settings | -1 Recently Played | 0..N-1 Systems.
    // The vertical list shows mSettingsItems instead of ROMs when
    // mXmbSystemIndex == -2 (see isOnSettingsColumn()).
    std::vector<SettingsItem> mSettingsItems;
    int mSettingsSelectedIndex;  // cursor inside the settings vertical list

    // Wi-Fi sub-screen state
    std::vector<WifiNetEntry> mWifiEntries;
    int mWifiEntrySelected;
    int mWifiScrollTop;
    int64_t mWifiLastScanMs;
    bool mWifiScanInProgress;       // scan thread is running
    bool mWifiListDirty;            // bg thread produced a fresh list; render should re-read
    std::mutex mWifiListMutex;
    std::thread mWifiScanThread;
    std::string mWifiStatusMsg;
    int64_t mWifiStatusMsgUntilMs;
    // Pending new-network add: when the user picks an unsaved SSID we pop
    // the OSK password prompt, and stash the SSID+security here so the
    // callback can finish the `cmd wifi connect-network` invocation.
    std::string mWifiPendingSsid;
    int mWifiPendingSecurity;
    // ---- Wi-Fi manage dialog (saved-network Connect / Change Password / Forget) ----
    // Opened when the user activates a SAVED row in the Wi-Fi list. Themed for both
    // XMB (dark PS3 panel) and DSi (glossy DS card); controller + touch driven.
    enum WifiManageAction { WMA_CONNECT = 0, WMA_DISCONNECT, WMA_CHANGE_PW, WMA_FORGET };
    bool mWifiManageActive = false;
    int  mWifiManageSel = 0;
    int  mWifiManageNetId = -1;          // saved network id being managed
    std::string mWifiManageSsid;
    int  mWifiManageSecurity = 0;        // 0=open,1=wep,2=wpa2,3=wpa3,4=owe
    bool mWifiManageConnected = false;
    std::vector<int> mWifiManageActions; // WifiManageAction values, in display order
    // Auto re-prompt after a wrong-password failure: wifiConnectWatch (detached)
    // sets these and pollInput (main input thread) pops the password OSK.
    std::atomic<bool> mWifiRepromptPending{false};
    std::string mWifiRepromptSsid;
    int  mWifiRepromptSecurity = 2;
    // Internet Connection Test (Network Settings dialog): an async thread runs the
    // connectivity checks and publishes progressive result text here; renderPs3Dialog
    // copies it into the dialog body each frame while mPs3NetTestLive is set.
    bool mPs3NetTestLive = false;        // the open dialog is the live connection test
    std::atomic<bool> mPs3NetTestActive{false};
    std::string mPs3NetTestBody;
    std::mutex mPs3NetTestMutex;
    std::thread mPs3NetTestThread;

    // ---- Internet Connection Settings wizard (NanoMenuPS3Menu.cpp) -------------
    // 1:1 port of the web NETCONF wireless flow with a real cmd-wifi backend:
    // intro -> method -> connection -> WLAN scan/AP list -> security/key (OSK) ->
    // IP/DNS/MTU/proxy/UPnP (Custom) -> review -> save (connect) -> test. Rendered
    // with the same chrome as the fullscreen dialogs + a horizontal slide.
    bool   mPs3WizActive = false;
    int    mPs3WizExit = 0;               // on close: +1 completed/forward, -1 cancelled/back (for the setup step)
    bool   mSetupNetWizSeen = false;      // setup-wizard tracking of the network step's wizard
    int    mPs3WizId = 0;                 // current screen (WizScreen enum, file-local)
    std::vector<int> mPs3WizStack;        // back stack of screen ids
    int    mPs3WizSel = 0;                // chooser/list selection
    int    mPs3WizScroll = 0;             // scan-list scroll top
    float  mPs3WizAnim = 0.0f;            // open fade 0..1
    float  mPs3WizSlide = 0.0f;           // body slide offset (virtual px), eases to 0
    int    mPs3WizSlideDir = 1;           // +1 forward (enter from right), -1 back
    float  mPs3WizSlideStart = 0.0f;      // mEffectTime at the start of the slide
    float  mPs3WizScreenStart = 0.0f;     // mEffectTime when the current screen opened
    int    mPs3WizBootDeferred = 0;   // the scan / connect / test screen (id) waiting for sys.boot_completed
    void   wizStartNetWork(int id);
    int    mPs3WizTextField = 0;          // which value the open OSK is editing (WizField)
    std::string mPs3WizFieldError;        // inline validation message under the current text field (red)
    // Collected values (mirror web wizState.values):
    std::string mPs3WizSsid;              // chosen SSID
    int    mPs3WizSecTok = 0;             // cmd-wifi security token: 0 open,1 wep,2 wpa2,3 wpa3,4 owe
    std::string mPs3WizSecLabel;          // display security label
    std::string mPs3WizKey;               // WEP/WPA passphrase
    bool   mPs3WizChangePwOnly = false;   // saved-net "Change Password": key -> WS_SAVE (skip IP/advanced walk)
    std::string mPs3WizMethod, mPs3WizConn, mPs3WizWlanMode;
    std::string mPs3WizIpMode, mPs3WizDnsMode, mPs3WizMtuMode, mPs3WizProxyMode, mPs3WizUpnp;
    std::string mPs3WizIpAddr, mPs3WizSubnet, mPs3WizRouter, mPs3WizPdns, mPs3WizSdns;
    std::string mPs3WizMtu, mPs3WizProxyAddr, mPs3WizProxyPort;
    std::string mPs3WizOpmode, mPs3WizSpeedDuplex;          // wired op-mode + speed/duplex
    std::string mPs3WizPppoeUser, mPs3WizPppoePass;         // PPPoE
    std::string mPs3WizDhcpHost;                            // DHCP host name
    std::string mPs3WizEapUser, mPs3WizEapPass;             // EAP authentication
    int mPs3WizPendingTextField = -1;   // text-field OSK to open from the render loop
                                        // (deferred so chained text fields don't open
                                        // a new OSK from inside the old OSK's callback)
    // Wizard lifecycle + render (NanoMenuPS3Menu.cpp).
    void startNetWizard();
    // Network: Internet Browser / Internet Search (open the user's browser, see launchUrl).
    void openInternetBrowser();
    void openInternetSearch();
    void openGoToUrl();                  // OSK prefilled https:// -> launch the typed URL
    bool tryOpenSearchEngineChooser();   // X/Triangle: Internet Search -> engine chooser, Internet Browser -> Go to URL
    void launchUrl(const std::string& url);
    void startDateTimeWizard(int mode);   // 0 = Set via Internet, 1 = Set Manually (reuses net-wizard UI)
    void wizEnter(int id, int dir);       // push/go to a screen
    void wizConfirm();                    // X / Enter
    void wizBack();                       // O / Back
    void wizNav(int dir, bool horizontal);// Up/Down/Left/Right
    void wizOpenTextField(int field);     // open the OSK for a value field
    std::string validateWizField(int field, const std::string& val);  // "" = ok, else error message
    void wizRescan();                     // X: re-scan APs on the scan-list screen
    int  wizNextScreen(int id, int sel);  // forward-nav table (commits the choice)
    void renderNetWizard();
    // DSi-theme painter for the WiFi/Bluetooth setup wizard: renderNetWizard() runs the
    // shared state machine then delegates the DRAW to this when mNdsTheme, so the wizard
    // matches the DSi System Menu (scanline field, banner header, glossy list buttons,
    // Back/OK bar) instead of the XMB chrome. Panel rect = the bottom DS touch screen.
    void renderNdsNetWizardBody(float rx, float ry, float rw, float rh);
    void ndsWizTouch();                   // DSi wizard bottom-panel touch: tap rows / Yes-No / Back-OK-Search bar
    void renderMinimaNetWizardBody(float rx, float ry, float rw, float rh);   // Minima-styled Wi-Fi/BT wizard

    // Bluetooth sub-screen state
    std::vector<BtDevEntry> mBtEntries;
    int mBtEntrySelected;
    int mBtScrollTop;
    int64_t mBtLastScanMs;
    bool mBtScanInProgress;
    bool mBtDiscoveryInProgress;
    bool mBtListDirty;
    std::mutex mBtListMutex;
    std::thread mBtScanThread;
    std::thread mBtDiscoveryThread;
    std::string mBtStatusMsg;

    // Accessory Settings Bluetooth wizard (PS3 UI). Reuses the net-wizard
    // screen machinery (WS_BT_* screens, renderNetWizard, wizConfirm/Nav/Back)
    // but drives the real gammaos-net bt backend on background threads.
    int  mBtWizMode = 0;                      // 0 Manage, 1 BD Remote, 2 Audio Device
    std::vector<BtDevEntry> mBtWizBonded;     // bonded devices for the Manage chooser
    std::vector<BtDevEntry> mBtWizScan;       // discovered devices for the Register list
    std::mutex mBtWizMutex;                   // guards mBtWizBonded / mBtWizScan
    std::atomic<bool> mBtWizBusy{false};      // a bg scan/pair/connect/disconnect/unpair is running
    std::atomic<bool> mBtWizOpOk{false};      // result of the last bg op (for the done screen)
    std::string mBtWizSelAddr, mBtWizSelName; // selected bonded device (opts / info)
    int  mBtWizSelCod = 0;                    // its class-of-device
    bool mBtWizSelConnected = false;          // its live connection state
    int  mBtWizAdInput = 0, mBtWizAdOutput = 0, mBtWizAdMic = 2;  // audio device choices
    std::string mBtWizPin;                    // user PIN entered for outbound classic pairing
    int  mBtWizInVariant = -1;                // inbound pairing variant (0 = classic PIN)
    std::string mBtWizInPasskey;              // inbound passkey to display for confirmation
    std::atomic<bool> mBtWizRadioOn{true};    // cached real radio state (dumpsys enabled:)
    std::atomic<bool> mBtWizToggling{false};  // a radio enable/disable is settling
    std::atomic<int>  mBtWizBondedGen{0};     // bumped each time mBtWizBonded is replaced
    float mBtWizManageRefreshT = -999.0f;     // last live-refresh time while sitting on Manage
    void btWizToggleRadioAsync(bool on);      // enable/disable the radio + settle + refresh

    void startBtWizard(int mode);             // mode 0 Manage, 1 BD Remote, 2 Audio Device
    void btWizRefreshBondedAsync();           // gammaos-net bt list-bonded -> mBtWizBonded
    void btWizScanAsync();                    // gammaos-net bt scan -> mBtWizScan
    void btWizPairAsync(const std::string& addr, const std::string& pin);
    void btWizConnectAsync(const std::string& addr);
    void btWizDisconnectAsync(const std::string& addr);
    void btWizUnpairAsync(const std::string& addr);
    void btWizStartReceive();                 // discoverable + open inbound bridge
    void btWizStopReceive();                  // stop discoverable + close inbound bridge
    void btWizInboundAcceptAsync(const std::string& addr, int variant);  // confirm + await bond
    int64_t mBtStatusMsgUntilMs;

    // OSK password mode: when active, keystrokes append to mOskQuery, but
    // the HUD renders masked chars. On Enter, mOskPasswordCallback fires
    // with the raw string and the overlay closes. Reusing mOskActive so the
    // existing render path still handles dismissal + keyboard grid.
    bool mOskPasswordMode;
    bool mOskPlaintext;
    std::string mOskPasswordPrompt;
    std::function<void(const std::string&)> mOskPasswordCallback;
    // Numeric field auto-formatting: 0 = free text, 1 = date (YYYY/MM/DD),
    // 2 = time (HH:MM). For 1/2 the OSK accepts digits only and inserts the
    // separators automatically as the user types (real-IME field behaviour).
    int mOskFieldFmt = 0;

    // Hierarchical settings tree
    std::vector<SettingNode> mSettingsNodes;
    std::vector<int> mSettingsNavStack;
    std::vector<int> mSettingsTreeVisible;
    int mSettingsTreeSelected;
    int mSettingsTreeScrollTop;
    int mSettingsEditNodeIdx;
    bool mSettingsValuesDirty;
    mutable std::mutex mSettingsValueMutex;
    std::unordered_map<int, std::string> mSettingsValueCache;

    // Icon rendering
    void initIconTextures();
    void ps3LoadCatIcons();          // (re)load the 7 category icon textures + their glass normal maps
    void drawIcon(int iconIdx, float x, float y, float size,
                  float r, float g, float b, float a);
    // Draw an arbitrary GL texture handle (PS3 category icons live outside
    // mIconTextures[]). Supports a non-square w/h. (NanoMenuPS3Menu.cpp)
    void drawIconTex(GLuint tex, float x, float y, float w, float h,
                     float r, float g, float b, float a, float rot = 0.0f, bool flipV = false,
                     float sharpUpW = 0.0f, float sharpUpH = 0.0f,
                     float uMaxU = 1.0f, float uMaxV = 1.0f, bool flipH = false);
    // Like drawIconTex but through the ES-DE FX program, applying the element's ES-DE brightness
    // (0 = none) and saturation (1 = none) to the sampled texture. Used only for ES-DE covers /
    // backdrops that set a non-default value; falls back to drawIconTex if the FX program is absent.
    void drawIconTexFx(GLuint tex, float x, float y, float w, float h,
                       float r, float g, float b, float a, float rot,
                       float saturation, float brightness, float cornerRadius = 0.0f,
                       const float* gradEnd = nullptr, bool gradHoriz = true,
                       bool flipH = false, bool flipV = false);
    GLuint mIconTextures[21]; // 0-14=systems, 15=history, 16=generic game cartridge, 17=setting, 18=app-grid, 19=4-square grid (Applications), 20=push-pin (Pinned Apps)

    // On-screen keyboard. mOskActive + mOskQuery are the keep-stable members
    // external code reads/writes directly; all new runtime state is in mOsk.
    bool mOskActive;           // OSK is visible and receiving input
    bool  mOskGlassValid = false;  // OSK frosted-panel blur cached (avoid per-frame full-FB resolve)
    float mOskGlassT = 0.0f;       // mEffectTime of the last OSK panel capture
    std::string mOskQuery;     // current typed buffer (committed text)
    NanoOskState mOsk;         // page/shift/focus/caret/popup/candidates/IME state
    // OSK clipboard paste (Y). nano is native (bootanim) and can't call ClipboardManager,
    // so oskPaste() asks the SystemServer clipboard bridge via sys.gammaos.nano.clip_req and
    // oskTick() picks up the reply (clip_ready + /data/system/nano_clipboard.txt) and inserts
    // it - non-blocking so the render/input thread never stalls.
    bool    mOskPastePending = false;  // a paste request is in flight
    long    mOskPasteNonce = 0;        // request id echoed back by the bridge in clip_ready
    int64_t mOskPasteReqMs = 0;        // when the request was sent (for timeout)
    // Touchscreen OSK input. The digitizer is read straight from the same evdev
    // stream (ABS_MT_POSITION_X/Y + BTN_TOUCH); ranges are read lazily via
    // EVIOCGABS. Raw -> panel-normalized -> logical is corrected per device by
    // the osk_touch_swap/flipx/flipy props (DRM-rotated panels need them; SF and
    // upright panels use the defaults). Works on both DRM and SF back-ends.
    int   mTouchMinX = 0, mTouchMaxX = 0;   // digitizer X range (max<=min = unread)
    int   mTouchMinY = 0, mTouchMaxY = 0;   // digitizer Y range
    int   mTouchRawX = -1, mTouchRawY = -1; // last raw ABS_MT position (slot 0)
    int   mTouchDevRot = -1;                 // digitizer mounting, quarter turns (-1 = not read yet)
    bool  mTouchDown = false, mTouchWasDown = false;
    // Mouse/keyboard support for the nano UI. The cursor is accumulated from EV_REL in
    // logical-pixel space and drawn on top each frame; a left click is routed through the
    // existing touch hit-test (touchMapRaw returns the cursor position when mTouchFromPointer).
    float   mCursorX = 0.0f, mCursorY = 0.0f;   // logical-pixel cursor position
    bool    mCursorVisible = false;             // shown while recently moved/clicked
    int64_t mLastPointerMs = 0;                 // last pointer activity (auto-hide timer)
    bool    mTouchFromPointer = false;          // current touch frame is a mouse click, not a finger
    int     mTouchInputOff = -1;                // Touch Input set to Off (Nano ignores the touchscreen); -1 = not read yet
    bool    mKbdShiftHeld = false;              // physical-keyboard shift state (for OSK typing)
    void    drawPointerCursor();                // draw the mouse cursor overlay (NanoMenuRender.cpp)
    int     kbdCodeToCp(int code, bool shift);  // USB-keyboard keycode -> printable codepoint (0 = none)
    // Multi-touch slot tracking (Type-B), per touchscreen: the pinch-zoom and the two- and three-finger
    // taps. mTouchId/SX/SY are the contacts of the panel the gesture is on (mTouchPanelFd).
    static constexpr int kTouchSlots = 5;
    struct TouchPanel {
        bool direct = true;                 // a touchscreen; a touchpad (a pad's) drives no menu touch
        bool mt = false;                    // has slots (ABS_MT_SLOT)
        bool btn = false;                   // BTN_TOUCH
        bool ignored = false;               // down when another panel's gesture ended: ignored until it lifts
        int  slot = 0;                      // current ABS_MT_SLOT selector
        int  id[kTouchSlots] = { -1, -1, -1, -1, -1 };
        int  sx[kTouchSlots] = {}, sy[kTouchSlots] = {};
        int  contacts() const { int n = 0; for (int v : id) n += v >= 0; return n; }
    };
    std::unordered_map<int, TouchPanel> mTouchPanels;   // by fd
    int   mTouchPanelFd = -1;
    TouchPanel& touchPanel(int fd);
    void  touchPanelsMirror();              // the gesture panel's contacts into mTouchId/SX/SY
    // A panel's contacts as the device holds them now (fd -1: every panel), run at the report ending a
    // drop or once that fd was read to EAGAIN; discarded: its events were dropped unread, so a slotless
    // panel's contact is let go.
    void  touchResync(int fd = -1, bool discarded = false);
    void  touchPanelGone(int fd);           // a removed device: its contacts are let go
    void  touchAfterChange(bool changed);
    std::vector<int> mTouchResyncFds;       // dropped events (SYN_DROPPED) or a lost queue: resync pending
    bool  multiTapAllowed();                // the two- and three-finger taps may act here
    bool  launchHeldByFinger();             // a touch launch waits for the finger to lift
    int64_t mTouchLastEvMs = 0;             // last touchscreen event (the resync watchdog)
    int   mTouchId[kTouchSlots] = { -1, -1, -1, -1, -1 };   // per-slot tracking id (-1 = no contact)
    int   mTouchSX[kTouchSlots] = {};       // per-slot raw X
    int   mTouchSY[kTouchSlots] = {};       // per-slot raw Y
    int   touchContacts() const { int n = 0; for (int v : mTouchId) n += v >= 0; return n; }
    bool  mPvPinchActive = false;           // two-finger pinch in progress (photo)
    // A gesture in which a second finger touched: no handler taps, selects or scrolls for it, from
    // the second finger down until all are up. Let go still, two fingers toggle the Quick Menu and
    // three the Applications view.
    bool  mTouchMulti = false, mTouchMultiMoved = false;
    bool  mTouchGestureLive = false, mTouchGestureOsk = false;   // a gesture under way; it began on the keyboard
    bool  mTouchPressUsed = false;          // a button acted as the gesture's finger landed: the rest does nothing
    // A press button's action waits this long after its finger lands (or until the finger lifts), so a
    // two-finger tap whose first finger lands on a button opens the Quick Menu instead.
    static constexpr int64_t kTouchPressDeferMs = 50;
    std::function<void()> mTouchPressAct;   // the press waiting to act (touchGestureAbort drops it)
    int64_t mTouchPressDueMs = 0;
    void  touchPress(std::function<void()> act);   // a press button under a landing finger
    void  touchPressTick(bool lifted);      // runs the waiting press once due, or at once when lifted
    std::string touchPressScreen() const;   // the dialog / wizard shown: a press made on another is dropped
    std::string mTouchPressScreen;          // touchPressScreen() when the waiting press was made
    int   mPointerFd = -1;                  // the mouse whose held click is mTouchDown (mTouchFromPointer)
    bool  mLaunchByTouch = false;           // a touch started the launch under way (launchHeldByFinger)
    bool  mLaunchNoted = false;             // launchStartNote saw the launch under way
    void  launchStartNote();
    // Movement that makes a press a drag: about 8 px at 640x480 (at least 10 px), in logical pixels and
    // in DS units.
    float touchSlopPx() const { return std::max(10.0f, (float)std::min(mWidth, mHeight) / 60.0f); }   // at least 10 px
    static constexpr float kNdsTouchSlopDs = 4.0f;   // 10 px on the RG DS bottom panel (2.5 px per DS unit)
    int64_t mNdsTouchMoveMs = 0;            // last DSi drag sample: a lift after a stop does not fling
    int   ndsBarHit(float dsX, float dsY, bool flash);   // the DSi bottom bar: -1 Back, +1 OK, 0 neither
    void  ndsPressFlash(float x, float y, float w, float h);   // a bottom-panel press lights up (DS units)
    void  drawNdsPressFlash();              // in the DSi secondary-panel pass
    TouchRect mNdsFlashR;
    int64_t mNdsFlashMs = 0;
    int   mTouchMultiMax = 0;               // most contacts down at once in the gesture
    int64_t mTouchMultiMs = 0;
    int   mTouchMultiId[kTouchSlots] = { -1, -1, -1, -1, -1 };   // the contact each start is for
    int   mTouchMultiX[kTouchSlots] = {}, mTouchMultiY[kTouchSlots] = {};
    bool  multiTouchFrame();                // true while the gesture is a multi-finger one
    void  touchGestureAbort();
    void  threeFingerTap();
    float mPvPinchStartDist = 0.0f, mPvPinchStartZoom = 1.0f;
    bool  touchMapRaw(int rawX, int rawY, float& px, float& py);  // raw -> logical px
    bool  mOskTouchTuneRead = false;
    bool  mOskTouchSwap = false, mOskTouchFlipX = false, mOskTouchFlipY = false;
    // Touch gesture state shared by the media players' and Minima's hit-testers. Positions are
    // in logical pixels (post swap/flip), matching touchMapRaw's output.
    bool    mXmbTouchTracking = false;   // a finger-down gesture is being followed
    bool    mXmbTouchMoved = false;      // exceeded the slop -> a drag, not a tap
    int64_t mXmbTouchDownMs = 0;         // press start (uptimeMillis)
    float   mXmbTouchDownPX = 0.0f, mXmbTouchDownPY = 0.0f;   // press position
    float   mXmbTouchLastPX = 0.0f, mXmbTouchLastPY = 0.0f;   // previous-frame position
    std::vector<SearchResult> mSearchResults;
    int mSearchSelectedIndex;
    bool mSearchActive;        // Search results being displayed

    // FreeType font rendering. The array holds the ~10 system faces (Noto Latin/CJK/emoji/Arabic +
    // the DSi DSVec/DSVecNum faces) PLUS every ES-DE theme fontPath loaded on demand (esdeFontFace).
    // A single ES-DE theme can declare many fonts (aura/canvas use 6, atari 3) and they accumulate
    // across theme switches, so keep generous headroom - a too-small cap silently drops later theme
    // fonts to the default face (e.g. Atari 50 Menu's carousel losing HarryHeavy). Each slot is one
    // pointer; the glyph-cache key packs the face index into bits 41+, so 48 stays well within range.
    static const int MAX_FT_FACES = 48;
    FT_Library mFtLib;
    FT_Face mFtFaces[MAX_FT_FACES];
    int mFtNumFaces;
    int mFontSize;  // render pixel size
    GLuint mGlyphAtlasTex;
    int mAtlasW, mAtlasH;
    int mAtlasCurX, mAtlasCurY, mAtlasRowH;
    // Guards against re-resetting the glyph atlas more than once per frame: the
    // atlas is recycled (cache cleared, cursor rewound) the first time it fills
    // mid-frame so glyphs keep rasterizing instead of re-reading the compressed
    // font off EROFS every frame (a sustained decompress storm -> kernel OOM).
    // render() clears this at the top of each frame.
    bool mGlyphAtlasReset = false;
    uint32_t mGlyphAtlasGen = 0;   // bumped by each recycle: glyphs looked up before it are stale
    uint32_t mGlyphGenAtFrame = 0;
    int mGlyphRecycleStreak = 0;   // frames in a row that recycled the atlas
    void resetGlyphAtlas();
    void glyphPanelPass();
    // Currently-set atlas MIN_FILTER (texture-object state). setGlyphAtlasAA()
    // flips this to GL_LINEAR_MIPMAP_NEAREST only around the home-XMB menu
    // content and back to GL_LINEAR everywhere else, skipping redundant GL sets.
    GLint mGlyphAtlasMinFilter = GL_LINEAR;
    // Keyed by (rasterPx << 32 | codepoint): one entry per glyph per display
    // size, so each is rendered crisp at its native pixel size.
    std::unordered_map<uint64_t, GlyphInfo> mGlyphCache;
    // Scale-independent text widths keyed by string (see measureText). Never
    // invalidated: the font size is fixed at init and glyphs only get added.
    std::unordered_map<std::string, float> mTextWidthCache;
    // Logical UTF-8 -> visual (Arabic-shaped / bidi-reordered) strings for
    // textForDisplay. Only strings whose bytes can carry RTL or zero-width
    // codepoints land here. Cleared wholesale past a size backstop because
    // OSK typing feeds it unbounded user text.
    std::unordered_map<std::string, std::string> mBidiCache;

    // Text shader (per-vertex color for emoji support)
    GLuint mTextProgram;
    GLint  mTextLocPosition;
    GLint  mTextLocTexCoord;
    GLint  mTextLocColor;
    GLint  mTextLocTexture;
    GLint  mTextLocRotation;
    GLint  mTextLocSharp = -1;   // uSharp uniform: crisp analytic edge AA amount
    GLint  mTextLocSharpUp = -1;   // uSharpUp: sharp-bilinear upscale source size (texels), 0=off
    // ES-DE cover FX program (brightness/saturation); 0 if it failed to link (drawIconTexFx then
    // falls back to the plain drawIconTex path).
    GLuint mEsdeFxProgram = 0;
    GLint  mEsdeFxLocPosition = -1;
    GLint  mEsdeFxLocTexCoord = -1;
    GLint  mEsdeFxLocColor = -1;
    GLint  mEsdeFxLocLocal = -1;
    GLint  mEsdeFxLocTexture = -1;
    GLint  mEsdeFxLocRotation = -1;
    GLint  mEsdeFxLocSat = -1;
    GLint  mEsdeFxLocBright = -1;
    GLint  mEsdeFxLocHalf = -1;
    GLint  mEsdeFxLocRadius = -1;
    GLint  mSceneFbo = 0;          // scene FBO captured once per XMB pass (half-res icons perf)
    GLint  mSceneVp[4] = {0,0,0,0}; // scene viewport captured once per XMB pass
    float  mTextSharp = 0.0f;    // current uSharp value (set by setGlyphAtlasAA), uploaded by drawText

    // Rounded-rect shader (OSK keys)
    GLuint mRoundProgram;
    GLint  mRoundLocPosition;
    GLint  mRoundLocLocal;
    GLint  mRoundLocRotation;
    GLint  mRoundLocHalf;
    GLint  mRoundLocRadius;
    GLint  mRoundLocInset = -1;
    GLint  mRoundLocColor;
    // Frosted-glass shader (OSK panel) + framebuffer snapshot texture
    GLuint mGlassProgram;
    GLint  mGlassLocPosition;
    GLint  mGlassLocLocal;
    GLint  mGlassLocTexCoord;
    GLint  mGlassLocRotation;
    GLint  mGlassLocHalf;
    GLint  mGlassLocRadius;
    GLint  mGlassLocTexture;
    GLint  mGlassLocTexel;
    GLint  mGlassLocTint;
    GLint  mGlassLocAlpha;
    GLint  mGlassLocTonemap;
    GLuint mGlassTex;
    int    mGlassTexW, mGlassTexH;
    // Dual-Kawase downsample blur for the frosted panel: render the full-res
    // capture down through box-filter passes into progressively smaller FBO
    // textures, then upsample with a tent filter in the panel pass. Every source
    // pixel contributes (unlike sparse single-pass taps), so the result is a
    // smooth aero-glass blur instead of a pixelated/ghosted one.
    GLuint mGlassDownProgram      = 0;
    GLint  mGlassDownLocPosition  = -1;
    GLint  mGlassDownLocTexCoord  = -1;
    GLint  mGlassDownLocTexture   = -1;
    GLint  mGlassDownLocHalfpixel = -1;
    GLint  mGlassDownLocOffset    = -1;
    GLuint mGlassFbo[4]      = {0, 0, 0, 0};   // 4 down levels: 1/2..1/16
    GLuint mGlassDownTex[4]  = {0, 0, 0, 0};
    int    mGlassDownW[4]    = {0, 0, 0, 0};
    int    mGlassDownH[4]    = {0, 0, 0, 0};
    // Separable Gaussian (H then V) run on the 1/8 downsample, ping-ponging
    // between two scratch buffers so we never sample and render the same texture
    // in one pass. mGlassGaussTex[1] is the final blur source.
    GLuint mGlassGaussProgram     = 0;
    GLint  mGlassGaussLocPosition = -1;
    GLint  mGlassGaussLocTexCoord = -1;
    GLint  mGlassGaussLocTexture  = -1;
    GLint  mGlassGaussLocDir      = -1;
    GLuint mGlassGaussFbo[2]      = {0, 0};
    GLuint mGlassGaussTex[2]      = {0, 0};
    int    mGlassGaussW[2]        = {0, 0};
    int    mGlassGaussH[2]        = {0, 0};
    GLuint mGlassBlurTex     = 0;   // final blurred texture sampled by the panel
    int    mGlassBlurW       = 0;
    int    mGlassBlurH       = 0;
    // PS3 XMB submenu depth-of-field cache: the capture+blur chain is expensive
    // (~26ms), so capture exactly once per submenu visit and reuse the cached
    // blur for every frame (the panel draw itself is cheap). Invalidated on
    // returning to the top level.
    bool   mPs3GlassValid      = false;
    float  mPs3GlassBlurT      = 0.0f;   // mEffectTime of last submenu wave-blur (30Hz cadence)
    float  mPs3DlgBlurT        = 0.0f;   // mEffectTime of last dialog backdrop wave-blur
    // Run the downsample + Gaussian passes on srcTex (srcW x srcH); leaves the
    // result in mGlassBlurTex. Called by captureGlass (FB snapshot) and
    // captureGlassFromWave (ps3bg::workTex, no FB capture).
    void   blurGlassChain(GLuint srcTex, int srcW, int srcH,
                          int downLevels = 3, int gaussIters = 2);
    // Blur the live PS3 wave/gradient (ps3bg::workTex) with no framebuffer
    // capture - cheap enough to sustain 60fps. Result is in LOGICAL orientation;
    // draw with drawFrostedGlass(..., waveSpace=true). False if wave not ready.
    bool   captureGlassFromWave();
    // OSK rounded-rect + frosted-glass primitives (NanoMenuRender.cpp)
    void drawRoundedRect(float x, float y, float w, float h, float radius,
                         float r, float g, float b, float a);
    void drawTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                      float r, float g, float b, float a);
    // A lightning bolt filling the box (x, y, w, h): the charging mark drawn inside the battery
    // glyph of every home theme (a colour change alone was reported as too subtle).
    void drawBolt(float x, float y, float w, float h, float r, float g, float b, float a);
    // Per-vertex-alpha triangle in logical device px, single RGB. Used to build
    // anti-aliased procedural shapes (feathered edges ramp alpha to 0). Emits into
    // the solid batch (per-vertex colour) when active; falls back to a flat solid
    // triangle at the mean alpha otherwise.
    void triAA(float x0, float y0, float a0,
               float x1, float y1, float a1,
               float x2, float y2, float a2,
               float r, float g, float b);
    // Snapshot the framebuffer region [x,y,w,h] (logical px) into mGlassTex.
    // Returns false if capture is unavailable (e.g. active DRM GL rotation).
    bool captureGlass(float x, float y, float w, float h);
    void freeGlassScratch();   // release glass-blur scratch buffers (parked overlay)
    // Draw a frosted-glass panel over the captured region (call captureGlass
    // first with the same rect). tint rgb darkens; tintA = panel opacity.
    void drawFrostedGlass(float x, float y, float w, float h, float radius,
                          float tr, float tg, float tb, float tintA, float fade,
                          bool waveSpace = false, float tonemapOverride = -1.0f,
                          GLuint srcTexOverride = 0, int srcOverrideW = 0, int srcOverrideH = 0);

    // Setup wizard state
    bool mSetupWizardActive;
    SetupWizardStep mSetupStep;
    float mSetupTransitionAlpha;
    float mSetupSlideOffset;
    bool mSetupTransitioning;
    SetupWizardStep mSetupTransitionTarget;
    bool mSetupBootWaited;
    // Language selection
    int mLangSelected;
    int mLangScrollTop;
    // Welcome greeting animation (iOS-style cycling)
    int mGreetingIndex;
    float mGreetingTimer;
    float mGreetingFade;     // 0..1 current greeting opacity
    bool mGreetingFadingOut; // true = fading out, false = fading in / holding
    int mGreetingTransType;  // randomized transition style (0-5)
    // Timezone
    std::vector<TimezoneEntry> mTzEntries;
    int mTzSelected;
    int mTzScrollTop;
    // Time Zone 3D-globe selector state (shared by the setup wizard step + the
    // XMB Date and Time -> Time Zone view). mPs3TzActive gates the XMB view;
    // mTzGlobeFadeStart drives the 360ms XMB->globe cross-fade (mEffectTime when
    // the screen opened, <0 = no fade / fully shown).
    bool mPs3TzActive = false;
    float mTzGlobeFadeStart = -1.0f;
    int mTzGlobeWarmFrames = -1;  // >=0 while warming up (loading textures): hold the
                                  // globe on black until ready so the one-time texture
                                  // load spike isn't charged against the 360ms fade
    int mTzSelOnOpen = 0;         // mTzSelected when the globe opened (restore on cancel)
    // Setup's time zone step, online: the clock is set from the internet and the zone looked up from
    // the connection (mTzAutoState 0 not started, 1 looking, 2 found, 3 not found / offline). The
    // found zone (mTzAutoId, its standard offset mTzAutoOffMin) is selected as list row mTzAutoSel,
    // a row of its own when the list has none, unless the user has moved the cursor first.
    std::atomic<int> mTzAutoState{0};
    std::string mTzAutoId;
    int mTzAutoOffMin = 0;
    bool mTzAutoClockSet = false;
    int mTzAutoSel = -1;
    bool mTzAutoShown = false;
    bool mTzUserMoved = false;
    static bool setClockFromInternet();     // network time (blocking): true when the clock was set
    static int tzStandardOffset(const std::string& id);
    int tzRowFor(const std::string& id, int stdOffMin);
    void tzAutoDetectAsync();
    void tzAutoDetectTick();
    std::string tzSetupChoiceId() const;   // the zone the setup step applies
    void setupApplyTimezone(const std::string& tz);
    void applySetupClockFormats(const std::string& tz);   // US zone: MM/DD/YYYY + 12-hour
    void setSetupClockFormats(int dateIdx, int timeIdx);
    // Setup's Date and Time screen: the selected row (0 Time Zone, 1 Date Format, 2 Time Format,
    // 3 Continue), whether the found zone has been applied, and whether the time zone step was
    // opened from it (it then returns there).
    int mSetupDtSel = 3;
    bool mSetupDtApplied = false;
    bool mSetupTzFromDt = false;
    bool mSetupTransitionBack = false;   // the running transition slides as going back
    GLuint mPs3TzHeaderTex = 0;   // cached xmb_icon_022 colour texture (header glyph)
    // System Language picker (Settings -> System Settings -> System Language).
    bool mPs3LangActive = false;  // gates the XMB fullscreen language picker
    int mLangSelOnOpen = 0;       // mLangSelected when the picker opened (revert on cancel)
    float mPs3LangAnim = 0.0f;    // 0->1 open transition (frosted backdrop + fade-in)
    // Setup script log tailing
    std::vector<std::string> mSetupLogLines;
    // Cached word-wrapped tail of the install log (text + colour kind). Re-wrapped
    // only when the log grows or the frame width changes so the running setup
    // script never makes the Configuring screen re-wrap every frame.
    std::vector<std::pair<std::string, int>> mSetupLogRows;
    int   mSetupLogRowsForLines = -1;
    float mSetupLogRowsForW = -1.0f;
    int mSetupLogScrollTop;
    bool mSetupScriptRunning;
    bool mSetupScriptDone;
    // Defer startSetupScript() until the enter-transition into the installing step
    // has fully faded in AND a short grace period has passed, so setup.sh's CPU
    // spike does not stutter the animation or its final presented frame.
    bool mSetupScriptPending = false;
    int64_t mSetupScriptReadyMs = 0;   // 0 = grace not armed yet
    int64_t mSetupScriptDoneMs = 0;    // when setup.sh finished (uptime ms); the installing step auto-advances 10 s later
    std::thread mSetupLogThread;
    std::mutex mSetupLogMutex;
    bool mSetupLogExitRequested;

    // Setup's microSD and BIOS steps, and Game Settings > Install Your Own BIOS Files (NanoBiosUi.cpp).
    // The BIOS dialog lists the files the emulators use (biosDialogRows), each row's other versions
    // (Alternates), everything the installer takes (All Supported Files), then installs from a
    // folder (the folder browser's entries, in the dialog) with progress and a report. In setup it is the
    // SETUP_BIOS page; from Game Settings it is a modal over the menu (mBiosDlgActive).
    enum BiosDlgMode { BIOS_LIST = 0, BIOS_ALTS, BIOS_PICK, BIOS_PROGRESS, BIOS_REPORT, BIOS_ALL };
    struct BiosLine {
        std::string text; int style = 0; int row = -1; float indent = 0.0f;
        bool link = false;                  // the row's Alternates link at its right
        int linkFrom = -1, linkTo = -1;     // bytes of text that are the All Supported Files link
    };
    bool mBiosDlgActive = false;            // the Game Settings modal is up
    bool mBiosDlgSetup = false;             // the dialog is setup's page
    int  mBiosDlgMode = BIOS_LIST;
    std::vector<BiosDialogRow> mBiosRows;
    std::vector<BiosLine> mBiosLines;       // the page, wrapped
    std::vector<int> mBiosRowFirst, mBiosRowLast;   // a row's (list) or entry's (picker) lines
    float mBiosLinesFs = -1.0f, mBiosLinesW = -1.0f;
    int   mBiosLinesMode = -1;
    bool  mBiosLinesDirty = true;
    int   mBiosScroll = 0;                  // first line shown
    int   mBiosVisible = 1;                 // lines that fit (last frame)
    int   mBiosSel = 0;                     // list: a row, or rows + button; picker: an entry
    int   mBiosAltRow = -1;                 // the row the Alternates page shows
    int   mBiosLinkFirst = -1, mBiosLinkLast = -1;   // the lines of the All Supported Files sentence
    Ps3Level mBiosPick;                     // setup's folder list (buildFolderBrowser)
    BiosInstallProgress mBiosReport;        // the finished install
    int64_t mBiosProgStartMs = 0;           // the progress page opened (uptime ms)
    bool    mBiosWaitEarlier = false;       // the progress page waits for a re-install to end
    bool    mBiosCancelAsked = false;       // Cancel pressed on the progress page
    void biosDlgCancel();
    void biosDrawButtons(float x, float w, float btnCy, float fs, const float* pal, float a);
    void biosBuildPick(const std::string& path);
    bool biosDlgRepick(int dir);            // the focus off the page: a row (or the link) in view
    bool biosWorkerBusy() const { return mBios && mBios->running.load(); }   // launches wait, overlay ones too
    bool biosDlgShown() const { return mBiosDlgActive || (mBiosDlgSetup && mSetupWizardActive && mSetupStep == SETUP_BIOS); }
    void biosDialogOpen(bool setup);
    void biosDialogClose();
    void biosFolderChosen(const std::string& path);   // the picker's Select This Folder
    void biosDlgTick();
    void biosDlgNav(int dir);               // Up / Down
    void biosDlgHoriz(int dir);             // Left / Right
    void biosDlgPage(int dir);              // L1 / R1
    void biosDlgSelect();
    void biosDlgBack();
    bool biosDlgKey(uint16_t code);         // the buttons the dialog takes (not the d-pad); true = taken
    bool biosDlgScroll(int dir);
    void biosDlgActivate(int focus);        // a row, a button or a picker entry (key or tap)
    void biosDlgSetMode(int mode);
    void biosDlgEnsureVisible();
    void biosBuildLines(float fs, float maxW);
    void biosDrawPage(float x, float y, float w, float h, float fs, float btnCy, const float* pal, float a);
    void renderBiosDialog();                // the modal, over any theme
    void renderSetupBios();
    // SETUP_SDCARD / SETUP_SDMOVE.
    bool mSetupSdEsde = false, mSetupSdEmu = false;   // the two options (Off unless turned on, ES-DE On
                                                      // when the card has an ES-DE folder)
    bool mSetupSdHasEsde = false;           // the card has an ES-DE folder
    bool mSetupSdHasEmu = false;            // the card has emulator_config (emulator files On)
    int  mSetupSdSel = 0;                   // 0 ES-DE, 1 emulator files, 2 Continue
    int  mSetupMovePhase = 0;               // 0 ES-DE, 1 its job, 2 emulators, 3 their job, 4 done
    int64_t mSetupMoveDoneMs = 0;
    void renderSetupSdCard();
    void renderSetupSdMove();
    void setupSdCardSelect();
    void setupSdMoveTick();
    // The moves without questions (setup): only what has files, card copies kept, internal ones deleted.
    bool mSdMoveAuto = false;
    bool mSdMoveAdopt = false;   // a card adopted at the home screen (emuSdTick): auto, but said in banners
    // Setup: a move's internal copies being deleted on a worker that never touches this object;
    // the handle's destructor (with NanoMenu's) tells it to stop, what is left stays in the backup.
    struct SdAutoDelState { std::atomic<bool> cancel{false}, done{false}; };
    struct SdAutoDelHandle {
        std::shared_ptr<SdAutoDelState> st;
        ~SdAutoDelHandle() { if (st) st->cancel.store(true); }
    };
    SdAutoDelHandle mSdAutoDel;
    bool sdAutoDeleting() const { return mSdAutoDel.st && !mSdAutoDel.st->done.load(); }
    std::string mSdMoveAutoNote;            // what the moves said, for the setup page
    void sdMoveNotice(const std::string& msg);   // a banner, or (setup) a line for its page
    bool esdeMoveAuto();                    // true when a move job started
    bool emuMoveAuto();
    // Emulator files kept on the card (NanoSdMove.cpp, NanoEmuSync.cpp).
    bool emuSdMode();
    int emuSettingsSync(const std::string& card, const std::string& onlyPkg, bool launching, bool appWins,
                        bool adopt = false);
    std::string emuSdSlotCard();
    void emuSettingsSyncAsync(const std::string& card, bool appWins = false, bool adopt = false);
    void emuSettingsSyncLaunch(const std::string& pkg);
    void emuSdTick();
    void emuSdAdoptBegin(const std::string& card, bool appWins);
    void emuSdForeignDialog();
    void emuSdForeignChoice(int sel);
    void emuSdAdoptDone(bool ok);
    // A card being adopted: launches wait while its settings sync or its move runs (files moving
    // under the emulators), or while it waits to move (not when other work holds it up).
    bool emuSdAdopting() const {
        return (mEmuSdAdoptPending && (mEmuSyncRunning.load() > 0 || !mEmuSdAdoptStalled))
               || (mFileJobKind == 4 && (mSdMoveAdopt || !mEmuSdAdoptingId.empty()));
    }
    std::atomic<int> mEmuSyncRunning{0};
    std::atomic<bool> mEmuSyncDone{false};   // a sync ended: the home looks without waiting
    std::atomic<bool> mEmuSdAdoptSynced{false};   // the adoption's sync ran (repointed what names an old card)
    std::atomic<int64_t> mLaunchHandOffMs{0};   // a game being handed off: no move or sync starts under it
    std::string mEmuSdAdoptCard;   // the card mEmuSdAdoptPending is for
    bool mEmuSdAsking = false;     // the card question is up
    bool mEmuMoveBusy = false;     // emuMoveStart found another move or a sync running (tried again)
    bool mEmuMoveRefused = false;  // emuMoveStart would not move (no card, a folder not the emulator's)
    int mEmuMoveLockFd = -1;       // a move of emulator files holds it (kEmuMoveLock) until it ends
    std::atomic<int> mEmuSyncRestored{0};   // files the syncs brought into apps (setup's page)
    std::string mEmuSdCard;   // the card this Nano looked at ("" none yet, or out)
    int64_t mEmuSdCheckMs = 0, mEmuSdSyncMs = 0;
    bool mEmuSdAdoptPending = false;
    bool mEmuSdAppWins = false;   // the card being adopted takes this device's files
    bool mEmuSdRecheck = false;   // emulators not yet pointed at the adopted card are looked for again
    bool mSdMoveQuiet = false;    // that check's notices only go to the log
    bool mSdMoveQuietJob = false; // the move it started: its notices too
    bool mEmuSdAdoptStalled = false;   // the adoption waits on a scrape, a BIOS install or setup
    std::string mEmuSdAdoptingId;      // the card the adoption's move is for (kEmuSdCardProp once done)
    int mEmuSdLockFd = -1;
};

} // namespace android

#endif // GAMMAOS_NANO_MENU_H
