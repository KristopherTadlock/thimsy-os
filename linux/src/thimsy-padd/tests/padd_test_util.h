// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Helpers for thimsy-padd's tests.
#pragma once

#include <linux/input.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "core/InputCodes.h"
#include "core/Merger.h"
#include "core/PadSpec.h"
#include "thimsy/json.h"

namespace padd_test {

// tests/traces (THIMSY_PADD_TRACES, set by the Makefile).
inline std::string tracesDir() {
    const char* d = getenv("THIMSY_PADD_TRACES");
    return d && *d ? d : "tests/traces";
}

// linux/, from the traces directory (linux/src/thimsy-padd/tests/traces).
inline std::string linuxDir() { return tracesDir() + "/../../../.."; }

// Parses a device.json text and its pad section; *err gets the reason on failure.
inline bool parseSpec(const std::string& text, padd::PadSpec* spec, std::string* err,
                      std::vector<std::string>* warnings = nullptr) {
    thimsy::json::Value doc;
    if (!thimsy::json::parse(text, &doc, err)) return false;
    return padd::parsePadSpec(doc, spec, err, warnings);
}

inline std::string fmt(const padd::OutEvent& e) {
    const char* dev = e.dev == padd::Dev::Pad ? "pad" : e.dev == padd::Dev::Keys ? "keys" : "tap";
    return std::string(dev) + " " + padd::eventCodeName(e.type, e.code) + " " + std::to_string(e.value);
}

inline std::vector<std::string> fmt(const std::vector<padd::OutEvent>& evs) {
    std::vector<std::string> out;
    for (const padd::OutEvent& e : evs) out.push_back(fmt(e));
    return out;
}

struct In {
    uint16_t type, code;
    int32_t value;
};

// Feeds events from one source followed by its SYN_REPORT; returns what came out, formatted.
inline std::vector<std::string> feed(padd::Merger& m, int source, const std::vector<In>& evs) {
    std::vector<padd::OutEvent> out;
    for (const In& e : evs) {
        padd::SourceEvent s;
        s.source = source;
        s.type = e.type;
        s.code = e.code;
        s.value = e.value;
        m.process(s, &out);
    }
    padd::SourceEvent syn;
    syn.source = source;
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    m.process(syn, &out);
    return fmt(out);
}

using Strings = std::vector<std::string>;

}  // namespace padd_test
