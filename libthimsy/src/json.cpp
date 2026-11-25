// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <locale>
#include <sstream>

namespace thimsy {
namespace json {

namespace {

const Value& nullValue() {
    static const Value v;
    return v;
}

const std::string& emptyString() {
    static const std::string s;
    return s;
}

void appendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes one UTF-8 sequence at s[*i]; returns the code point and advances *i, or returns
// 0xFFFFFFFF (and advances one byte) for an invalid, overlong or surrogate sequence.
uint32_t decodeUtf8(std::string_view s, size_t* i) {
    const auto b0 = static_cast<unsigned char>(s[*i]);
    if (b0 < 0x80) {
        ++*i;
        return b0;
    }
    int n;
    uint32_t cp, min;
    if ((b0 & 0xE0) == 0xC0) {
        n = 1; cp = b0 & 0x1F; min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 2; cp = b0 & 0x0F; min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 3; cp = b0 & 0x07; min = 0x10000;
    } else {
        ++*i;
        return 0xFFFFFFFF;
    }
    for (int k = 1; k <= n; ++k) {
        if (*i + static_cast<size_t>(k) >= s.size()) {
            ++*i;
            return 0xFFFFFFFF;
        }
        const auto b = static_cast<unsigned char>(s[*i + static_cast<size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            ++*i;
            return 0xFFFFFFFF;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++*i;
        return 0xFFFFFFFF;
    }
    *i += static_cast<size_t>(n) + 1;
    return cp;
}

}  // namespace

// ---- Value -------------------------------------------------------------------------------------

int64_t Value::asInt(int64_t def) const { return isInt() ? int_ : def; }

const std::string& Value::asString() const { return isString() ? str_ : emptyString(); }

size_t Value::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const std::vector<Value>& Value::elements() const {
    static const std::vector<Value> empty;
    return type_ == Type::Array ? arr_ : empty;
}

const Value& Value::operator[](size_t i) const {
    return type_ == Type::Array && i < arr_.size() ? arr_[i] : nullValue();
}

void Value::push(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ == Type::Array) arr_.push_back(std::move(v));
}

const std::vector<Member>& Value::members() const {
    static const std::vector<Member> empty;
    return type_ == Type::Object ? obj_ : empty;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const Member& m : obj_) {
        if (m.key == key) return &m.value;
    }
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : nullValue();
}

Value& Value::set(std::string_view key, Value v) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (Member& m : obj_) {
        if (m.key == key) {
            m.value = std::move(v);
            return m.value;
        }
    }
    obj_.push_back(Member{std::string(key), std::move(v)});
    return obj_.back().value;
}

bool Value::operator==(const Value& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number:
            if (isInt_ && o.isInt_) return int_ == o.int_;
            return num_ == o.num_;
        case Type::String: return str_ == o.str_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object:
            if (obj_.size() != o.obj_.size()) return false;
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (obj_[i].key != o.obj_[i].key || obj_[i].value != o.obj_[i].value) return false;
            }
            return true;
    }
    return false;
}

// ---- Parser ------------------------------------------------------------------------------------

class Parser {
 public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool parseDocument(Value* out, std::string* err) {
        skipWs();
        Value v;
        if (!parseValue(&v, 0)) return report(err);
        skipWs();
        if (pos_ != s_.size()) {
            why_ = "unexpected text after the document";
            return report(err);
        }
        *out = std::move(v);
        return true;
    }

 private:
    static constexpr int kMaxDepth = 256;

