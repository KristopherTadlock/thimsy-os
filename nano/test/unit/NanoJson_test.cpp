#include "NanoJson.h"

#include <climits>
#include <cmath>
#include <string>

#include <gtest/gtest.h>

using android::njson::Value;
using android::njson::parse;
using android::njson::serialize;

namespace {
bool parses(const std::string& s) {
    Value v;
    return parse(s, &v);
}
}  // namespace

TEST(NanoJson, Grammar) {
    Value v;
    ASSERT_TRUE(parse(" {\"a\":[1,2.5,-3e2,true,false,null,\"x\"],\"b\":{}} ", &v));
    ASSERT_TRUE(v.isObject());
    const Value* a = v.find("a");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->arr.size(), 7u);
    EXPECT_EQ(a->arr[2].num, -300.0);
    EXPECT_TRUE(a->arr[3].b);
    EXPECT_TRUE(a->arr[5].isNull());
    EXPECT_TRUE(v.find("b")->isObject());
    EXPECT_EQ(v.getString("missing", "d"), "d");

    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "\"abc", "\"\\u12\"", "\"\\q\"",
                            "[1] x", "tru", "nul", "{1:2}", "[1 2]"})
        EXPECT_FALSE(parses(bad)) << bad;
}

TEST(NanoJson, Strings) {
    Value v;
    ASSERT_TRUE(parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", &v));
    EXPECT_EQ(v.str, "\"\\/\b\f\n\r\t");
    ASSERT_TRUE(parse("\"\\u00e9\\ud83d\\ude00\"", &v));
    EXPECT_EQ(v.str, "\xC3\xA9\xF0\x9F\x98\x80");
    ASSERT_TRUE(parse("{\"a\\u0000b\":1}", &v));
    EXPECT_EQ(v.obj[0].first.size(), 3u);
}

TEST(NanoJson, LoneSurrogatesBecomeReplacementCharacters) {
    Value v;
    ASSERT_TRUE(parse("\"\\ud800x\"", &v));
    EXPECT_EQ(v.str, "\xEF\xBF\xBDx");
    ASSERT_TRUE(parse("\"\\udc00\"", &v));
    EXPECT_EQ(v.str, "\xEF\xBF\xBD");
    // A high surrogate followed by an escape that is not a low one: both kept, the first replaced.
    ASSERT_TRUE(parse("\"\\ud800\\u0041\"", &v));
    EXPECT_EQ(v.str, "\xEF\xBF\xBD" "A");
}

TEST(NanoJson, Numbers) {
    Value v;
    for (const char* ok : {"0", "-0", "7", "-12", "1.5", "1e3", "1E-3", "-0.25e+2", "123456789012"})
        EXPECT_TRUE(parses(ok)) << ok;
    for (const char* bad : {"+1", "01", "-01", "1.", ".5", "-", "1e", "1e+", "0x10", "inf", "nan", "1-2", "--1"})
        EXPECT_FALSE(parses(bad)) << bad;
    ASSERT_TRUE(parse("-0.25e+2", &v));
    EXPECT_EQ(v.num, -25.0);
}

TEST(NanoJson, AsIntClamps) {
    Value v;
    ASSERT_TRUE(parse("[1e300,-1e300,42.9,-7.9,2147483647,1e999]", &v));
    EXPECT_EQ(v.arr[0].asInt(), INT_MAX);
    EXPECT_EQ(v.arr[1].asInt(), INT_MIN);
    EXPECT_EQ(v.arr[2].asInt(), 42);
    EXPECT_EQ(v.arr[3].asInt(), -7);
    EXPECT_EQ(v.arr[4].asInt(), INT_MAX);
    EXPECT_EQ(v.arr[5].asInt(), INT_MAX);   // strtod overflow gives inf
    EXPECT_EQ(Value::makeNumber(std::nan("")).asInt(5), 5);
    EXPECT_EQ(Value::makeString("3").asInt(9), 9);
}

TEST(NanoJson, DepthLimit) {
    const int max = android::njson::kMaxDepth;
    EXPECT_TRUE(parses(std::string(max, '[') + std::string(max, ']')));
    EXPECT_FALSE(parses(std::string(max + 1, '[') + std::string(max + 1, ']')));
    std::string objs;
    for (int i = 0; i < max + 1; i++) objs += "{\"a\":";
    objs += "1" + std::string(max + 1, '}');
    EXPECT_FALSE(parses(objs));
    // Far beyond the limit: refused without exhausting the stack.
    EXPECT_FALSE(parses(std::string(200000, '[')));
}

TEST(NanoJson, NonFiniteNumbersSerializeAsNull) {
    Value a = Value::makeArray();
    a.arr.push_back(Value::makeNumber(INFINITY));
    a.arr.push_back(Value::makeNumber(-INFINITY));
    a.arr.push_back(Value::makeNumber(std::nan("")));
    a.arr.push_back(Value::makeNumber(1e300));
    a.arr.push_back(Value::makeNumber(-3));
    const std::string out = serialize(a, false);
    EXPECT_EQ(out, "[null,null,null,1e+300,-3]");
    EXPECT_TRUE(parses(out));
}

TEST(NanoJson, RoundTrip) {
    const std::string text =
        "{\"name\":\"Caf\xC3\xA9 \\\"x\\\"\",\"list\":[1,-2,0.5,true,false,null,[]],"
        "\"obj\":{\"k\":\"\\n\\t\\u0001\"},\"empty\":{}}";
    Value v;
    ASSERT_TRUE(parse(text, &v));
    for (bool pretty : {false, true}) {
        const std::string out = serialize(v, pretty);
        Value w;
        ASSERT_TRUE(parse(out, &w)) << out;
        EXPECT_EQ(serialize(w, pretty), out);
    }
    EXPECT_EQ(serialize(v, false), text);
    // Insertion order is kept and set() replaces in place.
    Value o;
    o.set("b") = Value::makeNumber(1);
    o.set("a") = Value::makeNumber(2);
    o.set("b") = Value::makeString("x");
    EXPECT_EQ(serialize(o, false), "{\"b\":\"x\",\"a\":2}");
}
