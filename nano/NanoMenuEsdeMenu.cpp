// GammaOS Nano - ES-DE start menu (the options menu opened with the Start/menu button when
// the ES-DE home theme is active). Additive and gated on mEsdeTheme; inert for XMB/DSi/Minima.
//
// Mirrors ES-DE's GuiMenu UI-settings subset in ES-DE's own row order: THEME, THEME VARIANT,
// THEME COLOR SCHEME, THEME FONT SIZE, THEME ASPECT RATIO. Each row shows the current value
// (upper-cased like ES-DE's menu); Left/Right cycle it in
// place, A opens a full-list picker, and choosing a value writes the persist.gammaos.nano.esde.*
// prop and live-reloads the theme through ensureEsdeTheme(). The panel geometry is expressed in
// fractions of the screen so it scales to any panel, matching ES-DE's MenuComponent.
#define LOG_TAG "GammaOSNano"

#include "NanoMenu.h"
#include "NanoEsdeTheme.h"
#include "NanoI18n.h"
#include "NanoUtf8Case.h"
#include "NanoMenuShaders.h"   // FONT_CHAR_H
#include "NanoVideo.h"         // NanoVideo::probe for a game's video
#include "NanoJson.h"          // njson: themes.json parsing
#include "NanoLaunch.h"        // isValidRepoName

#include <cutils/properties.h>
#include <utils/SystemClock.h>
#include <log/log.h>

#include <dirent.h>
#include <linux/input.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <GLES2/gl2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace android {

// The five ES-DE theme-option rows on the root page. Kept in one table so the renderer and the
// input handlers agree on order, labels and the prop each row writes.
struct EsdeMenuRowDef { const char* label; const char* prop; };
// Row order matches ES-DE's UI Settings menu: theme, variant, colour scheme, font size, aspect
// ratio (ES-DE also has Theme Transitions and Theme Language below these, which nano does not
// implement yet).
static const EsdeMenuRowDef kEsdeMenuRows[] = {
    {"THEME",              "persist.gammaos.nano.esde.themeset"},
    {"THEME VARIANT",      "persist.gammaos.nano.esde.variant"},
    {"THEME COLOR SCHEME", "persist.gammaos.nano.esde.colorscheme"},
    {"THEME FONT SIZE",    "persist.gammaos.nano.esde.fontsize"},
    {"THEME ASPECT RATIO", "persist.gammaos.nano.esde.aspectratio"},
};
static const int kEsdeMenuRowCount = (int)(sizeof(kEsdeMenuRows) / sizeof(kEsdeMenuRows[0]));

// Below the theme-option rows the root page carries nano action rows (A only): the focused
// platform's settings, GAME SYSTEMS and BOXART SCRAPER (each opens that settings screen over the
// ES-DE home), APPLICATIONS (an app launcher) and NANO SETTINGS (hands off to the XMB menu stack).
// Row 0 is the theme downloader. Naming the indices keeps the renderer and the input handlers in
// agreement instead of scattering raw +1/+2 offsets (which broke every time a row was inserted).
static const int kEsdeRowPlatform     = kEsdeMenuRowCount + 1;
static const int kEsdeRowSystems      = kEsdeMenuRowCount + 2;
static const int kEsdeRowScraper      = kEsdeMenuRowCount + 3;
static const int kEsdeRowApps         = kEsdeMenuRowCount + 4;
static const int kEsdeRowNanoSettings = kEsdeMenuRowCount + 5;
static const int kEsdeRootRows        = kEsdeMenuRowCount + 6;

// ES-DE menu colors are a GLOBAL scheme (dark/darkred/light in ViewController::setMenuColors),
// NOT theme-driven, so the panel looks the same across every theme. Mirror all three with their
// exact constants, selected by persist.gammaos.nano.esde.menucolorscheme (default dark, matching
// ES-DE's own default). The selected row is a solid bar in the selector colour (black / dark red /
// white) with the text kept at its normal shade - ES-DE never brightens the selected row.
// A Background Colour (persist.gammaos.nano.minima.bg) replaces the frame, with a darker shade
// of it for the selector and text dark or light to read on it.
struct EsdeMenuCol { float frame[3], title, prim, sec, sel[3], sep[3]; };
static EsdeMenuCol esdeMenuColors() {
    char bg[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.gammaos.nano.minima.bg", bg, "none");
    unsigned int br = 0, bgg = 0, bb = 0;
    if (strlen(bg) >= 6 && sscanf(bg, "%2x%2x%2x", &br, &bgg, &bb) == 3) {
        const float r = br / 255.0f, g = bgg / 255.0f, b = bb / 255.0f;
        const bool light = 0.299f * r + 0.587f * g + 0.114f * b > 0.55f;
        auto shade = [](float c, float k) { return std::max(0.0f, std::min(1.0f, c * k)); };
        auto lift = [](float c, float d) { return std::max(0.0f, std::min(1.0f, c + d)); };
        if (light)
            return {{r, g, b}, 0.33f, 0.40f, 0.47f, {shade(r, 0.82f), shade(g, 0.82f), shade(b, 0.82f)},
                    {shade(r, 0.88f), shade(g, 0.88f), shade(b, 0.88f)}};
        return {{r, g, b}, 0.56f, 0.62f, 0.68f, {shade(r, 0.55f), shade(g, 0.55f), shade(b, 0.55f)},
                {lift(r, 0.09f), lift(g, 0.09f), lift(b, 0.09f)}};
    }
    char msch[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.gammaos.nano.esde.menucolorscheme", msch, "dark");
    auto hx = [](int v) { return v / 255.0f; };
    // ES-DE ViewController::setMenuColors: separators are 0xC6C7C6 (light) / 0x303030 (dark, darkred).
    if (!strcmp(msch, "light"))                   // 0xEFEFEF / 0x555555 / 0x777777 / 0x888888 / 0xFFFFFF
        return {{hx(0xEF), hx(0xEF), hx(0xEF)}, hx(0x55), hx(0x77), hx(0x88), {1.0f, 1.0f, 1.0f}, {hx(0xC6), hx(0xC7), hx(0xC6)}};
    if (!strcmp(msch, "darkred"))                 // dark frame/text, 0x461816 selector
        return {{hx(0x19), hx(0x19), hx(0x19)}, hx(0x90), hx(0x80), hx(0x93), {hx(0x46), hx(0x18), hx(0x16)}, {hx(0x30), hx(0x30), hx(0x30)}};
    // dark (default): 0x191919 / 0x909090 / 0x808080 / 0x939393 / 0x000000
    return {{hx(0x19), hx(0x19), hx(0x19)}, hx(0x90), hx(0x80), hx(0x93), {0.0f, 0.0f, 0.0f}, {hx(0x30), hx(0x30), hx(0x30)}};
}

// ES-DE displays an aspect ratio the same as its key except that a "<ratio>_vertical" key is
// shown as "<ratio> vertical" (see ThemeData::sSupportedAspectRatios). Mirror that so the menu
// reads "5:3 vertical" rather than the raw "5:3_vertical".
static std::string esdeAspectDisplay(const std::string& key) {
    const std::string suf = "_vertical";
    if (key.size() > suf.size() && key.compare(key.size() - suf.size(), suf.size(), suf) == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), trDyn("%s vertical"), key.substr(0, key.size() - suf.size()).c_str());
        return buf;
    }
    return key;
}

// ES-DE's menu renders every option value in upper case; the theme name additionally shows its
// directory hyphens as spaces ("art-book-next" -> "ART BOOK NEXT"). This formats the DISPLAY string
// only - the raw value (opts.first) is left intact for prop matching.
static std::string esdeMenuDisplay(std::string s, bool themeName) {
    if (themeName)
        for (auto& c : s) if (c == '-') c = ' ';
    return nanoUtf8Upper(s);
}

// ES-DE spells out the two extreme font-size keys in its menus (ThemeData sSupportedFontSizes:
// x-large -> "extra large", x-small -> "extra small"); every other key shows verbatim. Mirror
// that mapping before upper-casing so the row reads "EXTRA LARGE" rather than "X-LARGE".
static std::string esdeFontSizeDisplay(const std::string& key) {
    if (key == "x-large") return trDyn("EXTRA LARGE");
    if (key == "x-small") return trDyn("EXTRA SMALL");
    return esdeMenuDisplay(key, false);
}

// Build the option list (value + display label) and current index for a root row. Values come
// from the parsed capabilities of the active set (variants/colorSchemes/fontSizes) or, for the
// THEME row, the enumerated installed sets. Returns the current selected index (>=0).
int NanoMenu::esdeMenuOptionCtx(int row, std::string& label, std::string& prop,
                                std::vector<std::pair<std::string, std::string>>& opts) {
    opts.clear();
    if (row < 0 || row >= kEsdeMenuRowCount) { label.clear(); prop.clear(); return 0; }
    label = kEsdeMenuRows[row].label;
    prop = kEsdeMenuRows[row].prop;
    const nanoesde::Capabilities& caps = mEsdeDoc.capabilities();

    if (row == 0) {                                   // THEME: installed sets
        // ES-DE labels a set by its <themeName> ("Slate"), not the directory ("slate-es-de").
        for (auto& s : mEsdeInstalled) {
            const std::string& tn = s.caps.themeName;
            opts.push_back({s.name, tn.empty() ? esdeMenuDisplay(s.name, true)
                                               : esdeMenuDisplay(tn, false)});
        }
    } else if (row == 1) {                            // VARIANT
        // ES-DE lists only selectable variants; trigger-only override variants (selectable=false)
        // never appear in the menu (ThemeData parseThemeCapabilities / GuiMenu).
        for (auto& v : caps.variants) {
            if (!v.selectable) continue;
            opts.push_back({v.name, esdeMenuDisplay(v.label.empty() ? v.name : v.label, false)});
        }
    } else if (row == 2) {                            // COLOR SCHEME
        for (auto& c : caps.colorSchemes)
            opts.push_back({c.name, esdeMenuDisplay(c.label.empty() ? c.name : c.label, false)});
    } else if (row == 3) {                            // FONT SIZE
        for (auto& f : caps.fontSizes) opts.push_back({f, esdeFontSizeDisplay(f)});
    } else if (row == 4) {                            // ASPECT RATIO ("automatic" + declared)
        opts.push_back({"automatic", trDyn("AUTOMATIC")});
        for (auto& a : caps.aspectRatios) {
            if (a == "automatic") continue;           // de-dupe: a theme may also declare it
            opts.push_back({a, esdeMenuDisplay(esdeAspectDisplay(a), false)});
        }
    }

    // Current value from the prop (THEME uses the loaded set name).
    std::string cur;
    if (row == 0) {
        cur = mEsdeSetName;
    } else {
        char buf[PROPERTY_VALUE_MAX] = {0};
        property_get(prop.c_str(), buf, row == 4 ? "automatic" : "");
        cur = buf;
    }
    for (int i = 0; i < (int)opts.size(); i++)
        if (opts[i].first == cur) return i;
    return 0;
}