    bool report(std::string* err) {
        if (err) {
            size_t line = 1, col = 1;
            for (size_t i = 0; i < errPos() && i < s_.size(); ++i) {
                if (s_[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            *err = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + why_;
        }
        return false;
    }
    size_t errPos() const { return pos_; }

    bool fail(const char* why) {
        why_ = why;
        return false;
    }

    void skipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(const char* word) {
        const size_t n = strlen(word);
        if (s_.substr(pos_, n) != word) return fail("invalid literal");
        pos_ += n;
        return true;
    }

    bool parseValue(Value* out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        const char c = s_[pos_];
        switch (c) {
            case 'n': if (!literal("null")) return false; *out = Value(); return true;
            case 't': if (!literal("true")) return false; *out = Value(true); return true;
            case 'f': if (!literal("false")) return false; *out = Value(false); return true;
            case '"': {
                std::string str;
                if (!parseString(&str)) return false;
                *out = Value(std::move(str));
                return true;
            }
            case '[': return parseArray(out, depth);
            case '{': return parseObject(out, depth);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
                return fail("unexpected character");
        }
    }

    bool parseArray(Value* out, int depth) {
        ++pos_;  // '['
        Value arr = Value::array();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            *out = std::move(arr);
            return true;
        }
        for (;;) {
            skipWs();
            Value v;
            if (!parseValue(&v, depth + 1)) return false;
            arr.arr_.push_back(std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') {
                ++pos_;
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ']') return fail("trailing comma in array");
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                *out = std::move(arr);
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Value* out, int depth) {
        ++pos_;  // '{'
        Value obj = Value::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            *out = std::move(obj);
            return true;
        }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] != '"') return fail(s_[pos_] == '}' ? "trailing comma in object" : "expected a string key");
            const size_t keyPos = pos_;
            std::string key;
            if (!parseString(&key)) return false;
            if (obj.find(key)) {
                pos_ = keyPos;
                return fail("duplicate key");
            }
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skipWs();
            Value v;
            if (!parseValue(&v, depth + 1)) return false;
            obj.obj_.push_back(Member{std::move(key), std::move(v)});
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                *out = std::move(obj);
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool hex4(uint32_t* out) {
        if (pos_ + 4 > s_.size()) return fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + static_cast<size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        pos_ += 4;
        *out = v;
        return true;
    }

    bool parseString(std::string* out) {
        ++pos_;  // opening quote
        std::string str;
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                *out = std::move(str);
                return true;
            }
            if (c < 0x20) return fail("control character in string");
            if (c == '\\') {
                if (++pos_ >= s_.size()) return fail("unterminated escape");
                const char e = s_[pos_++];
                switch (e) {
                    case '"': str.push_back('"'); break;
                    case '\\': str.push_back('\\'); break;
                    case '/': str.push_back('/'); break;
                    case 'b': str.push_back('\b'); break;
                    case 'f': str.push_back('\f'); break;
                    case 'n': str.push_back('\n'); break;
                    case 'r': str.push_back('\r'); break;
                    case 't': str.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!hex4(&cp)) return false;
                        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate in \\u escape");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            uint32_t lo;
                            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                                return fail("lone high surrogate in \\u escape");
                            }
                            pos_ += 2;
                            if (!hex4(&lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return fail("lone high surrogate in \\u escape");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(&str, cp);
                        break;
                    }
                    default:
                        --pos_;
                        return fail("invalid escape");
                }
                continue;
            }
            if (c < 0x80) {
                str.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            const size_t start = pos_;
            if (decodeUtf8(s_, &pos_) == 0xFFFFFFFF) {
                pos_ = start;
                return fail("invalid UTF-8 in string");
            }
            str.append(s_.data() + start, pos_ - start);
        }
    }

    bool parseNumber(Value* out) {
        const size_t start = pos_;
        bool integral = true;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return fail("bad number");
        if (s_[pos_] == '0') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') return fail("leading zero in number");
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return fail("bad number");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            integral = false;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail("bad fraction in number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            integral = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail("bad exponent in number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        Value v;
        v.type_ = Value::Type::Number;
        if (integral) {
            // Accumulate as a negative number so INT64_MIN fits.
            bool fits = true;
            int64_t acc = 0;
            const bool neg = text[0] == '-';
            for (size_t i = neg ? 1 : 0; i < text.size(); ++i) {
                const int d = text[i] - '0';
                if (acc < (INT64_MIN + d) / 10) {
                    fits = false;
                    break;
                }
                acc = acc * 10 - d;
            }
            if (fits && !neg && acc == INT64_MIN) fits = false;
            if (fits) {
                v.isInt_ = true;
                v.int_ = neg ? acc : -acc;
                v.num_ = static_cast<double>(v.int_);
                *out = std::move(v);
                return true;
            }
        }
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        double d = 0;
        in >> d;
        if (in.fail()) {
            // Out of range: istream sets failbit and the value to +-max; JSON has no infinity.
            pos_ = start;
            return fail("number out of range");
        }
        v.num_ = d;
        *out = std::move(v);
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
    const char* why_ = "";
};

bool parse(std::string_view text, Value* out, std::string* err) {
    Parser p(text);
    return p.parseDocument(out, err);
}

// ---- Writer ------------------------------------------------------------------------------------

namespace {

void writeEscapedU(std::string* out, uint32_t u) {
    char buf[8];
    snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(u));
    out->append(buf);
}

void writeString(std::string* out, std::string_view s, bool asciiOnly) {
    out->push_back('"');
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out->append("\\\""); ++i; continue;
            case '\\': out->append("\\\\"); ++i; continue;
            case '\b': out->append("\\b"); ++i; continue;
            case '\f': out->append("\\f"); ++i; continue;
            case '\n': out->append("\\n"); ++i; continue;
            case '\r': out->append("\\r"); ++i; continue;
            case '\t': out->append("\\t"); ++i; continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            writeEscapedU(out, c);
            ++i;
        } else if (c < 0x80 || !asciiOnly) {
            out->push_back(static_cast<char>(c));
            ++i;
        } else {
            uint32_t cp = decodeUtf8(s, &i);
            if (cp == 0xFFFFFFFF) cp = 0xFFFD;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                writeEscapedU(out, 0xD800 + (cp >> 10));
                writeEscapedU(out, 0xDC00 + (cp & 0x3FF));
            } else {
                writeEscapedU(out, cp);
            }
        }
    }
    out->push_back('"');
}

