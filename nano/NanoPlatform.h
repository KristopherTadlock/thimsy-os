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

// Nano's platform seam, layer 4 (docs/linux/IMPLEMENTATION.md B.6). C++17; no Android or Linux
// system header is included, and none may be.
//
// Declared when used, not ahead: this header holds only the platform object and how it is made,
// installed and reached. Each interface (Display, Launcher, Audio, Volumes, Power, SystemActions,
// InputPolicy, Network, Boot, Capabilities with the Cap set, Settings, and Emulators) is declared by
// the step that moves the code behind it, in the same commit as the move, sized to that code.
//
// Who may use it. drastic-nano, gammaos-nano-scrape, the host tests and the other binaries built
// from Nano's files never call install(). No file they compile may include this header or name
// nanoplat:: (tools/nano-tree/tests/test_nano_ratchets.py holds that rule); they reach platform
// behaviour through the shims and link-time substitution (layers 1 to 3).
//
// Ownership and threading. main() calls createPlatform(), then install(), before any Nano thread
// starts. The platform then lives until the process exits and is never destroyed, so a reference
// from get() stays valid on every thread, including during exit. Which platform is linked is a
// link-time choice: each platform library (platform/firmware, platform/linux, platform/host) defines
// createPlatform() once.
#pragma once

#include <memory>
#include <optional>  // Launcher (G7-launch-b)
#include <string>    // Launcher (G7-launch-b)
#include <cstdint>     // Audio (G7-audio)
#include <functional>  // Audio (G7-audio)
#include <vector>      // Volumes (G7-volumes)

namespace android {
struct LaunchTuple;  // NanoLaunchCommit.h (G7-launch-b)
}  // namespace android

namespace nanoplat {

// --- Display (G7-display) ------------------------------------------------------------------------
// The panel: taken at start-up, and handed to a game's process and back. Render thread only (the
// NanoMenu thread). startScanout runs in readyToRun() before its EGL set-up, so no EGL context is
// current then; releaseForChild and reacquire need Nano's context current. The firmware's is
// readyToRun's DRM splash decision, moved (platform/firmware/Display.cpp); Linux's always takes the panel and
// applies the rotation contract (platform/linux/display/); the host's records what it did
// (platform/host/FakeDisplay.cpp).
class Display {
 public:
    virtual ~Display() = default;

    // readyToRun(): takes the panel for Nano's own scanout (DRM master and the splash,
    // drmEarlySplash) unless this process presents through SurfaceFlinger. `earlyDrmFd` is main()'s
    // early DRM master fd, or -1; `overlayMode` is the --overlay instance; `forceSfPath` is
    // readyToRun's rule for the SoCs whose composer must keep master. Returns the early fd still
    // held, or -1; sDrmActive (NanoMenuDrm.h) tells whether the panel is Nano's.
    virtual int startScanout(int earlyDrmFd, bool overlayMode, bool (*forceSfPath)()) = 0;

    // The hand-off to a child process that becomes DRM master itself (a game), and back, for a
    // platform where Nano keeps running while the child runs (Linux). The firmware's home exits on
    // a launch instead, so its versions do nothing and return false.
    // releaseForChild: finishes the GPU's work, waits for the flip in flight, disables Nano's
    // overlay planes, frees the scanout ring and drops master; false when the panel was not Nano's.
    // reacquire: takes master back (retrying while the child's file is still being closed),
    // modesets and rebuilds the ring; false when the panel could not be taken back.
    virtual bool releaseForChild() = 0;
    virtual bool reacquire() = 0;
};
// --- end Display ---------------------------------------------------------------------------------

// --- Launcher (G7-launch-b) ----------------------------------------------------------------------
// Starting a game or an app. Every call site's launch handshake goes through commitLaunch()
// (NanoLaunchCommit.h), which hands its tuple to commit(). The firmware's commit is today's
// handshake, moved (platform/firmware/Launcher.cpp): the property writes in the call site's order,
// which Android's framework turns into an activity start as Nano exits. Linux's writes the same
// keys, and the handshake's trigger starts the game (platform/linux/Launcher.cpp, through the
// control keys); the host's writes them and records each launch (platform/host/FakeLauncher.cpp).

// Nano's stored launch, as the handshake's keys hold it when its trigger (do_launch=1 or
// service.bootanim.nano_retroarch=1) is written.
struct LaunchRequest {
    enum class Kind { Core, Package };
    Kind kind = Kind::Core;    // Core when the handshake names a RetroArch core, else Package
    std::string package;       // launch_app
    std::string core;          // launch_core: the core's Android path; "" for a package
    std::string rom;           // launch_rom, else launch_game (a standalone's game); may be ""
    std::string system;        // launch_system: the ROM folder's system id; may be ""
    std::string intent;        // launch_intent ("file": the intent file was written)
    std::string trigger;       // the key whose write fired the launch
};

struct ChildExit {
    int pid = 0;
    int status = 0;            // the exit status, or the signal number when signaled
    bool signaled = false;
};

class Launcher {
 public:
    virtual ~Launcher() = default;

