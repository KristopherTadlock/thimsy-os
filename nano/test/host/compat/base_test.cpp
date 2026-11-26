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

// android-base: file.h, unique_fd.h, strings.h, parseint.h and parsebool.h.

#include <android-base/file.h>
#include <android-base/parsebool.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "compat_test_util.h"

using android::base::unique_fd;

TEST(File, WriteThenRead) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "f.txt";
    ASSERT_TRUE(android::base::WriteStringToFile("hello\nworld", path));
    std::string got;
    ASSERT_TRUE(android::base::ReadFileToString(path, &got));
    EXPECT_EQ(got, "hello\nworld");
    ASSERT_TRUE(android::base::WriteStringToFile("short", path));  // truncates
    ASSERT_TRUE(android::base::ReadFileToString(path, &got));
    EXPECT_EQ(got, "short");
    EXPECT_FALSE(android::base::ReadFileToString(dir / "missing", &got));
}

TEST(File, NoFollowUnlessAsked) {
    compat_test::ScratchDir dir;
    ASSERT_EQ(mkdir((dir / "real").c_str(), 0755), 0);
    ASSERT_TRUE(android::base::WriteStringToFile("data", dir / "real/f"));
    ASSERT_EQ(symlink("real/f", (dir / "link").c_str()), 0);
    ASSERT_EQ(symlink("real", (dir / "dirlink").c_str()), 0);
    std::string got;
    EXPECT_FALSE(android::base::ReadFileToString(dir / "link", &got));  // libbase: O_NOFOLLOW
    EXPECT_TRUE(android::base::ReadFileToString(dir / "link", &got, true));
    EXPECT_EQ(got, "data");
    // A link earlier in the path (the Linux image's /data) is followed as usual.
    EXPECT_TRUE(android::base::ReadFileToString(dir / "dirlink/f", &got));
    EXPECT_FALSE(android::base::WriteStringToFile("x", dir / "link"));
}

TEST(File, WriteWithModeIgnoresTheUmask) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "m";
    const mode_t old = umask(077);
    ASSERT_TRUE(android::base::WriteStringToFile("x", path, 0644, getuid(), getgid()));
    umask(old);
    struct stat st;
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644u);
}

TEST(File, FdHelpers) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "fd";
    unique_fd fd(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    ASSERT_TRUE(fd.ok());
    ASSERT_TRUE(android::base::WriteStringToFd("0123456789", fd));
    ASSERT_EQ(lseek(fd.get(), 0, SEEK_SET), 0);
    char buf[4];
    ASSERT_TRUE(android::base::ReadFully(fd, buf, 4));
    EXPECT_EQ(std::string(buf, 4), "0123");
    ASSERT_TRUE(android::base::ReadFullyAtOffset(fd, buf, 3, 7));
    EXPECT_EQ(std::string(buf, 3), "789");
    std::string rest;
    ASSERT_TRUE(android::base::ReadFdToString(fd, &rest));
    EXPECT_EQ(rest, "456789");
    errno = 0;
    EXPECT_FALSE(android::base::ReadFully(fd, buf, 1));  // at EOF
    EXPECT_EQ(errno, ENODATA);
}

TEST(File, UniqueFdOwnsAndCloses) {
    int raw;
    {
        unique_fd fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        raw = fd.get();
        ASSERT_GE(raw, 0);
        EXPECT_TRUE(fd == raw);
        EXPECT_TRUE(fd >= 0);
        unique_fd moved(std::move(fd));
        EXPECT_EQ(fd.get(), -1);
        EXPECT_EQ(moved.get(), raw);
        EXPECT_NE(fcntl(raw, F_GETFD), -1);
    }
    EXPECT_EQ(fcntl(raw, F_GETFD), -1);  // closed with its owner
    EXPECT_EQ(errno, EBADF);

    unique_fd fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
    const int released = fd.release();
    EXPECT_FALSE(fd.ok());
    EXPECT_NE(fcntl(released, F_GETFD), -1);  // not closed
    fd.reset(released);
    fd.reset();
    EXPECT_EQ(fcntl(released, F_GETFD), -1);

    unique_fd r, w;
    ASSERT_TRUE(android::base::Pipe(&r, &w));
    ASSERT_TRUE(android::base::WriteFully(w, "p", 1));
    char c = 0;
    ASSERT_TRUE(android::base::ReadFully(r, &c, 1));
    EXPECT_EQ(c, 'p');
}

TEST(File, LinksAndPaths) {
    compat_test::ScratchDir dir;
    ASSERT_EQ(symlink("target/x", (dir / "l").c_str()), 0);
    std::string out;
    ASSERT_TRUE(android::base::Readlink(dir / "l", &out));
    EXPECT_EQ(out, "target/x");
    ASSERT_TRUE(android::base::Realpath(dir.path() + "/./", &out));
    EXPECT_EQ(out.find("/./"), std::string::npos);
    const std::string exe = android::base::GetExecutablePath();
    EXPECT_EQ(android::base::Basename(exe), "base_test");
    EXPECT_EQ(android::base::GetExecutableDirectory(), android::base::Dirname(exe));

    std::string err;
    ASSERT_TRUE(android::base::WriteStringToFile("x", dir / "rm"));
    EXPECT_TRUE(android::base::RemoveFileIfExists(dir / "rm", &err));
    EXPECT_TRUE(android::base::RemoveFileIfExists(dir / "rm", &err));  // already gone: fine
    EXPECT_FALSE(android::base::RemoveFileIfExists(dir.path(), &err));  // a directory
}

