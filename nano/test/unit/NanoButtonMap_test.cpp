// The button map's core (button_map_overlay_plan.md §9.2): the pad model and ABXY Swap (T3), the
// context (T4), the trigger rules, the art catalogue and retropad.json, the browser list's drift
// guard against gammapad's BrowserMode (T7) and the Button Map Button setting (T8).

#include <gtest/gtest.h>

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "NanoButtonMap.h"
#include "NanoDeviceKeys.h"
#include "NanoJson.h"
#include "NanoSystemsHeal.h"
#include "nano_test_fs.h"
#include "nano_test_util.h"

using namespace android;
using namespace android::bmap;

namespace bmaptest {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The Nano folder as installed (<bin>/ps3xmb/...) or in the source tree.
std::string nanoDir() { return nanotest::fixtureDir() + "/../.."; }

const Catalog& catalog() {
    static Catalog c = [] {
        Catalog cat;
        EXPECT_TRUE(loadCatalogFrom(nanoDir() + "/ps3xmb/controllers", nanoDir() + "/ps3xmb/buttonmap", &cat));
        return cat;
    }();
    return c;
}

PadSpec cubeSpec() {
    PadSpec s;
    EXPECT_TRUE(padSpecFromJson(slurp(nanotest::fixture("buttonmap/pads/rg_cube.json")), &s));
    return s;
}

PadModel cube(bool gammapad = false, bool swap = false, const std::string& remap = std::string()) {
    GamepadState gp;
    gp.running = gammapad;
    gp.swap = swap;
    gp.remap = remap;
    return padModel(cubeSpec(), gp);
}

Phys firstPhys(const PadModel& m, const Input& in) {
    const auto p = m.physFor(in);
    return p.empty() ? Phys::kCount : p[0];
}

}  // namespace bmaptest

using namespace bmaptest;

// ---- T3: the pad model ----

TEST(ButtonMapPad, SpecParsesTheCubesTable) {
    const PadSpec s = cubeSpec();
    EXPECT_EQ(0x484b, s.vendor);
    EXPECT_EQ(0x1101, s.product);
    EXPECT_EQ(13u, s.buttons.size());
    EXPECT_EQ(8u, s.axes.size());
    PadSpec bad;
    EXPECT_FALSE(padSpecFromJson("{", &bad));
    EXPECT_FALSE(padSpecFromJson("{\"buttons\": []}", &bad));
    EXPECT_FALSE(padSpecFromJson("[1,2]", &bad));
}

// §1.1's table, for the built-in pad in each gammapad state. gammapad gives apps the face buttons by
// position, as the pad's own layout does with it off, and ABXY Swap swaps them; Nano reads them by
// print in every state (NanoAbxy.h).
TEST(ButtonMapPad, FaceButtonsThroughGammapadAndSwap) {
    struct Want { Phys p; int off, offEv, on, onEv, swap, swapEv; };
    const Want kWant[] = {
        {Phys::South, 96, 305, 96, 305, 97, 305},
        {Phys::East, 97, 304, 97, 304, 96, 304},
        {Phys::North, 100, 307, 100, 307, 99, 307},
        {Phys::West, 99, 308, 99, 308, 100, 308},
    };
    const PadModel off = cube(false), on = cube(true), sw = cube(true, true);
    for (const auto& w : kWant) {
        SCOPED_TRACE(physId(w.p));
        EXPECT_EQ(w.off, off.input(w.p, Input::kKey).code);
        EXPECT_EQ(w.offEv, off.input(w.p, Input::kEvKey).code);
        EXPECT_EQ(w.on, on.input(w.p, Input::kKey).code);
        EXPECT_EQ(w.onEv, on.input(w.p, Input::kEvKey).code);
        EXPECT_EQ(w.swap, sw.input(w.p, Input::kKey).code);
        EXPECT_EQ(w.swapEv, sw.input(w.p, Input::kEvKey).code);
    }
    // Labels are always the print, whatever the codes.
    for (const PadModel* m : {&off, &on, &sw}) {
        EXPECT_EQ("B", m->labelOf(Phys::South));
        EXPECT_EQ("A", m->labelOf(Phys::East));
        EXPECT_EQ("X", m->labelOf(Phys::North));
        EXPECT_EQ("Y", m->labelOf(Phys::West));
    }
}

TEST(ButtonMapPad, SwapDoesNothingWhileGammapadIsStopped) {
    const PadModel a = cube(false, false), b = cube(false, true);
    for (int i = 0; i < kPhysCount; i++) {
        EXPECT_EQ(a.input((Phys)i, Input::kKey), b.input((Phys)i, Input::kKey)) << physId((Phys)i);
        EXPECT_EQ(a.input((Phys)i, Input::kEvKey), b.input((Phys)i, Input::kEvKey)) << physId((Phys)i);
    }
}

TEST(ButtonMapPad, RemapBtnAppliesAfterTheSwap) {
    // With the swap on, the bottom button stays BTN_B; remap_btn then sends it to BTN_A, which Nano
    // reads as it is.
    const PadModel m = cube(true, true, "304:305,305:304");
    EXPECT_EQ(304, southEvdev(m));
    EXPECT_EQ(96, southKeycode(m));
    EXPECT_EQ(Phys::South, firstPhys(m, Input::key(96)));
    const PadModel junk = cube(true, false, "x:y,,:,999999:1,304:");
    EXPECT_EQ(305, southEvdev(junk));
}

