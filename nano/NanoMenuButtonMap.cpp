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

// The button map in Nano (button_map_overlay_plan.md phases 3 and 4): the chords in the menus, the
// raise over a running app (qs_summon=3), the context from what has focus or what is in front, the
// panel drawn by NanoButtonMapArt into one texture over any theme, and its input. The map never
// pauses a game: over an app the overlay layer is up with drop_input, as under the Quick Menu.

#define LOG_TAG "GammaOSNano"

#include "NanoMenu.h"
#include "NanoAbxy.h"

#include <GLES2/gl2.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "NanoButtonMap.h"
#include "NanoButtonMapArt.h"
#include "NanoButtonMapNav.h"
#include "NanoButtonMapReaders.h"
#include "NanoDeviceKeys.h"
#include "NanoHotkeys.h"
#include "NanoI18n.h"
#include "NanoMenuStrings.h"
#include "NanoMenuUtils.h"

namespace android {

namespace {

constexpr const char* kButtonMapProp = "persist.gammaos.hotkeys.buttonmap";

std::string prop(const std::string& k) { return android::base::GetProperty(k, ""); }

std::string firstDir(std::initializer_list<const char*> dirs, const char* probe) {
    for (const char* d : dirs)
        if (access((std::string(d) + "/" + probe).c_str(), R_OK) == 0) return d;
    return *dirs.begin();
}

bmap::GamepadState gamepadState() { return bmap::gamepadStateFrom(prop); }

bmap::DeviceId deviceOfFd(int fd) {
    bmap::DeviceId d;
    struct input_id id {};
    if (fd >= 0 && ioctl(fd, EVIOCGID, &id) == 0) {
        d.vendor = id.vendor;
        d.product = id.product;
    }
    char name[256] = {};
    if (fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) d.name = name;
    return d;
}


const std::vector<bmap::PadSpec>& padSpecs() {
    static const std::vector<bmap::PadSpec> specs =
        bmap::loadPadSpecsFrom(firstDir({"/data/system/nano_xmb/buttonmap", "/system/etc/nano_xmb/buttonmap"}, "pads.json"));
    return specs;
}

bmap::PadModel padFor(const bmap::DeviceId& dev, const bmap::GamepadState& gp) {
    bmap::PadModel m = bmap::padModelFor(dev, padSpecs(), gp);
    // The function button is labelled as the device table names it.
    const auto& f = devkeys::current().function;
    if (m.has(bmap::Phys::Home) && !padSpecs().empty() && m.id == padSpecs().front().id && !f.empty() && !f[0].label.empty())
        m.label[(int)bmap::Phys::Home] = f[0].label;
    return m;
}

const char* trUi(const char* s) { return trDyn(s); }

}  // namespace

struct NanoBmapUi {
    bool open = false;
    bool overApp = false;            // raised over an app for the map alone: closing resumes the app
    bool catLoaded = false;
    bmap::Catalog cat;
    bmap::PadModel pad;              // the pad in hand, as the emulator sees it (the labels)
    bmap::MapResult result;
    bmart::Scene scene;
    bmart::Assets assets;
    bmart::Layout layout;
    bool table = false;              // Y: the table instead of the drawing
    bmap::Nav nav;
    bmap::NavInput input;
    GLuint tex = 0;
    int texW = 0, texH = 0;
    bool dirty = true;
    float drawnScroll = -1.0f;
    bool drawnMoving = false;
    std::vector<uint8_t> rgba;
    // The pads Nano reads, per fd (labels and codes through gammapad as it runs now).
    bmap::GamepadState gp;
    std::map<int, bmap::PadModel> fdPads;
    std::map<std::pair<int, int>, std::pair<int, int>> absRange;
    float stickX = 0, stickY = 0;
    std::set<int> swallowUp;         // keys whose release must not reach the menu after the map closed
};

bool NanoMenu::bmapActive() const { return mBmap && mBmap->open; }
bool NanoMenu::bmapOverAppOnly() const { return mBmap && mBmap->open && mBmap->overApp && mOverlayMode && mOverlayShown; }

namespace {

// The palette of the theme the panel opens over (NanoBiosUi's modal colours).
bmart::Palette themePalette(bool dsi, bool minima) {
    if (dsi) return bmart::paletteDsi();
    if (minima) return bmart::paletteMinima();
    return bmart::paletteXmb();
}

}  // namespace

// Builds the map for ctx and opens it. False (nothing shown) for a context with nothing to map.
static bool bmapBuild(NanoBmapUi* ui, const bmap::Context& ctx, const bmap::DeviceId& dev,
                      const std::vector<nanohotkeys::Profile>& profiles, const std::string& raCfg, int w, int h,
                      const bmart::Palette& pal) {
    if (ctx.kind == bmap::Context::kNone) return false;
    bmap::ReaderEnv env;
    env.dev = dev;
    env.prop = [](const std::string& k) { return prop(k); };
    env.raConfig = raCfg;
    ui->gp = gamepadState();
    ui->pad = padFor(dev, ui->gp);
    ui->result = bmap::build(env, ctx, ui->pad, ui->cat);
    if (ctx.kind == bmap::Context::kBrowser) {
        const bmap::BrowserState bs = bmap::browserState(ui->gp.running, prop("sys.gammaos.bindings.enabled") != "0");
        ui->result.browserGreyed = bs != bmap::BrowserState::kActive;
        ui->result.browserHint = bmap::browserHint(bs);
    } else {
        std::map<std::string, std::string> actions;
        for (const auto& p : profiles)
            if (p.id == ctx.emulator) actions = bmap::profileActions(njson::serialize(p.json, false));
        std::vector<bmap::HotkeyRow> rows = bmap::chordRows(actions, env, ui->pad, devkeys::functionPrompt(), devkeys::current().sharedBack);
        rows.insert(rows.end(), ui->result.hotkeys.begin(), ui->result.hotkeys.end());
        ui->result.hotkeys = rows;
    }
    static const char* const kSource[] = {"config", "defaults", "unknown", "not-mapped"};
    ALOGI("buttonmap: ctx=%s/%s/%s core=%s source=%s rows=%zu pad=%s", ctx.kind == bmap::Context::kBrowser ? "browser" : "game",
          ctx.emulator.c_str(), ctx.systemId.c_str(), ctx.core.c_str(), kSource[(int)ui->result.source & 3],
          ui->result.rows.size(), ui->pad.id.c_str());
    ui->scene = bmart::sceneFor(ui->result, ui->pad);
    ui->scene.pal = pal;
    ui->table = false;
    (void)w; (void)h;
    return true;
}

// The scene as shown now: the view (drawing or table) and the user's language.
static void bmapPrepareScene(NanoBmapUi* ui) {
    bmart::Scene& s = ui->scene;
    const bool hasDrawing = !ui->result.drawing.empty();
    s.table = ui->table || !hasDrawing;
    // sceneFor names the view Y switches to; in the table that is the drawing.
    for (auto& f : s.footer) {
        const size_t sp = f.rfind(' ');
        if (sp != std::string::npos && ui->table && f.compare(sp + 1, std::string::npos, "Table") == 0) f = f.substr(0, sp) + " Drawing";
        else if (sp != std::string::npos && !ui->table && f.compare(sp + 1, std::string::npos, "Drawing") == 0) f = f.substr(0, sp) + " Table";
    }
}

static bmart::Scene bmapTranslated(const bmart::Scene& s) {
    bmart::Scene t = s;
    bmart::translateScene(&t, trUi);
    const char* code = nanoGetLocaleInfo(nanoGetLocale()).code;
    t.rtl = code && (!strcmp(code, "ar") || !strcmp(code, "he"));
    return t;
}

static bool bmapLayout(NanoBmapUi* ui, int w, int h) {
    bmapPrepareScene(ui);
    const bmart::Scene shown = bmapTranslated(ui->scene);
    if (!bmart::layout(shown, ui->assets, w, h, &ui->layout)) return false;
    bmap::NavModel m = ui->layout.nav();
    const float unit = (float)std::min(w, h);
    m.slop = std::max(10.0f, unit / 60.0f);
    m.swipeMin = unit * 0.08f;
    ui->nav.setModel(m);
    ui->nav.setTouchSpace((float)w, (float)h, (float)w, (float)h);
    ui->dirty = true;
    return true;
}

static void bmapLoadAssets(NanoBmapUi* ui) {
    if (ui->catLoaded) return;
    const std::string art = firstDir({"/data/system/nano_xmb/controllers", "/system/etc/nano_xmb/controllers"}, "controllers.json");
    const std::string data = firstDir({"/data/system/nano_xmb/buttonmap", "/system/etc/nano_xmb/buttonmap"}, "retropad.json");
    if (!bmap::loadCatalogFrom(art, data, &ui->cat)) ALOGW("buttonmap: no controller catalogue in %s", art.c_str());
    ui->assets.artDir = art;
    ui->assets.catalog = &ui->cat;
    // Nano's own face chain (openFontFaces): the theme font, PS3 Rodin, then the script fallbacks.
    const std::string themeFont = prop("persist.gammaos.nano.font");
    for (const char* f : {themeFont.c_str(), "/data/system/nano_xmb/fonts/ps3-rodin-regular.ttf",
                          "/system/etc/nano_xmb/fonts/ps3-rodin-regular.ttf", "/system/fonts/Roboto-Regular.ttf"}) {
        if (f[0] && access(f, R_OK) == 0) { ui->assets.fontPath = f; break; }
    }
    for (const char* f : {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSansCJK-Regular.ttc",
                          "/system/fonts/NotoNaskhArabic-Regular.ttf", "/system/fonts/NotoSansThai-Regular.ttf",
                          "/system/fonts/NotoSansHebrew-Regular.ttf"}) {
        if (ui->assets.fontPath != f && access(f, R_OK) == 0) ui->assets.fallbackFonts.push_back(f);
    }
    ui->catLoaded = true;
}

namespace {

std::vector<bmap::ProfileRef> profileRefs(const std::vector<nanohotkeys::Profile>& profiles) {
    std::vector<bmap::ProfileRef> refs;
    for (const auto& p : profiles) refs.push_back({p.id, p.packages});
    return refs;
}

}  // namespace

// RetroArch's history where the config the launches pass keeps it (on the card with its folder).
static std::string raHistoryPath(const std::string& raCfg) {
    std::string text;
    if (raCfg.empty() || !bmap::readAppFile(bmap::ReaderEnv(), raCfg, &text)) return bmap::kContentHistoryPath;
    std::string h = bmap::cfgValue(text, "content_history_path");
    if (h.empty() || h == "default") {
        const std::string pl = bmap::cfgValue(text, "playlist_directory");
        if (pl.empty() || pl == "default") return bmap::kContentHistoryPath;
        h = pl + "/builtin/content_history.lpl";
    }
    return h;
}

// The map for what is in front of the overlay: the foreground app (button_map_overlay_plan.md §2.2).
// raw: a /storage path to the folder root reads it at.
static bmap::ContextInputs overAppInputs(const std::string& fgPkg, const std::string& raCfg,
                                         std::string (*raw)(const std::string&)) {
    bmap::OverAppState s;
    s.fgPkg = fgPkg;
    bmap::launchStateFrom(prop, &s, readPathFile(kLaunchGameFile));
    if (fgPkg == "com.retroarch.aarch64") {
        const std::string history = raHistoryPath(raCfg);
        bmap::readAppFile(bmap::ReaderEnv(), history, &s.historyJson, 8u << 20);
        struct stat st {};
        if (!s.historyJson.empty() && stat(raw(history).c_str(), &st) == 0) s.historyMtime = st.st_mtime;
    }
    return bmap::overAppContextInputs(s);
}

void NanoMenu::bmapOpenOverApp() {
    if (!mBmap) mBmap = std::make_shared<NanoBmapUi>();
    NanoBmapUi* ui = mBmap.get();
    bmapLoadAssets(ui);
    const auto profiles = nanohotkeys::loadProfiles();
    const std::string raCfg = retroarchConfigFile();
    bmap::ContextInputs in = overAppInputs(mOverlayPausedPkg, raCfg, &NanoMenu::rawStoragePath);
    in.browserPkgs = prop("persist.gammaos.gamepad.browser_pkgs");
    in.profiles = profileRefs(profiles);
    in.drasticNano = drasticNanoAvailable() && android::base::GetBoolProperty("persist.gammaos.nano.drastic_nano", false);
    const bmap::Context ctx = bmap::resolveContext(in, ui->cat);
    bmap::DeviceId dev;
    if (!bmap::parseDeviceId(prop("sys.gammaos.nano.bmap_dev"), &dev)) dev = bmap::DeviceId();
    if (!bmapBuild(ui, ctx, dev, profiles, raCfg, mWidth, mHeight, themePalette(mNdsTheme, mMinimaTheme))
        || !bmapLayout(ui, mWidth, mHeight)) {
        ALOGI("buttonmap: nothing to map over %s", mOverlayPausedPkg.c_str());
        ui->open = false;
        return;
    }
    ui->open = true;
    ui->overApp = true;
    ui->fdPads.clear();
    ui->absRange.clear();
    ui->stickX = ui->stickY = 0;
    ui->input = bmap::NavInput(bmap::NavConfig());   // Back and Y come from the pad that sent each key
    mDisplayDirty = true;
}

// What has focus in the menus, in any theme (§2.1).
bmap::MenuFocus NanoMenu::bmapMenuFocus() {
    bmap::MenuFocus f;
    f.modal = mPs3DlgActive || mPs3OptActive || mOskActive || mBiosDlgActive || mGSearchActive || mPs3WizActive
              || mPs3TzActive || mPs3LangActive || mMpActive || mPvActive || mVidActive || mPdfActive || mEsdeInfoActive
              || mEsdeViewerActive || mEsdeMenuActive || mSetupWizardActive || mScrapeProgActive || mGpTestActive
              || mGpCalibActive || mGpCaptureActive || mCpActive || mPs3BrightSlider || mPhotoMultiActive
              || mWifiManageActive || mPspClockOn || mMenuState == MENU_WIFI || mMenuState == MENU_BT
              || mMenuState == MENU_SETTINGS;
    if (f.modal || !mPs3Xmb) return f;
    auto fromSystem = [&](int sysIdx, bmap::MenuFocus::Item item) {
        if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return false;
        const XmbSystem& s = mXmbSystems[sysIdx];
        f.item = item;
        f.systemId = s.romDir;
        f.coreSo = s.coreSo;
        f.launchPkg = s.launchPkg;
        return true;
    };
    auto gameEmu = [&](int sysIdx, const std::string& rom) {
        if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size() || rom.empty()) return;
        XmbSystem s = mXmbSystems[sysIdx];
        if (!applyGameEmulator(rom, s)) return;
        f.gameEmu = true;
        f.gameCoreSo = s.isStandalone() ? std::string() : s.coreSo;
        f.gamePkg = s.launchPkg;
    };
    // The ES-DE home draws its own system and game views while no Nano level is pushed.
    if (mEsdeTheme && mPs3Stack.empty()) {
        if (mEsdeSysSel < 0 || mEsdeSysSel >= (int)mEsdeSysList.size()) return f;
        const int sysIdx = mEsdeSysList[mEsdeSysSel];
        if (!mEsdeInGamelist) { fromSystem(sysIdx, bmap::MenuFocus::kSystem); return f; }
        if (!fromSystem(sysIdx, bmap::MenuFocus::kGame)) return f;
        const auto& roms = mXmbSystems[sysIdx].roms;
        if (mEsdeGameSel < 0 || mEsdeGameSel >= (int)roms.size()) { f.item = bmap::MenuFocus::kNoItem; return f; }
        f.romPath = roms[mEsdeGameSel];
        gameEmu(sysIdx, f.romPath);
        return f;
    }
    if ((mNdsTheme || mMinimaTheme) && mNdsAtRoot && mPs3Stack.empty()) { f.atCategories = true; return f; }
    std::vector<Ps3Item>& items = ps3CurItems();
    const int sel = ps3CurSel();
    if (sel < 0 || sel >= (int)items.size()) return f;
    const Ps3Item& it = items[sel];
    switch (it.kind) {
        case PS3_SYSTEM:
            fromSystem(it.a, bmap::MenuFocus::kSystem);
            break;
        case PS3_ROM:
            if (fromSystem(it.a, bmap::MenuFocus::kGame) && it.b >= 0 && it.b < (int)mXmbSystems[it.a].roms.size()) {
                f.romPath = mXmbSystems[it.a].roms[it.b];
                gameEmu(it.a, f.romPath);
            }
            break;
        case PS3_RECENT:
            if (it.a >= 0 && it.a < (int)mXmbRecent.size()) {
                const XmbRecentEntry& re = mXmbRecent[it.a];
                const int sysIdx = recentSystemIndex(re);
                f.item = bmap::MenuFocus::kRecent;
                f.systemId = sysIdx >= 0 ? mXmbSystems[sysIdx].romDir : re.romDir;
                f.coreSo = re.standalone ? std::string() : re.coreSo;
                f.launchPkg = re.standalone ? re.launchPkg : std::string();
                f.romPath = re.romPath;
            }
            break;
        case PS3_APP:
        case PS3_LAUNCH_PKG:
            f.item = bmap::MenuFocus::kApp;
            f.appPkg = it.payloadStr;
            break;
        default:
            f.item = bmap::MenuFocus::kOther;
            break;
    }
    return f;
}