    // Whether a launched game runs only once Nano has exited: the firmware's true (Android starts
    // the activity, and the panel is freed, as the home exits); Linux's false (Nano hands the panel
    // to the child). The value states the intended contract, not today's behaviour: nothing reads
    // it yet, Nano's launch sites still exit after a launch on every platform, and until threadLoop
    // polls the child the Linux launcher holds the game until Nano's menu has ended.
    virtual bool nanoExitsOnLaunch() const = 0;

    // commitLaunch(): one call site's handshake writes, in its order, on the calling thread.
    virtual void commit(const android::LaunchTuple& tuple) = 0;

    // Starts `request` as a child in its own session and process group, after handing the panel
    // over (Display::releaseForChild). False, with *error, when it can't be started. The firmware
    // launches only through the handshake, so its launch() always returns false; Linux's checks the
    // request and queues it until Nano's menu has ended (platform/linux/Launcher.h).
    virtual bool launch(const LaunchRequest& request, std::string* error) = 0;
    // A child was started and pollChild() has not yet reported its end.
    virtual bool childRunning() const = 0;
    // Non-blocking: the child's end, reported once, after it has ended.
    virtual std::optional<ChildExit> pollChild() = 0;
    // Asks the child to quit: SIGTERM to its process group.
    virtual void requestChildQuit() = 0;
};
// --- end Launcher --------------------------------------------------------------------------------

// ---- Audio (G7-audio): NanoAudio.cpp's output streams, in the pull shape of its AAudio callback.
// Each player opens its own stream (NanoAudioPlayer per track, NanoSfxPlayer for the UI sounds), so
// Audio makes streams rather than being one. Samples are 16-bit, interleaved.

struct AudioFormat {
    const char* name = "";    // the stream's name in log lines ("NanoAudio", "NanoSfx")
    int rate = 0;             // frames per second asked for
    int channels = 0;         // samples per frame
    bool lowLatency = false;  // firmware: AAUDIO_PERFORMANCE_MODE_LOW_LATENCY rather than NONE
};

// One open output stream, made by Audio::open() and paused until start(). Destroying it stops and
// closes it; never destroy it from its own Fill or OnDisconnect.
class AudioStream {
 public:
    virtual ~AudioStream() = default;
    virtual bool start() = 0;               // true when the output accepted the start
    virtual void pause() = 0;
    virtual int sampleRate() const = 0;     // what the output agreed to (may differ from the request)
    virtual int channelCount() const = 0;
};

class Audio {
 public:
    virtual ~Audio() = default;
    // What a Fill returns: go on, or stop calling it (the stream then stays stopped until start()).
    static constexpr int kFillContinue = 0;
    static constexpr int kFillStop = 1;
    // Writes `count` frames into `frames`; called on the platform's audio thread while started.
    using Fill = std::function<int(int16_t* frames, int count)>;
    // The output device went away (a headphone plugged in, Bluetooth): the stream is dead and is to
    // be destroyed and opened again from another thread. Called on a platform thread.
    using OnDisconnect = std::function<void()>;
    // Null when the stream can't be opened; the platform logs why under format.name.
    virtual std::unique_ptr<AudioStream> open(const AudioFormat& format, Fill fill, OnDisconnect onDisconnect) = 0;
};

// --- Volumes (G7-volumes) -------------------------------------------------------------------------
// The removable volumes, a microSD card or a USB drive, as apps see them: /storage/<id>. What
// NanoSdMove.cpp's and NanoEsdeStore.cpp's /storage and /mnt/media_rw scans asked the filesystem,
// moved behind one interface. Any thread. Every call may touch the filesystem (on Linux it also
// reads /run/thimsy/volumes.json when the session has changed it), so none is for a frame's hot path.
class Volumes {
 public:
    virtual ~Volumes() = default;
    // The microSD card: the first removable volume Nano can write to (/storage/<id>), the slot's
    // before a USB drive's; "" when there is none (NanoMenu::sdCardRoot).
    virtual std::string cardRoot() = 0;
    // The volume at storagePath (/storage/<id>) is the microSD slot's (an MMC disk), not a USB
    // drive's (NanoSdMove.cpp's sdIsSlot).
    virtual bool isSlot(const std::string& storagePath) = 0;
    // The volume `id` is in: its /storage/<id> is there (NanoMenu::esdeStoreFollowCard).
    virtual bool present(const std::string& id) = 0;
    // The volume `id` is not in: neither its raw mount nor /storage/<id> is there (NanoSdMove.cpp's
    // sdVolumeAbsent).
    virtual bool absent(const std::string& id) = 0;
    // /storage/<id><rel> for each removable volume but `vol` where that is a folder Nano can write
    // to, in no set order (NanoMenu::esdeStoreFollowCard: the ES-DE folder on another card).
    virtual std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) = 0;
};

