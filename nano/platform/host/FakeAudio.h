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

// The host platform's Audio (docs/linux/IMPLEMENTATION.md B.6, G7-audio): no sound. Each stream has
// a thread that, while started, pulls one period from the player's Fill (at the stream's rate in
// real time, when paced, so Nano's audio clock runs as on a device) and counts the frames.
// disconnectAll() acts as a headphone plug: every live stream calls OnDisconnect once and dies.
#pragma once

#include <cstdint>
#include <memory>

#include "NanoPlatform.h"

namespace nanohost {

class FakeAudio : public nanoplat::Audio {
 public:
    struct Shared;
    explicit FakeAudio(int periodFrames = 256, bool paced = true);
    ~FakeAudio() override;

    std::unique_ptr<nanoplat::AudioStream> open(const nanoplat::AudioFormat& format, Fill fill,
                                                OnDisconnect onDisconnect) override;

    int64_t framesPulled() const;  // by every stream, from Fills that returned kFillContinue
    int streamsOpened() const;
    int streamsLive() const;       // opened and not yet destroyed
    void disconnectAll();

 private:
    const int mPeriod;
    const bool mPaced;
    std::shared_ptr<Shared> mShared;
};

}  // namespace nanohost