TEST(ButtonMapPad, AxesHatAndTriggers) {
    for (const PadModel& m : {cube(false), cube(true)}) {
        EXPECT_EQ(Phys::DUp, firstPhys(m, Input::axis(aaxis::kHatY, -1)));
        EXPECT_EQ(Phys::DRight, firstPhys(m, Input::axis(aaxis::kHatX, 1)));
        EXPECT_EQ(Phys::DLeft, firstPhys(m, Input::key(akey::kDpadLeft)));   // Android's hat keys
        EXPECT_EQ(Phys::LUp, firstPhys(m, Input::axis(aaxis::kY, -1)));
        EXPECT_EQ(Phys::RRight, firstPhys(m, Input::axis(aaxis::kZ, 1)));
        EXPECT_EQ(Phys::RDown, firstPhys(m, Input::axis(aaxis::kRZ, 1)));
        EXPECT_EQ(Phys::L2, firstPhys(m, Input::axis(aaxis::kLTrigger, 1)));
        EXPECT_EQ(Phys::R2, firstPhys(m, Input::axis(aaxis::kRTrigger, 1)));
        EXPECT_TRUE(m.physFor(Input::axis(aaxis::kLTrigger, -1)).empty());   // a trigger has one half
    }
    // gammapad's virtual pad: the right stick on ABS_Z/ABS_RZ, the triggers on ABS_BRAKE/ABS_GAS.
    const PadModel on = cube(true);
    EXPECT_EQ(Phys::RLeft, firstPhys(on, Input::evAbs(2, -1)));
    EXPECT_EQ(Phys::L2, firstPhys(on, Input::evAbs(10, 1)));
    EXPECT_EQ(Phys::R2, firstPhys(on, Input::evAbs(9, 1)));
    const PadModel off = cube(false);
    EXPECT_EQ(Phys::RLeft, firstPhys(off, Input::evAbs(3, -1)));
    EXPECT_EQ(Phys::L2, firstPhys(off, Input::evAbs(2, 1)));
}

TEST(ButtonMapPad, ExternalPadsKeepTheirOwnPrint) {
    struct Case { int vendor; const char* south; const char* east; const char* north; const char* west; const char* drawing; };
    const Case kCases[] = {
        {0x045e, "A", "B", "Y", "X", "xbox"},
        {0x054c, "Cross", "Circle", "Triangle", "Square", "dualshock"},
        {0x057e, "B", "A", "X", "Y", "switch"},
        {0x1234, "Bottom", "Right", "Top", "Left", ""},
    };
    for (const auto& c : kCases) {
        DeviceId d;
        d.vendor = c.vendor;
        const PadModel m = padModel(standardPadSpec(d), GamepadState());
        SCOPED_TRACE(c.vendor);
        EXPECT_EQ(c.south, m.labelOf(Phys::South));
        EXPECT_EQ(c.east, m.labelOf(Phys::East));
        EXPECT_EQ(c.north, m.labelOf(Phys::North));
        EXPECT_EQ(c.west, m.labelOf(Phys::West));
        EXPECT_EQ(c.drawing, m.drawing);
        // The Linux convention: BTN_SOUTH is the bottom button, BUTTON_A in Android's layouts.
        EXPECT_EQ(304, southEvdev(m));
        EXPECT_EQ(96, southKeycode(m));
    }
}

TEST(ButtonMapPad, SouthResolution) {
    EXPECT_EQ(96, southKeycode(cube(false)));
    EXPECT_EQ(305, southEvdev(cube(false)));
    EXPECT_EQ(96, southKeycode(cube(true)));
    EXPECT_EQ(305, southEvdev(cube(true)));
    EXPECT_EQ(97, southKeycode(cube(true, true)));
    EXPECT_EQ(305, southEvdev(cube(true, true)));   // Nano swaps the swap back
    PadModel none;
    EXPECT_EQ(0, southKeycode(none));
    EXPECT_EQ(0, southEvdev(none));
}

TEST(ButtonMapPad, DeviceIdFromTheProperty) {
    DeviceId d;
    ASSERT_TRUE(parseDeviceId("484b:1101:abc123:retrogame_joypad", &d));
    EXPECT_EQ(0x484b, d.vendor);
    EXPECT_EQ(0x1101, d.product);
    EXPECT_EQ("abc123", d.descriptor);
    EXPECT_EQ("retrogame_joypad", d.name);
    ASSERT_TRUE(parseDeviceId("045e:0b13:d:Xbox: Wireless", &d));
    EXPECT_EQ("Xbox: Wireless", d.name);
    EXPECT_FALSE(parseDeviceId("", &d));
    EXPECT_FALSE(parseDeviceId("zz:11:x:y", &d));
    EXPECT_FALSE(parseDeviceId("12345:1:x:y", &d));
    EXPECT_FALSE(parseDeviceId("484b", &d));
}

TEST(ButtonMapPad, Labels) {
    const PadModel m = cube();
    EXPECT_EQ("D-pad", physLabel({Phys::DUp, Phys::DDown, Phys::DLeft, Phys::DRight}, m));
    EXPECT_EQ("D-pad / Left stick", physLabel({Phys::DUp, Phys::LUp, Phys::DDown, Phys::LDown, Phys::DLeft, Phys::LLeft,
                                               Phys::DRight, Phys::LRight}, m));
    EXPECT_EQ("B / L1", physLabel({Phys::L1, Phys::South}, m));
    EXPECT_EQ("D-pad Up", physLabel({Phys::DUp}, m));
    EXPECT_EQ("\xE2\x80\x94", physLabel({}, m));
    EXPECT_EQ("Anbernic", m.labelOf(Phys::Home));
}

// ---- T8: the setting ----