// --- Power (G7-power) ----------------------------------------------------------------------------
// The battery, the platform's half of the brightness, and the power actions: what NanoMenuSystem.cpp
// asked Android's health and lights HALs, Settings and init for. The firmware's is that code, moved
// (platform/firmware/Power.cpp); Linux reads /sys/class/power_supply and asks thimsy-session to
// reboot or power off (platform/linux/Power.cpp); the host's records (platform/host/FakePower.cpp).
// The backlight nodes themselves are not here: NanoBacklight.h (shared with drastic-nano) drives
// /sys/class/backlight on every platform.
class Power {
 public:
    virtual ~Power() = default;

    // pollBattery(), about once a second on the render thread: the battery's charge (0-100) and
    // whether it is charging (or full), written in place; a value no source can give is left as it
    // was (-1 and false until a first reading).
    virtual void readBattery(int& percent, bool& charging) = 0;

    // applyBrightness() and the sleep paths: the platform's own backlight control besides the
    // sysfs nodes (the firmware's lights HAL), at `level` in the panel's node range (0 is off).
    // false when the platform has none, or it was not reached.
    virtual bool setBrightnessViaHal(int level) = 0;

    // syncBrightnessToAndroid(), after it wrote persist.gammaos.nano.brightness: records Nano's
    // level (1-255) where the rest of the system keeps the brightness (the firmware: Settings.System,
    // from a worker thread, the latest level only). Any thread; does not block.
    virtual void mirrorBrightness(int level) = 0;

    // readAndroidBrightness(): that recorded level (1-255), or -1 when there is none or it can't
    // be read. May block (the firmware runs `settings get`); not on the render thread after boot.
    virtual int savedBrightness() = 0;

    // prepareShutdown()'s last step, after the running game was asked to save and quit: `action`
    // is "reboot" or "shutdown" (also "recovery", "safemode" and "android", which only the firmware
    // has). Returns at once; the platform carries it out.
    virtual void powerAction(const char* action) = 0;
};
// --- end Power -----------------------------------------------------------------------------------

// --- SystemActions (G7-power) --------------------------------------------------------------------
// The radios and the clock settings: the Wi-Fi, Bluetooth, time-format and time-zone commands of
// NanoMenuSettings.cpp and NanoMenuSetupWizard.cpp. The firmware's are those commands, moved
// (platform/firmware/SystemActions.cpp); Linux's use wpa_supplicant's control socket, bluetoothctl
// and the zoneinfo link (platform/linux/actions/); the host's record (platform/host/FakeActions.cpp).
// Called from the menu's input and worker threads; each may block for as long as its command runs.
// The state they change is read back by the callers' own polls, so nothing is returned.
class SystemActions {
 public:
    virtual ~SystemActions() = default;

