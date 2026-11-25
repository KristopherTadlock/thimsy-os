// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The GoogleTest stand-in's own tests: its checks pass where they should, and (by running
// gtest_selfcheck_cases, whose path is in THIMSY_GTEST_SELFCHECK) they fail where they should,
// with GoogleTest's output, the "N tests, M failed" line, filters and fixture order.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_util.h"

namespace {

int suiteSetUps = 0;

class Counting : public ::testing::Test {
 protected:
    static void SetUpTestSuite() { ++suiteSetUps; }
    void SetUp() override { value = 42; }
    int value = 0;
};

std::string selfcheck() {
    const char* p = getenv("THIMSY_GTEST_SELFCHECK");
    return p && *p ? p : "";
}

size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + 1)) ++n;
    return n;
}

std::string lastLine(const std::string& s) {
    if (s.empty() || s.back() != '\n') return "(no final newline)";
    const size_t start = s.rfind('\n', s.size() - 2);
    return s.substr(start == std::string::npos ? 0 : start + 1);
}

}  // namespace

TEST_F(Counting, FixtureSetUpRuns) { EXPECT_EQ(value, 42); }
TEST_F(Counting, SuiteSetUpRunsOnce) { EXPECT_EQ(suiteSetUps, 1); }

TEST(StandIn, Comparisons) {
    const std::vector<int> v = {1, 2};
    EXPECT_EQ(2, v.size());  // signed against unsigned compiles under -Werror, as with GoogleTest
    EXPECT_NE(1, 2);
    EXPECT_LT(1, 2);
    EXPECT_LE(2, 2);
    EXPECT_GT(3u, 2u);
    EXPECT_GE(-1, -1);
    ASSERT_LT(1.5, 2.0);
    EXPECT_STRNE("a", "b");
    EXPECT_STRNE("a", nullptr);
    EXPECT_STRCASEEQ("ThImSy", "thimsy");
    EXPECT_STREQ(nullptr, nullptr);
    const char* p = nullptr;
    EXPECT_EQ(nullptr, p);
    EXPECT_TRUE(std::unique_ptr<int>(new int(1)));
    EXPECT_FALSE(std::unique_ptr<int>());
    SUCCEED();
}

TEST(StandIn, FloatingPointUlps) {
    float f = 1.0f;
    for (int i = 0; i < 4; ++i) f = std::nextafter(f, 2.0f);
    EXPECT_FLOAT_EQ(1.0f, f);  // 4 ULPs apart
    EXPECT_FALSE(::testing::internal::AlmostEquals<float>(1.0f, std::nextafter(f, 2.0f)));  // 5
    EXPECT_FLOAT_EQ(0.0f, -0.0f);
    EXPECT_FALSE(::testing::internal::AlmostEquals<float>(NAN, NAN));
    double d = -3.0;
    for (int i = 0; i < 4; ++i) d = std::nextafter(d, 0.0);
    EXPECT_DOUBLE_EQ(-3.0, d);
    EXPECT_FALSE(::testing::internal::AlmostEquals<double>(1.0, 1.0 + 1e-12));
    EXPECT_NEAR(1.0, 1.05, 0.1);
    ASSERT_NEAR(-1.0, -1.0, 0.0);
}

TEST(StandIn, Globs) {
    using ::testing::internal::FilterAccepts;
    using ::testing::internal::GlobMatch;
    EXPECT_TRUE(GlobMatch("*", "A.B"));
    EXPECT_TRUE(GlobMatch("A.*", "A.B"));
    EXPECT_TRUE(GlobMatch("?.B", "A.B"));
    EXPECT_TRUE(GlobMatch("*a*b*", "xxaxxbxx"));
    EXPECT_FALSE(GlobMatch("A.?", "A.BC"));
    EXPECT_FALSE(GlobMatch("B*", "A.B"));
    EXPECT_TRUE(FilterAccepts("", "A.B"));
    EXPECT_TRUE(FilterAccepts("X.*:A.*", "A.B"));
    EXPECT_FALSE(FilterAccepts("A.*-A.B", "A.B"));
    EXPECT_TRUE(FilterAccepts("-A.C", "A.B"));
    EXPECT_FALSE(FilterAccepts("-*.B:Q.*", "A.B"));
}

TEST(StandIn, Printing) {
    EXPECT_EQ(::testing::PrintToString(std::string("a\"b")), "\"a\\\"b\"");
    EXPECT_EQ(::testing::PrintToString(true), "true");
    EXPECT_EQ(::testing::PrintToString('A'), "'A' (65)");
    EXPECT_EQ(::testing::PrintToString(std::vector<int>{1, 2}), "{ 1, 2 }");
    EXPECT_EQ(::testing::PrintToString(static_cast<const char*>(nullptr)), "NULL");
}

