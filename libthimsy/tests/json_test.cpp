// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/json.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace json = thimsy::json;

namespace {

json::Value parseOk(const std::string& text) {
    json::Value v;
    std::string err;
    EXPECT_TRUE(json::parse(text, &v, &err)) << text << ": " << err;
    return v;
}

std::string parseErr(const std::string& text) {
    json::Value v = "unchanged";
    std::string err;
    EXPECT_FALSE(json::parse(text, &v, &err)) << text;
    EXPECT_EQ(v.asString(), "unchanged");
    return err;
}

}  // namespace

TEST(Json, Scalars) {
    EXPECT_TRUE(parseOk("null").isNull());
    EXPECT_TRUE(parseOk(" true ").asBool());
    EXPECT_FALSE(parseOk("false").asBool(true));
    EXPECT_EQ(parseOk("42").asInt(), 42);
    EXPECT_TRUE(parseOk("42").isInt());
    EXPECT_EQ(parseOk("-0").asInt(), 0);
    EXPECT_DOUBLE_EQ(parseOk("1.5e2").asDouble(), 150.0);
    EXPECT_FALSE(parseOk("1.5e2").isInt());
    EXPECT_DOUBLE_EQ(parseOk("-0.125").asDouble(), -0.125);
    EXPECT_EQ(parseOk("\"hi\"").asString(), "hi");
    EXPECT_EQ(parseOk("9223372036854775807").asInt(), INT64_MAX);
    EXPECT_EQ(parseOk("-9223372036854775808").asInt(), INT64_MIN);
    const json::Value big = parseOk("9223372036854775808");
    EXPECT_FALSE(big.isInt());
    EXPECT_DOUBLE_EQ(big.asDouble(), 9223372036854775808.0);
    EXPECT_EQ(parseOk("1").asString(), "");  // typed reads fall back
    EXPECT_EQ(parseOk("\"1\"").asInt(7), 7);
}

TEST(Json, DeviceJsonShape) {
    const json::Value d = parseOk(R"({
        "id": "anbernic-rg-arc-d", "family": "rk3566", "model": "Anbernic RG ARC-D", "ram_mb": 2048,
        "panel": {"width": 480, "height": 640, "rotation": 90, "rotation_source": "drm"},
        "pad": {"producer": "padd", "face_buttons": 6, "sticks": false,
                "sources": [{"name": "gpio-keys-control"}, {"name": "adc-keys-home"}]},
        "status": "boots"
    })");
    ASSERT_TRUE(d.isObject());
    EXPECT_EQ(d["id"].asString(), "anbernic-rg-arc-d");
    EXPECT_EQ(d["panel"]["rotation"].asInt(), 90);
    EXPECT_EQ(d["pad"]["sources"].size(), 2u);
    EXPECT_EQ(d["pad"]["sources"][1]["name"].asString(), "adc-keys-home");
    EXPECT_TRUE(d["pad"]["sources"][7].isNull());
    EXPECT_TRUE(d["missing"]["deeper"].isNull());
    EXPECT_FALSE(d["pad"]["sticks"].asBool(true));
    ASSERT_EQ(d.members().size(), 7u);  // file order kept
    EXPECT_EQ(d.members()[0].key, "id");
    EXPECT_EQ(d.members()[6].key, "status");
}

TEST(Json, UnicodeEscapes) {
    EXPECT_EQ(parseOk(R"("caf\u00e9")").asString(), "caf\xc3\xa9");
    EXPECT_EQ(parseOk(R"("\u20AC")").asString(), "\xe2\x82\xac");
    EXPECT_EQ(parseOk(R"("\ud83d\ude00")").asString(), "\xf0\x9f\x98\x80");  // U+1F600, a surrogate pair
    EXPECT_EQ(parseOk(R"("\u0000x")").asString(), std::string("\0x", 2));
    EXPECT_EQ(parseOk(R"("\"\\\/\b\f\n\r\t")").asString(), "\"\\/\b\f\n\r\t");
    EXPECT_EQ(parseOk("\"\xe3\x83\x86\xe3\x82\xa3\xe3\x83\xa0\"").asString(), "\xe3\x83\x86\xe3\x82\xa3\xe3\x83\xa0");
}