    // toggleWifiRadio(): turns the Wi-Fi radio on or off.
    virtual void setWifiEnabled(bool on) = 0;
    // forgetWifiNetwork(): removes the saved network with this id (an id the platform's own saved
    // list gave, >= 0).
    virtual void forgetWifiNetwork(int savedNetId) = 0;
    // toggleBtRadio(): turns the Bluetooth radio on or off.
    virtual void setBluetoothEnabled(bool on) = 0;
    // connectBtDevice(): reconnects a bonded device ("AA:BB:CC:DD:EE:FF").
    virtual void connectBluetooth(const std::string& mac) = 0;
    // setSetupClockFormats(), after persist.gammaos.nano.clock12 was written: tells the system's
    // clock the 12-hour (true) or 24-hour format. Does not block.
    virtual void setTimeFormat(bool h12) = 0;
    // setupApplyTimezone(): makes `tz` (an Olson id, "Europe/London") the system time zone, so
    // persist.sys.timezone reads it back.
    virtual void setTimeZone(const std::string& tz) = 0;
};
// --- end SystemActions ---------------------------------------------------------------------------

// --- Network (G7-net) ----------------------------------------------------------------------------
// The Wi-Fi and Bluetooth queries and operations of the Settings screens (NanoMenuSettings.cpp) and
// of the HUD's network poll (NanoMenuNet.cpp): the commands they ran (`cmd wifi`, `dumpsys`,
// `settings get`, the gammaos-net helper) and the parsing of their output. The firmware's is that
// code, moved (platform/firmware/Network.cpp); Linux's asks wpa_supplicant's control socket and
// bluetoothctl (platform/linux/actions/Network.cpp); the host's answers from the config's scripted
// networks and devices (platform/host/FakeNetwork.cpp). Methods are named after their call sites.
// NanoNetBridge, the firmware's resident system_server helper, stays at the call sites: it is
// reached through sys.gammaos.nano.* properties that only the firmware's bridge publishes.
//
// Threads. Each call blocks for as long as its commands run: seconds, a pairing up to minutes. The
// callers make them from their own worker threads, except wifiRadioEnabled, wifiAddresses and
// wifiMacAndIp (the menu's render thread) and unpairBt (its input handling), as on the firmware,
// where a timeout bounds each command; every platform bounds those to a few seconds.

// A Wi-Fi network as the screens list it (NanoMenu::WifiNetEntry's fields).
struct WifiNetEntry {
    std::string ssid;
    std::string bssid;
    int rssi = -127;        // dBm; -127 when unknown
    int security = 0;       // 0 open, 1 WEP, 2 WPA/WPA2, 3 WPA3, 4 OWE
    int savedNetId = -1;    // the platform's id of the saved network, -1 when not saved
    bool connected = false;
};

// A Bluetooth device as the screens list it (NanoMenu::BtDevEntry's fields).
struct BtDevEntry {
    std::string name;
    std::string address;    // "AA:BB:CC:DD:EE:FF"
    bool bonded = false;
    bool connected = false;
    int cod = 0;            // the class of device, 0 when unknown
};

class Network {
 public:
    virtual ~Network() = default;

    // The HUD's levels; NanoMenu's WifiLevel and BtLevel have the same values.
    enum WifiLevel {
        kWifiLevel_Unknown = 0,
        kWifiLevel_Off,
        kWifiLevel_Disconnected,  // radio on, no network
        kWifiLevel_Connected,
    };
    enum BtLevel {
        kBtLevel_Unknown = 0,
        kBtLevel_Off,
        kBtLevel_On,
        kBtLevel_Connected,       // at least one device connected
    };

    // The net poll thread (NanoMenuNet.cpp). autoEnableWifi: once at its start, turns the radio on
    // when it is off and networks are saved (the firmware's "remember last on/off"). pollState:
    // each tick the bridge did not answer, the state written in place over the caller's
    // Unknown/0/"" (wifiBars 0-4, wifiSsid the connected network's, btConnected the devices
    // connected); what can't be read is left as it was.
    virtual void autoEnableWifi() = 0;
    virtual void pollState(WifiLevel& wifiLevel, int& wifiBars, std::string& wifiSsid, BtLevel& btLevel,
                           int& btConnected) = 0;

