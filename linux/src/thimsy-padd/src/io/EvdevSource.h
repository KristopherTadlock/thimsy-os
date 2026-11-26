// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The source side: finding evdev nodes, opening, grabbing and hiding them, reading their events,
// and driving a vibrator node's force feedback.
//
// Nodes are found through sysfs (<sys>/class/input/eventN: its dev number and device/name), so a
// node padd unlinked from /dev/input earlier (a restart) can still be opened: when
// /dev/input/eventN is gone, padd makes a private node in its run directory, opens it and
// removes it again. Hiding is unlinking /dev/input/eventN after opening it, so libudev-zero
// users never see it (plan 5.3; proved or disproved on the device, open question 4).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Merger.h"
#include "core/PadSpec.h"
#include "core/Rumble.h"

namespace padd {

// One input node as sysfs shows it.
struct NodeEntry {
    std::string event;  // "event3"
    std::string name;   // device/name, for a cheap first match
    unsigned major = 0, minor = 0;
};

// Lists <sysDir>/class/input/event*; sorted by number.
std::vector<NodeEntry> listNodes(const std::string& sysDir);

// Reads a node's identity and capabilities from an open evdev fd. False if the ioctls fail.
bool queryNode(int fd, NodeInfo* info, SourceCaps* caps);

class EvdevSource {
 public:
    struct Paths {
        std::string devInputDir = "/dev/input";
        std::string privateDir = "/run/thimsy/padd";  // for nodes recreated with mknod
    };
    // Opens the node (read-write when it allows, for force feedback), non-blocking.
    static std::unique_ptr<EvdevSource> open(const NodeEntry& entry, const Paths& paths, std::string* err);
    // Wraps an fd that is already open (tests: a pipe or socket carrying struct input_event).
    static std::unique_ptr<EvdevSource> adopt(int fd, const std::string& path, const NodeInfo& info,
                                              const SourceCaps& caps);
    ~EvdevSource();
    EvdevSource(const EvdevSource&) = delete;
    EvdevSource& operator=(const EvdevSource&) = delete;

    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    const NodeInfo& info() const { return info_; }
    const SourceCaps& caps() const { return caps_; }

    bool grab();
    // Unlinks the /dev/input node (once). True when it is gone.
    bool hide();

    enum class Read { Ok, Gone, Error };
    // Reads what is queued (the fd is non-blocking) as source events for source.
    Read readEvents(int source, std::vector<SourceEvent>* out);

    // The node's state now (EVIOCGKEY, and EVIOCGABS for each axis it has).
    bool currentState(std::set<uint16_t>* keysDown, std::map<uint16_t, int32_t>* abs);

    // Force feedback on this node: one FF_RUMBLE effect, re-uploaded for each command.
    bool rumble(const VibCommand& c);

 private:
    EvdevSource(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    int fd_ = -1;
    std::string path_;
    bool hidden_ = false;
    NodeInfo info_;
    SourceCaps caps_;
    int ffId_ = -1;
};

// Watches /dev/input for new nodes (inotify); readable when something changed.
class NodeWatch {
 public:
    static std::unique_ptr<NodeWatch> create(const std::string& devInputDir);
    ~NodeWatch();
    int fd() const { return fd_; }
    // Empties the queue; true if a node was created (a rescan is due).
    bool drain();

 private:
    int fd_ = -1;
};

}  // namespace padd
