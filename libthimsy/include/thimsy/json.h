// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// A small, strict JSON (RFC 8259) reader and writer for device.json, install.json and volumes.json.
// Objects keep their members in file order. The reader rejects what the RFC does not allow:
// comments, trailing commas, single quotes, control characters in strings, invalid UTF-8, lone
// surrogates in \u escapes, leading zeros, duplicate keys, and nesting deeper than 256.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace thimsy {
namespace json {

struct Member;

class Value {
 public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), num_(d) {}
    template <class T, typename std::enable_if<std::is_integral<T>::value &&
                                                   !std::is_same<T, bool>::value, int>::type = 0>
    Value(T i) : type_(Type::Number), num_(static_cast<double>(i)), int_(static_cast<int64_t>(i)),
                 isInt_(true) {}
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), str_(s) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isInt() const { return type_ == Type::Number && isInt_; }  // integral and fits int64
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Typed reads; the default when the type differs.
    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asDouble(double def = 0) const { return isNumber() ? num_ : def; }
    int64_t asInt(int64_t def = 0) const;  // integral numbers only
    const std::string& asString() const;   // empty unless a string
    std::string asString(std::string_view def) const { return isString() ? str_ : std::string(def); }

    // Arrays.
    size_t size() const;  // elements or members; 0 for scalars
    const std::vector<Value>& elements() const;  // empty unless an array
    const Value& operator[](size_t i) const;     // a null Value when out of range
    void push(Value v);  // turns a null into an array

    // Objects.
    const std::vector<Member>& members() const;  // empty unless an object
    const Value* find(std::string_view key) const;
    const Value& operator[](std::string_view key) const;  // a null Value when missing
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    Value& set(std::string_view key, Value v);  // replaces or appends; turns a null into an object

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }

 private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    bool isInt_ = false;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<Member> obj_;
};

struct Member {
    std::string key;
    Value value;
};

// Parses a whole document. On failure returns false and sets *err to "line L, column C: why".
bool parse(std::string_view text, Value* out, std::string* err = nullptr);

struct WriteOptions {
    int indent = 0;          // 0: compact; n: pretty, n spaces per level, "key": value
    bool asciiOnly = false;  // \u-escape everything outside ASCII (surrogate pairs above U+FFFF)
};

// Writes a document. Non-finite numbers are written as null. Strings are expected to be UTF-8;
// with asciiOnly, invalid bytes are written as U+FFFD.
std::string write(const Value& v, const WriteOptions& opts = {});

}  // namespace json
}  // namespace thimsy
