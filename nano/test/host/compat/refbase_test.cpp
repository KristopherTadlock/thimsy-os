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

// utils/RefBase.h and utils/StrongPointer.h: counts, onFirstRef timing, sp and wp (promote),
// object lifetimes, LightRefBase, and a concurrent hammering of the counts.

#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using android::LightRefBase;
using android::RefBase;
using android::sp;
using android::wp;

namespace {

struct Events {
    int firstRef = 0;
    int lastStrong = 0;
    int lastWeak = 0;
    int destroyed = 0;
    int strongAtFirstRef = -1;
};

class Counted : public RefBase {
 public:
    explicit Counted(Events* e) : e_(e) {}
    ~Counted() override { e_->destroyed++; }
    int value = 7;

 protected:
    void onFirstRef() override {
        e_->firstRef++;
        e_->strongAtFirstRef = getStrongCount();
    }
    void onLastStrongRef(const void*) override { e_->lastStrong++; }
    void onLastWeakRef(const void*) override { e_->lastWeak++; }

    Events* e_;
};

class Derived : public Counted {
 public:
    using Counted::Counted;
};

class WeakLived : public Counted {
 public:
    explicit WeakLived(Events* e) : Counted(e) { extendObjectLifetime(OBJECT_LIFETIME_WEAK); }
};

// Two bases sharing one virtual RefBase, as NanoMenu (Thread, IBinder::DeathRecipient) does.
class BaseA : public virtual RefBase {
 public:
    virtual int a() const { return 1; }
};
class BaseB : public virtual RefBase {
 public:
    virtual int b() const { return 2; }
};
class Both : public BaseA, public BaseB {
 public:
    explicit Both(Events* e) : e_(e) {}
    ~Both() override { e_->destroyed++; }

 protected:
    void onFirstRef() override { e_->firstRef++; }

 private:
    Events* e_;
};

constexpr int32_t kInitialStrong = 1 << 28;

}  // namespace

TEST(RefBase, OnFirstRefRunsOnceAtTheFirstStrongReference) {
    Events e;
    Counted* raw = new Counted(&e);
    EXPECT_EQ(e.firstRef, 0);  // not in the constructor
    EXPECT_EQ(raw->getStrongCount(), kInitialStrong);
    {
        sp<Counted> a = raw;
        EXPECT_EQ(e.firstRef, 1);
        EXPECT_EQ(e.strongAtFirstRef, 1);  // the count is already right inside onFirstRef
        sp<Counted> b = a;
        sp<Counted> c(raw);
        EXPECT_EQ(e.firstRef, 1);
        EXPECT_EQ(raw->getStrongCount(), 3);
    }
    EXPECT_EQ(e.lastStrong, 1);
    EXPECT_EQ(e.destroyed, 1);
}

TEST(RefBase, MovesDoNotTouchTheCount) {
    Events e;
    sp<Counted> a = sp<Counted>::make(&e);
    Counted* raw = a.get();
    sp<Counted> b = std::move(a);
    EXPECT_TRUE(a == nullptr);
    EXPECT_EQ(raw->getStrongCount(), 1);
    sp<Counted> c;
    c = std::move(b);
    EXPECT_EQ(raw->getStrongCount(), 1);
    c = std::move(c);  // self-move keeps it
    EXPECT_EQ(c.get(), raw);
    c = nullptr;
    EXPECT_EQ(e.destroyed, 1);
}

TEST(RefBase, AssignmentAndComparisons) {
    Events e1, e2;
    sp<Counted> a = new Counted(&e1);
    sp<Counted> b = new Counted(&e2);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(static_cast<bool>(a));
    EXPECT_EQ(a->value, 7);
    EXPECT_EQ((*a).value, 7);
    a = b;  // drops the first object
    EXPECT_EQ(e1.destroyed, 1);
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a == b.get());
    a = a;  // self-assignment keeps it
    EXPECT_EQ(b->getStrongCount(), 2);
    a.clear();
    EXPECT_TRUE(a == nullptr);
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(e2.destroyed, 0);
}

TEST(RefBase, UpcastsAndCast) {
    Events e;
    sp<Derived> d = sp<Derived>::make(&e);
    sp<Counted> c = d;
    EXPECT_EQ(d->getStrongCount(), 2);
    sp<Derived> back = sp<Derived>::cast(c);
    EXPECT_EQ(back.get(), d.get());
    wp<Counted> w = d;
    EXPECT_EQ(w.promote().get(), static_cast<Counted*>(d.get()));
}