// Enumerate installed theme sets (a dir under a theme root with a capabilities.xml). Data root
// wins over the system root. Cheap (one XML per set via parseCapabilities); run on menu open and
// after an install, never per frame.
void NanoMenu::esdeMenuEnumerateInstalled() {
    mEsdeInstalled.clear();
    const std::string roots[] = {"/data/system/nano_esde_themes", "/system/etc/nano_esde_themes", esdeDataDir() + "/themes"};
    for (const std::string& root : roots) {
        DIR* d = opendir(root.c_str());
        if (!d) continue;
        for (dirent* e = readdir(d); e; e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string dir = std::string(root) + "/" + e->d_name;
            if (access((dir + "/capabilities.xml").c_str(), F_OK) != 0) continue;
            bool dup = false;
            for (auto& s : mEsdeInstalled) if (s.name == e->d_name) { dup = true; break; }
            if (dup) continue;                        // data root already provided this name
            EsdeInstalledSet s;
            s.dir = dir; s.name = e->d_name;
            s.caps = nanoesde::Theme::parseCapabilities(dir);
            mEsdeInstalled.push_back(std::move(s));
        }
        closedir(d);
    }
    std::sort(mEsdeInstalled.begin(), mEsdeInstalled.end(),
              [](const EsdeInstalledSet& a, const EsdeInstalledSet& b) {
                  std::string la = a.name, lb = b.name;
                  for (auto& c : la) c = tolower((unsigned char)c);
                  for (auto& c : lb) c = tolower((unsigned char)c);
                  return la < lb;
              });
}

void NanoMenu::esdeMenuOpen() {
    cancelPendingLaunch();           // a deferred A press would land on a menu row
    ensureEsdeTheme();               // make sure caps reflect the current set
    esdeMenuEnumerateInstalled();
    mEsdeMenuActive = true;
    mEsdeMenuClosing = false;
    mEsdeMenuPage = ESDE_PG_ROOT;
    mEsdeMenuSel = 0;
    mEsdeMenuScroll = 0;
    mEsdeMenuAnim = 0.0f;
    mDisplayDirty = true;
}

void NanoMenu::esdeMenuClose() {
    cancelPendingLaunch();           // a deferred menu choice would land on the home
    mEsdeMenuClosing = true;         // render keeps drawing during the fade-out
    mEsdeMenuActive = false;
    mDisplayDirty = true;
}

// Write a chosen value and live-reload. Clearing mEsdeLoaded defeats ensureEsdeTheme's
// themeset-only early-out so a variant/colorScheme/aspect/font change actually reparses.
void NanoMenu::esdeMenuApplyOption(const char* prop, const std::string& value) {
    property_set(prop, value.c_str());
    mEsdeLoaded = false;
    ensureEsdeTheme();
    mEsdeInGamelist = false;         // layout may have changed; return to the system view
    mEsdeCamCursor = -1;             // resnap the carousel to the reloaded set
    mEsdeOtherViewAnim = EsdeViewAnim();
    mDisplayDirty = true;
}

// Number of rows on the current page. The root page is the downloader row (0), the five option
// rows, then the Nano Settings row (last); the downloader page lists the fetched themes; a picker
// lists its options.
int NanoMenu::esdeMenuPageRows() {
    if (mEsdeMenuPage == ESDE_PG_ROOT) return kEsdeRootRows;
    if (mEsdeMenuPage == ESDE_PG_DOWNLOADER) {
        std::lock_guard<std::mutex> lk(mEsdeDlMutex);
        return (int)mEsdeDlList.size();
    }
    if (mEsdeMenuPage == ESDE_PG_APPS) return (int)mAppEntries.size();
    return (int)mEsdeMenuPickOpts.size();
}

void NanoMenu::esdeMenuMove(int dir) {
    mTouch.stopFling();
    int n = esdeMenuPageRows();
    if (n <= 0) return;
    mEsdeMenuSel = ((mEsdeMenuSel + dir) % n + n) % n;
    mDisplayDirty = true;
}

// Left/Right on the root page cycle the focused option's value in place and apply immediately
// (ES-DE OptionListComponent inline < > behavior). The downloader row (0) is not cyclable.
void NanoMenu::esdeMenuCycle(int dir) {
    if (mEsdeMenuPage != ESDE_PG_ROOT) { esdeMenuMove(dir); return; }
    if (mEsdeMenuSel == 0) return;                          // THEME DOWNLOADER row
    if (mEsdeMenuSel >= kEsdeRowPlatform) return;           // the action rows (A-only)
    int optRow = mEsdeMenuSel - 1;
    std::string label, prop;
    std::vector<std::pair<std::string, std::string>> opts;
    int cur = esdeMenuOptionCtx(optRow, label, prop, opts);
    if (opts.size() < 2) return;
    int next = ((cur + dir) % (int)opts.size() + (int)opts.size()) % (int)opts.size();
    esdeMenuApplyOption(prop.c_str(), opts[next].first);
    if (optRow == 0) esdeMenuEnumerateInstalled();  // THEME change re-enumerates sets
    mDisplayDirty = true;
}

// A: on root, open the downloader or the focused option's full-list picker; on a picker, commit;
// on the downloader, install the selected theme.
void NanoMenu::esdeMenuSelect() {
    if (mEsdeMenuPage == ESDE_PG_ROOT) {
        if (mEsdeMenuSel == 0) {                     // open the theme downloader
            mEsdeMenuPage = ESDE_PG_DOWNLOADER;
            mEsdeMenuSel = 0; mEsdeMenuScroll = 0;
            bool empty;
            { std::lock_guard<std::mutex> lk(mEsdeDlMutex); empty = mEsdeDlList.empty(); }
            if (empty && !mEsdeDlFetching.load()) esdeDlStartFetch();
        } else if (mEsdeMenuSel == kEsdeRowPlatform || mEsdeMenuSel == kEsdeRowSystems
                   || mEsdeMenuSel == kEsdeRowScraper) {   // a game-settings screen over the home
            esdeOpenGameSettings(mEsdeMenuSel == kEsdeRowPlatform ? 0 : mEsdeMenuSel == kEsdeRowSystems ? 1 : 2);
            if (!mEsdeMenuActive) { mEsdeMenuReturn = true; mEsdeMenuReturnSel = mEsdeMenuSel; }
            return;
        } else if (mEsdeMenuSel == kEsdeRowApps) {   // APPLICATIONS: open the app launcher list
            // With the Minima settings theme it is the Minima Applications grid, lent like the settings
            // screens; Back returns to this row.
            char stv[PROPERTY_VALUE_MAX] = {};
            property_get("persist.gammaos.nano.esde.settingstheme", stv, "xmb");
            if (!strcmp(stv, "minima")) {
                const int row = mEsdeMenuSel;
                openApplicationsView();
                if (applicationsViewOpen()) { mEsdeMenuReturn = true; mEsdeMenuReturnSel = row; }
                return;
            }
            if (mAppEntries.empty()) loadInstalledApps();
            mEsdeMenuPage = ESDE_PG_APPS;
            mEsdeMenuSel = 0; mEsdeMenuScroll = 0;
        } else if (mEsdeMenuSel == kEsdeRowNanoSettings) {   // NANO SETTINGS: hand off to the XMB menus
            esdeOpenNanoSettings();
            return;
        } else {                                     // open an option picker
            int optRow = mEsdeMenuSel - 1;
            std::string label, prop;
            std::vector<std::pair<std::string, std::string>> opts;
            int cur = esdeMenuOptionCtx(optRow, label, prop, opts);
            if (opts.empty()) { mDisplayDirty = true; return; }
            mEsdeMenuPage = (EsdeMenuPage)(ESDE_PG_THEME + optRow);
            mEsdeMenuPickOpts = opts;
            mEsdeMenuPickProp = prop;
            mEsdeMenuPickTitle = label;
            mEsdeMenuSel = cur;
            mEsdeMenuScroll = 0;
        }
    } else if (mEsdeMenuPage == ESDE_PG_DOWNLOADER) {
        if (mEsdeDlInstalling.load() || mEsdeDlFetching.load()) { mDisplayDirty = true; return; }
        EsdeDlEntry e; bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mEsdeDlMutex);
            if (mEsdeMenuSel >= 0 && mEsdeMenuSel < (int)mEsdeDlList.size()) {
                e = mEsdeDlList[mEsdeMenuSel]; ok = true;
            }
        }
        if (ok && !e.installed) esdeDlStartInstall(e);
    } else if (mEsdeMenuPage == ESDE_PG_APPS) {      // app launcher: start the selected app
        if (mEsdeMenuSel >= 0 && mEsdeMenuSel < (int)mAppEntries.size()) {
            std::string pkg = mAppEntries[mEsdeMenuSel].packageName;
            // Still booting: keep the menu up so the deferred retry selects this row again.
            if ((!mOverlayMode && !isLaunchReady()) || biosWorkerBusy()) { showLaunchBusyToast(); return; }
            esdeMenuClose();                         // dismiss the overlay before launching
            if (panelLaunch(pkg)) return;            // dual screen: on the Cube's screen
            launchInstalledApp(pkg, false);          // the cold-boot home must hand off, not just start it
        }
        return;
    } else {                                         // picker: commit the choice
        int i = mEsdeMenuSel;
        if (i >= 0 && i < (int)mEsdeMenuPickOpts.size()) {
            bool wasTheme = (mEsdeMenuPage == ESDE_PG_THEME);
            esdeMenuApplyOption(mEsdeMenuPickProp.c_str(), mEsdeMenuPickOpts[i].first);
            if (wasTheme) esdeMenuEnumerateInstalled();
        }
        mEsdeMenuSel = ((int)mEsdeMenuPage - (int)ESDE_PG_THEME) + 1;   // back onto the option row
        mEsdeMenuPage = ESDE_PG_ROOT;
        mEsdeMenuScroll = 0;
    }
    mDisplayDirty = true;
}

// B: from a picker or the downloader return to root; at the root, close the menu (as Start does).
void NanoMenu::esdeMenuBack() {
    if (mEsdeMenuPage == ESDE_PG_DOWNLOADER) {
        mEsdeMenuPage = ESDE_PG_ROOT; mEsdeMenuSel = 0; mEsdeMenuScroll = 0;
        mDisplayDirty = true;
    } else if (mEsdeMenuPage == ESDE_PG_APPS) {
        mEsdeMenuPage = ESDE_PG_ROOT; mEsdeMenuSel = kEsdeRowApps; mEsdeMenuScroll = 0;
        mDisplayDirty = true;
    } else if (mEsdeMenuPage != ESDE_PG_ROOT) {
        mEsdeMenuSel = ((int)mEsdeMenuPage - (int)ESDE_PG_THEME) + 1;
        mEsdeMenuPage = ESDE_PG_ROOT;
        mEsdeMenuScroll = 0;
        mDisplayDirty = true;
    } else {
        esdeMenuClose();
    }
}

// Y in Applications: the XMB's Add to / Remove from Android Games option, which ES-DE's menu has no
// option menu to hold.
void NanoMenu::esdeMenuToggleAndroidGame() {
    if (mEsdeMenuPage != ESDE_PG_APPS || mEsdeMenuSel < 0 || mEsdeMenuSel >= (int)mAppEntries.size())
        return;
    const AppEntry& app = mAppEntries[mEsdeMenuSel];
    if (app.packageName == "org.thimsy.androidgames") return;
    androidGameToggle(app.packageName, app.label);
    mDisplayDirty = true;
}

