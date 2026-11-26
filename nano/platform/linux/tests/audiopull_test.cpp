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

// PullAudio (AudioPull.cpp), the Linux Audio's stream threads, on a fake sink: pulls only while
// started, pause() returns with no Fill running, kFillStop stops until start(), a failed write
// disconnects once on the stream's thread, and a stream ends its thread when destroyed.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "AudioPull.h"

namespace {

using namespace std::chrono_literals;

struct SinkLog {
    std::mutex m;
    std::vector<int16_t> first;    // the first period written
    std::atomic<int> writes{0};
    std::atomic<int> frames{0};
    std::atomic<int> stops{0};
    std::atomic<int> failAfter{-1};  // writes that succeed before every write fails (-1: never)
    std::atomic<int> sinks{0};       // sinks made
    std::atomic<int> sinksLive{0};
};

class FakeSink : public nanolinux::PcmSink {
 public:
    FakeSink(SinkLog* log, int period, int channels) : mLog(log), mPeriod(period), mChannels(channels) {
        mLog->sinks++;
        mLog->sinksLive++;
    }
    ~FakeSink() override { mLog->sinksLive--; }
    int periodFrames() const override { return mPeriod; }
    bool write(const int16_t* frames, int count) override {
        std::this_thread::sleep_for(1ms);  // a device that plays
        const int fail = mLog->failAfter.load();
        if (fail >= 0 && mLog->writes.load() >= fail) return false;
        {
            std::lock_guard<std::mutex> lk(mLog->m);
            if (mLog->first.empty()) mLog->first.assign(frames, frames + (size_t)count * (size_t)mChannels);
        }
        mLog->frames += count;
        mLog->writes++;
        return true;
    }
    void stop() override { mLog->stops++; }

 private:
    SinkLog* const mLog;
    const int mPeriod;
    const int mChannels;
};

nanolinux::PullAudio makeTestAudio(SinkLog* log, int period) {
    return nanolinux::PullAudio([log, period](const nanoplat::AudioFormat& f, int*) {
        return std::unique_ptr<nanolinux::PcmSink>(new FakeSink(log, period, f.channels));
    });
}

nanoplat::AudioFormat format(int rate, int channels) {
    nanoplat::AudioFormat f;
    f.name = "Test";
    f.rate = rate;
    f.channels = channels;
    return f;
}

// Waits up to 2 s for `pred`.
template <typename P>
bool eventually(P pred) {
    for (int i = 0; i < 2000; i++) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

TEST(PullAudio, OpenFailsWhenTheSinkDoes) {
    nanolinux::PullAudio audio([](const nanoplat::AudioFormat&, int* error) {
        *error = -16;
        return std::unique_ptr<nanolinux::PcmSink>();
    });
    EXPECT_TRUE(audio.open(format(48000, 2), nullptr, nullptr) == nullptr);
}

TEST(PullAudio, OpenRefusesABadFormat) {
    SinkLog log;
    nanolinux::PullAudio audio = makeTestAudio(&log, 64);
    EXPECT_TRUE(audio.open(format(0, 2), nullptr, nullptr) == nullptr);
    EXPECT_TRUE(audio.open(format(48000, 0), nullptr, nullptr) == nullptr);
    EXPECT_EQ(log.sinks.load(), 0);
}

TEST(PullAudio, PullsOnlyWhileStarted) {
    SinkLog log;
    nanolinux::PullAudio audio = makeTestAudio(&log, 64);
    std::atomic<int> calls{0};
    std::atomic<int> badCount{0};
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(44100, 2), [&](int16_t* frames, int count) {
        if (count != 64) badCount++;
        for (int i = 0; i < count * 2; i++) frames[i] = (int16_t)(i + 1);
        calls++;
        return nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->sampleRate(), 44100);
    EXPECT_EQ(s->channelCount(), 2);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 0);  // opened paused, as an AAudio stream before requestStart

    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return log.writes.load() >= 5; }));
    EXPECT_EQ(badCount.load(), 0);
    EXPECT_EQ(log.frames.load() % 64, 0);
    {
        std::lock_guard<std::mutex> lk(log.m);
        ASSERT_EQ(log.first.size(), 128u);
        EXPECT_EQ(log.first[0], 1);
        EXPECT_EQ(log.first[127], 128);
    }

    s->pause();
    const int atPause = calls.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), atPause);  // no Fill after pause() returned
    EXPECT_TRUE(eventually([&] { return log.stops.load() >= 1; }));  // what was queued is dropped

    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() > atPause; }));
}

