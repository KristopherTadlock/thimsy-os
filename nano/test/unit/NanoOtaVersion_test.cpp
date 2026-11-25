#include "NanoOtaCheck.h"

#include <gtest/gtest.h>

using nano::otaCompareVersions;

TEST(OtaVersion, ComparesDottedPartsNumerically) {
    EXPECT_GT(otaCompareVersions("1.10", "1.9"), 0);
    EXPECT_LT(otaCompareVersions("1.9", "1.10"), 0);
    EXPECT_EQ(otaCompareVersions("1.2", "1.2"), 0);
    EXPECT_EQ(otaCompareVersions("1.2", "1.2.0"), 0);
    EXPECT_EQ(otaCompareVersions("01.002", "1.2"), 0);
    EXPECT_GT(otaCompareVersions("2", "1.99.99"), 0);
    EXPECT_GT(otaCompareVersions("1.3.1", "1.3"), 0);
    EXPECT_GT(otaCompareVersions("1.0b", "1.0a"), 0);    // a suffix breaks a tie as text
    EXPECT_GT(otaCompareVersions("1.10a", "1.9b"), 0);   // but only after the number
    EXPECT_GT(otaCompareVersions("1.0a", "1.0"), 0);
    EXPECT_EQ(otaCompareVersions("", ""), 0);
    EXPECT_GT(otaCompareVersions("1", ""), 0);
    // Parts longer than any integer type compare by their digits.
    EXPECT_GT(otaCompareVersions("1.123456789012345678901234567890", "1.99999999999999999999"), 0);
}
