// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// A GoogleTest stand-in, written fresh: the part of GoogleTest's API that Thimsy's and Nano's host
// tests use, so they build with plain g++ where GoogleTest is not installed.
//
//   TEST, TEST_F (SetUp, TearDown, SetUpTestSuite, TearDownTestSuite)
//   EXPECT_ and ASSERT_: TRUE FALSE EQ NE LT LE GT GE STREQ STRNE STRCASEEQ STRCASENE
//                        FLOAT_EQ DOUBLE_EQ (within 4 ULPs, as GoogleTest) NEAR
//                        NO_FATAL_FAILURE; a message can be streamed after any of them
//   FAIL, ADD_FAILURE, SUCCEED, GTEST_SKIP, SCOPED_TRACE, testing::Test::HasFatalFailure,
//   testing::UnitTest::GetInstance()->current_test_info(), testing::TempDir()
//   --gtest_filter=POS[:POS...][-NEG[:NEG...]] with * and ? globs (or GTEST_FILTER), and
//   --gtest_list_tests
//
// Output follows GoogleTest's console format on stdout (without timings), failure details go to
// stderr, and the last line is "N tests, M failed" (skipped tests count as not failed). main() is
// defined here as a weak symbol (define THIMSY_GTEST_NO_MAIN to leave it out), so a test program
// may consist of several files that include this header, or define its own main.
#pragma once
#pragma GCC system_header  // like GoogleTest under -isystem: no warnings from its templates

#include <strings.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace testing {

namespace internal {
struct TestRunner;
}

// ---- Message -----------------------------------------------------------------------------------

class Message {
 public:
    Message() = default;
    Message(const Message& o) { ss_ << o.ss_.str(); }
    template <class T>
    Message& operator<<(const T& v) {
        ss_ << v;
        return *this;
    }
    Message& operator<<(const char* s) {
        ss_ << (s ? s : "(null)");
        return *this;
    }
    Message& operator<<(char* s) { return *this << static_cast<const char*>(s); }
    Message& operator<<(bool b) {
        ss_ << (b ? "true" : "false");
        return *this;
    }
    Message& operator<<(std::nullptr_t) {
        ss_ << "(nullptr)";
        return *this;
    }
    Message& operator<<(std::ostream& (*manip)(std::ostream&)) {
        ss_ << manip;
        return *this;
    }
    std::string GetString() const { return ss_.str(); }

 private:
    std::ostringstream ss_;
};

// ---- Printing values ---------------------------------------------------------------------------

namespace internal {

template <class T, class = void>
struct IsStreamable : std::false_type {};
template <class T>
struct IsStreamable<T, decltype(void(std::declval<std::ostream&>() << std::declval<const T&>()))> : std::true_type {};

template <class T, class = void>
struct IsIterable : std::false_type {};
template <class T>
struct IsIterable<T, decltype(void(std::begin(std::declval<const T&>())), void(std::end(std::declval<const T&>())))>
    : std::true_type {};

inline std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

template <class T>
std::string Print(const T& v) {
    using D = typename std::decay<T>::type;
    std::ostringstream os;
    if constexpr (std::is_same<D, bool>::value) {
        os << (v ? "true" : "false");
    } else if constexpr (std::is_same<D, std::nullptr_t>::value) {
        os << "(nullptr)";
    } else if constexpr (std::is_same<D, char>::value || std::is_same<D, signed char>::value ||
                         std::is_same<D, unsigned char>::value) {
        const int i = static_cast<int>(v);
        if (i >= 0x20 && i < 0x7F) os << '\'' << static_cast<char>(v) << "' (" << i << ")";
        else os << i;
    } else if constexpr (std::is_same<D, const char*>::value || std::is_same<D, char*>::value) {
        if (v == nullptr) os << "NULL";
        else os << Quote(v);
    } else if constexpr (std::is_same<D, std::string>::value || std::is_same<D, std::string_view>::value) {
        os << Quote(v);
    } else if constexpr (std::is_enum<D>::value && !IsStreamable<D>::value) {
        os << static_cast<long long>(v);
    } else if constexpr (IsStreamable<D>::value) {
        os << v;
    } else if constexpr (IsIterable<D>::value) {
        os << "{ ";
        bool first = true;
        for (const auto& e : v) {
            if (!first) os << ", ";
            first = false;
            os << Print(e);
        }
        os << " }";
    } else {
        os << "<" << sizeof(v) << "-byte object>";
    }
    return os.str();
}

}  // namespace internal

