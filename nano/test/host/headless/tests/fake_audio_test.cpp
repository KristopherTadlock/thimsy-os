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

// The host platform's FakeAudio (platform/host/FakeAudio.cpp): it pulls and counts frames while a
// stream is started, paced at the stream's rate; pause, kFillStop, a disconnect and closing behave
// as the firmware's AAudio streams do.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "NanoPlatform.h"
#include "platform/host/FakeAudio.h"

using nanohost::FakeAudio;

namespace {

using namespace std::chrono_literals;

nanoplat::AudioFormat format(int rate, int channels) {
    nanoplat::AudioFormat f;
    f.name = "Test";
    f.rate = rate;
    f.channels = channels;
    return f;
}

template <typename P>
bool eventually(P pred) {
    for (int i = 0; i < 3000; i++) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

TEST(FakeAudio, PullsAndCountsWhileStarted) {
    FakeAudio audio(64, false);
    std::atomic<int> calls{0};
    std::atomic<int> wrongCount{0};
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 2), [&](int16_t* frames, int count) {
        if (count != 64) wrongCount++;
        frames[count * 2 - 1] = 1;  // the whole period is writable
        calls++;
        return nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(audio.streamsOpened(), 1);
    EXPECT_EQ(audio.streamsLive(), 1);
    EXPECT_EQ(s->sampleRate(), 48000);
    EXPECT_EQ(s->channelCount(), 2);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(audio.framesPulled(), 0);

    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 10; }));
    s->pause();
    const int atPause = calls.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), atPause);
    EXPECT_EQ(audio.framesPulled(), (int64_t)atPause * 64);
    EXPECT_EQ(wrongCount.load(), 0);

    s.reset();
    EXPECT_EQ(audio.streamsLive(), 0);
}

TEST(FakeAudio, PacedAtTheStreamRate) {
    FakeAudio audio(480, true);  // 10 ms periods at 48 kHz
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 2), [](int16_t*, int) {
        return nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(s->start());
    std::this_thread::sleep_for(200ms);
    s->pause();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double seconds = (double)audio.framesPulled() / 48000.0;
    EXPECT_GT(seconds, 0.05);        // it ran
    EXPECT_LE(seconds, secs + 0.02);  // and never ahead of real time (one period of slack)
}

TEST(FakeAudio, FillStopStopsUntilStart) {
    FakeAudio audio(32, false);
    std::atomic<int> calls{0};
    std::unique_ptr<nanoplat::AudioStream> s = audio.open(format(48000, 2), [&](int16_t*, int) {
        const int n = ++calls;
        return (n == 2 || n == 4) ? nanoplat::Audio::kFillStop : nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(s != nullptr);
    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 2; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(audio.framesPulled(), 32);  // a stopping Fill's frames are not counted
    EXPECT_TRUE(s->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 4; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(calls.load(), 4);
    EXPECT_EQ(audio.framesPulled(), 64);
}

TEST(FakeAudio, DisconnectAllCallsEachLiveStreamOnce) {
    FakeAudio audio(32, true);
    std::atomic<int> a{0}, b{0};
    auto fill = [](int16_t*, int) { return nanoplat::Audio::kFillContinue; };
    std::unique_ptr<nanoplat::AudioStream> sa = audio.open(format(44100, 2), fill, [&] { a++; });
    std::unique_ptr<nanoplat::AudioStream> sb = audio.open(format(48000, 1), fill, [&] { b++; });
    ASSERT_TRUE(sa != nullptr && sb != nullptr);
    EXPECT_TRUE(sa->start());  // one playing, one paused: both are told
    audio.disconnectAll();
    EXPECT_TRUE(eventually([&] { return a.load() == 1 && b.load() == 1; }));
    EXPECT_FALSE(sa->start());
    EXPECT_FALSE(sb->start());
    audio.disconnectAll();  // already dead: not told again
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(a.load(), 1);
    EXPECT_EQ(b.load(), 1);

    // The player's recovery: a fresh stream on the "new device" plays.
    sa.reset();
    std::atomic<int> calls{0};
    sa = audio.open(format(44100, 2), [&](int16_t*, int) {
        calls++;
        return nanoplat::Audio::kFillContinue;
    }, nullptr);
    ASSERT_TRUE(sa != nullptr);
    EXPECT_TRUE(sa->start());
    EXPECT_TRUE(eventually([&] { return calls.load() >= 2; }));
    EXPECT_EQ(audio.streamsOpened(), 3);
    EXPECT_EQ(audio.streamsLive(), 2);
}

TEST(FakeAudio, RefusesABadFormat) {
    FakeAudio audio;
    EXPECT_TRUE(audio.open(format(0, 2), nullptr, nullptr) == nullptr);
    EXPECT_TRUE(audio.open(format(48000, 0), nullptr, nullptr) == nullptr);
    EXPECT_EQ(audio.streamsOpened(), 0);
}

}  // namespace
