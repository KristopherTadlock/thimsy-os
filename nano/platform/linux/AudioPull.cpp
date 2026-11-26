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

// PullAudio (AudioPull.h): one thread per stream. While started it calls Fill for one period and
// writes the period to the sink; when paused, or when Fill returns kFillStop, it drops what the
// sink holds and waits. A failed write means the device is gone: the stream calls OnDisconnect
// once, on its thread, and never pulls again (start() then fails), as an AAudio stream does after
// AAUDIO_ERROR_DISCONNECTED.

#define LOG_TAG "GammaOSNano"

#include "AudioPull.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <log/log.h>

namespace nanolinux {

namespace {

class PullStream : public nanoplat::AudioStream {
 public:
    PullStream(const nanoplat::AudioFormat& format, std::unique_ptr<PcmSink> sink, nanoplat::Audio::Fill fill,
               nanoplat::Audio::OnDisconnect onDisconnect)
        : mName(format.name ? format.name : ""),
          mRate(format.rate),
          mChannels(format.channels),
          mSink(std::move(sink)),
          mFill(std::move(fill)),
          mOnDisconnect(std::move(onDisconnect)) {
        mThread = std::thread(&PullStream::run, this);
    }

    ~PullStream() override {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mQuit = true;
        }
        mCv.notify_all();
        mThread.join();
    }

    bool start() override {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mDisconnected) return false;
        mRunning = true;
        mCv.notify_all();
        return true;
    }

    // Returns once no Fill is running, so the caller may change what Fill reads (AAudio's
    // requestPause only asks; waiting here is stricter, never looser).
    void pause() override {
        std::unique_lock<std::mutex> lk(mMutex);
        mRunning = false;
        mCv.notify_all();
        mCv.wait(lk, [this] { return !mInFill; });
    }

    int sampleRate() const override { return mRate; }
    int channelCount() const override { return mChannels; }

 private:
    void run();

    const std::string mName;
    const int mRate;
    const int mChannels;
    const std::unique_ptr<PcmSink> mSink;
    const nanoplat::Audio::Fill mFill;
    const nanoplat::Audio::OnDisconnect mOnDisconnect;

    std::mutex mMutex;
    std::condition_variable mCv;
    bool mRunning = false;       // started and not paused or stopped by Fill
    bool mInFill = false;        // the thread is inside Fill
    bool mDisconnected = false;  // a write failed: the device is gone
    bool mQuit = false;          // the destructor asks the thread to end
    std::thread mThread;         // last: started once everything above is set
};

void PullStream::run() {
    const int period = std::max(1, mSink->periodFrames());
    std::vector<int16_t> buf((size_t)period * (size_t)std::max(1, mChannels));
    bool queued = false;  // the sink holds frames written since the last stop()
    std::unique_lock<std::mutex> lk(mMutex);
    for (;;) {
        mCv.wait(lk, [this, &queued] { return mQuit || (mRunning && !mDisconnected) || (queued && !mRunning); });
        if (mQuit) break;
        if (!mRunning) {  // paused, or Fill said stop: drop what the sink still holds
            queued = false;
            lk.unlock();
            mSink->stop();
            lk.lock();
            continue;
        }
        mInFill = true;
        lk.unlock();
        std::fill(buf.begin(), buf.end(), 0);
        const int result = mFill ? mFill(buf.data(), period) : nanoplat::Audio::kFillStop;
        lk.lock();
        mInFill = false;
        mCv.notify_all();
        if (result == nanoplat::Audio::kFillStop) {  // these frames are not played (as AAudio's STOP)
            mRunning = false;
            continue;
        }
        if (!mRunning || mQuit) continue;  // paused while filling: the period is dropped
        lk.unlock();
        const bool ok = mSink->write(buf.data(), period);
        lk.lock();
        if (ok) {
            queued = true;
            continue;
        }
        mDisconnected = true;
        mRunning = false;
        queued = false;
        lk.unlock();
        ALOGW("%s: output device gone", mName.c_str());
        if (mOnDisconnect) mOnDisconnect();
        lk.lock();
    }
}

}  // namespace

PullAudio::PullAudio(SinkFactory factory) : mFactory(std::move(factory)) {}

std::unique_ptr<nanoplat::AudioStream> PullAudio::open(const nanoplat::AudioFormat& format, Fill fill,
                                                       OnDisconnect onDisconnect) {
    const char* name = format.name ? format.name : "";
    if (format.rate <= 0 || format.channels <= 0) {
        ALOGW("%s: open stream failed (%d)", name, -22);
        return nullptr;
    }
    int error = 0;
    std::unique_ptr<PcmSink> sink = mFactory ? mFactory(format, &error) : nullptr;
    if (!sink) {
        ALOGW("%s: open stream failed (%d)", name, error);
        return nullptr;
    }
    return std::unique_ptr<nanoplat::AudioStream>(
        new PullStream(format, std::move(sink), std::move(fill), std::move(onDisconnect)));
}

}  // namespace nanolinux
