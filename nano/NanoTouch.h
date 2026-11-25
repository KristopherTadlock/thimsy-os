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

// Touch zones: whatever draws an input says, as it draws it, where it is on screen and what touching
// it does. A touch is matched against the zones of the last frame drawn, the topmost (registered
// last) first, so it always lands on what the user sees.
//
//   tap(rect, action[, hold])  pressed and let go without moving: action. A button, a row, an
//                              entry, a help prompt. With hold, keeping the finger still on it
//                              for half a second runs hold instead (and the release does nothing).
//   scroll(rect, axis, pitch, step)
//                              a drag along axis moves the content one step per pitch pixels
//                              (step(+1) = later entries; false at the end), and a flick keeps
//                              it going. Register it before the entries it holds. single: one
//                              step a drag at most, and no flick.
//   swipe(rect, action)        a drag no scroll takes, let go: action(axis, dir), dir +1 for
//                              right or down.
//   block(rect, action)        an opaque panel: nothing drawn under it is reached. A tap on it,
//                              outside the zones drawn over it, runs action (may be empty).
//   tapMargin(rect, action)    a tap zone that gives way: any other tap zone under the finger wins
//                              over it. For the slack around a small target (a button hint grown
//                              to fingertip size) that must not take taps from its neighbours.
//   press(rect, action)        a fixed on-screen button: highlighted as the finger lands, its action
//                              handed to the caller then (takeFired), to run once a second finger
//                              can no longer make the gesture a multi-finger one. The rest of that
//                              gesture does nothing (no drag, no tap).
//                              Over a scroll zone it acts on release like a tap, so a drag that
//                              starts on it still scrolls. pressMargin gives way like tapMargin
//                              and lights up its visible rect.
//
// A finger landing on a fling stops it, and that touch taps nothing; it presses only a button outside
// the flinging zone.
//
// Coordinates are the frame's logical pixels: the space Nano draws in and touchMapRaw maps to.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace android {

struct TouchRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool contains(float px, float py) const {
        return w > 0.0f && h > 0.0f && px >= x && px < x + w && py >= y && py < y + h;
    }
    TouchRect clippedTo(const TouchRect& c) const;   // the part inside c (empty when none)
    TouchRect grownTo(float minW, float minH) const;  // at least minW x minH, about its centre
};

class TouchZones {
public:
    using Action = std::function<void()>;
    using Step = std::function<bool(int dir)>;
    using Swipe = std::function<void(int axis, int dir)>;
    enum Axis { kAcross = 1, kUpDown = 2 };

    // ---- drawing: between beginFrame and endFrame, in draw order
    void beginFrame();
    void tap(const TouchRect& r, Action a, Action hold = nullptr);
    void tapMargin(const TouchRect& r, Action a);
    void press(const TouchRect& r, Action a);
    void pressMargin(const TouchRect& r, Action a, const TouchRect& visible = TouchRect());
    void scroll(const TouchRect& r, int axis, float pitch, Step s, bool single = false);
    void swipe(const TouchRect& r, Swipe s);
    void block(const TouchRect& r, Action a = nullptr);
    void endFrame();                                  // these zones answer touches from now on
    void clear();                                     // no zones until the next frame registers some
    bool any() const { return !mLive.empty(); }

    // ---- input, in the same pixels
    void setSlop(float px) { mSlop = px; }            // movement that makes a press a drag
    void setSwipeMin(float px) { mSwipeMin = px; }    // drag length that counts as a swipe
    void down(float x, float y, int64_t ms);
    void move(float x, float y, int64_t ms);
    void up(int64_t ms);
    void cancel();                                    // the finger's gesture ends without acting
    bool tick(float dtMs, int64_t nowMs);             // per frame: a fling, a hold; true while either needs frames
    void stopFling() { mFling = false; }
    bool tracking() const { return mDown; }
    bool flinging() const { return mFling; }
    bool takeFired(Action& a) {                       // a press button under this down: its action
        if (!mFired) return false;
        mFired = false; a = std::move(mFiredAct); mFiredAct = nullptr;
        return true;
    }
    bool pressShown(int64_t nowMs, TouchRect& r, float& alpha) const;   // the highlight of a press

private:
    enum Kind { kTap, kScroll, kSwipe, kBlock, kPress };
    struct Zone {
        Kind kind = kTap;
        bool margin = false;
        TouchRect r, vis;                             // vis: what a press lights up (r when empty)
        int axis = 0;
        float pitch = 0.0f;
        bool single = false;
        Action act, hold;
        Step step;
        Swipe sw;
    };
    void stepTo(const Zone& z, float pos);            // steps the content to follow pos

    std::vector<Zone> mBuild, mLive;

    // the gesture: copies of the zones it found under the finger when it went down
    bool mDown = false, mMoved = false, mHeld = false;
    int64_t mDownMs = 0;
    int mAxis = 0;
    float mX0 = 0.0f, mY0 = 0.0f, mX = 0.0f, mY = 0.0f;
    bool mHasPress = false, mHasScroll[3] = {false, false, false}, mHasSwipe = false;
    Zone mPress, mScroll[3], mSwipe;
    bool mScrolling = false;                          // the drag is moving mScroll[mAxis]
    bool mRefused = false;                            // the content would not take a step this drag
    int mSteps = 0;                                   // steps the drag (and its fling) has taken
    float mLastPos = 0.0f, mVel = 0.0f;               // along the axis; px per ms
    int64_t mLastMs = 0;
    bool mFling = false;
    bool mFired = false;                              // a press button was under the last down
    Action mFiredAct;
    TouchRect mFlashR;                                // the last press, highlighted briefly
    int64_t mFlashMs = 0;
    float mFlingPos = 0.0f, mFlingVel = 0.0f;
    Zone mFlingZone;

    float mSlop = 16.0f, mSwipeMin = 48.0f;
};

}  // namespace android
