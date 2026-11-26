// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-session: started by BusyBox init (inittab "::respawn:/usr/libexec/thimsy/session").
// It starts and supervises Nano, owns drives and volumes, the power, volume and lid keys, sleep,
// the quit hotkey, the deferred services and Nano's requests. linux/docs/session.md has the
// details; Session.cpp the loop.
#include <cstdio>
#include <cstring>
#include <string>

#include "Session.h"

namespace {

void usage(FILE* f) {
    fprintf(f,
            "usage: thimsy-session [--root DIR] [--fake-hw] [--tooldir DIR] [--nano PATH]\n"
            "                      [--scrape PATH] [--padd PATH] [--verbose]\n"
            "  --root DIR     read and write every path under DIR (tests)\n"
            "  --fake-hw      uevents and keys from FIFOs under DIR, mount(8) programs, no namespace\n"
            "  --tooldir DIR  programs in DIR replace the system's, by base name\n");
}

}  // namespace

int main(int argc, char** argv) {
    thimsy::session::Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) {
                fprintf(stderr, "thimsy-session: %s needs a value\n", a.c_str());
                return false;
            }
            *out = argv[++i];
            return true;
        };
        if (a == "--root") {
            if (!value(&o.root)) return 2;
            while (o.root.size() > 1 && o.root.back() == '/') o.root.pop_back();
        } else if (a == "--fake-hw") {
            o.fakeHw = true;
        } else if (a == "--tooldir") {
            if (!value(&o.toolDir)) return 2;
        } else if (a == "--nano") {
            if (!value(&o.nanoPath)) return 2;
        } else if (a == "--scrape") {
            if (!value(&o.scrapePath)) return 2;
        } else if (a == "--padd") {
            if (!value(&o.paddPath)) return 2;
        } else if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "thimsy-session: unknown argument %s\n", a.c_str());
            usage(stderr);
            return 2;
        }
    }
    if (o.fakeHw && o.root.empty()) {
        fprintf(stderr, "thimsy-session: --fake-hw needs --root\n");
        return 2;
    }
    return thimsy::session::runSession(o);
}