    // refreshWifiList(): whether the radio is on and the network it is connected to (""), then,
    // when it is on, the saved networks (savedNetId set, rssi -127) and the scan's results
    // (savedNetId -1), one entry per line the platform gave (the caller merges them).
    struct WifiRadio {
        bool on = false;
        std::string connectedSsid;
    };
    virtual WifiRadio wifiListRadio() = 0;
    struct WifiLists {
        std::vector<WifiNetEntry> saved;
        std::vector<WifiNetEntry> scanned;
    };
    virtual WifiLists wifiLists() = 0;
    // wifiScanThreadFunc(): asks for a fresh scan; its results reach a later wifiLists().
    virtual void startWifiScan() = 0;
    // connectToSavedWifi(): connects to the saved network savedNetId and waits for it; true when
    // it connected.
    virtual bool connectSavedWifi(int savedNetId) = 0;
    // addAndConnectWifi(): saves (or, with force, overwrites) the network `ssid` with `security`
    // (WifiNetEntry's) and `password` (ignored for open and OWE), connects unless it is already
    // the connected one (force: always), and waits about 12 s for it, retrying once.
    virtual void joinWifi(const std::string& ssid, int security, const std::string& password, bool force) = 0;
    // wifiConnectWatch(): one look at a connect to `ssid` in progress.
    enum class JoinCheck { Pending, Connected, WrongPassword };
    virtual JoinCheck checkWifiJoin(const std::string& ssid) = 0;
    // wifiRadioEnabled(): the radio is on.
    virtual bool wifiRadioEnabled() = 0;
    // buildNetStatusBody(): the interface's IPv4 address, and, when `connected`, the default
    // gateway and two DNS servers; the Wi-Fi MAC. "" for what is not known.
    struct WifiAddresses {
        std::string ip, gw, dns, dns2, mac;
    };
    virtual WifiAddresses wifiAddresses(bool connected) = 0;
    // buildSysInfoBody(): the Wi-Fi MAC and IPv4 address ("" when not known).
    struct WifiMacAndIp {
        std::string mac, ip;
    };
    virtual WifiMacAndIp wifiMacAndIp() = 0;
    // startNetTest(): the IPv4 address (""), and whether the system found the link reaches the
    // Internet (the firmware's VALIDATED).
    struct WifiLink {
        std::string ip;
        bool validated = false;
    };
    virtual WifiLink wifiLink() = 0;

    // refreshBtList(): whether the radio is on and, when it is, the bonded devices.
    struct BtList {
        bool on = false;
        std::vector<BtDevEntry> devices;
    };
    virtual BtList btListState() = 0;
    // discoverBtDevices(): an inquiry of about 8 s; the devices found (bonded set for bonded ones).
    virtual std::vector<BtDevEntry> discoverBt() = 0;
    // pairBtDevice(), unpairBtDevice(): bonds with / removes the bond of `mac`; true when done.
    virtual bool pairBt(const std::string& mac) = 0;
    virtual bool unpairBt(const std::string& mac) = 0;
    // The Bluetooth wizard (btWiz*Async): the radio's real state; the bonded devices (connected
    // set); turning the radio on or off, waiting for it; an inquiry of about 15 s; pairing `a`
    // with `p` ("", a PIN, or "--ask": the user answers the device's question through
    // sys.gammaos.bt.pair_request/pair_reply); accepting the inbound request of `a` (variant `v`,
    // 0 a classic PIN); connecting, disconnecting and removing `a`. The bools are true when done.
    virtual bool btRadioOn() = 0;
    virtual std::vector<BtDevEntry> bondedBt() = 0;
    virtual void setBtRadio(bool on) = 0;
    virtual std::vector<BtDevEntry> btWizScan() = 0;
    virtual bool btWizPair(const std::string& a, const std::string& p) = 0;
    virtual void btConfirmInbound(const std::string& a, int v) = 0;
    virtual bool btWizConnect(const std::string& a) = 0;
    virtual bool btWizDisconnect(const std::string& a) = 0;
    virtual bool btWizUnpair(const std::string& a) = 0;
};
// --- end Network ---------------------------------------------------------------------------------