// A face button from fd as the pad model names its position (nanoabxy::modelCode). Replayed keys are
// Nano's own codes already.
int NanoMenu::padModelCode(int fd, int code) const {
    return mKeyReplaying ? code : nanoabxy::modelCode(fd, code);
}

// The Switch Screen Button (persist.gammaos.hotkeys.screen) by position, as the pad sends it.
bool NanoMenu::screenChordKey(int fd, int code) {
    if (!mDualPanel) return false;
    const std::string setting = android::base::GetProperty("persist.gammaos.hotkeys.screen", "FACE_NORTH");
    if (setting == "NONE") return false;
    const int want = bmap::buttonMapEvdev(setting, padFor(deviceOfFd(fd), gamepadState()));
    return want > 0 && padModelCode(fd, code) == want;
}

bool NanoMenu::bmapChordKey(int fd, int code, int value) {
    if (value != 1) return false;
    const std::string setting = android::base::GetProperty(kButtonMapProp, "FACE_SOUTH");
    if (setting == "NONE") return false;
    const bmap::GamepadState gp = gamepadState();
    const bmap::DeviceId dev = deviceOfFd(fd);
    const bmap::PadModel pad = padFor(dev, gp);
    const int want = bmap::buttonMapEvdev(setting, pad);
    if (want <= 0 || padModelCode(fd, code) != want) return false;
    if (bmapActive()) {
        bmapClose();
        mBmap->swallowUp.insert(code);
        return true;
    }
    const bool overGame = mOverlayMode && mOverlayShown && !mOverlayWallpaper;
    const bool startSelect = !(mAnbernicHeld && anbernicPhysicallyDown());
    if (startSelect && overGame) return false;   // Start + Select + bottom is the game's over a game
    if (!mBmap) mBmap = std::make_shared<NanoBmapUi>();
    NanoBmapUi* ui = mBmap.get();
    bmapLoadAssets(ui);
    const auto profiles = nanohotkeys::loadProfiles();
    const std::string raCfg = retroarchConfigFile();
    bmap::ContextInputs in;
    if (overGame) {
        // The Quick Menu up over a game: the game's map, as the chord over the game gives.
        in = overAppInputs(mOverlayPausedPkg, raCfg, &NanoMenu::rawStoragePath);
    } else {
        in = bmap::menuContextInputs(bmapMenuFocus(),
                                     startSelect ? bmap::Trigger::kStartSelectSouth : bmap::Trigger::kFunctionSouth);
    }
    in.browserPkgs = prop("persist.gammaos.gamepad.browser_pkgs");
    in.profiles = profileRefs(profiles);
    in.drasticNano = drasticNanoAvailable() && android::base::GetBoolProperty("persist.gammaos.nano.drastic_nano", false);
    const bmap::Context ctx = bmap::resolveContext(in, ui->cat);
    // The chord is taken whether or not there is a map: the button never also goes back or confirms.
    ui->swallowUp.insert(code);
    if (!bmapBuild(ui, ctx, dev, profiles, raCfg, mWidth, mHeight, themePalette(mNdsTheme, mMinimaTheme))
        || !bmapLayout(ui, mWidth, mHeight)) {
        ALOGI("buttonmap: nothing to map here");
        return true;
    }
    ui->open = true;
    ui->overApp = false;
    ui->fdPads.clear();
    ui->absRange.clear();
    ui->stickX = ui->stickY = 0;
    ui->input = bmap::NavInput(bmap::NavConfig());
    navRelease(NavDir::None);
    mDisplayDirty = true;
    return true;
}

