// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <string>

#include "core/Requests.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

TEST(Requests, PlainAndNumbered) {
    Request r;
    ASSERT_TRUE(parseRequest("sleep", &r));
    EXPECT_EQ(r.verb, Verb::Sleep);
    EXPECT_EQ(r.seq, "");
    ASSERT_TRUE(parseRequest("17:eject:9602-C755", &r));
    EXPECT_EQ(r.verb, Verb::Eject);
    EXPECT_EQ(r.seq, "17");
    EXPECT_EQ(r.arg, "9602-C755");
    ASSERT_TRUE(parseRequest("3:quit-game\n", &r));
    EXPECT_EQ(r.verb, Verb::QuitGame);
    ASSERT_TRUE(parseRequest("brightness:40", &r));
    EXPECT_EQ(r.arg, "40");
    ASSERT_TRUE(parseRequest("9:restart-nano", &r));
    EXPECT_EQ(r.verb, Verb::RestartNano);
    ASSERT_TRUE(parseRequest("scrape", &r));
    ASSERT_TRUE(parseRequest("poweroff", &r));
    ASSERT_TRUE(parseRequest("reboot", &r));
    ASSERT_TRUE(parseRequest("21:take-ownership:0b1c2d3e-0000-4000-8000-00000000abcd", &r));
    EXPECT_EQ(r.verb, Verb::TakeOwnership);
    EXPECT_EQ(r.seq, "21");
    EXPECT_EQ(r.arg, "0b1c2d3e-0000-4000-8000-00000000abcd");
    EXPECT_STREQ(toString(r.verb), "take-ownership");
    ASSERT_TRUE(parseRequest("22:keep-read-only:0b1c2d3e-0000-4000-8000-00000000abcd", &r));
    EXPECT_EQ(r.verb, Verb::KeepReadOnly);
    EXPECT_EQ(r.arg, "0b1c2d3e-0000-4000-8000-00000000abcd");
    EXPECT_STREQ(toString(r.verb), "keep-read-only");
}

TEST(Requests, RefusesBadOnes) {
    Request r;
    std::string err;
    EXPECT_FALSE(parseRequest("dance", &r, &err));
    EXPECT_NE(err.find("unknown"), std::string::npos);
    EXPECT_FALSE(parseRequest("eject", &r, &err));
    EXPECT_FALSE(parseRequest("eject:", &r, &err));
    EXPECT_FALSE(parseRequest("sleep:now", &r, &err));
    EXPECT_FALSE(parseRequest("brightness:101", &r, &err));
    EXPECT_FALSE(parseRequest("brightness:-1", &r, &err));
    EXPECT_FALSE(parseRequest("eject:../../etc", &r, &err));
    EXPECT_FALSE(parseRequest("take-ownership", &r, &err));
    EXPECT_FALSE(parseRequest("take-ownership:", &r, &err));
    EXPECT_FALSE(parseRequest("take-ownership:../../etc", &r, &err));
    EXPECT_FALSE(parseRequest("take-ownership:a b", &r, &err));
    EXPECT_FALSE(parseRequest("keep-read-only", &r, &err));
    EXPECT_FALSE(parseRequest("keep-read-only:../x", &r, &err));
    EXPECT_FALSE(parseRequest("", &r, &err));
}

TEST(Requests, Powerctl) {
    Request r;
    ASSERT_TRUE(parsePowerctl("shutdown", &r));
    EXPECT_EQ(r.verb, Verb::PowerOff);
    ASSERT_TRUE(parsePowerctl("shutdown,userrequested", &r));
    EXPECT_EQ(r.verb, Verb::PowerOff);
    ASSERT_TRUE(parsePowerctl("reboot,recovery", &r));
    EXPECT_EQ(r.verb, Verb::Reboot);
    EXPECT_FALSE(parsePowerctl("sleep", &r));
}

TEST(Requests, DoneValues) {
    Request r;
    ASSERT_TRUE(parseRequest("17:eject:9602-C755", &r));
    EXPECT_EQ(doneValue(r, "ok"), "17:ok");
    ASSERT_TRUE(parseRequest("quit-game", &r));
    EXPECT_EQ(doneValue(r, "error:no game"), "quit-game:error:no game");
    EXPECT_EQ(doneValue(r, std::string(200, 'x')).size(), 91u);
}

TEST(Requests, ARepeatedSequenceNumberIsDropped) {
    RequestFilter f;
    Request a, b, c;
    ASSERT_TRUE(parseRequest("5:sleep", &a));
    ASSERT_TRUE(parseRequest("6:sleep", &b));
    ASSERT_TRUE(parseRequest("sleep", &c));
    EXPECT_TRUE(f.accept(a));
    EXPECT_FALSE(f.accept(a));
    EXPECT_TRUE(f.accept(b));
    EXPECT_TRUE(f.accept(c));
    EXPECT_TRUE(f.accept(c));
}