TEST(RefBase, PromoteWhileAliveAndAfter) {
    Events e;
    wp<Counted> w;
    {
        sp<Counted> s = new Counted(&e);
        w = s;
        sp<Counted> p = w.promote();
        ASSERT_TRUE(p != nullptr);
        EXPECT_EQ(s->getStrongCount(), 2);
    }
    EXPECT_EQ(e.destroyed, 1);
    EXPECT_TRUE(w.promote() == nullptr);  // the weak block outlives the object
    EXPECT_TRUE(w.unsafe_get() != nullptr);
    w.clear();
    EXPECT_TRUE(w == nullptr);
}

TEST(RefBase, WeakReferencesDoNotKeepItAlive) {
    Events e;
    sp<Counted> s = new Counted(&e);
    wp<Counted> w1(s), w2(w1), w3;
    w3 = w2;
    EXPECT_EQ(s->getWeakRefs()->getWeakCount(), 4);  // three wps and the sp
    s.clear();
    EXPECT_EQ(e.destroyed, 1);
    EXPECT_TRUE(w3.promote() == nullptr);
}

TEST(RefBase, PromotingANeverStrongObjectSkipsOnFirstRef) {
    // libutils: attemptIncStrong takes the first strong reference without onFirstRef.
    Events e;
    Counted* raw = new Counted(&e);
    wp<Counted> w(raw);
    {
        sp<Counted> p = w.promote();
        ASSERT_TRUE(p != nullptr);
        EXPECT_EQ(raw->getStrongCount(), 1);
        EXPECT_EQ(e.firstRef, 0);
    }
    EXPECT_EQ(e.destroyed, 1);
    EXPECT_TRUE(w.promote() == nullptr);
}

TEST(RefBase, DeletingANeverStrongObjectWithAWeakReference) {
    Events e;
    Counted* raw = new Counted(&e);
    wp<Counted> w(raw);
    delete raw;  // an owner that never used sp<>
    EXPECT_EQ(e.destroyed, 1);
    EXPECT_TRUE(w.promote() == nullptr);  // and no use after free (ASan)
}

TEST(RefBase, ObjectLifetimeWeak) {
    Events e;
    wp<WeakLived> w;
    {
        sp<WeakLived> s = new WeakLived(&e);
        w = s;
    }
    EXPECT_EQ(e.lastStrong, 1);
    EXPECT_EQ(e.destroyed, 0);  // lives while a weak reference does
    {
        sp<WeakLived> again = w.promote();
        ASSERT_TRUE(again != nullptr);
        EXPECT_EQ(again->value, 7);
    }
    EXPECT_EQ(e.destroyed, 0);
    w.clear();
    EXPECT_EQ(e.lastWeak, 1);
    EXPECT_EQ(e.destroyed, 1);
}

TEST(RefBase, ForceIncStrong) {
    Events e;
    Counted* raw = new Counted(&e);
    raw->forceIncStrong(nullptr);
    EXPECT_EQ(e.firstRef, 1);
    EXPECT_EQ(raw->getStrongCount(), 1);
    raw->decStrong(nullptr);
    EXPECT_EQ(e.destroyed, 1);
}

TEST(RefBase, VirtualBaseSharedByTwoParents) {
    Events e;
    {
        sp<Both> both = new Both(&e);
        sp<BaseA> a = both;
        sp<BaseB> b = both;
        EXPECT_EQ(e.firstRef, 1);
        EXPECT_EQ(both->getStrongCount(), 3);
        EXPECT_EQ(a->a() + b->b(), 3);
        wp<BaseB> wb = b;
        EXPECT_TRUE(wb.promote() != nullptr);
    }
    EXPECT_EQ(e.destroyed, 1);
}

TEST(RefBase, ConcurrentCopiesAndPromotes) {
    Events e;
    sp<Counted> root = new Counted(&e);
    wp<Counted> weak = root;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 20000; i++) {
                sp<Counted> copy = root;
                sp<Counted> promoted = weak.promote();
                wp<Counted> w2 = copy;
                if (!promoted || promoted.get() != copy.get()) abort();
            }
        });
    }
    go = true;
    for (auto& t : threads) t.join();
    EXPECT_EQ(root->getStrongCount(), 1);
    EXPECT_EQ(root->getWeakRefs()->getWeakCount(), 2);
    root.clear();
    EXPECT_EQ(e.destroyed, 1);
    EXPECT_EQ(e.firstRef, 1);
}

namespace {
struct Light : public LightRefBase<Light> {
    explicit Light(int* d) : destroyed(d) {}
    ~Light() { (*destroyed)++; }
    int* destroyed;
};
}  // namespace

TEST(RefBase, LightRefBase) {
    int destroyed = 0;
    {
        sp<Light> a = new Light(&destroyed);
        sp<Light> b = a;
        EXPECT_EQ(a->getStrongCount(), 2);
    }
    EXPECT_EQ(destroyed, 1);
}
