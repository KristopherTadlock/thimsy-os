#include "NanoI18n.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

using android::i18nFormatSafe;
using android::i18nLoad;
using android::i18nLoadFromText;
using android::trDyn;

TEST(NanoI18n, FormatSafeTable) {
    struct Case { const char* key; const char* value; bool safe; };
    const Case cases[] = {
        {"Hello", "Hallo", true},
        {"50 % done", "50 % fertig", true},              // no conversions: '%' is literal
        {"%d files", "%d Dateien", true},
        {"%d files", "%s Dateien", false},               // type changed
        {"%d files", "Dateien", false},                  // conversion dropped
        {"Files", "%s Dateien", false},                  // conversion added to plain text
        {"%s of %s", "%s von %s", true},
        {"%s of %d", "%d von %s", false},                // order matters
        {"%1$s of %2$s", "%1$s von %2$s", true},
        {"%.1f MB", "%.1f Mo", true},
        {"%.1f MB", "%f Mo", false},
        {"%lld B", "%lld o", true},
        {"%lld B", "%d o", false},
        {"%d%%", "%d %%", true},
        {"%d%%", "%d%", true},                           // a lone '%' at the end is not a conversion
        {"% d", "% d", true},
        {"%d", "% d", false},                            // the space flag counts in a format
        {"%-5s|", "%-5s|", true},
    };
    for (const Case& c : cases) EXPECT_EQ(i18nFormatSafe(c.key, c.value), c.safe) << c.key << " -> " << c.value;
}

TEST(NanoI18n, EnglishAndMissingLocalesPassThrough) {
    const char* key = "Hello %s";
    i18nLoad("en");
    EXPECT_EQ(trDyn(key), key);
    i18nLoad(nullptr);
    EXPECT_EQ(trDyn(key), key);
    i18nLoad("xx-nonexistent");   // no file on the host: passthrough, not a crash
    EXPECT_EQ(trDyn(key), key);
    EXPECT_EQ(trDyn(nullptr), nullptr);
    EXPECT_STREQ(trDyn(""), "");
}

TEST(NanoI18n, LoadFromTextFiltersUnsafeTranslations) {
    i18nLoadFromText("t-filter", "{\"Hello\":\"Hallo\",\"%d files\":\"%s Dateien\",\"Empty\":\"\",\"Num\":3}");
    EXPECT_STREQ(trDyn("Hello"), "Hallo");
    EXPECT_STREQ(trDyn("%d files"), "%d files");
    EXPECT_STREQ(trDyn("Empty"), "Empty");
    EXPECT_STREQ(trDyn("Num"), "Num");
    i18nLoadFromText("t-broken", "{not json");
    EXPECT_STREQ(trDyn("Hello"), "Hello");
    i18nLoad("en");
}

TEST(NanoI18n, PointersStayValidAfterALocaleSwitch) {
    i18nLoadFromText("t-one", "{\"Back\":\"Zur\xC3\xBC" "ck\",\"Save\":\"Speichern\"}");
    const char* back = trDyn("Back");
    ASSERT_STREQ(back, "Zur\xC3\xBC" "ck");
    i18nLoadFromText("t-two", "{\"Back\":\"Retour\"}");
    EXPECT_STREQ(trDyn("Back"), "Retour");
    i18nLoad("en");
    EXPECT_STREQ(trDyn("Back"), "Back");
    EXPECT_STREQ(back, "Zur\xC3\xBC" "ck");
    // Even a language that is loaded again keeps the strings handed out before.
    i18nLoadFromText("t-one", "{\"Back\":\"Anders\"}");
    EXPECT_STREQ(trDyn("Back"), "Anders");
    EXPECT_STREQ(back, "Zur\xC3\xBC" "ck");
    i18nLoad("en");
}