template <class T>
std::string PrintToString(const T& v) {
    return internal::Print(v);
}

// ---- AssertionResult ---------------------------------------------------------------------------

class AssertionResult {
 public:
    explicit AssertionResult(bool success) : success_(success) {}
    explicit operator bool() const { return success_; }
    AssertionResult operator!() const {
        AssertionResult r(!success_);
        r.message_ = message_;
        return r;
    }
    const char* message() const { return message_.c_str(); }
    const char* failure_message() const { return message(); }
    template <class T>
    AssertionResult& operator<<(const T& v) {
        message_ += (Message() << v).GetString();
        return *this;
    }

 private:
    bool success_;
    std::string message_;
};

inline AssertionResult AssertionSuccess() { return AssertionResult(true); }
inline AssertionResult AssertionFailure() { return AssertionResult(false); }

// ---- Test and its registry ---------------------------------------------------------------------

class Test {
 public:
    virtual ~Test() = default;
    static void SetUpTestSuite() {}
    static void TearDownTestSuite() {}
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    static bool HasFatalFailure();
    static bool HasNonfatalFailure();
    static bool HasFailure();
    static bool IsSkipped();

 protected:
    Test() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}

 private:
    virtual void TestBody() = 0;
    friend struct internal::TestRunner;
};

class TestInfo {
 public:
    TestInfo(std::string suite, std::string name) : suite_(std::move(suite)), name_(std::move(name)) {}
    const char* test_suite_name() const { return suite_.c_str(); }
    const char* test_case_name() const { return suite_.c_str(); }
    const char* name() const { return name_.c_str(); }

 private:
    std::string suite_, name_;
};

class UnitTest {
 public:
    static UnitTest* GetInstance() {
        static UnitTest instance;
        return &instance;
    }
    const TestInfo* current_test_info() const { return current_; }
    void set_current(const TestInfo* t) { current_ = t; }

 private:
    const TestInfo* current_ = nullptr;
};

inline std::string TempDir() {
    const char* t = getenv("TEST_TMPDIR");
    if (!t || !*t) t = getenv("TMPDIR");
    std::string dir = t && *t ? t : "/tmp";
    if (dir.back() != '/') dir += '/';
    return dir;
}

namespace internal {

struct TestEntry {
    std::string suite, name;
    const char* file;
    int line;
    void (*setUpSuite)();
    void (*tearDownSuite)();
    void (*setUpCase)();
    void (*tearDownCase)();
    Test* (*factory)();
};

inline std::vector<TestEntry>& Registry() {
    static std::vector<TestEntry> tests;
    return tests;
}

inline bool RegisterTest(const char* suite, const char* name, const char* file, int line,
                         void (*setUpSuite)(), void (*tearDownSuite)(), void (*setUpCase)(),
                         void (*tearDownCase)(), Test* (*factory)()) {
    Registry().push_back(TestEntry{suite, name, file, line, setUpSuite, tearDownSuite, setUpCase,
                                   tearDownCase, factory});
    return true;
}

struct State {
    std::atomic<bool> fatal{false};
    std::atomic<bool> nonfatal{false};
    std::atomic<bool> skipped{false};
    std::mutex outMu;
    std::string filter;
    bool listOnly = false;
};

inline State& GetState() {
    static State s;
    return s;
}

struct TraceEntry {
    const char* file;
    int line;
    std::string message;
};

inline std::vector<TraceEntry>& Traces() {
    thread_local std::vector<TraceEntry> traces;
    return traces;
}

enum class Kind { kNonFatal, kFatal, kSkip, kSuccess };

class AssertHelper {
 public:
    AssertHelper(Kind kind, const char* file, int line, const char* message)
        : kind_(kind), file_(file), line_(line), message_(message ? message : "") {}
    void operator=(const Message& user) const {
        if (kind_ == Kind::kSuccess) return;
        State& st = GetState();
        std::string text = std::string(file_) + ":" + std::to_string(line_) + ": ";
        text += kind_ == Kind::kSkip ? "Skipped" : "Failure";
        text += "\n";
        if (!message_.empty()) text += message_ + "\n";
        const std::string extra = user.GetString();
        if (!extra.empty()) text += extra + "\n";
        const auto& traces = Traces();
        if (!traces.empty() && kind_ != Kind::kSkip) {
            text += "Google Test trace:\n";
            for (auto it = traces.rbegin(); it != traces.rend(); ++it) {
                text += std::string(it->file) + ":" + std::to_string(it->line) + ": " + it->message + "\n";
            }
        }
        std::lock_guard<std::mutex> lock(st.outMu);
        if (kind_ == Kind::kSkip) {
            st.skipped = true;
            std::cout << text << std::flush;
        } else {
            if (kind_ == Kind::kFatal) st.fatal = true;
            else st.nonfatal = true;
            std::cout << std::flush;
            std::cerr << text << std::flush;
        }
    }