TEST(PullAudio, FillStopStopsUntilStart) {
    SinkLog log;
    nanolinux::PullAudio audio = makeTestAudio(&log, 32);
    std::atomic<int> calls{0};
    std::atomic<int> stopAt{3};
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 2), [&](int16_t*, int) {
        const int n = ++calls;
        return n == stopAt.load() ? nanoplat::Audio::kFillStop : nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 3; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(log.writes.load(), 2);  // the stopping call's frames are not played
    EXPECT_TRUE(eventually([&] { return log.stops.load() >= 1; }));

    stopAt = 6;
    EXPECT_TRUE(s->start());  // the UI sounds restart a stream that stopped itself
    EXPECT_TRUE(eventually([&] { return calls.load() >= 6; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 6);
}

TEST(PullAudio, AFailedWriteDisconnectsOnce) {
    SinkLog log;
    log.failAfter = 2;
    nanolinux::PullAudio audio = makeTestAudio(&log, 16);
    std::atomic<int> disconnects{0};
    std::atomic<bool> onOtherThread{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 1), [](int16_t*, int) {
        return nanoplat::Audio::kFillContinue;
    }, [&] {
        onOtherThread = std::this_thread::get_id() != caller;
        disconnects++;
    });
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return disconnects.load() == 1; }));
    EXPECT_TRUE(onOtherThread.load());
    EXPECT_EQ(log.writes.load(), 2);
    EXPECT_FALSE(s->start());  // dead until reopened
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(disconnects.load(), 1);
}

TEST(PullAudio, ReopenFromAnotherThreadAfterADisconnect) {
    // NanoAudioPlayer's recovery: OnDisconnect starts a thread that destroys the stream and opens
    // a new one, while the old stream's thread returns from OnDisconnect.
    SinkLog log;
    log.failAfter = 1;
    nanolinux::PullAudio audio = makeTestAudio(&log, 16);
    std::mutex m;
    std::unique_ptr<nanoplat::AudioStream> s;
    std::atomic<int> reopened{0};
    std::atomic<bool> done{false};
    auto fill = [](int16_t*, int) { return nanoplat::Audio::kFillContinue; };
    {
        std::lock_guard<std::mutex> lk(m);
        s = audio.open(format(48000, 2), fill, [&] {
            std::thread([&] {
                {
                    std::lock_guard<std::mutex> lk2(m);
                    log.failAfter = -1;
                    s.reset();
                    s = audio.open(format(48000, 2), fill, nullptr);
                    if (s && s->start()) reopened++;
                }
                done = true;
            }).detach();
        });
        ASSERT_TRUE(s != nullptr);
        EXPECT_TRUE(s->start());
    }
    ASSERT_TRUE(eventually([&] { return done.load(); }));
    EXPECT_EQ(reopened.load(), 1);
    const int before = log.writes.load();
    EXPECT_TRUE(eventually([&] { return log.writes.load() > before; }));
    std::lock_guard<std::mutex> lk(m);
    s.reset();
    EXPECT_EQ(log.sinks.load(), 2);
    EXPECT_EQ(log.sinksLive.load(), 0);
}

TEST(PullAudio, DestroyingARunningStreamEndsItsThread) {
    SinkLog log;
    nanolinux::PullAudio audio = makeTestAudio(&log, 8);
    std::atomic<int> calls{0};
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 2), [&](int16_t*, int) {
        calls++;
        return nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 3; }));
    s.reset();
    const int atClose = calls.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), atClose);
    EXPECT_EQ(log.sinksLive.load(), 0);
}

TEST(PullAudio, StreamsRunSideBySide) {
    SinkLog a, b;
    nanolinux::PullAudio audio([&](const nanoplat::AudioFormat& f, int*) {
        SinkLog* log = f.lowLatency ? &b : &a;
        return std::unique_ptr<nanolinux::PcmSink>(new FakeSink(log, 32, f.channels));
    });
    nanoplat::AudioFormat music = format(44100, 2);
    nanoplat::AudioFormat sfx = format(48000, 2);
    sfx.lowLatency = true;
    auto fill = [](int16_t*, int) { return nanoplat::Audio::kFillContinue; };
    std::unique_ptr<nanoplat::AudioStream> m = audio.open(music, fill, nullptr);
    std::unique_ptr<nanoplat::AudioStream> x = audio.open(sfx, fill, nullptr);
    ASSERT_TRUE(m != nullptr && x != nullptr);
    EXPECT_TRUE(m->start());
    EXPECT_TRUE(x->start());
    EXPECT_TRUE(eventually([&] { return a.writes.load() >= 3 && b.writes.load() >= 3; }));
}

}  // namespace
