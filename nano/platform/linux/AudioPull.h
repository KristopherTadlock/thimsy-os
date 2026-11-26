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

// The Linux Audio (docs/linux/IMPLEMENTATION.md B.6, G7-audio): NanoPlatform.h's pull model over a
// blocking PCM output. Each stream has its own thread that, while started, calls the player's Fill
// for one period and writes it to its sink. AudioAlsa.cpp gives the sink, ALSA's "default" PCM, one
// per stream: Nano keeps several open at once (music and the UI sounds), so "default" must mix
// (dmix, B.6). The tests give a fake.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "NanoPlatform.h"

namespace nanolinux {

// A blocking PCM output, used by one stream's thread only.
class PcmSink {
 public:
    virtual ~PcmSink() = default;
    virtual int periodFrames() const = 0;   // frames per Fill call (at least 1 is used)
    // Plays `count` frames, blocking while the output is full. False: the device is gone.
    virtual bool write(const int16_t* frames, int count) = 0;
    // Drops what is queued (on pause, or when the Fill stopped); the next write starts again.
    virtual void stop() = 0;
};

// Opens a sink for `format`. Null on failure, with *error set to a negative errno.
using SinkFactory = std::function<std::unique_ptr<PcmSink>(const nanoplat::AudioFormat& format, int* error)>;

class PullAudio : public nanoplat::Audio {
 public:
    explicit PullAudio(SinkFactory factory);
    // Logs "<name>: open stream failed (<error>)" when the sink can't be opened, as the firmware does.
    std::unique_ptr<nanoplat::AudioStream> open(const nanoplat::AudioFormat& format, Fill fill,
                                                OnDisconnect onDisconnect) override;

 private:
    const SinkFactory mFactory;
};

// The Linux platform's Audio: PullAudio on ALSA's "default" PCM (AudioAlsa.cpp).
std::unique_ptr<nanoplat::Audio> makeAudio();

}  // namespace nanolinux
