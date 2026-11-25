// The button map's pure pieces Nano, PhoneWindowManager and drastic-nano feed (button_map_overlay_plan.md
// phases 3-5): the handheld's pad table, the chord button per device (the same cases as
// ThimsyDeviceKeys.buttonMapKeycode), the "no two hotkeys share a button" rule with the Button Map row,
// the migration, the context from what has focus in the menus and from what runs over the overlay,
// and the translation of the panel's composed text.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "NanoButtonMap.h"
#include "NanoDeviceKeys.h"
#include "NanoJson.h"
#include "nano_test_util.h"

using namespace android;
using namespace android::bmap;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string nanoDir() { return nanotest::fixtureDir() + "/../.."; }

const Catalog& cat() {
    static Catalog c = [] {
        Catalog x;
        EXPECT_TRUE(loadCatalogFrom(nanoDir() + "/ps3xmb/controllers", nanoDir() + "/ps3xmb/buttonmap", &x));
        return x;
    }();
    return c;
}

const std::vector<PadSpec>& specs() {
    static const std::vector<PadSpec> s = padSpecsFromJson(slurp(nanoDir() + "/ps3xmb/buttonmap/pads.json"));
    return s;
}

DeviceId dev(int vendor, int product, const std::string& name = std::string()) {
    DeviceId d;
    d.vendor = vendor;
    d.product = product;
    d.name = name;
    return d;
}

GamepadState gamepad(bool running, bool swap = false, const std::string& remap = std::string()) {
    GamepadState g;
    g.running = running;
    g.swap = swap;
    g.remap = remap;
    return g;
}

PadModel builtin(bool gammapadOn = false, bool swap = false) {
    return padModelFor(dev(0x045e, 0x0b13), specs(), gamepad(gammapadOn, swap));
}

std::vector<ProfileRef> profiles() {
    return {
        {"retroarch", {"com.retroarch.aarch64"}},
        {"ppsspp", {"org.ppsspp.ppsspp"}},
        {"dolphin", {"org.dolphinemu.dolphinemu"}},
        {"drastic", {"com.dsemu.drastic"}},
        {"fallback", {"*"}},
    };
}

Context menus(const MenuFocus& f, Trigger t = Trigger::kFunctionSouth) {
    ContextInputs in = menuContextInputs(f, t);
    in.profiles = profiles();
    return resolveContext(in, cat());
}

Context overApp(const OverAppState& s) {
    ContextInputs in = overAppContextInputs(s);
    in.profiles = profiles();
    return resolveContext(in, cat());
}

std::string history(const std::string& core, const std::string& path) {
    return "{\"version\": \"1.5\", \"items\": [{\"path\": \"" + path + "\", \"core_path\": \"" + core +
           "\", \"core_name\": \"x\"}, {\"path\": \"/old.sfc\", \"core_path\": \"/c/snes9x_libretro_android.so\"}]}";
}

// Every key the panel may show becomes "<key>", formats with their argument.
const char* marker(const char* s) {
    static std::map<std::string, std::string> seen;
    std::string& v = seen[s];
    if (v.empty()) {
        v = std::string("<") + s + ">";
    }
    return v.c_str();
}

}  // namespace

// ---- the handheld's pad table ----

TEST(ButtonMapPads, TableIsTheBuiltInPad) {
    ASSERT_FALSE(specs().empty());
    PadSpec fixture;
    ASSERT_TRUE(padSpecFromJson(slurp(nanotest::fixture("buttonmap/pads/rg_cube.json")), &fixture));
    const PadSpec& s = specs().front();
    EXPECT_EQ(fixture.id, s.id);
    EXPECT_EQ(0x484b, s.vendor);
    EXPECT_EQ(0x1101, s.product);
    ASSERT_EQ(fixture.buttons.size(), s.buttons.size());
    for (size_t i = 0; i < s.buttons.size(); i++) {
        EXPECT_EQ(fixture.buttons[i].phys, s.buttons[i].phys);
        EXPECT_EQ(fixture.buttons[i].label, s.buttons[i].label);
        EXPECT_EQ(fixture.buttons[i].evdev, s.buttons[i].evdev);
        EXPECT_EQ(fixture.buttons[i].android, s.buttons[i].android);
    }
    EXPECT_EQ(fixture.axes.size(), s.axes.size());
    EXPECT_TRUE(padSpecsFromJson("[]").empty());
    EXPECT_TRUE(padSpecsFromJson("{\"pads\": [{\"id\": \"x\"}]}").empty());   // no buttons
    EXPECT_TRUE(padSpecsFromJson("not json").empty());
}

