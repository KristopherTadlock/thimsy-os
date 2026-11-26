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

// Thimsy's <utils/RefBase.h> for the Linux and host builds of Nano: libutils' reference-counted
// base class, weak pointers and LightRefBase, written fresh with libutils' interface and
// semantics (IMPLEMENTATION.md B.2):
//
//   - The strong count starts at a sentinel (getStrongCount() returns 1 << 28 until the first
//     strong reference). The first incStrong() calls onFirstRef(), once, before it returns.
//   - The last decStrong() calls onLastStrongRef() and, for the default OBJECT_LIFETIME_STRONG,
//     deletes the object. The weak-reference block lives until the last weak reference goes.
//   - wp<T>::promote() returns a strong reference while the object lives and null after; promoting
//     an object that never had a strong reference succeeds and does not call onFirstRef(), as in
//     libutils.
//   - With OBJECT_LIFETIME_WEAK the object lives until the last weak reference goes.
//   - Destroying an object that still has strong references aborts.
#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <type_traits>

#include <utils/StrongPointer.h>

namespace android {

class RefBase {
 public:
    void incStrong(const void* id) const;
    // As incStrong, but aborts if there is no strong reference yet.
    void incStrongRequireStrong(const void* id) const;
    void decStrong(const void* id) const;
    // As incStrong, but also calls onFirstRef() when the count is back up from 0.
    void forceIncStrong(const void* id) const;
    // The strong count; 1 << 28 before the first strong reference. For debugging only.
    int32_t getStrongCount() const;

    class weakref_type {
     public:
        RefBase* refBase() const;
        void incWeak(const void* id);
        void incWeakRequireWeak(const void* id);
        void decWeak(const void* id);
        // A strong reference if the object still lives; false (and no reference) otherwise.
        bool attemptIncStrong(const void* id);
        bool attemptIncWeak(const void* id);
        int32_t getWeakCount() const;
        void printRefs() const {}
        void trackMe(bool, bool) {}

     protected:
        weakref_type() = default;
        ~weakref_type() = default;
    };

    weakref_type* createWeak(const void* id) const;
    weakref_type* getWeakRefs() const;

    void printRefs() const {}
    void trackMe(bool, bool) {}

    typedef RefBase basetype;

 protected:
    RefBase();
    virtual ~RefBase();

    enum {
        OBJECT_LIFETIME_STRONG = 0x0000,
        OBJECT_LIFETIME_WEAK = 0x0001,
        OBJECT_LIFETIME_MASK = 0x0001,
    };
    void extendObjectLifetime(int32_t mode);

    enum {
        FIRST_INC_STRONG = 0x0001,
    };

    // Called when the first strong reference is taken (the object is fully constructed by then).
    virtual void onFirstRef();
    // Called when the last strong reference goes, before the object is deleted.
    virtual void onLastStrongRef(const void* id);
    // OBJECT_LIFETIME_WEAK only: whether a weak reference may become strong again.
    virtual bool onIncStrongAttempted(uint32_t flags, const void* id);
    // OBJECT_LIFETIME_WEAK only: called when the last weak reference goes, before deletion.
    virtual void onLastWeakRef(const void* id);

 private:
    friend class weakref_type;
    class weakref_impl;

    RefBase(const RefBase&) = delete;
    RefBase& operator=(const RefBase&) = delete;

    weakref_impl* const mRefs;
};

// A small strong-only count (no weak references), as in libutils.
template <class T>
class LightRefBase {
 public:
    LightRefBase() : mCount(0) {}
    void incStrong(const void* /*id*/) const { mCount.fetch_add(1, std::memory_order_relaxed); }
    void decStrong(const void* /*id*/) const {
        if (mCount.fetch_sub(1, std::memory_order_release) == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete static_cast<const T*>(this);
        }
    }
    int32_t getStrongCount() const { return mCount.load(std::memory_order_relaxed); }

    typedef LightRefBase<T> basetype;

 protected:
    ~LightRefBase() = default;

 private:
    mutable std::atomic<int32_t> mCount;
};

// LightRefBase with a virtual destructor.
class VirtualLightRefBase : public LightRefBase<VirtualLightRefBase> {
 public:
    virtual ~VirtualLightRefBase() = default;
};

// A weak reference: does not keep the object alive; promote() gives a strong one if it lives.
template <typename T>
class wp {
 public:
    typedef typename RefBase::weakref_type weakref_type;