 private:
    Kind kind_;
    const char* file_;
    int line_;
    std::string message_;
};

// -- comparisons --

template <class A, class B>
AssertionResult CmpEQ(const char* ae, const char* be, const A& a, const B& b) {
    if (a == b) return AssertionSuccess();
    return AssertionFailure() << "Expected equality of these values:\n  " << ae << "\n    Which is: " << Print(a)
                              << "\n  " << be << "\n    Which is: " << Print(b);
}

#define GTEST_STANDIN_CMP_(NAME, OP)                                                                   \
    template <class A, class B>                                                                        \
    AssertionResult Cmp##NAME(const char* ae, const char* be, const A& a, const B& b) {                \
        if (a OP b) return AssertionSuccess();                                                         \
        return AssertionFailure() << "Expected: (" << ae << ") " #OP " (" << be << "), actual: " << Print(a) \
                                  << " vs " << Print(b);                                               \
    }
GTEST_STANDIN_CMP_(NE, !=)
GTEST_STANDIN_CMP_(LT, <)
GTEST_STANDIN_CMP_(LE, <=)
GTEST_STANDIN_CMP_(GT, >)
GTEST_STANDIN_CMP_(GE, >=)
#undef GTEST_STANDIN_CMP_

inline bool CStrEq(const char* a, const char* b, bool ignoreCase) {
    if (!a || !b) return a == b;
    return ignoreCase ? strcasecmp(a, b) == 0 : strcmp(a, b) == 0;
}

inline AssertionResult CmpStr(const char* ae, const char* be, const char* a, const char* b, bool wantEqual,
                              bool ignoreCase) {
    if (CStrEq(a, b, ignoreCase) == wantEqual) return AssertionSuccess();
    const char* op = wantEqual ? (ignoreCase ? "strcaseeq" : "streq") : (ignoreCase ? "strcasene" : "strne");
    return AssertionFailure() << "Expected: (" << ae << ") " << op << " (" << be << "), actual: " << Print(a)
                              << " vs " << Print(b);
}

inline AssertionResult CheckBool(bool value, bool expected, const char* expr) {
    if (value == expected) return AssertionSuccess();
    return AssertionFailure() << "Value of: " << expr << "\n  Actual: " << (value ? "true" : "false")
                              << "\nExpected: " << (expected ? "true" : "false");
}

// Within 4 ULPs, as GoogleTest's FloatingPoint::AlmostEquals; NaN is never equal.
template <class F>
bool AlmostEquals(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    using U = typename std::conditional<sizeof(F) == 4, uint32_t, uint64_t>::type;
    U ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    const U sign = U(1) << (sizeof(U) * 8 - 1);
    auto biased = [sign](U u) -> U { return (u & sign) ? static_cast<U>(~u + 1) : static_cast<U>(sign | u); };
    const U x = biased(ua), y = biased(ub);
    return (x >= y ? x - y : y - x) <= 4;
}