TEST(ButtonMapPads, ModelPerDevice) {
    // The built-in pad by its own codes, gammapad running or not (gammapad grabs it).
    PadModel m = padModelFor(dev(0x484b, 0x1101), specs(), gamepad(true, true));
    EXPECT_EQ("rg_cube", m.id);
    EXPECT_EQ(305, southEvdev(m));
    EXPECT_EQ(96, southKeycode(m));
    // gammapad's virtual pad stands for the built-in pad, by position, then through the swap.
    m = padModelFor(dev(0x045e, 0x0b13), specs(), gamepad(true, false));
    EXPECT_EQ("rg_cube", m.id);
    EXPECT_EQ(305, southEvdev(m));
    EXPECT_EQ(96, southKeycode(m));
    EXPECT_EQ("B", m.labelOf(Phys::South));
    m = padModelFor(dev(0x045e, 0x0b13), specs(), gamepad(true, true));
    EXPECT_EQ(305, southEvdev(m));   // swapped back as Nano reads it
    EXPECT_EQ(97, southKeycode(m));
    // An Xbox pad (or a real 045e:0b13 with gammapad stopped) is labelled as an Xbox pad.
    m = padModelFor(dev(0x045e, 0x0b13, "Xbox Wireless Controller"), specs(), gamepad(false));
    EXPECT_EQ("xbox", m.id);
    EXPECT_EQ("A", m.labelOf(Phys::South));
    EXPECT_EQ("Xbox Wireless Controller", m.name);
    EXPECT_EQ(304, southEvdev(m));
    // No device (bmap_dev missing): the built-in pad as apps see it now.
    m = padModelFor(DeviceId(), specs(), gamepad(true, true));
    EXPECT_EQ("rg_cube", m.id);
    EXPECT_EQ(97, southKeycode(m));
    EXPECT_EQ("generic", padModelFor(dev(0x1234, 0x5678), specs(), gamepad(false)).id);
    EXPECT_EQ("generic", padModelFor(dev(0x1234, 0x5678), {}, gamepad(false)).id);
}

// PhoneWindowManager's ThimsyDeviceKeys.buttonMapKeycode runs the same cases (tests/unit/
// test_device_keys.py in the build repo).
TEST(ButtonMapPads, ChordKeycodeSameCasesAsPhoneWindowManager) {
    njson::Value doc;
    ASSERT_TRUE(njson::parse(slurp(nanotest::fixture("devkeys/cases.json")), &doc));
    const njson::Value* cases = doc.find("button_map");
    ASSERT_TRUE(cases && cases->isArray() && !cases->arr.empty());
    for (const auto& c : cases->arr) {
        GamepadState gp = gamepad(c.getBool("gammapad"), c.getBool("swap"), c.getString("remap"));
        gp.vid = (int)strtol(c.getString("gammapad_vendor").c_str(), nullptr, 16);
        gp.pid = (int)strtol(c.getString("gammapad_product").c_str(), nullptr, 16);
        const DeviceId d = dev((int)strtol(c.getString("vendor").c_str(), nullptr, 16),
                               (int)strtol(c.getString("product").c_str(), nullptr, 16));
        EXPECT_EQ(c.getInt("want"), buttonMapKeycode(c.getString("setting"), padModelFor(d, specs(), gp)))
            << c.getString("name");
    }
}