void NanoMenu::bmapClose(bool resumeApp) {
    if (!bmapActive()) return;
    NanoBmapUi* ui = mBmap.get();
    ui->open = false;
    ui->nav.touchCancel();
    if (ui->tex) { glDeleteTextures(1, &ui->tex); ui->tex = 0; }
    ui->rgba.clear();
    ui->rgba.shrink_to_fit();
    if (mAnbernicHeld) ui->swallowUp.insert(KEY_F10);
    mDisplayDirty = true;
    // Over an app the map was all the overlay showed: the app takes its input back.
    if (resumeApp && ui->overApp && mOverlayMode && mOverlayShown && !mOverlayWallpaper) {
        ui->overApp = false;
        overlayResume();
    }
    ui->overApp = false;
}

namespace {

bool isTouchCode(int type, int code) {
    return (type == EV_KEY && (code == BTN_TOUCH || code == BTN_TOOL_FINGER || code == BTN_TOOL_DOUBLETAP))
           || (type == EV_ABS && code >= ABS_MT_SLOT && code <= ABS_MT_TOOL_Y) || type == EV_SYN;
}

// A device whose ABS_X / ABS_Y is a stick: a touchscreen or touchpad also sends them, as the
// finger's position on its own scale, which read as a stick would scroll on and on.
bool isStickDevice(int fd) {
    constexpr size_t kBits = 8 * sizeof(unsigned long);
    unsigned long keys[(KEY_MAX + kBits) / kBits] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && ((keys[BTN_TOUCH / kBits] >> (BTN_TOUCH % kBits)) & 1UL))
        return false;
    unsigned long props[(INPUT_PROP_CNT + kBits - 1) / kBits] = {};
    if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0) {
        auto has = [&](int b) { return ((props[b / kBits] >> (b % kBits)) & 1UL) != 0; };
        if (has(INPUT_PROP_DIRECT) || has(INPUT_PROP_POINTER)) return false;
    }
    return true;
}

}  // namespace

