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

// cutils/properties.h and android-base/properties.h against Android's documented behaviour, and the
// property observers of nanocompat/nanocompat.h.

#include <android-base/properties.h>
#include <cutils/properties.h>

#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include <atomic>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <thimsy/config.h>

#include "compat_test_util.h"

using compat_test::StoreTest;

namespace {

std::string get(const char* key, const char* def = "") {
    char buf[PROPERTY_VALUE_MAX];
    memset(buf, 'x', sizeof buf);
    property_get(key, buf, def);
    return buf;
}

}  // namespace

class Props : public StoreTest {};

TEST_F(Props, ConstantsAreAndroids) {
    EXPECT_EQ(PROPERTY_VALUE_MAX, 92);
    EXPECT_EQ(PROPERTY_KEY_MAX, 32);
    EXPECT_EQ(PROP_VALUE_MAX, 92);
}

TEST_F(Props, UnsetGivesTheDefault) {
    char buf[PROPERTY_VALUE_MAX];
    EXPECT_EQ(property_get("sys.test.unset", buf, "fallback"), 8);
    EXPECT_STREQ(buf, "fallback");
    memset(buf, 'x', sizeof buf);
    EXPECT_EQ(property_get("sys.test.unset", buf, nullptr), 0);
    EXPECT_STREQ(buf, "");
    EXPECT_EQ(property_get("sys.test.unset", buf, ""), 0);
    EXPECT_STREQ(buf, "");
}

TEST_F(Props, DefaultIsCutTo91Bytes) {
    const std::string longDef(200, 'd');
    char buf[PROPERTY_VALUE_MAX];
    EXPECT_EQ(property_get("sys.test.unset", buf, longDef.c_str()), 91);
    EXPECT_EQ(std::string(buf), std::string(91, 'd'));
}

TEST_F(Props, SetThenGet) {
    EXPECT_EQ(property_set("sys.test.a", "hello"), 0);
    char buf[PROPERTY_VALUE_MAX];
    EXPECT_EQ(property_get("sys.test.a", buf, "def"), 5);
    EXPECT_STREQ(buf, "hello");
    EXPECT_EQ(android::base::GetProperty("sys.test.a", "def"), "hello");
}

TEST_F(Props, ValueLimitIs91BytesExceptReadOnly) {
    const std::string v91(91, 'a'), v92(92, 'b');
    EXPECT_EQ(property_set("sys.test.len", v91.c_str()), 0);
    EXPECT_EQ(get("sys.test.len"), v91);
    EXPECT_EQ(property_set("sys.test.len", v92.c_str()), -1);
    EXPECT_EQ(get("sys.test.len"), v91);  // unchanged
    EXPECT_FALSE(android::base::SetProperty("persist.test.len", v92));
    EXPECT_TRUE(android::base::SetProperty("persist.test.len", v91));

    // ro.* may be longer: property_get cuts it, GetProperty does not.
    const std::string v200(200, 'r');
    EXPECT_EQ(property_set("ro.test.long", v200.c_str()), 0);
    char buf[PROPERTY_VALUE_MAX];
    EXPECT_EQ(property_get("ro.test.long", buf, ""), 91);
    EXPECT_EQ(std::string(buf), std::string(91, 'r'));
    EXPECT_EQ(android::base::GetProperty("ro.test.long", ""), v200);
}

TEST_F(Props, ReadOnlyIsSetOnce) {
    EXPECT_EQ(property_set("ro.test.once", "1"), 0);
    EXPECT_EQ(property_set("ro.test.once", "2"), -1);
    EXPECT_EQ(get("ro.test.once"), "1");
}

TEST_F(Props, EmptyOrNullValueClears) {
    EXPECT_EQ(property_set("sys.test.c", "x"), 0);
    EXPECT_EQ(property_set("sys.test.c", ""), 0);
    EXPECT_EQ(get("sys.test.c", "gone"), "gone");
    EXPECT_EQ(property_set("sys.test.c", "y"), 0);
    EXPECT_EQ(property_set("sys.test.c", nullptr), 0);
    EXPECT_EQ(get("sys.test.c", "gone"), "gone");
}

