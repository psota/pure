// Author: James Psota
// File:   condition_frame.h 

// Copyright (c) 2024 James Psota
// __________________________________________

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//   http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef PURE_SUPPORT_CONDITION_FRAME_H
#define PURE_SUPPORT_CONDITION_FRAME_H

#pragma once

#include <condition_variable>
#include <mutex>

class ConditionFrame {

  public:
    ConditionFrame() : ready(false) {}
    explicit ConditionFrame(bool _ready_initial_val) : ready(_ready_initial_val) {}
    ~ConditionFrame() = default;

    inline bool Ready() const {
        // TODO: this shoudl be a lock_guard
        std::unique_lock<std::mutex> lock(mut);
        return ready;
    }

    inline void MarkNotReady() {
        // TODO: this shoudl be a lock_guard
        std::unique_lock<std::mutex> lock(mut);
        ready = false;
    }
    inline void MarkReady() {
        // TODO: this shoudl be a lock_guard
        std::unique_lock<std::mutex> lock(mut);
        ready = true;
    }

    inline void NotifyOne() {
        cv.notify_one();
    }

    inline void NotifyAll() {
        cv.notify_all();
    }

    inline void MarkReadyAndNotifyOne() {
        MarkReady();
        cv.notify_one();
    }

    // num_threads should be total number of threads participating in this condition variable.
    inline void MarkReadyAndNotifyAll(int num_threads = -1) {
        MarkReady();
        if (num_threads == 2) {
            // special case for case of just two threads in the system
            cv.notify_one();
        } else if (num_threads == 1) {
            // special case: do nothing -- no need to notify if number of threads is one
        } else {
            cv.notify_all();
        }
    }

    // note: this function doen't check for Ready. It just immediately waits. Use
    // WaitForReadyUnlessAlreadyReady if you want to just quickly return if already ready.
    inline void WaitForReady() const {

        // TODO(jim): consider double-check locking here for performance improvement

        std::unique_lock<std::mutex> lock(mut);
        cv.wait(lock, [this]() { return ready; });

    } // function WaitForReady

    inline void WaitForReadyUnlessAlreadyReady() const {

        // TODO(jim): consider double-check locking here for performance improvement
        std::unique_lock<std::mutex> lock(mut);
        if (!ready) {
            cv.wait(lock, [this]() { return ready; });
        }
    } // function WaitForReadyUnlessAlreadyReady

  private:
    mutable std::mutex              mut;
    mutable std::condition_variable cv;
    bool                            ready = false;
};
#endif
