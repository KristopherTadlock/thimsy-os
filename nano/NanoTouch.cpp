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

#include "NanoTouch.h"

#include <algorithm>
#include <cmath>

namespace android {

TouchRect TouchRect::clippedTo(const TouchRect& c) const {
    TouchRect o;
    o.x = std::max(x, c.x);
    o.y = std::max(y, c.y);
    o.w = std::min(x + w, c.x + c.w) - o.x;
    o.h = std::min(y + h, c.y + c.h) - o.y;
    if (o.w <= 0.0f || o.h <= 0.0f) o.w = o.h = 0.0f;
    return o;
}

TouchRect TouchRect::grownTo(float minW, float minH) const {
    TouchRect o = *this;
    if (o.w < minW) { o.x -= (minW - o.w) * 0.5f; o.w = minW; }
    if (o.h < minH) { o.y -= (minH - o.h) * 0.5f; o.h = minH; }
    return o;
}

void TouchZones::beginFrame() { mBuild.clear(); }

void TouchZones::tap(const TouchRect& r, Action a, Action hold) {
    Zone z; z.kind = kTap; z.r = r; z.act = std::move(a); z.hold = std::move(hold);
    mBuild.push_back(std::move(z));
}

void TouchZones::tapMargin(const TouchRect& r, Action a) {
    Zone z; z.kind = kTap; z.margin = true; z.r = r; z.act = std::move(a);
    mBuild.push_back(std::move(z));
}

void TouchZones::press(const TouchRect& r, Action a) {
    Zone z; z.kind = kPress; z.r = r; z.act = std::move(a);
    mBuild.push_back(std::move(z));
}

void TouchZones::pressMargin(const TouchRect& r, Action a, const TouchRect& visible) {
    Zone z; z.kind = kPress; z.margin = true; z.r = r; z.vis = visible; z.act = std::move(a);
    mBuild.push_back(std::move(z));
}

void TouchZones::scroll(const TouchRect& r, int axis, float pitch, Step s, bool single) {
    if (axis != kAcross && axis != kUpDown) return;
    Zone z; z.kind = kScroll; z.r = r; z.axis = axis; z.pitch = pitch; z.step = std::move(s);
    z.single = single;
    mBuild.push_back(std::move(z));
}

void TouchZones::swipe(const TouchRect& r, Swipe s) {
    Zone z; z.kind = kSwipe; z.r = r; z.sw = std::move(s);
    mBuild.push_back(std::move(z));
}

void TouchZones::block(const TouchRect& r, Action a) {
    Zone z; z.kind = kBlock; z.r = r; z.act = std::move(a);
    mBuild.push_back(std::move(z));
}

void TouchZones::endFrame() { mLive.swap(mBuild); mBuild.clear(); }

void TouchZones::clear() { mLive.clear(); mBuild.clear(); }

void TouchZones::down(float x, float y, int64_t ms) {
    const bool stopsFling = mFling;
    mFling = false;
    mFired = false;
    mDown = true; mMoved = false; mHeld = false; mDownMs = ms; mAxis = 0; mScrolling = false; mSteps = 0;
    mRefused = false;
    mX0 = mX = x; mY0 = mY = y;
    mLastPos = 0.0f; mVel = 0.0f; mLastMs = ms;
    mHasPress = mHasSwipe = false;
    mHasScroll[1] = mHasScroll[2] = false;
    // Topmost first: the first tap or block under the finger is what a tap presses; a block hides
    // everything under it. Scrolls and swipes under a tap zone still take a drag. A margin zone is
    // pressed only when no other tap zone above the block is under the finger.
    bool haveMargin = false;
    Zone margin;
    for (auto it = mLive.rbegin(); it != mLive.rend(); ++it) {
        if (!it->r.contains(x, y)) continue;
        const bool pressable = it->kind == kTap || it->kind == kPress;
        if (pressable && it->margin) {
            if (!haveMargin) { margin = *it; haveMargin = true; }
        } else if (pressable || it->kind == kBlock) {
            if (!mHasPress) {
                mPress = (it->kind == kBlock && haveMargin) ? margin : *it;
                mHasPress = true;
            }
            if (it->kind == kBlock) break;
        } else if (it->kind == kScroll) {
            if (!mHasScroll[it->axis]) { mScroll[it->axis] = *it; mHasScroll[it->axis] = true; }
        } else if (it->kind == kSwipe) {
            if (!mHasSwipe) { mSwipe = *it; mHasSwipe = true; }
        }
    }
    if (!mHasPress && haveMargin) { mPress = margin; mHasPress = true; }
    if (stopsFling && (mPress.kind != kPress || mFlingZone.r.contains(x, y))) mHasPress = false;
    if (mHasPress && mPress.kind == kPress && !mHasScroll[kAcross] && !mHasScroll[kUpDown]) {
        // A button: the gesture is over for the zones; the caller runs its action.
        mDown = false; mHasPress = false; mFired = true; mFiredAct = mPress.act;
        mFlashR = mPress.vis.w > 0.0f ? mPress.vis : mPress.r; mFlashMs = ms;
    }
}

bool TouchZones::pressShown(int64_t nowMs, TouchRect& r, float& alpha) const {
    const int64_t age = nowMs - mFlashMs;
    if (mFlashMs == 0 || age < 0 || age >= 160) return false;
    r = mFlashR;
    alpha = 1.0f - (float)age / 160.0f;
    return true;
}

void TouchZones::stepTo(const Zone& z, float pos) {
    if (z.pitch <= 0.0f || !z.step) return;
    int want = -(int)(pos / z.pitch);   // dragging up or left brings later entries in
    if (z.single) want = std::max(-1, std::min(1, want));
    while (mSteps != want) {
        const int dir = want > mSteps ? 1 : -1;
        if (!z.step(dir)) { mRefused = true; return; }
        mSteps += dir;
    }
}

void TouchZones::move(float x, float y, int64_t ms) {
    if (!mDown || (x == mX && y == mY)) return;   // no movement: no velocity sample (the lift's report)
    mX = x; mY = y;
    const float dx = x - mX0, dy = y - mY0;
    if (!mMoved) {
        if (dx * dx + dy * dy < mSlop * mSlop) return;
        mMoved = true;
        mAxis = fabsf(dx) > fabsf(dy) ? kAcross : kUpDown;
        mScrolling = mHasScroll[mAxis];
    }
    if (!mScrolling) return;
    const float pos = mAxis == kAcross ? dx : dy;
    if (ms > mLastMs) {
        const float v = (pos - mLastPos) / (float)(ms - mLastMs);
        // After a pause the finger's earlier speed is stale: a lift then must not fling with it.
        mVel = ms - mLastMs > 60 ? v : mVel * 0.4f + v * 0.6f;
    }
    mLastPos = pos; mLastMs = ms;
    stepTo(mScroll[mAxis], pos);
}

void TouchZones::up(int64_t ms) {
    if (!mDown) return;
    mDown = false;
    const float dx = mX - mX0, dy = mY - mY0;
    if (!mMoved) {
        if (!mHeld && mHasPress && mPress.act) mPress.act();
        return;
    }
    if (mScrolling) {
        const Zone& z = mScroll[mAxis];
        const float pos = mAxis == kAcross ? dx : dy;
        if (!z.single && fabsf(mVel) > 0.3f && ms - mLastMs < 80) {
            // A flick: on from where the finger left off, if the content can move that way.
            const int dir = mVel > 0.0f ? -1 : 1;
            if (mSteps != 0 || (z.step && z.step(dir))) {
                if (mSteps == 0) mSteps = dir;
                mFling = true; mFlingVel = mVel; mFlingZone = z;
                // From where the steps so far put it (a probe step may be ahead of the finger).
                mFlingPos = dir < 0 ? std::max(pos, -(float)mSteps * z.pitch)
                                    : std::min(pos, -(float)mSteps * z.pitch);
                return;
            }
        }
        if (mSteps != 0) return;
        // Shorter than one entry: a list that can move that way takes one step; content that cannot
        // (a list pulled past its first entry, say) makes the drag a swipe.
        const float dist = fabsf(pos);
        if (!mRefused) {
            if (dist < mSwipeMin) return;
            const int dir = pos > 0.0f ? -1 : 1;
            if (z.step && z.step(dir)) return;
        }
    }
    const float dist = mAxis == kAcross ? fabsf(dx) : fabsf(dy);
    if (mHasSwipe && mSwipe.sw && dist >= mSwipeMin) {
        const float d = mAxis == kAcross ? dx : dy;
        mSwipe.sw(mAxis, d > 0.0f ? 1 : -1);
    }
}

void TouchZones::cancel() { mDown = false; mFling = false; }

bool TouchZones::tick(float dtMs, int64_t nowMs) {
    const bool flash = mFlashMs != 0 && nowMs - mFlashMs < 160;   // frames for the press highlight
    if (mDown && !mMoved && !mHeld && mHasPress && mPress.hold) {
        if (nowMs - mDownMs < 500) return true;   // frames keep coming to see the half second out
        mHeld = true;
        mPress.hold();
        return true;
    }
    if (!mFling || mDown) return flash;
    dtMs = std::min(100.0f, std::max(0.0f, dtMs));
    mFlingPos += mFlingVel * dtMs;
    mFlingVel *= expf(-dtMs / 325.0f);   // Android's scroller friction, roughly
    const int before = mSteps;
    const int want = mFlingZone.pitch > 0.0f ? -(int)(mFlingPos / mFlingZone.pitch) : mSteps;
    while (mSteps != want) {
        const int dir = want > mSteps ? 1 : -1;
        if (!mFlingZone.step || !mFlingZone.step(dir)) { mFling = false; break; }
        mSteps += dir;
    }
    if (fabsf(mFlingVel) < 0.05f) mFling = false;
    return mSteps != before || mFling || flash;
}

}  // namespace android