// Start, the Anbernic button or Home in the menu or a screen opened from it: back to the game list.
void NanoMenu::esdeMenuExitAll() {
    mAppsReturn = QsReturn(); mAppsSummoned = false;
    mEsdeMenuReturn = false;
    if (mPs3OptActive) closeXmbOpt();
    if (mPs3DlgActive) closePs3Dialog(false);
    mPs3Stack.clear();
    if (mEsdeMenuActive) esdeMenuClose();
    esdeApplySettingsTheme();
    mDisplayDirty = true;
}

// Back out of the last level of a screen opened from the menu: the menu again, on that row.
void NanoMenu::esdeMenuReopen() {
    const int sel = mEsdeMenuReturnSel;
    mEsdeMenuReturn = false;
    esdeApplySettingsTheme();
    esdeMenuOpen();
    if (sel >= 0 && sel < esdeMenuPageRows()) mEsdeMenuSel = sel;
}

// "Nano Settings" row: leave the ES-DE options overlay and hand off to the shared XMB menu stack,
// which the ES-DE home already renders (renderPs3Xmb) and navigates (mPs3Stack) when non-empty -
// the same path the power-hold Quick Menu uses. Push a small chooser with the two menus the built-in
// homes reach from their carousel: the Quick Menu (global actions) and the full Settings tree. Back
// pops the chooser and returns to the ES-DE home automatically (handleBack).
void NanoMenu::esdeOpenNanoSettings() {
    mEsdeMenuActive = false; mEsdeMenuClosing = false; mEsdeMenuAnim = 0.0f;
    if (!mPs3MenuBuilt) initPs3Menu();
    // Clean, top-level XMB state behind the pushed chooser (mirrors openQuickPowerMenu).
    mPs3Stack.clear();
    mPs3DlgActive = false; mPs3OptActive = false; mPs3BrightSlider = false;
    mMenuState = MENU_MAIN;
    int landIdx = (mPs3QuickCatIdx >= 0) ? mPs3QuickCatIdx
                : (mPs3SettingsCatIdx >= 0) ? mPs3SettingsCatIdx : 0;
    if (landIdx >= 0 && landIdx < (int)mPs3Cats.size()) {
        mPs3CatIdx = landIdx;
        mPs3CatItemSel[mPs3CatIdx] = 0;
    }
    mPs3ItemIdx = 0;
    mNdsAtRoot = false;
    Ps3Level lvl; lvl.title = "Nano Settings"; lvl.sel = 0;
    auto add = [&](const char* label, int catIdx, int icon) {
        if (catIdx < 0 || catIdx >= (int)mPs3Cats.size()) return;
        Ps3Item it; it.label = label; it.kind = PS3_CAT_SUBMENU; it.a = catIdx;
        it.iconTex = iconTexForIcon(icon); it.nmapTex = nmapForIcon(icon);
        it.iconR = it.iconG = it.iconB = 1.0f;
        lvl.items.push_back(it);
    };
    add("Quick Menu",    mPs3QuickCatIdx,    54);   // power glyph
    add("Full Settings", mPs3SettingsCatIdx, 44);   // settings glyph
    if (lvl.items.empty()) { mDisplayDirty = true; return; }   // no categories built -> stay on ES-DE
    mPs3Stack.push_back(lvl);
    mEsdeMenuReturn = true; mEsdeMenuReturnSel = mEsdeMenuSel;
    mNdsCamera = (float)ndsFocusSel(); mNdsScrubbing = false; mNdsFlingVel = 0.0f;
    esdeApplySettingsTheme();
    ps3Sfx(PS3_SFX_OK);
    mDisplayDirty = true;
}

// The platform row names the focused system ("N64 SETTINGS"), from its short name like ES-DE's own
// system names.
std::string NanoMenu::esdeMenuPlatformLabel() {
    esdeRebuildSysList();
    std::string name = trDyn("PLATFORM");
    if (mEsdeSysSel >= 0 && mEsdeSysSel < (int)mEsdeSysList.size()) {
        const XmbSystem& sys = mXmbSystems[mEsdeSysList[mEsdeSysSel]];
        name = sys.shortname.empty() ? sys.name : sys.shortname;
    }
    name = nanoUtf8Upper(name);
    char buf[160];
    snprintf(buf, sizeof(buf), trDyn("%s SETTINGS"), name.c_str());
    return buf;
}

// Settings Theme (persist.gammaos.nano.esde.settingstheme): with "minima", the ES-DE home lends its
// settings screens (the mPs3Stack levels) to the Minima theme. Like a live Home Theme switch this
// flips the theme flags in memory only; esdeSettingsThemeTick flips them back once the settings close.
void NanoMenu::esdeApplySettingsTheme() {
    if (!mEsdeTheme && !mEsdeSettingsMinima) return;
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.esde.settingstheme", v, "xmb");
    const bool want = !strcmp(v, "minima") && !mPs3Stack.empty();
    if (want == mEsdeSettingsMinima) return;
    mEsdeSettingsMinima = want;
    mEsdeTheme = !want;
    mMinimaTheme = want;
    if (want) mNdsAtRoot = false;   // Minima draws the pushed level, not its categories root
    mMinimaScroll = 0.0f; mMinimaSelAnim = 0.0f; mMinimaPrevDepth = -1;
    mMinimaSfxDepth = -1; mMinimaSfxSel = -1; mMinimaTransStart = 0;
    mDisplayDirty = true;
}

// Called every frame: back on the ES-DE home once the last settings level has closed.
void NanoMenu::esdeSettingsThemeTick() {
    if (mPs3Stack.empty()) mEsdeMenuReturn = false;   // left the screen some other way
    if (mEsdeSettingsMinima && mPs3Stack.empty() && !ndsInModal()) esdeApplySettingsTheme();
}

// A game or app has exited and Nano is the home again (the overlay raised as the launcher, Close
// Current App / Kill All, the drastic-nano return): on the ES-DE home theme land on the ES-DE home
// itself, with no Nano level (Quick Menu, settings, Applications), dialog, option menu, keyboard or
// ES-DE menu left over from before the launch. The ES-DE system / game position is kept.
void NanoMenu::esdeReturnHome() {
    if (!mEsdeTheme && !mEsdeSettingsMinima) return;
    if (mOskActive) {
        mOskActive = false; mOsk.closing = false; mOsk.anim = 0.0f;
        mOskPasswordMode = false; mOskPlaintext = false;
        mOskPasswordPrompt.clear(); mOskPasswordCallback = nullptr; mOskQuery.clear();
    }
    mPs3DlgActive = mPs3OptActive = mPs3DlgClosing = mPs3OptClosing = false;
    mPs3WizActive = mPs3TzActive = mPs3LangActive = mPs3BrightSlider = false;
    if (mGSearchActive) gsearchClose();
    if (mMenuState == MENU_WIFI) closeWifiScreen();
    else if (mMenuState == MENU_BT) closeBtScreen();
    mMenuState = MENU_MAIN;
    esdeOverlayCloseAll();
    mPs3Stack.clear();
    mQsReturn = QsReturn();
    mAppsReturn = QsReturn(); mAppsSummoned = false;
    mEsdeMenuReturn = false;
    mEsdeMenuActive = false; mEsdeMenuClosing = false; mEsdeMenuAnim = 0.0f;
    esdeApplySettingsTheme();   // back from the Minima settings theme to the ES-DE home
    mDisplayDirty = true;
}

// ------------------------------------------------------------------ render

