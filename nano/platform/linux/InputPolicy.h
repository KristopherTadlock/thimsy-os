/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The Linux nanoplat::InputPolicy (docs/linux/IMPLEMENTATION.md B.6, G7-input). The built-in pad is
// thimsy-padd's uinput pad, which Nano reads with its own evdev code like any device. Nano never
// grabs it: a grab held by Nano, which keeps running while a game does, would starve the game, and
// padd already keeps the pad's source nodes from everyone else. So deviceOpened and deviceClosing
// do nothing.
//
// silenceGamePad is padd's intercept (G3a, linux/src/thimsy-padd/src/io/Control.h):
//   on:  connects to padd's tap (SOCK_SEQPACKET, /run/thimsy/padd-tap.sock), then sets
//        sys.thimsy.padd.intercept to "1". padd then releases every held button and centres every
//        axis on its pad, forwards nothing to it, and sends the pad's events to the tap instead: one
//        packet per batch of 8-byte {type, code, value} records ending in SYN_REPORT. When the tap
//        can't be reached, the key is left alone: a game that keeps the pad is better than a menu
//        that gets no input.
//   off: sets the key to "0", then closes the tap.
// While silenced, the menu's input from the built-in pad arrives only on the tap: readTap() turns
// its packets into struct input_event records, as Nano's evdev loop reads them. Reading the tap in
// that loop is not done yet: it needs an edit to NanoMenuInput.cpp's pollInput outside the grab
// (G7-input's report and DECISIONS.md give the follow-up), and no Linux code path turns silencing on
// before the Quick Menu over a running game exists.
//
// A "1" left by an earlier Nano that died while silenced would keep every game without the pad, so
// create() clears it.
#pragma once

#include <linux/input.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanolinux {
namespace input {

constexpr const char* kInterceptKey = "sys.thimsy.padd.intercept";
constexpr const char* kDefaultTapPath = "/run/thimsy/padd-tap.sock";

struct Options {
    std::string tapPath = kDefaultTapPath;
};

class LinuxInputPolicy : public nanoplat::InputPolicy {
 public:
    explicit LinuxInputPolicy(Options options);
    ~LinuxInputPolicy() override;  // gives the pad back if it is still silenced
    LinuxInputPolicy(const LinuxInputPolicy&) = delete;
    LinuxInputPolicy& operator=(const LinuxInputPolicy&) = delete;

    void deviceOpened(int, const char*, Opened) override {}
    void deviceClosing(int) override {}
    void silenceGamePad(bool on) override;

    // Whether silenceGamePad(true) took effect and silenceGamePad(false) has not been called since.
    bool silenced() const;
    // The tap's socket while silenced, for poll() (POLLIN); -1 otherwise, and after padd closed it.
    int tapFd() const;
    // Appends the events of every packet waiting on the tap, stamped with the time they were read,
    // and returns how many it appended; never blocks. A record cut short at a packet's end is
    // dropped. When padd closes the tap (it exited), the socket is closed and the next call tries to
    // reconnect, so a restarted padd's tap is picked up while still silenced.
    size_t readTap(std::vector<struct input_event>* out);

 private:
    int connectTap() const;  // a connected, non-blocking socket, or -1 (errno set)
    void closeTapLocked();

    const Options options_;
    mutable std::mutex mu_;
    bool silenced_ = false;  // guarded by mu_
    int tap_ = -1;           // guarded by mu_
};

std::unique_ptr<LinuxInputPolicy> create(Options options = Options());

}  // namespace input
}  // namespace nanolinux
