// Author: James Psota
// File:   striped_lock.h 

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

#ifndef STRIPED_LOCKS_H
#define STRIPED_LOCKS_H

#include "pure/support/helpers.h"
#include <array>
#include <mutex>

#define FOLLY_NO_CONFIG
#include <folly/SpinLock.h>
#include <folly/synchronization/MicroSpinLock.h>

/*
  The mutex case is complicated to use padding for the linux case. We special
  case LINUX && !USE_SPIN_LOCK
  */

// relies on USE_SPIN_LOCK being defined

#if USE_SPIN_LOCK
using Locktype = folly::SpinLock;
#else
using Locktype = std::mutex;

#if LINUX
static_assert(sizeof(std::mutex) < CACHE_LINE_BYTES);
struct PaddedMutex {
    alignas(CACHE_LINE_BYTES) std::mutex lock;
    char pad0[CACHE_LINE_BYTES - sizeof(std::mutex)];
};
static_assert(sizeof(PaddedMutex) == CACHE_LINE_BYTES);
#endif
#endif

template <size_t num_locks>
class StripedLock {
  public:
    Locktype& get_lock(int key) {
        const auto idx = int_hash(key) % num_locks;
#if USE_SPIN_LOCK || OSX
        return locks[idx];
#else
        // Linux mutex array
        return locks[idx].lock;
#endif
    }

  private:
#if USE_SPIN_LOCK
    folly::SpinLockArray<Locktype, num_locks> locks;
#else // mutex
#if LINUX
    std::array<PaddedMutex, num_locks> locks;
#else // OSX
    // right now we're not padding a std::mutex because it seems to already take
    // an entire cache line (!)
    static_assert(sizeof(std::mutex) % CACHE_LINE_BYTES == 0);
    std::array<std::mutex, num_locks> locks;
#endif
#endif
};

#endif