    wp() : m_ptr(nullptr), m_refs(nullptr) {}
    wp(std::nullptr_t) : wp() {}  // NOLINT(google-explicit-constructor)
    wp(T* other) : m_ptr(other), m_refs(other ? other->createWeak(this) : nullptr) {}  // NOLINT
    wp(const wp<T>& other) : m_ptr(other.m_ptr), m_refs(other.m_refs) {
        if (m_ptr) m_refs->incWeak(this);
    }
    wp(wp<T>&& other) noexcept : m_ptr(other.m_ptr), m_refs(other.m_refs) {
        other.m_ptr = nullptr;
        other.m_refs = nullptr;
    }
    wp(const sp<T>& other)  // NOLINT(google-explicit-constructor)
        : m_ptr(other.get()), m_refs(m_ptr ? m_ptr->createWeak(this) : nullptr) {}
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    wp(U* other)  // NOLINT(google-explicit-constructor)
        : m_ptr(other), m_refs(other ? other->createWeak(this) : nullptr) {}
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    wp(const sp<U>& other)  // NOLINT(google-explicit-constructor)
        : m_ptr(other.get()), m_refs(m_ptr ? m_ptr->createWeak(this) : nullptr) {}
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    wp(const wp<U>& other)  // NOLINT(google-explicit-constructor)
        : m_ptr(other.m_ptr), m_refs(other.m_refs) {
        if (m_ptr) m_refs->incWeak(this);
    }

    ~wp() {
        if (m_ptr) m_refs->decWeak(this);
    }

    wp& operator=(std::nullptr_t) {
        clear();
        return *this;
    }
    wp& operator=(const wp<T>& other) {
        weakref_type* refs = other.m_refs;
        T* ptr = other.m_ptr;
        if (ptr) refs->incWeak(this);
        if (m_ptr) m_refs->decWeak(this);
        m_ptr = ptr;
        m_refs = refs;
        return *this;
    }
    wp& operator=(wp<T>&& other) noexcept {
        if (this == &other) return *this;
        if (m_ptr) m_refs->decWeak(this);
        m_ptr = other.m_ptr;
        m_refs = other.m_refs;
        other.m_ptr = nullptr;
        other.m_refs = nullptr;
        return *this;
    }
    wp& operator=(T* other) {
        weakref_type* refs = other ? other->createWeak(this) : nullptr;
        if (m_ptr) m_refs->decWeak(this);
        m_ptr = other;
        m_refs = refs;
        return *this;
    }
    wp& operator=(const sp<T>& other) { return *this = other.get(); }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    wp& operator=(const wp<U>& other) {
        return *this = wp<T>(other);
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    wp& operator=(const sp<U>& other) {
        return *this = static_cast<T*>(other.get());
    }

    void set_object_and_refs(T* other, weakref_type* refs) {
        if (other) refs->incWeak(this);
        if (m_ptr) m_refs->decWeak(this);
        m_ptr = other;
        m_refs = other ? refs : nullptr;
    }

    // A strong reference to the object, or null if it has been destroyed.
    sp<T> promote() const {
        if (m_ptr && m_refs->attemptIncStrong(this)) {
            return sp<T>(m_ptr, typename sp<T>::AlreadyCounted());
        }
        return sp<T>();
    }

    void clear() {
        if (m_ptr) {
            m_refs->decWeak(this);
            m_ptr = nullptr;
            m_refs = nullptr;
        }
    }

    weakref_type* get_refs() const { return m_refs; }
    // The raw pointer, which may dangle. For comparisons and debugging only.
    T* unsafe_get() const { return m_ptr; }

    template <typename U>
    bool operator==(const wp<U>& o) const { return m_refs == o.m_refs; }
    template <typename U>
    bool operator!=(const wp<U>& o) const { return m_refs != o.m_refs; }
    template <typename U>
    bool operator<(const wp<U>& o) const {
        return std::less<const void*>()(m_ptr, o.m_ptr) ||
               (m_ptr == o.m_ptr && std::less<const void*>()(m_refs, o.m_refs));
    }
    bool operator==(std::nullptr_t) const { return m_ptr == nullptr; }
    bool operator!=(std::nullptr_t) const { return m_ptr != nullptr; }

 private:
    template <typename Y>
    friend class wp;
    template <typename Y>
    friend class sp;

    T* m_ptr;
    weakref_type* m_refs;
};

}  // namespace android