TEST(ButtonMapPads, ChordEvdevForNanoAndDrasticNano) {
    // Nano reads the built-in pad's raw codes: the bottom button is 305 (BTN_EAST, Back in Nano).
    EXPECT_EQ(305, buttonMapEvdev("FACE_SOUTH", padModelFor(dev(0x484b, 0x1101), specs(), gamepad(false))));
    EXPECT_EQ(304, buttonMapEvdev("FACE_EAST", padModelFor(dev(0x484b, 0x1101), specs(), gamepad(false))));
    // Through gammapad's virtual pad: Nano swaps the swap back.
    EXPECT_EQ(305, buttonMapEvdev("FACE_SOUTH", builtin(true, false)));
    EXPECT_EQ(305, buttonMapEvdev("FACE_SOUTH", builtin(true, true)));
    EXPECT_EQ(307, buttonMapEvdev("FACE_NORTH", builtin(true, true)));
    // An external pad follows the Linux convention.
    EXPECT_EQ(304, buttonMapEvdev("FACE_SOUTH", padModelFor(dev(0x054c, 0x09cc), specs(), gamepad(false))));
    // A shoulder button: the code of the button sending it; off: nothing.
    EXPECT_EQ(311, buttonMapEvdev("BUTTON_R1", padModelFor(dev(0x484b, 0x1101), specs(), gamepad(false))));
    EXPECT_EQ(0, buttonMapEvdev("NONE", builtin()));
    EXPECT_EQ(0, buttonMapEvdev("BUTTON_Z", builtin()));
}

// ---- Settings: no two hotkeys share a button ----

namespace {

// Save, Load, Fast Forward, Rewind, Quit, then Button Map, as hotkeyTakeButton lists them.
std::vector<ButtonRow> hotkeyRows(const std::string& buttonMap, const std::string& save = "BUTTON_R1",
                                  const std::string& quit = "NONE") {
    return {{save, false}, {"BUTTON_L1", false}, {"BUTTON_R2", false}, {"BUTTON_L2", false}, {quit, false},
            {buttonMap, true}};
}

}  // namespace

TEST(ButtonMapTake, AHotkeyOnTheBottomButtonTakesItFromTheMap) {
    // Gammapad off: BUTTON_A is the bottom button, so Save on A collides with the map's Bottom.
    const PadModel m = builtin(false);
    auto out = takeButton(hotkeyRows("FACE_SOUTH"), 0, "BUTTON_A", m);
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(5, out[0].first);
    EXPECT_EQ("BUTTON_R1", out[0].second);   // the map takes Save's old button, which it can hold
    // The map onto Save's button: Save takes the map's old Bottom, as the keycode it is.
    out = takeButton(hotkeyRows("FACE_EAST", "BUTTON_A"), 5, "FACE_SOUTH", m);
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(0, out[0].first);
    EXPECT_EQ("BUTTON_B", out[0].second);
}

TEST(ButtonMapTake, FollowsGammapadAndTheSwap) {
    // Swap on: the bottom button sends BUTTON_B, so A no longer collides.
    EXPECT_TRUE(takeButton(hotkeyRows("FACE_SOUTH"), 0, "BUTTON_A", builtin(true, true)).empty());
    auto out = takeButton(hotkeyRows("FACE_SOUTH"), 0, "BUTTON_B", builtin(true, true));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(5, out[0].first);
    // Gammapad on, swap off: A is the bottom button, as with gammapad off.
    EXPECT_EQ(1u, takeButton(hotkeyRows("FACE_SOUTH"), 0, "BUTTON_A", builtin(true, false)).size());
}

