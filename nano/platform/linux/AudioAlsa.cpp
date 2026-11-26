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

// The Linux platform's Audio sink (docs/linux/IMPLEMENTATION.md B.6, G7-audio): ALSA's "default"
// PCM through alsa-lib, blocking, S16 interleaved, one PCM per stream; PullAudio (AudioPull.cpp)
// runs the stream's thread. "default" has to mix, as Nano keeps the music and the UI sounds open at
// once (dmix). alsa-lib resamples when the device can't take the rate, so the stream reports the
// rate asked for. Needs alsa-lib's headers (nano::alsa, C.2): not built by platform.mk's tests.

#define LOG_TAG "GammaOSNano"

#include <errno.h>

#include <alsa/asoundlib.h>

#include <memory>

#include <log/log.h>

#include "AudioPull.h"

namespace nanolinux {

namespace {

constexpr const char* kDevice = "default";
// Latency asked of alsa-lib (snd_pcm_set_params): the UI sounds want short, music steady.
constexpr unsigned kLowLatencyUs = 20000;
constexpr unsigned kLatencyUs = 100000;

class AlsaSink : public PcmSink {
 public:
    ~AlsaSink() override {
        if (mPcm) snd_pcm_close(mPcm);
    }

    // 0, or a negative errno from alsa-lib.
    int open(const nanoplat::AudioFormat& format) {
        int err = snd_pcm_open(&mPcm, kDevice, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            mPcm = nullptr;
            return err;
        }
        err = snd_pcm_set_params(mPcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 (unsigned)format.channels, (unsigned)format.rate, 1 /* soft resample */,
                                 format.lowLatency ? kLowLatencyUs : kLatencyUs);
        if (err < 0) return err;
        snd_pcm_uframes_t buffer = 0, period = 0;
        if (snd_pcm_get_params(mPcm, &buffer, &period) == 0 && period > 0) mPeriod = (int)period;
        mChannels = format.channels;
        return 0;
    }

    int periodFrames() const override { return mPeriod; }

    bool write(const int16_t* frames, int count) override {
        while (count > 0) {
            snd_pcm_sframes_t n = snd_pcm_writei(mPcm, frames, (snd_pcm_uframes_t)count);
            if (n < 0) {
                // An underrun (-EPIPE), a suspend (-ESTRPIPE) or a signal (-EINTR) is recovered; what
                // snd_pcm_recover hands back (-ENODEV, -EBADFD, -EIO) means the device is gone.
                if (snd_pcm_recover(mPcm, (int)n, 1 /* silent */) < 0) {
                    ALOGW("ALSA %s: write: %s", kDevice, snd_strerror((int)n));
                    return false;
                }
                continue;
            }
            frames += (size_t)n * (size_t)mChannels;
            count -= (int)n;
        }
        return true;
    }

    void stop() override {
        snd_pcm_drop(mPcm);
        snd_pcm_prepare(mPcm);
    }

 private:
    snd_pcm_t* mPcm = nullptr;
    int mPeriod = 1024;
    int mChannels = 2;
};

}  // namespace

std::unique_ptr<nanoplat::Audio> makeAudio() {
    return std::unique_ptr<nanoplat::Audio>(new PullAudio(
        [](const nanoplat::AudioFormat& format, int* error) -> std::unique_ptr<PcmSink> {
            std::unique_ptr<AlsaSink> sink(new AlsaSink);
            const int err = sink->open(format);
            if (err < 0) {
                ALOGW("%s: ALSA %s: %s", format.name ? format.name : "", kDevice, snd_strerror(err));
                if (error) *error = err;
                return nullptr;
            }
            return std::unique_ptr<PcmSink>(sink.release());
        }));
}

}  // namespace nanolinux