TEST(ButtonMapSetting, FaceButtonsCollideWithKeycodesOnTheSamePhysicalButton) {
    const PadModel off = cube(false), on = cube(true), sw = cube(true, true);
    EXPECT_TRUE(sameButton("FACE_SOUTH", "BUTTON_A", off));   // gammapad off: BUTTON_A is the bottom button
    EXPECT_FALSE(sameButton("FACE_SOUTH", "BUTTON_B", off));
    EXPECT_TRUE(sameButton("FACE_SOUTH", "BUTTON_A", on));    // gammapad on: the same
    EXPECT_TRUE(sameButton("FACE_SOUTH", "BUTTON_B", sw));    // with ABXY Swap it sends BUTTON_B
    EXPECT_TRUE(sameButton("BUTTON_R1", "BUTTON_R1", off));
    EXPECT_FALSE(sameButton("NONE", "NONE", off));
    EXPECT_FALSE(sameButton("FACE_SOUTH", "", off));
    EXPECT_EQ(Phys::East, settingButton("FACE_EAST", off));
    EXPECT_EQ(Phys::kCount, settingButton("BUTTON_Z", off));
}

TEST(ButtonMapSetting, MigrationMovesOffAHotkeysButton) {
    const PadModel m = cube(false);
    std::string log;
    EXPECT_EQ("FACE_SOUTH", migrateButtonMap("FACE_SOUTH", {"BUTTON_R1", "BUTTON_L1", "BUTTON_R2", "BUTTON_L2", "NONE"}, m, &log));
    EXPECT_EQ("", log);
    EXPECT_EQ("FACE_EAST", migrateButtonMap("FACE_SOUTH", {"BUTTON_A", "BUTTON_L1"}, m, &log));
    EXPECT_NE(std::string::npos, log.find("BUTTON_A"));
    EXPECT_EQ("FACE_NORTH", migrateButtonMap("FACE_SOUTH", {"BUTTON_A", "BUTTON_B"}, m, &log));
    EXPECT_EQ("NONE", migrateButtonMap("FACE_SOUTH", {"BUTTON_A", "BUTTON_B", "BUTTON_X", "BUTTON_Y"}, m, &log));
    EXPECT_EQ("NONE", migrateButtonMap("NONE", {"BUTTON_A"}, m, &log));
}

// ---- the decided trigger rules ----

TEST(ButtonMapTrigger, StartSelectOnlyInNanosMenus) {
    EXPECT_TRUE(triggerOpens(Trigger::kFunctionSouth, Where::kNanoMenus));
    EXPECT_TRUE(triggerOpens(Trigger::kFunctionSouth, Where::kOverGame));
    EXPECT_TRUE(triggerOpens(Trigger::kStartSelectSouth, Where::kNanoMenus));
    EXPECT_FALSE(triggerOpens(Trigger::kStartSelectSouth, Where::kOverGame));
    static_assert(!kPausesGame, "the map blocks input like the Quick Menu; the game keeps running");
    static_assert(kBlocksGameInput, "the game gets no input while the map is up");
    static_assert(!kTurnsOnGammapad, "gammapad is never turned on for the browser list");
}

TEST(ButtonMapTrigger, BrowserStateAndHint) {
    EXPECT_EQ(BrowserState::kNeedsControllerEnable, browserState(false, true));
    EXPECT_EQ(BrowserState::kNeedsControllerEnable, browserState(false, false));
    EXPECT_EQ(BrowserState::kBindingsOff, browserState(true, false));
    EXPECT_EQ(BrowserState::kActive, browserState(true, true));
    EXPECT_STREQ("", browserHint(BrowserState::kActive));
    EXPECT_NE(std::string::npos, std::string(browserHint(BrowserState::kNeedsControllerEnable)).find("Controller Enable"));
}

// ---- T4: the context ----

namespace {

std::vector<ProfileRef> profiles() {
    return {
        {"retroarch", {"com.retroarch.aarch64", "com.retroarch", "com.retroarch.ra32"}},
        {"ppsspp", {"org.ppsspp.ppsspp", "org.ppsspp.ppssppgold"}},
        {"dolphin", {"org.dolphinemu.dolphinemu", "org.dolphinemu.mmjr"}},
        {"drastic", {"com.dsemu.drastic", "com.gammaos.drasticsf"}},
        {"watermelonds", {"me.magnum.melondualds"}},
        {"fallback", {"*"}},
    };
}

ContextInputs menus(ContextInputs::Focus f, const std::string& sys) {
    ContextInputs in;
    in.where = Where::kNanoMenus;
    in.focus = f;
    in.systemId = sys;
    in.profiles = profiles();
    return in;
}

ContextInputs over(const std::string& pkg) {
    ContextInputs in;
    in.where = Where::kOverGame;
    in.fgPkg = pkg;
    in.profiles = profiles();
    return in;
}

}  // namespace

TEST(ButtonMapContext, NanoMenus) {
    ContextInputs in = menus(ContextInputs::kFocusSystem, "snes");
    in.coreSo = "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so";
    Context c = resolveContext(in, catalog());
    EXPECT_EQ(Context::kGame, c.kind);
    EXPECT_EQ("retroarch", c.emulator);
    EXPECT_EQ("snes9x", c.core);
    EXPECT_EQ("snes", c.systemId);
    EXPECT_EQ("", c.romPath);   // a system row has no game

    in = menus(ContextInputs::kFocusGame, "snes");   // a per-game emulator choice
    in.launchPkg = "me.magnum.melondualds";
    in.romPath = "/storage/emulated/0/ROMs/snes/a.sfc";
    c = resolveContext(in, catalog());
    EXPECT_EQ("watermelonds", c.emulator);
    EXPECT_EQ("/storage/emulated/0/ROMs/snes/a.sfc", c.romPath);

    in = menus(ContextInputs::kFocusRecent, "psp");
    in.launchPkg = "org.ppsspp.ppsspp";
    c = resolveContext(in, catalog());
    EXPECT_EQ("ppsspp", c.emulator);
    EXPECT_EQ("psp", c.systemId);

    in = menus(ContextInputs::kFocusGame, "nds");
    in.launchPkg = "com.dsemu.drastic";
    in.drasticNano = true;
    EXPECT_EQ("drastic-nano", resolveContext(in, catalog()).emulator);
    in.drasticNano = false;
    EXPECT_EQ("drastic", resolveContext(in, catalog()).emulator);
}

