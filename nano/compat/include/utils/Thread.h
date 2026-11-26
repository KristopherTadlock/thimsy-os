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

// Thimsy's <utils/Thread.h> for the Linux and host builds of Nano: libutils' android::Thread on
// pthreads, written fresh with libutils' interface and semantics (IMPLEMENTATION.md B.2):
//
//   - run() starts a detached thread that calls readyToRun() once, then threadLoop() while it
//     returns true and no exit is pending. The thread holds a strong reference to the Thread while
//     readyToRun() and each threadLoop() run, and only a weak one between two calls, so a Thread
//     nobody else references ends after its current threadLoop() and is destroyed, as on Android.
//   - The name is set on the thread (the last 15 bytes when it is longer, as Android does). A
//     priority other than PRIORITY_DEFAULT is applied with setpriority() on the new thread, so
//     PRIORITY_DISPLAY is nice -4 (a failure, e.g. without CAP_SYS_NICE, is logged once).
//   - requestExit() only sets the flag; requestExitAndWait() and join() wait for the thread and
//     return WOULD_BLOCK when called from it.
#pragma once

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/ThreadDefs.h>

namespace android {

class Thread : virtual public RefBase {
 public:
    // canCallJava is accepted and ignored: there is no Java VM.
    explicit Thread(bool canCallJava = true);
    ~Thread() override;

    // Starts the thread. INVALID_OPERATION if it is already running, UNKNOWN_ERROR if it can't be
    // created.
    virtual status_t run(const char* name, int32_t priority = PRIORITY_DEFAULT, size_t stack = 0);

    // Asks the thread to stop after the current threadLoop() returns. Does not wait.
    virtual void requestExit();

    // Called once on the new thread before the first threadLoop(); anything but OK ends the thread
    // and becomes the status that join() returns.
    virtual status_t readyToRun();

    // requestExit(), then waits for the thread to end. WOULD_BLOCK from the thread itself.
    status_t requestExitAndWait();

    // Waits for the thread to end, without asking it to. WOULD_BLOCK from the thread itself.
    status_t join();

    bool isRunning() const;

    // The kernel thread id while running, else -1.
    pid_t getTid() const;

 protected:
    // Whether requestExit() has been called.
    bool exitPending() const;

 private:
    // Runs repeatedly while it returns true and no exit is pending.
    virtual bool threadLoop() = 0;

    Thread& operator=(const Thread&) = delete;
    Thread(const Thread&) = delete;

    static void* trampoline(void* arg);

    mutable std::mutex mLock;
    std::condition_variable mThreadExitedCondition;
    status_t mStatus;
    // Written under mLock, read without it by exitPending() (as Android's volatile flag is).
    std::atomic<bool> mExitPending;
    bool mRunning;
    sp<Thread> mHoldSelf;
    pthread_t mThread;
    bool mHaveThread;
    pid_t mTid;
};

}  // namespace android
