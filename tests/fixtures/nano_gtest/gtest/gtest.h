// The part of GoogleTest that Nano's host tests use (TEST, EXPECT_* and ASSERT_*, a message streamed
// after a check), so tests/unit/test_nano_units.py can build and run them with plain g++. Main is
// defined here too: include this from one file per program.
#pragma once

#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestInfo {
    const char* name;
    void (*body)();
};

inline std::vector<TestInfo>& registry() {
    static std::vector<TestInfo> tests;
    return tests;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*body)()) { registry().push_back({name, body}); }
};

class Message {
public:
    template <class T>
    Message& operator<<(const T& v) {
        text_ << v;
        return *this;
    }
    std::string str() const { return text_.str(); }

private:
    std::ostringstream text_;
};

// Reports a failed check; returns void, so an ASSERT can "return helper = Message() << ...".
struct AssertHelper {
    const char* file;
    int line;
    const char* what;
    void operator=(const Message& m) const {
        failures()++;
        std::cerr << file << ":" << line << ": check failed: " << what << " " << m.str() << "\n";
    }
};

inline bool strEq(const char* a, const char* b) { return a && b ? strcmp(a, b) == 0 : a == b; }

}  // namespace testing

#define TEST(suite, name)                                                                     \
    static void suite##_##name##_body();                                                      \
    static ::testing::Registrar suite##_##name##_registrar(#suite "." #name, suite##_##name##_body); \
    static void suite##_##name##_body()

#define GTEST_SHIM_CHECK_(cond, text, onFail) \
    if (cond) {                               \
    } else                                    \
        onFail ::testing::AssertHelper{__FILE__, __LINE__, text} = ::testing::Message()

#define EXPECT_TRUE(c) GTEST_SHIM_CHECK_(!!(c), #c, )
#define EXPECT_FALSE(c) GTEST_SHIM_CHECK_(!(c), "!(" #c ")", )
#define EXPECT_EQ(a, b) GTEST_SHIM_CHECK_((a) == (b), #a " == " #b, )
#define EXPECT_NE(a, b) GTEST_SHIM_CHECK_((a) != (b), #a " != " #b, )
#define EXPECT_STREQ(a, b) GTEST_SHIM_CHECK_(::testing::strEq((a), (b)), #a " streq " #b, )
#define EXPECT_NEAR(a, b, e) GTEST_SHIM_CHECK_(std::fabs((a) - (b)) <= (e), #a " near " #b, )
#define ASSERT_TRUE(c) GTEST_SHIM_CHECK_(!!(c), #c, return)
#define ASSERT_FALSE(c) GTEST_SHIM_CHECK_(!(c), "!(" #c ")", return)
#define ASSERT_EQ(a, b) GTEST_SHIM_CHECK_((a) == (b), #a " == " #b, return)
#define ASSERT_NE(a, b) GTEST_SHIM_CHECK_((a) != (b), #a " != " #b, return)
#define ASSERT_STREQ(a, b) GTEST_SHIM_CHECK_(::testing::strEq((a), (b)), #a " streq " #b, return)

int main() {
    int failed = 0;
    for (const auto& t : ::testing::registry()) {
        const int before = ::testing::failures();
        t.body();
        const bool ok = ::testing::failures() == before;
        failed += ok ? 0 : 1;
        std::cout << (ok ? "[       OK ] " : "[  FAILED  ] ") << t.name << "\n";
    }
    std::cout << ::testing::registry().size() << " tests, " << failed << " failed\n";
    return failed ? 1 : 0;
}