TEST(ButtonMapContext, NothingForAndroidGamesAppsAndSettings) {
    ContextInputs in = menus(ContextInputs::kFocusSystem, "androidgames");
    in.launchPkg = "org.thimsy.androidgames";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    in.focus = ContextInputs::kFocusGame;
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    EXPECT_EQ(Context::kNone, resolveContext(menus(ContextInputs::kFocusOther, ""), catalog()).kind);
    in = menus(ContextInputs::kFocusAppRow, "");
    in.appPkg = "com.android.settings";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    in.appPkg = "com.brave.browser";
    const Context c = resolveContext(in, catalog());
    EXPECT_EQ(Context::kBrowser, c.kind);
    EXPECT_EQ("com.brave.browser", c.browserPkg);
}

TEST(ButtonMapContext, OverAnApp) {
    ContextInputs in = over("com.retroarch.aarch64");
    in.launchCore = "/data/data/com.retroarch.aarch64/cores/mgba_libretro_android.so";
    Context c = resolveContext(in, catalog());
    EXPECT_EQ("mgba", c.core);
    EXPECT_EQ("gba", c.systemId);
    in.launchSystem = "gb";   // Nano launched a Game Boy game on mGBA
    EXPECT_EQ("gb", resolveContext(in, catalog()).systemId);
    in.launchSystem = "psx";  // not a system mGBA plays: its own first
    EXPECT_EQ("gba", resolveContext(in, catalog()).systemId);

    in = over("com.retroarch.aarch64");
    in.launchCore = "/x/mgba_libretro_android.so";
    in.historyNewer = true;
    in.historyCore = "/data/user/0/com.retroarch.aarch64/cores/snes9x_libretro_android.so";
    in.historyPath = "/r/game.sfc";
    c = resolveContext(in, catalog());
    EXPECT_EQ("snes9x", c.core);
    EXPECT_EQ("snes", c.systemId);
    EXPECT_EQ("/r/game.sfc", c.romPath);

    EXPECT_EQ("psp", resolveContext(over("org.ppsspp.ppsspp"), catalog()).systemId);
    in = over("org.dolphinemu.dolphinemu");
    in.launchSystem = "wii";
    EXPECT_EQ("wii", resolveContext(in, catalog()).systemId);
    EXPECT_EQ("gc", resolveContext(over("org.dolphinemu.dolphinemu"), catalog()).systemId);
    EXPECT_EQ(Context::kBrowser, resolveContext(over("com.brave.browser"), catalog()).kind);
    EXPECT_EQ(Context::kBrowser, resolveContext(over("net.slions.fulguris.debug"), catalog()).kind);
    EXPECT_EQ(Context::kNone, resolveContext(over("com.shatteredpixel.shatteredpixeldungeon"), catalog()).kind);
    EXPECT_EQ(Context::kNone, resolveContext(over(""), catalog()).kind);
    EXPECT_EQ("drastic-nano", resolveContext(over("com.gammaos.drasticsf"), catalog()).emulator);
    in = over("org.mozilla.firefox");
    in.browserPkgs = "org.mozilla.*, com.brave.browser";
    EXPECT_EQ(Context::kBrowser, resolveContext(in, catalog()).kind);
}

// Systems the emulator catalogue made (the microSD move's auto-add): named by its platform ("ws",
// "wsc") or their folder ("gamecube"), maybe with a player Thimsy doesn't ship (WonderSwan's first
// is the SwanEmu app, which has no profile).
TEST(ButtonMapContext, CatalogMadeSystems) {
    ReaderEnv env;
    env.root = nanotest::fixture("buttonmap/none");
    const char* const kSwan = "com.explusalpha.SwanEmu";
    ContextInputs in = menus(ContextInputs::kFocusSystem, "wsc");
    in.launchPkg = kSwan;
    Context c = resolveContext(in, catalog());
    ASSERT_EQ(Context::kGame, c.kind);
    EXPECT_EQ("", c.emulator);
    MapResult r = build(env, c, cube(), catalog());
    EXPECT_EQ("WonderSwan Color", r.consoleName);
    EXPECT_EQ("wonderswan", r.drawing);
    EXPECT_EQ(kSwan, r.emulatorName);
    ASSERT_FALSE(r.notes.empty());
    EXPECT_EQ(std::string("No reader for ") + kSwan, r.notes[0]);
    in.systemId = "gamecube";   // a system named by its folder
    EXPECT_EQ("gamecube", build(env, resolveContext(in, catalog()), cube(), catalog()).drawing);
    in.systemId = "androidgames";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    in.systemId = "kodi";   // an app the catalogue draws no console for
    in.launchPkg = "org.xbmc.kodi";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);

    // RetroArch on a catalog id: the core's own system.
    in = menus(ContextInputs::kFocusSystem, "wsc");
    in.coreSo = "mednafen_wswan_libretro_android.so";
    r = build(env, resolveContext(in, catalog()), cube(), catalog());
    EXPECT_EQ("wonderswancolor", r.console);
    EXPECT_EQ("WonderSwan Color", r.consoleName);
    EXPECT_EQ("RetroArch Beetle WonderSwan", r.emulatorName);
    in = over("com.retroarch.aarch64");
    in.launchCore = "/data/user/0/com.retroarch.aarch64/cores/mednafen_wswan_libretro_android.so";
    in.launchSystem = "wsc";
    EXPECT_EQ("wonderswancolor", build(env, resolveContext(in, catalog()), cube(), catalog()).console);

    // Over the game: the app Nano launched for that system, and no other.
    in = over(kSwan);
    in.launchApp = kSwan;
    in.launchSystem = "ws";
    in.launchGame = "/storage/1234-ABCD/ROMs/ws/game.ws";
    c = resolveContext(in, catalog());
    ASSERT_EQ(Context::kGame, c.kind);
    EXPECT_EQ("ws", c.systemId);
    EXPECT_EQ("/storage/1234-ABCD/ROMs/ws/game.ws", c.romPath);
    EXPECT_EQ("WonderSwan", build(env, c, cube(), catalog()).consoleName);
    in.launchApp = "org.ppsspp.ppsspp";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    in = over("com.shatteredpixel.shatteredpixeldungeon");
    in.launchApp = "com.shatteredpixel.shatteredpixeldungeon";
    in.launchSystem = "androidgames";
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    OverAppState s;
    launchStateFrom([&](const std::string& k) { return k == "sys.gammaos.nano.launch_app" ? std::string(kSwan) : std::string(); }, &s);
    EXPECT_EQ(kSwan, s.launchApp);
    EXPECT_EQ(kSwan, overAppContextInputs(s).launchApp);
}