bool NanoMenu::bmapKey(int fd, int type, int code, int value) {
    if (!mBmap) return false;
    NanoBmapUi* ui = mBmap.get();
    if (!ui->open) {
        if (type == EV_KEY && value == 0 && ui->swallowUp.erase(code)) return true;
        if (type == EV_KEY && value == 2 && ui->swallowUp.count(code)) return true;
        return false;
    }
    if (isTouchCode(type, code)) return false;
    if (type == EV_KEY && value == 0) {
        ui->swallowUp.erase(code);   // released while open: nothing left to drop
        // Start + Select + the bottom button opened it, or Back was held for a chord: their releases
        // end here, so the held state they set must not outlive the map.
        if (code == BTN_START || code == BTN_SELECT) mSsHeld[code == BTN_START ? 0 : 1] = false;
        else if (code == KEY_BACK) mBackHeld = false;
    }
    if (type == EV_KEY && (code == KEY_POWER || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN)) return false;
    if (type == EV_KEY && (code == BTN_LEFT || code == BTN_MIDDLE)) return false;   // the mouse: a tap at the cursor
    if (type == EV_REL) {
        if (code == REL_WHEEL && value != 0) {
            if (value > 0 ? ui->nav.lineUp() : ui->nav.lineDown()) ui->dirty = true;
            mDisplayDirty = true;
            return true;
        }
        return false;   // the pointer moves as usual
    }
    const int64_t now = android::uptimeMillis();
    auto act = [&](bmap::NavAction a) {
        if (a == bmap::NavAction::kNone) return;
        if (a == bmap::NavAction::kClose) {
            bmapClose();
            return;
        }
        if (a == bmap::NavAction::kToggleView) {
            if (!ui->result.drawing.empty()) {
                ui->table = !ui->table;
                bmapLayout(ui, mWidth, mHeight);
                ui->nav.scrollTo(0);
            }
            return;
        }
        if (ui->nav.apply(a)) ui->dirty = true;
    };
    auto it = ui->fdPads.find(fd);
    if (it == ui->fdPads.end()) it = ui->fdPads.emplace(fd, padFor(deviceOfFd(fd), ui->gp)).first;
    const bmap::PadModel& pad = it->second;
    mDisplayDirty = true;
    mLastInputMs = now;
    if (type == EV_KEY) {
        if (value == 2) return true;
        const bool down = value != 0;
        if (code == KEY_F10 || code == BTN_MODE) return true;
        if (down && (code == KEY_BACK || code == KEY_ESC || code == BTN_RIGHT)) {
            bmapClose();
            ui->swallowUp.insert(code);
            return true;
        }
        if (code == KEY_PAGEUP || code == KEY_PAGEDOWN) {
            if (down) act(code == KEY_PAGEUP ? bmap::NavAction::kPageUp : bmap::NavAction::kPageDown);
            return true;
        }
        bmap::Phys p = bmap::Phys::kCount;
        switch (code) {
            case KEY_UP: case BTN_DPAD_UP: p = bmap::Phys::DUp; break;
            case KEY_DOWN: case BTN_DPAD_DOWN: p = bmap::Phys::DDown; break;
            case KEY_LEFT: case BTN_DPAD_LEFT: p = bmap::Phys::DLeft; break;
            case KEY_RIGHT: case BTN_DPAD_RIGHT: p = bmap::Phys::DRight; break;
            default: {
                // Nano's code, not the position: Close and Table follow Nano's Back and Y (navConfigFor).
                const std::vector<bmap::Phys> ps = pad.physFor(bmap::Input::evKey(code));
                if (!ps.empty()) p = ps[0];
            }
        }
        // The chord again closes, as B and Start do.
        if (down && mAnbernicHeld && anbernicPhysicallyDown()) {
            const std::string setting = android::base::GetProperty(kButtonMapProp, "FACE_SOUTH");
            if (padModelCode(fd, code) == bmap::buttonMapEvdev(setting, pad)) {
                bmapClose();
                ui->swallowUp.insert(code);
                return true;
            }
        }
        if (p == bmap::Phys::kCount) return true;
        // Back and Start are the pad's own (navConfigFor reads them off the pad that sent this).
        bmap::NavInput probe(bmap::navConfigFor(pad));
        const bmap::NavAction a = down ? probe.button(p, true, now) : bmap::NavAction::kNone;
        if (a == bmap::NavAction::kClose || a == bmap::NavAction::kToggleView) {
            act(a);
            if (a == bmap::NavAction::kClose) ui->swallowUp.insert(code);
            return true;
        }
        act(ui->input.button(p, down, now));
        return true;
    }
    if (type == EV_ABS) {
        if (code == ABS_HAT0X || code == ABS_HAT0Y) {
            const bool x = code == ABS_HAT0X;
            const bmap::Phys neg = x ? bmap::Phys::DLeft : bmap::Phys::DUp, pos = x ? bmap::Phys::DRight : bmap::Phys::DDown;
            if (value == 0) {
                ui->input.button(neg, false, now);
                ui->input.button(pos, false, now);
            } else {
                act(ui->input.button(value < 0 ? neg : pos, true, now));
            }
            return true;
        }
        if (code == ABS_X || code == ABS_Y) {
            auto key = std::make_pair(fd, code);
            auto r = ui->absRange.find(key);
            if (r == ui->absRange.end()) {
                struct input_absinfo a {};
                std::pair<int, int> range(-32768, 32767);
                if (!isStickDevice(fd)) range = {0, 0};
                else if (ioctl(fd, EVIOCGABS(code), &a) == 0 && a.maximum > a.minimum) range = {a.minimum, a.maximum};
                r = ui->absRange.emplace(key, range).first;
            }
            if (r->second.second <= r->second.first) return true;   // a touchscreen's or touchpad's pointer
            const float t = (float)(value - r->second.first) / (float)(r->second.second - r->second.first);
            const float v = std::max(-1.0f, std::min(1.0f, t * 2.0f - 1.0f));
            (code == ABS_X ? ui->stickX : ui->stickY) = v;
            act(ui->input.stick(ui->stickX, ui->stickY, now));
            return true;
        }
        return true;
    }
    return true;
}

