// drastic-nano's DS screen at 4:3 in each window of a two-window session (DsScreenLayout fitOne):
// the RG DS's 4:3 panels, and in dual screen mode a monitor and the RG Cube's square panel.

#include <gtest/gtest.h>

#include "DsScreenLayout.h"

using drastic_nano::Rect;
using drastic_nano::fitOne;

TEST(DsScreenLayout, FitOneFillsA43Panel) {
    const Rect r = fitOne(640, 480);
    EXPECT_FLOAT_EQ(0.0f, r.x);
    EXPECT_FLOAT_EQ(0.0f, r.y);
    EXPECT_FLOAT_EQ(640.0f, r.w);
    EXPECT_FLOAT_EQ(480.0f, r.h);
}

TEST(DsScreenLayout, FitOneBarsASquarePanel) {
    const Rect r = fitOne(720, 720);
    EXPECT_FLOAT_EQ(0.0f, r.x);
    EXPECT_FLOAT_EQ(90.0f, r.y);
    EXPECT_FLOAT_EQ(720.0f, r.w);
    EXPECT_FLOAT_EQ(540.0f, r.h);
}

TEST(DsScreenLayout, FitOnePillarboxesAMonitor) {
    const Rect r = fitOne(1920, 1080);
    EXPECT_FLOAT_EQ(240.0f, r.x);
    EXPECT_FLOAT_EQ(0.0f, r.y);
    EXPECT_FLOAT_EQ(1440.0f, r.w);
    EXPECT_FLOAT_EQ(1080.0f, r.h);
}

TEST(DsScreenLayout, FitOneNoSurface) {
    const Rect r = fitOne(0, 0);
    EXPECT_LE(r.w, 0.0f);
}
