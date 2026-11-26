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

// Thimsy's <utils/StrongPointer.h> for the Linux and host builds of Nano: android::sp<T>, a strong
// reference to an object that counts its own references (RefBase, LightRefBase): T must have
// incStrong(const void*) and decStrong(const void*). Written fresh with libutils' interface.
#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace android {

template <typename T>
class wp;

template <typename T>
class sp {
 public:
    sp() noexcept : m_ptr(nullptr) {}
    sp(std::nullptr_t) noexcept : m_ptr(nullptr) {}  // NOLINT(google-explicit-constructor)

    // Takes a strong reference to other (the first one calls RefBase::onFirstRef).
    sp(T* other) : m_ptr(other) {  // NOLINT(google-explicit-constructor)
        if (other) other->incStrong(this);
    }
    sp(const sp<T>& other) : m_ptr(other.m_ptr) {
        if (m_ptr) m_ptr->incStrong(this);
    }
    sp(sp<T>&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp(U* other) : m_ptr(other) {  // NOLINT(google-explicit-constructor)
        if (other) m_ptr->incStrong(this);
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp(const sp<U>& other) : m_ptr(other.m_ptr) {  // NOLINT(google-explicit-constructor)
        if (m_ptr) m_ptr->incStrong(this);
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp(sp<U>&& other) noexcept : m_ptr(other.m_ptr) {  // NOLINT(google-explicit-constructor)
        other.m_ptr = nullptr;
    }

    ~sp() {
        if (m_ptr) m_ptr->decStrong(this);
    }

    // Allocates a T and holds it, so the object is reference-counted from its first moment.
    template <typename... Args>
    static sp<T> make(Args&&... args) {
        return sp<T>(new T(std::forward<Args>(args)...));
    }
    // A strong reference to an object some sp<> already owns.
    static sp<T> fromExisting(T* other) { return sp<T>(other); }
    // static_cast from sp<U>.
    template <typename U>
    static sp<T> cast(const sp<U>& other) {
        return sp<T>(static_cast<T*>(other.get()));
    }

    sp& operator=(std::nullptr_t) {
        clear();
        return *this;
    }
    sp& operator=(T* other) {
        T* old = m_ptr;
        if (other) other->incStrong(this);
        m_ptr = other;
        if (old) old->decStrong(this);
        return *this;
    }
    sp& operator=(const sp<T>& other) {
        T* old = m_ptr;
        T* next = other.m_ptr;
        if (next) next->incStrong(this);
        m_ptr = next;
        if (old) old->decStrong(this);
        return *this;
    }
    sp& operator=(sp<T>&& other) noexcept {
        if (this == &other) return *this;
        T* old = m_ptr;
        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
        if (old) old->decStrong(this);  // also right when both held the same object
        return *this;
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp& operator=(const sp<U>& other) {
        return *this = sp<T>(other);
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp& operator=(sp<U>&& other) {
        return *this = sp<T>(std::move(other));
    }
    template <typename U, typename = std::enable_if_t<std::is_convertible<U*, T*>::value>>
    sp& operator=(U* other) {
        return *this = static_cast<T*>(other);
    }

    void force_set(T* other) {
        other->forceIncStrong(this);
        m_ptr = other;
    }

    // Drops the reference.
    void clear() {
        T* old = m_ptr;
        if (old) {
            m_ptr = nullptr;
            old->decStrong(this);
        }
    }

    T& operator*() const { return *m_ptr; }
    T* operator->() const { return m_ptr; }
    T* get() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

 private:
    template <typename Y>
    friend class sp;
    template <typename Y>
    friend class wp;

    struct AlreadyCounted {};
    // Wraps a pointer whose strong count was already raised (wp::promote).
    sp(T* p, AlreadyCounted) noexcept : m_ptr(p) {}

    T* m_ptr;
};

// Comparisons by address, with sp, raw pointers and nullptr, as libutils has them.
template <typename T, typename U>
inline bool operator==(const sp<T>& a, const sp<U>& b) { return a.get() == b.get(); }
template <typename T, typename U>
inline bool operator!=(const sp<T>& a, const sp<U>& b) { return a.get() != b.get(); }
template <typename T, typename U>
inline bool operator<(const sp<T>& a, const sp<U>& b) { return std::less<const void*>()(a.get(), b.get()); }
template <typename T, typename U>
inline bool operator>(const sp<T>& a, const sp<U>& b) { return b < a; }
template <typename T, typename U>
inline bool operator<=(const sp<T>& a, const sp<U>& b) { return !(b < a); }
template <typename T, typename U>
inline bool operator>=(const sp<T>& a, const sp<U>& b) { return !(a < b); }
template <typename T, typename U>
inline bool operator==(const sp<T>& a, U* b) { return a.get() == b; }
template <typename T, typename U>
inline bool operator==(T* a, const sp<U>& b) { return a == b.get(); }
template <typename T, typename U>
inline bool operator!=(const sp<T>& a, U* b) { return a.get() != b; }
template <typename T, typename U>
inline bool operator!=(T* a, const sp<U>& b) { return a != b.get(); }
template <typename T>
inline bool operator==(const sp<T>& a, std::nullptr_t) { return a.get() == nullptr; }
template <typename T>
inline bool operator==(std::nullptr_t, const sp<T>& b) { return b.get() == nullptr; }
template <typename T>
inline bool operator!=(const sp<T>& a, std::nullptr_t) { return a.get() != nullptr; }
template <typename T>
inline bool operator!=(std::nullptr_t, const sp<T>& b) { return b.get() != nullptr; }

}  // namespace android