void NanoMenu::esdeDrawMenuBackdrop(float a) {
    const float W = (float)mWidth, H = (float)mHeight;
    // Background behind the options panel. ES-DE (Window.cpp) snapshots the home on
    // menu-open, runs a 2-pass gaussian when MenuBlurBackground is set (its default) and
    // multiplies the result by a dimming factor: 0.80 for the dark / darkred schemes,
    // 0.60 for light. core.glsl multiplies the sampled colour by `dimming`, so 0.80 means
    // 80% brightness (NOT an 80% dim). Mirror that exactly rather than the old flat scrim.
    // persist.gammaos.nano.esde.menucolorscheme picks the scheme (dark default, as ES-DE);
    // persist.gammaos.nano.esde.menublur mirrors MenuBlurBackground (default on) and lets
    // the blur fall back to a plain dim on any GPU where the framebuffer readback fails.
    char msch[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.gammaos.nano.esde.menucolorscheme", msch, "dark");
    const float dimBright = (!strcmp(msch, "light")) ? 0.60f : 0.80f;
    char mblurP[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.gammaos.nano.esde.menublur", mblurP, "1");
    bool bgBlurred = false;
    if (mblurP[0] != '0' && captureGlass(0.0f, 0.0f, W, H)) {
        // captureGlass leaves the blurred home in mGlassBlurTex already in display sRGB,
        // so no tonemap (tonemapOverride 0). Draw it opaque, tinted down to dimBright, and
        // fade with the open ease. radius 0 -> plain full-screen quad, no rounded SDF.
        drawFrostedGlass(0.0f, 0.0f, W, H, 0.0f, dimBright, dimBright, dimBright,
                         1.0f, a, /*waveSpace=*/false, /*tonemapOverride=*/0.0f);
        bgBlurred = true;
    }
    if (!bgBlurred) {
        // Blur off or readback unavailable: ES-DE still dims, so lay the equivalent black
        // scrim at 1 - dimBright alpha (0.20 for dark, 0.40 for light).
        drawQuad(0, 0, W, H, 0.0f, 0.0f, 0.0f, (1.0f - dimBright) * a);
    }
}

void NanoMenu::renderEsdeMenu() {
    // Ease the open/close animation (0..1). Closing decays to 0 then stops drawing.
    float step = 1.0f / 8.0f;
    if (mEsdeMenuActive) mEsdeMenuAnim = std::min(1.0f, mEsdeMenuAnim + step);
    else                 mEsdeMenuAnim = std::max(0.0f, mEsdeMenuAnim - step);
    if (mEsdeMenuAnim <= 0.001f && !mEsdeMenuActive) { mEsdeMenuClosing = false; return; }
    if (mEsdeMenuAnim < 1.0f) mDisplayDirty = true;
    float a = mEsdeMenuAnim;

    const float W = (float)mWidth, H = (float)mHeight;
    const bool vertical = H > W;
    const float unit = std::min(W, H);
    esdeDrawMenuBackdrop(a);

    const bool dl = (mEsdeMenuPage == ESDE_PG_DOWNLOADER);
    const bool apps = (mEsdeMenuPage == ESDE_PG_APPS);

    // Panel geometry (ES-DE MenuComponent, in screen fractions so it scales to any panel).
    float menuW = std::min(H * 1.05f, W * (vertical ? 0.94f : 0.90f));
    float titleH = unit * 0.11f;
    float rowH = unit * (dl ? 0.075f : 0.062f);        // downloader rows carry a second info line
    int rows = esdeMenuPageRows();
    float footerH = unit * (dl ? 0.10f : 0.06f);       // downloader footer holds a status/progress line
    float maxH = H * (vertical ? 0.70f : 0.80f);
    float listH = std::min((float)rows * rowH, maxH - titleH - footerH);
    int maxFit = rowH > 0 ? (int)(listH / rowH) : rows;
    if (maxFit < 1) maxFit = 1;
    float bodyRows = dl ? (float)maxFit : std::min((float)rows, (float)maxFit);
    float menuH = titleH + bodyRows * rowH + footerH;
    float menuX = (W - menuW) * 0.5f;
    float menuY = H * 0.13f;
    menuY += (1.0f - a) * unit * 0.03f;                // slide up + fade in

    const EsdeMenuCol mc = esdeMenuColors();
    const float* cFrame = mc.frame;
    const float cTitle = mc.title, cPrim = mc.prim, cSec = mc.sec;
    // List rows (downloader, apps, pickers): bright when selected, on a dark or a light frame.
    auto rowShade = [&](bool cur) { return cPrim < 0.45f ? (cur ? 0.10f : cPrim) : (cur ? 0.95f : 0.55f); };
    float rad = unit * 0.028f;   // corner radius as a fraction of the screen
    drawRoundedRect(menuX, menuY, menuW, menuH, rad, cFrame[0], cFrame[1], cFrame[2], a);
    drawRoundedRect(menuX, menuY, menuW, titleH, rad, cFrame[0], cFrame[1], cFrame[2], a);

    // Title.
    std::string title = (mEsdeMenuPage == ESDE_PG_ROOT) ? std::string(trDyn("UI SETTINGS"))
                      : dl ? std::string(trDyn("THEME DOWNLOADER"))
                      : apps ? std::string(trDyn("APPLICATIONS")) : std::string(trDyn(mEsdeMenuPickTitle.c_str()));
    float titleSc = (unit * 0.045f) / (float)FONT_CHAR_H;
    float tw = measureText(title.c_str(), titleSc);
    drawText(title.c_str(), menuX + (menuW - tw) * 0.5f,
             menuY + (titleH - unit * 0.045f) * 0.5f, titleSc, cTitle, cTitle, cTitle, a);

    // Keep the selection inside the visible window.
    if (mEsdeMenuSel < mEsdeMenuScroll) mEsdeMenuScroll = mEsdeMenuSel;
    if (mEsdeMenuSel >= mEsdeMenuScroll + maxFit) mEsdeMenuScroll = mEsdeMenuSel - maxFit + 1;
    if (mEsdeMenuScroll > rows - maxFit) mEsdeMenuScroll = std::max(0, rows - maxFit);
    if (mEsdeMenuScroll < 0) mEsdeMenuScroll = 0;

    float listY = menuY + titleH;
    if (mEsdeMenuActive) {
        // Touch: a tap outside the panel closes it (as B does), a row is chosen as A chooses it, and a
        // drag over the rows moves through them.
        mTouch.block({0.0f, 0.0f, W, H}, [this]() { esdeMenuBack(); mDisplayDirty = true; });
        mTouch.block({menuX, menuY, menuW, menuH});
        const TouchRect list{menuX, listY, menuW, rowH * (float)maxFit};
        mTouch.scroll(list, TouchZones::kUpDown, rowH, [this](int dir) {
            const int next = mEsdeMenuSel + dir;
            if (!mEsdeMenuActive || next < 0 || next >= esdeMenuPageRows()) return false;
            mEsdeMenuSel = next;
            mDisplayDirty = true;
            return true;
        });
        for (int r = 0; r < maxFit && mEsdeMenuScroll + r < rows; r++) {
            const int i = mEsdeMenuScroll + r;
            mTouch.tap({menuX, listY + r * rowH, menuW, rowH}, [this, i]() {
                if (!mEsdeMenuActive || i >= esdeMenuPageRows()) return;
                mEsdeMenuSel = i;
                esdeMenuSelect();
                mDisplayDirty = true;
            });
        }
    }
    float rowSc = (unit * 0.033f) / (float)FONT_CHAR_H;
    float subSc = (unit * 0.024f) / (float)FONT_CHAR_H;
    float glyphH = unit * 0.033f;
    float padX = menuW * 0.05f;

    // Snapshot the download list once (worker may mutate it) for a consistent frame.
    std::vector<EsdeDlEntry> dlSnap;
    if (dl) { std::lock_guard<std::mutex> lk(mEsdeDlMutex); dlSnap = mEsdeDlList; }

    for (int r = 0; r < maxFit && mEsdeMenuScroll + r < rows; r++) {
        int i = mEsdeMenuScroll + r;
        float ry = listY + r * rowH;
        bool cur = (i == mEsdeMenuSel);
        // ES-DE ComponentList selector: the selected row is a solid bar in mMenuColorSelector
        // (black / dark red / white per scheme), drawn behind the row; the text stays its normal
        // colour (ES-DE does not brighten the selected row), so selection reads as a bar, not a tint.
        if (cur) drawQuad(menuX, ry, menuW, rowH, mc.sel[0], mc.sel[1], mc.sel[2], a);

        if (mEsdeMenuPage == ESDE_PG_ROOT) {
            if (i == 0) {                              // THEME DOWNLOADER submenu row
                drawText(trDyn("THEME DOWNLOADER"), menuX + padX, ry + (rowH - glyphH) * 0.5f, rowSc,
                         cPrim, cPrim, cPrim, a);
                drawText(">", menuX + menuW - padX, ry + (rowH - glyphH) * 0.5f, rowSc,
                         cPrim, cPrim, cPrim, a);
                continue;
            }
            if (i >= kEsdeRowPlatform) {               // an action row (opens a submenu or screen)
                std::string lbl = i == kEsdeRowSystems ? trDyn("GAME SYSTEMS")
                                : i == kEsdeRowScraper ? trDyn("BOXART SCRAPER")
                                : i == kEsdeRowApps    ? trDyn("APPLICATIONS")
                                : i == kEsdeRowNanoSettings ? trDyn("NANO SETTINGS") : esdeMenuPlatformLabel();
                drawText(lbl.c_str(), menuX + padX, ry + (rowH - glyphH) * 0.5f, rowSc,
                         cPrim, cPrim, cPrim, a);
                drawText(">", menuX + menuW - padX, ry + (rowH - glyphH) * 0.5f, rowSc,
                         cPrim, cPrim, cPrim, a);
                continue;
            }
            std::string lbl, prop; std::vector<std::pair<std::string, std::string>> opts;
            int ci = esdeMenuOptionCtx(i - 1, lbl, prop, opts);
            lbl = std::string(trDyn(lbl.c_str()));
            std::string val = (ci >= 0 && ci < (int)opts.size()) ? opts[ci].second : std::string();
            float br = opts.empty() ? 0.314f : 1.0f;   // disabled rows dimmed (ES-DE grays these)
            float lc = cPrim * br;
            float labelW = measureText(lbl.c_str(), rowSc);
            drawText(lbl.c_str(), menuX + padX, ry + (rowH - glyphH) * 0.5f, rowSc, lc, lc, lc, a);
            if (!val.empty()) {
                std::string shown = "< " + val + " >";
                float valSc = rowSc;
                float vw = measureText(shown.c_str(), valSc);
                // Keep the value clear of the row label: shrink it to the free span between the
                // label's right edge and the right padding if it would otherwise overlap.
                float avail = menuW - 2.0f * padX - labelW - menuW * 0.03f;
                if (avail > 0.0f && vw > avail) { valSc *= avail / vw; vw = avail; }
                float vGlyphH = glyphH * (valSc / rowSc);
                drawText(shown.c_str(), menuX + menuW - padX - vw, ry + (rowH - vGlyphH) * 0.5f,
                         valSc, cSec, cSec, cSec, a);
            }
        } else if (dl) {                               // downloader theme entry
            if (i >= (int)dlSnap.size()) continue;
            const EsdeDlEntry& e = dlSnap[i];
            float lc = rowShade(cur);
            drawText(e.name.c_str(), menuX + padX, ry + rowH * 0.14f, rowSc, lc, lc, lc, a);
            if (e.installed) {
                const char* tag = trDyn("INSTALLED");
                float vw = measureText(tag, subSc);
                drawText(tag, menuX + menuW - padX - vw, ry + rowH * 0.18f, subSc, 0.5f, 0.75f, 0.5f, a);
            }
            char sub[160];
            snprintf(sub, sizeof(sub), trDyn("%s   %d variants  %d colours  %d aspects"),
                     e.author.c_str(), e.variants, e.colorSchemes, e.aspectRatios);
            drawText(sub, menuX + padX, ry + rowH * 0.58f, subSc, 0.45f, 0.45f, 0.45f, a);
        } else if (apps) {                             // app launcher row
            if (i >= (int)mAppEntries.size()) continue;
            const char* lbl = mAppEntries[i].label.c_str();
            float lc = rowShade(cur);
            drawText(lbl, menuX + padX, ry + (rowH - glyphH) * 0.5f, rowSc, lc, lc, lc, a);
        } else {                                       // option picker row
            const char* lbl = mEsdeMenuPickOpts[i].second.c_str();
            float lc = rowShade(cur);
            drawText(lbl, menuX + padX, ry + (rowH - glyphH) * 0.5f, rowSc, lc, lc, lc, a);
        }
    }

    // ES-DE ComponentList draws a thin separator at every row boundary - the top of each entry plus a
    // closing one at the list bottom - in mMenuColorSeparators (0x303030 dark / 0xC6C7C6 light). These
    // are the subtle lines that divide the menu rows; nano omitted them, so its list read as one flat
    // block. Draw them on top of the rows/selector like ES-DE, a single pixel scaled to the panel.
    {
        int nVis = std::min(maxFit, rows - mEsdeMenuScroll);
        if (nVis < 0) nVis = 0;
        float sepH = std::max(1.0f, unit * 0.0014f);   // one pixel at 720 lines, scaled
        for (int r = 0; r <= nVis; r++)
            drawQuad(menuX, listY + (float)r * rowH, menuW, sepH, mc.sep[0], mc.sep[1], mc.sep[2], a);
    }

    // Empty app list (e.g. packages.list not yet readable): say so rather than show a blank panel.
    if (apps && mAppEntries.empty()) {
        const char* msg = trDyn("No apps installed");
        float mw = measureText(msg, rowSc);
        drawText(msg, menuX + (menuW - mw) * 0.5f, listY + rowH * 0.5f, rowSc, cSec, cSec, cSec, a);
    }

    // Scroll indicators.
    if (rows > maxFit) {
        float ix = menuX + menuW - padX * 0.5f;
        if (mEsdeMenuScroll > 0)
            drawText("^", ix, listY + rowH * 0.1f, rowSc, 0.7f, 0.7f, 0.7f, a);
        if (mEsdeMenuScroll + maxFit < rows)
            drawText("v", ix, listY + (maxFit - 1) * rowH + rowH * 0.1f, rowSc, 0.7f, 0.7f, 0.7f, a);
    }

    float footY = menuY + menuH - footerH;
    // Downloader status / progress line above the help.
    if (dl) {
        std::string status;
        char inst[192];
        if (mEsdeDlInstalling.load())
            snprintf(inst, sizeof(inst), trDyn("Installing %s..."), mEsdeDlInstalledName.c_str());
        if (mEsdeDlFetching.load())        status = trDyn("Fetching theme list...");
        else if (mEsdeDlInstalling.load()) status = inst;
        else if (!mEsdeDlError.empty())    status = mEsdeDlError;
        else if (dlSnap.empty())           status = trDyn("Press A to fetch the theme list");
        if (!status.empty()) {
            float sw = measureText(status.c_str(), subSc);
            bool err = !mEsdeDlError.empty() && !mEsdeDlInstalling.load() && !mEsdeDlFetching.load();
            drawText(status.c_str(), menuX + (menuW - sw) * 0.5f, footY + footerH * 0.12f, subSc,
                     err ? 0.85f : 0.6f, err ? 0.35f : 0.6f, err ? 0.35f : 0.6f, a);
        }
        if (mEsdeDlInstalling.load()) {
            float bx = menuX + padX, bw = menuW - padX * 2, by = footY + footerH * 0.40f, bh = unit * 0.012f;
            drawRoundedRect(bx, by, bw, bh, bh * 0.5f, 0.25f, 0.25f, 0.25f, a);
            float p = mEsdeDlProgress.load() / 100.0f;
            if (p > 0) drawRoundedRect(bx, by, bw * p, bh, bh * 0.5f, 0.4f, 0.7f, 0.9f, a);
        }
    }

    // The menu's help bar is drawn like every ES-DE help bar - button-icon glyphs + labels at the
    // screen bottom, in the theme's helpsystem style - by esdeDrawHelp (which swaps in the menu's
    // prompts, see esdeMenuBuildHelpPrompts, whenever the menu is open), so nothing is drawn inside
    // the panel here (ES-DE shows no separate in-panel legend). The home element pass only cached
    // the view-matched helpsystem element (it draws before the dim/blur above); redraw it here, on
    // top of the panel, so the menu legend is not covered - matching ES-DE's top-GUI HelpComponent.
    if (mEsdeHelpElem)
        esdeDrawHelp(mEsdeHelpElem, mEsdeHelpGamelist, mEsdeHelpPrimary, /*menuOverlayPass=*/true);
}

// Build the ES-DE menu help prompts (icon id, label) for the currently-focused row, in the exact
// left-to-right ORDER ES-DE's HelpComponent renders them (it draws getHelpPrompts in vector order,
// with no priority sort - unlike the view/home bar, which esdeDrawHelp reorders to match the
// theme's per-system help; the menu uses the default HelpComponent). The selector rows are inline
// OptionListComponents, so their bar mirrors ES-DE's UI Settings menu exactly:
// ComponentList::getHelpPrompts = [focused component's prompts] + "up/down choose", then the
// GuiSettings container appends the back/close prompt. OptionListComponent contributes
// "left/right change value" then "a select". Hence a selector row reads
// LEFT/RIGHT CHANGE VALUE - A SELECT - UP/DOWN CHOOSE - B CLOSE MENU, and a plain row drops the
// change-value prompt. Sub-pages are their own GuiSettings-style screens (B goes back, not close).
void NanoMenu::esdeMenuBuildHelpPrompts(std::vector<std::pair<std::string, std::string>>& out) const {
    out.clear();
    if (mEsdeMenuPage == ESDE_PG_ROOT) {
        if (mEsdeMenuSel >= 1 && mEsdeMenuSel < kEsdeRowPlatform) // an inline OptionList selector row
            out.push_back({"left/right", "change value"});
        out.push_back({"a", "select"});
        out.push_back({"up/down", "choose"});
        out.push_back({"b", "close menu"});
        out.push_back({"start", "close menu"});
    } else if (mEsdeMenuPage == ESDE_PG_DOWNLOADER) {
        out.push_back({"a", "install"});
        out.push_back({"up/down", "choose"});
        out.push_back({"b", "back"});
    } else if (mEsdeMenuPage == ESDE_PG_APPS) {
        out.push_back({"a", "launch"});
        if (mEsdeMenuSel >= 0 && mEsdeMenuSel < (int)mAppEntries.size()
            && mAppEntries[mEsdeMenuSel].packageName != "org.thimsy.androidgames")
            out.push_back({"y", "android games"});   // adds or removes; the message says which
        out.push_back({"up/down", "choose"});
        out.push_back({"b", "back"});
    } else {
        out.push_back({"a", "select"});
        out.push_back({"up/down", "choose"});
        out.push_back({"b", "back"});
    }
}

// ------------------------------------------------------------------ downloader

// fork/exec a process, no shell (avoids injection); returns the exit code, -1 on spawn failure.
// netns=true runs it in init's mount namespace (nano's home service lives in the bootstrap ns;
// network/apex-dependent tools need init's default ns, the same trick gammaos-net uses). The
// child's stdout/stderr go to a log file so a failing curl/unzip leaves a diagnosable trail.
static int esdeRunProc(const std::vector<std::string>& argvIn, bool netns = false) {
    std::vector<std::string> argv;
    if (netns) { argv = {"/system/bin/nsenter", "-t", "1", "-m", "--"}; }
    for (const auto& a : argvIn) argv.push_back(a);
    std::vector<char*> cargv;
    for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) { ALOGE("esde-dl: fork failed: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        int fd = open("/data/local/tmp/esde_dl.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); if (fd > 2) close(fd); }
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0)   // only surface failures; a normal download is silent
        ALOGW("esde-dl: '%s' exited rc=%d", argvIn.empty() ? "?" : argvIn[0].c_str(), rc);
    return rc;
}

