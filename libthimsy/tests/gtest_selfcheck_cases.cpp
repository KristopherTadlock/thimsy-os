// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Not a test suite: a program with deliberate failures, run by gtest_standin_test.cpp to check
// the stand-in's output, exit status, filters and fixture order.
#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>

TEST(A, Passes) {
    EXPECT_EQ(1, 1);
    ASSERT_TRUE(true) << "x";
}

TEST(A, Fails) {
    ASSERT_EQ(1, 2) << "one";
    EXPECT_TRUE(false);
}

TEST(A, Expects) {
    EXPECT_STREQ("a", "b");
    EXPECT_STREQ(nullptr, nullptr);
}

namespace {
std::string order;
}

class Fix : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { order += "[suite"; }
    static void TearDownTestSuite() {
        order += "suite]";
        printf("order: %s\n", order.c_str());
    }
    void SetUp() override {
        order += " setup";
        if (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) == "SetUpFails") {
            ASSERT_EQ(0, 1) << "setup failed";
        }
    }
    void TearDown() override { order += " teardown "; }
};

TEST_F(Fix, Body) { order += " body"; }

TEST_F(Fix, SetUpFails) { order += " NOT-REACHED"; }

TEST_F(Fix, Skipped) {
    order += " body";
    GTEST_SKIP() << "not today";
    order += " NOT-REACHED";
}

TEST(Trace, Shows) {
    SCOPED_TRACE("ctx 7");
    {
        SCOPED_TRACE(std::string("inner ") + "8");
    }
    EXPECT_EQ(1, 2);
}

TEST(Float, Fails) { EXPECT_FLOAT_EQ(1.0f, 1.001f); }

TEST(Throw, Std) { throw std::runtime_error("boom"); }

namespace {
void helper() { ASSERT_EQ(3, 4) << "in helper"; }
}  // namespace

TEST(NoFatal, Catches) {
    EXPECT_NO_FATAL_FAILURE(helper());
    printf("after helper\n");
}
