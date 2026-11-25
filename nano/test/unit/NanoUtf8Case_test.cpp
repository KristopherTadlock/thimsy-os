#include <gtest/gtest.h>

#include "NanoUtf8Case.h"

using namespace android;

TEST(Utf8Case, Latin) {
  EXPECT_EQ(nanoUtf8Upper("hello, World 42"), "HELLO, WORLD 42");
  EXPECT_EQ(nanoUtf8Lower("\xc3\x80\xc3\x89\xc3\x8e"), "\xc3\xa0\xc3\xa9\xc3\xae");   // ÀÉÎ
  EXPECT_EQ(nanoUtf8Upper("\xe1\xba\xa1"), "\xe1\xba\xa0");                         // Vietnamese ạ
}

TEST(Utf8Case, SharpSBecomesSS) {
  EXPECT_EQ(nanoUtf8Upper("stra\xc3\x9f" "e"), "STRASSE");
  EXPECT_EQ(nanoUtf8Lower("STRASSE"), "strasse");
}

TEST(Utf8Case, GreekFinalSigma) {
  // ΟΔΟΣ ΟΔΟΣ: a sigma ending a word lowers to ς, elsewhere to σ.
  EXPECT_EQ(nanoUtf8Lower("\xce\x9f\xce\x94\xce\x9f\xce\xa3 \xce\x9f\xce\x94\xce\x9f\xce\xa3"),
            "\xce\xbf\xce\xb4\xce\xbf\xcf\x82 \xce\xbf\xce\xb4\xce\xbf\xcf\x82");
  EXPECT_EQ(nanoUtf8Lower("\xce\xa3\xce\x91"), "\xcf\x83\xce\xb1");   // ΣΑ -> σα
  EXPECT_EQ(nanoUtf8Upper("\xcf\x82"), "\xce\xa3");                   // ς -> Σ
}

TEST(Utf8Case, TurkishIIsLocaleNeutral) {
  EXPECT_EQ(nanoUtf8Upper("i"), "I");
  EXPECT_EQ(nanoUtf8Lower("I"), "i");
  EXPECT_EQ(nanoUtf8Upper("\xc4\xb1"), "I");   // dotless ı
  EXPECT_EQ(nanoUtf8Lower("\xc4\xb0"), "i");   // dotted İ
}

TEST(Utf8Case, OtherScripts) {
  EXPECT_EQ(nanoUtf8Upper("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"),
            "\xd0\x9f\xd0\xa0\xd0\x98\xd0\x92\xd0\x95\xd0\xa2");   // привет
  EXPECT_EQ(nanoUtf8Upper("\xd5\xa1"), "\xd4\xb1");                 // Armenian
  EXPECT_EQ(nanoUtf8Upper("\xef\xbd\x81"), "\xef\xbc\xa1");         // fullwidth a
  EXPECT_EQ(nanoUtf8Upper("\xe6\x97\xa5\xe6\x9c\xac"), "\xe6\x97\xa5\xe6\x9c\xac");   // CJK unchanged
}

TEST(Utf8Case, CodePoints) {
  EXPECT_EQ(nanoToUpperCp('a'), (uint32_t)'A');
  EXPECT_EQ(nanoToLowerCp('Z'), (uint32_t)'z');
  EXPECT_EQ(nanoToUpperCp('1'), (uint32_t)'1');
  EXPECT_EQ(nanoToLowerCp(0x0410), 0x0430u);   // А -> а
}

TEST(Utf8Case, Capitalize) {
  EXPECT_EQ(nanoUtf8Capitalize("hello WORLD"), "Hello World");
  EXPECT_EQ(nanoUtf8Capitalize("\xc3\xa9" "COLE DES \xce\x9f\xce\x94\xce\x9f\xce\xa3"),
            "\xc3\x89" "cole Des \xce\x9f\xce\xb4\xce\xbf\xcf\x82");
  EXPECT_EQ(nanoUtf8Capitalize(""), "");
}

TEST(Utf8Case, MalformedBytesPassThrough) {
  EXPECT_EQ(nanoUtf8Upper("a\xff" "b\xc3"), "A\xff" "B\xc3");
  const std::string bad[] = {"\xff", "\xc3", "\xe2\x82", "\xf0\x9f\x98", std::string("a\0b", 3), "\x80\x80"};
  for (const auto& s : bad) {
    (void)nanoUtf8Upper(s);
    (void)nanoUtf8Lower(s);
    (void)nanoUtf8Capitalize(s);
  }
  EXPECT_EQ(nanoUtf8Upper(std::string("a\0b", 3)), std::string("A\0B", 3));
}