TEST(ButtonMapTake, HotkeysAmongThemselvesAndTheMapsOnlyButtons) {
    const PadModel m = builtin(false);
    // Two hotkey rows swap as before.
    auto out = takeButton(hotkeyRows("FACE_SOUTH"), 1, "BUTTON_R1", m);
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(0, out[0].first);
    EXPECT_EQ("BUTTON_L1", out[0].second);
    // A D-pad hotkey onto the map's button: the map can't hold a D-pad direction, so it moves to the
    // first face button nothing holds.
    out = takeButton(hotkeyRows("FACE_SOUTH", "DPAD_UP"), 0, "BUTTON_A", m);
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ("FACE_EAST", out[0].second);
    // The map onto a shoulder hotkey: the hotkey takes the map's face button by its keycode.
    out = takeButton(hotkeyRows("FACE_NORTH"), 5, "BUTTON_R1", m);
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(0, out[0].first);
    EXPECT_EQ("BUTTON_Y", out[0].second);   // top is BUTTON_Y on the built-in pad
    // Off takes nothing, and nothing collides with Off.
    EXPECT_TRUE(takeButton(hotkeyRows("FACE_SOUTH"), 5, "NONE", m).empty());
    EXPECT_TRUE(takeButton(hotkeyRows("NONE"), 0, "BUTTON_A", m).empty());
    EXPECT_TRUE(takeButton(hotkeyRows("FACE_SOUTH"), 9, "BUTTON_A", m).empty());
}

TEST(ButtonMapTake, MigrationAtStart) {
    // A Save hotkey already on the bottom button keeps it; the map moves to the right button.
    std::string log;
    EXPECT_EQ("FACE_EAST", migrateButtonMap("FACE_SOUTH", {"BUTTON_A", "BUTTON_L1", "BUTTON_R2", "BUTTON_L2", "NONE"},
                                            builtin(false), &log));
    EXPECT_NE(std::string::npos, log.find("FACE_EAST"));
    // With gammapad's swap on, the bottom button is BUTTON_B.
    EXPECT_EQ("FACE_SOUTH", migrateButtonMap("FACE_SOUTH", {"BUTTON_A"}, builtin(true, true), &log));
    EXPECT_EQ("FACE_EAST", migrateButtonMap("FACE_SOUTH", {"BUTTON_B"}, builtin(true, true), &log));
    // The shipped defaults never move.
    EXPECT_EQ("FACE_SOUTH", migrateButtonMap("FACE_SOUTH", {"BUTTON_R1", "BUTTON_L1", "BUTTON_R2", "BUTTON_L2", "NONE"},
                                             builtin(false), &log));
    EXPECT_EQ("", log);
}

// ---- the context from Nano's menus ----

TEST(ButtonMapMenuFocus, SystemGameRecentAndApps) {
    MenuFocus f;
    f.item = MenuFocus::kSystem;
    f.systemId = "snes";
    f.coreSo = "snes9x_libretro_android.so";
    f.romPath = "/sdcard/ROMs/snes/x.sfc";   // ignored for a system row
    Context c = menus(f);
    EXPECT_EQ(Context::kGame, c.kind);
    EXPECT_EQ("retroarch", c.emulator);
    EXPECT_EQ("snes9x", c.core);
    EXPECT_EQ("", c.romPath);
    // A game whose Game Options give it another emulator.
    f.item = MenuFocus::kGame;
    f.gameEmu = true;
    f.gameCoreSo = "";
    f.gamePkg = "org.ppsspp.ppsspp";
    c = menus(f);
    EXPECT_EQ("ppsspp", c.emulator);
    EXPECT_EQ("/sdcard/ROMs/snes/x.sfc", c.romPath);
    // The per-game choice only counts on a game row.
    f.item = MenuFocus::kSystem;
    EXPECT_EQ("retroarch", menus(f).emulator);
    // A Recently Played entry.
    MenuFocus r;
    r.item = MenuFocus::kRecent;
    r.systemId = "psp";
    r.launchPkg = "org.ppsspp.ppsspp";
    r.romPath = "/sdcard/ROMs/psp/g.iso";
    c = menus(r);
    EXPECT_EQ("ppsspp", c.emulator);
    EXPECT_EQ("psp", c.systemId);
    EXPECT_EQ("/sdcard/ROMs/psp/g.iso", c.romPath);
    // A browser row in Applications, and any other app row.
    MenuFocus a;
    a.item = MenuFocus::kApp;
    a.appPkg = "com.brave.browser";
    c = menus(a);
    EXPECT_EQ(Context::kBrowser, c.kind);
    EXPECT_EQ("com.brave.browser", c.browserPkg);
    a.appPkg = "com.android.settings";
    EXPECT_EQ(Context::kNone, menus(a).kind);
}