TEST_F(Props, BadKeysAreRefused) {
    EXPECT_EQ(property_set(nullptr, "1"), -1);
    EXPECT_EQ(property_set("", "1"), -1);
    EXPECT_EQ(property_set("has space", "1"), -1);
    EXPECT_EQ(property_set("a..b", "1"), -1);
    EXPECT_EQ(property_set(".lead", "1"), -1);
    EXPECT_EQ(property_set("trail.", "1"), -1);
    EXPECT_EQ(get("has space", "def"), "def");
    EXPECT_EQ(android::base::GetProperty("has space", "def"), "def");
}

TEST_F(Props, PersistKeysReachThePersistFile) {
    EXPECT_EQ(property_set("persist.test.p", "kept"), 0);
    const std::string text = compat_test::readFile(dir_ / "persist.props");
    EXPECT_NE(text.find("persist.test.p=kept"), std::string::npos) << text;
    struct stat st;
    ASSERT_EQ(stat((dir_ / "persist.props").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST_F(Props, AnotherWriterIsSeenOnRead) {
    // A second store on the same roots stands in for another process.
    std::string err;
    auto other = thimsy::ConfigStore::open({dir_ / "persist.props", dir_ / "run"}, &err);
    ASSERT_TRUE(other != nullptr) << err;
    ASSERT_TRUE(other->set("sys.test.other", "from-elsewhere"));
    ASSERT_TRUE(other->set("persist.test.other", "p"));
    EXPECT_EQ(get("sys.test.other"), "from-elsewhere");
    EXPECT_EQ(get("persist.test.other"), "p");
}

TEST_F(Props, BoolSpellings) {
    const char* const truths[] = {"1", "y", "yes", "on", "true"};
    const char* const falsehoods[] = {"0", "n", "no", "off", "false"};
    const char* const neither[] = {"TRUE", "Yes", "2", " true", "true ", "yes!", "nope", "-1"};
    for (const char* v : truths) {
        SCOPED_TRACE(v);
        ASSERT_EQ(property_set("sys.test.b", v), 0);
        EXPECT_EQ(property_get_bool("sys.test.b", 0), 1);
        EXPECT_TRUE(android::base::GetBoolProperty("sys.test.b", false));
    }
    for (const char* v : falsehoods) {
        SCOPED_TRACE(v);
        ASSERT_EQ(property_set("sys.test.b", v), 0);
        EXPECT_EQ(property_get_bool("sys.test.b", 1), 0);
        EXPECT_FALSE(android::base::GetBoolProperty("sys.test.b", true));
    }
    for (const char* v : neither) {
        SCOPED_TRACE(v);
        ASSERT_EQ(property_set("sys.test.b", v), 0);
        EXPECT_EQ(property_get_bool("sys.test.b", 1), 1);
        EXPECT_EQ(property_get_bool("sys.test.b", 0), 0);
        EXPECT_TRUE(android::base::GetBoolProperty("sys.test.b", true));
        EXPECT_FALSE(android::base::GetBoolProperty("sys.test.b", false));
    }
    EXPECT_EQ(property_get_bool("sys.test.unset", 1), 1);
    EXPECT_EQ(property_get_bool(nullptr, 1), 1);
}

TEST_F(Props, Int32LikeLibcutils) {
    struct Case {
        const char* value;
        int32_t expect;
    };
    const Case cases[] = {
        {"42", 42},          {"-7", -7},          {"+5", 5},
        {"0x10", 16},        {"010", 8},          {"  12", 12},
        {"12abc", 12},       {"abc", -99},        {"2147483647", 2147483647},
        {"2147483648", -99}, {"-2147483648", INT32_MIN},
        {"-2147483649", -99}, {"99999999999999999999999", -99},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.value);
        ASSERT_EQ(property_set("sys.test.i", c.value), 0);
        EXPECT_EQ(property_get_int32("sys.test.i", -99), c.expect);
    }
    EXPECT_EQ(property_get_int32("sys.test.unset", 3), 3);
    EXPECT_EQ(property_get_int32(nullptr, 3), 3);
}

TEST_F(Props, Int64LikeLibcutils) {
    ASSERT_EQ(property_set("sys.test.l", "9223372036854775807"), 0);
    EXPECT_EQ(property_get_int64("sys.test.l", -1), INT64_MAX);
    ASSERT_EQ(property_set("sys.test.l", "9223372036854775808"), 0);
    EXPECT_EQ(property_get_int64("sys.test.l", -1), -1);
    ASSERT_EQ(property_set("sys.test.l", "-9223372036854775808"), 0);
    EXPECT_EQ(property_get_int64("sys.test.l", -1), INT64_MIN);
    ASSERT_EQ(property_set("sys.test.l", "4294967296"), 0);
    EXPECT_EQ(property_get_int64("sys.test.l", -1), 4294967296LL);
    EXPECT_EQ(property_get_int32("sys.test.l", -1), -1);  // outside int32
}

TEST_F(Props, IntLikeLibbase) {
    struct Case {
        const char* value;
        int expect;
    };
    // libbase's ParseInt: whole string, base 16 after "0x", else base 10.
    const Case cases[] = {
        {"42", 42}, {"-7", -7}, {"0x10", 16}, {"010", 10}, {"  12", 12},
        {"12abc", -99}, {"12 ", -99}, {"abc", -99}, {"2147483648", -99}, {"", -99},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.value);
        ASSERT_EQ(property_set("sys.test.j", c.value), 0);
        EXPECT_EQ(android::base::GetIntProperty("sys.test.j", -99), c.expect);
    }
    ASSERT_EQ(property_set("sys.test.j", "11"), 0);
    EXPECT_EQ(android::base::GetIntProperty("sys.test.j", 3, 0, 10), 3);
    EXPECT_EQ(android::base::GetIntProperty("sys.test.j", 3, 0, 11), 11);
    EXPECT_EQ(android::base::GetIntProperty<int64_t>("sys.test.j", 3), 11);
    EXPECT_EQ(android::base::GetUintProperty<uint32_t>("sys.test.j", 3u), 11u);
    EXPECT_EQ(android::base::GetUintProperty<uint32_t>("sys.test.j", 3u, 10u), 3u);
    ASSERT_EQ(property_set("sys.test.j", "-1"), 0);
    EXPECT_EQ(android::base::GetUintProperty<uint32_t>("sys.test.j", 3u), 3u);
    EXPECT_EQ(android::base::GetIntProperty("sys.test.unset", 8), 8);
}

TEST_F(Props, PropertyListSeesEveryKey) {
    ASSERT_EQ(property_set("sys.test.l1", "a"), 0);
    ASSERT_EQ(property_set("persist.test.l2", "b"), 0);
    std::map<std::string, std::string> seen;
    property_list(
        [](const char* k, const char* v, void* cookie) {
            (*static_cast<std::map<std::string, std::string>*>(cookie))[k] = v;
        },
        &seen);
    EXPECT_EQ(seen["sys.test.l1"], "a");
    EXPECT_EQ(seen["persist.test.l2"], "b");
}

// ---- observers (the control-key hook) ----

class Observers : public StoreTest {
 protected:
    void TearDown() override {
        for (int id : ids_) nanocompat::removePropertyObserver(id);
    }
    int add(const char* pattern, std::vector<std::pair<std::string, std::string>>* log) {
        const int id = nanocompat::addPropertyObserver(pattern, [log](std::string_view k, std::string_view v) {
            log->emplace_back(std::string(k), std::string(v));
        });
        ids_.push_back(id);
        return id;
    }
    std::vector<int> ids_;
};

TEST_F(Observers, SeeEverySetEvenOfTheSameValue) {
    std::vector<std::pair<std::string, std::string>> log;
    add("sys.test.do_launch", &log);
    ASSERT_EQ(property_set("sys.test.do_launch", "1"), 0);
    ASSERT_EQ(property_set("sys.test.do_launch", "1"), 0);  // Android's triggers fire again
    ASSERT_EQ(property_set("sys.test.do_launch", ""), 0);
    ASSERT_EQ(property_set("sys.test.other", "1"), 0);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0].second, "1");
    EXPECT_EQ(log[1].second, "1");
    EXPECT_EQ(log[2].second, "");
}