TEST(StandIn, NoFatalFailurePassesThrough) {
    int ran = 0;
    EXPECT_NO_FATAL_FAILURE(++ran);
    ASSERT_NO_FATAL_FAILURE({ ++ran; });
    EXPECT_EQ(ran, 2);
}

TEST(StandIn, ReportsFailuresLikeGoogleTest) {
    if (selfcheck().empty()) GTEST_SKIP() << "THIMSY_GTEST_SELFCHECK is not set";
    const auto r = thimsy_test::runTool({selfcheck()});
    EXPECT_EQ(r.code(), 1);
    // What tests/unit/test_nano_units.py asserts of the old stand-in.
    EXPECT_NE(r.out.find("[       OK ] A.Passes\n"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("[  FAILED  ] A.Fails\n"), std::string::npos);
    EXPECT_NE(r.out.find("[  FAILED  ] A.Expects\n"), std::string::npos);
    EXPECT_NE(r.err.find("one"), std::string::npos) << r.err;
    EXPECT_EQ(r.err.find("Value of: false"), std::string::npos);  // the ASSERT returned first
    EXPECT_EQ(count(r.err, "streq"), 1u);
    // GoogleTest's frame.
    EXPECT_EQ(r.out.rfind("[==========] Running 10 tests from 6 test suites.\n", 0), 0u) << r.out;
    EXPECT_NE(r.out.find("[ RUN      ] Fix.Body\n"), std::string::npos);
    EXPECT_NE(r.out.find("[  PASSED  ] 2 tests.\n"), std::string::npos);
    EXPECT_NE(r.out.find("[  FAILED  ] 7 tests, listed below:\n"), std::string::npos);
    EXPECT_EQ(lastLine(r.out), "10 tests, 7 failed\n");
    // Fixture order: a fatal SetUp skips the body, TearDown always runs, a skip is not a failure.
    EXPECT_NE(r.out.find("order: [suite setup body teardown  setup teardown  setup body teardown suite]\n"),
              std::string::npos);
    EXPECT_NE(r.out.find("[  SKIPPED ] Fix.Skipped\n"), std::string::npos);
    EXPECT_NE(r.out.find("not today"), std::string::npos);
    EXPECT_NE(r.err.find("setup failed"), std::string::npos);
    // Details.
    EXPECT_NE(r.err.find("Google Test trace:"), std::string::npos);
    EXPECT_NE(r.err.find("ctx 7"), std::string::npos);
    EXPECT_EQ(r.err.find("inner 8"), std::string::npos);  // its scope ended before the failure
    EXPECT_NE(r.err.find("1.00100005"), std::string::npos);
    EXPECT_NE(r.err.find("boom"), std::string::npos);
    EXPECT_NE(r.err.find("in helper"), std::string::npos);
    EXPECT_NE(r.err.find("doesn't generate new fatal failures"), std::string::npos);
    EXPECT_NE(r.out.find("after helper\n"), std::string::npos);  // EXPECT_ continues
    EXPECT_NE(r.out.find("[  FAILED  ] NoFatal.Catches\n"), std::string::npos);
}

TEST(StandIn, Filters) {
    if (selfcheck().empty()) GTEST_SKIP() << "THIMSY_GTEST_SELFCHECK is not set";
    auto r = thimsy_test::runTool({selfcheck(), "--gtest_filter=A.Passes"});
    EXPECT_EQ(r.code(), 0);
    EXPECT_EQ(lastLine(r.out), "1 tests, 0 failed\n");
    EXPECT_EQ(r.out.find("FAILED"), std::string::npos);

    r = thimsy_test::runTool({selfcheck(), "--gtest_filter=*-A.*:Throw*:NoFatal.*:Float.*:Trace.*:Fix.SetUpFails"});
    EXPECT_EQ(r.code(), 0) << r.out;
    EXPECT_EQ(lastLine(r.out), "2 tests, 0 failed\n");
    EXPECT_NE(r.out.find("[  SKIPPED ] 1 test, listed below:\n"), std::string::npos);

    r = thimsy_test::runTool({selfcheck(), "--gtest_color=no", "--gtest_filter=?.Fail*"}, {"GTEST_FILTER=nothing"});
    EXPECT_EQ(r.code(), 1);
    EXPECT_EQ(lastLine(r.out), "1 tests, 1 failed\n");

    r = thimsy_test::runTool({selfcheck()}, {"GTEST_FILTER=Throw.*"});
    EXPECT_EQ(lastLine(r.out), "1 tests, 1 failed\n");

    r = thimsy_test::runTool({selfcheck(), "--gtest_filter=Nothing.*"});
    EXPECT_EQ(r.code(), 0);
    EXPECT_EQ(lastLine(r.out), "0 tests, 0 failed\n");

    r = thimsy_test::runTool({selfcheck(), "--gtest_list_tests", "--gtest_filter=Fix.*"});
    EXPECT_EQ(r.code(), 0);
    EXPECT_EQ(r.out, "Fix.\n  Body\n  SetUpFails\n  Skipped\n");
}
