#include <gtest/gtest.h>

#include "NanoScreenChoice.h"

using namespace nanoscreen;

TEST(ScreenChoice, ControllerScreenSetting) {
    EXPECT_EQ(ctrlMode("monitor"), CtrlMode::Monitor);
    EXPECT_EQ(ctrlMode(""), CtrlMode::Monitor);         // unset: the monitor keeps it
    EXPECT_EQ(ctrlMode("bogus"), CtrlMode::Monitor);
    EXPECT_EQ(ctrlMode("panel"), CtrlMode::Panel);
    EXPECT_EQ(ctrlMode("touched"), CtrlMode::Touched);
}

TEST(ScreenChoice, WhenTheControllerFollows) {
    EXPECT_FALSE(focusOnLaunch(CtrlMode::Monitor));
    EXPECT_FALSE(focusOnTouch(CtrlMode::Monitor));
    EXPECT_TRUE(focusOnLaunch(CtrlMode::Panel));
    EXPECT_TRUE(focusOnTouch(CtrlMode::Panel));
    EXPECT_FALSE(focusOnLaunch(CtrlMode::Touched));
    EXPECT_TRUE(focusOnTouch(CtrlMode::Touched));
}

TEST(ScreenChoice, WhereAnAppOpens) {
    EXPECT_FALSE(openOnPanel(false, false, false));   // nothing on the monitor: there
    EXPECT_TRUE(openOnPanel(false, false, true));     // a game there: beside it
    EXPECT_TRUE(openOnPanel(true, false, false));     // chosen for it
    EXPECT_FALSE(openOnPanel(false, true, true));
}

TEST(ScreenChoice, NoticeChangesEachTime) {
    EXPECT_EQ(noticeValue(1, 0), "1:0");
    EXPECT_EQ(noticeValue(2, 3), "2:3");
    EXPECT_NE(noticeValue(1, 3), noticeValue(2, 3));
}
