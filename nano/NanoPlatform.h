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
#include <cstdint>     // Audio (G7-audio)
#include <functional>  // Audio (G7-audio)

namespace nanoplat {

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
