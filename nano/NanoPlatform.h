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
// InputPolicy, and the Cap set) is declared by the step that moves the code behind it, in the same
// commit as the move, sized to that code.
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

// The platform's services. Empty until the first G7 step declares one. Each platform library
// derives from it to keep its own state; services added later are members here.
struct Platform {
    virtual ~Platform();
    std::unique_ptr<Display> display;  // G7-display: never null once createPlatform() returns
    std::unique_ptr<Launcher> launcher;  // G7-launch-b: never null once createPlatform() returns
    std::unique_ptr<Audio> audio;  // G7-audio: set by every createPlatform(), never null after install()
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
