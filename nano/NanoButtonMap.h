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

// The button map (button_map_overlay_plan.md): which button of the pad in hand presses which button
// of the console being emulated. This file is the pure core: the pad model (physical button -> what
// an app or Nano receives, through gammapad and ABXY Swap), the context (what the chord is over),
// the art catalogue, and build(), which turns an emulator's bindings into labelled rows. The
// per-emulator readers are in NanoButtonMapReaders.cpp. No Android dependency: host-tested.
//
// User-visible text leaves this module in English, as the keys NanoI18n translates; the caller runs
// it through trDyn when it draws (uiStrings() lists every one).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace android {
namespace bmap {

// ---- the pad in hand ----

// A physical control, by position. The face buttons are where they sit, not what they print.
enum class Phys : uint8_t {
    South, East, North, West, L1, R1, L2, R2, L3, R3, Start, Select, Home,
    DUp, DDown, DLeft, DRight,
    LUp, LDown, LLeft, LRight,
    RUp, RDown, RLeft, RRight,
    kCount
};
constexpr int kPhysCount = (int)Phys::kCount;
const char* physId(Phys p);                          // "south", "l1", "dpad_up", "lstick_left" ...
bool physFromId(const std::string& id, Phys* out);
bool physIsDpad(Phys p);
bool physIsLStick(Phys p);
bool physIsRStick(Phys p);

// What a reader says an emulator binds, in the space the emulator reads it.
struct Input {
    enum Kind : uint8_t {
        kNone,
        kKey,     // Android keycode (what an app receives)
        kAxis,    // Android axis id, with dir +1 / -1
        kEvKey,   // Linux evdev key code (what Nano and drastic-nano read)
        kEvAbs,   // Linux evdev ABS code, with dir
        kOther,   // bound to something no pad sends (a keyboard key, a mouse): text says what
    };
    Kind kind = kNone;
    int code = 0;
    int dir = 0;
    std::string text;   // kOther only

