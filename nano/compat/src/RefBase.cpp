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

// android::RefBase for the Linux and host builds (utils/RefBase.h has the contract).

#define LOG_TAG "RefBase"

#include <utils/RefBase.h>

#include <log/log.h>

namespace android {

namespace {

// The strong count before the first strong reference.
constexpr int32_t INITIAL_STRONG_VALUE = 1 << 28;

}  // namespace

class RefBase::weakref_impl : public RefBase::weakref_type {
 public:
    explicit weakref_impl(RefBase* base)
        : mStrong(INITIAL_STRONG_VALUE), mWeak(0), mBase(base), mFlags(OBJECT_LIFETIME_STRONG) {}

    std::atomic<int32_t> mStrong;
    std::atomic<int32_t> mWeak;
    RefBase* const mBase;
    std::atomic<int32_t> mFlags;
};

RefBase::RefBase() : mRefs(new weakref_impl(this)) {}

RefBase::~RefBase() {
    const int32_t flags = mRefs->mFlags.load(std::memory_order_relaxed);
    if ((flags & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_WEAK) {
        // Deleted by the last decWeak (or explicitly with no weak references left).
        if (mRefs->mWeak.load(std::memory_order_relaxed) == 0) delete mRefs;
    } else {
        const int32_t strong = mRefs->mStrong.load(std::memory_order_relaxed);
        if (strong == INITIAL_STRONG_VALUE) {
            // Never strongly referenced: nothing else will free the block, unless weak references
            // remain, whose last decWeak must not touch a freed block.
            if (mRefs->mWeak.load(std::memory_order_relaxed) == 0) {
                delete mRefs;
            } else {
                // libutils leaves the block to the weak references too; they see a dead object.
                mRefs->mStrong.store(0, std::memory_order_relaxed);
            }
        } else if (strong != 0) {
            LOG_ALWAYS_FATAL("RefBase %p destroyed with %d strong reference(s) left", this, strong);
        }
        // strong == 0: decStrong deleted us; the last decWeak frees the block.
    }
    const_cast<weakref_impl*&>(mRefs) = nullptr;
}

void RefBase::incStrong(const void* id) const {
    weakref_impl* const refs = mRefs;
    refs->incWeak(id);
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
    if (c != INITIAL_STRONG_VALUE) return;
    refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
    refs->mBase->onFirstRef();
}

void RefBase::incStrongRequireStrong(const void* id) const {
    weakref_impl* const refs = mRefs;
    refs->incWeak(id);
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
    LOG_ALWAYS_FATAL_IF(c <= 0 || c == INITIAL_STRONG_VALUE,
                        "incStrongRequireStrong on %p with no strong reference (count %d)", this, c);
}

void RefBase::decStrong(const void* id) const {
    weakref_impl* const refs = mRefs;
    const int32_t c = refs->mStrong.fetch_sub(1, std::memory_order_release);
    LOG_ALWAYS_FATAL_IF(c <= 0, "decStrong on %p with no strong reference (count %d)", this, c);
    if (c == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        refs->mBase->onLastStrongRef(id);
        const int32_t flags = refs->mFlags.load(std::memory_order_relaxed);
        if ((flags & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_STRONG) delete this;
    }
    // The block outlives the object while weak references (including ours) remain.
    refs->decWeak(id);
}

void RefBase::forceIncStrong(const void* id) const {
    weakref_impl* const refs = mRefs;
    refs->incWeak(id);
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
    switch (c) {
        case INITIAL_STRONG_VALUE:
            refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
            [[fallthrough]];
        case 0:
            refs->mBase->onFirstRef();
            break;
        default:
            break;
    }
}

int32_t RefBase::getStrongCount() const { return mRefs->mStrong.load(std::memory_order_relaxed); }

RefBase* RefBase::weakref_type::refBase() const { return static_cast<const weakref_impl*>(this)->mBase; }

void RefBase::weakref_type::incWeak(const void* /*id*/) {
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    const int32_t c = impl->mWeak.fetch_add(1, std::memory_order_relaxed);
    LOG_ALWAYS_FATAL_IF(c < 0, "incWeak on %p after its last weak reference", this);
}

void RefBase::weakref_type::incWeakRequireWeak(const void* /*id*/) {
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    const int32_t c = impl->mWeak.fetch_add(1, std::memory_order_relaxed);
    LOG_ALWAYS_FATAL_IF(c <= 0, "incWeakRequireWeak on %p with no weak reference", this);
}

void RefBase::weakref_type::decWeak(const void* id) {
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    const int32_t c = impl->mWeak.fetch_sub(1, std::memory_order_release);
    LOG_ALWAYS_FATAL_IF(c <= 0, "decWeak on %p with no weak reference", this);
    if (c != 1) return;
    std::atomic_thread_fence(std::memory_order_acquire);

    const int32_t flags = impl->mFlags.load(std::memory_order_relaxed);
    if ((flags & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_STRONG) {
        const int32_t strong = impl->mStrong.load(std::memory_order_relaxed);
        if (strong == INITIAL_STRONG_VALUE) {
            // Weak references only, and the object was never strongly referenced: its owner deletes
            // it, and ~RefBase frees the block (libutils does the same).
        } else {
            // The object is gone (strong count 0, or ~RefBase ran with weak references left).
            delete impl;
        }
    } else {
        // OBJECT_LIFETIME_WEAK: the last weak reference deletes the object (and ~RefBase the block).
        impl->mBase->onLastWeakRef(id);
        delete impl->mBase;
    }
}

bool RefBase::weakref_type::attemptIncStrong(const void* id) {
    incWeak(id);
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    int32_t curCount = impl->mStrong.load(std::memory_order_relaxed);
    while (curCount > 0 && curCount != INITIAL_STRONG_VALUE) {
        if (impl->mStrong.compare_exchange_weak(curCount, curCount + 1, std::memory_order_relaxed)) break;
    }
    if (curCount <= 0 || curCount == INITIAL_STRONG_VALUE) {
        const int32_t flags = impl->mFlags.load(std::memory_order_relaxed);
        if ((flags & OBJECT_LIFETIME_MASK) == OBJECT_LIFETIME_STRONG) {
            if (curCount <= 0) {
                // The object is gone or going.
                decWeak(id);
                return false;
            }
            // Never strongly referenced yet: take the first strong reference.
            while (curCount > 0) {
                if (impl->mStrong.compare_exchange_weak(curCount, curCount + 1, std::memory_order_relaxed)) break;
            }
            if (curCount <= 0) {
                decWeak(id);
                return false;
            }
        } else {
            // OBJECT_LIFETIME_WEAK: the object still lives; it may refuse.
            if (!impl->mBase->onIncStrongAttempted(FIRST_INC_STRONG, id)) {
                decWeak(id);
                return false;
            }
            curCount = impl->mStrong.fetch_add(1, std::memory_order_relaxed);
            if (curCount != 0 && curCount != INITIAL_STRONG_VALUE) {
                impl->mBase->onLastStrongRef(id);
            }
        }
    }
    // Undo the sentinel if we were first; a concurrent first incStrong undoes it itself.
    if (curCount == INITIAL_STRONG_VALUE) {
        impl->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
    }
    return true;
}

bool RefBase::weakref_type::attemptIncWeak(const void* /*id*/) {
    weakref_impl* const impl = static_cast<weakref_impl*>(this);
    int32_t curCount = impl->mWeak.load(std::memory_order_relaxed);
    while (curCount > 0) {
        if (impl->mWeak.compare_exchange_weak(curCount, curCount + 1, std::memory_order_relaxed)) break;
    }
    return curCount > 0;
}

int32_t RefBase::weakref_type::getWeakCount() const {
    return static_cast<const weakref_impl*>(this)->mWeak.load(std::memory_order_relaxed);
}

RefBase::weakref_type* RefBase::createWeak(const void* id) const {
    mRefs->incWeak(id);
    return mRefs;
}

RefBase::weakref_type* RefBase::getWeakRefs() const { return mRefs; }

void RefBase::extendObjectLifetime(int32_t mode) {
    mRefs->mFlags.fetch_or(mode, std::memory_order_relaxed);
}

void RefBase::onFirstRef() {}

void RefBase::onLastStrongRef(const void* /*id*/) {}

bool RefBase::onIncStrongAttempted(uint32_t flags, const void* /*id*/) {
    return (flags & FIRST_INC_STRONG) != 0;
}

void RefBase::onLastWeakRef(const void* /*id*/) {}

}  // namespace android