// --- InputPolicy (G7-input) ----------------------------------------------------------------------
// Who else gets the pads' events while Nano reads them. Nano's own evdev code (opening
// /dev/input/event*, reading, hotplug) is the same on every platform; what differs is silencing.
// The firmware's home grabs each device node as it opens it (EVIOCGRAB when
// persist.gammaos.nano.grab_input is set) and releases the grab before closing it: that code, moved
// from NanoMenuInput.cpp (platform/firmware/InputPolicy.cpp). On Linux the built-in pad is
// thimsy-padd's, and silencing it is padd's intercept (sys.thimsy.padd.intercept), so Linux leaves
// the device nodes alone (platform/linux/InputPolicy.cpp). The host's grabs as the firmware's does,
// so its fake devices see the same grabs, and records each call (platform/host/FakeInput.cpp).
class InputPolicy {
 public:
    virtual ~InputPolicy() = default;

    // Where Nano opened a device node: openInputDevices' first scan, checkInputHotplug's IN_CREATE,
    // or checkInputHotplug's once-a-second sweep for missed devices. The firmware's log lines name it.
    enum class Opened { AtStart, Hotplugged, Swept };

    // Render thread only (the NanoMenu thread: openInputDevices and checkInputHotplug).
    // deviceOpened: Nano has just opened the node `path` as `fd` (O_RDONLY | O_NONBLOCK; fd >= 0),
    // before it keeps the fd. deviceClosing: Nano is about to close `fd`, a device it opened whose
    // node went away (IN_DELETE, or the sweep found it deleted).
    virtual void deviceOpened(int fd, const char* path, Opened where) = 0;
    virtual void deviceClosing(int fd) = 0;

    // Any thread. Stops the built-in pad's events reaching anyone but Nano's menu (true), and gives
    // them back (false), for a platform where the menu can be up while a game runs (Linux: padd's
    // intercept; the menu then reads the pad from padd's tap). The firmware silences per device in
    // deviceOpened and its home exits on a launch, so its version does nothing.
    virtual void silenceGamePad(bool on) = 0;
};
// --- end InputPolicy -----------------------------------------------------------------------------

// --- Boot (W6-boot) ------------------------------------------------------------------------------
// What Nano's start-up asks the platform before and while it comes up: whether this boot is Nano's
// at all, and whether what it reads at start-up (its saved settings, the sound system) is there yet.
// The firmware's is that Android code, moved (platform/firmware/Boot.cpp): readyToRun's "boot to
// Android" choice and its hand-over to Android's boot animation, the waits of readyToRun and
// threadLoop for init's persistent properties, startupPreloadStart's look at them, and the boot
// sound's look for the audio service. Linux's: Nano is the only owner of the screen, and its store
// is loaded before Nano starts, so nothing is read and nothing waits (platform/linux/Boot.cpp). The
// host's answers from its config and records each call (platform/host/FakeBoot.cpp).
class Boot {
 public:
    virtual ~Boot() = default;

    // readyToRun(), right after it took main()'s early DRM fd: true when Nano runs this boot; false
    // when the platform's own boot screen is to have the display instead (the firmware's: the user
    // chose to boot to Android). Render thread; may block (the firmware's waits up to 10 s for its
    // saved settings when the choice it caches is missing).
    virtual bool nanoOwnsBootScreen() = 0;
    // Only after nanoOwnsBootScreen() gave false: lets go of `earlyDrmFd` (main()'s early DRM
    // master fd, or -1), gives the display to the platform's boot screen unless `overlayMode` (the
    // --overlay instance never owns it), and ends the process with status 0. A platform without a
    // boot screen of its own (Linux) never gives false; if called it logs, touches nothing and
    // returns, and Nano runs on.
    virtual void handOverBootScreen(int earlyDrmFd, bool overlayMode) = 0;

    // Render thread. Waits a bounded time for Nano's saved settings to become readable, and logs
    // when they still are not; Nano then goes on with what it reads. `reader` names the call site,
    // whose bound and log lines the firmware keeps: QuickResumeCheck is readyToRun's, before the
    // DraStic Quick Resume pre-check (2 s); MenuSetup is threadLoop's first pass, before it re-reads
    // the settings and checks Quick Resume (3 s).
    enum class SettingsReader { QuickResumeCheck, MenuSetup };
    virtual void awaitSavedSettings(SettingsReader reader) = 0;
    // Non-blocking: the saved settings are readable now (startupPreloadStart preloads the fonts
    // only then).
    virtual bool savedSettingsLoaded() = 0;

