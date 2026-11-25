// NanoJson - a small, dependency-free JSON reader/writer for GammaOS Nano.
//
// gammaos-nano runs in the bootanim domain at early boot and must stay light
// and exception-free, so we avoid pulling a heavy parser (libjsoncpp) into the
// binary. This is a complete recursive-descent JSON parser plus a serializer
// over a tiny ordered DOM. Objects preserve key insertion order so serialized
// configs stay stable and human-diffable, and so we can mirror the Daijishou
// portable key ordering when importing/exporting platform JSONs.
//
// The parser implements the full JSON grammar (objects, arrays, strings with
// \uXXXX and surrogate-pair escapes, numbers, true/false/null), returns false
// on any malformed input or nesting deeper than kMaxDepth, and never throws.
#pragma once

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace njson {

enum class Type { Null, Bool, Number, String, Array, Object };

// Inf/NaN tests on the bits: gammaos-nano builds with -ffast-math, under which std::isnan and
// std::isfinite may be folded to constants.
inline bool nonFinite(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return ((bits >> 52) & 0x7FF) == 0x7FF;
}
inline bool isNaN(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return nonFinite(d) && (bits & 0xFFFFFFFFFFFFFull) != 0;
}

// Deeper arrays/objects are a parse error: the parser, the destructor and serialize() recurse once
// per level, and a few thousand "[" would otherwise exhaust the stack.
constexpr int kMaxDepth = 256;

class Value {
public:
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    // Object members kept in insertion order (small configs; linear lookup).
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;

    bool isNull()   const { return type == Type::Null; }
    bool isBool()   const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    // Object lookup; returns nullptr if not an object or key absent.
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // Object set-or-create. Promotes this value to an object if needed.
    Value& set(const std::string& key) {
        if (type != Type::Object) { *this = Value(); type = Type::Object; }
        for (auto& kv : obj)
            if (kv.first == key) return kv.second;
        obj.emplace_back(key, Value());
        return obj.back().second;
    }

    // Typed accessors with defaults (never throw).
    std::string asString(const std::string& def = "") const {
        return type == Type::String ? str : def;
    }
    double asNumber(double def = 0.0) const {
        if (type == Type::Number) return num;
        if (type == Type::Bool)   return b ? 1.0 : 0.0;
        return def;
    }
    // Clamped to the int range (a plain cast of 1e300 or NaN is undefined); NaN gives def.
    int asInt(int def = 0) const {
        if (type != Type::Number || isNaN(num)) return def;
        if (num >= (double)INT_MAX) return INT_MAX;
        if (num <= (double)INT_MIN) return INT_MIN;
        return (int)num;
    }
    bool asBool(bool def = false) const {
        if (type == Type::Bool)   return b;
        if (type == Type::Number) return num != 0.0;
        return def;
    }

    std::string getString(const std::string& key, const std::string& def = "") const {
        const Value* v = find(key); return v ? v->asString(def) : def;
    }
    int getInt(const std::string& key, int def = 0) const {
        const Value* v = find(key); return v ? v->asInt(def) : def;
    }
    bool getBool(const std::string& key, bool def = false) const {
        const Value* v = find(key); return v ? v->asBool(def) : def;
    }

    static Value makeObject() { Value v; v.type = Type::Object; return v; }
    static Value makeArray()  { Value v; v.type = Type::Array;  return v; }
    static Value makeString(const std::string& s) { Value v; v.type = Type::String; v.str = s; return v; }
    static Value makeNumber(double n) { Value v; v.type = Type::Number; v.num = n; return v; }
    static Value makeBool(bool x)     { Value v; v.type = Type::Bool;   v.b = x;  return v; }
};

// Parse JSON text into *out. Returns true on success, false on any syntax
// error (out may be left partially populated; callers should discard on false).
bool parse(const std::string& text, Value* out);

// Serialize a Value to JSON text. pretty => 2-space indented, one member/element
// per line; otherwise compact. A non-finite number (no JSON form) is written as null.
std::string serialize(const Value& v, bool pretty = true);

} // namespace njson
} // namespace android
