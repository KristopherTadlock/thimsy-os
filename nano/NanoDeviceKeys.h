// The device's function button (the Anbernic button on the RG Cube) and Back button, from the
// per-device table /system/etc/gammaos/device_keys.json (HOTKEYS.md, Devices). Nano and drastic-nano
// read the keys as evdev codes; PhoneWindowManager reads the same table through ThimsyDeviceKeys.java,
// which resolves it the same way.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace android {
namespace devkeys {

constexpr const char* kTablePath = "/system/etc/gammaos/device_keys.json";

struct Key {
    std::string label;
    int linuxCode = -1;       // evdev code, -1 when the entry gives only an Android keycode
    std::string android;      // Android keycode name without KEYCODE_ (F10, BUTTON_MODE)
    bool gamepadOnly = false; // PhoneWindowManager: only from a gamepad source (a keyboard's F10 stays)
    std::string prompt;       // the name printed on the button, where label is not one ("Anbernic / Home")
};

struct Map {
    std::string id = "builtin";
    std::string name;
    std::vector<Key> function;
    std::vector<Key> back;
    // The Back button doubles as the function button (a device with no function button): pressed
    // with another button it is a chord, tapped on its own it is still Back.
    bool sharedBack = false;

    bool isFunction(int linuxCode) const;
    bool isBack(int linuxCode) const;
    // A key that makes a chord while held: the function button, or Back where it doubles as one.
    bool isModifier(int linuxCode) const { return isFunction(linuxCode) || (sharedBack && isBack(linuxCode)); }
    std::vector<int> modifierCodes() const;
    // A function key that is Android's HOME (an "android": "HOME" entry). Android can't hand HOME to
    // an app, so such a key stays the system's whatever Toolbox says: a tap opens the Quick Menu and
    // a 3 s hold goes Home.
    bool isHomeFunction(int linuxCode) const;
    bool hasHomeFunction() const;   // Toolbox can't give this device's function button back to apps
    // The button's name in hotkey prompts ("%s + R1"): the first function key's prompt or label, or
    // Back's where Back doubles as the function button, since the chords are made with it there.
    std::string functionPrompt() const;
};

// What a Back key does when it is let go (it acts on release, never on press). used: another key
// went down while it was held. Where Back doubles as the function button, a tap is Back, a hold of
// 1 to 3 s the Quick Menu, and a longer hold was the hold to quit; elsewhere anything short of the
// 10 s recovery hold is Back. ThimsyDeviceKeys.backRelease is the same.
enum class BackRelease { Nothing, Back, QuickMenu };
constexpr long kSharedBackTapMs = 1000;
constexpr long kSharedBackMenuMs = 3000;
constexpr long kRecoveryHoldMs = 10000;
BackRelease backRelease(bool sharedBack, bool used, long heldMs);

// What the function button does when let go after heldMs (used: another key went down while it
// was held), over an app or in drastic-nano. A press shorter than the 3 s hold opens the Quick Menu,
// and with Universal Hotkeys off, where a non-HOME key has no hold, anything short of the 10 s
// recovery hold does. ThimsyDeviceKeys.functionRelease is the same.
enum class FunctionRelease { Nothing, QuickMenu };
constexpr long kFunctionHoldMs = 3000;
FunctionRelease functionRelease(bool home, bool hotkeys, bool used, long heldMs);

// What the function button held on its own for kFunctionHoldMs does: Home where it is Android's HOME
// key, Universal Hotkeys or not, so Home is always reachable; else the hold to quit, with hotkeys on.
// ThimsyDeviceKeys.functionHold is the same.
enum class FunctionHold { Nothing, Quit, Home };
FunctionHold functionHold(bool home, bool hotkeys);

// The function button is the system's rather than the app's: Toolbox's "Anbernic Button Opens Quick
// Menu" is on (switchOn), or the key is HOME, which can't be given to an app.
inline bool functionToSystem(bool home, bool switchOn) { return home || switchOn; }

// Kiosk Mode takes Start's ES-DE menu away, which leaves the Quick Menu, raised by the function
// button, as the way to every setting. So it holds only while the button opens the Quick Menu
// (functionToSystem), whatever its property says; the Toolbox switch turned off turns it off, and
// Kiosk Mode turned on turns the switch back on.
inline bool kioskInEffect(bool kiosk, bool buttonOpensQuickMenu) { return kiosk && buttonOpensQuickMenu; }
inline bool kioskAfterSwitch(bool switchOn, bool kiosk) { return switchOn && kiosk; }
inline bool switchAfterKiosk(bool kiosk, bool switchOn) { return kiosk || switchOn; }

// The keys with no table: the RG Cube's (F10 from its pad, the gamepad mode button, Back).
Map builtin();

// Picks the table entry for this device. prop returns a system property's value ("" when unset).
// Malformed JSON gives builtin().
Map resolve(const std::string& json, const std::function<std::string(const std::string&)>& prop);

// This device's keys, read once from kTablePath.
const Map& current();

inline bool isFunctionCode(int code) { return current().isFunction(code); }
inline bool isModifierCode(int code) { return current().isModifier(code); }
inline bool isHomeFunctionCode(int code) { return current().isHomeFunction(code); }
inline std::string functionPrompt() { return current().functionPrompt(); }

}  // namespace devkeys
}  // namespace android
