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

// The host platform's channel to the scenario runner (docs/linux/IMPLEMENTATION.md D.3;
// tests/e2e/nano_scenario.py is the other end; nano/test/host/scenarios/README.md documents it).
//
//   events   nano-headless -> runner: one JSON object per line, appended to a file the runner
//            tails: {"ev":"<kind>","t_ms":<monotonic ms>, ...}. Kinds: start, present, capture,
//            prop, shell, grab, splash, drm_stop, pong, exit.
//   control  runner -> nano-headless: text lines on a FIFO, read by one thread:
//              barrier on|off   stop every present until a grant (on), or never (off, the default)
//              grant N          let N more presents through the barrier
//              capture PATH     write the next presented frame to PATH (binary PPM, top row first)
//              ping [TOKEN]     answer with {"ev":"pong","token":TOKEN,"presents":N}
//              quit             ask Nano to exit (the quit handler) and open the barrier for good
//
// No dependency beyond libc and the C++ library, so the small host tests link it alone.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nanohost {

// A JSON string literal for `s`, quotes included. Invalid UTF-8 bytes become U+FFFD.
std::string jsonQuote(std::string_view s);

// --- events ---------------------------------------------------------------------------------------
// Opens (appending, creating it 0644) the event file; an empty path turns events off. false and *err
// when it can't be opened.
bool openEvents(const std::string& path, std::string* err = nullptr);
// The event file's descriptor, or -1. For writers that may not take locks or allocate (the exec
// wrappers in a forked child): they format a whole line on the stack and write() it once.
int eventsFd();
// Appends {"ev":kind,"t_ms":...,<fields>}. `fields` is a JSON fragment of members ("k":v,...) or
// empty. Thread-safe; one write() per line.
void event(std::string_view kind, std::string_view fields = {});
// Milliseconds on CLOCK_MONOTONIC, as the events carry them.
int64_t monotonicMs();

// --- the display the host pretends to have --------------------------------------------------------
struct DisplayConfig {
    int width = 1280;
    int height = 720;
    int refreshHz = 60;
    int count = 1;            // displays; the first is the primary
    int rotationDeg = 0;      // the panel's install rotation (sDrmRotationDeg)
    bool pace = true;         // each present waits for the next "vblank" at refreshHz
};
void setDisplayConfig(const DisplayConfig& config);
DisplayConfig displayConfig();

// --- presents, the frame barrier and captures ----------------------------------------------------
struct PresentInfo {
    int slot = 0;             // ring slot, -1 for a present outside the ring (drmPushFrame)
    uint32_t width = 0, height = 0;
    int64_t draws = -1;       // draw calls so far (the GL stub counts them), -1 when unknown
};
// Writes the frame being presented to `path`; false on failure. Supplied by DrmHost (it reads the
// slot's framebuffer back with glReadPixels).
using CaptureFn = std::function<bool(const std::string& path)>;
// One present: counts it, takes a pending capture, logs {"ev":"present"}, paces to the refresh
// rate, then waits at the barrier while it is on and no grant is left. Returns the present's number
// (1 for the first).
uint64_t present(const PresentInfo& info, const CaptureFn& capture);
uint64_t presentCount();

// The barrier and captures, also reachable from the control channel.
void setBarrier(bool on);
void grant(uint64_t frames);
void requestCapture(const std::string& path);

// --- the control channel --------------------------------------------------------------------------
// Opens the FIFO read-write (so neither the open nor a reader ever sees end-of-file) and starts the
// reader thread. false and *err when it can't.
bool startControl(const std::string& fifoPath, std::string* err = nullptr);
// Runs one control line (the reader thread calls it; tests call it directly). false for a line it
// does not understand (logged as {"ev":"control_error"}).
bool handleControl(std::string_view line);
// Called on "quit". main_headless sets it to ask NanoMenu's thread to exit.
void setQuitHandler(std::function<void()> handler);

// Puts the module back to its initial state (tests only): closes the event file, clears counters,
// the barrier, captures and the quit handler. Does not stop a control thread.
void resetForTest();

}  // namespace nanohost