TEST(ButtonMapMenuFocus, NothingToMap) {
    MenuFocus f;
    f.item = MenuFocus::kSystem;
    f.systemId = "snes";
    f.coreSo = "snes9x_libretro_android.so";
    // A dialog, a keyboard or a player over the menu.
    f.modal = true;
    EXPECT_EQ(Context::kNone, menus(f).kind);
    // The DSi / Minima category carousel itself.
    f.modal = false;
    f.atCategories = true;
    EXPECT_EQ(Context::kNone, menus(f).kind);
    // The Android apps playlist and its games.
    MenuFocus g;
    g.item = MenuFocus::kGame;
    g.systemId = "androidgames";
    g.launchPkg = "org.thimsy.androidgames";
    EXPECT_EQ(Context::kNone, menus(g).kind);
    // Settings rows, media and the rest.
    MenuFocus o;
    o.item = MenuFocus::kOther;
    EXPECT_EQ(Context::kNone, menus(o).kind);
    EXPECT_EQ(Context::kNone, menus(MenuFocus()).kind);
    // Start + Select + bottom opens it in the menus as Anbernic + bottom does.
    f.atCategories = false;
    EXPECT_EQ(Context::kGame, menus(f, Trigger::kStartSelectSouth).kind);
}

// ---- the context over an app ----

TEST(ButtonMapOverApp, RetroArchCoreFromTheLaunchOrTheHistory) {
    OverAppState s;
    s.fgPkg = "com.retroarch.aarch64";
    s.launchCore = "/data/data/com.retroarch.aarch64/cores/mgba_libretro_android.so";
    s.launchRom = "/sdcard/ROMs/gba/a.gba";
    s.launchSystem = "gba";
    s.launchTime = 1000;
    Context c = overApp(s);
    EXPECT_EQ("mgba", c.core);
    EXPECT_EQ("gba", c.systemId);
    EXPECT_EQ("/sdcard/ROMs/gba/a.gba", c.romPath);
    // Content loaded inside RetroArch after the launch: the history's newest entry names the core.
    s.historyJson = history("/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so", "/sdcard/ROMs/snes/b.sfc");
    s.historyMtime = 1200;
    c = overApp(s);
    EXPECT_EQ("snes9x", c.core);
    EXPECT_EQ("snes", c.systemId);
    EXPECT_EQ("/sdcard/ROMs/snes/b.sfc", c.romPath);
    // A history older than the launch is the last session's.
    s.historyMtime = 900;
    EXPECT_EQ("mgba", overApp(s).core);
    // An unknown launch time trusts the launch.
    s.historyMtime = 1200;
    s.launchTime = 0;
    EXPECT_EQ("mgba", overApp(s).core);
    // RetroArch started outside Nano: the history, whatever its age.
    s.launchCore.clear();
    s.launchRom.clear();
    s.historyMtime = 1;
    EXPECT_EQ("snes9x", overApp(s).core);
    // Malformed or empty history: no core known.
    s.historyJson = "{\"items\": [";
    EXPECT_EQ("", overApp(s).core);
    s.historyJson = "{\"items\": []}";
    EXPECT_EQ("", overApp(s).core);
}

