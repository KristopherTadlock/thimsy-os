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

#include "NanoButtonMapNav.h"

#include <algorithm>
#include <cmath>

namespace android {
namespace bmap {

namespace {
// NanoTouch's values, so a flick here feels like one in Nano's lists.
constexpr float kFlingMinVel = 0.3f;       // px per ms at the lift
constexpr int64_t kFlingStaleMs = 80;      // the finger held still this long before the lift: no flick
constexpr float kFlingStopVel = 0.05f;
constexpr float kFriction = 325.0f;        // velocity *= exp(-dt / kFriction)
}  // namespace

void Nav::setModel(const NavModel& m) {
    mModel = m;
    clamp();
}

// Pages from the top: each starts at the first item that doesn't wholly fit on the one before.
std::vector<float> Nav::pageStarts() const {
    std::vector<float> starts{0};
    float start = 0;
    for (size_t i = 0; i < mModel.itemY.size() && i < mModel.itemH.size(); i++) {
        const float bottom = (float)(mModel.itemY[i] + mModel.itemH[i]);
        if (bottom - start > mModel.viewH + 0.5f && mModel.itemY[i] > start + 0.5f) {
            start = (float)mModel.itemY[i];
            starts.push_back(start);
        }
    }
    return starts;
}

// The last page starts at an item, so the scroll goes that far (with room below on that page)
// rather than stopping where the content's end meets the body's.
float Nav::maxScroll() const { return pageStarts().back(); }

void Nav::clamp() { mScroll = std::clamp(mScroll, 0.0f, maxScroll()); }

std::vector<float> Nav::snaps() const {
    std::vector<float> s;
    const float mx = maxScroll();
    for (int y : mModel.itemY) s.push_back(std::min((float)y, mx));
    s.push_back(mx);
    s.push_back(0);
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end(), [](float a, float b) { return fabsf(a - b) < 0.5f; }), s.end());
    return s;
}

int Nav::topItem() const {
    for (size_t i = 0; i < mModel.itemY.size(); i++)
        if (mModel.itemY[i] + mModel.itemH[i] > mScroll + 0.5f) return (int)i;
    return mModel.itemY.empty() ? 0 : (int)mModel.itemY.size() - 1;
}

int Nav::pageCount() const { return (int)pageStarts().size(); }

int Nav::page() const {
    const std::vector<float> starts = pageStarts();
    int p = 1;
    for (size_t i = 1; i < starts.size(); i++)
        if (starts[i] <= mScroll + 0.5f) p = (int)i + 1;
    return p;
}

int Nav::section() const {
    const int top = topItem();
    int s = 0;
    for (size_t i = 0; i < mModel.sections.size(); i++)
        if (mModel.sections[i] <= top) s = (int)i;
    return s;
}

void Nav::scrollTo(float px) {
    mScroll = px;
    clamp();
}

