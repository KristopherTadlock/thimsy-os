// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/InputCodes.h"

#include <linux/input-event-codes.h>

#include <cstdio>
#include <cstdlib>

namespace padd {
namespace {

struct CodeName {
    const char* name;
    int value;
    bool canonical;
};

#include "core/InputCodes.inc"

struct Table {
    const CodeName* rows;
    size_t count;
};

template <size_t N>
constexpr Table table(const CodeName (&rows)[N]) {
    return Table{rows, N};
}

Table tableFor(int type) {
    switch (type) {
        case EV_SYN: return table(kSynCodes);
        case EV_KEY: return table(kKeyCodes);
        case EV_ABS: return table(kAbsCodes);
        case EV_MSC: return table(kMscCodes);
        case EV_SW: return table(kSwCodes);
        default: return Table{nullptr, 0};
    }
}

// A decimal or 0x-hex number; -1 when the text is not one.
int parseNumber(std::string_view s) {
    if (s.empty() || s.size() > 10) return -1;
    std::string t(s);
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 0);
    if (!end || *end != '\0' || v < 0 || v > 0xffff) return -1;
    if (t[0] < '0' || t[0] > '9') return -1;
    return static_cast<int>(v);
}

int lookup(Table t, std::string_view name) {
    for (size_t i = 0; i < t.count; ++i) {
        if (name == t.rows[i].name) return t.rows[i].value;
    }
    return -1;
}

std::string hex(int v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%03x", v);
    return buf;
}

}  // namespace

int eventTypeByName(std::string_view name) {
    int n = parseNumber(name);
    if (n >= 0) return n <= EV_MAX ? n : -1;
    return lookup(table(kEvCodes), name);
}

int eventCodeByName(int type, std::string_view name) {
    int n = parseNumber(name);
    if (n >= 0) return n;
    return lookup(tableFor(type), name);
}

int keyCodeByName(std::string_view name) {
    if (name.substr(0, 4) != "KEY_" && name.substr(0, 4) != "BTN_") return -1;
    return lookup(table(kKeyCodes), name);
}

int absCodeByName(std::string_view name) {
    if (name.substr(0, 4) != "ABS_") return -1;
    return lookup(table(kAbsCodes), name);
}

std::string eventTypeName(int type) {
    Table t = table(kEvCodes);
    for (size_t i = 0; i < t.count; ++i) {
        if (t.rows[i].value == type && t.rows[i].canonical) return t.rows[i].name;
    }
    return hex(type);
}

std::string eventCodeName(int type, int code) {
    Table t = tableFor(type);
    for (size_t i = 0; i < t.count; ++i) {
        if (t.rows[i].value == code && t.rows[i].canonical) return t.rows[i].name;
    }
    return hex(code);
}

}  // namespace padd