// Candidate zip-archive URLs for a theme's git repo (nano has no git; GitHub/GitLab serve repo
// zips). master then main since the default branch varies.
static std::vector<std::string> esdeZipUrls(const std::string& git) {
    std::string u = git;
    if (u.size() > 4 && u.compare(u.size() - 4, 4, ".git") == 0) u = u.substr(0, u.size() - 4);
    size_t slash = u.find_last_of('/');
    std::string repo = (slash == std::string::npos) ? u : u.substr(slash + 1);
    std::vector<std::string> out;
    if (u.find("gitlab.com") != std::string::npos) {
        out.push_back(u + "/-/archive/master/" + repo + "-master.zip");
        out.push_back(u + "/-/archive/main/" + repo + "-main.zip");
    } else {                                             // github and github-compatible hosts
        out.push_back(u + "/archive/refs/heads/master.zip");
        out.push_back(u + "/archive/refs/heads/main.zip");
    }
    return out;
}

// The directory inside `base` that holds a theme (has capabilities.xml), directly or one level
// down (repo zips wrap everything in a <repo>-<branch>/ dir). "" if none.
static std::string esdeFindThemeDir(const std::string& base) {
    if (access((base + "/capabilities.xml").c_str(), F_OK) == 0) return base;
    DIR* d = opendir(base.c_str());
    if (!d) return "";
    std::string found;
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string sub = base + "/" + e->d_name;
        if (access((sub + "/capabilities.xml").c_str(), F_OK) == 0) { found = sub; break; }
    }
    closedir(d);
    return found;
}