void NanoMenu::bmapTouchFrame() {
    if (!bmapActive()) return;
    NanoBmapUi* ui = mBmap.get();
    if (mTouchMulti) { ui->nav.touchCancel(); mTouchWasDown = mTouchDown; return; }
    float px, py;
    if (!touchMapRaw(mTouchRawX, mTouchRawY, px, py)) { mTouchWasDown = mTouchDown; return; }
    const bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;
    mTouchWasDown = mTouchDown;
    const int64_t now = android::uptimeMillis();
    ui->nav.pointers(1);
    if (downEdge) {
        ui->nav.touchDown(px, py, now);
    } else if (down) {
        ui->nav.touchMove(px, py, now);
        ui->dirty = true;
    } else if (upEdge) {
        ui->nav.touchMove(px, py, now);
        const bmap::NavAction a = ui->nav.touchUp(now);
        if (a == bmap::NavAction::kClose) {
            bmapClose();
            return;
        }
        ui->dirty = true;
    } else {
        return;
    }
    mLastInputMs = now;
    mDisplayDirty = true;
}

void NanoMenu::bmapTick() {
    if (!bmapActive()) return;
    NanoBmapUi* ui = mBmap.get();
    const int64_t now = android::uptimeMillis();
    const bmap::NavAction a = ui->input.tick(now);
    if (a != bmap::NavAction::kNone && ui->nav.apply(a)) ui->dirty = true;
    if (ui->nav.tick(now)) ui->dirty = true;
    // A held direction repeats and a fling coasts with no input: keep the frames coming.
    mDisplayDirty = true;
    if (ui->nav.flinging() || ui->nav.dragging() || a != bmap::NavAction::kNone) mLastInputMs = now;
}