std::string formatDouble(double d) {
    for (int prec = 15; prec <= 17; ++prec) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o.precision(prec);
        o << d;
        std::istringstream in(o.str());
        in.imbue(std::locale::classic());
        double back = 0;
        in >> back;
        if (back == d || prec == 17) return o.str();
    }
    return "0";
}

void newline(std::string* out, const WriteOptions& opts, int depth) {
    if (opts.indent <= 0) return;
    out->push_back('\n');
    out->append(static_cast<size_t>(opts.indent * depth), ' ');
}

void writeValue(std::string* out, const Value& v, const WriteOptions& opts, int depth) {
    switch (v.type()) {
        case Value::Type::Null: out->append("null"); return;
        case Value::Type::Bool: out->append(v.asBool() ? "true" : "false"); return;
        case Value::Type::Number:
            if (v.isInt()) {
                out->append(std::to_string(v.asInt()));
            } else if (!std::isfinite(v.asDouble())) {
                out->append("null");
            } else {
                out->append(formatDouble(v.asDouble()));
            }
            return;
        case Value::Type::String: writeString(out, v.asString(), opts.asciiOnly); return;
        case Value::Type::Array: {
            out->push_back('[');
            const auto& el = v.elements();
            for (size_t i = 0; i < el.size(); ++i) {
                if (i) out->push_back(',');
                newline(out, opts, depth + 1);
                writeValue(out, el[i], opts, depth + 1);
            }
            if (!el.empty()) newline(out, opts, depth);
            out->push_back(']');
            return;
        }
        case Value::Type::Object: {
            out->push_back('{');
            const auto& mem = v.members();
            for (size_t i = 0; i < mem.size(); ++i) {
                if (i) out->push_back(',');
                newline(out, opts, depth + 1);
                writeString(out, mem[i].key, opts.asciiOnly);
                out->append(opts.indent > 0 ? ": " : ":");
                writeValue(out, mem[i].value, opts, depth + 1);
            }
            if (!mem.empty()) newline(out, opts, depth);
            out->push_back('}');
            return;
        }
    }
}

}  // namespace

std::string write(const Value& v, const WriteOptions& opts) {
    std::string out;
    writeValue(&out, v, opts, 0);
    return out;
}

}  // namespace json
}  // namespace thimsy