TEST(ButtonMapContext, StartSelectDoesNothingOverAGame) {
    ContextInputs in = over("org.ppsspp.ppsspp");
    in.trigger = Trigger::kStartSelectSouth;
    EXPECT_EQ(Context::kNone, resolveContext(in, catalog()).kind);
    in = menus(ContextInputs::kFocusRecent, "psp");
    in.launchPkg = "org.ppsspp.ppsspp";
    in.trigger = Trigger::kStartSelectSouth;
    EXPECT_EQ(Context::kGame, resolveContext(in, catalog()).kind);
}

TEST(ButtonMapContext, ContentHistory) {
    HistoryEntry h;
    ASSERT_TRUE(parseContentHistory(slurp(nanotest::fixture("buttonmap/retroarch/history/content_history.lpl")), &h));
    EXPECT_EQ("mGBA", h.coreName);
    EXPECT_EQ("mgba", coreFromSo(h.corePath));
    EXPECT_FALSE(parseContentHistory("{\"version\":\"1.5\",\"items\":[]}", &h));
    EXPECT_FALSE(parseContentHistory("{\"items\":[{\"path\":\"x\"", &h));
    EXPECT_FALSE(parseContentHistory("{\"items\":[{\"path\":\"x\",\"core_path\":\"DETECT\"}]}", &h));
    EXPECT_FALSE(parseContentHistory("", &h));
}

TEST(ButtonMapContext, PackageMatching) {
    EXPECT_TRUE(packageMatches("net.slions.fulguris.*", "net.slions.fulguris.full.fdroid"));
    EXPECT_FALSE(packageMatches("net.slions.fulguris.*", "net.slions.fulgurisx"));
    EXPECT_TRUE(packageMatches("com.brave.browser", "com.brave.browser"));
    EXPECT_FALSE(packageMatches("com.brave.browser", "com.brave.browser.beta"));
    EXPECT_FALSE(packageMatches("", ""));
    EXPECT_TRUE(isBrowserPackage("", "com.brave.browser"));
    EXPECT_FALSE(isBrowserPackage("", ""));
}

// ---- the catalogue ----

TEST(ButtonMapCatalog, ArtLoads) {
    const Catalog& c = catalog();
    EXPECT_EQ(41u, c.drawings.size());
    const Drawing* snes = c.drawing("snes");
    ASSERT_NE(nullptr, snes);
    ASSERT_NE(nullptr, snes->find("btn-a"));
    EXPECT_EQ("A", snes->find("btn-a")->name);
    ASSERT_NE(nullptr, c.drawing("dualshock"));
    EXPECT_EQ("stick-l", c.drawing("dualshock")->find("btn-l3")->id);   // an alias
    EXPECT_EQ("megadrive", c.systems.at("genesis").drawing);
    EXPECT_EQ("Upper fire", c.systems.at("atari5200").names.at("btn-1"));
    Catalog bad;
    EXPECT_FALSE(loadCatalog("{", "", "", &bad));
    EXPECT_TRUE(loadCatalog("{}", "{\"cores\":5}", "[]", &bad));
}

// Every anchor retropad.json names exists on the drawing of each system the core plays.
TEST(ButtonMapCatalog, RetropadAnchorsExist) {
    const Catalog& c = catalog();
    ASSERT_GE(c.cores.size(), 30u);
    auto anchorLike = [](const std::string& v) {
        for (const char* p : {"btn-", "dpad", "stick-", "c-", "key-", "rpad"})
            if (v.compare(0, strlen(p), p) == 0) return true;
        return false;
    };
    for (const auto& kv : c.cores) {
        const CoreMap& cm = kv.second;
        EXPECT_FALSE(cm.systems.empty()) << cm.core;
        for (const auto& sys : cm.systems) {
            std::string drawing = cm.drawing;
            if (drawing.empty()) {
                auto it = c.systems.find(sys);
                ASSERT_NE(c.systems.end(), it) << cm.core << " plays unknown system " << sys;
                drawing = it->second.drawing;
            }
            const Drawing* dr = c.drawing(drawing);
            ASSERT_NE(nullptr, dr) << cm.core << " " << sys;
            std::map<std::string, std::string> map = cm.map;
            for (const auto& v : cm.variants)
                if (v.option == "@system" && v.value == sys)
                    for (const auto& o : v.map) map[o.first] = o.second;
            for (const auto& m : map) {
                if (m.first == "dpad" && m.second.empty()) continue;
                const std::string base = m.second.substr(0, m.second.find(':'));
                if (!anchorLike(base) || base == "dpad") continue;
                EXPECT_NE(nullptr, dr->find(base)) << cm.core << "/" << sys << ": " << m.first << " -> " << m.second;
            }
        }
    }
}