TEST_F(Observers, RunAfterTheValueIsStored) {
    std::string seenInside;
    ids_.push_back(nanocompat::addPropertyObserver("sys.test.after", [&](std::string_view, std::string_view) {
        seenInside = get("sys.test.after");
    }));
    ASSERT_EQ(property_set("sys.test.after", "stored"), 0);
    EXPECT_EQ(seenInside, "stored");
}

TEST_F(Observers, CanNeitherVetoNorSwallow) {
    ids_.push_back(nanocompat::addPropertyObserver("", [](std::string_view, std::string_view) {
        throw std::runtime_error("an observer that throws");
    }));
    std::vector<std::pair<std::string, std::string>> log;
    add("sys.test.", &log);
    EXPECT_EQ(property_set("sys.test.v", "1"), 0);
    EXPECT_EQ(get("sys.test.v"), "1");
    EXPECT_EQ(log.size(), 1u);  // the next observer still ran
}

TEST_F(Observers, Patterns) {
    std::vector<std::pair<std::string, std::string>> exact, dot, star, all;
    add("sys.test.k", &exact);
    add("sys.test.", &dot);
    add("sys.te*", &star);
    add("", &all);
    ASSERT_EQ(property_set("sys.test.k", "1"), 0);
    ASSERT_EQ(property_set("sys.test.kk", "1"), 0);
    ASSERT_EQ(property_set("sys.tex", "1"), 0);
    ASSERT_EQ(property_set("persist.test.z", "1"), 0);
    EXPECT_EQ(exact.size(), 1u);
    EXPECT_EQ(dot.size(), 2u);
    EXPECT_EQ(star.size(), 3u);
    EXPECT_EQ(all.size(), 4u);
}