TEST(ButtonMapOverApp, StandalonesBrowsersAndTheRest) {
    OverAppState s;
    s.fgPkg = "org.dolphinemu.dolphinemu";
    s.launchSystem = "wii";
    s.launchGame = "/sdcard/ROMs/wii/g.rvz";
    Context c = overApp(s);
    EXPECT_EQ("dolphin", c.emulator);
    EXPECT_EQ("wii", c.systemId);
    EXPECT_EQ("/sdcard/ROMs/wii/g.rvz", c.romPath);
    s.launchSystem.clear();
    EXPECT_EQ("gc", overApp(s).systemId);   // the emulator's first system
    s = OverAppState();
    s.fgPkg = "net.slions.fulguris.full.playstore";
    EXPECT_EQ(Context::kBrowser, overApp(s).kind);
    s.fgPkg = "com.shatteredpixel.shatteredpixeldungeon";
    EXPECT_EQ(Context::kNone, overApp(s).kind);
    s.fgPkg = "";
    EXPECT_EQ(Context::kNone, overApp(s).kind);
}

// The record of the game Nano launched last decides; launch_core (cleared by the framework, never
// set by the overlay's launches) only stands in when there is no record (a Quick Resume boot).
TEST(ButtonMapOverApp, LaunchStateComesFromTheGameRecord) {
    std::map<std::string, std::string> props = {
        {"sys.gammaos.nano.launch_system", "psx"},
        {"sys.gammaos.nano.launch_game", "/sdcard/ROMs/psx/g.chd"},
        {"sys.gammaos.nano.launch_game_core", "/data/data/com.retroarch.aarch64/cores/swanstation_libretro_android.so"},
        {"sys.gammaos.nano.launch_core", "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so"},
        {"sys.gammaos.nano.launch_rom", "/sdcard/ROMs/snes/old.sfc"},
        {"sys.gammaos.nano.launch_time", "1700000000"},
    };
    auto get = [&](const std::string& k) { auto it = props.find(k); return it == props.end() ? std::string() : it->second; };
    OverAppState s;
    s.fgPkg = "com.retroarch.aarch64";
    launchStateFrom(get, &s);
    EXPECT_EQ("psx", s.launchSystem);
    EXPECT_EQ("/sdcard/ROMs/psx/g.chd", s.launchRom);
    EXPECT_EQ(1700000000, s.launchTime);
    Context c = overApp(s);
    EXPECT_EQ("swanstation", c.core);
    EXPECT_EQ("psx", c.systemId);
    // A plain app launched over the game (setLaunchRomPath("")): nothing of the game is left.
    props["sys.gammaos.nano.launch_system"] = props["sys.gammaos.nano.launch_game"] = "";
    props["sys.gammaos.nano.launch_game_core"] = props["sys.gammaos.nano.launch_core"] = "";
    props["sys.gammaos.nano.launch_rom"] = "";
    s = OverAppState();
    s.fgPkg = "org.ppsspp.ppsspp";
    launchStateFrom(get, &s);
    EXPECT_TRUE(s.launchSystem.empty() && s.launchGame.empty() && s.launchCore.empty() && s.launchRom.empty());
    // A Quick Resume boot sets only launch_core and launch_rom.
    props["sys.gammaos.nano.launch_core"] = "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so";
    props["sys.gammaos.nano.launch_rom"] = "/sdcard/ROMs/snes/a.sfc";
    props["sys.gammaos.nano.launch_time"] = "junk";
    s = OverAppState();
    launchStateFrom(get, &s);
    EXPECT_EQ("/sdcard/ROMs/snes/a.sfc", s.launchRom);
    EXPECT_NE(std::string::npos, s.launchCore.find("snes9x"));
    EXPECT_EQ(0, s.launchTime);
}

