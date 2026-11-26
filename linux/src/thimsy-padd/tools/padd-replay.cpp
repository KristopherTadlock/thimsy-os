// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// padd-replay: runs a trace through thimsy-padd's core and prints what padd would emit.
//
//   padd-replay [--device FILE] TRACE|-
//
// The trace format is described in src/core/Replay.h. Exit status: 0 done, 1 a bad trace or
// device.json, 2 usage.

#include <fstream>
#include <iostream>
#include <string>

#include "core/Replay.h"

int main(int argc, char** argv) {
    std::string device, path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) {
            device = argv[++i];
        } else if (path.empty() && (a == "-" || a[0] != '-')) {
            path = a;
        } else {
            std::cerr << "usage: padd-replay [--device FILE] TRACE|-\n";
            return 2;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: padd-replay [--device FILE] TRACE|-\n";
        return 2;
    }
    std::string err;
    bool ok;
    if (path == "-") {
        ok = padd::runReplay(std::cin, ".", std::cout, &err, device);
    } else {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "padd-replay: " << path << ": can't open\n";
            return 1;
        }
        size_t slash = path.rfind('/');
        std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
        ok = padd::runReplay(in, dir, std::cout, &err, device);
    }
    if (!ok) {
        std::cout.flush();
        std::cerr << "padd-replay: " << path << ": " << err << "\n";
        return 1;
    }
    return 0;
}
