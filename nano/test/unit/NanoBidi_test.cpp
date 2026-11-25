#include <gtest/gtest.h>

#include "NanoBidi.h"

using namespace android;

// Hebrew letters: alef, bet; shalom = shin lamed vav final-mem.
#define ALEF "\xd7\x90"
#define BET "\xd7\x91"
#define SHALOM "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"

TEST(Bidi, LtrTextIsUnchanged) {
  EXPECT_EQ(nanoBidiVisual("Hello (World) 123"), "Hello (World) 123");
  EXPECT_EQ(nanoBidiVisual(""), "");
  EXPECT_EQ(nanoBidiVisual("caf\xc3\xa9"), "caf\xc3\xa9");
}

TEST(Bidi, Direction) {
  EXPECT_FALSE(nanoTextIsRtl(""));
  EXPECT_FALSE(nanoTextIsRtl("123 abc"));
  EXPECT_TRUE(nanoTextIsRtl("123 " SHALOM));     // digits are neutral: the first strong letter decides
  EXPECT_FALSE(nanoTextIsRtl("abc " ALEF));
  EXPECT_TRUE(nanoTextHasRtl("abc " ALEF));
  EXPECT_TRUE(nanoTextHasRtl("abc \xd8\xb3"));   // Arabic seen
  EXPECT_FALSE(nanoTextHasRtl("abc 123"));
}

TEST(Bidi, HebrewRunIsReversed) {
  EXPECT_EQ(nanoBidiVisual(SHALOM), "\xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9");
}

TEST(Bidi, EmbeddedLatinAndDigitsKeepTheirOrder) {
  EXPECT_EQ(nanoBidiVisual(ALEF " 123 " BET), BET " 123 " ALEF);
  EXPECT_EQ(nanoBidiVisual(ALEF " abc " BET), BET " abc " ALEF);
}

TEST(Bidi, BracketsMirrorInsideRtl) {
  // Logical "alef(bet)" reads right to left, so the visual string opens with "(" again.
  EXPECT_EQ(nanoBidiVisual(ALEF "(" BET ")"), "(" BET ")" ALEF);
}

TEST(Bidi, ArabicJoiningForms) {
  // beh beh beh: final, medial, initial in visual (left-to-right) order.
  EXPECT_EQ(nanoBidiVisual("\xd8\xa8\xd8\xa8\xd8\xa8"), "\xef\xba\x90\xef\xba\x92\xef\xba\x91");
}

TEST(Bidi, LamAlefLigature) {
  EXPECT_EQ(nanoBidiVisual("\xd9\x84\xd8\xa7"), "\xef\xbb\xbb");
  // A zero-width non-joiner keeps them apart (isolated alef, isolated lam) and is itself dropped.
  EXPECT_EQ(nanoBidiVisual("\xd9\x84\xe2\x80\x8c\xd8\xa7"), "\xef\xba\x8d\xef\xbb\x9d");
}

TEST(Bidi, ZeroWidthAndDirectionMarksAreStripped) {
  EXPECT_EQ(nanoBidiVisual("a\xe2\x80\x8d" "b"), "ab");   // ZWJ
  EXPECT_EQ(nanoBidiVisual("a\xe2\x80\x8f" "b"), "ab");   // RLM
}

TEST(Bidi, MalformedInputDoesNotCrash) {
  for (const char* s : {"\xd7", "\xd8\xb3\xff", "\xe2\x80", "\xef\xbb", ALEF "\xc3", "\xf0\x9f", "\xff\xfe\xfd",
                        ALEF "\x80\x80\x80" BET}) {
    (void)nanoBidiVisual(s);
    (void)nanoTextIsRtl(s);
    (void)nanoTextHasRtl(s);
  }
}

// A stray lead byte ("\xc3" "A") or a truncated sequence at the end decodes as U+FFFD, so the
// character after it survives and no control character is produced.
TEST(Bidi, StrayLeadByteDoesNotEatTheNextCharacter) {
  const std::string v = nanoBidiVisual(ALEF "\xc3" "A");
  EXPECT_NE(v.find('A'), std::string::npos) << v;
  const std::string t = nanoBidiVisual(ALEF "\xc3");
  for (unsigned char c : t) EXPECT_GE(c, 0x20) << "control byte in output";
}
