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

// Moving around the button map panel (button_map_overlay_plan.md §6.5): a pure state machine over
// the laid-out body (item tops and heights, sections, the visible height), driven by buttons by
// position (through the pad model, so any device's pad works), the left stick, and touch.
//
//   D-pad / left stick up, down   one item (a row of the table, the drawing, a section title)
//   D-pad / left stick left, right  the previous / next section (controls, hotkeys)
//   L1 / R1                       a page up / down; the page indicator reads "2/3"
//   held                          repeats after a delay
//   drag                          the content follows the finger; a flick coasts and slows down,
//                                 stopping at the ends
//   swipe left / right            the next / previous section, as the D-pad
//   tap outside the panel         close; a tap inside does nothing
//   two or more fingers           the gesture is dropped (the multi-finger taps are Nano's)
//
// Close is the button that is Back in Nano, Start, or the chord again (the caller's). Times are
// milliseconds, positions the layout's pixels.
#pragma once

#include <cstdint>
#include <vector>

#include "NanoButtonMap.h"

namespace android {
namespace bmap {

struct NavRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// What the layout tells the navigator.
struct NavModel {
    std::vector<int> itemY, itemH;   // body items, from the content's top
    std::vector<int> sections;       // the first item of each section
    int viewH = 0;                   // the body's visible height
    NavRect panel, body;             // on screen
    float slop = 10;                 // movement that makes a touch a drag (Nano: max(10, unit/60))
    float swipeMin = 48;             // a horizontal drag this long is a swipe (Nano: unit * 0.08)
};

enum class NavAction : uint8_t { kNone, kLineUp, kLineDown, kPrevSection, kNextSection, kPageUp, kPageDown, kClose, kToggleView };

class Nav {
public:
    void setModel(const NavModel& m);   // keeps the position, clamped
    const NavModel& model() const { return mModel; }

    float scroll() const { return mScroll; }
    float maxScroll() const;
    int topItem() const;                // the first item at least partly visible
    int pageCount() const;              // pages from the top, each as many whole items as fit
    int page() const;                   // 1-based, for "page/pageCount"
    int section() const;                // the section the top item is in
    bool canUp() const { return mScroll > 0.5f; }
    bool canDown() const { return mScroll < maxScroll() - 0.5f; }

    // Each returns whether the position moved.
    bool apply(NavAction a);
    bool lineUp();
    bool lineDown();
    bool pageUp();
    bool pageDown();
    bool prevSection();
    bool nextSection();
    void scrollTo(float px);

    // Touch, in screen pixels (setTouchSpace maps another space onto the layout's).
    void setTouchSpace(float touchW, float touchH, float layoutW, float layoutH);
    void pointers(int count);                       // fingers down now
    void touchDown(float x, float y, int64_t ms);
    void touchMove(float x, float y, int64_t ms);
    NavAction touchUp(int64_t ms);                  // kClose (tap outside), the section move it made, or kNone
    void touchCancel();
    bool tick(int64_t ms);                          // a fling: true while it moves
    bool flinging() const { return mFling; }
    bool dragging() const { return mDown && mMoved && mAxis == 2; }

private:
    std::vector<float> snaps() const;   // item tops the keys stop at, and the end
    std::vector<float> pageStarts() const;
    void clamp();

    NavModel mModel;
    float mScroll = 0;
    // touch
    float mSx = 1, mSy = 1;
    bool mDown = false, mMoved = false, mMulti = false, mStoppedFling = false;
    int mAxis = 0;                      // 1 across, 2 up-down
    float mX0 = 0, mY0 = 0, mX = 0, mY = 0, mScroll0 = 0;
    float mVel = 0, mLastPos = 0;       // finger along y, px per ms
    int64_t mLastMs = 0;
    bool mFling = false;
    float mFlingVel = 0;                // content px per ms
    int64_t mFlingMs = 0;
};

// Buttons and the left stick to actions, with repeat on hold.
struct NavConfig {
    Phys back = Phys::kCount;      // Nano's Back on this pad (the one sending BTN_EAST)
    Phys toggle = Phys::kCount;    // table / drawing (the one sending BTN_WEST)
    int repeatDelayMs = 350, repeatMs = 90;
    float deadzone = 0.5f;         // the stick counts past this...
    float release = 0.35f;         // ...and stops below this
};
NavConfig navConfigFor(const PadModel& pad);

class NavInput {
public:
    explicit NavInput(const NavConfig& c = NavConfig()) : mCfg(c) {}
    NavAction button(Phys p, bool down, int64_t ms);
    NavAction stick(float x, float y, int64_t ms);   // the left stick, -1..1, +y down
    NavAction tick(int64_t ms);                      // a held direction's repeat
private:
    void hold(NavAction a, int64_t ms);
    NavConfig mCfg;
    NavAction mHeld = NavAction::kNone;
    Phys mHeldPhys = Phys::kCount;
    bool mHeldByStick = false;
    int64_t mNext = 0;
};

}  // namespace bmap
}  // namespace android