// A ROM path of 92 bytes or more does not fit launch_game; setLaunchGame writes it to a file stamped
// with the launch's launch_time instead. A file from another launch or boot is ignored.
TEST(ButtonMapOverApp, LongLaunchGameComesFromTheFile) {
    const std::string rom = "/storage/1234-ABCD/ROMs/psx/Final Fantasy VII (USA) (Disc 1) "
                            "[Greatest Hits] (Rev 1) (Translated) (Undub).chd";
    ASSERT_GE(rom.size(), 92u);
    std::map<std::string, std::string> props = {
        {"sys.gammaos.nano.launch_system", "/storage/1234-ABCD/ROMs/psx"},
        {"sys.gammaos.nano.launch_game", ""},
        {"sys.gammaos.nano.launch_game_core", "/data/data/com.retroarch.aarch64/cores/swanstation_libretro_android.so"},
        {"sys.gammaos.nano.launch_core", ""},
        {"sys.gammaos.nano.launch_rom", ""},
        {"sys.gammaos.nano.launch_time", "1700000000"},
    };
    auto get = [&](const std::string& k) { auto it = props.find(k); return it == props.end() ? std::string() : it->second; };
    OverAppState s;
    s.fgPkg = "com.retroarch.aarch64";
    launchStateFrom(get, &s, "1700000000\n" + rom);
    EXPECT_EQ(rom, s.launchGame);
    EXPECT_EQ(rom, s.launchRom);
    EXPECT_EQ("swanstation", overApp(s).core);
    // The property wins when it holds the path.
    props["sys.gammaos.nano.launch_game"] = "/sdcard/ROMs/psx/g.chd";
    s = OverAppState();
    launchStateFrom(get, &s, "1700000000\n" + rom);
    EXPECT_EQ("/sdcard/ROMs/psx/g.chd", s.launchRom);
    props["sys.gammaos.nano.launch_game"] = "";
    // Left from an earlier launch, or from before a reboot (no launch_time yet).
    for (const char* t : {"1700000001", ""}) {
        props["sys.gammaos.nano.launch_time"] = t;
        s = OverAppState();
        launchStateFrom(get, &s, "1700000000\n" + rom);
        EXPECT_TRUE(s.launchGame.empty()) << t;
    }
    props["sys.gammaos.nano.launch_time"] = "1700000000";
    for (const std::string& f : {std::string(), rom, std::string("170000000\n") + rom}) {
        s = OverAppState();
        launchStateFrom(get, &s, f);
        EXPECT_TRUE(s.launchGame.empty()) << f;
    }
}

// ---- the panel's text in the user's language ----

TEST(ButtonMapTranslate, KeysFormatsAndComposedText) {
    EXPECT_EQ("<Default mapping>", translate("Default mapping", marker));
    EXPECT_EQ("<Core remap: %s>", std::string(marker("Core remap: %s")));
    // A format: its translation with the argument in the %s.
    auto fmt = [](const char* s) -> const char* {
        if (!strcmp(s, "Core remap: %s")) return "Remap del core: %s";
        if (!strcmp(s, "Hold %s for 3 s")) return "Mantén %s 3 s";
        if (!strcmp(s, "Left stick")) return "Stick izquierdo";
        if (!strcmp(s, "D-pad")) return "Cruceta";
        if (!strcmp(s, "Select")) return "Seleccionar";
        if (!strcmp(s, "Keyboard %s")) return "Teclado %s";
        return s;
    };
    EXPECT_EQ("Remap del core: Snes9x.rmp", translate("Core remap: Snes9x.rmp", fmt));
    EXPECT_EQ("Mantén Anbernic 3 s", translate("Hold Anbernic for 3 s", fmt));
    // Alternatives and chords, part by part; printed button names stay as they are.
    EXPECT_EQ("Cruceta / Stick izquierdo", translate("D-pad / Left stick", fmt));
    EXPECT_EQ("Seleccionar + L1", translate("Select + L1", fmt));
    EXPECT_EQ("Teclado F2", translate("Keyboard F2", fmt));
    EXPECT_EQ("Default mapping \xC2\xB7 Remap del core: a.rmp", translate("Default mapping \xC2\xB7 Core remap: a.rmp", fmt));
    // Not a key: as it is (a console's "A", a core's own button name, an emulator's name).
    EXPECT_EQ("A", translate("A", marker));
    EXPECT_EQ("III", translate("III", marker));
    EXPECT_EQ("<Turbo B>", translate("Turbo B", marker));
    EXPECT_EQ("RetroArch Snes9x", translate("RetroArch Snes9x", marker));
    EXPECT_EQ("", translate("", marker));
    EXPECT_EQ("Default mapping", translate("Default mapping", nullptr));
    // An argument that is itself a key is translated too.
    EXPECT_EQ("<Cursor mode: %s>", std::string(marker("Cursor mode: %s")));
    auto both = [](const char* s) -> const char* {
        if (!strcmp(s, "Cursor mode: %s")) return "Modo cursor: %s";
        if (!strcmp(s, "Right click")) return "Clic derecho";
        return s;
    };
    EXPECT_EQ("Modo cursor: Clic derecho", translate("Cursor mode: Right click", both));
}