    static Input key(int c) { Input i; i.kind = kKey; i.code = c; return i; }
    static Input axis(int c, int d) { Input i; i.kind = kAxis; i.code = c; i.dir = d; return i; }
    static Input evKey(int c) { Input i; i.kind = kEvKey; i.code = c; return i; }
    static Input evAbs(int c, int d) { Input i; i.kind = kEvAbs; i.code = c; i.dir = d; return i; }
    static Input other(const std::string& t) { Input i; i.kind = kOther; i.text = t; return i; }
    bool none() const { return kind == kNone; }
    bool operator==(const Input& o) const {
        return kind == o.kind && code == o.code && dir == o.dir && text == o.text;
    }
};

// Android constants used by the readers and the pad model.
namespace akey {
enum : int {
    kBack = 4, kDpadUp = 19, kDpadDown = 20, kDpadLeft = 21, kDpadRight = 22, kDpadCenter = 23,
    kEnter = 66, kEscape = 111,
    kButtonA = 96, kButtonB = 97, kButtonC = 98, kButtonX = 99, kButtonY = 100, kButtonZ = 101,
    kL1 = 102, kR1 = 103, kL2 = 104, kR2 = 105, kThumbL = 106, kThumbR = 107,
    kStart = 108, kSelect = 109, kMode = 110,
};
}
namespace aaxis {
enum : int { kX = 0, kY = 1, kZ = 11, kRX = 12, kRY = 13, kRZ = 14, kHatX = 15, kHatY = 16,
             kLTrigger = 17, kRTrigger = 18, kGas = 22, kBrake = 23 };
}
// Android keycode name (without KEYCODE_) <-> number, for the names settings and props store.
int keycodeFromName(const std::string& name);        // "BUTTON_A" -> 96, "F2" -> 132; 0 when unknown
std::string keycodeName(int keycode);                // the reverse; "" when unknown
std::string keyboardKeyText(int keycode);            // "F2", "Space", "Esc"; "" when not a keyboard key

// The device that pressed the chord: sys.gammaos.nano.bmap_dev, or EVIOCGID / EVIOCGNAME.
struct DeviceId {
    int vendor = 0, product = 0;
    std::string name, descriptor;
};
// "<vendor hex>:<product hex>:<descriptor>:<name>" (the name may hold ':'). False when malformed.
bool parseDeviceId(const std::string& s, DeviceId* out);

// gammapad's settings, read once per open (persist.gammaos.gamepad.*, init.svc.gammapad).
struct GamepadState {
    bool running = false;            // init.svc.gammapad == running
    bool swap = false;               // abxy_swap
    std::string remap;               // remap_btn: "from:to,..." Linux codes, after the swap
    int vid = 0x045e, pid = 0x0b13;  // the virtual device (device_vid / device_pid)
    std::string name = "Xbox Wireless Controller";
    bool merge = true, hideSource = true;
};

// One physical pad as its maker built it, from the device table (the other half of HOTKEYS.md,
// Devices) or from the Linux gamepad convention for a pad with no entry. Nothing here knows any
// particular handheld: the caller says which pad it is.
struct PadSpec {
    std::string id, name, drawing;   // drawing: "" for none
    int vendor = 0, product = 0;
    struct Button {
        Phys phys = Phys::South;
        std::string label;           // as printed: "B", "Cross", "L1"
        int evdev = -1;              // the raw evdev code
        int android = 0;             // the Android keycode its key layout gives (gammapad off)
        int gammapadLinux = -1;      // the code gammapad's key layout makes of it (-1: = evdev)
    };
    std::vector<Button> buttons;
    struct Axis {                    // a stick axis, the hat, a trigger
        Phys neg = Phys::kCount, pos = Phys::kCount;   // kCount: that half sends nothing
        int evdev = -1;              // ABS code
        int android = -1;            // Android axis id (its key layout)
        std::string posLabel;        // printed name of the positive half (a trigger: "LT")
    };
    std::vector<Axis> axes;
};
// {"id","name","drawing","vendor":"484b","product":"1101","buttons":[{"phys":"south","label":"B",
//  "linux":305,"android":"BUTTON_A"}],"axes":[{"neg":"dpad_left","pos":"dpad_right","linux":16,
//  "android":"HAT_X"}]}. False when it does not parse or has no buttons.
bool padSpecFromJson(const std::string& json, PadSpec* out);
// A pad that follows the Linux gamepad convention (BTN_SOUTH is the bottom button, Android's layouts
// by position), labelled as its maker prints it: Xbox (045e), PlayStation (054c), Switch Pro (057e),
// else generic ("Bottom", "Right" ...).
PadSpec standardPadSpec(const DeviceId& dev);

// The pad as an app (Android keycodes and axes) and as Nano (evdev) receive it now.
struct PadModel {
    std::string id, name, drawing;
    bool viaGammapad = false;
    bool swapped = false;            // gammapad's face-button codes differ from Nano's (Nano undoes the swap)
    std::array<std::string, kPhysCount> label{};   // printed names ("" when the pad lacks it)
    std::array<bool, kPhysCount> present{};
    std::vector<std::pair<Input, Phys>> sends;     // every Input the pad can send -> its button