template <class F>
AssertionResult CmpFloatEQ(const char* ae, const char* be, F a, F b) {
    if (AlmostEquals<F>(a, b)) return AssertionSuccess();
    std::ostringstream as, bs;
    as.precision(sizeof(F) == 4 ? 9 : 17);
    bs.precision(sizeof(F) == 4 ? 9 : 17);
    as << a;
    bs << b;
    return AssertionFailure() << "Expected equality of these values:\n  " << ae << "\n    Which is: " << as.str()
                              << "\n  " << be << "\n    Which is: " << bs.str();
}

inline AssertionResult CmpNear(const char* ae, const char* be, const char* errExpr, double a, double b, double absError) {
    const double diff = std::fabs(a - b);
    if (diff <= absError) return AssertionSuccess();
    return AssertionFailure() << "The difference between " << ae << " and " << be << " is " << diff
                              << ", which exceeds " << errExpr << ", where\n"
                              << ae << " evaluates to " << a << ",\n"
                              << be << " evaluates to " << b << ", and\n"
                              << errExpr << " evaluates to " << absError << ".";
}

// -- filter --

inline bool GlobMatch(const char* pat, const char* str) {
    // Iterative glob with backtracking on the last '*'.
    const char* starPat = nullptr;
    const char* starStr = nullptr;
    while (*str) {
        if (*pat == '?' || *pat == *str) {
            ++pat;
            ++str;
        } else if (*pat == '*') {
            starPat = pat++;
            starStr = str;
        } else if (starPat) {
            pat = starPat + 1;
            str = ++starStr;
        } else {
            return false;
        }
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

inline bool MatchesAny(const std::string& patterns, const std::string& name) {
    size_t start = 0;
    while (start <= patterns.size()) {
        size_t end = patterns.find(':', start);
        if (end == std::string::npos) end = patterns.size();
        const std::string p = patterns.substr(start, end - start);
        if (!p.empty() && GlobMatch(p.c_str(), name.c_str())) return true;
        start = end + 1;
    }
    return false;
}

inline bool FilterAccepts(const std::string& filter, const std::string& name) {
    if (filter.empty()) return true;
    const size_t dash = filter.find('-');
    std::string pos = dash == std::string::npos ? filter : filter.substr(0, dash);
    const std::string neg = dash == std::string::npos ? "" : filter.substr(dash + 1);
    if (pos.empty()) pos = "*";
    return MatchesAny(pos, name) && !MatchesAny(neg, name);
}

// -- running --

struct TestRunner {
    static void Run(Test* t) {
        State& st = GetState();
        auto guarded = [&st](const char* where, const std::function<void()>& fn) {
            try {
                fn();
            } catch (const std::exception& e) {
                AssertHelper(Kind::kFatal, "unknown file", 0,
                             (std::string("C++ exception with description \"") + e.what() + "\" thrown in " + where + ".").c_str()) =
                    Message();
            } catch (...) {
                AssertHelper(Kind::kFatal, "unknown file", 0,
                             (std::string("Unknown C++ exception thrown in ") + where + ".").c_str()) = Message();
            }
            (void)st;
        };
        guarded("SetUp()", [t] { t->SetUp(); });
        if (!st.fatal && !st.skipped) guarded("the test body", [t] { t->TestBody(); });
        guarded("TearDown()", [t] { t->TearDown(); });
    }
};

inline const char* Plural(size_t n, const char* one, const char* many) { return n == 1 ? one : many; }

}  // namespace internal

inline bool Test::HasFatalFailure() { return internal::GetState().fatal; }
inline bool Test::HasNonfatalFailure() { return internal::GetState().nonfatal; }
inline bool Test::HasFailure() { return HasFatalFailure() || HasNonfatalFailure(); }
inline bool Test::IsSkipped() { return internal::GetState().skipped; }

class ScopedTrace {
 public:
    ScopedTrace(const char* file, int line, const Message& message) {
        internal::Traces().push_back(internal::TraceEntry{file, line, message.GetString()});
    }
    template <class T>
    ScopedTrace(const char* file, int line, const T& message) : ScopedTrace(file, line, Message() << message) {}
    ~ScopedTrace() { internal::Traces().pop_back(); }
    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;
};

inline void InitGoogleTest(int* argc, char** argv) {
    internal::State& st = internal::GetState();
    if (const char* env = getenv("GTEST_FILTER")) st.filter = env;
    int out = 1;
    for (int i = 1; i < *argc; ++i) {
        const char* a = argv[i];
        if (strncmp(a, "--gtest_filter=", 15) == 0) {
            st.filter = a + 15;
        } else if (strcmp(a, "--gtest_list_tests") == 0) {
            st.listOnly = true;
        } else if (strncmp(a, "--gtest_", 8) == 0) {
            // other GoogleTest flags (color, repeat, output) are accepted and ignored
        } else {
            argv[out++] = argv[i];
        }
    }
    *argc = out;
    argv[out] = nullptr;
}
inline void InitGoogleTest() {}

inline int RunAllTests() {
    using namespace internal;
    State& st = GetState();
    // Selected tests, grouped by suite in order of first registration.
    std::vector<std::string> suites;
    std::vector<std::vector<const TestEntry*>> bySuite;
    for (const TestEntry& e : Registry()) {
        if (!FilterAccepts(st.filter, e.suite + "." + e.name)) continue;
        size_t i = 0;
        while (i < suites.size() && suites[i] != e.suite) ++i;
        if (i == suites.size()) {
            suites.push_back(e.suite);
            bySuite.emplace_back();
        }
        bySuite[i].push_back(&e);
    }
    size_t total = 0;
    for (const auto& v : bySuite) total += v.size();
    if (st.listOnly) {
        for (size_t s = 0; s < suites.size(); ++s) {
            std::cout << suites[s] << ".\n";
            for (const TestEntry* e : bySuite[s]) std::cout << "  " << e->name << "\n";
        }
        return 0;
    }
    if (!st.filter.empty()) std::cout << "Note: Google Test filter = " << st.filter << "\n";
    std::cout << "[==========] Running " << total << Plural(total, " test", " tests") << " from " << suites.size()
              << Plural(suites.size(), " test suite.", " test suites.") << "\n"
              << std::flush;
    std::vector<std::string> failed, skipped;
    size_t passed = 0;
    for (size_t s = 0; s < suites.size(); ++s) {
        const auto& tests = bySuite[s];
        std::cout << "[----------] " << tests.size() << Plural(tests.size(), " test", " tests") << " from " << suites[s]
                  << "\n";
        tests.front()->setUpCase();
        tests.front()->setUpSuite();
        for (const TestEntry* e : tests) {
            const std::string full = e->suite + "." + e->name;
            std::cout << "[ RUN      ] " << full << "\n" << std::flush;
            st.fatal = false;
            st.nonfatal = false;
            st.skipped = false;
            TestInfo info(e->suite, e->name);
            UnitTest::GetInstance()->set_current(&info);
            {
                std::unique_ptr<Test> t;
                try {
                    t.reset(e->factory());
                } catch (...) {
                    AssertHelper(Kind::kFatal, e->file, e->line, "Exception thrown constructing the fixture.") = Message();
                }
                if (t) TestRunner::Run(t.get());
            }
            UnitTest::GetInstance()->set_current(nullptr);
            std::lock_guard<std::mutex> lock(st.outMu);
            if (st.fatal || st.nonfatal) {
                failed.push_back(full);
                std::cout << "[  FAILED  ] " << full << "\n";
            } else if (st.skipped) {
                skipped.push_back(full);
                std::cout << "[  SKIPPED ] " << full << "\n";
            } else {
                ++passed;
                std::cout << "[       OK ] " << full << "\n";
            }
            std::cout << std::flush;
        }
        tests.front()->tearDownSuite();
        tests.front()->tearDownCase();
        std::cout << "[----------] " << tests.size() << Plural(tests.size(), " test", " tests") << " from " << suites[s]
                  << "\n\n";
    }
    std::cout << "[==========] " << total << Plural(total, " test", " tests") << " from " << suites.size()
              << Plural(suites.size(), " test suite", " test suites") << " ran.\n";
    std::cout << "[  PASSED  ] " << passed << Plural(passed, " test.", " tests.") << "\n";
    if (!skipped.empty()) {
        std::cout << "[  SKIPPED ] " << skipped.size() << Plural(skipped.size(), " test", " tests") << ", listed below:\n";
        for (const auto& n : skipped) std::cout << "[  SKIPPED ] " << n << "\n";
    }
    if (!failed.empty()) {
        std::cout << "[  FAILED  ] " << failed.size() << Plural(failed.size(), " test", " tests") << ", listed below:\n";
        for (const auto& n : failed) std::cout << "[  FAILED  ] " << n << "\n";
    }
    std::cout << total << " tests, " << failed.size() << " failed\n" << std::flush;
    return failed.empty() ? 0 : 1;
}

}  // namespace testing

