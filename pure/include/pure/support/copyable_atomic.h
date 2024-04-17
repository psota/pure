// Author: James Psota
// File:   copyable_atomic.h 

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

#ifndef COPYABLE_ATOMIC_H
#define COPYABLE_ATOMIC_H

/**
 * Drop in replacement for std::atomic that provides a copy constructor and copy
 * assignment operator. via:
 * https://codereview.stackexchange.com/questions/113439/copyable-atomic
 *
 * Contrary to normal atomics, these atomics don't prevent the generation of
 * default constructor and copy operators for classes they are members of.
 *
 * Copying those atomics is thread safe, but be aware that
 * it doesn't provide any form of synchronization.
 */

#include <atomic>

template <class T>
class CopyableAtomic : public std::atomic<T> {
  public:
    // defaultinitializes value
    CopyableAtomic() = default;

    constexpr CopyableAtomic(T desired) : std::atomic<T>(desired) {}

    constexpr CopyableAtomic(const CopyableAtomic<T>& other)
        : CopyableAtomic(other.load(std::memory_order_acquire)) {}

    CopyableAtomic& operator=(const CopyableAtomic<T>& other) {
        this->store(other.load(std::memory_order_acquire),
                    std::memory_order_release);
        return *this;
    }
};

#endif