    std::vector<Phys> physFor(const Input& in) const;   // every button that sends it (may be none)
    bool has(Phys p) const { return present[(int)p]; }
    // What p sends in a space (kKey, kAxis, kEvKey, kEvAbs); kNone when it sends none there.
    Input input(Phys p, Input::Kind space) const;
    std::string labelOf(Phys p) const;           // printed label, else a generic name
};
// The pad in hand. source: the physical pad (with gammapad on, the pad merged into the virtual
// one). With gammapad running, its face buttons are given by position (the built-in pad's are by
// print, so they are swapped) and swapped again by ABXY Swap, then go through remap_btn and reach
// apps through the virtual device's layout (Generic.kl); stopped, the swap does nothing.
PadModel padModel(const PadSpec& source, const GamepadState& gp);

// The chord's button for the device: the Android keycode PhoneWindowManager sees, and the evdev
// code Nano and drastic-nano read. 0 when the pad has no bottom button.
int southKeycode(const PadModel& m);
int southEvdev(const PadModel& m);

// The physical button a setting names: FACE_SOUTH/EAST/NORTH/WEST by position, a BUTTON_* (or
// other keycode name) by what sends it on this pad. kCount when none.
Phys settingButton(const std::string& value, const PadModel& m);
// Two settings name the same physical button (FACE_SOUTH and BUTTON_A collide with gammapad off).
bool sameButton(const std::string& a, const std::string& b, const PadModel& m);
// Button Map Button at start: if a hotkey already holds its button, the first free face button
// (south, east, north, west). Returns the value to store; *log says what moved ("" when nothing).
std::string migrateButtonMap(const std::string& current, const std::vector<std::string>& hotkeys,
                             const PadModel& m, std::string* log);

// ---- when the chord opens the map ----

enum class Trigger : uint8_t {
    kFunctionSouth,      // Anbernic (the device's function button) + the bottom button
    kStartSelectSouth,   // Start + Select + the bottom button
};
enum class Where : uint8_t {
    kNanoMenus,          // Nano's own menus, any theme, the overlay raised as the launcher
    kOverGame,           // over a running game or app (PhoneWindowManager), drastic-nano
};
// Decided 2026-11-13: Anbernic + bottom works everywhere; Start + Select + bottom only in Nano's
// menus (in a game it is a common in-game combination).
bool triggerOpens(Trigger t, Where w);
// The map over a game is the Quick Menu's kind of overlay: it blocks the game's input and the game
// keeps running. Gammapad is never turned on for it.
constexpr bool kPausesGame = false;
constexpr bool kBlocksGameInput = true;
constexpr bool kTurnsOnGammapad = false;

// The browser list is always shown; when gammapad's BrowserMode is not running it is greyed with
// one line saying what to turn on.
enum class BrowserState : uint8_t { kActive, kNeedsControllerEnable, kBindingsOff };
BrowserState browserState(bool gammapadRunning, bool bindingsEnabled);
const char* browserHint(BrowserState s);   // "" when active

// ---- the art and data ----

struct Anchor {
    std::string id, name, kind, side;   // side: "w", "e", "n", "s"
    float x = 0, y = 0, r = 0;          // centre and radius, viewBox units
    float lx = 0, ly = 0;               // the art's label point
};
struct Drawing {
    std::string id, file;               // file relative to the art folder ("svg/snes.svg")
    float vb[4] = {0, 0, 1000, 1000};   // viewBox
    float labelBounds[4] = {0, 0, 1000, 1000};
    float aspect = 1.0f;
    bool dynamicLabels = false;         // keyboards: label only the mapped keys
    std::vector<Anchor> anchors;
    std::map<std::string, std::string> aliases;   // btn-l3 -> stick-l
    const Anchor* find(const std::string& id) const;   // aliases followed
};
struct SystemArt {
    std::string name, drawing, alt;
    std::map<std::string, std::string> names;   // anchor id -> the name this console prints
};
// A RetroArch core's RetroPad -> console table (retropad.json).
struct CoreMap {
    std::string core, library;          // .so basename, its library_name (config folder names)
    std::vector<std::string> systems;   // Nano system ids it plays; the first is its default
    std::string drawing;
    bool perGame = false;               // fbneo, MAME: the game names its own buttons
    // RetroPad id ("b", "a", "l2", "lstick", "rstick" ...) -> console button: an anchor id, else text.
    std::map<std::string, std::string> map;
    std::map<std::string, std::string> notes;
    struct Variant {
        int device = -1;                          // input_libretro_device_p1, -1 any
        std::string option, value;                // a core option, "" any
        std::string drawing;                      // "" keeps the core's
        std::map<std::string, std::string> map;   // overrides ("" removes)
        std::string note;
    };
    std::vector<Variant> variants;
    std::vector<std::string> optionKeys;          // core options the variants read
};
struct BrowserRow {
    std::string control;      // "BTN_A", "BTN_TL", "LSTICK", "DPAD", "START", "SELECT+BTN_TL" ...
    std::string action;       // English
    std::string cursorAction; // in cursor mode, "" = same
};
struct Catalog {
    std::map<std::string, Drawing> drawings;
    std::map<std::string, SystemArt> systems;     // Nano systems and Daijishou platforms
    std::map<std::string, CoreMap> cores;
    std::vector<BrowserRow> browser;
    std::string browserBookmarkCtrlB;             // package prefix that bookmarks with Ctrl+B
    const Drawing* drawing(const std::string& id) const;
    const CoreMap* core(const std::string& core) const;
};
// controllers.json (the art), retropad.json and browser.json. False when controllers.json does not
// parse; the other two may be empty.
bool loadCatalog(const std::string& controllersJson, const std::string& retropadJson,
                 const std::string& browserJson, Catalog* out);
// The same from the installed folders (/data/system/nano_xmb/... checked first on the device).
bool loadCatalogFrom(const std::string& artDir, const std::string& dataDir, Catalog* out);

// ---- context ----

struct ProfileRef {   // a hotkey profile: id and packages (mods/hotkeys/*.json)
    std::string id;
    std::vector<std::string> packages;
};
struct ContextInputs {
    Where where = Where::kNanoMenus;
    Trigger trigger = Trigger::kFunctionSouth;
    // In Nano's menus: what has focus.
    enum Focus : uint8_t { kFocusNone, kFocusSystem, kFocusGame, kFocusRecent, kFocusAppRow, kFocusOther };
    Focus focus = kFocusNone;
    std::string systemId;      // romDir ("snes"); "androidgames" is the Android apps playlist
    std::string coreSo;        // the player's RetroArch core .so (path or basename), "" for an app
    std::string launchPkg;     // the player's app
    std::string romPath;
    std::string appPkg;        // a row in Applications
    bool drasticNano = false;  // persist.gammaos.nano.drastic_nano=1
    // Over an app.
    std::string fgPkg;
    std::string launchCore, launchRom, launchSystem, launchGame;   // sys.gammaos.nano.launch_*
    std::string launchApp;     // sys.gammaos.nano.launch_app: the app Nano launched last
    bool historyNewer = false; // content_history.lpl's newest entry is newer than the launch
    std::string historyCore, historyPath;
    // Both.
    std::string browserPkgs;   // persist.gammaos.gamepad.browser_pkgs ("" = gammapad's default)
    std::vector<ProfileRef> profiles;
};
struct Context {
    enum Kind : uint8_t { kNone, kGame, kBrowser };
    Kind kind = kNone;
    std::string systemId;     // "" when unknown
    std::string emulator;     // hotkey profile id: "retroarch", "ppsspp", "drastic-nano" ..., "" for an
                              // app with none (its console's drawing, no reader)
    std::string package;
    std::string core;         // RetroArch core basename ("snes9x"), else ""
    std::string romPath;
    std::string browserPkg;
};
Context resolveContext(const ContextInputs& in, const Catalog& cat);
// A system id as the catalogue knows it: itself, else the first of its ROM-folder aliases it has
// (systems made from the emulator catalogue are named by its platform, "ws", or by their folder,
// "gamecube"). "" when it knows none.
std::string knownSystem(const Catalog& cat, const std::string& systemId);
// The core's own id for a system (one of cm.systems, by the same aliases); "" when it plays none.
std::string coreSystem(const CoreMap& cm, const std::string& systemId);
std::string coreFromSo(const std::string& soPath);   // ".../snes9x_libretro_android.so" -> "snes9x"
bool packageMatches(const std::string& entry, const std::string& pkg);   // gammapad's rule
bool isBrowserPackage(const std::string& list, const std::string& pkg);
std::string defaultSystemFor(const std::string& emulator);   // ppsspp -> psp ...
// RetroArch's content_history.lpl: its newest entry. False when malformed or empty.
struct HistoryEntry { std::string path, corePath, coreName; };
bool parseContentHistory(const std::string& json, HistoryEntry* newest);

// ---- what the overlay shows ----

struct Row {
    std::string consoleId;    // anchor id, "anchor:dir" for one direction of a stick, or "" (text)
    std::string consoleName;  // English generic names, console prints as is
    std::vector<Phys> phys;   // physical buttons on the pad in hand
    std::string offPad;       // bound to what this pad does not have ("Keyboard F2")
    std::string keys;         // a chord, as its buttons' labels ("Select + L1"); "" = phys
    std::string note;
    bool unbound() const { return phys.empty() && offPad.empty(); }
};
struct HotkeyRow {
    std::string keys;         // "Anbernic + R1", "Select + R2"
    std::string action;       // English
};
enum class Source : uint8_t { kConfig, kDefaults, kUnknown, kNotMapped };
struct MapResult {
    Context ctx;
    std::string console;       // system id shown
    std::string consoleName;   // "SNES"
    std::string emulatorName;  // "RetroArch Snes9x"
    std::string drawing;       // drawing id, "" = table only
    std::vector<Row> rows;
    Source source = Source::kUnknown;
    std::string notMappedWhere;   // kNotMapped: where to map ("Settings > GameCube Input")
    std::vector<std::string> notes;
    std::vector<HotkeyRow> hotkeys;
    std::string padName;
    bool browserGreyed = false;
    std::string browserHint;
};
// The label of a row's physical buttons on the pad in hand ("B", "D-pad", "B / L1"); "—" unbound.
std::string physLabel(const std::vector<Phys>& phys, const PadModel& m);
// What the row says for "Your controller": its chord, its buttons, what it is bound to off the pad, or "—".
std::string rowKeys(const Row& row, const PadModel& m);

// Readers are declared in NanoButtonMapReaders.h; build() calls the one for ctx.emulator.
struct ReaderEnv {
    std::string root;                                          // "" on the device
    DeviceId dev;                                              // the pad in hand
    std::function<std::string(const std::string&)> prop;       // system properties ("" unset)
    // The retroarch.cfg the launches pass (NanoMenu::retroarchConfigFile, on the card when RetroArch's
    // folder is there); "" for RetroArch's default.
    std::string raConfig;
    std::string prop_(const std::string& k) const { return prop ? prop(k) : std::string(); }
};
MapResult build(const ReaderEnv& env, const Context& ctx, const PadModel& pad, const Catalog& cat);

// The universal and fixed chords for the hotkeys section. actions: the profile's action -> driver
// ("none" leaves it out). functionLabel: the device's function button ("Anbernic"). tapIsBack: the
// button is Back doubling as the function button, whose tap stays Back and whose 1 s hold opens the
// Quick Menu (devkeys::backRelease); drastic-nano's Back tap opens its menu, so it passes false.
std::vector<HotkeyRow> chordRows(const std::map<std::string, std::string>& actions,
                                 const ReaderEnv& env, const PadModel& pad,
                                 const std::string& functionLabel, bool tapIsBack = false);
std::map<std::string, std::string> profileActions(const std::string& profileJson);

// ---- what Nano, PhoneWindowManager and drastic-nano feed in (phases 3-5) ----

// The handheld's own pads (ps3xmb/buttonmap/pads.json, {"pads": [<padSpecFromJson object>...]}).
// The first is the built-in pad, the one gammapad's virtual device stands for.
std::vector<PadSpec> padSpecsFromJson(const std::string& json);
std::vector<PadSpec> loadPadSpecsFrom(const std::string& dataDir);   // <dataDir>/pads.json
// gammapad's settings from the properties (persist.gammaos.gamepad.*, init.svc.gammapad); prop
// returns "" for an unset property. Numbers as gammapad reads them (decimal or 0x hex).
GamepadState gamepadStateFrom(const std::function<std::string(const std::string&)>& prop);
// The pad that sent an event: the built-in pad by its own codes (gammapad grabs it while running),
// gammapad's virtual device as the built-in pad through gammapad (swap, remap_btn, Generic.kl), any
// other pad by the Linux convention and its own labels. An unknown device (no bmap_dev) is the
// built-in pad as apps see it now.
PadModel padModelFor(const DeviceId& dev, const std::vector<PadSpec>& specs, const GamepadState& gp);

// The codes a Button Map Button value arrives as from this pad: an Android keycode for
// PhoneWindowManager (ThimsyDeviceKeys.buttonMapKeycode is the same), an evdev code for Nano and
// drastic-nano. FACE_* is by position; a keycode name is that keycode, and for evdev the code of the
// button sending it. 0 when the setting is NONE or (evdev) the pad lacks the button.
int buttonMapKeycode(const std::string& setting, const PadModel& m);
int buttonMapEvdev(const std::string& setting, const PadModel& m);

// Two hotkeys can't share a button (Settings > Gamepad Settings > Hotkeys). rows: every button row's
// current value; the Button Map row holds FACE_* or BUTTON_* and the hotkey rows Android keycode
// names, compared as the physical buttons they are (FACE_SOUTH is BUTTON_A with gammapad off).
// Setting rows[row] to value: the rows that held the same button and the value each takes now, the
// changed row's old button in that row's own words (a FACE_* becomes the keycode it sends, a face
// keycode its FACE_*). A button the Button Map row can't take (a D-pad direction) leaves it the first
// face button no hotkey holds.
struct ButtonRow { std::string value; bool buttonMap = false; };
std::vector<std::pair<int, std::string>> takeButton(const std::vector<ButtonRow>& rows, int row,
                                                    const std::string& value, const PadModel& m);

// What has focus in Nano's menus (any theme), as plain values the caller reads off its menu.
struct MenuFocus {
    bool modal = false;           // a dialog, option menu, keyboard, viewer or player is over the menu
    bool atCategories = false;    // DSi / Minima: the category carousel itself has the focus
    enum Item : uint8_t { kNoItem, kSystem, kGame, kRecent, kApp, kOther };
    Item item = kNoItem;
    std::string systemId;         // the item's system (a game's, a recent entry's): its romDir
    std::string coreSo, launchPkg;   // the system's player, or the recent entry's
    std::string romPath;
    bool gameEmu = false;         // Game Options > Emulator set for this game: these replace the system's
    std::string gameCoreSo, gamePkg;
    std::string appPkg;           // kApp: the row's package
};
// The context inputs for a chord in the menus; trigger is the chord pressed.
ContextInputs menuContextInputs(const MenuFocus& f, Trigger trigger);

// Over an app (the overlay raised by qs_summon=3): what the caller read from the properties and
// RetroArch's content_history.lpl. launchTime and historyMtime are seconds since the epoch, 0 unknown.
struct OverAppState {
    std::string fgPkg;
    std::string launchCore, launchRom, launchSystem, launchGame;   // sys.gammaos.nano.launch_*
    std::string launchApp;        // sys.gammaos.nano.launch_app
    int64_t launchTime = 0;       // sys.gammaos.nano.launch_time
    std::string historyJson;      // "" when unreadable
    int64_t historyMtime = 0;
};
// The launch fields from the properties: the game record setLaunchGame keeps for the app Nano
// launched last (launch_system, launch_game, launch_game_core), else, with no record (a Quick
// Resume boot), launch_core and launch_rom. gameFile: the text of the file setLaunchGame writes
// for a ROM path too long for launch_game (NanoMenuUtils.h kLaunchGameFile), taken only when its
// first line is this launch's launch_time.
void launchStateFrom(const std::function<std::string(const std::string&)>& prop, OverAppState* s,
                     const std::string& gameFile = std::string());
// RetroArch writes content_history.lpl when content loads, so an entry at least as new as Nano's
// launch (or any entry, for RetroArch started outside Nano) names the core running now.
ContextInputs overAppContextInputs(const OverAppState& s);
constexpr const char* kContentHistoryPath =
    "/storage/emulated/0/RetroArch/playlists/builtin/content_history.lpl";

// The text the panel shows, in the user's language. tr is trDyn: only the strings uiStrings() lists
// are looked up (a console's printed "A" or "L2" is never a key), and composed text is
// translated by its parts: "Core remap: Snes9x.rmp" through "Core remap: %s", "Select + L1" and
// "D-pad / Left stick" part by part. Text no key matches is returned as it is.
using Translator = std::function<const char*(const char*)>;
std::string translate(const std::string& english, const Translator& tr);

// Every English string this module and NanoButtonMapArt can show, for the translation pass.
const std::vector<const char*>& uiStrings();

}  // namespace bmap
}  // namespace android
