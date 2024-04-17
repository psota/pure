// Author: James Psota
// File:   synchronized_vector.h 

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

#ifndef SYNCHRONIZED_VECTOR_H
#define SYNCHRONIZED_VECTOR_H

#include <atomic>
#include <vector>

using std::vector;

#define USE_ATOMIC_FLAG 0

template <typename T>
struct SynchronizedVector {

  public:
    SynchronizedVector() = default;
    explicit SynchronizedVector(int size) : vec(size) {}

    auto size() const noexcept { return vec.size(); }

    T*       data() noexcept { return vec.data(); };
    const T* data() const noexcept { return vec.data(); }

    T&       operator[](int idx) { return vec[idx]; }
    const T& operator[](int idx) const { return vec[idx]; }

    // this code is duplicated -- not sure how best to refactor given the const
    // argument
    void locked_push_back(const T& val) {
#if USE_ATOMIC_FLAG
        while (lock.test_and_set(std::memory_order_acquire))
            ; // spin
#else
        std::lock_guard<std::mutex> lg(lock);
#endif
        vec.push_back(val);

#if USE_ATOMIC_FLAG
        lock.clear(std::memory_order_release);
#endif
    }

    void locked_push_back(T&& val) {
#if USE_ATOMIC_FLAG
        while (lock.test_and_set(std::memory_order_acquire))
            ; // spin
#else
        std::lock_guard<std::mutex> lg(lock);
#endif
        vec.push_back(val);

#if USE_ATOMIC_FLAG
        lock.clear(std::memory_order_release);
#endif
    }

    // this code is duplicated -- not sure how best to refactor given the const
    // argument we are forcing the user to call "unlocked" here to acknowledge
    // it.
    void unlocked_push_back(const T& val) { vec.push_back(val); }
    void unlocked_push_back(T&& val) { vec.push_back(val); }

  public:
    alignas(CACHE_LINE_BYTES) vector<T> vec;

  private:
#if USE_ATOMIC_FLAG
    // details on atomic_flag:
    // https://en.cppreference.com/w/cpp/atomic/atomic_flag
    std::atomic_flag lock;
    char pad0[CACHE_LINE_BYTES - sizeof(vector<T>) - sizeof(std::atomic_flag)];
#else
    std::mutex lock;
    // TODO: deal with padding?
#endif
};

#endif