bool Nav::lineDown() {
    const float was = mScroll;
    for (float s : snaps())
        if (s > mScroll + 0.5f) { mScroll = s; break; }
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::lineUp() {
    const float was = mScroll;
    const std::vector<float> s = snaps();
    for (auto it = s.rbegin(); it != s.rend(); ++it)
        if (*it < mScroll - 0.5f) { mScroll = *it; break; }
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::pageDown() {
    const float was = mScroll;
    float target = -1;
    // The first item not wholly on screen goes to the top.
    for (size_t i = 0; i < mModel.itemY.size(); i++)
        if (mModel.itemY[i] + mModel.itemH[i] > mScroll + mModel.viewH + 0.5f) { target = (float)mModel.itemY[i]; break; }
    if (target < 0) target = maxScroll();
    if (target <= mScroll + 0.5f) target = mScroll + mModel.viewH;   // one item taller than the view
    mScroll = target;
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::pageUp() {
    const float was = mScroll;
    // The view that ends where this one starts.
    float target = 0;
    for (float s : snaps())
        if (s >= mScroll - mModel.viewH - 0.5f) { target = s; break; }
    if (target >= mScroll - 0.5f) {
        const std::vector<float> s = snaps();
        target = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it)
            if (*it < mScroll - 0.5f) { target = *it; break; }
    }
    mScroll = target;
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::nextSection() {
    const float was = mScroll;
    for (int s : mModel.sections)
        if (s >= 0 && s < (int)mModel.itemY.size() && mModel.itemY[s] > mScroll + 0.5f) {
            mScroll = (float)mModel.itemY[s];
            break;
        }
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::prevSection() {
    const float was = mScroll;
    float target = 0;
    for (int s : mModel.sections)
        if (s >= 0 && s < (int)mModel.itemY.size() && mModel.itemY[s] < mScroll - 0.5f) target = (float)mModel.itemY[s];
    mScroll = target;
    clamp();
    return fabsf(mScroll - was) > 0.25f;
}

bool Nav::apply(NavAction a) {
    switch (a) {
        case NavAction::kLineUp: return lineUp();
        case NavAction::kLineDown: return lineDown();
        case NavAction::kPageUp: return pageUp();
        case NavAction::kPageDown: return pageDown();
        case NavAction::kPrevSection: return prevSection();
        case NavAction::kNextSection: return nextSection();
        default: return false;
    }
}

// ---- touch ----

void Nav::setTouchSpace(float touchW, float touchH, float layoutW, float layoutH) {
    mSx = touchW > 0 ? layoutW / touchW : 1;
    mSy = touchH > 0 ? layoutH / touchH : 1;
}

void Nav::pointers(int count) {
    if (count >= 2) {
        mMulti = true;
        mDown = false;
        mFling = false;
    } else if (count == 0) {
        mMulti = false;
    }
}

void Nav::touchDown(float x, float y, int64_t ms) {
    if (mMulti) return;
    x *= mSx; y *= mSy;
    mStoppedFling = mFling;   // a finger landing on a fling stops it and taps nothing
    mFling = false;
    mDown = true;
    mMoved = false;
    mAxis = 0;
    mX0 = mX = x;
    mY0 = mY = y;
    mScroll0 = mScroll;
    mVel = 0;
    mLastPos = y;
    mLastMs = ms;
}

void Nav::touchMove(float x, float y, int64_t ms) {
    if (!mDown || mMulti) return;
    x *= mSx; y *= mSy;
    if (x == mX && y == mY) return;
    mX = x; mY = y;
    const float dx = x - mX0, dy = y - mY0;
    if (!mMoved) {
        if (dx * dx + dy * dy < mModel.slop * mModel.slop) return;
        mMoved = true;
        mAxis = fabsf(dx) > fabsf(dy) ? 1 : 2;
        // The drag starts from where the finger crossed the slop, so the content doesn't jump.
        mY0 = y;
        mScroll0 = mScroll;
        mLastPos = y;
        mLastMs = ms;
        return;
    }
    if (mAxis != 2) return;
    if (ms > mLastMs) {
        const float v = (y - mLastPos) / (float)(ms - mLastMs);
        mVel = ms - mLastMs > 60 ? v : mVel * 0.4f + v * 0.6f;
    }
    mLastPos = y;
    mLastMs = ms;
    mScroll = mScroll0 - (y - mY0);
    clamp();
}

NavAction Nav::touchUp(int64_t ms) {
    if (!mDown || mMulti) {
        mDown = false;
        return NavAction::kNone;
    }
    mDown = false;
    if (!mMoved) {
        if (mStoppedFling) return NavAction::kNone;
        return mModel.panel.contains(mX0, mY0) ? NavAction::kNone : NavAction::kClose;
    }
    if (mAxis == 1) {
        const float dx = mX - mX0;
        if (fabsf(dx) < mModel.swipeMin) return NavAction::kNone;
        const NavAction a = dx < 0 ? NavAction::kNextSection : NavAction::kPrevSection;
        apply(a);
        return a;
    }
    if (fabsf(mVel) > kFlingMinVel && ms - mLastMs < kFlingStaleMs) {
        const float v = -mVel;   // the content moves against the finger
        if ((v > 0 && canDown()) || (v < 0 && canUp())) {
            mFling = true;
            mFlingVel = v;
            mFlingMs = ms;
        }
    }
    return NavAction::kNone;
}

void Nav::touchCancel() {
    mDown = false;
    mFling = false;
}

bool Nav::tick(int64_t ms) {
    if (!mFling || mDown) return false;
    const float dt = std::clamp((float)(ms - mFlingMs), 0.0f, 100.0f);
    mFlingMs = ms;
    mScroll += mFlingVel * dt;
    mFlingVel *= expf(-dt / kFriction);
    const float mx = maxScroll();
    if (mScroll <= 0 || mScroll >= mx) {   // the end stops it
        mScroll = std::clamp(mScroll, 0.0f, mx);
        mFling = false;
        return true;
    }
    if (fabsf(mFlingVel) < kFlingStopVel) mFling = false;
    return true;
}

// ---- buttons and the stick ----

NavConfig navConfigFor(const PadModel& pad) {
    NavConfig c;
    const std::vector<Phys> back = pad.physFor(Input::evKey(305));    // BTN_EAST: Back in Nano
    const std::vector<Phys> toggle = pad.physFor(Input::evKey(308));  // BTN_WEST
    if (!back.empty()) c.back = back[0];
    if (!toggle.empty()) c.toggle = toggle[0];
    return c;
}

void NavInput::hold(NavAction a, int64_t ms) {
    mHeld = a;
    mNext = ms + mCfg.repeatDelayMs;
}

NavAction NavInput::button(Phys p, bool down, int64_t ms) {
    if (!down) {
        if (p == mHeldPhys && !mHeldByStick) { mHeld = NavAction::kNone; mHeldPhys = Phys::kCount; }
        return NavAction::kNone;
    }
    if (p == mCfg.back || p == Phys::Start) return NavAction::kClose;
    if (p == mCfg.toggle) return NavAction::kToggleView;
    NavAction a = NavAction::kNone;
    switch (p) {
        case Phys::DUp: a = NavAction::kLineUp; break;
        case Phys::DDown: a = NavAction::kLineDown; break;
        case Phys::DLeft: a = NavAction::kPrevSection; break;
        case Phys::DRight: a = NavAction::kNextSection; break;
        case Phys::L1: a = NavAction::kPageUp; break;
        case Phys::R1: a = NavAction::kPageDown; break;
        default: return NavAction::kNone;
    }
    mHeldPhys = p;
    mHeldByStick = false;
    hold(a, ms);
    return a;
}

NavAction NavInput::stick(float x, float y, int64_t ms) {
    const float ax = fabsf(x), ay = fabsf(y);
    NavAction want = NavAction::kNone;
    const float need = mHeldByStick && mHeld != NavAction::kNone ? mCfg.release : mCfg.deadzone;
    if (std::max(ax, ay) >= need) {
        if (ay >= ax) want = y < 0 ? NavAction::kLineUp : NavAction::kLineDown;
        else want = x < 0 ? NavAction::kPrevSection : NavAction::kNextSection;
    }
    if (mHeldByStick && want == mHeld) return NavAction::kNone;   // still held: tick repeats it
    if (want == NavAction::kNone) {
        if (mHeldByStick) { mHeld = NavAction::kNone; mHeldByStick = false; }
        return NavAction::kNone;
    }
    // A new direction past the dead zone: the edge acts now.
    if (mHeldByStick && std::max(ax, ay) < mCfg.deadzone) {
        mHeld = NavAction::kNone;   // turned off the held direction without crossing the dead zone
        mHeldByStick = false;
        return NavAction::kNone;
    }
    mHeldByStick = true;
    mHeldPhys = Phys::kCount;
    hold(want, ms);
    return want;
}

NavAction NavInput::tick(int64_t ms) {
    if (mHeld == NavAction::kNone || ms < mNext) return NavAction::kNone;
    mNext += mCfg.repeatMs;
    if (mNext <= ms) mNext = ms + mCfg.repeatMs;   // a late tick doesn't burst
    return mHeld;
}

}  // namespace bmap
}  // namespace android
