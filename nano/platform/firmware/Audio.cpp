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

// The firmware Audio (docs/linux/IMPLEMENTATION.md B.6, G7-audio): NanoAudio.cpp's AAudio output,
// moved here from NanoAudioPlayer and NanoSfxPlayer. One AAudio stream in callback mode per
// AudioStream: the stream builder of their openStreamLocked(), their requestStart, requestPause,
// requestStop and close calls. The callbacks reach the player through its Fill and OnDisconnect.
// Same LOG_TAG as NanoAudio.cpp, so the moved log lines read as before.

#define LOG_TAG "GammaOSNano"

#include <aaudio/AAudio.h>

#include <log/log.h>

#include <memory>
#include <utility>

#include "NanoPlatform.h"

namespace nanofirmware {

namespace {

static_assert(nanoplat::Audio::kFillContinue == AAUDIO_CALLBACK_RESULT_CONTINUE, "Fill results are AAudio's");
static_assert(nanoplat::Audio::kFillStop == AAUDIO_CALLBACK_RESULT_STOP, "Fill results are AAudio's");

class FirmwareAudioStream : public nanoplat::AudioStream {
 public:
    FirmwareAudioStream(nanoplat::Audio::Fill fill, nanoplat::Audio::OnDisconnect onDisconnect)
        : mFill(std::move(fill)), mOnDisconnect(std::move(onDisconnect)) {}

    ~FirmwareAudioStream() override {
        if (mStream) {
            AAudioStream* s = static_cast<AAudioStream*>(mStream);
            AAudioStream_requestStop(s);
            AAudioStream_close(s);
            mStream = nullptr;
        }
    }

    bool open(const nanoplat::AudioFormat& format);

    bool start() override {
        return AAudioStream_requestStart(static_cast<AAudioStream*>(mStream)) == AAUDIO_OK;
    }

    void pause() override {
        AAudioStream_requestPause(static_cast<AAudioStream*>(mStream));
    }

    int sampleRate() const override { return AAudioStream_getSampleRate(static_cast<AAudioStream*>(mStream)); }
    int channelCount() const override { return AAudioStream_getChannelCount(static_cast<AAudioStream*>(mStream)); }

 private:
    const nanoplat::Audio::Fill mFill;
    const nanoplat::Audio::OnDisconnect mOnDisconnect;
    void* mStream = nullptr;   // AAudioStream*, as the players kept it
};

// Build the AAudio output stream. Installs the data AND error callbacks; the error callback drives
// route-change recovery (the player's OnDisconnect reopens the stream).
bool FirmwareAudioStream::open(const nanoplat::AudioFormat& format) {
    const int rate = format.rate;
    const int channels = format.channels;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    if (format.lowLatency)
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    else
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(b, [](AAudioStream*, void* u, void* data, int32_t n) {
        return (aaudio_data_callback_result_t)
            static_cast<FirmwareAudioStream*>(u)->mFill(static_cast<int16_t*>(data), n);
    }, this);
    AAudioStreamBuilder_setErrorCallback(b, [](AAudioStream*, void* u, aaudio_result_t err) {
        FirmwareAudioStream* self = static_cast<FirmwareAudioStream*>(u);
        if (err != AAUDIO_ERROR_DISCONNECTED) return;
        self->mOnDisconnect();
    }, this);
    AAudioStream* s = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s) { ALOGW("%s: open stream failed (%d)", format.name, (int)r); return false; }
    mStream = s;
    return true;
}

class FirmwareAudio : public nanoplat::Audio {
 public:
    std::unique_ptr<nanoplat::AudioStream> open(const nanoplat::AudioFormat& format, Fill fill,
                                                OnDisconnect onDisconnect) override {
        std::unique_ptr<FirmwareAudioStream> stream(new FirmwareAudioStream(std::move(fill), std::move(onDisconnect)));
        if (!stream->open(format)) return nullptr;
        return std::unique_ptr<nanoplat::AudioStream>(stream.release());
    }
};

}  // namespace

std::unique_ptr<nanoplat::Audio> makeAudio() {
    return std::unique_ptr<nanoplat::Audio>(new FirmwareAudio);
}

}  // namespace nanofirmware