    // Non-blocking, any thread: the system service Nano's sound output goes through is up
    // (bootSoundStart's thread waits for it before it opens the boot sound; the firmware's is
    // Android's AudioService, which unmutes the streams). A platform whose output needs no such
    // service (Linux: ALSA, opened directly) answers true.
    virtual bool soundSystemReady() = 0;
};
// --- end Boot ------------------------------------------------------------------------------------

// --- Capabilities (W6-caps) ----------------------------------------------------------------------
// What the platform can do, by intent, for features that are not settings (docs/linux/
// ARCHITECTURE-AUDIT.md R3 and R4): the menus ask Platform::has() and hide what the platform has
// not implemented, so it is never attempted. The firmware's answers true for every capability
// (Android does all of these today; platform/firmware/Capabilities.cpp). Linux's answers true only
// for what it really implements (platform/linux/Capabilities.cpp); the host's answers from its
// config, true unless the config turns a capability off (platform/host/FakeCapabilities.cpp). Any
// thread; never blocks. Adding a capability: append it before Count, and give each platform's
// answer (Linux's switch has no default, so the compiler asks for it).
enum class Cap {
    OverlayApps,         // the overlay home: Nano drawn over a running app, with its app controls
    DualStack,           // two panels driven as one stacked display
    SystemSettingsApp,   // the platform's own full settings app, opened from Nano's menus
    FactoryReset,        // erase the user's data and start again
    ResetRadios,         // turn the Wi-Fi and Bluetooth radios off and on again (Reset Options)
    RebootRecovery,      // restart into the recovery system
    RebootSafeMode,      // restart with third-party apps disabled
    RebootAndroid,       // restart into Android's own home instead of Nano
    OtaUpdate,           // check for, download and install a system update (OA-16)
    Mtp,                 // USB file transfer to a computer
    TypecRole,           // switch the USB-C port between host and device
    MediaPlayback,       // video snaps, theme videos and the music player (OA-24)
    CpuProfiles,         // performance profiles for the CPU and GPU
    Dnd,                 // Do Not Disturb
    LocationToggle,      // the location service on or off
    DarkTheme,           // the system-wide dark theme
    ImeSelect,           // choosing the system's input method (keyboard app)
    Notifications,       // the system's notifications, shown in Nano
    ArchiveBrowsing,     // games inside .zip and .7z in the file explorer (OA-16)
    ManualViewer,        // PDF and comic-archive manuals (OA-16)
    Count                // not a capability: how many there are
};

class Capabilities {
 public:
    virtual ~Capabilities() = default;
    virtual bool has(Cap cap) const = 0;
};
// --- end Capabilities ----------------------------------------------------------------------------

// --- Settings (W6-caps) --------------------------------------------------------------------------
// The settings dispatcher behind NanoMenuSettingsTree.h's readSettingValue, writeSettingValue and
// settingSupported (the settings tree's rows, the XMB's settings bindings, and their direct
// callers). A setting is a store and a key: kProp is a property (a Nano preference, or a key another
// part of the system reads), kGlobal, kSecure and kSystem are the platform's system settings tables
// (Android's Settings.Global, Secure and System); the values are android::SettingSource's
// (NanoMenuSettingsTree.h). The firmware's is Android's code, moved: property_get and property_set,
// `settings get` and `settings put`, and the post-write hooks that make some writes take effect
// (platform/firmware/Settings.cpp); it supports every key. Linux's answers from a keyed table:
// Nano's own preferences through the property store, the few system settings Linux implements
// itself, and nothing else, so an unknown key is unsupported (platform/linux/Settings.cpp). The
// host's supports what its config does not turn off (platform/host/FakeSettings.cpp). Any thread;
// read and write may block (the firmware's run a command for the system tables).
enum class SettingSource { kNone, kProp, kGlobal, kSecure, kSystem };

class Settings {
 public:
    virtual ~Settings() = default;
    // Whether the platform keeps `key` of `source` and gives a write to it its effect, or (read-only
    // state, such as the device's name) provides it; the menus hide a row bound to an unsupported
    // setting. Does not block.
    virtual bool supports(SettingSource source, const std::string& key) const = 0;
    // The stored value; `def` when there is none, it can't be read, or the setting is unsupported.
    virtual std::string read(SettingSource source, const std::string& key, const std::string& def) = 0;
    // Stores `value` and applies it. A write to an unsupported setting does nothing (logged once
    // per setting) and returns false; false too when the store refused the value.
    virtual bool write(SettingSource source, const std::string& key, const std::string& value) = 0;
};
// --- end Settings --------------------------------------------------------------------------------