TEST(File, BasenameAndDirname) {
    struct Case {
        const char* path;
        const char* base;
        const char* dir;
    };
    const Case cases[] = {
        {"", ".", "."},          {"/", "/", "/"},       {"a", "a", "."},
        {"/a", "a", "/"},        {"a/b", "b", "a"},     {"a/b/", "b", "a"},
        {"/a/b/c", "c", "/a/b"}, {"//a", "a", "/"},     {"a//b", "b", "a"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.path);
        EXPECT_EQ(android::base::Basename(c.path), c.base);
        EXPECT_EQ(android::base::Dirname(c.path), c.dir);
    }
}

TEST(Strings, SplitTrimJoin) {
    using V = std::vector<std::string>;
    EXPECT_EQ(android::base::Split("a,b,,c", ","), (V{"a", "b", "", "c"}));
    EXPECT_EQ(android::base::Split("", ","), (V{""}));
    EXPECT_EQ(android::base::Split("a b:c", " :"), (V{"a", "b", "c"}));
    EXPECT_EQ(android::base::Trim("  x y \t\n"), "x y");
    EXPECT_EQ(android::base::Trim(std::string("   ")), "");
    EXPECT_EQ(android::base::Join(V{"a", "b", "c"}, ','), "a,b,c");
    EXPECT_EQ(android::base::Join(std::set<std::string>{"x", "y"}, ", "), "x, y");
    EXPECT_EQ(android::base::Join(V{}, ","), "");
}

TEST(Strings, PrefixesAndCase) {
    EXPECT_TRUE(android::base::StartsWith("persist.x", "persist"));
    EXPECT_FALSE(android::base::StartsWith("pers", "persist"));
    EXPECT_TRUE(android::base::StartsWith("abc", 'a'));
    EXPECT_TRUE(android::base::EndsWith("file.json", ".json"));
    EXPECT_FALSE(android::base::EndsWith("json", ".json"));
    EXPECT_TRUE(android::base::EndsWith("abc", 'c'));
    EXPECT_TRUE(android::base::StartsWithIgnoreCase("HeLLo", "hell"));
    EXPECT_TRUE(android::base::EndsWithIgnoreCase("IMG.PNG", ".png"));
    EXPECT_TRUE(android::base::EqualsIgnoreCase("Nano", "nANO"));
    EXPECT_FALSE(android::base::EqualsIgnoreCase("Nano", "Nan"));
    std::string_view sv = "prefix-body-suffix";
    EXPECT_TRUE(android::base::ConsumePrefix(&sv, "prefix-"));
    EXPECT_TRUE(android::base::ConsumeSuffix(&sv, "-suffix"));
    EXPECT_FALSE(android::base::ConsumePrefix(&sv, "nope"));
    EXPECT_EQ(sv, "body");
    EXPECT_EQ(android::base::StringReplace("a.b.c", ".", "::", true), "a::b::c");
    EXPECT_EQ(android::base::StringReplace("a.b.c", ".", "::", false), "a::b.c");
}

TEST(Parse, IntAndUint) {
    int i = 0;
    EXPECT_TRUE(android::base::ParseInt("-12", &i));
    EXPECT_EQ(i, -12);
    EXPECT_TRUE(android::base::ParseInt(" 0x1f", &i));
    EXPECT_EQ(i, 31);
    EXPECT_TRUE(android::base::ParseInt("010", &i));
    EXPECT_EQ(i, 10);
    EXPECT_FALSE(android::base::ParseInt("-0x1", &i));
    EXPECT_FALSE(android::base::ParseInt("1x", &i));
    EXPECT_FALSE(android::base::ParseInt("", &i));
    EXPECT_FALSE(android::base::ParseInt("5", &i, 0, 4));
    EXPECT_EQ(errno, ERANGE);
    int8_t small = 0;
    EXPECT_FALSE(android::base::ParseInt("128", &small));
    EXPECT_TRUE(android::base::ParseInt("-128", &small));
    uint16_t u = 0;
    EXPECT_TRUE(android::base::ParseUint("65535", &u));
    EXPECT_FALSE(android::base::ParseUint("65536", &u));
    EXPECT_FALSE(android::base::ParseUint("-1", &u));
    uint64_t bytes = 0;
    EXPECT_TRUE(android::base::ParseByteCount("4k", &bytes));
    EXPECT_EQ(bytes, 4096u);
    EXPECT_FALSE(android::base::ParseUint("4k", &bytes));
}

TEST(Parse, Bool) {
    using android::base::ParseBool;
    using android::base::ParseBoolResult;
    for (const char* t : {"1", "y", "yes", "on", "true"}) EXPECT_TRUE(ParseBool(t) == ParseBoolResult::kTrue);
    for (const char* f : {"0", "n", "no", "off", "false"}) EXPECT_TRUE(ParseBool(f) == ParseBoolResult::kFalse);
    for (const char* e : {"", "True", "2", " yes"}) EXPECT_TRUE(ParseBool(e) == ParseBoolResult::kError);
}