inline int RUN_ALL_TESTS() { return ::testing::RunAllTests(); }

// ---- Macros ------------------------------------------------------------------------------------

#define GTEST_STANDIN_CONCAT_INNER_(a, b) a##b
#define GTEST_STANDIN_CONCAT_(a, b) GTEST_STANDIN_CONCAT_INNER_(a, b)
#define GTEST_STANDIN_ELSE_BLOCKER_ \
    switch (0)                      \
    case 0:                         \
    default:

#define GTEST_STANDIN_MESSAGE_(kind, text) \
    ::testing::internal::AssertHelper(::testing::internal::Kind::kind, __FILE__, __LINE__, text) = ::testing::Message()

// on_failure is empty for EXPECT_ and "return" for ASSERT_.
#define GTEST_STANDIN_CHECK_(result, kind, on_failure)                           \
    GTEST_STANDIN_ELSE_BLOCKER_                                                  \
    if (const ::testing::AssertionResult gtest_ar_ = (result)) {                 \
    } else                                                                       \
        on_failure GTEST_STANDIN_MESSAGE_(kind, gtest_ar_.failure_message())

#define GTEST_STANDIN_EXPECT_(result) GTEST_STANDIN_CHECK_(result, kNonFatal, )
#define GTEST_STANDIN_ASSERT_(result) GTEST_STANDIN_CHECK_(result, kFatal, return)

