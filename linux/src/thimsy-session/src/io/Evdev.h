// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The input nodes the session reads (plan 5.3: "volume, power and lid stay separate nodes, read by
// thimsy-session"): every /dev/input/event* whose capabilities (sysfs, capabilities/key and /sw)
// include KEY_POWER, a volume key or SW_LID, except gamepads (thimsy-padd grabs their sources);
// and the "Thimsy Gamepad" itself, read without a grab, for the quit hotkey.
//
// On a device the nodes are read as struct input_event with CLOCK_MONOTONIC stamps. On fake
// hardware the nodes are FIFOs (named like the real ones, with the same sysfs entries in the test
// tree) carrying text lines:
//
//   KEY_POWER 1          <code name or type:code> <value> [<ms>]
//   KEY_POWER 0 3100
//
// The optional <ms> is the event's time relative to the line that started the sequence: a stamped
// line whose <ms> is not above the previous one's starts a new sequence and is stamped "now". Without
// <ms> an event is stamped when it is read.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/KeyMachine.h"
#include "io/Sysfs.h"

namespace thimsy {
namespace session {

// Whether a sysfs capability bitmap ("1 0 1c000 ..." hex words, most significant first) has bit.
bool capabilityHas(const std::string& bitmap, int bit);

class Evdev {
 public:
    Evdev(const Sysfs& sysfs, bool fake);
    ~Evdev();

    // Opens the relevant nodes not yet open; closes ones that vanished. Returns true if the set of
    // descriptors changed (the loop re-registers them).
    bool scan();
    std::vector<int> fds() const;
    // Reads what is ready on fd; nowMs stamps text events. False when the node went away.
    bool read(int fd, int64_t nowMs, std::vector<KeyEvent>* out);
    void close(int fd);
    // node name ("event3") for a descriptor, for logs.
    std::string nameOf(int fd) const;

 private:
    struct Node {
        std::string node;     // "event3"
        std::string devName;  // the device's name
        KeyEvent::Source source = KeyEvent::Source::Keys;
        std::string text;     // fake: unfinished line
        bool haveBase = false;
        int64_t baseMs = 0;   // fake: the time of the first stamped line
        int64_t baseRel = 0;
        int64_t lastRel = 0;
    };
    bool parseLine(Node* n, const std::string& line, int64_t nowMs, KeyEvent* ev);

    const Sysfs& sysfs_;
    bool fake_;
    std::map<int, Node> nodes_;
};

}  // namespace session
}  // namespace thimsy