// Every core the build ships (the build repo's mods/retroarch-cores.txt) has a table.
TEST(ButtonMapCatalog, EveryShippedCoreHasATable) {
    std::string path;
    for (const std::string& d : {std::string(getenv("THIMSY_DIR") ? getenv("THIMSY_DIR") : ""),
                                 std::string(getenv("HOME") ? getenv("HOME") : "") + "/thimsy"}) {
        if (!d.empty() && access((d + "/mods/retroarch-cores.txt").c_str(), R_OK) == 0) { path = d + "/mods/retroarch-cores.txt"; break; }
    }
    if (path.empty()) GTEST_SKIP() << "the build repo's mods/retroarch-cores.txt is not reachable (THIMSY_DIR)";
    std::istringstream in(slurp(path));
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream ls(line);
        std::string w;
        std::vector<std::string> words;
        while (ls >> w) words.push_back(w);
        if (words.empty()) continue;
        const std::string core = words.size() >= 3 && words[0] == "D" && words[1] == "-" ? words[2] : words[0];
        EXPECT_NE(nullptr, catalog().core(core)) << core << " ships without a retropad.json entry";
        n++;
    }
    EXPECT_GE(n, 30);
}

// Every system the build ships (mods/nano-systems.txt, each player resolved in Nano's emulator
// catalog as nano_systems.py does) has a map in the menus and over its game: a context, a reader,
// the console's name and drawing. So does each system the microSD move's auto-add makes for those
// ROM folders from the catalog (NanoMenuXmb.cpp gsAutoAddTick): named by the catalog platform
// ("ws", "wsc", "coleco"), with the platform's first player that runs here, or, made before that
// rule, its first player whatever it is (WonderSwan's SwanEmu app): the console's drawing, no reader.
TEST(ButtonMapContext, EveryShippedSystemHasAMap) {
    std::string thimsy;
    for (const std::string& d : {std::string(getenv("THIMSY_DIR") ? getenv("THIMSY_DIR") : ""),
                                 std::string(getenv("HOME") ? getenv("HOME") : "") + "/thimsy"}) {
        if (!d.empty() && access((d + "/mods/nano-systems.txt").c_str(), R_OK) == 0) { thimsy = d; break; }
    }
    if (thimsy.empty()) GTEST_SKIP() << "the build repo's mods/nano-systems.txt is not reachable (THIMSY_DIR)";
    auto jsonFiles = [](const std::string& dir, const std::function<void(const njson::Value&)>& each) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            njson::Value v;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && njson::parse(slurp(dir + "/" + n), &v)) each(v);
        }
        closedir(d);
    };
    std::map<std::string, std::string> players;                                   // uniqueId -> am start arguments
    std::map<std::string, std::vector<std::string>> platformPlayers;              // platform id -> its players, in order
    jsonFiles(nanoDir() + "/ps3xmb/daijishou", [&](const njson::Value& v) {
        const njson::Value* pf = v.find("platform");
        const std::string pid = pf ? pf->getString("uniqueId") : std::string();
        if (const njson::Value* pl = v.find("playerList"); pl && pl->isArray())
            for (const auto& p : pl->arr) {
                players[p.getString("uniqueId")] = p.getString("amStartArguments");
                if (!pid.empty()) platformPlayers[pid].push_back(p.getString("uniqueId"));
            }
    });
    ASSERT_FALSE(players.empty());
    std::vector<ProfileRef> profs;
    std::set<std::string> profiled;
    jsonFiles(thimsy + "/mods/hotkeys", [&](const njson::Value& v) {
        ProfileRef r{v.getString("id"), {}};
        if (const njson::Value* pk = v.find("packages"); pk && pk->isArray())
            for (const auto& x : pk->arr) r.packages.push_back(x.asString());
        if (r.id != "fallback") profiled.insert(r.packages.begin(), r.packages.end());
        profs.push_back(r);
    });
    std::set<std::string> shippedCores;
    {
        std::istringstream cores(slurp(thimsy + "/mods/retroarch-cores.txt"));
        std::string l;
        while (std::getline(cores, l)) {
            std::istringstream ls(l.substr(0, l.find('#')));
            std::vector<std::string> w;
            for (std::string x; ls >> x;) w.push_back(x);
            if (!w.empty()) shippedCores.insert(w.size() >= 3 && w[0] == "D" && w[1] == "-" ? w[2] : w[0]);
        }
    }
    nanotest::TempDir t("bmapsystems");
    ReaderEnv env;
    env.root = t.path();
    const PadModel pad = cube();
    const std::regex fieldSep(" {2,}"), pkgRe("-n (\\S+?)/"),
        coreRe("-e LIBRETRO (?:\\S*/)?([A-Za-z0-9_\\-]+?)(?:_libretro_android\\.so)?(?:\\s|$)");
    // A player as Nano's emulator picker applies it (applyEmuEntryToSystem): a RetroArch core, else an app.
    auto launchOf = [&](const std::string& player, std::string* coreSo, std::string* pkg) {
        const std::string& args = players[player];
        std::smatch m;
        coreSo->clear();
        pkg->clear();
        if (std::regex_search(args, m, pkgRe)) *pkg = m[1].str();
        if (pkg->find("retroarch") != std::string::npos && std::regex_search(args, m, coreRe)) {
            *coreSo = m[1].str() + "_libretro_android.so";
            pkg->clear();
        }
    };
    // The map for a system, in the menus and over its game; reader: whether a reader must read it.
    auto check = [&](const std::string& sys, const std::string& coreSo, const std::string& pkg, bool reader) {
        SCOPED_TRACE(sys + " " + coreSo + pkg);
        MenuFocus mf;
        mf.item = MenuFocus::kSystem;
        mf.systemId = sys;
        mf.coreSo = coreSo;
        mf.launchPkg = pkg;
        ContextInputs menu = menuContextInputs(mf, Trigger::kFunctionSouth);
        menu.profiles = profs;
        OverAppState os;
        os.fgPkg = os.launchApp = coreSo.empty() ? pkg : "com.retroarch.aarch64";
        os.launchSystem = sys;
        os.launchGame = "/storage/emulated/0/ROMs/" + sys + "/game";
        if (!coreSo.empty()) os.launchCore = "/data/user/0/com.retroarch.aarch64/cores/" + coreSo;
        ContextInputs over = overAppContextInputs(os);
        over.profiles = profs;
        for (const ContextInputs& ci : {menu, over}) {
            const Context c = resolveContext(ci, catalog());
            if (sys == "androidgames") {   // the Android games: nothing to map
                EXPECT_EQ(Context::kNone, c.kind);
                continue;
            }
            ASSERT_EQ(Context::kGame, c.kind);
            const MapResult r = build(env, c, pad, catalog());
            bool noReader = false;
            for (const auto& note : r.notes) noReader = noReader || note.find("No reader") != std::string::npos;
            if (reader) EXPECT_FALSE(noReader) << c.emulator;
            EXPECT_TRUE(catalog().systems.count(r.console)) << r.console;
            EXPECT_NE(r.console, r.consoleName);
            if (c.emulator != "scummvm") EXPECT_NE(nullptr, catalog().drawing(r.drawing)) << r.drawing;   // ScummVM: a table
            if (!noReader) EXPECT_FALSE(r.rows.empty());
        }
    };
    std::istringstream in(slurp(thimsy + "/mods/nano-systems.txt"));
    std::string line;
    int n = 0, made = 0;
    std::set<std::string> wsMade;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.find_first_not_of(' ') == std::string::npos) continue;
        const std::vector<std::string> parts(std::sregex_token_iterator(line.begin(), line.end(), fieldSep, -1),
                                             std::sregex_token_iterator());
        const std::string sys = parts[0];
        std::map<std::string, std::string> f;
        for (size_t i = 1; i < parts.size(); i++) {
            const size_t eq = parts[i].find('=');
            if (eq != std::string::npos) f[parts[i].substr(0, eq)] = parts[i].substr(eq + 1);
        }
        std::string coreSo, pkg = f["package"];
        if (f.count("player")) {
            ASSERT_TRUE(players.count(f["player"])) << sys;
            launchOf(f["player"], &coreSo, &pkg);
        }
        n++;
        check(sys, coreSo, pkg, true);
        if (sys == "androidgames") continue;
        // The catalog platforms the auto-add can make of this system's ROM folder.
        for (const auto& alias : nanosys::romFolderAliases(sys)) {
            auto pp = platformPlayers.find(alias);
            if (pp == platformPlayers.end() || pp->second.empty()) continue;
            made++;
            if (alias == "ws" || alias == "wsc") wsMade.insert(alias);
            std::string firstCore, firstPkg, readyCore, readyPkg;
            launchOf(pp->second[0], &firstCore, &firstPkg);
            check(alias, firstCore, firstPkg, false);
            // RetroArch cores first, as emuPlatformChoices orders the ones that run here.
            for (int pass = 0; pass < 2 && readyCore.empty() && readyPkg.empty(); pass++)
                for (const auto& p : pp->second) {
                    std::string cso, pk;
                    launchOf(p, &cso, &pk);
                    const bool ok = pass == 0 ? !cso.empty() && shippedCores.count(cso.substr(0, cso.find("_libretro")))
                                              : !pk.empty() && profiled.count(pk);
                    if (ok) { readyCore = cso; readyPkg = pk; break; }
                }
            if (!readyCore.empty() || !readyPkg.empty()) check(alias, readyCore, readyPkg, true);
        }
    }
    EXPECT_GE(n, 40);
    EXPECT_GE(made, 30);
    EXPECT_EQ((std::set<std::string>{"ws", "wsc"}), wsMade);
}

