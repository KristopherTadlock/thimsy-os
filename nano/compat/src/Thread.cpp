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

// android::Thread for the Linux and host builds (utils/Thread.h has the contract).

#define LOG_TAG "Thread"

#include <utils/Thread.h>

#include <errno.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include <log/log.h>

namespace android {

namespace {

pid_t currentTid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

// Android keeps the last 15 bytes of a longer name (the kernel's limit is 16 with the NUL).
void setCurrentThreadName(const char* name) {
    if (name == nullptr || *name == '\0') return;
    const size_t len = strlen(name);
    const char* s = len > 15 ? name + (len - 15) : name;
    prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(s), 0, 0, 0);
}

void applyPriority(int32_t priority) {
    if (priority == PRIORITY_DEFAULT) return;
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(currentTid()), priority) != 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            ALOGW("can't set thread priority %d: %s (logged once)", static_cast<int>(priority), strerror(errno));
        }
    }
}

struct StartArgs {
    Thread* thread;
    std::string name;
    int32_t priority;
};

}  // namespace

Thread::Thread(bool /*canCallJava*/)
    : mStatus(OK), mExitPending(false), mRunning(false), mThread(), mHaveThread(false), mTid(-1) {}

Thread::~Thread() {}

status_t Thread::readyToRun() { return OK; }

status_t Thread::run(const char* name, int32_t priority, size_t stack) {
    // On failure the thread's reference is dropped after mLock is released: it may be the last one.
    sp<Thread> dropAfterUnlock;
    std::lock_guard<std::mutex> lock(mLock);
    if (mRunning) return INVALID_OPERATION;

    mStatus = OK;
    mExitPending.store(false);
    mHaveThread = false;
    mTid = -1;
    // The thread's strong reference to us, taken over by the thread itself.
    mHoldSelf = this;
    mRunning = true;

    StartArgs* args = new StartArgs{this, name ? name : "", priority};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack > 0) pthread_attr_setstacksize(&attr, stack);
    pthread_t t;
    const int err = pthread_create(&t, &attr, trampoline, args);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        delete args;
        mRunning = false;
        dropAfterUnlock = std::move(mHoldSelf);  // destroyed after `lock`, which is declared later
        ALOGE("run(%s): pthread_create: %s", name ? name : "", strerror(err));
        return UNKNOWN_ERROR;
    }
    mThread = t;
    mHaveThread = true;
    return OK;
}

void* Thread::trampoline(void* arg) {
    StartArgs* args = static_cast<StartArgs*>(arg);
    Thread* const self = args->thread;
    setCurrentThreadName(args->name.c_str());
    applyPriority(args->priority);
    delete args;

    sp<Thread> strong;
    {
        std::lock_guard<std::mutex> lock(self->mLock);
        self->mTid = currentTid();
        strong = self->mHoldSelf;   // run() has returned or is about to: it holds mLock
        self->mHoldSelf.clear();
    }
    wp<Thread> weak(strong);

    bool first = true;
    do {
        bool result;
        if (first) {
            first = false;
            const status_t status = self->readyToRun();
            {
                std::lock_guard<std::mutex> lock(self->mLock);
                self->mStatus = status;
            }
            result = (status == OK);
            if (result && !self->exitPending()) result = self->threadLoop();
        } else {
            result = self->threadLoop();
        }

        {
            std::lock_guard<std::mutex> lock(self->mLock);
            if (!result || self->mExitPending.load()) {
                self->mExitPending.store(true);
                self->mRunning = false;
                self->mHaveThread = false;
                self->mTid = -1;
                self->mThreadExitedCondition.notify_all();
                break;
            }
        }

        // Between two loops only a weak reference is held, so the last outside reference going
        // away ends the thread (and destroys the Thread) here, as on Android.
        strong.clear();
        strong = weak.promote();
    } while (strong != nullptr);

    return nullptr;
}

void Thread::requestExit() {
    std::lock_guard<std::mutex> lock(mLock);
    mExitPending.store(true);
}

status_t Thread::requestExitAndWait() {
    std::unique_lock<std::mutex> lock(mLock);
    if (mHaveThread && pthread_equal(mThread, pthread_self())) {
        ALOGW("Thread (this=%p): don't call requestExitAndWait() from this Thread object's thread; "
              "that would deadlock",
              this);
        return WOULD_BLOCK;
    }
    mExitPending.store(true);
    mThreadExitedCondition.wait(lock, [this] { return !mRunning; });
    // Like Android: the flag is cleared again, so the Thread can be run() again.
    mExitPending.store(false);
    return mStatus;
}

status_t Thread::join() {
    std::unique_lock<std::mutex> lock(mLock);
    if (mHaveThread && pthread_equal(mThread, pthread_self())) {
        ALOGW("Thread (this=%p): don't call join() from this Thread object's thread; that would "
              "deadlock",
              this);
        return WOULD_BLOCK;
    }
    mThreadExitedCondition.wait(lock, [this] { return !mRunning; });
    return mStatus;
}

bool Thread::isRunning() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mRunning;
}

pid_t Thread::getTid() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mRunning ? mTid : -1;
}

bool Thread::exitPending() const { return mExitPending.load(); }

}  // namespace android
