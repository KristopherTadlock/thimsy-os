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

// The host's fake display (platform/host/FakeDisplay.h) over a recording HostDrm: what it records,
// the hand-off's order and its {"ev":"display"} events.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/FakeDisplay.h"
#include "platform/host/HostControl.h"

using namespace nanohost;

namespace {

struct Log {
    std::vector<std::string> calls;
    bool active = false;
    bool takeBackOk = true;
    std::string joined() const {
        std::string s;
        for (const std::string& c : calls) s += (s.empty() ? "" : " ") + c;
        return s;
    }
};

class RecordingDrm : public HostDrm {
 public:
    explicit RecordingDrm(Log* log) : log_(log) {}
    void earlySplash(int fd) override {
        log_->calls.push_back("splash(" + std::to_string(fd) + ")");
        log_->active = true;
    }
    bool active() const override { return log_->active; }
    int width() const override { return 640; }
    int height() const override { return 480; }
    int rotationDeg() const override { return 90; }
    void waitForFlips() override { log_->calls.push_back("flips"); }
    void rememberInstallMatrix() override { log_->calls.push_back("matrix"); }
    void stop() override {
        log_->calls.push_back("stop");
        log_->active = false;
    }
    bool takeBack(int timeoutMs) override {
        log_->calls.push_back("takeBack(" + std::to_string(timeoutMs) + ")");
        if (log_->takeBackOk) log_->active = true;
        return log_->takeBackOk;
    }

 private:
    Log* log_;
};

bool alwaysSf() { return true; }

TEST(FakeDisplay, NeedsAHostDrm) { EXPECT_TRUE(createFakeDisplay(nullptr) == nullptr); }

TEST(FakeDisplay, RecordsAndHandsOff) {
    headless_test::TmpDir dir;
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    Log log;
    std::unique_ptr<nanoplat::Display> d = createFakeDisplay(std::unique_ptr<HostDrm>(new RecordingDrm(&log)));
    ASSERT_TRUE(d != nullptr);
    EXPECT_EQ(displayRecord().starts, 0);

    // The panel is always taken, as on Linux, whatever the firmware's SurfaceFlinger rule says.
    EXPECT_EQ(d->startScanout(5, true, alwaysSf), -1);
    DisplayRecord r = displayRecord();
    EXPECT_EQ(r.starts, 1);
    EXPECT_EQ(r.width, 640);
    EXPECT_EQ(r.height, 480);
    EXPECT_TRUE(d->reacquire());  // not released: the panel is Nano's

    ASSERT_TRUE(d->releaseForChild());
    EXPECT_FALSE(d->releaseForChild());
    log.takeBackOk = false;
    EXPECT_FALSE(d->reacquire());
    log.takeBackOk = true;
    EXPECT_TRUE(d->reacquire());
    EXPECT_EQ(log.joined(), "splash(5) flips matrix stop takeBack(8000) takeBack(8000)");
    r = displayRecord();
    EXPECT_EQ(r.releases, 1);
    EXPECT_EQ(r.reacquires, 2);

    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("\"ev\":\"display\""), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"start\",\"w\":640,\"h\":480,\"rotation\":90"), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"release\""), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"reacquire\",\"ok\":false"), std::string::npos);
    EXPECT_NE(text.find("\"op\":\"reacquire\",\"ok\":true"), std::string::npos);
    ASSERT_TRUE(openEvents(""));
}

TEST(FakeDisplay, NoDisplayRecordsZeroSize) {
    Log log;
    class Empty : public RecordingDrm {
     public:
        using RecordingDrm::RecordingDrm;
        void earlySplash(int) override {}
    };
    std::unique_ptr<nanoplat::Display> d = createFakeDisplay(std::unique_ptr<HostDrm>(new Empty(&log)));
    EXPECT_EQ(d->startScanout(-1, false, nullptr), -1);
    EXPECT_EQ(displayRecord().width, 0);
    EXPECT_FALSE(d->releaseForChild());
    EXPECT_EQ(log.joined(), "");
}

}  // namespace