void NanoMenu::esdeDlStartFetch() {
    if (mEsdeDlFetching.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(mEsdeDlMutex); mEsdeDlError.clear(); }
    if (mEsdeDlThread.joinable()) mEsdeDlThread.join();
    mEsdeDlThread = std::thread([this] {
        char urlbuf[PROPERTY_VALUE_MAX] = {0};
        property_get("persist.gammaos.nano.esde.themes_url", urlbuf,
                     "https://gitlab.com/es-de/themes/themes-list/-/raw/master/themes.json");
        const std::string tmp = "/data/system/nano_esde_themes/.themes.json";
        // Ensure the theme root exists first. On a fresh install nothing has
        // created /data/system/nano_esde_themes yet, so gammaos-net's
        // FileOutputStream(dest.part) failed with ENOENT (rc 5) and the theme
        // list came back empty -- surfacing as "themes list not working" even
        // with Wi-Fi connected. Create the directory before the fetch.
        esdeRunProc({"/system/bin/mkdir", "-p", "/data/system/nano_esde_themes"});
        unlink(tmp.c_str());
        // Download through the framework helper: nano itself has no default network, so it routes
        // the fetch through gammaos-net (an app_process with the framework + a validated network).
        int rc = esdeRunProc({"/system/bin/gammaos-net", "download", urlbuf, tmp});
        // Trust the downloaded file if it parses, so an unreliable exit code under nano's process
        // context does not mask a good fetch.
        std::vector<EsdeDlEntry> list;
        std::string err;
        std::string text;
        if (FILE* f = fopen(tmp.c_str(), "rb")) {
            char b[8192]; size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) text.append(b, n);
            fclose(f);
        }
        njson::Value root;
        if (njson::parse(text, &root) && root.isObject()) {
            const njson::Value* th = root.find("themes");
            if (th && th->isArray()) {
                for (const njson::Value& t : th->arr) {
                    if (!t.isObject()) continue;
                    EsdeDlEntry e;
                    auto s = [&](const char* k) { const njson::Value* v = t.find(k); return v ? v->asString() : std::string(); };
                    auto cnt = [&](const char* k) { const njson::Value* v = t.find(k); return (v && v->isArray()) ? (int)v->arr.size() : 0; };
                    e.name = s("name"); e.reponame = s("reponame"); e.url = s("url"); e.author = s("author");
                    e.variants = cnt("variants"); e.colorSchemes = cnt("colorSchemes");
                    e.aspectRatios = cnt("aspectRatios"); e.fontSizes = cnt("fontSizes");
                    // reponame becomes a directory root runs rm -rf / mv / chown -R on.
                    if (e.name.empty() || !nanolaunch::isValidRepoName(e.reponame)) continue;
                    e.installed = access(("/data/system/nano_esde_themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0
                               || access(("/system/etc/nano_esde_themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0
                               || access((esdeDataDir() + "/themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0;
                    list.push_back(std::move(e));
                }
            }
        }
        if (list.empty()) {
            char eb[192];
            snprintf(eb, sizeof(eb), trDyn("theme list fetch failed (net rc %d, %ld bytes) - check Wi-Fi"),
                     rc, (long)text.size());
            err = eb;
        }
        ALOGI("esde-dl: fetch rc=%d bytes=%zu parsed=%zu", rc, text.size(), list.size());
        {
            std::lock_guard<std::mutex> lk(mEsdeDlMutex);
            mEsdeDlList.swap(list);
            mEsdeDlError = err;
        }
        mEsdeDlFetching.store(false);
        mEsdeDlDone.store(true); FrameFlag::wake();
    });
}

std::string NanoMenu::esdeDlDoInstall(const EsdeDlEntry& e) {
    if (!nanolaunch::isValidRepoName(e.reponame)) return trDyn("download failed");
    const std::string root = "/data/system/nano_esde_themes";
    const std::string staging = root + "/.dl_stage";
    esdeRunProc({"/system/bin/rm", "-rf", staging});
    esdeRunProc({"/system/bin/mkdir", "-p", staging});
    const std::string zip = staging + "/theme.zip";
    mEsdeDlProgress.store(5);
    int rc = -1;
    for (const std::string& url : esdeZipUrls(e.url)) {
        esdeRunProc({"/system/bin/gammaos-net", "download", url, zip});
        struct stat st;
        if (stat(zip.c_str(), &st) == 0 && st.st_size > 1000) { rc = 0; break; }
        rc = -1;
    }
    if (rc != 0) { esdeRunProc({"/system/bin/rm", "-rf", staging}); return trDyn("download failed"); }
    mEsdeDlProgress.store(50);
    if (esdeRunProc({"/system/bin/unzip", "-o", "-q", zip, "-d", staging}) != 0) {
        esdeRunProc({"/system/bin/rm", "-rf", staging});
        return trDyn("unzip failed");
    }
    mEsdeDlProgress.store(80);
    std::string themeDir = esdeFindThemeDir(staging);
    if (themeDir.empty()) { esdeRunProc({"/system/bin/rm", "-rf", staging}); return trDyn("no theme in archive"); }
    const std::string target = root + "/" + e.reponame;
    esdeRunProc({"/system/bin/rm", "-rf", target});
    if (esdeRunProc({"/system/bin/mv", themeDir, target}) != 0) {
        esdeRunProc({"/system/bin/rm", "-rf", staging});
        return trDyn("install move failed");
    }
    esdeRunProc({"/system/bin/rm", "-rf", staging});
    // Match the manual-install convention (system:system + the system_data_file context) so a
    // downloaded set is owned and labelled like the bundled ones rather than left root:root from
    // unzip. nano runs as root so it reads either way, but this keeps ownership consistent and the
    // set readable regardless of the process uid.
    esdeRunProc({"/system/bin/chown", "-R", "system:system", target});
    esdeRunProc({"/system/bin/restorecon", "-R", target});
    mEsdeDlProgress.store(100);
    return "";
}

void NanoMenu::esdeDlStartInstall(const EsdeDlEntry& e) {
    if (mEsdeDlInstalling.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(mEsdeDlMutex); mEsdeDlError.clear(); }
    mEsdeDlInstalledName = e.name;
    mEsdeDlProgress.store(0);
    if (mEsdeDlThread.joinable()) mEsdeDlThread.join();
    EsdeDlEntry ec = e;
    mEsdeDlThread = std::thread([this, ec] {
        std::string err = esdeDlDoInstall(ec);
        { std::lock_guard<std::mutex> lk(mEsdeDlMutex); mEsdeDlError = err; }
        mEsdeDlInstalling.store(false);
        mEsdeDlDone.store(true); FrameFlag::wake();
    });
}

// Per-frame while the menu is open: join a finished worker and refresh installed state.
void NanoMenu::esdeDlTick() {
    if (!mEsdeDlDone.exchange(false)) return;
    if (mEsdeDlThread.joinable()) mEsdeDlThread.join();
    esdeMenuEnumerateInstalled();
    std::lock_guard<std::mutex> lk(mEsdeDlMutex);
    for (auto& e : mEsdeDlList)
        e.installed = access(("/data/system/nano_esde_themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0
                   || access(("/system/etc/nano_esde_themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0
                   || access((esdeDataDir() + "/themes/" + e.reponame + "/capabilities.xml").c_str(), F_OK) == 0;
    mDisplayDirty = true;
}

// ------------------------------------------------------------------ media viewer

// X in a game list: the game's information first, then, as ES-DE's media viewer, its images full
// screen, one at a time, in ES-DE's order. Nano's own scraped box art and fan art stand in where
// ES-DE has no cover or fan art of its own.
void NanoMenu::esdeViewerOpen(int sysIdx, int romIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    const XmbSystem& sys = mXmbSystems[sysIdx];
    if (romIdx < 0 || romIdx >= (int)sys.roms.size()) return;
    const std::string& rom = sys.roms[romIdx];
    mEsdeViewerFiles.clear(); mEsdeViewerTypes.clear();
    static const struct { const char* type; const char* label; } kTypes[] = {
        {"screenshot", "screenshot"}, {"titlescreen", "title screen"}, {"cover", "cover"},
        {"backcover", "back cover"},  {"3dbox", "3D box"},             {"physicalmedia", "physical media"},
        {"fanart", "fan art"},        {"miximage", "miximage"},
    };
    const ScrapeEntry* se = scrapeEntryFor(rom);
    for (const auto& t : kTypes) {
        std::string f = esdeGameMediaPath(rom, t.type);
        if (f.empty() && se && !strcmp(t.type, "cover"))  f = se->box;
        if (f.empty() && se && !strcmp(t.type, "fanart")) f = se->fan;
        if (f.empty() || std::find(mEsdeViewerFiles.begin(), mEsdeViewerFiles.end(), f) != mEsdeViewerFiles.end())
            continue;
        mEsdeViewerFiles.push_back(f);
        mEsdeViewerTypes.push_back(t.label);
    }
    mEsdeViewerManual = esdeGameMediaPath(rom, "manual");
    {
        // The reader opens PDFs; a comic book archive manual is not offered.
        std::string low = mEsdeViewerManual;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".pdf") != 0) mEsdeViewerManual.clear();
    }
    mEsdeViewerVideo = esdeGameMediaPath(rom, "video");
    esdeEnsureGamelistLoaded(sysIdx, kGlWait);
    mEsdeInfoRom = rom;
    mEsdeInfoLoc = esdeRomLocation(rom);
    mEsdeInfoScroll = 0.0f; mEsdeInfoMaxScroll = 0.0f; mEsdeInfoPageBack.clear();
    mEsdeViewerIdx = 0;
    mEsdeViewerTexIdx = -1;
    mEsdeViewerActive = true;
    mDisplayDirty = true;
}

// The game's video in the full-screen video player; leaving it returns to the viewer.
void NanoMenu::esdePlayGameVideo(const std::string& path) {
    esdeBgVideoStop();   // the one hardware decoder
    NanoVideo::Meta m;
    NanoVideo::probe(path, m);
    const ScrapeEntry* se = esdeMetaFor(mEsdeInfoRom);
    if (se && !se->title.empty()) mVidSoloTitle = se->title;
    else {
        size_t sl = mEsdeInfoRom.find_last_of('/'), dot = mEsdeInfoRom.find_last_of('.');
        mVidSoloTitle = mEsdeInfoRom.substr(sl == std::string::npos ? 0 : sl + 1);
        if (dot != std::string::npos && dot > sl + 1) mVidSoloTitle.resize(dot - sl - 1);
    }
    mVidList.clear();
    mVidIdx = 0;
    VidPending p;
    p.file = path;
    p.w = m.width; p.h = m.height;
    p.resumeChoice = 0;
    mVidStepActive = false;
    vidBeginOpen(p);
    ps3Sfx(PS3_SFX_OK);
}

// Up / Down hints for the game's manual and video, when it has them.
void NanoMenu::esdeViewerMediaHelp(std::vector<std::pair<std::string, std::string>>& prompts) const {
    if (!mEsdeViewerManual.empty()) prompts.push_back({"up", "manual"});
    if (!mEsdeViewerVideo.empty()) prompts.push_back({"down", "video"});
}

void NanoMenu::renderEsdeViewer() {
    if (mEsdeViewerActive && !mPdfActive && !mVidActive && mVidEnterT > 0.001f)   // the game's video fading out
        mTouch.block({0.0f, 0.0f, (float)mWidth, (float)mHeight});
    if (!mEsdeViewerActive || mPdfActive || mVidActive || mVidEnterT > 0.001f) return;
    // Touch: a tap closes it (as B does) but on its help prompts; a swipe across browses, as Left/Right do.
    mTouch.block({0.0f, 0.0f, (float)mWidth, (float)mHeight}, [this]() {
        if (mEsdeViewerActive && !mPdfActive && !mVidActive) { esdeOverlayKey(BTN_EAST); mDisplayDirty = true; }
    });
    mTouch.swipe({0.0f, 0.0f, (float)mWidth, (float)mHeight}, [this](int axis, int dir) {
        if (axis == TouchZones::kAcross && mEsdeViewerActive && !mPdfActive && !mVidActive)
            esdeOverlayNav(dir < 0 ? NavDir::Right : NavDir::Left);
    });
    if (mEsdeViewerIdx <= 0 || mEsdeViewerFiles.empty()) { drawEsdeInfoPanel(1.0f, true); return; }
    const float W = (float)mWidth, H = (float)mHeight, unit = std::min(W, H);
    drawQuad(0.0f, 0.0f, W, H, 0.0f, 0.0f, 0.0f, 1.0f);
    // The image fills the screen above the help bar, keeping its shape.
    const float barH = unit * 0.07f;
    const int i = std::max(0, std::min(mEsdeViewerIdx - 1, (int)mEsdeViewerFiles.size() - 1));
    if (mEsdeViewerTexIdx != i) {
        if (mEsdeViewerTex) { glDeleteTextures(1, &mEsdeViewerTex); mEsdeViewerTex = 0; }
        mEsdeViewerAR = 1.0f;
        mEsdeViewerTex = scraperDecodeTex(mEsdeViewerFiles[i], (int)std::max(W, H), &mEsdeViewerAR);
        if (!mEsdeViewerTex) {   // webp and the other formats stb_image does not read
            int iw = 0, ih = 0;
            mEsdeViewerTex = esdeDecodeImageTex(mEsdeViewerFiles[i], &iw, &ih);
            mEsdeViewerAR = ih > 0 ? (float)iw / (float)ih : 1.0f;
        }
        mEsdeViewerTexIdx = i;
    }
    const float boxW = W, boxH = H - barH;
    if (mEsdeViewerTex && mEsdeViewerAR > 0.0f) {
        float w = boxW, h = boxW / mEsdeViewerAR;
        if (h > boxH) { h = boxH; w = boxH * mEsdeViewerAR; }
        drawIconTex(mEsdeViewerTex, (W - w) * 0.5f, (boxH - h) * 0.5f, w, h, 1.0f, 1.0f, 1.0f, 1.0f);
    } else {
        const char* msg = trDyn("THIS IMAGE CAN'T BE SHOWN");
        const float msc = (unit * 0.033f) / (float)FONT_CHAR_H;
        drawText(msg, (W - measureText(msg, msc)) * 0.5f, (boxH - unit * 0.033f) * 0.5f, msc, 0.6f, 0.6f, 0.6f, 1.0f);
    }
    std::vector<std::pair<std::string, std::string>> prompts;
    prompts.push_back({"left/right", "browse"});
    esdeViewerMediaHelp(prompts);
    prompts.push_back({"b", "close"});
    esdeDrawPlainHelp(prompts, 1.0f);
    // Which image this is, at the right of the help bar.
    char pos[96];
    snprintf(pos, sizeof(pos), "%s  %d/%d", trDyn(mEsdeViewerTypes[i].c_str()), i + 1, (int)mEsdeViewerFiles.size());
    const std::string label = nanoUtf8Upper(pos);
    const float fsc = (unit * 0.026f) / (float)FONT_CHAR_H;
    const float lw = measureText(label.c_str(), fsc);
    drawText(label.c_str(), W - unit * 0.02f - lw, H - barH * 0.5f - unit * 0.013f, fsc, 0.6f, 0.6f, 0.6f, 1.0f);
}

// ES-DE's help bar in its default style (bottom left, built-in button icons), for screens that
// have no theme help element of their own to follow.
void NanoMenu::esdeDrawPlainHelp(const std::vector<std::pair<std::string, std::string>>& prompts, float a) {
    const float W = (float)mWidth, H = (float)mHeight, unit = std::min(W, H);
    const float iconH = unit * 0.035f, textH = unit * 0.026f;
    const float sc = textH / (float)FONT_CHAR_H;
    const float y = H - unit * 0.035f - iconH * 0.5f;
    float x = W * 0.012f;
    for (const auto& p : prompts) {
        const float x0 = x;
        std::string path = esdeHelpIconPath(p.first);
        if (!path.empty()) {
            EsdeSvg ic = esdeArtTex(path, (int)iconH, (int)iconH);
            if (ic.tex) {
                drawIconTex(ic.tex, x, y + (iconH - ic.h) * 0.5f, (float)ic.w, (float)ic.h, 0.6f, 0.6f, 0.6f, a);
                x += ic.w + W * 0.00416f;
            }
        }
        const std::string label = nanoUtf8Upper(trDyn(p.second.c_str()));
        drawText(label.c_str(), x, y + (iconH - textH) * 0.5f, sc, 0.6f, 0.6f, 0.6f, a);
        const float lw = measureText(label.c_str(), sc);
        // Tapping a prompt does what its button does; a two-way prompt is split, a half per way.
        const TouchRect r{x0, y, x + lw - x0, iconH};
        const TouchRect lo{r.x, r.y, r.w * 0.5f, r.h}, hi{r.x + r.w * 0.5f, r.y, r.w * 0.5f, r.h};
        auto nav = [this, unit](const TouchRect& z, NavDir d) {
            mTouch.press(z.grownTo(0.0f, unit * 0.09f), [this, d]() { esdeOverlayNav(d); mDisplayDirty = true; });
        };
        if (p.first == "left/right")   { nav(lo, NavDir::Left); nav(hi, NavDir::Right); }
        else if (p.first == "up/down") { nav(lo, NavDir::Up); nav(hi, NavDir::Down); }
        else if (p.first == "up")      nav(r, NavDir::Up);
        else if (p.first == "down")    nav(r, NavDir::Down);
        else if (p.first == "lr")      { touchButtonZone(BTN_TL, lo); touchButtonZone(BTN_TR, hi); }
        else                           touchHelpPrompt(p.first, r);
        x += lw + W * 0.0125f;
    }
}

// ------------------------------------------------------------------ game information

// Game Options > Information in the ES-DE theme: the gamelist's metadata in an ES-DE menu panel
// (the look of ES-DE's metadata editor) instead of the XMB's information page.
// Where a game's file is, as ES-DE's metadata editor shows it: the path in its system folder and
// the system, "subfolder/Game.zip [SNES]". A game outside every system folder shows its full path.
std::string NanoMenu::esdeRomLocation(const std::string& rom) {
    std::string rel, folder;
    if (!esdeRomRef(rom, nullptr, &rel, &folder)) return rom;
    folder = nanoUtf8Upper(folder);
    return rel + " [" + folder + "]";
}

void NanoMenu::esdeInfoOpen(int sysIdx, int romIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    if (romIdx < 0 || romIdx >= (int)mXmbSystems[sysIdx].roms.size()) return;
    esdeEnsureGamelistLoaded(sysIdx, kGlWait);
    mEsdeInfoRom = mXmbSystems[sysIdx].roms[romIdx];
    mEsdeInfoLoc = esdeRomLocation(mEsdeInfoRom);
    mEsdeInfoScroll = 0.0f; mEsdeInfoMaxScroll = 0.0f; mEsdeInfoAnim = 0.0f; mEsdeInfoPageBack.clear();
    mEsdeInfoActive = true;
    ps3Sfx(PS3_SFX_OK);
    mDisplayDirty = true;
}

// A rating as stars out of five when it is a score (ES-DE's 0-1, ScreenScraper's n/20), else
// as written (TheGamesDB gives an age rating).
static bool esdeRatingStars(const std::string& r, int& stars) {
    if (r.empty()) return false;
    char* end = nullptr;
    double v = strtod(r.c_str(), &end);
    if (end == r.c_str()) return false;
    double frac;
    if (*end == '/') {
        double d = strtod(end + 1, nullptr);
        if (d <= 0.0) return false;
        frac = v / d;
    } else if (*end == '\0' && v >= 0.0 && v <= 1.0) {
        frac = v;
    } else {
        return false;
    }
    stars = std::max(0, std::min(5, (int)lround(frac * 5.0)));
    return true;
}

void NanoMenu::renderEsdeInfo() {
    if (mEsdeInfoActive && !mVidActive && mVidEnterT > 0.001f)   // the game's video fading out
        mTouch.block({0.0f, 0.0f, (float)mWidth, (float)mHeight});
    if (!mEsdeInfoActive || mVidActive || mVidEnterT > 0.001f) return;
    // Touch: a tap outside the panel closes it (as B does); a drag over the text scrolls it.
    mTouch.block({0.0f, 0.0f, (float)mWidth, (float)mHeight}, [this]() {
        if (mEsdeInfoActive && !mPdfActive && !mVidActive) { esdeOverlayKey(BTN_EAST); mDisplayDirty = true; }
    });
    mEsdeInfoAnim = std::min(1.0f, mEsdeInfoAnim + 1.0f / 8.0f);
    if (mEsdeInfoAnim < 1.0f) mDisplayDirty = true;
    drawEsdeInfoPanel(mEsdeInfoAnim, false);
}

void NanoMenu::drawEsdeInfoPanel(float a, bool inViewer) {
    const float W = (float)mWidth, H = (float)mHeight, unit = std::min(W, H);
    const bool vertical = H > W;
    esdeDrawMenuBackdrop(a);
    const EsdeMenuCol mc = esdeMenuColors();

    const ScrapeEntry* se = esdeMetaFor(mEsdeInfoRom);
    std::string title;
    if (se && !se->title.empty()) title = se->title;
    else {
        size_t sl = mEsdeInfoRom.find_last_of('/');
        title = sl == std::string::npos ? mEsdeInfoRom : mEsdeInfoRom.substr(sl + 1);
        size_t dot = title.find_last_of('.');
        if (dot != std::string::npos && dot > 0) title = title.substr(0, dot);
    }
    title = nanoUtf8Upper(title);

    // The detail rows ES-DE's metadata editor shows, those the game has.
    std::vector<std::pair<std::string, std::string>> rows;
    int stars = -1;
    if (se) {
        if (esdeRatingStars(se->rating, stars)) rows.push_back({"RATING", std::string()});
        else if (!se->rating.empty())           rows.push_back({"RATING", se->rating});
        if (!se->releaseDate.empty()) rows.push_back({"RELEASE DATE", se->releaseDate});
        if (!se->developer.empty())   rows.push_back({"DEVELOPER", se->developer});
        if (!se->publisher.empty())   rows.push_back({"PUBLISHER", se->publisher});
        if (!se->genre.empty())       rows.push_back({"GENRE", se->genre});
        if (!se->players.empty())     rows.push_back({"PLAYERS", se->players});
        if (se->lastPlayed.size() >= 8 && se->lastPlayed.compare(0, 8, "19700101") != 0)   // ES-DE's "never"
            rows.push_back({"LAST PLAYED", se->lastPlayed.substr(0, 4) + "-" + se->lastPlayed.substr(4, 2) + "-" + se->lastPlayed.substr(6, 2)});
        if (!se->playCount.empty() && se->playCount != "0") rows.push_back({"TIMES PLAYED", se->playCount});
    }
    const std::string desc = se ? se->synopsis : std::string();

    // Panel geometry, as the options menu's.
    const float menuW = std::min(H * 1.05f, W * (vertical ? 0.94f : 0.90f));
    const float locH = mEsdeInfoLoc.empty() ? 0.0f : unit * 0.04f;
    const float titleH = unit * 0.11f + locH, rowH = unit * 0.062f, footerH = unit * 0.04f;
    const float padX = menuW * 0.05f;
    const float rowSc = (unit * 0.033f) / (float)FONT_CHAR_H, glyphH = unit * 0.033f;
    const float descSc = (unit * 0.030f) / (float)FONT_CHAR_H, descLineH = unit * 0.042f;
    std::vector<std::string> lines;
    if (!desc.empty()) lines = wrapTextToWidth(desc, descSc, menuW - 2.0f * padX);
    const float descH = lines.empty() ? 0.0f : unit * 0.02f + lines.size() * descLineH + unit * 0.02f;
    const float contentH = rows.size() * rowH + descH;
    const float maxBodyH = H * (vertical ? 0.70f : 0.80f) - titleH - footerH;
    const float bodyH = std::max(rowH, std::min(contentH, maxBodyH));
    mEsdeInfoMaxScroll = std::max(0.0f, contentH - bodyH);
    mEsdeInfoScroll = std::max(0.0f, std::min(mEsdeInfoScroll, mEsdeInfoMaxScroll));
    mEsdeInfoViewH = bodyH;
    if (bodyH != mEsdeInfoLayoutH || contentH != mEsdeInfoLayoutContent) {   // rotated, re-wrapped
        mEsdeInfoLayoutH = bodyH; mEsdeInfoLayoutContent = contentH;
        mEsdeInfoPageBack.clear();
    }
    mEsdeInfoLines.clear();
    for (size_t i = 0; i < rows.size(); i++) {
        const float t = i * rowH + (rowH - glyphH) * 0.5f;
        mEsdeInfoLines.push_back({t, t + glyphH});
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const float t = rows.size() * rowH + unit * 0.02f + i * descLineH;
        mEsdeInfoLines.push_back({t, t + unit * 0.030f});
    }
    const float menuH = titleH + bodyH + footerH;
    const float menuX = (W - menuW) * 0.5f;
    const float menuY = H * 0.13f + (1.0f - a) * unit * 0.03f;
    const float rad = unit * 0.028f;   // corner radius as a fraction of the screen
    drawRoundedRect(menuX, menuY, menuW, menuH, rad, mc.frame[0], mc.frame[1], mc.frame[2], a);
    if (!inViewer) mTouch.block({menuX, menuY, menuW, menuH});

    const float titleSc = (unit * 0.045f) / (float)FONT_CHAR_H;
    float tsc = titleSc, tw = measureText(title.c_str(), tsc);
    if (tw > menuW - 2.0f * padX) { tsc *= (menuW - 2.0f * padX) / tw; tw = menuW - 2.0f * padX; }
    drawText(title.c_str(), menuX + (menuW - tw) * 0.5f, menuY + (titleH - locH - unit * 0.045f * (tsc / titleSc)) * 0.5f,
             tsc, mc.title, mc.title, mc.title, a);
    if (locH > 0.0f) {
        // Shrunk to fit, down to two thirds; a longer one keeps its end (the file name).
        const float locSc = (unit * 0.028f) / (float)FONT_CHAR_H, maxW = menuW - 2.0f * padX;
        std::string loc = mEsdeInfoLoc;
        float lsc = locSc, lw = measureText(loc.c_str(), lsc);
        if (lw > maxW) {
            lsc = std::max(locSc * 0.67f, locSc * maxW / lw);
            lw = measureText(loc.c_str(), lsc);
            size_t cut = 0;
            while (lw > maxW && cut + 1 < loc.size()) {
                do { cut++; } while (cut < loc.size() && (loc[cut] & 0xC0) == 0x80);   // whole UTF-8 characters
                lw = measureText(("..." + loc.substr(cut)).c_str(), lsc);
            }
            if (cut > 0) loc = "..." + loc.substr(cut);
        }
        drawText(loc.c_str(), menuX + (menuW - lw) * 0.5f, menuY + titleH - locH - unit * 0.005f,
                 lsc, mc.prim, mc.prim, mc.prim, a);
    }

    const float bodyY = menuY + titleH;
    if (mEsdeInfoMaxScroll > 0.0f) {   // a drag over the text scrolls it
        const float pitch = unit * 0.02f;
        mTouch.scroll({menuX, bodyY, menuW, bodyH}, TouchZones::kUpDown, pitch, [this, pitch](int dir) {
            if (!mEsdeInfoActive && !mEsdeViewerActive) return false;
            const float sc = std::max(0.0f, std::min(mEsdeInfoMaxScroll, mEsdeInfoScroll + dir * pitch));
            if (sc == mEsdeInfoScroll) return false;
            mEsdeInfoScroll = sc;
            mEsdeInfoPageBack.clear();
            mDisplayDirty = true;
            return true;
        });
    }
    scissorLogicalRect(menuX, bodyY, menuW, bodyH);
    float y = bodyY - mEsdeInfoScroll;
    const float sepH = std::max(1.0f, unit * 0.0014f);   // one pixel at 720 lines, scaled
    if (rows.empty() && lines.empty()) {
        const char* msg = trDyn("NO INFORMATION FOR THIS GAME");
        float mw = measureText(msg, rowSc);
        drawText(msg, menuX + (menuW - mw) * 0.5f, y + (rowH - glyphH) * 0.5f, rowSc, mc.sec, mc.sec, mc.sec, a);
    }
    for (const auto& r : rows) {
        drawQuad(menuX, y, menuW, sepH, mc.sep[0], mc.sep[1], mc.sep[2], a);
        const float ty = y + (rowH - glyphH) * 0.5f;
        const char* rowLabel = trDyn(r.first.c_str());
        drawText(rowLabel, menuX + padX, ty, rowSc, mc.prim, mc.prim, mc.prim, a);
        const float labelW = measureText(rowLabel, rowSc);
        if (r.first == "RATING" && r.second.empty() && stars >= 0) {
            const char* five = "\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85";   // five stars
            const float sw = measureText(five, rowSc);
            const float sx = menuX + menuW - padX - sw;
            drawText(five, sx, ty, rowSc, mc.sep[0], mc.sep[1], mc.sep[2], a);
            std::string lit;
            for (int k = 0; k < stars; k++) lit += "\xE2\x98\x85";
            if (!lit.empty()) drawText(lit.c_str(), sx, ty, rowSc, mc.sec, mc.sec, mc.sec, a);
        } else {
            float vsc = rowSc, vw = measureText(r.second.c_str(), vsc);
            const float avail = menuW - 2.0f * padX - labelW - menuW * 0.04f;
            if (avail > 0.0f && vw > avail) { vsc *= avail / vw; vw = avail; }
            drawText(r.second.c_str(), menuX + menuW - padX - vw, y + (rowH - glyphH * (vsc / rowSc)) * 0.5f,
                     vsc, mc.sec, mc.sec, mc.sec, a);
        }
        y += rowH;
    }
    if (!lines.empty()) {
        drawQuad(menuX, y, menuW, sepH, mc.sep[0], mc.sep[1], mc.sep[2], a);
        y += unit * 0.02f;
        for (const auto& l : lines) {
            if (y + descLineH >= bodyY && y <= bodyY + bodyH)
                drawText(l.c_str(), menuX + padX, y, descSc, mc.prim, mc.prim, mc.prim, a);
            y += descLineH;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    // More below / above.
    if (mEsdeInfoMaxScroll > 0.0f) {
        const float ix = menuX + menuW - padX * 0.5f;
        if (mEsdeInfoScroll > 0.5f) drawText("^", ix, bodyY + unit * 0.005f, rowSc, 0.7f, 0.7f, 0.7f, a);
        if (mEsdeInfoScroll < mEsdeInfoMaxScroll - 0.5f)
            drawText("v", ix, bodyY + bodyH - glyphH - unit * 0.005f, rowSc, 0.7f, 0.7f, 0.7f, a);
    }

    std::vector<std::pair<std::string, std::string>> prompts;
    if (inViewer && !mEsdeViewerFiles.empty()) prompts.push_back({"left/right", "media"});
    const bool upDownMedia = inViewer && (!mEsdeViewerManual.empty() || !mEsdeViewerVideo.empty());
    if (inViewer) esdeViewerMediaHelp(prompts);
    if (mEsdeInfoMaxScroll > 0.0f) prompts.push_back({upDownMedia ? "lr" : "up/down", "scroll"});
    prompts.push_back({"b", inViewer ? "close" : "back"});
    esdeDrawPlainHelp(prompts, a);
}

// ------------------------------------------------------------------ viewer / information input

void NanoMenu::esdeOverlayCloseAll() {
    pdfClose();
    if (!mEsdeViewerActive && !mEsdeInfoActive) return;
    mEsdeViewerActive = false; mEsdeInfoActive = false;
    mEsdeViewerFiles.clear(); mEsdeViewerTypes.clear();
    if (mEsdeViewerTex) { glDeleteTextures(1, &mEsdeViewerTex); mEsdeViewerTex = 0; }
    mEsdeViewerTexIdx = -1;
    mDisplayDirty = true;
}

bool NanoMenu::esdeOverlayNav(NavDir dir) {
    if (mPdfActive) return pdfNav(dir);
    if (!mEsdeViewerActive && !mEsdeInfoActive) return false;
    if (mVidActive) return false;                 // the game's video player takes the keys
    if (mVidEnterT > 0.001f) return true;         // and nothing reaches the game list while it fades out
    if (mEsdeViewerActive) {
        // A held key does not go on repeating into the manual or the video it opened.
        if (dir == NavDir::Up && !mEsdeViewerManual.empty())  { pdfOpen(mEsdeViewerManual); mNavRepeatHold = true; return true; }
        if (dir == NavDir::Down && !mEsdeViewerVideo.empty()) { esdePlayGameVideo(mEsdeViewerVideo); mNavRepeatHold = true; return true; }
        const int n = (int)mEsdeViewerFiles.size() + 1;   // the information page, then each image
        if (n > 1 && (dir == NavDir::Left || dir == NavDir::Right)) {
            mEsdeViewerIdx = (mEsdeViewerIdx + (dir == NavDir::Right ? 1 : n - 1)) % n;
            esdeSfx(4);
            mDisplayDirty = true;
            return true;
        }
        if (mEsdeViewerIdx != 0) { mDisplayDirty = true; return true; }
        // on the information page up/down scroll it, below
    }
    if (mEsdeInfoActive || mEsdeViewerActive) {
        const float step = std::min((float)mWidth, (float)mHeight) * 0.084f;
        if (dir == NavDir::Up || dir == NavDir::Down) mEsdeInfoPageBack.clear();
        if (dir == NavDir::Up)   mEsdeInfoScroll = std::max(0.0f, mEsdeInfoScroll - step);
        if (dir == NavDir::Down) mEsdeInfoScroll = std::min(mEsdeInfoMaxScroll, mEsdeInfoScroll + step);
        mDisplayDirty = true;
        return true;
    }
    return false;
}

// A page of the information panel: the last line on screen (whole, or cut by the bottom edge) moves
// to the top, a small gap above it so no glyph is clipped. Back returns to where the page turned
// from; past that (after scrolling with Up/Down), the first line moves to the bottom.
float NanoMenu::esdeInfoPageScroll(int dir) {
    const float s = mEsdeInfoScroll, view = mEsdeInfoViewH;
    if (dir < 0 && !mEsdeInfoPageBack.empty()) {
        const float back = mEsdeInfoPageBack.back();
        mEsdeInfoPageBack.pop_back();
        if (back < s) return back;
        mEsdeInfoPageBack.clear();
    }
    const float gap = std::min((float)mWidth, (float)mHeight) * 0.006f;
    float t = s + dir * view;
    if (dir > 0) {
        for (auto it = mEsdeInfoLines.rbegin(); it != mEsdeInfoLines.rend(); ++it)
            if (it->first < s + view - 0.5f) { t = it->first - gap; break; }
        if (t <= s + 0.5f) t = s + view - gap;           // a line taller than the window
        t = std::max(0.0f, std::min(mEsdeInfoMaxScroll, t));
        if (t > s) mEsdeInfoPageBack.push_back(s);
        return t;
    }
    for (const auto& l : mEsdeInfoLines)
        if (l.second > s + 0.5f) { t = l.second + gap - view; break; }
    if (t >= s - 0.5f) t = s - view + gap;
    // Above the first line's bottom there is nothing left to show but the top.
    if (!mEsdeInfoLines.empty() && t < mEsdeInfoLines.front().second) t = 0.0f;
    return std::max(0.0f, std::min(mEsdeInfoMaxScroll, t));
}

bool NanoMenu::esdeOverlayKey(int code) {
    if (mPdfActive) return pdfKey(code);
    if (!mEsdeViewerActive && !mEsdeInfoActive) return false;
    if (mVidActive) return false;                 // the game's video player takes the keys
    if (mVidEnterT > 0.001f) {                    // fading out: Home still leaves, nothing else gets through
        if (code != KEY_F10 && code != BTN_MODE && code != KEY_HOMEPAGE) return true;
        esdeOverlayCloseAll();
        return false;
    }
    switch (code) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
        return false;                                   // navPress -> esdeOverlayNav
    case BTN_TL: case BTN_TR:                           // scroll the information a page
        if (mEsdeInfoActive || mEsdeViewerIdx == 0) mEsdeInfoScroll = esdeInfoPageScroll(code == BTN_TL ? -1 : 1);
        break;
    case BTN_TL2: case BTN_TR2:                         // first / last image, top / bottom
        if (mEsdeViewerActive) mEsdeViewerIdx = code == BTN_TL2 ? 0 : (int)mEsdeViewerFiles.size();
        if (mEsdeInfoActive) { mEsdeInfoScroll = code == BTN_TL2 ? 0.0f : mEsdeInfoMaxScroll; mEsdeInfoPageBack.clear(); }
        break;
    case BTN_EAST: case KEY_BACK: case KEY_ESC: case BTN_SOUTH: case KEY_ENTER:
    case BTN_NORTH: case BTN_WEST: case BTN_START: case BTN_SELECT:
        esdeOverlayCloseAll();
        if (code == BTN_SELECT) mSelectSearchArmed = false;   // its release must not open Game Options
        ps3Sfx(PS3_SFX_BACK);
        break;
    case KEY_F10: case BTN_MODE: case KEY_HOMEPAGE:   // leave, and let the key do its own job
        esdeOverlayCloseAll();
        return false;
    default:
        break;
    }
    mDisplayDirty = true;
    return true;
}

}  // namespace android