void NanoMenu::renderButtonMap() {
    if (!bmapActive()) return;
    NanoBmapUi* ui = mBmap.get();
    bmapTick();
    if (!ui->open) return;
    if (ui->layout.w != mWidth || ui->layout.h != mHeight) bmapLayout(ui, mWidth, mHeight);   // rotated, resized
    const bool moving = ui->nav.flinging() || ui->nav.dragging();
    const float scroll = ui->nav.scroll();
    if (ui->dirty || !ui->tex || fabsf(scroll - ui->drawnScroll) > 0.25f || moving != ui->drawnMoving) {
        const bmart::Scene shown = bmapTranslated(ui->scene);
        if (bmart::render(shown, ui->assets, ui->layout, scroll, &ui->rgba, moving)) {
            if (!ui->tex) {
                glGenTextures(1, &ui->tex);
                glBindTexture(GL_TEXTURE_2D, ui->tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                ui->texW = ui->texH = 0;
            }
            glBindTexture(GL_TEXTURE_2D, ui->tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (ui->texW != ui->layout.w || ui->texH != ui->layout.h) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ui->layout.w, ui->layout.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui->rgba.data());
                ui->texW = ui->layout.w;
                ui->texH = ui->layout.h;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ui->layout.w, ui->layout.h, GL_RGBA, GL_UNSIGNED_BYTE, ui->rgba.data());
            }
        }
        ui->dirty = false;
        ui->drawnScroll = scroll;
        ui->drawnMoving = moving;
    }
    if (ui->tex) drawIconTex(ui->tex, 0.0f, 0.0f, (float)mWidth, (float)mHeight, 1.0f, 1.0f, 1.0f, 1.0f);
}