// The suite hooks are reached through the test class, so a fixture may declare them protected.
#define GTEST_STANDIN_TEST_(suite, name, parent)                                                          \
    class suite##_##name##_Test : public parent {                                                         \
     public:                                                                                              \
        suite##_##name##_Test() = default;                                                                \
        static void GtestSetUpSuite_() { parent::SetUpTestSuite(); }                                      \
        static void GtestTearDownSuite_() { parent::TearDownTestSuite(); }                                \
        static void GtestSetUpCase_() { parent::SetUpTestCase(); }                                        \
        static void GtestTearDownCase_() { parent::TearDownTestCase(); }                                  \
        static ::testing::Test* GtestCreate_() { return new suite##_##name##_Test; }                      \
                                                                                                          \
     private:                                                                                             \
        void TestBody() override;                                                                         \
    };                                                                                                    \
    __attribute__((unused)) static const bool gtest_##suite##_##name##_registered_ =                       \
        ::testing::internal::RegisterTest(#suite, #name, __FILE__, __LINE__,                               \
                                          &suite##_##name##_Test::GtestSetUpSuite_,                        \
                                          &suite##_##name##_Test::GtestTearDownSuite_,                     \
                                          &suite##_##name##_Test::GtestSetUpCase_,                         \
                                          &suite##_##name##_Test::GtestTearDownCase_,                      \
                                          &suite##_##name##_Test::GtestCreate_);                           \
    void suite##_##name##_Test::TestBody()

#define TEST(suite, name) GTEST_STANDIN_TEST_(suite, name, ::testing::Test)
#define TEST_F(fixture, name) GTEST_STANDIN_TEST_(fixture, name, fixture)

#define EXPECT_TRUE(c) GTEST_STANDIN_EXPECT_(::testing::internal::CheckBool(static_cast<bool>(c), true, #c))
#define EXPECT_FALSE(c) GTEST_STANDIN_EXPECT_(::testing::internal::CheckBool(static_cast<bool>(c), false, #c))
#define ASSERT_TRUE(c) GTEST_STANDIN_ASSERT_(::testing::internal::CheckBool(static_cast<bool>(c), true, #c))
#define ASSERT_FALSE(c) GTEST_STANDIN_ASSERT_(::testing::internal::CheckBool(static_cast<bool>(c), false, #c))

#define EXPECT_EQ(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpEQ(#a, #b, a, b))
#define EXPECT_NE(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpNE(#a, #b, a, b))
#define EXPECT_LT(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpLT(#a, #b, a, b))
#define EXPECT_LE(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpLE(#a, #b, a, b))
#define EXPECT_GT(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpGT(#a, #b, a, b))
#define EXPECT_GE(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpGE(#a, #b, a, b))
#define ASSERT_EQ(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpEQ(#a, #b, a, b))
#define ASSERT_NE(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpNE(#a, #b, a, b))
#define ASSERT_LT(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpLT(#a, #b, a, b))
#define ASSERT_LE(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpLE(#a, #b, a, b))
#define ASSERT_GT(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpGT(#a, #b, a, b))
#define ASSERT_GE(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpGE(#a, #b, a, b))

#define EXPECT_STREQ(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpStr(#a, #b, a, b, true, false))
#define EXPECT_STRNE(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpStr(#a, #b, a, b, false, false))
#define EXPECT_STRCASEEQ(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpStr(#a, #b, a, b, true, true))
#define EXPECT_STRCASENE(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpStr(#a, #b, a, b, false, true))
#define ASSERT_STREQ(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpStr(#a, #b, a, b, true, false))
#define ASSERT_STRNE(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpStr(#a, #b, a, b, false, false))
#define ASSERT_STRCASEEQ(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpStr(#a, #b, a, b, true, true))
#define ASSERT_STRCASENE(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpStr(#a, #b, a, b, false, true))

#define EXPECT_FLOAT_EQ(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpFloatEQ<float>(#a, #b, a, b))
#define EXPECT_DOUBLE_EQ(a, b) GTEST_STANDIN_EXPECT_(::testing::internal::CmpFloatEQ<double>(#a, #b, a, b))
#define ASSERT_FLOAT_EQ(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpFloatEQ<float>(#a, #b, a, b))
#define ASSERT_DOUBLE_EQ(a, b) GTEST_STANDIN_ASSERT_(::testing::internal::CmpFloatEQ<double>(#a, #b, a, b))
#define EXPECT_NEAR(a, b, e) GTEST_STANDIN_EXPECT_(::testing::internal::CmpNear(#a, #b, #e, a, b, e))
#define ASSERT_NEAR(a, b, e) GTEST_STANDIN_ASSERT_(::testing::internal::CmpNear(#a, #b, #e, a, b, e))

// The statement runs in a lambda, so an ASSERT_ in it returns from the lambda, as in GoogleTest.
#define GTEST_STANDIN_NO_FATAL_(stmt, kind, on_failure)                                              \
    GTEST_STANDIN_ELSE_BLOCKER_                                                                      \
    if (const bool gtest_before_ = ::testing::Test::HasFatalFailure();                               \
        ((void)[&]() { stmt; }(), gtest_before_ || !::testing::Test::HasFatalFailure())) {           \
    } else                                                                                           \
        on_failure GTEST_STANDIN_MESSAGE_(kind, "Expected: " #stmt " doesn't generate new fatal failures in the current thread.\n  Actual: it does.")
#define EXPECT_NO_FATAL_FAILURE(stmt) GTEST_STANDIN_NO_FATAL_(stmt, kNonFatal, )
#define ASSERT_NO_FATAL_FAILURE(stmt) GTEST_STANDIN_NO_FATAL_(stmt, kFatal, return)

#define ADD_FAILURE() GTEST_STANDIN_MESSAGE_(kNonFatal, "Failed")
#define FAIL() return GTEST_STANDIN_MESSAGE_(kFatal, "Failed")
#define SUCCEED() GTEST_STANDIN_MESSAGE_(kSuccess, "")
#define GTEST_SKIP() return GTEST_STANDIN_MESSAGE_(kSkip, "")

#define SCOPED_TRACE(message) \
    ::testing::ScopedTrace GTEST_STANDIN_CONCAT_(gtest_trace_, __LINE__)(__FILE__, __LINE__, (message))

#ifndef THIMSY_GTEST_NO_MAIN
__attribute__((weak)) int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