// --- Emulators (W8-EMU) --------------------------------------------------------------------------
// Which emulators this platform can run, and the image's own default emulator per system (OA-38: a
// pick this platform cannot run starts with that default instead). Nano names an emulator the same
// way on every platform, as its launch fields do: a libretro core by its Android file name
// (<core>_libretro_android.so) and a standalone emulator by its Android package. The firmware's
// answers are NanoMenu::coreSoExists and packageInstalled, moved (platform/firmware/Emulators.cpp);
// Linux's come from the image's core folder and fail closed for every app, since it starts none yet
// (platform/linux/Emulators.cpp); the host's from the config's scripted lists
// (platform/host/FakeEmulators.cpp). Any thread; each call may touch the filesystem.

// One system's emulator in the launch fields' terms (NanoMenu::XmbSystem's): a core, or an app.
struct SystemEmulator {
    std::string system;   // the system's id, which is its ROM folder for the built-in systems ("psx")
    std::string coreSo;   // a libretro core, "<core>_libretro_android.so"; "" for an app
    std::string package;  // a standalone emulator's package; "" for a core
    std::string intent;   // the app's am start template ({file.uri} and the rest); "" for a core
};

class Emulators {
 public:
    virtual ~Emulators() = default;
    // The libretro core `coreSo` (Nano's name for it) can run here. True for "": no core to check.
    virtual bool coreInstalled(const std::string& coreSo) = 0;
    // The standalone emulator `package` can run here. True for "": no app to check.
    virtual bool appInstalled(const std::string& package) = 0;
    // The image's default emulator for each system it has one for, which Nano writes into the
    // device's store of defaults on its first run (OA-38). Empty when the image's defaults are the
    // systems shared code already reads: the firmware's are the build's nano_systems.json, else
    // Nano's built-in table.
    virtual std::vector<SystemEmulator> imageDefaults() = 0;
};
// --- end Emulators -------------------------------------------------------------------------------

// The platform's services, one member per G7 area. Each platform library derives from it to keep
// its own state, and its createPlatform() sets every member, so call sites use them untested.
struct Platform {
    virtual ~Platform();
    // Whether the platform implements `cap` (caps->has); false when caps is not set.
    bool has(Cap cap) const;
    std::unique_ptr<Display> display;  // G7-display: never null once createPlatform() returns
    std::unique_ptr<Launcher> launcher;  // G7-launch-b: never null once createPlatform() returns
    std::unique_ptr<Audio> audio;  // G7-audio: set by every createPlatform(), never null after install()
    std::unique_ptr<Volumes> volumes;   // G7-volumes
    std::unique_ptr<Power> power;  // G7-power: never null once createPlatform() returns
    std::unique_ptr<SystemActions> actions;  // G7-power: never null once createPlatform() returns
    std::unique_ptr<InputPolicy> input;  // G7-input: never null once createPlatform() returns
    std::unique_ptr<Network> net;  // G7-net: never null once createPlatform() returns
    std::unique_ptr<Boot> boot;  // W6-boot: never null once createPlatform() returns
    std::unique_ptr<Capabilities> caps;  // W6-caps: never null once createPlatform() returns
    std::unique_ptr<Settings> settings;  // W6-caps: never null once createPlatform() returns
    std::unique_ptr<Emulators> emulators;  // W8-EMU: never null once createPlatform() returns
};

struct PlatformArgs {
    int argc = 0;
    char** argv = nullptr;
};

// Made by the linked platform library. Returns nullptr when the platform can't start (the reason
// is logged); main() then exits with an error.
std::unique_ptr<Platform> createPlatform(const PlatformArgs& args);

// Makes `platform` the process's platform. Call it once, from main(), before any Nano thread
// starts. Aborts with a message when `platform` is null or a platform is already installed.
void install(std::unique_ptr<Platform> platform);

// The installed platform; any thread. Never null: aborts with a message if install() was never
// called (which the rule above keeps out of the binaries that don't call it).
Platform& get();

}  // namespace nanoplat
