#include "NanoFrameFlag.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <gtest/gtest.h>

using android::FrameFlag;

namespace {

int64_t gFakeNow = 0;
int64_t fakeNow() { return gFakeNow; }

// The flag's state is process-wide: each test gets a fresh eventfd and leaves it disarmed.
class FrameFlagTest : public ::testing::Test {
protected:
    void SetUp() override {
        mFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        ASSERT_GE(mFd, 0);
        FrameFlag::sWakeFd = mFd;
        FrameFlag::rearm();
        FrameFlag::takeRenderWants();
        FrameFlag::tInRender = false;
    }
    void TearDown() override {
        FrameFlag::sWakeFd = -1;
        FrameFlag::rearm();
        FrameFlag::tInRender = false;
        FrameFlag::sNowMs = &FrameFlag::monotonicMs;
        close(mFd);
    }
    // The eventfd counter: how many writes reached it since the last drain.
    uint64_t drain() {
        uint64_t v = 0;
        return read(mFd, &v, sizeof(v)) == sizeof(v) ? v : 0;
    }
    int mFd = -1;
};

}  // namespace

TEST_F(FrameFlagTest, WakeBumpsTheSequenceAndWritesOnceUntilRearmed) {
    const uint32_t s0 = FrameFlag::seq();
    FrameFlag::wake();
    FrameFlag::wake();
    FrameFlag::wake();
    EXPECT_EQ(FrameFlag::seq(), s0 + 3);
    EXPECT_EQ(drain(), 1u);
    FrameFlag::wake();
    EXPECT_EQ(drain(), 0u);   // still armed
    FrameFlag::rearm();
    FrameFlag::wake();
    EXPECT_EQ(drain(), 1u);
    EXPECT_EQ(FrameFlag::seq(), s0 + 5);
}

TEST_F(FrameFlagTest, NudgeCountsWithoutBumpingTheSequence) {
    const uint32_t s0 = FrameFlag::seq(), n0 = FrameFlag::nudges();
    FrameFlag::nudge();
    FrameFlag::nudge();
    EXPECT_EQ(FrameFlag::seq(), s0);
    EXPECT_EQ(FrameFlag::nudges(), n0 + 2);
    EXPECT_EQ(drain(), 1u);
    // A nudge while armed is still counted, so the loop sees it after its wait.
    FrameFlag::nudge();
    EXPECT_EQ(FrameFlag::nudges(), n0 + 3);
    EXPECT_EQ(drain(), 0u);
}

TEST_F(FrameFlagTest, SettingDuringRenderOnlyLatchesRenderWants) {
    const uint32_t s0 = FrameFlag::seq();
    FrameFlag f;
    EXPECT_FALSE(f);
    FrameFlag::tInRender = true;
    f = true;
    EXPECT_TRUE(f);
    EXPECT_EQ(FrameFlag::seq(), s0);
    EXPECT_EQ(drain(), 0u);
    EXPECT_TRUE(FrameFlag::renderWants());
    EXPECT_TRUE(FrameFlag::takeRenderWants());
    EXPECT_FALSE(FrameFlag::takeRenderWants());
    FrameFlag::tInRender = false;
    f = false;   // clearing never wakes
    EXPECT_FALSE(f);
    EXPECT_EQ(FrameFlag::seq(), s0);
    EXPECT_EQ(drain(), 0u);
}

TEST_F(FrameFlagTest, WakeThrottledAllowsOneWakePer250Ms) {
    FrameFlag::sNowMs = &fakeNow;
    gFakeNow = 1000000;
    const uint32_t s0 = FrameFlag::seq();
    FrameFlag::wakeThrottled();
    EXPECT_EQ(FrameFlag::seq(), s0 + 1);
    gFakeNow += 249;
    FrameFlag::wakeThrottled();
    EXPECT_EQ(FrameFlag::seq(), s0 + 1);
    gFakeNow += 1;
    FrameFlag::wakeThrottled();
    EXPECT_EQ(FrameFlag::seq(), s0 + 2);
    FrameFlag::wakeThrottled();
    EXPECT_EQ(FrameFlag::seq(), s0 + 2);
    gFakeNow += 1000;
    FrameFlag::wakeThrottled();
    EXPECT_EQ(FrameFlag::seq(), s0 + 3);
}