TEST_F(Observers, RefusedSetsAreNotSeen) {
    std::vector<std::pair<std::string, std::string>> log;
    add("", &log);
    EXPECT_EQ(property_set("sys.test.big", std::string(92, 'x').c_str()), -1);
    EXPECT_EQ(property_set("bad key", "1"), -1);
    EXPECT_TRUE(log.empty());
}

TEST_F(Observers, RemoveStopsThem) {
    std::vector<std::pair<std::string, std::string>> log;
    const int id = add("sys.test.r", &log);
    ASSERT_EQ(property_set("sys.test.r", "1"), 0);
    nanocompat::removePropertyObserver(id);
    ASSERT_EQ(property_set("sys.test.r", "2"), 0);
    EXPECT_EQ(log.size(), 1u);
}

TEST_F(Observers, MaySetProperties) {
    ids_.push_back(nanocompat::addPropertyObserver("sys.test.trigger", [](std::string_view, std::string_view v) {
        property_set("sys.test.echo", std::string(v).c_str());
    }));
    std::vector<std::pair<std::string, std::string>> log;
    add("sys.test.echo", &log);
    ASSERT_EQ(property_set("sys.test.trigger", "ping"), 0);
    EXPECT_EQ(get("sys.test.echo"), "ping");
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].second, "ping");
}

TEST_F(Observers, SurviveAStoreChange) {
    std::vector<std::pair<std::string, std::string>> log;
    add("sys.test.s", &log);
    compat_test::ScratchDir other;
    ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(other)));
    ASSERT_EQ(property_set("sys.test.s", "1"), 0);
    EXPECT_EQ(log.size(), 1u);
}
