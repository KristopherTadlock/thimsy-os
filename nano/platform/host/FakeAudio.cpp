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

// FakeAudio (FakeAudio.h): the host's output streams. Same stream rules as the firmware's and the
// Linux ones: opened paused; Fill runs only while started; pause() returns with no Fill running;
// kFillStop stops until start(); a disconnect calls OnDisconnect once, and start() then fails.

#define LOG_TAG "NanoHost"

#include "FakeAudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <log/log.h>

namespace nanohost {

class FakeStream;

struct FakeAudio::Shared {
    std::atomic<int64_t> frames{0};
    std::atomic<int> opened{0};
    std::mutex m;                 // guards live
    std::set<FakeStream*> live;
};

class FakeStream : public nanoplat::AudioStream {
 public:
    FakeStream(std::shared_ptr<FakeAudio::Shared> shared, const nanoplat::AudioFormat& format, int period,
               bool paced, nanoplat::Audio::Fill fill, nanoplat::Audio::OnDisconnect onDisconnect)
        : mShared(std::move(shared)),
          mRate(format.rate),
          mChannels(format.channels),
          mPeriod(std::max(1, period)),
          mPaced(paced),
          mFill(std::move(fill)),
          mOnDisconnect(std::move(onDisconnect)) {
        {
            std::lock_guard<std::mutex> lk(mShared->m);
            mShared->live.insert(this);
        }
        mThread = std::thread(&FakeStream::run, this);
    }

    ~FakeStream() override {
        {
            std::lock_guard<std::mutex> lk(mShared->m);
            mShared->live.erase(this);
        }
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mQuit = true;
        }
        mCv.notify_all();
        mThread.join();
    }

    bool start() override {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mDisconnected || mDisconnectAsked) return false;
        mRunning = true;
        mCv.notify_all();
        return true;
    }

    void pause() override {
        std::unique_lock<std::mutex> lk(mMutex);
        mRunning = false;
        mCv.notify_all();
        mCv.wait(lk, [this] { return !mInFill; });
    }

    int sampleRate() const override { return mRate; }
    int channelCount() const override { return mChannels; }

    void disconnect() {  // FakeAudio::disconnectAll(), with Shared::m held
        std::lock_guard<std::mutex> lk(mMutex);
        mDisconnectAsked = true;
        mCv.notify_all();
    }

 private:
    void run() {
        std::vector<int16_t> buf((size_t)mPeriod * (size_t)std::max(1, mChannels));
        const auto periodTime = std::chrono::microseconds(mRate > 0 ? (int64_t)mPeriod * 1000000 / mRate : 1000);
        auto next = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(mMutex);
        for (;;) {
            mCv.wait(lk, [this] { return mQuit || mDisconnectAsked || (mRunning && !mDisconnected); });
            if (mQuit) break;
            if (mDisconnectAsked && !mDisconnected) {
                mDisconnected = true;
                mRunning = false;
                lk.unlock();
                ALOGI("fake audio: disconnected (as a route change)");
                if (mOnDisconnect) mOnDisconnect();
                lk.lock();
                continue;
            }
            if (mDisconnected) {  // dead: wait for the destructor
                mCv.wait(lk, [this] { return mQuit; });
                break;
            }
            mInFill = true;
            lk.unlock();
            std::fill(buf.begin(), buf.end(), 0);
            const int result = mFill ? mFill(buf.data(), mPeriod) : nanoplat::Audio::kFillStop;
            lk.lock();
            mInFill = false;
            mCv.notify_all();
            if (result == nanoplat::Audio::kFillStop) {
                mRunning = false;
                continue;
            }
            mShared->frames += mPeriod;
            if (!mPaced) {
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
                continue;
            }
            // The period "plays" for its length; a pause, a disconnect or the destructor cuts it short.
            next += periodTime;
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            mCv.wait_until(lk, next, [this] { return mQuit || mDisconnectAsked || !mRunning; });
        }
    }

    const std::shared_ptr<FakeAudio::Shared> mShared;
    const int mRate;
    const int mChannels;
    const int mPeriod;
    const bool mPaced;
    const nanoplat::Audio::Fill mFill;
    const nanoplat::Audio::OnDisconnect mOnDisconnect;

    std::mutex mMutex;
    std::condition_variable mCv;
    bool mRunning = false;
    bool mInFill = false;
    bool mDisconnectAsked = false;
    bool mDisconnected = false;
    bool mQuit = false;
    std::thread mThread;  // last: started once everything above is set
};

FakeAudio::FakeAudio(int periodFrames, bool paced)
    : mPeriod(periodFrames), mPaced(paced), mShared(std::make_shared<Shared>()) {}

FakeAudio::~FakeAudio() = default;

std::unique_ptr<nanoplat::AudioStream> FakeAudio::open(const nanoplat::AudioFormat& format, Fill fill,
                                                       OnDisconnect onDisconnect) {
    const char* name = format.name ? format.name : "";
    if (format.rate <= 0 || format.channels <= 0) {
        ALOGW("%s: open stream failed (%d)", name, -22);
        return nullptr;
    }
    mShared->opened++;
    ALOGI("fake audio: %s open %dHz x%d", name, format.rate, format.channels);
    return std::unique_ptr<nanoplat::AudioStream>(
        new FakeStream(mShared, format, mPeriod, mPaced, std::move(fill), std::move(onDisconnect)));
}

int64_t FakeAudio::framesPulled() const { return mShared->frames.load(); }
int FakeAudio::streamsOpened() const { return mShared->opened.load(); }

int FakeAudio::streamsLive() const {
    std::lock_guard<std::mutex> lk(mShared->m);
    return (int)mShared->live.size();
}

void FakeAudio::disconnectAll() {
    std::lock_guard<std::mutex> lk(mShared->m);
    for (FakeStream* s : mShared->live) s->disconnect();
}

}  // namespace nanohost