// ---- T7: the browser list follows gammapad's BrowserMode ----

TEST(ButtonMapBrowser, CoversEveryCaseOfBrowserModeOnButton) {
    std::string src;
    for (const std::string& top : {std::string(getenv("ANDROID_BUILD_TOP") ? getenv("ANDROID_BUILD_TOP") : ""),
                                   nanoDir() + "/../../../.."}) {
        if (top.empty()) continue;
        src = slurp(top + "/frameworks/native/services/gammapad/BrowserMode.cpp");
        if (!src.empty()) break;
    }
    if (src.empty()) GTEST_SKIP() << "BrowserMode.cpp not reachable (ANDROID_BUILD_TOP)";
    const size_t a = src.find("void BrowserMode::onButton(");
    ASSERT_NE(std::string::npos, a);
    const size_t b = src.find("\nvoid BrowserMode::", a + 10);
    const std::string body = src.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::set<std::string> cases;
    const std::regex re("case (BTN_[A-Z0-9_]+)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it)
        cases.insert((*it)[1]);
    ASSERT_GE(cases.size(), 10u);
    std::set<std::string> listed;
    for (const auto& r : catalog().browser) {
        std::string rest = r.control;
        while (!rest.empty()) {
            const size_t plus = rest.find('+');
            listed.insert(rest.substr(0, plus));
            rest = plus == std::string::npos ? std::string() : rest.substr(plus + 1);
        }
    }
    for (const auto& c : cases) EXPECT_TRUE(listed.count(c)) << c << " is handled by BrowserMode but not in browser.json";
    for (const auto& l : listed)
        if (l.compare(0, 4, "BTN_") == 0) EXPECT_NE(std::string::npos, src.find(l)) << l << " is in browser.json but not in BrowserMode";
    // The bookmark key differs per browser.
    EXPECT_NE(std::string::npos, src.find(catalog().browserBookmarkCtrlB));
}

TEST(ButtonMapBrowser, RowsLabelledThroughThePadModel) {
    Context ctx;
    ctx.kind = Context::kBrowser;
    ctx.browserPkg = "com.brave.browser";
    ReaderEnv env;
    // BrowserMode sees gammapad's codes: BTN_A is the bottom button with the swap off.
    const MapResult r = build(env, ctx, cube(true), catalog());
    EXPECT_EQ("Brave", r.emulatorName);
    ASSERT_FALSE(r.rows.empty());
    EXPECT_EQ("Enter", r.rows[0].consoleName);
    EXPECT_EQ("B", rowKeys(r.rows[0], cube(true)));
    const MapResult s = build(env, ctx, cube(true, true), catalog());
    EXPECT_EQ("A", rowKeys(s.rows[0], cube(true, true)));
    bool chord = false;
    for (const auto& row : r.rows) chord = chord || row.keys == "Select + L1";
    EXPECT_TRUE(chord);
}

// ---- the hotkeys section ----

TEST(ButtonMapHotkeys, ChordRowsFollowTheProfileAndProperties) {
    std::map<std::string, std::string> props;
    ReaderEnv env;
    env.prop = [&](const std::string& k) { auto it = props.find(k); return it == props.end() ? std::string() : it->second; };
    const auto ra = profileActions(slurp(nanotest::fixture("buttonmap/profiles/retroarch.json")));
    EXPECT_EQ("key", ra.at("save_state"));
    const std::string fn = devkeys::builtin().functionPrompt();   // the RG Cube's
    EXPECT_EQ("Anbernic", fn);
    auto rows = chordRows(ra, env, cube(), fn);
    auto has = [&](const std::string& keys, const std::string& action) {
        for (const auto& r : rows) if (r.keys == keys && r.action == action) return true;
        return false;
    };
    EXPECT_TRUE(has("Anbernic + R1", "Save state"));
    EXPECT_TRUE(has("Anbernic + L2", "Rewind"));
    EXPECT_TRUE(has("Hold Anbernic for 3 s", "Quit game"));
    EXPECT_TRUE(has("Anbernic + B", "Button map"));   // the bottom button, printed B
    EXPECT_TRUE(has("Anbernic", "Quick Menu"));
    props["persist.gammaos.hotkeys.save"] = "BUTTON_A";
    props["persist.gammaos.hotkeys.buttonmap"] = "FACE_EAST";
    rows = chordRows(ra, env, cube(), fn);
    EXPECT_TRUE(has("Anbernic + B", "Save state"));
    EXPECT_TRUE(has("Anbernic + A", "Button map"));
    // M64Plus FZ: every action but quit is "none".
    rows = chordRows(profileActions(slurp(nanotest::fixture("buttonmap/profiles/mupen64plus-fz.json"))), env, cube(), fn);
    EXPECT_FALSE(has("Anbernic + R1", "Save state"));
    EXPECT_TRUE(has("Hold Anbernic for 3 s", "Quit game"));
    props["persist.gammaos.hotkeys.enable"] = "0";
    rows = chordRows(ra, env, cube(), fn);
    EXPECT_FALSE(has("Anbernic + L1", "Load state"));
    EXPECT_TRUE(has("Anbernic + A", "Button map"));   // not a hotkey: works with hotkeys off
    // Where Back doubles as the function button, the chords are made with Back.
    const devkeys::Map back = devkeys::resolve(
            R"({"default": {"function": [], "back": [{"label": "Back", "linux": 158, "android": "BACK"}], "fallback": "back"}})",
            [](const std::string&) { return std::string(); });
    props.clear();
    rows = chordRows(ra, env, cube(), back.functionPrompt());
    EXPECT_TRUE(has("Back + R1", "Save state"));
    EXPECT_TRUE(has("Hold Back for 3 s", "Quit game"));
    EXPECT_TRUE(has("Back + B", "Button map"));
    EXPECT_TRUE(has("Back", "Quick Menu"));   // drastic-nano: its Back tap opens its menu
    // Over an app and in Nano's menus a tap is still Back; held 1 to 3 s it opens the Quick Menu.
    rows = chordRows(ra, env, cube(), back.functionPrompt(), back.sharedBack);
    EXPECT_TRUE(has("Hold Back for 1 s", "Quick Menu"));
    EXPECT_FALSE(has("Back", "Quick Menu"));
    EXPECT_TRUE(has("Back + R1", "Save state"));
    EXPECT_TRUE(has("Hold Back for 3 s", "Quit game"));
}

TEST(ButtonMapStrings, ListedOnceAndNonEmpty) {
    std::set<std::string> seen;
    for (const char* s : uiStrings()) {
        EXPECT_TRUE(s && *s);
        EXPECT_TRUE(seen.insert(s).second) << s;
    }
    EXPECT_TRUE(seen.count(browserHint(BrowserState::kNeedsControllerEnable)));
    EXPECT_TRUE(seen.count(browserHint(BrowserState::kBindingsOff)));
    for (const auto& r : catalog().browser) EXPECT_TRUE(seen.count(r.action)) << r.action;
}