// Button Map Button may not sit on a universal hotkey's button (§1.2): a hotkey already on it keeps
// it, and the map takes the first free face button. A persist write waits for init: not here.
void NanoMenu::bmapMigrateSetting() {
    const std::string cur = android::base::GetProperty(kButtonMapProp, "FACE_SOUTH");
    std::vector<std::string> hotkeys;
    static const std::pair<const char*, const char*> kHot[] = {
        {"save", "BUTTON_R1"}, {"load", "BUTTON_L1"}, {"fastforward", "BUTTON_R2"}, {"rewind", "BUTTON_L2"}, {"quit", "NONE"}};
    for (const auto& h : kHot) hotkeys.push_back(android::base::GetProperty(std::string("persist.gammaos.hotkeys.") + h.first, h.second));
    const bmap::PadModel pad = padFor(bmap::DeviceId(), gamepadState());
    std::string log;
    const std::string next = bmap::migrateButtonMap(cur, hotkeys, pad, &log);
    if (next != cur) {
        ALOGI("NanoMenu: %s", log.c_str());
        std::thread([next] { property_set(kButtonMapProp, next.c_str()); }).detach();
    }
    // The Switch Screen Button the same way, off the hotkeys' and the Button Map Button's.
    static const char* const kScreenProp = "persist.gammaos.hotkeys.screen";
    const std::string screen = android::base::GetProperty(kScreenProp, "FACE_NORTH");
    hotkeys.push_back(next);
    const std::string screenNext = bmap::migrateButtonMap(screen, hotkeys, pad, &log);
    if (screenNext == screen) return;
    ALOGI("NanoMenu: Switch Screen Button: %s", log.c_str());
    std::thread([screenNext] { property_set(kScreenProp, screenNext.c_str()); }).detach();
}

// The Button Map row among the hotkey rows, for hotkeyTakeButton: the built-in pad as it is now.
std::vector<std::pair<int, std::string>> nanoBmapTakeButton(const std::vector<bmap::ButtonRow>& rows, int row,
                                                            const std::string& value);
std::vector<std::pair<int, std::string>> nanoBmapTakeButton(const std::vector<bmap::ButtonRow>& rows, int row,
                                                            const std::string& value) {
    return bmap::takeButton(rows, row, value, padFor(bmap::DeviceId(), gamepadState()));
}

}  // namespace android