// retropad.json names a console's controls the drawing lacks, and notes on rows: words are keys
// (§7.4), only printed letters and symbols ("III", "A+B", "Coleco #") stay as they are.
TEST(ButtonMapTranslate, EveryRetropadLabelIsTranslatable) {
    auto anchorLike = [](const std::string& v) {
        for (const char* p : {"btn-", "dpad", "stick-", "c-", "key-", "rpad", "touch"})
            if (v.compare(0, strlen(p), p) == 0) return true;
        return false;
    };
    auto printed = [](const std::string& v) {
        if (v.compare(0, 7, "Coleco ") == 0) return true;   // the keypad's * and #, after the maker's name
        return std::none_of(v.begin(), v.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    };
    std::set<std::string> labels;
    ASSERT_FALSE(cat().cores.empty());
    for (const auto& kv : cat().cores) {
        const CoreMap& cm = kv.second;
        for (const auto& m : cm.map) labels.insert(m.second);
        for (const auto& n : cm.notes) labels.insert(n.second);
        for (const auto& v : cm.variants) {
            for (const auto& m : v.map) labels.insert(m.second);
            labels.insert(v.note);
        }
    }
    size_t words = 0;
    for (const std::string& l : labels) {
        if (l.empty() || anchorLike(l) || printed(l)) continue;
        words++;
        const std::string t = translate(l, marker);
        EXPECT_NE(l, t) << "\"" << l << "\" has no key in uiStrings()";
        EXPECT_EQ('<', t[0]) << l << " -> " << t;
    }
    EXPECT_GE(words, 30u);
    EXPECT_EQ("<Turbo %s>", std::string(marker("Turbo %s")));
    EXPECT_EQ("<Turbo A+B>", translate("Turbo A+B", marker));
    EXPECT_EQ("<C-Up (hold R2)>", translate("C-Up (hold R2)", marker));
}

TEST(ButtonMapTranslate, EveryChordRowIsTranslatable) {
    ReaderEnv env;
    const std::map<std::string, std::string> actions = {{"save_state", "retroarch"}, {"load_state", "retroarch"},
                                                        {"fast_forward", "retroarch"}, {"rewind", "retroarch"},
                                                        {"quit", "retroarch"}};
    auto rows = chordRows(actions, env, builtin(false), devkeys::builtin().functionPrompt());
    const auto backRows = chordRows(actions, env, builtin(false), "Back", true);   // Back doubling as the button
    rows.insert(rows.end(), backRows.begin(), backRows.end());
    ASSERT_FALSE(rows.empty());
    for (const auto& r : rows) {
        EXPECT_NE(r.action, translate(r.action, marker)) << r.action;
        // The keys are the function button, printed labels and the generic names.
        const std::string k = translate(r.keys, marker);
        if (r.keys.find("Volume") != std::string::npos || r.keys.find("Start") != std::string::npos
            || r.keys.find("Hold") != std::string::npos)
            EXPECT_NE(r.keys, k) << r.keys;
    }
}
