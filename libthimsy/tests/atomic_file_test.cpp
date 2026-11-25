// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/atomic_file.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>

#include "test_util.h"

using thimsy_test::ScratchDir;

namespace {
void killSelf(const char*) { raise(SIGKILL); }
std::string seenTemp;
void recordTemp(const char* p) { seenTemp = p; }
}  // namespace

TEST(AtomicFile, WritesAndReplaces) {
    ScratchDir d;
    const std::string f = d / "f.txt";
    std::string err;
    ASSERT_TRUE(thimsy::writeFileAtomic(f, "one", {}, &err)) << err;
    EXPECT_EQ(thimsy_test::slurp(f), "one");
    ASSERT_TRUE(thimsy::writeFileAtomic(f, std::string("two\0bytes", 9)));
    EXPECT_EQ(thimsy_test::slurp(f), std::string("two\0bytes", 9));
    ASSERT_TRUE(thimsy::writeFileAtomic(f, ""));
    EXPECT_EQ(thimsy_test::slurp(f), "");
    EXPECT_EQ(thimsy_test::listDir(d.path()).size(), 1u);  // no temp file left behind
}

TEST(AtomicFile, ModeIgnoresTheUmask) {
    ScratchDir d;
    const mode_t old = umask(077);
    thimsy::AtomicWriteOptions o;
    o.mode = 0644;
    EXPECT_TRUE(thimsy::writeFileAtomic(d / "a", "x", o));
    umask(0);
    o.mode = 0600;
    o.durable = false;
    EXPECT_TRUE(thimsy::writeFileAtomic(d / "b", "x", o));
    umask(old);
    struct stat st;
    ASSERT_EQ(stat((d / "a").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644u);
    ASSERT_EQ(stat((d / "b").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST(AtomicFile, TempFileIsBesideTheTargetAndHidden) {
    ScratchDir d;
    thimsy::setAtomicWriteTestHook(recordTemp);
    EXPECT_TRUE(thimsy::writeFileAtomic(d / "name.props", "x"));
    thimsy::setAtomicWriteTestHook(nullptr);
    EXPECT_EQ(seenTemp.rfind(d / ".name.props.tmp.", 0), 0u) << seenTemp;
    EXPECT_EQ(thimsy::atomicTempPrefix("/a/b/name.props"), ".name.props.tmp.");
}

TEST(AtomicFile, KilledBeforeRenameKeepsTheOldContent) {
    ScratchDir d;
    const std::string f = d / "state.json";
    ASSERT_TRUE(thimsy::writeFileAtomic(f, "{\"old\":true}"));
    const int status = thimsy_test::runInChild([&] {
        thimsy::setAtomicWriteTestHook(killSelf);
        thimsy::writeFileAtomic(f, "{\"new\":true}");
        return 0;
    });
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(thimsy_test::slurp(f), "{\"old\":true}");
    EXPECT_EQ(thimsy_test::listDir(d.path()).size(), 2u);  // the target and the stray temp
    thimsy::removeStaleTemps(f);
    const auto left = thimsy_test::listDir(d.path());
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0], "state.json");
}

TEST(AtomicFile, FailsCleanlyWithoutTheDirectory) {
    ScratchDir d;
    std::string err;
    EXPECT_FALSE(thimsy::writeFileAtomic(d / "missing/f", "x", {}, &err));
    EXPECT_NE(err.find("missing"), std::string::npos) << err;
    std::string out = "keep";
    EXPECT_FALSE(thimsy::readFile(d / "missing/f", &out));
    EXPECT_EQ(out, "keep");
}

TEST(AtomicFile, MakeDirs) {
    ScratchDir d;
    EXPECT_TRUE(thimsy::makeDirs(d / "a/b/c", 0700));
    EXPECT_TRUE(thimsy::makeDirs(d / "a/b/c"));  // existing is fine
    struct stat st;
    ASSERT_EQ(stat((d / "a/b/c").c_str(), &st), 0);
    EXPECT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_TRUE(thimsy::writeFileAtomic(d / "file", "x"));
    EXPECT_FALSE(thimsy::makeDirs(d / "file"));
    EXPECT_FALSE(thimsy::makeDirs(""));
}