TEST(Json, Errors) {
    struct Case {
        const char* text;
        const char* why;
    };
    const Case cases[] = {
        {"", "unexpected end"},
        {"[1,2,]", "trailing comma"},
        {"{\"a\":1,}", "trailing comma"},
        {"// c\n1", "unexpected character"},
        {"{'a':1}", "expected a string key"},
        {"\"\\ud800\"", "lone high surrogate"},
        {"\"\\ud800\\u0041\"", "lone high surrogate"},
        {"\"\\udc00\"", "lone low surrogate"},
        {"\"\\u12g4\"", "bad hex digit"},
        {"\"a\nb\"", "control character"},
        {"\"\xc0\x80\"", "invalid UTF-8"},
        {"\"\xff\"", "invalid UTF-8"},
        {"\"\xed\xa0\x80\"", "invalid UTF-8"},
        {"\"\xe2\x82\"", "invalid UTF-8"},
        {"01", "leading zero"},
        {"1.", "bad fraction"},
        {".5", "unexpected character"},
        {"-", "bad number"},
        {"1e", "bad exponent"},
        {"1e999", "out of range"},
        {"{\"a\":1,\"a\":2}", "duplicate key"},
        {"\"open", "unterminated string"},
        {"[1 2]", "expected ',' or ']'"},
        {"{\"a\" 1}", "expected ':'"},
        {"1 2", "after the document"},
        {"nul", "invalid literal"},
        {"\"\\x\"", "invalid escape"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        const std::string err = parseErr(c.text);
        EXPECT_NE(err.find(c.why), std::string::npos) << err;
    }
}

TEST(Json, ErrorPositionAndDepth) {
    EXPECT_EQ(parseErr("{\n  \"a\": [1,\n  ]\n}").rfind("line 3, column 3", 0), 0u);
    std::string deep(300, '[');
    deep += std::string(300, ']');
    EXPECT_NE(parseErr(deep).find("nesting too deep"), std::string::npos);
    std::string ok(200, '[');
    ok += std::string(200, ']');
    parseOk(ok);
}

TEST(Json, WriteCompactAndPretty) {
    json::Value v = json::Value::object();
    v.set("b", 1);
    v.set("a", json::Value::array());
    json::Value arr = json::Value::array();
    arr.push(true);
    arr.push(nullptr);
    arr.push(2.5);
    arr.push("s");
    v.set("a", arr);  // replaces in place, keeping the order
    v.set("e", json::Value::object());
    EXPECT_EQ(json::write(v), R"({"b":1,"a":[true,null,2.5,"s"],"e":{}})");
    json::WriteOptions pretty;
    pretty.indent = 2;
    EXPECT_EQ(json::write(v, pretty), "{\n  \"b\": 1,\n  \"a\": [\n    true,\n    null,\n    2.5,\n    \"s\"\n  ],\n  \"e\": {}\n}");
}

TEST(Json, WriteEscapes) {
    EXPECT_EQ(json::write(json::Value(std::string("q\"b\\n\n\x01\x7f", 8))), R"("q\"b\\n\n\u0001\u007f")");
    EXPECT_EQ(json::write(json::Value("caf\xc3\xa9 \xf0\x9f\x98\x80")), "\"caf\xc3\xa9 \xf0\x9f\x98\x80\"");
    json::WriteOptions ascii;
    ascii.asciiOnly = true;
    EXPECT_EQ(json::write(json::Value("caf\xc3\xa9 \xf0\x9f\x98\x80"), ascii), R"("caf\u00e9 \ud83d\ude00")");
    EXPECT_EQ(json::write(json::Value("bad\xff"), ascii), R"("bad\ufffd")");
    EXPECT_EQ(json::write(json::Value("/")), "\"/\"");
}

TEST(Json, Numbers) {
    EXPECT_EQ(json::write(json::Value(INT64_MIN)), "-9223372036854775808");
    EXPECT_EQ(json::write(json::Value(0.1)), "0.1");
    EXPECT_EQ(json::write(json::Value(1e21)), "1e+21");
    EXPECT_EQ(json::write(json::Value(std::nan(""))), "null");
    EXPECT_EQ(json::write(json::Value(std::numeric_limits<double>::infinity())), "null");
    const double tricky = 0.1 + 0.2;
    EXPECT_EQ(parseOk(json::write(json::Value(tricky))).asDouble(), tricky);  // shortest round trip
    EXPECT_EQ(json::write(json::Value(3u)), "3");
    EXPECT_EQ(json::write(json::Value(static_cast<int8_t>(-3))), "-3");
}

TEST(Json, RoundTrip) {
    const std::string text =
        R"({"id":"x","n":[1,-2,3.25,1e-07],"o":{"k":"\u00e9\ud83d\ude00","t":true,"f":false,"z":null},"s":"a\tb"})";
    const json::Value v = parseOk(text);
    const std::string out = json::write(v);
    EXPECT_EQ(parseOk(out), v);
    json::WriteOptions pretty;
    pretty.indent = 4;
    EXPECT_EQ(parseOk(json::write(v, pretty)), v);
    json::WriteOptions ascii;
    ascii.asciiOnly = true;
    EXPECT_EQ(parseOk(json::write(v, ascii)), v);
    EXPECT_NE(parseOk(R"({"a":1})"), parseOk(R"({"a":2})"));
    EXPECT_NE(parseOk(R"({"a":1,"b":2})"), parseOk(R"({"b":2,"a":1})"));  // order matters
    EXPECT_EQ(parseOk("1"), parseOk("1.0"));
}
